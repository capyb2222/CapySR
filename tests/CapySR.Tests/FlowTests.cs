using System.Net;
using System.Net.Sockets;
using CapySR.Common;
using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;
using Xunit.Abstractions;

namespace CapySR.Tests;

// the whole pipeline through a real gateway: login, teams, overworld fights, a MOC floor
public class FlowTests(ITestOutputHelper output)
{
    private static readonly GameWorld World = new(TestConfig(), NullLogger.Instance);

    // the default harness plays real stages so an assertion about a fight is about the data,
    // not about whatever freesr-data.json happens to hold
    private static ServerConfig TestConfig() => new()
    {
        GameServer = { Host = "127.0.0.1", Persist = false, BattleSource = "stage" },
    };

    private sealed class Harness : IAsyncDisposable
    {
        private readonly CancellationTokenSource _cancellation = new(TimeSpan.FromSeconds(60));
        private readonly Task _server;

        public Harness(string battleSource = "stage")
        {
            var port = FreeUdpPort();
            var config = TestConfig();
            config.GameServer.Port = port;
            config.GameServer.BattleSource = battleSource;

            var handlers = HandlerRegistry.Build(NullLogger.Instance);
            var gateway = new KcpGateway(config, handlers, World, NullLogger<KcpGateway>.Instance);
            _server = gateway.RunAsync(_cancellation.Token);
            Client = new TestClient(new IPEndPoint(IPAddress.Loopback, port));
        }

        public TestClient Client { get; }

        public async Task LoginAsync()
        {
            await Client.ConnectAsync();
            await Client.RequestAsync<PlayerGetTokenCsReq, PlayerGetTokenScRsp>(new PlayerGetTokenCsReq());
            var login = await Client.RequestAsync<PlayerLoginCsReq, PlayerLoginScRsp>(new PlayerLoginCsReq { LoginRandom = 1 });
            Assert.Equal(0u, login.Retcode);
            Client.ClearInbox();
        }

        public async ValueTask DisposeAsync()
        {
            Client.Dispose();
            await _cancellation.CancelAsync();
            await Task.WhenAny(_server, Task.Delay(2000));
        }

        private static int FreeUdpPort()
        {
            using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            probe.Bind(new IPEndPoint(IPAddress.Loopback, 0));
            return ((IPEndPoint)probe.LocalEndPoint!).Port;
        }
    }

    private static bool HasData => new DataConfig().ResolvedSources.Any(Directory.Exists);

    [Fact]
    public async Task RosterAndTeamsLoad()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        var avatars = await client.RequestAsync<GetAvatarDataCsReq, GetAvatarDataScRsp>(new GetAvatarDataCsReq { IsGetAll = true });
        Assert.True(avatars.AvatarList.Count >= 4, $"only {avatars.AvatarList.Count} avatars");
        Assert.All(avatars.AvatarPathDataInfoList, p => Assert.NotEmpty(p.AvatarPathSkillTree));
        Assert.Contains(8001u, avatars.BasicTypeIdList);

        var all = await client.RequestAsync<GetAllLineupDataCsReq, GetAllLineupDataScRsp>(new GetAllLineupDataCsReq());
        Assert.Equal(LineupBook.SquadCount, all.LineupList.Count);
        Assert.Equal(0u, all.CurIndex);
        Assert.NotEmpty(all.LineupList[0].AvatarList);
        Assert.All(all.LineupList, l => Assert.Equal(ExtraLineupType.LineupNone, l.ExtraLineupType));
        Assert.Equal([0u, 1u, 2u, 3u, 4u, 5u], all.LineupList.Select(l => l.Index));

        var current = await client.RequestAsync<GetCurLineupDataCsReq, GetCurLineupDataScRsp>(new GetCurLineupDataCsReq());
        Assert.Equal(all.LineupList[0].AvatarList.Count, current.Lineup.AvatarList.Count);
        Assert.Equal(LineupBook.BaseMaxMp, current.Lineup.MaxMp);

