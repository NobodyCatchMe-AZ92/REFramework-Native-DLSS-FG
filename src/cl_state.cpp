#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <format>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include <MinHook.h>

#include "log.hpp"

#include "cl_state.hpp"
#include "descriptor_map.hpp"

using Microsoft::WRL::ComPtr;

namespace {
// ID3D12GraphicsCommandList vtable slots (fixed by the COM interface).
enum Slot : size_t {
    RESET = 10,
    CLEAR_STATE = 11,
    SET_PIPELINE_STATE = 25,
    SET_DESCRIPTOR_HEAPS = 28,
    SET_COMPUTE_ROOT_SIGNATURE = 29,
    SET_GRAPHICS_ROOT_SIGNATURE = 30,
    SET_COMPUTE_ROOT_DESCRIPTOR_TABLE = 31,
    SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE = 32,
    SET_COMPUTE_ROOT_32BIT_CONSTANT = 33,
    SET_COMPUTE_ROOT_32BIT_CONSTANTS = 35,
    SET_COMPUTE_ROOT_CBV = 37,
    SET_COMPUTE_ROOT_SRV = 39,
    SET_COMPUTE_ROOT_UAV = 41,
    DRAW_INSTANCED = 12,
    DRAW_INDEXED_INSTANCED = 13,
    DISPATCH = 14,
    COPY_TEXTURE_REGION = 16,
    COPY_RESOURCE = 17,
    RESOURCE_BARRIER = 26,
    OM_SET_RENDER_TARGETS = 46,
    CLEAR_RENDER_TARGET_VIEW = 48,
};

constexpr Slot ALL_SLOTS[] = {RESET, CLEAR_STATE, SET_PIPELINE_STATE, SET_DESCRIPTOR_HEAPS, SET_COMPUTE_ROOT_SIGNATURE, SET_GRAPHICS_ROOT_SIGNATURE,
    SET_COMPUTE_ROOT_DESCRIPTOR_TABLE, SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE, SET_COMPUTE_ROOT_32BIT_CONSTANT, SET_COMPUTE_ROOT_32BIT_CONSTANTS,
    SET_COMPUTE_ROOT_CBV, SET_COMPUTE_ROOT_SRV, SET_COMPUTE_ROOT_UAV, CLEAR_RENDER_TARGET_VIEW, DRAW_INSTANCED, DRAW_INDEXED_INSTANCED, DISPATCH,
    COPY_TEXTURE_REGION, COPY_RESOURCE, RESOURCE_BARRIER, OM_SET_RENDER_TARGETS};

void** g_vtable{}; // the command list vtable the hooks were made from

constexpr uint32_t MAX_PARAMS = 64;    // root parameters per root signature (the D3D12 maximum)
constexpr uint32_t MAX_CONSTANTS = 64; // 32-bit constants per root parameter

struct ComputeArg {
    enum Kind : uint8_t { NONE, TABLE, CONSTANTS, CBV, SRV, UAV } kind{};
    uint64_t value{};       // GPU descriptor handle or GPU virtual address
    uint32_t constants_set{}; // highest constant index written + 1
    uint32_t constants[MAX_CONSTANTS]{};
};

struct ListState {
    ID3D12PipelineState* pso{};
    ID3D12DescriptorHeap* heaps[2]{};
    UINT heap_count{};
    ID3D12RootSignature* compute_root{};
    ComputeArg compute[MAX_PARAMS]{};
    uint64_t graphics_tables[MAX_PARAMS]{};
    uint64_t graphics_tables_set{}; // bit per parameter
    const char* overflow{}; // why the state is not fully known (can't restore), nullptr if it is
    uint32_t epoch{};        // g_epoch at the list's last Reset: older = recorded partly while the hooks were off

    void reset(ID3D12PipelineState* initial);
    void reset_fields(ID3D12PipelineState* initial) {
        pso = initial;
        heap_count = 0;
        compute_root = nullptr;
        graphics_tables_set = 0;
        overflow = nullptr;

        for (auto& arg : compute) {
            arg.kind = ComputeArg::NONE;
            arg.constants_set = 0;
        }
    }
};

// One state per list, shared by all threads: a list is recorded by one thread at a time (between Reset and Close), but
// the engine resets lists on one thread and records them on another. States are never freed (lists are reused), so each
// thread caches the last one it used: nearly every call is for the same list as the one before.
// Diagnostics (setting perf_bypass): the hooks can be switched off live; each switch back on starts a new epoch, and only
// lists reset since then have a known state.
std::atomic<uint32_t> g_epoch{1};

void ListState::reset(ID3D12PipelineState* initial) {
    reset_fields(initial);
    epoch = g_epoch.load(std::memory_order_relaxed);
}

std::shared_mutex g_lists_mutex{};
std::unordered_map<ID3D12GraphicsCommandList*, std::unique_ptr<ListState>> g_lists{};

struct ThreadStates {
    ID3D12GraphicsCommandList* last_list{};
    ListState* last{};

