// Anomaly Arbitration.
//
// The overview is one GetChallengePeakData: every season, its three knights and its
// boss, with the last team used on each. Starting a fight answers with a bare retcode --
// the arena has to go out first, as EnterSceneByServerScNotify, or the client is left
// on a black screen. The result is a settle notify off the battle result, and a season
// is sent again after anything that changes it.
#include <utility>
#include <vector>

#include "core/logger.h"
#include "game/handlers.h"
#include "game/peak.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

Player* playerOf(net::Session& session, const char* what) {
    Player* player = session.player();
    if (player == nullptr) logging::warn("game", "{} before login", what);
    return player;
}

std::vector<uint32_t> nonZero(const std::vector<uint32_t>& ids) {
    std::vector<uint32_t> out;
    for (uint32_t id : ids) {
        if (id != 0) out.push_back(id);
    }
    return out;
}

// The arena, then the team fighting in it, then the season with that team in its slot.
void pushArena(net::Session& session, Player& player, proto::SceneInfo& scene) {
    proto::SyncLineupNotify sync;
    sync.lineup = peak::lineup(player);
    session.send(cmd::SyncLineupNotify, sync);

    proto::EnterSceneByServerScNotify notify;
    notify.scene = std::move(scene);
    notify.lineup = peak::lineup(player);
    session.send(cmd::EnterSceneByServerScNotify, notify);

    peak::pushGroup(session, player, player.peak().groupId);
}

void onGetChallengePeakData(net::Session& session, const proto::GetChallengePeakDataCsReq&) {
    Player* player = playerOf(session, "GetChallengePeakData");
    if (player == nullptr) return;
    session.send(cmd::GetChallengePeakDataScRsp, peak::overview(*player));
}

void onGetCurChallengePeak(net::Session& session, const proto::GetCurChallengePeakCsReq&) {
    Player* player = playerOf(session, "GetCurChallengePeak");
    if (player == nullptr) return;

    proto::GetCurChallengePeakScRsp rsp;
    rsp.retcode = 0;
    const PeakRun& run = player->peak();
    if (run.active) {
        rsp.peak_id = run.peakId;
        rsp.boss_buff_id = run.buffId;
        rsp.has_passed = true;
    }
    session.send(cmd::GetCurChallengePeakScRsp, rsp);
}

void onSetChallengePeakMobLineupAvatar(net::Session& session,
                                       const proto::SetChallengePeakMobLineupAvatarCsReq& req) {
    Player* player = playerOf(session, "SetChallengePeakMobLineupAvatar");
    if (player == nullptr) return;

    PeakProgress& progress = player->peakProgress();
    for (const proto::ChallengePeakLineup& lineup : req.lineup_list) {
        if (lineup.peak_id != 0) progress.teams[lineup.peak_id] = nonZero(lineup.peak_avatar_id_list);
    }
    player->saveNow();
    peak::pushGroup(session, *player, req.peak_group_id);

    proto::SetChallengePeakMobLineupAvatarScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::SetChallengePeakMobLineupAvatarScRsp, rsp);
}

void onSetChallengePeakBossHardMode(net::Session& session,
                                    const proto::SetChallengePeakBossHardModeCsReq& req) {
    Player* player = playerOf(session, "SetChallengePeakBossHardMode");
    if (player == nullptr) return;

    // Kept per season and across restarts: the toggle is set before the boss starts.
    std::set<uint32_t>& hard = player->peakProgress().hardGroups;
    if (req.is_hard_mode) {
        hard.insert(req.peak_group_id);
    } else {
        hard.erase(req.peak_group_id);
    }
    player->saveNow();

    proto::SetChallengePeakBossHardModeScRsp rsp;
    rsp.retcode = 0;
    rsp.peak_group_id = req.peak_group_id;
    rsp.is_hard_mode = req.is_hard_mode;
    session.send(cmd::SetChallengePeakBossHardModeScRsp, rsp);
}

