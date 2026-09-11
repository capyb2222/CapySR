#include <algorithm>

#include <map>

#include "core/logger.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/handlers.h"
#include "game/notify.h"
#include "game/player.h"
#include "game/srtools.h"
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

void onGetAvatarData(net::Session& session, const proto::GetAvatarDataCsReq& req) {
    Player* player = playerOf(session, "GetAvatarData");
    if (player == nullptr) return;
    Roster roster = player->roster();

    proto::GetAvatarDataScRsp rsp;
    rsp.retcode = 0;
    rsp.is_get_all = req.is_get_all;
    for (uint32_t baseId : Roster::baseAvatarIds()) rsp.avatar_list.push_back(roster.toAvatar(baseId));
    // One row per path, so the client knows the traces and gear of every form.
    for (const auto& [id, avatar] : roster.data().avatars) {
        (void)id;
        rsp.avatar_path_data_info_list.push_back(roster.toPathData(avatar));
    }
    session.send(cmd::GetAvatarDataScRsp, rsp);
}

// Moves a lightcone onto an avatar, taking it off whoever had it and taking off the
// one the avatar was already wearing. Returns the unique ids that changed.
std::vector<uint32_t> dressLightcone(uint32_t avatarId, uint32_t equipmentUid) {
    std::vector<uint32_t> touched;
    SrTools::instance().mutate([&](SrToolsData& data) {
        for (Lightcone& lightcone : data.lightcones) {
            uint32_t uniqueId = equipmentUniqueId(lightcone.internalUid);
            bool wanted = equipmentUid != 0 && uniqueId == equipmentUid;
            bool worn = lightcone.equipAvatar == avatarId;
            if (wanted) {
                if (lightcone.equipAvatar != avatarId) {
                    lightcone.equipAvatar = avatarId;
                    touched.push_back(uniqueId);
                }
            } else if (worn) {
                lightcone.equipAvatar = 0;
                touched.push_back(uniqueId);
            }
        }
    });
    return touched;
}

void onDressAvatar(net::Session& session, const proto::DressAvatarCsReq& req) {
    Player* player = playerOf(session, "DressAvatar");
    if (player == nullptr) return;

    std::vector<uint32_t> touched = dressLightcone(req.avatar_id, req.equipment_unique_id);
    notify::avatarChanged(session, *player,
                          data::Tables::get().baseAvatarId(req.avatar_id), touched, {});

    session.sendEmpty(cmd::DressAvatarScRsp);
}

void onTakeOffEquipment(net::Session& session, const proto::TakeOffEquipmentCsReq& req) {
    Player* player = playerOf(session, "TakeOffEquipment");
    if (player == nullptr) return;

    std::vector<uint32_t> touched = dressLightcone(req.avatar_id, 0);
    notify::avatarChanged(session, *player,
                          data::Tables::get().baseAvatarId(req.avatar_id), touched, {});

    session.sendEmpty(cmd::TakeOffEquipmentScRsp);
}

// `wanted` maps relic slot -> the relic unique id that should end up there; a slot
// mapped to 0 is taken off.
std::vector<uint32_t> dressRelics(uint32_t avatarId,
                                  const std::map<uint32_t, uint32_t>& wanted) {
    std::vector<uint32_t> touched;
    SrTools::instance().mutate([&](SrToolsData& data) {
        for (Relic& relic : data.relics) {
            uint32_t uniqueId = relicUniqueId(relic.internalUid);
            auto slot = wanted.find(relic.slot());
            bool shouldWear = slot != wanted.end() && slot->second == uniqueId;
            bool wornHere = relic.equipAvatar == avatarId;

            if (shouldWear) {
                if (relic.equipAvatar != avatarId) {
                    relic.equipAvatar = avatarId;
                    touched.push_back(uniqueId);
                }
            } else if (wornHere && slot != wanted.end()) {
                // The avatar is changing this slot, so whatever was in it comes off.
                relic.equipAvatar = 0;
                touched.push_back(uniqueId);
            }
        }
    });
    return touched;
}

void onDressRelicAvatar(net::Session& session, const proto::DressRelicAvatarCsReq& req) {
    Player* player = playerOf(session, "DressRelicAvatar");
    if (player == nullptr) return;

    std::map<uint32_t, uint32_t> wanted;
    for (const proto::DressRelicParam& param : req.switch_list) {
        wanted[param.relic_type] = param.relic_unique_id;
    }
    std::vector<uint32_t> touched = dressRelics(req.avatar_id, wanted);
    notify::avatarChanged(session, *player,
                          data::Tables::get().baseAvatarId(req.avatar_id), {}, touched);

    proto::DressRelicAvatarScRsp rsp;
    rsp.retcode = 0;
    rsp.avatar_id = req.avatar_id;
    session.send(cmd::DressRelicAvatarScRsp, rsp);
}

