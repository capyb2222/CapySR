// Memory of Chaos, Pure Fiction and Apocalyptic Shadow.
//
// The history goes out as a full clear: star is a bitmask over the floor's
// ChallengeTargetID entries, and the client locks a floor whose predecessor has none, so
// an empty GetChallengeScRsp leaves every mode showing floor 1 and nothing else. Rewards
// are reported as already taken -- there is no inventory to pay them into, and a
// claimable reward would be a button that does nothing.
//
// Running a floor is the rest of this file. The client sends both teams with the start
// request, so there is no separate lineup to edit: the run holds them, the arena is the
// floor with everything but the maze config's own monsters switched off, and each node's
// battle stage comes from the monster's event id rather than from PlaneEvent.
#include "core/logger.h"
#include "data/excel.h"
#include "game/challenge.h"
#include "game/handlers.h"
#include "game/player.h"
#include "game/scene.h"
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

// 4.3 moved the two teams from the packed uint32 lists into AvatarIdentifier lists; the
// client still fills whichever it was built against, so read both.
std::vector<uint32_t> teamOf(const std::vector<proto::AvatarIdentifier>& identifiers,
                             const std::vector<uint32_t>& packed) {
    std::vector<uint32_t> team;
    for (const proto::AvatarIdentifier& entry : identifiers) {
        if (entry.id != 0) team.push_back(entry.id);
    }
    if (team.empty()) {
        for (uint32_t id : packed) {
            if (id != 0) team.push_back(id);
        }
    }
    return team;
}

void onGetChallenge(net::Session& session, const proto::GetChallengeCsReq&) {
    session.send(cmd::GetChallengeScRsp, challenge::history());
}

void onStartChallenge(net::Session& session, const proto::StartChallengeCsReq& req) {
    Player* player = playerOf(session, "StartChallenge");
    if (player == nullptr) return;

    proto::StartChallengeScRsp rsp;
    const data::ChallengeInfo* config = data::Tables::get().challenge(req.challenge_id);
    if (config == nullptr) {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
        session.send(cmd::StartChallengeScRsp, rsp);
        return;
    }

    ChallengeRun run;
    run.party[0] = teamOf(req.avatar_lineup_first, req.first_lineup);
    run.party[1] = teamOf(req.avatar_lineup_second, req.second_lineup);
    if (run.party[0].empty() || (config->stageNum >= 2 && run.party[1].empty())) {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_LINEUP_EMPTY);
        session.send(cmd::StartChallengeScRsp, rsp);
        return;
    }

    run.active = true;
    run.challengeId = config->id;
    run.stage = 1;
    run.status = proto::ChallengeStatus::CHALLENGE_DOING;
    run.roundsLeft = config->roundLimit;
    // Pure Fiction and Apocalyptic Shadow let the player pick a buff per half.
    if (req.stage_info) {
        if (req.stage_info->story_info) {
            run.buffs[0] = req.stage_info->story_info->buff_one;
            run.buffs[1] = req.stage_info->story_info->buff_two;
        } else if (req.stage_info->boss_info) {
            run.buffs[0] = req.stage_info->boss_info->buff_one;
            run.buffs[1] = req.stage_info->boss_info->buff_two;
        }
    }
    // Where to put the player back when the run ends.
    run.origin = player->location();
    run.originPos = player->position();

    ChallengeRun saved = player->challenge();
    player->challenge() = run;

    proto::SceneInfo scene;
    if (!challenge::enterArena(*player, scene)) {
        logging::warn("challenge", "floor {} names map entrance {}, which is not in the dump",
                      config->id, config->mapEntranceId);
        player->challenge() = saved;
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_EXIST);
        session.send(cmd::StartChallengeScRsp, rsp);
        return;
    }

    logging::info("challenge", "floor {} started: {} node(s), {} cycle(s), maze buff {}",
                  config->id, config->stageNum, config->roundLimit, config->mazeBuffId);

    proto::ChallengeLineupNotify swap;
    swap.extra_lineup_type = proto::ExtraLineupType::ExtraLineupType_LineupChallenge;
    session.send(cmd::ChallengeLineupNotify, swap);

    // The response carries the run and both teams; the scene follows as a notify, which
    // is the only order Apocalyptic Shadow accepts.
    rsp.retcode = 0;
    rsp.cur_challenge = challenge::current(*player);
    rsp.stage_info.emplace();
    rsp.lineup_list.push_back(challenge::lineup(*player, 1));
    rsp.lineup_list.push_back(challenge::lineup(*player, 2));
    session.send(cmd::StartChallengeScRsp, rsp);

    proto::EnterSceneByServerScNotify notify;
    notify.scene = std::move(scene);
    notify.lineup = challenge::lineup(*player, 1);
    session.send(cmd::EnterSceneByServerScNotify, notify);
}