void onStartChallengePeak(net::Session& session, const proto::StartChallengePeakCsReq& req) {
    Player* player = playerOf(session, "StartChallengePeak");
    if (player == nullptr) return;

    proto::SceneInfo scene;
    proto::StartChallengePeakScRsp rsp;
    rsp.retcode = peak::start(*player, req.peak_id, req.boss_buff_id,
                              nonZero(req.peak_avatar_id_list), scene);
    if (rsp.retcode == 0) pushArena(session, *player, scene);
    session.send(cmd::StartChallengePeakScRsp, rsp);
}

void onReStartChallengePeak(net::Session& session, const proto::ReStartChallengePeakCsReq&) {
    Player* player = playerOf(session, "ReStartChallengePeak");
    if (player == nullptr) return;

    // The result screen's retry: the same fight, team and buff, from the entrance.
    proto::ReStartChallengePeakScRsp rsp;
    const PeakRun& run = player->peak();
    if (!run.active) {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_DOING);
    } else {
        proto::SceneInfo scene;
        rsp.retcode = peak::start(*player, run.peakId, run.buffId, run.party, scene);
        if (rsp.retcode == 0) pushArena(session, *player, scene);
    }
    session.send(cmd::ReStartChallengePeakScRsp, rsp);
}

void onLeaveChallengePeak(net::Session& session, const proto::LeaveChallengePeakCsReq&) {
    Player* player = playerOf(session, "LeaveChallengePeak");
    if (player == nullptr) return;

    bool wasRunning = player->peak().active;
    uint32_t groupId = player->peak().groupId;
    peak::leave(session, *player);

    proto::LeaveChallengePeakScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::LeaveChallengePeakScRsp, rsp);

    // Back on the overview, which refills its team slots from the season.
    if (wasRunning) peak::pushGroup(session, *player, groupId);
}

void onConfirmChallengePeakSettle(net::Session& session,
                                  const proto::ConfirmChallengePeakSettleCsReq& req) {
    proto::ConfirmChallengePeakSettleScRsp rsp;
    rsp.retcode = 0;
    rsp.peak_id = req.peak_id;
    session.send(cmd::ConfirmChallengePeakSettleScRsp, rsp);
}

void onTakeChallengePeakReward(net::Session& session,
                               const proto::TakeChallengePeakRewardCsReq& req) {
    // A cleared season already reports every reward as taken, but the request still has
    // to complete or the screen hangs.
    proto::TakeChallengePeakRewardScRsp rsp;
    rsp.retcode = 0;
    rsp.peak_group_id = req.peak_group_id;
    session.send(cmd::TakeChallengePeakRewardScRsp, rsp);
}

}  // namespace

void registerPeakHandlers() {
    net::on<proto::GetChallengePeakDataCsReq>(cmd::GetChallengePeakDataCsReq,
                                              onGetChallengePeakData);
    net::on<proto::GetCurChallengePeakCsReq>(cmd::GetCurChallengePeakCsReq, onGetCurChallengePeak);
    net::on<proto::SetChallengePeakMobLineupAvatarCsReq>(cmd::SetChallengePeakMobLineupAvatarCsReq,
                                                         onSetChallengePeakMobLineupAvatar);
    net::on<proto::SetChallengePeakBossHardModeCsReq>(cmd::SetChallengePeakBossHardModeCsReq,
                                                      onSetChallengePeakBossHardMode);
    net::on<proto::StartChallengePeakCsReq>(cmd::StartChallengePeakCsReq, onStartChallengePeak);
    net::on<proto::ReStartChallengePeakCsReq>(cmd::ReStartChallengePeakCsReq,
                                              onReStartChallengePeak);
    net::on<proto::LeaveChallengePeakCsReq>(cmd::LeaveChallengePeakCsReq, onLeaveChallengePeak);
    net::on<proto::ConfirmChallengePeakSettleCsReq>(cmd::ConfirmChallengePeakSettleCsReq,
                                                    onConfirmChallengePeakSettle);
    net::on<proto::TakeChallengePeakRewardCsReq>(cmd::TakeChallengePeakRewardCsReq,
                                                 onTakeChallengePeakReward);
}

}  // namespace game
