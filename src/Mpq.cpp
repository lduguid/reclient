#include "Mpq.hpp"

#include "miniz.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

namespace reclient {
namespace {

uint32_t cryptTable[0x500];
std::once_flag cryptOnce;

void initCrypt() {
    std::call_once(cryptOnce, [] {
        uint32_t seed = 0x00100001;
        for (uint32_t index1 = 0; index1 < 0x100; ++index1) {
            uint32_t index2 = index1;
            for (int i = 0; i < 5; ++i) {
                seed = (seed * 125 + 3) % 0x2AAAAB;
                const uint32_t temp1 = (seed & 0xFFFF) << 16;
                seed = (seed * 125 + 3) % 0x2AAAAB;
                const uint32_t temp2 = seed & 0xFFFF;
                cryptTable[index2] = temp1 | temp2;
                index2 += 0x100;
            }
        }
    });
}

uint32_t hashString(const std::string& name, uint32_t type) {
    initCrypt();
    uint32_t seed1 = 0x7FED7FED;
    uint32_t seed2 = 0xEEEEEEEE;
    for (unsigned char ch : name) {
        if (ch == '/') ch = '\\';
        if (ch >= 'a' && ch <= 'z') ch = uint8_t(ch - 'a' + 'A');
        seed1 = cryptTable[(type << 8) + ch] ^ (seed1 + seed2);
        seed2 = uint32_t(ch) + seed1 + seed2 + (seed2 << 5) + 3;
    }
    return seed1;
}

void decryptDwords(uint32_t* data, size_t dwords, uint32_t key) {
    initCrypt();
    uint32_t seed = 0xEEEEEEEE;
    for (size_t i = 0; i < dwords; ++i) {
        seed += cryptTable[0x400 + (key & 0xFF)];
        const uint32_t ch = data[i] ^ (key + seed);
        key = ((~key << 0x15) + 0x11111111) | (key >> 0x0B);
        seed = ch + seed + (seed << 5) + 3;
        data[i] = ch;
    }
}

std::string normalize(std::string name) {
    for (char& c : name) {
        if (c == '/') c = '\\';
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    }
    return name;
}

constexpr uint32_t kFileImplode = 0x00000100;
constexpr uint32_t kFileCompress = 0x00000200;
constexpr uint32_t kFileEncrypted = 0x00010000;
constexpr uint32_t kFileFixKey = 0x00020000;
constexpr uint32_t kFilePatch = 0x00100000;
constexpr uint32_t kFileSingle = 0x01000000;
constexpr uint32_t kFileSectorCrc = 0x04000000;
constexpr uint32_t kFileExists = 0x80000000;
constexpr uint32_t kBlockFree = 0xFFFFFFFF;
constexpr uint32_t kBlockDeleted = 0xFFFFFFFE;

bool inflateZlib(const uint8_t* data, size_t len, size_t expect, std::vector<uint8_t>& out) {
    out.assign(expect, 0);
    mz_ulong dest = mz_ulong(expect);
    if (mz_uncompress(out.data(), &dest, data, mz_ulong(len)) != MZ_OK) return false;
    out.resize(dest);
    return true;
}

uint32_t load32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t load16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

}  // namespace

bool MpqArchive::readAt(uint32_t offset, void* dest, size_t n) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return false;
    in.seekg(std::streamoff(offset), std::ios::beg);
    in.read(reinterpret_cast<char*>(dest), std::streamsize(n));
    return bool(in) && size_t(in.gcount()) == n;
}

bool MpqArchive::open(const std::string& path) {
    path_ = path;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint8_t probe[0x600];
    in.read(reinterpret_cast<char*>(probe), sizeof(probe));
    const size_t got = size_t(in.gcount());
    headerOffset_ = 0;
    bool found = false;
    for (size_t i = 0; i + 4 <= got; i += 4) {
        if (std::memcmp(probe + i, "MPQ\x1A", 4) == 0) {
            headerOffset_ = uint32_t(i);
            found = true;
            break;
        }
    }
    if (!found) return false;
    if (headerOffset_ + 32 > got) return false;
    const uint8_t* header = probe + headerOffset_;
    const uint16_t version = load16(header + 12);
    const uint16_t blockShift = load16(header + 14);
    uint32_t hashPos = load32(header + 16);
    uint32_t blockPos = load32(header + 20);
    const uint32_t hashCount = load32(header + 24);
    const uint32_t blockCount = load32(header + 28);
    if (version > 1 || hashCount == 0 || blockCount == 0 || hashCount > 0x200000 || blockCount > 0x200000)
        return false;
    sectorShift_ = blockShift;
    hashPos += headerOffset_;
    blockPos += headerOffset_;

    hashes_.resize(hashCount);
    blocks_.resize(blockCount);
    if (!readAt(hashPos, hashes_.data(), hashCount * sizeof(HashEntry))) return false;
    if (!readAt(blockPos, blocks_.data(), blockCount * sizeof(BlockEntry))) return false;
    decryptDwords(reinterpret_cast<uint32_t*>(hashes_.data()), hashCount * 4, hashString("(hash table)", 3));
    decryptDwords(reinterpret_cast<uint32_t*>(blocks_.data()), blockCount * 4, hashString("(block table)", 3));
    return true;
}

