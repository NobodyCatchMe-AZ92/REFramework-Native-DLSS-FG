#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <thread>

#include "log.hpp"

#include "frame_pacer.hpp"

using Microsoft::WRL::ComPtr;

namespace {
ComPtr<ID3D12Fence> g_fence{};
ComPtr<ID3D12CommandQueue> g_queue{};
HANDLE g_waitable{};       // released once per finished frame: the game waits on it before starting one
HANDLE g_submitted{};      // released once per presented frame: the thread waits for its fence
std::atomic<uint64_t> g_value{};
std::atomic<int> g_latency{1};
std::atomic<int> g_debt{};  // releases to hold back after the latency went down
std::atomic<bool> g_ready{};

void completion_thread() {
    const auto event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t value = 0;

    for (;;) {
        WaitForSingleObject(g_submitted, INFINITE);
        ++value;

        if (g_fence->GetCompletedValue() < value) {
            g_fence->SetEventOnCompletion(value, event);
            WaitForSingleObject(event, INFINITE);
        }

        // The latency went down: this frame's release is kept back instead.
        if (auto debt = g_debt.load(); debt > 0 && g_debt.compare_exchange_strong(debt, debt - 1)) {
            continue;
        }

        ReleaseSemaphore(g_waitable, 1, nullptr);
    }
}
} // namespace

namespace frame_pacer {
bool init(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (g_ready) {
        return true;
    }

    if (device == nullptr || queue == nullptr || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))) {
        log_error("Frame pacer: no fence");
        return false;
    }

    g_fence->SetName(L"refw_framegen frame pacer");
    g_queue = queue;
    g_waitable = CreateSemaphoreW(nullptr, g_latency.load(), 64, nullptr);
    g_submitted = CreateSemaphoreW(nullptr, 0, 1 << 20, nullptr);

    if (g_waitable == nullptr || g_submitted == nullptr) {
        log_error("Frame pacer: no semaphores");
        return false;
    }

    std::thread{completion_thread}.detach();
    g_ready = true;
    log_info("Frame pacer ready: the game's frame latency waitable is the plugin's (released when each frame's GPU work is done)");
    return true;
}

bool ready() {
    return g_ready;
}

HANDLE waitable() {
    return g_waitable;
}

void set_max_latency(int frames) {
    frames = std::clamp(frames, 1, 16);
    const auto before = g_latency.exchange(frames);

    if (!g_ready || frames == before) {
        return;
    }

    if (frames > before) {
        // Pay back held-back releases first, then add the rest.
        auto extra = frames - before;

        while (extra > 0) {
            auto debt = g_debt.load();

            if (debt <= 0) {
                break;
            }

            if (g_debt.compare_exchange_strong(debt, debt - 1)) {
                --extra;
            }
        }

        if (extra > 0) {
            ReleaseSemaphore(g_waitable, extra, nullptr);
        }
    } else {
        g_debt += before - frames;
    }

    log_info("Frame pacer: maximum frame latency {}", frames);
}

int max_latency() {
    return g_latency.load();
}

void on_present() {
    if (!g_ready) {
        return;
    }

    g_queue->Signal(g_fence.Get(), ++g_value);
    ReleaseSemaphore(g_submitted, 1, nullptr);
}
} // namespace frame_pacer
