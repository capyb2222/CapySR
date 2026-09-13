#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct EndpointConfig {
    std::string bind = "0.0.0.0";
    uint16_t port = 0;
    std::string publicHost = "127.0.0.1";
};

struct LogConfig {
    std::string level = "info";
    bool color = true;
    std::string file;
    bool packets = false;
    bool packetBodies = false;
};

struct HotfixConfig {
    std::string file = "config/hotfix.json";
    // On when the client's own design data is incomplete and the CDN is reachable --
    // the 4.5.53 package indexes only 6 design data bundles and cannot find its own
    // TextMap, which renders every dialog blank. Turn it off for a package that ships
    // the full set, or on a network that cannot reach autopatchcn: an unreachable CDN
    // hangs the loading bar forever instead.
    bool enableDesignDataUpdate = true;
    bool enableVideoUpdate = true;
    bool sendResourceUrls = true;
    bool fetchRemote = false;
};

struct PlayerDefaults {
    uint32_t uid = 1337;
    std::string name = "Capybara";
    std::string signature = "powered by CapySR";
    uint32_t level = 70;
    uint32_t worldLevel = 6;
    uint32_t headIcon = 201001;
    uint32_t stamina = 240;
    uint32_t hcoin = 999999;
    uint32_t scoin = 99999999;
    uint32_t mcoin = 0;
};

struct PathsConfig {
    std::string srtoolsFile = "data/freesr-data.json";
    std::string playerFile = "data/player.json";
    // Priority ordered: the first source to define a row id keeps it, later ones
    // only fill gaps. Beta dumps are trimmed extracts, so they go last.
    std::vector<std::string> dataSources{"resources/excel", "resources/excel-beta"};
    std::string sceneRes = "resources/res.json";
    std::string anchors = "resources/Anchor.json";
    std::string resources = "resources";
};

struct WarpBanner {
    uint32_t id = 0;
    uint32_t featured = 0;  // 0 keeps the pool's own
};

struct GameplayConfig {
    // auto = a srtools build replaces calyx fights and nothing else; srtools = it
    // replaces every fight; stage = never, always use the stage the client asked for.
    std::string battleSource = "auto";
    bool globalBuffs = true;
    // Report every challenge floor as a full clear. A floor with no stars locks the one
    // after it, so without this only floor 1 of each mode is reachable.
    bool unlockAllChallenges = true;
    uint32_t mainCharacter = 8008;
    uint32_t marchType = 1224;
    // Missions never reported as finished. A scene group gated on one of these makes
    // the 4.5.54 client throw inside MissionModule._CheckVerseByMainMission while it
    // works out which groups to activate, which kills the whole map load: a hang on the
    // loading screen when entering at login, a fade that rolls straight back otherwise.
    // Main and sub mission ids both go here.
    std::vector<uint32_t> skipMissions;
    // Limited warp pools offered next to the standard one, each optionally featuring another
    // 5*. The beta clients list 2001, 2002, 3001 and 3002 but ship no banner art for any of
    // them, and one pool without art turns the whole warp page black, so leave it empty there.
    std::vector<WarpBanner> limitedWarpPools;
    // A 5* character Stellar Warp features, 50/50 and guarantee included, standing in for
    // the current banner the beta clients have no art for. 0 keeps it the standard pool.
    uint32_t standardWarpFeatured = 0;

    bool missionSkipped(uint32_t id) const {
        return std::find(skipMissions.begin(), skipMissions.end(), id) != skipMissions.end();
    }
};

struct Config {
    std::string serverName = "CapySR";
    EndpointConfig http{"0.0.0.0", 21000, "127.0.0.1"};
    EndpointConfig game{"0.0.0.0", 23301, "127.0.0.1"};
    LogConfig log;
    HotfixConfig hotfix;
    PlayerDefaults player;
    PathsConfig paths;
    GameplayConfig gameplay;

    static Config& get();
    bool load(const std::string& path);
};

}  // namespace core
