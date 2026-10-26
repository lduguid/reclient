#include "Scene.hpp"

#include "Mpq.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace reclient {
namespace {

constexpr float kNear = 0.35f;

struct Vert {
    float x, y, z;
    float u, v;
    float r, g, b;
};

float focalOf(int height) { return float(height) * (420.f / 720.f); }

void toCamera(const Camera& cam, float x, float y, float z, float& rx, float& up, float& ahead) {
    // Facing 0 looks north (+x). Screen right is (sin o, -cos o). Positive pitch looks down.
    const float dx = x - cam.x;
    const float dy = y - cam.y;
    const float dz = z - cam.z;
    const float c = std::cos(cam.o);
    const float s = std::sin(cam.o);
    const float right = dx * s - dy * c;
    const float forward = dx * c + dy * s;
    const float cp = std::cos(cam.pitch);
    const float sp = std::sin(cam.pitch);
    rx = right;
    ahead = forward * cp - dz * sp;
    up = forward * sp + dz * cp;
}

Vert lerpVert(const Vert& a, const Vert& b, float t) {
    Vert o;
    o.x = a.x + (b.x - a.x) * t;
    o.y = a.y + (b.y - a.y) * t;
    o.z = a.z + (b.z - a.z) * t;
    o.u = a.u + (b.u - a.u) * t;
    o.v = a.v + (b.v - a.v) * t;
    o.r = a.r + (b.r - a.r) * t;
    o.g = a.g + (b.g - a.g) * t;
    o.b = a.b + (b.b - a.b) * t;
    return o;
}

void sampleTexture(const Image* tex, float u, float v, int& r, int& g, int& b, int& a) {
    if (!tex || tex->width <= 0 || tex->height <= 0 || tex->rgba.empty()) {
        r = g = b = 168;
        a = 255;
        return;
    }
    const float uu = u - std::floor(u);
    const float vv = v - std::floor(v);
    const int tx = std::min(tex->width - 1, int(uu * float(tex->width)));
    const int ty = std::min(tex->height - 1, int(vv * float(tex->height)));
    const uint8_t* p = tex->rgba.data() + (size_t(ty) * tex->width + tx) * 4;
    r = p[0];
    g = p[1];
    b = p[2];
    a = p[3];
}

void rasterScreen(ColorBuffer& buf, const Vert& s0, const Vert& s1, const Vert& s2, const Image* tex) {
    float x0 = s0.x, y0 = s0.y, x1 = s1.x, y1 = s1.y, x2 = s2.x, y2 = s2.y;
    float z0 = s0.z, z1 = s1.z, z2 = s2.z;
    float u0 = s0.u, v0 = s0.v, u1 = s1.u, v1 = s1.v, u2 = s2.u, v2 = s2.v;
    float r0 = s0.r, g0 = s0.g, b0 = s0.b, r1 = s1.r, g1 = s1.g, b1 = s1.b, r2 = s2.r, g2 = s2.g, b2 = s2.b;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (area < 0.f) {
        std::swap(x1, x2);
        std::swap(y1, y2);
        std::swap(z1, z2);
        std::swap(u1, u2);
        std::swap(v1, v2);
        std::swap(r1, r2);
        std::swap(g1, g2);
        std::swap(b1, b2);
        area = -area;
    }
    if (area < 0.5f || z0 < kNear || z1 < kNear || z2 < kNear) return;
    const int minX = std::max(0, int(std::floor(std::min(x0, std::min(x1, x2)))));
    const int maxX = std::min(buf.width - 1, int(std::ceil(std::max(x0, std::max(x1, x2)))));
    const int minY = std::max(0, int(std::floor(std::min(y0, std::min(y1, y2)))));
    const int maxY = std::min(buf.height - 1, int(std::ceil(std::max(y0, std::max(y1, y2)))));
    if (maxX < minX || maxY < minY) return;
    const float invArea = 1.f / area;
    const float iz0 = 1.f / z0, iz1 = 1.f / z1, iz2 = 1.f / z2;
    const float w0dx = -(y2 - y1);
    const float w0dy = x2 - x1;
    const float w1dx = -(y0 - y2);
    const float w1dy = x0 - x2;
    const float w2dx = -(y1 - y0);
    const float w2dy = x1 - x0;
    const float originX = float(minX) + 0.5f;
    const float originY = float(minY) + 0.5f;
    float w0row = (x2 - x1) * (originY - y1) - (y2 - y1) * (originX - x1);
    float w1row = (x0 - x2) * (originY - y2) - (y0 - y2) * (originX - x2);
    float w2row = (x1 - x0) * (originY - y0) - (y1 - y0) * (originX - x0);
    for (int y = minY; y <= maxY; ++y) {
        float w0 = w0row;
        float w1 = w1row;
        float w2 = w2row;
        uint8_t* row = buf.rgba.data() + size_t(y) * buf.width * 4;
        float* zrow = buf.depth.data() + size_t(y) * buf.width;
        for (int x = minX; x <= maxX; ++x) {
            if (w0 >= 0.f && w1 >= 0.f && w2 >= 0.f) {
                const float a = w0 * invArea;
                const float b = w1 * invArea;
                const float c = w2 * invArea;
                const float iz = a * iz0 + b * iz1 + c * iz2;
                if (iz > 0.f) {
                    const float depth = 1.f / iz;
                    if (depth < zrow[x]) {
                        const float tu = (a * u0 * iz0 + b * u1 * iz1 + c * u2 * iz2) * depth;
                        const float tv = (a * v0 * iz0 + b * v1 * iz1 + c * v2 * iz2) * depth;
                        int tr, tg, tb, ta;
                        sampleTexture(tex, tu, tv, tr, tg, tb, ta);
                        if (ta >= 96) {
                            const float lr = (a * r0 * iz0 + b * r1 * iz1 + c * r2 * iz2) * depth;
                            const float lg = (a * g0 * iz0 + b * g1 * iz1 + c * g2 * iz2) * depth;
                            const float lb = (a * b0 * iz0 + b * b1 * iz1 + c * b2 * iz2) * depth;
                            row[x * 4] = uint8_t(std::clamp(int(lr * tr / 255.f), 0, 255));
                            row[x * 4 + 1] = uint8_t(std::clamp(int(lg * tg / 255.f), 0, 255));
                            row[x * 4 + 2] = uint8_t(std::clamp(int(lb * tb / 255.f), 0, 255));
                            row[x * 4 + 3] = 255;
                            zrow[x] = depth;
                        }
                    }
                }
            }
            w0 += w0dx;
            w1 += w1dx;
            w2 += w2dx;
        }
        w0row += w0dy;
        w1row += w1dy;
        w2row += w2dy;
    }
}

void projectVert(const Vert& cam, float focal, int width, int height, Vert& screen) {
    screen = cam;
    screen.x = width * 0.5f + cam.x / cam.z * focal;
    screen.y = height * 0.5f - cam.y / cam.z * focal;
    screen.z = cam.z;
}

void paintClipped(ColorBuffer& buf, Vert a, Vert b, Vert c, const Image* tex) {
    Vert poly[8];
    int n = 0;
    const Vert in[3] = {a, b, c};
    for (int i = 0; i < 3; ++i) {
        const Vert& s = in[i];
        const Vert& e = in[(i + 1) % 3];
        const bool sin = s.z >= kNear;
        const bool ein = e.z >= kNear;
        if (sin && ein) poly[n++] = e;
        else if (sin != ein) {
            const float t = (kNear - s.z) / (e.z - s.z);
            poly[n++] = lerpVert(s, e, t);
            if (ein) poly[n++] = e;
        }
    }
    if (n < 3) return;
    const float focal = focalOf(buf.height);
    Vert screen[8];
    for (int i = 0; i < n; ++i) projectVert(poly[i], focal, buf.width, buf.height, screen[i]);
    for (int i = 1; i + 1 < n; ++i) rasterScreen(buf, screen[0], screen[i], screen[i + 1], tex);
}

void paintMesh(ColorBuffer& buf, const Camera& cam, const LitMesh& mesh, const Mat4& world, const std::vector<Image>& images,
               int& budget) {
    const size_t count = mesh.xyz.size() / 3;
    if (count == 0 || budget <= 0) return;
    std::vector<Vert> verts(count);
    for (size_t i = 0; i < count; ++i) {
        float wx, wy, wz;
        transformPoint(world, mesh.xyz[i * 3], mesh.xyz[i * 3 + 1], mesh.xyz[i * 3 + 2], wx, wy, wz);
        Vert& v = verts[i];
        toCamera(cam, wx, wy, wz, v.x, v.y, v.z);
        v.u = i * 2 < mesh.uv.size() ? mesh.uv[i * 2] : 0.f;
        v.v = i * 2 + 1 < mesh.uv.size() ? mesh.uv[i * 2 + 1] : 0.f;
        v.r = i * 3 < mesh.rgb.size() ? mesh.rgb[i * 3] : 220.f;
        v.g = i * 3 + 1 < mesh.rgb.size() ? mesh.rgb[i * 3 + 1] : 210.f;
        v.b = i * 3 + 2 < mesh.rgb.size() ? mesh.rgb[i * 3 + 2] : 190.f;
    }
    auto draw = [&](uint32_t start, uint32_t nidx, int texture) {
        const Image* tex = texture >= 0 && size_t(texture) < images.size() ? &images[texture] : nullptr;
        for (uint32_t i = 0; i + 2 < nidx && budget > 0; i += 3) {
            const uint32_t ia = mesh.indices[start + i];
            const uint32_t ib = mesh.indices[start + i + 1];
            const uint32_t ic = mesh.indices[start + i + 2];
            if (ia >= count || ib >= count || ic >= count) continue;
            --budget;
            paintClipped(buf, verts[ia], verts[ib], verts[ic], tex);
        }
    };
    if (mesh.batches.empty()) draw(0, uint32_t(mesh.indices.size()), -1);
    else
        for (const PrimBatch& batch : mesh.batches) draw(batch.start, batch.count, batch.texture);
}

const char* mapDirectory(uint32_t map) {
    switch (map) {
    case 0: return "Azeroth";
    case 1: return "Kalimdor";
    case 530: return "Expansion01";
    default: return nullptr;
    }
}

int tileIndex(float world) {
    constexpr float kTile = 533.33333f;
    const int tile = 32 - int(std::floor(world / kTile)) - 1;
    if (tile < 0) return 0;
    if (tile > 63) return 63;
    return tile;
}

std::string groupFile(std::string path, int index) {
    char suffix[8];
    std::snprintf(suffix, sizeof(suffix), "_%03d", index);
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return path + suffix;
    path.insert(dot, suffix);
    return path;
}

float boxDistance(float x, float y, float z, float x0, float y0, float z0, float x1, float y1, float z1) {
    const float dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0.f;
    const float dy = y < y0 ? y0 - y : y > y1 ? y - y1 : 0.f;
    const float dz = z < z0 ? z0 - z : z > z1 ? z - z1 : 0.f;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

void transformedBox(const Mat4& world, const float bmin[3], const float bmax[3], float& x0, float& y0, float& z0, float& x1,
                    float& y1, float& z1) {
    bool any = false;
    for (int corner = 0; corner < 8; ++corner) {
        const float x = corner & 1 ? bmax[0] : bmin[0];
        const float y = corner & 2 ? bmax[1] : bmin[1];
        const float z = corner & 4 ? bmax[2] : bmin[2];
        float ox, oy, oz;
        transformPoint(world, x, y, z, ox, oy, oz);
        if (!any) {
            x0 = x1 = ox;
            y0 = y1 = oy;
            z0 = z1 = oz;
            any = true;
        } else {
            x0 = std::min(x0, ox);
            y0 = std::min(y0, oy);
            z0 = std::min(z0, oz);
            x1 = std::max(x1, ox);
            y1 = std::max(y1, oy);
            z1 = std::max(z1, oz);
        }
    }
}

void downsample(Image& image) {
    while ((image.width > 512 || image.height > 512) && image.width > 1 && image.height > 1) {
        Image next;
        next.width = std::max(1, image.width / 2);
        next.height = std::max(1, image.height / 2);
        next.rgba.resize(size_t(next.width) * next.height * 4);
        for (int y = 0; y < next.height; ++y) {
            for (int x = 0; x < next.width; ++x) {
                const int sx = std::min(image.width - 1, x * 2);
                const int sy = std::min(image.height - 1, y * 2);
                const uint8_t* p = image.rgba.data() + (size_t(sy) * image.width + sx) * 4;
                uint8_t* d = next.rgba.data() + (size_t(y) * next.width + x) * 4;
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
                d[3] = p[3];
            }
        }
        image = std::move(next);
    }
}

Mat4 identity() { return Mat4{}; }

std::vector<std::string> blpChoices(std::string path, uint32_t gender) {
    for (char& c : path)
        if (c == '/') c = '\\';
    std::vector<std::string> names;
    auto add = [&](const std::string& name) {
        for (const std::string& have : names) {
            if (have.size() != name.size()) continue;
            bool same = true;
            for (size_t i = 0; i < have.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(have[i])) != std::tolower(static_cast<unsigned char>(name[i]))) {
                    same = false;
                    break;
                }
            }
            if (same) return;
        }
        names.push_back(name);
    };
    std::string stem = path;
    if (stem.size() >= 4) {
        const std::string ext = stem.substr(stem.size() - 4);
        if (ext == ".blp" || ext == ".BLP") {
            add(path);
            stem.resize(stem.size() - 4);
        }
    }
    add(stem + "_U.blp");
    add(stem + (gender == 1 ? "_F.blp" : "_M.blp"));
    add(stem + ".blp");
    return names;
}

}  // namespace

