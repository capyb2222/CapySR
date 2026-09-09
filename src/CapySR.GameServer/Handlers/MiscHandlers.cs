using CapySR.GameServer.Net;
using CapySR.Protocol;
using Google.Protobuf;

namespace CapySR.GameServer.Handlers;

// small answers the client polls for on login. most are "nothing here" said properly.
[Handlers]
public static class MiscHandlers
{
    private const uint StandardBannerId = 1001;

    // client settings the game stores server side (auto battle, speed, ...)
    public static Task OnGetAllServerPrefsData(PlayerSession session, GetAllServerPrefsDataCsReq request)
    {
        var response = new GetAllServerPrefsDataScRsp { Retcode = 0 };

        foreach (var (id, data) in session.Player.ServerPrefs.OrderBy(kv => kv.Key))
        {
            response.ServerPrefsList.Add(new ServerPrefs { ServerPrefsId = id, Data = ByteString.CopyFrom(data) });
        }

        return session.SendAsync(response);
    }

    public static Task OnUpdateServerPrefs(PlayerSession session, UpdateServerPrefsCsReq request)
    {
        var prefs = request.ServerPrefs;

        if (prefs is not null)
        {
            session.Player.ServerPrefs[prefs.ServerPrefsId] = prefs.Data.ToByteArray();
            session.Player.Touch();
        }

        return session.SendAsync(new UpdateServerPrefsDataScRsp
        {
            Retcode = 0,
            ServerPrefsId = prefs?.ServerPrefsId ?? 0,
        });
    }

    public static Task OnGetMarkItemList(PlayerSession session, GetMarkItemListCsReq request) =>
        session.SendAsync(new GetMarkItemListScRsp { Retcode = 0 });

    public static Task OnGetCurAssist(PlayerSession session, GetCurAssistCsReq request) =>
        session.SendAsync(new GetCurAssistScRsp { Retcode = 0 });

    public static Task OnGetMissionData(PlayerSession session, GetMissionDataCsReq request)
    {
        var response = new GetMissionDataScRsp { Retcode = 0 };
        response.FinishedMainMissionIdList.AddRange(session.World.Data.MainMissions.Keys);
        return session.SendAsync(response);
    }

    public static Task OnGetQuestData(PlayerSession session, GetQuestDataCsReq request) =>
        session.SendAsync(new GetQuestDataScRsp { Retcode = 0 });

    public static Task OnGetActivityScheduleConfig(PlayerSession session, GetActivityScheduleConfigCsReq request) =>
        session.SendAsync(new GetActivityScheduleConfigScRsp { Retcode = 0 });

    public static Task OnGetSecretKeyInfo(PlayerSession session, GetSecretKeyInfoCsReq request) =>
        session.SendAsync(new GetSecretKeyInfoScRsp { Retcode = 0 });

    public static Task OnGetVideoVersionKey(PlayerSession session, GetVideoVersionKeyCsReq request) =>
        session.SendAsync(new GetVideoVersionKeyScRsp { Retcode = 0 });

    // an empty list leaves the gacha red-dot filter indexing nil every UI tick, which throws
    // out of UIManager.Tick every frame. hand it the standard banner and nothing else.
    public static Task OnGetGachaInfo(PlayerSession session, GetGachaInfoCsReq request)
    {
        var ceiling = new GachaCeiling { CeilingNum = 300, IsClaimed = false };

        foreach (var avatarId in session.World.Data.GachaCeilingAvatars)
        {
            ceiling.AvatarList.Add(new GachaCeilingAvatar { AvatarId = avatarId, RepeatedCnt = 300 });
        }

        var response = new GetGachaInfoScRsp { Retcode = 0, GachaRandom = 0 };

        response.GachaInfoList.Add(new GachaInfo
        {
            GachaId = StandardBannerId,
            GachaCeiling = ceiling,
            BeginTime = 0,
            EndTime = 2524608000,
        });

        return session.SendAsync(response);
    }

    public static Task OnGetDailyActiveInfo(PlayerSession session, GetDailyActiveInfoCsReq request) =>
        session.SendAsync(new GetDailyActiveInfoScRsp { Retcode = 0 });

    public static Task OnGetUnreleasedBlockInfo(PlayerSession session, GetUnreleasedBlockInfoCsReq request) =>
        session.SendAsync(new GetUnreleasedBlockInfoScRsp { Retcode = 0 });

    public static Task OnGetLevelRewardTakenList(PlayerSession session, GetLevelRewardTakenListCsReq request) =>
        session.SendAsync(new GetLevelRewardTakenListScRsp { Retcode = 0 });

    public static Task OnGetPreAvatarGrowthInfo(PlayerSession session, GetPreAvatarGrowthInfoCsReq request) =>
        session.SendAsync(new GetPreAvatarGrowthInfoScRsp { Retcode = 0 });

    public static Task OnGetBigDataAllRecommend(PlayerSession session, GetBigDataAllRecommendCsReq request) =>
        session.SendAsync(new GetBigDataAllRecommendScRsp { Retcode = 0 });

    public static Task OnTextJoinQuery(PlayerSession session, TextJoinQueryCsReq request) =>
        session.SendAsync(new TextJoinQueryScRsp { Retcode = 0 });

    public static Task OnUpdateTrackMainMission(PlayerSession session, UpdateTrackMainMissionCsReq request) =>
        session.SendAsync(new UpdateTrackMainMissionScRsp
        {
            Retcode = 0,
            TrackMissionId = request.TrackMissionId,
        });
}
