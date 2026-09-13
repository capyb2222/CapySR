#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "net/session.h"
#include "net/socket.h"

namespace net {

class Gateway {
public:
    ~Gateway();

    bool start(const std::string& bind, uint16_t port);
    void stop();

    bool sendUdp(const char* data, size_t size, const UdpAddress& to);
    // `why` is logged: knowing whether the client hung up, timed out or handshook
    // again is most of the diagnosis when a session ends unexpectedly.
    void drop(uint32_t conv, const char* why);
    // A restarted client comes from a new port, so its old session outlives it; this drops
    // that one before the new login reads the save it would otherwise overwrite later.
    void dropOthers(const Session& keep, uint32_t uid);
    std::shared_ptr<Session> find(uint32_t conv);
    // Every session still connected, in conv order. The admin routes act on these.
    std::vector<std::shared_ptr<Session>> sessions();

private:
    void receiveLoop();
    void updateLoop();
    void handleControl(const char* data, size_t size, const UdpAddress& from);
    void accept(uint32_t enet, const UdpAddress& from);
    void sendControl(uint32_t head, uint32_t p1, uint32_t p2, uint32_t data, uint32_t tail,
                     const UdpAddress& to);

    uintptr_t socket_ = kInvalidSocket;
    std::thread receiver_;
    std::thread updater_;
    std::atomic<bool> running_{false};
    std::mutex mutex_;
    std::map<uint32_t, std::shared_ptr<Session>> sessions_;
    uint32_t nextConv_ = 1;
};

}  // namespace net
