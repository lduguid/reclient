#include "Sound.hpp"

#include "Formats.hpp"
#include "Mpq.hpp"

#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace reclient {
namespace {

uint32_t readU32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint16_t readU16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

std::string slash(std::string path) {
    for (char& c : path)
        if (c == '/') c = '\\';
    return path;
}

bool audioFile(const std::string& name) {
    if (name.size() < 5) return false;
    const std::string ext = name.substr(name.size() - 4);
    return ext == ".mp3" || ext == ".MP3" || ext == ".wav" || ext == ".WAV";
}

std::string joinPath(const std::string& dir, const std::string& file) {
    std::string name = slash(file);
    if (name.find('\\') != std::string::npos) return name;
    if (dir.empty()) return name;
    std::string full = slash(dir);
    if (!full.empty() && full.back() != '\\') full.push_back('\\');
    full += name;
    return full;
}

void appendResampled(std::vector<int16_t>& out, const int16_t* in, int frames, int channels, int rate) {
    if (!in || frames <= 0 || channels <= 0 || rate <= 0) return;
    const double step = double(rate) / 44100.0;
    int count = int(double(frames) / step);
    if (count < 1) count = 1;
    const size_t base = out.size();
    out.resize(base + size_t(count) * 2);
    for (int i = 0; i < count; ++i) {
        const double src = i * step;
        int i0 = int(src);
        if (i0 >= frames) i0 = frames - 1;
        const int i1 = i0 + 1 < frames ? i0 + 1 : i0;
        const float blend = float(src - i0);
        for (int c = 0; c < 2; ++c) {
            const int channel = channels == 1 ? 0 : std::min(c, channels - 1);
            const float sample = float(in[i0 * channels + channel]) * (1.f - blend) + float(in[i1 * channels + channel]) * blend;
            out[base + size_t(i) * 2 + c] = int16_t(std::max(-32768.f, std::min(32767.f, sample)));
        }
    }
}

bool decodeWav(const uint8_t* data, size_t len, std::vector<int16_t>& out) {
    if (len < 12 || std::memcmp(data, "RIFF", 4) != 0 || std::memcmp(data + 8, "WAVE", 4) != 0) return false;
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t* pcm = nullptr;
    size_t pcmBytes = 0;
    size_t at = 12;
    while (at + 8 <= len) {
        const uint32_t size = readU32(data + at + 4);
        const uint8_t* body = data + at + 8;
        if (at + 8 + size > len) break;
        if (std::memcmp(data + at, "fmt ", 4) == 0 && size >= 16) {
            format = readU16(body);
            channels = readU16(body + 2);
            rate = readU32(body + 4);
            bits = readU16(body + 14);
        } else if (std::memcmp(data + at, "data", 4) == 0) {
            pcm = body;
            pcmBytes = size;
        }
        at += 8 + size + (size & 1);
    }
    if (format != 1 || bits != 16 || channels == 0 || rate == 0 || !pcm) return false;
    const int frames = int(pcmBytes / (size_t(channels) * 2));
    appendResampled(out, reinterpret_cast<const int16_t*>(pcm), frames, channels, int(rate));
    return !out.empty();
}

bool decodeMp3(const uint8_t* data, size_t len, std::vector<int16_t>& out) {
    mp3dec_t dec;
    mp3dec_init(&dec);
    std::vector<int16_t> native;
    int rate = 0;
    int channels = 0;
    size_t off = 0;
    while (off < len) {
        mp3dec_frame_info_t info{};
        int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
        const int samples = mp3dec_decode_frame(&dec, data + off, int(len - off), frame, &info);
        if (info.frame_bytes <= 0) break;
        off += size_t(info.frame_bytes);
        if (samples <= 0 || info.channels <= 0 || info.hz <= 0) continue;
        rate = info.hz;
        channels = info.channels;
        native.insert(native.end(), frame, frame + samples * info.channels);
    }
    if (native.empty() || channels <= 0) return false;
    appendResampled(out, native.data(), int(native.size() / size_t(channels)), channels, rate);
    return !out.empty();
}

float volumeOf(uint32_t bits) {
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    if (!(value > 0.05f && value <= 1.5f)) return 1.f;
    return value;
}

}  // namespace

