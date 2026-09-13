#include "game/srtools.h"

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/files.h"
#include "core/logger.h"
#include "core/util.h"

using json = nlohmann::json;

namespace game {
namespace {

uint32_t u32(const json& j, const char* key, uint32_t fallback = 0) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    return it->get<uint32_t>();
}

std::string str(const json& j, const char* key, const std::string& fallback = {}) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : fallback;
}

std::vector<SubAffix> readAffixes(const json& j) {
    std::vector<SubAffix> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        SubAffix a;
        a.id = u32(e, "sub_affix_id", u32(e, "subAffixId"));
        a.count = u32(e, "count");
        a.step = u32(e, "step");
        out.push_back(a);
    }
    return out;
}

void readAvatars(const json& j, SrToolsData& out) {
    if (!j.is_object()) return;
    for (const auto& [key, value] : j.items()) {
        if (!value.is_object()) continue;
        Avatar avatar;
        avatar.avatarId = u32(value, "avatar_id",
                              static_cast<uint32_t>(std::strtoul(key.c_str(), nullptr, 10)));
        if (avatar.avatarId == 0) continue;
        avatar.level = u32(value, "level", 1);
        avatar.promotion = u32(value, "promotion");
        avatar.enhancedId = u32(value, "enhanced_id");
        avatar.spValue = u32(value, "sp_value");
        avatar.spMax = u32(value, "sp_max", 120);

        if (auto data = value.find("data"); data != value.end() && data->is_object()) {
            avatar.rank = u32(*data, "rank");
            if (auto skills = data->find("skills"); skills != data->end() && skills->is_object()) {
                for (const auto& [pointId, level] : skills->items()) {
                    if (!level.is_number()) continue;
                    avatar.skills[static_cast<uint32_t>(std::strtoul(pointId.c_str(), nullptr, 10))] =
                        level.get<uint32_t>();
                }
            }
            if (auto anchors = data->find("skills_by_anchor_type");
                anchors != data->end() && anchors->is_object()) {
                for (const auto& [anchor, level] : anchors->items()) {
                    if (!level.is_number()) continue;
                    avatar.skillsByAnchor[static_cast<uint32_t>(
                        std::strtoul(anchor.c_str(), nullptr, 10))] = level.get<uint32_t>();
                }
            }
        }
        if (auto tech = value.find("techniques"); tech != value.end() && tech->is_array()) {
            for (const auto& t : *tech) {
                if (t.is_number()) avatar.techniques.push_back(t.get<uint32_t>());
            }
        }
        out.avatars[avatar.avatarId] = std::move(avatar);
    }
}

void readBattle(const json& j, BattleConfig& out) {
    if (!j.is_object()) return;
    out.battleType = str(j, "battle_type", "DEFAULT");
    out.stageId = u32(j, "stage_id");
    out.cycleCount = u32(j, "cycle_count");
    out.pathResonanceId = u32(j, "path_resonance_id");

    if (auto waves = j.find("monsters"); waves != j.end() && waves->is_array()) {
        for (const auto& wave : *waves) {
            if (!wave.is_array()) continue;
            std::vector<BattleMonster> monsters;
            for (const auto& m : wave) {
                if (!m.is_object()) continue;
                BattleMonster monster;
                monster.monsterId = u32(m, "monster_id");
                monster.level = u32(m, "level", 1);
                monster.amount = u32(m, "amount", 1);
                monsters.push_back(monster);
            }
            out.waves.push_back(std::move(monsters));
        }
    }
    if (auto blessings = j.find("blessings"); blessings != j.end() && blessings->is_array()) {
        for (const auto& b : *blessings) {
            if (!b.is_object()) continue;
            BattleBuff buff;
            buff.id = u32(b, "id");
            buff.level = u32(b, "level", 1);
            if (auto dyn = b.find("dynamic_key"); dyn != b.end() && dyn->is_object()) {
                buff.dynamicValues[str(*dyn, "key")] = static_cast<float>(u32(*dyn, "value"));
            }
            out.blessings.push_back(std::move(buff));
        }
    }
    if (auto stats = j.find("custom_stats"); stats != j.end()) out.customStats = readAffixes(*stats);
    if (auto lineup = j.find("custom_battle_lineup"); lineup != j.end() && lineup->is_object()) {
        for (const auto& [slot, avatarId] : lineup->items()) {
            if (!avatarId.is_number()) continue;
            out.customLineup[static_cast<uint32_t>(std::strtoul(slot.c_str(), nullptr, 10))] =
                avatarId.get<uint32_t>();
        }
    }
}

