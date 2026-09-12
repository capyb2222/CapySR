#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "game/lineup.h"
#include "game/roster.h"
#include "game/scene.h"

namespace game {

// What the client is currently fighting, so the result packet can be answered and
// the beaten monsters removed from the scene.
struct BattleContext {
    uint32_t battleId = 0;
    uint32_t stageId = 0;
    uint32_t cocoonId = 0;
    uint32_t wave = 0;
    std::vector<uint32_t> monsterEntityIds;
    bool active = false;
};

// A challenge run in progress. Not persisted: leaving the client mid-floor drops the
// run, which is what the real server does with an expired season anyway.
struct ChallengeRun {
    bool active = false;
    uint32_t challengeId = 0;
    // 1 or 2. MoC and its siblings fight two nodes with two separate teams.
    uint32_t stage = 1;
    proto::ChallengeStatus status = proto::ChallengeStatus::CHALLENGE_UNKNOWN;
    uint32_t roundsLeft = 0;
    uint32_t deadAvatars = 0;
    uint32_t stars = 0;
    // Pure Fiction and Apocalyptic Shadow score each half separately.
    uint32_t score[2] = {0, 0};
    // The buff the player picked for each half, when the mode has them.
    uint32_t buffs[2] = {0, 0};
    // The two teams, exactly as StartChallenge sent them.
    std::vector<uint32_t> party[2];
    // Where to put the player back when they leave.
    SceneLocation origin;
    Position originPos;

    uint32_t totalScore() const { return score[0] + score[1]; }
    const std::vector<uint32_t>& curParty() const { return party[stage > 1 ? 1 : 0]; }
};

// An Anomaly Arbitration fight in progress: one team, one monster, no second node.
struct PeakRun {
    bool active = false;
    uint32_t peakId = 0;
    uint32_t groupId = 0;
    bool hard = false;
    uint32_t buffId = 0;  // the boss buff the player picked
    std::vector<uint32_t> party;
    // The arena actually entered; a season newer than the scene dump borrows another.
    uint32_t entryId = 0;
    uint32_t mazeGroupId = 0;
    SceneLocation origin;
    Position originPos;
};

// The three-node run 4.6 replaced Memory of Chaos's two with. Each node brings its own
// team and its own buff, and the cycle pool is spent across all three.
struct TierceRun {
    bool active = false;
    uint32_t tierceId = 0;
    uint32_t stage = 0;  // 0..2
    // Replaying one cleared node rather than walking the whole floor.
    bool singleStage = false;
    bool passed = false;
    uint32_t roundsLeft = 0;
    uint32_t deaths[3] = {0, 0, 0};
    uint32_t cycles[3] = {0, 0, 0};
    uint32_t scores[3] = {0, 0, 0};
    // proto::BattleEndStatus per node, as the result list reports it.
    uint32_t endStatus[3] = {0, 0, 0};
    uint32_t buffs[3] = {0, 0, 0};
    std::vector<uint32_t> party[3];
    SceneLocation origin;
    Position originPos;

    uint32_t totalScore() const { return scores[0] + scores[1] + scores[2]; }
    const std::vector<uint32_t>& curParty() const { return party[stage < 3 ? stage : 2]; }
};

// What the three-node history remembers between runs: the teams and buffs the player
// last set per floor, and how far they got.
struct TierceProgress {
    std::vector<uint32_t> party[3];
    uint32_t buffs[3] = {0, 0, 0};
    uint32_t scores[3] = {0, 0, 0};
    uint32_t cycles[3] = {0, 0, 0};
    uint32_t deaths[3] = {0, 0, 0};
    bool cleared[3] = {false, false, false};
    bool passed = false;
    std::vector<uint32_t> targets;
};

// A cleared Anomaly Arbitration fight, as the overview shows it.
struct PeakRecord {
    uint32_t cycles = 0;
    std::vector<uint32_t> targets;
    std::vector<uint32_t> team;
    uint32_t buffId = 0;
};

// What the Anomaly Arbitration overview remembers between fights and restarts: the
// last team and boss buff per fight, the seasons on hard, and the best clears.
struct PeakProgress {
    std::map<uint32_t, std::vector<uint32_t>> teams;
    std::map<uint32_t, uint32_t> bossBuffs;
    std::set<uint32_t> hardGroups;
    std::map<uint32_t, PeakRecord> records;  // by key()

