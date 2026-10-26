#include "Formats.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>

namespace reclient {
namespace {

uint32_t u32(const uint8_t* p) {
    uint32_t v = 0;
    std::memcpy(&v, p, 4);
    return v;
}
uint16_t u16(const uint8_t* p) {
    uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}
float f32(const uint8_t* p) {
    float v = 0;
    std::memcpy(&v, p, 4);
    return v;
}

struct Span {
    const uint8_t* p;
    size_t n;
    bool has(size_t at, size_t len) const { return at + len <= n; }
};

// Chunk ids in ADT, WDT, and WMO are stored as a little-endian fourcc, so the
// bytes on disk read backwards from the name used in documentation.
bool chunkId(const uint8_t* p, const char* name) {
    return p[0] == name[3] && p[1] == name[2] && p[2] == name[1] && p[3] == name[0];
}

void rgb565(uint16_t value, uint8_t& r, uint8_t& g, uint8_t& b) {
    r = uint8_t(((value >> 11) & 31) * 255 / 31);
    g = uint8_t(((value >> 5) & 63) * 255 / 63);
    b = uint8_t((value & 31) * 255 / 31);
}

void writePixel(Image& image, int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || y < 0 || x >= image.width || y >= image.height) return;
    uint8_t* d = image.rgba.data() + (size_t(y) * image.width + x) * 4;
    d[0] = r;
    d[1] = g;
    d[2] = b;
    d[3] = a;
}

void decodeDxt1(const uint8_t* data, size_t len, Image& image, bool opaque) {
    const int blocksX = (image.width + 3) / 4;
    const int blocksY = (image.height + 3) / 4;
    size_t pos = 0;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (pos + 8 > len) return;
            const uint16_t c0 = u16(data + pos);
            const uint16_t c1 = u16(data + pos + 2);
            const uint32_t bits = u32(data + pos + 4);
            pos += 8;
            uint8_t color[4][4];
            rgb565(c0, color[0][0], color[0][1], color[0][2]);
            rgb565(c1, color[1][0], color[1][1], color[1][2]);
            color[0][3] = color[1][3] = 255;
            if (opaque || c0 > c1) {
                for (int i = 0; i < 3; ++i) {
                    color[2][i] = uint8_t((2 * color[0][i] + color[1][i]) / 3);
                    color[3][i] = uint8_t((color[0][i] + 2 * color[1][i]) / 3);
                }
                color[2][3] = color[3][3] = 255;
            } else {
                for (int i = 0; i < 3; ++i) color[2][i] = uint8_t((color[0][i] + color[1][i]) / 2);
                color[2][3] = 255;
                color[3][0] = color[3][1] = color[3][2] = 0;
                color[3][3] = 0;
            }
            for (int i = 0; i < 16; ++i) {
                const int idx = (bits >> (2 * i)) & 3;
                writePixel(image, bx * 4 + (i % 4), by * 4 + (i / 4), color[idx][0], color[idx][1], color[idx][2],
                           color[idx][3]);
            }
        }
    }
}

void writeColorBlock(Image& image, int bx, int by, const uint8_t* block, const uint8_t alpha[16]) {
    const uint16_t c0 = u16(block);
    const uint16_t c1 = u16(block + 2);
    const uint32_t bits = u32(block + 4);
    uint8_t color[4][3];
    rgb565(c0, color[0][0], color[0][1], color[0][2]);
    rgb565(c1, color[1][0], color[1][1], color[1][2]);
    for (int i = 0; i < 3; ++i) {
        color[2][i] = uint8_t((2 * color[0][i] + color[1][i]) / 3);
        color[3][i] = uint8_t((color[0][i] + 2 * color[1][i]) / 3);
    }
    for (int i = 0; i < 16; ++i) {
        const int idx = (bits >> (2 * i)) & 3;
        writePixel(image, bx * 4 + (i % 4), by * 4 + (i / 4), color[idx][0], color[idx][1], color[idx][2], alpha[i]);
    }
}

void decodeDxt3(const uint8_t* data, size_t len, Image& image) {
    const int blocksX = (image.width + 3) / 4;
    const int blocksY = (image.height + 3) / 4;
    size_t pos = 0;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (pos + 16 > len) return;
            uint8_t alpha[16];
            for (int i = 0; i < 16; ++i) {
                const uint8_t byte = data[pos + i / 2];
                const uint8_t nibble = (i % 2 == 0) ? (byte & 15) : (byte >> 4);
                alpha[i] = uint8_t(nibble * 17);
            }
            writeColorBlock(image, bx, by, data + pos + 8, alpha);
            pos += 16;
        }
    }
}

void decodeDxt5(const uint8_t* data, size_t len, Image& image) {
    const int blocksX = (image.width + 3) / 4;
    const int blocksY = (image.height + 3) / 4;
    size_t pos = 0;
    for (int by = 0; by < blocksY; ++by) {
        for (int bx = 0; bx < blocksX; ++bx) {
            if (pos + 16 > len) return;
            const uint8_t a0 = data[pos];
            const uint8_t a1 = data[pos + 1];
            uint8_t table[8] = {a0, a1, 0, 0, 0, 0, 0, 255};
            if (a0 > a1) {
                for (int i = 1; i <= 6; ++i) table[i + 1] = uint8_t(((7 - i) * a0 + i * a1) / 7);
            } else {
                for (int i = 1; i <= 4; ++i) table[i + 1] = uint8_t(((5 - i) * a0 + i * a1) / 5);
                table[6] = 0;
                table[7] = 255;
            }
            uint64_t bits = 0;
            for (int b = 0; b < 6; ++b) bits |= uint64_t(data[pos + 2 + b]) << (8 * b);
            uint8_t alpha[16];
            for (int i = 0; i < 16; ++i) alpha[i] = table[(bits >> (3 * i)) & 7];
            writeColorBlock(image, bx, by, data + pos + 8, alpha);
            pos += 16;
        }
    }
}

bool chunkWalk(Span span, const char* want, Span& found) {
    size_t at = 0;
    while (span.has(at, 8)) {
        const size_t size = u32(span.p + at + 4);
        if (!span.has(at + 8, size)) return false;
        if (chunkId(span.p + at, want)) {
            found = Span{span.p + at + 8, size};
            return true;
        }
        at += 8 + size;
    }
    return false;
}

}  // namespace

bool Dbc::load(const uint8_t* data, size_t len) {
    if (len < 20 || std::memcmp(data, "WDBC", 4) != 0) return false;
    records = u32(data + 4);
    fields = u32(data + 8);
    recordSize = u32(data + 12);
    const uint32_t stringSize = u32(data + 16);
    if (recordSize == 0 || fields == 0) return false;
    const uint64_t need = 20ull + uint64_t(records) * recordSize + stringSize;
    if (need > len) return false;
    bytes.assign(data, data + need);
    stringOffset = 20 + size_t(records) * recordSize;
    return true;
}

uint32_t Dbc::get(uint32_t row, uint32_t field) const {
    if (row >= records || field >= fields) return 0;
    const size_t at = 20 + size_t(row) * recordSize + size_t(field) * 4;
    if (at + 4 > bytes.size()) return 0;
    return u32(bytes.data() + at);
}

std::string Dbc::text(uint32_t offset) const {
    if (offset == 0 || stringOffset + offset >= bytes.size()) return {};
    const char* start = reinterpret_cast<const char*>(bytes.data() + stringOffset + offset);
    const char* end = reinterpret_cast<const char*>(bytes.data() + bytes.size());
    const char* stop = start;
    while (stop < end && *stop) ++stop;
    return std::string(start, stop);
}

bool decodeBlp(const uint8_t* data, size_t len, Image& out) {
    if (len < 148 || std::memcmp(data, "BLP2", 4) != 0) return false;
    const uint8_t encoding = data[8];
    const uint8_t alphaDepth = data[9];
    const uint8_t alphaEncoding = data[10];
    const uint32_t width = u32(data + 12);
    const uint32_t height = u32(data + 16);
    const uint32_t offset = u32(data + 20);
    if (width == 0 || height == 0 || width > 4096 || height > 4096 || offset >= len) return false;
    out.width = int(width);
    out.height = int(height);
    out.rgba.assign(size_t(width) * height * 4, 0);
    if (encoding == 1) {
        const uint8_t* palette = data + 148;
        const size_t count = size_t(width) * height;
        if (offset + count > len) return false;
        const uint8_t* indices = data + offset;
        const uint8_t* alpha = indices + count;
        const size_t alphaBytes = len - (offset + count);
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* src = palette + size_t(indices[i]) * 4;
            uint8_t b = src[0], g = src[1], r = src[2], a = src[3];
            if (alphaDepth == 8 && i < alphaBytes) a = alpha[i];
            else if (alphaDepth == 4 && i / 2 < alphaBytes) {
                const uint8_t byte = alpha[i / 2];
                a = uint8_t(((i % 2 == 0 ? byte & 15 : byte >> 4)) * 17);
            } else if (alphaDepth == 1 && i / 8 < alphaBytes) {
                a = (alpha[i / 8] >> (i % 8)) & 1 ? 255 : 0;
            } else if (alphaDepth == 0 && a == 0 && (r || g || b)) {
                a = 255;
            }
            uint8_t* dest = out.rgba.data() + i * 4;
            dest[0] = r;
            dest[1] = g;
            dest[2] = b;
            dest[3] = a;
        }
        return true;
    }
    if (encoding == 2) {
        if (alphaEncoding == 7) {
            decodeDxt5(data + offset, len - offset, out);
            return true;
        }
        if (alphaEncoding == 1) {
            decodeDxt3(data + offset, len - offset, out);
            return true;
        }
        const bool opaque = alphaDepth == 0;
        decodeDxt1(data + offset, len - offset, out, opaque || alphaDepth != 1);
        return true;
    }
    if (encoding == 3) {
        const size_t count = size_t(width) * height * 4;
        if (offset + count > len) return false;
        for (size_t i = 0; i < count; i += 4) {
            out.rgba[i] = data[offset + i + 2];
            out.rgba[i + 1] = data[offset + i + 1];
            out.rgba[i + 2] = data[offset + i];
            out.rgba[i + 3] = data[offset + i + 3];
        }
        return true;
    }
    return false;
}

bool loadM2(const uint8_t* data, size_t len, Mesh& out) {
    if (len < 0x14 + 20 * 8 || std::memcmp(data, "MD20", 4) != 0) return false;
    const uint32_t version = u32(data + 4);
    if (version < 256 || version > 264) return false;
    auto arrayAt = [&](int index, uint32_t& count, uint32_t& offset) {
        const size_t at = 0x14 + size_t(index) * 8;
        count = u32(data + at);
        offset = u32(data + at + 4);
    };
    uint32_t vertexCount = 0, vertexOfs = 0, skinCount = 0, skinOfs = 0;
    arrayAt(6, vertexCount, vertexOfs);
    arrayAt(7, skinCount, skinOfs);
    if (vertexCount == 0 || vertexCount > 200000) return false;
    if (uint64_t(vertexOfs) + uint64_t(vertexCount) * 48 > len) return false;
    out.xyz.resize(size_t(vertexCount) * 3);
    out.uv.resize(size_t(vertexCount) * 2);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const uint8_t* v = data + vertexOfs + size_t(i) * 48;
        out.xyz[i * 3] = f32(v);
        out.xyz[i * 3 + 1] = f32(v + 4);
        out.xyz[i * 3 + 2] = f32(v + 8);
        out.uv[i * 2] = f32(v + 32);
        out.uv[i * 2 + 1] = f32(v + 36);
    }
    if (skinCount == 0 || uint64_t(skinOfs) + 44 > len) return true;
    const uint8_t* skin = data + skinOfs;
    const uint32_t lookupCount = u32(skin);
    const uint32_t lookupOfs = u32(skin + 4);
    const uint32_t indexCount = u32(skin + 8);
    const uint32_t indexOfs = u32(skin + 12);
    if (indexCount == 0 || indexCount > 600000 || uint64_t(indexOfs) + uint64_t(indexCount) * 2 > len) return true;
    if (lookupCount == 0 || lookupCount > vertexCount || uint64_t(lookupOfs) + uint64_t(lookupCount) * 2 > len) return true;
    out.indices.reserve(indexCount);
    for (uint32_t i = 0; i < indexCount; ++i) {
        const uint32_t local = u16(data + indexOfs + size_t(i) * 2);
        if (local >= lookupCount) continue;
        const uint32_t idx = u16(data + lookupOfs + size_t(local) * 2);
        if (idx < vertexCount) out.indices.push_back(idx);
    }
    return true;
}

