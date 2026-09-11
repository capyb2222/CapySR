#include "game/notify.h"

#include <algorithm>

#include "game/player.h"
#include "game/scene.h"
#include "net/cmd_ids.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game {
namespace notify {

void lineupChanged(net::Session& session, Player& player) {
    // Rebuild the actor entities: the party changed, so the models in the world did.
    player.sceneState().removeKind(EntityKind::Actor);
    proto::SceneEntityGroupInfo actors = scene::actorGroup(player, player.position());

    proto::SceneGroupRefreshScNotify refresh;
    refresh.floor_id = player.location().floorId;
    auto& group = refresh.group_refresh_list.emplace_back();
    group.group_id = 0;
    group.refresh_type = proto::SceneGroupRefreshType::SCENE_GROUP_REFRESH_TYPE_LOADED;
    for (const proto::SceneEntityInfo& entity : actors.entity_list) {
        auto& change = group.refresh_entity.emplace_back();
        change.add_entity = entity;
        change.GDAKHEKABFD_case = proto::SceneEntityRefreshInfo::k_add_entity;
    }
    session.send(cmd::SceneGroupRefreshScNotify, refresh);

    proto::SyncLineupNotify sync;
    sync.lineup = scene::lineupInfo(player);
    session.send(cmd::SyncLineupNotify, sync);
}

void avatarChanged(net::Session& session, Player& player, uint32_t baseAvatarId,
                   const std::vector<uint32_t>& touchedEquipmentIds,
                   const std::vector<uint32_t>& touchedRelicIds) {
    Roster roster = player.roster();
    proto::PlayerSyncScNotify sync;

    auto& avatars = sync.avatar_sync.emplace();
    if (baseAvatarId != 0) {
        avatars.avatar_list.push_back(roster.toAvatar(baseAvatarId));
        uint32_t pathId = roster.resolvePath(baseAvatarId);
        if (const Avatar* avatar = roster.find(pathId)) {
            avatars.avatar_path_data_info_list.push_back(roster.toPathData(*avatar));
        }
    }

    for (const Lightcone& lightcone : roster.data().lightcones) {
        uint32_t uniqueId = equipmentUniqueId(lightcone.internalUid);
        if (std::find(touchedEquipmentIds.begin(), touchedEquipmentIds.end(), uniqueId) !=
            touchedEquipmentIds.end()) {
            sync.equipment_list.push_back(roster.toEquipment(lightcone));
        }
    }
    for (const Relic& relic : roster.data().relics) {
        uint32_t uniqueId = relicUniqueId(relic.internalUid);
        if (std::find(touchedRelicIds.begin(), touchedRelicIds.end(), uniqueId) !=
            touchedRelicIds.end()) {
            sync.relic_list.push_back(roster.toRelic(relic));
        }
    }

    session.send(cmd::PlayerSyncScNotify, sync);
}

void monstersRemoved(net::Session& session, Player& player,
                     const std::vector<uint32_t>& entityIds) {
    if (entityIds.empty()) return;

    proto::SceneGroupRefreshScNotify refresh;
    refresh.floor_id = player.location().floorId;
    // Grouped by the scene group each monster belonged to, which is how the client
    // matches the delete against what it has loaded.
    std::vector<uint32_t> groups;
    for (uint32_t entityId : entityIds) {
        const SceneEntity* entity = player.sceneState().find(entityId);
        if (entity == nullptr || entity->kind != EntityKind::Monster) continue;
        if (std::find(groups.begin(), groups.end(), entity->groupId) == groups.end()) {
            groups.push_back(entity->groupId);
        }
    }
    for (uint32_t groupId : groups) {
        auto& group = refresh.group_refresh_list.emplace_back();
        group.group_id = groupId;
        group.refresh_type = proto::SceneGroupRefreshType::SCENE_GROUP_REFRESH_TYPE_LOADED;
        for (uint32_t entityId : entityIds) {
            const SceneEntity* entity = player.sceneState().find(entityId);
            if (entity == nullptr || entity->groupId != groupId) continue;
            auto& change = group.refresh_entity.emplace_back();
            change.delete_entity = entityId;
            change.GDAKHEKABFD_case = proto::SceneEntityRefreshInfo::k_delete_entity;
        }
    }
    if (refresh.group_refresh_list.empty()) return;

    session.send(cmd::SceneGroupRefreshScNotify, refresh);
    for (uint32_t entityId : entityIds) player.sceneState().remove(entityId);
}

}  // namespace notify
}  // namespace game
