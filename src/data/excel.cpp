#include "data/excel.h"

#include <algorithm>
#include <cstdlib>

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

const ChallengeGroupInfo* Tables::challengeGroup(uint32_t groupId) const {
    return findById(challengeGroups_, groupId);
}

const ChallengeTarget* Tables::challengeTarget(uint32_t id) const {
    auto it = challengeTargets_.find(id);
    return it == challengeTargets_.end() ? nullptr : &it->second;
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
        }

        for (const json* row : table("GachaBasicInfo.json")) {
            if (standardGachaFound || str(*row, "GachaType") != "Normal") continue;
            uint32_t id = u32(*row, "GachaID");
            if (id == 0) continue;
            standardGacha_.gachaId = id;
            standardGachaFound = true;
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
        ids("MainMission.json", "MainMissionID", mainMissions_);
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
                challengeRewardStars_[line] |= uint64_t{1} << stars;
            }
        };
        challengeRewards("ChallengeMazeRewardLine.json");
        challengeRewards("ChallengeStoryRewardLine.json");
        challengeRewards("ChallengeBossRewardLine.json");

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

    for (ChallengeInfo& floor : challenges_) {
        auto extra = challengeExtras_.find(floor.id);
        if (extra == challengeExtras_.end()) continue;
        if (extra->second.turnLimit != 0) floor.roundLimit = extra->second.turnLimit;
        floor.battleTargetIds = extra->second.battleTargetIds;
    }
    challengeExtras_.clear();

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
    logging::info("data", "{} challenge floors in {} seasons",
                  challenges_.size(), challengeGroups_.size());
    logging::info("data", "{} main missions, {} tutorials, {} guides, {} quests",
                  mainMissions_.size(), tutorials_.size(), tutorialGuides_.size(),
                  quests_.size());
    return true;
}

}  // namespace data
