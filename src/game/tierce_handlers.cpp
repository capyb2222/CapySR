// The three-node challenge floor.
//
// StartChallengeTierce answers with the run and the arena in one response -- unlike
// Anomaly Arbitration, this one carries its SceneInfo in the reply rather than in a
// notify, and the same goes for the next node and a restart. Clearing a node is
// reported with ChallengeTierceSyncNotify; the client then asks for the node after it.
#include <utility>

#include "core/logger.h"
#include "game/handlers.h"
#include "game/player.h"
#include "game/tierce.h"
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

// The team for the node being entered, so the client swaps to it before the scene.
void pushLineup(net::Session& session, const Player& player) {
    uint32_t stage = player.tierce().stage;

    proto::ChallengeLineupNotify swap;
    swap.extra_lineup_type = stage >= 2
                                 ? proto::ExtraLineupType::ExtraLineupType_LineupChallenge3
                             : stage == 1
                                 ? proto::ExtraLineupType::ExtraLineupType_LineupChallenge2
                                 : proto::ExtraLineupType::ExtraLineupType_LineupChallenge;
    session.send(cmd::ChallengeLineupNotify, swap);

    proto::SyncLineupNotify sync;
    sync.lineup = tierce::lineup(player, stage);
    session.send(cmd::SyncLineupNotify, sync);
}

void onGetChallengeTierceData(net::Session& session, const proto::GetChallengeTierceDataCsReq&) {
    Player* player = playerOf(session, "GetChallengeTierceData");
    if (player == nullptr) return;
    session.send(cmd::GetChallengeTierceDataScRsp, tierce::history(*player));
}

// Asked every login. Carries the run in progress so a reconnect lands back in it.
void onGetChallengeTierceController(net::Session& session,
                                    const proto::GetChallengeTierceControllerCsReq&) {
    Player* player = playerOf(session, "GetChallengeTierceController");
    if (player == nullptr) return;

    proto::GetChallengeTierceControllerScRsp rsp;
    rsp.retcode = 0;
    if (player->tierce().active) {
        rsp.challenge_tierce_info = tierce::current(*player);
    }
    session.send(cmd::GetChallengeTierceControllerScRsp, rsp);
}

void onSetChallengeTierceLineup(net::Session& session,
                                const proto::SetChallengeTierceLineupCsReq& req) {
    Player* player = playerOf(session, "SetChallengeTierceLineup");
    if (player == nullptr) return;

    tierce::setLineups(*player, req.challenge_id, req.stage_info_list);

    proto::SetChallengeTierceLineupScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::SetChallengeTierceLineupScRsp, rsp);
}

void onStartChallengeTierce(net::Session& session, const proto::StartChallengeTierceCsReq& req) {
    Player* player = playerOf(session, "StartChallengeTierce");
    if (player == nullptr) return;

    proto::StartChallengeTierceScRsp rsp;
    proto::SceneInfo scene;
    rsp.retcode = tierce::start(*player, req.challenge_id, req.is_single_stage, req.stage_index,
                                req.stage_info_list, scene);
    if (rsp.retcode != 0) {
        session.send(cmd::StartChallengeTierceScRsp, rsp);
        return;
    }

    pushLineup(session, *player);
    rsp.challenge_tierce_info = tierce::current(*player);
    rsp.scene = std::move(scene);
    session.send(cmd::StartChallengeTierceScRsp, rsp);
}

void onStartNextChallengeTierce(net::Session& session,
                                const proto::StartNextChallengeTierceCsReq&) {
    Player* player = playerOf(session, "StartNextChallengeTierce");
    if (player == nullptr) return;

    proto::StartNextChallengeTierceScRsp rsp;
    proto::SceneInfo scene;
    if (!tierce::nextStage(session, *player, scene)) {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
        session.send(cmd::StartNextChallengeTierceScRsp, rsp);
        return;
    }
    rsp.retcode = 0;
    rsp.challenge_tierce_info = tierce::current(*player);
    rsp.scene = std::move(scene);
    session.send(cmd::StartNextChallengeTierceScRsp, rsp);
}

void onRestartChallengeTierce(net::Session& session, const proto::RestartChallengeTierceCsReq&) {
    Player* player = playerOf(session, "RestartChallengeTierce");
    if (player == nullptr) return;

    proto::RestartChallengeTierceScRsp rsp;
    proto::SceneInfo scene;
    if (!tierce::restart(session, *player, scene)) {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
        session.send(cmd::RestartChallengeTierceScRsp, rsp);
        return;
    }
    rsp.retcode = 0;
    rsp.GFACJIPNIAN = std::move(scene);
    session.send(cmd::RestartChallengeTierceScRsp, rsp);
}

// The settle screen closing. Nothing to hand over -- rewards are already taken.
void onConfirmChallengeTierceStageSettle(
    net::Session& session, const proto::ConfirmChallengeTierceStageSettleCsReq&) {
    proto::ConfirmChallengeTierceStageSettleScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::ConfirmChallengeTierceStageSettleScRsp, rsp);
}

void onLeaveChallengeTierce(net::Session& session, const proto::LeaveChallengeTierceCsReq&) {
    Player* player = playerOf(session, "LeaveChallengeTierce");
    if (player == nullptr) return;

    tierce::leave(session, *player);

    proto::LeaveChallengeTierceScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::LeaveChallengeTierceScRsp, rsp);
}

}  // namespace

void registerTierceHandlers() {
    net::on<proto::GetChallengeTierceDataCsReq>(cmd::GetChallengeTierceDataCsReq,
                                                onGetChallengeTierceData);
    net::on<proto::GetChallengeTierceControllerCsReq>(cmd::GetChallengeTierceControllerCsReq,
                                                      onGetChallengeTierceController);
    net::on<proto::SetChallengeTierceLineupCsReq>(cmd::SetChallengeTierceLineupCsReq,
                                                  onSetChallengeTierceLineup);
    net::on<proto::StartChallengeTierceCsReq>(cmd::StartChallengeTierceCsReq,
                                              onStartChallengeTierce);
    net::on<proto::StartNextChallengeTierceCsReq>(cmd::StartNextChallengeTierceCsReq,
                                                  onStartNextChallengeTierce);
    net::on<proto::RestartChallengeTierceCsReq>(cmd::RestartChallengeTierceCsReq,
                                                onRestartChallengeTierce);
    net::on<proto::ConfirmChallengeTierceStageSettleCsReq>(
        cmd::ConfirmChallengeTierceStageSettleCsReq, onConfirmChallengeTierceStageSettle);
    net::on<proto::LeaveChallengeTierceCsReq>(cmd::LeaveChallengeTierceCsReq,
                                              onLeaveChallengeTierce);
}

}  // namespace game
