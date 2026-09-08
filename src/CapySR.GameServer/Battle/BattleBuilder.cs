using CapySR.Data;
using CapySR.Data.Excel;
using CapySR.Data.SrTools;
using CapySR.GameServer.Game;
using CapySR.Protocol;

namespace CapySR.GameServer.Battle;

public sealed class BattleRequest
{
    public uint StageId { get; set; }

    // further stages whose waves follow the first one's: several monsters pulled at once
    public List<uint> ExtraStageIds { get; set; } = [];

    public uint BattleId { get; set; } = 1;

    public IReadOnlyList<uint> Lineup { get; set; } = [];

    // index (in Lineup) of the avatar that opened the fight
    public uint LeaderSlot { get; set; }

    // 1 = normal attack, 2 = technique; drives the toughness-break buff on entry
    public uint SkillIndex { get; set; } = 1;

    // a monster attacked first
    public bool Ambush { get; set; }

    public BattleKind Kind { get; set; } = BattleKind.Default;

    // 0 = no cycle limit, the overworld default; challenges set their own
    public uint CycleCount { get; set; }

    public uint WorldLevel { get; set; } = 6;

    // set when the player started the fight by hitting something in the overworld
    public bool PlayerInitiated { get; set; } = true;

    public uint MonsterLevel { get; set; }

    public List<List<SrMonster>>? MonsterWaves { get; set; }

    public List<SrBlessing> Blessings { get; set; } = [];

    // simulated universe path resonance
    public uint PathResonanceId { get; set; }

    public List<SrSubAffix> CustomStats { get; set; } = [];

    // stage-lent characters replace the lineup when present
    public List<SpecialAvatarExcel> TrialAvatars { get; set; } = [];

    public Gender Gender { get; set; } = Gender.Woman;

    // PF: score carried over from the first half
    public uint ChallengeScore { get; set; }

    public List<uint> ScoreTargets { get; set; } = [];

    public List<(uint Id, uint Param)> PeakTargets { get; set; } = [];

    public Func<uint, AvatarState>? StateOf { get; set; }

    public Func<uint, bool>? IsEnhanced { get; set; }
}

public sealed class BattleBuilder(GameData data)
{
    private const uint AllWaves = 0xFFFFFFFF;
    private const uint FirstWave = 1;
    private const uint NoOwner = 0xFFFFFFFF;
    private const uint AmbushBuff = 1000102;

    public SceneBattleInfo Build(BattleRequest request, SrToolsData player)
    {
        var stage = data.GetStage(request.StageId);
        var waves = ResolveWaves(request, stage);

        var battle = new SceneBattleInfo
        {
            BattleId = request.BattleId,
            StageId = request.StageId,
            LogicRandomSeed = (uint)Random.Shared.Next(),
            RoundsLimit = request.CycleCount,
            MonsterWaveLength = (uint)waves.Count,
            WorldLevel = request.WorldLevel,
        };

        var hasTrial = request.TrialAvatars.Count > 0;

        if (hasTrial)
        {
            BuildTrialAvatars(battle, request);
        }
        else
        {
            BuildAvatars(battle, request, player);
        }

        AddEntryBuff(battle, request, player, hasTrial);
        AddBlessings(battle, request);

        if (!hasTrial)
        {
            AddGlobalBuffs(battle, player);
            ApplyCustomStats(battle, request);
        }

        AddPathResonance(battle, request);
        AddStageInvasion(battle, request);
        AddMonsterWaves(battle, request, waves, stage);
        BattleTargets.Apply(battle, request);

        return battle;
    }

    private void BuildAvatars(SceneBattleInfo battle, BattleRequest request, SrToolsData player)
    {
        var index = 0u;

        foreach (var avatarId in request.Lineup.Where(id => id != 0))
        {
            var slot = index++;
            battle.BattleAvatarList.Add(BuildAvatar(avatarId, slot, request, player));

            player.Avatars.TryGetValue(avatarId, out var configured);

            // srtools writes the exact technique buff ids it wants; otherwise derive them
            if (configured is { Techniques.Count: > 0 })
            {
                foreach (var id in configured.Techniques)
                {
                    battle.BuffList.Add(TechniqueBuff(id, slot, 2f));
                }
            }
            else
            {
                foreach (var buff in TechniqueBuffs.For(data, avatarId))
                {
                    battle.BuffList.Add(
                        TechniqueBuff(buff.Id, buff.OwnedByLeader ? request.LeaderSlot : slot, buff.SkillIndex));
                }
            }
        }
    }

