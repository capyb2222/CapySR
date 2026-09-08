using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

[Handlers]
public static class ProfileHandlers
{
    private const uint HeadIconBase = 200_000;

    public static Task OnGetPlayerBoardData(PlayerSession session, GetPlayerBoardDataCsReq request)
    {
        var world = session.World;
        var player = session.Player;

        var response = new GetPlayerBoardDataScRsp
        {
            Retcode = 0,
            Signature = player.Signature,
            CurrentHeadIconId = player.HeadIcon,
            DisplayAvatarVec = new DisplayAvatarVec { IsDisplay = true },
        };

        // one portrait per owned character; the icon table is the authority when present
        var owned = Roster.OwnedIds(world).ToHashSet();

        var icons = world.Data.HeadIcons.Count > 0
            ? world.Data.HeadIcons.Values.Where(i => owned.Contains(i.AvatarID)).Select(i => i.ID)
            : owned.Select(id => HeadIconBase + id);

        foreach (var iconId in icons.Order())
        {
            response.UnlockedHeadIconList.Add(new HeadIconData { Id = iconId });
        }

        var shown = player.DisplayAvatars.Count > 0 ? player.DisplayAvatars : player.Lineups.Current.AvatarIds;

        for (var i = 0; i < shown.Count; i++)
        {
            response.DisplayAvatarVec.DisplayAvatarList.Add(new DisplayAvatarData { Pos = (uint)i, AvatarId = shown[i] });
        }

        return session.SendAsync(response);
    }

    public static Task OnSetDisplayAvatar(PlayerSession session, SetDisplayAvatarCsReq request)
    {
        var player = session.Player;
        player.DisplayAvatars.Clear();

        foreach (var entry in request.DisplayAvatarList.OrderBy(d => d.Pos))
        {
            if (session.OwnsBase(entry.AvatarId) && !player.DisplayAvatars.Contains(entry.AvatarId))
            {
                player.DisplayAvatars.Add(entry.AvatarId);
            }
        }

        player.Touch();

        var response = new SetDisplayAvatarScRsp { Retcode = 0 };

        for (var i = 0; i < player.DisplayAvatars.Count; i++)
        {
            response.DisplayAvatarList.Add(new DisplayAvatarData { Pos = (uint)i, AvatarId = player.DisplayAvatars[i] });
        }

        return session.SendAsync(response);
    }

    public static Task OnSetHeadIcon(PlayerSession session, SetHeadIconCsReq request)
    {
        session.Player.HeadIcon = request.Id;
        session.Player.Touch();

        return session.SendAsync(new SetHeadIconScRsp { Retcode = 0, CurrentHeadIconId = request.Id });
    }

    public static Task OnSetSignature(PlayerSession session, SetSignatureCsReq request)
    {
        session.Player.Signature = request.Signature;
        session.Player.Touch();
        return session.SendAsync(new SetSignatureScRsp { Retcode = 0, Signature = request.Signature });
    }

    public static Task OnSetNickname(PlayerSession session, SetNicknameCsReq request)
    {
        session.Player.Nickname = request.Nickname;
        session.Player.Touch();

        return session.SendAsync(new SetNicknameScRsp
        {
            Retcode = 0,
            IsModify = request.IsModify,
            SetTime = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
        });
    }

    // the client picks its own language; echo the choice back or the selector appears to do nothing
    public static Task OnSetLanguage(PlayerSession session, SetLanguageCsReq request)
    {
        session.Player.Language = request.HOFEMNAIIHB;
        session.Logger.LogInformation("language set to {Language}", request.HOFEMNAIIHB);

        return session.SendAsync(new SetLanguageScRsp { Retcode = 0, HOFEMNAIIHB = request.HOFEMNAIIHB });
    }

    public static Task OnGetPlayerDetailInfo(PlayerSession session, GetPlayerDetailInfoCsReq request) =>
        session.SendAsync(new GetPlayerDetailInfoScRsp
        {
            Retcode = 0,
            DetailInfo = new PlayerDetailInfo
            {
                Uid = session.Player.Uid,
                Nickname = session.Player.Nickname,
                Signature = session.Player.Signature,
                Level = session.Player.Level,
                WorldLevel = session.Player.WorldLevel,
                HeadIcon = session.Player.HeadIcon,
                IsBanned = false,
            },
        });
}
