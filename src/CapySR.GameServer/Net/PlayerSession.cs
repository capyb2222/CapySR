using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using CapySR.Common;
using CapySR.GameServer.Battle;
using CapySR.GameServer.Game;
using CapySR.GameServer.Handlers;
using CapySR.GameServer.Scene;
using CapySR.Protocol;
using Google.Protobuf;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Net;

public sealed class PlayerSession
{
    private const ushort HeartBeatCmdId = 44;
    private const ushort HeartBeatRspCmdId = 62;
    private const ushort SceneEntityMoveCmdId = 1434;
    private const ushort SceneEntityMoveRspCmdId = 1425;
    private const ushort LogoutCmdId = 5;

    private static readonly TimeSpan SaveInterval = TimeSpan.FromSeconds(2);

    private readonly Kcp.Kcp _kcp;
    private readonly Socket _socket;
    private readonly EndPoint _remote;
    private readonly HandlerRegistry _handlers;
    private readonly ILogger _logger;
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly HashSet<ushort> _warnedUnimplemented = [];

    private volatile bool _reloadPending;
    private DateTime _lastSave = DateTime.UtcNow;

    public PlayerSession(
        uint conversation, uint token, Socket socket, EndPoint remote,
        HandlerRegistry handlers, ServerConfig config, GameWorld world, ILogger logger)
    {
        Conversation = conversation;
        Token = token;
        Config = config;
        World = world;
        Player = new Player(config.GameServer);
        _socket = socket;
        _remote = remote;
        _handlers = handlers;
        _logger = logger;

        Scene = new SceneState();
        Challenge = new ChallengeState();
        Battles = new BattleController(this);

        if (config.GameServer.Persist && PlayerStore.TryLoad(world.PlayerStorePath, Player))
        {
            _logger.LogInformation("conv {Conv}: restored player state from {Path}", conversation, world.PlayerStorePath);
        }

        SeedLineups();
        world.PlayerReloaded += OnPlayerReloaded;

        _kcp = new Kcp.Kcp(conversation, token, SendDatagram);
        _kcp.SetNoDelay(true, 10, 2, true);
        _kcp.SetWindowSize(256, 256);
    }

    public uint Conversation { get; }

    public uint Token { get; }

    public ServerConfig Config { get; }

    public GameWorld World { get; }

    public ILogger Logger => _logger;

    public Player Player { get; }

    public SceneState Scene { get; }

    public ChallengeState Challenge { get; }

    public BattleController Battles { get; }

    public bool IsClosed { get; private set; }

    private uint Now => (uint)_clock.ElapsedMilliseconds;

    // squads store base ids; the roster lists paths
    private void SeedLineups()
    {
        var owned = World.OwnedAvatarIds.Select(World.Data.BaseAvatarId).Distinct().ToList();

        Player.Lineups.Prune(id => owned.Contains(id));
        Player.Lineups.Seed(owned.Take(Lineup.Size));

        // the walking team must never be empty, whichever squad is selected
        if (Player.Lineups.Current.IsEmpty)
        {
            Player.Lineups.Squad(Player.Lineups.CurrentIndex).Fill(owned.Take(Lineup.Size));
        }
    }

    private void OnPlayerReloaded() => _reloadPending = true;

    public async Task ReceiveAsync(byte[] datagram)
    {
        await _gate.WaitAsync();
        var packets = new List<NetPacket>();

        try
        {
            _kcp.Input(datagram);
            _kcp.Update(Now);

            while (_kcp.TryReceive(out var data))
            {
                var offset = 0;

                while (NetPacket.TryRead(data.AsSpan(offset), out var packet, out var consumed))
                {
                    packets.Add(packet);
                    offset += consumed;
                }

                if (offset != data.Length)
                {
                    _logger.LogWarning("conv {Conv}: {Left} trailing bytes in kcp payload", Conversation, data.Length - offset);
                }
            }
        }
        finally
        {
            _gate.Release();
        }

        if (_reloadPending && Player.LoggedIn)
        {
            _reloadPending = false;
            await this.OnSrToolsReloadedAsync();
        }

        foreach (var packet in packets)
        {
            await DispatchAsync(packet);
        }

        FlushPlayerState(force: false);
        await TickAsync();
    }

