#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <vector>

#include "game_profile.hpp"
#include "log.hpp"
#include "refw.hpp"

#include "own_dof.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// --------------------------------------------------------------------------------------------------------------------
// The game's depth of field values (BeginRendering, game thread), for the render thread.
struct Lens {
    bool wanted{};      // the game has depth of field on this frame
    float f_number{12.0f};
    float focus{1.0f};  // focus distance (m)
    float sensor{0.023f}; // sensor size (m; taken as its height)
    float max_coc{2.0f};  // the largest blur, % of the screen height
    float near_scale{1.0f}; // blur in front of the focus (0: the game ignores it, BokehControl IgnoreNear - RE2's item examine)
    float far_scale{1.0f};  // ...and behind it (0: IgnoreFar)
    uint32_t bokeh_control{}; // DepthOfFieldBokehControl (0 Default, 1 IgnoreNear, 2 IgnoreFar)
};

std::atomic<bool> g_taken_over{};
std::atomic<bool> g_examine_hint{}; // RE3: its examine GUI drawn
Lens g_lens{};
SRWLOCK g_lens_lock = SRWLOCK_INIT;

REFrameworkMethodHandle method(const char* name) {
    if (const auto m = refw::find_method("via.render.DepthOfField", name)) return m;
    return refw::find_method("via.Component", name);
}

float get_float(void* object, REFrameworkMethodHandle getter, float fallback) {
    return getter != nullptr ? (float)std::bit_cast<double>(refw::call(getter, object)) : fallback;
}

// The scene's via.render.DepthOfField component on the camera rendering the main view (RE7 and RE2 have one).
void* find_depth_of_field() {
    static const auto get_scene = refw::find_method("via.SceneManager", "get_CurrentScene");
    static const auto find_components = refw::find_method("via.Scene", "findComponents(System.Type)");
    static const auto get_length = refw::find_method("System.Array", "get_Length");
    static const auto get_value = refw::find_method("System.Array", "GetValue(System.Int32)");
    static const auto type = [] {
        const auto t = refw::find_type("via.render.DepthOfField");
        return t != nullptr ? (void*)refw::sdk()->type_definition->get_runtime_type(t) : nullptr;
    }();

    const auto manager = refw::native_singleton("via.SceneManager");
    const auto scene = manager != nullptr ? (void*)refw::call(get_scene, manager) : nullptr;

    if (scene == nullptr || type == nullptr || find_components == nullptr || get_length == nullptr || get_value == nullptr) {
        return nullptr;
    }

    uint64_t arg = (uint64_t)type;
    const auto array = (void*)refw::call_args(find_components, scene, &arg, 1);

    if (array == nullptr || (int32_t)refw::call(get_length, array) < 1) {
        return nullptr;
    }

    // RE4 has two: ScopeCamera's (always on, focus 120 m) first, MainCamera's second. The one on the camera rendering the
    // main view (the primary camera's game object), else the first.
    static const auto get_main_view = refw::find_method("via.SceneManager", "get_MainView");
    static const auto get_primary_camera = refw::find_method("via.SceneView", "get_PrimaryCamera");
    static const auto get_game_object = refw::find_method("via.Component", "get_GameObject");
    const auto count = (int32_t)refw::call(get_length, array);
    const auto view = get_main_view != nullptr ? (void*)refw::call(get_main_view, manager) : nullptr;
    const auto camera = view != nullptr && get_primary_camera != nullptr ? (void*)refw::call(get_primary_camera, view) : nullptr;
    const auto camera_object = camera != nullptr && get_game_object != nullptr ? (void*)refw::call(get_game_object, camera) : nullptr;
    void* first = nullptr;

    // Diagnostics (RE8's cutscenes: the lens flipped between IgnoreNear and IgnoreFar every half second): every component,
    // logged when the set (which are enabled, which are on the primary camera) changes, at most twice a second. Looked at on
    // every 30th call only (this runs every frame: engine calls and a string per component each time otherwise).
    if (static uint32_t calls = 0; calls++ % 30 == 0) {
        static const auto get_enabled = refw::find_method("via.render.DepthOfField", "get_Enabled");
        static const auto get_f = refw::find_method("via.render.DepthOfField", "get_FNumber");
        static const auto get_focus = refw::find_method("via.render.DepthOfField", "get_FocusDistance");
        static const auto get_control = refw::find_method("via.render.DepthOfField", "get_BokehControl");
        static uint64_t last_signature = 0;
        static ULONGLONG last_log = 0;
        uint64_t signature = (uint64_t)count;
        std::string list{};

        for (int32_t i = 0; i < count && i < 8; ++i) {
            uint64_t index = (uint64_t)i;
            const auto component = (void*)refw::call_args(get_value, array, &index, 1);

            if (component == nullptr) {
                continue;
            }

            const auto on_primary = camera_object != nullptr && (void*)refw::call(get_game_object, component) == camera_object;
            const auto enabled = get_enabled != nullptr && (refw::call(get_enabled, component) & 0xFF) != 0;
            const auto control = get_control != nullptr ? (uint32_t)refw::call(get_control, component) : 0u;
            signature = signature * 31 + ((uint64_t)(uintptr_t)component >> 4) * 7 + (on_primary ? 2 : 0) + (enabled ? 1 : 0) + control * 11;
            list += std::format(" [{}{}{} bokeh {} f/{:.1f} {:.2f} m]", i, on_primary ? " primary" : "", enabled ? " on" : " off", control,
                get_float(component, get_f, 0.0f), get_float(component, get_focus, 0.0f));
        }

        if (signature != last_signature && GetTickCount64() - last_log >= 500) {
            last_signature = signature;
            last_log = GetTickCount64();
            log_info("Depth of field components: {}{}", count, list);
        }
    }

    for (int32_t i = 0; i < count; ++i) {
        uint64_t index = (uint64_t)i;
        const auto component = (void*)refw::call_args(get_value, array, &index, 1);

        if (component == nullptr) {
            continue;
        }

        if (first == nullptr) {
            first = component;
        }

        if (count == 1 || camera_object == nullptr) {
            break;
        }

        if ((void*)refw::call(get_game_object, component) == camera_object) {
            if (static void* logged = nullptr; logged != component) {
                logged = component;
                log_info("Depth of field: component {} of {} (on the primary camera)", i, count);
            }

            return component;
        }
    }

    return first;
}

