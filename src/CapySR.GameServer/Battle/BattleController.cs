using CapySR.Data.Excel;
using CapySR.Data.SrTools;
using CapySR.GameServer.Game;
using CapySR.GameServer.Handlers;
using CapySR.GameServer.Net;
using CapySR.GameServer.Scene;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Battle;

public sealed class CastOutcome
{
    public SceneBattleInfo? Battle { get; set; }

    public List<HitMonsterBattleInfo> Hits { get; } = [];

    // props smashed and fodder one-shot by a technique
    public List<uint> RemovedEntities { get; } = [];

    // orbs that refund technique points or heal on the way through
    public int MpOrbs { get; set; }

    public int HpOrbs { get; set; }
}

// starts fights for a session and settles them when the client reports back
public sealed class BattleController(PlayerSession session)
{
    private const uint FallbackStageId = 201012311;

    private uint _nextBattleId;

    public BattleInstance? Current { get; private set; }

    public BattleEndStatus LastEndStatus { get; private set; }

    public bool InBattle => Current is not null;

    private GameWorld World => session.World;

    private Player Player => session.Player;

    private SrToolsData SrTools => World.Player;

    // the srtools battle_config is a fight of its own. it drives every non-challenge fight,
    // calyxes included, so an old MOC/PF/AS stage can be replayed from one.
    private bool UseSrTools(BattleOrigin origin)
    {
        if (origin == BattleOrigin.Challenge)
        {
            return false;
        }

        var config = SrTools.BattleConfig;
        var ready = config.StageId != 0 && (config.Monsters.Count > 0 || World.Data.GetStage(config.StageId) is not null);

        return session.Config.GameServer.BattleSource.Trim().ToLowerInvariant() switch
        {
            "stage" => false,
            _ => ready,
        };
    }

    public void Quit()
    {
        if (Current is not null)
        {
            LastEndStatus = BattleEndStatus.BattleEndQuit;
        }

        Current = null;
    }

    public CastOutcome HandleCast(SceneCastSkillCsReq request)
    {
        var outcome = new CastOutcome();
        var scene = session.Scene;

        var targetIds = request.HitTargetEntityIdList
            .Concat(request.AssistMonsterEntityIdList)
            .Concat(request.AssistMonsterEntityInfo.SelectMany(i => i.EntityIdList))
            .Where(id => id != 0)
            .Distinct()
            .ToList();

        foreach (var prop in scene.PropsAmong(targetIds))
        {
            if (World.Data.Props.GetValueOrDefault(prop.PropId) is not { IsDestructible: true } excel)
            {
                continue;
            }

            outcome.RemovedEntities.Add(prop.EntityId);
            outcome.MpOrbs += excel.IsMpRecover ? 1 : 0;
            outcome.HpOrbs += excel.IsHpRecover ? 1 : 0;
        }

        var monsters = scene.MonstersAmong(targetIds).ToList();
        var caster = scene.Get(request.AttackedByEntityId) ?? scene.Get(request.CastEntityId);
        var ambush = caster?.Kind == SceneEntityKind.Monster;

        if (ambush && monsters.All(m => m.EntityId != caster!.EntityId))
        {
            monsters.Add(caster!);
        }

        if (monsters.Count == 0)
        {
            return outcome;
        }

        // the client retries a cast it never got an answer for
        if (Current is not null)
        {
            outcome.Battle = Current.Info;
            return outcome;
        }

        var technique = request.SkillIndex == 1;
        var lineup = ResolveLineup(session.Challenge.Active ? session.Challenge.Lineup : Player.Lineups.Current.AvatarIds);
        var casterBase = caster?.Kind == SceneEntityKind.Actor ? caster.AvatarId : 0;
        var casterIndex = IndexOfBase(lineup, casterBase);

        if (casterIndex < 0)
        {
            casterIndex = session.Challenge.Active ? 0 : (int)Player.Lineups.Current.LeaderIndex;
        }

        // some techniques wipe fodder on the spot instead of starting a fight; never inside a challenge
        if (technique && !session.Challenge.Active && casterIndex < lineup.Count && TechniqueBuffs.KillsFodder(lineup[casterIndex]))
        {
            var fodder = monsters.Where(m => World.Data.IsFodderMonster(m.MonsterId)).ToList();

            foreach (var monster in fodder)
            {
                outcome.Hits.Add(new HitMonsterBattleInfo
                {
                    TargetMonsterEntityId = monster.EntityId,
                    MonsterBattleType = MonsterBattleType.DirectDieSkipBattle,
                });

                outcome.RemovedEntities.Add(monster.EntityId);
            }

            monsters = [.. monsters.Except(fodder)];

            if (monsters.Count == 0)
            {
                return outcome;
            }
        }

        foreach (var monster in monsters)
        {
            outcome.Hits.Add(new HitMonsterBattleInfo
            {
                TargetMonsterEntityId = monster.EntityId,
                MonsterBattleType = MonsterBattleType.TriggerBattle,
            });
        }

        var skillIndex = technique ? 2u : 1u;

        var instance = session.Challenge.Active
            ? StartChallenge(monsters, (uint)casterIndex, skillIndex, ambush)
            : UseSrTools(BattleOrigin.Overworld)
                ? StartSrTools(BattleOrigin.Overworld, monsters, (uint)casterIndex, skillIndex, ambush)
                : StartOverworld(monsters, (uint)casterIndex, skillIndex, ambush);

        outcome.Battle = instance.Info;
        return outcome;
    }

