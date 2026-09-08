using CapySR.Data;
using CapySR.Data.Excel;
using CapySR.Protocol;

namespace CapySR.GameServer.Game;

// the floor the session is running: a MOC/PF/AS floor with up to two nodes, or an AA stage
public sealed class ChallengeState
{
    public bool Active { get; private set; }

    public bool IsPeak { get; private set; }

    public uint ChallengeId { get; private set; }

    public ChallengeKind Kind { get; private set; }

    // 0 = first half, 1 = second half
    public int Node { get; private set; }

    // a toggle the client sets before starting, not per-run state, so Clear leaves it alone
    public bool HardMode { get; set; }

    public ChallengeMazeExcel? Config { get; private set; }

    public ChallengePeakExcel? Peak { get; private set; }

    public ChallengePeakBossExcel? Boss { get; private set; }

    public List<uint>[] Lineups { get; } = [[], []];

    public uint[] SelectedBuffs { get; } = new uint[2];

    public uint[] Scores { get; } = new uint[2];

    public uint MazeBuffId { get; private set; }

    public uint Stars { get; private set; }

    public uint RoundsLeft { get; set; }

    public uint DeadAvatars { get; set; }

    public uint SavedMp { get; set; }

    public ChallengeStatus Status { get; private set; } = ChallengeStatus.ChallengeUnknown;

    // AA: the battle targets in play, so stars can be read back from the result
    public List<uint> PeakTargets { get; } = [];

    public uint StageId { get; private set; }

    public uint CycleCount { get; private set; } = 30;

    public uint FloorId { get; private set; }

    public uint PlaneId { get; private set; }

    public uint WorldId { get; private set; }

    public uint EntryId { get; private set; }

    public uint GroupId { get; private set; }

    public uint MonsterId { get; private set; }

    public uint EventId { get; private set; }

    public List<uint> Lineup => Lineups[Node];

    public int NodeCount => IsPeak ? 1 : Config?.NodeCount ?? 1;

    public bool IsLastNode => Node >= NodeCount - 1;

    public bool Finished => Status is ChallengeStatus.ChallengeFinish or ChallengeStatus.ChallengeFailed;

    public uint TotalScore => Scores[0] + Scores[1];

    public ExtraLineupType LineupType => Node == 0 ? ExtraLineupType.LineupChallenge : ExtraLineupType.LineupChallenge2;

    // the maze buff always applies; the player's pick for the node is layered on top
    public List<uint> Blessings
    {
        get
        {
            var list = new List<uint>();

            if (IsPeak)
            {
                if (SelectedBuffs[0] != 0)
                {
                    list.Add(SelectedBuffs[0]);
                }

                list.AddRange(HardMode && Boss is not null ? Boss.HardTagList : Peak?.TagList ?? []);
                return list;
            }

            if (MazeBuffId != 0)
            {
                list.Add(MazeBuffId);
            }

            if (SelectedBuffs[Node] != 0)
            {
                list.Add(SelectedBuffs[Node]);
            }

            return list;
        }
    }

    public void Clear()
    {
        Active = false;
        IsPeak = false;
        ChallengeId = 0;
        Node = 0;
        Config = null;
        Peak = null;
        Boss = null;
        Lineups[0].Clear();
        Lineups[1].Clear();
        Array.Clear(SelectedBuffs);
        Array.Clear(Scores);
        MazeBuffId = 0;
        Stars = 0;
        RoundsLeft = 0;
        DeadAvatars = 0;
        SavedMp = 0;
        Status = ChallengeStatus.ChallengeUnknown;
        PeakTargets.Clear();
        StageId = 0;
        CycleCount = 30;
        FloorId = PlaneId = WorldId = EntryId = GroupId = MonsterId = EventId = 0;
    }

    // single-node convenience: the node's lineup and pick, nothing for the other half
    public bool StartChallenge(GameData data, ChallengeMazeExcel challenge, int node, uint selectedBuff, IEnumerable<uint> lineup)
    {
        var first = node == 0 ? lineup : [];
        var second = node == 1 ? lineup : [];

        return Start(data, challenge, first, second, node == 0 ? selectedBuff : 0, node == 1 ? selectedBuff : 0)
               && (node == 0 || SwitchNode(data, 1));
    }

    public bool Start(
        GameData data, ChallengeMazeExcel challenge,
        IEnumerable<uint> first, IEnumerable<uint> second, uint buffOne, uint buffTwo)
    {
        Clear();

        Config = challenge;
        ChallengeId = challenge.ID;
        Kind = challenge.Kind;
        MazeBuffId = challenge.MazeBuffID;
        SelectedBuffs[0] = buffOne;
        SelectedBuffs[1] = buffTwo;
        Lineups[0].AddRange(first.Where(id => id != 0).Distinct().Take(4));
        Lineups[1].AddRange(second.Where(id => id != 0).Distinct().Take(4));
        CycleCount = challenge.ChallengeCountDown == 0 ? 30 : challenge.ChallengeCountDown;
        RoundsLeft = CycleCount;
        Status = ChallengeStatus.ChallengeDoing;

        if (!ResolveNode(data, 0))
        {
            Clear();
            return false;
        }

        Active = true;
        return true;
    }

