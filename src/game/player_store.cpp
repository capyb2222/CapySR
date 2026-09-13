#include "game/player_store.h"

#include <cstdlib>
#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/files.h"
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

int64_t i64(const json& j, const char* key, int64_t fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<int64_t>() : fallback;
}

bool boolOr(const json& j, const char* key, bool fallback) {
    auto it = j.find(key);
    return (it != j.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

std::vector<uint32_t> u32Array(const json& j, const char* key) {
    std::vector<uint32_t> out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const json& entry : *it) {
        if (entry.is_number()) out.push_back(entry.get<uint32_t>());
    }
    return out;
}

}  // namespace

bool loadPlayerState(Player& player) {
    // A session that just ended may still have its save queued.
    files::flush();
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

    if (auto peak = j.find("peak"); peak != j.end() && peak->is_object()) {
        PeakProgress& progress = player.peakProgress();
        auto ids = [](const json& entry, const char* key) {
            std::vector<uint32_t> out;
            auto it = entry.find(key);
            if (it == entry.end() || !it->is_array()) return out;
            for (const json& id : *it) {
                if (id.is_number()) out.push_back(id.get<uint32_t>());
            }
            return out;
        };
        for (uint32_t groupId : ids(*peak, "hard")) progress.hardGroups.insert(groupId);
        if (auto fights = peak->find("fights"); fights != peak->end() && fights->is_array()) {
            for (const json& fight : *fights) {
                uint32_t id = fight.is_object() ? u32(fight, "id", 0) : 0;
                if (id == 0) continue;
                progress.teams[id] = ids(fight, "team");
                if (uint32_t buff = u32(fight, "buff", 0); buff != 0) progress.bossBuffs[id] = buff;
            }
        }
        if (auto records = peak->find("records"); records != peak->end() && records->is_array()) {
            for (const json& entry : *records) {
                uint32_t id = entry.is_object() ? u32(entry, "id", 0) : 0;
                if (id == 0) continue;
                PeakRecord& record =
                    progress.records[PeakProgress::key(id, boolOr(entry, "hard", false))];
                record.cycles = u32(entry, "cycles", 0);
                record.targets = ids(entry, "targets");
                record.team = ids(entry, "team");
                record.buffId = u32(entry, "buff", 0);
            }
        }
    }

    // The three-node floors: what the team editor last held, and how far each got.
    if (auto tierce = j.find("tierce"); tierce != j.end() && tierce->is_array()) {
        for (const json& entry : *tierce) {
            uint32_t id = entry.is_object() ? u32(entry, "id", 0) : 0;
            if (id == 0) continue;
            TierceProgress& progress = player.tierceHistory()[id];
            progress.passed = boolOr(entry, "passed", false);
            progress.targets = u32Array(entry, "targets");
            auto nodes = entry.find("nodes");
            if (nodes == entry.end() || !nodes->is_array()) continue;
            for (size_t stage = 0; stage < nodes->size() && stage < 3; ++stage) {
                const json& node = (*nodes)[stage];
                if (!node.is_object()) continue;
                progress.party[stage] = u32Array(node, "team");
                progress.buffs[stage] = u32(node, "buff", 0);
                progress.scores[stage] = u32(node, "score", 0);
                progress.cycles[stage] = u32(node, "cycles", 0);
                progress.deaths[stage] = u32(node, "deaths", 0);
                progress.cleared[stage] = boolOr(node, "cleared", false);
            }
        }
    }

    if (auto gacha = j.find("gacha"); gacha != j.end() && gacha->is_object()) {
        auto readPity = [&gacha](const char* key, GachaPity& pity) {
            auto it = gacha->find(key);
            if (it == gacha->end() || !it->is_object()) return;
            pity.sinceFive = u32(*it, "since_five", 0);
            pity.sinceFour = u32(*it, "since_four", 0);
            pity.guaranteed = boolOr(*it, "guaranteed", false);
            pity.total = u32(*it, "total", 0);
        };
        readPity("standard", player.gacha().standard);
        readPity("character", player.gacha().character);
        readPity("lightcone", player.gacha().lightcone);
    }

    if (auto saved = j.find("inventory"); saved != j.end() && saved->is_object()) {
        Inventory& bag = player.inventory();
        bag.stamina = u32(*saved, "stamina", bag.stamina);
        bag.reserveStamina = u32(*saved, "reserve_stamina", 0);
        bag.staminaUpdatedAt = i64(*saved, "stamina_updated_at", 0);
        bag.purchasesToday = u32(*saved, "stamina_purchases", 0);
        bag.purchaseDay = i64(*saved, "stamina_purchase_day", 0);
        bag.hcoin = u32(*saved, "hcoin", bag.hcoin);
        bag.scoin = u32(*saved, "scoin", bag.scoin);
        bag.mcoin = u32(*saved, "mcoin", bag.mcoin);
        if (auto items = saved->find("items"); items != saved->end() && items->is_object()) {
            for (const auto& entry : items->items()) {
                auto id = static_cast<uint32_t>(std::strtoul(entry.key().c_str(), nullptr, 10));
                if (id != 0 && entry.value().is_number_unsigned()) {
                    bag.items[id] = entry.value().get<uint32_t>();
                }
            }
        }
    }

    if (auto floors = j.find("challenge_records"); floors != j.end() && floors->is_array()) {
        for (const json& entry : *floors) {
            uint32_t id = entry.is_object() ? u32(entry, "id", 0) : 0;
            if (id == 0) continue;
            ChallengeRecord& record = player.challengeRecords()[id];
            record.stars = u32(entry, "stars", 0);
            record.roundsUsed = u32(entry, "rounds_used", 0);
            record.score = u32(entry, "score", 0);
            std::vector<uint32_t> buffs = u32Array(entry, "buffs");
            for (size_t i = 0; i < buffs.size() && i < 2; ++i) record.buffs[i] = buffs[i];
            if (auto teams = entry.find("teams"); teams != entry.end() && teams->is_array()) {
                for (size_t i = 0; i < teams->size() && i < 2; ++i) {
                    for (const json& avatar : (*teams)[i]) {
                        if (avatar.is_number_unsigned()) record.teams[i].push_back(avatar.get<uint32_t>());
                    }
                }
            }
        }
    }

    if (auto taken = j.find("challenge_rewards_taken"); taken != j.end() && taken->is_array()) {
        for (const json& entry : *taken) {
            uint32_t group = entry.is_object() ? u32(entry, "group", 0) : 0;
            auto stars = entry.is_object() ? entry.find("stars") : entry.end();
            if (group == 0 || stars == entry.end() || !stars->is_number_unsigned()) continue;
            player.challengeRewardsTaken()[group] = stars->get<uint64_t>();
        }
    }

    if (auto bought = j.find("goods_purchases"); bought != j.end() && bought->is_array()) {
        for (const json& entry : *bought) {
            uint32_t goods = entry.is_object() ? u32(entry, "goods", 0) : 0;
            if (goods == 0) continue;
            GoodsPurchase& purchase = player.goodsPurchases()[goods];
            purchase.times = u32(entry, "times", 0);
            purchase.period = i64(entry, "period", 0);
        }
    }
    return true;
}