SoundBank::Piece SoundBank::pieceFor(uint32_t soundId) const {
    Piece piece;
    const auto found = tracks_.find(soundId);
    if (found == tracks_.end()) return piece;
    piece.files = found->second.files;
    piece.volume = found->second.volume;
    return piece;
}

bool SoundBank::load(ArchiveSet& archives) {
    std::vector<uint8_t> bytes;
    Dbc sounds;
    Dbc areas;
    Dbc mapAreas;
    Dbc music;
    Dbc intro;
    Dbc ambience;
    if (!archives.read("DBFilesClient\\SoundEntries.dbc", bytes) || !sounds.load(bytes.data(), bytes.size())) return false;
    for (uint32_t row = 0; row < sounds.records; ++row) {
        Track track;
        track.volume = volumeOf(sounds.get(row, 24));
        const std::string dir = sounds.text(sounds.get(row, 23));
        for (uint32_t field = 3; field <= 12 && field < sounds.fields; ++field) {
            const std::string file = sounds.text(sounds.get(row, field));
            if (!audioFile(file)) continue;
            track.files.push_back(joinPath(dir, file));
        }
        const uint32_t id = sounds.get(row, 0);
        const std::string name = sounds.text(sounds.get(row, 2));
        tracks_[id] = track;
        if (!name.empty()) byName_[name] = id;
    }
    if (archives.read("DBFilesClient\\ZoneMusic.dbc", bytes) && music.load(bytes.data(), bytes.size())) {
        for (uint32_t row = 0; row < music.records; ++row) {
            Tune tune;
            tune.silenceMin = music.get(row, 2);
            tune.silenceMax = music.get(row, 4);
            if (tune.silenceMax < tune.silenceMin) std::swap(tune.silenceMin, tune.silenceMax);
            tune.day = music.get(row, 6);
            tune.night = music.get(row, 7);
            tunes_[music.get(row, 0)] = tune;
        }
    }
    if (archives.read("DBFilesClient\\ZoneIntroMusicTable.dbc", bytes) && intro.load(bytes.data(), bytes.size())) {
        int bestField = 2;
        int bestHits = -1;
        const int fields = std::min(5, int(intro.fields));
        for (int field = 1; field < fields; ++field) {
            int hits = 0;
            for (uint32_t row = 0; row < intro.records; ++row)
                if (tracks_.count(intro.get(row, uint32_t(field)))) ++hits;
            if (hits > bestHits) {
                bestHits = hits;
                bestField = field;
            }
        }
        for (uint32_t row = 0; row < intro.records; ++row) {
            const uint32_t sound = intro.get(row, uint32_t(bestField));
            if (tracks_.count(sound)) introSound_[intro.get(row, 0)] = sound;
        }
    }
    std::unordered_map<uint32_t, uint32_t> ambienceDay;
    if (archives.read("DBFilesClient\\SoundAmbience.dbc", bytes) && ambience.load(bytes.data(), bytes.size())) {
        for (uint32_t row = 0; row < ambience.records; ++row) ambienceDay[ambience.get(row, 0)] = ambience.get(row, 1);
    }
    if (archives.read("DBFilesClient\\AreaTable.dbc", bytes) && areas.load(bytes.data(), bytes.size())) {
        for (uint32_t row = 0; row < areas.records; ++row) {
            AreaSound sound;
            sound.parent = areas.get(row, 2);
            sound.music = areas.get(row, 8);
            sound.intro = areas.get(row, 9);
            const auto bed = ambienceDay.find(areas.get(row, 7));
            if (bed != ambienceDay.end()) sound.ambience = bed->second;
            sound.name = areas.text(areas.get(row, 11));
            areas_[areas.get(row, 0)] = std::move(sound);
        }
    }
    Dbc wmoAreas;
    if (archives.read("DBFilesClient\\WMOAreaTable.dbc", bytes) && wmoAreas.load(bytes.data(), bytes.size())) {
        auto keyOf = [](int32_t root, int32_t group) {
            return (uint64_t(uint32_t(root)) << 32) | uint32_t(group);
        };
        for (uint32_t row = 0; row < wmoAreas.records; ++row) {
            WmoCue cue;
            cue.found = true;
            cue.area = wmoAreas.get(row, 10);
            cue.music = wmoAreas.get(row, 7);
            cue.intro = wmoAreas.get(row, 8);
            const auto bed = ambienceDay.find(wmoAreas.get(row, 6));
            if (bed != ambienceDay.end()) cue.ambience = bed->second;
            const uint64_t key = keyOf(int32_t(wmoAreas.get(row, 1)), int32_t(wmoAreas.get(row, 3)));
            const auto have = wmo_.find(key);
            const bool better = have == wmo_.end() || (!have->second.music && cue.music) || (!have->second.area && cue.area);
            if (better) wmo_[key] = cue;
        }
    }
    if (archives.read("DBFilesClient\\WorldMapArea.dbc", bytes) && mapAreas.load(bytes.data(), bytes.size())) {
        for (uint32_t row = 0; row < mapAreas.records; ++row) {
            Rect rect;
            rect.map = mapAreas.get(row, 1);
            rect.area = mapAreas.get(row, 2);
            auto axis = [](uint32_t bits) {
                float value = 0;
                std::memcpy(&value, &bits, sizeof(value));
                return value;
            };
            const float yA = axis(mapAreas.get(row, 4));
            const float yB = axis(mapAreas.get(row, 5));
            const float xA = axis(mapAreas.get(row, 6));
            const float xB = axis(mapAreas.get(row, 7));
            rect.x0 = std::min(xA, xB);
            rect.x1 = std::max(xA, xB);
            rect.y0 = std::min(yA, yB);
            rect.y1 = std::max(yA, yB);
            if (rect.area != 0) rects_.push_back(rect);
        }
    }
    return !tracks_.empty();
}

