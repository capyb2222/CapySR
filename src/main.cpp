#include <atomic>
#include <chrono>
#include <csignal>
#include <string>
#include <thread>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "data/excel.h"
#include "data/scene_res.h"
#include "game/handlers.h"
#include "game/srtools.h"
#include "http/http_server.h"
#include "net/gateway.h"
#include "sdk/admin.h"
#include "sdk/routes.h"

namespace {

std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

}  // namespace

int main(int argc, char** argv) {
    std::string configPath = "config/config.json";
    bool explicitConfig = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-c" || arg == "--config") && i + 1 < argc) {
            configPath = argv[++i];
            explicitConfig = true;
        }
    }

    // Every path in the config is relative to the repo root, so move there unless the
    // caller named a config themselves. Without this, launching build\bin\capysr.exe
    // directly starts a server with no config, no tables and no scenes.
    std::string root;
    if (!explicitConfig) {
        root = util::findRootDir(configPath);
        if (!root.empty()) util::setCurrentDir(root);
    }

    auto& config = core::Config::get();
    config.load(configPath);
    logging::init(logging::parseLevel(config.log.level), config.log.color, config.log.file);

    logging::info("capysr", "CapySR starting ({})", config.serverName);
    if (!root.empty()) logging::debug("capysr", "working directory: {}", root);

    data::Tables::get().load(config.paths.dataSources);
    data::SceneRes::get().load(config.paths.sceneRes);
    data::SceneRes::get().loadAnchors(config.paths.anchors);
    game::SrTools::instance().loadFromDisk();
    game::registerAllHandlers();

    // Say plainly whether the client will be able to get in world, because every
    // symptom of a missing data source looks like a broken server from the client.
    if (!data::Tables::get().loaded() || !data::SceneRes::get().loaded()) {
        logging::error("capysr",
                       "{} missing -- the client can log in but not enter the world. "
                       "Fix the paths in {} (run from the repo root).",
                       !data::Tables::get().loaded() ? "game tables" : "scene dump", configPath);
    }

    net::Gateway gateway;

    http::Server web;
    sdk::registerRoutes(web);
    sdk::registerAdminRoutes(web, gateway);
    if (!web.start(config.http.bind, config.http.port)) return 1;
    logging::info("capysr", "dispatch listening on {}:{}", config.http.bind, config.http.port);

    if (!gateway.start(config.game.bind, config.game.port)) return 1;
    logging::info("capysr", "game listening on {}:{} (udp/kcp)", config.game.bind, config.game.port);
    logging::info("capysr", "black screen? http://{}:{}/unstick", config.http.publicHost,
                  config.http.port);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    while (g_running) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    logging::info("capysr", "shutting down");
    gateway.stop();
    web.stop();
    logging::shutdown();
    return 0;
}