    // trial characters come fully specified by SpecialAvatar; the roster is not consulted
    private void BuildTrialAvatars(SceneBattleInfo battle, BattleRequest request)
    {
        var index = 0u;

        foreach (var special in request.TrialAvatars)
        {
            if (!MatchesGender(special.AvatarID, request.Gender) || !data.Avatars.ContainsKey(special.AvatarID))
            {
                continue;
            }

            var avatar = new BattleAvatar
            {
                Index = index++,
                Id = special.AvatarID,
                AvatarType = AvatarType.AvatarTrialType,
                Level = special.Level == 0 ? 80 : special.Level,
                Promotion = special.Promotion,
                Rank = 0,
                Hp = Player.FullHp,
                WorldLevel = request.WorldLevel,
                SpBar = new SpBarInfo { CurSp = 5_000, MaxSp = 10_000 },
            };

            foreach (var (pointId, level) in data.MaxedSkillTree(special.AvatarID))
            {
                avatar.SkilltreeList.Add(new AvatarSkillTree { PointId = pointId, Level = level });
            }

            if (special.EquipmentID != 0)
            {
                avatar.EquipmentList.Add(new BattleEquipment
                {
                    Id = special.EquipmentID,
                    Level = special.EquipmentLevel,
                    Promotion = special.EquipmentPromotion,
                    Rank = Math.Max(1, special.EquipmentRank),
                });
            }

            battle.BattleAvatarList.Add(avatar);

            foreach (var buff in TechniqueBuffs.For(data, special.AvatarID))
            {
                battle.BuffList.Add(TechniqueBuff(buff.Id, buff.OwnedByLeader ? 0 : avatar.Index, buff.SkillIndex));
            }
        }
    }

    // trailblazer trial rows come in pairs; keep the one matching the player's gender
    private static bool MatchesGender(uint avatarId, Gender gender) =>
        avatarId is < 8001 or > 8010 || (avatarId % 2 == 1) == (gender == Gender.Man);

    private static BattleBuff TechniqueBuff(uint id, uint ownerIndex, float skillIndex) => new()
    {
        Id = id,
        Level = 1,
        OwnerIndex = ownerIndex,
        WaveFlag = AllWaves,
        TargetIndexList = { 0u },
        DynamicValues = { ["SkillIndex"] = skillIndex },
    };

    private BattleAvatar BuildAvatar(uint avatarId, uint index, BattleRequest request, SrToolsData player)
    {
        player.Avatars.TryGetValue(avatarId, out var configured);
        var state = request.StateOf?.Invoke(avatarId);

        var avatar = new BattleAvatar
        {
            Index = index,
            Id = avatarId,
            AvatarType = AvatarType.AvatarFormalType,
            Level = configured?.Level ?? 80,
            Promotion = configured?.Promotion ?? 6,
            Rank = configured?.Data.Rank ?? 6,
            Hp = state?.Hp ?? Player.FullHp,
            WorldLevel = request.WorldLevel,
            SpBar = state?.SpBar ?? new SpBarInfo
            {
                CurSp = configured?.SpValue ?? 10_000,
                MaxSp = configured?.SpMax ?? 10_000,
            },
        };

        var enhanced = request.IsEnhanced is null
            ? configured?.EnhancedId is > 0
            : request.IsEnhanced(avatarId);

        // only avatars that actually have an enhanced kit can enter with one
        if (enhanced && (configured?.EnhancedId is > 0 || data.EnhancedAvatars.ContainsKey(avatarId)))
        {
            avatar.EnhancedId = configured?.EnhancedId is > 0
                ? configured.EnhancedId.Value
                : Roster.EnhancedId(data, avatarId);
        }

        // srtools carries the player's actual trace levels; otherwise give a fully built kit
        if (configured is { Data.Skills.Count: > 0 })
        {
            foreach (var (pointId, level) in configured.Data.Skills)
            {
                avatar.SkilltreeList.Add(new AvatarSkillTree { PointId = pointId, Level = level });
            }
        }
        else
        {
            foreach (var (pointId, level) in data.MaxedSkillTree(avatarId))
            {
                avatar.SkilltreeList.Add(new AvatarSkillTree { PointId = pointId, Level = level });
            }
        }

        if (player.LightconeFor(avatarId) is { } lightcone)
        {
            avatar.EquipmentList.Add(new BattleEquipment
            {
                Id = lightcone.ItemId,
                Level = lightcone.Level,
                Promotion = lightcone.Promotion,
                Rank = lightcone.Rank,
            });
        }

        foreach (var relic in player.RelicsFor(avatarId))
        {
            var battleRelic = new BattleRelic
            {
                Id = relic.RelicId,
                Level = relic.Level,
                MainAffixId = relic.MainAffixId,
                UniqueId = relic.UniqueId,
            };

            foreach (var affix in relic.SubAffixes)
            {
                battleRelic.SubAffixList.Add(new RelicAffix
                {
                    AffixId = affix.SubAffixId,
                    Cnt = affix.Count,
                    Step = affix.Step,
                });
            }

            avatar.RelicList.Add(battleRelic);
        }

        return avatar;
    }

