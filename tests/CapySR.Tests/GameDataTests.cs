using CapySR.Common;
using CapySR.Data;
using CapySR.Data.Excel;
using Xunit;
using Xunit.Abstractions;

namespace CapySR.Tests;

public class GameDataTests(ITestOutputHelper output)
{
    private static GameData? TryLoad(ITestOutputHelper output)
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();

        if (sources.Count == 0)
        {
            output.WriteLine("skipping: no game data sources found");
            return null;
        }

        return GameData.Load(sources, output.WriteLine);
    }

    [Fact]
    public void LoadsEveryTable()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        Assert.True(data.Avatars.Count > 80, $"only {data.Avatars.Count} avatars");
        Assert.True(data.Stages.Count > 20_000, $"only {data.Stages.Count} stages");
        Assert.NotEmpty(data.Monsters);
        Assert.NotEmpty(data.Equipment);
    }

    // an empty banner leaves the client's gacha red dot throwing once a frame
    [Fact]
    public void ReadsStandardBannerCeiling()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        Assert.NotEmpty(data.GachaCeilingAvatars);
        Assert.Contains(1003u, data.GachaCeilingAvatars);
    }

    [Fact]
    public void ParsesMarch7th()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        var march = data.GetAvatar(1001);
        Assert.NotNull(march);
        Assert.Equal("Ice", march.DamageType);
        Assert.Equal("Knight", march.AvatarBaseType);
        Assert.Equal(120u, march.SpMax);
        Assert.Equal(6, march.RankIDList.Count);

        var tree = data.MaxedSkillTree(1001).ToList();
        Assert.True(tree.Count > 10, $"only {tree.Count} trace points");
        Assert.All(tree, p => Assert.True(p.Level > 0));
    }

    [Fact]
    public void ParsesStageMonsterWaves()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        var stage = data.GetStage(1022010);
        Assert.NotNull(stage);
        Assert.Equal("Cocoon", stage.StageType);
        Assert.Equal(16u, stage.Level);

        var wave = Assert.Single(stage.MonsterList);
        Assert.Equal([8001010, 8001020, 8001010, 8001020], wave);

        // obfuscated key names, matched by the '_'-prefixed value
        Assert.Equal("1", stage.StageConfigData["_Wave"]);
    }

    [Fact]
    public void BetaOverlayAddsNewCharacters()
    {
        var data = TryLoad(output);
        if (data is null || data.Sources.Count < 2)
        {
            return;
        }

        // 1503 Pearl ships in the 4.6 beta dump only, not in prod 4.5.0
        var pearl = data.GetAvatar(1503);
        Assert.NotNull(pearl);
        Assert.Equal("Ice", pearl.DamageType);
        Assert.Equal("Elation", pearl.AvatarBaseType);
        Assert.True(data.MaxedSkillTree(1503).Count() >= 10);

        // prod rows still present and not clobbered by the overlay
        Assert.NotNull(data.GetAvatar(1001));
        Assert.Equal(100101u, data.DefaultMazeBuffs[1001].BuffId);
    }

    // 101 is the Space Anchor; the Chap00 boxes are the first MP/HP orbs the game shows
    [Fact]
    public void PropsAreClassifiedByEntityPath()
    {
        var data = TryLoad(output);
        if (data is null || data.Props.Count == 0)
        {
            return;
        }

        Assert.True(data.Props[101].IsSpring);
        Assert.True(data.Props[100025].IsMpRecover);
        Assert.False(data.Props[100025].IsHpRecover);
        Assert.True(data.Props[100024].IsHpRecover);
        Assert.True(data.Props[100026].IsDestructible);
        Assert.False(data.Props[100026].IsMpRecover || data.Props[100026].IsHpRecover);
        Assert.Contains(100026u, data.DestructiblePropIds);
        Assert.DoesNotContain(101u, data.DestructiblePropIds);
    }

    [Fact]
    public void OverworldEventsResolveToStages()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        // 30001011 is MOC floor 1's encounter; its event is also its stage
        Assert.Equal(30001011u, data.ResolveStageForEvent(30001011, 6));
        Assert.Equal(0u, data.ResolveStageForEvent(0, 6));

        // a real overworld monster from the start scene resolves at every world level
        var monster = data.GetScene(new GameServerConfig().StartEntryId)!.Monsters.First(m => m.EventId != 0);
        for (var level = 0u; level <= 6; level++)
        {
            var stage = data.ResolveStageForEvent(monster.EventId, level);
            Assert.NotEqual(0u, stage);
            Assert.NotNull(data.GetStage(stage));
        }

        // calyx and farm rows fall back to the closest lower world level
        Assert.NotNull(data.FindCocoon(1001, 6));
        Assert.NotNull(data.FindCocoon(1001, 0));
        Assert.Null(data.FindCocoon(999999, 6));
        Assert.NotNull(data.FindFarmElement(1101, 6));
    }

    [Fact]
    public void ChallengeTablesLoad()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        Assert.True(data.Challenges.Count > 600, $"only {data.Challenges.Count} challenge floors");
        Assert.NotEmpty(data.MapEntrances);
        Assert.NotEmpty(data.MazePlanes);

        var kinds = data.Challenges.Values.GroupBy(c => c.Kind).ToDictionary(g => g.Key, g => g.Count());
        foreach (var (kind, count) in kinds)
        {
            output.WriteLine($"{kind}: {count}");
        }

        Assert.All(Enum.GetValues<ChallengeKind>(), k => Assert.True(kinds.ContainsKey(k), $"no {k} floors"));
    }

    // dimbreath's ChallengePeakConfig has no MapEntranceID column at all; the beta dump does.
    // whole-row merging would drop it and leave AA with nowhere to teleport to.
    [Fact]
    public void FieldMergeRecoversBetaOnlyColumns()
    {
        var data = TryLoad(output);
        if (data is null || data.Sources.Count < 2)
        {
            return;
        }

        var playable = data.ChallengePeaks.Values.Where(p => p.IsPlayable).ToList();
        output.WriteLine($"{playable.Count}/{data.ChallengePeaks.Count} AA stages have scene fields");
        Assert.NotEmpty(playable);

        // 101 exists in both sources; only the beta one carries MapEntranceID
        var first = data.ChallengePeaks[101];
        Assert.NotEqual(0u, first.MapEntranceID);
        Assert.NotEmpty(first.NpcMonsterIDList);
        Assert.NotEmpty(first.TagList);
    }

    [Fact]
    public void EntranceResolvesToAPlane()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        var challenge = data.Challenges.Values.First(c => c.MapEntranceID > 0);
        var resolved = data.ResolveEntrance(challenge.MapEntranceID);

        Assert.NotNull(resolved);
        Assert.NotEqual(0u, resolved.Value.Entrance.FloorID);
        Assert.NotNull(resolved.Value.Plane);
        Assert.NotEqual(0u, resolved.Value.Plane!.WorldID);
    }

    [Fact]
    public void ResolvesTechniqueAndGlobalBuffs()
    {
        var data = TryLoad(output);
        if (data is null)
        {
            return;
        }

        var march = data.DefaultMazeBuffs[1001];
        Assert.Equal(100101u, march.BuffId);
        Assert.Equal(2u, march.SkillIndex);

        // castorice and silver wolf are the only two with a global buff
        Assert.Equal(2, data.GlobalBuffs.Count);
        Assert.Contains(data.GlobalBuffs, b => b.AvatarID == 1407);
        Assert.Contains(data.GlobalBuffs, b => b.AvatarID == 1506);
    }
}

