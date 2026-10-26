#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace reclient {

struct Dbc {
    uint32_t records = 0;
    uint32_t fields = 0;
    uint32_t recordSize = 0;
    bool load(const uint8_t* data, size_t len);
    uint32_t get(uint32_t row, uint32_t field) const;
    std::string text(uint32_t offset) const;

    std::vector<uint8_t> bytes;
    size_t stringOffset = 0;
};

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
};

bool decodeBlp(const uint8_t* data, size_t len, Image& out);

struct Mesh {
    std::vector<float> xyz;
    std::vector<float> uv;
    std::vector<uint32_t> indices;
};

bool loadM2(const uint8_t* data, size_t len, Mesh& out);

struct MapInfo {
    bool present[64][64]{};
    std::vector<std::string> models;
};

bool loadWdt(const uint8_t* data, size_t len, MapInfo& out);

struct Terrain {
    std::vector<float> xyz;
    std::vector<float> uv;
    std::vector<uint8_t> shade;
    std::vector<uint32_t> indices;
    // One ground texture and the triangles that use it. image is filled when the tile is dressed.
    struct Surface {
        std::string path;
        int image = -1;
        std::vector<uint32_t> indices;
        // One map chunk's triangles inside indices, so a chunk outside the view can be skipped.
        struct Patch {
            uint32_t first = 0;
            uint32_t count = 0;
            float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
        };
        std::vector<Patch> patches;
    };
    std::vector<Surface> surfaces;
    // A blended coat over a chunk. alpha lines up with vertices [vertBegin, vertBegin + alpha.size()).
    struct Coat {
        std::string path;
        int image = -1;
        uint32_t vertBegin = 0;
        std::vector<uint32_t> indices;
        std::vector<uint8_t> alpha;
        float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    };
    std::vector<Coat> coats;
    // Absolute height of each of the 145 MCVT vertices, 16*16 chunks. Missing chunks are NaN.
    std::vector<float> height;
    // Low 16 bits of each chunk: a 4 by 4 hole map. A set bit is ground the map leaves open.
    std::vector<uint32_t> holes;
    // AreaTable id of each of the 256 chunks. 0 when that chunk was not in the tile.
    std::vector<uint32_t> area;
    int tileX = -1;
    int tileY = -1;
    float minX = 0, minY = 0, maxX = 0, maxY = 0, minZ = 0, maxZ = 0;
};

// tileX/tileY are the numbers in MapName_tileX_tileY.adt.
bool loadAdt(const uint8_t* data, size_t len, int tileX, int tileY, Terrain& out);
// Height of the terrain surface at a world position. False when the tile has no ground there.
bool terrainHeight(const Terrain& ground, float x, float y, float& z);
// AreaTable id of the chunk under a world position. 0 when this tile does not cover it.
uint32_t terrainArea(const Terrain& ground, float x, float y);

bool loadWmo(const uint8_t* data, size_t len, Mesh& out);
int wmoGroupCount(const uint8_t* data, size_t len);

// Column-major. Maps a model vertex into server coordinates (x north, y west, z up).
struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

Mat4 mapPlacement(float px, float py, float pz, float rx, float ry, float rz, float scale);
Mat4 unitPlacement(float x, float y, float z, float facing, float scale);
void transformPoint(const Mat4& m, float x, float y, float z, float& ox, float& oy, float& oz);

struct MapPlacement {
    std::string path;
    bool wmo = false;
    float scale = 1.f;
    Mat4 world;
    float minX = 0, minY = 0, minZ = 0, maxX = 0, maxY = 0, maxZ = 0;
    bool hasBox = false;
};

bool loadAdtPlacements(const uint8_t* data, size_t len, std::vector<MapPlacement>& out);

struct WmoMaterial {
    std::string texture;
    bool twoSided = false;
};

struct WmoGroupBox {
    float min[3]{};
    float max[3]{};
    uint32_t flags = 0;
    int32_t groupId = -1;
};

struct WmoRoot {
    uint32_t wmoId = 0;
    std::vector<WmoMaterial> materials;
    std::vector<WmoGroupBox> groups;
};

// WMOAreaTable group id stored in the group file. -1 when the file has none.
int32_t wmoGroupFileId(const uint8_t* data, size_t len);

bool loadWmoRoot(const uint8_t* data, size_t len, WmoRoot& out);

struct PrimBatch {
    uint32_t start = 0;
    uint32_t count = 0;
    int texture = -1;
    bool twoSided = false;
    uint16_t geoset = 0;
    uint8_t slot = 0;  // 0 body or creature, 1 hair, 2 cape or item
};

struct BoneTrack {
    std::vector<uint32_t> times;
    std::vector<float> values;
    uint32_t loopMs = 0;
};

struct ModelBone {
    int16_t parent = -1;
    float pivot[3]{};
    float bindT[3]{};
    float bindR[4]{0, 0, 0, 1};
    float bindS[3]{1, 1, 1};
    BoneTrack standT, standR, standS;
    BoneTrack walkT, walkR, walkS;
    BoneTrack runT, runR, runS;
    BoneTrack backT, backR, backS;
};

