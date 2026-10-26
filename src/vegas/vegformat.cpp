#include "vegformat.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <climits>
#include <functional>
#include <set>
#include <map>

namespace reno {
namespace veg {

// ------------------------------------------------------------------------------------------ Bytes
// For tests/vegas/vegaudit.cpp only: with RENO_VEG_TRACE defined, every number or text read through Bytes is
// reported to g_vegTrace so the audit can tell which bytes of a project the reader looked at. No cost otherwise.
#ifdef RENO_VEG_TRACE
void (*g_vegTrace)(const uint8_t* p, size_t len) = nullptr;
#define VEG_TRACE(p, n) do { if (g_vegTrace) g_vegTrace((p), (n)); } while (0)
#else
#define VEG_TRACE(p, n) do {} while (0)
#endif

uint8_t Bytes::u8(size_t off) const
{
    if (!has(off, 1)) { bad = true; return 0; }
    VEG_TRACE(p_ + off, 1);
    return p_[off];
}

uint16_t Bytes::u16(size_t off) const
{
    if (!has(off, 2)) { bad = true; return 0; }
    VEG_TRACE(p_ + off, 2);
    return (uint16_t)(p_[off] | (p_[off + 1] << 8));
}

uint32_t Bytes::u32(size_t off) const
{
    if (!has(off, 4)) { bad = true; return 0; }
    VEG_TRACE(p_ + off, 4);
    return (uint32_t)p_[off] | ((uint32_t)p_[off + 1] << 8) | ((uint32_t)p_[off + 2] << 16) | ((uint32_t)p_[off + 3] << 24);
}

uint64_t Bytes::u64(size_t off) const
{
    if (!has(off, 8)) { bad = true; return 0; }
    return (uint64_t)u32(off) | ((uint64_t)u32(off + 4) << 32);
}

float Bytes::f32(size_t off) const
{
    const uint32_t v = u32(off);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

double Bytes::f64(size_t off) const
{
    const uint64_t v = u64(off);
    double d;
    std::memcpy(&d, &v, 8);
    return d;
}

uint32_t Bytes::be32(size_t off) const
{
    if (!has(off, 4)) { bad = true; return 0; }
    VEG_TRACE(p_ + off, 4);
    return ((uint32_t)p_[off] << 24) | ((uint32_t)p_[off + 1] << 16) | ((uint32_t)p_[off + 2] << 8) | (uint32_t)p_[off + 3];
}

Bytes Bytes::sub(size_t off, size_t len) const
{
    if (off > n_) { bad = true; return Bytes(); }
    if (len > n_ - off) { bad = true; len = n_ - off; }
    return Bytes(p_ + off, len);
}

std::string Bytes::guid(size_t off) const
{
    if (!has(off, 16)) { bad = true; return std::string(); }
    VEG_TRACE(p_ + off, 16);
    // hand-written: printf was most of the load time on big projects
    static const char hex[] = "0123456789abcdef";
    char buf[36];
    int o = 0;
    auto put = [&](uint32_t v, int digits) {
        for (int i = digits - 1; i >= 0; --i) buf[o++] = hex[(v >> (4 * i)) & 0xf];
    };
    put(u32(off), 8); buf[o++] = '-';
    put(u16(off + 4), 4); buf[o++] = '-';
    put(u16(off + 6), 4); buf[o++] = '-';
    put(p_[off + 8], 2); put(p_[off + 9], 2); buf[o++] = '-';
    for (int i = 10; i < 16; ++i) put(p_[off + (size_t)i], 2);
    return std::string(buf, 36);
}

static void putUtf8(std::string& s, uint32_t c)
{
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

std::string Bytes::text(size_t off, size_t bytes) const
{
    std::string s;
    if (off > n_) { bad = true; return s; }
    if (bytes > n_ - off) { bad = true; bytes = n_ - off; }
    VEG_TRACE(p_ + off, bytes);
    for (size_t i = 0; i + 1 < bytes; i += 2) {
        uint32_t c = (uint32_t)(p_[off + i] | (p_[off + i + 1] << 8));
        if (c == 0) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {
            const uint32_t lo = (uint32_t)(p_[off + i + 2] | (p_[off + i + 3] << 8));
            if (lo >= 0xDC00 && lo < 0xE000) {
                c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else c = 0xFFFD;
        } else if (c >= 0xD800 && c < 0xE000) c = 0xFFFD;   // a lone half of a pair
        putUtf8(s, c);
    }
    return s;
}

std::string Bytes::textZ(size_t off, size_t* next) const
{
    size_t end = off;
    while (end + 1 < n_ && (p_[end] || p_[end + 1])) end += 2;
    if (end + 1 >= n_) bad = true;              // no 0 before the end
    std::string s = text(off, end - std::min(end, off));
    if (next) *next = std::min(n_, end + 2);
    return s;
}

size_t Bytes::find(const void* needle, size_t len, size_t from) const
{
    if (!len || len > n_) return std::string::npos;
    const uint8_t* nd = (const uint8_t*)needle;
    for (size_t i = from; i + len <= n_; ++i)
        if (p_[i] == nd[0] && std::memcmp(p_ + i, nd, len) == 0) return i;
    return std::string::npos;
}

// ------------------------------------------------------------------------------------------ GUIDs
namespace G {
const char* const Riff = "66666972-912e-11cf-a5d6-28db04c10000";
const char* const List = "7473696c-912f-11cf-a5d6-28db04c10000";
const char* const VegForm = "46c429ef-904a-11d2-8722-00c04f8edb8a";
const char* const Header = "b28f2d5a-230f-11d2-86af-00c04f8edb8a";
const char* const MediaList = "b28f2d5b-230f-11d2-86af-00c04f8edb8a";
const char* const Media = "b28f2d5d-230f-11d2-86af-00c04f8edb8a";
const char* const Tracks = "4d6c0747-2316-11d2-86b0-00c04f8edb8a";
const char* const Track = "4d6c0748-2316-11d2-86b0-00c04f8edb8a";
const char* const TrackHeader = "4d6c0749-2316-11d2-86b0-00c04f8edb8a";      // a chunk
const char* const Events = "4d6c0749-2316-11d2-86b0-00c04f8edb8a";           // a list with the same GUID
const char* const Event = "4d6c074a-2316-11d2-86b0-00c04f8edb8a";
const char* const EventData = "168d206a-2321-11d2-86b0-00c04f8edb8a";
const char* const Take = "168d206d-2321-11d2-86b0-00c04f8edb8a";
const char* const VideoEventList = "fc2b3bce-7985-11d2-8711-00c04f8edb8a";
const char* const VideoEventData = "fc2b3bcf-7985-11d2-8711-00c04f8edb8a";
const char* const AudioEventList = "d54ad82a-efa7-4c24-8c0c-316427a5f209";
const char* const AudioEventData = "d54ad82a-efa7-4c24-8c0c-316427a5f209";
const char* const EventFx = "1af0e4e4-6e7f-11d2-8705-00c04f8edb8a";
const char* const TrackFx = "1af0e4e5-6e7f-11d2-8705-00c04f8edb8a";
const char* const Fx = "1af0e4e6-6e7f-11d2-8705-00c04f8edb8a";
const char* const PanCrop = "884bacff-74eb-456c-88f3-739e8390bd07";
const char* const EventEnvelopes = "84701b5a-6863-11d2-86fd-00c04f8edb8a";
const char* const TrackEnvelopes = "84701b5d-6863-11d2-86fd-00c04f8edb8a";
const char* const EnvelopeHeader = "84701b5b-6863-11d2-86fd-00c04f8edb8a";
const char* const EnvelopePoints = "84701b5c-6863-11d2-86fd-00c04f8edb8a";
const char* const EventAudioFx = "b66a95ce-5946-48ab-a4f4-f88c95acdc1d";
const char* const TrackAudioFx = "f79298f4-5695-4b17-9ddb-19a0912d39f3";
const char* const VideoTrackList = "69d46c28-7f0b-11d2-8717-00c04f8edb8a";
const char* const VideoTrackData = "69d46c29-7f0b-11d2-8717-00c04f8edb8a";
const char* const AudioTrackList = "69d46c26-7f0b-11d2-8717-00c04f8edb8a";
const char* const AudioTrackData = "69d46c27-7f0b-11d2-8717-00c04f8edb8a";
const char* const TrackMotionList = "d24df987-1842-4a39-b079-9e4379485a38";
const char* const TrackMotion = "cd6915d8-117a-45d6-8ccd-be37a6c3bc9d";
const char* const TrackExtraA = "b5707e20-7a06-4e83-b961-4de58141220a";
const char* const TrackExtraB = "20e4e6b9-4eee-4d5a-9edf-2e7aac9672f4";
const char* const Groups = "b8e73e18-7f4c-456b-93cd-6866b95e5f2b";
const char* const Group = "e901e132-c677-4604-ac9d-db0b87b4eebd";
const char* const Markers = "90020ef4-392d-11d2-86c7-00c04f8edb8a";
const char* const Marker = "abf76256-392d-11d2-86c7-00c04f8edb8a";
const char* const CommandMarkers = "90020ef5-392d-11d2-86c7-00c04f8edb8a";
const char* const MarkersB = "90020ef6-392d-11d2-86c7-00c04f8edb8a";
const char* const CdMarkers = "90020ef7-392d-11d2-86c7-00c04f8edb8a";
const char* const Summary = "925f94bc-525a-11d2-86dc-00c04f8edb8a";
const char* const SummaryData = "71234f1d-5257-11d2-86dc-00c04f8edb8a";
const char* const Buses = "220d2be4-7fd3-11d2-8718-00c04f8edb8a";
const char* const Mixer = "f1ff1c46-a61a-4bcc-bcf3-2477d138db8a";
const char* const Bus = "72b8749d-14ab-4588-94a9-39343aeef7cc";
const char* const ViewList = "2cccb9d8-dedd-46ac-853d-1916a99a9a02";
const char* const ViewData = "5e8a8c39-da6b-4499-b0b2-3663709e1ce4";
const char* const Timelines = "aa585c13-5a89-4b7c-a288-cddd1066dacf";
const char* const TimelineList = "ca8caf31-cb40-4c9c-992c-659cb76624ba";
const char* const TimelineData = "ca8caf32-cb40-4c9c-992c-659cb76624ba";
const char* const WindowState = "c5ea942e-91f9-4ad0-accc-72c090e71969";
const char* const EmptyListA = "1f0900b5-dc84-4743-81cb-087744404598";
const char* const EmptyChunkA = "4c05a4b4-4e50-4e4e-8f5a-0d5f28eb9ede";
const char* const OfxEffect = "a2fc1a41-01f6-460e-ae5a-2be4af57b92e";
const char* const AudioChain = "b4128601-702e-11d1-8253-00009290dd44";
}  // namespace G

// ------------------------------------------------------------------------------------------ the chunk tree
const Chunk* Chunk::child(const char* g) const
{
    for (const auto& k : kids)
        if (k.is(g)) return &k;
    return nullptr;
}

std::vector<const Chunk*> Chunk::children(const char* g) const
{
    std::vector<const Chunk*> v;
    for (const auto& k : kids)
        if (k.is(g)) v.push_back(&k);
    return v;
}

bool looksLikeVeg(const Bytes& file)
{
    if (file.size() < 40) return false;
    if (file.guid(0) != G::Riff || file.guid(24) != G::VegForm) return false;
    const uint64_t size = file.u64(16);
    // the stored size is the file's; a truncated copy is still a (damaged) VEGAS project
    return size >= 40;
}

// Real projects nest five or six deep; this stops a damaged file recursing forever.
static const int kMaxDepth = 32;
// Far more chunks than any project has (the biggest sample has under 40,000); guards against crafted files.
static const size_t kMaxChunks = 5000000;

static void readChunks(const Bytes& file, size_t off, size_t end, int depth, Chunk& parent, ChunkTree& tree)
{
    while (off + 24 <= end) {
        if (tree.count >= kMaxChunks) {
            tree.damaged = true;
            tree.problem = "the file has far more parts than a project can have";
            return;
        }
        const uint64_t size = file.u64(off + 16);
        if (size < 24 || size > end - off) {
            tree.damaged = true;
            tree.problem = strfmt("a part at byte %llu runs past the end of the data around it", (unsigned long long)off);
            return;
        }
        Chunk c;
        c.id = file.guid(off);
        c.fileOffset = off;
        ++tree.count;
        const bool nested = c.id == G::List || c.id == G::Riff;
        if (nested && size >= 40) {
            c.isList = true;
            c.type = file.guid(off + 24);
            c.data = file.sub(off + 40, (size_t)size - 40);
            if (depth + 1 >= kMaxDepth) {
                tree.damaged = true;
                tree.problem = "the parts of the file are nested deeper than a project's are";
            } else {
                readChunks(file, off + 40, off + (size_t)size, depth + 1, c, tree);
            }
        } else {
            c.data = file.sub(off + 24, (size_t)size - 24);
        }
        parent.kids.push_back(std::move(c));
        // chunks start on multiples of 8 (the last may end the file unpadded)
        const uint64_t step = (size + 7) & ~(uint64_t)7;
        if (step > end - off) break;
        off += (size_t)step;
    }
    if (off < end && end - off >= 8) {
        // a few stray bytes are normal padding; a header's worth that is not a chunk is not
        bool zeros = true;
        for (size_t i = off; i < end && zeros; ++i) zeros = file.u8(i) == 0;
        if (!zeros && end - off >= 24 && !tree.damaged) {
            tree.damaged = true;
            tree.problem = strfmt("unexpected bytes at %llu", (unsigned long long)off);
        }
    }
}

void readChunkTree(const Bytes& file, ChunkTree& out)
{
    out = ChunkTree();
    if (!looksLikeVeg(file)) {
        out.damaged = true;
        out.problem = "this is not a VEGAS project";
        return;
    }
    // The riff's size can be smaller than the file (trailing bytes) or, in a truncated copy, larger.
    uint64_t size = file.u64(16);
    if (size > file.size()) {
        out.damaged = true;
        out.problem = "the file is shorter than it says it is (it was cut off)";
        size = file.size();
    }
    out.root.id = G::Riff;
    out.root.type = G::VegForm;
    out.root.isList = true;
    out.root.data = file.sub(40, (size_t)size - 40);
    readChunks(file, 40, (size_t)size, 0, out.root, out);
}

// ------------------------------------------------------------------------------------------ OpenFX records
std::string ofxIdFromVegas(const std::string& raw)
{
    std::string s = raw;
    const std::string pre = "{Svfx:";
    if (s.size() > pre.size() && s.compare(0, pre.size(), pre) == 0) s = s.substr(pre.size());
    if (!s.empty() && s.back() == '}') s.pop_back();
    return s;
}

bool isOfxRecordAt(const Bytes& b, size_t off)
{
    if (!b.has(off, 0x30)) return false;
    const uint32_t hs = b.u32(off);
    return hs >= 0x30 && hs <= 0x400 && b.guid(off + 4) == G::OfxEffect;
}

// A double a parameter could hold: zero, or neither denormal nor huge. Two 32-bit integers read as a double
// nearly always land in the denormal range.
static bool plausibleDouble(double d)
{
    if (!std::isfinite(d)) return false;
    const double a = std::fabs(d);
    return a == 0.0 || (a > 1e-200 && a < 1e200);
}

enum class ValKind { None, Int, Double, Text };

// How a parameter's value bytes are laid out: the value (vsize bytes), u32 keyframe count, then the keyframes
// (keySize bytes each, or text keys of varying size).
struct ValLayout {
    size_t vsize = 0;
    size_t keySize = 0;
    ValKind kind = ValKind::None;
    uint32_t keys = 0;
};

// Text: u32 bytes, UTF-16 ending with its 0; keyframes are f64 time, u32 interpolation, u32 bytes and the text.
static bool textLayout(const Bytes& v, ValLayout& out)
{
    const size_t A = v.size();
    const uint32_t n = v.u32(0);
    if (n < 2 || n % 2 != 0 || (size_t)n + 8 > A || v.u16(4 + n - 2) != 0) return false;
    for (size_t i = 4; i + 2 < 4 + (size_t)n; i += 2)
        if (v.u16(i) == 0) return false;
    const uint32_t k = v.u32(4 + n);
    size_t at = 8 + (size_t)n;
    for (uint32_t i = 0; i < k; ++i) {
        if (!v.has(at, 16)) return false;
        const uint32_t m = v.u32(at + 12);
        if (!v.has(at + 16, m)) return false;
        at += 16 + (size_t)m;
    }
    if (at != A) return false;
    out.vsize = 4 + n;
    out.kind = ValKind::Text;
    out.keys = k;
    return true;
}

// A text parameter in Reno's state shape: {"s": text, "ks": [[time, text], ...]} (key times in frames from the
// start of what the effect is on, like numeric keys).
static Json textJson(const Bytes& v, const ValLayout& lay)
{
    Json e = Json::object();
    const uint32_t n = v.u32(0);
    e["s"] = v.text(4, n);
    if (lay.keys > 0) {
        Json ks = Json::array();
        size_t at = 8 + (size_t)n;
        for (uint32_t i = 0; i < lay.keys && v.has(at, 16); ++i) {
            const uint32_t m = v.u32(at + 12);
            Json k = Json::array();
            k.push(v.f64(at));
            k.push(v.text(at + 16, m));
            ks.push(k);
            at += 16 + (size_t)m;
        }
        e["ks"] = ks;
    }
    return e;
}


static bool valueLayout(const Bytes& v, ValLayout& out)
{
    const size_t A = v.size();
    if (A < 8) return false;
    if (textLayout(v, out)) return true;
    // Numbers: the OpenFX type sizes with the key sizes seen: a double key carries in and out tangents
    // (12 + 5 x the value's size), an integer key does not (12 + the value's size).
    struct Try { size_t vsize, keySize; ValKind kind; };
    static const Try tries[] = {
        {8, 52, ValKind::None}, {16, 92, ValKind::Double}, {24, 132, ValKind::Double}, {32, 172, ValKind::Double},
        {4, 16, ValKind::Int}, {4, 32, ValKind::Int}, {12, 24, ValKind::Int}, {12, 72, ValKind::Int}, {8, 20, ValKind::Int},
    };
    for (const Try& t : tries) {
        if (t.vsize + 4 > A) continue;
        const uint32_t k = v.u32(t.vsize);
        if (k > (A - t.vsize - 4) / t.keySize) continue;
        if (A != t.vsize + 4 + (size_t)k * t.keySize) continue;
        out.vsize = t.vsize;
        out.keySize = t.keySize;
        out.keys = k;
        out.kind = t.kind;
        // 8 bytes with no keys: a double, or two 32-bit integers (an integer 2D parameter)
        if (out.kind == ValKind::None) out.kind = (k > 0 || plausibleDouble(v.f64(0))) ? ValKind::Double : ValKind::Int;
        return true;
    }
    return false;
}

// A key's interpolation as stored (OFXInterpolationType: 1 Linear, 2 Fast, 3 Slow, 4 Smooth, 5 Sharp, 6 Hold,
// 7 Manual, 8 Split; every number key in the samples has 1, choices and other whole numbers 0) mapped to Reno's
// OpenFX host curve (Param::KeyCurve: 0 Linear, 1 Fast, 2 Slow, 3 Smooth, 4 Sharp, 5 Hold), and back. Manual and
// Split (Bezier handles) become Linear.
int curveOfOfxInterp(uint32_t v)
{
    return (v >= 2 && v <= 6) ? (int)v - 1 : 0;
}
uint32_t ofxInterpOfCurve(int curve)
{
    return (curve >= 1 && curve <= 5) ? (uint32_t)curve + 1 : 1u;
}

// The "kc" of Reno's state ([[frame, curve], ...], non-Linear keys only) for a number's keys.
static Json keyCurvesJson(const Bytes& v, const ValLayout& lay)
{
    Json kc = Json::array();
    if (lay.kind != ValKind::Double) return kc;
    size_t k0 = lay.vsize + 4;
    for (uint32_t k = 0; k < lay.keys; ++k, k0 += lay.keySize) {
        const int c = curveOfOfxInterp(v.u32(k0 + 8));
        if (c == 0) continue;
        Json one = Json::array();
        one.push(v.f64(k0));
        one.push((double)c);
        kc.push(one);
    }
    return kc;
}

static void pushValue(Json& arr, const Bytes& v, size_t off, size_t vsize, ValKind kind)
{
    if (kind == ValKind::Double) {
        for (size_t i = 0; i + 8 <= vsize; i += 8) arr.push(v.f64(off + i));
    } else {
        for (size_t i = 0; i + 4 <= vsize; i += 4) arr.push((double)(int32_t)v.u32(off + i));
    }
}

bool readOfxRecord(const Bytes& b, size_t off, OfxRecord& out)
{
    out = OfxRecord();
    if (!isOfxRecordAt(b, off)) return false;
    const uint32_t hs = b.u32(off);
    out.flags = b.u32(off + 0x20);
    const uint32_t idLen = b.u32(off + 0x28), preLen = b.u32(off + 0x2c);
    size_t p = off + hs;
    if (!b.has(p, idLen)) return false;
    out.rawId = b.text(p, idLen);
    p += idLen;
    if (!b.has(p, preLen)) return false;
    out.preset = b.text(p, preLen);
    p += preLen;
    out.pluginId = ofxIdFromVegas(out.rawId);
    if (out.pluginId.empty()) return false;
    const uint32_t blk = b.u32(p), count = b.u32(p + 4);
    if (!b.has(p + 8, blk)) return false;
    out.end = p + 8 + blk;
    Json params = Json::object();
    size_t q = p + 8;
    for (uint32_t i = 0; i < count; ++i) {
        if (!b.has(q, 8) || q + 8 > out.end) { ++out.unreadParams; break; }
        const uint32_t vlen = b.u32(q), nlen = b.u32(q + 4);
        if ((size_t)nlen > out.end - q - 8 || (size_t)vlen > out.end - q - 8 - nlen) { ++out.unreadParams; break; }
        const std::string name = b.text(q + 8, nlen);
        const Bytes v = b.sub(q + 8 + nlen, vlen);
        q += 8 + (size_t)nlen + vlen;
        ValLayout lay;
        if (name.empty() || !valueLayout(v, lay)) {
            ++out.unreadParams;
            if (!name.empty()) out.unreadNames.push_back(name);
            continue;
        }
        Json e = Json::object();
        if (lay.kind == ValKind::Text) {
            e = textJson(v, lay);
            if (lay.keys > 0) ++out.keyframedParams;
        } else {
            Json val = Json::array();
            pushValue(val, v, 0, lay.vsize, lay.kind);
            e["v"] = val;
            if (lay.kind == ValKind::Int) e["i"] = true;     // as Reno's OpenFX host marks whole numbers
            if (lay.keys > 0) {
                Json ks = Json::array();
                size_t k0 = lay.vsize + 4;
                for (uint32_t k = 0; k < lay.keys; ++k, k0 += lay.keySize) {
                    Json key = Json::array();
                    key.push(v.f64(k0));                         // frames from the start of the event
                    pushValue(key, v, k0 + 12, lay.vsize, lay.kind);
                    ks.push(key);
                }
                e["k"] = ks;
                const Json kc = keyCurvesJson(v, lay);
                if (kc.size() > 0) e["kc"] = kc;
                ++out.keyframedParams;
            }
        }
        params[name] = e;
    }
    out.state = Json::object();
    out.state["plugin"] = out.pluginId;
    out.state["params"] = params;
    return true;
}

std::vector<OfxRecord> findOfxRecords(const Bytes& b, size_t from)
{
    // the record's class GUID as it is stored; the record starts 4 bytes before it
    static const uint8_t ofxGuid[16] = {0x41, 0x1a, 0xfc, 0xa2, 0xf6, 0x01, 0x0e, 0x46, 0xae, 0x5a, 0x2b, 0xe4, 0xaf, 0x57, 0xb9, 0x2e};
    std::vector<OfxRecord> out;
    size_t f = b.find(ofxGuid, 16, from);
    while (f != std::string::npos && out.size() < 10000) {
        OfxRecord r;
        if (f >= 4 && f - 4 >= from && readOfxRecord(b, f - 4, r) && r.end > f) {
            const size_t next = r.end;
            out.push_back(std::move(r));
            f = b.find(ofxGuid, 16, next);
        } else {
            f = b.find(ofxGuid, 16, f + 1);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------ audio plug-in chains
static std::string base64Of(const Bytes& b)
{
    return base64Encode(b.data(), b.size());
}

// Windows writes GUIDs upper case in braces; Reno's DirectX host names plug-ins that way.
static std::string upperGuid(const std::string& g)
{
    std::string s = g;
    for (auto& c : s) if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return s;
}

// A VST plug-in's VEGAS CLSID: the VST id's four bytes (memory order), 4 more, then a fixed tail.
static bool isVstClsid(const Bytes& b, size_t off)
{
    static const uint8_t tail[8] = {0x02, 0x69, 0x00, 0x07, 0xe9, 0x5d, 0x36, 0xfa};
    return b.has(off, 16) && std::memcmp(b.data() + off + 8, tail, 8) == 0;
}

// A VST program ("CcnK" ... "FxCk": one float per parameter, or "FPCh": the plug-in's own chunk), all big-endian,
// converted to what Reno's VST 2 host loads.
static bool vstState(const Bytes& d, uint32_t& vstId, std::string& state)
{
    if (d.size() < 60 || d.be32(0) != 0x43636e4bu) return false;          // 'CcnK'
    const uint32_t kind = d.be32(8);
    vstId = d.be32(16);
    const uint32_t numParams = d.be32(24);
    if (kind == 0x4678436bu /* FxCk */) {
        if (numParams > (d.size() - 56) / 4) return false;
        std::vector<float> v(numParams);
        for (uint32_t i = 0; i < numParams; ++i) {
            const uint32_t x = d.be32(56 + 4 * (size_t)i);
            std::memcpy(&v[i], &x, 4);
        }
        state = "P:" + base64Encode(v.data(), v.size() * sizeof(float));
        return true;
    }
    if (kind == 0x46504368u /* FPCh */) {
        const uint32_t n = d.be32(56);
        if (!d.has(60, n)) return false;
        state = "Q:" + base64Of(d.sub(60, n));        // a program's chunk (VST's isPreset): see vst2host.cpp
        return true;
    }
    // banks ('FxBk', 'FBCh') would need a program picked out; VEGAS is not seen to write them
    return false;
}

bool readAudioChain(const Bytes& b, size_t off, std::vector<AudioPluginRecord>& out, std::string* problem)
{
    out.clear();
    if (b.guid(off) != G::AudioChain) {
        if (problem) *problem = "the plug-in chain does not start the expected way";
        return false;
    }
    const uint32_t nameLen = b.u32(off + 36);
    if (!b.has(off + 40, nameLen)) { if (problem) *problem = "the plug-in chain is cut short"; return false; }
    size_t p = off + 40 + nameLen;
    p += 8 + 18 + 4;                                  // u64, WAVEFORMATEX, u32
    const uint32_t count = b.u32(p);
    p += 8;
    if (count > 1000) { if (problem) *problem = "the plug-in chain says it holds more plug-ins than it can"; return false; }
    for (uint32_t i = 0; i < count; ++i) {
        if (!b.has(p, 16 + 8)) { if (problem) *problem = "the plug-in chain is cut short"; return !out.empty(); }
        AudioPluginRecord r;
        const size_t at = p;
        r.clsid = b.guid(at);
        r.flags = b.u32(at + 16);
        r.bypass = (r.flags & 1) != 0;
        const uint32_t nl = b.u32(at + 20);
        if (!b.has(at + 24, nl)) { if (problem) *problem = "the plug-in chain is cut short"; return !out.empty(); }
        r.name = b.text(at + 24, nl);
        // the plug-in's data: u32 byte count, the bytes, then a u32 of VEGAS's own (see the header)
        const size_t q = at + 24 + nl + 16;
        const uint32_t dlen = b.u32(q);
        if (!b.has(q + 4, (size_t)dlen + 4)) { if (problem) *problem = "the plug-in chain is cut short"; return !out.empty(); }
        const Bytes data = b.sub(q + 4, dlen);
        r.trailer = b.u32(q + 4 + dlen);
        p = q + 4 + dlen + 4;
        uint32_t vstId = 0;
        std::string st;
        if (data.size() >= 4 && vstState(data.sub(4, data.size() - 4), vstId, st)) {
            r.format = "VST2";
            r.uid = strfmt("%u", (unsigned)vstId);
            r.state = st;
        } else if (isVstClsid(b, at)) {
            r.format = "VST2";
            // the id is also the CLSID's first four bytes backwards ('MX2A' is stored "A2XM")
            r.uid = strfmt("%u", (unsigned)(((uint32_t)b.u8(at + 3) << 24) | ((uint32_t)b.u8(at + 2) << 16) |
                                            ((uint32_t)b.u8(at + 1) << 8) | (uint32_t)b.u8(at)));
            r.problem = "its settings are not in a form Reno can read";
        } else {
            r.format = "DX";
            r.uid = "dshow:{" + upperGuid(r.clsid) + "}";
            // what IPersistStream::Save wrote, whole (VEGAS's own plug-ins start it with its length);
            // VEGAS passes exactly these bytes to IPersistStream::Load
            if (data.size() > 0) r.state = "S:" + base64Of(data);
        }
        out.push_back(r);
    }
    return true;
}

// ------------------------------------------------------------------------------------------ pan/crop
static void riff32(const Bytes& b, size_t off, size_t end, int depth, PanCropRecord& out, bool& ok)
{
    while (off + 8 <= end && ok) {
        const uint32_t cc = b.be32(off);
        const uint32_t sz = b.u32(off + 4);
        if ((size_t)sz > end - off - 8) { ok = false; return; }
        const size_t body = off + 8;
        if (cc == 0x4c495354u /* LIST */) {
            if (sz < 4 || depth > 8) { ok = false; return; }
            const uint32_t type = b.be32(body);
            if (type == 0x4d534b4cu /* MSKL */) {
                // An empty mask list is one MSKK with one 20-byte MSKB of zeros; anything else is a drawn mask.
                bool any = sz > 44;
                const size_t mb = b.find("MSKB", 4, body);
                if (!any && mb != std::string::npos && mb + 8 <= body + sz) {
                    const uint32_t ms = b.u32(mb + 4);
                    for (size_t i = 0; i < ms && mb + 8 + i < body + sz; ++i)
                        if (b.u8(mb + 8 + i)) { any = true; break; }
                }
                out.mask = out.mask || any;
            } else {
                riff32(b, body + 4, body + sz, depth + 1, out, ok);
            }
        } else if (cc == 0x45504353u /* EPCS */) {
            if (sz >= 0x34) {
                out.mediaW = (int)b.u32(body + 0x2c);
                out.mediaH = (int)b.u32(body + 0x30);
            }
        } else if (cc == 0x504f534bu /* POSK */) {
            if (sz >= 84) {
                PanCropRecord::Key k;
                k.time = (double)b.i64(body) / 1e7;
                k.type = b.u32(body + 16);
                k.w = b.f64(body + 20);
                k.h = b.f64(body + 28);
                k.cx = b.f64(body + 36);
                k.cy = b.f64(body + 44);
                k.rot = b.f64(body + 52);
                k.rcx = b.f64(body + 60);
                k.rcy = b.f64(body + 68);
                if (out.keys.size() < 100000) out.keys.push_back(k);
            }
        }
        off = body + sz + (sz & 1);
    }
}

bool readPanCrop(const Bytes& b, PanCropRecord& out)
{
    out = PanCropRecord();
    if (b.size() < 12 || b.be32(0) != 0x4c495354u || b.be32(8) != 0x45504350u /* EPCP */) return false;
    bool ok = true;
    riff32(b, 0, b.size(), 0, out, ok);
    return ok || !out.keys.empty();
}

// ------------------------------------------------------------------------------------------ envelopes
bool readEnvelope(const Bytes& b, EnvelopeRecord& out)
{
    out = EnvelopeRecord();
    const uint32_t first = b.u32(0), count = b.u32(0x18), step = b.u32(0x34);
    if (first < 0x38 || step < 16 || step > 0x1000 || !b.has(0, 0x38)) return false;
    out.type = b.u32(8);
    out.lowest = b.f32(0x0c);
    out.highest = b.f32(0x10);
    out.neutral = b.f32(0x14);
    for (uint32_t i = 0; i < count && i < 1000000; ++i) {
        const size_t at = (size_t)first + (size_t)i * step;
        if (!b.has(at, 16)) return false;
        EnvelopeRecord::Point p;
        p.time = (double)b.i64(at) / 1e7;
        p.value = b.f32(at + 8);
        p.curve = b.i32(at + 12);
        if (!std::isfinite(p.value)) return false;
        out.points.push_back(p);
    }
    return true;
}

std::string envelopeName(uint32_t type)
{
    switch (type) {
    case EnvVolume: return "volume";
    case EnvPan: return "pan";
    case EnvComposite: return "composite level (opacity)";
    case EnvFadeToColor: return "fade to colour";
    case EnvMute: return "mute";
    case EnvVelocity: return "velocity (speed)";
    default: break;
    }
    if (type >= EnvBusA && type <= EnvBusZ) return strfmt("send to Bus %c", (char)('A' + (type - EnvBusA)));
    if (type >= EnvFx1 && type < EnvFx1 + 32) return strfmt("automation of effect %u", type - EnvFx1 + 1);
    return strfmt("type %u", type);
}

// ------------------------------------------------------------------------------------------ track motion
// VEGAS's untouched key of each part (motion, shadow, glow), after its time: zeros except these values. Read off the
// one sample with track motion data; the motion key's width is the frame aspect ratio (frame is 1 high), taken
// from the record itself.
namespace {
struct DefaultValue { size_t off; double v; bool f32; };
const double kAspect = -12345.0;        // stands for "the record's aspect ratio"
const DefaultValue kMotionDefaults[] = {
    {0x24, 1, false}, {0x44, 1, false}, {0x4c, kAspect, false}, {0x54, 1, false}, {0x5c, 0.05, false},
    {0x64, 1, false}, {0x84, 1, false}, {0x8c, 1, false}, {0xc8, -1, false}, {0xd0, 1, false},
};
const DefaultValue kShadowDefaults[] = {
    {0x24, 1, false}, {0x2c, 0.05, false}, {0x34, -0.05, false}, {0x44, 1, false}, {0x4c, 1, false}, {0x54, 1, false},
    {0x5c, 1, false}, {0x64, 1, false}, {0x84, 1, false}, {0x8c, 1, false}, {0xb8, 1, true}, {0xbc, 0.05, false},
};
const DefaultValue kGlowDefaults[] = {
    {0x24, 1, false}, {0x44, 1, false}, {0x4c, 1.05, false}, {0x54, 1.05, false}, {0x5c, 1, false}, {0x64, 1, false},
    {0x84, 1, false}, {0x8c, 1, false}, {0xb0, 1, true}, {0xb4, 1, true}, {0xb8, 1, true}, {0xbc, 0.05, false},
};

bool keyIsDefault(const Bytes& key, const DefaultValue* defs, size_t n, size_t keyBytes, double aspect)
{
    if (key.size() != keyBytes) return false;
    std::vector<uint8_t> want(keyBytes, 0);
    for (size_t i = 0; i < n; ++i) {
        const double v = defs[i].v == kAspect ? aspect : defs[i].v;
        if (defs[i].f32) {
            const float f = (float)v;
            if (defs[i].off + 4 <= keyBytes) std::memcpy(&want[defs[i].off], &f, 4);
        } else if (defs[i].off + 8 <= keyBytes) {
            std::memcpy(&want[defs[i].off], &v, 8);
        }
    }
    // the first 8 bytes are the key's time
    if (keyBytes <= 8 || key.size() < keyBytes || want.size() < keyBytes) return false;
    return std::memcmp(key.data() + 8, want.data() + 8, keyBytes - 8) == 0;
}
}  // namespace

bool readTrackMotion(const Bytes& b, TrackMotionRecord& out)
{
    out = TrackMotionRecord();
    const uint32_t hs = b.u32(0);
    if (hs < 0x20 || hs > 0x100 || !b.has(0, hs)) return false;
    const double aspect = b.f64(0x14);
    const uint32_t parts = b.u32(0x1c);
    if (parts > 16) return false;
    out.parts = (int)parts;
    size_t at = hs;
    bool untouched = parts == 3 && std::isfinite(aspect);
    for (uint32_t i = 0; i < parts; ++i) {
        if (!b.has(at, 4)) return false;
        const uint32_t n = b.u32(at);
        if (n > 100000) return false;
        at += 4;
        out.keys.push_back((int)n);
        for (uint32_t k = 0; k < n; ++k) {
            const uint32_t size = b.u32(at);
            if (size < 12 || !b.has(at, size)) return false;
            const Bytes key = b.sub(at + 4, size - 4);
            if (n != 1) untouched = false;
            else if (i == 0) untouched = untouched && keyIsDefault(key, kMotionDefaults, sizeof kMotionDefaults / sizeof kMotionDefaults[0], 0xd8, aspect);
            else if (i == 1) untouched = untouched && keyIsDefault(key, kShadowDefaults, sizeof kShadowDefaults / sizeof kShadowDefaults[0], 0xd4, aspect);
            else if (i == 2) untouched = untouched && keyIsDefault(key, kGlowDefaults, sizeof kGlowDefaults / sizeof kGlowDefaults[0], 0xd4, aspect);
            at += size;
        }
    }
    out.untouched = untouched;
    return true;
}

// ------------------------------------------------------------------------------------------ compositing modes
namespace {
struct CompositeName { int32_t mode; const char* guid; const char* name; };
const CompositeName kComposite[] = {
    {1, "", "add"}, {2, "", "subtract"}, {3, "", "multiply"}, {4, "", "srcalpha"}, {12, "", "3dsrcalpha"}, {1000, "", "custom"},
    // VEGAS's compositing plug-ins (GUIDs and names as listed in vegas180k.dll)
    {-1, "41bbf5a0-1a37-41d9-ba9e-e9dd0b01dec9", "cut"},
    {-1, "ff881323-40af-4ab7-b679-3bab8e8f33fb", "screen"},
    {-1, "4d6f7103-7b51-427e-894a-d19681734f4d", "overlay"},
    {-1, "7d97a03f-d482-432e-967c-58a6815b6853", "hardlight"},
    {-1, "c7dcb7e6-0c7b-4e33-bb7d-7b09f952b59b", "dodge"},
    {-1, "f7382d2c-fdc9-4e71-8ab2-914d415a8454", "burn"},
    {-1, "12189503-4141-474d-91b9-3d89f32c4923", "darken"},
    {-1, "3e85b55c-e5ed-4cf0-befa-ce6539151485", "lighten"},
    {-1, "c8c5f00a-ea00-429d-8611-9b07e90692d1", "difference"},
    {-1, "7826f660-88f0-4e70-8d41-08bf0ee79992", "differencesquared"},
};
}  // namespace

std::string compositeModeName(int32_t mode, const std::string& guid)
{
    const bool noGuid = guid.empty() || guid == "00000000-0000-0000-0000-000000000000";
    for (const auto& c : kComposite) {
        if (c.mode != mode) continue;
        if (mode == -1 ? guid == c.guid : noGuid) return c.name;
    }
    return std::string();
}

bool compositeModeOf(const std::string& name, int32_t& mode, std::string& guid)
{
    const std::string n = name == "diffsquared" ? "differencesquared" : name;
    for (const auto& c : kComposite)
        if (n == c.name) {
            mode = c.mode;
            guid = c.guid;
            return true;
        }
    return false;
}

// ------------------------------------------------------------------------------------------ track colours and pan types
namespace {
// vegas180.exe's default track colours (the acrTrackColors preference's defaults, as COLORREF 0x00BBGGRR).
const uint32_t kTrackColors[8] = {0xa87359, 0x41347c, 0xa89c59, 0x81415d, 0x7c8933, 0x34767c, 0x815e41, 0x69a859};
const char* const kPanTypes[6] = {"add", "power", "balance0", "balance3", "balance6", "film"};
}  // namespace

uint32_t trackColorOfIndex(uint32_t index)
{
    const uint32_t c = kTrackColors[index % 8];
    return 0xFF000000u | ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
}

uint32_t trackColorIndex(uint32_t argb)
{
    uint32_t best = 0;
    long bestD = -1;
    for (uint32_t i = 0; i < 8; ++i) {
        const uint32_t c = trackColorOfIndex(i);
        const long dr = (long)((c >> 16) & 0xff) - (long)((argb >> 16) & 0xff), dg = (long)((c >> 8) & 0xff) - (long)((argb >> 8) & 0xff),
                   db = (long)(c & 0xff) - (long)(argb & 0xff);
        const long d = dr * dr + dg * dg + db * db;
        if (bestD < 0 || d < bestD) { bestD = d; best = i; }
    }
    return best;
}

std::string panTypeName(uint32_t v) { return v < 6 ? kPanTypes[v] : std::string(); }

bool panTypeOf(const std::string& name, uint32_t& v)
{
    const std::string n = name.empty() ? "balance0" : name;
    for (uint32_t i = 0; i < 6; ++i)
        if (n == kPanTypes[i]) { v = i; return true; }
    return false;
}

}  // namespace veg
}  // namespace reno

// ================================================================================================ writing
namespace reno {
namespace veg {

std::string guidBytes(const char* g)
{
    // same as sscanf "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x" (which was slow on big projects); malformed text gives
    // zeros for the parts it could not read
    auto hv = [](char ch) -> int {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
        return -1;
    };
    static const int pos[16] = {0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34};
    bool plain = std::strlen(g) >= 36 && g[8] == '-' && g[13] == '-' && g[18] == '-' && g[23] == '-';
    unsigned char b[16];
    for (int i = 0; plain && i < 16; ++i) {
        const int h = hv(g[pos[i]]), l = hv(g[pos[i] + 1]);
        if (h < 0 || l < 0) plain = false;
        else b[i] = (unsigned char)(h * 16 + l);
    }
    if (plain) {
        std::string s(16, '\0');
        s[0] = (char)b[3]; s[1] = (char)b[2]; s[2] = (char)b[1]; s[3] = (char)b[0];
        s[4] = (char)b[5]; s[5] = (char)b[4];
        s[6] = (char)b[7]; s[7] = (char)b[6];
        for (int i = 8; i < 16; ++i) s[(size_t)i] = (char)b[i];
        return s;
    }
    unsigned a = 0, bb = 0, c = 0, d[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    std::sscanf(g, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x", &a, &bb, &c, &d[0], &d[1], &d[2], &d[3], &d[4], &d[5], &d[6], &d[7]);
    std::string s(16, '\0');
    for (int i = 0; i < 4; ++i) s[(size_t)i] = (char)((a >> (8 * i)) & 0xff);
    for (int i = 0; i < 2; ++i) s[(size_t)(4 + i)] = (char)((bb >> (8 * i)) & 0xff);
    for (int i = 0; i < 2; ++i) s[(size_t)(6 + i)] = (char)((c >> (8 * i)) & 0xff);
    for (int i = 0; i < 8; ++i) s[(size_t)(8 + i)] = (char)d[i];
    return s;
}

std::string utf16(const std::string& u, bool withZero)
{
    std::string out;
    size_t i = 0;
    auto put = [&](uint32_t c) {
        out += (char)(c & 0xff);
        out += (char)((c >> 8) & 0xff);
    };
    while (i < u.size()) {
        const uint8_t c0 = (uint8_t)u[i];
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c0 < 0x80) cp = c0;
        else if ((c0 & 0xE0) == 0xC0 && i + 1 < u.size()) { cp = ((c0 & 0x1Fu) << 6) | ((uint8_t)u[i + 1] & 0x3Fu); n = 2; }
        else if ((c0 & 0xF0) == 0xE0 && i + 2 < u.size()) {
            cp = ((c0 & 0x0Fu) << 12) | (((uint8_t)u[i + 1] & 0x3Fu) << 6) | ((uint8_t)u[i + 2] & 0x3Fu);
            n = 3;
        } else if ((c0 & 0xF8) == 0xF0 && i + 3 < u.size()) {
            cp = ((c0 & 0x07u) << 18) | (((uint8_t)u[i + 1] & 0x3Fu) << 12) | (((uint8_t)u[i + 2] & 0x3Fu) << 6) | ((uint8_t)u[i + 3] & 0x3Fu);
            n = 4;
        }
        i += n;
        if (cp >= 0x10000 && cp <= 0x10FFFF) {
            cp -= 0x10000;
            put(0xD800 + (cp >> 10));
            put(0xDC00 + (cp & 0x3FF));
        } else {
            put(cp > 0xFFFF ? 0xFFFD : cp);
        }
    }
    if (withZero) put(0);
    return out;
}

static void setBytes(std::string& s, size_t off, const void* p, size_t n)
{
    if (off > s.size() || n > s.size() - off) return;
    std::memcpy(&s[off], p, n);
}
void setU16(std::string& s, size_t off, uint16_t v)
{
    const uint8_t b[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    setBytes(s, off, b, 2);
}
void setU32(std::string& s, size_t off, uint32_t v)
{
    const uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    setBytes(s, off, b, 4);
}
void setU64(std::string& s, size_t off, uint64_t v)
{
    setU32(s, off, (uint32_t)v);
    setU32(s, off + 4, (uint32_t)(v >> 32));
}
void setF32(std::string& s, size_t off, float v)
{
    uint32_t x;
    std::memcpy(&x, &v, 4);
    setU32(s, off, x);
}
void setF64(std::string& s, size_t off, double v)
{
    uint64_t x;
    std::memcpy(&x, &v, 8);
    setU64(s, off, x);
}
void addU32(std::string& s, uint32_t v) { s.append(4, '\0'); setU32(s, s.size() - 4, v); }
void addU64(std::string& s, uint64_t v) { s.append(8, '\0'); setU64(s, s.size() - 8, v); }
void addF32(std::string& s, float v) { s.append(4, '\0'); setF32(s, s.size() - 4, v); }
void addF64(std::string& s, double v) { s.append(8, '\0'); setF64(s, s.size() - 8, v); }

int64_t toTicks(double seconds)
{
    if (!std::isfinite(seconds)) return 0;
    const double t = seconds * 1e7;
    if (t > 9.0e18) return INT64_MAX;
    if (t < -9.0e18) return INT64_MIN;
    return (int64_t)std::llround(t);
}

void pad8(std::string& s)
{
    while (s.size() % 8) s += '\0';
}

OutNode copyNode(const Chunk& c)
{
    OutNode n;
    n.isList = c.isList;
    n.id = guidBytes(c.isList ? c.type.c_str() : c.id.c_str());
    if (c.isList) {
        n.kids.reserve(c.kids.size());
        for (const auto& k : c.kids) n.kids.push_back(copyNode(k));
    } else {
        n.data.assign((const char*)c.data.data(), c.data.size());
    }
    return n;
}

OutNode chunkNode(const char* guid, std::string data)
{
    OutNode n;
    n.id = guidBytes(guid);
    n.data = std::move(data);
    return n;
}

OutNode listNode(const char* type)
{
    OutNode n;
    n.isList = true;
    n.id = guidBytes(type);
    return n;
}

static void writeNode(const OutNode& n, std::string& out, bool riff)
{
    const size_t start = out.size();
    if (n.isList || riff) {
        out += guidBytes(riff ? G::Riff : G::List);
        out.append(8, '\0');
        out += riff ? guidBytes(G::VegForm) : n.id;
        for (const auto& k : n.kids) {
            writeNode(k, out, false);
            while ((out.size() - start) % 8) out += '\0';
        }
    } else {
        out += n.id;
        out.append(8, '\0');
        out += n.data;
        while ((out.size() - start) % 8) out += '\0';     // VEGAS counts a record's padding in its size
    }
    setU64(out, start + 16, (uint64_t)(out.size() - start));
}

std::string writeTree(const OutNode& top)
{
    std::string out;
    out.reserve(1 << 20);
    writeNode(top, out, true);
    return out;
}

// ------------------------------------------------------------------------------------------ OpenFX records
namespace {

// One parameter as VEGAS wrote it.
struct OfxParamBytes {
    std::string name;
    Bytes nameBytes, value;         // value: everything after the name
    ValLayout lay;
    bool laid = false;
};

// A parameter's value (and keys) in Reno's state shape, for comparing.
Json paramJson(const Bytes& v, const ValLayout& lay)
{
    Json e = Json::object();
    if (lay.kind == ValKind::Text) return textJson(v, lay);
    Json val = Json::array();
    pushValue(val, v, 0, lay.vsize, lay.kind);
    e["v"] = val;
    if (lay.keys > 0) {
        Json ks = Json::array();
        size_t k0 = lay.vsize + 4;
        for (uint32_t k = 0; k < lay.keys; ++k, k0 += lay.keySize) {
            Json key = Json::array();
            key.push(v.f64(k0));
            pushValue(key, v, k0 + 12, lay.vsize, lay.kind);
            ks.push(key);
        }
        e["k"] = ks;
        const Json kc = keyCurvesJson(v, lay);
        if (kc.size() > 0) e["kc"] = kc;
    }
    return e;
}

bool sameNumbers(const Json& a, const Json& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const Json& x = a.at(i);
        const Json& y = b.at(i);
        if (x.isArray() || y.isArray()) {
            if (!sameNumbers(x, y)) return false;
        } else if (x.asDouble() != y.asDouble()) {
            return false;
        }
    }
    return true;
}

// Whether Reno's entry for a parameter says the same as what VEGAS wrote.
bool sameParam(const Json& reno, const Json& vegas)
{
    if (vegas.has("s")) {
        if (!reno.has("s") || reno.get("s").asString() != vegas.get("s").asString()) return false;
        // keys: same times and texts
        const Json& rk = reno.get("ks");
        const Json& vk = vegas.get("ks");
        if (rk.size() != vk.size()) return false;
        for (size_t i = 0; i < rk.size(); ++i)
            if (rk.at(i).at(0).asDouble() != vk.at(i).at(0).asDouble() || rk.at(i).at(1).asString() != vk.at(i).at(1).asString()) return false;
        return true;
    }
    if (!sameNumbers(reno.get("v"), vegas.get("v"))) return false;
    const Json& rk = reno.get("k");
    const Json& vk = vegas.get("k");
    if (rk.size() != vk.size()) return false;
    if (!sameNumbers(rk, vk)) return false;
    // keys' curves: same non-Linear ones
    return sameNumbers(reno.get("kc"), vegas.get("kc"));
}

void putNumber(std::string& s, double v, bool whole)
{
    if (whole) {
        const double r = std::round(std::isfinite(v) ? v : 0.0);
        addU32(s, (uint32_t)(int32_t)std::max(-2147483648.0, std::min(2147483647.0, r)));
    } else {
        addF64(s, std::isfinite(v) ? v : 0.0);
    }
}

std::string textValue(const std::string& t, uint32_t keys)
{
    std::string s;
    const std::string u = utf16(t, true);
    addU32(s, (uint32_t)u.size());
    s += u;
    addU32(s, keys);
    return s;
}

// A text parameter's bytes for Reno's entry 'e' ({"s", "ks"}): the text, then its keys (f64 time, u32 interpolation,
// u32 bytes, the text). A key keeps the interpolation of the original's key at its time, else the original's first
// key, else 1 (linear, as in the samples).
std::string textValueKeyed(const Json& e, const Bytes* orig)
{
    const Json& ks = e.get("ks");
    std::string s = textValue(e.get("s").asString(), (uint32_t)ks.size());
    std::map<double, uint32_t> interp;
    uint32_t first = 1;
    if (orig && orig->size() >= 8) {
        const uint32_t n = orig->u32(0), k = orig->u32(4 + n);
        size_t at = 8 + (size_t)n;
        for (uint32_t i = 0; i < k && orig->has(at, 16); ++i) {
            if (i == 0) first = orig->u32(at + 8);
            interp[orig->f64(at)] = orig->u32(at + 8);
            at += 16 + (size_t)orig->u32(at + 12);
        }
    }
    for (const auto& k : ks.items()) {
        const double t = k.at(0).asDouble();
        const std::string u = utf16(k.at(1).asString(), true);
        addF64(s, t);
        auto it = interp.find(t);
        addU32(s, it != interp.end() ? it->second : first);
        addU32(s, (uint32_t)u.size());
        s += u;
    }
    return s;
}

// A parameter's value bytes for Reno's entry 'e': 'dims' numbers of 4 bytes (whole) or 8, then its keys. Keys at
// the times of the original's keys keep their interpolation and tangents (with the new value).
std::string numberValue(const Json& e, size_t dims, bool whole, const Bytes* orig, const ValLayout* olay)
{
    const size_t vsize = dims * (whole ? 4 : 8);
    std::string s;
    const Json& v = e.get("v");
    for (size_t d = 0; d < dims; ++d) putNumber(s, d < v.size() ? v.at(d).asDouble() : 0.0, whole);
    const Json& ks = e.get("k");
    addU32(s, (uint32_t)ks.size());
    for (const auto& k : ks.items()) {
        const double t = k.size() > 0 ? k.at(0).asDouble() : 0.0;
        std::string key;
        addF64(key, t);
        // the original key at this time, if any
        size_t ok = std::string::npos;
        if (orig && olay && olay->keys > 0 && olay->vsize == vsize) {
            size_t k0 = olay->vsize + 4;
            for (uint32_t i = 0; i < olay->keys; ++i, k0 += olay->keySize)
                if (std::fabs(orig->f64(k0) - t) < 1e-9) { ok = k0; break; }
        }
        // interpolation: whole numbers keep the original's (0); a number's comes from its curve in "kc" (the original
        // key keeps its own while it means the same curve; Manual and Split count as Linear)
        uint32_t interp = ok != std::string::npos ? orig->u32(ok + 8) : (whole ? 0u : 1u);
        if (!whole) {
            int curve = 0;
            for (const auto& c : e.get("kc").items())
                if (c.size() >= 2 && std::fabs(c.at(0).asDouble() - t) < 1e-9) curve = c.at(1).asInt();
            if (ok == std::string::npos || curveOfOfxInterp(interp) != curve) interp = ofxInterpOfCurve(curve);
        }
        addU32(key, interp);
        for (size_t d = 0; d < dims; ++d) putNumber(key, d + 1 < k.size() ? k.at(d + 1).asDouble() : 0.0, whole);
        if (!whole) {
            // tangents: per dimension the in handle's time offset and value, then the out handle's
            std::vector<double> inT(dims, -0.1), outT(dims, 0.1);
            if (ok != std::string::npos) {
                for (size_t d = 0; d < dims; ++d) {
                    inT[d] = orig->f64(ok + 12 + vsize + 8 * d);
                    outT[d] = orig->f64(ok + 12 + vsize + 2 * vsize + 8 * d);
                }
            }
            for (size_t d = 0; d < dims; ++d) addF64(key, inT[d]);
            for (size_t d = 0; d < dims; ++d) addF64(key, d + 1 < k.size() ? k.at(d + 1).asDouble() : 0.0);
            for (size_t d = 0; d < dims; ++d) addF64(key, outT[d]);
            for (size_t d = 0; d < dims; ++d) addF64(key, d + 1 < k.size() ? k.at(d + 1).asDouble() : 0.0);
        }
        s += key;
    }
    return s;
}

}  // namespace

OfxWriteResult writeOfxRecord(const OfxWriteInput& in)
{
    OfxWriteResult res;
    const Bytes& o = in.original;
    // the original's parameters, by name, in order
    std::vector<OfxParamBytes> orig;
    std::string header;
    if (isOfxRecordAt(o, 0)) {
        const uint32_t hs = o.u32(0);
        header.assign((const char*)o.data(), hs);
        size_t p = hs + o.u32(0x28) + o.u32(0x2c);
        const uint32_t blk = o.u32(p), count = o.u32(p + 4);
        const size_t end = std::min(o.size(), p + 8 + (size_t)blk);
        size_t q = p + 8;
        for (uint32_t i = 0; i < count && q + 8 <= end; ++i) {
            const uint32_t vlen = o.u32(q), nlen = o.u32(q + 4);
            if ((size_t)nlen > end - q - 8 || (size_t)vlen > end - q - 8 - nlen) break;
            OfxParamBytes pb;
            pb.nameBytes = o.sub(q + 8, nlen);
            pb.name = o.text(q + 8, nlen);
            pb.value = o.sub(q + 8 + nlen, vlen);
            pb.laid = valueLayout(pb.value, pb.lay);
            orig.push_back(pb);
            q += 8 + (size_t)nlen + vlen;
        }
    } else if (in.headerTemplate.size() >= 0x30 && isOfxRecordAt(in.headerTemplate, 0)) {
        header.assign((const char*)in.headerTemplate.data(), in.headerTemplate.u32(0));
        setU32(header, 0x1c, 0);        // plug-in specific; 0 in nearly every effect VEGAS wrote
        setU32(header, 0x24, 0);        // counts edits to the effect; 0 for a new one
    } else {
        // the header every OpenFX effect of VEGAS 18 starts with (see vegformat.h)
        header.assign(0x30, '\0');
        setU32(header, 0, 0x30);
        header.replace(4, 16, guidBytes(G::OfxEffect));
        setU32(header, 0x14, 2);
        setU32(header, 0x18, 1);
    }
    setU32(header, 0x20, in.flags);
    const std::string id16 = utf16(in.rawId, true), pre16 = utf16(in.preset, true);
    setU32(header, 0x28, (uint32_t)id16.size());
    setU32(header, 0x2c, (uint32_t)pre16.size());

    const Json& params = in.state.get("params");
    std::string block;
    uint32_t count = 0;
    std::set<std::string> done;
    auto addParam = [&](const std::string& nameBytes, const std::string& value) {
        addU32(block, (uint32_t)value.size());
        addU32(block, (uint32_t)nameBytes.size());
        block += nameBytes;
        block += value;
        ++count;
    };
    for (const auto& pb : orig) {
        done.insert(pb.name);
        const std::string nb((const char*)pb.nameBytes.data(), pb.nameBytes.size());
        const std::string ov((const char*)pb.value.data(), pb.value.size());
        if (!params.has(pb.name) || !pb.laid) { addParam(nb, ov); continue; }
        const Json& e = params.get(pb.name);
        if (sameParam(e, paramJson(pb.value, pb.lay))) { addParam(nb, ov); continue; }
        if (pb.lay.kind == ValKind::Text) {
            addParam(nb, textValueKeyed(e, &pb.value));
        } else if (e.has("v")) {
            const bool whole = pb.lay.kind == ValKind::Int;
            const size_t dims = pb.lay.vsize / (whole ? 4 : 8);
            addParam(nb, numberValue(e, dims, whole, &pb.value, &pb.lay));
        } else {
            addParam(nb, ov);
        }
    }
    // what another record of the same effect says about parameter layout (for parameters Reno's state does not mark
    // as whole numbers or not)
    std::map<std::string, ValLayout> layouts;
    if (isOfxRecordAt(in.layout, 0)) {
        const Bytes& l = in.layout;
        size_t p = l.u32(0) + l.u32(0x28) + l.u32(0x2c);
        const uint32_t blk = l.u32(p), count = l.u32(p + 4);
        const size_t end = std::min(l.size(), p + 8 + (size_t)blk);
        size_t q = p + 8;
        for (uint32_t i = 0; i < count && q + 8 <= end; ++i) {
            const uint32_t vlen = l.u32(q), nlen = l.u32(q + 4);
            if ((size_t)nlen > end - q - 8 || (size_t)vlen > end - q - 8 - nlen) break;
            ValLayout lay;
            if (valueLayout(l.sub(q + 8 + nlen, vlen), lay)) layouts[l.text(q + 8, nlen)] = lay;
            q += 8 + (size_t)nlen + vlen;
        }
    }
    // parameters VEGAS did not write for this effect (or all of them, for a new one)
    for (const auto& m : params.members()) {
        if (done.count(m.first)) continue;
        const Json& e = m.second;
        const std::string nb = utf16(m.first, true);
        auto lay = layouts.find(m.first);
        if (e.has("s")) {
            addParam(nb, textValueKeyed(e, nullptr));
        } else if (e.has("v") && e.get("v").size() >= 1 && e.get("v").size() <= 4 && e.has("i")) {
            addParam(nb, numberValue(e, e.get("v").size(), e.get("i").asBool(), nullptr, nullptr));
        } else if (e.has("v") && lay != layouts.end() && lay->second.kind != ValKind::Text && lay->second.kind != ValKind::None) {
            const bool whole = lay->second.kind == ValKind::Int;
            addParam(nb, numberValue(e, lay->second.vsize / (whole ? 4 : 8), whole, nullptr, nullptr));
        } else {
            // whole-number-ness is not in the state: left out rather than written in a layout VEGAS would misread
            res.dropped.push_back(m.first);
        }
    }
    std::string rec = header;
    rec += id16;
    rec += pre16;
    addU32(rec, (uint32_t)block.size());
    addU32(rec, count);
    rec += block;
    res.record = rec;
    return res;
}

// ------------------------------------------------------------------------------------------ audio chains
std::string audioEntry(const AudioEntryInput& in)
{
    std::string s = in.clsid16;
    s.resize(16, '\0');
    addU32(s, in.bypass ? 1u : 0u);
    const std::string n = utf16(in.name, true);
    addU32(s, (uint32_t)n.size());
    s += n;
    s += in.clsid16.substr(0, 16);
    s.resize(s.size() + (16 - std::min<size_t>(16, in.clsid16.size())), '\0');
    if (in.vst) {
        addU32(s, (uint32_t)in.data.size() + 4);
        addU32(s, in.vstLead);
    } else {
        addU32(s, (uint32_t)in.data.size());
    }
    s += in.data;
    addU32(s, in.trailer);
    return s;
}

static void addBe32(std::string& s, uint32_t v)
{
    s += (char)(v >> 24);
    s += (char)(v >> 16);
    s += (char)(v >> 8);
    s += (char)v;
}

bool vstProgram(const std::string& state, uint32_t vstId, const Bytes& orig, std::string& out)
{
    if (state.size() < 2 || state[1] != ':') return false;
    const std::string data = base64Decode(state.substr(2));
    // what the original program had that Reno does not keep: plug-in version and program name
    // the program file's format version: VEGAS writes 0
    uint32_t version = 1, formatVersion = 0;
    std::string name(28, '\0');
    if (orig.size() >= 56 && orig.be32(0) == 0x43636e4bu) {
        formatVersion = orig.be32(12);
        version = orig.be32(20);
        name.assign((const char*)orig.data() + 28, 28);
    }
    std::string p;
    if (state[0] == 'P') {
        const size_t n = data.size() / 4;
        addBe32(p, 0x43636e4bu);                        // 'CcnK'
        addBe32(p, (uint32_t)(48 + 4 * n));             // bytes after this field
        addBe32(p, 0x4678436bu);                        // 'FxCk'
        addBe32(p, formatVersion);
        addBe32(p, vstId);
        addBe32(p, version);
        addBe32(p, (uint32_t)n);
        p += name;
        for (size_t i = 0; i < n; ++i) {
            uint32_t x;
            std::memcpy(&x, data.data() + 4 * i, 4);
            addBe32(p, x);
        }
    } else if (state[0] == 'C' || state[0] == 'Q') {
        // VEGAS writes the current program's chunk; a bank's chunk (Reno's "C:") is written the same way, so VEGAS
        // hands the plug-in a bank as if it were a program
        addBe32(p, 0x43636e4bu);
        addBe32(p, (uint32_t)(52 + data.size()));
        addBe32(p, 0x46504368u);                        // 'FPCh'
        addBe32(p, formatVersion);
        addBe32(p, vstId);
        addBe32(p, version);
        addBe32(p, orig.size() >= 28 && orig.be32(8) == 0x46504368u ? orig.be32(24) : 0u);  // numParams
        p += name;
        addBe32(p, (uint32_t)data.size());
        p += data;
    } else {
        return false;
    }
    out = p;
    return true;
}

bool splitAudioChain(const Bytes& b, AudioChainParts& out)
{
    out = AudioChainParts();
    if (b.guid(0) != G::AudioChain) return false;
    const uint32_t nameLen = b.u32(36);
    if (!b.has(40, nameLen)) return false;
    size_t p = 40 + (size_t)nameLen + 8 + 18 + 4;
    out.countOff = p;
    const uint32_t count = b.u32(p);
    p += 8;
    if (!b.has(0, p) || count > 1000) return false;
    out.head.assign((const char*)b.data(), p);
    for (uint32_t i = 0; i < count; ++i) {
        if (!b.has(p, 24)) return false;
        const uint32_t nl = b.u32(p + 20);
        if (!b.has(p + 24, nl)) return false;
        const size_t q = p + 24 + nl + 16;
        const uint32_t dlen = b.u32(q);
        if (!b.has(q + 8, dlen)) return false;
        out.entries.push_back({p, q + 8 + dlen - p});
        p = q + 8 + dlen;
    }
    out.tail.assign((const char*)b.data() + p, b.size() - p);
    return true;
}

// ------------------------------------------------------------------------------------------ command markers
void splitMarkerCommand(const std::string& text, std::string& command, std::string& parameter)
{
    command.clear();
    parameter.clear();
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\t' && i + 1 < text.size()) { command += text[++i]; continue; }
        if (ch == ':') { parameter = text.substr(i + 1); return; }
        command += ch;
    }
}

std::string joinMarkerCommand(const std::string& command, const std::string& parameter)
{
    std::string s;
    for (char ch : command) {
        if (ch == ':' || ch == '\t') s += '\t';
        s += ch;
    }
    return s + ":" + parameter;
}

}  // namespace veg
}  // namespace reno
