#include "game/challenge.h"

#include <algorithm>
#include <utility>

#include "core/config.h"
#include "core/logger.h"
#include "data/excel.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/session.h"

namespace game {
namespace {

// Highest floor of each mode, in the order the client numbers reward_display_type:
// Memory, Story (Pure Fiction), Boss (Apocalyptic Shadow). Hardcoded because the tables
// still carry retired seasons -- the largest Memory group has 15 floors, and claiming a
// floor the current season does not have is worse than being one behind.
constexpr std::pair<uint32_t, uint32_t> kMaxLevels[] = {{1, 12}, {2, 4}, {3, 4}};

uint32_t half(uint32_t stage) { return stage > 1 ? 1u : 0u; }

const data::ChallengeInfo* configOf(const Player& player) {
    const ChallengeRun& run = player.challenge();
    if (!run.active) return nullptr;
    return data::Tables::get().challenge(run.challengeId);
}

// A challenge team is not a squad, so the technique-point cap comes from the avatars
// standing in it rather than from the book.
uint32_t maxMp(const std::vector<uint32_t>& party) {
    uint32_t extra = 0;
    for (uint32_t avatarId : party) {
        if (raisesMazeMpCap(avatarId)) extra += 3;
    }
    return kBaseMazeMp + extra;
}

proto::ExtraLineupType lineupTypeOf(uint32_t stage) {
    return stage > 1 ? proto::ExtraLineupType::ExtraLineupType_LineupChallenge2
                     : proto::ExtraLineupType::ExtraLineupType_LineupChallenge;
}

}  // namespace

namespace challenge {

proto::GetChallengeScRsp history() {
    const data::Tables& tables = data::Tables::get();
    bool cleared = core::Config::get().gameplay.unlockAllChallenges;

    proto::GetChallengeScRsp rsp;
    rsp.retcode = 0;

    for (const data::ChallengeInfo& floor : tables.challenges()) {
        proto::Challenge entry;
        entry.challenge_id = floor.id;
        if (cleared && !floor.targetIds.empty()) {
            entry.star = (uint32_t{1} << floor.targetIds.size()) - 1;
            entry.taken_reward = entry.star;
        }
        rsp.challenge_list.push_back(entry);
    }

    for (const data::ChallengeGroupInfo& season : tables.challengeGroups()) {
        proto::ChallengeGroup group;
        group.group_id = season.id;
        if (cleared) {
            group.taken_stars_count_reward = tables.challengeRewardStars(season.rewardLineGroupId);
        }
        rsp.challenge_group_list.push_back(group);
    }

    for (const auto& [displayType, level] : kMaxLevels) {
        proto::ChallengeHistoryMaxLevel max;
        max.reward_display_type = displayType;
        max.level = cleared ? level : 0;
        rsp.max_level_list.push_back(max);
    }
    return rsp;
}

proto::LineupInfo lineup(const Player& player, uint32_t stage) {
    const std::vector<uint32_t>& party = player.challenge().party[half(stage)];
    Roster roster = player.roster();

    proto::LineupInfo info;
    info.extra_lineup_type = lineupTypeOf(stage);
    // An extra lineup is addressed as its type plus ten, which is how the client keeps
    // it out of the six ordinary squads.
    info.index = static_cast<uint32_t>(info.extra_lineup_type) + 10;
    info.max_mp = maxMp(party);
    info.mp = info.max_mp;
    info.leader_slot = 0;

    uint32_t slot = 0;
    for (uint32_t avatarId : party) {
        info.avatar_list.push_back(roster.toLineupAvatar(avatarId, slot++));
    }
    return info;
}

proto::CurChallenge current(const Player& player) {
    const ChallengeRun& run = player.challenge();
    const data::ChallengeInfo* config = configOf(player);

    proto::CurChallenge cur;
    cur.challenge_id = run.challengeId;
    cur.status = run.status;
    cur.extra_lineup_type = lineupTypeOf(run.stage);
    cur.dead_avatar_num = run.deadAvatars;
    cur.score_id = run.score[0];
    cur.score_two = run.score[1];
    // The client counts cycles used, not cycles left.
    if (config != nullptr && config->roundLimit >= run.roundsLeft) {
        cur.round_count = config->roundLimit - run.roundsLeft;
    }
    // Present but empty: the client reads through the buff info without a null check.
    cur.stage_info.emplace();
    return cur;
}

const ChallengeArena* arena(const Player& player, ChallengeArena& storage) {
    const data::ChallengeInfo* config = configOf(player);
    if (config == nullptr) return nullptr;
    const data::ChallengeStage& stage = config->stages[half(player.challenge().stage)];
    storage.mazeGroupId = stage.mazeGroupId;
    storage.monsters = &stage.monsters;
    return &storage;
}

bool enterArena(Player& player, proto::SceneInfo& out) {
    const data::ChallengeInfo* config = configOf(player);
    if (config == nullptr) return false;

    uint32_t entryId = config->mapEntranceId;
    if (player.challenge().stage > 1 && config->mapEntranceId2 != 0) {
        entryId = config->mapEntranceId2;
    }

    ChallengeArena storage;
    const ChallengeArena* filter = arena(player, storage);
    return scene::load(player, entryId, 0, true, out, filter);
}

uint32_t stars(const Player& player) {
    const data::ChallengeInfo* config = configOf(player);
    if (config == nullptr) return 0;
    const ChallengeRun& run = player.challenge();
    const data::Tables& tables = data::Tables::get();

    uint32_t mask = 0;
    for (size_t i = 0; i < config->targetIds.size(); ++i) {
        const data::ChallengeTarget* target = tables.challengeTarget(config->targetIds[i]);
        if (target == nullptr) continue;
        bool met = false;
        switch (target->kind) {
            case data::ChallengeTarget::Kind::RoundsLeft:
                met = run.roundsLeft >= target->param;
                break;
            case data::ChallengeTarget::Kind::DeadAvatar:
                met = run.deadAvatars == 0;
                break;
            case data::ChallengeTarget::Kind::TotalScore:
                met = run.totalScore() >= target->param;
                break;
        }
        if (met) mask |= uint32_t{1} << i;
    }
    return mask;
}

namespace {

void pushLineup(net::Session& session, const Player& player) {
    uint32_t stage = player.challenge().stage;

    proto::ChallengeLineupNotify swap;
    swap.extra_lineup_type = lineupTypeOf(stage);
    session.send(cmd::ChallengeLineupNotify, swap);

    proto::SyncLineupNotify sync;
    sync.lineup = lineup(player, stage);
    session.send(cmd::SyncLineupNotify, sync);
}

void settle(net::Session& session, Player& player, bool win) {
    ChallengeRun& run = player.challenge();
    run.status = win ? proto::ChallengeStatus::CHALLENGE_FINISH
                     : proto::ChallengeStatus::CHALLENGE_FAILED;
    run.stars = win ? stars(player) : 0;

    proto::ChallengeSettleNotify notify;
    notify.challenge_id = run.challengeId;
    notify.is_win = win;
    notify.star = run.stars;
    notify.challenge_score = run.score[0];
    notify.score_two = run.score[1];
    notify.cur_challenge = current(player);
    // Every reward is already reported as taken, so there is nothing to hand over.
    notify.reward.emplace();
    session.send(cmd::ChallengeSettleNotify, notify);

    logging::info("challenge", "floor {} {} with {} star(s), score {}", run.challengeId,
                  win ? "cleared" : "failed", run.stars, run.totalScore());
}

// Walks the run on to its second node and pushes the new arena.
bool advance(net::Session& session, Player& player) {
    ChallengeRun& run = player.challenge();
    ++run.stage;

    proto::SceneInfo scene;
    if (!enterArena(player, scene)) {
        logging::warn("challenge", "floor {} has no second arena", run.challengeId);
        --run.stage;
        return false;
    }
    pushLineup(session, player);

    proto::EnterSceneByServerScNotify notify;
    notify.scene = std::move(scene);
    notify.lineup = lineup(player, run.stage);
    session.send(cmd::EnterSceneByServerScNotify, notify);
    return true;
}

}  // namespace

bool battleFinished(net::Session& session, Player& player,
                    const proto::PVEBattleResultCsReq& req) {
    ChallengeRun& run = player.challenge();
    const data::ChallengeInfo* config = configOf(player);
    if (config == nullptr) return false;

    // Pure Fiction and Apocalyptic Shadow report a running total, so this node's share
    // is whatever is above what the earlier one banked.
    if (req.stt) {
        uint32_t total = req.stt->challenge_score;
        uint32_t before = run.totalScore();
        run.score[half(run.stage)] = total > before ? total - before : 0;
    }

    if (req.end_status != proto::BattleEndStatus::BATTLE_END_WIN) {
        // Backing out of a node is not losing the floor; the client stays put.
        if (req.end_status != proto::BattleEndStatus::BATTLE_END_QUIT) {
            settle(session, player, false);
        }
        return true;
    }

    if (req.stt) {
        for (const proto::AvatarBattleInfo& avatar : req.stt->battle_avatar_list) {
            if (avatar.avatar_status && avatar.avatar_status->left_hp == 0) ++run.deadAvatars;
        }
        // MoC counts its cycles down across the whole floor, and never below one.
        run.roundsLeft = run.roundsLeft > req.stt->round_cnt ? run.roundsLeft - req.stt->round_cnt
                                                             : 1;
    }

    // Monsters still standing means this node is not over.
    for (const SceneEntity& entity : player.sceneState().entities()) {
        if (entity.kind == EntityKind::Monster) return true;
    }

    if (run.stage >= config->stageNum) {
        settle(session, player, true);
    } else if (config->kind != data::ChallengeKind::Boss) {
        // Apocalyptic Shadow's client asks for the second node itself; the other two
        // expect the server to walk them into it.
        advance(session, player);
    }
    return true;
}

bool nextPhase(net::Session& session, Player& player, proto::SceneInfo& out) {
    ChallengeRun& run = player.challenge();
    const data::ChallengeInfo* config = configOf(player);
    if (config == nullptr || run.stage >= config->stageNum) return false;

    ++run.stage;
    if (!enterArena(player, out)) {
        --run.stage;
        return false;
    }
    pushLineup(session, player);
    return true;
}

void leave(net::Session& session, Player& player) {
    ChallengeRun& run = player.challenge();
    if (!run.active) return;

    uint32_t entryId = run.origin.entryId;
    Position at = run.originPos;
    run = ChallengeRun{};

    proto::SceneInfo scene;
    if (!scene::load(player, entryId, 0, true, scene)) {
        logging::warn("challenge", "cannot put the player back at entry {}", entryId);
        return;
    }
    // scene::load lands on an anchor; the player left from wherever they were standing.
    player.position() = at;

    proto::EnterSceneByServerScNotify notify;
    notify.scene = std::move(scene);
    notify.lineup = scene::lineupInfo(player);
    session.send(cmd::EnterSceneByServerScNotify, notify);

    proto::SyncLineupNotify sync;
    sync.lineup = scene::lineupInfo(player);
    session.send(cmd::SyncLineupNotify, sync);
    player.saveNow();
}

}  // namespace challenge
}  // namespace game
