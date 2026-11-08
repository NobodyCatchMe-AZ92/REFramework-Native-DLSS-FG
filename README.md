# Native DLSS + Frame Generation

(Plugin file: `refw_framegen.dll`.) DLSS super resolution and DLSS Frame Generation for **Resident Evil 2 (2019)**, **Resident Evil 3 (2020)**,
**Resident Evil 4 (2023)**, **Resident Evil 7** and **Resident Evil Village** (DirectX 12), as a plugin for the official
[REFramework](https://github.com/praydog/REFramework) builds. No modified REFramework needed. One plugin for all five
games: what differs per game is chosen automatically from the game's executable. NVIDIA's Streamline and DLSS files are
included, and everything installs inside the game's `reframework` folder (Fluffy Mod Manager or by hand).

## Features

- **DLSS super resolution before tonemapping and post effects.** DLSS replaces the game's own upscale, so tonemapping,
  lens effects and the HUD are drawn afterwards at full resolution (no ghosting HUD from upscaling). Splash screens, fades
  and menu transitions show exactly as without DLSS.
- **Bloom.** RE4: the game's bloom is added onto DLSS's image. RE Village: the plugin draws the bloom itself after DLSS at
  the output resolution (matched to the game's look; strength and inner glow adjustable), so it doesn't flicker with
  DLSS's jitter. RE2 and RE3: the game's bloom is part of DLSS's input; it reads a stabilized scene, so DLSS's jitter
  doesn't make it flicker (no flicker on puddles and shiny surfaces).
  Modes: DLAA, Quality, Balanced, Performance, Ultra Performance. **DLAA is true native** (DLSS's DLAA mode at the
  output resolution).
- **Depth of field that survives DLSS.**
  - RE2, RE3 and RE7: the game's own depth of field runs at the render resolution, loses its blur at the lower modes and
    shimmers without the game's TAA. The plugin draws the same lens (the game's aperture, focus distance and blur mode)
    after DLSS at the output resolution instead, the same in every mode, with its edges smoothed over time. Strength
    adjustable in the menu (100% = the game's).
  - RE4: the game's own depth of field and bloom at every mode (its upscale pass, which DLSS replaces, used to drop
    them below DLAA).
  - RE Village: the item examine view's background blur is drawn by the plugin after DLSS (no shimmer, matched to the
    game's); cutscenes keep the game's own depth of field.
- **RE2 menus.** The typewriter's save screen and the Story menu keep DLSS (the game itself draws the save screen's
  background black below 100% Image Quality; the plugin supplies it). The item box background shows with DLSS on.
  Item examine: no shimmer on a still item (a stabilizer after DLSS), and **Force DLAA while examining items** (menu,
  on by default) keeps a rotating item sharp. Item-use animations (bolt cutters, fuses, ...) play normally (the game
  itself freezes them below 100% Image Quality). The bonus Model Viewer shows animated characters without trails.
- **RE3 menus.** The save screen keeps the room behind it in every mode, the item box shows normally, and **Force DLAA
  while examining items** works as in RE2 (examined items also steadier at the lower modes).
- **RE Village.** DLAA is true native, menus and the save screen as without DLSS, **Force DLAA while examining items**
  as in RE2/RE3.
- **RE2: [EXPERIMENTAL] hair/shadow dither fix** (menu: Advanced, off by default). An experimental attempt at cleaning up
  the dithering pattern in hair and shadows. It produces improvements, but they are only very minor: the scene is
  rendered moved by whole pixels in a small repeating pattern, and moved back before DLSS, so the game's fixed dither
  pattern changes from frame to frame and DLSS can smooth it. Shadows from nearby lights barely change (their grain
  doesn't follow the screen). While the camera turns fast, a thin strip along the top and left screen edges can lag a
  little.
- **RE7 videotape sections.** DLSS up to the tape's own lower resolution, then scaled as the game does, or (menu:
  **Disable DLSS during Videotape Sections**) exactly as in the unmodded game.