// --------------------------------------------------------------------------------------------------------------------
// The GPU passes. 1: the circle of confusion at the output resolution, signed (behind the focus +, in front -; it orders
// like the depth), smoothed over time: the depth is render-sized and jittered, so where sharp meets blurred the nearest
// depth texel flips from frame to frame (a 2-pixel staircase at Performance that crawls). The previous frame's value is
// reprojected with the engine's motion vectors and clamped to the range of the current depth texels around the pixel
// (no ghosting), then blended in. 2: the gather on DLSS's image with that circle of confusion.
constexpr char COC_SHADER[] = R"(
Texture2D<float> depth_tex : register(t0);   // the engine's depth (render resolution, reversed)
Texture2D<float4> motion : register(t1);     // the engine's velocity (render resolution, xy in clip space units)
Texture2D<float> history : register(t2);     // the previous frame's smoothed circle of confusion (output size)
RWTexture2D<float> coc_out : register(u0);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);

cbuffer Constants : register(b0) {
    float2 inv_out;
    float2 depth_uv_scale;  // render extent / depth texture size
    float2 inv_depth;       // 1 / depth texture size
    float2 motion_uv_scale; // render extent / velocity texture size
    float p22, p32, p23;    // projection terms: view distance from the depth
    float focus;            // m
    float coc_k;            // circle of confusion radius in output pixels = coc_k * (z - focus) / z
    float max_px;           // its cap (output pixels)
    float jitter_u;         // experiment (dlss_dof_jitter): the depth lookup shifted by this frame's jitter
    float jitter_v;
    float blend;            // weight of this frame (1: no history)
    float near_scale;       // the game's blur scale in front of the focus (0: none)
    float far_scale;        // ...and behind it
};

