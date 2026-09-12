#include "game/rescue.h"

#include "core/config.h"
#include "core/logger.h"
#include "data/scene_res.h"
#include "game/challenge.h"
#include "game/peak.h"
#include "game/player.h"
#include "game/scene.h"
#include "game/tierce.h"
#include "net/cmd_ids.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace rescue {

std::string unstick(net::Session& session, uint32_t entryId) {
    Player* player = session.player();
    if (player == nullptr) return "not logged in";

    std::string did;
    auto note = [&did](const std::string& what) {
        if (!did.empty()) did += ", ";
        did += what;
    };

    // A fight the client is still showing but the server has forgotten is the most
    // common way to end up looking at nothing.
    if (player->battle().active) {
        player->battle().active = false;
        player->battle().monsterEntityIds.clear();
        note("ended the fight");
    }
    session.sendEmpty(cmd::QuitBattleScNotify);

    // A challenge already knows how to put the player back where they came from.
    if (player->challenge().active && entryId == 0) {
        challenge::leave(session, *player);
        note("left the challenge");
        logging::info("rescue", "uid {}: {}", player->uid(), did);
        return did;
    }
    if (player->peak().active && entryId == 0) {
        peak::leave(session, *player);
        note("left the arbitration");
        logging::info("rescue", "uid {}: {}", player->uid(), did);
        return did;
    }
    if (player->tierce().active && entryId == 0) {
        tierce::leave(session, *player);
        note("left the challenge floor");
        logging::info("rescue", "uid {}: {}", player->uid(), did);
        return did;
    }
    if (player->challenge().active) {
        player->challenge() = ChallengeRun{};
        note("abandoned the challenge");
    }
    if (player->peak().active) {
        player->peak() = PeakRun{};
        note("abandoned the arbitration");
    }
    if (player->tierce().active) {
        player->tierce() = TierceRun{};
        note("abandoned the challenge floor");
    }

    uint32_t target = entryId != 0 ? entryId : player->location().entryId;
    proto::SceneInfo scene;
    if (!scene::load(*player, target, 0, true, scene)) {
        return "entry " + std::to_string(target) + " is not in the scene dump";
    }
    player->saveNow();

    proto::EnterSceneByServerScNotify notify;
    notify.scene = std::move(scene);
    notify.lineup = scene::lineupInfo(*player);
    session.send(cmd::EnterSceneByServerScNotify, notify);

    proto::SyncLineupNotify sync;
    sync.lineup = scene::lineupInfo(*player);
    session.send(cmd::SyncLineupNotify, sync);

    note("pushed entry " + std::to_string(target));
    logging::info("rescue", "uid {}: {}", player->uid(), did);
    return did;
}

std::string relocate(uint32_t entryId) {
    if (data::SceneRes::get().byEntry(entryId) == nullptr) {
        return "entry " + std::to_string(entryId) + " is not in the scene dump";
    }

    Player player(core::Config::get().player.uid);
    player.load();
    // scene::load only re-anchors when the floor changes, and the saved spot may already
    // be on this one. Clearing the floor forces the entrance anchor -- keeping old
    // coordinates, or worse (0, 0, 0), drops the player outside the geometry.
    player.location().floorId = 0;
    proto::SceneInfo scene;
    if (!scene::load(player, entryId, 0, true, scene)) {
        return "entry " + std::to_string(entryId) + " could not be loaded";
    }
    player.saveNow();

    logging::info("rescue", "no client connected; the save now starts at entry {}", entryId);
    return "saved spot moved to entry " + std::to_string(entryId);
}

}  // namespace rescue
}  // namespace game