    public BattleInstance? StartCocoon(uint cocoonId, uint wave, uint worldLevel)
    {
        if (UseSrTools(BattleOrigin.Cocoon))
        {
            return StartSrTools(BattleOrigin.Cocoon, [], 0, 1, false, cocoonId: cocoonId, wave: wave);
        }

        var level = worldLevel == 0 ? Player.WorldLevel : worldLevel;
        var cocoon = World.Data.FindCocoon(cocoonId, level);

        if (cocoon is null)
        {
            session.Logger.LogWarning("no calyx config for {Cocoon} at world level {Level}", cocoonId, level);
            return null;
        }

        // every run of the calyx rolls one of its stages
        var runs = Math.Clamp((int)wave, 1, 6);
        var stages = Enumerable.Range(0, runs)
            .Select(_ => cocoon.Stages[Random.Shared.Next(cocoon.Stages.Count)])
            .ToList();

        return Start(new BattleRequest
        {
            StageId = stages[0],
            ExtraStageIds = stages.Skip(1).ToList(),
            Lineup = ResolveLineup(Player.Lineups.Current.AvatarIds),
            LeaderSlot = Player.Lineups.Current.LeaderIndex,
            PlayerInitiated = false,
        }, BattleOrigin.Cocoon, [], cocoonId: cocoonId, wave: (uint)runs);
    }

    public BattleInstance? StartFarmElement(uint farmId, uint worldLevel)
    {
        if (UseSrTools(BattleOrigin.FarmElement))
        {
            return StartSrTools(BattleOrigin.FarmElement, [], 0, 1, false, farmId: farmId);
        }

        var level = worldLevel == 0 ? Player.WorldLevel : worldLevel;
        var farm = World.Data.FindFarmElement(farmId, level);

        if (farm is null)
        {
            session.Logger.LogWarning("no farm element config for {Farm} at world level {Level}", farmId, level);
            return null;
        }

        return Start(new BattleRequest
        {
            StageId = farm.StageID,
            Lineup = ResolveLineup(Player.Lineups.Current.AvatarIds),
            LeaderSlot = Player.Lineups.Current.LeaderIndex,
            PlayerInitiated = false,
        }, BattleOrigin.FarmElement, [], farmId: farmId);
    }

