#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reclient {

// Blocking TCP socket. Timeouts let the main loop service input and heartbeats.
class Socket {
public:
    Socket();
    ~Socket();
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;

    void connect(const std::string& host, int port);
    void close();
    bool connected() const { return fd_ != ~uintptr_t{0}; }

    void sendAll(const uint8_t* data, size_t len);
    void sendAll(const std::vector<uint8_t>& data) { sendAll(data.data(), data.size()); }

    // Returns false on timeout. Throws on a closed or failed socket.
    bool recvExact(uint8_t* dest, size_t len, int timeoutMs);
    void adopt(uintptr_t fd);

private:
    uintptr_t fd_ = ~uintptr_t{0};
};

class Listener {
public:
    Listener();
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    void listen(int port);
    bool acceptInto(Socket& dest, int timeoutMs);

private:
    uintptr_t fd_ = ~uintptr_t{0};
};

}  // namespace reclient
