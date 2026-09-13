#include "core/logger.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/challenge.h"
#include "game/handlers.h"
#include "game/peak.h"
#include "game/player.h"
#include "game/scene.h"
#include "game/tierce.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

// Star Rail's retcode for "that scene does not exist".
constexpr uint32_t kRetcodeSceneNotFound = 2605;

Player* playerOf(net::Session& session, const char* what) {
    Player* player = session.player();
    if (player == nullptr) logging::warn("game", "{} before login", what);
    return player;
}

void onGetCurSceneInfo(net::Session& session, const proto::GetCurSceneInfoCsReq&) {
    Player* player = playerOf(session, "GetCurSceneInfo");
    if (player == nullptr) return;

    proto::GetCurSceneInfoScRsp rsp;
    proto::SceneInfo info;
    // Rebuilding a challenge arena without its filter would repopulate the floor with
    // every monster the dump has in it.
    ChallengeArena storage;
    const ChallengeArena* arena = challenge::arena(*player, storage);
    if (arena == nullptr) arena = tierce::arena(*player, storage);
    if (arena == nullptr) arena = peak::arena(*player, storage);
    if (scene::load(*player, player->location().entryId, 0, false, info, arena)) {
        rsp.scene = std::move(info);
    } else {
        // Still answer with the bare location, or the client waits forever.
        auto& fallback = rsp.scene.emplace();
        fallback.entry_id = player->location().entryId;
        fallback.plane_id = player->location().planeId;
        fallback.floor_id = player->location().floorId;
        fallback.game_mode_type = 3;
        fallback.leader_entity_id = 1;
    }
    session.send(cmd::GetCurSceneInfoScRsp, rsp);
}

void onEnterScene(net::Session& session, const proto::EnterSceneCsReq& req) {
    Player* player = playerOf(session, "EnterScene");
    if (player == nullptr) return;
    logging::debug("scene", "enter {} teleport {} interact {} from entry {}", req.entry_id,
                   req.teleport_id, req.interact_id, player->location().entryId);

    proto::SceneInfo info;
    if (!scene::load(*player, req.entry_id, req.teleport_id, true, info)) {
        proto::EnterSceneScRsp rsp;
        rsp.retcode = kRetcodeSceneNotFound;
        session.send(cmd::EnterSceneScRsp, rsp);
        return;
    }
    player->saveNow();

    // The scene arrives as a notify; the response only acknowledges the request.
    proto::EnterSceneByServerScNotify notify;
    notify.scene = info;
    notify.lineup = scene::lineupInfo(*player);
    session.send(cmd::EnterSceneByServerScNotify, notify);

    proto::EnterSceneScRsp rsp;
    rsp.retcode = 0;
    rsp.is_close_map = req.is_close_map;
    rsp.scene_identifier.emplace().floor_id = player->location().floorId;
    session.send(cmd::EnterSceneScRsp, rsp);
}

void onSceneEntityMove(net::Session& session, const proto::SceneEntityMoveCsReq& req) {
    Player* player = playerOf(session, "SceneEntityMove");
    if (player == nullptr) return;

    for (const proto::EntityMotion& motion : req.entity_motion_list) {
        // Entity 0 is the party as a whole; the individual actors follow it.
        if (motion.entity_id != 0 && motion.entity_id > kSquadSlots) continue;
        if (!motion.motion) continue;
        if (motion.motion->pos) {
            player->position().x = motion.motion->pos->x;
            player->position().y = motion.motion->pos->y;
            player->position().z = motion.motion->pos->z;
        }
        if (motion.motion->rot) player->position().rotY = motion.motion->rot->y;
    }
    player->save();
    session.sendEmpty(cmd::SceneEntityMoveScRsp);
}

void onSceneEntityTeleport(net::Session& session, const proto::SceneEntityTeleportCsReq& req) {
    Player* player = playerOf(session, "SceneEntityTeleport");
    if (player == nullptr) return;

    proto::SceneEntityTeleportScRsp rsp;
    rsp.retcode = 0;
    if (req.entity_motion) {
        rsp.entity_motion = *req.entity_motion;
        if (req.entity_motion->motion && req.entity_motion->motion->pos) {
            player->position().x = req.entity_motion->motion->pos->x;
            player->position().y = req.entity_motion->motion->pos->y;
            player->position().z = req.entity_motion->motion->pos->z;
        }
        if (req.entity_motion->motion && req.entity_motion->motion->rot) {
            player->position().rotY = req.entity_motion->motion->rot->y;
        }
        player->saveNow();
    }
    session.send(cmd::SceneEntityTeleportScRsp, rsp);
}

void onGetSceneMapInfo(net::Session& session, const proto::GetSceneMapInfoCsReq& req) {
    Player* player = playerOf(session, "GetSceneMapInfo");
    if (player == nullptr) return;

    proto::GetSceneMapInfoScRsp rsp;
    rsp.retcode = 0;
    for (const proto::SceneIdentifier& id : req.scene_identifiers) {
        rsp.scene_map_info.push_back(scene::mapInfo(*player, id.floor_id));
    }
    session.send(cmd::GetSceneMapInfoScRsp, rsp);
}

void onGetUnlockTeleport(net::Session& session, const proto::GetUnlockTeleportCsReq&) {
    proto::GetUnlockTeleportScRsp rsp;
    rsp.retcode = 0;
    for (const data::ResFloor& floor : data::SceneRes::get().floors()) {
        for (const data::ResGroup& group : floor.groups) {
            for (const data::ResTeleport& teleport : group.teleports) {
                rsp.unlocked_teleport_list.push_back(teleport.id);
            }
        }
    }
    session.send(cmd::GetUnlockTeleportScRsp, rsp);
}