void ColorBuffer::resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == width && h == height && rgba.size() == size_t(w * h * 4)) return;
    width = w;
    height = h;
    rgba.assign(size_t(w) * h * 4, 0);
    depth.assign(size_t(w) * h, 1.0e9f);
}

void ColorBuffer::clear(uint8_t r, uint8_t g, uint8_t b) {
    for (size_t i = 0; i < rgba.size(); i += 4) {
        rgba[i] = r;
        rgba[i + 1] = g;
        rgba[i + 2] = b;
        rgba[i + 3] = 255;
    }
    std::fill(depth.begin(), depth.end(), 1.0e9f);
}

bool ColorBuffer::occluded(float sx, float sy, float ahead) const {
    const int x = int(sx);
    const int y = int(sy);
    if (x < 0 || y < 0 || x >= width || y >= height) return true;
    return depth[size_t(y) * width + x] + 1.5f < ahead;
}

bool projectPoint(const Camera& cam, float x, float y, float z, int width, int height, float& sx, float& sy, float& ahead) {
    float rx, up;
    toCamera(cam, x, y, z, rx, up, ahead);
    if (ahead < kNear) return false;
    const float focal = focalOf(height);
    sx = width * 0.5f + rx / ahead * focal;
    sy = height * 0.5f - up / ahead * focal;
    return true;
}

int WorldScene::imageId(ArchiveSet& archives, const std::string& path) {
    if (path.empty()) return -1;
    const auto found = imageOf_.find(path);
    if (found != imageOf_.end()) return found->second;
    Image image;
    std::vector<uint8_t> bytes;
    if (archives.read(path, bytes)) decodeBlp(bytes.data(), bytes.size(), image);
    if (image.width > 0) downsample(image);
    const int id = int(images_.size());
    imageOf_[path] = id;
    images_.push_back(std::move(image));
    return image.width > 0 || !images_.back().rgba.empty() ? id : id;
}

std::string WorldScene::status() const {
    if (map_ != 0xffffffffu && !terrainOk_ && placements_.empty() && grounds_.empty()) return "map tile is not loaded";
    if (!jobsReady_ || !jobs_.empty()) return "loading the hall";
    return {};
}

void WorldScene::dressTerrain(ArchiveSet& archives, Terrain& tile) {
    auto picture = [&](std::string path) {
        if (path.empty()) return -1;
        for (char& c : path)
            if (c == '/') c = '\\';
        const bool blp = path.size() >= 4 && (path.compare(path.size() - 4, 4, ".blp") == 0 ||
                                               path.compare(path.size() - 4, 4, ".BLP") == 0);
        if (!blp) path += ".blp";
        return imageId(archives, path);
    };
    for (Terrain::Surface& surface : tile.surfaces) surface.image = picture(surface.path);
    for (Terrain::Coat& coat : tile.coats) coat.image = picture(coat.path);
}

WorldScene::SoundSpot WorldScene::soundSpot(ArchiveSet& files) {
    SoundSpot spot;
    for (size_t i = 0; i < placements_.size() && i < roots_.size(); ++i) {
        if (i >= rootState_.size() || rootState_[i] != 1 || roots_[i].wmoId == 0) continue;
        for (size_t g = 0; g < roots_[i].groups.size(); ++g) {
            WmoGroupBox& box = roots_[i].groups[g];
            if ((box.flags & 0x2000) == 0) continue;
            float x0, y0, z0, x1, y1, z1;
            transformedBox(placements_[i].world, box.min, box.max, x0, y0, z0, x1, y1, z1);
            if (boxDistance(viewerX_, viewerY_, viewerZ_, x0, y0, z0, x1, y1, z1) != 0.f) continue;
            spot.indoors = true;
            spot.wmoRoot = int32_t(roots_[i].wmoId);
            spot.wmoIndex = int32_t(g);
            if (box.groupId == -1) {
                std::vector<uint8_t> bytes;
                int32_t id = -2;
                if (files.read(groupFile(placements_[i].path, int(g)), bytes)) {
                    const int32_t got = wmoGroupFileId(bytes.data(), bytes.size());
                    if (got >= 0) id = got;
                }
                box.groupId = id;
            }
            spot.wmoGroup = box.groupId >= 0 ? box.groupId : -1;
            i = placements_.size();
            break;
        }
    }
    for (const GroundTile& tile : grounds_) {
        const uint32_t id = terrainArea(tile.mesh, viewerX_, viewerY_);
        if (id) {
            spot.chunk = id;
            break;
        }
    }
    if (!spot.chunk && terrainOk_) spot.chunk = terrainArea(terrain_, viewerX_, viewerY_);
    return spot;
}

