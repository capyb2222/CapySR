using CapySR.Common;
using CapySR.Data;
using CapySR.Data.SrTools;
using CapySR.GameServer.Battle;
using Xunit;
using Xunit.Abstractions;

namespace CapySR.Tests;

public class BattleTests(ITestOutputHelper output)
{
    private const uint AllWaves = 0xFFFFFFFF;

    private static GameData? Load()
    {
        var sources = new DataConfig().ResolvedSources.Where(Directory.Exists).ToList();
        return sources.Count == 0 ? null : GameData.Load(sources);
    }

    private static SrToolsData PlayerWith(params uint[] avatarIds)
    {
        var data = new SrToolsData();

        foreach (var id in avatarIds)
        {
            data.Avatars[id] = new SrAvatar { AvatarId = id, Level = 80, Promotion = 6 };
        }

        return data;
    }

    private static BattleRequest Request(uint stageId, params uint[] lineup) => new()
    {
        StageId = stageId,
        Lineup = lineup,
        CycleCount = 30,
    };

    [Fact]
    public void BuildsBattleFromStageData()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var builder = new BattleBuilder(data);
        var battle = builder.Build(Request(1022010, 1001, 1003), PlayerWith(1001, 1003));

        Assert.Equal(1022010u, battle.StageId);
        Assert.Equal(2, battle.BattleAvatarList.Count);
        Assert.Equal(30u, battle.RoundsLimit);

