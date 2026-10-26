// Private to the .veg reader (vegas*.cpp): the helpers and the Importer class that its files share.
#pragma once
#include "vegas.h"
#include "vegformat.h"
#include "veglegacyfx.h"
#include "vegmanaged.h"
#include "vegorigin.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <filesystem>
#include <mutex>

namespace reno {
namespace vegread {

using namespace veg;

const double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------------------------------ helpers
inline const Chunk* childChunk(const Chunk& c, const char* g)
{
    for (const auto& k : c.kids)
        if (!k.isList && k.id == g) return &k;
    return nullptr;
}

inline const Chunk* childList(const Chunk& c, const char* g)
{
    for (const auto& k : c.kids)
        if (k.isList && k.type == g) return &k;
    return nullptr;
}

inline std::vector<const Chunk*> childChunks(const Chunk& c, const char* g)
{
    std::vector<const Chunk*> v;
    for (const auto& k : c.kids)
        if (!k.isList && k.id == g) v.push_back(&k);
    return v;
}

inline std::vector<const Chunk*> childLists(const Chunk& c, const char* g)
{
    std::vector<const Chunk*> v;
    for (const auto& k : c.kids)
        if (k.isList && k.type == g) v.push_back(&k);
    return v;
}

inline double finiteOr(double v, double def) { return std::isfinite(v) ? v : def; }

inline std::string secs(double t) { return strfmt("%.3f s", t); }

// Media paths in a project are Windows paths: helpers for comparing them and for finding the part below a folder.
inline std::string normPath(const std::string& p)
{
    std::string s = toLower(p);
    for (auto& c : s) if (c == '/') c = '\\';
    return s;
}

inline std::string windowsFileName(const std::string& p)
{
    const size_t sep = p.find_last_of("\\/");
    return sep == std::string::npos ? p : p.substr(sep + 1);
}

inline bool isStillImage(const std::string& path)
{
    static const char* const exts[] = {".jpg", ".jpeg", ".png", ".bmp", ".tif", ".tiff", ".tga", ".psd", ".dds",
                                       ".webp", ".heic", ".jfif", ".exr", ".dib", ".pcx", ".gif"};
    for (const char* e : exts)
        if (endsWithNoCase(path, e)) return true;
    return false;
}

// Collects what Reno cannot use. Some things are reported once per item (a track's compositing mode); others occur
// hundreds of times in some projects and are counted and reported once.
class Notes {
public:
    void add(const std::string& s)
    {
        if (seen_.insert(s).second) lines_.push_back(s);
    }
    // 'key' is a format with one %d for the count.
    void count(const std::string& key, const std::string& example = std::string())
    {
        auto it = counts_.find(key);
        if (it == counts_.end()) { order_.push_back(key); counts_[key] = {1, example}; }
        else ++it->second.n;
    }
    std::vector<std::string> finish() const
    {
        std::vector<std::string> v = lines_;
        for (const auto& k : order_) {
            const auto& c = counts_.at(k);
            std::string s = strfmt(k.c_str(), c.n);
            if (!c.example.empty()) s += " (for example " + c.example + ")";
            v.push_back(s);
        }
        return v;
    }

private:
    struct Count { int n = 0; std::string example; };
    std::vector<std::string> lines_;
    std::set<std::string> seen_;
    std::vector<std::string> order_;
    std::map<std::string, Count> counts_;
};

// ------------------------------------------------------------------------------------------ media
// One entry of the project's media pool as VEGAS has it.
struct MediaRec {
    VegMediaRecord::Kind kind = VegMediaRecord::File;
    const Chunk* chunk = nullptr;
    uint32_t vegId = 0;
    std::string path;               // File: the file. Subclip: the file it is cut from. Other: what VEGAS wrote.
    std::string name;               // VEGAS's own name for it, if it has one
    bool video = false, audio = false;
    double subStart = 0, subLen = 0;
    bool subReversed = false;
    OfxRecord generator;
    size_t generatorOff = 0;        // where the generator's record starts in the media chunk
    bool generatorOk = false;
    std::string generatorName;      // "VEGAS Titles & Text"
    std::vector<std::pair<OfxRecord, size_t>> mediaFx;    // the media's own video effects (VEGAS's Media FX), and where
    int genW = 0, genH = 0;
    uint64_t renoId = 0;
};


// ------------------------------------------------------------------------------------------ the reader
class Importer {
public:
    Importer(const std::string& vegPath, const VegFilePtr& file) : vegPath_(vegPath), file_(file), tree_(file->tree) {}

