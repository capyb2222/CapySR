#include <algorithm>

#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/battle.h"
#include "game/challenge.h"
#include "game/handlers.h"
#include "game/inventory.h"
#include "game/notify.h"
#include "game/peak.h"
#include "game/player.h"
#include "game/scene.h"
#include "game/tierce.h"
#include "net/cmd_ids.h"
#include "net/handler.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace {

Player* playerOf(net::Session& session, const char* what) {
    Player* player = session.player();
    if (player == nullptr) logging::warn("game", "{} before login", what);
    return player;
}

// Techniques that clear trash without a fight; never inside a challenge.
constexpr uint32_t kFodderKillers[] = {1308, 1408, 1506, 1510};

bool clearsFodder(const Player& player, uint32_t skillIndex, uint32_t casterEntityId) {
    if (skillIndex == 0 || casterEntityId == 0) return false;
    // A challenge node is not cleared by walking through it, and the caster would be
    // looked up in the overworld squad rather than the challenge team.
    if (player.challenge().active || player.peak().active || player.tierce().active) return false;
    std::vector<uint32_t> party = player.lineups().curMembers();
    if (casterEntityId > party.size()) return false;
    uint32_t avatarId = party[casterEntityId - 1];
    return std::find(std::begin(kFodderKillers), std::end(kFodderKillers), avatarId) !=
           std::end(kFodderKillers);
}

bool isMinion(uint32_t monsterId) {
    const data::Tables& tables = data::Tables::get();
    const data::MonsterInfo* monster = tables.monster(monsterId);
    if (monster == nullptr) return false;
    const std::string* rank = tables.monsterRank(monster->templateId);
    return rank != nullptr && rank->rfind("Minion", 0) == 0;
}

// Inside a challenge the fight belongs to the floor, not to the overworld: its own team,
// its maze buff, the buff the player picked for this half, and the cycle count left over
// from the previous node. Nothing here applies outside a run.
void applyChallenge(const Player& player, BattleRequest& request) {
    const ChallengeRun& run = player.challenge();
    if (!run.active) return;
    const data::ChallengeInfo* config = data::Tables::get().challenge(run.challengeId);
    if (config == nullptr) return;

    uint32_t half = run.stage > 1 ? 1u : 0u;
    request.party = run.party[half];
    request.mazeBuffId = config->mazeBuffId;
    request.stageBuffId = run.buffs[half];
    request.roundsLimit = run.roundsLeft;
    request.scoreSoFar = run.totalScore();
    request.battleTargetIds = config->battleTargetIds;
    switch (config->kind) {
        case data::ChallengeKind::Story:
            request.battleType = "PF";
            break;
        case data::ChallengeKind::Boss:
            request.battleType = "AS";
            break;
        case data::ChallengeKind::Memory:
            break;
    }
}

