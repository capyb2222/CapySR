using System.Buffers.Binary;

namespace CapySR.Kcp;

public delegate void KcpOutput(ReadOnlySpan<byte> data);

// ikcp port with mihoyo's 28-byte header (conv + token prefix), little-endian.
public sealed class Kcp
{
    public const int Overhead = 28;

    private const uint RtoNoDelay = 30;
    private const uint RtoMin = 100;
    private const uint RtoDefault = 200;
    private const uint RtoMax = 60000;

    private const byte CmdPush = 81;
    private const byte CmdAck = 82;
    private const byte CmdWindowAsk = 83;
    private const byte CmdWindowTell = 84;

    private const uint AskSend = 1;
    private const uint AskTell = 2;

    private const int WndSend = 32;
    private const int WndRecv = 258;
    private const int MtuDefault = 1400;
    private const int Interval = 100;
    private const uint DeadLink = 20;
    private const uint ThreshInit = 2;
    private const uint ThreshMin = 2;
    private const uint ProbeInit = 7000;
    private const uint ProbeLimit = 120000;
    private const uint FastAckLimit = 5;

    private readonly KcpOutput _output;
    private readonly List<Segment> _sendBuffer = [];
    private readonly List<Segment> _receiveBuffer = [];
    private readonly Queue<Segment> _sendQueue = new();
    private readonly Queue<Segment> _receiveQueue = new();
    private readonly List<(uint Sn, uint Ts)> _ackList = [];

    private readonly byte[] _flushBuffer;

    private uint _sendUna;
    private uint _sendNext;
    private uint _receiveNext;

    private uint _sendWindow = WndSend;
    private uint _receiveWindow = WndRecv;
    private uint _remoteWindow = WndRecv;
    private uint _congestionWindow;
    private uint _increment;

    private uint _probe;
    private uint _probeTimestamp;
    private uint _probeWait;

    private int _mtu = MtuDefault;
    private int _mss = MtuDefault - Overhead;

    private uint _rttSmoothed;
    private uint _rttVariance;
    private uint _rto = RtoDefault;
    private uint _minRto = RtoMin;

    private uint _current;
    private uint _interval = Interval;
    private uint _flushTimestamp = Interval;
    private bool _noDelay;
    private bool _updated;

    private uint _ssthresh = ThreshInit;
    private uint _fastResend;
    private uint _fastLimit = FastAckLimit;
    private bool _noCongestionWindow;
    private uint _transmissions;
    private uint _deadLink = DeadLink;

    public Kcp(uint conversation, uint token, KcpOutput output)
    {
        Conversation = conversation;
        Token = token;
        _output = output;
        _flushBuffer = new byte[(MtuDefault + Overhead) * 3];
    }

    public uint Conversation { get; }

    public uint Token { get; }

    public bool IsDead { get; private set; }

    public int WaitingToSend => _sendBuffer.Count + _sendQueue.Count;

    public static uint GetConversation(ReadOnlySpan<byte> datagram) =>
        datagram.Length >= Overhead ? BinaryPrimitives.ReadUInt32LittleEndian(datagram) : 0;

    public void SetNoDelay(bool noDelay, int interval, int resend, bool noCongestionWindow)
    {
        _noDelay = noDelay;
        _minRto = noDelay ? RtoNoDelay : RtoMin;
        _interval = (uint)Math.Clamp(interval, 10, 5000);
        _fastResend = resend > 0 ? (uint)resend : 0;
        _noCongestionWindow = noCongestionWindow;
    }

    public void SetWindowSize(int send, int receive)
    {
        _sendWindow = (uint)Math.Max(1, send);
        _receiveWindow = (uint)Math.Max(WndRecv, receive);
    }

