#include "game/roster.h"

#include <algorithm>
#include <mutex>

#include "core/config.h"
#include "data/excel.h"

namespace game {
namespace {

// Somewhere in the past; the client only uses it to sort the character list.
constexpr uint64_t kFirstMetTimestamp = 1712924677;

// A relic's slot is the last digit of its id.
uint32_t relicSlot(uint32_t relicId) { return relicId % 10; }

}  // namespace

uint32_t relicUniqueId(uint32_t internalUid) { return internalUid + 1; }

uint32_t equipmentUniqueId(uint32_t internalUid) {
    return kEquipmentUidBase + internalUid + 1;
}

uint32_t relicInternalUid(uint32_t uniqueId) { return uniqueId == 0 ? 0 : uniqueId - 1; }

uint32_t equipmentInternalUid(uint32_t uniqueId) {
    return uniqueId <= kEquipmentUidBase ? 0 : uniqueId - kEquipmentUidBase - 1;
}

Roster::Roster(std::shared_ptr<const SrToolsData> data, uint32_t mainCharacter, uint32_t marchType)
    : data_(std::move(data)), mainCharacter_(mainCharacter), marchType_(marchType) {}

const std::vector<uint32_t>& Roster::baseAvatarIds() {
    static std::vector<uint32_t> ids;
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    // Built on the first call after the tables are loaded, and never again; an empty
    // result means the tables were not ready, so it is not cached.
    if (!ids.empty()) return ids;

    const data::Tables& tables = data::Tables::get();
    for (const auto& [id, info] : tables.avatars()) {
        (void)info;
        uint32_t base = tables.baseAvatarId(id);
        // Trial and story-only rows live outside the playable ranges.
        if (base < 1001 || (base > 1999 && base != 8001)) continue;
        if (std::find(ids.begin(), ids.end(), base) == ids.end()) ids.push_back(base);
    }
    // The Trailblazer and March lead the roster, then everyone in id order.
    std::sort(ids.begin(), ids.end());
    auto pull = [](std::vector<uint32_t>& v, uint32_t id, size_t to) {
        auto it = std::find(v.begin(), v.end(), id);
        if (it == v.end() || static_cast<size_t>(it - v.begin()) == to) return;
        v.erase(it);
        v.insert(v.begin() + static_cast<long>(std::min(to, v.size())), id);
    };
    pull(ids, 8001, 0);
    pull(ids, 1001, 1);
    return ids;
}

uint32_t Roster::resolvePath(uint32_t baseId) const {
    if (baseId == 8001) return mainCharacter_ != 0 ? mainCharacter_ : 8001;
    if (baseId == 1001) return marchType_ != 0 ? marchType_ : 1001;
    return baseId;
}

const Avatar* Roster::find(uint32_t avatarId) const {
    auto it = data_->avatars.find(avatarId);
    return it == data_->avatars.end() ? nullptr : &it->second;
}

const Lightcone* Roster::lightconeOf(uint32_t avatarId) const {
    for (const Lightcone& lc : data_->lightcones) {
        if (lc.equipAvatar == avatarId) return &lc;
    }
    return nullptr;
}

std::vector<const Relic*> Roster::relicsOf(uint32_t avatarId) const {
    std::vector<const Relic*> out;
    if (avatarId == 0) return out;
    for (const Relic& relic : data_->relics) {
        if (relic.equipAvatar == avatarId) out.push_back(&relic);
    }
    return out;
}

proto::Avatar Roster::toAvatar(uint32_t baseId) const {
    const data::Tables& tables = data::Tables::get();
    proto::Avatar out;
    out.base_avatar_id = baseId;
    out.first_met_time_stamp = kFirstMetTimestamp;
    // Only the Trailblazer and March carry a path; everyone else reports themselves.
    uint32_t pathId = resolvePath(baseId);
    out.cur_multi_path_avatar_type = tables.multiPathVariants(baseId) != nullptr ? pathId : baseId;

    const Avatar* src = find(pathId);
    if (src != nullptr) {
        out.level = src->level;
        out.promotion = src->promotion;
    } else {
        // Not in the srtools build: still show it, maxed, so it can be used.
        out.level = 80;
        out.promotion = 6;
    }
    if (const Lightcone* lc = lightconeOf(pathId)) {
        out.equipment_unique_id = equipmentUniqueId(lc->internalUid);
    }
    for (uint32_t step = 1; step <= out.promotion; ++step) {
        out.has_taken_promotion_reward_list.push_back(step);
    }
    return out;
}

proto::AvatarPathData Roster::toPathData(const Avatar& avatar) const {
    proto::AvatarPathData out;
    out.avatar_id = avatar.avatarId;
    out.rank = avatar.rank;
    out.unk_enhanced_id = avatar.enhancedId;
    if (const Lightcone* lc = lightconeOf(avatar.avatarId)) {
        out.path_equipment_id = equipmentUniqueId(lc->internalUid);
    }
    // The client keys traces by anchor type here, not by point id.
    for (const auto& [anchor, level] : avatar.skillsByAnchor) {
        proto::AvatarPathSkillTree point;
        point.point_id = anchor;
        point.level = level;
        out.avatar_path_skill_tree.push_back(point);
    }
    for (const Relic* relic : relicsOf(avatar.avatarId)) {
        proto::EquipRelic equip;
        equip.type = relicSlot(relic->relicId);
        equip.relic_unique_id = relicUniqueId(relic->internalUid);
        out.equip_relic_list.push_back(equip);
    }
    return out;
}

proto::LineupAvatar Roster::toLineupAvatar(uint32_t baseId, uint32_t slot) const {
    proto::LineupAvatar out;
    out.id = baseId;
    out.slot = slot;
    out.hp = kFullHp;
    out.satiety = 100;
    out.avatar_type = proto::AvatarType::AvatarType_AvatarFormalType;

    uint32_t pathId = resolvePath(baseId);
    auto& bar = out.sp_bar.emplace();
    if (const Avatar* src = find(pathId)) {
        bar.cur_sp = src->spValue * 100;
        bar.max_sp = src->spMax * 100;
    } else if (const data::AvatarInfo* info = data::Tables::get().avatar(pathId)) {
        bar.cur_sp = info->spNeed / 2;
        bar.max_sp = info->spNeed;
    } else {
        bar.cur_sp = kFullHp / 2;
        bar.max_sp = kFullHp;
    }
    return out;
}

proto::Equipment Roster::toEquipment(const Lightcone& lightcone) const {
    proto::Equipment out;
    out.unique_id = equipmentUniqueId(lightcone.internalUid);
    out.tid = lightcone.itemId;
    out.level = lightcone.level;
    out.promotion = lightcone.promotion;
    out.rank = lightcone.rank;
    out.dress_avatar_id = lightcone.equipAvatar;
    return out;
}

proto::Relic Roster::toRelic(const Relic& relic) const {
    proto::Relic out;
    out.unique_id = relicUniqueId(relic.internalUid);
    out.tid = relic.relicId;
    out.level = relic.level;
    out.main_affix_id = relic.mainAffixId;
    out.dress_avatar_id = relic.equipAvatar;
    for (const SubAffix& affix : relic.subAffixes) {
        proto::RelicAffix sub;
        sub.affix_id = affix.id;
        sub.cnt = affix.count;
        sub.step = affix.step;
        out.sub_affix_list.push_back(sub);
    }
    return out;
}

proto::BattleAvatar Roster::toBattleAvatar(uint32_t baseId, uint32_t index,
                                           std::vector<proto::BattleBuff>& buffs) const {
    const data::Tables& tables = data::Tables::get();
    uint32_t pathId = resolvePath(baseId);
    const data::AvatarInfo* info = tables.avatar(pathId);
    const Avatar* src = find(pathId);

    proto::BattleAvatar out;
    out.index = index;
    out.id = pathId;
    out.avatar_type = proto::AvatarType::AvatarType_AvatarFormalType;
    out.hp = kFullHp;
    out.world_level = core::Config::get().player.worldLevel;
    out.level = src != nullptr ? src->level : 80;
    out.promotion = src != nullptr ? src->promotion : (info != nullptr ? info->maxPromotion : 6);
    out.rank = src != nullptr ? src->rank : (info != nullptr ? info->maxRank : 6);
    out.enhanced_id = src != nullptr ? src->enhancedId : 0;

    auto& bar = out.sp_bar.emplace();
    if (src != nullptr) {
        bar.cur_sp = src->spValue * 100;
        bar.max_sp = src->spMax * 100;
    } else {
        bar.max_sp = info != nullptr ? info->spNeed : kFullHp;
        bar.cur_sp = bar.max_sp / 2;
    }

    if (src != nullptr && !src->skills.empty()) {
        for (const auto& [point, level] : src->skills) {
            proto::AvatarSkillTree node;
            node.point_id = point;
            node.level = level;
            out.skilltree_list.push_back(node);
        }
    } else if (const std::vector<data::SkillTreePoint>* tree = tables.skillTree(pathId)) {
        // Not in the build: unlock the whole tree at its table maximum.
        for (const data::SkillTreePoint& point : *tree) {
            proto::AvatarSkillTree node;
            node.point_id = point.pointId;
            node.level = point.maxLevel;
            out.skilltree_list.push_back(node);
        }
    }

    if (const Lightcone* lc = lightconeOf(pathId)) {
        proto::BattleEquipment equipment;
        equipment.id = lc->itemId;
        equipment.level = lc->level;
        equipment.promotion = lc->promotion;
        equipment.rank = lc->rank;
        out.equipment_list.push_back(equipment);
    }
    for (const Relic* relic : relicsOf(pathId)) {
        proto::BattleRelic battleRelic;
        battleRelic.id = relic->relicId;
        battleRelic.unique_id = relicUniqueId(relic->internalUid);
        battleRelic.level = relic->level;
        battleRelic.main_affix_id = relic->mainAffixId;
        for (const SubAffix& affix : relic->subAffixes) {
            proto::RelicAffix sub;
            sub.affix_id = affix.id;
            sub.cnt = affix.count;
            sub.step = affix.step;
            battleRelic.sub_affix_list.push_back(sub);
        }
        out.relic_list.push_back(std::move(battleRelic));
    }

    // Technique buffs: what srtools says is switched on, else the table default,
    // else the id * 100 + 1 shape every avatar follows.
    std::vector<uint32_t> techniques;
    uint32_t skillIndex = info != nullptr ? info->techniqueSkillIndex : 2;
    if (src != nullptr) {
        techniques = src->techniques;
    } else if (info != nullptr && !info->techniqueBuffs.empty()) {
        techniques = info->techniqueBuffs;
    } else {
        techniques.push_back(pathId * 100 + 1);
    }
    for (uint32_t buffId : techniques) {
        if (buffId == 0) continue;
        proto::BattleBuff buff;
        buff.id = buffId;
        buff.level = 1;
        buff.owner_index = index;
        buff.wave_flag = 0xFFFFFFFFu;
        buff.dynamic_values["SkillIndex"] = static_cast<float>(skillIndex);
        buffs.push_back(std::move(buff));
    }
    return out;
}

}  // namespace game