void onSceneCastSkill(net::Session& session, const proto::SceneCastSkillCsReq& req) {
    Player* player = playerOf(session, "SceneCastSkill");
    if (player == nullptr) return;

    proto::SceneCastSkillScRsp rsp;
    rsp.retcode = 0;
    rsp.cast_entity_id = req.cast_entity_id;

    // attacked_by_entity_id is whoever swung: a party actor, or a monster that got the
    // first hit in. cast_entity_id is only a per-cast counter (42, 43, ...), so reading
    // it as the caster lost the slot and made every fight look like an ambush.
    uint32_t caster = 0;
    uint32_t ambusher = 0;
    if (const SceneEntity* attacker = player->sceneState().find(req.attacked_by_entity_id)) {
        if (attacker->kind == EntityKind::Actor) caster = attacker->entityId;
        if (attacker->kind == EntityKind::Monster) ambusher = attacker->entityId;
    }

    // Only monsters start a fight; props and npcs in the hit list are scenery.
    std::vector<uint32_t> monsters;
    auto collect = [&](uint32_t id) {
        const SceneEntity* entity = player->sceneState().find(id);
        if (entity == nullptr || entity->kind != EntityKind::Monster) return;
        if (std::find(monsters.begin(), monsters.end(), id) == monsters.end()) {
            monsters.push_back(id);
        }
    };
    // A monster that struck first leads the fight; what it hit was the party.
    if (ambusher != 0) collect(ambusher);
    for (uint32_t id : req.hit_target_entity_id_list) collect(id);
    for (uint32_t id : req.assist_monster_entity_id_list) collect(id);
    for (const proto::AssistMonsterEntityInfo& info : req.assist_monster_entity_info) {
        for (uint32_t id : info.entity_id_list) collect(id);
    }

    // "swung at nothing" and "named an entity we do not know" look identical from the
    // response, so say which.
    logging::debug("battle", "cast {} by {} skill {} hit {} target(s), {} of them monsters",
                   req.cast_entity_id, req.attacked_by_entity_id, req.skill_index,
                   req.hit_target_entity_id_list.size() + req.assist_monster_entity_id_list.size(),
                   monsters.size());

    if (monsters.empty()) {
        // A technique cast into thin air: nothing to fight, so nothing to answer with.
        session.send(cmd::SceneCastSkillScRsp, rsp);
        return;
    }

    // Which monsters the swing took with it, and how.
    auto report = [&](const std::vector<uint32_t>& ids, proto::MonsterBattleType type) {
        for (uint32_t id : ids) {
            proto::HitMonsterBattleInfo hit;
            hit.target_monster_entity_id = id;
            hit.monster_battle_type = type;
            rsp.monster_battle_info.push_back(hit);
        }
    };

    if (clearsFodder(*player, req.skill_index, caster)) {
        std::vector<uint32_t> killed;
        for (uint32_t entityId : monsters) {
            const SceneEntity* entity = player->sceneState().find(entityId);
            if (entity != nullptr && isMinion(entity->configId)) killed.push_back(entityId);
        }
        if (killed.size() == monsters.size()) {
            report(killed, proto::MonsterBattleType::MONSTER_BATTLE_TYPE_DIRECT_DIE_SKIP_BATTLE);
            notify::monstersRemoved(session, *player, killed);
            session.send(cmd::SceneCastSkillScRsp, rsp);
            return;
        }
    }

    BattleRequest request;
    request.casterEntityId = caster;
    request.attackedByEntityId = ambusher;
    request.skillIndex = req.skill_index;
    request.monsterEntityIds = monsters;
    applyChallenge(*player, request);
    tierce::prepareBattle(*player, request);
    peak::prepareBattle(*player, request);
    rsp.battle_info = battle::create(*player, request);
    report(monsters, proto::MonsterBattleType::MONSTER_BATTLE_TYPE_TRIGGER_BATTLE);
    session.send(cmd::SceneCastSkillScRsp, rsp);
}

void onSceneCastSkillCostMp(net::Session& session, const proto::SceneCastSkillCostMpCsReq& req) {
    Player* player = playerOf(session, "SceneCastSkillCostMp");
    if (player == nullptr) return;

    // Techniques are free here: the pool is refilled and pushed back so the client's
    // own counter does not drift down and start refusing casts.
    LineupBook& book = player->lineups();
    book.setMp(book.maxMp());

    proto::SyncLineupNotify sync;
    sync.lineup = scene::lineupInfo(*player);
    sync.reason_list.push_back(proto::SyncLineupReason::SyncLineupReason_SyncReasonMpAdd);
    session.send(cmd::SyncLineupNotify, sync);

    proto::SceneCastSkillCostMpScRsp rsp;
    rsp.retcode = 0;
    rsp.cast_entity_id = req.cast_entity_id;
    session.send(cmd::SceneCastSkillCostMpScRsp, rsp);
}

int64_t now() { return static_cast<int64_t>(util::nowSec()); }

