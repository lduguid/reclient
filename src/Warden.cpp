#include "Warden.hpp"

#include "Sha1.hpp"

namespace reclient {
namespace {

class Sha1Randx {
public:
    explicit Sha1Randx(const uint8_t* buff, size_t size) {
        const size_t half = size / 2;
        o1_ = sha1(buff, half);
        o2_ = sha1(buff + half, size - half);
        o0_.fill(0);
        fill();
    }

    void generate(uint8_t* out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            if (taken_ == 20) fill();
            out[i] = o0_[taken_++];
        }
    }

private:
    void fill() {
        Sha1 hash;
        hash.update(o1_.data(), o1_.size());
        hash.update(o0_.data(), o0_.size());
        hash.update(o2_.data(), o2_.size());
        o0_ = hash.finish();
        taken_ = 0;
    }

    Digest20 o0_{};
    Digest20 o1_{};
    Digest20 o2_{};
    size_t taken_ = 0;
};

std::vector<uint8_t> unpadded(std::vector<uint8_t> key) {
    while (!key.empty() && key.back() == 0) key.pop_back();
    return key;
}

}  // namespace

WardenKeys deriveWardenKeys(const std::vector<uint8_t>& sessionKey) {
    const std::vector<uint8_t> key = unpadded(sessionKey);
    Sha1Randx mix(key.data(), key.size());
    WardenKeys out;
    mix.generate(out.toServer.data(), out.toServer.size());
    mix.generate(out.fromServer.data(), out.fromServer.size());
    return out;
}

void WardenStream::init(const std::vector<uint8_t>& sessionKey) {
    const WardenKeys keys = deriveWardenKeys(sessionKey);
    toServer_.init(keys.toServer.data(), keys.toServer.size());
    fromServer_.init(keys.fromServer.data(), keys.fromServer.size());
    ready_ = true;
}

}  // namespace reclient
