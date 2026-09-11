#include "core/logger.h"
#include "game/handlers.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

// Warp currency, so the banner screens are usable without a real inventory.
constexpr uint32_t kStockedMaterials[] = {101, 102};
constexpr uint32_t kStockedAmount = 999999;

void onGetBag(net::Session& session, const proto::GetBagCsReq&) {
    Player* player = session.player();
    if (player == nullptr) {
        logging::warn("game", "GetBag before login");
        return;
    }
    Roster roster = player->roster();

    proto::GetBagScRsp rsp;
    rsp.retcode = 0;
    for (const Lightcone& lightcone : roster.data().lightcones) {
        rsp.equipment_list.push_back(roster.toEquipment(lightcone));
    }
    for (const Relic& relic : roster.data().relics) {
        rsp.relic_list.push_back(roster.toRelic(relic));
    }
    for (uint32_t tid : kStockedMaterials) {
        proto::Material material;
        material.tid = tid;
        material.num = kStockedAmount;
        rsp.material_list.push_back(material);
    }
    session.send(cmd::GetBagScRsp, rsp);
}

}  // namespace

void registerInventoryHandlers() {
    net::on<proto::GetBagCsReq>(cmd::GetBagCsReq, onGetBag);
}

}  // namespace game
