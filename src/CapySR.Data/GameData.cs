using System.Diagnostics;
using System.Text.Json;
using System.Text.Json.Nodes;
using CapySR.Data.Excel;

namespace CapySR.Data;

// sources are listed in priority order and merged per row id: the first source to define a
// row keeps it, later ones only fill gaps. beta dumps are trimmed extracts, so letting them
// replace full prod rows would silently drop fields like StageType.
public sealed partial class GameData
{
    private static readonly JsonSerializerOptions NodeOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        NumberHandling = System.Text.Json.Serialization.JsonNumberHandling.AllowReadingFromString,
    };

    private readonly string[] _sources;

    private GameData(string[] sources) => _sources = sources;

    public IReadOnlyList<string> Sources => _sources;

    public IReadOnlyDictionary<uint, AvatarExcel> Avatars { get; private set; } = new Dictionary<uint, AvatarExcel>();

    public IReadOnlyDictionary<uint, List<AvatarSkillTreeExcel>> SkillTrees { get; private set; } =
        new Dictionary<uint, List<AvatarSkillTreeExcel>>();

    public IReadOnlyDictionary<uint, AvatarDefaultMazeBuffExcel> DefaultMazeBuffs { get; private set; } =
        new Dictionary<uint, AvatarDefaultMazeBuffExcel>();

    public IReadOnlyList<AvatarGlobalBuffExcel> GlobalBuffs { get; private set; } = [];

    public IReadOnlyDictionary<uint, StageExcel> Stages { get; private set; } = new Dictionary<uint, StageExcel>();

    public IReadOnlyDictionary<uint, MonsterExcel> Monsters { get; private set; } = new Dictionary<uint, MonsterExcel>();

    public IReadOnlyDictionary<uint, EquipmentExcel> Equipment { get; private set; } = new Dictionary<uint, EquipmentExcel>();

    public IReadOnlyDictionary<uint, uint> EnhancedAvatars { get; private set; } = new Dictionary<uint, uint>();

    public IReadOnlyDictionary<uint, AvatarMazeBuffExcel> MazeBuffs { get; private set; } =
        new Dictionary<uint, AvatarMazeBuffExcel>();

    // (avatar, buff) pairs that apply from the roster rather than the lineup
    public IReadOnlyList<(uint AvatarId, uint BuffId)> GlobalMazeBuffs { get; private set; } = [];

    public IReadOnlyDictionary<uint, ChallengeMazeExcel> Challenges { get; private set; } =
        new Dictionary<uint, ChallengeMazeExcel>();

    public IReadOnlyDictionary<uint, ChallengeStoryExtraExcel> ChallengeStoryExtras { get; private set; } =
        new Dictionary<uint, ChallengeStoryExtraExcel>();

    public IReadOnlyDictionary<uint, ChallengeBossExtraExcel> ChallengeBossExtras { get; private set; } =
        new Dictionary<uint, ChallengeBossExtraExcel>();

    public IReadOnlyDictionary<uint, ChallengeTargetExcel> ChallengeTargets { get; private set; } =
        new Dictionary<uint, ChallengeTargetExcel>();

    public IReadOnlyDictionary<uint, ChallengeGroupExcel> ChallengeGroups { get; private set; } =
        new Dictionary<uint, ChallengeGroupExcel>();

    public IReadOnlyDictionary<uint, BattleTargetExcel> BattleTargets { get; private set; } =
        new Dictionary<uint, BattleTargetExcel>();

    public IReadOnlyDictionary<uint, ChallengePeakExcel> ChallengePeaks { get; private set; } =
        new Dictionary<uint, ChallengePeakExcel>();

    public IReadOnlyDictionary<uint, ChallengePeakBossExcel> ChallengePeakBosses { get; private set; } =
        new Dictionary<uint, ChallengePeakBossExcel>();

    public IReadOnlyDictionary<uint, MapEntranceExcel> MapEntrances { get; private set; } =
        new Dictionary<uint, MapEntranceExcel>();

    public IReadOnlyDictionary<uint, MazePlaneExcel> MazePlanes { get; private set; } =
        new Dictionary<uint, MazePlaneExcel>();

    public IReadOnlyDictionary<uint, ChallengePeakGroupExcel> ChallengePeakGroups { get; private set; } =
        new Dictionary<uint, ChallengePeakGroupExcel>();

    public IReadOnlyDictionary<uint, SceneResourceExcel> Scenes { get; private set; } =
        new Dictionary<uint, SceneResourceExcel>();

    public IReadOnlyDictionary<uint, AnchorExcel> Anchors { get; private set; } =
        new Dictionary<uint, AnchorExcel>();

    public IReadOnlyDictionary<uint, FloorSavedValuesExcel> FloorSavedValues { get; private set; } =
        new Dictionary<uint, FloorSavedValuesExcel>();

    public IReadOnlyDictionary<uint, MainMissionExcel> MainMissions { get; private set; } =
        new Dictionary<uint, MainMissionExcel>();

    public IReadOnlyDictionary<uint, CocoonExcel> Cocoons { get; private set; } = new Dictionary<uint, CocoonExcel>();

    public IReadOnlyDictionary<uint, FarmElementExcel> FarmElements { get; private set; } =
        new Dictionary<uint, FarmElementExcel>();

    public IReadOnlyDictionary<uint, PlaneEventExcel> PlaneEvents { get; private set; } =
        new Dictionary<uint, PlaneEventExcel>();

    public IReadOnlyDictionary<uint, BattleCollegeExcel> BattleColleges { get; private set; } =
        new Dictionary<uint, BattleCollegeExcel>();

    public IReadOnlyDictionary<uint, SpecialAvatarExcel> SpecialAvatars { get; private set; } =
        new Dictionary<uint, SpecialAvatarExcel>();

    public IReadOnlyDictionary<uint, NpcMonsterExcel> NpcMonsters { get; private set; } =
        new Dictionary<uint, NpcMonsterExcel>();

    public IReadOnlyDictionary<uint, MazePropExcel> Props { get; private set; } = new Dictionary<uint, MazePropExcel>();

    public IReadOnlyDictionary<uint, HeadIconExcel> HeadIcons { get; private set; } = new Dictionary<uint, HeadIconExcel>();

    public IReadOnlyDictionary<uint, StageInvasionExcel> StageInvasions { get; private set; } = new Dictionary<uint, StageInvasionExcel>();

    public IReadOnlySet<uint> DestructiblePropIds { get; private set; } = new HashSet<uint>();

    public IReadOnlyList<uint> TutorialIds { get; private set; } = [];

    public IReadOnlyList<uint> TutorialGuideIds { get; private set; } = [];

    // every relic / lightcone id the client can resolve a type for
    public IReadOnlySet<uint> KnownRelicIds { get; private set; } = new HashSet<uint>();

    public IReadOnlySet<uint> KnownLightconeIds { get; private set; } = new HashSet<uint>();

    // path avatar id -> the base id the client files it under (8002 -> 8001, 1224 -> 1001)
    public IReadOnlyDictionary<uint, uint> BaseAvatarIds { get; private set; } = new Dictionary<uint, uint>();

    public IReadOnlySet<uint> MultiPathAvatarIds { get; private set; } = new HashSet<uint>();

    public static GameData Load(IEnumerable<string> sources, Action<string>? log = null)
    {
        log ??= _ => { };

        var roots = sources.Where(Directory.Exists).ToArray();
        if (roots.Length == 0)
        {
            throw new DirectoryNotFoundException(
                "no game data sources found. Clone Dimbreath/turnbasedgamedata and point " +
                "Data:Sources at its ExcelOutput folder.");
        }

        var data = new GameData(roots);
        var stopwatch = Stopwatch.StartNew();

        data.Avatars = data.Merge<AvatarExcel>("AvatarConfig.json", r => UInt(r, "AvatarID"));
        data.DefaultMazeBuffs = data.Merge<AvatarDefaultMazeBuffExcel>("AvatarDefaultMazeBuff.json", r => UInt(r, "ID"), required: false);
        data.Stages = data.Merge<StageExcel>("StageConfig.json", r => UInt(r, "StageID"));
        data.Monsters = data.Merge<MonsterExcel>("MonsterConfig.json", r => UInt(r, "MonsterID"), required: false);
        data.Equipment = data.Merge<EquipmentExcel>("EquipmentConfig.json", r => UInt(r, "EquipmentID"), required: false);

        data.SkillTrees = data
            // one row per (point, level)
            .Merge<AvatarSkillTreeExcel>("AvatarSkillTreeConfig.json", r => (UInt(r, "PointID") * 100u) + (UInt(r, "Level") ?? 0))
            .Values
            .GroupBy(p => p.AvatarID)
            .ToDictionary(g => g.Key, g => g.ToList());

        data.GlobalBuffs = [.. data.Merge<AvatarGlobalBuffExcel>("AvatarGlobalBuffConfig.json", r => UInt(r, "AvatarID"), required: false).Values];

        data.EnhancedAvatars = data
            .Merge<AvatarConfigEnhancedExcel>("AvatarConfigEnhanced.json", r => UInt(r, "AvatarID"), required: false)
            .ToDictionary(kv => kv.Key, kv => kv.Value.EnhancedID == 0 ? 1u : kv.Value.EnhancedID);

        data.MazeBuffs = data.Merge<AvatarMazeBuffExcel>("AvatarMazeBuff.json", r => UInt(r, "ID"), required: false);

        // prod splits the three modes into three files with one shape; the beta dump merges them
        var challenges = data.Merge<ChallengeMazeExcel>("ChallengeMazeConfig.json", r => UInt(r, "ID"), required: false);
        foreach (var file in new[] { "ChallengeStoryMazeConfig.json", "ChallengeBossMazeConfig.json" })
        {
            foreach (var (id, row) in data.Merge<ChallengeMazeExcel>(file, r => UInt(r, "ID"), required: false))
            {
                challenges.TryAdd(id, row);
            }
        }

        data.Challenges = challenges;
        data.ChallengeStoryExtras = data.Merge<ChallengeStoryExtraExcel>("ChallengeStoryMazeExtra.json", r => UInt(r, "ID"), required: false);
        data.ChallengeBossExtras = data.Merge<ChallengeBossExtraExcel>("ChallengeBossMazeExtra.json", r => UInt(r, "ID"), required: false);
        data.ChallengeGroups = data.Merge<ChallengeGroupExcel>("ChallengeGroupConfig.json", r => UInt(r, "GroupID"), required: false);

        var targets = data.Merge<ChallengeTargetExcel>("ChallengeTargetConfig.json", r => UInt(r, "ID"), required: false);
        foreach (var file in new[] { "ChallengeStoryTargetConfig.json", "ChallengeBossTargetConfig.json" })
        {
            foreach (var (id, row) in data.Merge<ChallengeTargetExcel>(file, r => UInt(r, "ID"), required: false))
            {
                targets.TryAdd(id, row);
            }
        }

        data.ChallengeTargets = targets;
        data.BattleTargets = data.Merge<BattleTargetExcel>("BattleTargetConfig.json", r => UInt(r, "ID"), required: false);

        data.ChallengePeaks = data.Merge<ChallengePeakExcel>("ChallengePeakConfig.json", r => UInt(r, "ID"), required: false);
        data.ChallengePeakBosses = data.Merge<ChallengePeakBossExcel>("ChallengePeakBossConfig.json", r => UInt(r, "ID"), required: false);
        data.MapEntrances = data.Merge<MapEntranceExcel>("MapEntrance.json", r => UInt(r, "ID"), required: false);
        data.ChallengePeakGroups = data.Merge<ChallengePeakGroupExcel>("ChallengePeakGroupConfig.json", r => UInt(r, "ID"), required: false);
        data.MazePlanes = data.Merge<MazePlaneExcel>("MazePlane.json", r => UInt(r, "PlaneID"), required: false);
        data.Scenes = data.Merge<SceneResourceExcel>("res.json", r => UInt(r, "entryID"), required: false);
        data.Anchors = data.Merge<AnchorExcel>("Anchor.json", r => UInt(r, "entryID"), required: false);
        data.FloorSavedValues = data.Merge<FloorSavedValuesExcel>("FloorSavedValuesConfig.json", r => UInt(r, "FloorID"), required: false);
        data.MainMissions = data.Merge<MainMissionExcel>("MainMission.json", r => UInt(r, "MainMissionID"), required: false);
        data.TutorialIds = [.. data.Merge<TutorialExcel>("TutorialData.json", r => UInt(r, "TutorialID"), required: false).Keys];
        data.TutorialGuideIds = [.. data.Merge<TutorialGuideGroupExcel>("TutorialGuideGroup.json", r => UInt(r, "GroupID"), required: false).Keys];
        data.KnownRelicIds = data.Merge<ItemConfigExcel>("ItemConfigRelic.json", r => UInt(r, "ID"), required: false).Keys.ToHashSet();
        data.KnownLightconeIds = data.Merge<ItemConfigExcel>("ItemConfigEquipment.json", r => UInt(r, "ID"), required: false).Keys.ToHashSet();

        data.Cocoons = data.Merge<CocoonExcel>("CocoonConfig.json", r => (UInt(r, "ID") * 10) + (UInt(r, "WorldLevel") ?? 0), required: false);
        data.FarmElements = data.Merge<FarmElementExcel>("FarmElementConfig.json", r => (UInt(r, "ID") * 10) + (UInt(r, "WorldLevel") ?? 0), required: false);
        data.PlaneEvents = data.Merge<PlaneEventExcel>("PlaneEvent.json", r => (UInt(r, "EventID") * 10) + (UInt(r, "WorldLevel") ?? 0), required: false);
        data.BattleColleges = data.Merge<BattleCollegeExcel>("BattleCollegeConfig.json", r => UInt(r, "ID"), required: false);
        data.SpecialAvatars = data.Merge<SpecialAvatarExcel>("SpecialAvatar.json", r => UInt(r, "SpecialAvatarID"), required: false);
        data.NpcMonsters = data.Merge<NpcMonsterExcel>("NPCMonsterData.json", r => UInt(r, "ID"), required: false);
        data.Props = data.Merge<MazePropExcel>("MazeProp.json", r => UInt(r, "ID"), required: false);
        data.HeadIcons = data.Merge<HeadIconExcel>("AvatarPlayerIcon.json", r => UInt(r, "ID"), required: false);
        data.StageInvasions = data.Merge<StageInvasionExcel>("StageInvasionConfig.json", r => UInt(r, "StageID"), required: false);
        data.DestructiblePropIds = data.Props.Values.Where(p => p.IsDestructible).Select(p => p.ID).ToHashSet();

        var multiPath = data.Merge<MultiPathAvatarExcel>("MultiplePathAvatarConfig.json", r => UInt(r, "AvatarID"), required: false);
        data.BaseAvatarIds = multiPath.ToDictionary(kv => kv.Key, kv => kv.Value.BaseAvatarID == 0 ? kv.Key : kv.Value.BaseAvatarID);
        data.MultiPathAvatarIds = multiPath.Keys.ToHashSet();

        data.GlobalMazeBuffs = [.. data.MazeBuffs.Values
            .Where(b => b.IsGlobal)
            .Select(b => (b.OwnerAvatarId, b.ID))
            .OrderBy(b => b.Item1)];

        log($"game data loaded in {stopwatch.ElapsedMilliseconds} ms from {roots.Length} source(s): " +
            $"{data.Avatars.Count} avatars, {data.SkillTrees.Sum(t => t.Value.Count)} skill tree points, " +
            $"{data.Stages.Count} stages, {data.Monsters.Count} monsters, {data.Equipment.Count} lightcones, " +
            $"{data.Challenges.Count} challenge floors, {data.ChallengePeaks.Count(p => p.Value.IsPlayable)}/" +
            $"{data.ChallengePeaks.Count} playable AA stages, {data.Scenes.Count} scenes, " +
            $"{data.Cocoons.Count} calyx rows, {data.FarmElements.Count} farm rows, {data.PlaneEvents.Count} plane events, " +
            $"{data.MainMissions.Count} main missions, " +
            $"{data.TutorialIds.Count} tutorials / {data.TutorialGuideIds.Count} guides");

        return data;
    }

    public AvatarExcel? GetAvatar(uint avatarId) => Avatars.GetValueOrDefault(avatarId);

    public StageExcel? GetStage(uint stageId) => Stages.GetValueOrDefault(stageId);

    public ChallengeMazeExcel? GetChallenge(uint challengeId) => Challenges.GetValueOrDefault(challengeId);

    public uint BaseAvatarId(uint avatarId) => BaseAvatarIds.TryGetValue(avatarId, out var baseId)
        ? baseId
        : avatarId switch
        {
            >= 8001 and <= 8010 => 8001,
            1224 => 1001,
            _ => avatarId,
        };

    // the row for this world level, else the closest lower one, else any
    public CocoonExcel? FindCocoon(uint cocoonId, uint worldLevel)
    {
        for (var level = (int)Math.Min(worldLevel, 9); level >= 0; level--)
        {
            if (Cocoons.TryGetValue((cocoonId * 10) + (uint)level, out var row) && row.Stages.Count > 0)
            {
                return row;
            }
        }

        return Cocoons.Values.FirstOrDefault(c => c.ID == cocoonId && c.Stages.Count > 0);
    }

    public FarmElementExcel? FindFarmElement(uint farmId, uint worldLevel)
    {
        for (var level = (int)Math.Min(worldLevel, 9); level >= 0; level--)
        {
            if (FarmElements.TryGetValue((farmId * 10) + (uint)level, out var row) && row.StageID != 0)
            {
                return row;
            }
        }

        return FarmElements.Values.FirstOrDefault(f => f.ID == farmId && f.StageID != 0);
    }

    // an overworld monster's event -> the stage it fights at this world level
    public uint ResolveStageForEvent(uint eventId, uint worldLevel)
    {
        if (eventId == 0)
        {
            return 0;
        }

        for (var level = (int)Math.Min(worldLevel, 9); level >= 0; level--)
        {
            if (PlaneEvents.TryGetValue((eventId * 10) + (uint)level, out var planeEvent) && planeEvent.StageID != 0)
            {
                return planeEvent.StageID;
            }
        }

        if (Stages.ContainsKey((eventId * 10) + worldLevel))
        {
            return (eventId * 10) + worldLevel;
        }

        if (Stages.ContainsKey(eventId))
        {
            return eventId;
        }

        return 0;
    }

    public bool IsFodderMonster(uint npcMonsterId) =>
        NpcMonsters.TryGetValue(npcMonsterId, out var monster) && monster.IsFodder;

    // monster placement lives per plane, matched by the challenge's maze group
    public ScenePlacement? FindMonsterPlacement(uint planeId, uint groupId)
    {
        foreach (var scene in Scenes.Values.Where(s => s.PlaneID == planeId))
        {
            foreach (var monster in scene.Monsters.Where(m => m.GroupId == groupId))
            {
                return monster;
            }
        }

        return null;
    }

    public SceneResourceExcel? GetScene(uint entryId) => Scenes.GetValueOrDefault(entryId);

    // teleport ids are shared by every entry on a plane, so a point picked on the map is
    // often not listed under the entry the client asks to enter
    public SceneTeleport? FindTeleport(uint entryId, uint teleportId)
    {
        if (teleportId == 0)
        {
            return null;
        }

        var scene = GetScene(entryId);

        if (scene?.Teleports.FirstOrDefault(t => t.TeleportId == teleportId) is { } exact)
        {
            return exact;
        }

        var planeId = scene?.PlaneID ?? ResolveEntrance(entryId)?.Entrance.PlaneID ?? 0;

        return Scenes.Values
            .Where(s => s.PlaneID == planeId)
            .SelectMany(s => s.Teleports)
            .FirstOrDefault(t => t.TeleportId == teleportId);
    }

    public IEnumerable<uint> TeleportIdsForEntry(uint entryId) =>
        TeleportIdsForPlane(GetScene(entryId)?.PlaneID ?? ResolveEntrance(entryId)?.Entrance.PlaneID ?? 0);

    // floor ids are the plane id with a three digit suffix
    public IEnumerable<uint> TeleportIdsForFloor(uint floorId) => TeleportIdsForPlane(floorId / 1000);

    private IEnumerable<uint> TeleportIdsForPlane(uint planeId) =>
        Scenes.Values
            .Where(s => s.PlaneID == planeId)
            .SelectMany(s => s.Teleports)
            .Select(t => t.TeleportId)
            .Distinct();

    public AnchorPoint? FindAnchor(uint entryId) =>
        Anchors.TryGetValue(entryId, out var anchor) && anchor.Anchor.Count > 0 ? anchor.Anchor[0] : null;

    // an entrance points at a floor; the plane it belongs to carries the world id
    public (MapEntranceExcel Entrance, MazePlaneExcel? Plane)? ResolveEntrance(uint entranceId)
    {
        if (!MapEntrances.TryGetValue(entranceId, out var entrance))
        {
            return null;
        }

        var plane = MazePlanes.GetValueOrDefault(entrance.PlaneID)
                    ?? MazePlanes.Values.FirstOrDefault(p => p.FloorIDList.Contains(entrance.FloorID));

        return (entrance, plane);
    }

    // AvatarPathData.point_id is the anchor index ("Point01" -> 1), not the raw point id
    public IEnumerable<(uint AnchorIndex, uint Level)> MaxedPathSkillTree(uint avatarId)
    {
        if (!SkillTrees.TryGetValue(avatarId, out var points))
        {
            yield break;
        }

        foreach (var group in points.Where(p => p.AnchorIndex > 0).GroupBy(p => p.AnchorIndex))
        {
            var best = group.MaxBy(p => p.Level)!;
            yield return (group.Key, best.MaxLevel > 0 ? best.MaxLevel : best.Level);
        }
    }

    public IEnumerable<(uint PointId, uint Level)> MaxedSkillTree(uint avatarId)
    {
        if (!SkillTrees.TryGetValue(avatarId, out var points))
        {
            yield break;
        }

        foreach (var group in points.GroupBy(p => p.PointID))
        {
            var best = group.MaxBy(p => p.Level)!;
            yield return (best.PointID, best.MaxLevel > 0 ? best.MaxLevel : best.Level);
        }
    }
}