    public BattleInstance? StartCollege(uint collegeId)
    {
        if (!World.Data.BattleColleges.TryGetValue(collegeId, out var college))
        {
            return null;
        }

        var trial = college.TrialAvatarList
            .Select(id => World.Data.SpecialAvatars.GetValueOrDefault(id))
            .OfType<SpecialAvatarExcel>()
            .ToList();

        return Start(new BattleRequest
        {
            StageId = college.StageID,
            Lineup = trial.Count > 0 ? [] : ResolveLineup(Player.Lineups.Current.AvatarIds),
            TrialAvatars = trial,
            PlayerInitiated = false,
        }, BattleOrigin.College, [], collegeId: collegeId);
    }

    // a stage entered by event id: story fights, stagnant shadows once activated, ...
    public BattleInstance? StartEvent(uint eventId)
    {
        if (Current is not null)
        {
            return Current;
        }

        if (session.Challenge.Active)
        {
            return StartChallenge([], 0, 1, false);
        }

        var stageId = World.Data.ResolveStageForEvent(eventId, Player.WorldLevel);

        if (stageId == 0)
        {
            if (UseSrTools(BattleOrigin.Stage) || UseSrTools(BattleOrigin.Overworld))
            {
                return StartSrTools(BattleOrigin.Stage, [], 0, 1, false, eventId: eventId);
            }

            session.Logger.LogWarning("event {Event} resolves to no stage", eventId);
            return null;
        }

        var stage = World.Data.GetStage(stageId);
        var trial = TrialAvatarsFor(stage);

        return Start(new BattleRequest
        {
            StageId = stageId,
            Lineup = trial.Count > 0 ? [] : ResolveLineup(Player.Lineups.Current.AvatarIds),
            TrialAvatars = trial,
            LeaderSlot = Player.Lineups.Current.LeaderIndex,
            PlayerInitiated = false,
        }, BattleOrigin.Stage, [], eventId: eventId);
    }

    private BattleInstance StartOverworld(List<SceneEntity> monsters, uint casterIndex, uint skillIndex, bool ambush)
    {
        var stages = monsters
            .Select(m => World.Data.ResolveStageForEvent(m.EventId, Player.WorldLevel))
            .Where(id => id != 0)
            .Distinct()
            .ToList();

        if (stages.Count == 0)
        {
            if (UseSrTools(BattleOrigin.Stage))
            {
                return StartSrTools(BattleOrigin.Overworld, monsters, casterIndex, skillIndex, ambush);
            }

            session.Logger.LogWarning(
                "no stage for monsters [{Monsters}]; using stage {Stage}",
                string.Join(", ", monsters.Select(m => $"{m.MonsterId}/{m.EventId}")), FallbackStageId);

            stages.Add(FallbackStageId);
        }

        var stage = World.Data.GetStage(stages[0]);
        var trial = TrialAvatarsFor(stage);

        return Start(new BattleRequest
        {
            StageId = stages[0],
            ExtraStageIds = stages.Skip(1).ToList(),
            Lineup = trial.Count > 0 ? [] : ResolveLineup(Player.Lineups.Current.AvatarIds),
            TrialAvatars = trial,
            LeaderSlot = casterIndex,
            SkillIndex = skillIndex,
            Ambush = ambush,
        }, BattleOrigin.Overworld, monsters, eventId: monsters[0].EventId);
    }

    private BattleInstance StartSrTools(
        BattleOrigin origin, List<SceneEntity> monsters, uint casterIndex, uint skillIndex, bool ambush,
        uint cocoonId = 0, uint wave = 0, uint farmId = 0, uint eventId = 0)
    {
        var config = SrTools.BattleConfig;

        // srtools' explicit lineup wins over the in-game team
        var lineup = config.CustomLineup is { Count: > 0 } custom
            ? custom.OrderBy(kv => kv.Key).Select(kv => kv.Value).Where(id => id != 0).ToList()
            : ResolveLineup(Player.Lineups.Current.AvatarIds);

        if (lineup.Count == 0)
        {
            lineup = [.. SrTools.Avatars.Keys.Where(Roster.IsPlayable).OrderBy(id => id).Take(4)];
        }

        return Start(new BattleRequest
        {
            StageId = config.StageId,
            Lineup = lineup,
            LeaderSlot = Math.Min(casterIndex, (uint)Math.Max(0, lineup.Count - 1)),
            SkillIndex = skillIndex,
            Ambush = ambush,
            PlayerInitiated = origin == BattleOrigin.Overworld,
            Kind = config.Kind,
            CycleCount = config.CycleCount,
            MonsterWaves = config.Monsters is { Count: > 0 } ? config.Monsters : null,
            Blessings = config.Blessings,
            PathResonanceId = config.Kind == BattleKind.SimulatedUniverse ? config.PathResonanceId : 0,
            CustomStats = config.CustomStats,
        }, origin, monsters, cocoonId: cocoonId, wave: wave, farmId: farmId, eventId: eventId);
    }

