#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>

#include "log.hpp"

#include "upscale_guard.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char SHADER[] = R"(
Texture2D<float4> engine : register(t0); // the engine's result (full resolution)
Texture2D<float4> post : register(t1);   // the post color it should have upscaled (render resolution)
Texture2D<float4> dlss : register(t2);   // DLSS's output (full resolution)
RWTexture2D<float4> output : register(u0);
RWStructuredBuffer<float4> decision : register(u1); // k, r, match, sum(s*s)
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 inv_out_size;
    float2 uv_scale; // render size / post texture size
    float2 out_size;
    float threshold;
    float live_rule; // 1: a live post color (changed since the last frame) wins over a non-black engine image that doesn't match
};

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

groupshared float3 sums[256];

// One group: 16x16 threads x 3x3 samples = a 48x48 grid over the screen.
[numthreads(16, 16, 1)]
void reduce(uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex) {
    float3 acc = 0; // e*s, s*s, e*e

    for (uint j = 0; j < 3; ++j) {
        for (uint i = 0; i < 3; ++i) {
            const float2 cell = (float2(thread.x * 3 + i, thread.y * 3 + j) + 0.5) / 48.0;
            const uint2 px = min(uint2(cell * out_size), uint2(out_size) - 1);
            const float e = luminance(engine[px].rgb);
            const float s = luminance(post.SampleLevel(linear_clamp, (float2(px) + 0.5) * inv_out_size * uv_scale, 0).rgb);
            acc += float3(e * s, s * s, e * e);
        }
    }

    sums[index] = acc;
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (index < stride) sums[index] += sums[index + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (index == 0) {
        const float3 t = sums[0];
        float k = t.y > 1e-8 ? t.x / t.y : 0.0;
        // The least-squares residual: sum((e - k*s)^2) = sum(e*e) - k*sum(e*s).
        const float r = max(t.z - k * t.x, 0.0) / max(t.z, 1e-8);
        const bool match = t.y > 1e-8 && t.z > 1e-8 && r < threshold;
        // The live rule (RE2's item box: its engine image is black behind the menu, the post color is the dimmed room): a
        // post color that changed since the last frame (the jitter changes every live frame; a stale one, as behind splash
        // screens, stays the same to the bit) and an engine image that isn't black (a fade's black frame stays black).
        const float previous = decision[0].w;
        const bool live = live_rule > 0.5 && !match && t.y > 1e-8 && t.z > 1e-8 && abs(t.y - previous) > 1e-7 * t.y;
        // A steady frame's k is 1 up to the engine's filter: no brightness wobble from that.
        k = abs(k - 1.0) < 0.03 ? 1.0 : saturate(k);
        decision[0] = float4(live ? 1.0 : k, r, match ? 1.0 : live ? 2.0 : 0.0, t.y);
    }
}

[numthreads(8, 8, 1)]
void apply(uint3 id : SV_DispatchThreadID) {
    const float4 d = decision[0];

    if (d.z > 0.5) {
        const float4 upscaled = dlss[id.xy];
        output[id.xy] = float4(upscaled.rgb * d.x, upscaled.a);
    } else {
        output[id.xy] = engine[id.xy];
    }
}
)";

