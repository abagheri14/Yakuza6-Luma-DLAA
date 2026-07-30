// Reconstruct camera motion from Yakuza 6's full-resolution depth buffer.
// The game has no native velocity render target, so this handles static
// geometry and camera motion. Dynamic-object velocity is added separately if
// a reliable previous-object transform source is found.

cbuffer Yakuza6MotionData : register(b0)
{
   float4 ReprojectionRow0;
   float4 ReprojectionRow1;
   float4 ReprojectionRow2;
   float4 ReprojectionRow3;
   uint2 Resolution;
   uint ResetHistory;
   uint Padding;
}

Texture2D<float> SceneDepth : register(t0);
RWTexture2D<float2> MotionVectors : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatch_thread_id : SV_DispatchThreadID)
{
   if (any(dispatch_thread_id.xy >= Resolution))
      return;

   if (ResetHistory != 0)
   {
      MotionVectors[dispatch_thread_id.xy] = 0.0;
      return;
   }

   const float2 current_uv =
      (float2(dispatch_thread_id.xy) + 0.5) / float2(Resolution);
   const float2 current_ndc =
      float2(current_uv.x * 2.0 - 1.0, 1.0 - current_uv.y * 2.0);
   const float depth = SceneDepth.Load(int3(dispatch_thread_id.xy, 0));
   const float4 current_clip = float4(current_ndc, depth, 1.0);

   float4 previous_clip;
   previous_clip.x = dot(ReprojectionRow0, current_clip);
   previous_clip.y = dot(ReprojectionRow1, current_clip);
   previous_clip.z = dot(ReprojectionRow2, current_clip);
   previous_clip.w = dot(ReprojectionRow3, current_clip);

   if (abs(previous_clip.w) < 1e-7)
   {
      MotionVectors[dispatch_thread_id.xy] = 0.0;
      return;
   }

   const float2 previous_ndc = previous_clip.xy / previous_clip.w;
   const float2 previous_uv =
      float2(previous_ndc.x * 0.5 + 0.5, 0.5 - previous_ndc.y * 0.5);

   // DLSS receives UV-space current-to-previous vectors through a negative
   // render-size scale, matching the convention used by Luma's other DX11
   // integrations.
   MotionVectors[dispatch_thread_id.xy] = current_uv - previous_uv;
}
