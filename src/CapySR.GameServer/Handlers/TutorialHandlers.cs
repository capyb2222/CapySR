using CapySR.GameServer.Net;
using CapySR.Protocol;

namespace CapySR.GameServer.Handlers;

// the client retries these until answered, so an unhandled one stalls the load forever
[Handlers]
public static class TutorialHandlers
{
    public static Task OnUnlockTutorial(PlayerSession session, UnlockTutorialCsReq request) =>
        session.SendAsync(new UnlockTutorialScRsp { Retcode = 0 });

    public static Task OnFinishTutorial(PlayerSession session, FinishTutorialCsReq request) =>
        session.SendAsync(new FinishTutorialScRsp { Retcode = 0 });

    public static Task OnUnlockTutorialGuide(PlayerSession session, UnlockTutorialGuideCsReq request) =>
        session.SendAsync(new UnlockTutorialGuideScRsp { Retcode = 0 });

    public static Task OnFinishTutorialGuide(PlayerSession session, FinishTutorialGuideCsReq request) =>
        session.SendAsync(new FinishTutorialGuideScRsp { Retcode = 0 });

    public static Task OnSetClientPaused(PlayerSession session, SetClientPausedCsReq request) =>
        session.SendAsync(new SetClientPausedScRsp { Retcode = 0, Paused = request.Paused });

    public static Task OnGetArchiveData(PlayerSession session, GetArchiveDataCsReq request) =>
        session.SendAsync(new GetArchiveDataScRsp { Retcode = 0, ArchiveData = new ArchiveData() });

    public static Task OnGetFirstTalkNpc(PlayerSession session, GetFirstTalkNpcCsReq request)
    {
        var response = new GetFirstTalkNpcScRsp { Retcode = 0 };

        foreach (var npcId in request.NpcIdList)
        {
            response.NpcMeetStatusList.Add(new FirstNpcTalkInfo { NpcId = npcId, IsMeet = true });
        }

        return session.SendAsync(response);
    }

    public static Task OnGetMail(PlayerSession session, GetMailCsReq request) =>
        session.SendAsync(new GetMailScRsp { Retcode = 0, IsEnd = true, TotalNum = 0, Start = 0 });

    public static Task OnGetShopList(PlayerSession session, GetShopListCsReq request) =>
        session.SendAsync(new GetShopListScRsp { Retcode = 0, ShopType = request.ShopType });
}
