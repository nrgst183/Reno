// vegmatrix_diff: for every .veg file made by the VEGAS script "Reno Settings Matrix", lists which bytes and fields
// differ from the base project, so each setting can be tied to its place in the file.
//
//   vegmatrix_diff [options] <folder>                  the folder the script wrote (manifest.txt names each file's base)
//   vegmatrix_diff [options] base.veg other.veg ...    each other file against the first
//
// The files are read with the chunk walker of vegaudit (veg::readChunkTree). The two trees are laid side by side: the
// chunks of a list are matched by GUID in order (longest common subsequence, so an added or removed chunk does not
// shift the rest), matched chunks of the same size are compared word by word (4-byte, little-endian), and chunks of
// different size as "the middle part that differs".
//
// The script also saves the base opened and saved again with nothing changed (the "control" file). Whatever differs
// there (times, counters, ids, random bytes) is noise; a variant's change at the same place is hidden unless --noise.
//
//   --control FILE   the control file (default: the one in the manifest / a file named *control*.veg)
//   --noise          show the changes at places where the control differs from the base as well
//   --full           show every changed word (default: the first 12 of a chunk)
//   --brief          one line per file
//   --no-map         leave out the final table "place -> files that change it"
//   --map-only       only that table
//   --csv FILE       also write every change as a row: file, base, change, chunk, offset, old, new, field
//   --only TEXT      only the files whose name contains TEXT
//
// Output: for each file the chunk path (names as in vegaudit; "#n" counts same-named siblings), the offset in the chunk,
// the old and the new word (hex), and what Reno knows the place to be ("field"). Exit code 0; 2 for bad arguments.
#include "../../src/vegas/vegformat.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace reno;

