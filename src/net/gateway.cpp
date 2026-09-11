#include "net/gateway.h"

#include <chrono>
#include <cstring>

#include "core/logger.h"
#include "core/util.h"

namespace net {
namespace {

constexpr size_t kMaxDatagram = 1500;
constexpr size_t kControlSize = 20;

constexpr uint32_t kConnect = 0x000000FF;
constexpr uint32_t kConnectAlt = 0xC84B145C;  // seen from some clients instead of 0xFF
constexpr uint32_t kConnectTail = 0xFFFFFFFF;
constexpr uint32_t kAccept = 0x00000145;
constexpr uint32_t kAcceptTail = 0x14514545;
constexpr uint32_t kDisconnect = 0x00000194;
constexpr uint32_t kDisconnectTail = 0x19419494;

uint32_t readBE32(const char* p) {
    const uint8_t* u = reinterpret_cast<const uint8_t*>(p);
    return (static_cast<uint32_t>(u[0]) << 24) | (static_cast<uint32_t>(u[1]) << 16) |
           (static_cast<uint32_t>(u[2]) << 8) | static_cast<uint32_t>(u[3]);
}

void writeBE32(char* p, uint32_t v) {
    p[0] = static_cast<char>((v >> 24) & 0xFF);
    p[1] = static_cast<char>((v >> 16) & 0xFF);
    p[2] = static_cast<char>((v >> 8) & 0xFF);
    p[3] = static_cast<char>(v & 0xFF);
}

uint32_t readLE32(const char* p) {
    const uint8_t* u = reinterpret_cast<const uint8_t*>(p);
    return static_cast<uint32_t>(u[0]) | (static_cast<uint32_t>(u[1]) << 8) |
           (static_cast<uint32_t>(u[2]) << 16) | (static_cast<uint32_t>(u[3]) << 24);
}

}  // namespace

Gateway::~Gateway() { stop(); }

bool Gateway::start(const std::string& bind, uint16_t port) {
    socket_ = udpBind(bind, port);
    if (socket_ == kInvalidSocket) {
        logging::error("game", "cannot bind udp {}:{}", bind, port);
        return false;
    }
    running_ = true;
    receiver_ = std::thread([this] { receiveLoop(); });
    updater_ = std::thread([this] { updateLoop(); });
    return true;
}

void Gateway::stop() {
    if (!running_.exchange(false)) return;
    closeSocket(socket_);
    socket_ = kInvalidSocket;
    if (receiver_.joinable()) receiver_.join();
    if (updater_.joinable()) updater_.join();
    std::lock_guard lock(mutex_);
    sessions_.clear();
}

bool Gateway::sendUdp(const char* data, size_t size, const UdpAddress& to) {
    if (socket_ == kInvalidSocket) return false;
    return udpSend(socket_, data, size, to);
}

std::shared_ptr<Session> Gateway::find(uint32_t conv) {
    std::lock_guard lock(mutex_);
    auto it = sessions_.find(conv);
    return it == sessions_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<Session>> Gateway::sessions() {
    std::lock_guard lock(mutex_);
    std::vector<std::shared_ptr<Session>> out;
    out.reserve(sessions_.size());
    for (const auto& [conv, session] : sessions_) {
        (void)conv;
        out.push_back(session);
    }
    return out;
}

void Gateway::drop(uint32_t conv, const char* why) {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        auto it = sessions_.find(conv);
        if (it == sessions_.end()) return;
        session = it->second;
        sessions_.erase(it);
    }
    session->close();
    sendControl(kDisconnect, session->conv(), session->token(), 1, kDisconnectTail,
                session->remote());
    logging::info("game", "conv {} from {} disconnected ({})", conv, session->remote().str(), why);
}

void Gateway::receiveLoop() {
    std::vector<char> buffer(kMaxDatagram);
    while (running_) {
        UdpAddress from;
        int n = udpRecv(socket_, buffer.data(), buffer.size(), &from);
        if (n <= 0) {
            if (!running_) break;
            continue;
        }
        size_t size = static_cast<size_t>(n);
        if (size == kControlSize) {
            handleControl(buffer.data(), size, from);
            continue;
        }
        if (size < 28) continue;

        uint32_t conv = readLE32(buffer.data());
        auto session = find(conv);
        if (!session) continue;
        if (!(session->remote() == from)) continue;
        session->onUdp(buffer.data(), size);
    }
}

void Gateway::updateLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint64_t now = util::nowMs();
        std::vector<std::shared_ptr<Session>> snapshot;
        {
            std::lock_guard lock(mutex_);
            snapshot.reserve(sessions_.size());
            for (auto& [conv, session] : sessions_) snapshot.push_back(session);
        }
        for (auto& session : snapshot) {
            session->update(static_cast<uint32_t>(now));
            if (session->expired(now)) drop(session->conv(), "idle timeout");
        }
    }
}

void Gateway::handleControl(const char* data, size_t size, const UdpAddress& from) {
    (void)size;
    uint32_t head = readBE32(data);
    uint32_t p1 = readBE32(data + 4);
    uint32_t p2 = readBE32(data + 8);
    uint32_t payload = readBE32(data + 12);
    uint32_t tail = readBE32(data + 16);

    if (head == kConnect || head == kConnectAlt) {
        (void)kConnectTail;
        accept(payload, from);
        return;
    }
    if (head == kDisconnect && tail == kDisconnectTail) {
        auto session = find(p1);
        if (session && session->token() == p2) drop(p1, "client sent a disconnect");
        return;
    }
    logging::warn("game", "unknown control packet {:#x}/{:#x} from {}", head, tail, from.str());
}

void Gateway::accept(uint32_t enet, const UdpAddress& from) {
    std::shared_ptr<Session> existing;
    {
        std::lock_guard lock(mutex_);
        for (auto& [conv, session] : sessions_) {
            if (session->remote() == from) {
                existing = session;
                break;
            }
        }
    }
    if (existing) {
        // A repeated handshake means the client restarted; give it a clean session.
        drop(existing->conv(), "the client handshook again");
    }

    uint32_t conv;
    uint32_t token = util::randomRange(1, 0x7FFFFFFF);
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(mutex_);
        conv = nextConv_++;
        session = std::make_shared<Session>(*this, conv, token, from);
        sessions_[conv] = session;
    }

    logging::info("game", "accepting handshake from {} conv={} token={} enet={}", from.str(), conv,
              token, enet);
    sendControl(kAccept, conv, token, enet, kAcceptTail, from);
}

void Gateway::sendControl(uint32_t head, uint32_t p1, uint32_t p2, uint32_t data, uint32_t tail,
                          const UdpAddress& to) {
    char buffer[kControlSize];
    writeBE32(buffer, head);
    writeBE32(buffer + 4, p1);
    writeBE32(buffer + 8, p2);
    writeBE32(buffer + 12, data);
    writeBE32(buffer + 16, tail);
    sendUdp(buffer, sizeof(buffer), to);
}

}  // namespace net
