#include <windows.h>
#include <psapi.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl.h>
#include <intrin.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_security.h>

#include <reframework/API.h>
#include <MinHook.h>

#include "cl_state.hpp"
#include "engine_hooks.hpp"
#include "game_profile.hpp"
#include "hooks.hpp"
#include "refw.hpp"
#include "dlss_pass.hpp"
#include "descriptor_map.hpp"
#include "hud_split.hpp"
#include "nvapi_drs.hpp"
#include "pipeline_probe.hpp"
#include "scene_inputs.hpp"
#include "log.hpp"
#include "unhook_guard.hpp"
#include "wait_probe.hpp"
#include "frame_pacer.hpp"

#include "framegen.hpp"

#pragma comment(lib, "dxguid.lib")

using Microsoft::WRL::ComPtr;

namespace {
// ---------------------------------------------------------------------------------------------------------------------
// Settings: reframework/data/refw_framegen.txt, "key=value" per line (written with defaults if missing).
struct Settings {
    bool enabled{true};
    uint32_t generated{1};   // frames generated per real frame: 1 = 2x, 2 = 3x, ...
    int status_every_s{5};   // seconds between status lines in the log
    bool dlss{true};         // DLSS super resolution (before the post effects)
    bool override_pd_upscaler{true}; // REFramework pd-upscaler builds: keep their TemporalUpscaler off (startup only)
    int dlss_mode{1};        // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int dlss_preset{0};      // 0 = DLSS default, else the preset letter (dlss_preset=K)
    bool dlss_bloom{false};  // experimental: bloom kept out of DLSS and put back over its output (breaks around lights, see development notes)
    int dlss_debug{0};       // 1 = split screen (left DLSS, right the engine's upscale), 2 = 20x jitter without DLSS output
    bool probe_methods{false}; // diagnostics: log the type database methods about LOD / mip bias
    std::string probe_words{};   // diagnostics, at startup: comma-separated words; type database methods containing them are logged
    bool probe_descriptors{};    // diagnostics, at startup: the descriptor map (which textures a compute pass reads)
    std::string probe_code{};    // diagnostics, at startup: comma-separated Type.Method; their wrapper and engine function code logged
    bool probe_drs{false}; // diagnostics: log the NVIDIA driver profile settings that apply to the game (NVAPI DRS)
    bool dlss_lod_bias{true};          // texture LOD bias for the render scale, set automatically
    float dlss_lod_bias_offset{-1.0f};  // mip levels added to log2(render / output): -1 as DLSS recommends, 0 = output res mips
    int dlss_skip_draws{1};             // the engine's upscale: its first draw into the scene target (the 2nd is its overlay layer)
    float dlss_bloom_alpha{0.1f};       // the bloom stabilizer: this frame's weight (lower = steadier)
    float dlss_bloom_moving{0.0f};      // the bloom stabilizer's moving limit in render pixels (0 = the game profile's)
    bool dlss_bloom_stabilize{true};    // a temporally stabilized scene for the engine's bloom (no bloom flicker from DLSS's jitter)
    float dlss_guard{0.1f};             // upscale guard: DLSS only where the engine's draw matches an upscale (residual below; 0 = off)
    int dlss_lod_bias_key{0x77};        // F8: switches the automatic LOD bias off/on live (0 = no key)
    int dlss_toggle_key{0x76};          // F7: switches DLSS off/on live (the game's own upscale; 0 = no key)
    int fg_inputs_key{0x75};            // F6: frame generation with the real inputs / the stand-ins, live (0 = no key)
    bool dlss_hud_fix{true};             // HUD-less frames for frame generation (no HUD ghosting in generated frames)
    bool dlss_cut_reset{false};          // reset DLSS / DLSS-G history on camera cuts (no ghosting, but a brief frame rate dip)
    bool debug_keys{false};             // the comparison keys below work only with this on (menu: diagnostic hotkeys)
    int hud_fix_key{0x74};               // F5: switches the HUD fix off/on live (0 = no key)
    int reflex{1};                       // NVIDIA Reflex: 0 off, 1 low latency, 2 low latency + boost (frame generation: at least 1)
    int perf_bypass{0};                  // diagnostics, live: 1 = command list hooks off (no DLSS), 2 = no Reflex sleep / latency markers
    bool sl_waitable{true};              // at startup: the swapchain gets the frame latency waitable (Streamline's own limit off, see below)
    int max_frame_latency{2};            // with sl_waitable, live: frames the CPU may run ahead of the GPU (0 = no wait)
    int frame_latency_override{0};       // live: the swapchain's maximum frame latency instead of the game's own (0 = the game's)
    bool perf_waits{false};              // diagnostics, live: what the game's threads wait on per frame (log)
    bool own_waitable{true};             // at startup: the game's frame latency waitable is the plugin's, not Streamline's (frame_pacer)
    int dlss_sharpening_off{1};          // the engine's sharpening while DLSS is on: 0 left alone, 1 off unless another mod does it, 2 off
    int dlss_dither_test{0};             // RE2 [EXPERIMENTAL]: hair/shadow dither fix (0 off, else on: the 4x4 loop; the menu writes 2)
    int dlss_engine_aa{-1};              // experiments, live: -1 = the game's TAA set to none while DLSS is on; else the anti-aliasing type set
    bool dlss_no_upscale_fallback{true}; // diagnostics, live: 0 = the profile's no-upscale fallback off (RE7 videotape segments)
    bool dlss_examine_dlaa{true};        // RE2, menu: DLAA while examining items (a rotating item stays sharper)
    bool dlss_half_limit{true};          // RE7 (profile min_half_render): never below half the output (0: the mode's own scale)
    float dlss_rate_override{0.0f};      // diagnostics: the render scale forced (0 = the mode's)
    int dlss_guard_log{0};               // diagnostics: the upscale guard's log interval (frames; 0 = every 600)
    bool dlss_guard_live{true};          // RE2: the upscale guard's live rule (diagnostics: 0 = off)
    bool dlss_native_path{true};         // RE2: DLSS in the path without the upscale step (RE2 typewriter) from the scene color
    bool dlss_examine_stabilize{true};   // RE2: item examine stabilizer after DLSS (still pixels)
    float dlss_examine_blend{0.1f};      // its weight of the new frame
    bool dlss_examine_no_jitter{true};   // RE2: no jitter while examining items
    bool dlss_compute_input{true};       // RE4: DLSS from the compute upscale's own input
    float dlss_bloom_transfer{1.0f};     // RE4: the bloom from its upscale pass onto DLSS's image (0 = off)
    bool dlss_own_bloom{true};           // RE8: the game's bloom replaced by the plugin's after DLSS (0 = the game's own)
    float dlss_own_bloom_strength{1.0f}; // its strength relative to the game profile's calibration (1 = matched to the game's)
    bool dlss_own_bloom_add{true};
    float dlss_own_bloom_threshold{-1.0f};
    float dlss_own_bloom_inner{-1.0f};   // its inner glow boost (-1 = the game profile's) // its threshold in DLSS's image's units (tuning; -1 = the game's, scaled)      // its mix: scene + bloom * ratio (1) or lerp(scene, bloom, ratio) (0)
    bool dlss_own_motion_blur{true};     // RE7: the game's motion blur replaced by the plugin's after DLSS (0 = the game's own)
    int dlss_tape_scaler{1};             // videotape mode (RE7): DLSS's tape-sized output to the screen: 0 bilinear, 1 Catmull-Rom
    bool dlss_tape_sharpening{true};     // videotape mode: the game's own sharpening kept there (as in vanilla, with its TAA on)
    bool dlss_tape_off{false};           // videotape mode (RE7): DLSS off in those scenes (native render scale, the game's own TAA, as vanilla)
    bool dlss_own_dof{true};             // RE7: the plugin's depth of field after DLSS at the output resolution (the game's switched off)
    bool dlss_cas{true};                 // FidelityFX CAS sharpening on DLSS's output (menu, on by default)
    float dlss_cas_strength{0.5f};       // its strength, 0..1 (as ReShade's CAS "Sharpening"); 0.5 suits DLSS's already sharp output
    float dlss_dof_scale{1.0f};          // its strength (1 = the lens as the game set it)
    bool probe_dof{false};               // diagnostics, live: log the game's depth of field on/off and lens changes (own_dof::probe)
                                         // - both directions made edges shimmer MORE (the point-sampled depth then jumps between texels)
    float dlss_dof_max_scale{1.0f};      // the plugin's depth of field: its largest blur against the game's (1 = the profile's)
    float dlss_dof_temporal{0.15f};      // the depth of field's circle of confusion smoothed over time (reprojected): this frame's weight; 1 = none
} g_settings{};

// The mod's name and version (the menu script has the same; package.sh checks they match the package's).
constexpr const char* MOD_NAME = "Native DLSS + Frame Generation";
constexpr const char* MOD_VERSION = "1.0";

std::filesystem::path g_game_dir{};
std::filesystem::path g_data_dir{};
// NVIDIA's files (Streamline, DLSS): reframework/data/Native DLSS + Frame Generation/ (the release's layout: mod managers
// such as Fluffy Mod Manager install into reframework/), else next to the game's executable (older installs).
std::filesystem::path g_nvidia_dir{};
constexpr const wchar_t* NVIDIA_SUBDIR = L"Native DLSS + Frame Generation";

std::filesystem::path find_nvidia_dir() {
    std::error_code ec;
    const auto own = g_data_dir / NVIDIA_SUBDIR;
    return std::filesystem::exists(own / L"sl.interposer.dll", ec) ? own : g_game_dir;
}

// Read at startup and again whenever the file changes (the in-game menu, refw_framegen.lua, writes it): most settings
// apply live. Lines without "=" (comments) are ignored.
std::filesystem::file_time_type g_settings_time{};
bool g_fg_hooked{}; // frame generation's hooks were installed at startup (it was on then)
std::string g_game{}; // REFramework's game name (RE2, RE4, ...)
uint32_t g_nv_sr_override{}; // NVIDIA's forced DLSS-SR preset (0 none, 1.. = A.., 0xffffff = latest), read at startup
std::string g_error{}; // why the plugin can't work (shown by the menu and the overlay); empty if fine
std::string g_sharpening_mod{}; // another mod removing the engine's sharpening (then the plugin leaves it to that), "" if none
std::string g_sharpening_mod_kind{}; // "plugin" or "script"

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// Another mod that removes the engine's sharpening: a loaded DLL or a REFramework autorun script with "sharp" in its name
// (RE4.Sharpness.dll, RE2_DisableSharpeningAndTAA.lua). RE4.Sharpness with bDisableSharpness = false in its INI doesn't count.
std::string find_sharpening_mod() {
    HMODULE modules[1024]{};
    DWORD needed{};

    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        for (DWORD i = 0; i < std::min<DWORD>(needed / sizeof(HMODULE), 1024); ++i) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(modules[i], path, MAX_PATH);
            const auto file = std::filesystem::path(path);
            const auto name = lower(file.filename().string());

            if (name.find("sharp") == std::string::npos) {
                continue;
            }

            // Its INI next to the game (RE4.Sharpness.ini): "bDisableSharpness = false" means it isn't doing it.
            std::ifstream ini{g_game_dir / file.filename().replace_extension(".ini")};
            std::string line{};
            bool off = false;

            while (std::getline(ini, line)) {
                const auto l = lower(line);
                off = off || (l.find("bdisablesharpness") != std::string::npos && l.find("false") != std::string::npos);
            }

            if (!off) {
                g_sharpening_mod_kind = "plugin";
                return file.filename().string();
            }
        }
    }

    // REFramework autorun scripts that change the tone mapping sharpening (get_Sharpness / SharpnessSetting), by what they
    // do rather than their name: stubs that return at once ("if true then return end", e.g. the deprecated
    // re2_sharpness_removal.lua REFramework leaves behind) don't count.
    std::error_code error{};

    for (const auto& entry : std::filesystem::directory_iterator(g_game_dir / L"reframework" / L"autorun", error)) {
        if (entry.path().extension() != ".lua" || lower(entry.path().filename().string()) == "refw_framegen.lua") {
            continue;
        }

        std::ifstream file{entry.path()};
        const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
        const auto first_code = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");

        if (first_code != std::string::npos && text.compare(first_code, 23, "if true then return end") == 0) {
            continue;
        }

        // Its saved settings (reframework/data/<script>.json, as RE2_DisableSharpeningAndTAA.lua keeps them): switched off
        // in its menu ("b_enabled": false / "enabled": false) it doesn't do anything.
        {
            std::ifstream json{g_game_dir / L"reframework" / L"data" / (entry.path().filename().wstring() + L".json")};
            std::string saved{std::istreambuf_iterator<char>{json}, std::istreambuf_iterator<char>{}};
            saved.erase(std::remove_if(saved.begin(), saved.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }),
                saved.end());

            if (saved.find("\"b_enabled\":false") != std::string::npos || saved.find("\"enabled\":false") != std::string::npos) {
                continue;
            }
        }

        if (text.find("get_Sharpness") != std::string::npos || text.find("SharpnessSetting") != std::string::npos) {
            g_sharpening_mod_kind = "script";
            return entry.path().filename().string();
        }
    }

    return {};
}