    ListState& get(ID3D12GraphicsCommandList* list) {
        if (list == last_list) {
            return *last;
        }

        ListState* state{};
        {
            std::shared_lock _{g_lists_mutex};

            if (const auto it = g_lists.find(list); it != g_lists.end()) {
                state = it->second.get();
            }
        }

        if (state == nullptr) {
            std::unique_lock _{g_lists_mutex};
            auto& entry = g_lists[list];

            if (entry == nullptr) {
                entry = std::make_unique<ListState>();
                entry->overflow = "recorded before its first Reset since tracking began";
            }

            state = entry.get();
        }

        last_list = list;
        last = state;
        return *last;
    }
};

thread_local ThreadStates t_states{};

template <typename Fn>
struct Hook {
    Fn original{};
};

Hook<HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*)> g_reset{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)> g_clear_state{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*)> g_set_pso{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*)> g_set_heaps{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)> g_set_compute_root{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*)> g_set_graphics_root{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)> g_compute_table{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE)> g_graphics_table{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)> g_compute_constant{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT)> g_compute_constants{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)> g_compute_cbv{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)> g_compute_srv{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS)> g_compute_uav{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*)> g_clear_rtv{};

std::atomic<cl_state::ClearHandler> g_clear_handler{};

// Armed after the DLSS marker (arm()): the last resource made a render target on that list, and the bind of the scene
// target that starts the engine's upscale, whose draws are then skipped if the handler replaced them.
struct Armed {
    ID3D12GraphicsCommandList* list{};
    uint64_t rtv{};
    ID3D12Resource* last_render_target{};
    D3D12_RESOURCE_STATES last_render_target_state{}; // its state now (transitions after it became a render target too)
    cl_state::UpscaleHandler handler{};
    uint32_t context{};
    enum Phase { WAITING, SKIPPING, AFTER, AFTER_DRAWS } phase{};
    uint32_t skip_left{}; // draws still to skip (the engine's upscale) or, AFTER_DRAWS, to run before the handler
    ID3D12Resource* watch{}; // watch_render_target()
    uint32_t compute_w{};    // watch_compute_upscale(): the output size, the texture seen, the handler
    uint32_t compute_h{};
    ID3D12Resource* compute_target{};
    ID3D12Resource* compute_source{};
    bool compute_source_read{}; // the source left the render target state (since arm(), in whatever order) and hasn't gone back
    ID3D12Resource* compute_input{}; // the last texture like the source (size, format) made readable since arm(): the pass's input
    D3D12_RESOURCE_STATES compute_input_state{};
    cl_state::ComputeHandler compute_handler{};
    cl_state::WatchHandler watch_handler{};
    uint32_t watch_context{};
    // watch_bloom_chain(): the scene's first half-size render target (the bloom chain's first draw), the scene's tracked state.
    ID3D12Resource* chain_source{};
    D3D12_RESOURCE_STATES chain_source_state{};
    cl_state::WatchHandler chain_handler{};
    uint32_t chain_context{};
};

thread_local Armed t_armed{};
thread_local D3D12_RESOURCE_STATES t_bind_source_state{D3D12_RESOURCE_STATE_RENDER_TARGET}; // inside the upscale handler
thread_local ID3D12Resource* t_last_compute_input{}; // the compute upscale's input, for its handler
thread_local D3D12_RESOURCE_STATES t_last_compute_input_state{};

// AFTER_DRAWS: one of the engine's draws ran; after the last one counted, the handler's second call.
void after_engine_draw(ID3D12GraphicsCommandList* list) {
    if (t_armed.phase != Armed::AFTER_DRAWS || list != t_armed.list || t_armed.skip_left == 0 || --t_armed.skip_left > 0) {
        return;
    }

    const auto armed = t_armed;
    t_armed = {};
    armed.handler(list, armed.last_render_target, armed.context, true);
}

// Diagnostics: the next calls recorded on one list after a point (trace()).
thread_local ID3D12GraphicsCommandList* t_trace_list{};
thread_local int t_trace_left{};

bool tracing(ID3D12GraphicsCommandList* list) {
    return t_trace_left > 0 && (list == t_trace_list || t_trace_list == nullptr) && t_trace_left-- > 0; // nullptr: any list on this thread
}

Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT)> g_draw{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT)> g_draw_indexed{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT)> g_dispatch{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*)> g_copy_region{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*)> g_copy_resource{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*)> g_barrier{};
Hook<void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*)> g_set_rts{};

