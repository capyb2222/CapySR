#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "net/packet.h"
#include "net/socket.h"

struct IKCPCB;

namespace game {
class Player;
}

namespace net {

class Gateway;

enum class SessionState { WaitingForToken, WaitingForLogin, Active, Closed };

// Both timestamps are unsigned, and the receive thread can move `last` past the `now`
// the updater captured a moment earlier -- so subtracting first underflows to ~1.8e19
// and every session looks idle. Compare before subtracting.
inline bool isIdle(uint64_t now, uint64_t last, uint64_t timeoutMs) {
    return now > last && now - last > timeoutMs;
}

class Session {
public:
    Session(Gateway& gateway, uint32_t conv, uint32_t token, UdpAddress remote);
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    uint32_t conv() const { return conv_; }
    uint32_t token() const { return token_; }
    const UdpAddress& remote() const { return remote_; }
    SessionState state() const { return state_; }
    void setState(SessionState s) { state_ = s; }
    game::Player* player() { return player_.get(); }
    void setPlayer(std::shared_ptr<game::Player> player) { player_ = std::move(player); }

    // Called from the gateway receive thread with raw UDP payload.
    void onUdp(const char* data, size_t size);
    void update(uint32_t nowMs);
    bool expired(uint64_t nowMs) const;
    void close();

    void sendRaw(uint16_t cmdId, std::string_view body);
    void sendEmpty(uint16_t cmdId);

    template <class T>
    void send(uint16_t cmdId, const T& message) {
        sendRaw(cmdId, message.serialize());
    }

private:
    void flushOutbound();
    void flushEmptyReplies();
    static int kcpOutput(const char* buf, int len, IKCPCB* kcp, void* user);
    void dispatch(const Packet& packet);

    Gateway& gateway_;
    uint32_t conv_;
    uint32_t token_;
    UdpAddress remote_;
    IKCPCB* kcp_ = nullptr;
    SessionState state_ = SessionState::WaitingForToken;
    std::shared_ptr<game::Player> player_;
    std::recursive_mutex mutex_;
    std::vector<char> recvBuffer_;
    // Written by the receive thread, read by the updater thread.
    std::atomic<uint64_t> lastActivity_{0};
    // Unimplemented requests answered empty since the last summary line; under mutex_.
    std::vector<std::string> emptyReplies_;
    uint32_t emptyReplyCount_ = 0;
    uint64_t lastEmptyReplyMs_ = 0;
};

}  // namespace net
