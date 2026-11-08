#include <windows.h>

#include <chrono>
#include <cstdio>
#include <mutex>

#include "log.hpp"

namespace {
std::mutex g_mutex{};
FILE* g_file{};
void (*g_ref_warn)(const char*, ...){};
void (*g_ref_error)(const char*, ...){};
} // namespace

namespace logging {
void open(const std::wstring& path) {
    std::scoped_lock _{g_mutex};

    if (g_file == nullptr) {
        g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    }
}

void set_reframework_sink(void (*warn)(const char*, ...), void (*error)(const char*, ...)) {
    std::scoped_lock _{g_mutex};
    g_ref_warn = warn;
    g_ref_error = error;
}

void write(std::string_view level, std::string_view text) {
    const auto now = std::chrono::system_clock::now();
    const auto line = std::format("[{:%H:%M:%S}] [{}] [tid {}] {}\n", std::chrono::floor<std::chrono::milliseconds>(now), level,
        GetCurrentThreadId(), text);

    std::scoped_lock _{g_mutex};

    if (g_file != nullptr) {
        fputs(line.c_str(), g_file);
        fflush(g_file);
    }

    if (level == "warn" && g_ref_warn != nullptr) {
        g_ref_warn("[Native DLSS + FG] %.*s", (int)text.size(), text.data());
    } else if (level == "error" && g_ref_error != nullptr) {
        g_ref_error("[Native DLSS + FG] %.*s", (int)text.size(), text.data());
    }
}
} // namespace logging
