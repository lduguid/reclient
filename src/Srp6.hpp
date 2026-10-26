#pragma once

#include "Sha1.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace reclient {

struct Srp6Session {
    std::array<uint8_t, 32> A{};
    Digest20 M1{};
    Digest20 M2{};
    std::vector<uint8_t> K;  // 40-byte interleaved session key, little-endian
};

// Client half of the 2.4.3 logon (SRP-6, k = 3, g usually 7).
// Number encoding follows the server: SHA-1 inputs use unpadded little-endian
// bignums (trailing zero bytes removed). See docs/discrepancies.md.
Srp6Session srp6Client(std::string username, std::string password, const std::vector<uint8_t>& B,
                       const std::vector<uint8_t>& g, const std::vector<uint8_t>& N,
                       const std::vector<uint8_t>& salt);

Digest20 worldAuthDigest(std::string_view account, uint32_t clientSeed, uint32_t serverSeed,
                         const std::vector<uint8_t>& sessionKey);

}  // namespace reclient
