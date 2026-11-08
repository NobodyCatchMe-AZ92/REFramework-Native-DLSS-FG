# Native DLSS + Frame Generation: design and findings

How the plugin works, what was learned about RE Engine along the way, what was tried and dropped, and what is still
open. Written for anyone who wants to understand the code, port it to another RE Engine game, or build on it. The code
comments refer to this document as "the development notes".

Games covered: Resident Evil 2 (2019, TDB 70), Resident Evil 3 (2020), Resident Evil 4 (2023, TDB 71), Resident Evil 7
(TDB 70, DirectX 12 build) and Resident Evil Village (RE8). "TDB" is the engine's type database version, as REFramework
reports it.

Contents:
1. Design rules
2. The pipeline in one frame
3. Finding the scene's inputs
4. Where DLSS goes: the marker and the engine's upscale
5. Jitter, motion vectors and camera cuts
6. Render scale, DLAA and texture LOD bias
7. Frame generation
8. Frame pacing and performance
9. Effects built for the engine's TAA (bloom, depth of field, motion blur, sharpening)
10. Menus, fades and special screens (the upscale guard and its relatives)
11. Per-game notes
12. The RE2 hair/shadow dither experiment
13. Compatibility (REFramework builds, pd-upscaler, other mods)
14. Open leads
15. Gotchas

---

## 1. Design rules

1. Only REFramework's official C plugin API (`include/reframework/API.h`, API 1.15 = REFramework v1.5.9.1+71 or
   newer). No REFramework internals, no modified REFramework.
2. Game knowledge comes from the game itself and is validated at runtime: reflected names through the type database,
   offsets found and checked while running, byte patterns verified before hooking.
3. Hooks go on the game's side and are chained (MinHook on the function itself), never replacing other mods' hooks.
4. Present handling is generic and falls back safely.
5. Each feature checks its prerequisites and switches itself off with the reason in the log and the menu.
6. One DLL for every game. Everything game-specific is a switch in a per-game profile (`src/game_profile.*`, chosen
   from the executable name before any hook), so a fix for one game can't change another. Rule used during development:
   a change to shared code means re-testing every game; a profile-only change only that game.
7. The plugin imports only KERNEL32 and USER32. Importing d3d12.dll (even for `D3D12SerializeRootSignature`) made the
   plugin, which REFramework loads very early, load d3d12.dll before the game; another mod's d3d12.dll proxy then broke
   and the game silently fell back to DirectX 11. Graphics functions are taken with `GetProcAddress` from the DLL the game
   already loaded. Shaders are compiled at runtime through `d3dcompiler_47.dll` (LoadLibrary).

## 2. The pipeline in one frame

