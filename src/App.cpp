#include "App.hpp"

#include "Diag.hpp"
#include "Formats.hpp"
#include "FrameXml.hpp"
#include "Mpq.hpp"
#include "Scene.hpp"
#include "Sound.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <GL/gl.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace reclient {
namespace {

constexpr unsigned kGlArrayBuffer = 0x8892;
constexpr unsigned kGlElementArrayBuffer = 0x8893;
constexpr unsigned kGlStreamDraw = 0x88E0;
constexpr unsigned kGlStaticDraw = 0x88E4;
using GlGenBuffersFn = void(APIENTRY*)(GLsizei, GLuint*);
using GlBindBufferFn = void(APIENTRY*)(GLenum, GLuint);
using GlBufferDataFn = void(APIENTRY*)(GLenum, std::intptr_t, const void*, GLenum);
using GlDeleteBuffersFn = void(APIENTRY*)(GLsizei, const GLuint*);

constexpr float kChunkYards = 533.33333f / 16.f;

struct VideoSettings {
    float nearClip = kNearClip;
    float farClip = kFarClip;
    float horizon = kHorizonFarClip;
    bool localTime = true;
};

VideoSettings readVideo(const std::string& root) {
    VideoSettings out;
    std::ifstream in(root + "\\WTF\\Config.wtf");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("SET ", 0) != 0) continue;
        const size_t nameAt = 4;
        const size_t nameEnd = line.find(' ', nameAt);
        if (nameEnd == std::string::npos) continue;
        const std::string name = line.substr(nameAt, nameEnd - nameAt);
        const size_t quote = line.find('"', nameEnd);
        const size_t end = quote == std::string::npos ? std::string::npos : line.find('"', quote + 1);
        if (quote == std::string::npos || end == std::string::npos) continue;
        const std::string value = line.substr(quote + 1, end - quote - 1);
        const float number = std::strtof(value.c_str(), nullptr);
        if (name == "farclip") out.farClip = number;
        else if (name == "horizonfarclip") out.horizon = number;
        else if (name == "nearclip") out.nearClip = number;
        else if (name == "timeMgrUseLocalTime") out.localTime = number != 0.f;
    }
    if (out.farClip < 1.f) out.farClip = kFarClip;
    if (out.horizon < out.farClip) out.horizon = kHorizonFarClip;
    if (out.nearClip <= 0.f || out.nearClip >= out.farClip) out.nearClip = kNearClip;
    return out;
}

int halfMinuteNow() {
    SYSTEMTIME clock;
    GetLocalTime(&clock);
    return (int(clock.wHour) * 60 + int(clock.wMinute)) * 2 + (clock.wSecond >= 30 ? 1 : 0);
}

void cameraMatrices(const Camera& cam, int width, int height, float proj[16], float view[16], float nearPlane, float farPlane) {
    const float focal = float(height) * (420.f / 720.f);
    const float sx = 2.f * focal / float(width);
    const float sy = 2.f * focal / float(height);
    const float span = farPlane - nearPlane;
    const float A = (farPlane + nearPlane) / span;
    const float B = -2.f * farPlane * nearPlane / span;
    std::fill(proj, proj + 16, 0.f);
    proj[0] = sx;
    proj[5] = sy;
    proj[10] = A;
    proj[11] = 1.f;
    proj[14] = B;

    const float c = std::cos(cam.o);
    const float s = std::sin(cam.o);
    const float cp = std::cos(cam.pitch);
    const float sp = std::sin(cam.pitch);
    std::fill(view, view + 16, 0.f);
    view[0] = s;
    view[1] = c * sp;
    view[2] = c * cp;
    view[4] = -c;
    view[5] = s * sp;
    view[6] = s * cp;
    view[9] = cp;
    view[10] = -sp;
    view[12] = -s * cam.x + c * cam.y;
    view[13] = -(c * sp) * cam.x - (s * sp) * cam.y - cp * cam.z;
    view[14] = -(c * cp) * cam.x - (s * cp) * cam.y + sp * cam.z;
    view[15] = 1.f;
}

struct Draw {
    SDL_Window* window = nullptr;
    SDL_GLContext gl = nullptr;
    SDL_Surface* hud = nullptr;
    SDL_Renderer* hudRenderer = nullptr;
    GLuint hudTex = 0;
    GLuint whiteTex = 0;
    std::unordered_map<int, GLuint> textures;
    std::unordered_map<std::string, int> artIds;
    std::vector<std::vector<float>> poseSlots;
    size_t poseCursor = 0;
    GlGenBuffersFn glGenBuffers_ = nullptr;
    GlBindBufferFn glBindBuffer_ = nullptr;
    GlBufferDataFn glBufferData_ = nullptr;
    GlDeleteBuffersFn glDeleteBuffers_ = nullptr;
    struct GroundGpu {
        const Terrain* key = nullptr;
        const float* xyz = nullptr;
        size_t xyzN = 0;
        const float* uv = nullptr;
        GLuint xyzBuf = 0;
        GLuint uvBuf = 0;
    };
    std::vector<GroundGpu> groundGpu;
    size_t groundGpuNext = 0;
    GLuint poseXyz_ = 0;
    GLuint poseUv_ = 0;
    GLuint poseRgb_ = 0;
    GLuint poseIdx_ = 0;
    bool poseBufTried_ = false;
    uint64_t hudStamp = 0;
    bool hudReady = false;
    std::vector<uint8_t> terrainColor;
    std::vector<uint32_t> terrainIndex;
    std::vector<float> skyXyz;
    std::vector<uint8_t> skyRgb;
    std::vector<uint32_t> skyIndex;
    uint8_t skyKey[18]{};
    OutdoorLight sun;
    float farYards = kFarClip;
    float nearYards = kNearClip;
    uint8_t clearR = 24, clearG = 20, clearB = 18;
    int w = 1280;
    int h = 720;

