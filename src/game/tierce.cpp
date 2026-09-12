#include "game/tierce.h"

#include <algorithm>
#include <utility>

#include "core/logger.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/battle.h"
#include "game/challenge.h"
#include "game/handlers.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/session.h"

namespace game {
namespace {

constexpr uint32_t kNodes = 3;

const data::ChallengeTierceInfo* configOf(const Player& player) {
    const TierceRun& run = player.tierce();
    if (!run.active) return nullptr;
    return data::Tables::get().challengeTierce(run.tierceId);
}

// The floor a tierce row extends; everything but the third node comes from it.
const data::ChallengeInfo* floorOf(const data::ChallengeTierceInfo& config) {
    return data::Tables::get().challenge(config.preChallengeId);
}

const data::ChallengeInfo* floorOf(const Player& player) {
    const data::ChallengeTierceInfo* config = configOf(player);
    return config == nullptr ? nullptr : floorOf(*config);
}

proto::ExtraLineupType lineupTypeOf(uint32_t stage) {
    switch (stage) {
        case 0:
            return proto::ExtraLineupType::ExtraLineupType_LineupChallenge;
        case 1:
            return proto::ExtraLineupType::ExtraLineupType_LineupChallenge2;
        default:
            return proto::ExtraLineupType::ExtraLineupType_LineupChallenge3;
    }
}

uint32_t entranceOf(const data::ChallengeTierceInfo& config, const data::ChallengeInfo& floor,
                    uint32_t stage) {
    if (stage >= 2) return config.mapEntranceId;
    if (stage == 1 && floor.mapEntranceId2 != 0) return floor.mapEntranceId2;
    return floor.mapEntranceId;
}

// Memory of Chaos spends one pool of cycles across all three nodes; the other two keep
// the floor's own per-node limit.
bool poolsCycles(const data::ChallengeInfo& floor) {
    return floor.kind == data::ChallengeKind::Memory;
}

std::string battleTypeOf(const data::ChallengeInfo& floor) {
    switch (floor.kind) {
        case data::ChallengeKind::Story:
            return "PF";
        case data::ChallengeKind::Boss:
            return "AS";
        case data::ChallengeKind::Memory:
            break;
    }
    return {};
}

// Which of the floor's star targets the run has met, as a flat list of target ids --
// this mode reports the targets themselves rather than a bitmask.
std::vector<uint32_t> finishedTargets(const Player& player) {
    const data::ChallengeTierceInfo* config = configOf(player);
    if (config == nullptr) return {};
    const TierceRun& run = player.tierce();
    const data::Tables& tables = data::Tables::get();

    std::vector<uint32_t> met;
    for (uint32_t id : config->targetIds) {
        const data::ChallengeTarget* target = tables.challengeTarget(id);
        if (target == nullptr) continue;
        bool ok = false;
        switch (target->kind) {
            case data::ChallengeTarget::Kind::RoundsLeft:
                ok = run.roundsLeft >= target->param;
                break;
            case data::ChallengeTarget::Kind::DeadAvatar:
                ok = run.deaths[0] + run.deaths[1] + run.deaths[2] == 0;
                break;
            case data::ChallengeTarget::Kind::TotalScore:
                ok = run.totalScore() >= target->param;
                break;
        }
        if (ok) met.push_back(id);
    }
    return met;
}

bool enterArena(Player& player, proto::SceneInfo& out) {
    const data::ChallengeTierceInfo* config = configOf(player);
    if (config == nullptr) return false;
    const data::ChallengeInfo* floor = floorOf(*config);
    if (floor == nullptr) return false;

    uint32_t entryId = entranceOf(*config, *floor, player.tierce().stage);
    ChallengeArena storage;
    // Every node starts at its entrance, not wherever the last one ended.
    SceneLocation before = player.location();
    player.location().floorId = 0;
    if (scene::load(player, entryId, 0, true, out, tierce::arena(player, storage))) return true;
    player.location() = before;
    logging::warn("tierce", "floor {} node {} names entrance {}, which is not in the dump",
                  config->id, player.tierce().stage + 1, entryId);
    return false;
}

proto::ChallengeTierceStageData stageData(const TierceRun& run, uint32_t stage) {
    proto::ChallengeTierceStageData data;
    data.stage_index = stage;
    data.end_status = static_cast<proto::BattleEndStatus>(run.endStatus[stage]);
    data.score_id = run.scores[stage];
    data.INLLMKEDGLC = run.cycles[stage];
    data.IBECKONIICF = run.deaths[stage];
    return data;
}

// One node cleared, or the whole floor. The client redraws the node list off these.
void syncStage(net::Session& session, const Player& player, uint32_t stage) {
    const TierceRun& run = player.tierce();
    proto::ChallengeTierceSyncNotify notify;
    auto& single = notify.IANBCDJPDJF.emplace();
    single.stage_data = stageData(run, stage);
    single.is_passed = run.passed;
    single.finished_target_list = finishedTargets(player);
    notify.MMCGBGDJIPN_case = proto::ChallengeTierceSyncNotify::k_IANBCDJPDJF;
    session.send(cmd::ChallengeTierceSyncNotify, notify);
}

void syncFloor(net::Session& session, const Player& player) {
    const TierceRun& run = player.tierce();
    proto::ChallengeTierceSyncNotify notify;
    auto& all = notify.DFNNKENBOEN.emplace();
    all.challenge_id = run.tierceId;
    all.is_passed = run.passed;
    all.finished_target_list = finishedTargets(player);
    for (uint32_t stage = 0; stage < kNodes; ++stage) {
        all.GBMBAHHDONN.push_back(stageData(run, stage));
    }
    auto& record = all.EGEHLOPFELK.emplace();
    record.used_cycle_count = run.cycles[0] + run.cycles[1] + run.cycles[2];
    record.total_score = run.totalScore();
    notify.MMCGBGDJIPN_case = proto::ChallengeTierceSyncNotify::k_DFNNKENBOEN;
    session.send(cmd::ChallengeTierceSyncNotify, notify);
}

void syncMedal(net::Session& session, uint32_t groupId) {
    proto::ChallengeTierceSyncNotify notify;
    notify.medal.emplace().group_id = groupId;
    notify.MMCGBGDJIPN_case = proto::ChallengeTierceSyncNotify::k_medal;
    session.send(cmd::ChallengeTierceSyncNotify, notify);
}

// Folds the run into what the floor's history shows from now on.
void record(Player& player) {
    const TierceRun& run = player.tierce();
    TierceProgress& progress = player.tierceHistory()[run.tierceId];
    for (uint32_t stage = 0; stage < kNodes; ++stage) {
        if (!run.party[stage].empty()) progress.party[stage] = run.party[stage];
        progress.buffs[stage] = run.buffs[stage];
        if (run.endStatus[stage] ==
            static_cast<uint32_t>(proto::BattleEndStatus::BATTLE_END_WIN)) {
            progress.cleared[stage] = true;
            progress.scores[stage] = run.scores[stage];
            progress.cycles[stage] = run.cycles[stage];
            progress.deaths[stage] = run.deaths[stage];
        }
    }
    if (run.passed) progress.passed = true;
    std::vector<uint32_t> met = finishedTargets(player);
    for (uint32_t id : met) {
        if (std::find(progress.targets.begin(), progress.targets.end(), id) ==
            progress.targets.end()) {
            progress.targets.push_back(id);
        }
    }
}

// The per-node teams a start request carries, or the ones the floor remembers when it
// sends none.
void adoptLineups(Player& player, uint32_t tierceId,
                  const std::vector<proto::ChallengeTierceStageLineupInfo>& stages) {
    TierceRun& run = player.tierce();
    auto it = player.tierceHistory().find(tierceId);
    const TierceProgress* progress =
        it == player.tierceHistory().end() ? nullptr : &it->second;

    for (uint32_t stage = 0; stage < kNodes; ++stage) {
        std::vector<uint32_t> party;
        if (stage < stages.size()) {
            for (const proto::AvatarIdentifier& entry : stages[stage].lineup) {
                if (entry.id != 0) party.push_back(entry.id);
            }
            run.buffs[stage] = stages[stage].buff_id;
        }
        if (party.empty() && progress != nullptr) party = progress->party[stage];
        if (stage >= stages.size() && progress != nullptr) run.buffs[stage] = progress->buffs[stage];
        run.party[stage] = std::move(party);
    }
}

}  // namespace

namespace tierce {

proto::GetChallengeTierceDataScRsp history(const Player& player) {
    const data::Tables& tables = data::Tables::get();

    proto::GetChallengeTierceDataScRsp rsp;
    rsp.retcode = 0;
    for (const data::ChallengeInfo& floor : tables.challenges()) {
        const data::ChallengeTierceInfo* config = tables.challengeTierceFor(floor.id);
        if (config == nullptr) continue;

        auto it = player.tierceHistory().find(config->id);
        const TierceProgress* progress =
            it == player.tierceHistory().end() ? nullptr : &it->second;

        proto::ChallengeTierceData data;
        data.challenge_id = config->id;
        // Unlocking floors is GetChallenge's job; this only says what has been played.
        data.is_passed = progress != nullptr && progress->passed;
        if (progress != nullptr) data.finished_target_list = progress->targets;
        auto& summary = data.record.emplace();
        for (uint32_t stage = 0; stage < kNodes; ++stage) {
            proto::ChallengeTierceStageInfo info;
            info.stage_index = stage;
            if (progress != nullptr) {
                info.buff_id = progress->buffs[stage];
                for (uint32_t avatarId : progress->party[stage]) {
                    proto::AvatarIdentifier entry;
                    entry.id = avatarId;
                    info.lineup.push_back(entry);
                }
                summary.used_cycle_count += progress->cycles[stage];
                summary.total_score += progress->scores[stage];

                proto::ChallengeTierceStageData result;
                result.stage_index = stage;
                result.end_status = progress->cleared[stage]
                                        ? proto::BattleEndStatus::BATTLE_END_WIN
                                        : proto::BattleEndStatus::BATTLE_END_NONE;
                result.score_id = progress->scores[stage];
                result.INLLMKEDGLC = progress->cycles[stage];
                result.IBECKONIICF = progress->deaths[stage];
                data.result_list.push_back(std::move(result));
            }
            data.stage_info_list.push_back(std::move(info));
        }
        rsp.challenge_info_list.push_back(std::move(data));
    }
    return rsp;
}

proto::ChallengeTierceChallengeInfo current(const Player& player) {
    const TierceRun& run = player.tierce();

    proto::ChallengeTierceChallengeInfo info;
    info.challenge_id = run.tierceId;
    info.stage_index = run.stage;
    info.is_single_stage = run.singleStage;
    for (uint32_t stage = 0; stage < kNodes; ++stage) {
        info.lineup_list.push_back(lineup(player, stage));
    }
    return info;
}

proto::LineupInfo lineup(const Player& player, uint32_t stage) {
    uint32_t index = std::min(stage, kNodes - 1);
    return challenge::extraLineup(player, player.tierce().party[index], lineupTypeOf(index));
}

void setLineups(Player& player, uint32_t tierceId,
                const std::vector<proto::ChallengeTierceStageLineupInfo>& stages) {
    if (data::Tables::get().challengeTierce(tierceId) == nullptr) return;
    TierceProgress& progress = player.tierceHistory()[tierceId];
    for (uint32_t stage = 0; stage < kNodes && stage < stages.size(); ++stage) {
        std::vector<uint32_t> party;
        for (const proto::AvatarIdentifier& entry : stages[stage].lineup) {
            if (entry.id != 0) party.push_back(entry.id);
        }
        if (!party.empty()) progress.party[stage] = std::move(party);
        progress.buffs[stage] = stages[stage].buff_id;
    }
    player.save();
}

uint32_t start(Player& player, uint32_t tierceId, bool single, uint32_t stageIndex,
               const std::vector<proto::ChallengeTierceStageLineupInfo>& stages,
               proto::SceneInfo& out) {
    const data::ChallengeTierceInfo* config = data::Tables::get().challengeTierce(tierceId);
    if (config == nullptr) return fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
    const data::ChallengeInfo* floor = floorOf(*config);
    if (floor == nullptr) return fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
    if (stageIndex >= kNodes) return fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);

