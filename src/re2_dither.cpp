#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <iterator>

#include "framegen.hpp"
#include "game_profile.hpp"
#include "log.hpp"

#include "re2_dither.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// One pass per input: out[p] = in[p + moved] (clamped to the image): the picture moved by `moved`, moved back. The motion
// vectors: the shift's change (in them via the previous matrices) taken out, the storage rounding left of a still pixel
// snapped to exactly 0 (as without the shift). The color: the strip along the edges the picture moved away from (nothing
// rendered there) from DLSS's previous output instead of the repeated last row / column (which changes every frame: the
// edges flickered).
constexpr char SHADER[] = R"(
Texture2D<float4> input : register(t0);
Texture2D<float4> previous : register(t1); // the color: DLSS's previous output (fill != 0)
RWTexture2D<float4> output : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    int2 moved;
    uint2 size;
    float2 subtract; // the motion vectors only (else 0)
    uint fill;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= size.x || id.y >= size.y) {
        return;
    }

    const int2 wanted = int2(id.xy) + moved;
    const int2 from = clamp(wanted, int2(0, 0), int2(size) - 1);
    float4 value = input.Load(int3(from, 0));

    if (fill != 0 && any(wanted != from)) {
        value = float4(previous.SampleLevel(linear_clamp, (float2(id.xy) + 0.5) / float2(size), 0).rgb, 1.0);
    }

    if (any(subtract != 0)) {
        value.xy -= subtract;
        value.xy = abs(value.xy) < 1.5 / 32767.0 ? 0.0 : value.xy;
    }

    output[id.xy] = value;
}
)";

constexpr UINT FRAMES = 16;
constexpr UINT PASSES = 3; // color, depth, motion
constexpr UINT PER_PASS = 3; // 2 SRVs + UAV
constexpr auto READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next{};
int g_ready{}; // 0 not tried, 1 ready, -1 failed

struct Copy {
    ComPtr<ID3D12Resource> texture{}; // kept in NON_PIXEL_SHADER_RESOURCE between frames
} g_copies[PASSES]{};

DXGI_FORMAT srv_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
    }
}

// The copy's format: one a UAV can write. The color and the motion vectors keep their own (RE2's motion: 16-bit SNORM, so
// it can be copied back into the engine's); depth as R32_FLOAT.
DXGI_FORMAT copy_format(UINT pass, DXGI_FORMAT source) {
    if (pass == 1) return DXGI_FORMAT_R32_FLOAT;
    return srv_format(source);
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = resource;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    list->ResourceBarrier(1, &b);
}

bool readable(D3D12_RESOURCE_STATES state) {
    return (state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) != 0;
}

