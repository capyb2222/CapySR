#include "net/session.h"

#include <algorithm>
#include <format>

#include <ikcp.h>

#include "core/config.h"
#include "core/logger.h"
#include "core/util.h"
#include "game/player.h"
#include "net/cmd_ids.h"
#include "net/gateway.h"
#include "net/handler.h"

namespace net {
namespace {

constexpr uint64_t kIdleTimeoutMs = 60000;
constexpr uint64_t kEmptyReplyQuietMs = 1000;

}  // namespace

Session::Session(Gateway& gateway, uint32_t conv, uint32_t token, UdpAddress remote)
    : gateway_(gateway), conv_(conv), token_(token), remote_(remote) {
    kcp_ = ikcp_create(conv, this);
    ikcp_settoken(kcp_, token);
    ikcp_setoutput(kcp_, &Session::kcpOutput);
    ikcp_nodelay(kcp_, 1, 10, 2, 1);
    ikcp_wndsize(kcp_, 256, 256);
    ikcp_setmtu(kcp_, kKcpMtu);
    recvBuffer_.resize(1 << 20);
    lastActivity_ = util::nowMs();
}

Session::~Session() {
    if (kcp_) ikcp_release(kcp_);
}

int Session::kcpOutput(const char* buf, int len, IKCPCB* kcp, void* user) {
    (void)kcp;
    auto* self = static_cast<Session*>(user);
    self->gateway_.sendUdp(buf, static_cast<size_t>(len), self->remote_);
    return 0;
}

void Session::onUdp(const char* data, size_t size) {
    std::lock_guard lock(mutex_);
    if (state_ == SessionState::Closed) return;
    if (ikcp_input(kcp_, data, static_cast<long>(size)) < 0) {
        logging::debug("net", "conv {} rejected a kcp segment", conv_);
        return;
    }
    lastActivity_ = util::nowMs();

    while (true) {
        int peek = ikcp_peeksize(kcp_);
        if (peek <= 0) break;
        if (static_cast<size_t>(peek) > recvBuffer_.size()) recvBuffer_.resize(peek);
        int n = ikcp_recv(kcp_, recvBuffer_.data(), static_cast<int>(recvBuffer_.size()));
        if (n <= 0) break;

        std::vector<Packet> packets;
        std::string error;
        if (!decodePackets(std::string_view(recvBuffer_.data(), static_cast<size_t>(n)), packets,
                           &error)) {
            logging::warn("net", "conv {} sent a bad packet ({})", conv_, error);
            continue;
        }
        for (const auto& packet : packets) dispatch(packet);
    }
    flushOutbound();
}

void Session::dispatch(const Packet& packet) {
    auto& handlers = Handlers::get();
    bool logIt = core::Config::get().log.packets && !handlers.muted(packet.cmdId);
    if (logIt) {
        logging::debug("recv", "{} ({}) {} bytes", cmd::name(packet.cmdId), packet.cmdId,
                   packet.body.size());
        if (core::Config::get().log.packetBodies && !packet.body.empty()) {
            logging::debug("recv", "  {}", util::hexDump(packet.body));
        }
    }

    if (const HandlerFn* fn = handlers.find(packet.cmdId)) {
        try {
            (*fn)(*this, packet);
        } catch (const std::exception& e) {
            logging::error("net", "{} handler threw: {}", cmd::name(packet.cmdId), e.what());
        }
        return;
    }

    // Unhandled request: answer with an empty response of the matching name so the
    // client's pending request completes instead of hanging.
    std::string name = cmd::name(packet.cmdId);
    size_t pos = name.find("CsReq");
    if (pos == std::string::npos) {
        if (logIt) logging::debug("net", "ignored {}", name);
        return;
    }
    // An explicit mapping wins: some responses were never named after their request.
    uint16_t rspId = handlers.aliasFor(packet.cmdId);
    if (rspId == 0) rspId = cmd::id(name.substr(0, pos) + "ScRsp");
    if (rspId == 0) {
        logging::warn("net", "{} has no response id -- the client will wait on it", name);
        return;
    }
    if (!handlers.isEmptyReply(packet.cmdId)) {
        logging::trace("net", "{} is unimplemented, replying empty", name);
        // A login fires dozens of these; they go out as one line once they stop.
        if (logging::enabled(logging::Level::Debug)) {
            ++emptyReplyCount_;
            if (std::find(emptyReplies_.begin(), emptyReplies_.end(), name) == emptyReplies_.end()) {
                emptyReplies_.push_back(name);
            }
            lastEmptyReplyMs_ = util::nowMs();
        }
    }
    sendEmpty(rspId);
}

void Session::sendRaw(uint16_t cmdId, std::string_view body) {
    std::lock_guard lock(mutex_);
    if (state_ == SessionState::Closed || !kcp_) return;

    std::string packet = encodePacket(cmdId, {}, body);
    if (packet.size() > kMaxKcpMessage) {
        logging::error("net", "dropping {} ({} bytes): over the {} byte kcp message limit",
                   cmd::name(cmdId), packet.size(), kMaxKcpMessage);
        return;
    }
    if (core::Config::get().log.packets && !Handlers::get().muted(cmdId)) {
        logging::debug("send", "{} ({}) {} bytes", cmd::name(cmdId), cmdId, body.size());
        if (core::Config::get().log.packetBodies && !body.empty()) {
            logging::debug("send", "  {}", util::hexDump(body));
        }
    }
    ikcp_send(kcp_, packet.data(), static_cast<int>(packet.size()));
    flushOutbound();
}

void Session::sendEmpty(uint16_t cmdId) { sendRaw(cmdId, {}); }

void Session::flushOutbound() {
    if (!kcp_) return;
    // ikcp_flush stamps every segment from kcp->current, and only ikcp_update ever
    // sets it. Flushing on its own leaves current at 0 for a brand new session, so the
    // first update stamps the whole backlog as long overdue, retransmits it and
    // computes a nonsense rtt from ts == 0 -- which shows up as the client dropping the
    // session at an unpredictable point. Update first, then flush for immediacy.
    ikcp_update(kcp_, static_cast<uint32_t>(util::nowMs()));
    ikcp_flush(kcp_);
}

void Session::update(uint32_t nowMs) {
    std::lock_guard lock(mutex_);
    if (kcp_) ikcp_update(kcp_, nowMs);
    if (emptyReplyCount_ != 0 && util::nowMs() >= lastEmptyReplyMs_ + kEmptyReplyQuietMs) {
        flushEmptyReplies();
    }
}

void Session::flushEmptyReplies() {
    constexpr size_t kNamed = 4;
    std::string names;
    for (size_t i = 0; i < emptyReplies_.size() && i < kNamed; ++i) {
        if (i != 0) names += ", ";
        names += emptyReplies_[i];
    }
    if (emptyReplies_.size() > kNamed) {
        names += std::format(" and {} more", emptyReplies_.size() - kNamed);
    }
    logging::debug("net", "{} unimplemented request{} answered empty ({}); log level trace names each",
                   emptyReplyCount_, emptyReplyCount_ == 1 ? "" : "s", names);
    emptyReplies_.clear();
    emptyReplyCount_ = 0;
}

bool Session::expired(uint64_t nowMs) const {
    return isIdle(nowMs, lastActivity_.load(std::memory_order_relaxed), kIdleTimeoutMs);
}

void Session::close() {
    std::lock_guard lock(mutex_);
    if (state_ == SessionState::Closed) return;
    state_ = SessionState::Closed;
    if (emptyReplyCount_ != 0) flushEmptyReplies();
    // The periodic save is throttled, so the last few seconds of walking would be
    // lost without this.
    if (player_) player_->saveNow();
}

}  // namespace net
