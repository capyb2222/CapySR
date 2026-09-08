using System.Text.Json;
using System.Text.Json.Serialization;

namespace CapySR.Data.Excel;

// dimbreath wraps scalars as {"Value": n}; the zig servers' dumps flatten them to bare numbers
[JsonConverter(typeof(ExcelValueConverter))]
public readonly record struct ExcelValue(long Value)
{
    public static implicit operator long(ExcelValue v) => v.Value;

    public uint AsUInt32() => (uint)Value;
}

internal sealed class ExcelValueConverter : JsonConverter<ExcelValue>
{
    public override ExcelValue Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
    {
        switch (reader.TokenType)
        {
            case JsonTokenType.Number:
                return new ExcelValue(reader.TryGetInt64(out var n) ? n : (long)reader.GetDouble());

            case JsonTokenType.String:
                return new ExcelValue(long.TryParse(reader.GetString(), out var parsed) ? parsed : 0);

            case JsonTokenType.StartObject:
                long value = 0;

                while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
                {
                    var name = reader.GetString();
                    reader.Read();

                    if (name is "Value" or "value" && reader.TokenType == JsonTokenType.Number)
                    {
                        value = reader.TryGetInt64(out var v) ? v : (long)reader.GetDouble();
                    }
                    else
                    {
                        reader.Skip();
                    }
                }

                return new ExcelValue(value);

            default:
                reader.Skip();
                return default;
        }
    }

    public override void Write(Utf8JsonWriter writer, ExcelValue value, JsonSerializerOptions options) =>
        writer.WriteNumberValue(value.Value);
}

public sealed class AvatarExcel
{
    public uint AvatarID { get; set; }

    public string Rarity { get; set; } = string.Empty;

    public string DamageType { get; set; } = string.Empty;

    public string AvatarBaseType { get; set; } = string.Empty;

    public ExcelValue SPNeed { get; set; }

    public List<uint> RankIDList { get; set; } = [];

    public bool Release { get; set; }

    public uint SpMax => SPNeed.AsUInt32();
}

public sealed class AvatarSkillTreeExcel
{
    public uint PointID { get; set; }

    public uint Level { get; set; }

    public uint AvatarID { get; set; }

    public uint MaxLevel { get; set; }

    // "Point01"
    public string AnchorType { get; set; } = string.Empty;

    // AvatarPathData.point_id is the anchor index, not the raw point id
    public uint AnchorIndex
    {
        get
        {
            var start = AnchorType.Length;

            while (start > 0 && char.IsAsciiDigit(AnchorType[start - 1]))
            {
                start--;
            }

            return start < AnchorType.Length && uint.TryParse(AnchorType.AsSpan(start), out var value)
                ? value
                : 0;
        }
    }
}

// ID is the avatar; DefaultMazeBuffIDList[0] is the buff the client expects when
// that avatar starts a battle, with SkillIndex passed as a dynamic value
public sealed class AvatarDefaultMazeBuffExcel
{
    public uint ID { get; set; }

    public uint SkillIndex { get; set; }

    public List<uint> DefaultMazeBuffIDList { get; set; } = [];

    public uint BuffId => DefaultMazeBuffIDList.Count > 0 ? DefaultMazeBuffIDList[0] : 0;
}

public sealed class AvatarGlobalBuffExcel
{
    public uint AvatarID { get; set; }

}

// one profile portrait per character
public sealed class HeadIconExcel
{
    public uint ID { get; set; }

    public uint AvatarID { get; set; }
}

// the ten avatars with an enhanced technique state
public sealed class AvatarConfigEnhancedExcel
{
    public uint AvatarID { get; set; }

    public uint EnhancedID { get; set; }
}

public sealed class AvatarMazeBuffExcel
{
    public uint ID { get; set; }

    public string ModifierName { get; set; } = string.Empty;

    // ADV_GlobalSkill_Maze_* buffs apply even when the avatar is not in the lineup
    public bool IsGlobal => ModifierName.StartsWith("ADV_GlobalSkill_Maze", StringComparison.Ordinal);

    public uint OwnerAvatarId => ID / 100;
}