bool MpqArchive::read(const std::string& name, std::vector<uint8_t>& out) const {
    if (hashes_.empty()) return false;
    const std::string key = normalize(name);
    const uint32_t start = hashString(key, 0) % uint32_t(hashes_.size());
    const uint32_t hashA = hashString(key, 1);
    const uint32_t hashB = hashString(key, 2);
    uint32_t blockIndex = kBlockFree;
    for (size_t n = 0; n < hashes_.size(); ++n) {
        const HashEntry& entry = hashes_[(start + uint32_t(n)) % hashes_.size()];
        if (entry.block == kBlockFree) break;
        if (entry.block == kBlockDeleted) continue;
        if (entry.a == hashA && entry.b == hashB) {
            blockIndex = entry.block;
            break;
        }
    }
    if (blockIndex >= blocks_.size()) return false;
    const BlockEntry& block = blocks_[blockIndex];
    if ((block.flags & kFileExists) == 0 || (block.flags & kFilePatch) != 0) return false;
    if (block.unpacked == 0 || block.unpacked > 64 * 1024 * 1024) {
        if (block.unpacked == 0) {
            out.clear();
            return true;
        }
        return false;
    }
    const uint32_t filePos = block.position + headerOffset_;
    uint32_t fileKey = hashString(key, 3);
    if (block.flags & kFileFixKey) fileKey = (fileKey + block.position) ^ block.unpacked;

    const bool compressed = (block.flags & (kFileCompress | kFileImplode)) != 0;
    const bool encrypted = (block.flags & kFileEncrypted) != 0;
    const bool single = (block.flags & kFileSingle) != 0 || !compressed;
    const uint32_t sectorSize = 512u << sectorShift_;

    auto readRange = [&](uint32_t offset, size_t n, std::vector<uint8_t>& dest) {
        dest.resize(n);
        return n == 0 || readAt(offset, dest.data(), n);
    };

    if (single) {
        std::vector<uint8_t> packed;
        if (!readRange(filePos, block.packed, packed)) return false;
        if (encrypted && packed.size() >= 4)
            decryptDwords(reinterpret_cast<uint32_t*>(packed.data()), packed.size() / 4, fileKey);
        if (!compressed || block.packed >= block.unpacked) {
            packed.resize(block.unpacked);
            out = std::move(packed);
            return true;
        }
        if ((block.flags & kFileImplode) || packed.empty() || packed[0] != 0x02) return false;
        return inflateZlib(packed.data() + 1, packed.size() - 1, block.unpacked, out);
    }

    uint32_t sectorCount = (block.unpacked + sectorSize - 1) / sectorSize;
    if (block.flags & kFileSectorCrc) ++sectorCount;
    const uint32_t tableBytes = (sectorCount + 1) * 4;
    std::vector<uint8_t> table;
    if (!readRange(filePos, tableBytes, table)) return false;
    std::vector<uint32_t> offsets(sectorCount + 1);
    std::memcpy(offsets.data(), table.data(), tableBytes);
    if (encrypted) decryptDwords(offsets.data(), offsets.size(), fileKey - 1);

    out.assign(block.unpacked, 0);
    const uint32_t dataSectors = (block.unpacked + sectorSize - 1) / sectorSize;
    for (uint32_t sector = 0; sector < dataSectors; ++sector) {
        if (sector + 1 >= offsets.size()) return false;
        const uint32_t begin = offsets[sector];
        const uint32_t end = offsets[sector + 1];
        if (end < begin || end - begin > 8 * 1024 * 1024) return false;
        const uint32_t packedLen = end - begin;
        const uint32_t want = std::min(sectorSize, block.unpacked - sector * sectorSize);
        std::vector<uint8_t> packed;
        if (!readRange(filePos + begin, packedLen, packed)) return false;
        if (encrypted && packed.size() >= 4)
            decryptDwords(reinterpret_cast<uint32_t*>(packed.data()), packed.size() / 4, fileKey + sector);
        uint8_t* dest = out.data() + size_t(sector) * sectorSize;
        if (packedLen >= want) {
            std::memcpy(dest, packed.data(), want);
            continue;
        }
        if (packed.empty() || packed[0] != 0x02) return false;
        std::vector<uint8_t> inflated;
        if (!inflateZlib(packed.data() + 1, packed.size() - 1, want, inflated) || inflated.size() < want) return false;
        std::memcpy(dest, inflated.data(), want);
    }
    return true;
}

ArchiveSet::ArchiveSet(const std::string& clientRoot) : root_(clientRoot) {
    const char* names[] = {
        "Data\\patch-2.MPQ",        "Data\\patch.MPQ",
        "Data\\expansion.MPQ",      "Data\\common.MPQ",
        "Data\\enUS\\patch-enUS-2.MPQ", "Data\\enUS\\patch-enUS.MPQ",
        "Data\\enUS\\expansion-locale-enUS.MPQ", "Data\\enUS\\locale-enUS.MPQ",
        "Data\\enUS\\base-enUS.MPQ",
    };
    for (const char* name : names) {
        MpqArchive archive;
        std::string full = clientRoot;
        if (!full.empty() && full.back() != '\\' && full.back() != '/') full.push_back('\\');
        full += name;
        if (archive.open(full)) {
            archives_.push_back(std::move(archive));
            ++opened_;
        }
    }
}

bool ArchiveSet::read(const std::string& name, std::vector<uint8_t>& out) const {
    for (const MpqArchive& archive : archives_) {
        if (archive.read(name, out)) return true;
    }
    return false;
}

}  // namespace reclient
