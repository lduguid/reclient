#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reclient {

// Reads one MPQ archive (format 0 or 1). Paths use backslashes. The archive
// is opened read-only from the user's game install and nothing is copied out
// of that install into this repository.
class MpqArchive {
public:
    bool open(const std::string& path);
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    const std::string& path() const { return path_; }

private:
    struct HashEntry {
        uint32_t a = 0;
        uint32_t b = 0;
        uint16_t locale = 0;
        uint16_t platform = 0;
        uint32_t block = 0;
    };
    struct BlockEntry {
        uint32_t position = 0;
        uint32_t packed = 0;
        uint32_t unpacked = 0;
        uint32_t flags = 0;
    };

    bool readAt(uint32_t offset, void* dest, size_t n) const;
    std::vector<uint8_t> file_;
    std::string path_;
    uint32_t headerOffset_ = 0;
    uint32_t sectorShift_ = 3;
    std::vector<HashEntry> hashes_;
    std::vector<BlockEntry> blocks_;
};

// Highest-priority archive first, matching the 2.4.3 patch chain.
class ArchiveSet {
public:
    explicit ArchiveSet(const std::string& clientRoot);
    bool read(const std::string& name, std::vector<uint8_t>& out) const;
    int opened() const { return opened_; }
    const std::string& root() const { return root_; }

private:
    std::string root_;
    std::vector<MpqArchive> archives_;
    int opened_ = 0;
};

}  // namespace reclient
