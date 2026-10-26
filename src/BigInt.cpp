#include "BigInt.hpp"

#include <algorithm>
#include <stdexcept>

namespace reclient {

BigInt BigInt::fromU32(uint32_t v) {
    BigInt b;
    if (v) b.w_.push_back(v);
    return b;
}

BigInt BigInt::fromLE(const uint8_t* data, size_t len) {
    BigInt b;
    if (len == 0) return b;
    b.w_.assign((len + 3) / 4, 0);
    for (size_t i = 0; i < len; ++i) b.w_[i / 4] |= uint32_t(data[i]) << ((i % 4) * 8);
    b.trim();
    return b;
}

BigInt BigInt::fromLE(const std::vector<uint8_t>& data) { return fromLE(data.data(), data.size()); }

void BigInt::trim() {
    while (!w_.empty() && w_.back() == 0) w_.pop_back();
}

std::vector<uint8_t> BigInt::toLE(size_t width) const {
    std::vector<uint8_t> out(width, 0);
    for (size_t i = 0; i < width; ++i) {
        const size_t limb = i / 4;
        if (limb < w_.size()) out[i] = uint8_t((w_[limb] >> ((i % 4) * 8)) & 0xff);
    }
    return out;
}

std::vector<uint8_t> BigInt::unpadded() const {
    if (w_.empty()) return {};
    std::vector<uint8_t> out = toLE(w_.size() * 4);
    while (!out.empty() && out.back() == 0) out.pop_back();
    return out;
}

int BigInt::cmp(const BigInt& other) const {
    if (w_.size() != other.w_.size()) return w_.size() < other.w_.size() ? -1 : 1;
    for (size_t i = w_.size(); i-- > 0;) {
        if (w_[i] != other.w_[i]) return w_[i] < other.w_[i] ? -1 : 1;
    }
    return 0;
}

int BigInt::bitLength() const {
    if (w_.empty()) return 0;
    uint32_t top = w_.back();
    int bits = int((w_.size() - 1) * 32);
    while (top) {
        ++bits;
        top >>= 1;
    }
    return bits;
}

BigInt BigInt::add(const BigInt& a, const BigInt& b) {
    BigInt r;
    const size_t n = std::max(a.w_.size(), b.w_.size());
    r.w_.resize(n, 0);
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint64_t av = i < a.w_.size() ? a.w_[i] : 0;
        const uint64_t bv = i < b.w_.size() ? b.w_[i] : 0;
        const uint64_t sum = av + bv + carry;
        r.w_[i] = uint32_t(sum);
        carry = sum >> 32;
    }
    if (carry) r.w_.push_back(uint32_t(carry));
    return r;
}

BigInt BigInt::sub(const BigInt& a, const BigInt& b) {
    if (a.cmp(b) < 0) throw std::logic_error("BigInt subtraction underflow");
    BigInt r;
    r.w_.resize(a.w_.size(), 0);
    int64_t borrow = 0;
    for (size_t i = 0; i < a.w_.size(); ++i) {
        const int64_t bv = i < b.w_.size() ? b.w_[i] : 0;
        int64_t diff = int64_t(a.w_[i]) - bv - borrow;
        if (diff < 0) {
            diff += 0x100000000ll;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r.w_[i] = uint32_t(diff);
    }
    r.trim();
    return r;
}

BigInt BigInt::mul(const BigInt& a, const BigInt& b) {
    if (a.isZero() || b.isZero()) return {};
    BigInt r;
    r.w_.assign(a.w_.size() + b.w_.size(), 0);
    for (size_t i = 0; i < a.w_.size(); ++i) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.w_.size(); ++j) {
            const uint64_t cur = uint64_t(r.w_[i + j]) + uint64_t(a.w_[i]) * b.w_[j] + carry;
            r.w_[i + j] = uint32_t(cur);
            carry = cur >> 32;
        }
        r.w_[i + b.w_.size()] = uint32_t(carry);
    }
    r.trim();
    return r;
}

void BigInt::shl1() {
    uint32_t carry = 0;
    for (uint32_t& limb : w_) {
        const uint32_t next = limb >> 31;
        limb = (limb << 1) | carry;
        carry = next;
    }
    if (carry) w_.push_back(carry);
}

void BigInt::shr1() {
    uint32_t carry = 0;
    for (size_t i = w_.size(); i-- > 0;) {
        const uint32_t next = w_[i] & 1u;
        w_[i] = (w_[i] >> 1) | (carry << 31);
        carry = next;
    }
    trim();
}

BigInt BigInt::mod(BigInt a, const BigInt& m) {
    if (m.isZero()) throw std::logic_error("modulo zero");
    if (a.cmp(m) < 0) return a;
    const int shift = a.bitLength() - m.bitLength();
    BigInt d = m;
    for (int i = 0; i < shift; ++i) d.shl1();
    for (int i = shift; i >= 0; --i) {
        if (a.cmp(d) >= 0) a = sub(a, d);
        if (i) d.shr1();
    }
    return a;
}

BigInt BigInt::modexp(BigInt base, BigInt exp, const BigInt& modulus) {
    if (modulus.isZero()) throw std::logic_error("modexp modulo zero");
    base = mod(std::move(base), modulus);
    BigInt result = fromU32(1);
    if (exp.isZero()) return result;
    while (!exp.isZero()) {
        if (!exp.w_.empty() && (exp.w_[0] & 1u)) result = mod(mul(result, base), modulus);
        base = mod(mul(base, base), modulus);
        exp.shr1();
    }
    return result;
}

}  // namespace reclient
