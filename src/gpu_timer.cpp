#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <format>
#include <mutex>

#include "log.hpp"

#include "gpu_timer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT RING = 16;      // frames of query slots
constexpr UINT READ_AFTER = 8; // a frame's results are read this many frames later (long done on the GPU by then)
constexpr UINT PER_FRAME = gpu_timer::PASS_COUNT * 2;

ComPtr<ID3D12QueryHeap> g_heap{};
ComPtr<ID3D12Resource> g_readback{};
const uint64_t* g_values{}; // the readback buffer, mapped for good
double g_ms_per_tick{};
bool g_ready{};

std::mutex g_mutex{};
uint64_t g_frame{};
uint32_t g_written[RING]{}; // per ring slot: bit per pass whose end was recorded
double g_sum_ms[gpu_timer::PASS_COUNT]{};
uint32_t g_count[gpu_timer::PASS_COUNT]{};

const char* NAMES[gpu_timer::PASS_COUNT] = {"bloom stabilizer", "DLSS evaluate", "upscale guard", "depth of field"};

UINT index(uint64_t frame, gpu_timer::Pass pass, bool end) {
    return (UINT)(frame % RING) * PER_FRAME + (UINT)pass * 2 + (end ? 1 : 0);
}
} // namespace

namespace gpu_timer {
bool init(ID3D12Device* device) {
    // The timestamp frequency comes from a command queue: a temporary one of the type the game renders with.
    ComPtr<ID3D12CommandQueue> queue{};
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    UINT64 frequency = 0;

    if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) || FAILED(queue->GetTimestampFrequency(&frequency)) || frequency == 0) {
        log_error("GPU timer: no timestamp frequency");
        return false;
    }

    D3D12_QUERY_HEAP_DESC heap{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, RING * PER_FRAME, 0};
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = sizeof(uint64_t) * RING * PER_FRAME;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    if (FAILED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&g_heap))) ||
        FAILED(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback))) ||
        FAILED(g_readback->Map(0, nullptr, (void**)&g_values)))
    {
        log_error("GPU timer: query heap or readback buffer failed");
        return false;
    }

    g_ms_per_tick = 1000.0 / (double)frequency;
    g_ready = true;
    log_info("GPU timer ready ({} ticks/s)", frequency);
    return true;
}

void next_frame() {
    if (!g_ready) {
        return;
    }

    std::scoped_lock _{g_mutex};
    ++g_frame;

    if (g_frame < READ_AFTER) {
        return;
    }

    // The frame READ_AFTER back: its passes' times, then its slot is free for reuse.
    const auto old = g_frame - READ_AFTER;
    auto& written = g_written[old % RING];

    for (int p = 0; p < PASS_COUNT; ++p) {
        if ((written & (1u << p)) == 0) {
            continue;
        }

        const auto t0 = g_values[index(old, (Pass)p, false)];
        const auto t1 = g_values[index(old, (Pass)p, true)];

        if (t1 > t0) {
            g_sum_ms[p] += (double)(t1 - t0) * g_ms_per_tick;
            ++g_count[p];
        }
    }

    written = 0;
}

void begin(ID3D12GraphicsCommandList* list, Pass pass) {
    if (!g_ready) {
        return;
    }

    std::scoped_lock _{g_mutex};
    list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index(g_frame, pass, false));
}

void end(ID3D12GraphicsCommandList* list, Pass pass) {
    if (!g_ready) {
        return;
    }

    std::scoped_lock _{g_mutex};
    const auto first = index(g_frame, pass, false);
    list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
    list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2, g_readback.Get(), (UINT64)first * sizeof(uint64_t));
    g_written[g_frame % RING] |= 1u << pass;
}

std::string report() {
    std::scoped_lock _{g_mutex};
    std::string text = "GPU ms:";

    for (int p = 0; p < PASS_COUNT; ++p) {
        text += g_count[p] > 0 ? std::format(" {} {:.3f}", NAMES[p], g_sum_ms[p] / g_count[p]) : std::format(" {} -", NAMES[p]);
        g_sum_ms[p] = 0.0;
        g_count[p] = 0;
    }

    return text;
}
} // namespace gpu_timer