RE Engine renders the scene at a render scale (`via.render.RenderConfig.ImageQualityRate`), then one "PrePostEffect"
layer upscales it into a full-size HDR target (`PostMainTarget` in the scene's reflection), then post effects,
tonemapping, the HUD (Overlay layer) and `PrepareOutput` (into the swapchain backbuffer). The plugin:

1. **Scene update** (hook on `via.render.layer.Scene` update): writes its Halton jitter into the scene's projection
   matrices and the matching previous matrices (section 5).
2. **PrePostEffect draw** (hook): queues a marker command (a Clear with a magic colour) into the engine's own command
   stream, and remembers which frame's inputs belong to it (a slot index in the colour).
3. **Command list translation** (hooks on `ID3D12GraphicsCommandList`, `src/cl_state.*`): when the engine's commands are
   translated to D3D12, the `ClearRenderTargetView` with the magic colour marks the spot. From there the plugin watches
   the following commands for the engine's upscale (a draw into the scene target, or a compute dispatch writing it) and
   runs DLSS instead (section 4).
4. **After DLSS** (all on the engine's command list, compute passes, state saved and restored): optional FidelityFX CAS,
   the plugin's own depth of field / motion blur / bloom where a game needs them, the upscale guard.
5. **PrepareOutput** (hook): the HUD-less frame for frame generation (section 7).
6. **Present** (DXGI swapchain created through Streamline): frame generation, Reflex, frame pacing.

`cl_state` tracks each command list's pipeline state, root signatures, descriptor heaps and root arguments, so the
plugin can record its own passes in the middle of the engine's list and put everything back (`save()` / `restore()`).
Notes: the engine resets lists on one thread and records them on another, so state is kept per list in a global map
(with a thread-local cache), not per thread; gameplay root signatures have more than 32 parameters (limit 64).

## 3. Finding the scene's inputs

From the main scene layer (enabled, not a mirror, its camera = `SceneManager.MainView.PrimaryCamera`):

| Input | Where | Format (RE2) |
|---|---|---|
| Depth | reflection `DepthStencilTex` (Texture) | R32G8X24_TYPELESS, reversed (near = 1) |
| Motion vectors | reflection `VelocityTarget` (TargetState) -> rtv[0] -> Texture | R16G16B16A16_SNORM, xy in clip units |
| Colour (render size) | `HDRTarget` (the low-res HDR colour), or the last render target before the upscale | R11G11B10_FLOAT |
| Scene target (output size) | `PostMainTarget` | R11G11B10_FLOAT |
| Camera | the scene's `SceneInfo` (7 matrices: view-projection, view, inverse view, projection, inverse projection, inverse view-projection, previous view-projection) | |

Object chain (offsets found at runtime, cached): TargetState rtvs +0x10, RTV -> Texture +0x90, SRV -> Texture +0x20,
Texture -> RenderResource +0x98 -> `ID3D12Resource` +0x10 (RE4: RenderResource header 0x10 different).

- Motion vectors are `previous - current` position in clip units; DLSS gets `mvecScale = (0.5, -0.5)`.
- The scene has six `SceneInfo`s (SceneInfo, DepthDistortion, Filter, ZPrepass, JitterDisable, JitterDisablePost); the
  first four get the jitter, the JitterDisable ones are the unjittered references.
- RE Engine's camera is right-handed: `projection.m[2][3] = -1`. An offset `+s` added to `m[2][0]` (row-vector
  matrices) moves the picture by `-s` in NDC.
- Some games run post effects into a second render-sized buffer (RE4, RE7 with motion blur, RE3's save screen): the
  upscale's real input is then that buffer, not `HDRTarget` (profile `compute_upscale_input`, `scene_color_source`).

## 4. Where DLSS goes: the marker and the engine's upscale

**DLSS runs before tonemapping and post effects**, replacing the engine's own upscale, so the HUD and post effects are
drawn afterwards at full resolution and HDR goes into DLSS (`colorBuffersHDR`, auto exposure).

Two upscale styles exist, per game (profile `upscale`):
- **Draw path (RE2, RE3 with SharpnessSetting 1, RE8 gameplay):** after the marker the engine binds `PostMainTarget`
  and draws two quads: quad 1 = the upscale, quad 2 = its screen overlay layer (logos, fades). `cl_state::arm` remembers
  the last resource made a render target after the marker (the low-res post colour) and, at the bind, calls the
  handler: REPLACE (DLSS output copied in, the engine's first draw skipped; `dlss_skip_draws` = 1), AFTER_DRAWS (the
  engine's draw runs, the handler decides afterwards: the upscale guard), or KEEP.
- **Compute path (RE4, RE7, RE3 with SharpnessSetting 2, RE8 title):** an output-sized texture becomes a UAV while the
  HDR colour leaves the render-target state (in either order), then a Dispatch writes it (16x16 groups covering the
  output). `cl_state::watch_compute_upscale` calls the handler right after that Dispatch; DLSS's output is copied into
  the engine's upscale output, so the engine's own composition (exposure etc.) still runs on it. In RE7 the compute
  pass writes the scene target itself (in place).

RE3 switches style with `RenderConfig.SharpnessSetting`: 2 (its default) = compute, 1 = RE2's draw. RE2 needs
`SharpnessSetting` 1 while DLSS runs (0 made DLSS's input far too bright).

## 5. Jitter, motion vectors and camera cuts

- Jitter: Halton(2,3) in [-0.5, 0.5] render pixels, `max(8, 8 * ratio^2)` phases (NVIDIA's recommendation), written
  into the four jittered SceneInfos' projections after the engine's Scene update, only while DLSS works.
- Motion vectors without jitter: each view's previous view-projection is rebuilt as `previous projection with THIS
  frame's jitter * previous view`, so the engine's velocity contains no jitter (`motionVectorsJittered = false`).
- The engine's own TAA is reported as "none" while DLSS runs (hook on `ToneMapping.getTemporalAA` returning 5 = Disable,
  and the anti-aliasing setting), so its jitter doesn't add to ours.
- Streamline accepts one set of `sl::Constants` per frame token: the DLSS pass's constants (real camera) are reused by
  frame generation for that token.
- Camera cuts (camera moved > 1 m, turned > 30 deg, FOV changed > 10%, a new scene, DLSS resuming) are detected and
  logged. Resetting history on them (`dlss_cut_reset=1`) avoids ghosting at cuts but makes frame generation skip a
  frame (a visible dip), so it's off by default.
- DLSS accepts jitter only within half a pixel: jitter offsets of a whole pixel or more drew flickering black lines
  (vertical) or a grid of black dots (horizontal).

## 6. Render scale, DLAA and texture LOD bias

- Render scale = `optimalRenderWidth / output width` from DLSS's optimal settings for the mode, written to
  `ImageQualityRate` (TDB calls pass and return floats as doubles). The engine acts only on changes; frames rendered
  at the previous size for a few frames after a change are skipped (DLSS otherwise sized its feature by the first,
  larger input and kept it: Performance cost 2.4 ms instead of 0.9).
- **True native DLAA:** at exactly 1.0 the engine skips its upscale step (nothing to replace). A rate of
  `1 + 0.25/height` rounds to exactly the output size while keeping the scaled path, and DLSS runs in its DLAA mode
  (profile `native_dlaa`). RE7 and RE3 keep their compute upscale at that size (`compute_upscale_at_native`); RE4 takes
  its native path, where DLAA runs in place after its composition draw (`native_dlaa_in_place`).
- NGX fails a frame larger than the mode allows (0xbad00005 once at startup, harmless); 853x479 is accepted where
  Ultra Performance's exact size is 853x480.
- **Texture LOD bias:** there is no setter, but `via.render.layer.Scene.get_LODBias` returns a UV-derivative scale
  (1 = none). The plugin hooks the engine function behind the TDB wrapper and multiplies it by
  `2^(log2(render/output) + offset)` (offset -1 by default). RE4's wrapper differs (profile `lod_bias_wrapper`).

## 7. Frame generation

- The game's swapchain is created through Streamline's DXGI proxy. RE2: hooks on the game's DXGI imports. RE4 and
  others create their factory before REFramework loads plugins, so the plugin hooks the factory class's
  `CreateSwapChain(ForHwnd)` instead (profile `factory_class_hooks`), skipping REFramework's dummy swapchain.
- **Deadlock found at the start:** `slInit` takes 1.6-3.3 s (NGX updater). Done inside the game's CreateSwapChain, it let
  REFramework install its D3D12 hooks meanwhile, including an inline hook on `sl.dlss_g`'s `linkSwapchainToCmdQueue`
  that waits for REFramework's startup, which waits for the game. Fix: `slInit` when the game creates its DXGI factory,
  and while the plugin creates the swapchain, `unhook_guard` temporarily restores that function's original bytes.
- Real inputs: right after DLSS, depth and motion vectors are copied into a ring of three textures (the engine
  overwrites its own next frame) and tagged at Present with the DLSS pass's constants. Without them (frames where
  DLSS didn't run) neutral stand-ins (no motion, far depth) are used.
- **HUD-less frames:** the Overlay layer composites the whole GUI into the scene target in one fullscreen draw
  (quad 2 of the draw path); `PrepareOutput` then draws the scene target into the backbuffer. `hud_split` puts the
  HUD-free scene into the scene target for PrepareOutput's first draw, copies the result (= the HUD-less frame) into a
  ring, and restores the real frame for the second draw. Tagged as `kBufferTypeHUDLessColor`.
- Multipliers 2x-6x as the GPU/driver allow; switchable live. Frame generation pauses while another window is in front
  (DLSS-G behaviour).
- A third-party plugin that resets its command allocator in every REFramework Present callback broke with frame
  generation (REFramework's Present callback then runs on Streamline's present thread at the doubled rate).

## 8. Frame pacing and performance

- With Streamline's swapchain, the game waited on Streamline's internal frame-latency waitable (signalled only after
  Streamline's copy and present of the previous frame): RE4 lost 20+% (78.7 fps vs 99.3 without). Fix (`frame_pacer`):
  the game gets the plugin's own waitable from `GetFrameLatencyWaitableObject`, a semaphore released when each presented
  frame's GPU work is done (fence on the game's queue, waited on by a plugin thread); the game's own latency setting is
  kept. Games that don't ask for a waitable get the flag added and the plugin waits at frame start.
- GPU cost of the plugin's own passes is measured (`gpu_timer`, logged every status line): 0.01-0.08 ms each. DLSS
  evaluate matches NVIDIA's table for the model (e.g. 1440p Performance: K 0.71-0.75 ms, M 0.92 ms).
- A DLSS preset forced in the NVIDIA app does NOT reach DLSS through Streamline by itself; the plugin reads the driver
  profile (NVAPI DRS, `nvapi_drs.cpp`, read-only) and passes a forced preset letter on. "Latest" selects M for
  Performance, which costs clearly more than K.
- Frame generation multiplies frames, but the game itself then runs at `frame cap / multiplier` (e.g. 40 fps at 3x
  with a 120 cap). Inputs, animations and transitions run at that rate; for responsive gameplay keep it at 60 or more.

## 9. Effects built for the engine's TAA

Several effects assume the engine's TAA and break once DLSS replaces it (jitter without TAA's resolve, or render-scale
dependence). Per effect and game:

- **Bloom flicker (RE2, RE3):** bloom is computed from the jittered render-resolution scene; small bright sources alias
  differently every frame. Fix (`bloom_stabilizer`): before the engine's bloom chain reads the scene, a temporally
  stabilised copy (reprojected with the motion vectors, 3x3 clamp, exponential blend) is swapped in, and the original
  restored before the bloom is added to it. DLSS still gets the jittered scene. RE3: the chain-start trigger
  (`watch_bloom_chain`: a half-size target of the scene's format becoming a render target), no "moving" limit. RE2's
  Model Viewer: pixels with motion pass through (else trails in its strong glow).
- **Bloom after DLSS (RE8, `own_bloom`):** RE8's SoftBloom (CoD: Advanced Warfare pyramid) is switched off
  (`OutputRatio` 0 while DLSS runs) and redrawn on DLSS's output: Karis-weighted 13-tap prefilter with the game's
  threshold, tent upsampling with CoD:AW accumulation, an inner-glow boost on the finest levels. The game's threshold is
  in HDR units divided by 10; the post/HDR brightness ratio is measured on the GPU each frame to convert it. Calibrated
  by eye to the game's look.
- **Bloom transfer (RE4):** the bloom is added in RE4's upscale pass itself; the plugin takes that pass's result minus a
  bilinear upscale of its input (at 1/8 size, median of 16 differences against sparkles) and adds it onto DLSS's image.
- **Keeping bloom out of DLSS (RE2, `dlss_bloom`, experimental):** DLSS on the scene before bloom, then the bloom
  difference added back. Dark outlines around lights: the post step clamps highlights and lifts darks, so the
  difference isn't linear. Left off.
- **Depth of field (`own_dof`, RE2/RE3/RE7, RE8 item examine only):** the game's DoF runs at half the render
  resolution: it shimmers without TAA and loses sub-pixel blur at lower modes. The plugin reads the game's live lens
  (FNumber, FocusDistance, SensorSize, MaxCocSize, BokehControl Default/IgnoreNear/IgnoreFar, near/far bokeh scale),
  neutralises the game's blur (FNumber 10000; `Enabled = false` turned the image white because a later step uses the
  DoF output), and redraws it after DLSS at the output resolution: thin-lens circle of confusion from the reversed
  depth, golden-angle spiral gather, the CoC smoothed over time (reprojected, clamped to the 3x3 render texels) to stop
  edge shimmer. RE4: its lens didn't match (scope / merchant), the game's own kept. RE8 cutscenes animate FNumber from
  the current value, so the f/10000 trick leaks into them: RE8 uses the plugin's DoF only on the examine screen.
- **Motion blur (RE4, RE7, `own_motion_blur`):** their motion blur runs before the upscale, so DLSS got a blurred,
  flickering image (foliage). The camera's MotionBlur component is neutralised while DLSS runs (ShutterAngle or
  ExposureFrame 0, remembered) and the plugin blurs DLSS's output along each pixel's velocity instead, with the game's
  length. The game's option keeps working. RE2/RE3: fine as is.
- **Forced sharpening:** `ToneMapping.get_Sharpness` returns a fixed value in TAA modes 3/4 (forced sharpening); it
  oversharpens DLSS's output. The plugin returns 0 while DLSS runs (hooks on the component's getter, the render-data
  getter 0x40 bytes later, and the method through REFramework), unless another mod already does it. RE4 sharpens inside
  its upscale pass, which DLSS replaces. The plugin's own FidelityFX CAS replaces it (on by default, adjustable).
- **FidelityFX CAS on HDR (`cas_sharpen`):** per pixel the 3x3 neighbourhood is scaled so its brightest channel is 0.5
  (exposure-independent, headroom above), passed through a square root (sharpening as on the displayed image), CAS's
  no-scaling filter (sharpness 0) applied, blended by the strength and converted back. Runs right after DLSS.

## 10. Menus, fades and special screens

- **The upscale guard (`upscale_guard`):** during splash screens and menu transitions the engine's "upscale" draw
  shows something else (black, fades) while the post colour is stale. The engine's draw is allowed to run, then on a
  sample grid its result E is compared with a bilinear upscale S of the post colour: `k = sum(e*s)/sum(s*s)`,
  `r = sum((e - k*s)^2)/sum(e^2)` (luminance). A match (r < 0.1) shows DLSS * k (k carries fades), anything else the
  engine's own frame. Gameplay frames come out at k 1.000, r 0.0000.
- **The live rule (RE2 item box, RE3 save screen):** the engine draws black behind those menus while the post colour
  holds the dimmed room; a post colour that changed since the last frame plus a non-black engine image means DLSS.
  Only while that screen's GUI object/component is shown.
- **No upscale step at all (RE2 typewriter and Story menu, below 100% render scale):** the engine skips its upscale and
  the full-size pass comes out black (unmodded too). DLSS from the scene colour replaces it (`native_path_dlss`).
- **Upscale skipped on some frames (RE2 item-use animations below 100%):** the picture froze (unmodded too). A marker
  before every PrepareOutput detects frames where the upscale didn't happen and puts DLSS there
  (`missing_upscale_fallback`, only within 60 frames of a real upscale).
- **RE7 videotape segments:** render at 2/3 of the render scale with their own upscale through the game's TAA. The
  plugin keeps the game's TAA on there, replaces the engine's jitter with its own (terms from `JitterDisableSceneInfo`),
  runs DLSS to the tape's 2/3 size and scales it to the screen (Catmull-Rom), as the game does (`videotape_mode`).
  Without TAA the tape path has no upscale at all (black); `no_upscale_fallback` covers the frames before detection.
- **RE7 title menu below half resolution:** with its TAA off, RE7's title scene freezes or goes black whenever the
  render size is under half the output on either side (whole pixels or not); gameplay, pause and inventory are fine.
  The plugin holds the render size at half while the `TitleScreen` GUI object exists in the scene and runs Ultra
  Performance as Performance there (`min_half_render`, `min_half_object`).
- **Item examine (RE2, RE3, RE8):** a still item shimmers on its edges with DLSS; an examine stabilizer after DLSS
  (still pixels, clamped history) and no jitter while examining fix it (0.58% -> 0.07% changing pixels). "Force DLAA
  while examining items" keeps a rotating item sharp. Detected by the DoF lens (RE2) or a GUI component (RE3:
  `offline.gui.EsInventoryDetailBehavior`, RE8: `app.GUIDetailSearch`).