bool loadWdt(const uint8_t* data, size_t len, MapInfo& out) {
    Span main{};
    if (!chunkWalk(Span{data, len}, "MAIN", main) || main.n < 64u * 64u * 8u) return false;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const uint32_t flags = u32(main.p + (size_t(y) * 64 + x) * 8);
            out.present[x][y] = (flags & 1) != 0;
        }
    }
    Span names{};
    if (chunkWalk(Span{data, len}, "MWMO", names)) {
        size_t at = 0;
        while (at < names.n) {
            if (names.p[at] == 0) {
                ++at;
                continue;
            }
            const char* start = reinterpret_cast<const char*>(names.p + at);
            const size_t room = names.n - at;
            const size_t end = ::strnlen(start, room);
            out.models.emplace_back(start, end);
            at += end + (end < room ? 1 : 0);
        }
    }
    return true;
}

bool loadAdt(const uint8_t* data, size_t len, int tileX, int tileY, Terrain& out) {
    constexpr float kTile = 533.33333f;
    constexpr float kChunk = kTile / 16.f;
    constexpr float kStep = kChunk / 8.f;
    const float originX = (32.f - float(tileX)) * kTile;
    const float originY = (32.f - float(tileY)) * kTile;
    out.tileX = tileX;
    out.tileY = tileY;
    std::vector<std::string> textures;
    bool any = false;
    size_t at = 0;
    while (at + 8 <= len) {
        const size_t size = u32(data + at + 4);
        if (at + 8 + size > len) break;
        if (chunkId(data + at, "MTEX")) {
            const uint8_t* names = data + at + 8;
            size_t i = 0;
            while (i < size) {
                if (names[i] == 0) {
                    ++i;
                    continue;
                }
                const char* text = reinterpret_cast<const char*>(names + i);
                const size_t room = size - i;
                const size_t end = ::strnlen(text, room);
                textures.emplace_back(text, end);
                i += end + (end < room ? 1 : 0);
            }
        } else if (chunkId(data + at, "MCNK") && size > 128) {
            const uint8_t* chunk = data + at + 8;
            const uint32_t ix = u32(chunk + 4);
            const uint32_t iy = u32(chunk + 8);
            const uint8_t* heights = nullptr;
            const uint8_t* normals = nullptr;
            const uint8_t* layers = nullptr;
            size_t layerBytes = 0;
            const uint8_t* alpha = nullptr;
            size_t alphaBytes = 0;
            size_t inner = size > 0x80 ? 0x80 : 0;
            while (inner + 8 <= size) {
                const size_t sub = u32(chunk + inner + 4);
                if (inner + 8 + sub > size) break;
                const uint8_t* body = chunk + inner + 8;
                const bool normalPad = chunkId(chunk + inner, "MCNR") && sub == 435;
                if (chunkId(chunk + inner, "MCVT") && sub >= 145 * 4) heights = body;
                else if (normalPad) normals = body;
                else if (chunkId(chunk + inner, "MCLY") && sub >= 16) {
                    layers = body;
                    layerBytes = sub;
                } else if (chunkId(chunk + inner, "MCAL") && sub > 0) {
                    alpha = body;
                    alphaBytes = sub;
                }
                inner += 8 + sub;
                // Normals are 435 bytes and then 13 bytes of pad, which the size does not count.
                if (normalPad) inner += 13;
            }
            if (heights && ix < 16 && iy < 16) {
                // Chunk origin is world x, y, z at 0x68. MCVT heights are relative to that z.
                const float baseZ = f32(chunk + 0x70);
                const float baseX = originX - float(ix) * kChunk;
                const float baseY = originY - float(iy) * kChunk;
                if (out.height.size() != 256u * 145u)
                    out.height.assign(256u * 145u, std::numeric_limits<float>::quiet_NaN());
                if (out.holes.size() != 256u) out.holes.assign(256u, 0);
                if (out.area.size() != 256u) out.area.assign(256u, 0);
                // The low 16 bits are the 4 by 4 holes. Ironforge's mountain is open where the city sits.
                out.holes[size_t(iy) * 16u + ix] = u32(chunk + 0x3C) & 0xFFFFu;
                out.area[size_t(iy) * 16u + ix] = u32(chunk + 0x34);
                float* column = out.height.data() + (size_t(iy) * 16u + ix) * 145u;
                for (int i = 0; i < 145; ++i) column[i] = baseZ + f32(heights + size_t(i) * 4);
                const uint32_t vertBegin = uint32_t(out.xyz.size() / 3);
                std::vector<float> localU;
                std::vector<float> localV;
                auto vert = [&](int row, int col, bool innerVert) -> uint32_t {
                    const int index = innerVert ? (row * 17 + 9 + col) : (row * 17 + col);
                    const float h = f32(heights + size_t(index) * 4);
                    const float alongY = baseX - (innerVert ? (float(col) + 0.5f) * kStep : float(col) * kStep);
                    const float alongX = baseY - (innerVert ? (float(row) + 0.5f) * kStep : float(row) * kStep);
                    const float z = baseZ + h;
                    const uint32_t id = uint32_t(out.xyz.size() / 3);
                    out.xyz.push_back(alongX);
                    out.xyz.push_back(alongY);
                    out.xyz.push_back(z);
                    // Ground art repeats every few yards, the same span the tileset was drawn for.
                    out.uv.push_back(alongX / 8.f);
                    out.uv.push_back(alongY / 8.f);
                    float facing = 1.f;
                    if (normals) {
                        int8_t nz = 0;
                        std::memcpy(&nz, normals + size_t(index) * 3 + 2, 1);
                        facing = std::clamp(float(nz) / 127.f, 0.f, 1.f);
                    }
                    out.shade.push_back(uint8_t(std::clamp(facing * 255.f, 0.f, 255.f)));
                    localU.push_back(innerVert ? (float(col) + 0.5f) / 8.f : float(col) / 8.f);
                    localV.push_back(innerVert ? (float(row) + 0.5f) / 8.f : float(row) / 8.f);
                    if (!any) {
                        out.minX = out.maxX = alongX;
                        out.minY = out.maxY = alongY;
                        out.minZ = out.maxZ = z;
                        any = true;
                    }
                    if (alongX < out.minX) out.minX = alongX;
                    if (alongX > out.maxX) out.maxX = alongX;
                    if (alongY < out.minY) out.minY = alongY;
                    if (alongY > out.maxY) out.maxY = alongY;
                    if (z < out.minZ) out.minZ = z;
                    if (z > out.maxZ) out.maxZ = z;
                    return id;
                };
                const uint32_t chunkHoles = out.holes[size_t(iy) * 16u + ix];
                std::vector<uint32_t> tris;
                for (int row = 0; row < 8; ++row) {
                    for (int col = 0; col < 8; ++col) {
                        if (chunkHoles & (1u << ((row / 2) * 4 + col / 2))) continue;
                        const uint32_t t0 = vert(row, col, false);
                        const uint32_t t1 = vert(row, col + 1, false);
                        const uint32_t t2 = vert(row + 1, col, false);
                        const uint32_t t3 = vert(row + 1, col + 1, false);
                        const uint32_t c = vert(row, col, true);
                        const uint32_t tri[12] = {t0, t1, c, t1, t3, c, t3, t2, c, t2, t0, c};
                        tris.insert(tris.end(), tri, tri + 12);
                    }
                }
                out.indices.insert(out.indices.end(), tris.begin(), tris.end());
                auto bounds = [&](float& minX, float& minY, float& minZ, float& maxX, float& maxY, float& maxZ) {
                    minX = minY = minZ = 1.0e9f;
                    maxX = maxY = maxZ = -1.0e9f;
                    for (size_t v = vertBegin; v < out.xyz.size() / 3; ++v) {
                        const float x = out.xyz[v * 3];
                        const float y = out.xyz[v * 3 + 1];
                        const float z = out.xyz[v * 3 + 2];
                        if (x < minX) minX = x;
                        if (y < minY) minY = y;
                        if (z < minZ) minZ = z;
                        if (x > maxX) maxX = x;
                        if (y > maxY) maxY = y;
                        if (z > maxZ) maxZ = z;
                    }
                };
                auto cover = [&](Terrain::Surface& surface) {
                    Terrain::Surface::Patch patch;
                    patch.first = uint32_t(surface.indices.size());
                    patch.count = uint32_t(tris.size());
                    bounds(patch.minX, patch.minY, patch.minZ, patch.maxX, patch.maxY, patch.maxZ);
                    surface.indices.insert(surface.indices.end(), tris.begin(), tris.end());
                    surface.patches.push_back(patch);
                };
                if (!tris.empty() && layers && layerBytes >= 16) {
                    const size_t layerCount = layerBytes / 16;
                    auto sample = [&](const uint8_t* map, size_t mapBytes, float u, float v, bool wide) -> uint8_t {
                        if (!map || mapBytes < 64) return 255;
                        const int x = std::clamp(int(u * 64.f), 0, 63);
                        const int y = std::clamp(int(v * 64.f), 0, 63);
                        if (wide && mapBytes >= 4096) return map[size_t(y) * 64 + x];
                        // Four bits per texel, low nibble first, still 64 across. One layer is 2048 bytes.
                        const size_t packed = size_t(y) * 32 + size_t(x / 2);
                        if (packed >= mapBytes) return 255;
                        const uint8_t byte = map[packed];
                        const uint8_t nibble = (x & 1) ? (byte >> 4) : (byte & 0x0f);
                        return uint8_t(nibble * 17);
                    };
                    for (size_t layer = 0; layer < layerCount && layer < 4; ++layer) {
                        const uint8_t* rec = layers + layer * 16;
                        const uint32_t texture = u32(rec);
                        const uint32_t flags = u32(rec + 4);
                        const uint32_t alphaAt = u32(rec + 8);
                        // 0x200 is the later 8-bit alpha. 2.4.3 layers are 4-bit unless that bit is set.
                        const bool wide = (flags & 0x200u) != 0;
                        std::string path = texture < textures.size() ? textures[texture] : std::string();
                        for (char& c : path)
                            if (c == '/') c = '\\';
                        if (layer == 0) {
                            Terrain::Surface* surface = nullptr;
                            for (Terrain::Surface& have : out.surfaces)
                                if (have.path == path) surface = &have;
                            if (!surface) {
                                out.surfaces.push_back({});
                                surface = &out.surfaces.back();
                                surface->path = path;
                            }
                            cover(*surface);
                            continue;
                        }
                        if (!alpha || alphaAt >= alphaBytes) continue;
                        Terrain::Coat coat;
                        coat.path = std::move(path);
                        coat.vertBegin = vertBegin;
                        coat.indices = tris;
                        coat.alpha.resize(localU.size());
                        const uint8_t* map = alpha + alphaAt;
                        const size_t mapBytes = alphaBytes - alphaAt;
                        for (size_t v = 0; v < localU.size(); ++v)
                            coat.alpha[v] = sample(map, mapBytes, localU[v], localV[v], wide);
                        bounds(coat.minX, coat.minY, coat.minZ, coat.maxX, coat.maxY, coat.maxZ);
                        out.coats.push_back(std::move(coat));
                    }
                } else if (!tris.empty()) {
                    Terrain::Surface* surface = nullptr;
                    for (Terrain::Surface& have : out.surfaces)
                        if (have.path.empty()) surface = &have;
                    if (!surface) {
                        out.surfaces.push_back({});
                        surface = &out.surfaces.back();
                    }
                    cover(*surface);
                }
            }
        }
        at += 8 + size;
    }
    return any;
}

