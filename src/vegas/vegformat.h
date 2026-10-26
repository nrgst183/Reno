// Layout of a VEGAS Pro project file (.veg) as worked out from real VEGAS Pro 18 projects. MAGIX documents
// none of it, so the readers check everything and drop a record instead of guessing when it looks wrong.
//
// The file is a tree of chunks in the style of Sony Wave64: each chunk starts with a 16-byte GUID and a 64-bit
// size that includes the 24-byte header; the next chunk starts at the next multiple of 8. The top chunk is "riff"
// ({66666972-912E-11CF-A5D6-28DB04C10000}), lists are "list" ({7473696C-912F-11CF-A5D6-28DB04C10000}); both carry
// a second GUID after the size saying what they hold. Numbers are little-endian, text is UTF-16LE, times are
// 64-bit counts of 100 ns (event ends meet the next start exactly and keyframe times end exactly at event ends).
#pragma once
#include "../core/base.h"
#include "../core/json.h"
#include "vegas.h"
#include <cstdint>
#include <string>
#include <vector>

namespace reno {
namespace veg {

// A run of bytes of the file. Reads are bounds-checked: a read past the end gives 0 (or empty text) and sets
// 'bad', so a damaged file gives wrong values, not a crash.
class Bytes {
public:
    Bytes() {}
    Bytes(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    size_t size() const { return n_; }
    const uint8_t* data() const { return p_; }
    bool has(size_t off, size_t len) const { return off <= n_ && len <= n_ - off; }
    uint8_t u8(size_t off) const;
    uint16_t u16(size_t off) const;
    uint32_t u32(size_t off) const;
    int32_t i32(size_t off) const { return (int32_t)u32(off); }
    uint64_t u64(size_t off) const;
    int64_t i64(size_t off) const { return (int64_t)u64(off); }
    float f32(size_t off) const;
    double f64(size_t off) const;
    uint32_t be32(size_t off) const;                   // big-endian (VST program files)
    Bytes sub(size_t off, size_t len) const;           // clipped to what is there
    std::string guid(size_t off) const;                // as Windows writes GUIDs, lower case, no braces
    // UTF-16LE text of at most 'bytes' bytes, ending early at a 0 character, as UTF-8.
    std::string text(size_t off, size_t bytes) const;
    // UTF-16LE text ending with a 0 character; 'next' (if given) is set to the byte after it.
    std::string textZ(size_t off, size_t* next = nullptr) const;
    // Where 'needle' first occurs at or after 'from' (npos if nowhere).
    size_t find(const void* needle, size_t len, size_t from = 0) const;
    mutable bool bad = false;

private:
    const uint8_t* p_ = nullptr;
    size_t n_ = 0;
};

// GUIDs that name chunks and lists (compared whole).
namespace G {
extern const char* const Riff;
extern const char* const List;
extern const char* const VegForm;            // the riff's form type for a VEGAS project
extern const char* const Header;             // project properties, paths
extern const char* const MediaList, *const Media;
extern const char* const Tracks, *const Track, *const TrackHeader, *const Events, *const Event;
extern const char* const EventData, *const Take;
extern const char* const VideoEventList, *const VideoEventData;
extern const char* const AudioEventList, *const AudioEventData;
extern const char* const EventFx, *const TrackFx, *const Fx;
extern const char* const PanCrop;
extern const char* const EventEnvelopes, *const TrackEnvelopes, *const EnvelopeHeader, *const EnvelopePoints;
extern const char* const EventAudioFx, *const TrackAudioFx;
extern const char* const VideoTrackList, *const VideoTrackData, *const AudioTrackList, *const AudioTrackData;
extern const char* const TrackMotionList, *const TrackMotion;
extern const char* const TrackExtraA, *const TrackExtraB;
extern const char* const Groups, *const Group;
extern const char* const Markers, *const Marker;
extern const char* const CommandMarkers, *const MarkersB, *const CdMarkers;   // other marker lists (see vegas.cpp)
extern const char* const Summary, *const SummaryData;
extern const char* const Buses, *const Mixer, *const Bus;
extern const char* const ViewList, *const ViewData, *const Timelines, *const TimelineList, *const TimelineData;
extern const char* const WindowState, *const EmptyListA, *const EmptyChunkA;
extern const char* const OfxEffect;          // the class of an OpenFX effect record
extern const char* const AudioChain;         // the start of a chain of audio plug-ins
}  // namespace G

struct Chunk {
    std::string id;                 // the chunk's GUID ("list" chunks: G::List)
    std::string type;               // lists: what the list holds
    bool isList = false;
    Bytes data;                     // a chunk: its contents; a list: the bytes of the chunks inside it
    size_t fileOffset = 0;          // where the chunk's header is in the file
    std::vector<Chunk> kids;

