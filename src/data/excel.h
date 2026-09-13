#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace data {

// Element -> the entry buff the client expects when a technique starts a fight.
uint32_t attackBuffForElement(const std::string& damageType);

struct AvatarInfo {
    uint32_t id = 0;
    std::string damageType;
    std::string baseType;
    uint32_t spNeed = 10000;  // hundredths, as the SpBarInfo wants it
    uint32_t maxPromotion = 6;
    uint32_t maxRank = 6;
    uint32_t rarity = 0;
    uint32_t enhancedId = 0;
    uint32_t attackBuffId = 0;
    // AvatarDefaultMazeBuff; falls back to id * 100 + 1 when the row is missing.
    std::vector<uint32_t> techniqueBuffs;
    uint32_t techniqueSkillIndex = 2;
};

struct LightconeInfo {
    uint32_t id = 0;
    uint32_t rarity = 0;
};

enum class GachaType { Normal, AvatarUp, WeaponUp };

// A warp pool. A limited one is named after its featured 5* in its prefab.
struct GachaPool {
    uint32_t id = 0;
    GachaType type = GachaType::Normal;
    uint32_t featured = 0;
    uint32_t upChance = 0;  // percent
};

struct SkillTreePoint {
    uint32_t pointId = 0;
    uint32_t avatarId = 0;
    uint32_t level = 1;
    uint32_t maxLevel = 1;
    uint32_t anchorType = 0;
    bool defaultUnlock = false;
};

struct StageInfo {
    uint32_t id = 0;
    uint32_t level = 1;
    uint32_t eliteGroup = 0;
    uint32_t hardLevelGroup = 1;
    std::vector<std::vector<uint32_t>> waves;
};

struct MonsterInfo {
    uint32_t id = 0;
    uint32_t templateId = 0;
    uint32_t eliteGroup = 0;
    uint32_t hardLevelGroup = 1;
};

struct PlaneInfo {
    uint32_t planeId = 0;
    uint32_t worldId = 0;
    uint32_t planeType = 0;
    uint32_t startFloorId = 0;
    std::vector<uint32_t> floorIds;
};

struct EntranceInfo {
    uint32_t id = 0;
    uint32_t planeId = 0;
    uint32_t floorId = 0;
    // Which anchor of the entry the player lands on, when the dump names one.
    uint32_t startGroupId = 0;
    uint32_t startAnchorId = 0;
};

// MultiplePathAvatarConfig: the Trailblazer's paths and March 7th's two forms.
struct MultiPathInfo {
    uint32_t avatarId = 0;
    uint32_t baseAvatarId = 0;
    std::string gender;
};

// Keyed by id * 100 + world level, the way the client addresses a calyx.
struct CocoonInfo {
    uint32_t id = 0;
    uint32_t worldLevel = 0;
    uint32_t propId = 0;
    uint32_t staminaCost = 0;
    std::vector<uint32_t> stageIds;
};

// The standard warp: GachaBasicInfo's Normal pool, and GachaCeiling's pick at 300 pulls.
struct StandardGacha {
    uint32_t gachaId = 1001;
    uint32_t ceilingNum = 0;
    std::vector<uint32_t> ceilingAvatars;
};

// The three challenge modes, which differ in how a floor is scored and which scene the
// client leaves back to.
enum class ChallengeKind { Memory, Story, Boss };

// One monster the maze config plants in the arena. The scene dump's own monster is only
// a placeholder: the config says which model actually stands there and which event -- and
// so which battle stage -- it starts.
struct ChallengeMonster {
    uint32_t configId = 0;      // matches the group monster's instId in the scene dump
    uint32_t npcMonsterId = 0;
    uint32_t eventId = 0;       // the stage id too; challenge events are not in PlaneEvent
};

// Half a floor: MoC and its two siblings all fight two nodes with two separate teams.
struct ChallengeStage {
    uint32_t mazeGroupId = 0;
    std::vector<ChallengeMonster> monsters;
};

