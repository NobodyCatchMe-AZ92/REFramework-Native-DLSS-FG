#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <unordered_set>
#include <vector>

#include <MinHook.h>

#include "log.hpp"

#include "wait_probe.hpp"

using Microsoft::WRL::ComPtr;

namespace {
enum Kind { WAITABLE, FENCE, OTHER, KINDS };

struct ThreadWaits {
    std::atomic<DWORD> id{};
    std::atomic<int64_t> ticks[KINDS]{};
};

constexpr int MAX_THREADS = 256;
ThreadWaits g_threads[MAX_THREADS]{};
std::atomic<int> g_thread_count{};
thread_local ThreadWaits* t_slot{};
thread_local bool t_inside{}; // WaitForSingleObject -> WaitForSingleObjectEx: counted once

std::atomic<bool> g_enabled{};
std::atomic<HANDLE> g_waitable{};
std::atomic<DWORD> g_present_thread{};
std::atomic<uint32_t> g_frames{};

std::mutex g_fence_mutex{};
std::unordered_set<HANDLE> g_fence_events{};

using WaitSingleFn = DWORD(WINAPI*)(HANDLE, DWORD);
using WaitSingleExFn = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using WaitMultipleExFn = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
using SetEventOnCompletionFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12Fence*, UINT64, HANDLE);
WaitSingleFn g_wait_single{};
WaitSingleExFn g_wait_single_ex{};
WaitMultipleExFn g_wait_multiple_ex{};
SetEventOnCompletionFn g_set_event_on_completion{};
std::vector<void*> g_targets{};

int64_t now() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

ThreadWaits* slot() {
    if (t_slot == nullptr) {
        const auto index = g_thread_count++;

        if (index >= MAX_THREADS) {
            return nullptr;
        }

        t_slot = &g_threads[index];
        t_slot->id = GetCurrentThreadId();
    }

    return t_slot;
}

Kind classify(HANDLE handle) {
    if (handle == g_waitable.load()) {
        return WAITABLE;
    }

    std::scoped_lock _{g_fence_mutex};
    return g_fence_events.contains(handle) ? FENCE : OTHER;
}

void record(Kind kind, int64_t ticks) {
    if (const auto s = slot()) {
        s->ticks[kind] += ticks;
    }
}

DWORD WINAPI wait_single_hook(HANDLE handle, DWORD ms) {
    if (t_inside || !g_enabled.load(std::memory_order_relaxed)) {
        return g_wait_single(handle, ms);
    }

    t_inside = true;
    const auto t0 = now();
    const auto result = g_wait_single(handle, ms);
    record(classify(handle), now() - t0);
    t_inside = false;
    return result;
}

DWORD WINAPI wait_single_ex_hook(HANDLE handle, DWORD ms, BOOL alertable) {
    if (t_inside || !g_enabled.load(std::memory_order_relaxed)) {
        return g_wait_single_ex(handle, ms, alertable);
    }

    t_inside = true;
    const auto t0 = now();
    const auto result = g_wait_single_ex(handle, ms, alertable);
    record(classify(handle), now() - t0);
    t_inside = false;
    return result;
}

DWORD WINAPI wait_multiple_ex_hook(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL alertable) {
    if (t_inside || !g_enabled.load(std::memory_order_relaxed)) {
        return g_wait_multiple_ex(count, handles, all, ms, alertable);
    }

    t_inside = true;
    auto kind = OTHER;

    for (DWORD i = 0; i < count && kind == OTHER; ++i) {
        kind = classify(handles[i]);
    }

    const auto t0 = now();
    const auto result = g_wait_multiple_ex(count, handles, all, ms, alertable);
    record(kind, now() - t0);
    t_inside = false;
    return result;
}

HRESULT STDMETHODCALLTYPE set_event_on_completion_hook(ID3D12Fence* fence, UINT64 value, HANDLE event) {
    if (event != nullptr && g_enabled.load(std::memory_order_relaxed)) {
        std::scoped_lock _{g_fence_mutex};
        g_fence_events.insert(event);
    }

    return g_set_event_on_completion(fence, value, event);
}

