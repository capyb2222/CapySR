#pragma once

#include <string>

#include "proto/gen/protos.h"

namespace game::lua {

// Wraps Lua for ClientDownloadDataScNotify, which the client runs as it arrives. Each
// push gets a newer version than the last.
proto::ClientDownloadDataScNotify push(std::string code);

// Stops every UI canvas drawing, and draws them again.
std::string hideUi();
std::string showUi();

}  // namespace game::lua
