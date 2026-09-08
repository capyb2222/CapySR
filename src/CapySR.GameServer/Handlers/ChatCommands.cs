using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

// slash commands typed at the CapySR friend in chat
public static class ChatCommands
{
    private delegate Task Command(PlayerSession session, string[] args);

    private static readonly (string Name, string Usage, Command Run)[] Commands =
    [
        ("help", "/help - this list", HelpAsync),
        ("tp", "/tp <entryId> [teleportId] - travel to a map entrance", TeleportAsync),
        ("unstuck", "/unstuck - respawn at the current map's anchor", UnstuckAsync),
        ("heal", "/heal - full health and technique points", HealAsync),
        ("mp", "/mp [n] - set technique points (default max)", MpAsync),
        ("wl", "/wl <0-6> - set the equilibrium (world) level", WorldLevelAsync),
        ("stage", "/stage <stageId> - the next fight you start plays this stage", StageAsync),
        ("sync", "/sync - reload freesr-data.json from srtools now", SyncAsync),
        ("lineup", "/lineup - show the walking team", LineupAsync),
        ("pos", "/pos - show where you are", PositionAsync),
        ("clear", "/clear - remove every monster on the current map", ClearAsync),
    ];

    public static async Task RunAsync(PlayerSession session, string line)
    {
        var parts = line.Split(' ', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        if (parts.Length == 0)
        {
            await HelpAsync(session, []);
            return;
        }

        var name = parts[0].ToLowerInvariant();
        var command = Commands.FirstOrDefault(c => c.Name == name);

        if (command.Run is null)
        {
            await session.SendBotMessageAsync($"Unknown command /{name}. Try /help.");
            return;
        }

        session.Logger.LogInformation("command: /{Line}", line);

        try
        {
            await command.Run(session, parts[1..]);
        }
        catch (Exception e)
        {
            session.Logger.LogError(e, "command /{Name} failed", name);
            await session.SendBotMessageAsync($"/{name} failed: {e.Message}");
        }
    }

    private static Task HelpAsync(PlayerSession session, string[] args) =>
        session.SendBotMessageAsync(string.Join("\n", Commands.Select(c => c.Usage)));

    private static async Task TeleportAsync(PlayerSession session, string[] args)
    {
        if (args.Length == 0 || !uint.TryParse(args[0], out var entryId))
        {
            await session.SendBotMessageAsync("Usage: /tp <entryId> [teleportId]");
            return;
        }

        var data = session.World.Data;

        if (data.ResolveEntrance(entryId) is null && data.GetScene(entryId) is null)
        {
            await session.SendBotMessageAsync($"Entry {entryId} is not a known map entrance.");
            return;
        }

        var teleportId = args.Length > 1 && uint.TryParse(args[1], out var t) ? t : 0u;

        await session.SendBotMessageAsync($"Travelling to entry {entryId}.");
        await session.EnterWorldSceneAsync(entryId, teleportId);
    }

    private static async Task UnstuckAsync(PlayerSession session, string[] args)
    {
        await session.SendBotMessageAsync("Back to the anchor.");
        await session.EnterWorldSceneAsync(session.Player.EntryId, 0, EnterSceneReason.DimensionMerge);
    }

    private static async Task HealAsync(PlayerSession session, string[] args)
    {
        session.Player.HealAll(session.World.Player);
        session.Player.Lineups.RefillMp();
        session.Player.Touch();

        await session.SyncLineupAsync(SyncLineupReason.SyncReasonHpAdd);
        await session.SendBotMessageAsync("Team healed, technique points refilled.");
    }

    private static async Task MpAsync(PlayerSession session, string[] args)
    {
        var book = session.Player.Lineups;
        book.Mp = args.Length > 0 && uint.TryParse(args[0], out var n) ? n : book.MaxMp;
        session.Player.Touch();

        await session.SyncLineupAsync(SyncLineupReason.SyncReasonMpAdd);
        await session.SendBotMessageAsync($"Technique points: {book.Mp}/{book.MaxMp}.");
    }

    private static async Task WorldLevelAsync(PlayerSession session, string[] args)
    {
        if (args.Length == 0 || !uint.TryParse(args[0], out var level) || level > 6)
        {
            await session.SendBotMessageAsync($"Usage: /wl <0-6> (now {session.Player.WorldLevel})");
            return;
        }

        session.Player.WorldLevel = level;
        session.Player.Touch();

        await session.SendAsync(new PlayerSyncScNotify { BasicInfo = LoginHandlers.BasicInfo(session) });
        await session.SendBotMessageAsync($"Equilibrium level {level}. Monsters will fight at that level from the next encounter.");
    }

    private static async Task StageAsync(PlayerSession session, string[] args)
    {
        if (args.Length == 0 || !uint.TryParse(args[0], out var stageId))
        {
            await session.SendBotMessageAsync("Usage: /stage <stageId>");
            return;
        }

        if (session.World.Data.GetStage(stageId) is not { } stage)
        {
            await session.SendBotMessageAsync($"Stage {stageId} does not exist.");
            return;
        }

        session.Player.NextStageOverride = stageId;

        await session.SendBotMessageAsync(
            $"The next fight plays stage {stageId} ({stage.StageType}, level {stage.Level}, {stage.MonsterList.Count} wave(s)). Hit anything.");
    }

    private static async Task SyncAsync(PlayerSession session, string[] args)
    {
        session.World.Reload();
        await session.OnSrToolsReloadedAsync();
        await session.SendBotMessageAsync($"Reloaded srtools data: {session.World.OwnedAvatarIds.Count} avatars.");
    }

    private static Task LineupAsync(PlayerSession session, string[] args)
    {
        var book = session.Player.Lineups;
        var lineup = book.Current;
        var members = string.Join(", ", lineup.Slots.Select(id => id == 0 ? "-" : id.ToString()));

        return session.SendBotMessageAsync(
            $"{(book.InExtraLineup ? book.ExtraType.ToString() : lineup.Name)}: [{members}], leader {lineup.LeaderAvatarId}, " +
            $"technique points {book.Mp}/{book.MaxMp}.");
    }

    private static Task PositionAsync(PlayerSession session, string[] args)
    {
        var player = session.Player;
        var scene = session.Scene;
        var position = player.Position is { } p ? $"({p.X}, {p.Y}, {p.Z})" : "unknown";

        return session.SendBotMessageAsync(
            $"Entry {scene.EntryId} (plane {scene.PlaneId}, floor {scene.FloorId}), teleport {player.TeleportId}, position {position}, " +
            $"{scene.Monsters.Count()} monsters around.");
    }

    private static async Task ClearAsync(PlayerSession session, string[] args)
    {
        var monsters = session.Scene.Monsters.Select(m => m.EntityId).ToList();
        await session.RemoveEntitiesAsync(monsters);
        await session.SendBotMessageAsync($"Removed {monsters.Count} monster(s).");
    }
}