// Written at once (the regular status file needs the game's frames going through the plugin, which may not happen).
void report_error(const std::string& error) {
    g_error = error;
    std::ofstream out{g_data_dir / L"refw_framegen_status.txt", std::ios::trunc};
    out << "error=" << error << '\n';
}

void load_settings() {
    const auto path = g_data_dir / L"refw_framegen.txt";

    if (!std::filesystem::exists(path)) {
        std::ofstream out{path};
        out << "# Native DLSS + Frame Generation settings. The in-game menu (REFramework menu > Script Generated UI) writes this file; it can\n"
            << "# also be edited by hand while the game runs (changes apply within a second).\n"
            << "#\n"
            << "# Frame generation (DLSS-G): on/off\n"
            << "enabled=1\n"
            << "# Generated frames per rendered frame: 1 = 2x, 2 = 3x ... 5 = 6x (stepped down to what the GPU/driver allows)\n"
            << "generated=1\n"
            << "# DLSS super resolution: on/off\n"
            << "dlss=1\n"
            << "# 0 = DLAA, 1 = Quality, 2 = Balanced, 3 = Performance, 4 = Ultra Performance\n"
            << "dlss_mode=1\n"
            << "# 0 = DLSS default, or a preset letter (e.g. K). A preset forced in the NVIDIA app wins over this.\n"
            << "dlss_preset=0\n"
            << "# Texture LOD bias for the render resolution, automatic: on/off; offset in mip levels added to log2(render/output)\n"
            << "dlss_lod_bias=1\n"
            << "dlss_lod_bias_offset=-1\n"
            << "# HUD-less frames for frame generation (keeps the HUD clean in generated frames)\n"
            << "dlss_hud_fix=1\n"
            << "# Reset DLSS / frame generation history on camera cuts: no ghosting at cuts, but a brief frame rate dip\n"
            << "dlss_cut_reset=0\n"
            << "# The engine's own sharpening while DLSS is on (it oversharpens DLSS's output): 0 = left alone, 1 = off unless\n"
            << "# another mod already removes it (e.g. RE2_DisableSharpeningAndTAA.lua, RE4.Sharpness), 2 = always off\n"
            << "dlss_sharpening_off=1\n"
            << "# On-screen overlay; corner 0 = top left, 1 = top right, 2 = bottom left, 3 = bottom right\n"
            << "overlay=0\n"
            << "overlay_corner=0\n"
            << "# Diagnostic hotkeys for comparisons, off by default (menu: Advanced > Diagnostic hotkeys). Keys (virtual key\n"
            << "# codes, 0 = none): F5 HUD fix, F6 frame generation inputs, F7 DLSS, F8 LOD bias\n"
            << "debug_keys=0\n"
            << "hud_fix_key=116\n"
            << "fg_inputs_key=117\n"
            << "dlss_toggle_key=118\n"
            << "dlss_lod_bias_key=119\n";
    }

    std::error_code error{};
    g_settings_time = std::filesystem::last_write_time(path, error);

    Settings s{}; // defaults, then the file
    std::ifstream in{path};
    std::string line{};

    while (std::getline(in, line)) {
        const auto eq = line.find('=');

        if (eq == std::string::npos) {
            continue;
        }

        const auto key = line.substr(0, eq);
        const auto text = line.substr(eq + 1);
        int value = 0;

        if (key == "probe_words") {
            s.probe_words = text;
            continue;
        }

        if (key == "probe_code") {
            s.probe_code = text;
            continue;
        }

        if (key == "dlss_preset" && text.size() == 1 && std::isalpha((unsigned char)text[0])) {
            value = std::toupper((unsigned char)text[0]) - 'A' + 1; // Streamline numbers presets A=1 ... K=11
        } else {
            try {
                value = std::stoi(text);
            } catch (...) {
                continue;
            }
        }

        if (key == "enabled") s.enabled = value != 0;
        else if (key == "generated") s.generated = (uint32_t)std::clamp(value, 1, 5);
        else if (key == "status_every_s") s.status_every_s = std::max(1, value);
        else if (key == "dlss") s.dlss = value != 0;
        else if (key == "override_pd_upscaler") s.override_pd_upscaler = value != 0;
        else if (key == "dlss_mode") s.dlss_mode = std::clamp(value, 0, 4);
        else if (key == "dlss_preset") s.dlss_preset = std::clamp(value, 0, 26);
        else if (key == "dlss_bloom") s.dlss_bloom = value != 0;
        else if (key == "dlss_cut_reset") s.dlss_cut_reset = value != 0;
        else if (key == "dlss_hud_fix") s.dlss_hud_fix = value != 0;
        else if (key == "debug_keys") s.debug_keys = value != 0;
        else if (key == "hud_fix_key") s.hud_fix_key = std::clamp(value, 0, 255);
        else if (key == "fg_inputs_key") s.fg_inputs_key = std::clamp(value, 0, 255);
        else if (key == "dlss_toggle_key") s.dlss_toggle_key = std::clamp(value, 0, 255);
        else if (key == "dlss_lod_bias_key") s.dlss_lod_bias_key = std::clamp(value, 0, 255);
        else if (key == "dlss_skip_draws") s.dlss_skip_draws = std::clamp(value, 1, 8);
        else if (key == "dlss_bloom_stabilize") s.dlss_bloom_stabilize = value != 0;
        else if (key == "dlss_bloom_alpha") s.dlss_bloom_alpha = std::clamp(std::stof(text), 0.01f, 1.0f);
        else if (key == "dlss_bloom_moving") s.dlss_bloom_moving = std::clamp(std::stof(text), 0.0f, 1000.0f);
        else if (key == "dlss_guard") s.dlss_guard = std::clamp(std::stof(text), 0.0f, 1.0f);
        else if (key == "dlss_lod_bias") s.dlss_lod_bias = value != 0;
        else if (key == "dlss_lod_bias_offset") s.dlss_lod_bias_offset = std::clamp(std::stof(text), -3.0f, 3.0f);
        else if (key == "probe_drs") s.probe_drs = value != 0;
        else if (key == "probe_methods") s.probe_methods = value != 0;
        else if (key == "dlss_debug") s.dlss_debug = std::clamp(value, 0, 20);
        else if (key == "reflex") s.reflex = std::clamp(value, 0, 2);
        else if (key == "perf_bypass") s.perf_bypass = std::clamp(value, 0, 3);
        else if (key == "sl_waitable") s.sl_waitable = value != 0;
        else if (key == "max_frame_latency") s.max_frame_latency = std::clamp(value, 0, 16);
        else if (key == "frame_latency_override") s.frame_latency_override = std::clamp(value, 0, 16);
        else if (key == "perf_waits") s.perf_waits = value != 0;
        else if (key == "own_waitable") s.own_waitable = value != 0;
        else if (key == "dlss_sharpening_off") s.dlss_sharpening_off = std::clamp(value, 0, 2);
        else if (key == "dlss_dither_test") s.dlss_dither_test = value != 0 ? 2 : 0;
        else if (key == "dlss_engine_aa") s.dlss_engine_aa = std::clamp(value, -1, 16);
        else if (key == "dlss_no_upscale_fallback") s.dlss_no_upscale_fallback = value != 0;
        else if (key == "dlss_examine_dlaa") s.dlss_examine_dlaa = value != 0;
        else if (key == "dlss_half_limit") s.dlss_half_limit = value != 0;
        else if (key == "dlss_rate_override") s.dlss_rate_override = std::clamp(std::stof(text), 0.0f, 1.0f);
        else if (key == "dlss_guard_log") s.dlss_guard_log = std::clamp(value, 0, 100000);
        else if (key == "dlss_guard_live") s.dlss_guard_live = value != 0;
        else if (key == "dlss_native_path") s.dlss_native_path = value != 0;
        else if (key == "dlss_examine_stabilize") s.dlss_examine_stabilize = value != 0;
        else if (key == "dlss_examine_blend") s.dlss_examine_blend = std::clamp(std::stof(text), 0.02f, 1.0f);
        else if (key == "dlss_examine_no_jitter") s.dlss_examine_no_jitter = value != 0;
        else if (key == "dlss_compute_input") s.dlss_compute_input = value != 0;
        else if (key == "dlss_bloom_transfer") s.dlss_bloom_transfer = std::clamp(std::stof(text), 0.0f, 4.0f);
        else if (key == "dlss_own_bloom") s.dlss_own_bloom = value != 0;
        else if (key == "dlss_own_bloom_strength") s.dlss_own_bloom_strength = std::clamp(std::stof(text), 0.0f, 20.0f);
        else if (key == "dlss_own_bloom_inner") s.dlss_own_bloom_inner = std::clamp(std::stof(text), -1.0f, 50.0f);
        else if (key == "dlss_own_bloom_threshold") s.dlss_own_bloom_threshold = std::clamp(std::stof(text), -1.0f, 1000.0f);
        else if (key == "dlss_own_bloom_add") s.dlss_own_bloom_add = value != 0;
        else if (key == "dlss_own_motion_blur") s.dlss_own_motion_blur = value != 0;
        else if (key == "dlss_tape_scaler") s.dlss_tape_scaler = std::clamp(value, 0, 1);
        else if (key == "dlss_tape_sharpening") s.dlss_tape_sharpening = value != 0;
        else if (key == "dlss_tape_off") s.dlss_tape_off = value != 0;
        else if (key == "dlss_own_dof") s.dlss_own_dof = value != 0;
        else if (key == "dlss_cas") s.dlss_cas = value != 0;
        else if (key == "dlss_cas_strength") s.dlss_cas_strength = std::clamp(std::stof(text), 0.0f, 1.0f);
        else if (key == "dlss_dof_scale") s.dlss_dof_scale = std::clamp(std::stof(text), 0.0f, 8.0f);
        else if (key == "probe_dof") s.probe_dof = value != 0;
        else if (key == "probe_descriptors") s.probe_descriptors = value != 0;
        else if (key == "dlss_dof_max_scale") s.dlss_dof_max_scale = std::clamp(std::stof(text), 0.1f, 10.0f);
        else if (key == "dlss_dof_temporal") s.dlss_dof_temporal = std::clamp(std::stof(text), 0.01f, 1.0f);
        else if (key == "probe_image_quality") pipeline_probe::configure(std::clamp(value, 0, 200));
    }

    g_settings = s;
}

