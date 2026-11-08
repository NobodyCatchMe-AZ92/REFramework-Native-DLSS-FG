#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "log.hpp"

#include "cas_sharpen.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// CAS's no-scaling path (ffx_cas.h, CasFilter with sharpness 0: peak -1/8), per channel, on the neighbourhood scaled and
// gamma-adjusted as described in the header.
constexpr char SHADER[] = R"(
Texture2D<float4> input : register(t0);
RWTexture2D<float4> output : register(u0);

cbuffer Constants : register(b0) {
    uint2 size;
    float strength; // 0..1: the sharpened result's share
};

float3 load(int2 p) {
    return max(input[clamp(p, int2(0, 0), int2(size) - 1)].rgb, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= size)) {
        return;
    }

    const int2 p = int2(id.xy);
    const float4 center = input[id.xy];

    //  a b c
    //  d e f
    //  g h i
    float3 a = load(p + int2(-1, -1)), b = load(p + int2(0, -1)), c = load(p + int2(1, -1));
    float3 d = load(p + int2(-1, 0)), e = max(center.rgb, 0.0), f = load(p + int2(1, 0));
    float3 g = load(p + int2(-1, 1)), h = load(p + int2(0, 1)), i = load(p + int2(1, 1));

    // The neighbourhood's brightest channel -> 0.5 (exposure-independent, headroom above), then ~gamma 2.
    float3 top = max(max(max(a, b), max(c, d)), max(max(e, f), max(max(g, h), i)));
    const float peak_value = max(top.r, max(top.g, top.b));

    if (peak_value <= 1e-8 || strength <= 0.0) {
        output[id.xy] = center;
        return;
    }

    const float scale = 0.25 / peak_value; // sqrt(0.25) = 0.5
    a = sqrt(a * scale); b = sqrt(b * scale); c = sqrt(c * scale);
    d = sqrt(d * scale); e = sqrt(e * scale); f = sqrt(f * scale);
    g = sqrt(g * scale); h = sqrt(h * scale); i = sqrt(i * scale);

    // CAS: soft min / max over the cross and the whole 3x3 (added: a range of 0..2).
    float3 mn = min(min(min(d, e), min(f, b)), h);
    const float3 mn2 = min(mn, min(min(a, c), min(g, i)));
    mn += mn2;
    float3 mx = max(max(max(d, e), max(f, b)), h);
    const float3 mx2 = max(mx, max(max(a, c), max(g, i)));
    mx += mx2;

    // Smooth minimum distance to the signal limit divided by the smooth max: the local contrast's headroom.
    const float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-8)));
    const float peak = -1.0 / 8.0; // sharpness 0 (CAS's default; lerp(8, 5, sharpness))
    const float3 w = amp * peak;
    const float3 sharpened = saturate((b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w));

    // The blend (ReShade's "Sharpening"), back to linear and the scene's brightness.
    const float3 result = lerp(e, sharpened, strength);
    output[id.xy] = float4(result * result / scale, center.a);
}
)";

constexpr UINT SETS = 64;
constexpr UINT PER_SET = 2;
constexpr UINT CONSTANTS = 3;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
int g_ready{}; // 0 not tried, 1 ready, -1 failed

ComPtr<ID3D12Resource> g_out{}; // UNORDERED_ACCESS between frames
std::vector<ComPtr<ID3D12Resource>> g_retired{};

DXGI_FORMAT view_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    default: return format;
    }
}

bool ensure_output(const D3D12_RESOURCE_DESC& like) {
    if (g_out != nullptr) {
        const auto desc = g_out->GetDesc();

        if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
            return true;
        }

        g_retired.push_back(g_out); // frames in flight may still read it
        g_out = nullptr;
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

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_out)))) {
        log_error("CAS sharpening: output texture ({}x{} format {}) failed", desc.Width, desc.Height, (int)desc.Format);
        return false;
    }

    g_out->SetName(L"refw_framegen CAS sharpening");
    return true;
}
} // namespace

namespace cas_sharpen {
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
        FAILED(compile(SHADER, sizeof(SHADER) - 1, "cas_sharpen", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        log_error("CAS sharpening: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
        return false;
    }

    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, CONSTANTS};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // From the d3d12.dll the game loaded (importing it breaks other mods' d3d12.dll proxies).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    D3D12_ROOT_SIGNATURE_DESC root{2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};

    if (serialize == nullptr || FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("CAS sharpening: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("CAS sharpening: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = 1;
    log_info("CAS sharpening ready");
    return true;
}

ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, uint32_t width, uint32_t height, float strength) {
    if (g_ready <= 0 || src == nullptr || !ensure_output(src->GetDesc())) {
        return nullptr;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;

    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = view_format(src->GetDesc().Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(src, &srv, cpu);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = view_format(g_out->GetDesc().Format);
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_out.Get(), nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + g_increment});

    const auto w = std::min<uint32_t>(width, (uint32_t)src->GetDesc().Width);
    const auto h = std::min<uint32_t>(height, src->GetDesc().Height);
    const float s = std::clamp(strength, 0.0f, 1.0f);
    uint32_t constants[CONSTANTS] = {w, h, 0};
    std::memcpy(&constants[2], &s, sizeof(s));

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());
    list->SetComputeRoot32BitConstants(0, CONSTANTS, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((w + 7) / 8, (h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER written{};
    written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written.UAV.pResource = g_out.Get();
    list->ResourceBarrier(1, &written);
    return g_out.Get();
}
} // namespace cas_sharpen
