#include "game/scene.h"

#include <algorithm>

#include "core/logger.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/player.h"

namespace game {
namespace {

// World 100 is the Express interior, which the client only accepts as 501.
uint32_t fixWorldId(uint32_t worldId) { return worldId == 100 ? 501u : worldId; }

proto::Vector toVector(const data::ResVector& v) {
    proto::Vector out;
    out.x = v.x;
    out.y = v.y;
    out.z = v.z;
    return out;
}

proto::MotionInfo motionOf(const data::ResVector& pos, const data::ResVector& rot) {
    proto::MotionInfo out;
    out.pos.emplace() = toVector(pos);
    out.rot.emplace() = toVector(rot);
    return out;
}

}  // namespace

void SceneState::reset() {
    entities_.clear();
    propCursor_ = kPropEntityIdBase;
    npcCursor_ = kNpcEntityIdBase;
    monsterCursor_ = kMonsterEntityIdBase;
}

void SceneState::add(const SceneEntity& entity) { entities_.push_back(entity); }

void SceneState::remove(uint32_t entityId) {
    entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                   [&](const SceneEntity& e) { return e.entityId == entityId; }),
                    entities_.end());
}

void SceneState::removeKind(EntityKind kind) {
    entities_.erase(
        std::remove_if(entities_.begin(), entities_.end(),
                       [&](const SceneEntity& e) { return e.kind == kind; }),
        entities_.end());
}

const SceneEntity* SceneState::find(uint32_t entityId) const {
    for (const SceneEntity& entity : entities_) {
        if (entity.entityId == entityId) return &entity;
    }
    return nullptr;
}

SceneEntity* SceneState::find(uint32_t entityId) {
    for (SceneEntity& entity : entities_) {
        if (entity.entityId == entityId) return &entity;
    }
    return nullptr;
}

proto::MotionInfo toMotion(const Position& position) {
    proto::MotionInfo out;
    auto& pos = out.pos.emplace();
    pos.x = position.x;
    pos.y = position.y;
    pos.z = position.z;
    // Only the yaw matters; the client ignores pitch and roll for the party.
    out.rot.emplace().y = position.rotY;
    return out;
}