        // stage 1022010 is one wave of four monsters
        var wave = Assert.Single(battle.MonsterWaveList);
        Assert.Equal(1u, wave.BattleWaveId);
        Assert.Equal([8001010u, 8001020u, 8001010u, 8001020u], wave.MonsterList.Select(m => m.MonsterId));
        Assert.Equal(16u, wave.MonsterParam.Level);
        Assert.Equal((uint)battle.MonsterWaveList.Count, battle.MonsterWaveLength);
        Assert.NotEqual(0u, battle.LogicRandomSeed);
    }

    [Fact]
    public void EveryAvatarProducesAUsableBattleAvatar()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var builder = new BattleBuilder(data);
        var noTree = new List<uint>();
        var noBuff = new List<uint>();

        // playable ranges only - 6xxx/7xxx are npc and trial models with no trace tree
        var playable = data.Avatars.Keys
            .Where(id => id is (> 1000 and < 2000) or (>= 8001 and <= 8010))
            .OrderBy(id => id)
            .ToList();

        foreach (var avatarId in playable)
        {
            var battle = builder.Build(Request(1022010, avatarId), PlayerWith(avatarId));
            var avatar = Assert.Single(battle.BattleAvatarList);

            if (avatar.SkilltreeList.Count == 0)
            {
                noTree.Add(avatarId);
            }

            if (!battle.BuffList.Any(b => b.OwnerIndex == 0 && b.Id != 0))
            {
                noBuff.Add(avatarId);
            }
        }

        output.WriteLine($"checked {playable.Count} playable avatars");
        Assert.True(playable.Count >= 90, $"only found {playable.Count} playable avatars");
        Assert.True(noTree.Count == 0, $"no skill tree: {string.Join(", ", noTree)}");
        Assert.True(noBuff.Count == 0, $"no technique buff: {string.Join(", ", noBuff)}");
    }

    [Fact]
    public void NewBetaCharacterBuildsCompletely()
    {
        var data = Load();
        if (data is null || !data.Avatars.ContainsKey(1503))
        {
            return;
        }

        var builder = new BattleBuilder(data);
        var battle = builder.Build(Request(1022010, 1503), PlayerWith(1503));

        var pearl = Assert.Single(battle.BattleAvatarList);
        Assert.Equal(1503u, pearl.Id);
        Assert.True(pearl.SkilltreeList.Count >= 10, $"only {pearl.SkilltreeList.Count} trace points");

        // 1503 is absent from AvatarDefaultMazeBuff, so it must come from the fallback table
        Assert.Contains(battle.BuffList, b => b.Id == 150301);
        Assert.Contains(battle.BuffList, b => b.Id == 1000121);
        Assert.DoesNotContain(battle.BuffList, b => b.Id == 150301 + 0 && b.WaveFlag != AllWaves);
    }

    // guards against regressing away from what the 4.6 reference server sends
    [Theory]
    [InlineData(1001, 100101u)]
    [InlineData(1208, 120802u)]
    [InlineData(1304, 130403u)]
    [InlineData(1308, 130803u)]
    [InlineData(1310, 131001u)]
    [InlineData(1412, 141201u)]
    [InlineData(1510, 151002u)]
    public void TechniqueBuffsMatchTheReference(uint avatarId, uint expectedBuffId)
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var buffs = TechniqueBuffs.For(data, avatarId).Select(b => b.Id).ToList();
        Assert.Contains(expectedBuffId, buffs);
    }

    [Theory]
    [InlineData(1208, 120801u)]
    [InlineData(1310, 1000112u)]
    [InlineData(1412, 1000121u)]
    [InlineData(1224, 122401u)]
    public void ExtraTechniqueBuffsAreKept(uint avatarId, uint extraBuffId)
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        Assert.Contains(extraBuffId, TechniqueBuffs.For(data, avatarId).Select(b => b.Id));
    }

    [Fact]
    public void FemaleTrailblazerPathsReuseMaleBuffs()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        foreach (var (female, male) in new[] { (8004u, 8003u), (8006u, 8005u), (8008u, 8007u), (8010u, 8009u) })
        {
            var f = TechniqueBuffs.For(data, female).Select(b => b.Id).ToList();
            var m = TechniqueBuffs.For(data, male).Select(b => b.Id).ToList();
            Assert.Equal(m, f);
        }
    }

    [Theory]
    [InlineData("Physical", 1000111u)]
    [InlineData("Fire", 1000112u)]
    [InlineData("Ice", 1000113u)]
    [InlineData("Thunder", 1000114u)]
    [InlineData("Wind", 1000115u)]
    [InlineData("Quantum", 1000116u)]
    [InlineData("Imaginary", 1000117u)]
    public void AttackerBuffFollowsElement(string element, uint expected)
    {
        Assert.Equal(expected, TechniqueBuffs.AttackerBuff(element));
    }

    [Fact]
    public void LeaderElementDrivesTheAttackerBuff()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var builder = new BattleBuilder(data);

        // 1001 March 7th is Ice
        var battle = builder.Build(Request(1022010, 1001), PlayerWith(1001));
        Assert.Contains(battle.BuffList, b => b.Id == 1000113 && b.DynamicValues["SkillIndex"] == 1f);
    }

    [Fact]
    public void GlobalBuffsOnlyApplyWhenTheAvatarIsOwned()
    {
        var data = Load();
        if (data is null || data.GlobalMazeBuffs.Count == 0)
        {
            return;
        }

        var builder = new BattleBuilder(data);
        var (globalAvatar, globalBuff) = data.GlobalMazeBuffs[0];

        var without = builder.Build(Request(1022010, 1001), PlayerWith(1001));
        Assert.DoesNotContain(without.BuffList, b => b.Id == globalBuff);

        var with = builder.Build(Request(1022010, 1001), PlayerWith(1001, globalAvatar));
        Assert.Contains(with.BuffList, b => b.Id == globalBuff);
    }

    [Fact]
    public void GlobalBuffsAreTheOnesTheReferenceHardcodes()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var ids = data.GlobalMazeBuffs.Select(g => g.BuffId).OrderBy(i => i).ToList();
        Assert.Equal([140703u, 150602u], ids);
    }

    [Fact]
    public void PureFictionGetsScoreTargets()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var builder = new BattleBuilder(data);
        var request = Request(30301001, 1001);
        request.Kind = BattleKind.PureFiction;

        var battle = builder.Build(request, PlayerWith(1001));

        Assert.Equal(5, battle.BattleTargetInfo.Count);
        Assert.Contains(battle.BattleTargetInfo[1].BattleTargetList_, t => t.Id is 10002 or 10003);
        Assert.Equal([2001u, 2002u], battle.BattleTargetInfo[5].BattleTargetList_.Select(t => t.Id));
    }

    [Fact]
    public void ApocalypticShadowGetsItsGauge()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var builder = new BattleBuilder(data);
        var request = Request(1022010, 1001);
        request.Kind = BattleKind.ApocalypticShadow;

        var battle = builder.Build(request, PlayerWith(1001));
        Assert.Equal(90005u, Assert.Single(battle.BattleTargetInfo[1].BattleTargetList_).Id);
    }

    [Fact]
    public void InvadedStagesCarryTheInvasionBuff()
    {
        var data = Load();
        if (data is null || data.StageInvasions.Count == 0)
        {
            return;
        }

        var invasion = data.StageInvasions.Values.First();
        var battle = new BattleBuilder(data).Build(Request(invasion.StageID, 1001), PlayerWith(1001));

        Assert.Contains(battle.BuffList, b => b.Id == 3034000 + invasion.InvasionID);

        var plain = new BattleBuilder(data).Build(Request(1022010, 1001), PlayerWith(1001));
        Assert.DoesNotContain(plain.BuffList, b => b.Id is >= 3034000 and < 3035000);
    }

    [Fact]
    public void EnhancedAvatarsCarryTheirEnhancedId()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var builder = new BattleBuilder(data);

        // 1310 is one of the ten in AvatarConfigEnhanced; the kit is a toggle, off by default
        var untoggled = builder.Build(Request(1022010, 1310), PlayerWith(1310));
        Assert.Equal(0u, Assert.Single(untoggled.BattleAvatarList).EnhancedId);

        var toggled = Request(1022010, 1310);
        toggled.IsEnhanced = _ => true;
        Assert.NotEqual(0u, Assert.Single(builder.Build(toggled, PlayerWith(1310)).BattleAvatarList).EnhancedId);

        // srtools can switch it on too
        var srtools = PlayerWith(1310);
        srtools.Avatars[1310].EnhancedId = 1;
        Assert.Equal(1u, Assert.Single(builder.Build(Request(1022010, 1310), srtools).BattleAvatarList).EnhancedId);

        // an avatar without an enhanced kit never gets one, toggle or not
        var plain = Request(1022010, 1001);
        plain.IsEnhanced = _ => true;
        Assert.Equal(0u, Assert.Single(builder.Build(plain, PlayerWith(1001)).BattleAvatarList).EnhancedId);
    }

    [Fact]
    public void SrToolsGearReachesTheBattle()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var player = PlayerWith(1001);
        player.Lightcones.Add(new SrLightcone { ItemId = 23001, EquipAvatar = 1001, Level = 80, Rank = 5, Promotion = 6, InternalUid = 1 });
        player.Relics.Add(new SrRelic
        {
            RelicId = 61011,
            EquipAvatar = 1001,
            Level = 15,
            MainAffixId = 1,
            InternalUid = 7,
            SubAffixes = [new SrSubAffix { SubAffixId = 2, Count = 5, Step = 3 }],
        });

        var battle = new BattleBuilder(data).Build(Request(1022010, 1001), player);
        var avatar = Assert.Single(battle.BattleAvatarList);

        var lightcone = Assert.Single(avatar.EquipmentList);
        Assert.Equal(23001u, lightcone.Id);
        Assert.Equal(5u, lightcone.Rank);

        var relic = Assert.Single(avatar.RelicList);
        Assert.Equal(61011u, relic.Id);
        var affix = Assert.Single(relic.SubAffixList);
        Assert.Equal(5u, affix.Cnt);
        Assert.Equal(3u, affix.Step);
    }

    [Fact]
    public void CustomMonsterWavesOverrideTheStage()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var request = Request(1022010, 1001);
        request.MonsterWaves =
        [
            [new SrMonster { MonsterId = 3014022, Level = 95 }],
            [new SrMonster { MonsterId = 3014023, Level = 95 }, new SrMonster { MonsterId = 3014024, Level = 95 }],
        ];

        var battle = new BattleBuilder(data).Build(request, PlayerWith(1001));

        Assert.Equal(2, battle.MonsterWaveList.Count);
        Assert.Equal(2u, battle.MonsterWaveLength);
        Assert.Equal([1u, 2u], battle.MonsterWaveList.Select(w => w.BattleWaveId));
        Assert.Equal(95u, battle.MonsterWaveList[0].MonsterParam.Level);
    }

    [Fact]
    public void BuffsTargetEveryWave()
    {
        var data = Load();
        if (data is null)
        {
            return;
        }

        var battle = new BattleBuilder(data).Build(Request(1022010, 1001, 1003), PlayerWith(1001, 1003));

        Assert.NotEmpty(battle.BuffList);
        Assert.All(battle.BuffList, b => Assert.Equal(AllWaves, b.WaveFlag));
        Assert.All(battle.BuffList, b => Assert.NotEqual(0u, b.Id));
    }
}