float signed_coc(float2 duv) {
    const float d = depth_tex.SampleLevel(point_clamp, duv, 0);
    float z = abs(p32 / (d * p23 - p22));
    z = isfinite(z) ? z : 1.0e6;
    const float s = coc_k * (z - focus) / max(z, 1.0e-4);
    return clamp(s * (s < 0.0 ? near_scale : far_scale), -4.0 * max_px, 4.0 * max_px);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = (float2(id.xy) + 0.5) * inv_out;
    const float2 duv = uv * depth_uv_scale + float2(jitter_u, jitter_v);
    const float now = signed_coc(duv);

    if (blend >= 1.0) {
        coc_out[id.xy] = now;
        return;
    }

    // The range the true value can have here: the depth texels around this pixel (render resolution).
    float lo = now;
    float hi = now;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            if (x == 0 && y == 0) continue;
            const float s = signed_coc(duv + float2(x, y) * inv_depth);
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }

    const float2 previous = uv + motion.SampleLevel(point_clamp, uv * motion_uv_scale, 0).xy * float2(0.5, -0.5);

    if (any(previous < 0.0) || any(previous > 1.0)) {
        coc_out[id.xy] = now;
        return;
    }

    const float past = clamp(history.SampleLevel(linear_clamp, previous, 0), lo, hi);
    coc_out[id.xy] = lerp(past, now, blend);
}
)";

constexpr char GATHER_SHADER[] = R"(
Texture2D<float4> src : register(t0);       // DLSS's output (output size, linear HDR)
Texture2D<float> coc_tex : register(t1);    // the signed circle of confusion (output size, output pixels)
RWTexture2D<float4> dst : register(u0);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);

cbuffer Constants : register(b0) {
    float2 inv_out;
    float max_px;    // the blur's cap (output pixels)
    float rad_scale; // spiral step
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = (float2(id.xy) + 0.5) * inv_out;
    const float4 c = src.SampleLevel(point_clamp, uv, 0);
    const float sc_center = coc_tex[id.xy];
    const float cs = min(abs(sc_center), max_px);

    if (cs < 0.5) { // in focus
        dst[id.xy] = c;
        return;
    }

    // Golden-angle spiral gather (single-pass bokeh): a sample adds itself where its own blur reaches this pixel; a
    // farther sample can't spread more blur here than this pixel has (no background haze over a sharper foreground).
    float3 color = c.rgb;
    float total = 1.0;
    float radius = rad_scale;
    float angle = 0.0;

    [loop]
    for (; radius < cs + 0.5 && radius < max_px; angle += 2.39996323) {
        const float2 tuv = uv + float2(cos(angle), sin(angle)) * radius * inv_out;
        const float3 sc = src.SampleLevel(linear_clamp, tuv, 0).rgb;
        const float s = coc_tex.SampleLevel(point_clamp, tuv, 0);
        float ss = min(abs(s), max_px);

        if (s > sc_center) ss = clamp(ss, 0.0, cs * 2.0);

        const float m = smoothstep(radius - 0.5, radius + 0.5, ss);
        color += lerp(color / total, sc, m);
        total += 1.0;
        radius += rad_scale / radius;
    }

    dst[id.xy] = float4(color / total, c.a);
}
)";

constexpr UINT SETS = 64;
constexpr UINT PER_PASS = 4; // descriptor table per pass: 3 SRVs, 1 UAV
constexpr UINT PER_SET = 2 * PER_PASS;
constexpr UINT CONSTANTS = 20;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_coc_pso{};
ComPtr<ID3D12PipelineState> g_gather_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};

// The smoothed circle of confusion, ping-pong (output size, R16_FLOAT, kept in UNORDERED_ACCESS between frames).
ComPtr<ID3D12Resource> g_coc[2]{};
std::vector<ComPtr<ID3D12Resource>> g_retired{}; // replaced on a resize (the GPU may still use them; rare: kept)
uint32_t g_coc_frame{};
bool g_history_valid{};