std::string playerStateJson(const Player& player) {
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

    const PeakProgress& progress = player.peakProgress();
    json fights = json::array();
    for (const auto& [id, team] : progress.teams) {
        auto buff = progress.bossBuffs.find(id);
        fights.push_back({{"id", id},
                          {"team", team},
                          {"buff", buff != progress.bossBuffs.end() ? buff->second : 0u}});
    }
    json records = json::array();
    for (const auto& [key, record] : progress.records) {
        records.push_back({{"id", key / 2},
                           {"hard", key % 2 == 1},
                           {"cycles", record.cycles},
                           {"targets", record.targets},
                           {"team", record.team},
                           {"buff", record.buffId}});
    }
    j["peak"] = {{"hard", progress.hardGroups}, {"fights", fights}, {"records", records}};

    json tierce = json::array();
    for (const auto& [id, floor] : player.tierceHistory()) {
        json nodes = json::array();
        for (uint32_t stage = 0; stage < 3; ++stage) {
            nodes.push_back({{"team", floor.party[stage]},
                             {"buff", floor.buffs[stage]},
                             {"score", floor.scores[stage]},
                             {"cycles", floor.cycles[stage]},
                             {"deaths", floor.deaths[stage]},
                             {"cleared", floor.cleared[stage]}});
        }
        tierce.push_back({{"id", id},
                          {"passed", floor.passed},
                          {"targets", floor.targets},
                          {"nodes", nodes}});
    }
    j["tierce"] = tierce;

    auto pity = [](const GachaPity& p) {
        return json{{"since_five", p.sinceFive},
                    {"since_four", p.sinceFour},
                    {"guaranteed", p.guaranteed},
                    {"total", p.total}};
    };
    const GachaProgress& gacha = player.gacha();
    j["gacha"] = {{"standard", pity(gacha.standard)},
                  {"character", pity(gacha.character)},
                  {"lightcone", pity(gacha.lightcone)}};

    const Inventory& bag = player.inventory();
    json items = json::object();
    for (const auto& [id, count] : bag.items) {
        if (count != 0) items[std::to_string(id)] = count;
    }
    j["inventory"] = {{"stamina", bag.stamina},
                      {"reserve_stamina", bag.reserveStamina},
                      {"stamina_updated_at", bag.staminaUpdatedAt},
                      {"stamina_purchases", bag.purchasesToday},
                      {"stamina_purchase_day", bag.purchaseDay},
                      {"hcoin", bag.hcoin},
                      {"scoin", bag.scoin},
                      {"mcoin", bag.mcoin},
                      {"items", items}};

    json floors = json::array();
    for (const auto& [id, record] : player.challengeRecords()) {
        floors.push_back({{"id", id},
                          {"stars", record.stars},
                          {"rounds_used", record.roundsUsed},
                          {"score", record.score},
                          {"buffs", json::array({record.buffs[0], record.buffs[1]})},
                          {"teams", json::array({record.teams[0], record.teams[1]})}});
    }
    j["challenge_records"] = floors;

    json taken = json::array();
    for (const auto& [group, stars] : player.challengeRewardsTaken()) {
        taken.push_back({{"group", group}, {"stars", stars}});
    }
    j["challenge_rewards_taken"] = taken;

    json bought = json::array();
    for (const auto& [goods, purchase] : player.goodsPurchases()) {
        if (purchase.times == 0) continue;
        bought.push_back({{"goods", goods}, {"times", purchase.times}, {"period", purchase.period}});
    }
    j["goods_purchases"] = bought;

    return j.dump(2);
}

bool savePlayerState(const Player& player) {
    const std::string& path = core::Config::get().paths.playerFile;
    if (!util::writeFile(path, playerStateJson(player))) {
        logging::warn("player", "could not write {}", path);
        return false;
    }
    return true;
}

}  // namespace game