        var picker = await client.RequestAsync<GetLineupAvatarDataCsReq, GetLineupAvatarDataScRsp>(new GetLineupAvatarDataCsReq());
        Assert.True(picker.AvatarDataList.Count >= avatars.AvatarList.Count);
        Assert.All(picker.AvatarDataList, a => Assert.Equal(Player.FullHp, a.Hp));
    }

    [Fact]
    public async Task TeamEditsRoundTripAndRefreshTheScene()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        var scene = await client.RequestAsync<GetCurSceneInfoCsReq, GetCurSceneInfoScRsp>(new GetCurSceneInfoCsReq());
        var actorsBefore = scene.Scene.EntityGroupList.SelectMany(g => g.EntityList).Where(e => e.Actor is not null).ToList();
        Assert.NotEmpty(actorsBefore);
        client.ClearInbox();

        var owned = World.OwnedAvatarIds.Select(World.Data.BaseAvatarId).Distinct().ToList();
        var a = owned[0];
        var b = owned[1];
        var c = owned[2];

        var replace = new ReplaceLineupCsReq { Index = 0, LeaderSlot = 1 };
        replace.LineupSlotList.Add(new LineupSlotData { Slot = 0, Id = a, AvatarType = AvatarType.AvatarFormalType });
        replace.LineupSlotList.Add(new LineupSlotData { Slot = 1, Id = b, AvatarType = AvatarType.AvatarFormalType });

        var replaced = await client.RequestAsync<ReplaceLineupCsReq, ReplaceLineupScRsp>(replace);
        Assert.Equal(0u, replaced.Retcode);

        var sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.Equal([a, b], sync.Lineup.AvatarList.Select(x => x.Id));
        Assert.Equal(1u, sync.Lineup.LeaderSlot);

        // the party model changed, so the scene had to swap actors in place
        await client.PumpAsync(TimeSpan.FromMilliseconds(300));
        var refreshes = client.Take<SceneGroupRefreshScNotify>();
        Assert.NotEmpty(refreshes);
        Assert.Contains(refreshes.SelectMany(r => r.GroupRefreshList), g => g.GroupId == 0 && g.RefreshEntity.Count > 0);

        var join = await client.RequestAsync<JoinLineupCsReq, JoinLineupScRsp>(new JoinLineupCsReq { Index = 0, Slot = 2, OPCJCDKMJEI = c });
        Assert.Equal(0u, join.Retcode);
        sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.Equal([a, b, c], sync.Lineup.AvatarList.Select(x => x.Id));
        Assert.Equal([0u, 1u, 2u], sync.Lineup.AvatarList.Select(x => x.Slot));

        var swap = await client.RequestAsync<SwapLineupCsReq, SwapLineupScRsp>(new SwapLineupCsReq { Index = 0, MPIEGFCNBEN = 0, ENBIBCPPLDC = 2 });
        Assert.Equal(0u, swap.Retcode);
        sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.Equal([c, b, a], sync.Lineup.AvatarList.Select(x => x.Id));
        // the leader is a character, not a chair
        Assert.Equal(1u, sync.Lineup.LeaderSlot);

        var leader = await client.RequestAsync<ChangeLineupLeaderCsReq, ChangeLineupLeaderScRsp>(new ChangeLineupLeaderCsReq { Slot = 2 });
        Assert.Equal(0u, leader.Retcode);
        Assert.Equal(2u, leader.Slot);
        sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.Equal(2u, sync.Lineup.LeaderSlot);

        var quit = await client.RequestAsync<QuitLineupCsReq, QuitLineupScRsp>(new QuitLineupCsReq { Index = 0, BaseAvatarId = b });
        Assert.Equal(0u, quit.Retcode);
        sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.Equal([c, a], sync.Lineup.AvatarList.Select(x => x.Id));
        Assert.Equal([0u, 2u], sync.Lineup.AvatarList.Select(x => x.Slot));

        // an empty squad cannot become the walking team
        var empty = await client.RequestAsync<SwitchLineupIndexCsReq, SwitchLineupIndexScRsp>(new SwitchLineupIndexCsReq { Index = 3 });
        Assert.Equal((uint)Retcode.RetLineupIsEmpty, empty.Retcode);

        var named = await client.RequestAsync<SetLineupNameCsReq, SetLineupNameScRsp>(new SetLineupNameCsReq { Index = 3, Name = "Bench" });
        Assert.Equal("Bench", named.Name);

        var fill = new ReplaceLineupCsReq { Index = 3 };
        fill.LineupSlotList.Add(new LineupSlotData { Slot = 0, Id = b, AvatarType = AvatarType.AvatarFormalType });
        await client.RequestAsync<ReplaceLineupCsReq, ReplaceLineupScRsp>(fill);

        var switched = await client.RequestAsync<SwitchLineupIndexCsReq, SwitchLineupIndexScRsp>(new SwitchLineupIndexCsReq { Index = 3 });
        Assert.Equal(0u, switched.Retcode);

        var current = await client.RequestAsync<GetCurLineupDataCsReq, GetCurLineupDataScRsp>(new GetCurLineupDataCsReq());
        Assert.Equal("Bench", current.Lineup.Name);
        Assert.Equal([b], current.Lineup.AvatarList.Select(x => x.Id));
        Assert.Equal(3u, current.Lineup.Index);
    }

    [Fact]
    public async Task HittingAMonsterStartsAndSettlesAFight()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        var scene = await client.RequestAsync<GetCurSceneInfoCsReq, GetCurSceneInfoScRsp>(new GetCurSceneInfoCsReq());
        var entities = scene.Scene.EntityGroupList.SelectMany(g => g.EntityList).ToList();
        var actor = entities.First(e => e.Actor is not null);
        var monster = entities.First(e => e.NpcMonster is not null && e.NpcMonster.EventId != 0);
        client.ClearInbox();

        var cast = new SceneCastSkillCsReq
        {
            CastEntityId = actor.EntityId,
            AttackedByEntityId = actor.EntityId,
            SkillIndex = 0,
        };
        cast.HitTargetEntityIdList.Add(monster.EntityId);
        cast.AssistMonsterEntityIdList.Add(monster.EntityId);

        var response = await client.RequestAsync<SceneCastSkillCsReq, SceneCastSkillScRsp>(cast);
        Assert.Equal(0u, response.Retcode);
        Assert.NotNull(response.BattleInfo);

        var hit = Assert.Single(response.MonsterBattleInfo);
        Assert.Equal(monster.EntityId, hit.TargetMonsterEntityId);
        Assert.Equal(MonsterBattleType.TriggerBattle, hit.MonsterBattleType);

        var battle = response.BattleInfo;
        output.WriteLine($"stage {battle.StageId}: {battle.BattleAvatarList.Count} avatars, {battle.MonsterWaveList.Count} waves, {battle.BuffList.Count} buffs");
        Assert.NotEqual(0u, battle.StageId);
        Assert.NotEmpty(battle.BattleAvatarList);
        Assert.NotEmpty(battle.MonsterWaveList);
        // the overworld has no cycle limit; only challenges count rounds
        Assert.Equal(0u, battle.RoundsLimit);
        Assert.Equal((uint)battle.MonsterWaveList.Count, battle.MonsterWaveLength);
        Assert.Equal(Enumerable.Range(0, battle.BattleAvatarList.Count).Select(i => (uint)i), battle.BattleAvatarList.Select(a => a.Index));

        // the attacker opened with a normal attack, so the entry buff says so
        var opener = battle.BuffList.FirstOrDefault(b => b.Id is >= 1000111 and <= 1000117);
        Assert.NotNull(opener);
        Assert.Equal(1f, opener.DynamicValues["SkillIndex"]);

        // the client can ask again mid-fight and gets the same seed back
        var current = await client.RequestAsync<GetCurBattleInfoCsReq, GetCurBattleInfoScRsp>(new GetCurBattleInfoCsReq());
        Assert.Equal(battle.LogicRandomSeed, current.BattleInfo.LogicRandomSeed);

        var result = new PVEBattleResultCsReq
        {
            BattleId = battle.BattleId,
            StageId = battle.StageId,
            EndStatus = BattleEndStatus.BattleEndWin,
            Stt = new BattleStatistics { RoundCnt = 3 },
        };

        foreach (var avatar in battle.BattleAvatarList)
        {
            result.Stt.BattleAvatarList.Add(new AvatarBattleInfo
            {
                Id = avatar.Id,
                AvatarType = AvatarType.AvatarFormalType,
                AvatarStatus = new AvatarProperty { MaxHp = 1000, LeftHp = 500, MaxSp = 120, LeftSp = 60 },
            });
        }

        var settled = await client.RequestAsync<PVEBattleResultCsReq, PVEBattleResultScRsp>(result);
        Assert.Equal(0u, settled.Retcode);
        Assert.Equal(BattleEndStatus.BattleEndWin, settled.EndStatus);
        Assert.Equal(battle.BattleId, settled.BattleId);

        // the monster is gone from the map and the party remembers its wounds
        var refresh = await client.ExpectAsync<SceneGroupRefreshScNotify>();
        Assert.Contains(refresh.GroupRefreshList.SelectMany(g => g.RefreshEntity), e => e.DeleteEntity == monster.EntityId);

        var sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.All(sync.Lineup.AvatarList, a => Assert.Equal(5000u, a.Hp));
        Assert.All(sync.Lineup.AvatarList, a => Assert.Equal(60u, a.SpBar.CurSp));

        var after = await client.RequestAsync<GetCurSceneInfoCsReq, GetCurSceneInfoScRsp>(new GetCurSceneInfoCsReq());
        Assert.DoesNotContain(after.Scene.EntityGroupList.SelectMany(g => g.EntityList), e => e.EntityId == monster.EntityId);

        // nothing is left to fight, so no battle is offered
        var idle = await client.RequestAsync<GetCurBattleInfoCsReq, GetCurBattleInfoScRsp>(new GetCurBattleInfoCsReq());
        Assert.Equal(0u, idle.BattleInfo.StageId);
        Assert.Equal(BattleEndStatus.BattleEndWin, idle.LastEndStatus);
    }

    // a calyx is how an older MOC/PF/AS stage gets replayed: it runs whatever srtools uploaded
    [Fact]
    public async Task CalyxPlaysTheConfiguredBattle()
    {
        var configured = World.Player.BattleConfig;

        if (!HasData || configured.StageId == 0)
        {
            output.WriteLine("no srtools battle_config; skipping");
            return;
        }

        var request = new StartCocoonStageCsReq { CocoonId = 1001, Wave = 1, WorldLevel = 6, PropEntityId = 7 };

        await using (var auto = new Harness("auto"))
        {
            await auto.LoginAsync();
            var rsp = await auto.Client.RequestAsync<StartCocoonStageCsReq, StartCocoonStageScRsp>(request);

            Assert.Equal(0u, rsp.Retcode);
            Assert.Equal(7u, rsp.PropEntityId);
            Assert.Equal(configured.StageId, rsp.BattleInfo.StageId);
        }

        // "stage" opts out and plays the calyx's own encounter
        await using var stages = new Harness("stage");
        await stages.LoginAsync();
        var own = await stages.Client.RequestAsync<StartCocoonStageCsReq, StartCocoonStageScRsp>(request);

        Assert.Equal(0u, own.Retcode);
        Assert.NotEqual(configured.StageId, own.BattleInfo.StageId);
        Assert.NotNull(World.Data.GetStage(own.BattleInfo.StageId));
    }

    [Fact]
    public async Task TechniquesSpendPointsAndTeleportsRestoreThem()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        var cost = await client.RequestAsync<SceneCastSkillCostMpCsReq, SceneCastSkillCostMpScRsp>(new SceneCastSkillCostMpCsReq { CastEntityId = 101001 });
        Assert.Equal(0u, cost.Retcode);

        var update = await client.ExpectAsync<SceneCastSkillMpUpdateScNotify>();
        Assert.Equal(LineupBook.BaseMaxMp - 1, update.Mp);

        var entry = new GameServerConfig().StartEntryId;
        var enter = await client.RequestAsync<EnterSceneCsReq, EnterSceneScRsp>(new EnterSceneCsReq { EntryId = entry, TeleportId = new GameServerConfig().StartTeleportId });
        Assert.Equal(0u, enter.Retcode);

        var scene = await client.ExpectAsync<EnterSceneByServerScNotify>();
        Assert.Equal(entry, scene.Scene.EntryId);
        Assert.Equal(LineupBook.BaseMaxMp, scene.Lineup.Mp);
        Assert.NotEmpty(scene.Scene.EntityGroupList);
    }

    // the party bar only redraws from the notify, and the client stops listening once the
    // request it sent is answered, so the notify has to be on the wire first
    [Fact]
    public async Task TheTeamIsPushedBeforeTheEditIsAnswered()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        await client.RequestAsync<GetCurSceneInfoCsReq, GetCurSceneInfoScRsp>(new GetCurSceneInfoCsReq());

        var owned = World.OwnedAvatarIds.Select(World.Data.BaseAvatarId).Distinct().ToList();
        client.ClearInbox();

        var replace = new ReplaceLineupCsReq { Index = 0, LeaderSlot = 0 };
        replace.LineupSlotList.Add(new LineupSlotData { Slot = 0, Id = owned[2], AvatarType = AvatarType.AvatarFormalType });
        replace.LineupSlotList.Add(new LineupSlotData { Slot = 1, Id = owned[3], AvatarType = AvatarType.AvatarFormalType });

        await client.RequestAsync<ReplaceLineupCsReq, ReplaceLineupScRsp>(replace);

        var order = client.ArrivalOrder.ToList();
        var sync = order.IndexOf(nameof(SyncLineupNotify));
        var scene = order.IndexOf(nameof(SceneGroupRefreshScNotify));
        var answer = order.IndexOf(nameof(ReplaceLineupScRsp));

        Assert.True(sync >= 0, $"no SyncLineupNotify, got [{string.Join(", ", order)}]");
        Assert.True(scene >= 0, $"no SceneGroupRefreshScNotify, got [{string.Join(", ", order)}]");
        Assert.True(answer >= 0);
        Assert.True(scene < answer, $"scene refresh must precede the answer, got [{string.Join(", ", order)}]");
        Assert.True(sync < answer, $"lineup sync must precede the answer, got [{string.Join(", ", order)}]");
    }

    [Fact]
    public async Task EveryChallengeFloorIsReachable()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();

        var list = await harness.Client.RequestAsync<GetChallengeCsReq, GetChallengeScRsp>(new GetChallengeCsReq());

        Assert.Equal(World.Data.Challenges.Count, list.ChallengeList.Count);
        Assert.Equal(World.Data.ChallengeGroups.Count, list.ChallengeGroupList.Count);

        // a floor with no stars locks the one after it, so none may be left at zero
        Assert.All(list.ChallengeList, c => Assert.True(c.Star > 0, $"floor {c.ChallengeId} is locked"));

        // pure fiction and apocalyptic shadow open on score, taken from their target rows
        foreach (var challenge in World.Data.Challenges.Values.Where(c => c.Kind != Data.Excel.ChallengeKind.MemoryOfChaos))
        {
            var reported = list.ChallengeList.First(c => c.ChallengeId == challenge.ID);
            var target = challenge.ChallengeTargetID
                .Select(id => World.Data.ChallengeTargets.GetValueOrDefault(id))
                .Where(t => t?.ChallengeTargetType == "TOTAL_SCORE")
                .Select(t => t!.ChallengeTargetParam1)
                .DefaultIfEmpty(0u)
                .Max();

            Assert.True(reported.ScoreId >= target, $"floor {challenge.ID} scored {reported.ScoreId}, needs {target}");
        }
    }

    [Fact]
    public async Task ChatCommandsAnswerThroughTheBotFriend()
    {
        if (!HasData)
        {
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        var friends = await client.RequestAsync<GetFriendListInfoCsReq, GetFriendListInfoScRsp>(new GetFriendListInfoCsReq());
        var bot = Assert.Single(friends.FriendList);
        Assert.Equal(GameServer.Handlers.ChatHandlers.BotUid, bot.PlayerInfo.Uid);

        var help = await Say(client, "/help");
        Assert.Contains("/tp", help);

        var unknown = await Say(client, "/nope");
        Assert.Contains("Unknown command", unknown);

        await Say(client, "/wl 3");
        var basic = await client.ExpectAsync<PlayerSyncScNotify>();
        Assert.Equal(3u, basic.BasicInfo.WorldLevel);

        await Say(client, "/mp 2");
        var sync = await client.ExpectAsync<SyncLineupNotify>();
        Assert.Equal(2u, sync.Lineup.Mp);

        // the next fight plays the requested stage, whatever was hit
        var reply = await Say(client, "/stage 1022010");
        Assert.Contains("1022010", reply);

        var scene = await client.RequestAsync<GetCurSceneInfoCsReq, GetCurSceneInfoScRsp>(new GetCurSceneInfoCsReq());
        var entities = scene.Scene.EntityGroupList.SelectMany(g => g.EntityList).ToList();
        var actor = entities.First(e => e.Actor is not null);
        var monster = entities.First(e => e.NpcMonster is not null);

        var cast = new SceneCastSkillCsReq { CastEntityId = actor.EntityId, AttackedByEntityId = actor.EntityId };
        cast.HitTargetEntityIdList.Add(monster.EntityId);
        var battle = await client.RequestAsync<SceneCastSkillCsReq, SceneCastSkillScRsp>(cast);
        Assert.Equal(1022010u, battle.BattleInfo.StageId);
        Assert.Equal(3u, battle.BattleInfo.WorldLevel);
    }

    private static async Task<string> Say(TestClient client, string text)
    {
        client.ClearInbox();

        var message = new SendMsgCsReq
        {
            ChatType = ChatType.Private,
            MessageDatas = new MessageChatData
            {
                MessageType = MsgType.CustomText,
                ChatData = new ChatData { MessageText = text },
            },
        };
        message.TargetList.Add(GameServer.Handlers.ChatHandlers.BotUid);

        var sent = await client.RequestAsync<SendMsgCsReq, SendMsgScRsp>(message);
        Assert.Equal(0u, sent.Retcode);

        var reply = await client.ExpectAsync<RevcMsgScNotify>();
        Assert.Equal(GameServer.Handlers.ChatHandlers.BotUid, reply.SourceUid);
        return reply.RecvMessageData.MessageDatas[0].ChatData.MessageText;
    }

    [Fact]
    public async Task MemoryOfChaosRunsBothNodesAndSettles()
    {
        if (!HasData)
        {
            return;
        }

        var floor = World.Data.Challenges.Values
            .Where(c => c.Kind == Data.Excel.ChallengeKind.MemoryOfChaos && c.NodeCount == 2)
            .OrderBy(c => c.ID)
            .FirstOrDefault(c => new ChallengeState().StartChallenge(World.Data, c, 0, 0, [1001]) &&
                                  new ChallengeState().StartChallenge(World.Data, c, 1, 0, [1001]));

        if (floor is null)
        {
            output.WriteLine("no two-node MOC floor resolves; skipping");
            return;
        }

        await using var harness = new Harness();
        await harness.LoginAsync();
        var client = harness.Client;

        var owned = World.OwnedAvatarIds.Select(World.Data.BaseAvatarId).Distinct().ToList();
        var first = owned.Take(2).ToList();
        var second = owned.Skip(2).Take(2).ToList();

        var start = new StartChallengeCsReq { ChallengeId = floor.ID };
        start.AvatarLineupFirst.AddRange(first.Select(id => new AvatarIdentifier { Id = id, AvatarType = AvatarType.AvatarFormalType }));
        start.AvatarLineupSecond.AddRange(second.Select(id => new AvatarIdentifier { Id = id, AvatarType = AvatarType.AvatarFormalType }));

        var started = await client.RequestAsync<StartChallengeCsReq, StartChallengeScRsp>(start);
        Assert.Equal(0u, started.Retcode);
        Assert.Equal(floor.ID, started.CurChallenge.ChallengeId);
        Assert.Equal(ChallengeStatus.ChallengeDoing, started.CurChallenge.Status);
        Assert.Equal(ExtraLineupType.LineupChallenge, started.CurChallenge.ExtraLineupType);
        Assert.Equal(2, started.LineupList.Count);
        Assert.Equal(first, started.LineupList[0].AvatarList.Select(a => a.Id));
        Assert.Equal(second, started.LineupList[1].AvatarList.Select(a => a.Id));
        Assert.True(started.LineupList[0].IsVirtual);

        var actors = started.Scene.EntityGroupList.SelectMany(g => g.EntityList).Where(e => e.Actor is not null).ToList();
        Assert.Equal(first.Count, actors.Count);

        await FightNodeAsync(client, started.Scene, first.Count, expectSettle: false);

        // the second half arrives as a fresh scene with the second team
        var lineupNotify = await client.ExpectAsync<ChallengeLineupNotify>();
        Assert.Equal(ExtraLineupType.LineupChallenge2, lineupNotify.ExtraLineupType);

        var node2 = await client.ExpectAsync<EnterSceneByServerScNotify>();
        Assert.Equal(second, node2.Lineup.AvatarList.Select(a => a.Id));
        Assert.Equal(ExtraLineupType.LineupChallenge2, node2.Lineup.ExtraLineupType);

        var cur = await client.RequestAsync<GetCurChallengeCsReq, GetCurChallengeScRsp>(new GetCurChallengeCsReq());
        Assert.Equal(ExtraLineupType.LineupChallenge2, cur.CurChallenge.ExtraLineupType);

        await FightNodeAsync(client, node2.Scene, second.Count, expectSettle: true);

        var settle = await client.ExpectAsync<ChallengeSettleNotify>();
        Assert.Equal(floor.ID, settle.ChallengeId);
        Assert.True(settle.IsWin);
        Assert.True(settle.Star > 0, "a full clear should award stars");

        // the record shows up on the floor list afterwards
        var list = await client.RequestAsync<GetChallengeCsReq, GetChallengeScRsp>(new GetChallengeCsReq());
        var record = list.ChallengeList.First(c => c.ChallengeId == floor.ID);
        Assert.Equal(settle.Star, record.Star);

        var left = await client.RequestAsync<LeaveChallengeCsReq, LeaveChallengeScRsp>(new LeaveChallengeCsReq());
        Assert.Equal(0u, left.Retcode);

        var world = await client.ExpectAsync<EnterSceneByServerScNotify>();
        Assert.Equal(ExtraLineupType.LineupNone, world.Lineup.ExtraLineupType);
        Assert.Equal(new GameServerConfig().StartEntryId, world.Scene.EntryId);
    }

    private static async Task FightNodeAsync(TestClient client, SceneInfo scene, int partySize, bool expectSettle)
    {
        var entities = scene.EntityGroupList.SelectMany(g => g.EntityList).ToList();
        var actor = entities.First(e => e.Actor is not null);
        var monster = entities.First(e => e.NpcMonster is not null);
        client.ClearInbox();

        var cast = new SceneCastSkillCsReq { CastEntityId = actor.EntityId, AttackedByEntityId = actor.EntityId, SkillIndex = 1 };
        cast.HitTargetEntityIdList.Add(monster.EntityId);

        var response = await client.RequestAsync<SceneCastSkillCsReq, SceneCastSkillScRsp>(cast);
        var battle = response.BattleInfo;
        Assert.NotNull(battle);
        Assert.Equal(partySize, battle.BattleAvatarList.Count);
        Assert.NotEmpty(battle.MonsterWaveList);
        Assert.True(battle.RoundsLimit is > 0 and <= 30);

        var result = new PVEBattleResultCsReq
        {
            BattleId = battle.BattleId,
            StageId = battle.StageId,
            EndStatus = BattleEndStatus.BattleEndWin,
            Stt = new BattleStatistics { RoundCnt = 2 },
        };

        var settled = await client.RequestAsync<PVEBattleResultCsReq, PVEBattleResultScRsp>(result);
        Assert.Equal(BattleEndStatus.BattleEndWin, settled.EndStatus);
        await client.PumpAsync(TimeSpan.FromMilliseconds(300));
    }
}
