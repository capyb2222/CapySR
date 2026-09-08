using CapySR.Data.Excel;
using CapySR.GameServer.Battle;
using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

// what happens between the fights of a floor: node changes, scores, stars, settle screens
public static class ChallengeFlow
{
    public static async Task OnBattleEndAsync(PlayerSession session, BattleInstance battle, PVEBattleResultCsReq request)
    {
        var state = session.Challenge;

        if (state.IsPeak)
        {
            await OnPeakBattleEndAsync(session, request);
            return;
        }

        RecordScore(state, request.Stt);

        switch (request.EndStatus)
        {
            case BattleEndStatus.BattleEndWin:
                if (state.Kind == ChallengeKind.MemoryOfChaos)
                {
                    state.DeadAvatars += (uint)state.Lineup.Count(id => session.Player.StateOf(
                        Roster.ResolvePath(session.World, session.Player, id), session.World.Player).Hp == 0);

                    var used = request.Stt?.RoundCnt ?? 0;
                    state.RoundsLeft = Math.Max(1, state.RoundsLeft - Math.Min(used, state.RoundsLeft));
                }

                state.SavedMp = session.Player.Lineups.Mp;

                if (!session.Scene.Monsters.Any())
                {
                    await OnNodeClearedAsync(session, request);
                }

                break;

            case BattleEndStatus.BattleEndQuit:
                session.Player.Lineups.Mp = state.SavedMp;
                await ReturnToStartAsync(session);
                break;

            default:
                // PF and AS end on the turn limit by design; that is a finished half, not a loss
                if (state.Kind != ChallengeKind.MemoryOfChaos && request.Stt?.EndReason == BattleEndReason.TurnLimit)
                {
                    await OnNodeClearedAsync(session, request);
                }
                else
                {
                    state.Fail();
                    await SendSettleAsync(session, isWin: false);
                }

                break;
        }
    }

    private static void RecordScore(ChallengeState state, BattleStatistics? stats)
    {
        if (stats is null)
        {
            return;
        }

        switch (state.Kind)
        {
            case ChallengeKind.PureFiction:
                // the client reports the running total; keep this half's share
                var previous = state.TotalScore - state.Scores[state.Node];
                state.Scores[state.Node] = stats.ChallengeScore >= previous ? stats.ChallengeScore - previous : stats.ChallengeScore;
                break;

            case ChallengeKind.ApocalypticShadow:
                if (stats.BattleTargetInfo.TryGetValue(1, out var gauge))
                {
                    state.Scores[state.Node] = (uint)gauge.BattleTargetList_.Sum(t => (long)t.Progress);
                }

                break;
        }
    }

    private static async Task OnNodeClearedAsync(PlayerSession session, PVEBattleResultCsReq request)
    {
        var state = session.Challenge;

        if (state.Kind == ChallengeKind.ApocalypticShadow)
        {
            await SendBossPhaseSettleAsync(session, request);

            if (!state.IsLastNode)
            {
                // the client asks for the next phase itself
                return;
            }
        }

        if (!state.IsLastNode)
        {
            await AdvanceNodeAsync(session);
            return;
        }

        await FinishAsync(session);
    }

    public static async Task AdvanceNodeAsync(PlayerSession session)
    {
        var state = session.Challenge;

        if (!state.SwitchNode(session.World.Data, state.Node + 1))
        {
            session.Logger.LogWarning("challenge {Id} has no second node; settling", state.ChallengeId);
            await FinishAsync(session);
            return;
        }

        await ActivateNodeAsync(session);
        await session.SendAsync(new ChallengeLineupNotify { ExtraLineupType = state.LineupType });
        await session.EnterChallengeSceneAsync();
        await session.SyncLineupAsync();

        session.Logger.LogInformation("challenge {Id}: node {Node} at stage {Stage}", state.ChallengeId, state.Node + 1, state.StageId);
    }

    // full health, half energy, a full technique bar: the way every node opens
    public static Task ActivateNodeAsync(PlayerSession session)
    {
        var state = session.Challenge;
        var book = session.Player.Lineups;

        var lineup = book.Extra(state.LineupType) ?? book.SetExtra(state.LineupType, state.Lineup, activate: false);

        if (lineup.IsEmpty)
        {
            lineup.Fill(state.Lineup);
        }

        book.ActivateExtra(state.LineupType);
        book.RefillMp();
        state.SavedMp = book.Mp;

        session.Player.PrepareForChallenge(
            lineup.AvatarIds.Select(id => Roster.ResolvePath(session.World, session.Player, id)), session.World.Player);

        return Task.CompletedTask;
    }

    private static async Task FinishAsync(PlayerSession session)
    {
        var state = session.Challenge;
        var stars = state.CalculateStars(session.World.Data);
        state.Finish(stars);

        var records = session.Player.ChallengeRecords;
        var record = records.GetValueOrDefault(state.ChallengeId) ?? new ChallengeRecord();

        if (stars >= record.Stars)
        {
            record.Stars = stars;
            record.Score = Math.Max(record.Score, state.Scores[0]);
            record.ScoreTwo = Math.Max(record.ScoreTwo, state.Scores[1]);
        }

        records[state.ChallengeId] = record;
        session.Player.Touch();

        session.Logger.LogInformation("challenge {Id} cleared: {Stars} stars, score {Score}/{ScoreTwo}",
            state.ChallengeId, stars, state.Scores[0], state.Scores[1]);

        await SendSettleAsync(session, isWin: true);
    }