    // splits into mss-sized fragments, numbered high to low
    public void Send(ReadOnlySpan<byte> data)
    {
        if (data.IsEmpty)
        {
            return;
        }

        var count = data.Length <= _mss ? 1 : (data.Length + _mss - 1) / _mss;
        if (count > 255)
        {
            throw new ArgumentException($"payload of {data.Length} bytes needs {count} fragments (max 255)", nameof(data));
        }

        for (var i = 0; i < count; i++)
        {
            var size = Math.Min(data.Length, _mss);
            _sendQueue.Enqueue(new Segment(data[..size].ToArray()) { Fragment = (byte)(count - i - 1) });
            data = data[size..];
        }
    }

    public bool TryReceive(out byte[] data)
    {
        data = [];

        if (_receiveQueue.Count == 0)
        {
            return false;
        }

        var size = PeekSize();
        if (size < 0)
        {
            return false;
        }

        var recover = _receiveQueue.Count >= _receiveWindow;

        data = new byte[size];
        var offset = 0;

        while (_receiveQueue.Count > 0)
        {
            var segment = _receiveQueue.Dequeue();
            segment.Data.CopyTo(data, offset);
            offset += segment.Data.Length;

            if (segment.Fragment == 0)
            {
                break;
            }
        }

        MoveToReceiveQueue();

        // window reopened, tell the peer
        if (_receiveQueue.Count < _receiveWindow && recover)
        {
            _probe |= AskTell;
        }

        return true;
    }

    public void Input(ReadOnlySpan<byte> datagram)
    {
        if (datagram.Length < Overhead)
        {
            return;
        }

        var previousUna = _sendUna;
        uint maxAck = 0;
        uint latestTs = 0;
        var sawAck = false;

        while (datagram.Length >= Overhead)
        {
            var conversation = BinaryPrimitives.ReadUInt32LittleEndian(datagram);
            if (conversation != Conversation)
            {
                return;
            }

            var cmd = datagram[8];
            var fragment = datagram[9];
            var window = BinaryPrimitives.ReadUInt16LittleEndian(datagram[10..]);
            var timestamp = BinaryPrimitives.ReadUInt32LittleEndian(datagram[12..]);
            var sn = BinaryPrimitives.ReadUInt32LittleEndian(datagram[16..]);
            var una = BinaryPrimitives.ReadUInt32LittleEndian(datagram[20..]);
            var length = BinaryPrimitives.ReadUInt32LittleEndian(datagram[24..]);

            datagram = datagram[Overhead..];

            if (length > (uint)datagram.Length)
            {
                return;
            }

            _remoteWindow = window;
            RemoveAcknowledged(una);
            ShrinkSendBuffer();

            switch (cmd)
            {
                case CmdAck:
                    if (TimeDiff(_current, timestamp) >= 0)
                    {
                        UpdateRto((uint)TimeDiff(_current, timestamp));
                    }

                    AcknowledgeOne(sn);
                    ShrinkSendBuffer();

                    if (!sawAck || TimeDiff(sn, maxAck) > 0)
                    {
                        sawAck = true;
                        maxAck = sn;
                        latestTs = timestamp;
                    }

                    break;

                case CmdPush:
                    if (TimeDiff(sn, _receiveNext + _receiveWindow) < 0)
                    {
                        _ackList.Add((sn, timestamp));

                        if (TimeDiff(sn, _receiveNext) >= 0)
                        {
                            InsertReceived(new Segment(datagram[..(int)length].ToArray())
                            {
                                Conversation = conversation,
                                Command = cmd,
                                Fragment = fragment,
                                Window = window,
                                Timestamp = timestamp,
                                SequenceNumber = sn,
                                Una = una,
                            });
                        }
                    }

                    break;

                case CmdWindowAsk:
                    _probe |= AskTell;
                    break;

                case CmdWindowTell:
                    break;

                default:
                    return;
            }

            datagram = datagram[(int)length..];
        }

        if (sawAck)
        {
            AcknowledgeFast(maxAck, latestTs);
        }

        // ack advanced our window, so grow the congestion window
        if (TimeDiff(_sendUna, previousUna) > 0 && _congestionWindow < _remoteWindow)
        {
            var mss = (uint)_mss;

            if (_congestionWindow < _ssthresh)
            {
                _congestionWindow++;
                _increment += mss;
            }
            else
            {
                _increment = Math.Max(_increment, mss);
                _increment += mss * mss / _increment + mss / 16;

                if ((_congestionWindow + 1) * mss <= _increment)
                {
                    _congestionWindow = (_increment + mss - 1) / mss;
                }
            }

            if (_congestionWindow > _remoteWindow)
            {
                _congestionWindow = _remoteWindow;
                _increment = _remoteWindow * mss;
            }
        }
    }

