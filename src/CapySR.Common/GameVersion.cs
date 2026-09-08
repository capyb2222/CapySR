using System.Diagnostics.CodeAnalysis;

namespace CapySR.Common;

// A client version reads as a channel followed by dotted numbers: CNBETAWin4.5.53.
// Ordering them matters because a beta dispatch stops answering once its window closes,
// so a build newer than anything on hand has to fall back to the closest one.
public readonly struct GameVersion(string channel, int[] parts) : IComparable<GameVersion>
{
    private static readonly char[] Digits = "0123456789".ToCharArray();

    public string Channel { get; } = channel;

    public int[] Parts { get; } = parts;

    public static bool TryParse(string? version, out GameVersion parsed)
    {
        parsed = default;

        if (string.IsNullOrEmpty(version))
        {
            return false;
        }

        var digit = version.IndexOfAny(Digits);

        if (digit <= 0)
        {
            return false;
        }

        var pieces = version[digit..].Split('.');
        var parts = new int[pieces.Length];

        for (var i = 0; i < pieces.Length; i++)
        {
            if (!int.TryParse(pieces[i], out parts[i]))
            {
                return false;
            }
        }

        parsed = new GameVersion(version[..digit], parts);
        return true;
    }

    public bool SameChannel(GameVersion other) =>
        Channel.Equals(other.Channel, StringComparison.OrdinalIgnoreCase);

    public int CompareTo(GameVersion other)
    {
        var length = Math.Max(Parts.Length, other.Parts.Length);

        for (var i = 0; i < length; i++)
        {
            var mine = i < Parts.Length ? Parts[i] : 0;
            var theirs = i < other.Parts.Length ? other.Parts[i] : 0;

            if (mine != theirs)
            {
                return mine.CompareTo(theirs);
            }
        }

        return 0;
    }

    // the closest build of the same channel: the newest one no later than what was asked
    // for, or failing that the newest there is. never crosses channels.
    public static bool TryFindNearest(IEnumerable<string> candidates, string wanted, [NotNullWhen(true)] out string? nearest)
    {
        nearest = null;

        if (!TryParse(wanted, out var target))
        {
            return false;
        }

        GameVersion older = default, any = default;
        var haveOlder = false;
        var haveAny = false;

        foreach (var candidate in candidates)
        {
            if (!TryParse(candidate, out var version) || !version.SameChannel(target))
            {
                continue;
            }

            if (!haveAny || version.CompareTo(any) > 0)
            {
                any = version;
                haveAny = true;

                if (!haveOlder)
                {
                    nearest = candidate;
                }
            }

            if (version.CompareTo(target) <= 0 && (!haveOlder || version.CompareTo(older) > 0))
            {
                older = version;
                haveOlder = true;
                nearest = candidate;
            }
        }

        return nearest is not null;
    }

    public override string ToString() => Channel + string.Join('.', Parts);
}
