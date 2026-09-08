using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;

namespace CapySR.GameServer.Handlers;

// one friend on the list: the server itself. talking to it runs commands.
[Handlers]
public static class ChatHandlers
{
    public const uint BotUid = 2000;
    public const string BotName = "CapySR";

    public static Task OnGetFriendListInfo(PlayerSession session, GetFriendListInfoCsReq request)
    {
        var response = new GetFriendListInfoScRsp { Retcode = 0 };

        response.FriendList.Add(new FriendSimpleInfo
        {
            CreateTime = 0,
            RemarkName = string.Empty,
            IsMarked = true,
            PlayerInfo = new PlayerSimpleInfo
            {
                Uid = BotUid,
                Nickname = BotName,
                Level = 70,
                HeadIcon = 201001,
                Signature = "Message me /help for commands",
                OnlineStatus = FriendOnlineStatus.Online,
                IsBanned = false,
            },
        });

        return session.SendAsync(response);
    }

    public static Task OnGetChatFriendHistory(PlayerSession session, GetChatFriendHistoryCsReq request)
    {
        var response = new GetChatFriendHistoryScRsp { Retcode = 0 };

        response.FriendHistoryInfo.Add(new FriendHistoryInfo
        {
            ContactSide = BotUid,
            LastSendTime = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
        });

        return session.SendAsync(response);
    }

    public static Task OnGetPrivateChatHistory(PlayerSession session, GetPrivateChatHistoryCsReq request)
    {
        var response = new GetPrivateChatHistoryScRsp
        {
            Retcode = 0,
            TargetSide = request.TargetSide,
            ContactSide = BotUid,
        };

        if (request.ContactSide == BotUid || request.TargetSide == BotUid)
        {
            response.ChatMessageList.Add(BotMessage(session, "Welcome to CapySR. Type /help for the command list."));
        }

        return session.SendAsync(response);
    }

    public static Task OnGetChatEmojiList(PlayerSession session, GetChatEmojiListCsReq request) =>
        session.SendAsync(new GetChatEmojiListScRsp { Retcode = 0 });

    public static async Task OnSendMsg(PlayerSession session, SendMsgCsReq request)
    {
        var chat = request.MessageDatas?.ChatData;
        var text = chat is { HasMessageText: true } ? chat.MessageText.Trim('\0').Trim() : string.Empty;

        await session.SendAsync(new SendMsgScRsp { Retcode = 0 });

        if (text.Length == 0)
        {
            return;
        }

        if (text.StartsWith('/'))
        {
            await ChatCommands.RunAsync(session, text[1..]);
        }
        else
        {
            await session.SendBotMessageAsync("I only understand commands. Try /help.");
        }
    }

    public static Task SendBotMessageAsync(this PlayerSession session, string text) =>
        session.SendAsync(new RevcMsgScNotify
        {
            ChatType = ChatType.Private,
            SourceUid = BotUid,
            RecvMessageData = BotMessage(session, text),
        });

    private static ChatMessageData BotMessage(PlayerSession session, string text) => new()
    {
        CreateTime = (ulong)DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
        CKHPFFENOBE = new EKNABKLPEEL { RoleId = BotUid, KPOBMNLKLOK = HCMEILLLKBD.Jdoaipkbipe },
        BKOALKHDLOB = new EKNABKLPEEL { RoleId = session.Player.Uid, KPOBMNLKLOK = HCMEILLLKBD.Jdoaipkbipe },
        MessageDatas =
        {
            new MessageChatData
            {
                MessageType = MsgType.CustomText,
                ChatData = new ChatData { MessageText = text },
            },
        },
    };
}