- **FidelityFX CAS sharpening** (menu: **Enable FidelityFX CAS Sharpening**, on by default): AMD's Contrast Adaptive
  Sharpening on DLSS's image, adjustable (**Sharpening** 0 to 1, default 0.50, as ReShade's CAS "Sharpening"). It runs
  right after DLSS, before the game's tonemapping, post effects and HUD (the HUD isn't sharpened). Untick it if you
  use your own sharpening (e.g. ReShade).
- **Automatic texture LOD bias** for the render resolution (log2(render / output) - 1, adjustable).
- **The game's forced sharpening off while DLSS is on** (it oversharpens DLSS's output). If another mod already does
  this (e.g. `RE2_DisableSharpeningAndTAA.lua`, `RE4.Sharpness`), the plugin leaves it to that mod (a script switched
  off in its own menu doesn't count). The menu shows who handles it. RE2 and RE3 need no such mod: the plugin also sets
  what that script used to set for DLSS to look right (without it, RE2 came out far too bright). RE4: its sharpening is part of its own upscale, which DLSS replaces, so nothing sharpens with DLSS on.
- **DLSS Frame Generation, 2x to 6x** (as far as the GPU and driver allow), with the game's real depth, motion vectors
  and camera, and **HUD-less frames** so the HUD stays clean in generated frames. Frame generation can be switched on
  and off live.
- **No frame rate cost from frame generation support while it is off.** The plugin paces the game's frames itself
  instead of through Streamline's swapchain, which otherwise made the game wait for every frame to be presented
  (measured in RE4: 78.7 -> 99.3 fps at native, the same as without the plugin).
- **In-game menu** in REFramework's menu, changes apply live.
- **On-screen overlay**: DLSS / DLSS-G versions in use, the mode running (including DLAA while examining items) and
  preset (including a preset forced in the NVIDIA app), render and output resolution, texture LOD bias, frame
  generation multiplier, frame rates.

## Requirements

- An NVIDIA RTX GPU and a recent driver. DLSS super resolution: RTX 20 series or newer. Frame generation: RTX 40 series
  or newer (more than 2x where the GPU/driver supports multi frame generation).
- The game running in **DirectX 12** mode (RE3: tested with a current REFramework nightly build).
- An **official REFramework nightly** with plugin API 1.15 (tested: nightly-01431 and nightly-01433; any build from April
  2025 on, v1.5.9.1+71 or newer). With an older REFramework the plugin sets nothing up and the menu says so (older builds
  refuse the plugin anyway, and set up halfway it could hang the game).
- RE7: tested with the game's 2023 DirectX 12 build (`re7.exe` of May 2023); later updates are untested.
- NVIDIA's files are included (in `reframework/data/Native DLSS + Frame Generation/`): Streamline 2.12 (`sl.interposer.dll`,
  `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`) and DLSS / DLSS Frame Generation 310.8
  (`nvngx_dlss.dll`, `nvngx_dlssg.dll`), all signed by NVIDIA (the plugin only loads NVIDIA-signed Streamline files). A
  DLSS version or preset forced in the NVIDIA app still takes precedence. Their licenses are in `licenses/` there. An
  `nvngx_dlss.dll` the game itself ships next to its executable (RE Village) stays untouched; the mod's own folder is used
  first. (Older installs with NVIDIA's files next to the executable keep working.)

## Installation

1. Install an official REFramework nightly for the game (see Requirements).
2. **Fluffy Mod Manager:** add the zip as a mod and enable it. **By hand:** copy the `reframework` folder from the zip into
   the game folder (where `re2.exe` / `re3.exe` / `re4.exe` / `re7.exe` / `re8.exe` is), merging it with the existing
   one (`modinfo.ini` and `screenshot.jpg` are only for Fluffy Mod Manager). Everything goes inside `reframework`:

```
reframework/plugins/refw_framegen.dll
reframework/autorun/refw_framegen.lua
reframework/data/Native DLSS + Frame Generation/   NVIDIA's DLLs, their licenses, this README
```

3. Start the game. Open REFramework's menu (Insert) > Script Generated UI > **Native DLSS + Frame Generation**.

To uninstall: disable the mod in Fluffy Mod Manager, or delete the files above (and `reframework/data/refw_framegen.txt`
/ `.log`).

## Settings

The menu writes `reframework/data/refw_framegen.txt` (created with comments on first start; it can also be edited by
hand while the game runs). Settings that are only in the file:

| Setting | Default | |
|---|---|---|
| `dlss_hud_fix` | 1 | HUD-less frames for frame generation |
| `dlss_bloom_stabilize` | 1 | RE2, RE3: stabilized scene for the game's bloom (no bloom flicker on small bright lights) |
| `dlss_guard` | 0.1 | DLSS only where the game really upscales the scene (splash screens, fades and menu transitions as without DLSS; 0 = off) |
| `dlss_cut_reset` | 0 | Reset DLSS / frame generation history on camera cuts (no ghosting at cuts, but a brief frame rate dip) |
| `dlss_sharpening_off` | 1 | The game's sharpening while DLSS is on: 1 = off unless another mod removes it, 0 = kept, 2 = always off (menu: Advanced) |
| `dlss_own_dof` | 1 | RE2, RE3, RE7: the plugin's depth of field after DLSS (0 = the game's own, at the render resolution) |
| `dlss_dof_temporal` | 0.15 | RE2, RE3, RE7: how much each frame changes the depth of field's blur amount (lower = steadier edges; 1 = no smoothing) |
| `dlss_examine_stabilize` | 1 | RE2, RE3: the stabilizer on examined items (still pixels only) |
| `dlss_bloom_transfer` | 1 | RE4: strength of the game's bloom on DLSS's image (0 = off) |
| `reflex` | 1 | NVIDIA Reflex: 0 = off, 1 = low latency, 2 = low latency + boost (frame generation always uses at least 1) |
| `debug_keys` | 0 | Diagnostic hotkeys (menu: Advanced): with 1, `hud_fix_key`, `fg_inputs_key`, `dlss_toggle_key`, `dlss_lod_bias_key` (F5, F6, F7, F8) switch the HUD fix, frame generation's real inputs, DLSS and the LOD bias off/on live, to compare |

