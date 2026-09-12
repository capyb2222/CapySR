#include "sdk/routes.h"

#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "game/srtools.h"
#include "proto/gen/protos.h"
#include "sdk/hotfix.h"

using json = nlohmann::json;

namespace sdk {
namespace {

std::mutex versionMutex;
std::string lastVersion;

json accountBlock(const std::string& uid, const std::string& token) {
    return json{{"uid", uid},
                {"name", ""},
                {"email", "capybara@capysr.local"},
                {"mobile", ""},
                {"is_email_verify", "1"},
                {"realname", ""},
                {"identity_card", ""},
                {"token", token},
                {"safe_mobile", ""},
                {"facebook_name", ""},
                {"google_name", ""},
                {"twitter_name", ""},
                {"game_center_name", ""},
                {"apple_name", ""},
                {"sony_name", ""},
                {"tap_name", ""},
                {"country", "US"},
                {"reactivate_ticket", ""},
                {"area_code", "**"},
                {"device_grant_ticket", ""},
                {"steam_name", ""},
                {"unmasked_email", "capybara@capysr.local"},
                {"unmasked_email_type", 1},
                {"cx_name", ""}};
}

void ok(http::Response& res, json data) {
    res.json(json{{"retcode", 0}, {"message", "OK"}, {"data", std::move(data)}}.dump());
}

std::string dispatchBody() {
    const auto& cfg = core::Config::get();
    proto::Dispatch dispatch;
    dispatch.retcode = 0;
    auto& region = dispatch.region_list.emplace_back();
    region.name = cfg.serverName;
    region.display_name = cfg.serverName;
    region.title = cfg.serverName;
    region.env_type = "9";
    region.dispatch_url = "http://" + cfg.http.publicHost + ":" +
                          std::to_string(cfg.http.port) + "/query_gateway";
    return util::base64Encode(dispatch.serialize());
}

std::string gatewayBody(const std::string& version) {
    const auto& cfg = core::Config::get();
    proto::GateServer gate;
    gate.retcode = 0;
    gate.ip = cfg.game.publicHost;
    gate.port = cfg.game.port;
    gate.region_name = cfg.serverName;
    gate.server_description = cfg.serverName;
    gate.use_tcp = false;

    if (cfg.hotfix.sendResourceUrls) {
        HotfixUrls urls = lookupHotfix(version);
        gate.asset_bundle_url = urls.assetBundleUrl;
        gate.ex_resource_url = urls.exResourceUrl;
        gate.lua_url = urls.luaUrl;
        gate.ifix_url = urls.ifixUrl;
        // As the official gateway does. Without the lua revision the client keeps running
        // its base-package Lua even with the hotfix downloaded.
        gate.lua_version = urls.luaVersion;
        gate.i_fix_patch_revision = urls.ifixVersion;
        // Misnamed like 552: the official gateway sends the Windows asset url here as well,
        // and without it the client never ran its full update (asb, audio, lua).
        gate.asset_bundle_url_android = urls.assetBundleUrl;
        if (!urls.empty()) {
            logging::info("dispatch", "hotfix for {}: {}", version, urls.exResourceUrl);
        }
    }
    // Leaving these off keeps a packaged client from re-downloading design data it
    // already ships, which is what hangs the loading bar when the CDN is unreachable.
    gate.enable_design_data_bundle_version_update = cfg.hotfix.enableDesignDataUpdate;
    gate.enable_video_bundle_version_update = cfg.hotfix.enableVideoUpdate;
    // The rest of the official reply's switches, bar the watermark (1660) and the
    // packet-encryption key (1569).
    gate.network_diagnostic = true;
    gate.event_tracking_open = true;
    gate.android_middle_package_enable = true;
    gate.close_redeem_code = true;
    return util::base64Encode(gate.serialize());
}

}  // namespace

std::string lastClientVersion() {
    std::lock_guard<std::mutex> lock(versionMutex);
    return lastVersion;
}

void registerRoutes(http::Server& server) {
    server.get("/query_dispatch", [](const http::Request&, http::Response& res) {
        res.text(dispatchBody());
    });

    server.get("/query_gateway", [](const http::Request& req, http::Response& res) {
        std::string version = req.queryValue("version");
        {
            std::lock_guard<std::mutex> lock(versionMutex);
            lastVersion = version;
        }
        logging::info("dispatch", "query_gateway version={} seed={}", version,
                  req.queryValue("dispatch_seed"));
        res.text(gatewayBody(version));
    });

    // ---- account / sdk ----
    auto shieldLogin = [](const http::Request&, http::Response& res) {
        ok(res, json{{"account", accountBlock("1337", "capysr-token")},
                     {"device_grant_required", false},
                     {"safe_moblie_required", false},
                     {"realperson_required", false},
                     {"reactivate_required", false},
                     {"realname_operation", "None"}});
    };
    server.any("/:product/mdk/shield/api/login", shieldLogin);
    server.any("/:product/mdk/shield/api/verify", [](const http::Request& req, http::Response& res) {
        std::string uid = "1337";
        std::string token = "capysr-token";
        try {
            json body = json::parse(req.body);
            if (body.contains("uid")) uid = body["uid"].is_string()
                                                ? body["uid"].get<std::string>()
                                                : std::to_string(body["uid"].get<int64_t>());
            if (body.contains("token")) token = body["token"].get<std::string>();
        } catch (const std::exception&) {
        }
        ok(res, json{{"account", accountBlock(uid, token)},
                     {"device_grant_required", false},
                     {"safe_moblie_required", false},
                     {"realperson_required", false},
                     {"reactivate_required", false}});
    });

    server.any("/:product/combo/granter/login/v2/login", [](const http::Request&, http::Response& res) {
        ok(res, json{{"combo_id", "1337"},
                     {"open_id", "1337"},
                     {"combo_token", "capysr-combo-token"},
                     {"data", "{\"guest\":false}"},
                     {"heartbeat", false},
                     {"account_type", 1},
                     {"fatigue_remind", nullptr}});
    });

    server.any("/account/risky/api/check", [](const http::Request&, http::Response& res) {
        ok(res, json{{"id", util::randomHex(16)}, {"action", "ACTION_NONE"}, {"geetest", nullptr}});
    });

    server.any("/account/ma-cn-passport/app/loginByPassword",
                [](const http::Request&, http::Response& res) {
                    ok(res, json{{"token", {{"token", "capysr-token"}, {"token_type", 1}}},
                                 {"user_info",
                                  {{"aid", "1337"},
                                   {"mid", "1337"},
                                   {"account_name", "capybara"},
                                   {"area_code", "**"},
                                   {"email", "capybara@capysr.local"},
                                   {"is_email_verify", "1"},
                                   {"country", "US"}}},
                                 {"realname_info", nullptr},
                                 {"need_realperson", false}});
                });

    server.any("/account/ma-cn-session/app/verify", [](const http::Request&, http::Response& res) {
        ok(res, json{{"tokens", {{{"token", "capysr-token"}, {"token_type", 1}}}},
                     {"user_info",
                      {{"aid", "1337"},
                       {"mid", "1337"},
                       {"area_code", "**"},
                       {"email", "capybara@capysr.local"},
                       {"is_email_verify", "1"},
                       {"country", "US"}}}});
    });

    server.any("/:product/combo/granter/api/getConfig", [](const http::Request&, http::Response& res) {
        ok(res, json{{"protocol", true},
                     {"qr_enabled", false},
                     {"log_level", "INFO"},
                     {"announce_url", ""},
                     {"push_alias_type", 0},
                     {"disable_ysdk_guard", true},
                     {"enable_announce_pic_popup", false},
                     {"app_name", "CapySR"},
                     {"enable_user_center", false}});
    });

    server.any("/:product/mdk/shield/api/loadConfig", [](const http::Request&, http::Response& res) {
        ok(res, json{{"id", 24},
                     {"game_key", "hkrpg_global"},
                     {"client", "PC"},
                     {"identity", "I_IDENTITY"},
                     {"guest", false},
                     {"ignore_versions", ""},
                     {"scene", "S_NORMAL"},
                     {"name", "CapySR"},
                     {"disable_regist", false},
                     {"enable_email_captcha", false},
                     {"thirdparty", json::array()},
                     {"disable_mmt", false},
                     {"server_guest", false},
                     {"thirdparty_ignore", json::object()},
                     {"enable_ps_bind_account", false},
                     {"thirdparty_login_configs", json::object()},
                     {"initialize_firebase", false},
                     {"bbs_auth_login", false},
                     {"fetch_instance_id", false},
                     {"enable_flash_login", false}});
    });

    server.any("/device-fp/api/getFp", [](const http::Request&, http::Response& res) {
        ok(res, json{{"device_fp", util::randomHex(8)}, {"code", 200}, {"msg", "ok"}});
    });
    server.any("/device-fp/api/getExtList", [](const http::Request&, http::Response& res) {
        ok(res, json{{"code", 200}, {"msg", "ok"}, {"ext_list", json::array()}});
    });

    // ---- srtools ----
    server.post("/srtools", [](const http::Request& req, http::Response& res) {
        std::string message = game::SrTools::instance().upload(req.body);
        bool good = message == "OK";
        res.json(json{{"message", message}, {"status", good ? 200 : 500}}.dump());
        if (good) {
            logging::info("srtools", "received a build from {}", req.remote);
        } else {
            logging::warn("srtools", "rejected a build: {}", message);
        }
    });

    // Everything else the sdk pokes at is happy with a bare success.
    server.fallback([](const http::Request& req, http::Response& res) {
        logging::debug("http", "stubbed {} {}", req.method, req.path);
        ok(res, json::object());
    });
}

}  // namespace sdk
