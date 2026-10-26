#pragma once

#include "Formats.hpp"

#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace reclient {

class ArchiveSet;

struct Camera {
    float x = 0, y = 0, z = 0, o = 0, pitch = 0;
};

// Wow.exe 2.4.3 registers these video CVars with these default strings.
constexpr float kNearClip = 0.1f;         // nearclip
constexpr float kFarClip = 350.f;         // farclip
constexpr float kHorizonFarClip = 2112.f; // horizonfarclip
constexpr float kDistCull = 500.f;        // DistCull, "Object distance culling"

struct ColorBuffer {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::vector<float> depth;
    void resize(int w, int h);
    void clear(uint8_t r, uint8_t g, uint8_t b);
    bool occluded(float sx, float sy, float ahead) const;
};

bool projectPoint(const Camera& cam, float x, float y, float z, int width, int height, float& sx, float& sy, float& ahead);

// Streams the map tile around the camera: the city model, nearby doodads, and terrain.
class WorldScene {
public:
    ~WorldScene();
    bool advance(ArchiveSet& archives, uint32_t map, float x, float y, float z);
    // How far terrain tiles are kept. Yards, from the client's farclip.
    void setFarClip(float yards) { viewFar_ = yards > 1.f ? yards : 1.f; }
    void flushLoad();
    void raster(ColorBuffer& buffer, const Camera& cam) const;
    void rasterModel(ColorBuffer& buffer, const Camera& cam, int model, const Mat4& world) const;
    // Loads one model file when it is not cached. -1 failed, -2 try again next frame.
    // skin and hair replace the body and hair images. A dressed look composites the body.
    int prepareModel(ArchiveSet& archives, const std::string& path, bool loadIfMissing = true,
                     const std::string& skin = {}, const std::string& hair = {}, const CharacterLook* dressed = nullptr,
                     uint32_t gender = 0);
    bool indoors() const { return indoors_; }
    // The area the listener is in. Indoors, the containing WMO group; outdoors, the terrain chunk.
    struct SoundSpot {
        bool indoors = false;
        int32_t wmoRoot = 0;
        int32_t wmoGroup = -1;
        int32_t wmoIndex = -1;
        uint32_t chunk = 0;
    };
    SoundSpot soundSpot(ArchiveSet& files);
    void beginFrame() { loadedThisFrame_ = false; }
    bool loadedThisFrame() const { return loadedThisFrame_; }
    int groups() const { return int(solid_.size()); }
    std::string status() const;
    bool showTerrain() const;
    const Terrain& terrain() const { return terrain_; }
    void terrains(std::vector<const Terrain*>& out) const;
    // Closest walkable surface under the feet. A room floor is kept until it meets the outdoor ground.
    bool floorAt(float x, float y, float z, float& height) const;
    // Pull an orbit in so the eye stays in front of a wall or floor. (dx,dy,dz) is a unit
    // vector from the look point toward the eye, and dist is the unobstructed length.
    float clipCamera(float x, float y, float z, float dx, float dy, float dz, float dist) const;
    // Distance along a horizontal step before a wall. Floors and ceilings are not walls.
    float wallAhead(float x, float y, float z, float dx, float dy, float dist) const;

    struct Drawable {
        const LitMesh* mesh = nullptr;
        Mat4 world;
    };
    void drawables(std::vector<Drawable>& out) const;
    LitMesh* modelMesh(int index);
    const LitMesh* modelMesh(int index) const;
    const Image* image(int index) const;
    int picture(ArchiveSet& archives, const std::string& path) { return imageId(archives, path); }

private:
    struct Prop {
        LitMesh mesh;
        Mat4 world;
        int image = -1;
        int placement = -1;
        int group = -1;
        float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
        bool bounds = false;
        struct Span {
            uint32_t begin = 0;
            uint32_t count = 0;
            float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
        };
        std::vector<Span> spans;
    };
    struct GroundTile {
        int x = -1;
        int y = -1;
        Terrain mesh;
    };
    struct ModelCache {
        LitMesh mesh;
        int image = -1;
        bool failed = false;
    };

    uint32_t map_ = 0xffffffffu;
    int tileX_ = -1;
    int tileY_ = -1;
    float viewerX_ = 0, viewerY_ = 0, viewerZ_ = 0;
    float viewFar_ = kFarClip;
    bool indoors_ = false;
    bool loadedThisFrame_ = false;
    bool jobsReady_ = false;
    float builtX_ = 0, builtY_ = 0;
    Terrain terrain_;
    bool terrainOk_ = false;
    std::vector<GroundTile> grounds_;
    std::unordered_set<int> groundMiss_;
    std::vector<MapPlacement> placements_;
    std::vector<Prop> solid_;
    std::unordered_set<uint32_t> loadedGroups_;
    std::vector<int> pendingDoodads_;
    std::unordered_set<int> spawnedDoodads_;
    float doodadScanX_ = 1.0e9f, doodadScanY_ = 1.0e9f;
    mutable float clipX_ = 0, clipY_ = 0, clipZ_ = 0, clipDx_ = 0, clipDy_ = 0, clipDz_ = 0, clipDist_ = 0, clipOut_ = 0;
    mutable int clipSolids_ = -1;
    mutable bool clipIndoors_ = false, clipValid_ = false;
    bool indoorsBuilt_ = false;
    mutable int floorHintProp_ = -1;
    mutable uint32_t floorHintTri_ = 0;
    mutable float floorCacheX_ = 1.0e9f, floorCacheY_ = 0, floorCacheH_ = 0;
    mutable int floorCacheSolids_ = -1, floorCacheGrounds_ = -1, floorCacheRoots_ = -1;
    mutable bool floorCacheIndoors_ = false, floorCacheOk_ = false;
    void pruneFar(float x, float y, float z);
    void dressTerrain(ArchiveSet& archives, Terrain& tile);
    void refreshIndoors();
    void queueDoodads(float x, float y, float z);
    bool readGroundTile(ArchiveSet& archives, int tx, int ty);
    struct Job {
        int placement = 0;
        int group = 0;
        float distance = 0;
    };
    struct GroupBuild;
    std::shared_ptr<GroupBuild> groupSlot_;
    std::future<void> groupFuture_;
    bool groupBusy_ = false;
    uint32_t loadGeneration_ = 0;
    void installGroup();
    struct ModelBuild {
        struct Tex {
            std::string name;
            Image image;
            bool ok = false;
        };
        std::string key;
        LitMesh mesh;
        Tex body, hair, cape;
        // Parallel to LitMesh::textures for a static model. Empty for a character or an item skin.
        std::vector<Tex> images;
        bool ok = false;
    };
    std::shared_ptr<ModelBuild> modelSlot_;
    std::future<void> modelFuture_;
    bool modelBusy_ = false;
    int installModel();
    std::vector<Job> jobs_;
    std::vector<WmoRoot> roots_;
    std::vector<char> rootState_;
    std::vector<Image> images_;
    std::unordered_map<std::string, int> imageOf_;
    std::vector<ModelCache> models_;
    std::unordered_map<std::string, int> modelOf_;
    int imageId(ArchiveSet& archives, const std::string& path);
};

}  // namespace reclient
