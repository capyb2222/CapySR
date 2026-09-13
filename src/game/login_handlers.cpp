#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "game/handlers.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

// Story chapters the client checks before it will let you walk around; anything
// missing just stays locked.
constexpr uint32_t kUnlockedContentPackages[] = {
    200001, 200002, 200003, 200004, 200005, 200006, 200007, 200008, 200009, 200010, 200011,
    200012, 150015, 150017, 150018, 150021, 150024, 150025, 150026, 150029, 150034, 150035,
    150039, 150041, 150042, 150045, 150057, 150063, 150064, 150067, 150068, 150070, 150071,
    150073, 150074, 150075, 150076, 150077, 150078, 150079, 130011, 130012, 130013, 130014,
    140006, 171002};

void fillBasicInfo(proto::PlayerBasicInfo& info, const Player& player) {
    info.nickname = player.name();
    info.level = player.level();
    info.world_level = player.worldLevel();
    info.stamina = player.stamina();
    info.hcoin = player.hcoin();
    info.scoin = player.scoin();
    info.mcoin = player.mcoin();
}

void onGetToken(net::Session& session, const proto::PlayerGetTokenCsReq& req) {
    uint32_t uid = core::Config::get().player.uid;
    // Before the save is read: a session this login replaces saves its newest state on the
    // way out, and cannot write a stale copy over this one's later.
    session.dropOtherLogins(uid);
    auto player = std::make_shared<Player>(uid);
    player->setMainCharacter(core::Config::get().gameplay.mainCharacter);
    player->setMarchType(core::Config::get().gameplay.marchType);
    player->setGlobalBuffs(core::Config::get().gameplay.globalBuffs);
    player->load();
    session.setPlayer(player);
    session.setState(net::SessionState::WaitingForLogin);

    proto::PlayerGetTokenScRsp rsp;
    rsp.retcode = 0;
    rsp.uid = uid;
    rsp.secret_key_seed = 0;
    session.send(cmd::PlayerGetTokenScRsp, rsp);

    logging::info("game", "uid {} authenticated (account '{}')", uid, req.account_uid);
}

void onLogin(net::Session& session, const proto::PlayerLoginCsReq& req) {
    Player* player = session.player();
    if (player == nullptr) {
        logging::warn("game", "login before token from {}", session.remote().str());
        return;
    }
    player->setLoginRandom(req.login_random);
    session.setState(net::SessionState::Active);

    proto::PlayerLoginScRsp rsp;
    rsp.retcode = 0;
    rsp.login_random = req.login_random;
    rsp.server_timestamp_ms = util::nowMs();
    rsp.stamina = player->stamina();
    fillBasicInfo(rsp.basic_info.emplace(), *player);

    session.send(cmd::PlayerLoginScRsp, rsp);
    logging::info("game", "uid {} logged in (client res version {})", player->uid(),
                  req.client_res_version);
}

void onLoginFinish(net::Session& session, const proto::PlayerLoginFinishCsReq&) {
    Player* player = session.player();
    if (player != nullptr) {
        proto::ContentPackageSyncDataScNotify notify;
        auto& data = notify.data.emplace();
        for (uint32_t contentId : kUnlockedContentPackages) {
            proto::ContentPackageInfo info;
            info.content_id = contentId;
            info.status = proto::ContentPackageStatus::ContentPackageStatus_Finished;
            data.content_package_list.push_back(info);
        }
        session.send(cmd::ContentPackageSyncDataScNotify, notify);
    }

    proto::PlayerLoginFinishScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::PlayerLoginFinishScRsp, rsp);
}

void onHeartBeat(net::Session& session, const proto::PlayerHeartBeatCsReq& req) {
    proto::PlayerHeartBeatScRsp rsp;
    rsp.retcode = 0;
    rsp.client_time_ms = req.client_time_ms;
    rsp.server_time_ms = util::nowMs();
    session.send(cmd::PlayerHeartBeatScRsp, rsp);
}

void onGetBasicInfo(net::Session& session, const proto::GetBasicInfoCsReq&) {
    Player* player = session.player();
    proto::GetBasicInfoScRsp rsp;
    // PlayerSettingInfo carries sub-messages of its own, rearranged in 4.3, and the
    // client reads them without a null check, so the whole tree goes out present.
    rsp.fill(3);
    rsp.retcode = 0;
    rsp.is_gender_set = true;
    rsp.gender = player != nullptr ? player->gender() : 2;
    rsp.cur_day = 1;
    rsp.next_recover_time = static_cast<int64_t>(util::nowSec() + 300);
    session.send(cmd::GetBasicInfoScRsp, rsp);
}

