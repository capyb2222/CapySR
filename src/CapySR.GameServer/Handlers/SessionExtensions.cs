using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.GameServer.Scene;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

public static class SessionExtensions
{
    // base ids of the team walking around, or the first four the account owns
    public static List<uint> DefaultLineup(this PlayerSession session)
    {
        var current = session.Player.Lineups.Current.AvatarIds;

        if (current.Count > 0)
        {
            return current;
        }

        return [.. session.World.OwnedAvatarIds.Select(session.World.Data.BaseAvatarId).Distinct().Take(Lineup.Size)];
    }

    public static SceneInfo CurrentScene(this PlayerSession session)
    {
        if (session.Scene.Info is { } info)
        {
            return info;
        }

        var scene = session.Challenge.Active
            ? session.World.Scenes.BuildChallengeScene(session.Challenge)
            : session.World.Scenes.BuildWorldScene(
                session.Player.EntryId, session.DefaultLineup(), session.Player.TeleportId,
                session.Config.GameServer.WorldId, session.Player.Position);

        session.Scene.Load(scene, session.Challenge.Active);
        return scene;
    }

    public static LineupInfo BuildLineupInfo(this PlayerSession session, Lineup lineup, ExtraLineupType type, uint index)
    {
        var book = session.Player.Lineups;
        var isCurrent = ReferenceEquals(lineup, book.Current);
        var maxMp = isCurrent ? book.MaxMp : MaxMpFor(lineup);

        var planeId = session.Scene.PlaneId != 0
            ? session.Scene.PlaneId
            : session.World.Data.ResolveEntrance(session.Player.EntryId)?.Entrance.PlaneID ?? 0;

        var info = new LineupInfo
        {
            Name = lineup.Name,
            ExtraLineupType = type,
            LeaderSlot = lineup.LeaderSlot,
            Mp = Math.Min(book.Mp, maxMp),
            MaxMp = maxMp,
            PlaneId = planeId,
            Index = type == ExtraLineupType.LineupNone ? index : 0,
            IsVirtual = type != ExtraLineupType.LineupNone,
        };

        foreach (var (slot, avatarId) in lineup.Occupied)
        {
            var path = Roster.ResolvePath(session.World, session.Player, avatarId);
            var state = session.Player.StateOf(path, session.World.Player);

            info.AvatarList.Add(new LineupAvatar
            {
                Id = avatarId,
                Slot = slot,
                Hp = state.Hp,
                Satiety = 100,
                AvatarType = AvatarType.AvatarFormalType,
                SpBar = state.SpBar,
            });
        }

        return info;
    }

    private static uint MaxMpFor(Lineup lineup) =>
        lineup.AvatarIds.Any(id => id is 1408 or 1510) ? LineupBook.BaseMaxMp + 3 : LineupBook.BaseMaxMp;

    public static LineupInfo BuildWorldLineup(this PlayerSession session)
    {
        var book = session.Player.Lineups;
        return session.BuildLineupInfo(book.Current, book.ExtraType, book.CurrentIndex);
    }

    public static LineupInfo BuildChallengeLineup(this PlayerSession session, int node)
    {
        var type = node == 0 ? ExtraLineupType.LineupChallenge : ExtraLineupType.LineupChallenge2;
        var lineup = session.Player.Lineups.Extra(type)
                     ?? session.Player.Lineups.SetExtra(type, session.Challenge.Lineups[node], activate: false);

        return session.BuildLineupInfo(lineup, type, 0);
    }

    // the client only redraws the team once the server confirms it
    public static Task SyncLineupAsync(this PlayerSession session, SyncLineupReason reason = SyncLineupReason.SyncReasonNone)
    {
        var notify = new SyncLineupNotify { Lineup = session.BuildWorldLineup() };

        if (reason != SyncLineupReason.SyncReasonNone)
        {
            notify.ReasonList.Add(reason);
        }

        return session.SendAsync(notify);
    }

