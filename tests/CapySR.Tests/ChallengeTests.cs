using CapySR.Common;
using CapySR.Data;
using CapySR.Data.Excel;
using CapySR.GameServer.Game;
using CapySR.GameServer.Scene;
using Xunit;
using Xunit.Abstractions;

namespace CapySR.Tests;

public class ChallengeTests(ITestOutputHelper output)
{
    private static GameData? Load()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        return sources.Count == 0 ? null : GameData.Load(sources);
    }

    private static readonly uint[] Lineup = [1001, 1003, 1004, 1005];

    [Fact]
    public void EveryChallengeFloorResolvesAScene()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var failed = new List<uint>();
        var noStage = new List<uint>();

        foreach (var challenge in data.Challenges.Values.OrderBy(c => c.ID))
        {
            var state = new ChallengeState();

            if (!state.StartChallenge(data, challenge, 0, 0, Lineup))
            {
                failed.Add(challenge.ID);
                continue;
            }

            if (data.GetStage(state.StageId) is null)
            {
                noStage.Add(challenge.ID);
            }
        }

        output.WriteLine($"{data.Challenges.Count} floors, {failed.Count} unresolvable, {noStage.Count} without a stage");
        Assert.True(failed.Count == 0, $"no scene for: {string.Join(", ", failed.Take(12))}");
        Assert.True(noStage.Count == 0, $"no stage for: {string.Join(", ", noStage.Take(12))}");
    }

    [Fact]
    public void SecondNodeResolvesWhereDefined()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var withSecond = data.Challenges.Values.Where(c => c.HasSecondNode).ToList();
        output.WriteLine($"{withSecond.Count}/{data.Challenges.Count} floors have a second node");
        Assert.NotEmpty(withSecond);

        foreach (var challenge in withSecond.Take(50))
        {
            var state = new ChallengeState();
            Assert.True(state.StartChallenge(data, challenge, 1, 0, Lineup), $"floor {challenge.ID} node 2");
            Assert.Equal(1, state.Node);
            Assert.NotEqual(0u, state.StageId);
        }
    }

    [Fact]
    public void ChallengeCarriesMazeBuffAndPlayerPick()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var challenge = data.Challenges.Values.First(c => c.MazeBuffID != 0);
        var state = new ChallengeState();

        Assert.True(state.StartChallenge(data, challenge, 0, 900001, Lineup));
        Assert.Contains(challenge.MazeBuffID, state.Blessings);
        Assert.Contains(900001u, state.Blessings);
    }

    [Fact]
    public void ChallengeKindsSplitByIdRange()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        Assert.All(data.Challenges.Values.Where(c => c.ID < 20000), c => Assert.Equal(ChallengeKind.MemoryOfChaos, c.Kind));
        Assert.All(data.Challenges.Values.Where(c => c.ID is > 20000 and < 30000), c => Assert.Equal(ChallengeKind.PureFiction, c.Kind));
        Assert.All(data.Challenges.Values.Where(c => c.ID > 30000), c => Assert.Equal(ChallengeKind.ApocalypticShadow, c.Kind));
    }

    [Fact]
    public void EveryAaStageResolvesAScene()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var playable = data.ChallengePeaks.Values.Where(p => p.IsPlayable).ToList();
        Assert.NotEmpty(playable);

        foreach (var peak in playable)
        {
            data.ChallengePeakBosses.TryGetValue(peak.ID, out var boss);
            var state = new ChallengeState();

            Assert.True(state.StartPeak(data, peak, boss, 0, Lineup), $"AA {peak.ID}");
            Assert.NotEqual(0u, state.StageId);
            Assert.NotNull(data.GetStage(state.StageId));
            Assert.NotEmpty(state.Blessings);
        }

        output.WriteLine($"{playable.Count} AA stages all resolve");
    }

    [Fact]
    public void AaHardModeSwapsTagsAndEncounter()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var boss = data.ChallengePeakBosses.Values.FirstOrDefault(b => b.HardTagList.Count > 0);
        if (boss is null || !data.ChallengePeaks.TryGetValue(boss.ID, out var peak) || !peak.IsPlayable)
        {
            return;
        }

        var easy = new ChallengeState();
        easy.StartPeak(data, peak, boss, 0, Lineup);

        var hard = new ChallengeState { HardMode = true };
        hard.StartPeak(data, peak, boss, 0, Lineup);

        Assert.NotEqual(easy.Blessings, hard.Blessings);
        Assert.All(boss.HardTagList, tag => Assert.Contains(tag, hard.Blessings));

        if (boss.HardEventIDList.Count > 0)
        {
            Assert.Equal(boss.HardEventIDList[^1], hard.StageId);
        }
    }

    [Fact]
    public void SceneHasAvatarsAndTheEncounter()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var challenge = data.Challenges.Values.First();
        var state = new ChallengeState();
        Assert.True(state.StartChallenge(data, challenge, 0, 0, Lineup));

        var scene = new SceneBuilder(data).BuildChallengeScene(state);

        Assert.Equal(state.FloorId, scene.FloorId);
        Assert.Equal(state.EntryId, scene.EntryId);
        Assert.NotEqual(0u, scene.PlaneId);
        Assert.Equal(2, scene.EntityGroupList.Count);

        var avatars = scene.EntityGroupList[0];
        Assert.Equal(Lineup.Length, avatars.EntityList.Count);
        Assert.All(avatars.EntityList, e => Assert.NotNull(e.Actor));
        Assert.Equal(avatars.EntityList[0].EntityId, scene.LeaderEntityId);

        var monsters = scene.EntityGroupList[1];
        var monster = Assert.Single(monsters.EntityList);
        Assert.NotNull(monster.NpcMonster);
        Assert.Equal(state.MonsterId, monster.NpcMonster.MonsterId);
        Assert.Equal(state.EventId, monster.NpcMonster.EventId);
        Assert.Equal(state.GroupId, monsters.GroupId);
    }

    [Fact]
    public void MultipathAvatarsUseTheirBaseIdInScenes()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var challenge = data.Challenges.Values.First();
        var state = new ChallengeState();
        state.StartChallenge(data, challenge, 0, 0, [8010, 1224]);

        var scene = new SceneBuilder(data).BuildChallengeScene(state);
        var actors = scene.EntityGroupList[0].EntityList.Select(e => e.Actor!.BaseAvatarId).ToList();

        Assert.Equal([8001u, 1001u], actors);
    }

    [Fact]
    public void ClearResetsEverything()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var state = new ChallengeState();
        state.StartChallenge(data, data.Challenges.Values.First(), 0, 0, Lineup);
        Assert.True(state.Active);

        state.Clear();

        Assert.False(state.Active);
        Assert.Equal(0u, state.StageId);
        Assert.Empty(state.Lineup);
        Assert.Empty(state.Blessings);
    }
}

