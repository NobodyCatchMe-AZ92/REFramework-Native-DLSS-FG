#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "log.hpp"

#include "bloom_transfer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT BLOCK = 8; // output pixels per side of a residual texel

constexpr char SHADER[] = R"(
Texture2D<float4> tex0 : register(t0); // reduce: the engine's result; apply: DLSS's output
Texture2D<float4> tex1 : register(t1); // reduce: the pass's input (render size); apply: the residual (1/8)
RWTexture2D<float4> output : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 inv_out;
    float2 input_uv_scale; // render extent / input texture size
    float2 out_size;
    float2 low_size;
    float strength;
};

// One residual texel: the mean of the engine's result over its 8x8 output pixels minus the mean of a plain bilinear
// upscale of the input there (16 samples each, every other pixel).
[numthreads(8, 8, 1)]
void reduce(uint3 id : SV_DispatchThreadID) {
    if (any(float2(id.xy) >= low_size)) {
        return;
    }

    // The median of the 16 differences (by luminance), not their mean: a single very bright sparkle (gold, chrome at the
    // merchant) comes out differently in the engine's upscale and the plain one, and its difference pulled whole blocks
    // dark for a frame. Bloom only adds light: never below 0.
    float3 d[16];
    float l[16];

    [unroll]
    for (uint i = 0; i < 16; ++i) {
        const uint2 px = min(id.xy * 8 + uint2((i % 4) * 2, (i / 4) * 2) + 1, uint2(out_size) - 1);
        d[i] = tex0[px].rgb - tex1.SampleLevel(linear_clamp, (float2(px) + 0.5) * inv_out * input_uv_scale, 0).rgb;
        l[i] = dot(d[i], float3(0.2126, 0.7152, 0.0722));
    }

    // Insertion sort by luminance (16 entries).
    [unroll]
    for (uint a = 1; a < 16; ++a) {
        [unroll]
        for (uint b = a; b > 0; --b) {
            if (l[b] < l[b - 1]) {
                const float tl = l[b]; l[b] = l[b - 1]; l[b - 1] = tl;
                const float3 td = d[b]; d[b] = d[b - 1]; d[b - 1] = td;
            }
        }
    }

    output[id.xy] = float4(max((d[7] + d[8]) * 0.5, 0.0), 1.0);
}

// DLSS's output plus the residual, smoothly upscaled.
[numthreads(8, 8, 1)]
void apply(uint3 id : SV_DispatchThreadID) {
    if (any(float2(id.xy) >= out_size)) {
        return;
    }

    const float4 c = tex0[id.xy];
    const float3 bloom = tex1.SampleLevel(linear_clamp, (float2(id.xy) + 0.5) * inv_out, 0).rgb;
    output[id.xy] = float4(max(c.rgb + bloom * strength, 0.0), c.a);
}
)";

constexpr UINT SETS = 64;
constexpr UINT PER_PASS = 3; // 2 SRVs, 1 UAV
constexpr UINT PER_SET = 2 * PER_PASS;
constexpr UINT CONSTANTS = 9;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_reduce{};
ComPtr<ID3D12PipelineState> g_apply{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};

ComPtr<ID3D12Resource> g_residual{}; // 1/8 size, R16G16B16A16_FLOAT, UNORDERED_ACCESS
ComPtr<ID3D12Resource> g_output{};   // like DLSS's output, UNORDERED_ACCESS
std::vector<ComPtr<ID3D12Resource>> g_retired{};

bool ensure(ComPtr<ID3D12Resource>& texture, UINT64 width, UINT height, DXGI_FORMAT format, const wchar_t* name) {
    if (texture != nullptr) {
        const auto desc = texture->GetDesc();

        if (desc.Width == width && desc.Height == height && desc.Format == format) {
            return true;
        }

        g_retired.push_back(texture);
        texture = nullptr;
    }

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&texture))))
    {
        log_error("Bloom transfer: texture {}x{} format {} failed", width, height, (int)format);
        return false;
    }

    texture->SetName(name);
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

void uav_barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    b.UAV.pResource = resource;
    list->ResourceBarrier(1, &b);
}

void make_srv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = resource->GetDesc().Format;
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

bool compile(const char* entry, ComPtr<ID3DBlob>& code) {
    static const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    static const auto fn = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;
    ComPtr<ID3DBlob> errors{};

    if (fn == nullptr || FAILED(fn(SHADER, sizeof(SHADER) - 1, "bloom_transfer", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                             &code, &errors)))
    {
        log_error("Bloom transfer: shader {} failed: {}", entry, errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
        return false;
    }

    return true;
}
} // namespace

namespace bloom_transfer {
bool init(ID3D12Device* device) {
    if (g_ready) {
        return true;
    }

    g_device = device;
    ComPtr<ID3DBlob> reduce{};
    ComPtr<ID3DBlob> apply{};

    if (!compile("reduce", reduce) || !compile("apply", apply)) {
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 2};

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, CONSTANTS};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // From the d3d12.dll the game loaded (importing it breaks other mods' d3d12.dll proxies).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    D3D12_ROOT_SIGNATURE_DESC root{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};

    if (serialize == nullptr || FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Bloom transfer: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {reduce->GetBufferPointer(), reduce->GetBufferSize()};
    D3D12_COMPUTE_PIPELINE_STATE_DESC apply_pso = pso;
    apply_pso.CS = {apply->GetBufferPointer(), apply->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_reduce))) ||
        FAILED(device->CreateComputePipelineState(&apply_pso, IID_PPV_ARGS(&g_apply))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap))))
    {
        log_error("Bloom transfer: pipeline states or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Bloom transfer ready");
    return true;
}

ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* engine, ID3D12Resource* input, ID3D12Resource* dlss, uint32_t out_w,
    uint32_t out_h, uint32_t render_w, uint32_t render_h, float strength)
{
    const auto low_w = (out_w + BLOCK - 1) / BLOCK;
    const auto low_h = (out_h + BLOCK - 1) / BLOCK;

    if (!g_ready || engine == nullptr || input == nullptr || dlss == nullptr ||
        !ensure(g_residual, low_w, low_h, DXGI_FORMAT_R16G16B16A16_FLOAT, L"refw_framegen bloom transfer residual") ||
        !ensure(g_output, out_w, out_h, dlss->GetDesc().Format, L"refw_framegen DLSS output with the bloom"))
    {
        return nullptr;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;
    const auto at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * g_increment}; };

    make_srv(engine, at(0));
    make_srv(input, at(1));
    make_uav(g_residual.Get(), at(2));
    make_srv(dlss, at(3));
    make_srv(g_residual.Get(), at(4));
    make_uav(g_output.Get(), at(5));

    const auto input_desc = input->GetDesc();
    const float constants[CONSTANTS] = {1.0f / (float)out_w, 1.0f / (float)out_h, (float)render_w / (float)input_desc.Width,
        (float)render_h / (float)input_desc.Height, (float)out_w, (float)out_h, (float)low_w, (float)low_h, strength};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetComputeRoot32BitConstants(0, CONSTANTS, constants, 0);

    list->SetPipelineState(g_reduce.Get());
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((low_w + 7) / 8, (low_h + 7) / 8, 1);

    transition(list, g_residual.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->SetPipelineState(g_apply.Get());
    list->SetComputeRootDescriptorTable(1, D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + (UINT64)PER_PASS * g_increment});
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    transition(list, g_residual.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    uav_barrier(list, g_output.Get());
    return g_output.Get();
}
} // namespace bloom_transfer
