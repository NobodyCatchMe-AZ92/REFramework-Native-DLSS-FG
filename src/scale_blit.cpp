#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>

#include "log.hpp"

#include "scale_blit.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char SHADER[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 inv_dst_size;
    float2 dst_size;
    float2 src_size;
    float bicubic; // 1 = Catmull-Rom, 0 = bilinear
    float unused;
};

// Catmull-Rom from 9 bilinear taps (the weights of each pair of texels folded into one tap). Its negative lobes keep edges
// crisp; clamped at 0 (the HDR targets can't hold negative values).
float4 catmull_rom(float2 uv) {
    const float2 pos = uv * src_size;
    const float2 p1 = floor(pos - 0.5) + 0.5;
    const float2 f = pos - p1;

    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;

    const float2 t0 = (p1 - 1.0) / src_size;
    const float2 t12 = (p1 + w2 / w12) / src_size;
    const float2 t3 = (p1 + 2.0) / src_size;

    float4 c = 0;
    c += src.SampleLevel(linear_clamp, float2(t0.x, t0.y), 0) * w0.x * w0.y;
    c += src.SampleLevel(linear_clamp, float2(t12.x, t0.y), 0) * w12.x * w0.y;
    c += src.SampleLevel(linear_clamp, float2(t3.x, t0.y), 0) * w3.x * w0.y;
    c += src.SampleLevel(linear_clamp, float2(t0.x, t12.y), 0) * w0.x * w12.y;
    c += src.SampleLevel(linear_clamp, float2(t12.x, t12.y), 0) * w12.x * w12.y;
    c += src.SampleLevel(linear_clamp, float2(t3.x, t12.y), 0) * w3.x * w12.y;
    c += src.SampleLevel(linear_clamp, float2(t0.x, t3.y), 0) * w0.x * w3.y;
    c += src.SampleLevel(linear_clamp, float2(t12.x, t3.y), 0) * w12.x * w3.y;
    c += src.SampleLevel(linear_clamp, float2(t3.x, t3.y), 0) * w3.x * w3.y;
    return max(c, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= (uint)dst_size.x || id.y >= (uint)dst_size.y) return;
    const float2 uv = (float2(id.xy) + 0.5) * inv_dst_size;
    dst[id.xy] = bicubic > 0.5 ? catmull_rom(uv) : src.SampleLevel(linear_clamp, uv, 0);
}
)";

constexpr UINT SETS = 64; // descriptor sets in the ring (frames the GPU may still be reading)
constexpr UINT PER_SET = 2;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};
} // namespace

namespace scale_blit {
bool init(ID3D12Device* device) {
    if (g_ready) {
        return true;
    }

    g_device = device;
    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;
    ComPtr<ID3DBlob> code{};
    ComPtr<ID3DBlob> errors{};

    if (compile == nullptr ||
        FAILED(compile(SHADER, sizeof(SHADER) - 1, "scale_blit", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        log_error("Scale blit: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1};

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 8};
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

    if (serialize == nullptr || FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Scale blit: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("Scale blit: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Scale blit ready");
    return true;
}

bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* dst, uint32_t dst_w, uint32_t dst_h, bool bicubic) {
    if (!g_ready || src == nullptr || dst == nullptr) {
        return false;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = src->GetDesc().Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = dst->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(dst, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + g_increment});

    const auto src_desc = src->GetDesc();
    const float constants[8] = {1.0f / (float)dst_w, 1.0f / (float)dst_h, (float)dst_w, (float)dst_h, (float)src_desc.Width, (float)src_desc.Height,
        bicubic ? 1.0f : 0.0f, 0.0f};
    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());
    list->SetComputeRoot32BitConstants(0, 8, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
    return true;
}
} // namespace scale_blit