public class WorldSceneTests(ITestOutputHelper output)
{
    private static GameData? Load()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        return sources.Count == 0 ? null : GameData.Load(sources);
    }

    [Fact]
    public void TrainSceneHasAvatarsAndResolves()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var scene = new SceneBuilder(data).BuildWorldScene(1000001, [8002, 1001, 1003, 1004]);

        Assert.Equal(1000001u, scene.EntryId);
        Assert.Equal(10000u, scene.PlaneId);
        Assert.Equal(10000000u, scene.FloorId);
        Assert.NotEqual(0u, scene.LeaderEntityId);

        var avatars = scene.EntityGroupList[0];
        Assert.Equal(4, avatars.EntityList.Count);
        Assert.All(avatars.EntityList, e => Assert.NotNull(e.Actor));

        // 8002 collapses to the 8001 base id in scenes
        Assert.Equal(8001u, avatars.EntityList[0].Actor.BaseAvatarId);

        // the train deliberately gets no props back
        Assert.Single(scene.EntityGroupList);
    }

    [Fact]
    public void NonTrainSceneGetsItsProps()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var scene = new SceneBuilder(data).BuildWorldScene(2000101, [8002]);

        Assert.Equal(20001u, scene.PlaneId);
        Assert.True(scene.EntityGroupList.Count > 1, "expected prop groups");
        output.WriteLine($"entry 2000101: {scene.EntityGroupList.Count} groups, " +
                         $"{scene.EntityGroupList.Sum(g => g.EntityList.Count)} entities");
    }
}

public class StartSceneTests(ITestOutputHelper output)
{
    [Fact]
    public void StartSceneIsPopulatedAndAvatarsAreNotAtTheOrigin()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        if (sources.Count == 0)
        {
            return;
        }

        var data = GameData.Load(sources);
        var server = new GameServerConfig();

        var scene = new SceneBuilder(data).BuildWorldScene(
            server.StartEntryId, [8002, 1001, 1003, 1004], server.StartTeleportId, server.WorldId);

        output.WriteLine($"entry {scene.EntryId}: plane {scene.PlaneId}, floor {scene.FloorId}, " +
                         $"{scene.EntityGroupList.Count} groups, " +
                         $"{scene.EntityGroupList.Sum(g => g.EntityList.Count)} entities");

        Assert.Equal(server.StartEntryId, scene.EntryId);
        Assert.NotEqual(0u, scene.PlaneId);
        Assert.NotEqual(0u, scene.FloorId);
        Assert.NotEqual(0u, scene.LeaderEntityId);

        // the environment has to come through, not just the party
        Assert.True(scene.EntityGroupList.Count > 1, "start scene has no props or monsters");

        var spawn = scene.EntityGroupList[0].EntityList[0].Motion.Pos;
        Assert.False(spawn.X == 0 && spawn.Y == 0 && spawn.Z == 0,
            "avatars spawned at the origin - the teleport position was not applied");
        output.WriteLine($"spawn: {spawn.X},{spawn.Y},{spawn.Z}");
    }
}
