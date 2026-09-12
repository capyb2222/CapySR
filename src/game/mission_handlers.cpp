// Missions, tutorials and quests, all reported as already done.
//
// With nothing finished the client believes it is still in the prologue: it takes the
// scene, plays the Astral Express intro and then waits for the opening mission to drive
// it somewhere, which reads as an infinite black screen. So every main mission is
// finished, every tutorial finished, every requested sub-mission echoed back as complete.
//
// This is also what unlocks the world: the client gates map regions on finished main
// missions.
#include "core/config.h"
#include "core/logger.h"
#include "data/excel.h"
#include "game/handlers.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

// Everything finished, bar the handful config names -- see gameplay.skip_missions.
std::vector<uint32_t> finishedMainMissions() {
    const core::GameplayConfig& gameplay = core::Config::get().gameplay;
    const std::vector<uint32_t>& all = data::Tables::get().mainMissions();
    if (gameplay.skipMissions.empty()) return all;
    std::vector<uint32_t> out;
    out.reserve(all.size());
    for (uint32_t id : all) {
        if (!gameplay.missionSkipped(id)) out.push_back(id);
    }
    return out;
}

void onGetMissionStatus(net::Session& session, const proto::GetMissionStatusCsReq& req) {
    proto::GetMissionStatusScRsp rsp;
    rsp.retcode = 0;
    // Whatever it asked about is finished. There are ~15k sub-missions in the tables,
    // so echoing the request is the only sane way to answer this.
    for (uint32_t id : req.sub_mission_id_list) {
        proto::Mission mission;
        mission.id = id;
        mission.status = proto::MissionStatus::MissionStatus_MissionFinish;
        mission.progress = 1;
        rsp.sub_mission_status_list.push_back(mission);
    }
    rsp.finished_main_mission_id_list = finishedMainMissions();
    // The client cross-checks this against the version it is running.
    rsp.curversion_finished_main_mission_id_list = rsp.finished_main_mission_id_list;
    session.send(cmd::GetMissionStatusScRsp, rsp);
}

// The login-time mission snapshot, asked for before the first scene. Everything done,
// same as the status query.
void onGetMissionData(net::Session& session, const proto::GetMissionDataCsReq&) {
    proto::GetMissionDataScRsp rsp;
    rsp.retcode = 0;
    rsp.finished_main_mission_id_list = finishedMainMissions();
    session.send(cmd::GetMissionDataScRsp, rsp);
}

void onGetTutorial(net::Session& session, const proto::GetTutorialCsReq&) {
    proto::GetTutorialScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t id : data::Tables::get().tutorials()) {
        proto::Tutorial tutorial;
        tutorial.id = id;
        tutorial.status = proto::TutorialStatus::TutorialStatus_TutorialFinish;
        rsp.tutorial_list.push_back(tutorial);
    }
    session.send(cmd::GetTutorialScRsp, rsp);
}

void onGetTutorialGuide(net::Session& session, const proto::GetTutorialGuideCsReq&) {
    proto::GetTutorialGuideScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t id : data::Tables::get().tutorialGuides()) {
        proto::TutorialGuide guide;
        guide.id = id;
        guide.status = proto::TutorialStatus::TutorialStatus_TutorialFinish;
        rsp.tutorial_guide_list.push_back(guide);
    }
    session.send(cmd::GetTutorialGuideScRsp, rsp);
}

void onGetQuestData(net::Session& session, const proto::GetQuestDataCsReq&) {
    proto::GetQuestDataScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t id : data::Tables::get().quests()) {
        proto::Quest quest;
        quest.id = id;
        quest.progress = 200;
        // Closed rather than finished, or the client offers thousands of unclaimed
        // rewards; only these four go out as finished.
        bool finished = id >= 2200503 && id <= 2200506;
        quest.status = finished ? proto::QuestStatus::QuestStatus_QuestFinish
                                : proto::QuestStatus::QuestStatus_QuestClose;
        rsp.quest_list.push_back(quest);
    }
    session.send(cmd::GetQuestDataScRsp, rsp);
}