bool terrainHeight(const Terrain& ground, float x, float y, float& z) {
    if (ground.height.size() != 256u * 145u || ground.tileX < 0) return false;
    constexpr float kTile = 533.33333f;
    constexpr float kChunk = kTile / 16.f;
    constexpr float kStep = kChunk / 8.f;
    const float originX = (32.f - float(ground.tileX)) * kTile;
    const float originY = (32.f - float(ground.tileY)) * kTile;
    const float localX = originY - x;
    const float localY = originX - y;
    if (localX < 0.f || localY < 0.f || localX >= kTile || localY >= kTile) return false;
    const int iy = std::min(15, int(localX / kChunk));
    const int ix = std::min(15, int(localY / kChunk));
    const float fy = (localX - float(iy) * kChunk) / kStep;
    const float fx = (localY - float(ix) * kChunk) / kStep;
    const int row = std::min(7, int(fy));
    const int col = std::min(7, int(fx));
    if (ground.holes.size() == 256u) {
        const uint32_t bits = ground.holes[size_t(iy) * 16 + ix];
        if (bits & (1u << ((row / 2) * 4 + col / 2))) return false;
    }
    const float* h = ground.height.data() + (size_t(iy) * 16 + ix) * 145;
    auto at = [&](int r, int c, bool inner) {
        const int index = inner ? (r * 17 + 9 + c) : (r * 17 + c);
        return h[index];
    };
    const float z00 = at(row, col, false);
    const float z10 = at(row, col + 1, false);
    const float z01 = at(row + 1, col, false);
    const float z11 = at(row + 1, col + 1, false);
    const float zc = at(row, col, true);
    if (!std::isfinite(z00) || !std::isfinite(z10) || !std::isfinite(z01) || !std::isfinite(z11) || !std::isfinite(zc))
        return false;
    const float u = fx - float(col);
    const float v = fy - float(row);
    auto bary = [](float px, float py, float ax, float ay, float bx, float by, float cx, float cy, float& w0, float& w1,
                   float& w2) {
        const float det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
        if (std::fabs(det) < 1e-8f) return false;
        w0 = ((by - cy) * (px - cx) + (cx - bx) * (py - cy)) / det;
        w1 = ((cy - ay) * (px - cx) + (ax - cx) * (py - cy)) / det;
        w2 = 1.f - w0 - w1;
        return w0 >= -0.02f && w1 >= -0.02f && w2 >= -0.02f;
    };
    auto pick = [&](float ax, float ay, float az, float bx, float by, float bz, float cx, float cy, float cz) {
        float w0, w1, w2;
        if (!bary(u, v, ax, ay, bx, by, cx, cy, w0, w1, w2)) return false;
        z = w0 * az + w1 * bz + w2 * cz;
        return true;
    };
    if (pick(0.f, 0.f, z00, 1.f, 0.f, z10, 0.5f, 0.5f, zc)) return true;
    if (pick(1.f, 0.f, z10, 1.f, 1.f, z11, 0.5f, 0.5f, zc)) return true;
    if (pick(1.f, 1.f, z11, 0.f, 1.f, z01, 0.5f, 0.5f, zc)) return true;
    return pick(0.f, 1.f, z01, 0.f, 0.f, z00, 0.5f, 0.5f, zc);
}

uint32_t terrainArea(const Terrain& ground, float x, float y) {
    if (ground.area.size() != 256u || ground.tileX < 0) return 0;
    constexpr float kTile = 533.33333f;
    constexpr float kChunk = kTile / 16.f;
    const float originX = (32.f - float(ground.tileX)) * kTile;
    const float originY = (32.f - float(ground.tileY)) * kTile;
    const float localX = originY - x;
    const float localY = originX - y;
    if (localX < 0.f || localY < 0.f || localX >= kTile || localY >= kTile) return 0;
    const int iy = std::min(15, int(localX / kChunk));
    const int ix = std::min(15, int(localY / kChunk));
    return ground.area[size_t(iy) * 16 + ix];
}

int wmoGroupCount(const uint8_t* data, size_t len) {
    Span header{};
    if (!chunkWalk(Span{data, len}, "MOHD", header) || header.n < 8) return 0;
    return int(u32(header.p + 4));
}

bool loadWmo(const uint8_t* data, size_t len, Mesh& out) {
    size_t at = 0;
    const uint8_t* verts = nullptr;
    size_t vertBytes = 0;
    const uint8_t* indices = nullptr;
    size_t indexBytes = 0;
    std::function<void(const uint8_t*, size_t)> consider = [&](const uint8_t* p, size_t n) {
        size_t cursor = 0;
        while (cursor + 8 <= n) {
            const size_t size = u32(p + cursor + 4);
            if (cursor + 8 + size > n) break;
            if (chunkId(p + cursor, "MOVT")) {
                verts = p + cursor + 8;
                vertBytes = size;
            } else if (chunkId(p + cursor, "MOVI")) {
                indices = p + cursor + 8;
                indexBytes = size;
            } else if (chunkId(p + cursor, "MOGP") && size > 0x44) {
                consider(p + cursor + 8 + 0x44, size - 0x44);
            }
            cursor += 8 + size;
        }
    };
    consider(data, len);
    if (!verts || vertBytes < 12 || !indices || indexBytes < 6) return false;
    const size_t count = vertBytes / 12;
    if (count > 500000) return false;
    out.xyz.resize(count * 3);
    for (size_t i = 0; i < count; ++i) {
        out.xyz[i * 3] = f32(verts + i * 12);
        out.xyz[i * 3 + 1] = f32(verts + i * 12 + 4);
        out.xyz[i * 3 + 2] = f32(verts + i * 12 + 8);
    }
    const size_t indexCount = indexBytes / 2;
    out.indices.reserve(indexCount);
    for (size_t i = 0; i < indexCount; ++i) {
        const uint32_t idx = u16(indices + i * 2);
        if (idx < count) out.indices.push_back(idx);
    }
    return out.indices.size() >= 3;
}

void multiply(const float a[16], const float b[16], float o[16]) {
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) {
            o[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
        }
    }
}

Mat4 rotationX(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 m;
    m.m[5] = c;
    m.m[6] = s;
    m.m[9] = -s;
    m.m[10] = c;
    return m;
}

Mat4 rotationY(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 m;
    m.m[0] = c;
    m.m[2] = -s;
    m.m[8] = s;
    m.m[10] = c;
    return m;
}

Mat4 rotationZ(float radians) {
    const float c = std::cos(radians);
    const float s = std::sin(radians);
    Mat4 m;
    m.m[0] = c;
    m.m[1] = s;
    m.m[4] = -s;
    m.m[5] = c;
    return m;
}

Mat4 translation(float x, float y, float z) {
    Mat4 m;
    m.m[12] = x;
    m.m[13] = y;
    m.m[14] = z;
    return m;
}

Mat4 scaleMatrix(float s) {
    Mat4 m;
    m.m[0] = s;
    m.m[5] = s;
    m.m[10] = s;
    return m;
}

Mat4 product(const Mat4& a, const Mat4& b) {
    Mat4 o;
    multiply(a.m, b.m, o.m);
    return o;
}

bool wornTransform(const Mat4& placed, const LitMesh& body, uint32_t attachment, Mat4& out) {
    if (attachment >= body.attachmentOf.size()) return false;
    const int16_t index = body.attachmentOf[attachment];
    if (index < 0 || size_t(index) >= body.attachments.size()) return false;
    const ModelAttachment& att = body.attachments[size_t(index)];
    float bone[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    if (size_t(att.bone) < body.boneMats.size() / 16) std::memcpy(bone, body.boneMats.data() + size_t(att.bone) * 16, sizeof(bone));
    const Mat4 shift = translation(att.x, att.y, att.z);
    Mat4 local;
    multiply(bone, shift.m, local.m);
    out = product(placed, local);
    return true;
}

constexpr float kMapOrigin = 32.f * 533.33333f;

Mat4 mapPlacement(float px, float py, float pz, float rx, float ry, float rz, float scale) {
    constexpr float kDeg = 0.0174532925f;
    const Mat4 spin = product(product(rotationY((ry - 270.f) * kDeg), rotationZ(-rx * kDeg)), rotationX((rz - 90.f) * kDeg));
    const Mat4 placed = product(translation(kMapOrigin - px, py, kMapOrigin - pz), spin);
    const Mat4 upright = product(rotationX(90.f * kDeg), rotationY(90.f * kDeg));
    Mat4 world = product(upright, placed);
    if (scale != 1.f) world = product(world, scaleMatrix(scale));
    return world;
}

Mat4 unitPlacement(float x, float y, float z, float facing, float scale) {
    // Same model-space conversion the map doodads use, then the server facing.
    // Facing 0 looks north (+x) and model +z stays up, so pitch cannot tip the body.
    constexpr float kDeg = 0.0174532925f;
    const float yawDeg = facing * (180.f / 3.14159265f) + 180.f;
    const Mat4 spin = product(product(rotationY((yawDeg - 270.f) * kDeg), rotationZ(0.f)), rotationX(-90.f * kDeg));
    const Mat4 upright = product(rotationX(90.f * kDeg), rotationY(90.f * kDeg));
    Mat4 linear = product(upright, spin);
    if (scale != 1.f) linear = product(linear, scaleMatrix(scale));
    return product(translation(x, y, z), linear);
}

void transformPoint(const Mat4& m, float x, float y, float z, float& ox, float& oy, float& oz) {
    ox = m.m[0] * x + m.m[4] * y + m.m[8] * z + m.m[12];
    oy = m.m[1] * x + m.m[5] * y + m.m[9] * z + m.m[13];
    oz = m.m[2] * x + m.m[6] * y + m.m[10] * z + m.m[14];
}

std::string chunkString(const uint8_t* data, size_t len, size_t offset) {
    if (offset >= len) return {};
    const char* start = reinterpret_cast<const char*>(data + offset);
    const size_t room = len - offset;
    return std::string(start, ::strnlen(start, room));
}

bool findChunk(const uint8_t* data, size_t len, const char* name, const uint8_t*& payload, size_t& size) {
    size_t at = 0;
    while (at + 8 <= len) {
        const size_t n = u32(data + at + 4);
        if (at + 8 + n > len) return false;
        if (chunkId(data + at, name)) {
            payload = data + at + 8;
            size = n;
            return true;
        }
        at += 8 + n;
    }
    return false;
}

void addPlacement(std::vector<MapPlacement>& out, std::string path, bool wmo, float px, float py, float pz, float rx,
                  float ry, float rz, float scale, const float* box) {
    for (char& c : path)
        if (c == '/') c = '\\';
    if (path.size() >= 4) {
        const std::string ext = path.substr(path.size() - 4);
        if (ext == ".mdx" || ext == ".MDX") path.replace(path.size() - 4, 4, ".m2");
    }
    MapPlacement place;
    place.path = std::move(path);
    place.wmo = wmo;
    place.scale = scale;
    place.world = mapPlacement(px, py, pz, rx, ry, rz, scale);
    if (box) {
        float xs[2] = {kMapOrigin - box[5], kMapOrigin - box[2]};
        float ys[2] = {kMapOrigin - box[3], kMapOrigin - box[0]};
        place.minX = std::min(xs[0], xs[1]);
        place.maxX = std::max(xs[0], xs[1]);
        place.minY = std::min(ys[0], ys[1]);
        place.maxY = std::max(ys[0], ys[1]);
        place.minZ = std::min(box[1], box[4]);
        place.maxZ = std::max(box[1], box[4]);
        place.hasBox = true;
    }
    out.push_back(std::move(place));
}

bool loadAdtPlacements(const uint8_t* data, size_t len, std::vector<MapPlacement>& out) {
    const uint8_t* names = nullptr;
    size_t namesN = 0;
    const uint8_t* ids = nullptr;
    size_t idsN = 0;
    const uint8_t* defs = nullptr;
    size_t defsN = 0;
    const uint8_t* wnames = nullptr;
    size_t wnamesN = 0;
    const uint8_t* wids = nullptr;
    size_t widsN = 0;
    const uint8_t* wdefs = nullptr;
    size_t wdefsN = 0;
    size_t at = 0;
    while (at + 8 <= len) {
        const size_t n = u32(data + at + 4);
        if (at + 8 + n > len) break;
        const uint8_t* p = data + at;
        if (chunkId(p, "MMDX")) {
            names = p + 8;
            namesN = n;
        } else if (chunkId(p, "MMID")) {
            ids = p + 8;
            idsN = n;
        } else if (chunkId(p, "MDDF")) {
            defs = p + 8;
            defsN = n;
        } else if (chunkId(p, "MWMO")) {
            wnames = p + 8;
            wnamesN = n;
        } else if (chunkId(p, "MWID")) {
            wids = p + 8;
            widsN = n;
        } else if (chunkId(p, "MODF")) {
            wdefs = p + 8;
            wdefsN = n;
        }
        at += 8 + n;
    }
    auto nameAt = [](const uint8_t* blob, size_t blobN, const uint8_t* table, size_t tableN, uint32_t id) {
        if (!blob || !table || id * 4 + 4 > tableN) return std::string();
        const uint32_t offset = u32(table + id * 4);
        return chunkString(blob, blobN, offset);
    };
    for (size_t i = 0; i + 64 <= wdefsN; i += 64) {
        const uint8_t* e = wdefs + i;
        float box[6];
        for (int k = 0; k < 6; ++k) box[k] = f32(e + 32 + k * 4);
        addPlacement(out, nameAt(wnames, wnamesN, wids, widsN, u32(e)), true, f32(e + 8), f32(e + 12), f32(e + 16),
                     f32(e + 20), f32(e + 24), f32(e + 28), 1.f, box);
    }
    for (size_t i = 0; i + 36 <= defsN; i += 36) {
        const uint8_t* e = defs + i;
        const uint16_t rawScale = u16(e + 32);
        const float scale = rawScale == 0 ? 1.f : float(rawScale) / 1024.f;
        addPlacement(out, nameAt(names, namesN, ids, idsN, u32(e)), false, f32(e + 8), f32(e + 12), f32(e + 16),
                     f32(e + 20), f32(e + 24), f32(e + 28), scale, nullptr);
    }
    return !out.empty();
}

bool loadWmoRoot(const uint8_t* data, size_t len, WmoRoot& out) {
    const uint8_t* motx = nullptr;
    size_t motxN = 0;
    const uint8_t* momt = nullptr;
    size_t momtN = 0;
    const uint8_t* mogi = nullptr;
    size_t mogiN = 0;
    size_t at = 0;
    while (at + 8 <= len) {
        const size_t n = u32(data + at + 4);
        if (at + 8 + n > len) break;
        if (chunkId(data + at, "MOTX")) {
            motx = data + at + 8;
            motxN = n;
        } else if (chunkId(data + at, "MOMT")) {
            momt = data + at + 8;
            momtN = n;
        } else if (chunkId(data + at, "MOGI")) {
            mogi = data + at + 8;
            mogiN = n;
        } else if (chunkId(data + at, "MOHD") && n >= 36) {
            out.wmoId = u32(data + at + 8 + 32);
        }
        at += 8 + n;
    }
    if (!momt || momtN < 64) return false;
    const size_t materials = momtN / 64;
    out.materials.resize(materials);
    for (size_t i = 0; i < materials; ++i) {
        const uint8_t* p = momt + i * 64;
        out.materials[i].twoSided = (u32(p) & 4) != 0;
        std::string path = chunkString(motx, motxN, u32(p + 12));
        for (char& c : path)
            if (c == '/') c = '\\';
        out.materials[i].texture = std::move(path);
    }
    for (size_t i = 0; i + 32 <= mogiN; i += 32) {
        WmoGroupBox box;
        box.flags = u32(mogi + i);
        for (int k = 0; k < 3; ++k) {
            box.min[k] = f32(mogi + i + 4 + k * 4);
            box.max[k] = f32(mogi + i + 16 + k * 4);
        }
        out.groups.push_back(box);
    }
    return !out.groups.empty();
}

int32_t wmoGroupFileId(const uint8_t* data, size_t len) {
    const uint8_t* mogp = nullptr;
    size_t mogpN = 0;
    if (!findChunk(data, len, "MOGP", mogp, mogpN) || mogpN < 0x3C) return -1;
    return int32_t(u32(mogp + 0x38));
}

bool loadWmoGroupMesh(const uint8_t* data, size_t len, const Mat4& world, const std::vector<WmoMaterial>& materials,
                      LitMesh& out) {
    const uint8_t* mogp = nullptr;
    size_t mogpN = 0;
    if (!findChunk(data, len, "MOGP", mogp, mogpN) || mogpN < 0x44) return false;
    const uint8_t* sub = mogp + 0x44;
    size_t subN = mogpN - 0x44;
    const uint8_t* verts = nullptr;
    size_t vertN = 0;
    const uint8_t* indices = nullptr;
    size_t indexN = 0;
    const uint8_t* uvs = nullptr;
    size_t uvN = 0;
    const uint8_t* colors = nullptr;
    size_t colorN = 0;
    const uint8_t* batches = nullptr;
    size_t batchN = 0;
    size_t at = 0;
    while (at + 8 <= subN) {
        const size_t n = u32(sub + at + 4);
        if (at + 8 + n > subN) break;
        if (chunkId(sub + at, "MOVT")) {
            verts = sub + at + 8;
            vertN = n;
        } else if (chunkId(sub + at, "MOVI")) {
            indices = sub + at + 8;
            indexN = n;
        } else if (chunkId(sub + at, "MOTV") && !uvs) {
            uvs = sub + at + 8;
            uvN = n;
        } else if (chunkId(sub + at, "MOCV") && !colors) {
            colors = sub + at + 8;
            colorN = n;
        } else if (chunkId(sub + at, "MOBA")) {
            batches = sub + at + 8;
            batchN = n;
        }
        at += 8 + n;
    }
    if (!verts || vertN < 12 || !indices || indexN < 6) return false;
    const size_t count = vertN / 12;
    if (count > 500000) return false;
    out.xyz.resize(count * 3);
    out.uv.resize(count * 2);
    out.rgb.assign(count * 3, 255);
    for (size_t i = 0; i < count; ++i) {
        transformPoint(world, f32(verts + i * 12), f32(verts + i * 12 + 4), f32(verts + i * 12 + 8), out.xyz[i * 3],
                       out.xyz[i * 3 + 1], out.xyz[i * 3 + 2]);
        if (uvs && (i + 1) * 8 <= uvN) {
            out.uv[i * 2] = f32(uvs + i * 8);
            out.uv[i * 2 + 1] = f32(uvs + i * 8 + 4);
        }
    }
    if (colors && colorN >= count * 4) {
        uint64_t sum = 0;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* c = colors + i * 4;
            out.rgb[i * 3] = c[2];
            out.rgb[i * 3 + 1] = c[1];
            out.rgb[i * 3 + 2] = c[0];
            sum += c[0] + c[1] + c[2];
        }
        if (sum / count < 12) out.rgb.assign(count * 3, 220);
    }
    const size_t indexCount = indexN / 2;
    std::vector<uint32_t> all(indexCount);
    for (size_t i = 0; i < indexCount; ++i) all[i] = u16(indices + i * 2);
    const size_t stride = batchN >= 24 && batchN % 24 == 0 ? 24 : batchN % 32 == 0 ? 32 : 0;
    if (!batches || stride == 0) return false;
    for (size_t i = 0; i + stride <= batchN; i += stride) {
        const uint8_t* b = batches + i;
        const uint32_t start = u32(b + 12);
        uint32_t countIdx = u16(b + 16);
        const uint32_t material = b[23];
        if (countIdx < 3 || start >= all.size()) continue;
        if (start + countIdx > all.size()) countIdx = uint32_t(all.size() - start);
        countIdx -= countIdx % 3;
        if (countIdx < 3) continue;
        PrimBatch batch;
        batch.start = uint32_t(out.indices.size());
        batch.twoSided = material < materials.size() && materials[material].twoSided;
        batch.texture = material < materials.size() ? int(material) : -1;
        bool kept = false;
        for (uint32_t n = 0; n < countIdx; ++n) {
            const uint32_t idx = all[start + n];
            if (idx >= count) {
                kept = false;
                break;
            }
            out.indices.push_back(idx);
            kept = true;
        }
        if (!kept) {
            out.indices.resize(batch.start);
            continue;
        }
        batch.count = uint32_t(out.indices.size()) - batch.start;
        out.batches.push_back(batch);
    }
    return !out.batches.empty();
}

