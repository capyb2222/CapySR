namespace CapySR.Data.Excel;

// MOC / PF / AS floors. id ranges: <20000 MOC, 20000-30000 PF, >30000 AS.
// dimbreath keeps them in three files with one shape; the beta dump merges them.
public sealed class ChallengeMazeExcel
{
    public uint ID { get; set; }

    public uint GroupID { get; set; }

    public uint Floor { get; set; }

    public uint MapEntranceID { get; set; }

    public uint MapEntranceID2 { get; set; }

    public uint MazeGroupID1 { get; set; }

    public uint? MazeGroupID2 { get; set; }

    public uint MazeBuffID { get; set; }

    public uint ChallengeCountDown { get; set; }

    public uint StageNum { get; set; }

    public List<uint> ChallengeTargetID { get; set; } = [];

    public List<uint> NpcMonsterIDList1 { get; set; } = [];

    public List<uint> NpcMonsterIDList2 { get; set; } = [];

    public List<uint> EventIDList1 { get; set; } = [];

    public List<uint> EventIDList2 { get; set; } = [];

    public ChallengeKind Kind => ID switch
    {
        > 30000 => ChallengeKind.ApocalypticShadow,
        > 20000 => ChallengeKind.PureFiction,
        _ => ChallengeKind.MemoryOfChaos,
    };

    public bool HasSecondNode => MazeGroupID2 is > 0 && NpcMonsterIDList2.Count > 0 && EventIDList2.Count > 0;

    // StageNum is missing from the trimmed beta rows; fall back to what the node data says
    public int NodeCount => StageNum > 0 ? (int)Math.Min(StageNum, 2) : HasSecondNode ? 2 : 1;
}

public enum ChallengeKind
{
    MemoryOfChaos,
    PureFiction,
    ApocalypticShadow,
}

// Anomaly Arbitration. MapEntranceID/MazeGroupID/NpcMonsterIDList only exist in the beta dump.
public sealed class ChallengePeakExcel
{
    public uint ID { get; set; }

    public uint MapEntranceID { get; set; }

    public uint MazeGroupID { get; set; }

    public List<uint> NpcMonsterIDList { get; set; } = [];

    public List<uint> EventIDList { get; set; } = [];

    public List<uint> TagList { get; set; } = [];

    public List<uint> NormalTargetList { get; set; } = [];

    public List<string> DamageType { get; set; } = [];

    public bool IsPlayable => MapEntranceID > 0 && NpcMonsterIDList.Count > 0 && EventIDList.Count > 0;
}

public sealed class ChallengePeakBossExcel
{
    public uint ID { get; set; }

    public List<uint> BuffList { get; set; } = [];

    public List<uint> HardTagList { get; set; } = [];

    public List<uint> HardEventIDList { get; set; } = [];

    public uint HardTarget { get; set; }
}

// PF extras: the score targets the battle reports against
public sealed class ChallengeStoryExtraExcel
{
    public uint ID { get; set; }

    public uint TurnLimit { get; set; }

    public List<uint> BattleTargetID { get; set; } = [];

    public uint ClearScore { get; set; }
}

public sealed class ChallengeBossExtraExcel
{
    public uint ID { get; set; }

    public uint MonsterID1 { get; set; }

    public uint MonsterID2 { get; set; }
}

// star conditions for MOC (ROUNDS_LEFT / DEAD_AVATAR) and PF/AS (TOTAL_SCORE)
public sealed class ChallengeTargetExcel
{
    public uint ID { get; set; }

    public string ChallengeTargetType { get; set; } = string.Empty;

    public uint ChallengeTargetParam1 { get; set; }
}

public sealed class ChallengeGroupExcel
{
    public uint GroupID { get; set; }

    public string ChallengeGroupType { get; set; } = string.Empty;

    public uint RewardLineGroupID { get; set; }
}

public sealed class MapEntranceExcel
{
    public uint ID { get; set; }

    public uint PlaneID { get; set; }

    public uint FloorID { get; set; }
}

public sealed class MazePlaneExcel
{
    public uint PlaneID { get; set; }

    public uint WorldID { get; set; }

    public uint ChallengePlaneID { get; set; }

    public List<uint> FloorIDList { get; set; } = [];
}

public sealed class ChallengePeakGroupExcel
{
    public uint ID { get; set; }

    public uint BossLevelID { get; set; }

    public List<uint> PreLevelIDList { get; set; } = [];

    public IEnumerable<uint> AllStages => BossLevelID > 0 ? PreLevelIDList.Append(BossLevelID) : PreLevelIDList;
}

public sealed class MainMissionExcel
{
    public uint MainMissionID { get; set; }
}

public sealed class TutorialExcel
{
    public uint TutorialID { get; set; }
}

public sealed class TutorialGuideGroupExcel
{
    public uint GroupID { get; set; }
}

// maps an item id to its main/sub type. the client NPEs on an item it cannot type.
public sealed class ItemConfigExcel
{
    public uint ID { get; set; }
}

public sealed class MultiPathAvatarExcel
{
    public uint AvatarID { get; set; }

    public uint BaseAvatarID { get; set; }

    public string Gender { get; set; } = string.Empty;
}