// The client asks for these while loading a floor, and MapDef.InitActivateHoyoGroupByMission
// walks the answer to decide which scene groups are switched on. An empty reply left it
// dereferencing a mission that was not in the list, and the NullReferenceException took
// the rest of the group activation with it -- so props and monsters never appeared and
// there was nothing in the world to swing at.
void onGetMainMissionCustomValue(net::Session& session,
                                 const proto::GetMainMissionCustomValueCsReq& req) {
    proto::GetMainMissionCustomValueScRsp rsp;
    rsp.retcode = 0;
    for (uint32_t id : req.main_mission_id_list) {
        proto::MainMission mission;
        mission.id = id;
        mission.status = proto::MissionStatus::MissionStatus_MissionFinish;
        rsp.main_mission_list.push_back(std::move(mission));
    }
    session.send(cmd::GetMainMissionCustomValueScRsp, rsp);
}

// Which mission the compass points at. Echoed back so the marker settles instead of
// snapping back to whatever it tracked before.
void onUpdateTrackMainMission(net::Session& session,
                              const proto::UpdateTrackMainMissionCsReq& req) {
    proto::UpdateTrackMainMissionScRsp rsp;
    rsp.retcode = 0;
    rsp.track_mission_id = req.track_mission_id;
    session.send(cmd::UpdateTrackMainMissionScRsp, rsp);
}

void onFinishTalkMission(net::Session& session, const proto::FinishTalkMissionCsReq& req) {
    proto::FinishTalkMissionScRsp rsp;
    rsp.retcode = 0;
    rsp.sub_mission_id = req.sub_mission_id;
    rsp.talk_str = req.talk_str;
    rsp.custom_value_list = req.custom_value_list;
    session.send(cmd::FinishTalkMissionScRsp, rsp);
}

void onUnlockTutorial(net::Session& session, const proto::UnlockTutorialCsReq& req) {
    // Field 1 was never named in the dump; it is the tutorial id.
    uint32_t id = req.HPIMDINCCNN;
    logging::debug("game", "tutorial {} unlocked", id);

    proto::UnlockTutorialScRsp rsp;
    rsp.retcode = 0;
    auto& tutorial = rsp.tutorial.emplace();
    tutorial.id = id;
    tutorial.status = proto::TutorialStatus::TutorialStatus_TutorialFinish;
    session.send(cmd::UnlockTutorialScRsp, rsp);
}

void onUnlockTutorialGuide(net::Session& session, const proto::UnlockTutorialGuideCsReq& req) {
    logging::debug("game", "tutorial guide {} unlocked", req.group_id);

    proto::UnlockTutorialGuideScRsp rsp;
    rsp.retcode = 0;
    auto& guide = rsp.tutorial_guide.emplace();
    guide.id = req.group_id;
    guide.type = req.type;
    guide.status = proto::TutorialStatus::TutorialStatus_TutorialFinish;
    session.send(cmd::UnlockTutorialGuideScRsp, rsp);
}

}  // namespace

void registerMissionHandlers() {
    net::on<proto::GetMissionStatusCsReq>(cmd::GetMissionStatusCsReq, onGetMissionStatus);
    net::on<proto::GetMissionDataCsReq>(cmd::GetMissionDataCsReq, onGetMissionData);
    net::on<proto::GetTutorialCsReq>(cmd::GetTutorialCsReq, onGetTutorial);
    net::on<proto::GetTutorialGuideCsReq>(cmd::GetTutorialGuideCsReq, onGetTutorialGuide);
    net::on<proto::GetQuestDataCsReq>(cmd::GetQuestDataCsReq, onGetQuestData);
    net::on<proto::GetMainMissionCustomValueCsReq>(cmd::GetMainMissionCustomValueCsReq,
                                                   onGetMainMissionCustomValue);
    net::on<proto::UpdateTrackMainMissionCsReq>(cmd::UpdateTrackMainMissionCsReq,
                                                onUpdateTrackMainMission);
    net::on<proto::FinishTalkMissionCsReq>(cmd::FinishTalkMissionCsReq, onFinishTalkMission);
    net::on<proto::UnlockTutorialCsReq>(cmd::UnlockTutorialCsReq, onUnlockTutorial);
    net::on<proto::UnlockTutorialGuideCsReq>(cmd::UnlockTutorialGuideCsReq, onUnlockTutorialGuide);

    // The status request repeats constantly while walking around.
    net::Handlers::get().muteLog(cmd::GetMissionStatusCsReq);
    net::Handlers::get().muteLog(cmd::GetMissionStatusScRsp);
}

}  // namespace game