public class PathSkillTreeTests(ITestOutputHelper output)
{
    // AvatarPathData.point_id is the anchor index ("Point01" -> 1), not the raw point id.
    // sending raw point ids gives the client something it cannot resolve.
    [Fact]
    public void AnchorIndexParsesFromAnchorType()
    {
        Assert.Equal(1u, new AvatarSkillTreeExcel { AnchorType = "Point01" }.AnchorIndex);
        Assert.Equal(12u, new AvatarSkillTreeExcel { AnchorType = "Point12" }.AnchorIndex);
        Assert.Equal(0u, new AvatarSkillTreeExcel { AnchorType = "" }.AnchorIndex);
        Assert.Equal(0u, new AvatarSkillTreeExcel { AnchorType = "Point" }.AnchorIndex);
    }

    [Fact]
    public void EveryPlayableAvatarHasAnchoredTracePoints()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        if (sources.Count == 0)
        {
            return;
        }

        var data = GameData.Load(sources);
        var playable = data.Avatars.Keys.Where(id => id is (> 1000 and < 2000) or (>= 8001 and <= 8010)).ToList();

        var missing = playable.Where(id => !data.MaxedPathSkillTree(id).Any()).ToList();
        output.WriteLine($"{playable.Count} playable avatars, {missing.Count} without anchored trace points");

        Assert.Empty(missing);

        // anchors are small indices, never raw point ids
        foreach (var (anchor, _) in data.MaxedPathSkillTree(1001))
        {
            Assert.InRange(anchor, 1u, 100u);
        }
    }

    [Fact]
    public void MultiPathAvatarsAreKnown()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        if (sources.Count == 0)
        {
            return;
        }

        var data = GameData.Load(sources);

        Assert.Contains(8001u, data.MultiPathAvatarIds);
        Assert.Contains(1001u, data.MultiPathAvatarIds);
        Assert.Contains(1224u, data.MultiPathAvatarIds);
        Assert.DoesNotContain(1003u, data.MultiPathAvatarIds);
    }
}
