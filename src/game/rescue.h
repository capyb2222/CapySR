#pragma once

#include <cstdint>
#include <string>

namespace net {
class Session;
}

namespace game {

// Getting the client out of a black screen without restarting it.
//
// A black screen is almost always the client waiting on a transition the server never
// finished: it began loading a scene, or a battle, and nothing arrived to end it. The
// client has no timeout for that, but it will drop whatever it was doing and render
// again the moment the server pushes it a scene -- so that is what this does.
namespace rescue {

// Ends any fight, abandons any challenge run, and pushes the player's scene again.
// `entryId` of 0 means wherever they already are; naming one moves them there instead,
// which is the way out when the scene itself is what the client cannot load.
// Returns a one-line summary of what was done, for the log and the http reply.
std::string unstick(net::Session& session, uint32_t entryId);

}  // namespace rescue
}  // namespace game
