#include "Formats.hpp"
#include "Mpq.hpp"
#include "Sound.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

int main() {
    {
        const reclient::Mat4 upright = reclient::unitPlacement(0, 0, 5, 0, 1);
        float x = 0, y = 0, z = 0;
        reclient::transformPoint(upright, 0, 0, 1, x, y, z);
        std::printf("unit up %.2f %.2f %.2f\n", x, y, z);
        if (std::fabs(x) > 0.05f || std::fabs(y) > 0.05f || std::fabs(z - 6.f) > 0.05f) {
            std::printf("unit model is not standing up\n");
            return 1;
        }
        reclient::transformPoint(upright, 1, 0, 0, x, y, z);
        std::printf("unit forward %.2f %.2f %.2f\n", x, y, z);
        if (std::fabs(x - 1.f) > 0.05f || std::fabs(y) > 0.05f || std::fabs(z - 5.f) > 0.05f) {
            std::printf("unit facing 0 is not north\n");
            return 1;
        }
        const bool pumpkin = reclient::seasonalHidden("G.Pumpkin");
        const bool brewfest = reclient::seasonalHidden("BrewfestKeg");
        std::printf("calendar pumpkin %s brewfest %s\n", pumpkin ? "hidden" : "shown", brewfest ? "hidden" : "shown");
        if (!brewfest || pumpkin) {
            std::printf("calendar window does not match 24 Oct (Hallow's End on, Brewfest off)\n");
            return 1;
        }
    }
    reclient::ArchiveSet archives(R"(D:\home\World of Warcraft)");
    std::vector<uint8_t> bytes;
    if (!archives.read("DBFilesClient\\Map.dbc", bytes)) {
        std::printf("miss Map.dbc\n");
        return 1;
    }
    reclient::Dbc maps;
    if (!maps.load(bytes.data(), bytes.size())) return 1;
    std::printf("Map.dbc rows %u fields %u dir0 %s\n", maps.records, maps.fields, maps.text(maps.get(0, 1)).c_str());

    {
        reclient::Dbc models;
        if (archives.read("DBFilesClient\\CreatureModelData.dbc", bytes) && models.load(bytes.data(), bytes.size())) {
            int shown = 0;
            for (uint32_t row = 0; row < models.records && shown < 2; ++row) {
                std::string path = models.text(models.get(row, 2));
                for (char& c : path)
                    if (c == '/') c = '\\';
                if (path.find("DwarfMale") == std::string::npos && path.find("HumanMale") == std::string::npos) continue;
                if (path.size() >= 4 && (path.substr(path.size() - 4) == ".mdx" || path.substr(path.size() - 4) == ".MDX"))
                    path.replace(path.size() - 4, 4, ".m2");
                std::vector<uint8_t> modelBytes;
                reclient::LitMesh mesh;
                std::string texture;
                const bool ok = archives.read(path, modelBytes) &&
                                reclient::loadM2Mesh(modelBytes.data(), modelBytes.size(), mesh, texture);
                std::printf("creature %s ok %d tris %zu texture %s bytes %zu\n", path.c_str(), ok ? 1 : 0,
                            mesh.indices.size() / 3, texture.c_str(), modelBytes.size());
                if (ok && path.find("DwarfMale") != std::string::npos) {
                    reclient::poseModel(mesh, 0, 0);
                    reclient::Mat4 ident;
                    reclient::Mat4 left;
                    const bool hung = reclient::wornTransform(ident, mesh, 6, left);
                    std::printf("dwarf left shoulder z %.2f bones %zu\n", left.m[14], mesh.boneMats.size() / 16);
                    if (!hung || left.m[14] < 0.5f || left.m[14] > 2.5f || mesh.boneMats.size() != mesh.bones.size() * 16) {
                        std::printf("shoulder attachment is missing\n");
                        return 1;
                    }
                }
                if (ok && path.find("HumanMale") != std::string::npos) {
                    auto farthest = [&]() {
                        float worst = 0;
                        const size_t n = std::min(mesh.rest.size(), mesh.xyz.size());
                        for (size_t i = 0; i + 2 < n; i += 3) {
                            const float dx = mesh.xyz[i] - mesh.rest[i];
                            const float dy = mesh.xyz[i + 1] - mesh.rest[i + 1];
                            const float dz = mesh.xyz[i + 2] - mesh.rest[i + 2];
                            worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
                        }
                        return worst;
                    };
                    reclient::poseModel(mesh, 0, 0);
                    const float stand = farthest();
                    reclient::poseModel(mesh, 200, 2);
                    const float run = farthest();
                    int hairBatches = 0, bodyBatches = 0;
                    uint8_t maxJoint = 0;
                    for (const reclient::PrimBatch& batch : mesh.batches) {
                        if (batch.slot == 1) ++hairBatches;
                        if (batch.geoset == 0) ++bodyBatches;
                    }
                    for (size_t i = 0; i < mesh.joints.size() && i < mesh.weights.size(); ++i)
                        if (mesh.weights[i] && mesh.joints[i] > maxJoint) maxJoint = mesh.joints[i];
                    const uint8_t slow = reclient::movementGait(true, 2.5f, 0, 0x100, true, 2.5f, 2.5f, 7.f);
                    const uint8_t fast = reclient::movementGait(true, 7.f, 0, 0x100, true, 7.f, 2.5f, 7.f);
                    const uint8_t back = reclient::movementGait(true, -4.5f, 0, 0, false, 4.5f, 2.5f, 7.f);
                    const uint8_t idle = reclient::movementGait(false, 0.f, 0, 0, false, 0.f, 2.5f, 7.f);
                    std::printf("pose stand %.3f run %.3f bones %zu span %u/%u/%u ground %.2f/%.2f character %d hair %d body %d joint %u\n",
                                stand, run, mesh.bones.size(), mesh.standMs, mesh.walkMs, mesh.runMs, mesh.walkGround, mesh.runGround,
                                mesh.character ? 1 : 0, hairBatches, bodyBatches, maxJoint);
                    if (mesh.walkMs == 0 || mesh.runGround < 5.f || slow != 1 || fast != 2 || back != 3 || idle != 0) {
                        std::printf("walk gait is wrong\n");
                        return 1;
                    }
                    if (stand > 1.f || run < 0.02f || run > 3.f || mesh.bones.empty() || !mesh.character || hairBatches == 0 ||
                        bodyBatches == 0 || maxJoint >= mesh.bones.size()) {
                        std::printf("character mesh is not skinned\n");
                        return 1;
                    }
                }
                ++shown;
            }
            std::vector<uint8_t> padBytes;
            reclient::LitMesh pad;
            std::string padTex;
            const bool padOk = archives.read(R"(Item\ObjectComponents\Shoulder\LShoulder_Leather_A_01.m2)", padBytes) &&
                               reclient::loadM2Mesh(padBytes.data(), padBytes.size(), pad, padTex);
            std::printf("shoulder mesh ok %d tris %zu\n", padOk ? 1 : 0, pad.indices.size() / 3);
            if (!padOk || pad.indices.size() < 3) {
                std::printf("shoulder mesh did not load\n");
                return 1;
            }
        }
        {
            reclient::Dbc sections, hairs, facials;
            const bool loaded = archives.read("DBFilesClient\\CharSections.dbc", bytes) && sections.load(bytes.data(), bytes.size()) &&
                                archives.read("DBFilesClient\\CharHairGeosets.dbc", bytes) && hairs.load(bytes.data(), bytes.size()) &&
                                archives.read("DBFilesClient\\CharacterFacialHairStyles.dbc", bytes) &&
                                facials.load(bytes.data(), bytes.size());
            reclient::CharacterLook bald, haired;
            const bool body = loaded && reclient::characterLook(sections, hairs, facials, 1, 0, 0, 0, 0, 0, 0, bald);
            const bool hair = loaded && reclient::characterLook(sections, hairs, facials, 1, 0, 0, 0, 1, 0, 0, haired);
            std::printf("human skin %s hair %s face %s\n", bald.body.c_str(), haired.hair.c_str(), haired.face[0].c_str());
            const auto image = [](const std::string& path) {
                return path.size() >= 4 && (path.compare(path.size() - 4, 4, ".blp") == 0 || path.compare(path.size() - 4, 4, ".BLP") == 0);
            };
            if (!body || !hair || !image(bald.body) || !image(haired.hair) || !bald.hair.empty() || !image(haired.face[0]) ||
                haired.paints.empty()) {
                std::printf("character skin did not resolve\n");
                return 1;
            }
            reclient::Image canvas;
            canvas.width = 512;
            canvas.height = 512;
            canvas.rgba.assign(size_t(512 * 512 * 4), 0);
            for (size_t i = 0; i < canvas.rgba.size(); i += 4) {
                canvas.rgba[i] = 10;
                canvas.rgba[i + 3] = 255;
            }
            reclient::Image clearPatch;
            clearPatch.width = 4;
            clearPatch.height = 4;
            clearPatch.rgba.assign(size_t(4 * 4 * 4), 0);
            reclient::paintSkin(canvas, clearPatch, 8);
            reclient::Image patch;
            patch.width = 4;
            patch.height = 4;
            patch.rgba.assign(size_t(4 * 4 * 4), 0);
            for (size_t i = 0; i < patch.rgba.size(); i += 4) {
                patch.rgba[i] = 200;
                patch.rgba[i + 3] = 255;
            }
            reclient::paintSkin(canvas, patch, 9);
            const size_t head = (size_t(400) * 512 + 10) * 4;
            const size_t upper = (size_t(330) * 512 + 10) * 4;
            const size_t away = (size_t(10) * 512 + 400) * 4;
            if (canvas.rgba[head] < 100 || canvas.rgba[upper] != 10 || canvas.rgba[away] != 10) {
                std::printf("skin section paste missed\n");
                return 1;
            }
            reclient::CharacterLook dressed = haired;
            reclient::EquipVisual piece;
            piece.slot = 4;
            piece.geoset[0] = 1;
            piece.component[3] = "LeatherChest";
            reclient::dressCharacter(&piece, 1, dressed);
            bool paintFound = false;
            bool geoFound = false;
            for (const reclient::SkinPaint& paint : dressed.paints) {
                if (paint.section == 3 && paint.path.find("TorsoUpperTexture") != std::string::npos &&
                    paint.path.find("LeatherChest") != std::string::npos)
                    paintFound = true;
            }
            for (uint16_t id : dressed.geosets)
                if (id == 802) geoFound = true;
            if (!paintFound || !geoFound) {
                std::printf("clothes did not dress the body\n");
                return 1;
            }
            auto loadBlp = [&](const std::string& path, reclient::Image& image) {
                std::vector<uint8_t> file;
                image = {};
                return archives.read(path, file) && reclient::decodeBlp(file.data(), file.size(), image) && image.width > 0;
            };
            reclient::Image skinImg, faceImg;
            if (!loadBlp(bald.body, skinImg) || !loadBlp(haired.face[0], faceImg)) {
                std::printf("skin images did not decode\n");
                return 1;
            }
            reclient::Image painted = skinImg;
            reclient::paintSkin(painted, faceImg, 9);
            bool changed = false;
            const int y0 = painted.height * 384 / 512;
            const int y1 = std::min(painted.height, y0 + std::max(1, painted.height * 128 / 512));
            const int x1 = std::min(painted.width, std::max(1, painted.width * 256 / 512));
            for (int y = y0; y < y1 && !changed; ++y) {
                for (int x = 0; x < x1; ++x) {
                    const size_t i = (size_t(y) * size_t(painted.width) + x) * 4;
                    if (i + 2 >= painted.rgba.size() || i + 2 >= skinImg.rgba.size()) continue;
                    if (painted.rgba[i] != skinImg.rgba[i] || painted.rgba[i + 1] != skinImg.rgba[i + 1] ||
                        painted.rgba[i + 2] != skinImg.rgba[i + 2])
                        changed = true;
                }
            }
            if (!changed) {
                std::printf("face overlay did not change the skin\n");
                return 1;
            }
        }
        if (archives.read("World\\Maps\\Azeroth\\Azeroth_33_41.adt", bytes)) {
            reclient::Terrain ground;
            if (reclient::loadAdt(bytes.data(), bytes.size(), 33, 41, ground)) {
                float hz = 0;
                const bool hit = reclient::terrainHeight(ground, -4918.f, -917.f, hz);
                std::printf("ironforge ground %s z %.1f tile %.1f..%.1f\n", hit ? "hit" : "open", hz, ground.minZ,
                            ground.maxZ);
                // The forge itself is a hole in the mountain. A hit anywhere else still has to be city height.
                if (ground.maxZ < 400.f || ground.minZ > 900.f || (hit && (hz < 400.f || hz > 900.f))) {
                    std::printf("ironforge ground is not at city height\n");
                    return 1;
                }
            }
        }
        std::vector<reclient::MapPlacement> places;
        reclient::loadAdtPlacements(bytes.data(), bytes.size(), places);
        const reclient::MapPlacement* hall = nullptr;
        for (const reclient::MapPlacement& place : places) {
            if (place.wmo && place.path.find("ironforge.wmo") != std::string::npos) hall = &place;
        }
        if (!hall) {
            std::printf("ironforge building is not in the map tile\n");
            return 1;
        }
        std::vector<uint8_t> rootBytes;
        reclient::WmoRoot root;
        if (!archives.read(hall->path, rootBytes) || !reclient::loadWmoRoot(rootBytes.data(), rootBytes.size(), root)) {
            std::printf("ironforge building did not load from the archives\n");
            return 1;
        }
        const float qx = -4918.f, qy = -933.f, qz = 520.f;
        float floorZ = qz;
        float floorGap = 1.0e9f;
        bool floorHit = false;
        int groupsTried = 0;
        for (int g = 0; g < int(root.groups.size()); ++g) {
            const reclient::WmoGroupBox& box = root.groups[g];
            if ((box.flags & 0x2000) == 0) continue;
            float x0 = 1.0e9f, y0 = 1.0e9f, z0 = 1.0e9f, x1 = -1.0e9f, y1 = -1.0e9f, z1 = -1.0e9f;
            for (int corner = 0; corner < 8; ++corner) {
                float x = 0, y = 0, z = 0;
                reclient::transformPoint(hall->world, corner & 1 ? box.max[0] : box.min[0], corner & 2 ? box.max[1] : box.min[1],
                                         corner & 4 ? box.max[2] : box.min[2], x, y, z);
                x0 = std::min(x0, x);
                y0 = std::min(y0, y);
                z0 = std::min(z0, z);
                x1 = std::max(x1, x);
                y1 = std::max(y1, y);
                z1 = std::max(z1, z);
            }
            if (qx < x0 || qx > x1 || qy < y0 || qy > y1 || qz + 1.8f < z0 || qz - 40.f > z1) continue;
            std::string groupPath = hall->path;
            const size_t dot = groupPath.rfind('.');
            char suffix[8];
            std::snprintf(suffix, sizeof(suffix), "_%03d", g);
            if (dot == std::string::npos) groupPath += suffix;
            else groupPath.insert(dot, suffix);
            std::vector<uint8_t> groupBytes;
            reclient::LitMesh mesh;
            if (!archives.read(groupPath, groupBytes) ||
                !reclient::loadWmoGroupMesh(groupBytes.data(), groupBytes.size(), hall->world, root.materials, mesh))
                continue;
            ++groupsTried;
            const size_t tris = mesh.indices.size() / 3;
            for (size_t t = 0; t < tris; ++t) {
                const uint32_t i0 = mesh.indices[t * 3];
                const uint32_t i1 = mesh.indices[t * 3 + 1];
                const uint32_t i2 = mesh.indices[t * 3 + 2];
                if (i0 * 3 + 2 >= mesh.xyz.size() || i1 * 3 + 2 >= mesh.xyz.size() || i2 * 3 + 2 >= mesh.xyz.size()) continue;
                const float ax = mesh.xyz[i0 * 3], ay = mesh.xyz[i0 * 3 + 1], az = mesh.xyz[i0 * 3 + 2];
                const float bx = mesh.xyz[i1 * 3], by = mesh.xyz[i1 * 3 + 1], bz = mesh.xyz[i1 * 3 + 2];
                const float cx = mesh.xyz[i2 * 3], cy = mesh.xyz[i2 * 3 + 1], cz = mesh.xyz[i2 * 3 + 2];
                const float det = (by - cy) * (ax - cx) + (cx - bx) * (ay - cy);
                if (std::fabs(det) < 1e-4f) continue;
                const float w0 = ((by - cy) * (qx - cx) + (cx - bx) * (qy - cy)) / det;
                const float w1 = ((cy - ay) * (qx - cx) + (ax - cx) * (qy - cy)) / det;
                const float w2 = 1.f - w0 - w1;
                if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
                const float hz = w0 * az + w1 * bz + w2 * cz;
                if (hz > qz + 1.8f || hz < qz - 40.f) continue;
                const float gap = std::fabs(qz - hz);
                if (gap < floorGap) {
                    floorGap = gap;
                    floorZ = hz;
                    floorHit = true;
                }
            }
        }
        std::printf("archive floor %s z %.1f groups %d\n", floorHit ? "hit" : "miss", floorZ, groupsTried);
        if (!floorHit || floorZ < 495.f || floorZ > 510.f) {
            std::printf("archive floor is not the Ironforge hall\n");
            return 1;
        }
    }

    if (!archives.read("World\\Maps\\Azeroth\\Azeroth.wdt", bytes)) {
        std::printf("miss wdt\n");
        return 1;
    }
    std::printf("wdt %zu %.4s\n", bytes.size(), bytes.data());
    for (size_t at = 0; at + 8 <= bytes.size() && at < 64;) {
        uint32_t sz = 0;
        std::memcpy(&sz, bytes.data() + at + 4, 4);
        std::printf(" chunk %.4s %u\n", bytes.data() + at, sz);
        if (at + 8 + sz > bytes.size()) break;
        at += 8 + sz;
    }
    reclient::MapInfo info;
    if (!loadWdt(bytes.data(), bytes.size(), info)) {
        std::printf("wdt parse failed\n");
        return 1;
    }
    int tiles = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (info.present[x][y]) ++tiles;
    std::printf("Azeroth tiles %d wmos %zu\n", tiles, info.models.size());

    const char* adts[] = {"World\\Maps\\Azeroth\\Azeroth_32_48.adt", "World\\Maps\\Azeroth\\Azeroth_48_32.adt"};
    bool terrain = false;
    for (const char* name : adts) {
        if (!archives.read(name, bytes)) {
            std::printf("miss %s\n", name);
            continue;
        }
        int tx = 0, ty = 0;
        std::sscanf(name, "World\\Maps\\Azeroth\\Azeroth_%d_%d.adt", &tx, &ty);
        reclient::Terrain ground;
        const bool ok = loadAdt(bytes.data(), bytes.size(), tx, ty, ground);
        std::printf("%s tris %zu bounds %.1f %.1f .. %.1f %.1f z %.1f..%.1f\n", ok ? "ok" : "bad",
                    ground.indices.size() / 3, ground.minX, ground.minY, ground.maxX, ground.maxY, ground.minZ,
                    ground.maxZ);
        const float nx = -8949.95f, ny = -132.49f;
        if (ok && nx <= ground.maxX && nx >= ground.minX && ny <= ground.maxY && ny >= ground.minY) terrain = true;
        const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        const size_t mark = text.find(".wmo");
        if (mark != std::string::npos) {
            size_t start = mark;
            while (start > 0 && text[start - 1] != '\0') --start;
            const std::string model = text.substr(start, mark + 4 - start);
            std::vector<uint8_t> wmoBytes;
            reclient::Mesh building;
            if (!archives.read(model, wmoBytes)) {
                std::printf("wmo missing %s\n", model.c_str());
            } else if (loadWmo(wmoBytes.data(), wmoBytes.size(), building)) {
                std::printf("wmo %s verts %zu tris %zu\n", model.c_str(), building.xyz.size() / 3,
                            building.indices.size() / 3);
            } else {
                const int groups = reclient::wmoGroupCount(wmoBytes.data(), wmoBytes.size());
                std::string group = model;
                const size_t dot = group.rfind('.');
                if (dot != std::string::npos) group.insert(dot, "_000");
                std::vector<uint8_t> groupBytes;
                if (archives.read(group, groupBytes) && loadWmo(groupBytes.data(), groupBytes.size(), building))
                    std::printf("wmo group %s verts %zu tris %zu (root groups %d)\n", group.c_str(),
                                building.xyz.size() / 3, building.indices.size() / 3, groups);
                else
                    std::printf("wmo parse miss %s groups %d\n", model.c_str(), groups);
            }
        }
    }

    if (!archives.read("Interface\\Glues\\Models\\UI_MainMenu\\UI_MainMenu.m2", bytes)) return 1;
    reclient::Mesh menu;
    if (!loadM2(bytes.data(), bytes.size(), menu)) return 1;
    std::printf("m2 verts %zu indices %zu\n", menu.xyz.size() / 3, menu.indices.size());

    bool wmo = false;
    if (!info.models.empty()) {
        std::string path = info.models.front();
        for (char& c : path)
            if (c == '/') c = '\\';
        if (archives.read(path, bytes)) {
            reclient::Mesh building;
            wmo = loadWmo(bytes.data(), bytes.size(), building);
            std::printf("wmo %s verts %zu\n", path.c_str(), building.xyz.size() / 3);
        } else {
            std::printf("miss wmo %s\n", path.c_str());
        }
    }

    bool blp = false;
    const char* textures[] = {"Interface\\Glues\\Models\\UI_MainMenu\\UI_MainMenu.blp",
                              "Interface\\Buttons\\UI-CheckBox-Check.blp"};
    for (const char* name : textures) {
        if (!archives.read(name, bytes)) continue;
        reclient::Image image;
        if (decodeBlp(bytes.data(), bytes.size(), image)) {
            std::printf("blp %s %dx%d\n", name, image.width, image.height);
            blp = true;
            break;
        }
    }
    if (archives.read("World\\Maps\\Azeroth\\Azeroth_33_41.adt", bytes)) {
        auto ru32 = [](const uint8_t* p) {
            uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return v;
        };
        auto rf32 = [](const uint8_t* p) {
            float v = 0;
            std::memcpy(&v, p, 4);
            return v;
        };
        auto cid = [](const uint8_t* p, const char* n) {
            return p[0] == n[3] && p[1] == n[2] && p[2] == n[1] && p[3] == n[0];
        };
        const uint8_t* mwmo = nullptr;
        size_t mwmoN = 0;
        const uint8_t* mwid = nullptr;
        size_t mwidN = 0;
        const uint8_t* modf = nullptr;
        size_t modfN = 0;
        size_t at = 0;
        while (at + 8 <= bytes.size()) {
            const size_t sz = ru32(bytes.data() + at + 4);
            if (at + 8 + sz > bytes.size()) break;
            const uint8_t* p = bytes.data() + at;
            if (cid(p, "MWMO")) {
                mwmo = p + 8;
                mwmoN = sz;
            } else if (cid(p, "MWID")) {
                mwid = p + 8;
                mwidN = sz;
            } else if (cid(p, "MODF")) {
                modf = p + 8;
                modfN = sz;
            }
            at += 8 + sz;
        }
        std::printf("ironforge tile mwmo %zu modf %zu\n", mwmoN, modfN / 64);
        const size_t count = modf ? modfN / 64 : 0;
        int printed = 0;
        for (size_t i = 0; i < count && printed < 12; ++i) {
            const uint8_t* e = modf + i * 64;
            const uint32_t nameId = ru32(e);
            const float x = rf32(e + 8), y = rf32(e + 12), z = rf32(e + 16);
            const float rx = rf32(e + 20), ry = rf32(e + 24), rz = rf32(e + 28);
            const float e0 = rf32(e + 32), e1 = rf32(e + 36), e2 = rf32(e + 40);
            const float e3 = rf32(e + 44), e4 = rf32(e + 48), e5 = rf32(e + 52);
            std::string name;
            if (mwid && nameId * 4 + 4 <= mwidN && mwmo) {
                const uint32_t off = ru32(mwid + nameId * 4);
                if (off < mwmoN) name = reinterpret_cast<const char*>(mwmo + off);
            }
            const bool near = (e0 < -4918 && -4918 < e3 && e1 < -917 && -917 < e4) ||
                              (e0 < -4918 && -4918 < e3 && e2 < 501 && 501 < e5) ||
                              (std::fabs(x + 4918) < 400 && std::fabs(y + 917) < 400);
            if (!near && name.find("iron") == std::string::npos && name.find("Iron") == std::string::npos) continue;
            std::printf("modf %s pos %.1f %.1f %.1f rot %.1f %.1f %.1f ext %.1f %.1f %.1f .. %.1f %.1f %.1f\n",
                        name.c_str(), x, y, z, rx, ry, rz, e0, e1, e2, e3, e4, e5);
            ++printed;
        }
    }

    if (!terrain || menu.indices.empty()) {
        std::printf("terrain %d mesh %zu\n", terrain ? 1 : 0, menu.indices.size());
        return 1;
    }
    (void)wmo;
    (void)blp;
    reclient::SoundBank bank;
    if (!bank.load(archives)) {
        std::printf("sound tables did not load\n");
        return 1;
    }
    const reclient::SoundBank::Plan iron = bank.planFor(bank.areaAt(0, -4918.f, -917.f));
    bool walking = false;
    for (const std::string& file : iron.music.files)
        if (file.find("IronForge Walking") != std::string::npos) walking = true;
    const reclient::SoundBank::Piece click = bank.cue("igMainMenuOpen");
    std::vector<uint8_t> clickBytes;
    const bool clickFile = !click.files.empty() && archives.read(click.files[0], clickBytes);
    const std::vector<int16_t> clickPcm = clickFile ? bank.decode(archives, click.files[0]) : std::vector<int16_t>{};
    std::printf("ironforge music %zu intro %zu silence %u click %s pcm %zu\n", iron.music.files.size(), iron.intro.files.size(),
                iron.silenceMinMs, click.files.empty() ? "" : click.files[0].c_str(), clickPcm.size());
    const std::vector<int16_t> introPcm = iron.intro.files.empty() ? std::vector<int16_t>{} : bank.decode(archives, iron.intro.files[0]);
    std::printf("ironforge intro pcm %zu\n", introPcm.size());
    if (iron.name != "Ironforge" || !walking || introPcm.size() < 44100 || clickPcm.size() < 100) return 1;
    const reclient::SoundBank::Plan storm = bank.planFor(1519);
    if (storm.music.files.empty() || storm.intro.files.empty()) {
        std::printf("stormwind music %zu intro %zu\n", storm.music.files.size(), storm.intro.files.size());
        return 1;
    }
    return 0;
}
