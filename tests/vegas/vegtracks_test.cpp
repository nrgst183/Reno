// vegtracks_test: the per-track settings of the sample VEGAS Pro 18 projects, and Event Pan/Crop of events whose media
// is another VEGAS project (nested projects).
//
//   vegtracks_test [folder with .veg files] [--scratch folder] [--table]
//                  (default $RENO_SAMPLES, searched recursively; it is only read)
//
//  1. Every project is read and each track's settings are printed with --table (name, colour, height, minimized, mute,
//     solo, level, pan and pan type, invert phase, output bus, compositing mode and Parent Composite Mode, fade colours,
//     effects, envelopes, track motion) and checked against the file's own bytes, read independently of the reader,
//     and against what is known of the samples (track names, the muted reference track, the inverted track...).
//  2. Round trip: every project is written (writeVegasData) and read back; every track setting must survive. Then
//     every setting of every track is changed, written, read back and compared.
//  3. Pan/Crop: flipped boxes (negative width or height: Flip Horizontal / Vertical) are read, and a nested project's
//     events are rendered with the samples' own keys over a nested project made here (a test pattern), and the pixels
//     compared with where VEGAS's numbers put the nested picture.
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "../../src/engine/engine.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include "../testdata.h"

using namespace reno;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
static bool check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " - ", detail.c_str()); std::fflush(stdout); }
    return ok;
}

static std::string readFile(const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return std::string();
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    std::fclose(f);
    return s;
}

static double dB(double g) { return g > 0 ? 20.0 * std::log10(g) : -1e9; }

// ------------------------------------------------------------------------------------------ the file's own bytes
// What a track's records hold, read straight off the chunks (not through the reader).
struct RawTrack {
    uint32_t flags = 0, height = 0, kind = 0, colour = 0;
    std::string name;
    bool haveAudio = false, haveVideo = false;
    float vol = 1, pan = 0, vlevel = 1;
    uint32_t panType = 0, invert = 0, bus = 0;
    int32_t mode = 4, parentMode = 4;
};
static std::vector<RawTrack> rawTracks(const std::string& bytes)
{
    using namespace veg;
    std::vector<RawTrack> out;
    ChunkTree tree;
    readChunkTree(Bytes((const uint8_t*)bytes.data(), bytes.size()), tree);
    std::function<void(const Chunk&)> walk = [&](const Chunk& c) {
        for (const auto& k : c.kids) {
            if (k.isList && k.type == G::Track) {
                RawTrack r;
                for (const auto& x : k.kids) {
                    if (!x.isList && x.id == G::TrackHeader) {
                        r.flags = x.data.u32(4);
                        r.height = x.data.u32(8);
                        r.kind = x.data.u32(0x0c);
                        r.colour = x.data.u32(0x14);
                        r.name = x.data.text(x.data.u32(0), x.data.u32(0x10));
                    }
                    if (x.isList && x.type == G::AudioTrackList)
                        for (const auto& d : x.kids)
                            if (!d.isList && d.id == G::AudioTrackData) {
                                r.haveAudio = true;
                                r.bus = d.data.u32(8);
                                r.vol = d.data.f32(0x0c);
                                r.pan = d.data.f32(0x10);
                                r.panType = d.data.u32(0x1f8);
                                r.invert = d.data.u32(0x1fc);
                            }
                    if (x.isList && x.type == G::VideoTrackList)
                        for (const auto& d : x.kids)
                            if (!d.isList && d.id == G::VideoTrackData) {
                                r.haveVideo = true;
                                r.mode = d.data.i32(0x64);
                                r.vlevel = d.data.f32(0x68);
                                r.parentMode = d.data.i32(0x80);
                            }
                }
                out.push_back(r);
            } else if (k.isList) {
                walk(k);
            }
        }
    };
    walk(tree.root);
    return out;
}