bool ensure_coc(uint32_t width, uint32_t height) {
    if (g_coc[0] != nullptr) {
        const auto desc = g_coc[0]->GetDesc();

        if (desc.Width == width && desc.Height == height) {
            return true;
        }

        g_retired.push_back(g_coc[0]);
        g_retired.push_back(g_coc[1]);
        g_coc[0] = g_coc[1] = nullptr;
    }

    g_history_valid = false;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    for (auto& texture : g_coc) {
        if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&texture))))
        {
            log_error("Depth of field: circle of confusion texture ({}x{}) failed", width, height);
            g_coc[0] = g_coc[1] = nullptr;
            return false;
        }

        texture->SetName(L"refw_framegen depth of field circle of confusion");
    }

    return true;
}

void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

DXGI_FORMAT srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

void make_srv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = resource != nullptr ? srv_format(resource->GetDesc().Format) : DXGI_FORMAT_R32_FLOAT;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &srv, where);
}

void make_uav(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = resource->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(resource, nullptr, &uav, where);
}

bool compile_shader(const char* source, size_t size, const char* name, ComPtr<ID3DBlob>& code) {
    static const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    static const auto compile = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;
    ComPtr<ID3DBlob> errors{};

    if (compile == nullptr || FAILED(compile(source, size, name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors))) {
        log_error("Depth of field: shader {} compile failed: {}", name, errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
        return false;
    }

    return true;
}
} // namespace

