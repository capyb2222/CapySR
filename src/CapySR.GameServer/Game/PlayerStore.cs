using System.Text.Json;
using System.Text.Json.Serialization;
using CapySR.Protocol;

namespace CapySR.GameServer.Game;

// data/player.json. small, human readable, written whole; srtools keeps the roster elsewhere.
public static class PlayerStore
{
    public const string FileName = "player.json";

    private static readonly JsonSerializerOptions Options = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        PropertyNameCaseInsensitive = true,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static bool TryLoad(string path, Player player)
    {
        if (!File.Exists(path))
        {
            return false;
        }

        PlayerSave? save;

        try
        {
            save = JsonSerializer.Deserialize<PlayerSave>(File.ReadAllText(path), Options);
        }
        catch (Exception e) when (e is IOException or JsonException)
        {
            return false;
        }

        if (save is null)
        {
            return false;
        }

        Apply(save, player);
        return true;
    }

    public static void Save(string path, Player player)
    {
        var save = Capture(player);
        var json = JsonSerializer.Serialize(save, Options);

        Directory.CreateDirectory(Path.GetDirectoryName(path)!);

        // write-then-move so a crash mid-write never leaves a torn file
        var temp = path + ".tmp";
        File.WriteAllText(temp, json);
        File.Move(temp, path, overwrite: true);
    }

    private static PlayerSave Capture(Player player)
    {
        var save = new PlayerSave
        {
            Nickname = player.Nickname,
            Signature = player.Signature,
            WorldLevel = player.WorldLevel,
            HeadIcon = player.HeadIcon,
            Gender = (int)player.Gender,
            EntryId = player.EntryId,
            TeleportId = player.TeleportId,
            Position = player.Position,
            TrailblazerPath = player.TrailblazerPath,
            MarchPath = player.MarchPath,
            CurrentLineup = player.Lineups.CurrentIndex,
            Mp = player.Lineups.Mp,
            EnhancedAvatars = [.. player.EnhancedAvatars.Order()],
            FinishedColleges = [.. player.FinishedColleges.Order()],
            DisplayAvatars = [.. player.DisplayAvatars],
        };

        foreach (var (id, state) in player.AvatarStates)
        {
            save.AvatarStates[id.ToString()] = new SavedAvatarState { Hp = state.Hp, SpCur = state.SpCur, SpMax = state.SpMax };
        }

        foreach (var squad in player.Lineups.Squads)
        {
            save.Lineups.Add(new SavedLineup
            {
                Name = squad.Name,
                Slots = [.. squad.Slots],
                Leader = squad.LeaderAvatarId,
            });
        }

        foreach (var (id, record) in player.ChallengeRecords)
        {
            save.ChallengeRecords[id.ToString()] = record;
        }

        foreach (var (key, record) in player.PeakRecords)
        {
            save.PeakRecords[key.ToString()] = record;
        }

        foreach (var (peakId, avatars) in player.PeakLineups)
        {
            save.PeakLineups[peakId.ToString()] = avatars;
        }

        foreach (var (id, data) in player.ServerPrefs)
        {
            save.ServerPrefs[id.ToString()] = Convert.ToBase64String(data);
        }

        return save;
    }

