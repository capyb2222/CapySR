using CapySR.Data;
using CapySR.Data.Excel;
using CapySR.GameServer.Game;
using CapySR.Protocol;

namespace CapySR.GameServer.Scene;

public sealed class SceneBuilder(GameData data)
{
    public const uint AvatarEntityBase = 100_000;
    public const uint MonsterEntityBase = 200_000;

    // the astral express is a special case: the client already has its props, and feeding
    // them back confuses it, so pearl-sr skips entity population for plane 10000 too
    private const uint TrainPlaneId = 10000;

    public static uint ActorEntityId(uint baseAvatarId) => baseAvatarId + AvatarEntityBase;

    // avatars land in group 0; the challenge encounter gets its own group so the client
    // can spawn and despawn it independently
    public SceneInfo BuildChallengeScene(ChallengeState challenge)
    {
        var scene = new SceneInfo
        {
            GameModeType = 1,
            PlaneId = challenge.PlaneId,
            FloorId = challenge.FloorId,
            EntryId = challenge.EntryId,
            WorldId = challenge.WorldId,
            ClientPosVersion = 1,
            SceneIdentifier = new SceneIdentifier { FloorId = challenge.FloorId },
        };

        var spawn = data.FindAnchor(challenge.EntryId);
        var motion = Motion(spawn?.Pos, spawn?.Rot);
        var avatars = new SceneEntityGroupInfo { State = 1, GroupId = 0 };

        foreach (var avatarId in challenge.Lineup)
        {
            avatars.EntityList.Add(BuildActor(avatarId, motion));
        }

        if (avatars.EntityList.Count > 0)
        {
            scene.LeaderEntityId = avatars.EntityList[0].EntityId;
        }

        scene.EntityGroupList.Add(avatars);

        var placement = data.FindMonsterPlacement(challenge.PlaneId, challenge.GroupId);

        var monsters = new SceneEntityGroupInfo { State = 1, GroupId = challenge.GroupId };
        monsters.EntityList.Add(new SceneEntityInfo
        {
            InstId = placement?.InstId ?? 1,
            GroupId = challenge.GroupId,
            EntityId = MonsterEntityBase + 1,
            NpcMonster = new SceneNpcMonsterInfo
            {
                MonsterId = challenge.MonsterId,
                EventId = challenge.EventId,
                WorldLevel = 6,
            },
            Motion = Motion(placement?.Pos, placement?.Rot),
        });

        scene.EntityGroupList.Add(monsters);

        return scene;
    }

    public SceneInfo BuildWorldScene(
        uint entryId, IReadOnlyList<uint> lineup, uint teleportId = 0, uint worldId = 501, PlayerPosition? spawn = null)
    {
        var resolved = data.ResolveEntrance(entryId);

        var planeId = resolved?.Entrance.PlaneID ?? 0;
        var floorId = resolved?.Entrance.FloorID ?? 0;

        var scene = new SceneInfo
        {
            GameModeType = 1,
            PlaneId = planeId,
            FloorId = floorId,
            EntryId = entryId,
            WorldId = worldId,
            ClientPosVersion = 1,
            SceneIdentifier = new SceneIdentifier { FloorId = floorId },
        };

        if (planeId != TrainPlaneId && data.FloorSavedValues.TryGetValue(floorId, out var saved))
        {
            foreach (var value in saved.SavedValues.Where(v => v.Name.Length > 0))
            {
                scene.FloorSavedData[value.Name] = value.MaxValue;
            }
        }

        var motion = SpawnMotion(entryId, teleportId, spawn);
        var avatars = new SceneEntityGroupInfo { State = 1, GroupId = 0 };

        foreach (var avatarId in lineup.Where(id => id != 0))
        {
            avatars.EntityList.Add(BuildActor(avatarId, motion));
        }

        if (avatars.EntityList.Count > 0)
        {
            scene.LeaderEntityId = avatars.EntityList[0].EntityId;
        }

        scene.EntityGroupList.Add(avatars);

        var resource = data.GetScene(entryId);

        if (planeId != TrainPlaneId && resource is not null)
        {
            AddEnvironment(scene, resource);
        }

        return scene;
    }

    // a teleport the player picked wins; otherwise where they last stood; otherwise the anchor
    public MotionInfo SpawnMotion(uint entryId, uint teleportId, PlayerPosition? spawn)
    {
        if (data.FindTeleport(entryId, teleportId) is { } teleport)
        {
            return Motion(teleport.Pos, teleport.Rot);
        }

        if (spawn is not null)
        {
            return spawn.ToMotion();
        }

        if (data.FindAnchor(entryId) is { } anchor)
        {
            return Motion(anchor.Pos, anchor.Rot);
        }

        var fallback = data.GetScene(entryId)?.Teleports.FirstOrDefault();
        return Motion(fallback?.Pos, fallback?.Rot);
    }

    public SceneEntityInfo BuildActor(uint avatarId, MotionInfo motion)
    {
        var baseId = data.BaseAvatarId(avatarId);

        return new SceneEntityInfo
        {
            InstId = 1,
            EntityId = ActorEntityId(baseId),
            Actor = new SceneActorInfo
            {
                BaseAvatarId = baseId,
                AvatarType = AvatarType.AvatarFormalType,
                Uid = 1,
                MapLayer = 0,
            },
            Motion = motion.Clone(),
        };
    }

    private static void AddEnvironment(SceneInfo scene, SceneResourceExcel resource)
    {
        var groups = new Dictionary<uint, SceneEntityGroupInfo>();
        var nextEntityId = MonsterEntityBase;

        SceneEntityGroupInfo Group(uint groupId)
        {
            if (!groups.TryGetValue(groupId, out var group))
            {
                groups[groupId] = group = new SceneEntityGroupInfo { State = 1, GroupId = groupId };
            }

            return group;
        }

        foreach (var prop in resource.Props)
        {
            Group(prop.GroupId).EntityList.Add(new SceneEntityInfo
            {
                InstId = prop.InstId,
                GroupId = prop.GroupId,
                EntityId = ++nextEntityId,
                Prop = new ScenePropInfo { PropId = prop.PropId, PropState = prop.PropState },
                Motion = Motion(prop.Pos, prop.Rot),
            });
        }

        foreach (var monster in resource.Monsters)
        {
            Group(monster.GroupId).EntityList.Add(new SceneEntityInfo
            {
                InstId = monster.InstId,
                GroupId = monster.GroupId,
                EntityId = ++nextEntityId,
                NpcMonster = new SceneNpcMonsterInfo
                {
                    MonsterId = monster.MonsterId,
                    EventId = monster.EventId,
                    WorldLevel = 6,
                },
                Motion = Motion(monster.Pos, monster.Rot),
            });
        }

        foreach (var group in groups.Values)
        {
            scene.EntityGroupList.Add(group);
        }
    }

    public MotionInfo? AnchorMotion(uint entryId)
    {
        var anchor = data.FindAnchor(entryId);
        return anchor is null ? null : Motion(anchor.Pos, anchor.Rot);
    }

    public static MotionInfo Motion(ResVector? pos, ResVector? rot) => new()
    {
        Pos = Vector(pos),
        Rot = Vector(rot),
    };

    private static Protocol.Vector Vector(ResVector? v) =>
        v is null ? new Protocol.Vector() : new Protocol.Vector { X = v.X, Y = v.Y, Z = v.Z };
}
