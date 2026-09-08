using CapySR.Protocol;

namespace CapySR.GameServer.Game;

// One squad. Four slots that may have holes, exactly the way the client addresses them:
// every lineup packet carries a slot index, and the team screen redraws from whatever we
// echo back. The leader is remembered by avatar id so it follows the character when the
// team is swapped around.
public sealed class Lineup
{
    public const int Size = 4;

    private readonly uint[] _slots = new uint[Size];

    public string Name { get; set; } = string.Empty;

    public uint LeaderAvatarId { get; private set; }

    public IReadOnlyList<uint> Slots => _slots;

    public IEnumerable<(uint Slot, uint AvatarId)> Occupied =>
        _slots.Select((id, i) => ((uint)i, id)).Where(s => s.id != 0);

    // packed order, the way battles and scenes want it
    public List<uint> AvatarIds => [.. _slots.Where(id => id != 0)];

    public int Count => _slots.Count(id => id != 0);

    public bool IsEmpty => Count == 0;

    public uint LeaderSlot
    {
        get
        {
            var index = Array.IndexOf(_slots, LeaderAvatarId);
            return index >= 0 ? (uint)index : FirstOccupiedSlot();
        }
    }

    // position of the leader among the occupied slots
    public uint LeaderIndex
    {
        get
        {
            var ids = AvatarIds;
            var index = ids.IndexOf(LeaderAvatarId);
            return index >= 0 ? (uint)index : 0;
        }
    }

    public bool Contains(uint avatarId) => avatarId != 0 && Array.IndexOf(_slots, avatarId) >= 0;

    public uint At(uint slot) => slot < Size ? _slots[slot] : 0;

    public void Clear()
    {
        Array.Clear(_slots);
        LeaderAvatarId = 0;
    }

    public void Fill(IEnumerable<uint> avatarIds)
    {
        Clear();

        var next = 0;
        foreach (var id in avatarIds.Where(id => id != 0).Distinct().Take(Size))
        {
            _slots[next++] = id;
        }

        NormaliseLeader();
    }

    public void Replace(IEnumerable<(uint Slot, uint AvatarId)> entries, uint leaderSlot)
    {
        Array.Clear(_slots);

        foreach (var (slot, avatarId) in entries)
        {
            if (slot < Size && avatarId != 0 && Array.IndexOf(_slots, avatarId) < 0)
            {
                _slots[slot] = avatarId;
            }
        }

        LeaderAvatarId = leaderSlot < Size ? _slots[leaderSlot] : 0;
        NormaliseLeader();
    }

    public Retcode Join(uint avatarId, uint slot)
    {
        if (avatarId == 0)
        {
            return Retcode.RetLineupAvatarNotExist;
        }

        if (slot >= Size)
        {
            return Retcode.RetLineupInvalidMemberPos;
        }

        var current = Array.IndexOf(_slots, avatarId);
        if (current == (int)slot)
        {
            return Retcode.RetLineupAvatarAlreadyIn;
        }

        // joining an occupied slot swaps with whoever sat there
        if (current >= 0)
        {
            _slots[current] = _slots[slot];
        }

        _slots[slot] = avatarId;
        NormaliseLeader();
        return Retcode.RetSucc;
    }

    public Retcode Quit(uint avatarId)
    {
        var index = Array.IndexOf(_slots, avatarId);

        if (index < 0)
        {
            return Retcode.RetLineupAvatarNotExist;
        }

        if (Count <= 1)
        {
            return Retcode.RetLineupOnlyOneMember;
        }

        _slots[index] = 0;
        NormaliseLeader();
        return Retcode.RetSucc;
    }

    public Retcode Swap(uint a, uint b)
    {
        if (a >= Size || b >= Size)
        {
            return Retcode.RetLineupInvalidMemberPos;
        }

        if (a == b)
        {
            return Retcode.RetLineupSwapSameSlot;
        }

        (_slots[a], _slots[b]) = (_slots[b], _slots[a]);
        return Retcode.RetSucc;
    }

    public Retcode SetLeader(uint slot)
    {
        if (slot >= Size || _slots[slot] == 0)
        {
            return Retcode.RetLineupNotValidLeader;
        }

        LeaderAvatarId = _slots[slot];
        return Retcode.RetSucc;
    }

    public void SetLeaderAvatar(uint avatarId)
    {
        if (Contains(avatarId))
        {
            LeaderAvatarId = avatarId;
        }
    }

