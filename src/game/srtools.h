#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace game {

struct SubAffix {
    uint32_t id = 0;
    uint32_t count = 0;
    uint32_t step = 0;
};

struct Relic {
    uint32_t level = 0;
    uint32_t relicId = 0;
    uint32_t relicSetId = 0;
    uint32_t mainAffixId = 0;
    uint32_t equipAvatar = 0;
    uint32_t internalUid = 0;
    std::vector<SubAffix> subAffixes;

    uint32_t slot() const { return relicId % 10; }
};

struct Lightcone {
    uint32_t level = 0;
    uint32_t itemId = 0;
    uint32_t equipAvatar = 0;
    uint32_t rank = 0;
    uint32_t promotion = 0;
    uint32_t internalUid = 0;
};

struct Avatar {
    uint32_t avatarId = 0;
    uint32_t level = 1;
    uint32_t promotion = 0;
    uint32_t rank = 0;
    uint32_t enhancedId = 0;
    uint32_t spValue = 0;
    uint32_t spMax = 120;
    std::map<uint32_t, uint32_t> skills;            // skill tree point -> level
    std::map<uint32_t, uint32_t> skillsByAnchor;    // trace anchor -> level
    std::vector<uint32_t> techniques;
};

struct BattleMonster {
    uint32_t monsterId = 0;
    uint32_t level = 1;
    uint32_t amount = 1;
};

struct BattleBuff {
    uint32_t id = 0;
    uint32_t level = 1;
    std::map<std::string, float> dynamicValues;
};

struct BattleConfig {
    std::string battleType = "DEFAULT";
    uint32_t stageId = 0;
    uint32_t cycleCount = 0;
    uint32_t pathResonanceId = 0;
    std::vector<std::vector<BattleMonster>> waves;
    std::vector<BattleBuff> blessings;
    std::vector<SubAffix> customStats;
    std::map<uint32_t, uint32_t> customLineup;  // slot -> avatar id
};

struct SrToolsData {
    std::map<uint32_t, Avatar> avatars;
    std::vector<Lightcone> lightcones;
    std::vector<Relic> relics;
    BattleConfig battle;
    // The parts of the build we do not model, kept verbatim so writing it back never
    // loses the user's loadout presets or whatever srtools adds next.
    std::string battleJson;
    std::string extrasJson;
    bool loaded = false;
};

// Holds the freesr-data.json build that srtools.neonteam.dev uploads to /srtools.
class SrTools {
public:
    static SrTools& instance();

    bool loadFromDisk();
    // Accepts the POST body from srtools; returns "OK" or an error message.
    std::string upload(const std::string& body);

    std::shared_ptr<const SrToolsData> data() const;

    // Copy-on-write edit: the caller mutates a fresh copy, which then replaces the
    // snapshot and is written back to disk. Keeps every reader on an immutable view.
    void mutate(const std::function<void(SrToolsData&)>& edit, bool persist = true);

private:
    bool writeToDisk(const SrToolsData& data) const;

    void set(std::shared_ptr<const SrToolsData> data);

    mutable std::mutex mutex_;
    std::shared_ptr<const SrToolsData> data_ = std::make_shared<SrToolsData>();
};

}  // namespace game
