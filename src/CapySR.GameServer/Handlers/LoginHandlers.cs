using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

[Handlers]
public static class LoginHandlers
{
    public static Task OnGetToken(PlayerSession session, PlayerGetTokenCsReq request) =>
        session.SendAsync(new PlayerGetTokenScRsp
        {
            Retcode = 0,
            Uid = session.Player.Uid,
            SecretKeySeed = 0,
        });

    public static Task OnLogin(PlayerSession session, PlayerLoginCsReq request)
    {
        var player = session.Player;
        player.LoggedIn = true;
        player.Language = request.HOFEMNAIIHB;

        session.Logger.LogInformation("login: client language {Language}, res version {Version}",
            request.HOFEMNAIIHB, request.ClientResVersion);

        return session.SendAsync(new PlayerLoginScRsp
        {
            Retcode = 0,
            LoginRandom = request.LoginRandom,
            ServerTimestampMs = (ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
            Stamina = player.Stamina,
            BasicInfo = BasicInfo(session),
        });
    }

    public static PlayerBasicInfo BasicInfo(PlayerSession session)
    {
        var player = session.Player;

        return new PlayerBasicInfo
        {
            Nickname = player.Nickname,
            Level = player.Level,
            WorldLevel = player.WorldLevel,
            Stamina = player.Stamina,
            Mcoin = 999_999,
            Hcoin = 999_999,
            Scoin = 999_999,
        };
    }

    public static Task OnLoginFinish(PlayerSession session, PlayerLoginFinishCsReq request) =>
        session.SendAsync(new PlayerLoginFinishScRsp { Retcode = 0 });

    public static Task OnGetBasicInfo(PlayerSession session, GetBasicInfoCsReq request) =>
        session.SendAsync(new GetBasicInfoScRsp
        {
            Retcode = 0,
            IsGenderSet = true,
            Gender = (uint)session.Player.Gender,
            CurDay = 1,
        });

    public static Task OnHeartBeat(PlayerSession session, PlayerHeartBeatCsReq request) =>
        session.SendAsync(new PlayerHeartBeatScRsp
        {
            Retcode = 0,
            ClientTimeMs = request.ClientTimeMs,
            ServerTimeMs = (ulong)DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
        });
}