struct ModelAttachment {
    uint32_t id = 0;
    uint16_t bone = 0;
    float x = 0, y = 0, z = 0;
};

// Vertices are model space unless a placement was baked in by the loader.
struct LitMesh {
    std::vector<float> xyz;
    std::vector<float> uv;
    std::vector<uint8_t> rgb;
    std::vector<uint32_t> indices;
    std::vector<PrimBatch> batches;
    // Texture-definition order. A static model's batch.texture indexes this until the scene binds images.
    std::vector<std::string> textures;
    std::vector<float> rest;
    std::vector<uint8_t> weights;
    std::vector<uint8_t> joints;
    std::vector<ModelBone> bones;
    std::vector<float> boneMats;
    std::vector<ModelAttachment> attachments;
    std::vector<int16_t> attachmentOf;
    uint32_t standMs = 0;
    uint32_t walkMs = 0;
    uint32_t runMs = 0;
    uint32_t backMs = 0;
    float walkGround = 0;
    float runGround = 0;
    float backGround = 0;
    uint32_t posedAt = 0;
    uint8_t posedMoving = 0xff;
    bool character = false;
};

bool loadWmoGroupMesh(const uint8_t* data, size_t len, const Mat4& world, const std::vector<WmoMaterial>& materials,
                      LitMesh& out);
bool loadM2Mesh(const uint8_t* data, size_t len, LitMesh& out, std::string& texture);
// Writes one locomotion pose into xyz. 0 stand, 1 walk, 2 run, 3 backward.
void poseModel(LitMesh& mesh, uint32_t timeMs, uint8_t gait);
// along is signed speed in the direction the model faces. Positive is forward.
uint8_t movementGait(bool moving, float along, uint32_t moveFlags, uint32_t splineFlags, bool onSpline, float speed,
                     float walkSpeed, float runSpeed);
// Item-local to world: the attachment bone's current pose, then the body's placement.
bool wornTransform(const Mat4& placed, const LitMesh& body, uint32_t attachment, Mat4& out);

struct SkinPaint {
    int section = 0;
    int priority = 0;
    int order = 0;
    std::string path;
};

// One worn equipment slot. Component names are bare filenames or full paths.
// Geoset values are the small group numbers from the item display record.
struct EquipVisual {
    int slot = -1;
    uint32_t geoset[3]{};
    std::string component[8];
    std::string cape;
    std::string modelLeft;
    std::string modelRight;
    std::string textureLeft;
    std::string textureRight;
};

struct CharacterLook {
    std::string body;
    std::string hair;
    std::string cape;
    std::string face[3];
    std::string facial[3];
    std::string hairLayer[3];
    uint32_t gender = 0;
    std::vector<SkinPaint> paints;
    std::vector<uint16_t> geosets;
    bool ok = false;
};

// Naked body, hair card, face layers, and geosets for one appearance.
bool characterLook(const Dbc& sections, const Dbc& hairGeosets, const Dbc& facial, uint32_t race, uint32_t gender,
                   uint32_t skin, uint32_t face, uint32_t hairStyle, uint32_t hairColor, uint32_t facialHair,
                   CharacterLook& out);
// Paints worn item sections over a naked look and swaps the clothed geosets.
void dressCharacter(const EquipVisual* worn, int count, CharacterLook& look);
// Scales one texture into a body-skin section and composites it over the canvas.
void paintSkin(Image& canvas, const Image& patch, int section);

// Map-file props have no spawn record. A filename that is only a festival decoration
// follows the local calendar. Server creatures and game objects do not: the world
// already applied the event, pool, quest, reputation, and respawn checks before sending them.
bool seasonalHidden(const std::string& name);

class ArchiveSet;

// One outdoor light sample. Colors are RGB from LightIntBand (rows 0 direct,
// 1 ambient, 2-6 sky top through horizon, 7 fog). Fog distances are yards.
struct OutdoorLight {
    uint8_t direct[3]{};
    uint8_t ambient[3]{};
    uint8_t sky[5][3]{};
    uint8_t fog[3]{};
    float fogStart = 0;
    float fogEnd = 0;
    bool ok = false;
};

// Light.dbc plus the time bands. halfMinute is the client's 0–2880 clock.
class LightTable {
public:
    struct Band {
        uint32_t count = 0;
        uint32_t time[16]{};
        uint32_t data[16]{};
    };
    bool load(ArchiveSet& archives);
    bool ready() const { return ready_; }
    OutdoorLight at(uint32_t map, float x, float y, float z, int halfMinute) const;

private:
    struct Place {
        uint32_t map = 0;
        float x = 0, y = 0, z = 0;
        float falloffStart = 0, falloffEnd = 0;
        uint32_t param = 0;
    };
    bool ready_ = false;
    std::vector<Place> places_;
    std::unordered_map<uint32_t, Band> colors_;
    std::unordered_map<uint32_t, Band> floats_;
};

}  // namespace reclient
