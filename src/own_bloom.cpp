#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

#include "log.hpp"

#include "own_bloom.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// One shader, four passes (mode): 0 prefilter (the full-size scene -> level 0, threshold, brightness-weighted), 1 downsample,
// 2 upsample (this level's downsample blended with the coarser upsample), 3 composite (the scene with the bloom).
constexpr char SHADER[] = R"(
Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t2 : register(t2); // the measured post / HDR scale (1x1, r)
RWTexture2D<float4> output : register(u0);
SamplerState linear_clamp : register(s0);

cbuffer Constants : register(b0) {
    float2 src_texel;  // 1 / the size of t0 (prefilter / downsample: the finer level; upsample: t1's)
    float2 dst_size;
    float threshold;
    float dispersion;
    float ratio;
    float mode;
    float additive;
    float norm;      // composite: a scale for the summed levels (1)
    float fine_weight; // upsample: this level's own weight (the inner glow boost on the finest levels)
    float pad;
};

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

groupshared float4 g_sums[64]; // per thread: sum of HDR luma, sum of post luma, max post luma, max HDR luma

// CoD: Advanced Warfare's 13-tap downsample: five overlapping 2x2 boxes (the centre one weighted 0.5). With `karis`, each
// box weighted by 1 / (1 + its luma): a single very bright pixel can't dominate (no flickering fireflies).
float3 downsample13(Texture2D<float4> tex, float2 uv, float2 texel, bool karis) {
    const float3 a = tex.SampleLevel(linear_clamp, uv + texel * float2(-2, -2), 0).rgb;
    const float3 b = tex.SampleLevel(linear_clamp, uv + texel * float2(0, -2), 0).rgb;
    const float3 c = tex.SampleLevel(linear_clamp, uv + texel * float2(2, -2), 0).rgb;
    const float3 d = tex.SampleLevel(linear_clamp, uv + texel * float2(-1, -1), 0).rgb;
    const float3 e = tex.SampleLevel(linear_clamp, uv + texel * float2(1, -1), 0).rgb;
    const float3 f = tex.SampleLevel(linear_clamp, uv + texel * float2(-2, 0), 0).rgb;
    const float3 g = tex.SampleLevel(linear_clamp, uv, 0).rgb;
    const float3 h = tex.SampleLevel(linear_clamp, uv + texel * float2(2, 0), 0).rgb;
    const float3 i = tex.SampleLevel(linear_clamp, uv + texel * float2(-1, 1), 0).rgb;
    const float3 j = tex.SampleLevel(linear_clamp, uv + texel * float2(1, 1), 0).rgb;
    const float3 k = tex.SampleLevel(linear_clamp, uv + texel * float2(-2, 2), 0).rgb;
    const float3 l = tex.SampleLevel(linear_clamp, uv + texel * float2(0, 2), 0).rgb;
    const float3 m = tex.SampleLevel(linear_clamp, uv + texel * float2(2, 2), 0).rgb;

    float3 boxes[5] = {(d + e + i + j) * 0.25, (a + b + f + g) * 0.25, (b + c + g + h) * 0.25, (f + g + k + l) * 0.25,
        (g + h + l + m) * 0.25};
    const float weights[5] = {0.5, 0.125, 0.125, 0.125, 0.125};
    float3 sum = 0.0;
    float total = 0.0;

    [unroll]
    for (int n = 0; n < 5; ++n) {
        const float w = weights[n] * (karis ? 1.0 / (1.0 + luma(boxes[n])) : 1.0);
        sum += boxes[n] * w;
        total += w;
    }

    return sum / total;
}