void WorldScene::refreshIndoors() {
    const bool wasInside = indoors_;
    const float pad = wasInside ? 3.f : 0.f;
    indoors_ = false;
    for (size_t i = 0; i < placements_.size() && i < roots_.size(); ++i) {
        if (i >= rootState_.size() || rootState_[i] != 1) continue;
        for (const WmoGroupBox& box : roots_[i].groups) {
            if ((box.flags & 0x2000) == 0) continue;
            float x0, y0, z0, x1, y1, z1;
            transformedBox(placements_[i].world, box.min, box.max, x0, y0, z0, x1, y1, z1);
            if (boxDistance(viewerX_, viewerY_, viewerZ_, x0 - pad, y0 - pad, z0 - pad, x1 + pad, y1 + pad, z1 + pad) == 0.f) {
                indoors_ = true;
                return;
            }
        }
    }
}

void WorldScene::queueDoodads(float x, float y, float z) {
    const float movedX = x - doodadScanX_;
    const float movedY = y - doodadScanY_;
    if (movedX * movedX + movedY * movedY < 12.f * 12.f) return;
    doodadScanX_ = x;
    doodadScanY_ = y;
    for (size_t i = 0; i < placements_.size(); ++i) {
        if (placements_[i].wmo || spawnedDoodads_.count(int(i))) continue;
        if (std::find(pendingDoodads_.begin(), pendingDoodads_.end(), int(i)) != pendingDoodads_.end()) continue;
        // Baked into the map tile, so the server cannot spawn or remove it.
        if (seasonalHidden(placements_[i].path)) continue;
        float ox, oy, oz;
        transformPoint(placements_[i].world, 0, 0, 0, ox, oy, oz);
        const float dx = ox - x, dy = oy - y, dz = oz - z;
        if (dx * dx + dy * dy + dz * dz < kDistCull * kDistCull) pendingDoodads_.push_back(int(i));
    }
}

bool WorldScene::readGroundTile(ArchiveSet& archives, int tx, int ty) {
    if (tx < 0 || ty < 0 || tx > 63 || ty > 63) return false;
    if (tx == tileX_ && ty == tileY_ && terrainOk_) return false;
    for (const GroundTile& tile : grounds_)
        if (tile.x == tx && tile.y == ty) return false;
    const int key = tx * 64 + ty;
    if (groundMiss_.count(key)) return false;
    const char* dir = mapDirectory(map_);
    if (!dir) return false;
    const std::string path = std::string("World\\Maps\\") + dir + "\\" + dir + "_" + std::to_string(tx) + "_" +
                             std::to_string(ty) + ".adt";
    std::vector<uint8_t> bytes;
    GroundTile tile;
    tile.x = tx;
    tile.y = ty;
    if (!archives.read(path, bytes) || !loadAdt(bytes.data(), bytes.size(), tx, ty, tile.mesh)) {
        groundMiss_.insert(key);
        return false;
    }
    dressTerrain(archives, tile.mesh);
    grounds_.push_back(std::move(tile));
    return true;
}

struct WorldScene::GroupBuild {
    bool ok = false;
    bool doodad = false;
    uint32_t generation = 0;
    int placement = 0;
    int group = 0;
    int doodadIndex = -1;
    std::string modelKey;
    Mat4 doodadWorld;
    LitMesh doodadMesh;
    Prop prop;
    struct Tex {
        std::string name;
        Image image;
        bool ok = false;
    };
    std::vector<Tex> textures;
};

LitMesh drawnMesh(const LitMesh& src) {
    LitMesh mesh;
    mesh.xyz = src.rest.empty() ? src.xyz : src.rest;
    mesh.uv = src.uv;
    mesh.rgb = src.rgb;
    mesh.indices = src.indices;
    mesh.batches = src.batches;
    return mesh;
}

WorldScene::~WorldScene() {
    if (groupBusy_) groupFuture_.wait();
    if (modelBusy_) modelFuture_.wait();
}

void WorldScene::flushLoad() {
    if (modelBusy_) {
        modelFuture_.wait();
        installModel();
    }
    if (!groupBusy_) return;
    groupFuture_.wait();
    try {
        groupFuture_.get();
    } catch (...) {
    }
    installGroup();
    groupBusy_ = false;
    groupSlot_.reset();
}

void WorldScene::installGroup() {
    if (!groupSlot_ || groupSlot_->generation != loadGeneration_) return;
    if (groupSlot_->doodad) {
        spawnedDoodads_.insert(groupSlot_->doodadIndex);
        if (!groupSlot_->ok || models_.size() >= 180) return;
        ModelCache cache;
        cache.mesh = std::move(groupSlot_->doodadMesh);
        for (PrimBatch& batch : cache.mesh.batches) {
            if (batch.texture < 0 || size_t(batch.texture) >= groupSlot_->textures.size()) {
                batch.texture = -1;
                continue;
            }
            GroupBuild::Tex& tex = groupSlot_->textures[size_t(batch.texture)];
            if (!tex.ok) {
                batch.texture = -1;
                continue;
            }
            const auto known = imageOf_.find(tex.name);
            if (known != imageOf_.end()) {
                batch.texture = known->second;
                if (cache.image < 0) cache.image = known->second;
                continue;
            }
            const int id = int(images_.size());
            imageOf_[tex.name] = id;
            images_.push_back(std::move(tex.image));
            batch.texture = id;
            if (cache.image < 0) cache.image = id;
        }
        if (cache.image < 0) {
            for (GroupBuild::Tex& tex : groupSlot_->textures) {
                if (!tex.ok) continue;
                const auto known = imageOf_.find(tex.name);
                int id = known != imageOf_.end() ? known->second : int(images_.size());
                if (known == imageOf_.end()) {
                    imageOf_[tex.name] = id;
                    images_.push_back(std::move(tex.image));
                }
                cache.image = id;
                for (PrimBatch& batch : cache.mesh.batches)
                    if (batch.texture < 0) batch.texture = id;
                break;
            }
        }
        const int model = int(models_.size());
        modelOf_[groupSlot_->modelKey] = model;
        models_.push_back(std::move(cache));
        if (groupSlot_->doodadIndex < 0 || size_t(groupSlot_->doodadIndex) >= placements_.size()) return;
        Prop placed;
        placed.mesh = drawnMesh(models_[model].mesh);
        placed.world = groupSlot_->doodadWorld;
        solid_.push_back(std::move(placed));
        return;
    }
    if (!groupSlot_->ok) return;
    Prop& prop = groupSlot_->prop;
    for (PrimBatch& batch : prop.mesh.batches) {
        if (batch.texture < 0 || size_t(batch.texture) >= groupSlot_->textures.size()) {
            batch.texture = -1;
            continue;
        }
        GroupBuild::Tex& tex = groupSlot_->textures[size_t(batch.texture)];
        if (!tex.ok) {
            batch.texture = -1;
            continue;
        }
        const auto known = imageOf_.find(tex.name);
        if (known != imageOf_.end()) {
            batch.texture = known->second;
            continue;
        }
        const int id = int(images_.size());
        imageOf_[tex.name] = id;
        images_.push_back(std::move(tex.image));
        batch.texture = id;
    }
    loadedGroups_.insert((uint32_t(groupSlot_->placement) << 16) | uint32_t(groupSlot_->group));
    solid_.push_back(std::move(prop));
}