- GUI detection helpers: `gui_object_shown` (a game object by name, drawn), `gui_object_exists` (in the scene at all),
  `gui_component_shown` (a component type with its game object drawn).

## 11. Per-game notes

- **RE2:** draw path, bloom stabiliser, live rule (item box), native-path DLSS (typewriter), missing-upscale fallback,
  SharpnessSetting 1, TAA reported off, own DoF, examine stabiliser, the experimental dither fix.
- **RE3:** same engine generation as RE7; upscale style follows SharpnessSetting (1 while DLSS runs = RE2's draw path).
  Bloom stabiliser at the chain start; save screen via the live rule on `offline.gui.SaveBehavior` and the scene colour
  always from `HDRTarget` (at exactly 960x540 the save screen's half-size background buffer was otherwise taken as the
  source). Small distant lights are sub-pixel at Performance and drop out (resolution limit).
- **RE4:** compute path; MinHook needed a fix (only 4 MB free within 2 GB of `re4.exe`, largest block 60 KB: the
  trampoline buffer search range extended and a fallback in a module section's zero tail, see
  `third_party/minhook/src/buffer.c`); DXGI factory class hooks; bloom transfer; own motion blur; DLAA in place; often
  CPU-limited (its rendering at output size and the frame pacing matter more than the DLSS mode).
- **RE7:** compute path writing the scene target in place (the GUI bind after it is left alone); own DoF and motion
  blur; videotape mode; whole-pixel render sizes; the title-menu half-resolution limit.
- **RE Village:** draw path in gameplay (compute on the title); own bloom after DLSS; native DLAA through the compute
  upscale; own DoF only on the item examine screen. Ships its own `nvngx_dlss.dll` next to the exe; the plugin's folder
  is used first.

## 12. The RE2 hair/shadow dither experiment

RE2 draws hair and some shadows with a dither pattern its TAA used to hide. DLAA/Quality resolve it; Performance shows
it (one render pixel = 2x2 output pixels).

- The pattern is fixed to the render-pixel grid: nothing per frame drives it. The engine's jitter values in SceneInfo
  (cycling 16 phases at +0x2a0 etc. with its TAA on, zero with it off) don't change it; the game's TAA with the
  plugin's jitter replacing the engine's only blurred over it.
- Working approach (`re2_dither`, experimental, off by default): the scene is rendered moved by whole render pixels in
  a 4x4 loop where every step is exactly one pixel (only the projection gets the offset; DLSS is told only the
  sub-pixel jitter), and DLSS's inputs (colour, depth, motion) are moved back by a compute pass before it runs. The
  pattern then lands on a different part of each surface each frame and DLSS averages it. The engine's previous
  matrices carry last frame's shift, so its own temporal passes (ray-traced reflection denoiser) follow the picture;
  DLSS's motion vectors get that change taken out (still pixels snapped to exactly 0: SNORM16 rounding otherwise left
  ~0.01 px). The edge strip the picture moved away from comes from DLSS's previous output.