// One floor of Memory of Chaos, Pure Fiction or Apocalyptic Shadow. Star is a bitmask
// over targetIds, so a full clear is `(1 << targetIds.size()) - 1`.
struct ChallengeInfo {
    uint32_t id = 0;
    uint32_t groupId = 0;
    ChallengeKind kind = ChallengeKind::Memory;
    uint32_t floor = 0;
    uint32_t stageNum = 1;
    uint32_t mapEntranceId = 0;
    uint32_t mapEntranceId2 = 0;
    uint32_t mazeBuffId = 0;
    // Cycles for MoC (ChallengeCountDown), turns for Pure Fiction (TurnLimit).
    uint32_t roundLimit = 0;
    std::vector<uint32_t> targetIds;
    // Pure Fiction's per-wave score targets, from ChallengeStoryMazeExtra.
    std::vector<uint32_t> battleTargetIds;
    ChallengeStage stages[2];
};

// The third node 4.6 bolts onto a challenge floor. It brings its own arena, monster and
// star targets; the buff, the mode and the first two nodes come from the floor it
// extends. The client starts the whole floor through this once a row exists for it.
struct ChallengeTierceInfo {
    uint32_t id = 0;
    uint32_t preChallengeId = 0;  // PreChallengeMazeID, the floor this extends
    uint32_t mapEntranceId = 0;
    uint32_t mazeGroupId = 0;
    // Cycles for the whole floor, MoC only; the other two keep the floor's own limit.
    uint32_t roundLimit = 0;
    std::vector<uint32_t> targetIds;
    std::vector<ChallengeMonster> monsters;
};

// How one star is earned. MoC counts cycles left and deaths; the other two only score.
struct ChallengeTarget {
    uint32_t id = 0;
    enum class Kind { RoundsLeft, DeadAvatar, TotalScore } kind = Kind::RoundsLeft;
    uint32_t param = 0;
};

// The season a set of floors belongs to. Its rewards are keyed by star *threshold*
// rather than by index, so they are addressed as a bitmask over those thresholds.
struct ChallengeGroupInfo {
    uint32_t id = 0;
    uint32_t rewardLineGroupId = 0;
    ChallengeKind kind = ChallengeKind::Memory;
};

// Every Anomaly Arbitration arena keeps its one monster at this instance id.
constexpr uint32_t kPeakMarkerId = 200001;

// One Anomaly Arbitration fight: a knight, or the season's boss. Like the other
// challenges it is fought on an ordinary floor cut down to one maze group, where a
// single monster starts the stage its event names.
struct PeakInfo {
    uint32_t id = 0;
    uint32_t groupId = 0;
    uint32_t mapEntranceId = 0;
    uint32_t mazeGroupId = 0;
    uint32_t npcMonsterId = 0;
    uint32_t eventId = 0;
    std::vector<uint32_t> targetIds;  // NormalTargetList
    std::vector<uint32_t> tagBuffs;   // the enemy tags, TagList
    std::vector<ChallengeMonster> monsters;
    // The boss only: the buffs the player picks one of, and what hard mode swaps in.
    bool boss = false;
    std::vector<uint32_t> bossBuffs;
    uint32_t hardTarget = 0;
    uint32_t hardEventId = 0;
    std::vector<uint32_t> hardTagBuffs;
    std::vector<ChallengeMonster> hardMonsters;
};

// One Anomaly Arbitration season: three knights and a boss.
struct PeakGroupInfo {
    uint32_t id = 0;
    std::vector<uint32_t> mobIds;
    uint32_t bossId = 0;
    uint32_t rewardGroupId = 0;
};

// BattleTargetConfig, as far as a result is judged by it: a ceiling on the cycles used,
// or on the avatars lost.
struct BattleTargetInfo {
    uint32_t id = 0;
    uint32_t param = 0;
    bool countsDeaths = false;
};