void reload_settings_if_changed() {
    static auto last_check = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();

    if (now - last_check < std::chrono::seconds{1}) {
        return;
    }

    last_check = now;
    std::error_code error{};
    const auto time = std::filesystem::last_write_time(g_data_dir / L"refw_framegen.txt", error);

    if (!error && time != g_settings_time) {
        const auto before = g_settings;
        load_settings();
        log_info("Settings reloaded: frame generation {} ({}x), DLSS {} (mode {}, preset {}), LOD bias {} ({:+.2f}), HUD fix {}",
            g_settings.enabled, g_settings.generated + 1, g_settings.dlss, g_settings.dlss_mode, g_settings.dlss_preset, g_settings.dlss_lod_bias,
            g_settings.dlss_lod_bias_offset, g_settings.dlss_hud_fix);

        if (g_settings.enabled && !before.enabled && !g_fg_hooked) {
            log_warn("Frame generation is unavailable: the game's DXGI imports could not be hooked");
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Streamline, loaded from the sl.interposer.dll in g_nvidia_dir (only if NVIDIA-signed).
struct Streamline {
    HMODULE module{};

    PFun_slInit* init{};
    PFun_slSetD3DDevice* set_d3d_device{};
    PFun_slIsFeatureSupported* is_feature_supported{};
    PFun_slUpgradeInterface* upgrade_interface{};
    PFun_slGetFeatureFunction* get_feature_function{};
    PFun_slGetNewFrameToken* get_new_frame_token{};
    PFun_slSetConstants* set_constants{};
    PFun_slSetTagForFrame* set_tag_for_frame{};
    PFun_slEvaluateFeature* evaluate_feature{};

    PFun_slDLSSGSetOptions* dlssg_set_options{};
    PFun_slDLSSGGetState* dlssg_get_state{};
    PFun_slReflexSetOptions* reflex_set_options{};
    PFun_slReflexSleep* reflex_sleep{};
    PFun_slPCLSetMarker* pcl_set_marker{};
    PFun_slDLSSGetOptimalSettings* dlss_get_optimal_settings{};
    PFun_slDLSSSetOptions* dlss_set_options{};
    bool dlss_supported{};

    bool initialized{};    // both stages done
    bool failed{};         // stage 1 (loading) failed
    bool device_failed{};  // stage 2 (device) failed
} g_sl{};

std::mutex g_init_mutex{};
const sl::ViewportHandle g_viewport{0};
std::atomic<uint32_t> g_frame_index{0};

void sl_log(sl::LogType type, const char* msg) {
    std::string_view text{msg};

    while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
        text.remove_suffix(1);
    }

    if (type == sl::LogType::eError) {
        log_error("[Streamline] {}", text);
    } else if (type == sl::LogType::eWarn) {
        log_warn("[Streamline] {}", text);
    } else {
        log_info("[Streamline] {}", text);
    }
}

template <typename T>
bool load_feature_function(sl::Feature feature, const char* name, T*& out) {
    void* fn{};

    if (g_sl.get_feature_function(feature, name, fn) != sl::Result::eOk || fn == nullptr) {
        log_error("Missing Streamline function {}", name);
        return false;
    }

    out = (T*)fn;
    return true;
}

sl::FrameToken* new_frame_token() {
    sl::FrameToken* token{};
    const uint32_t index = ++g_frame_index;
    return g_sl.get_new_frame_token(token, &index) == sl::Result::eOk ? token : nullptr;
}

void pcl_marker(sl::PCLMarker marker, sl::FrameToken* token) {
    if (token != nullptr && g_sl.pcl_set_marker != nullptr && (g_settings.perf_bypass & 2) == 0) {
        g_sl.pcl_set_marker(marker, *token);
    }
}

ComPtr<ID3D12Device> g_device{};
ID3D12CommandQueue* g_queue{};

// Streamline's DLSS-G module and its linkSwapchainToCmdQueue, which we keep other hooks out of while creating our
// swapchain (see unhook_guard.hpp).
HMODULE g_dlssg_module{};
void* g_link_fn{};

// Stage 1, when the game creates its DXGI factory (well before its device and swapchain): load and initialize
// Streamline. This is the slow part (seconds: NVIDIA's driver/update checks), so that the game's swapchain creation
// later stays as quick as without the plugin. Doing it there instead let REFramework install its D3D12 hooks in the
// middle of the game's swapchain creation, and its Streamline hook then deadlocked waiting for REFramework's startup.
bool init_streamline() {
    std::scoped_lock _{g_init_mutex};

    if (g_sl.module != nullptr || g_sl.failed) {
        return g_sl.module != nullptr;
    }

    g_sl.failed = true; // until everything below succeeds
    const auto start = std::chrono::steady_clock::now();
    const auto interposer = g_nvidia_dir / L"sl.interposer.dll";

    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        log_error("{} is missing or not signed by NVIDIA, not loading it", interposer.string());
        report_error("sl.interposer.dll is missing or not signed by NVIDIA (reframework/data/Native DLSS + Frame Generation: see README)");
        return false;
    }

    // From its own folder: its dependencies resolve there too.
    g_sl.module = LoadLibraryExW(interposer.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (g_sl.module == nullptr) {
        log_error("Failed to load {}", interposer.string());
        return false;
    }

    const auto get = [](const char* name) { return GetProcAddress(g_sl.module, name); };
    g_sl.init = (PFun_slInit*)get("slInit");
    g_sl.set_d3d_device = (PFun_slSetD3DDevice*)get("slSetD3DDevice");
    g_sl.is_feature_supported = (PFun_slIsFeatureSupported*)get("slIsFeatureSupported");
    g_sl.upgrade_interface = (PFun_slUpgradeInterface*)get("slUpgradeInterface");
    g_sl.get_feature_function = (PFun_slGetFeatureFunction*)get("slGetFeatureFunction");
    g_sl.get_new_frame_token = (PFun_slGetNewFrameToken*)get("slGetNewFrameToken");
    g_sl.set_constants = (PFun_slSetConstants*)get("slSetConstants");
    g_sl.set_tag_for_frame = (PFun_slSetTagForFrame*)get("slSetTagForFrame");
    g_sl.evaluate_feature = (PFun_slEvaluateFeature*)get("slEvaluateFeature");

    if (g_sl.init == nullptr || g_sl.set_d3d_device == nullptr || g_sl.is_feature_supported == nullptr || g_sl.upgrade_interface == nullptr ||
        g_sl.get_feature_function == nullptr || g_sl.get_new_frame_token == nullptr || g_sl.set_constants == nullptr ||
        g_sl.set_tag_for_frame == nullptr || g_sl.evaluate_feature == nullptr)
    {
        log_error("sl.interposer.dll is missing exports");
        return false;
    }

    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL, sl::kFeatureDLSS};
    static const std::wstring log_dir = g_data_dir.wstring();
    // Streamline's plugins (sl.dlss.dll, sl.dlss_g.dll, ...) and NVIDIA's DLSS DLLs (NGX gets the same paths) from there.
    static const std::wstring plugin_dir = g_nvidia_dir.wstring();
    static const wchar_t* plugin_paths[] = {plugin_dir.c_str()};

    sl::Preferences pref{};
    pref.showConsole = false;
    pref.logLevel = sl::LogLevel::eDefault;
    pref.pathToLogsAndData = log_dir.c_str();
    pref.pathsToPlugins = plugin_paths;
    pref.numPathsToPlugins = 1;
    pref.logMessageCallback = &sl_log;
    // Manual hooking: only the swapchain we create through Streamline goes through it; REFramework, ReShade and
    // OptiScaler keep seeing native interfaces.
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging | sl::PreferenceFlags::eUseDXGIFactoryProxy;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = (uint32_t)std::size(features);
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "RE Engine";
    pref.projectId = "5b0d7a5e-2f61-4c3a-9a6e-7e2c1d4f9b31";
    pref.renderAPI = sl::RenderAPI::eD3D12;

    if (const auto result = g_sl.init(pref, sl::kSDKVersion); result != sl::Result::eOk) {
        log_error("slInit failed: {}", (uint32_t)result);
        FreeLibrary(g_sl.module);
        g_sl.module = nullptr;
        return false;
    }

    g_dlssg_module = GetModuleHandleW(L"sl.dlss_g.dll");
    g_link_fn = find_link_swapchain_to_cmd_queue(g_dlssg_module);

    if (g_link_fn == nullptr) {
        log_warn("linkSwapchainToCmdQueue not found in sl.dlss_g.dll: can't guard our swapchain creation against hooks on it");
    }

    g_sl.failed = false;
    log_info("Streamline loaded and initialized in {} ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    return true;
}

// Reflex low latency keeps the GPU's queue short: lower latency, but the CPU then waits for the GPU each frame, so where
// the game is CPU-bound every bit of GPU work (DLSS) costs frame rate. Frame generation requires it; otherwise a setting.
bool apply_reflex() {
    static int applied = -1;
    const auto wanted = g_settings.enabled ? std::max(1, g_settings.reflex) : g_settings.reflex;

    if (wanted == applied || g_sl.reflex_set_options == nullptr) {
        return true;
    }

    sl::ReflexOptions reflex{};
    reflex.mode = wanted == 0 ? sl::ReflexMode::eOff : wanted == 1 ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eLowLatencyWithBoost;

    if (const auto result = g_sl.reflex_set_options(reflex); result != sl::Result::eOk) {
        log_error("slReflexSetOptions failed: {}", (uint32_t)result);
        return false;
    }

    applied = wanted;
    log_info("Reflex: {}", wanted == 0 ? "off" : wanted == 1 ? "low latency" : "low latency + boost");
    return true;
}

// Stage 2, when the game creates its swapchain: the device-specific part (quick).
bool attach_streamline(IUnknown* queue_unk) {
    std::scoped_lock _{g_init_mutex};

    if (g_sl.initialized) {
        return true;
    }

    if (g_sl.module == nullptr || g_sl.device_failed) {
        return false;
    }

    g_sl.device_failed = true; // until everything below succeeds

    ComPtr<ID3D12CommandQueue> queue{};
    ComPtr<ID3D12Device> device{};

    if (queue_unk == nullptr || FAILED(queue_unk->QueryInterface(IID_PPV_ARGS(&queue))) || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
        log_error("The swapchain is not being created with a D3D12 command queue");
        return false;
    }

    if (const auto result = g_sl.set_d3d_device(device.Get()); result != sl::Result::eOk) {
        log_error("slSetD3DDevice failed: {}", (uint32_t)result);
        return false;
    }

    auto luid = device->GetAdapterLuid();
    sl::AdapterInfo adapter{};
    adapter.deviceLUID = (uint8_t*)&luid;
    adapter.deviceLUIDSizeInBytes = sizeof(luid);

    if (const auto result = g_sl.is_feature_supported(sl::kFeatureDLSS_G, adapter); result != sl::Result::eOk) {
        log_error("DLSS Frame Generation is not supported on this GPU/driver/OS: {}", (uint32_t)result);
        report_error(std::format("DLSS Frame Generation is not supported on this GPU / driver / Windows (Streamline result {})", (uint32_t)result));
        return false;
    }

    if (!load_feature_function(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_sl.dlssg_set_options) ||
        !load_feature_function(sl::kFeatureDLSS_G, "slDLSSGGetState", g_sl.dlssg_get_state) ||
        !load_feature_function(sl::kFeatureReflex, "slReflexSetOptions", g_sl.reflex_set_options) ||
        !load_feature_function(sl::kFeatureReflex, "slReflexSleep", g_sl.reflex_sleep) ||
        !load_feature_function(sl::kFeaturePCL, "slPCLSetMarker", g_sl.pcl_set_marker))
    {
        return false;
    }

    // DLSS-G requires Reflex (the mode follows the settings: apply_reflex()).
    if (!apply_reflex()) {
        return false;
    }

    g_device = device;
    queue->AddRef();
    g_queue = queue.Get();

    // DLSS super resolution: optional (frame generation works without it). Loaded even when off, so the menu can switch it on.
    {
        g_sl.dlss_supported = g_sl.is_feature_supported(sl::kFeatureDLSS, adapter) == sl::Result::eOk &&
            load_feature_function(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", g_sl.dlss_get_optimal_settings) &&
            load_feature_function(sl::kFeatureDLSS, "slDLSSSetOptions", g_sl.dlss_set_options);
        log_info("DLSS super resolution {}", g_sl.dlss_supported ? "supported" : "NOT supported on this GPU/driver (or nvngx_dlss.dll missing)");

        if (g_sl.dlss_supported) {
            if (g_settings.probe_descriptors) {
                descriptor_map::install(device.Get()); // diagnostics: before the plugin's own hooks on the same device
            }

            dlss_pass::on_device(device.Get());
        }
    }

    g_sl.initialized = true;
    g_sl.device_failed = false;
    log_info("Streamline attached to the game's device, DLSS-G supported");
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Hooks on the game's side only.
std::unique_ptr<PointerHook> g_factory1_hook{};
std::unique_ptr<PointerHook> g_factory2_hook{};
std::mutex g_hooks_mutex{};
std::unordered_map<void*, InstanceVtableHook*> g_factory_hooks{};
InstanceVtableHook* g_swapchain_hook{};
std::atomic<IDXGISwapChain3*> g_proxy_swapchain{};
thread_local bool t_inside_streamline{false}; // Streamline calls back into the native factory: don't re-route that

constexpr size_t FACTORY_VTABLE_ENTRIES = 30;   // IDXGIFactory7 has 29 methods
constexpr size_t SWAPCHAIN_VTABLE_ENTRIES = 40; // IDXGISwapChain4 has 39

InstanceVtableHook* factory_hook(void* factory) {
    std::scoped_lock _{g_hooks_mutex};
    const auto it = g_factory_hooks.find(factory);
    return it != g_factory_hooks.end() ? it->second : nullptr;
}

HRESULT WINAPI present_hook(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags);
HRESULT WINAPI present1_hook(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* params);
HRESULT WINAPI resize_buffers_hook(IDXGISwapChain3* swapchain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags);

// Frame pacing through Streamline's swapchain proxy (see frame_pacer.hpp). The waitable a game gets from the proxy is
// Streamline's: signaled only after Streamline's copy and present of the frame before, even with frame generation off
// (RE4, which waits on it with a maximum frame latency of 1: 78 fps at native against 99 with a normal swapchain). The
// plugin's own waitable replaces it (setting own_waitable): released when each frame's GPU work is done, as a normal
// swapchain's is. Games that don't ask for a waitable get the flag added (setting sl_waitable: without it Streamline keeps
// the waitable and paces with it itself) and the plugin waits on its own waitable at the start of each frame.
bool g_waitable_added{};           // the plugin added the flag (kept on ResizeBuffers) and waits itself
bool g_own_waitable{};             // the plugin's waitable is in use (decided when the swapchain is created)
std::atomic<int> g_game_latency{0};
std::atomic<int> g_applied_latency{-1};

template <typename Desc>
Desc* with_waitable(Desc& desc) {
    g_waitable_added = false;
    g_own_waitable = g_settings.own_waitable;

    if (!g_settings.sl_waitable || (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
        return &desc; // off, or the game asked for it itself (then it waits on it)
    }

    desc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    g_waitable_added = true;
    return &desc;
}

HRESULT WINAPI set_max_latency_hook(IDXGISwapChain2* swapchain, UINT latency);

// The latency in effect: the override setting, else the game's own (or, where the plugin added the waitable, its setting).
int wanted_latency() {
    if (g_settings.frame_latency_override > 0) return g_settings.frame_latency_override;
    if (g_waitable_added) return std::max(1, g_settings.max_frame_latency);
    return std::max(1, g_game_latency.load());
}

void apply_latency(IDXGISwapChain3* swapchain) {
    const auto latency = wanted_latency();

    if (swapchain == nullptr || g_swapchain_hook == nullptr || latency == g_applied_latency.exchange(latency)) {
        return;
    }

    if (g_own_waitable) {
        frame_pacer::set_max_latency(latency);
    } else {
        g_swapchain_hook->original<decltype(&set_max_latency_hook)>(31)((IDXGISwapChain2*)swapchain, (UINT)latency);
        log_info("Frame latency: {} (Streamline's waitable)", latency);
    }
}

HRESULT WINAPI set_max_latency_hook(IDXGISwapChain2* swapchain, UINT latency) {
    const auto original = g_swapchain_hook->original<decltype(&set_max_latency_hook)>(31);
    g_game_latency = (int)latency;

    if (static int logged = 0; logged < 10) {
        ++logged;
        log_info("The game sets the maximum frame latency: {}", latency);
    }

    g_applied_latency = -1; // applied (with the override, if any) at the next frame start
    return original(swapchain, latency);
}

// The waitable the game takes from the swapchain: the plugin's (own_waitable), else Streamline's.
HANDLE WINAPI get_waitable_hook(IDXGISwapChain2* swapchain) {
    const auto original = g_swapchain_hook->original<decltype(&get_waitable_hook)>(33);
    const auto streamline_handle = original(swapchain);
    const auto handle = g_own_waitable && frame_pacer::ready() ? frame_pacer::waitable() : streamline_handle;
    wait_probe::set_waitable(handle);

    if (static bool logged = false; !logged) {
        logged = true;
        log_info("The game took the frame latency waitable: {}", handle == streamline_handle ? "Streamline's" : "the plugin's");
    }

    return handle;
}

// Start of a frame: the latency applied (live); where the plugin added the waitable, its wait.
void pace_frame_start(IDXGISwapChain3* swapchain) {
    apply_latency(swapchain);

    if (!g_waitable_added || swapchain == nullptr || g_settings.max_frame_latency == 0) {
        return;
    }

    if (g_own_waitable && frame_pacer::ready()) {
        WaitForSingleObjectEx(frame_pacer::waitable(), 1000, TRUE);
    } else if (static HANDLE streamline_handle = ((IDXGISwapChain2*)swapchain)->GetFrameLatencyWaitableObject(); streamline_handle != nullptr) {
        WaitForSingleObjectEx(streamline_handle, 1000, TRUE);
    }
}

void hook_proxy_swapchain(IDXGISwapChain1* swapchain1) {
    ComPtr<IDXGISwapChain3> swapchain{};

    if (FAILED(swapchain1->QueryInterface(IID_PPV_ARGS(&swapchain)))) {
        log_error("The Streamline swapchain has no IDXGISwapChain3");
        return;
    }

    // The plugin's waitable (own_waitable): its fence goes on the game's presenting queue.
    if (g_own_waitable && !frame_pacer::init(g_device.Get(), g_queue)) {
        g_own_waitable = false;
    }

    log_info("Swapchain frame latency waitable: {} flag, {} waitable", g_waitable_added ? "the plugin's" : "the game's", g_own_waitable ? "the plugin's" : "Streamline's");

    std::scoped_lock _{g_hooks_mutex};
    auto hook = new InstanceVtableHook(swapchain.Get(), SWAPCHAIN_VTABLE_ENTRIES);
    hook->hook(8, (void*)&present_hook);
    hook->hook(13, (void*)&resize_buffers_hook);
    hook->hook(22, (void*)&present1_hook);
    hook->hook(31, (void*)&set_max_latency_hook); // IDXGISwapChain2::SetMaximumFrameLatency
    hook->hook(33, (void*)&get_waitable_hook);    // IDXGISwapChain2::GetFrameLatencyWaitableObject
    g_swapchain_hook = hook;
    g_proxy_swapchain = swapchain.Get();

    log_info("Hooked the game's Streamline swapchain @ {:x}", (uintptr_t)swapchain.Get());
}

IDXGIFactory2* make_proxy_factory(IDXGIFactory* factory) {
    ComPtr<IDXGIFactory2> base{};

    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&base)))) {
        return nullptr;
    }

    IDXGIFactory2* proxy = base.Detach();

    if (const auto result = g_sl.upgrade_interface((void**)&proxy); result != sl::Result::eOk) {
        log_error("slUpgradeInterface(factory) failed: {}", (uint32_t)result);
        proxy->Release();
        return nullptr;
    }

    return proxy;
}

// The factory class's own CreateSwapChain(ForHwnd) (MinHook on the functions): for a factory the game created before the
// plugin loaded (RE4), which the per-instance hooks never saw. Original functions here.
void* g_class_create_swapchain{};
void* g_class_create_swapchain_for_hwnd{};

bool from_game(void* address);

// For the class hooks: a swapchain call that may be the game's (directly, or through another mod in front of DXGI such
// as OptiScaler): not REFramework's (its dummy swapchain), not Streamline's, not the plugin's own.
bool maybe_game_call(void* address) {
    if (from_game(address)) {
        return true;
    }

    HMODULE module{};
    wchar_t path[MAX_PATH]{};

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module)) {
        return false;
    }

    GetModuleFileNameW(module, path, MAX_PATH);
    auto name = std::filesystem::path(path).filename().wstring();
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    return name != L"dinput8.dll" && name != L"refw_framegen.dll" && !name.starts_with(L"sl.");
}