namespace vmd {

// ------------------------------------------------------------------------------------------ files
struct Loaded {
    std::string name;
    std::vector<uint8_t> buf;
    veg::ChunkTree tree;
    bool ok = false;
    std::string problem;
};

inline std::unique_ptr<Loaded> loadFile(const std::string& path, const std::string& name)
{
    std::unique_ptr<Loaded> l(new Loaded());
    l->name = name;
    std::ifstream f(path, std::ios::binary);
    if (!f) { l->problem = "cannot open"; return l; }
    l->buf.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    veg::Bytes all(l->buf.data(), l->buf.size());
    if (!veg::looksLikeVeg(all)) { l->problem = "not a VEGAS project"; return l; }
    veg::readChunkTree(all, l->tree);
    l->ok = true;
    if (l->tree.damaged) l->problem = l->tree.problem;      // read as far as it goes
    return l;
}

// ------------------------------------------------------------------------------------------ names
inline const std::map<std::string, std::string>& names()
{
    namespace G = veg::G;
    static const std::map<std::string, std::string> m = {
        {G::Header, "ProjectHeader"}, {G::MediaList, "MediaList"}, {G::Media, "Media"}, {G::Tracks, "Tracks"},
        {G::Track, "Track"}, {G::TrackHeader, "TrackHeader"}, {G::Event, "Event"}, {G::EventData, "EventData"},
        {G::Take, "Take"}, {G::VideoEventList, "VideoEventList"}, {G::VideoEventData, "VideoEventData"},
        {G::AudioEventList, "AudioEventList"}, {G::EventFx, "EventVideoFx"}, {G::TrackFx, "TrackVideoFx"},
        {G::Fx, "FxRecord"}, {G::PanCrop, "PanCrop"}, {G::EventEnvelopes, "EventEnvelopes"},
        {G::TrackEnvelopes, "TrackEnvelopes"}, {G::EnvelopeHeader, "EnvelopeHeader"}, {G::EnvelopePoints, "EnvelopePoints"},
        {G::EventAudioFx, "EventAudioFx"}, {G::TrackAudioFx, "TrackAudioFx"}, {G::VideoTrackList, "VideoTrackList"},
        {G::VideoTrackData, "VideoTrackData"}, {G::AudioTrackList, "AudioTrackList"}, {G::AudioTrackData, "AudioTrackData"},
        {G::TrackMotionList, "TrackMotionList"}, {G::TrackMotion, "TrackMotion"}, {G::TrackExtraA, "TrackExtraA"},
        {G::TrackExtraB, "TrackExtraB"}, {G::Groups, "Groups"}, {G::Group, "Group"}, {G::Markers, "Markers"},
        {G::Marker, "Marker"}, {G::CommandMarkers, "CommandMarkers"}, {G::MarkersB, "MarkersB"}, {G::CdMarkers, "CdMarkers"},
        {G::Summary, "Summary"}, {G::SummaryData, "SummaryData"}, {G::Buses, "Buses"}, {G::Mixer, "Mixer"},
        {G::Bus, "Bus"}, {G::ViewList, "ViewList"}, {G::ViewData, "ViewData"}, {G::Timelines, "Timelines"},
        {G::TimelineList, "TimelineList"}, {G::TimelineData, "TimelineData"}, {G::WindowState, "WindowState"},
        {G::EmptyListA, "EmptyListA"}, {G::EmptyChunkA, "EmptyChunkA"}};
    return m;
}

inline std::string guidOf(const veg::Chunk& c) { return c.isList ? c.type : c.id; }

inline std::string nameOf(const veg::Chunk& c)
{
    const std::string g = guidOf(c);
    auto it = names().find(g);
    const std::string n = it != names().end() ? it->second : g.substr(0, 8);
    return c.isList ? "[" + n + "]" : n;
}

// ------------------------------------------------------------------------------------------ what a place is
// Words whose meaning Reno knows (read off the samples and VEGAS's own export; see vegas.cpp, vegaudit_report.md).
inline std::string fieldNote(const std::string& chunkName, const veg::Bytes& data, size_t off)
{
    char buf[96];
    if (chunkName == "ProjectHeader") {
        switch (off & ~(size_t)3) {
        case 0x00: return "offset of the paths";
        case 0x08: return "header word 0x08 (1,3,7 in the samples; not the ruler format)";
        case 0x0c: return "audio sample rate";
        case 0x10: case 0x14: return "frame rate (f64)";
        case 0x18: case 0x1c: return "tempo (f64)";
        case 0x28: return "beats per measure (u16)";
        case 0x48: case 0x4c: return "last event id";
        }
        // the video blocks start after the four texts; each is 0x358 bytes
        size_t at = data.u32(0);
        if (at >= 0x40 && at <= data.size()) {
            size_t p = at;
            data.textZ(p, &p);
            for (int i = 0; i < 3; ++i) data.textZ(p, &p);
            if (off >= p && off < p + 3 * 0x358) {
                const size_t blk = (off - p) / 0x358, in = (off - p) % 0x358;
                const char* what = in == 0x1c ? " render quality" : in == 0xf4 ? " deinterlace" : in == 4 ? " width" : in == 8 ? " height" :
                                   (in >= 0x38 && in < 0x40) ? " frame rate" : (in >= 0x40 && in < 0x48) ? " pixel aspect" : "";
                std::snprintf(buf, sizeof buf, "video block %zu +0x%zx%s", blk, in, what);
                return buf;
            }
            if (off >= at && off < p) return "paths / texts";
            if (off >= p + 3 * 0x358) {
                std::snprintf(buf, sizeof buf, "after the video blocks +0x%zx", off - (p + 3 * 0x358));
                return buf;
            }
        }
    } else if (chunkName == "VideoEventData") {
        if ((off & ~(size_t)3) == 4) return "flags (0x200 = resample on)";
    }
    return "";
}

// ------------------------------------------------------------------------------------------ the differences
struct Change {
    std::string chunk;          // path with sibling indices
    std::string chunkName;      // the chunk's own name
    std::string kind;           // word | span | added | removed
    size_t off = 0;             // word: offset of the word; span: where the differing middle starts
    uint32_t a = 0, b = 0;      // word: old and new value
    size_t aLen = 0, bLen = 0;  // span: the lengths of the differing middle; added / removed: the chunk's size
    std::string aHex, bHex;     // span: first bytes
    std::string aText, bText;   // span: as UTF-16 text if it looks like text
    std::string field;
    std::string key;            // for noise and the map
    bool noise = false;
};

inline std::string hexWord(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08x", v); return b; }

inline std::string hexBytes(const uint8_t* p, size_t n, size_t cap = 24)
{
    std::string s;
    char b[8];
    for (size_t i = 0; i < n && i < cap; ++i) { std::snprintf(b, sizeof b, "%02x", p[i]); s += b; if (i + 1 < n && i + 1 < cap) s += ' '; }
    if (n > cap) s += " ..";
    return s;
}

// UTF-16LE that is printable ASCII/Latin text, three characters or more; else empty
inline std::string asText(const uint8_t* p, size_t n)
{
    if (n < 6) return "";
    std::string s;
    for (size_t i = 0; i + 1 < n && s.size() < 40; i += 2) {
        const unsigned c = p[i] | (p[i + 1] << 8);
        if (c == 0) break;
        if (c < 0x20 || c > 0x7e) return "";
        s += (char)c;
    }
    return s.size() >= 3 ? s : "";
}

inline uint32_t word(const veg::Bytes& d, size_t off) { return d.u32(off); }

// The project header holds the project's own path and three more texts before the video blocks, so a variant saved under
// a longer file name has a longer header and everything after the texts would differ as one shifted span. The header is
// therefore compared in three parts: the words before the texts, each text, and the words after (offsets counted from
// where the texts end).
inline bool headerParts(const veg::Bytes& d, size_t& at, size_t& after, std::string texts[4])
{
    if (d.size() < 0x50) return false;
    at = d.u32(0);
    if (at < 0x40 || at > d.size()) return false;
    d.bad = false;
    size_t p = at;
    for (int i = 0; i < 4; ++i) texts[i] = d.textZ(p, &p);
    if (d.bad || p > d.size()) return false;
    after = p;
    return true;
}

inline std::string blockNote(size_t rel)
{
    char buf[96];
    if (rel < 3 * 0x358) {
        const size_t blk = rel / 0x358, in = rel % 0x358;
        const char* what = in == 0x1c ? " render quality" : in == 0xf4 ? " deinterlace" : in == 4 ? " width" : in == 8 ? " height" :
                           (in >= 0x38 && in < 0x40) ? " frame rate" : (in >= 0x40 && in < 0x48) ? " pixel aspect" : "";
        std::snprintf(buf, sizeof buf, "video block %zu +0x%zx%s", blk, in, what);
    } else {
        std::snprintf(buf, sizeof buf, "after the video blocks +0x%zx", rel - 3 * 0x358);
    }
    return buf;
}

inline bool diffHeader(const std::string& path, const veg::Chunk& ca, const veg::Chunk& cb, std::vector<Change>& out)
{
    size_t atA, atB, pA, pB;
    std::string tA[4], tB[4];
    if (!headerParts(ca.data, atA, pA, tA) || !headerParts(cb.data, atB, pB, tB)) return false;
    const veg::Bytes& A = ca.data;
    const veg::Bytes& B = cb.data;
    const std::string cname = "ProjectHeader";
    char k[48];
    const size_t head = std::min(atA, atB);
    for (size_t w = 0; w + 4 <= head; w += 4) {
        const uint32_t x = word(A, w), y = word(B, w);
        if (x == y) continue;
        Change c;
        c.chunk = path; c.chunkName = cname; c.kind = "word"; c.off = w; c.a = x; c.b = y;
        c.field = fieldNote(cname, A, w);
        std::snprintf(k, sizeof k, "+0x%zx", w);
        c.key = path + k;
        out.push_back(c);
    }
    for (int i = 0; i < 4; ++i) {
        if (tA[i] == tB[i]) continue;
        Change c;
        c.chunk = path; c.chunkName = cname; c.kind = "text"; c.off = (size_t)i;
        c.aText = tA[i]; c.bText = tB[i];
        c.field = i == 0 ? "the project's own path (differs in every file)" : "text " + std::to_string(i);
        std::snprintf(k, sizeof k, "~text%d", i);
        c.key = path + k;
        out.push_back(c);
    }
    const size_t la = A.size() - pA, lb = B.size() - pB;
    const std::string tail = path + "{after texts}";
    for (size_t w = 0; w + 4 <= std::min(la, lb); w += 4) {
        const uint32_t x = word(A, pA + w), y = word(B, pB + w);
        if (x == y) continue;
        Change c;
        c.chunk = tail; c.chunkName = cname; c.kind = "word"; c.off = w; c.a = x; c.b = y;
        c.field = blockNote(w);
        std::snprintf(k, sizeof k, "+0x%zx", w);
        c.key = tail + k;
        out.push_back(c);
    }
    if (la != lb) {
        Change c;
        c.chunk = tail; c.chunkName = cname; c.kind = "span"; c.off = std::min(la, lb);
        c.aLen = la - std::min(la, lb); c.bLen = lb - std::min(la, lb);
        c.a = (uint32_t)la; c.b = (uint32_t)lb;
        c.aHex = hexBytes(A.data() + pA + std::min(la, lb), c.aLen);
        c.bHex = hexBytes(B.data() + pB + std::min(la, lb), c.bLen);
        std::snprintf(k, sizeof k, "~size");
        c.key = tail + k;
        out.push_back(c);
    }
    return true;
}

inline void diffLeaf(const std::string& path, const veg::Chunk& ca, const veg::Chunk& cb, std::vector<Change>& out)
{
    const veg::Bytes& A = ca.data;
    const veg::Bytes& B = cb.data;
    const size_t na = A.size(), nb = B.size();
    const std::string cname = nameOf(ca);
    if (cname == "ProjectHeader" && diffHeader(path, ca, cb, out)) return;
    if (na == nb) {
        const size_t n = na;
        for (size_t w = 0; w + 4 <= n; w += 4) {
            const uint32_t x = word(A, w), y = word(B, w);
            if (x == y) continue;
            Change c;
            c.chunk = path; c.chunkName = cname; c.kind = "word"; c.off = w; c.a = x; c.b = y;
            c.field = fieldNote(cname, A, w);
            char k[32]; std::snprintf(k, sizeof k, "+0x%zx", w);
            c.key = path + k;
            out.push_back(c);
        }
        for (size_t i = n & ~(size_t)3; i < n; ++i)
            if (A.u8(i) != B.u8(i)) {
                Change c;
                c.chunk = path; c.chunkName = cname; c.kind = "span"; c.off = i; c.aLen = c.bLen = 1;
                c.aHex = hexBytes(A.data() + i, 1); c.bHex = hexBytes(B.data() + i, 1);
                char k[32]; std::snprintf(k, sizeof k, "~0x%zx", i);
                c.key = path + k;
                out.push_back(c);
            }
        return;
    }
    // different sizes: the middle that differs
    size_t pre = 0;
    const size_t mn = std::min(na, nb);
    while (pre < mn && A.u8(pre) == B.u8(pre)) ++pre;
    size_t suf = 0;
    while (suf < mn - pre && A.u8(na - 1 - suf) == B.u8(nb - 1 - suf)) ++suf;
    Change c;
    c.chunk = path; c.chunkName = cname; c.kind = "span"; c.off = pre;
    c.aLen = na - pre - suf; c.bLen = nb - pre - suf;
    c.aHex = hexBytes(A.data() + pre, c.aLen);
    c.bHex = hexBytes(B.data() + pre, c.bLen);
    c.aText = asText(A.data() + pre, c.aLen);
    c.bText = asText(B.data() + pre, c.bLen);
    c.a = (uint32_t)na; c.b = (uint32_t)nb;     // the sizes
    c.field = fieldNote(cname, A, pre);
    char k[48]; std::snprintf(k, sizeof k, "~0x%zx(size)", pre);
    c.key = path + k;
    out.push_back(c);
}

inline void diffTrees(const veg::Chunk& a, const veg::Chunk& b, const std::string& path, std::vector<Change>& out);

// longest common subsequence of the kids' GUIDs; pairs (index in a, index in b), -1 for none
inline std::vector<std::pair<int, int>> align(const veg::Chunk& a, const veg::Chunk& b)
{
    const size_t n = a.kids.size(), m = b.kids.size();
    std::vector<std::string> ka(n), kb(m);
    for (size_t i = 0; i < n; ++i) ka[i] = (a.kids[i].isList ? "L" : "C") + guidOf(a.kids[i]);
    for (size_t j = 0; j < m; ++j) kb[j] = (b.kids[j].isList ? "L" : "C") + guidOf(b.kids[j]);
    std::vector<std::pair<int, int>> r;
    // skip the common start and end first (nearly all of a list is the same)
    size_t lo = 0;
    while (lo < n && lo < m && ka[lo] == kb[lo]) { r.push_back({(int)lo, (int)lo}); ++lo; }
    size_t ea = n, eb = m;
    std::vector<std::pair<int, int>> tail;
    while (ea > lo && eb > lo && ka[ea - 1] == kb[eb - 1]) { --ea; --eb; tail.push_back({(int)ea, (int)eb}); }
    const size_t ma = ea - lo, mb = eb - lo;
    if (ma && mb) {
        if (ma * mb <= 16u * 1024 * 1024) {
            std::vector<uint32_t> dp((ma + 1) * (mb + 1), 0);
            auto at = [&](size_t i, size_t j) -> uint32_t& { return dp[i * (mb + 1) + j]; };
            for (size_t i = ma; i-- > 0;)
                for (size_t j = mb; j-- > 0;)
                    at(i, j) = ka[lo + i] == kb[lo + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
            size_t i = 0, j = 0;
            while (i < ma && j < mb) {
                if (ka[lo + i] == kb[lo + j]) { r.push_back({(int)(lo + i), (int)(lo + j)}); ++i; ++j; }
                else if (at(i + 1, j) >= at(i, j + 1)) { r.push_back({(int)(lo + i), -1}); ++i; }
                else { r.push_back({-1, (int)(lo + j)}); ++j; }
            }
            for (; i < ma; ++i) r.push_back({(int)(lo + i), -1});
            for (; j < mb; ++j) r.push_back({-1, (int)(lo + j)});
        } else {
            // too many to align: pair by position
            for (size_t i = 0; i < std::max(ma, mb); ++i)
                r.push_back({i < ma ? (int)(lo + i) : -1, i < mb ? (int)(lo + i) : -1});
        }
    } else {
        for (size_t i = 0; i < ma; ++i) r.push_back({(int)(lo + i), -1});
        for (size_t j = 0; j < mb; ++j) r.push_back({-1, (int)(lo + j)});
    }
    for (size_t t = tail.size(); t-- > 0;) r.push_back(tail[t]);
    return r;
}

// "name#k" for same-named siblings
inline std::vector<std::string> siblingNames(const veg::Chunk& c)
{
    std::map<std::string, int> total, seen;
    for (const auto& k : c.kids) total[nameOf(k)]++;
    std::vector<std::string> r;
    for (const auto& k : c.kids) {
        const std::string n = nameOf(k);
        r.push_back(total[n] > 1 ? n + "#" + std::to_string(seen[n]++) : n);
    }
    return r;
}

inline void diffTrees(const veg::Chunk& a, const veg::Chunk& b, const std::string& path, std::vector<Change>& out)
{
    const std::vector<std::string> na = siblingNames(a), nb = siblingNames(b);
    for (const auto& pr : align(a, b)) {
        if (pr.first >= 0 && pr.second >= 0) {
            const veg::Chunk& ka = a.kids[pr.first];
            const veg::Chunk& kb = b.kids[pr.second];
            const std::string p = path + "/" + na[pr.first];
            if (ka.isList) diffTrees(ka, kb, p, out);
            else diffLeaf(p, ka, kb, out);
        } else if (pr.first >= 0) {
            const veg::Chunk& k = a.kids[pr.first];
            Change c;
            c.chunk = path + "/" + na[pr.first]; c.chunkName = nameOf(k); c.kind = "removed";
            c.aLen = k.isList ? k.data.size() : k.data.size();
            c.key = c.chunk + " (removed)";
            out.push_back(c);
        } else {
            const veg::Chunk& k = b.kids[pr.second];
            Change c;
            c.chunk = path + "/" + nb[pr.second]; c.chunkName = nameOf(k); c.kind = "added";
            c.bLen = k.data.size();
            c.key = c.chunk + " (added)";
            // what an added chunk holds, in a few words
            if (!k.isList) c.bHex = hexBytes(k.data.data(), k.data.size(), 32);
            else c.bHex = std::to_string(k.kids.size()) + " chunks inside";
            out.push_back(c);
        }
    }
}

inline std::vector<Change> diffFiles(const Loaded& base, const Loaded& other)
{
    std::vector<Change> out;
    diffTrees(base.tree.root, other.tree.root, "", out);
    return out;
}

// ------------------------------------------------------------------------------------------ output
inline std::string describe(const Change& c)
{
    char buf[256];
    if (c.kind == "word") {
        std::snprintf(buf, sizeof buf, "%s +0x%zx: %s -> %s", c.chunk.c_str(), c.off, hexWord(c.a).c_str(), hexWord(c.b).c_str());
        std::string s = buf;
        if (c.field.size()) s += "   [" + c.field + "]";
        return s;
    }
    if (c.kind == "span") {
        std::string s;
        if (c.aLen == c.bLen && c.a == 0 && c.b == 0) std::snprintf(buf, sizeof buf, "%s ~0x%zx: %zu byte(s): %s -> %s", c.chunk.c_str(), c.off, c.aLen, c.aHex.c_str(), c.bHex.c_str());
        else std::snprintf(buf, sizeof buf, "%s size %u -> %u, differs from 0x%zx: %zu -> %zu byte(s): [%s] -> [%s]", c.chunk.c_str(), c.a, c.b, c.off, c.aLen, c.bLen, c.aHex.c_str(), c.bHex.c_str());
        s = buf;
        if (c.aText.size() || c.bText.size()) s += "   text \"" + c.aText + "\" -> \"" + c.bText + "\"";
        if (c.field.size()) s += "   [" + c.field + "]";
        return s;
    }
    if (c.kind == "text") {
        std::string s = c.chunk + " text #" + std::to_string(c.off) + ": \"" + c.aText + "\" -> \"" + c.bText + "\"";
        if (c.field.size()) s += "   [" + c.field + "]";
        return s;
    }
    if (c.kind == "added") {
        std::snprintf(buf, sizeof buf, "%s ADDED (%zu bytes): %s", c.chunk.c_str(), c.bLen, c.bHex.c_str());
        return buf;
    }
    std::snprintf(buf, sizeof buf, "%s REMOVED (%zu bytes)", c.chunk.c_str(), c.aLen);
    return buf;
}

struct Options {
    bool noise = false, full = false, brief = false, noMap = false, mapOnly = false;
    std::string control, csv, only, manifest;
};

struct ManifestRow { std::string file, base, status, object, change, readback, note; };

inline std::vector<ManifestRow> readManifest(const std::string& path)
{
    std::vector<ManifestRow> rows;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> col;
        size_t s = 0;
        for (;;) {
            const size_t t = line.find('\t', s);
            if (t == std::string::npos) { col.push_back(line.substr(s)); break; }
            col.push_back(line.substr(s, t - s));
            s = t + 1;
        }
        col.resize(7);
        rows.push_back({col[0], col[1], col[2], col[3], col[4], col[5], col[6]});
    }
    return rows;
}

inline std::vector<std::string> listVeg(const std::string& dir)
{
    std::vector<std::string> r;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n.size() > 4 && n.compare(n.size() - 4, 4, ".veg") == 0 && n[0] != '_') r.push_back(n);
        }
        closedir(d);
    }
    std::sort(r.begin(), r.end());
    return r;
}

