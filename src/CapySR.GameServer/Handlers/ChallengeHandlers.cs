using CapySR.Data;
using CapySR.Data.Excel;
using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

[Handlers]
public static class ChallengeHandlers
{
    // MOC floor N is locked until floor N-1 is cleared, and PF/AS want a score on the floor
    // before. reporting a full clear for anything unplayed keeps every floor reachable.
    private const uint AllStars = 7;

    public static Task OnGetChallenge(PlayerSession session, GetChallengeCsReq request)
    {
        var data = session.World.Data;
        var records = session.Player.ChallengeRecords;
        var unlockAll = session.Config.GameServer.UnlockAllChallenges;
        var response = new GetChallengeScRsp { Retcode = 0 };

        foreach (var challenge in data.Challenges.Values.OrderBy(c => c.ID))
        {
            var record = records.GetValueOrDefault(challenge.ID);
            var clearScore = unlockAll ? ClearScore(data, challenge) : 0;

            response.ChallengeList.Add(new Challenge
            {
                ChallengeId = challenge.ID,
                Star = record?.Stars ?? (unlockAll ? AllStars : 0),
                ScoreId = record?.Score ?? clearScore,
                ScoreTwo = record?.ScoreTwo ?? clearScore,
                TakenReward = 0,
            });
        }

        foreach (var groupId in data.ChallengeGroups.Keys.Order())
        {
            response.ChallengeGroupList.Add(new ChallengeGroup { GroupId = groupId, TakenStarsCountReward = 0 });
        }

        response.MaxLevelList.Add(new ChallengeHistoryMaxLevel { RewardDisplayType = 1, Level = 12 });
        response.MaxLevelList.Add(new ChallengeHistoryMaxLevel { RewardDisplayType = 2, Level = 4 });
        response.MaxLevelList.Add(new ChallengeHistoryMaxLevel { RewardDisplayType = 3, Level = 4 });

        return session.SendAsync(response);
    }

    // the score a PF or AS floor counts as fully cleared, straight out of its target rows
    private static uint ClearScore(GameData data, ChallengeMazeExcel challenge)
    {
        if (challenge.Kind == ChallengeKind.MemoryOfChaos)
        {
            return 0;
        }

        return challenge.ChallengeTargetID
            .Select(id => data.ChallengeTargets.GetValueOrDefault(id))
            .Where(t => t?.ChallengeTargetType == "TOTAL_SCORE")
            .Select(t => t!.ChallengeTargetParam1)
            .DefaultIfEmpty(0u)
            .Max();
    }

    public static Task OnGetCurChallenge(PlayerSession session, GetCurChallengeCsReq request)
    {
        var state = session.Challenge;
        var response = new GetCurChallengeScRsp { Retcode = 0 };

        if (state.Active && !state.IsPeak)
        {
            response.CurChallenge = ChallengeFlow.BuildCurChallenge(session);
            response.LineupList.Add(session.BuildChallengeLineup(0));

            if (state.NodeCount > 1)
            {
                response.LineupList.Add(session.BuildChallengeLineup(1));
            }
        }

        return session.SendAsync(response);
    }

    public static Task OnGetChallengeGroupStatistics(PlayerSession session, GetChallengeGroupStatisticsCsReq request) =>
        session.SendAsync(new GetChallengeGroupStatisticsScRsp { Retcode = 0, GroupId = request.GroupId });

    public static Task OnTakeChallengeReward(PlayerSession session, TakeChallengeRewardCsReq request) =>
        session.SendAsync(new TakeChallengeRewardScRsp { Retcode = 0, GroupId = request.GroupId });