// The monsters a won fight removes. A Stagnant Shadow stays, to be farmed again.
std::vector<uint32_t> defeated(const Player& player, const std::vector<uint32_t>& entityIds) {
    const data::Tables& tables = data::Tables::get();
    std::vector<uint32_t> out;
    for (uint32_t entityId : entityIds) {
        const SceneEntity* entity = player.sceneState().find(entityId);
        if (entity == nullptr || entity->kind != EntityKind::Monster) continue;
        uint32_t stage = entity->stageId != 0
                             ? entity->stageId
                             : tables.stageForEvent(entity->eventId, player.worldLevel());
        if (stage != 0 && tables.farmElement(stage) != nullptr) continue;
        out.push_back(entityId);
    }
    return out;
}

// Calyx: the one fight a srtools build is allowed to take over, monsters, blessings and
// all. `wave` is how many runs were bought, one stage each.
BattleRequest cocoonRequest(uint32_t cocoonId, uint32_t wave, uint32_t worldLevel) {
    BattleRequest request;
    request.cocoonId = cocoonId;
    request.wave = std::max(1u, wave);
    request.allowSrToolsOverride = true;
    request.worldLevel = worldLevel;

    const data::CocoonInfo* cocoon = data::Tables::get().cocoon(cocoonId, worldLevel);
    if (cocoon == nullptr || cocoon->stageIds.empty()) {
        logging::warn("battle", "cocoon {} has no stage at world level {}", cocoonId, worldLevel);
        return request;
    }
    request.staminaCost = cocoon->staminaCost * request.wave;
    request.mappingInfoId = cocoon->mappingInfoId;
    // Each run draws one of the calyx's stage variants, as the real server does.
    for (uint32_t run = 0; run < request.wave; ++run) {
        uint32_t pick = util::randomRange(0, static_cast<uint32_t>(cocoon->stageIds.size()) - 1);
        request.stageIds.push_back(cocoon->stageIds[pick]);
    }
    return request;
}

// A run the stamina does not cover is refused before it starts. A srtools build fighting
// in its place costs nothing.
bool affordable(Player& player, const BattleRequest& request) {
    if (request.staminaCost == 0 || battle::srToolsTakesOver(player, request)) return true;
    inventory::refreshStamina(player, now());
    return player.stamina() >= request.staminaCost;
}

// Charges `cost`, rolls `runs` runs of the drop table into the bag and tells the client.
std::vector<data::ItemStack> payOut(net::Session& session, Player& player, uint32_t mappingInfoId,
                                    uint32_t worldLevel, uint32_t runs, uint32_t cost) {
    std::vector<data::ItemStack> drops;
    if (const data::MappingInfo* table = inventory::dropTable(mappingInfoId, worldLevel)) {
        for (uint32_t run = 0; run < runs; ++run) {
            std::vector<data::ItemStack> rolled = inventory::rollDrops(*table, gachaRoll);
            drops.insert(drops.end(), rolled.begin(), rolled.end());
        }
    }
    drops = inventory::merged(drops);
    inventory::chargeStamina(player, cost, now());
    inventory::grant(player, drops);
    session.send(cmd::PlayerSyncScNotify, inventory::sync(player, drops));
    session.send(cmd::StaminaInfoScNotify, inventory::staminaInfo(player));
    player.saveNow();
    return drops;
}

void onStartCocoonStage(net::Session& session, const proto::StartCocoonStageCsReq& req) {
    Player* player = playerOf(session, "StartCocoonStage");
    if (player == nullptr) return;

    proto::StartCocoonStageScRsp rsp;
    rsp.cocoon_id = req.cocoon_id;
    rsp.prop_entity_id = req.prop_entity_id;
    rsp.wave = req.wave;
    BattleRequest request = cocoonRequest(req.cocoon_id, req.wave, player->worldLevel());
    if (!affordable(*player, request)) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_STAMINA);
    } else {
        rsp.retcode = 0;
        rsp.battle_info = battle::create(*player, request);
    }
    session.send(cmd::StartCocoonStageScRsp, rsp);
}