namespace {

struct TrackHead {
    uint16_t interp = 0;
    int16_t globalSeq = -1;
    uint32_t nRanges = 0, ofsRanges = 0, nTimes = 0, ofsTimes = 0, nKeys = 0, ofsKeys = 0;
    bool ok = false;
};

TrackHead readTrack(const uint8_t* data, size_t len, size_t off) {
    TrackHead head;
    if (off + 28 > len) return head;
    head.interp = u16(data + off);
    std::memcpy(&head.globalSeq, data + off + 2, 2);
    head.nRanges = u32(data + off + 4);
    head.ofsRanges = u32(data + off + 8);
    head.nTimes = u32(data + off + 12);
    head.ofsTimes = u32(data + off + 16);
    head.nKeys = u32(data + off + 20);
    head.ofsKeys = u32(data + off + 24);
    head.ok = true;
    return head;
}

float packedQuat(int16_t value) {
    return ((value < 0 ? float(value) + 32768.f : float(value) - 32767.f) / 32767.f);
}

bool readTrackKey(const uint8_t* data, size_t len, const TrackHead& head, uint32_t index, bool quat, float* dest) {
    const int base = quat ? 8 : 12;
    const int stride = base * (head.interp >= 2 ? 3 : 1);
    const size_t at = size_t(head.ofsKeys) + size_t(index) * stride;
    if (!head.ok || index >= head.nKeys || at + size_t(base) > len) return false;
    if (quat) {
        int16_t q[4];
        std::memcpy(q, data + at, 8);
        dest[0] = packedQuat(q[0]);
        dest[1] = packedQuat(q[1]);
        dest[2] = packedQuat(q[2]);
        dest[3] = packedQuat(q[3]);
        const float n = dest[0] * dest[0] + dest[1] * dest[1] + dest[2] * dest[2] + dest[3] * dest[3];
        if (n < 1e-8f) {
            dest[0] = dest[1] = dest[2] = 0;
            dest[3] = 1;
        } else {
            const float inv = 1.f / std::sqrt(n);
            dest[0] *= inv;
            dest[1] *= inv;
            dest[2] *= inv;
            dest[3] *= inv;
        }
    } else {
        dest[0] = f32(data + at);
        dest[1] = f32(data + at + 4);
        dest[2] = f32(data + at + 8);
    }
    return true;
}

void fillSlice(const uint8_t* data, size_t len, const TrackHead& head, uint32_t anim, bool quat, bool windowed,
               uint32_t windowStart, uint32_t windowEnd, uint32_t globalLoop, BoneTrack& out) {
    if (!head.ok || head.nTimes == 0 || head.nKeys == 0) return;
    uint32_t first = 0;
    uint32_t last = 0;
    bool filter = windowed;
    if (head.globalSeq >= 0) {
        last = std::min(head.nTimes, head.nKeys) - 1;
        filter = false;
        out.loopMs = globalLoop;
    } else {
        if (anim == 0xffffffffu || anim >= head.nRanges) return;
        const size_t rangeAt = size_t(head.ofsRanges) + size_t(anim) * 8;
        if (rangeAt + 8 > len) return;
        first = u32(data + rangeAt);
        last = u32(data + rangeAt + 4);
        if (last < first || first >= head.nTimes || first >= head.nKeys) return;
        if (last >= head.nTimes) last = head.nTimes - 1;
        if (last >= head.nKeys) last = head.nKeys - 1;
    }
    if (last > first + 4096) last = first + 4096;
    const int width = quat ? 4 : 3;
    uint32_t origin = filter ? windowStart : 0;
    bool originSet = filter;
    for (uint32_t i = first; i <= last; ++i) {
        const size_t timeAt = size_t(head.ofsTimes) + size_t(i) * 4;
        if (timeAt + 4 > len) break;
        const uint32_t stamp = u32(data + timeAt);
        if (filter && (stamp < windowStart || stamp > windowEnd)) continue;
        float key[4] = {0, 0, 0, 1};
        if (!readTrackKey(data, len, head, i, quat, key)) break;
        if (!originSet) {
            origin = stamp;
            originSet = true;
        }
        out.times.push_back(stamp >= origin ? stamp - origin : 0);
        out.values.insert(out.values.end(), key, key + width);
    }
}

void widenSpan(const BoneTrack& track, uint32_t& span) {
    if (track.times.size() < 2 || track.times.back() <= track.times.front()) return;
    const uint32_t delta = track.times.back() - track.times.front();
    if (delta > span && delta < 120000) span = delta;
}

void attachRig(const uint8_t* data, size_t len, LitMesh& out) {
    if (len < 0x14 + 20 * 8) return;
    auto arrayAt = [&](int index, uint32_t& count, uint32_t& offset) {
        const size_t at = 0x14 + size_t(index) * 8;
        count = u32(data + at);
        offset = u32(data + at + 4);
    };
    uint32_t nSeq = 0, seqOfs = 0, nLook = 0, lookOfs = 0, nBone = 0, boneOfs = 0, nVert = 0, vertOfs = 0;
    arrayAt(1, nSeq, seqOfs);
    arrayAt(2, nLook, lookOfs);
    arrayAt(4, nBone, boneOfs);
    arrayAt(6, nVert, vertOfs);
    if (nBone == 0 || nBone > 512 || nVert == 0 || nVert != out.xyz.size() / 3) return;
    if (size_t(boneOfs) + size_t(nBone) * 112 > len) return;
    if (size_t(vertOfs) + size_t(nVert) * 48 > len) return;
    auto slotOf = [&](uint16_t animId) -> uint32_t {
        if (animId >= nLook || size_t(lookOfs) + (size_t(animId) + 1) * 2 > len) return 0xffffffffu;
        const uint16_t slot = u16(data + lookOfs + size_t(animId) * 2);
        if (slot == 0xffff || slot >= nSeq) return 0xffffffffu;
        return slot;
    };
    const uint32_t stand = slotOf(0);
    const uint32_t walk = slotOf(4);
    const uint32_t back = slotOf(13);
    uint32_t run = slotOf(5);
    if (run == 0xffffffffu) run = walk;
    uint32_t nGlob = 0, globOfs = 0;
    arrayAt(0, nGlob, globOfs);
    auto globalLoop = [&](int16_t seq) -> uint32_t {
        if (seq < 0 || uint32_t(seq) >= nGlob) return 0;
        if (size_t(globOfs) + (size_t(seq) + 1) * 4 > len) return 0;
        const uint32_t duration = u32(data + globOfs + size_t(seq) * 4);
        return duration < 120000u ? duration : 0;
    };
    auto clipOf = [&](uint32_t slot, uint32_t& start, uint32_t& end) {
        start = end = 0;
        if (slot == 0xffffffffu || slot >= nSeq) return false;
        if (size_t(seqOfs) + (size_t(slot) + 1) * 68 > len) return false;
        start = u32(data + seqOfs + size_t(slot) * 68 + 4);
        end = u32(data + seqOfs + size_t(slot) * 68 + 8);
        return end > start && end - start < 120000u;
    };
    uint32_t standStart = 0, standEnd = 0, walkStart = 0, walkEnd = 0, runStart = 0, runEnd = 0, backStart = 0, backEnd = 0;
    const bool standClip = clipOf(stand, standStart, standEnd);
    const bool walkClip = clipOf(walk, walkStart, walkEnd);
    const bool runClip = clipOf(run, runStart, runEnd);
    const bool backClip = clipOf(back, backStart, backEnd);
    auto groundSpeed = [&](uint32_t slot) {
        if (slot == 0xffffffffu || slot >= nSeq) return 0.f;
        if (size_t(seqOfs) + (size_t(slot) + 1) * 68 > len) return 0.f;
        const float speed = std::fabs(f32(data + seqOfs + size_t(slot) * 68 + 12));
        return speed > 0.2f && speed < 40.f ? speed : 0.f;
    };
    out.walkGround = groundSpeed(walk);
    out.runGround = groundSpeed(run);
    out.backGround = groundSpeed(back);
    out.bones.resize(nBone);
    uint32_t standSpan = 0, walkSpan = 0, runSpan = 0, backSpan = 0;
    for (uint32_t i = 0; i < nBone; ++i) {
        const size_t at = size_t(boneOfs) + size_t(i) * 112;
        ModelBone& bone = out.bones[i];
        std::memcpy(&bone.parent, data + at + 8, 2);
        bone.pivot[0] = f32(data + at + 100);
        bone.pivot[1] = f32(data + at + 104);
        bone.pivot[2] = f32(data + at + 108);
        const TrackHead trans = readTrack(data, len, at + 16);
        const TrackHead rot = readTrack(data, len, at + 44);
        const TrackHead scale = readTrack(data, len, at + 72);
        readTrackKey(data, len, trans, 0, false, bone.bindT);
        readTrackKey(data, len, rot, 0, true, bone.bindR);
        readTrackKey(data, len, scale, 0, false, bone.bindS);
        if (stand != 0xffffffffu) {
            fillSlice(data, len, trans, stand, false, standClip, standStart, standEnd, globalLoop(trans.globalSeq), bone.standT);
            fillSlice(data, len, rot, stand, true, standClip, standStart, standEnd, globalLoop(rot.globalSeq), bone.standR);
            fillSlice(data, len, scale, stand, false, standClip, standStart, standEnd, globalLoop(scale.globalSeq), bone.standS);
        }
        if (walk != 0xffffffffu) {
            fillSlice(data, len, trans, walk, false, walkClip, walkStart, walkEnd, globalLoop(trans.globalSeq), bone.walkT);
            fillSlice(data, len, rot, walk, true, walkClip, walkStart, walkEnd, globalLoop(rot.globalSeq), bone.walkR);
            fillSlice(data, len, scale, walk, false, walkClip, walkStart, walkEnd, globalLoop(scale.globalSeq), bone.walkS);
        }
        if (run != 0xffffffffu) {
            fillSlice(data, len, trans, run, false, runClip, runStart, runEnd, globalLoop(trans.globalSeq), bone.runT);
            fillSlice(data, len, rot, run, true, runClip, runStart, runEnd, globalLoop(rot.globalSeq), bone.runR);
            fillSlice(data, len, scale, run, false, runClip, runStart, runEnd, globalLoop(scale.globalSeq), bone.runS);
        }
        if (back != 0xffffffffu) {
            fillSlice(data, len, trans, back, false, backClip, backStart, backEnd, globalLoop(trans.globalSeq), bone.backT);
            fillSlice(data, len, rot, back, true, backClip, backStart, backEnd, globalLoop(rot.globalSeq), bone.backR);
            fillSlice(data, len, scale, back, false, backClip, backStart, backEnd, globalLoop(scale.globalSeq), bone.backS);
        }
        widenSpan(bone.standT, standSpan);
        widenSpan(bone.standR, standSpan);
        widenSpan(bone.standS, standSpan);
        widenSpan(bone.walkT, walkSpan);
        widenSpan(bone.walkR, walkSpan);
        widenSpan(bone.walkS, walkSpan);
        widenSpan(bone.runT, runSpan);
        widenSpan(bone.runR, runSpan);
        widenSpan(bone.runS, runSpan);
        widenSpan(bone.backT, backSpan);
        widenSpan(bone.backR, backSpan);
        widenSpan(bone.backS, backSpan);
    }
    out.standMs = standClip ? standEnd - standStart : standSpan;
    out.walkMs = walkClip ? walkEnd - walkStart : walkSpan;
    out.runMs = runClip ? runEnd - runStart : runSpan;
    out.backMs = backClip ? backEnd - backStart : backSpan;
    out.weights.resize(size_t(nVert) * 4);
    out.joints.resize(size_t(nVert) * 4);
    bool overflow = false;
    for (uint32_t i = 0; i < nVert; ++i) {
        const uint8_t* v = data + vertOfs + size_t(i) * 48;
        std::memcpy(out.weights.data() + size_t(i) * 4, v + 12, 4);
        std::memcpy(out.joints.data() + size_t(i) * 4, v + 16, 4);
        for (int k = 0; k < 4; ++k) {
            if (out.weights[size_t(i) * 4 + k] && out.joints[size_t(i) * 4 + k] >= nBone) overflow = true;
        }
    }
    if (overflow) {
        uint32_t nLookup = 0, lookupOfs = 0;
        arrayAt(15, nLookup, lookupOfs);
        if (nLookup && size_t(lookupOfs) + size_t(nLookup) * 2 <= len) {
            for (uint8_t& joint : out.joints) {
                if (joint < nLookup) joint = uint8_t(u16(data + lookupOfs + size_t(joint) * 2));
            }
        }
    }
    out.rest = out.xyz;
}

void splitCharacter(const uint8_t* data, size_t len, const std::vector<uint32_t>& types, LitMesh& out) {
    if (len < 0x14 + 17 * 8) return;
    auto arrayAt = [&](int index, uint32_t& count, uint32_t& offset) {
        count = u32(data + 0x14 + size_t(index) * 8);
        offset = u32(data + 0x14 + size_t(index) * 8 + 4);
    };
    uint32_t nView = 0, viewOfs = 0, nCombo = 0, comboOfs = 0;
    arrayAt(7, nView, viewOfs);
    arrayAt(16, nCombo, comboOfs);
    if (nView == 0 || size_t(viewOfs) + 44 > len) return;
    const uint8_t* skin = data + viewOfs;
    const uint32_t lookupCount = u32(skin);
    const uint32_t lookupOfs = u32(skin + 4);
    const uint32_t indexCount = u32(skin + 8);
    const uint32_t indexOfs = u32(skin + 12);
    const uint32_t subCount = u32(skin + 24);
    const uint32_t subOfs = u32(skin + 28);
    const uint32_t batchCount = u32(skin + 32);
    const uint32_t batchOfs = u32(skin + 36);
    const uint32_t vertexCount = uint32_t(out.xyz.size() / 3);
    if (indexCount < 3 || subCount == 0 || batchCount == 0) return;
    if (size_t(lookupOfs) + size_t(lookupCount) * 2 > len || size_t(indexOfs) + size_t(indexCount) * 2 > len) return;
    std::vector<uint32_t> kept;
    std::vector<PrimBatch> batches;
    auto texOf = [&](uint16_t combo) -> uint32_t {
        if (combo < nCombo && size_t(comboOfs) + (size_t(combo) + 1) * 2 <= len) {
            const uint16_t mapped = u16(data + comboOfs + size_t(combo) * 2);
            if (mapped < types.size()) return mapped;
        }
        return combo < types.size() ? combo : 0;
    };
    for (uint32_t i = 0; i < batchCount; ++i) {
        const size_t at = size_t(batchOfs) + size_t(i) * 24;
        if (at + 24 > len) break;
        const uint16_t submesh = u16(data + at + 4);
        const uint16_t combo = u16(data + at + 16);
        if (submesh >= subCount || size_t(subOfs) + (size_t(submesh) + 1) * 48 > len) continue;
        const uint8_t* sub = data + subOfs + size_t(submesh) * 48;
        const uint16_t geoset = u16(sub);
        const uint16_t istart = u16(sub + 8);
        uint16_t icount = u16(sub + 10);
        if (icount < 3 || uint32_t(istart) + icount > indexCount) continue;
        icount = uint16_t(icount - icount % 3);
        const uint32_t tex = texOf(combo);
        PrimBatch batch;
        batch.geoset = geoset;
        batch.slot = tex < types.size() && types[tex] == 6 ? 1 : tex < types.size() && types[tex] == 2 ? 2 : 0;
        batch.start = uint32_t(kept.size());
        bool good = true;
        for (uint16_t k = 0; k < icount; ++k) {
            const uint16_t local = u16(data + indexOfs + (size_t(istart) + k) * 2);
            if (local >= lookupCount) {
                good = false;
                break;
            }
            const uint16_t idx = u16(data + lookupOfs + size_t(local) * 2);
            if (idx >= vertexCount) {
                good = false;
                break;
            }
            kept.push_back(idx);
        }
        if (!good) {
            kept.resize(batch.start);
            continue;
        }
        batch.count = uint32_t(kept.size()) - batch.start;
        if (batch.count >= 3) batches.push_back(batch);
    }
    if (batches.empty()) return;
    out.indices.swap(kept);
    out.batches.swap(batches);
    out.character = true;
}

// Static models keep one batch per skin texture unit. textureCount layers share those triangles;
// layer 0 is the base image and a later layer is the alpha overlay (snow on a pine).
void splitStaticTextures(const uint8_t* data, size_t len, LitMesh& out) {
    if (len < 0x14 + 17 * 8 || out.textures.empty()) return;
    auto arrayAt = [&](int index, uint32_t& count, uint32_t& offset) {
        count = u32(data + 0x14 + size_t(index) * 8);
        offset = u32(data + 0x14 + size_t(index) * 8 + 4);
    };
    uint32_t nView = 0, viewOfs = 0, nCombo = 0, comboOfs = 0;
    arrayAt(7, nView, viewOfs);
    arrayAt(16, nCombo, comboOfs);
    if (nView == 0 || size_t(viewOfs) + 44 > len) return;
    const uint8_t* skin = data + viewOfs;
    const uint32_t lookupCount = u32(skin);
    const uint32_t lookupOfs = u32(skin + 4);
    const uint32_t indexCount = u32(skin + 8);
    const uint32_t indexOfs = u32(skin + 12);
    const uint32_t subCount = u32(skin + 24);
    const uint32_t subOfs = u32(skin + 28);
    const uint32_t batchCount = u32(skin + 32);
    const uint32_t batchOfs = u32(skin + 36);
    const uint32_t vertexCount = uint32_t(out.xyz.size() / 3);
    if (indexCount < 3 || subCount == 0 || batchCount == 0) return;
    if (size_t(lookupOfs) + size_t(lookupCount) * 2 > len || size_t(indexOfs) + size_t(indexCount) * 2 > len) return;
    auto texOf = [&](uint16_t combo) -> int {
        uint32_t mapped = combo;
        if (combo < nCombo && size_t(comboOfs) + (size_t(combo) + 1) * 2 <= len)
            mapped = u16(data + comboOfs + size_t(combo) * 2);
        if (mapped >= out.textures.size() || out.textures[mapped].empty()) return -1;
        return int(mapped);
    };
    std::vector<uint32_t> kept;
    std::vector<PrimBatch> batches;
    for (uint32_t i = 0; i < batchCount; ++i) {
        const size_t at = size_t(batchOfs) + size_t(i) * 24;
        if (at + 24 > len) break;
        const uint16_t submesh = u16(data + at + 4);
        uint16_t layers = u16(data + at + 14);
        const uint16_t combo = u16(data + at + 16);
        if (layers == 0) layers = 1;
        if (layers > 4) layers = 4;
        if (submesh >= subCount || size_t(subOfs) + (size_t(submesh) + 1) * 48 > len) continue;
        const uint8_t* sub = data + subOfs + size_t(submesh) * 48;
        const uint16_t istart = u16(sub + 8);
        uint16_t icount = u16(sub + 10);
        if (icount < 3 || uint32_t(istart) + icount > indexCount) continue;
        icount = uint16_t(icount - icount % 3);
        const uint32_t start = uint32_t(kept.size());
        bool good = true;
        for (uint16_t k = 0; k < icount; ++k) {
            const uint16_t local = u16(data + indexOfs + (size_t(istart) + k) * 2);
            if (local >= lookupCount) {
                good = false;
                break;
            }
            const uint16_t idx = u16(data + lookupOfs + size_t(local) * 2);
            if (idx >= vertexCount) {
                good = false;
                break;
            }
            kept.push_back(idx);
        }
        if (!good) {
            kept.resize(start);
            continue;
        }
        const uint32_t count = uint32_t(kept.size()) - start;
        if (count < 3) continue;
        for (uint16_t layer = 0; layer < layers; ++layer) {
            const int tex = texOf(uint16_t(combo + layer));
            if (tex < 0 && layer > 0) continue;
            bool repeat = false;
            for (uint16_t earlier = 0; earlier < layer; ++earlier)
                if (texOf(uint16_t(combo + earlier)) == tex) repeat = true;
            if (repeat) continue;
            PrimBatch batch;
            batch.start = start;
            batch.count = count;
            batch.texture = tex;
            batches.push_back(batch);
        }
    }
    if (batches.empty()) return;
    out.indices.swap(kept);
    out.batches.swap(batches);
}

}  // namespace

