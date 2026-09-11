#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace game {

constexpr uint32_t kSquadCount = 6;
constexpr uint32_t kSquadSlots = 4;
constexpr uint32_t kBaseMazeMp = 5;

// One party. Members are kept packed at the front: the client's LineupInfo.slot and
// leader_slot are both indices into the occupied prefix, so a hole in the middle
// would renumber the party out from under it.
struct Squad {
    std::array<uint32_t, kSquadSlots> slots{};  // base avatar ids, 0 = empty
    std::string name;
    uint32_t leaderAvatarId = 0;
    bool favourite = false;

    bool contains(uint32_t avatarId) const;
    uint32_t count() const;
    bool full() const { return count() == kSquadSlots; }
    uint32_t slotOf(uint32_t avatarId) const;  // kSquadSlots when absent
    // Shifts members left so the occupied slots are 0..count-1, order preserved.
    void compact();
};

// The six squads plus the shared technique-point pool.
class LineupBook {
public:
    LineupBook();

    uint32_t curIndex() const { return curIndex_; }
    void setCurIndex(uint32_t index);

    Squad& squad(uint32_t index);
    const Squad& squad(uint32_t index) const;
    Squad& cur() { return squad(curIndex_); }
    const Squad& cur() const { return squad(curIndex_); }

    // Occupied slots of a squad, in slot order.
    std::vector<uint32_t> members(uint32_t index) const;
    std::vector<uint32_t> curMembers() const { return members(curIndex_); }

    uint32_t leaderSlot(uint32_t index) const;
    void setLeaderSlot(uint32_t index, uint32_t slot);

    bool join(uint32_t index, uint32_t slot, uint32_t avatarId);
    bool quit(uint32_t index, uint32_t avatarId);
    bool swap(uint32_t index, uint32_t slotA, uint32_t slotB);
    void replace(uint32_t index, const std::vector<uint32_t>& avatarIds, uint32_t leaderSlot);

    uint32_t mp() const { return mp_; }
    void setMp(uint32_t value);
    uint32_t maxMp() const;
    // Called after any squad edit: the cap follows who is on the field.
    void clampMp();

private:
    void settle(Squad& squad);

    std::array<Squad, kSquadCount> squads_;
    uint32_t curIndex_ = 0;
    uint32_t mp_ = kBaseMazeMp;
};

// Avatars whose adventure ability raises the technique-point cap by 3.
bool raisesMazeMpCap(uint32_t avatarId);

}  // namespace game