public sealed class EquipmentExcel
{
    public uint EquipmentID { get; set; }

    // "CombatPowerLightconeRarity3"
    public string Rarity { get; set; } = string.Empty;

    public uint MaxPromotion { get; set; }

    public uint MaxRank { get; set; }

    public bool Release { get; set; }
}

public sealed class MonsterExcel
{
    public uint MonsterID { get; set; }

}

public sealed class StageExcel
{
    public uint StageID { get; set; }

    public string StageType { get; set; } = string.Empty;

    public uint Level { get; set; }

    public uint HardLevelGroup { get; set; }

    public uint EliteGroup { get; set; }

    public bool Release { get; set; }

    [JsonConverter(typeof(MonsterWaveConverter))]
    public List<List<uint>> MonsterList { get; set; } = [];

    [JsonConverter(typeof(StageConfigDataConverter))]
    public Dictionary<string, string> StageConfigData { get; set; } = [];

    // SpecialAvatar ids lent to the player for this fight
    public List<uint> TrialAvatarList { get; set; } = [];
}

// dimbreath: [{ "Monster0": id, ... }] per wave. trimmed dumps: [[id, id, ...]] per wave.
internal sealed class MonsterWaveConverter : JsonConverter<List<List<uint>>>
{
    public override List<List<uint>> Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
    {
        var waves = new List<List<uint>>();

        if (reader.TokenType != JsonTokenType.StartArray)
        {
            reader.Skip();
            return waves;
        }

        while (reader.Read() && reader.TokenType != JsonTokenType.EndArray)
        {
            if (reader.TokenType == JsonTokenType.StartArray)
            {
                var flat = new List<uint>();

                while (reader.Read() && reader.TokenType != JsonTokenType.EndArray)
                {
                    if (reader.TokenType == JsonTokenType.Number)
                    {
                        flat.Add(reader.GetUInt32());
                    }
                    else
                    {
                        reader.Skip();
                    }
                }

                waves.Add(flat);
                continue;
            }

            if (reader.TokenType != JsonTokenType.StartObject)
            {
                reader.Skip();
                continue;
            }

            var wave = new SortedDictionary<string, uint>(StringComparer.Ordinal);

            while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
            {
                var name = reader.GetString()!;
                reader.Read();

                if (reader.TokenType == JsonTokenType.Number)
                {
                    wave[name] = reader.GetUInt32();
                }
                else
                {
                    reader.Skip();
                }
            }

            waves.Add([.. wave.Values]);
        }

        return waves;
    }

    public override void Write(Utf8JsonWriter writer, List<List<uint>> value, JsonSerializerOptions options) =>
        throw new NotSupportedException();
}

// entries look like { "<obf>": "_Wave", "<obf>": "1" } - the key names are obfuscated
// and reshuffle every version, so key off the value that starts with '_'
internal sealed class StageConfigDataConverter : JsonConverter<Dictionary<string, string>>
{
    public override Dictionary<string, string> Read(ref Utf8JsonReader reader, Type type, JsonSerializerOptions options)
    {
        var result = new Dictionary<string, string>(StringComparer.Ordinal);

        if (reader.TokenType != JsonTokenType.StartArray)
        {
            reader.Skip();
            return result;
        }

        while (reader.Read() && reader.TokenType != JsonTokenType.EndArray)
        {
            if (reader.TokenType != JsonTokenType.StartObject)
            {
                reader.Skip();
                continue;
            }

            var values = new List<string>(2);

            while (reader.Read() && reader.TokenType != JsonTokenType.EndObject)
            {
                reader.Read();
                values.Add(reader.TokenType == JsonTokenType.String ? reader.GetString()! : string.Empty);
            }

            var key = values.FirstOrDefault(v => v.StartsWith('_'));

            if (key is not null)
            {
                result[key] = values.FirstOrDefault(v => !v.StartsWith('_')) ?? string.Empty;
            }
        }

        return result;
    }

    public override void Write(Utf8JsonWriter writer, Dictionary<string, string> value, JsonSerializerOptions options) =>
        throw new NotSupportedException();
}
