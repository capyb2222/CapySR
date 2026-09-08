using CapySR.Data;
using CapySR.Data.SrTools;
using CapySR.Protocol;

namespace CapySR.GameServer.Game;

// what the account owns. srtools' roster when one has been uploaded, otherwise everything
// playable so the game is usable before the tool has ever been pointed at us.
public static class Roster
{
    public static bool IsPlayable(uint avatarId) => avatarId is (> 1000 and < 2000) or (>= 8001 and <= 8010);

    public static IReadOnlyList<uint> OwnedIds(GameWorld world) => world.OwnedAvatarIds;

    public static bool IsOwned(GameWorld world, uint avatarId) => world.OwnedAvatarIds.Contains(avatarId);

    // resolves a base id (8001, 1001) to the path the player currently walks
    public static uint ResolvePath(GameWorld world, Player player, uint avatarId)
    {
        var baseId = world.Data.BaseAvatarId(avatarId);

        if (baseId == avatarId && world.Data.MultiPathAvatarIds.Contains(avatarId))
        {
            var variants = world.OwnedAvatarIds.Where(id => world.Data.BaseAvatarId(id) == baseId).ToList();
            return variants.Count > 0 ? SelectedPath(baseId, variants, player) : avatarId;
        }

        return avatarId;
    }

    public static List<Avatar> BuildAvatars(GameWorld world, Player player)
    {
        var owned = OwnedIds(world);
        var avatars = new List<Avatar>();

        // the client wants one Avatar per *base* id; paths ride along in AvatarPathData
        foreach (var group in owned.GroupBy(world.Data.BaseAvatarId).OrderBy(g => g.Key))
        {
            // gear hangs off the selected path (8002), not the base id (8001)
            var path = SelectedPath(group.Key, group, player);
            var configured = world.Player.Avatars.GetValueOrDefault(path)
                             ?? world.Player.Avatars.GetValueOrDefault(group.First());

            avatars.Add(new Avatar
            {
                BaseAvatarId = group.Key,
                Level = configured?.Level ?? 80,
                Promotion = configured?.Promotion ?? 6,
                Exp = 0,
                FirstMetTimeStamp = 1712924677,
                CurMultiPathAvatarType = path,
                EquipmentUniqueId = world.Player.LightconeFor(path)?.UniqueId ?? 0,
                HasTakenPromotionRewardList = { Enumerable.Range(0, (int)(configured?.Promotion ?? 6)).Select(i => (uint)i) },
            });
        }

        return avatars;
    }

    public static List<AvatarPathData> BuildPathData(GameWorld world, Player player)
    {
        var paths = new List<AvatarPathData>();

        foreach (var avatarId in OwnedIds(world))
        {
            var configured = world.Player.Avatars.GetValueOrDefault(avatarId);

            var path = new AvatarPathData
            {
                AvatarId = avatarId,
                Rank = configured?.Data.Rank ?? 6,
                PathEquipmentId = world.Player.LightconeFor(avatarId)?.UniqueId ?? 0,
                UnkEnhancedId = player.IsEnhanced(avatarId, world.Player) ? EnhancedId(world.Data, avatarId) : 0,
            };

            // point_id here is the anchor index; srtools stores exactly that
            var tree = configured is { Data.SkillsByAnchorType.Count: > 0 }
                ? configured.Data.SkillsByAnchorType.Select(kv => (kv.Key, kv.Value))
                : world.Data.MaxedPathSkillTree(avatarId);

            foreach (var (anchor, level) in tree)
            {
                path.AvatarPathSkillTree.Add(new AvatarPathSkillTree { PointId = anchor, Level = level });
            }

            foreach (var relic in world.Player.RelicsFor(avatarId))
            {
                path.EquipRelicList.Add(new EquipRelic { Type = relic.Slot, RelicUniqueId = relic.UniqueId });
            }

            paths.Add(path);
        }

        return paths;
    }

    public static uint EnhancedId(GameData data, uint avatarId) =>
        data.EnhancedAvatars.TryGetValue(avatarId, out var id) ? id : 1;

    public static uint SelectedPath(uint baseId, IEnumerable<uint> variants, Player player)
    {
        var list = variants.ToList();

        return baseId switch
        {
            8001 => list.Contains(player.TrailblazerPath) ? player.TrailblazerPath : list.Max(),
            1001 => list.Contains(player.MarchPath) ? player.MarchPath : list.Min(),
            _ => baseId,
        };
    }
}
