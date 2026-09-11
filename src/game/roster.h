#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "game/srtools.h"
#include "proto/gen/protos.h"

namespace game {

// srtools numbers its items from 0 and unique id 0 means "nothing", so ids shift up
// by one; lightcones sit above the relic range so the two ranges never collide.
constexpr uint32_t kEquipmentUidBase = 3000;

// Avatar hp is a fraction in ten-thousandths, so this is an untouched character.
constexpr uint32_t kFullHp = 10000;

uint32_t relicUniqueId(uint32_t internalUid);
uint32_t equipmentUniqueId(uint32_t internalUid);
uint32_t relicInternalUid(uint32_t uniqueId);
uint32_t equipmentInternalUid(uint32_t uniqueId);

// One player's avatars and gear, seen through their chosen Trailblazer/March paths.
class Roster {
public:
    Roster(std::shared_ptr<const SrToolsData> data, uint32_t mainCharacter, uint32_t marchType);

    const SrToolsData& data() const { return *data_; }

    // Base ids the client wants a row for: every avatar in the tables, with the
    // multi-path variants folded into their base.
    static const std::vector<uint32_t>& baseAvatarIds();

    // 8001 -> the chosen path, 1001 -> the chosen March form, anything else unchanged.
    uint32_t resolvePath(uint32_t baseId) const;

    const Avatar* find(uint32_t avatarId) const;
    const Lightcone* lightconeOf(uint32_t avatarId) const;
    std::vector<const Relic*> relicsOf(uint32_t avatarId) const;

    proto::Avatar toAvatar(uint32_t baseId) const;
    proto::AvatarPathData toPathData(const Avatar& avatar) const;
    proto::LineupAvatar toLineupAvatar(uint32_t baseId, uint32_t slot) const;
    // Also appends the avatar's technique buffs to `buffs`.
    proto::BattleAvatar toBattleAvatar(uint32_t baseId, uint32_t index,
                                       std::vector<proto::BattleBuff>& buffs) const;

    proto::Equipment toEquipment(const Lightcone& lightcone) const;
    proto::Relic toRelic(const Relic& relic) const;

    uint32_t mainCharacter() const { return mainCharacter_; }
    uint32_t marchType() const { return marchType_; }

private:
    std::shared_ptr<const SrToolsData> data_;
    uint32_t mainCharacter_;
    uint32_t marchType_;
};

}  // namespace game