std::shared_ptr<SrToolsData> parse(const std::string& text, std::string& error) {
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& e) {
        error = std::string("malformed json: ") + e.what();
        return nullptr;
    }
    // srtools posts {"data": {...}}; a file on disk is the bare object.
    if (auto wrapper = j.find("data"); wrapper != j.end() && wrapper->is_object()) j = *wrapper;
    if (!j.is_object()) {
        error = "expected a json object";
        return nullptr;
    }

    auto data = std::make_shared<SrToolsData>();
    if (auto it = j.find("avatars"); it != j.end()) readAvatars(*it, *data);
    if (auto it = j.find("lightcones"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            Lightcone lc;
            lc.level = u32(e, "level", 1);
            lc.itemId = u32(e, "item_id", u32(e, "itemId"));
            lc.equipAvatar = u32(e, "equip_avatar", u32(e, "equipAvatar"));
            lc.rank = u32(e, "rank", 1);
            lc.promotion = u32(e, "promotion");
            lc.internalUid = u32(e, "internal_uid", u32(e, "internalUid"));
            data->lightcones.push_back(lc);
        }
    }
    if (auto it = j.find("relics"); it != j.end() && it->is_array()) {
        for (const auto& e : *it) {
            if (!e.is_object()) continue;
            Relic relic;
            relic.level = u32(e, "level");
            relic.relicId = u32(e, "relic_id", u32(e, "relicId"));
            relic.relicSetId = u32(e, "relic_set_id", u32(e, "relicSetId"));
            relic.mainAffixId = u32(e, "main_affix_id", u32(e, "mainAffixId"));
            relic.equipAvatar = u32(e, "equip_avatar", u32(e, "equipAvatar"));
            relic.internalUid = u32(e, "internal_uid", u32(e, "internalUid"));
            if (auto sub = e.find("sub_affixes"); sub != e.end()) relic.subAffixes = readAffixes(*sub);
            else if (auto sub2 = e.find("subAffixes"); sub2 != e.end()) relic.subAffixes = readAffixes(*sub2);
            data->relics.push_back(std::move(relic));
        }
    }
    if (auto it = j.find("battle_config"); it != j.end()) {
        readBattle(*it, data->battle);
        if (it->is_object()) data->battleJson = it->dump();
    }

    // Anything we do not model (srtools' own "key", the loadout presets) rides along
    // untouched so an equip change cannot throw it away.
    json extras = json::object();
    for (const auto& [key, value] : j.items()) {
        if (key == "avatars" || key == "lightcones" || key == "relics" ||
            key == "battle_config") {
            continue;
        }
        extras[key] = value;
    }
    if (!extras.empty()) data->extrasJson = extras.dump();

    data->loaded = true;
    return data;
}

