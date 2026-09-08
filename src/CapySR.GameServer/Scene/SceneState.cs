using CapySR.Protocol;

namespace CapySR.GameServer.Scene;

public enum SceneEntityKind
{
    Actor,
    Monster,
    Prop,
    Npc,
}

public sealed record SceneEntity(
    uint EntityId,
    SceneEntityKind Kind,
    uint GroupId,
    uint InstId,
    uint AvatarId,
    uint MonsterId,
    uint EventId,
    uint PropId,
    uint PropState,
    MotionInfo? Motion);

// what the client currently has loaded, by entity id, so a cast skill can be resolved to
// the monsters it hit and a won fight can take them off the map
public sealed class SceneState
{
    private readonly Dictionary<uint, SceneEntity> _entities = [];

    public SceneInfo? Info { get; private set; }

    public uint EntryId => Info?.EntryId ?? 0;

    public uint FloorId => Info?.FloorId ?? 0;

    public uint PlaneId => Info?.PlaneId ?? 0;

    public bool IsChallenge { get; private set; }

    public IReadOnlyDictionary<uint, SceneEntity> Entities => _entities;

    public IEnumerable<SceneEntity> Monsters => _entities.Values.Where(e => e.Kind == SceneEntityKind.Monster);

    public IEnumerable<SceneEntity> Actors => _entities.Values.Where(e => e.Kind == SceneEntityKind.Actor);

    public void Load(SceneInfo info, bool isChallenge)
    {
        Info = info;
        IsChallenge = isChallenge;
        _entities.Clear();

        foreach (var group in info.EntityGroupList)
        {
            foreach (var entity in group.EntityList)
            {
                Register(entity);
            }
        }
    }

    public void Clear()
    {
        Info = null;
        IsChallenge = false;
        _entities.Clear();
    }

    public SceneEntity? Get(uint entityId) => _entities.GetValueOrDefault(entityId);

    public bool IsActor(uint entityId) => Get(entityId)?.Kind == SceneEntityKind.Actor;

    public bool IsMonster(uint entityId) => Get(entityId)?.Kind == SceneEntityKind.Monster;

    public IEnumerable<SceneEntity> MonstersAmong(IEnumerable<uint> entityIds) =>
        entityIds.Distinct().Select(Get).OfType<SceneEntity>().Where(e => e.Kind == SceneEntityKind.Monster);

    public IEnumerable<SceneEntity> PropsAmong(IEnumerable<uint> entityIds) =>
        entityIds.Distinct().Select(Get).OfType<SceneEntity>().Where(e => e.Kind == SceneEntityKind.Prop);

    public bool Remove(uint entityId)
    {
        if (!_entities.Remove(entityId) || Info is null)
        {
            return false;
        }

        foreach (var group in Info.EntityGroupList)
        {
            var index = group.EntityList.ToList().FindIndex(e => e.EntityId == entityId);
            if (index >= 0)
            {
                group.EntityList.RemoveAt(index);
                break;
            }
        }

        return true;
    }

    // the party lives in group 0; swap it wholesale when the lineup changes
    public void ReplaceActors(IEnumerable<SceneEntityInfo> actors, uint leaderEntityId)
    {
        if (Info is null)
        {
            return;
        }

        foreach (var id in Actors.Select(a => a.EntityId).ToList())
        {
            _entities.Remove(id);
        }

        var group = Info.EntityGroupList.FirstOrDefault(g => g.GroupId == 0);
        if (group is null)
        {
            group = new SceneEntityGroupInfo { State = 1, GroupId = 0 };
            Info.EntityGroupList.Insert(0, group);
        }

        group.EntityList.Clear();

        foreach (var actor in actors)
        {
            group.EntityList.Add(actor);
            Register(actor);
        }

        Info.LeaderEntityId = leaderEntityId;
    }

    private void Register(SceneEntityInfo entity)
    {
        var record = entity.EntityCase switch
        {
            SceneEntityInfo.EntityOneofCase.Actor => new SceneEntity(
                entity.EntityId, SceneEntityKind.Actor, entity.GroupId, entity.InstId,
                entity.Actor.BaseAvatarId, 0, 0, 0, 0, entity.Motion),

            SceneEntityInfo.EntityOneofCase.NpcMonster => new SceneEntity(
                entity.EntityId, SceneEntityKind.Monster, entity.GroupId, entity.InstId,
                0, entity.NpcMonster.MonsterId, entity.NpcMonster.EventId, 0, 0, entity.Motion),

            SceneEntityInfo.EntityOneofCase.Prop => new SceneEntity(
                entity.EntityId, SceneEntityKind.Prop, entity.GroupId, entity.InstId,
                0, 0, 0, entity.Prop.PropId, entity.Prop.PropState, entity.Motion),

            SceneEntityInfo.EntityOneofCase.Npc => new SceneEntity(
                entity.EntityId, SceneEntityKind.Npc, entity.GroupId, entity.InstId,
                0, 0, 0, 0, 0, entity.Motion),

            _ => null,
        };

        if (record is not null)
        {
            _entities[record.EntityId] = record;
        }
    }
}
