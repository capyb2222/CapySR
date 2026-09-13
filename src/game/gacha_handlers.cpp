#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/gacha.h"
#include "game/handlers.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

constexpr int64_t kLimitedEndTime = 4102416000;  // 2100-01-01
// Character pools carry this in item_detail_list.
constexpr uint32_t kAvatarPoolDetail = 11;

// Always claimed: nothing is granted, so the 300-pull pick has nothing to pay out.
proto::GachaCeiling standardCeiling(const Player* player) {
    const data::StandardGacha& standard = data::Tables::get().standardGacha();
    proto::GachaCeiling ceiling;
    ceiling.is_claimed = true;
    uint32_t pulls = player != nullptr ? player->gacha().standard.total : 0;
    ceiling.ceiling_num = std::min(pulls, standard.ceilingNum);
    for (uint32_t avatarId : standard.ceilingAvatars) {
        proto::GachaCeilingAvatar avatar;
        avatar.avatar_id = avatarId;
        ceiling.avatar_list.push_back(avatar);
    }
    return ceiling;
}

void onGetGachaInfo(net::Session& session, const proto::GetGachaInfoCsReq&) {
    proto::GetGachaInfoScRsp rsp;
    rsp.retcode = 0;
    rsp.gacha_random = util::randomRange(1000, 1999);
    const std::vector<core::WarpBanner>& limited = core::Config::get().gameplay.limitedWarpPools;
    for (const data::GachaPool& pool : offeredGachaPools(data::Tables::get(), limited)) {
        proto::GachaInfo& info = rsp.gacha_info_list.emplace_back();
        info.gacha_id = pool.id;
        if (pool.type == data::GachaType::Normal) {
            info.gacha_ceiling.emplace() = standardCeiling(session.player());
            continue;
        }
        info.end_time = kLimitedEndTime;
        info.prize_item_list.push_back(pool.featured);
        if (pool.type == data::GachaType::AvatarUp) info.item_detail_list.push_back(kAvatarPoolDetail);
    }
    session.send(cmd::GetGachaInfoScRsp, rsp);
}

void onDoGacha(net::Session& session, const proto::DoGachaCsReq& req) {
    proto::DoGachaScRsp rsp;
    rsp.gacha_id = req.gacha_id;
    rsp.gacha_num = req.gacha_num;

    Player* player = session.player();
    const data::Tables& tables = data::Tables::get();
    std::optional<data::GachaPool> pool =
        offeredGachaPool(tables, core::Config::get().gameplay.limitedWarpPools, req.gacha_id);
    if (player == nullptr) {
        rsp.retcode = fail(proto::Retcode::RET_FAIL);
    } else if (!pool) {
        rsp.retcode = fail(proto::Retcode::RET_GACHA_ID_NOT_EXIST);
    } else if (req.gacha_num != 1 && req.gacha_num != 10) {
        rsp.retcode = fail(proto::Retcode::RET_GACHA_NUM_INVALID);
    } else {
        GachaItemPools items = gachaItemPools(tables);
        if (!items.complete()) {
            logging::warn("game", "warp {} has an empty item pool, check the tables", pool->id);
            rsp.retcode = fail(proto::Retcode::RET_GACHA_NOT_SUPPORT);
        } else {
            uint32_t featured = core::Config::get().gameplay.standardWarpFeatured;
            data::GachaPool banner = effectiveGachaPool(*pool, tables, featured);
            if (pool->type == data::GachaType::Normal && featured != 0 && banner.featured == 0) {
                logging::warn("game", "standard_warp_featured {} is not a 5* character", featured);
            }
            Roster roster = player->roster();
            GachaPity& pity = player->gacha().forType(banner.type);
            uint32_t fiveStars = 0;
            for (const GachaPull& pull : pullGacha(banner, items, pity, req.gacha_num, gachaRoll)) {
                if (pull.rarity == 5) {
                    ++fiveStars;
                    const char* how = banner.type == data::GachaType::Normal ? "standard"
                                      : pull.guaranteed                      ? "guaranteed"
                                      : pull.featured                        ? "won the 50/50"
                                                                             : "lost the 50/50";
                    logging::info("game", "warp {} 5*: {} ({})", banner.id, pull.itemId, how);
                }
                rsp.gacha_item_list.push_back(gachaResult(pull, roster.data()));
            }
            player->saveNow();
            logging::info("game", "warp {} x{}: {} 5*, {} pulls since the last", banner.id,
                          req.gacha_num, fiveStars, pity.sinceFive);
        }
    }
    session.send(cmd::DoGachaScRsp, rsp);
}

void onGetGachaCeiling(net::Session& session, const proto::GetGachaCeilingCsReq& req) {
    proto::GetGachaCeilingScRsp rsp;
    rsp.retcode = 0;
    rsp.gacha_type = req.DDMCNOJFGON;
    rsp.gacha_ceiling.emplace() = standardCeiling(session.player());
    session.send(cmd::GetGachaCeilingScRsp, rsp);
}

void onExchangeGachaCeiling(net::Session& session, const proto::ExchangeGachaCeilingCsReq& req) {
    proto::ExchangeGachaCeilingScRsp rsp;
    rsp.retcode = fail(proto::Retcode::RET_GACHA_CEILING_CLOSE);
    rsp.gacha_type = req.gacha_type;
    rsp.avatar_id = req.avatar_id;
    rsp.gacha_ceiling.emplace() = standardCeiling(session.player());
    rsp.transfer_item_list.emplace();
    session.send(cmd::ExchangeGachaCeilingScRsp, rsp);
}

// A lost 50/50 always draws from the standard seven, so a custom pick is refused.
void onSetGachaDecideItem(net::Session& session, const proto::SetGachaDecideItemCsReq&) {
    proto::SetGachaDecideItemScRsp rsp;
    rsp.retcode = fail(proto::Retcode::RET_GACHA_NOT_SUPPORT);
    rsp.NBLOJLDLBEB.emplace();
    session.send(cmd::SetGachaDecideItemScRsp, rsp);
}

}  // namespace

void registerGachaHandlers() {
    net::on<proto::GetGachaInfoCsReq>(cmd::GetGachaInfoCsReq, onGetGachaInfo);
    net::on<proto::DoGachaCsReq>(cmd::DoGachaCsReq, onDoGacha);
    net::on<proto::GetGachaCeilingCsReq>(cmd::GetGachaCeilingCsReq, onGetGachaCeiling);
    net::on<proto::ExchangeGachaCeilingCsReq>(cmd::ExchangeGachaCeilingCsReq, onExchangeGachaCeiling);
    net::on<proto::SetGachaDecideItemCsReq>(cmd::SetGachaDecideItemCsReq, onSetGachaDecideItem);
}

}  // namespace game
