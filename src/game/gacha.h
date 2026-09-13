#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "data/excel.h"
#include "game/srtools.h"
#include "proto/gen/protos.h"

namespace game {

struct GachaPity {
    uint32_t sinceFive = 0;
    uint32_t sinceFour = 0;
    bool guaranteed = false;  // the next 5* is the featured one
    uint32_t total = 0;
};

// Every limited pool of one kind shares its counter.
struct GachaProgress {
    GachaPity standard;
    GachaPity character;
    GachaPity lightcone;

    GachaPity& forType(data::GachaType type);
};

struct GachaItemPools {
    std::vector<uint32_t> fiveStarAvatars;
    std::vector<uint32_t> fiveStarLightcones;
    std::vector<uint32_t> fourStarAvatars;
    std::vector<uint32_t> fourStarLightcones;
    std::vector<uint32_t> threeStarLightcones;

    bool complete() const;
};

GachaItemPools gachaItemPools(const data::Tables& tables);

struct GachaPull {
    uint32_t itemId = 0;
    uint32_t rarity = 3;
    bool avatar = false;
    bool featured = false;    // a limited 5* that was the featured one
    bool guaranteed = false;  // and was owed it by an earlier lost 50/50
};

// Uniform in [0, 1); injectable so the rates can be tested.
using GachaRoll = std::function<double()>;
double gachaRoll();

// The standard pool, then each configured limited pool the tables have, featuring what the
// config names. An override that is not a 5* of the pool's kind is ignored.
std::vector<data::GachaPool> offeredGachaPools(const data::Tables& tables,
                                               const std::vector<core::WarpBanner>& limited);
std::optional<data::GachaPool> offeredGachaPool(const data::Tables& tables,
                                                const std::vector<core::WarpBanner>& limited,
                                                uint32_t gachaId);
// The standard pool as a character banner featuring `standardFeatured`. Anything but a 5*
// character leaves the pool as it is, and so does any limited pool.
data::GachaPool effectiveGachaPool(const data::GachaPool& pool, const data::Tables& tables,
                                   uint32_t standardFeatured);

// `items` must be complete().
std::vector<GachaPull> pullGacha(const data::GachaPool& pool, const GachaItemPools& items,
                                 GachaPity& pity, uint32_t count, const GachaRoll& roll);

// The result card. Nothing is granted: the srtools build stays the roster.
proto::GachaItem gachaResult(const GachaPull& pull, const SrToolsData& roster);

}  // namespace game
