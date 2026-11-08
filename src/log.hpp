#pragma once

#include <format>
#include <string>
#include <string_view>

// Our own log file (reframework/data/refw_framegen.log): the plugin starts working before REFramework hands it its
// logging functions, so everything goes here; once REFramework is ready, warnings and errors also go to its log.
namespace logging {
void open(const std::wstring& path);
void write(std::string_view level, std::string_view text);
void set_reframework_sink(void (*warn)(const char*, ...), void (*error)(const char*, ...));
} // namespace logging

template <typename... Args>
void log_info(std::format_string<Args...> fmt, Args&&... args) {
    logging::write("info", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void log_warn(std::format_string<Args...> fmt, Args&&... args) {
    logging::write("warn", std::format(fmt, std::forward<Args>(args)...));
}

template <typename... Args>
void log_error(std::format_string<Args...> fmt, Args&&... args) {
    logging::write("error", std::format(fmt, std::forward<Args>(args)...));
}
