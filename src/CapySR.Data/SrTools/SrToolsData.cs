using System.Text.Json;
using System.Text.Json.Serialization;

namespace CapySR.Data.SrTools;

// freesr-data.json, as written by srtools.neonteam.dev
public sealed class SrToolsData
{
    public Dictionary<uint, SrAvatar> Avatars { get; set; } = [];

    public List<SrRelic> Relics { get; set; } = [];

    public List<SrLightcone> Lightcones { get; set; } = [];

    [JsonPropertyName("battle_config")]
    public SrBattleConfig BattleConfig { get; set; } = new();

    public static SrToolsData Empty { get; } = new();

    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNameCaseInsensitive = true,
        NumberHandling = JsonNumberHandling.AllowReadingFromString,
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
    };

    public const string FileName = "freesr-data.json";

    public static SrToolsData Load(string path) =>
        File.Exists(path)
            ? JsonSerializer.Deserialize<SrToolsData>(File.ReadAllText(path), Options) ?? new SrToolsData()
            : new SrToolsData();

    // robinsr and srtools users drop it at the repo root; we also accept data/
    public static string ResolvePath(string root)
    {
        var atRoot = Path.Combine(root, FileName);
        var inData = Path.Combine(root, "data", FileName);

        return File.Exists(atRoot) || !File.Exists(inData) ? atRoot : inData;
    }

    public SrLightcone? LightconeFor(uint avatarId) => Lightcones.FirstOrDefault(l => l.EquipAvatar == avatarId);

    public IEnumerable<SrRelic> RelicsFor(uint avatarId) => Relics.Where(r => r.EquipAvatar == avatarId);
}

public sealed class SrAvatar
{
    [JsonPropertyName("avatar_id")]
    public uint AvatarId { get; set; }

    public uint Level { get; set; } = 80;

    public uint Promotion { get; set; } = 6;

    public SrAvatarData Data { get; set; } = new();

    // the buff ids the avatar enters battle with
    [JsonPropertyName("techniques")]
    public List<uint> Techniques { get; set; } = [];

    [JsonPropertyName("sp_value")]
    public uint? SpValue { get; set; }

    [JsonPropertyName("sp_max")]
    public uint? SpMax { get; set; }

    [JsonPropertyName("enhanced_id")]
    public uint? EnhancedId { get; set; }
}

public sealed class SrAvatarData
{
    public uint Rank { get; set; }

    // point id -> level
    public Dictionary<uint, uint> Skills { get; set; } = [];

    [JsonPropertyName("skills_by_anchor_type")]
    public Dictionary<uint, uint> SkillsByAnchorType { get; set; } = [];
}

public sealed class SrRelic
{
    public uint Level { get; set; }

    [JsonPropertyName("relic_id")]
    public uint RelicId { get; set; }

    [JsonPropertyName("main_affix_id")]
    public uint MainAffixId { get; set; }

    [JsonPropertyName("sub_affixes")]
    public List<SrSubAffix> SubAffixes { get; set; } = [];

    [JsonPropertyName("internal_uid")]
    public uint InternalUid { get; set; }

    [JsonPropertyName("equip_avatar")]
    public uint EquipAvatar { get; set; }

    public uint Slot => RelicId % 10;

    // srtools numbers from 0, but 0 means "no item" on the wire
    public uint UniqueId => InternalUid + 1;
}

public sealed class SrSubAffix
{
    [JsonPropertyName("sub_affix_id")]
    public uint SubAffixId { get; set; }

    public uint Count { get; set; }

    public uint Step { get; set; }
}

public sealed class SrLightcone
{
    public uint Level { get; set; }

    [JsonPropertyName("item_id")]
    public uint ItemId { get; set; }

    [JsonPropertyName("equip_avatar")]
    public uint EquipAvatar { get; set; }

    public uint Rank { get; set; }

    public uint Promotion { get; set; }

    [JsonPropertyName("internal_uid")]
    public uint InternalUid { get; set; }

    // lightcones live in a separate unique-id range from relics
    public uint UniqueId => 3001 + InternalUid;
}

public sealed class SrBattleConfig
{
    [JsonPropertyName("battle_type")]
    public string BattleType { get; set; } = "Default";

    [JsonPropertyName("stage_id")]
    public uint StageId { get; set; }

    [JsonPropertyName("cycle_count")]
    public uint CycleCount { get; set; } = 30;

    public List<List<SrMonster>> Monsters { get; set; } = [];

    public List<SrBlessing> Blessings { get; set; } = [];

    [JsonPropertyName("path_resonance_id")]
    public uint PathResonanceId { get; set; }

    [JsonPropertyName("custom_stats")]
    public List<SrSubAffix> CustomStats { get; set; } = [];

    [JsonPropertyName("custom_battle_lineup")]
    public Dictionary<uint, uint>? CustomLineup { get; set; }

    public BattleKind Kind => BattleType.ToUpperInvariant() switch
    {
        "MOC" or "1" => BattleKind.Moc,
        "PF" or "2" => BattleKind.PureFiction,
        "SU" or "3" => BattleKind.SimulatedUniverse,
        "AS" or "4" => BattleKind.ApocalypticShadow,
        "AA" or "5" => BattleKind.AnomalyArbitration,
        _ => BattleKind.Default,
    };
}

public sealed class SrMonster
{
    public uint Level { get; set; } = 95;

    [JsonPropertyName("monster_id")]
    public uint MonsterId { get; set; }

    [JsonPropertyName("max_hp")]
    public uint MaxHp { get; set; }

    // srtools can ask for the same monster several times in one wave
    public uint Amount { get; set; } = 1;
}

public sealed class SrBlessing
{
    public uint Id { get; set; }

    public uint Level { get; set; } = 1;

    [JsonPropertyName("dynamic_key")]
    public SrDynamicValue? DynamicKey { get; set; }

    [JsonPropertyName("dynamic_values")]
    public List<SrDynamicValue> DynamicValues { get; set; } = [];
}

public sealed class SrDynamicValue
{
    public string Key { get; set; } = string.Empty;

    public float Value { get; set; }
}

public enum BattleKind
{
    Default,
    Moc,
    PureFiction,
    SimulatedUniverse,
    ApocalypticShadow,
    AnomalyArbitration,
}