HRESULT WINAPI create_swapchain_for_hwnd_hook(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, IDXGIOutput* output, IDXGISwapChain1** swapchain)
{
    const auto instance = factory_hook(factory);
    const auto original = instance != nullptr ? instance->original<decltype(&create_swapchain_for_hwnd_hook)>(15)
                                              : (decltype(&create_swapchain_for_hwnd_hook))g_class_create_swapchain_for_hwnd;

    if (t_inside_streamline || g_proxy_swapchain != nullptr || !attach_streamline(device)) {
        return original(factory, device, hwnd, desc, fullscreen_desc, output, swapchain);
    }

    auto proxy = make_proxy_factory(factory);

    if (proxy == nullptr) {
        return original(factory, device, hwnd, desc, fullscreen_desc, output, swapchain);
    }

    log_info("Creating the game's swapchain ({}x{}, format {}) through Streamline", desc != nullptr ? desc->Width : 0,
        desc != nullptr ? desc->Height : 0, desc != nullptr ? (uint32_t)desc->Format : 0);

    t_inside_streamline = true;
    HRESULT result{};
    {
        TemporaryUnhook guard{g_link_fn, g_dlssg_module};
        log_info("linkSwapchainToCmdQueue {}", guard.was_hooked() ? "was hooked by something: bypassed for this call" : "not hooked");
        auto own_desc = desc != nullptr ? *desc : DXGI_SWAP_CHAIN_DESC1{};
        result = proxy->CreateSwapChainForHwnd(device, hwnd, desc != nullptr ? with_waitable(own_desc) : desc, fullscreen_desc, output, swapchain);
    }
    t_inside_streamline = false;

    if (SUCCEEDED(result) && swapchain != nullptr && *swapchain != nullptr) {
        hook_proxy_swapchain(*swapchain);
    } else {
        log_error("Streamline CreateSwapChainForHwnd failed: {:x}", (uint32_t)result);
    }

    proxy->Release(); // the proxy swapchain keeps what it needs alive
    return result;
}

HRESULT WINAPI create_swapchain_hook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain) {
    const auto instance = factory_hook(factory);
    const auto original = instance != nullptr ? instance->original<decltype(&create_swapchain_hook)>(10)
                                              : (decltype(&create_swapchain_hook))g_class_create_swapchain;

    if (t_inside_streamline || g_proxy_swapchain != nullptr || !attach_streamline(device)) {
        return original(factory, device, desc, swapchain);
    }

    auto proxy = make_proxy_factory(factory);

    if (proxy == nullptr) {
        return original(factory, device, desc, swapchain);
    }

    log_info("Creating the game's swapchain (CreateSwapChain) through Streamline");

    t_inside_streamline = true;
    HRESULT result{};
    {
        TemporaryUnhook guard{g_link_fn, g_dlssg_module};
        log_info("linkSwapchainToCmdQueue {}", guard.was_hooked() ? "was hooked by something: bypassed for this call" : "not hooked");
        auto own_desc = desc != nullptr ? *desc : DXGI_SWAP_CHAIN_DESC{};
        result = proxy->CreateSwapChain(device, desc != nullptr ? with_waitable(own_desc) : desc, swapchain);
    }
    t_inside_streamline = false;

    if (SUCCEEDED(result) && swapchain != nullptr && *swapchain != nullptr) {
        ComPtr<IDXGISwapChain1> sc1{};

        if (SUCCEEDED((*swapchain)->QueryInterface(IID_PPV_ARGS(&sc1)))) {
            hook_proxy_swapchain(sc1.Get());
        }
    } else {
        log_error("Streamline CreateSwapChain failed: {:x}", (uint32_t)result);
    }

    proxy->Release();
    return result;
}

// The class hooks only take the game's own calls on factories without an instance hook (those go through the instance
// hook, which ends up here as a call from the plugin and passes); REFramework's dummy swapchain, ReShade, OptiScaler and
// Streamline's own calls pass straight through.
HRESULT WINAPI class_create_swapchain_for_hwnd_hook(IDXGIFactory2* factory, IUnknown* device, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
    const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen_desc, IDXGIOutput* output, IDXGISwapChain1** swapchain)
{
    if (factory_hook(factory) != nullptr || t_inside_streamline || !maybe_game_call(_ReturnAddress())) {
        return ((decltype(&create_swapchain_for_hwnd_hook))g_class_create_swapchain_for_hwnd)(factory, device, hwnd, desc, fullscreen_desc, output,
            swapchain);
    }

    init_streamline(); // normally done when the game creates its factory, which here was before the plugin loaded
    return create_swapchain_for_hwnd_hook(factory, device, hwnd, desc, fullscreen_desc, output, swapchain);
}

HRESULT WINAPI class_create_swapchain_hook(IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swapchain) {
    if (factory_hook(factory) != nullptr || t_inside_streamline || !maybe_game_call(_ReturnAddress())) {
        return ((decltype(&create_swapchain_hook))g_class_create_swapchain)(factory, device, desc, swapchain);
    }

    init_streamline();
    return create_swapchain_hook(factory, device, desc, swapchain);
}

// A factory of the kind the game gets (through its own import, so the same class, other mods' wrappers included): its
// class's CreateSwapChain(ForHwnd) are hooked for all factories of that class.
bool hook_factory_class(void* create_factory1, void* create_factory2) {
    // As IDXGIFactory1 (asking for IDXGIFactory2 directly failed in RE4: DXGI_ERROR_INVALID_CALL), then the newer interface.
    ComPtr<IDXGIFactory1> base{};
    auto result = create_factory1 != nullptr ? ((HRESULT(WINAPI*)(REFIID, void**))create_factory1)(IID_PPV_ARGS(&base)) : E_POINTER;

    if (FAILED(result) && create_factory2 != nullptr) {
        result = ((HRESULT(WINAPI*)(UINT, REFIID, void**))create_factory2)(0, IID_PPV_ARGS(&base));
    }

    // A mod's dxgi.dll in front may refuse (OptiScaler in RE4): Windows' own, the class underneath it.
    if (FAILED(result)) {
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const auto real = LoadLibraryW((std::filesystem::path(system) / L"dxgi.dll").c_str());
        const auto create = real != nullptr ? GetProcAddress(real, "CreateDXGIFactory1") : nullptr;
        log_info("DXGI factory class hooks: the game's dxgi.dll refused ({:x}): Windows' own dxgi.dll", (uint32_t)result);
        result = create != nullptr ? ((HRESULT(WINAPI*)(REFIID, void**))create)(IID_PPV_ARGS(&base)) : E_POINTER;
    }

    ComPtr<IDXGIFactory2> factory{};

    if (FAILED(result) || base == nullptr || FAILED(base.As(&factory))) {
        log_error("DXGI factory class hooks: no factory to read the class from ({:x})", (uint32_t)result);
        return false;
    }

    const auto vtable = *(void***)factory.Get();
    const auto ok = MH_CreateHook(vtable[10], (void*)&class_create_swapchain_hook, &g_class_create_swapchain) == MH_OK &&
        MH_EnableHook(vtable[10]) == MH_OK &&
        MH_CreateHook(vtable[15], (void*)&class_create_swapchain_for_hwnd_hook, &g_class_create_swapchain_for_hwnd) == MH_OK &&
        MH_EnableHook(vtable[15]) == MH_OK;

    log_info("DXGI factory class hooks (CreateSwapChain {:x}, CreateSwapChainForHwnd {:x}): {}", (uintptr_t)vtable[10], (uintptr_t)vtable[15],
        ok ? "on" : "FAILED");
    return ok;
}

