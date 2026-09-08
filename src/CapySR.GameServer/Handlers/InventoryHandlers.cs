using CapySR.Data.SrTools;
using CapySR.GameServer.Game;
using CapySR.GameServer.Net;
using CapySR.Protocol;
using Microsoft.Extensions.Logging;

namespace CapySR.GameServer.Handlers;

// gear lives in the srtools file. edits made in-game are applied to the in-memory copy so the
// character screen behaves, but they are not written back - srtools owns that file.
[Handlers]
public static class InventoryHandlers
{
    public static Task OnGetBag(PlayerSession session, GetBagCsReq request)
    {
        var response = new GetBagScRsp { Retcode = 0 };

        // the client needs at least some materials or its item map never initialises and
        // GetItemsInTypes throws every frame, freezing the character screen
        foreach (var (tid, count) in Materials)
        {
            response.MaterialList.Add(new Material { Tid = tid, Num = count });
        }

        foreach (var relic in VisibleRelics(session))
        {
            response.RelicList.Add(ToProto(relic));
        }

        foreach (var lightcone in VisibleLightcones(session))
        {
            response.EquipmentList.Add(ToProto(lightcone));
        }

        session.Logger.LogInformation(
            "bag: {Relics} relics, {Lightcones} lightcones, {Materials} materials (mode={Mode})",
            response.RelicList.Count, response.EquipmentList.Count, response.MaterialList.Count,
            session.Config.GameServer.InventoryMode);

        return session.SendAsync(response);
    }

    // all = everything, equipped = only what is worn, none = nothing
    public static IEnumerable<SrRelic> VisibleRelics(PlayerSession session)
    {
        var mode = session.Config.GameServer.InventoryMode.Trim().ToLowerInvariant();

        if (mode == "none")
        {
            return [];
        }

        var equippedOnly = mode != "all";

        return session.World.Player.Relics.Where(r =>
            r.RelicId != 0 && r.UniqueId != 0 && (!equippedOnly || r.EquipAvatar != 0));
    }

    public static IEnumerable<SrLightcone> VisibleLightcones(PlayerSession session)
    {
        var mode = session.Config.GameServer.InventoryMode.Trim().ToLowerInvariant();

        if (mode == "none")
        {
            return [];
        }

        var equippedOnly = mode != "all";

        return session.World.Player.Lightcones.Where(l =>
            l.ItemId != 0 && l.UniqueId != 0 && (!equippedOnly || l.EquipAvatar != 0));
    }

    public static async Task OnDressAvatar(PlayerSession session, DressAvatarCsReq request)
    {
        var player = session.World.Player;
        var lightcone = player.Lightcones.FirstOrDefault(l => l.UniqueId == request.EquipmentUniqueId);

        if (lightcone is not null)
        {
            // a lightcone can only be worn once
            foreach (var worn in player.Lightcones.Where(l => l.EquipAvatar == request.AvatarId))
            {
                worn.EquipAvatar = 0;
            }

            lightcone.EquipAvatar = request.AvatarId;
        }

        await session.SendAsync(new DressAvatarScRsp());
        await session.SyncGearAsync(request.AvatarId);
    }

    public static async Task OnTakeOffEquipment(PlayerSession session, TakeOffEquipmentCsReq request)
    {
        foreach (var lightcone in session.World.Player.Lightcones.Where(l => l.EquipAvatar == request.AvatarId))
        {
            lightcone.EquipAvatar = 0;
        }

        await session.SendAsync(new TakeOffEquipmentScRsp());
        await session.SyncGearAsync(request.AvatarId);
    }

