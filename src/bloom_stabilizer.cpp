#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>

#include "log.hpp"

#include "bloom_stabilizer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char SHADER[] = R"(
Texture2D<float4> current : register(t0); // the scene now (jittered, render resolution)
Texture2D<float4> history : register(t1); // the stabilized scene of the previous frame
Texture2D<float4> motion : register(t2);  // the engine's velocity (xy, clip space units)
RWTexture2D<float4> output : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 inv_size;
    float2 mvec_scale; // velocity -> UV offset to the previous frame
    float2 size;
    float reset;
    float moving_px; // pixels moving more than this are passed through
    float alpha;     // weight of the current frame
    float3 pad;
};

static const float WIDEN = 0.5; // the neighborhood range is widened by this much each way (thin lights blink in and out of it)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(float2(id.xy) >= size)) return;

    const float3 now = current[id.xy].rgb;

    if (reset > 0.5) {
        output[id.xy] = float4(now, 1.0);
        return;
    }

    float3 lo = now;
    float3 hi = now;

    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            const int2 p = clamp(int2(id.xy) + int2(x, y), int2(0, 0), int2(size) - 1);
            const float3 c = current[p].rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    }

    const float3 range = hi - lo;
    lo = max(lo - range * WIDEN, 0.0);
    hi = hi + range * WIDEN;

    // Moving pixels (3x3, the engine's velocity in render pixels): the current scene as it is. Blending there left trails in
    // the glow behind moving limbs (RE2's Model Viewer: strong glow over the character, soft and smeared); the flicker this
    // is for (small lights blinking with the jitter) is on still pixels.
    float moving = 0.0;

    for (int my = -1; my <= 1; ++my) {
        for (int mx = -1; mx <= 1; ++mx) {
            const int2 p = clamp(int2(id.xy) + int2(mx, my), int2(0, 0), int2(size) - 1);
            moving = max(moving, length(motion[p].xy * mvec_scale * size));
        }
    }

    if (moving > moving_px) {
        output[id.xy] = float4(now, 1.0);
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) * inv_size;
    const float2 previous = uv + motion[id.xy].xy * mvec_scale;

    if (any(previous < 0.0) || any(previous > 1.0)) {
        output[id.xy] = float4(now, 1.0);
        return;
    }

    const float3 past = clamp(history.SampleLevel(linear_clamp, previous, 0).rgb, lo, hi);
    output[id.xy] = float4(lerp(past, now, alpha), 1.0);
}
)";

constexpr UINT SETS = 64; // descriptor sets in the ring (frames the GPU may still be reading)
constexpr UINT PER_SET = 4; // 3 SRVs + 1 UAV

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};
} // namespace

namespace bloom_stabilizer {
bool init(ID3D12Device* device) {
    g_device = device;
    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile_fn = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;

    if (compile_fn == nullptr) {
        log_error("Bloom stabilizer: d3dcompiler_47.dll not available");
        return false;
    }

    ComPtr<ID3DBlob> bytecode{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_fn(SHADER, sizeof(SHADER) - 1, "bloom_stabilizer", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors)))
    {
        log_error("Bloom stabilizer: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    // Root: 8 constants (b0) and a table of 3 SRVs (t0-t2) + 1 UAV (u0); a static linear clamp sampler (s0).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3};

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants = {0, 0, 12};
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = {2, ranges};
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // From the d3d12.dll the game loaded (see bloom_composite: importing it breaks other mods' d3d12.dll proxies).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");

    if (serialize == nullptr) {
        log_error("Bloom stabilizer: D3D12SerializeRootSignature not found");
        return false;
    }

    D3D12_ROOT_SIGNATURE_DESC root{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};

    if (FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Bloom stabilizer: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("Bloom stabilizer: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Bloom stabilizer ready");
    return true;
}

bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* current, ID3D12Resource* history, ID3D12Resource* motion, ID3D12Resource* out,
    uint32_t width, uint32_t height, const float mvec_scale[2], bool reset, float moving_px, float alpha)
{
    if (!g_ready) {
        return false;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;

    ID3D12Resource* inputs[] = {current, history, motion};

    for (UINT i = 0; i < 3; ++i) {
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
    g_device->CreateUnorderedAccessView(out, nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 3 * (SIZE_T)g_increment});

    const float constants[12] = {1.0f / (float)width, 1.0f / (float)height, mvec_scale[0], mvec_scale[1], (float)width, (float)height,
        reset ? 1.0f : 0.0f, moving_px, alpha, 0.0f, 0.0f, 0.0f};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());
    list->SetComputeRoot32BitConstants(0, 12, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    return true;
}
} // namespace bloom_stabilizer