void hook_factory(void* factory) {
    if (factory == nullptr) {
        return;
    }

    std::scoped_lock _{g_hooks_mutex};

    if (g_factory_hooks.contains(factory)) {
        return;
    }

    auto hook = new InstanceVtableHook(factory, FACTORY_VTABLE_ENTRIES);
    hook->hook(10, (void*)&create_swapchain_hook);
    hook->hook(15, (void*)&create_swapchain_for_hwnd_hook);
    g_factory_hooks[factory] = hook;

    log_info("Hooked the game's DXGI factory @ {:x}", (uintptr_t)factory);
}

HRESULT WINAPI create_dxgi_factory1_hook(REFIID riid, void** factory) {
    const auto result = g_factory1_hook->original<decltype(&create_dxgi_factory1_hook)>()(riid, factory);

    if (SUCCEEDED(result) && factory != nullptr) {
        hook_factory(*factory);
        init_streamline();
    }

    return result;
}

HRESULT WINAPI create_dxgi_factory2_hook(UINT flags, REFIID riid, void** factory) {
    const auto result = g_factory2_hook->original<decltype(&create_dxgi_factory2_hook)>()(flags, riid, factory);

    if (SUCCEEDED(result) && factory != nullptr) {
        hook_factory(*factory);
        init_streamline();
    }

    return result;
}

// Games that look CreateDXGIFactory1/2 up at runtime (RE4: its import slots are never called) get these instead, through
// a GetProcAddress hook that only answers the game's own code.
std::atomic<void*> g_runtime_factory1{};
std::atomic<void*> g_runtime_factory2{};
decltype(&GetProcAddress) g_get_proc_address{};

HRESULT WINAPI runtime_factory1_hook(REFIID riid, void** factory) {
    const auto result = ((decltype(&create_dxgi_factory1_hook))g_runtime_factory1.load())(riid, factory);

    if (SUCCEEDED(result) && factory != nullptr) {
        hook_factory(*factory);
        init_streamline();
    }

    return result;
}

HRESULT WINAPI runtime_factory2_hook(UINT flags, REFIID riid, void** factory) {
    const auto result = ((decltype(&create_dxgi_factory2_hook))g_runtime_factory2.load())(flags, riid, factory);

    if (SUCCEEDED(result) && factory != nullptr) {
        hook_factory(*factory);
        init_streamline();
    }

    return result;
}

// REFramework's pd-upscaler builds (TemporalUpscaler + PureDark's PDPerfPlugin.dll): their own DLSS/FSR2/XeSS. With the
// plugin's DLSS both start NVIDIA's NGX in one process: RE8 crashed in the PD side's DLSS creation (2026-11-06), and both
// would set the render size every frame (it forces ImageQualityRate 1 and spoofs SceneView.get_Size). Its
// IsUpscaleMethodAvailable is patched to return false (setting override_pd_upscaler, on by default) as soon as the DLL is
// mapped (a DLL load notification; older builds import it, it is then already loaded): the TemporalUpscaler finds no method
// and stays off ("No upscale methods are available") before it creates anything. Nothing in its files or settings changes.
// (A GetProcAddress answer didn't work: the current builds delay-load it, and kernel32's GetProcAddress bypasses kernelbase's.)
std::atomic<bool> g_pd_overridden{};
bool g_standing_down{}; // override_pd_upscaler=0 with a PD backend, or REFramework too old: nothing set up, initialize() does nothing

void patch_pd_upscaler(HMODULE module) {
    const auto target = (uint8_t*)GetProcAddress(module, "IsUpscaleMethodAvailable");
    DWORD old_protect{};

    if (target == nullptr || !VirtualProtect(target, 3, PAGE_EXECUTE_READWRITE, &old_protect)) {
        log_error("PDPerfPlugin.dll: IsUpscaleMethodAvailable not patched: REFramework's PD upscaler may start its own DLSS");
        return;
    }

    target[0] = 0x31; // xor eax, eax
    target[1] = 0xC0;
    target[2] = 0xC3; // ret
    VirtualProtect(target, 3, old_protect, &old_protect);
    FlushInstructionCache(GetCurrentProcess(), target, 3);
    g_pd_overridden = true;
    log_info("REFramework's PD upscaler (PDPerfPlugin.dll): IsUpscaleMethodAvailable answers no, its TemporalUpscaler stays off; "
             "Native DLSS + FG's DLSS is in charge (setting override_pd_upscaler)");
}

struct LdrUnicodeString {
    USHORT length;
    USHORT maximum_length;
    PWSTR buffer;
};

struct LdrDllLoadedData {
    ULONG flags;
    const LdrUnicodeString* full_name;
    const LdrUnicodeString* base_name;
    void* base;
    ULONG size;
};

void CALLBACK dll_notification(ULONG reason, const LdrDllLoadedData* data, void*) {
    if (reason != 1 || data == nullptr || data->base_name == nullptr || data->base_name->buffer == nullptr) { // 1: loaded
        return;
    }

    const std::wstring_view name{data->base_name->buffer, data->base_name->length / sizeof(wchar_t)};

    if (name.size() == 16 && _wcsnicmp(name.data(), L"PDPerfPlugin.dll", 16) == 0) {
        patch_pd_upscaler((HMODULE)data->base);
    }
}

void override_pd_upscaler() {
    if (const auto module = GetModuleHandleW(L"PDPerfPlugin.dll"); module != nullptr) {
        patch_pd_upscaler(module); // older builds import it: already loaded
        return;
    }

    using Register = LONG(NTAPI*)(ULONG, void(CALLBACK*)(ULONG, const LdrDllLoadedData*, void*), void*, void**);
    const auto register_notification = (Register)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "LdrRegisterDllNotification");
    static void* cookie{};

    if (register_notification == nullptr || register_notification(0, &dll_notification, nullptr, &cookie) != 0) {
        log_error("PDPerfPlugin.dll: no DLL load notification: REFramework's PD upscaler may start its own DLSS");
    }
}

bool from_game(void* address) {
    HMODULE module{};
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module) &&
        module == GetModuleHandleW(nullptr);
}

FARPROC WINAPI get_proc_address_hook(HMODULE module, LPCSTR name) {
    const auto result = g_get_proc_address(module, name);

    if (result == nullptr || ((uintptr_t)name >> 16) == 0) {
        return result;
    }

    // Diagnostics: who looks the DXGI factory functions up (once per module and name).
    if (std::strncmp(name, "CreateDXGIFactory", 17) == 0) {
        static std::mutex mutex{};
        static std::vector<std::string> seen{};
        HMODULE caller{};
        wchar_t path[MAX_PATH]{};
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)_ReturnAddress(), &caller);
        GetModuleFileNameW(caller, path, MAX_PATH);
        const auto key = std::filesystem::path(path).filename().string() + " " + name;
        std::scoped_lock _{mutex};

        if (std::find(seen.begin(), seen.end(), key) == seen.end()) {
            seen.push_back(key);
            wchar_t from[MAX_PATH]{};
            GetModuleFileNameW(module, from, MAX_PATH);
            log_info("GetProcAddress {} by {} (from {})", name, std::filesystem::path(path).filename().string(), std::filesystem::path(from).filename().string());
        }
    }

    // Only the game's own code gets our hooks.
    if (!from_game(_ReturnAddress())) {
        return result;
    }

    if (std::strcmp(name, "CreateDXGIFactory1") == 0) {
        if (g_runtime_factory1.exchange((void*)result) == nullptr) log_info("The game looks up CreateDXGIFactory1 at runtime: hooked");
        return (FARPROC)&runtime_factory1_hook;
    }

    if (std::strcmp(name, "CreateDXGIFactory2") == 0) {
        if (g_runtime_factory2.exchange((void*)result) == nullptr) log_info("The game looks up CreateDXGIFactory2 at runtime: hooked");
        return (FARPROC)&runtime_factory2_hook;
    }

    return result;
}

// ---------------------------------------------------------------------------------------------------------------------
// Per frame.
std::atomic<sl::FrameToken*> g_sim_token{};
std::atomic<sl::FrameToken*> g_pending_token{};
sl::FrameToken* g_present_token{};
std::atomic<sl::FrameToken*> g_constants_token{}; // the frame whose camera constants the DLSS pass already set

// The DLSS pass's inputs for frame generation, taken by the next Present (fresh = not used yet).
std::mutex g_frame_inputs_mutex{};
framegen::FrameInputs g_frame_inputs{};
bool g_frame_inputs_fresh{};
ID3D12Resource* g_hudless{};
uint32_t g_hudless_state{};
bool g_hudless_fresh{};
std::atomic<uint32_t> g_hudless_frames{0};
std::atomic<uint32_t> g_real_input_frames{0};
std::atomic<bool> g_force_placeholders{}; // toggled live with the inputs key: the stand-ins instead, to compare

std::atomic<uint32_t> g_game_presents{0};   // the game's Presents (real frames)
std::atomic<uint32_t> g_screen_presents{0}; // REFramework's Present callback: every frame shown, real or generated
bool g_reset_history{true};

// Feasibility test only: stand-in inputs (no motion, depth at the far plane) until the scene's own are wired up.
ComPtr<ID3D12Resource> g_depth{};
ComPtr<ID3D12Resource> g_motion{};
uint32_t g_input_size[2]{};
constexpr auto INPUT_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

bool ensure_placeholder_inputs(uint32_t w, uint32_t h) {
    if (g_depth != nullptr && g_input_size[0] == w && g_input_size[1] == h) {
        return true;
    }

    g_depth.Reset();
    g_motion.Reset();

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;

    // New committed resources are zero-filled: depth 0 (far plane with inverted depth), motion (0, 0).
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    const auto depth_ok = SUCCEEDED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, INPUT_STATE, nullptr, IID_PPV_ARGS(&g_depth)));
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    const auto motion_ok = SUCCEEDED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, INPUT_STATE, nullptr, IID_PPV_ARGS(&g_motion)));

    if (!depth_ok || !motion_ok) {
        log_error("Failed to create the placeholder inputs ({}x{})", w, h);
        g_depth.Reset();
        g_motion.Reset();
        return false;
    }

    g_input_size[0] = w;
    g_input_size[1] = h;
    g_reset_history = true;
    log_info("Placeholder depth/motion inputs {}x{}", w, h);
    return true;
}

void set_placeholder_constants(const sl::FrameToken& token, uint32_t w, uint32_t h) {
    sl::Constants c{};
    sl::float4x4 identity{};
    identity.setRow(0, {1, 0, 0, 0});
    identity.setRow(1, {0, 1, 0, 0});
    identity.setRow(2, {0, 0, 1, 0});
    identity.setRow(3, {0, 0, 0, 1});

    c.cameraViewToClip = identity;
    c.clipToCameraView = identity;
    c.clipToPrevClip = identity;
    c.prevClipToClip = identity;
    c.cameraRight = {1, 0, 0};
    c.cameraUp = {0, 1, 0};
    c.cameraFwd = {0, 0, 1};
    c.cameraPos = {0, 0, 0};
    c.jitterOffset = {0, 0};
    c.mvecScale = {1.0f / (float)w, 1.0f / (float)h};
    c.cameraPinholeOffset = {0, 0};
    c.cameraNear = 0.1f;
    c.cameraFar = 1000.0f;
    c.cameraFOV = 1.0f;
    c.cameraAspectRatio = (float)w / (float)h;
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = g_reset_history ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;

    g_sl.set_constants(c, token, g_viewport);
    g_reset_history = false;
}

