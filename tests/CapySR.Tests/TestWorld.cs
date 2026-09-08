using CapySR.Common;
using CapySR.GameServer.Game;
using Microsoft.Extensions.Logging.Abstractions;

namespace CapySR.Tests;

// The game data is a separate checkout, so a fresh clone has none. Tests that need it check
// Available and return instead of failing. Building the world is deferred for the same
// reason, and shared because loading the tables is the slow part of the suite.
internal static class TestWorld
{
    private static readonly Lazy<GameWorld> Instance =
        new(() => new GameWorld(new ServerConfig(), NullLogger.Instance));

    public static bool Available { get; } = new DataConfig().ResolvedSources.Any(Directory.Exists);

    public static GameWorld Current => Instance.Value;
}
