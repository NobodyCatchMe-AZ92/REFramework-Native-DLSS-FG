#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <array>
#include <cstring>
#include <format>
#include <set>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <numeric>

#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss.h>

#include "bloom_composite.hpp"
#include "upscale_guard.hpp"
#include "bloom_stabilizer.hpp"
#include "gpu_timer.hpp"
#include "game_profile.hpp"
#include "cl_state.hpp"
#include "scale_blit.hpp"
#include "own_dof.hpp"
#include "examine_stabilizer.hpp"
#include "bloom_transfer.hpp"
#include "motion_blur.hpp"
#include "own_bloom.hpp"
#include "cas_sharpen.hpp"
#include "re2_dither.hpp"
#include "descriptor_map.hpp"
#include "engine_hooks.hpp"
#include "hud_split.hpp"
#include "framegen.hpp"
#include "log.hpp"
#include "mat4.hpp"
#include "refw.hpp"
#include "render_objects.hpp"
#include "scene_inputs.hpp"

#include "dlss_pass.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// --------------------------------------------------------------------------------------------------------------------
// Code patterns in the game's executable ("?" = any byte).
std::optional<uintptr_t> scan(HMODULE module, std::string_view pattern) {
    std::vector<int> bytes{};

    for (size_t i = 0; i < pattern.size();) {
        if (pattern[i] == ' ') {
            ++i;
        } else if (pattern[i] == '?') {
            bytes.push_back(-1);
            i += (i + 1 < pattern.size() && pattern[i + 1] == '?') ? 2 : 1;
        } else {
            bytes.push_back(std::stoi(std::string{pattern.substr(i, 2)}, nullptr, 16));
            i += 2;
        }
    }

    const auto base = (const uint8_t*)module;
    const auto nt = (const IMAGE_NT_HEADERS*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    for (WORD s = 0; s < nt->FileHeader.NumberOfSections; ++s, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const auto start = base + section->VirtualAddress;
        const auto size = (size_t)section->Misc.VirtualSize;

        for (size_t i = 0; i + bytes.size() <= size; ++i) {
            size_t j = 0;

            while (j < bytes.size() && (bytes[j] < 0 || start[i + j] == (uint8_t)bytes[j])) {
                ++j;
            }

            if (j == bytes.size()) {
                return (uintptr_t)(start + i);
            }
        }
    }

    return std::nullopt;
}

// --------------------------------------------------------------------------------------------------------------------
// Engine commands. A render context allocates each command (type id from the type database enum
// via.render.command.TypeId) in its command buffer; the engine turns them into D3D12 calls later.
using AllocFn = uint8_t* (*)(void* render_context, uint32_t type, uint32_t size);
AllocFn g_alloc{};
uint32_t g_clear_type{};
bool g_engine_ready{};

// The Clear command (TDB 70): header 0x10, clear type +0x10, target state +0x18, view +0x20, color +0x28.
constexpr uint32_t CLEAR_SIZE = 0x38;
constexpr size_t RENDER_CONTEXT_TARGET_STATE = 0x98; // the render context's current target state

// A color the engine never clears to: the marker. The 4th value carries the slot of the frame's inputs.
constexpr float MARKER[4] = {-12345.0f, 6789.0f, -2468.0f, 1000.0f};
constexpr uint32_t SLOTS = 4;

bool find_engine_functions() {
    const auto game = GetModuleHandleW(nullptr);

    // Same pattern REFramework uses: "mov rcx, ?; lea r8d, [rdx+38h]; call alloc" (allocating a 0x38 command).
    if (const auto ref = scan(game, "48 8B ? 44 8D 42 38 E8 ? ? ? ?")) {
        const auto call = *ref + 7;
        g_alloc = (AllocFn)(call + 5 + *(const int32_t*)(call + 1));
    }

    const auto field = refw::sdk()->tdb->find_field(refw::sdk()->functions->get_tdb(), "via.render.command.TypeId", "Clear");
    const auto data = field != nullptr ? refw::sdk()->field->get_data_raw(field, nullptr, false) : nullptr;

    if (data != nullptr) {
        g_clear_type = *(const uint32_t*)data;
    }

    log_info("Engine commands: alloc {:x}, Clear type {}", (uintptr_t)g_alloc, data != nullptr ? (int)g_clear_type : -1);
    return g_alloc != nullptr && data != nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// Per frame: the camera (from the Scene update) and the inputs (at PrePostEffect), handed to the marker by slot.
struct Camera {
    bool valid{};
    Mat4 view{};
    Mat4 projection{}; // without jitter
    Mat4 prev_view{};
    Mat4 prev_projection{};
    float jitter[2]{}; // Streamline's jitter offset, render pixels
    int dither_shift[2]{}; // RE2 [EXPERIMENTAL] dither tests: whole render pixels in the projection only; always 0 elsewhere
    int dither_prev_shift[2]{}; // the same, last frame's (the bloom stabilizer's history follows the change)
    float near_plane{0.1f};
    float far_plane{1000.0f};
    bool cut{}; // a camera cut (or a new scene): DLSS and DLSS-G drop their history this frame
};

struct Slot {
    Camera camera{};
    ID3D12Resource* color{};
    ID3D12Resource* depth{};
    ID3D12Resource* motion{};
    ID3D12Resource* target{};
    bool pre_copied{}; // the scene before bloom was copied into g_pre at the marker
};

std::mutex g_mutex{};
Camera g_camera{};
Slot g_slots[SLOTS]{};
uint32_t g_next_slot{};

ComPtr<ID3D12Device> g_device{};
std::atomic<bool> g_ready{}; // device + command list state tracking

// Sizes and render scale, shared between the threads.
std::atomic<uint32_t> g_render_size[2]{};
std::atomic<uint32_t> g_seen_render_size[2]{}; // the size the engine renders at, every frame (also ones DLSS skips)
std::atomic<uint32_t> g_output_size[2]{};
std::atomic<float> g_wanted_rate{0.0f};

// The engine's full resolution scene target (DLSS's output is the same size, except in videotape mode).
D3D12_RESOURCE_DESC g_target_desc{};

// Profile videotape_mode (RE7): the scene renders at 2/3 of the render scale (its videotape segments). While it lasts the
// game's TAA is kept on (only then does that scene have an upscale step), DLSS outputs the 2/3 size and the plugin scales
// it to the scene target; the plugin's jitter replaces the engine's (built from its unjittered projection).
std::atomic<bool> g_tape{};
int g_tape_votes{}; // consecutive frames that disagree with g_tape (switches after 3)
std::atomic<float> g_applied_rate{-1.0f}; // the render scale the plugin last set (the engine's getter can disagree with what it renders)
std::atomic<float> g_original_rate{-1.0f}; // the game's render scale before the plugin first changed it (-1 = unchanged)

// Counters for the status line.
std::atomic<uint32_t> g_evaluated{};
std::atomic<uint32_t> g_skipped{};
std::atomic<uint32_t> g_markers{};
std::atomic<const char*> g_skip_reason{"-"};
std::atomic<bool> g_working{}; // the last marker was upscaled: jitter only then (else the engine's own upscale shows it)
std::atomic<bool> g_lod_bias_paused{}; // toggled live with the LOD bias key (comparing with the game's own)
std::atomic<bool> g_dlss_paused{};     // toggled live with the DLSS key: the engine's own upscale, to compare
std::atomic<bool> g_examine_dlaa{};    // RE2 item examine with setting dlss_examine_dlaa: DLSS's mode is DLAA there
// RE7 (profile own_motion_blur): the game's motion blur option (-1 unknown), whether the plugin's runs this frame and its
// shutter share (ShutterAngle / 360).
std::atomic<bool> g_own_motion_blur{};
std::atomic<float> g_motion_blur_shutter{0.5f};
// RE8 (profile own_bloom): whether the plugin's bloom runs this frame, and the game's bloom settings (game thread -> render).
std::atomic<bool> g_own_bloom{};
std::atomic<float> g_bloom_threshold{1.0f};
std::atomic<float> g_bloom_dispersion{0.5f};
std::atomic<float> g_bloom_ratio{0.0f};
std::atomic<bool> g_live_rule_screen{}; // RE2: the item box screen shows (GUI_ItemBox): the guard's live rule applies only then
// RE2's item-use animations (profile missing_upscale_fallback): below 100% the engine skips its upscale step on most
// frames there (the picture froze, unmodded too). The frame number at each scene marker and at the last upscale call; if
// the output stage comes without one, a marker there runs DLSS into the scene target.
std::atomic<uint32_t> g_marker_frame{};
std::atomic<uint32_t> g_upscale_call_frame{};
std::atomic<uint32_t> g_marker_index{};
std::atomic<uint32_t> g_fallback_upscales{};
std::atomic<uint32_t> g_real_upscale_frame{}; // the marker frame of the last upscale call that wasn't the fallback's
thread_local bool t_forced_replace{}; // the fallback's DLSS: replace the scene target (no guard)
thread_local D3D12_RESOURCE_STATES t_forced_source_state{}; // its source's state (the last render target's, or INPUT_STATE)
thread_local bool t_native_path{};     // this upscale call is RE2's typewriter path (dlss_native_path): DLSS from the scene color
thread_local ID3D12Resource* t_engine_input{};   // RE4: the compute upscale's own input given to DLSS (dlss_compute_input)
thread_local D3D12_RESOURCE_STATES t_engine_input_state{};
std::atomic<uint32_t> g_cuts{};          // camera cuts since the last status line
uint32_t g_cuts_logged{};
std::atomic<const char*> g_cut_reason{"-"};

void skip(const char* reason) {
    ++g_skipped;
    g_working = false;
    g_skip_reason = reason;
}

bool active() {
    return g_ready && framegen::dlss() != nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// The Streamline side (on the thread recording the command list with the marker).
// Textures replaced (new size or format, e.g. a DLSS mode change) may still be in use by the GPU for a few frames: they
// are kept that long, then released. (Kept forever before: about ten textures per mode change, VRAM grew with each.)
constexpr uint64_t RETIRED_FRAMES = 8;

struct RetiredTexture {
    ComPtr<ID3D12Resource> texture{};
    uint64_t frame{};
};

std::mutex g_retired_mutex{};
std::vector<RetiredTexture> g_retired{};
std::atomic<uint64_t> g_frame_number{}; // DLSS markers seen (one per frame)

void retire(ComPtr<ID3D12Resource>& texture) {
    std::scoped_lock _{g_retired_mutex};
    g_retired.push_back({texture, g_frame_number.load()});
    texture.Reset();
}

void release_retired() {
    const auto now = g_frame_number.load();
    std::scoped_lock _{g_retired_mutex};
    std::erase_if(g_retired, [now](const RetiredTexture& r) { return r.frame + RETIRED_FRAMES < now; });
}

struct Output {
    ComPtr<ID3D12Resource> texture{};
    D3D12_RESOURCE_DESC desc{};
    int mode{-1};
    int preset{-1};
    bool tape_off{}; // configured with setting dlss_tape_off in videotape mode (native render scale, no DLSS)
    float rate_override{-1.0f}; // configured with setting dlss_rate_override (diagnostics)
    bool half_limit{true};      // configured with the half-size limit active (RE7's title; setting dlss_half_limit)
    bool configured{};
    uint32_t render_min[2]{};
    uint32_t render_max[2]{};
    uint32_t render_expected[2]{}; // the render size the mode's render scale gives (DLSS's input from then on)
    uint32_t early_frames{};       // frames skipped since the mode change because the engine hadn't applied it yet
    bool reset{true};
} g_output{};

// Option A (dlss_bloom): the scene before the engine's bloom (copied at the marker, render size, DLSS's input) and the
// result with the bloom put back (full size). g_pre stays in NON_PIXEL_SHADER_RESOURCE, g_final in UNORDERED_ACCESS.
ComPtr<ID3D12Resource> g_pre{};
ComPtr<ID3D12Resource> g_final{};
ComPtr<ID3D12Resource> g_tape_full{}; // videotape mode: DLSS's 2/3-size output scaled to the scene target (UNORDERED_ACCESS)
ComPtr<ID3D12Resource> g_dof_out{}; // the plugin's depth of field on DLSS's result (profile own_dof; UNORDERED_ACCESS)
ID3D12Resource* g_result{}; // what is copied into the scene target this frame (g_final or g_output.texture)
ComPtr<ID3D12Resource> g_bloom_history[2]{}; // the bloom stabilizer's output, this frame's and the previous one (ping-pong)
ComPtr<ID3D12Resource> g_bloom_backup{};     // the scene as the engine rendered it, put back before its bloom is added
uint32_t g_bloom_frame{};
bool g_bloom_valid{};                         // the history holds a previous frame of this size
std::atomic<bool> g_bloom_restore_pending{}; // the stabilized scene is in the scene color until the bloom draw puts the original back
ComPtr<ID3D12Resource> g_guarded{}; // the upscale guard's output: DLSS where the engine's draw was an upscale, else the engine's

bool ensure_texture(ComPtr<ID3D12Resource>& texture, const D3D12_RESOURCE_DESC& like, D3D12_RESOURCE_STATES state, const wchar_t* name) {
    if (texture != nullptr) {
        const auto desc = texture->GetDesc();

        if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
            return true;
        }

        retire(texture);
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = like.Width;
    desc.Height = like.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = like.Format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&texture)))) {
        log_error("DLSS: could not create {}x{} texture (format {})", like.Width, like.Height, (int)like.Format);
        return false;
    }

    texture->SetName(name);
    return true;
}

constexpr auto INPUT_STATE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto DEPTH_STATE = INPUT_STATE | D3D12_RESOURCE_STATE_DEPTH_READ;

// RE7 (profile min_half_render / min_half_object): its title scene loaded (the TitleScreen GUI object exists), checked with
// the other GUI screens every 10th frame. True until the first check (the game starts on its title).
std::atomic<bool> g_half_screen{true};

bool half_limit_active(const framegen::Dlss& dlss);

bool half_limit_active(const framegen::Dlss& dlss) {
    const auto& profile = game_profile::current();
    return profile.min_half_render && dlss.half_limit && (profile.min_half_object == nullptr || g_half_screen.load());
}

sl::DLSSMode to_sl_mode(int mode) {
    switch (mode) {
    case 0: return sl::DLSSMode::eDLAA;
    case 1: return sl::DLSSMode::eMaxQuality;
    case 2: return sl::DLSSMode::eBalanced;
    case 3: return sl::DLSSMode::eMaxPerformance;
    case 4: return sl::DLSSMode::eUltraPerformance;
    default: return sl::DLSSMode::eMaxQuality;
    }
}

