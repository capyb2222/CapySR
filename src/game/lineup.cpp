#include "game/lineup.h"

#include <algorithm>

#include "data/excel.h"

namespace game {
namespace {

// Static because the +3 lives in ConfigAdventureAbility rather than ExcelOutput:
// Phainon and Himeko Nova both declare AdvModifyMaxMazeMP.
constexpr uint32_t kMpCapRaisers[] = {1408, 1510};

}  // namespace

bool raisesMazeMpCap(uint32_t avatarId) {
    uint32_t base = data::Tables::get().baseAvatarId(avatarId);
    for (uint32_t id : kMpCapRaisers) {
        if (id == avatarId || id == base) return true;
    }
    return false;
}

bool Squad::contains(uint32_t avatarId) const {
    return avatarId != 0 && std::find(slots.begin(), slots.end(), avatarId) != slots.end();
}

uint32_t Squad::count() const {
    return static_cast<uint32_t>(
        std::count_if(slots.begin(), slots.end(), [](uint32_t id) { return id != 0; }));
}

uint32_t Squad::slotOf(uint32_t avatarId) const {
    for (uint32_t slot = 0; slot < kSquadSlots; ++slot) {
        if (avatarId != 0 && slots[slot] == avatarId) return slot;
    }
    return kSquadSlots;
}

void Squad::compact() {
    std::array<uint32_t, kSquadSlots> packed{};
    uint32_t next = 0;
    for (uint32_t id : slots) {
        if (id != 0) packed[next++] = id;
    }
    slots = packed;
}

LineupBook::LineupBook() {
    for (uint32_t i = 0; i < kSquadCount; ++i) {
        squads_[i].name = "Squad " + std::to_string(i + 1);
    }
}

void LineupBook::setCurIndex(uint32_t index) {
    if (index < kSquadCount) curIndex_ = index;
    clampMp();
}

Squad& LineupBook::squad(uint32_t index) { return squads_[index < kSquadCount ? index : 0]; }

const Squad& LineupBook::squad(uint32_t index) const {
    return squads_[index < kSquadCount ? index : 0];
}

std::vector<uint32_t> LineupBook::members(uint32_t index) const {
    std::vector<uint32_t> out;
    for (uint32_t id : squad(index).slots) {
        if (id != 0) out.push_back(id);
    }
    return out;
}

uint32_t LineupBook::leaderSlot(uint32_t index) const {
    uint32_t slot = squad(index).slotOf(squad(index).leaderAvatarId);
    return slot < kSquadSlots ? slot : 0;
}

void LineupBook::setLeaderSlot(uint32_t index, uint32_t slot) {
    Squad& s = squad(index);
    if (slot < kSquadSlots && s.slots[slot] != 0) s.leaderAvatarId = s.slots[slot];
}

// Keeps the members packed and the leader pointing at someone who is still present.
void LineupBook::settle(Squad& squad) {
    squad.compact();
    if (!squad.contains(squad.leaderAvatarId)) squad.leaderAvatarId = squad.slots[0];
    clampMp();
}

bool LineupBook::join(uint32_t index, uint32_t slot, uint32_t avatarId) {
    if (avatarId == 0 || index >= kSquadCount) return false;
    Squad& s = squad(index);

    // Rebuild the order with the avatar inserted once, so a join that moves an
    // existing member cannot duplicate or drop anyone.
    std::vector<uint32_t> order;
    for (uint32_t id : s.slots) {
        if (id != 0 && id != avatarId) order.push_back(id);
    }
    if (order.size() >= kSquadSlots) return false;
    // Members stay packed, so a slot past the end lands at the end.
    size_t target = std::min<size_t>(slot, order.size());
    order.insert(order.begin() + static_cast<long>(target), avatarId);

    s.slots.fill(0);
    for (size_t i = 0; i < order.size(); ++i) s.slots[i] = order[i];
    settle(s);
    return true;
}

bool LineupBook::quit(uint32_t index, uint32_t avatarId) {
    if (index >= kSquadCount) return false;
    Squad& s = squad(index);
    uint32_t slot = s.slotOf(avatarId);
    if (slot >= kSquadSlots) return false;
    // The client refuses an empty party, so the last member cannot leave.
    if (s.count() <= 1) return false;
    s.slots[slot] = 0;
    settle(s);
    return true;
}

bool LineupBook::swap(uint32_t index, uint32_t slotA, uint32_t slotB) {
    if (index >= kSquadCount || slotA >= kSquadSlots || slotB >= kSquadSlots) return false;
    Squad& s = squad(index);
    if (s.slots[slotA] == 0 || s.slots[slotB] == 0) return false;
    std::swap(s.slots[slotA], s.slots[slotB]);
    return true;
}

void LineupBook::replace(uint32_t index, const std::vector<uint32_t>& avatarIds,
                         uint32_t leaderSlot) {
    if (index >= kSquadCount) return;
    Squad& s = squad(index);
    s.slots.fill(0);
    uint32_t next = 0;
    for (uint32_t id : avatarIds) {
        if (id == 0 || next >= kSquadSlots) continue;
        if (s.contains(id)) continue;
        s.slots[next++] = id;
    }
    if (leaderSlot < kSquadSlots && s.slots[leaderSlot] != 0) {
        s.leaderAvatarId = s.slots[leaderSlot];
    }
    settle(s);
}

uint32_t LineupBook::maxMp() const {
    uint32_t extra = 0;
    for (uint32_t id : cur().slots) {
        if (id != 0 && raisesMazeMpCap(id)) extra += 3;
    }
    return kBaseMazeMp + extra;
}

void LineupBook::setMp(uint32_t value) { mp_ = std::min(value, maxMp()); }

void LineupBook::clampMp() { mp_ = std::min(mp_, maxMp()); }

}  // namespace game
