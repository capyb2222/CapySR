#include "game/gacha.h"

#include <algorithm>

#include "core/util.h"

namespace game {
namespace {

// Stellar Warp's 5* light cones; no table lists them.
constexpr uint32_t kStandardLightcones[] = {23000, 23002, 23003, 23004, 23005, 23012, 23013};

constexpr uint32_t kUndyingEmbers = 251;
constexpr uint32_t kUndyingStarlight = 252;
constexpr uint32_t kEidolonItemOffset = 10000;
constexpr uint32_t kFourStarPity = 10;

struct Rates {
    uint32_t hardPity;
    uint32_t softPity;
    double base;
    double step;
    double fourStar;
};

// 0.6%, +6% a pull from 74, certain at 90. Light cones: 0.8%, +7% from 66, certain at 80.
constexpr Rates kCharacterRates{90, 74, 0.006, 0.06, 0.051};
constexpr Rates kLightconeRates{80, 66, 0.008, 0.07, 0.066};

double fiveStarChance(const Rates& rates, uint32_t pullNumber) {
    if (pullNumber >= rates.hardPity) return 1.0;
    if (pullNumber < rates.softPity) return rates.base;
    return std::min(1.0, rates.base + rates.step * (pullNumber - rates.softPity + 1));
}

uint32_t pick(const std::vector<uint32_t>& ids, const GachaRoll& roll) {
    size_t index = static_cast<size_t>(roll() * static_cast<double>(ids.size()));
    return ids[std::min(index, ids.size() - 1)];
}

GachaPull pullOne(const data::GachaPool& pool, const GachaItemPools& items, GachaPity& pity,
                  const GachaRoll& roll) {
    const Rates& rates = pool.type == data::GachaType::WeaponUp ? kLightconeRates : kCharacterRates;
    ++pity.total;
    GachaPull pull;

    if (roll() < fiveStarChance(rates, pity.sinceFive + 1)) {
        pull.rarity = 5;
        pity.sinceFive = 0;
        // A 5* does not use up the 4* guarantee; it lands on the next pull instead.
        ++pity.sinceFour;
        if (pool.type == data::GachaType::Normal) {
            pull.avatar = roll() < 0.5;
            pull.itemId = pick(pull.avatar ? items.fiveStarAvatars : items.fiveStarLightcones, roll);
            return pull;
        }
        pull.avatar = pool.type == data::GachaType::AvatarUp;
        std::vector<uint32_t> others;
        for (uint32_t id : pull.avatar ? items.fiveStarAvatars : items.fiveStarLightcones) {
            if (id != pool.featured) others.push_back(id);
        }
        pull.guaranteed = pity.guaranteed;
        pull.featured = pity.guaranteed || others.empty() || roll() * 100.0 < pool.upChance;
        pity.guaranteed = !pull.featured;
        pull.itemId = pull.featured ? pool.featured : pick(others, roll);
        return pull;
    }

    ++pity.sinceFive;
    if (pity.sinceFour + 1 >= kFourStarPity || roll() < rates.fourStar) {
        pull.rarity = 4;
        pity.sinceFour = 0;
        pull.avatar = roll() < 0.5;
        pull.itemId = pick(pull.avatar ? items.fourStarAvatars : items.fourStarLightcones, roll);
        return pull;
    }

    ++pity.sinceFour;
    pull.itemId = pick(items.threeStarLightcones, roll);
    return pull;
}

}  // namespace

GachaPity& GachaProgress::forType(data::GachaType type) {
    switch (type) {
        case data::GachaType::AvatarUp:
            return character;
        case data::GachaType::WeaponUp:
            return lightcone;
        case data::GachaType::Normal:
            break;
    }
    return standard;
}

bool GachaItemPools::complete() const {
    return !fiveStarAvatars.empty() && !fiveStarLightcones.empty() && !fourStarAvatars.empty() &&
           !fourStarLightcones.empty() && !threeStarLightcones.empty();
}

GachaItemPools gachaItemPools(const data::Tables& tables) {
    GachaItemPools items;
    items.fiveStarAvatars = tables.standardGacha().ceilingAvatars;
    for (uint32_t id : kStandardLightcones) {
        const data::LightconeInfo* info = tables.lightcone(id);
        if (info != nullptr && info->rarity == 5) items.fiveStarLightcones.push_back(id);
    }
    for (const auto& [id, avatar] : tables.avatars()) {
        // Playable ids, with March's second form and the Trailblazer's paths folded away.
        if (avatar.rarity != 4 || id < 1000 || id >= 2000 || tables.baseAvatarId(id) != id) continue;
        items.fourStarAvatars.push_back(id);
    }
    for (const auto& [id, lightcone] : tables.lightcones()) {
        // 22xxx are event rewards.
        if (id >= 22000 || tables.battlePassReward(id)) continue;
        if (lightcone.rarity == 4) items.fourStarLightcones.push_back(id);
        if (lightcone.rarity == 3) items.threeStarLightcones.push_back(id);
    }
    std::sort(items.fourStarAvatars.begin(), items.fourStarAvatars.end());
    std::sort(items.fourStarLightcones.begin(), items.fourStarLightcones.end());
    std::sort(items.threeStarLightcones.begin(), items.threeStarLightcones.end());
    return items;
}

double gachaRoll() { return static_cast<double>(util::randomU64() >> 11) * 0x1.0p-53; }

namespace {

bool featurable(const data::Tables& tables, data::GachaType type, uint32_t id) {
    if (type == data::GachaType::AvatarUp) {
        const data::AvatarInfo* avatar = tables.avatar(id);
        return avatar != nullptr && avatar->rarity == 5;
    }
    if (type == data::GachaType::WeaponUp) {
        const data::LightconeInfo* lightcone = tables.lightcone(id);
        return lightcone != nullptr && lightcone->rarity == 5;
    }
    return false;
}

}  // namespace

std::vector<data::GachaPool> offeredGachaPools(const data::Tables& tables,
                                               const std::vector<core::WarpBanner>& limited) {
    const std::vector<data::GachaPool>& pools = tables.gachaPools();
    auto find = [&pools](uint32_t id) -> const data::GachaPool* {
        for (const data::GachaPool& pool : pools) {
            if (pool.id == id) return &pool;
        }
        return nullptr;
    };

    std::vector<data::GachaPool> offered;
    const data::GachaPool* standard = find(tables.standardGacha().gachaId);
    if (standard != nullptr && standard->type == data::GachaType::Normal) offered.push_back(*standard);
    for (const core::WarpBanner& banner : limited) {
        const data::GachaPool* pool = find(banner.id);
        if (pool == nullptr || pool->type == data::GachaType::Normal) continue;
        bool taken = std::any_of(offered.begin(), offered.end(),
                                 [pool](const data::GachaPool& p) { return p.id == pool->id; });
        if (taken) continue;
        data::GachaPool entry = *pool;
        if (banner.featured != 0 && featurable(tables, entry.type, banner.featured)) {
            entry.featured = banner.featured;
        }
        offered.push_back(std::move(entry));
    }
    return offered;
}

std::optional<data::GachaPool> offeredGachaPool(const data::Tables& tables,
                                                const std::vector<core::WarpBanner>& limited,
                                                uint32_t gachaId) {
    for (data::GachaPool& pool : offeredGachaPools(tables, limited)) {
        if (pool.id == gachaId) return std::move(pool);
    }
    return std::nullopt;
}

data::GachaPool effectiveGachaPool(const data::GachaPool& pool, const data::Tables& tables,
                                   uint32_t standardFeatured) {
    if (pool.type != data::GachaType::Normal || standardFeatured == 0) return pool;
    if (!featurable(tables, data::GachaType::AvatarUp, standardFeatured)) return pool;
    data::GachaPool banner = pool;
    banner.type = data::GachaType::AvatarUp;
    banner.featured = standardFeatured;
    banner.upChance = tables.gachaUpChance(data::GachaType::AvatarUp);
    return banner;
}

std::vector<GachaPull> pullGacha(const data::GachaPool& pool, const GachaItemPools& items,
                                 GachaPity& pity, uint32_t count, const GachaRoll& roll) {
    std::vector<GachaPull> pulls;
    pulls.reserve(count);
    for (uint32_t i = 0; i < count; ++i) pulls.push_back(pullOne(pool, items, pity, roll));
    return pulls;
}

proto::GachaItem gachaResult(const GachaPull& pull, const SrToolsData& roster) {
    proto::GachaItem result;
    proto::Item& item = result.gacha_item.emplace();
    item.item_id = pull.itemId;
    item.num = 1;
    item.level = 1;
    item.rank = 1;
    // Both lists go out even when empty.
    proto::ItemList& transfer = result.transfer_item_list.emplace();
    proto::ItemList& tokens = result.token_item.emplace();
    auto token = [&tokens](uint32_t id, uint32_t num) {
        proto::Item entry;
        entry.item_id = id;
        entry.num = num;
        tokens.item_list.push_back(entry);
    };

    if (!pull.avatar) {
        if (pull.rarity == 3) {
            token(kUndyingEmbers, 20);
        } else {
            token(kUndyingStarlight, pull.rarity == 5 ? 40 : 8);
        }
        return result;
    }

    auto owned = roster.avatars.find(pull.itemId);
    if (owned == roster.avatars.end()) {
        result.is_new = true;
        return result;
    }
    bool fiveStar = pull.rarity == 5;
    if (owned->second.rank >= 6) {
        token(kUndyingStarlight, fiveStar ? 100 : 20);
        return result;
    }
    token(kUndyingStarlight, fiveStar ? 40 : 8);
    proto::Item eidolon;
    eidolon.item_id = pull.itemId + kEidolonItemOffset;
    eidolon.num = 1;
    transfer.item_list.push_back(eidolon);
    return result;
}

}  // namespace game