    public static async Task OnStartChallenge(PlayerSession session, StartChallengeCsReq request)
    {
        var world = session.World;
        var challenge = world.Data.GetChallenge(request.ChallengeId);

        if (challenge is null)
        {
            session.Logger.LogWarning("unknown challenge {Id}", request.ChallengeId);
            await session.SendAsync(new StartChallengeScRsp { Retcode = (uint)Retcode.RetChallengeNotExist });
            return;
        }

        var first = Lineup(request.AvatarLineupFirst, request.FirstLineup);
        var second = Lineup(request.AvatarLineupSecond, request.SecondLineup);

        if (first.Count == 0)
        {
            first = second.Count > 0 ? second : session.DefaultLineup();
        }

        // a two node floor wants a team per half; run the first one twice rather than refuse
        if (challenge.NodeCount > 1 && second.Count == 0)
        {
            second = first;
        }

        var (buffOne, buffTwo) = challenge.Kind switch
        {
            ChallengeKind.PureFiction => (request.StageInfo?.StoryInfo?.BuffOne ?? 0u, request.StageInfo?.StoryInfo?.BuffTwo ?? 0u),
            ChallengeKind.ApocalypticShadow => (request.StageInfo?.BossInfo?.BuffOne ?? 0u, request.StageInfo?.BossInfo?.BuffTwo ?? 0u),
            _ => (0u, 0u),
        };

        var state = session.Challenge;

        if (!state.Start(world.Data, challenge, first, second, buffOne, buffTwo))
        {
            session.Logger.LogWarning("challenge {Id} has no usable scene", challenge.ID);
            await session.SendAsync(new StartChallengeScRsp { Retcode = (uint)Retcode.RetChallengeNotExist });
            return;
        }

        session.Battles.Quit();

        var book = session.Player.Lineups;
        book.ClearExtra();
        book.SetExtra(ExtraLineupType.LineupChallenge, state.Lineups[0], activate: false);

        if (state.NodeCount > 1)
        {
            book.SetExtra(ExtraLineupType.LineupChallenge2, state.Lineups[1], activate: false);
        }

        await ChallengeFlow.ActivateNodeAsync(session);

        session.Logger.LogInformation(
            "challenge {Id} ({Kind}) started: {Nodes} node(s), stage {Stage}, floor {Floor}, monster {Monster}, buffs [{Buffs}]",
            state.ChallengeId, state.Kind, state.NodeCount, state.StageId, state.FloorId, state.MonsterId,
            string.Join(", ", state.Blessings));

        var scene = world.Scenes.BuildChallengeScene(state);
        session.Scene.Load(scene, isChallenge: true);

        var response = new StartChallengeScRsp
        {
            Retcode = 0,
            CurChallenge = ChallengeFlow.BuildCurChallenge(session),
            Scene = scene,
            StageInfo = ChallengeFlow.BuildStageInfo(session),
        };

        response.LineupList.Add(session.BuildChallengeLineup(0));

        if (state.NodeCount > 1)
        {
            response.LineupList.Add(session.BuildChallengeLineup(1));
        }

        await session.SendAsync(response);
        await session.NotifySpawnPositionAsync(scene);
        await session.SyncLineupAsync();
    }

    public static async Task OnLeaveChallenge(PlayerSession session, LeaveChallengeCsReq request)
    {
        await session.SendAsync(new LeaveChallengeScRsp { Retcode = 0 });
        await LeaveAsync(session);
    }

    // AS: the second boss half, requested once the phase settle screen is dismissed
    public static async Task OnEnterChallengeNextPhase(PlayerSession session, EnterChallengeNextPhaseCsReq request)
    {
        var state = session.Challenge;

        if (!state.Active || state.IsPeak || state.IsLastNode)
        {
            await session.SendAsync(new EnterChallengeNextPhaseScRsp { Retcode = (uint)Retcode.RetChallengeNotDoing });
            return;
        }

        await ChallengeFlow.AdvanceNodeAsync(session);
        await session.SendAsync(new EnterChallengeNextPhaseScRsp { Retcode = 0, Scene = session.CurrentScene() });
    }

    public static async Task OnRestartChallengePhase(PlayerSession session, RestartChallengePhaseCsReq request)
    {
        if (!session.Challenge.Active || session.Challenge.IsPeak)
        {
            await session.SendAsync(new RestartChallengePhaseScRsp { Retcode = (uint)Retcode.RetChallengeNotDoing });
            return;
        }

        session.Battles.Quit();
        await ChallengeFlow.RestartNodeAsync(session);
        await session.SendAsync(new RestartChallengePhaseScRsp { Retcode = 0, Scene = session.CurrentScene() });
    }

    // ---- Anomaly Arbitration (Challenge Peak) ----

    public static Task OnGetChallengePeakData(PlayerSession session, GetChallengePeakDataCsReq request)
    {
        var data = session.World.Data;
        var response = new GetChallengePeakDataScRsp { Retcode = 0 };

        foreach (var group in data.ChallengePeakGroups.Values.OrderBy(g => g.ID))
        {
            response.ChallengePeakGroups.Add(ChallengeFlow.BuildPeakGroup(session, group));
            response.CurrentPeakGroupId = group.ID;
        }

        return session.SendAsync(response);
    }

    public static Task OnGetCurChallengePeak(PlayerSession session, GetCurChallengePeakCsReq request)
    {
        var state = session.Challenge;
        var response = new GetCurChallengePeakScRsp { Retcode = 0 };

        if (state.Active && state.IsPeak)
        {
            response.PeakId = state.ChallengeId;
            response.BossBuffId = state.SelectedBuffs[0];
            response.HasPassed = state.Status == ChallengeStatus.ChallengeFinish;
        }

        return session.SendAsync(response);
    }

    public static Task OnSetChallengePeakBossHardMode(PlayerSession session, SetChallengePeakBossHardModeCsReq request)
    {
        session.Challenge.HardMode = request.IsHardMode;

        return session.SendAsync(new SetChallengePeakBossHardModeScRsp
        {
            Retcode = 0,
            PeakGroupId = request.PeakGroupId,
            IsHardMode = request.IsHardMode,
        });
    }