    bool open(bool hidden) {
        if (!SDL_Init(SDL_INIT_VIDEO)) return false;
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
        SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
        const SDL_WindowFlags flags = SDL_WindowFlags(SDL_WINDOW_OPENGL | (hidden ? SDL_WINDOW_HIDDEN : SDL_WINDOW_RESIZABLE));
        window = SDL_CreateWindow("reclient 2.4.3", w, h, flags);
        if (!window) return false;
        gl = SDL_GL_CreateContext(window);
        if (!gl) return false;
        SDL_GL_SetSwapInterval(1);
        glDisable(GL_CULL_FACE);
        glDisable(GL_LIGHTING);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LEQUAL);
        glGenTextures(1, &whiteTex);
        glBindTexture(GL_TEXTURE_2D, whiteTex);
        const uint8_t white[4] = {255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        return true;
    }
    void close() {
        if (gl) {
            SDL_GL_MakeCurrent(window, gl);
            for (const auto& item : textures) glDeleteTextures(1, &item.second);
            textures.clear();
            if (whiteTex) glDeleteTextures(1, &whiteTex);
            if (hudTex) glDeleteTextures(1, &hudTex);
            whiteTex = 0;
            hudTex = 0;
        }
        if (hudRenderer) SDL_DestroyRenderer(hudRenderer);
        if (hud) SDL_DestroySurface(hud);
        if (gl) SDL_GL_DestroyContext(gl);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        hudRenderer = nullptr;
        hud = nullptr;
        gl = nullptr;
        window = nullptr;
    }
    void ensureHud() {
        int dw = 0, dh = 0;
        SDL_GetWindowSizeInPixels(window, &dw, &dh);
        if (dw > 0 && dh > 0) {
            w = dw;
            h = dh;
        }
        if (hud && hud->w == w && hud->h == h) return;
        if (hudRenderer) SDL_DestroyRenderer(hudRenderer);
        if (hud) SDL_DestroySurface(hud);
        hud = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        hudRenderer = hud ? SDL_CreateSoftwareRenderer(hud) : nullptr;
    }
    bool poseBuffers() {
        if (poseXyz_ != 0) return true;
        if (poseBufTried_) return false;
        poseBufTried_ = true;
        glGenBuffers_ = reinterpret_cast<GlGenBuffersFn>(SDL_GL_GetProcAddress("glGenBuffers"));
        glBindBuffer_ = reinterpret_cast<GlBindBufferFn>(SDL_GL_GetProcAddress("glBindBuffer"));
        glBufferData_ = reinterpret_cast<GlBufferDataFn>(SDL_GL_GetProcAddress("glBufferData"));
        glDeleteBuffers_ = reinterpret_cast<GlDeleteBuffersFn>(SDL_GL_GetProcAddress("glDeleteBuffers"));
        if (!glGenBuffers_ || !glBindBuffer_ || !glBufferData_ || !glDeleteBuffers_) return false;
        GLuint ids[4] = {};
        glGenBuffers_(4, ids);
        poseXyz_ = ids[0];
        poseUv_ = ids[1];
        poseRgb_ = ids[2];
        poseIdx_ = ids[3];
        return poseXyz_ != 0;
    }
    void unbindPose() {
        if (!glBindBuffer_) return;
        glBindBuffer_(kGlArrayBuffer, 0);
        glBindBuffer_(kGlElementArrayBuffer, 0);
    }
    void begin() {
        SDL_GL_MakeCurrent(window, gl);
        ensureHud();
        glViewport(0, 0, w, h);
        glClearColor(clearR / 255.f, clearG / 255.f, clearB / 255.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    void wipeHud() {
        if (!hudRenderer) return;
        SDL_SetRenderDrawColor(hudRenderer, 0, 0, 0, 0);
        SDL_RenderClear(hudRenderer);
    }
    void look(const Camera& cam, float farPlane) {
        float proj[16], view[16];
        cameraMatrices(cam, w, h, proj, view, nearYards, farPlane);
        glMatrixMode(GL_PROJECTION);
        glLoadMatrixf(proj);
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixf(view);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST);
        glAlphaFunc(GL_GEQUAL, 96.f / 255.f);
        glDisable(GL_BLEND);
        unbindPose();
    }
    void bindImage(const Image* image, int id) {
        glEnable(GL_TEXTURE_2D);
        if (!image) {
            glBindTexture(GL_TEXTURE_2D, whiteTex);
            return;
        }
        const auto found = textures.find(id);
        if (found != textures.end()) {
            glBindTexture(GL_TEXTURE_2D, found->second);
            return;
        }
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image->width, image->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image->rgba.data());
        textures[id] = tex;
    }
    void lit(WorldScene& scene, const LitMesh& mesh, const Mat4& world, bool towardCamera = false,
             const uint16_t* geosets = nullptr, int geosetCount = 0) {
        const size_t count = mesh.xyz.size() / 3;
        if (count == 0 || mesh.indices.empty()) return;
        glPushMatrix();
        glMultMatrixf(world.m);
        (void)towardCamera;
        const bool streamed = geosetCount > 0 && poseBuffers();
        const float* posed = mesh.xyz.data();
        if (!streamed && geosetCount > 0) {
            // Characters share one mesh, and the draw reads these vertices later.
            // Reusing last frame's buffer makes the body pop between two poses.
            if (poseSlots.size() < 256) poseSlots.resize(256);
            auto& slot = poseSlots[poseCursor % poseSlots.size()];
            slot = mesh.xyz;
            posed = slot.data();
            ++poseCursor;
        }
        glEnableClientState(GL_VERTEX_ARRAY);
        if (streamed) {
            // The hall draws from client memory too. Handing the pose to the driver
            // here means a later character cannot replace this step before it is drawn.
            glBindBuffer_(kGlArrayBuffer, poseXyz_);
            glBufferData_(kGlArrayBuffer, std::intptr_t(mesh.xyz.size() * sizeof(float)), mesh.xyz.data(), kGlStreamDraw);
            glVertexPointer(3, GL_FLOAT, 0, nullptr);
        } else glVertexPointer(3, GL_FLOAT, 0, posed);
        if (mesh.uv.size() >= count * 2) {
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            if (streamed) {
                glBindBuffer_(kGlArrayBuffer, poseUv_);
                glBufferData_(kGlArrayBuffer, std::intptr_t(mesh.uv.size() * sizeof(float)), mesh.uv.data(), kGlStreamDraw);
                glTexCoordPointer(2, GL_FLOAT, 0, nullptr);
            } else glTexCoordPointer(2, GL_FLOAT, 0, mesh.uv.data());
        } else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        if (mesh.rgb.size() >= count * 3) {
            glEnableClientState(GL_COLOR_ARRAY);
            if (streamed) {
                glBindBuffer_(kGlArrayBuffer, poseRgb_);
                glBufferData_(kGlArrayBuffer, std::intptr_t(mesh.rgb.size()), mesh.rgb.data(), kGlStreamDraw);
                glColorPointer(3, GL_UNSIGNED_BYTE, 0, nullptr);
            } else glColorPointer(3, GL_UNSIGNED_BYTE, 0, mesh.rgb.data());
        } else {
            glDisableClientState(GL_COLOR_ARRAY);
            glColor3ub(255, 255, 255);
        }
        if (streamed) glBindBuffer_(kGlArrayBuffer, 0);
        const bool cullBody = geosetCount > 0;
        auto drawIndices = [&](const uint32_t* indices, uint32_t nidx, int texture, bool twoSided) {
            if (nidx < 3) return;
            if (cullBody && !twoSided) {
                glEnable(GL_CULL_FACE);
                glCullFace(GL_BACK);
            } else glDisable(GL_CULL_FACE);
            bindImage(scene.image(texture), texture);
            if (streamed) {
                glBindBuffer_(kGlElementArrayBuffer, poseIdx_);
                glBufferData_(kGlElementArrayBuffer, std::intptr_t(nidx * sizeof(uint32_t)), indices, kGlStreamDraw);
                glDrawElements(GL_TRIANGLES, GLsizei(nidx), GL_UNSIGNED_INT, nullptr);
            } else glDrawElements(GL_TRIANGLES, GLsizei(nidx), GL_UNSIGNED_INT, indices);
        };
        if (mesh.batches.empty()) drawIndices(mesh.indices.data(), uint32_t(mesh.indices.size()), -1, true);
        else if (geosetCount > 0) {
            // Body, hair, and cloak batches share a texture. The grouped index list depends only on
            // the mesh and which geosets are worn, so it is built once and reused.
            uint32_t geoHash = 2166136261u;
            geoHash ^= uint32_t(mesh.indices.size()) + uint32_t(mesh.batches.size()) * 1315423911u;
            for (int g = 0; g < geosetCount; ++g)
                geoHash ^= uint32_t(geosets[g]) + 0x9e3779b9u + (geoHash << 6) + (geoHash >> 2);
            struct Group {
                int texture = -2;
                bool twoSided = false;
                std::vector<uint32_t> indices;
            };
            struct Cached {
                const void* id = nullptr;
                uint32_t hash = 0;
                std::vector<Group> groups;
            };
            static std::vector<Cached> cache;
            const void* id = mesh.indices.data();
            Cached* hit = nullptr;
            for (Cached& item : cache)
                if (item.id == id && item.hash == geoHash) hit = &item;
            if (!hit) {
                if (cache.size() > 96) cache.clear();
                bool inMesh[4096] = {};
                for (const PrimBatch& batch : mesh.batches)
                    if (batch.geoset < 4096) inMesh[batch.geoset] = true;
                bool show[4096] = {};
                for (int g = 0; g < geosetCount; ++g)
                    if (geosets[g] < 4096 && inMesh[geosets[g]]) show[geosets[g]] = true;
                // A clothing id that this body does not have used to hide the whole limb.
                // The naked section, or the first section the mesh actually has, still has to animate.
                static const uint16_t kNaked[] = {0, 1, 101, 201, 301, 401, 501, 601, 702, 801, 901, 1001, 1101, 1201, 1301, 1401, 1501};
                for (uint16_t base : kNaked) {
                    const int lo = base < 100 ? base : (base / 100) * 100;
                    const int hi = base < 100 ? base : lo + 99;
                    bool covered = false;
                    for (int g = lo; g <= hi; ++g)
                        if (show[g]) covered = true;
                    if (covered) continue;
                    int pick = inMesh[base] ? base : -1;
                    if (pick < 0)
                        for (int g = lo; g <= hi; ++g)
                            if (inMesh[g]) {
                                pick = g;
                                break;
                            }
                    if (pick >= 0) show[pick] = true;
                }
                auto append = [&](std::vector<Group>& groups, const PrimBatch& batch) {
                    if (batch.count < 3 || size_t(batch.start) + batch.count > mesh.indices.size()) return;
                    const bool twoSided = batch.twoSided || batch.slot != 0;
                    Group* dest = nullptr;
                    for (Group& group : groups) {
                        if (group.texture == batch.texture && group.twoSided == twoSided) {
                            dest = &group;
                            break;
                        }
                    }
                    if (!dest) {
                        groups.emplace_back();
                        dest = &groups.back();
                        dest->texture = batch.texture;
                        dest->twoSided = twoSided;
                    }
                    dest->indices.insert(dest->indices.end(), mesh.indices.begin() + batch.start,
                                         mesh.indices.begin() + batch.start + batch.count);
                };
                Cached made;
                made.id = id;
                made.hash = geoHash;
                for (const PrimBatch& batch : mesh.batches) {
                    if (batch.geoset >= 4096 || !show[batch.geoset]) continue;
                    append(made.groups, batch);
                }
                if (made.groups.empty()) {
                    for (const PrimBatch& batch : mesh.batches) {
                        if (batch.geoset > 99 && batch.slot == 0) continue;
                        append(made.groups, batch);
                    }
                }
                cache.push_back(std::move(made));
                hit = &cache.back();
            }
            for (const Group& group : hit->groups)
                drawIndices(group.indices.data(), uint32_t(group.indices.size()), group.texture, group.twoSided);
        } else {
            uint32_t lastStart = 0xffffffffu;
            for (const PrimBatch& batch : mesh.batches) {
                if (batch.count < 3 || size_t(batch.start) + batch.count > mesh.indices.size()) continue;
                const bool overlay = batch.start == lastStart;
                lastStart = batch.start;
                if (overlay) {
                    glDisable(GL_ALPHA_TEST);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glDepthMask(GL_FALSE);
                    glDepthFunc(GL_LEQUAL);
                }
                drawIndices(mesh.indices.data() + batch.start, batch.count, batch.texture, batch.twoSided);
                if (overlay) {
                    glDisable(GL_BLEND);
                    glDepthMask(GL_TRUE);
                    glDepthFunc(GL_LESS);
                    glEnable(GL_ALPHA_TEST);
                }
            }
        }
        if (cullBody) glDisable(GL_CULL_FACE);
        if (streamed) unbindPose();
        glPopMatrix();
    }
    void paintArt(WorldScene& scene, ArchiveSet& archives, const std::vector<UiWidget>& widgets) {
        bool any = false;
        for (const UiWidget& widget : widgets)
            if (!widget.art.empty()) any = true;
        if (!any) return;
        glDisable(GL_FOG);
        glMatrixMode(GL_PROJECTION);
        glPushMatrix();
        glLoadIdentity();
        glOrtho(0.0, double(w), double(h), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glPushMatrix();
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_ALPHA_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_TEXTURE_2D);
        glColor4ub(255, 255, 255, 255);
        for (const UiWidget& widget : widgets) {
            if (widget.art.empty() || widget.w < 2.f || widget.h < 2.f) continue;
            int id = -1;
            const auto cached = artIds.find(widget.art);
            if (cached != artIds.end()) id = cached->second;
            else {
                std::string path = widget.art;
                for (char& c : path)
                    if (c == '/') c = '\\';
                if (path.size() < 4 || (path.compare(path.size() - 4, 4, ".blp") != 0 && path.compare(path.size() - 4, 4, ".BLP") != 0))
                    path += ".blp";
                id = scene.picture(archives, path);
                artIds.emplace(widget.art, id);
            }
            const Image* image = scene.image(id);
            if (!image) continue;
            bindImage(image, id);
            const float x0 = widget.x;
            const float y0 = widget.y;
            const float x1 = widget.x + widget.w;
            const float y1 = widget.y + widget.h;
            const float xy[] = {x0, y0, x1, y0, x1, y1, x0, y1};
            const float uv[] = {widget.u0, widget.v0, widget.u1, widget.v0, widget.u1, widget.v1, widget.u0, widget.v1};
            glEnableClientState(GL_VERTEX_ARRAY);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glDisableClientState(GL_COLOR_ARRAY);
            glVertexPointer(2, GL_FLOAT, 0, xy);
            glTexCoordPointer(2, GL_FLOAT, 0, uv);
            glDrawArrays(GL_QUADS, 0, 4);
        }
        glPopMatrix();
        glMatrixMode(GL_PROJECTION);
        glPopMatrix();
        glMatrixMode(GL_MODELVIEW);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
    }
    void fogOn() {
        if (!sun.ok || sun.fogEnd <= sun.fogStart + 1.f) {
            glDisable(GL_FOG);
            return;
        }
        const float color[4] = {sun.fog[0] / 255.f, sun.fog[1] / 255.f, sun.fog[2] / 255.f, 1.f};
        glFogi(GL_FOG_MODE, GL_LINEAR);
        glFogfv(GL_FOG_COLOR, color);
        glFogf(GL_FOG_START, sun.fogStart);
        glFogf(GL_FOG_END, sun.fogEnd);
        glEnable(GL_FOG);
    }
    void fogOff() { glDisable(GL_FOG); }
    void sky(const Camera& cam) {
        if (!sun.ok) return;
        uint8_t key[18];
        for (int i = 0; i < 5; ++i) {
            key[i * 3] = sun.sky[i][0];
            key[i * 3 + 1] = sun.sky[i][1];
            key[i * 3 + 2] = sun.sky[i][2];
        }
        key[15] = sun.fog[0];
        key[16] = sun.fog[1];
        key[17] = sun.fog[2];
        if (skyXyz.empty() || std::memcmp(skyKey, key, 18) != 0) {
            std::memcpy(skyKey, key, 18);
            skyXyz.clear();
            skyRgb.clear();
            skyIndex.clear();
            const int slices = 24;
            const int rings = 12;
            const float radius = 1000.f;
            auto colorAt = [&](float pitch, uint8_t rgb[3]) {
                const float zenith = 1.5707963f;
                const float down = (zenith - pitch) / zenith;
                if (down >= 1.f) {
                    rgb[0] = sun.fog[0];
                    rgb[1] = sun.fog[1];
                    rgb[2] = sun.fog[2];
                    return;
                }
                const float u = std::clamp(down, 0.f, 1.f) * 4.f;
                const int band = std::min(3, int(u));
                const float t = u - float(band);
                for (int c = 0; c < 3; ++c) {
                    const float mixed = float(sun.sky[band][c]) + (float(sun.sky[band + 1][c]) - float(sun.sky[band][c])) * t;
                    rgb[c] = uint8_t(std::clamp(mixed, 0.f, 255.f));
                }
            };
            uint8_t zenithRgb[3];
            colorAt(1.5707963f, zenithRgb);
            skyXyz.push_back(0);
            skyXyz.push_back(0);
            skyXyz.push_back(radius);
            skyRgb.insert(skyRgb.end(), zenithRgb, zenithRgb + 3);
            for (int ring = 1; ring <= rings; ++ring) {
                const float v = float(ring) / float(rings);
                const float pitch = (1.f - v) * 1.5707963f - 0.45f;
                const float z = std::sin(pitch) * radius;
                const float ringR = std::cos(pitch) * radius;
                uint8_t rgb[3];
                colorAt(pitch, rgb);
                for (int slice = 0; slice < slices; ++slice) {
                    const float yaw = float(slice) / float(slices) * 6.2831853f;
                    skyXyz.push_back(std::cos(yaw) * ringR);
                    skyXyz.push_back(std::sin(yaw) * ringR);
                    skyXyz.push_back(z);
                    skyRgb.insert(skyRgb.end(), rgb, rgb + 3);
                }
            }
            auto at = [slices](int ring, int slice) {
                if (ring <= 0) return 0u;
                return uint32_t(1 + (ring - 1) * slices + (slice % slices));
            };
            for (int slice = 0; slice < slices; ++slice) {
                skyIndex.push_back(0);
                skyIndex.push_back(at(1, slice));
                skyIndex.push_back(at(1, slice + 1));
            }
            for (int ring = 1; ring < rings; ++ring) {
                for (int slice = 0; slice < slices; ++slice) {
                    const uint32_t a = at(ring, slice);
                    const uint32_t b = at(ring, slice + 1);
                    const uint32_t c = at(ring + 1, slice);
                    const uint32_t d = at(ring + 1, slice + 1);
                    const uint32_t tri[6] = {a, c, b, b, c, d};
                    skyIndex.insert(skyIndex.end(), tri, tri + 6);
                }
            }
        }
        glDisable(GL_FOG);
        glDisable(GL_TEXTURE_2D);
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glDepthMask(GL_FALSE);
        const GLboolean cull = glIsEnabled(GL_CULL_FACE);
        glDisable(GL_CULL_FACE);
        glPushMatrix();
        glTranslatef(cam.x, cam.y, cam.z);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, skyXyz.data());
        glColorPointer(3, GL_UNSIGNED_BYTE, 0, skyRgb.data());
        glDrawElements(GL_TRIANGLES, GLsizei(skyIndex.size()), GL_UNSIGNED_INT, skyIndex.data());
        glPopMatrix();
        glDepthMask(GL_TRUE);
        if (cull) glEnable(GL_CULL_FACE);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_ALPHA_TEST);
    }
    void terrain(WorldScene& scene, const Terrain& ground, float camX, float camY, float camZ, float fwdX, float fwdY) {
        if (ground.xyz.empty() || ground.indices.empty()) return;
        const float nearX = camX < ground.minX ? ground.minX : camX > ground.maxX ? ground.maxX : camX;
        const float nearY = camY < ground.minY ? ground.minY : camY > ground.maxY ? ground.maxY : camY;
        const float tileDx = nearX - camX;
        const float tileDy = nearY - camY;
        const float reach = farYards + kChunkYards;
        if (tileDx * tileDx + tileDy * tileDy > reach * reach) return;
        const size_t verts = ground.xyz.size() / 3;
        auto see = [&](float minX, float minY, float maxX, float maxY, float reach) {
            const float cx = (minX + maxX) * 0.5f;
            const float cy = (minY + maxY) * 0.5f;
            const float dx = cx - camX;
            const float dy = cy - camY;
            if (dx * dx + dy * dy > reach * reach) return false;
            if (dx * fwdX + dy * fwdY < -50.f) return false;
            return true;
        };
        glDisable(GL_ALPHA_TEST);
        glDisable(GL_BLEND);
        glDepthFunc(GL_LEQUAL);
        glEnableClientState(GL_VERTEX_ARRAY);
        bool uploaded = false;
        if (poseBuffers()) {
            GroundGpu* slot = nullptr;
            for (GroundGpu& have : groundGpu)
                if (have.key == &ground) slot = &have;
            if (!slot) {
                if (groundGpu.size() < 48) {
                    groundGpu.push_back({});
                    slot = &groundGpu.back();
                } else {
                    slot = &groundGpu[groundGpuNext % groundGpu.size()];
                    ++groundGpuNext;
                    if (slot->xyzBuf) {
                        const GLuint drop[2] = {slot->xyzBuf, slot->uvBuf};
                        glDeleteBuffers_(2, drop);
                        slot->xyzBuf = 0;
                        slot->uvBuf = 0;
                    }
                }
                slot->key = &ground;
                slot->xyz = nullptr;
                slot->uv = nullptr;
            }
            if (!slot->xyzBuf) {
                GLuint ids[2] = {};
                glGenBuffers_(2, ids);
                slot->xyzBuf = ids[0];
                slot->uvBuf = ids[1];
            }
            if (slot->xyzBuf && (slot->xyz != ground.xyz.data() || slot->xyzN != ground.xyz.size())) {
                glBindBuffer_(kGlArrayBuffer, slot->xyzBuf);
                glBufferData_(kGlArrayBuffer, std::intptr_t(ground.xyz.size() * sizeof(float)), ground.xyz.data(), kGlStaticDraw);
                slot->xyz = ground.xyz.data();
                slot->xyzN = ground.xyz.size();
                slot->uv = nullptr;
            }
            if (slot->xyzBuf) {
                glBindBuffer_(kGlArrayBuffer, slot->xyzBuf);
                glVertexPointer(3, GL_FLOAT, 0, nullptr);
                uploaded = true;
                if (slot->uvBuf && ground.uv.size() == verts * 2) {
                    if (slot->uv != ground.uv.data()) {
                        glBindBuffer_(kGlArrayBuffer, slot->uvBuf);
                        glBufferData_(kGlArrayBuffer, std::intptr_t(ground.uv.size() * sizeof(float)), ground.uv.data(),
                                      kGlStaticDraw);
                        slot->uv = ground.uv.data();
                    }
                    glBindBuffer_(kGlArrayBuffer, slot->uvBuf);
                    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                    glTexCoordPointer(2, GL_FLOAT, 0, nullptr);
                } else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
                glBindBuffer_(kGlArrayBuffer, 0);
            }
        }
        if (!uploaded) {
            glVertexPointer(3, GL_FLOAT, 0, ground.xyz.data());
            if (ground.uv.size() == verts * 2) {
                glEnableClientState(GL_TEXTURE_COORD_ARRAY);
                glTexCoordPointer(2, GL_FLOAT, 0, ground.uv.data());
            } else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        const bool shaded = ground.shade.size() == verts;
        glDisableClientState(GL_COLOR_ARRAY);
        glColor4ub(255, 255, 255, 255);
        auto bindGround = [&](int image) {
            const Image* picture = scene.image(image);
            if (!picture) {
                glDisable(GL_TEXTURE_2D);
                return;
            }
            glEnable(GL_TEXTURE_2D);
            bindImage(picture, image);
        };
        auto drawRange = [&](const std::vector<uint32_t>& indices) {
            if (indices.size() < 3) return;
            glDrawElements(GL_TRIANGLES, GLsizei(indices.size()), GL_UNSIGNED_INT, indices.data());
        };
        if (ground.surfaces.empty()) {
            bindGround(-1);
            drawRange(ground.indices);
        }
        for (const Terrain::Surface& surface : ground.surfaces) {
            if (surface.indices.size() < 3) continue;
            terrainIndex.clear();
            if (surface.patches.empty()) {
                terrainIndex = surface.indices;
            } else {
                for (const Terrain::Surface::Patch& patch : surface.patches) {
                    if (patch.count < 3 || !see(patch.minX, patch.minY, patch.maxX, patch.maxY, reach)) continue;
                    const size_t end = std::min(surface.indices.size(), size_t(patch.first) + patch.count);
                    if (patch.first >= surface.indices.size()) continue;
                    terrainIndex.insert(terrainIndex.end(), surface.indices.begin() + patch.first,
                                        surface.indices.begin() + end);
                }
            }
            if (terrainIndex.size() < 3) continue;
            bindGround(surface.image);
            drawRange(terrainIndex);
        }
        const float coatReach = (sun.ok && sun.fogEnd > 1.f) ? sun.fogEnd + kChunkYards : reach;
        if (shaded && tileDx * tileDx + tileDy * tileDy <= coatReach * coatReach) {
            struct Merged {
                int image = -1;
                std::vector<uint32_t> indices;
                std::vector<const Terrain::Coat*> coats;
            };
            std::vector<Merged> merged;
            for (const Terrain::Coat& coat : ground.coats) {
                if (coat.indices.size() < 3 || coat.alpha.empty()) continue;
                if (!see(coat.minX, coat.minY, coat.maxX, coat.maxY, coatReach)) continue;
                Merged* batch = nullptr;
                for (Merged& have : merged)
                    if (have.image == coat.image) batch = &have;
                if (!batch) {
                    merged.push_back({});
                    batch = &merged.back();
                    batch->image = coat.image;
                }
                batch->indices.insert(batch->indices.end(), coat.indices.begin(), coat.indices.end());
                batch->coats.push_back(&coat);
            }
            if (!merged.empty()) {
            if (terrainColor.size() != verts * 4) terrainColor.assign(verts * 4, 255);
            else std::fill(terrainColor.begin(), terrainColor.end(), 255);
            glEnableClientState(GL_COLOR_ARRAY);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            for (Merged& batch : merged) {
                for (const Terrain::Coat* coat : batch.coats) {
                    for (size_t i = 0; i < coat->alpha.size() && coat->vertBegin + i < verts; ++i)
                        terrainColor[(coat->vertBegin + i) * 4 + 3] = coat->alpha[i];
                }
                glColorPointer(4, GL_UNSIGNED_BYTE, 0, terrainColor.data());
                bindGround(batch.image);
                drawRange(batch.indices);
                for (const Terrain::Coat* coat : batch.coats) {
                    for (size_t i = 0; i < coat->alpha.size() && coat->vertBegin + i < verts; ++i)
                        terrainColor[(coat->vertBegin + i) * 4 + 3] = 255;
                }
            }
            }
        }
        glDepthFunc(GL_LESS);
        glDisable(GL_BLEND);
        glDisableClientState(GL_COLOR_ARRAY);
        glEnable(GL_ALPHA_TEST);
        glEnable(GL_TEXTURE_2D);
    }
    void rect(float x, float y, float rw, float rh, unsigned rgb) {
        if (!hudRenderer) return;
        unsigned alpha = (rgb >> 24) & 255;
        if (alpha == 0) alpha = 255;
        SDL_SetRenderDrawColor(hudRenderer, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, alpha);
        const SDL_FRect box{x, y, rw, rh};
        SDL_RenderFillRect(hudRenderer, &box);
    }
    void flush() {}
    void text(float x, float y, const std::string& line, unsigned rgb = 0xFFECE2C6) {
        if (!hudRenderer || line.empty()) return;
        SDL_SetRenderDrawColor(hudRenderer, (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, 255);
        SDL_RenderDebugText(hudRenderer, x, y, line.c_str());
    }
    void blitHud() {
        if (!hudTex) return;
        glDisable(GL_FOG);
        glBindTexture(GL_TEXTURE_2D, hudTex);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0.0, double(w), double(h), 0.0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_ALPHA_TEST);
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glDisableClientState(GL_COLOR_ARRAY);
        glEnable(GL_TEXTURE_2D);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4ub(255, 255, 255, 255);
        glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f);
        glVertex2f(0.f, 0.f);
        glTexCoord2f(1.f, 0.f);
        glVertex2f(float(w), 0.f);
        glTexCoord2f(1.f, 1.f);
        glVertex2f(float(w), float(h));
        glTexCoord2f(0.f, 1.f);
        glVertex2f(0.f, float(h));
        glEnd();
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
    }
    void uploadHud() {
        if (!hud || !hudRenderer || !hud->pixels) return;
        SDL_RenderPresent(hudRenderer);
        if (!hudTex) glGenTextures(1, &hudTex);
        glBindTexture(GL_TEXTURE_2D, hudTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, hud->pitch / 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, hud->w, hud->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, hud->pixels);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        blitHud();
    }
    void compositeHud() { uploadHud(); }
    template <class DrawHud>
    void present(uint64_t stamp, DrawHud&& drawHud) {
        if (!hudReady || stamp != hudStamp) {
            wipeHud();
            drawHud();
            uploadHud();
            hudStamp = stamp;
            hudReady = true;
        } else {
            blitHud();
        }
        SDL_GL_SwapWindow(window);
    }
    bool save(const std::string& path) {
        compositeHud();
        std::vector<uint8_t> pixels(size_t(w) * size_t(h) * 4);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        SDL_Surface* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
        if (!surface) return false;
        for (int y = 0; y < h; ++y) {
            std::memcpy(static_cast<uint8_t*>(surface->pixels) + size_t(y) * surface->pitch,
                        pixels.data() + size_t(h - 1 - y) * w * 4, size_t(w) * 4);
        }
        const bool ok = SDL_SaveBMP(surface, path.c_str());
        SDL_DestroySurface(surface);
        SDL_GL_SwapWindow(window);
        return ok;
    }
};

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

