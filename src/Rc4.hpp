#pragma once

#include <cstddef>
#include <cstdint>

namespace reclient {

// RC4 as OpenSSL's EVP_rc4 uses it: no dropped keystream bytes.
class Rc4 {
public:
    void init(const uint8_t* key, size_t len);
    void process(uint8_t* data, size_t len);

private:
    uint8_t s_[256]{};
    uint8_t i_ = 0;
    uint8_t j_ = 0;
};

}  // namespace reclient