## Notes

- While DLSS is on, the plugin sets the game's render scale (the in-game Image Quality setting) and turns the game's
  own TAA off (DLSS replaces it); switching DLSS off restores the render scale. Leave the game's own
  FidelityFX/interlaced upscaling off.
- **DLSS model and the NVIDIA app.** A preset forced in the NVIDIA app always wins (the plugin passes it to DLSS itself;
  through Streamline the driver override alone did not reach DLSS), the menu's preset choice is hidden then, and the
  overlay shows the preset in use. With the NVIDIA app's **"Latest"**, Performance mode uses preset **M**, which costs
  more than K (NVIDIA's figures, RTX 4080 at 4K: M 2.08 ms, K 1.49 ms). Where the game is GPU-limited and you want the
  most frame rate, preset K for Performance (forced in the NVIDIA app) is faster.
- When the CPU is the limit (busy areas, high frame rates), lowering the render resolution can't raise the frame rate;
  frame generation is what helps there.
- RE2: hair and some shadows are drawn with a dither pattern the game's TAA used to hide. DLAA, Quality and Balanced
  resolve it; at Performance and below it can stay visible (the pattern doesn't change from frame to frame, so DLSS
  keeps it). The [EXPERIMENTAL] dither fix (menu: Advanced) helps a little.
- RE2: the typewriter's save screen background is black in the unmodded game below 100% Image Quality; with DLSS on,
  the plugin draws it in every mode.
- RE7: on the title menu the render resolution never goes below half the output on either side. With its TAA off (DLSS
  replaces it), RE7's title menu freezes or goes black below half (e.g. a 2560x1440 window at Performance came out at
  1257x707); there the plugin rounds Performance up to at least half and **Ultra Performance runs as Performance** (the
  overlay says so). Gameplay, the pause menu and the inventory use the mode's own resolution (Ultra Performance included).
- RE3: very small distant lights (e.g. powerline lamps) can drop out at Performance and below: they are smaller than a
  pixel at that render resolution (Quality and DLAA keep them).
