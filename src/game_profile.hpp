#pragma once

#include <string>

// What differs between the games, in one place. The profile is chosen from the game's executable when the plugin loads
// (before any hook), and every game-specific behavior is a switch here: a change made for one game's profile can't alter
// how another game runs. A game without a profile gets the generic one (untested defaults, logged as such).
//
// Adding a game: a new entry in game_profile.cpp, switches set to what that game needs; the others stay untouched.
namespace game_profile {
enum class Upscale {
    DRAW,    // the engine upscales its post color with a draw into the scene target (RE2): DLSS replaces that draw
    COMPUTE, // the engine upscales the HDR color with a compute pass, then composes (exposure) into the scene target (RE4):
             // DLSS replaces the compute pass's output
};

struct Profile {
    std::string name;    // "RE2", "RE3", "RE4", "RE7", or "generic"
    bool tested{};       // a profile verified in the game (generic: no)
    Upscale upscale{Upscale::DRAW};

    // DRAW: when the last render target before the upscale isn't render-sized, the scene's HDR color is the input instead.
    bool hdr_color_fallback{};

    // A temporally stabilized scene for the engine's bloom (RE2: the bloom flickered with DLSS's jitter). Needs the scene
    // readable at the DLSS marker.
    bool bloom_stabilizer{};

    // The game creates its DXGI factory before REFramework loads plugins (RE4): hooks on the factory class's
    // CreateSwapChain(ForHwnd), and a GetProcAddress hook for runtime lookups of CreateDXGIFactory1/2.
    bool factory_class_hooks{};

    // How the type database wrapper of via.render.layer.Scene.get_LODBias calls the engine function (scene, result):
    // RE2 "lea rdx, [rsp+38h]; call"; RE4 (TDB 71) "mov rdx, rcx; mov rcx, r8; call" (the wrapper's result buffer).
    enum class LodBiasWrapper { LEA_RESULT, R8_SCENE } lod_bias_wrapper{LodBiasWrapper::LEA_RESULT};

    // DLAA at the native resolution (DLSS's DLAA mode, render size = output size) through the engine's scaled path: render
    // scale a hair above 1 (at exactly 1.0 RE2 used its own Image Quality option and skipped its upscale). Without it DLAA
    // renders one pixel below native and DLSS's Quality mode reconstructs to native (the fallback for games not done yet).
    bool native_dlaa{};

    // With native_dlaa: the engine takes its native path at that render scale (RE4: no upscale pass, its composition
    // draws straight into the scene target), so DLAA runs in place on the scene target right after that draw.
    bool native_dlaa_in_place{};

    // COMPUTE: the engine keeps its compute upscale at native size too (RE7: 1:1 into the scene target), so native DLAA
    // goes through it. Off (RE4): at native size other output-sized compute passes would be taken for the upscale.
    bool compute_upscale_at_native{};

