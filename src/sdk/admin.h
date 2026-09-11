#pragma once

namespace http {
class Server;
}
namespace net {
class Gateway;
}

namespace sdk {

// Routes that act on the live session rather than serving the client. Kept apart from
// the SDK routes because these are for whoever is running the server, not for the game.
void registerAdminRoutes(http::Server& server, net::Gateway& gateway);

}  // namespace sdk