void onGetEnteredScene(net::Session& session, const proto::GetEnteredSceneCsReq&) {
    proto::GetEnteredSceneScRsp rsp;
    rsp.retcode = 0;
    for (const data::ResFloor& floor : data::SceneRes::get().floors()) {
        if (!floor.isEnteredSceneInfo) continue;
        proto::EnteredSceneInfo info;
        info.plane_id = floor.planeId;
        info.floor_id = floor.floorId;
        rsp.entered_scene_info_list.push_back(info);
    }
    session.send(cmd::GetEnteredSceneScRsp, rsp);
}

void onEnterSection(net::Session& session, const proto::EnterSectionCsReq&) {
    proto::EnterSectionScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::EnterSectionScRsp, rsp);
}

void onInteractProp(net::Session& session, const proto::InteractPropCsReq& req) {
    Player* player = playerOf(session, "InteractProp");
    if (player == nullptr) return;

    proto::InteractPropScRsp rsp;
    rsp.retcode = 0;
    rsp.prop_entity_id = req.prop_entity_id;

    SceneEntity* prop = player->sceneState().find(req.prop_entity_id);
    if (prop != nullptr && prop->kind == EntityKind::Prop) {
        // interact_id can be a token-sized value; the config id is in interact_id2
        // when the client sends both.
        uint32_t interactId =
            req.interact_id2 != 0 ? req.interact_id2 : static_cast<uint32_t>(req.interact_id);
        const data::InteractInfo* action = data::Tables::get().interact(interactId);
        // InteractConfig only fires from the state it names.
        if (action != nullptr && (action->anySource || action->srcState == prop->state)) {
            prop->state = action->targetState;
        }
        rsp.prop_state = prop->state;
    }
    session.send(cmd::InteractPropScRsp, rsp);
}

// A Stagnant Shadow in the overworld. The fight itself then comes through
// SceneCastSkill like any other.
void onActiveFarmElement(net::Session& session, const proto::ActiveFarmElementCsReq& req) {
    // Remembered so the fight with it costs and pays like a shadow run, and so winning
    // leaves it standing.
    if (Player* player = session.player()) {
        player->farmElement() = {req.entity_id, req.HECCOBFBJFI, req.world_level};
        logging::debug("scene", "farm element entity {} activated: element {} at world level {}",
                       req.entity_id, req.HECCOBFBJFI, req.world_level);
    }
    proto::ActiveFarmElementScRsp rsp;
    rsp.retcode = 0;
    rsp.entity_id = req.entity_id;
    rsp.world_level = req.world_level;
    session.send(cmd::ActiveFarmElementScRsp, rsp);
}

void onDeactivateFarmElement(net::Session& session,
                             const proto::DeactivateFarmElementCsReq& req) {
    if (Player* player = session.player(); player != nullptr && player->farmElement().entityId == req.entity_id) {
        player->farmElement() = {};
    }
    proto::DeactivateFarmElementScRsp rsp;
    rsp.retcode = 0;
    rsp.entity_id = req.entity_id;
    session.send(cmd::DeactivateFarmElementScRsp, rsp);
}

// Sent right after entering a floor; the prop it names is echoed back.
void onChangePropTimelineInfo(net::Session& session,
                              const proto::ChangePropTimelineInfoCsReq& req) {
    proto::ChangePropTimelineInfoScRsp rsp;
    rsp.retcode = 0;
    rsp.prop_entity_id = req.prop_entity_id;
    session.send(cmd::ChangePropTimelineInfoScRsp, rsp);
}

}  // namespace

void registerSceneHandlers() {
    net::on<proto::GetCurSceneInfoCsReq>(cmd::GetCurSceneInfoCsReq, onGetCurSceneInfo);
    net::on<proto::EnterSceneCsReq>(cmd::EnterSceneCsReq, onEnterScene);
    net::on<proto::SceneEntityMoveCsReq>(cmd::SceneEntityMoveCsReq, onSceneEntityMove);
    net::on<proto::SceneEntityTeleportCsReq>(cmd::SceneEntityTeleportCsReq, onSceneEntityTeleport);
    net::on<proto::GetSceneMapInfoCsReq>(cmd::GetSceneMapInfoCsReq, onGetSceneMapInfo);
    net::on<proto::GetUnlockTeleportCsReq>(cmd::GetUnlockTeleportCsReq, onGetUnlockTeleport);
    net::on<proto::GetEnteredSceneCsReq>(cmd::GetEnteredSceneCsReq, onGetEnteredScene);
    net::on<proto::EnterSectionCsReq>(cmd::EnterSectionCsReq, onEnterSection);
    net::on<proto::InteractPropCsReq>(cmd::InteractPropCsReq, onInteractProp);
    net::on<proto::ActiveFarmElementCsReq>(cmd::ActiveFarmElementCsReq, onActiveFarmElement);
    net::on<proto::DeactivateFarmElementCsReq>(cmd::DeactivateFarmElementCsReq,
                                               onDeactivateFarmElement);
    net::on<proto::ChangePropTimelineInfoCsReq>(cmd::ChangePropTimelineInfoCsReq,
                                                onChangePropTimelineInfo);
}

}  // namespace game
