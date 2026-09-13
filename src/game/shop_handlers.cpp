#include "core/logger.h"
#include "core/util.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/player.h"
#include "game/shop.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

Player* playerOf(net::Session& session, const char* what) {
    Player* player = session.player();
    if (player == nullptr) logging::warn("game", "{} before login", what);
    return player;
}

int64_t now() { return static_cast<int64_t>(util::nowSec()); }

void onGetShopList(net::Session& session, const proto::GetShopListCsReq& req) {
    Player* player = playerOf(session, "GetShopList");
    if (player == nullptr) return;
    session.send(cmd::GetShopListScRsp, shop::list(*player, req.shop_type, now()));
}

void onBuyGoods(net::Session& session, const proto::BuyGoodsCsReq& req) {
    Player* player = playerOf(session, "BuyGoods");
    if (player == nullptr) return;

    std::vector<data::ItemStack> changed;
    proto::BuyGoodsScRsp rsp = shop::buy(*player, req, now(), changed);
    if (rsp.retcode == 0) {
        session.send(cmd::PlayerSyncScNotify, inventory::sync(*player, changed));
        player->saveNow();
    }
    session.send(cmd::BuyGoodsScRsp, rsp);
}

}  // namespace

void registerShopHandlers() {
    net::on<proto::GetShopListCsReq>(cmd::GetShopListCsReq, onGetShopList);
    net::on<proto::BuyGoodsCsReq>(cmd::BuyGoodsCsReq, onBuyGoods);
}

}  // namespace game