    bool is(const char* g) const { return isList ? type == g : id == g; }
    const Chunk* child(const char* g) const;                    // first child that is(g), or null
    std::vector<const Chunk*> children(const char* g) const;
};

struct ChunkTree {
    Chunk root;
    bool damaged = false;           // a chunk ran past the end of its parent, or nesting was too deep
    std::string problem;            // what, in words
    size_t count = 0;
};

// True if the bytes start the way a VEGAS project does.
bool looksLikeVeg(const Bytes& file);
// Reads the whole tree. Stops at the first chunk that does not fit and marks the tree damaged; keeps what came before.
void readChunkTree(const Bytes& file, ChunkTree& out);

// ---------------------------------------------------------------- chunks vegas180.exe writes that no sample has
// vegas180.exe keeps its chunk GUIDs in one table (file offset 0xddea00); each record is written by its own Persist
// function (e.g. _sfmarker::Persist). Chunks it can write that no sample contains (contents not worked out): two
// envelope lists in a video event's VideoEventList after VideoEventData (probably transition progress), a list
// d52af8d4-7359-11d2-8709-00c04f8edb8a in an event after EventFx (probably the transition), a list
// b12f5229-c44c-413e-84bd-a6eff5746218 in a track after TrackMotionList, and several project-level chunks
// (media bins among them). Reno keeps unknown chunks as they are.
// The four marker lists (90020ef4..ef7) are the project's lists at 0x7a0, 0x7a8, 0x7b0, 0x7b8: markers and regions,
// command markers, a list no scripting object uses, CD tracks and indexes.

// ---------------------------------------------------------------- an OpenFX effect (video effect, generator)
// Record layout (offsets from its start):
//   0x00 u32 header size (0x30)     0x04 GUID G::OfxEffect        0x20 u32 flags
//   0x28 u32 bytes of the identifier text   0x2c u32 bytes of the preset name
//   then the identifier ("{Svfx:com.vegascreativesoftware:sharpen}"), the preset name ("(Default)"),
//   u32 bytes of the parameters, u32 how many, and per parameter:
//     u32 n, u32 bytes of the name, the name, n bytes of value: the value itself, u32 how many keyframes,
//     and per keyframe: f64 time (frames from the start of what the effect is on, at the project's frame
//     rate), u32 interpolation, the value at the key, and for doubles the in and out tangents (each as large
//     as the value, twice); text keys are f64 time, u32 interpolation, u32 bytes and the text.
//   A value is a double per dimension (Double, 2D, 3D, RGB, RGBA), a 32-bit integer per dimension (Integer,
//   Boolean, Choice, 2D/3D integers), or text: u32 bytes, UTF-16LE with its 0 (String, Custom).
// Flags: 0x01 switches the effect off; 0x80 is "applied before pan/crop" (only set on a run of effects at the start
// of an event's chain); 0x400 is nearly always set; 0x200 and 0x40 appear only with VEGAS's own plug-ins and change
// nothing Reno can show.
struct OfxRecord {
    std::string pluginId;           // "com.vegascreativesoftware:sharpen" (without "{Svfx:" and "}")
    std::string rawId;              // as written
    std::string preset;
    uint32_t flags = 0;
    bool bypass() const { return (flags & 0x01) != 0; }
    bool beforePanCrop() const { return (flags & 0x80) != 0; }
    Json state;                     // {"plugin": id, "params": {...}} in the shape Reno's OpenFX host loads
    int unreadParams = 0;           // parameters whose value could not be made sense of (left out)
    std::vector<std::string> unreadNames;
    int keyframedParams = 0;
    size_t end = 0;                 // the byte after the record
};
// Whether an OpenFX record starts at 'off'.
bool isOfxRecordAt(const Bytes& b, size_t off);
bool readOfxRecord(const Bytes& b, size_t off, OfxRecord& out);
// Keyframe interpolation as stored (OFXInterpolationType: 1 Linear, 2 Fast, 3 Slow, 4 Smooth, 5 Sharp, 6 Hold,
// 7 Manual, 8 Split) as the curve in Reno's state ("kc": 0 Linear, 1 Fast, 2 Slow, 3 Smooth, 4 Sharp, 5 Hold;
// Manual and Split become Linear), and back.
int curveOfOfxInterp(uint32_t v);
uint32_t ofxInterpOfCurve(int curve);
// "{Svfx:com.x:y}" -> "com.x:y"
std::string ofxIdFromVegas(const std::string& raw);
// Every OpenFX record at or after 'from', in order (records inside records are not searched). For places that
// hold effects among other things (generators, the video output).
std::vector<OfxRecord> findOfxRecords(const Bytes& b, size_t from);

// ---------------------------------------------------------------- a chain of audio plug-ins
// G::AudioChain, u32 version (3), a GUID, u32 bytes + name ("Empty Graph"), u64, the 18-byte WAVEFORMATEX the chain
// runs at, u32, u32 plug-in count, u32, then per plug-in:
//   CLSID, u32 flags (1 = switched off: the tick in VEGAS's chain, scripting Effect.Bypass), u32 bytes + name, CLSID
//   again, u32 n, n bytes of plug-in data, u32 VEGAS's own (0xFFFFFFFF; occasionally a small number, probably the
//   last preset loaded).
// A DirectX plug-in's data is what its IPersistStream::Save wrote, passed back to Load unchanged (see
// tests/vegas/vegaudiofx_test.cpp). VEGAS's own plug-ins start it with its length (they refuse it otherwise), then
// a GUID of the plug-in's format, then settings.
// A VST plug-in's data is a u32 (0, or 0xFFFFFFFF for eFX programs picked from the "[Sys]" list) and a VST program
// file ("CcnK": "FxCk" with a value per parameter, or "FPCh" with the plug-in's program chunk). Its CLSID is the
// VST id backwards, 4 more bytes and a fixed tail.
struct AudioPluginRecord {
    std::string clsid;
    std::string name;
    std::string format;             // "VST2", "DX"
    std::string uid;                // as Reno's AudioPluginDesc wants it
    std::string state;              // as Reno's host loads it ("P:", "Q:", "S:" + base64); empty if not known
    std::string problem;            // why the state is missing
    bool bypass = false;
    uint32_t flags = 0;             // as stored
    uint32_t trailer = 0xFFFFFFFFu; // the u32 after the data
};
bool readAudioChain(const Bytes& b, size_t off, std::vector<AudioPluginRecord>& out, std::string* problem);

// ---------------------------------------------------------------- event pan/crop
// A small classic RIFF tree (32-bit sizes): LIST EPCP { EPCS, KFRS, LIST POSL { POSK... }, LIST MSKL {...} }.
// EPCS holds the media's frame size at 0x2c and 0x30 (the rest is uninitialised). Each POSK is i64 time (100 ns
// from the event's start), u32, u32, u32 key type (VideoKeyframeType: 0 linear, 1 hold, 2 slow, 3 fast, 4 smooth,
// 5 sharp), and eight doubles: box width, height, centre x, centre y (media pixels, y down), rotation (radians),
// the centre it turns about (x, y), and one more (always 0).
struct PanCropRecord {
    int mediaW = 0, mediaH = 0;
    struct Key { double time = 0, w = 0, h = 0, cx = 0, cy = 0, rot = 0, rcx = 0, rcy = 0; uint32_t type = 0; };
    std::vector<Key> keys;
    bool mask = false;              // a mask (bezier) is drawn
};
bool readPanCrop(const Bytes& b, PanCropRecord& out);

// ---------------------------------------------------------------- envelopes
// Points chunk of an envelope (G::EnvelopePoints): u32 where the points start (0x38), u32 flags, u32 envelope type
// (EnvelopeType: 0 volume, 1 pan, 3..28 sends to Bus A..Z, 29 composite level (opacity), 30 fade to colour, 31
// mute, 64.. effect automation, 202 velocity), f32 lowest value, f32 highest, f32 neutral, u32 point count, ...,
// u32 bytes per point at 0x34 (0x18). A point: i64 time (100 ns; from the event's start for an event's envelope,
// timeline time for a track's), f32 value, u32 curve (CurveType: 1 linear, 2 fast, -2 slow, 4 smooth, -4 sharp, 0 hold).
struct EnvelopeRecord {
    uint32_t type = 0;
    float lowest = 0, highest = 0, neutral = 0;
    struct Point { double time = 0; double value = 0; int32_t curve = 0; };
    std::vector<Point> points;
};
enum EnvelopeType : uint32_t {
    EnvVolume = 0, EnvPan = 1, EnvBusA = 3, EnvBusZ = 28, EnvComposite = 29, EnvFadeToColor = 30, EnvMute = 31,
    EnvFx1 = 64, EnvVelocity = 202,
};
bool readEnvelope(const Bytes& b, EnvelopeRecord& out);
std::string envelopeName(uint32_t type);         // "volume", "pan", "send to Bus B"...

// ---------------------------------------------------------------- track motion
// G::TrackMotion: u32 0x24 (header size), u32, u32, u32 (an id), u32, f64 frame aspect ratio at 0x14, u32 part
// count at 0x1c (3: motion, shadow, glow), u32, then per part: u32 key count, and per key u32 bytes (including
// itself) and the key: i64 time, then doubles. Only VEGAS's untouched track motion (one default key per part) has
// been seen, so only that is recognised.
// Key offsets from its u32 size: i64 time at 0x04; doubles at 0x30, 0x38, 0x40 are the position (y points up, the
// frame is 1 high; the shadow's default offset is (0.05, -0.05)), 1.0 at 0x48; at 0x50 and 0x58 the motion part's
// default box (1.7778 = frame aspect, 1.0) and the glow's default size (1.05, 1.05); 0.05 at 0x60 (motion); 1.0 at
// 0x28, 0x68, 0x88, 0x90; shadow and glow keys have four floats at 0xb0 (colour) and 0.05 at 0xc0. Type, smoothness,
// angle and rotation centre are not located, so Reno does not write changed track motion.
struct TrackMotionRecord {
    int parts = 0;
    std::vector<int> keys;          // per part
    bool untouched = false;         // exactly VEGAS's default: no motion, no shadow, no glow
};
bool readTrackMotion(const Bytes& b, TrackMotionRecord& out);

// ---------------------------------------------------------------- command markers
// A command marker stores its command as one text, "<command>:<parameter>" (see readMarkers in vegas.cpp), split at
// the first ':' not after a TAB (a TAB makes the next character part of the command).
void splitMarkerCommand(const std::string& text, std::string& command, std::string& parameter);
std::string joinMarkerCommand(const std::string& command, const std::string& parameter);

// ---------------------------------------------------------------- compositing modes
// A video track's compositing mode (and Parent Composite Mode): a number, or for modes VEGAS does with a plug-in,
// the plug-in's GUID with the number -1. VEGAS 18's numbers: 1 Add, 2 Subtract, 3 Multiply (Mask), 4 Source Alpha,
// 12 3D Source Alpha, 1000 Custom; plug-in compositors (listed in vegas180k.dll): Cut, Screen, Overlay, Hard Light,
// Dodge, Burn, Darken, Lighten, Difference, Difference Squared. Reno's names (Track::composite): "add", "subtract",
// "multiply", "srcalpha", "3dsrcalpha", "custom", "cut", "screen", "overlay", "hardlight", "dodge", "burn",
// "darken", "lighten", "difference", "differencesquared". Empty if unknown.
std::string compositeModeName(int32_t mode, const std::string& guid);
// The reverse: number and GUID (lower case, no braces; empty for a built-in mode). False if unknown.
bool compositeModeOf(const std::string& name, int32_t& mode, std::string& guid);

// ---------------------------------------------------------------- tracks (from vegas180.exe's reader and writer)
// Track header chunk (G::TrackHeader): u32 0x38 (where the name starts), u32 flags at 0x04, u32 height at 0x08
// (pixels at 96 dpi), u32 kind at 0x0c (3 video, 2 audio), u32 name bytes at 0x10, u32 colour at 0x14, u32 0x18 (0),
// u32 track id at 0x1c, u32 at 0x20 (1 audio, 2 video), three u32 at 0x24..0x2c passed to the track's parameter
// store (2, 0, 0 in every sample; not worked out), u32 bytes of a second text at 0x34.
//   flags: 0x01 solo, 0x02 mute, 0x10 selected, 0x80 armed for record, 0x10000 minimized, 0x1000000 sliders in
//   automation mode (vegas180.exe then uses the parameter values in the video/audio settings chunk instead of the
//   trim values; see vegas_tracks.cpp). 0x20000, 0x100000 and 0x2000000 are common in the samples and not worked
//   out; kept as they are. 0x200, 0x4 and 0x200000 are set or cleared by vegas180.exe when it reads a track.
//   colour: an index into VEGAS's 8 track colours (Preferences > Display > Track Colors), taken modulo 8. New
//   tracks get the next index, copies keep theirs.
enum TrackFlag : uint32_t {
    TrackSolo = 0x01, TrackMute = 0x02, TrackSelected = 0x10, TrackArmed = 0x80, TrackMinimized = 0x10000, TrackAutomation = 0x1000000,
};
// VEGAS's default colour for a colour index (ARGB, opaque), and the index whose colour is nearest an ARGB colour.
uint32_t trackColorOfIndex(uint32_t index);
uint32_t trackColorIndex(uint32_t argb);
// An audio track's pan type as stored (at 0x1f8 of G::AudioTrackData; the Pan Type menu commands 44300..44305 store
// their number minus 44300): 0 Add Channels, 1 Constant Power, 2 Balance (0 dB center), 3 Balance (-3 dB), 4 Balance
// (-6 dB), 5 Film; as Reno's names (Track::panType). Empty / false if unknown.
std::string panTypeName(uint32_t v);
bool panTypeOf(const std::string& name, uint32_t& v);

// ================================================================ writing
// Numbers are written little-endian and text as UTF-16LE, as the readers take them.
std::string guidBytes(const char* guid);                    // a GUID's 16 bytes as stored
std::string utf16(const std::string& utf8, bool withZero);  // UTF-16LE (with its 0 character if asked)
// Overwrites a number inside a record (out-of-range writes are ignored; the record must already be long enough).
void setU16(std::string& s, size_t off, uint16_t v);
void setU32(std::string& s, size_t off, uint32_t v);
void setU64(std::string& s, size_t off, uint64_t v);
void setF32(std::string& s, size_t off, float v);
void setF64(std::string& s, size_t off, double v);
void addU32(std::string& s, uint32_t v);
void addU64(std::string& s, uint64_t v);
void addF32(std::string& s, float v);
void addF64(std::string& s, double v);
// Seconds as VEGAS keeps time (100 ns units).
int64_t toTicks(double seconds);
// Pads with zeros to a multiple of 8 bytes (the padding counts in the chunk size).
void pad8(std::string& s);

// A chunk or list to be written. Lists hold children; chunks hold their contents (without the 24-byte header,
// which is added when the tree is written).
struct OutNode {
    bool isList = false;
    std::string id;                 // 16 bytes: the chunk's GUID, or (lists) the list's type
    std::string data;               // chunks: the contents
    std::vector<OutNode> kids;      // lists
    int tag = -1;                   // the writer's own mark (which object this is), carried to the written tree
};
// A copy of a chunk (or a whole list) as in the file.
OutNode copyNode(const Chunk& c);
OutNode chunkNode(const char* guid, std::string data);
OutNode listNode(const char* type);
// The whole file: a "riff" of the VEGAS form holding 'top' (its own id is ignored).
std::string writeTree(const OutNode& top);

// ---------------------------------------------------------------- OpenFX records
// What to write in an OpenFX record. Parameters come from Reno's saved state ({"params": {name: {"v": [...],
// "k": [[time, ...], ...], "s": text, "i": whole numbers}}}). If 'original' (the record VEGAS wrote for this effect)
// is set, its header and every unchanged parameter are kept byte for byte, and changed parameters keep its layout
// (number sizes, interpolation and tangents where a key did not move).
struct OfxWriteInput {
    std::string rawId;              // "{Svfx:com.x:y}"
    std::string preset;             // "(Default)"
    uint32_t flags = 0x400;
    Json state;
    Bytes original;
    Bytes headerTemplate;           // used when there is no original: its 0x30-byte header is copied
    Bytes layout;                   // another record of the same effect, for parameter layout
};
struct OfxWriteResult {
    std::string record;             // unpadded
    std::vector<std::string> dropped;       // parameters left out (their kind could not be told)
};
OfxWriteResult writeOfxRecord(const OfxWriteInput& in);

// ---------------------------------------------------------------- audio plug-in chains
// Bytes of one plug-in in a chain (see readAudioChain): CLSID, flags, name, CLSID, size, data, VEGAS's u32.
struct AudioEntryInput {
    std::string clsid16;            // 16 bytes as stored
    std::string name;
    bool bypass = false;
    std::string data;               // VST: the program file ("CcnK"); DirectX: what the plug-in saved, whole
    bool vst = false;               // VST: a u32 (vstLead) comes before the program
    uint32_t vstLead = 0;
    uint32_t trailer = 0xFFFFFFFFu;
};
std::string audioEntry(const AudioEntryInput& in);
// The state of Reno's VST2 host ("P:..." parameter values, "Q:..." chunk of one program, "C:..." chunk of the whole
// bank) as a VST program file ("CcnK"), keeping what the original had that Reno does not (version, program name).
bool vstProgram(const std::string& state, uint32_t vstId, const Bytes& originalProgram, std::string& out);
// Splits a chain chunk into: everything before the first plug-in, each plug-in's entry, and what follows the last.
struct AudioChainParts {
    std::string head;                       // through the count and the u32 after it
    size_t countOff = 0;                    // where in 'head' the count is
    std::vector<std::pair<size_t, size_t>> entries;     // offset, length in the chunk's data
    std::string tail;
};
bool splitAudioChain(const Bytes& b, AudioChainParts& out);

}  // namespace veg

// importVegasProject() on bytes already read ('path' is where they came from, for finding moved media). For tests
// that damage in-memory copies of projects.
VegasImportResult importVegasData(const std::string& bytes, const std::string& path, Project& out);

}  // namespace reno