bool loadTile(const std::string& root, uint32_t map, float x, float y, Terrain& ground) {
    const char* dir = mapDirectory(map);
    if (!dir) return false;
    const int tileX = tileIndex(y);
    const int tileY = tileIndex(x);
    ArchiveSet archives(root.empty() ? std::string(R"(D:\home\World of Warcraft)") : root);
    std::vector<uint8_t> bytes;
    const std::string path = std::string("World\\Maps\\") + dir + "\\" + dir + "_" + std::to_string(tileX) + "_" +
                             std::to_string(tileY) + ".adt";
    if (!archives.read(path, bytes)) return false;
    ground = Terrain{};
    return loadAdt(bytes.data(), bytes.size(), tileX, tileY, ground);
}

std::string modelFile(std::string path) {
    for (char& c : path)
        if (c == '/') c = '\\';
    if (path.size() >= 4) {
        const std::string ext = path.substr(path.size() - 4);
        if (ext == ".mdx" || ext == ".MDX") path.replace(path.size() - 4, 4, ".m2");
    }
    return path;
}

struct ModelCatalog {
    bool ready = false;
    std::unordered_map<uint32_t, uint32_t> displayToModel;
    std::unordered_map<uint32_t, float> displayScale;
    std::unordered_map<uint32_t, std::string> displaySkin;
    struct Appearance {
        uint32_t race = 0, gender = 0, skin = 0, face = 0, hair = 0, color = 0, facial = 0;
        uint32_t gear[11]{};
    };
    std::unordered_map<uint32_t, Appearance> appearance;
    Dbc charSections;
    Dbc hairGeosets;
    Dbc facialStyles;
    Dbc itemDisplays;
    std::unordered_map<uint32_t, uint32_t> itemToDisplay;
    std::unordered_map<uint32_t, uint32_t> displayRow;
    std::unordered_map<uint64_t, CharacterLook> looks;
    std::unordered_map<uint32_t, std::string> modelPath;
    std::unordered_map<uint32_t, std::string> objectPath;
    std::unordered_map<std::string, Mesh> meshes;
    struct Zone {
        uint32_t map = 0;
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
        std::string name;
    };
    std::vector<Zone> zones;

    void load(const std::string& root) {
        if (ready) return;
        ready = true;
        ArchiveSet archives(root.empty() ? std::string(R"(D:\home\World of Warcraft)") : root);
        std::vector<uint8_t> bytes;
        Dbc displays;
        Dbc models;
        Dbc objects;
        if (archives.read("DBFilesClient\\CreatureDisplayInfo.dbc", bytes)) displays.load(bytes.data(), bytes.size());
        Dbc extras;
        std::unordered_map<uint32_t, Appearance> byExtra;
        if (archives.read("DBFilesClient\\CreatureDisplayInfoExtra.dbc", bytes) && extras.load(bytes.data(), bytes.size())) {
            for (uint32_t row = 0; row < extras.records; ++row) {
                Appearance item;
                item.race = extras.get(row, 1);
                item.gender = extras.get(row, 2);
                item.skin = extras.get(row, 3);
                item.face = extras.get(row, 4);
                item.hair = extras.get(row, 5);
                item.color = extras.get(row, 6);
                item.facial = extras.get(row, 7);
                for (int i = 0; i < 11; ++i) item.gear[i] = extras.get(row, 8 + i);
                byExtra[extras.get(row, 0)] = item;
            }
        }
        if (archives.read("DBFilesClient\\CharSections.dbc", bytes)) charSections.load(bytes.data(), bytes.size());
        if (archives.read("DBFilesClient\\CharHairGeosets.dbc", bytes)) hairGeosets.load(bytes.data(), bytes.size());
        if (archives.read("DBFilesClient\\CharacterFacialHairStyles.dbc", bytes))
            facialStyles.load(bytes.data(), bytes.size());
        if (archives.read("DBFilesClient\\ItemDisplayInfo.dbc", bytes) && itemDisplays.load(bytes.data(), bytes.size())) {
            for (uint32_t row = 0; row < itemDisplays.records; ++row) displayRow[itemDisplays.get(row, 0)] = row;
        }
        Dbc items;
        if (archives.read("DBFilesClient\\Item.dbc", bytes) && items.load(bytes.data(), bytes.size())) {
            for (uint32_t row = 0; row < items.records; ++row) itemToDisplay[items.get(row, 0)] = items.get(row, 1);
        }
        if (archives.read("DBFilesClient\\CreatureModelData.dbc", bytes)) models.load(bytes.data(), bytes.size());
        if (archives.read("DBFilesClient\\GameObjectDisplayInfo.dbc", bytes)) objects.load(bytes.data(), bytes.size());
        Dbc areas;
        if (archives.read("DBFilesClient\\WorldMapArea.dbc", bytes)) areas.load(bytes.data(), bytes.size());
        for (uint32_t row = 0; row < models.records; ++row) {
            const uint32_t id = models.get(row, 0);
            const std::string path = modelFile(models.text(models.get(row, 2)));
            if (!path.empty()) modelPath[id] = path;
        }
        for (uint32_t row = 0; row < displays.records; ++row) {
            const uint32_t id = displays.get(row, 0);
            displayToModel[id] = displays.get(row, 1);
            float scale = 1.f;
            const uint32_t bits = displays.get(row, 4);
            std::memcpy(&scale, &bits, sizeof(scale));
            if (scale > 0.05f && scale < 20.f) displayScale[id] = scale;
            for (uint32_t field = 6; field <= 8; ++field) {
                const std::string skin = modelFile(displays.text(displays.get(row, field)));
                if (skin.empty()) continue;
                displaySkin[id] = skin;
                break;
            }
            const uint32_t extraId = displays.get(row, 3);
            const auto extra = byExtra.find(extraId);
            if (extraId != 0 && extra != byExtra.end()) appearance[id] = extra->second;
        }
        for (uint32_t row = 0; row < objects.records; ++row) {
            const std::string path = modelFile(objects.text(objects.get(row, 1)));
            if (!path.empty()) objectPath[objects.get(row, 0)] = path;
        }
        for (uint32_t row = 0; row < areas.records; ++row) {
            Zone zone;
            zone.map = areas.get(row, 1);
            zone.name = areas.text(areas.get(row, 3));
            auto axis = [](uint32_t bits) {
                float value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            };
            const float yA = axis(areas.get(row, 4));
            const float yB = axis(areas.get(row, 5));
            const float xA = axis(areas.get(row, 6));
            const float xB = axis(areas.get(row, 7));
            zone.x0 = std::min(xA, xB);
            zone.x1 = std::max(xA, xB);
            zone.y0 = std::min(yA, yB);
            zone.y1 = std::max(yA, yB);
            if (!zone.name.empty()) zones.push_back(std::move(zone));
        }
    }

    CharacterLook lookUp(uint32_t race, uint32_t gender, uint32_t skin, uint32_t face, uint32_t style, uint32_t color,
                         uint32_t facial) {
        const uint64_t key = uint64_t(race & 255u) | (uint64_t(gender & 255u) << 8) | (uint64_t(skin & 255u) << 16) |
                             (uint64_t(face & 255u) << 24) | (uint64_t(style & 255u) << 32) | (uint64_t(color & 255u) << 40) |
                             (uint64_t(facial & 255u) << 48);
        const auto found = looks.find(key);
        if (found != looks.end()) return found->second;
        CharacterLook look;
        characterLook(charSections, hairGeosets, facialStyles, race, gender, skin, face, style, color, facial, look);
        looks.emplace(key, look);
        return look;
    }

    EquipVisual visualOf(uint32_t displayId, int slot) const {
        EquipVisual vis;
        const auto row = displayRow.find(displayId);
        if (row == displayRow.end()) return vis;
        vis.slot = slot;
        vis.geoset[0] = itemDisplays.get(row->second, 7);
        vis.geoset[1] = itemDisplays.get(row->second, 8);
        vis.geoset[2] = itemDisplays.get(row->second, 9);
        for (int i = 0; i < 8; ++i) vis.component[i] = itemDisplays.text(itemDisplays.get(row->second, 15 + i));
        if (slot == 2) {
            auto file = [](std::string name, bool mesh) {
                if (name.empty()) return name;
                for (char& c : name)
                    if (c == '/') c = '\\';
                if (name.find('\\') == std::string::npos) name = "Item\\ObjectComponents\\Shoulder\\" + name;
                const auto ends = [&](const char* ext) {
                    const size_t n = std::strlen(ext);
                    return name.size() >= n && name.compare(name.size() - n, n, ext) == 0;
                };
                if (ends(".mdx") || ends(".MDX")) name.replace(name.size() - 4, 4, mesh ? ".m2" : ".blp");
                else if (mesh && !ends(".m2") && !ends(".M2")) name += ".m2";
                else if (!mesh && !ends(".blp") && !ends(".BLP")) name += ".blp";
                return name;
            };
            vis.modelLeft = file(itemDisplays.text(itemDisplays.get(row->second, 1)), true);
            vis.modelRight = file(itemDisplays.text(itemDisplays.get(row->second, 2)), true);
            vis.textureLeft = file(itemDisplays.text(itemDisplays.get(row->second, 3)), false);
            vis.textureRight = file(itemDisplays.text(itemDisplays.get(row->second, 4)), false);
            if (vis.textureLeft.empty()) vis.textureLeft = vis.textureRight;
            if (vis.textureRight.empty()) vis.textureRight = vis.textureLeft;
        }
        if (slot == 14) {
            std::string name = itemDisplays.text(itemDisplays.get(row->second, 3));
            if (name.empty()) name = itemDisplays.text(itemDisplays.get(row->second, 4));
            for (char& c : name)
                if (c == '/') c = '\\';
            if (!name.empty() && name.find('\\') == std::string::npos) name = "Item\\ObjectComponents\\Cape\\" + name;
            if (!name.empty()) {
                const bool blp = name.size() >= 4 && (name.compare(name.size() - 4, 4, ".blp") == 0 ||
                                                       name.compare(name.size() - 4, 4, ".BLP") == 0);
                if (!blp) name += ".blp";
            }
            vis.cape = std::move(name);
        }
        return vis;
    }

