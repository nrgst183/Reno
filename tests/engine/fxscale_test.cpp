// Tests of the built-in effects at a fraction of the project size (preview Half / Quarter size, Draft quality):
// given a picture reduced to 1/2 or 1/4 and the render scale (Ctx::scale), each effect must give nearly what the
// full-size render reduced by the same amount gives. Reports the mean and largest difference in 8-bit levels per
// effect. Also checks that OpenFX effects supporting multi-resolution get the render scale and scaled pictures.
//
//   fxscale_test [--ignore-scale] [--hash] [--size W H] [--only <text>] [--csv file] [--engine [<testfx.ofx>]] [--stretch] [--plugins <file.ofx>]
//   --ignore-scale: never tell the effects the scale
//   --engine: test through the video engine at the project size, 1/2 and 1/4 (FrameOptions::scale); the test effect
//     file, built with TESTFX_MULTIRES set or not, decides whether it supports multi-resolution
//   --stretch: built-in effects say they do not support multi-resolution (pictures are stretched), to compare times
//   --hash: only print a hash of every effect's output at full size, 1/2 and 1/4
//   --threads N: at most N processors for the effects' own threads
//   --host-time id[,id...] [--host-scale S]: cost of each effect as the engine calls it (1920 x 1080 project, still +
//   effect, at 1/4, 1/2 and full size): CPU ms of the frame with the effect minus without
//   --time: only time the render functions (best of several runs, CPU ms of all threads)
#include "../../src/engine/engine.h"
#include "../../src/engine/fxruntime.h"
#include "../../src/media/media.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/builtins_impl.h"
#include "../../src/ofx/ofxhost.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// ------------------------------------------------------------------ pictures
// A picture with smooth colour, hard edges, a bright disc, fine stripes and a soft-alpha corner, drawn from fractions
// of the frame so the same picture exists at every size.
static Buf picture(int W, int H, int variant)
{
    Buf b;
    b.resize(W, H);
    const int SS = 4;       // samples per pixel across and down
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < SS; ++j)
                for (int i = 0; i < SS; ++i) {
                    const double u = (x + (i + 0.5) / SS) / W, v = (y + (j + 0.5) / SS) / H;
                    double r = 0.2 + 0.6 * u, g = 0.3 + 0.5 * v, bl = 0.5 + 0.3 * std::sin(u * 14.0) * std::cos(v * 9.0);
                    if ((((int)std::floor(u * 12)) + ((int)std::floor(v * 9))) % 2 == 0) { r *= 0.5; bl = 1.0 - bl; }       // blocks with hard edges
                    const double dx = (u - 0.7) * W / H, dy = v - 0.3;
                    if (dx * dx + dy * dy < 0.12 * 0.12) { r = 1.0; g = 0.95; bl = 0.8; }                                      // a bright disc
                    if (variant == 1 && v > 0.55 && v < 0.8 && std::fmod(u * 40.0, 1.0) < 0.5) { r = g = bl = 0.95; }         // stripes 1/80 of the width
                    double a = 1.0;
                    if (u < 1.0 / 6 && v > 5.0 / 6) a = 0.25 + 0.5 * u * 6;                                                     // soft alpha corner
                    acc[0] += r * a; acc[1] += g * a; acc[2] += bl * a; acc[3] += a;
                }
            float* p = b.at(x, y);
            for (int k = 0; k < 4; ++k) p[k] = (float)(acc[k] / (SS * SS));
        }
    return b;
}

// A picture averaged down by a whole number (the same shape, 1/f of the size).
static Buf reduce(const Buf& s, int f)
{
    if (f == 1) return s;
    Buf o;
    o.resize(s.w / f, s.h / f);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) {
            double a[4] = {0, 0, 0, 0};
            for (int j = 0; j < f; ++j)
                for (int i = 0; i < f; ++i)
                    for (int k = 0; k < 4; ++k) a[k] += s.at(x * f + i, y * f + j)[k];
            for (int k = 0; k < 4; ++k) o.at(x, y)[k] = (float)(a[k] / (f * f));
        }
    return o;
}

struct Diff { double mean = 0, mx = 0; };
// What the host would store: 0..1 rounded to a byte; differences in levels, over all four channels.
static Diff compare(const Buf& a, const Buf& b)
{
    Diff d;
    if (a.w != b.w || a.h != b.h) { d.mean = d.mx = 999; return d; }
    auto q = [](float v) { return std::floor(std::min(1.f, std::max(0.f, v)) * 255.f + 0.5f); };
    double sum = 0;
    const size_t n = a.px.size();
    for (size_t i = 0; i < n; ++i) {
        const double df = std::fabs((double)q(a.px[i]) - q(b.px[i]));
        sum += df;
        d.mx = std::max(d.mx, df);
    }
    d.mean = sum / (double)n;
    return d;
}