uint32_t SoundBank::areaAt(uint32_t map, float x, float y) const {
    uint32_t area = 0;
    float best = 1.0e20f;
    for (const Rect& rect : rects_) {
        if (rect.map != map || x < rect.x0 || x > rect.x1 || y < rect.y0 || y > rect.y1) continue;
        const float size = (rect.x1 - rect.x0) * (rect.y1 - rect.y0);
        if (size < best) {
            best = size;
            area = rect.area;
        }
    }
    return area;
}

SoundBank::WmoCue SoundBank::wmoCue(int32_t root, int32_t group) const {
    auto keyOf = [](int32_t rootId, int32_t groupId) {
        return (uint64_t(uint32_t(rootId)) << 32) | uint32_t(groupId);
    };
    const auto exact = wmo_.find(keyOf(root, group));
    if (exact != wmo_.end()) return exact->second;
    if (group != -1) {
        const auto any = wmo_.find(keyOf(root, -1));
        if (any != wmo_.end()) return any->second;
    }
    return {};
}

SoundBank::Plan SoundBank::planFor(uint32_t area) const { return planFor(area, nullptr); }

SoundBank::Plan SoundBank::planFor(uint32_t area, const WmoCue* cue) const {
    Plan plan;
    uint32_t musicRow = 0;
    uint32_t introRow = 0;
    uint32_t ambSound = 0;
    uint32_t walk = area;
    if (cue && cue->found) {
        if (cue->area) {
            plan.area = cue->area;
            walk = cue->area;
        }
        musicRow = cue->music;
        introRow = cue->intro;
        ambSound = cue->ambience;
    }
    if (!plan.area) plan.area = area;
    std::string name;
    for (int guard = 0; guard < 8 && walk; ++guard) {
        const auto found = areas_.find(walk);
        if (found == areas_.end()) break;
        if (name.empty()) name = found->second.name;
        if (!musicRow && found->second.music) {
            musicRow = found->second.music;
            if (!introRow) introRow = found->second.intro;
        }
        if (!ambSound && found->second.ambience) ambSound = found->second.ambience;
        if (musicRow && ambSound) break;
        const uint32_t parent = found->second.parent;
        if (!parent || parent == walk) break;
        walk = parent;
    }
    plan.name = std::move(name);
    const Tune* tune = nullptr;
    uint32_t musicSound = 0;
    if (musicRow) {
        const auto row = tunes_.find(musicRow);
        if (row != tunes_.end()) {
            tune = &row->second;
            plan.musicId = musicRow;
        }
    }
    if (!tune && plan.music.files.empty() && !plan.name.empty() && !musicRow) {
        const auto named = byName_.find("Zone-" + plan.name);
        if (named != byName_.end()) {
            musicSound = named->second;
            plan.musicId = 0x80000000u | musicSound;
            for (const auto& [id, candidate] : tunes_) {
                (void)id;
                if (candidate.day == musicSound || candidate.night == musicSound) {
                    tune = &candidate;
                    plan.musicId = id;
                    break;
                }
            }
        }
    }
    if (tune) {
        musicSound = tune->day ? tune->day : tune->night;
        plan.silenceMinMs = tune->silenceMin;
        plan.silenceMaxMs = tune->silenceMax;
    }
    if (musicSound) plan.music = pieceFor(musicSound);
    if (introRow) {
        const auto sound = introSound_.find(introRow);
        if (sound != introSound_.end()) plan.intro = pieceFor(sound->second);
    } else if (!plan.name.empty() && !musicRow) {
        const auto named = byName_.find("Zone-" + plan.name + " Intro");
        if (named != byName_.end()) plan.intro = pieceFor(named->second);
    }
    if (ambSound) {
        plan.ambience = pieceFor(ambSound);
        plan.ambience.volume *= 0.45f;
        plan.ambienceId = ambSound;
    }
    return plan;
}

