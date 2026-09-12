#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "proto/gen/protos.h"

namespace game {

class Player;

// Everything the server knows about a fight the client is asking to start.
struct BattleRequest {
    uint32_t casterEntityId = 0;      // party actor that swung, 0 for calyx/quick start
    uint32_t attackedByEntityId = 0;  // monster that ambushed, 0 when the player struck
    uint32_t skillIndex = 0;          // 0 = basic attack, 1 = technique
    std::vector<uint32_t> monsterEntityIds;
    // Explicitly named stages, in order; a calyx run past the first adds one each.
    std::vector<uint32_t> stageIds;
    uint32_t cocoonId = 0;
    uint32_t wave = 0;
    // Only a calyx hands its fight over to a srtools build; everything else keeps the
    // stage it named. See gameplay.battle_source.
    bool allowSrToolsOverride = false;

    // ---- challenges ----------------------------------------------------------
    // The team fighting this node, when it is not the squad the player walks around
    // with: MoC and its siblings send both of their teams with StartChallenge.
    std::vector<uint32_t> party;
    // The floor's own maze buff, and the one the player picked for this half.
    uint32_t mazeBuffId = 0;
    uint32_t stageBuffId = 0;
    // Anomaly Arbitration's enemy tags and the boss buff the player picked.
    std::vector<uint32_t> floorBuffIds;
    // MoC's cycle limit, counted down across the whole floor.
    uint32_t roundsLimit = 0;
    // "PF", "AS" or "AA": modes that need a win condition of their own, or the client
    // hangs on the last turn with nothing to settle.
    std::string battleType;
    // Score carried in from the first half, and Pure Fiction's per-wave targets.
    uint32_t scoreSoFar = 0;
    std::vector<uint32_t> battleTargetIds;
};

namespace battle {

// Assembles the SceneBattleInfo the client simulates, and records the fight on the
// player so PVEBattleResult can be answered.
proto::SceneBattleInfo create(Player& player, const BattleRequest& request);

}  // namespace battle
}  // namespace game