void on_game_present(IDXGISwapChain3* swapchain) {
    g_present_token = nullptr;

    if (!g_sl.initialized) {
        return;
    }

    auto token = g_pending_token.exchange(nullptr);

    if (token == nullptr) {
        token = new_frame_token();
    }

    if (token == nullptr) {
        return;
    }

    g_present_token = token;

    ComPtr<ID3D12Resource> backbuffer{};
    swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer));

    if (backbuffer == nullptr) {
        return;
    }

    const auto bb = backbuffer->GetDesc();
    const auto w = (uint32_t)bb.Width;
    const auto h = bb.Height;

    sl::DLSSGOptions options{};
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    options.numFramesToGenerate = 1;
    options.mode = sl::DLSSGMode::eOff;

    // This frame's real inputs from the DLSS pass, if it ran for it (else the stand-ins: no motion, far depth).
    framegen::FrameInputs inputs{};
    bool real = false;
    ID3D12Resource* hudless{};
    uint32_t hudless_state{};
    {
        std::scoped_lock _{g_frame_inputs_mutex};
        real = g_frame_inputs_fresh && !g_force_placeholders;
        g_frame_inputs_fresh = false;
        inputs = g_frame_inputs;
        hudless = g_hudless_fresh ? g_hudless : nullptr;
        hudless_state = g_hudless_state;
        g_hudless_fresh = false;
    }

    if (g_settings.enabled && (real || ensure_placeholder_inputs(w, h))) {
        // Streamline takes one set of constants per frame token: the DLSS pass set them if it used this one.
        if (g_constants_token.load() != token) {
            if (real) {
                g_sl.set_constants(inputs.constants, *token, g_viewport);
            } else {
                set_placeholder_constants(*token, w, h);
            }
        }

        const auto input_w = real ? inputs.width : w;
        const auto input_h = real ? inputs.height : h;
        g_real_input_frames += real ? 1 : 0;

        sl::Resource depth{sl::ResourceType::eTex2d, real ? inputs.depth : g_depth.Get(), nullptr, nullptr, real ? inputs.depth_state : (uint32_t)INPUT_STATE};
        sl::Resource motion{sl::ResourceType::eTex2d, real ? inputs.motion : g_motion.Get(), nullptr, nullptr, real ? inputs.motion_state : (uint32_t)INPUT_STATE};
        const sl::Extent extent{0, 0, input_w, input_h};
        sl::ResourceTag tags[] = {
            sl::ResourceTag{&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent},
            sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent},
        };
        g_sl.set_tag_for_frame(*token, g_viewport, tags, (uint32_t)std::size(tags), nullptr);

        // The HUD fix: this frame's HUD-less copy (only with the real inputs, the same frame).
        if (real && hudless != nullptr) {
            sl::Resource hudless_res{sl::ResourceType::eTex2d, hudless, nullptr, nullptr, hudless_state};
            const sl::Extent full{0, 0, w, h};
            sl::ResourceTag hudless_tag{&hudless_res, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &full};
            g_sl.set_tag_for_frame(*token, g_viewport, &hudless_tag, 1, nullptr);
            ++g_hudless_frames;
        }

        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = g_settings.generated;
        options.colorWidth = w;
        options.colorHeight = h;
        options.mvecDepthWidth = input_w;
        options.mvecDepthHeight = input_h;
    }

    auto result = g_sl.dlssg_set_options(g_viewport, options);

    // Above the driver's limit (or without the multi frame generation unlock) DLSS-G refuses: step down.
    while (result != sl::Result::eOk && options.mode == sl::DLSSGMode::eOn && options.numFramesToGenerate > 1) {
        --options.numFramesToGenerate;
        result = g_sl.dlssg_set_options(g_viewport, options);
    }

    static uint32_t last_generated = 0;

    if (result != sl::Result::eOk) {
        static bool once = true;

        if (once) {
            log_error("slDLSSGSetOptions failed: {}", (uint32_t)result);
            once = false;
        }
    } else if (options.mode == sl::DLSSGMode::eOn && options.numFramesToGenerate != last_generated) {
        log_info("DLSS-G on: {}x (asked for {}x)", options.numFramesToGenerate + 1, g_settings.generated + 1);
        last_generated = options.numFramesToGenerate;
    }

    pcl_marker(sl::PCLMarker::ePresentStart, token);
}

std::string module_version(HMODULE module);

std::string module_version(const wchar_t* name) {
    return module_version(GetModuleHandleW(name));
}

// The DLSS DLL actually in use: NVIDIA's driver override (the NVIDIA app: ProgramData\NVIDIA\NGX\models\<feature>\...) if
// loaded, else the game's nvngx_<feature>.dll. "310.9.16.0 (NVIDIA override)" or "310.7.0.0", "" if neither is loaded.
std::string feature_version(const wchar_t* feature) {
    HMODULE modules[1024]{};
    DWORD needed{};

    if (K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
        const auto models = std::wstring{L"\\ngx\\models\\"} + feature + L"\\";

        for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
            wchar_t path[MAX_PATH]{};
            GetModuleFileNameW(modules[i], path, MAX_PATH);
            std::wstring lower{path};
            std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });

            if (lower.find(models) != std::wstring::npos) {
                return module_version(modules[i]) + " (NVIDIA override)";
            }
        }
    }

    const auto module = GetModuleHandleW((std::wstring{L"nvngx_"} + feature + L".dll").c_str());

    // Once per feature: which file is in use (the mod's folder, or another nvngx_ DLL the game or the driver brought).
    if (static std::set<std::wstring> logged{}; module != nullptr && logged.insert(feature).second) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        log_info("nvngx_{}.dll in use: {} ({})", std::filesystem::path{feature}.string(), std::filesystem::path{path}.string(), module_version(module));
    }

    return module_version(module);
}

// The file version of a loaded DLL (from its version resource, without version.dll), e.g. "310.2.1.0"; "" if not loaded.
std::string module_version(HMODULE module) {
    const auto info = module != nullptr ? FindResourceW(module, MAKEINTRESOURCEW(1), RT_VERSION) : nullptr;
    const auto data = info != nullptr ? LoadResource(module, info) : nullptr;
    const auto bytes = data != nullptr ? (const uint8_t*)LockResource(data) : nullptr;
    const auto size = info != nullptr ? SizeofResource(module, info) : 0;

    for (DWORD i = 0; bytes != nullptr && i + sizeof(VS_FIXEDFILEINFO) <= size; i += 4) {
        const auto fixed = (const VS_FIXEDFILEINFO*)(bytes + i);

        if (fixed->dwSignature == 0xFEEF04BD) {
            return std::format("{}.{}.{}.{}", HIWORD(fixed->dwFileVersionMS), LOWORD(fixed->dwFileVersionMS), HIWORD(fixed->dwFileVersionLS),
                LOWORD(fixed->dwFileVersionLS));
        }
    }

    return "";
}

// For the in-game menu (refw_framegen.lua): reframework/data/refw_framegen_status.txt, "key=value" lines, once a second.
std::atomic<uint32_t> g_status_game_presents{0};
std::atomic<uint32_t> g_status_screen_presents{0};

// The engine's sharpening, for the menu: kept (setting off), in_upscale (the profile: DLSS replaces the pass that sharpens),
// mod (another mod removes it), unavailable (the getter wasn't recognised), dlss_off (applies when DLSS runs), plugin.
const char* sharpening_state() {
    if (game_profile::current().sharpening_in_upscale) return "in_upscale";
    if (g_settings.dlss_sharpening_off == 0) return g_sharpening_mod.empty() ? "kept" : "kept_mod"; // kept_mod: unticked, another mod removes it
    if (g_settings.dlss_sharpening_off == 1 && !g_sharpening_mod.empty()) return "mod";
    if (!engine_hooks::sharpness_hooked()) return "unavailable";
    if (!g_settings.dlss || !g_sl.dlss_supported) return "dlss_off";
    return "plugin";
}

void write_status_file() {
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<float>(now - last).count();

    if (elapsed < 1.0f) {
        return;
    }

    last = now;
    const auto game = (float)g_status_game_presents.exchange(0) / elapsed;
    const auto shown = (float)g_status_screen_presents.exchange(0) / elapsed;

    sl::DLSSGState state{};
    const auto state_ok = g_sl.initialized && g_sl.dlssg_get_state(g_viewport, state, nullptr) == sl::Result::eOk;

    std::ofstream out{g_data_dir / L"refw_framegen_status.txt", std::ios::trunc};

    if (!g_error.empty()) {
        out << "error=" << g_error << '\n';
    }

    out << std::format("fps_game={:.1f}\nfps_shown={:.1f}\n", game, shown)
        << std::format("fg_available={}\nfg_hooked={}\nfg_max_generated={}\nfg_frames_presented={}\n", g_sl.initialized ? 1 : 0,
               g_fg_hooked ? 1 : 0, state_ok ? state.numFramesToGenerateMax : 0, state_ok ? state.numFramesActuallyPresented : 0)
        << std::format("dlss_available={}\n", g_sl.dlss_supported ? 1 : 0) << dlss_pass::status_values()
        << std::format("dlss_version={}\ndlssg_version={}\nsl_version={}\n", feature_version(L"dlss"), feature_version(L"dlssg"),
               module_version(L"sl.interposer.dll"))
        << std::format("set_enabled={}\nset_generated={}\nset_dlss={}\nset_dlss_mode={}\nset_dlss_preset={}\nset_lod_bias={}\nset_lod_bias_offset={:.2f}\n"
                       "set_hud_fix={}\n",
               g_settings.enabled ? 1 : 0, g_settings.generated, g_settings.dlss ? 1 : 0, g_settings.dlss_mode, g_settings.dlss_preset,
               g_settings.dlss_lod_bias ? 1 : 0, g_settings.dlss_lod_bias_offset, g_settings.dlss_hud_fix ? 1 : 0)
        // RE2: DLAA while examining items; RE7: Ultra Performance runs as Performance (dlss_pass: profile min_half_render).
        << std::format("active_dlss_mode={}\n", dlss_pass::examine_dlaa() ? 0
               : dlss_pass::half_limit_now() && g_settings.dlss_mode == 4 ? 3 : g_settings.dlss_mode)
        << std::format("nv_sr_override={}\n", g_nv_sr_override)
        << std::format("own_bloom={}\n", game_profile::current().own_bloom ? 1 : 0)
        << std::format("re2_dither={}\n", game_profile::current().dither_experiments ? 1 : 0)
        << std::format("pd_upscaler={}\n", g_pd_overridden.load() ? 1 : 0)
        << std::format("sharpening={}\nsharpening_mod={}\nsharpening_mod_kind={}\n", sharpening_state(), g_sharpening_mod, g_sharpening_mod_kind)
        << std::format("profile={}\nvideotape_mode={}\nown_dof={}\nexamine_off={}\n", game_profile::current().name, game_profile::current().videotape_mode ? 1 : 0,
               game_profile::current().own_dof ? 1 : 0, game_profile::current().examine_dlss_off ? 1 : 0)
        << hud_split::status_values();
}

std::string cpu_times_text();

void on_game_present_end() {
    pcl_marker(sl::PCLMarker::ePresentEnd, g_present_token);
    g_present_token = nullptr;
    ++g_game_presents;
    ++g_status_game_presents;
    reload_settings_if_changed();
    write_status_file();

    static auto window_start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<float>(now - window_start).count();

    if (elapsed < (float)g_settings.status_every_s) {
        return;
    }

    const auto game = (float)g_game_presents.exchange(0) / elapsed;
    const auto screen = (float)g_screen_presents.exchange(0) / elapsed;
    window_start = now;

    sl::DLSSGState state{};
    const auto state_ok = g_sl.initialized && g_sl.dlssg_get_state(g_viewport, state, nullptr) == sl::Result::eOk;

    log_info("fps: game {:.1f}, shown {:.1f} (x{:.2f}); DLSS-G status {}, frames presented per game frame {}, max generated {}",
        game, screen, game > 0.0f ? screen / game : 0.0f, state_ok ? (uint32_t)state.status : 9999, state_ok ? state.numFramesActuallyPresented : 0,
        state_ok ? state.numFramesToGenerateMax : 0);

    const auto calls = engine_hooks::take_counts();
    log_info("engine calls/s: SceneView.get_Size {:.0f}, Scene update {:.0f}, Overlay draw {:.0f}, PrepareOutput draw {:.0f}, Scene.get_LODBias {:.0f}, "
             "ToneMapping.get_Sharpness {:.0f}",
        calls.view_get_size / elapsed, calls.scene_update / elapsed, calls.overlay_draw / elapsed, calls.prepare_output_draw / elapsed,
        calls.lod_bias / elapsed, calls.sharpness / elapsed);
    log_info("{}", cpu_times_text());

    if (const auto waits = wait_probe::status(); !waits.empty()) {
        log_info("{}", waits);
    }
    log_info("inputs: {}", scene_inputs::describe());
    log_info("{}", dlss_pass::status());
    log_info("DLSS-G inputs: real depth/motion/camera on {:.0f}% of frames", game > 0.0f ? 100.0f * g_real_input_frames.exchange(0) / (game * elapsed) : 0.0f);
    log_info("{}; HUD-less frame tagged on {:.0f}% of frames", hud_split::status(), game > 0.0f ? 100.0f * g_hudless_frames.exchange(0) / (game * elapsed) : 0.0f);
}

// CPU time on the game's thread in the plugin's own Present work, in Streamline's Present and in Reflex's sleep (for the
// status line: what Streamline's swapchain costs per frame).
struct CpuTimes {
    std::atomic<int64_t> plugin{}, present{}, present_max{}, sleep{};
    std::atomic<uint32_t> frames{};
} g_cpu_times{};

int64_t qpc() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

double qpc_ms(int64_t ticks) {
    static const auto frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return (double)f.QuadPart;
    }();
    return 1000.0 * (double)ticks / frequency;
}

std::string cpu_times_text() {
    const auto frames = std::max<uint32_t>(1, g_cpu_times.frames.exchange(0));
    return std::format("CPU ms per frame: plugin before Present {:.3f}, Streamline's Present {:.3f} (max {:.2f}), Reflex sleep {:.3f}",
        qpc_ms(g_cpu_times.plugin.exchange(0)) / frames, qpc_ms(g_cpu_times.present.exchange(0)) / frames, qpc_ms(g_cpu_times.present_max.exchange(0)),
        qpc_ms(g_cpu_times.sleep.exchange(0)) / frames);
}

