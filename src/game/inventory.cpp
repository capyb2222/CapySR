#include "game/inventory.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include "game/player.h"

namespace game::inventory {
namespace {

constexpr uint32_t kNoLimit = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kMaxWorldLevel = 6;

bool inWallet(uint32_t id) {
    return id == kStellarJade || id == kCredit || id == kOneiricShard || id == kStamina ||
           id == kReserveStamina || id == kTrailblazeExp;
}

void add(uint32_t& field, uint32_t amount, uint32_t limit) {
    field = static_cast<uint32_t>(std::min<uint64_t>(uint64_t{field} + amount, limit));
}

uint32_t randomBetween(uint32_t low, uint32_t high, const std::function<double()>& roll) {
    if (high <= low) return low;
    auto offset = static_cast<uint32_t>(roll() * static_cast<double>(high - low + 1));
    return low + std::min(offset, high - low);
}

// How many of a material with no listed count drop, by ItemConfig.PurposeType.
uint32_t dropAmount(const data::ItemInfo& item, uint32_t level) {
    switch (item.purposeType) {
        case 1:  // character EXP
            if (item.rarity == 2) return level < 3 ? level + 3 : 2;
            if (item.rarity == 3) return level < 3 ? level + 3 : level * 2 - 3;
            return 1;
        case 2:  // Stagnant Shadow materials
            return level;
        case 3:  // trace materials
            return 5;
        case 4:  // Echo of War materials
            return (level + 1) / 2;
        case 5:  // light cone EXP
            if (item.rarity == 2) return std::max<uint32_t>(level < 5 ? 5 - level : 0, 2);
            if (item.rarity == 3) return level % 3 + 1;
            return 1;
        case 11:
            return 4 + level;
        default:
            return 0;
    }
}

}  // namespace

void refreshStamina(Player& player, int64_t now) {
    Inventory& bag = player.inventory();
    const data::StaminaRules& rules = data::Tables::get().staminaRules();
    if (bag.staminaUpdatedAt == 0 || now < bag.staminaUpdatedAt) {
        bag.staminaUpdatedAt = now;
        return;
    }

    if (bag.stamina < rules.max && rules.recoverSeconds != 0) {
        int64_t gained = (now - bag.staminaUpdatedAt) / rules.recoverSeconds;
        int64_t missing = rules.max - bag.stamina;
        if (gained < missing) {
            bag.stamina += static_cast<uint32_t>(gained);
            bag.staminaUpdatedAt += gained * rules.recoverSeconds;
            return;
        }
        bag.stamina = rules.max;
        bag.staminaUpdatedAt += missing * rules.recoverSeconds;
    }

    // Full, so the time runs into reserve stamina instead.
    if (rules.reserveRecoverSeconds == 0 || bag.reserveStamina >= rules.reserveMax) {
        bag.staminaUpdatedAt = now;
        return;
    }
    int64_t reserve = (now - bag.staminaUpdatedAt) / rules.reserveRecoverSeconds;
    bag.reserveStamina = static_cast<uint32_t>(
        std::min<int64_t>(rules.reserveMax, int64_t{bag.reserveStamina} + reserve));
    bag.staminaUpdatedAt += reserve * rules.reserveRecoverSeconds;
    if (bag.reserveStamina >= rules.reserveMax) bag.staminaUpdatedAt = now;
}

int64_t nextRecoverTime(const Player& player) {
    const Inventory& bag = player.inventory();
    const data::StaminaRules& rules = data::Tables::get().staminaRules();
    return bag.stamina < rules.max ? bag.staminaUpdatedAt + rules.recoverSeconds : 0;
}

void chargeStamina(Player& player, uint32_t cost, int64_t now) {
    refreshStamina(player, now);
    Inventory& bag = player.inventory();
    bool wasFull = bag.stamina >= data::Tables::get().staminaRules().max;
    bag.stamina -= std::min(cost, bag.stamina);
    if (wasFull && bag.stamina < data::Tables::get().staminaRules().max) bag.staminaUpdatedAt = now;
}

void grant(Player& player, const std::vector<data::ItemStack>& items) {
    Inventory& bag = player.inventory();
    const data::Tables& tables = data::Tables::get();
    for (const data::ItemStack& stack : items) {
        if (stack.num == 0) continue;
        switch (stack.id) {
            case kStellarJade: add(bag.hcoin, stack.num, kNoLimit); break;
            case kCredit: add(bag.scoin, stack.num, kNoLimit); break;
            case kOneiricShard: add(bag.mcoin, stack.num, kNoLimit); break;
            case kStamina: add(bag.stamina, stack.num, kNoLimit); break;
            case kReserveStamina:
                add(bag.reserveStamina, stack.num, tables.staminaRules().reserveMax);
                break;
            case kTrailblazeExp: break;
            default: {
                const data::ItemInfo* info = tables.item(stack.id);
                uint32_t limit = info != nullptr && info->pileLimit != 0 ? info->pileLimit : kNoLimit;
                add(bag.items[stack.id], stack.num, limit);
            }
        }
    }
}

uint64_t held(const Player& player, uint32_t id) {
    const Inventory& bag = player.inventory();
    switch (id) {
        case kStellarJade: return bag.hcoin;
        case kCredit: return bag.scoin;
        case kOneiricShard: return bag.mcoin;
        case kStamina: return bag.stamina;
        case kReserveStamina: return bag.reserveStamina;
        default: {
            auto it = bag.items.find(id);
            return it == bag.items.end() ? 0 : it->second;
        }
    }
}

bool spend(Player& player, const std::vector<data::ItemStack>& items) {
    std::vector<data::ItemStack> total = merged(items);
    for (const data::ItemStack& stack : total) {
        if (held(player, stack.id) < stack.num) return false;
    }
    Inventory& bag = player.inventory();
    for (const data::ItemStack& stack : total) {
        switch (stack.id) {
            case kStellarJade: bag.hcoin -= stack.num; break;
            case kCredit: bag.scoin -= stack.num; break;
            case kOneiricShard: bag.mcoin -= stack.num; break;
            case kStamina: bag.stamina -= stack.num; break;
            case kReserveStamina: bag.reserveStamina -= stack.num; break;
            default:
                if ((bag.items[stack.id] -= stack.num) == 0) bag.items.erase(stack.id);
        }
    }
    return true;
}

const data::MappingInfo* dropTable(uint32_t mappingInfoId, uint32_t worldLevel) {
    if (mappingInfoId == 0) return nullptr;
    const data::Tables& tables = data::Tables::get();
    int64_t wanted = std::min(worldLevel, kMaxWorldLevel);
    // Downwards first: a lower level never pays more than the player earned.
    for (int64_t distance = 0; distance <= kMaxWorldLevel; ++distance) {
        for (int64_t level : {wanted - distance, wanted + distance}) {
            if (level < 0 || level > kMaxWorldLevel || (distance == 0 && level != wanted)) continue;
            const data::MappingInfo* table = tables.mappingInfo(mappingInfoId, static_cast<uint32_t>(level));
            if (table != nullptr && !table->display.empty()) return table;
        }
    }
    return nullptr;
}

std::vector<data::ItemStack> rollDrops(const data::MappingInfo& table,
                                       const std::function<double()>& roll) {
    const data::Tables& tables = data::Tables::get();
    uint32_t level = table.worldLevel;
    std::vector<data::ItemStack> drops;
    for (const data::ItemStack& shown : table.display) {
        uint32_t amount = shown.num;
        if (amount == 0 && shown.id == kCredit) {
            amount = randomBetween((50 + level * 10) * table.farmType,
                                   (100 + level * 10) * table.farmType, roll);
        } else if (amount == 0) {
            const data::ItemInfo* item = tables.item(shown.id);
            if (item != nullptr && item->mainType == "Material") amount = dropAmount(*item, level);
        }
        if (amount != 0) drops.push_back({shown.id, amount});
    }
    return drops;
}

std::vector<data::ItemStack> rewardItems(const data::RewardInfo& reward) {
    std::vector<data::ItemStack> items;
    if (reward.hcoin != 0) items.push_back({kStellarJade, reward.hcoin});
    items.insert(items.end(), reward.items.begin(), reward.items.end());
    return items;
}

std::vector<data::ItemStack> merged(const std::vector<data::ItemStack>& items) {
    std::vector<data::ItemStack> out;
    for (const data::ItemStack& stack : items) {
        auto it = std::find_if(out.begin(), out.end(),
                               [&](const data::ItemStack& s) { return s.id == stack.id; });
        if (it == out.end()) {
            out.push_back(stack);
        } else {
            add(it->num, stack.num, kNoLimit);
        }
    }
    return out;
}

proto::ItemList itemList(const std::vector<data::ItemStack>& items) {
    proto::ItemList list;
    for (const data::ItemStack& stack : items) {
        proto::Item item;
        item.item_id = stack.id;
        item.num = stack.num;
        list.item_list.push_back(item);
    }
    return list;
}

proto::PlayerBasicInfo basicInfo(const Player& player) {
    proto::PlayerBasicInfo info;
    info.nickname = player.name();
    info.level = player.level();
    info.world_level = player.worldLevel();
    info.stamina = player.stamina();
    info.hcoin = player.hcoin();
    info.scoin = player.scoin();
    info.mcoin = player.mcoin();
    return info;
}

proto::PlayerSyncScNotify sync(const Player& player, const std::vector<data::ItemStack>& changed) {
    proto::PlayerSyncScNotify notify;
    notify.basic_info.emplace() = basicInfo(player);
    const Inventory& bag = player.inventory();
    std::set<uint32_t> seen;
    for (const data::ItemStack& stack : changed) {
        if (inWallet(stack.id) || !seen.insert(stack.id).second) continue;
        auto it = bag.items.find(stack.id);
        proto::Material material;
        material.tid = stack.id;
        material.num = it == bag.items.end() ? 0 : it->second;
        notify.material_list.push_back(material);
    }
    return notify;
}

proto::StaminaInfoScNotify staminaInfo(const Player& player) {
    proto::StaminaInfoScNotify notify;
    notify.stamina = player.stamina();
    notify.next_recover_time = nextRecoverTime(player);
    notify.reserve_stamina = player.inventory().reserveStamina;
    return notify;
}

}  // namespace game::inventory
