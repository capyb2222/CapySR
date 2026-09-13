#include "game/battle.h"

#include <algorithm>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/player.h"
#include "game/srtools.h"

namespace game {
namespace {

constexpr uint32_t kAllWaves = 0xFFFFFFFFu;
constexpr uint32_t kNoOwner = 0xFFFFFFFFu;
constexpr uint32_t kAmbushBuffId = 1000102;
constexpr uint32_t kIgnoreWeaknessBuffId = 1000119;
constexpr uint32_t kLeaderCheckBuffId = 1000121;
// A relic to hang the srtools stat overrides on when an avatar wears none.
constexpr uint32_t kPlaceholderRelicId = 61011;

// These are cast by whoever is leading, not by the avatar that brought them.
bool ownedByLeader(uint32_t buffId) {
    return buffId == 141202 || buffId == 141403 || buffId == kLeaderCheckBuffId;
}

proto::BattleBuff makeBuff(uint32_t id, uint32_t level, uint32_t ownerIndex) {
    proto::BattleBuff buff;
    buff.id = id;
    buff.level = level;
    buff.owner_index = ownerIndex;
    buff.wave_flag = kAllWaves;
    return buff;
}

void addWave(proto::SceneBattleInfo& info, uint32_t stageId,
             const std::vector<uint32_t>& monsterIds, uint32_t level) {
    if (monsterIds.empty()) return;
    proto::SceneMonsterWave wave;
    wave.battle_wave_id = static_cast<uint32_t>(info.monster_wave_list.size()) + 1;
    wave.battle_stage_id = stageId;
    // Left empty for a real stage: the client reads the level off StageConfig itself.
    auto& param = wave.monster_param.emplace();
    param.level = level;
    for (uint32_t id : monsterIds) {
        proto::SceneMonster monster;
        monster.monster_id = id;
        wave.monster_list.push_back(monster);
    }
    info.monster_wave_list.push_back(std::move(wave));
}

bool useSrToolsBattle(const BattleConfig& config, bool allowOverride) {
    if (config.waves.empty() || config.stageId == 0) return false;
    const std::string& source = core::Config::get().gameplay.battleSource;
    if (source == "stage") return false;
    if (source == "srtools") return true;
    return allowOverride;  // "auto": the calyx only
}

// Battle types that need a win condition of their own, or the client hangs on the
// last turn with nothing to settle. `targets` is Pure Fiction's per-wave score targets
// when the caller knows them (a real floor does, a srtools build does not), and
// `progress` is the score already banked in the first half.
void addBattleTargets(proto::SceneBattleInfo& info, const std::string& battleType,
                      const std::vector<uint32_t>& targets, uint32_t progress) {
    auto target = [&](uint32_t key, uint32_t id) {
        proto::BattleTarget entry;
        entry.id = id;
        entry.progress = progress;
        info.battle_target_info[key].battle_target_list.push_back(entry);
    };
    if (battleType == "PF") {
        target(1, info.stage_id >= 30309011 ? 10003u : 10002u);
        for (uint32_t key = 2; key <= 4; ++key) info.battle_target_info[key];
        if (targets.empty()) {
            target(5, 2001);
            target(5, 2002);
        } else {
            for (uint32_t id : targets) target(5, id);
        }
    } else if (battleType == "AS") {
        target(1, 90005);
    } else if (battleType == "AA") {
        // Every slot present; the fight's own targets go in the fifth, each with the
        // ceiling it is judged against.
        for (uint32_t key = 1; key <= 4; ++key) info.battle_target_info[key];
        for (uint32_t id : targets) {
            const data::BattleTargetInfo* config = data::Tables::get().battleTarget(id);
            proto::BattleTarget entry;
            entry.id = id;
            entry.total_progress = config != nullptr ? config->param : 0;
            info.battle_target_info[5].battle_target_list.push_back(entry);
        }
    }
}

void applyCustomStats(proto::SceneBattleInfo& info, const std::vector<SubAffix>& stats) {
    if (stats.empty()) return;
    for (proto::BattleAvatar& avatar : info.battle_avatar_list) {
        if (avatar.relic_list.empty()) {
            proto::BattleRelic filler;
            filler.id = kPlaceholderRelicId;
            filler.main_affix_id = 1;
            filler.level = 1;
            avatar.relic_list.push_back(std::move(filler));
        }
        proto::BattleRelic& relic = avatar.relic_list.front();
        for (const SubAffix& stat : stats) {
            auto it = std::find_if(relic.sub_affix_list.begin(), relic.sub_affix_list.end(),
                                   [&](const proto::RelicAffix& a) { return a.affix_id == stat.id; });
            if (it != relic.sub_affix_list.end()) {
                it->cnt = stat.count;
                it->step = stat.step;
            } else {
                proto::RelicAffix affix;
                affix.affix_id = stat.id;
                affix.cnt = stat.count;
                affix.step = stat.step;
                relic.sub_affix_list.push_back(affix);
            }
        }
    }
}

}  // namespace

namespace battle {

proto::SceneBattleInfo create(Player& player, const BattleRequest& request) {
    const data::Tables& tables = data::Tables::get();
    Roster roster = player.roster();
    const BattleConfig& config = roster.data().battle;
    LineupBook& book = player.lineups();

    proto::SceneBattleInfo info;
    info.battle_id = player.nextBattleId();
    info.world_level = player.worldLevel();
    info.logic_random_seed = util::randomU32();

    // ---- who is fighting ------------------------------------------------------
    std::vector<uint32_t> party = book.curMembers();
    if (!request.party.empty()) {
        // A challenge brings its own team, and it is not the one on the overworld.
        party = request.party;
    } else if (!config.customLineup.empty()) {
        party.clear();
        for (const auto& [slot, avatarId] : config.customLineup) {
            (void)slot;
            if (avatarId != 0) party.push_back(avatarId);
        }
    }
    if (party.empty()) {
        logging::warn("battle", "no party to fight with");
        return info;
    }

    uint32_t leaderIndex = 0;
    if (request.casterEntityId != 0 && request.casterEntityId <= party.size()) {
        leaderIndex = request.casterEntityId - 1;
    } else {
        leaderIndex = std::min(book.leaderSlot(book.curIndex()),
                               static_cast<uint32_t>(party.size() - 1));
    }

    for (uint32_t index = 0; index < party.size(); ++index) {
        info.battle_avatar_list.push_back(
            roster.toBattleAvatar(party[index], index, info.buff_list));
    }
    for (proto::BattleBuff& buff : info.buff_list) {
        if (ownedByLeader(buff.id)) buff.owner_index = leaderIndex;
    }

    // ---- what they are fighting ----------------------------------------------
    bool srtools = useSrToolsBattle(config, request.allowSrToolsOverride);
    if (srtools) {
        info.stage_id = config.stageId;
        info.rounds_limit = config.cycleCount;
        // A calyx sweep buys several runs, one fight each, so the build's waves repeat.
        for (uint32_t run = 0; run < std::max(1u, request.wave); ++run) {
            for (const std::vector<BattleMonster>& wave : config.waves) {
                std::vector<uint32_t> ids;
                uint32_t level = 1;
                for (const BattleMonster& monster : wave) {
                    for (uint32_t n = 0; n < std::max(1u, monster.amount); ++n) {
                        ids.push_back(monster.monsterId);
                    }
                    level = std::max(level, monster.level);
                }
                addWave(info, config.stageId, ids, level);
            }
        }
        for (const BattleBuff& blessing : config.blessings) {
            proto::BattleBuff buff = makeBuff(blessing.id, blessing.level, kNoOwner);
            for (const auto& [key, value] : blessing.dynamicValues) buff.dynamic_values[key] = value;
            info.buff_list.push_back(std::move(buff));
        }
        addBattleTargets(info, config.battleType, {}, 0);
        if (config.battleType == "SU" && config.pathResonanceId != 0) {
            auto& event = info.battle_event.emplace_back();
            event.battle_event_id = config.pathResonanceId;
            auto& bar = event.status.emplace().sp_bar.emplace();
            bar.cur_sp = 10000;
            bar.max_sp = 10000;
        }
    } else if (!request.stageIds.empty()) {
        // A named stage, or several of them for a multi-run calyx.
        info.stage_id = request.stageIds.front();
        for (uint32_t stageId : request.stageIds) {
            const data::StageInfo* stage = tables.stage(stageId);
            if (stage == nullptr) {
                logging::warn("battle", "stage {} is not in the tables", stageId);
                continue;
            }
            for (const std::vector<uint32_t>& wave : stage->waves) addWave(info, stageId, wave, 0);
        }
    } else {
        // Overworld: each monster's PlaneEvent names its stage at this world level,
        // and every monster the swing connected with chains on as further waves.
        for (uint32_t entityId : request.monsterEntityIds) {
            const SceneEntity* entity = player.sceneState().find(entityId);
            if (entity == nullptr || entity->kind != EntityKind::Monster) continue;
            // A challenge monster carries its stage outright; an overworld one has to
            // go through PlaneEvent at the player's world level.
            uint32_t monsterStage =
                entity->stageId != 0 ? entity->stageId
                                     : tables.stageForEvent(entity->eventId, player.worldLevel());
            if (info.stage_id == 0) info.stage_id = monsterStage;

            const data::StageInfo* stage = tables.stage(monsterStage);
            if (stage != nullptr && !stage->waves.empty()) {
                for (const std::vector<uint32_t>& wave : stage->waves) {
                    addWave(info, monsterStage, wave, 0);
                }
            } else {
                // No stage for this event: fight the model that is standing there.
                addWave(info, monsterStage, {entity->configId}, 0);
            }
        }
    }

    // ---- what the floor itself brings ----------------------------------------
    if (request.roundsLimit != 0) info.rounds_limit = request.roundsLimit;
    for (uint32_t buffId : {request.mazeBuffId, request.stageBuffId}) {
        if (buffId != 0) info.buff_list.push_back(makeBuff(buffId, 1, kNoOwner));
    }
    for (uint32_t buffId : request.floorBuffIds) {
        if (buffId != 0) info.buff_list.push_back(makeBuff(buffId, 1, kNoOwner));
    }
    if (!request.battleType.empty()) {
        addBattleTargets(info, request.battleType, request.battleTargetIds, request.scoreSoFar);
    }

    // ---- entry buffs ----------------------------------------------------------
    uint32_t casterAvatar = roster.resolvePath(party[leaderIndex]);
    const data::AvatarInfo* casterInfo = tables.avatar(casterAvatar);
    bool ignoresWeakness = false;
    if (const Avatar* src = roster.find(casterAvatar)) {
        ignoresWeakness = std::find(src->techniques.begin(), src->techniques.end(),
                                    kIgnoreWeaknessBuffId) != src->techniques.end();
    }
    if (casterInfo != nullptr && casterInfo->attackBuffId != 0 && !ignoresWeakness) {
        proto::BattleBuff buff = makeBuff(casterInfo->attackBuffId, 1, leaderIndex);
        // skill_index 0 is a basic attack, 1 a technique; the buff counts from one.
        buff.dynamic_values["SkillIndex"] = static_cast<float>(request.skillIndex + 1);
        info.buff_list.push_back(std::move(buff));
    }
    if (request.attackedByEntityId != 0) {
        // Ambushed: only the first wave starts at a disadvantage.
        proto::BattleBuff buff = makeBuff(kAmbushBuffId, 1, kNoOwner);
        buff.wave_flag = 1;
        info.buff_list.push_back(std::move(buff));
    }

    if (player.globalBuffs()) {
        for (uint32_t buffId : tables.globalMazeBuffs()) {
            bool already = std::any_of(info.buff_list.begin(), info.buff_list.end(),
                                       [&](const proto::BattleBuff& b) { return b.id == buffId; });
            if (!already) info.buff_list.push_back(makeBuff(buffId, 1, kNoOwner));
        }
    }

    applyCustomStats(info, config.customStats);

    BattleContext& context = player.battle();
    context.active = true;
    context.battleId = info.battle_id;
    context.stageId = info.stage_id;
    context.cocoonId = request.cocoonId;
    context.wave = request.wave;
    context.monsterEntityIds = request.monsterEntityIds;
    // A srtools build fighting in the calyx's place is not a farming run.
    context.staminaCost = srtools ? 0 : request.staminaCost;
    context.mappingInfoId = srtools ? 0 : request.mappingInfoId;
    context.worldLevel = request.worldLevel != 0 ? request.worldLevel : player.worldLevel();
    context.runs = std::max(1u, request.wave);

    logging::debug("battle", "battle {} stage {} with {} wave(s) and {} buff(s)", info.battle_id,
                   info.stage_id, info.monster_wave_list.size(), info.buff_list.size());
    return info;
}

bool srToolsTakesOver(const Player& player, const BattleRequest& request) {
    Roster roster = player.roster();
    return useSrToolsBattle(roster.data().battle, request.allowSrToolsOverride);
}

}  // namespace battle
}  // namespace game
