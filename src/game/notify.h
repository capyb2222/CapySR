#pragma once

#include <cstdint>
#include <vector>

namespace net {
class Session;
}

namespace game {

class Player;

namespace notify {

// The party bar only redraws from SyncLineupNotify, and the client stops listening
// once the request it sent has been answered -- so every lineup edit pushes the
// scene refresh and the lineup notify *before* its own ScRsp.
void lineupChanged(net::Session& session, Player& player);

// Pushes one avatar plus its gear back to the client after an equip change.
void avatarChanged(net::Session& session, Player& player, uint32_t baseAvatarId,
                   const std::vector<uint32_t>& touchedEquipmentIds,
                   const std::vector<uint32_t>& touchedRelicIds);

// Removes beaten monsters from the scene so they do not respawn under the player.
void monstersRemoved(net::Session& session, Player& player,
                     const std::vector<uint32_t>& entityIds);

}  // namespace notify
}  // namespace game