namespace own_dof {
// The game's own depth of field is neutralised by an extreme aperture rather than switched off: with the component
// disabled RE7's image turns white (a later post step uses its output). Its passes still run, with no blur.
constexpr float NEUTRAL_F_NUMBER = 10000.0f;

void on_begin_rendering(bool take_over) {
    static const auto get_enabled = method("get_Enabled");
    static const auto get_f_number = method("get_FNumber");
    static const auto set_f_number = method("set_FNumber");
    static const auto get_focus = method("get_FocusDistance");
    static const auto get_sensor = method("get_SensorSize");
    static const auto get_max_coc = method("get_MaxCocSize");
    static const auto get_bokeh_control = method("get_BokehControl"); // DepthOfFieldBokehControl: Default 0, IgnoreNear 1, IgnoreFar 2
    static const auto get_near_scale = method("get_NearBokehScale");
    static const auto get_far_scale = method("get_FarBokehScale");
    static float game_f_number = 12.0f; // the game's own aperture (if it didn't write it again this frame, ours still reads)

    const auto dof = find_depth_of_field();

    if (dof == nullptr || get_enabled == nullptr || get_f_number == nullptr || set_f_number == nullptr) {
        g_taken_over = false;
        return;
    }

    const auto f_number = get_float(dof, get_f_number, 12.0f);

    if (f_number < NEUTRAL_F_NUMBER * 0.5f) {
        game_f_number = std::max(0.5f, f_number);
    }


    Lens lens{};
    lens.wanted = (refw::call(get_enabled, dof) & 0xFF) != 0;
    lens.f_number = game_f_number;
    lens.focus = std::max(0.01f, get_float(dof, get_focus, 1.0f));
    lens.sensor = std::max(0.001f, get_float(dof, get_sensor, 0.023f));
    lens.max_coc = std::clamp(get_float(dof, get_max_coc, 2.0f), 0.0f, 10.0f);
    const auto control = get_bokeh_control != nullptr ? (uint32_t)refw::call(get_bokeh_control, dof) : 0u;
    lens.near_scale = control == 1 ? 0.0f : std::clamp(get_float(dof, get_near_scale, 1.0f), 0.0f, 8.0f);
    lens.far_scale = control == 2 ? 0.0f : std::clamp(get_float(dof, get_far_scale, 1.0f), 0.0f, 8.0f);
    lens.bokeh_control = control;

    // Which lens mode the game uses where (RE3's item examine wasn't RE2's IgnoreNear): logged on a change, at most once a
    // second (RE8's cutscenes switch it every half second).
    static int last_mode = -1;
    static ULONGLONG last_mode_log = 0;

    if (const auto mode = (int)(lens.wanted ? control : 99u); mode != last_mode && GetTickCount64() - last_mode_log >= 1000) {
        last_mode = mode;
        last_mode_log = GetTickCount64();
        log_info("Depth of field lens: {} (BokehControl {}, aperture f/{:.1f}, focus {:.3f} m, near x{:.2f}, far x{:.2f})", lens.wanted ? "on" : "off",
            control, lens.f_number, lens.focus, lens.near_scale, lens.far_scale);
    }

    AcquireSRWLockExclusive(&g_lens_lock);
    g_lens = lens;
    ReleaseSRWLockExclusive(&g_lens_lock);

    // Taking over: the game's blur to (almost) nothing for this frame. Handing back: its own aperture again.
    if (take_over || f_number >= NEUTRAL_F_NUMBER * 0.5f) {
        uint64_t value = std::bit_cast<uint64_t>((double)(take_over ? NEUTRAL_F_NUMBER : game_f_number));
        refw::call_args(set_f_number, dof, &value, 1);
    }

    if (const auto was = g_taken_over.exchange(take_over); was != take_over) {
        log_info("Depth of field: {} (aperture f/{:.1f}, focus {:.3f} m, sensor {:.3f} m, max blur {:.1f}%)",
            take_over ? "the plugin's, after DLSS at the output resolution (the game's neutralised: aperture f/10000)" : "the game's own", lens.f_number,
            lens.focus, lens.sensor, lens.max_coc);
    }
}

void probe() {
    static const auto get_enabled = method("get_Enabled");
    static const auto get_f_number = method("get_FNumber");
    static const auto get_focus = method("get_FocusDistance");
    static const auto get_max_coc = method("get_MaxCocSize");
    static int last_enabled = -1;
    static float last_f = 0.0f;
    static float last_focus = 0.0f;
    static ULONGLONG last_log = 0;
    static bool logged_missing = false;

    const auto dof = find_depth_of_field();

    if (dof == nullptr || get_enabled == nullptr) {
        if (!logged_missing) {
            logged_missing = true;
            log_info("probe_dof: no via.render.DepthOfField component in the current scene (yet)");
        }

        return;
    }

    logged_missing = false;
    const auto enabled = (refw::call(get_enabled, dof) & 0xFF) != 0 ? 1 : 0;
    const auto f = get_float(dof, get_f_number, 0.0f);
    const auto focus = get_float(dof, get_focus, 0.0f);
    const auto now = GetTickCount64();
    const auto changed_lens = std::abs(f - last_f) > 0.05f * std::max(last_f, 0.01f) || std::abs(focus - last_focus) > 0.1f * std::max(last_focus, 0.01f);

    if (enabled != last_enabled || (enabled && changed_lens && now - last_log >= 500)) {
        log_info("probe_dof: depth of field {} - aperture f/{:.2f}, focus {:.3f} m, max blur {:.2f}%", enabled ? "ON" : "off", f, focus,
            get_float(dof, get_max_coc, 0.0f));
        last_enabled = enabled;
        last_f = f;
        last_focus = focus;
        last_log = now;
    }
}

bool examining() {
    AcquireSRWLockShared(&g_lens_lock);
    const auto lens = g_lens;
    ReleaseSRWLockShared(&g_lens_lock);
    if (g_examine_hint.load()) {
        return true;
    }

    if (!lens.wanted || !game_profile::current().examine_by_lens) {
        return false;
    }

    // RE2: the examine view's lens ignores the near blur.
    return lens.bokeh_control == 1;
}

void set_examine_hint(bool examining) {
    g_examine_hint = examining;
}

bool active() {
    AcquireSRWLockShared(&g_lens_lock);
    const auto wanted = g_lens.wanted;
    ReleaseSRWLockShared(&g_lens_lock);
    return g_taken_over.load() && wanted;
}

bool init(ID3D12Device* device) {
    if (g_ready) {
        return true;
    }

    g_device = device;
    ComPtr<ID3DBlob> coc_code{};
    ComPtr<ID3DBlob> gather_code{};

    if (!compile_shader(COC_SHADER, sizeof(COC_SHADER) - 1, "own_dof_coc", coc_code) ||
        !compile_shader(GATHER_SHADER, sizeof(GATHER_SHADER) - 1, "own_dof_gather", gather_code))
    {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3};

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, CONSTANTS};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    D3D12_ROOT_SIGNATURE_DESC root{2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};