void STDMETHODCALLTYPE draw_hook(ID3D12GraphicsCommandList* list, UINT v, UINT n, UINT first_v, UINT first_i) {
    if (t_armed.phase == Armed::SKIPPING && list == t_armed.list && t_armed.skip_left > 0) {
        --t_armed.skip_left;
        return;
    }

    if (tracing(list)) log_info("trace: Draw {} vertices x {}", v, n);
    g_draw.original(list, v, n, first_v, first_i);
    after_engine_draw(list);
}

void STDMETHODCALLTYPE draw_indexed_hook(ID3D12GraphicsCommandList* list, UINT i, UINT n, UINT first, INT base, UINT first_i) {
    if (t_armed.phase == Armed::SKIPPING && list == t_armed.list && t_armed.skip_left > 0) {
        --t_armed.skip_left;
        return;
    }

    if (tracing(list)) log_info("trace: DrawIndexed {} indices x {}", i, n);
    g_draw_indexed.original(list, i, n, first, base, first_i);
    after_engine_draw(list);
}

// Diagnostics (probe_descriptors): the textures behind the compute tables bound for a dispatch (RE4's upscale pass), from
// the descriptor map. 16 descriptors from each table's start (the table's own size isn't known here).
void dump_compute_bindings(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
    static std::atomic<int> dumps{};

    if (!descriptor_map::installed() || dumps.fetch_add(1) % 300 != 150) {
        return;
    }

    auto& state = t_states.get(list);
    ID3D12DescriptorHeap* heap{};

    for (UINT i = 0; i < state.heap_count; ++i) {
        if (state.heaps[i] != nullptr && state.heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            heap = state.heaps[i];
        }
    }

    log_info("descriptors: compute upscale dispatch {}x{}x{}, pso {:x}, root {:x}, heap {:x}", x, y, z, (uintptr_t)state.pso,
        (uintptr_t)state.compute_root, (uintptr_t)heap);

    if (heap == nullptr) {
        return;
    }

    const auto desc = heap->GetDesc();
    const auto gpu_start = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const auto cpu_start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    list->GetDevice(IID_PPV_ARGS(&device));
    const auto increment = device != nullptr ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 32u;

    for (uint32_t p = 0; p < MAX_PARAMS; ++p) {
        const auto& arg = state.compute[p];

        if (arg.kind == ComputeArg::TABLE) {
            if (arg.value < gpu_start || arg.value >= gpu_start + (uint64_t)desc.NumDescriptors * increment) {
                log_info("descriptors:   param {} table {:x} (not in the bound heap)", p, arg.value);
                continue;
            }

            const auto cpu = cpu_start + (SIZE_T)(arg.value - gpu_start);
            log_info("descriptors:   param {} table (heap slot {}):", p, (arg.value - gpu_start) / increment);

            for (UINT i = 0; i < 16; ++i) {
                const auto text = descriptor_map::describe(cpu + (SIZE_T)i * increment);
                log_info("descriptors:     [{}] {}", i, text.empty() ? "-" : text);
            }
        } else if (arg.kind == ComputeArg::CONSTANTS) {
            std::string values{};

            for (uint32_t c = 0; c < arg.constants_set && c < 24; ++c) {
                values += std::format(" {:.4g}", std::bit_cast<float>(arg.constants[c]));
            }

            log_info("descriptors:   param {} constants ({}):{}", p, arg.constants_set, values);
        } else if (arg.kind != ComputeArg::NONE) {
            log_info("descriptors:   param {} root view kind {} at {:x}", p, (int)arg.kind, arg.value);
        }
    }
}

void STDMETHODCALLTYPE dispatch_hook(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z) {
    if (tracing(list)) log_info("trace: Dispatch {}x{}x{}", x, y, z);
    g_dispatch.original(list, x, y, z);

    // The engine's compute upscale wrote its output texture: the handler, once (its own calls aren't the engine's).
    // A dispatch with an output-sized UAV but no source transition is another pass: forget its texture.
    if (list == t_armed.list && t_armed.phase == Armed::WAITING && t_armed.compute_target != nullptr && !t_armed.compute_source_read) {
        t_armed.compute_target = nullptr;
    }

    if (list == t_armed.list && t_armed.phase == Armed::WAITING && t_armed.compute_target != nullptr && t_armed.compute_handler != nullptr) {
        dump_compute_bindings(list, x, y, z);
        const auto target = t_armed.compute_target;
        const auto handler = t_armed.compute_handler;
        t_armed.compute_target = nullptr;
        t_armed.compute_handler = nullptr;
        t_armed.list = nullptr;
        t_last_compute_input = t_armed.compute_input;
        t_last_compute_input_state = t_armed.compute_input_state;
        handler(list, target, t_armed.context);
        t_last_compute_input = nullptr;
        t_armed.list = list;
    }
}

void STDMETHODCALLTYPE copy_region_hook(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
    const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
    if (tracing(list)) log_info("trace: CopyTextureRegion {:x} <- {:x}", (uintptr_t)dst->pResource, (uintptr_t)src->pResource);
    g_copy_region.original(list, dst, x, y, z, src, box);
}

