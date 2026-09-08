using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.GameServer.Scene;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

[Handlers]
public static class SceneHandlers
{
    public static Task OnGetCurSceneInfo(PlayerSession session, GetCurSceneInfoCsReq request)
    {
        var scene = session.CurrentScene();

        session.Logger.LogInformation(
            "scene {Entry}: plane {Plane}, floor {Floor}, {Groups} groups, {Entities} entities",
            scene.EntryId, scene.PlaneId, scene.FloorId, scene.EntityGroupList.Count,
            scene.EntityGroupList.Sum(g => g.EntityList.Count));

        return session.SendAsync(new GetCurSceneInfoScRsp { Retcode = 0, Scene = scene });
    }

    public static async Task OnEnterScene(PlayerSession session, EnterSceneCsReq request)
    {
        var world = session.World;
        var entryId = request.EntryId != 0 ? request.EntryId : session.Player.EntryId;
        var resolved = world.Data.ResolveEntrance(entryId);
        var floorId = resolved?.Entrance.FloorID ?? request.SceneIdentifier?.FloorId ?? 0;

        // echo whatever the client sent back at it; dropping content or storyline ids here
        // leaves it waiting on a scene it cannot match to the request
        var identifier = request.SceneIdentifier?.Clone() ?? new SceneIdentifier();

        if (floorId != 0)
        {
            identifier.FloorId = floorId;
        }

        session.Logger.LogInformation(
            "enter scene: entry {Entry} (from {Previous}), teleport {Teleport}, plane {Plane}, floor {Floor}",
            entryId, session.Player.EntryId, request.TeleportId, resolved?.Entrance.PlaneID ?? 0, floorId);

        await session.SendAsync(new EnterSceneScRsp
        {
            Retcode = 0,
            SceneIdentifier = identifier,
            IsCloseMap = request.IsCloseMap,
            IsOverMap = true,
        });

        await session.EnterWorldSceneAsync(entryId, request.TeleportId);
    }

    public static Task OnGetSceneMapInfo(PlayerSession session, GetSceneMapInfoCsReq request)
    {
        var response = new GetSceneMapInfoScRsp { Retcode = 0 };
        var data = session.World.Data;

        foreach (var identifier in request.SceneIdentifiers)
        {
            var info = new SceneMapInfo
            {
                Retcode = 0,
                FloorId = identifier.FloorId,
                EntryId = session.Player.EntryId,
                CurMapEntryId = session.Player.EntryId,
                SceneIdentifier = identifier,
            };

            // everything lit and every teleport open
            if (data.FloorSavedValues.TryGetValue(identifier.FloorId, out var saved))
            {
                foreach (var value in saved.SavedValues.Where(v => v.Name.Length > 0))
                {
                    info.FloorSavedValueMap[value.Name] = value.MaxValue;
                }
            }

            info.UnlockTeleportList.AddRange(data.TeleportIdsForFloor(identifier.FloorId));
            info.ClientGroupMissionInfo = new MissionStatusBySceneInfo();
            info.LightenSectionList.AddRange(LightenSections());

            foreach (var scene in data.Scenes.Values.Where(s => s.PlaneID == identifier.FloorId / 1000))
            {
                foreach (var prop in scene.Props)
                {
                    info.MazePropList.Add(new MazePropState
                    {
                        GroupId = prop.GroupId,
                        ConfigId = prop.InstId,
                        State = prop.PropState,
                    });
                }

                foreach (var groupId in scene.Props.Select(p => p.GroupId).Distinct())
                {
                    info.MazeGroupList.Add(new MazeGroup { GroupId = groupId });
                }
            }

            response.SceneMapInfo.Add(info);
        }

        return session.SendAsync(response);
    }

    // section ids are small per floor; lighting more than exists is harmless
    private static IEnumerable<uint> LightenSections()
    {
        for (var i = 0u; i <= 100; i++)
        {
            yield return i;
        }

        for (var i = 10000u; i <= 10050; i++)
        {
            yield return i;
        }

        yield return 20000;

        for (var i = 30000u; i <= 30019; i++)
        {
            yield return i;
        }
    }

    public static Task OnSceneEntityMove(PlayerSession session, SceneEntityMoveCsReq request)
    {
        // challenge maps are not where the player lives; only overworld steps are remembered
        if (!session.Scene.IsChallenge)
        {
            foreach (var motion in request.EntityMotionList)
            {
                if (motion.Motion is null || !IsParty(session, motion.EntityId))
                {
                    continue;
                }

                session.Player.Position = PlayerPosition.From(motion.Motion);
                session.Player.Touch();
                break;
            }
        }

        var response = new SceneEntityMoveScRsp { Retcode = 0 };
        response.EntityMotionList.AddRange(request.EntityMotionList);

        return session.SendAsync(response);
    }

    // the client reports the party as entity 0 in some builds and as the leader in others
    private static bool IsParty(PlayerSession session, uint entityId) =>
        entityId == 0 || session.Scene.IsActor(entityId);

    public static async Task OnSceneEntityTeleport(PlayerSession session, SceneEntityTeleportCsReq request)
    {
        if (request.EntityMotion?.Motion is { } motion && !session.Scene.IsChallenge)
        {
            session.Player.Position = PlayerPosition.From(motion);
            session.Player.Touch();
        }

        await session.SendAsync(new SceneEntityTeleportScRsp
        {
            Retcode = 0,
            EntityMotion = request.EntityMotion,
            ClientPosVersion = 1,
        });
    }

    public static Task OnGetEnteredScene(PlayerSession session, GetEnteredSceneCsReq request)
    {
        var response = new GetEnteredSceneScRsp { Retcode = 0 };

        foreach (var entrance in session.World.Data.MapEntrances.Values)
        {
            response.EnteredSceneInfoList.Add(new EnteredSceneInfo
            {
                PlaneId = entrance.PlaneID,
                FloorId = entrance.FloorID,
            });
        }

        return session.SendAsync(response);
    }

    // the client asks per entry; answering with every teleport in the game makes it filter
    // points that belong to maps it is not looking at
    public static Task OnGetUnlockTeleport(PlayerSession session, GetUnlockTeleportCsReq request)
    {
        var data = session.World.Data;
        var response = new GetUnlockTeleportScRsp { Retcode = 0 };

        var teleports = request.EntryIdList.Count > 0
            ? request.EntryIdList.SelectMany(data.TeleportIdsForEntry)
            : data.Scenes.Values.SelectMany(s => s.Teleports).Select(t => t.TeleportId);

        response.UnlockedTeleportList.AddRange(teleports.Distinct());
        return session.SendAsync(response);
    }

    public static Task OnEnterSection(PlayerSession session, EnterSectionCsReq request) =>
        session.SendAsync(new EnterSectionScRsp { Retcode = 0 });

    // space anchors bring the team back to full and refill technique points
    public static async Task OnInteractProp(PlayerSession session, InteractPropCsReq request)
    {
        var prop = session.Scene.Get(request.PropEntityId);
        var excel = prop is null ? null : session.World.Data.Props.GetValueOrDefault(prop.PropId);

        await session.SendAsync(new InteractPropScRsp
        {
            Retcode = 0,
            PropEntityId = request.PropEntityId,
            PropState = prop?.PropState ?? 0,
        });

        if (excel is { IsSpring: true })
        {
            session.Player.HealAll(session.World.Player);
            session.Player.Lineups.RefillMp();
            session.Player.Touch();
            await session.SyncLineupAsync(SyncLineupReason.SyncReasonHpAdd);
        }
    }
}
