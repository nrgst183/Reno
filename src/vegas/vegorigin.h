// What a Reno project keeps of the .veg it was read from, so saving as .veg writes back exactly what VEGAS wrote
// for everything unchanged (see VegOrigin in engine/project.h).
//
// The file's bytes and chunk tree are read once and shared (never modified) by every object that came from it.
// Each object points at its own chunk(s) and keeps a copy of itself as read ("asRead"). The writer (vegwrite.cpp)
// compares an object with its asRead copy: the same is written as the original bytes, differences are patched into
// a copy of them. Internal to src/vegas.
#pragma once
#include "../engine/project.h"
#include "vegformat.h"
#include <map>

namespace reno {

// A .veg as read: its bytes and chunk tree (whose Bytes point into 'bytes').
struct VegFile {
    std::string bytes;
    veg::ChunkTree tree;
    std::string path;
};
using VegFilePtr = std::shared_ptr<const VegFile>;

// Parts of a list that Reno could not read, kept to be written back in place; 'before' is how many readable
// entries of the list came before them.
struct VegHidden {
    size_t before = 0;
    std::vector<const veg::Chunk*> chunks;
};

struct VegOrigin {
    enum Kind { ProjectK, MediaK, TrackK, EventK, FxK, EnvelopeK, BusK, MarkerK };
    explicit VegOrigin(Kind k) : kind(k) {}
    virtual ~VegOrigin() {}
    Kind kind;
    VegFilePtr file;
    const veg::Chunk* chunk = nullptr;      // the object's chunk (or list) in file->tree
};

// One entry of the file's media pool and what became of it.
struct VegMediaRecord {
    enum Kind { File, Subclip, Generated, Other } kind = File;
    const veg::Chunk* chunk = nullptr;
    uint32_t vegId = 0;
    std::string path;                       // File / Subclip: the file (as written); Other: the whole text
    double subStart = 0, subLen = 0;        // Subclip: where it starts in the file and how long it is (seconds)
    bool subReversed = false;
    uint64_t renoId = 0;                    // the MediaItem it was read into (0: none)
};

// Where VEGAS mirrors a reversed subclip: its end, but not beyond the end of the file's picture when the subclip is
// only a little longer than that. (A reversed subclip of one sample .mp4 is recorded 12.4 ms longer than its video stream
// and VEGAS plays it backwards from the video's end; without this Reno's reversed events came out 12.4 ms late, 596
// samples, against VEGAS's render of a sample project. Subclips far longer than the picture are the sound's length, an audio
// take of a file with shorter picture, and stay as they are.) 'info' is what the file says; without it, or for a
// sound file, the subclip's own end.
inline double reversedClipEnd(double subStart, double subLen, const MediaInfo* info)
{
    double end = subStart + subLen;
    if (info && info->hasVideo && !info->isStill && info->videoDuration > 0 && info->videoDuration < end) {
        const double frame = info->fps > 1.0 ? 1.0 / info->fps : 1.0 / 30.0;
        if (end - info->videoDuration <= 1.5 * frame) end = info->videoDuration;
    }
    return std::max(end, subStart);
}

struct VegProjectOrigin : VegOrigin {
    VegProjectOrigin() : VegOrigin(ProjectK) {}
    ProjectSettings settings;               // as read
    double masterVolume = 1.0;
    std::vector<VegMediaRecord> media;      // every media record, in the file's order
    uint64_t lastEventId = 0;               // the largest event id in the file (and in the header's counter)
    uint32_t maxTrackId = 0, maxMediaId = 0;
    std::string headerPath;                 // the project's own path, as written in the header
};

struct VegMediaOrigin : VegOrigin {
    VegMediaOrigin() : VegOrigin(MediaK) {}
    MediaItem asRead;                       // generator state included
    uint32_t vegId = 0;                     // the record it was read from (0: it came only from subclips)
};

struct VegTrackOrigin : VegOrigin {
    VegTrackOrigin() : VegOrigin(TrackK) {}
    Track asRead;                           // without its events, effects and envelopes (they have their own)
    uint32_t vegId = 0;                     // the track's id (header 0x1c)
    std::vector<VegHidden> hiddenEvents;    // events Reno could not read: written back unchanged
    std::vector<VegHidden> hiddenEnvelopes; // envelopes Reno has no kind for (effect automation...)
    std::vector<VegHidden> hiddenFx;        // video effects Reno cannot host (pre-OpenFX plug-ins)
};

struct VegEventOrigin : VegOrigin {
    VegEventOrigin() : VegOrigin(EventK) {}
    Event asRead;                           // without effects and envelopes
    uint64_t vegId = 0;                     // the event's id (what groups list)
    uint32_t takeMedia = 0;                 // the media record the active take plays
    size_t activeTake = 0;                  // which of the event's takes plays
    std::vector<Take> takesRead;            // every take as read, in the file's order (the active one too)
    std::vector<bool> takesOk;              // whether each could be read (its media is known)
    std::vector<VegHidden> hiddenFx;        // video effects Reno cannot host (pre-OpenFX plug-ins)
    std::vector<VegHidden> hiddenEnvelopes; // envelopes Reno has no kind for
};

struct VegFxOrigin : VegOrigin {
    VegFxOrigin() : VegOrigin(FxK) {}
    FxSlot asRead;
    // Video effects: 'chunk' is the effect's chunk. Audio effects: the plug-in's entry in the chain chunk 'chunk'
    // (bytes [entryOff, entryOff + entryLen) of its data). Generators: the OpenFX record inside the media chunk.
    size_t entryOff = 0, entryLen = 0;
    bool mediaFx = false;                   // a media's own effect (VEGAS's Media FX), put on the event by the reader
};

struct VegEnvelopeOrigin : VegOrigin {
    VegEnvelopeOrigin() : VegOrigin(EnvelopeK) {}
    Envelope asRead;
    const veg::Chunk* header = nullptr;     // the header chunk before the points chunk ('chunk')
    uint32_t vegType = 0;
};

struct VegBusOrigin : VegOrigin {
    VegBusOrigin() : VegOrigin(BusK) {}
    Bus asRead;                             // without its effects
};

struct VegMarkerOrigin : VegOrigin {
    VegMarkerOrigin() : VegOrigin(MarkerK) {}
    Project::Marker asRead;
};

template <class T> const T* vegOriginAs(const VegOriginPtr& p, VegOrigin::Kind k)
{
    return (p && p->kind == k) ? static_cast<const T*>(p.get()) : nullptr;
}

}  // namespace reno
