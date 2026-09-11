#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/logger.h"
#include "net/packet.h"
#include "net/session.h"

namespace net {

using HandlerFn = std::function<void(Session&, const Packet&)>;

class Handlers {
public:
    static Handlers& get();

    void add(uint16_t cmdId, HandlerFn fn);
    // Replies with an empty ScRsp; enough for the many "give me your data" packets
    // the client sends at login but does not act on.
    void addEmpty(uint16_t reqId);
    // For requests whose response the dump never named: the automatic reply derives
    // the response from the request name, and without a mapping it can send nothing
    // at all, which is the one thing that hangs the client.
    void addAlias(uint16_t reqId, uint16_t rspId);
    void muteLog(uint16_t cmdId);

    const HandlerFn* find(uint16_t cmdId) const;
    bool isEmptyReply(uint16_t cmdId) const;
    // 0 when the response id has to be derived from the name instead.
    uint16_t aliasFor(uint16_t reqId) const;
    bool muted(uint16_t cmdId) const;
    size_t count() const { return handlers_.size(); }
    size_t emptyCount() const { return empties_.size(); }

private:
    std::unordered_map<uint16_t, HandlerFn> handlers_;
    std::unordered_set<uint16_t> empties_;
    std::unordered_map<uint16_t, uint16_t> aliases_;
    std::unordered_set<uint16_t> muted_;
};

// Registers a handler that decodes Req first; a malformed body is logged and dropped.
template <class Req, class Fn>
void on(uint16_t cmdId, Fn fn) {
    Handlers::get().add(cmdId, [fn, cmdId](Session& session, const Packet& packet) {
        Req req;
        if (!req.parse(reinterpret_cast<const uint8_t*>(packet.body.data()), packet.body.size())) {
            logging::warn("net", "malformed {}", Req::kName);
            return;
        }
        fn(session, req);
    });
}

inline void onRaw(uint16_t cmdId, HandlerFn fn) { Handlers::get().add(cmdId, std::move(fn)); }

// Answers with an otherwise-default Rsp whose sub-messages are all present. The client
// reads them without a null check, so an actually-empty body makes it throw inside its
// own module -- see the `fill` comment in the generated header.
template <class Rsp>
void onFilled(uint16_t reqId, uint16_t rspId, int depth = 2) {
    Handlers::get().add(reqId, [rspId, depth](Session& session, const Packet&) {
        Rsp rsp;
        rsp.fill(depth);
        session.sendRaw(rspId, rsp.serialize());
    });
}

// Deliberately says nothing at all, which also stops the automatic empty reply.
// For a module we do not implement this is safer than any body: the client's response
// handler never runs, so it can neither throw on a missing sub-message nor choke on a
// shape we guessed at. It waits, and this client does not block on that.
inline void onSilent(uint16_t reqId) {
    Handlers::get().add(reqId, [](Session&, const Packet&) {});
}

}  // namespace net
