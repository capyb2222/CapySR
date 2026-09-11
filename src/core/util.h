#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace util {

std::string base64Encode(std::string_view data);
std::string base64Decode(std::string_view data);

std::string hexDump(std::string_view data, size_t limit = 256);

std::vector<std::string_view> split(std::string_view s, char sep);
std::string_view trim(std::string_view s);
bool startsWith(std::string_view s, std::string_view prefix);
bool endsWith(std::string_view s, std::string_view suffix);
std::string toLower(std::string_view s);

uint64_t nowMs();
uint64_t nowSec();

uint32_t randomU32();
uint64_t randomU64();
uint32_t randomRange(uint32_t low, uint32_t high);
std::string randomHex(size_t bytes);

std::string readFile(const std::string& path, bool* ok = nullptr);
bool writeFile(const std::string& path, std::string_view data);
bool fileExists(const std::string& path);

std::string executableDir();
// Directory that holds `marker`, looked for in the working directory first and then
// upwards from the executable. Empty when nothing has it.
std::string findRootDir(const std::string& marker);
bool setCurrentDir(const std::string& path);

}  // namespace util
