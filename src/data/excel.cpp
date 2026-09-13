#include "data/excel.h"

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/util.h"

using json = nlohmann::json;

namespace data {
namespace {

// Dumps disagree on shape: production dumps are a bare array of rows, the beta extracts
// wrap it in a single-key object, and older dumps use id -> row maps. A row is the
// first object that holds a scalar of its own; anything above that is a container.
bool looksLikeRow(const json& j) {
    if (!j.is_object()) return false;
    for (const auto& entry : j.items()) {
        const json& value = entry.value();
        if (value.is_number() || value.is_string() || value.is_boolean()) return true;
    }
    return false;
}

void collectRows(const json& j, std::vector<const json*>& out) {
    if (j.is_array()) {
        for (const auto& e : j) collectRows(e, out);
        return;
    }
    if (!j.is_object()) return;
    if (looksLikeRow(j)) {
        out.push_back(&j);
        return;
    }
    for (const auto& entry : j.items()) collectRows(entry.value(), out);
}

// Scalars are sometimes flat and sometimes boxed as {"Value": n}.
double numberOf(const json& j) {
    if (j.is_number()) return j.get<double>();
    if (j.is_object()) {
        auto it = j.find("Value");
        if (it != j.end() && it->is_number()) return it->get<double>();
    }
    return 0.0;
}

uint32_t u32(const json& j, const char* key, uint32_t fallback = 0) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    double v = numberOf(*it);
    if (v <= 0) return it->is_number() ? 0u : fallback;
    return static_cast<uint32_t>(v);
}

std::string str(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

bool boolOf(const json& j, const char* key) {
    auto it = j.find(key);
    return it != j.end() && it->is_boolean() && it->get<bool>();
}

std::vector<uint32_t> u32List(const json& j, const char* key) {
    std::vector<uint32_t> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& e : *it) {
        double v = numberOf(e);
        if (v > 0) out.push_back(static_cast<uint32_t>(v));
    }
    return out;
}

// "CombatPowerAvatarRarityType5", "CombatPowerLightconeRarity4": the stars are the last digit.
uint32_t rarityOf(const json& row) {
    auto it = row.find("Rarity");
    if (it == row.end()) return 0;
    if (it->is_number()) return static_cast<uint32_t>(numberOf(*it));
    if (!it->is_string()) return 0;
    const std::string& name = it->get_ref<const std::string&>();
    char last = name.empty() ? '\0' : name.back();
    return (last >= '1' && last <= '9') ? static_cast<uint32_t>(last - '0') : 0;
}

// ".../AvatarGacha_1102.prefab" -> 1102. The Sub* prefabs belong to a grouped pool and
// give 0.
uint32_t featuredFromPrefab(const std::string& path, std::string_view stem) {
    size_t slash = path.find_last_of('/');
    std::string_view file(path);
    if (slash != std::string::npos) file.remove_prefix(slash + 1);
    constexpr std::string_view suffix = ".prefab";
    if (file.size() <= stem.size() + suffix.size() || file.substr(0, stem.size()) != stem ||
        file.substr(file.size() - suffix.size()) != suffix) {
        return 0;
    }
    std::string_view digits = file.substr(stem.size(), file.size() - stem.size() - suffix.size());
    if (digits.size() > 9 || digits.find_first_not_of("0123456789") != std::string_view::npos) {
        return 0;
    }
    return static_cast<uint32_t>(std::strtoul(std::string(digits).c_str(), nullptr, 10));
}

uint32_t itemRarity(const std::string& name) {
    if (name == "Normal") return 1;
    if (name == "NotNormal") return 2;
    if (name == "Rare") return 3;
    if (name == "VeryRare") return 4;
    if (name == "SuperRare") return 5;
    return 0;
}

// AvatarSkillTreeConfig.AnchorType is "Point07"; the client wants the number.
uint32_t anchorNumber(const std::string& anchor) {
    size_t i = 0;
    while (i < anchor.size() && (anchor[i] < '0' || anchor[i] > '9')) ++i;
    if (i >= anchor.size()) return 0;
    return static_cast<uint32_t>(std::strtoul(anchor.c_str() + i, nullptr, 10));
}

// StageConfig.MonsterList is [{"Monster0":id,...}] in production dumps and [[id,...]]
// in the beta extracts. Either way, one entry per wave.
std::vector<std::vector<uint32_t>> readWaves(const json& row) {
    std::vector<std::vector<uint32_t>> waves;
    auto it = row.find("MonsterList");
    if (it == row.end() || !it->is_array()) return waves;
    for (const auto& wave : *it) {
        std::vector<uint32_t> ids;
        if (wave.is_array()) {
            for (const auto& e : wave) {
                double v = numberOf(e);
                if (v > 0) ids.push_back(static_cast<uint32_t>(v));
            }
        } else if (wave.is_object()) {
            for (int slot = 0; slot < 8; ++slot) {
                auto m = wave.find("Monster" + std::to_string(slot));
                if (m == wave.end()) continue;
                double v = numberOf(*m);
                if (v > 0) ids.push_back(static_cast<uint32_t>(v));
            }
        }
        if (!ids.empty()) waves.push_back(std::move(ids));
    }
    return waves;
}

bool readTable(const std::string& dir, const char* name, json& doc) {
    bool ok = false;
    std::string path = dir + "/" + name;
    std::string text = util::readFile(path, &ok);
    if (!ok) return false;
    try {
        doc = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        logging::warn("data", "{} is not valid json: {}", path, e.what());
        return false;
    }
    return true;
}

}  // namespace

