# Changelog

## 1.0.1

- Fixed packaged precompiled native shaders not always being recognized by the
  Luma native-shader loader.
- Stopped marking native-resolution motion vectors as low-resolution when
  creating the DLAA feature.
- Made Luma's visible DLSS preset selector apply to Yakuza 6; preset K remains
  the tested default.
- Documented the remaining fast-motion breakup on very thin geometry more
  accurately after extensive in-game testing.
- Kept experimental wire expansion, material replacement, coverage dilation,
  history-bias, and postfilter diagnostics out of the public build because they
  did not provide a meaningful improvement.

## 1.0.0

- Added native-resolution NVIDIA DLAA at the game's FXAA injection point.
- Added projection jitter to both camera-matrix layouts used by rigid,
  skinned, deforming, and vegetation rendering paths.
- Added depth-reconstructed camera motion vectors and temporal-history resets.
- Fixed jitter direction after controlled A/B testing.
- Selected NVIDIA preset K for native DLAA image quality.
- Preserved the game's swap-chain format and existing SDR/HDR output behavior.
- Removed development probes, shader-coverage auditing, SMAA experiments, and
  unfinished sub-native DLSS scaling paths from the public runtime.
