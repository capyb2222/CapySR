using System.Net;
using System.Net.Sockets;
using CapySR.Common;
using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging.Abstractions;
using Xunit;

namespace CapySR.Tests;

public class GatewayIntegrationTests
{
    // one world for the whole class; loading the tables is the slow part
    private static readonly GameWorld World =
        new(new ServerConfig(), NullLogger.Instance);

    [Fact]
    public async Task ClientCompletesHandshakeAndLogin()
    {
        var port = FreeUdpPort();
        // Persist off: a test must not read or write the machine's saved profile
        var config = new ServerConfig { GameServer = { Host = "127.0.0.1", Port = port, Nickname = "Capybara", Persist = false } };
        var handlers = HandlerRegistry.Build(NullLogger.Instance);
        var gateway = new KcpGateway(config, handlers, World, NullLogger<KcpGateway>.Instance);

        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var server = gateway.RunAsync(cancellation.Token);

        using var client = new TestClient(new IPEndPoint(IPAddress.Loopback, port));
        await client.ConnectAsync();

        var token = await client.RequestAsync<PlayerGetTokenCsReq, PlayerGetTokenScRsp>(new PlayerGetTokenCsReq());
        Assert.Equal(0u, token.Retcode);
        Assert.Equal(config.GameServer.Uid, token.Uid);

        var login = await client.RequestAsync<PlayerLoginCsReq, PlayerLoginScRsp>(
            new PlayerLoginCsReq { LoginRandom = 4242 });

        Assert.Equal(0u, login.Retcode);
        Assert.Equal(4242u, login.LoginRandom);
        Assert.Equal("Capybara", login.BasicInfo.Nickname);
        Assert.Equal(6u, login.BasicInfo.WorldLevel);

        var heartbeat = await client.RequestAsync<PlayerHeartBeatCsReq, PlayerHeartBeatScRsp>(
            new PlayerHeartBeatCsReq { ClientTimeMs = 1234 });

        Assert.Equal(1234u, heartbeat.ClientTimeMs);

        await cancellation.CancelAsync();
        await Task.WhenAny(server, Task.Delay(2000));
    }

    [Fact]
    public async Task UnmodelledRequestStillGetsAnswered()
    {
        var port = FreeUdpPort();
        var config = new ServerConfig { GameServer = { Host = "127.0.0.1", Port = port, Persist = false } };
        var handlers = HandlerRegistry.Build(NullLogger.Instance);
        var gateway = new KcpGateway(config, handlers, World, NullLogger<KcpGateway>.Instance);

        using var cancellation = new CancellationTokenSource(TimeSpan.FromSeconds(30));
        var server = gateway.RunAsync(cancellation.Token);

        using var client = new TestClient(new IPEndPoint(IPAddress.Loopback, port));
        await client.ConnectAsync();

        var response = await client.RequestAsync<GetBagCsReq, GetBagScRsp>(new GetBagCsReq());
        Assert.NotNull(response);

        await cancellation.CancelAsync();
        await Task.WhenAny(server, Task.Delay(2000));
    }

    private static int FreeUdpPort()
    {
        using var probe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        probe.Bind(new IPEndPoint(IPAddress.Loopback, 0));
        return ((IPEndPoint)probe.LocalEndPoint!).Port;
    }
}

