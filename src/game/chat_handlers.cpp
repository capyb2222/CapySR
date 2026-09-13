// The CapySR console, as a friend. There is no GM panel on a private server, so the
// server puts itself in the friend list and reads whatever is typed at it: a line
// starting with '/' runs as a command, anything else is echoed back with a nudge.
#include <string>
#include <vector>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "game/command.h"
#include "game/handlers.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

constexpr uint32_t kConsoleUid = command::kConsoleUid;

// One line of chat, attributed to `senderUid`.
proto::ChatMessageData chatLine(const std::string& text, uint32_t senderUid) {
    proto::ChatMessageData message;
    message.create_time = static_cast<uint64_t>(util::nowSec());
    proto::MessageChatData& data = message.message_datas.emplace_back();
    data.message_type = proto::MsgType::MsgType_CustomText;
    // A oneof scalar is only written when its case is set; without this the line goes
    // out empty and the client shows a blank bubble.
    proto::ChatData& chat = data.chat_data.emplace();
    chat.message_text = text;
    chat.PGBLIFDKBHK_case = proto::ChatData::k_message_text;

    // Two unnamed fields carry the same {role type, uid} pair and both have to name the
    // sender, in history and in a live push alike. Filling one renders nothing at all.
    proto::EKNABKLPEEL side;
    side.KPOBMNLKLOK = proto::HCMEILLLKBD::HCMEILLLKBD_JDOAIPKBIPE;
    side.role_id = senderUid;
    message.CKHPFFENOBE = side;
    message.BKOALKHDLOB = side;
    return message;
}

void say(net::Session& session, const std::string& text) {
    const Player* player = session.player();
    if (player == nullptr) return;

    proto::RevcMsgScNotify notify;
    notify.chat_type = proto::ChatType::ChatType_Private;
    // Named `source_uid` in the dump, but it is the side the message is *for*. With the
    // sender's uid in it the client has no thread to file the line under, and shows
    // nothing.
    notify.source_uid = player->uid();
    notify.recv_message_data = chatLine(text, kConsoleUid);
    session.send(cmd::RevcMsgScNotify, notify);
}

// The client does not draw the line it just sent -- it waits to be handed it back. So
// the player's own message goes home the same way, from the player, addressed to the
// console.
void echo(net::Session& session, const std::string& text) {
    const Player* player = session.player();
    if (player == nullptr) return;

    proto::RevcMsgScNotify notify;
    notify.chat_type = proto::ChatType::ChatType_Private;
    notify.source_uid = kConsoleUid;
    notify.recv_message_data = chatLine(text, player->uid());
    session.send(cmd::RevcMsgScNotify, notify);
}

void onGetFriendListInfo(net::Session& session, const proto::GetFriendListInfoCsReq&) {
    const core::PlayerDefaults& defaults = core::Config::get().player;
    proto::GetFriendListInfoScRsp rsp;
    rsp.retcode = 0;

    proto::FriendSimpleInfo& friendInfo = rsp.friend_list.emplace_back();
    friendInfo.remark_name = "CapySR";
    friendInfo.is_marked = true;
    friendInfo.create_time = 0;
    friendInfo.playing_state = proto::PlayingState::PlayingState_None;
    proto::PlayerSimpleInfo& info = friendInfo.player_info.emplace();
    info.uid = kConsoleUid;
    info.nickname = "CapySR";
    info.signature = "type /help for the command list";
    info.level = defaults.level;
    info.head_icon = defaults.headIcon;
    info.online_status = proto::FriendOnlineStatus::FRIEND_ONLINE_STATUS_ONLINE;
    info.last_active_time = static_cast<int64_t>(util::nowSec());
    // Without these two the client drops the entry from its own list the next time the
    // friend screen opens, and never asks for the list again.
    info.platform = proto::PlatformType::PC;
    info.chat_bubble_id = 220005;

    session.send(cmd::GetFriendListInfoScRsp, rsp);
}