    private static void Apply(PlayerSave save, Player player)
    {
        if (!string.IsNullOrWhiteSpace(save.Nickname))
        {
            player.Nickname = save.Nickname;
        }

        player.Signature = save.Signature ?? player.Signature;
        player.WorldLevel = save.WorldLevel is { } worldLevel and <= 6 ? worldLevel : player.WorldLevel;
        player.HeadIcon = save.HeadIcon == 0 ? player.HeadIcon : save.HeadIcon;
        player.Gender = save.Gender is 1 or 2 ? (Gender)save.Gender : player.Gender;
        player.EntryId = save.EntryId == 0 ? player.EntryId : save.EntryId;
        player.TeleportId = save.TeleportId;
        player.Position = save.Position;
        player.TrailblazerPath = save.TrailblazerPath == 0 ? player.TrailblazerPath : save.TrailblazerPath;
        player.MarchPath = save.MarchPath == 0 ? player.MarchPath : save.MarchPath;

        for (var i = 0; i < save.Lineups.Count && i < LineupBook.SquadCount; i++)
        {
            var saved = save.Lineups[i];
            var squad = player.Lineups.Squad((uint)i);

            squad.Replace(saved.Slots.Select((id, slot) => ((uint)slot, id)), 0);
            squad.SetLeaderAvatar(saved.Leader);

            if (!string.IsNullOrEmpty(saved.Name))
            {
                squad.Name = saved.Name;
            }
        }

        if (player.Lineups.SetCurrent(save.CurrentLineup) != Retcode.RetSucc)
        {
            player.Lineups.SetCurrent(0);
        }

        player.Lineups.Mp = save.Mp;

        foreach (var id in save.EnhancedAvatars)
        {
            player.EnhancedAvatars.Add(id);
        }

        foreach (var id in save.FinishedColleges)
        {
            player.FinishedColleges.Add(id);
        }

        player.DisplayAvatars.AddRange(save.DisplayAvatars.Where(id => id != 0).Distinct().Take(4));

        foreach (var (key, state) in save.AvatarStates)
        {
            if (uint.TryParse(key, out var id) && state.SpMax > 0)
            {
                player.AvatarStates[id] = new AvatarState
                {
                    Hp = Math.Min(state.Hp, Player.FullHp),
                    SpCur = Math.Min(state.SpCur, state.SpMax),
                    SpMax = state.SpMax,
                };
            }
        }

        foreach (var (key, record) in save.ChallengeRecords)
        {
            if (uint.TryParse(key, out var id))
            {
                player.ChallengeRecords[id] = record;
            }
        }

        foreach (var (key, record) in save.PeakRecords)
        {
            if (uint.TryParse(key, out var id))
            {
                player.PeakRecords[id] = record;
            }
        }

        foreach (var (key, avatars) in save.PeakLineups)
        {
            if (uint.TryParse(key, out var id))
            {
                player.PeakLineups[id] = avatars;
            }
        }

        foreach (var (key, data) in save.ServerPrefs)
        {
            if (uint.TryParse(key, out var id))
            {
                try
                {
                    player.ServerPrefs[id] = Convert.FromBase64String(data);
                }
                catch (FormatException)
                {
                    // a hand-edited entry; the client will just resend it
                }
            }
        }
    }

    public sealed class PlayerSave
    {
        public string? Nickname { get; set; }

        public string? Signature { get; set; }

        public uint? WorldLevel { get; set; }

        public uint HeadIcon { get; set; }

        public int Gender { get; set; }

        public uint EntryId { get; set; }

        public uint TeleportId { get; set; }

        public PlayerPosition? Position { get; set; }

        public uint TrailblazerPath { get; set; }

        public uint MarchPath { get; set; }

        public uint CurrentLineup { get; set; }

        public uint Mp { get; set; } = LineupBook.BaseMaxMp;

        public List<SavedLineup> Lineups { get; set; } = [];

        public List<uint> EnhancedAvatars { get; set; } = [];

        public List<uint> FinishedColleges { get; set; } = [];

        public List<uint> DisplayAvatars { get; set; } = [];

        public Dictionary<string, SavedAvatarState> AvatarStates { get; set; } = [];

        public Dictionary<string, ChallengeRecord> ChallengeRecords { get; set; } = [];

        public Dictionary<string, PeakRecord> PeakRecords { get; set; } = [];

        public Dictionary<string, List<uint>> PeakLineups { get; set; } = [];

        public Dictionary<string, string> ServerPrefs { get; set; } = [];
    }

    public sealed class SavedAvatarState
    {
        public uint Hp { get; set; } = Player.FullHp;

        public uint SpCur { get; set; }

        public uint SpMax { get; set; }
    }

    public sealed class SavedLineup
    {
        public string Name { get; set; } = string.Empty;

        public List<uint> Slots { get; set; } = [];

        public uint Leader { get; set; }
    }
}