// Our output texture and DLSS's options, redone when the output (the engine's full resolution scene target) changes.
bool configure(const framegen::Dlss& dlss, ID3D12Resource* target) {
    g_target_desc = target->GetDesc();
    auto desc = g_target_desc;

    // Videotape mode (profile videotape_mode, RE7): the game renders those scenes at 2/3 of the render scale and upscales
    // them to the full size itself; DLSS outputs that 2/3 size (the tape's own resolution, its look), and the plugin
    // scales it to the scene target as the game's step did. The render scale stays the mode's.
    if (g_tape.load()) {
        desc.Width = (UINT64)std::lround((double)desc.Width * 2.0 / 3.0);
        desc.Height = (UINT)std::lround((double)desc.Height * 2.0 / 3.0);
    }

    if (g_output.configured && desc.Width == g_output.desc.Width && desc.Height == g_output.desc.Height && desc.Format == g_output.desc.Format &&
        dlss.mode == g_output.mode && dlss.preset == g_output.preset && dlss.tape_off == g_output.tape_off && dlss.rate_override == g_output.rate_override &&
        half_limit_active(dlss) == g_output.half_limit)
    {
        return g_output.texture != nullptr;
    }

    g_output.configured = true;
    g_output.desc = desc;
    g_output.mode = dlss.mode;
    g_output.preset = dlss.preset;
    g_output.tape_off = dlss.tape_off;
    g_output.rate_override = dlss.rate_override;
    g_output.half_limit = half_limit_active(dlss);
    g_output.reset = true;

    if (g_output.texture != nullptr) {
        retire(g_output.texture);
    }

    D3D12_RESOURCE_DESC out{};
    out.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    out.Width = desc.Width;
    out.Height = desc.Height;
    out.DepthOrArraySize = 1;
    out.MipLevels = 1;
    out.Format = desc.Format;
    out.SampleDesc.Count = 1;
    out.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&g_output.texture))))
    {
        log_error("DLSS: could not create the {}x{} output texture (format {})", desc.Width, desc.Height, (int)desc.Format);
        return false;
    }

    g_output.texture->SetName(L"refw_framegen DLSS output");

    sl::DLSSOptions options{};
    // DLAA: at a render scale of exactly 1 the engine skips its upscale step (no upscale draw to replace). With the
    // profile's native_dlaa the render scale is a hair above 1 (exactly the output size, but the engine's scaled path) and
    // DLSS runs in its DLAA mode; otherwise "DLAA" renders one pixel below native, which DLSS's Quality mode takes as a
    // dynamic resolution (DLAA in practice; the fork did the same).
    const auto dlaa = dlss.mode == 0;
    const auto native = dlaa && game_profile::current().native_dlaa;
    options.mode = native ? sl::DLSSMode::eDLAA : dlaa ? sl::DLSSMode::eMaxQuality : to_sl_mode(dlss.mode);

    // RE7 (profile min_half_render): never below half the output, so Ultra Performance runs as Performance (DLSS's Ultra
    // Performance mode doesn't take a half-size input).
    if (g_output.half_limit && dlss.mode == 4) {
        options.mode = sl::DLSSMode::eMaxPerformance;
        log_info("DLSS: Ultra Performance runs as Performance in this game (its picture freezes below half the output)");
    }
    options.outputWidth = (uint32_t)desc.Width;
    options.outputHeight = desc.Height;
    options.colorBuffersHDR = sl::Boolean::eTrue; // the scene before tonemapping
    options.useAutoExposure = sl::Boolean::eTrue;
    options.exposureScale = 1.0f;

    if (dlss.preset > 0) {
        const auto preset = (sl::DLSSPreset)dlss.preset;
        options.dlaaPreset = options.qualityPreset = options.balancedPreset = options.performancePreset = options.ultraPerformancePreset =
            options.ultraQualityPreset = preset;
    }

    sl::DLSSOptimalSettings settings{};

    if (const auto result = dlss.get_optimal_settings(options, settings); result != sl::Result::eOk || settings.optimalRenderWidth == 0) {
        log_error("DLSS: slDLSSGetOptimalSettings failed: {}", (uint32_t)result);
        g_output.texture.Reset();
        return false;
    }

    if (const auto result = dlss.set_options(dlss.viewport, options); result != sl::Result::eOk) {
        log_error("DLSS: slDLSSSetOptions failed: {}", (uint32_t)result);
        g_output.texture.Reset();
        return false;
    }

    // The engine's render scale is per axis.
    // Native: a hair above 1 (rounds to exactly the output size): at exactly 1.0 RE2 renders at its own Image Quality
    // option instead (1280x720 here) and skips its upscale step.
    // Videotape mode with dlss_tape_off: native (the full size's native rate), DLSS skipped there, as vanilla.
    auto rate = g_tape.load() && dlss.tape_off ? 1.0f + 0.25f / (float)g_target_desc.Height
        : native ? 1.0f + 0.25f / (float)desc.Height : dlaa ? (float)(desc.Height - 1) / (float)desc.Height : (float)settings.optimalRenderWidth / (float)desc.Width;

    // RE7 (profile whole_render_size): with its TAA off (DLSS) a render size of half a pixel (1680x945 at 0.5: 840x472.5)
    // froze its picture black. The nearest scale at or above the mode's where both sides are whole numbers (k / gcd(W, H):
    // 53/105 -> 848x477 there), within 2% and DLSS's range; 1920x1080 and 2560x1440 at 0.5 are whole already.
    if (game_profile::current().whole_render_size && !native && !dlaa && !(g_tape.load() && dlss.tape_off)) {
        const auto w = (uint32_t)desc.Width;
        const auto h = desc.Height;
        const auto g = std::gcd(w, h);
        const auto k = std::ceil(rate * (float)g - 1e-4f);
        const auto snapped = k / (float)g;
        const auto sw = (uint32_t)std::lround(snapped * (float)w);
        const auto sh = (uint32_t)std::lround(snapped * (float)h);

        if (snapped != rate && snapped <= rate * 1.02f && sw >= settings.renderWidthMin && sh >= settings.renderHeightMin && sw <= settings.renderWidthMax &&
            sh <= settings.renderHeightMax)
        {
            log_info("DLSS: render scale {:.4f} -> {:.4f} ({}x{}, whole pixels)", rate, snapped, sw, sh);
            rate = snapped;
        }
    }

    // RE7 (profile min_half_render): at least half the output on both sides once the engine rounds (it rounds down: the
    // half rounded up, plus a quarter pixel). Performance at odd sizes, and Ultra Performance, land there.
    if (g_output.half_limit && !native && !dlaa && !(g_tape.load() && dlss.tape_off)) {
        const auto w = (float)desc.Width;
        const auto h = (float)desc.Height;
        const auto half = std::max((std::ceil(w / 2.0f) + 0.25f) / w, (std::ceil(h / 2.0f) + 0.25f) / h);

        if (rate < half) {
            log_info("DLSS: render scale {:.4f} -> {:.4f} (RE7: not below half the output, {}x{})", rate, half, (uint32_t)std::ceil(w / 2.0f),
                (uint32_t)std::ceil(h / 2.0f));
            rate = half;
        }
    }

    if (dlss.rate_override > 0.0f && !native && !dlaa) { // diagnostics
        log_info("DLSS: render scale {:.4f} -> {:.4f} (dlss_rate_override)", rate, dlss.rate_override);
        rate = dlss.rate_override;
    }

    if (dlaa && !native && (settings.renderWidthMax < desc.Width - 2 || settings.renderHeightMax < desc.Height - 1)) {
        log_warn("DLSS: DLAA at native - 1 is outside DLSS's range ({}x{} max)", settings.renderWidthMax, settings.renderHeightMax);
    }
    g_wanted_rate = rate;
    g_output.render_expected[0] = std::min((uint32_t)std::lround(rate * (float)desc.Width), (uint32_t)desc.Width);
    g_output.render_expected[1] = std::min((uint32_t)std::lround(rate * (float)desc.Height), desc.Height);
    g_output.early_frames = 0;
    g_output.render_min[0] = settings.renderWidthMin;
    g_output.render_min[1] = settings.renderHeightMin;
    g_output.render_max[0] = settings.renderWidthMax;
    g_output.render_max[1] = settings.renderHeightMax;
    g_output_size[0] = (uint32_t)desc.Width;
    g_output_size[1] = desc.Height;

    log_info("DLSS: mode {} preset {} for {}x{}: render {}x{} (render scale {:.3f}, allowed {}x{} - {}x{})", dlss.mode, dlss.preset, desc.Width,
        desc.Height, settings.optimalRenderWidth, settings.optimalRenderHeight, rate, settings.renderWidthMin, settings.renderHeightMin,
        settings.renderWidthMax, settings.renderHeightMax);
    return true;
}

void copy_matrix(sl::float4x4& out, const Mat4& in) {
    static_assert(sizeof(sl::float4x4) == sizeof(Mat4));
    std::memcpy(&out, &in, sizeof(out));
}