    public static async Task OnSetChallengePeakMobLineupAvatar(PlayerSession session, SetChallengePeakMobLineupAvatarCsReq request)
    {
        foreach (var lineup in request.LineupList)
        {
            session.Player.PeakLineups[lineup.PeakId] = [.. lineup.PeakAvatarIdList.Where(session.OwnsBase)];
        }

        session.Player.Touch();

        if (session.World.Data.ChallengePeakGroups.TryGetValue(request.PeakGroupId, out var group))
        {
            await session.SendAsync(new ChallengePeakGroupDataUpdateScNotify
            {
                ChallengePeakGroup = ChallengeFlow.BuildPeakGroup(session, group),
            });
        }

        await session.SendAsync(new SetChallengePeakMobLineupAvatarScRsp { Retcode = 0 });
    }

    public static async Task OnStartChallengePeak(PlayerSession session, StartChallengePeakCsReq request)
    {
        var world = session.World;

        if (!world.Data.ChallengePeaks.TryGetValue(request.PeakId, out var peak))
        {
            session.Logger.LogWarning("unknown AA stage {Id}", request.PeakId);
            await session.SendAsync(new StartChallengePeakScRsp { Retcode = (uint)Retcode.RetChallengeNotExist });
            return;
        }

        world.Data.ChallengePeakBosses.TryGetValue(peak.ID, out var boss);

        var lineup = request.PeakAvatarIdList.Where(session.OwnsBase).ToList();

        if (lineup.Count == 0)
        {
            lineup = session.Player.PeakLineups.GetValueOrDefault(peak.ID)?.Where(session.OwnsBase).ToList() ?? [];
        }

        if (lineup.Count == 0)
        {
            lineup = session.DefaultLineup();
        }

        if (!session.Challenge.StartPeak(world.Data, peak, boss, request.BossBuffId, lineup))
        {
            session.Logger.LogWarning("AA stage {Id} has no usable scene", peak.ID);
            await session.SendAsync(new StartChallengePeakScRsp { Retcode = (uint)Retcode.RetChallengeNotExist });
            return;
        }

        session.Battles.Quit();

        var state = session.Challenge;
        var book = session.Player.Lineups;
        book.ClearExtra();
        book.SetExtra(ExtraLineupType.LineupChallenge, state.Lineups[0], activate: false);
        await ChallengeFlow.ActivateNodeAsync(session);

        session.Logger.LogInformation(
            "AA stage {Id}{Hard}: stage {Stage}, floor {Floor}, monster {Monster}, tags [{Tags}]",
            state.ChallengeId, state.HardMode && boss is not null ? " (hard)" : string.Empty, state.StageId, state.FloorId,
            state.MonsterId, string.Join(", ", state.Blessings));

        // AA delivers the scene out of band; the response itself is only a retcode
        await session.EnterChallengeSceneAsync();
        await session.SendAsync(new StartChallengePeakScRsp { Retcode = 0 });
        await session.SyncLineupAsync();
    }

    public static async Task OnReStartChallengePeak(PlayerSession session, ReStartChallengePeakCsReq request)
    {
        var state = session.Challenge;

        if (!state.Active || !state.IsPeak || state.Peak is null)
        {
            await session.SendAsync(new ReStartChallengePeakScRsp { Retcode = (uint)Retcode.RetChallengeNotDoing });
            return;
        }

        session.Battles.Quit();
        state.StartPeak(session.World.Data, state.Peak, state.Boss, state.SelectedBuffs[0], state.Lineup.ToList());
        await ChallengeFlow.ActivateNodeAsync(session);
        await session.EnterChallengeSceneAsync();
        await session.SendAsync(new ReStartChallengePeakScRsp { Retcode = 0 });
        await session.SyncLineupAsync();
    }

    public static async Task OnLeaveChallengePeak(PlayerSession session, LeaveChallengePeakCsReq request)
    {
        await session.SendAsync(new LeaveChallengePeakScRsp { Retcode = 0 });
        await LeaveAsync(session);
    }

    public static Task OnConfirmChallengePeakSettle(PlayerSession session, ConfirmChallengePeakSettleCsReq request) =>
        session.SendAsync(new ConfirmChallengePeakSettleScRsp
        {
            Retcode = 0,
            PeakId = request.PeakId,
            IOBCIMIPEOM = request.IOBCIMIPEOM,
        });

    public static Task OnTakeChallengePeakReward(PlayerSession session, TakeChallengePeakRewardCsReq request) =>
        session.SendAsync(new TakeChallengePeakRewardScRsp { Retcode = 0, PeakGroupId = request.PeakGroupId });

    // back to the overworld the player left from
    private static Task LeaveAsync(PlayerSession session)
    {
        if (!session.Challenge.Active && !session.Scene.IsChallenge)
        {
            return Task.CompletedTask;
        }

        var player = session.Player;
        return session.EnterWorldSceneAsync(player.EntryId, player.TeleportId);
    }

    private static List<uint> Lineup(IEnumerable<AvatarIdentifier> identifiers, IEnumerable<uint> plain)
    {
        var ids = identifiers.Select(a => a.Id).Where(id => id != 0).Distinct().ToList();
        return ids.Count > 0 ? ids : [.. plain.Where(id => id != 0).Distinct()];
    }
}
