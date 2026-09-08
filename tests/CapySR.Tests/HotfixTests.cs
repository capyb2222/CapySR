using CapySR.Common;
using Xunit;

namespace CapySR.Tests;

// A beta dispatch stops answering once its window closes, so a client that updated past
// everything in config/versions.json has to be served from the closest build on hand.
public class GameVersionTests
{
    private static readonly string[] Cached =
    [
        "OSBETAWin4.4.51",
        "CNBETAWin4.4.53",
        "CNBETAWin4.5.51",
        "CNBETAWin4.5.52",
    ];

    [Fact]
    public void VersionsParseIntoAChannelAndNumbers()
    {
        Assert.True(GameVersion.TryParse("CNBETAWin4.5.53", out var version));
        Assert.Equal("CNBETAWin", version.Channel);
        Assert.Equal([4, 5, 53], version.Parts);

        Assert.False(GameVersion.TryParse("nonsense", out _));
        Assert.False(GameVersion.TryParse("CNBETAWin4.5.x", out _));
        Assert.False(GameVersion.TryParse("", out _));
        Assert.False(GameVersion.TryParse(null, out _));
    }

    [Theory]
    [InlineData("CNBETAWin4.5.52", "CNBETAWin4.5.53", -1)]
    [InlineData("CNBETAWin4.5.53", "CNBETAWin4.5.52", 1)]
    [InlineData("CNBETAWin4.5.53", "CNBETAWin4.5.53", 0)]
    [InlineData("CNBETAWin4.9.0", "CNBETAWin4.10.0", -1)]
    // a missing trailing part counts as zero, so 4.5 and 4.5.0 are the same build
    [InlineData("CNBETAWin4.5", "CNBETAWin4.5.0", 0)]
    public void VersionsCompareNumerically(string left, string right, int expected)
    {
        Assert.True(GameVersion.TryParse(left, out var a));
        Assert.True(GameVersion.TryParse(right, out var b));
        Assert.Equal(expected, Math.Sign(a.CompareTo(b)));
    }

    // the case that matters: the client updated past anything the dispatch will still serve
    [Fact]
    public void ANewerClientBorrowsTheClosestOlderBuild()
    {
        Assert.True(GameVersion.TryFindNearest(Cached, "CNBETAWin4.5.53", out var nearest));
        Assert.Equal("CNBETAWin4.5.52", nearest);
    }

    [Fact]
    public void ChannelsNeverBorrowFromEachOther()
    {
        Assert.True(GameVersion.TryFindNearest(Cached, "OSBETAWin4.9.99", out var os));
        Assert.Equal("OSBETAWin4.4.51", os);

        Assert.False(GameVersion.TryFindNearest(Cached, "CNPRODWin4.5.53", out _));
        Assert.False(GameVersion.TryFindNearest(Cached, "nonsense", out _));
        Assert.False(GameVersion.TryFindNearest([], "CNBETAWin4.5.53", out _));
    }

    // nothing older to fall back on, so the newest known build beats having no urls at all
    [Fact]
    public void AnOlderClientStillGetsUrls()
    {
        Assert.True(GameVersion.TryFindNearest(Cached, "CNBETAWin3.0.0", out var nearest));
        Assert.Equal("CNBETAWin4.5.52", nearest);
    }

    [Fact]
    public void AnExactMatchIsPreferredOverEverythingElse()
    {
        Assert.True(GameVersion.TryFindNearest(Cached, "CNBETAWin4.5.51", out var nearest));
        Assert.Equal("CNBETAWin4.5.51", nearest);
    }
}
