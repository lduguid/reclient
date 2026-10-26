#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace reclient {

// Little-endian arbitrary-precision integer. Wire values in the 2.4.3
// logon exchange are little-endian, so this stores the same byte order.
class BigInt {
public:
    BigInt() = default;
    static BigInt fromU32(uint32_t v);
    static BigInt fromLE(const uint8_t* data, size_t len);
    static BigInt fromLE(const std::vector<uint8_t>& data);

    std::vector<uint8_t> toLE(size_t width) const;
    // Drop trailing zero bytes. Matches the server's unpadded bignum encoding.
    std::vector<uint8_t> unpadded() const;

    bool isZero() const { return w_.empty(); }
    int cmp(const BigInt& other) const;
    int bitLength() const;

    static BigInt add(const BigInt& a, const BigInt& b);
    static BigInt sub(const BigInt& a, const BigInt& b);
    static BigInt mul(const BigInt& a, const BigInt& b);
    static BigInt mod(BigInt a, const BigInt& m);
    static BigInt modexp(BigInt base, BigInt exp, const BigInt& mod);

    void shl1();
    void shr1();

private:
    void trim();
    std::vector<uint32_t> w_;
};

}  // namespace reclient