void onQuickStartCocoonStage(net::Session& session,
                             const proto::QuickStartCocoonStageCsReq& req) {
    Player* player = playerOf(session, "QuickStartCocoonStage");
    if (player == nullptr) return;

    uint32_t worldLevel = req.world_level != 0 ? req.world_level : player->worldLevel();
    proto::QuickStartCocoonStageScRsp rsp;
    rsp.cocoon_id = req.cocoon_id;
    rsp.wave = req.wave;
    BattleRequest request = cocoonRequest(req.cocoon_id, req.wave, worldLevel);
    if (!affordable(*player, request)) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_STAMINA);
    } else {
        rsp.retcode = 0;
        auto& info = rsp.battle_info.emplace();
        info = battle::create(*player, request);
        info.world_level = worldLevel;
    }
    session.send(cmd::QuickStartCocoonStageScRsp, rsp);
}

// A Stagnant Shadow started from the Survival Index. PAOFHFLFFHD is the shadow's stage,
// the key FarmElementSweep uses too.
void onQuickStartFarmElement(net::Session& session,
                             const proto::QuickStartFarmElementCsReq& req) {
    Player* player = playerOf(session, "QuickStartFarmElement");
    if (player == nullptr) return;

    const data::Tables& tables = data::Tables::get();
    uint32_t worldLevel = req.world_level != 0 ? req.world_level : player->worldLevel();
    BattleRequest request;
    request.worldLevel = worldLevel;
    uint32_t stageId = tables.farmElementStage(req.PAOFHFLFFHD, worldLevel);
    if (stageId != 0) request.stageIds.push_back(stageId);
    if (const data::FarmElementInfo* element = tables.farmElement(stageId)) {
        request.staminaCost = element->staminaCost;
        request.mappingInfoId = element->mappingInfoId;
    }

    proto::QuickStartFarmElementScRsp rsp;
    rsp.PAOFHFLFFHD = req.PAOFHFLFFHD;
    rsp.world_level = worldLevel;
    if (request.stageIds.empty()) {
        logging::warn("battle", "farm element {} has no stage", req.PAOFHFLFFHD);
        rsp.retcode = fail(proto::Retcode::RET_STAGE_NOT_FOUND);
    } else if (!affordable(*player, request)) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_STAMINA);
    } else {
        rsp.retcode = 0;
        auto& info = rsp.battle_info.emplace();
        info = battle::create(*player, request);
        info.world_level = worldLevel;
    }
    session.send(cmd::QuickStartFarmElementScRsp, rsp);
}

void onSceneEnterStage(net::Session& session, const proto::SceneEnterStageCsReq& req) {
    Player* player = playerOf(session, "SceneEnterStage");
    if (player == nullptr) return;

    BattleRequest request;
    uint32_t stageId = data::Tables::get().stageForEvent(req.event_id, player->worldLevel());
    if (stageId != 0) request.stageIds.push_back(stageId);

    proto::SceneEnterStageScRsp rsp;
    rsp.retcode = 0;
    rsp.battle_info = battle::create(*player, request);
    session.send(cmd::SceneEnterStageScRsp, rsp);
}

void onGetCurBattleInfo(net::Session& session, const proto::GetCurBattleInfoCsReq&) {
    // No fight is resumed across a reconnect, but battle_info still has to be there:
    // the client's AdventureModule reads it without a null check and throws otherwise,
    // which leaves the overworld half-initialised on a black screen.
    proto::GetCurBattleInfoScRsp rsp;
    rsp.retcode = 0;
    rsp.battle_info.emplace();
    session.send(cmd::GetCurBattleInfoScRsp, rsp);
}

// Losing, fleeing or retrying: three ways out of a fight that all used to go
// unanswered, which leaves the client sitting on the defeat screen.
void onQuitBattle(net::Session& session, const proto::QuitBattleCsReq&) {
    Player* player = playerOf(session, "QuitBattle");
    if (player == nullptr) return;

    // Fleeing leaves the monsters standing, so only the fight is forgotten.
    BattleContext& context = player->battle();
    context.active = false;
    context.monsterEntityIds.clear();

    proto::QuitBattleScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::QuitBattleScRsp, rsp);
}

