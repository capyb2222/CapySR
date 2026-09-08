using CapySR.GameServer.Game;
using CapySR.Protocol;
using Xunit;

namespace CapySR.Tests;

public class LineupTests
{
    private static Lineup Team(params uint[] ids)
    {
        var lineup = new Lineup();
        lineup.Fill(ids);
        return lineup;
    }

    [Fact]
    public void FillTakesTheFirstFour()
    {
        var lineup = Team(1001, 1002, 1003, 1004, 1005);

        Assert.Equal([1001u, 1002u, 1003u, 1004u], lineup.Slots);
        Assert.Equal(4, lineup.Count);
        Assert.Equal(1001u, lineup.LeaderAvatarId);
        Assert.Equal(0u, lineup.LeaderSlot);
    }

    [Fact]
    public void JoinPlacesIntoASlot()
    {
        var lineup = Team(1001);

        Assert.Equal(Retcode.RetSucc, lineup.Join(1002, 2));
        Assert.Equal(1002u, lineup.Slots[2]);
        Assert.Equal(2, lineup.Count);
        Assert.Equal([1001u, 1002u], lineup.AvatarIds);
    }

    [Fact]
    public void JoiningTwiceMovesRatherThanDuplicates()
    {
        var lineup = Team(1001, 1002, 1003);

        Assert.Equal(Retcode.RetSucc, lineup.Join(1003, 0));

        Assert.Equal(1, lineup.Slots.Count(id => id == 1003));
        Assert.Equal(1003u, lineup.Slots[0]);
        // whatever was in slot 0 moved to where 1003 came from
        Assert.Equal(1001u, lineup.Slots[2]);
    }

    [Fact]
    public void JoiningTheSameSlotIsRejected()
    {
        var lineup = Team(1001, 1002);

        Assert.Equal(Retcode.RetLineupAvatarAlreadyIn, lineup.Join(1002, 1));
        Assert.Equal(Retcode.RetLineupInvalidMemberPos, lineup.Join(1003, 4));
    }

    [Fact]
    public void QuitRemovesButNeverEmptiesTheTeam()
    {
        var lineup = Team(1001, 1002);

        Assert.Equal(Retcode.RetSucc, lineup.Quit(1002));
        Assert.Equal(1, lineup.Count);

        Assert.Equal(Retcode.RetLineupOnlyOneMember, lineup.Quit(1001));
        Assert.Equal(1, lineup.Count);

        Assert.Equal(Retcode.RetLineupAvatarNotExist, lineup.Quit(1009));
    }

    [Fact]
    public void SwapExchangesSlotsAndTheLeaderFollows()
    {
        var lineup = Team(1001, 1002, 1003, 1004);

        Assert.Equal(Retcode.RetSucc, lineup.Swap(0, 3));

        Assert.Equal(1004u, lineup.Slots[0]);
        Assert.Equal(1001u, lineup.Slots[3]);
        Assert.Equal(1001u, lineup.LeaderAvatarId);
        Assert.Equal(3u, lineup.LeaderSlot);
        Assert.Equal(Retcode.RetLineupSwapSameSlot, lineup.Swap(1, 1));
    }

    [Fact]
    public void ReplaceSetsSlotsAndLeader()
    {
        var lineup = new Lineup();

        lineup.Replace([(0u, 1005u), (2u, 1006u)], leaderSlot: 2);

        Assert.Equal(1005u, lineup.Slots[0]);
        Assert.Equal(0u, lineup.Slots[1]);
        Assert.Equal(1006u, lineup.Slots[2]);
        Assert.Equal(1006u, lineup.LeaderAvatarId);
        Assert.Equal(2u, lineup.LeaderSlot);
        Assert.Equal(1u, lineup.LeaderIndex);
    }

    [Fact]
    public void ReplaceDropsDuplicates()
    {
        var lineup = new Lineup();

        lineup.Replace([(0u, 1005u), (1u, 1005u), (2u, 1006u)], leaderSlot: 0);

        Assert.Equal([1005u, 0u, 1006u, 0u], lineup.Slots);
    }

