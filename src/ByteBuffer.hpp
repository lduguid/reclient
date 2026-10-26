#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace reclient {

class PacketError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class ByteBuffer {
public:
    ByteBuffer() = default;
    explicit ByteBuffer(std::vector<uint8_t> data) : data_(std::move(data)) {}

    const uint8_t* data() const { return data_.data(); }
    uint8_t* data() { return data_.data(); }
    size_t size() const { return data_.size(); }
    size_t remaining() const { return data_.size() - r_; }
    bool empty() const { return data_.empty(); }
    void clear() { data_.clear(); r_ = 0; }

    void append(const uint8_t* p, size_t n) { data_.insert(data_.end(), p, p + n); }
    void append(const std::vector<uint8_t>& v) { append(v.data(), v.size()); }

    template <typename T>
    void write(T v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const uint8_t*>(&v);
        append(p, sizeof(T));
    }

    void writeCString(std::string_view s) {
        append(reinterpret_cast<const uint8_t*>(s.data()), s.size());
        uint8_t z = 0;
        append(&z, 1);
    }

    void writePackedGuid(uint64_t guid) {
        uint8_t pack[9]{};
        size_t n = 1;
        for (uint8_t i = 0; guid != 0; ++i) {
            if (guid & 0xff) {
                pack[0] |= uint8_t(1u << i);
                pack[n++] = uint8_t(guid & 0xff);
            }
            guid >>= 8;
        }
        append(pack, n);
    }

    template <typename T>
    T read() {
        if (remaining() < sizeof(T)) throw PacketError("short read");
        T v{};
        std::memcpy(&v, data_.data() + r_, sizeof(T));
        r_ += sizeof(T);
        return v;
    }

    void readBytes(uint8_t* dest, size_t n) {
        if (remaining() < n) throw PacketError("short read");
        std::memcpy(dest, data_.data() + r_, n);
        r_ += n;
    }

    std::vector<uint8_t> readVec(size_t n) {
        std::vector<uint8_t> v(n);
        readBytes(v.data(), n);
        return v;
    }

    std::string readCString() {
        std::string s;
        while (true) {
            if (remaining() == 0) throw PacketError("unterminated string");
            const char c = char(read<uint8_t>());
            if (c == 0) break;
            s.push_back(c);
        }
        return s;
    }

    uint64_t readPackedGuid() {
        const uint8_t mask = read<uint8_t>();
        uint64_t guid = 0;
        for (int i = 0; i < 8; ++i) {
            if (mask & (1u << i)) guid |= uint64_t(read<uint8_t>()) << (8 * i);
        }
        return guid;
    }

    std::vector<uint8_t>& raw() { return data_; }
    const std::vector<uint8_t>& raw() const { return data_; }

private:
    std::vector<uint8_t> data_;
    size_t r_ = 0;
};

}  // namespace reclient
