#include <windows.h>

#include <format>
#include <mutex>
#include <unordered_map>

#include <MinHook.h>

#include "log.hpp"

#include "descriptor_map.hpp"

namespace {
struct Entry {
    ID3D12Resource* resource{};
    UINT64 width{};
    UINT height{};
    DXGI_FORMAT resource_format{};
    DXGI_FORMAT view_format{};
    bool uav{};
    std::string name{};
};

// ID3D12Device vtable slots.
constexpr size_t CREATE_SRV = 18;
constexpr size_t CREATE_UAV = 19;
constexpr size_t COPY_DESCRIPTORS = 23;
constexpr size_t COPY_DESCRIPTORS_SIMPLE = 24;

using CreateSrv = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
using CreateUav = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*,
    D3D12_CPU_DESCRIPTOR_HANDLE);
using CopyDescriptors = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, UINT,
    const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
using CopyDescriptorsSimple = void(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
    D3D12_DESCRIPTOR_HEAP_TYPE);

CreateSrv g_create_srv{};
CreateUav g_create_uav{};
CopyDescriptors g_copy{};
CopyDescriptorsSimple g_copy_simple{};

std::mutex g_mutex{};
std::unordered_map<SIZE_T, Entry> g_map{};
UINT g_increment{};
bool g_installed{};

// WKPDID_D3DDebugObjectNameW (d3dcommon.h), defined here (no dxguid.lib).
constexpr GUID DEBUG_NAME_W = {0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};

std::string resource_name(ID3D12Resource* resource) {
    wchar_t name[128]{};
    UINT size = sizeof(name) - sizeof(wchar_t);

    if (FAILED(resource->GetPrivateData(DEBUG_NAME_W, &size, name))) {
        return {};
    }

    std::string out{};

    for (UINT i = 0; i < size / sizeof(wchar_t) && name[i] != 0; ++i) {
        out += name[i] < 128 ? (char)name[i] : '?';
    }

    return out;
}

void remember(SIZE_T handle, ID3D12Resource* resource, DXGI_FORMAT view_format, bool uav) {
    Entry e{};

    if (resource != nullptr) {
        const auto desc = resource->GetDesc();
        e.resource = resource;
        e.width = desc.Width;
        e.height = desc.Height;
        e.resource_format = desc.Format;
        e.name = resource_name(resource);
    }

    e.view_format = view_format;
    e.uav = uav;
    std::scoped_lock _{g_mutex};
    g_map[handle] = std::move(e);
}

void copy_range(SIZE_T dst, SIZE_T src, UINT count) {
    std::scoped_lock _{g_mutex};

    for (UINT i = 0; i < count; ++i) {
        const auto it = g_map.find(src + (SIZE_T)i * g_increment);

        if (it != g_map.end()) {
            g_map[dst + (SIZE_T)i * g_increment] = it->second;
        } else {
            g_map.erase(dst + (SIZE_T)i * g_increment);
        }
    }
}

void STDMETHODCALLTYPE create_srv_hook(ID3D12Device* device, ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
    D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    g_create_srv(device, resource, desc, handle);
    remember(handle.ptr, resource, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN, false);
}

void STDMETHODCALLTYPE create_uav_hook(ID3D12Device* device, ID3D12Resource* resource, ID3D12Resource* counter,
    const D3D12_UNORDERED_ACCESS_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
    g_create_uav(device, resource, counter, desc, handle);
    remember(handle.ptr, resource, desc != nullptr ? desc->Format : DXGI_FORMAT_UNKNOWN, true);
}

void STDMETHODCALLTYPE copy_simple_hook(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst, D3D12_CPU_DESCRIPTOR_HANDLE src,
    D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    g_copy_simple(device, count, dst, src, type);

    if (type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        copy_range(dst.ptr, src.ptr, count);
    }
}

void STDMETHODCALLTYPE copy_hook(ID3D12Device* device, UINT dst_count, const D3D12_CPU_DESCRIPTOR_HANDLE* dst_starts, const UINT* dst_sizes,
    UINT src_count, const D3D12_CPU_DESCRIPTOR_HANDLE* src_starts, const UINT* src_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
    g_copy(device, dst_count, dst_starts, dst_sizes, src_count, src_starts, src_sizes, type);

    if (type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
        return;
    }

    // Walk both range lists in step, descriptor by descriptor.
    UINT di = 0, si = 0, d_off = 0, s_off = 0;

    while (di < dst_count && si < src_count) {
        const auto d_size = dst_sizes != nullptr ? dst_sizes[di] : 1u;
        const auto s_size = src_sizes != nullptr ? src_sizes[si] : 1u;
        const auto n = std::min(d_size - d_off, s_size - s_off);
        copy_range(dst_starts[di].ptr + (SIZE_T)d_off * g_increment, src_starts[si].ptr + (SIZE_T)s_off * g_increment, n);
        d_off += n;
        s_off += n;

        if (d_off == d_size) {
            ++di;
            d_off = 0;
        }

        if (s_off == s_size) {
            ++si;
            s_off = 0;
        }
    }
}

template <typename T>
bool hook(void** vtable, size_t slot, void* detour, T& original, const char* name) {
    if (MH_CreateHook(vtable[slot], detour, (void**)&original) != MH_OK || MH_EnableHook(vtable[slot]) != MH_OK) {
        log_error("descriptor map: could not hook ID3D12Device::{}", name);
        return false;
    }

    return true;
}
} // namespace

namespace descriptor_map {
bool install(ID3D12Device* device) {
    if (g_installed || device == nullptr) {
        return g_installed;
    }

    if (const auto status = MH_Initialize(); status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        log_error("descriptor map: MinHook init failed ({})", (int)status);
        return false;
    }

    g_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const auto vtable = *(void***)device;
    g_installed = hook(vtable, CREATE_SRV, (void*)&create_srv_hook, g_create_srv, "CreateShaderResourceView") &&
        hook(vtable, CREATE_UAV, (void*)&create_uav_hook, g_create_uav, "CreateUnorderedAccessView") &&
        hook(vtable, COPY_DESCRIPTORS, (void*)&copy_hook, g_copy, "CopyDescriptors") &&
        hook(vtable, COPY_DESCRIPTORS_SIMPLE, (void*)&copy_simple_hook, g_copy_simple, "CopyDescriptorsSimple");
    log_info("descriptor map (probe_descriptors): {}", g_installed ? "hooked (views created from now on are known; change the DLSS mode once "
                                                                     "to have the render targets' views made again)"
                                                                   : "failed");
    return g_installed;
}

bool installed() {
    return g_installed;
}

std::string describe(SIZE_T cpu_handle) {
    std::scoped_lock _{g_mutex};
    const auto it = g_map.find(cpu_handle);

    if (it == g_map.end()) {
        return "";
    }

    const auto& e = it->second;

    if (e.resource == nullptr) {
        return std::format("null {}", e.uav ? "UAV" : "SRV");
    }

    return std::format("{:x} {}x{} fmt {} (view {}) {}{}", (uintptr_t)e.resource, e.width, e.height, (int)e.resource_format, (int)e.view_format,
        e.uav ? "UAV" : "SRV", e.name.empty() ? "" : " '" + e.name + "'");
}
} // namespace descriptor_map
