#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <mutex>
#include <string>
#include <unordered_map>

#include "log.hpp"
#include "refw.hpp"

#include "render_objects.hpp"

namespace {
bool readable(const void* p, size_t size) {
    return p != nullptr && (uintptr_t)p > 0x10000 && !IsBadReadPtr(p, size);
}

HMODULE module_of(const void* address) {
    HMODULE module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)address, &module);
    return module;
}

std::mutex g_cache_mutex{};
std::unordered_map<std::string, size_t> g_member_offsets{}; // "container type/member type" -> offset

struct ContainerPath {
    size_t to_container{};
    size_t to_native{};
    bool known{};
};
ContainerPath g_texture_path{}; // Texture -> its D3D12 resource container -> the ID3D12Resource
size_t g_rtv_array_offset{};    // TargetState -> its array of render target views
} // namespace

namespace render_objects {
std::string type_name(const void* object) {
    if (!readable(object, sizeof(void*))) {
        return "";
    }

    const auto vtable = *(uint8_t* const* const*)object;

    if (!readable(vtable, 4 * sizeof(void*)) || module_of(vtable) == nullptr) {
        return "";
    }

    // Each render object's get-type virtual is "mov rax, [rip+x]; ret": read what it would return instead of calling it.
    const auto fn = vtable[refw::tdb_version() <= 67 ? 2 : 3];

    if (!readable(fn, 7) || module_of(fn) == nullptr || fn[0] != 0x48 || fn[1] != 0x8B || fn[2] != 0x05) {
        return "";
    }

    const auto slot = fn + 7 + *(const int32_t*)(fn + 3);

    if (!readable(slot, sizeof(void*))) {
        return "";
    }

    const auto info = *(void* const*)slot;

    if (!readable(info, 0x20)) {
        return "";
    }

    const auto name = refw::sdk()->type_info->get_name((REFrameworkTypeInfoHandle)info);
    return name != nullptr && readable(name, 1) ? std::string{name} : std::string{};
}

void* member_of_type(const void* object, const char* object_type, const char* type, size_t range) {
    if (!readable(object, range)) {
        return nullptr;
    }

    const auto key = std::string{object_type} + "/" + type;

    {
        std::scoped_lock _{g_cache_mutex};

        if (const auto it = g_member_offsets.find(key); it != g_member_offsets.end()) {
            return *(void* const*)((const uint8_t*)object + it->second);
        }
    }

    for (size_t offset = sizeof(void*); offset < range; offset += sizeof(void*)) {
        const auto member = *(void* const*)((const uint8_t*)object + offset);

        if (type_name(member) == type) {
            std::scoped_lock _{g_cache_mutex};
            g_member_offsets[key] = offset;
            log_info("{} -> {} at +{:x}", object_type, type, offset);
            return member;
        }
    }

    return nullptr;
}

bool is_d3d_object(const void* object) {
    if (!readable(object, sizeof(void*))) {
        return false;
    }

    const auto vtable = *(void* const*)object;
    const auto module = readable(vtable, sizeof(void*)) ? module_of(vtable) : nullptr;

    if (module == nullptr) {
        return false;
    }

    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    std::wstring name{path};
    name = name.substr(name.find_last_of(L"\\/") + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return (wchar_t)std::towlower(c); });
    return name.starts_with(L"d3d12");
}

ID3D12Resource* d3d12_resource(const void* object) {
    const auto type = type_name(object);

    if (type == "via.render.TargetState") {
        // TargetState -> array of render target views -> the first one -> its texture.
        if (g_rtv_array_offset == 0) {
            for (size_t offset = sizeof(void*); offset < 0x80 && g_rtv_array_offset == 0; offset += sizeof(void*)) {
                const auto array = *(void* const* const*)((const uint8_t*)object + offset);

                if (readable(array, sizeof(void*)) && type_name(*array) == "via.render.RenderTargetView") {
                    g_rtv_array_offset = offset;
                    log_info("via.render.TargetState -> render target views at +{:x}", offset);
                }
            }

            if (g_rtv_array_offset == 0) {
                return nullptr;
            }
        }

        const auto array = *(void* const* const*)((const uint8_t*)object + g_rtv_array_offset);
        const auto rtv = readable(array, sizeof(void*)) ? *array : nullptr;
        const auto texture = rtv != nullptr ? member_of_type(rtv, "via.render.RenderTargetView", "via.render.Texture") : nullptr;
        return texture != nullptr ? d3d12_resource(texture) : nullptr;
    }

    if (type == "via.render.ShaderResourceView") {
        const auto texture = member_of_type(object, "via.render.ShaderResourceView", "via.render.Texture");
        return texture != nullptr ? d3d12_resource(texture) : nullptr;
    }

    if (type != "via.render.Texture") {
        return nullptr;
    }

    // Texture -> resource container -> ID3D12Resource: found once by looking for a pointer to something that holds a
    // native D3D12 object near its start.
    if (!g_texture_path.known) {
        for (size_t offset = sizeof(void*); offset < 0x200 && !g_texture_path.known; offset += sizeof(void*)) {
            const auto container = *(const uint8_t* const*)((const uint8_t*)object + offset);

            if (!readable(container, 0x40)) {
                continue;
            }

            for (size_t inner = sizeof(void*); inner < 0x40; inner += sizeof(void*)) {
                const auto native = *(IUnknown* const*)(container + inner);
                ID3D12Resource* resource{};

                // A Direct3D object, and specifically a resource (QueryInterface is safe on any COM object).
                if (is_d3d_object(native) && SUCCEEDED(native->QueryInterface(IID_PPV_ARGS(&resource)))) {
                    resource->Release();
                    g_texture_path = {offset, inner, true};
                    log_info("via.render.Texture -> D3D12 resource container at +{:x}, native resource at +{:x}", offset, inner);
                    break;
                }
            }
        }

        if (!g_texture_path.known) {
            return nullptr;
        }
    }

    const auto container = *(const uint8_t* const*)((const uint8_t*)object + g_texture_path.to_container);

    if (!readable(container, g_texture_path.to_native + sizeof(void*))) {
        return nullptr;
    }

    const auto native = *(ID3D12Resource* const*)(container + g_texture_path.to_native);
    return is_d3d_object(native) ? native : nullptr;
}
} // namespace render_objects

namespace render_objects {
void dump(const char* label, const void* object, size_t range) {
    log_info("dump {}: {:x} type '{}'", label, (uintptr_t)object, type_name(object));

    if (!readable(object, range)) {
        return;
    }

    for (size_t offset = sizeof(void*); offset < range; offset += sizeof(void*)) {
        const auto member = *(void* const*)((const uint8_t*)object + offset);
        const auto name = type_name(member);

        if (!name.empty() || is_d3d_object(member)) {
            log_info("  +{:x}: {:x} '{}'{}", offset, (uintptr_t)member, name, is_d3d_object(member) ? " (d3d)" : "");
        }
    }
}
} // namespace render_objects