// MazePlane.PlaneType strings, in the numbering SceneInfo.game_mode_type uses.
uint32_t planeTypeFromName(const std::string& name);

// InteractConfig / ScenePropInfo state names, in the client's numbering.
uint32_t propStateFromName(const std::string& name);

// What interacting with a prop does to it: only valid from `srcState`, and a zero
// srcState means the row applies whatever the prop is currently doing.
struct InteractInfo {
    uint32_t id = 0;
    uint32_t srcState = 0;
    uint32_t targetState = 0;
    bool anySource = false;
};

// Game tables, merged over a priority-ordered list of ExcelOutput-shaped directories.
// The first source that defines a row id keeps it; later ones only fill gaps, because
// the beta dumps are trimmed extracts that would otherwise drop production columns.
class Tables {
public:
    static Tables& get();

    bool load(const std::vector<std::string>& sources);
    bool loaded() const { return loaded_; }

    const AvatarInfo* avatar(uint32_t id) const;
    const StageInfo* stage(uint32_t id) const;
    const MonsterInfo* monster(uint32_t id) const;
    const PlaneInfo* plane(uint32_t id) const;
    const EntranceInfo* entrance(uint32_t id) const;
    const CocoonInfo* cocoon(uint32_t id, uint32_t worldLevel) const;
    const std::string* monsterRank(uint32_t templateId) const;
    const MultiPathInfo* multiPath(uint32_t avatarId) const;
    const InteractInfo* interact(uint32_t interactId) const;

    // 8002..8010 collapse to 8001 and 1224 to 1001; anything else is its own base.
    uint32_t baseAvatarId(uint32_t avatarId) const;
    const std::vector<uint32_t>* multiPathVariants(uint32_t baseAvatarId) const;

    // Skill tree points of one avatar, with their table max level.
    const std::vector<SkillTreePoint>* skillTree(uint32_t avatarId) const;

    // PlaneEvent: an overworld monster's event id resolves to a stage at this world level.
    uint32_t stageForEvent(uint32_t eventId, uint32_t worldLevel) const;

    // FarmElementConfig, the Stagnant Shadows. The client names one by its stage id; a
    // bare element id is resolved at `worldLevel`.
    uint32_t farmElementStage(uint32_t idOrStage, uint32_t worldLevel) const;

    const StandardGacha& standardGacha() const { return standardGacha_; }
    // The standard pool, then the limited ones in id order.
    const std::vector<GachaPool>& gachaPools() const { return gachaPools_; }
    // GachaTypeBasicInfo.UpPropability, in percent; 0 for the standard pool.
    uint32_t gachaUpChance(GachaType type) const;

    const LightconeInfo* lightcone(uint32_t id) const;
    const std::unordered_map<uint32_t, LightconeInfo>& lightcones() const { return lightcones_; }
    // BattlePassReward: paid out by the pass, never by a warp.
    bool battlePassReward(uint32_t itemId) const { return battlePassRewards_.count(itemId) != 0; }

    // AvatarMazeBuff rows named ADV_GlobalSkill_Maze*: Castorice and Silver Wolf.
    const std::vector<uint32_t>& globalMazeBuffs() const { return globalMazeBuffs_; }

    // Reported wholesale as already done, which is what keeps the client out of the
    // prologue -- with nothing finished it waits for the intro to drive it.
    const std::vector<uint32_t>& mainMissions() const { return mainMissions_; }
    const std::vector<uint32_t>& tutorials() const { return tutorials_; }
    const std::vector<uint32_t>& tutorialGuides() const { return tutorialGuides_; }
    const std::vector<uint32_t>& quests() const { return quests_; }