    [Fact]
    public void LeaderMustPointAtSomeone()
    {
        var lineup = Team(1001, 1002);

        Assert.Equal(Retcode.RetSucc, lineup.SetLeader(1));
        Assert.Equal(1002u, lineup.LeaderAvatarId);

        // removing the leader moves it to whoever is left
        lineup.Quit(1002);
        Assert.Equal(1001u, lineup.LeaderAvatarId);
        Assert.Equal(0u, lineup.LeaderSlot);

        Assert.Equal(Retcode.RetLineupNotValidLeader, lineup.SetLeader(3));
        Assert.Equal(Retcode.RetLineupNotValidLeader, lineup.SetLeader(1));
    }

    [Fact]
    public void ReplaceWithAHoleKeepsTheLeaderValid()
    {
        var lineup = new Lineup();

        lineup.Replace([(1u, 1007u)], leaderSlot: 0);

        Assert.Equal(1007u, lineup.LeaderAvatarId);
        Assert.Equal(1u, lineup.LeaderSlot);
        Assert.Equal(1007u, lineup.Slots[1]);
    }

    [Fact]
    public void OccupiedSkipsHoles()
    {
        var lineup = new Lineup();
        lineup.Replace([(0u, 1001u), (3u, 1004u)], leaderSlot: 0);

        Assert.Equal([(0u, 1001u), (3u, 1004u)], lineup.Occupied);
        Assert.Equal([1001u, 1004u], lineup.AvatarIds);
    }

    [Fact]
    public void PruneDropsWhatIsNoLongerOwned()
    {
        var lineup = Team(1001, 1002, 1003);
        lineup.SetLeader(2);

        Assert.True(lineup.Prune(id => id != 1003));
        Assert.Equal([1001u, 1002u, 0u, 0u], lineup.Slots);
        Assert.Equal(1001u, lineup.LeaderAvatarId);
        Assert.False(lineup.Prune(_ => true));
    }
}

public class LineupBookTests
{
    [Fact]
    public void SeedFillsOnlyTheFirstSquadOnce()
    {
        var book = new LineupBook();
        book.Seed([1001, 1002, 1003, 1004, 1005]);

        Assert.Equal([1001u, 1002u, 1003u, 1004u], book.Squad(0).Slots);
        Assert.True(book.Squad(1).IsEmpty);

        book.Seed([1009]);
        Assert.Equal(1001u, book.Squad(0).Slots[0]);
    }

    [Fact]
    public void SwitchingNeedsANonEmptySquad()
    {
        var book = new LineupBook();
        book.Seed([1001, 1002]);

        Assert.Equal(Retcode.RetLineupIsEmpty, book.SetCurrent(1));
        Assert.Equal(Retcode.RetLineupInvalidIndex, book.SetCurrent(9));

        book.Squad(1).Fill([1003]);
        Assert.Equal(Retcode.RetSucc, book.SetCurrent(1));
        Assert.Same(book.Squad(1), book.Current);
    }

    [Fact]
    public void ExtraLineupsTakeOverWhileActive()
    {
        var book = new LineupBook();
        book.Seed([1001, 1002]);

        var challenge = book.SetExtra(ExtraLineupType.LineupChallenge, [1003, 1004]);

        Assert.True(book.InExtraLineup);
        Assert.Same(challenge, book.Current);
        Assert.Equal([1003u, 1004u], book.Current.AvatarIds);

        book.ClearExtra();
        Assert.False(book.InExtraLineup);
        Assert.Equal([1001u, 1002u], book.Current.AvatarIds);
    }

    [Fact]
    public void TechniquePointsAreSharedAndCapped()
    {
        var book = new LineupBook();
        book.Seed([1001, 1002]);

        Assert.Equal(LineupBook.BaseMaxMp, book.MaxMp);
        Assert.True(book.SpendMp());
        Assert.Equal(LineupBook.BaseMaxMp - 1, book.Mp);

        book.GainMp(10);
        Assert.Equal(LineupBook.BaseMaxMp, book.Mp);

        // Phainon raises the cap while he is in the team
        book.Squad(0).Join(1408, 2);
        Assert.Equal(LineupBook.BaseMaxMp + 3, book.MaxMp);

        book.RefillMp();
        Assert.Equal(LineupBook.BaseMaxMp + 3, book.Mp);

        book.Squad(0).Quit(1408);
        Assert.Equal(LineupBook.BaseMaxMp, book.Mp);
    }
}