    struct WornLook {
        CharacterLook look;
        std::string shoulderLeft;
        std::string shoulderRight;
        std::string shoulderTexLeft;
        std::string shoulderTexRight;
        bool ok = false;
    };
    struct PlayerKey {
        uint32_t bytes0 = 0;
        uint32_t bytes = 0;
        uint32_t bytes2 = 0;
        uint32_t item[19]{};
        bool operator==(const PlayerKey& other) const {
            return bytes0 == other.bytes0 && bytes == other.bytes && bytes2 == other.bytes2 &&
                   std::memcmp(item, other.item, sizeof(item)) == 0;
        }
    };
    struct PlayerKeyHash {
        size_t operator()(const PlayerKey& key) const {
            size_t h = key.bytes0 ^ (size_t(key.bytes) << 1) ^ (size_t(key.bytes2) << 2);
            for (uint32_t id : key.item) h = h * 131u + id;
            return h;
        }
    };
    std::unordered_map<uint32_t, WornLook> npcWorn;
    std::unordered_map<PlayerKey, WornLook, PlayerKeyHash> playerWorn;

    void fillWorn(WornLook& worn, const EquipVisual* pieces, int count) {
        if (count > 0 && worn.look.ok) dressCharacter(pieces, count, worn.look);
        for (int i = 0; i < count; ++i) {
            if (pieces[i].slot != 2) continue;
            worn.shoulderLeft = pieces[i].modelLeft;
            worn.shoulderRight = pieces[i].modelRight;
            worn.shoulderTexLeft = pieces[i].textureLeft;
            worn.shoulderTexRight = pieces[i].textureRight;
        }
        worn.ok = worn.look.ok;
    }

    const WornLook* npcOutfit(uint32_t display) {
        const auto found = npcWorn.find(display);
        if (found != npcWorn.end()) return &found->second;
        WornLook worn;
        const auto face = appearance.find(display);
        if (face != appearance.end() && face->second.race != 0) {
            const Appearance& item = face->second;
            worn.look = lookUp(item.race, item.gender, item.skin, item.face, item.hair, item.color, item.facial);
            EquipVisual pieces[11];
            int count = 0;
            for (int slot = 0; slot < 11; ++slot) {
                if (item.gear[slot] == 0) continue;
                EquipVisual vis = visualOf(item.gear[slot], slot);
                if (vis.slot >= 0) pieces[count++] = std::move(vis);
            }
            fillWorn(worn, pieces, count);
        }
        return &npcWorn.emplace(display, std::move(worn)).first->second;
    }

    const WornLook* playerOutfit(const PlayerKey& key) {
        const auto found = playerWorn.find(key);
        if (found != playerWorn.end()) return &found->second;
        if (playerWorn.size() > 128) playerWorn.clear();
        WornLook worn;
        const uint32_t race = key.bytes0 & 255u;
        const uint32_t gender = (key.bytes0 >> 16) & 255u;
        if (race != 0) {
            worn.look = lookUp(race, gender, key.bytes & 255u, (key.bytes >> 8) & 255u, (key.bytes >> 16) & 255u,
                               (key.bytes >> 24) & 255u, key.bytes2 & 255u);
            EquipVisual pieces[19];
            int count = 0;
            for (int slot = 0; slot < 19; ++slot) {
                if (key.item[slot] == 0) continue;
                const auto shown = itemToDisplay.find(key.item[slot]);
                if (shown == itemToDisplay.end()) continue;
                EquipVisual vis = visualOf(shown->second, slot);
                if (vis.slot >= 0) pieces[count++] = std::move(vis);
            }
            fillWorn(worn, pieces, count);
        }
        return &playerWorn.emplace(key, std::move(worn)).first->second;
    }

    std::string zoneAt(uint32_t map, float x, float y) const {
        std::string best;
        float bestArea = 1.0e20f;
        for (const Zone& zone : zones) {
            if (zone.map != map || x < zone.x0 || x > zone.x1 || y < zone.y0 || y > zone.y1) continue;
            const float area = (zone.x1 - zone.x0) * (zone.y1 - zone.y0);
            if (area < bestArea) {
                bestArea = area;
                best = zone.name;
            }
        }
        return best;
    }

    const Mesh* meshFor(ArchiveSet& archives, const std::string& path) {
        const auto found = meshes.find(path);
        if (found != meshes.end()) return found->second.indices.empty() ? nullptr : &found->second;
        Mesh mesh;
        std::vector<uint8_t> bytes;
        if (archives.read(path, bytes)) {
            if (path.size() >= 4 && path.substr(path.size() - 4) == ".wmo") loadWmo(bytes.data(), bytes.size(), mesh);
            else loadM2(bytes.data(), bytes.size(), mesh);
        }
        if (mesh.indices.empty() && path.size() > 4 && path.substr(path.size() - 4) == ".wmo") {
            std::string group = path;
            group.insert(group.size() - 4, "_000");
            if (archives.read(group, bytes)) loadWmo(bytes.data(), bytes.size(), mesh);
        }
        meshes[path] = std::move(mesh);
        return meshes[path].indices.empty() ? nullptr : &meshes[path];
    }
};

struct Cam {
    float x = 0, y = 0, z = 0, o = 0, pitch = 0;
};

}  // namespace

struct Speaker {
    SDL_AudioStream* stream = nullptr;
    SoundBank* bank = nullptr;
    ArchiveSet* archives = nullptr;
    std::unordered_map<std::string, std::shared_ptr<PcmStream>> streams;
    struct Voice {
        std::shared_ptr<std::vector<int16_t>> pcm;
        std::shared_ptr<PcmStream> feed;
        std::string path;
        size_t at = 0;
        float volume = 1.f;
        float gain = 1.f;
        float gainTo = 1.f;
        float fadeStep = 0.f;
        int fadeSamples = 0;
        bool loop = false;
        bool music = false;
        bool retire = false;
    };
    struct Hold {
        std::string path;
        float volume = 1.f;
        bool loop = false;
        bool music = false;
        bool fadeIn = false;
    };
    std::vector<Voice> voices;
    std::vector<Hold> waiting;
    std::vector<int32_t> mix;
    std::vector<int16_t> packed;
    uint32_t area = 0;
    bool havePlan = false;
    uint32_t musicId = 0;
    uint32_t ambienceId = 0;
    uint32_t wantMusic = 0xffffffffu;
    uint32_t wantAmb = 0xffffffffu;
    uint32_t wantSince = 0;
    uint32_t silenceMin = 0;
    uint32_t silenceMax = 0;
    uint32_t silenceFrames = 0;
    float musicVolume = 0.8f;
    std::vector<std::string> tracks;
    std::vector<std::string> intro;
    size_t introAt = 0;
    size_t lastTrack = 0;
    bool introOn = false;
    uint32_t rng = 0x6d2b79f5u;
    ~Speaker() { close(); }
    void close() {
        if (stream) SDL_DestroyAudioStream(stream);
        stream = nullptr;
    }
    void bind(SoundBank& sounds, ArchiveSet& files) {
        bank = &sounds;
        archives = &files;
    }
    bool open() {
        if (stream) return true;
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
        SDL_AudioSpec spec{};
        spec.freq = 44100;
        spec.format = SDL_AUDIO_S16LE;
        spec.channels = 2;
        stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
        if (!stream) return false;
        SDL_ResumeAudioStreamDevice(stream);
        return true;
    }
    std::shared_ptr<PcmStream> openStream(const std::string& path) {
        const auto found = streams.find(path);
        if (found != streams.end()) return found->second;
        auto stream = std::make_shared<PcmStream>();
        if (!archives || !stream->start(*archives, path)) return nullptr;
        streams.emplace(path, stream);
        return stream;
    }
    void decodeSome() {
        for (const auto& item : streams) {
            if (!item.second || item.second->done()) continue;
            item.second->pump(8);
            break;
        }
    }
    uint32_t nextRand() {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    }
    static constexpr int kFade = 44100 * 3 / 2;
    void armFade(Voice& voice, float to, int samples) {
        voice.gainTo = to;
        if (samples <= 0) {
            voice.gain = to;
            voice.fadeSamples = 0;
            voice.fadeStep = 0.f;
            return;
        }
        voice.fadeSamples = samples;
        voice.fadeStep = (to - voice.gain) / float(samples);
    }
    void fadeMusic() {
        for (Voice& voice : voices)
            if (voice.music && !voice.retire) {
                voice.retire = true;
                armFade(voice, 0.f, kFade);
            }
        waiting.erase(std::remove_if(waiting.begin(), waiting.end(), [](const Hold& hold) { return hold.music; }), waiting.end());
        silenceFrames = 0;
        introOn = false;
        introAt = 0;
    }
    void startFile(const std::string& path, float volume, bool loop, bool music, bool fadeIn) {
        std::shared_ptr<PcmStream> feed = openStream(path);
        if (!feed) return;
        if (!feed->ready()) feed->pump(32);
        if (!feed->ready()) {
            for (const Hold& hold : waiting)
                if (hold.path == path && hold.music == music && hold.loop == loop) return;
            waiting.push_back({path, volume, loop, music, fadeIn});
            return;
        }
        Voice voice;
        voice.pcm = feed->samples();
        voice.feed = std::move(feed);
        voice.path = path;
        voice.volume = volume;
        voice.loop = loop;
        voice.music = music;
        voice.gain = fadeIn ? 0.f : 1.f;
        if (fadeIn) armFade(voice, 1.f, kFade);
        voices.push_back(std::move(voice));
    }
    void startTrack(bool fadeIn) {
        if (tracks.empty()) return;
        size_t pick = tracks.size() == 1 ? 0 : nextRand() % tracks.size();
        if (tracks.size() > 1) {
            for (int guard = 0; guard < 4 && pick == lastTrack; ++guard) pick = nextRand() % tracks.size();
        }
        lastTrack = pick;
        startFile(tracks[pick], musicVolume, false, true, fadeIn);
        introOn = false;
    }
    void cue(const std::string& name) {
        if (!bank || !open()) return;
        SoundBank::Piece piece = bank->cue(name);
        if (piece.files.empty()) {
            const bool file = name.size() >= 4 && (name.find(".mp3") != std::string::npos || name.find(".wav") != std::string::npos);
            if (!file) return;
            piece.files.push_back(name);
        }
        int shots = 0;
        for (const Voice& voice : voices)
            if (!voice.music && !voice.loop) ++shots;
        if (shots >= 8) return;
        startFile(piece.files[nextRand() % piece.files.size()], piece.volume, false, false, false);
    }
    void setAmbience(const SoundBank::Piece& piece) {
        const std::string path = piece.files.empty() ? std::string() : piece.files[0];
        bool playing = false;
        for (Voice& voice : voices) {
            if (voice.music || !voice.loop) continue;
            if (!path.empty() && voice.path == path && !voice.retire) {
                playing = true;
                continue;
            }
            voice.retire = true;
            armFade(voice, 0.f, kFade);
        }
        waiting.erase(std::remove_if(waiting.begin(), waiting.end(),
                                     [](const Hold& hold) { return hold.loop && !hold.music; }),
                      waiting.end());
        if (!playing && !path.empty()) startFile(path, piece.volume, true, false, true);
    }
    void commit(const SoundBank::Plan& plan) {
        const bool musicChanged = !havePlan || plan.musicId != musicId;
        const bool ambChanged = !havePlan || plan.ambienceId != ambienceId;
        bool live = false;
        if (musicChanged) {
            for (const Voice& voice : voices)
                if (voice.music && !voice.retire) live = true;
            fadeMusic();
        }
        havePlan = true;
        area = plan.area;
        if (musicChanged) {
            musicId = plan.musicId;
            tracks = plan.music.files;
            intro = plan.intro.files;
            silenceMin = plan.silenceMinMs;
            silenceMax = plan.silenceMaxMs;
            musicVolume = plan.music.volume > 0.f ? plan.music.volume * 0.8f : 0.8f;
            if (!intro.empty()) {
                introOn = true;
                introAt = 0;
                startFile(intro[0], plan.intro.volume > 0.f ? plan.intro.volume : 0.8f, false, true, live);
            } else if (!tracks.empty()) {
                startTrack(live);
            }
        }
        if (ambChanged) {
            ambienceId = plan.ambienceId;
            setAmbience(plan.ambience);
        }
    }
    void apply(const SoundBank::Plan& plan) {
        if (!bank || !open()) return;
        if (!havePlan) {
            commit(plan);
            return;
        }
        if (plan.musicId == musicId && plan.ambienceId == ambienceId) {
            area = plan.area;
            wantMusic = 0xffffffffu;
            return;
        }
        if (plan.musicId != wantMusic || plan.ambienceId != wantAmb) {
            wantMusic = plan.musicId;
            wantAmb = plan.ambienceId;
            wantSince = SDL_GetTicks();
            return;
        }
        if (SDL_GetTicks() - wantSince < 250u) return;
        wantMusic = 0xffffffffu;
        commit(plan);
    }
    void pump() {
        if (!stream) return;
        decodeSome();
        if (!waiting.empty()) {
            std::vector<Hold> pending;
            pending.swap(waiting);
            for (const Hold& hold : pending) startFile(hold.path, hold.volume, hold.loop, hold.music, hold.fadeIn);
        }
        const int queued = SDL_GetAudioStreamQueued(stream);
        const int target = 44100 * 4 * 120 / 1000;
        if (queued >= target) return;
        int frames = (target - queued) / 4;
        if (frames < 1) return;
        if (frames > 44100 / 4) frames = 44100 / 4;
        mix.assign(size_t(frames) * 2, 0);
        bool musicEnded = false;
        for (Voice& voice : voices) {
            if (!voice.pcm || voice.pcm->empty()) continue;
            const std::vector<int16_t>& samples = *voice.pcm;
            const bool filling = voice.feed && !voice.feed->done();
            for (int i = 0; i < frames; ++i) {
                if (voice.fadeSamples > 0) {
                    voice.gain += voice.fadeStep;
                    if (--voice.fadeSamples == 0) voice.gain = voice.gainTo;
                }
                if (voice.at + 1 >= samples.size()) {
                    if (filling) break;
                    if (voice.loop && !voice.retire) voice.at = 0;
                    else {
                        if (voice.music && !voice.retire) musicEnded = true;
                        voice.pcm.reset();
                        break;
                    }
                }
                const float gain = voice.volume * voice.gain;
                mix[size_t(i) * 2] += int32_t(samples[voice.at] * gain);
                mix[size_t(i) * 2 + 1] += int32_t(samples[voice.at + 1] * gain);
                voice.at += 2;
            }
            if (voice.retire && voice.fadeSamples == 0) voice.pcm.reset();
        }
        voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice& voice) { return !voice.pcm; }), voices.end());
        if (musicEnded) {
            if (introOn && introAt + 1 < intro.size()) {
                ++introAt;
                startFile(intro[introAt], 0.8f, false, true, false);
            } else if (introOn) {
                introOn = false;
                startTrack(false);
            } else if (silenceMax > 0) {
                uint32_t span = silenceMin;
                if (silenceMax > silenceMin) span += nextRand() % (silenceMax - silenceMin + 1);
                silenceFrames = 44100u * span / 1000u;
            } else {
                startTrack(false);
            }
        } else if (silenceFrames > 0) {
            const uint32_t step = uint32_t(frames);
            silenceFrames = silenceFrames > step ? silenceFrames - step : 0;
            if (silenceFrames == 0) startTrack(false);
        }
        packed.resize(size_t(frames) * 2);
        for (size_t i = 0; i < packed.size(); ++i) {
            const int32_t sample = mix[i];
            packed[i] = int16_t(sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample);
        }
        SDL_PutAudioStreamData(stream, packed.data(), int(packed.size() * sizeof(int16_t)));
    }
};

