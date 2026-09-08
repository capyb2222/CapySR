namespace CapySR.Data.Excel;

// res.json / Anchor.json ship with the zig servers, not with the ExcelOutput dump
public sealed class SceneResourceExcel
{
    public uint EntryID { get; set; }

    public uint PlaneID { get; set; }

    public List<ScenePlacement> Monsters { get; set; } = [];

    public List<ScenePropPlacement> Props { get; set; } = [];

    public List<SceneTeleport> Teleports { get; set; } = [];
}

public sealed class ScenePlacement
{
    public uint GroupId { get; set; }

    public uint InstId { get; set; }

    public uint MonsterId { get; set; }

    public uint EventId { get; set; }

    public ResVector Pos { get; set; } = new();

    public ResVector Rot { get; set; } = new();
}

public sealed class ScenePropPlacement
{
    public uint GroupId { get; set; }

    public uint InstId { get; set; }

    public uint PropId { get; set; }

    public uint PropState { get; set; }

    public ResVector Pos { get; set; } = new();

    public ResVector Rot { get; set; } = new();
}

public sealed class SceneTeleport
{
    public uint TeleportId { get; set; }

    public ResVector Pos { get; set; } = new();

    public ResVector Rot { get; set; } = new();
}

public sealed class AnchorExcel
{
    public uint EntryID { get; set; }

    public List<AnchorPoint> Anchor { get; set; } = [];
}

public sealed class AnchorPoint
{
    public uint ID { get; set; }

    public ResVector Pos { get; set; } = new();

    public ResVector Rot { get; set; } = new();
}

public sealed class ResVector
{
    public int X { get; set; }

    public int Y { get; set; }

    public int Z { get; set; }
}

public sealed class MazePropExcel
{
    public uint ID { get; set; }

    public string PropType { get; set; } = string.Empty;

    public string ConfigEntityPath { get; set; } = string.Empty;

    public string JsonPath { get; set; } = string.Empty;

    public bool IsDestructible => PropType is "PROP_DESTRUCT" or "PROP_NO_REWARD_DESTRUCT";

    // space anchors and sustenance anchors
    public bool IsSpring => PropType == "PROP_SPRING";

    // the orbs that refund technique points / heal when smashed; only the entity name tells
    public bool IsMpRecover => IsDestructible && (Mentions("MPBox") || Mentions("MPRecover"));

    public bool IsHpRecover => IsDestructible && (Mentions("HPBox") || Mentions("HPRecover"));

    private bool Mentions(string token) =>
        ConfigEntityPath.Contains(token, StringComparison.OrdinalIgnoreCase) ||
        JsonPath.Contains(token, StringComparison.OrdinalIgnoreCase);
}

public sealed class FloorSavedValuesExcel
{
    public uint FloorID { get; set; }

    public List<FloorSavedValue> SavedValues { get; set; } = [];
}

public sealed class FloorSavedValue
{
    public string Name { get; set; } = string.Empty;

    public int MaxValue { get; set; }
}
