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

        // same story as GetMainMissionCustomValue: the ids asked about need a row each
        foreach (var id in request.MainMissionIdList)
        {
            response.MainMissionMcvList.Add(new MainMissionCustomValue
            {
                MainMissionId = id,
                CustomValueList = new ONHKODAFEMH(),
            });
        }

        session.Logger.LogInformation(
            "missions: {Sub} sub finished, {Main} main finished",
            response.SubMissionStatusList.Count, response.FinishedMainMissionIdList.Count);

        return session.SendAsync(response);
    }

    // an entry has to come back for every id asked about. the client indexes the result by
    // mission id while it activates the map's hoyo groups, and a miss throws inside
    // AdventureModule.EnterMap - the map never finishes entering and the load screen stays up.
    public static Task OnGetMainMissionCustomValue(PlayerSession session, GetMainMissionCustomValueCsReq request)
    {
        var response = new GetMainMissionCustomValueScRsp { Retcode = 0 };

        foreach (var id in request.MainMissionIdList)
        {
            response.MainMissionList.Add(new MainMission
            {
                Id = id,
                Status = MissionStatus.MissionFinish,
            });
        }

        return session.SendAsync(response);
    }

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
