#include "game/shop.h"

#include <algorithm>
#include <ctime>
#include <limits>

#include "core/logger.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/player.h"

namespace game::shop {
namespace {

// 2100-01-01: a shelf that never closes.
constexpr int64_t kForever = 4102444800;

std::tm localTime(int64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

// Light cones, relics and characters live in the srtools build, which a purchase must not
// rewrite, so only what the bag holds is sold.
bool sellable(uint32_t itemId) {
    const data::ItemInfo* item = data::Tables::get().item(itemId);
    return item != nullptr &&
           (item->mainType == "Material" || item->mainType == "Usable" || item->mainType == "Virtual");
}

}  // namespace

int64_t periodOf(data::GoodsRefresh refresh, int64_t now) {
    if (refresh == data::GoodsRefresh::Never) return 0;
    std::tm tm = localTime(now);
    if (refresh == data::GoodsRefresh::Weekly) {
        // Back to Monday; mktime carries the day across month and year ends.
        tm.tm_mday -= (tm.tm_wday + 6) % 7;
        tm.tm_hour = 12;
        tm.tm_isdst = -1;
        std::mktime(&tm);
    }
    int64_t date = int64_t{tm.tm_year + 1900} * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
    return refresh == data::GoodsRefresh::Monthly ? date / 100 : date;
}

uint32_t boughtTimes(const Player& player, const data::GoodsInfo& goods, int64_t now) {
    auto it = player.goodsPurchases().find(goods.id);
    if (it == player.goodsPurchases().end()) return 0;
    return it->second.period == periodOf(goods.refresh, now) ? it->second.times : 0;
}

proto::GetShopListScRsp list(const Player& player, uint32_t shopType, int64_t now) {
    const data::Tables& tables = data::Tables::get();
    proto::GetShopListScRsp rsp;
    rsp.retcode = 0;
    rsp.shop_type = shopType;
    for (const data::ShopInfo* info : tables.shopsOfType(shopType)) {
        proto::Shop shop;
        shop.shop_id = info->id;
        shop.city_level = 1;
        shop.end_time = kForever;
        for (uint32_t goodsId : info->goods) {
            const data::GoodsInfo* goods = tables.goods(goodsId);
            if (goods == nullptr) continue;
            proto::Goods entry;
            entry.goods_id = goods->id;
            entry.item_id = goods->itemId;
            entry.buy_times = boughtTimes(player, *goods, now);
            entry.end_time = kForever;
            shop.goods_list.push_back(entry);
        }
        rsp.shop_list.push_back(std::move(shop));
    }
    return rsp;
}

proto::BuyGoodsScRsp buy(Player& player, const proto::BuyGoodsCsReq& req, int64_t now,
                         std::vector<data::ItemStack>& changed) {
    const data::Tables& tables = data::Tables::get();
    changed.clear();
    proto::BuyGoodsScRsp rsp;
    rsp.shop_id = req.shop_id;
    rsp.goods_id = req.goods_id;

    const data::GoodsInfo* goods = tables.goods(req.goods_id);
    if (goods == nullptr || goods->shopId != req.shop_id || !sellable(goods->itemId)) {
        logging::info("shop", "goods {} of shop {} is not on sale here", req.goods_id, req.shop_id);
        rsp.retcode = fail(proto::Retcode::RET_GOODS_NOT_OPEN);
        return rsp;
    }

    uint64_t count = std::max(1u, req.goods_num);
    uint32_t bought = boughtTimes(player, *goods, now);
    rsp.goods_buy_times = bought;
    if (goods->limitTimes != 0 && bought + count > goods->limitTimes) {
        logging::info("shop", "goods {} is limited to {} per period, {} already bought", goods->id,
                      goods->limitTimes, bought);
        rsp.retcode = fail(proto::Retcode::RET_BUY_TIMES_LIMIT);
        return rsp;
    }

    constexpr uint64_t kMax = std::numeric_limits<uint32_t>::max();
    std::vector<data::ItemStack> price;
    for (const data::ItemStack& cost : goods->cost) {
        if (cost.num * count > kMax) {
            rsp.retcode = fail(proto::Retcode::RET_ITEM_COST_TOO_MUCH);
            return rsp;
        }
        price.push_back({cost.id, static_cast<uint32_t>(cost.num * count)});
    }
    if (goods->itemCount * count > kMax) {
        rsp.retcode = fail(proto::Retcode::RET_ITEM_COST_TOO_MUCH);
        return rsp;
    }
    if (!inventory::spend(player, price)) {
        for (const data::ItemStack& cost : price) {
            logging::info("shop", "goods {} costs {} x item {}, {} held -- /give {} to buy it", goods->id,
                          cost.num, cost.id, inventory::held(player, cost.id), cost.id);
        }
        rsp.retcode = fail(proto::Retcode::RET_ITEM_NOT_ENOUGH);
        return rsp;
    }

    std::vector<data::ItemStack> items{{goods->itemId, static_cast<uint32_t>(goods->itemCount * count)}};
    inventory::grant(player, items);
    if (goods->limitTimes != 0) {
        GoodsPurchase& purchase = player.goodsPurchases()[goods->id];
        purchase.period = periodOf(goods->refresh, now);
        purchase.times = bought + static_cast<uint32_t>(count);
        rsp.goods_buy_times = purchase.times;
    }

    rsp.retcode = 0;
    rsp.return_item_list = inventory::itemList(items);
    changed = price;
    changed.insert(changed.end(), items.begin(), items.end());
    return rsp;
}

}  // namespace game::shop