inline std::string csvQuote(const std::string& s)
{
    std::string r = "\"";
    for (char c : s) { if (c == '"') r += '"'; r += c; }
    return r + "\"";
}

struct Job { std::string file, base, change; };

// returns the number of files compared
inline int run(const std::string& folder, const std::vector<std::string>& loose, const Options& opt, FILE* out)
{
    std::vector<Job> jobs;
    std::string dir = folder;
    std::string controlName = opt.control;
    std::string baseName;
    if (!folder.empty()) {
        std::string mpath = opt.manifest.empty() ? folder + "/manifest.txt" : opt.manifest;
        std::vector<ManifestRow> rows = readManifest(mpath);
        std::set<std::string> have;
        for (const auto& r : rows) {
            if (r.status != "ok" || r.file.empty()) continue;
            if (r.base.empty()) { if (baseName.empty()) baseName = r.file; continue; }     // the base itself
            have.insert(r.file);
            jobs.push_back({r.file, r.base, r.change});
            if (controlName.empty() && r.file.find("control") != std::string::npos) controlName = r.file;
        }
        if (baseName.empty()) baseName = "00_base.veg";
        if (rows.empty()) {
            // no manifest: every .veg against 00_base.veg
            for (const auto& n : listVeg(folder)) {
                if (n == baseName) continue;
                jobs.push_back({n, baseName, ""});
                if (controlName.empty() && n.find("control") != std::string::npos) controlName = n;
            }
        }
    } else if (loose.size() >= 2) {
        // base.veg other.veg ...: paths as given
        baseName = loose[0];
        for (size_t i = 1; i < loose.size(); ++i) jobs.push_back({loose[i], baseName, ""});
        dir = "";
    }
    auto pathOf = [&](const std::string& n) { return dir.empty() ? n : dir + "/" + n; };

    // the files' base trees are loaded once each
    std::map<std::string, std::unique_ptr<Loaded>> bases;
    auto baseTree = [&](const std::string& n) -> Loaded* {
        auto it = bases.find(n);
        if (it == bases.end()) it = bases.emplace(n, loadFile(pathOf(n), n)).first;
        return it->second.get();
    };

    // noise: what differs between the base and the control
    std::set<std::string> noise;
    if (!controlName.empty()) {
        Loaded* b = baseTree(baseName);
        std::unique_ptr<Loaded> c = loadFile(pathOf(controlName), controlName);
        if (b->ok && c->ok) {
            for (const Change& ch : diffFiles(*b, *c)) noise.insert(ch.key);
            if (!opt.mapOnly && !opt.brief) std::fprintf(out, "control %s against %s: %zu place(s) differ with nothing changed (noise)\n\n", controlName.c_str(), baseName.c_str(), noise.size());
        } else {
            std::fprintf(out, "control %s not usable (%s)\n\n", controlName.c_str(), (b->ok ? c->problem : b->problem).c_str());
        }
    }

    FILE* csv = nullptr;
    if (!opt.csv.empty()) {
        csv = std::fopen(opt.csv.c_str(), "w");
        if (csv) std::fprintf(csv, "file,base,change,chunk,kind,offset,old,new,field\n");
    }

    struct Hit { std::string file; Change c; };
    std::map<std::string, std::vector<Hit>> byPlace;
    int compared = 0;
    for (const Job& j : jobs) {
        if (!opt.only.empty() && j.file.find(opt.only) == std::string::npos) continue;
        if (j.file == controlName) continue;
        Loaded* b = baseTree(j.base);
        std::unique_ptr<Loaded> o = loadFile(pathOf(j.file), j.file);
        if (!b->ok || !o->ok) {
            if (!opt.mapOnly) std::fprintf(out, "== %s: skipped (%s)\n\n", j.file.c_str(), (b->ok ? o->problem : "base: " + b->problem).c_str());
            continue;
        }
        ++compared;
        std::vector<Change> changes = diffFiles(*b, *o);
        size_t hidden = 0, bytes = 0;
        std::vector<Change> shown;
        for (Change& c : changes) {
            c.noise = noise.count(c.key) != 0;
            if (c.noise && !opt.noise) { ++hidden; continue; }
            shown.push_back(c);
            bytes += c.kind == "word" ? 4 : std::max(c.aLen, c.bLen);
        }
        if (opt.brief) {
            std::fprintf(out, "%-60s %4zu change(s), %6zu byte(s)%s%s\n", j.file.c_str(), shown.size(), bytes,
                         hidden ? (", " + std::to_string(hidden) + " noise hidden").c_str() : "", "");
        } else if (!opt.mapOnly) {
            std::fprintf(out, "== %s   (base %s)\n", j.file.c_str(), j.base.c_str());
            if (!j.change.empty()) std::fprintf(out, "   change: %s\n", j.change.c_str());
            if (!o->problem.empty()) std::fprintf(out, "   file problem: %s\n", o->problem.c_str());
            std::map<std::string, int> perChunk;
            for (const Change& c : shown) {
                const int n = ++perChunk[c.chunk];
                if (c.kind == "word" && !opt.full && n > 12) { if (n == 13) std::fprintf(out, "   %s: more changed words (use --full)\n", c.chunk.c_str()); continue; }
                std::fprintf(out, "   %s%s\n", c.noise ? "(noise) " : "", describe(c).c_str());
            }
            if (shown.empty()) std::fprintf(out, "   no differences%s\n", hidden ? " except noise" : "");
            if (hidden) std::fprintf(out, "   (%zu noise change(s) hidden)\n", hidden);
            std::fprintf(out, "\n");
        }
        for (const Change& c : shown) {
            if (!c.noise) byPlace[c.key].push_back({j.file, c});
            if (csv) {
                char off[32]; std::snprintf(off, sizeof off, "0x%zx", c.off);
                std::fprintf(csv, "%s,%s,%s,%s,%s,%s,%s,%s,%s\n", csvQuote(j.file).c_str(), csvQuote(j.base).c_str(), csvQuote(j.change).c_str(),
                             csvQuote(c.chunk).c_str(), c.kind.c_str(), off,
                             c.kind == "word" ? hexWord(c.a).c_str() : csvQuote(c.aHex).c_str(),
                             c.kind == "word" ? hexWord(c.b).c_str() : csvQuote(c.bHex).c_str(), csvQuote(c.field).c_str());
            }
        }
    }
    if (csv) std::fclose(csv);

    if (!opt.noMap && !opt.brief) {
        std::fprintf(out, "==== places that change, and the files that change them (%zu place(s); noise left out) ====\n", byPlace.size());
        for (const auto& kv : byPlace) {
            const Change& first = kv.second[0].c;
            std::string head = kv.first;
            if (first.field.size()) head += "   [" + first.field + "]";
            std::fprintf(out, "%s\n", head.c_str());
            // group by (old -> new)
            std::map<std::string, std::vector<std::string>> groups;
            for (const Hit& h : kv.second) {
                std::string v = h.c.kind == "word" ? hexWord(h.c.a) + " -> " + hexWord(h.c.b) : h.c.kind == "span" ? "[" + h.c.aHex + "] -> [" + h.c.bHex + "]" : h.c.kind == "text" ? "\"" + h.c.aText + "\" -> \"" + h.c.bText + "\"" : h.c.kind;
                groups[v].push_back(h.file);
            }
            for (const auto& g : groups) {
                std::string files;
                size_t n = 0;
                for (const auto& f : g.second) { if (n++ < 4) files += (files.empty() ? "" : ", ") + f; }
                if (g.second.size() > 4) files += ", ... (" + std::to_string(g.second.size()) + " files)";
                std::fprintf(out, "    %s    %s\n", g.first.c_str(), files.c_str());
            }
        }
    }
    return compared;
}

}  // namespace vmd

