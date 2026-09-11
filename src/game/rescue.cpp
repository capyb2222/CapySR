#include "game/rescue.h"

#include "core/logger.h"
#include "game/challenge.h"
#include "game/player.h"
#include "game/scene.h"
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
    if (player->challenge().active) {
        player->challenge() = ChallengeRun{};
        note("abandoned the challenge");
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

}  // namespace rescue
}  // namespace game