    // who struck first decides how the fight opens: the attacker's element breaks toughness,
    // an ambushing monster gets the drop on the party instead
    private void AddEntryBuff(SceneBattleInfo battle, BattleRequest request, SrToolsData player, bool trial)
    {
        if (request.Ambush)
        {
            battle.BuffList.Add(new BattleBuff
            {
                Id = AmbushBuff,
                Level = 1,
                OwnerIndex = NoOwner,
                WaveFlag = FirstWave,
            });

            return;
        }

        if (!request.PlayerInitiated)
        {
            return;
        }

        var avatars = trial
            ? battle.BattleAvatarList.Select(a => a.Id).ToList()
            : request.Lineup.Where(id => id != 0).ToList();

        var attacker = avatars.ElementAtOrDefault((int)request.LeaderSlot);
        if (attacker == 0 || TechniqueBuffs.IgnoresToughnessOnEntry(attacker))
        {
            return;
        }

        var element = data.GetAvatar(attacker)?.DamageType ?? string.Empty;
        var buffId = TechniqueBuffs.AttackerBuff(element);

        if (buffId == 0)
        {
            return;
        }

        battle.BuffList.Add(new BattleBuff
        {
            Id = buffId,
            Level = 1,
            OwnerIndex = request.LeaderSlot,
            WaveFlag = AllWaves,
            TargetIndexList = { 0u },
            DynamicValues = { ["SkillIndex"] = request.SkillIndex },
        });
    }

    private static void AddBlessings(SceneBattleInfo battle, BattleRequest request)
    {
        foreach (var blessing in request.Blessings)
        {
            var buff = new BattleBuff
            {
                Id = blessing.Id,
                Level = blessing.Level == 0 ? 1 : blessing.Level,
                OwnerIndex = NoOwner,
                WaveFlag = AllWaves,
                TargetIndexList = { 0u },
            };

            if (blessing.DynamicKey is { } key && key.Key.Length > 0)
            {
                buff.DynamicValues[key.Key] = key.Value;
            }

            foreach (var value in blessing.DynamicValues)
            {
                buff.DynamicValues.TryAdd(value.Key, value.Value);
            }

            buff.DynamicValues.TryAdd("SkillIndex", 0f);
            battle.BuffList.Add(buff);
        }
    }

    // ADV_GlobalSkill_Maze buffs apply when the account owns the avatar, lineup or not
    private void AddGlobalBuffs(SceneBattleInfo battle, SrToolsData player)
    {
        foreach (var (avatarId, buffId) in data.GlobalMazeBuffs)
        {
            if (!player.Avatars.ContainsKey(avatarId) || battle.BuffList.Any(b => b.Id == buffId))
            {
                continue;
            }

            battle.BuffList.Add(new BattleBuff
            {
                Id = buffId,
                Level = 1,
                OwnerIndex = NoOwner,
                WaveFlag = AllWaves,
            });
        }
    }

