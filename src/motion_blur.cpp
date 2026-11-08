#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <vector>

#include "log.hpp"

#include "motion_blur.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char SHADER[] = R"(
Texture2D<float4> src : register(t0);    // DLSS's output (output size)
Texture2D<float4> motion : register(t1); // the engine's velocity (render resolution, xy in clip space units)
RWTexture2D<float4> output : register(u0);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);

cbuffer Constants : register(b0) {
    float2 inv_out;
    float2 motion_uv_scale; // render extent / velocity texture size
    float2 out_size;
    float shutter;          // the share of the frame's movement blurred
    float max_px;           // the longest blur (output pixels)
};

// The movement to the previous frame in UV (the velocity's convention, as for DLSS: x 0.5, y -0.5).
float2 velocity_uv(float2 uv) {
    return motion.SampleLevel(point_clamp, uv * motion_uv_scale, 0).xy * float2(0.5, -0.5);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(float2(id.xy) >= out_size)) {
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) * inv_out;
    const float4 c = src[id.xy];
    const float2 v = velocity_uv(uv);
    float2 blur = v * shutter;
    float len = length(blur * out_size);

    if (len < 0.5) {
        output[id.xy] = c;
        return;
    }

    if (len > max_px) {
        blur *= max_px / len;
        len = max_px;
    }

    // Centred on the pixel (the shutter is open around the frame's moment); samples of another object (their own motion
    // far from this pixel's) are left out, so a moving character doesn't smear the still background or the reverse.
    const int count = clamp((int)(len * 0.5), 4, 16);
    const float v_len = length(v * out_size);
    float3 color = c.rgb;
    float total = 1.0;

    [loop]
    for (int i = 0; i < count; ++i) {
        const float t = ((float)i + 0.5) / (float)count - 0.5;
        const float2 p = uv + blur * t;
        const float2 vs = velocity_uv(p);

        if (length((vs - v) * out_size) <= 0.5 * max(v_len, length(vs * out_size)) + 1.0) {
            color += src.SampleLevel(linear_clamp, p, 0).rgb;
            total += 1.0;
        }
    }

    output[id.xy] = float4(color / total, c.a);
}
)";

constexpr UINT SETS = 64;
constexpr UINT PER_SET = 3;
constexpr UINT CONSTANTS = 8;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};
ComPtr<ID3D12Resource> g_output{}; // like the source, UNORDERED_ACCESS
std::vector<ComPtr<ID3D12Resource>> g_retired{};

bool ensure_output(const D3D12_RESOURCE_DESC& like) {
    if (g_output != nullptr) {
        const auto desc = g_output->GetDesc();

        if (desc.Width == like.Width && desc.Height == like.Height && desc.Format == like.Format) {
            return true;
        }

        g_retired.push_back(g_output);
        g_output = nullptr;
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

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&g_output))))
    {
        log_error("Motion blur: output texture failed");
        return false;
    }

    g_output->SetName(L"refw_framegen motion blur");
    return true;
}

void make_srv(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = resource->GetDesc().Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_device->CreateShaderResourceView(resource, &srv, where);
}
} // namespace

namespace motion_blur {
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
        FAILED(compile(SHADER, sizeof(SHADER) - 1, "motion_blur", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        log_error("Motion blur: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
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

    D3D12_STATIC_SAMPLER_DESC samplers[2]{};
    samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    samplers[1] = samplers[0];
    samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samplers[1].ShaderRegister = 1;

    // From the d3d12.dll the game loaded (importing it breaks other mods' d3d12.dll proxies).
    const auto serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE)GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature");
    D3D12_ROOT_SIGNATURE_DESC root{2, params, 2, samplers, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> serialized{};

    if (serialize == nullptr || FAILED(serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&g_root))))
    {
        log_error("Motion blur: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, SETS * PER_SET, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("Motion blur: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Motion blur pass ready");
    return true;
}

ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* motion, uint32_t out_w, uint32_t out_h,
    uint32_t render_w, uint32_t render_h, float shutter)
{
    if (!g_ready || src == nullptr || motion == nullptr || !ensure_output(src->GetDesc())) {
        return nullptr;
    }

    const auto set = g_next_set++ % SETS;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)set * PER_SET * g_increment;
    gpu.ptr += (UINT64)set * PER_SET * g_increment;

    make_srv(src, cpu);
    make_srv(motion, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + g_increment});
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = g_output->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(g_output.Get(), nullptr, &uav, D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + 2 * (SIZE_T)g_increment});

    const auto motion_desc = motion->GetDesc();
    const float constants[CONSTANTS] = {1.0f / (float)out_w, 1.0f / (float)out_h, (float)render_w / (float)motion_desc.Width,
        (float)render_h / (float)motion_desc.Height, (float)out_w, (float)out_h, std::clamp(shutter, 0.0f, 2.0f), 0.04f * (float)out_h};

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());
    list->SetComputeRoot32BitConstants(0, CONSTANTS, constants, 0);
    list->SetComputeRootDescriptorTable(1, gpu);
    list->Dispatch((out_w + 7) / 8, (out_h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER written{};
    written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    written.UAV.pResource = g_output.Get();
    list->ResourceBarrier(1, &written);
    return g_output.Get();
}
} // namespace motion_blur
