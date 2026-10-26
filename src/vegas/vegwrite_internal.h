// Private to the .veg writer (vegwrite*.cpp): helper functions and the Writer class the files share.
#pragma once
#include "vegas.h"
#include "vegformat.h"
#include "veglegacyfx.h"
#include "vegmanaged.h"
#include "vegorigin.h"
#include "../engine/stretch.h"
#include "vegtemplates.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

namespace reno {
namespace vegwrite {

using namespace veg;

const double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------------------------------ templates
// Template chunks, each read once into a tree of its own.
class Templates {
public:
    const Chunk* get(const char* name)
    {
        auto it = files_.find(name);
        if (it == files_.end()) {
            auto f = std::make_shared<VegFile>();
            const std::string raw = templateBytes(name);
            if (std::strcmp(name, "project") == 0) {
                f->bytes = raw;
            } else if (!raw.empty()) {
                // a riff around the one chunk so the ordinary reader accepts it
                f->bytes = guidBytes(G::Riff);
                f->bytes.append(8, '\0');
                f->bytes += guidBytes(G::VegForm);
                f->bytes += raw;
                setU64(f->bytes, 16, f->bytes.size());
            }
            if (!f->bytes.empty()) readChunkTree(Bytes((const uint8_t*)f->bytes.data(), f->bytes.size()), f->tree);
            it = files_.emplace(name, f).first;
        }
        const VegFile& f = *it->second;
        if (f.bytes.empty()) return nullptr;
        if (std::strcmp(name, "project") == 0) return &f.tree.root;
        return f.tree.root.kids.empty() ? nullptr : &f.tree.root.kids[0];
    }

private:
    std::map<std::string, VegFilePtr> files_;
};

Templates& templates();

// ------------------------------------------------------------------------------------------ helpers (vegwrite_util.cpp)
const Chunk* childChunk(const Chunk& c, const char* g);
const Chunk* childList(const Chunk& c, const char* g);
std::string bytesOf(const Bytes& b);
const Chunk* findChunk(const Chunk& c, const std::function<bool(const Chunk&)>& pred);
int curveToVegas(int c);
bool samePanCrop(const std::vector<PanCropKey>& a, const std::vector<PanCropKey>& b);
bool samePoints(const Envelope& a, const Envelope& b);
std::string jsonText(const Json& j);
bool sameFxState(const FxSlot& a, const FxSlot& b);
std::string vegasPath(const std::string& p);
std::string fileNameOf(const std::string& p);
std::string stemOf(const std::string& p);
std::string extOf(const std::string& p);
bool isStill(const MediaItem& m);

// The media chunk's parts: [0, 0x80) header, the path, two more texts (empty when present), the name, then the
// rest (see vegas.cpp).
struct MediaParts {
    std::string head, path, gap, name, rest;
};
bool splitMedia(const Bytes& b, MediaParts& out);
std::string joinMedia(MediaParts m);

// ------------------------------------------------------------------------------------------ the writer
// Builds the bytes of a .veg from a project: run() makes the file, warnings() lists what could not be written.
// Members are spread over vegwrite.cpp, vegwrite_tracks.cpp and vegwrite_fx.cpp.
class Writer {
public:
    Writer(const Project& p, const std::string& path) : p_(p), path_(path) {}
    std::string run();
    std::vector<std::string> warnings() const;

    // what was written for each object, to tie the project to the new file afterwards
    std::map<uint64_t, uint32_t> mediaVegId;            // Reno media id -> its record's id
    std::map<uint64_t, uint64_t> eventVegId;            // Reno event id -> its VEGAS id

private:
    const Project& p_;
    std::string path_;
    const VegProjectOrigin* po_ = nullptr;
    const Chunk* root_ = nullptr;                       // the project's own file, or the template project
    std::vector<std::string> warnLines_;
    std::map<std::string, int> warnCounts_;
    std::vector<std::string> warnOrder_;

