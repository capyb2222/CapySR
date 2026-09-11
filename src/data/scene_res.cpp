#include "data/scene_res.h"

#include <cstdlib>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "core/util.h"

using json = nlohmann::json;

namespace data {
namespace {

uint32_t u32(const json& j, const char* key, uint32_t fallback = 0) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<uint32_t>() : fallback;
}

int32_t i32(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int32_t>() : 0;
}

ResVector vec(const json& j, const char* key) {
    ResVector v;
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return v;
    v.x = i32(*it, "x");
    v.y = i32(*it, "y");
    v.z = i32(*it, "z");
    return v;
}

std::vector<uint32_t> u32List(const json& j, const char* key) {
    std::vector<uint32_t> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& e : *it) {
        if (e.is_number()) out.push_back(e.get<uint32_t>());
    }
    return out;
}

uint32_t parseId(const std::string& s) {
    return static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
}

// The plane key is "P<planeId>_F<floorId>".
bool parsePlaneKey(const std::string& key, uint32_t& planeId, uint32_t& floorId) {
    size_t sep = key.find('_');
    if (key.size() < 4 || key[0] != 'P' || sep == std::string::npos || sep + 2 > key.size() ||
        key[sep + 1] != 'F') {
        return false;
    }
    planeId = parseId(key.substr(1, sep - 1));
    floorId = parseId(key.substr(sep + 2));
    return planeId != 0 && floorId != 0;
}

}  // namespace

const ResTeleport* ResFloor::teleport(uint32_t id) const {
    for (const ResGroup& group : groups) {
        for (const ResTeleport& tp : group.teleports) {
            if (tp.id == id) return &tp;
        }
    }
    return nullptr;
}

const ResTeleport* ResFloor::anyTeleport() const {
    for (const ResGroup& group : groups) {
        if (!group.teleports.empty()) return &group.teleports.front();
    }
    return nullptr;
}

SceneRes& SceneRes::get() {
    static SceneRes instance;
    return instance;
}

const ResFloor* SceneRes::byEntry(uint32_t entryId) const {
    auto it = byEntry_.find(entryId);
    return it == byEntry_.end() ? nullptr : &floors_[it->second];
}

uint32_t SceneRes::defaultEntrance(uint32_t floorId) const {
    auto it = floorToEntry_.find(floorId);
    return it == floorToEntry_.end() ? 0 : it->second;
}

const ResFloor* SceneRes::byFloor(uint32_t floorId) const {
    uint32_t entryId = defaultEntrance(floorId);
    if (entryId != 0) {
        if (const ResFloor* floor = byEntry(entryId)) return floor;
    }
    for (const ResFloor& floor : floors_) {
        if (floor.floorId == floorId) return &floor;
    }
    return nullptr;
}

const ResTeleport* SceneRes::anchor(uint32_t entryId, uint32_t anchorId) const {
    auto it = anchors_.find(entryId);
    if (it == anchors_.end() || it->second.empty()) return nullptr;
    if (anchorId != 0) {
        for (const ResTeleport& candidate : it->second) {
            if (candidate.anchorId == anchorId) return &candidate;
        }
    }
    return &it->second.front();
}

bool SceneRes::loadAnchors(const std::string& path) {
    if (!anchors_.empty()) return true;
    bool ok = false;
    std::string text = util::readFile(path, &ok);
    if (!ok) {
        logging::debug("scene", "{} not found -- entrances fall back to teleport pads", path);
        return false;
    }

    json doc;
    try {
        doc = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        logging::warn("scene", "{} is not valid json: {}", path, e.what());
        return false;
    }

    // Bare array, or the single-key wrapper the beta extracts use.
    const json* rows = &doc;
    if (doc.is_object()) {
        for (const auto& entry : doc.items()) {
            if (entry.value().is_array()) rows = &entry.value();
        }
    }
    if (!rows->is_array()) return false;

    size_t count = 0;
    for (const json& row : *rows) {
        if (!row.is_object()) continue;
        uint32_t entryId = u32(row, "entryID", u32(row, "entry_id"));
        auto list = row.find("anchor");
        if (entryId == 0 || list == row.end() || !list->is_array()) continue;
        std::vector<ResTeleport> anchors;
        for (const json& a : *list) {
            if (!a.is_object()) continue;
            ResTeleport point;
            point.anchorId = u32(a, "ID");
            point.pos = vec(a, "pos");
            point.rot = vec(a, "rot");
            anchors.push_back(point);
            ++count;
        }
        if (!anchors.empty()) anchors_.emplace(entryId, std::move(anchors));
    }
    logging::info("scene", "{} entrance anchors for {} entries from {}", count, anchors_.size(),
                  path);
    return !anchors_.empty();
}

