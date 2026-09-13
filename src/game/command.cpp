#include "game/command.h"

#include <algorithm>
#include <charconv>

#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "game/inventory.h"
#include "game/player.h"
#include "game/rescue.h"
#include "net/cmd_ids.h"
#include "net/session.h"
#include "proto/gen/protos.h"

namespace game::command {
namespace {

using Args = std::vector<std::string_view>;
using Reply = std::vector<std::string>;

Args split(std::string_view line) {
    Args out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ') ++i;
        if (i > start) out.push_back(line.substr(start, i - start));
    }
    return out;
}

// Parses a whole number, or nothing at all: a command must not act on a half-read id.
bool number(std::string_view text, uint32_t& out) {
    if (text.empty()) return false;
    uint32_t value = 0;
    auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size()) return false;
    out = value;
    return true;
}

Reply onHelp(net::Session&, Player&, const Args&) {
    Reply reply{"CapySR console. Commands:"};
    for (const Entry& entry : list()) {
        std::string line = "/";
        line += entry.name;
        if (!entry.usage.empty()) {
            line += ' ';
            line += entry.usage;
        }
        line += " - ";
        line += entry.help;
        reply.push_back(std::move(line));
    }
    return reply;
}

Reply onGive(net::Session& session, Player& player, const Args& args) {
    uint32_t itemId = 0;
    uint32_t count = 1;
    if (args.empty() || !number(args[0], itemId)) return {"usage: /give <item id> [count]"};
    if (args.size() > 1 && !number(args[1], count)) return {"that count is not a number"};
    if (count == 0) return {"nothing to give"};

    const data::ItemInfo* item = data::Tables::get().item(itemId);
    bool wallet = itemId == inventory::kStellarJade || itemId == inventory::kCredit ||
                  itemId == inventory::kOneiricShard;
    if (item == nullptr && !wallet) return {"no item " + std::to_string(itemId)};

    std::vector<data::ItemStack> items{{itemId, count}};
    inventory::grant(player, items);
    session.send(cmd::PlayerSyncScNotify, inventory::sync(player, items));
    player.saveNow();

    std::string held = std::to_string(inventory::held(player, itemId));
    return {"gave " + std::to_string(count) + " x " + std::to_string(itemId) + ", you now hold " + held};
}

Reply onRefill(net::Session& session, Player& player, const Args&) {
    const data::StaminaRules& rules = data::Tables::get().staminaRules();
    Inventory& bag = player.inventory();
    if (bag.stamina >= rules.max) return {"Trailblaze Power is already full"};

    bag.stamina = rules.max;
    bag.staminaUpdatedAt = static_cast<int64_t>(util::nowSec());
    session.send(cmd::StaminaInfoScNotify, inventory::staminaInfo(player));
    session.send(cmd::PlayerSyncScNotify, inventory::sync(player, {}));
    player.saveNow();
    return {"Trailblaze Power refilled to " + std::to_string(rules.max)};
}

Reply onUnstuck(net::Session& session, Player&, const Args&) {
    return {rescue::unstick(session, 0)};
}

Reply onTp(net::Session& session, Player&, const Args& args) {
    uint32_t entryId = 0;
    if (args.empty() || !number(args[0], entryId)) return {"usage: /tp <entrance id>"};
    if (data::Tables::get().entrances().count(entryId) == 0) {
        return {"no entrance " + std::to_string(entryId)};
    }
    return {rescue::unstick(session, entryId)};
}

Reply onWhere(net::Session&, Player& player, const Args&) {
    const SceneLocation& at = player.location();
    const Position& pos = player.position();
    return {"uid " + std::to_string(player.uid()) + ", world level " + std::to_string(player.worldLevel()),
            "entry " + std::to_string(at.entryId) + ", plane " + std::to_string(at.planeId) + ", floor " +
                std::to_string(at.floorId),
            "at " + std::to_string(pos.x / 1000) + " " + std::to_string(pos.y / 1000) + " " +
                std::to_string(pos.z / 1000)};
}

using Handler = Reply (*)(net::Session&, Player&, const Args&);

struct Command {
    Entry entry;
    Handler run;
};

const Command kCommands[] = {
    {{"help", "", "this list"}, onHelp},
    {{"give", "<item id> [count]", "put items in the bag"}, onGive},
    {{"refill", "", "Trailblaze Power back to full"}, onRefill},
    {{"unstuck", "", "end whatever the client is waiting on and redraw the scene"}, onUnstuck},
    {{"tp", "<entrance id>", "move to an entrance"}, onTp},
    {{"where", "", "uid, world level and where you are standing"}, onWhere},
};

}  // namespace

const std::vector<Entry>& list() {
    static const std::vector<Entry> entries = [] {
        std::vector<Entry> out;
        for (const Command& command : kCommands) out.push_back(command.entry);
        return out;
    }();
    return entries;
}

std::vector<std::string> run(net::Session& session, std::string_view line) {
    Player* player = session.player();
    if (player == nullptr) return {"not logged in yet"};

    if (!line.empty() && line.front() == '/') line.remove_prefix(1);
    Args args = split(line);
    if (args.empty()) return {"type /help for the command list"};

    std::string_view name = args.front();
    args.erase(args.begin());
    for (const Command& command : kCommands) {
        if (command.entry.name != name) continue;
        logging::info("command", "uid {} ran /{}", player->uid(), name);
        return command.run(session, *player, args);
    }
    return {"no command /" + std::string(name) + ", type /help for the list"};
}

}  // namespace game::command