static uint64_t hashBuf(const Buf& b)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t* p = (const uint8_t*)b.px.data();
    for (size_t i = 0; i < b.px.size() * sizeof(float); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

// Text for the title effects: a block per letter, as tall as the font size and 0.6 of it wide (what a font gives, in this
// test, at every size: the shapes of real letters at other sizes are not the same pixels).
static bool blockRasterizer(const ofx::TextRequest& rq, ofx::TextMask& m)
{
    std::vector<std::string> lines;
    std::string cur;
    for (char c : rq.text) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
    lines.push_back(cur);
    const int lh = std::max(1, (int)std::lround(rq.pixelSize * 1.2 * rq.lineSpacing));
    const int cw = std::max(1, (int)std::lround(rq.pixelSize * 0.6 + rq.tracking * rq.pixelSize));
    size_t longest = 0;
    for (auto& l : lines) longest = std::max(longest, l.size());
    m.w = std::max(1, (int)longest * cw);
    m.h = std::max(1, lh * (int)lines.size());
    m.a.assign((size_t)m.w * m.h, 0);
    const int gh = std::max(1, (int)std::lround(rq.pixelSize * 0.8)), gw = std::max(1, (int)std::lround(rq.pixelSize * 0.4));
    for (size_t li = 0; li < lines.size(); ++li) {
        const int offset = rq.align == 1 ? (m.w - (int)lines[li].size() * cw) / 2 : (rq.align == 2 ? m.w - (int)lines[li].size() * cw : 0);
        for (size_t ci = 0; ci < lines[li].size(); ++ci) {
            if (lines[li][ci] == ' ') continue;
            for (int y = 0; y < gh; ++y)
                for (int x = 0; x < gw; ++x) {
                    const int xx = offset + (int)ci * cw + x, yy = (int)li * lh + y;
                    if (xx >= 0 && xx < m.w && yy >= 0 && yy < m.h) m.a[(size_t)yy * m.w + xx] = 255;
                }
        }
    }
    return true;
}


// ------------------------------------------------------------------ the engine's side
// A still that is the test picture at any size asked for.
static int gStillW = 640, gStillH = 360;          // (the size of the project's still: --host-time makes it 1920 x 1080)
class SynthStill : public MediaReader {
public:
    MediaInfo mi;
    int variant = 0;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        const double k = std::min(1.0, std::min((double)maxW / mi.width, (double)maxH / mi.height));
        const int w = std::max(1, (int)std::lround(mi.width * k)), h = std::max(1, (int)std::lround(mi.height * k));
        auto it = cache.find(w * 100000 + h);              // (made once per size: the picture is slow to draw)
        if (it == cache.end()) it = cache.emplace(w * 100000 + h, toImage(picture(w, h, variant))).first;
        return it->second;
    }
    std::map<int, ImagePtr> cache;
    bool audio(int64_t, int, int, float*, float*) override { return false; }
    // (Reno's pictures: 8-bit blue first, bottom row first, straight in the colour that is premultiplied here: the pictures are opaque but a corner)
    static ImagePtr toImage(const Buf& b)
    {
        ImagePtr img = Image::make(b.w, b.h, PixelDepth::Byte, false);
        for (int y = 0; y < b.h; ++y) {
            uint8_t* d = img->row(b.h - 1 - y);
            for (int x = 0; x < b.w; ++x, d += 4) {
                const float* p = b.at(x, y);
                auto q = [](float v) { return (uint8_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f); };
                d[0] = q(p[2]); d[1] = q(p[1]); d[2] = q(p[0]); d[3] = q(p[3]);
            }
        }
        return img;
    }
};
static std::unique_ptr<MediaReader> openSynth(const std::string& path, std::string*)
{
    std::unique_ptr<SynthStill> r(new SynthStill());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = gStillW;
    r->mi.height = gStillH;
    r->variant = path == "synth:1" ? 1 : 0;
    return std::unique_ptr<MediaReader>(r.release());
}

static FxSlot slot(Project& p, const std::string& id, const Json& params = Json())
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    s.name = id;
    if (params.isObject()) {
        Json st = Json::object();
        st["params"] = params;
        s.state = st;
    }
    return s;
}
static Json val(std::initializer_list<double> v)
{
    Json e = Json::object(), a = Json::array();
    for (double x : v) a.push(x);
    e["v"] = a;
    return e;
}

struct Rig {
    MediaPool pool;
    FxRuntime fx;
    VideoEngine video{pool, fx};
    FrameOptions fo;
    Rig() { pool.setOpener(openSynth); }
    ~Rig() { fx.releaseExcept({}); }
    Project base()
    {
        Project p;
        p.settings.width = gStillW;
        p.settings.height = gStillH;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        p.tracks.push_back(t);
        return p;
    }
    MediaItem still(Project& p, const char* path)
    {
        MediaItem mi;
        mi.id = p.newId();
        mi.path = path;
        std::string err;
        mi.info = openSynth(path, &err)->info();
        p.media.push_back(mi);
        return mi;
    }
    MediaItem generator(Project& p, const std::string& id, const Json& params = Json())
    {
        MediaItem g;
        g.id = p.newId();
        g.name = "Generated";
        g.generated = true;
        g.generator = slot(p, id, params);
        g.info.hasVideo = true;
        g.info.width = 640;
        g.info.height = 360;
        p.media.push_back(g);
        return g;
    }
    Event event(Project& p, const MediaItem& m, double start, double length)
    {
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = start;
        e.length = length;
        return e;
    }
    // (every test project numbers its slots from 1: the instances of the one before must go)
    void fresh() { fx.clear(); pool.clear(); }
    ImagePtr render(const Project& p, double t, double scale, double* ms = nullptr)
    {
        fx.prepare(p);
        fo.scale = scale;
        fo.parallel = false;
        const auto t0 = std::chrono::steady_clock::now();
        ImagePtr img = video.render(p, t, fo);
        if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return img;
    }
};