// ------------------------------------------------------------------------------------------ track settings as text
static std::string envText(const Envelopes& envs)
{
    std::string s;
    for (const auto& e : envs) {
        s += (s.empty() ? "" : ",") + e.kind + strfmt("(%zu", e.points.size());
        if (!e.points.empty()) s += strfmt(" %.3g..%.3g", e.points.front().value, e.points.back().value);
        s += ")";
    }
    return s.empty() ? "-" : s;
}
static std::string fxText(const FxChain& fx)
{
    std::string s;
    for (const auto& f : fx) {
        std::string what;
        if (f.kind == "ofx") what = strfmt("%s[%zu params]", f.pluginId.c_str(), f.state.get("params").members().size());
        else what = strfmt("%s[%s %zu chars]", f.name.c_str(), f.audio.format.c_str(), f.state.asString().size());
        s += (s.empty() ? "" : "; ") + what + (f.bypass ? " OFF" : "");
    }
    return s.empty() ? "-" : s;
}
// Every setting of a track that the .veg keeps, as one line (for comparing a round trip).
static std::string settingsText(const Project& p, const Track& t)
{
    std::string bus = "master";
    for (size_t i = 0; i < p.buses.size(); ++i)
        if (p.buses[i].id == t.output) bus = p.buses[i].name.empty() ? strfmt("bus %zu", i + 1) : p.buses[i].name;
    std::string s = strfmt("%s '%s' colour %06x height %d%s%s%s%s level %.6g pan %.6g", t.type == TrackType::Video ? "video" : "audio",
                           t.name.c_str(), t.color & 0xFFFFFF, t.height, t.minimized ? " minimized" : "", t.mute ? " mute" : "",
                           t.solo ? " solo" : "", t.invertPhase ? " invert" : "", t.level, t.pan);
    if (t.type == TrackType::Audio) s += " panType " + t.panType + " out " + bus;
    else s += strfmt(" composite %s parent %s fade %08x/%08x motion %zu", t.composite.c_str(), t.parentComposite.c_str(), t.fadeTop, t.fadeBottom, t.motion.size());
    s += " fx " + fxText(t.fx) + " env " + envText(t.envelopes);
    for (const auto& e : t.envelopes)
        for (const auto& pt : e.points) s += strfmt(" %.6g:%.6g:%d", pt.time, pt.value, pt.curve);
    return s;
}

// ------------------------------------------------------------------------------------------ 1. the samples' tracks
static void checkSample(const std::string& path, bool table, std::map<std::string, Project>& loaded)
{
    const std::string name = fs::path(path).filename().string();
    Project p;
    VegasImportResult r = importVegasProject(path, p);
    if (!check(r.ok, name + ": read", r.error)) return;
    loaded[path] = p;
    const std::string bytes = readFile(path);
    const std::vector<RawTrack> raw = rawTracks(bytes);
    check(raw.size() == p.tracks.size(), name + ": every track read", strfmt("%zu in the file, %zu read", raw.size(), p.tracks.size()));
    if (table) {
        std::printf("== %s  (%dx%d)\n", name.c_str(), p.settings.width, p.settings.height);
        std::printf("  %-2s %-5s %-24s %-3s %-6s %-4s %-3s %-4s %-4s %-9s %-7s %-8s %-4s %-6s %-11s %s\n", "#", "kind", "name", "col", "height", "min",
                    "mut", "solo", "inv", "level", "pan", "pantype", "out", "comp", "parentcomp", "fx / envelopes");
    }
    for (size_t i = 0; i < p.tracks.size() && i < raw.size(); ++i) {
        const Track& t = p.tracks[i];
        const RawTrack& w = raw[i];
        const std::string where = strfmt("%s track %zu", name.c_str(), i + 1);
        const bool video = t.type == TrackType::Video;
        if (table) {
            std::string out = "-";
            if (!video) {
                out = "M";
                for (size_t b = 0; b < p.buses.size(); ++b)
                    if (p.buses[b].id == t.output) out = std::string(1, (char)('A' + b));
            }
            std::printf("  %-2zu %-5s %-24s %-3u %-6d %-4s %-3s %-4s %-4s %-9s %-7s %-8s %-4s %-6s %-11s %s | %s\n", i + 1, video ? "video" : "audio",
                        ("'" + t.name.substr(0, 22) + "'").c_str(), veg::trackColorIndex(t.color), t.height, t.minimized ? "yes" : "", t.mute ? "M" : "",
                        t.solo ? "S" : "", t.invertPhase ? "inv" : "",
                        video ? strfmt("%.1f%%", t.level * 100).c_str() : strfmt("%.2fdB", dB(t.level)).c_str(),
                        video ? "" : (std::fabs(t.pan) < 1e-9 ? "Center" : strfmt("%.0f%%%s", std::fabs(t.pan) * 100, t.pan < 0 ? "L" : "R")).c_str(),
                        video ? "" : t.panType.c_str(), out.c_str(), video ? t.composite.c_str() : "", video ? t.parentComposite.c_str() : "",
                        fxText(t.fx).c_str(), envText(t.envelopes).c_str());
        }
        // against the bytes
        check(t.name == w.name, where + ": name");
        check(video == (w.kind == 3), where + ": kind");
        check(t.mute == ((w.flags & 2) != 0) && t.solo == ((w.flags & 1) != 0), where + ": mute and solo");
        check(t.minimized == ((w.flags & 0x10000) != 0), where + ": minimized");
        check(t.color == veg::trackColorOfIndex(w.colour) && t.color != 0, where + ": colour", strfmt("index %u -> %08x", w.colour, t.color));
        const int defH = video ? 64 : 68;
        check(t.height == ((int)w.height == defH ? 0 : (int)w.height), where + ": height", strfmt("file %u, read %d", w.height, t.height));
        if (!video && w.haveAudio) {
            check(std::fabs(t.level - w.vol) < 1e-6, where + ": volume");
            check(std::fabs(t.pan - w.pan) < 1e-6, where + ": pan");
            check(t.panType == veg::panTypeName(w.panType), where + ": pan type", t.panType);
            check(t.invertPhase == ((w.invert & 1) != 0), where + ": invert phase");
        }
        if (video && w.haveVideo) {
            check(std::fabs(t.level - w.vlevel) < 1e-6, where + ": level");
            int32_t m = 0;
            std::string g;
            check(veg::compositeModeOf(t.composite, m, g) && (m == w.mode || w.mode == -1 || t.composite == "srcalpha"), where + ": compositing mode");
        }
        // the effects' settings loaded
        for (const auto& f : t.fx) {
            if (f.kind == "ofx") check(!f.state.get("params").members().empty(), where + ": effect " + f.pluginId + " has its settings");
            else check(!f.state.asString().empty() || f.audio.format.empty(), where + ": audio effect " + f.name + " has its settings");
        }
        for (const auto& e : t.envelopes) check(!e.points.empty() || e.kind.empty(), where + ": envelope " + e.kind + " has points");
    }
}

