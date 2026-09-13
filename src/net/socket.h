#pragma once

#include <cstdint>
#include <string>

namespace net {

#ifdef _WIN32
constexpr uintptr_t kInvalidSocket = ~static_cast<uintptr_t>(0);
#else
constexpr uintptr_t kInvalidSocket = static_cast<uintptr_t>(-1);
#endif

void initSockets();

uintptr_t tcpListen(const std::string& bind, uint16_t port);
uintptr_t tcpConnect(const std::string& host, uint16_t port);
uintptr_t tcpAccept(uintptr_t listener, std::string* remote);
int tcpRecv(uintptr_t socket, char* buffer, size_t size);
bool tcpSendAll(uintptr_t socket, const char* data, size_t size);
void setRecvTimeout(uintptr_t socket, int ms);
void closeSocket(uintptr_t socket);
// Wakes a thread blocked reading the socket without freeing the handle under it.
void shutdownSocket(uintptr_t socket);

struct UdpAddress {
    uint32_t ip = 0;  // network byte order
    uint16_t port = 0;

    bool operator==(const UdpAddress& o) const { return ip == o.ip && port == o.port; }
    bool operator<(const UdpAddress& o) const {
        return ip != o.ip ? ip < o.ip : port < o.port;
    }
    std::string str() const;
};

uintptr_t udpBind(const std::string& bind, uint16_t port);
int udpRecv(uintptr_t socket, char* buffer, size_t size, UdpAddress* from);
bool udpSend(uintptr_t socket, const char* data, size_t size, const UdpAddress& to);

}  // namespace net
