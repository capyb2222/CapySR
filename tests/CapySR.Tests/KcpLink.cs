using CapySR.Kcp;

namespace CapySR.Tests;

// two kcp endpoints wired together over a virtual clock, with optional loss/reorder
public sealed class KcpLink
{
    private readonly List<byte[]> _towardsServer = [];
    private readonly List<byte[]> _towardsClient = [];
    private readonly Random _random;

    public KcpLink(uint conversation = 1, uint token = 0xC0FFEE, int seed = 1234)
    {
        _random = new Random(seed);

        Client = new Kcp.Kcp(conversation, token, data => _towardsServer.Add(data.ToArray()));
        Server = new Kcp.Kcp(conversation, token, data => _towardsClient.Add(data.ToArray()));

        foreach (var kcp in new[] { Client, Server })
        {
            kcp.SetNoDelay(true, 10, 2, true);
            kcp.SetWindowSize(256, 256);
        }
    }

    public Kcp.Kcp Client { get; }

    public Kcp.Kcp Server { get; }

    public double LossRate { get; set; }

    public bool Reorder { get; set; }

    public uint Clock { get; private set; }

    public int DroppedDatagrams { get; private set; }

    public void Pump(int steps, int stepMs = 10)
    {
        for (var i = 0; i < steps; i++)
        {
            Clock += (uint)stepMs;
            Client.Update(Clock);
            Server.Update(Clock);

            Deliver(_towardsServer, Server);
            Deliver(_towardsClient, Client);
        }
    }

    private void Deliver(List<byte[]> queue, Kcp.Kcp destination)
    {
        if (queue.Count == 0)
        {
            return;
        }

        var batch = queue.ToList();
        queue.Clear();

        if (Reorder && batch.Count > 1)
        {
            batch.Reverse();
        }

        foreach (var datagram in batch)
        {
            if (LossRate > 0 && _random.NextDouble() < LossRate)
            {
                DroppedDatagrams++;
                continue;
            }

            destination.Input(datagram);
        }
    }

    public List<byte[]> DrainServer() => Drain(Server);

    public List<byte[]> DrainClient() => Drain(Client);

    private static List<byte[]> Drain(Kcp.Kcp kcp)
    {
        var received = new List<byte[]>();

        while (kcp.TryReceive(out var data))
        {
            received.Add(data);
        }

        return received;
    }
}