float3 tent9(Texture2D<float4> tex, float2 uv, float2 texel) {
    float3 s = tex.SampleLevel(linear_clamp, uv, 0).rgb * 4.0;
    s += (tex.SampleLevel(linear_clamp, uv + texel * float2(-1, 0), 0).rgb + tex.SampleLevel(linear_clamp, uv + texel * float2(1, 0), 0).rgb +
          tex.SampleLevel(linear_clamp, uv + texel * float2(0, -1), 0).rgb + tex.SampleLevel(linear_clamp, uv + texel * float2(0, 1), 0).rgb) * 2.0;
    s += tex.SampleLevel(linear_clamp, uv + texel * float2(-1, -1), 0).rgb + tex.SampleLevel(linear_clamp, uv + texel * float2(1, -1), 0).rgb +
         tex.SampleLevel(linear_clamp, uv + texel * float2(-1, 1), 0).rgb + tex.SampleLevel(linear_clamp, uv + texel * float2(1, 1), 0).rgb;
    return s / 16.0;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    // Mode 4 (one group of 8x8): the brightness of the HDR scene (t0) and the post effects' result (t1) on a 32x32 grid of
    // the render image; their ratio, smoothed over frames, into the 1x1 target.
    if (mode > 3.5) {
        const uint index = id.y * 8 + id.x;
        float4 sum = 0.0;

        [unroll]
        for (int gy = 0; gy < 4; ++gy) {
            [unroll]
            for (int gx = 0; gx < 4; ++gx) {
                const float2 p = (float2(id.x * 4 + gx, id.y * 4 + gy) + 0.5) / 32.0;
                const float a = luma(t0.SampleLevel(linear_clamp, p, 0).rgb);
                const float b = luma(t1.SampleLevel(linear_clamp, p, 0).rgb);

                if (a >= 0.0 && b >= 0.0 && a < 65000.0 && b < 65000.0) {
                    sum.xy += float2(a, b);
                    sum.z = max(sum.z, b);
                    sum.w = max(sum.w, a);
                }
            }
        }

        g_sums[index] = sum;
        GroupMemoryBarrierWithGroupSync();

        if (index == 0) {
            float4 total = 0.0;

            for (uint n = 0; n < 64; ++n) {
                total.xy += g_sums[n].xy;
                total.z = max(total.z, g_sums[n].z);
                total.w = max(total.w, g_sums[n].w);
            }

            const float measured = total.x > 1e-12 ? total.y / total.x : -1.0;
            const float previous = output[uint2(0, 0)].r;
            const float k = measured <= 0.0 ? previous : previous > 0.0 ? lerp(previous, measured, 0.1) : measured;
            // r: the smoothed post / HDR ratio; g: mean post luma; b: max post luma; a: mean HDR luma (1024 samples).
            output[uint2(0, 0)] = float4(k, total.y / 1024.0, total.z, total.x / 1024.0);
        }

        return;
    }

    if (any(float2(id.xy) >= dst_size)) {
        return;
    }

    const float2 uv = (float2(id.xy) + 0.5) / dst_size;

    if (mode < 0.5) {
        float3 c = downsample13(t0, uv, src_texel, true);
        const float l = luma(c);
        const float scale = t2[uint2(0, 0)].r;
        // The game's Threshold in tenths of the HDR scene's brightness (100 -> 10: matched by eye against the game's own
        // 2026-11-05; as raw brightness it was above every pixel), in DLSS's units via the measured ratio. < 0: an override.
        const float t = threshold < 0.0 ? -threshold - 1.0 : threshold * 0.1 * (scale > 0.0 ? scale : 1.0);
        c *= max(l - t, 0.0) / max(l, 1e-5); // only what is brighter than the threshold (in DLSS's image's units)
        output[id.xy] = float4(c, 1.0);
    } else if (mode < 1.5) {
        output[id.xy] = float4(downsample13(t0, uv, src_texel, false), 1.0);
    } else if (mode < 2.5) {
        const float3 fine = t0.SampleLevel(linear_clamp, uv, 0).rgb;
        const float3 coarse = tent9(t1, uv, src_texel);
        output[id.xy] = float4(fine * fine_weight + coarse * dispersion, 1.0); // CoD:AW: every level adds up, the coarser ones weighted
    } else {
        const float4 scene = t0[id.xy];
        const float3 bloom = tent9(t1, uv, src_texel) * norm;
        const float3 result = additive > 0.5 ? scene.rgb + bloom * ratio : lerp(scene.rgb, bloom, ratio);
        output[id.xy] = float4(result, scene.a);
    }
}
)";