// Facts that hold for every sample: track levels and pans are usable numbers, a track's colour is one of VEGAS's, a track
// of audio has a pan type, and the effects of a track are named.
static void checkKnown(std::map<std::string, Project>& loaded, const std::string& /*folder*/)
{
    int bad = 0, tracks = 0;
    for (auto& kv : loaded)
        for (const Track& t : kv.second.tracks) {
            ++tracks;
            if (!(std::isfinite(t.level) && t.level >= 0 && std::isfinite(t.pan) && t.pan >= -1.0 && t.pan <= 1.0)) ++bad;
            if (t.type == TrackType::Audio && t.panType.empty()) ++bad;
        }
    check(bad == 0, strfmt("samples: every track has a usable level, pan and pan type (%d tracks)", tracks));
}

// ------------------------------------------------------------------------------------------ 2. round trips
static void roundTrip(const std::string& path, const Project& p0)
{
    const std::string name = fs::path(path).filename().string();
    std::vector<std::string> warnings;
    const std::string bytes = writeVegasData(p0, path, &warnings);
    Project p1;
    VegasImportResult r = importVegasData(bytes, path, p1);
    if (!check(r.ok && p1.tracks.size() == p0.tracks.size(), name + ": written and read back")) return;
    for (size_t i = 0; i < p0.tracks.size(); ++i) {
        const std::string a = settingsText(p0, p0.tracks[i]), b = settingsText(p1, p1.tracks[i]);
        check(a == b, strfmt("%s track %zu: settings kept", name.c_str(), i + 1), a + "\n   became " + b);
        // and its events' pan/crop (flipped boxes among them)
        const Track& t0 = p0.tracks[i];
        const Track& t1 = p1.tracks[i];
        if (t0.events.size() != t1.events.size()) continue;
        int bad = 0;
        std::string first;
        for (size_t k = 0; k < t0.events.size(); ++k) {
            const auto& x = t0.events[k].panCrop;
            const auto& y = t1.events[k].panCrop;
            bool same = x.size() == y.size() && t0.events[k].resample == t1.events[k].resample;
            for (size_t j = 0; same && j < x.size(); ++j)
                same = std::fabs(x[j].time - y[j].time) < 1e-6 && std::fabs(x[j].cx - y[j].cx) < 1e-6 && std::fabs(x[j].cy - y[j].cy) < 1e-6 &&
                       std::fabs(x[j].w - y[j].w) < 1e-6 && std::fabs(x[j].h - y[j].h) < 1e-6 && std::fabs(x[j].rotation - y[j].rotation) < 1e-6 &&
                       x[j].curve == y[j].curve;
            if (!same && !bad++) first = strfmt("event %zu: %zu keys, became %zu", k + 1, x.size(), y.size());
        }
        check(bad == 0, strfmt("%s track %zu: events' pan/crop kept", name.c_str(), i + 1), strfmt("%d events differ; ", bad) + first);
    }
    // every setting of every track changed
    Project q = p0;
    const char* panTypes[] = {"add", "power", "balance0", "balance3", "balance6", "film"};
    for (size_t i = 0; i < q.tracks.size(); ++i) {
        Track& t = q.tracks[i];
        t.name = strfmt("Renamed %zu", i);
        t.color = veg::trackColorOfIndex((uint32_t)(i * 3 + 5));
        t.height = 40 + (int)i;
        t.minimized = !t.minimized;
        t.mute = !t.mute;
        t.solo = !t.solo;
        if (t.type == TrackType::Audio) {
            t.level = 0.5 + 0.01 * (double)i;
            t.pan = -0.25;
            t.panType = panTypes[i % 6];
            t.invertPhase = !t.invertPhase;
        } else {
            t.level = 0.75;
            t.composite = i % 2 ? "multiply" : "screen";
            t.parentComposite = "add";
            t.fadeTop = 0xFF336699;
            t.fadeBottom = 0xFF102030;
        }
    }
    const std::string b2 = writeVegasData(q, path, &warnings);
    Project q1;
    r = importVegasData(b2, path, q1);
    if (!check(r.ok && q1.tracks.size() == q.tracks.size(), name + ": changed, written and read back")) return;
    for (size_t i = 0; i < q.tracks.size(); ++i) {
        std::string a = settingsText(q, q.tracks[i]), b = settingsText(q1, q1.tracks[i]);
        check(a == b, strfmt("%s track %zu: changed settings kept", name.c_str(), i + 1), a + "\n   became " + b);
    }
}