namespace scene {

proto::LineupInfo lineupInfo(const Player& player, uint32_t index) {
    const LineupBook& book = player.lineups();
    const Squad& squad = book.squad(index);
    Roster roster = player.roster();

    proto::LineupInfo info;
    info.name = squad.name;
    info.index = index;
    info._is_favourite = squad.favourite;
    info.mp = book.mp();
    info.max_mp = book.maxMp();
    info.leader_slot = book.leaderSlot(index);
    info.extra_lineup_type = proto::ExtraLineupType::ExtraLineupType_LineupNone;

    uint32_t slot = 0;
    for (uint32_t avatarId : book.members(index)) {
        info.avatar_list.push_back(roster.toLineupAvatar(avatarId, slot++));
    }
    return info;
}

proto::LineupInfo lineupInfo(const Player& player) {
    return lineupInfo(player, player.lineups().curIndex());
}

proto::SceneEntityGroupInfo actorGroup(Player& player, const Position& at) {
    proto::MotionInfo motion = toMotion(at);

    proto::SceneEntityGroupInfo group;
    group.group_id = 0;

    uint32_t slot = 0;
    for (uint32_t avatarId : player.lineups().curMembers()) {
        SceneEntity entity;
        entity.entityId = slot + 1;
        entity.kind = EntityKind::Actor;
        entity.avatarId = avatarId;
        player.sceneState().add(entity);

        proto::SceneEntityInfo info;
        info.entity_id = entity.entityId;
        info.motion = motion;
        auto& actor = info.actor.emplace();
        actor.avatar_type = proto::AvatarType::AvatarType_AvatarFormalType;
        // The base id, not the path: the client picks the model from the
        // cur_multi_path_avatar_type it already got with GetAvatarData.
        actor.base_avatar_id = avatarId;
        actor.uid = player.uid();
        group.entity_list.push_back(std::move(info));
        ++slot;
    }
    return group;
}

bool load(Player& player, uint32_t entryId, uint32_t teleportId, bool commit,
          proto::SceneInfo& out, const ChallengeArena* arena) {
    const data::ResFloor* floor = data::SceneRes::get().byEntry(entryId);
    if (floor == nullptr) {
        logging::warn("scene", "entry {} is not in the scene dump", entryId);
        return false;
    }

    // Entering a scene lands on an anchor; re-entering the one you are standing in
    // keeps your coordinates. The teleport the client named wins, then the
    // entrance's own anchor, then any pad on the floor.
    Position spawn = player.position();
    const data::ResTeleport* anchor = teleportId != 0 ? floor->teleport(teleportId) : nullptr;
    if (anchor == nullptr && floor->floorId != player.location().floorId) {
        const data::EntranceInfo* entrance = data::Tables::get().entrance(entryId);
        uint32_t anchorId = entrance != nullptr ? entrance->startAnchorId : 0;
        anchor = data::SceneRes::get().anchor(entryId, anchorId);
        if (anchor == nullptr) anchor = floor->anyTeleport();
    }
    if (anchor != nullptr) {
        spawn.x = anchor->pos.x;
        spawn.y = anchor->pos.y;
        spawn.z = anchor->pos.z;
        spawn.rotY = anchor->rot.y;
    }
    if (commit) {
        player.position() = spawn;
        player.location().entryId = floor->entryId;
        player.location().planeId = floor->planeId;
        player.location().floorId = floor->floorId;
    }

    proto::SceneInfo scene;
    scene.entry_id = floor->entryId;
    scene.plane_id = floor->planeId;
    scene.floor_id = floor->floorId;
    scene.game_mode_type = floor->planeType;
    scene.world_id = fixWorldId(floor->worldId);
    scene.lighten_section_list = floor->sections;
    scene.leader_entity_id = player.lineups().leaderSlot(player.lineups().curIndex()) + 1;
    scene.scene_identifier.emplace().floor_id = floor->floorId;
    for (const auto& [name, value] : floor->savedValues) scene.floor_saved_data[name] = value;

    auto& missions = scene.scene_mission_info.emplace();

    // The registry is rebuilt from scratch: ids from the old scene are meaningless.
    SceneState& state = player.sceneState();
    state.reset();

    std::vector<uint32_t> loadedNpcs;
    std::vector<uint32_t> party = player.lineups().curMembers();

    for (const data::ResGroup& group : floor->groups) {
        proto::SceneEntityGroupInfo groupInfo;
        groupInfo.group_id = group.groupId;

        for (const data::ResProp& prop : group.props) {
            SceneEntity entity;
            entity.entityId = state.nextPropEntityId();
            entity.instId = prop.instId;
            entity.groupId = prop.groupId;
            entity.kind = EntityKind::Prop;
            entity.configId = prop.propId;
            entity.state = prop.propState;
            state.add(entity);

            proto::SceneEntityInfo info;
            info.entity_id = entity.entityId;
            info.inst_id = prop.instId;
            info.group_id = prop.groupId;
            info.motion = motionOf(prop.pos, prop.rot);
            auto& propInfo = info.prop.emplace();
            propInfo.prop_id = prop.propId;
            propInfo.prop_state = prop.propState;
            groupInfo.entity_list.push_back(std::move(info));
        }

        for (const data::ResNpc& npc : group.npcs) {
            // Nobody is standing around in an arena.
            if (arena != nullptr) break;
            // A party member also standing around as an npc would show up twice, and
            // the same npc id is often placed in several groups of one floor.
            if (std::find(party.begin(), party.end(), npc.npcId) != party.end()) continue;
            if (std::find(loadedNpcs.begin(), loadedNpcs.end(), npc.npcId) != loadedNpcs.end()) {
                continue;
            }
            loadedNpcs.push_back(npc.npcId);

            SceneEntity entity;
            entity.entityId = state.nextNpcEntityId();
            entity.instId = npc.instId;
            entity.groupId = npc.groupId;
            entity.kind = EntityKind::Npc;
            entity.configId = npc.npcId;
            state.add(entity);

            proto::SceneEntityInfo info;
            info.entity_id = entity.entityId;
            info.inst_id = npc.instId;
            info.group_id = npc.groupId;
            info.motion = motionOf(npc.pos, npc.rot);
            info.npc.emplace().npc_id = npc.npcId;
            groupInfo.entity_list.push_back(std::move(info));
        }

        for (const data::ResMonster& monster : group.monsters) {
            // In an arena the dump's monster is only a marker: the maze config decides
            // which of them are there at all, and what each one really is.
            const data::ChallengeMonster* planted = nullptr;
            if (arena != nullptr) {
                if (group.groupId != arena->mazeGroupId || arena->monsters == nullptr) continue;
                for (const data::ChallengeMonster& candidate : *arena->monsters) {
                    if (candidate.configId == monster.instId) planted = &candidate;
                }
                if (planted == nullptr) continue;
            }

            SceneEntity entity;
            entity.entityId = state.nextMonsterEntityId();
            entity.instId = monster.instId;
            entity.groupId = monster.groupId;
            entity.kind = EntityKind::Monster;
            entity.configId = planted != nullptr ? planted->npcMonsterId : monster.monsterId;
            entity.eventId = planted != nullptr ? planted->eventId : monster.eventId;
            // Challenge events are not in PlaneEvent, so the event id is the stage id.
            entity.stageId = planted != nullptr ? planted->eventId : 0;
            state.add(entity);

            proto::SceneEntityInfo info;
            info.entity_id = entity.entityId;
            info.inst_id = monster.instId;
            info.group_id = monster.groupId;
            info.motion = motionOf(monster.pos, monster.rot);
            auto& npcMonster = info.npc_monster.emplace();
            npcMonster.monster_id = entity.configId;
            npcMonster.event_id = entity.eventId;
            npcMonster.world_level = player.worldLevel();
            groupInfo.entity_list.push_back(std::move(info));
        }

        for (uint32_t chest : group.chests) scene.opened_chests_list.push_back(chest);
        for (uint32_t mission : group.finishedMainMissions) {
            missions.finished_main_mission_id_list.push_back(mission);
        }
        for (uint32_t mission : group.finishedSubMissions) {
            proto::Mission entry;
            entry.id = mission;
            entry.status = proto::MissionStatus::MissionStatus_MissionFinish;
            missions.sub_mission_status_list.push_back(entry);
        }

        // An empty group still costs the client a load, so skip the ones that hold
        // nothing but anchors.
        if (groupInfo.entity_list.empty()) continue;
        scene.entity_group_list.push_back(std::move(groupInfo));
    }

    // The party is group 0; its entity ids are slot + 1, which is what
    // SceneCastSkill and leader_entity_id refer to.
    scene.entity_group_list.push_back(actorGroup(player, spawn));

    out = std::move(scene);
    return true;
}

proto::SceneMapInfo mapInfo(const Player& player, uint32_t floorId) {
    proto::SceneMapInfo info;
    info.floor_id = floorId;
    info.scene_identifier.emplace().floor_id = floorId;
    // Chest counters the map screen expects; without them the legend is blank.
    for (proto::ChestType type : {proto::ChestType::MAP_INFO_CHEST_TYPE_NORMAL,
                                  proto::ChestType::MAP_INFO_CHEST_TYPE_CHALLENGE,
                                  proto::ChestType::MAP_INFO_CHEST_TYPE_PUZZLE}) {
        proto::ChestInfo chest;
        chest.chest_type = type;
        info.chest_list.push_back(chest);
    }

    const data::ResFloor* floor = data::SceneRes::get().byFloor(floorId);
    if (floor == nullptr) return info;

    info.entry_id = floor->entryId;
    info.cur_map_entry_id = player.location().entryId;
    info.lighten_section_list = floor->sections;
    for (const auto& [name, value] : floor->savedValues) info.floor_saved_value_map[name] = value;

    for (const data::ResGroup& group : floor->groups) {
        proto::MazeGroup mazeGroup;
        mazeGroup.group_id = group.groupId;
        info.maze_group_list.push_back(std::move(mazeGroup));

        for (const data::ResTeleport& teleport : group.teleports) {
            info.unlock_teleport_list.push_back(teleport.id);
        }
        for (const data::ResProp& prop : group.props) {
            proto::MazePropState state;
            state.group_id = prop.groupId;
            state.config_id = prop.instId;
            state.state = prop.propState;
            info.maze_prop_list.push_back(state);
        }
        for (uint32_t chest : group.chests) info.opened_chest_id_list.push_back(chest);
    }
    return info;
}

}  // namespace scene
}  // namespace game
