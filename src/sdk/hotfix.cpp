#include "sdk/hotfix.h"

#include <mutex>

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"

using json = nlohmann::json;

namespace sdk {
namespace {

std::mutex g_mutex;

std::string field(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

json loadFile(const std::string& path) {
    bool ok = false;
    std::string text = util::readFile(path, &ok);
    if (!ok) return json::object();
    try {
        json j = json::parse(text, nullptr, true, true);
        return j.is_object() ? j : json::object();
    } catch (const std::exception& e) {
        logging::error("hotfix", "{} is not valid json: {}", path, e.what());
        return json::object();
    }
}

// The revision the CDN folder is named after: .../output_16399471_bf17e3d5c3d7_... -> 16399471.
std::string revisionOf(const std::string& url) {
    size_t at = url.rfind("/output_");
    if (at == std::string::npos) return {};
    at += 8;
    size_t end = url.find('_', at);
    if (end == std::string::npos || end == at) return {};
    std::string rev = url.substr(at, end - at);
    return rev.find_first_not_of("0123456789") == std::string::npos ? rev : std::string();
}

}  // namespace

HotfixUrls lookupHotfix(const std::string& version) {
    const auto& cfg = core::Config::get();
    std::lock_guard lock(g_mutex);
    json file = loadFile(cfg.hotfix.file);

    auto it = file.find(version);
    if (it == file.end() || !it->is_object()) {
        logging::error("hotfix",
                   "no entry for client version '{}' in {} -- the client will stall on the "
                   "loading bar. Add its hotfix urls there.",
                   version, cfg.hotfix.file);
        return {};
    }

    HotfixUrls urls;
    urls.assetBundleUrl = field(*it, "asset_bundle_url");
    urls.exResourceUrl = field(*it, "ex_resource_url");
    urls.luaUrl = field(*it, "lua_url");
    urls.ifixUrl = field(*it, "ifix_url");
    urls.ifixVersion = field(*it, "ifix_version");
    urls.luaVersion = field(*it, "lua_version");
    // Usually left empty in hotfix.json; the urls carry them.
    if (urls.luaVersion.empty()) urls.luaVersion = revisionOf(urls.luaUrl);
    if (urls.ifixVersion.empty()) urls.ifixVersion = revisionOf(urls.ifixUrl);
    return urls;
}

void storeHotfix(const std::string& version, const HotfixUrls& urls) {
    const auto& cfg = core::Config::get();
    std::lock_guard lock(g_mutex);
    json file = loadFile(cfg.hotfix.file);
    json entry;
    entry["asset_bundle_url"] = urls.assetBundleUrl;
    entry["ex_resource_url"] = urls.exResourceUrl;
    entry["lua_url"] = urls.luaUrl;
    entry["ifix_url"] = urls.ifixUrl;
    entry["ifix_version"] = urls.ifixVersion;
    entry["lua_version"] = urls.luaVersion;
    file[version] = entry;
    util::writeFile(cfg.hotfix.file, file.dump(4));
}

}  // namespace sdk