constexpr UINT MAX_LEVELS = 10;
constexpr UINT PASSES = MAX_LEVELS * 2 + 3;
constexpr UINT FRAMES = 16; // descriptor sets in the ring (frames the GPU may still be reading)
constexpr UINT PER_PASS = 4; // 3 SRVs + 1 UAV
constexpr UINT CONSTANTS = 16;

ComPtr<ID3D12Device> g_device{};
ComPtr<ID3D12RootSignature> g_root{};
ComPtr<ID3D12PipelineState> g_pso{};
ComPtr<ID3D12DescriptorHeap> g_heap{};
UINT g_increment{};
std::atomic<UINT> g_next_set{};
bool g_ready{};

struct Chain {
    UINT width{};
    UINT height{};
    DXGI_FORMAT format{};
    std::vector<ComPtr<ID3D12Resource>> down{};
    std::vector<ComPtr<ID3D12Resource>> up{};
    ComPtr<ID3D12Resource> output{};
} g_chain{};

std::vector<ComPtr<ID3D12Resource>> g_retired{};
ComPtr<ID3D12Resource> g_scale{}; // 1x1 R32G32B32A32_FLOAT: the post / HDR ratio and brightness statistics (UNORDERED_ACCESS between frames)
ComPtr<ID3D12Resource> g_stats_readback{}; // a copy of it every few seconds, for the log
UINT g_stats_calls{};
bool g_stats_pending{};

ComPtr<ID3D12Resource> make_texture(UINT w, UINT h, DXGI_FORMAT format, const wchar_t* name) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> texture{};

    if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }

    texture->SetName(name);
    return texture;
}

// The pyramid for this output size: level 0 at half the output (as the game's own at native resolution), halved down to a
// height of 4 (the game's: 7x4 at the bottom).
bool ensure_chain(UINT w, UINT h, DXGI_FORMAT src_format) {
    if (g_chain.width == w && g_chain.height == h && g_chain.format == src_format && g_chain.output != nullptr) {
        return true;
    }

    for (auto& t : g_chain.down) g_retired.push_back(t);
    for (auto& t : g_chain.up) g_retired.push_back(t);
    if (g_chain.output != nullptr) g_retired.push_back(g_chain.output);
    g_chain = {};

    UINT lw = std::max(1u, w / 2);
    UINT lh = std::max(1u, h / 2);

    for (UINT i = 0; i < MAX_LEVELS; ++i) {
        auto down = make_texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"refw_framegen bloom down");
        auto up = make_texture(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, L"refw_framegen bloom up");

        if (down == nullptr || up == nullptr) {
            log_error("Bloom: pyramid texture failed");
            g_chain = {};
            return false;
        }

        g_chain.down.push_back(down);
        g_chain.up.push_back(up);

        if (lh <= 4 || lw <= 4) {
            break;
        }

        lw = std::max(1u, lw / 2);
        lh = std::max(1u, lh / 2);
    }

    g_chain.output = make_texture(w, h, src_format, L"refw_framegen bloom output");

    if (g_chain.output == nullptr) {
        log_error("Bloom: output texture failed");
        g_chain = {};
        return false;
    }

    g_chain.width = w;
    g_chain.height = h;
    g_chain.format = src_format;
    log_info("Bloom: pyramid of {} levels for {}x{} (finest {}x{})", g_chain.down.size(), w, h, w / 2, h / 2);
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

void make_uav(ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE where) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = resource->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g_device->CreateUnorderedAccessView(resource, nullptr, &uav, where);
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

