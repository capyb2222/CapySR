using CapySR.GameServer.Net;
using CapySR.Protocol;

namespace CapySR.GameServer.Handlers;

[Handlers]
public static class BattleHandlers
{
    public static Task OnStartCocoonStage(PlayerSession session, StartCocoonStageCsReq request)
    {
        var battle = session.Battles.StartCocoon(request.CocoonId, request.Wave, request.WorldLevel);

        return session.SendAsync(new StartCocoonStageScRsp
        {
            Retcode = battle is null ? (uint)Retcode.RetStageConfigNotExist : 0,
            CocoonId = request.CocoonId,
            PropEntityId = request.PropEntityId,
            Wave = battle?.Wave ?? request.Wave,
            BattleInfo = battle?.Info,
        });
    }

    public static Task OnQuickStartCocoonStage(PlayerSession session, QuickStartCocoonStageCsReq request)
    {
        var battle = session.Battles.StartCocoon(request.CocoonId, request.Wave, request.WorldLevel);

        if (battle is not null && request.WorldLevel > 0)
        {
            battle.Info.WorldLevel = request.WorldLevel;
        }

        return session.SendAsync(new QuickStartCocoonStageScRsp
        {
            Retcode = battle is null ? (uint)Retcode.RetStageConfigNotExist : 0,
            CocoonId = request.CocoonId,
            Wave = battle?.Wave ?? request.Wave,
            BattleInfo = battle?.Info,
        });
    }

    public static Task OnQuickStartFarmElement(PlayerSession session, QuickStartFarmElementCsReq request)
    {
        var battle = session.Battles.StartFarmElement(request.PAOFHFLFFHD, request.WorldLevel);

        return session.SendAsync(new QuickStartFarmElementScRsp
        {
            Retcode = battle is null ? (uint)Retcode.RetStageConfigNotExist : 0,
            PAOFHFLFFHD = request.PAOFHFLFFHD,
            WorldLevel = request.WorldLevel,
            BattleInfo = battle?.Info,
        });
    }

    public static Task OnStartBattleCollege(PlayerSession session, StartBattleCollegeCsReq request)
    {
        var battle = session.Battles.StartCollege(request.Id);

        return session.SendAsync(new StartBattleCollegeScRsp
        {
            Retcode = battle is null ? (uint)Retcode.RetStageConfigNotExist : 0,
            Id = request.Id,
            BattleInfo = battle?.Info,
        });
    }

    public static Task OnGetBattleCollegeData(PlayerSession session, GetBattleCollegeDataCsReq request)
    {
        var response = new GetBattleCollegeDataScRsp { Retcode = 0 };
        response.FinishedCollegeIdList.AddRange(session.Player.FinishedColleges.Order());
        return session.SendAsync(response);
    }

    public static Task OnSceneEnterStage(PlayerSession session, SceneEnterStageCsReq request)
    {
        var battle = session.Battles.StartEvent(request.EventId);

        return session.SendAsync(new SceneEnterStageScRsp
        {
            Retcode = battle is null ? (uint)Retcode.RetStageConfigNotExist : 0,
            BattleInfo = battle?.Info,
        });
    }

    public static async Task OnSceneCastSkill(PlayerSession session, SceneCastSkillCsReq request)
    {
        var outcome = session.Battles.HandleCast(request);

        if (outcome.RemovedEntities.Count > 0)
        {
            await session.RemoveEntitiesAsync(outcome.RemovedEntities);
        }

        // a smashed orb pays out two points or a chunk of health for the whole team
        if (outcome.MpOrbs > 0)
        {
            session.Player.Lineups.GainMp((uint)(2 * outcome.MpOrbs));
            session.Player.Touch();
            await session.SyncLineupAsync(SyncLineupReason.SyncReasonMpAddPropHit);
        }

        if (outcome.HpOrbs > 0)
        {
            session.HealParty((uint)(2000 * outcome.HpOrbs));
            await session.SyncLineupAsync(SyncLineupReason.SyncReasonHpAddPropHit);
        }

        var response = new SceneCastSkillScRsp
        {
            Retcode = 0,
            CastEntityId = request.CastEntityId,
            BattleInfo = outcome.Battle,
        };

        response.MonsterBattleInfo.AddRange(outcome.Hits);
        await session.SendAsync(response);
    }

    // a technique costs one point; the client shows whatever we say is left
    public static async Task OnSceneCastSkillCostMp(PlayerSession session, SceneCastSkillCostMpCsReq request)
    {
        var book = session.Player.Lineups;
        book.SpendMp();
        session.Player.Touch();

        await session.SendAsync(new SceneCastSkillMpUpdateScNotify
        {
            CastEntityId = request.CastEntityId,
            Mp = book.Mp,
        });

        await session.SendAsync(new SceneCastSkillCostMpScRsp
        {
            Retcode = 0,
            CastEntityId = request.CastEntityId,
        });
    }

    public static async Task OnPveBattleResult(PlayerSession session, PVEBattleResultCsReq request)
    {
        var response = await session.Battles.FinishAsync(request);
        await session.SendAsync(response);
    }

    public static Task OnQuitBattle(PlayerSession session, QuitBattleCsReq request)
    {
        session.Battles.Quit();
        return session.SendAsync(new QuitBattleScRsp { Retcode = 0 });
    }

    public static Task OnGetCurBattleInfo(PlayerSession session, GetCurBattleInfoCsReq request) =>
        session.SendAsync(new GetCurBattleInfoScRsp
        {
            Retcode = 0,
            BattleInfo = session.Battles.Current?.Info ?? new SceneBattleInfo(),
            LastEndStatus = session.Battles.LastEndStatus,
        });

    public static Task OnSyncClientResVersion(PlayerSession session, SyncClientResVersionCsReq request) =>
        session.SendAsync(new SyncClientResVersionScRsp
        {
            Retcode = 0,
            ClientResVersion = request.ClientResVersion,
        });

    // the client sends this whenever it walks out of range of a farm prop
    public static Task OnDeactivateFarmElement(PlayerSession session, DeactivateFarmElementCsReq request) =>
        session.SendAsync(new DeactivateFarmElementScRsp { Retcode = 0, EntityId = request.EntityId });

    public static Task OnGetFarmStageGachaInfo(PlayerSession session, GetFarmStageGachaInfoCsReq request)
    {
        var response = new GetFarmStageGachaInfoScRsp { Retcode = 0 };
        var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();

        foreach (var gachaId in request.FarmStageGachaIdList)
        {
            response.FarmStageGachaInfoList.Add(new FarmStageGachaInfo
            {
                GachaId = gachaId,
                BeginTime = 0,
                EndTime = now + 3600,
            });
        }

        return session.SendAsync(response);
    }
}