void onSceneReviveAfterRebattle(net::Session& session,
                                const proto::SceneReviveAfterRebattleCsReq&) {
    Player* player = playerOf(session, "SceneReviveAfterRebattle");
    if (player == nullptr) return;

    // Nobody here ever carries damage between fights, but the client has just drawn
    // the party as dead and only redraws it from the lineup notify.
    proto::SyncLineupNotify sync;
    sync.lineup = scene::lineupInfo(*player);
    session.send(cmd::SyncLineupNotify, sync);

    proto::SceneReviveAfterRebattleScRsp rsp;
    rsp.retcode = 0;
    session.send(cmd::SceneReviveAfterRebattleScRsp, rsp);
}

void onReEnterLastElementStage(net::Session& session,
                               const proto::ReEnterLastElementStageCsReq& req) {
    Player* player = playerOf(session, "ReEnterLastElementStage");
    if (player == nullptr) return;

    // The retry button names the stage it wants replayed.
    BattleRequest request;
    if (req.stage_id != 0) request.stageIds.push_back(req.stage_id);
    if (const data::FarmElementInfo* element = data::Tables::get().farmElement(req.stage_id)) {
        request.staminaCost = element->staminaCost;
        request.mappingInfoId = element->mappingInfoId;
        request.worldLevel = element->worldLevel;
    }

    proto::ReEnterLastElementStageScRsp rsp;
    rsp.stage_id = req.stage_id;
    if (request.stageIds.empty()) {
        rsp.retcode = fail(proto::Retcode::RET_STAGE_NOT_FOUND);
    } else if (!affordable(*player, request)) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_STAMINA);
    } else {
        rsp.retcode = 0;
        rsp.battle_info = battle::create(*player, request);
    }
    session.send(cmd::ReEnterLastElementStageScRsp, rsp);
}

// Clears the calyx as many times as the stamina covers, in one go.
void onCocoonSweep(net::Session& session, const proto::CocoonSweepCsReq& req) {
    Player* player = playerOf(session, "CocoonSweep");
    if (player == nullptr) return;

    uint32_t worldLevel = req.world_level != 0 ? req.world_level : player->worldLevel();
    const data::CocoonInfo* cocoon = data::Tables::get().cocoon(req.cocoon_id, worldLevel);
    inventory::refreshStamina(*player, now());

    proto::CocoonSweepScRsp rsp;
    rsp.cocoon_id = req.cocoon_id;
    if (cocoon == nullptr || cocoon->staminaCost == 0) {
        rsp.retcode = fail(proto::Retcode::RET_STAGE_NOT_FOUND);
    } else if (player->stamina() < cocoon->staminaCost) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_STAMINA);
    } else {
        uint32_t runs = player->stamina() / cocoon->staminaCost;
        std::vector<data::ItemStack> drops = payOut(session, *player, cocoon->mappingInfoId, worldLevel,
                                                    runs, runs * cocoon->staminaCost);
        rsp.retcode = 0;
        rsp.NCEFJFOHGBI = runs;
        rsp.multiple_drop_data = inventory::itemList(drops);
    }
    session.send(cmd::CocoonSweepScRsp, rsp);
}

// The same for a Stagnant Shadow, named the way QuickStartFarmElement names it.
void onFarmElementSweep(net::Session& session, const proto::FarmElementSweepCsReq& req) {
    Player* player = playerOf(session, "FarmElementSweep");
    if (player == nullptr) return;

    const data::Tables& tables = data::Tables::get();
    uint32_t worldLevel = req.world_level != 0 ? req.world_level : player->worldLevel();
    const data::FarmElementInfo* element =
        tables.farmElement(tables.farmElementStage(req.PAOFHFLFFHD, worldLevel));
    inventory::refreshStamina(*player, now());

    proto::FarmElementSweepScRsp rsp;
    rsp.PAOFHFLFFHD = req.PAOFHFLFFHD;
    if (element == nullptr || element->staminaCost == 0) {
        rsp.retcode = fail(proto::Retcode::RET_STAGE_NOT_FOUND);
    } else if (player->stamina() < element->staminaCost) {
        rsp.retcode = fail(proto::Retcode::RET_LACK_STAMINA);
    } else {
        uint32_t runs = player->stamina() / element->staminaCost;
        uint32_t level = element->worldLevel != 0 ? element->worldLevel : worldLevel;
        std::vector<data::ItemStack> drops = payOut(session, *player, element->mappingInfoId, level,
                                                    runs, runs * element->staminaCost);
        rsp.retcode = 0;
        rsp.multiple_drop_data = inventory::itemList(drops);
    }
    session.send(cmd::FarmElementSweepScRsp, rsp);
}

