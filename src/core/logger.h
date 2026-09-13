#pragma once

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace logging {

enum class Level { Trace = 0, Debug, Info, Warn, Error };

using BannerRows = std::vector<std::pair<std::string, std::string>>;

// After init() a line is only queued; a background thread writes it. Before init() and
// after shutdown() lines are written on the spot.
void init(Level level, bool color, const std::string& file);
void shutdown();
Level level();
void write(Level level, std::string_view tag, std::string_view message);
// A boxed summary on the console; the file gets one plain line per row under `tag`.
void banner(std::string_view tag, std::string_view title, const BannerRows& rows);

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

// The formatting, exposed for the tests.
namespace detail {
std::string consoleLine(Level level, std::string_view time, std::string_view tag,
                        std::string_view message, bool color);
std::string fileLine(Level level, std::string_view time, std::string_view tag,
                     std::string_view message);
std::string bannerBox(std::string_view title, const BannerRows& rows, bool color);
}  // namespace detail

}  // namespace logging