void STDMETHODCALLTYPE copy_resource_hook(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src) {
    if (tracing(list)) log_info("trace: CopyResource {:x} <- {:x}", (uintptr_t)dst, (uintptr_t)src);
    g_copy_resource.original(list, dst, src);
}

void STDMETHODCALLTYPE barrier_hook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
    if (tracing(list)) {
        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];

            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                const auto desc = b.Transition.pResource->GetDesc();
                log_info("trace:   barrier {:x} ({}x{} fmt {}) {:x} -> {:x}", (uintptr_t)b.Transition.pResource, desc.Width, desc.Height, (int)desc.Format,
                    (int)b.Transition.StateBefore, (int)b.Transition.StateAfter);
            } else {
                log_info("trace:   barrier type {} {:x}", (int)b.Type, (uintptr_t)b.UAV.pResource);
            }
        }
    }

    // watch_bloom_chain(): a texture of half the scene's size (+-1) becoming a render target starts the bloom chain. The
    // handler gets the scene's state before this batch (its own transition in the batch, else the tracked one).
    if (list == t_armed.list && t_armed.phase == Armed::WAITING && t_armed.chain_handler != nullptr && t_armed.chain_source != nullptr) {
        const auto scene = t_armed.chain_source->GetDesc();
        bool start = false;
        auto state = t_armed.chain_source_state;

        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];

            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
                continue;
            }

            if (b.Transition.pResource == t_armed.chain_source) {
                state = b.Transition.StateBefore;
            } else if (b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                const auto d = b.Transition.pResource->GetDesc();
                const auto hw = (int64_t)(scene.Width / 2);
                const auto hh = (int64_t)(scene.Height / 2);

                if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && std::llabs((int64_t)d.Width - hw) <= 1 && std::llabs((int64_t)d.Height - hh) <= 1 &&
                    d.Format == scene.Format)
                {
                    start = true;
                }
            }
        }

        if (start) {
            const auto handler = t_armed.chain_handler;
            const auto source = t_armed.chain_source;
            const auto context = t_armed.chain_context;
            t_armed.chain_handler = nullptr; // its own barriers pass through here too
            handler(list, source, state, context);
        } else {
            for (UINT i = 0; i < count; ++i) {
                const auto& b = barriers[i];

                if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == t_armed.chain_source) {
                    t_armed.chain_source_state = b.Transition.StateAfter;
                }
            }
        }
    }

    if (list == t_armed.list && t_armed.phase == Armed::WAITING && t_armed.watch != nullptr) {
        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];

            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == t_armed.watch &&
                b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                const auto handler = t_armed.watch_handler;
                const auto context = t_armed.watch_context;
                t_armed.watch = nullptr; // the handler's own barriers pass through here too
                handler(list, b.Transition.pResource, b.Transition.StateBefore, context);
                break;
            }
        }
    }

    if (list == t_armed.list && t_armed.phase == Armed::WAITING && t_armed.compute_handler != nullptr) {
        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];

            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.StateAfter == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                const auto desc = b.Transition.pResource->GetDesc();

                if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == t_armed.compute_w && desc.Height == t_armed.compute_h) {
                    t_armed.compute_target = b.Transition.pResource;
                }
            }

            // The pass's own input: RE4 upscales the scene's second color buffer (its post effects' result, depth of
            // field in it), not HDRTarget. The last texture like the source made readable before the dispatch.
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && t_armed.compute_source != nullptr &&
                (b.Transition.StateAfter & (D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) != 0)
            {
                const auto desc = b.Transition.pResource->GetDesc();
                const auto source = t_armed.compute_source->GetDesc();

                if (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && desc.Width == source.Width && desc.Height == source.Height &&
                    desc.Format == source.Format)
                {
                    t_armed.compute_input = b.Transition.pResource;
                    t_armed.compute_input_state = b.Transition.StateAfter;
                }
            }

            // Not tied to the order of the target's barrier: in some areas RE4 copies the HDR color first (render target ->
            // copy source -> shader resource), long before the upscale's output becomes a UAV.
            if (b.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && b.Transition.pResource == t_armed.compute_source) {
                if (b.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    t_armed.compute_source_read = true;
                } else if (b.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                    t_armed.compute_source_read = false;
                }
            }
        }
    }

    if (list == t_armed.list && t_armed.phase == Armed::WAITING) {
        for (UINT i = 0; i < count; ++i) {
            if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barriers[i].Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET) {
                t_armed.last_render_target = barriers[i].Transition.pResource;
                t_armed.last_render_target_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
            } else if (barriers[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION && barriers[i].Transition.pResource == t_armed.last_render_target) {
                t_armed.last_render_target_state = barriers[i].Transition.StateAfter;
            }
        }
    }

    g_barrier.original(list, count, barriers);
}

