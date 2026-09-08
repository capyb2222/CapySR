using CapySR.Common;
using CapySR.Data;
using CapySR.Data.SrTools;
using CapySR.GameServer.Battle;
using CapySR.GameServer.Game;
using Xunit;
using Xunit.Abstractions;

namespace CapySR.Tests;

// runs against a real freesr-data.json when one is present
public class SrToolsTests(ITestOutputHelper output)
{
    private static SrToolsData? Load(ITestOutputHelper output)
    {
        var path = SrToolsData.ResolvePath(RepoPaths.Root);

        if (!File.Exists(path))
        {
            output.WriteLine($"skipping: no {path}");
            return null;
        }

        output.WriteLine($"loaded {path}");
        return SrToolsData.Load(path);
    }

    [Fact]
    public void ParsesRealExport()
    {
        var player = Load(output);
        if (player is null)
        {
            return;
        }

        output.WriteLine($"{player.Avatars.Count} avatars, {player.Relics.Count} relics, " +
                         $"{player.Lightcones.Count} lightcones, stage {player.BattleConfig.StageId}");

        Assert.NotEmpty(player.Avatars);
        Assert.NotEmpty(player.Relics);
        Assert.NotEmpty(player.Lightcones);

        // dictionary keys arrive as json strings
        Assert.All(player.Avatars, kv => Assert.Equal(kv.Key, kv.Value.AvatarId));
    }

    [Fact]
    public void ReadsTechniquesNotUseTechnique()
    {
        var player = Load(output);
        if (player is null)
        {
            return;
        }

        var withTechniques = player.Avatars.Values.Count(a => a.Techniques.Count > 0);
        output.WriteLine($"{withTechniques}/{player.Avatars.Count} avatars carry technique buffs");

        Assert.True(withTechniques > player.Avatars.Count / 2,
            $"only {withTechniques} avatars had techniques - is the json field name still 'techniques'?");
    }

    [Fact]
    public void TracesAndGearSurvive()
    {
        var player = Load(output);
        if (player is null)
        {
            return;
        }

        Assert.All(player.Avatars.Values, a => Assert.NotEmpty(a.Data.Skills));
        Assert.Contains(player.Avatars.Values, a => a.Data.Rank > 0);

        // relic and lightcone unique ids must not collide
        var relicIds = player.Relics.Select(r => r.UniqueId).ToHashSet();
        var lightconeIds = player.Lightcones.Select(l => l.UniqueId).ToHashSet();
        Assert.Empty(relicIds.Intersect(lightconeIds));

        Assert.Contains(player.Relics, r => r.EquipAvatar != 0 && r.SubAffixes.Count > 0);
        Assert.Contains(player.Lightcones, l => l.EquipAvatar != 0);
    }

    [Fact]
    public void BuildsABattleFromTheRealExport()
    {
        var player = Load(output);
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();

        if (player is null || sources.Count == 0)
        {
            return;
        }

        var data = GameData.Load(sources);
        var config = player.BattleConfig;

        var lineup = player.Avatars.Keys.Where(Roster.IsPlayable).OrderBy(id => id).Take(4).ToList();

        var battle = new BattleBuilder(data).Build(
            new BattleRequest
            {
                StageId = config.StageId,
                Lineup = lineup,
                Kind = config.Kind,
                CycleCount = config.CycleCount == 0 ? 30 : config.CycleCount,
                MonsterWaves = config.Monsters is { Count: > 0 } ? config.Monsters : null,
                Blessings = config.Blessings,
            },
            player);

        output.WriteLine($"stage {battle.StageId}: {battle.BattleAvatarList.Count} avatars, " +
                         $"{battle.BuffList.Count} buffs, {battle.MonsterWaveList.Count} waves");

        Assert.Equal(lineup.Count, battle.BattleAvatarList.Count);
        Assert.All(battle.BattleAvatarList, a => Assert.NotEmpty(a.SkilltreeList));
        Assert.NotEmpty(battle.MonsterWaveList);

        // srtools' own technique ids must reach the battle
        foreach (var avatarId in lineup)
        {
            foreach (var technique in player.Avatars[avatarId].Techniques)
            {
                Assert.Contains(battle.BuffList, b => b.Id == technique);
            }
        }

        // and its blessings
        foreach (var blessing in config.Blessings)
        {
            Assert.Contains(battle.BuffList, b => b.Id == blessing.Id);
        }
    }

    [Fact]
    public void MonsterAmountIsExpanded()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        if (sources.Count == 0)
        {
            return;
        }

        var data = GameData.Load(sources);
        var player = new SrToolsData();
        player.Avatars[1001] = new SrAvatar { AvatarId = 1001 };

        var battle = new BattleBuilder(data).Build(
            new BattleRequest
            {
                StageId = 1022010,
                Lineup = [1001],
                MonsterWaves = [[new SrMonster { MonsterId = 3014022, Level = 95, Amount = 3 }]],
            },
            player);

        Assert.Equal(3, battle.MonsterWaveList[0].MonsterList.Count);
    }
}

public class InventoryIdTests(ITestOutputHelper output)
{
    // a unique_id of 0 means "no item"; sending one makes the client's inventory
    // red-dot filter NPE every frame and the character screen freezes black
    [Fact]
    public void NoInventoryItemUsesIdZero()
    {
        var path = SrToolsData.ResolvePath(RepoPaths.Root);
        if (!File.Exists(path))
        {
            return;
        }

        var player = SrToolsData.Load(path);

        Assert.All(player.Relics, r => Assert.NotEqual(0u, r.UniqueId));
        Assert.All(player.Lightcones, l => Assert.NotEqual(0u, l.UniqueId));
    }

    [Fact]
    public void RelicAndLightconeIdRangesDoNotOverlap()
    {
        var path = SrToolsData.ResolvePath(RepoPaths.Root);
        if (!File.Exists(path))
        {
            return;
        }

        var player = SrToolsData.Load(path);

        var relics = player.Relics.Select(r => r.UniqueId).ToHashSet();
        var lightcones = player.Lightcones.Select(l => l.UniqueId).ToHashSet();

        output.WriteLine($"relics {relics.Min()}-{relics.Max()}, lightcones {lightcones.Min()}-{lightcones.Max()}");
        Assert.Empty(relics.Intersect(lightcones));
        Assert.Equal(player.Relics.Count, relics.Count);
        Assert.Equal(player.Lightcones.Count, lightcones.Count);
    }
}
