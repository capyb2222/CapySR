namespace CapySR.Data.Excel;

// Tables that decide what a fight is made of: where it is started from, which stage that
// resolves to, and who is lent to the player for it.

// Calyx. one row per (ID, WorldLevel); WorldLevel is absent from the level-0 row
public sealed class CocoonExcel
{
    public uint ID { get; set; }

    public uint WorldLevel { get; set; }

    public uint StageID { get; set; }

    public List<uint> StageIDList { get; set; } = [];

    public uint StaminaCost { get; set; }

    public uint MappingInfoID { get; set; }

    public string CocoonType { get; set; } = string.Empty;

    public IReadOnlyList<uint> Stages => StageIDList.Count > 0 ? StageIDList : StageID != 0 ? [StageID] : [];
}

// Stagnant Shadow / Cavern of Corrosion. same (ID, WorldLevel) shape as cocoons
public sealed class FarmElementExcel
{
    public uint ID { get; set; }

    public uint WorldLevel { get; set; }

    public uint StageID { get; set; }

    public uint StaminaCost { get; set; }

    public uint MappingInfoID { get; set; }
}

// an overworld monster's EventID resolves to a stage through here, per world level
public sealed class PlaneEventExcel
{
    public uint EventID { get; set; }

    public uint WorldLevel { get; set; }

    public uint StageID { get; set; }
}

public sealed class BattleCollegeExcel
{
    public uint ID { get; set; }

    public uint StageID { get; set; }

    public List<uint> TrialAvatarList { get; set; } = [];
}

// the win conditions a battle reports progress against
public sealed class BattleTargetExcel
{
    public uint ID { get; set; }

    public string Type { get; set; } = string.Empty;

    public uint TargetParam { get; set; }
}

// PF floors with an invading monster: the stage carries an extra buff 3034000 + id
public sealed class StageInvasionExcel
{
    public uint StageID { get; set; }

    public uint InvasionID { get; set; }
}

// trial characters lent to a stage: battle college, story fights, ...
public sealed class SpecialAvatarExcel
{
    public uint SpecialAvatarID { get; set; }

    public uint AvatarID { get; set; }

    public uint PlayerID { get; set; }

    public uint Level { get; set; }

    public uint Promotion { get; set; }

    public uint EquipmentID { get; set; }

    public uint EquipmentLevel { get; set; }

    public uint EquipmentPromotion { get; set; }

    public uint EquipmentRank { get; set; }

    public bool IsUseWorldLevel { get; set; }
}

public sealed class NpcMonsterExcel
{
    public uint ID { get; set; }

    public string Rank { get; set; } = string.Empty;

    // techniques that one-shot fodder (Acheron, Phainon, ...) skip the fight for these
    public bool IsFodder => Rank is "Minion" or "MinionLv2";
}