bool SceneRes::load(const std::string& path) {
    if (loaded()) return true;

    uint64_t started = util::nowMs();
    bool ok = false;
    std::string text = util::readFile(path, &ok);
    if (!ok) {
        logging::warn("scene", "{} not found -- scenes will be empty", path);
        return false;
    }

    json doc;
    try {
        doc = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        logging::error("scene", "{} is not valid json: {}", path, e.what());
        return false;
    }
    text.clear();
    text.shrink_to_fit();

    size_t groupCount = 0;
    size_t entityCount = 0;

    auto entries = doc.find("levelOutputConfigs");
    if (entries != doc.end() && entries->is_object()) {
        floors_.reserve(entries->size());
        for (const auto& entry : entries->items()) {
            uint32_t entryId = parseId(entry.key());
            if (entryId == 0 || !entry.value().is_object()) continue;

            for (const auto& planeEntry : entry.value().items()) {
                const json& cfg = planeEntry.value();
                if (!cfg.is_object()) continue;

                ResFloor floor;
                floor.entryId = entryId;
                if (!parsePlaneKey(planeEntry.key(), floor.planeId, floor.floorId)) {
                    logging::debug("scene", "entry {} has an odd plane key '{}'", entryId,
                                   planeEntry.key());
                    continue;
                }
                floor.planeType = u32(cfg, "planeType");
                floor.worldId = u32(cfg, "worldId");
                auto entered = cfg.find("isEnteredSceneInfo");
                floor.isEnteredSceneInfo =
                    entered != cfg.end() && entered->is_boolean() && entered->get<bool>();
                floor.sections = u32List(cfg, "sections");

                if (auto saved = cfg.find("savedValues");
                    saved != cfg.end() && saved->is_object()) {
                    for (const auto& sv : saved->items()) {
                        if (sv.value().is_number()) {
                            floor.savedValues[sv.key()] = sv.value().get<int32_t>();
                        }
                    }
                }

                if (auto scenes = cfg.find("scenes"); scenes != cfg.end() && scenes->is_object()) {
                    floor.groups.reserve(scenes->size());
                    for (const auto& sceneEntry : scenes->items()) {
                        const json& g = sceneEntry.value();
                        if (!g.is_object()) continue;

                        ResGroup group;
                        group.groupId = parseId(sceneEntry.key());

                        if (auto props = g.find("props"); props != g.end() && props->is_array()) {
                            for (const auto& p : *props) {
                                if (!p.is_object()) continue;
                                ResProp prop;
                                prop.groupId = u32(p, "groupId", group.groupId);
                                prop.instId = u32(p, "instId");
                                prop.propId = u32(p, "propId");
                                prop.propState = u32(p, "propState");
                                prop.pos = vec(p, "pos");
                                prop.rot = vec(p, "rot");
                                group.props.push_back(prop);
                            }
                        }
                        if (auto npcs = g.find("npcs"); npcs != g.end() && npcs->is_array()) {
                            for (const auto& n : *npcs) {
                                if (!n.is_object()) continue;
                                ResNpc npc;
                                npc.groupId = u32(n, "groupId", group.groupId);
                                npc.instId = u32(n, "instId");
                                npc.npcId = u32(n, "npcId");
                                npc.pos = vec(n, "pos");
                                npc.rot = vec(n, "rot");
                                group.npcs.push_back(npc);
                            }
                        }
                        if (auto mons = g.find("monsters"); mons != g.end() && mons->is_array()) {
                            for (const auto& m : *mons) {
                                if (!m.is_object()) continue;
                                ResMonster monster;
                                monster.groupId = u32(m, "groupId", group.groupId);
                                monster.instId = u32(m, "instId");
                                monster.monsterId = u32(m, "monsterId");
                                monster.eventId = u32(m, "eventId");
                                monster.pos = vec(m, "pos");
                                monster.rot = vec(m, "rot");
                                group.monsters.push_back(monster);
                            }
                        }
                        if (auto tps = g.find("teleports"); tps != g.end() && tps->is_object()) {
                            for (const auto& tpEntry : tps->items()) {
                                const json& t = tpEntry.value();
                                if (!t.is_object()) continue;
                                ResTeleport tp;
                                tp.id = parseId(tpEntry.key());
                                tp.anchorId = u32(t, "anchorId");
                                tp.groupId = u32(t, "groupId", group.groupId);
                                tp.instId = u32(t, "instId");
                                tp.pos = vec(t, "pos");
                                tp.rot = vec(t, "rot");
                                group.teleports.push_back(tp);
                            }
                        }
                        group.chests = u32List(g, "chests");
                        group.finishedSubMissions = u32List(g, "finishedSubMissions");
                        group.finishedMainMissions = u32List(g, "finishedMainMissions");

                        entityCount += group.props.size() + group.npcs.size() +
                                       group.monsters.size();
                        ++groupCount;
                        floor.groups.push_back(std::move(group));
                    }
                }

                byEntry_[entryId] = floors_.size();
                floors_.push_back(std::move(floor));
            }
        }
    }

    if (auto map = doc.find("mapDefaultEntranceMap"); map != doc.end() && map->is_object()) {
        for (const auto& e : map->items()) {
            if (!e.value().is_number()) continue;
            floorToEntry_[parseId(e.key())] = e.value().get<uint32_t>();
        }
    }

    if (auto rec = doc.find("relicAvatarRecommend"); rec != doc.end() && rec->is_object()) {
        for (const auto& e : rec->items()) {
            if (!e.value().is_array()) continue;
            std::vector<uint32_t> avatars;
            for (const auto& id : e.value()) {
                if (id.is_number()) avatars.push_back(id.get<uint32_t>());
            }
            relicRecommend_[parseId(e.key())] = std::move(avatars);
        }
    }

    doc = json();
    logging::info("scene", "{} floors, {} groups, {} entities from {} in {} ms", floors_.size(),
                  groupCount, entityCount, path, util::nowMs() - started);
    return !floors_.empty();
}

}  // namespace data