    public bool SwitchNode(GameData data, int node)
    {
        if (Config is null || node < 0 || node >= NodeCount)
        {
            return false;
        }

        if (!ResolveNode(data, node))
        {
            return false;
        }

        Node = node;
        return true;
    }

    private bool ResolveNode(GameData data, int node)
    {
        var challenge = Config!;
        var useSecond = node == 1 && challenge.HasSecondNode;

        var entranceId = useSecond && challenge.MapEntranceID2 != 0 ? challenge.MapEntranceID2 : challenge.MapEntranceID;
        var monsters = useSecond ? challenge.NpcMonsterIDList2 : challenge.NpcMonsterIDList1;
        var events = useSecond ? challenge.EventIDList2 : challenge.EventIDList1;
        var group = useSecond ? challenge.MazeGroupID2 ?? challenge.MazeGroupID1 : challenge.MazeGroupID1;

        if (monsters.Count == 0 || events.Count == 0)
        {
            return false;
        }

        if (data.ResolveEntrance(entranceId) is not { } resolved)
        {
            return false;
        }

        // the last entry is the real encounter; earlier ones are approach mobs
        MonsterId = monsters[^1];
        EventId = events[^1];
        StageId = EventId;

        EntryId = entranceId;
        FloorId = resolved.Entrance.FloorID;
        PlaneId = resolved.Plane?.ChallengePlaneID is > 0 ? resolved.Plane.ChallengePlaneID : resolved.Entrance.PlaneID;
        WorldId = resolved.Plane?.WorldID ?? 0;
        GroupId = group;

        return true;
    }

    public bool StartPeak(GameData data, ChallengePeakExcel peak, ChallengePeakBossExcel? boss, uint selectedBuff, IEnumerable<uint> lineup)
    {
        Clear();

        if (!peak.IsPlayable || data.ResolveEntrance(peak.MapEntranceID) is not { } resolved)
        {
            return false;
        }

        Active = true;
        IsPeak = true;
        Peak = peak;
        Boss = boss;
        ChallengeId = peak.ID;
        Kind = ChallengeKind.ApocalypticShadow;
        Status = ChallengeStatus.ChallengeDoing;
        SelectedBuffs[0] = selectedBuff;
        Lineups[0].AddRange(lineup.Where(id => id != 0).Distinct().Take(4));

        // hard mode swaps the whole tag set, and can swap the encounter too
        var events = peak.EventIDList;

        if (HardMode && boss is not null && boss.HardEventIDList.Count > 0)
        {
            events = boss.HardEventIDList;
        }

        if (HardMode && boss is not null && boss.HardTarget != 0)
        {
            PeakTargets.Add(boss.HardTarget);
        }
        else
        {
            PeakTargets.AddRange(peak.NormalTargetList);
        }

        MonsterId = peak.NpcMonsterIDList[^1];
        EventId = events[^1];
        StageId = EventId;

        EntryId = peak.MapEntranceID;
        FloorId = resolved.Entrance.FloorID;
        PlaneId = resolved.Plane?.ChallengePlaneID is > 0 ? resolved.Plane.ChallengePlaneID : resolved.Entrance.PlaneID;
        WorldId = resolved.Plane?.WorldID ?? 0;
        GroupId = peak.MazeGroupID;

        return true;
    }

    public void Finish(uint stars)
    {
        Stars = stars;
        Status = ChallengeStatus.ChallengeFinish;
    }

    public void Fail() => Status = ChallengeStatus.ChallengeFailed;

    // MOC stars are rounds and deaths, PF/AS stars are total score
    public uint CalculateStars(GameData data)
    {
        if (Config is null)
        {
            return 0;
        }

        var stars = 0u;
        var targets = Config.ChallengeTargetID;

        for (var i = 0; i < targets.Count && i < 3; i++)
        {
            if (!data.ChallengeTargets.TryGetValue(targets[i], out var target))
            {
                continue;
            }

            var met = target.ChallengeTargetType switch
            {
                "ROUNDS_LEFT" => RoundsLeft >= target.ChallengeTargetParam1,
                "DEAD_AVATAR" => DeadAvatars == 0,
                "TOTAL_SCORE" => TotalScore >= target.ChallengeTargetParam1,
                _ => false,
            };

            if (met)
            {
                stars |= 1u << i;
            }
        }

        // a floor with no target rows still counts a win as full marks
        return targets.Count == 0 ? 7 : stars;
    }
}