template <typename Fn>
HRESULT present_common(IDXGISwapChain3* swapchain, UINT flags, Fn&& call_original) {
    if ((flags & DXGI_PRESENT_TEST) != 0) {
        return call_original();
    }

    const auto t0 = qpc();
    on_game_present(swapchain);

    if (g_own_waitable) {
        frame_pacer::on_present(); // this frame's GPU work, ahead of Streamline's copy and present
    }

    const auto t1 = qpc();
    const auto result = call_original();
    const auto t2 = qpc();
    g_cpu_times.plugin += t1 - t0;
    g_cpu_times.present += t2 - t1;

    if (t2 - t1 > g_cpu_times.present_max.load()) {
        g_cpu_times.present_max = t2 - t1;
    }

    ++g_cpu_times.frames;
    wait_probe::on_present_thread();
    wait_probe::on_frame();
    on_game_present_end();
    return result;
}

HRESULT WINAPI present_hook(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags) {
    const auto original = g_swapchain_hook->original<decltype(&present_hook)>(8);
    return present_common(swapchain, flags, [&]() { return original(swapchain, sync_interval, flags); });
}

HRESULT WINAPI present1_hook(IDXGISwapChain3* swapchain, UINT sync_interval, UINT flags, const DXGI_PRESENT_PARAMETERS* params) {
    const auto original = g_swapchain_hook->original<decltype(&present1_hook)>(22);
    return present_common(swapchain, flags, [&]() { return original(swapchain, sync_interval, flags, params); });
}

HRESULT WINAPI resize_buffers_hook(IDXGISwapChain3* swapchain, UINT count, UINT w, UINT h, DXGI_FORMAT format, UINT flags) {
    const auto original = g_swapchain_hook->original<decltype(&resize_buffers_hook)>(13);

    // DLSS-G has to be off while the swapchain changes size.
    if (g_sl.initialized) {
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOff;
        options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        g_sl.dlssg_set_options(g_viewport, options);
    }

    // The waitable flag must stay as created.
    if (g_waitable_added) {
        flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    log_info("ResizeBuffers({}, {}x{}, format {})", count, w, h, (uint32_t)format);
    return original(swapchain, count, w, h, format, flags);
}

// ---------------------------------------------------------------------------------------------------------------------
// REFramework callbacks.
void on_update_hid() {
    if (!g_sl.initialized || g_proxy_swapchain == nullptr) {
        return;
    }

    apply_reflex(); // the setting or frame generation changed
    cl_state::set_hooks_enabled((g_settings.perf_bypass & 1) == 0);
    pace_frame_start(g_proxy_swapchain.load());
    wait_probe::set_enabled(g_settings.perf_waits, g_device.Get());

    // Start of a game frame: Reflex paces it here.
    auto token = new_frame_token();
    g_sim_token = token;

    if (token != nullptr && (g_settings.perf_bypass & 2) == 0) {
        const auto t0 = qpc();
        g_sl.reflex_sleep(*token);
        g_cpu_times.sleep += qpc() - t0;
        pcl_marker(sl::PCLMarker::eSimulationStart, token);
    }
}

void on_begin_rendering() {
    // The inputs key (setting fg_inputs_key, F6 by default): frame generation with the stand-in inputs instead, to compare.
    // Without the diagnostic hotkeys (setting debug_keys, off by default) a state switched by a key goes back to normal.
    if (const auto key = g_settings.debug_keys ? g_settings.fg_inputs_key : 0; key == 0) {
        if (g_force_placeholders.exchange(false)) {
            log_info("DLSS-G inputs: REAL (diagnostic hotkeys off)");
        }
    } else {
        static bool was_down = false;
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const auto down = pid == GetCurrentProcessId() && (GetAsyncKeyState(key) & 0x8000) != 0;

        if (down && !was_down) {
            const auto placeholders = !g_force_placeholders.exchange(!g_force_placeholders.load());
            log_info("DLSS-G inputs: {} (key)", placeholders ? "STAND-INS (no motion, far depth)" : "REAL");
        }

        was_down = down;
    }

    pipeline_probe::on_begin_rendering();
    dlss_pass::on_begin_rendering();
    auto token = g_sim_token.load();
    pcl_marker(sl::PCLMarker::eSimulationEnd, token);
    pcl_marker(sl::PCLMarker::eRenderSubmitStart, token);
}

void on_end_rendering() {
    auto token = g_sim_token.exchange(nullptr);
    pcl_marker(sl::PCLMarker::eRenderSubmitEnd, token);

    if (token != nullptr) {
        g_pending_token = token; // presented next
    }
}

void on_reframework_present() {
    ++g_screen_presents;
    ++g_status_screen_presents;

    // Which thread REFramework's Present runs on (with DLSS-G: Streamline's present thread, real and generated frames).
    static std::atomic<int> logged{0};

    if (logged < 3 && ++logged <= 3) {
        log_info("REFramework Present callback (thread {})", GetCurrentThreadId());
    }
}
} // namespace

namespace framegen {
void early_init(HMODULE self) try {
    wchar_t exe_path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
    g_game_dir = std::filesystem::path{exe_path}.parent_path();
    g_data_dir = g_game_dir / L"reframework" / L"data";
    std::filesystem::create_directories(g_data_dir);
    g_nvidia_dir = find_nvidia_dir();

    logging::open((g_data_dir / L"refw_framegen.log").wstring());
    log_info("{} {} (refw_framegen.dll) loaded into {} (thread {})", MOD_NAME, MOD_VERSION, std::filesystem::path{exe_path}.filename().string(),
        GetCurrentThreadId());

    load_settings();
    log_info("Settings: enabled={} generated={} ({}x)", g_settings.enabled, g_settings.generated, g_settings.generated + 1);

    // REFramework too old for plugin API 1.15 (it refuses the plugin only later, after these early hooks): nothing is set up.
    // A 2024 pd-upscaler build (v.1.5.6+220) did that and then hung while hooking D3D (2026-11-06). Its version is in its log
    // by now: API 1.15 since v1.5.9.1 + 71 commits (2025-04-21). Without a readable log: as usual.
    {
        std::ifstream log{g_game_dir / L"re2_framework_log.txt"};
        std::string line{};
        std::string tag{};
        std::string branch{};
        int past = -1;

        for (int n = 0; n < 200 && std::getline(log, line); ++n) {
            if (const auto at = line.find("] Tag: "); at != std::string::npos && tag.empty()) tag = line.substr(at + 7);
            else if (const auto at = line.find("] Commits past tag: "); at != std::string::npos && past < 0) past = std::atoi(line.c_str() + at + 20);
            else if (const auto at = line.find("] Branch: "); at != std::string::npos && branch.empty()) branch = line.substr(at + 10);
        }

        if (!tag.empty() && past >= 0) {
            // "v1.5.9.1", "v.1.5.6": the numbers, compared with 1.5.9.1 (+71 commits).
            std::vector<int> numbers{};

            for (size_t i = 0; i < tag.size();) {
                if (std::isdigit((unsigned char)tag[i])) {
                    numbers.push_back(std::atoi(tag.c_str() + i));
                    while (i < tag.size() && std::isdigit((unsigned char)tag[i])) ++i;
                } else {
                    ++i;
                }
            }

            // GitHub Actions builds without tags ("no_tag", +0, branch HEAD: the current pd-upscaler artifact): unknown, as usual.
            const std::vector<int> base{1, 5, 9, 1};
            const auto cmp = std::lexicographical_compare(numbers.begin(), numbers.end(), base.begin(), base.end()) ? -1 : numbers == base ? 0 : 1;
            const auto too_old = !numbers.empty() && (cmp < 0 || (cmp == 0 && past < 71));
            log_info("REFramework {}+{} ({}): {}", tag, past, branch.empty() ? "?" : branch,
                numbers.empty() ? "version unknown (no tag)" : too_old ? "older than plugin API 1.15" : "plugin API 1.15 or newer");

            if (too_old) {
                log_error("REFramework {}+{} is too old for this plugin (it needs plugin API 1.15: v1.5.9.1+71, April 2025, or newer): nothing set up",
                    tag, past);
                report_error("REFramework is too old (plugin API 1.15 needed: a nightly from April 2025 or newer, see README)");
                g_standing_down = true;
                return;
            }
        }
    }

    // REFramework's PD upscaler (PDPerfPlugin.dll next to the game: the pd-upscaler builds' backend). override_pd_upscaler=1
    // (default): kept off before it starts, this plugin's DLSS in its place. 0: this plugin stands down completely (nothing
    // set up, not even Streamline): both together crashed RE8 (2026-11-06). Before any hook, so standing down leaves none.
    {
        std::error_code ec{};

        if (std::filesystem::exists(g_game_dir / L"PDPerfPlugin.dll", ec)) {
            if (!g_settings.override_pd_upscaler) {
                log_info("PDPerfPlugin.dll next to the game and override_pd_upscaler=0: REFramework's PD upscaler is left in charge, "
                         "Native DLSS + FG sets nothing up (both together crashed RE8 in testing)");
                report_error("off: REFramework's PD upscaler is in charge (override_pd_upscaler=0 in refw_framegen.txt; 1 = this plugin's DLSS)");
                g_standing_down = true;
                return;
            }

            log_info("PDPerfPlugin.dll next to the game (REFramework pd-upscaler builds' backend): its upscaler will be kept off, "
                     "Native DLSS + FG's DLSS takes its place (override_pd_upscaler=1)");
            override_pd_upscaler();
        }
    }

    log_info("NVIDIA's files from {}", g_nvidia_dir == g_game_dir ? std::string{"the game's folder (older install layout)"} : g_nvidia_dir.string());

    if (!std::filesystem::exists(g_nvidia_dir / L"sl.interposer.dll") || !std::filesystem::exists(g_nvidia_dir / L"sl.dlss_g.dll")) {
        log_error("Streamline (sl.interposer.dll, sl.dlss_g.dll) not found in reframework/data/Native DLSS + Frame Generation (or next to "
                  "the game): DLSS and frame generation unavailable");
        report_error("NVIDIA's files are missing: reframework/data/Native DLSS + Frame Generation (sl.interposer.dll, ...: see README)");
        return;
    }

    // Streamline is set up even with frame generation off: DLSS super resolution goes through it too, and frame
    // generation can then be switched on later (enabled only decides whether DLSS-G generates frames, at Present).
    if (!g_settings.enabled) {
        log_info("Frame generation is off in refw_framegen.txt (Streamline is still set up for DLSS)");
    }

    // Only the game's own import slots: REFramework, ReShade, OptiScaler and Streamline itself still get plain factories.
    const auto exe = GetModuleHandleW(nullptr);

    if (auto slot = find_import_slot(exe, "dxgi.dll", "CreateDXGIFactory1"); slot != nullptr) {
        g_factory1_hook = std::make_unique<PointerHook>(slot, (void*)&create_dxgi_factory1_hook);
    }

    if (auto slot = find_import_slot(exe, "dxgi.dll", "CreateDXGIFactory2"); slot != nullptr) {
        g_factory2_hook = std::make_unique<PointerHook>(slot, (void*)&create_dxgi_factory2_hook);
    }

    // Per game (profile, RE4): runtime lookups (GetProcAddress, in kernelbase where kernel32's forwards to) and, below, the
    // factory class hooks. Other games never get these hooks.
    bool runtime_hooked = false;

    if (game_profile::current().factory_class_hooks) {
        const auto get_proc = (void*)GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetProcAddress");
        const auto mh = MH_Initialize();
        runtime_hooked = (mh == MH_OK || mh == MH_ERROR_ALREADY_INITIALIZED) && get_proc != nullptr &&
            MH_CreateHook(get_proc, (void*)&get_proc_address_hook, (void**)&g_get_proc_address) == MH_OK && MH_EnableHook(get_proc) == MH_OK;
    }

    log_info("DXGI import hooks: CreateDXGIFactory1={} CreateDXGIFactory2={}, runtime lookups={}", g_factory1_hook != nullptr,
        g_factory2_hook != nullptr, runtime_hooked);

    // For a factory the game created before the plugin loaded: hooks on its class (the game's import's original).
    if (runtime_hooked) {
        // From a thread: this runs while the plugin DLL is still being loaded, and DXGI refuses to create factories then
        // (DXGI_ERROR_INVALID_CALL). The thread starts as soon as loading is done, before the game's swapchain.
        const auto thread = CreateThread(nullptr, 0, [](void*) -> DWORD {
            const auto dxgi = GetModuleHandleW(L"dxgi.dll");
            hook_factory_class(g_factory1_hook != nullptr ? g_factory1_hook->original<void*>() : (void*)g_get_proc_address(dxgi, "CreateDXGIFactory1"),
                g_factory2_hook != nullptr ? g_factory2_hook->original<void*>() : (void*)g_get_proc_address(dxgi, "CreateDXGIFactory2"));
            return 0;
        }, nullptr, 0, nullptr);

        if (thread != nullptr) {
            CloseHandle(thread);
        }
    }
    g_fg_hooked = g_factory1_hook != nullptr || g_factory2_hook != nullptr || runtime_hooked;
} catch (const std::exception& e) {
    log_error("early_init failed: {}", e.what());
}

bool initialize(const REFrameworkPluginInitializeParam* param) {
    const auto* f = param->functions;
    logging::set_reframework_sink(f->log_warn, f->log_error);
    refw::init(param);

    log_info("REFramework plugin API {}.{}.{} ({}), initializing", param->version->major, param->version->minor, param->version->patch,
        param->version->game_name != nullptr ? param->version->game_name : "?");

    // Standing down (early_init set nothing up): no callbacks, no hooks; the reason stays in the menu.
    if (g_standing_down) {
        log_info("Standing down: nothing initialized ({})", g_error);
        return true;
    }
    g_game = param->version->game_name != nullptr ? param->version->game_name : "";

    const auto ok = f->on_present(&on_reframework_present) &&
        f->on_pre_application_entry("UpdateHID", &on_update_hid) &&
        f->on_pre_application_entry("BeginRendering", &on_begin_rendering) &&
        f->on_post_application_entry("EndRendering", &on_end_rendering);

    if (!ok) {
        log_error("Could not register REFramework callbacks: frame generation pacing unavailable");
    }

    // NVIDIA's DLSS-SR preset override (NVIDIA app / control panel), for the overlay: the game's profile, else the global one.
    {
        const auto app = nvapi_drs::read_settings(nullptr, false);
        const auto global = nvapi_drs::read_settings(nullptr, true);

        auto find = [](const std::vector<nvapi_drs::Setting>& settings, uint32_t id) -> std::optional<uint32_t> {
            for (const auto& s : settings) {
                if (s.id == id && s.is_dword) {
                    return s.value;
                }
            }

            return std::nullopt;
        };

        constexpr uint32_t ENABLE_SR_OVERRIDE = 0x10E41E01;
        constexpr uint32_t SR_PRESETS = 0x10E41DF3;
        const auto enabled = find(app, ENABLE_SR_OVERRIDE).value_or(find(global, ENABLE_SR_OVERRIDE).value_or(0));
        const auto preset = find(app, SR_PRESETS).value_or(find(global, SR_PRESETS).value_or(0));
        g_nv_sr_override = enabled != 0 ? preset : 0;
        log_info("NVIDIA DLSS-SR preset override: {}", enabled != 0 ? std::format("on, {:#x}", preset) : "off");
    }

    if (g_settings.probe_drs) {
        std::string profile{};
        const auto settings = nvapi_drs::read_settings(&profile);
        log_info("NVIDIA driver settings for the game ({}): {}", profile, settings.size());

        for (const auto& s : settings) {
            log_info("  drs {:08x} {} = {} (location {})", s.id, s.name.empty() ? "?" : s.name, s.is_dword ? std::format("{:#x}", s.value) : "(not a number)",
                s.location);
        }

        const auto global = nvapi_drs::read_settings(nullptr, true);
        log_info("NVIDIA driver settings, global profile: {}", global.size());

        for (const auto& s : global) {
            log_info("  drs global {:08x} {} = {} (location {})", s.id, s.name.empty() ? "?" : s.name,
                s.is_dword ? std::format("{:#x}", s.value) : "(not a number)", s.location);
        }
    }

    for (size_t start = 0; start < g_settings.probe_code.size();) {
        const auto end = std::min(g_settings.probe_code.find(',', start), g_settings.probe_code.size());
        const auto entry = g_settings.probe_code.substr(start, end - start);

        if (const auto dot = entry.rfind('.'); dot != std::string::npos) {
            pipeline_probe::dump_code(entry.substr(0, dot).c_str(), entry.substr(dot + 1).c_str());
        }

        start = end + 1;
    }

    if (!g_settings.probe_words.empty()) {
        std::vector<std::string> words{};

        for (size_t start = 0; start <= g_settings.probe_words.size();) {
            const auto end = std::min(g_settings.probe_words.find(',', start), g_settings.probe_words.size());

            if (end > start) {
                words.push_back(g_settings.probe_words.substr(start, end - start));
            }

            start = end + 1;
        }

        std::vector<const char*> pointers{};

        for (const auto& w : words) {
            pointers.push_back(w.c_str());
        }

        pipeline_probe::find_methods(pointers.data(), pointers.size());
    }

    if (g_settings.probe_methods) {
        static const char* const words[] = {"Bias", "Lod", "LOD", "Mip"};
        pipeline_probe::find_methods(words, std::size(words));
        pipeline_probe::dump_code("via.render.layer.Scene", "get_LODBias");
        pipeline_probe::dump_code("via.render.Renderer", "useLegacyTextureMipBias");
    }

    if (!engine_hooks::install()) {
        log_error("Some engine hooks could not be installed (see above): DLSS unavailable");
    }

    g_sharpening_mod = find_sharpening_mod();
    log_info("Engine sharpening while DLSS is on: {}", g_settings.dlss_sharpening_off == 0 ? "left alone (dlss_sharpening_off=0)"
        : g_settings.dlss_sharpening_off == 2           ? "off (dlss_sharpening_off=2)"
        : g_sharpening_mod.empty()                      ? "off (no other mod removes it)"
                                                        : std::format("left to {} (another mod removing it)", g_sharpening_mod));

    // Diagnostics: where the game's DXGI factory imports point now (another mod may have replaced our import hooks).
    for (const auto name : {"CreateDXGIFactory", "CreateDXGIFactory1", "CreateDXGIFactory2"}) {
        if (const auto slot = find_import_slot(GetModuleHandleW(nullptr), "dxgi.dll", name); slot != nullptr) {
            HMODULE module{};
            wchar_t path[MAX_PATH]{};
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)*slot, &module);
            GetModuleFileNameW(module, path, MAX_PATH);
            log_info("Game import {} -> {:x} ({})", name, (uintptr_t)*slot, std::filesystem::path(path).filename().string());
        }
    }

    if (g_proxy_swapchain != nullptr) {
        log_info("The game's swapchain goes through Streamline: frame generation available");
    } else if (g_settings.enabled) {
        log_warn("The game's swapchain was not created through Streamline (created before the plugin loaded?): frame generation unavailable");
        report_error("The game's swapchain was created before the plugin loaded (another mod creating it early?)");
    }

    return true;
}
} // namespace framegen