    // drop anything the roster no longer owns, e.g. after a new srtools upload
    public bool Prune(Func<uint, bool> owned)
    {
        var changed = false;

        for (var i = 0; i < Size; i++)
        {
            if (_slots[i] != 0 && !owned(_slots[i]))
            {
                _slots[i] = 0;
                changed = true;
            }
        }

        if (changed)
        {
            NormaliseLeader();
        }

        return changed;
    }

    private uint FirstOccupiedSlot()
    {
        for (var i = 0; i < Size; i++)
        {
            if (_slots[i] != 0)
            {
                return (uint)i;
            }
        }

        return 0;
    }

    private void NormaliseLeader()
    {
        if (!Contains(LeaderAvatarId))
        {
            LeaderAvatarId = _slots[FirstOccupiedSlot()];
        }
    }
}

// Every squad the client can edit by index, plus the virtual lineups a challenge swaps in.
// Technique points are a shared pool, so they live here rather than on a squad.
public sealed class LineupBook
{
    public const int SquadCount = 6;
    public const uint BaseMaxMp = 5;

    // Phainon and Himeko Nova each raise the cap by three while in the team
    private static readonly HashSet<uint> ExtraMpAvatars = [1408, 1510];

    private readonly Lineup[] _squads =
        [.. Enumerable.Range(0, SquadCount).Select(i => new Lineup { Name = $"Squad {i + 1}" })];

    private readonly Dictionary<ExtraLineupType, Lineup> _extra = [];

    private uint _mp = BaseMaxMp;

    public uint CurrentIndex { get; private set; }

    public ExtraLineupType ExtraType { get; private set; } = ExtraLineupType.LineupNone;

    public IReadOnlyList<Lineup> Squads => _squads;

    public Lineup Squad(uint index) => _squads[Math.Min(index, SquadCount - 1)];

    // the lineup that is actually walking around: a virtual one while it is active
    public Lineup Current =>
        ExtraType != ExtraLineupType.LineupNone && _extra.TryGetValue(ExtraType, out var extra)
            ? extra
            : _squads[CurrentIndex];

    public bool InExtraLineup => ExtraType != ExtraLineupType.LineupNone;

    public uint MaxMp => Current.AvatarIds.Any(ExtraMpAvatars.Contains) ? BaseMaxMp + 3 : BaseMaxMp;

    public uint Mp
    {
        get => Math.Min(_mp, MaxMp);
        set => _mp = Math.Min(value, MaxMp);
    }

    public Retcode SetCurrent(uint index)
    {
        if (index >= SquadCount)
        {
            return Retcode.RetLineupInvalidIndex;
        }

        if (_squads[index].IsEmpty)
        {
            return Retcode.RetLineupIsEmpty;
        }

        CurrentIndex = index;
        return Retcode.RetSucc;
    }

    public Lineup? Extra(ExtraLineupType type) => _extra.GetValueOrDefault(type);

    public Lineup SetExtra(ExtraLineupType type, IEnumerable<uint> avatarIds, bool activate = true)
    {
        var lineup = new Lineup { Name = string.Empty };
        lineup.Fill(avatarIds);
        _extra[type] = lineup;

        if (activate)
        {
            ExtraType = type;
        }

        return lineup;
    }

    public bool ActivateExtra(ExtraLineupType type)
    {
        if (type == ExtraLineupType.LineupNone)
        {
            ExtraType = ExtraLineupType.LineupNone;
            return true;
        }

        if (!_extra.TryGetValue(type, out var lineup) || lineup.IsEmpty)
        {
            return false;
        }

        ExtraType = type;
        return true;
    }

    public void ClearExtra()
    {
        _extra.Clear();
        ExtraType = ExtraLineupType.LineupNone;
    }

    public void RefillMp() => _mp = MaxMp;

    public bool SpendMp(uint count = 1)
    {
        if (Mp < count)
        {
            return false;
        }

        _mp = Mp - count;
        return true;
    }

    public void GainMp(uint count) => _mp = Math.Min(Mp + count, MaxMp);

    // the first squad gets the roster's first four; the rest start empty like a fresh account
    public void Seed(IEnumerable<uint> avatarIds)
    {
        if (_squads.All(s => s.IsEmpty))
        {
            _squads[0].Fill(avatarIds);
        }
    }

    public bool Prune(Func<uint, bool> owned)
    {
        var changed = false;

        foreach (var squad in _squads.Concat(_extra.Values))
        {
            changed |= squad.Prune(owned);
        }

        return changed;
    }
}