    if (serialize == nullptr || FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Depth of field: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {coc_code->GetBufferPointer(), coc_code->GetBufferSize()};
    D3D12_COMPUTE_PIPELINE_STATE_DESC gather_pso = pso;
    gather_pso.CS = {gather_code->GetBufferPointer(), gather_code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_coc_pso))) ||
        FAILED(device->CreateComputePipelineState(&gather_pso, IID_PPV_ARGS(&g_gather_pso))) ||
        FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap))))
    {
        log_error("Depth of field: pipeline states or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Depth of field pass ready");
    return true;
}

bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* depth, ID3D12Resource* motion, ID3D12Resource* dst,
    uint32_t out_w, uint32_t out_h, uint32_t render_w, uint32_t render_h, const Mat4& projection, const float jitter_px[2], float strength, float max_scale,
    float temporal_blend, bool reset)
{
    if (!g_ready || src == nullptr || depth == nullptr || motion == nullptr || dst == nullptr) {
        return false;
    }

    AcquireSRWLockShared(&g_lens_lock);
    const auto lens = g_lens;
    ReleaseSRWLockShared(&g_lens_lock);

    // Thin lens: focal length from the vertical field of view and the sensor; the circle of confusion on the sensor
    // f^2 / (N (F - f)) * |z - F| / z, as a fraction of the sensor height, as a radius in output pixels.
    const auto f = 0.5f * lens.sensor * projection.m[1][1];

    if (!(f > 0.0f) || lens.focus <= f || !ensure_coc(out_w, out_h)) {
        g_history_valid = false;
        return false;
    }

    const auto coc_k = strength * f * f / (lens.f_number * (lens.focus - f)) / lens.sensor * (float)out_h * 0.5f;
    // max_scale: the cap against the game's (RE8's cutscene backgrounds reach it: blurrier in the game than 2% of the height).
    const auto max_px = std::max(1.0f, lens.max_coc * 0.01f * (float)out_h * std::max(max_scale, 0.1f));

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;
    const auto at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * g_increment}; };

    const auto current = g_coc_frame++ % 2;
    ID3D12Resource* coc_now = g_coc[current].Get();
    ID3D12Resource* coc_past = g_coc[1 - current].Get();

    // Pass 1's table: depth, motion, history; the current circle of confusion. Pass 2's: DLSS's image, the circle of
    // confusion, (unused); the output.
    make_srv(depth, at(0));
    make_srv(motion, at(1));
    make_srv(coc_past, at(2));
    make_uav(coc_now, at(3));
    make_srv(src, at(4));
    make_srv(coc_now, at(5));
    make_srv(nullptr, at(6));
    make_uav(dst, at(7));

    const auto depth_desc = depth->GetDesc();
    const auto motion_desc = motion->GetDesc();
    const auto blend = reset || !g_history_valid ? 1.0f : std::clamp(temporal_blend, 0.01f, 1.0f);
    const float coc_constants[CONSTANTS] = {1.0f / (float)out_w, 1.0f / (float)out_h, (float)render_w / (float)depth_desc.Width,
        (float)render_h / (float)depth_desc.Height, 1.0f / (float)depth_desc.Width, 1.0f / (float)depth_desc.Height,
        (float)render_w / (float)motion_desc.Width, (float)render_h / (float)motion_desc.Height, projection.m[2][2], projection.m[3][2],
        projection.m[2][3], lens.focus, coc_k, max_px, jitter_px[0] / (float)depth_desc.Width, jitter_px[1] / (float)depth_desc.Height, blend, lens.near_scale,
        lens.far_scale};
    const float gather_constants[CONSTANTS] = {1.0f / (float)out_w, 1.0f / (float)out_h, max_px, 1.5f};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());

    transition(list, coc_past, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(g_coc_pso.Get());
    list->SetComputeRoot32BitConstants(0, CONSTANTS, coc_constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    transition(list, coc_past, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    transition(list, coc_now, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(g_gather_pso.Get());
    list->SetComputeRoot32BitConstants(0, CONSTANTS, gather_constants, 0);
    list->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + (UINT64)PER_PASS * g_increment});
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    transition(list, coc_now, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    g_history_valid = true;
    return true;
}

void invalidate_history() {
    g_history_valid = false;
}
} // namespace own_dof
