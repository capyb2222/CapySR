// Exercises the game layer against the real tables and scene dump when they are
// present; the data-driven checks are skipped (and reported) when they are not.
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/files.h"
#include "core/util.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/battle.h"
#include "game/challenge.h"
#include "game/gacha.h"
#include "game/inventory.h"
#include "game/lineup.h"
#include "game/player.h"
#include "game/player_store.h"
#include "game/roster.h"
#include "game/scene.h"
#include "game/shop.h"
#include "game/srtools.h"
#include "game/tierce.h"
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

    // Main missions come from the client's own (beta) table, which lacks 2031101.
    const std::vector<uint32_t>& missions = tables.mainMissions();
    check(!tables.knowsMainMission(2031101) &&
              std::find(missions.begin(), missions.end(), 2031101u) == missions.end(),
          "a mission the client lacks is not reported");
    check(tables.knowsMainMission(1054600) && tables.knowsMainMission(1000101),
          "beta and production missions both are");
}

bool contains(const std::vector<uint32_t>& ids, uint32_t id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

uint32_t tokenCount(const proto::GachaItem& result, uint32_t itemId) {
    uint32_t total = 0;
    if (!result.token_item.has()) return 0;
    for (const proto::Item& item : result.token_item->item_list) {
        if (item.item_id == itemId) total += item.num;
    }
    return total;
}

void testGacha() {
    const data::Tables& tables = data::Tables::get();
    if (!tables.loaded()) {
        std::printf("SKIP gacha (no data source configured)\n");
        return;
    }

    check(tables.avatar(1001) != nullptr && tables.avatar(1001)->rarity == 4, "March is a 4*");
    check(tables.avatar(1003) != nullptr && tables.avatar(1003)->rarity == 5, "Himeko is a 5*");
    check(tables.lightcone(23000) != nullptr && tables.lightcone(23000)->rarity == 5,
          "light cone rarity is read");
    check(tables.lightcone(20000) != nullptr && tables.lightcone(20000)->rarity == 3,
          "down to the 3* ones");
    check(tables.battlePassReward(21028) && !tables.battlePassReward(21000),
          "battle pass light cones are known");

    // The beta client's own table has six pools, and it throws on any other.
    check(tables.gachaPools().size() == 5, "the client's pools, less the beginner one");
    std::vector<data::GachaPool> standardOnly = game::offeredGachaPools(tables, {});
    check(standardOnly.size() == 1 && standardOnly[0].id == 1001,
          "only the standard pool unless limited ones are configured");
    const std::vector<core::WarpBanner> limited{{2002, 0}, {3002, 0}, {2002, 0},
                                                {2138, 0}, {1001, 0}, {4001, 0}};
    std::vector<data::GachaPool> offered = game::offeredGachaPools(tables, limited);
    check(offered.size() == 3, "each configured pool once, if the table has it");
    check(!offered.empty() && offered[0].id == 1001, "standard first");
    std::optional<data::GachaPool> character = game::offeredGachaPool(tables, limited, 2002);
    check(character && character->type == data::GachaType::AvatarUp &&
              character->featured == 1204 && character->upChance == 50,
          "2002 features 1204 at 50%");
    std::optional<data::GachaPool> lightcone = game::offeredGachaPool(tables, limited, 3002);
    check(lightcone && lightcone->type == data::GachaType::WeaponUp &&
              lightcone->featured == 23010 && lightcone->upChance == 75,
          "3002 features 23010 at 75%");
    check(!game::offeredGachaPool(tables, limited, 2138), "a pool the client lacks is not offered");
    check(!game::offeredGachaPool(tables, limited, 4001), "nor the beginner pool");
    check(!game::offeredGachaPool(tables, {}, 2002), "nor an unconfigured one");

    game::GachaItemPools items = game::gachaItemPools(tables);
    check(items.complete(), "every rarity has something to draw");
    check(items.fiveStarAvatars.size() == 7 && items.fiveStarLightcones.size() == 7,
          "seven standard 5* of each");
    check(contains(items.fourStarAvatars, 1001) && !contains(items.fourStarAvatars, 1224),
          "4* characters without March's second form");
    check(!contains(items.fourStarAvatars, 1003), "and without 5*s");
    check(contains(items.fourStarLightcones, 21000) && !contains(items.fourStarLightcones, 21028) &&
              !contains(items.fourStarLightcones, 22000),
          "4* light cones without pass or event ones");
    check(items.threeStarLightcones.size() == 25, "all 25 3* light cones");
    if (!items.complete() || !character || !lightcone || offered.empty()) return;

    auto unlucky = [] { return 0.999999; };
    auto lucky = [] { return 0.0; };

    game::GachaPity pity;
    std::vector<game::GachaPull> pulls = game::pullGacha(offered[0], items, pity, 91, unlucky);
    bool early = false;
    for (size_t i = 0; i < 89; ++i) early |= pulls[i].rarity == 5;
    check(!early && pulls[89].rarity == 5, "the 90th standard pull is a 5*");
    check(pulls[9].rarity == 4 && pulls[19].rarity == 4 && pulls[8].rarity == 3,
          "every tenth pull is a 4*");
    check(pulls[90].rarity == 4, "a 5* pushes the due 4* to the next pull");
    check(pity.sinceFive == 1 && pity.sinceFour == 0 && pity.total == 91, "pity is counted");

    game::GachaPity lcPity;
    pulls = game::pullGacha(*lightcone, items, lcPity, 80, unlucky);
    early = false;
    for (size_t i = 0; i < 79; ++i) early |= pulls[i].rarity == 5;
    check(!early && pulls[79].rarity == 5, "the 80th light cone pull is a 5*");

    game::GachaPity charPity;
    pulls = game::pullGacha(*character, items, charPity, 90, unlucky);
    check(pulls[89].rarity == 5 && pulls[89].avatar && pulls[89].itemId != 1204 &&
              contains(items.fiveStarAvatars, pulls[89].itemId),
          "losing the 50/50 gives a standard 5*");
    check(charPity.guaranteed, "and guarantees the next one");
    pulls = game::pullGacha(*character, items, charPity, 90, unlucky);
    check(pulls[89].itemId == 1204 && !charPity.guaranteed, "which is the featured 5*");

    game::GachaPity luckyPity;
    pulls = game::pullGacha(*character, items, luckyPity, 10, lucky);
    bool allFeatured = pulls.size() == 10;
    for (const game::GachaPull& pull : pulls) allFeatured &= pull.rarity == 5 && pull.itemId == 1204;
    check(allFeatured, "winning every roll gives the featured 5* every time");
    pulls = game::pullGacha(offered[0], items, luckyPity, 1, lucky);
    check(pulls[0].rarity == 5 && pulls[0].itemId == items.fiveStarAvatars[0], "standard draws too");

    game::SrToolsData roster;
    game::Avatar maxed;
    maxed.avatarId = 1003;
    maxed.rank = 6;
    roster.avatars[1003] = maxed;
    game::Avatar fresh;
    fresh.avatarId = 1004;
    roster.avatars[1004] = fresh;

    proto::GachaItem result = game::gachaResult({20000, 3, false}, roster);
    check(result.gacha_item.has() && result.gacha_item->item_id == 20000 && result.gacha_item->num == 1,
          "the card names the item");
    check(result.transfer_item_list.has() && result.token_item.has(), "both lists are present");
    check(tokenCount(result, 251) == 20 && !result.is_new, "a 3* light cone is 20 embers");
    check(tokenCount(game::gachaResult({23000, 5, false}, roster), 252) == 40, "a 5* one 40 starlight");
    check(tokenCount(game::gachaResult({21000, 4, false}, roster), 252) == 8, "a 4* one 8");
    result = game::gachaResult({1003, 5, true}, roster);
    check(tokenCount(result, 252) == 100 && result.transfer_item_list->item_list.empty(),
          "a 5* past E6 is 100 starlight");
    result = game::gachaResult({1004, 5, true}, roster);
    check(tokenCount(result, 252) == 40 && result.transfer_item_list->item_list.size() == 1 &&
              result.transfer_item_list->item_list[0].item_id == 11004,
          "below E6 it is 40 and an eidolon");
    result = game::gachaResult({1101, 5, true}, roster);
    check(result.is_new && tokenCount(result, 252) == 0, "a character not in the build is new");

    // A configured featured 5* replaces the pool's own, if it is of the pool's kind.
    std::optional<data::GachaPool> pearl = game::offeredGachaPool(tables, {{2001, 1503}}, 2001);
    check(pearl && pearl->type == data::GachaType::AvatarUp && pearl->featured == 1503 &&
              pearl->upChance == 50,
          "2001 can feature Pearl at 50%");
    std::optional<data::GachaPool> fourStar = game::offeredGachaPool(tables, {{2001, 1001}}, 2001);
    check(fourStar && fourStar->featured == 1102, "a 4* cannot be featured");
    std::optional<data::GachaPool> wrongKind = game::offeredGachaPool(tables, {{3001, 1503}}, 3001);
    check(wrongKind && wrongKind->featured == 23001, "nor a character on a light cone pool");
    if (pearl) {
        game::GachaPity pearlPity;
        pulls = game::pullGacha(*pearl, items, pearlPity, 10, lucky);
        bool allPearl = pulls.size() == 10;
        for (const game::GachaPull& pull : pulls) {
            allPearl &= pull.rarity == 5 && pull.avatar && pull.itemId == 1503;
        }
        check(allPearl, "winning the 50/50 draws her");
        pulls = game::pullGacha(*pearl, items, pearlPity, 90, unlucky);
        check(pulls[89].rarity == 5 && pulls[89].avatar && pulls[89].itemId != 1503 &&
                  contains(items.fiveStarAvatars, pulls[89].itemId) && pearlPity.guaranteed,
              "losing it gives a standard 5* character and the guarantee");
    }

    // Stellar Warp standing in for Pearl's banner.
    data::GachaPool standardPearl = game::effectiveGachaPool(offered[0], tables, 1503);
    check(standardPearl.id == 1001 && standardPearl.type == data::GachaType::AvatarUp &&
              standardPearl.featured == 1503 && standardPearl.upChance == 50,
          "Stellar Warp can feature Pearl at 50%");
    check(game::effectiveGachaPool(offered[0], tables, 0).type == data::GachaType::Normal,
          "0 keeps it the standard pool");
    check(game::effectiveGachaPool(offered[0], tables, 1001).type == data::GachaType::Normal,
          "a 4* cannot be featured on it");
    check(game::effectiveGachaPool(*character, tables, 1503).featured == 1204,
          "a limited pool is left alone");
    game::GachaPity standardPearlPity;
    pulls = game::pullGacha(standardPearl, items, standardPearlPity, 10, lucky);
    bool allStandardPearl = pulls.size() == 10;
    for (const game::GachaPull& pull : pulls) {
        allStandardPearl &= pull.rarity == 5 && pull.avatar && pull.itemId == 1503;
    }
    check(allStandardPearl, "and it draws her on a won 50/50");

    // The real dice, 200000 pulls: no two lost 50/50s in a row, and the losses spread evenly.
    game::GachaPity simPity;
    std::map<uint32_t, uint32_t> lostTo;
    uint32_t fiveStars = 0, owed = 0, lost = 0;
    bool lostTwice = false, lastLost = false;
    for (int batch = 0; batch < 20000; ++batch) {
        for (const game::GachaPull& pull :
             game::pullGacha(standardPearl, items, simPity, 10, game::gachaRoll)) {
            if (pull.rarity != 5) continue;
            ++fiveStars;
            if (pull.featured) {
                if (pull.guaranteed) ++owed;
                lastLost = false;
                continue;
            }
            ++lost;
            ++lostTo[pull.itemId];
            lostTwice |= lastLost;
            lastLost = true;
        }
    }
    check(!lostTwice, "a lost 50/50 is never followed by another");
    check(owed == lost || owed + 1 == lost, "every loss is paid back by the next 5*");
    double rate = static_cast<double>(fiveStars) / 200000.0;
    check(rate > 0.013 && rate < 0.019, "about 1.6% of pulls are 5*");
    bool even = lostTo.size() == items.fiveStarAvatars.size();
    for (const auto& [id, count] : lostTo) even &= count > lost * 8 / 100 && count < lost * 20 / 100;
    check(even, "a lost 50/50 lands evenly on the seven standard 5*s");
    std::printf("      warp simulation: %u 5* (%.2f%%), %u lost, spread:", fiveStars, rate * 100, lost);
    for (const auto& [id, count] : lostTo) std::printf(" %u=%u", id, count);
    std::printf("\n");

    // Pity outlives a restart.
    std::string realPath = core::Config::get().paths.playerFile;
    core::Config::get().paths.playerFile = "build/test-gacha-player.json";
    game::Player saved(1);
    saved.gacha().character = {12, 3, true, 345};
    check(game::savePlayerState(saved), "pity saves");
    game::Player loaded(1);
    check(game::loadPlayerState(loaded), "and loads");
    const game::GachaPity& back = loaded.gacha().character;
    check(back.sinceFive == 12 && back.sinceFour == 3 && back.guaranteed && back.total == 345,
          "unchanged");
    core::Config::get().paths.playerFile = realPath;
}

void testInventory() {
    const data::Tables& tables = data::Tables::get();
    if (!tables.loaded()) {
        std::printf("SKIP inventory (no data source configured)\n");
        return;
    }
    namespace inventory = game::inventory;

    const data::StaminaRules& rules = tables.staminaRules();
    check(rules.max == 300 && rules.recoverSeconds == 360 && rules.reserveMax == 2400 &&
              rules.reserveRecoverSeconds == 1080,
          "stamina rules come from the tables");
    check(tables.staminaPrices().size() == 8 && tables.staminaPrices()[0] == 50 &&
              tables.staminaPerPurchase() == 60,
          "and the stamina prices");

    game::Player player(1);
    game::Inventory& bag = player.inventory();
    bag.stamina = 100;
    bag.staminaUpdatedAt = 1000;
    inventory::refreshStamina(player, 1000 + 360 * 5 + 10);
    check(bag.stamina == 105 && bag.staminaUpdatedAt == 1000 + 360 * 5,
          "stamina recovers a point every six minutes");
    check(inventory::nextRecoverTime(player) == 1000 + 360 * 6, "and says when the next is due");

    bag.stamina = 299;
    bag.staminaUpdatedAt = 5000;
    bag.reserveStamina = 0;
    inventory::refreshStamina(player, 5000 + 360 + 1080 * 2);
    check(bag.stamina == 300 && bag.reserveStamina == 2, "once full, time runs into reserve stamina");
    check(inventory::nextRecoverTime(player) == 0, "and nothing more is due");

    inventory::chargeStamina(player, 40, 9000);
    check(bag.stamina == 260 && bag.staminaUpdatedAt == 9000, "spending from full starts the clock");

    uint32_t jade = bag.hcoin;
    uint32_t credits = bag.scoin;
    inventory::grant(player, {{1, 50}, {2, 1000}, {211, 3}, {22, 150}, {11, 60}});
    check(bag.hcoin == jade + 50 && bag.scoin == credits + 1000, "jade and credits go to the wallet");
    check(bag.items[211] == 3 && bag.stamina == 320, "materials to the bag, stamina to stamina");
    check(bag.items.count(22) == 0, "and trailblaze exp nowhere");

    const data::MappingInfo* calyx = inventory::dropTable(1001, 6);
    check(calyx != nullptr && !calyx->display.empty(), "a calyx has a drop table at world level 6");
    if (calyx != nullptr) {
        bool credit = false;
        for (const data::ItemStack& drop : inventory::rollDrops(*calyx, [] { return 0.5; })) {
            credit |= drop.id == 2 && drop.num > 0;
        }
        check(credit, "and a run drops credits");
    }
    const data::MappingInfo* shadow = inventory::dropTable(1101, 6);
    check(shadow != nullptr && shadow->worldLevel == 5, "a shadow's empty level falls back a level");
    if (shadow != nullptr) {
        bool boss = false;
        for (const data::ItemStack& drop : inventory::rollDrops(*shadow, [] { return 0.5; })) {
            boss |= drop.id == 110406 && drop.num == 5;
        }
        check(boss, "and its boss material scales with the world level");
    }
    check(inventory::dropTable(0, 6) == nullptr, "no table, no drops");

    const data::RewardInfo* reward = tables.reward(101101);
    check(reward != nullptr, "RewardData loads");
    if (reward != nullptr) {
        std::vector<data::ItemStack> items = inventory::rewardItems(*reward);
        check(items.size() == 2 && items[0].id == 1 && items[0].num == 200 && items[1].id == 2 &&
                  items[1].num == 20000,
              "a reward carries its jade and items");
    }
    std::vector<data::ItemStack> merged = inventory::merged({{2, 10}, {211, 1}, {2, 5}});
    check(merged.size() == 2 && merged[0].num == 15, "repeats merge");
    proto::PlayerSyncScNotify sync = inventory::sync(player, {{211, 3}, {1, 50}});
    check(sync.material_list.size() == 1 && sync.material_list[0].tid == 211 &&
              sync.material_list[0].num == 3 && sync.basic_info.has(),
          "a sync names bag totals and carries the wallet");

    // Season rewards: paid once, and the history marks them.
    bool unlocked = core::Config::get().gameplay.unlockAllChallenges;
    core::Config::get().gameplay.unlockAllChallenges = true;
    uint32_t before = bag.hcoin;
    std::vector<data::ItemStack> granted;
    proto::TakeChallengeRewardScRsp first = game::challenge::takeRewards(player, 100, granted);
    check(!first.taken_reward_list.empty() && !granted.empty() && bag.hcoin > before,
          "a cleared season's rewards pay out");
    std::vector<data::ItemStack> again;
    proto::TakeChallengeRewardScRsp second = game::challenge::takeRewards(player, 100, again);
    check(second.taken_reward_list.empty() && again.empty(), "and only once");
    const data::ChallengeGroupInfo* season = tables.challengeGroup(100);
    uint64_t line = season != nullptr ? tables.challengeRewardStars(season->rewardLineGroupId) : 0;
    uint64_t marked = 0;
    for (const proto::ChallengeGroup& group : game::challenge::history(&player).challenge_group_list) {
        if (group.group_id == 100) marked = group.taken_stars_count_reward;
    }
    check(marked != 0 && (marked & ~line) == 0, "the history marks what was claimed");
    core::Config::get().gameplay.unlockAllChallenges = unlocked;

    // A cleared floor shows in its season's statistics.
    game::ChallengeRecord& record = player.challengeRecords()[1];
    record.stars = 7;
    record.roundsUsed = 12;
    record.teams[0] = {1001, 1002};
    record.teams[1] = {1003, 1004};
    proto::GetChallengeGroupStatisticsScRsp stats = game::challenge::statistics(player, 100);
    check(stats.challenge_default.has() && stats.challenge_default->record_id == 1 &&
              stats.challenge_default->PPBHLLOJNEK.has() &&
              stats.challenge_default->PPBHLLOJNEK->EEJCPNAEKLJ == 3 &&
              stats.challenge_default->PPBHLLOJNEK->round_count == 12 &&
              stats.challenge_default->PPBHLLOJNEK->lineup_list.size() == 2,
          "a season's best clear shows in its statistics");
    proto::GetChallengeGroupStatisticsScRsp none = game::challenge::statistics(player, 101);
    check(none.challenge_default.has() && !none.challenge_default->PPBHLLOJNEK.has(),
          "and a season with no clear shows none");

    std::string realPath = core::Config::get().paths.playerFile;
    core::Config::get().paths.playerFile = "build/test-inventory-player.json";
    check(game::savePlayerState(player), "the inventory saves");
    game::Player loaded(1);
    check(game::loadPlayerState(loaded), "and loads");
    check(loaded.inventory().items[211] == 3 && loaded.inventory().reserveStamina == bag.reserveStamina &&
              loaded.challengeRecords().count(1) == 1 &&
              loaded.challengeRecords().at(1).teams[1].size() == 2 &&
              !loaded.challengeRewardsTaken().empty(),
          "bag, records and claimed rewards survive a restart");
    core::Config::get().paths.playerFile = realPath;
}

void testShop() {
    const data::Tables& tables = data::Tables::get();
    if (!tables.loaded()) {
        std::printf("SKIP shop (no data source configured)\n");
        return;
    }
    namespace inventory = game::inventory;
    namespace shop = game::shop;
    using data::GoodsRefresh;

    // Noon, local time. 2026-09-14 is a Monday.
    auto at = [](int year, int month, int day) {
        std::tm tm{};
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = 12;
        tm.tm_isdst = -1;
        return static_cast<int64_t>(std::mktime(&tm));
    };
    int64_t monday = at(2026, 9, 14);
    int64_t sunday = at(2026, 9, 20);
    int64_t nextMonday = at(2026, 9, 21);
    check(shop::periodOf(GoodsRefresh::Daily, monday) == 20260914 &&
              shop::periodOf(GoodsRefresh::Monthly, sunday) == 202609 &&
              shop::periodOf(GoodsRefresh::Never, monday) == 0,
          "goods refresh by day and by month, or never");
    check(shop::periodOf(GoodsRefresh::Weekly, sunday) == 20260914 &&
              shop::periodOf(GoodsRefresh::Weekly, nextMonday) == 20260921 &&
              shop::periodOf(GoodsRefresh::Weekly, at(2026, 10, 2)) == 20260928,
          "and by the week, from Monday, across a month end");

    // A material sold for credits, a few at a time.
    const data::GoodsInfo* pick = nullptr;
    const data::GoodsInfo* locked = nullptr;
    for (const auto& [shopId, info] : tables.shops()) {
        for (uint32_t goodsId : info.goods) {
            const data::GoodsInfo* goods = tables.goods(goodsId);
            const data::ItemInfo* item = goods != nullptr ? tables.item(goods->itemId) : nullptr;
            if (goods == nullptr) continue;
            if (item == nullptr && (locked == nullptr || goods->id < locked->id)) locked = goods;
            bool credits = goods->cost.size() == 1 && goods->cost[0].id == inventory::kCredit;
            if (item != nullptr && item->mainType == "Material" && credits && goods->limitTimes >= 2 &&
                goods->itemCount * goods->limitTimes <= 999 && (pick == nullptr || goods->id < pick->id)) {
                pick = goods;
            }
        }
    }
    check(pick != nullptr && locked != nullptr, "the shops stock materials for credits, and gear");
    if (pick == nullptr || locked == nullptr) return;

    // Rotating stock is what a client on another patch has no row for: its shop module
    // throws part way through building the shelf and the rest of that shop is lost. So
    // nothing paid for with Oneiric Shards, and no recharge page, ever goes out.
    size_t paid = 0;
    for (const auto& [shopId, info] : tables.shops()) {
        for (uint32_t goodsId : info.goods) {
            const data::GoodsInfo* goods = tables.goods(goodsId);
            if (goods == nullptr) continue;
            for (const data::ItemStack& cost : goods->cost) {
                if (cost.id == inventory::kOneiricShard) ++paid;
            }
        }
    }
    check(paid == 0, "and never real-money bundles, which rotate every patch");

    game::Player player(1);
    player.inventory().scoin = 0;
    std::vector<data::ItemStack> changed;
    proto::BuyGoodsCsReq req;
    req.shop_id = pick->shopId;
    req.goods_id = pick->id;
    req.goods_num = pick->limitTimes;
    check(shop::buy(player, req, monday, changed).retcode ==
                  static_cast<uint32_t>(proto::Retcode::RET_ITEM_NOT_ENOUGH) &&
              inventory::held(player, pick->itemId) == 0 && changed.empty(),
          "without the credits nothing is bought");

    uint32_t price = pick->cost[0].num * pick->limitTimes;
    player.inventory().scoin = price;
    proto::BuyGoodsScRsp bought = shop::buy(player, req, monday, changed);
    check(bought.retcode == 0 && bought.goods_buy_times == pick->limitTimes && player.inventory().scoin == 0,
          "buying up to the limit takes the whole price");
    check(inventory::held(player, pick->itemId) == uint64_t{pick->itemCount} * pick->limitTimes &&
              bought.return_item_list.has() && changed.size() == 2,
          "and puts the goods in the bag");

    proto::GetShopListScRsp listed = shop::list(player, tables.shop(pick->shopId)->type, monday);
    uint32_t shown = 0;
    for (const proto::Shop& entry : listed.shop_list) {
        for (const proto::Goods& goods : entry.goods_list) {
            if (goods.goods_id == pick->id) shown = goods.buy_times;
        }
    }
    check(shown == pick->limitTimes, "the shelf shows how many were bought");

    req.goods_num = 1;
    player.inventory().scoin = pick->cost[0].num;
    check(shop::buy(player, req, monday, changed).retcode ==
              static_cast<uint32_t>(proto::Retcode::RET_BUY_TIMES_LIMIT),
          "one more is over the limit");
    if (pick->refresh != GoodsRefresh::Never) {
        int64_t later = pick->refresh == GoodsRefresh::Daily    ? at(2026, 9, 15)
                        : pick->refresh == GoodsRefresh::Weekly ? nextMonday
                                                                : at(2026, 10, 1);
        check(shop::buy(player, req, later, changed).retcode == 0, "until the next period");
    }

    proto::BuyGoodsCsReq gear;
    gear.shop_id = locked->shopId;
    gear.goods_id = locked->id;
    gear.goods_num = 1;
    player.inventory().hcoin = 1000000;
    player.inventory().scoin = 1000000;
    player.inventory().mcoin = 1000000;
    check(shop::buy(player, gear, monday, changed).retcode ==
                  static_cast<uint32_t>(proto::Retcode::RET_GOODS_NOT_OPEN) &&
              player.inventory().hcoin == 1000000 && player.inventory().scoin == 1000000,
          "gear the srtools build keeps is not sold");

    player.inventory().items = {{101, 5}};
    check(!inventory::spend(player, {{101, 3}, {inventory::kCredit, 2000000}}) &&
              player.inventory().items[101] == 5,
          "spending is all or nothing");
    check(inventory::spend(player, {{101, 5}}) && player.inventory().items.count(101) == 0,
          "and an emptied stack leaves the bag");

    std::string realPath = core::Config::get().paths.playerFile;
    core::Config::get().paths.playerFile = "build/test-shop-player.json";
    game::savePlayerState(player);
    game::Player loaded(1);
    game::loadPlayerState(loaded);
    check(loaded.goodsPurchases().count(pick->id) == 1, "purchases survive a restart");
    core::Config::get().paths.playerFile = realPath;
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

    // Anomaly Arbitration. Production rows have no arena, so the beta dump fills it in,
    // and places season ten, which production does not have at all.
    check(tables.peakGroups().size() >= 10, "every arbitration season loaded");
    const data::PeakInfo* knight = tables.peak(101);
    check(knight != nullptr && knight->mapEntranceId == 3013501 && knight->eventId == 30501011,
          "a production knight gets its arena from the beta dump");
    check(knight != nullptr && knight->monsters.size() == 1 &&
              knight->monsters[0].configId == data::kPeakMarkerId,
          "and plants one monster over the arena's marker");
    const data::PeakInfo* boss = tables.peak(1004);
    check(boss != nullptr && boss->boss && boss->groupId == 10, "1004 is season ten's boss");
    check(boss != nullptr && boss->mapEntranceId == 3014601 && boss->mazeGroupId == 7,
          "on its own arena");
    check(boss != nullptr && boss->hardEventId == 30510022 && boss->hardTarget == 3007,
          "with a hard stage and target of its own");
    const data::BattleTargetInfo* turns = tables.battleTarget(3001);
    check(turns != nullptr && turns->param == 4 && !turns->countsDeaths,
          "target 3001 is four cycles or fewer");
    const data::BattleTargetInfo* deaths = tables.battleTarget(3000);
    check(deaths != nullptr && deaths->countsDeaths, "target 3000 counts avatars lost");
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

// The third node 4.6 bolts onto a floor: its own arena and monster, everything else
// borrowed from the floor it extends.
void testTierceRun() {
    if (!data::SceneRes::get().loaded() || !data::Tables::get().loaded()) return;
    const data::Tables& tables = data::Tables::get();

    const data::ChallengeTierceInfo* config = tables.challengeTierce(5213);
    check(config != nullptr, "the tierce table is loaded");
    if (config == nullptr) return;
    check(config->preChallengeId == 5212, "5213 extends Memory of Chaos floor 5212");
    check(config->roundLimit == 45, "with a cycle pool for the whole floor");
    check(config->mazeGroupId == 11, "its node is scene group 11");
    check(config->monsters.size() == 1, "holding one planted monster");
    check(config->targetIds.size() == 3, "and three star targets");
    check(tables.challengeTierceFor(5212) == config, "the floor finds it the other way too");

    const data::ChallengeInfo* floor = tables.challenge(config->preChallengeId);
    check(floor != nullptr, "the floor it extends is loaded");
    if (floor == nullptr) return;

    game::Player player = makeTestPlayer();
    std::vector<proto::ChallengeTierceStageLineupInfo> stages(3);
    for (uint32_t stage = 0; stage < 3; ++stage) {
        proto::AvatarIdentifier one;
        one.id = 8001 + stage;
        stages[stage].lineup.push_back(one);
        stages[stage].buff_id = 1000 + stage;
    }

    proto::SceneInfo scene;
    uint32_t retcode = game::tierce::start(player, 5213, false, 0, stages, scene);
    check(retcode == 0, "the floor starts");
    if (retcode != 0) return;
    check(player.tierce().active, "the run is recorded on the player");
    check(player.tierce().stage == 0, "on its first node");
    check(player.tierce().roundsLeft == 45, "with the whole cycle pool");
    check(player.location().entryId == floor->mapEntranceId,
          "and the first node is the floor's own arena");

    // The third node is the one the tierce row plants, not one of the floor's two.
    player.tierce().stage = 2;
    game::ChallengeArena storage;
    const game::ChallengeArena* arena = game::tierce::arena(player, storage);
    check(arena != nullptr && arena->mazeGroupId == config->mazeGroupId,
          "the last node fights in the tierce's own group");
    check(arena != nullptr && arena->monsters == &config->monsters,
          "with the tierce's own monster");
    player.tierce().stage = 0;

    // What the node brings to a fight comes from both rows.
    game::BattleRequest request;
    game::tierce::prepareBattle(player, request);
    check(request.party.size() == 1, "the node fights with its own team");
    check(request.mazeBuffId == floor->mazeBuffId, "under the floor's maze buff");
    check(request.stageBuffId == 1000, "and the buff picked for this node");
    check(request.roundsLimit == 45, "with the cycles left in the pool");

    // Starting again keeps the spot the first attempt left from.
    game::SceneLocation origin = player.tierce().origin;
    proto::SceneInfo again;
    check(game::tierce::start(player, 5213, false, 0, stages, again) == 0, "a retry starts");
    check(player.tierce().origin.entryId == origin.entryId, "and goes back to the same place");

    // Every floor with a third node shows up in the history, under the tierce's id.
    proto::GetChallengeTierceDataScRsp history = game::tierce::history(player);
    bool found = false;
    for (const proto::ChallengeTierceData& entry : history.challenge_info_list) {
        if (entry.challenge_id != 5213) continue;
        found = true;
        check(entry.stage_info_list.size() == 3, "with a slot for each of its three nodes");
    }
    check(found, "the floor is in the history");
}

// "auto" hands a calyx over to the srtools build and leaves every other fight on the
// stage the client named. See gameplay.battle_source.
void testBattleSourceSplit() {
    if (!data::Tables::get().loaded()) return;
    if (core::Config::get().gameplay.battleSource != "auto") return;

    game::Player player = makeTestPlayer();
    game::Roster roster = player.roster();
    const game::BattleConfig& build = roster.data().battle;
    if (build.waves.empty() || build.stageId == 0) {
        std::printf("SKIP battle source split (the build on disk names no fight)\n");
        return;
    }
    const data::CocoonInfo* cocoon = data::Tables::get().cocoon(1001, player.worldLevel());
    if (cocoon == nullptr || cocoon->stageIds.empty()) {
        std::printf("SKIP battle source split (calyx 1001 is not in the tables)\n");
        return;
    }
    uint32_t calyxStage = cocoon->stageIds.front();
    check(calyxStage != build.stageId, "the calyx and the build name different stages");

    game::BattleRequest calyx;
    calyx.cocoonId = 1001;
    calyx.wave = 1;
    calyx.stageIds.push_back(calyxStage);
    calyx.allowSrToolsOverride = true;
    proto::SceneBattleInfo run = game::battle::create(player, calyx);
    check(run.stage_id == build.stageId, "a calyx fights the srtools build");

    game::BattleRequest shadow;
    shadow.stageIds.push_back(calyxStage);
    proto::SceneBattleInfo other = game::battle::create(player, shadow);
    check(other.stage_id == calyxStage, "every other fight keeps its own stage");
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

    // The write is queued; wait for it before reading.
    files::flush();
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
    // Anything that saves goes to a scratch file: a test must never write the save a
    // real client is using.
    core::Config::get().paths.playerFile = "build/test-player.json";
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
    testGacha();
    testInventory();
    testShop();
    testChallengeHistory();
    testSceneRes();
    testRosterProtos();
    testSceneBuild();
    testChallengeRun();
    testTierceRun();
    testEveryScenePacketFits();
    testBattleBuild();
    testBattleSourceSplit();
    testPropInteraction();
    testSrToolsRoundTrip();
}