SoundBank::Piece SoundBank::cue(const std::string& name) const {
    const auto found = byName_.find(name);
    if (found == byName_.end()) return {};
    return pieceFor(found->second);
}

struct PcmStream::Body {
    std::vector<uint8_t> file;
    mp3dec_t dec{};
    bool mp3 = false;
    bool started = false;
    size_t off = 0;
    int hz = 0;
    int channels = 0;
    bool done = false;
    std::shared_ptr<std::vector<int16_t>> pcm = std::make_shared<std::vector<int16_t>>();
};

PcmStream::PcmStream() : body_(std::make_unique<Body>()) {}
PcmStream::~PcmStream() = default;

bool PcmStream::start(ArchiveSet& archives, const std::string& path) {
    if (!archives.read(path, body_->file) || body_->file.size() < 16) return false;
    body_->mp3 = path.size() >= 4 && (path.compare(path.size() - 4, 4, ".mp3") == 0 || path.compare(path.size() - 4, 4, ".MP3") == 0);
    if (!body_->mp3) {
        decodeWav(body_->file.data(), body_->file.size(), *body_->pcm);
        body_->file.clear();
        body_->done = true;
        return !body_->pcm->empty();
    }
    mp3dec_init(&body_->dec);
    body_->started = true;
    return true;
}

void PcmStream::pump(int maxFrames) {
    if (!body_->mp3 || body_->done || maxFrames <= 0) return;
    int produced = 0;
    while (body_->off < body_->file.size() && produced < maxFrames) {
        mp3dec_frame_info_t info{};
        int16_t frame[MINIMP3_MAX_SAMPLES_PER_FRAME];
        const int samples = mp3dec_decode_frame(&body_->dec, body_->file.data() + body_->off, int(body_->file.size() - body_->off),
                                                frame, &info);
        if (info.frame_bytes <= 0) break;
        body_->off += size_t(info.frame_bytes);
        ++produced;
        if (samples <= 0 || info.channels <= 0 || info.hz <= 0) continue;
        std::vector<int16_t> chunk;
        appendResampled(chunk, frame, samples, info.channels, info.hz);
        body_->pcm->insert(body_->pcm->end(), chunk.begin(), chunk.end());
    }
    if (body_->off >= body_->file.size()) {
        body_->done = true;
        body_->file.clear();
    }
}

std::shared_ptr<std::vector<int16_t>> PcmStream::samples() const { return body_->pcm; }
bool PcmStream::done() const { return body_->done; }
bool PcmStream::ready() const { return body_->done ? !body_->pcm->empty() : body_->pcm->size() >= 22050; }

std::vector<int16_t> SoundBank::decode(ArchiveSet& archives, const std::string& path) const {
    std::vector<int16_t> out;
    std::vector<uint8_t> bytes;
    if (!archives.read(path, bytes) || bytes.size() < 16) return out;
    const bool mp3 = path.size() >= 4 && (path.compare(path.size() - 4, 4, ".mp3") == 0 || path.compare(path.size() - 4, 4, ".MP3") == 0);
    if (mp3) decodeMp3(bytes.data(), bytes.size(), out);
    else decodeWav(bytes.data(), bytes.size(), out);
    return out;
}

}  // namespace reclient
