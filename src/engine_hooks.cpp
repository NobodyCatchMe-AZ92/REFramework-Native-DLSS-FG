#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <string>

#include <MinHook.h>

#include "log.hpp"
#include "refw.hpp"
#include "dlss_pass.hpp"
#include "hud_split.hpp"
#include "scene_inputs.hpp"
#include "game_profile.hpp"

#include "engine_hooks.hpp"

namespace {
// MinHook on `target`, logging why it failed: MinHook's status, the module the target is in and its first bytes (a jump
// there means something else hooked it already).
bool hook_function(void* target, void* detour, void** original, const char* what) {
    auto status = MH_CreateHook(target, detour, original);

    if (status == MH_OK) {
        status = MH_EnableHook(target);
    }

    if (status == MH_OK) {
        return true;
    }

    HMODULE module{};
    wchar_t path[MAX_PATH]{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)target, &module);
    GetModuleFileNameW(module, path, MAX_PATH);
    std::string bytes{};

    for (int i = 0; i < 16 && !IsBadReadPtr((const uint8_t*)target + i, 1); ++i) {
        bytes += std::format("{:02x} ", ((const uint8_t*)target)[i]);
    }

    log_error("{}: hooking {:x} failed: {} (module {}, bytes {})", what, (uintptr_t)target, MH_StatusToString(status),
        std::filesystem::path(path).filename().string(), bytes);

    // Why no memory: executable memory anywhere at all, and the free regions within 2 GB of the target.
    if (status == MH_ERROR_MEMORY_ALLOC) {
        const auto anywhere = VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        const auto error = anywhere == nullptr ? GetLastError() : 0;
        if (anywhere != nullptr) VirtualFree(anywhere, 0, MEM_RELEASE);

        size_t free_regions = 0;
        size_t free_bytes = 0;
        size_t largest = 0;
        const auto lo = (uintptr_t)target > 0x7FFF0000 ? (uintptr_t)target - 0x7FFF0000 : 0x10000;
        const auto hi = (uintptr_t)target + 0x7FFF0000;
        MEMORY_BASIC_INFORMATION mbi{};

        for (auto at = lo; at < hi && VirtualQuery((void*)at, &mbi, sizeof(mbi)) != 0; at = (uintptr_t)mbi.BaseAddress + mbi.RegionSize) {
            if (mbi.State == MEM_FREE) {
                ++free_regions;
                free_bytes += mbi.RegionSize;
                largest = std::max(largest, (size_t)mbi.RegionSize);
            }
        }

        log_error("  RWX memory anywhere: {} (error {}); within 2 GB of it: {} free regions, {} MB free, largest {} KB",
            anywhere != nullptr ? "ok" : "FAILED", error, free_regions, free_bytes >> 20, largest >> 10);
    }

    return false;
}

std::atomic<uint32_t> g_view_get_size{0};
std::atomic<uint32_t> g_scene_update{0};
std::atomic<uint32_t> g_overlay_draw{0};
std::atomic<uint32_t> g_prepare_output_draw{0};

// --------------------------------------------------------------------------------------------------------------------
// Finding the engine function behind a type-database method: the method is a small wrapper that loads its arguments and
// calls the real function. Same patterns REFramework uses (the register moves before the call vary by game/TDB).
uint8_t* follow_thunk(uint8_t* code) {
    for (int i = 0; i < 4 && code != nullptr && code[0] == 0xE9; ++i) {
        code = code + 5 + *(int32_t*)(code + 1);
    }

    return code;
}