- Dropped on the way: DLSS told whole-pixel jitter (black lines/dots); no correction (reflections jittered, then
  "rotated" with the loop); jumps of 3 px in the loop (edges in the game's image jumped); the shift injected into the
  finished motion vectors instead of the matrices (edges jittered every frame: matrices and motion vectors disagreed);
  the offset in the engine's TAA jitter values (ignored with TAA off); 6x6 / 8x8 loops (barely better than 4x4, wider
  edge strip).
- Result: a small improvement on hair and distant shadows. Shadows from nearby lights keep their grain: no engine
  setting tried changed it (spot/point ShadowVariance and LOD bias, translucent/detail shadows, the directional light's
  `SSTDitherEnable`, `LightRenderer` ShadowFilter / BlueNoiseSpp up to 256 / ShadowPrecision, `AlphaAA`, `FurBlur`).

## 13. Compatibility

- **REFramework:** official nightly builds with plugin API 1.15 (tested nightly-01431/01433). Older builds: the plugin
  sets nothing up and says so (it reads REFramework's log for "Tag / Commits past tag"; an untagged CI build is treated
  as unknown and allowed).
- **REFramework pd-upscaler builds** (TemporalUpscaler + PureDark's `PDPerfPlugin.dll`): both together crashed. With
  `PDPerfPlugin.dll` present the plugin patches its `IsUpscaleMethodAvailable` to "none available" the moment the DLL is
  mapped (a DLL load notification; a GetProcAddress hook was bypassed because kernel32's GetProcAddress doesn't go
  through kernelbase's), so REFramework's upscaler stays off and this plugin's DLSS runs. `override_pd_upscaler=0` makes
  the plugin stand down completely instead. An old (2024) PDPerfPlugin's NGX failed with newer NVIDIA feature DLLs next
  to the game.
- **Other mods:** ReShade and other dxgi/d3d12 proxies work. Don't combine with other upscaler / frame generation mods'
  upscaling or frame generation. Mods that remove the game's sharpening are detected and left in charge.
- **NVIDIA files:** loaded from `reframework/data/Native DLSS + Frame Generation/` (Streamline's `pathsToPlugins`; NGX
  searches the same folder first), falling back to the game folder. Only NVIDIA-signed Streamline is loaded.

## 14. Open leads

- **Bloom after DLSS for RE2 and RE3** (as RE8): RE2/RE3's bloom is a 480x270..7x4 down/up chain after the marker;
  their own bloom could be redrawn on DLSS's output like `own_bloom`.
- **Close-shadow dither in RE2:** likely a fixed sample-rotation pattern in the shadow filter, in the light's or the
  surface's space. `LightRenderer.ShadowRotation` is a `via.render.ConstantBuffer`: probe its contents (a rotation
  table? filled once or per frame?) and rotate it per frame so DLSS can average the grain.
- **An "Auto" frame generation multiplier:** the highest multiplier that keeps the game's own frame rate (cap /
  multiplier) at or above a target such as 60.
- **Known limitations:** RE8's own bloom is absent on frames the guard leaves to the engine (fades, splash); the own
  bloom / motion blur keep their old textures after a resolution change (a small leak per change); RE7 title menu
  limited to half resolution.
- **More RE Engine games:** the generic profile (both upscale styles armed, HDR fallback, factory hooks) is a starting
  point; a command trace (`dlss_debug=4` or `10`) shows which path a game uses.

## 15. Gotchas

- TDB method calls pass and return floats as doubles.
- The plugin API's `reflection_property` size is unreliable for pointer properties: call the getter into a buffer and
  read the first pointer.
- Never import graphics DLLs (section 1).
- RE Engine renders the title screen of RE2 with a depth-less backdrop: don't judge DLSS or motion vectors there.
- REFramework Lua: a literal `%` in imgui text must be `%%`; `fs.write` paths are relative to `reframework/data`.
- Engine state changes only take effect when they differ from what the engine last saw (render scale nudges).
- A changing post colour with the same motion is not proof of a live picture: paused scenes behind menus still change
  with the jitter.
- Streamline logs "Don't know the size ... native 13" for the SNORM motion vectors: VRAM accounting only, harmless.
