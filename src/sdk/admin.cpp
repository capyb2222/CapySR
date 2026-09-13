#include "sdk/admin.h"

#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/logger.h"
#include "game/rescue.h"
#include "http/http_server.h"
#include "net/gateway.h"
#include "net/session.h"

using json = nlohmann::json;

namespace sdk {

void registerAdminRoutes(http::Server& server, net::Gateway& gateway) {
    // GET /unstick            -- push the current scene again
    // GET /unstick?entry=1000002 -- and move to the Astral Express while doing it,
    //                            or move the save there when nothing is connected
    //
    // The client has no timeout on a transition it started, so a scene or a battle the
    // server never finished leaves it looking at nothing forever. Pushing a scene is
    // what makes it let go and render again, and it is a lot faster than a restart.
    server.any("/unstick", [&gateway](const http::Request& req, http::Response& res) {
        uint32_t entryId = 0;
        std::string wanted = req.queryValue("entry");
        if (!wanted.empty()) entryId = std::strtoul(wanted.c_str(), nullptr, 10);

        json out;
        out["sessions"] = json::array();
        for (const std::shared_ptr<net::Session>& session : gateway.sessions()) {
            if (session == nullptr) continue;
            // Held like a packet handler: the receive thread may be busy with this player.
            out["sessions"].push_back(
                session->locked([&] { return game::rescue::unstick(*session, entryId); }));
        }
        if (out["sessions"].empty() && entryId != 0) {
            // Nobody is connected, so move the saved spot instead. A client that cannot
            // finish loading the floor it was left on has no session to rescue, and this
            // is the only way out of it short of editing the save by hand.
            out["message"] = game::rescue::relocate(entryId);
        } else if (out["sessions"].empty()) {
            logging::warn("rescue", "nothing to unstick -- no client is connected");
            out["message"] = "no client is connected";
        } else {
            out["message"] = "ok";
        }
        res.json(out.dump());
    });
}

}  // namespace sdk