void* find_wrapped_call(void* wrapper, std::initializer_list<std::initializer_list<uint8_t>> patterns) {
    const auto code = follow_thunk((uint8_t*)wrapper);

    if (code == nullptr) {
        return nullptr;
    }

    for (const auto& pattern : patterns) {
        const auto size = pattern.size(); // ends with E8 (call rel32)

        for (size_t i = 0; i + size + 4 <= 1000; ++i) {
            if (IsBadReadPtr(code + i, size + 4)) {
                break;
            }

            if (std::memcmp(code + i, pattern.begin(), size) == 0) {
                const auto call = code + i + size - 1;
                return call + 5 + *(int32_t*)(call + 1);
            }
        }
    }

    return nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// via.SceneView.get_Size
using ViewGetSizeFn = float* (*)(void* scene_view, float* result);
ViewGetSizeFn g_view_get_size_original{};

float* view_get_size_hook(void* scene_view, float* result) {
    ++g_view_get_size;
    return g_view_get_size_original(scene_view, result);
}

bool hook_view_get_size() {
    const auto wrapper = refw::method_function(refw::find_method("via.SceneView", "get_Size"));

    if (wrapper == nullptr) {
        log_error("via.SceneView.get_Size not found in the type database");
        return false;
    }

    const auto native = find_wrapped_call(wrapper, {
        {0x49, 0x8B, 0xC8, 0xE8}, // mov rcx, r8; call
        {0x48, 0x8B, 0xCB, 0xE8}, // mov rcx, rbx; call
        {0x48, 0x89, 0xF2, 0xE8}, // mov rdx, rsi; call (TDB >= 74)
        {0x48, 0x8B, 0xCF, 0xE8}, // mov rcx, rdi; call
    });

    if (native == nullptr) {
        log_error("via.SceneView.get_Size: engine function not found in its wrapper");
        return false;
    }

    if (!hook_function(native, (void*)&view_get_size_hook, (void**)&g_view_get_size_original, "via.SceneView.get_Size")) {
        return false;
    }

    log_info("Hooked via.SceneView.get_Size (engine function {:x})", (uintptr_t)native);
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// via.render.layer.Scene.get_LODBias: the scene's texture LOD scale (UV derivative scale, 1 = none; the engine sets it
// to its own viewport ratio in its dynamic resolution mode). The DLSS pass multiplies it for its render scale.
struct Vec2 {
    float x, y;
};

using LodBiasFn = Vec2* (*)(void* scene, Vec2* result);
LodBiasFn g_lod_bias_original{};

std::atomic<uint32_t> g_lod_bias_calls{0};

Vec2* lod_bias_hook(void* scene, Vec2* result) {
    ++g_lod_bias_calls;
    const auto out = g_lod_bias_original(scene, result);
    const auto scale = dlss_pass::lod_scale(scene);

    if (out != nullptr && scale != 1.0f) {
        out->x *= scale;
        out->y *= scale;
    }

    return out;
}

bool hook_lod_bias() {
    const auto wrapper = refw::method_function(refw::find_method("via.render.layer.Scene", "get_LODBias"));

    if (wrapper == nullptr) {
        log_error("via.render.layer.Scene.get_LODBias not found in the type database: no automatic LOD bias");
        return false;
    }

    // The wrapper calls the engine function (scene, result); how depends on the game (profile).
    const auto native = game_profile::current().lod_bias_wrapper == game_profile::Profile::LodBiasWrapper::R8_SCENE
        ? find_wrapped_call(wrapper, {
              {0x49, 0x8B, 0xC8, 0xE8}, // mov rcx, r8; call (RE4: "mov rdx, rcx" before it passes the result buffer)
          })
        : find_wrapped_call(wrapper, {
              {0x48, 0x8D, 0x54, 0x24, 0x38, 0xE8}, // lea rdx, [rsp+38h]; call (RE2: after "mov rcx, rdx")
              {0x48, 0x8D, 0x54, 0x24, 0x30, 0xE8}, // lea rdx, [rsp+30h]; call
          });

    if (native == nullptr) {
        std::string bytes{};

        for (int i = 0; i < 96 && !IsBadReadPtr((const uint8_t*)wrapper + i, 1); ++i) {
            bytes += std::format("{:02x} ", ((const uint8_t*)wrapper)[i]);
        }

        log_error("via.render.layer.Scene.get_LODBias: engine function not found in its wrapper ({:x}: {}): no automatic LOD bias",
            (uintptr_t)wrapper, bytes);
        return false;
    }

    if (!hook_function(native, (void*)&lod_bias_hook, (void**)&g_lod_bias_original, "via.render.layer.Scene.get_LODBias")) {
        return false;
    }

    log_info("Hooked via.render.layer.Scene.get_LODBias (engine function {:x})", (uintptr_t)native);
    return true;
}

// --------------------------------------------------------------------------------------------------------------------
// via.render.ToneMapping.get_Sharpness. Its wrapper is "mov rcx, rdx; jmp" to the engine function (the component),
// followed 0x40 bytes later by the same getter on the component's render data (what the renderer calls). Both read
// (RE2 TDB 70 and RE4 TDB 71 alike):
//     [data + flag] != 0      -> 0 (sharpening off)
//     TAA mode 3 or 4         -> a fixed value (the forced sharpening the in-game setting doesn't change)
//     else                    -> the sharpness value
// Hooked only when both match exactly that code; returns 0 while set_zero_sharpness(true).
using SharpnessFn = float (*)(void* object);
SharpnessFn g_sharpness_original[2]{};
std::atomic<bool> g_zero_sharpness{};
std::atomic<bool> g_sharpness_hooked{};
std::atomic<uint32_t> g_sharpness_calls{0};

float sharpness_hook_component(void* object) {
    ++g_sharpness_calls;
    return g_zero_sharpness.load(std::memory_order_relaxed) ? 0.0f : g_sharpness_original[0](object);
}

float sharpness_hook_data(void* data) {
    ++g_sharpness_calls;
    return g_zero_sharpness.load(std::memory_order_relaxed) ? 0.0f : g_sharpness_original[1](data);
}

int sharpness_method_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void sharpness_method_post(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (ret_val != nullptr && g_zero_sharpness.load(std::memory_order_relaxed)) {
        *ret_val = nullptr; // 0.0 (all bits zero)
    }
}

bool hook_sharpness() {
    auto code = (uint8_t*)refw::method_function(refw::find_method("via.render.ToneMapping", "get_Sharpness"));
    constexpr uint8_t WRAPPER[] = {0x48, 0x8B, 0xCA, 0xE9};               // mov rcx, rdx; jmp
    constexpr uint8_t OFF_RETURNS_ZERO[] = {0x00, 0x74, 0x04, 0x0F, 0x57, 0xC0, 0xC3}; // ...0; je +4; xorps xmm0, xmm0; ret

    if (code == nullptr || IsBadReadPtr(code, 8) || std::memcmp(code, WRAPPER, sizeof(WRAPPER)) != 0) {
        log_info("via.render.ToneMapping.get_Sharpness: not the expected wrapper: the plugin leaves the engine's sharpening alone");
        return false;
    }

    const auto component_fn = follow_thunk(code + 3 + 5 + *(int32_t*)(code + 4));

    // mov rax, [rcx+30h]; cmp byte [rax+flag], 0; je +4; xorps xmm0, xmm0; ret
    constexpr uint8_t COMPONENT[] = {0x48, 0x8B, 0x41, 0x30, 0x80, 0xB8};

    if (IsBadReadPtr(component_fn, 0x80) || std::memcmp(component_fn, COMPONENT, sizeof(COMPONENT)) != 0 ||
        std::memcmp(component_fn + 10, OFF_RETURNS_ZERO, sizeof(OFF_RETURNS_ZERO)) != 0)
    {
        log_info("via.render.ToneMapping.get_Sharpness: engine function not as expected: the plugin leaves the engine's sharpening alone");
        return false;
    }

    // The render data variant: cmp byte [rcx+flag], 0 (same flag offset); je +4; xorps xmm0, xmm0; ret
    const auto flag = *(uint32_t*)(component_fn + 6);
    uint8_t* data_fn = nullptr;

    for (int i = 0x20; i < 0x70; ++i) {
        const auto p = component_fn + i;

        if (p[0] == 0x80 && p[1] == 0xB9 && *(uint32_t*)(p + 2) == flag && std::memcmp(p + 6, OFF_RETURNS_ZERO, sizeof(OFF_RETURNS_ZERO)) == 0) {
            data_fn = p;
            break;
        }
    }

    if (!hook_function(component_fn, (void*)&sharpness_hook_component, (void**)&g_sharpness_original[0], "via.render.ToneMapping.get_Sharpness")) {
        return false;
    }

    if (data_fn == nullptr ||
        !hook_function(data_fn, (void*)&sharpness_hook_data, (void**)&g_sharpness_original[1], "via.render.ToneMapping.get_Sharpness (render data)"))
    {
        log_warn("via.render.ToneMapping.get_Sharpness: render data variant not found: only the component's getter is hooked");
    }

    // Also the method itself through REFramework (as RE2_DisableSharpeningAndTAA.lua does): RE2's title screen and main menu
    // read the sharpness that way and stayed sharpened with only the engine functions hooked.
    if (const auto method = refw::find_method("via.render.ToneMapping", "get_Sharpness")) {
        refw::sdk()->functions->add_hook(method, &sharpness_method_pre, &sharpness_method_post, false);
    }

    g_sharpness_hooked = true;
    log_info("Hooked via.render.ToneMapping.get_Sharpness (engine functions {:x}, {:x})", (uintptr_t)component_fn, (uintptr_t)data_fn);
    return true;
}


// --------------------------------------------------------------------------------------------------------------------
// via.render.ToneMapping.getTemporalAA (RE2, profile temporal_aa_off): reports TAA "none" (5) while DLSS runs, as
// RE2_DisableSharpeningAndTAA.lua does. Without it RE2 keeps parts of its TAA route below the anti-aliasing setting
// (which the plugin sets to none): the upscale step went missing, gameplay and the title screen came out too bright,
// splash screens faded to white. A method hook through REFramework (both returning 5 together is harmless).
std::atomic<bool> g_no_temporal_aa{};
std::atomic<bool> g_temporal_aa_hooked{};

int temporal_aa_pre(int, void**, REFrameworkTypeDefinitionHandle*, unsigned long long) {
    return REFRAMEWORK_HOOK_CALL_ORIGINAL;
}

void temporal_aa_post(void** ret_val, REFrameworkTypeDefinitionHandle, unsigned long long) {
    if (ret_val != nullptr && g_no_temporal_aa.load(std::memory_order_relaxed)) {
        *ret_val = (void*)(uintptr_t)5;
    }
}

bool hook_temporal_aa() {
    const auto method = refw::find_method("via.render.ToneMapping", "getTemporalAA");

    if (method == nullptr) {
        log_info("via.render.ToneMapping.getTemporalAA not found: the game's TAA check stays as it is");
        return false;
    }

    refw::sdk()->functions->add_hook(method, &temporal_aa_pre, &temporal_aa_post, false);
    g_temporal_aa_hooked = true;
    log_info("Hooked via.render.ToneMapping.getTemporalAA (TAA reported off while DLSS runs)");
    return true;
}
// --------------------------------------------------------------------------------------------------------------------
// Render layers: draw/update are virtual. The vtable slot moved with engine versions (REFramework uses the same table).
uint32_t draw_vtable_index(uint32_t tdb) {
    if (tdb >= 81) return 15;
    if (tdb >= 69) return 14;
    if (tdb > 49) return 12;
    return 10;
}

using LayerFn = void (*)(void* layer, void* render_context);

struct LayerHook {
    const char* type{};
    bool update{}; // update instead of draw
    LayerFn hook{};
    LayerFn original{};
};

void scene_update_hook(void* layer, void* ctx);
void overlay_draw_hook(void* layer, void* ctx);
void prepare_output_draw_hook(void* layer, void* ctx);
void pre_post_effect_draw_hook(void* layer, void* ctx);

LayerHook g_layer_hooks[] = {
    {"via.render.layer.Scene", true, &scene_update_hook},
    {"via.render.layer.Overlay", false, &overlay_draw_hook},
    {"via.render.layer.PrepareOutput", false, &prepare_output_draw_hook},
    {"via.render.layer.PrePostEffect", false, &pre_post_effect_draw_hook},
};

void scene_update_hook(void* layer, void* ctx) {
    ++g_scene_update;
    scene_inputs::on_scene_update(layer);
    g_layer_hooks[0].original(layer, ctx);
    dlss_pass::on_scene_updated(layer);
}

void overlay_draw_hook(void* layer, void* ctx) {
    ++g_overlay_draw;
    hud_split::on_overlay_draw_pre(layer, ctx);
    g_layer_hooks[1].original(layer, ctx);
}

void prepare_output_draw_hook(void* layer, void* ctx) {
    ++g_prepare_output_draw;
    dlss_pass::before_prepare_output(layer, ctx);
    hud_split::prepare_output_draw(layer, ctx, g_layer_hooks[2].original);
    scene_inputs::on_prepare_output_draw(layer);
}

void pre_post_effect_draw_hook(void* layer, void* ctx) {
    dlss_pass::before_pre_post_effect_draw(layer); // diagnostics (dlss_debug=10: a trace from here)
    g_layer_hooks[3].original(layer, ctx);
    dlss_pass::on_pre_post_effect_draw(layer, ctx);
}

bool hook_layer(LayerHook& hook, uint32_t tdb) {
    const auto instance = refw::create_native_instance(hook.type);

    if (instance == nullptr) {
        log_error("{}: could not create an instance to read its vtable", hook.type);
        return false;
    }

    const auto vtable = *(void***)instance;
    const auto index = draw_vtable_index(tdb) + (hook.update ? 1 : 0);
    const auto target = vtable[index];

    // Hook the function itself, not the vtable entry: the layers the engine renders with don't all call through this
    // vtable (patching the entry caught nothing in RE2). MinHook chains with a hook already on the function
    // (REFramework hooks these too): its jump moves into our trampoline and still runs.
    if (!hook_function(target, (void*)hook.hook, (void**)&hook.original, hook.type)) {
        return false;
    }

    log_info("Hooked {} {} (vtable[{}] -> {:x})", hook.type, hook.update ? "update" : "draw", index, (uintptr_t)target);
    return true;
}
} // namespace

namespace engine_hooks {
bool install() {
    const auto tdb = refw::tdb_version();
    log_info("Type database version {}", tdb);

    if (tdb == 0) {
        log_error("Unknown type database: engine hooks not installed");
        return false;
    }

    if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_error("MinHook failed to initialize: {}", (int)status);
        return false;
    }

    bool ok = hook_view_get_size();
    hook_lod_bias(); // optional: without it DLSS works, only without the automatic texture LOD bias
    hook_sharpness(); // optional: without it the engine's own sharpening stays on DLSS's output

    if (game_profile::current().temporal_aa_off) {
        hook_temporal_aa();
    }

    for (auto& hook : g_layer_hooks) {
        ok = hook_layer(hook, tdb) && ok;
    }

    return ok;
}

Counts take_counts() {
    return {g_view_get_size.exchange(0), g_scene_update.exchange(0), g_overlay_draw.exchange(0), g_prepare_output_draw.exchange(0),
        g_lod_bias_calls.exchange(0), g_sharpness_calls.exchange(0)};
}

void set_zero_sharpness(bool zero) {
    if (g_zero_sharpness.exchange(zero) != zero && g_sharpness_hooked) {
        log_info("Engine sharpening: {}", zero ? "OFF (reads as 0 while DLSS is on)" : "the game's own");
    }
}

bool sharpness_hooked() {
    return g_sharpness_hooked;
}
} // namespace engine_hooks

namespace engine_hooks {
void set_no_temporal_aa(bool off) {
    if (g_no_temporal_aa.exchange(off) != off && g_temporal_aa_hooked) {
        log_info("The game's TAA check: {}", off ? "reports none (DLSS)" : "the game's own");
    }
}
} // namespace engine_hooks
