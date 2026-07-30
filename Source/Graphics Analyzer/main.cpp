#define GRAPHICS_ANALYZER 1
#define ENABLE_NGX 1
#define ENABLE_SMAA 0

// Always true in the graphics analyzer
#ifdef DEVELOPMENT
#undef DEVELOPMENT
#define DEVELOPMENT 1
#endif // DEVELOPMENT

#define CHECK_GRAPHICS_API_COMPATIBILITY 1

#include "..\Core\core.hpp"

namespace
{
   // The public build is native-resolution DLAA only. The experimental
   // sub-native scaler/UI path is retained in source for future work, but is
   // deliberately unreachable in production.
   constexpr bool enable_experimental_dlss_upscaling = false;

   // Canonical FXAA 3.11 pass used when graphics.ini contains aa=2.
   const ShaderHashesList shader_hashes_yakuza6_fxaa = { .pixel_shaders = { 0x5066FDED } };

   // Final post-tonemap scene scaler. At sub-native render scales it reads the
   // low-resolution scene from t0 and writes the full-resolution pre-UI image
   // to u0.
   const ShaderHashesList shader_hashes_yakuza6_scaler = { .compute_shaders = { 0xF7B49AD6 } };

   // Final gamma/output transform from the full-resolution UI composition
   // texture into the swap chain.
   const ShaderHashesList shader_hashes_yakuza6_display = { .pixel_shaders = { 0x03468A21 } };

   // A representative three-target material shader. It is only used to locate
   // the main scene depth resource; other material variants share that DSV.
   const ShaderHashesList shader_hashes_yakuza6_gbuffer = { .pixel_shaders = { 0x05A884CF } };

}

struct GraphicsAnalyzerDeviceData final : public GameDeviceData
{
   struct Yakuza6CameraConstants
   {
      float4 values[31];
   };

   struct alignas(16) Yakuza6MotionConstants
   {
      DirectX::XMFLOAT4 reprojection_rows[4];
      uint2 resolution;
      uint32_t reset_history = 1;
      uint32_t padding = 0;
   };

   uint64_t present_index = 0;

   // Yakuza 6 DLAA resources and camera state.
   std::mutex camera_mutex;
   std::unordered_map<ID3D11Buffer*, Yakuza6CameraConstants> camera_buffer_cache;
   std::unordered_map<ID3D11Buffer*, void*> mapped_camera_buffers;
   std::unordered_set<ID3D11Buffer*> main_camera_buffers;
   std::unordered_set<ID3D11Buffer*> jittered_camera_buffers;
   Yakuza6CameraConstants current_camera = {};
   Yakuza6CameraConstants previous_camera = {};
   bool current_camera_valid = false;
   bool previous_camera_valid = false;
   uint64_t camera_frame = UINT64_MAX;
   float2 current_jitter_pixels = {};

   com_ptr<ID3D11Resource> scene_depth;
   com_ptr<ID3D11ShaderResourceView> scene_depth_srv;
   com_ptr<ID3D11Texture2D> dlaa_output;
   com_ptr<ID3D11Texture2D> motion_vectors;
   com_ptr<ID3D11ShaderResourceView> motion_vectors_srv;
   com_ptr<ID3D11UnorderedAccessView> motion_vectors_uav;
   com_ptr<ID3D11Buffer> motion_constants;
   uint2 dlaa_resolution = {};
   uint2 render_resolution = {};
   bool dlaa_ran_this_frame = false;
   bool logged_camera_capture = false;
   bool logged_motion_dispatch = false;
   bool logged_dlaa_success = false;
   bool logged_dlss_upscale_success = false;
   bool logged_dlss_upscale_failure = false;
   bool logged_upscale_depth_snapshot = false;
   std::atomic_bool logged_subnative_fxaa_bypass = false;

   // DLSS must execute on the immediate D3D11 context. Yakuza records its
   // final scaler on a deferred context, so split that command list at the
   // scaler and insert DLSS between the two halves during replay.
   std::mutex upscale_command_list_mutex;
   com_ptr<ID3D11CommandList> upscale_partial_command_list;
   std::atomic<ID3D11CommandList*> upscale_remainder_command_list = nullptr;
   std::atomic<ID3D11DeviceContext*> upscale_deferred_context = nullptr;
   com_ptr<ID3D11ShaderResourceView> pending_upscale_source_srv;
   com_ptr<ID3D11UnorderedAccessView> pending_upscale_output_uav;
   com_ptr<ID3D11ComputeShader> pending_scaler_shader;
   com_ptr<ID3D11Buffer> pending_scaler_cbuffer;
   uint2 pending_upscale_output_resolution = {};

   com_ptr<ID3D11Texture2D> display_dlss_output;
   com_ptr<ID3D11ShaderResourceView> display_dlss_output_srv;
   com_ptr<ID3D11UnorderedAccessView> display_dlss_output_uav;
   com_ptr<ID3D11Resource> native_scaled_scene;
   com_ptr<ID3D11ShaderResourceView> native_scaled_scene_srv;
   com_ptr<ID3D11Texture2D> upscale_depth_snapshot;
   com_ptr<ID3D11ShaderResourceView> upscale_depth_snapshot_srv;
   uint2 display_dlss_resolution = {};
};

// Graphics analyzer project.
// This is not meant to be a separate devkit for mods, but an alternative tool that uses the same features that Luma mods have to analyze graphics in games,
// so if you want to make a new game mod, just make a new project for it, after potentially testing the game out with this (it's not necessary, but this is less invasive than a whole mod).
class GraphicsAnalyzer final : public Game
{
   static GraphicsAnalyzerDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<GraphicsAnalyzerDeviceData*>(device_data.game);
   }

   static bool SrActive(const DeviceData& device_data)
   {
      return device_data.sr_type != SR::Type::None && !device_data.sr_suppressed;
   }

   static DirectX::XMMATRIX BuildViewProjection(
      const GraphicsAnalyzerDeviceData::Yakuza6CameraConstants& camera)
   {
      const auto& p0 = camera.values[0];
      const auto& p1 = camera.values[1];
      const auto& p2 = camera.values[2];
      const auto& p3 = camera.values[3];
      const auto& v0 = camera.values[4];
      const auto& v1 = camera.values[5];
      const auto& v2 = camera.values[6];
      const auto& position = camera.values[30];

      const DirectX::XMMATRIX projection = DirectX::XMMatrixSet(
         p0.x, p0.y, p0.z, p0.w,
         p1.x, p1.y, p1.z, p1.w,
         p2.x, p2.y, p2.z, p2.w,
         p3.x, p3.y, p3.z, p3.w);

      const DirectX::XMMATRIX view = DirectX::XMMatrixSet(
         v0.x, v0.y, v0.z, -(v0.x * position.x + v0.y * position.y + v0.z * position.z),
         v1.x, v1.y, v1.z, -(v1.x * position.x + v1.y * position.y + v1.z * position.z),
         v2.x, v2.y, v2.z, -(v2.x * position.x + v2.y * position.y + v2.z * position.z),
         0.0f, 0.0f, 0.0f, 1.0f);

      // These are column-vector matrices in the game shaders (each output
      // component is a dot product with one stored row).
      return DirectX::XMMatrixMultiply(projection, view);
   }

   static uint32_t GetJitterPhaseCount(
      const DeviceData& device_data,
      const GraphicsAnalyzerDeviceData& game_device_data)
   {
      if (game_device_data.render_resolution.y == 0
         || device_data.output_resolution.y <= 0)
      {
         return SR::GetDefaultJitterPhases();
      }

      const float scale =
         static_cast<float>(device_data.output_resolution.y)
         / static_cast<float>(game_device_data.render_resolution.y);
      return (std::max)(
         1u,
         static_cast<uint32_t>(std::lround(
            static_cast<float>(SR::GetDefaultJitterPhases())
            * scale
            * scale)));
   }

   static uint2 GetJitterResolution(
      const DeviceData& device_data,
      const GraphicsAnalyzerDeviceData& game_device_data)
   {
      if (game_device_data.render_resolution.x != 0
         && game_device_data.render_resolution.y != 0)
      {
         return game_device_data.render_resolution;
      }
      return {
         static_cast<uint32_t>((std::max)(device_data.output_resolution.x, 1.0f)),
         static_cast<uint32_t>((std::max)(device_data.output_resolution.y, 1.0f))};
   }

   static DXGI_FORMAT GetDepthSrvFormat(DXGI_FORMAT format)
   {
      switch (format)
      {
      case DXGI_FORMAT_R32_TYPELESS:
         return DXGI_FORMAT_R32_FLOAT;
      case DXGI_FORMAT_R24G8_TYPELESS:
         return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      case DXGI_FORMAT_R32G8X24_TYPELESS:
         return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      default:
         return DXGI_FORMAT_UNKNOWN;
      }
   }

   static bool IsMainAspectPerspectiveCamera(
      const GraphicsAnalyzerDeviceData::Yakuza6CameraConstants& camera,
      const uint2& resolution)
   {
      if (resolution.x == 0 || resolution.y == 0)
         return false;

      const auto& p0 = camera.values[0];
      const auto& p1 = camera.values[1];
      const auto& p3 = camera.values[3];
      if (std::fabs(p0.x) < 1e-6f
         || std::fabs(p1.y) < 1e-6f
         || std::fabs(p3.z) < 1e-4f
         || std::fabs(p3.w) > 1e-4f)
      {
         return false;
      }

      const float projection_aspect =
         std::fabs(p1.y / p0.x);
      const float render_aspect =
         static_cast<float>(resolution.x)
         / static_cast<float>(resolution.y);
      return std::fabs(projection_aspect - render_aspect)
         <= render_aspect * 0.03f;
   }

