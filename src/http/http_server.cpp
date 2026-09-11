#include "http/http_server.h"

#include <algorithm>
#include <cstring>

#include "core/logger.h"
#include "core/util.h"
#include "net/socket.h"

namespace http {
namespace {

constexpr size_t kMaxHeader = 16 * 1024;
constexpr size_t kMaxBody = 16 * 1024 * 1024;

const char* statusText(int code) {
    switch (code) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        default: return "OK";
    }
}

std::vector<std::string> splitPath(std::string_view path) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < path.size()) {
        while (i < path.size() && path[i] == '/') ++i;
        size_t start = i;
        while (i < path.size() && path[i] != '/') ++i;
        if (i > start) out.emplace_back(path.substr(start, i - start));
    }
    return out;
}

bool isCapture(const std::string& seg) {
    return (!seg.empty() && seg[0] == ':') ||
           (seg.size() >= 2 && seg.front() == '{' && seg.back() == '}');
}

std::string captureName(const std::string& seg) {
    return seg[0] == ':' ? seg.substr(1) : seg.substr(1, seg.size() - 2);
}

}  // namespace

std::string urlDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(s[i + 1]), lo = hex(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
            out += s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

std::string Request::queryValue(const std::string& key, const std::string& fallback) const {
    auto it = query.find(key);
    return it == query.end() ? fallback : it->second;
}

std::string Request::header(const std::string& key, const std::string& fallback) const {
    auto it = headers.find(util::toLower(key));
    return it == headers.end() ? fallback : it->second;
}

Server::~Server() { stop(); }

void Server::route(std::string method, std::string pattern, Handler handler) {
    routes_.push_back({std::move(method), splitPath(pattern), std::move(handler)});
}

bool Server::start(const std::string& bind, uint16_t port) {
    net::initSockets();
    listener_ = net::tcpListen(bind, port);
    if (listener_ == net::kInvalidSocket) {
        logging::error("http", "cannot bind {}:{}", bind, port);
        return false;
    }
    running_ = true;
    thread_ = std::thread([this] { acceptLoop(); });
    return true;
}

void Server::stop() {
    if (!running_.exchange(false)) return;
    net::closeSocket(listener_);
    listener_ = net::kInvalidSocket;
    if (thread_.joinable()) thread_.join();
}

void Server::acceptLoop() {
    while (running_) {
        std::string remote;
        uintptr_t client = net::tcpAccept(listener_, &remote);
        if (client == net::kInvalidSocket) {
            if (!running_) break;
            continue;
        }
        std::thread([this, client, remote] {
            try {
                serve(client, remote);
            } catch (const std::exception& e) {
                logging::error("http", "connection crashed: {}", e.what());
            }
            net::closeSocket(client);
        }).detach();
    }
}

void Server::serve(uintptr_t socket, std::string remote) {
    std::string buffer;
    net::setRecvTimeout(socket, 30000);

    while (running_) {
        size_t headerEnd = buffer.find("\r\n\r\n");
        while (headerEnd == std::string::npos) {
            char chunk[8192];
            int n = net::tcpRecv(socket, chunk, sizeof(chunk));
            if (n <= 0) return;
            buffer.append(chunk, static_cast<size_t>(n));
            if (buffer.size() > kMaxHeader) return;
            headerEnd = buffer.find("\r\n\r\n");
        }

        Request req;
        req.remote = remote;
        std::string_view head(buffer.data(), headerEnd);
        auto lines = util::split(head, '\n');
        if (lines.empty()) return;

        auto first = util::split(util::trim(lines[0]), ' ');
        if (first.size() < 2) return;
        req.method = std::string(first[0]);
        std::string target(first[1]);
        if (size_t q = target.find('?'); q != std::string::npos) {
            req.rawQuery = target.substr(q + 1);
            target = target.substr(0, q);
            for (auto pair : util::split(req.rawQuery, '&')) {
                if (pair.empty()) continue;
                size_t eq = pair.find('=');
                if (eq == std::string_view::npos) {
                    req.query[urlDecode(pair)] = "";
                } else {
                    req.query[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
                }
            }
        }
        req.path = urlDecode(target);

        for (size_t i = 1; i < lines.size(); ++i) {
            std::string_view line = util::trim(lines[i]);
            size_t colon = line.find(':');
            if (colon == std::string_view::npos) continue;
            req.headers[util::toLower(util::trim(line.substr(0, colon)))] =
                std::string(util::trim(line.substr(colon + 1)));
        }

        size_t bodyLen = 0;
        if (auto it = req.headers.find("content-length"); it != req.headers.end()) {
            bodyLen = static_cast<size_t>(std::strtoull(it->second.c_str(), nullptr, 10));
        }
        if (bodyLen > kMaxBody) return;

        size_t total = headerEnd + 4 + bodyLen;
        while (buffer.size() < total) {
            char chunk[16384];
            int n = net::tcpRecv(socket, chunk, sizeof(chunk));
            if (n <= 0) return;
            buffer.append(chunk, static_cast<size_t>(n));
        }
        req.body = buffer.substr(headerEnd + 4, bodyLen);
        buffer.erase(0, total);

        Response res;
        bool handled = dispatch(req, res);
        if (!handled) {
            res.status = 404;
            res.body = "not found";
        }

        std::string out;
        out.reserve(res.body.size() + 256);
        out += "HTTP/1.1 " + std::to_string(res.status) + " " + statusText(res.status) + "\r\n";
        out += "Content-Type: " + res.contentType + "\r\n";
        out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
        for (const auto& [k, v] : res.headers) out += k + ": " + v + "\r\n";
        bool keepAlive = util::toLower(req.header("connection", "keep-alive")) != "close";
        out += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
        out += "\r\n";
        out += res.body;

        if (!net::tcpSendAll(socket, out.data(), out.size())) return;
        if (!keepAlive) return;
    }
}

bool Server::dispatch(Request& req, Response& res) {
    auto segments = splitPath(req.path);
    const Route* match = nullptr;
    bool pathSeen = false;

    for (const auto& route : routes_) {
        if (route.segments.size() != segments.size()) continue;
        std::map<std::string, std::string> params;
        bool ok = true;
        for (size_t i = 0; i < segments.size(); ++i) {
            const std::string& pat = route.segments[i];
            if (isCapture(pat)) {
                params[captureName(pat)] = segments[i];
            } else if (pat != segments[i]) {
                ok = false;
                break;
            }
        }
        if (!ok) continue;
        pathSeen = true;
        if (route.method != req.method) continue;
        req.params = std::move(params);
        match = &route;
        break;
    }

    if (match) {
        try {
            match->handler(req, res);
        } catch (const std::exception& e) {
            logging::error("http", "{} {} failed: {}", req.method, req.path, e.what());
            res.status = 500;
            res.body = "internal error";
        }
        return true;
    }

    // A 405 is correct HTTP and useless here: the client's SDK treats it as a hard
    // failure and throws, so a known path with an unexpected verb falls through to
    // the catch-all rather than breaking login.
    if (pathSeen && !fallback_) {
        res.status = 405;
        res.body = "method not allowed";
        return true;
    }
    if (pathSeen) {
        logging::debug("http", "{} {} has no handler for that verb, stubbing", req.method,
                       req.path);
    }
    if (fallback_) {
        fallback_(req, res);
        return true;
    }
    return false;
}

}  // namespace http
