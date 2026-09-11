#include "game/player_store.h"

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "game/player.h"

using json = nlohmann::json;

namespace game {
namespace {

uint32_t u32(const json& j, const char* key, uint32_t fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<uint32_t>() : fallback;
}

int32_t i32(const json& j, const char* key, int32_t fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int32_t>() : fallback;
}

bool boolOr(const json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

}  // namespace

bool loadPlayerState(Player& player) {
    const std::string& path = core::Config::get().paths.playerFile;
    bool ok = false;
    std::string text = util::readFile(path, &ok);
    if (!ok) {
        logging::info("player", "{} not found, starting from the defaults", path);
        return false;
    }

    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        logging::error("player", "{} is not valid json: {}", path, e.what());
        return false;
    }
    if (!j.is_object()) return false;

    if (auto it = j.find("name"); it != j.end() && it->is_string()) {
        player.setName(it->get<std::string>());
    }
    if (auto it = j.find("signature"); it != j.end() && it->is_string()) {
        player.setSignature(it->get<std::string>());
    }
    player.setHeadIcon(u32(j, "head_icon", player.headIcon()));
    player.setMainCharacter(u32(j, "main_character", player.mainCharacter()));
    player.setMarchType(u32(j, "march_type", player.marchType()));
    player.setGlobalBuffs(boolOr(j, "global_buffs", player.globalBuffs()));

    if (auto pos = j.find("position"); pos != j.end() && pos->is_object()) {
        Position& p = player.position();
        p.x = i32(*pos, "x", p.x);
        p.y = i32(*pos, "y", p.y);
        p.z = i32(*pos, "z", p.z);
        p.rotY = i32(*pos, "rot_y", p.rotY);
    }
    if (auto loc = j.find("scene"); loc != j.end() && loc->is_object()) {
        SceneLocation& l = player.location();
        l.planeId = u32(*loc, "plane_id", l.planeId);
        l.floorId = u32(*loc, "floor_id", l.floorId);
        l.entryId = u32(*loc, "entry_id", l.entryId);
    }

    LineupBook& book = player.lineups();
    if (auto squads = j.find("squads"); squads != j.end() && squads->is_array()) {
        for (size_t i = 0; i < squads->size() && i < kSquadCount; ++i) {
            const json& entry = (*squads)[i];
            if (!entry.is_object()) continue;
            Squad& squad = book.squad(static_cast<uint32_t>(i));
            if (auto name = entry.find("name"); name != entry.end() && name->is_string()) {
                squad.name = name->get<std::string>();
            }
            squad.slots.fill(0);
            if (auto slots = entry.find("slots"); slots != entry.end() && slots->is_array()) {
                for (size_t s = 0; s < slots->size() && s < kSquadSlots; ++s) {
                    if ((*slots)[s].is_number()) squad.slots[s] = (*slots)[s].get<uint32_t>();
                }
            }
            // Slots on disk may predate the packing rule, or have been hand edited.
            squad.compact();
            squad.favourite = entry.value("favourite", false);
            squad.leaderAvatarId = u32(entry, "leader", 0);
            if (!squad.contains(squad.leaderAvatarId)) {
                squad.leaderAvatarId = 0;
                for (uint32_t id : squad.slots) {
                    if (id != 0) {
                        squad.leaderAvatarId = id;
                        break;
                    }
                }
            }
        }
    }
    book.setCurIndex(u32(j, "cur_squad", book.curIndex()));
    book.setMp(u32(j, "mp", book.maxMp()));
    return true;
}

bool savePlayerState(const Player& player) {
    const LineupBook& book = player.lineups();
    json squads = json::array();
    for (uint32_t i = 0; i < kSquadCount; ++i) {
        const Squad& squad = book.squad(i);
        squads.push_back({{"name", squad.name},
                          {"slots", squad.slots},
                          {"leader", squad.leaderAvatarId},
                          {"favourite", squad.favourite}});
    }

    json j{
        {"uid", player.uid()},
        {"name", player.name()},
        {"signature", player.signature()},
        {"head_icon", player.headIcon()},
        {"main_character", player.mainCharacter()},
        {"march_type", player.marchType()},
        {"global_buffs", player.globalBuffs()},
        {"cur_squad", book.curIndex()},
        {"mp", book.mp()},
        {"squads", squads},
        {"position",
         {{"x", player.position().x},
          {"y", player.position().y},
          {"z", player.position().z},
          {"rot_y", player.position().rotY}}},
        {"scene",
         {{"plane_id", player.location().planeId},
          {"floor_id", player.location().floorId},
          {"entry_id", player.location().entryId}}},
    };

    const std::string& path = core::Config::get().paths.playerFile;
    if (!util::writeFile(path, j.dump(2))) {
        logging::warn("player", "could not write {}", path);
        return false;
    }
    return true;
}

}  // namespace game