    private async Task DispatchAsync(NetPacket packet)
    {
        if (packet.CmdId != HeartBeatCmdId && packet.CmdId != SceneEntityMoveCmdId)
        {
            _logger.LogInformation("conv {Conv}: <- {Name} ({Id}, {Bytes}b)",
                Conversation, packet.Name, packet.CmdId, packet.Body.Length);
        }

        if (packet.CmdId == LogoutCmdId)
        {
            _logger.LogInformation("conv {Conv}: logout", Conversation);
            Close();
            return;
        }

        if (_handlers.TryGetHandler(packet.CmdId, out var handler))
        {
            var message = packet.ParseBody();

            if (message is null)
            {
                _logger.LogWarning("conv {Conv}: could not parse {Name}", Conversation, packet.Name);
                return;
            }

            try
            {
                await handler(this, message);
            }
            catch (Exception e)
            {
                _logger.LogError(e, "conv {Conv}: {Name} handler threw", Conversation, packet.Name);
                await SendErrorResponseAsync(packet.CmdId);
            }

            return;
        }

        if (packet.Name.EndsWith("Notify", StringComparison.Ordinal))
        {
            _logger.LogDebug("conv {Conv}: ignoring notify {Name}", Conversation, packet.Name);
            return;
        }

        if (ProtocolRegistry.TryGetResponseFor(packet.CmdId, out var responseCmdId))
        {
            // once per session is enough to know a feature is missing
            if (_warnedUnimplemented.Add(packet.CmdId))
            {
                _logger.LogWarning("conv {Conv}: {Name} not implemented - sending empty {Response}",
                    Conversation, packet.Name, ProtocolRegistry.GetName(responseCmdId));
            }

            await SendRawAsync(NetPacket.Empty(responseCmdId));
            return;
        }

        if (_warnedUnimplemented.Add(packet.CmdId))
        {
            _logger.LogWarning("conv {Conv}: {Name} ({Id}) has no matching ScRsp in the dump",
                Conversation, packet.Name, packet.CmdId);
        }
    }

    // a request that blew up still gets its response, flagged as a server error, so the
    // client shows a retry prompt instead of waiting on a spinner forever
    private async Task SendErrorResponseAsync(ushort requestCmdId)
    {
        if (!ProtocolRegistry.TryGetResponseFor(requestCmdId, out var responseCmdId) ||
            !ProtocolRegistry.TryGetDescriptor(responseCmdId, out var descriptor))
        {
            return;
        }

        var response = descriptor.Parser.ParseFrom(ReadOnlySpan<byte>.Empty);
        var retcode = descriptor.FindFieldByName("retcode");

        if (retcode is not null && retcode.FieldType == Google.Protobuf.Reflection.FieldType.UInt32)
        {
            retcode.Accessor.SetValue(response, (uint)Retcode.RetServerInternalError);
        }

        await SendAsync(response);
    }

    public Task SendAsync(IMessage message) => SendRawAsync(NetPacket.Create(message));

    public async Task SendRawAsync(NetPacket packet)
    {
        await _gate.WaitAsync();

        try
        {
            _kcp.Send(packet.ToArray());
            _kcp.Flush();
            _kcp.Update(Now);
        }
        finally
        {
            _gate.Release();
        }

        if (packet.CmdId is not (SceneEntityMoveRspCmdId or HeartBeatRspCmdId))
        {
            _logger.LogDebug("conv {Conv}: -> {Name}", Conversation, packet.Name);
        }
    }

    public async Task TickAsync()
    {
        await _gate.WaitAsync();

        try
        {
            _kcp.Update(Now);

            if (_kcp.IsDead)
            {
                _logger.LogWarning("conv {Conv}: kcp link dead", Conversation);
                Close();
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    public void Close()
    {
        if (IsClosed)
        {
            return;
        }

        IsClosed = true;
        World.PlayerReloaded -= OnPlayerReloaded;
        FlushPlayerState(force: true);
    }

    private void FlushPlayerState(bool force)
    {
        if (!Config.GameServer.Persist || !Player.Dirty)
        {
            return;
        }

        if (!force && DateTime.UtcNow - _lastSave < SaveInterval)
        {
            return;
        }

        try
        {
            PlayerStore.Save(World.PlayerStorePath, Player);
            Player.ClearDirty();
            _lastSave = DateTime.UtcNow;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            _logger.LogWarning("could not save player state: {Message}", e.Message);
        }
    }

    private void SendDatagram(ReadOnlySpan<byte> data)
    {
        try
        {
            _socket.SendTo(data, SocketFlags.None, _remote);
        }
        catch (SocketException e)
        {
            _logger.LogWarning("conv {Conv}: send failed ({Error})", Conversation, e.SocketErrorCode);
            IsClosed = true;
        }
    }
}
