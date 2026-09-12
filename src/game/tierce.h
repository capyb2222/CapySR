#pragma once

#include <cstdint>
#include <vector>

#include "game/scene.h"
#include "proto/gen/protos.h"

namespace net {
class Session;
}

namespace game {

class Player;
struct BattleRequest;

// The three-node floor 4.6 replaced the two-node one with. A ChallengeMazeTierce row
// bolts a third node onto a Memory of Chaos, Pure Fiction or Apocalyptic Shadow floor,
// and from then on the client starts that whole floor through here rather than through
// StartChallenge -- so a floor with a tierce row never sees the older packet at all.
//
// The first two nodes are the ones the floor already had; the third brings its own
// arena and monster. Each node fights with its own team and its own picked buff.
namespace tierce {

// Every floor that has a third node, with whatever the player has done on it.
proto::GetChallengeTierceDataScRsp history(const Player& player);
// The run in progress, as the client models it. Meaningless when none is.
proto::ChallengeTierceChallengeInfo current(const Player& player);
// The team fighting node `stage` (0..2), as an extra lineup rather than a squad.
proto::LineupInfo lineup(const Player& player, uint32_t stage);

// Remembers what the team editor set without starting anything, so the per-node UI
// keeps showing it.
void setLineups(Player& player, uint32_t tierceId,
                const std::vector<proto::ChallengeTierceStageLineupInfo>& stages);

// Starts a floor, entering the arena for its first node -- or for `stageIndex` alone
// when the player is replaying one. Returns the retcode; on success `out` holds the
// arena.
uint32_t start(Player& player, uint32_t tierceId, bool single, uint32_t stageIndex,
               const std::vector<proto::ChallengeTierceStageLineupInfo>& stages,
               proto::SceneInfo& out);

// Walks on to the next node; false when the run is on its last.
bool nextStage(net::Session& session, Player& player, proto::SceneInfo& out);
// Re-enters the node in progress, with its counters rolled back.
bool restart(net::Session& session, Player& player, proto::SceneInfo& out);

// Same as challenge::arena, for the node in progress.
const ChallengeArena* arena(const Player& player, ChallengeArena& storage);

// What the node brings to a fight: its team, the floor's maze buff, the buff the player
// picked for it, the cycles left and the mode's win condition.
void prepareBattle(const Player& player, BattleRequest& request);

// A fight inside a run has ended. False when no run is going.
bool battleFinished(net::Session& session, Player& player,
                    const proto::PVEBattleResultCsReq& req);

// Ends the run and puts the player back where they started.
void leave(net::Session& session, Player& player);

}  // namespace tierce
}  // namespace game
