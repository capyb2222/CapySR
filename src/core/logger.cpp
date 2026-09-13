#include "core/logger.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace logging {
namespace {

// Wide enough for the longest tag in use, "challenge".
constexpr size_t kTagWidth = 10;

constexpr const char* kReset = "\033[0m";
constexpr const char* kGray = "\033[90m";
constexpr const char* kBold = "\033[1m";

std::atomic<Level> g_level{Level::Info};

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

const char* levelStyle(Level l) {
    switch (l) {
        case Level::Trace: return "\033[90m";
        case Level::Debug: return "\033[36m";
        case Level::Info: return "\033[32m";
        case Level::Warn: return "\033[1;33m";
        case Level::Error: return "\033[1;31m";
    }
    return "";
}

// A tag keeps its colour from run to run: it is picked off a hash of the name, from a
// palette that stays clear of the warning and error colours.
const char* tagStyle(std::string_view tag) {
    static constexpr const char* kPalette[] = {
        "\033[38;5;111m", "\033[38;5;147m", "\033[38;5;180m", "\033[38;5;116m",
        "\033[38;5;181m", "\033[38;5;150m", "\033[38;5;139m", "\033[38;5;109m",
    };
    uint32_t hash = 2166136261u;
    for (unsigned char c : tag) hash = (hash ^ c) * 16777619u;
    return kPalette[hash % std::size(kPalette)];
}

std::string timeOf(std::chrono::system_clock::time_point when) {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(when.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(when);
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

struct Entry {
    std::chrono::system_clock::time_point when;
    Level level = Level::Info;
    std::string tag;
    std::string message;
    bool banner = false;
    BannerRows rows;
};

// The writer and everything it touches. A function-local static, so the thread is joined
// on exit even when main() returns early without calling shutdown().
class Sink {
public:
    ~Sink() { stop(); }

    void start(bool color, const std::string& path) {
        std::lock_guard lock(mutex_);
        color_ = color;
        if (!path.empty() && !file_.is_open()) {
            std::error_code ec;
            std::filesystem::path parent = std::filesystem::path(path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent, ec);
            file_.open(path, std::ios::app);
        }
        if (!writer_.joinable()) {
            stopping_ = false;
            writer_ = std::thread([this] { run(); });
        }
    }

    void stop() {
        {
            std::lock_guard lock(mutex_);
            if (!writer_.joinable()) return;
            stopping_ = true;
        }
        wake_.notify_one();
        writer_.join();
        std::lock_guard lock(mutex_);
        writer_ = std::thread();
        if (file_.is_open()) file_.close();
    }

    void push(Entry entry) {
        std::unique_lock lock(mutex_);
        if (!writer_.joinable() || stopping_) {
            std::string console, file;
            render(entry, console, file);
            emit(console, file);
            return;
        }
        // The writer drains the whole queue at once, so it only needs waking when it is empty.
        bool wasEmpty = queue_.empty();
        queue_.push_back(std::move(entry));
        lock.unlock();
        if (wasEmpty) wake_.notify_one();
    }

private:
    void run() {
        std::vector<Entry> batch;
        std::string console, file;
        std::unique_lock lock(mutex_);
        while (true) {
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return;
            batch.swap(queue_);
            lock.unlock();
            console.clear();
            file.clear();
            for (const Entry& entry : batch) render(entry, console, file);
            batch.clear();
            emit(console, file);
            lock.lock();
        }
    }

    void render(const Entry& entry, std::string& console, std::string& file) const {
        std::string time = timeOf(entry.when);
        if (entry.banner) {
            console += detail::bannerBox(entry.message, entry.rows, color_);
            for (const auto& [label, value] : entry.rows) {
                file += detail::fileLine(Level::Info, time, entry.tag, label + " " + value);
            }
            return;
        }
        console += detail::consoleLine(entry.level, time, entry.tag, entry.message, color_);
        file += detail::fileLine(entry.level, time, entry.tag, entry.message);
    }

    // One write and one flush per batch: the console is the slow part.
    void emit(const std::string& console, const std::string& file) {
        std::lock_guard out(outputMutex_);
        if (!console.empty()) {
            std::fwrite(console.data(), 1, console.size(), stdout);
            std::fflush(stdout);
        }
        if (file_.is_open() && !file.empty()) {
            file_.write(file.data(), static_cast<std::streamsize>(file.size()));
            file_.flush();
        }
    }

    std::mutex mutex_;
    std::mutex outputMutex_;
    std::condition_variable wake_;
    std::vector<Entry> queue_;
    std::thread writer_;
    bool stopping_ = false;
    bool color_ = false;
    std::ofstream file_;
};

Sink& sink() {
    static Sink instance;
    return instance;
}

}  // namespace

namespace detail {

std::string consoleLine(Level level, std::string_view time, std::string_view tag,
                        std::string_view message, bool color) {
    std::string out;
    out.reserve(time.size() + tag.size() + message.size() + 96);
    auto styled = [&](const char* style, std::string_view text) {
        if (color) out += style;
        out += text;
        if (color) out += kReset;
    };

    styled(kGray, time);
    out += "  ";
    styled(levelStyle(level), levelName(level));
    out += "  ";
    styled(tagStyle(tag), tag);
    size_t tagColumn = std::max(kTagWidth, tag.size() + 1);
    out.append(tagColumn - tag.size(), ' ');
    size_t indent = time.size() + 2 + 5 + 2 + tagColumn;

    bool packet = tag == "recv" || tag == "send";
    size_t start = 0;
    while (true) {
        size_t end = message.find('\n', start);
        std::string_view line =
            message.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (start != 0) {
            out += '\n';
            out.append(indent, ' ');
        }
        if (packet && start == 0 && !line.starts_with("  ")) {
            // "DoGachaCsReq (1905) 5 bytes": the name stands out, the numbers step back.
            if (tag == "recv") {
                styled("\033[32m", color ? "← " : "<- ");
            } else {
                styled("\033[34m", color ? "→ " : "-> ");
            }
            size_t split = line.find(" (");
            styled(kBold, line.substr(0, split));
            if (split != std::string_view::npos) styled(kGray, line.substr(split));
        } else if (packet) {
            styled(kGray, line);
        } else if (level >= Level::Warn) {
            styled(levelStyle(level), line);
        } else {
            out += line;
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    out += '\n';
    return out;
}

std::string fileLine(Level level, std::string_view time, std::string_view tag,
                     std::string_view message) {
    return std::format("{} {} {} {}\n", time, levelName(level), tag, message);
}

std::string bannerBox(std::string_view title, const BannerRows& rows, bool color) {
    size_t labelWidth = 0;
    size_t valueWidth = 0;
    for (const auto& [label, value] : rows) {
        labelWidth = std::max(labelWidth, label.size());
        valueWidth = std::max(valueWidth, value.size());
    }
    size_t inner = std::max(labelWidth + 2 + valueWidth, title.size() + 2);

    std::string_view dash = color ? "─" : "-";
    std::string_view bar = color ? "│" : "|";
    auto frame = [color](const std::string& text) {
        return color ? std::string(kGray) + text + kReset : text;
    };
    auto dashes = [&](size_t n) {
        std::string line;
        for (size_t i = 0; i < n; ++i) line += dash;
        return line;
    };

    std::string out;
    out += frame(std::string(color ? "╭" : "+") + std::string(dash) + " ");
    out += color ? std::string(kBold) + std::string(title) + kReset : std::string(title);
    out += frame(" " + dashes(inner - title.size() - 1) + (color ? "╮" : "+"));
    out += '\n';
    for (const auto& [label, value] : rows) {
        out += frame(std::string(bar) + " ");
        std::string paddedLabel = label + std::string(labelWidth - label.size() + 2, ' ');
        out += color ? std::string(kGray) + paddedLabel + kReset : paddedLabel;
        out += value;
        out.append(inner - labelWidth - 2 - value.size(), ' ');
        out += frame(" " + std::string(bar));
        out += '\n';
    }
    out += frame(std::string(color ? "╰" : "+") + dashes(inner + 2) + (color ? "╯" : "+"));
    out += '\n';
    return out;
}

}  // namespace detail

void init(Level lvl, bool color, const std::string& file) {
    g_level = lvl;
#ifdef _WIN32
    if (color) {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD mode = 0;
        if (GetConsoleMode(h, &mode)) SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        SetConsoleOutputCP(CP_UTF8);
    }
#endif
    sink().start(color, file);
}

void shutdown() { sink().stop(); }

Level level() { return g_level; }

void write(Level lvl, std::string_view tag, std::string_view message) {
    Entry entry;
    entry.when = std::chrono::system_clock::now();
    entry.level = lvl;
    entry.tag = tag;
    entry.message = message;
    sink().push(std::move(entry));
}

void banner(std::string_view tag, std::string_view title, const BannerRows& rows) {
    Entry entry;
    entry.when = std::chrono::system_clock::now();
    entry.tag = tag;
    entry.message = title;
    entry.banner = true;
    entry.rows = rows;
    sink().push(std::move(entry));
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