bool loadM2Mesh(const uint8_t* data, size_t len, LitMesh& out, std::string& texture) {
    Mesh mesh;
    if (!loadM2(data, len, mesh) || mesh.indices.size() < 3) return false;
    out = LitMesh{};
    out.xyz = std::move(mesh.xyz);
    out.uv = std::move(mesh.uv);
    out.indices = std::move(mesh.indices);
    out.rgb.assign(out.xyz.size(), 255);
    PrimBatch batch;
    batch.start = 0;
    batch.count = uint32_t(out.indices.size());
    batch.texture = -1;
    out.batches.push_back(batch);
    texture.clear();
    if (len < 0x14 + 17 * 8) return true;
    const uint32_t texCount = u32(data + 0x14 + 9 * 8);
    const uint32_t texOfs = u32(data + 0x14 + 9 * 8 + 4);
    std::vector<uint32_t> types;
    bool character = false;
    if (texCount > 0 && texCount <= 32) out.textures.assign(texCount, {});
    for (uint32_t i = 0; i < texCount && i < 32; ++i) {
        const size_t at = size_t(texOfs) + size_t(i) * 16;
        if (at + 16 > len) break;
        const uint32_t kind = u32(data + at);
        types.push_back(kind);
        if (kind == 1 || kind == 6) character = true;
        if (kind != 0) continue;
        const uint32_t nameLen = u32(data + at + 8);
        const uint32_t nameOfs = u32(data + at + 12);
        if (nameLen < 2 || size_t(nameOfs) + nameLen > len) continue;
        std::string name(reinterpret_cast<const char*>(data + nameOfs), ::strnlen(reinterpret_cast<const char*>(data + nameOfs), nameLen));
        for (char& c : name)
            if (c == '/') c = '\\';
        if (i < out.textures.size()) out.textures[i] = name;
        if (texture.empty()) texture = std::move(name);
    }
    if (!character && texture.empty()) {
        for (size_t i = 0; i + 4 < len; ++i) {
            const bool blp = std::memcmp(data + i, ".blp", 4) == 0 || std::memcmp(data + i, ".BLP", 4) == 0;
            if (!blp) continue;
            size_t start = i;
            while (start > 0 && data[start - 1] >= 32 && data[start - 1] < 127) --start;
            texture.assign(reinterpret_cast<const char*>(data + start), i + 4 - start);
            for (char& c : texture)
                if (c == '/') c = '\\';
            break;
        }
    }
    attachRig(data, len, out);
    if (character) splitCharacter(data, len, types, out);
    else splitStaticTextures(data, len, out);
    if (len >= 0xEC + 40) {
        const size_t base = 0xEC;
        const uint32_t nAtt = u32(data + base + 24);
        const uint32_t ofsAtt = u32(data + base + 28);
        const uint32_t nLook = u32(data + base + 32);
        const uint32_t ofsLook = u32(data + base + 36);
        if (nAtt > 0 && nAtt <= 512 && size_t(ofsAtt) + size_t(nAtt) * 48 <= len) {
            out.attachments.resize(nAtt);
            for (uint32_t i = 0; i < nAtt; ++i) {
                const size_t off = size_t(ofsAtt) + size_t(i) * 48;
                ModelAttachment& att = out.attachments[i];
                att.id = u32(data + off);
                att.bone = u16(data + off + 4);
                att.x = f32(data + off + 8);
                att.y = f32(data + off + 12);
                att.z = f32(data + off + 16);
            }
            if (nLook > 0 && nLook <= 64 && size_t(ofsLook) + size_t(nLook) * 2 <= len) {
                out.attachmentOf.assign(nLook, -1);
                for (uint32_t i = 0; i < nLook; ++i) {
                    int16_t index = -1;
                    std::memcpy(&index, data + ofsLook + size_t(i) * 2, 2);
                    out.attachmentOf[i] = index;
                }
            }
        }
    }
    return true;
}