int runWindow(const Config& config, bool shot) {
    Draw draw;
    if (!draw.open(shot)) {
        SDL_Log("renderer: %s", SDL_GetError());
        return 1;
    }
    diag::configure(config.diagnostics, config.diagnosticsStrict, "diag.log");
    if (diag::enabled()) diag::event("render.context", "opengl window");
    Cam cam{-8949.95f, -132.49f, 140.f, 0.9f, 0.45f};
    FrameXml ui;
    std::string said;
    ui.setChatter([&](const std::string& text) { said = text; });
    uint64_t targetGuid = 0;
    bool pickClick = false;
    float pickX = 0, pickY = 0;
    std::string channelsFor;
    std::vector<std::string> chatChannels;
    if (!shot) SDL_StartTextInput(draw.window);
    const std::string clientRoot = config.clientPath.empty() ? std::string(R"(D:\home\World of Warcraft)") : config.clientPath;
    const VideoSettings video = readVideo(clientRoot);
    draw.farYards = video.farClip;
    draw.nearYards = video.nearClip;
    LightTable lights;

    if (shot) {
        ArchiveSet archives(clientRoot);
        WorldScene scene;
        scene.setFarClip(video.farClip);
        lights.load(archives);
        const float fx = -4918.f, fy = -917.f, fz = 501.f;
        for (int step = 0; step < 120; ++step) {
            scene.flushLoad();
            if (!scene.advance(archives, 0, fx, fy, fz)) break;
        }
        scene.flushLoad();
        bool saved = true;
        for (int view = 0; view < 4; ++view) {
            const float yaw = view * 1.5708f;
            const float pitch = 0.12f;
            const float dist = 6.f;
            Camera viewCam;
            viewCam.x = fx - std::cos(yaw) * dist * std::cos(pitch);
            viewCam.y = fy - std::sin(yaw) * dist * std::cos(pitch);
            viewCam.z = fz + 1.7f + std::sin(pitch) * dist;
            viewCam.o = yaw;
            viewCam.pitch = pitch;
            if (lights.ready() && video.localTime)
                draw.sun = lights.at(0, viewCam.x, viewCam.y, viewCam.z, halfMinuteNow());
            if (draw.sun.ok) {
                draw.clearR = draw.sun.fog[0];
                draw.clearG = draw.sun.fog[1];
                draw.clearB = draw.sun.fog[2];
            }
            draw.begin();
            draw.wipeHud();
            draw.look(viewCam, video.horizon);
            draw.sky(viewCam);
            draw.look(viewCam, video.farClip);
            draw.fogOn();
            if (scene.showTerrain()) {
                std::vector<const Terrain*> tiles;
                scene.terrains(tiles);
                for (const Terrain* tile : tiles)
                    draw.terrain(scene, *tile, viewCam.x, viewCam.y, viewCam.z, std::cos(viewCam.o), std::sin(viewCam.o));
            }
            std::vector<WorldScene::Drawable> items;
            scene.drawables(items);
            for (const WorldScene::Drawable& item : items) draw.lit(scene, *item.mesh, item.world);
            draw.text(16, 16, scene.status() + "  view " + std::to_string(view));
            const std::string path = std::string(R"(D:\home\cursor\wow\reclient\build\ironforge)") + std::to_string(view) + ".bmp";
            saved = draw.save(path) && saved;
        }
        draw.close();
        return saved ? 0 : 1;
    }

    std::unique_ptr<GameClient> client;
    ui.setPicker([&](int index) {
        if (client) client->enqueue("option " + std::to_string(index));
    });
    std::string account = config.account.empty() ? "PLAYER" : config.account;
    std::string password = config.password;
    std::string character = config.character;
    std::string chat;
    std::string status = "Enter account, password, and character. Press Enter to connect.";
    int field = 0;
    bool chatFocus = false;
    uint32_t gossipSeen = 0;
    uint32_t chatSeen = 0;
    std::string uiProblem;
    bool wHeld = false, aHeld = false, sHeld = false, dHeld = false, qHeld = false, eHeld = false;
    bool walkMode = false;
    // Forward is the run the server granted. Backward uses the slower back speed.
    // Walk mode is the whole step at the walk speed, which is what the walk clip covers.
    auto movePace = [](const Movement& motion, bool walking, float forward) {
        const float walk = motion.walk > 0.2f && motion.walk < 4.f ? motion.walk : 2.5f;
        if (walking) return walk;
        if (forward < 0.f) return motion.runBack > 2.f && motion.runBack < 8.f ? motion.runBack : 4.5f;
        return motion.run > 4.f && motion.run < 14.f ? motion.run : 7.f;
    };
    bool jumpRequested = false;
    bool quit = false;
    bool joining = false;
    bool looking = false;
    bool rightDown = false;
    bool rightDrag = false;
    float rightX = 0, rightY = 0;
    bool talkClick = false;
    float camYaw = 0.9f;
    float camPitch = 0.18f;
    float camDist = 8.f;
    float camApplied = 8.f;
    float camClipNear = 8.f;
    bool camClipReady = false;
    bool cameraAimed = false;
    ModelCatalog catalog;
    std::unique_ptr<ArchiveSet> assets;
    WorldScene scene;
    scene.setFarClip(video.farClip);
    std::string focusName;
    uint32_t focusMap = 0;
    float focusX = 0, focusY = 0, focusZ = 0;
    uint64_t lastMove = 0;
    std::unordered_set<uint64_t> heldGuids;
    struct Glide {
        uint32_t splineId = 0xffffffffu;
        uint32_t started = 0;
        uint32_t duration = 0;
        float x = 0, y = 0, z = 0, o = 0;
        float fromX = 0, fromY = 0, fromZ = 0;
        float toX = 0, toY = 0, toZ = 0;
        float serverX = 0, serverY = 0, serverZ = 0;
        bool seen = false;
        bool following = false;
        float animMs = 0;
    };
    std::unordered_map<uint64_t, Glide> glides;
    struct KeptModel {
        uint32_t display = 0xffffffffu;
        uint32_t items = 0;
        int model = -1;
    };
    std::unordered_map<uint64_t, KeptModel> keptModels;
    std::unordered_map<std::string, int> padModels;
    std::string zoneName;
    int zoneX = 1 << 30;
    int zoneY = 1 << 30;
    uint32_t zoneMap = 0xffffffffu;
    float frameDt = 0;
    float selfLoco = 0;
    LitMesh selfMesh;
    int selfMeshModel = -1;
    uint64_t lastFrame = SDL_GetTicks();
    float smoothX = 0, smoothY = 0, smoothZ = 0;
    float pendingDx = 0, pendingDy = 0;
    float sentZ = 0;
    bool smoothReady = false;
    bool airborne = false;
    float verticalSpeed = 0;
    uint64_t fpsWindow = SDL_GetTicks();
    int fpsFrames = 0;
    int fpsShown = 0;
    uint64_t diagSecond = 0;
    uint64_t worldSince = 0;
    float diagMeters = 0, diagMoveTime = 0, diagMaxStep = 0;
    float diagWAlong = 0, diagWLen = 0, diagARight = 0, diagALen = 0, diagDRight = 0, diagDLen = 0;
    const float fieldX = 28.f;
    const float fieldW = 400.f;
    const float fieldTop[] = {42.f, 96.f, 150.f};

    bool uiWorld = false;
    bool showUi = true;
    SoundBank sound;
    Speaker speaker;
    auto toPixel = [&](float x, float y, float& ox, float& oy) {
        int ww = 0, wh = 0;
        SDL_GetWindowSize(draw.window, &ww, &wh);
        ox = ww > 0 ? x * float(draw.w) / float(ww) : x;
        oy = wh > 0 ? y * float(draw.h) / float(wh) : y;
    };
    if (!config.clientPath.empty()) {
        assets = std::make_unique<ArchiveSet>(config.clientPath);
        catalog.load(config.clientPath);
        if (sound.load(*assets)) {
            speaker.bind(sound, *assets);
            speaker.open();
            ui.setSounder([&](const std::string& name) { speaker.cue(name); });
        }
        ui.useClient(assets.get(), config.clientPath);
        draw.begin();
        ui.layout(float(draw.w), float(draw.h));
        const int files = ui.loadStockUi();
        const std::string dialogs = ui.prepareDialogs();
        if (!dialogs.empty()) status = dialogs;
        if (diag::enabled())
            diag::event("ui.stock", std::to_string(files) + " files, " + std::to_string(ui.widgets().size()) + " widgets");
    }
    try {
    int laidW = 0;
    int laidH = 0;
    while (!quit) {
        auto sendChat = [&](std::string text) {
            if (!client) return;
            auto trimEnd = [](std::string value) {
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
                while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) value.pop_back();
                return value;
            };
            text = trimEnd(text);
            if (text.empty()) return;
            if (text.rfind("channel\t", 0) == 0) {
                client->enqueue("channel " + text.substr(8));
                return;
            }
            if (text.rfind("yell\t", 0) == 0) {
                client->enqueue("yell " + text.substr(5));
                return;
            }
            if (text.rfind("say\t", 0) == 0) text = trimEnd(text.substr(4));
            if (text.rfind("/join ", 0) == 0) client->enqueue("join " + trimEnd(text.substr(6)));
            else if (text.rfind("/y ", 0) == 0) client->enqueue("yell " + trimEnd(text.substr(3)));
            else if (text.rfind("/s ", 0) == 0) client->enqueue("say " + trimEnd(text.substr(3)));
            else if (text.size() >= 3 && text[0] == '/' && text[1] >= '1' && text[1] <= '9' && text[2] == ' ') {
                const int index = text[1] - '1';
                if (index >= 0 && index < int(chatChannels.size()))
                    client->enqueue("channel " + chatChannels[size_t(index)] + "|" + trimEnd(text.substr(3)));
            } else client->enqueue("say " + text);
        };
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) quit = true;
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                const bool playing = joining || (client && client->view().inWorld);
                if (event.button.button == SDL_BUTTON_RIGHT && playing) {
                    // A still right click talks to an NPC. A drag turns the camera.
                    rightDown = true;
                    rightDrag = false;
                    rightX = event.button.x;
                    rightY = event.button.y;
                } else if (!playing) {
                    for (int i = 0; i < 3; ++i) {
                        if (event.button.x >= fieldX && event.button.x <= fieldX + fieldW && event.button.y >= fieldTop[i] &&
                            event.button.y <= fieldTop[i] + 26.f)
                            field = i;
                    }
                } else if (event.button.button == SDL_BUTTON_LEFT && !looking) {
                    float x = 0, y = 0;
                    toPixel(event.button.x, event.button.y, x, y);
                    const bool ate = showUi && ui.click(x, y);
                    if (!ate) {
                        pickX = x;
                        pickY = y;
                        pickClick = true;
                    }
                }
                if (client) {
                    const GameClient::View view = client->view();
                    float y = 234.f;
                    for (const Character& c : view.characters) {
                        if (event.button.x >= 16 && event.button.x <= 420 && event.button.y >= y &&
                            event.button.y <= y + 12) {
                            client->enqueue("enter " + c.name);
                            joining = true;
                            focusName = c.name;
                            focusMap = c.map;
                            focusX = c.x;
                            focusY = c.y;
                            focusZ = c.z;
                            status = "entering " + c.name;
                        }
                        y += 12;
                    }
                }
            } else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                const SDL_Keycode key = event.key.key;
                if (key == SDLK_F3) {
                    const bool nowOn = diag::toggle();
                    status = nowOn ? "diagnostics on " + diag::logPath() : "diagnostics off";
                } else if (key == SDLK_ESCAPE) {
                    if (chatFocus) {
                        chat.clear();
                        ui.script("if ChatFrame1EditBox then ChatFrame1EditBox:SetText('') ChatFrame1EditBox:Hide() end");
                    }
                    chatFocus = false;
                    ui.script("if GossipFrame and GossipFrame:IsShown() then HideUIPanel(GossipFrame) end");
                    if (joining) SDL_StopTextInput(draw.window);
                } else if (key == SDLK_TAB && !joining) {
                    field = (field + 1) % 3;
                } else if (key == SDLK_RETURN) {
                    if (chatFocus && client) {
                        ui.setNamedText("ChatFrame1EditBox", chat);
                        const std::string problem = ui.script("this = ChatFrame1EditBox\nChatEdit_OnEnterPressed()");
                        if (!problem.empty() && said.empty() && !chat.empty()) sendChat(chat);
                        chat.clear();
                        chatFocus = false;
                    } else if (!client) {
                        Config next = config;
                        next.account = account;
                        next.password = password;
                        next.character = character;
                        if (!character.empty()) {
                            joining = true;
                            focusName = character;
                        }
                        try {
                            client = std::make_unique<GameClient>(next);
                            client->setLogger([&](const std::string& line) { status = line; });
                            client->connectSession();
                            if (!character.empty()) client->enqueue("enter " + character);
                            status = "connected";
                        } catch (const std::exception& ex) {
                            status = ex.what();
                            client.reset();
                        }
                    } else if (client->view().inWorld) {
                        chat.clear();
                        chatFocus = true;
                        SDL_StartTextInput(draw.window);
                        ui.script("if ChatFrame_OpenChat then ChatFrame_OpenChat('') end\n"
                                  "if ChatFrame1EditBox then ChatFrame1EditBox:Show() ChatFrame1EditBox:SetText('') end");
                    }
                } else if (key == SDLK_Z && (event.key.mod & SDL_KMOD_ALT) != 0 && (event.key.mod & SDL_KMOD_CTRL) == 0 &&
                           (joining || (client && client->view().inWorld))) {
                    // Bindings.xml command TOGGLEUI. The 2.4.3 default key is Alt-Z.
                    showUi = !showUi;
                    ui.setShown("UIParent", showUi);
                    if (!showUi) chatFocus = false;
                } else if (key == SDLK_BACKSPACE) {
                    std::string& target = chatFocus ? chat : field == 0 ? account : field == 1 ? password : character;
                    if (!target.empty()) target.pop_back();
                    if (chatFocus) ui.setNamedText("ChatFrame1EditBox", chat);
                } else if (!chatFocus && client && client->view().inWorld) {
                    if (key >= SDLK_1 && key <= SDLK_9 && client) {
                        const GameClient::View menu = client->view();
                        const int index = int(key - SDLK_1);
                        if (menu.gossip.open && index >= 0 && index < int(menu.gossip.choices.size()))
                            client->enqueue("option " + std::to_string(index));
                    }
                    if (key == SDLK_F && targetGuid != 0 && client) client->enqueue("hello " + std::to_string(targetGuid));
                    if (key == SDLK_KP_0) {
                        walkMode = !walkMode;
                        status = walkMode ? "walking" : "running";
                        if (client) client->enqueue(walkMode ? "pace 1" : "pace 0");
                    }
                    if (key == SDLK_W) wHeld = true;
                    if (key == SDLK_A) aHeld = true;
                    if (key == SDLK_S) sHeld = true;
                    if (key == SDLK_D) dHeld = true;
                    if (key == SDLK_Q) qHeld = true;
                    if (key == SDLK_E) eHeld = true;
                    if (key == SDLK_SPACE) jumpRequested = true;
                }
            } else if (event.type == SDL_EVENT_KEY_UP) {
                if (event.key.key == SDLK_W) wHeld = false;
                if (event.key.key == SDLK_A) aHeld = false;
                if (event.key.key == SDLK_S) sHeld = false;
                if (event.key.key == SDLK_D) dHeld = false;
                if (event.key.key == SDLK_Q) qHeld = false;
                if (event.key.key == SDLK_E) eHeld = false;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_RIGHT) {
                if (looking) {
                    looking = false;
                    SDL_SetWindowRelativeMouseMode(draw.window, false);
                }
                if (rightDown && !rightDrag && (joining || (client && client->view().inWorld))) {
                    float x = 0, y = 0;
                    toPixel(event.button.x, event.button.y, x, y);
                    if (!(showUi && ui.click(x, y))) {
                        pickX = x;
                        pickY = y;
                        talkClick = true;
                    }
                }
                rightDown = false;
                rightDrag = false;
            } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && !looking && showUi) {
                float x = 0, y = 0;
                toPixel(event.button.x, event.button.y, x, y);
                ui.release(x, y);
            } else if (event.type == SDL_EVENT_MOUSE_MOTION && (looking || rightDown)) {
                if (rightDown && !rightDrag) {
                    const float dx = event.motion.x - rightX;
                    const float dy = event.motion.y - rightY;
                    if (dx * dx + dy * dy > 25.f) {
                        rightDrag = true;
                        looking = true;
                        SDL_SetWindowRelativeMouseMode(draw.window, true);
                    }
                }
                if (looking) {
                    camYaw -= event.motion.xrel * 0.004f;
                    camPitch = std::clamp(camPitch + event.motion.yrel * 0.003f, -0.2f, 1.2f);
                }
            } else if (event.type == SDL_EVENT_MOUSE_WHEEL && (joining || (client && client->view().inWorld))) {
                const float axisX = event.wheel.integer_x != 0 ? float(event.wheel.integer_x) : event.wheel.x;
                const float axisY = event.wheel.integer_y != 0 ? float(event.wheel.integer_y) : event.wheel.y;
                float notches = std::fabs(axisY) >= std::fabs(axisX) ? axisY : axisX;
                if (std::fabs(notches) > 3.f) notches = std::copysign(1.f, notches);
                const float yawBefore = camYaw;
                const float distBefore = camDist;
                if (notches != 0.f) camDist = std::clamp(camDist - notches * 1.5f, 3.f, 36.f);
                if (diag::enabled() && notches != 0.f) {
                    const bool atLimit = camDist == 3.f || camDist == 36.f;
                    diag::check("camera.zoom", camYaw == yawBefore && (camDist != distBefore || atLimit),
                                "distance " + std::to_string(distBefore) + " -> " + std::to_string(camDist));
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT) {
                if ((joining || (client && client->view().inWorld)) && !chatFocus) continue;
                const char* text = event.text.text;
                if (text && text[0] >= 32) {
                    std::string& target = chatFocus ? chat : field == 0 ? account : field == 1 ? password : character;
                    if (target.size() < (chatFocus ? 255 : 40)) target += text[0];
                    if (chatFocus) ui.setNamedText("ChatFrame1EditBox", chat);
                }
            }
        }
        if (client) {
            for (int n = 0; n < 8; ++n) {
                if (!client->pump(0)) break;
            }
            const GameClient::View view = client->view();
            ui.setPlayerName(focusName.empty() ? character : focusName);
            const uint64_t now = SDL_GetTicks();
            float dt = now > lastFrame ? (now - lastFrame) / 1000.f : 0.f;
            if (dt > 0.05f) dt = 0.05f;
            frameDt = dt;
            lastFrame = now;
            if (view.selfGuid != 0 && view.inWorld) focusMap = view.map;
            if (view.inWorld && view.selfGuid != 0 && !smoothReady) {
                smoothX = view.self.x;
                smoothY = view.self.y;
                smoothZ = view.self.z;
                sentZ = smoothZ;
                smoothReady = true;
            }
            if (view.inWorld && !cameraAimed && view.selfGuid != 0) {
                camYaw = view.self.o;
                camPitch = 0.18f;
                camDist = 8.f;
                cameraAimed = true;
                smoothX = view.self.x;
                smoothY = view.self.y;
                smoothZ = view.self.z;
                sentZ = smoothZ;
                smoothReady = true;
            }
            if (qHeld) camYaw += 2.2f * dt;
            if (eHeld) camYaw -= 2.2f * dt;
            const bool moving = view.inWorld && (wHeld || aHeld || sHeld || dHeld);
            float forward = (wHeld ? 1.f : 0.f) - (sHeld ? 1.f : 0.f);
            float side = (aHeld ? 1.f : 0.f) - (dHeld ? 1.f : 0.f);
            const float mag = std::sqrt(forward * forward + side * side);
            if (mag > 1.f) {
                forward /= mag;
                side /= mag;
            }
            const float speed = movePace(view.self, walkMode, forward);
            if (moving && smoothReady) {
                const float step = speed * dt;
                float dx = (std::cos(camYaw) * forward - std::sin(camYaw) * side) * step;
                float dy = (std::sin(camYaw) * forward + std::cos(camYaw) * side) * step;
                auto clear = [&](float sx, float sy) {
                    const float reach = std::sqrt(sx * sx + sy * sy) + 0.4f;
                    if (reach < 0.08f) return true;
                    const float knee = scene.wallAhead(smoothX, smoothY, smoothZ + 0.7f, sx, sy, reach);
                    const float chest = scene.wallAhead(smoothX, smoothY, smoothZ + 1.5f, sx, sy, reach);
                    return std::min(knee, chest) + 0.02f >= reach;
                };
                if (!clear(dx, dy)) {
                    if (clear(dx, 0.f)) dy = 0.f;
                    else if (clear(0.f, dy)) dx = 0.f;
                    else dx = dy = 0.f;
                }
                smoothX += dx;
                smoothY += dy;
                pendingDx += dx;
                pendingDy += dy;
                if (diag::enabled()) {
                    const float stepLen = std::sqrt(dx * dx + dy * dy);
                    diagMeters += stepLen;
                    diagMoveTime += dt;
                    if (stepLen > diagMaxStep) diagMaxStep = stepLen;
                    const float facingX = std::cos(camYaw);
                    const float facingY = std::sin(camYaw);
                    const float along = dx * facingX + dy * facingY;
                    const float right = dx * facingY - dy * facingX;
                    if (wHeld && !aHeld && !sHeld && !dHeld) {
                        diagWAlong += along;
                        diagWLen += stepLen;
                    } else if (aHeld && !wHeld && !sHeld && !dHeld) {
                        diagARight += right;
                        diagALen += stepLen;
                    } else if (dHeld && !wHeld && !aHeld && !sHeld) {
                        diagDRight += right;
                        diagDLen += stepLen;
                    }
                    if (stepLen > 0.55f)
                        diag::check("move.step", false, "frame moved " + std::to_string(stepLen) + " yards");
                }
                const float driftX = view.self.x - smoothX;
                const float driftY = view.self.y - smoothY;
                if (driftX * driftX + driftY * driftY > 20.f * 20.f) {
                    smoothX = view.self.x;
                    smoothY = view.self.y;
                    smoothZ = view.self.z;
                    sentZ = smoothZ;
                    airborne = false;
                    verticalSpeed = 0;
                    pendingDx = pendingDy = 0;
                }
            } else if (smoothReady && view.selfGuid != 0) {
                const float driftX = view.self.x - smoothX;
                const float driftY = view.self.y - smoothY;
                if (driftX * driftX + driftY * driftY > 12.f * 12.f) {
                    smoothX = view.self.x;
                    smoothY = view.self.y;
                    smoothZ = view.self.z;
                    sentZ = smoothZ;
                    airborne = false;
                    verticalSpeed = 0;
                } else {
                    const float blend = std::min(1.f, dt * 8.f);
                    smoothX += driftX * blend;
                    smoothY += driftY * blend;
                }
            }
            if (smoothReady && jumpRequested && !airborne && view.inWorld) {
                if (pendingDx * pendingDx + pendingDy * pendingDy > 0.0004f) {
                    client->enqueue("move " + std::to_string(pendingDx) + " " + std::to_string(pendingDy) + " " +
                                    std::to_string(smoothZ - sentZ));
                    pendingDx = pendingDy = 0;
                    sentZ = smoothZ;
                }
                airborne = true;
                verticalSpeed = 7.96f;
                const float xySpeed = moving ? speed : 0.f;
                client->enqueue("jump " + std::to_string(verticalSpeed) + " " + std::to_string(xySpeed) + " " +
                                std::to_string(std::cos(camYaw)) + " " + std::to_string(std::sin(camYaw)));
                if (diag::enabled()) diag::event("move.jump", "space");
            }
            jumpRequested = false;
            if (airborne && smoothReady) {
                verticalSpeed -= 19.29f * dt;
                smoothZ += verticalSpeed * dt;
            }
            if (view.selfGuid != 0) {
                focusX = smoothReady ? smoothX : view.self.x;
                focusY = smoothReady ? smoothY : view.self.y;
                focusZ = smoothReady ? smoothZ : view.self.z;
            }
            auto placeCamera = [&]() {
                const float cy = std::cos(camYaw);
                const float sy = std::sin(camYaw);
                const float horiz = camDist * std::cos(camPitch);
                const float lookZ = focusZ + 1.6f;
                cam.x = focusX - cy * horiz;
                cam.y = focusY - sy * horiz;
                cam.z = lookZ + std::sin(camPitch) * camDist;
                cam.o = camYaw;
                cam.pitch = camPitch;
            };
            if (joining || view.inWorld) placeCamera();
            if ((pickClick || talkClick) && client && view.inWorld) {
                const bool talk = talkClick;
                pickClick = false;
                talkClick = false;
                const float focal = float(draw.h) * (420.f / 720.f);
                const float right = focal > 1.f ? (pickX - float(draw.w) * 0.5f) / focal : 0.f;
                const float up = focal > 1.f ? (float(draw.h) * 0.5f - pickY) / focal : 0.f;
                const float c = std::cos(cam.o);
                const float s = std::sin(cam.o);
                const float cp = std::cos(cam.pitch);
                const float sp = std::sin(cam.pitch);
                float dirX = s * right + c * sp * up + c * cp;
                float dirY = -c * right + s * sp * up + s * cp;
                float dirZ = cp * up - sp;
                const float dirLen = std::sqrt(dirX * dirX + dirY * dirY + dirZ * dirZ);
                if (dirLen > 1e-4f) {
                    dirX /= dirLen;
                    dirY /= dirLen;
                    dirZ /= dirLen;
                }
                uint64_t hit = 0;
                float nearest = 40.f;
                for (const auto& entry : client->objects()) {
                    const WowObject& object = entry.second;
                    if (object.guid == view.selfGuid || (object.typeId != 3 && object.typeId != 4)) continue;
                    const float vx = object.move.x - cam.x;
                    const float vy = object.move.y - cam.y;
                    const float vz = object.move.z + 1.f - cam.z;
                    const float along = vx * dirX + vy * dirY + vz * dirZ;
                    if (along < 0.5f || along > nearest) continue;
                    const float ox = cam.x + dirX * along - object.move.x;
                    const float oy = cam.y + dirY * along - object.move.y;
                    const float oz = cam.z + dirZ * along - (object.move.z + 1.f);
                    if (ox * ox + oy * oy + oz * oz > 1.4f * 1.4f) continue;
                    nearest = along;
                    hit = object.guid;
                }
                targetGuid = hit;
                client->enqueue("select " + std::to_string(hit));
                if (talk && hit != 0) {
                    const WowObject* unit = nullptr;
                    for (const auto& entry : client->objects())
                        if (entry.first == hit) unit = &entry.second;
                    if (unit && unit->typeId == 3) {
                        const float dx = unit->move.x - focusX;
                        const float dy = unit->move.y - focusY;
                        const float dz = unit->move.z - focusZ;
                        if (dx * dx + dy * dy + dz * dz < 8.f * 8.f) client->enqueue("hello " + std::to_string(hit));
                        else status = "too far to talk";
                    }
                }
            }
            if (view.inWorld && !airborne) {
                const float dz = smoothZ - sentZ;
                const bool flush = !moving && (pendingDx != 0.f || pendingDy != 0.f || std::fabs(dz) > 0.05f);
                const bool due = now - lastMove > 100 &&
                                 (pendingDx * pendingDx + pendingDy * pendingDy > 0.0004f || std::fabs(dz) > 0.05f);
                if (flush || due) {
                    float turn = camYaw - view.self.o;
                    while (turn > 3.14159f) turn -= 6.28318f;
                    while (turn < -3.14159f) turn += 6.28318f;
                    if (std::fabs(turn) > 0.02f) client->enqueue("turn " + std::to_string(turn));
                    if (pendingDx * pendingDx + pendingDy * pendingDy > 0.0004f || std::fabs(dz) > 0.02f)
                        client->enqueue("move " + std::to_string(pendingDx) + " " + std::to_string(pendingDy) + " " +
                                        std::to_string(dz));
                    pendingDx = pendingDy = 0;
                    sentZ = smoothZ;
                    lastMove = now;
                }
            }
            if (!said.empty()) {
                sendChat(said);
                said.clear();
            }
        }

        if (assets && !lights.ready()) lights.load(*assets);
        if (lights.ready() && video.localTime && smoothReady)
            draw.sun = lights.at(focusMap, smoothX, smoothY, smoothZ, halfMinuteNow());
        else draw.sun = {};
        if (draw.sun.ok) {
            draw.clearR = draw.sun.fog[0];
            draw.clearG = draw.sun.fog[1];
            draw.clearB = draw.sun.fog[2];
        } else {
            draw.clearR = 24;
            draw.clearG = 20;
            draw.clearB = 18;
        }
        draw.begin();
        if (ui.layoutPending() || draw.w != laidW || draw.h != laidH) {
            ui.layout(float(draw.w), float(draw.h));
            laidW = draw.w;
            laidH = draw.h;
        }
        const GameClient::View view = client ? client->view() : GameClient::View{};
        const bool playing = joining || view.inWorld;
        if (playing && !catalog.ready && !config.clientPath.empty()) {
            if (!assets) assets = std::make_unique<ArchiveSet>(config.clientPath);
            catalog.load(config.clientPath);
            SDL_StopTextInput(draw.window);
        }
        if (playing && assets) {
            scene.beginFrame();
            scene.advance(*assets, focusMap, focusX, focusY, focusZ);
            if (smoothReady) {
                float ground = smoothZ;
                if (scene.floorAt(focusX, focusY, smoothZ, ground)) {
                    const float gap = ground - smoothZ;
                    if (!airborne) {
                        if (gap < -4.f) {
                            airborne = true;
                            verticalSpeed = std::min(verticalSpeed, 0.f);
                        } else if (gap <= 1.8f) {
                            const float delta = ground - smoothZ;
                            const float absDelta = std::fabs(delta);
                            // Stacked floor triangles disagree by a centimeter. Snapping to
                            // whichever wins this frame hops the whole body while walking.
                            if (absDelta > 0.03f) {
                                const float maxStep = std::max(0.02f, 3.f * frameDt);
                                if (absDelta <= maxStep) smoothZ = ground;
                                else smoothZ += std::copysign(maxStep, delta);
                            }
                        }
                    } else if (verticalSpeed <= 0.f && smoothZ <= ground + 0.08f) {
                        smoothZ = ground;
                        airborne = false;
                        verticalSpeed = 0;
                        if (client) {
                            client->enqueue("land " + std::to_string(smoothX) + " " + std::to_string(smoothY) + " " +
                                            std::to_string(smoothZ) + " " + std::to_string(camYaw));
                            pendingDx = pendingDy = 0;
                            sentZ = smoothZ;
                        }
                    }
                }
                focusZ = smoothZ;
                const float cy = std::cos(camYaw);
                const float sy = std::sin(camYaw);
                const float cp = std::cos(camPitch);
                const float sp = std::sin(camPitch);
                const float rawClip = scene.clipCamera(focusX, focusY, focusZ + 1.6f, -cy * cp, -sy * cp, sp, camDist);
                if (!camClipReady) {
                    camApplied = rawClip;
                    camClipReady = true;
                } else if (rawClip + 0.08f < camApplied) {
                    // A wall that stays in front of the lens takes effect immediately.
                    // A one-frame graze, much closer than the previous trace, eases in.
                    if (rawClip <= camClipNear + 0.45f) camApplied = rawClip;
                    else camApplied -= std::min(camApplied - rawClip, 16.f * frameDt);
                } else if (rawClip > camApplied + 0.12f) {
                    camApplied += std::min(rawClip - camApplied, 8.f * frameDt);
                }
                camClipNear = rawClip;
                const float horiz = camApplied * cp;
                cam.x = focusX - cy * horiz;
                cam.y = focusY - sy * horiz;
                cam.z = focusZ + 1.6f + sp * camApplied;
                cam.o = camYaw;
                cam.pitch = camPitch;
            }
            const Camera viewCam{cam.x, cam.y, cam.z, cam.o, cam.pitch};
            draw.look(viewCam, video.horizon);
            draw.sky(viewCam);
            draw.look(viewCam, video.farClip);
            draw.fogOn();
            if (scene.showTerrain()) {
                std::vector<const Terrain*> tiles;
                scene.terrains(tiles);
                for (const Terrain* tile : tiles)
                    draw.terrain(scene, *tile, viewCam.x, viewCam.y, viewCam.z, std::cos(viewCam.o), std::sin(viewCam.o));
            }
            std::vector<WorldScene::Drawable> items;
            scene.drawables(items);
            for (const WorldScene::Drawable& item : items) draw.lit(scene, *item.mesh, item.world);
            static const std::unordered_map<uint64_t, WowObject> noObjects;
            const auto& worldObjects = client ? client->objects() : noObjects;
            struct Near {
                float distance;
                const WowObject* object;
                bool held = false;
            };
            std::vector<Near> nearby;
            nearby.reserve(worldObjects.size());
            for (const auto& entry : worldObjects) {
                const WowObject& object = entry.second;
                const float dx = object.move.x - focusX;
                const float dy = object.move.y - focusY;
                const float distance = dx * dx + dy * dy;
                const bool held = heldGuids.count(object.guid) != 0;
                const float reach = kDistCull;
                if (object.guid == view.selfGuid || distance < reach * reach) nearby.push_back({distance, &object, held});
            }
            std::sort(nearby.begin(), nearby.end(), [](const Near& a, const Near& b) {
                if (a.held != b.held) return a.held;
                return a.distance < b.distance;
            });
            std::unordered_set<uint64_t> nextHeld;
            int loadedModels = 0;
            int drawnModels = 0;
            const bool moving = view.inWorld && (wHeld || aHeld || sHeld || dHeld);
            const uint32_t now = uint32_t(SDL_GetTicks());
            auto drawOne = [&](const WowObject& object) {
                const bool creature = object.typeId == 3 || object.typeId == 4 || object.guid == view.selfGuid;
                const uint32_t display = creature ? object.displayId() : (object.fields.size() > 8 ? object.fields[8] : 0);
                std::string path;
                const std::string* skin = nullptr;
                const std::string* hair = nullptr;
                const ModelCatalog::WornLook* outfit = nullptr;
                float scale = 1.f;
                if (creature) {
                    const auto model = catalog.displayToModel.find(display);
                    if (model != catalog.displayToModel.end()) {
                        const auto file = catalog.modelPath.find(model->second);
                        if (file != catalog.modelPath.end()) path = file->second;
                    }
                    const auto sized = catalog.displayScale.find(display);
                    if (sized != catalog.displayScale.end()) scale = sized->second;
                    if (object.typeId == 4 && object.fields.size() > kFieldPlayerBytes2 && (object.fields[kFieldBytes0] & 255u) != 0) {
                        ModelCatalog::PlayerKey key;
                        key.bytes0 = object.fields[kFieldBytes0];
                        key.bytes = object.fields[kFieldPlayerBytes];
                        key.bytes2 = object.fields[kFieldPlayerBytes2];
                        for (int slot = 0; slot < 19; ++slot) {
                            const uint32_t index = kFieldVisibleItem + uint32_t(slot) * kVisibleItemStride;
                            if (index >= object.fields.size()) break;
                            key.item[slot] = object.fields[index];
                        }
                        outfit = catalog.playerOutfit(key);
                    }
                    if (!outfit || !outfit->ok) outfit = catalog.npcOutfit(display);
                    if (outfit && outfit->ok) {
                        skin = &outfit->look.body;
                        hair = &outfit->look.hair;
                    } else {
                        const auto painted = catalog.displaySkin.find(display);
                        if (painted != catalog.displaySkin.end()) skin = &painted->second;
                    }
                } else {
                    const auto file = catalog.objectPath.find(display);
                    if (file != catalog.objectPath.end()) path = file->second;
                }
                if (path.size() >= 4 && path.compare(path.size() - 4, 4, ".wmo") == 0) return;
                const bool self = object.guid == view.selfGuid;
                const CharacterLook* dressed = outfit && outfit->ok ? &outfit->look : nullptr;
                static const std::string noSkin;
                const std::string& skinFile = skin ? *skin : noSkin;
                const std::string& hairFile = hair ? *hair : noSkin;
                uint32_t itemHash = display;
                if (object.typeId == 4) {
                    for (int slot = 0; slot < 19; ++slot) {
                        const uint32_t index = kFieldVisibleItem + uint32_t(slot) * kVisibleItemStride;
                        const uint32_t item = index < object.fields.size() ? object.fields[index] : 0;
                        itemHash = itemHash * 16777619u ^ item;
                    }
                }
                int model = -1;
                const auto kept = keptModels.find(object.guid);
                if (kept != keptModels.end() && kept->second.display == display && kept->second.items == itemHash && kept->second.model >= 0)
                    model = kept->second.model;
                else if (!path.empty())
                    model = scene.prepareModel(*assets, path, false, skinFile, hairFile, dressed);
                if (model == -2 && loadedModels < 1) {
                    model = scene.prepareModel(*assets, path, true, skinFile, hairFile, dressed);
                    ++loadedModels;
                }
                if (model >= 0) {
                    KeptModel& slot = keptModels[object.guid];
                    slot.display = display;
                    slot.items = itemHash;
                    slot.model = model;
                }
                if (model < 0) return;
                nextHeld.insert(object.guid);
                float px = self ? focusX : object.move.x;
                float py = self ? focusY : object.move.y;
                float pz = self ? focusZ : object.move.z;
                float facing = self ? camYaw : object.move.o;
                bool walking = self ? moving : (object.move.flags & (0x0000000Fu | kMoveSplineEnabled)) != 0;
                float travel = 0.f;
                float along = 0.f;
                // Only the player may read and advance selfLoco. Game objects used to share it,
                // so every chair and brazier nearby pushed his walk forward again each frame.
                float* loco = self ? &selfLoco : &glides[object.guid].animMs;
                if (!self && (object.typeId == 3 || object.typeId == 4)) {
                    Glide& glide = glides[object.guid];
                    loco = &glide.animMs;
                    if (!glide.seen) {
                        glide.x = px;
                        glide.y = py;
                        glide.z = pz;
                        glide.o = facing;
                        glide.serverX = px;
                        glide.serverY = py;
                        glide.serverZ = pz;
                        glide.seen = true;
                    }
                    const float jumpedX = object.move.x - glide.serverX;
                    const float jumpedY = object.move.y - glide.serverY;
                    const float jumpedZ = object.move.z - glide.serverZ;
                    const bool serverMoved = jumpedX * jumpedX + jumpedY * jumpedY + jumpedZ * jumpedZ > 0.04f;
                    if (serverMoved) {
                        glide.serverX = object.move.x;
                        glide.serverY = object.move.y;
                        glide.serverZ = object.move.z;
                        const float offX = object.move.x - glide.x;
                        const float offY = object.move.y - glide.y;
                        if (!object.move.splineLive && offX * offX + offY * offY > 36.f) {
                            glide.x = object.move.x;
                            glide.y = object.move.y;
                            glide.z = object.move.z;
                            glide.o = object.move.o;
                        } else if (!object.move.splineLive) {
                            const float blend = std::min(1.f, frameDt * 8.f);
                            glide.x += (object.move.x - glide.x) * blend;
                            glide.y += (object.move.y - glide.y) * blend;
                            glide.z += (object.move.z - glide.z) * blend;
                            glide.o = object.move.o;
                        }
                    }
                    if (object.move.splineId != glide.splineId) {
                        glide.splineId = object.move.splineId;
                        glide.started = now;
                        glide.duration = object.move.splineMs;
                        glide.following = object.move.splineLive && object.move.splineMs > 0;
                        glide.fromX = object.move.x;
                        glide.fromY = object.move.y;
                        glide.fromZ = object.move.z;
                        glide.toX = object.move.splineX;
                        glide.toY = object.move.splineY;
                        glide.toZ = object.move.splineZ;
                        glide.x = object.move.x;
                        glide.y = object.move.y;
                        glide.z = object.move.z;
                        const float dx = glide.toX - glide.fromX;
                        const float dy = glide.toY - glide.fromY;
                        if (dx * dx + dy * dy > 0.04f) glide.o = std::atan2(dy, dx);
                    }
                    if (glide.following) {
                        const float span = float(glide.duration);
                        float u = span > 0.f ? float(now - glide.started) / span : 1.f;
                        if (u >= 1.f) {
                            u = 1.f;
                            glide.following = false;
                            glide.serverX = glide.toX;
                            glide.serverY = glide.toY;
                            glide.serverZ = glide.toZ;
                        }
                        glide.x = glide.fromX + (glide.toX - glide.fromX) * u;
                        glide.y = glide.fromY + (glide.toY - glide.fromY) * u;
                        glide.z = glide.fromZ + (glide.toZ - glide.fromZ) * u;
                        const float dx = glide.toX - glide.fromX;
                        const float dy = glide.toY - glide.fromY;
                        const float dz = glide.toZ - glide.fromZ;
                        travel = std::sqrt(dx * dx + dy * dy + dz * dz) / (float(glide.duration) / 1000.f);
                        walking = u < 1.f;
                    } else {
                        // Stay on the server position. Guessing a direction from the move flags
                        // walked every creature out of the room, so the models disappeared.
                        glide.x = object.move.x;
                        glide.y = object.move.y;
                        glide.z = object.move.z;
                        glide.o = object.move.o;
                        walking = (object.move.flags & 0x0000000Fu) != 0;
                    }
                    px = glide.x;
                    py = glide.y;
                    pz = glide.z;
                    facing = glide.o;
                }
                const Mat4 placed = unitPlacement(px, py, pz, facing, scale);
                if (LitMesh* shared = scene.modelMesh(model)) {
                    // Nearby people share one mesh. The player's copy stays for as long
                    // as he wears this model, so a later draw cannot free the buffers
                    // his step is still reading.
                    LitMesh* mesh = shared;
                    if (self) {
                        if (selfMeshModel != model) {
                            selfMesh = *shared;
                            if (!selfMesh.rest.empty()) selfMesh.xyz = selfMesh.rest;
                            selfMesh.posedMoving = 0xff;
                            selfMesh.posedAt = 0xffffffffu;
                            selfMeshModel = model;
                        }
                        mesh = &selfMesh;
                    }
                    const Movement& motion = self ? view.self : object.move;
                    if (self && moving) {
                        float forward = (wHeld ? 1.f : 0.f) - (sHeld ? 1.f : 0.f);
                        float side = (aHeld ? 1.f : 0.f) - (dHeld ? 1.f : 0.f);
                        const float mag = std::sqrt(forward * forward + side * side);
                        if (mag > 1.f) {
                            forward /= mag;
                            side /= mag;
                        }
                        travel = movePace(motion, walkMode, forward);
                        along = forward * travel;
                    } else if (!self && travel <= 0.f && walking) {
                        travel = (motion.flags & 0x00000100u) != 0 ? motion.walk : motion.run;
                        along = travel;
                    } else if (!self) {
                        along = travel;
                    }
                    uint32_t flags = motion.flags;
                    if (self && walkMode) flags |= kMoveWalk;
                    const uint8_t gait = movementGait(self ? moving : walking, along, flags, motion.splineFlags,
                                                      motion.splineLive, travel, motion.walk, motion.run);
                    float ground = 0.f;
                    if (gait == 2) ground = mesh->runGround;
                    else if (gait == 1) ground = mesh->walkGround;
                    else if (gait == 3) ground = mesh->backGround;
                    // A running clip is drawn for its own ground speed. HumanMale's run is
                    // 667 ms at 6.94 yd/s, so one cycle is the distance the body just moved.
                    // Playing it any faster than that spins the legs ahead of the floor.
                    float advance = frameDt * 1000.f;
                    if (gait != 0 && ground > 0.2f && travel > 0.2f) {
                        // Same clock the other walkers use: elapsed time scaled by
                        // how this step compares to the clip. Not a per-frame snap.
                        advance = frameDt * 1000.f * (travel / ground);
                        if (advance > frameDt * 1000.f) advance = frameDt * 1000.f;
                    }
                    if (self && gait == 0) {
                        poseModel(*mesh, 0, 0);
                    } else {
                        *loco += advance;
                        poseModel(*mesh, uint32_t(*loco), gait);
                    }
                    static const uint16_t naked[] = {0, 1, 101, 201, 301, 401, 501, 601, 702, 801, 901, 1001, 1101, 1201, 1301, 1401, 1501};
                    const uint16_t* geos = nullptr;
                    int geosN = 0;
                    if (mesh->character) {
                        if (outfit && !outfit->look.geosets.empty()) {
                            geos = outfit->look.geosets.data();
                            geosN = int(outfit->look.geosets.size());
                        } else {
                            geos = naked;
                            geosN = int(sizeof(naked) / sizeof(naked[0]));
                        }
                    }
                    draw.lit(scene, *mesh, placed, self, geos, geosN);
                    if (mesh->character && outfit && outfit->ok) {
                        const uint32_t sex = outfit->look.gender;
                        auto hang = [&](const std::string& file, const std::string& tex, uint32_t att) {
                            if (file.empty()) return;
                            int pad = -1;
                            const auto known = padModels.find(file);
                            if (known != padModels.end()) pad = known->second;
                            else pad = scene.prepareModel(*assets, file, false, tex, {}, nullptr, sex);
                            if (pad == -2 && loadedModels < 1) {
                                pad = scene.prepareModel(*assets, file, true, tex, {}, nullptr, sex);
                                ++loadedModels;
                            }
                            if (pad >= 0) padModels.emplace(file, pad);
                            if (pad < 0) return;
                            const LitMesh* item = scene.modelMesh(pad);
                            if (!item || item->indices.empty()) return;
                            Mat4 hung;
                            if (!wornTransform(placed, *mesh, att, hung)) return;
                            draw.lit(scene, *item, hung, self);
                        };
                        // Display field 1 is the left pad (attachment 6). Field 2 is the right pad (attachment 5).
                        hang(outfit->shoulderLeft, outfit->shoulderTexLeft, 6);
                        hang(outfit->shoulderRight, outfit->shoulderTexRight, 5);
                    }
                }
                ++drawnModels;
            };
            const WowObject* selfObject = nullptr;
            for (const auto& entry : worldObjects)
                if (entry.first == view.selfGuid) selfObject = &entry.second;
            for (const Near& item : nearby) {
                if (drawnModels >= 100) break;
                if (item.object->guid == view.selfGuid) continue;
                drawOne(*item.object);
            }
            if (selfObject) drawOne(*selfObject);
            heldGuids.swap(nextHeld);
        }
        if (view.inWorld && !uiWorld) {
            ui.fire("PLAYER_LOGIN");
            ui.fire("PLAYER_ENTERING_WORLD");
            uiWorld = true;
        }
        if (view.inWorld) {
            ui.setChannels(view.channels);
            if (view.gossip.open) {
                if (view.gossip.serial != gossipSeen) {
                    gossipSeen = view.gossip.serial;
                    std::string npc = "npc";
                    if (client) {
                        for (const auto& entry : client->objects())
                            if (entry.first == view.gossip.guid && !entry.second.name.empty()) npc = entry.second.name;
                    }
                    std::vector<std::pair<std::string, std::string>> options;
                    for (const GossipChoice& choice : view.gossip.choices)
                        options.emplace_back(choice.text, choice.kind);
                    ui.showGossip(npc, view.gossip.text.empty() ? std::string("...") : view.gossip.text, options);
                }
            } else if (gossipSeen != 0) {
                gossipSeen = 0;
                ui.hideGossip();
            }
            for (const ChatLine& spoken : view.chat) {
                if (spoken.seq <= chatSeen) continue;
                ui.chatLine(spoken.type, spoken.sender, spoken.text, spoken.channel, spoken.language);
                chatSeen = spoken.seq;
            }
        }
        if (view.inWorld) {
            if (const std::string problem = ui.lastError(); !problem.empty() && problem != uiProblem) {
                uiProblem = problem;
                status = problem;
            }
        }
        if (ui.layoutPending()) {
            ui.layout(float(draw.w), float(draw.h));
            laidW = draw.w;
            laidH = draw.h;
        }
        if (playing && view.inWorld && assets) {
            const WorldScene::SoundSpot spot = scene.soundSpot(*assets);
            SoundBank::WmoCue cue;
            if (spot.indoors && spot.wmoRoot) {
                cue = sound.wmoCue(spot.wmoRoot, spot.wmoGroup);
                if (!cue.found && spot.wmoIndex >= 0) cue = sound.wmoCue(spot.wmoRoot, spot.wmoIndex);
            }
            uint32_t area = 0;
            if (cue.found && cue.area) area = cue.area;
            else if (!spot.indoors && spot.chunk) area = spot.chunk;
            else area = sound.areaAt(focusMap, focusX, focusY);
            if (area || cue.found) speaker.apply(sound.planFor(area, cue.found ? &cue : nullptr));
        }
        speaker.pump();
        if (playing && assets && showUi) draw.paintArt(scene, *assets, ui.widgets());
        ++fpsFrames;
        const uint64_t fpsNow = SDL_GetTicks();
        if (fpsNow - fpsWindow >= 500) {
            fpsShown = int(float(fpsFrames) * 1000.f / float(fpsNow - fpsWindow) + 0.5f);
            fpsFrames = 0;
            fpsWindow = fpsNow;
        }
        if (playing && (zoneMap != focusMap || zoneX != int(focusX) || zoneY != int(focusY))) {
            zoneMap = focusMap;
            zoneX = int(focusX);
            zoneY = int(focusY);
            zoneName = catalog.zoneAt(focusMap, focusX, focusY);
            if (client && view.inWorld && !zoneName.empty() && channelsFor != zoneName) {
                channelsFor = zoneName;
                client->enqueue("join General - " + zoneName);
                client->enqueue("join LocalDefense - " + zoneName);
                client->enqueue("join Trade - City");
                client->enqueue("join LookingForGroup");
            }
        }
        if (client) chatChannels = view.channels;
        auto mix = [](uint64_t h, uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            return h;
        };
        auto mixText = [&](uint64_t h, const std::string& s) {
            h = mix(h, s.size());
            for (unsigned char c : s) h = mix(h, c);
            return h;
        };
        uint64_t stamp = mix(ui.revision(), playing ? 1 : 0);
        stamp = mix(stamp, showUi ? 1 : 0);
        const int posX = int(std::floor(focusX / 4.f));
        const int posY = int(std::floor(focusY / 4.f));
        stamp = mix(stamp, uint64_t(fpsShown / 4));
        stamp = mix(stamp, uint64_t(field));
        stamp = mix(stamp, chatFocus ? 1 : 0);
        stamp = mixText(stamp, account);
        stamp = mixText(stamp, character);
        stamp = mix(stamp, password.size());
        stamp = mixText(stamp, status);
        stamp = mixText(stamp, zoneName);
        stamp = mix(stamp, uint64_t(posX) ^ (uint64_t(posY) << 20) ^ (uint64_t(int(focusZ)) << 40));
        stamp = mixText(stamp, chat);
        stamp = mixText(stamp, scene.status());
        stamp = mixText(stamp, view.error);
        stamp = mix(stamp, view.characters.size());
        for (const Character& listed : view.characters) stamp = mixText(stamp, listed.name);
        stamp = mix(stamp, view.chat.size());
        if (!view.chat.empty()) stamp = mixText(stamp, view.chat.back().text);
        if (diag::enabled()) stamp = mix(stamp, uint64_t(diag::failures()));
        draw.present(stamp, [&] {
            float textY = 16;
            auto line = [&](const std::string& s) {
                draw.text(16, textY, s);
                textY += 12;
            };
            if (!playing) {
                draw.rect(16, 16, 440, 250, 0xFF12100C);
                const std::string values[] = {account, std::string(password.size(), '*'), character};
                const char* labels[] = {"account", "password", "character"};
                for (int i = 0; i < 3; ++i) {
                    draw.text(fieldX, fieldTop[i] - 14.f, labels[i]);
                    draw.rect(fieldX, fieldTop[i], fieldW, 26.f, field == i ? 0xFF5A4632 : 0xFF2A241C);
                    draw.text(fieldX + 8.f, fieldTop[i] + 8.f, values[i]);
                }
                textY = 210;
                line("Click a box and type. Tab switches. Enter connects. F3 toggles diagnostics.");
                line(status.substr(0, 70));
                if (view.warden && view.characters.empty())
                    line("Warden is holding the character list. Disable it on the server to enter the world.");
                for (const Character& c : view.characters) line(c.name + "  level " + std::to_string(c.level));
            } else if (showUi) {
                std::string who = focusName.empty() ? "you" : focusName;
                if (who.size() > 16) who.resize(16);
                line(who + (zoneName.empty() ? "" : "  " + zoneName));
                line("pos " + std::to_string(int(focusX)) + " " + std::to_string(int(focusY)) + " " + std::to_string(int(focusZ)));
                line(view.inWorld ? (walkMode ? "walking   right click talks   numpad 0 runs"
                                              : "running   right click talks   numpad 0 walks")
                                   : "entering the world");
                if (targetGuid != 0 && client && !view.gossip.open) {
                    std::string targetName = "creature";
                    for (const auto& entry : client->objects())
                        if (entry.first == targetGuid) targetName = entry.second.name.empty() ? "creature" : entry.second.name;
                    line("target " + targetName);
                }
                if (fpsShown > 0) line(std::to_string(fpsShown) + " fps");
                if (diag::enabled()) {
                    const std::string fail = diag::lastFailure();
                    line("diag F3  fails " + std::to_string(diag::failures()) + (fail.empty() ? "  ok" : "  " + fail.substr(0, 70)));
                }
                if (!scene.status().empty()) line(scene.status());
                if (!view.error.empty()) line(view.error.substr(0, 80));
                if (chatFocus && ui.textOf("ChatFrame1EditBox") != chat) line("> " + chat);
            }
            if (playing && showUi) {
                auto rowsOf = [](const std::string& text, int cols) {
                    std::vector<std::string> rows;
                    if (cols < 8) cols = 8;
                    size_t at = 0;
                    while (at < text.size()) {
                        size_t end = text.find('\n', at);
                        if (end == std::string::npos) end = text.size();
                        std::string piece = text.substr(at, end - at);
                        at = end < text.size() ? end + 1 : text.size();
                        while (int(piece.size()) > cols) {
                            int cut = cols;
                            const auto space = piece.rfind(' ', size_t(cols));
                            if (space != std::string::npos && int(space) > cols / 2) cut = int(space);
                            rows.push_back(piece.substr(0, size_t(cut)));
                            piece.erase(0, size_t(cut));
                            if (!piece.empty() && piece[0] == ' ') piece.erase(0, 1);
                        }
                        rows.push_back(piece);
                    }
                    return rows;
                };
                float gx = 0, gy = 0, gw = 0, gh = 0;
                for (const UiWidget& widget : ui.widgets()) {
                    if (widget.name == "GossipFrame" && widget.w > 2.f && widget.h > 2.f) {
                        gx = widget.x;
                        gy = widget.y;
                        gw = widget.w;
                        gh = widget.h;
                    }
                }
                const bool gossipOpen = gw > 2.f;
                auto overGossip = [&](float x, float y, float rw, float rh) {
                    return gossipOpen && x < gx + gw && x + rw > gx && y < gy + gh && y + rh > gy;
                };
                auto chatBackdrop = [&](float x, float y, float rw, float rh) {
                    if (rw < 1.f || rh < 1.f) return;
                    if (!overGossip(x, y, rw, rh)) {
                        draw.rect(x, y, rw, rh, 0xFF101010);
                        return;
                    }
                    const float x1 = x + rw;
                    const float y1 = y + rh;
                    const float hx1 = gx + gw;
                    const float hy1 = gy + gh;
                    if (gy > y) draw.rect(x, y, rw, gy - y, 0xFF101010);
                    if (hy1 < y1) draw.rect(x, hy1, rw, y1 - hy1, 0xFF101010);
                    const float midY = gy > y ? gy : y;
                    const float midH = (hy1 < y1 ? hy1 : y1) - midY;
                    if (midH > 0.f && gx > x) draw.rect(x, midY, gx - x, midH, 0xFF101010);
                    if (midH > 0.f && hx1 < x1) draw.rect(hx1, midY, x1 - hx1, midH, 0xFF101010);
                };
                for (const UiWidget& widget : ui.widgets()) {
                    if (widget.name == "ChatFrame1" || widget.name == "ChatFrame1EditBox" || widget.name == "ChatFrame1Tab")
                        chatBackdrop(widget.x, widget.y, widget.w, widget.h);
                }
                auto paintWords = [&](const UiWidget& widget) {
                    if (widget.text.empty()) return;
                    const bool gossip = widget.name.find("Gossip") != std::string::npos;
                    const bool chatBox = widget.name.find("ChatFrame") != std::string::npos;
                    if (!gossip && !chatBox) {
                        draw.text(widget.x + 4, widget.y + 4, widget.text.substr(0, 48));
                        return;
                    }
                    unsigned rgb = gossip ? 0xFF2A1C10 : 0xFFE7D7A4;
                    if (widget.name == "GossipFrameNpcNameText" || widget.name == "GossipFrameGreetingGoodbyeButton")
                        rgb = 0xFFFFF0C0;
                    const bool upward = widget.type == "ScrollingMessageFrame";
                    float boxH = widget.h;
                    if (boxH < 12.f) boxH = upward ? 140.f : (gossip ? 96.f : 20.f);
                    const float boxW = widget.w < 32.f ? (chatBox ? 430.f : 280.f) : widget.w;
                    const int cols = std::max(8, int((boxW - 16.f) / 8.f));
                    const std::vector<std::string> rows = rowsOf(widget.text, cols);
                    const float lineH = 10.f;
                    if (upward) {
                        float y = widget.y + boxH - lineH - 6.f;
                        for (int i = int(rows.size()) - 1; i >= 0 && y >= widget.y; --i) {
                            if (!overGossip(widget.x, y, boxW, lineH))
                                draw.text(widget.x + 8.f, y, rows[size_t(i)], rgb);
                            y -= lineH;
                        }
                    } else {
                        float y = widget.y + 4.f;
                        for (const std::string& row : rows) {
                            if (y > widget.y + boxH - 8.f) break;
                            draw.text(widget.x + 8.f, y, row, rgb);
                            y += lineH;
                        }
                    }
                };
                for (const UiWidget& widget : ui.widgets()) {
                    if (widget.name.find("Gossip") != std::string::npos) continue;
                    paintWords(widget);
                }
                for (const UiWidget& widget : ui.widgets()) {
                    if (widget.name.find("Gossip") == std::string::npos) continue;
                    paintWords(widget);
                }
            }
        });
        if (diag::enabled()) {
            if (view.inWorld && worldSince == 0) worldSince = fpsNow;
            if (!view.inWorld) worldSince = 0;
            if (diagSecond == 0) diagSecond = fpsNow;
            if (fpsNow - diagSecond >= 1000) {
                if (diagMoveTime > 0.6f) {
                    const float speed = diagMeters / diagMoveTime;
                    diag::check("move.speed", speed > 2.f && speed < 12.f, std::to_string(speed) + " yards/s");
                    diag::event("move.speed", std::to_string(speed) + " yards/s max step " + std::to_string(diagMaxStep));
                }
                if (diagWLen > 2.f)
                    diag::check("move.forward", diagWAlong > 0.85f * diagWLen, "along " + std::to_string(diagWAlong));
                if (diagALen > 2.f)
                    diag::check("move.strafe", diagARight < -0.85f * diagALen, "right " + std::to_string(diagARight));
                if (diagDLen > 2.f)
                    diag::check("move.strafe", diagDRight > 0.85f * diagDLen, "right " + std::to_string(diagDRight));
                if (view.inWorld && !airborne && smoothReady) {
                    float ground = 0;
                    if (scene.floorAt(smoothX, smoothY, smoothZ, ground))
                        diag::check("move.floor", std::fabs(smoothZ - ground) < 0.75f,
                                    "feet " + std::to_string(smoothZ - ground));
                }
                if (view.inWorld) {
                    const float hx = cam.x - focusX;
                    const float hy = cam.y - focusY;
                    const float horiz = std::sqrt(hx * hx + hy * hy);
                    const float expect = camApplied * std::cos(camPitch);
                    diag::check("camera.distance", std::fabs(horiz - expect) < 0.35f,
                                std::to_string(horiz) + " expected " + std::to_string(expect));
                    const float driftX = view.self.x - smoothX;
                    const float driftY = view.self.y - smoothY;
                    const float drift = std::sqrt(driftX * driftX + driftY * driftY);
                    diag::check("move.drift", drift < 8.f, std::to_string(drift) + " yards from server");
                }
                const bool settled = worldSince != 0 && fpsNow - worldSince > 3000 && scene.status().empty();
                if (settled && fpsShown > 0)
                    diag::check("render.fps", fpsShown >= 60, std::to_string(fpsShown));
                if (settled)
                    diag::check("render.scene", scene.groups() > 0 || scene.showTerrain(),
                                "groups " + std::to_string(scene.groups()));
                if (view.inWorld)
                    diag::event("frame", "fps " + std::to_string(fpsShown) + " groups " + std::to_string(scene.groups()) +
                                             " fails " + std::to_string(diag::failures()));
                diagMeters = diagMoveTime = diagMaxStep = 0;
                diagWAlong = diagWLen = diagARight = diagALen = diagDRight = diagDLen = 0;
                diagSecond = fpsNow;
            }
        }
    }
    } catch (const diag::Failure& ex) {
        SDL_Log("%s", ex.what());
        SDL_StopTextInput(draw.window);
        draw.close();
        return 2;
    }
    SDL_StopTextInput(draw.window);
    draw.close();
    return 0;
}

}  // namespace reclient