// The picture averaged down by a whole number, in the engine's form (8-bit, bottom row first); mean difference in levels over B, G, R.
static ImagePtr reduceImg(const ImagePtr& s, int f)
{
    ImagePtr o = Image::make(s->w / f, s->h / f, PixelDepth::Byte, false);
    for (int y = 0; y < o->h; ++y)
        for (int x = 0; x < o->w; ++x) {
            int a[4] = {0, 0, 0, 0};
            for (int j = 0; j < f; ++j)
                for (int i = 0; i < f; ++i)
                    for (int k = 0; k < 4; ++k) a[k] += s->row(y * f + j)[(x * f + i) * 4 + k];
            for (int k = 0; k < 4; ++k) o->row(y)[x * 4 + k] = (uint8_t)((a[k] + f * f / 2) / (f * f));
        }
    return o;
}
static double meanDiff(const ImagePtr& a, const ImagePtr& b, double* mx = nullptr)
{
    if (!a || !b || a->w != b->w || a->h != b->h) return 999;
    double sum = 0, top = 0;
    for (int y = 0; y < a->h; ++y)
        for (int x = 0; x < a->w; ++x)
            for (int k = 0; k < 3; ++k) {
                const double d = std::abs((int)a->row(y)[x * 4 + k] - (int)b->row(y)[x * 4 + k]);
                sum += d;
                top = std::max(top, d);
            }
    if (mx) *mx = top;
    return sum / ((double)a->w * a->h * 3);
}

static const std::string kV = "com.vegascreativesoftware:";

// Every filter effect of an OpenFX file, one at a time on the test picture: whether it says it supports multi-resolution, and how
// the picture at 1/2 and 1/4 compares with the full-size one averaged down.
static void pluginFileReport(Rig& rig, const std::string& file)
{
    std::string err;
    ofx::Binary* bin = ofx::Host::get().loadBinary(file, &err);
    check(bin != nullptr, "the effects of " + file + " load", err);
    if (!bin) return;
    for (const auto& up : bin->plugins) {
        ofx::Plugin* pl = up.get();
        if (!pl || !pl->load()) continue;
        if (!pl->supportsContext(kOfxImageEffectContextFilter) && !pl->supportsContext(kOfxImageEffectContextGeneral)) continue;
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, pl->info.id));
        p.tracks[0].events.push_back(e);
        rig.fresh();
        ImagePtr full = rig.render(p, 1.0, 1.0), half = rig.render(p, 1.0, 0.5), quarter = rig.render(p, 1.0, 0.25);
        const std::string why = rig.fx.error(p.tracks[0].events[0].fx[0].id);
        if (!full || !half || !quarter) { check(false, pl->info.id + ": renders", why); continue; }
        double m2 = 0, m4 = 0;
        const double d2 = meanDiff(half, reduceImg(full, 2), &m2), d4 = meanDiff(quarter, reduceImg(full, 4), &m4);
        std::printf("INFO %-50s %-17s 1/2: mean %.3f max %.0f   1/4: mean %.3f max %.0f%s\n", pl->info.id.c_str(), pl->info.multiResolution ? "multi-resolution" : "project size only", d2, m2, d4, m4,
                    why.empty() ? "" : ("  (" + why + ")").c_str());
    }
}


// The engine's cost of effects: CPU ms of a frame with the effect minus one without, at 1/4, 1/2 and full size of a 1920 x 1080 project.
static int hostTimeTests(const std::string& ids, double onlyScale)
{
    gStillW = 1920;
    gStillH = 1080;
    Rig rig;
    const double scales[3] = {0.25, 0.5, 1.0};
    const int nScales = onlyScale > 0 ? 1 : 3;
    auto cpu = [&](const Project& p, double scale) {
        double best = 1e30;
        for (int rep = 0; rep < (scale > 0.9 ? 3 : 15); ++rep) {
            rig.fresh();
            rig.render(p, 1.0, scale);                    // (the first: opens the still, makes the instances)
            const std::clock_t t0 = std::clock();
            for (int k = 0; k < 4; ++k) rig.render(p, 1.0 + 0.001 * k, scale);
            best = std::min(best, 1000.0 * (double)(std::clock() - t0) / CLOCKS_PER_SEC / 4.0);
        }
        return best;
    };
    Project none = rig.base();
    {
        MediaItem m = rig.still(none, "synth:0");
        none.tracks[0].events.push_back(rig.event(none, m, 0, 5));
    }
    double base[3];
    for (int i = 0; i < nScales; ++i) base[i] = cpu(none, onlyScale > 0 ? onlyScale : scales[i]);
    std::printf("HOST %-40s  %7.3f  %7.3f  %7.3f   (a frame without an effect, CPU ms, at 1/4, 1/2, full size)\n", "(no effect)", base[0], nScales > 1 ? base[1] : 0.0, nScales > 1 ? base[2] : 0.0);
    size_t pos = 0;
    while (pos <= ids.size()) {
        size_t e = ids.find(',', pos);
        if (e == std::string::npos) e = ids.size();
        const std::string id = ids.substr(pos, e - pos);
        pos = e + 1;
        if (id.empty()) continue;
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event ev = rig.event(p, m, 0, 5);
        ev.fx.push_back(slot(p, id.find(':') == std::string::npos ? kV + id : id));
        p.tracks[0].events.push_back(ev);
        std::printf("HOST %-40s", id.c_str());
        for (int i = 0; i < nScales; ++i) std::printf("  %s %7.3f", onlyScale > 0 ? "scale" : i == 0 ? "1/4" : i == 1 ? "1/2" : "full", cpu(p, onlyScale > 0 ? onlyScale : scales[i]) - base[i]);
        std::printf("\n");
        std::fflush(stdout);
    }
    return 0;
}

