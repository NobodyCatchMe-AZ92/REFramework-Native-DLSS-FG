# Contributing

Thanks for looking into Native DLSS + Frame Generation. This guide covers building, testing, the code layout, logs and
the diagnostic switches. For *why* things work the way they do (and what was tried and dropped), read
[FINDINGS.md](FINDINGS.md) first.

## Building

- Windows, Visual Studio 2022 (Desktop development with C++), CMake (the one in Visual Studio is fine).
- `build.bat` configures (first time) and builds `build/Release/refw_framegen.dll` (+ `.pdb` and `.map`). It calls
  Visual Studio's `vcvars64.bat` from its default install path; adjust the path in `build.bat` if yours differs.
- Third-party code is in the repository: REFramework's plugin API header, Streamline 2.12 headers, MinHook (with a small
  change in `src/buffer.c` for RE4's crowded address space, see FINDINGS section 11).
- The plugin must import only KERNEL32 and USER32. Check after linking:
  `dumpbin /dependents build\Release\refw_framegen.dll`. Never link d3d12, dxgi or d3dcompiler; take their functions with
  `GetProcAddress` from the DLLs the game loaded.

## Packaging

`package.sh` (Git Bash) builds the release zip in `dist/` in Fluffy Mod Manager's layout:

```
modinfo.ini, screenshot.jpg                              (packaging/; the version filled in)
reframework/plugins/refw_framegen.dll
reframework/autorun/refw_framegen.lua
reframework/data/Native DLSS + Frame Generation/         NVIDIA's DLLs, licenses/, README, notices, VERSION.txt
```

- NVIDIA's DLLs are not in this repository. Put them in `third_party/nvidia/bin/`: Streamline 2.12 (`bin/x64` of
  NVIDIA's Streamline release: `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`,
  `sl.pcl.dll`) and `nvngx_dlss.dll` / `nvngx_dlssg.dll` from NVIDIA's DLSS SDK. They are redistributed under NVIDIA's
  licenses (`third_party/nvidia/`): only as part of the mod, never on their own.
- `VERSION=1.1 ./package.sh` names the release; it refuses to build if `MOD_VERSION` in `lua/refw_framegen.lua` and
  `src/framegen.cpp` doesn't match. Without `VERSION` it builds a dated test package.
- The script scans the package for home paths and e-mail addresses; add your own names with `PII_PATTERN='a|b'`.

## Installing a test build

Install an official REFramework nightly in the game, then copy the package's `reframework` folder into the game folder
(or install the zip with Fluffy Mod Manager). For quick iterations only `reframework/plugins/refw_framegen.dll` (and
the menu script) need replacing. The game must be closed: REFramework keeps the plugin's `.pdb` open.

## Code map (`src/`)

| File | What it does |
|---|---|
| `plugin.cpp` | REFramework plugin exports and DllMain |
| `framegen.cpp` | Settings (`refw_framegen.txt`, reloaded live), status file, Streamline setup, DXGI / swapchain hooks, Present: frame generation, Reflex, NVIDIA's files' location |
| `game_profile.*` | Per-game switches (RE2, RE3, RE4, RE7, RE8, generic), chosen from the executable name |
| `engine_hooks.cpp` | Layer hooks (Scene update/draw, PrePostEffect, Overlay, PrepareOutput), `SceneView.get_Size`, LOD bias, sharpness, TAA |
| `dlss_pass.cpp` | The core: jitter and camera, the marker, DLSS configuration and evaluation, the post-DLSS passes, GUI screen detection |
| `cl_state.*` | Command list state tracking (save/restore around the plugin's passes), the marker, upscale / bloom-chain watches, command traces |
| `scene_inputs.*`, `render_objects.*`, `refw.*` | Finding the scene's textures and camera through REFramework's API |
| `hud_split.*` | HUD-less frames for frame generation |
| `frame_pacer.*`, `wait_probe.*` | Frame latency waitable / pacing; diagnostics of what the game waits on |
| `upscale_guard.*` | DLSS vs the engine's frame on menus, fades and splash screens |
| `bloom_stabilizer.*`, `bloom_transfer.*`, `bloom_composite.*`, `own_bloom.*` | Bloom with DLSS (RE2/RE3, RE4, experimental, RE8) |
| `own_dof.*`, `motion_blur.*`, `examine_stabilizer.*`, `scale_blit.*`, `cas_sharpen.*` | Effects after DLSS |
| `re2_dither.*` | The experimental RE2 dither fix |
| `nvapi_drs.*` | Reads the NVIDIA driver profile (DLSS preset overrides), read-only |
| `descriptor_map.*`, `pipeline_probe.*`, `gpu_timer.*`, `crash_log.*`, `log.*`, `hooks.*`, `unhook_guard.*` | Diagnostics, logging, hooking helpers |

`lua/refw_framegen.lua` is the in-game menu and the on-screen overlay. It only writes the settings file and reads the
plugin's status file (`reframework/data/refw_framegen_status.txt`).

## Ground rules

- **Game-specific behaviour goes behind a profile switch.** A change to shared code needs a check in every game; a
  profile-only change needs only that game.
- **Verify before hooking:** byte patterns checked, prerequisites checked, and a feature that can't run turns itself off
  with a reason in the log and the menu (`report_error`).
- **Hooks are chained** (MinHook on the function), never replacing another mod's.
- **Comments explain why** (what the engine does, what was measured, what was tried). New engine findings belong in
  FINDINGS.md too.

## Logs

- `reframework/data/refw_framegen.log` (the plugin): startup, which NVIDIA files were loaded, render sizes, camera cuts,
  the upscale guard's decisions, and a status block every 5 s (fps rendered/shown, frames upscaled/skipped and why,
  GPU time per pass, inputs, HUD-less frames, engine call counts).
