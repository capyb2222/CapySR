using Xunit;

namespace CapySR.Tests;

public class KcpTests
{
    [Fact]
    public void DeliversSmallMessage()
    {
        var link = new KcpLink();
        var payload = "hello capybara"u8.ToArray();

        link.Client.Send(payload);
        link.Pump(20);

        var received = link.DrainServer();
        Assert.Single(received);
        Assert.Equal(payload, received[0]);
    }

    [Fact]
    public void ReassemblesFragmentedMessage()
    {
        var link = new KcpLink();
        var payload = RandomBytes(100_000, seed: 7);

        link.Client.Send(payload);
        link.Pump(400);

        var received = link.DrainServer();
        Assert.Single(received);
        Assert.Equal(payload, received[0]);
    }

    [Fact]
    public void PreservesOrderAcrossManyMessages()
    {
        var link = new KcpLink();
        var sent = new List<byte[]>();

        for (var i = 0; i < 50; i++)
        {
            var payload = RandomBytes(300 + (i * 37), seed: i);
            sent.Add(payload);
            link.Client.Send(payload);
        }

        link.Pump(400);

        var received = link.DrainServer();
        Assert.Equal(sent.Count, received.Count);

        for (var i = 0; i < sent.Count; i++)
        {
            Assert.Equal(sent[i], received[i]);
        }
    }

    [Fact]
    public void RecoversFromPacketLoss()
    {
        var link = new KcpLink { LossRate = 0.25 };
        var payload = RandomBytes(40_000, seed: 11);

        link.Client.Send(payload);
        link.Pump(2000);

        var received = link.DrainServer();
        Assert.True(link.DroppedDatagrams > 0, "expected the link to actually drop something");
        Assert.Single(received);
        Assert.Equal(payload, received[0]);
    }

    [Fact]
    public void RecoversFromReordering()
    {
        var link = new KcpLink { Reorder = true };
        var payload = RandomBytes(40_000, seed: 13);

        link.Client.Send(payload);
        link.Pump(1000);

        var received = link.DrainServer();
        Assert.Single(received);
        Assert.Equal(payload, received[0]);
    }

    [Fact]
    public void CarriesTrafficBothWays()
    {
        var link = new KcpLink { LossRate = 0.1 };
        var request = RandomBytes(5_000, seed: 17);
        var response = RandomBytes(9_000, seed: 19);

        link.Client.Send(request);
        link.Server.Send(response);
        link.Pump(1000);

        Assert.Equal(request, Assert.Single(link.DrainServer()));
        Assert.Equal(response, Assert.Single(link.DrainClient()));
    }

    [Fact]
    public void ReadsConversationFromDatagram()
    {
        var captured = Array.Empty<byte>();
        var kcp = new Kcp.Kcp(0xABCDEF, 0x1234, data => captured = data.ToArray());

        kcp.Update(0);
        kcp.Send("x"u8.ToArray());
        kcp.Update(100);

        Assert.NotEmpty(captured);
        Assert.Equal(0xABCDEFu, Kcp.Kcp.GetConversation(captured));
    }

    private static byte[] RandomBytes(int length, int seed)
    {
        var buffer = new byte[length];
        new Random(seed).NextBytes(buffer);
        return buffer;
    }
}
