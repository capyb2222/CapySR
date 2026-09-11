#pragma once

#include <format>
#include <string>
#include <string_view>

namespace logging {

enum class Level { Trace = 0, Debug, Info, Warn, Error };

void init(Level level, bool color, const std::string& file);
void shutdown();
Level level();
void write(Level level, std::string_view tag, std::string_view message);

inline bool enabled(Level l) { return l >= level(); }

template <class... Args>
void trace(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Trace)) write(Level::Trace, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void debug(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void info(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void warn(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, tag, std::format(fmt, std::forward<Args>(args)...));
}
template <class... Args>
void error(std::string_view tag, std::format_string<Args...> fmt, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, tag, std::format(fmt, std::forward<Args>(args)...));
}

Level parseLevel(std::string_view name, Level fallback = Level::Info);

}  // namespace logging