template <typename T>
bool hook(void* target, void* detour, T& original, const char* what) {
    if (target == nullptr || MH_CreateHook(target, detour, (void**)&original) != MH_OK) {
        log_error("Wait probe: hooking {} failed", what);
        return false;
    }

    g_targets.push_back(target);
    return true;
}

bool install(ID3D12Device* device) {
    static int state = 0; // 0 not tried, 1 ok, -1 failed

    if (state != 0) {
        return state > 0;
    }

    state = -1;
    MH_Initialize();
    const auto kernelbase = GetModuleHandleW(L"KernelBase.dll");

    if (kernelbase == nullptr || device == nullptr) {
        return false;
    }

    ComPtr<ID3D12Fence> fence{};

    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        log_error("Wait probe: no fence");
        return false;
    }

    const auto vtable = *(void***)fence.Get();
    auto ok = hook(GetProcAddress(kernelbase, "WaitForSingleObject"), (void*)&wait_single_hook, g_wait_single, "WaitForSingleObject");
    ok = ok && hook(GetProcAddress(kernelbase, "WaitForSingleObjectEx"), (void*)&wait_single_ex_hook, g_wait_single_ex, "WaitForSingleObjectEx");
    ok = ok && hook(GetProcAddress(kernelbase, "WaitForMultipleObjectsEx"), (void*)&wait_multiple_ex_hook, g_wait_multiple_ex, "WaitForMultipleObjectsEx");
    ok = ok && hook(vtable[9], (void*)&set_event_on_completion_hook, g_set_event_on_completion, "ID3D12Fence::SetEventOnCompletion");

    if (!ok) {
        return false;
    }

    state = 1;
    return true;
}
} // namespace

namespace wait_probe {
void set_enabled(bool enabled, ID3D12Device* device) {
    if (enabled == g_enabled.load()) {
        return;
    }

    if (enabled && !install(device)) {
        return;
    }

    for (const auto target : g_targets) {
        enabled ? MH_QueueEnableHook(target) : MH_QueueDisableHook(target);
    }

    MH_ApplyQueued();
    g_enabled = enabled;

    // A fresh start for the numbers.
    for (auto& t : g_threads) {
        for (auto& k : t.ticks) k = 0;
    }

    g_frames = 0;
    log_info("Wait probe {}", enabled ? "ON" : "OFF");
}

void set_waitable(HANDLE waitable) {
    g_waitable = waitable;
}

void on_present_thread() {
    g_present_thread = GetCurrentThreadId();
}

void on_frame() {
    ++g_frames;
}

std::string status() {
    if (!g_enabled.load()) {
        return {};
    }

    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    const auto frames = std::max<uint32_t>(1, g_frames.exchange(0));
    const auto ms = [&](int64_t ticks) { return 1000.0 * (double)ticks / (double)f.QuadPart / frames; };

    struct Row {
        DWORD id;
        double kinds[KINDS];
        double total;
    };

    std::vector<Row> rows{};
    const auto count = std::min(g_thread_count.load(), MAX_THREADS);

    for (int i = 0; i < count; ++i) {
        Row row{g_threads[i].id.load(), {}, 0.0};

        for (int k = 0; k < KINDS; ++k) {
            row.kinds[k] = ms(g_threads[i].ticks[k].exchange(0));
            row.total += row.kinds[k];
        }

        if (row.total > 0.05 || row.id == g_present_thread.load()) {
            rows.push_back(row);
        }
    }

    std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
        const auto ap = a.id == g_present_thread.load();
        const auto bp = b.id == g_present_thread.load();
        return ap != bp ? ap : a.total > b.total;
    });

    std::string text = std::format("waits per frame (ms; latency waitable / GPU fences / other){}:", g_waitable.load() != nullptr ? "" : " [waitable unknown]");

    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
        const auto& r = rows[i];
        text += std::format(" | {}{} {:.2f}/{:.2f}/{:.2f}", r.id == g_present_thread.load() ? "render " : "", r.id, r.kinds[WAITABLE], r.kinds[FENCE],
            r.kinds[OTHER]);
    }

    return text;
}
} // namespace wait_probe