// ------------------------------------------------------------------------------------------ 3. pan/crop
struct Px { int b, g, r, a; };
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static bool near(const Px& p, int r, int g, int b, int tol = 12) { return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol; }
static std::string pxs(const Px& p) { return strfmt("r=%d g=%d b=%d", p.r, p.g, p.b); }

// The test pattern: quadrants red (top left), green (top right), blue (bottom left), white (bottom right).
static const int kQ[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
static bool writePattern(const std::string& path, int w, int h)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = w;
    es.height = h;
    std::unique_ptr<MediaWriter> mw = createMediaWriter(es, nullptr);
    if (!mw) return false;
    ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int q = (y < h / 2 ? 0 : 2) + (x < w / 2 ? 0 : 1);
            uint8_t* d = img->row(h - 1 - y) + x * 4;
            d[0] = (uint8_t)kQ[q][2]; d[1] = (uint8_t)kQ[q][1]; d[2] = (uint8_t)kQ[q][0]; d[3] = 255;
        }
    return mw->writeVideo(*img) && mw->finish(nullptr);
}

// A project of w x h with the picture on one video track, whole (or with this event pan/crop), 10 s long.
static Project patternProject(const std::string& picture, int w, int h, const std::vector<PanCropKey>& keys = {})
{
    Project p;
    p.settings.width = w;
    p.settings.height = h;
    MediaItem m;
    m.id = p.newId();
    m.path = picture;
    m.name = "pattern.png";
    m.info.path = picture;
    m.info.hasVideo = true;
    m.info.isStill = true;
    m.info.width = w;
    m.info.height = h;
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 10;
    e.panCrop = keys;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

// Where a media point (mx, my) in pixels lands in the output frame, for a pan/crop box (VEGAS's numbers: box size
// bw x bh and centre (cx, cy) in the media's pixels, not turned) filling a W x H frame whose shape it has.
static void toOutput(double mx, double my, double bw, double bh, double cx, double cy, int W, int H, double& ox, double& oy)
{
    ox = (mx - (cx - bw / 2)) * W / bw;
    oy = (my - (cy - bh / 2)) * H / bh;
}

static void panCropChecks(std::map<std::string, Project>& loaded, const std::string& folder, const std::string& scratch)
{
    // flipped boxes in the samples (a negative width or height mirrors the picture): every key is a usable box
    int flippedEvents = 0, flippedNested = 0, badKeys = 0;
    for (auto& kv : loaded) {
        const Project& p = kv.second;
        for (const Track& t : p.tracks)
            for (const Event& e : t.events) {
                bool flip = false;
                for (const auto& k : e.panCrop) {
                    flip = flip || k.w < 0 || k.h < 0;
                    if (!(std::isfinite(k.w) && std::isfinite(k.h) && std::isfinite(k.cx) && std::isfinite(k.cy) && k.w != 0 && k.h != 0)) ++badKeys;
                }
                if (!flip) continue;
                ++flippedEvents;
                const MediaItem* m = p.findMedia(e.mediaId);
                if (m && m->path.size() > 4 && m->path.compare(m->path.size() - 4, 4, ".veg") == 0) ++flippedNested;
            }
    }
    std::printf("pan/crop: %d events with a flipped box, %d of them of nested projects\n", flippedEvents, flippedNested);
    check(badKeys == 0, "pan/crop: the samples' boxes are all usable (no zero or non-finite size)", strfmt("%d bad", badKeys));

    // the samples' own keys, over a nested project made here
    const std::string pattern = scratch + "/pattern.png";
    const std::string nested = scratch + "/nested.veg";
    if (!check(writePattern(pattern, 1920, 1080), "pan/crop: test pattern written")) return;
    {
        Project np = patternProject(pattern, 1920, 1080);
        VegasSaveResult sr = saveVegasProject(np, nested);
        if (!check(sr.ok, "pan/crop: nested project written", sr.error)) return;
    }
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });

    // a project at the samples' size whose one event is the nested project with the given keys
    auto renderWith = [&](const std::vector<PanCropKey>& keys, bool keepAspect, double at) -> ImagePtr {
        Project outer = patternProject(nested, 1920, 1080, keys);
        MediaItem& m = outer.media[0];
        m.name = "nested.veg";
        m.info.isStill = false;
        if (!vegasProjectMediaInfo(nested, m.info, nullptr)) return nullptr;
        outer.tracks[0].events[0].keepAspect = keepAspect;
        MediaPool pool;
        FxRuntime fx;
        VideoEngine video(pool, fx);
        FrameOptions fo;
        fo.interactive = false;
        return video.render(outer, at, fo);
    };
    auto quadrants = [&](const ImagePtr& img, const int order[4], const std::string& what) {
        const int xs[2] = {480, 1440}, ys[2] = {270, 810};
        for (int q = 0; q < 4; ++q) {
            const Px px = pixel(img, xs[q % 2], ys[q / 2]);
            const int* c = kQ[order[q]];
            check(near(px, c[0], c[1], c[2]), what + strfmt(": quadrant %d", q), pxs(px));
        }
    };
    {
        const int plain[4] = {0, 1, 2, 3};
        quadrants(renderWith({}, true, 1.0), plain, "nested, no pan/crop: the nested project's frame fills the frame");
    }
    // a nested project's event flipped horizontally and vertically (the keys as the reader makes them: a negative size)
    {
        Event hflipEv, vflipEv;
        PanCropKey hk, vk;
        hk.w = -1;
        vk.h = -1;
        hflipEv.panCrop = {hk};
        vflipEv.panCrop = {vk};
        const Event* hflip = &hflipEv;
        const Event* vflip = &vflipEv;
        const PanCropKey& k = hflip->panCrop[0];
        check(std::fabs(k.w + 1) < 1e-9 && std::fabs(k.h - 1) < 1e-9 && std::fabs(k.cx - 0.5) < 1e-9 && std::fabs(k.cy - 0.5) < 1e-9,
              "pan/crop: a horizontal flip is the whole frame mirrored", strfmt("w %g h %g c %g,%g", k.w, k.h, k.cx, k.cy));
        const int h[4] = {1, 0, 3, 2}, v[4] = {2, 3, 0, 1};
        quadrants(renderWith(hflip->panCrop, hflip->keepAspect, 1.0), h, "nested, Flip Horizontal");
        quadrants(renderWith(vflip->panCrop, vflip->keepAspect, 1.0), v, "nested, Flip Vertical");
    }
    // the nested frame zoomed out (a box of 8805.9 x 4953.3 of a 1920 x 1080 project) and moving: two keys with VEGAS's numbers
    // (box 8805.9 x 4953.3; centre (2303.6, 636.8) at 0 s, (2485.0, 1322.8) at 0.307 s - the box is in the nested project's
    // pixels, 1920 x 1080)
    {
        Event evObj;
        PanCropKey k0, k1;
        k0.time = 0;
        k0.w = 8805.9 / 1920; k0.h = 4953.3 / 1080; k0.cx = 2303.6 / 1920; k0.cy = 636.8 / 1080;
        k1 = k0;
        k1.time = 0.307;
        k1.cx = 2485.0 / 1920; k1.cy = 1322.8 / 1080;
        evObj.panCrop = {k0, k1};
        const Event* ev = &evObj;
        {
            struct { double t, cx, cy; } want[] = {{0.0, 2303.6, 636.8}, {0.307, 2485.0, 1322.8}};
            for (const auto& w : want) {
                // the key at that time, as Reno has it (and as the engine interpolates: exactly on a key)
                const PanCropKey* k = nullptr;
                for (const auto& x : ev->panCrop) if (std::fabs(x.time - w.t) < 0.001) k = &x;
                if (!check(k != nullptr, strfmt("pan/crop: a key at %.3f s", w.t))) continue;
                const double bw = k->w * 1920, bh = k->h * 1080, cx = k->cx * 1920, cy = k->cy * 1080;
                check(std::fabs(bw - 8805.9) < 0.1 && std::fabs(bh - 4953.3) < 0.1 && std::fabs(cx - w.cx) < 0.1 && std::fabs(cy - w.cy) < 0.1,
                      strfmt("pan/crop: the key at %.3f s is VEGAS's box", w.t), strfmt("%.1f x %.1f at (%.1f, %.1f)", bw, bh, cx, cy));
                // where the nested frame's corners and quadrant centres land
                double x0, y0, x1, y1;
                toOutput(0, 0, bw, bh, cx, cy, 1920, 1080, x0, y0);
                toOutput(1920, 1080, bw, bh, cx, cy, 1920, 1080, x1, y1);
                std::printf("pan/crop: at %.3f s VEGAS's numbers put the nested frame at (%.1f, %.1f)-(%.1f, %.1f)\n", w.t, x0, y0, x1, y1);
                std::vector<PanCropKey> one = {*k};
                one[0].time = 0;
                ImagePtr img = renderWith(one, ev->keepAspect, 1.0);
                for (int q = 0; q < 4; ++q) {
                    double ox, oy;
                    toOutput(q % 2 ? 1440 : 480, q / 2 ? 810 : 270, bw, bh, cx, cy, 1920, 1080, ox, oy);
                    const Px px = pixel(img, (int)std::lround(ox), (int)std::lround(oy));
                    check(near(px, kQ[q][0], kQ[q][1], kQ[q][2]), strfmt("pan/crop zoomed out at %.3f s: quadrant %d at (%.0f, %.0f)", w.t, q, ox, oy), pxs(px));
                }
                // just outside the nested frame: nothing (black)
                const int out[4][2] = {{(int)x0 - 4, (int)((y0 + y1) / 2)}, {(int)x1 + 4, (int)((y0 + y1) / 2)},
                                       {(int)((x0 + x1) / 2), (int)y0 - 4}, {(int)((x0 + x1) / 2), (int)y1 + 4}};
                for (const auto& o : out) {
                    const Px px = pixel(img, o[0], o[1]);
                    check(px.r < 0 || near(px, 0, 0, 0, 6), strfmt("pan/crop zoomed out at %.3f s: black outside at (%d, %d)", w.t, o[0], o[1]), pxs(px));
                }
                // just inside its corners: the corner quadrants
                const Px tl = pixel(img, (int)std::ceil(x0) + 3, (int)std::ceil(y0) + 3), br = pixel(img, (int)x1 - 3, (int)y1 - 3);
                check(near(tl, 255, 0, 0, 40) && near(br, 255, 255, 255, 40), strfmt("pan/crop zoomed out at %.3f s: the nested frame's corners", w.t),
                      pxs(tl) + " / " + pxs(br));
            }
            // between keys the box moves in a line (linear keys): half way between the first two keys
            const PanCropKey a = ev->panCrop[0], b = ev->panCrop[1];
            check(a.curve == 0, "pan/crop: the keys are linear");
            const double tm = (a.time + b.time) / 2;
            const double cx = (a.cx + b.cx) / 2 * 1920, cy = (a.cy + b.cy) / 2 * 1080, bw = a.w * 1920, bh = a.h * 1080;
            ImagePtr img = renderWith(ev->panCrop, ev->keepAspect, tm);
            double ox, oy;
            toOutput(480, 270, bw, bh, cx, cy, 1920, 1080, ox, oy);
            check(near(pixel(img, (int)std::lround(ox), (int)std::lround(oy)), 255, 0, 0), "pan/crop: half way between keys, the box half way",
                  pxs(pixel(img, (int)std::lround(ox), (int)std::lround(oy))));
        }
    }
    // Pan/Crop inside the nested project: its own event flipped; the outer event plain
    {
        const std::string inner = scratch + "/nested_flipped.veg";
        PanCropKey k;
        k.w = -1;
        Project np = patternProject(pattern, 1920, 1080, {k});
        VegasSaveResult sr = saveVegasProject(np, inner);
        if (check(sr.ok, "pan/crop: nested project with a flipped event written", sr.error)) {
            Project back;
            std::string err;
            check(loadProjectFile(inner, back, &err) && back.tracks.size() == 1 && back.tracks[0].events.size() == 1 &&
                      back.tracks[0].events[0].panCrop.size() == 1 && back.tracks[0].events[0].panCrop[0].w < 0,
                  "pan/crop: the nested project's flipped event reads back flipped");
            Project outer = patternProject(inner, 1920, 1080);
            outer.media[0].info.isStill = false;
            vegasProjectMediaInfo(inner, outer.media[0].info, nullptr);
            MediaPool pool;
            FxRuntime fx;
            VideoEngine video(pool, fx);
            FrameOptions fo;
            fo.interactive = false;
            const int h[4] = {1, 0, 3, 2};
            quadrants(video.render(outer, 1.0, fo), h, "the nested project's own Flip Horizontal");
        }
    }
    // A nested project of another shape (4:3, 1440 x 1080) in a 16:9 project: no keys - fitted, pillarboxed; a box
    // of its own frame (VEGAS's untouched key for it: its picture widened to the outer frame's shape) the same
    {
        const std::string pat43 = scratch + "/pattern43.png";
        const std::string n43 = scratch + "/nested43.veg";
        if (check(writePattern(pat43, 1440, 1080), "pan/crop: 4:3 pattern written")) {
            Project np = patternProject(pat43, 1440, 1080);
            VegasSaveResult sr = saveVegasProject(np, n43);
            if (check(sr.ok, "pan/crop: 4:3 nested project written", sr.error)) {
                Project outer = patternProject(n43, 1920, 1080);
                outer.media[0].info.isStill = false;
                vegasProjectMediaInfo(n43, outer.media[0].info, nullptr);
                check(outer.media[0].info.width == 1440 && outer.media[0].info.height == 1080, "pan/crop: the 4:3 nested project's own size is its media size");
                MediaPool pool;
                FxRuntime fx;
                VideoEngine video(pool, fx);
                FrameOptions fo;
                fo.interactive = false;
                ImagePtr img = video.render(outer, 1.0, fo);
                check(near(pixel(img, 100, 540), 0, 0, 0, 6) && near(pixel(img, 1820, 540), 0, 0, 0, 6), "pan/crop: 4:3 nested, no keys: pillarboxed",
                      pxs(pixel(img, 100, 540)) + " / " + pxs(pixel(img, 1820, 540)));
                check(near(pixel(img, 240 + 360, 270), 255, 0, 0) && near(pixel(img, 240 + 1080, 810), 255, 255, 255), "pan/crop: 4:3 nested, no keys: its picture in the middle");
                // the right half of the nested frame only (a box 720 x 1080 at (1080, 540)): stretched to fill? no - it keeps
                // its shape (maintain aspect): 720 x 1080 shown 720 x 1080 in the middle of the frame
                PanCropKey k;
                k.cx = 0.75; k.w = 0.5;
                outer.tracks[0].events[0].panCrop = {k};
                img = video.render(outer, 1.0, fo);
                check(near(pixel(img, 960 - 200, 270), 0, 255, 0) && near(pixel(img, 960 + 200, 810), 255, 255, 255) && near(pixel(img, 500, 540), 0, 0, 0, 6),
                      "pan/crop: 4:3 nested, its right half: green over white, kept in shape", pxs(pixel(img, 760, 270)) + " / " + pxs(pixel(img, 500, 540)));
                outer.tracks[0].events[0].keepAspect = false;
                img = video.render(outer, 1.0, fo);
                check(near(pixel(img, 100, 270), 0, 255, 0) && near(pixel(img, 1820, 810), 255, 255, 255), "pan/crop: 4:3 nested, its right half stretched to fill",
                      pxs(pixel(img, 100, 270)));
            }
        }
    }
    setProjectFileLoader(ProjectFileLoader());
}