    // Scenes without any upscale step (RE7's videotape segments: their VHS screen is drawn from the low-resolution image
    // and comes out black below a render scale of 1): after 30 frames with the marker but no upscale, the render scale
    // goes to native and DLSS pauses; the first upscale step seen again brings the DLSS mode's render scale back.
    bool no_upscale_fallback{};
    // RE7: render sizes of whole pixels only (with its TAA off a half pixel, 1680x945 at 0.5, froze the picture black).
    bool whole_render_size{};
    // RE7: not below half the output on either side while its title scene is loaded (min_half_object). With its TAA off
    // (DLSS) RE7's title menu freezes / goes black (cursor trails) whenever the render size is under half the output
    // (2515x1415: 1257x707 at 0.5, 1006x566 at 0.4; 2560x1440 at 1/3; 1680x945: 840x472), whole pixels or not; at half or
    // above it works. Gameplay, its pause menu and inventory work below half (Ultra Performance, 853x480 at 2560x1440).
    // There Ultra Performance runs as Performance.
    bool min_half_render{};
    const char* min_half_object{}; // the GUI object whose presence in the scene marks the title (RE7: TitleScreen); nullptr = always
    // RE7: the engine's motion blur off while DLSS runs, the plugin's after DLSS if the game's option is on (its own before the
    // upscale made foliage flicker under DLSS while moving).
    bool own_motion_blur{};
    // RE8: the game's bloom (via.render.SoftBloom, CoDAW) zeroed while DLSS runs, the plugin's after DLSS from its
    // full-resolution output with the game's threshold / dispersion / output ratio (setting dlss_own_bloom). See own_bloom.hpp.
    bool own_bloom{};
    // ...its calibration against the game's own by eye (RE8: strength x4.97 on the game's output ratio, inner glow 2.5).
    float own_bloom_strength{1.0f};
    float own_bloom_inner{0.0f};
    // RE2: the item examine view (detected by its depth of field mode) gets DLAA (setting dlss_examine_dlaa, menu,
    // on by default): a rotating item is softer at the lower modes (DLSS keeps less history for moving pixels).
    bool examine_dlss_off{};
    // RE2: the upscale guard's live rule (its item box: the engine draws black behind the menu, the post color is the room),
    // only while that screen shows (elsewhere, e.g. the bolt cutter animation, it froze the picture).
    const char* guard_live_rule{}; // the GUI object whose screen it applies to (RE2: GUI_ItemBox), nullptr = none
    // RE2: the typewriter's save screen draws without the upscale step and comes out black (the game's own bug below 100%):
    // DLSS from the scene color replaces that pass (setting dlss_native_path).
    bool native_path_dlss{};
    // RE2: below 100% the engine skips its upscale step on most frames of item-use animations (bolt cutters, fuse: the
    // picture froze, unmodded too): DLSS into the scene target at the output stage then.
    bool missing_upscale_fallback{};
    // RE2: its TAA check (ToneMapping.getTemporalAA) reports none while DLSS runs, as RE2_DisableSharpeningAndTAA does (without
    // either, parts of its TAA route stayed: missing upscales, too bright, white fades).
    bool temporal_aa_off{};
    // RE2: RenderConfig.SharpnessSetting while DLSS runs (0 made DLSS's input far too bright), -1 = left alone.
    int sharpness_setting{-1};
    // RE3: the item examine view recognised by its GUI component being drawn (offline.gui.EsInventoryDetailBehavior: its
    // lens is only on for a moment as the view opens). nullptr = none.
    const char* examine_component{};
    // RE3: the guard's live rule while a GUI component of this type is drawn (its save screen, offline.gui.SaveBehavior: the
    // engine draws black behind it, the post color is the room). nullptr = none (guard_live_rule: by GameObject name).
    const char* guard_live_component{};
    // RE3: the draw path's DLSS input is always the scene color (HDRTarget, render-sized), never another last render target.
    bool scene_color_source{};
    // The bloom stabilizer leaves pixels moving more than this (render pixels per frame) as they are (RE2 0.1).
    float bloom_moving_px{0.1f};
    // RE3: the scene color is still a render target at the DLSS marker (RE2's is readable there): the bloom stabilizer
    // makes it readable for its pass and puts it back.
    bool bloom_input_render_target{};
    // RE2: the item examine view gets a temporal stabilizer after DLSS where nothing moves (setting dlss_examine_stabilize).
    bool examine_stabilizer{};
    // RE2: the [EXPERIMENTAL] hair/shadow dither tests (re2_dither.hpp; setting dlss_dither_test, menu: Advanced, off by
    // default). Nothing of them runs in any other game.
    bool dither_experiments{};
    // RE4: its compute upscale adds the bloom chain; the bloom is taken from its result onto DLSS's (setting dlss_bloom_transfer).
    bool bloom_transfer{};
    // RE4: DLSS from its compute upscale's own input (the scene's second color buffer, depth of field in it), not
    // HDRTarget (setting dlss_compute_input).
    bool compute_upscale_input{};

    // Scenes rendered at 2/3 of the render scale (RE7's videotape segments, a deliberate low-resolution look): recognised
    // by the depth's size; the game's TAA kept on then (their upscale step only exists with it), DLSS outputs the 2/3 size
    // and the plugin scales it to the scene target, the plugin's jitter replacing the engine's.
    bool videotape_mode{};

    // The plugin's depth of field after DLSS at the output resolution, the game's switched off while DLSS is on (RE7: its
    // own runs at render resolution inside DLSS's input, shimmers, and loses its blur at the lower modes). See own_dof.hpp.
    bool own_dof{};
    // ...its strength against the game's own, matched by eye (RE8: 0.3, item examine; the menu slider multiplies it).
    float own_dof_scale{1.0f};
    // ...and its largest blur against the game's MaxCocSize (% of the height), matched by eye (the menu slider multiplies it).
    float own_dof_max_scale{1.0f};
    // RE8: the plugin's depth of field only while the item examine view shows (examine_component); elsewhere the game's own
    // (its cutscenes animate the aperture, so neither neutralising it nor MaxCocSize 0 worked there; the game's matches).
    bool own_dof_examine_only{};
    // RE2's examine rule (the lens's BokehControl IgnoreNear): off for RE8 (its cutscene lenses use it too).
    bool examine_by_lens{true};

    // The engine's forced sharpening is part of the upscale pass DLSS replaces (RE4: measured, nothing sharpens with DLSS
    // on): nothing for the plugin to switch off (the menu says so instead of "off by this mod").
    bool sharpening_in_upscale{};
};

// The profile for this process (chosen on first use from the executable's name).
const Profile& current();
} // namespace game_profile
