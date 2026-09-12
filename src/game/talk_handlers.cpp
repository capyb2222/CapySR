// NPC talk state.
//
// Before it will offer a talk option the client asks whether the party has already met
// the npcs standing around it, and each of these replies has to carry back the id it
// was asked about -- an empty reply reads as "npc 0", so every npc stays unmet and the
// interact prompt comes up with nothing in it. Everyone is met; nobody has taken a
// talk reward.
#include "game/handlers.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

void onGetFirstTalkNpc(net::Session& session, const proto::GetFirstTalkNpcCsReq& req) {
    proto::GetFirstTalkNpcScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t npcId : req.npc_id_list) {
        proto::FirstNpcTalkInfo info;
        info.npc_id = npcId;
        info.is_meet = true;
        rsp.npc_meet_status_list.push_back(info);
    }
    session.send(cmd::GetFirstTalkNpcScRsp, rsp);
}

// The same question for the npcs a cutscene introduces. Left unmet, as the official
// server does -- claiming otherwise skips the first-meet performance.
void onGetFirstTalkByPerformanceNpc(net::Session& session,
                                    const proto::GetFirstTalkByPerformanceNpcCsReq& req) {
    proto::GetFirstTalkByPerformanceNpcScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t performanceId : req.performance_id_list) {
        proto::NpcMeetByPerformanceStatus status;
        status.performance_id = performanceId;
        rsp.npc_meet_status_list.push_back(status);
    }
    session.send(cmd::GetFirstTalkByPerformanceNpcScRsp, rsp);
}

void onFinishFirstTalkNpc(net::Session& session, const proto::FinishFirstTalkNpcCsReq& req) {
    proto::FinishFirstTalkNpcScRsp rsp;
    rsp.retcode = 0;
    rsp.npc_id = req.npc_id;
    session.send(cmd::FinishFirstTalkNpcScRsp, rsp);
}

void onFinishFirstTalkByPerformanceNpc(net::Session& session,
                                       const proto::FinishFirstTalkByPerformanceNpcCsReq& req) {
    proto::FinishFirstTalkByPerformanceNpcScRsp rsp;
    rsp.retcode = 0;
    rsp.performance_id = req.performance_id;
    // Present but empty: there is no inventory to pay a first-meet reward into.
    rsp.reward.emplace();
    session.send(cmd::FinishFirstTalkByPerformanceNpcScRsp, rsp);
}

void onGetNpcTakenReward(net::Session& session, const proto::GetNpcTakenRewardCsReq& req) {
    proto::GetNpcTakenRewardScRsp rsp;
    rsp.retcode = 0;
    rsp.npc_id = req.npc_id;
    session.send(cmd::GetNpcTakenRewardScRsp, rsp);
}

}  // namespace

void registerTalkHandlers() {
    net::on<proto::GetFirstTalkNpcCsReq>(cmd::GetFirstTalkNpcCsReq, onGetFirstTalkNpc);
    net::on<proto::GetFirstTalkByPerformanceNpcCsReq>(cmd::GetFirstTalkByPerformanceNpcCsReq,
                                                      onGetFirstTalkByPerformanceNpc);
    net::on<proto::FinishFirstTalkNpcCsReq>(cmd::FinishFirstTalkNpcCsReq, onFinishFirstTalkNpc);
    net::on<proto::FinishFirstTalkByPerformanceNpcCsReq>(cmd::FinishFirstTalkByPerformanceNpcCsReq,
                                                         onFinishFirstTalkByPerformanceNpc);
    net::on<proto::GetNpcTakenRewardCsReq>(cmd::GetNpcTakenRewardCsReq, onGetNpcTakenReward);
}

}  // namespace game
