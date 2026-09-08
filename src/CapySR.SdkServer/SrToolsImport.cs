using System.Text.Json;
using CapySR.Common;

namespace CapySR.SdkServer;

// srtools.neonteam.dev posts {"data": {...freesr-data...}}; we keep the inner object
public static class SrToolsImport
{
    public static string TargetPath => Path.Combine(RepoPaths.Data, "freesr-data.json");

    public static (string Message, int Status) Save(string body, ILogger log)
    {
        JsonDocument document;

        try
        {
            document = JsonDocument.Parse(body);
        }
        catch (JsonException e)
        {
            log.LogError("srtools sent malformed json: {Message}", e.Message);
            return ($"malformed json: {e.Message}", 500);
        }

        using (document)
        {
            if (!document.RootElement.TryGetProperty("data", out var data) ||
                data.ValueKind is JsonValueKind.Null or JsonValueKind.Undefined)
            {
                return ("OK", 200);
            }

            try
            {
                Directory.CreateDirectory(RepoPaths.Data);

                var json = JsonSerializer.Serialize(data, new JsonSerializerOptions { WriteIndented = true });

                // write-then-move so the game server's watcher never sees a half file
                var temp = TargetPath + ".tmp";
                File.WriteAllText(temp, json);
                File.Move(temp, TargetPath, overwrite: true);

                log.LogInformation("saved srtools data to {Path} ({Size} KiB)", TargetPath, json.Length / 1024);
                return ("OK", 200);
            }
            catch (IOException e)
            {
                log.LogError("could not write {Path}: {Message}", TargetPath, e.Message);
                return ($"failed to write freesr-data.json: {e.Message}", 500);
            }
        }
    }
}
