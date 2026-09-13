#pragma once

#include <cstdint>
#include <vector>

#include "data/excel.h"
#include "game/scene.h"
#include "proto/gen/protos.h"

namespace net {
class Session;
}

namespace game {

class Player;

// Memory of Chaos, Pure Fiction and Apocalyptic Shadow. All three run the same way --
// one or two nodes, each with its own team, fought in an arena that is an ordinary floor
// with everything but the maze config's own monsters switched off.
namespace challenge {

// The player's challenge history: every floor of every mode, with its stars and its
// season's rewards. Reports a full clear when gameplay.unlock_all_challenges is set.
// Without a player every reward reads as claimed.
proto::GetChallengeScRsp history(const Player* player = nullptr);

// Pays out every star reward of the season that is earned and not yet claimed, returning
// what was granted in `granted` so the caller can sync it.
proto::TakeChallengeRewardScRsp takeRewards(Player& player, uint32_t groupId,
                                            std::vector<data::ItemStack>& granted);

// The season panel: the best cleared floor of `groupId`, with the teams that cleared it.
proto::GetChallengeGroupStatisticsScRsp statistics(const Player& player, uint32_t groupId);

// The run in progress, as the client models it. Meaningless when none is.
proto::CurChallenge current(const Player& player);
// The team fighting node `stage` (1 or 2), as an extra lineup rather than a squad.
proto::LineupInfo lineup(const Player& player, uint32_t stage);

// Fills `storage` and returns it when a run is going, so the scene is rebuilt with the
// same monsters it was entered with; nullptr otherwise.
const ChallengeArena* arena(const Player& player, ChallengeArena& storage);

// Enters the arena for the run's current node. False when its map entrance is not in
// the scene dump, in which case the run is left untouched.
bool enterArena(Player& player, proto::SceneInfo& out);

// Which of the floor's star targets the run has met, as the bitmask the client wants.
uint32_t stars(const Player& player);

// A fight inside a run has ended. Returns false when no run is going, so the ordinary
// overworld handling applies.
bool battleFinished(net::Session& session, Player& player,
                    const proto::PVEBattleResultCsReq& req);

// Moves to the second node, for the one mode whose client asks for it explicitly.
bool nextPhase(net::Session& session, Player& player, proto::SceneInfo& out);

// Ends the run and puts the player back where they started.
void leave(net::Session& session, Player& player);

// Shared with Anomaly Arbitration, which fights with the same kind of team and leaves
// the same way.
proto::LineupInfo extraLineup(const Player& player, const std::vector<uint32_t>& party,
                              proto::ExtraLineupType type);
void putBack(net::Session& session, Player& player, uint32_t entryId, const Position& at);

}  // namespace challenge
}  // namespace game