void STDMETHODCALLTYPE set_rts_hook(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rts, BOOL single,
    const D3D12_CPU_DESCRIPTOR_HANDLE* depth)
{
    if (tracing(list)) log_info("trace: OMSetRenderTargets {} first rtv {:x} depth {:x}", count, count > 0 && rts != nullptr ? rts[0].ptr : 0, depth != nullptr ? depth->ptr : 0);
    g_set_rts.original(list, count, rts, single, depth);

    if (list != t_armed.list) {
        return;
    }

    // The next bind after the engine's upscale: done (or, for AFTER, the handler's second call).
    if (t_armed.phase != Armed::WAITING) {
        const auto armed = t_armed;
        t_armed = {};

        if (armed.phase == Armed::AFTER) {
            armed.handler(list, armed.last_render_target, armed.context, true);
        }

        return;
    }

    if (count >= 1 && rts != nullptr && rts[0].ptr == t_armed.rtv) {
        auto armed = t_armed;
        t_armed.list = nullptr; // the handler's own calls are not the engine's
        t_bind_source_state = armed.last_render_target_state;
        const auto action = armed.handler(list, armed.last_render_target, armed.context, false);
        t_bind_source_state = D3D12_RESOURCE_STATE_RENDER_TARGET;

        if (action != cl_state::KEEP) {
            armed.phase = action == cl_state::REPLACE ? Armed::SKIPPING : action == cl_state::AFTER_DRAWS ? Armed::AFTER_DRAWS : Armed::AFTER;
            t_armed = armed;
        }
    }
}

std::atomic<bool> g_installed{};

HRESULT STDMETHODCALLTYPE reset_hook(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso) {
    if (list == t_armed.list) {
        t_armed = {};
    }

    const auto result = g_reset.original(list, allocator, pso);
    t_states.get(list).reset(pso);
    return result;
}

void STDMETHODCALLTYPE clear_state_hook(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
    g_clear_state.original(list, pso);
    t_states.get(list).reset(pso);
}

void STDMETHODCALLTYPE set_pso_hook(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso) {
    g_set_pso.original(list, pso);
    t_states.get(list).pso = pso;
}

void STDMETHODCALLTYPE set_heaps_hook(ID3D12GraphicsCommandList* list, UINT count, ID3D12DescriptorHeap* const* heaps) {
    g_set_heaps.original(list, count, heaps);
    auto& state = t_states.get(list);

    if (count > 2) {
        state.overflow = "more than 2 descriptor heaps";
        return;
    }

    state.heap_count = count;

    for (UINT i = 0; i < count; ++i) {
        state.heaps[i] = heaps[i];
    }
}

void STDMETHODCALLTYPE set_compute_root_hook(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) {
    g_set_compute_root.original(list, root);
    auto& state = t_states.get(list);

    // Changing the root signature drops the arguments; setting the same one again keeps them.
    if (root != state.compute_root) {
        for (auto& arg : state.compute) {
            arg.kind = ComputeArg::NONE;
            arg.constants_set = 0;
        }
    }

    state.compute_root = root;
}

void STDMETHODCALLTYPE set_graphics_root_hook(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root) {
    g_set_graphics_root.original(list, root);
    t_states.get(list).graphics_tables_set = 0;
}

ComputeArg* compute_arg(ID3D12GraphicsCommandList* list, UINT index) {
    auto& state = t_states.get(list);

    if (index >= MAX_PARAMS) {
        state.overflow = "compute root parameter index over 63";
        return nullptr;
    }

    return &state.compute[index];
}

void STDMETHODCALLTYPE compute_table_hook(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    g_compute_table.original(list, index, handle);

    if (const auto arg = compute_arg(list, index)) {
        arg->kind = ComputeArg::TABLE;
        arg->value = handle.ptr;
    }
}

void STDMETHODCALLTYPE graphics_table_hook(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    g_graphics_table.original(list, index, handle);
    auto& state = t_states.get(list);

    if (index >= MAX_PARAMS) {
        state.overflow = "graphics root parameter index over 63";
        return;
    }

    state.graphics_tables[index] = handle.ptr;
    state.graphics_tables_set |= 1ull << index;
}

void store_constants(ID3D12GraphicsCommandList* list, UINT index, UINT count, const void* data, UINT offset) {
    const auto arg = compute_arg(list, index);

    if (arg == nullptr) {
        return;
    }

    if (offset + count > MAX_CONSTANTS) {
        t_states.get(list).overflow = "more than 64 root constants";
        return;
    }

    if (arg->kind != ComputeArg::CONSTANTS) {
        arg->kind = ComputeArg::CONSTANTS;
        arg->constants_set = 0;
    }

    std::memcpy(arg->constants + offset, data, count * sizeof(uint32_t));
    arg->constants_set = std::max(arg->constants_set, offset + count);
}

