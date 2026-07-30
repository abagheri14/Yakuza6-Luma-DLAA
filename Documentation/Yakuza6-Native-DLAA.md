# Yakuza 6 Native DLAA

Version 1.0.0

This ReShade add-on replaces Yakuza 6's FXAA pass with NVIDIA DLAA at the
game's native rendering resolution. FXAA is used only as a reliable injection
point; the original FXAA shader is not run and no extra FXAA pass is applied
after DLAA.

This release is deliberately DLAA-only. Experimental sub-native DLSS scaling
modes are not included.

## Requirements

- Yakuza 6: The Song of Life for Windows (Steam version tested)
- An NVIDIA RTX graphics card with a current driver
- ReShade installed for `Yakuza6.exe` with full add-on support
- DirectX 11 selected when installing ReShade

## Installation

1. Close the game.
2. Install ReShade for `Yakuza6.exe` if it is not already installed. Select
   DirectX 10/11/12 and use a ReShade build with full add-on support.
3. Extract this archive into the game's main folder, preserving the included
   `Luma` directory. This is the folder containing `Yakuza6.exe`.
4. Remove or move any earlier Yakuza 6 test add-on, especially
   `Luma-Graphics Analyzer.addon64`. Only one version should be loaded.
5. Start the game and use these graphics settings:
   - Anti-aliasing: **FXAA**
   - Render scale: **100%**
   - Resolution: your display's native resolution
6. Open the ReShade overlay and verify that `Yakuza 6 Native DLAA` appears in
   the Add-ons list. Luma's Super Resolution setting should be **Auto** or
   **DLSS**. Auto selects DLSS when it is available.

Restart the game after changing the in-game anti-aliasing mode or render
scale.

## How to verify it is active

The in-game anti-aliasing setting must read FXAA, but the original FXAA pass is
replaced by DLAA. In `ReShade.log`, a successful run contains messages similar
to:

```text
[Luma][Yakuza6] Captured the main camera constants and injected projection jitter.
[Luma][Yakuza6] Generated camera motion vectors.
[Luma][Yakuza6] Native DLAA completed successfully.
```

If those messages are absent, check that:

- `Luma-Yakuza6-DLAA.addon64` is in the same folder as `Yakuza6.exe`.
- `nvngx_dlss.dll` is in that folder.
- `Luma\Yakuza 6 DLAA\Luma_Yakuza6_MotionVectors_!0x020C7006.cso` exists.
- ReShade add-on support is enabled.
- The game is using FXAA at 100% render scale.

## Recommended settings

Disable the game's motion blur when comparing image quality. Its camera blur
can smear water highlights and other bright reflections in motion, independent
of DLAA.

Do not enable a ReShade FXAA or SMAA effect on top of this add-on. That would
soften the DLAA result without fixing temporal aliasing.

## Known limitations

- Very thin, high-contrast subpixel geometry such as distant overhead wires can
  retain a small amount of temporal stair-stepping.
- Yakuza 6 does not expose an object-motion-vector target at this point in its
  renderer. The add-on reconstructs camera motion from depth, so independently
  animated objects may not be as temporally stable as in games with native
  per-object motion vectors.
- Only native-resolution DLAA is supported in this release. Render scales below
  100% intentionally use the game's original path.
- Mods which replace the same FXAA shader or interfere with ReShade's DirectX
  11 add-on hooks may conflict.

## Uninstallation

Close the game, then remove:

- `Luma-Yakuza6-DLAA.addon64`
- `Luma\Yakuza 6 DLAA`

Remove `nvngx_dlss.dll` only if no other installed mod uses that file. This
package does not modify or remove ReShade itself.

## Credits and license

Built from the work of the
[Luma Framework](https://github.com/Filoppi/Luma-Framework/) project by
Filippo Tarpini/Pumbo and contributors. ReShade is by crosire. DLSS and the
NGX runtime are NVIDIA technologies.

This is an unofficial community modification and is not affiliated with SEGA,
Ryu Ga Gotoku Studio, NVIDIA, or the ReShade project.

The source-derived mod code is covered by the included `LICENSE.md`. The
bundled NVIDIA runtime remains subject to NVIDIA's applicable terms and is not
covered by that license.
