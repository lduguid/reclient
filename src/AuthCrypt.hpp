#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace reclient {

// Header cipher used after world authentication.
// The server encrypts 4-byte server headers and decrypts 6-byte client headers
// with one HMAC-SHA1 key. Sending uses the inverse of the server decrypt, which
// is the same mix of XOR and add the server uses when it encrypts.
class AuthCrypt {
public:
    void init(const std::vector<uint8_t>& sessionKey);
    void encryptSend(uint8_t* data, size_t len);
    void decryptRecv(uint8_t* data, size_t len);
    bool ready() const { return ready_; }

private:
    std::vector<uint8_t> key_;
    uint8_t sendI_ = 0, sendJ_ = 0, recvI_ = 0, recvJ_ = 0;
    bool ready_ = false;
};

}  // namespace reclient
