using CapySR.Protocol;
using Xunit;

namespace CapySR.Tests;

public class ProtocolTests
{
    [Fact]
    public void RegistryResolvesEveryCmdId()
    {
        ProtocolRegistry.EnsureInitialized();
        Assert.True(ProtocolRegistry.Count > 2000);
    }

    [Theory]
    [InlineData(34, "PlayerLoginCsReq")]
    [InlineData(60, "PlayerGetTokenCsReq")]
    [InlineData(134, "PVEBattleResultCsReq")]
    [InlineData(125, "PVEBattleResultScRsp")]
    public void CmdIdsMatchTheDump(ushort id, string name)
    {
        Assert.Equal(name, ProtocolRegistry.GetName(id));
    }

    [Fact]
    public void CmdIdLookupIsTwoWay()
    {
        Assert.Equal(34, ProtocolRegistry.GetCmdId<PlayerLoginCsReq>());
        Assert.Equal(34u, (uint)(ProtocolRegistry.GetCmdId(new PlayerLoginCsReq()) ?? 0));
        Assert.Equal((ushort)CmdId.PlayerLoginCsReq, ProtocolRegistry.GetCmdId<PlayerLoginCsReq>());
    }

    [Fact]
    public void PacketRoundTrips()
    {
        var login = new PlayerLoginScRsp
        {
            LoginRandom = 12345,
            Stamina = 240,
            BasicInfo = new PlayerBasicInfo { Nickname = "Capy", Level = 70, WorldLevel = 6 },
        };

        var encoded = NetPacket.Create(login).ToArray();

        Assert.True(NetPacket.TryRead(encoded, out var packet, out var consumed));
        Assert.Equal(encoded.Length, consumed);
        Assert.Equal(ProtocolRegistry.GetCmdId<PlayerLoginScRsp>(), packet.CmdId);

        var decoded = packet.ParseBody<PlayerLoginScRsp>();
        Assert.Equal(12345u, decoded.LoginRandom);
        Assert.Equal("Capy", decoded.BasicInfo.Nickname);
    }

    [Fact]
    public void ReadsBackToBackPackets()
    {
        var first = NetPacket.Create(new PlayerHeartBeatScRsp { ClientTimeMs = 1 }).ToArray();
        var second = NetPacket.Empty(ProtocolRegistry.GetCmdId<GetBagScRsp>()).ToArray();

        var stream = new byte[first.Length + second.Length];
        first.CopyTo(stream, 0);
        second.CopyTo(stream, first.Length);

        Assert.True(NetPacket.TryRead(stream, out var a, out var consumedA));
        Assert.True(NetPacket.TryRead(stream.AsSpan(consumedA), out var b, out _));

        Assert.Equal(ProtocolRegistry.GetCmdId<PlayerHeartBeatScRsp>(), a.CmdId);
        Assert.Equal(ProtocolRegistry.GetCmdId<GetBagScRsp>(), b.CmdId);
        Assert.Empty(b.Body);
    }

    [Fact]
    public void RejectsMalformedPackets()
    {
        Assert.False(NetPacket.TryRead([1, 2, 3], out _, out _));

        var good = NetPacket.Create(new PlayerHeartBeatScRsp()).ToArray();

        var badHead = good.ToArray();
        badHead[0] ^= 0xFF;
        Assert.False(NetPacket.TryRead(badHead, out _, out _));

        var badTail = good.ToArray();
        badTail[^1] ^= 0xFF;
        Assert.False(NetPacket.TryRead(badTail, out _, out _));

        Assert.False(NetPacket.TryRead(good.AsSpan(0, good.Length - 1), out _, out _));
    }

    [Fact]
    public void HandshakeOperationRoundTrips()
    {
        var accept = NetOperation.Accept(conversation: 7, token: 0xDEAD, echo: 99);

        Assert.True(NetOperation.TryRead(accept.ToArray(), out var parsed));
        Assert.Equal(accept, parsed);
        Assert.Equal(NetOperation.Size, accept.ToArray().Length);

        var connect = new NetOperation(
            NetOperation.ConnectHead, 0, 0, 1234, NetOperation.ConnectTail);

        Assert.True(connect.IsConnect);
        Assert.False(connect.IsDisconnect);
    }
}
