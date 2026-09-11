#pragma once

#include <string>

namespace sdk {

struct HotfixUrls {
    std::string assetBundleUrl;
    std::string exResourceUrl;
    std::string luaUrl;
    std::string ifixUrl;
    std::string ifixVersion;
    std::string luaVersion;

    bool empty() const { return assetBundleUrl.empty() && exResourceUrl.empty(); }
};

// Looks version up in config/hotfix.json. An unknown version returns empty urls,
// which stalls the client at ~99% -- so it is logged loudly.
HotfixUrls lookupHotfix(const std::string& version);
void storeHotfix(const std::string& version, const HotfixUrls& urls);

}  // namespace sdk