// Every sub-message here has to be present: the client dereferences them without a
// null check and throws inside its own PlayerModule if one is missing.
void onGetPlayerBoardData(net::Session& session, const proto::GetPlayerBoardDataCsReq&) {
    Player* player = session.player();
    if (player == nullptr) return;

    proto::GetPlayerBoardDataScRsp rsp;
    rsp.retcode = 0;
    rsp.signature = player->signature();
    rsp.current_head_icon_id = player->headIcon();
    rsp.head_frame_info.emplace();
    auto& display = rsp.display_avatar_vec.emplace();
    display.is_display = true;

    uint32_t pos = 0;
    Roster roster = player->roster();
    for (uint32_t avatarId : player->lineups().curMembers()) {
        proto::DisplayAvatarData entry;
        entry.avatar_id = roster.resolvePath(avatarId);
        entry.pos = pos++;
        display.display_avatar_list.push_back(entry);
    }
    // Every icon the roster can show, so the profile screen is not empty.
    for (uint32_t baseId : Roster::baseAvatarIds()) {
        proto::HeadIconData icon;
        icon.id = 200000 + baseId;
        rsp.unlocked_head_icon_list.push_back(icon);
    }
    session.send(cmd::GetPlayerBoardDataScRsp, rsp);
}

void onGetPlayerDetailInfo(net::Session& session, const proto::GetPlayerDetailInfoCsReq&) {
    Player* player = session.player();
    if (player == nullptr) return;

    proto::GetPlayerDetailInfoScRsp rsp;
    rsp.retcode = 0;
    auto& detail = rsp.detail_info.emplace();
    detail.uid = player->uid();
    detail.nickname = player->name();
    detail.signature = player->signature();
    detail.level = player->level();
    detail.world_level = player->worldLevel();
    detail.head_icon = player->headIcon();
    detail.gender = player->gender();
    session.send(cmd::GetPlayerDetailInfoScRsp, rsp);
}

void onSetNickname(net::Session& session, const proto::SetNicknameCsReq& req) {
    Player* player = session.player();
    if (player != nullptr && !req.nickname.empty()) {
        player->setName(req.nickname);
        player->saveNow();
    }
    session.sendEmpty(cmd::SetNicknameScRsp);
}

void onSetSignature(net::Session& session, const proto::SetSignatureCsReq& req) {
    Player* player = session.player();
    if (player != nullptr) {
        player->setSignature(req.signature);
        player->saveNow();
    }
    proto::SetSignatureScRsp rsp;
    rsp.retcode = 0;
    rsp.signature = req.signature;
    session.send(cmd::SetSignatureScRsp, rsp);
}

void onSetHeadIcon(net::Session& session, const proto::SetHeadIconCsReq& req) {
    Player* player = session.player();
    if (player != nullptr) {
        player->setHeadIcon(req.id);
        player->saveNow();
    }
    proto::SetHeadIconScRsp rsp;
    rsp.retcode = 0;
    rsp.current_head_icon_id = req.id;
    session.send(cmd::SetHeadIconScRsp, rsp);
}

}  // namespace

void registerLoginHandlers() {
    net::on<proto::PlayerGetTokenCsReq>(cmd::PlayerGetTokenCsReq, onGetToken);
    net::on<proto::PlayerLoginCsReq>(cmd::PlayerLoginCsReq, onLogin);
    net::on<proto::PlayerLoginFinishCsReq>(cmd::PlayerLoginFinishCsReq, onLoginFinish);
    net::on<proto::PlayerHeartBeatCsReq>(cmd::PlayerHeartBeatCsReq, onHeartBeat);
    net::on<proto::GetBasicInfoCsReq>(cmd::GetBasicInfoCsReq, onGetBasicInfo);
    net::on<proto::GetPlayerBoardDataCsReq>(cmd::GetPlayerBoardDataCsReq, onGetPlayerBoardData);
    net::on<proto::GetPlayerDetailInfoCsReq>(cmd::GetPlayerDetailInfoCsReq, onGetPlayerDetailInfo);
    net::on<proto::SetNicknameCsReq>(cmd::SetNicknameCsReq, onSetNickname);
    net::on<proto::SetSignatureCsReq>(cmd::SetSignatureCsReq, onSetSignature);
    net::on<proto::SetHeadIconCsReq>(cmd::SetHeadIconCsReq, onSetHeadIcon);

    net::Handlers::get().muteLog(cmd::PlayerHeartBeatCsReq);
    net::Handlers::get().muteLog(cmd::PlayerHeartBeatScRsp);
    net::Handlers::get().muteLog(cmd::SceneEntityMoveCsReq);
    net::Handlers::get().muteLog(cmd::SceneEntityMoveScRsp);
}

}  // namespace game
