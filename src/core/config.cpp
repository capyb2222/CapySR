#include "core/config.h"

#include <filesystem>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/util.h"

using json = nlohmann::json;

namespace core {
namespace {

template <class T>
void pick(const json& j, const char* key, T& out) {
    auto it = j.find(key);
    if (it != j.end() && !it->is_null()) {
        try {
            out = it->get<T>();
        } catch (const std::exception& e) {
            logging::warn("config", "bad value for '{}': {}", key, e.what());
        }
    }
}

void readEndpoint(const json& j, const char* key, EndpointConfig& ep) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return;
    pick(*it, "bind", ep.bind);
    pick(*it, "port", ep.port);
    pick(*it, "public_host", ep.publicHost);
}

}  // namespace

Config& Config::get() {
    static Config instance;
    return instance;
}

bool Config::load(const std::string& path) {
    bool ok = false;
    std::string text = util::readFile(path, &ok);
    if (!ok) {
        // Naming the directory it looked in is the difference between a one-line fix
        // and an afternoon: every path below is relative to it.
        std::error_code ec;
        logging::warn("config", "{} not found under {}, using defaults", path,
                      std::filesystem::current_path(ec).string());
        return false;
    }

    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        logging::error("config", "{} is not valid json: {}", path, e.what());
        return false;
    }

    if (auto it = j.find("server"); it != j.end() && it->is_object()) {
        pick(*it, "name", serverName);
        readEndpoint(*it, "http", http);
        readEndpoint(*it, "game", game);
    }
    if (auto it = j.find("log"); it != j.end() && it->is_object()) {
        pick(*it, "level", log.level);
        pick(*it, "color", log.color);
        pick(*it, "file", log.file);
        pick(*it, "packets", log.packets);
        pick(*it, "packet_bodies", log.packetBodies);
    }
    if (auto it = j.find("hotfix"); it != j.end() && it->is_object()) {
        pick(*it, "file", hotfix.file);
        pick(*it, "enable_design_data_update", hotfix.enableDesignDataUpdate);
        pick(*it, "enable_video_update", hotfix.enableVideoUpdate);
        pick(*it, "send_resource_urls", hotfix.sendResourceUrls);
        pick(*it, "fetch_remote", hotfix.fetchRemote);
    }
    if (auto it = j.find("player"); it != j.end() && it->is_object()) {
        pick(*it, "uid", player.uid);
        pick(*it, "name", player.name);
        pick(*it, "signature", player.signature);
        pick(*it, "level", player.level);
        pick(*it, "world_level", player.worldLevel);
        pick(*it, "head_icon", player.headIcon);
        pick(*it, "stamina", player.stamina);
        pick(*it, "hcoin", player.hcoin);
        pick(*it, "scoin", player.scoin);
        pick(*it, "mcoin", player.mcoin);
    }
    if (auto it = j.find("paths"); it != j.end() && it->is_object()) {
        pick(*it, "srtools_file", paths.srtoolsFile);
        pick(*it, "player_file", paths.playerFile);
        pick(*it, "data_sources", paths.dataSources);
        pick(*it, "scene_res", paths.sceneRes);
        pick(*it, "anchors", paths.anchors);
        pick(*it, "resources", paths.resources);
    }
    if (auto it = j.find("gameplay"); it != j.end() && it->is_object()) {
        pick(*it, "battle_source", gameplay.battleSource);
        pick(*it, "global_buffs", gameplay.globalBuffs);
        pick(*it, "unlock_all_challenges", gameplay.unlockAllChallenges);
        pick(*it, "main_character", gameplay.mainCharacter);
        pick(*it, "march_type", gameplay.marchType);
        pick(*it, "skip_missions", gameplay.skipMissions);
        // Each entry is a pool id, or {"id": pool, "featured": 5* id}.
        if (auto pools = it->find("limited_warp_pools"); pools != it->end() && pools->is_array()) {
            gameplay.limitedWarpPools.clear();
            for (const json& entry : *pools) {
                WarpBanner banner;
                if (entry.is_number_unsigned()) {
                    banner.id = entry.get<uint32_t>();
                } else if (entry.is_object()) {
                    pick(entry, "id", banner.id);
                    pick(entry, "featured", banner.featured);
                }
                if (banner.id == 0) {
                    logging::warn("config", "a limited_warp_pools entry has no pool id");
                    continue;
                }
                gameplay.limitedWarpPools.push_back(banner);
            }
        }
        pick(*it, "standard_warp_featured", gameplay.standardWarpFeatured);
    }
    return true;
}

}  // namespace core