uint32_t attackBuffForElement(const std::string& damageType) {
    // 1000111..1000117, in the order the client numbers its element entry buffs.
    static const char* kOrder[] = {"Physical", "Fire",    "Ice",       "Thunder",
                                   "Wind",     "Quantum", "Imaginary"};
    for (uint32_t i = 0; i < 7; ++i) {
        if (damageType == kOrder[i]) return 1000111 + i;
    }
    return 0;
}

uint32_t propStateFromName(const std::string& name) {
    static const std::pair<const char*, uint32_t> kStates[] = {
        {"Closed", 0},          {"Open", 1},          {"Locked", 2},
        {"BridgeState1", 3},    {"BridgeState2", 4},  {"BridgeState3", 5},
        {"BridgeState4", 6},    {"CheckPointDisable", 7}, {"CheckPointEnable", 8},
        {"TriggerDisable", 9},  {"TriggerEnable", 10}, {"ChestLocked", 11},
        {"ChestClosed", 12},    {"ChestUsed", 13},    {"Elevator1", 14},
        {"Elevator2", 15},      {"Elevator3", 16},    {"WaitActive", 17},
        {"EventClose", 18},     {"EventOpen", 19},    {"Hidden", 20},
        {"TeleportGate0", 21},  {"TeleportGate1", 22}, {"TeleportGate2", 23},
        {"TeleportGate3", 24},  {"Destructed", 25},   {"CustomState01", 101},
        {"CustomState02", 102}, {"CustomState03", 103}, {"CustomState04", 104},
        {"CustomState05", 105}, {"CustomState06", 106}, {"CustomState07", 107},
        {"CustomState08", 108}, {"CustomState09", 109},
    };
    for (const auto& [text, value] : kStates) {
        if (name == text) return value;
    }
    return 0;
}

uint32_t planeTypeFromName(const std::string& name) {
    if (name == "Town") return 1;
    if (name == "Maze") return 2;
    if (name == "Train") return 3;
    if (name == "Challenge") return 4;
    if (name == "Rogue") return 5;
    if (name == "Raid") return 6;
    if (name == "AetherDivide") return 7;
    if (name == "TrialActivity") return 8;
    return 0;
}

Tables& Tables::get() {
    static Tables instance;
    return instance;
}

const AvatarInfo* Tables::avatar(uint32_t id) const {
    auto it = avatars_.find(id);
    return it == avatars_.end() ? nullptr : &it->second;
}

const LightconeInfo* Tables::lightcone(uint32_t id) const {
    auto it = lightcones_.find(id);
    return it == lightcones_.end() ? nullptr : &it->second;
}

uint32_t Tables::gachaUpChance(GachaType type) const {
    switch (type) {
        case GachaType::AvatarUp:
            return avatarUpChance_;
        case GachaType::WeaponUp:
            return weaponUpChance_;
        case GachaType::Normal:
            break;
    }
    return 0;
}

const FarmElementInfo* Tables::farmElement(uint32_t stageId) const {
    auto it = farmElementsByStage_.find(stageId);
    return it == farmElementsByStage_.end() ? nullptr : &it->second;
}

const ItemInfo* Tables::item(uint32_t id) const {
    auto it = items_.find(id);
    return it == items_.end() ? nullptr : &it->second;
}

const MappingInfo* Tables::mappingInfo(uint32_t id, uint32_t worldLevel) const {
    auto it = mappingInfos_.find(uint64_t{id} * 10 + worldLevel);
    return it == mappingInfos_.end() ? nullptr : &it->second;
}

const RewardInfo* Tables::reward(uint32_t id) const {
    auto it = rewards_.find(id);
    return it == rewards_.end() ? nullptr : &it->second;
}

const std::vector<ChallengeRewardLine>* Tables::challengeRewardLine(uint32_t rewardLineGroupId) const {
    auto it = challengeRewardLines_.find(rewardLineGroupId);
    return it == challengeRewardLines_.end() ? nullptr : &it->second;
}

const StageInfo* Tables::stage(uint32_t id) const {
    auto it = stages_.find(id);
    return it == stages_.end() ? nullptr : &it->second;
}

const MonsterInfo* Tables::monster(uint32_t id) const {
    auto it = monsters_.find(id);
    return it == monsters_.end() ? nullptr : &it->second;
}

const PlaneInfo* Tables::plane(uint32_t id) const {
    auto it = planes_.find(id);
    return it == planes_.end() ? nullptr : &it->second;
}

const EntranceInfo* Tables::entrance(uint32_t id) const {
    auto it = entrances_.find(id);
    return it == entrances_.end() ? nullptr : &it->second;
}

const CocoonInfo* Tables::cocoon(uint32_t id, uint32_t worldLevel) const {
    auto it = cocoons_.find(id * 100 + worldLevel);
    if (it != cocoons_.end()) return &it->second;
    // Not every calyx is defined at every world level; fall back to the highest one
    // that is, then to the world-level-less row.
    for (uint32_t level = worldLevel; level > 0; --level) {
        auto alt = cocoons_.find(id * 100 + level);
        if (alt != cocoons_.end()) return &alt->second;
    }
    auto base = cocoons_.find(id * 100);
    return base == cocoons_.end() ? nullptr : &base->second;
}

namespace {

template <class T>
const T* findById(const std::vector<T>& sorted, uint32_t id) {
    auto it = std::lower_bound(sorted.begin(), sorted.end(), id,
                               [](const T& row, uint32_t key) { return row.id < key; });
    return (it == sorted.end() || it->id != id) ? nullptr : &*it;
}

}  // namespace

