#include "World.hpp"

#include "miniz.h"

#include <cstring>

namespace reclient {
namespace {
void readSpline(ByteBuffer& in) {
    const uint32_t flags = in.read<uint32_t>();
    constexpr uint32_t kFinalPoint = 0x00010000;
    constexpr uint32_t kFinalTarget = 0x00020000;
    constexpr uint32_t kFinalAngle = 0x00040000;
    if (flags & kFinalAngle) {
        (void)in.read<float>();
    } else if (flags & kFinalTarget) {
        (void)in.read<uint64_t>();
    } else if (flags & kFinalPoint) {
        (void)in.read<float>();
        (void)in.read<float>();
        (void)in.read<float>();
    }
    (void)in.read<uint32_t>();
    (void)in.read<uint32_t>();
    (void)in.read<uint32_t>();
    const uint32_t nodes = in.read<uint32_t>();
    if (nodes > 4096) throw PacketError("spline node count is not plausible");
    for (uint32_t i = 0; i < nodes; ++i) {
        (void)in.read<float>();
        (void)in.read<float>();
        (void)in.read<float>();
    }
    (void)in.read<float>();
    (void)in.read<float>();
    (void)in.read<float>();
}
}  // namespace

void writeMovement(ByteBuffer& out, const Movement& move) {
    out.write<uint32_t>(move.flags);
    out.write<uint8_t>(move.flags2);
    out.write<uint32_t>(move.time);
    out.write<float>(move.x);
    out.write<float>(move.y);
    out.write<float>(move.z);
    out.write<float>(move.o);
    if (move.flags & kMoveOnTransport) {
        out.write<uint64_t>(move.transport);
        out.write<float>(move.tx);
        out.write<float>(move.ty);
        out.write<float>(move.tz);
        out.write<float>(move.to);
        out.write<uint32_t>(move.transportTime);
    }
    if (move.flags & (kMoveSwimming | kMoveFlying2)) out.write<float>(move.pitch);
    out.write<uint32_t>(move.fallTime);
    if (move.flags & kMoveFalling) {
        out.write<float>(move.jumpZ);
        out.write<float>(move.jumpCos);
        out.write<float>(move.jumpSin);
        out.write<float>(move.jumpXy);
    }
    if (move.flags & kMoveSplineElevation) out.write<float>(move.splineElevation);
}

void readMovement(ByteBuffer& in, Movement& move) {
    move.flags = in.read<uint32_t>();
    move.flags2 = in.read<uint8_t>();
    move.time = in.read<uint32_t>();
    move.x = in.read<float>();
    move.y = in.read<float>();
    move.z = in.read<float>();
    move.o = in.read<float>();
    if (move.flags & kMoveOnTransport) {
        move.transport = in.read<uint64_t>();
        move.tx = in.read<float>();
        move.ty = in.read<float>();
        move.tz = in.read<float>();
        move.to = in.read<float>();
        move.transportTime = in.read<uint32_t>();
    }
    if (move.flags & (kMoveSwimming | kMoveFlying2)) move.pitch = in.read<float>();
    move.fallTime = in.read<uint32_t>();
    if (move.flags & kMoveFalling) {
        move.jumpZ = in.read<float>();
        move.jumpCos = in.read<float>();
        move.jumpSin = in.read<float>();
        move.jumpXy = in.read<float>();
    }
    if (move.flags & kMoveSplineElevation) move.splineElevation = in.read<float>();
}

void ObjectStore::readValues(ByteBuffer& in, WowObject& object) {
    const uint8_t blocks = in.read<uint8_t>();
    if (blocks > 64) throw PacketError("update mask is too large");
    const std::vector<uint8_t> mask = in.readVec(size_t(blocks) * 4);
    for (uint32_t bit = 0; bit < uint32_t(blocks) * 32; ++bit) {
        if ((mask[bit / 8] & (1u << (bit % 8))) == 0) continue;
        if (object.fields.size() <= bit) object.fields.resize(bit + 1, 0);
        object.fields[bit] = in.read<uint32_t>();
    }
}

void ObjectStore::readMovementBlock(ByteBuffer& in, Movement& move) {
    const uint8_t flags = in.read<uint8_t>();
    constexpr uint8_t kLiving = 0x20;
    constexpr uint8_t kHasPosition = 0x40;
    constexpr uint8_t kLow = 0x08;
    constexpr uint8_t kHigh = 0x10;
    constexpr uint8_t kTarget = 0x04;
    constexpr uint8_t kTransport = 0x02;
    if (flags & kLiving) {
        readMovement(in, move);
        move.walk = in.read<float>();
        move.run = in.read<float>();
        move.runBack = in.read<float>();
        move.swim = in.read<float>();
        move.swimBack = in.read<float>();
        move.flight = in.read<float>();
        move.flightBack = in.read<float>();
        move.turn = in.read<float>();
        if (move.flags & kMoveSplineEnabled) readSpline(in);
    } else if (flags & kHasPosition) {
        move.x = in.read<float>();
        move.y = in.read<float>();
        move.z = in.read<float>();
        move.o = in.read<float>();
    }
    if (flags & kLow) (void)in.read<uint32_t>();
    if (flags & kHigh) (void)in.read<uint32_t>();
    if (flags & kTarget) (void)in.readPackedGuid();
    if (flags & kTransport) (void)in.read<uint32_t>();
}

bool ObjectStore::applyUpdate(const uint8_t* data, size_t len, std::string& error) {
    try {
        ByteBuffer in(std::vector<uint8_t>(data, data + len));
        const uint32_t count = in.read<uint32_t>();
        (void)in.read<uint8_t>();
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t kind = in.read<uint8_t>();
            if (kind == kUpdateOutOfRange) {
                const uint32_t n = in.read<uint32_t>();
                for (uint32_t g = 0; g < n; ++g) {
                    const uint64_t guid = in.readPackedGuid();
                    objects_.erase(guid);
                    removed.push_back(guid);
                }
                continue;
            }
            const uint64_t guid = in.readPackedGuid();
            WowObject& object = objects_[guid];
            object.guid = guid;
            if (kind == kUpdateCreate || kind == kUpdateCreate2) {
                object.typeId = in.read<uint8_t>();
                readMovementBlock(in, object.move);
                readValues(in, object);
            } else if (kind == kUpdateValues) {
                readValues(in, object);
            } else if (kind == kUpdateMovement) {
                readMovementBlock(in, object.move);
            } else {
                throw PacketError("unknown update block " + std::to_string(kind));
            }
        }
        return true;
    } catch (const std::exception& ex) {
        error = ex.what();
        return false;
    }
}