    // the overworld models are the scene's actor entities; swap them in place instead of
    // reloading the whole scene when the team changes
    public static async Task RefreshSceneActorsAsync(this PlayerSession session)
    {
        var scene = session.Scene;

        if (scene.Info is null)
        {
            return;
        }

        var data = session.World.Data;
        var lineup = session.Player.Lineups.Current;
        var desired = lineup.AvatarIds.Select(data.BaseAvatarId).Distinct().ToList();
        var existing = scene.Actors.ToList();

        var removed = existing.Where(a => !desired.Contains(a.AvatarId)).ToList();
        var added = desired.Where(id => existing.All(a => a.AvatarId != id)).ToList();

        var leaderEntityId = SceneBuilder.ActorEntityId(data.BaseAvatarId(lineup.LeaderAvatarId));

        if (removed.Count == 0 && added.Count == 0)
        {
            scene.Info.LeaderEntityId = leaderEntityId;
            return;
        }

        var motion = session.Player.Position?.ToMotion()
                     ?? existing.FirstOrDefault()?.Motion
                     ?? session.World.Scenes.SpawnMotion(scene.EntryId, session.Player.TeleportId, null);

        var actors = desired.Select(id => session.World.Scenes.BuildActor(id, motion)).ToList();
        scene.ReplaceActors(actors, leaderEntityId);

        var group = new GroupRefreshInfo
        {
            GroupId = 0,
            RefreshType = removed.Count > 0 ? SceneGroupRefreshType.Afibfmafncc : SceneGroupRefreshType.Loaded,
        };

        foreach (var actor in removed)
        {
            group.RefreshEntity.Add(new SceneEntityRefreshInfo { DeleteEntity = actor.EntityId });
        }

        foreach (var actor in actors.Where(a => added.Contains(a.Actor.BaseAvatarId)))
        {
            group.RefreshEntity.Add(new SceneEntityRefreshInfo { AddEntity = actor });
        }

        await session.SendAsync(new SceneGroupRefreshScNotify
        {
            FloorId = scene.FloorId,
            GroupRefreshList = { group },
        });
    }

    public static async Task RemoveEntitiesAsync(this PlayerSession session, IEnumerable<uint> entityIds)
    {
        var scene = session.Scene;
        var groups = new Dictionary<uint, GroupRefreshInfo>();

        foreach (var entityId in entityIds.Distinct())
        {
            var entity = scene.Get(entityId);

            if (entity is null || !scene.Remove(entityId))
            {
                continue;
            }

            if (!groups.TryGetValue(entity.GroupId, out var group))
            {
                groups[entity.GroupId] = group = new GroupRefreshInfo
                {
                    GroupId = entity.GroupId,
                    RefreshType = SceneGroupRefreshType.Afibfmafncc,
                };
            }

            group.RefreshEntity.Add(new SceneEntityRefreshInfo { DeleteEntity = entityId });
        }

        if (groups.Count == 0)
        {
            return;
        }

        var notify = new SceneGroupRefreshScNotify { FloorId = scene.FloorId };
        notify.GroupRefreshList.AddRange(groups.Values);
        await session.SendAsync(notify);
    }

    // a real scene change: teleport, map travel, leaving a challenge
    public static async Task EnterWorldSceneAsync(
        this PlayerSession session, uint entryId, uint teleportId, EnterSceneReason reason = EnterSceneReason.None,
        bool keepPosition = false)
    {
        var player = session.Player;
        var world = session.World;

        session.Challenge.Clear();
        session.Battles.Quit();
        player.Lineups.ClearExtra();

        if (world.Data.ResolveEntrance(entryId) is null && world.Data.GetScene(entryId) is null)
        {
            session.Logger.LogWarning("entry {Entry} is unknown; staying at {Current}", entryId, player.EntryId);
            entryId = player.EntryId;
        }

        player.EntryId = entryId;
        player.TeleportId = teleportId;

        if (!keepPosition)
        {
            player.Position = null;
        }

        player.Lineups.RefillMp();
        player.HealAll(world.Player);
        player.Touch();

        var scene = world.Scenes.BuildWorldScene(
            entryId, session.DefaultLineup(), teleportId, session.Config.GameServer.WorldId,
            keepPosition ? player.Position : null);

        session.Scene.Load(scene, isChallenge: false);

        await session.SendAsync(new EnterSceneByServerScNotify
        {
            Reason = reason,
            Lineup = session.BuildWorldLineup(),
            Scene = scene,
        });

        await session.NotifySpawnPositionAsync(scene);
    }

