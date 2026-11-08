#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "log.hpp"

#include "examine_stabilizer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char SHADER[] = R"(
Texture2D<float4> current : register(t0); // DLSS's output (output size)
Texture2D<float4> history : register(t1); // the previous frame's stabilized output
Texture2D<float4> motion : register(t2);  // the engine's velocity (render resolution, xy in clip space units)
RWTexture2D<float4> output : register(u0);
SamplerState point_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 inv_out;
    float2 motion_uv_scale; // render extent / velocity texture size
    float2 inv_motion;      // 1 / velocity texture size
    float2 out_size;
    float blend;            // this frame's weight where nothing moves (1: no history)
    float threshold;        // motion (output pixels) above which a pixel passes through
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    const uint2 size = uint2(out_size);

    if (any(id.xy >= size)) {
        return;
    }

    const float4 c = current[id.xy];

    if (blend >= 1.0) {
        output[id.xy] = c;
        return;
    }

    // Motion around this pixel (3x3 velocity texels): a rotating item's pixels and their edges pass through.
    const float2 uv = (float2(id.xy) + 0.5) * inv_out;
    float moving = 0.0;

    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            const float2 v = motion.SampleLevel(point_clamp, uv * motion_uv_scale + float2(x, y) * inv_motion, 0).xy;
            moving = max(moving, length(v * 0.5 * out_size));
        }
    }

    if (moving > threshold) {
        output[id.xy] = c;
        return;
    }

    // The range the stable value can have here: the current 3x3 neighbourhood.
    float3 lo = c.rgb;
    float3 hi = c.rgb;

    [unroll]
    for (int j = -1; j <= 1; ++j) {
        [unroll]
        for (int i = -1; i <= 1; ++i) {
            const float3 s = current[clamp(int2(id.xy) + int2(i, j), int2(0, 0), int2(size) - 1)].rgb;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }

    const float3 past = clamp(history[id.xy].rgb, lo, hi);
    output[id.xy] = float4(lerp(past, c.rgb, blend), c.a);
}
)";

constexpr UINT SETS = 64;
constexpr UINT PER_SET = 4;
constexpr UINT CONSTANTS = 10;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};

ComPtr<ID3D12Resource> g_history[2]{}; // UNORDERED_ACCESS between frames
std::vector<ComPtr<ID3D12Resource>> g_retired{};
uint32_t g_frame{};
bool g_valid{};

bool ensure_history(const D3D12_RESOURCE_DESC& like) {
    if (g_history[0] != nullptr) {
        const auto desc = g_history[0]->GetDesc();

        if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
            return true;
        }

        g_retired.push_back(g_history[0]);
        g_retired.push_back(g_history[1]);
        g_history[0] = g_history[1] = nullptr;
    }

    g_valid = false;
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

    for (auto& texture : g_history) {
        if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&texture))))
        {
            log_error("Examine stabilizer: history texture ({}x{} format {}) failed", desc.Width, desc.Height, (int)desc.Format);
            g_history[0] = g_history[1] = nullptr;
            return false;
        }

        texture->SetName(L"refw_framegen examine stabilizer history");
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
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return DXGI_FORMAT_R16G16B16A16_UNORM;
    default: return format;
    }
}

void make_srv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = srv_format(resource->GetDesc().Format);
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &srv, where);
}
} // namespace

namespace examine_stabilizer {
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
        FAILED(compile(SHADER, sizeof(SHADER) - 1, "examine_stabilizer", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code,
            &errors)))
    {
        log_error("Examine stabilizer: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
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

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
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
        log_error("Examine stabilizer: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("Examine stabilizer: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Examine stabilizer ready");
    return true;
}

ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* motion, uint32_t out_w, uint32_t out_h,
    uint32_t render_w, uint32_t render_h, float blend, bool reset)
{
    if (!g_ready || src == nullptr || motion == nullptr || !ensure_history(src->GetDesc())) {
        g_valid = false;
        return nullptr;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;
    const auto at = [&](UINT i) { return D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)i * g_increment}; };

    const auto current = g_frame++ % 2;
    ID3D12Resource* out = g_history[current].Get();
    ID3D12Resource* past = g_history[1 - current].Get();

    make_srv(src, at(0));
    make_srv(past, at(1));
    make_srv(motion, at(2));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = srv_format(out->GetDesc().Format);
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(out, nullptr, &uav, at(3));

    const auto motion_desc = motion->GetDesc();
    const float constants[CONSTANTS] = {1.0f / (float)out_w, 1.0f / (float)out_h, (float)render_w / (float)motion_desc.Width,
        (float)render_h / (float)motion_desc.Height, 1.0f / (float)motion_desc.Width, 1.0f / (float)motion_desc.Height, (float)out_w, (float)out_h,
        reset || !g_valid ? 1.0f : std::clamp(blend, 0.02f, 1.0f), 0.05f};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());
    list->SetComputeRoot32BitConstants(0, CONSTANTS, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);

    transition(list, past, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);
    transition(list, past, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    D3D12_RESOURCE_BARRIER written{};
    written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written.UAV.pResource = out;
    list->ResourceBarrier(1, &written);

    g_valid = true;
    return out;
}

void invalidate() {
    g_valid = false;
}
} // namespace examine_stabilizer