// ------------------------------------------------------------------------------------------ 4. the engine
// Invert Phase of a track: the same sound, upside down (and two copies, one inverted, cancel out).
namespace {
class Tone : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const float v = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * (double)(start + k) / sampleRate);
            left[k] = v;
            right[k] = 0.5f * v;
        }
        return true;
    }
};
}  // namespace
static void invertPhaseChecks()
{
    MediaPool pool;
    pool.setOpener([](const std::string& path, std::string*) -> std::unique_ptr<MediaReader> {
        std::unique_ptr<Tone> r(new Tone());
        r->mi.path = path;
        r->mi.hasAudio = true;
        r->mi.duration = 10;
        r->mi.sampleRate = 48000;
        r->mi.channels = 2;
        return std::unique_ptr<MediaReader>(r.release());
    });
    FxRuntime fx;
    AudioEngine audio(pool, fx);
    Project p;
    MediaItem m;
    m.id = p.newId();
    m.path = m.name = "tone:";
    m.info.path = m.path;
    m.info.hasAudio = true;
    m.info.duration = 10;
    m.info.sampleRate = 48000;
    m.info.channels = 2;
    p.media.push_back(m);
    auto addTrack = [&](bool invert) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.panType = "add";
        t.invertPhase = invert;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 5;
        t.events.push_back(e);
        p.tracks.push_back(t);
    };
    const int n = 4800;
    std::vector<float> l0(n), r0(n), l1(n), r1(n);
    addTrack(false);
    audio.render(p, 48000, n, l0.data(), r0.data(), false);
    p.tracks[0].invertPhase = true;
    audio.render(p, 48000, n, l1.data(), r1.data(), false);
    double peak = 0, err = 0;
    for (int k = 0; k < n; ++k) {
        peak = std::max(peak, (double)std::fabs(l0[(size_t)k]));
        err = std::max(err, (double)std::fabs(l0[(size_t)k] + l1[(size_t)k]) + std::fabs(r0[(size_t)k] + r1[(size_t)k]));
    }
    check(peak > 0.3 && err < 1e-5, "engine: an inverted track plays its sound upside down", strfmt("peak %.3f, difference %.2g", peak, err));
    addTrack(false);        // a second, plain copy: with the first inverted they cancel
    audio.render(p, 48000, n, l1.data(), r1.data(), false);
    double rest = 0;
    for (int k = 0; k < n; ++k) rest = std::max(rest, (double)std::fabs(l1[(size_t)k]) + std::fabs(r1[(size_t)k]));
    check(rest < 1e-5, "engine: a track and its inverted copy cancel out", strfmt("%.2g left", rest));
}

int main(int argc, char** argv)
{
    std::string folder = testDataDir("RENO_SAMPLES"), scratch = (fs::temp_directory_path() / "vegtracks_test").string();
    bool table = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scratch" && i + 1 < argc) scratch = argv[++i];
        else if (a == "--table") table = true;
        else folder = a;
    }
    if (testDataMissing(folder)) return 0;
    fs::create_directories(scratch);
    std::vector<std::string> files;
    for (const auto& de : fs::recursive_directory_iterator(folder))
        if (de.is_regular_file() && de.path().extension() == ".veg") files.push_back(de.path().string());
    std::sort(files.begin(), files.end());
    std::map<std::string, Project> loaded;
    for (const auto& f : files) checkSample(f, table, loaded);
    checkKnown(loaded, folder);
    for (const auto& kv : loaded) roundTrip(kv.first, kv.second);
    panCropChecks(loaded, folder, scratch);
    invertPhaseChecks();
    std::printf("vegtracks_test: %d passed, %d failed (%zu projects)\n", g_pass, g_fail, files.size());
    return g_fail ? 1 : 0;
}