    public void Update(uint currentMs)
    {
        _current = currentMs;

        if (!_updated)
        {
            _updated = true;
            _flushTimestamp = _current;
        }

        var slap = TimeDiff(_current, _flushTimestamp);

        if (slap is >= 10000 or < -10000)
        {
            _flushTimestamp = _current;
            slap = 0;
        }

        if (slap >= 0)
        {
            _flushTimestamp += _interval;

            if (TimeDiff(_current, _flushTimestamp) >= 0)
            {
                _flushTimestamp = _current + _interval;
            }

            Flush();
        }
    }

    public void Flush()
    {
        if (!_updated)
        {
            return;
        }

        var offset = 0;
        var window = (ushort)Math.Max(0, (int)_receiveWindow - _receiveQueue.Count);

        var template = new Segment([])
        {
            Conversation = Conversation,
            Command = CmdAck,
            Window = window,
            Una = _receiveNext,
        };

        foreach (var (sn, ts) in _ackList)
        {
            FlushIfFull(ref offset, 0);
            template.SequenceNumber = sn;
            template.Timestamp = ts;
            WriteHeader(template, _flushBuffer.AsSpan(offset), 0);
            offset += Overhead;
        }

        _ackList.Clear();

        ProbeWindow();

        if ((_probe & AskSend) != 0)
        {
            FlushIfFull(ref offset, 0);
            template.Command = CmdWindowAsk;
            template.SequenceNumber = 0;
            template.Timestamp = 0;
            WriteHeader(template, _flushBuffer.AsSpan(offset), 0);
            offset += Overhead;
        }

        if ((_probe & AskTell) != 0)
        {
            FlushIfFull(ref offset, 0);
            template.Command = CmdWindowTell;
            template.SequenceNumber = 0;
            template.Timestamp = 0;
            WriteHeader(template, _flushBuffer.AsSpan(offset), 0);
            offset += Overhead;
        }

        _probe = 0;

        var congestionWindow = Math.Min(_sendWindow, _remoteWindow);
        if (!_noCongestionWindow)
        {
            congestionWindow = Math.Min(congestionWindow, _congestionWindow);
        }

        while (_sendQueue.Count > 0 && TimeDiff(_sendNext, _sendUna + congestionWindow) < 0)
        {
            var segment = _sendQueue.Dequeue();
            segment.Conversation = Conversation;
            segment.Command = CmdPush;
            segment.Window = window;
            segment.Timestamp = _current;
            segment.SequenceNumber = _sendNext++;
            segment.Una = _receiveNext;
            segment.ResendTimestamp = _current;
            segment.Rto = _rto;
            _sendBuffer.Add(segment);
        }

        var resent = _fastResend > 0 ? _fastResend : uint.MaxValue;
        var minRto = _noDelay ? 0u : _rto >> 3;
        var lost = false;
        var fastRetransmit = false;

        foreach (var segment in _sendBuffer)
        {
            var send = false;

            if (segment.Transmissions == 0)
            {
                send = true;
                segment.Transmissions++;
                segment.Rto = _rto;
                segment.ResendTimestamp = _current + segment.Rto + minRto;
            }
            else if (TimeDiff(_current, segment.ResendTimestamp) >= 0)
            {
                send = true;
                segment.Transmissions++;
                _transmissions++;

                segment.Rto = _noDelay
                    ? segment.Rto + Math.Max(segment.Rto, _rto) / 2
                    : segment.Rto + Math.Max(segment.Rto, _rto);

                segment.Rto = Math.Min(segment.Rto, RtoMax);
                segment.ResendTimestamp = _current + segment.Rto;
                lost = true;
            }
            else if (segment.FastAck >= resent &&
                     (segment.Transmissions <= _fastLimit || _fastLimit == 0))
            {
                send = true;
                segment.Transmissions++;
                segment.FastAck = 0;
                segment.ResendTimestamp = _current + segment.Rto;
                fastRetransmit = true;
            }

            if (!send)
            {
                continue;
            }

            segment.Timestamp = _current;
            segment.Window = window;
            segment.Una = _receiveNext;

            FlushIfFull(ref offset, segment.Data.Length);
            WriteHeader(segment, _flushBuffer.AsSpan(offset), segment.Data.Length);
            segment.Data.CopyTo(_flushBuffer, offset + Overhead);
            offset += Overhead + segment.Data.Length;

            if (segment.Transmissions >= _deadLink)
            {
                IsDead = true;
            }
        }

        if (offset > 0)
        {
            _output(_flushBuffer.AsSpan(0, offset));
        }

        if (fastRetransmit)
        {
            var inflight = _sendNext - _sendUna;
            _ssthresh = Math.Max(inflight / 2, ThreshMin);
            _congestionWindow = _ssthresh + resent;
            _increment = _congestionWindow * (uint)_mss;
        }

        if (lost)
        {
            _ssthresh = Math.Max(congestionWindow / 2, ThreshMin);
            _congestionWindow = 1;
            _increment = (uint)_mss;
        }

        if (_congestionWindow < 1)
        {
            _congestionWindow = 1;
            _increment = (uint)_mss;
        }
    }