    private BattleInstance StartChallenge(List<SceneEntity> monsters, uint casterIndex, uint skillIndex, bool ambush)
    {
        var state = session.Challenge;

        var request = new BattleRequest
        {
            StageId = state.StageId,
            Lineup = ResolveLineup(state.Lineup),
            LeaderSlot = casterIndex,
            SkillIndex = skillIndex,
            Ambush = ambush,
            PlayerInitiated = monsters.Count > 0,
            Kind = state.IsPeak ? BattleKind.AnomalyArbitration : state.Kind switch
            {
                ChallengeKind.PureFiction => BattleKind.PureFiction,
                ChallengeKind.ApocalypticShadow => BattleKind.ApocalypticShadow,
                _ => BattleKind.Moc,
            },
            CycleCount = state.Kind == ChallengeKind.MemoryOfChaos && !state.IsPeak ? Math.Max(1, state.RoundsLeft) : state.CycleCount,
            Blessings = [.. state.Blessings.Select(id => new SrBlessing { Id = id, Level = 1 })],
            ChallengeScore = state.TotalScore,
        };

        if (state.Kind == ChallengeKind.PureFiction && World.Data.ChallengeStoryExtras.TryGetValue(state.ChallengeId, out var extra))
        {
            request.ScoreTargets = extra.BattleTargetID;
        }

        foreach (var targetId in state.PeakTargets)
        {
            var param = World.Data.BattleTargets.GetValueOrDefault(targetId)?.TargetParam ?? 0;
            request.PeakTargets.Add((targetId, param));
        }

        return Start(request, BattleOrigin.Challenge, monsters);
    }

    private BattleInstance Start(
        BattleRequest request, BattleOrigin origin, List<SceneEntity> monsters,
        uint cocoonId = 0, uint wave = 0, uint farmId = 0, uint collegeId = 0, uint eventId = 0)
    {
        // /stage: one fight plays the requested stage, whatever started it
        if (origin != BattleOrigin.Challenge && Player.NextStageOverride != 0)
        {
            request.StageId = Player.NextStageOverride;
            request.ExtraStageIds = [];
            request.MonsterWaves = null;
            request.TrialAvatars = TrialAvatarsFor(World.Data.GetStage(request.StageId));
            Player.NextStageOverride = 0;
        }

        request.BattleId = ++_nextBattleId;
        request.WorldLevel = Player.WorldLevel;
        request.Gender = Player.Gender;
        request.StateOf = id => Player.StateOf(id, SrTools);
        request.IsEnhanced = id => Player.IsEnhanced(id, SrTools);

        var info = World.Battles.Build(request, SrTools);

        var instance = new BattleInstance
        {
            BattleId = request.BattleId,
            StageId = request.StageId,
            Origin = origin,
            Kind = request.Kind,
            Info = info,
            Lineup = [.. request.Lineup],
            MonsterEntityIds = [.. monsters.Select(m => m.EntityId)],
            EventId = eventId,
            CocoonId = cocoonId,
            Wave = wave,
            FarmElementId = farmId,
            CollegeId = collegeId,
            UsesTrialAvatars = request.TrialAvatars.Count > 0,
        };

        Current = instance;

        session.Logger.LogInformation(
            "battle {Id} ({Origin}): stage {Stage}, {Avatars} avatars, {Waves} waves, {Buffs} buffs, {Monsters} scene monsters",
            instance.BattleId, origin, instance.StageId, info.BattleAvatarList.Count,
            info.MonsterWaveList.Count, info.BuffList.Count, instance.MonsterEntityIds.Count);

        return instance;
    }