bool WorldScene::advance(ArchiveSet& archives, uint32_t map, float x, float y, float z) {
    viewerX_ = x;
    viewerY_ = y;
    viewerZ_ = z;
    const int tx = tileIndex(y);
    const int ty = tileIndex(x);
    if (map != map_ || tx != tileX_ || ty != tileY_) {
        const bool mapChanged = map != map_;
        if (mapChanged) {
            grounds_.clear();
            groundMiss_.clear();
        } else if (terrainOk_) {
            grounds_.push_back(GroundTile{tileX_, tileY_, std::move(terrain_)});
            terrain_ = Terrain{};
            terrainOk_ = false;
        }
        const int tileSpan = std::max(1, int(std::ceil(viewFar_ / 533.33333f)));
        for (size_t i = 0; i < grounds_.size();) {
            const int dx = std::abs(grounds_[i].x - tx);
            const int dy = std::abs(grounds_[i].y - ty);
            if (dx > tileSpan || dy > tileSpan) {
                grounds_.erase(grounds_.begin() + i);
                continue;
            }
            if (grounds_[i].x == tx && grounds_[i].y == ty) {
                terrain_ = std::move(grounds_[i].mesh);
                terrainOk_ = !terrain_.xyz.empty();
                grounds_.erase(grounds_.begin() + i);
                continue;
            }
            ++i;
        }
        map_ = map;
        tileX_ = tx;
        tileY_ = ty;
        solid_.clear();
        ++loadGeneration_;
        jobs_.clear();
        roots_.clear();
        rootState_.clear();
        placements_.clear();
        pendingDoodads_.clear();
        spawnedDoodads_.clear();
        doodadScanX_ = 1.0e9f;
        doodadScanY_ = 1.0e9f;
        loadedGroups_.clear();
        jobsReady_ = false;
        indoors_ = false;
        indoorsBuilt_ = false;
        const char* dir = mapDirectory(map);
        if (!dir) return false;
        const std::string path = std::string("World\\Maps\\") + dir + "\\" + dir + "_" + std::to_string(tx) + "_" +
                                 std::to_string(ty) + ".adt";
        std::vector<uint8_t> bytes;
        if (!archives.read(path, bytes)) return false;
        if (!terrainOk_) {
            terrainOk_ = loadAdt(bytes.data(), bytes.size(), tx, ty, terrain_);
            if (terrainOk_) dressTerrain(archives, terrain_);
        }
        loadAdtPlacements(bytes.data(), bytes.size(), placements_);
        roots_.resize(placements_.size());
        rootState_.assign(placements_.size(), 0);
        queueDoodads(x, y, z);
    }

    refreshIndoors();
    if (jobsReady_) queueDoodads(x, y, z);

    if (showTerrain()) {
        bool loadedNeighbor = false;
        const int tileSpan = std::max(1, int(std::ceil(viewFar_ / 533.33333f)));
        for (int oy = -tileSpan; oy <= tileSpan && !loadedNeighbor; ++oy) {
            for (int ox = -tileSpan; ox <= tileSpan && !loadedNeighbor; ++ox) {
                if (ox == 0 && oy == 0) continue;
                if (readGroundTile(archives, tileX_ + ox, tileY_ + oy)) loadedNeighbor = true;
            }
        }
        if (loadedNeighbor) return true;
    }

    for (size_t i = 0; i < placements_.size(); ++i) {
        if (!placements_[i].wmo || rootState_[i] != 0) continue;
        if (placements_[i].hasBox && boxDistance(x, y, z, placements_[i].minX, placements_[i].minY, placements_[i].minZ,
                                                 placements_[i].maxX, placements_[i].maxY, placements_[i].maxZ) > 120.f) {
            rootState_[i] = 2;
            continue;
        }
        std::vector<uint8_t> bytes;
        if (archives.read(placements_[i].path, bytes) && loadWmoRoot(bytes.data(), bytes.size(), roots_[i])) rootState_[i] = 1;
        else rootState_[i] = 2;
        return true;
    }

    if (!jobsReady_) {
        pruneFar(x, y, z);
        bool insideInterior = false;
        struct Candidate {
            int placement;
            int group;
            float distance;
            bool interior;
            bool contains;
        };
        std::vector<Candidate> candidates;
        for (size_t i = 0; i < placements_.size(); ++i) {
            if (rootState_[i] != 1) continue;
            for (int g = 0; g < int(roots_[i].groups.size()); ++g) {
                const WmoGroupBox& box = roots_[i].groups[g];
                if (box.flags & 0x4000000) continue;
                float x0, y0, z0, x1, y1, z1;
                transformedBox(placements_[i].world, box.min, box.max, x0, y0, z0, x1, y1, z1);
                const float distance = boxDistance(x, y, z, x0, y0, z0, x1, y1, z1);
                const bool interior = (box.flags & 0x2000) != 0;
                const bool always = (box.flags & 0x10000) != 0;
                const bool contains = distance == 0.f;
                if (contains && interior) insideInterior = true;
                if (distance < 160.f) candidates.push_back({int(i), g, distance, interior || always, contains});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            if (a.contains != b.contains) return a.contains;
            const bool aNear = a.distance < 55.f;
            const bool bNear = b.distance < 55.f;
            if (aNear != bNear) return aNear;
            if (a.interior != b.interior) return a.interior;
            return a.distance < b.distance;
        });
        indoors_ = insideInterior;
        indoorsBuilt_ = indoors_;
        queueDoodads(x, y, z);
        const int room = std::max(0, 96 - int(loadedGroups_.size()));
        for (const Candidate& candidate : candidates) {
            // Keep the shell just outside a door loaded before you reach it.
            if (indoors_ && !candidate.interior && !candidate.contains && candidate.distance > 110.f) continue;
            const uint32_t key = (uint32_t(candidate.placement) << 16) | uint32_t(candidate.group);
            if (loadedGroups_.count(key)) continue;
            if (int(jobs_.size()) >= room) break;
            jobs_.push_back({candidate.placement, candidate.group, candidate.distance});
        }
        jobsReady_ = true;
        builtX_ = x;
        builtY_ = y;
        return !jobs_.empty() || !pendingDoodads_.empty();
    }

    if (jobs_.empty() && pendingDoodads_.empty()) {
        bool loadedNeighbor = false;
        const int tileSpan = std::max(1, int(std::ceil(viewFar_ / 533.33333f)));
        for (int oy = -tileSpan; oy <= tileSpan && !loadedNeighbor; ++oy) {
            for (int ox = -tileSpan; ox <= tileSpan && !loadedNeighbor; ++ox) {
                if (ox == 0 && oy == 0) continue;
                if (readGroundTile(archives, tileX_ + ox, tileY_ + oy)) loadedNeighbor = true;
            }
        }
        if (loadedNeighbor) return true;
        const float dx = x - builtX_;
        const float dy = y - builtY_;
        if (indoors_ != indoorsBuilt_ || dx * dx + dy * dy > 24.f * 24.f) {
            jobsReady_ = false;
            return true;
        }
    }

    if (groupBusy_) {
        if (groupFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                groupFuture_.get();
            } catch (...) {
            }
            installGroup();
            groupBusy_ = false;
            groupSlot_.reset();
        }
        return true;
    }

    if (!jobs_.empty()) {
        const Job job = jobs_.front();
        jobs_.erase(jobs_.begin());
        const MapPlacement& place = placements_[job.placement];
        auto built = std::make_shared<GroupBuild>();
        built->generation = loadGeneration_;
        built->placement = job.placement;
        built->group = job.group;
        const std::string path = groupFile(place.path, job.group);
        const Mat4 world = place.world;
        const std::vector<WmoMaterial> materials = roots_[job.placement].materials;
        ArchiveSet* files = &archives;
        groupSlot_ = built;
        groupFuture_ = std::async(std::launch::async, [built, files, path, world, materials]() {
            std::vector<uint8_t> bytes;
            LitMesh mesh;
            if (!files->read(path, bytes) || !loadWmoGroupMesh(bytes.data(), bytes.size(), world, materials, mesh)) return;
            built->prop.mesh = std::move(mesh);
            built->prop.world = identity();
            built->prop.placement = built->placement;
            built->prop.group = built->group;
            for (PrimBatch& batch : built->prop.mesh.batches) {
                if (batch.texture < 0 || size_t(batch.texture) >= materials.size()) {
                    batch.texture = -1;
                    continue;
                }
                GroupBuild::Tex tex;
                tex.name = materials[size_t(batch.texture)].texture;
                std::vector<uint8_t> imageBytes;
                if (!tex.name.empty() && files->read(tex.name, imageBytes) &&
                    decodeBlp(imageBytes.data(), imageBytes.size(), tex.image) && tex.image.width > 0) {
                    tex.ok = true;
                }
                batch.texture = int(built->textures.size());
                built->textures.push_back(std::move(tex));
            }
            Prop& prop = built->prop;
            if (!prop.mesh.xyz.empty()) {
                prop.minX = prop.maxX = prop.mesh.xyz[0];
                prop.minY = prop.maxY = prop.mesh.xyz[1];
                prop.minZ = prop.maxZ = prop.mesh.xyz[2];
                for (size_t v = 3; v + 2 < prop.mesh.xyz.size(); v += 3) {
                    prop.minX = std::min(prop.minX, prop.mesh.xyz[v]);
                    prop.maxX = std::max(prop.maxX, prop.mesh.xyz[v]);
                    prop.minY = std::min(prop.minY, prop.mesh.xyz[v + 1]);
                    prop.maxY = std::max(prop.maxY, prop.mesh.xyz[v + 1]);
                    prop.minZ = std::min(prop.minZ, prop.mesh.xyz[v + 2]);
                    prop.maxZ = std::max(prop.maxZ, prop.mesh.xyz[v + 2]);
                }
                prop.bounds = true;
                constexpr uint32_t kSpan = 256;
                const size_t tris = prop.mesh.indices.size() / 3;
                prop.spans.reserve((tris + kSpan - 1) / kSpan);
                for (size_t t = 0; t < tris; t += kSpan) {
                    Prop::Span span;
                    span.begin = uint32_t(t);
                    span.count = uint32_t(std::min(size_t(kSpan), tris - t));
                    bool seeded = false;
                    for (uint32_t n = 0; n < span.count; ++n) {
                        const size_t tri = size_t(span.begin) + n;
                        for (int k = 0; k < 3; ++k) {
                            const uint32_t index = prop.mesh.indices[tri * 3 + k];
                            if (index * 3 + 2 >= prop.mesh.xyz.size()) continue;
                            const float vx = prop.mesh.xyz[index * 3];
                            const float vy = prop.mesh.xyz[index * 3 + 1];
                            const float vz = prop.mesh.xyz[index * 3 + 2];
                            if (!seeded) {
                                span.minX = span.maxX = vx;
                                span.minY = span.maxY = vy;
                                span.minZ = span.maxZ = vz;
                                seeded = true;
                            } else {
                                span.minX = std::min(span.minX, vx);
                                span.maxX = std::max(span.maxX, vx);
                                span.minY = std::min(span.minY, vy);
                                span.maxY = std::max(span.maxY, vy);
                                span.minZ = std::min(span.minZ, vz);
                                span.maxZ = std::max(span.maxZ, vz);
                            }
                        }
                    }
                    if (seeded) prop.spans.push_back(span);
                }
            }
            built->ok = true;
        });
        groupBusy_ = true;
        return true;
    }

    if (!pendingDoodads_.empty()) {
        if (models_.size() >= 160) return false;
        for (int n = 0; n < 1 && !pendingDoodads_.empty(); ++n) {
            if (loadedThisFrame_) break;
            const int index = pendingDoodads_.back();
            pendingDoodads_.pop_back();
            if (index < 0 || size_t(index) >= placements_.size()) continue;
            const MapPlacement& doodad = placements_[index];
            const int model = prepareModel(archives, doodad.path, false);
            if (model == -2) {
                auto built = std::make_shared<GroupBuild>();
                built->doodad = true;
                built->doodadIndex = index;
                built->generation = loadGeneration_;
                built->modelKey = doodad.path;
                built->doodadWorld = doodad.world;
                ArchiveSet* files = &archives;
                const std::string path = doodad.path;
                groupSlot_ = built;
                groupFuture_ = std::async(std::launch::async, [built, files, path]() {
                    std::vector<uint8_t> bytes;
                    std::string texture;
                    if (!files->read(path, bytes) || !loadM2Mesh(bytes.data(), bytes.size(), built->doodadMesh, texture)) return;
                    std::vector<std::string> names = built->doodadMesh.textures;
                    if (names.empty() && !texture.empty()) names.push_back(texture);
                    auto rooted = [](std::string file, const std::string& model) {
                        for (char& c : file)
                            if (c == '/') c = '\\';
                        if (file.find('\\') == std::string::npos) {
                            const size_t slash = model.find_last_of('\\');
                            if (slash != std::string::npos) file = model.substr(0, slash + 1) + file;
                        }
                        if (file.size() >= 4) {
                            const std::string ext = file.substr(file.size() - 4);
                            if (ext != ".blp" && ext != ".BLP" && ext != ".m2" && ext != ".M2") file += ".blp";
                        }
                        return file;
                    };
                    for (const std::string& name : names) {
                        GroupBuild::Tex tex;
                        if (!name.empty()) {
                            for (const std::string& candidate : blpChoices(rooted(name, path), 0)) {
                                std::vector<uint8_t> imageBytes;
                                if (!files->read(candidate, imageBytes)) continue;
                                if (decodeBlp(imageBytes.data(), imageBytes.size(), tex.image) && tex.image.width > 0) {
                                    tex.name = candidate;
                                    tex.ok = true;
                                    break;
                                }
                            }
                        }
                        built->textures.push_back(std::move(tex));
                    }
                    built->ok = true;
                });
                groupBusy_ = true;
                return true;
            }
            spawnedDoodads_.insert(index);
            if (model >= 0) {
                Prop prop;
                prop.mesh = drawnMesh(models_[model].mesh);
                prop.world = doodad.world;
                solid_.push_back(std::move(prop));
            }
        }
        return !pendingDoodads_.empty();
    }
    return false;
}

