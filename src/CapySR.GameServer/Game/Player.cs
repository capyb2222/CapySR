using CapySR.Common;
using CapySR.Data.SrTools;
using CapySR.Protocol;

namespace CapySR.GameServer.Game;

// everything the account remembers between packets. srtools owns the roster and gear;
// this is the rest: profile, teams, where the player stands, how the fights went.
public sealed class Player(GameServerConfig config)
{
    public const uint FullHp = 10_000;

    public uint Uid { get; } = config.Uid;

    public string Nickname { get; set; } = config.Nickname;

    public string Signature { get; set; } = config.Signature;

    public uint Level { get; set; } = config.Level;

    public uint WorldLevel { get; set; } = config.WorldLevel;

    public uint Stamina { get; set; } = 240;

    public uint HeadIcon { get; set; } = 201001;

    public Gender Gender { get; set; } = Gender.Woman;

    public LanguageType Language { get; set; } = LanguageType.LanguageEn;

    public bool LoggedIn { get; set; }

    public uint EntryId { get; set; } = config.StartEntryId;

    public uint TeleportId { get; set; } = config.StartTeleportId;

    // last position the client reported; null until it moves
    public PlayerPosition? Position { get; set; }

    public uint TrailblazerPath { get; set; } = 8002;

    public uint MarchPath { get; set; } = 1001;

    public LineupBook Lineups { get; } = new();

    public Dictionary<uint, AvatarState> AvatarStates { get; } = [];

    // avatars switched to their enhanced kit from the character screen
    public HashSet<uint> EnhancedAvatars { get; } = [];

    public Dictionary<uint, ChallengeRecord> ChallengeRecords { get; } = [];

    // keyed by peak id * 2 + (hard ? 1 : 0)
    public Dictionary<uint, PeakRecord> PeakRecords { get; } = [];

    // the mob-stage teams picked on the AA screen, by peak id
    public Dictionary<uint, List<uint>> PeakLineups { get; } = [];

    public Dictionary<uint, byte[]> ServerPrefs { get; } = [];

    public HashSet<uint> FinishedColleges { get; } = [];

    // the characters shown on the profile card, in order
    public List<uint> DisplayAvatars { get; } = [];

    // set by /stage; the next fight plays it instead of whatever was hit
    public uint NextStageOverride { get; set; }

    public bool Dirty { get; private set; }

    public void Touch() => Dirty = true;

    public void ClearDirty() => Dirty = false;

    public AvatarState StateOf(uint avatarId, SrToolsData srtools)
    {
        if (!AvatarStates.TryGetValue(avatarId, out var state))
        {
            state = AvatarState.From(srtools.Avatars.GetValueOrDefault(avatarId));
            AvatarStates[avatarId] = state;
        }

        return state;
    }

    public void HealAll(SrToolsData srtools)
    {
        foreach (var state in AvatarStates.Values)
        {
            state.Hp = FullHp;
        }

        foreach (var avatarId in Lineups.Current.AvatarIds)
        {
            StateOf(avatarId, srtools).Hp = FullHp;
        }
    }

    // challenges start every member at full health and half energy
    public void PrepareForChallenge(IEnumerable<uint> avatarIds, SrToolsData srtools)
    {
        foreach (var avatarId in avatarIds)
        {
            var state = StateOf(avatarId, srtools);
            state.Hp = FullHp;
            state.SpCur = state.SpMax / 2;
        }
    }

    public bool IsEnhanced(uint avatarId, SrToolsData srtools)
    {
        if (EnhancedAvatars.Contains(avatarId))
        {
            return true;
        }

        return srtools.Avatars.GetValueOrDefault(avatarId)?.EnhancedId is > 0;
    }

    public PeakRecord? PeakRecord(uint peakId, bool hard) => PeakRecords.GetValueOrDefault((peakId * 2) + (hard ? 1u : 0u));

    public void SetPeakRecord(uint peakId, bool hard, PeakRecord record)
    {
        PeakRecords[(peakId * 2) + (hard ? 1u : 0u)] = record;
        Touch();
    }
}

public sealed class AvatarState
{
    public uint Hp { get; set; } = Player.FullHp;

    // energy as (current, max) in the avatar's own units
    public uint SpCur { get; set; } = 10_000;

    public uint SpMax { get; set; } = 10_000;

    public static AvatarState From(SrAvatar? configured) => new()
    {
        SpCur = configured?.SpValue ?? 10_000,
        SpMax = configured?.SpMax ?? 10_000,
    };

    public SpBarInfo SpBar => new() { CurSp = SpCur, MaxSp = SpMax };
}

public sealed class PlayerPosition
{
    public int X { get; set; }

    public int Y { get; set; }

    public int Z { get; set; }

    public int RotY { get; set; }

    public MotionInfo ToMotion() => new()
    {
        Pos = new Protocol.Vector { X = X, Y = Y, Z = Z },
        Rot = new Protocol.Vector { Y = RotY },
    };

    public static PlayerPosition From(MotionInfo motion) => new()
    {
        X = motion.Pos?.X ?? 0,
        Y = motion.Pos?.Y ?? 0,
        Z = motion.Pos?.Z ?? 0,
        RotY = motion.Rot?.Y ?? 0,
    };
}

public sealed class ChallengeRecord
{
    public uint Stars { get; set; }

    public uint Score { get; set; }

    public uint ScoreTwo { get; set; }
}

public sealed class PeakRecord
{
    public uint Stars { get; set; }

    public uint Cycles { get; set; }

    public uint BuffId { get; set; }

    public List<uint> FinishedTargets { get; set; } = [];

    public List<uint> Avatars { get; set; } = [];
}
