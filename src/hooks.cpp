#include <cstring>

#include "hooks.hpp"

namespace {
void write_pointer(void** slot, void* value) {
    DWORD old{};
    VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    *slot = value;
    VirtualProtect(slot, sizeof(void*), old, &old);
}
} // namespace

PointerHook::PointerHook(void** slot, void* replacement)
    : m_slot{slot}, m_original{*slot}
{
    write_pointer(m_slot, replacement);
}

PointerHook::~PointerHook() {
    // Only undo it if nobody hooked the same slot after us.
    if (m_slot != nullptr) {
        write_pointer(m_slot, m_original);
    }
}

InstanceVtableHook::InstanceVtableHook(void* object, size_t entries)
    : m_object{object}, m_original_vtable{*(void***)object}
{
    m_vtable.assign(m_original_vtable, m_original_vtable + entries);
    *(void***)object = m_vtable.data();
}

void InstanceVtableHook::hook(size_t index, void* replacement) {
    m_vtable[index] = replacement;
}

void** find_import_slot(HMODULE module, const char* dll, const char* function) {
    const auto base = (uint8_t*)module;
    const auto dos = (IMAGE_DOS_HEADER*)base;
    const auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];

    if (dir.VirtualAddress == 0) {
        return nullptr;
    }

    for (auto desc = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); desc->Name != 0; ++desc) {
        if (_stricmp((const char*)(base + desc->Name), dll) != 0 || desc->OriginalFirstThunk == 0) {
            continue;
        }

        auto names = (IMAGE_THUNK_DATA*)(base + desc->OriginalFirstThunk);
        auto slots = (void**)(base + desc->FirstThunk);

        for (; names->u1.AddressOfData != 0; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
                continue;
            }

            const auto by_name = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);

            if (std::strcmp((const char*)by_name->Name, function) == 0) {
                return slots;
            }
        }
    }

    return nullptr;
}
