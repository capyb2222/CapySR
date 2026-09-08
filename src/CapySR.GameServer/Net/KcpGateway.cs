using System.Collections.Concurrent;
using System.Net;
using System.Net.Sockets;
using CapySR.Common;
using CapySR.GameServer.Game;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Net;

public sealed class KcpGateway(ServerConfig config, HandlerRegistry handlers, GameWorld world, ILogger<KcpGateway> logger)
{
    private const int MaxDatagram = 1500;

    private readonly ConcurrentDictionary<uint, PlayerSession> _sessions = [];
    private Socket? _socket;
    private uint _nextConversation;

    public async Task RunAsync(CancellationToken cancellationToken)
    {
        var endpoint = new IPEndPoint(IPAddress.Parse(config.GameServer.Host), config.GameServer.Port);

        _socket = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        _socket.Bind(endpoint);

        // windows udp resets the socket when an ICMP unreachable comes back; ignore it
        if (OperatingSystem.IsWindows())
        {
            _socket.IOControl(unchecked((int)0x9800000C), [0, 0, 0, 0], null);
        }

        logger.LogInformation("kcp gateway listening on {Endpoint}", endpoint);

        _ = Task.Run(() => TickLoopAsync(cancellationToken), cancellationToken);

        var buffer = new byte[MaxDatagram];
        var from = new IPEndPoint(IPAddress.Any, 0);

        while (!cancellationToken.IsCancellationRequested)
        {
            SocketReceiveFromResult result;

            try
            {
                result = await _socket.ReceiveFromAsync(buffer, SocketFlags.None, from, cancellationToken);
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch (SocketException e)
            {
                logger.LogDebug("recv failed ({Error})", e.SocketErrorCode);
                continue;
            }

            var datagram = buffer.AsSpan(0, result.ReceivedBytes).ToArray();

            switch (datagram.Length)
            {
                case NetOperation.Size:
                    await HandleOperationAsync(datagram, result.RemoteEndPoint);
                    break;

                case >= Kcp.Kcp.Overhead:
                    await HandlePayloadAsync(datagram);
                    break;

                default:
                    logger.LogDebug("ignoring {Length}-byte datagram from {From}", datagram.Length, result.RemoteEndPoint);
                    break;
            }
        }

        // shutting down: let every session write out what it remembers
        foreach (var (key, session) in _sessions)
        {
            session.Close();
            _sessions.TryRemove(key, out _);
        }
    }

    private async Task HandleOperationAsync(byte[] datagram, EndPoint remote)
    {
        if (!NetOperation.TryRead(datagram, out var operation))
        {
            return;
        }

        if (operation.IsConnect)
        {
            var conversation = Interlocked.Increment(ref _nextConversation);
            var token = (uint)Random.Shared.Next(1, int.MaxValue);

            // one player at a time for now; a reconnect replaces the old session
            foreach (var (key, existing) in _sessions)
            {
                existing.Close();
                _sessions.TryRemove(key, out _);
            }

            var session = new PlayerSession(
                conversation, token, _socket!, remote, handlers, config, world, logger);

            _sessions[conversation] = session;

            logger.LogInformation("conv {Conv}: connected from {Remote}", conversation, remote);

            var accept = NetOperation.Accept(conversation, token, operation.Data);
            await _socket!.SendToAsync(accept.ToArray(), SocketFlags.None, remote);
            return;
        }

        if (operation.IsDisconnect)
        {
            if (_sessions.TryGetValue(operation.Param1, out var session) && session.Token == operation.Param2)
            {
                session.Close();
                _sessions.TryRemove(operation.Param1, out _);
                logger.LogInformation("conv {Conv}: disconnected", operation.Param1);
            }

            return;
        }

        logger.LogWarning("unknown handshake magic {Head:X}-{Tail:X}", operation.Head, operation.Tail);
    }

    private async Task HandlePayloadAsync(byte[] datagram)
    {
        var conversation = Kcp.Kcp.GetConversation(datagram);

        if (!_sessions.TryGetValue(conversation, out var session))
        {
            logger.LogDebug("no session for conv {Conv}", conversation);
            return;
        }

        try
        {
            await session.ReceiveAsync(datagram);
        }
        catch (Exception e)
        {
            logger.LogError(e, "conv {Conv}: session error", conversation);
        }

        if (session.IsClosed)
        {
            _sessions.TryRemove(conversation, out _);
        }
    }

    // kcp needs a heartbeat even when the client is quiet, for retransmits
    private async Task TickLoopAsync(CancellationToken cancellationToken)
    {
        using var timer = new PeriodicTimer(TimeSpan.FromMilliseconds(20));

        while (await timer.WaitForNextTickAsync(cancellationToken))
        {
            foreach (var (conversation, session) in _sessions)
            {
                await session.TickAsync();

                if (session.IsClosed)
                {
                    _sessions.TryRemove(conversation, out _);
                }
            }
        }
    }
}
