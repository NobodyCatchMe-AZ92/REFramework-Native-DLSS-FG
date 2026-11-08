#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "log.hpp"

#include "game_profile.hpp"

namespace {
using game_profile::Profile;
using game_profile::Upscale;

Profile make_re2() {
    Profile p{};
    p.name = "RE2";
    p.tested = true;
    p.upscale = Upscale::DRAW;
    p.bloom_stabilizer = true;
    p.native_dlaa = true;
    // Its depth of field (the same component as RE7's: inventory f/2.8, cutscenes f/2 - f/64) doesn't shimmer at DLAA, but
    // loses its blur at the lower modes: the plugin's after DLSS (testing).
    p.own_dof = true;
    p.examine_dlss_off = true;
    p.guard_live_rule = "GUI_ItemBox";
    p.native_path_dlss = true;
    p.missing_upscale_fallback = true;
    p.temporal_aa_off = true;
    p.sharpness_setting = 1;
    p.examine_stabilizer = true;
    p.dither_experiments = true; // [EXPERIMENTAL], off unless chosen in the menu
    return p;
}

Profile make_re4() {
    Profile p{};
    p.name = "RE4";
    p.tested = true;
    p.upscale = Upscale::COMPUTE;
    p.factory_class_hooks = true;
    p.lod_bias_wrapper = Profile::LodBiasWrapper::R8_SCENE;
    // At native size RE4 skips its compute upscale and composes (exposure) straight into the scene target: DLAA in place
    // after that draw. (DLSS's image in another full-size compute pass's output came out overexposed.)
    p.native_dlaa = true;
    p.native_dlaa_in_place = true;
    // Its forced sharpening (strong: edge contrast 24 -> 32 without RE4.Sharpness, DLSS off) is in its upscale pass.
    p.sharpening_in_upscale = true;
    p.bloom_transfer = true;
    p.compute_upscale_input = true;
    // Its motion blur (before the upscale) made foliage flicker under DLSS while moving: the plugin's after DLSS, as RE7.
    p.own_motion_blur = true;
    // Its depth of field (via.render.DepthOfField: gameplay f/5 at 22 m, merchant f/4.8 at 2 m, scopes f/1.8 at 100-120 m)
    // disappears below DLAA (its upscale pass, which DLSS replaces, composes it): the plugin's after DLSS (testing).
    // The plugin's own doesn't match it (its Lite near blur, a second component on ScopeCamera, a 10x sensor at the
    // merchant): too strong near the camera, gone in scopes and at the merchant even at DLAA. Off; see development notes.
    p.own_dof = false;
    return p;
}

// RE7 (TDB 70, the 2023 version): upscales per frame either with a draw into the scene target (with the GUI's
// weapon/inventory layers on screen) or with a compute pass writing the scene target itself (the rest): both armed. The
// draw path's last render target isn't render-sized there: the HDR color fallback. Bloom stabilizer and native DLAA: not
// yet tested in RE7.
Profile make_re7() {
    Profile p{};
    p.name = "RE7";
    p.tested = true;
    p.upscale = Upscale::COMPUTE;
    p.hdr_color_fallback = true;
    p.factory_class_hooks = true;
    // At the native rate (1 + 0.25/height) RE7 keeps its compute upscale (1:1 into the scene target): DLAA through it.
    p.native_dlaa = true;
    p.compute_upscale_at_native = true;
    // Videotape segments: no upscale step, black below render scale 1 (the game's own path).
    p.no_upscale_fallback = true;
    // ...and DLSS in them: up to the tape's own 2/3 resolution, scaled to the screen as the game does.
    p.videotape_mode = true;
    p.whole_render_size = true;
    p.min_half_render = true; // below half the output RE7's title menu freezes (2026-11-08); gameplay is fine
    p.min_half_object = "TitleScreen";
    p.own_motion_blur = true;
    p.compute_upscale_input = true; // testing: with motion blur on, RE7 upscales its post effects' result, not HDRTarget
    p.own_dof = true;
    return p;
}

// RE3 (2020, DirectX 12 build). Its upscale depends on RenderConfig.SharpnessSetting: at the game's 2 a compute pass
// writing the scene target (Dispatch 120x68 of 16x16, reading the post effects' 960x540 result, as RE7); at 1 (set while
// DLSS runs, as RE2) RE2's draw, reading HDRTarget (traces 2026-11-04; the log showed the compute pass back whenever DLSS
// was switched off). Both armed (COMPUTE also arms the draw). With the draw RE3 is RE2: the game's motion blur works
// through DLSS (tested), the typewriter's save screen is black (all modes, DLAA too). Its HDRTarget is still a render target
// at the marker (the game copies it right after), unlike RE2's: bloom_input_render_target for the bloom stabilizer.
// All reflected names checked against RE3's il2cpp dump.
Profile make_re3() {
    Profile p{};
    p.name = "RE3";
    p.tested = true; // tested 2026-11-04
    p.upscale = Upscale::COMPUTE;
    p.hdr_color_fallback = true;
    p.factory_class_hooks = true;
    p.compute_upscale_input = true;
    p.native_dlaa = true; // tested: DLAA at 1920x1080 (2026-11-04)
    p.compute_upscale_at_native = true;
    p.whole_render_size = true;
    // Flickering bright spots on shiny surfaces (fences) from the bloom of the jittered scene, gone with bloom off (tested).
    // No "moving" limit (RE2's 0.1 px is for its Model Viewer's trails): at 1 px the flicker came back while moving; with
    // every pixel stabilized (reprojected, neighbourhood-clamped) calmer while moving and no smearing (tested 2026-11-04).
    p.bloom_stabilizer = true;
    p.bloom_input_render_target = true;
    p.bloom_moving_px = 1000.0f;
    // Save screen: the guard kept the engine's frame there (k 0.006 r 0.98: black drawn, the room in the post color), RE2's
    // item box case: its live rule while the save GUI shows.
    p.guard_live_component = "offline.gui.SaveBehavior";
    // ...and still black at Performance only: at render scale exactly 0.5 the save screen's half-output background buffer
    // (960x540) is render-sized and was taken as DLSS's input instead of HDRTarget (descriptor probe, 2026-11-04). The scene
    // color always (RE3's upscale draw reads HDRTarget itself); Performance stays exactly 0.5.
    p.scene_color_source = true;
    // Item examine: the lens rule caught only its first second (the lens goes off while examining): its GUI component.
    p.examine_component = "offline.gui.EsInventoryDetailBehavior";
    // RE2's lens (the same DepthOfField component): the plugin's after DLSS. Its item examine lens (f/2.0 at 0.69 m) isn't
    // IgnoreNear and is on only as the view opens: recognised by its GUI component (examine_component below).
    p.own_dof = true;
    p.examine_dlss_off = true;
    p.examine_stabilizer = true;
    // Not by its lens (examine_focus_below): the save screen's camera has the same lens (f/2.0 at 0.69 m), and "examining"
    // there blocked the save screen's live rule (the engine's black frame kept: k 0.02 r 0.98, tested: black at the save screen).
    p.native_path_dlss = true;
    p.missing_upscale_fallback = true;
    // RE2: SharpnessSetting 0 blew DLSS's input out; 1 brings RE2's draw upscale (RE3's own value is 2).
    p.sharpness_setting = 1;
    return p;
}

// RE8 (Village, 2021, DirectX 12 with ray tracing): a starting point (2026-11-04), not yet traced in the game. Every
// reflected name the plugin uses is in its il2cpp dump (DepthOfField with RE2's lens and BokehControl, MotionBlur,
// RenderConfig SharpnessSetting / ImageQualityRate). Both upscale styles armed (COMPUTE also arms the draw) so the first
// trace shows which one RE8 uses. GUI classes for later: app.GUISaveLoad (save screen), app.GUIDetailSearch (item
// inspect), app.GUIInventory. The RE8.NoTAA.dll plugin switches its TAA off too.
Profile make_re8() {
    Profile p{};
    p.name = "RE8";
    p.upscale = Upscale::COMPUTE;
    // DLAA at the true native size (render scale a hair above 1, as RE2 / RE7; was one pixel below): its gameplay upscales
    // with the draw, its title screen with the compute pass, kept at native too.
    p.native_dlaa = true;
    p.compute_upscale_at_native = true;
    // Item examine (Balanced / Quality / DLAA, tested): the blurred background flickered violently where bright sparkles are
    // (the game's depth of field at render resolution makes bokeh discs of what the jitter catches): the plugin's after DLSS,
    // as RE2 / RE3 / RE7 (the same DepthOfField component). Tested: flicker gone; 30% of the plugin's lens matches the game's
    // examine blur (its 0.1 m sensor). Cutscenes: under investigation (two components?).
    p.own_dof = true;
    p.own_dof_scale = 0.3f;
    // Cutscenes: its sequencer animates the aperture from the current value (f/10000 eased from: the game's blur leaked
    // back, the lens read back as ~f/9666); MaxCocSize 0 doesn't switch its blur off. The game's own there (tested: it matches
    // vanilla), the plugin's only in the examine view, where the flicker was.
    p.own_dof_examine_only = true;
    p.examine_by_lens = false;
    p.examine_component = "app.GUIDetailSearch";
    p.hdr_color_fallback = true;
    p.factory_class_hooks = true;
    p.compute_upscale_input = true;
    // Gameplay (trace 2026-11-05): RE2/RE3's draw upscale (marker -> HDRTarget copied -> bloom chain 480x270..7x4 from
    // HDRTarget -> composite -> post draw into a second buffer -> the upscale draw from it); the title screen the compute
    // pass. Tested: detail stable, brightness = DLSS off, FG 2x; bloom flicker on bright spots: RE3's stabilizer (HDRTarget a
    // render target at the marker: at the chain's start, every pixel).
    p.bloom_stabilizer = true;
    p.bloom_input_render_target = true;
    p.bloom_moving_px = 1000.0f;
    // Bloom after DLSS (the stabilizer above only while it is off): the game's SoftBloom settings, matched by eye against
    // its own (tested 2026-11-05: strength 14.2, inner glow 2.5, its threshold read in tenths); then 0.35x of that strength
    // preferred (tested 2026-11-08): 14.2 * 0.35 = 4.97.
    p.own_bloom = true;
    p.own_bloom_strength = 4.97f;
    p.own_bloom_inner = 2.5f;
    return p;
}

// Untested games: the draw path with the safety nets on (the upscale guard decides per frame anyway).
Profile make_generic() {
    Profile p{};
    p.name = "generic";
    p.upscale = Upscale::DRAW;
    p.hdr_color_fallback = true;
    p.factory_class_hooks = true;
    return p;
}

Profile choose() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    auto exe = std::filesystem::path(path).filename().wstring();
    std::transform(exe.begin(), exe.end(), exe.begin(), ::towlower);

    auto profile = exe == L"re2.exe" ? make_re2() : exe == L"re3.exe" ? make_re3() : exe == L"re4.exe" ? make_re4() : exe == L"re7.exe" ? make_re7() : exe == L"re8.exe" ? make_re8() : make_generic();
    log_info("Game profile: {}{}", profile.name, profile.tested ? "" : profile.name == "generic" ? " (no profile for this game: untested defaults)" : " (not yet verified in the game)");
    return profile;
}
} // namespace

namespace game_profile {
const Profile& current() {
    static const Profile profile = choose();
    return profile;
}
} // namespace game_profile