const ChallengeInfo* Tables::challenge(uint32_t id) const { return findById(challenges_, id); }

const PeakGroupInfo* Tables::peakGroup(uint32_t id) const { return findById(peakGroups_, id); }

const PeakInfo* Tables::peak(uint32_t id) const {
    auto it = peaks_.find(id);
    return it == peaks_.end() ? nullptr : &it->second;
}

const std::vector<uint32_t>* Tables::peakRewards(uint32_t rewardGroupId) const {
    auto it = peakRewards_.find(rewardGroupId);
    return it == peakRewards_.end() ? nullptr : &it->second;
}

const BattleTargetInfo* Tables::battleTarget(uint32_t id) const {
    auto it = battleTargets_.find(id);
    return it == battleTargets_.end() ? nullptr : &it->second;
}

const ChallengeGroupInfo* Tables::challengeGroup(uint32_t groupId) const {
    return findById(challengeGroups_, groupId);
}

const ChallengeTarget* Tables::challengeTarget(uint32_t id) const {
    auto it = challengeTargets_.find(id);
    return it == challengeTargets_.end() ? nullptr : &it->second;
}

const ChallengeTierceInfo* Tables::challengeTierce(uint32_t id) const {
    auto it = challengeTierces_.find(id);
    return it == challengeTierces_.end() ? nullptr : &it->second;
}

const ChallengeTierceInfo* Tables::challengeTierceFor(uint32_t challengeId) const {
    for (const auto& [id, info] : challengeTierces_) {
        if (info.preChallengeId == challengeId) return &info;
    }
    return nullptr;
}

uint64_t Tables::challengeRewardStars(uint32_t rewardLineGroupId) const {
    auto it = challengeRewardStars_.find(rewardLineGroupId);
    return it == challengeRewardStars_.end() ? 0 : it->second;
}

const std::string* Tables::monsterRank(uint32_t templateId) const {
    auto it = monsterRanks_.find(templateId);
    return it == monsterRanks_.end() ? nullptr : &it->second;
}

const MultiPathInfo* Tables::multiPath(uint32_t avatarId) const {
    auto it = multiPaths_.find(avatarId);
    return it == multiPaths_.end() ? nullptr : &it->second;
}

const InteractInfo* Tables::interact(uint32_t interactId) const {
    auto it = interacts_.find(interactId);
    return it == interacts_.end() ? nullptr : &it->second;
}

uint32_t Tables::baseAvatarId(uint32_t avatarId) const {
    auto it = multiPaths_.find(avatarId);
    return it == multiPaths_.end() ? avatarId : it->second.baseAvatarId;
}

const std::vector<uint32_t>* Tables::multiPathVariants(uint32_t baseAvatarId) const {
    auto it = multiPathVariants_.find(baseAvatarId);
    return it == multiPathVariants_.end() ? nullptr : &it->second;
}

const std::vector<SkillTreePoint>* Tables::skillTree(uint32_t avatarId) const {
    auto it = skillTrees_.find(avatarId);
    return it == skillTrees_.end() ? nullptr : &it->second;
}

uint32_t Tables::stageForEvent(uint32_t eventId, uint32_t worldLevel) const {
    uint64_t base = static_cast<uint64_t>(eventId) * 10;
    auto it = planeEvents_.find(base + worldLevel);
    if (it != planeEvents_.end()) return it->second;
    // Not every event is defined at every world level; take the closest one below.
    for (uint32_t level = 6; level > 0; --level) {
        auto alt = planeEvents_.find(base + level);
        if (alt != planeEvents_.end()) return alt->second;
    }
    auto zero = planeEvents_.find(base);
    return zero == planeEvents_.end() ? 0 : zero->second;
}

uint32_t Tables::farmElementStage(uint32_t idOrStage, uint32_t worldLevel) const {
    if (farmStages_.count(idOrStage) != 0) return idOrStage;
    uint64_t base = static_cast<uint64_t>(idOrStage) * 100;
    auto it = farmElements_.find(base + worldLevel);
    if (it != farmElements_.end()) return it->second;
    for (uint32_t level = 7; level-- > 0;) {
        auto alt = farmElements_.find(base + level);
        if (alt != farmElements_.end()) return alt->second;
    }
    return 0;
}

