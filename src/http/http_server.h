#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace http {

struct Request {
    std::string method;
    std::string path;
    std::string rawQuery;
    std::map<std::string, std::string> query;
    std::map<std::string, std::string> headers;  // keys lowercased
    std::map<std::string, std::string> params;   // from :name / {name} patterns
    std::string body;
    std::string remote;

    std::string queryValue(const std::string& key, const std::string& fallback = {}) const;
    std::string header(const std::string& key, const std::string& fallback = {}) const;
};

struct Response {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::map<std::string, std::string> headers;
    std::string body;

    void text(std::string value) { body = std::move(value); }
    void json(std::string value) {
        contentType = "application/json; charset=utf-8";
        body = std::move(value);
    }
};

using Handler = std::function<void(const Request&, Response&)>;

class Server {
public:
    ~Server();

    void route(std::string method, std::string pattern, Handler handler);
    void get(std::string pattern, Handler handler) { route("GET", std::move(pattern), std::move(handler)); }
    void post(std::string pattern, Handler handler) { route("POST", std::move(pattern), std::move(handler)); }
    // The SDK endpoints do not agree with themselves about GET vs POST between
    // versions and regions, and none of them care, so serve both.
    void any(std::string pattern, Handler handler) {
        get(pattern, handler);
        post(std::move(pattern), std::move(handler));
    }
    // Matches any path under prefix; used for the catch-all SDK stubs.
    void fallback(Handler handler) { fallback_ = std::move(handler); }

    bool start(const std::string& bind, uint16_t port);
    void stop();

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments;
        Handler handler;
    };

    void acceptLoop();
    void serve(uintptr_t socket, std::string remote);
    bool dispatch(Request& req, Response& res);

    std::vector<Route> routes_;
    Handler fallback_;
    uintptr_t listener_ = static_cast<uintptr_t>(-1);
    std::thread thread_;
    std::atomic<bool> running_{false};
};

std::string urlDecode(std::string_view s);

}  // namespace http
