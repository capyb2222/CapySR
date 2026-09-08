using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

[Handlers]
public static class AvatarHandlers
{
    public static Task OnGetAvatarData(PlayerSession session, GetAvatarDataCsReq request)
    {
        var world = session.World;

        var response = new GetAvatarDataScRsp
        {
            Retcode = 0,
            IsGetAll = request.IsGetAll,
        };

        response.AvatarList.AddRange(Roster.BuildAvatars(world, session.Player));

        var paths = Roster.BuildPathData(world, session.Player);
        response.AvatarPathDataInfoList.AddRange(paths);

        // the client needs to know which of those ids are multipath avatars
        response.BasicTypeIdList.AddRange(
            paths.Select(p => p.AvatarId).Where(world.Data.MultiPathAvatarIds.Contains).Distinct());

        session.Logger.LogInformation(
            "roster: {Avatars} avatars, {Paths} paths ({Source})",
            response.AvatarList.Count, response.AvatarPathDataInfoList.Count,
            world.HasSrToolsRoster ? "srtools" : "all playable");

        return session.SendAsync(response);
    }

    public static async Task OnSetAvatarPath(PlayerSession session, SetAvatarPathCsReq request)
    {
        var avatarId = (uint)request.AvatarId;
        var baseId = session.World.Data.BaseAvatarId(avatarId);

        if (!Roster.IsOwned(session.World, avatarId))
        {
            await session.SendAsync(new SetAvatarPathScRsp { Retcode = (uint)Retcode.RetAvatarNotExist, AvatarId = request.AvatarId });
            return;
        }

        if (baseId == 8001)
        {
            session.Player.TrailblazerPath = avatarId;
            session.Player.Gender = avatarId % 2 == 1 ? Gender.Man : Gender.Woman;
        }
        else
        {
            session.Player.MarchPath = avatarId;
        }

        session.Player.Touch();
        session.Logger.LogInformation("path {Base} -> {Path}", baseId, avatarId);

        await session.SendAsync(new AvatarPathChangedNotify
        {
            BaseAvatarId = baseId,
            CurMultiPathAvatarType = request.AvatarId,
        });

        await session.SyncAvatarsAsync();
        await session.SyncLineupAsync();
        await session.SendAsync(new SetAvatarPathScRsp { Retcode = 0, AvatarId = request.AvatarId });
    }

    // the enhanced kit is a per-avatar toggle on the character screen
    public static async Task OnSetAvatarEnhancedId(PlayerSession session, SetAvatarEnhancedIdCsReq request)
    {
        var player = session.Player;

        if (request.EnhancedId == 0)
        {
            player.EnhancedAvatars.Remove(request.AvatarId);
        }
        else if (session.World.Data.EnhancedAvatars.ContainsKey(request.AvatarId))
        {
            player.EnhancedAvatars.Add(request.AvatarId);
        }

        player.Touch();
        await session.SyncAvatarsAsync();

        await session.SendAsync(new SetAvatarEnhancedIdScRsp
        {
            Retcode = 0,
            GrowthAvatarId = request.AvatarId,
            UnkEnhancedId = request.EnhancedId,
        });
    }

    public static async Task OnSetGender(PlayerSession session, SetGenderCsReq request)
    {
        var player = session.Player;
        var world = session.World;

        if (request.Gender is Gender.Man or Gender.Woman)
        {
            player.Gender = request.Gender;

            // keep the trailblazer on a path of the chosen body
            var wantOdd = request.Gender == Gender.Man;
            var variants = world.OwnedAvatarIds.Where(id => id is >= 8001 and <= 8010 && (id % 2 == 1) == wantOdd).ToList();

            if (variants.Count > 0 && !variants.Contains(player.TrailblazerPath))
            {
                player.TrailblazerPath = variants.Max();
            }

            player.Touch();
            await session.SyncAvatarsAsync();
            await session.SyncLineupAsync();
        }

        await session.SendAsync(new SetGenderScRsp
        {
            Retcode = 0,
            CurAvatarPath = (MultiPathAvatarType)player.TrailblazerPath,
        });
    }
}
