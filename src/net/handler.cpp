#include "net/handler.h"

namespace net {

Handlers& Handlers::get() {
    static Handlers instance;
    return instance;
}

void Handlers::add(uint16_t cmdId, HandlerFn fn) { handlers_[cmdId] = std::move(fn); }

void Handlers::addEmpty(uint16_t reqId) { empties_.insert(reqId); }

void Handlers::addAlias(uint16_t reqId, uint16_t rspId) {
    aliases_[reqId] = rspId;
    empties_.insert(reqId);
}

void Handlers::muteLog(uint16_t cmdId) { muted_.insert(cmdId); }

const HandlerFn* Handlers::find(uint16_t cmdId) const {
    auto it = handlers_.find(cmdId);
    return it == handlers_.end() ? nullptr : &it->second;
}

bool Handlers::isEmptyReply(uint16_t cmdId) const { return empties_.count(cmdId) != 0; }

uint16_t Handlers::aliasFor(uint16_t reqId) const {
    auto it = aliases_.find(reqId);
    return it == aliases_.end() ? 0 : it->second;
}

bool Handlers::muted(uint16_t cmdId) const { return muted_.count(cmdId) != 0; }

}  // namespace net
