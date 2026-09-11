// Exercises the game layer against the real tables and scene dump when they are
// present; the data-driven checks are skipped (and reported) when they are not.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/util.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/battle.h"
#include "game/challenge.h"
#include "game/lineup.h"
#include "game/player.h"
#include "game/roster.h"
#include "game/scene.h"
#include "game/srtools.h"
#include "net/packet.h"
#include "tests/harness.h"

namespace {

using testing::check;

void testLineupPacking() {
    game::LineupBook book;
    check(book.curIndex() == 0, "lineup starts on squad 1");
    check(book.maxMp() == game::kBaseMazeMp, "empty squad has the base mp cap");

    check(book.join(0, 0, 1001), "join slot 0");
    check(book.join(0, 1, 1002), "join slot 1");
    check(book.join(0, 2, 1003), "join slot 2");
    check(book.join(0, 3, 1004), "join slot 3");
    check(!book.join(0, 3, 1005), "a full squad refuses a fifth member");
    check(book.members(0).size() == 4, "four members");
    check(book.leaderSlot(0) == 0, "the first to join leads");

    // A join that names an occupied slot moves the member, never duplicates it.
    check(book.join(0, 0, 1004), "move 1004 to the front");
    std::vector<uint32_t> moved = book.members(0);
    check(moved.size() == 4, "moving keeps the squad size");
    check(moved[0] == 1004 && moved[1] == 1001 && moved[2] == 1002 && moved[3] == 1003,
          "moving shifts the others right");

    check(book.quit(0, 1002), "quit 1002");
    std::vector<uint32_t> after = book.members(0);
    check(after.size() == 3, "three left");
    check(after[0] == 1004 && after[1] == 1001 && after[2] == 1003, "members stay packed");

    check(book.swap(0, 0, 2), "swap the ends");
    check(book.members(0)[0] == 1003 && book.members(0)[2] == 1004, "swap took effect");
    check(!book.swap(0, 0, 3), "cannot swap with an empty slot");

    // The leader follows the avatar, not the slot.
    book.setLeaderSlot(0, 2);
    check(book.squad(0).leaderAvatarId == 1004, "leader set by slot");
    book.swap(0, 0, 2);
    check(book.leaderSlot(0) == 0, "leader follows its avatar across a swap");

    book.quit(0, 1004);
    check(book.squad(0).leaderAvatarId != 0, "leader moves on when it leaves");

    book.replace(0, {1001, 1002, 1001, 1003}, 1);
    check(book.members(0).size() == 3, "replace drops the duplicate");
    check(book.squad(0).leaderAvatarId == 1002, "replace honours leader_slot");

    // The last member cannot leave: the client refuses an empty party.
    book.replace(0, {8001}, 0);
    check(!book.quit(0, 8001), "the last member stays");
    check(book.members(0).size() == 1, "squad still has its member");
}

void testSquadSwitching() {
    game::LineupBook book;
    book.join(0, 0, 1001);
    book.join(2, 0, 1002);
    book.join(2, 1, 1003);

    book.setCurIndex(2);
    check(book.curIndex() == 2, "switched to squad 3");
    check(book.curMembers().size() == 2, "and it is that squad's members that walk around");

    // Out of range is ignored rather than wrapping; the handler answers it as an error.
    book.setCurIndex(game::kSquadCount);
    check(book.curIndex() == 2, "an out of range index changes nothing");

    check(book.members(1).empty(), "squad 2 was never filled");
    check(!book.squad(1).favourite, "and is not marked a favourite");
    book.squad(1).favourite = true;
    check(book.squad(1).favourite, "the mark sticks");
}

void testMazeMpCap() {
    game::LineupBook book;
    book.join(0, 0, 1001);
    book.setMp(99);
    check(book.mp() == game::kBaseMazeMp, "mp is clamped to the cap");

    book.join(0, 1, 1408);  // Phainon raises the cap by three
    check(book.maxMp() == game::kBaseMazeMp + 3, "cap raiser widens the pool");
    book.setMp(8);
    check(book.mp() == 8, "the wider pool can be filled");

    book.quit(0, 1408);
    check(book.maxMp() == game::kBaseMazeMp, "cap drops again");
    check(book.mp() == game::kBaseMazeMp, "mp follows the cap down");
}

void testItemUniqueIds() {
    // srtools numbers from zero and unique id 0 means "nothing", so nothing maps to 0
    // and the two ranges never meet.
    check(game::relicUniqueId(0) == 1, "first relic is 1");
    check(game::equipmentUniqueId(0) == 3001, "first lightcone is 3001");
    check(game::relicUniqueId(1274) < game::equipmentUniqueId(0), "ranges do not overlap");
    check(game::relicInternalUid(game::relicUniqueId(42)) == 42, "relic id round trips");
    check(game::equipmentInternalUid(game::equipmentUniqueId(42)) == 42, "lightcone round trips");
    check(game::equipmentInternalUid(0) == 0, "unset lightcone id stays 0");
}

void testElementBuffs() {
    check(data::attackBuffForElement("Physical") == 1000111, "physical entry buff");
    check(data::attackBuffForElement("Fire") == 1000112, "fire entry buff");
    check(data::attackBuffForElement("Ice") == 1000113, "ice entry buff");
    check(data::attackBuffForElement("Thunder") == 1000114, "lightning entry buff");
    check(data::attackBuffForElement("Wind") == 1000115, "wind entry buff");
    check(data::attackBuffForElement("Quantum") == 1000116, "quantum entry buff");
    check(data::attackBuffForElement("Imaginary") == 1000117, "imaginary entry buff");
    check(data::attackBuffForElement("Nonsense") == 0, "unknown element has no buff");
}

void testTables() {
    const data::Tables& tables = data::Tables::get();
    if (!tables.loaded()) {
        std::printf("SKIP game tables (no data source configured)\n");
        return;
    }

    check(tables.avatar(1001) != nullptr, "March 7th is in the tables");
    const data::AvatarInfo* march = tables.avatar(1001);
    check(march != nullptr && march->damageType == "Ice", "March is an ice character");
    check(march != nullptr && march->attackBuffId == 1000113, "her entry buff follows her element");
    check(march != nullptr && march->spNeed == 12000, "SPNeed is scaled to hundredths");
    check(!march->techniqueBuffs.empty() && march->techniqueBuffs[0] == 100101,
          "her technique buff comes from AvatarDefaultMazeBuff");

    check(tables.baseAvatarId(8008) == 8001, "a Trailblazer path folds into 8001");
    check(tables.baseAvatarId(1224) == 1001, "March the Hunt folds into 1001");
    check(tables.baseAvatarId(1310) == 1310, "everyone else is their own base");
    check(tables.multiPathVariants(8001) != nullptr, "8001 has paths");
    check(tables.multiPathVariants(1310) == nullptr, "1310 has none");

    const std::vector<uint32_t>& globals = tables.globalMazeBuffs();
    check(globals.size() == 2, "two global maze buffs");
    check(std::find(globals.begin(), globals.end(), 140703u) != globals.end(), "Castorice global");
    check(std::find(globals.begin(), globals.end(), 150602u) != globals.end(), "Silver Wolf global");

    // PlaneEvent 10301299 at world level 6 is the Express corridor fight.
    check(tables.stageForEvent(10301299, 6) == 103012996, "plane event resolves its stage");
    check(tables.stageForEvent(10301299, 0) != 0, "an unlisted world level falls back");
    check(tables.stageForEvent(999999999, 6) == 0, "an unknown event resolves to nothing");

    const data::StageInfo* stage = tables.stage(103012996);
    check(stage != nullptr && !stage->waves.empty(), "the stage has monster waves");

    // CocoonConfig is keyed by id * 100 + world level.
    const data::CocoonInfo* cocoon = tables.cocoon(1001, 6);
    check(cocoon != nullptr, "calyx 1001 exists at world level 6");
    check(cocoon != nullptr && cocoon->worldLevel == 6, "and it is the world level 6 row");
    check(cocoon != nullptr && !cocoon->stageIds.empty(), "with stages to draw from");

    // FarmElementConfig: a shadow comes as its stage id, or as its id at a world level.
    check(tables.farmElementStage(1012011, 6) == 1012011, "a shadow's stage id is its own stage");
    check(tables.farmElementStage(1101, 1) == 1012011, "a shadow id resolves at its world level");
    check(tables.farmElementStage(999999999, 0) == 0, "an unknown shadow resolves to nothing");

    const data::StandardGacha& gacha = tables.standardGacha();
    check(gacha.gachaId == 1001, "the standard warp is pool 1001");
    check(gacha.ceilingNum == 300 && gacha.ceilingAvatars.size() == 7,
          "with its seven picks at 300 pulls");

    check(tables.entrance(1000001) != nullptr, "map entrance 1000001 is loaded");
    check(tables.plane(10000) != nullptr, "plane 10000 is loaded");
}

void testChallengeHistory() {
    const data::Tables& tables = data::Tables::get();
    if (!tables.loaded()) return;

    check(tables.challenges().size() > 700, "the three challenge modes loaded their floors");
    check(tables.challengeGroups().size() > 90, "and their seasons");

    // Memory of Chaos floor 1 of the first season, and the season it belongs to.
    const data::ChallengeInfo* floor = nullptr;
    for (const data::ChallengeInfo& c : tables.challenges()) {
        if (c.id == 1) floor = &c;
    }
    check(floor != nullptr && floor->groupId == 100, "challenge 1 is in group 100");
    check(floor != nullptr && floor->targetIds.size() == 3, "a floor has three star targets");

    // Reward line 1 pays out every three stars from 3 to 36.
    uint64_t mask = tables.challengeRewardStars(1);
    check((mask & (uint64_t{1} << 3)) != 0, "reward line 1 pays out at 3 stars");
    check((mask & (uint64_t{1} << 36)) != 0, "and at 36");
    check((mask & (uint64_t{1} << 4)) == 0, "but not at 4");
    check(tables.challengeRewardStars(999999) == 0, "an unknown reward line pays nothing");

    proto::GetChallengeScRsp rsp = game::challenge::history();
    check(rsp.challenge_list.size() == tables.challenges().size(), "every floor is reported");
    check(rsp.challenge_list[0].star == 7, "with all three of its stars");
    check(rsp.max_level_list.size() == 3, "one max level per mode");
    check(rsp.serialize().size() + net::kPacketOverhead < net::kMaxKcpMessage,
          "the challenge history fits in one kcp message");
}

void testSceneRes() {
    const data::SceneRes& res = data::SceneRes::get();
    if (!res.loaded()) {
        std::printf("SKIP scene dump (res.json not configured)\n");
        return;
    }

    const data::ResFloor* floor = res.byEntry(2041101);
    check(floor != nullptr, "the Express parlour entry is in the dump");
    if (floor == nullptr) return;
    check(floor->planeId == 20411, "plane id parsed out of the key");
    check(floor->floorId == 20411001, "floor id parsed out of the key");
    check(!floor->groups.empty(), "the floor has groups");
    check(res.byFloor(20411001) != nullptr, "floor lookup goes through the entrance map");
    check(res.byEntry(1) == nullptr, "an unknown entry is not invented");

    // Half the entries have no teleport pad, so the entrance anchors are what put
    // the player somewhere sane when they arrive.
    const data::ResTeleport* express = res.anchor(1000002);
    check(express != nullptr, "the Express entrance has an anchor");
    check(express == nullptr || express->pos.z != 0, "and it is a real position");
    check(res.anchor(1) == nullptr, "an entry with no anchor gets none");
}

// A player wired up the way a session would have it, without any networking.
game::Player makeTestPlayer() {
    game::Player player(1337);
    player.setMainCharacter(8008);
    player.setMarchType(1224);
    game::LineupBook& book = player.lineups();
    book.replace(0, {8001, 1001}, 0);
    return player;
}

void testRosterProtos() {
    if (!data::Tables::get().loaded()) return;
    game::Player player = makeTestPlayer();
    game::Roster roster = player.roster();

    check(roster.resolvePath(8001) == 8008, "8001 resolves to the chosen path");
    check(roster.resolvePath(1001) == 1224, "1001 resolves to the chosen March");
    check(roster.resolvePath(1310) == 1310, "everyone else resolves to themselves");

    const std::vector<uint32_t>& ids = game::Roster::baseAvatarIds();
    check(!ids.empty(), "the roster has base ids");
    // Derived from AvatarConfig; the 4.5 + 4.6-beta merge comes to exactly 88 base ids.
    check(ids.size() == 88, "the derived roster has all 88 base ids");
    check(std::find(ids.begin(), ids.end(), 6022u) == ids.end(), "story-only units are excluded");
    check(std::find(ids.begin(), ids.end(), 1503u) != ids.end(), "beta-only avatars are included");
    check(ids.size() >= 2 && ids[0] == 8001 && ids[1] == 1001, "Trailblazer and March lead");
    check(std::find(ids.begin(), ids.end(), 8008u) == ids.end(), "paths are not listed as bases");
    check(std::find(ids.begin(), ids.end(), 1224u) == ids.end(), "March the Hunt is not a base");

    proto::Avatar avatar = roster.toAvatar(8001);
    check(avatar.base_avatar_id == 8001, "avatar keeps its base id");
    check(avatar.cur_multi_path_avatar_type == 8008, "and reports the active path");
    check(avatar.has_taken_promotion_reward_list.size() == avatar.promotion,
          "promotion rewards are all claimed");

    proto::Avatar other = roster.toAvatar(1310);
    check(other.cur_multi_path_avatar_type == 1310, "a single-path avatar reports itself");

    std::vector<proto::BattleBuff> buffs;
    proto::BattleAvatar battle = roster.toBattleAvatar(8001, 0, buffs);
    check(battle.id == 8008, "the battle avatar is the resolved path");
    check(battle.index == 0, "index carried through");
    check(battle.hp == 10000, "hp is a percentage in hundredths");
    check(battle.sp_bar && battle.sp_bar->max_sp >= 10000, "sp is scaled to hundredths");
    check(!battle.skilltree_list.empty(), "traces are filled in");
    check(!buffs.empty(), "a technique buff was produced");
    check(buffs[0].wave_flag == 0xFFFFFFFFu, "technique buffs apply to every wave");
    check(buffs[0].owner_index == 0, "owned by the avatar that brought it");
}

void testSceneBuild() {
    if (!data::SceneRes::get().loaded() || !data::Tables::get().loaded()) return;
    game::Player player = makeTestPlayer();

    proto::SceneInfo scene;
    check(!game::scene::load(player, 1, 0, false, scene), "an unknown entry fails cleanly");

    check(game::scene::load(player, 2041101, 0, true, scene), "the Express parlour loads");
    check(scene.entry_id == 2041101, "entry id");
    check(scene.plane_id == 20411, "plane id");
    check(scene.floor_id == 20411001, "floor id");
    check(!scene.entity_group_list.empty(), "the scene has entity groups");
    check(player.location().floorId == 20411001, "committing moved the player");

    // The party is group 0 and its entity ids are slot + 1.
    const proto::SceneEntityGroupInfo& actors = scene.entity_group_list.back();
    check(actors.group_id == 0, "the party is the last group");
    check(actors.entity_list.size() == 2, "both party members are in the world");
    check(actors.entity_list[0].entity_id == 1, "first actor is entity 1");
    check(actors.entity_list[0].actor && actors.entity_list[0].actor->base_avatar_id == 8001,
          "the actor carries the base id, not the path");
    check(scene.leader_entity_id == 1, "leader entity id points at the leader");

    // Entity id bands let a kind be told from the id alone.
    bool propsBanded = true;
    bool monstersBanded = true;
    size_t monsters = 0;
    for (const game::SceneEntity& entity : player.sceneState().entities()) {
        if (entity.kind == game::EntityKind::Prop && entity.entityId <= game::kPropEntityIdBase) {
            propsBanded = false;
        }
        if (entity.kind == game::EntityKind::Monster) {
            ++monsters;
            if (entity.entityId <= game::kMonsterEntityIdBase) monstersBanded = false;
        }
    }
    check(propsBanded, "props sit in their id band");
    check(monstersBanded, "monsters sit in their id band");
    check(player.sceneState().find(1) != nullptr, "the registry knows the actors");

    proto::SceneMapInfo map = game::scene::mapInfo(player, 20411001);
    check(map.floor_id == 20411001, "map info is for the right floor");
    check(map.chest_list.size() == 3, "the map legend has its chest counters");

    proto::LineupInfo lineup = game::scene::lineupInfo(player);
    check(lineup.avatar_list.size() == 2, "the lineup has both members");
    check(lineup.avatar_list[0].slot == 0 && lineup.avatar_list[1].slot == 1,
          "lineup slots are the packed indices");
    check(lineup.max_mp == game::kBaseMazeMp, "lineup carries the technique cap");
    (void)monsters;
}

// Every scene in the dump has to fit in one KCP message, or entering it silently
// drops the packet and the client is left staring at a loading screen.
void testChallengeRun() {
    if (!data::SceneRes::get().loaded() || !data::Tables::get().loaded()) return;
    const data::Tables& tables = data::Tables::get();

    // Memory of Chaos floor 1: one node, in scene group 2 of map entrance 3000101.
    const data::ChallengeInfo* config = tables.challenge(1);
    check(config != nullptr, "challenge 1 is loaded");
    if (config == nullptr) return;
    check(config->kind == data::ChallengeKind::Memory, "and it is a Memory of Chaos floor");
    check(config->mapEntranceId == 3000101, "with its own map entrance");
    check(config->roundLimit == 20, "and a cycle limit");
    check(config->mazeBuffId != 0, "and a maze buff");
    check(config->stages[0].mazeGroupId == 2, "its first node is scene group 2");
    check(config->stages[0].monsters.size() == 1, "holding one planted monster");

    // Pure Fiction and Apocalyptic Shadow both fight two nodes.
    const data::ChallengeInfo* fiction = tables.challenge(20011);
    check(fiction != nullptr && fiction->kind == data::ChallengeKind::Story, "20011 is Pure Fiction");
    check(fiction != nullptr && fiction->stageNum == 2, "and has two nodes");
    check(fiction != nullptr && !fiction->battleTargetIds.empty(),
          "with the score targets from its extra table");
    const data::ChallengeInfo* shadow = tables.challenge(30011);
    check(shadow != nullptr && shadow->kind == data::ChallengeKind::Boss,
          "30011 is Apocalyptic Shadow");
    check(shadow != nullptr && shadow->mapEntranceId2 != shadow->mapEntranceId,
          "whose second node has its own entrance");

    game::Player player = makeTestPlayer();
    game::ChallengeRun& run = player.challenge();
    run.active = true;
    run.challengeId = config->id;
    run.stage = 1;
    run.roundsLeft = config->roundLimit;
    run.party[0] = {8001, 1001};
    run.party[1] = {1002, 1003};

    proto::SceneInfo scene;
    check(game::challenge::enterArena(player, scene), "the arena loads");
    check(player.location().entryId == config->mapEntranceId, "and the player is standing in it");

    // Only the node's own monster is there, and it is the one the maze config names --
    // not the placeholder the scene dump carries.
    size_t monsters = 0;
    size_t npcs = 0;
    for (const proto::SceneEntityGroupInfo& group : scene.entity_group_list) {
        for (const proto::SceneEntityInfo& entity : group.entity_list) {
            if (entity.npc_monster) {
                ++monsters;
                check(entity.npc_monster->monster_id == config->stages[0].monsters[0].npcMonsterId,
                      "the planted monster replaces the dump's placeholder");
                check(entity.group_id == config->stages[0].mazeGroupId,
                      "and stands in the node's own group");
            }
            if (entity.npc) ++npcs;
        }
    }
    check(monsters == 1, "exactly one monster is in the arena");
    check(npcs == 0, "and nobody is standing around");

    // A challenge event is its own stage id -- PlaneEvent maps it to itself, so the
    // entity can carry it directly and skip the world-level lookup entirely.
    uint32_t eventId = config->stages[0].monsters[0].eventId;
    check(tables.stageForEvent(eventId, player.worldLevel()) == eventId,
          "a challenge event maps to itself");
    const game::SceneEntity* planted = nullptr;
    for (const game::SceneEntity& entity : player.sceneState().entities()) {
        if (entity.kind == game::EntityKind::Monster) planted = &entity;
    }
    check(planted != nullptr && planted->stageId == eventId, "so the entity carries the stage id");
    check(tables.stage(eventId) != nullptr, "and that stage is in the tables");

    // Stars: cycles left and nobody dead, both met on a fresh run.
    check(game::challenge::stars(player) == 7, "an untouched run has all three stars");
    run.roundsLeft = 1;
    run.deadAvatars = 2;
    check(game::challenge::stars(player) == 0, "a slow run with deaths has none");

    // Score targets are Pure Fiction's, so a Memory floor ignores them entirely.
    run.challengeId = fiction->id;
    run.roundsLeft = 0;
    run.score[0] = 30000;
    run.score[1] = 30000;
    check(game::challenge::stars(player) == 7, "60000 points clears all three score targets");
    run.score[1] = 0;
    check(game::challenge::stars(player) == 0, "30000 clears none of them");
}

void testEveryScenePacketFits() {
    if (!data::SceneRes::get().loaded() || !data::Tables::get().loaded()) return;

    game::Player player = makeTestPlayer();
    size_t worst = 0;
    uint32_t worstEntry = 0;
    size_t failures = 0;
    size_t built = 0;

    for (const data::ResFloor& floor : data::SceneRes::get().floors()) {
        proto::SceneInfo scene;
        if (!game::scene::load(player, floor.entryId, 0, false, scene)) continue;
        ++built;

        proto::EnterSceneByServerScNotify notify;
        notify.scene = std::move(scene);
        notify.lineup = game::scene::lineupInfo(player);
        size_t size = notify.serialize().size() + net::kPacketOverhead;
        if (size > worst) {
            worst = size;
            worstEntry = floor.entryId;
        }
        if (size > net::kMaxKcpMessage) ++failures;
    }

    check(built > 100, "the dump produced scenes to measure");
    check(failures == 0, "every scene fits in one kcp message");
    if (failures != 0) {
        std::printf("      largest scene is entry %u at %zu bytes (limit %zu)\n", worstEntry, worst,
                    net::kMaxKcpMessage);
    }

    // The bag is the other packet that grows with the player's data.
    game::Roster roster = player.roster();
    proto::GetBagScRsp bag;
    for (const game::Lightcone& lightcone : roster.data().lightcones) {
        bag.equipment_list.push_back(roster.toEquipment(lightcone));
    }
    for (const game::Relic& relic : roster.data().relics) {
        bag.relic_list.push_back(roster.toRelic(relic));
    }
    check(bag.serialize().size() + net::kPacketOverhead < net::kMaxKcpMessage,
          "the bag fits in one kcp message");
}

void testBattleBuild() {
    if (!data::SceneRes::get().loaded() || !data::Tables::get().loaded()) return;

    game::Player player = makeTestPlayer();
    proto::SceneInfo scene;
    if (!game::scene::load(player, 1000002, 0, true, scene)) {
        std::printf("SKIP battle build (entry 1000002 missing from the dump)\n");
        return;
    }

    // Find a monster the dump placed in this scene and swing at it.
    uint32_t monsterEntity = 0;
    for (const game::SceneEntity& entity : player.sceneState().entities()) {
        if (entity.kind == game::EntityKind::Monster && entity.eventId != 0) {
            monsterEntity = entity.entityId;
            break;
        }
    }
    check(monsterEntity != 0, "the scene has a monster to fight");
    if (monsterEntity == 0) return;

    game::BattleRequest request;
    request.casterEntityId = 1;
    request.skillIndex = 0;
    request.monsterEntityIds = {monsterEntity};
    request.allowSrToolsOverride = false;

    proto::SceneBattleInfo info = game::battle::create(player, request);
    check(info.battle_id != 0, "the fight got an id");
    check(info.stage_id != 0, "the monster's event resolved to a stage");
    check(info.battle_avatar_list.size() == 2, "the whole party is in the fight");
    check(!info.monster_wave_list.empty(), "the fight has monsters");
    check(info.monster_wave_list[0].battle_wave_id == 1, "waves are numbered from one");
    check(info.world_level == player.worldLevel(), "world level carried through");
    check(player.battle().active, "the fight is recorded on the player");
    check(player.battle().battleId == info.battle_id, "and by its id");

    // A basic attack gives the leader the element entry buff at SkillIndex 1.
    bool entryBuff = false;
    for (const proto::BattleBuff& buff : info.buff_list) {
        if (buff.id < 1000111 || buff.id > 1000117) continue;
        entryBuff = true;
        check(buff.owner_index == 0, "the entry buff belongs to whoever swung");
        auto it = buff.dynamic_values.find("SkillIndex");
        check(it != buff.dynamic_values.end() && it->second == 1.0f,
              "a basic attack is SkillIndex 1");
    }
    check(entryBuff, "the swing produced an element entry buff");

    bool ambush = false;
    for (const proto::BattleBuff& buff : info.buff_list) {
        if (buff.id == 1000102) ambush = true;
    }
    check(!ambush, "no ambush buff when the player struck first");

    // Being attacked adds the ambush buff, and only to the first wave.
    request.attackedByEntityId = monsterEntity;
    request.skillIndex = 1;
    proto::SceneBattleInfo ambushed = game::battle::create(player, request);
    const proto::BattleBuff* ambushBuff = nullptr;
    for (const proto::BattleBuff& buff : ambushed.buff_list) {
        if (buff.id == 1000102) ambushBuff = &buff;
    }
    check(ambushBuff != nullptr, "an ambush adds its buff");
    check(ambushBuff == nullptr || ambushBuff->wave_flag == 1, "ambush only hits wave one");
    check(ambushed.battle_id != info.battle_id, "each fight gets a fresh id");

    for (const proto::BattleBuff& buff : ambushed.buff_list) {
        if (buff.id < 1000111 || buff.id > 1000117) continue;
        auto it = buff.dynamic_values.find("SkillIndex");
        check(it != buff.dynamic_values.end() && it->second == 2.0f,
              "a technique is SkillIndex 2");
    }

    // Global maze buffs ride along unless they are switched off.
    bool castorice = false;
    for (const proto::BattleBuff& buff : ambushed.buff_list) {
        if (buff.id == 140703) castorice = true;
    }
    check(castorice, "the global maze buffs are added");

    player.setGlobalBuffs(false);
    proto::SceneBattleInfo plain = game::battle::create(player, request);
    bool stillThere = false;
    for (const proto::BattleBuff& buff : plain.buff_list) {
        if (buff.id == 140703) stillThere = true;
    }
    check(!stillThere, "and can be switched off");
}

// Writing an equip change back must not drop the parts of the build we do not
// model -- the loadout presets are the user's, not ours.
void testSrToolsRoundTrip() {
    const std::string original = core::Config::get().paths.srtoolsFile;
    bool ok = false;
    std::string source = util::readFile(original, &ok);
    if (!ok) {
        std::printf("SKIP srtools round trip (no build on disk)\n");
        return;
    }

    const std::string scratch = "build/test-srtools.json";
    check(util::writeFile(scratch, source), "copied the build to a scratch file");
    core::Config::get().paths.srtoolsFile = scratch;
    check(game::SrTools::instance().loadFromDisk(), "scratch build loads");

    uint32_t movedItemId = 0;
    game::SrTools::instance().mutate([&](game::SrToolsData& data) {
        if (!data.lightcones.empty()) {
            data.lightcones.front().equipAvatar = 1310;
            movedItemId = data.lightcones.front().itemId;
        }
    });

    std::string written = util::readFile(scratch, &ok);
    check(ok, "the build was written back");
    if (ok) {
        nlohmann::json before = nlohmann::json::parse(source);
        nlohmann::json after = nlohmann::json::parse(written);
        for (const auto& entry : before.items()) {
            if (entry.key() == "avatars" || entry.key() == "lightcones" ||
                entry.key() == "relics") {
                continue;
            }
            check(after.contains(entry.key()), "unmodelled fields survive a write");
            if (after.contains(entry.key())) {
                check(after[entry.key()] == entry.value(), "and survive unchanged");
            }
        }
        check(after["lightcones"].size() == before["lightcones"].size(), "no lightcones lost");
        check(after["relics"].size() == before["relics"].size(), "no relics lost");
        check(after["avatars"].size() == before["avatars"].size(), "no avatars lost");
        check(after["lightcones"][0]["equip_avatar"] == 1310, "the equip change was written");
        check(after["lightcones"][0]["item_id"] == movedItemId, "on the right lightcone");
    }

    // A reload of the written file has to produce the same thing we just wrote.
    check(game::SrTools::instance().loadFromDisk(), "the written build reloads");
    auto reloaded = game::SrTools::instance().data();
    check(!reloaded->lightcones.empty() && reloaded->lightcones.front().equipAvatar == 1310,
          "and the change survived the round trip");
    check(!reloaded->avatars.empty(), "avatars survived the round trip");
    if (auto march = reloaded->avatars.find(1001); march != reloaded->avatars.end()) {
        check(!march->second.skills.empty(), "traces survived the round trip");
        check(!march->second.skillsByAnchor.empty(), "trace anchors survived too");
    }

    core::Config::get().paths.srtoolsFile = original;
    game::SrTools::instance().loadFromDisk();
}

// Launching build\bin\capysr.exe directly used to start a server with no config, no
// tables and no scenes, and every symptom looked like a broken server.
void testRootDirLookup() {
    check(!util::executableDir().empty(), "the executable knows where it lives");

    std::string root = util::findRootDir("config/config.json");
    check(!root.empty(), "the repo root is found from the test binary");
    check(util::fileExists(root + "/config/config.json"), "and it really holds the config");
    check(util::findRootDir("no/such/marker.json").empty(), "a bogus marker finds nothing");

    // The working directory wins, so a deliberate cd still decides.
    check(util::findRootDir("config/config.json") == root, "lookup is stable");
}

void testPropInteraction() {
    if (!data::Tables::get().loaded()) return;

    game::Player player = makeTestPlayer();
    game::SceneEntity prop;
    prop.entityId = 1001;
    prop.kind = game::EntityKind::Prop;
    prop.state = 12;  // ChestClosed
    player.sceneState().add(prop);

    game::SceneEntity* stored = player.sceneState().find(1001u);
    check(stored != nullptr, "the prop is in the registry");

    // InteractConfig 1010 opens a prop that is Closed; a chest is not, so it stays.
    const data::InteractInfo* open = data::Tables::get().interact(1010);
    check(open != nullptr, "InteractConfig 1010 is loaded");
    check(open == nullptr || open->targetState == 1, "and it opens things");

    check(data::propStateFromName("ChestUsed") == 13, "prop state names map to numbers");
    check(data::propStateFromName("Closed") == 0, "Closed is zero");
    check(data::propStateFromName("Nonsense") == 0, "an unknown state reads as Closed");
}

}  // namespace

void runGameTests() {
    // The tests read the same config the server does, so they exercise the real paths.
    // Find the root the same way main() does, so the suite runs from anywhere.
    testRootDirLookup();
    std::string root = util::findRootDir("config/config.json");
    if (!root.empty()) util::setCurrentDir(root);
    core::Config::get().load("config/config.json");
    data::Tables::get().load(core::Config::get().paths.dataSources);
    data::SceneRes::get().load(core::Config::get().paths.sceneRes);
    data::SceneRes::get().loadAnchors(core::Config::get().paths.anchors);
    game::SrTools::instance().loadFromDisk();

    testLineupPacking();
    testSquadSwitching();
    testMazeMpCap();
    testItemUniqueIds();
    testElementBuffs();
    testTables();
    testChallengeHistory();
    testSceneRes();
    testRosterProtos();
    testSceneBuild();
    testChallengeRun();
    testEveryScenePacketFits();
    testBattleBuild();
    testPropInteraction();
    testSrToolsRoundTrip();
}