bool Tables::load(const std::vector<std::string>& sources) {
    // Rows are merged, never replaced, so loading twice would only double the counts.
    if (loaded_) return true;

    uint64_t started = util::nowMs();
    size_t used = 0;
    bool standardGachaFound = false;
    std::unordered_map<std::string, uint32_t> upChances;

    for (const std::string& dir : sources) {
        json doc;
        std::vector<const json*> rows;
        bool any = false;

        // Rows stay owned by `doc`, so every loop below has to finish before the
        // next table replaces it.
        auto table = [&](const char* name) -> const std::vector<const json*>& {
            rows.clear();
            doc = json();
            if (readTable(dir, name, doc)) collectRows(doc, rows);
            if (!rows.empty()) any = true;
            return rows;
        };

        for (const json* row : table("AvatarConfig.json")) {
            uint32_t id = u32(*row, "AvatarID");
            if (id == 0 || avatars_.count(id) != 0) continue;
            AvatarInfo info;
            info.id = id;
            info.damageType = str(*row, "DamageType");
            info.baseType = str(*row, "AvatarBaseType");
            info.maxPromotion = u32(*row, "MaxPromotion", 6);
            info.maxRank = u32(*row, "MaxRank", 6);
            info.rarity = rarityOf(*row);
            // SPNeed is in whole points; SpBarInfo counts hundredths.
            uint32_t spNeed = u32(*row, "SPNeed");
            info.spNeed = spNeed != 0 ? spNeed * 100 : 10000;
            info.attackBuffId = attackBuffForElement(info.damageType);
            avatars_.emplace(id, std::move(info));
        }

        for (const json* row : table("AvatarConfigEnhanced.json")) {
            auto it = avatars_.find(u32(*row, "AvatarID"));
            if (it == avatars_.end() || it->second.enhancedId != 0) continue;
            it->second.enhancedId = u32(*row, "EnhancedID");
        }

        for (const json* row : table("AvatarDefaultMazeBuff.json")) {
            auto it = avatars_.find(u32(*row, "ID"));
            if (it == avatars_.end() || !it->second.techniqueBuffs.empty()) continue;
            it->second.techniqueBuffs = u32List(*row, "DefaultMazeBuffIDList");
            it->second.techniqueSkillIndex = u32(*row, "SkillIndex", 2);
        }

        for (const json* row : table("AvatarMazeBuff.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0 || str(*row, "ModifierName").rfind("ADV_GlobalSkill_Maze", 0) != 0) continue;
            if (std::find(globalMazeBuffs_.begin(), globalMazeBuffs_.end(), id) ==
                globalMazeBuffs_.end()) {
                globalMazeBuffs_.push_back(id);
            }
        }

        for (const json* row : table("MultiplePathAvatarConfig.json")) {
            uint32_t id = u32(*row, "AvatarID");
            uint32_t base = u32(*row, "BaseAvatarID");
            if (id == 0 || base == 0 || multiPaths_.count(id) != 0) continue;
            MultiPathInfo info;
            info.avatarId = id;
            info.baseAvatarId = base;
            info.gender = str(*row, "Gender");
            multiPaths_.emplace(id, std::move(info));
            auto& variants = multiPathVariants_[base];
            if (std::find(variants.begin(), variants.end(), id) == variants.end()) {
                variants.push_back(id);
            }
        }

        for (const json* row : table("AvatarSkillTreeConfig.json")) {
            uint32_t avatarId = u32(*row, "AvatarID");
            uint32_t pointId = u32(*row, "PointID");
            if (avatarId == 0 || pointId == 0) continue;
            auto& points = skillTrees_[avatarId];
            // The table holds one row per level; keep the first and its table max.
            bool seen = std::any_of(points.begin(), points.end(), [&](const SkillTreePoint& p) {
                return p.pointId == pointId;
            });
            if (seen) continue;
            SkillTreePoint point;
            point.pointId = pointId;
            point.avatarId = avatarId;
            point.level = u32(*row, "Level", 1);
            point.maxLevel = u32(*row, "MaxLevel", 1);
            point.anchorType = anchorNumber(str(*row, "AnchorType"));
            point.defaultUnlock = boolOf(*row, "DefaultUnlock");
            points.push_back(point);
            ++skillPointCount_;
        }

        for (const json* row : table("StageConfig.json")) {
            uint32_t id = u32(*row, "StageID");
            if (id == 0 || stages_.count(id) != 0) continue;
            StageInfo stage;
            stage.id = id;
            stage.level = u32(*row, "Level", 1);
            stage.eliteGroup = u32(*row, "EliteGroup");
            stage.hardLevelGroup = u32(*row, "HardLevelGroup", 1);
            stage.waves = readWaves(*row);
            stages_.emplace(id, std::move(stage));
        }

        for (const json* row : table("MonsterConfig.json")) {
            uint32_t id = u32(*row, "MonsterID");
            if (id == 0 || monsters_.count(id) != 0) continue;
            MonsterInfo monster;
            monster.id = id;
            monster.templateId = u32(*row, "MonsterTemplateID");
            monster.eliteGroup = u32(*row, "EliteGroup");
            monster.hardLevelGroup = u32(*row, "HardLevelGroup", 1);
            monsters_.emplace(id, monster);
        }

        for (const json* row : table("MonsterTemplateConfig.json")) {
            uint32_t id = u32(*row, "MonsterTemplateID");
            std::string rank = str(*row, "Rank");
            if (id == 0 || rank.empty() || monsterRanks_.count(id) != 0) continue;
            monsterRanks_.emplace(id, std::move(rank));
        }

        for (const json* row : table("MazePlane.json")) {
            uint32_t id = u32(*row, "PlaneID");
            if (id == 0 || planes_.count(id) != 0) continue;
            PlaneInfo plane;
            plane.planeId = id;
            plane.worldId = u32(*row, "WorldID");
            plane.planeType = planeTypeFromName(str(*row, "PlaneType"));
            plane.startFloorId = u32(*row, "StartFloorID");
            plane.floorIds = u32List(*row, "FloorIDList");
            planes_.emplace(id, std::move(plane));
        }

        for (const json* row : table("MapEntrance.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0 || entrances_.count(id) != 0) continue;
            EntranceInfo entrance;
            entrance.id = id;
            entrance.planeId = u32(*row, "PlaneID");
            entrance.floorId = u32(*row, "FloorID");
            entrance.startGroupId = u32(*row, "StartGroupID");
            entrance.startAnchorId = u32(*row, "StartAnchorID");
            entrances_.emplace(id, entrance);
        }

        for (const json* row : table("CocoonConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0) continue;
            uint32_t worldLevel = u32(*row, "WorldLevel");
            uint32_t key = id * 100 + worldLevel;
            if (cocoons_.count(key) != 0) continue;
            CocoonInfo cocoon;
            cocoon.id = id;
            cocoon.worldLevel = worldLevel;
            cocoon.propId = u32(*row, "PropID");
            cocoon.staminaCost = u32(*row, "StaminaCost");
            cocoon.mappingInfoId = u32(*row, "MappingInfoID");
            cocoon.stageIds = u32List(*row, "StageIDList");
            if (cocoon.stageIds.empty()) {
                uint32_t single = u32(*row, "StageID");
                if (single != 0) cocoon.stageIds.push_back(single);
            }
            cocoons_.emplace(key, std::move(cocoon));
        }

        for (const json* row : table("FarmElementConfig.json")) {
            uint32_t id = u32(*row, "ID");
            uint32_t stageId = u32(*row, "StageID");
            if (id == 0 || stageId == 0) continue;
            farmElements_.emplace(static_cast<uint64_t>(id) * 100 + u32(*row, "WorldLevel"),
                                  stageId);
            farmStages_.insert(stageId);
            FarmElementInfo element;
            element.id = id;
            element.worldLevel = u32(*row, "WorldLevel");
            element.stageId = stageId;
            element.staminaCost = u32(*row, "StaminaCost");
            element.mappingInfoId = u32(*row, "MappingInfoID");
            farmElementsByStage_.emplace(stageId, element);
        }

        for (const json* row : table("ItemConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0 || items_.count(id) != 0) continue;
            ItemInfo item;
            item.id = id;
            item.mainType = str(*row, "ItemMainType");
            item.subType = str(*row, "ItemSubType");
            item.rarity = itemRarity(str(*row, "Rarity"));
            item.purposeType = u32(*row, "PurposeType");
            item.pileLimit = u32(*row, "PileLimit");
            items_.emplace(id, std::move(item));
        }

        for (const json* row : table("MappingInfo.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0) continue;
            MappingInfo info;
            info.id = id;
            info.worldLevel = u32(*row, "WorldLevel");
            uint64_t key = uint64_t{id} * 10 + info.worldLevel;
            if (mappingInfos_.count(key) != 0) continue;
            std::string farm = str(*row, "FarmType");
            info.farmType = farm == "COCOON"                        ? 1
                            : farm == "COCOON2" || farm == "ELEMENT" ? 3
                            : farm == "RELIC"                        ? 4
                                                                     : 0;
            if (auto list = row->find("DisplayItemList"); list != row->end() && list->is_array()) {
                for (const json& entry : *list) {
                    uint32_t item = u32(entry, "ItemID");
                    if (item != 0) info.display.push_back({item, u32(entry, "ItemNum")});
                }
            }
            mappingInfos_.emplace(key, std::move(info));
        }

        for (const json* row : table("RewardData.json")) {
            uint32_t id = u32(*row, "RewardID");
            if (id == 0 || rewards_.count(id) != 0) continue;
            RewardInfo reward;
            reward.id = id;
            reward.hcoin = u32(*row, "Hcoin");
            for (int n = 1; n <= 6; ++n) {
                std::string suffix = std::to_string(n);
                uint32_t item = u32(*row, ("ItemID_" + suffix).c_str());
                uint32_t count = u32(*row, ("Count_" + suffix).c_str());
                if (item != 0 && count != 0) reward.items.push_back({item, count});
            }
            rewards_.emplace(id, std::move(reward));
        }

        for (const json* row : table("ConstValueCommon.json")) {
            auto value = row->find("Value");
            if (value == row->end() || !value->is_object()) continue;
            uint32_t n = u32(*value, "IntValue");
            if (n == 0) continue;
            std::string name = str(*row, "ConstValueName");
            if (name == "Stamina_Maximum_Num") staminaRules_.max = n;
            if (name == "Stamina_Auto_Recover_Interval") staminaRules_.recoverSeconds = n;
            if (name == "ReserveStamina_Maximum_Num") staminaRules_.reserveMax = n;
            if (name == "ReserveStamina_Auto_Recover_Interval") staminaRules_.reserveRecoverSeconds = n;
        }

        std::vector<std::pair<uint32_t, uint32_t>> sales;  // purchase number -> jade
        for (const json* row : table("StaminaSaleConfig.json")) {
            uint32_t times = u32(*row, "Times");
            auto price = row->find("Price");
            if (times == 0 || price == row->end() || !price->is_object()) continue;
            sales.emplace_back(times, u32(*price, "1"));
            if (uint32_t to = u32(*row, "ToStamina"); to != 0) staminaPerPurchase_ = to;
        }
        if (!sales.empty() && staminaPrices_.empty()) {
            std::sort(sales.begin(), sales.end());
            for (const auto& [times, jade] : sales) staminaPrices_.push_back(jade);
        }

        for (const json* row : table("GachaTypeBasicInfo.json")) {
            std::string type = str(*row, "GachaTypeID");
            if (!type.empty()) upChances.emplace(type, u32(*row, "UpPropability"));
        }
        // The client throws on a pool its own table lacks, and the beta table is a six-row
        // stub, so here the last source with the table wins instead of the first.
        const std::vector<const json*>& gachaRows = table("GachaBasicInfo.json");
        if (!gachaRows.empty()) {
            gachaPools_.clear();
            standardGachaFound = false;
        }
        for (const json* row : gachaRows) {
            uint32_t id = u32(*row, "GachaID");
            if (id == 0) continue;
            std::string type = str(*row, "GachaType");
            GachaPool pool;
            pool.id = id;
            if (type == "Normal") {
                if (standardGachaFound && id != standardGacha_.gachaId) continue;
                standardGacha_.gachaId = id;
                standardGachaFound = true;
            } else if (type == "AvatarUp") {
                pool.type = GachaType::AvatarUp;
                pool.featured = featuredFromPrefab(str(*row, "PrefabPath"), "AvatarGacha_");
            } else if (type == "WeaponUp") {
                pool.type = GachaType::WeaponUp;
                pool.featured = featuredFromPrefab(str(*row, "PrefabPath"), "LightConeGacha_");
            } else {
                continue;
            }
            if (pool.type != GachaType::Normal && pool.featured == 0) continue;
            gachaPools_.push_back(pool);
        }

        for (const json* row : table("EquipmentConfig.json")) {
            uint32_t id = u32(*row, "EquipmentID");
            if (id == 0 || lightcones_.count(id) != 0) continue;
            lightcones_.emplace(id, LightconeInfo{id, rarityOf(*row)});
        }
        for (const json* row : table("BattlePassReward.json")) {
            uint32_t item = u32(*row, "RewardItem");
            if (item != 0) battlePassRewards_.insert(item);
        }
        for (const json* row : table("GachaCeiling.json")) {
            if (!standardGacha_.ceilingAvatars.empty() || str(*row, "GachaType") != "Normal") {
                continue;
            }
            standardGacha_.ceilingNum = u32(*row, "CeilingNum");
            standardGacha_.ceilingAvatars = u32List(*row, "CeilingItemList");
        }

        // Id-only tables; the first source to list an id wins, as everywhere else.
        auto ids = [&](const char* file, const char* key, std::vector<uint32_t>& into) {
            for (const json* row : table(file)) {
                uint32_t id = u32(*row, key);
                if (id == 0) continue;
                if (std::find(into.begin(), into.end(), id) == into.end()) into.push_back(id);
            }
        };
        // The client logs "can't find MainMissionRow" for a mission its own table lacks, and
        // the beta table is the client's, so the last source with this table wins.
        const std::vector<const json*>& missionRows = table("MainMission.json");
        if (!missionRows.empty()) mainMissions_.clear();
        for (const json* row : missionRows) {
            uint32_t id = u32(*row, "MainMissionID");
            if (id == 0) continue;
            if (std::find(mainMissions_.begin(), mainMissions_.end(), id) == mainMissions_.end()) {
                mainMissions_.push_back(id);
            }
        }
        ids("TutorialData.json", "TutorialID", tutorials_);
        ids("TutorialGuideGroup.json", "GroupID", tutorialGuides_);
        ids("QuestData.json", "QuestID", quests_);

        // Memory of Chaos, Pure Fiction and Apocalyptic Shadow, in that order. Each
        // mode keeps its floors, its seasons and its reward line in its own table but
        // the client reads all three from one response.
        auto challengeFloors = [&](const char* file, ChallengeKind kind) {
            for (const json* row : table(file)) {
                uint32_t id = u32(*row, "ID");
                if (id == 0) continue;
                ChallengeInfo info;
                info.id = id;
                info.kind = kind;
                info.groupId = u32(*row, "GroupID");
                info.floor = u32(*row, "Floor");
                info.stageNum = u32(*row, "StageNum", 1);
                info.mapEntranceId = u32(*row, "MapEntranceID");
                info.mapEntranceId2 = u32(*row, "MapEntranceID2");
                info.mazeBuffId = u32(*row, "MazeBuffID");
                info.roundLimit = u32(*row, "ChallengeCountDown");
                info.targetIds = u32List(*row, "ChallengeTargetID");
                // Two halves, each its own scene group and its own set of monsters.
                // ConfigList names the placeholder in the dump, NpcMonsterIDList what
                // really stands there and EventIDList the fight it starts.
                for (uint32_t half = 0; half < 2; ++half) {
                    std::string suffix = std::to_string(half + 1);
                    ChallengeStage& stage = info.stages[half];
                    stage.mazeGroupId = u32(*row, ("MazeGroupID" + suffix).c_str());
                    std::vector<uint32_t> configs = u32List(*row, ("ConfigList" + suffix).c_str());
                    std::vector<uint32_t> npcs = u32List(*row, ("NpcMonsterIDList" + suffix).c_str());
                    std::vector<uint32_t> events = u32List(*row, ("EventIDList" + suffix).c_str());
                    for (size_t i = 0; i < configs.size(); ++i) {
                        ChallengeMonster monster;
                        monster.configId = configs[i];
                        monster.npcMonsterId = i < npcs.size() ? npcs[i] : 0;
                        monster.eventId = i < events.size() ? events[i] : 0;
                        stage.monsters.push_back(monster);
                    }
                }
                challenges_.push_back(std::move(info));
            }
        };
        challengeFloors("ChallengeMazeConfig.json", ChallengeKind::Memory);
        challengeFloors("ChallengeStoryMazeConfig.json", ChallengeKind::Story);
        challengeFloors("ChallengeBossMazeConfig.json", ChallengeKind::Boss);

        // The third node, beta-only so far. GNOOAGPBNLD is the floor-wide cycle limit;
        // only the Memory rows carry one.
        for (const json* row : table("ChallengeMazeTierceConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0 || challengeTierces_.count(id) != 0) continue;
            ChallengeTierceInfo info;
            info.id = id;
            info.preChallengeId = u32(*row, "PreChallengeMazeID");
            info.mapEntranceId = u32(*row, "MapEntranceID");
            info.mazeGroupId = u32(*row, "MazeGroupID");
            info.roundLimit = u32(*row, "GNOOAGPBNLD");
            info.targetIds = u32List(*row, "ChallengeTargetID");
            std::vector<uint32_t> configs = u32List(*row, "ConfigList");
            std::vector<uint32_t> npcs = u32List(*row, "NpcMonsterIDList");
            std::vector<uint32_t> events = u32List(*row, "EventIDList");
            for (size_t i = 0; i < configs.size(); ++i) {
                ChallengeMonster monster;
                monster.configId = configs[i];
                monster.npcMonsterId = i < npcs.size() ? npcs[i] : 0;
                monster.eventId = i < events.size() ? events[i] : 0;
                info.monsters.push_back(monster);
            }
            if (info.preChallengeId != 0) challengeTierces_.emplace(id, std::move(info));
        }

        // Pure Fiction keeps its turn limit and its score targets one table over.
        for (const json* row : table("ChallengeStoryMazeExtra.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0) continue;
            challengeExtras_[id] = {u32(*row, "TurnLimit"), u32List(*row, "BattleTargetID")};
        }

        auto challengeSeasons = [&](const char* file, ChallengeKind kind) {
            for (const json* row : table(file)) {
                uint32_t id = u32(*row, "GroupID");
                if (id == 0) continue;
                challengeGroups_.push_back({id, u32(*row, "RewardLineGroupID"), kind});
            }
        };
        challengeSeasons("ChallengeGroupConfig.json", ChallengeKind::Memory);
        challengeSeasons("ChallengeStoryGroupConfig.json", ChallengeKind::Story);
        challengeSeasons("ChallengeBossGroupConfig.json", ChallengeKind::Boss);

        auto challengeTargets = [&](const char* file) {
            for (const json* row : table(file)) {
                uint32_t id = u32(*row, "ID");
                if (id == 0 || challengeTargets_.count(id) != 0) continue;
                ChallengeTarget target;
                target.id = id;
                std::string kind = str(*row, "ChallengeTargetType");
                target.kind = kind == "DEAD_AVATAR"  ? ChallengeTarget::Kind::DeadAvatar
                              : kind == "TOTAL_SCORE" ? ChallengeTarget::Kind::TotalScore
                                                      : ChallengeTarget::Kind::RoundsLeft;
                target.param = u32(*row, "ChallengeTargetParam1");
                challengeTargets_.emplace(id, target);
            }
        };
        challengeTargets("ChallengeTargetConfig.json");
        challengeTargets("ChallengeStoryTargetConfig.json");
        challengeTargets("ChallengeBossTargetConfig.json");

        // GroupID here is the reward line's own id, not a season's.
        auto challengeRewards = [&](const char* file) {
            for (const json* row : table(file)) {
                uint32_t line = u32(*row, "GroupID");
                uint32_t stars = u32(*row, "StarCount");
                if (line == 0 || stars == 0 || stars >= 64) continue;
                uint64_t bit = uint64_t{1} << stars;
                if ((challengeRewardStars_[line] & bit) != 0) continue;
                challengeRewardStars_[line] |= bit;
                challengeRewardLines_[line].push_back({stars, u32(*row, "RewardID")});
            }
        };
        challengeRewards("ChallengeMazeRewardLine.json");
        challengeRewards("ChallengeStoryRewardLine.json");
        challengeRewards("ChallengeBossRewardLine.json");

        // Anomaly Arbitration. Only the beta dump places the fights -- production rows
        // have no arena columns -- so a later source fills what an earlier one left
        // empty rather than losing the row to it. The last entry of each list is the
        // encounter itself.
        for (const json* row : table("ChallengePeakConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0) continue;
            PeakInfo& peak = peaks_[id];
            peak.id = id;
            if (peak.mapEntranceId == 0) peak.mapEntranceId = u32(*row, "MapEntranceID");
            if (peak.mazeGroupId == 0) peak.mazeGroupId = u32(*row, "MazeGroupID");
            if (peak.npcMonsterId == 0) {
                std::vector<uint32_t> npcs = u32List(*row, "NpcMonsterIDList");
                if (!npcs.empty()) peak.npcMonsterId = npcs.back();
            }
            if (peak.eventId == 0) {
                std::vector<uint32_t> events = u32List(*row, "EventIDList");
                if (!events.empty()) peak.eventId = events.back();
            }
            if (peak.targetIds.empty()) peak.targetIds = u32List(*row, "NormalTargetList");
            if (peak.tagBuffs.empty()) peak.tagBuffs = u32List(*row, "TagList");
        }
        for (const json* row : table("ChallengePeakBossConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0) continue;
            PeakInfo& peak = peaks_[id];
            if (peak.boss) continue;
            peak.id = id;
            peak.boss = true;
            peak.bossBuffs = u32List(*row, "BuffList");
            peak.hardTarget = u32(*row, "HardTarget");
            std::vector<uint32_t> events = u32List(*row, "HardEventIDList");
            if (!events.empty()) peak.hardEventId = events.back();
            peak.hardTagBuffs = u32List(*row, "HardTagList");
        }
        for (const json* row : table("ChallengePeakGroupConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0) continue;
            peakGroups_.push_back({id, u32List(*row, "PreLevelIDList"), u32(*row, "BossLevelID"),
                                   u32(*row, "RewardGroupID")});
        }
        for (const json* row : table("ChallengePeakReward.json")) {
            uint32_t id = u32(*row, "ID");
            uint32_t group = u32(*row, "RewardGroupID");
            if (id == 0 || group == 0) continue;
            std::vector<uint32_t>& ids = peakRewards_[group];
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
        }
        for (const json* row : table("BattleTargetConfig.json")) {
            uint32_t id = u32(*row, "ID");
            if (id == 0 || battleTargets_.count(id) != 0) continue;
            bool deaths = str(*row, "AbilityName").find("DeathCount") != std::string::npos;
            battleTargets_.emplace(id, BattleTargetInfo{id, u32(*row, "TargetParam"), deaths});
        }

        for (const json* row : table("InteractConfig.json")) {
            uint32_t id = u32(*row, "InteractID");
            if (id == 0 || interacts_.count(id) != 0) continue;
            InteractInfo info;
            info.id = id;
            std::string src = str(*row, "SrcState");
            info.anySource = src.empty();
            info.srcState = propStateFromName(src);
            info.targetState = propStateFromName(str(*row, "TargetState"));
            interacts_.emplace(id, info);
        }

        for (const json* row : table("PlaneEvent.json")) {
            uint32_t eventId = u32(*row, "EventID");
            uint32_t stageId = u32(*row, "StageID");
            if (eventId == 0 || stageId == 0) continue;
            planeEvents_.emplace(static_cast<uint64_t>(eventId) * 10 + u32(*row, "WorldLevel"),
                                 stageId);
        }

        rows.clear();
        doc = json();
        if (any) {
            ++used;
        } else {
            logging::warn("data", "no tables found under {}", dir);
        }
    }

    // Merged across sources like every other table -- stable_sort so the earlier source
    // still wins the id, and the client gets the floors in order.
    auto sortById = [](auto& rows) {
        std::stable_sort(rows.begin(), rows.end(),
                         [](const auto& a, const auto& b) { return a.id < b.id; });
        rows.erase(std::unique(rows.begin(), rows.end(),
                               [](const auto& a, const auto& b) { return a.id == b.id; }),
                   rows.end());
    };
    sortById(challenges_);
    sortById(challengeGroups_);
    sortById(peakGroups_);
    sortById(gachaPools_);

    // The defaults are GachaTypeBasicInfo's own numbers, for when the table is missing.
    if (auto it = upChances.find("AvatarUp"); it != upChances.end()) avatarUpChance_ = it->second;
    if (auto it = upChances.find("WeaponUp"); it != upChances.end()) weaponUpChance_ = it->second;
    for (GachaPool& pool : gachaPools_) pool.upChance = gachaUpChance(pool.type);
    // A featured 5* the tables do not have cannot be drawn.
    std::erase_if(gachaPools_, [&](const GachaPool& pool) {
        if (pool.type == GachaType::AvatarUp) {
            const AvatarInfo* info = avatar(pool.featured);
            return info == nullptr || info->rarity != 5;
        }
        if (pool.type == GachaType::WeaponUp) {
            const LightconeInfo* info = lightcone(pool.featured);
            return info == nullptr || info->rarity != 5;
        }
        return false;
    });

    // Each fight learns its season, and plants its one monster over the arena's marker.
    for (const PeakGroupInfo& group : peakGroups_) {
        for (uint32_t id : group.mobIds) {
            if (auto it = peaks_.find(id); it != peaks_.end()) it->second.groupId = group.id;
        }
        if (auto it = peaks_.find(group.bossId); it != peaks_.end()) it->second.groupId = group.id;
    }
    for (auto& [id, peak] : peaks_) {
        if (peak.eventId != 0) peak.monsters = {{kPeakMarkerId, peak.npcMonsterId, peak.eventId}};
        if (peak.hardEventId != 0) {
            peak.hardMonsters = {{kPeakMarkerId, peak.npcMonsterId, peak.hardEventId}};
        }
    }

    for (ChallengeInfo& floor : challenges_) {
        auto extra = challengeExtras_.find(floor.id);
        if (extra == challengeExtras_.end()) continue;
        if (extra->second.turnLimit != 0) floor.roundLimit = extra->second.turnLimit;
        floor.battleTargetIds = extra->second.battleTargetIds;
    }
    challengeExtras_.clear();
    mainMissionIds_.insert(mainMissions_.begin(), mainMissions_.end());

    loaded_ = !avatars_.empty() && !stages_.empty();
    if (!loaded_) {
        logging::warn("data", "no game tables loaded -- check paths.data_sources in config.json");
        return false;
    }
    logging::info("data",
                  "{} avatars, {} skill tree points, {} stages, {} monsters, {} entrances, "
                  "{} plane events from {} source(s) in {} ms",
                  avatars_.size(), skillPointCount_, stages_.size(), monsters_.size(),
                  entrances_.size(), planeEvents_.size(), used, util::nowMs() - started);
    logging::info("data", "{} challenge floors in {} seasons, {} of them with a third node",
                  challenges_.size(), challengeGroups_.size(), challengeTierces_.size());
    logging::info("data", "{} lightcones, {} warp pools", lightcones_.size(), gachaPools_.size());
    logging::info("data", "{} items, {} drop tables, {} rewards", items_.size(), mappingInfos_.size(),
                  rewards_.size());
    logging::info("data", "{} main missions, {} tutorials, {} guides, {} quests",
                  mainMissions_.size(), tutorials_.size(), tutorialGuides_.size(),
                  quests_.size());
    return true;
}

}  // namespace data
