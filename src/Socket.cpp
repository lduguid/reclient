#include "Socket.hpp"

#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace reclient {
namespace {
#ifdef _WIN32
struct Wsa {
    Wsa() {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) throw std::runtime_error("WSAStartup failed");
    }
    ~Wsa() { WSACleanup(); }
};
Wsa& wsa() {
    static Wsa instance;
    return instance;
}
using Sock = SOCKET;
constexpr Sock kBad = INVALID_SOCKET;
int lastErr() { return WSAGetLastError(); }
void closeSock(Sock s) { closesocket(s); }
bool timedOut(int err) { return err == WSAETIMEDOUT || err == WSAEWOULDBLOCK; }
#else
using Sock = int;
constexpr Sock kBad = -1;
int lastErr() { return errno; }
void closeSock(Sock s) { ::close(s); }
bool timedOut(int err) { return err == EAGAIN || err == EWOULDBLOCK || err == ETIMEDOUT; }
#endif
}  // namespace

Socket::Socket() {
#ifdef _WIN32
    (void)wsa();
#endif
}

Socket::~Socket() { close(); }

void Socket::close() {
    if (fd_ == ~uintptr_t{0}) return;
    closeSock(static_cast<Sock>(fd_));
    fd_ = ~uintptr_t{0};
}

void Socket::connect(const std::string& host, int port) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    const std::string portText = std::to_string(port);
    if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0 || !result)
        throw std::runtime_error("could not resolve " + host);

    Sock fd = kBad;
    for (addrinfo* it = result; it; it = it->ai_next) {
        fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd == kBad) continue;
        if (::connect(fd, it->ai_addr, int(it->ai_addrlen)) == 0) break;
        closeSock(fd);
        fd = kBad;
    }
    freeaddrinfo(result);
    if (fd == kBad) throw std::runtime_error("could not connect to " + host + ":" + portText);

    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&yes), sizeof(yes));
    fd_ = static_cast<uintptr_t>(fd);
}

void Socket::sendAll(const uint8_t* data, size_t len) {
    if (fd_ == ~uintptr_t{0}) throw std::runtime_error("send on closed socket");
    size_t sent = 0;
    while (sent < len) {
        const int n = ::send(static_cast<Sock>(fd_), reinterpret_cast<const char*>(data + sent), int(len - sent), 0);
        if (n <= 0) throw std::runtime_error("socket send failed");
        sent += size_t(n);
    }
}

void Socket::adopt(uintptr_t fd) {
    close();
    fd_ = fd;
}

bool Socket::recvExact(uint8_t* dest, size_t len, int timeoutMs) {
    if (fd_ == ~uintptr_t{0}) throw std::runtime_error("recv on closed socket");
    // A zero timeout means "do not block". On Windows SO_RCVTIMEO of 0 waits forever.
    if (timeoutMs == 0) {
        fd_set ready;
        FD_ZERO(&ready);
        FD_SET(static_cast<Sock>(fd_), &ready);
        timeval none{};
        if (select(0, &ready, nullptr, nullptr, &none) <= 0) return false;
        timeoutMs = 50;
    }
#ifdef _WIN32
    DWORD tv = timeoutMs < 0 ? 0 : DWORD(timeoutMs);
    setsockopt(static_cast<Sock>(fd_), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(static_cast<Sock>(fd_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    size_t got = 0;
    while (got < len) {
        const int n = ::recv(static_cast<Sock>(fd_), reinterpret_cast<char*>(dest + got), int(len - got), 0);
        if (n == 0) throw std::runtime_error("server closed the connection");
        if (n < 0) {
            if (timedOut(lastErr()) && got == 0) return false;
            if (timedOut(lastErr())) continue;
            throw std::runtime_error("socket recv failed (" + std::to_string(lastErr()) + ")");
        }
        got += size_t(n);
    }
    return true;
}

Listener::Listener() {
#ifdef _WIN32
    (void)wsa();
#endif
}

Listener::~Listener() {
    if (fd_ != ~uintptr_t{0}) closeSock(static_cast<Sock>(fd_));
}

void Listener::listen(int port) {
    Sock fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == kBad) throw std::runtime_error("could not open control socket");
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001);
    addr.sin_port = htons(uint16_t(port));
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(fd, 4) != 0) {
        closeSock(fd);
        throw std::runtime_error("could not listen on 127.0.0.1:" + std::to_string(port));
    }
    fd_ = static_cast<uintptr_t>(fd);
}

bool Listener::acceptInto(Socket& dest, int timeoutMs) {
    if (fd_ == ~uintptr_t{0}) return false;
    fd_set set;
    FD_ZERO(&set);
    FD_SET(static_cast<Sock>(fd_), &set);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int ready = select(int(fd_) + 1, &set, nullptr, nullptr, &tv);
    if (ready <= 0) return false;
    Sock client = ::accept(static_cast<Sock>(fd_), nullptr, nullptr);
    if (client == kBad) return false;
    dest.adopt(static_cast<uintptr_t>(client));
    return true;
}

}  // namespace reclient