    uint64_t lastEventId_ = 0;
    uint32_t lastMediaId_ = 0, lastTrackId_ = 0;
    std::set<uint64_t> usedEventIds_;
    std::set<uint32_t> usedTrackIds_;
    std::map<uint64_t, uint32_t> busIndex_;             // Reno bus id -> VEGAS bus number (1 = Bus A)
    // media
    struct SubclipRef { uint32_t vegId; double start, len; bool reversed; };
    std::map<uint64_t, std::vector<SubclipRef>> subclips_;  // Reno media id -> its subclip records that are written
    std::set<uint32_t> writtenRecords_;                 // media records written (original ids)
    std::map<uint64_t, uint32_t> fileRecord_;           // Reno media id -> the id of its file record (made when needed)
    std::vector<OutNode> newMedia_;                     // records made for this file, written after the others
    int generatorCounter_ = 0;

    bool mine(const VegOriginPtr& o) const { return o && po_ && o->file == po_->file; }
    template <class T> const T* originOf(const VegOriginPtr& o, VegOrigin::Kind k) const { return mine(o) ? vegOriginAs<T>(o, k) : nullptr; }
    void warn(const std::string& s)
    {
        if (std::find(warnLines_.begin(), warnLines_.end(), s) == warnLines_.end()) warnLines_.push_back(s);
    }
    // 'key' has one %d for the count
    void warnCount(const std::string& key)
    {
        if (!warnCounts_.count(key)) warnOrder_.push_back(key);
        ++warnCounts_[key];
    }

    const Chunk* tpl(const char* name) { return templates().get(name); }
    const Chunk* sameFile(const std::function<bool(const Chunk&)>& pred) { return po_ ? findChunk(*root_, pred) : nullptr; }

    OutNode header();
    OutNode summary(const Chunk& c);
    OutNode buses(const Chunk* list);
    std::string busRecord(const Bytes& orig, const FxChain* fx, const std::string* name, double level, uint32_t index, const std::string& where);
    bool sameAudioChain(const FxChain& chain, const Chunk* chunk);
    void prepareMedia();
    std::set<uint64_t> grouped_;                        // VEGAS ids of the events that are in a group
    std::set<const Chunk*> hiddenDone_;                 // events Reno could not read, written once each
    OutNode track(const Track& t, size_t index);
    std::string trackHeader(const Track& t, size_t index, const Chunk& base, const VegTrackOrigin* o, const Track& was, uint32_t vegId, bool video);
    void trackEvents(const Track& t, const VegTrackOrigin* o, OutNode& out);
    void trackSettings(const Track& t, const Chunk& base, const VegTrackOrigin* o, const Track& was, bool video, const std::string& label,
                       size_t fxWritten, size_t chainWritten, OutNode& out);
    void trackWarnings(const Track& t, const VegTrackOrigin* o, const Track& was, const std::string& label);
    OutNode event(const Event& e, const Track& t);
    const Chunk* eventTemplate(bool video);
    std::string eventRecord(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, bool video, uint64_t vegId);
    void eventVideoParts(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, const std::string& where, OutNode& out);
    void eventPanCrop(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, const std::string& where, OutNode& out);
    void eventAudioParts(const Event& e, const Chunk& base, const VegEventOrigin* o, const Event& was, const std::string& where, OutNode& out);
    void eventTake(const Event& e, const Track& t, const VegEventOrigin* o, const Chunk& base, OutNode& out, uint64_t vegId);
    void envelopes(OutNode& list, const Envelopes& envs, const std::vector<VegHidden>* hidden, bool track, TrackType type, const Event* ev,
                   const Event* evRead, const std::string& where);
    void videoFx(OutNode& list, const FxChain& chain, const std::vector<VegHidden>* hidden, const std::string& where, size_t* written);
    std::string ofxRecord(const FxSlot& s, const std::string& where);
    bool audioChain(const FxChain& chain, const Bytes* original, const char* tplName, std::string& out, size_t* written, const std::string& where);
    OutNode media();
    uint32_t fileRecordFor(const MediaItem& m);
    std::string writtenPath(const MediaItem& m) const;
    uint32_t reversedSubclipFor(const MediaItem& m, double needStart, double needEnd, SubclipRef& out);
    OutNode groups(const Chunk* orig);
    OutNode markers(const Chunk* orig, int which = 0);     // 0 markers and regions, 1 command markers, 2 CD markers
    uint64_t newEventId(TrackType type);
};

}  // namespace vegwrite
}  // namespace reno