namespace {

void matMul(const float* a, const float* b, float* out) {
    float m[16];
    for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r)
            m[c * 4 + r] = a[r] * b[c * 4] + a[4 + r] * b[c * 4 + 1] + a[8 + r] * b[c * 4 + 2] + a[12 + r] * b[c * 4 + 3];
    }
    std::memcpy(out, m, sizeof(m));
}

void matTranslate(float x, float y, float z, float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

void matScale(float x, float y, float z, float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = x;
    m[5] = y;
    m[10] = z;
    m[15] = 1;
}

void matQuat(float x, float y, float z, float w, float* m) {
    const float n = x * x + y * y + z * z + w * w;
    std::memset(m, 0, 16 * sizeof(float));
    m[15] = 1;
    if (n < 1e-12f) {
        m[0] = m[5] = m[10] = 1;
        return;
    }
    const float s = 2.f / n;
    const float xx = x * x * s, yy = y * y * s, zz = z * z * s;
    const float xy = x * y * s, xz = x * z * s, yz = y * z * s;
    const float wx = w * x * s, wy = w * y * s, wz = w * z * s;
    m[0] = 1.f - (yy + zz);
    m[1] = xy + wz;
    m[2] = xz - wy;
    m[4] = xy - wz;
    m[5] = 1.f - (xx + zz);
    m[6] = yz + wx;
    m[8] = xz + wy;
    m[9] = yz - wx;
    m[10] = 1.f - (xx + yy);
}

void boneMatrix(const float pivot[3], const float t[3], const float q[4], const float s[3], float* out) {
    float tp[16], tn[16], tr[16], sc[16], rot[16], a[16], b[16];
    matTranslate(pivot[0], pivot[1], pivot[2], tp);
    matTranslate(-pivot[0], -pivot[1], -pivot[2], tn);
    matTranslate(t[0], t[1], t[2], tr);
    matScale(s[0], s[1], s[2], sc);
    matQuat(q[0], q[1], q[2], q[3], rot);
    matMul(tr, tn, a);
    matMul(sc, a, b);
    matMul(rot, b, a);
    matMul(tp, a, out);
}

void sampleTrack(const BoneTrack& track, uint32_t tick, uint32_t span, int width, const float* fallback, float* dest) {
    if (track.times.empty() || track.values.size() < size_t(width)) {
        std::memcpy(dest, fallback, size_t(width) * 4);
        return;
    }
    const size_t last = track.times.size() - 1;
    if (last == 0 || span == 0) {
        std::memcpy(dest, track.values.data(), size_t(width) * 4);
        return;
    }
    const uint32_t clock = track.loopMs ? track.loopMs : span;
    const uint32_t t = clock ? tick % clock : 0;
    const float* values = track.values.data();
    auto copyAt = [&](size_t index) { std::memcpy(dest, values + index * width, size_t(width) * 4); };
    if (t <= track.times.front()) {
        copyAt(0);
        return;
    }
    if (t >= track.times[last]) {
        copyAt(last);
        return;
    }
    size_t i = 0;
    while (i + 1 < track.times.size() && track.times[i + 1] <= t) ++i;
    const uint32_t t0 = track.times[i];
    const uint32_t t1 = track.times[i + 1];
    const float u = t1 == t0 ? 0.f : float(t - t0) / float(t1 - t0);
    const float* a = values + i * width;
    const float* b = a + width;
    if (width == 4) {
        float flip = 1.f;
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.f) flip = -1.f;
        for (int k = 0; k < 4; ++k) dest[k] = a[k] * (1.f - u) + b[k] * flip * u;
        const float n = dest[0] * dest[0] + dest[1] * dest[1] + dest[2] * dest[2] + dest[3] * dest[3];
        if (n > 1e-8f) {
            const float inv = 1.f / std::sqrt(n);
            for (int k = 0; k < 4; ++k) dest[k] *= inv;
        }
    } else {
        for (int k = 0; k < width; ++k) dest[k] = a[k] * (1.f - u) + b[k] * u;
    }
}

void writeIdentity(float* m) {
    std::memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1;
}

void buildChain(const std::vector<ModelBone>& bones, uint8_t gait, uint32_t tick, uint32_t span, float* mats) {
    const int n = int(bones.size());
    static thread_local std::vector<uint8_t> state;
    static thread_local std::vector<int> stack;
    state.assign(size_t(n), 0);
    stack.clear();
    const float restT[3] = {0.f, 0.f, 0.f};
    const float restR[4] = {0.f, 0.f, 0.f, 1.f};
    const float restS[3] = {1.f, 1.f, 1.f};
    for (int start = 0; start < n; ++start) {
        if (state[size_t(start)] == 2) continue;
        stack.push_back(start);
        while (!stack.empty()) {
            const int index = stack.back();
            if (state[size_t(index)] == 2) {
                stack.pop_back();
                continue;
            }
            const ModelBone& bone = bones[size_t(index)];
            const int parent = bone.parent;
            const bool hasParent = parent >= 0 && parent < n && parent != index;
            if (hasParent && state[size_t(parent)] != 2) {
                if (state[size_t(parent)] == 1) {
                    writeIdentity(mats + size_t(index) * 16);
                    state[size_t(index)] = 2;
                    stack.pop_back();
                    continue;
                }
                state[size_t(index)] = 1;
                stack.push_back(parent);
                continue;
            }
            float t[3], q[4], s[3];
            // A bone with no keys in this clip stays at the pivot rest. Falling back to the
            // other clip plays the wrong gesture (stand fingers during a run).
            const BoneTrack& trans = gait == 3 ? bone.backT : gait == 2 ? bone.runT : gait == 1 ? bone.walkT : bone.standT;
            const BoneTrack& rot = gait == 3 ? bone.backR : gait == 2 ? bone.runR : gait == 1 ? bone.walkR : bone.standR;
            const BoneTrack& scale = gait == 3 ? bone.backS : gait == 2 ? bone.runS : gait == 1 ? bone.walkS : bone.standS;
            sampleTrack(trans, tick, span, 3, restT, t);
            sampleTrack(rot, tick, span, 4, restR, q);
            sampleTrack(scale, tick, span, 3, restS, s);
            const bool still = t[0] == 0.f && t[1] == 0.f && t[2] == 0.f && q[0] == 0.f && q[1] == 0.f && q[2] == 0.f &&
                               q[3] == 1.f && s[0] == 1.f && s[1] == 1.f && s[2] == 1.f;
            float* dest = mats + size_t(index) * 16;
            if (still) {
                if (hasParent) std::memcpy(dest, mats + size_t(parent) * 16, 16 * sizeof(float));
                else writeIdentity(dest);
            } else {
                float local[16];
                boneMatrix(bone.pivot, t, q, s, local);
                if (hasParent) matMul(mats + size_t(parent) * 16, local, dest);
                else std::memcpy(dest, local, sizeof(local));
            }
            state[size_t(index)] = 2;
            stack.pop_back();
        }
    }
}

}  // namespace

uint8_t movementGait(bool moving, float along, uint32_t moveFlags, uint32_t splineFlags, bool onSpline, float speed,
                     float walkSpeed, float runSpeed) {
    if (!moving) return 0;
    // Moving against the facing plays the backward clip. The forward run would moonwalk.
    if (along < -0.35f && -along >= speed * 0.45f) return 3;
    constexpr uint32_t kWalkMode = 0x00000100;
    constexpr uint32_t kSplineRun = 0x00000100;
    const float walk = walkSpeed > 0.2f ? walkSpeed : 2.5f;
    const float run = runSpeed > 0.2f ? runSpeed : 7.f;
    if ((moveFlags & kWalkMode) != 0) return 1;
    if (speed > 0.2f && speed <= (walk + run) * 0.5f) return 1;
    if (onSpline && (splineFlags & kSplineRun) == 0) return 1;
    return 2;
}