public:
   static void OnMapBufferRegion(
      reshade::api::device* device,
      reshade::api::resource resource,
      uint64_t offset,
      uint64_t size,
      reshade::api::map_access access,
      void** data)
   {
      if (!data || !*data || offset != 0)
         return;

      auto* device_data = device->get_private_data<DeviceData>();
      if (!device_data || !device_data->game)
         return;

      auto* buffer = reinterpret_cast<ID3D11Buffer*>(resource.handle);
      D3D11_BUFFER_DESC desc = {};
      buffer->GetDesc(&desc);
      if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0)
      {
         return;
      }

      auto& game_device_data = GetGameDeviceData(*device_data);
      std::lock_guard lock(game_device_data.camera_mutex);
      if (desc.ByteWidth
         == sizeof(GraphicsAnalyzerDeviceData::Yakuza6CameraConstants))
      {
         game_device_data.mapped_camera_buffers[buffer] = *data;
      }
   }

   static void OnUnmapBufferRegion(
      reshade::api::device* device,
      reshade::api::resource resource)
   {
      auto* device_data = device->get_private_data<DeviceData>();
      if (!device_data || !device_data->game)
         return;

      auto& game_device_data = GetGameDeviceData(*device_data);
      auto* buffer = reinterpret_cast<ID3D11Buffer*>(resource.handle);

      std::lock_guard lock(game_device_data.camera_mutex);
      const auto mapped = game_device_data.mapped_camera_buffers.find(buffer);
      if (mapped == game_device_data.mapped_camera_buffers.end() || !mapped->second)
         return;

      GraphicsAnalyzerDeviceData::Yakuza6CameraConstants camera = {};
      std::memcpy(&camera, mapped->second, sizeof(camera));
      game_device_data.camera_buffer_cache[buffer] = camera;

      if (!SrActive(*device_data)
         || !game_device_data.main_camera_buffers.contains(buffer)
         || device_data->output_resolution.x <= 0
         || device_data->output_resolution.y <= 0)
      {
         game_device_data.mapped_camera_buffers.erase(mapped);
         return;
      }

      // Do not promote mapped data to camera history here. The same buffer can
      // temporarily contain a reflection or shadow camera. Only the matrix
      // observed at the known main G-buffer draw is allowed into history.
      const uint32_t temporal_frame = static_cast<uint32_t>(
         game_device_data.present_index
         % GetJitterPhaseCount(*device_data, game_device_data));
      game_device_data.current_jitter_pixels.x = SR::HaltonSequence(temporal_frame, 2);
      game_device_data.current_jitter_pixels.y = SR::HaltonSequence(temporal_frame, 3);

      const uint2 jitter_resolution =
         GetJitterResolution(*device_data, game_device_data);
      if (IsMainAspectPerspectiveCamera(camera, jitter_resolution))
      {
         auto jittered = camera;
         const float jitter_x_ndc =
            game_device_data.current_jitter_pixels.x * 2.0f
            / static_cast<float>(jitter_resolution.x);
         const float jitter_y_ndc =
            game_device_data.current_jitter_pixels.y * -2.0f
            / static_cast<float>(jitter_resolution.y);
         for (uint32_t component = 0; component < 4; ++component)
         {
            reinterpret_cast<float*>(&jittered.values[0])[component] +=
               jitter_x_ndc * reinterpret_cast<const float*>(&camera.values[3])[component];
            reinterpret_cast<float*>(&jittered.values[1])[component] +=
               jitter_y_ndc * reinterpret_cast<const float*>(&camera.values[3])[component];

            // Yakuza binds the full 31-float4 allocation even to shaders that
            // declare only the first 12 entries. Skinned, deforming, and
            // vegetation variants use a combined view-projection matrix in
            // rows 8-11 instead of the split projection/view matrices above.
            reinterpret_cast<float*>(&jittered.values[8])[component] +=
               jitter_x_ndc
               * reinterpret_cast<const float*>(&camera.values[11])[component];
            reinterpret_cast<float*>(&jittered.values[9])[component] +=
               jitter_y_ndc
               * reinterpret_cast<const float*>(&camera.values[11])[component];
         }
         std::memcpy(mapped->second, &jittered, sizeof(jittered));
         game_device_data.jittered_camera_buffers.emplace(buffer);
      }

      game_device_data.mapped_camera_buffers.erase(mapped);
   }

   static bool OnUpdateBufferRegionCommand(
      reshade::api::command_list* cmd_list,
      const void* data,
      reshade::api::resource destination,
      uint64_t destination_offset,
      uint64_t size)
   {
      if (!data || destination_offset != 0)
         return false;

      auto& device_data = *cmd_list->get_device()->get_private_data<DeviceData>();
      if (!device_data.game)
         return false;

      auto& game_device_data = GetGameDeviceData(device_data);
      auto* buffer = reinterpret_cast<ID3D11Buffer*>(destination.handle);
      if (!buffer)
         return false;

      D3D11_BUFFER_DESC desc = {};
      buffer->GetDesc(&desc);
      if ((desc.BindFlags & D3D11_BIND_CONSTANT_BUFFER) == 0)
      {
         return false;
      }

      std::lock_guard lock(game_device_data.camera_mutex);
      if (desc.ByteWidth
            == sizeof(GraphicsAnalyzerDeviceData::Yakuza6CameraConstants)
         && size
            >= sizeof(GraphicsAnalyzerDeviceData::Yakuza6CameraConstants))
      {
         std::memcpy(
            &game_device_data.camera_buffer_cache[buffer],
            data,
            sizeof(GraphicsAnalyzerDeviceData::Yakuza6CameraConstants));
      }
      return false;
   }

   static bool UploadJitteredCameraConstants(
      ID3D11DeviceContext* context,
      ID3D11Buffer* buffer,
      const GraphicsAnalyzerDeviceData::Yakuza6CameraConstants& camera,
      const float2& jitter_pixels,
      const uint2& resolution)
   {
      if (!buffer || resolution.x == 0 || resolution.y == 0)
         return false;

      auto jittered = camera;
      const float jitter_x_ndc = jitter_pixels.x * 2.0f / static_cast<float>(resolution.x);
      const float jitter_y_ndc = jitter_pixels.y * -2.0f / static_cast<float>(resolution.y);
      for (uint32_t component = 0; component < 4; ++component)
      {
         reinterpret_cast<float*>(&jittered.values[0])[component] +=
            jitter_x_ndc * reinterpret_cast<const float*>(&camera.values[3])[component];
         reinterpret_cast<float*>(&jittered.values[1])[component] +=
            jitter_y_ndc * reinterpret_cast<const float*>(&camera.values[3])[component];

         // The full camera allocation also contains the combined
         // view-projection matrix consumed by skinned/deforming shaders.
         reinterpret_cast<float*>(&jittered.values[8])[component] +=
            jitter_x_ndc
            * reinterpret_cast<const float*>(&camera.values[11])[component];
         reinterpret_cast<float*>(&jittered.values[9])[component] +=
            jitter_y_ndc
            * reinterpret_cast<const float*>(&camera.values[11])[component];
      }

      D3D11_BUFFER_DESC desc = {};
      buffer->GetDesc(&desc);
      if (desc.Usage == D3D11_USAGE_DYNAMIC)
      {
         D3D11_MAPPED_SUBRESOURCE mapped = {};
         if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
         std::memcpy(mapped.pData, &jittered, sizeof(jittered));
         context->Unmap(buffer, 0);
      }
      else if (desc.Usage == D3D11_USAGE_DEFAULT)
      {
         context->UpdateSubresource(buffer, 0, nullptr, &jittered, 0, 0);
      }
      else
      {
         return false;
      }
      return true;
   }

   static void CaptureAndJitterMainCamera(
      ID3D11DeviceContext* context,
      DeviceData& device_data,
      GraphicsAnalyzerDeviceData& game_device_data)
   {
      com_ptr<ID3D11Buffer> camera_buffer;
      context->VSGetConstantBuffers(12, 1, &camera_buffer);
      if (!camera_buffer)
         return;

      GraphicsAnalyzerDeviceData::Yakuza6CameraConstants camera = {};
      {
         std::lock_guard lock(game_device_data.camera_mutex);
         const auto found = game_device_data.camera_buffer_cache.find(camera_buffer.get());
         if (found == game_device_data.camera_buffer_cache.end())
            return;
         camera = found->second;
         game_device_data.main_camera_buffers.emplace(
            camera_buffer.get());

         if (game_device_data.camera_frame != game_device_data.present_index)
         {
            if (game_device_data.current_camera_valid)
            {
               game_device_data.previous_camera = game_device_data.current_camera;
               game_device_data.previous_camera_valid = true;
            }
            game_device_data.current_camera = camera;
            game_device_data.current_camera_valid = true;
            game_device_data.camera_frame = game_device_data.present_index;

            const uint32_t temporal_frame = static_cast<uint32_t>(
               game_device_data.present_index
               % GetJitterPhaseCount(device_data, game_device_data));
            game_device_data.current_jitter_pixels.x = SR::HaltonSequence(temporal_frame, 2);
            game_device_data.current_jitter_pixels.y = SR::HaltonSequence(temporal_frame, 3);
         }

         if (!game_device_data.jittered_camera_buffers.emplace(camera_buffer.get()).second)
            return;
      }

      UploadJitteredCameraConstants(
         context,
         camera_buffer.get(),
         camera,
         game_device_data.current_jitter_pixels,
         GetJitterResolution(device_data, game_device_data));

      if (!game_device_data.logged_camera_capture)
      {
         reshade::log::message(
            reshade::log::level::info,
            "[Luma][Yakuza6] Captured the main camera constants and injected projection jitter.");
         game_device_data.logged_camera_capture = true;
      }

      // Keep the cache unjittered even if the native update is observed by a
      // ReShade buffer callback on this driver/runtime combination.
      {
         std::lock_guard lock(game_device_data.camera_mutex);
         game_device_data.camera_buffer_cache[camera_buffer.get()] = camera;
      }
   }

   static void OnExecuteSecondaryCommandList(
      reshade::api::command_list* cmd_list,
      reshade::api::command_list* secondary_cmd_list)
   {
      auto* device_data =
         cmd_list->get_device()->get_private_data<DeviceData>();
      if (!device_data || !device_data->game)
         return;

      auto& game_device_data = GetGameDeviceData(*device_data);
      if (!game_device_data.upscale_partial_command_list
         && game_device_data.upscale_deferred_context.load(
            std::memory_order_acquire) == nullptr)
      {
         return;
      }

      com_ptr<ID3D11DeviceContext> immediate_context;
      com_ptr<ID3D11CommandList> finish_command_list;
      auto* primary_native =
         reinterpret_cast<ID3D11DeviceChild*>(cmd_list->get_native());
      primary_native->QueryInterface(&immediate_context);
      primary_native->QueryInterface(&finish_command_list);

      com_ptr<ID3D11DeviceContext> source_deferred_context;
      com_ptr<ID3D11CommandList> secondary_native_command_list;
      auto* secondary_native =
         reinterpret_cast<ID3D11DeviceChild*>(secondary_cmd_list->get_native());
      secondary_native->QueryInterface(&source_deferred_context);
      secondary_native->QueryInterface(&secondary_native_command_list);

      // FinishCommandList notification: associate the command list generated
      // from our split deferred context with the pending second half.
      if (finish_command_list
         && source_deferred_context
         && source_deferred_context.get()
            == game_device_data.upscale_deferred_context.load(
               std::memory_order_acquire)
         && game_device_data.upscale_partial_command_list)
      {
         game_device_data.upscale_remainder_command_list.store(
            finish_command_list.get(),
            std::memory_order_release);
         return;
      }

      // ExecuteCommandList notification: replay the pre-scaler half, run DLSS
      // on the immediate context, then allow ReShade/the game to replay the
      // post-scaler half after this callback returns.
      if (!immediate_context
         || !secondary_native_command_list
         || secondary_native_command_list.get()
            != game_device_data.upscale_remainder_command_list.load(
               std::memory_order_acquire))
      {
         return;
      }

      std::lock_guard lock(game_device_data.upscale_command_list_mutex);
      game_device_data.upscale_remainder_command_list.store(
         nullptr,
         std::memory_order_release);
      game_device_data.upscale_deferred_context.store(
         nullptr,
         std::memory_order_release);

      if (!game_device_data.upscale_partial_command_list
         || !game_device_data.pending_upscale_source_srv
         || !game_device_data.pending_upscale_output_uav)
      {
         return;
      }

      immediate_context->ExecuteCommandList(
         game_device_data.upscale_partial_command_list.get(),
         FALSE);
      game_device_data.upscale_partial_command_list.reset();

      ID3D11ShaderResourceView* source_srv =
         game_device_data.pending_upscale_source_srv.get();
      ID3D11UnorderedAccessView* output_uav =
         game_device_data.pending_upscale_output_uav.get();
      immediate_context->CSSetShaderResources(0, 1, &source_srv);
      immediate_context->CSSetUnorderedAccessViews(
         0,
         1,
         &output_uav,
         nullptr);

      ShaderHashesList<OneShaderPerPipeline> scaler_hashes;
      scaler_hashes.Clear();
      scaler_hashes.compute_shaders[0] = 0xF7B49AD6;

      auto& cmd_list_data =
         *cmd_list->get_private_data<CommandListData>();
      bool updated_cbuffers = false;
      auto* analyzer = static_cast<GraphicsAnalyzer*>(game);
      const DrawOrDispatchOverrideType result = analyzer->OnDrawOrDispatch(
         reinterpret_cast<ID3D11Device*>(
            cmd_list->get_device()->get_native()),
         immediate_context.get(),
         cmd_list_data,
         *device_data,
         reshade::api::shader_stage::compute,
         scaler_hashes,
         false,
         updated_cbuffers,
         nullptr);

      if (result != DrawOrDispatchOverrideType::Replaced)
      {
         // Keep a safe visual fallback: replay Yakuza's original scaler with
         // the bindings captured at the split point.
         ID3D11Buffer* scaler_cbuffer =
            game_device_data.pending_scaler_cbuffer.get();
         immediate_context->CSSetShader(
            game_device_data.pending_scaler_shader.get(),
            nullptr,
            0);
         immediate_context->CSSetConstantBuffers(
            0,
            1,
            &scaler_cbuffer);
         immediate_context->CSSetShaderResources(0, 1, &source_srv);
         immediate_context->CSSetUnorderedAccessViews(
            0,
            1,
            &output_uav,
            nullptr);
         immediate_context->Dispatch(
            (game_device_data.pending_upscale_output_resolution.x + 15) / 16,
            (game_device_data.pending_upscale_output_resolution.y + 15) / 16,
            1);

         device_data->force_reset_sr = true;
         if (!game_device_data.logged_dlss_upscale_failure)
         {
            reshade::log::message(
               reshade::log::level::warning,
               "[Luma][Yakuza6] DLSS scaler insertion failed; used the native scaler fallback.");
            game_device_data.logged_dlss_upscale_failure = true;
         }
      }

      game_device_data.pending_upscale_source_srv.reset();
      game_device_data.pending_upscale_output_uav.reset();
      game_device_data.pending_scaler_shader.reset();
      game_device_data.pending_scaler_cbuffer.reset();
   }