sl::Constants set_constants(const framegen::Dlss& dlss, const sl::FrameToken& token, const Camera& camera, uint32_t render_w,
    uint32_t render_h, bool reset)
{
    sl::Constants c{};
    const auto clip_to_view = camera.projection.inverse();
    const auto view_to_world = camera.view.inverse();

    // clip -> view -> world -> previous view -> previous clip.
    const auto clip_to_prev_clip = camera.prev_projection * camera.prev_view * view_to_world * clip_to_view;

    copy_matrix(c.cameraViewToClip, camera.projection);
    copy_matrix(c.clipToCameraView, clip_to_view);
    copy_matrix(c.clipToPrevClip, clip_to_prev_clip);
    copy_matrix(c.prevClipToClip, clip_to_prev_clip.inverse());

    const auto& w = view_to_world.m;
    c.cameraRight = {w[0][0], w[0][1], w[0][2]};
    c.cameraUp = {w[1][0], w[1][1], w[1][2]};
    c.cameraFwd = {-w[2][0], -w[2][1], -w[2][2]};
    c.cameraPos = {w[3][0], w[3][1], w[3][2]};

    c.jitterOffset = {camera.jitter[0], camera.jitter[1]};
    c.mvecScale = {0.5f, -0.5f}; // the engine's velocity is in clip space units (y up)
    c.cameraPinholeOffset = {0.0f, 0.0f};
    c.cameraNear = camera.near_plane;
    c.cameraFar = camera.far_plane;
    c.cameraFOV = 2.0f * std::atan(1.0f / camera.projection.m[1][1]);
    c.cameraAspectRatio = (float)render_w / (float)render_h;
    c.depthInverted = sl::Boolean::eTrue;
    c.cameraMotionIncluded = sl::Boolean::eTrue;
    c.motionVectors3D = sl::Boolean::eFalse;
    c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    c.orthographicProjection = sl::Boolean::eFalse;
    c.motionVectorsDilated = sl::Boolean::eFalse;
    c.motionVectorsJittered = sl::Boolean::eFalse;

    dlss.set_constants(c, token, dlss.viewport);
    return c;
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

// --------------------------------------------------------------------------------------------------------------------
// Diagnostics (dlss_debug=5): the velocity and depth DLSS gets, copied to the CPU once and summarized in the log.
struct Readback {
    ComPtr<ID3D12Resource> buffer{};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows{};
    UINT64 row_size{};
    DXGI_FORMAT format{};
    const char* name{};
};

Readback g_readbacks[2]{};
void decode_r11g11b10(uint32_t texel, float rgb[3]);
uint32_t g_readback_render[2]{}; // dlss_debug=11: the render size (the region of the color target the scene was drawn into)
std::atomic<int> g_readback_frames{-1}; // frames until the copies are read, -1 = none pending

void record_readback(ID3D12GraphicsCommandList* list, Readback& rb, ID3D12Resource* resource, D3D12_RESOURCE_STATES state, const char* name) {
    const auto desc = resource->GetDesc();
    UINT64 total{};
    g_device->GetCopyableFootprints(&desc, 0, 1, 0, &rb.footprint, &rb.rows, &rb.row_size, &total);

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb.buffer)))) {
        log_error("readback: could not create a buffer for {}", name);
        return;
    }

    rb.format = rb.footprint.Footprint.Format;
    rb.name = name;
    barrier(list, resource, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{rb.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = rb.footprint;
    D3D12_TEXTURE_COPY_LOCATION src{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(list, resource, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    log_info("readback: {} {}x{} format {} (footprint format {}) queued", name, desc.Width, desc.Height, (int)desc.Format, (int)rb.format);
}

// Called with each status line: the copies are read at the one after they were queued (seconds later, long done).
void finish_readbacks() {
    const auto left = g_readback_frames.load();

    if (left < 0) {
        return;
    }

    if (left > 0) {
        g_readback_frames = left - 1;
        return;
    }

    g_readback_frames = -1;

    for (auto& rb : g_readbacks) {
        if (rb.buffer == nullptr) {
            continue;
        }

        uint8_t* data{};
        if (FAILED(rb.buffer->Map(0, nullptr, (void**)&data))) {
            log_error("readback: map failed for {}", rb.name);
            continue;
        }

        const auto w = rb.footprint.Footprint.Width;
        const auto h = rb.rows;
        const auto pitch = rb.footprint.Footprint.RowPitch;

        if (std::string_view{rb.name} == "color" && rb.format == DXGI_FORMAT_R11G11B10_FLOAT) {
            // dlss_debug=11: the scene color in the typewriter path; inside the render region vs outside it.
            const auto rw = std::min<UINT>(g_readback_render[0], w);
            const auto rh = std::min<UINT>(g_readback_render[1], h);
            double inside{};
            double outside{};
            uint64_t inside_n{};
            uint64_t outside_n{};
            uint64_t inside_lit{};
            uint64_t outside_lit{};

            for (UINT y = 0; y < h; y += 2) {
                const auto row = (const uint32_t*)(data + rb.footprint.Offset + (UINT64)y * pitch);

                for (UINT x = 0; x < w; x += 2) {
                    float rgb[3];
                    decode_r11g11b10(row[x], rgb);
                    const auto l = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];

                    if (x < rw && y < rh) {
                        inside += l;
                        ++inside_n;
                        inside_lit += l > 0.001;
                    } else {
                        outside += l;
                        ++outside_n;
                        outside_lit += l > 0.001;
                    }
                }
            }

            log_info("readback: scene color {}x{}, render region {}x{}: inside mean {:.4f} ({:.1f}% lit), outside mean {:.4f} ({:.1f}% lit)", w, h,
                rw, rh, inside / std::max<uint64_t>(1, inside_n), 100.0 * inside_lit / std::max<uint64_t>(1, inside_n),
                outside / std::max<uint64_t>(1, outside_n), 100.0 * outside_lit / std::max<uint64_t>(1, outside_n));
            rb.buffer->Unmap(0, nullptr);
            rb.buffer.Reset();
            continue;
        }

        if (std::string_view{rb.name} == "velocity") {
            // 4 x SNORM16 per pixel (format 13): x, y, z, w in [-1, 1].
            double sum[4]{};
            double abs_max[4]{};
            uint64_t nonzero[4]{};
            uint64_t count{};

            for (UINT y = 0; y < h; y += 2) {
                const auto row = (const int16_t*)(data + rb.footprint.Offset + (UINT64)y * pitch);

                for (UINT x = 0; x < w; x += 2) {
                    for (int c = 0; c < 4; ++c) {
                        const auto v = std::max(-1.0, row[x * 4 + c] / 32767.0);
                        sum[c] += v;
                        abs_max[c] = std::max(abs_max[c], std::fabs(v));
                        nonzero[c] += row[x * 4 + c] != 0;
                    }

                    ++count;
                }
            }

            for (int c = 0; c < 4; ++c) {
                log_info("readback: velocity channel {}: mean {:.6f}, max |v| {:.6f}, nonzero {:.1f}% (in render pixels: mean {:.3f})", c,
                    sum[c] / count, abs_max[c], 100.0 * nonzero[c] / count, sum[c] / count * (c == 0 ? w : h) * 0.5);
            }

            // A few samples across the middle row.
            std::string samples{};
            // The middle of the screen (an examined item): how far its pixels move, in render pixels.
            {
                double center_sum{};
                double center_max{};
                uint64_t center_moving{};
                uint64_t center_count{};

                for (UINT y = h / 4; y < h * 3 / 4; ++y) {
                    const auto r = (const int16_t*)(data + rb.footprint.Offset + (UINT64)y * pitch);

                    for (UINT x = w * 3 / 10; x < w * 7 / 10; ++x) {
                        const auto vx = r[x * 4] / 32767.0 * w * 0.5;
                        const auto vy = r[x * 4 + 1] / 32767.0 * h * 0.5;
                        const auto len = std::sqrt(vx * vx + vy * vy);
                        center_sum += len;
                        center_max = std::max(center_max, len);
                        center_moving += len > 0.02;
                        ++center_count;
                    }
                }

                log_info("readback: velocity centre: mean {:.4f} px, max {:.4f} px, {:.1f}% move > 0.02 px", center_sum / std::max<uint64_t>(1, center_count),
                    center_max, 100.0 * center_moving / std::max<uint64_t>(1, center_count));
            }

            const auto row = (const int16_t*)(data + rb.footprint.Offset + (UINT64)(h / 2) * pitch);

            for (UINT x = 0; x < w; x += w / 8) {
                samples += std::format(" ({:.5f},{:.5f})", row[x * 4] / 32767.0, row[x * 4 + 1] / 32767.0);
            }

            log_info("readback: velocity middle row xy:{}", samples);

            // The motion as an image (reframework/data/refw_framegen_velocity.bmp): red = |x|, green = |y| (40 per render
            // pixel), blue marks any motion; to see which moving parts have none.
            {
                std::vector<uint8_t> bmp(54 + (size_t)w * h * 3);
                const auto size = (uint32_t)bmp.size();
                bmp[0] = 'B';
                bmp[1] = 'M';
                std::memcpy(&bmp[2], &size, 4);
                const uint32_t offset = 54, header = 40, pixels = (uint32_t)(w * h * 3);
                const int32_t width = (int32_t)w, height = -(int32_t)h;
                const uint16_t planes = 1, bits = 24;
                std::memcpy(&bmp[10], &offset, 4);
                std::memcpy(&bmp[14], &header, 4);
                std::memcpy(&bmp[18], &width, 4);
                std::memcpy(&bmp[22], &height, 4);
                std::memcpy(&bmp[26], &planes, 2);
                std::memcpy(&bmp[28], &bits, 2);
                std::memcpy(&bmp[34], &pixels, 4);

                for (UINT y = 0; y < h; ++y) {
                    const auto r = (const int16_t*)(data + rb.footprint.Offset + (UINT64)y * pitch);

                    for (UINT x = 0; x < w; ++x) {
                        const auto vx = std::fabs(r[x * 4] / 32767.0 * w * 0.5);
                        const auto vy = std::fabs(r[x * 4 + 1] / 32767.0 * h * 0.5);
                        auto out = &bmp[54 + ((size_t)y * w + x) * 3];
                        out[2] = (uint8_t)std::min(255.0, vx * 40.0);
                        out[1] = (uint8_t)std::min(255.0, vy * 40.0);
                        out[0] = vx + vy > 0.01 ? 96 : 0;
                    }
                }

                wchar_t exe[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                std::wstring path{exe};
                path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"reframework\\data\\refw_framegen_velocity.bmp";

                if (const auto file = _wfopen(path.c_str(), L"wb")) {
                    fwrite(bmp.data(), 1, bmp.size(), file);
                    fclose(file);
                }

                // The other two channels (z red, w green; 0..1 -> 0..255, negative blue): per-object data the game's TAA
                // may use (refw_framegen_velocity_zw.bmp).
                for (UINT y = 0; y < h; ++y) {
                    const auto r = (const int16_t*)(data + rb.footprint.Offset + (UINT64)y * pitch);

                    for (UINT x = 0; x < w; ++x) {
                        const auto z = r[x * 4 + 2] / 32767.0;
                        const auto v = r[x * 4 + 3] / 32767.0;
                        auto out = &bmp[54 + ((size_t)y * w + x) * 3];
                        out[2] = (uint8_t)std::clamp(z * 255.0, 0.0, 255.0);
                        out[1] = (uint8_t)std::clamp(v * 255.0, 0.0, 255.0);
                        out[0] = z < 0.0 || v < 0.0 ? 255 : 0;
                    }
                }

                path = path.substr(0, path.size() - 4) + L"_zw.bmp";

                if (const auto file = _wfopen(path.c_str(), L"wb")) {
                    fwrite(bmp.data(), 1, bmp.size(), file);
                    fclose(file);
                }
            }
        } else {
            // Depth: 32-bit float depth per pixel (the depth plane), inverted (1 near, 0 far).
            float lo = 1e30f;
            float hi = -1e30f;
            double sum{};
            uint64_t count{};

            for (UINT y = 0; y < h; y += 2) {
                const auto row = (const float*)(data + rb.footprint.Offset + (UINT64)y * pitch);
                const auto stride = pitch / std::max<UINT>(1, w) >= 8 ? 2 : 1; // 8-byte texels: depth then stencil/pad

                for (UINT x = 0; x < w; x += 2) {
                    const auto v = row[x * stride];
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                    sum += v;
                    ++count;
                }
            }

            log_info("readback: depth min {:.6f} max {:.6f} mean {:.6f} (pitch {} for width {})", lo, hi, sum / count, pitch, w);
        }

        rb.buffer->Unmap(0, nullptr);
        rb.buffer.Reset();
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Diagnostics (dlss_debug=6): how the engine's post pass maps the scene (with bloom) to its post color, from one frame's
// low-res images read back: the ratio post / lit per brightness range, and how much the bloom adds (lit - pre).
float r11g11b10_channel(uint32_t bits, int mantissa_bits) {
    const auto mantissa = bits & ((1u << mantissa_bits) - 1);
    const auto exponent = (bits >> mantissa_bits) & 31u;

    if (exponent == 0) {
        return std::ldexp((float)mantissa / (float)(1u << mantissa_bits), -14);
    }

    if (exponent == 31) {
        return 65504.0f;
    }

    return std::ldexp(1.0f + (float)mantissa / (float)(1u << mantissa_bits), (int)exponent - 15);
}

void decode_r11g11b10(uint32_t texel, float rgb[3]) {
    rgb[0] = r11g11b10_channel(texel & 0x7FF, 6);
    rgb[1] = r11g11b10_channel((texel >> 11) & 0x7FF, 6);
    rgb[2] = r11g11b10_channel((texel >> 22) & 0x3FF, 5);
}

struct ColorReadback {
    Readback rb{};
    std::vector<uint32_t> texels{};
    UINT w{};
    UINT h{};
};

ColorReadback g_color_readbacks[3]{}; // pre, lit, post
std::atomic<int> g_color_readback_frames{-1};

void finish_color_readbacks() {
    const auto left = g_color_readback_frames.load();

    if (left < 0) {
        return;
    }

    if (left > 0) {
        g_color_readback_frames = left - 1;
        return;
    }

    g_color_readback_frames = -1;

    for (auto& c : g_color_readbacks) {
        uint8_t* data{};

        if (c.rb.buffer == nullptr || FAILED(c.rb.buffer->Map(0, nullptr, (void**)&data))) {
            log_error("analysis: could not read {}", c.rb.name != nullptr ? c.rb.name : "?");
            return;
        }

        c.w = c.rb.footprint.Footprint.Width;
        c.h = c.rb.rows;
        c.texels.resize((size_t)c.w * c.h);

        for (UINT y = 0; y < c.h; ++y) {
            std::memcpy(&c.texels[(size_t)y * c.w], data + c.rb.footprint.Offset + (UINT64)y * c.rb.footprint.Footprint.RowPitch, c.w * 4);
        }

        c.rb.buffer->Unmap(0, nullptr);
        c.rb.buffer.Reset();
    }

    const auto& pre = g_color_readbacks[0];
    const auto& lit = g_color_readbacks[1];
    const auto& post = g_color_readbacks[2];

    for (const auto* c : {&pre, &lit, &post}) {
        uint32_t lo = 0xFFFFFFFF;
        uint32_t hi = 0;
        size_t distinct_changes = 0;

        for (size_t i = 0; i < c->texels.size(); ++i) {
            lo = std::min(lo, c->texels[i]);
            hi = std::max(hi, c->texels[i]);
            distinct_changes += i > 0 && c->texels[i] != c->texels[i - 1];
        }

        log_info("analysis: {} {}x{} format {}: texel[0] {:08x} middle {:08x}, raw min {:08x} max {:08x}, value changes {}", c->rb.name, c->w,
            c->h, (int)c->rb.format, c->texels.empty() ? 0 : c->texels[0], c->texels.empty() ? 0 : c->texels[c->texels.size() / 2], lo, hi,
            distinct_changes);
    }

    if (pre.w != lit.w || lit.w != post.w || pre.h != lit.h || lit.h != post.h) {
        log_error("analysis: sizes differ");
        return;
    }

    // Bins by the green channel of `lit` (log2 steps from 2^-12 to 2^6).
    constexpr int BINS = 18;
    double ratio_sum[BINS]{};
    double bloom_sum[BINS]{};
    double lit_sum[BINS]{};
    double post_sum[BINS]{};
    uint64_t count[BINS]{};
    double max_lit{};
    double max_post{};

    for (size_t i = 0; i < lit.texels.size(); i += 3) {
        float p[3], l[3], o[3];
        decode_r11g11b10(pre.texels[i], p);
        decode_r11g11b10(lit.texels[i], l);
        decode_r11g11b10(post.texels[i], o);
        max_lit = std::max(max_lit, (double)l[1]);
        max_post = std::max(max_post, (double)o[1]);

        if (l[1] <= 0.0f) {
            continue;
        }

        const auto bin = std::clamp((int)std::floor(std::log2(l[1])) + 12, 0, BINS - 1);
        ratio_sum[bin] += o[1] / l[1];
        bloom_sum[bin] += l[1] - p[1];
        lit_sum[bin] += l[1];
        post_sum[bin] += o[1];
        ++count[bin];
    }

    log_info("analysis: {}x{}, max lit {:.4f}, max post {:.4f}", lit.w, lit.h, max_lit, max_post);

    for (int b = 0; b < BINS; ++b) {
        if (count[b] == 0) {
            continue;
        }

        log_info("analysis: lit ~2^{:>3}: {:>7} px, mean lit {:.5f} post {:.5f}, post/lit {:.4f}, bloom (lit-pre) {:.5f}", b - 12, count[b],
            lit_sum[b] / count[b], post_sum[b] / count[b], ratio_sum[b] / count[b], bloom_sum[b] / count[b]);
    }
}

void record_color_readbacks(ID3D12GraphicsCommandList* list, ID3D12Resource* pre, ID3D12Resource* lit, ID3D12Resource* post) {
    record_readback(list, g_color_readbacks[0].rb, pre, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "pre");
    record_readback(list, g_color_readbacks[1].rb, lit, INPUT_STATE, "lit");
    record_readback(list, g_color_readbacks[2].rb, post, D3D12_RESOURCE_STATE_RENDER_TARGET, "post");
    g_color_readback_frames = 1;
}

cl_state::UpscaleAction on_upscale(ID3D12GraphicsCommandList* list, ID3D12Resource* source, uint32_t index, bool after);
void on_missing_upscale(ID3D12GraphicsCommandList* list);
void on_compute_upscale(ID3D12GraphicsCommandList* list, ID3D12Resource* target, uint32_t index);

// RE4's compute upscale: DLSS's output goes into the engine's upscale output texture (t_compute_target, set while the
// DLSS pass runs from the compute hook); its composition (exposure, fades) then works on DLSS's image. g_compute_frame:
// this frame went that way, so the scene target bind only keeps the HUD-less scene.
thread_local ID3D12Resource* t_compute_target{};

// Native DLAA in place (profile native_dlaa_in_place): the scene target right after the engine's draw into it, copied, is
// DLAA's input (t_native_input while the DLSS pass runs from there); the result goes back into the scene target.
ComPtr<ID3D12Resource> g_native_input{};
thread_local ID3D12Resource* t_native_input{};
std::atomic<bool> g_compute_frame{};
std::atomic<bool> g_compute_in_place{}; // that frame's compute pass wrote the scene target itself (RE7)

// Profile no_upscale_fallback (RE7): frames with the marker but no upscale step in a row, and whether the render scale is
// at native for such a scene (DLSS paused) until an upscale step shows up again.
std::atomic<int> g_markers_without_upscale{};
std::atomic<bool> g_no_upscale_fallback{};

// Videotape mode (profile videotape_mode): this frame's depth at 2/3 of the render size the mode's scale gives (the tape
// scenes), or at that size (normal scenes); switches after 3 frames in a row, reconfiguring DLSS's output size.
void update_videotape_mode(uint32_t index) {
    ID3D12Resource* depth{};
    {
        std::scoped_lock _{g_mutex};
        depth = g_slots[index].depth;
    }

    const auto rate = (double)g_wanted_rate.load();

    if (depth == nullptr || g_target_desc.Width == 0 || rate <= 0.0) {
        return;
    }

    const auto d = depth->GetDesc();
    const auto w = rate * (double)g_target_desc.Width;
    const auto h = rate * (double)g_target_desc.Height;
    const auto tape = std::abs((double)d.Width - w * 2.0 / 3.0) <= 2.0 && std::abs((double)d.Height - h * 2.0 / 3.0) <= 2.0;
    const auto normal = std::abs((double)d.Width - w) <= 2.0 && std::abs((double)d.Height - h) <= 2.0;

    if ((!tape && !normal) || tape == g_tape.load()) {
        g_tape_votes = 0; // in between (a render scale being applied), or no change
        return;
    }

    if (++g_tape_votes < 3) {
        return;
    }

    g_tape_votes = 0;
    g_tape = tape;
    g_output.configured = false; // DLSS's output size changes
    g_no_upscale_fallback = false; // tape scenes are handled here now (the fallback stays for anything else)
    g_markers_without_upscale = 0;
    log_info("DLSS: videotape mode {} (depth {}x{})", tape ? "ON: DLSS up to the tape's own 2/3 resolution, the game's TAA on" : "off", d.Width,
        d.Height);
}

// An upscale step (draw or compute) reached the plugin with `input` as its image. A fallback for a scene without one is
// over only when that image is output-sized: at native render scale RE7's videotape segments do use the compute step,
// but from an image about two thirds of the output size (they render at 2/3 of the render scale), normal scenes from a
// full-size one.
void note_upscale_seen(ID3D12Resource* input) {
    g_markers_without_upscale = 0;

    if (g_no_upscale_fallback.load()) {
        const auto desc = input != nullptr ? input->GetDesc() : D3D12_RESOURCE_DESC{};

        if (desc.Width + 2 < g_target_desc.Width || desc.Height + 2 < g_target_desc.Height) {
            return; // still the scene drawn another way
        }

        g_no_upscale_fallback = false;
        g_output.configured = false; // the DLSS mode's render scale again (configure)
        log_info("DLSS: upscale step back: the DLSS mode's render scale again");
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Frame generation's copies of the depth and motion vectors (same description as the engine's, kept in their read states).
constexpr uint32_t FG_RING = 3;
ComPtr<ID3D12Resource> g_fg_depth[FG_RING]{};
ComPtr<ID3D12Resource> g_fg_motion[FG_RING]{};
uint32_t g_fg_next{};

bool ensure_copy(ComPtr<ID3D12Resource>& texture, const D3D12_RESOURCE_DESC& like, D3D12_RESOURCE_STATES state, const wchar_t* name) {
    if (texture != nullptr) {
        const auto desc = texture->GetDesc();

        if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
            return true;
        }

        retire(texture);
    }

    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &like, state, nullptr, IID_PPV_ARGS(&texture)))) {
        log_error("DLSS: could not create {}x{} copy (format {})", like.Width, like.Height, (int)like.Format);
        return false;
    }

    texture->SetName(name);
    return true;
}

void copy_resource(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, D3D12_RESOURCE_STATES dst_state, ID3D12Resource* src,
    D3D12_RESOURCE_STATES src_state)
{
    barrier(list, src, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(list, dst, dst_state, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(dst, src);
    barrier(list, dst, D3D12_RESOURCE_STATE_COPY_DEST, dst_state);
    barrier(list, src, D3D12_RESOURCE_STATE_COPY_SOURCE, src_state);
}

// Before the engine adds its bloom to the scene: the scene as it rendered it back (the stabilized one was only for the bloom).
void restore_bloom_input(ID3D12GraphicsCommandList* list, ID3D12Resource* scene, D3D12_RESOURCE_STATES state, uint32_t) {
    if (!g_bloom_restore_pending.exchange(false)) {
        return; // already put back (before the upscale: the bloom draw never came)
    }

    copy_resource(list, scene, state, g_bloom_backup.Get(), INPUT_STATE);
}

// At the marker (before the engine's bloom chain reads the scene): the scene is backed up and replaced by a temporally
// stabilized one (bloom_stabilizer), restored right before the bloom chain's last draw adds the bloom to it.
void stabilize_bloom_input(ID3D12GraphicsCommandList* list, uint32_t index, D3D12_RESOURCE_STATES scene_state = INPUT_STATE) {
    Slot slot{};
    {
        std::scoped_lock _{g_mutex};
        slot = g_slots[index];
    }

    static const bool ready = bloom_stabilizer::init(g_device.Get());

    if (!ready || slot.color == nullptr || slot.motion == nullptr) {
        g_bloom_valid = false;
        return;
    }

    const auto desc = slot.color->GetDesc();
    const auto had = g_bloom_history[0] != nullptr ? g_bloom_history[0]->GetDesc() : D3D12_RESOURCE_DESC{};

    if (!ensure_texture(g_bloom_history[0], desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen bloom input history A") ||
        !ensure_texture(g_bloom_history[1], desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen bloom input history B") ||
        !ensure_copy(g_bloom_backup, desc, INPUT_STATE, L"refw_framegen scene for the bloom (original)"))
    {
        g_bloom_valid = false;
        return;
    }

    if (had.Width != desc.Width || had.Height != desc.Height || had.Format != desc.Format) {
        g_bloom_valid = false;
    }

    const auto saved = cl_state::save(list);

    if (saved == nullptr) {
        g_bloom_valid = false;
        return;
    }

    const auto current = g_bloom_frame++ % 2;
    const auto previous = 1 - current;
    const float mvec_scale[2] = {0.5f, -0.5f}; // as for DLSS: the engine's velocity is in clip space units (y up)

    if (static float logged[2] = {-1.0f, -1.0f}; logged[0] != framegen::dlss()->bloom_alpha || logged[1] != framegen::dlss()->bloom_moving) {
        logged[0] = framegen::dlss()->bloom_alpha;
        logged[1] = framegen::dlss()->bloom_moving;
        log_info("Bloom stabilizer: this frame's weight {:.2f}, moving limit {:.2f} render px", logged[0],
            logged[1] > 0.0f ? logged[1] : game_profile::current().bloom_moving_px);
    }

    // RE3 (profile bloom_input_render_target): called at its bloom chain's start, the scene in whatever state it is then
    // (a render target or a copy source): readable for this pass, back at the end.
    const auto scene_other = scene_state != INPUT_STATE;

    if (scene_other) {
        barrier(list, slot.color, scene_state, INPUT_STATE);
    }

    copy_resource(list, g_bloom_backup.Get(), INPUT_STATE, slot.color, INPUT_STATE);
    barrier(list, g_bloom_history[previous].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    gpu_timer::begin(list, gpu_timer::BLOOM_STABILIZER);
    const auto recorded = bloom_stabilizer::record(list, slot.color, g_bloom_history[previous].Get(), slot.motion, g_bloom_history[current].Get(),
        (uint32_t)desc.Width, desc.Height, mvec_scale, !g_bloom_valid || slot.camera.cut,
        framegen::dlss()->bloom_moving > 0.0f ? framegen::dlss()->bloom_moving : game_profile::current().bloom_moving_px, framegen::dlss()->bloom_alpha);
    gpu_timer::end(list, gpu_timer::BLOOM_STABILIZER);

    barrier(list, g_bloom_history[previous].Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    if (recorded) {
        copy_resource(list, slot.color, INPUT_STATE, g_bloom_history[current].Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (scene_other) {
        barrier(list, slot.color, INPUT_STATE, scene_state);
    }

    cl_state::restore(list, *saved);
    g_bloom_valid = recorded;

    if (recorded) {
        g_bloom_restore_pending = true;
        cl_state::watch_render_target(list, slot.color, &restore_bloom_input, index);
    }
}

// RE3: the stabilizer when its bloom chain starts (watch_bloom_chain), not at the marker: a draw into the scene between the
// marker and the chain (seen at a puddle / window) made the restore run there, before the chain read the scene.
void stabilize_bloom_at_chain(ID3D12GraphicsCommandList* list, ID3D12Resource*, D3D12_RESOURCE_STATES state, uint32_t index) {
    stabilize_bloom_input(list, index, state);
}

// The marker reached a command list: arm for the engine's upscale that follows it on the same list (the bind of the
// scene target). Returns true for every marker (it is never a real clear).
bool on_clear(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle, const FLOAT color[4]) {
    if (color == nullptr || color[0] != MARKER[0] || color[1] != MARKER[1] || color[2] != MARKER[2]) {
        return false;
    }

    // The missing-upscale fallback (3000): DLSS for this frame's slot into the scene target, as the replaced upscale would.
    if (color[3] >= 3000.0f) {
        on_missing_upscale(list);
        return true;
    }

    // HUD markers (hud_split) carry 2000 + their kind.
    if (color[3] >= 2000.0f) {
        return hud_split::on_marker(list, rtv_handle, (uint32_t)(color[3] - 2000.0f));
    }

    ++g_markers;
    ++g_frame_number;
    release_retired();

    // A scene without an upscale step (RE7's videotape segments): native render scale until one shows up again.
    if (game_profile::current().no_upscale_fallback && !g_tape.load() && framegen::dlss() != nullptr && framegen::dlss()->no_upscale_fallback && !g_dlss_paused && ++g_markers_without_upscale > 30 && !g_no_upscale_fallback &&
        g_target_desc.Height > 0 && g_wanted_rate.load() > 0.0f && g_wanted_rate.load() < 1.0f)
    {
        g_no_upscale_fallback = true;
        g_wanted_rate = 1.0f + 0.25f / (float)g_target_desc.Height; // native (the engine's scaled path, as native DLAA)
        log_info("DLSS: no upscale step for 30 frames (a scene drawn another way): native render scale, DLSS paused until it is back");
    }

    // Diagnostics: the fallback switched off (dlss_no_upscale_fallback=0) while it was on: the DLSS mode's render scale again.
    if (g_no_upscale_fallback.load() && framegen::dlss() != nullptr && !framegen::dlss()->no_upscale_fallback) {
        g_no_upscale_fallback = false;
        g_output.configured = false;
        log_info("DLSS: no-upscale fallback switched off (dlss_no_upscale_fallback=0)");
    }

    static const bool timer_ready = gpu_timer::init(g_device.Get());
    gpu_timer::next_frame();
    const auto index = (uint32_t)(color[3] - MARKER[3]);
    const auto dlss = framegen::dlss();

    if (index >= SLOTS || dlss == nullptr) {
        skip("off");
        return true;
    }

    if (game_profile::current().videotape_mode) {
        update_videotape_mode(index);
    }

    // Debug 4: what the engine records on this list after the marker (3 frames, once the render scale applies).
    if (dlss->debug == 4) {
        // With the render scale DLSS would use, so the engine's own upscale shows in the trace.
        Slot current{};
        {
            std::scoped_lock _{g_mutex};
            current = g_slots[index];
        }

        if (current.target != nullptr) {
            configure(*dlss, current.target);
        }

        static int frames = 0;

        if (++frames == 60 || frames == 180 || frames == 300 || frames == 1500 || frames == 3000) {
            Slot slot{};
            {
                std::scoped_lock _{g_mutex};
                slot = g_slots[index];
            }

            log_info("trace: ---- marker on list {:x}: scene target {:x} (rtv {:x}), HDR color {:x}, depth {:x}, velocity {:x}", (uintptr_t)list,
                (uintptr_t)slot.target, rtv_handle.ptr, (uintptr_t)slot.color, (uintptr_t)slot.depth, (uintptr_t)slot.motion);
            cl_state::trace(list, 400); // RE4: its post chain is long (compute, half-res targets) before the upscale
        }

        skip("debug 4");
        return true;
    }

    // Option A: the scene as it is here, before the engine's bloom is added to it, is DLSS's input.
    if (dlss->bloom) {
        Slot slot{};
        {
            std::scoped_lock _{g_mutex};
            slot = g_slots[index];
        }

        if (slot.color != nullptr &&
            ensure_texture(g_pre, slot.color->GetDesc(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"refw_framegen scene before bloom"))
        {
            barrier(list, slot.color, INPUT_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
            barrier(list, g_pre.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(g_pre.Get(), slot.color);
            barrier(list, g_pre.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(list, slot.color, D3D12_RESOURCE_STATE_COPY_SOURCE, INPUT_STATE);

            std::scoped_lock _{g_mutex};
            g_slots[index].pre_copied = true;
        }
    }

    // A new frame: a compute-path frame before it is over (RE7 switches between its draw and compute upscale per frame).
    g_compute_frame = false;
    g_compute_in_place = false;
    g_marker_index = index;
    g_marker_frame = g_frame_number.load();
    cl_state::arm(list, rtv_handle, &on_upscale, index, (uint32_t)dlss->skip_draws);

    // Games that upscale with a compute pass (RE4): DLSS replaces that pass's output (see on_compute_upscale).
    if (game_profile::current().upscale == game_profile::Upscale::COMPUTE) {
        Slot slot{};
        {
            std::scoped_lock _{g_mutex};
            slot = g_slots[index];
        }

        // Not at native size (RE4, DLAA in place: no upscale then, and another output-sized compute pass would be taken for it),
        // unless the profile says the engine keeps its compute upscale there (RE7: native DLAA through it).
        if (slot.target != nullptr && slot.color != nullptr) {
            const auto desc = slot.target->GetDesc();
            const auto color = slot.color->GetDesc();

            if (color.Width < desc.Width || color.Height < desc.Height || game_profile::current().compute_upscale_at_native) {
                cl_state::watch_compute_upscale(list, (uint32_t)desc.Width, desc.Height, slot.color, &on_compute_upscale);
            }
        }
    }

    // Per game (profile): it expects the scene readable at the marker (RE4's is still a render target there).
    if (dlss->bloom_stabilize && !dlss->bloom && game_profile::current().bloom_stabilizer && !g_own_bloom.load()) {
        if (game_profile::current().bloom_input_render_target) {
            ID3D12Resource* color{};
            {
                std::scoped_lock _{g_mutex};
                color = g_slots[index].color;
            }

            if (color != nullptr) {
                cl_state::watch_bloom_chain(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, &stabilize_bloom_at_chain, index);
            }
        } else {
            stabilize_bloom_input(list, index);
        }
    } else {
        g_bloom_valid = false;
    }

    return true;
}

// The engine binds the scene target to upscale its low-res post color (the scene with bloom) into it: DLSS instead.
// `source` is that post color (the last render target before the bind, still in the render target state).
void guard_after_engine_draw(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_RESOURCE_STATES source_state, const Slot& slot,
    const framegen::Dlss& dlss)
{
    // Diagnostics (probe_descriptors): what the engine's upscale draw read (RE2's item-use animations freeze below 100%).
    if (static int calls = 0; descriptor_map::installed() && calls++ % 60 == 0) {
        log_info("descriptors: upscale draw: source {:x} ({}x{} fmt {}), HDRTarget {:x}, target {:x}", (uintptr_t)source,
            source != nullptr ? source->GetDesc().Width : 0, source != nullptr ? source->GetDesc().Height : 0,
            source != nullptr ? (int)source->GetDesc().Format : 0, (uintptr_t)slot.color, (uintptr_t)slot.target);
        cl_state::dump_graphics_bindings(list, "upscale draw");
    }

    const auto source_to_read = source_state == D3D12_RESOURCE_STATE_RENDER_TARGET; // else it is readable already
    const auto out_w = (uint32_t)g_target_desc.Width;
    const auto out_h = g_target_desc.Height;
    const auto saved = cl_state::save(list);

    if (saved == nullptr || g_result == nullptr || slot.target == nullptr ||
        !ensure_texture(g_guarded, g_target_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen upscale guard output"))
    {
        hud_split::set_hudless_scene(nullptr);
        return; // the engine's own upscale stays
    }

    barrier(list, slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (source_to_read) barrier(list, source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    gpu_timer::begin(list, gpu_timer::UPSCALE_GUARD);
    const auto recorded = upscale_guard::record(list, slot.target, source, g_result, g_guarded.Get(), out_w, out_h, g_render_size[0], g_render_size[1],
        dlss.guard, (game_profile::current().guard_live_rule != nullptr || game_profile::current().guard_live_component != nullptr) && dlss.guard_live && g_live_rule_screen.load() && !own_dof::examining());
    gpu_timer::end(list, gpu_timer::UPSCALE_GUARD);

    barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (source_to_read) barrier(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);

    if (recorded) {
        barrier(list, g_guarded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list, slot.target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyResource(slot.target, g_guarded.Get());
        barrier(list, slot.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(list, g_guarded.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    } else {
        barrier(list, slot.target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    cl_state::restore(list, *saved);
    hud_split::set_hudless_scene(recorded ? g_guarded.Get() : nullptr); // the scene without the GUI the engine draws next

    // Every few seconds, for tuning the threshold.
    float k = 0.0f;
    float r = 0.0f;
    bool match = false;

    upscale_guard::set_log_every(dlss.guard_log > 0 ? (uint32_t)dlss.guard_log : 600u);

    if (upscale_guard::latest(k, r, match)) {
        log_info("Upscale guard: k {:.3f} r {:.4f} post {:.4f} -> {}", k, r, upscale_guard::last_post_energy(),
            match ? (upscale_guard::last_live() ? "DLSS (live post color, the engine's image differs)" : "DLSS") : "the engine's frame");
    }
}

// After the engine's compute upscale wrote `target`: the DLSS pass, its output copied into `target`.
void on_compute_upscale(ID3D12GraphicsCommandList* list, ID3D12Resource* target, uint32_t index) {
    Slot slot{};
    {
        std::scoped_lock _{g_mutex};
        slot = g_slots[index];
    }

    note_upscale_seen(slot.color);

    // Diagnostics (probe_descriptors): the texture given to DLSS, next to the upscale pass's bindings (cl_state logs those).
    if (static int calls = 0; descriptor_map::installed() && calls++ % 300 == 150) {
        log_info("descriptors: DLSS input (slot.color) {:x}, upscale output {:x}", (uintptr_t)slot.color, (uintptr_t)target);
    }

    if (slot.color == nullptr) {
        return;
    }

    t_compute_target = target;
    // RE4 (setting dlss_compute_input): the pass's own input (the scene's second color buffer, depth of field in it) rather
    // than HDRTarget.
    auto input = slot.color;

    if (const auto engine_input = cl_state::compute_upscale_input(); engine_input != nullptr && game_profile::current().compute_upscale_input &&
        framegen::dlss() != nullptr &&
        framegen::dlss()->compute_input)
    {
        if (static ID3D12Resource* logged = nullptr; logged != engine_input) {
            logged = engine_input;
            log_info("DLSS: compute upscale input {:x} (HDRTarget {:x})", (uintptr_t)engine_input, (uintptr_t)slot.color);
        }

        input = engine_input;
        t_engine_input = engine_input;
        t_engine_input_state = cl_state::compute_upscale_input_state();
    }

    on_upscale(list, input, index, false);
    t_engine_input = nullptr;
    t_compute_target = nullptr;
}


// The missing-upscale fallback's marker (queued before every PrepareOutput, profile missing_upscale_fallback): runs after
// the point where this frame's upscale would have been. If the engine skipped it (RE2's item-use animations below 100%),
// DLSS for the frame's slot replaces the scene target as the upscale would have; otherwise nothing.
void on_missing_upscale(ID3D12GraphicsCommandList* list) {
    const auto frame = g_marker_frame.load();

    // Only gaps between real upscales (item-use animations upscale about every 4th frame): splash screens and the title
    // screen skip it on purpose for long stretches (their own content; the fallback turned the fades white).
    if (frame == 0 || g_upscale_call_frame.load() == frame || g_real_upscale_frame.load() == 0 || frame - g_real_upscale_frame.load() > 60 || framegen::dlss() == nullptr || g_dlss_paused ||
        g_examine_dlaa.load())
    {
        return;
    }

    const auto index = g_marker_index.load();
    Slot slot{};
    {
        std::scoped_lock _{g_mutex};
        slot = g_slots[index];
    }

    if (slot.color == nullptr || slot.target == nullptr) {
        return;
    }

    if (g_fallback_upscales++ < 3) {
        log_info("DLSS: the engine skipped its upscale this frame (RE2 item-use animation): DLSS into the scene target instead");
    }

    // The image the upscale would have read: the last render target before this point if it is like the scene color
    // (RE2 runs some post effects into a second buffer: the fuse box's light came out too bright from HDRTarget), else
    // HDRTarget.
    auto source = slot.color;
    D3D12_RESOURCE_STATES state = INPUT_STATE;

    if (const auto last = cl_state::armed_last_render_target(list); last != nullptr && last != slot.color && last != slot.target) {
        const auto a = last->GetDesc();
        const auto c = slot.color->GetDesc();

        if (a.Width == c.Width && a.Height == c.Height && a.Format == c.Format) {
            source = last;
            state = cl_state::armed_last_render_target_state(list);

            if (static int logged = 0; logged++ < 3) {
                log_info("DLSS: missing upscale: source {:x} (the last render target), not HDRTarget {:x}", (uintptr_t)last, (uintptr_t)slot.color);
            }
        }
    }

    t_forced_replace = true;
    t_forced_source_state = state;
    on_upscale(list, source, index, false);
    t_forced_replace = false;
}

// The scene target after the engine's composition (with DLSS's image in it): the scene without the GUI drawn next, for
// frame generation's HUD-less frames.
void keep_hudless_scene(ID3D12GraphicsCommandList* list, const Slot& slot) {
    if (slot.target == nullptr || !ensure_texture(g_guarded, g_target_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen HUD-less scene")) {
        hud_split::set_hudless_scene(nullptr);
        return;
    }

    copy_resource(list, g_guarded.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET);
    hud_split::set_hudless_scene(g_guarded.Get());
}

cl_state::UpscaleAction on_upscale(ID3D12GraphicsCommandList* list, ID3D12Resource* source, uint32_t index, bool after) {
    // The bloom stabilizer's original scene back if the engine's bloom draw didn't put it back (RE2's Model Viewer: DLSS got
    // the time-smoothed scene, with trails behind moving limbs and soft detail).
    if (g_bloom_restore_pending.load() && index < SLOTS) {
        ID3D12Resource* color{};
        {
            std::scoped_lock _{g_mutex};
            color = g_slots[index].color;
        }

        if (color != nullptr) {
            if (static int logged = 0; logged++ < 3) {
                log_info("Bloom stabilizer: the scene put back at the upscale (the bloom draw didn't)");
            }

            // RE3: HDRTarget is a render target again when its upscale draw binds the scene target.
            restore_bloom_input(list, color,
                game_profile::current().bloom_input_render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : INPUT_STATE, index);
        }
    }

    g_upscale_call_frame = g_marker_frame.load();

    if (!t_forced_replace) {
        g_real_upscale_frame = g_marker_frame.load();
    }
    t_native_path = false;
    const auto dlss = framegen::dlss();

    // The draw path's bind (the compute path noted it already), judged by the scene's HDR color (the render input), not by
    // the last render target: with the GUI on screen that is a full-size GUI layer (RE7's videotape segments went black
    // with the pause menu, inventory or weapon HUD up: the fallback ended on it).
    if (t_compute_target == nullptr) {
        ID3D12Resource* color{};
        {
            std::scoped_lock _{g_mutex};
            color = g_slots[index].color;
        }

        note_upscale_seen(color);
    }

    // A scene without the usual upscale (profile no_upscale_fallback): the engine's own image at native render scale.
    if (g_no_upscale_fallback.load()) {
        skip("scene without the usual upscale (native render scale)");
        return cl_state::KEEP;
    }

    if (dlss == nullptr || g_dlss_paused) {
        skip("off");
        return cl_state::KEEP;
    }

    Slot slot{};
    {
        std::scoped_lock _{g_mutex};
        slot = g_slots[index];
    }

    const auto out_w = (uint32_t)g_target_desc.Width;
    const auto out_h = g_target_desc.Height;

    // The scene target bind after a compute-path frame: DLSS already ran. RE4: after its composition draw, the HUD-less
    // scene. RE7 (the compute pass wrote the scene target itself, HUD-less scene kept then): this bind draws the GUI (the
    // title logo, weapon/inventory layers) into it: left alone.
    if (t_compute_target == nullptr && g_compute_frame.load()) {
        if (g_compute_in_place.exchange(false)) {
            g_compute_frame = false;
            return cl_state::KEEP;
        }

        if (!after) {
            return cl_state::AFTER_DRAWS;
        }

        g_compute_frame = false;
        keep_hudless_scene(list, slot);
        return cl_state::KEEP;
    }

    // The image the engine upscales. RE2: the last render target before the bind (its post color, render-sized, still a
    // render target). RE4 upscales with a compute pass from the scene's HDR color (bloom already in it), and the last
    // render target there is a half-resolution bloom texture: then the HDR color target, readable at this point.
    auto source_state = source != nullptr && source == slot.color ? INPUT_STATE : D3D12_RESOURCE_STATE_RENDER_TARGET;

    // RE3: its upscale draw reads HDRTarget itself, the last render target, made readable only after the scene target's bind.
    if (game_profile::current().bloom_input_render_target && source != nullptr && source == slot.color && !after && !t_forced_replace &&
        t_compute_target == nullptr)
    {
        source_state = cl_state::upscale_source_state();
    }

    if (t_forced_replace && source != nullptr && source != slot.color) {
        source_state = t_forced_source_state; // the missing-upscale fallback's source (the last render target's state)
    }

    if (source != nullptr && source == t_engine_input) {
        source_state = t_engine_input_state;
    }

    if (game_profile::current().hdr_color_fallback && source != nullptr && slot.color != nullptr && slot.depth != nullptr) {
        const auto s_desc = source->GetDesc();
        const auto d_desc = slot.depth->GetDesc();
        const auto c_desc = slot.color->GetDesc();

        if ((s_desc.Width != d_desc.Width || s_desc.Height != d_desc.Height) && c_desc.Width == d_desc.Width && c_desc.Height == d_desc.Height) {
            source = slot.color;
            source_state = INPUT_STATE;
        }
    }

    // RE3 (profile scene_color_source): its upscale draw reads HDRTarget itself. The last render target is not a safe guess:
    // its save screen draws a background buffer of half the output size after the scene, which at Performance (render scale
    // exactly 0.5) is render-sized too and was taken for the scene (DLSS upscaled a dark UI buffer: black background).
    if (game_profile::current().scene_color_source && source != nullptr && source != slot.color && slot.color != nullptr && slot.depth != nullptr &&
        !t_forced_replace && t_compute_target == nullptr)
    {
        const auto c_desc = slot.color->GetDesc();
        const auto d_desc = slot.depth->GetDesc();

        if (c_desc.Width == d_desc.Width && c_desc.Height == d_desc.Height) {
            if (static int logged = 0; logged++ < 3) {
                log_info("DLSS: source {:x} ({}x{}) is not the scene: HDRTarget {:x} instead (scene_color_source)", (uintptr_t)source,
                    source->GetDesc().Width, source->GetDesc().Height, (uintptr_t)slot.color);
            }

            source = slot.color;
            source_state = INPUT_STATE;
        }
    }


    // Native DLAA in place: the engine's draw into the scene target runs first (AFTER_DRAWS), then DLAA on a copy of its
    // image (the same stage as the upscale in the other modes: after the post effects, before the GUI), written back.
    // Native = the depth is output-sized: the last render target before the bind isn't always the scene target (RE4 with
    // its health bar on screen: a half-size bloom texture), which must not become DLAA's input.
    const auto engine_at_native = [&] {
        if (slot.target == nullptr || slot.depth == nullptr) return false;
        const auto t = slot.target->GetDesc();
        const auto d = slot.depth->GetDesc();
        return d.Width == t.Width && d.Height == t.Height;
    };

    if (slot.target != nullptr && dlss->mode == 0 && game_profile::current().native_dlaa_in_place && t_native_input == nullptr &&
        dlss->debug == 0 && (source == slot.target || engine_at_native()))
    {
        if (!after) {
            configure(*dlss, slot.target);
            return cl_state::AFTER_DRAWS;
        }

        if (!ensure_texture(g_native_input, slot.target->GetDesc(), INPUT_STATE, L"refw_framegen DLAA input (in place)")) {
            return cl_state::KEEP;
        }

        copy_resource(list, g_native_input.Get(), INPUT_STATE, slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET);
        t_native_input = g_native_input.Get();
        on_upscale(list, g_native_input.Get(), index, false);
        t_native_input = nullptr;
        return cl_state::KEEP;
    }

    if (source != nullptr && source == t_native_input) {
        source_state = INPUT_STATE;
    }

    // Upscale guard (AFTER_DRAWS): the engine's upscale draw ran into the scene target; DLSS's output replaces it where it
    // was an upscale of the post color (scaled by its fade), else the engine's result stays.
    if (after && dlss->debug != 1 && dlss->debug != 6) {
        guard_after_engine_draw(list, source, source_state, slot, *dlss);
        return cl_state::KEEP;
    }

    // Split screen (debug 1): the engine's upscale ran (after = true): DLSS's left half over it.
    if (after) {
        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list, slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{slot.target, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION src{g_result, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        const D3D12_BOX left{0, 0, 0, out_w / 2, out_h, 1};
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, &left);
        barrier(list, slot.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(list, g_result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        return cl_state::KEEP;
    }

    if (source == nullptr || slot.depth == nullptr || slot.motion == nullptr || slot.target == nullptr || !slot.camera.valid) {
        // Still configured (the render scale for the mode): RE3 at a render scale of exactly 1 (DLSS switched off and on
        // again) binds the scene target with no render target before it, and stayed at native for good without this.
        if (slot.target != nullptr) {
            configure(*dlss, slot.target);
        }

        skip(source == nullptr ? "inputs missing (no source)" : !slot.camera.valid ? "inputs missing (camera)" : "inputs missing");
        return cl_state::KEEP;
    }

    // At a render scale of exactly 1 the engine's post pass draws into the scene target itself: nothing to replace here (the
    // bind is its post pass, not an upscale). Leave the frame to the engine rather than skip its post pass.
    if (source == slot.target) {
        // Diagnostics (dlss_debug=11): the scene color and depth in this path (RE2's typewriter save screen below native).
        if (static uint32_t native_frames = 0; dlss->debug == 11 && g_readback_frames < 0 && ++native_frames % 60 == 0 && slot.color != nullptr &&
            slot.depth != nullptr)
        {
            const auto d = slot.depth->GetDesc();
            g_readback_render[0] = (uint32_t)d.Width;
            g_readback_render[1] = d.Height;
            record_readback(list, g_readbacks[0], slot.color, INPUT_STATE, "color");
            record_readback(list, g_readbacks[1], slot.depth, DEPTH_STATE, "depth");
            g_readback_frames = 1;
            log_info("readback: typewriter path: color {}x{} fmt {}, depth {}x{}, source = scene target {}x{}", slot.color->GetDesc().Width,
                slot.color->GetDesc().Height, (int)slot.color->GetDesc().Format, d.Width, d.Height, g_target_desc.Width, g_target_desc.Height);
        }

        // RE2's typewriter save screen (profile native_path_dlss, setting dlss_native_path): the engine skips its upscale step
        // and its full-size pass comes out black (unmodded too, at any render scale but exactly 1.0; DLAA's 1.0002 too), though the
        // scene color has the room: DLSS from the scene
        // color instead, its output replacing that pass (as the upscale draw is replaced in the usual path).
        if (game_profile::current().native_path_dlss && dlss->native_path && slot.color != nullptr && slot.depth != nullptr) {
            source = slot.color;
            source_state = INPUT_STATE;
            t_native_path = true;
        } else {
            // Configured anyway: a game that starts at native (RE4) or a mode change from native DLAA must still get its
            // render scale from configure().
            configure(*dlss, slot.target);
            skip("engine at native resolution");
            return cl_state::KEEP;
        }
    }

    if (!configure(*dlss, slot.target)) {
        skip("not configured");
        return cl_state::KEEP;
    }

    // Videotape scenes with dlss_tape_off: the game's own image (native render scale, its TAA), as vanilla.
    if (g_tape.load() && dlss->tape_off) {
        skip("videotape scene: DLSS off there (setting)");
        return cl_state::KEEP;
    }

    // Option A: the scene before the bloom (copied at the marker) instead of the engine's post color, if it matches.
    const auto use_pre = dlss->bloom && slot.pre_copied && g_pre != nullptr && g_pre->GetDesc().Width == source->GetDesc().Width &&
        g_pre->GetDesc().Height == source->GetDesc().Height;
    const auto input = use_pre ? g_pre.Get() : source;
    const auto input_state = use_pre ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : source_state;

    const auto color_desc = source->GetDesc();
    const auto depth_desc = slot.depth->GetDesc();
    const auto render_w = (uint32_t)std::min(color_desc.Width, depth_desc.Width);
    const auto render_h = std::min(color_desc.Height, depth_desc.Height);

    g_seen_render_size[0] = render_w;
    g_seen_render_size[1] = render_h;

    // The frames before the engine applies a new render scale are larger than DLSS takes for the mode (NGX fails them).
    // Smaller by the engine's rounding (853x479 for Ultra Performance's exact 853x480) is accepted.
    if (render_w > g_output.render_max[0] || render_h > g_output.render_max[1]) {
        skip("render scale not applied yet");
        return cl_state::KEEP;
    }

    // Larger than the mode's own render size (the previous mode's, a few frames after a switch) though within DLSS's
    // range: DLSS would size its feature for that input and keep it (RE2, Performance after DLAA: M at 2.41 ms instead of
    // ~1.3 for the whole session). The engine's own upscale shows for those frames; after 30 the size is taken as it is.
    if ((render_w > g_output.render_expected[0] + 2 || render_h > g_output.render_expected[1] + 2) && g_output.early_frames < 30) {
        ++g_output.early_frames;
        skip("render scale not applied yet");
        return cl_state::KEEP;
    }

    if (render_w != g_render_size[0] || render_h != g_render_size[1]) {
        g_render_size[0] = render_w;
        g_render_size[1] = render_h;
        g_output.reset = true;
        log_info("DLSS: render size {}x{} -> {}x{} (post color {}x{} fmt {})", render_w, render_h, (uint32_t)g_output.desc.Width,
            g_output.desc.Height, color_desc.Width, color_desc.Height, (int)color_desc.Format);
    }

    // Debug 9 (performance comparisons): the render scale stays, DLSS isn't evaluated (the engine's own upscale shows).
    if (dlss->debug == 9) {
        skip("debug 9");
        return cl_state::KEEP;
    }

    const auto saved = cl_state::save(list);

    if (saved == nullptr) {
        skip("command list state unknown");
        return cl_state::KEEP;
    }

    const auto token = framegen::render_frame_token();

    if (token == nullptr) {
        skip("no frame token");
        return cl_state::KEEP;
    }

    const auto constants = set_constants(*dlss, *token, slot.camera, render_w, render_h, g_output.reset || slot.camera.cut);
    framegen::constants_set(token);

    // RE2 [EXPERIMENTAL] dither tests: the frame was rendered moved by whole pixels; DLSS's inputs moved back (never
    // elsewhere: the shift is 0 outside RE2's experiment).
    re2_dither::Inputs dlss_in{input, input_state, slot.depth, DEPTH_STATE, slot.motion, INPUT_STATE};

    if (slot.camera.dither_shift[0] != 0 || slot.camera.dither_shift[1] != 0 || slot.camera.dither_prev_shift[0] != 0 ||
        slot.camera.dither_prev_shift[1] != 0)
    {
        int moved[2]{};
        int moved_before[2]{};
        re2_dither::picture_moved(slot.camera.dither_shift, slot.camera.projection.m[2][3], moved);
        re2_dither::picture_moved(slot.camera.dither_prev_shift, slot.camera.projection.m[2][3], moved_before);
        // The shift's change in the engine's velocity (clip units, previous - current position; y up): taken out for DLSS.
        const float motion_change[2] = {2.0f * (float)(moved_before[0] - moved[0]) / (float)render_w,
            -2.0f * (float)(moved_before[1] - moved[1]) / (float)render_h};
        const auto history = g_output.reset || slot.camera.cut ? nullptr : g_output.texture.Get();
        re2_dither::unshift(list, g_device.Get(), dlss_in, moved, motion_change, history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, render_w, render_h);
    }

    sl::Resource color_res{sl::ResourceType::eTex2d, dlss_in.color, nullptr, nullptr, (uint32_t)dlss_in.color_state};
    sl::Resource depth_res{sl::ResourceType::eTex2d, dlss_in.depth, nullptr, nullptr, (uint32_t)dlss_in.depth_state};
    sl::Resource motion_res{sl::ResourceType::eTex2d, dlss_in.motion, nullptr, nullptr, (uint32_t)dlss_in.motion_state};
    sl::Resource output_res{sl::ResourceType::eTex2d, g_output.texture.Get(), nullptr, nullptr, (uint32_t)D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
    const sl::Extent render_extent{0, 0, render_w, render_h};
    const sl::Extent output_extent{0, 0, (uint32_t)g_output.desc.Width, g_output.desc.Height};

    sl::ResourceTag tags[] = {
        sl::ResourceTag{&color_res, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent},
        sl::ResourceTag{&depth_res, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent},
        sl::ResourceTag{&motion_res, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilEvaluate, &render_extent},
        sl::ResourceTag{&output_res, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eValidUntilEvaluate, &output_extent},
    };

    dlss->set_tag_for_frame(*token, dlss->viewport, tags, (uint32_t)std::size(tags), (sl::CommandBuffer*)list);

    const sl::BaseStructure* inputs[] = {&dlss->viewport};
    gpu_timer::begin(list, gpu_timer::DLSS_EVALUATE);
    const auto result = dlss->evaluate_feature(sl::kFeatureDLSS, *token, inputs, (uint32_t)std::size(inputs), (sl::CommandBuffer*)list);
    gpu_timer::end(list, gpu_timer::DLSS_EVALUATE);

    // Debug 6: how the engine's post pass maps the scene to its post color (every 1200 frames, to the log; with the split screen).
    if (static int analysis_frame = 0; dlss->debug == 6 && use_pre && ++analysis_frame % 1200 == 0 && g_color_readback_frames < 0) {
        record_color_readbacks(list, g_pre.Get(), slot.color, source);
    }

    // Option A: the engine's bloom (its post color minus the scene before it) put back over DLSS's output.
    g_result = g_output.texture.Get();

    // The composite's shader and pipeline: made the first time they are needed (not during the game's swapchain creation).
    static const bool composite_ready = use_pre ? bloom_composite::init(g_device.Get()) : false;

    if (result == sl::Result::eOk && use_pre && composite_ready &&
        ensure_texture(g_final, g_output.desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen DLSS output with bloom"))
    {
        barrier(list, g_output.texture.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(list, source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        if (bloom_composite::record(list, g_output.texture.Get(), source, g_pre.Get(), slot.color, g_final.Get(), (uint32_t)g_output.desc.Width,
                g_output.desc.Height, render_w, render_h))
        {
            g_result = g_final.Get();
        } else if (static bool once = true; once) {
            once = false;
            log_error("DLSS: bloom composite unavailable: the output is without the engine's bloom");
        }

        barrier(list, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(list, g_output.texture.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    // Videotape mode: DLSS's output is the tape's own 2/3 size; scaled to the scene target (bilinear, as soft as the game's
    // own step), which the rest of the pass then uses as DLSS's result.
    static const bool blit_ready = game_profile::current().videotape_mode ? scale_blit::init(g_device.Get()) : false;

    if (result == sl::Result::eOk && g_tape.load() && blit_ready &&
        ensure_texture(g_tape_full, g_target_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen DLSS output scaled to the scene (videotape)"))
    {
        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        scale_blit::record(list, g_result, g_tape_full.Get(), (uint32_t)g_target_desc.Width, g_target_desc.Height, dlss->tape_bicubic);
        barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        D3D12_RESOURCE_BARRIER written{};
        written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        written.UAV.pResource = g_tape_full.Get();
        list->ResourceBarrier(1, &written);
        g_result = g_tape_full.Get();
    }

    // FidelityFX CAS sharpening (setting dlss_cas, menu): on DLSS's result, before the plugin's depth of field, bloom and
    // motion blur (it sharpens DLSS's detail, not their blur or glow) and before the game's tonemapping and HUD.
    static bool cas_ready = false;

    if (result == sl::Result::eOk && dlss->cas && dlss->cas_strength > 0.0f && (cas_ready || (cas_ready = cas_sharpen::init(g_device.Get())))) {
        const auto rd = g_result->GetDesc();
        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto sharpened = cas_sharpen::record(list, g_result, (uint32_t)rd.Width, rd.Height, dlss->cas_strength);
        barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (sharpened != nullptr) {
            g_result = sharpened;
        }
    }

    // The plugin's depth of field (profile own_dof): on DLSS's (full-size) result, at the output resolution.
    static const bool dof_ready = game_profile::current().own_dof ? own_dof::init(g_device.Get()) : false;

    if (result == sl::Result::eOk && dof_ready && own_dof::active() &&
        ensure_texture(g_dof_out, g_target_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"refw_framegen depth of field"))
    {
        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const float dof_shift[2] = {0.0f, 0.0f}; // the depth lookup not shifted by the jitter (tried, removed 2026-11-08)
        gpu_timer::begin(list, gpu_timer::DEPTH_OF_FIELD);
        const auto blurred = own_dof::record(list, g_result, slot.depth, slot.motion, g_dof_out.Get(), (uint32_t)g_target_desc.Width, g_target_desc.Height,
            render_w, render_h, slot.camera.projection, dof_shift, dlss->dof_scale * game_profile::current().own_dof_scale,
            dlss->dof_max_scale * game_profile::current().own_dof_max_scale, dlss->dof_temporal, g_output.reset || slot.camera.cut);
        gpu_timer::end(list, gpu_timer::DEPTH_OF_FIELD);
        barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (blurred) {
            D3D12_RESOURCE_BARRIER written{};
            written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            written.UAV.pResource = g_dof_out.Get();
            list->ResourceBarrier(1, &written);
            g_result = g_dof_out.Get();
        }
    }
    else if (dof_ready) {
        own_dof::invalidate_history();
    }

    // RE2's item examine (profile examine_stabilizer, setting dlss_examine_stabilize): DLSS's output smoothed over time where
    // nothing moves (the item held still), the rotating item passes through.
    static const bool stabilizer_ready = game_profile::current().examine_stabilizer ? examine_stabilizer::init(g_device.Get()) : false;

    if (result == sl::Result::eOk && stabilizer_ready && dlss->examine_stabilize && own_dof::examining()) {
        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gpu_timer::begin(list, gpu_timer::DEPTH_OF_FIELD);
        const auto stabilized = examine_stabilizer::record(list, g_result, slot.motion, (uint32_t)g_target_desc.Width, g_target_desc.Height, render_w,
            render_h, dlss->examine_blend, g_output.reset || slot.camera.cut);
        gpu_timer::end(list, gpu_timer::DEPTH_OF_FIELD);
        barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (stabilized != nullptr) {
            g_result = stabilized;
        }
    } else if (stabilizer_ready) {
        examine_stabilizer::invalidate();
    }

    // RE7 (profile own_motion_blur): the game's motion blur after DLSS (its own is off while DLSS runs), if its option is on.
    static const bool motion_blur_ready = game_profile::current().own_motion_blur ? motion_blur::init(g_device.Get()) : false;

    if (result == sl::Result::eOk && motion_blur_ready && g_own_motion_blur.load()) {
        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto blurred = motion_blur::record(list, g_result, slot.motion, (uint32_t)g_target_desc.Width, g_target_desc.Height, render_w, render_h,
            g_motion_blur_shutter.load());
        barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (blurred != nullptr) {
            g_result = blurred;
        }
    }

    // RE8 (profile own_bloom): the game's bloom after DLSS (its own output ratio is zero while DLSS runs), from DLSS's
    // full-resolution output, last (in the game it comes after depth of field and motion blur too).
    static const bool own_bloom_ready = game_profile::current().own_bloom ? own_bloom::init(g_device.Get()) : false;

    if (result == sl::Result::eOk && own_bloom_ready && g_own_bloom.load()) {
        own_bloom::Params params{};
        params.threshold = g_bloom_threshold.load();
        params.dispersion = g_bloom_dispersion.load();
        params.ratio = g_bloom_ratio.load();
        params.strength = dlss->own_bloom_strength * game_profile::current().own_bloom_strength;
        params.additive = dlss->own_bloom_add;
        params.threshold_override = dlss->own_bloom_threshold;
        params.inner = dlss->own_bloom_inner >= 0.0f ? dlss->own_bloom_inner : game_profile::current().own_bloom_inner;

        barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const auto bloomed = own_bloom::record(list, g_result, slot.color, INPUT_STATE, input, input_state, (uint32_t)g_target_desc.Width,
            g_target_desc.Height, params);
        barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (bloomed != nullptr) {
            g_result = bloomed;
        }
    }

    cl_state::restore(list, *saved);

    // RE2 [EXPERIMENTAL] dither tests (never elsewhere: DLSS's inputs are the engine's then): the engine's motion vectors as
    // DLSS got them (moved back) for its passes after the upscale (they work on DLSS's output, which isn't moved).
    if (result == sl::Result::eOk && dlss_in.motion != slot.motion) {
        re2_dither::restore_motion(list, slot.motion, INPUT_STATE);
    }

    // Frame generation (at this frame's Present) gets copies of the depth and motion vectors: the engine overwrites its own
    // next frame, possibly before DLSS-G reads them. A ring of 3, since DLSS-G runs later on its own queue.
    if (result == sl::Result::eOk) {
        const auto i = g_fg_next++ % FG_RING;

        // RE2 [EXPERIMENTAL] dither tests: DLSS's moved-back copies (plain textures, no depth state); else the engine's.
        const auto fg_depth_state = dlss_in.depth != slot.depth ? INPUT_STATE : DEPTH_STATE;

        if (ensure_copy(g_fg_depth[i], dlss_in.depth->GetDesc(), fg_depth_state, L"refw_framegen depth for DLSS-G") &&
            ensure_copy(g_fg_motion[i], dlss_in.motion->GetDesc(), INPUT_STATE, L"refw_framegen motion vectors for DLSS-G"))
        {
            copy_resource(list, g_fg_depth[i].Get(), fg_depth_state, dlss_in.depth, dlss_in.depth_state);
            copy_resource(list, g_fg_motion[i].Get(), INPUT_STATE, dlss_in.motion, dlss_in.motion_state);

            framegen::FrameInputs inputs{};
            inputs.depth = g_fg_depth[i].Get();
            inputs.depth_state = (uint32_t)fg_depth_state;
            inputs.motion = g_fg_motion[i].Get();
            inputs.motion_state = (uint32_t)INPUT_STATE;
            inputs.width = render_w;
            inputs.height = render_h;
            inputs.constants = constants;
            framegen::set_frame_inputs(inputs);
        }
    }

    if (result != sl::Result::eOk) {
        static bool once = true;

        if (once) {
            log_error("DLSS: slEvaluateFeature failed: {}", (uint32_t)result);
            once = false;
        }

        skip("evaluate failed");
        return cl_state::KEEP;
    }

    g_output.reset = false;
    g_working = true;
    ++g_evaluated;

    if (static std::atomic<bool> logged{}; !logged.exchange(true)) {
        log_info("DLSS: first frame upscaled ({}x{} -> {}x{}, command list {:x}, thread {})", render_w, render_h, (uint32_t)g_output.desc.Width,
            g_output.desc.Height, (uintptr_t)list, GetCurrentThreadId());
    }

    // Debug 5: the velocity and depth of one frame (300 frames in) to the log; with the split screen (debug 1) every
    // 1200 frames, to see them in gameplay.
    static int readback_frame = 0;
    ++readback_frame;

    if ((dlss->debug == 5 && readback_frame % 300 == 0 && g_readback_frames < 0) ||(dlss->debug == 1 && readback_frame % 1200 == 0 && g_readback_frames < 0)) {
        record_readback(list, g_readbacks[0], slot.motion, INPUT_STATE, "velocity");
        record_readback(list, g_readbacks[1], slot.depth, DEPTH_STATE, "depth");
        g_readback_frames = 1;
    }

    // Debug 1 (split screen): the engine upscales as usual, the left half is replaced after it. Debug 2: not used.
    if (dlss->debug == 1 || dlss->debug == 6) {
        return cl_state::AFTER;
    }

    if (dlss->debug == 2) {
        return cl_state::KEEP;
    }

    // Native DLAA in place: back into the scene target (bound as a render target); it is the scene without the GUI too.
    if (t_native_input != nullptr) {
        copy_resource(list, slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        hud_split::set_hudless_scene(g_result);
        return cl_state::KEEP;
    }

    // The compute path: into the engine's upscale output. RE4: a texture of its own, its composition draws into the scene
    // target next (that bind keeps the HUD-less scene). RE7: the scene target itself, tone mapped straight from it (no
    // bind follows): DLSS's output is the scene without the GUI, as in the draw path.
    // RE4 (profile bloom_transfer, setting dlss_bloom_transfer): its pass added the bloom while upscaling; its own result is
    // still in the output texture: the bloom taken from it onto DLSS's image.
    static const bool bloom_transfer_ready = game_profile::current().bloom_transfer ? bloom_transfer::init(g_device.Get()) : false;

    if (t_compute_target != nullptr && t_compute_target != slot.target && bloom_transfer_ready && dlss->bloom_transfer > 0.0f) {
        if (const auto saved_state = cl_state::save(list)) {
            barrier(list, t_compute_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            const auto with_bloom = bloom_transfer::record(list, t_compute_target, input, g_result, (uint32_t)g_target_desc.Width, g_target_desc.Height,
                render_w, render_h, dlss->bloom_transfer);
            barrier(list, g_result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            barrier(list, t_compute_target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cl_state::restore(list, *saved_state);

            if (with_bloom != nullptr) {
                g_result = with_bloom;
            }
        }
    }

    if (t_compute_target != nullptr) {
        copy_resource(list, t_compute_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        if (t_compute_target == slot.target) {
            hud_split::set_hudless_scene(g_result);
            g_compute_in_place = true;
        }

        g_compute_frame = true;

        return cl_state::KEEP;
    }

    // The upscale guard: the engine's upscale draw runs, then the guard decides (the second call, after = true).
    static const bool guard_ready = dlss->guard > 0.0f ? upscale_guard::init(g_device.Get()) : false;

    if (dlss->guard > 0.0f && guard_ready && !t_native_path && !t_forced_replace) {
        return cl_state::AFTER_DRAWS;
    }

    // The scene target is bound as a render target (the state the engine's upscale draws in).
    barrier(list, g_result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(list, slot.target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(slot.target, g_result);
    barrier(list, slot.target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    barrier(list, g_result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hud_split::set_hudless_scene(g_result); // the scene without the GUI the engine draws next
    return cl_state::REPLACE;
}

// --------------------------------------------------------------------------------------------------------------------
// The camera: jitter in the scene's matrices, after the engine's Scene update computed them.
float halton(int index, int base) {
    float f = 1.0f;
    float r = 0.0f;

    for (; index > 0; index /= base) {
        f /= (float)base;
        r += f * (float)(index % base);
    }

    return r;
}

// The jittered views; JitterDisableSceneInfo/JitterDisablePostSceneInfo are the engine's unjittered ones, left alone.
constexpr const char* JITTERED_INFOS[] = {"SceneInfo", "DepthDistortionSceneInfo", "FilterSceneInfo", "ZPrepassSceneInfo"};

struct InfoHistory {
    Mat4 projection{};
    Mat4 view{};
    bool valid{};
} g_history[std::size(JITTERED_INFOS)]{};

uint32_t g_jitter_index{};

// Game thread, each BeginRendering (profile own_motion_blur): the game's option (RenderConfig.MotionBlurEnable) is left
// as it is; while DLSS runs the camera's MotionBlur component gets a shutter angle of 0 (re-applied every frame, the game
// writes it again: no blur from the engine's pass, before the upscale) and, if the option is on, the plugin's blurs after
// DLSS with the game's own angle. Without DLSS the game's angle is put back.
void update_motion_blur() {
    if (!game_profile::current().own_motion_blur) {
        return;
    }

    static const auto get_render_config = refw::find_method("via.render.Renderer", "get_RenderConfig");
    static const auto get_enable = refw::find_method("via.render.RenderConfig", "get_MotionBlurEnable");
    static const auto get_scene = refw::find_method("via.SceneManager", "get_CurrentScene");
    static const auto find_components = refw::find_method("via.Scene", "findComponents(System.Type)");
    static const auto get_length = refw::find_method("System.Array", "get_Length");
    static const auto get_value = refw::find_method("System.Array", "GetValue(System.Int32)");
    static const auto get_angle_enable = refw::find_method("via.render.MotionBlur", "get_ShutterAngleEnable");
    static const auto get_angle = refw::find_method("via.render.MotionBlur", "get_ShutterAngle");
    static const auto set_angle = refw::find_method("via.render.MotionBlur", "set_ShutterAngle");
    static const auto type = [] {
        const auto t = refw::find_type("via.render.MotionBlur");
        return t != nullptr ? (void*)refw::sdk()->type_definition->get_runtime_type(t) : nullptr;
    }();
    static float game_angle = 180.0f; // the game's own angle (the plugin's 0 never taken for it)

    const auto renderer = refw::native_singleton("via.render.Renderer");
    const auto config = renderer != nullptr ? (void*)refw::call(get_render_config, renderer) : nullptr;
    const auto manager = refw::native_singleton("via.SceneManager");
    const auto scene = manager != nullptr ? (void*)refw::call(get_scene, manager) : nullptr;

    if (config == nullptr || get_enable == nullptr || scene == nullptr || type == nullptr || find_components == nullptr || get_length == nullptr ||
        get_value == nullptr || get_angle == nullptr || set_angle == nullptr)
    {
        g_own_motion_blur = false;
        return;
    }

    uint64_t arg = (uint64_t)type;
    const auto array = (void*)refw::call_args(find_components, scene, &arg, 1);

    if (array == nullptr || (int32_t)refw::call(get_length, array) < 1) {
        g_own_motion_blur = false;
        return;
    }

    const auto option = (refw::call(get_enable, config) & 0xFF) != 0;
    const auto d = framegen::dlss();
    const auto take_over = active() && !g_dlss_paused && d != nullptr && d->own_motion_blur;

    // Every camera's (RE4 has more than one, as for its depth of field). Its blur length is the shutter angle's share
    // (RE7: ShutterAngleEnable, 180 degrees) or ExposureFrame (RE4: angle off, 0.2 of the frame): whichever the camera uses
    // is set to 0 while DLSS runs, each one's own value remembered and put back.
    static const auto get_exposure = refw::find_method("via.render.MotionBlur", "get_ExposureFrame");
    static const auto set_exposure = refw::find_method("via.render.MotionBlur", "set_ExposureFrame");
    static std::unordered_map<void*, float> game_values{};
    const auto count = std::min((int32_t)refw::call(get_length, array), 8);
    float shutter = 0.5f;

    for (int32_t i = 0; i < count; ++i) {
        uint64_t index = (uint64_t)i;
        const auto component = (void*)refw::call_args(get_value, array, &index, 1);

        if (component == nullptr) {
            continue;
        }

        const auto angle_on = get_angle_enable == nullptr || (refw::call(get_angle_enable, component) & 0xFF) != 0;
        const auto getter = angle_on ? get_angle : get_exposure;
        const auto setter = angle_on ? set_angle : set_exposure;

        if (getter == nullptr || setter == nullptr) {
            continue;
        }

        const auto value = (float)std::bit_cast<double>(refw::call(getter, component));
        auto& own_value = game_values.try_emplace(component, angle_on ? 180.0f : 0.2f).first->second;

        if (value > 0.0001f) {
            own_value = value;
        }

        if (i == 0) {
            game_angle = angle_on ? own_value : own_value * 360.0f; // for the log
            shutter = angle_on ? own_value / 360.0f : own_value;
        }

        const auto wanted = take_over ? 0.0f : own_value;

        if (std::fabs(value - wanted) > 0.0001f) {
            uint64_t raw = std::bit_cast<uint64_t>((double)wanted);
            refw::call_args(setter, component, &raw, 1);
        }
    }

    if (game_values.size() > 64) {
        game_values.clear(); // scenes come and go: components of old ones are gone
    }

    g_motion_blur_shutter = std::clamp(shutter, 0.0f, 2.0f);
    const auto own = take_over && option;

    if (g_own_motion_blur.exchange(own) != own) {
        log_info("Motion blur: {} (the game's option {}, shutter {:.0f} degrees)", own ? "the plugin's after DLSS" : take_over ? "none (DLSS on)" : "the game's own",
            option ? "on" : "off", game_angle);
    }
}


// Game thread, each BeginRendering (profile own_bloom, RE8): the game's bloom (via.render.SoftBloom) gets an output ratio
// of 0 while DLSS runs (re-applied every frame; each component's own value remembered and put back without DLSS); the
// plugin's bloom after DLSS uses the game's settings, if the game's option (RenderConfig.BloomEnable) is on.
void update_own_bloom() {
    if (!game_profile::current().own_bloom) {
        return;
    }

    static const auto get_render_config = refw::find_method("via.render.Renderer", "get_RenderConfig");
    static const auto get_option = refw::find_method("via.render.RenderConfig", "get_BloomEnable");
    static const auto get_scene = refw::find_method("via.SceneManager", "get_CurrentScene");
    static const auto find_components = refw::find_method("via.Scene", "findComponents(System.Type)");
    static const auto get_length = refw::find_method("System.Array", "get_Length");
    static const auto get_value = refw::find_method("System.Array", "GetValue(System.Int32)");
    static const auto get_enabled = refw::find_method("via.render.SoftBloom", "get_Enabled");
    static const auto get_threshold = refw::find_method("via.render.SoftBloom", "get_Threshold");
    static const auto get_dispersion = refw::find_method("via.render.SoftBloom", "get_Dispersion");
    static const auto get_ratio = refw::find_method("via.render.SoftBloom", "get_OutputRatio");
    static const auto set_ratio = refw::find_method("via.render.SoftBloom", "set_OutputRatio");
    static const auto type = [] {
        const auto t = refw::find_type("via.render.SoftBloom");
        return t != nullptr ? (void*)refw::sdk()->type_definition->get_runtime_type(t) : nullptr;
    }();
    static std::unordered_map<void*, float> game_ratios{};

    const auto renderer = refw::native_singleton("via.render.Renderer");
    const auto config = renderer != nullptr ? (void*)refw::call(get_render_config, renderer) : nullptr;
    const auto manager = refw::native_singleton("via.SceneManager");
    const auto scene = manager != nullptr ? (void*)refw::call(get_scene, manager) : nullptr;

    if (config == nullptr || get_option == nullptr || scene == nullptr || type == nullptr || find_components == nullptr || get_length == nullptr ||
        get_value == nullptr || get_enabled == nullptr || get_ratio == nullptr || set_ratio == nullptr)
    {
        g_own_bloom = false;
        return;
    }

    uint64_t arg = (uint64_t)type;
    const auto array = (void*)refw::call_args(find_components, scene, &arg, 1);
    const auto count = array != nullptr ? std::min((int32_t)refw::call(get_length, array), 8) : 0;
    const auto option = (refw::call(get_option, config) & 0xFF) != 0;
    const auto d = framegen::dlss();
    const auto take_over = active() && !g_dlss_paused && d != nullptr && d->own_bloom;
    bool found = false;
    float threshold = 1.0f;
    float dispersion = 0.5f;
    float ratio = 0.0f;
    const auto get = [](REFrameworkMethodHandle m, void* o, float fallback) {
        return m != nullptr ? (float)std::bit_cast<double>(refw::call(m, o)) : fallback;
    };

    for (int32_t i = 0; i < count; ++i) {
        uint64_t index = (uint64_t)i;
        const auto component = (void*)refw::call_args(get_value, array, &index, 1);

        if (component == nullptr) {
            continue;
        }

        const auto value = get(get_ratio, component, 0.0f);
        auto& own_value = game_ratios.try_emplace(component, value).first->second;

        if (value > 0.0001f) {
            own_value = value;
        }

        if (!found && (refw::call(get_enabled, component) & 0xFF) != 0) {
            found = true;
            threshold = get(get_threshold, component, 1.0f);
            dispersion = get(get_dispersion, component, 0.5f);
            ratio = own_value;
        }

        const auto wanted = take_over ? 0.0f : own_value;

        if (std::fabs(value - wanted) > 0.0001f) {
            uint64_t raw = std::bit_cast<uint64_t>((double)wanted);
            refw::call_args(set_ratio, component, &raw, 1);
        }
    }

    if (game_ratios.size() > 64) {
        game_ratios.clear();
    }

    g_bloom_threshold = threshold;
    g_bloom_dispersion = dispersion;
    g_bloom_ratio = ratio;
    const auto own = take_over && option && found && ratio > 0.0f;

    // Logged on a change of state, and the settings at most every 5 s (the game animates them, e.g. between zones).
    static ULONGLONG last_log = 0;
    const auto now = GetTickCount64();

    if (g_own_bloom.exchange(own) != own || (own && now - last_log >= 5000)) {
        last_log = now;
        log_info("Bloom: {} (the game's option {}, {} SoftBloom component(s); threshold {:.3f}, dispersion {:.3f}, output ratio {:.4f})",
            own ? "the plugin's after DLSS" : take_over ? "none (DLSS on)" : "the game's own", option ? "on" : "off", count, threshold, dispersion,
            ratio);
    }
}

// Whether a GUI game object of this name is in the current scene and drawn (RE2's GUI_ItemBox).
// Whether a game object of this name is in the current scene at all (drawn or not: RE7's title scene, its TitleScreen).
bool gui_object_exists(const char* name) {
    static const auto get_scene = refw::find_method("via.SceneManager", "get_CurrentScene");
    static const auto find_game_object = refw::find_method("via.Scene", "findGameObject(System.String)");

    if (get_scene == nullptr || find_game_object == nullptr) {
        return false;
    }

    const auto manager = refw::native_singleton("via.SceneManager");
    const auto scene = manager != nullptr ? (void*)refw::call(get_scene, manager) : nullptr;
    uint64_t arg = scene != nullptr ? (uint64_t)refw::sdk()->functions->create_managed_string_normal(name) : 0;
    return arg != 0 && (void*)refw::call_args(find_game_object, scene, &arg, 1) != nullptr;
}

bool gui_object_shown(const char* name) {
    static const auto get_scene = refw::find_method("via.SceneManager", "get_CurrentScene");
    static const auto find_game_object = refw::find_method("via.Scene", "findGameObject(System.String)");
    static const auto get_draw_self = refw::find_method("via.GameObject", "get_DrawSelf");

    if (get_scene == nullptr || find_game_object == nullptr || get_draw_self == nullptr) {
        return false;
    }

    const auto manager = refw::native_singleton("via.SceneManager");
    const auto scene = manager != nullptr ? (void*)refw::call(get_scene, manager) : nullptr;
    uint64_t arg = scene != nullptr ? (uint64_t)refw::sdk()->functions->create_managed_string_normal(name) : 0;
    const auto game_object = arg != 0 ? (void*)refw::call_args(find_game_object, scene, &arg, 1) : nullptr;
    return game_object != nullptr && (refw::call(get_draw_self, game_object) & 0xFF) != 0;
}

// Whether a GUI component of this type is in the current scene with its game object drawn (RE3's offline.gui.SaveBehavior,
// offline.gui.EsInventoryDetailBehavior). Logs the count and drawn count whenever they change, per type.
bool gui_component_shown(const char* type_name) {
    static const auto get_scene = refw::find_method("via.SceneManager", "get_CurrentScene");
    static const auto find_components = refw::find_method("via.Scene", "findComponents(System.Type)");
    static const auto get_length = refw::find_method("System.Array", "get_Length");
    static const auto get_value = refw::find_method("System.Array", "GetValue(System.Int32)");
    static const auto get_game_object = refw::find_method("via.Component", "get_GameObject");
    static const auto get_draw_self = refw::find_method("via.GameObject", "get_DrawSelf");
    static std::unordered_map<std::string, std::pair<void*, uint64_t>> types{}; // runtime type, last (count, drawn) logged

    if (get_scene == nullptr || find_components == nullptr || get_length == nullptr || get_value == nullptr || get_game_object == nullptr ||
        get_draw_self == nullptr)
    {
        return false;
    }

    auto& entry = types[type_name];

    if (entry.first == nullptr) {
        const auto t = refw::find_type(type_name);
        entry.first = t != nullptr ? (void*)refw::sdk()->type_definition->get_runtime_type(t) : nullptr;
        entry.second = ~0ull;

        if (entry.first == nullptr) {
            log_info("GUI: no type {}", type_name);
            entry.first = (void*)1; // not looked up again
        }
    }

    if (entry.first == (void*)1) {
        return false;
    }

    const auto manager = refw::native_singleton("via.SceneManager");
    const auto scene = manager != nullptr ? (void*)refw::call(get_scene, manager) : nullptr;
    uint64_t arg = (uint64_t)entry.first;
    const auto array = scene != nullptr ? (void*)refw::call_args(find_components, scene, &arg, 1) : nullptr;
    const auto count = array != nullptr ? std::min((int32_t)refw::call(get_length, array), 16) : 0;
    int32_t drawn = 0;

    for (int32_t i = 0; i < count; ++i) {
        uint64_t index = (uint64_t)i;
        const auto component = (void*)refw::call_args(get_value, array, &index, 1);
        const auto game_object = component != nullptr ? (void*)refw::call(get_game_object, component) : nullptr;

        if (game_object != nullptr && (refw::call(get_draw_self, game_object) & 0xFF) != 0) {
            ++drawn;
        }
    }

    if (const auto state = ((uint64_t)count << 32) | (uint32_t)drawn; state != entry.second) {
        entry.second = state;
        log_info("GUI: {} - {} in the scene, {} drawn", type_name, count, drawn);
    }

    return drawn > 0;
}
} // namespace

namespace dlss_pass {

void on_device(ID3D12Device* device) {
    g_device = device;

    if (!cl_state::install(device)) {
        return;
    }

    cl_state::set_clear_handler(&on_clear);
    hud_split::on_device(device);
    g_ready = true;
}

void on_begin_rendering() {
    update_motion_blur();
    update_own_bloom();
    // The guard's live rule only on its screen (profile guard_live_rule: RE2's item box), checked every 10th frame: elsewhere
    // (the bolt cutter animation) a scene shown paused behind another image still changes every frame with the jitter.
    // RE3: by GUI component type (its save screen); the examine view's GUI component too (DLAA and the stabilizer there).
    {
        const auto& profile = game_profile::current();
        static uint32_t frame = 0;

        if ((profile.guard_live_rule != nullptr || profile.guard_live_component != nullptr || profile.examine_component != nullptr ||
                profile.min_half_object != nullptr) &&
            frame++ % 10 == 0)
        {
            if (profile.min_half_object != nullptr) {
                const auto title = gui_object_exists(profile.min_half_object);

                if (g_half_screen.exchange(title) != title) {
                    log_info("DLSS: {} {}: render size {}", profile.min_half_object, title ? "in the scene" : "gone",
                        title ? "not below half the output (Ultra Performance as Performance)" : "the mode's own");
                }
            }

            if (profile.guard_live_rule != nullptr || profile.guard_live_component != nullptr) {
                const auto screen = profile.guard_live_rule != nullptr ? profile.guard_live_rule : profile.guard_live_component;
                const auto shown = profile.guard_live_rule != nullptr ? gui_object_shown(screen) : gui_component_shown(screen);

                if (g_live_rule_screen.exchange(shown) != shown) {
                    log_info("Upscale guard: live rule {} ({})", shown ? "on" : "off", screen);
                }
            }

            if (profile.examine_component != nullptr) {
                own_dof::set_examine_hint(gui_component_shown(profile.examine_component));
            }
        }
    }

    // The engine's sharpening: off while DLSS is on (setting dlss_sharpening_off, unless another mod does it), the game's
    // own otherwise (DLSS off, or paused with the comparison key).
    // Videotape mode keeps it by default (setting dlss_tape_sharpening): vanilla sharpens those scenes (its TAA modes).
    // Diagnostics: where the game uses depth of field (probe_dof, games without the plugin's own).
    if (const auto d = framegen::dlss(); d != nullptr && d->probe_dof && !game_profile::current().own_dof) {
        own_dof::probe();
    }

    // RE2's item examine (setting dlss_examine_dlaa, menu, off by default): DLSS's mode is DLAA there (a rotating item stays
    // sharper), the chosen mode again when it ends. Detected by the game's depth of field mode (last frame's values).
    {
        const auto d = framegen::dlss();
        const auto examining = game_profile::current().examine_dlss_off && game_profile::current().own_dof && d != nullptr && d->examine_dlaa &&
            own_dof::examining();

        // At once; back only after 20 frames without it (it dropped out for single frames while examining: each switch sets
        // DLSS up again).
        static int frames_without = 0;
        frames_without = examining ? 0 : frames_without + 1;
        const auto dlaa = examining || (g_examine_dlaa.load() && frames_without < 20 && d != nullptr && d->examine_dlaa);

        if (g_examine_dlaa.exchange(dlaa) != dlaa) {
            log_info("DLSS: {}", dlaa ? "item examine: DLAA there (dlss_examine_dlaa)" : "item examine over: the chosen mode again");
        }
    }

    // The game's depth of field: the plugin's instead while DLSS runs (profile own_dof; not in tape scenes left to the game).
    if (game_profile::current().own_dof) {
        const auto d = framegen::dlss();
        // RE8 (profile own_dof_examine_only): only in the item examine view (its GUI component), the game's own elsewhere.
        const auto here = !game_profile::current().own_dof_examine_only || own_dof::examining();
        own_dof::on_begin_rendering(active() && !g_dlss_paused && d != nullptr && d->own_dof && here && !(g_tape.load() && d->tape_off));
    }

    engine_hooks::set_zero_sharpness(active() && !g_dlss_paused && framegen::dlss()->sharpening_off && !(g_tape.load() && (framegen::dlss()->tape_sharpening || framegen::dlss()->tape_off)));
    engine_hooks::set_no_temporal_aa(active() && !g_dlss_paused && !g_tape.load() && framegen::dlss()->engine_aa < 0);

    // RE2 (profile sharpness_setting): RenderConfig.SharpnessSetting 0 made the scene DLSS gets far too bright (gameplay and
    // title screen blown out, splash fades to white); 1 is right (RE2_DisableSharpeningAndTAA.lua sets it too). 1 while
    // DLSS runs, the game's own value back when it is off.
    if (const auto wanted_setting = game_profile::current().sharpness_setting; wanted_setting >= 0) {
        static const auto get_render_config = refw::find_method("via.render.Renderer", "get_RenderConfig");
        static const auto get_sharpness = refw::find_method("via.render.RenderConfig", "get_SharpnessSetting");
        static const auto set_sharpness = refw::find_method("via.render.RenderConfig", "set_SharpnessSetting");
        static int original = -1;
        const auto renderer = refw::native_singleton("via.render.Renderer");
        const auto config = renderer != nullptr ? (void*)refw::call(get_render_config, renderer) : nullptr;

        if (config != nullptr && get_sharpness != nullptr && set_sharpness != nullptr) {
            const auto current = (int)(refw::call(get_sharpness, config) & 0xFFFFFFFF);
            const auto on = active() && !g_dlss_paused && !g_tape.load();

            if (on && current != wanted_setting) {
                if (original < 0) {
                    original = current;
                }

                uint64_t value = (uint64_t)wanted_setting;
                refw::call_args(set_sharpness, config, &value, 1);
                log_info("DLSS: the game's SharpnessSetting {} -> {} while DLSS runs", current, wanted_setting);
            } else if (!on && original >= 0) {
                uint64_t value = (uint64_t)original;
                refw::call_args(set_sharpness, config, &value, 1);
                log_info("DLSS: the game's SharpnessSetting back to {}", original);
                original = -1;
            }
        }
    }

    // DLSS switched off (the menu, or the item examine setting): the game's own render scale back, as it was before the plugin
    // changed it.
    if (!active()) {
        if (const auto original = g_original_rate.exchange(-1.0f); original >= 0.0f) {
            static const auto get_render_config = refw::find_method("via.render.Renderer", "get_RenderConfig");
            static const auto set_rate = refw::find_method("via.render.RenderConfig", "set_ImageQualityRate");
            const auto renderer = refw::native_singleton("via.render.Renderer");
            const auto config = renderer != nullptr ? (void*)refw::call(get_render_config, renderer) : nullptr;

            if (config != nullptr) {
                uint64_t arg = std::bit_cast<uint64_t>((double)original);
                refw::call_args(set_rate, config, &arg, 1);
                log_info("DLSS: off, the game's render scale back to {:.3f}", original);
            }

            g_wanted_rate = 0.0f;
            g_applied_rate = -1.0f;
            g_no_upscale_fallback = false;
            g_markers_without_upscale = 0;
            g_output.configured = false; // configured again (and the render scale set) when DLSS comes back
            g_working = false;
        }

        return;
    }

    // The DLSS key (setting dlss_toggle_key, F7 by default) switches DLSS off and on live (the engine's own upscale, at the
    // same render scale), to compare.
    if (const auto key = framegen::dlss()->toggle_key; key == 0) {
        if (g_dlss_paused.exchange(false)) {
            log_info("DLSS: ON (diagnostic hotkeys off)");
        }
    } else {
        static bool was_down = false;
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const auto down = pid == GetCurrentProcessId() && (GetAsyncKeyState(key) & 0x8000) != 0;

        if (down && !was_down) {
            const auto paused = !g_dlss_paused.exchange(!g_dlss_paused.load());
            log_info("DLSS: {} (key)", paused ? "OFF (the game's own upscale)" : "ON");
        }

        was_down = down;
    }

    // The LOD bias key (setting dlss_lod_bias_key, F8 by default) switches the automatic LOD bias off and on live, to compare.
    if (const auto key = framegen::dlss()->lod_bias_key; key == 0) {
        if (g_lod_bias_paused.exchange(false)) {
            log_info("DLSS: automatic LOD bias ON (diagnostic hotkeys off)");
        }
    } else {
        static bool was_down = false;
        DWORD pid{};
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const auto down = pid == GetCurrentProcessId() && (GetAsyncKeyState(key) & 0x8000) != 0;

        if (down && !was_down) {
            const auto paused = !g_lod_bias_paused.exchange(!g_lod_bias_paused.load());
            log_info("DLSS: automatic LOD bias {} (key)", paused ? "OFF (the game's own)" : "ON");
        }

        was_down = down;
    }

    static const auto get_render_config = refw::find_method("via.render.Renderer", "get_RenderConfig");
    static const auto get_rate = refw::find_method("via.render.RenderConfig", "get_ImageQualityRate");
    static const auto set_rate = refw::find_method("via.render.RenderConfig", "set_ImageQualityRate");
    static const auto get_aa = refw::find_method("via.render.RenderConfig", "get_AntiAliasing");
    static const auto set_aa = refw::find_method("via.render.RenderConfig", "set_AntiAliasing");

    const auto renderer = refw::native_singleton("via.render.Renderer");
    const auto config = renderer != nullptr ? (void*)refw::call(get_render_config, renderer) : nullptr;

    if (config == nullptr) {
        return;
    }

    // The engine's TAA would jitter too and blur DLSS's output: off (via.render.RenderConfig.AntiAliasingType NONE = 4).
    // Experiments (setting dlss_engine_aa >= 0): that anti-aliasing type instead. Videotape mode: TAA (1), the tape scenes'
    // upscale step only exists with it (the plugin's jitter replaces the engine's then, see on_scene_updated).
    const auto wanted_aa = framegen::dlss()->engine_aa >= 0 ? framegen::dlss()->engine_aa : g_tape.load() ? 1 : -1;

    if (const auto wanted = wanted_aa; wanted >= 0) {
        if (const auto aa = (int)refw::call(get_aa, config); aa != wanted) {
            uint64_t value = (uint64_t)wanted;
            refw::call_args(set_aa, config, &value, 1);
            log_info("DLSS: the game's anti-aliasing {} -> {}{}", aa, wanted, g_tape.load() ? " (videotape mode)" : " (dlss_engine_aa)");
        }
    } else if (const auto aa = (int)refw::call(get_aa, config); aa == 1 || aa == 2) {
        uint64_t none = 4;
        refw::call_args(set_aa, config, &none, 1);
        log_info("DLSS: the game's TAA (anti-aliasing {}) turned off", aa);
    }

    // Float arguments and results go through the invoke wrapper as doubles.
    const auto wanted = g_wanted_rate.load();
    const auto current = (float)std::bit_cast<double>(refw::call(get_rate, config));

    if (static bool logged = false; !logged && wanted > 0.0f) {
        logged = true;
        log_info("DLSS: the game's render scale is {:.4f}, DLSS wants {:.4f}", current, wanted);
    }

    // Also when the engine reports the wanted value already: at startup it reported 1.0 while still rendering at the 0.5
    // saved from the previous session, so native DLAA never got native. Each new wanted value is set once.
    // The engine only acts on a change of the value, so the same value is first nudged (applied on the next frame).
    // Only when the size it really renders at isn't the wanted one: RE7 starting at its own 50% (already what DLSS wanted)
    // got two changes in two frames from the nudge and showed black, teal or oversharpened menus. Not before any frame
    // was seen.
    if (wanted > 0.0f && std::fabs(current - wanted) <= 0.0001f && std::fabs(g_applied_rate.load() - wanted) > 0.0001f) {
        const auto seen_w = g_seen_render_size[0].load();
        const auto seen_h = g_seen_render_size[1].load();

        if (seen_w == 0 || seen_h == 0) {
            return; // no frame yet: decide once one shows the size
        }

        const auto expected_w = g_output.render_expected[0];
        const auto expected_h = g_output.render_expected[1];

        if (expected_w > 0 && seen_w + 2 >= expected_w && seen_w <= expected_w + 2 && seen_h + 2 >= expected_h && seen_h <= expected_h + 2) {
            g_applied_rate = wanted;
            log_info("DLSS: render scale {:.4f} already applied ({}x{}): no nudge", wanted, seen_w, seen_h);
            return;
        }
    }

    if (wanted > 0.0f && std::fabs(current - wanted) <= 0.0001f && std::fabs(g_applied_rate.load() - wanted) > 0.0001f) {
        uint64_t nudge = std::bit_cast<uint64_t>((double)(wanted - 0.01f));
        refw::call_args(set_rate, config, &nudge, 1);
        g_applied_rate = wanted - 0.01f;
        log_info("DLSS: render scale reported as {:.4f} already: nudged to apply it", wanted);
        return;
    }

    if (wanted > 0.0f && std::fabs(current - wanted) > 0.0001f) {
        g_applied_rate = wanted;
        if (g_original_rate.load() < 0.0f) {
            g_original_rate = current;
        }

        uint64_t arg = std::bit_cast<uint64_t>((double)wanted);
        refw::call_args(set_rate, config, &arg, 1);
        log_info("DLSS: render scale (ImageQualityRate) {:.4f} -> {:.4f}", current, wanted);
    }
}

// Diagnostics (dlss_debug=13, research: RE2's fixed hair/shadow dither): the per-frame constants after SceneInfo's seven
// matrices, recorded for 180 frames; every 32-bit value that changes is logged (how often, its first values as int and
// float): frame counters, noise offsets, jitter. Bounded by the memory region it lies in.
void probe_scene_info(const void* scene_info) {
    constexpr size_t START = 0;                // from the start (the matrices too)
    constexpr size_t WORDS = 1024;             // 4 KB
    constexpr int FRAMES = 180;
    static std::vector<std::array<uint32_t, WORDS>> frames{};
    static bool done = false;

    if (done) {
        return;
    }

    MEMORY_BASIC_INFORMATION info{};
    const auto begin = (const uint8_t*)scene_info + START;

    if (VirtualQuery(begin, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) {
        return;
    }

    const auto available = (size_t)((const uint8_t*)info.BaseAddress + info.RegionSize - begin) / 4;
    const auto words = std::min(WORDS, available);
    std::array<uint32_t, WORDS> now{};
    std::memcpy(now.data(), begin, words * 4);
    frames.push_back(now);

    if ((int)frames.size() < FRAMES) {
        return;
    }

    done = true;
    log_info("probe_scene_info: {} frames, {} words after the matrices (+0x{:x}); values that changed:", frames.size(), words, START);

    for (size_t w = 0; w < words; ++w) {
        int changes = 0;
        std::set<uint32_t> distinct{};

        for (size_t f = 0; f < frames.size(); ++f) {
            distinct.insert(frames[f][w]);
            if (f > 0 && frames[f][w] != frames[f - 1][w]) ++changes;
        }

        if (changes == 0) {
            continue;
        }

        std::string ints{};
        std::string floats{};

        for (size_t f = 0; f < 8; ++f) {
            ints += std::format(" {}", frames[f][w]);
            floats += std::format(" {:.5g}", std::bit_cast<float>(frames[f][w]));
        }

        log_info("  +0x{:x}: {} changes, {} distinct; int{}; float{}", START + w * 4, changes, distinct.size(), ints, floats);
    }

    frames.clear();
}

void on_scene_updated(void* scene_layer) {
    if (!active() || scene_layer == nullptr || scene_layer != scene_inputs::main_scene()) {
        return;
    }

    const auto main = (SceneInfo*)refw::reflection_pointer(scene_layer, "SceneInfo");

    if (main == nullptr) {
        return;
    }

    if (const auto d = framegen::dlss(); d != nullptr && d->debug == 13) {
        probe_scene_info(main);
    }

    Camera camera{};
    {
        std::scoped_lock _{g_mutex};
        camera.prev_view = g_camera.valid ? g_camera.view : main->view;
        camera.prev_projection = g_camera.valid ? g_camera.projection : main->projection;
    }

    camera.valid = true;
    camera.view = main->view;
    camera.projection = main->projection;

    // Camera cuts: a jump of more than 1 m or 30 degrees in one frame, the field of view changing by more than 10%, a new
    // main scene, or DLSS resuming after frames it didn't run on. No motion is computed across a cut either.
    {
        static void* last_scene = nullptr;
        static bool was_working = false;
        const auto previous = camera.prev_view.inverse();
        const auto current = camera.view.inverse();
        const float dx = current.m[3][0] - previous.m[3][0];
        const float dy = current.m[3][1] - previous.m[3][1];
        const float dz = current.m[3][2] - previous.m[3][2];
        const auto moved = std::sqrt(dx * dx + dy * dy + dz * dz);

        // The angle between the forward axes (third columns).
        const float dot = current.m[2][0] * previous.m[2][0] + current.m[2][1] * previous.m[2][1] + current.m[2][2] * previous.m[2][2];
        const auto turned = std::acos(std::clamp(dot, -1.0f, 1.0f)) * 57.2957795f;
        const auto fov_ratio = camera.prev_projection.m[1][1] != 0.0f ? camera.projection.m[1][1] / camera.prev_projection.m[1][1] : 1.0f;
        const auto working = g_working.load();
        const char* reason = nullptr;

        if (scene_layer != last_scene) {
            reason = "new scene";
        } else if (working && !was_working) {
            reason = "DLSS resumed";
        } else if (moved > 1.0f) {
            reason = "camera jump";
        } else if (turned > 30.0f) {
            reason = "camera turn";
        } else if (fov_ratio > 1.1f || fov_ratio < 0.9f) {
            reason = "field of view change";
        }

        last_scene = scene_layer;
        was_working = working;

        // Only with the setting dlss_cut_reset (off by default): the reset makes frame generation skip the cut frame, a brief
        // frame rate dip that is more visible than the ghosting it avoids.
        if (reason != nullptr) {
            const auto reset = framegen::dlss() != nullptr && framegen::dlss()->cut_reset;
            ++g_cuts;
            g_cut_reason = reason;

            if (reset) {
                camera.cut = true;
                camera.prev_view = camera.view;
                camera.prev_projection = camera.projection;
            }

            if (g_cuts_logged++ < 50) {
                log_info("DLSS: camera cut ({}: {:.2f} m, {:.1f} deg, fov x{:.3f}){}", reason, moved, turned, fov_ratio,
                    reset ? ": history reset" : " (no reset: dlss_cut_reset=0)");
            }
        }
    }

    static const auto get_camera = refw::find_method("via.render.layer.Scene", "get_Camera");
    static const auto get_near = refw::find_method("via.Camera", "get_NearClipPlane");
    static const auto get_far = refw::find_method("via.Camera", "get_FarClipPlane");

    if (const auto cam = (void*)refw::call(get_camera, scene_layer)) {
        camera.near_plane = (float)std::bit_cast<double>(refw::call(get_near, cam));
        camera.far_plane = (float)std::bit_cast<double>(refw::call(get_far, cam));
    }

    // Halton(2,3) in [-0.5, 0.5] render pixels, with enough phases for the upscale ratio (DLSS: 8 x ratio squared).
    const auto render_w = g_render_size[0].load();
    const auto render_h = g_render_size[1].load();
    const auto out_w = g_output_size[0].load();
    float x = 0.0f;
    float y = 0.0f;
    float shift_change[2]{}; // RE2 [EXPERIMENTAL] dither tests: last frame's shift - this frame's, clip space (else 0)

    if (g_working && render_w > 0 && render_h > 0 && out_w > 0) {
        const auto ratio = (float)out_w / (float)render_w;
        const auto phases = std::max(8, (int)(8.0f * ratio * ratio));
        const auto i = (int)(++g_jitter_index % (uint32_t)phases) + 1;
        x = halton(i, 2) - 0.5f;
        y = halton(i, 3) - 0.5f;
        camera.jitter[0] = -x;
        camera.jitter[1] = -y;
        const auto dlss = framegen::dlss();
        // Debug 2: visible jitter (20x, DLSS told 1x).
        // RE2's item examine (profile examine_stabilizer, setting dlss_examine_no_jitter): no jitter there: a pass after the
        // upscale (the item's cut-out from the background, render-sized) shimmered with it along the outline (measured 0.58% of
        // the pixels with the stabilizer, 0.07% without jitter; DLSS's detail looked the same).
        const auto examine_still = game_profile::current().examine_stabilizer && dlss != nullptr && dlss->examine_no_jitter && own_dof::examining();
        const auto js = examine_still ? 0.0f : 1.0f;
        camera.jitter[0] *= js;
        camera.jitter[1] *= js;
        const auto scale = dlss != nullptr && dlss->debug == 2 ? 20.0f : js;
        // RE2 [EXPERIMENTAL] dither tests: whole pixels into the projection only (DLSS's inputs are moved back by them before
        // it runs); {0, 0} in every other game and when off.
        if (!examine_still) {
            re2_dither::pixel_shift(g_jitter_index, camera.dither_shift);
        }

        // The previous matrices get last frame's shift (below): the engine's motion vectors then hold the shift's change, so
        // its temporal passes (ray-traced reflection denoiser, the bloom stabilizer) follow the moved picture; DLSS's copy
        // has it taken out again (re2_dither::unshift).
        static int last_shift[2]{};
        camera.dither_prev_shift[0] = last_shift[0];
        camera.dither_prev_shift[1] = last_shift[1];
        last_shift[0] = camera.dither_shift[0];
        last_shift[1] = camera.dither_shift[1];
        shift_change[0] = scale * 2.0f * (float)(camera.dither_prev_shift[0] - camera.dither_shift[0]) / (float)render_w;
        shift_change[1] = scale * -2.0f * (float)(camera.dither_prev_shift[1] - camera.dither_shift[1]) / (float)render_h;

        x = scale * 2.0f * (x + (float)camera.dither_shift[0]) / (float)render_w; // to clip space
        y = scale * -2.0f * (y + (float)camera.dither_shift[1]) / (float)render_h;
    }

    // Videotape scenes with dlss_tape_off: the matrices as the engine made them (its TAA with its own jitter), as vanilla;
    // the plugin's history starts over when DLSS comes back.
    if (const auto d = framegen::dlss(); g_tape.load() && d != nullptr && d->tape_off) {
        for (auto& h : g_history) {
            h.valid = false;
        }

        std::scoped_lock _{g_mutex};
        g_camera = camera;
        return;
    }

    // Videotape mode: the game's TAA is on, so the engine jitters these projections itself: the plugin's jitter replaces
    // it (the jitter terms from the engine's unjittered projection), else the two would add up.
    const auto unjittered = g_tape.load() ? (SceneInfo*)refw::reflection_pointer(scene_layer, "JitterDisableSceneInfo") : nullptr;

    // Each view: this frame's jitter in its projection, and its previous matrices with the same jitter for the motion
    // vectors (so they come out without jitter).
    for (size_t i = 0; i < std::size(JITTERED_INFOS); ++i) {
        const auto info = (SceneInfo*)refw::reflection_pointer(scene_layer, JITTERED_INFOS[i]);

        if (info == nullptr) {
            continue;
        }

        if (unjittered != nullptr) {
            if (static int logged = 0; logged < 3 && i == 0) {
                ++logged;
                log_info("DLSS: videotape mode: engine jitter removed ({:+.6f}, {:+.6f} clip space)", info->projection.m[2][0] - unjittered->projection.m[2][0],
                    info->projection.m[2][1] - unjittered->projection.m[2][1]);
            }

            info->projection.m[2][0] = unjittered->projection.m[2][0];
            info->projection.m[2][1] = unjittered->projection.m[2][1];
        }

        auto& history = g_history[i];

        if (!history.valid) {
            history = {info->projection, info->view, true};
        }

        auto old_projection = history.projection;
        old_projection.m[2][0] += x + shift_change[0]; // shift_change: RE2 [EXPERIMENTAL] dither tests only, else 0
        old_projection.m[2][1] += y + shift_change[1];
        info->old_view_projection = old_projection * history.view;

        history.projection = info->projection;
        history.view = info->view;

        info->projection.m[2][0] += x;
        info->projection.m[2][1] += y;
        info->inverse_projection = info->projection.inverse();
        info->view_projection = info->projection * info->view;
        info->inverse_view_projection = info->view_projection.inverse();
    }

    std::scoped_lock _{g_mutex};
    g_camera = camera;
}

// Diagnostics (dlss_debug=10): a trace of everything the render thread records from the start of the main scene's
// PrePostEffect layer (its post effects, depth of field, the marker, the upscale), 3 frames.
void before_pre_post_effect_draw(void* layer) {
    const auto dlss = framegen::dlss();

    if (!active() || dlss == nullptr || dlss->debug != 10) {
        return;
    }

    static const auto get_parent = refw::find_method("via.render.RenderLayer", "get_Parent");

    if ((void*)refw::call(get_parent, layer) != scene_inputs::main_scene()) {
        return;
    }

    static int frames = 0;

    if (++frames == 60 || frames == 180 || frames == 300) {
        log_info("trace: ---- PrePostEffect start (thread {}), frame {}", GetCurrentThreadId(), frames);
        cl_state::trace(nullptr, 2500);
    }
}

void on_pre_post_effect_draw(void* layer, void* render_context) {
    if (!active() || render_context == nullptr) {
        return;
    }

    static std::once_flag once{};
    std::call_once(once, [] { g_engine_ready = find_engine_functions(); });

    if (!g_engine_ready) {
        return;
    }

    // Only the main scene.
    static const auto get_parent = refw::find_method("via.render.RenderLayer", "get_Parent");
    const auto scene = (void*)refw::call(get_parent, layer);

    if (scene == nullptr || scene != scene_inputs::main_scene()) {
        return;
    }

    // The full resolution scene target PrePostEffect upscales into (and the post effects, HUD and output use).
    const auto target = refw::reflection_pointer(scene, "PostMainTarget");

    if (render_objects::type_name(target) != "via.render.TargetState") {
        return;
    }

    const auto rtvs = *(void* const* const*)((const uint8_t*)target + 0x10);
    const auto rtv = rtvs != nullptr ? rtvs[0] : nullptr;

    if (render_objects::type_name(rtv) != "via.render.RenderTargetView") {
        return;
    }

    Slot slot{};
    slot.color = render_objects::d3d12_resource(refw::reflection_pointer(scene, "HDRTarget"));
    slot.depth = render_objects::d3d12_resource(refw::reflection_pointer(scene, "DepthStencilTex"));
    slot.motion = render_objects::d3d12_resource(refw::reflection_pointer(scene, "VelocityTarget"));
    slot.target = render_objects::d3d12_resource(target);

    uint32_t index{};
    {
        std::scoped_lock _{g_mutex};
        slot.camera = g_camera;
        index = g_next_slot++ % SLOTS;
        g_slots[index] = slot;
    }

    const auto command = g_alloc(render_context, g_clear_type, CLEAR_SIZE);

    if (command == nullptr) {
        return;
    }

    float color[4] = {MARKER[0], MARKER[1], MARKER[2], MARKER[3] + (float)index};
    *(uint32_t*)(command + 0x10) = 0;
    *(void**)(command + 0x18) = *(void**)((uint8_t*)render_context + RENDER_CONTEXT_TARGET_STATE);
    *(void**)(command + 0x20) = rtv;
    std::memcpy(command + 0x28, color, sizeof(color));
}

std::string status() {
    finish_readbacks();
    finish_color_readbacks();

    if (!g_ready) {
        return "DLSS: unavailable";
    }

    log_info("DLSS: {}", gpu_timer::report());

    // (No engine calls on scene_inputs::main_scene() here: it is only a remembered address and the engine frees and
    // recreates its scenes, e.g. on resolution changes. Calling into a freed one crashed the game.)
    return std::format("DLSS: {} upscaled, {} skipped (last reason: {}), {} markers, {} camera cuts (last: {}); render {}x{} -> {}x{}",
        g_evaluated.exchange(0), g_skipped.exchange(0), g_skip_reason.load(), g_markers.exchange(0), g_cuts.exchange(0), g_cut_reason.load(),
        g_render_size[0].load(), g_render_size[1].load(), g_output_size[0].load(), g_output_size[1].load());
}
} // namespace dlss_pass

namespace dlss_pass {
float lod_scale(void* scene) {
    const auto dlss = framegen::dlss();

    if (dlss == nullptr || !dlss->lod_bias || g_lod_bias_paused || !g_working || scene != scene_inputs::main_scene()) {
        return 1.0f;
    }

    const auto render_w = g_render_size[0].load();
    const auto out_w = g_output_size[0].load();

    if (render_w == 0 || out_w == 0) {
        return 1.0f;
    }

    return std::exp2(std::log2((float)render_w / (float)out_w) + dlss->lod_bias_offset);
}
} // namespace dlss_pass

namespace dlss_pass {
void* scene_target_rtv(void* scene) {
    const auto target = scene != nullptr ? refw::reflection_pointer(scene, "PostMainTarget") : nullptr;

    if (render_objects::type_name(target) != "via.render.TargetState") {
        return nullptr;
    }

    const auto rtvs = *(void* const* const*)((const uint8_t*)target + 0x10);
    const auto rtv = rtvs != nullptr ? rtvs[0] : nullptr;
    return render_objects::type_name(rtv) == "via.render.RenderTargetView" ? rtv : nullptr;
}

ID3D12Resource* scene_target(void* scene) {
    return scene != nullptr ? render_objects::d3d12_resource(refw::reflection_pointer(scene, "PostMainTarget")) : nullptr;
}

bool queue_marker(void* render_context, void* rtv, float tag) {
    if (!g_engine_ready || render_context == nullptr || rtv == nullptr) {
        return false;
    }

    const auto command = g_alloc(render_context, g_clear_type, CLEAR_SIZE);

    if (command == nullptr) {
        return false;
    }

    const float color[4] = {MARKER[0], MARKER[1], MARKER[2], tag};
    *(uint32_t*)(command + 0x10) = 0;
    *(void**)(command + 0x18) = *(void**)((uint8_t*)render_context + RENDER_CONTEXT_TARGET_STATE);
    *(void**)(command + 0x20) = rtv;
    std::memcpy(command + 0x28, color, sizeof(color));
    return true;
}
} // namespace dlss_pass

namespace dlss_pass {
std::string status_values() {
    const auto render_w = g_render_size[0].load();
    const auto out_w = g_output_size[0].load();
    return std::format("dlss_running={}\nrender_width={}\nrender_height={}\noutput_width={}\noutput_height={}\nlod_scale={:.3f}\n",
        g_working.load() ? 1 : 0, render_w, g_render_size[1].load(), out_w, g_output_size[1].load(), lod_scale(scene_inputs::main_scene()));
}
} // namespace dlss_pass

namespace dlss_pass {
bool examine_dlaa() {
    return g_examine_dlaa.load();
}

bool half_limit_now() {
    const auto dlss = framegen::dlss();
    return dlss != nullptr && half_limit_active(*dlss);
}
} // namespace dlss_pass

namespace dlss_pass {
void before_prepare_output(void* layer, void* render_context) {
    if (!game_profile::current().missing_upscale_fallback || !active() || !g_engine_ready) {
        return;
    }

    static const auto get_parent = refw::find_method("via.render.RenderLayer", "get_Parent");
    const auto scene = (void*)refw::call(get_parent, layer);

    if (scene == nullptr || scene != scene_inputs::main_scene()) {
        return;
    }

    queue_marker(render_context, scene_target_rtv(scene), 3000.0f);
}
} // namespace dlss_pass
