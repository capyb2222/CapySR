#pragma once

#include <string>

namespace game {

class Player;

// data/player.json: squads, position, scene, path choices and server-side prefs.
// Everything else the client shows comes from the srtools build or the game tables.
bool loadPlayerState(Player& player);
bool savePlayerState(const Player& player);
// What savePlayerState writes, for a caller that queues the write instead.
std::string playerStateJson(const Player& player);

}  // namespace game
