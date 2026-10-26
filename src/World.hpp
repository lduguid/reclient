#pragma once

#include "ByteBuffer.hpp"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace reclient {

constexpr uint32_t kMoveForward = 0x00000001;
constexpr uint32_t kMoveWalk = 0x00000100;
constexpr uint32_t kMoveOnTransport = 0x00000200;
constexpr uint32_t kMoveFalling = 0x00001000;
constexpr uint32_t kMoveJumping = 0x00002000;
constexpr uint32_t kMoveSwimming = 0x00200000;
constexpr uint32_t kMoveFlying2 = 0x02000000;
constexpr uint32_t kMoveSplineElevation = 0x04000000;
constexpr uint32_t kMoveSplineEnabled = 0x08000000;

constexpr uint8_t kUpdateValues = 0;
constexpr uint8_t kUpdateMovement = 1;
constexpr uint8_t kUpdateCreate = 2;
constexpr uint8_t kUpdateCreate2 = 3;
constexpr uint8_t kUpdateOutOfRange = 4;

constexpr uint32_t kFieldEntry = 0x0003;
constexpr uint32_t kFieldHealth = 0x0016;
constexpr uint32_t kFieldLevel = 0x0022;
constexpr uint32_t kFieldDisplayId = 0x0098;
constexpr uint32_t kFieldNpcFlags = 0x00A8;
constexpr uint32_t kFieldBytes0 = 0x0024;
constexpr uint32_t kFieldPlayerBytes = 0x00EF;
constexpr uint32_t kFieldPlayerBytes2 = 0x00F0;
// First visible equipment entry (head). Nineteen slots, sixteen fields apart.
constexpr uint32_t kFieldVisibleItem = 0x015A;
constexpr uint32_t kVisibleItemStride = 0x0010;

struct Movement {
    uint32_t flags = 0;
    uint8_t flags2 = 0;
    uint32_t time = 0;
    float x = 0, y = 0, z = 0, o = 0;
    uint32_t fallTime = 0;
    uint64_t transport = 0;
    float tx = 0, ty = 0, tz = 0, to = 0;
    uint32_t transportTime = 0;
    float pitch = 0;
    float jumpZ = 0, jumpCos = 0, jumpSin = 0, jumpXy = 0;
    float splineElevation = 0;
    float walk = 2.5f, run = 7.0f, runBack = 4.5f, swim = 4.7222f, swimBack = 2.5f;
    float flight = 7.0f, flightBack = 4.5f, turn = 3.14159f;
    // A path the server asked this unit to walk. The renderer advances it.
    uint32_t splineId = 0;
    uint32_t splineFlags = 0;
    uint32_t splineMs = 0;
    float splineX = 0, splineY = 0, splineZ = 0;
    bool splineLive = false;
};

struct WowObject {
    uint64_t guid = 0;
    uint8_t typeId = 0;
    Movement move;
    std::vector<uint32_t> fields;
    std::string name;
    uint32_t entry() const { return field(kFieldEntry); }
    uint32_t health() const { return field(kFieldHealth); }
    uint32_t level() const { return field(kFieldLevel); }
    uint32_t displayId() const { return field(kFieldDisplayId); }
    uint32_t npcFlags() const { return field(kFieldNpcFlags); }

private:
    uint32_t field(uint32_t index) const { return index < fields.size() ? fields[index] : 0; }
};

struct Character {
    uint64_t guid = 0;
    std::string name;
    uint8_t race = 0, klass = 0, gender = 0, level = 0;
    uint32_t zone = 0, map = 0;
    float x = 0, y = 0, z = 0;
};

struct ChatLine {
    uint8_t type = 0;
    uint32_t language = 0;
    uint32_t seq = 0;
    uint64_t senderGuid = 0;
    std::string sender;
    std::string text;
    std::string channel;
};

struct GossipChoice {
    uint32_t id = 0;
    std::string text;
    std::string kind = "gossip";
    bool coded = false;
};

struct GossipMenu {
    uint64_t guid = 0;
    uint32_t menu = 0;
    uint32_t textId = 0;
    uint32_t serial = 0;
    std::string text;
    std::vector<GossipChoice> choices;
    bool open = false;
};

struct Realm {
    uint8_t icon = 0, lock = 0, flags = 0;
    std::string name;
    std::string address;
    float population = 0;
    uint8_t characters = 0;
    uint8_t category = 0;
};

void writeMovement(ByteBuffer& out, const Movement& move);
void readMovement(ByteBuffer& in, Movement& move);

class ObjectStore {
public:
    // Applies one SMSG_UPDATE_OBJECT body. Returns false if a block could not be parsed.
    bool applyUpdate(const uint8_t* data, size_t len, std::string& error);
    void destroy(uint64_t guid);
    void rename(uint64_t guid, const std::string& name);
    void renameEntry(uint8_t typeId, uint32_t entry, const std::string& name);
    WowObject* find(uint64_t guid);
    const std::unordered_map<uint64_t, WowObject>& all() const { return objects_; }
    std::vector<uint64_t> removed;

private:
    void readValues(ByteBuffer& in, WowObject& object);
    void readMovementBlock(ByteBuffer& in, Movement& move);
    std::unordered_map<uint64_t, WowObject> objects_;
};

std::vector<Character> parseCharEnum(ByteBuffer& in);
std::vector<Realm> parseRealmList(ByteBuffer& in);
ChatLine parseChat(ByteBuffer& in);
std::vector<uint8_t> buildAddonInfo();
std::vector<uint8_t> inflateZlib(const uint8_t* data, size_t len, size_t uncompressed);

}  // namespace reclient