    static uint32_t key(uint32_t peakId, bool hard) { return peakId * 2 + (hard ? 1 : 0); }
    const PeakRecord* record(uint32_t peakId, bool hard) const {
        auto it = records.find(key(peakId, hard));
        return it == records.end() ? nullptr : &it->second;
    }
};

// One logged-in account: identity, party, position and the current fight.
class Player {
public:
    explicit Player(uint32_t uid);

    uint32_t uid() const { return uid_; }
    const std::string& name() const { return name_; }
    void setName(std::string value) { name_ = std::move(value); }
    const std::string& signature() const { return signature_; }
    void setSignature(std::string value) { signature_ = std::move(value); }
    uint32_t level() const { return level_; }
    uint32_t worldLevel() const { return worldLevel_; }
    uint32_t stamina() const { return stamina_; }
    uint32_t headIcon() const { return headIcon_; }
    void setHeadIcon(uint32_t value) { headIcon_ = value; }
    uint32_t hcoin() const { return hcoin_; }
    uint32_t scoin() const { return scoin_; }
    uint32_t mcoin() const { return mcoin_; }
    uint64_t loginRandom() const { return loginRandom_; }
    void setLoginRandom(uint64_t value) { loginRandom_ = value; }

    uint32_t gender() const;
    bool isGenderSet() const { return true; }

    LineupBook& lineups() { return lineups_; }
    const LineupBook& lineups() const { return lineups_; }
    Position& position() { return position_; }
    const Position& position() const { return position_; }
    SceneLocation& location() { return location_; }
    const SceneLocation& location() const { return location_; }
    SceneState& sceneState() { return sceneState_; }
    const SceneState& sceneState() const { return sceneState_; }
    BattleContext& battle() { return battle_; }
    const BattleContext& battle() const { return battle_; }
    ChallengeRun& challenge() { return challenge_; }
    const ChallengeRun& challenge() const { return challenge_; }
    PeakRun& peak() { return peak_; }
    const PeakRun& peak() const { return peak_; }
    TierceRun& tierce() { return tierce_; }
    const TierceRun& tierce() const { return tierce_; }
    std::map<uint32_t, TierceProgress>& tierceHistory() { return tierceHistory_; }
    const std::map<uint32_t, TierceProgress>& tierceHistory() const { return tierceHistory_; }
    PeakProgress& peakProgress() { return peakProgress_; }
    const PeakProgress& peakProgress() const { return peakProgress_; }

    uint32_t mainCharacter() const { return mainCharacter_; }
    void setMainCharacter(uint32_t value) { mainCharacter_ = value; }
    uint32_t marchType() const { return marchType_; }
    void setMarchType(uint32_t value) { marchType_ = value; }
    bool globalBuffs() const { return globalBuffs_; }
    void setGlobalBuffs(bool value) { globalBuffs_ = value; }

    // A snapshot of the current srtools build with this player's path choices.
    Roster roster() const;

    uint32_t nextBattleId() { return ++battleIdCursor_; }

    // data/player.json. save() coalesces bursts; saveNow() always writes.
    void load();
    void save();
    void saveNow();

private:
    uint32_t uid_;
    std::string name_;
    std::string signature_;
    uint32_t level_ = 1;
    uint32_t worldLevel_ = 0;
    uint32_t stamina_ = 240;
    uint32_t headIcon_ = 201001;
    uint32_t hcoin_ = 0;
    uint32_t scoin_ = 0;
    uint32_t mcoin_ = 0;
    uint64_t loginRandom_ = 0;

    LineupBook lineups_;
    Position position_;
    SceneLocation location_;
    SceneState sceneState_;
    BattleContext battle_;
    ChallengeRun challenge_;
    PeakRun peak_;
    PeakProgress peakProgress_;
    TierceRun tierce_;
    std::map<uint32_t, TierceProgress> tierceHistory_;

    uint32_t mainCharacter_ = 8008;
    uint32_t marchType_ = 1224;
    bool globalBuffs_ = true;
    uint32_t battleIdCursor_ = 0;
    uint64_t lastSaveMs_ = 0;
};

}  // namespace game
