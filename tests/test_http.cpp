// Drives the dispatch/SDK server over a real socket with the exact requests a client
// makes. The SDK is not verb-consistent, and a 405 here throws inside the client's
// SDK and bounces it back to the login screen with an empty dialog.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/util.h"
#include "http/http_server.h"
#include "net/socket.h"
#include "proto/gen/protos.h"
#include "sdk/routes.h"
#include "tests/harness.h"

namespace {

using testing::check;

constexpr uint16_t kTestPort = 21099;

// Minimal HTTP/1.0 client: sends one request, returns the whole raw reply.
std::string request(uint16_t port, const std::string& method, const std::string& target,
                    const std::string& body = {}) {
    net::initSockets();
    uintptr_t sock = net::tcpConnect("127.0.0.1", port);
    if (sock == net::kInvalidSocket) return {};

    std::string req = method + " " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n" + body;
    net::tcpSendAll(sock, req.data(), req.size());

    std::string reply;
    char buffer[4096];
    while (true) {
        int n = net::tcpRecv(sock, buffer, sizeof(buffer));
        if (n <= 0) break;
        reply.append(buffer, static_cast<size_t>(n));
    }
    net::closeSocket(sock);
    return reply;
}

int statusOf(const std::string& reply) {
    // "HTTP/1.1 200 OK"
    size_t space = reply.find(' ');
    if (space == std::string::npos) return 0;
    return std::atoi(reply.c_str() + space + 1);
}

bool succeeded(const std::string& reply) {
    return statusOf(reply) == 200 && reply.find("\"retcode\":0") != std::string::npos;
}

}  // namespace

void runHttpTests() {
    http::Server server;
    sdk::registerRoutes(server);
    if (!server.start("127.0.0.1", kTestPort)) {
        std::printf("SKIP http tests (tcp %d is busy)\n", kTestPort);
        return;
    }

    // The three the client actually asked for, with the query strings from its log.
    // It GETs all of them, which used to collide with POST-only routes and answer 405.
    // Each is checked for a field only its real handler produces, so falling through
    // to the catch-all stub does not pass: the client needs the actual config back.
    struct Endpoint {
        const char* target;
        const char* marker;
    };
    const Endpoint kClientGets[] = {
        {"/device-fp/api/getExtList?platform=3", "ext_list"},
        {"/hkrpg_cn/combo/granter/api/getConfig?app_id=8&channel_id=1&client_type=3", "app_name"},
        {"/hkrpg_cn/mdk/shield/api/loadConfig?client=3&game_key=hkrpg_cn", "game_key"},
    };
    for (const Endpoint& e : kClientGets) {
        for (const char* method : {"GET", "POST"}) {
            std::string reply = request(kTestPort, method, e.target, "{}");
            bool ok = succeeded(reply) && reply.find(e.marker) != std::string::npos;
            check(ok, "the client's sdk call gets its real answer on either verb");
            if (!ok) {
                std::printf("      %s %s -> %d (wanted \"%s\")\n", method, e.target,
                            statusOf(reply), e.marker);
            }
        }
    }

    // No SDK path may ever answer 405: the client's SDK throws on it rather than
    // handling it, which shows an empty dialog and loops back to login.
    const char* kProbes[] = {"/hkrpg_cn/mdk/shield/api/login",
                             "/hkrpg_cn/mdk/shield/api/verify",
                             "/hkrpg_cn/combo/granter/login/v2/login",
                             "/account/risky/api/check",
                             "/account/ma-cn-passport/app/loginByPassword",
                             "/account/ma-cn-session/app/verify",
                             "/device-fp/api/getFp",
                             "/some/endpoint/we/never/heard/of"};
    for (const char* target : kProbes) {
        for (const char* method : {"GET", "POST"}) {
            std::string reply = request(kTestPort, method, target, "{}");
            bool ok = succeeded(reply);
            check(ok, "every sdk path answers success on any verb");
            if (!ok) std::printf("      %s %s -> %d\n", method, target, statusOf(reply));
        }
    }

    // Dispatch itself: base64 protobuf, and the gateway carries the hotfix urls.
    std::string dispatch = request(kTestPort, "GET", "/query_dispatch");
    check(statusOf(dispatch) == 200, "query_dispatch answers");
    check(dispatch.find("\r\n\r\n") != std::string::npos && dispatch.size() > 40,
          "and returns a body");

    std::string gateway = request(
        kTestPort, "GET", "/query_gateway?version=CNBETAWin4.5.53&language_type=3&platform_type=3");
    check(statusOf(gateway) == 200, "query_gateway answers");
    size_t split = gateway.find("\r\n\r\n");
    std::string encoded = split == std::string::npos ? "" : gateway.substr(split + 4);
    std::string decoded = util::base64Decode(encoded);
    check(!decoded.empty(), "the gateway reply is base64");
    // An empty ex_resource_url stalls the client at ~99% before it opens a session.
    check(decoded.find("autopatch") != std::string::npos,
          "and carries hotfix urls for the client version");
    check(decoded.find("127.0.0.1") != std::string::npos, "and the game server address");
    proto::GateServer gate;
    check(gate.parse(reinterpret_cast<const uint8_t*>(decoded.data()), decoded.size()),
          "the gateway reply parses");
    // Without the lua revision the client keeps its base-package Lua.
    check(gate.lua_version == "16399471", "and names the lua revision, from the lua url");
    check(gate.i_fix_patch_revision == "0", "and the ifix revision");
    check(!gate.asset_bundle_url_android.empty() &&
              gate.asset_bundle_url_android == gate.asset_bundle_url,
          "and the asset url again in 1335, as the official gateway does");

    server.stop();
}