constexpr auto READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto WRITE = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
} // namespace

namespace own_bloom {
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
        FAILED(compile(SHADER, sizeof(SHADER) - 1, "own_bloom", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors)))
    {
        log_error("Bloom: shader compile failed: {}", errors != nullptr ? (const char*)errors->GetBufferPointer() : "no d3dcompiler_47.dll");
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
        log_error("Bloom: root signature failed");
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g_root.Get();
    pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, FRAMES * PASSES * PER_PASS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};

    if (FAILED(device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g_pso))) || FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&g_heap)))) {
        log_error("Bloom: pipeline state or descriptor heap failed");
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_ready = true;
    log_info("Bloom pass ready");
    return true;
}

ID3D12Resource* record(ID3D12GraphicsCommandList* list, ID3D12Resource* src, ID3D12Resource* hdr, D3D12_RESOURCE_STATES hdr_state,
    ID3D12Resource* post, D3D12_RESOURCE_STATES post_state, uint32_t out_w, uint32_t out_h, const Params& params)
{
    if (!g_ready || src == nullptr || !ensure_chain(out_w, out_h, src->GetDesc().Format)) {
        return nullptr;
    }

    if (g_scale == nullptr) {
        g_scale = make_texture(1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L"refw_framegen bloom threshold scale");

        if (g_scale == nullptr) {
            return nullptr;
        }
    }

    const auto frame = g_next_set++ % FRAMES;
    auto cpu = g_heap->GetCPUDescriptorHandleForHeapStart();
    auto gpu = g_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)frame * PASSES * PER_PASS * g_increment;
    gpu.ptr += (UINT64)frame * PASSES * PER_PASS * g_increment;

    ID3D12DescriptorHeap* heaps[] = {g_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_root.Get());
    list->SetPipelineState(g_pso.Get());

    // One pass: t0, t1 (nullptr = t0 again), the target (UNORDERED_ACCESS, then readable for the next passes).
    UINT pass = 0;
    const auto disp = std::clamp(params.dispersion, 0.0f, 1.0f);
    const auto norm = 1.0f; // CoD:AW sums its levels (an average made the bloom 8x weaker)

    const auto run = [&](float mode, ID3D12Resource* a, ID3D12Resource* b, ID3D12Resource* target, ID3D12Resource* texel_of, bool target_to_read,
                         ID3D12Resource* c = nullptr, float fine_weight = 1.0f) {
        const auto set_cpu = D3D12_CPU_DESCRIPTOR_HANDLE{cpu.ptr + (SIZE_T)pass * PER_PASS * g_increment};
        const auto set_gpu = D3D12_GPU_DESCRIPTOR_HANDLE{gpu.ptr + (UINT64)pass * PER_PASS * g_increment};
        ++pass;
        make_srv(a, set_cpu);
        make_srv(b != nullptr ? b : a, D3D12_CPU_DESCRIPTOR_HANDLE{set_cpu.ptr + g_increment});
        make_srv(c != nullptr ? c : a, D3D12_CPU_DESCRIPTOR_HANDLE{set_cpu.ptr + 2 * (SIZE_T)g_increment});
        make_uav(target, D3D12_CPU_DESCRIPTOR_HANDLE{set_cpu.ptr + 3 * (SIZE_T)g_increment});

        const auto t = texel_of->GetDesc();
        const auto d = target->GetDesc();
        const float constants[CONSTANTS] = {1.0f / (float)t.Width, 1.0f / (float)t.Height, (float)d.Width, (float)d.Height,
            params.threshold_override >= 0.0f ? -(params.threshold_override + 1.0f) : params.threshold, disp, std::clamp(params.ratio * params.strength, 0.0f, 4.0f), mode,
            params.additive ? 1.0f : 0.0f, norm, fine_weight, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        list->SetComputeRoot32BitConstants(0, CONSTANTS, constants, 0);
        list->SetComputeRootDescriptorTable(1, set_gpu);
        list->Dispatch(mode > 3.5f ? 1 : ((UINT)d.Width + 7) / 8, mode > 3.5f ? 1 : (d.Height + 7) / 8, 1);

        if (target_to_read) {
            transition(list, target, WRITE, READ);
        } else {
            D3D12_RESOURCE_BARRIER written{};
            written.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            written.UAV.pResource = target;
            list->ResourceBarrier(1, &written);
        }
    };

    const auto levels = (UINT)g_chain.down.size();

    // The threshold's scale: the HDR scene vs DLSS's input (both readable for it, then back).
    if (hdr != nullptr && post != nullptr) {
        if (hdr_state != READ && hdr_state != (READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) transition(list, hdr, hdr_state, READ);
        if (post_state != READ && post_state != (READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) transition(list, post, post_state, READ);
        run(4.0f, hdr, post, g_scale.Get(), hdr, true);
        if (hdr_state != READ && hdr_state != (READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) transition(list, hdr, READ, hdr_state);
        if (post_state != READ && post_state != (READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) transition(list, post, READ, post_state);
    } else {
        transition(list, g_scale.Get(), WRITE, READ);
    }

    run(0.0f, src, nullptr, g_chain.down[0].Get(), src, true, g_scale.Get());

    for (UINT i = 1; i < levels; ++i) {
        run(1.0f, g_chain.down[i - 1].Get(), nullptr, g_chain.down[i].Get(), g_chain.down[i - 1].Get(), true);
    }

    // The coarsest level's upsample is its downsample.
    ID3D12Resource* coarser = g_chain.down[levels - 1].Get();

    for (int i = (int)levels - 2; i >= 0; --i) {
        // The inner glow: the finest levels weighted up (level 0 by 1 + inner, halving per level).
        run(2.0f, g_chain.down[i].Get(), coarser, g_chain.up[i].Get(), coarser, true, nullptr, 1.0f + params.inner * std::pow(0.5f, (float)i));
        coarser = g_chain.up[i].Get();
    }

    run(3.0f, src, coarser, g_chain.output.Get(), coarser, false);

    // Diagnostics: the measured statistics in the log every ~5 s (copied now, read 300 frames later, certainly done).
    if (g_stats_readback == nullptr) {
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = 256;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_stats_readback));
    }

    if (g_stats_readback != nullptr && ++g_stats_calls % 300 == 0) {
        if (g_stats_pending) {
            float* values{};
            D3D12_RANGE range{0, 16};

            if (SUCCEEDED(g_stats_readback->Map(0, &range, (void**)&values)) && values != nullptr) {
                log_info("Bloom: DLSS input / HDR scene brightness {:.5f} (smoothed); DLSS input mean {:.5f} max {:.4f}; HDR scene mean {:.5f}; "
                         "the game's threshold {:.3f} -> {:.5f} in DLSS's units",
                    values[0], values[1], values[2], values[3], params.threshold, params.threshold * values[0]);
                D3D12_RANGE none{0, 0};
                g_stats_readback->Unmap(0, &none);
            }
        }

        transition(list, g_scale.Get(), READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{g_stats_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint.Footprint = {DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 256};
        D3D12_TEXTURE_COPY_LOCATION from{g_scale.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&dst, 0, 0, 0, &from, nullptr);
        transition(list, g_scale.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, READ);
        g_stats_pending = true;
    }

    // Back to UNORDERED_ACCESS for the next frame.
    transition(list, g_scale.Get(), READ, WRITE);
    for (UINT i = 0; i < levels; ++i) {
        transition(list, g_chain.down[i].Get(), READ, WRITE);
    }

    for (int i = (int)levels - 2; i >= 0; --i) {
        transition(list, g_chain.up[i].Get(), READ, WRITE);
    }

    return g_chain.output.Get();
}
} // namespace own_bloom
