#include "game/peak.h"

#include <algorithm>
#include <utility>

#include "core/config.h"
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

// Season one's arena. A season newer than the scene dump fights here instead: the
// floor is only a backdrop for one monster, and the stage comes from the fight.
constexpr uint32_t kFallbackEntrance = 3013501;
constexpr uint32_t kFallbackMazeGroup = 8;

// The battle target slot a fight's own targets go in.
constexpr uint32_t kTargetSlot = 5;

bool cleared() { return core::Config::get().gameplay.unlockAllChallenges; }

const data::PeakInfo* configOf(const Player& player) {
    const PeakRun& run = player.peak();
    if (!run.active) return nullptr;
    return data::Tables::get().peak(run.peakId);
}

// Hard mode judges the boss by one target of its own.
std::vector<uint32_t> targetsOf(const data::PeakInfo& config, bool hard) {
    if (hard && config.hardTarget != 0) return {config.hardTarget};
    return config.targetIds;
}

// The most cycles a clear can take and still meet a target.
uint32_t turnBudget(const std::vector<uint32_t>& targets) {
    uint32_t budget = 0;
    for (uint32_t id : targets) {
        const data::BattleTargetInfo* target = data::Tables::get().battleTarget(id);
        if (target != nullptr && !target->countsDeaths) budget = std::max(budget, target->param);
    }
    return budget;
}

const std::vector<uint32_t>& teamOf(const PeakProgress& progress, uint32_t peakId) {
    static const std::vector<uint32_t> kNone;
    auto it = progress.teams.find(peakId);
    return it == progress.teams.end() ? kNone : it->second;
}

// The boss buff last picked, or the first on offer so the slot is never empty.
uint32_t bossBuffOf(const PeakProgress& progress, const data::PeakInfo& boss) {
    auto it = progress.bossBuffs.find(boss.id);
    if (it != progress.bossBuffs.end() && it->second != 0) return it->second;
    return boss.bossBuffs.empty() ? 0 : boss.bossBuffs.front();
}

std::vector<proto::ChallengePeakRecordAvatar> recordAvatars(const std::vector<uint32_t>& team) {
    std::vector<proto::ChallengePeakRecordAvatar> out;
    for (uint32_t avatarId : team) {
        proto::ChallengePeakRecordAvatar entry;
        entry.avatar_id = avatarId;
        out.push_back(entry);
    }
    return out;
}

proto::ChallengePeakBossClearance clearance(const PeakProgress& progress,
                                            const data::PeakInfo& boss, bool hard) {
    proto::ChallengePeakBossClearance out;
    const PeakRecord* record = progress.record(boss.id, hard);
    out.has_passed = record != nullptr || cleared();
    // Both difficulties show the same team and buff; the client fills its slots from them.
    out.peak_avatar_id_list = teamOf(progress, boss.id);
    out.buff_id = bossBuffOf(progress, boss);
    if (record != nullptr) {
        uint32_t budget = turnBudget(targetsOf(boss, hard));
        out.best_cycle_count = record->cycles;
        out.best_record_buff_id = record->buffId;
        out.max_left_turn = budget > record->cycles ? budget - record->cycles : 0;
        out.best_record_avatar_list = recordAvatars(record->team);
    }
    return out;
}

bool enterArena(Player& player, proto::SceneInfo& out) {
    const data::PeakInfo* config = configOf(player);
    if (config == nullptr) return false;

    PeakRun& run = player.peak();
    run.entryId = config->mapEntranceId;
    run.mazeGroupId = config->mazeGroupId;
    if (run.entryId == 0 || data::SceneRes::get().byEntry(run.entryId) == nullptr) {
        logging::info("peak", "fight {}: arena {} is not in the scene dump, using {}",
                      config->id, run.entryId, kFallbackEntrance);
        run.entryId = kFallbackEntrance;
        run.mazeGroupId = kFallbackMazeGroup;
    }

    // Every attempt starts at the entrance, not wherever the last fight ended.
    SceneLocation before = player.location();
    player.location().floorId = 0;
    ChallengeArena storage;
    if (scene::load(player, run.entryId, 0, true, out, peak::arena(player, storage))) return true;
    player.location() = before;
    return false;
}

}  // namespace

