#include "core/logger.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

#ifdef _WIN32
#include <windows.h>
#endif

namespace logging {
namespace {

std::mutex g_mutex;
Level g_level = Level::Info;
bool g_color = true;
std::ofstream g_file;

const char* levelName(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info: return "INFO ";
        case Level::Warn: return "WARN ";
        case Level::Error: return "ERROR";
    }
    return "?????";
}

const char* levelColor(Level l) {
    switch (l) {
        case Level::Trace: return "\033[90m";
        case Level::Debug: return "\033[36m";
        case Level::Info: return "\033[32m";
        case Level::Warn: return "\033[33m";
        case Level::Error: return "\033[31m";
    }
    return "";
}

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms.count()));
    return buf;
}

}  // namespace

void init(Level lvl, bool color, const std::string& file) {
    std::lock_guard lock(g_mutex);
    g_level = lvl;
    g_color = color;
#ifdef _WIN32
    if (color) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
    if (!file.empty()) {
        std::error_code ec;
        std::filesystem::path parent = std::filesystem::path(file).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        g_file.open(file, std::ios::app);
    }
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file.is_open()) g_file.close();
}

Level level() { return g_level; }

void write(Level lvl, std::string_view tag, std::string_view message) {
    std::string ts = timestamp();
    std::lock_guard lock(g_mutex);
    if (g_color) {
        std::fprintf(stdout, "\033[90m%s\033[0m %s%s\033[0m \033[95m%.*s\033[0m %.*s\n", ts.c_str(),
                     levelColor(lvl), levelName(lvl), static_cast<int>(tag.size()), tag.data(),
                     static_cast<int>(message.size()), message.data());
    } else {
        std::fprintf(stdout, "%s %s %.*s %.*s\n", ts.c_str(), levelName(lvl),
                     static_cast<int>(tag.size()), tag.data(), static_cast<int>(message.size()),
                     message.data());
    }
    std::fflush(stdout);
    if (g_file.is_open()) {
        g_file << ts << ' ' << levelName(lvl) << ' ' << tag << ' ' << message << '\n';
        g_file.flush();
    }
}

Level parseLevel(std::string_view name, Level fallback) {
    if (name == "trace") return Level::Trace;
    if (name == "debug") return Level::Debug;
    if (name == "info") return Level::Info;
    if (name == "warn" || name == "warning") return Level::Warn;
    if (name == "error") return Level::Error;
    return fallback;
}

}  // namespace logging
