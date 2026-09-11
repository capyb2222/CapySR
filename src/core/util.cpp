#include "core/util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace util {
namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

std::mt19937_64& engine() {
    static thread_local std::mt19937_64 rng(std::random_device{}() ^
                                            static_cast<uint64_t>(nowMs()));
    return rng;
}

}  // namespace

std::string base64Encode(std::string_view data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = (static_cast<uint8_t>(data[i]) << 16) |
                     (static_cast<uint8_t>(data[i + 1]) << 8) | static_cast<uint8_t>(data[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
    }
    if (i + 1 == data.size()) {
        uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        uint32_t v =
            (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string base64Decode(std::string_view data) {
    std::string out;
    out.reserve(data.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : data) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        int v = b64Value(c);
        if (v < 0) continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((acc >> bits) & 0xFF);
        }
    }
    return out;
}

std::string hexDump(std::string_view data, size_t limit) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    size_t n = std::min(limit, data.size());
    out.reserve(n * 3);
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = static_cast<uint8_t>(data[i]);
        out += kHex[c >> 4];
        out += kHex[c & 15];
        out += ' ';
    }
    if (n < data.size()) out += "...";
    return out;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
    std::vector<std::string_view> parts;
    size_t start = 0;
    while (true) {
        size_t pos = s.find(sep, start);
        if (pos == std::string_view::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

bool startsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

uint64_t nowMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
}

uint64_t nowSec() { return nowMs() / 1000; }

uint32_t randomU32() { return static_cast<uint32_t>(engine()()); }
uint64_t randomU64() { return engine()(); }

uint32_t randomRange(uint32_t low, uint32_t high) {
    if (high <= low) return low;
    return low + static_cast<uint32_t>(engine()() % (high - low + 1));
}

std::string randomHex(size_t bytes) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        uint8_t b = static_cast<uint8_t>(engine()());
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

std::string readFile(const std::string& path, bool* ok) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (ok) *ok = false;
        return {};
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

bool writeFile(const std::string& path, std::string_view data) {
    std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return out.good();
}

std::string executableDir() {
    std::error_code ec;
#ifdef _WIN32
    wchar_t buffer[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0) return {};
    std::filesystem::path exe(std::wstring(buffer, n));
#else
    std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return {};
#endif
    return exe.parent_path().string();
}

std::string findRootDir(const std::string& marker) {
    std::error_code ec;
    // The working directory wins, so a deliberate `cd` still decides.
    std::filesystem::path here = std::filesystem::current_path(ec);
    if (!ec && std::filesystem::exists(here / marker, ec)) return here.string();

    // Otherwise walk up from the executable: build/bin/capysr.exe is two below root.
    std::filesystem::path dir(executableDir());
    for (int depth = 0; depth < 5 && !dir.empty(); ++depth) {
        if (std::filesystem::exists(dir / marker, ec)) return dir.string();
        std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

bool setCurrentDir(const std::string& path) {
    std::error_code ec;
    std::filesystem::current_path(path, ec);
    return !ec;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

}  // namespace util