namespace peak {

proto::ChallengePeakGroup group(const Player& player, uint32_t groupId) {
    const data::Tables& tables = data::Tables::get();
    const PeakProgress& progress = player.peakProgress();

    proto::ChallengePeakGroup out;
    out.peak_group_id = groupId;
    // Read the other way round by the client: set means the boss is on normal.
    out.disable_hard_mode = progress.hardGroups.count(groupId) == 0;

    const data::PeakGroupInfo* info = tables.peakGroup(groupId);
    if (info == nullptr) return out;

    uint32_t stars = 0;
    proto::ChallengePeakBestRecordList best;
    for (uint32_t mobId : info->mobIds) {
        const data::PeakInfo* mob = tables.peak(mobId);
        if (mob == nullptr) continue;
        const PeakRecord* record = progress.record(mobId, false);

        proto::ChallengePeak entry;
        entry.peak_id = mobId;
        entry.has_passed = record != nullptr || cleared();
        entry.peak_avatar_id_list = teamOf(progress, mobId);
        if (record != nullptr) {
            entry.finished_target_list = record->targets;
            entry.cycles_used = record->cycles;
        } else if (cleared()) {
            entry.finished_target_list = mob->targetIds;
        }
        stars += static_cast<uint32_t>(entry.finished_target_list.size());

        if (entry.has_passed) {
            proto::ChallengePeakBestRecord clear;
            clear.peak_id = mobId;
            clear.cycles_used = entry.cycles_used;
            clear.finished_target_list = entry.finished_target_list;
            clear.peak_avatar_id_list =
                recordAvatars(record != nullptr ? record->team : entry.peak_avatar_id_list);
            best.peaks.push_back(std::move(clear));
        }
        out.peaks.push_back(std::move(entry));
    }
    out.count_of_peaks = static_cast<uint32_t>(out.peaks.size());
    // A full clear is three stars a knight, whatever the records say.
    out.obtained_stars = cleared() ? std::max(stars, out.count_of_peaks * 3) : stars;
    if (!best.peaks.empty()) out.best_record_list = std::move(best);
    if (cleared()) {
        // Reported as taken: there is no inventory to pay them into.
        if (const std::vector<uint32_t>* rewards = tables.peakRewards(info->rewardGroupId)) {
            out.taken_star_rewards = *rewards;
        }
    }

    const data::PeakInfo* boss = tables.peak(info->bossId);
    if (boss == nullptr) return out;
    const PeakRecord* easy = progress.record(boss->id, false);
    auto& bossInfo = out.peak_boss.emplace();
    bossInfo.hard_mode_has_passed = progress.record(boss->id, true) != nullptr || cleared();
    bossInfo.easy_mode = clearance(progress, *boss, false);
    bossInfo.hard_mode = clearance(progress, *boss, true);
    if (easy != nullptr) {
        bossInfo.finished_target_list = easy->targets;
    } else if (cleared()) {
        bossInfo.finished_target_list = boss->targetIds;
        if (boss->hardTarget != 0) bossInfo.finished_target_list.push_back(boss->hardTarget);
    }
    return out;
}

proto::GetChallengePeakDataScRsp overview(const Player& player) {
    proto::GetChallengePeakDataScRsp rsp;
    rsp.retcode = 0;
    for (const data::PeakGroupInfo& info : data::Tables::get().peakGroups()) {
        rsp.challenge_peak_groups.push_back(group(player, info.id));
        // The newest season is the one on show.
        rsp.current_peak_group_id = info.id;
    }
    return rsp;
}

void pushGroup(net::Session& session, const Player& player, uint32_t groupId) {
    proto::ChallengePeakGroupDataUpdateScNotify notify;
    notify.challenge_peak_group = group(player, groupId);
    session.send(cmd::ChallengePeakGroupDataUpdateScNotify, notify);
}

uint32_t start(Player& player, uint32_t peakId, uint32_t buffId, std::vector<uint32_t> team,
               proto::SceneInfo& out) {
    const data::PeakInfo* config = data::Tables::get().peak(peakId);
    if (config == nullptr || config->monsters.empty()) {
        return fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
    }

    PeakProgress& progress = player.peakProgress();
    if (team.empty()) team = teamOf(progress, peakId);
    if (team.empty()) return fail(proto::Retcode::RET_CHALLENGE_LINEUP_EMPTY);
    if (config->boss && buffId == 0) buffId = bossBuffOf(progress, *config);
    progress.teams[peakId] = team;
    if (config->boss) progress.bossBuffs[peakId] = buffId;

    PeakRun run;
    run.active = true;
    run.peakId = peakId;
    run.groupId = config->groupId;
    run.hard = config->boss && progress.hardGroups.count(config->groupId) != 0;
    run.buffId = config->boss ? buffId : 0;
    run.party = std::move(team);
    // A retry keeps the spot the first attempt left from.
    const PeakRun& previous = player.peak();
    run.origin = previous.active ? previous.origin : player.location();
    run.originPos = previous.active ? previous.originPos : player.position();

    PeakRun saved = player.peak();
    player.peak() = std::move(run);
    if (!enterArena(player, out)) {
        player.peak() = std::move(saved);
        return fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
    }

    logging::info("peak", "fight {} of season {} started{}, stage {}", peakId, config->groupId,
                  player.peak().hard ? " on hard" : "",
                  player.peak().hard && config->hardEventId != 0 ? config->hardEventId
                                                                 : config->eventId);
    return 0;
}

proto::LineupInfo lineup(const Player& player) {
    return challenge::extraLineup(player, player.peak().party,
                                  proto::ExtraLineupType::ExtraLineupType_LineupChallenge);
}

const ChallengeArena* arena(const Player& player, ChallengeArena& storage) {
    const data::PeakInfo* config = configOf(player);
    if (config == nullptr) return nullptr;
    const PeakRun& run = player.peak();
    storage.mazeGroupId = run.mazeGroupId;
    storage.monsters = run.hard && !config->hardMonsters.empty() ? &config->hardMonsters
                                                                 : &config->monsters;
    storage.party = &run.party;
    return &storage;
}

void prepareBattle(const Player& player, BattleRequest& request) {
    const data::PeakInfo* config = configOf(player);
    if (config == nullptr) return;
    const PeakRun& run = player.peak();

    request.party = run.party;
    // The enemy tags are not sent back by the client; they come from the fight's config.
    request.floorBuffIds = run.hard ? config->hardTagBuffs : config->tagBuffs;
    if (run.buffId != 0) request.floorBuffIds.push_back(run.buffId);
    request.battleType = "AA";
    request.battleTargetIds = targetsOf(*config, run.hard);
}

bool battleFinished(net::Session& session, Player& player,
                    const proto::PVEBattleResultCsReq& req) {
    const data::PeakInfo* config = configOf(player);
    if (config == nullptr) return false;
    const PeakRun& run = player.peak();

    // Backing out of the fight is not losing it; the client stays in the arena.
    if (req.end_status == proto::BattleEndStatus::BATTLE_END_QUIT) return true;

    proto::ChallengePeakSettleScNotify notify;
    notify.peak_id = run.peakId;
    notify.is_win = req.end_status == proto::BattleEndStatus::BATTLE_END_WIN;
    if (!notify.is_win) {
        session.send(cmd::ChallengePeakSettleScNotify, notify);
        logging::info("peak", "fight {} lost", run.peakId);
        return true;
    }

    uint32_t cycles = req.stt ? req.stt->round_cnt : 0;
    uint32_t deaths = 0;
    const proto::BattleTargetList* reported = nullptr;
    if (req.stt) {
        for (const proto::AvatarBattleInfo& avatar : req.stt->battle_avatar_list) {
            if (avatar.avatar_status && avatar.avatar_status->left_hp == 0) ++deaths;
        }
        auto it = req.stt->battle_target_info.find(kTargetSlot);
        if (it != req.stt->battle_target_info.end()) reported = &it->second;
    }

    // The client's own count wins; without one, judge by cycles used and avatars lost.
    std::vector<uint32_t> targets = targetsOf(*config, run.hard);
    std::vector<uint32_t> finished;
    for (uint32_t id : targets) {
        const data::BattleTargetInfo* target = data::Tables::get().battleTarget(id);
        if (target == nullptr) continue;
        uint32_t progress = target->countsDeaths ? deaths : cycles;
        if (reported != nullptr) {
            for (const proto::BattleTarget& entry : reported->battle_target_list) {
                if (entry.id == id) progress = entry.progress;
            }
        }
        if (progress <= target->param) finished.push_back(id);
    }

    PeakProgress& progress = player.peakProgress();
    notify.cycles_used = cycles;
    notify.is_first_pass = progress.record(run.peakId, run.hard) == nullptr;
    if (config->boss && run.hard) {
        // A hard clear has a result screen of its own, with no targets on it.
        notify.hard_mode_has_passed = true;
    } else {
        notify.finished_target_list = finished;
        if (config->boss) {
            uint32_t budget = turnBudget(config->targetIds);
            notify.turn_left = budget > cycles ? budget - cycles : 0;
        }
    }
    session.send(cmd::ChallengePeakSettleScNotify, notify);

    // Keep the better clear: more targets, then fewer cycles.
    uint32_t key = PeakProgress::key(run.peakId, run.hard);
    auto it = progress.records.find(key);
    if (it == progress.records.end() || finished.size() > it->second.targets.size() ||
        (finished.size() == it->second.targets.size() && cycles < it->second.cycles)) {
        progress.records[key] = PeakRecord{cycles, finished, run.party, run.buffId};
    }
    pushGroup(session, player, run.groupId);

    logging::info("peak", "fight {} cleared{} in {} cycle(s), {} of {} target(s)", run.peakId,
                  run.hard ? " on hard" : "", cycles, finished.size(), targets.size());
    return true;
}

void leave(net::Session& session, Player& player) {
    PeakRun& run = player.peak();
    if (!run.active) return;

    uint32_t entryId = run.origin.entryId;
    Position at = run.originPos;
    run = PeakRun{};
    challenge::putBack(session, player, entryId, at);
}

}  // namespace peak
}  // namespace game