static int engineTests(const std::string& testfx, bool stretch, const std::string& pluginFile)
{
    if (stretch)
        for (const auto& info : ofx::builtinInfos())
            if (const Def* d = findDef(info.id)) const_cast<Def*>(d)->multiRes = false;
    bool haveFx = false;
    if (!testfx.empty()) {
        std::string err;
        haveFx = ofx::Host::get().loadBinary(testfx, &err) != nullptr;
        check(haveFx, "the test effects load", err);
    }
    Rig rig;
    if (!pluginFile.empty()) {
        pluginFileReport(rig, pluginFile);
        return 0;
    }
    struct Case { std::string name; Project p; double t; };
    std::vector<Case> cases;
    {   // effects on a still: blurs, light, a frame, a sharpen, a vignette
        Project p = rig.base();
        MediaItem m = rig.still(p, "synth:0");
        Event e = rig.event(p, m, 0, 5);
        e.fx.push_back(slot(p, kV + "gaussianblur", [] { Json j = Json::object(); j["HorizontalRange"] = val({0.12}); j["VerticalRange"] = val({0.12}); return j; }()));
        e.fx.push_back(slot(p, kV + "glow"));
        e.fx.push_back(slot(p, kV + "border", [] { Json j = Json::object(); j["Type"] = val({2}); j["Size"] = val({0.08}); return j; }()));
        e.fx.push_back(slot(p, kV + "sharpen"));
        e.fx.push_back(slot(p, kV + "vignette"));
        p.tracks[0].events.push_back(e);
        cases.push_back({"six effects on an event", p, 1.0});
    }
    {   // a generator, with an effect on it
        Project p = rig.base();
        MediaItem g = rig.generator(p, kV + "checkerboard");
        Event e = rig.event(p, g, 0, 5);
        e.fx.push_back(slot(p, kV + "gaussianblur"));
        p.tracks[0].events.push_back(e);
        MediaItem n = rig.generator(p, kV + "noisetexture");
        Event e2 = rig.event(p, n, 0, 5);
        e2.fx.push_back(slot(p, kV + "colorcorrector"));
        Track t2;
        t2.id = p.newId();
        t2.type = TrackType::Video;
        t2.events.push_back(e2);
        t2.level = 0.5;
        p.tracks.push_back(t2);
        cases.push_back({"generators, one with effects, over another", p, 1.0});
    }
    {   // an OpenFX transition between two events, each with an effect; a track effect too
        Project p = rig.base();
        MediaItem a = rig.still(p, "synth:0"), b = rig.still(p, "synth:1");
        Event e1 = rig.event(p, a, 0, 4), e2 = rig.event(p, b, 2, 4);
        e1.fx.push_back(slot(p, kV + "invert"));
        e2.fx.push_back(slot(p, kV + "sepia"));
        e2.transition = slot(p, kV + "crosseffect");
        p.tracks[0].events.push_back(e1);
        p.tracks[0].events.push_back(e2);
        p.tracks[0].fx.push_back(slot(p, kV + "glow"));
        cases.push_back({"a transition between events with effects, a track effect", p, 3.0});
    }
    if (std::getenv("FXS_DEBUG")) {
        Project p = rig.base();
        MediaItem a = rig.still(p, "synth:0"), b = rig.still(p, "synth:1");
        Event e1 = rig.event(p, a, 0, 4), e2 = rig.event(p, b, 2, 4);
        e2.transition = slot(p, kV + std::getenv("FXS_DEBUG"));
        p.tracks[0].events.push_back(e1);
        p.tracks[0].events.push_back(e2);
        cases.push_back({std::string("debug transition ") + std::getenv("FXS_DEBUG"), p, 3.0});
    }
    // against the full-size picture averaged down
    const double scales[2] = {0.5, 0.25};
    for (auto& c : cases) {
        rig.fresh();
        double msFull = 0;
        ImagePtr full = rig.render(c.p, c.t, 1.0, &msFull);
        check(full && full->w == 640 && full->h == 360, c.name + ": the full-size picture is the project's", full ? strfmt("%dx%d", full->w, full->h) : "none");
        for (int k = 0; k < 2; ++k) {
            const int f = k == 0 ? 2 : 4;
            double ms = 0, mx = 0;
            ImagePtr small = rig.render(c.p, c.t, scales[k], &ms);
            check(small && small->w == 640 / f && small->h == 360 / f, strfmt("%s: at 1/%d the picture is 1/%d of the size", c.name.c_str(), f, f),
                  small ? strfmt("%dx%d", small->w, small->h) : "none");
            if (!small || !full) continue;
            const double d = meanDiff(small, reduceImg(full, f), &mx);
            check(d < 5.0, strfmt("%s: at 1/%d it is the full-size one averaged down", c.name.c_str(), f),
                  strfmt("mean %.3f, largest %.0f levels; %.1f ms against %.1f ms at full size", d, mx, ms, msFull));
        }
    }
    // the test effect: told the scale and given pictures of the smaller size when it supports multi-resolution, else left at full size
    if (haveFx) {
        void* h = dlopen(testfx.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        auto count = h ? (int (*)(void))dlsym(h, "testfx_log_count") : nullptr;
        auto line = h ? (const char* (*)(int))dlsym(h, "testfx_log_line") : nullptr;
        auto clear = h ? (void (*)(void))dlsym(h, "testfx_log_clear") : nullptr;
        const bool multi = std::getenv("TESTFX_MULTIRES") != nullptr;
        check(count && line && clear, "the test effects' log is there");
        if (count && line && clear) {
            Project p = rig.base();
            MediaItem m = rig.still(p, "synth:0");
            Event e = rig.event(p, m, 0, 5);
            e.fx.push_back(slot(p, "reno.test.invert"));
            p.tracks[0].events.push_back(e);
            rig.fresh();
            clear();
            ImagePtr small = rig.render(p, 1.0, 0.5);
            std::string scaledLine, invertLine;
            for (int i = 0; i < count(); ++i) {
                const std::string l = line(i);
                if (l.rfind("scaled:", 0) == 0) scaledLine = l;
                if (l.rfind("invert:", 0) == 0) invertLine = l;
            }
            check(small && small->w == 320 && small->h == 180, "the test effect: the picture is 320 x 180 either way", small ? strfmt("%dx%d", small->w, small->h) : "none");
            if (multi) {
                check(scaledLine.find("renderScale=0.5,0.5") != std::string::npos && scaledLine.find("image=0,0,320,180") != std::string::npos &&
                          scaledLine.find("rod=320,180") != std::string::npos && scaledLine.find("window=0,0,320,180") != std::string::npos,
                      "it supports multi-resolution: told render scale 0.5, handed 320 x 180 pictures and a render window of that size", scaledLine);
            } else {
                check(scaledLine.empty() && invertLine.find("window=0,0,640,360") != std::string::npos,
                      "it does not: rendered at the project's 640 x 360 with a render scale of 1 (its picture stretched to it and back)", invertLine);
            }
            // at full size, the same: scale 1, whole picture
            clear();
            ImagePtr full = rig.render(p, 1.0, 1.0);
            scaledLine.clear();
            for (int i = 0; i < count(); ++i) if (std::string(line(i)).rfind("scaled:", 0) == 0) scaledLine = line(i);
            check(!multi || (scaledLine.find("renderScale=1,1") != std::string::npos && scaledLine.find("image=0,0,640,360") != std::string::npos),
                  "at full size: render scale 1 and 640 x 360 pictures", scaledLine);
            if (small && full) {
                double mx = 0;
                const double d = meanDiff(small, reduceImg(full, 2), &mx);
                check(d < 2.0, "the test effect at half size agrees with the full-size picture averaged down", strfmt("mean %.3f, largest %.0f levels", d, mx));
            }
        }
    }
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}

typedef std::map<std::string, std::vector<double>> Params;

struct Rendered { Buf out; };

typedef std::map<std::string, std::string> Strings;
static Buf run(const Def& def, const Params& over, const Buf& src, const Buf& src2, double scale, bool tellScale, double progress, double time, const Strings* overStr = nullptr)
{
    Params store;
    Strings strs;
    for (const PDef& p : def.params) {
        if (p.type == PT::Group || p.type == PT::Button) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    if (overStr) for (const auto& kv : *overStr) strs[kv.first] = kv.second;
    if (def.transition) store["Transition"] = {progress};
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.time = time;
    c.fps = 30;
    c.src = src;
    c.src2 = src2;
    c.progress = progress;
    c.out.resize(src.w, src.h);
    c.frameW = src.w;
    c.frameH = src.h;
#ifndef FXS_BASE
    c.scale = tellScale ? scale : 1.0;
#else
    (void)scale; (void)tellScale;
#endif
    def.render(c);
    return c.out;
}

// The best time (ms) of def.render on a prepared Ctx, over several runs (the output buffer is the one the host would reuse).
static double timeRun(const Def& def, const Params& over, const Buf& src, const Buf& src2, double scale, double progress, double time, const Strings* overStr = nullptr)
{
    Params store;
    Strings strs;
    for (const PDef& p : def.params) {
        if (p.type == PT::Group || p.type == PT::Button) continue;
        if (p.type == PT::String || p.type == PT::Custom) strs[p.name] = p.sdef;
        else store[p.name] = p.def;
    }
    for (const auto& kv : over) store[kv.first] = kv.second;
    if (overStr) for (const auto& kv : *overStr) strs[kv.first] = kv.second;
    if (def.transition) store["Transition"] = {progress};
    Ctx c;
    c.testParams = &store;
    c.testStrings = &strs;
    c.time = time;
    c.fps = 30;
    c.src = src;
    c.src2 = src2;
    c.progress = progress;
    c.frameW = src.w;
    c.frameH = src.h;
    c.scale = scale;
    double best = 1e30;
    const int reps = src.w * src.h > 600000 ? 3 : 9;
    for (int i = 0; i < reps; ++i) {
        if (def.overwritesOut) c.out.ensure(src.w, src.h);
        else c.out.resize(src.w, src.h);
        const std::clock_t t0 = std::clock();      // (the processors' time, all threads: steady where other programs share the machine)
        def.render(c);
        const double ms = 1000.0 * (double)(std::clock() - t0) / CLOCKS_PER_SEC;
        best = std::min(best, ms);
    }
    return best;
}

// Settings that make an effect do something visible where its defaults do little (name -> overrides); several per effect allowed.
struct Variant { std::string suffix; Params p; Strings s; };
static std::vector<Variant> variantsFor(const std::string& id)
{
    const std::string V = "com.vegascreativesoftware:";
    std::vector<Variant> v;
    v.push_back({"", {}});
    auto add = [&](const char* name, const char* sfx, Params p) { if (id == V + name) v.push_back({sfx, p}); };
    add("gaussianblur", "wide", {{"HorizontalRange", {0.4}}, {"VerticalRange", {0.4}}});
    add("gaussianblur", "small", {{"HorizontalRange", {0.01}}, {"VerticalRange", {0.01}}});
    add("border", "bevel", {{"Type", {1}}, {"Size", {0.1}}});
    add("border", "solid", {{"Type", {2}}, {"Size", {0.1}}});
    add("sharpen", "full", {{"Amount", {1}}});
    add("pixelate", "coarse", {{"HorizontalPixelization", {0.5}}, {"VerticalPixelization", {0.5}}});
    add("pixelate", "fine", {{"HorizontalPixelization", {0.05}}, {"VerticalPixelization", {0.05}}});
    add("quickblur", "full", {{"BlendAmount", {1}}});
    add("median", "big", {{"HorizontalRange", {1}}, {"VerticalRange", {1}}});
    add("convolutionkernel", "sharpen", {{"Row1Column2", {-1}}, {"Row2Column1", {-1}}, {"Row2Column3", {-1}}, {"Row3Column2", {-1}}, {"Row2Column2", {5}}});
    add("convolutionkernel", "edge+turn", {{"Row1Column2", {-1}}, {"Row2Column1", {-1}}, {"Row2Column3", {-1}}, {"Row3Column2", {-1}}, {"Row2Column2", {4}}, {"Rotation", {30}}, {"AutoNormalize", {0}}});
    add("deform", "bent", {{"Amount", {1}}, {"SqueezeLeft", {0.3}}, {"ShearVertical", {0.2}}, {"CurveTop", {0.3}}});
    add("layerdimensionality", "all", {{"ShadowType", {1}}, {"ShadowHeight", {0.2}}, {"ShadowBlur", {0.5}}, {"GlowType", {1}}, {"GlowAmount", {0.5}}, {"ForegroundAmount", {1}}});
    add("tvsimulator", "grill", {{"ApertureGrill", {0.6}}, {"Interlacing", {0.5}}, {"Static", {0}}});
    add("filmeffects", "dust", {{"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {1}}});
    add("filmeffects", "scratch", {{"EnableEnableParticles", {1}}, {"EnableParticlesType", {4}}, {"EnableParticlesAmount", {1}}});
    add("cookiecutter", "feather", {{"Feather", {1}}, {"Border", {0.1}}, {"RepeatX", {3}}, {"RepeatY", {2}}, {"Shape", {4}}});
    add("newsprint", "tiny", {{"DotSize", {0.03}}});
    add("newsprint", "large", {{"DotSize", {0.4}}});
    add("newsprint", "mono", {{"DotSize", {0.2}}, {"Type", {1}}});
    add("glow", "strong", {{"Intensity", {8}}, {"GlowPercent", {0.5}}});
    add("chromablur", "wide", {{"HorizontalPixels", {20}}, {"VerticalPixels", {20}}});
    add("defocusvelvetmatter", "big", {{"Radius", {40}}});
    add("radialblur", "strong", {{"Strength", {0.8}}});
    add("minandmax", "big", {{"HorizontalRange", {1}}, {"VerticalRange", {1}}});
    add("wave", "strong", {{"HorizontalAmplitude", {0.1}}, {"VerticalAmplitude", {0.1}}, {"HorizontalWaves", {30}}});
    add("filmgrain", "coarse", {{"Amount", {1}}, {"Granularity", {1}}});
    add("addnoise", "gaussian", {{"GaussianNoise", {1}}, {"Monochromatic", {0}}});
    // (more settings for the effects the speed-ups of the built-ins touch: what they must leave exactly as it was)
    add("convolutionkernel", "emboss", {{"Row1Column1", {2}}, {"Row2Column2", {0}}, {"Row3Column3", {-2}}, {"Offset", {0.5}}, {"AutoNormalize", {1}}});
    add("convolutionkernel", "blur", {{"Row1Column1", {4}}, {"Row1Column2", {4}}, {"Row1Column3", {4}}, {"Row2Column1", {4}}, {"Row2Column2", {5}}, {"Row2Column3", {4}}, {"Row3Column1", {4}}, {"Row3Column2", {4}}, {"Row3Column3", {4}}});
    add("convolutionkernel", "scale", {{"Row1Column1", {1}}, {"Row1Column3", {-1}}, {"Row3Column1", {2}}, {"Row2Column2", {0}}, {"Row3Column3", {-0.5}}, {"AutoNormalize", {0}}, {"Scale", {0.7}}, {"Offset", {0.1}}});
    add("convolutionkernel", "turn90", {{"Row1Column2", {-1}}, {"Row2Column1", {-1}}, {"Row2Column3", {-1}}, {"Row3Column2", {-1}}, {"Row2Column2", {5}}, {"Rotation", {90}}});
    add("mirror", "left", {{"Angle", {90}}});
    add("mirror", "turned", {{"Angle", {33}}, {"Center", {0.4, 0.6}}});
    add("mirror", "bottom", {{"Angle", {180}}, {"Center", {0.5, 0.3}}});
    add("mirror", "bg", {{"Angle", {-90}}, {"Center", {0.2, 0.5}}, {"Background", {0.2, 0.4, 0.6, 1}}});
    add("cookiecutter", "circle", {{"Shape", {0}}, {"Size", {0.2}}});
    add("cookiecutter", "cutout", {{"Shape", {3}}, {"Method", {1}}, {"Size", {0.3}}, {"Border", {0.05}}, {"Color", {1, 0, 0}}});
    add("cookiecutter", "tri", {{"Shape", {6}}, {"Size", {0.1}}, {"RepeatX", {2}}, {"RepeatY", {2}}, {"Feather", {0.3}}});
    add("cookiecutter", "arrow", {{"Shape", {10}}, {"Size", {0.15}}, {"Border", {0.1}}, {"Feather", {0.5}}, {"Center", {0.3, 0.6}}});
    add("cookiecutter", "oval3d", {{"Shape", {1}}, {"Size", {0.25}}, {"StereoscopicDepth", {0.05}}});
    add("raysvelvetmatter", "strong", {{"EffectAmount", {60}}, {"EffectThreshold", {30}}, {"EffectPosition", {0.3, 0.7}}});
    add("raysvelvetmatter", "only", {{"EffectEffectOnly", {1}}, {"EffectHue", {120}}, {"EffectSaturation", {80}}});
    add("raysvelvetmatter", "key", {{"EffectKeyAlphaChannel", {1}}, {"EffectPosition", {0.8, 0.2}}});
    add("starburstvelvetmatter", "strong", {{"Strength", {100}}, {"CenterBurst", {60}}});
    add("filllightvelvetmatter", "soft", {{"Softness", {60}}, {"Tint", {80}}});
    add("layerdimensionality", "shadow", {{"ShadowType", {1}}, {"ShadowHeight", {0.1}}, {"ShadowBlur", {0.3}}, {"ShadowAmount", {0.7}}});
    add("layerdimensionality", "glow", {{"GlowType", {2}}, {"GlowAmount", {0.3}}});
    add("layerdimensionality", "glow0", {{"GlowType", {3}}, {"GlowAmount", {0.2}}, {"ForegroundAmount", {0}}});
    add("layerdimensionality", "emboss", {{"ForegroundAmount", {1.5}}, {"EmbossRoundness", {0.5}}, {"LightLocation", {3, -3, 1}}});
    // a mesh of 4 x 4 points with the inner ones moved (WarpData: 4 ints, then x y of each point)
    if (id == "de.magix:MeshWarp") {
        for (int dir = 0; dir < 2; ++dir) {
            int32_t hdr[4] = {4, 4, 16, 0};
            float pts[32];
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) {
                    float x = i / 3.f, y = j / 3.f;
                    if (i > 0 && i < 3 && j > 0 && j < 3) { x += 0.06f * (j == 1 ? 1 : -1); y += 0.05f * (i == 1 ? -1 : 1); }
                    pts[(j * 4 + i) * 2] = x; pts[(j * 4 + i) * 2 + 1] = y;
                }
            std::string raw(16 + sizeof pts, '\0');
            std::memcpy(&raw[0], hdr, 16);
            std::memcpy(&raw[16], pts, sizeof pts);
            Variant vr;
            vr.suffix = dir ? "target" : "source";
            vr.p = {{"WarpDirection", {(double)dir}}};
            vr.s = {{"WarpData", base64Encode(raw.data(), raw.size())}};
            v.push_back(vr);
        }
    }
    add("timecode", "big", {{"Width", {1}}, {"Height", {1}}});
    return v;
}

// Effects with random per-pixel output (grain, noise, static, dust) cannot match pixel by pixel, so the mean change
// they make to the picture is compared instead.
static bool stochastic(const std::string& label)
{
    return label == "addnoise" || label == "filmgrain" || label == "filmeffects" || label == "tvsimulator";
}

int main(int argc, char** argv)
{
    bool tellScale = true, hashOnly = false, timeOnly = false, engineMode = false, stretch = false;
    std::string testfx, pluginFile, hostTime;
    double hostScale = 0;
    int W = 960, H = 540;
    std::string only, csv;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--ignore-scale")) tellScale = false;
        else if (!std::strcmp(argv[i], "--hash")) hashOnly = true;
        else if (!std::strcmp(argv[i], "--time")) timeOnly = true;
        else if (!std::strcmp(argv[i], "--host-time") && i + 1 < argc) hostTime = argv[++i];
        else if (!std::strcmp(argv[i], "--host-scale") && i + 1 < argc) hostScale = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) setProcessorLimit(std::atoi(argv[++i]));
        else if (!std::strcmp(argv[i], "--size") && i + 2 < argc) { W = std::atoi(argv[i + 1]); H = std::atoi(argv[i + 2]); i += 2; }
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) only = argv[++i];
        else if (!std::strcmp(argv[i], "--csv") && i + 1 < argc) csv = argv[++i];
        else if (!std::strcmp(argv[i], "--engine")) { engineMode = true; if (i + 1 < argc && argv[i + 1][0] != '-') testfx = argv[++i]; }
        else if (!std::strcmp(argv[i], "--stretch")) stretch = true;
        else if (!std::strcmp(argv[i], "--plugins") && i + 1 < argc) pluginFile = argv[++i];
    }
    ofx::setTextRasterizer(blockRasterizer);
    if (!hostTime.empty()) return hostTimeTests(hostTime, hostScale);
    if (engineMode) return engineTests(testfx, stretch, pluginFile);
    const Buf pic = picture(W, H, 0), pic2 = picture(W, H, 1);
    Buf other = pic2;      // the picture coming in, for transitions: the stripes one, flipped so that it differs everywhere
    std::vector<ofx::BuiltinInfo> infos = ofx::builtinInfos();
    FILE* cf = csv.empty() ? nullptr : std::fopen(csv.c_str(), "w");
    if (cf) std::fprintf(cf, "effect,variant,mean_half,max_half,mean_quarter,max_quarter,change\n");
    if (!hashOnly && !timeOnly) std::printf("%-44s %-8s | %-20s | %-20s | %s\n", "effect", "variant", "1/2 mean  max", "1/4 mean  max", "change (levels, full size vs source)");
    int count = 0;
    for (const auto& info : infos) {
        const Def* def = findDef(info.id);
        if (!def || !def->render) continue;
        if (!only.empty() && info.id.find(only) == std::string::npos) continue;
        for (const Variant& var : variantsFor(info.id)) {
            const std::string label = info.id.substr(info.id.find(':') == std::string::npos ? 0 : info.id.find(':') + 1);
            const double progress = 0.37, time = 14;
            const Buf full = run(*def, var.p, pic, other, 1.0, true, progress, time, &var.s);
            if (hashOnly) {
                std::printf("HASH %s %s %016llx", info.id.c_str(), var.suffix.c_str(), (unsigned long long)hashBuf(full));
                for (int f : {2, 4}) {
                    const Buf got = run(*def, var.p, reduce(pic, f), reduce(other, f), 1.0 / f, tellScale, progress, time, &var.s);
                    std::printf(" %016llx", (unsigned long long)hashBuf(got));
                }
                std::printf("\n");
                continue;
            }
            if (timeOnly) {
                std::printf("TIME %-40s %-8s", label.c_str(), var.suffix.c_str());
                for (int f : {1, 2, 4}) {
                    const Buf s = reduce(pic, f), s2 = reduce(other, f);
                    std::printf(" %8.3f", timeRun(*def, var.p, s, s2, 1.0 / f, progress, time, &var.s));
                }
                std::printf("\n");
                std::fflush(stdout);
                continue;
            }
            double change = 0;
            {
                Diff d = def->generator ? Diff() : compare(full, pic);
                change = d.mean;
            }
            Diff dd[2];
            double ratio[2] = {0, 0};
            const int fs[2] = {2, 4};
            for (int k = 0; k < 2; ++k) {
                const Buf s = reduce(pic, fs[k]), s2 = reduce(other, fs[k]);
                const Buf got = run(*def, var.p, s, s2, 1.0 / fs[k], tellScale, progress, time, &var.s);
                const Buf want = reduce(full, fs[k]);
                dd[k] = compare(got, want);
                if (stochastic(label)) {
                    const double a = compare(got, s).mean, b = compare(want, s).mean;
                    ratio[k] = b > 0 ? a / b : 0;
                }
            }
            ++count;
            std::printf("%-44s %-8s | %7.3f %7.1f | %7.3f %7.1f | %7.3f%s\n", label.c_str(), var.suffix.c_str(), dd[0].mean, dd[0].mx, dd[1].mean, dd[1].mx, change,
                        (!def->generator && change < 0.2) ? "  (little change)" : "");
            if (stochastic(label)) std::printf("%-44s %-8s   random: strength of the change, smaller / full-size averaged down: 1/2 %.2f  1/4 %.2f\n", "", "", ratio[0], ratio[1]);
            if (cf) std::fprintf(cf, "%s,%s,%.4f,%.1f,%.4f,%.1f,%.4f\n", label.c_str(), var.suffix.c_str(), dd[0].mean, dd[0].mx, dd[1].mean, dd[1].mx, change);
            std::fflush(stdout);
        }
    }
    if (cf) std::fclose(cf);
    if (hashOnly || timeOnly) return 0;
    std::printf("%d effect renderings compared\n", count);
    return 0;
}
