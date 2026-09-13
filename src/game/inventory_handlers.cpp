#include <ctime>
#include <vector>

#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

// Warp currency, kept topped up: pulls only show results and never spend it.
constexpr uint32_t kStockedMaterials[] = {101, 102};
constexpr uint32_t kStockedAmount = 999999;

Player* playerOf(net::Session& session, const char* what) {
    Player* player = session.player();
    if (player == nullptr) logging::warn("game", "{} before login", what);
    return player;
}

bool stocked(uint32_t tid) {
    for (uint32_t id : kStockedMaterials) {
        if (id == tid) return true;
    }
    return false;
}

int64_t now() { return static_cast<int64_t>(util::nowSec()); }

// The local date as yyyymmdd, which the daily stamina purchases count against.
int64_t localDate(int64_t seconds) {
    std::time_t t = static_cast<std::time_t>(seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return int64_t{tm.tm_year + 1900} * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

void onGetBag(net::Session& session, const proto::GetBagCsReq&) {
    Player* player = playerOf(session, "GetBag");
    if (player == nullptr) return;
    Roster roster = player->roster();

    proto::GetBagScRsp rsp;
    rsp.retcode = 0;
    for (const Lightcone& lightcone : roster.data().lightcones) {
        rsp.equipment_list.push_back(roster.toEquipment(lightcone));
    }
    for (const Relic& relic : roster.data().relics) {
        rsp.relic_list.push_back(roster.toRelic(relic));
    }
    for (const auto& [tid, count] : player->inventory().items) {
        if (count == 0 || stocked(tid)) continue;
        proto::Material material;
        material.tid = tid;
        material.num = count;
        rsp.material_list.push_back(material);
    }
    for (uint32_t tid : kStockedMaterials) {
        proto::Material material;
        material.tid = tid;
        material.num = kStockedAmount;
        rsp.material_list.push_back(material);
    }
    session.send(cmd::GetBagScRsp, rsp);
}

// Stellar Jade for stamina, at StaminaSaleConfig's price for today's purchase number.
void onExchangeStamina(net::Session& session, const proto::ExchangeStaminaCsReq&) {
    Player* player = playerOf(session, "ExchangeStamina");
    if (player == nullptr) return;

    const data::Tables& tables = data::Tables::get();
    inventory::refreshStamina(*player, now());
    Inventory& bag = player->inventory();
    if (int64_t today = localDate(now()); bag.purchaseDay != today) {
        bag.purchaseDay = today;
        bag.purchasesToday = 0;
    }

    const std::vector<uint32_t>& prices = tables.staminaPrices();
    proto::ExchangeStaminaScRsp rsp;
    if (bag.purchasesToday >= prices.size()) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_EXCHANGE_STAMINA_TIMES);
    } else if (bag.hcoin < prices[bag.purchasesToday]) {
        rsp.retcode = fail(proto::Retcode::RET_ITEM_NOT_ENOUGH);
    } else {
        uint32_t price = prices[bag.purchasesToday];
        bag.hcoin -= price;
        ++bag.purchasesToday;
        inventory::grant(*player, {{inventory::kStamina, tables.staminaPerPurchase()}});

        rsp.retcode = 0;
        rsp.stamina_add = tables.staminaPerPurchase();
        rsp.exchange_times = bag.purchasesToday;
        rsp.last_recover_time = inventory::nextRecoverTime(*player);
        proto::ItemCost cost;
        auto& jade = cost.pile_item.emplace();
        jade.item_id = inventory::kStellarJade;
        jade.item_num = price;
        cost.item_case = proto::ItemCost::k_pile_item;
        rsp.item_cost_list.push_back(cost);

        session.send(cmd::PlayerSyncScNotify, inventory::sync(*player, {}));
        session.send(cmd::StaminaInfoScNotify, inventory::staminaInfo(*player));
        player->saveNow();
    }
    session.send(cmd::ExchangeStaminaScRsp, rsp);
}

// Moves banked reserve stamina into stamina.
void onReserveStaminaExchange(net::Session& session, const proto::ReserveStaminaExchangeCsReq& req) {
    Player* player = playerOf(session, "ReserveStaminaExchange");
    if (player == nullptr) return;

    inventory::refreshStamina(*player, now());
    Inventory& bag = player->inventory();
    proto::ReserveStaminaExchangeScRsp rsp;
    if (req.num == 0 || req.num > bag.reserveStamina) {
        rsp.retcode = fail(proto::Retcode::RET_ITEM_NOT_ENOUGH);
    } else {
        bag.reserveStamina -= req.num;
        inventory::grant(*player, {{inventory::kStamina, req.num}});
        rsp.retcode = 0;
        rsp.num = req.num;
        session.send(cmd::PlayerSyncScNotify, inventory::sync(*player, {}));
        session.send(cmd::StaminaInfoScNotify, inventory::staminaInfo(*player));
        player->saveNow();
    }
    session.send(cmd::ReserveStaminaExchangeScRsp, rsp);
}

}  // namespace

void registerInventoryHandlers() {
    net::on<proto::GetBagCsReq>(cmd::GetBagCsReq, onGetBag);
    net::on<proto::ExchangeStaminaCsReq>(cmd::ExchangeStaminaCsReq, onExchangeStamina);
    net::on<proto::ReserveStaminaExchangeCsReq>(cmd::ReserveStaminaExchangeCsReq,
                                                onReserveStaminaExchange);
}

}  // namespace game