void onGetCurChallenge(net::Session& session, const proto::GetCurChallengeCsReq&) {
    Player* player = playerOf(session, "GetCurChallenge");
    if (player == nullptr) return;

    proto::GetCurChallengeScRsp rsp;
    rsp.retcode = 0;
    if (player->challenge().active) {
        rsp.cur_challenge = challenge::current(*player);
        rsp.lineup_list.push_back(challenge::lineup(*player, 1));
        rsp.lineup_list.push_back(challenge::lineup(*player, 2));
    }
    session.send(cmd::GetCurChallengeScRsp, rsp);
}

void onEnterChallengeNextPhase(net::Session& session, const proto::EnterChallengeNextPhaseCsReq&) {
    Player* player = playerOf(session, "EnterChallengeNextPhase");
    if (player == nullptr) return;

    proto::EnterChallengeNextPhaseScRsp rsp;
    proto::SceneInfo scene;
    if (challenge::nextPhase(session, *player, scene)) {
        rsp.retcode = 0;
        rsp.scene = std::move(scene);
    } else {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_DOING);
    }
    session.send(cmd::EnterChallengeNextPhaseScRsp, rsp);
}

void onRestartChallengePhase(net::Session& session, const proto::RestartChallengePhaseCsReq&) {
    Player* player = playerOf(session, "RestartChallengePhase");
    if (player == nullptr) return;

    // Retrying a node puts the arena back exactly as it was entered, monsters included.
    proto::RestartChallengePhaseScRsp rsp;
    proto::SceneInfo scene;
    if (player->challenge().active && challenge::enterArena(*player, scene)) {
        rsp.retcode = 0;
        rsp.scene = std::move(scene);
    } else {
        rsp.retcode = fail(proto::Retcode::RET_CHALLENGE_NOT_DOING);
    }
    session.send(cmd::RestartChallengePhaseScRsp, rsp);
}

void onLeaveChallenge(net::Session& session, const proto::LeaveChallengeCsReq&) {
    Player* player = playerOf(session, "LeaveChallenge");
    if (player == nullptr) return;

    challenge::leave(session, *player);

    proto::LeaveChallengeScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::LeaveChallengeScRsp, rsp);
}

void onGetChallengeGroupStatistics(net::Session& session,
                                   const proto::GetChallengeGroupStatisticsCsReq& req) {
    // Nobody's records are kept, but the oneof has to be set to the shape that matches
    // the season or the client reads the wrong arm of it and throws.
    proto::GetChallengeGroupStatisticsScRsp rsp;
    rsp.retcode = 0;
    rsp.group_id = req.group_id;

    const data::ChallengeGroupInfo* season = data::Tables::get().challengeGroup(req.group_id);
    data::ChallengeKind kind = season != nullptr ? season->kind : data::ChallengeKind::Memory;
    switch (kind) {
        case data::ChallengeKind::Story:
            rsp.challenge_story.emplace();
            rsp.EDKOHAAMONH_case = proto::GetChallengeGroupStatisticsScRsp::k_challenge_story;
            break;
        case data::ChallengeKind::Boss:
            rsp.challenge_boss.emplace();
            rsp.EDKOHAAMONH_case = proto::GetChallengeGroupStatisticsScRsp::k_challenge_boss;
            break;
        case data::ChallengeKind::Memory:
            rsp.challenge_default.emplace();
            rsp.EDKOHAAMONH_case = proto::GetChallengeGroupStatisticsScRsp::k_challenge_default;
            break;
    }
    session.send(cmd::GetChallengeGroupStatisticsScRsp, rsp);
}

void onTakeChallengeReward(net::Session& session, const proto::TakeChallengeRewardCsReq& req) {
    // The history already reports every reward as taken, so there is nothing left to
    // hand over -- but the request still has to complete or the screen hangs.
    proto::TakeChallengeRewardScRsp rsp;
    rsp.retcode = 0;
    rsp.group_id = req.group_id;
    session.send(cmd::TakeChallengeRewardScRsp, rsp);
}

}  // namespace

void registerChallengeHandlers() {
    net::on<proto::GetChallengeCsReq>(cmd::GetChallengeCsReq, onGetChallenge);
    net::on<proto::StartChallengeCsReq>(cmd::StartChallengeCsReq, onStartChallenge);
    net::on<proto::GetCurChallengeCsReq>(cmd::GetCurChallengeCsReq, onGetCurChallenge);
    net::on<proto::EnterChallengeNextPhaseCsReq>(cmd::EnterChallengeNextPhaseCsReq,
                                                 onEnterChallengeNextPhase);
    net::on<proto::RestartChallengePhaseCsReq>(cmd::RestartChallengePhaseCsReq,
                                               onRestartChallengePhase);
    net::on<proto::LeaveChallengeCsReq>(cmd::LeaveChallengeCsReq, onLeaveChallenge);
    net::on<proto::GetChallengeGroupStatisticsCsReq>(cmd::GetChallengeGroupStatisticsCsReq,
                                                     onGetChallengeGroupStatistics);
    net::on<proto::TakeChallengeRewardCsReq>(cmd::TakeChallengeRewardCsReq, onTakeChallengeReward);
}

}  // namespace game
