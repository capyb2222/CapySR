using CapySR.Data.SrTools;
using CapySR.Protocol;

namespace CapySR.GameServer.Battle;

public enum BattleOrigin
{
    Overworld,
    Stage,
    Cocoon,
    FarmElement,
    College,
    Challenge,
}

// one fight from the moment the client is told about it until it reports the result
public sealed class BattleInstance
{
    public required uint BattleId { get; init; }

    public required uint StageId { get; init; }

    public required BattleOrigin Origin { get; init; }

    public BattleKind Kind { get; init; } = BattleKind.Default;

    public required SceneBattleInfo Info { get; init; }

    public List<uint> Lineup { get; init; } = [];

    // scene entities that die when the fight is won
    public List<uint> MonsterEntityIds { get; init; } = [];

    public uint EventId { get; init; }

    public uint CocoonId { get; init; }

    public uint Wave { get; init; }

    public uint FarmElementId { get; init; }

    public uint CollegeId { get; init; }

    public bool UsesTrialAvatars { get; init; }

    public bool IsChallenge => Origin == BattleOrigin.Challenge;
}