void WorldScene::raster(ColorBuffer& buffer, const Camera& cam) const {
    int budget = 80000;
    if (!indoors_ && terrainOk_) {
        const size_t tris = terrain_.indices.size() / 3;
        for (size_t t = 0; t < tris && budget > 0; ++t) {
            const uint32_t i0 = terrain_.indices[t * 3];
            const uint32_t i1 = terrain_.indices[t * 3 + 1];
            const uint32_t i2 = terrain_.indices[t * 3 + 2];
            const float x0 = terrain_.xyz[i0 * 3], y0 = terrain_.xyz[i0 * 3 + 1], z0 = terrain_.xyz[i0 * 3 + 2];
            const float x1 = terrain_.xyz[i1 * 3], y1 = terrain_.xyz[i1 * 3 + 1], z1 = terrain_.xyz[i1 * 3 + 2];
            const float x2 = terrain_.xyz[i2 * 3], y2 = terrain_.xyz[i2 * 3 + 1], z2 = terrain_.xyz[i2 * 3 + 2];
            const float mx = (x0 + x1 + x2) / 3.f;
            const float my = (y0 + y1 + y2) / 3.f;
            const float dx = mx - cam.x;
            const float dy = my - cam.y;
            if (dx * dx + dy * dy > 110.f * 110.f) continue;
            const float nx = (y1 - y0) * (z2 - z0) - (z1 - z0) * (y2 - y0);
            const float ny = (z1 - z0) * (x2 - x0) - (x1 - x0) * (z2 - z0);
            const float nz = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
            const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            const float up = len > 0.001f ? std::fabs(nz) / len : 1.f;
            const int shade = std::clamp(int(70 + up * 90), 40, 180);
            Vert a, b, c;
            toCamera(cam, x0, y0, z0, a.x, a.y, a.z);
            toCamera(cam, x1, y1, z1, b.x, b.y, b.z);
            toCamera(cam, x2, y2, z2, c.x, c.y, c.z);
            const float red = up > 0.75f ? shade * 0.45f : shade * 0.55f;
            const float green = up > 0.75f ? shade * 0.72f : shade * 0.48f;
            const float blue = up > 0.75f ? shade * 0.28f : shade * 0.32f;
            a.r = b.r = c.r = red;
            a.g = b.g = c.g = green;
            a.b = b.b = c.b = blue;
            a.u = b.u = c.u = a.v = b.v = c.v = 0.f;
            --budget;
            paintClipped(buffer, a, b, c, nullptr);
        }
    }
    for (const Prop& prop : solid_) paintMesh(buffer, cam, prop.mesh, prop.world, images_, budget);
}

void WorldScene::rasterModel(ColorBuffer& buffer, const Camera& cam, int model, const Mat4& world) const {
    if (model < 0 || size_t(model) >= models_.size() || models_[model].failed) return;
    int budget = 4000;
    paintMesh(buffer, cam, models_[model].mesh, world, images_, budget);
}

void WorldScene::pruneFar(float x, float y, float z) {
    std::vector<Prop> keep;
    keep.reserve(solid_.size());
    for (Prop& prop : solid_) {
        bool drop = false;
        if (prop.group >= 0 && prop.placement >= 0 && size_t(prop.placement) < roots_.size() &&
            prop.group < int(roots_[prop.placement].groups.size())) {
            const WmoGroupBox& box = roots_[prop.placement].groups[prop.group];
            float x0, y0, z0, x1, y1, z1;
            transformedBox(placements_[prop.placement].world, box.min, box.max, x0, y0, z0, x1, y1, z1);
            drop = boxDistance(x, y, z, x0, y0, z0, x1, y1, z1) > 220.f;
        }
        if (drop) loadedGroups_.erase((uint32_t(prop.placement) << 16) | uint32_t(prop.group));
        else keep.push_back(std::move(prop));
    }
    solid_ = std::move(keep);
}

void WorldScene::drawables(std::vector<Drawable>& out) const {
    out.clear();
    out.reserve(solid_.size());
    for (const Prop& prop : solid_) {
        if (prop.mesh.xyz.empty() || prop.mesh.indices.empty()) continue;
        out.push_back({&prop.mesh, prop.world});
    }
}

