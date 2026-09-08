using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.GameServer.Scene;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

// The client redraws the party bar from SyncLineupNotify and the overworld models from the
// scene, and it takes both as read only until the request it sent is answered. So every edit
// pushes the scene and the lineup first and closes with the response, or the new team does
// not show until something else nudges the UI.
[Handlers]
public static class LineupHandlers
{
    public static Task OnGetAllLineupData(PlayerSession session, GetAllLineupDataCsReq request)
    {
        var book = session.Player.Lineups;
        var response = new GetAllLineupDataScRsp { Retcode = 0, CurIndex = book.CurrentIndex };

        for (var i = 0; i < book.Squads.Count; i++)
        {
            response.LineupList.Add(session.BuildLineupInfo(book.Squads[i], ExtraLineupType.LineupNone, (uint)i));
        }

        return session.SendAsync(response);
    }

    public static Task OnGetCurLineupData(PlayerSession session, GetCurLineupDataCsReq request) =>
        session.SendAsync(new GetCurLineupDataScRsp { Retcode = 0, Lineup = session.BuildWorldLineup() });

    public static async Task OnReplaceLineup(PlayerSession session, ReplaceLineupCsReq request)
    {
        var book = session.Player.Lineups;
        var entries = request.LineupSlotList
            .Where(s => session.OwnsBase(s.Id))
            .Select(s => (s.Slot, s.Id))
            .ToList();

        Lineup target;
        bool isCurrent;

        if (request.ExtraLineupType != ExtraLineupType.LineupNone)
        {
            target = book.SetExtra(request.ExtraLineupType, entries.Select(e => e.Id));
            isCurrent = true;
        }
        else
        {
            target = book.Squad(request.Index);
            target.Replace(entries, request.LeaderSlot);
            isCurrent = !book.InExtraLineup && request.Index == book.CurrentIndex;
        }

        // never leave the walking team empty; put the roster's first back
        if (target.IsEmpty && ReferenceEquals(target, book.Current))
        {
            target.Fill(session.DefaultLineup());
        }

        session.Player.Touch();
        session.LogTeam($"replace #{request.Index}", target);

        await session.PushTeamAsync(isCurrent);
        await session.SendAsync(new ReplaceLineupScRsp { Retcode = 0 });
    }

    public static async Task OnJoinLineup(PlayerSession session, JoinLineupCsReq request)
    {
        var target = session.Target(request.ExtraLineupType, request.Index);
        var avatarId = request.OPCJCDKMJEI;

        var retcode = !session.OwnsBase(avatarId)
            ? Retcode.RetLineupAvatarNotExist
            : target.Join(avatarId, request.Slot);

        session.LogTeam($"join {avatarId} -> slot {request.Slot} ({retcode})", target);

        if (retcode == Retcode.RetSucc)
        {
            session.Player.Touch();
            await session.PushTeamAsync(session.IsCurrent(request.ExtraLineupType, request.Index));
        }

        await session.SendAsync(new JoinLineupScRsp { Retcode = (uint)retcode });
    }

    public static async Task OnQuitLineup(PlayerSession session, QuitLineupCsReq request)
    {
        var target = session.Target(request.ExtraLineupType, request.Index);
        var retcode = target.Quit(request.BaseAvatarId);

        session.LogTeam($"quit {request.BaseAvatarId} ({retcode})", target);

        if (retcode == Retcode.RetSucc)
        {
            session.Player.Touch();
            await session.PushTeamAsync(session.IsCurrent(request.ExtraLineupType, request.Index));
        }

        await session.SendAsync(new QuitLineupScRsp { Retcode = (uint)retcode });
    }

    public static async Task OnSwapLineup(PlayerSession session, SwapLineupCsReq request)
    {
        var target = session.Target(request.ExtraLineupType, request.Index);
        var retcode = target.Swap(request.MPIEGFCNBEN, request.ENBIBCPPLDC);

        session.LogTeam($"swap {request.MPIEGFCNBEN} <-> {request.ENBIBCPPLDC} ({retcode})", target);

        if (retcode == Retcode.RetSucc)
        {
            session.Player.Touch();
            await session.PushTeamAsync(session.IsCurrent(request.ExtraLineupType, request.Index));
        }

        await session.SendAsync(new SwapLineupScRsp { Retcode = (uint)retcode });
    }

