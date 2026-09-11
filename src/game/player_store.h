#pragma once

namespace game {

class Player;

// data/player.json: squads, position, scene, path choices and server-side prefs.
// Everything else the client shows comes from the srtools build or the game tables.
bool loadPlayerState(Player& player);
bool savePlayerState(const Player& player);

}  // namespace game