    bool run(Project& p, std::string& error);
    std::vector<std::string> notes() const { return notes_.finish(); }

private:
    std::string vegPath_;
    VegFilePtr file_;
    const ChunkTree& tree_;
    std::shared_ptr<VegProjectOrigin> po_;
    Notes notes_;
    Project* p_ = nullptr;
    std::string originalDir_;                       // the folder the project was saved in (as VEGAS wrote it)
    std::map<uint32_t, MediaRec> media_;            // by VEGAS's media id
    std::map<std::string, uint64_t> fileMedia_;     // normalised path -> MediaItem id
    std::map<uint64_t, std::pair<int, int>> mediaSize_;     // MediaItem id -> frame size seen in pan/crop
    std::map<uint32_t, double> mirrorEnd_;          // by VEGAS's media id: where a reversed subclip is mirrored (reversedClipEnd)
    double mirrorEndOf(const MediaRec& m);
    std::map<uint64_t, double> mediaMinLength_;     // MediaItem id -> how long it is at least (from subclips)
    std::map<uint64_t, uint64_t> vegEventToReno_;   // VEGAS event id -> Reno event id
    std::map<uint32_t, uint64_t> busByIndex_;       // VEGAS bus number (1 = Bus A) -> Reno bus id
    std::map<std::string, int> unknownChunks_;      // where -> how many parts were skipped there
    std::set<uint32_t> mediaFxUsed_;                // media whose effects were put on events (said once each)

    template <class O> std::shared_ptr<O> origin(const Chunk* c)
    {
        auto o = std::make_shared<O>();
        o->file = file_;
        o->chunk = c;
        return o;
    }

    void readHeader(const Chunk& c);
    void readSummary(const Chunk& c);
    void readMixer(const Chunk& c);
    void readMedia(const Chunk& list);
    uint64_t mediaItemForFile(const std::string& path, const std::string& name);
    void readBuses(const Chunk& list);
    void readTrack(const Chunk& t, int index);
    void readAudioTrackSettings(const Chunk& tc, Track& t, const std::string& label, bool automation, const bool* hasEnvelope);
    void readVideoTrackSettings(const Chunk& tc, Track& t, const std::string& label, bool automation, const bool* hasEnvelope);
    bool readEnvelopeList(const Chunk& list, bool track, TrackType type, const std::string& label, Envelopes& out,
                          std::vector<VegHidden>& hidden);
    bool readEvent(const Chunk& ev, Track& track, const std::string& where, Event& out);
    void readEventAudioSettings(const Chunk& ev, Event& e, const std::string& where);
    void readEventVideoFx(const Chunk& ev, Event& e, VegEventOrigin& eo, const MediaRec& m, const std::string& where);
    void readEventPanCrop(const Chunk& ev, Event& e, const MediaRec& m, const std::string& where);
    void readGroups(const Chunk& list);
    void readMarkers(const Chunk& list, int which = 0);     // 0 markers/regions, 1 command markers, 2 CD markers
    void finishMedia();
    FxSlot fxSlotOf(const OfxRecord& r, const std::string& where, const Chunk* chunk, size_t off, bool mediaFx);
    // VEGAS managed effects (CoreGraphics: Rays, Glint): true if Reno has a stand-in for the chunk ('out' is its slot)
    bool managedFxSlotOf(const Chunk& chunk, const std::string& where, FxSlot& out);
    bool noteSkippedManagedFx(const Chunk& chunk, const std::string& where);
    void audioChain(const Chunk* chunk, size_t off, const std::string& where, FxChain& into);
    void unknown(const std::string& where) { ++unknownChunks_[where]; }
};

}  // namespace vegread
}  // namespace reno