constexpr UINT SETS = 64; // descriptor sets in the ring (frames the GPU may still be reading)
constexpr UINT PER_SET = 5; // 3 SRVs + 2 UAVs
std::atomic<UINT> g_readback_every{600}; // frames (setting dlss_guard_log)
float g_last_s2{};
bool g_last_live{};

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_reduce{};
ComPtr<ID3D12PipelineState> g_apply{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
ComPtr<ID3D12Resource> g_decision{};
ComPtr<ID3D12Resource> g_readback{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
std::atomic<UINT> g_frames{};
std::atomic<bool> g_readback_pending{};
bool g_ready{};

bool compile(const char* entry, ComPtr<ID3DBlob>& bytecode) {
    const auto module = LoadLibraryW(L"d3dcompiler_47.dll");
    const auto compile_fn = module != nullptr ? (pD3DCompile)GetProcAddress(module, "D3DCompile") : nullptr;

    if (compile_fn == nullptr) {
        log_error("Upscale guard: d3dcompiler_47.dll not available");
        return false;
    }

    ComPtr<ID3DBlob> errors{};

    if (FAILED(compile_fn(SHADER, sizeof(SHADER) - 1, "upscale_guard", nullptr, nullptr, entry, "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors)))
    {
        log_error("Upscale guard: shader compile failed ({}): {}", entry, errors != nullptr ? (const char*)errors->GetBufferPointer() : "?");
        return false;
    }

    return true;
}

bool make_buffer(D3D12_HEAP_TYPE heap_type, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& buffer) {
    D3D12_HEAP_PROPERTIES heap{heap_type};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 16;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return SUCCEEDED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer)));
}

void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
} // namespace

namespace upscale_guard {
bool init(ID3D12Device* device) {
    g_device = device;
    ComPtr<ID3DBlob> reduce{};
    ComPtr<ID3DBlob> apply{};

    if (!compile("reduce", reduce) || !compile("apply", apply)) {
        return false;
    }

    // Root: 8 constants (b0) and a table of 3 SRVs (t0-t2) + 2 UAVs (u0-u1); a static linear clamp sampler (s0).
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
    ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 3};

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

    // From the d3d12.dll the game loaded (see bloom_composite: importing it breaks other mods' d3d12.dll proxies).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");

    if (serialize == nullptr) {
        log_error("Upscale guard: D3D12SerializeRootSignature not found");
        return false;
    }

    D3D12_ROOT_SIGNATURE_DESC root{2, params, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};
    ComPtr<ID3DBlob> errors{};

    if (FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Upscale guard: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {reduce->GetBufferPointer(), reduce->GetBufferSize()};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_reduce)))) {
        log_error("Upscale guard: pipeline state failed (reduce)");
        return false;
    }

    pso.CS = {apply->GetBufferPointer(), apply->GetBufferSize()};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_apply)))) {
        log_error("Upscale guard: pipeline state failed (apply)");
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap))) ||
        !make_buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, g_decision) ||
        !make_buffer(D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, g_readback))
    {
        log_error("Upscale guard: descriptor heap or buffers failed");
        return false;
    }

    g_decision->SetName(L"refw_framegen upscale guard decision");
    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Upscale guard ready");
    return true;
}

bool record(ID3D12GraphicsCommandList* list, ID3D12Resource* engine, ID3D12Resource* post, ID3D12Resource* dlss, ID3D12Resource* out,
    uint32_t out_w, uint32_t out_h, uint32_t render_w, uint32_t render_h, float threshold, bool live_rule)
{
    if (!g_ready) {
        return false;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;

    ID3D12Resource* inputs[] = {engine, post, dlss};

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

    D3D12_UNORDERED_ACCESS_VIEW_DESC buffer_uav{};
    buffer_uav.Format = DXGI_FORMAT_UNKNOWN;
    buffer_uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    buffer_uav.Buffer.NumElements = 1;
    buffer_uav.Buffer.StructureByteStride = 16;
    g_device->CreateUnorderedAccessView(g_decision.Get(), nullptr, &buffer_uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 4 * (SIZE_T)g_increment});

    const auto post_desc = post->GetDesc();
    const float constants[8] = {1.0f / (float)out_w, 1.0f / (float)out_h, (float)render_w / (float)post_desc.Width,
        (float)render_h / (float)post_desc.Height, (float)out_w, (float)out_h, threshold, live_rule ? 1.0f : 0.0f};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetComputeRoot32BitConstants(0, 8, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);

    list->SetPipelineState(g_reduce.Get());
    list->Dispatch(1, 1, 1);

    D3D12_RESOURCE_BARRIER uav_barrier{};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = g_decision.Get();
    list->ResourceBarrier(1, &uav_barrier);

    list->SetPipelineState(g_apply.Get());
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);

    // Every few seconds a copy of the decision for the log (read READBACK_EVERY frames later, long done by then).
    if (g_frames++ % g_readback_every.load() == 0) {
        list->ResourceBarrier(1, &uav_barrier);
        barrier(list, g_decision.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(g_readback.Get(), g_decision.Get());
        barrier(list, g_decision.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_readback_pending = true;
    }

    return true;
}

bool latest(float& k, float& r, bool& match) {
    // Only between copies (the last one is READBACK_EVERY frames old when the next is due).
    if (!g_ready || !g_readback_pending || g_frames % g_readback_every.load() != g_readback_every.load() - 1) {
        return false;
    }

    float* values = nullptr;
    const D3D12_RANGE range{0, 16};

    if (FAILED(g_readback->Map(0, &range, (void**)&values))) {
        return false;
    }

    k = values[0];
    r = values[1];
    match = values[2] > 0.5f;
    g_last_s2 = values[3];
    g_last_live = values[2] > 1.5f;
    const D3D12_RANGE none{0, 0};
    g_readback->Unmap(0, &none);
    g_readback_pending = false;
    return true;
}
} // namespace upscale_guard

namespace upscale_guard {
void set_log_every(uint32_t frames) {
    g_readback_every = frames < 10 ? 10 : frames;
}

bool last_live() {
    return g_last_live;
}

float last_post_energy() {
    return g_last_s2;
}
} // namespace upscale_guard
