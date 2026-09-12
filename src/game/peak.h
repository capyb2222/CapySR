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

// Anomaly Arbitration: three knights and a boss a season, each one fight with its own
// team. Hard mode is a per-season toggle set before the boss is started; it swaps in
// the boss's hard stage, tags and target.
namespace peak {

// Every season, cleared when gameplay.unlock_all_challenges is set.
proto::GetChallengePeakDataScRsp overview(const Player& player);
// One season as the overview draws it.
proto::ChallengePeakGroup group(const Player& player, uint32_t groupId);
// Re-sends one season, after anything that changes what the overview shows.
void pushGroup(net::Session& session, const Player& player, uint32_t groupId);

// Starts a fight and enters its arena. Returns the retcode; on success `out` holds the
// arena. An empty team or a zero buff reuses the last ones picked for that fight.
uint32_t start(Player& player, uint32_t peakId, uint32_t buffId, std::vector<uint32_t> team,
               proto::SceneInfo& out);

// The run's team, as the extra lineup the arena is fought with.
proto::LineupInfo lineup(const Player& player);

// Same as challenge::arena, for a fight in progress.
const ChallengeArena* arena(const Player& player, ChallengeArena& storage);

// What a fight inside the run brings: its team, the enemy tags, the boss buff, targets.
void prepareBattle(const Player& player, BattleRequest& request);

// The fight has ended. Returns false when no run is going.
bool battleFinished(net::Session& session, Player& player,
                    const proto::PVEBattleResultCsReq& req);

// Ends the run and puts the player back where they started.
void leave(net::Session& session, Player& player);

}  // namespace peak
}  // namespace game
