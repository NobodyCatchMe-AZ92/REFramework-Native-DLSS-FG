#include <windows.h>

#include <atomic>
#include <string>

#include "log.hpp"

#include "crash_log.hpp"

namespace {
std::string describe(uintptr_t address) {
    HMODULE module{};

    if (address != 0 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)address, &module))
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(module, path, MAX_PATH);
        std::wstring name{path};
        name = name.substr(name.find_last_of(L"\\/") + 1);
        return std::format("{}+{:x}", std::string(name.begin(), name.end()), address - (uintptr_t)module);
    }

    return std::format("{:x} (no module)", address);
}

LONG WINAPI handler(EXCEPTION_POINTERS* info) {
    const auto code = info->ExceptionRecord->ExceptionCode;

    // Only real faults; C++ exceptions, debugger and other notifications pass straight through.
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION && code != EXCEPTION_PRIV_INSTRUCTION &&
        code != EXCEPTION_STACK_OVERFLOW)
    {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Pointer probes (IsBadReadPtr and friends, used a lot by REFramework) fault inside kernel32/KernelBase on purpose
    // and handle it: not crashes.
    static const auto kernel32 = GetModuleHandleW(L"kernel32.dll");
    static const auto kernelbase = GetModuleHandleW(L"kernelbase.dll");
    HMODULE fault_module{};
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)info->ExceptionRecord->ExceptionAddress, &fault_module);

    if (fault_module != nullptr && (fault_module == kernel32 || fault_module == kernelbase)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static std::atomic<int> logged{0};

    if (logged.fetch_add(1) >= 5) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    const auto ctx = info->ContextRecord;
    log_error("Fault {:x} at {} (thread {}), accessing {:x}", (uint32_t)code, describe((uintptr_t)info->ExceptionRecord->ExceptionAddress),
        GetCurrentThreadId(), info->ExceptionRecord->NumberParameters >= 2 ? info->ExceptionRecord->ExceptionInformation[1] : 0);
    log_error("  rip {:x} rsp {:x} rax {:x} rcx {:x} rdx {:x} r8 {:x}", ctx->Rip, ctx->Rsp, ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->R8);

    // The return addresses on the stack that point into a module: a rough stack trace that survives a garbage rip.
    auto sp = (uintptr_t*)ctx->Rsp;
    int shown = 0;

    for (int i = 0; i < 512 && shown < 24; ++i) {
        uintptr_t value{};

        if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &value, sizeof(value), nullptr)) {
            break;
        }

        HMODULE module{};

        if (value > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)value, &module))
        {
            log_error("  stack[{}] {}", i, describe(value));
            ++shown;
        }
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

namespace crash_log {
void install() {
    AddVectoredExceptionHandler(0, &handler); // last in line: others (REFramework, the game) see it first
}
} // namespace crash_log