void ObjectStore::destroy(uint64_t guid) { objects_.erase(guid); }

void ObjectStore::rename(uint64_t guid, const std::string& name) {
    if (WowObject* object = find(guid)) object->name = name;
}

void ObjectStore::renameEntry(uint8_t typeId, uint32_t entry, const std::string& name) {
    for (auto& [guid, object] : objects_) {
        (void)guid;
        if (object.typeId == typeId && object.entry() == entry) object.name = name;
    }
}

WowObject* ObjectStore::find(uint64_t guid) {
    const auto it = objects_.find(guid);
    return it == objects_.end() ? nullptr : &it->second;
}

std::vector<Character> parseCharEnum(ByteBuffer& in) {
    std::vector<Character> out;
    const uint8_t count = in.read<uint8_t>();
    out.reserve(count);
    for (uint8_t i = 0; i < count; ++i) {
        Character c;
        c.guid = in.read<uint64_t>();
        c.name = in.readCString();
        c.race = in.read<uint8_t>();
        c.klass = in.read<uint8_t>();
        c.gender = in.read<uint8_t>();
        (void)in.read<uint8_t>();
        (void)in.read<uint8_t>();
        (void)in.read<uint8_t>();
        (void)in.read<uint8_t>();
        (void)in.read<uint8_t>();
        c.level = in.read<uint8_t>();
        c.zone = in.read<uint32_t>();
        c.map = in.read<uint32_t>();
        c.x = in.read<float>();
        c.y = in.read<float>();
        c.z = in.read<float>();
        (void)in.read<uint32_t>();
        (void)in.read<uint32_t>();
        (void)in.read<uint8_t>();
        (void)in.read<uint32_t>();
        (void)in.read<uint32_t>();
        (void)in.read<uint32_t>();
        for (int slot = 0; slot < 20; ++slot) {
            (void)in.read<uint32_t>();
            (void)in.read<uint8_t>();
            (void)in.read<uint32_t>();
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Realm> parseRealmList(ByteBuffer& in) {
    (void)in.read<uint32_t>();
    const uint16_t count = in.read<uint16_t>();
    std::vector<Realm> realms;
    realms.reserve(count);
    for (uint16_t i = 0; i < count; ++i) {
        Realm realm;
        realm.icon = in.read<uint8_t>();
        realm.lock = in.read<uint8_t>();
        realm.flags = in.read<uint8_t>();
        realm.name = in.readCString();
        realm.address = in.readCString();
        realm.population = in.read<float>();
        realm.characters = in.read<uint8_t>();
        realm.category = in.read<uint8_t>();
        (void)in.read<uint8_t>();
        if (realm.flags & 0x01) {
            (void)in.read<uint8_t>();
            (void)in.read<uint8_t>();
            (void)in.read<uint8_t>();
            (void)in.read<uint16_t>();
        }
        realms.push_back(std::move(realm));
    }
    return realms;
}

ChatLine parseChat(ByteBuffer& in) {
    ChatLine line;
    line.type = in.read<uint8_t>();
    line.language = in.read<uint32_t>();
    line.senderGuid = in.read<uint64_t>();
    (void)in.read<uint32_t>();
    const bool monster = line.type == 12 || line.type == 13 || line.type == 14 || line.type == 15 ||
                         line.type == 16 || line.type == 41 || line.type == 42 || line.type == 43;
    if (monster) {
        const uint32_t nameLen = in.read<uint32_t>();
        if (nameLen > 0 && nameLen < 256) {
            std::string raw(nameLen, '\0');
            in.readBytes(reinterpret_cast<uint8_t*>(raw.data()), nameLen);
            if (!raw.empty() && raw.back() == 0) raw.pop_back();
            line.sender = std::move(raw);
        }
        (void)in.read<uint64_t>();
    } else if (line.type == 17) {
        line.channel = in.readCString();
    }
    if (!monster) (void)in.read<uint64_t>();
    const uint32_t textLen = in.read<uint32_t>();
    if (textLen > 0 && textLen < 4096) {
        std::string raw(textLen, '\0');
        in.readBytes(reinterpret_cast<uint8_t*>(raw.data()), textLen);
        if (!raw.empty() && raw.back() == 0) raw.pop_back();
        line.text = std::move(raw);
    }
    return line;
}

std::vector<uint8_t> inflateZlib(const uint8_t* data, size_t len, size_t uncompressed) {
    std::vector<uint8_t> out(uncompressed);
    mz_ulong dest = mz_ulong(uncompressed);
    const int rc = mz_uncompress(out.data(), &dest, data, mz_ulong(len));
    if (rc != MZ_OK) throw PacketError("zlib inflate failed");
    out.resize(dest);
    return out;
}

std::vector<uint8_t> buildAddonInfo() {
    ByteBuffer plain;
    const char* addons[] = {"Blizzard_BindingUI", "Blizzard_AuctionUI", "Blizzard_BattlefieldMinimap",
                            "Blizzard_CombatText", "Blizzard_CraftUI", "Blizzard_GMSurveyUI",
                            "Blizzard_InspectUI", "Blizzard_MacroUI", "Blizzard_RaidUI", "Blizzard_TalentUI",
                            "Blizzard_TradeSkillUI", "Blizzard_TrainerUI"};
    for (const char* name : addons) {
        plain.writeCString(name);
        plain.write<uint8_t>(0);
        plain.write<uint32_t>(0);
        plain.write<uint32_t>(0);
    }
    mz_ulong bound = mz_compressBound(mz_ulong(plain.size()));
    std::vector<uint8_t> compressed(bound);
    const int rc = mz_compress(compressed.data(), &bound, plain.data(), mz_ulong(plain.size()));
    if (rc != MZ_OK) throw PacketError("zlib deflate failed");
    compressed.resize(bound);
    ByteBuffer packet;
    packet.write<uint32_t>(uint32_t(plain.size()));
    packet.append(compressed);
    return packet.raw();
}

}  // namespace reclient
