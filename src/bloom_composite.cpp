#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

#include "log.hpp"

#include "bloom_composite.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// The post and pre textures are exactly the render size; render_size / their size scales the UV for the part in use.
constexpr char SHADER[] = R"(
Texture2D<float4> upscaled : register(t0);
Texture2D<float4> post : register(t1);
Texture2D<float4> pre : register(t2);
Texture2D<float4> lit : register(t3); // the scene with the engine's bloom, before its post pass
RWTexture2D<float4> output : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 inv_out_size;
    float2 uv_scale; // render size / texture size of post and pre
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const float2 uv = (float2(id.xy) + 0.5) * inv_out_size * uv_scale;
    // The engine's own result (exposure, bloom, extras: all smooth) plus DLSS's detail over a plain upscale of the same
    // input, scaled by what the engine's post pass does to the scene there (its exposure).
    const float3 base = post.SampleLevel(linear_clamp, uv, 0).rgb;
    const float3 scene = lit.SampleLevel(linear_clamp, uv, 0).rgb;
    const float3 exposure = clamp((base + 1e-4) / (scene + 1e-4), 0.0, 16.0);
    const float3 detail = upscaled[id.xy].rgb - pre.SampleLevel(linear_clamp, uv, 0).rgb;
    output[id.xy] = float4(max(base + exposure * detail, 0.0), 1.0);
}
)";

constexpr UINT SETS = 64; // descriptor sets in the ring (frames the GPU may still be reading)
constexpr UINT PER_SET = 5; // 4 SRVs + 1 UAV

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};

bool compile(ComPtr<ID3DBlob>& bytecode) {
    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile_fn = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;

    if (compile_fn == nullptr) {
        log_error("Bloom composite: d3dcompiler_47.dll not available");
        return false;
    }

    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_fn(SHADER, sizeof(SHADER) - 1, "bloom_composite", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors)))
    {
        log_error("Bloom composite: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    return true;
}
} // namespace

namespace bloom_composite {
bool init(ID3D12Device* device) {
    g_device = device;
    log_info("Bloom composite: compiling its shader");
    ComPtr<ID3DBlob> bytecode{};

    if (!compile(bytecode)) {
        return false;
    }

    log_info("Bloom composite: shader compiled ({} bytes)", bytecode->GetBufferSize());

    // Root: 4 constants (b0) and a table of 4 SRVs (t0-t3) + 1 UAV (u0); a static linear clamp sampler (s0).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 4};

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 4};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // From the d3d12.dll the game loaded: importing it would load it with the plugin, before the game, which breaks
    // d3d12.dll proxies of other mods (the game then falls back to DirectX 11).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");

    if (serialize == nullptr) {
        log_error("Bloom composite: D3D12SerializeRootSignature not found");
        return false;
    }

    D3D12_ROOT_SIGNATURE_DESC root{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Bloom composite: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso)))) {
        log_error("Bloom composite: pipeline state failed");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("Bloom composite: descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Bloom composite ready");
    return true;
}

bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* upscaled, ID3D12Resource* post, ID3D12Resource* pre, ID3D12Resource* lit, ID3D12Resource* out,
    uint32_t out_w, uint32_t out_h, uint32_t render_w, uint32_t render_h)
{
    if (!g_ready) {
        return false;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;

    ID3D12Resource* inputs[] = {upscaled, post, pre, lit};

    for (UINT i = 0; i < 4; ++i) {
        const auto desc = inputs[i]->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(inputs[i], &srv, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * g_increment});
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = out->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(out, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 4 * (SIZE_T)g_increment});

    const auto post_desc = post->GetDesc();
    const float constants[4] = {1.0f / (float)out_w, 1.0f / (float)out_h, (float)render_w / (float)post_desc.Width,
        (float)render_h / (float)post_desc.Height};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());
    list->SetComputeRoot32BitConstants(0, 4, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    return true;
}
} // namespace bloom_composite
