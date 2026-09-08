using CapySR.Data.SrTools;
using CapySR.Protocol;

namespace CapySR.GameServer.Battle;

// PF scores against a target object; AS tracks a boss gauge; AA counts its own targets.
// wrong or missing targets leave the client with no win condition, so it hangs at the end.
public static class BattleTargets
{
    public static void Apply(SceneBattleInfo battle, BattleRequest request)
    {
        switch (Resolve(request))
        {
            case BattleKind.PureFiction:
                ApplyPureFiction(battle, request);
                break;

            case BattleKind.ApocalypticShadow:
                // the boss gauge starts empty; its final progress is the node's score
                battle.BattleTargetInfo[1] = List(new BattleTarget { Id = 90005, Progress = 0 });
                break;

            case BattleKind.AnomalyArbitration:
                ApplyPeak(battle, request);
                break;
        }
    }

    // the explicit setting wins; stage id ranges are only a fallback
    private static BattleKind Resolve(BattleRequest request)
    {
        if (request.Kind is not BattleKind.Default)
        {
            return request.Kind;
        }

        return request.StageId switch
        {
            >= 30019000 and <= 30019100 => BattleKind.PureFiction,
            >= 30021000 and <= 30021100 => BattleKind.PureFiction,
            >= 30301000 and <= 30399900 => BattleKind.PureFiction,
            >= 420100 and <= 420900 => BattleKind.ApocalypticShadow,
            _ => BattleKind.Default,
        };
    }

    private static void ApplyPureFiction(SceneBattleInfo battle, BattleRequest request)
    {
        battle.BattleTargetInfo[1] = List(new BattleTarget
        {
            Id = battle.StageId >= 30309011 ? 10003u : 10002u,
            Progress = request.ChallengeScore,
            TotalProgress = 80000,
        });

        for (uint i = 2; i <= 4; i++)
        {
            battle.BattleTargetInfo[i] = new BattleTargetList();
        }

        var scoreTargets = request.ScoreTargets.Count > 0 ? request.ScoreTargets : [2001, 2002];

        battle.BattleTargetInfo[5] = List(
            [.. scoreTargets.Select(id => new BattleTarget { Id = id, Progress = request.ChallengeScore })]);
    }

    private static void ApplyPeak(SceneBattleInfo battle, BattleRequest request)
    {
        if (request.PeakTargets.Count == 0)
        {
            return;
        }

        battle.BattleTargetInfo[5] = List(
            [.. request.PeakTargets.Select(t => new BattleTarget { Id = t.Id, Progress = 0, TotalProgress = t.Param })]);
    }

    private static BattleTargetList List(params BattleTarget[] targets)
    {
        var list = new BattleTargetList();
        list.BattleTargetList_.AddRange(targets);
        return list;
    }
}
