#include "Rc4.hpp"

namespace reclient {

void Rc4::init(const uint8_t* key, size_t len) {
    for (int i = 0; i < 256; ++i) s_[i] = uint8_t(i);
    uint8_t j = 0;
    for (int i = 0; i < 256; ++i) {
        j = uint8_t(j + s_[i] + key[size_t(i) % len]);
        const uint8_t tmp = s_[i];
        s_[i] = s_[j];
        s_[j] = tmp;
    }
    i_ = 0;
    j_ = 0;
}

void Rc4::process(uint8_t* data, size_t len) {
    for (size_t n = 0; n < len; ++n) {
        i_ = uint8_t(i_ + 1);
        j_ = uint8_t(j_ + s_[i_]);
        const uint8_t tmp = s_[i_];
        s_[i_] = s_[j_];
        s_[j_] = tmp;
        data[n] ^= s_[uint8_t(s_[i_] + s_[j_])];
    }
}

}  // namespace reclient
