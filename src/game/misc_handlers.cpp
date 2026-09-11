#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/handlers.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

// The client fires these at login and is happy with an empty reply; listing them
// keeps them out of the "unimplemented" log noise.
constexpr uint16_t kEmptyReplies[] = {
    cmd::GetUpdatedArchiveDataCsReq,
    cmd::ContentPackageGetDataCsReq,
    cmd::GetMarkItemListCsReq,
    cmd::GetAllServerPrefsDataCsReq,
    cmd::GetQuestRecordCsReq,
    cmd::GetRogueInfoCsReq,
    cmd::GetFriendListInfoCsReq,
    cmd::GetFriendApplyListInfoCsReq,
    cmd::GetFriendLoginInfoCsReq,
    cmd::GetFriendRecommendListInfoCsReq,
    cmd::GetChatEmojiListCsReq,
    cmd::GetPrivateChatHistoryCsReq,
    cmd::GetPhoneDataCsReq,
    cmd::GetMailCsReq,
    cmd::GetSecretKeyInfoCsReq,
    cmd::GetVideoVersionKeyCsReq,
    cmd::GetLevelRewardTakenListCsReq,
    cmd::GetDailyActiveInfoCsReq,
    cmd::GetCurAssistCsReq,
    cmd::GetAssistListCsReq,
    cmd::GetNpcStatusCsReq,
    cmd::GetShareDataCsReq,
    cmd::GetMultipleDropInfoCsReq,
    cmd::GetMapRotationDataCsReq,
    cmd::GetExpeditionDataCsReq,
    cmd::GetRaidInfoCsReq,
    cmd::GetMuseumInfoCsReq,
    cmd::TextJoinQueryCsReq,
    cmd::GetLoginChatInfoCsReq,
    cmd::QueryProductInfoCsReq,
    cmd::GetLoginActivityCsReq,
    cmd::GetTrialActivityDataCsReq,
    cmd::GetFightActivityDataCsReq,
    cmd::GetRechargeGiftInfoCsReq,
    cmd::GetFriendAssistListCsReq,
};

// Requests whose response the dump never named after them; without these the
// automatic empty reply has nothing to send and the client waits forever.
// The request and its response were named off different stems, so swapping CsReq for
// ScRsp cannot find them, and the client retries each of them until answered.
constexpr std::pair<uint16_t, uint16_t> kResponseAliases[] = {
    {cmd::UpdateServerPrefsCsReq, cmd::UpdateServerPrefsDataScRsp},
    {cmd::GetMatchPlayDataCsReq, cmd::MultiplayerGetMatchPlayDataScRsp},
    {cmd::SwitchHandDataCsReq, cmd::GetSwitchHandDataScRsp},
    {cmd::VoracityInvasionGetDataCsReq, cmd::GetVoracityInvasionDataScRsp},
    {cmd::PlayerReturnInfoQueryCsReq, cmd::PlayerReturnInfoQueryScRsp},
};

// The client compares this against what it asked for, so it has to be echoed.
void onSyncClientResVersion(net::Session& session, const proto::SyncClientResVersionCsReq& req) {
    proto::SyncClientResVersionScRsp rsp;
    rsp.retcode = 0;
    rsp.client_res_version = req.client_res_version;
    session.send(cmd::SyncClientResVersionScRsp, rsp);
}

// The warp pools. Only the standard pool is offered, and its ceiling as already
// claimed: nothing can pay a warp out yet.
void onGetGachaInfo(net::Session& session, const proto::GetGachaInfoCsReq&) {
    const data::StandardGacha& standard = data::Tables::get().standardGacha();
    proto::GetGachaInfoScRsp rsp;
    rsp.retcode = 0;
    auto& pool = rsp.gacha_info_list.emplace_back();
    pool.gacha_id = standard.gachaId;
    auto& ceiling = pool.gacha_ceiling.emplace();
    ceiling.is_claimed = true;
    for (uint32_t avatarId : standard.ceilingAvatars) {
        proto::GachaCeilingAvatar avatar;
        avatar.avatar_id = avatarId;
        ceiling.avatar_list.push_back(avatar);
    }
    session.send(cmd::GetGachaInfoScRsp, rsp);
}

// Activities reported as scheduled: module id, panel and window.
struct ScheduledActivity {
    uint32_t activityId;
    uint32_t panelId;
    int64_t begin;
    int64_t end;
};
constexpr ScheduledActivity kSchedule[] = {
    {5010601, 50106, 1664355600, 4294967295},
    {5011101, 50111, 1664355600, 4294967295},
    {7100101, 71001, 1762113600, 1794351599},
    {7100501, 71005, 1783886400, 1794348000},
};