    private static Task SendSettleAsync(PlayerSession session, bool isWin)
    {
        var state = session.Challenge;

        return session.SendAsync(new ChallengeSettleNotify
        {
            ChallengeId = state.ChallengeId,
            IsWin = isWin,
            Star = state.Stars,
            ChallengeScore = state.Scores[0],
            ScoreTwo = state.Scores[1],
            Reward = new ItemList(),
        });
    }

    private static Task SendBossPhaseSettleAsync(PlayerSession session, PVEBattleResultCsReq request)
    {
        var state = session.Challenge;

        var notify = new ChallengeBossPhaseSettleNotify
        {
            ChallengeId = state.ChallengeId,
            IsWin = true,
            IsSecondHalf = state.Node == 1,
            Phase = (uint)state.Node + 1,
            ChallengeScore = state.Scores[0],
            ScoreTwo = state.Scores[1],
            Star = state.Stars,
            IsReward = true,
            PageType = 1,
        };

        if (request.Stt?.BattleTargetInfo.TryGetValue(1, out var targets) == true)
        {
            notify.BattleTargetList.AddRange(targets.BattleTargetList_);
        }

        return session.SendAsync(notify);
    }

    // the retry button: same half, fresh party
    public static async Task RestartNodeAsync(PlayerSession session)
    {
        var state = session.Challenge;
        state.SwitchNode(session.World.Data, state.Node);

        await ActivateNodeAsync(session);
        await session.EnterChallengeSceneAsync();
        await session.SyncLineupAsync();
    }

    private static async Task ReturnToStartAsync(PlayerSession session)
    {
        var motion = session.World.Scenes.AnchorMotion(session.Challenge.EntryId);

        if (motion is not null)
        {
            await session.MoveActorsAsync(motion);
        }
    }

    public static CurChallenge BuildCurChallenge(PlayerSession session)
    {
        var state = session.Challenge;

        var current = new CurChallenge
        {
            ChallengeId = state.ChallengeId,
            Status = state.Status == ChallengeStatus.ChallengeUnknown ? ChallengeStatus.ChallengeDoing : state.Status,
            RoundCount = state.Kind == ChallengeKind.MemoryOfChaos ? state.CycleCount - state.RoundsLeft : state.CycleCount,
            ScoreId = state.Scores[0],
            ScoreTwo = state.Scores[1],
            DeadAvatarNum = state.DeadAvatars,
            ExtraLineupType = state.LineupType,
        };

        // PF and AS carry their active blessings back to the client
        switch (state.Kind)
        {
            case ChallengeKind.PureFiction:
                current.StageInfo = new ChallengeCurBuffInfo
                {
                    CurStoryBuffs = new ChallengeStoryBuffList { BuffList = { state.SelectedBuffs.Where(id => id != 0) } },
                };
                break;

            case ChallengeKind.ApocalypticShadow:
                current.StageInfo = new ChallengeCurBuffInfo
                {
                    CurBossBuffs = new ChallengeBossBuffList
                    {
                        BuffList = { state.SelectedBuffs.Where(id => id != 0) },
                        ChallengeBossConst = 1,
                    },
                };
                break;

            default:
                current.StageInfo = new ChallengeCurBuffInfo();
                break;
        }

        return current;
    }

    public static ChallengeStageInfo? BuildStageInfo(PlayerSession session)
    {
        var state = session.Challenge;

        if (state.Kind != ChallengeKind.ApocalypticShadow)
        {
            return null;
        }

        var info = new ChallengeStageInfo
        {
            BossInfo = new ChallengeBossInfo
            {
                FirstNode = new ChallengeBossSingleNodeInfo { BuffId = state.SelectedBuffs[0], IsWin = state.Node > 0 },
                SecondNode = new ChallengeBossSingleNodeInfo { BuffId = state.SelectedBuffs[1] },
                Unk1 = true,
            },
        };

        info.BossInfo.AvatarLineupFirst.AddRange(state.Lineups[0].Select(id => new AvatarIdentifier { Id = id, AvatarType = AvatarType.AvatarFormalType }));
        info.BossInfo.AvatarLineupSecond.AddRange(state.Lineups[1].Select(id => new AvatarIdentifier { Id = id, AvatarType = AvatarType.AvatarFormalType }));

        return info;
    }

    // ---- Anomaly Arbitration ----