public:
   void OnInit(bool async) override
   {
      // Needed by the final display composition shader
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12;

      native_shaders_definitions.emplace(
         CompileTimeStringHash("Yakuza 6 Motion Vectors"),
         ShaderDefinition{
            "Luma_Yakuza6_MotionVectors",
            reshade::api::pipeline_subobject_type::compute_shader});
      if constexpr (enable_experimental_dlss_upscaling)
      {
         native_shaders_definitions.emplace(
            CompileTimeStringHash("Yakuza 6 DLSS UI Composite"),
            ShaderDefinition{
               "Luma_Yakuza6_DLSS_UI_Composite",
               reshade::api::pipeline_subobject_type::pixel_shader});
      }
      reshade::register_event<reshade::addon_event::update_buffer_region_command>(
         GraphicsAnalyzer::OnUpdateBufferRegionCommand);
      reshade::register_event<reshade::addon_event::map_buffer_region>(
         GraphicsAnalyzer::OnMapBufferRegion);
      reshade::register_event<reshade::addon_event::unmap_buffer_region>(
         GraphicsAnalyzer::OnUnmapBufferRegion);
      if constexpr (enable_experimental_dlss_upscaling)
      {
         reshade::register_event<
            reshade::addon_event::execute_secondary_command_list>(
         GraphicsAnalyzer::OnExecuteSecondaryCommandList);
      }
   }

   void LoadConfigs() override
   {
      // This public build has one supported mode: NVIDIA DLAA at native
      // resolution. Do not inherit a disabled SR state or an unrelated preset
      // from another Luma add-on that shares the ReShade configuration.
      sr_user_type = SR::UserType::DLSS;
      dlss_render_preset = 11u; // Preset K, NVIDIA's native-DLAA default.
   }

   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      auto* game_device_data = new GraphicsAnalyzerDeviceData;
      device_data.game = game_device_data;
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

      if (!is_custom_pass && original_shader_hashes.Contains(shader_hashes_yakuza6_gbuffer))
      {
         com_ptr<ID3D11DepthStencilView> dsv;
         native_device_context->OMGetRenderTargets(0, nullptr, &dsv);
         if (dsv)
         {
            com_ptr<ID3D11Resource> depth;
            dsv->GetResource(&depth);
            com_ptr<ID3D11Texture2D> depth_texture;
            if (depth && SUCCEEDED(depth->QueryInterface(&depth_texture)))
            {
               D3D11_TEXTURE2D_DESC depth_desc = {};
               depth_texture->GetDesc(&depth_desc);
               game_device_data.render_resolution = {
                  depth_desc.Width,
                  depth_desc.Height};
            }
            if (depth.get() != game_device_data.scene_depth.get())
            {
               game_device_data.scene_depth = depth;
               game_device_data.scene_depth_srv.reset();
            }
         }

         if (SrActive(device_data))
            CaptureAndJitterMainCamera(native_device_context, device_data, game_device_data);
      }

      // Capture the low-resolution final scene and Yakuza's native spatially
      // scaled reference. They remain valid until the immediate-context final
      // display draw, where DLSS can execute safely.
      if (!is_custom_pass
         && enable_experimental_dlss_upscaling
         && original_shader_hashes.Contains(shader_hashes_yakuza6_scaler)
         && SrActive(device_data))
      {
         com_ptr<ID3D11ShaderResourceView> source_srv;
         com_ptr<ID3D11UnorderedAccessView> output_uav;
         native_device_context->CSGetShaderResources(0, 1, &source_srv);
         native_device_context->CSGetUnorderedAccessViews(0, 1, &output_uav);

         com_ptr<ID3D11Resource> source_resource;
         com_ptr<ID3D11Resource> output_resource;
         if (source_srv)
            source_srv->GetResource(&source_resource);
         if (output_uav)
            output_uav->GetResource(&output_resource);

         com_ptr<ID3D11Texture2D> source_texture;
         com_ptr<ID3D11Texture2D> output_texture;
         if (source_resource)
            source_resource->QueryInterface(&source_texture);
         if (output_resource)
            output_resource->QueryInterface(&output_texture);

         if (source_texture && output_texture)
         {
            D3D11_TEXTURE2D_DESC source_desc = {};
            D3D11_TEXTURE2D_DESC output_desc = {};
            source_texture->GetDesc(&source_desc);
            output_texture->GetDesc(&output_desc);
            if (source_desc.Width < output_desc.Width
               && source_desc.Height < output_desc.Height
               && output_desc.Width == device_data.output_resolution.x
               && output_desc.Height == device_data.output_resolution.y)
            {
               std::lock_guard lock(
                  game_device_data.upscale_command_list_mutex);
               game_device_data.pending_upscale_source_srv = source_srv;
               game_device_data.pending_upscale_output_uav = output_uav;
               game_device_data.pending_upscale_output_resolution = {
                  output_desc.Width,
                  output_desc.Height};

               if (output_resource.get()
                  != game_device_data.native_scaled_scene.get())
               {
                  game_device_data.native_scaled_scene = output_resource;
                  game_device_data.native_scaled_scene_srv.reset();
                  native_device->CreateShaderResourceView(
                     output_resource.get(),
                     nullptr,
                     &game_device_data.native_scaled_scene_srv);
               }

               // The final display draw can occur well after this scaler was
               // recorded. Keep the depth contents from this exact point in
               // the frame rather than retaining only the game's reusable
               // depth-resource pointer.
               com_ptr<ID3D11Texture2D> scene_depth_texture;
               if (game_device_data.scene_depth
                  && SUCCEEDED(game_device_data.scene_depth->QueryInterface(
                     &scene_depth_texture)))
               {
                  D3D11_TEXTURE2D_DESC depth_desc = {};
                  scene_depth_texture->GetDesc(&depth_desc);

                  D3D11_TEXTURE2D_DESC snapshot_desc = {};
                  if (game_device_data.upscale_depth_snapshot)
                     game_device_data.upscale_depth_snapshot->GetDesc(
                        &snapshot_desc);

                  if (!game_device_data.upscale_depth_snapshot
                     || snapshot_desc.Width != depth_desc.Width
                     || snapshot_desc.Height != depth_desc.Height
                     || snapshot_desc.Format != depth_desc.Format
                     || snapshot_desc.SampleDesc.Count
                        != depth_desc.SampleDesc.Count)
                  {
                     game_device_data.upscale_depth_snapshot.reset();
                     game_device_data.upscale_depth_snapshot_srv.reset();

                     D3D11_TEXTURE2D_DESC copy_desc = depth_desc;
                     copy_desc.Usage = D3D11_USAGE_DEFAULT;
                     copy_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                     copy_desc.CPUAccessFlags = 0;
                     copy_desc.MiscFlags = 0;
                     if (SUCCEEDED(native_device->CreateTexture2D(
                        &copy_desc,
                        nullptr,
                        &game_device_data.upscale_depth_snapshot)))
                     {
                        const DXGI_FORMAT srv_format =
                           GetDepthSrvFormat(copy_desc.Format);
                        if (srv_format != DXGI_FORMAT_UNKNOWN)
                        {
                           D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                           srv_desc.Format = srv_format;
                           srv_desc.ViewDimension =
                              D3D11_SRV_DIMENSION_TEXTURE2D;
                           srv_desc.Texture2D.MostDetailedMip = 0;
                           srv_desc.Texture2D.MipLevels = 1;
                           native_device->CreateShaderResourceView(
                              game_device_data.upscale_depth_snapshot.get(),
                              &srv_desc,
                              &game_device_data.upscale_depth_snapshot_srv);
                        }
                     }
                  }

                  if (game_device_data.upscale_depth_snapshot
                     && game_device_data.upscale_depth_snapshot_srv)
                  {
                     native_device_context->CopyResource(
                        game_device_data.upscale_depth_snapshot.get(),
                        game_device_data.scene_depth.get());
                     if (!game_device_data.logged_upscale_depth_snapshot)
                     {
                        reshade::log::message(
                           reshade::log::level::info,
                           "[Luma][Yakuza6] Snapshotted scene depth at the final scaler.");
                        game_device_data.logged_upscale_depth_snapshot = true;
                     }
                  }
               }
            }
         }
      }

      if (false
         && !is_custom_pass
         && original_shader_hashes.Contains(shader_hashes_yakuza6_scaler)
         && SrActive(device_data)
         && game_device_data.scene_depth
         && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED)
      {
         com_ptr<ID3D11ShaderResourceView> source_srv;
         com_ptr<ID3D11UnorderedAccessView> output_uav;
         native_device_context->CSGetShaderResources(0, 1, &source_srv);
         native_device_context->CSGetUnorderedAccessViews(0, 1, &output_uav);

         com_ptr<ID3D11Resource> source_resource;
         com_ptr<ID3D11Resource> output_resource;
         if (source_srv)
            source_srv->GetResource(&source_resource);
         if (output_uav)
            output_uav->GetResource(&output_resource);

         com_ptr<ID3D11Texture2D> source_texture;
         com_ptr<ID3D11Texture2D> output_texture;
         if (source_resource)
            source_resource->QueryInterface(&source_texture);
         if (output_resource)
            output_resource->QueryInterface(&output_texture);

         if (source_texture && output_texture)
         {
            D3D11_TEXTURE2D_DESC source_desc = {};
            D3D11_TEXTURE2D_DESC output_desc = {};
            source_texture->GetDesc(&source_desc);
            output_texture->GetDesc(&output_desc);

            const bool valid_deferred_upscale_pass =
               source_desc.Width < output_desc.Width
               && source_desc.Height < output_desc.Height
               && output_desc.Width == device_data.output_resolution.x
               && output_desc.Height == device_data.output_resolution.y;

            if (valid_deferred_upscale_pass)
            {
               std::lock_guard lock(
                  game_device_data.upscale_command_list_mutex);
               if (!game_device_data.upscale_partial_command_list)
               {
                  game_device_data.pending_upscale_source_srv = source_srv;
                  game_device_data.pending_upscale_output_uav = output_uav;
                  game_device_data.pending_upscale_output_resolution = {
                     output_desc.Width,
                     output_desc.Height};
                  native_device_context->CSGetShader(
                     &game_device_data.pending_scaler_shader,
                     nullptr,
                     nullptr);
                  native_device_context->CSGetConstantBuffers(
                     0,
                     1,
                     &game_device_data.pending_scaler_cbuffer);
                  game_device_data.upscale_deferred_context.store(
                     native_device_context,
                     std::memory_order_release);

                  const HRESULT split_result =
                     native_device_context->FinishCommandList(
                        TRUE,
                        &game_device_data.upscale_partial_command_list);
                  if (SUCCEEDED(split_result)
                     && game_device_data.upscale_partial_command_list)
                  {
                     return DrawOrDispatchOverrideType::Replaced;
                  }

                  game_device_data.upscale_deferred_context.store(
                     nullptr,
                     std::memory_order_release);
                  game_device_data.pending_upscale_source_srv.reset();
                  game_device_data.pending_upscale_output_uav.reset();
                  game_device_data.pending_scaler_shader.reset();
                  game_device_data.pending_scaler_cbuffer.reset();
               }
            }
         }
      }

      // At sub-native render scales, replace the game's final spatial scaler
      // with DLSS. This pass runs after tone mapping but before the native
      // resolution UI, so the HUD remains perfectly sharp.
      if (!is_custom_pass
         && enable_experimental_dlss_upscaling
         && original_shader_hashes.Contains(shader_hashes_yakuza6_scaler)
         && SrActive(device_data)
         && game_device_data.scene_depth
         && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
      {
         com_ptr<ID3D11ShaderResourceView> source_srv;
         com_ptr<ID3D11UnorderedAccessView> output_uav;
         native_device_context->CSGetShaderResources(0, 1, &source_srv);
         native_device_context->CSGetUnorderedAccessViews(0, 1, &output_uav);

         com_ptr<ID3D11Resource> source_resource;
         com_ptr<ID3D11Resource> output_resource;
         if (source_srv)
            source_srv->GetResource(&source_resource);
         if (output_uav)
            output_uav->GetResource(&output_resource);

         com_ptr<ID3D11Texture2D> source_texture;
         com_ptr<ID3D11Texture2D> output_texture;
         com_ptr<ID3D11Texture2D> depth_texture;
         if (source_resource)
            source_resource->QueryInterface(&source_texture);
         if (output_resource)
            output_resource->QueryInterface(&output_texture);
          ID3D11Resource* depth_resource =
             game_device_data.upscale_depth_snapshot
                ? static_cast<ID3D11Resource*>(
                     game_device_data.upscale_depth_snapshot.get())
                : game_device_data.scene_depth.get();
          if (depth_resource)
             depth_resource->QueryInterface(&depth_texture);

         if (source_texture && output_texture && depth_texture)
         {
            D3D11_TEXTURE2D_DESC source_desc = {};
            D3D11_TEXTURE2D_DESC output_desc = {};
            D3D11_TEXTURE2D_DESC depth_desc = {};
            source_texture->GetDesc(&source_desc);
            output_texture->GetDesc(&output_desc);
            depth_texture->GetDesc(&depth_desc);

            const bool valid_upscale_pass =
               source_desc.Width < output_desc.Width
               && source_desc.Height < output_desc.Height
               && source_desc.Width == depth_desc.Width
               && source_desc.Height == depth_desc.Height
               && source_desc.SampleDesc.Count == 1
               && output_desc.SampleDesc.Count == 1
               && output_desc.Width == device_data.output_resolution.x
               && output_desc.Height == device_data.output_resolution.y
               && (output_desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;

            if (valid_upscale_pass)
            {
               const uint2 render_resolution = {
                  source_desc.Width,
                  source_desc.Height};
               const uint2 output_resolution = {
                  output_desc.Width,
                  output_desc.Height};

               if (game_device_data.dlaa_resolution != render_resolution)
               {
                  game_device_data.dlaa_output.reset();
                  game_device_data.motion_vectors.reset();
                  game_device_data.motion_vectors_srv.reset();
                  game_device_data.motion_vectors_uav.reset();
                  game_device_data.motion_constants.reset();
                  game_device_data.dlaa_resolution = render_resolution;

                  D3D11_TEXTURE2D_DESC mv_desc = {};
                  mv_desc.Width = render_resolution.x;
                  mv_desc.Height = render_resolution.y;
                  mv_desc.MipLevels = 1;
                  mv_desc.ArraySize = 1;
                  mv_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
                  mv_desc.SampleDesc.Count = 1;
                  mv_desc.Usage = D3D11_USAGE_DEFAULT;
                  mv_desc.BindFlags =
                     D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                  if (SUCCEEDED(native_device->CreateTexture2D(
                     &mv_desc,
                     nullptr,
                     &game_device_data.motion_vectors)))
                  {
                     native_device->CreateShaderResourceView(
                        game_device_data.motion_vectors.get(),
                        nullptr,
                        &game_device_data.motion_vectors_srv);
                     native_device->CreateUnorderedAccessView(
                        game_device_data.motion_vectors.get(),
                        nullptr,
                        &game_device_data.motion_vectors_uav);
                  }

                  D3D11_BUFFER_DESC constants_desc = {};
                  constants_desc.ByteWidth =
                     sizeof(GraphicsAnalyzerDeviceData::Yakuza6MotionConstants);
                  constants_desc.Usage = D3D11_USAGE_DEFAULT;
                  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                  native_device->CreateBuffer(
                     &constants_desc,
                     nullptr,
                     &game_device_data.motion_constants);
               }

               ID3D11ShaderResourceView* depth_srv =
                  game_device_data.upscale_depth_snapshot_srv
                     ? game_device_data.upscale_depth_snapshot_srv.get()
                     : game_device_data.scene_depth_srv.get();
               if (!depth_srv)
               {
                  DXGI_FORMAT depth_srv_format = DXGI_FORMAT_UNKNOWN;
                  switch (depth_desc.Format)
                  {
                  case DXGI_FORMAT_R32_TYPELESS:
                     depth_srv_format = DXGI_FORMAT_R32_FLOAT;
                     break;
                  case DXGI_FORMAT_R24G8_TYPELESS:
                     depth_srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                     break;
                  case DXGI_FORMAT_R32G8X24_TYPELESS:
                     depth_srv_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                     break;
                  default:
                     break;
                  }

                  if (depth_srv_format != DXGI_FORMAT_UNKNOWN
                     && (depth_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
                  {
                     D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                     srv_desc.Format = depth_srv_format;
                     srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                     srv_desc.Texture2D.MostDetailedMip = 0;
                     srv_desc.Texture2D.MipLevels = 1;
                     native_device->CreateShaderResourceView(
                        depth_texture.get(),
                        &srv_desc,
                        &game_device_data.scene_depth_srv);
                     depth_srv = game_device_data.scene_depth_srv.get();
                  }
               }

               auto* sr_instance_data = device_data.GetSRInstanceData();
               if (sr_instance_data
                  && game_device_data.motion_vectors
                  && game_device_data.motion_vectors_srv
                  && game_device_data.motion_vectors_uav
                  && game_device_data.motion_constants
                   && depth_srv)
               {
                  DrawStateStack<DrawStateStackType::FullGraphics> graphics_state;
                  DrawStateStack<DrawStateStackType::Compute> compute_state;
                  graphics_state.Cache(
                     native_device_context,
                     device_data.uav_max_count);
                  compute_state.Cache(
                     native_device_context,
                     device_data.uav_max_count);

                  const bool reset_sr = device_data.force_reset_sr;
                  device_data.force_reset_sr = false;
                   const bool reset_history =
                      !game_device_data.current_camera_valid
                      || !game_device_data.previous_camera_valid
                      || reset_sr;

                  GraphicsAnalyzerDeviceData::Yakuza6MotionConstants motion_constants = {};
                  motion_constants.resolution = render_resolution;
                  motion_constants.reset_history = reset_history ? 1u : 0u;

                  if (!reset_history)
                  {
                     const DirectX::XMMATRIX current_view_projection =
                        BuildViewProjection(game_device_data.current_camera);
                     const DirectX::XMMATRIX previous_view_projection =
                        BuildViewProjection(game_device_data.previous_camera);
                     DirectX::XMVECTOR determinant;
                     const DirectX::XMMATRIX inverse_current =
                        DirectX::XMMatrixInverse(
                           &determinant,
                           current_view_projection);
                     const DirectX::XMMATRIX reprojection =
                        DirectX::XMMatrixMultiply(
                           previous_view_projection,
                           inverse_current);
                     for (uint32_t row = 0; row < 4; ++row)
                     {
                        DirectX::XMStoreFloat4(
                           &motion_constants.reprojection_rows[row],
                           reprojection.r[row]);
                     }
                  }

                  native_device_context->UpdateSubresource(
                     game_device_data.motion_constants.get(),
                     0,
                     nullptr,
                     &motion_constants,
                     0,
                     0);

                   ID3D11ShaderResourceView* motion_srvs[] = {
                      depth_srv};
                  ID3D11UnorderedAccessView* motion_uavs[] = {
                     game_device_data.motion_vectors_uav.get()};
                  ID3D11Buffer* motion_cbuffers[] = {
                     game_device_data.motion_constants.get()};
                  native_device_context->CSSetShader(
                     device_data.native_compute_shaders.at(
                        CompileTimeStringHash("Yakuza 6 Motion Vectors")).get(),
                     nullptr,
                     0);
                  native_device_context->CSSetShaderResources(
                     0,
                     1,
                     motion_srvs);
                  native_device_context->CSSetUnorderedAccessViews(
                     0,
                     1,
                     motion_uavs,
                     nullptr);
                  native_device_context->CSSetConstantBuffers(
                     0,
                     1,
                     motion_cbuffers);
                  native_device_context->Dispatch(
                     (render_resolution.x + 7) / 8,
                     (render_resolution.y + 7) / 8,
                     1);

                  ID3D11ShaderResourceView* null_srvs[] = {nullptr};
                  ID3D11UnorderedAccessView* null_uavs[] = {nullptr};
                  native_device_context->CSSetShaderResources(
                     0,
                     1,
                     null_srvs);
                  native_device_context->CSSetUnorderedAccessViews(
                     0,
                     1,
                     null_uavs,
                     nullptr);

                  SR::SettingsData settings_data;
                  settings_data.output_width = output_resolution.x;
                  settings_data.output_height = output_resolution.y;
                  settings_data.render_width = render_resolution.x;
                  settings_data.render_height = render_resolution.y;
                  settings_data.dynamic_resolution = false;
                  settings_data.hdr = false;
                  settings_data.inverted_depth = true;
                  settings_data.mvs_jittered = false;
                  settings_data.mvs_x_scale =
                     -static_cast<float>(render_resolution.x);
                  settings_data.mvs_y_scale =
                     -static_cast<float>(render_resolution.y);
                  settings_data.render_preset = dlss_render_preset;
                  settings_data.auto_exposure = false;

                  bool upscale_succeeded = false;
                  if (sr_implementations[device_data.sr_type]->UpdateSettings(
                     sr_instance_data,
                     native_device_context,
                     settings_data))
                  {
                     SR::SuperResolutionImpl::DrawData draw_data;
                     draw_data.source_color = source_resource.get();
                     draw_data.output_color = output_resource.get();
                     draw_data.motion_vectors =
                        game_device_data.motion_vectors.get();
                      draw_data.depth_buffer = depth_resource;
                     draw_data.jitter_x =
                        game_device_data.current_jitter_pixels.x;
                     draw_data.jitter_y =
                        game_device_data.current_jitter_pixels.y;
                     draw_data.render_width = render_resolution.x;
                     draw_data.render_height = render_resolution.y;
                     draw_data.reset = reset_history;
                     draw_data.pre_exposure = 1.0f;
                     draw_data.frame_index = game_device_data.present_index;

                     upscale_succeeded =
                        sr_implementations[device_data.sr_type]->Draw(
                           sr_instance_data,
                           native_device_context,
                           draw_data);
                  }

                  compute_state.Restore(native_device_context);
                  graphics_state.Restore(native_device_context);
                  if (upscale_succeeded)
                  {
                     device_data.has_drawn_sr = true;
                     game_device_data.dlaa_ran_this_frame = true;
                     if (!game_device_data.logged_dlss_upscale_success)
                     {
                        reshade::log::message(
                           reshade::log::level::info,
                           "[Luma][Yakuza6] DLSS upscaling replaced the native scene scaler.");
                        game_device_data.logged_dlss_upscale_success = true;
                     }
                     return DrawOrDispatchOverrideType::Replaced;
                  }

                  device_data.force_reset_sr = true;
               }
            }
         }
      }

      if (!is_custom_pass
         && enable_experimental_dlss_upscaling
         && original_shader_hashes.Contains(shader_hashes_yakuza6_display)
         && SrActive(device_data)
         && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
      {
         com_ptr<ID3D11ShaderResourceView> low_resolution_scene_srv;
         com_ptr<ID3D11ShaderResourceView> native_scaled_scene_srv;
         uint2 output_resolution = {};
         {
            std::lock_guard lock(
               game_device_data.upscale_command_list_mutex);
            low_resolution_scene_srv =
               game_device_data.pending_upscale_source_srv;
            native_scaled_scene_srv =
               game_device_data.native_scaled_scene_srv;
            output_resolution =
               game_device_data.pending_upscale_output_resolution;
         }

         com_ptr<ID3D11ShaderResourceView> ui_composite_srv;
         native_device_context->PSGetShaderResources(
            0,
            1,
            &ui_composite_srv);

         if (low_resolution_scene_srv
            && native_scaled_scene_srv
            && ui_composite_srv
            && output_resolution.x != 0
            && output_resolution.y != 0)
         {
            if (!game_device_data.display_dlss_output
               || game_device_data.display_dlss_resolution
                  != output_resolution)
            {
               game_device_data.display_dlss_output.reset();
               game_device_data.display_dlss_output_srv.reset();
               game_device_data.display_dlss_output_uav.reset();
               game_device_data.display_dlss_resolution =
                  output_resolution;

               D3D11_TEXTURE2D_DESC output_desc = {};
               output_desc.Width = output_resolution.x;
               output_desc.Height = output_resolution.y;
               output_desc.MipLevels = 1;
               output_desc.ArraySize = 1;
               output_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
               output_desc.SampleDesc.Count = 1;
               output_desc.Usage = D3D11_USAGE_DEFAULT;
               output_desc.BindFlags =
                  D3D11_BIND_SHADER_RESOURCE
                  | D3D11_BIND_UNORDERED_ACCESS;
               if (SUCCEEDED(native_device->CreateTexture2D(
                  &output_desc,
                  nullptr,
                  &game_device_data.display_dlss_output)))
               {
                  native_device->CreateShaderResourceView(
                     game_device_data.display_dlss_output.get(),
                     nullptr,
                     &game_device_data.display_dlss_output_srv);
                  native_device->CreateUnorderedAccessView(
                     game_device_data.display_dlss_output.get(),
                     nullptr,
                     &game_device_data.display_dlss_output_uav);
               }

            }

            if (game_device_data.display_dlss_output
               && game_device_data.display_dlss_output_srv
               && game_device_data.display_dlss_output_uav)
            {
               DrawStateStack<DrawStateStackType::FullGraphics>
                  display_graphics_state;
               DrawStateStack<DrawStateStackType::Compute>
                  display_compute_state;
               display_graphics_state.Cache(
                  native_device_context,
                  device_data.uav_max_count);
               display_compute_state.Cache(
                  native_device_context,
                  device_data.uav_max_count);

               ID3D11ShaderResourceView* scaler_source =
                  low_resolution_scene_srv.get();
               ID3D11UnorderedAccessView* scaler_output =
                  game_device_data.display_dlss_output_uav.get();
               native_device_context->CSSetShaderResources(
                  0,
                  1,
                  &scaler_source);
               native_device_context->CSSetUnorderedAccessViews(
                  0,
                  1,
                  &scaler_output,
                  nullptr);

               ShaderHashesList<OneShaderPerPipeline> scaler_hashes;
               scaler_hashes.Clear();
               scaler_hashes.compute_shaders[0] = 0xF7B49AD6;
               bool nested_updated_cbuffers = false;
               const DrawOrDispatchOverrideType upscale_result =
                  OnDrawOrDispatch(
                     native_device,
                     native_device_context,
                     cmd_list_data,
                     device_data,
                     reshade::api::shader_stage::compute,
                     scaler_hashes,
                     false,
                     nested_updated_cbuffers,
                     nullptr);

               display_compute_state.Restore(native_device_context);
               display_graphics_state.Restore(native_device_context);

               if (upscale_result == DrawOrDispatchOverrideType::Replaced)
               {
                  ID3D11ShaderResourceView* resolved_scene =
                     game_device_data.display_dlss_output_srv.get();

                  const auto composite_shader =
                     device_data.native_pixel_shaders.find(
                        CompileTimeStringHash(
                           "Yakuza 6 DLSS UI Composite"));
                  if (composite_shader
                     != device_data.native_pixel_shaders.end()
                     && composite_shader->second)
                  {
                     ID3D11ShaderResourceView* composite_srvs[] = {
                        resolved_scene,
                        ui_composite_srv.get(),
                        native_scaled_scene_srv.get()};
                     native_device_context->PSSetShader(
                        composite_shader->second.get(),
                        nullptr,
                        0);
                     native_device_context->PSSetShaderResources(
                        0,
                        static_cast<UINT>(std::size(composite_srvs)),
                        composite_srvs);
                  }
               }
            }
         }
      }

      // Replace Yakuza 6's spatial FXAA resolve with native-resolution DLSS
      // (DLAA). In upscaling modes, bypass FXAA here and let DLSS handle
      // anti-aliasing at the final scene-scaler pass.
      if (!is_custom_pass
         && original_shader_hashes.Contains(shader_hashes_yakuza6_fxaa)
         && device_data.sr_type != SR::Type::None
         && !device_data.sr_suppressed
         && game_device_data.scene_depth)
      {
         com_ptr<ID3D11ShaderResourceView> source_srv;
         native_device_context->PSGetShaderResources(0, 1, &source_srv);

         com_ptr<ID3D11RenderTargetView> output_rtv;
         native_device_context->OMGetRenderTargets(1, &output_rtv, nullptr);

         com_ptr<ID3D11Resource> source_resource;
         com_ptr<ID3D11Resource> output_resource;
         if (source_srv)
            source_srv->GetResource(&source_resource);
         if (output_rtv)
            output_rtv->GetResource(&output_resource);

         com_ptr<ID3D11Texture2D> source_texture;
         com_ptr<ID3D11Texture2D> output_texture;
         if (source_resource)
            source_resource->QueryInterface(&source_texture);
         if (output_resource)
            output_resource->QueryInterface(&output_texture);

         if (source_texture && output_texture)
         {
            D3D11_TEXTURE2D_DESC source_desc = {};
            D3D11_TEXTURE2D_DESC output_desc = {};
            source_texture->GetDesc(&source_desc);
            output_texture->GetDesc(&output_desc);

            const bool valid_sub_native_fxaa_pass =
               enable_experimental_dlss_upscaling
               && source_desc.Width == output_desc.Width
               && source_desc.Height == output_desc.Height
               && source_desc.SampleDesc.Count == 1
               && output_desc.SampleDesc.Count == 1
               && source_desc.Width < device_data.output_resolution.x
               && source_desc.Height < device_data.output_resolution.y;

            if (valid_sub_native_fxaa_pass)
            {
               DrawStateStack<DrawStateStackType::FullGraphics> graphics_state;
               graphics_state.Cache(
                  native_device_context,
                  device_data.uav_max_count);
               native_device_context->OMSetRenderTargets(0, nullptr, nullptr);
               ID3D11ShaderResourceView* null_srv = nullptr;
               native_device_context->PSSetShaderResources(0, 1, &null_srv);
               native_device_context->CopyResource(
                  output_resource.get(),
                  source_resource.get());
               graphics_state.Restore(native_device_context);
               if (!game_device_data.logged_subnative_fxaa_bypass.exchange(true))
               {
                  reshade::log::message(
                     reshade::log::level::info,
                     native_device_context->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED
                        ? "[Luma][Yakuza6] Bypassed low-resolution FXAA on the deferred context."
                        : "[Luma][Yakuza6] Bypassed low-resolution FXAA on the immediate context.");
               }
               return DrawOrDispatchOverrideType::Replaced;
            }

            const bool valid_native_res_pass =
               native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE
               &&
               source_desc.Width == output_desc.Width
               && source_desc.Height == output_desc.Height
               && source_desc.SampleDesc.Count == 1
               && output_desc.SampleDesc.Count == 1
               && source_desc.Width == device_data.output_resolution.x
               && source_desc.Height == device_data.output_resolution.y;

            if (valid_native_res_pass)
            {
               const uint2 resolution = { source_desc.Width, source_desc.Height };
               if (!game_device_data.dlaa_output || game_device_data.dlaa_resolution != resolution)
               {
                  game_device_data.dlaa_output.reset();
                  game_device_data.motion_vectors.reset();
                  game_device_data.motion_vectors_srv.reset();
                  game_device_data.motion_vectors_uav.reset();
                  game_device_data.motion_constants.reset();
                  game_device_data.dlaa_resolution = resolution;

                  D3D11_TEXTURE2D_DESC dlaa_desc = output_desc;
                  dlaa_desc.MipLevels = 1;
                  dlaa_desc.ArraySize = 1;
                  dlaa_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                  dlaa_desc.Usage = D3D11_USAGE_DEFAULT;
                  dlaa_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                  dlaa_desc.CPUAccessFlags = 0;
                  dlaa_desc.MiscFlags = 0;
                  if (FAILED(native_device->CreateTexture2D(&dlaa_desc, nullptr, &game_device_data.dlaa_output)))
                     game_device_data.dlaa_output.reset();

                  D3D11_TEXTURE2D_DESC mv_desc = {};
                  mv_desc.Width = resolution.x;
                  mv_desc.Height = resolution.y;
                  mv_desc.MipLevels = 1;
                  mv_desc.ArraySize = 1;
                  mv_desc.Format = DXGI_FORMAT_R32G32_FLOAT;
                  mv_desc.SampleDesc.Count = 1;
                  mv_desc.Usage = D3D11_USAGE_DEFAULT;
                  mv_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
                  if (SUCCEEDED(native_device->CreateTexture2D(&mv_desc, nullptr, &game_device_data.motion_vectors)))
                  {
                     native_device->CreateShaderResourceView(
                        game_device_data.motion_vectors.get(),
                        nullptr,
                        &game_device_data.motion_vectors_srv);
                     native_device->CreateUnorderedAccessView(
                        game_device_data.motion_vectors.get(),
                        nullptr,
                        &game_device_data.motion_vectors_uav);
                  }

                  D3D11_BUFFER_DESC constants_desc = {};
                  constants_desc.ByteWidth = sizeof(GraphicsAnalyzerDeviceData::Yakuza6MotionConstants);
                  constants_desc.Usage = D3D11_USAGE_DEFAULT;
                  constants_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                  native_device->CreateBuffer(
                     &constants_desc,
                     nullptr,
                     &game_device_data.motion_constants);
               }

               if (!game_device_data.scene_depth_srv && game_device_data.scene_depth)
               {
                  com_ptr<ID3D11Texture2D> depth_texture;
                  if (SUCCEEDED(game_device_data.scene_depth->QueryInterface(&depth_texture)))
                  {
                     D3D11_TEXTURE2D_DESC depth_desc = {};
                     depth_texture->GetDesc(&depth_desc);

                     DXGI_FORMAT depth_srv_format = DXGI_FORMAT_UNKNOWN;
                     switch (depth_desc.Format)
                     {
                     case DXGI_FORMAT_R32_TYPELESS:
                        depth_srv_format = DXGI_FORMAT_R32_FLOAT;
                        break;
                     case DXGI_FORMAT_R24G8_TYPELESS:
                        depth_srv_format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                        break;
                     case DXGI_FORMAT_R32G8X24_TYPELESS:
                        depth_srv_format = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
                        break;
                     default:
                        break;
                     }

                     if (depth_srv_format != DXGI_FORMAT_UNKNOWN
                        && (depth_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0)
                     {
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
                        srv_desc.Format = depth_srv_format;
                        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                        srv_desc.Texture2D.MostDetailedMip = 0;
                        srv_desc.Texture2D.MipLevels = 1;
                        native_device->CreateShaderResourceView(
                           depth_texture.get(),
                           &srv_desc,
                           &game_device_data.scene_depth_srv);
                     }
                  }
               }

               auto* sr_instance_data = device_data.GetSRInstanceData();
               if (sr_instance_data
                  && game_device_data.dlaa_output
                  && game_device_data.motion_vectors
                  && game_device_data.motion_vectors_srv
                  && game_device_data.motion_vectors_uav
                  && game_device_data.motion_constants
                  && game_device_data.scene_depth_srv)
               {
                  DrawStateStack<DrawStateStackType::FullGraphics> graphics_state;
                  DrawStateStack<DrawStateStackType::Compute> compute_state;
                  graphics_state.Cache(native_device_context, device_data.uav_max_count);
                  compute_state.Cache(native_device_context, device_data.uav_max_count);

                  const bool reset_sr = device_data.force_reset_sr;
                  // Consume the reset exactly once. Leaving this latched makes
                  // DLSS discard history every frame and look almost spatial.
                  device_data.force_reset_sr = false;

                  const bool reset_history =
                     !game_device_data.current_camera_valid
                     || !game_device_data.previous_camera_valid
                     || reset_sr;
                  GraphicsAnalyzerDeviceData::Yakuza6MotionConstants motion_constants = {};
                  motion_constants.resolution = resolution;
                  motion_constants.reset_history = reset_history ? 1u : 0u;

                  if (motion_constants.reset_history == 0)
                  {
                     const DirectX::XMMATRIX current_view_projection =
                        BuildViewProjection(game_device_data.current_camera);
                     const DirectX::XMMATRIX previous_view_projection =
                        BuildViewProjection(game_device_data.previous_camera);
                     DirectX::XMVECTOR determinant;
                     const DirectX::XMMATRIX inverse_current =
                        DirectX::XMMatrixInverse(&determinant, current_view_projection);
                     const DirectX::XMMATRIX reprojection =
                        DirectX::XMMatrixMultiply(previous_view_projection, inverse_current);
                     for (uint32_t row = 0; row < 4; ++row)
                     {
                        DirectX::XMStoreFloat4(
                           &motion_constants.reprojection_rows[row],
                           reprojection.r[row]);
                     }
                  }

                  native_device_context->UpdateSubresource(
                     game_device_data.motion_constants.get(),
                     0,
                     nullptr,
                     &motion_constants,
                     0,
                     0);

                  ID3D11ShaderResourceView* motion_srvs[] = {
                     game_device_data.scene_depth_srv.get()};
                  ID3D11UnorderedAccessView* motion_uavs[] = {
                     game_device_data.motion_vectors_uav.get()};
                  ID3D11Buffer* motion_cbuffers[] = {
                     game_device_data.motion_constants.get()};
                  native_device_context->CSSetShader(
                     device_data.native_compute_shaders.at(
                        CompileTimeStringHash("Yakuza 6 Motion Vectors")).get(),
                     nullptr,
                     0);
                  native_device_context->CSSetShaderResources(0, 1, motion_srvs);
                  native_device_context->CSSetUnorderedAccessViews(0, 1, motion_uavs, nullptr);
                  native_device_context->CSSetConstantBuffers(0, 1, motion_cbuffers);
                  native_device_context->Dispatch(
                     (resolution.x + 7) / 8,
                     (resolution.y + 7) / 8,
                     1);
                  if (!game_device_data.logged_motion_dispatch)
                  {
                     reshade::log::message(
                        reshade::log::level::info,
                        "[Luma][Yakuza6] Generated depth-derived camera motion vectors.");
                     game_device_data.logged_motion_dispatch = true;
                  }

                  ID3D11ShaderResourceView* null_srvs[] = {nullptr};
                  ID3D11UnorderedAccessView* null_uavs[] = {nullptr};
                  native_device_context->CSSetShaderResources(0, 1, null_srvs);
                  native_device_context->CSSetUnorderedAccessViews(0, 1, null_uavs, nullptr);

                  SR::SettingsData settings_data;
                  settings_data.output_width = resolution.x;
                  settings_data.output_height = resolution.y;
                  settings_data.render_width = resolution.x;
                  settings_data.render_height = resolution.y;
                  settings_data.dynamic_resolution = false;
                  settings_data.hdr = true;
                  settings_data.inverted_depth = true;
                  settings_data.mvs_jittered = false;
                  settings_data.mvs_x_scale = -static_cast<float>(resolution.x);
                  settings_data.mvs_y_scale = -static_cast<float>(resolution.y);
                  // NGX preset K is the recommended high-quality transformer
                  // model for DLAA. The global preset was M, which NVIDIA
                  // defines as the Performance-mode default and leaves more
                  // residual aliasing at native resolution.
                  settings_data.render_preset = 11u;
                  settings_data.auto_exposure = true;

                  bool dlaa_succeeded = false;
                  const bool settings_accepted =
                     sr_implementations[device_data.sr_type]->UpdateSettings(
                        sr_instance_data,
                        native_device_context,
                        settings_data);
                  if (settings_accepted)
                  {
                     // The original FXAA output is currently bound as an RTV.
                     // DLSS and the final copy require it to be unbound.
                     native_device_context->OMSetRenderTargets(0, nullptr, nullptr);

                     SR::SuperResolutionImpl::DrawData draw_data;
                     draw_data.source_color = source_resource.get();
                     draw_data.output_color = game_device_data.dlaa_output.get();
                     draw_data.motion_vectors = game_device_data.motion_vectors.get();
                     draw_data.depth_buffer = game_device_data.scene_depth.get();
                     draw_data.jitter_x =
                        game_device_data.current_jitter_pixels.x;
                     draw_data.jitter_y =
                        game_device_data.current_jitter_pixels.y;
                     draw_data.render_width = resolution.x;
                     draw_data.render_height = resolution.y;
                     draw_data.reset = reset_history;
                     draw_data.pre_exposure = 1.0f;
                     draw_data.frame_index = game_device_data.present_index;

                     if (sr_implementations[device_data.sr_type]->Draw(sr_instance_data, native_device_context, draw_data))
                     {
                        native_device_context->CopyResource(output_resource.get(), game_device_data.dlaa_output.get());
                        dlaa_succeeded = true;
                        device_data.has_drawn_sr = true;
                        game_device_data.dlaa_ran_this_frame = true;
                        if (!game_device_data.logged_dlaa_success)
                        {
                           reshade::log::message(
                              reshade::log::level::info,
                              "[Luma][Yakuza6] DLAA completed with camera motion and Halton jitter.");
                           game_device_data.logged_dlaa_success = true;
                        }
                     }
                  }

                  compute_state.Restore(native_device_context);
                  graphics_state.Restore(native_device_context);
                  if (dlaa_succeeded)
                     return DrawOrDispatchOverrideType::Replaced;
                  device_data.force_reset_sr = true;
               }
            }
         }
      }

      return DrawOrDispatchOverrideType::None; // Don't cancel the original draw call
   }
   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);
      ++game_device_data.present_index;
      device_data.force_reset_sr = !game_device_data.dlaa_ran_this_frame;
      game_device_data.dlaa_ran_this_frame = false;
      {
         std::lock_guard camera_lock(game_device_data.camera_mutex);
         game_device_data.jittered_camera_buffers.clear();
      }
      if constexpr (enable_experimental_dlss_upscaling)
      {
         std::lock_guard upscale_lock(
            game_device_data.upscale_command_list_mutex);
         game_device_data.pending_upscale_source_srv.reset();
         game_device_data.pending_upscale_output_uav.reset();
         game_device_data.native_scaled_scene.reset();
         game_device_data.native_scaled_scene_srv.reset();
         game_device_data.pending_upscale_output_resolution = {};
      }
   }

   void PrintImGuiAbout() override
   {
      ImGui::Text(
         "Yakuza 6 Native DLAA - based on Luma Framework",
         "");
   }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      Globals::SetGlobals(
         PROJECT_NAME,
         "Luma - Yakuza 6 Native DLAA",
         "https://github.com/Filoppi/Luma-Framework/");

      // Production play build: shader analysis/dumping is disabled.
      auto_dump = false;
      // Don't automatically load custom shaders
      auto_load = false;

      // Keep the target application's swapchain untouched. Debug texture visualization
      // is unavailable in this mode, but capture/trace tools remain functional and the
      // analyzer cannot reinterpret an SDR back buffer as scRGB.
      swapchain_format_upgrade_type = TextureFormatUpgradesType::None;
      swapchain_upgrade_type = SwapchainUpgradeType::None;
      force_disable_display_composition = true;

      // It might break some games, but at least one can alt tab quickly.
      prevent_fullscreen_state = false;

      game = new GraphicsAnalyzer();
   }
   else if (ul_reason_for_call == DLL_PROCESS_DETACH)
   {
      reshade::unregister_event<reshade::addon_event::update_buffer_region_command>(
         GraphicsAnalyzer::OnUpdateBufferRegionCommand);
      reshade::unregister_event<reshade::addon_event::map_buffer_region>(
         GraphicsAnalyzer::OnMapBufferRegion);
      reshade::unregister_event<reshade::addon_event::unmap_buffer_region>(
         GraphicsAnalyzer::OnUnmapBufferRegion);
      if constexpr (enable_experimental_dlss_upscaling)
      {
         reshade::unregister_event<
            reshade::addon_event::execute_secondary_command_list>(
               GraphicsAnalyzer::OnExecuteSecondaryCommandList);
      }
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}
