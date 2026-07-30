# Changelog

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
