using Microsoft.Extensions.Configuration;

namespace CapySR.Common;

public sealed class ServerConfig
{
    public DispatchConfig Dispatch { get; set; } = new();

    public GameServerConfig GameServer { get; set; } = new();

    public HotfixConfig Hotfix { get; set; } = new();

    public DataConfig Data { get; set; } = new();

    public static ServerConfig Load(string? path = null)
    {
        path ??= Path.Combine(RepoPaths.Root, "config", "config.json");

        var config = new ConfigurationBuilder()
            .AddJsonFile(path, optional: true)
            .AddEnvironmentVariables("CAPYSR_")
            .Build()
            .Get<ServerConfig>() ?? new ServerConfig();

        return config;
    }
}

public sealed class DispatchConfig
{
    public string Host { get; set; } = "0.0.0.0";

    public int Port { get; set; } = 21000;

    public string RegionName { get; set; } = "CapySR";

    public string RegionTitle { get; set; } = "CapySR";

    // what the client is told to hit next; must be reachable from the client
    public string PublicHost { get; set; } = "127.0.0.1";

    public string GatewayIp { get; set; } = "127.0.0.1";

    public int GatewayPort { get; set; } = 23301;

    public string QueryGatewayUrl => $"http://{PublicHost}:{Port}/query_gateway";
}

public sealed class GameServerConfig
{
    public string Host { get; set; } = "0.0.0.0";

    public int Port { get; set; } = 23301;

    public string ServerName { get; set; } = "CapySR";

    public uint Uid { get; set; } = 25;

    public string Nickname { get; set; } = "CapySR";

    public string Signature { get; set; } = "CapySR";

    // all = every relic and lightcone, equipped = only what is worn, none = empty bag.
    // "all" makes the client's red-dot filter walk every item, which throws if it cannot
    // type one of them and freezes the character screen.
    public string InventoryMode { get; set; } = "equipped";

    // where the player spawns. must be an entry res.json knows, with a matching teleport.
    public uint StartEntryId { get; set; } = 2050301;

    public uint StartTeleportId { get; set; } = 1029;

    public uint WorldId { get; set; } = 501;

    public uint Level { get; set; } = 70;

    public uint WorldLevel { get; set; } = 6;

    // what a fight plays. "auto" and "srtools" both hand every non-challenge fight to the
    // battle_config uploaded from srtools, so a calyx runs whatever stage is configured -
    // that is how an older MOC/PF/AS floor gets tested while F4 keeps the current rotation.
    // "stage" always plays the encounter's own stage instead.
    public string BattleSource { get; set; } = "auto";

    // report every MOC/PF/AS floor as cleared so none of them sit locked behind the one
    // before it. floors that have actually been played report their real result.
    public bool UnlockAllChallenges { get; set; } = true;

    // remember lineups, position and challenge records in data/player.json
    public bool Persist { get; set; } = true;
}

public sealed class HotfixConfig
{
    public bool AutoFetch { get; set; } = true;

    // local MITM proxies throttle the CDN badly; go direct
    public bool BypassProxy { get; set; } = true;

    public string CacheFile { get; set; } = "versions.json";

    // Telling a pre-packaged client to update makes it fetch design data and lua from the
    // official CDN. autopatchcn.bhsr.com is China-only, so off-mainland clients time out and
    // hang on the loading bar. Packaged clients already ship these files.
    public bool EnableDesignDataUpdate { get; set; }

    public bool EnableVideoUpdate { get; set; }

    // blank the resource urls entirely so the client cannot even try
    public bool SendResourceUrls { get; set; } = true;

    // serve the cdn through our own host, for clients whose dns lands on an unreachable edge
    public bool MirrorResources { get; set; } = true;
}

public sealed class DataConfig
{
    // priority order; the first source to define a row id keeps it.
    // left empty so the config binder replaces rather than appends to a default.
    public List<string> Sources { get; set; } = [];

    private static readonly string[] DefaultSources =
    [
        "turnbasedgamedata/ExcelOutput",
        "pearl-sr/resources",
    ];

    public IEnumerable<string> ResolvedSources => (Sources.Count > 0 ? Sources : DefaultSources.ToList())
        .Distinct(StringComparer.OrdinalIgnoreCase)
        .Select(s => Path.IsPathRooted(s) ? s : Path.Combine(RepoPaths.Root, s));
}

public static class RepoPaths
{
    public static string Root { get; } = Find();

    public static string Data => Path.Combine(Root, "data");

    public static string Config => Path.Combine(Root, "config");

    private static string Find()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);

        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "CapySR.slnx")) ||
                File.Exists(Path.Combine(dir.FullName, "CapySR.sln")))
            {
                return dir.FullName;
            }

            dir = dir.Parent;
        }

        return Directory.GetCurrentDirectory();
    }
}
