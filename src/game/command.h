#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace net {
class Session;
}

namespace game {

// The chat console. A private server has no GM panel, so the one place a player can
// reach the server with a keyboard is the chat box: `src/game/chat_handlers.cpp` puts a
// friend named CapySR in the friend list and routes whatever is typed at it through
// here.
namespace command {

// The console's own uid. Anything but the player's, so the client files the thread
// under a contact of its own.
constexpr uint32_t kConsoleUid = 2000;

struct Entry {
    std::string_view name;
    std::string_view usage;  // arguments only, empty for a command that takes none
    std::string_view help;
};

// Every command, in the order /help lists them.
const std::vector<Entry>& list();

// Runs one typed line, with or without its leading '/', and answers with the lines to
// send back. Never empty: an unknown command answers with the complaint to show.
std::vector<std::string> run(net::Session& session, std::string_view line);

}  // namespace command
}  // namespace game
