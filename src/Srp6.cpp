#include "Srp6.hpp"

#include "BigInt.hpp"

#include <algorithm>
#include <cctype>
#include <random>
#include <stdexcept>

namespace reclient {
namespace {
std::string upperLatin(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    }
    return s;
}

std::vector<uint8_t> dropTrailingZeros(std::vector<uint8_t> v) {
    while (!v.empty() && v.back() == 0) v.pop_back();
    return v;
}
}  // namespace

Srp6Session srp6Client(std::string username, std::string password, const std::vector<uint8_t>& B,
                       const std::vector<uint8_t>& gBytes, const std::vector<uint8_t>& Nbytes,
                       const std::vector<uint8_t>& salt) {
    username = upperLatin(std::move(username));
    password = upperLatin(std::move(password));
    if (B.empty() || Nbytes.empty() || gBytes.empty() || salt.empty())
        throw std::runtime_error("incomplete SRP6 challenge");

    const BigInt N = BigInt::fromLE(Nbytes);
    const BigInt g = BigInt::fromLE(gBytes);
    const BigInt Bnum = BigInt::fromLE(B);
    const BigInt saltNum = BigInt::fromLE(salt);

    const std::string identity = username + ":" + password;
    const Digest20 I = sha1(identity);
    const std::vector<uint8_t> saltHash = saltNum.unpadded();
    Sha1 xHash;
    xHash.update(saltHash);
    xHash.update(I.data(), I.size());
    const Digest20 xDig = xHash.finish();
    const BigInt x = BigInt::fromLE(xDig.data(), xDig.size());

    std::array<uint8_t, 19> secret{};
    std::random_device rd;
    for (uint8_t& b : secret) b = uint8_t(rd());
    if (std::all_of(secret.begin(), secret.end(), [](uint8_t b) { return b == 0; })) secret[0] = 1;
    const BigInt a = BigInt::fromLE(secret.data(), secret.size());

    const BigInt A = BigInt::modexp(g, a, N);
    const std::vector<uint8_t> Araw = A.toLE(32);

    Sha1 uHash;
    uHash.update(A.unpadded());
    uHash.update(Bnum.unpadded());
    const Digest20 uDig = uHash.finish();
    const BigInt u = BigInt::fromLE(uDig.data(), uDig.size());

    const BigInt v = BigInt::modexp(g, x, N);
    const BigInt kv = BigInt::mod(BigInt::mul(BigInt::fromU32(3), v), N);
    BigInt base;
    if (Bnum.cmp(kv) >= 0)
        base = BigInt::sub(Bnum, kv);
    else
        base = BigInt::sub(BigInt::add(Bnum, N), kv);
    const BigInt exponent = BigInt::add(a, BigInt::mul(u, x));
    const BigInt S = BigInt::modexp(base, exponent, N);
    const std::vector<uint8_t> sBytes = S.toLE(32);

    std::vector<uint8_t> even(16), odd(16);
    for (int i = 0; i < 16; ++i) {
        even[i] = sBytes[i * 2];
        odd[i] = sBytes[i * 2 + 1];
    }
    const Digest20 evenHash = sha1(even.data(), even.size());
    const Digest20 oddHash = sha1(odd.data(), odd.size());
    std::vector<uint8_t> K(40);
    for (int i = 0; i < 20; ++i) {
        K[i * 2] = evenHash[i];
        K[i * 2 + 1] = oddHash[i];
    }
    const BigInt Knum = BigInt::fromLE(K);

    const Digest20 nHash = sha1(N.unpadded().data(), N.unpadded().size());
    const Digest20 gHash = sha1(g.unpadded().data(), g.unpadded().size());
    uint8_t mixed[20];
    for (int i = 0; i < 20; ++i) mixed[i] = uint8_t(nHash[i] ^ gHash[i]);
    const Digest20 userHash = sha1(username);

    Sha1 m1;
    m1.update(mixed, 20);
    m1.update(userHash.data(), userHash.size());
    m1.update(saltNum.unpadded());
    m1.update(A.unpadded());
    m1.update(Bnum.unpadded());
    m1.update(Knum.unpadded());
    const Digest20 M1 = m1.finish();
    const BigInt Mnum = BigInt::fromLE(M1.data(), M1.size());

    Sha1 m2;
    m2.update(A.unpadded());
    m2.update(Mnum.unpadded());
    m2.update(Knum.unpadded());
    const Digest20 M2 = m2.finish();

    Srp6Session session;
    std::copy(Araw.begin(), Araw.end(), session.A.begin());
    session.M1 = M1;
    session.M2 = M2;
    session.K = std::move(K);
    return session;
}

Digest20 worldAuthDigest(std::string_view account, uint32_t clientSeed, uint32_t serverSeed,
                         const std::vector<uint8_t>& sessionKey) {
    const std::vector<uint8_t> key = BigInt::fromLE(sessionKey).unpadded();
    Sha1 h;
    h.update(account);
    const uint32_t zero = 0;
    h.update(reinterpret_cast<const uint8_t*>(&zero), 4);
    h.update(reinterpret_cast<const uint8_t*>(&clientSeed), 4);
    h.update(reinterpret_cast<const uint8_t*>(&serverSeed), 4);
    h.update(key);
    return h.finish();
}

}  // namespace reclient
