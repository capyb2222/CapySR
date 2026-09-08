using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

// everything is finished. the client blocks on this during load: without a reply it
// never decides which scene it is supposed to be in and sits on a black screen.
[Handlers]
public static class MissionHandlers
{
    public static Task OnGetMissionStatus(PlayerSession session, GetMissionStatusCsReq request)
    {
        var response = new GetMissionStatusScRsp { Retcode = 0 };

        foreach (var id in request.SubMissionIdList)
        {
            response.SubMissionStatusList.Add(new Mission
            {
                Id = id,
                Status = MissionStatus.MissionFinish,
                Progress = 1,
            });
        }

        foreach (var id in session.World.Data.MainMissions.Keys)
        {
            response.FinishedMainMissionIdList.Add(id);
            response.CurversionFinishedMainMissionIdList.Add(id);
        }

        session.Logger.LogInformation(
            "missions: {Sub} sub finished, {Main} main finished",
            response.SubMissionStatusList.Count, response.FinishedMainMissionIdList.Count);

        return session.SendAsync(response);
    }

    public static Task OnGetMainMissionCustomValue(PlayerSession session, GetMainMissionCustomValueCsReq request) =>
        session.SendAsync(new GetMainMissionCustomValueScRsp { Retcode = 0 });

    // report every tutorial already finished. an empty list makes the client try to unlock
    // them one at a time, forever, and it never leaves the loading screen.
    public static Task OnGetTutorial(PlayerSession session, GetTutorialCsReq request)
    {
        var response = new GetTutorialScRsp { Retcode = 0 };

        foreach (var id in session.World.Data.TutorialIds)
        {
            response.TutorialList.Add(new Tutorial { Id = id, Status = TutorialStatus.TutorialFinish });
        }

        session.Logger.LogInformation("tutorials: {Count} finished", response.TutorialList.Count);
        return session.SendAsync(response);
    }

    public static Task OnGetTutorialGuide(PlayerSession session, GetTutorialGuideCsReq request)
    {
        var response = new GetTutorialGuideScRsp { Retcode = 0 };

        foreach (var id in session.World.Data.TutorialGuideIds)
        {
            response.TutorialGuideList.Add(new TutorialGuide { Id = id, Status = TutorialStatus.TutorialFinish });
        }

        session.Logger.LogInformation("tutorial guides: {Count} finished", response.TutorialGuideList.Count);
        return session.SendAsync(response);
    }
}