LitMesh* WorldScene::modelMesh(int index) {
    if (index < 0 || size_t(index) >= models_.size() || models_[index].failed) return nullptr;
    return &models_[index].mesh;
}

const LitMesh* WorldScene::modelMesh(int index) const {
    if (index < 0 || size_t(index) >= models_.size() || models_[index].failed) return nullptr;
    return &models_[index].mesh;
}

const Image* WorldScene::image(int index) const {
    if (index < 0 || size_t(index) >= images_.size()) return nullptr;
    const Image& found = images_[index];
    if (found.width <= 0 || found.rgba.empty()) return nullptr;
    return &found;
}

bool WorldScene::showTerrain() const {
    // The outdoor ground stays up while you are indoors, so a doorway already shows it.
    // Hole quads are omitted, which is what keeps the mountain shell out of the room.
    return terrainOk_ || !grounds_.empty();
}

void WorldScene::terrains(std::vector<const Terrain*>& out) const {
    out.clear();
    if (terrainOk_ && !terrain_.xyz.empty()) out.push_back(&terrain_);
    for (const GroundTile& tile : grounds_)
        if (!tile.mesh.xyz.empty()) out.push_back(&tile.mesh);
}

bool rayEnters(float ox, float oy, float oz, float dx, float dy, float dz, float reach, float minX, float minY, float minZ,
              float maxX, float maxY, float maxZ) {
    float t0 = 0.f;
    float t1 = reach;
    auto slab = [&](float origin, float dir, float lo, float hi) {
        if (std::fabs(dir) < 1e-8f) return origin >= lo && origin <= hi;
        float a = (lo - origin) / dir;
        float b = (hi - origin) / dir;
        if (a > b) std::swap(a, b);
        if (a > t0) t0 = a;
        if (b < t1) t1 = b;
        return t0 <= t1;
    };
    return slab(ox, dx, minX, maxX) && slab(oy, dy, minY, maxY) && slab(oz, dz, minZ, maxZ);
}

float WorldScene::clipCamera(float x, float y, float z, float dx, float dy, float dz, float dist) const {
    if (!(dist > 0.05f) || !std::isfinite(dist)) return dist;
    const float movedX = x - clipX_, movedY = y - clipY_, movedZ = z - clipZ_;
    const float aim = dx * clipDx_ + dy * clipDy_ + dz * clipDz_;
    // A shortened view is against a wall. Reuse it only while the aim has barely moved.
    const bool against = clipValid_ && clipOut_ + 0.2f < clipDist_;
    const float aimNeed = against ? 0.999f : 0.985f;
    const float movedNeed = against ? 0.05f : 0.4f;
    if (clipValid_ && clipSolids_ == int(solid_.size()) && clipIndoors_ == indoors_ &&
        movedX * movedX + movedY * movedY + movedZ * movedZ < movedNeed * movedNeed && aim > aimNeed &&
        std::fabs(dist - clipDist_) < 0.5f)
        return clipOut_;
    float limit = dist;
    float ox = x, oy = y, oz = z;
    auto closer = [&](float t) {
        if (t > 0.08f && t < limit) limit = t;
    };
    auto rayTri = [&](float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2) {
        const float e1x = x1 - x0, e1y = y1 - y0, e1z = z1 - z0;
        const float e2x = x2 - x0, e2y = y2 - y0, e2z = z2 - z0;
        const float px = dy * e2z - dz * e2y;
        const float py = dz * e2x - dx * e2z;
        const float pz = dx * e2y - dy * e2x;
        const float det = e1x * px + e1y * py + e1z * pz;
        if (std::fabs(det) < 1e-7f) return;
        const float inv = 1.f / det;
        const float sx = ox - x0, sy = oy - y0, sz = oz - z0;
        const float u = (sx * px + sy * py + sz * pz) * inv;
        if (u < -0.001f || u > 1.001f) return;
        const float qx = sy * e1z - sz * e1y;
        const float qy = sz * e1x - sx * e1z;
        const float qz = sx * e1y - sy * e1x;
        const float v = (dx * qx + dy * qy + dz * qz) * inv;
        if (v < -0.001f || u + v > 1.001f) return;
        const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
        closer(t);
    };
    const float endX = x + dx * dist, endY = y + dy * dist, endZ = z + dz * dist;
    const float boxMinX = std::min(x, endX) - 1.f, boxMaxX = std::max(x, endX) + 1.f;
    const float boxMinY = std::min(y, endY) - 1.f, boxMaxY = std::max(y, endY) + 1.f;
    const float boxMinZ = std::min(z, endZ) - 1.f, boxMaxZ = std::max(z, endZ) + 1.f;
    const float side = std::sqrt(dx * dx + dy * dy);
    const float rightX = side > 1e-4f ? dy / side * 0.32f : 0.32f;
    const float rightY = side > 1e-4f ? -dx / side * 0.32f : 0.f;
    const float sampleX[4] = {0.f, rightX, -rightX, 0.f};
    const float sampleY[4] = {0.f, rightY, -rightY, 0.f};
    const float sampleZ[4] = {0.f, 0.08f, 0.08f, 0.28f};
    for (int sample = 0; sample < 4; ++sample) {
        ox = x + sampleX[sample];
        oy = y + sampleY[sample];
        oz = z + sampleZ[sample];
    for (const Prop& prop : solid_) {
        if (!prop.bounds || prop.mesh.indices.size() < 3) continue;
        if (!rayEnters(ox, oy, oz, dx, dy, dz, dist, prop.minX, prop.minY, prop.minZ, prop.maxX, prop.maxY, prop.maxZ))
            continue;
        const LitMesh& mesh = prop.mesh;
        auto testSpan = [&](uint32_t begin, uint32_t count) {
            for (uint32_t n = 0; n < count; ++n) {
                const size_t t = size_t(begin) + n;
                const uint32_t i0 = mesh.indices[t * 3];
                const uint32_t i1 = mesh.indices[t * 3 + 1];
                const uint32_t i2 = mesh.indices[t * 3 + 2];
                if (i0 * 3 + 2 >= mesh.xyz.size() || i1 * 3 + 2 >= mesh.xyz.size() || i2 * 3 + 2 >= mesh.xyz.size()) continue;
                const float x0 = mesh.xyz[i0 * 3], y0 = mesh.xyz[i0 * 3 + 1], z0 = mesh.xyz[i0 * 3 + 2];
                const float x1 = mesh.xyz[i1 * 3], y1 = mesh.xyz[i1 * 3 + 1], z1 = mesh.xyz[i1 * 3 + 2];
                const float x2 = mesh.xyz[i2 * 3], y2 = mesh.xyz[i2 * 3 + 1], z2 = mesh.xyz[i2 * 3 + 2];
                if (std::max(x0, std::max(x1, x2)) < boxMinX || std::min(x0, std::min(x1, x2)) > boxMaxX) continue;
                if (std::max(y0, std::max(y1, y2)) < boxMinY || std::min(y0, std::min(y1, y2)) > boxMaxY) continue;
                if (std::max(z0, std::max(z1, z2)) < boxMinZ || std::min(z0, std::min(z1, z2)) > boxMaxZ) continue;
                rayTri(x0, y0, z0, x1, y1, z1, x2, y2, z2);
            }
        };
        if (prop.spans.empty()) testSpan(0, uint32_t(mesh.indices.size() / 3));
        else {
            for (const Prop::Span& span : prop.spans) {
                if (!rayEnters(ox, oy, oz, dx, dy, dz, dist, span.minX, span.minY, span.minZ, span.maxX, span.maxY,
                               span.maxZ))
                    continue;
                testSpan(span.begin, span.count);
            }
        }
    }
    if (showTerrain()) {
        float prevAbove = 1.f;
        const int steps = std::max(1, int(dist));
        for (int i = 1; i <= steps; ++i) {
            const float t = dist * float(i) / float(steps);
            if (t >= limit) break;
            const float px = ox + dx * t, py = oy + dy * t, pz = oz + dz * t;
            float hz = 0;
            bool hit = terrainOk_ && terrainHeight(terrain_, px, py, hz);
            if (!hit) {
                for (const GroundTile& tile : grounds_) {
                    if (terrainHeight(tile.mesh, px, py, hz)) {
                        hit = true;
                        break;
                    }
                }
            }
            const float above = hit ? pz - hz : 1.f;
            if (hit && prevAbove > 0.f && above < 0.35f) {
                const float denom = prevAbove - above;
                const float span = dist / float(steps);
                const float cross = denom > 1e-4f ? t - span * (0.35f - above) / denom : t;
                closer(cross);
                break;
            }
            prevAbove = above;
        }
    }
    }
    // Stay far enough in front of the hit that the near plane cannot cut the wall.
    float result = dist;
    if (limit < dist) {
        result = std::max(0.35f, limit - 0.5f);
        if (limit - result < 0.22f) result = std::max(0.2f, limit - 0.22f);
    }
    clipX_ = x;
    clipY_ = y;
    clipZ_ = z;
    clipDx_ = dx;
    clipDy_ = dy;
    clipDz_ = dz;
    clipDist_ = dist;
    clipSolids_ = int(solid_.size());
    clipIndoors_ = indoors_;
    clipOut_ = result;
    clipValid_ = true;
    return result;
}