void onPveBattleResult(net::Session& session, const proto::PVEBattleResultCsReq& req) {
    Player* player = playerOf(session, "PVEBattleResult");
    if (player == nullptr) return;

    BattleContext& context = player->battle();
    bool won = req.end_status == proto::BattleEndStatus::BATTLE_END_WIN && context.active &&
               context.battleId == req.battle_id;
    if (won) notify::monstersRemoved(session, *player, defeated(*player, context.monsterEntityIds));
    // A farming run costs and pays only when it is won; losing or fleeing is free.
    std::vector<data::ItemStack> drops;
    if (won && (context.staminaCost != 0 || context.mappingInfoId != 0)) {
        drops = payOut(session, *player, context.mappingInfoId, context.worldLevel, context.runs,
                       context.staminaCost);
    }
    context.active = false;
    context.monsterEntityIds.clear();

    // The monsters are gone from the scene by now, which is how the run tells whether
    // the node it was fighting is finished.
    challenge::battleFinished(session, *player, req);
    tierce::battleFinished(session, *player, req);
    peak::battleFinished(session, *player, req);

    // The fight was simulated on the client; the server only confirms the outcome.
    proto::PVEBattleResultScRsp rsp;
    rsp.retcode = 0;
    rsp.battle_id = req.battle_id;
    rsp.stage_id = req.stage_id;
    rsp.end_status = req.end_status;
    rsp.check_identical = true;
    if (!drops.empty()) rsp.drop_data = inventory::itemList(drops);
    session.send(cmd::PVEBattleResultScRsp, rsp);
}

}  // namespace

void registerBattleHandlers() {
    net::on<proto::SceneCastSkillCsReq>(cmd::SceneCastSkillCsReq, onSceneCastSkill);
    net::on<proto::SceneCastSkillCostMpCsReq>(cmd::SceneCastSkillCostMpCsReq,
                                              onSceneCastSkillCostMp);
    net::on<proto::StartCocoonStageCsReq>(cmd::StartCocoonStageCsReq, onStartCocoonStage);
    net::on<proto::QuickStartCocoonStageCsReq>(cmd::QuickStartCocoonStageCsReq,
                                               onQuickStartCocoonStage);
    net::on<proto::QuickStartFarmElementCsReq>(cmd::QuickStartFarmElementCsReq,
                                               onQuickStartFarmElement);
    net::on<proto::SceneEnterStageCsReq>(cmd::SceneEnterStageCsReq, onSceneEnterStage);
    net::on<proto::GetCurBattleInfoCsReq>(cmd::GetCurBattleInfoCsReq, onGetCurBattleInfo);
    net::on<proto::PVEBattleResultCsReq>(cmd::PVEBattleResultCsReq, onPveBattleResult);
    net::on<proto::QuitBattleCsReq>(cmd::QuitBattleCsReq, onQuitBattle);
    net::on<proto::SceneReviveAfterRebattleCsReq>(cmd::SceneReviveAfterRebattleCsReq,
                                                  onSceneReviveAfterRebattle);
    net::on<proto::ReEnterLastElementStageCsReq>(cmd::ReEnterLastElementStageCsReq,
                                                 onReEnterLastElementStage);
    net::on<proto::CocoonSweepCsReq>(cmd::CocoonSweepCsReq, onCocoonSweep);
    net::on<proto::FarmElementSweepCsReq>(cmd::FarmElementSweepCsReq, onFarmElementSweep);
}

}  // namespace game