void STDMETHODCALLTYPE compute_constant_hook(ID3D12GraphicsCommandList* list, UINT index, UINT value, UINT offset) {
    g_compute_constant.original(list, index, value, offset);
    store_constants(list, index, 1, &value, offset);
}

void STDMETHODCALLTYPE compute_constants_hook(ID3D12GraphicsCommandList* list, UINT index, UINT count, const void* data, UINT offset) {
    g_compute_constants.original(list, index, count, data, offset);
    store_constants(list, index, count, data, offset);
}

template <ComputeArg::Kind kind, auto& hook>
void STDMETHODCALLTYPE compute_view_hook(ID3D12GraphicsCommandList* list, UINT index, D3D12_GPU_VIRTUAL_ADDRESS address) {
    hook.original(list, index, address);

    if (const auto arg = compute_arg(list, index)) {
        arg->kind = kind;
        arg->value = address;
    }
}

void STDMETHODCALLTYPE clear_rtv_hook(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE rtv, const FLOAT* color, UINT rects,
    const D3D12_RECT* rect)
{
    const auto handler = g_clear_handler.load();

    if (handler != nullptr && handler(list, rtv, color)) {
        return;
    }

    g_clear_rtv.original(list, rtv, color, rects, rect);
}

template <typename T>
bool hook_slot(void** vtable, size_t slot, void* detour, T& original) {
    const auto target = vtable[slot];
    const auto status = MH_CreateHook(target, detour, (void**)&original);

    // Two slots can point at the same function: hooked once is enough.
    if (status == MH_ERROR_ALREADY_CREATED) {
        log_error("Command list slot {} shares its function with another slot: not supported", slot);
        return false;
    }

    if (status != MH_OK || MH_EnableHook(target) != MH_OK) {
        log_error("Hooking command list slot {} ({:x}) failed: {}", slot, (uintptr_t)target, (int)status);
        return false;
    }

    return true;
}
} // namespace

namespace cl_state {
bool install(ID3D12Device* device) {
    if (g_installed) {
        return true;
    }

    ComPtr<ID3D12CommandAllocator> allocator{};
    ComPtr<ID3D12GraphicsCommandList> list{};

    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
    {
        log_error("Command list state: could not create a command list to find its functions");
        return false;
    }

    list->Close();

    if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_error("Command list state: MinHook failed to initialize: {}", (int)status);
        return false;
    }

    const auto vtable = *(void***)list.Get();
    g_vtable = vtable;
    bool ok = true;
    ok = ok && hook_slot(vtable, RESET, (void*)&reset_hook, g_reset.original);
    ok = ok && hook_slot(vtable, CLEAR_STATE, (void*)&clear_state_hook, g_clear_state.original);
    ok = ok && hook_slot(vtable, SET_PIPELINE_STATE, (void*)&set_pso_hook, g_set_pso.original);
    ok = ok && hook_slot(vtable, SET_DESCRIPTOR_HEAPS, (void*)&set_heaps_hook, g_set_heaps.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_SIGNATURE, (void*)&set_compute_root_hook, g_set_compute_root.original);
    ok = ok && hook_slot(vtable, SET_GRAPHICS_ROOT_SIGNATURE, (void*)&set_graphics_root_hook, g_set_graphics_root.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_DESCRIPTOR_TABLE, (void*)&compute_table_hook, g_compute_table.original);
    ok = ok && hook_slot(vtable, SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE, (void*)&graphics_table_hook, g_graphics_table.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_32BIT_CONSTANT, (void*)&compute_constant_hook, g_compute_constant.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_32BIT_CONSTANTS, (void*)&compute_constants_hook, g_compute_constants.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_CBV, (void*)&compute_view_hook<ComputeArg::CBV, g_compute_cbv>, g_compute_cbv.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_SRV, (void*)&compute_view_hook<ComputeArg::SRV, g_compute_srv>, g_compute_srv.original);
    ok = ok && hook_slot(vtable, SET_COMPUTE_ROOT_UAV, (void*)&compute_view_hook<ComputeArg::UAV, g_compute_uav>, g_compute_uav.original);
    ok = ok && hook_slot(vtable, CLEAR_RENDER_TARGET_VIEW, (void*)&clear_rtv_hook, g_clear_rtv.original);
    ok = ok && hook_slot(vtable, DRAW_INSTANCED, (void*)&draw_hook, g_draw.original);
    ok = ok && hook_slot(vtable, DRAW_INDEXED_INSTANCED, (void*)&draw_indexed_hook, g_draw_indexed.original);
    ok = ok && hook_slot(vtable, DISPATCH, (void*)&dispatch_hook, g_dispatch.original);
    ok = ok && hook_slot(vtable, COPY_TEXTURE_REGION, (void*)&copy_region_hook, g_copy_region.original);
    ok = ok && hook_slot(vtable, COPY_RESOURCE, (void*)&copy_resource_hook, g_copy_resource.original);
    ok = ok && hook_slot(vtable, RESOURCE_BARRIER, (void*)&barrier_hook, g_barrier.original);
    ok = ok && hook_slot(vtable, OM_SET_RENDER_TARGETS, (void*)&set_rts_hook, g_set_rts.original);

