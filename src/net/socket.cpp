#include "net/socket.h"

#include <cstring>
#include <mutex>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
using socklen_t = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#define closesocket close
#endif

#include "core/logger.h"

namespace net {
namespace {

sockaddr_in makeAddr(const std::string& host, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    }
    return addr;
}

}  // namespace

void initSockets() {
#ifdef _WIN32
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    });
#endif
}

std::string UdpAddress::str() const {
    char buf[INET_ADDRSTRLEN] = {0};
    in_addr a{};
    std::memcpy(&a, &ip, sizeof(uint32_t));
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf) + ":" + std::to_string(port);
}

uintptr_t tcpListen(const std::string& bind, uint16_t port) {
    initSockets();
    uintptr_t s = static_cast<uintptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalidSocket) return kInvalidSocket;
    int on = 1;
    setsockopt(static_cast<int>(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
               sizeof(on));
    sockaddr_in addr = makeAddr(bind, port);
    if (::bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(static_cast<int>(s), 128) != 0) {
        closesocket(static_cast<int>(s));
        return kInvalidSocket;
    }
    return s;
}

uintptr_t tcpAccept(uintptr_t listener, std::string* remote) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    uintptr_t c = static_cast<uintptr_t>(
        ::accept(static_cast<int>(listener), reinterpret_cast<sockaddr*>(&addr), &len));
    if (c == kInvalidSocket) return kInvalidSocket;
    int on = 1;
    setsockopt(static_cast<int>(c), IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on),
               sizeof(on));
    if (remote) {
        char buf[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        *remote = std::string(buf) + ":" + std::to_string(ntohs(addr.sin_port));
    }
    return c;
}

int tcpRecv(uintptr_t socket, char* buffer, size_t size) {
    return ::recv(static_cast<int>(socket), buffer, static_cast<int>(size), 0);
}

bool tcpSendAll(uintptr_t socket, const char* data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        int n = ::send(static_cast<int>(socket), data + sent, static_cast<int>(size - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void setRecvTimeout(uintptr_t socket, int ms) {
#ifdef _WIN32
    DWORD timeout = static_cast<DWORD>(ms);
    setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    setsockopt(static_cast<int>(socket), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

void closeSocket(uintptr_t socket) {
    if (socket == kInvalidSocket) return;
    closesocket(static_cast<int>(socket));
}

void shutdownSocket(uintptr_t socket) {
    if (socket == kInvalidSocket) return;
#ifdef _WIN32
    ::shutdown(static_cast<SOCKET>(socket), SD_BOTH);
#else
    ::shutdown(static_cast<int>(socket), SHUT_RDWR);
#endif
}

uintptr_t tcpConnect(const std::string& host, uint16_t port) {
    initSockets();
    uintptr_t s = static_cast<uintptr_t>(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (s == kInvalidSocket) return kInvalidSocket;
    sockaddr_in addr = makeAddr(host, port);
    if (::connect(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(static_cast<int>(s));
        return kInvalidSocket;
    }
    return s;
}

uintptr_t udpBind(const std::string& bind, uint16_t port) {
    initSockets();
    uintptr_t s = static_cast<uintptr_t>(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s == kInvalidSocket) return kInvalidSocket;
    int on = 1;
    setsockopt(static_cast<int>(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on),
               sizeof(on));
    int bufSize = 1 << 20;
    setsockopt(static_cast<int>(s), SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufSize),
               sizeof(bufSize));
    setsockopt(static_cast<int>(s), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufSize),
               sizeof(bufSize));
#ifdef _WIN32
    // Without this a client that vanishes makes recvfrom fail with WSAECONNRESET forever.
    BOOL behavior = FALSE;
    DWORD bytes = 0;
    WSAIoctl(static_cast<SOCKET>(s), SIO_UDP_CONNRESET, &behavior, sizeof(behavior), nullptr, 0,
             &bytes, nullptr, nullptr);
#endif
    sockaddr_in addr = makeAddr(bind, port);
    if (::bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(static_cast<int>(s));
        return kInvalidSocket;
    }
    return s;
}

int udpRecv(uintptr_t socket, char* buffer, size_t size, UdpAddress* from) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    int n = ::recvfrom(static_cast<int>(socket), buffer, static_cast<int>(size), 0,
                       reinterpret_cast<sockaddr*>(&addr), &len);
    if (n > 0 && from) {
        std::memcpy(&from->ip, &addr.sin_addr, sizeof(uint32_t));
        from->port = ntohs(addr.sin_port);
    }
    return n;
}

bool udpSend(uintptr_t socket, const char* data, size_t size, const UdpAddress& to) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    std::memcpy(&addr.sin_addr, &to.ip, sizeof(uint32_t));
    int n = ::sendto(static_cast<int>(socket), data, static_cast<int>(size), 0,
                     reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<int>(size);
}

}  // namespace net