void onGetActivityScheduleConfig(net::Session& session,
                                 const proto::GetActivityScheduleConfigCsReq&) {
    proto::GetActivityScheduleConfigScRsp rsp;
    rsp.retcode = 0;
    for (const ScheduledActivity& activity : kSchedule) {
        proto::ActivityScheduleData data;
        data.activity_id = activity.activityId;
        data.panel_id = activity.panelId;
        data.begin_time = activity.begin;
        data.end_time = activity.end;
        rsp.schedule_data.push_back(data);
    }
    session.send(cmd::GetActivityScheduleConfigScRsp, rsp);
}

// A "CapySR" tag on the client's version label: Lua in ClientDownloadDataScNotify that
// rewrites the VersionText object. Only the name -- the native watermark (gateway field
// 1660) stays off, it prints account data.
constexpr std::string_view kWatermarkText = "CapySR";
constexpr uint32_t kWatermarkFrom = 0xD08C4F;  // caramel
constexpr uint32_t kWatermarkTo = 0xF6DDA0;    // sand

std::string watermarkLua() {
    std::string rich;
    size_t n = kWatermarkText.size();
    for (size_t i = 0; i < n; ++i) {
        double t = n > 1 ? static_cast<double>(i) / static_cast<double>(n - 1) : 0.0;
        auto channel = [&](int shift) {
            int from = static_cast<int>((kWatermarkFrom >> shift) & 0xFF);
            int to = static_cast<int>((kWatermarkTo >> shift) & 0xFF);
            return from + static_cast<int>((to - from) * t);
        };
        char tag[24];
        std::snprintf(tag, sizeof(tag), "<color=#%02X%02X%02X>", channel(16), channel(8),
                      channel(0));
        rich += tag;
        rich += kWatermarkText[i];
        rich += "</color>";
    }
    return "pcall(function()\n"
           "    local go = CS.UnityEngine.GameObject.Find(\"VersionText\")\n"
           "    if go == nil then return end\n"
           "    local text = go:GetComponent(\"Text\")\n"
           "    text.supportRichText = true\n"
           "    text.text = \"" +
           rich +
           "\"\n"
           "    text.fontSize = 76\n"
           "end)\n";
}

void sendWatermark(net::Session& session) {
    // Every push gets a fresh version.
    static std::atomic<uint32_t> version{static_cast<uint32_t>(util::nowMs() / 1000)};
    proto::ClientDownloadDataScNotify notify;
    auto& download = notify.download_data.emplace();
    download.version = ++version;
    download.time = static_cast<int64_t>(util::nowMs() / 1000);
    download.data = watermarkLua();
    session.send(cmd::ClientDownloadDataScNotify, notify);
}

void onSetClientPaused(net::Session& session, const proto::SetClientPausedCsReq& req) {
    proto::SetClientPausedScRsp rsp;
    rsp.retcode = 0;
    rsp.paused = req.paused;
    session.send(cmd::SetClientPausedScRsp, rsp);
    // The label is on screen while paused, so it is redrawn on every toggle.
    sendWatermark(session);
}

}  // namespace

void registerMiscHandlers() {
    for (uint16_t cmdId : kEmptyReplies) net::Handlers::get().addEmpty(cmdId);
    for (const auto& [req, rsp] : kResponseAliases) net::Handlers::get().addAlias(req, rsp);
    net::on<proto::SetClientPausedCsReq>(cmd::SetClientPausedCsReq, onSetClientPaused);
    net::on<proto::GetGachaInfoCsReq>(cmd::GetGachaInfoCsReq, onGetGachaInfo);
    net::on<proto::GetActivityScheduleConfigCsReq>(cmd::GetActivityScheduleConfigCsReq,
                                                   onGetActivityScheduleConfig);
    net::on<proto::SyncClientResVersionCsReq>(cmd::SyncClientResVersionCsReq,
                                              onSyncClientResVersion);
}

void registerAllHandlers() {
    registerLoginHandlers();
    registerAvatarHandlers();
    registerInventoryHandlers();
    registerLineupHandlers();
    registerSceneHandlers();
    registerBattleHandlers();
    registerMissionHandlers();
    registerChallengeHandlers();
    registerModuleHandlers();
    registerMiscHandlers();
    logging::info("game", "{} packet handlers registered, {} answered empty",
              net::Handlers::get().count(), net::Handlers::get().emptyCount());
}

}  // namespace game