void poseModel(LitMesh& mesh, uint32_t timeMs, uint8_t gait) {
    const size_t verts = mesh.rest.size() / 3;
    if (mesh.bones.empty() || verts == 0 || mesh.weights.size() < verts * 4 || mesh.joints.size() < verts * 4) return;
    if (gait == 3 && mesh.backMs == 0) gait = mesh.walkMs > 0 ? 1 : 0;
    if (gait == 1 && mesh.walkMs == 0) gait = mesh.runMs > 0 ? 2 : 0;
    if (gait == 2 && mesh.runMs == 0) gait = mesh.walkMs > 0 ? 1 : 0;
    // The pivot rest is the identity, so the skinning matrix is the animated bone itself.
    // Copies of one model share those vertices: the second stander in a frame is already posed.
    if (mesh.posedMoving == gait && mesh.posedAt == timeMs && mesh.boneMats.size() == mesh.bones.size() * 16) return;
    const uint32_t span = gait == 3 ? mesh.backMs : gait == 2 ? mesh.runMs : gait == 1 ? mesh.walkMs : mesh.standMs;
    const int n = int(mesh.bones.size());
    mesh.boneMats.resize(size_t(n) * 16);
    buildChain(mesh.bones, gait, timeMs, span, mesh.boneMats.data());
    if (mesh.xyz.size() < mesh.rest.size()) mesh.xyz.resize(mesh.rest.size());
    const float* mats = mesh.boneMats.data();
    const float* rest = mesh.rest.data();
    float* out = mesh.xyz.data();
    const uint8_t* weights = mesh.weights.data();
    const uint8_t* joints = mesh.joints.data();
    const size_t boneCount = mesh.bones.size();
    for (size_t v = 0; v < verts; ++v) {
        const float x = rest[v * 3];
        const float y = rest[v * 3 + 1];
        const float z = rest[v * 3 + 2];
        const uint8_t* weight = weights + v * 4;
        const uint8_t* joint = joints + v * 4;
        float ox, oy, oz;
        if (weight[0] == 255 && joint[0] < boneCount) {
            const float* m = mats + size_t(joint[0]) * 16;
            ox = m[0] * x + m[4] * y + m[8] * z + m[12];
            oy = m[1] * x + m[5] * y + m[9] * z + m[13];
            oz = m[2] * x + m[6] * y + m[10] * z + m[14];
        } else {
            ox = oy = oz = 0;
            float wsum = 0;
            for (int k = 0; k < 4; ++k) {
                if (weight[k] == 0 || joint[k] >= boneCount) continue;
                const float* m = mats + size_t(joint[k]) * 16;
                const float fw = float(weight[k]) * (1.f / 255.f);
                ox += (m[0] * x + m[4] * y + m[8] * z + m[12]) * fw;
                oy += (m[1] * x + m[5] * y + m[9] * z + m[13]) * fw;
                oz += (m[2] * x + m[6] * y + m[10] * z + m[14]) * fw;
                wsum += fw;
            }
            if (wsum < 0.001f) {
                ox = x;
                oy = y;
                oz = z;
            } else if (wsum < 0.99f || wsum > 1.01f) {
                ox /= wsum;
                oy /= wsum;
                oz /= wsum;
            }
        }
        float* dest = out + v * 3;
        dest[0] = ox;
        dest[1] = oy;
        dest[2] = oz;
    }
    mesh.posedAt = timeMs;
    mesh.posedMoving = gait;
}

bool characterLook(const Dbc& sections, const Dbc& hairGeosets, const Dbc& facial, uint32_t race, uint32_t gender, uint32_t skin,
                   uint32_t face, uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair, CharacterLook& out) {
    out = CharacterLook{};
    out.gender = gender == 1 ? 1u : 0u;
    const uint16_t bases[] = {1, 101, 201, 301, 401, 501, 601, 702, 801, 901, 1001, 1101, 1201, 1301, 1401, 1501};
    uint16_t chosen[16];
    for (int i = 0; i < 16; ++i) chosen[i] = bases[i];
    uint32_t hairId = 1;
    for (uint32_t row = 0; row < hairGeosets.records; ++row) {
        if (hairGeosets.get(row, 1) == race && hairGeosets.get(row, 2) == gender && hairGeosets.get(row, 3) == hairStyle) {
            hairId = hairGeosets.get(row, 4);
            break;
        }
    }
    chosen[0] = hairId > 9999 ? 1 : uint16_t(hairId);
    for (uint32_t row = 0; row < facial.records; ++row) {
        if (facial.get(row, 0) != race || facial.get(row, 1) != gender || facial.get(row, 2) != facialHair) continue;
        auto part = [&](uint32_t field, int group, uint16_t base) {
            uint32_t value = facial.get(row, field);
            if (value > 99) value = 0;
            chosen[group] = uint16_t(base + value);
        };
        part(6, 1, 100);
        part(8, 2, 200);
        part(7, 3, 300);
        break;
    }
    out.geosets.push_back(0);
    out.geosets.insert(out.geosets.end(), chosen, chosen + 16);
    auto fill3 = [&](uint32_t row, std::string dest[3]) {
        for (int i = 0; i < 3; ++i) {
            dest[i] = sections.text(sections.get(row, 6 + i));
            for (char& c : dest[i])
                if (c == '/') c = '\\';
        }
    };
    auto empty3 = [](const std::string values[3]) { return values[0].empty() && values[1].empty() && values[2].empty(); };
    for (uint32_t row = 0; row < sections.records; ++row) {
        if (sections.get(row, 1) != race || sections.get(row, 2) != gender || (sections.get(row, 9) & 1)) continue;
        const uint32_t kind = sections.get(row, 3);
        const uint32_t variant = sections.get(row, 4);
        const uint32_t color = sections.get(row, 5);
        if (kind == 0 && variant == 0 && color == skin && out.body.empty()) {
            std::string paths[3];
            fill3(row, paths);
            out.body = paths[0];
        } else if (kind == 1 && variant == face && color == skin && empty3(out.face)) {
            fill3(row, out.face);
        } else if (kind == 2 && variant == facialHair && color == hairColor && empty3(out.facial)) {
            fill3(row, out.facial);
        } else if (kind == 3 && variant == hairStyle && color == hairColor && empty3(out.hairLayer)) {
            fill3(row, out.hairLayer);
        }
    }
    out.hair = out.hairLayer[0];
    auto lay = [&](const std::string& path, int section, int order) {
        if (path.empty()) return;
        SkinPaint paint;
        paint.section = section;
        paint.priority = -1;
        paint.order = order;
        paint.path = path;
        out.paints.push_back(std::move(paint));
    };
    // Head lower receives the lower face, facial hair, and lower scalp. Head upper receives the matching upper layers.
    lay(out.face[0], 9, 0);
    lay(out.facial[0], 9, 1);
    lay(out.hairLayer[1], 9, 2);
    lay(out.face[1], 8, 3);
    lay(out.facial[1], 8, 4);
    lay(out.hairLayer[2], 8, 5);
    out.ok = !out.body.empty();
    return out.ok;
}

namespace {

std::string componentPath(int section, std::string name) {
    if (name.empty() || section < 0 || section > 7) return {};
    for (char& c : name)
        if (c == '/') c = '\\';
    if (name.find('\\') == std::string::npos) {
        static const char* kDirs[8] = {
            "Item\\TextureComponents\\ArmUpperTexture\\", "Item\\TextureComponents\\ArmLowerTexture\\",
            "Item\\TextureComponents\\HandTexture\\",     "Item\\TextureComponents\\TorsoUpperTexture\\",
            "Item\\TextureComponents\\TorsoLowerTexture\\", "Item\\TextureComponents\\LegUpperTexture\\",
            "Item\\TextureComponents\\LegLowerTexture\\", "Item\\TextureComponents\\FootTexture\\",
        };
        name = std::string(kDirs[section]) + name;
    }
    const auto ends = [&](const char* ext) {
        const size_t n = std::strlen(ext);
        return name.size() >= n && name.compare(name.size() - n, n, ext) == 0;
    };
    if (!ends(".blp") && !ends(".BLP")) name += ".blp";
    return name;
}

int slotOrder(int slot) {
    static const int kSlots[] = {3, 4, 18, 6, 5, 7, 8, 9};
    for (int i = 0; i < 8; ++i)
        if (kSlots[i] == slot) return i;
    return 8;
}

bool sectionRank(int slot, int section, int& priority) {
    struct Rank {
        int slot;
        int section;
        int priority;
    };
    static const Rank kRanks[] = {
        {3, 0, 0}, {3, 1, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 1}, {3, 6, 1}, {4, 0, 1}, {4, 1, 1}, {4, 3, 1},
        {4, 4, 1}, {4, 5, 1}, {4, 6, 1}, {5, 4, 5}, {5, 5, 2}, {6, 5, 0}, {6, 6, 0}, {7, 6, 2}, {7, 7, 0},
        {8, 1, 2}, {8, 2, 0}, {9, 1, 3}, {9, 2, 0}, {18, 3, 4}, {18, 4, 4},
    };
    for (const Rank& rank : kRanks) {
        if (rank.slot == slot && rank.section == section) {
            priority = rank.priority;
            return true;
        }
    }
    return false;
}

}  // namespace

void dressCharacter(const EquipVisual* worn, int count, CharacterLook& look) {
    if (!worn || count <= 0) return;
    bool present[19] = {};
    uint32_t geo[19][3] = {};
    look.paints.erase(std::remove_if(look.paints.begin(), look.paints.end(),
                                     [](const SkinPaint& paint) { return paint.priority >= 0; }),
                      look.paints.end());
    look.cape.clear();
    for (int i = 0; i < count; ++i) {
        const EquipVisual& piece = worn[i];
        if (piece.slot < 0 || piece.slot > 18) continue;
        present[piece.slot] = true;
        geo[piece.slot][0] = piece.geoset[0];
        geo[piece.slot][1] = piece.geoset[1];
        geo[piece.slot][2] = piece.geoset[2];
        if (piece.slot == 14 && !piece.cape.empty()) look.cape = piece.cape;
        const int order = slotOrder(piece.slot);
        for (int section = 0; section < 8; ++section) {
            int priority = 0;
            if (!sectionRank(piece.slot, section, priority)) continue;
            std::string path = componentPath(section, piece.component[section]);
            if (path.empty()) continue;
            SkinPaint paint;
            paint.section = section;
            paint.priority = priority;
            paint.order = order;
            paint.path = std::move(path);
            look.paints.push_back(std::move(paint));
        }
    }
    std::sort(look.paints.begin(), look.paints.end(), [](const SkinPaint& a, const SkinPaint& b) {
        if (a.priority != b.priority) return a.priority < b.priority;
        if (a.order != b.order) return a.order < b.order;
        return a.section < b.section;
    });
    if (look.geosets.empty()) return;
    std::vector<uint16_t> enabled = look.geosets;
    auto group = [&](int slot, int sub) -> int {
        if (slot < 0 || slot > 18 || !present[slot]) return -1;
        const uint32_t value = geo[slot][sub];
        if (value == 0 || value > 99) return -1;
        return int(value);
    };
    auto disable = [&](int lo, int hi) {
        enabled.erase(std::remove_if(enabled.begin(), enabled.end(),
                                     [&](uint16_t id) { return int(id) >= lo && int(id) <= hi; }),
                      enabled.end());
    };
    const int robe = group(4, 2) >= 0 ? group(4, 2) : group(6, 2);
    if (const int hands = group(9, 0); hands >= 0) {
        disable(401, 499);
        enabled.push_back(uint16_t(401 + hands));
    } else if (const int chest = group(4, 0); chest >= 0) {
        enabled.push_back(uint16_t(801 + chest));
    }
    if (!present[4]) {
        if (const int shirt = group(3, 0); shirt >= 0) enabled.push_back(uint16_t(801 + shirt));
    }
    if (robe >= 0) {
        disable(501, 599);
        disable(902, 999);
        disable(1100, 1199);
        disable(1300, 1399);
        enabled.push_back(uint16_t(1301 + robe));
    } else if (const int feet = group(7, 0); feet >= 0) {
        disable(501, 599);
        enabled.push_back(uint16_t(501 + feet));
    } else if (const int legs = group(6, 1); legs >= 0) {
        enabled.push_back(uint16_t(901 + legs));
    }
    if (robe < 0) {
        if (const int tabard = group(18, 0); tabard >= 0) enabled.push_back(uint16_t(1201 + tabard));
    }
    if (const int shirt = group(3, 1); shirt >= 0) enabled.push_back(uint16_t(1001 + shirt));
    if (robe < 0) {
        if (const int legs = group(6, 0); legs >= 0) enabled.push_back(uint16_t(1102 + legs));
    }
    if (const int back = group(14, 0); back >= 0) {
        disable(1500, 1599);
        enabled.push_back(uint16_t(1501 + back));
    }
    if (const int waist = group(5, 0); waist >= 0) {
        disable(1800, 1899);
        enabled.push_back(uint16_t(1801 + waist));
    }
    if (std::find(enabled.begin(), enabled.end(), 0) == enabled.end()) enabled.insert(enabled.begin(), uint16_t{0});
    look.geosets.swap(enabled);
}