float WorldScene::wallAhead(float x, float y, float z, float dx, float dy, float dist) const {
    if (!(dist > 0.02f)) return dist;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len < 1e-6f) return dist;
    dx /= len;
    dy /= len;
    const float dz = 0.f;
    // Start behind the body so a wall it is already touching still stops the step.
    const float radius = 0.45f;
    const float shoulder = 0.28f;
    const float sideX = -dy * shoulder;
    const float sideY = dx * shoulder;
    float best = dist;
    const float shiftX[3] = {0.f, sideX, -sideX};
    const float shiftY[3] = {0.f, sideY, -sideY};
    for (int sample = 0; sample < 3; ++sample) {
        const float ox = x + shiftX[sample] - dx * radius;
        const float oy = y + shiftY[sample] - dy * radius;
        const float reach = dist + radius;
        float limit = reach;
        auto rayTri = [&](float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2) {
            const float e1x = x1 - x0, e1y = y1 - y0, e1z = z1 - z0;
            const float e2x = x2 - x0, e2y = y2 - y0, e2z = z2 - z0;
            const float nx = e1y * e2z - e1z * e2y;
            const float ny = e1z * e2x - e1x * e2z;
            const float nz = e1x * e2y - e1y * e2x;
            const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (nl < 1e-6f || std::fabs(nz) / nl > 0.65f) return;
            const float px = dy * e2z - dz * e2y;
            const float py = dz * e2x - dx * e2z;
            const float pz = dx * e2y - dy * e2x;
            const float det = e1x * px + e1y * py + e1z * pz;
            if (std::fabs(det) < 1e-7f) return;
            const float inv = 1.f / det;
            const float sx = ox - x0, sy = oy - y0, sz = z - z0;
            const float u = (sx * px + sy * py + sz * pz) * inv;
            if (u < -0.001f || u > 1.001f) return;
            const float qx = sy * e1z - sz * e1y;
            const float qy = sz * e1x - sx * e1z;
            const float qz = sx * e1y - sy * e1x;
            const float v = (dx * qx + dy * qy + dz * qz) * inv;
            if (v < -0.001f || u + v > 1.001f) return;
            const float t = (e2x * qx + e2y * qy + e2z * qz) * inv;
            if (t > 0.02f && t < limit) limit = t;
        };
        for (const Prop& prop : solid_) {
            if (!prop.bounds || prop.mesh.indices.size() < 3) continue;
            if (!rayEnters(ox, oy, z, dx, dy, dz, reach, prop.minX, prop.minY, prop.minZ, prop.maxX, prop.maxY, prop.maxZ))
                continue;
            const LitMesh& mesh = prop.mesh;
            auto testSpan = [&](uint32_t begin, uint32_t count) {
                for (uint32_t n = 0; n < count; ++n) {
                    const size_t tri = size_t(begin) + n;
                    if (tri * 3 + 2 >= mesh.indices.size()) break;
                    const uint32_t i0 = mesh.indices[tri * 3];
                    const uint32_t i1 = mesh.indices[tri * 3 + 1];
                    const uint32_t i2 = mesh.indices[tri * 3 + 2];
                    if (i0 * 3 + 2 >= mesh.xyz.size() || i1 * 3 + 2 >= mesh.xyz.size() || i2 * 3 + 2 >= mesh.xyz.size())
                        continue;
                    rayTri(mesh.xyz[i0 * 3], mesh.xyz[i0 * 3 + 1], mesh.xyz[i0 * 3 + 2], mesh.xyz[i1 * 3],
                           mesh.xyz[i1 * 3 + 1], mesh.xyz[i1 * 3 + 2], mesh.xyz[i2 * 3], mesh.xyz[i2 * 3 + 1],
                           mesh.xyz[i2 * 3 + 2]);
                }
            };
            if (prop.spans.empty()) testSpan(0, uint32_t(mesh.indices.size() / 3));
            else {
                for (const Prop::Span& span : prop.spans) {
                    if (!rayEnters(ox, oy, z, dx, dy, dz, reach, span.minX, span.minY, span.minZ, span.maxX, span.maxY,
                                   span.maxZ))
                        continue;
                    testSpan(span.begin, span.count);
                }
            }
        }
        const float fromBody = limit - radius;
        // A shoulder already inside a wall must not freeze a step that slides along it.
        if (sample == 0 || fromBody > 0.02f) {
            if (fromBody < best) best = fromBody;
        }
    }
    return std::max(0.f, best);
}

bool WorldScene::floorAt(float x, float y, float z, float& height) const {
    int resolvedRoots = 0;
    for (char state : rootState_)
        if (state) ++resolvedRoots;
    auto triHeight = [&](const LitMesh& mesh, uint32_t tri, float& hz) {
        if (size_t(tri) * 3 + 2 >= mesh.indices.size()) return false;
        const uint32_t i0 = mesh.indices[tri * 3];
        const uint32_t i1 = mesh.indices[tri * 3 + 1];
        const uint32_t i2 = mesh.indices[tri * 3 + 2];
        if (i0 * 3 + 2 >= mesh.xyz.size() || i1 * 3 + 2 >= mesh.xyz.size() || i2 * 3 + 2 >= mesh.xyz.size()) return false;
        const float x0 = mesh.xyz[i0 * 3], y0 = mesh.xyz[i0 * 3 + 1], z0 = mesh.xyz[i0 * 3 + 2];
        const float x1 = mesh.xyz[i1 * 3], y1 = mesh.xyz[i1 * 3 + 1], z1 = mesh.xyz[i1 * 3 + 2];
        const float x2 = mesh.xyz[i2 * 3], y2 = mesh.xyz[i2 * 3 + 1], z2 = mesh.xyz[i2 * 3 + 2];
        const float det = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (std::fabs(det) < 1e-4f) return false;
        const float w0 = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / det;
        const float w1 = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / det;
        const float w2 = 1.f - w0 - w1;
        if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) return false;
        hz = w0 * z0 + w1 * z1 + w2 * z2;
        return true;
    };
    if (floorHintProp_ >= 0 && floorCacheSolids_ == int(solid_.size()) && size_t(floorHintProp_) < solid_.size()) {
        float hz = 0;
        if (triHeight(solid_[size_t(floorHintProp_)].mesh, floorHintTri_, hz) && hz <= z + 1.8f && hz >= z - 40.f) {
            floorCacheX_ = x;
            floorCacheY_ = y;
            floorCacheH_ = hz;
            floorCacheOk_ = true;
            height = hz;
            return true;
        }
    }
    if (std::fabs(x - floorCacheX_) < 0.3f && std::fabs(y - floorCacheY_) < 0.3f && floorCacheSolids_ == int(solid_.size()) &&
        floorCacheGrounds_ == int(grounds_.size()) + (terrainOk_ ? 1 : 0) && floorCacheRoots_ == resolvedRoots &&
        floorCacheIndoors_ == indoors_) {
        if (!floorCacheOk_) return false;
        height = floorCacheH_;
        return true;
    }
    auto consider = [&](float hz, float& best, float& bestGap, bool& any) {
        if (!std::isfinite(hz)) return;
        if (hz > z + 1.8f || hz < z - 40.f) return;
        const float gap = std::fabs(z - hz);
        if (gap < bestGap) {
            bestGap = gap;
            best = hz;
            any = true;
        }
    };
    bool terrainAny = false;
    float terrainBest = z;
    float terrainGap = 1.0e9f;
    if (terrainOk_ || !grounds_.empty()) {
        float hz = 0;
        if (terrainOk_ && terrainHeight(terrain_, x, y, hz)) consider(hz, terrainBest, terrainGap, terrainAny);
        for (const GroundTile& tile : grounds_)
            if (terrainHeight(tile.mesh, x, y, hz)) consider(hz, terrainBest, terrainGap, terrainAny);
    }
    bool any = false;
    float best = z;
    float bestGap = 1.0e9f;
    for (const Prop& prop : solid_) {
        if (prop.group < 0 || !prop.bounds) continue;
        if (x < prop.minX - 1.f || x > prop.maxX + 1.f || y < prop.minY - 1.f || y > prop.maxY + 1.f) continue;
        if (z + 1.8f < prop.minZ || z - 40.f > prop.maxZ) continue;
        const LitMesh& mesh = prop.mesh;
        const int propIndex = int(&prop - solid_.data());
        auto testSpan = [&](uint32_t begin, uint32_t count) {
            for (uint32_t n = 0; n < count; ++n) {
                const uint32_t tri = begin + n;
                float hz = 0;
                if (!triHeight(mesh, tri, hz)) continue;
                const float gapBefore = bestGap;
                consider(hz, best, bestGap, any);
                if (any && bestGap < gapBefore) {
                    floorHintProp_ = propIndex;
                    floorHintTri_ = tri;
                }
            }
        };
        if (prop.spans.empty()) testSpan(0, uint32_t(mesh.indices.size() / 3));
        else {
            for (const Prop::Span& span : prop.spans) {
                if (x < span.minX - 1.f || x > span.maxX + 1.f || y < span.minY - 1.f || y > span.maxY + 1.f) continue;
                if (z + 1.8f < span.minZ || z - 40.f > span.maxZ) continue;
                testSpan(span.begin, span.count);
            }
        }
    }
    // The building floor and the outdoor ground meet at a door. While a room floor is
    // under the feet, keep it. When the two surfaces are the same lip, either one is the step.
    const bool roomFloor = any && best <= z + 1.2f && best >= z - 2.5f;
    if (any && terrainAny && std::fabs(best - terrainBest) <= 0.45f)
        best = bestGap <= terrainGap ? best : terrainBest;
    else if (!roomFloor && terrainAny && (!any || terrainGap < bestGap)) {
        best = terrainBest;
        bestGap = terrainGap;
        any = true;
        floorHintProp_ = -1;
    }
    floorCacheX_ = x;
    floorCacheY_ = y;
    floorCacheSolids_ = int(solid_.size());
    floorCacheGrounds_ = int(grounds_.size()) + (terrainOk_ ? 1 : 0);
    floorCacheRoots_ = resolvedRoots;
    floorCacheIndoors_ = indoors_;
    floorCacheOk_ = any;
    floorCacheH_ = best;
    if (!any) return false;
    height = best;
    return true;
}