    public static async Task OnDressRelicAvatar(PlayerSession session, DressRelicAvatarCsReq request)
    {
        var player = session.World.Player;

        foreach (var change in request.SwitchList)
        {
            var relic = player.Relics.FirstOrDefault(r => r.UniqueId == change.RelicUniqueId);
            if (relic is null)
            {
                continue;
            }

            // clear whatever was in that slot on this avatar
            foreach (var worn in player.Relics.Where(r => r.EquipAvatar == request.AvatarId && r.Slot == relic.Slot))
            {
                worn.EquipAvatar = 0;
            }

            relic.EquipAvatar = request.AvatarId;
        }

        await session.SendAsync(new DressRelicAvatarScRsp { Retcode = 0, AvatarId = request.AvatarId });
        await session.SyncGearAsync(request.AvatarId);
    }

    public static async Task OnTakeOffRelic(PlayerSession session, TakeOffRelicCsReq request)
    {
        var player = session.World.Player;

        foreach (var slot in request.RelicTypeList)
        {
            foreach (var relic in player.Relics.Where(r => r.EquipAvatar == request.AvatarId && r.Slot == slot))
            {
                relic.EquipAvatar = 0;
            }
        }

        await session.SendAsync(new TakeOffRelicScRsp());
        await session.SyncGearAsync(request.AvatarId);
    }

    public static async Task OnRankUpAvatar(PlayerSession session, RankUpAvatarCsReq request)
    {
        if (session.World.Player.Avatars.TryGetValue(request.AvatarId, out var avatar))
        {
            avatar.Data.Rank = request.Rank;
        }

        await session.SendAsync(new RankUpAvatarScRsp());
        await session.SyncGearAsync(request.AvatarId);
    }

    // currencies, then the player outfits the wardrobe expects
    private static readonly (uint Tid, uint Count)[] Materials =
    [
        (101, 999_999), (238, 999_999), (239, 999_999),
        (251001, 100), (251002, 100), (251003, 100), (251004, 100),
        (227001, 1), (227002, 1), (227003, 1), (227004, 1), (227005, 1), (227006, 1),
        (227007, 1), (227008, 1), (227009, 1), (227010, 1), (227012, 1), (227013, 1),
        (227015, 1), (227016, 1), (229001, 1),
    ];

    public static Relic ToProto(SrRelic relic)
    {
        var proto = new Relic
        {
            UniqueId = relic.UniqueId,
            Tid = relic.RelicId,
            Level = relic.Level,
            MainAffixId = relic.MainAffixId,
            DressAvatarId = relic.EquipAvatar,
            Exp = 0,
            IsProtected = false,
        };

        foreach (var affix in relic.SubAffixes)
        {
            proto.SubAffixList.Add(new RelicAffix
            {
                AffixId = affix.SubAffixId,
                Cnt = affix.Count,
                Step = affix.Step,
            });
        }

        return proto;
    }

    public static Equipment ToProto(SrLightcone lightcone) => new()
    {
        UniqueId = lightcone.UniqueId,
        Tid = lightcone.ItemId,
        Level = lightcone.Level,
        Promotion = lightcone.Promotion,
        Rank = lightcone.Rank,
        DressAvatarId = lightcone.EquipAvatar,
        Exp = 0,
        IsProtected = false,
    };

    // push the avatar's new gear so the character screen updates without a relog
    public static Task SyncGearAsync(this PlayerSession session, uint avatarId)
    {
        var world = session.World;

        var sync = new PlayerSyncScNotify
        {
            AvatarSync = new AvatarSync(),
        };

        foreach (var relic in world.Player.Relics.Where(r => r.EquipAvatar == avatarId || r.EquipAvatar == 0))
        {
            sync.RelicList.Add(ToProto(relic));
        }

        foreach (var lightcone in world.Player.Lightcones.Where(l => l.EquipAvatar == avatarId || l.EquipAvatar == 0))
        {
            sync.EquipmentList.Add(ToProto(lightcone));
        }

        sync.AvatarSync.AvatarList.AddRange(Roster.BuildAvatars(world, session.Player));
        sync.AvatarSync.AvatarPathDataInfoList.AddRange(Roster.BuildPathData(world, session.Player));

        return session.SendAsync(sync);
    }
}