    // Every floor and season of the three challenge modes, in id order.
    const std::vector<ChallengeInfo>& challenges() const { return challenges_; }
    // Keyed by the tierce's own id, and by the floor it extends.
    const ChallengeTierceInfo* challengeTierce(uint32_t id) const;
    const ChallengeTierceInfo* challengeTierceFor(uint32_t challengeId) const;
    const std::vector<ChallengeGroupInfo>& challengeGroups() const { return challengeGroups_; }
    const ChallengeInfo* challenge(uint32_t id) const;
    const ChallengeGroupInfo* challengeGroup(uint32_t groupId) const;
    const ChallengeTarget* challengeTarget(uint32_t id) const;
    // Bit n set for every star count the reward line of that group pays out at.
    uint64_t challengeRewardStars(uint32_t rewardLineGroupId) const;

    // Anomaly Arbitration seasons in id order, and the fights they are made of.
    const std::vector<PeakGroupInfo>& peakGroups() const { return peakGroups_; }
    const PeakGroupInfo* peakGroup(uint32_t id) const;
    const PeakInfo* peak(uint32_t id) const;
    // ChallengePeakReward rows paid out by one reward group.
    const std::vector<uint32_t>* peakRewards(uint32_t rewardGroupId) const;
    const BattleTargetInfo* battleTarget(uint32_t id) const;

    const std::unordered_map<uint32_t, AvatarInfo>& avatars() const { return avatars_; }
    const std::unordered_map<uint32_t, EntranceInfo>& entrances() const { return entrances_; }

    size_t stageCount() const { return stages_.size(); }
    size_t skillPointCount() const { return skillPointCount_; }

private:
    std::unordered_map<uint32_t, AvatarInfo> avatars_;
    std::unordered_map<uint32_t, std::vector<SkillTreePoint>> skillTrees_;
    std::unordered_map<uint32_t, StageInfo> stages_;
    std::unordered_map<uint32_t, MonsterInfo> monsters_;
    std::unordered_map<uint32_t, std::string> monsterRanks_;
    std::unordered_map<uint32_t, PlaneInfo> planes_;
    std::unordered_map<uint32_t, EntranceInfo> entrances_;
    std::unordered_map<uint32_t, CocoonInfo> cocoons_;
    std::unordered_map<uint32_t, MultiPathInfo> multiPaths_;
    std::unordered_map<uint32_t, InteractInfo> interacts_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> multiPathVariants_;
    std::unordered_map<uint64_t, uint32_t> planeEvents_;  // eventId * 10 + worldLevel -> stage
    std::unordered_map<uint64_t, uint32_t> farmElements_;  // id * 100 + worldLevel -> stage
    std::unordered_set<uint32_t> farmStages_;
    StandardGacha standardGacha_;
    std::vector<GachaPool> gachaPools_;
    uint32_t avatarUpChance_ = 50;
    uint32_t weaponUpChance_ = 75;
    std::unordered_map<uint32_t, LightconeInfo> lightcones_;
    std::unordered_set<uint32_t> battlePassRewards_;
    std::vector<uint32_t> globalMazeBuffs_;
    std::vector<uint32_t> mainMissions_;
    std::vector<uint32_t> tutorials_;
    std::vector<uint32_t> tutorialGuides_;
    std::vector<uint32_t> quests_;
    std::vector<ChallengeInfo> challenges_;
    std::unordered_map<uint32_t, ChallengeTierceInfo> challengeTierces_;
    std::vector<ChallengeGroupInfo> challengeGroups_;
    std::unordered_map<uint32_t, ChallengeTarget> challengeTargets_;
    // ChallengeStoryMazeExtra, held only until load() folds it into the floors.
    struct ChallengeExtra {
        uint32_t turnLimit = 0;
        std::vector<uint32_t> battleTargetIds;
    };
    std::unordered_map<uint32_t, ChallengeExtra> challengeExtras_;
    std::unordered_map<uint32_t, uint64_t> challengeRewardStars_;
    std::vector<PeakGroupInfo> peakGroups_;
    std::unordered_map<uint32_t, PeakInfo> peaks_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> peakRewards_;
    std::unordered_map<uint32_t, BattleTargetInfo> battleTargets_;
    size_t skillPointCount_ = 0;
    bool loaded_ = false;
};

}  // namespace data
