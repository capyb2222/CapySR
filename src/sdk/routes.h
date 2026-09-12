#pragma once

#include "http/http_server.h"

namespace sdk {

void registerRoutes(http::Server& server);

// The client version the last gateway query named, such as "CNBETAWin4.5.54"; empty
// until one has. The game connection that follows carries nothing that says.
std::string lastClientVersion();

}  // namespace sdk
