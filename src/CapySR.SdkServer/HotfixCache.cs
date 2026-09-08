using System.Text.Json;
using System.Text.Json.Serialization;
using CapySR.Common;
using CapySR.Protocol;
using Google.Protobuf;

namespace CapySR.SdkServer;

public sealed class VersionHotfix
{
    [JsonPropertyName("asset_bundle_url")]
    public string AssetBundleUrl { get; set; } = string.Empty;

    [JsonPropertyName("ex_resource_url")]
    public string ExResourceUrl { get; set; } = string.Empty;

    [JsonPropertyName("lua_url")]
    public string LuaUrl { get; set; } = string.Empty;

    [JsonPropertyName("ifix_url")]
    public string IfixUrl { get; set; } = string.Empty;

    // an empty ex_resource_url hangs the client at ~99% before it ever reaches kcp
    public bool IsUsable => AssetBundleUrl.Length > 0 && ExResourceUrl.Length > 0;
}

public sealed class HotfixCache
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNameCaseInsensitive = true,
    };

    private readonly HotfixConfig _config;
    private readonly ILogger<HotfixCache> _logger;
    private readonly HttpClient _http;
    private readonly SemaphoreSlim _lock = new(1, 1);
    private readonly string _path;

    private Dictionary<string, VersionHotfix> _versions = new(StringComparer.OrdinalIgnoreCase);

    public HotfixCache(HotfixConfig config, ILogger<HotfixCache> logger)
    {
        _config = config;
        _logger = logger;
        _path = Path.IsPathRooted(config.CacheFile)
            ? config.CacheFile
            : Path.Combine(RepoPaths.Config, config.CacheFile);

        _http = new HttpClient(new HttpClientHandler { UseProxy = !config.BypassProxy })
        {
            Timeout = TimeSpan.FromSeconds(30),
        };

        _http.DefaultRequestHeaders.UserAgent.ParseAdd("UnityPlayer/2021.3.21f1");

        Reload();
    }

    public IReadOnlyCollection<string> KnownVersions => _versions.Keys;

    public void Reload()
    {
        if (!File.Exists(_path))
        {
            _versions = new Dictionary<string, VersionHotfix>(StringComparer.OrdinalIgnoreCase);
            return;
        }

        try
        {
            _versions = JsonSerializer.Deserialize<Dictionary<string, VersionHotfix>>(
                            File.ReadAllText(_path), JsonOptions)
                        ?? [];

            _versions = new Dictionary<string, VersionHotfix>(_versions, StringComparer.OrdinalIgnoreCase);
        }
        catch (JsonException e)
        {
            _logger.LogError("{Path} is malformed ({Message}), starting empty", _path, e.Message);
            _versions = new Dictionary<string, VersionHotfix>(StringComparer.OrdinalIgnoreCase);
        }
    }

    public async Task<VersionHotfix?> GetAsync(string version, string dispatchSeed)
    {
        if (_versions.TryGetValue(version, out var cached) && cached.IsUsable)
        {
            return cached;
        }

        if (!_config.AutoFetch)
        {
            return cached;
        }

        await _lock.WaitAsync();

        try
        {
            if (_versions.TryGetValue(version, out cached) && cached.IsUsable)
            {
                return cached;
            }

            var fetched = await FetchAsync(version, dispatchSeed);
            if (fetched is null)
            {
                return cached;
            }

            _versions[version] = fetched;
            Save();
            return fetched;
        }
        finally
        {
            _lock.Release();
        }
    }

    private async Task<VersionHotfix?> FetchAsync(string version, string dispatchSeed)
    {
        var host = SelectHost(version);
        if (host is null)
        {
            _logger.LogWarning("no upstream host known for version {Version}", version);
            return null;
        }

        var url = $"https://{host}/query_gateway?version={version}&platform_type=1&language_type=3" +
                  $"&dispatch_seed={dispatchSeed}&channel_id=1&sub_channel_id=1&is_need_url=1";

        _logger.LogInformation("fetching hotfix for {Version} from {Host}", version, host);

        try
        {
            var body = await _http.GetStringAsync(url);
            var gateway = GateServer.Parser.ParseFrom(Convert.FromBase64String(body.Trim()));

            var hotfix = new VersionHotfix
            {
                AssetBundleUrl = gateway.AssetBundleUrl,
                ExResourceUrl = gateway.ExResourceUrl,
                LuaUrl = gateway.LuaUrl,
                IfixUrl = gateway.IfixUrl,
            };

            if (!hotfix.IsUsable)
            {
                _logger.LogWarning(
                    "upstream returned no urls for {Version} (retcode {Retcode}, msg '{Msg}')",
                    version, gateway.Retcode, gateway.StopDesc);

                return null;
            }

            _logger.LogInformation("got hotfix for {Version}", version);
            return hotfix;
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or FormatException
                                     or InvalidProtocolBufferException)
        {
            _logger.LogError("hotfix fetch for {Version} failed: {Message}", version, e.Message);
            return null;
        }
    }

    private void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
            File.WriteAllText(_path, JsonSerializer.Serialize(_versions, JsonOptions));
        }
        catch (IOException e)
        {
            _logger.LogError("could not write {Path}: {Message}", _path, e.Message);
        }
    }

    private static string? SelectHost(string version) => version switch
    {
        _ when version.StartsWith("OSBETA", StringComparison.Ordinal) => "beta-release01-asia.starrails.com",
        _ when version.StartsWith("OSPROD", StringComparison.Ordinal) => "prod-official-asia-dp01.starrails.com",
        _ when version.StartsWith("CNBETA", StringComparison.Ordinal) => "beta-release01-cn.bhsr.com",
        _ when version.StartsWith("CNPROD", StringComparison.Ordinal) => "prod-gf-cn-dp01.bhsr.com",
        _ => null,
    };
}
