#include "AuthCrypt.hpp"
#include "BigInt.hpp"
#include "Rc4.hpp"
#include "Sha1.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
int failures = 0;

void expect(bool cond, const char* name) {
    if (cond) {
        std::printf("ok %s\n", name);
        return;
    }
    std::printf("FAIL %s\n", name);
    ++failures;
}

bool isHex(const reclient::Digest20& dig, const char* hex) {
    return reclient::hexOf(dig.data(), dig.size()) == hex;
}
}  // namespace

void checkRc4() {
    reclient::Rc4 rc4;
    const char* key = "Key";
    const char* plain = "Plaintext";
    uint8_t buf[9];
    std::memcpy(buf, plain, 9);
    rc4.init(reinterpret_cast<const uint8_t*>(key), 3);
    rc4.process(buf, 9);
    const uint8_t known[9] = {0xBB, 0xF3, 0x16, 0xE8, 0xD9, 0x40, 0xAF, 0x0A, 0xD3};
    expect(std::memcmp(buf, known, 9) == 0, "rc4 plaintext vector");
}

int main() {
    const reclient::Digest20 abc = reclient::sha1("abc");
    expect(isHex(abc, "a9993e364706816aba3e25717850c26c9cd0d89d"), "sha1 abc");

    const uint8_t key[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
                             0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    const char* data = "Hi There";
    const reclient::Digest20 mac =
        reclient::hmacSha1(key, sizeof(key), reinterpret_cast<const uint8_t*>(data), std::strlen(data));
    expect(isHex(mac, "b617318655057264e28bc0b6fb378c8ef146be00"), "hmac sha1 rfc2202");

    const reclient::BigInt four = reclient::BigInt::fromU32(4);
    const reclient::BigInt thirteen = reclient::BigInt::fromU32(13);
    const reclient::BigInt mod = reclient::BigInt::fromU32(497);
    const reclient::BigInt pow = reclient::BigInt::modexp(four, thirteen, mod);
    const auto bytes = pow.toLE(2);
    expect(bytes[0] == 189 && bytes[1] == 1, "modexp 4^13 % 497 = 445");

    uint8_t le[4] = {0x07, 0x00, 0x00, 0x00};
    const reclient::BigInt seven = reclient::BigInt::fromLE(le, 4);
    expect(seven.unpadded().size() == 1 && seven.unpadded()[0] == 7, "unpadded little-endian");

    std::vector<uint8_t> session(40, 0x11);
    reclient::AuthCrypt crypt;
    crypt.init(session);
    uint8_t header[6] = {1, 2, 3, 4, 5, 6};
    uint8_t copy[6];
    std::memcpy(copy, header, 6);
    crypt.encryptSend(header, 6);
    expect(std::memcmp(header, copy, 6) != 0, "authcrypt changes the header");

    uint8_t j = 0, i = 0;
    const reclient::Digest20 stream = reclient::hmacSha1(
        reinterpret_cast<const uint8_t*>("\x38\xA7\x83\x15\xF8\x92\x25\x30\x71\x98\x67\xB1\x8C\x04\xE2\xAA"), 16,
        reclient::BigInt::fromLE(session).unpadded().data(), reclient::BigInt::fromLE(session).unpadded().size());
    uint8_t plain[6];
    std::memcpy(plain, header, 6);
    for (int n = 0; n < 6; ++n) {
        i %= 20;
        const uint8_t x = uint8_t((plain[n] - j) ^ stream[i]);
        ++i;
        j = plain[n];
        plain[n] = x;
    }
    expect(std::memcmp(plain, copy, 6) == 0, "authcrypt send is reversible");
    checkRc4();

    return failures == 0 ? 0 : 1;
}