    // srtools' custom stats ride on the first relic of every avatar
    private static void ApplyCustomStats(SceneBattleInfo battle, BattleRequest request)
    {
        if (request.CustomStats.Count == 0)
        {
            return;
        }

        foreach (var avatar in battle.BattleAvatarList)
        {
            if (avatar.RelicList.Count == 0)
            {
                avatar.RelicList.Add(new BattleRelic { Id = 61011, MainAffixId = 1, Level = 1 });
            }

            var relic = avatar.RelicList[0];

            foreach (var stat in request.CustomStats)
            {
                var existing = relic.SubAffixList.FirstOrDefault(a => a.AffixId == stat.SubAffixId);

                if (existing is not null)
                {
                    existing.Cnt = stat.Count;
                    existing.Step = stat.Step;
                }
                else
                {
                    relic.SubAffixList.Add(new RelicAffix { AffixId = stat.SubAffixId, Cnt = stat.Count, Step = stat.Step });
                }
            }
        }
    }

    // the invading monster on some PF floors rides in as a stage buff
    private void AddStageInvasion(SceneBattleInfo battle, BattleRequest request)
    {
        if (!data.StageInvasions.TryGetValue(request.StageId, out var invasion) || invasion.InvasionID == 0)
        {
            return;
        }

        battle.BuffList.Add(new BattleBuff
        {
            Id = 3034000 + invasion.InvasionID,
            Level = 1,
            OwnerIndex = request.LeaderSlot,
            WaveFlag = AllWaves,
            TargetIndexList = { 0u },
            DynamicValues = { ["SkillIndex"] = 1f },
        });
    }

    private static void AddPathResonance(SceneBattleInfo battle, BattleRequest request)
    {
        if (request.PathResonanceId == 0)
        {
            return;
        }

        battle.BattleEvent.Add(new BattleEventBattleInfo
        {
            BattleEventId = request.PathResonanceId,
            Status = new BattleEventProperty
            {
                SpBar = new SpBarInfo { CurSp = 10_000, MaxSp = 10_000 },
            },
        });
    }

    private List<(uint StageId, List<SrMonster> Monsters, StageExcel? Stage)> ResolveWaves(BattleRequest request, StageExcel? stage)
    {
        var waves = new List<(uint, List<SrMonster>, StageExcel?)>();

        if (request.MonsterWaves is { Count: > 0 })
        {
            foreach (var wave in request.MonsterWaves)
            {
                waves.Add((request.StageId, wave, stage));
            }

            return waves;
        }

        foreach (var (stageId, excel) in Stages(request, stage))
        {
            var level = request.MonsterLevel > 0 ? request.MonsterLevel : excel.Level;

            foreach (var wave in excel.MonsterList)
            {
                waves.Add((stageId, [.. wave.Select(id => new SrMonster { MonsterId = id, Level = level })], excel));
            }
        }

        return waves;
    }

    private IEnumerable<(uint StageId, StageExcel Stage)> Stages(BattleRequest request, StageExcel? first)
    {
        if (first is not null)
        {
            yield return (request.StageId, first);
        }

        foreach (var stageId in request.ExtraStageIds.Where(id => id != request.StageId).Distinct())
        {
            if (data.GetStage(stageId) is { } stage)
            {
                yield return (stageId, stage);
            }
        }
    }

    private static void AddMonsterWaves(
        SceneBattleInfo battle, BattleRequest request,
        List<(uint StageId, List<SrMonster> Monsters, StageExcel? Stage)> waves, StageExcel? firstStage)
    {
        for (var i = 0; i < waves.Count; i++)
        {
            var (stageId, wave, stage) = waves[i];
            stage ??= firstStage;

            var level = request.MonsterLevel > 0
                ? request.MonsterLevel
                : wave.Count > 0 ? wave.Max(m => m.Level) : stage?.Level ?? 95;

            var sceneWave = new SceneMonsterWave
            {
                BattleWaveId = (uint)i + 1,
                BattleStageId = stageId,
                MonsterParam = new SceneMonsterWaveParam
                {
                    Level = level,
                    HardLevelGroup = stage?.HardLevelGroup ?? 1,
                    EliteGroup = stage?.EliteGroup ?? 0,
                },
            };

            foreach (var monster in wave)
            {
                for (var n = 0; n < Math.Max(1, (int)monster.Amount); n++)
                {
                    sceneWave.MonsterList.Add(new SceneMonster
                    {
                        MonsterId = monster.MonsterId,
                        MaxHp = monster.MaxHp,
                        CurHp = monster.MaxHp,
                    });
                }
            }

            battle.MonsterWaveList.Add(sceneWave);
        }
    }
}