#ifndef VEGMATRIX_DIFF_NO_MAIN
int main(int argc, char** argv)
{
    vmd::Options opt;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& dst) { if (i + 1 < argc) dst = argv[++i]; else { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); } };
        if (a == "--noise") opt.noise = true;
        else if (a == "--full") opt.full = true;
        else if (a == "--brief") opt.brief = true;
        else if (a == "--no-map") opt.noMap = true;
        else if (a == "--map-only") opt.mapOnly = true;
        else if (a == "--control") next(opt.control);
        else if (a == "--csv") next(opt.csv);
        else if (a == "--only") next(opt.only);
        else if (a == "--manifest") next(opt.manifest);
        else if (a.size() > 1 && a[0] == '-' && a != "-") { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
        else args.push_back(a);
    }
    if (args.empty()) {
        std::fprintf(stderr, "usage: vegmatrix_diff [--noise] [--full] [--brief] [--no-map|--map-only] [--control F] [--csv F] [--only TEXT] <folder | base.veg other.veg ...>\n");
        return 2;
    }
    std::string folder;
    std::vector<std::string> loose;
    DIR* d = opendir(args[0].c_str());
    if (d && args.size() == 1) { closedir(d); folder = args[0]; }
    else { if (d) closedir(d); loose = args; if (loose.size() < 2) { std::fprintf(stderr, "give a folder, or a base file and the files to compare with it\n"); return 2; } }
    const int n = vmd::run(folder, loose, opt, stdout);
    if (n == 0) std::fprintf(stderr, "no files compared\n");
    return 0;
}
#endif
