using CapySR.Common;
using CapySR.Data;
using CapySR.Data.SrTools;
using CapySR.GameServer.Battle;
using CapySR.GameServer.Scene;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Game;

// shared, read-mostly server state: static tables plus the srtools file, reloaded on change
public sealed class GameWorld
{
    private readonly ILogger _logger;
    private readonly string _srToolsPath;
    private FileSystemWatcher? _watcher;
    private DateTime _lastLoad = DateTime.MinValue;

    public GameWorld(ServerConfig config, ILogger logger)
    {
        _logger = logger;
        _srToolsPath = SrToolsData.ResolvePath(RepoPaths.Root);
        PlayerStorePath = Path.Combine(RepoPaths.Data, PlayerStore.FileName);

        Data = GameData.Load(config.Data.ResolvedSources, m => logger.LogInformation("{Message}", m));
        Battles = new BattleBuilder(Data);
        Scenes = new SceneBuilder(Data);
        Player = LoadPlayer();
        OwnedAvatarIds = ComputeOwned(Player);
        WatchPlayerFile();
    }

    public GameData Data { get; }

    public BattleBuilder Battles { get; }

    public SceneBuilder Scenes { get; }

    public SrToolsData Player { get; private set; }

    public string PlayerStorePath { get; }

    // sorted; the roster the account owns right now
    public IReadOnlyList<uint> OwnedAvatarIds { get; private set; }

    public bool HasSrToolsRoster => Player.Avatars.Count > 0;

    public event Action? PlayerReloaded;

    private IReadOnlyList<uint> ComputeOwned(SrToolsData player) =>
        player.Avatars.Count > 0
            ? [.. player.Avatars.Keys.Where(id => Roster.IsPlayable(id) && Data.Avatars.ContainsKey(id)).OrderBy(id => id)]
            : [.. Data.Avatars.Keys.Where(Roster.IsPlayable).OrderBy(id => id)];

    private SrToolsData LoadPlayer()
    {
        if (!File.Exists(_srToolsPath))
        {
            _logger.LogWarning("no {Path} yet - upload from srtools to populate avatars", _srToolsPath);
            return SrToolsData.Empty;
        }

        try
        {
            var data = SrToolsData.Load(_srToolsPath);
            DropUnknownItems(data);

            _logger.LogInformation(
                "srtools data: {Avatars} avatars, {Relics} relics, {Lightcones} lightcones, stage {Stage}",
                data.Avatars.Count, data.Relics.Count, data.Lightcones.Count, data.BattleConfig.StageId);

            return data;
        }
        catch (Exception e) when (e is IOException or System.Text.Json.JsonException)
        {
            _logger.LogError("could not read freesr-data.json: {Message}", e.Message);
            return Player ?? SrToolsData.Empty;
        }
    }

    // srtools can hand out items newer than the client knows. an item whose type the client
    // cannot resolve makes its inventory red-dot filter throw every frame, which freezes the
    // character screen, so drop them before they ever reach the wire.
    private void DropUnknownItems(SrToolsData player)
    {
        if (Data.KnownRelicIds.Count == 0 && Data.KnownLightconeIds.Count == 0)
        {
            return;
        }

        var relics = player.Relics.RemoveAll(r => !Data.KnownRelicIds.Contains(r.RelicId));
        var lightcones = player.Lightcones.RemoveAll(l => !Data.KnownLightconeIds.Contains(l.ItemId));
        var avatars = player.Avatars.Keys.Where(id => !Data.Avatars.ContainsKey(id)).ToList();

        foreach (var id in avatars)
        {
            player.Avatars.Remove(id);
        }

        if (relics + lightcones + avatars.Count > 0)
        {
            _logger.LogWarning(
                "dropped {Relics} relic(s), {Lightcones} lightcone(s) and {Avatars} avatar(s) the game data does not know",
                relics, lightcones, avatars.Count);
        }
    }

    private void WatchPlayerFile()
    {
        var directory = Path.GetDirectoryName(_srToolsPath)!;
        Directory.CreateDirectory(directory);

        _watcher = new FileSystemWatcher(directory, Path.GetFileName(_srToolsPath))
        {
            NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.FileName | NotifyFilters.Size,
            EnableRaisingEvents = true,
        };

        _watcher.Changed += OnChanged;
        _watcher.Created += OnChanged;
        _watcher.Renamed += (_, _) => OnChanged(null!, null!);
    }

    private void OnChanged(object sender, FileSystemEventArgs e)
    {
        // editors and our own atomic move fire several events for one save
        if (DateTime.UtcNow - _lastLoad < TimeSpan.FromMilliseconds(500))
        {
            return;
        }

        _lastLoad = DateTime.UtcNow;
        Thread.Sleep(100);

        Reload();
        PlayerReloaded?.Invoke();
    }

    // re-read the srtools file on demand; callers decide who gets told
    public void Reload()
    {
        var player = LoadPlayer();
        Player = player;
        OwnedAvatarIds = ComputeOwned(player);
    }
}