void onTakeOffRelic(net::Session& session, const proto::TakeOffRelicCsReq& req) {
    Player* player = playerOf(session, "TakeOffRelic");
    if (player == nullptr) return;

    std::map<uint32_t, uint32_t> wanted;
    for (uint32_t slot : req.relic_type_list) wanted[slot] = 0;
    std::vector<uint32_t> touched = dressRelics(req.avatar_id, wanted);
    notify::avatarChanged(session, *player,
                          data::Tables::get().baseAvatarId(req.avatar_id), {}, touched);

    session.sendEmpty(cmd::TakeOffRelicScRsp);
}

void onSetAvatarPath(net::Session& session, const proto::SetAvatarPathCsReq& req) {
    Player* player = playerOf(session, "SetAvatarPath");
    if (player == nullptr) return;

    uint32_t avatarId = static_cast<uint32_t>(req.avatar_id);
    uint32_t baseId = data::Tables::get().baseAvatarId(avatarId);
    if (baseId == 8001) {
        player->setMainCharacter(avatarId);
    } else if (baseId == 1001) {
        player->setMarchType(avatarId);
    } else {
        logging::debug("game", "avatar {} has no paths to switch between", avatarId);
    }
    player->saveNow();

    proto::AvatarPathChangedNotify changed;
    changed.base_avatar_id = baseId;
    changed.cur_multi_path_avatar_type = static_cast<proto::MultiPathAvatarType>(avatarId);
    session.send(cmd::AvatarPathChangedNotify, changed);

    // The party may be showing this avatar, so the world models change with it.
    if (player->lineups().cur().contains(baseId)) notify::lineupChanged(session, *player);

    proto::SetAvatarPathScRsp rsp;
    rsp.retcode = 0;
    rsp.avatar_id = req.avatar_id;
    session.send(cmd::SetAvatarPathScRsp, rsp);
}

void onSetAvatarEnhancedId(net::Session& session, const proto::SetAvatarEnhancedIdCsReq& req) {
    Player* player = playerOf(session, "SetAvatarEnhancedId");
    if (player == nullptr) return;

    uint32_t avatarId = req.avatar_id;
    uint32_t enhancedId = req.enhanced_id;
    SrTools::instance().mutate([&](SrToolsData& data) {
        auto it = data.avatars.find(avatarId);
        if (it != data.avatars.end()) it->second.enhancedId = enhancedId;
    });
    notify::avatarChanged(session, *player, data::Tables::get().baseAvatarId(avatarId), {}, {});

    proto::SetAvatarEnhancedIdScRsp rsp;
    rsp.retcode = 0;
    rsp.growth_avatar_id = avatarId;
    rsp.unk_enhanced_id = enhancedId;
    session.send(cmd::SetAvatarEnhancedIdScRsp, rsp);
}

void onTakePromotionReward(net::Session& session, const proto::TakePromotionRewardCsReq&) {
    // Every avatar already reports its promotion rewards as taken.
    proto::TakePromotionRewardScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::TakePromotionRewardScRsp, rsp);
}

void onGetBigDataAllRecommend(net::Session& session,
                              const proto::GetBigDataAllRecommendCsReq& req) {
    proto::GetBigDataAllRecommendScRsp rsp;
    rsp.retcode = 0;
    rsp.big_data_recommend_type = req.big_data_recommend_type;
    if (req.big_data_recommend_type ==
        proto::BigDataRecommendType::BIG_DATA_RECOMMEND_TYPE_RELIC_AVATAR) {
        auto& recommend = rsp.relic_avatar.emplace();
        for (const auto& [setId, avatars] : data::SceneRes::get().relicRecommend()) {
            proto::RecomendedAvatarInfo info;
            info.relic_set_id = setId;
            info.avatar_id_list = avatars;
            info.recommend_avatar_id = avatars.empty() ? 0 : avatars.front();
            recommend.recommended_avatar_info_list.push_back(std::move(info));
        }
        rsp.OODKOEILJCD_case = proto::GetBigDataAllRecommendScRsp::k_relic_avatar;
    }
    session.send(cmd::GetBigDataAllRecommendScRsp, rsp);
}

}  // namespace

void registerAvatarHandlers() {
    net::on<proto::GetAvatarDataCsReq>(cmd::GetAvatarDataCsReq, onGetAvatarData);
    net::on<proto::DressAvatarCsReq>(cmd::DressAvatarCsReq, onDressAvatar);
    net::on<proto::TakeOffEquipmentCsReq>(cmd::TakeOffEquipmentCsReq, onTakeOffEquipment);
    net::on<proto::DressRelicAvatarCsReq>(cmd::DressRelicAvatarCsReq, onDressRelicAvatar);
    net::on<proto::TakeOffRelicCsReq>(cmd::TakeOffRelicCsReq, onTakeOffRelic);
    net::on<proto::SetAvatarPathCsReq>(cmd::SetAvatarPathCsReq, onSetAvatarPath);
    net::on<proto::SetAvatarEnhancedIdCsReq>(cmd::SetAvatarEnhancedIdCsReq, onSetAvatarEnhancedId);
    net::on<proto::TakePromotionRewardCsReq>(cmd::TakePromotionRewardCsReq, onTakePromotionReward);
    net::on<proto::GetBigDataAllRecommendCsReq>(cmd::GetBigDataAllRecommendCsReq,
                                                onGetBigDataAllRecommend);
}

}  // namespace game