    public uint Check(uint currentMs)
    {
        if (!_updated)
        {
            return currentMs;
        }

        var flushTimestamp = _flushTimestamp;

        if (TimeDiff(currentMs, flushTimestamp) is >= 10000 or < -10000)
        {
            flushTimestamp = currentMs;
        }

        if (TimeDiff(currentMs, flushTimestamp) >= 0)
        {
            return currentMs;
        }

        var next = (uint)TimeDiff(flushTimestamp, currentMs);
        var minimum = next;

        foreach (var segment in _sendBuffer)
        {
            var diff = TimeDiff(segment.ResendTimestamp, currentMs);
            if (diff <= 0)
            {
                return currentMs;
            }

            minimum = Math.Min(minimum, (uint)diff);
        }

        return currentMs + Math.Min(minimum, _interval);
    }

    private void FlushIfFull(ref int offset, int payload)
    {
        if (offset + Overhead + payload > _mtu)
        {
            _output(_flushBuffer.AsSpan(0, offset));
            offset = 0;
        }
    }

    private void WriteHeader(Segment segment, Span<byte> destination, int length)
    {
        BinaryPrimitives.WriteUInt32LittleEndian(destination, Conversation);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[4..], Token);
        destination[8] = segment.Command;
        destination[9] = segment.Fragment;
        BinaryPrimitives.WriteUInt16LittleEndian(destination[10..], segment.Window);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[12..], segment.Timestamp);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[16..], segment.SequenceNumber);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[20..], segment.Una);
        BinaryPrimitives.WriteUInt32LittleEndian(destination[24..], (uint)length);
    }

    private int PeekSize()
    {
        if (_receiveQueue.Count == 0)
        {
            return -1;
        }

        var first = _receiveQueue.Peek();
        if (first.Fragment == 0)
        {
            return first.Data.Length;
        }

        if (_receiveQueue.Count <= first.Fragment)
        {
            return -1;
        }

        var length = 0;

        foreach (var segment in _receiveQueue)
        {
            length += segment.Data.Length;

            if (segment.Fragment == 0)
            {
                break;
            }
        }

        return length;
    }

    private void InsertReceived(Segment segment)
    {
        var index = _receiveBuffer.Count;
        var duplicate = false;

        for (var i = _receiveBuffer.Count - 1; i >= 0; i--)
        {
            var existing = _receiveBuffer[i];

            if (existing.SequenceNumber == segment.SequenceNumber)
            {
                duplicate = true;
                break;
            }

            if (TimeDiff(segment.SequenceNumber, existing.SequenceNumber) > 0)
            {
                index = i + 1;
                break;
            }

            index = i;
        }

        if (!duplicate)
        {
            _receiveBuffer.Insert(index, segment);
        }

        MoveToReceiveQueue();
    }

    private void MoveToReceiveQueue()
    {
        while (_receiveBuffer.Count > 0)
        {
            var first = _receiveBuffer[0];

            if (first.SequenceNumber != _receiveNext || _receiveQueue.Count >= _receiveWindow)
            {
                break;
            }

            _receiveBuffer.RemoveAt(0);
            _receiveQueue.Enqueue(first);
            _receiveNext++;
        }
    }

    private void RemoveAcknowledged(uint una)
    {
        _sendBuffer.RemoveAll(s => TimeDiff(una, s.SequenceNumber) > 0);
    }

    private void AcknowledgeOne(uint sn)
    {
        if (TimeDiff(sn, _sendUna) < 0 || TimeDiff(sn, _sendNext) >= 0)
        {
            return;
        }

        var index = _sendBuffer.FindIndex(s => s.SequenceNumber == sn);
        if (index >= 0)
        {
            _sendBuffer.RemoveAt(index);
        }
    }

    private void AcknowledgeFast(uint sn, uint timestamp)
    {
        if (TimeDiff(sn, _sendUna) < 0 || TimeDiff(sn, _sendNext) >= 0)
        {
            return;
        }

        foreach (var segment in _sendBuffer)
        {
            if (TimeDiff(sn, segment.SequenceNumber) < 0)
            {
                break;
            }

            if (sn != segment.SequenceNumber && TimeDiff(timestamp, segment.Timestamp) >= 0)
            {
                segment.FastAck++;
            }
        }
    }

    private void ShrinkSendBuffer()
    {
        _sendUna = _sendBuffer.Count > 0 ? _sendBuffer[0].SequenceNumber : _sendNext;
    }

    private void UpdateRto(uint rtt)
    {
        if (_rttSmoothed == 0)
        {
            _rttSmoothed = rtt;
            _rttVariance = rtt / 2;
        }
        else
        {
            var delta = rtt > _rttSmoothed ? rtt - _rttSmoothed : _rttSmoothed - rtt;
            _rttVariance = (3 * _rttVariance + delta) / 4;
            _rttSmoothed = (7 * _rttSmoothed + rtt) / 8;

            if (_rttSmoothed < 1)
            {
                _rttSmoothed = 1;
            }
        }

        var rto = _rttSmoothed + Math.Max(_interval, 4 * _rttVariance);
        _rto = (uint)Math.Clamp(rto, _minRto, RtoMax);
    }

    private void ProbeWindow()
    {
        if (_remoteWindow != 0)
        {
            _probeTimestamp = 0;
            _probeWait = 0;
            return;
        }

        if (_probeWait == 0)
        {
            _probeWait = ProbeInit;
            _probeTimestamp = _current + _probeWait;
            return;
        }

        if (TimeDiff(_current, _probeTimestamp) < 0)
        {
            return;
        }

        _probeWait = Math.Max(_probeWait, ProbeInit);
        _probeWait += _probeWait / 2;
        _probeWait = Math.Min(_probeWait, ProbeLimit);
        _probeTimestamp = _current + _probeWait;
        _probe |= AskSend;
    }

    private static int TimeDiff(uint later, uint earlier) => (int)(later - earlier);

    private sealed class Segment(byte[] data)
    {
        public byte[] Data { get; } = data;

        public uint Conversation { get; set; }

        public byte Command { get; set; }

        public byte Fragment { get; set; }

        public ushort Window { get; set; }

        public uint Timestamp { get; set; }

        public uint SequenceNumber { get; set; }

        public uint Una { get; set; }

        public uint ResendTimestamp { get; set; }

        public uint Rto { get; set; }

        public uint FastAck { get; set; }

        public uint Transmissions { get; set; }
    }
}