- RE4: the merchant's background blur is weaker below DLAA than without DLSS (the game's FSR 2 path shows the same).
- **Motion blur** (RE4, RE7): the games blur before their upscale, which made foliage flicker under DLSS while moving.
  With DLSS on and the game's motion blur option on, the plugin blurs DLSS's output instead, with the game's own blur
  length (the option keeps working as usual). RE2's and RE3's motion blur work with DLSS as they are.
- Frame generation pauses briefly while another window is in front.
- **REFramework pd-upscaler builds** (REFramework's own DLSS/FSR2/XeSS through PureDark's `PDPerfPlugin.dll`): with
  `PDPerfPlugin.dll` next to the game, this plugin keeps REFramework's upscaler off from the start and its own DLSS runs in
  its place (both together crashed the game in testing). Nothing in REFramework's files or settings is changed; the menu
  shows it. `override_pd_upscaler=0` in `refw_framegen.txt` hands upscaling back to REFramework's: this plugin then turns
  itself off completely (restart the game after changing it). Tested with the pd-upscaler build of October 2026 in RE8.
  Notes from testing: UpscalerBasePlugin 1.2.0 offered no upscaler at all in RE8 (an older `PDPerfPlugin.dll` did), and old
  pd-upscaler builds (2024) are too old for this plugin; their `PDPerfPlugin.dll` also failed to start DLSS with the newer
  NVIDIA DLLs (`nvngx_dlssg.dll`, `nvngx_dlssd.dll`) next to the game.
- Don't combine it with other upscaler / frame generation mods' upscaling or frame generation (OptiScaler, DLSS Enabler
  frame generation, ...). Other mods that replace `dxgi.dll` / `d3d12.dll` (ReShade, OptiScaler with its features off)
  worked in testing. ReShade add-ons that hook every draw call can cost a lot of CPU time in these games.

## Known issues

- RE2 and RE3 don't draw their bloom after DLSS yet (RE Village does): it is part of DLSS's input, stabilized.
- RE Village: on fades and splash screens (frames the plugin leaves to the game) there is no bloom for those frames.
- Changing the output resolution several times in one session leaks a little video memory (RE4, RE7 motion blur and
  RE Village bloom keep their old textures); restarting the game frees it.
- RE2's [EXPERIMENTAL] dither fix: very minor improvement; shadows from nearby lights keep their grain; a thin edge strip
  can lag while the camera turns fast.

## Troubleshooting

The menu shows problems (missing or unsigned Streamline files, unsupported GPU/driver, an REFramework that is too old).
Details: `reframework/data/refw_framegen.log`.

## Building from source

- Visual Studio 2022 (C++ desktop workload) and CMake. `build.bat` builds `build/Release/refw_framegen.dll`.
- `package.sh` (Git Bash) builds the release zip in `dist/`. It needs NVIDIA's files in `third_party/nvidia/bin/` (not in
  this repository): Streamline 2.12 (`bin/x64` of NVIDIA's Streamline release on GitHub: `sl.interposer.dll`,
  `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`) and `nvngx_dlss.dll` / `nvngx_dlssg.dll`
  (310.8 in the release; NVIDIA's DLSS SDK). `VERSION=1.0 ./package.sh` builds `dist/NativeDLSS_+_FG_1.0.zip` in Fluffy
  Mod Manager's layout (`packaging/`: its modinfo.ini and screenshot; the version must match `MOD_VERSION` in
  `lua/refw_framegen.lua` and `src/framegen.cpp`).
- License: MIT (`LICENSE`) for this project's code; the third-party licenses are in `THIRD_PARTY_NOTICES.md`.

## Credits

AMD FidelityFX CAS (its algorithm, MIT), REFramework's plugin API and MinHook. See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Not affiliated with or
endorsed by NVIDIA or Capcom. NVIDIA, DLSS and RTX are trademarks of NVIDIA Corporation.
DLSS and DLSS Frame Generation are NVIDIA technologies; this project is not affiliated with NVIDIA, Capcom or REFramework.