void onGetFriendLoginInfo(net::Session& session, const proto::GetFriendLoginInfoCsReq&) {
    proto::GetFriendLoginInfoScRsp rsp;
    rsp.retcode = 0;
    rsp._friend_uid_list.push_back(kConsoleUid);
    rsp.is_allow_other_apply = true;
    session.send(cmd::GetFriendLoginInfoScRsp, rsp);
}

void onGetChatEmojiList(net::Session& session, const proto::GetChatEmojiListCsReq&) {
    proto::GetChatEmojiListScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::GetChatEmojiListScRsp, rsp);
}

// The chat tab's conversation list. Without the console in it the thread exists but
// there is nothing to tap to open it.
void onGetChatFriendHistory(net::Session& session, const proto::GetChatFriendHistoryCsReq&) {
    proto::GetChatFriendHistoryScRsp rsp;
    rsp.retcode = 0;
    proto::FriendHistoryInfo& entry = rsp.friend_history_info.emplace_back();
    entry.contact_side = kConsoleUid;
    entry.last_send_time = static_cast<int64_t>(util::nowSec());
    session.send(cmd::GetChatFriendHistoryScRsp, rsp);
}

void onGetLoginChatInfo(net::Session& session, const proto::GetLoginChatInfoCsReq&) {
    proto::GetLoginChatInfoScRsp rsp;
    rsp.retcode = 0;
    rsp.contact_id_list.push_back(kConsoleUid);
    session.send(cmd::GetLoginChatInfoScRsp, rsp);
}

void onGetPrivateChatHistory(net::Session& session, const proto::GetPrivateChatHistoryCsReq& req) {
    proto::GetPrivateChatHistoryScRsp rsp;
    rsp.retcode = 0;
    rsp.target_side = req.target_side;
    rsp.contact_side = kConsoleUid;
    if (req.contact_side == kConsoleUid || req.target_side == kConsoleUid) {
        for (const char* line : {"CapySR console.", "Type /help for the command list."}) {
            rsp.chat_message_list.push_back(chatLine(line, kConsoleUid));
        }
    }
    session.send(cmd::GetPrivateChatHistoryScRsp, rsp);
}

void onSendMsg(net::Session& session, const proto::SendMsgCsReq& req) {
    std::string text;
    if (req.message_datas && req.message_datas->chat_data) {
        text = req.message_datas->chat_data->message_text;
    }
    bool toConsole = req.target_list.empty() ||
                     std::find(req.target_list.begin(), req.target_list.end(), kConsoleUid) !=
                         req.target_list.end();

    // The replies go out before the ScRsp: the client stops listening for the thread
    // once the request it sent has been answered.
    if (toConsole && !text.empty()) {
        echo(session, text);
        if (text.front() == '/') {
            for (const std::string& line : command::run(session, text)) say(session, line);
        } else {
            say(session, "type /help for the command list");
        }
    }

    proto::SendMsgScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::SendMsgScRsp, rsp);
}

}  // namespace

void registerChatHandlers() {
    net::on<proto::GetFriendListInfoCsReq>(cmd::GetFriendListInfoCsReq, onGetFriendListInfo);
    net::on<proto::GetFriendLoginInfoCsReq>(cmd::GetFriendLoginInfoCsReq, onGetFriendLoginInfo);
    net::on<proto::GetChatEmojiListCsReq>(cmd::GetChatEmojiListCsReq, onGetChatEmojiList);
    net::on<proto::GetChatFriendHistoryCsReq>(cmd::GetChatFriendHistoryCsReq, onGetChatFriendHistory);
    net::on<proto::GetLoginChatInfoCsReq>(cmd::GetLoginChatInfoCsReq, onGetLoginChatInfo);
    net::on<proto::GetPrivateChatHistoryCsReq>(cmd::GetPrivateChatHistoryCsReq, onGetPrivateChatHistory);
    net::on<proto::SendMsgCsReq>(cmd::SendMsgCsReq, onSendMsg);
}

}  // namespace game