json toJson(const SrToolsData& data) {
    json avatars = json::object();
    for (const auto& [id, avatar] : data.avatars) {
        json skills = json::object();
        for (const auto& [point, level] : avatar.skills) skills[std::to_string(point)] = level;
        json anchors = json::object();
        for (const auto& [anchor, level] : avatar.skillsByAnchor) {
            anchors[std::to_string(anchor)] = level;
        }
        avatars[std::to_string(id)] = {
            {"avatar_id", avatar.avatarId},
            {"level", avatar.level},
            {"promotion", avatar.promotion},
            {"enhanced_id", avatar.enhancedId},
            {"sp_value", avatar.spValue},
            {"sp_max", avatar.spMax},
            {"techniques", avatar.techniques},
            {"data", {{"rank", avatar.rank}, {"skills", skills},
                      {"skills_by_anchor_type", anchors}}},
        };
    }

    json lightcones = json::array();
    for (const Lightcone& lc : data.lightcones) {
        lightcones.push_back({{"level", lc.level},
                              {"item_id", lc.itemId},
                              {"equip_avatar", lc.equipAvatar},
                              {"rank", lc.rank},
                              {"promotion", lc.promotion},
                              {"internal_uid", lc.internalUid}});
    }

    json relics = json::array();
    for (const Relic& relic : data.relics) {
        json subs = json::array();
        for (const SubAffix& affix : relic.subAffixes) {
            subs.push_back(
                {{"sub_affix_id", affix.id}, {"count", affix.count}, {"step", affix.step}});
        }
        relics.push_back({{"level", relic.level},
                          {"relic_id", relic.relicId},
                          {"relic_set_id", relic.relicSetId},
                          {"main_affix_id", relic.mainAffixId},
                          {"sub_affixes", subs},
                          {"equip_avatar", relic.equipAvatar},
                          {"internal_uid", relic.internalUid}});
    }

    json out = data.extrasJson.empty() ? json::object() : json::parse(data.extrasJson);
    out["avatars"] = avatars;
    out["lightcones"] = lightcones;
    out["relics"] = relics;
    if (!data.battleJson.empty()) out["battle_config"] = json::parse(data.battleJson);
    return out;
}

}  // namespace

SrTools& SrTools::instance() {
    static SrTools tools;
    return tools;
}

std::shared_ptr<const SrToolsData> SrTools::data() const {
    std::lock_guard lock(mutex_);
    return data_;
}

void SrTools::set(std::shared_ptr<const SrToolsData> data) {
    std::lock_guard lock(mutex_);
    data_ = std::move(data);
}

void SrTools::mutate(const std::function<void(SrToolsData&)>& edit, bool persist) {
    auto next = std::make_shared<SrToolsData>();
    {
        std::lock_guard lock(mutex_);
        *next = *data_;
    }
    edit(*next);
    if (persist && !writeToDisk(*next)) {
        logging::warn("srtools", "could not write {}", core::Config::get().paths.srtoolsFile);
    }
    set(next);
}

bool SrTools::writeToDisk(const SrToolsData& data) const {
    try {
        // Queued: edits come in on the packet thread, and the file is only read at startup.
        files::writeLater(core::Config::get().paths.srtoolsFile, toJson(data).dump(2));
        return true;
    } catch (const std::exception& e) {
        logging::warn("srtools", "could not serialise the build: {}", e.what());
        return false;
    }
}

bool SrTools::loadFromDisk() {
    const std::string& path = core::Config::get().paths.srtoolsFile;
    bool ok = false;
    std::string text = util::readFile(path, &ok);
    if (!ok) {
        logging::warn("srtools", "{} not found -- upload a build from srtools.neonteam.dev", path);
        return false;
    }
    std::string error;
    auto parsed = parse(text, error);
    if (!parsed) {
        logging::error("srtools", "{}: {}", path, error);
        return false;
    }
    logging::info("srtools", "loaded {} avatars, {} lightcones, {} relics from {}",
              parsed->avatars.size(), parsed->lightcones.size(), parsed->relics.size(), path);
    set(std::move(parsed));
    return true;
}

std::string SrTools::upload(const std::string& body) {
    std::string error;
    auto parsed = parse(body, error);
    if (!parsed) return error;

    const std::string& path = core::Config::get().paths.srtoolsFile;
    try {
        json j = json::parse(body, nullptr, true, true);
        if (auto wrapper = j.find("data"); wrapper != j.end() && wrapper->is_object()) j = *wrapper;
        // An edit still queued must not land on top of the build replacing it.
        files::flush();
        if (!util::writeFile(path, j.dump(2))) return "could not write " + path;
    } catch (const std::exception& e) {
        return std::string("could not store the build: ") + e.what();
    }

    logging::info("srtools", "stored {} avatars, {} lightcones, {} relics", parsed->avatars.size(),
              parsed->lightcones.size(), parsed->relics.size());
    set(std::move(parsed));
    return "OK";
}

}  // namespace game
