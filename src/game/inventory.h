#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "data/excel.h"
#include "proto/gen/protos.h"

namespace game {

class Player;

namespace inventory {

// Ids the wallet and stamina use instead of the bag.
constexpr uint32_t kStellarJade = 1;
constexpr uint32_t kCredit = 2;
constexpr uint32_t kOneiricShard = 3;
constexpr uint32_t kStamina = 11;
constexpr uint32_t kReserveStamina = 12;
constexpr uint32_t kTrailblazeExp = 22;

// Stamina and reserve stamina brought up to `now` (unix seconds) at the table's rates.
void refreshStamina(Player& player, int64_t now);
// When the next point comes in; 0 while stamina is full.
int64_t nextRecoverTime(const Player& player);
// Takes up to `cost`. Dropping below full starts the recovery clock.
void chargeStamina(Player& player, uint32_t cost, int64_t now);

// Puts items where they go: the wallet, stamina, or the bag up to each item's pile limit.
// Trailblaze EXP has nowhere to go while the level is fixed.
void grant(Player& player, const std::vector<data::ItemStack>& items);

// A farming spot's drop table at `worldLevel`, or at the nearest world level that lists
// any drops: the tables leave some levels empty.
const data::MappingInfo* dropTable(uint32_t mappingInfoId, uint32_t worldLevel);
// One run's drops. A listed count drops as is; the rest are sized by the item's kind and
// the table's world level.
std::vector<data::ItemStack> rollDrops(const data::MappingInfo& table,
                                       const std::function<double()>& roll);
// RewardData as items, its Stellar Jade included.
std::vector<data::ItemStack> rewardItems(const data::RewardInfo& reward);
// Repeats of one item summed, in first-seen order.
std::vector<data::ItemStack> merged(const std::vector<data::ItemStack>& items);

proto::ItemList itemList(const std::vector<data::ItemStack>& items);
proto::PlayerBasicInfo basicInfo(const Player& player);
// The bag entries for `changed`, with the wallet and stamina alongside.
proto::PlayerSyncScNotify sync(const Player& player, const std::vector<data::ItemStack>& changed);
proto::StaminaInfoScNotify staminaInfo(const Player& player);

}  // namespace inventory
}  // namespace game