- Streamline's own log lines are included (prefixed `[Streamline]`).
- REFramework's log (`re2_framework_log.txt`, etc. in the game folder) shows plugin loading and Lua errors.

## Diagnostic settings

All in `reframework/data/refw_framegen.txt` (edited by hand; most apply live).

| Setting | What it does |
|---|---|
| `dlss_debug=1` | Split screen: left DLSS, right the engine's upscale; velocity/depth readback in the log |
| `dlss_debug=2` | Jitter x20 (made visible) |
| `dlss_debug=4` / `10` | Command trace after the marker / from PrePostEffect's start (draws, dispatches, barriers, copies) |
| `dlss_debug=5` | Motion vector / depth readback every 300 frames (`refw_framegen_velocity.bmp`) |
| `dlss_debug=7` / `8` | HUD split trace / HUD-less frame snapshot (`refw_framegen_hudless.bmp`, F9) |
| `dlss_debug=9` | Render scale kept, DLSS not evaluated (the engine's own upscale; performance comparisons) |
| `dlss_debug=11` / `12` / `13` | Scene colour readback on the no-upscale path / all scene layers / SceneInfo words that change per frame |
| `dlss_rate_override=0.55` | Force a render scale (0 = the mode's) |
| `dlss_guard=0` / `dlss_guard_log=N` | Upscale guard off / its decision logged every N frames |
| `dlss_cut_reset=1` | Reset DLSS / frame generation history on camera cuts |
| `dlss_half_limit=0` | RE7: no half-resolution limit on the title menu |
| `debug_keys=1` | Live comparison keys: F5 HUD fix, F6 frame generation's real inputs, F7 DLSS, F8 LOD bias (only with the game in front) |
| `perf_bypass=1` / `2` | Command list hooks off / Reflex sleep and latency markers off |
| `perf_waits=1` | Log what the game's threads wait on per frame |
| `probe_descriptors=1` | Descriptor map: which textures a compute pass reads (at startup) |
| `probe_dof=1` | Log the game's depth of field lens changes |
| `probe_drs=1` | Log the NVIDIA driver profile settings for the game |
| `probe_methods=1`, `probe_words=a,b`, `probe_code=Type.Method` | Type database lookups and engine function bytes, at startup |
| `override_pd_upscaler=0` | With a pd-upscaler REFramework: the plugin stands down, REFramework's upscaler runs (restart) |

Temporary REFramework Lua scripts in `reframework/autorun` are the quickest way to explore the engine live
(`sdk.find_type_definition`, `scene:call("findComponents(System.Type)", sdk.typeof(...))`, getters/setters): most
findings in FINDINGS.md started there.

## Porting to another RE Engine game

1. Add a profile in `game_profile.cpp` (start from the generic one: both upscale styles armed, HDR colour fallback, DXGI
   factory class hooks) and the executable name.
2. Run the game, read the log: is the swapchain created through Streamline, is DLSS upscaling every frame ("N upscaled,
   0 skipped"), what does the upscale guard decide?
3. If DLSS doesn't run, take a command trace (`dlss_debug=4`) and find the engine's upscale (a draw into the scene target
   or a compute dispatch writing an output-sized texture).
4. Check menus, fades, splash screens, the save screen, item examine, cutscenes, DLAA and every DLSS mode; then the
   effects (bloom, depth of field, motion blur) at DLAA and Performance.
