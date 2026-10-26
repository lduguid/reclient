#include "AuthCrypt.hpp"

#include "BigInt.hpp"
#include "Sha1.hpp"

namespace reclient {
namespace {
constexpr uint8_t kSeed[16] = {0x38, 0xA7, 0x83, 0x15, 0xF8, 0x92, 0x25, 0x30,
                               0x71, 0x98, 0x67, 0xB1, 0x8C, 0x04, 0xE2, 0xAA};
}

void AuthCrypt::init(const std::vector<uint8_t>& sessionKey) {
    const std::vector<uint8_t> material = BigInt::fromLE(sessionKey).unpadded();
    const Digest20 digest = hmacSha1(kSeed, sizeof(kSeed), material.data(), material.size());
    key_.assign(digest.begin(), digest.end());
    sendI_ = sendJ_ = recvI_ = recvJ_ = 0;
    ready_ = true;
}

void AuthCrypt::encryptSend(uint8_t* data, size_t len) {
    if (!ready_ || len < 6) return;
    for (size_t t = 0; t < 6; ++t) {
        sendI_ %= uint8_t(key_.size());
        const uint8_t x = uint8_t((data[t] ^ key_[sendI_]) + sendJ_);
        ++sendI_;
        data[t] = sendJ_ = x;
    }
}

void AuthCrypt::decryptRecv(uint8_t* data, size_t len) {
    if (!ready_ || len < 4) return;
    for (size_t t = 0; t < 4; ++t) {
        recvI_ %= uint8_t(key_.size());
        const uint8_t x = uint8_t((data[t] - recvJ_) ^ key_[recvI_]);
        ++recvI_;
        recvJ_ = data[t];
        data[t] = x;
    }
}

}  // namespace reclient