int WorldScene::prepareModel(ArchiveSet& archives, const std::string& path, bool loadIfMissing, const std::string& skin,
                             const std::string& hair, const CharacterLook* dressed, uint32_t gender) {
    if (path.empty()) return -1;
    std::string key = path;
    if (dressed) {
        key.push_back('\n');
        key += dressed->body;
        key.push_back('\n');
        key += dressed->hair;
        key.push_back('\n');
        key += dressed->cape;
        key.push_back('\n');
        key += std::to_string(dressed->gender);
        for (const SkinPaint& paint : dressed->paints) {
            key.push_back('\n');
            key += std::to_string(paint.section);
            key.push_back(':');
            key += paint.path;
        }
    } else if (!skin.empty() || !hair.empty()) {
        key += "\n" + skin + "\n" + hair + "\n" + std::to_string(gender);
    }
    const auto found = modelOf_.find(key);
    if (found != modelOf_.end()) return models_[found->second].failed ? -1 : int(found->second);
    if (!loadIfMissing) return -2;
    // One character file is read and decoded off the draw thread at a time. Reading it
    // here stalled the frame every time a new person walked into range.
    if (modelBusy_) {
        if (modelFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return -2;
        const int done = installModel();
        if (modelOf_.count(key)) return done;
    }
    if (loadedThisFrame_ || models_.size() >= 180) return -2;
    loadedThisFrame_ = true;
    auto build = std::make_shared<ModelBuild>();
    build->key = key;
    ArchiveSet* files = &archives;
    const CharacterLook look = dressed ? *dressed : CharacterLook{};
    const bool hasLook = dressed != nullptr;
    modelSlot_ = build;
    modelFuture_ = std::async(std::launch::async, [build, files, path, skin, hair, look, hasLook, gender]() {
        std::vector<uint8_t> bytes;
        std::string texture;
        if (!files->read(path, bytes) || !loadM2Mesh(bytes.data(), bytes.size(), build->mesh, texture)) return;
        auto resolve = [&](std::string file) {
            if (file.empty()) return file;
            for (char& c : file)
                if (c == '/') c = '\\';
            if (file.find('\\') == std::string::npos) {
                const size_t slash = path.find_last_of('\\');
                if (slash != std::string::npos) file = path.substr(0, slash + 1) + file;
            }
            if (file.size() >= 4) {
                const std::string ext = file.substr(file.size() - 4);
                if (ext != ".blp" && ext != ".BLP" && ext != ".m2" && ext != ".M2") file += ".blp";
            }
            return file;
        };
        const bool propTextures = !build->mesh.character && skin.empty() && hair.empty() && !hasLook && !build->mesh.textures.empty();
        const uint32_t sex = hasLook ? look.gender : gender;
        auto readImage = [&](const std::string& file, Image& image, std::string* name) {
            image = Image{};
            if (file.empty()) return false;
            for (const std::string& candidate : blpChoices(resolve(file), sex)) {
                std::vector<uint8_t> fileBytes;
                if (!files->read(candidate, fileBytes)) continue;
                if (decodeBlp(fileBytes.data(), fileBytes.size(), image) && image.width > 0) {
                    if (name) *name = candidate;
                    return true;
                }
            }
            return false;
        };
        auto loadTex = [&](const std::string& file, ModelBuild::Tex& tex) {
            tex.ok = readImage(file, tex.image, &tex.name);
            if (tex.ok) downsample(tex.image);
            return tex.ok;
        };
        const std::string bodyFile = hasLook && !look.body.empty() ? look.body : (skin.empty() ? texture : skin);
        const std::string hairFile = hasLook && !look.hair.empty() ? look.hair : hair;
        if (!propTextures && hasLook && !look.paints.empty()) {
            Image canvas;
            if (readImage(bodyFile, canvas, nullptr)) {
                for (const SkinPaint& paint : look.paints) {
                    Image patch;
                    if (readImage(paint.path, patch, nullptr)) paintSkin(canvas, patch, paint.section);
                }
                downsample(canvas);
                if (canvas.width > 0) {
                    build->body.image = std::move(canvas);
                    build->body.ok = true;
                }
            }
        }
        if (propTextures) {
            build->images.resize(build->mesh.textures.size());
            for (size_t i = 0; i < build->mesh.textures.size(); ++i)
                if (!build->mesh.textures[i].empty()) loadTex(build->mesh.textures[i], build->images[i]);
        } else {
            if (!build->body.ok && !loadTex(bodyFile, build->body) && !skin.empty()) loadTex(texture, build->body);
            loadTex(hairFile, build->hair);
            if (hasLook) loadTex(look.cape, build->cape);
        }
        build->ok = true;
    });
    modelBusy_ = true;
    return -2;
}

int WorldScene::installModel() {
    try {
        modelFuture_.get();
    } catch (...) {
    }
    modelBusy_ = false;
    std::shared_ptr<ModelBuild> build = std::move(modelSlot_);
    if (!build) return -1;
    ModelCache cache;
    cache.failed = !build->ok;
    if (build->ok) {
        cache.mesh = std::move(build->mesh);
        auto place = [&](ModelBuild::Tex& tex) {
            if (!tex.ok) return -1;
            if (!tex.name.empty()) {
                const auto known = imageOf_.find(tex.name);
                if (known != imageOf_.end() && image(known->second)) return known->second;
            }
            const int id = int(images_.size());
            if (!tex.name.empty()) imageOf_[tex.name] = id;
            images_.push_back(std::move(tex.image));
            return id;
        };
        if (!cache.mesh.character && !build->images.empty()) {
            std::vector<int> ids(build->images.size(), -1);
            int first = -1;
            for (size_t i = 0; i < build->images.size(); ++i) {
                ids[i] = place(build->images[i]);
                if (first < 0 && ids[i] >= 0) first = ids[i];
            }
            bool any = false;
            for (PrimBatch& batch : cache.mesh.batches) {
                if (batch.texture >= 0 && size_t(batch.texture) < ids.size() && ids[size_t(batch.texture)] >= 0) {
                    batch.texture = ids[size_t(batch.texture)];
                    any = true;
                } else batch.texture = -1;
            }
            if (!any && first >= 0)
                for (PrimBatch& batch : cache.mesh.batches) batch.texture = first;
            cache.image = first;
        } else {
            const int bodyId = place(build->body);
            const int hairId = place(build->hair);
            const int capeId = place(build->cape);
            for (PrimBatch& batch : cache.mesh.batches) {
                int id = bodyId;
                if (batch.slot == 1) id = hairId;
                else if (batch.slot == 2 && capeId >= 0) id = capeId;
                if (id >= 0) batch.texture = id;
            }
            cache.image = bodyId >= 0 ? bodyId : hairId;
        }
    }
    const bool failed = cache.failed;
    const int id = int(models_.size());
    modelOf_[build->key] = id;
    models_.push_back(std::move(cache));
    return failed ? -1 : id;
}

}  // namespace reclient
