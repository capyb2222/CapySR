#pragma once

#include <cstdint>
#include <vector>

#include "data/excel.h"
#include "proto/gen/protos.h"

namespace game {

class Player;

// The in-game shops: ShopConfig's shelves, stocked from ShopGoodsConfig.
namespace shop {

// The refresh period `now` falls in, in local time: yyyymmdd for a day or for the Monday
// that starts a week, yyyymm for a month. Goods that never refresh are always in 0.
int64_t periodOf(data::GoodsRefresh refresh, int64_t now);

// How many of `goods` were bought in the current period.
uint32_t boughtTimes(const Player& player, const data::GoodsInfo& goods, int64_t now);

proto::GetShopListScRsp list(const Player& player, uint32_t shopType, int64_t now);

// Checks the limit and the price, takes the price and hands the goods over. `changed`
// gets every item whose count moved, for the sync.
proto::BuyGoodsScRsp buy(Player& player, const proto::BuyGoodsCsReq& req, int64_t now,
                         std::vector<data::ItemStack>& changed);

}  // namespace shop
}  // namespace game