internal sealed class TestClient(IPEndPoint server) : IDisposable
{
    private readonly Socket _socket = new(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
    private readonly List<NetPacket> _inbox = [];
    private Kcp.Kcp? _kcp;

    public async Task ConnectAsync()
    {
        var connect = new NetOperation(NetOperation.ConnectHead, 0, 0, 1234, NetOperation.ConnectTail);
        await _socket.SendToAsync(connect.ToArray(), SocketFlags.None, server);

        var buffer = new byte[1500];
        var result = await ReceiveAsync(buffer, TimeSpan.FromSeconds(5));

        Assert.True(NetOperation.TryRead(buffer.AsSpan(0, result), out var accept));
        Assert.Equal(NetOperation.AcceptHead, accept.Head);
        Assert.Equal(NetOperation.AcceptTail, accept.Tail);

        _kcp = new Kcp.Kcp(accept.Param1, accept.Param2, data =>
            _socket.SendTo(data, SocketFlags.None, server));

        _kcp.SetNoDelay(true, 10, 2, true);
        _kcp.SetWindowSize(256, 256);
    }

    private uint _clock;

    private readonly List<string> _arrival = [];

    // notifies the server pushed alongside the responses, in arrival order
    public IReadOnlyList<NetPacket> Inbox => _inbox;

    // packet names exactly as they came off the wire, so ordering can be asserted
    public IReadOnlyList<string> ArrivalOrder => _arrival;

    public async Task<TResponse> RequestAsync<TRequest, TResponse>(TRequest request)
        where TRequest : Google.Protobuf.IMessage<TRequest>
        where TResponse : Google.Protobuf.IMessage<TResponse>, new()
    {
        var expected = ProtocolRegistry.GetCmdId<TResponse>();

        _kcp!.Send(NetPacket.Create(request).ToArray());
        _kcp.Flush();

        var deadline = DateTime.UtcNow + TimeSpan.FromSeconds(10);

        while (DateTime.UtcNow < deadline)
        {
            foreach (var packet in _inbox)
            {
                if (packet.CmdId == expected)
                {
                    _inbox.Remove(packet);
                    return packet.ParseBody<TResponse>();
                }
            }

            await PumpOnceAsync(TimeSpan.FromMilliseconds(200));
        }

        throw new TimeoutException($"never received {ProtocolRegistry.GetName(expected)}");
    }

    // keep reading for a while so pushes that trail a response land in the inbox
    public async Task PumpAsync(TimeSpan duration)
    {
        var deadline = DateTime.UtcNow + duration;

        while (DateTime.UtcNow < deadline)
        {
            await PumpOnceAsync(TimeSpan.FromMilliseconds(50));
        }
    }

    public List<T> Take<T>() where T : Google.Protobuf.IMessage<T>, new()
    {
        var cmdId = ProtocolRegistry.GetCmdId<T>();
        var taken = new List<T>();

        foreach (var packet in _inbox.Where(p => p.CmdId == cmdId).ToList())
        {
            taken.Add(packet.ParseBody<T>());
            _inbox.Remove(packet);
        }

        return taken;
    }

    public async Task<T> ExpectAsync<T>(TimeSpan? timeout = null) where T : Google.Protobuf.IMessage<T>, new()
    {
        var deadline = DateTime.UtcNow + (timeout ?? TimeSpan.FromSeconds(5));

        while (DateTime.UtcNow < deadline)
        {
            var found = Take<T>();
            if (found.Count > 0)
            {
                return found[0];
            }

            await PumpOnceAsync(TimeSpan.FromMilliseconds(100));
        }

        throw new TimeoutException($"never received {typeof(T).Name}");
    }

    public void ClearInbox()
    {
        _inbox.Clear();
        _arrival.Clear();
    }

    private async Task PumpOnceAsync(TimeSpan wait)
    {
        var buffer = new byte[1500];
        var received = await ReceiveAsync(buffer, wait);

        if (received > 0)
        {
            _kcp!.Input(buffer.AsSpan(0, received));

            while (_kcp.TryReceive(out var stream))
            {
                var offset = 0;

                while (NetPacket.TryRead(stream.AsSpan(offset), out var packet, out var consumed))
                {
                    _inbox.Add(packet);
                    _arrival.Add(packet.Name);
                    offset += consumed;
                }
            }
        }

        _clock += 20;
        _kcp!.Update(_clock);
    }

    private async Task<int> ReceiveAsync(byte[] buffer, TimeSpan timeout)
    {
        using var cancellation = new CancellationTokenSource(timeout);

        try
        {
            var result = await _socket.ReceiveFromAsync(
                buffer, SocketFlags.None, new IPEndPoint(IPAddress.Any, 0), cancellation.Token);

            return result.ReceivedBytes;
        }
        catch (OperationCanceledException)
        {
            return 0;
        }
    }

    public void Dispose() => _socket.Dispose();
}
