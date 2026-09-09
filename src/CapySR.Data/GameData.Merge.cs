using System.Text.Json;
using System.Text.Json.Nodes;

namespace CapySR.Data;

public sealed partial class GameData
{
    // Rows merge field by field across sources, not wholesale. Neither source is a superset:
    // pearl's StageConfig rows drop StageType, while its ChallengePeakConfig carries
    // MapEntranceID/NpcMonsterIDList that Dimbreath has no column for at all. Earlier sources
    // win on any field they define; later ones only fill in what is missing.
    private Dictionary<uint, T> Merge<T>(string fileName, Func<JsonObject, uint?> key, bool required = true)
    {
        var merged = new Dictionary<uint, JsonObject>();
        var found = false;

        foreach (var root in _sources)
        {
            var path = Path.Combine(root, fileName);
            if (!File.Exists(path))
            {
                continue;
            }

            found = true;

            foreach (var row in ReadRows(path))
            {
                if (key(row) is not { } id)
                {
                    continue;
                }

                if (merged.TryGetValue(id, out var existing))
                {
                    FillMissing(existing, row);
                }
                else
                {
                    merged[id] = row;
                }
            }
        }

        if (!found && required)
        {
            throw new FileNotFoundException(
                $"no source provides {fileName} (looked in {string.Join(", ", _sources)})");
        }

        var result = new Dictionary<uint, T>(merged.Count);

        foreach (var (id, row) in merged)
        {
            if (row.Deserialize<T>(NodeOptions) is { } value)
            {
                result[id] = value;
            }
        }

        return result;
    }

    private static void FillMissing(JsonObject target, JsonObject extra)
    {
        foreach (var (name, value) in extra)
        {
            if (!target.TryGetPropertyValue(name, out var existing) || existing is null)
            {
                target[name] = value?.DeepClone();
            }
        }
    }

    private static List<JsonObject> ReadRows(string path)
    {
        using var stream = File.OpenRead(path);
        var node = JsonNode.Parse(stream);

        // dimbreath writes a bare array; the zig servers wrap it as {"avatar_config": [...]}
        var array = node as JsonArray;

        if (array is null && node is JsonObject wrapper)
        {
            foreach (var (_, value) in wrapper)
            {
                if (value is JsonArray inner)
                {
                    array = inner;
                    break;
                }
            }
        }

        if (array is null)
        {
            return [];
        }

        var rows = new List<JsonObject>(array.Count);

        foreach (var item in array)
        {
            if (item is JsonObject row)
            {
                rows.Add(row);
            }
        }

        return rows;
    }

    private static uint? UInt(JsonObject row, string name) =>
        row.TryGetPropertyValue(name, out var node) && node is not null && node.GetValueKind() is JsonValueKind.Number
            ? node.GetValue<uint>()
            : null;

    private static string? Str(JsonObject row, string name) =>
        row.TryGetPropertyValue(name, out var node) && node is not null && node.GetValueKind() is JsonValueKind.String
            ? node.GetValue<string>()
            : null;
}