    if (!ok) {
        // Partially hooked lists would restore garbage: undo all.
        for (const auto slot : {RESET, CLEAR_STATE, SET_PIPELINE_STATE, SET_DESCRIPTOR_HEAPS, SET_COMPUTE_ROOT_SIGNATURE, SET_GRAPHICS_ROOT_SIGNATURE,
                 SET_COMPUTE_ROOT_DESCRIPTOR_TABLE, SET_GRAPHICS_ROOT_DESCRIPTOR_TABLE, SET_COMPUTE_ROOT_32BIT_CONSTANT,
                 SET_COMPUTE_ROOT_32BIT_CONSTANTS, SET_COMPUTE_ROOT_CBV, SET_COMPUTE_ROOT_SRV, SET_COMPUTE_ROOT_UAV, CLEAR_RENDER_TARGET_VIEW, DRAW_INSTANCED,
                 DRAW_INDEXED_INSTANCED, DISPATCH, COPY_TEXTURE_REGION, COPY_RESOURCE, RESOURCE_BARRIER, OM_SET_RENDER_TARGETS})
        {
            MH_DisableHook(vtable[slot]);
        }

        log_error("Command list state tracking unavailable: DLSS can't be recorded into the game's command lists");
        return false;
    }

    g_installed = true;
    log_info("Command list state tracking installed (21 command list functions hooked)");
    return true;
}

bool installed() {
    return g_installed;
}

void set_hooks_enabled(bool enabled) {
    static bool current = true;

    if (!g_installed || g_vtable == nullptr || enabled == current) {
        return;
    }

    if (enabled) {
        ++g_epoch; // states recorded while off are incomplete
    }

    for (const auto slot : ALL_SLOTS) {
        enabled ? MH_QueueEnableHook(g_vtable[slot]) : MH_QueueDisableHook(g_vtable[slot]);
    }

    const auto status = MH_ApplyQueued();
    current = enabled;
    log_info("Command list hooks {} (diagnostics: perf_bypass){}", enabled ? "ON" : "OFF", status == MH_OK ? "" : " - MinHook failed");
}

struct Saved {
    ListState state{};
};

void SavedDeleter::operator()(Saved* saved) const {
    delete saved;
}

SavedPtr save(ID3D12GraphicsCommandList* list) {
    const auto& state = t_states.get(list);

    if (state.epoch != g_epoch.load(std::memory_order_relaxed)) {
        return nullptr; // recorded (partly) while the hooks were off
    }

    if (state.overflow != nullptr) {
        static std::mutex logged_mutex{};
        static std::vector<const char*> logged{};
        std::scoped_lock _{logged_mutex};

        if (std::find(logged.begin(), logged.end(), state.overflow) == logged.end()) {
            logged.push_back(state.overflow);
            log_warn("Command list {:x} state not fully known: {}", (uintptr_t)list, state.overflow);
        }

        return nullptr;
    }

    return SavedPtr{new Saved{state}};
}

void restore(ID3D12GraphicsCommandList* list, const Saved& saved) {
    // On the thread recording the list, through the originals; then the tracked state is the saved one again.
    const auto& state = saved.state;
    t_states.get(list) = state;

    if (state.heap_count > 0) {
        g_set_heaps.original(list, state.heap_count, state.heaps);
    }

    if (state.pso != nullptr) {
        g_set_pso.original(list, state.pso);
    }

    if (state.compute_root != nullptr) {
        g_set_compute_root.original(list, state.compute_root);

        for (UINT i = 0; i < MAX_PARAMS; ++i) {
            const auto& arg = state.compute[i];

            switch (arg.kind) {
            case ComputeArg::TABLE: g_compute_table.original(list, i, D3D12_GPU_DESCRIPTOR_HANDLE{arg.value}); break;
            case ComputeArg::CONSTANTS: g_compute_constants.original(list, i, arg.constants_set, arg.constants, 0); break;
            case ComputeArg::CBV: g_compute_cbv.original(list, i, arg.value); break;
            case ComputeArg::SRV: g_compute_srv.original(list, i, arg.value); break;
            case ComputeArg::UAV: g_compute_uav.original(list, i, arg.value); break;
            default: break;
            }
        }
    }

    // New descriptor heaps invalidate the tables set from the old ones: set the graphics ones again too.
    for (UINT i = 0; i < MAX_PARAMS; ++i) {
        if ((state.graphics_tables_set & (1ull << i)) != 0) {
            g_graphics_table.original(list, i, D3D12_GPU_DESCRIPTOR_HANDLE{state.graphics_tables[i]});
        }
    }
}