    private static async Task OnPeakBattleEndAsync(PlayerSession session, PVEBattleResultCsReq request)
    {
        var state = session.Challenge;

        switch (request.EndStatus)
        {
            case BattleEndStatus.BattleEndWin:
                state.SavedMp = session.Player.Lineups.Mp;

                if (session.Scene.Monsters.Any())
                {
                    return;
                }

                var finished = FinishedPeakTargets(session, request.Stt);
                var stars = state.HardMode && state.Boss is not null ? 3u : (uint)Math.Min(3, finished.Count);
                state.Finish(stars);

                session.Player.SetPeakRecord(state.ChallengeId, state.HardMode && state.Boss is not null, new PeakRecord
                {
                    Stars = stars,
                    Cycles = request.Stt?.RoundCnt ?? 0,
                    BuffId = state.SelectedBuffs[0],
                    FinishedTargets = finished,
                    Avatars = [.. state.Lineup],
                });

                await session.SendAsync(new ChallengePeakSettleScNotify
                {
                    PeakId = state.ChallengeId,
                    IsWin = true,
                    CyclesUsed = request.Stt?.RoundCnt ?? 0,
                    FinishedTargetList = { finished },
                    HardModeHasPassed = state.HardMode && state.Boss is not null,
                    IsFirstPass = true,
                });

                await SendPeakGroupUpdateAsync(session, state.ChallengeId);
                break;

            case BattleEndStatus.BattleEndQuit:
                session.Player.Lineups.Mp = state.SavedMp;
                await ReturnToStartAsync(session);
                break;

            default:
                state.Fail();

                await session.SendAsync(new ChallengePeakSettleScNotify
                {
                    PeakId = state.ChallengeId,
                    IsWin = false,
                    CyclesUsed = request.Stt?.RoundCnt ?? 0,
                });

                break;
        }
    }

    // a target is met when its counter stayed within the configured parameter
    private static List<uint> FinishedPeakTargets(PlayerSession session, BattleStatistics? stats)
    {
        var state = session.Challenge;
        var finished = new List<uint>();

        if (stats is null || !stats.BattleTargetInfo.TryGetValue(5, out var reported))
        {
            return state.PeakTargets.Count == 0 ? [] : finished;
        }

        foreach (var targetId in state.PeakTargets)
        {
            var target = reported.BattleTargetList_.FirstOrDefault(t => t.Id == targetId);
            var param = session.World.Data.BattleTargets.GetValueOrDefault(targetId)?.TargetParam ?? 0;

            if (target is not null && target.Progress <= param)
            {
                finished.Add(targetId);
            }
        }

        return finished;
    }

    public static Task SendPeakGroupUpdateAsync(PlayerSession session, uint peakId)
    {
        var group = session.World.Data.ChallengePeakGroups.Values
            .FirstOrDefault(g => g.BossLevelID == peakId || g.PreLevelIDList.Contains(peakId));

        return group is null
            ? Task.CompletedTask
            : session.SendAsync(new ChallengePeakGroupDataUpdateScNotify { ChallengePeakGroup = BuildPeakGroup(session, group) });
    }

    public static ChallengePeakGroup BuildPeakGroup(PlayerSession session, ChallengePeakGroupExcel group)
    {
        var data = session.World.Data;
        var player = session.Player;

        var proto = new ChallengePeakGroup
        {
            PeakGroupId = group.ID,
            DisableHardMode = false,
        };

        var stars = 0u;

        foreach (var peakId in group.PreLevelIDList.Where(data.ChallengePeaks.ContainsKey))
        {
            var record = player.PeakRecord(peakId, hard: false);

            var peak = new ChallengePeak
            {
                PeakId = peakId,
                HasPassed = record is not null,
                CyclesUsed = record?.Cycles ?? 0,
            };

            var lineup = player.PeakLineups.GetValueOrDefault(peakId) ?? record?.Avatars ?? [];
            peak.PeakAvatarIdList.AddRange(lineup);
            peak.FinishedTargetList.AddRange(record?.FinishedTargets ?? []);

            stars += record?.Stars ?? 0;
            proto.Peaks.Add(peak);
        }

        proto.CountOfPeaks = (uint)proto.Peaks.Count;

        if (group.BossLevelID > 0 && data.ChallengePeaks.ContainsKey(group.BossLevelID))
        {
            var easy = player.PeakRecord(group.BossLevelID, hard: false);
            var hard = player.PeakRecord(group.BossLevelID, hard: true);

            var boss = new ChallengePeakBoss
            {
                HardModeHasPassed = hard is not null,
                EasyMode = Clearance(easy),
                HardMode = Clearance(hard),
            };

            boss.FinishedTargetList.AddRange((easy?.FinishedTargets ?? []).Concat(hard?.FinishedTargets ?? []).Distinct());
            proto.PeakBoss = boss;

            stars += easy?.Stars ?? 0;
            stars += hard?.Stars ?? 0;
        }

        proto.ObtainedStars = stars;
        return proto;
    }

    private static ChallengePeakBossClearance Clearance(PeakRecord? record)
    {
        var clearance = new ChallengePeakBossClearance
        {
            HasPassed = record is not null,
            BuffId = record?.BuffId ?? 0,
            BestCycleCount = record?.Cycles ?? 0,
            BestRecordBuffId = record?.BuffId ?? 0,
        };

        clearance.PeakAvatarIdList.AddRange(record?.Avatars ?? []);
        return clearance;
    }
}