namespace framegen {
const std::string& game() {
    return g_game;
}

const Dlss* dlss() {
    static Dlss dlss{};

    if (!g_sl.initialized || !g_sl.dlss_supported || !g_settings.dlss) {
        return nullptr;
    }

    dlss.set_constants = g_sl.set_constants;
    dlss.set_tag_for_frame = g_sl.set_tag_for_frame;
    dlss.evaluate_feature = g_sl.evaluate_feature;
    dlss.get_optimal_settings = g_sl.dlss_get_optimal_settings;
    dlss.set_options = g_sl.dlss_set_options;
    dlss.viewport = g_viewport;
    dlss.mode = dlss_pass::examine_dlaa() ? 0 : g_settings.dlss_mode; // RE2: DLAA while examining items (dlss_examine_dlaa)
    // A preset forced in the NVIDIA app (one letter) is passed to DLSS here too: through Streamline the driver override
    // alone did not reach DLSS (Performance kept DLSS's own default, the heavier M, with K forced: 1.06 vs 0.70 ms).
    // "Latest" needs nothing: it is DLSS's default per mode anyway.
    dlss.preset = g_nv_sr_override >= 1 && g_nv_sr_override <= 26 ? (int)g_nv_sr_override : g_settings.dlss_preset;
    dlss.debug = g_settings.dlss_debug;
    dlss.bloom = g_settings.dlss_bloom;
    dlss.lod_bias = g_settings.dlss_lod_bias;
    dlss.lod_bias_offset = g_settings.dlss_lod_bias_offset;
    dlss.skip_draws = g_settings.dlss_skip_draws;
    dlss.guard = g_settings.dlss_guard;
    dlss.bloom_stabilize = g_settings.dlss_bloom_stabilize;
    dlss.bloom_alpha = g_settings.dlss_bloom_alpha;
    dlss.bloom_moving = g_settings.dlss_bloom_moving;
    dlss.lod_bias_key = g_settings.debug_keys ? g_settings.dlss_lod_bias_key : 0;
    dlss.toggle_key = g_settings.debug_keys ? g_settings.dlss_toggle_key : 0;
    dlss.hud_fix = g_settings.dlss_hud_fix;
    dlss.fg_enabled = g_settings.enabled;
    dlss.cut_reset = g_settings.dlss_cut_reset;
    dlss.hud_fix_key = g_settings.debug_keys ? g_settings.hud_fix_key : 0;
    dlss.sharpening_off = g_settings.dlss_sharpening_off == 2 || (g_settings.dlss_sharpening_off == 1 && g_sharpening_mod.empty());
    dlss.engine_aa = g_settings.dlss_engine_aa;
    dlss.dither_test = g_settings.dlss_dither_test;
    dlss.no_upscale_fallback = g_settings.dlss_no_upscale_fallback;
    dlss.examine_dlaa = g_settings.dlss_examine_dlaa;
    dlss.guard_log = g_settings.dlss_guard_log;
    dlss.rate_override = g_settings.dlss_rate_override;
    dlss.half_limit = g_settings.dlss_half_limit;
    dlss.guard_live = g_settings.dlss_guard_live;
    dlss.native_path = g_settings.dlss_native_path;
    dlss.examine_stabilize = g_settings.dlss_examine_stabilize;
    dlss.examine_blend = g_settings.dlss_examine_blend;
    dlss.examine_no_jitter = g_settings.dlss_examine_no_jitter;
    dlss.compute_input = g_settings.dlss_compute_input;
    dlss.bloom_transfer = g_settings.dlss_bloom_transfer;
    dlss.own_motion_blur = g_settings.dlss_own_motion_blur;
    dlss.own_bloom = g_settings.dlss_own_bloom;
    dlss.own_bloom_strength = g_settings.dlss_own_bloom_strength;
    dlss.own_bloom_add = g_settings.dlss_own_bloom_add;
    dlss.own_bloom_threshold = g_settings.dlss_own_bloom_threshold;
    dlss.own_bloom_inner = g_settings.dlss_own_bloom_inner;
    dlss.tape_bicubic = g_settings.dlss_tape_scaler == 1;
    dlss.tape_sharpening = g_settings.dlss_tape_sharpening;
    dlss.tape_off = g_settings.dlss_tape_off;
    dlss.own_dof = g_settings.dlss_own_dof;
    dlss.dof_scale = g_settings.dlss_dof_scale;
    dlss.cas = g_settings.dlss_cas;
    dlss.cas_strength = g_settings.dlss_cas_strength;
    dlss.probe_dof = g_settings.probe_dof;
    dlss.dof_temporal = g_settings.dlss_dof_temporal;
    dlss.dof_max_scale = g_settings.dlss_dof_max_scale;
    return &dlss;
}

sl::FrameToken* render_frame_token() {
    if (auto token = g_pending_token.load()) {
        return token;
    }

    // The game hasn't finished this frame's submission yet: a token for it now, which its Present then uses (unless
    // the game's own one arrives first, then the two features use different tokens for this frame, which is harmless).
    sl::FrameToken* expected{};
    const auto token = new_frame_token();

    if (token != nullptr && !g_pending_token.compare_exchange_strong(expected, token)) {
        return expected;
    }

    return token;
}
} // namespace framegen

namespace framegen {
void constants_set(sl::FrameToken* token) {
    g_constants_token = token;
}
} // namespace framegen

namespace framegen {
void set_frame_inputs(const FrameInputs& inputs) {
    std::scoped_lock _{g_frame_inputs_mutex};
    g_frame_inputs = inputs;
    g_frame_inputs_fresh = true;
}
} // namespace framegen

namespace framegen {
std::string describe_backbuffers() {
    const auto swapchain = g_proxy_swapchain.load();
    std::string out{};

    for (UINT i = 0; swapchain != nullptr && i < 8; ++i) {
        ComPtr<ID3D12Resource> buffer{};

        if (FAILED(swapchain->GetBuffer(i, IID_PPV_ARGS(&buffer)))) {
            break;
        }

        const auto desc = buffer->GetDesc();
        out += std::format("{}{:x} ({}x{} fmt {})", out.empty() ? "" : ", ", (uintptr_t)buffer.Get(), desc.Width, desc.Height, (int)desc.Format);
    }

    return out.empty() ? "none" : out;
}
} // namespace framegen

namespace framegen {
ComPtr<ID3D12Resource> current_backbuffer() {
    const auto swapchain = g_proxy_swapchain.load();
    ComPtr<ID3D12Resource> buffer{};

    if (swapchain != nullptr) {
        swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&buffer));
    }

    return buffer;
}

void set_hudless(ID3D12Resource* hudless, uint32_t state) {
    std::scoped_lock _{g_frame_inputs_mutex};
    g_hudless = hudless;
    g_hudless_state = state;
    g_hudless_fresh = true;
}
} // namespace framegen
