#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace reclient {

using Digest20 = std::array<uint8_t, 20>;

// FIPS 180-1 SHA-1. Used by the 2.4.3 logon and world-auth digests.
class Sha1 {
public:
    Sha1();
    void update(const uint8_t* data, size_t len);
    void update(std::string_view text);
    void update(const std::vector<uint8_t>& data);
    Digest20 finish();

private:
    void reset();
    void transform();

    uint32_t h_[5]{};
    uint64_t bits_ = 0;
    uint8_t block_[64]{};
    size_t blockLen_ = 0;
};

Digest20 sha1(const uint8_t* data, size_t len);
Digest20 sha1(std::string_view text);
Digest20 hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen);

std::string hexOf(const uint8_t* data, size_t len);

}  // namespace reclient