void paintSkin(Image& canvas, const Image& patch, int section) {
    static const int kSection[10][4] = {
        {0, 0, 256, 128},   {0, 128, 256, 128}, {0, 256, 256, 64},  {256, 0, 256, 128}, {256, 128, 256, 64},
        {256, 192, 256, 128}, {256, 320, 256, 128}, {256, 448, 256, 64}, {0, 320, 256, 64}, {0, 384, 256, 128},
    };
    if (section < 0 || section > 9 || canvas.width <= 0 || canvas.height <= 0 || patch.width <= 0 || patch.height <= 0)
        return;
    if (canvas.rgba.size() < size_t(canvas.width) * size_t(canvas.height) * 4) return;
    if (patch.rgba.size() < size_t(patch.width) * size_t(patch.height) * 4) return;
    const float scaleX = float(canvas.width) / 512.f;
    const float scaleY = float(canvas.height) / 512.f;
    const int x0 = int(std::lround(kSection[section][0] * scaleX));
    const int y0 = int(std::lround(kSection[section][1] * scaleY));
    const int spanW = std::max(1, int(std::lround(kSection[section][2] * scaleX)));
    const int spanH = std::max(1, int(std::lround(kSection[section][3] * scaleY)));
    auto sample = [&](float u, float v, float& r, float& g, float& b, float& a) {
        const float x = std::clamp(u, 0.f, 1.f) * float(std::max(0, patch.width - 1));
        const float y = std::clamp(v, 0.f, 1.f) * float(std::max(0, patch.height - 1));
        const int x1 = std::min(patch.width - 1, int(x));
        const int y1 = std::min(patch.height - 1, int(y));
        const int x2 = std::min(patch.width - 1, x1 + 1);
        const int y2 = std::min(patch.height - 1, y1 + 1);
        const float tx = x - float(x1);
        const float ty = y - float(y1);
        auto at = [&](int px, int py, int channel) {
            return float(patch.rgba[(size_t(py) * size_t(patch.width) + px) * 4 + channel]);
        };
        auto mix = [&](int channel) {
            const float top = at(x1, y1, channel) * (1.f - tx) + at(x2, y1, channel) * tx;
            const float bottom = at(x1, y2, channel) * (1.f - tx) + at(x2, y2, channel) * tx;
            return top * (1.f - ty) + bottom * ty;
        };
        r = mix(0);
        g = mix(1);
        b = mix(2);
        a = mix(3);
    };
    for (int y = 0; y < spanH; ++y) {
        const int dy = y0 + y;
        if (dy < 0 || dy >= canvas.height) continue;
        const float v = spanH == 1 ? 0.f : float(y) / float(spanH - 1);
        for (int x = 0; x < spanW; ++x) {
            const int dx = x0 + x;
            if (dx < 0 || dx >= canvas.width) continue;
            const float u = spanW == 1 ? 0.f : float(x) / float(spanW - 1);
            float sr, sg, sb, sa;
            sample(u, v, sr, sg, sb, sa);
            if (sa <= 0.5f) continue;
            uint8_t* dest = canvas.rgba.data() + (size_t(dy) * size_t(canvas.width) + dx) * 4;
            const float saN = sa / 255.f;
            const float daN = float(dest[3]) / 255.f;
            const float outN = saN + daN * (1.f - saN);
            if (outN < 1e-4f) continue;
            const float keep = daN * (1.f - saN);
            dest[0] = uint8_t(std::clamp((sr * saN + float(dest[0]) * keep) / outN, 0.f, 255.f));
            dest[1] = uint8_t(std::clamp((sg * saN + float(dest[1]) * keep) / outN, 0.f, 255.f));
            dest[2] = uint8_t(std::clamp((sb * saN + float(dest[2]) * keep) / outN, 0.f, 255.f));
            dest[3] = uint8_t(std::clamp(outN * 255.f, 0.f, 255.f));
        }
    }
}

bool seasonalHidden(const std::string& name) {
    if (name.empty()) return false;
    const std::chrono::year_month_day ymd{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    const int today = int(static_cast<unsigned>(ymd.month())) * 100 + int(static_cast<unsigned>(ymd.day()));
    auto has = [&](const char* token) {
        const size_t n = std::strlen(token);
        if (n == 0 || name.size() < n) return false;
        for (size_t i = 0; i + n <= name.size(); ++i) {
            bool match = true;
            for (size_t k = 0; k < n; ++k) {
                unsigned char c = static_cast<unsigned char>(name[i + k]);
                if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
                if (c != static_cast<unsigned char>(token[k])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        return false;
    };
    struct Rule {
        const char* token;
        int start;
        int end;
        bool wraps;
    };
    // Month-day windows repeat every year. Hallow's End covers late October.
    static const Rule rules[] = {
        {"pumpkin", 1018, 1101, false}, {"hallow", 1018, 1101, false},
        {"wickerman", 1018, 1101, false}, {"brewfest", 920, 1006, false},
        {"winterveil", 1215, 102, true},  {"mistletoe", 1215, 102, true},
        {"lunarfestival", 124, 207, false}, {"valentine", 206, 220, false},
        {"noblegarden", 415, 422, false}, {"midsummer", 621, 705, false},
        {"ribbonpole", 621, 705, false},
    };
    for (const Rule& rule : rules) {
        if (!has(rule.token)) continue;
        const bool open = rule.wraps ? (today >= rule.start || today <= rule.end) : (today >= rule.start && today <= rule.end);
        if (!open) return true;
    }
    return false;
}

namespace {

uint32_t blendChannel(uint32_t a, uint32_t b, float t) {
    return uint32_t(std::clamp(float(a) + (float(int(b) - int(a))) * t, 0.f, 255.f));
}

void readBand(const Dbc& dbc, uint32_t id, LightTable::Band& out) {
    out.count = 0;
    for (uint32_t row = 0; row < dbc.records; ++row) {
        if (dbc.get(row, 0) != id) continue;
        out.count = std::min(16u, dbc.get(row, 1));
        for (uint32_t i = 0; i < out.count; ++i) {
            out.time[i] = dbc.get(row, 2 + i);
            out.data[i] = dbc.get(row, 18 + i);
        }
        return;
    }
}

bool sampleBand(const LightTable::Band& band, int half, float& scalar, uint32_t& packed, bool asFloat) {
    if (band.count == 0) return false;
    int clock = half % 2880;
    if (clock < 0) clock += 2880;
    uint32_t prevT = band.time[band.count - 1];
    uint32_t prevD = band.data[band.count - 1];
    uint32_t nextT = band.time[0] + 2880;
    uint32_t nextD = band.data[0];
    for (uint32_t i = 0; i < band.count; ++i) {
        if (int(band.time[i]) <= clock) {
            prevT = band.time[i];
            prevD = band.data[i];
        }
        if (int(band.time[i]) > clock) {
            nextT = band.time[i];
            nextD = band.data[i];
            break;
        }
    }
    if (nextT < prevT) nextT += 2880;
    int clockAt = clock;
    if (clockAt < int(prevT)) clockAt += 2880;
    float span = float(int(nextT) - int(prevT));
    if (span < 1.f) span = 1.f;
    const float t = std::clamp((float(clockAt) - float(int(prevT))) / span, 0.f, 1.f);
    if (asFloat) {
        float a, b;
        std::memcpy(&a, &prevD, 4);
        std::memcpy(&b, &nextD, 4);
        scalar = a + (b - a) * t;
    } else {
        const uint32_t ar = (prevD >> 16) & 255, ag = (prevD >> 8) & 255, ab = prevD & 255;
        const uint32_t br = (nextD >> 16) & 255, bg = (nextD >> 8) & 255, bb = nextD & 255;
        packed = (blendChannel(ar, br, t) << 16) | (blendChannel(ag, bg, t) << 8) | blendChannel(ab, bb, t);
    }
    return true;
}

}  // namespace

bool LightTable::load(ArchiveSet& archives) {
    std::vector<uint8_t> bytes;
    Dbc light, colors, floats;
    if (!archives.read("DBFilesClient\\Light.dbc", bytes) || !light.load(bytes.data(), bytes.size())) return false;
    if (!archives.read("DBFilesClient\\LightIntBand.dbc", bytes) || !colors.load(bytes.data(), bytes.size())) return false;
    if (!archives.read("DBFilesClient\\LightFloatBand.dbc", bytes) || !floats.load(bytes.data(), bytes.size())) return false;
    places_.clear();
    for (uint32_t row = 0; row < light.records; ++row) {
        Place place;
        place.map = light.get(row, 1);
        uint32_t bits = light.get(row, 2);
        std::memcpy(&place.x, &bits, 4);
        bits = light.get(row, 3);
        std::memcpy(&place.y, &bits, 4);
        bits = light.get(row, 4);
        std::memcpy(&place.z, &bits, 4);
        bits = light.get(row, 5);
        std::memcpy(&place.falloffStart, &bits, 4);
        bits = light.get(row, 6);
        std::memcpy(&place.falloffEnd, &bits, 4);
        place.param = light.get(row, 7);
        places_.push_back(place);
    }
    auto take = [](const Dbc& dbc, uint32_t id, Band& band) { readBand(dbc, id, band); };
    colors_.clear();
    floats_.clear();
    for (const Place& place : places_) {
        for (int row = 0; row < 8; ++row) {
            const uint32_t id = place.param * 18 - 17 + uint32_t(row);
            if (!colors_.count(id)) take(colors, id, colors_[id]);
        }
        for (int row = 0; row < 2; ++row) {
            const uint32_t id = place.param * 6 - 5 + uint32_t(row);
            if (!floats_.count(id)) take(floats, id, floats_[id]);
        }
    }
    ready_ = !places_.empty();
    return ready_;
}

OutdoorLight LightTable::at(uint32_t map, float x, float y, float z, int halfMinute) const {
    OutdoorLight out;
    int chosen = -1;
    int base = -1;
    float tight = 1.0e30f;
    for (int i = 0; i < int(places_.size()); ++i) {
        const Place& place = places_[i];
        if (place.map != map) continue;
        if (place.falloffEnd <= 0.f && place.falloffStart <= 0.f) {
            base = i;
            continue;
        }
        const float dx = place.x - x, dy = place.y - y, dz = place.z - z;
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist < place.falloffEnd && place.falloffEnd < tight) {
            tight = place.falloffEnd;
            chosen = i;
        }
    }
    if (chosen < 0) chosen = base;
    if (chosen < 0) return out;
    const uint32_t param = places_[size_t(chosen)].param;
    uint32_t packed = 0;
    float ignored = 0;
    auto color = [&](int row, uint8_t dest[3]) {
        const auto found = colors_.find(param * 18 - 17 + uint32_t(row));
        if (found == colors_.end() || !sampleBand(found->second, halfMinute, ignored, packed, false)) return false;
        dest[0] = uint8_t((packed >> 16) & 255);
        dest[1] = uint8_t((packed >> 8) & 255);
        dest[2] = uint8_t(packed & 255);
        return true;
    };
    if (!color(0, out.direct) || !color(1, out.ambient)) return out;
    for (int row = 0; row < 5; ++row)
        if (!color(2 + row, out.sky[row])) return out;
    if (!color(7, out.fog)) return out;
    const auto endBand = floats_.find(param * 6 - 5);
    const auto scaleBand = floats_.find(param * 6 - 5 + 1);
    if (endBand == floats_.end() || scaleBand == floats_.end()) return out;
    float end = 0, scale = 0;
    if (!sampleBand(endBand->second, halfMinute, end, packed, true)) return out;
    if (!sampleBand(scaleBand->second, halfMinute, scale, packed, true)) return out;
    // The stored fog distance is the yard distance times 36.
    end /= 36.f;
    if (scale < 0.f) scale = 0.f;
    if (scale > 1.f) scale = 1.f;
    out.fogEnd = end;
    out.fogStart = end * scale;
    out.ok = end > 1.f;
    return out;
}

}  // namespace reclient
