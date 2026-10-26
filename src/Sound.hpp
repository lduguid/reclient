#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace reclient {

class ArchiveSet;

// Fills a 44100 Hz stereo buffer. MP3 files decode a few frames at a time so a
// long track does not stall the frame that starts it.
class PcmStream {
public:
    PcmStream();
    ~PcmStream();
    PcmStream(const PcmStream&) = delete;
    PcmStream& operator=(const PcmStream&) = delete;
    bool start(ArchiveSet& archives, const std::string& path);
    void pump(int maxFrames);
    std::shared_ptr<std::vector<int16_t>> samples() const;
    bool done() const;
    bool ready() const;

private:
    struct Body;
    std::unique_ptr<Body> body_;
};

// SoundEntries, ZoneMusic, ZoneIntroMusicTable, SoundAmbience, and AreaTable
// from the 2.4.3 client archives. Playback is separate; this resolves names to files.
class SoundBank {
public:
    struct Piece {
        std::vector<std::string> files;
        float volume = 1.f;
    };
    struct Plan {
        uint32_t area = 0;
        std::string name;
        Piece intro;
        Piece music;
        uint32_t musicId = 0;
        uint32_t silenceMinMs = 0;
        uint32_t silenceMaxMs = 0;
        Piece ambience;
        uint32_t ambienceId = 0;
    };
    // A WMOAreaTable row. music is a ZoneMusic id, intro a ZoneIntroMusicTable id,
    // ambience a SoundEntries id. found is false when no row matches.
    struct WmoCue {
        bool found = false;
        uint32_t area = 0;
        uint32_t music = 0;
        uint32_t intro = 0;
        uint32_t ambience = 0;
    };

    bool load(ArchiveSet& archives);
    // Smallest world-map rectangle that contains the point. 0 when none does.
    uint32_t areaAt(uint32_t map, float x, float y) const;
    WmoCue wmoCue(int32_t root, int32_t group) const;
    Plan planFor(uint32_t area) const;
    // cue overrides the area's music, intro, and ambience where the WMO row sets them.
    Plan planFor(uint32_t area, const WmoCue* cue) const;
    Piece cue(const std::string& name) const;
    // 44100 Hz stereo signed 16-bit. Empty when the file is missing or not PCM/MP3.
    std::vector<int16_t> decode(ArchiveSet& archives, const std::string& path) const;

private:
    struct Rect {
        uint32_t map = 0;
        uint32_t area = 0;
        float x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    };
    struct AreaSound {
        uint32_t parent = 0;
        uint32_t music = 0;
        uint32_t intro = 0;
        uint32_t ambience = 0;
        std::string name;
    };
    struct Track {
        std::vector<std::string> files;
        float volume = 1.f;
    };
    struct Tune {
        uint32_t day = 0;
        uint32_t night = 0;
        uint32_t silenceMin = 0;
        uint32_t silenceMax = 0;
    };
    std::vector<Rect> rects_;
    std::unordered_map<uint32_t, AreaSound> areas_;
    std::unordered_map<uint32_t, Track> tracks_;
    std::unordered_map<std::string, uint32_t> byName_;
    std::unordered_map<uint32_t, Tune> tunes_;
    std::unordered_map<uint32_t, uint32_t> introSound_;
    std::unordered_map<uint64_t, WmoCue> wmo_;
    Piece pieceFor(uint32_t soundId) const;
};

}  // namespace reclient
