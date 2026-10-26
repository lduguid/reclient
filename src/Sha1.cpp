#include "Sha1.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace reclient {
namespace {
uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
}  // namespace

Sha1::Sha1() { reset(); }

void Sha1::reset() {
    h_[0] = 0x67452301u;
    h_[1] = 0xEFCDAB89u;
    h_[2] = 0x98BADCFEu;
    h_[3] = 0x10325476u;
    h_[4] = 0xC3D2E1F0u;
    bits_ = 0;
    blockLen_ = 0;
}

void Sha1::update(const uint8_t* data, size_t len) {
    bits_ += static_cast<uint64_t>(len) * 8u;
    while (len > 0) {
        const size_t n = std::min(len, size_t{64} - blockLen_);
        std::memcpy(block_ + blockLen_, data, n);
        blockLen_ += n;
        data += n;
        len -= n;
        if (blockLen_ == 64) {
            transform();
            blockLen_ = 0;
        }
    }
}

void Sha1::update(std::string_view text) {
    update(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void Sha1::update(const std::vector<uint8_t>& data) { update(data.data(), data.size()); }

void Sha1::transform() {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block_[i * 4]) << 24) | (uint32_t(block_[i * 4 + 1]) << 16) |
               (uint32_t(block_[i * 4 + 2]) << 8) | uint32_t(block_[i * 4 + 3]);
    }
    for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f = 0, k = 0;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol(b, 30);
        b = a;
        a = t;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
}

Digest20 Sha1::finish() {
    const uint64_t bitCount = bits_;
    uint8_t one = 0x80;
    block_[blockLen_++] = one;
    if (blockLen_ > 56) {
        while (blockLen_ < 64) block_[blockLen_++] = 0;
        transform();
        blockLen_ = 0;
    }
    while (blockLen_ < 56) block_[blockLen_++] = 0;
    for (int i = 7; i >= 0; --i) block_[blockLen_++] = uint8_t((bitCount >> (i * 8)) & 0xff);
    transform();

    Digest20 out{};
    for (int i = 0; i < 5; ++i) {
        out[i * 4] = uint8_t((h_[i] >> 24) & 0xff);
        out[i * 4 + 1] = uint8_t((h_[i] >> 16) & 0xff);
        out[i * 4 + 2] = uint8_t((h_[i] >> 8) & 0xff);
        out[i * 4 + 3] = uint8_t(h_[i] & 0xff);
    }
    reset();
    return out;
}

Digest20 sha1(const uint8_t* data, size_t len) {
    Sha1 h;
    h.update(data, len);
    return h.finish();
}

Digest20 sha1(std::string_view text) { return sha1(reinterpret_cast<const uint8_t*>(text.data()), text.size()); }

Digest20 hmacSha1(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen) {
    uint8_t k[64]{};
    if (keyLen > 64) {
        const Digest20 hashed = sha1(key, keyLen);
        std::memcpy(k, hashed.data(), hashed.size());
    } else {
        std::memcpy(k, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = uint8_t(k[i] ^ 0x36);
        opad[i] = uint8_t(k[i] ^ 0x5c);
    }
    Sha1 inner;
    inner.update(ipad, 64);
    inner.update(data, dataLen);
    const Digest20 innerDig = inner.finish();
    Sha1 outer;
    outer.update(opad, 64);
    outer.update(innerDig.data(), innerDig.size());
    return outer.finish();
}

std::string hexOf(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out(len * 2, '0');
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[data[i] >> 4];
        out[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return out;
}

}  // namespace reclient