    public static async Task OnChangeLineupLeader(PlayerSession session, ChangeLineupLeaderCsReq request)
    {
        var lineup = session.Player.Lineups.Current;
        var retcode = lineup.SetLeader(request.Slot);

        session.LogTeam($"leader -> slot {request.Slot} ({retcode})", lineup);

        if (retcode == Retcode.RetSucc)
        {
            session.Player.Touch();

            if (session.Scene.Info is { } scene)
            {
                scene.LeaderEntityId = SceneBuilder.ActorEntityId(session.World.Data.BaseAvatarId(lineup.LeaderAvatarId));
            }

            await session.SyncLineupAsync();
        }

        await session.SendAsync(new ChangeLineupLeaderScRsp
        {
            Retcode = (uint)retcode,
            Slot = request.Slot,
        });
    }

    public static async Task OnSwitchLineupIndex(PlayerSession session, SwitchLineupIndexCsReq request)
    {
        var book = session.Player.Lineups;
        var retcode = book.InExtraLineup ? Retcode.RetLineupInvalidIndex : book.SetCurrent(request.Index);

        session.LogTeam($"switch -> #{request.Index} ({retcode})", book.Current);

        if (retcode == Retcode.RetSucc)
        {
            session.Player.Touch();
            await session.PushTeamAsync(isCurrent: true);
        }

        await session.SendAsync(new SwitchLineupIndexScRsp
        {
            Retcode = (uint)retcode,
            Index = request.Index,
        });
    }

    public static Task OnSetLineupName(PlayerSession session, SetLineupNameCsReq request)
    {
        var book = session.Player.Lineups;

        if (request.Index >= LineupBook.SquadCount)
        {
            return session.SendAsync(new SetLineupNameScRsp { Retcode = (uint)Retcode.RetLineupInvalidIndex });
        }

        book.Squad(request.Index).Name = request.Name;
        session.Player.Touch();

        return session.SendAsync(new SetLineupNameScRsp
        {
            Retcode = 0,
            Index = request.Index,
            Name = request.Name,
        });
    }

    // every owned character with its current health, for the team picker
    public static Task OnGetLineupAvatarData(PlayerSession session, GetLineupAvatarDataCsReq request)
    {
        var world = session.World;
        var response = new GetLineupAvatarDataScRsp { Retcode = 0 };

        foreach (var baseId in world.OwnedAvatarIds.Select(world.Data.BaseAvatarId).Distinct())
        {
            var path = Roster.ResolvePath(world, session.Player, baseId);

            response.AvatarDataList.Add(new LineupAvatarData
            {
                Id = baseId,
                AvatarType = AvatarType.AvatarFormalType,
                Hp = session.Player.StateOf(path, world.Player).Hp,
            });
        }

        return session.SendAsync(response);
    }

    private static async Task PushTeamAsync(this PlayerSession session, bool isCurrent)
    {
        if (isCurrent)
        {
            await session.RefreshSceneActorsAsync();
        }

        await session.SyncLineupAsync();
    }

    private static Lineup Target(this PlayerSession session, ExtraLineupType type, uint index)
    {
        var book = session.Player.Lineups;

        return type != ExtraLineupType.LineupNone
            ? book.Extra(type) ?? book.SetExtra(type, [], activate: false)
            : book.Squad(index);
    }

    private static bool IsCurrent(this PlayerSession session, ExtraLineupType type, uint index)
    {
        var book = session.Player.Lineups;

        return type != ExtraLineupType.LineupNone
            ? book.ExtraType == type
            : !book.InExtraLineup && index == book.CurrentIndex;
    }

    // a multipath character is addressed by its base id; owning any path counts
    public static bool OwnsBase(this PlayerSession session, uint avatarId)
    {
        if (avatarId == 0)
        {
            return false;
        }

        var world = session.World;
        return world.OwnedAvatarIds.Any(id => id == avatarId || world.Data.BaseAvatarId(id) == avatarId);
    }

    private static void LogTeam(this PlayerSession session, string what, Lineup lineup)
    {
        session.Logger.LogInformation("lineup {What}: [{Team}] leader {Leader}",
            what, string.Join(", ", lineup.Slots.Select(id => id == 0 ? "-" : id.ToString())), lineup.LeaderAvatarId);
    }
}