void arm(ID3D12GraphicsCommandList* list, D3D12_CPU_DESCRIPTOR_HANDLE scene_target_rtv, UpscaleHandler handler, uint32_t context, uint32_t skip_draws) {
    t_armed = {};
    t_armed.skip_left = skip_draws;
    t_armed.list = list;
    t_armed.rtv = scene_target_rtv.ptr;
    t_armed.handler = handler;
    t_armed.context = context;
}

void trace(ID3D12GraphicsCommandList* list, int calls) {
    t_trace_list = list;
    t_trace_left = calls;
}

void watch_render_target(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, WatchHandler handler, uint32_t context) {
    if (list != t_armed.list) {
        return;
    }

    t_armed.watch = resource;
    t_armed.watch_handler = handler;
    t_armed.watch_context = context;
}

void watch_bloom_chain(ID3D12GraphicsCommandList* list, ID3D12Resource* source, D3D12_RESOURCE_STATES state, WatchHandler handler, uint32_t context) {
    if (list != t_armed.list) {
        return;
    }

    t_armed.chain_source = source;
    t_armed.chain_source_state = state;
    t_armed.chain_handler = handler;
    t_armed.chain_context = context;
}

void watch_compute_upscale(ID3D12GraphicsCommandList* list, uint32_t width, uint32_t height, ID3D12Resource* source, ComputeHandler handler) {
    if (list != t_armed.list) {
        return;
    }

    t_armed.compute_w = width;
    t_armed.compute_h = height;
    t_armed.compute_target = nullptr;
    t_armed.compute_source = source;
    t_armed.compute_source_read = false;
    t_armed.compute_input = nullptr;
    t_armed.compute_handler = handler;
}

void set_clear_handler(ClearHandler handler) {
    g_clear_handler = handler;
}
} // namespace cl_state

namespace cl_state {
ID3D12Resource* compute_upscale_input() {
    return t_last_compute_input;
}
} // namespace cl_state

namespace cl_state {
D3D12_RESOURCE_STATES compute_upscale_input_state() {
    return t_last_compute_input_state;
}
} // namespace cl_state

namespace cl_state {
// Diagnostics (probe_descriptors): the textures behind the graphics tables bound on `list` now (RE2's upscale draw, right
// after it), 16 descriptors from each table's start.
void dump_graphics_bindings(ID3D12GraphicsCommandList* list, const char* label) {
    if (!descriptor_map::installed()) {
        return;
    }

    auto& state = t_states.get(list);
    ID3D12DescriptorHeap* heap{};

    for (UINT i = 0; i < state.heap_count; ++i) {
        if (state.heaps[i] != nullptr && state.heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
            heap = state.heaps[i];
        }
    }

    log_info("descriptors: {} - graphics tables, pso {:x}, heap {:x}", label, (uintptr_t)state.pso, (uintptr_t)heap);

    if (heap == nullptr) {
        return;
    }

    const auto desc = heap->GetDesc();
    const auto gpu_start = heap->GetGPUDescriptorHandleForHeapStart().ptr;
    const auto cpu_start = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    Microsoft::WRL::ComPtr<ID3D12Device> device{};
    list->GetDevice(IID_PPV_ARGS(&device));
    const auto increment = device != nullptr ? device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) : 32u;

    for (uint32_t p = 0; p < MAX_PARAMS; ++p) {
        if ((state.graphics_tables_set & (1ull << p)) == 0) {
            continue;
        }

        const auto value = state.graphics_tables[p];

        if (value < gpu_start || value >= gpu_start + (uint64_t)desc.NumDescriptors * increment) {
            log_info("descriptors:   param {} table {:x} (not in the bound heap: a sampler table?)", p, value);
            continue;
        }

        const auto cpu = cpu_start + (SIZE_T)(value - gpu_start);
        log_info("descriptors:   param {} table (heap slot {}):", p, (value - gpu_start) / increment);

        for (UINT i = 0; i < 16; ++i) {
            const auto text = descriptor_map::describe(cpu + (SIZE_T)i * increment);

            if (!text.empty()) {
                log_info("descriptors:     [{}] {}", i, text);
            }
        }
    }
}
} // namespace cl_state

namespace cl_state {
ID3D12Resource* armed_last_render_target(ID3D12GraphicsCommandList* list) {
    return t_armed.list == list ? t_armed.last_render_target : nullptr;
}
} // namespace cl_state

namespace cl_state {
D3D12_RESOURCE_STATES armed_last_render_target_state(ID3D12GraphicsCommandList* list) {
    return t_armed.list == list ? t_armed.last_render_target_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
}

D3D12_RESOURCE_STATES upscale_source_state() {
    return t_bind_source_state;
}
} // namespace cl_state