    TierceRun saved = player.tierce();

    TierceRun run;
    run.active = true;
    run.tierceId = tierceId;
    run.singleStage = single;
    run.stage = single ? stageIndex : 0;
    run.roundsLeft = poolsCycles(*floor) ? config->roundLimit : floor->roundLimit;
    // A retry keeps the spot the first attempt left from.
    run.origin = saved.active ? saved.origin : player.location();
    run.originPos = saved.active ? saved.originPos : player.position();

    player.tierce() = std::move(run);
    adoptLineups(player, tierceId, stages);
    if (player.tierce().curParty().empty()) {
        player.tierce() = std::move(saved);
        return fail(proto::Retcode::RET_CHALLENGE_LINEUP_EMPTY);
    }
    if (!enterArena(player, out)) {
        player.tierce() = std::move(saved);
        return fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
    }

    // Both cannot be going at once, and the older packet's run is what the client would
    // otherwise still think it is in.
    player.challenge() = ChallengeRun{};
    record(player);
    player.saveNow();

    logging::info("tierce", "floor {} started at node {}{}, {} cycle(s)", tierceId,
                  player.tierce().stage + 1, single ? " on its own" : "",
                  player.tierce().roundsLeft);
    return 0;
}

const ChallengeArena* arena(const Player& player, ChallengeArena& storage) {
    const data::ChallengeTierceInfo* config = configOf(player);
    if (config == nullptr) return nullptr;
    const data::ChallengeInfo* floor = floorOf(*config);
    if (floor == nullptr) return nullptr;

    const TierceRun& run = player.tierce();
    if (run.stage >= 2) {
        storage.mazeGroupId = config->mazeGroupId;
        storage.monsters = &config->monsters;
    } else {
        storage.mazeGroupId = floor->stages[run.stage].mazeGroupId;
        storage.monsters = &floor->stages[run.stage].monsters;
    }
    storage.party = &run.curParty();
    return &storage;
}

void prepareBattle(const Player& player, BattleRequest& request) {
    const data::ChallengeTierceInfo* config = configOf(player);
    if (config == nullptr) return;
    const data::ChallengeInfo* floor = floorOf(*config);
    if (floor == nullptr) return;
    const TierceRun& run = player.tierce();

    request.party = run.curParty();
    request.mazeBuffId = floor->mazeBuffId;
    request.stageBuffId = run.buffs[run.stage < kNodes ? run.stage : kNodes - 1];
    request.roundsLimit = run.roundsLeft;
    request.scoreSoFar = run.totalScore();
    request.battleType = battleTypeOf(*floor);
    request.battleTargetIds = floor->battleTargetIds;
}

bool nextStage(net::Session& session, Player& player, proto::SceneInfo& out) {
    TierceRun& run = player.tierce();
    if (!run.active || run.singleStage || run.stage + 1 >= kNodes) return false;

    ++run.stage;
    if (run.party[run.stage].empty()) {
        --run.stage;
        return false;
    }
    if (!enterArena(player, out)) {
        --run.stage;
        return false;
    }

    proto::ChallengeLineupNotify swap;
    swap.extra_lineup_type = lineupTypeOf(run.stage);
    session.send(cmd::ChallengeLineupNotify, swap);

    proto::SyncLineupNotify sync;
    sync.lineup = lineup(player, run.stage);
    session.send(cmd::SyncLineupNotify, sync);
    player.saveNow();
    return true;
}

bool restart(net::Session& session, Player& player, proto::SceneInfo& out) {
    TierceRun& run = player.tierce();
    if (!run.active) return false;

    // The node is fought again from scratch; what it already banked is dropped.
    run.endStatus[run.stage] = 0;
    run.scores[run.stage] = 0;
    const data::ChallengeTierceInfo* config = configOf(player);
    const data::ChallengeInfo* floor = config == nullptr ? nullptr : floorOf(*config);
    if (floor != nullptr && poolsCycles(*floor)) {
        run.roundsLeft += run.cycles[run.stage];
    }
    run.cycles[run.stage] = 0;
    run.deaths[run.stage] = 0;

    if (!enterArena(player, out)) return false;

    proto::SyncLineupNotify sync;
    sync.lineup = lineup(player, run.stage);
    session.send(cmd::SyncLineupNotify, sync);
    player.saveNow();
    return true;
}

bool battleFinished(net::Session& session, Player& player,
                    const proto::PVEBattleResultCsReq& req) {
    const data::ChallengeTierceInfo* config = configOf(player);
    if (config == nullptr) return false;
    const data::ChallengeInfo* floor = floorOf(*config);
    if (floor == nullptr) return false;

    TierceRun& run = player.tierce();
    uint32_t stage = std::min(run.stage, kNodes - 1);

    // Backing out of a node is not losing the floor; the client stays put.
    if (req.end_status == proto::BattleEndStatus::BATTLE_END_QUIT) return true;

    if (req.stt) {
        // Pure Fiction and Apocalyptic Shadow report a running total, so this node's
        // share is whatever is above what the earlier ones banked.
        uint32_t total = req.stt->challenge_score;
        uint32_t before = run.totalScore() - run.scores[stage];
        run.scores[stage] = total > before ? total - before : 0;

        for (const proto::AvatarBattleInfo& avatar : req.stt->battle_avatar_list) {
            if (avatar.avatar_status && avatar.avatar_status->left_hp == 0) ++run.deaths[stage];
        }
        run.cycles[stage] = std::max(run.cycles[stage], req.stt->round_cnt);
        if (poolsCycles(*floor)) {
            uint32_t used = run.cycles[0] + run.cycles[1] + run.cycles[2];
            uint32_t limit = config->roundLimit;
            run.roundsLeft = limit > used ? limit - used : 0;
        }
    }

    bool win = req.end_status == proto::BattleEndStatus::BATTLE_END_WIN;
    if (!win) {
        run.endStatus[stage] = static_cast<uint32_t>(proto::BattleEndStatus::BATTLE_END_LOSE);
        syncFloor(session, player);
        record(player);
        player.saveNow();
        logging::info("tierce", "floor {} failed at node {}", run.tierceId, stage + 1);
        return true;
    }

    // Monsters still standing means this node is not over.
    for (const SceneEntity& entity : player.sceneState().entities()) {
        if (entity.kind == EntityKind::Monster) return true;
    }

    run.endStatus[stage] = static_cast<uint32_t>(proto::BattleEndStatus::BATTLE_END_WIN);
    // Replaying one node ends there; walking the floor ends after the third, and only
    // the third clears the floor either way.
    bool last = run.singleStage || stage + 1 >= kNodes;
    if (stage + 1 >= kNodes) run.passed = true;
    record(player);
    player.saveNow();

    logging::info("tierce", "floor {} node {} cleared in {} cycle(s), score {}", run.tierceId,
                  stage + 1, run.cycles[stage], run.scores[stage]);

    if (last) {
        syncFloor(session, player);
        if (run.passed) syncMedal(session, floor->groupId);
    } else {
        // The client asks for the next node itself once it has read this.
        syncStage(session, player, stage);
    }
    return true;
}

void leave(net::Session& session, Player& player) {
    TierceRun& run = player.tierce();
    if (!run.active) return;

    record(player);
    uint32_t entryId = run.origin.entryId;
    Position at = run.originPos;
    run = TierceRun{};
    challenge::putBack(session, player, entryId, at);
}

}  // namespace tierce
}  // namespace game
