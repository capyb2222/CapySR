using System.Buffers.Binary;
using Google.Protobuf;

namespace CapySR.Protocol;

// magic | cmdid u16 | headLen u16 | bodyLen u32 | head | body | magic. big-endian, no encryption.
public readonly struct NetPacket
{
    public const uint HeadMagic = 0x9D74C714;
    public const uint TailMagic = 0xD7A152C8;
    public const int Overhead = 16;

    public NetPacket(ushort cmdId, byte[] body, byte[]? head = null)
    {
        CmdId = cmdId;
        Body = body;
        Head = head ?? [];
    }

    public ushort CmdId { get; }

    public byte[] Head { get; }

    public byte[] Body { get; }

    public string Name => ProtocolRegistry.GetName(CmdId);

    public int Length => Overhead + Head.Length + Body.Length;

    public static NetPacket Create<T>(T message) where T : IMessage<T> =>
        new(ProtocolRegistry.GetCmdId<T>(), message.ToByteArray());

    public static NetPacket Create(IMessage message)
    {
        var cmdId = ProtocolRegistry.GetCmdId(message)
                    ?? throw new InvalidOperationException($"{message.GetType().Name} is not a top-level packet type.");

        return new NetPacket(cmdId, message.ToByteArray());
    }

    public static NetPacket Empty(ushort cmdId) => new(cmdId, []);

    public void WriteTo(Span<byte> destination)
    {
        if (destination.Length < Length)
        {
            throw new ArgumentException($"need {Length} bytes, got {destination.Length}", nameof(destination));
        }

        BinaryPrimitives.WriteUInt32BigEndian(destination, HeadMagic);
        BinaryPrimitives.WriteUInt16BigEndian(destination[4..], CmdId);
        BinaryPrimitives.WriteUInt16BigEndian(destination[6..], (ushort)Head.Length);
        BinaryPrimitives.WriteUInt32BigEndian(destination[8..], (uint)Body.Length);

        var offset = 12;
        Head.CopyTo(destination[offset..]);
        offset += Head.Length;
        Body.CopyTo(destination[offset..]);
        offset += Body.Length;

        BinaryPrimitives.WriteUInt32BigEndian(destination[offset..], TailMagic);
    }

    public byte[] ToArray()
    {
        var buffer = new byte[Length];
        WriteTo(buffer);
        return buffer;
    }

    // false rather than throw: a bad datagram should drop the session, not the process
    public static bool TryRead(ReadOnlySpan<byte> source, out NetPacket packet, out int consumed)
    {
        packet = default;
        consumed = 0;

        if (source.Length < Overhead || BinaryPrimitives.ReadUInt32BigEndian(source) != HeadMagic)
        {
            return false;
        }

        var cmdId = BinaryPrimitives.ReadUInt16BigEndian(source[4..]);
        var headLength = BinaryPrimitives.ReadUInt16BigEndian(source[6..]);
        var bodyLength = BinaryPrimitives.ReadUInt32BigEndian(source[8..]);

        if (bodyLength > int.MaxValue - Overhead - headLength)
        {
            return false;
        }

        var total = Overhead + headLength + (int)bodyLength;
        if (source.Length < total || BinaryPrimitives.ReadUInt32BigEndian(source[(total - 4)..]) != TailMagic)
        {
            return false;
        }

        packet = new NetPacket(
            cmdId,
            source.Slice(12 + headLength, (int)bodyLength).ToArray(),
            source.Slice(12, headLength).ToArray());

        consumed = total;
        return true;
    }

    public IMessage? ParseBody() => ProtocolRegistry.Parse(CmdId, Body);

    public T ParseBody<T>() where T : IMessage<T>, new() => new MessageParser<T>(() => new T()).ParseFrom(Body);
}

// 20-byte handshake datagrams, told apart from kcp payloads by length alone
public readonly record struct NetOperation(uint Head, uint Param1, uint Param2, uint Data, uint Tail)
{
    public const int Size = 20;

    public const uint ConnectHead = 0xFF;
    public const uint ConnectTail = 0xFFFFFFFF;
    public const uint DisconnectHead = 0x194;
    public const uint DisconnectTail = 0x19419494;
    public const uint AcceptHead = 0x145;
    public const uint AcceptTail = 0x14514545;

    public bool IsConnect => Head == ConnectHead && Tail == ConnectTail;

    public bool IsDisconnect => Head == DisconnectHead && Tail == DisconnectTail;

    public static NetOperation Accept(uint conversation, uint token, uint echo) =>
        new(AcceptHead, conversation, token, echo, AcceptTail);

    public static bool TryRead(ReadOnlySpan<byte> source, out NetOperation operation)
    {
        if (source.Length < Size)
        {
            operation = default;
            return false;
        }

        operation = new NetOperation(
            BinaryPrimitives.ReadUInt32BigEndian(source),
            BinaryPrimitives.ReadUInt32BigEndian(source[4..]),
            BinaryPrimitives.ReadUInt32BigEndian(source[8..]),
            BinaryPrimitives.ReadUInt32BigEndian(source[12..]),
            BinaryPrimitives.ReadUInt32BigEndian(source[16..]));

        return true;
    }

    public void WriteTo(Span<byte> destination)
    {
        BinaryPrimitives.WriteUInt32BigEndian(destination, Head);
        BinaryPrimitives.WriteUInt32BigEndian(destination[4..], Param1);
        BinaryPrimitives.WriteUInt32BigEndian(destination[8..], Param2);
        BinaryPrimitives.WriteUInt32BigEndian(destination[12..], Data);
        BinaryPrimitives.WriteUInt32BigEndian(destination[16..], Tail);
    }

    public byte[] ToArray()
    {
        var buffer = new byte[Size];
        WriteTo(buffer);
        return buffer;
    }
}