    // the client tells us how it went; the world catches up with it
    public async Task<PVEBattleResultScRsp> FinishAsync(PVEBattleResultCsReq request)
    {
        var battle = Current;
        Current = null;
        LastEndStatus = request.EndStatus;

        var response = new PVEBattleResultScRsp
        {
            Retcode = 0,
            BattleId = request.BattleId,
            StageId = request.StageId,
            EndStatus = request.EndStatus,
            CheckIdentical = false,
            EventId = battle?.EventId ?? 0,
            DropData = new ItemList(),
            MultipleDropData = new ItemList(),
            ItemListUnk1 = new ItemList(),
            ItemListUnk2 = new ItemList(),
        };

        session.Logger.LogInformation(
            "battle {Id} stage {Stage} ended {Status} after {Rounds} rounds ({Ms} ms)",
            request.BattleId, request.StageId, request.EndStatus, request.Stt?.RoundCnt ?? 0, request.CostTime);

        if (battle is null)
        {
            return response;
        }

        if (!battle.UsesTrialAvatars && request.EndStatus != BattleEndStatus.BattleEndQuit)
        {
            ApplyAvatarStatus(request);
        }

        switch (request.EndStatus)
        {
            case BattleEndStatus.BattleEndWin:
                await session.RemoveEntitiesAsync(battle.MonsterEntityIds);

                if (battle.CollegeId != 0 && Player.FinishedColleges.Add(battle.CollegeId))
                {
                    Player.Touch();
                }

                break;

            case BattleEndStatus.BattleEndLose:
                // the party limps away rather than lying dead in the overworld
                foreach (var state in Player.AvatarStates.Values)
                {
                    state.Hp = Math.Max(state.Hp, 2000);
                }

                break;
        }

        if (battle.IsChallenge && session.Challenge.Active)
        {
            await ChallengeFlow.OnBattleEndAsync(session, battle, request);
        }

        await session.SyncLineupAsync();
        return response;
    }

    private void ApplyAvatarStatus(PVEBattleResultCsReq request)
    {
        if (request.Stt is null)
        {
            return;
        }

        foreach (var avatar in request.Stt.BattleAvatarList)
        {
            if (avatar.AvatarType != AvatarType.AvatarFormalType || avatar.AvatarStatus is null)
            {
                continue;
            }

            var status = avatar.AvatarStatus;
            var state = Player.StateOf(avatar.Id, SrTools);

            if (status.MaxHp > 0)
            {
                var ratio = Math.Clamp(status.LeftHp / status.MaxHp, 0, 1);
                state.Hp = (uint)Math.Round(ratio * Player.FullHp);
            }

            if (status.MaxSp > 0)
            {
                state.SpMax = (uint)Math.Round(status.MaxSp);
                state.SpCur = (uint)Math.Round(Math.Clamp(status.LeftSp, 0, status.MaxSp));
            }
        }
    }

    // story fights lend their own characters; the roster stays out of it
    private List<SpecialAvatarExcel> TrialAvatarsFor(StageExcel? stage) =>
        stage is null
            ? []
            : [.. stage.TrialAvatarList.Select(id => World.Data.SpecialAvatars.GetValueOrDefault(id)).OfType<SpecialAvatarExcel>()];

    // lineups hold base ids; the fight wants the path actually equipped
    private List<uint> ResolveLineup(IEnumerable<uint> avatarIds) =>
        [.. avatarIds.Where(id => id != 0).Select(id => Roster.ResolvePath(World, Player, id)).Distinct()];

    private int IndexOfBase(List<uint> lineup, uint baseAvatarId)
    {
        if (baseAvatarId == 0)
        {
            return -1;
        }

        for (var i = 0; i < lineup.Count; i++)
        {
            if (World.Data.BaseAvatarId(lineup[i]) == baseAvatarId)
            {
                return i;
            }
        }

        return -1;
    }
}
