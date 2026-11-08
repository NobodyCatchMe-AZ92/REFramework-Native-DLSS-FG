#include <cstring>
#include <fstream>
#include <vector>

#include "log.hpp"

#include "unhook_guard.hpp"

namespace {
struct Image {
    uint8_t* base{};
    IMAGE_NT_HEADERS* nt{};
    size_t size{};
};

Image image_of(HMODULE module) {
    Image image{};
    image.base = (uint8_t*)module;
    image.nt = (IMAGE_NT_HEADERS*)(image.base + ((IMAGE_DOS_HEADER*)image.base)->e_lfanew);
    image.size = image.nt->OptionalHeader.SizeOfImage;
    return image;
}

// Reads `count` bytes at `rva` from the module's file on disk (RVA -> file offset via the section table).
bool read_file_bytes(HMODULE module, uint32_t rva, uint8_t* out, size_t count) {
    wchar_t path[MAX_PATH]{};

    if (GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        return false;
    }

    std::ifstream file{path, std::ios::binary};
    std::vector<uint8_t> headers(4096);

    if (!file.read((char*)headers.data(), headers.size())) {
        return false;
    }

    const auto nt = (IMAGE_NT_HEADERS*)(headers.data() + ((IMAGE_DOS_HEADER*)headers.data())->e_lfanew);
    auto section = IMAGE_FIRST_SECTION(nt);

    for (auto i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (rva >= section->VirtualAddress && rva + count <= section->VirtualAddress + section->SizeOfRawData) {
            file.seekg(section->PointerToRawData + (rva - section->VirtualAddress));
            return (bool)file.read((char*)out, count);
        }
    }

    return false;
}

void write_code(uint8_t* at, const uint8_t* bytes, size_t count) {
    DWORD old{};
    VirtualProtect(at, count, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(at, bytes, count);
    VirtualProtect(at, count, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, count);
}

bool is_jump(const uint8_t* p) {
    return p[0] == 0xE9 || p[0] == 0xEB || (p[0] == 0xFF && p[1] == 0x25);
}
} // namespace

void* find_link_swapchain_to_cmd_queue(HMODULE dlssg) {
    if (dlssg == nullptr) {
        return nullptr;
    }

    const auto image = image_of(dlssg);
    static const char name[] = "linkSwapchainToCmdQueue";

    // The string...
    uint8_t* str = nullptr;

    for (size_t i = 0; i + sizeof(name) <= image.size && str == nullptr; ++i) {
        if (std::memcmp(image.base + i, name, sizeof(name)) == 0) { // includes the terminator
            str = image.base + i;
        }
    }

    if (str == nullptr) {
        return nullptr;
    }

    // ...the code referencing it (RIP-relative displacement), in executable sections only...
    auto section = IMAGE_FIRST_SECTION(image.nt);

    for (auto s = 0; s < image.nt->FileHeader.NumberOfSections; ++s, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const auto begin = image.base + section->VirtualAddress;
        const auto end = begin + section->Misc.VirtualSize - 4;

        for (auto p = begin; p < end; ++p) {
            if (p + 4 + *(int32_t*)p != str) {
                continue;
            }

            // ...and the start of the function containing it, from the unwind table (following chained entries).
            DWORD64 base{};
            auto entry = RtlLookupFunctionEntry((DWORD64)p, &base, nullptr);

            while (entry != nullptr && (entry->UnwindData & 1) == 0) {
                const auto unwind = (uint8_t*)(base + entry->UnwindData);

                if (((unwind[0] >> 3) & UNW_FLAG_CHAININFO) == 0) {
                    break;
                }

                const auto codes = unwind[2];
                entry = (PRUNTIME_FUNCTION)(unwind + 4 + ((codes + 1) & ~1) * 2);
            }

            return entry != nullptr ? (void*)(base + entry->BeginAddress) : nullptr;
        }
    }

    return nullptr;
}

TemporaryUnhook::TemporaryUnhook(void* fn, HMODULE module) {
    if (fn == nullptr || module == nullptr) {
        return;
    }

    m_fn = (uint8_t*)fn;
    uint8_t original[BYTES]{};
    const auto rva = (uint32_t)(m_fn - (uint8_t*)module);

    if (!read_file_bytes(module, rva, original, BYTES)) {
        log_warn("Could not read linkSwapchainToCmdQueue's original bytes from disk");
        return;
    }

    std::memcpy(m_patched, m_fn, BYTES);

    if (std::memcmp(m_patched, original, BYTES) == 0) {
        return; // not hooked
    }

    if (!is_jump(m_patched)) {
        log_warn("linkSwapchainToCmdQueue differs from the file but isn't a jump: leaving it alone");
        return;
    }

    write_code(m_fn, original, BYTES);
    m_active = true;
}

TemporaryUnhook::~TemporaryUnhook() {
    if (m_active) {
        write_code(m_fn, m_patched, BYTES);
    }
}
