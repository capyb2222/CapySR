using CapySR.Data;

namespace CapySR.GameServer.Battle;

public readonly record struct TechniqueBuff(uint Id, float SkillIndex, bool OwnedByLeader = false);

// technique buffs come from AvatarDefaultMazeBuff. the overrides below are the extras that
// table does not express, plus characters missing from it entirely (beta-only ones).
public static class TechniqueBuffs
{
    private const uint IgnoreToughnessBuff = 1000119;

    // female MC/March paths reuse the male path's buff ids
    private static uint BuffOwner(uint avatarId) => avatarId switch
    {
        8004 => 8003,
        8006 => 8005,
        8008 => 8007,
        8010 => 8009,
        _ => avatarId,
    };

    // extra buffs layered on top of whatever the data gives
    private static readonly Dictionary<uint, TechniqueBuff[]> Extras = new()
    {
        [1208] = [new TechniqueBuff(120801, 0)],
        [1224] = [new TechniqueBuff(122403, 0), new TechniqueBuff(122401, 0)],
        [1306] = [new TechniqueBuff(130601, 0)],
        [1309] = [new TechniqueBuff(130901, 0)],
        [1310] = [new TechniqueBuff(1000112, 0)],
        [1412] = [new TechniqueBuff(1000121, 0, OwnedByLeader: true)],
        [1414] = [new TechniqueBuff(1000121, 0, OwnedByLeader: true)],
        [1510] = [new TechniqueBuff(151001, 0)],
    };

    // avatars absent from AvatarDefaultMazeBuff - beta content the prod dump has not caught up to
    private static readonly Dictionary<uint, TechniqueBuff[]> Fallbacks = new()
    {
        [1503] = [new TechniqueBuff(150301, 0), new TechniqueBuff(1000121, 0, OwnedByLeader: true)],
    };

    // techniques that already ignore toughness, so we must not also add the element buff
    private static readonly HashSet<uint> IgnoresToughness = [1006, 1308, 1317, 1405];

    // techniques that defeat ordinary enemies outright instead of starting a fight
    private static readonly HashSet<uint> FodderKillers = [1308, 1408, 1506, 1510];

    public static bool IgnoresToughnessOnEntry(uint avatarId) => IgnoresToughness.Contains(BuffOwner(avatarId));

    public static bool KillsFodder(uint avatarId) => FodderKillers.Contains(BuffOwner(avatarId));

    public static IEnumerable<TechniqueBuff> For(GameData data, uint avatarId)
    {
        var owner = BuffOwner(avatarId);

        if (IgnoresToughness.Contains(owner))
        {
            yield return new TechniqueBuff(IgnoreToughnessBuff, 0);
        }

        if (Fallbacks.TryGetValue(owner, out var fallback))
        {
            foreach (var buff in fallback)
            {
                yield return buff;
            }
        }
        else if (data.DefaultMazeBuffs.TryGetValue(owner, out var row) && row.DefaultMazeBuffIDList.Count > 0)
        {
            foreach (var id in row.DefaultMazeBuffIDList)
            {
                yield return new TechniqueBuff(id, row.SkillIndex);
            }
        }
        else
        {
            // the shape the client expects when nothing else is known
            yield return new TechniqueBuff((owner * 100) + 1, 0);
        }

        if (Extras.TryGetValue(owner, out var extras))
        {
            foreach (var buff in extras)
            {
                yield return buff;
            }
        }
    }

    // the buff that breaks enemy toughness when this element starts the fight
    public static uint AttackerBuff(string damageType) => damageType switch
    {
        "Physical" => 1000111,
        "Fire" => 1000112,
        "Ice" => 1000113,
        "Thunder" => 1000114,
        "Wind" => 1000115,
        "Quantum" => 1000116,
        "Imaginary" => 1000117,
        _ => 0,
    };
}
