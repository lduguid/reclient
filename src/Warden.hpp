#pragma once

#include "Rc4.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace reclient {

// Keys for the Warden packet cipher. The server generates the decrypt key
// first and the encrypt key second from the unpadded session key, using the
// SHA1Randx mixer. Packets the server sends are encrypted with the second key.
struct WardenKeys {
    std::array<uint8_t, 16> toServer{};
    std::array<uint8_t, 16> fromServer{};
};

WardenKeys deriveWardenKeys(const std::vector<uint8_t>& sessionKey);

class WardenStream {
public:
    void init(const std::vector<uint8_t>& sessionKey);
    bool ready() const { return ready_; }
    void decrypt(uint8_t* data, size_t len) { fromServer_.process(data, len); }
    void encrypt(uint8_t* data, size_t len) { toServer_.process(data, len); }

private:
    Rc4 toServer_;
    Rc4 fromServer_;
    bool ready_ = false;
};

}  // namespace reclient