bool init(ID3D12Device* device) {
    if (g_ready != 0) {
        return g_ready > 0;
    }

    g_ready = -1;
    g_device = device;
    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;
    ComPtr<ID3DBlob> code{};
    ComPtr<ID3DBlob> errors{};

    if (compile == nullptr ||
        FAILED(compile(SHADER, sizeof(SHADER) - 1, "re2_dither_unshift", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        log_error("[EXPERIMENTAL] dither: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 7};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // From the d3d12.dll the game loaded (importing it breaks other mods' d3d12.dll proxies).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};

    if (serialize == nullptr || FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("[EXPERIMENTAL] dither: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, FRAMES * PASSES * PER_PASS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("[EXPERIMENTAL] dither: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = 1;
    return true;
}

bool ensure_copy(UINT pass, const D3D12_RESOURCE_DESC& source) {
    const auto format = copy_format(pass, source.Format);
    auto& copy = g_copies[pass];

    if (copy.texture != nullptr) {
        const auto d = copy.texture->GetDesc();

        if (d.Width == source.Width && d.Height == source.Height && d.Format == format) {
            return true;
        }

        // The old one may still be in use by frames in flight: kept for a while (sizes change rarely).
        static ComPtr<ID3D12Resource> retired[8]{};
        static UINT next_retired{};
        retired[next_retired++ % std::size(retired)] = std::move(copy.texture);
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, READ, nullptr, IID_PPV_ARGS(&copy.texture)))) {
        log_error("[EXPERIMENTAL] dither: copy texture failed");
        return false;
    }

    copy.texture->SetName(L"refw_framegen RE2 dither: DLSS input moved back");
    return true;
}

// One pass (pipeline, root signature and heap already set on the list): `source` (in `state`, readable by compute or made
// so for the pass) into copy `pass`, left in READ.
void run_pass(ID3D12GraphicsCommandList* list, UINT pass, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE gpu, ID3D12Resource* source,
    D3D12_RESOURCE_STATES state, const int moved[2], const float subtract[2], ID3D12Resource* previous, uint32_t render_w, uint32_t render_h)
{
    const auto target = g_copies[pass].texture.Get();
    const auto sd = source->GetDesc();

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = srv_format(sd.Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(source, &srv, cpu);

    // t1: DLSS's previous output for the color's edge strip (else the source again: unused).
    D3D12_SHADER_RESOURCE_VIEW_DESC previous_srv = srv;

    if (previous != nullptr) {
        previous_srv.Format = srv_format(previous->GetDesc().Format);
    }

    g_device->CreateShaderResourceView(previous != nullptr ? previous : source, &previous_srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + g_increment});

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = target->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(target, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 2 * g_increment});

    const auto source_read = readable(state) ? state : READ;
    barrier(list, source, state, source_read);
    barrier(list, target, READ, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // Only the render extent (the textures can be larger than what was rendered).
    const uint32_t w = std::min<uint32_t>(render_w, (uint32_t)sd.Width);
    const uint32_t h = std::min<uint32_t>(render_h, sd.Height);
    const uint32_t constants[7] = {(uint32_t)moved[0], (uint32_t)moved[1], w, h, std::bit_cast<uint32_t>(subtract[0]), std::bit_cast<uint32_t>(subtract[1]),
        previous != nullptr ? 1u : 0u};
    list->SetComputeRoot32BitConstants(0, 7, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    barrier(list, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, READ);
    barrier(list, source, source_read, state);
}

} // namespace

namespace re2_dither {
int mode() {
    if (!game_profile::current().dither_experiments) {
        return 0;
    }

    const auto d = framegen::dlss();
    return d != nullptr && d->dither_test != 0 ? 1 : 0;
}

// Position `k` of a closed loop through all n x n positions (n even) where every step moves exactly one pixel: along the top
// row, back and forth through the other rows (columns 1..n-1), up column 0 to the start. (With jumps of 3, edges in the
// game's own image jumped now and then.) n = 4: 00 10 20 30 31 21 11 12 22 32 33 23 13 03 02 01. (2x2, 6x6 and 8x8 were
// tried too: 2x2 barely helped, 6x6 / 8x8 hardly more than 4x4 with wider edge strips; development notes.)
void loop_position(uint32_t n, uint32_t k, int out[2]) {
    k %= n * n;

    if (k < n) { // the top row
        out[0] = (int)k;
        out[1] = 0;
        return;
    }

    k -= n;

    if (k < (n - 1) * (n - 1)) { // rows 1..n-1 over columns 1..n-1, odd rows right to left
        const auto row = 1 + k / (n - 1);
        const auto i = k % (n - 1);
        out[0] = (int)(row % 2 == 1 ? n - 1 - i : 1 + i);
        out[1] = (int)row;
        return;
    }

    k -= (n - 1) * (n - 1); // column 0, from the bottom up
    out[0] = 0;
    out[1] = (int)(n - 1 - k);
}

void pixel_shift(uint32_t frame, int shift[2]) {
    shift[0] = 0;
    shift[1] = 0;
    if (mode() != 0) {
        loop_position(4, frame, shift); // the 4x4 loop (16 frames)
    }
}

void picture_moved(const int shift[2], float projection_m23, int moved[2]) {
    const auto sign = projection_m23 < 0.0f ? -1 : 1;
    moved[0] = sign * shift[0];
    moved[1] = sign * shift[1];

    if (static bool logged = false; !logged && (shift[0] != 0 || shift[1] != 0)) {
        logged = true;
        log_info("[EXPERIMENTAL] dither: projection m[2][3] {:.3f}: the picture moves {} the shift", projection_m23, sign < 0 ? "against" : "with");
    }
}

bool unshift(ID3D12GraphicsCommandList* list, ID3D12Device* device, Inputs& inputs, const int moved[2], const float motion_change[2],
    ID3D12Resource* previous_output, D3D12_RESOURCE_STATES previous_state, uint32_t render_w, uint32_t render_h)
{
    const float none[2] = {0.0f, 0.0f};

    if (inputs.color == nullptr || inputs.depth == nullptr || inputs.motion == nullptr || !init(device)) {
        return false;
    }

    ID3D12Resource* sources[3] = {inputs.color, inputs.depth, inputs.motion};
    D3D12_RESOURCE_STATES states[3] = {inputs.color_state, inputs.depth_state, inputs.motion_state};

    for (UINT p = 0; p < 3; ++p) {
        if (!ensure_copy(p, sources[p]->GetDesc())) {
            return false;
        }
    }

    const auto set = g_next++ % FRAMES;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PASSES * PER_PASS * g_increment;
    gpu.ptr += (UINT64)set * PASSES * PER_PASS * g_increment;

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());

    if (previous_output != nullptr) {
        barrier(list, previous_output, previous_state, READ);
    }

    for (UINT p = 0; p < 3; ++p) {
        run_pass(list, p, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)p * PER_PASS * g_increment},
            D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + (UINT64)p * PER_PASS * g_increment}, sources[p], states[p], moved, p == 2 ? motion_change : none,
            p == 0 ? previous_output : nullptr, render_w, render_h);
    }

    if (previous_output != nullptr) {
        barrier(list, previous_output, READ, previous_state);
    }

    inputs.color = g_copies[0].texture.Get();
    inputs.color_state = READ;
    inputs.depth = g_copies[1].texture.Get();
    inputs.depth_state = READ;
    inputs.motion = g_copies[2].texture.Get();
    inputs.motion_state = READ;

    static int logged = -1;

    if (const auto m = mode(); logged != m) {
        logged = m;
        log_info("[EXPERIMENTAL] dither fix on: the scene rendered moved by whole pixels (4x4 loop), DLSS's inputs moved back (only the sub-pixel "
                 "jitter told)");
    }

    return true;
}

void restore_motion(ID3D12GraphicsCommandList* list, ID3D12Resource* engine_motion, D3D12_RESOURCE_STATES state) {
    const auto copy = g_copies[2].texture.Get();

    if (copy == nullptr || engine_motion == nullptr) {
        return;
    }

    const auto cd = copy->GetDesc();
    const auto ed = engine_motion->GetDesc();

    if ((cd.Format != srv_format(ed.Format) && cd.Format != ed.Format) || cd.Width != ed.Width || cd.Height != ed.Height) {
        static bool logged = false;

        if (!logged) {
            logged = true;
            log_error("[EXPERIMENTAL] dither: the engine's motion vectors not restored (format {} vs {})", (int)ed.Format, (int)cd.Format);
        }

        return;
    }

    barrier(list, copy, READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    barrier(list, engine_motion, state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{engine_motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION src{copy, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    barrier(list, engine_motion, D3D12_RESOURCE_STATE_COPY_DEST, state);
    barrier(list, copy, D3D12_RESOURCE_STATE_COPY_SOURCE, READ);
}

} // namespace re2_dither
