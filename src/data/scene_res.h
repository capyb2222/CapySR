#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace data {

// Scene geometry is quantised to 1/1000 of a unit, which is how the client sends it.
struct ResVector {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

struct ResProp {
    uint32_t groupId = 0;
    uint32_t instId = 0;
    uint32_t propId = 0;
    uint32_t propState = 0;
    ResVector pos;
    ResVector rot;
};

struct ResNpc {
    uint32_t groupId = 0;
    uint32_t instId = 0;
    uint32_t npcId = 0;
    ResVector pos;
    ResVector rot;
};

struct ResMonster {
    uint32_t groupId = 0;
    uint32_t instId = 0;
    uint32_t monsterId = 0;
    uint32_t eventId = 0;
    ResVector pos;
    ResVector rot;
};

struct ResTeleport {
    uint32_t id = 0;
    uint32_t anchorId = 0;
    uint32_t groupId = 0;
    uint32_t instId = 0;
    ResVector pos;
    ResVector rot;
};

struct ResGroup {
    uint32_t groupId = 0;
    std::vector<ResProp> props;
    std::vector<ResNpc> npcs;
    std::vector<ResMonster> monsters;
    std::vector<ResTeleport> teleports;
    std::vector<uint32_t> chests;
    std::vector<uint32_t> finishedSubMissions;
    std::vector<uint32_t> finishedMainMissions;

    bool empty() const { return props.empty() && npcs.empty() && monsters.empty(); }
};

struct ResFloor {
    uint32_t entryId = 0;
    uint32_t planeId = 0;
    uint32_t floorId = 0;
    uint32_t planeType = 0;
    uint32_t worldId = 0;
    bool isEnteredSceneInfo = false;
    std::vector<uint32_t> sections;
    std::map<std::string, int32_t> savedValues;
    std::vector<ResGroup> groups;

    const ResTeleport* teleport(uint32_t id) const;
    const ResTeleport* anyTeleport() const;
};

// The scene dump: entry id -> one floor's groups, props, npcs, monsters and anchors.
// Parsed into flat structs at startup so the json DOM can be released again.
class SceneRes {
public:
    static SceneRes& get();

    bool load(const std::string& path);
    // Where each entrance drops the player, for the floors whose dump has no
    // teleport pads to fall back on.
    bool loadAnchors(const std::string& path);
    bool loaded() const { return !floors_.empty(); }

    const ResTeleport* anchor(uint32_t entryId, uint32_t anchorId = 0) const;

    const ResFloor* byEntry(uint32_t entryId) const;
    // Through mapDefaultEntranceMap, which names one entry per floor.
    const ResFloor* byFloor(uint32_t floorId) const;
    uint32_t defaultEntrance(uint32_t floorId) const;

    const std::vector<ResFloor>& floors() const { return floors_; }
    const std::unordered_map<uint32_t, std::vector<uint32_t>>& relicRecommend() const {
        return relicRecommend_;
    }

private:
    std::vector<ResFloor> floors_;
    std::unordered_map<uint32_t, size_t> byEntry_;
    std::unordered_map<uint32_t, uint32_t> floorToEntry_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> relicRecommend_;
    std::unordered_map<uint32_t, std::vector<ResTeleport>> anchors_;
};

}  // namespace data