    public static async Task EnterChallengeSceneAsync(this PlayerSession session)
    {
        var scene = session.World.Scenes.BuildChallengeScene(session.Challenge);
        session.Scene.Load(scene, isChallenge: true);

        await session.SendAsync(new EnterSceneByServerScNotify
        {
            Reason = EnterSceneReason.None,
            Lineup = session.BuildWorldLineup(),
            Scene = scene,
        });

        await session.NotifySpawnPositionAsync(scene);
    }

    // the client places actors where the move notify says, so repeat the spawn for each
    public static async Task NotifySpawnPositionAsync(this PlayerSession session, SceneInfo scene)
    {
        foreach (var group in scene.EntityGroupList)
        {
            foreach (var entity in group.EntityList.Where(e => e.Actor is not null && e.Motion is not null))
            {
                await session.SendAsync(new SceneEntityMoveScNotify
                {
                    EntityId = entity.EntityId,
                    EntryId = scene.EntryId,
                    Motion = entity.Motion,
                });
            }
        }
    }

    public static async Task MoveActorsAsync(this PlayerSession session, MotionInfo motion)
    {
        var scene = session.Scene;

        foreach (var actor in scene.Actors.ToList())
        {
            await session.SendAsync(new SceneEntityMoveScNotify
            {
                EntityId = actor.EntityId,
                EntryId = scene.EntryId,
                Motion = motion,
            });
        }

        session.Player.Position = PlayerPosition.From(motion);
    }

    // heal the walking team; the fallen stay down, as in the real thing
    public static void HealParty(this PlayerSession session, uint amount)
    {
        foreach (var avatarId in session.Player.Lineups.Current.AvatarIds)
        {
            var path = Roster.ResolvePath(session.World, session.Player, avatarId);
            var state = session.Player.StateOf(path, session.World.Player);

            if (state.Hp > 0)
            {
                state.Hp = Math.Min(Player.FullHp, state.Hp + amount);
            }
        }
    }

    public static Task SyncAvatarsAsync(this PlayerSession session)
    {
        var sync = new PlayerSyncScNotify { AvatarSync = new AvatarSync() };
        sync.AvatarSync.AvatarList.AddRange(Roster.BuildAvatars(session.World, session.Player));
        sync.AvatarSync.AvatarPathDataInfoList.AddRange(Roster.BuildPathData(session.World, session.Player));
        return session.SendAsync(sync);
    }

    // a fresh srtools upload: new roster, new gear, teams pruned to what still exists
    public static async Task OnSrToolsReloadedAsync(this PlayerSession session)
    {
        var world = session.World;
        var player = session.Player;

        session.Logger.LogInformation("srtools data changed; pushing {Avatars} avatars to the client", world.OwnedAvatarIds.Count);

        var owned = world.OwnedAvatarIds.Select(world.Data.BaseAvatarId).Distinct().ToList();
        player.Lineups.Prune(id => owned.Contains(id));
        player.Lineups.Seed(owned.Take(Lineup.Size));

        if (player.Lineups.Current.IsEmpty)
        {
            player.Lineups.Squad(player.Lineups.CurrentIndex).Fill(owned.Take(Lineup.Size));
        }

        player.AvatarStates.Clear();
        player.Touch();

        var sync = new PlayerSyncScNotify { AvatarSync = new AvatarSync() };
        sync.AvatarSync.AvatarList.AddRange(Roster.BuildAvatars(world, player));
        sync.AvatarSync.AvatarPathDataInfoList.AddRange(Roster.BuildPathData(world, player));

        foreach (var relic in InventoryHandlers.VisibleRelics(session))
        {
            sync.RelicList.Add(InventoryHandlers.ToProto(relic));
        }

        foreach (var lightcone in InventoryHandlers.VisibleLightcones(session))
        {
            sync.EquipmentList.Add(InventoryHandlers.ToProto(lightcone));
        }

        await session.SendAsync(sync);
        await session.SyncLineupAsync();

        if (!session.Challenge.Active)
        {
            await session.RefreshSceneActorsAsync();
        }
    }
}
