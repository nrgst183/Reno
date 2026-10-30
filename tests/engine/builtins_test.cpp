// Tests of the built-in OpenFX plug-ins (src/ofx/builtins*.cpp): catalog entries, rendering through the normal OpenFX
// path, results on plain pictures, nonsense settings, and settings saved in a project. Titles & Text is tested with
// a stand-in text rasterizer, VEGAS's rich text with real samples.
//
//   builtins_test [sample projects folder]
#include "../../src/engine/engine.h"
#include "../../src/engine/catalog.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/vegas/vegas.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <set>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A still of one colour ("solid:b,g,r") or the left half one colour and the right another ("split:b,g,r,b,g,r").
class Still : public MediaReader {
public:
    MediaInfo mi;
    int c[6] = {0, 0, 0, 0, 0, 0};
    bool split = false;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        ImagePtr img = Image::make(std::min(maxW, mi.width), std::min(maxH, mi.height), PixelDepth::Byte, false);
        for (int y = 0; y < img->h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < img->w; ++x, d += 4) {
                const int* k = (split && x >= img->w / 2) ? c + 3 : c;
                d[0] = (uint8_t)k[0]; d[1] = (uint8_t)k[1]; d[2] = (uint8_t)k[2]; d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};
static std::unique_ptr<MediaReader> openStill(const std::string& path, std::string* err)
{
    std::unique_ptr<Still> r(new Still());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 160;
    r->mi.height = 90;
    if (std::sscanf(path.c_str(), "split:%d,%d,%d,%d,%d,%d", &r->c[0], &r->c[1], &r->c[2], &r->c[3], &r->c[4], &r->c[5]) == 6) r->split = true;
    else if (std::sscanf(path.c_str(), "solid:%d,%d,%d", &r->c[0], &r->c[1], &r->c[2]) != 3) {
        // anything else: the real reader (the sample project's media)
        return openMediaOrProject(path, err);
    }
    return std::unique_ptr<MediaReader>(r.release());
}

struct Px { int b, g, r, a; };
// (x, y from the top left; Reno's pictures keep their rows bottom first)
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return {-1, -1, -1, -1};
    y = img->h - 1 - y;
    if (img->depth == PixelDepth::Float) {
        const float* f = (const float*)img->row(y) + x * 4;
        auto q = [](float v) { return (int)std::lround(std::min(1.f, std::max(0.f, v)) * 255); };
        return {q(f[2]), q(f[1]), q(f[0]), q(f[3])};
    }
    const uint8_t* d = img->row(y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool near(const Px& p, int r, int g, int b, int tol = 3) { return std::abs(p.r - r) <= tol && std::abs(p.g - g) <= tol && std::abs(p.b - b) <= tol; }

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
static Json str(const std::string& s)
{
    Json e = Json::object();
    e["s"] = s;
    return e;
}
static const std::string kV = "com.vegascreativesoftware:";

struct Rig {
    MediaPool pool;
    FxRuntime fx;
    VideoEngine video{pool, fx};
    FrameOptions fo;
    Rig() { pool.setOpener(openStill); }
    ~Rig() { fx.releaseExcept({}); }
    Project base()
    {
        Project p;
        p.settings.width = 160;
        p.settings.height = 90;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        p.tracks.push_back(t);
        return p;
    }
    // A project with one generated event of 'id' over 0..5 s.
    Project generated(const std::string& id, const Json& params = Json())
    {
        Project p = base();
        MediaItem g;
        g.id = p.newId();
        g.name = "Generated";
        g.generated = true;
        g.generator = slot(p, id, params);
        g.info.hasVideo = true;
        g.info.width = 160;
        g.info.height = 90;
        p.media.push_back(g);
        Event e;
        e.id = p.newId();
        e.mediaId = g.id;
        e.length = 5;
        p.tracks[0].events.push_back(e);
        return p;
    }
    // A project with one event of the still 'path' carrying the effect 'id'.
    Project filtered(const std::string& path, const std::string& id, const Json& params = Json())
    {
        Project p = base();
        MediaItem mi;
        mi.id = p.newId();
        mi.path = path;
        std::string err;
        mi.info = openStill(path, &err)->info();
        p.media.push_back(mi);
        Event e;
        e.id = p.newId();
        e.mediaId = mi.id;
        e.length = 5;
        e.fx.push_back(slot(p, id, params));
        p.tracks[0].events.push_back(e);
        return p;
    }
    ImagePtr render(const Project& p, double t = 1.0)
    {
        // (every test project numbers its slots from 1: the instances of the one before must go)
        fx.clear();
        pool.clear();
        fx.prepare(p);
        return video.render(p, t, fo);
    }
    std::string error(const Project& p)
    {
        const Event& e = p.tracks[0].events[0];
        if (!e.fx.empty()) return fx.error(e.fx[0].id);
        const MediaItem* m = p.findMedia(e.mediaId);
        return m ? fx.error(m->generator.id) : std::string("?");
    }
};

// The stand-in rasterizer: each character a solid block (half an em wide, an em tall), spaces empty.
static int gRasterCalls = 0;
static ofx::TextRequest gLastRequest;
static bool blockText(const ofx::TextRequest& r, ofx::TextMask& m)
{
    ++gRasterCalls;
    gLastRequest = r;
    std::vector<std::string> lines(1);
    for (char c : r.text) { if (c == '\n') lines.emplace_back(); else lines.back() += c; }
    size_t longest = 0;
    for (const auto& l : lines) longest = std::max(longest, l.size());
    const int em = std::max(1, (int)std::lround(r.pixelSize));
    m.w = std::max(1, (int)longest * em / 2);
    m.h = (int)lines.size() * em;
    m.a.assign((size_t)m.w * m.h, 0);
    for (size_t li = 0; li < lines.size(); ++li)
        for (size_t k = 0; k < lines[li].size(); ++k) {
            if (lines[li][k] == ' ') continue;
            for (int y = (int)li * em; y < (int)(li + 1) * em; ++y)
                for (int x = (int)k * em / 2; x < (int)(k + 1) * em / 2 && x < m.w; ++x) m.a[(size_t)y * m.w + x] = 255;
        }
    return true;
}

int main(int argc, char** argv)
{
    const std::string samples = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");

    // ---- listed with nothing scanned
    {
        const auto infos = ofx::builtinInfos();
        std::set<std::string> ids;
        int gens = 0, filters = 0;
        for (const auto& i : infos) { ids.insert(i.id); gens += i.generator; filters += i.filter; }
        check(gens == 7 && filters >= 15 && ids.size() == infos.size(), "the built-ins: seven generators and the effects, each identifier once",
              strfmt("%d generators, %d effects", gens, filters));
        int listed = 0;
        for (const auto& e : Catalog::get().video()) if (e.builtin && e.ok && e.path == ofx::kBuiltinBinaryPath) ++listed;
        check(listed == (int)infos.size(), "the catalog lists them all before any scan", strfmt("%d of %d", listed, (int)infos.size()));
        VideoFxEntry e;
        check(Catalog::get().findVideo(kV + "solidcolor", &e) && e.generator && !e.filter && e.label == "Solid Color", "Solid Color is a generator in the catalog");
        check(Catalog::get().findVideo(kV + "gaussianblur", &e) && e.filter && !e.generator, "Gaussian Blur is a filter in the catalog");
        // each one describes itself through the host like a plug-in file
        std::string err;
        ofx::Binary* bin = ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath, &err);
        bool allOk = bin && bin->builtin && bin->plugins.size() == infos.size();
        std::string bad;
        if (bin)
            for (auto& pl : bin->plugins) {
                std::string e2;
                if (!pl->load(&e2)) { allOk = false; bad += pl->info.id + " (" + e2 + ") "; continue; }
                const std::string ctx = pl->supportsContext("OfxImageEffectContextGenerator") ? "OfxImageEffectContextGenerator" : pl->supportsContext("OfxImageEffectContextTransition") ? "OfxImageEffectContextTransition" : pl->supportsContext("OfxImageEffectContextFilter") ? "OfxImageEffectContextFilter" : "OfxImageEffectContextGeneral";
                if (!pl->describeInContext(ctx, &e2)) { allOk = false; bad += pl->info.id + " in context (" + e2 + ") "; }
            }
        check(allOk, "every built-in loads and describes itself through the OpenFX host", bad + err);
        check(ofx::Host::get().loadBinary(ofx::kBuiltinBinaryPath) == bin, "and is loaded once");
        // the presets: well-formed, each for a built-in
        const std::string& xml = ofx::builtinPresetPackage();
        size_t n = 0, unknown = 0, pos = 0;
        while ((pos = xml.find("<OfxPreset plugin=\"", pos)) != std::string::npos) {
            pos += 19;
            const std::string id = xml.substr(pos, xml.find('"', pos) - pos);
            ++n;
            if (!ids.count(id)) ++unknown;
        }
        check(n >= 60 && unknown == 0 && xml.find("</OfxPresetPackage>") != std::string::npos, "the presets they come with", strfmt("%zu presets, %zu for unknown effects", n, unknown));
        // scanning a folder with other plug-ins keeps them
        Catalog::get().scanInProcess({}, {});
        listed = 0;
        for (const auto& e3 : Catalog::get().video()) if (e3.builtin) ++listed;
        check(listed == (int)infos.size(), "and after a scan that found nothing");
    }

    Rig rig;
    // ---- generators
    {
        Json ps = Json::object();
        ps["Color"] = val({1, 0.5, 0, 1});
        Project p = rig.generated(kV + "solidcolor", ps);
        ImagePtr img = rig.render(p);
        Px a = pixel(img, 10, 10), b = pixel(img, 150, 80);
        check(near(a, 255, 128, 0) && near(b, 255, 128, 0) && a.a == 255, "Solid Color fills the frame with its colour", pxs(a) + " / " + pxs(b) + " " + rig.error(p));
        Project d = rig.generated(kV + "solidcolor");
        img = rig.render(d);
        a = pixel(img, 80, 45);
        check(near(a, 0, 0, 0) && a.a == 255, "Solid Color's default is black", pxs(a));
        // half transparent over a white track below
        ps["Color"] = val({0, 0, 1, 0.5});
        Project h = rig.generated(kV + "solidcolor", ps);
        Track below;
        below.id = h.newId();
        below.type = TrackType::Video;
        MediaItem w;
        w.id = h.newId();
        w.path = "solid:255,255,255";
        std::string err;
        w.info = openStill(w.path, &err)->info();
        h.media.push_back(w);
        Event we;
        we.id = h.newId();
        we.mediaId = w.id;
        we.length = 5;
        below.events.push_back(we);
        h.tracks.push_back(below);
        img = rig.render(h);
        a = pixel(img, 80, 45);
        check(near(a, 128, 128, 255, 4), "a half transparent Solid Color lets the track below through", pxs(a));
    }
    {
        Json ps = Json::object();
        ps["Color1"] = val({0, 0, 0, 1});
        ps["Color2"] = val({1, 1, 1, 1});
        Project p = rig.generated(kV + "colorgradient", ps);
        ImagePtr img = rig.render(p);
        Px l = pixel(img, 2, 45), m = pixel(img, 80, 45), r = pixel(img, 157, 45);
        check(l.r < 60 && r.r > 195 && std::abs(m.r - 128) < 20 && l.r < m.r && m.r < r.r, "Color Gradient (linear) runs from Color 1 on the left to Color 2 on the right",
              pxs(l) + " / " + pxs(m) + " / " + pxs(r));
        ps["GradientType"] = val({1});
        ps["Size"] = val({0.5});
        Project e = rig.generated(kV + "colorgradient", ps);
        img = rig.render(e);
        Px c = pixel(img, 80, 45), o = pixel(img, 2, 2);
        check(c.r < 10 && o.r > 245, "elliptical: Color 1 at the centre, Color 2 outside", pxs(c) + " / " + pxs(o));
    }
    {
        Json ps = Json::object();
        ps["TileDimensionsWidth"] = val({0.25});
        ps["TileDimensionsSquare"] = val({0});
        ps["TileDimensionsHeight"] = val({0.5});
        ps["Color1"] = val({1, 0, 0, 1});
        ps["Color2"] = val({0, 0, 1, 1});
        Project p = rig.generated(kV + "checkerboard", ps);
        ImagePtr img = rig.render(p);
        Px a = pixel(img, 10, 10), b = pixel(img, 50, 10), c = pixel(img, 10, 60), d = pixel(img, 50, 60);
        check(near(a, 255, 0, 0) && near(b, 0, 0, 255) && near(c, 0, 0, 255) && near(d, 255, 0, 0), "Checkerboard alternates its two colours across and down",
              pxs(a) + " " + pxs(b) + " " + pxs(c) + " " + pxs(d));
    }
    {
        Project p = rig.generated(kV + "noisetexture");
        ImagePtr img = rig.render(p);
        int lo = 255, hi = 0;
        for (int y = 0; y < 90; y += 3)
            for (int x = 0; x < 160; x += 3) { const int v = pixel(img, x, y).r; lo = std::min(lo, v); hi = std::max(hi, v); }
        check(hi - lo > 80, "Noise Texture makes a texture (light and dark parts)", strfmt("%d..%d", lo, hi));
        ImagePtr again = rig.render(p);
        bool same = again && img && again->w == img->w;
        for (int y = 0; same && y < 90; y += 7) same = pixel(img, 33, y).r == pixel(again, 33, y).r;
        check(same, "and the same one every time");
        Json ps = Json::object();
        ps["OffsetprogressinDegrees"] = val({90});
        Project q = rig.generated(kV + "noisetexture", ps);
        ImagePtr moved = rig.render(q);
        int diff = 0;
        for (int y = 0; y < 90; y += 5)
            for (int x = 0; x < 160; x += 5) diff += std::abs(pixel(moved, x, y).r - pixel(img, x, y).r);
        check(diff > 500, "Progress changes it (keyframed, it moves)", strfmt("%d", diff));
    }
    {
        Project p = rig.generated(kV + "testpattern");
        ImagePtr img = rig.render(p);
        Px grey = pixel(img, 5, 20), blue = pixel(img, 155, 20);
        check(near(grey, 180, 180, 180, 2) && near(blue, 180, 16, 16, 2), "Test Pattern: SMPTE bars by default (VEGAS's: 180 grey on the left, 180/16/16 at the right edge)", pxs(grey) + " / " + pxs(blue));
        Json ps = Json::object();
        ps["TestPatternType"] = val({4});
        Project r = rig.generated(kV + "testpattern", ps);
        img = rig.render(r);
        check(near(pixel(img, 20, 45), 25 + 4 * 204 / 128, 25 + 4 * 204 / 128, 25 + 4 * 204 / 128, 3) && near(pixel(img, 140, 45), 25 + 124 * 204 / 128, 25 + 124 * 204 / 128, 25 + 124 * 204 / 128, 3),
              "Ramp: VEGAS's 25..229 grey steps across the middle", pxs(pixel(img, 20, 45)) + " / " + pxs(pixel(img, 140, 45)));
        ps["TestPatternType"] = val({99});
        Project bad = rig.generated(kV + "testpattern", ps);
        img = rig.render(bad);
        check(img && pixel(img, 80, 45).a >= 0, "a pattern number that does not exist draws black rather than failing");
    }
    // ---- Titles & Text
    {
        Json ps = Json::object();
        ps["Text"] = str("HI");
        ps["FontSize"] = val({100});
        ps["Background"] = val({0, 0, 1, 1});
        Project p = rig.generated(kV + "titlesandtext", ps);
        ofx::setTextRasterizer(nullptr);
        ImagePtr img = rig.render(p);
        check(near(pixel(img, 80, 45), 0, 0, 255), "Titles & Text with no font machinery: just its background", pxs(pixel(img, 80, 45)) + " " + rig.error(p));
        ofx::setTextRasterizer(blockText);
        gRasterCalls = 0;
        rig.fx.clear();
        img = rig.render(p);
        // 100 points at 90 lines: 22.5 px em; "HI" is 22 px wide, centred at (80, 45)
        Px c = pixel(img, 74, 45), off = pixel(img, 80, 10);
        check(gRasterCalls > 0 && near(c, 255, 255, 255) && near(off, 0, 0, 255), "the text in Text color, centred, over the background", pxs(c) + " / " + pxs(off));
        check(std::fabs(gLastRequest.pixelSize - 22.5) < 0.01 && gLastRequest.font == "Arial" && gLastRequest.text == "HI", "its size scales with the frame (100 points: a quarter of the height)",
              strfmt("%.2f px, %s", gLastRequest.pixelSize, gLastRequest.font.c_str()));
        // anchor: bottom left at (0.1, 0.1)
        ps["Alignment"] = val({6});
        ps["Location"] = val({0.1, 0.1});
        ps["Background"] = val({0, 0, 0, 0});
        Project q = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(q);
        Px in = pixel(img, 20, 75), out = pixel(img, 80, 45);
        check(near(in, 255, 255, 255) && near(out, 0, 0, 0), "Anchor Point and Location place the block; no background is transparent",
              pxs(in) + " / " + pxs(out));
        // outline and shadow
        ps["Alignment"] = val({4});
        ps["Location"] = val({0.5, 0.5});
        ps["OutlineWidth"] = val({10});
        ps["OutlineColor"] = val({1, 0, 0, 1});
        Project o = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(o);
        // the block spans x 69..91, y 34..56; the outline (10 at 720 lines: 1.25 px) just outside
        bool red = false;
        for (int x = 64; x < 70; ++x) { Px e = pixel(img, x, 45); if (e.r > 120 && e.g < 80) red = true; }
        check(red, "an outline round the letters in its colour");
        // VEGAS's rich text: font, size and style from it
        ps["Text"] = str("{\\rtf1\\ansi\\ansicpg1252\\deff0\\nouicompat\\deflang1033{\\fonttbl{\\f0\\fnil\\fcharset0 Impact;}}\r\n"
                         "{\\*\\generator Riched20 10.0.19041}\\viewkind4\\uc1 \r\n\\pard\\qc\\b\\f0\\fs96 Title\\par\r\n}\r\n");
        ps["OutlineWidth"] = val({0});
        Project rtf = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(rtf);
        check(gLastRequest.text == "Title" && gLastRequest.font == "Impact" && gLastRequest.bold && std::fabs(gLastRequest.pixelSize - 48 * 90 / 400.0) < 0.01,
              "VEGAS's rich text: its words, font, size (\\fs96 = 48 points) and bold", gLastRequest.text + " / " + gLastRequest.font + strfmt(" %.2f", gLastRequest.pixelSize));
        // nonsense: a huge size, an empty text, negative scale
        ps["Text"] = str("");
        Project empty = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(empty);
        check(img && near(pixel(img, 80, 45), 0, 0, 0), "empty text: nothing drawn");
        ps["Text"] = str("BIG");
        ps["FontSize"] = val({1000});
        ps["Scale"] = val({10});
        Project big = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(big);
        check(img && gLastRequest.pixelSize <= 90 * 4 + 0.01, "a huge size is kept within reason", strfmt("%.1f px", gLastRequest.pixelSize));
        ps["Scale"] = val({-3});
        Project neg = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(neg);
        check(img != nullptr, "a negative scale draws nothing rather than failing");
    }
    // ---- Titles & Text's animations by VEGAS's hidden AnimationName, or the Animation choice
    {
        Json ps = Json::object();
        ps["Text"] = str("HI");
        ps["FontSize"] = val({100});
        ps["AnimationName"] = str("_Fly_in");
        Project p = rig.generated(kV + "titlesandtext", ps);
        // at rest the block spans x 69..91 round (80, 45)
        ImagePtr img = rig.render(p, 0.0);
        check(img && near(pixel(img, 74, 45), 0, 0, 0), "Fly In (by name): at the start the text is not yet in place", pxs(pixel(img, 74, 45)));
        img = rig.render(p, 0.5);
        bool left = false;
        for (int x = 0; x < 66; ++x) if (pixel(img, x, 45).r > 200) left = true;
        check(left && !(pixel(img, 85, 45).r > 200), "Fly In: half way it is coming in from the left");
        img = rig.render(p, 1.5);
        check(near(pixel(img, 74, 45), 255, 255, 255), "Fly In: after a second it is in place", pxs(pixel(img, 74, 45)));
        ps["AnimationName"] = str("");
        ps["collection"] = val({10});
        Project q = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(q, 0.0);
        check(img && near(pixel(img, 74, 45), 0, 0, 0), "the Animation choice (Fly In) does the same");
        ps["collection"] = val({0});
        ps["AnimationName"] = str("_Popup");
        Project pop = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(pop, 0.0);
        Px a = pixel(img, 74, 45);
        img = rig.render(pop, 2.0);
        check(near(a, 0, 0, 0) && near(pixel(img, 74, 45), 255, 255, 255), "Popup: grows from nothing to its size");
        ps["AnimationName"] = str("_Scroll");
        Project sc = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(sc, 0.0);
        bool any = false;
        for (int y = 0; y < 90; ++y) for (int x = 60; x < 100; ++x) if (pixel(img, x, y).r > 200) any = true;
        Px mid = pixel(rig.render(sc, 4.9), 74, 45);
        check(!any && near(mid, 255, 255, 255), "Scroll: a credit roll from below the frame, through the middle near 5 s", pxs(mid));
        // Credit Roll rows roll from below the frame to above it over the Roll duration
        {
            Json cs = Json::object();
            cs["Text"] = str("// a comment\n Cast\nAlice\tHero");
            cs["FontSize"] = val({30});
            cs["RollDuration"] = val({4});    // (the test events are 5 s long)
            Project cr = rig.generated(kV + "creditroll", cs);
            auto whites = [&](ImagePtr im, int x0, int x1) {
                int n = 0;
                for (int y = 0; y < 90; ++y) for (int x = x0; x < x1; ++x) if (pixel(im, x, y).r > 200) ++n;
                return n;
            };
            gRasterCalls = 0;
            ImagePtr a0 = rig.render(cr, 0.0), a5 = rig.render(cr, 2.0), a10 = rig.render(cr, 4.9);
            check(whites(a0, 0, 160) == 0 && whites(a10, 0, 160) == 0, "Credit Roll: nothing at the start (below the frame) or at the end (above it)",
                  strfmt("%d / %d", whites(a0, 0, 160), whites(a10, 0, 160)));
            check(whites(a5, 0, 80) > 0 && whites(a5, 80, 160) > 0, "Credit Roll: in the middle of the roll the two columns are either side of the centre",
                  strfmt("%d / %d", whites(a5, 0, 80), whites(a5, 80, 160)));
            ImagePtr a2 = rig.render(cr, 1.0);
            int lowest2 = -1, lowest5 = -1;
            for (int y = 0; y < 90; ++y) for (int x = 0; x < 160; ++x) { if (pixel(a2, x, y).r > 200) lowest2 = y; if (pixel(a5, x, y).r > 200) lowest5 = y; }
            check(lowest2 > lowest5, "Credit Roll: it rolls up", strfmt("%d then %d", lowest2, lowest5));
            check(gRasterCalls == 4 * 3 && gLastRequest.text != "// a comment", "Credit Roll: the comment line is not drawn (the title and the two columns are)", strfmt("%d calls", gRasterCalls));
        }
        ps["AnimationName"] = str("_Title07");
        Project fade = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(fade, 0.25);
        Px f = pixel(img, 74, 45);
        check(f.r > 60 && f.r < 200, "VEGAS's numbered titles fade in", pxs(f));
        ps["AnimationName"] = str("_No_Such_Animation");
        ps["collection"] = val({999});
        Project odd = rig.generated(kV + "titlesandtext", ps);
        img = rig.render(odd, 3.0);
        check(img && near(pixel(img, 74, 45), 255, 255, 255), "an unknown animation and a nonsense choice: the text still ends up in place");
    }
    // ---- rich text on its own
    {
        auto r = ofx::parseRichText("{\\rtf1\\ansi{\\fonttbl{\\f0\\fswiss Arial;}{\\f1\\froman Times New Roman;}}\\f1\\fs40\\i Caf\\'e9 \\u8364? two\\par line\\par}");
        check(r.text == "Caf\xc3\xa9 \xe2\x82\xac two\nline" && r.font == "Times New Roman" && r.points == 20 && r.italic && !r.bold,
              "rich text: accents, Unicode, paragraphs, the font used and its size", r.text + " / " + r.font + strfmt(" %.1f", r.points));
        auto plain = ofx::parseRichText("just\r\ntext");
        check(plain.text == "just\ntext" && plain.font.empty(), "plain text is taken as it is");
        auto broken = ofx::parseRichText("{\\rtf1{\\fonttbl{\\f0 Ari");
        check(broken.text.empty(), "broken rich text gives what could be read (and no crash)");
        std::string junk = "{\\rtf1";
        for (int k = 0; k < 2000; ++k) junk += (k % 3) ? "{\\u-3" : "}\\'zz\\";
        (void)ofx::parseRichText(junk);
        check(true, "nonsense rich text does not crash");
    }

    // ---- effects
    {
        const std::string blue = "solid:200,100,50";     // b, g, r
        Project p = rig.filtered(blue, kV + "invert");
        Px a = pixel(rig.render(p), 80, 45);
        check(near(a, 205, 155, 55), "Invert turns the colours over", pxs(a) + " " + rig.error(p));
        Project bw = rig.filtered(blue, kV + "blackandwhite");
        a = pixel(rig.render(bw), 80, 45);
        const int l = (50 + 100 + 200) / 3;       // VEGAS: the integer mean of the three levels
        check(near(a, l, l, l, 1), "Black and White: the grey is the mean of the levels", pxs(a));
        Json ps = Json::object();
        ps["Brightness"] = val({0.2});
        Project br = rig.filtered(blue, kV + "brightnessandcontrast", ps);
        a = pixel(rig.render(br), 80, 45);
        check(near(a, 50 + 51, 100 + 51, 200 + 51), "Brightness and Contrast: brighter", pxs(a));
        ps = Json::object();
        ps["Saturation"] = val({0});
        Project hs = rig.filtered(blue, kV + "hsladjust", ps);
        a = pixel(rig.render(hs), 80, 45);
        check(a.r == a.g && a.g == a.b, "HSL Adjust: saturation 0 greys", pxs(a));
        ps = Json::object();
        ps["BlendingStrength"] = val({1});
        Project sp = rig.filtered(blue, kV + "sepia", ps);
        a = pixel(rig.render(sp), 80, 45);
        check(a.r > a.b && a.g > a.b, "Sepia: warm greys", pxs(a));
        ps = Json::object();
        ps["Threshold"] = val({0.5});
        Project th = rig.filtered(blue, kV + "threshold", ps);
        a = pixel(rig.render(th), 80, 45);
        check(near(a, 0, 0, 0), "Threshold: dark becomes black", pxs(a));
        // Rays: the Points setting (an integer parameter) changes the star
        {
            auto sb = [&](int pts) {
                Json q = Json::object();
                q["Strength"] = val({100});
                q["Threshold"] = val({0});
                q["Radius"] = val({10});
                q["Points"] = val({(double)pts});
                return rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "starburstvelvetmatter", q));
            };
            ImagePtr p2 = sb(2), p4 = sb(4), p6 = sb(6);
            long d24 = 0, d46 = 0;
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) {
                    d24 += std::abs(pixel(p2, x, y).g - pixel(p4, x, y).g);
                    d46 += std::abs(pixel(p4, x, y).g - pixel(p6, x, y).g);
                }
            check(d24 > 0 && d46 > 0, "Rays: Points 2, 4 and 6 draw different stars", strfmt("%ld %ld", d24, d46));
        }
        // keying: the blue half goes, the red half stays
        ps = Json::object();
        ps["color"] = val({0, 0, 1});
        Project ck = rig.filtered("split:255,0,0,0,0,255", kV + "chromakeyer", ps);
        ImagePtr img = rig.render(ck);
        Px left = pixel(img, 20, 45), right = pixel(img, 140, 45);
        check(near(left, 0, 0, 0) && near(right, 255, 0, 0), "Chroma Keyer: the key colour becomes transparent (black behind), the rest stays", pxs(left) + " / " + pxs(right));
        // VEGAS's own numbers (its pictures of the default Blue Screen key): a grey keeps alpha 244, cyan 177 (blue kept: it is not
        // above green), magenta 232 with its blue removed; over black the colour is the premultiplied one
        {
            const char* solids[3] = {"solid:157,162,164", "solid:255,255,0", "solid:255,0,255"};      // (b, g, r)
            const int want[3][3] = {{156, 155, 150}, {0, 177, 177}, {232, 0, 0}};
            for (int k = 0; k < 3; ++k) {
                Px v = pixel(rig.render(rig.filtered(solids[k], kV + "chromakeyer")), 80, 45);
                check(near(v, want[k][0], want[k][1], want[k][2], 1), strfmt("Chroma Keyer matches VEGAS on b,g,r %s", solids[k] + 6), pxs(v));
            }
        }
        // mirror: the top kept and reflected down (the split is left/right: angle 90 keeps the left)
        ps = Json::object();
        ps["Angle"] = val({90});
        Project mr = rig.filtered("split:255,0,0,0,0,255", kV + "mirror", ps);
        img = rig.render(mr);
        check(near(pixel(img, 140, 45), 0, 0, 255), "Mirror: one side reflected over the other", pxs(pixel(img, 140, 45)));
        // blur: the edge between the halves softens
        ps = Json::object();
        ps["HorizontalRange"] = val({0.5});
        Project gb = rig.filtered("split:0,0,0,255,255,255", kV + "gaussianblur", ps);
        img = rig.render(gb);
        const int e1 = pixel(img, 76, 45).r, e2 = pixel(img, 84, 45).r, far = pixel(img, 5, 45).r;
        check(e1 > 20 && e2 < 235 && far < 5, "Gaussian Blur softens an edge", strfmt("%d %d %d", e1, e2, far));
        Project px = rig.filtered("split:0,0,0,255,255,255", kV + "pixelate");
        img = rig.render(px);
        check(img != nullptr, "Pixelate renders");
        // Color Corrector (Secondary): hue turned half way round, a hue range limited, a colour keyed out
        ps = Json::object();
        ps["RotateHue"] = val({180});
        Project ccs = rig.filtered("solid:0,0,255", kV + "colorcorrectorsecondary", ps);
        a = pixel(rig.render(ccs), 80, 45);
        check(a.r < 60 && a.g > 100 && a.b > 100, "Color Corrector (Secondary): Rotate hue 180 turns red to cyan", pxs(a) + " " + rig.error(ccs));
        ps = Json::object();
        ps["Saturation"] = val({0});
        ps["LimitHueCenter"] = val({283.5});
        ps["LimitHueWidth"] = val({300});
        Project dr = rig.filtered("split:0,0,255,255,0,0", kV + "colorcorrectorsecondary", ps);
        img = rig.render(dr);
        left = pixel(img, 20, 45);
        right = pixel(img, 140, 45);
        check(near(left, 255, 0, 0) && right.r == right.g && right.g == right.b, "its hue limit (VEGAS's Desaturate All but Reds): red kept, blue greyed",
              pxs(left) + " / " + pxs(right));
        ps = Json::object();
        ps["Alpha"] = val({0});
        ps["LimitHueCenter"] = val({347.1});
        ps["LimitHueWidth"] = val({40});
        Project key = rig.filtered("split:0,0,255,255,0,0", kV + "colorcorrectorsecondary", ps);
        img = rig.render(key);
        check(near(pixel(img, 20, 45), 255, 0, 0) && near(pixel(img, 140, 45), 0, 0, 0), "and its Alpha makes the range transparent (blue keyed out)",
              pxs(pixel(img, 20, 45)) + " / " + pxs(pixel(img, 140, 45)));
        ps["ShowMask"] = val({1});
        Project mk = rig.filtered("split:0,0,255,255,0,0", kV + "colorcorrectorsecondary", ps);
        img = rig.render(mk);
        check(near(pixel(img, 20, 45), 0, 0, 0) && near(pixel(img, 140, 45), 255, 255, 255), "Show mask: white where it changes", pxs(pixel(img, 20, 45)) + " / " + pxs(pixel(img, 140, 45)));
        // Color Corrector: a midtone pushed to red
        ps = Json::object();
        ps["Mid"] = val({103.5, 1});
        Project cc = rig.filtered("solid:128,128,128", kV + "colorcorrector", ps);
        a = pixel(rig.render(cc), 80, 45);
        check(a.r > a.g + 30 && a.r > a.b + 30, "Color Corrector: the Mid wheel towards red reddens a grey", pxs(a));
        // Convolution Kernel: the identity changes nothing; Find Edges makes a flat picture black and finds the edge
        Project ck1 = rig.filtered(blue, kV + "convolutionkernel");
        a = pixel(rig.render(ck1), 80, 45);
        check(near(a, 50, 100, 200, 1), "Convolution Kernel's default (the identity) changes nothing", pxs(a));
        ps = Json::object();
        for (int k = 0; k < 9; ++k) ps[strfmt("Row%dColumn%d", k / 3 + 1, k % 3 + 1)] = val({k == 4 ? -8.0 : 1.0});
        Project fe = rig.filtered("split:0,0,0,255,255,255", kV + "convolutionkernel", ps);
        img = rig.render(fe);
        check(near(pixel(img, 20, 45), 0, 0, 0) && pixel(img, 79, 45).r + pixel(img, 80, 45).r > 200, "Find Edges: black on flat parts, bright at the edge",
              pxs(pixel(img, 20, 45)) + " / " + pxs(pixel(img, 79, 45)) + " " + pxs(pixel(img, 80, 45)));
        ps["Rotation"] = val({45});
        Project fr = rig.filtered("split:0,0,0,255,255,255", kV + "convolutionkernel", ps);
        check(rig.render(fr) != nullptr, "and turned");
        // Spherize: the centre stays, the edge of a split moves
        ps = Json::object();
        ps["Amount"] = val({1});
        ps["Center"] = val({0.4, 0.5});
        Project sz = rig.filtered("split:0,0,0,255,255,255", kV + "spherize", ps);
        img = rig.render(sz);
        check(near(pixel(img, 85, 45), 0, 0, 0) && near(pixel(img, 150, 45), 255, 255, 255), "Spherize bulges (the edge pushed away from the centre)", pxs(pixel(img, 85, 45)));
        // Swirl turns a split's straight edge into a curve; Pinch/Punch moves it; Wave ripples it; at Amount 0
        // / amplitude 0 they change nothing
        {
            auto edgeAt = [&](const ImagePtr& im, int y) { int x = 0; while (x < 159 && pixel(im, x, y).r < 128) ++x; return x; };
            Project base = rig.filtered("split:0,0,0,255,255,255", kV + "spherize", Json::object());
            ps = Json::object();
            ps["Amount"] = val({1});
            img = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "swirl", ps));
            // (inside its circle the halves are twisted into each other; the corners, outside it, stay)
            int moved = 0;
            for (int y = 0; y < 90; ++y)
                for (int x = 0; x < 160; ++x) moved += (pixel(img, x, y).r > 128) != (x >= 80);
            check(img && moved > 1500 && near(pixel(img, 2, 2), 0, 0, 0) && near(pixel(img, 157, 87), 255, 255, 255), "Swirl twists the picture inside its circle",
                  strfmt("%d pixels changed sides", moved));
            ps["Center"] = val({0.4, 0.5});
            ps["ScalingHorizontal"] = val({0.5});
            img = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "pinchpunch", ps));
            check(img && edgeAt(img, 45) != 80 && edgeAt(img, 2) == 80, "Pinch/Punch moves the edge inside its circle, not outside", strfmt("%d %d", edgeAt(img, 45), edgeAt(img, 2)));
            ps = Json::object();
            ps["Amount"] = val({0});
            img = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "swirl", ps));
            check(img && edgeAt(img, 30) == 80 && edgeAt(img, 60) == 80, "Swirl at 0 changes nothing");
            ps = Json::object();
            ps["HorizontalWaves"] = val({2});
            ps["HorizontalAmplitude"] = val({0.1});
            ps["VerticalAmplitude"] = val({0});
            img = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "wave", ps));
            int lo = 999, hi = -1;
            for (int y = 0; y < 90; y += 3) { lo = std::min(lo, edgeAt(img, y)); hi = std::max(hi, edgeAt(img, y)); }
            check(img && hi - lo >= 20 && hi - lo <= 40, "Wave ripples the edge side to side by about its amplitude", strfmt("%d..%d", lo, hi));
            (void)base;
        }
        // Newsprint: monochrome dots on a mid grey are black or white (mostly)
        ps = Json::object();
        ps["Type"] = val({1});
        ps["DotSize"] = val({0.5});
        Project np = rig.filtered("solid:128,128,128", kV + "newsprint", ps);
        img = rig.render(np);
        int dark = 0, light = 0;
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) { const int v = pixel(img, x, y).r; dark += v < 40; light += v > 215; }
        check(dark > 2000 && light > 2000, "Newsprint: grey becomes black dots on white", strfmt("%d dark, %d light", dark, light));
        // every effect renders with its defaults, and with every number set to nonsense
        for (const auto& info : ofx::builtinInfos()) {
            if (!info.filter) continue;
            Project d = rig.filtered("split:10,200,30,240,20,100", info.id);
            ImagePtr im = rig.render(d);
            const std::string err = rig.error(d);
            check(im && err.empty(), info.label + " renders with its defaults", err);
        }
        int crashes = 0;
        for (const auto& info : ofx::builtinInfos()) {
            ofx::Plugin* pl = ofx::Host::get().findPlugin(info.id);
            if (!pl) continue;
            ofx::Descriptor* desc = pl->describeInContext(info.generator ? "OfxImageEffectContextGenerator" : info.transition ? "OfxImageEffectContextTransition" : "OfxImageEffectContextFilter");
            if (!desc) continue;
            for (double nonsense : {-1e9, 1e9, std::nan(""), 0.0}) {
                Json ps2 = Json::object();
                for (const auto& prm : desc->params.params) {
                    if (!prm->hasValue() || prm->dim <= 0) continue;
                    Json v = Json::object(), a2 = Json::array();
                    for (int k = 0; k < prm->dim; ++k) a2.push(nonsense);
                    v["v"] = a2;
                    ps2[prm->name] = v;
                }
                Project d = info.generator ? rig.generated(info.id, ps2) : rig.filtered("solid:1,2,3", info.id, ps2);
                if (std::getenv("BT_VERBOSE")) std::fprintf(stderr, "nonsense %s %g\n", info.id.c_str(), nonsense);
                if (!rig.render(d)) ++crashes;
            }
        }
        check(crashes == 0, "every built-in survives nonsense settings (huge, negative, not a number, zero)", strfmt("%d failed", crashes));
    }
    // ---- 32-bit projects
    {
        Json ps = Json::object();
        ps["Color"] = val({0.25, 0.5, 0.75, 1});
        Project p = rig.generated(kV + "solidcolor", ps);
        p.settings.pixelFormat = "32bit";
        ImagePtr img = rig.render(p);
        Px a = pixel(img, 80, 45);
        check(img && near(a, 64, 128, 191), "in a 32-bit project too", pxs(a));
    }
    // ---- kept in the project
    {
        Json ps = Json::object();
        ps["Color"] = val({0.1, 0.2, 0.3, 1});
        Project p = rig.generated(kV + "solidcolor", ps);
        rig.render(p);
        ofx::Instance* inst = rig.fx.video(p.media[0].generator.id);
        Json st = inst ? inst->saveState() : Json();
        Project q;
        Json j = p.toJson();
        check(q.fromJson(j) && q.media.size() == 1 && q.media[0].generator.pluginId == kV + "solidcolor", "a project keeps its built-in generator");
        check(st.get("params").get("Color").get("v").at(2).asDouble() == 0.3, "with its settings", st.dump());
    }

    // ---- a sample project with VEGAS's Titles & Text, rendered without VEGAS: the first .veg under the samples folder
    // (or the one in RENO_SAMPLE_VEG) that has such media
    {
        std::vector<std::string> files = testVegFiles(samples);
        if (!testSampleVeg().empty()) files = {testSampleVeg()};
        Project p;
        std::string err;
        const MediaItem* title = nullptr;
        for (const auto& path : files) {
            Project cand;
            if (!fileExists(path) || !loadProjectFile(path, cand, &err)) continue;
            bool has = false;
            for (const auto& m : cand.media) if (m.generated && m.generator.pluginId == kV + "titlesandtext") has = true;
            if (!has) continue;
            p = cand;
            for (const auto& m : p.media) if (m.generated && m.generator.pluginId == kV + "titlesandtext") { title = &m; break; }
            break;
        }
        if (!title) {
            std::printf("SKIP no sample project with Titles & Text media (set RENO_SAMPLES to a folder of .veg files, or RENO_SAMPLE_VEG)\n");
        } else {
            check(title != nullptr, "the sample project has Titles & Text media");
            if (title) {
                // a project of only that media, the size of the sample's
                Project q;
                q.settings = p.settings;
                q.settings.width = 320;
                q.settings.height = 180;
                q.media.push_back(*title);
                Track t;
                t.id = q.newId();
                t.type = TrackType::Video;
                Event e;
                e.id = q.newId();
                e.mediaId = title->id;
                e.length = 5;
                t.events.push_back(e);
                q.tracks.push_back(t);
                gRasterCalls = 0;
                rig.fx.clear();
                rig.fx.prepare(q);
                ImagePtr img = rig.video.render(q, 0.5, rig.fo);
                int lit = 0;
                for (int y = 0; img && y < img->h; y += 2)
                    for (int x = 0; x < img->w; x += 2) if (pixel(img, x, y).a > 0) ++lit;
                const std::string text = gLastRequest.text;
                check(img && gRasterCalls > 0 && !text.empty() && lit > 10, "its title renders with the built-in Titles & Text (text read from VEGAS's settings)",
                      strfmt("\"%s\" in %s, %d pixels; ", text.c_str(), gLastRequest.font.c_str(), lit) + rig.fx.error(title->generator.id));
            }
        }
    }

    // ---- the stock transitions (man3): in the catalog as transitions, from the outgoing picture to the incoming one
    {
        int nTrans = 0;
        for (const auto& info : ofx::builtinInfos()) if (info.transition) { ++nTrans; check(!info.filter && !info.generator, info.label + " is listed as a transition only"); }
        check(nTrans >= 20, "the built-in transitions are listed", strfmt("%d", nTrans));
        auto overlap = [&](const std::string& id, const Json& ps) {
            Project p = rig.base();
            MediaItem a, b;
            a.id = p.newId(); a.path = "solid:20,40,220"; a.info = openStill(a.path, nullptr)->info();     // b,g,r: red
            b.id = p.newId(); b.path = "solid:200,60,20"; b.info = openStill(b.path, nullptr)->info();     // blue
            p.media.push_back(a); p.media.push_back(b);
            Event e1; e1.id = p.newId(); e1.mediaId = a.id; e1.start = 0; e1.length = 4;
            Event e2; e2.id = p.newId(); e2.mediaId = b.id; e2.start = 2; e2.length = 4;
            e2.transition = slot(p, id, ps);
            p.tracks[0].events.push_back(e1);
            p.tracks[0].events.push_back(e2);
            return p;
        };
        for (const auto& info : ofx::builtinInfos()) {
            if (!info.transition) continue;
            Project p = overlap(info.id, Json());
            ImagePtr s0 = rig.render(p, 2.0 + 1.0 / 60), s1 = rig.render(p, 3.0), s2 = rig.render(p, 4.0 - 1.0 / 60);
            const std::string err = rig.fx.error(p.tracks[0].events[1].transition.id);
            check(s0 && s1 && s2 && err.empty(), info.label + " renders as a transition", err);
            if (!s0 || !s1 || !s2) continue;
            // mostly the outgoing picture (red) at the start and the incoming (blue) at the end, over the whole frame
            auto share = [&](const ImagePtr& im, bool red) {
                int n = 0, t = 0;
                for (int y = 0; y < 90; y += 5)
                    for (int x = 0; x < 160; x += 5) { const Px q = pixel(im, x, y); ++t; n += red ? (q.r > q.b + 60) : (q.b > q.r + 60); }
                return 100 * n / t;
            };
            const int r0 = share(s0, true), b2 = share(s2, false), r1 = share(s1, true), b1 = share(s1, false);
            check(r0 >= 60 && b2 >= 60, info.label + " starts on the outgoing picture and ends on the incoming one", strfmt("%d%% red at the start, %d%% blue at the end", r0, b2));
            (void)r1; (void)b1;
        }
        // the wipes are half way at half way; a clock wipe turns clockwise from the top
        {
            Project p = overlap(kV + "linearwipe", Json());
            ImagePtr im = rig.render(p, 3.0);
            const Px l = pixel(im, 8, 45), r = pixel(im, 152, 45);
            check(im && l.b > l.r && r.r > r.b, "Linear Wipe at half way has shown the incoming picture on the left only", pxs(l) + " / " + pxs(r));
            p = overlap(kV + "clockwipe", Json());
            im = rig.render(p, 2.0 + 0.3 * 2);
            const Px tr = pixel(im, 120, 20), tl = pixel(im, 40, 20);
            check(im && tr.b > tr.r && tl.r > tl.b, "Clock Wipe starts at the top and goes clockwise (upper right first)", pxs(tr) + " / " + pxs(tl));
            p = overlap(kV + "dissolve", Json());
            Px m = pixel(rig.render(p, 3.0), 80, 45);
            check(m.r > 60 && m.b > 60, "Dissolve at half way is both pictures", pxs(m));
        }
        // the transition keeps its settings, and the settings change the picture
        {
            Json ps = Json::object();
            ps["Angle"] = val({90});
            Project p = overlap(kV + "linearwipe", ps);
            ImagePtr im = rig.render(p, 3.0);
            const Px t = pixel(im, 80, 8), bt = pixel(im, 80, 82);
            check(im && (t.b > t.r) != (bt.b > bt.r), "Linear Wipe's Angle turns the wipe", pxs(t) + " / " + pxs(bt));
        }
        // nonsense
        int crashes = 0;
        for (const auto& info : ofx::builtinInfos()) {
            if (!info.transition) continue;
            ofx::Plugin* pl = ofx::Host::get().findPlugin(info.id);
            ofx::Descriptor* desc = pl ? pl->describeInContext("OfxImageEffectContextTransition") : nullptr;
            if (!desc) { ++crashes; continue; }
            for (double nonsense : {-1e9, 1e9, std::nan(""), 0.0}) {
                Json ps2 = Json::object();
                for (const auto& prm : desc->params.params) {
                    if (!prm->hasValue() || prm->dim <= 0 || prm->name == "Transition") continue;
                    Json v = Json::object(), a2 = Json::array();
                    for (int k = 0; k < prm->dim; ++k) a2.push(nonsense);
                    v["v"] = a2;
                    ps2[prm->name] = v;
                }
                if (!rig.render(overlap(info.id, ps2), 3.0)) ++crashes;
            }
        }
        check(crashes == 0, "every built-in transition survives nonsense settings", strfmt("%d failed", crashes));
    }

    // ---- the added stock effects (man3): what they do on plain pictures
    {
        auto lit = [&](const ImagePtr& im, int thresh) { int n = 0; for (int y = 0; im && y < 90; y += 3) for (int x = 0; x < 160; x += 3) n += pixel(im, x, y).r > thresh; return n; };
        // Quick Blur softens an edge
        Json ps = Json::object();
        ps["BlendAmount"] = val({1});
        ImagePtr im = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "quickblur", ps));
        Px a = pixel(im, 79, 45), b = pixel(im, 81, 45);
        check(im && a.r > 5 && a.r < 250, "Quick Blur softens an edge", pxs(a) + " / " + pxs(b));
        // Vignette darkens the corners and leaves the middle
        im = rig.render(rig.filtered("solid:200,200,200", kV + "vignette"));
        a = pixel(im, 80, 45); b = pixel(im, 2, 2);
        check(im && a.r > 190 && b.r < a.r - 20, "Vignette darkens the corners and leaves the middle", pxs(a) + " / " + pxs(b));
        // Saturation Adjust at -1 greys, Lab Adjust invert luminance changes a mid grey's neighbour
        ps = Json::object();
        ps["Amount"] = val({-1}); ps["Center"] = val({0.5}); ps["Spread"] = val({2}); ps["Low"] = val({-1}); ps["High"] = val({-1});
        im = rig.render(rig.filtered("solid:30,60,200", kV + "saturationadjust", ps));
        a = pixel(im, 80, 45);
        check(im && std::abs(a.r - a.b) < 40 && std::abs(a.r - a.g) < 40, "Saturation Adjust at -1 takes the colour away", pxs(a));
        // Channel Blend: red from blue
        ps = Json::object();
        ps["BLUE weight B"] = val({0}); ps["BLUE weight G"] = val({0}); ps["BLUE weight R"] = val({1});      // (VEGAS names the outputs and inputs in B, G, R order: the "BLUE" output is the picture's red, its "R" weight reads the picture's blue)
        im = rig.render(rig.filtered("solid:200,10,10", kV + "channelblend", ps));
        a = pixel(im, 80, 45);
        check(im && a.r > 180, "Channel Blend takes the red channel from the blue one", pxs(a));
        // Mask Generator: luminance as grey
        im = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "maskgenerator"));
        a = pixel(im, 20, 45); b = pixel(im, 140, 45);
        check(im && a.r < 20 && b.r > 235 && std::abs(b.r - b.b) < 6, "Mask Generator makes a grey mask from brightness", pxs(a) + " / " + pxs(b));
        // Crop keeps a box and clears the rest
        ps = Json::object();
        ps["XScale"] = val({0.5}); ps["YScale"] = val({0.5});
        ps["Location"] = val({0.5, 0.5});
        im = rig.render(rig.filtered("solid:100,100,100", kV + "crop", ps));
        a = pixel(im, 80, 45); b = pixel(im, 8, 8);
        check(im && a.r > 90 && b.r < 10, "Crop keeps the box and clears the rest (shows black behind)", pxs(a) + " / " + pxs(b));
        // Picture in Picture shrinks the picture
        ps = Json::object();
        ps["Location"] = val({0.5, 0.5});
        im = rig.render(rig.filtered("solid:100,100,100", kV + "pictureinpicture", ps));
        a = pixel(im, 80, 45); b = pixel(im, 8, 8);
        check(im && a.r > 90 && b.r < 10, "Picture in Picture shrinks the picture into the frame", pxs(a) + " / " + pxs(b));
        // Tiny Planet
        ps = Json::object();
        im = rig.render(rig.filtered("split:0,0,0,255,255,255", kV + "tinyplanet", ps));
        {
            Px left = pixel(im, 20, 45), right = pixel(im, 140, 45);
            check(im && right.r > left.r + 100, "Tiny Planet wraps the panorama round the middle (left and right of the planet show opposite halves)", pxs(left) + " / " + pxs(right));
        }
        // Timecode burns text in
        im = rig.render(rig.filtered("solid:0,0,0", kV + "timecode"), 1.0);
        check(im && lit(im, 128) > 5, "Timecode draws the time into the picture", strfmt("%d lit", lit(im, 128)));
        // Broadcast Colors pulls full white down
        ps = Json::object();
        ps["LumaMax"] = val({90});
        im = rig.render(rig.filtered("solid:255,255,255", kV + "broadcastcolors", ps));
        a = pixel(im, 80, 45);
        check(im && a.r < 250 && a.r > 200, "Broadcast Colors holds white inside the luma limit", pxs(a));
        check(im && std::abs(a.r - 212) <= 1 && std::abs(a.b - 212) <= 1, "... at VEGAS's level for 90 IRE (212)", pxs(a));
        // Film Grain moves the picture's pixels
        im = rig.render(rig.filtered("solid:128,128,128", kV + "filmgrain"));
        int differ = 0;
        for (int x = 0; im && x < 160; x += 2) differ += std::abs(pixel(im, x, 45).r - 128) > 2;
        check(im && differ > 10, "Film Grain adds grain", strfmt("%d differ", differ));
        // Lens Flare adds light
        ps = Json::object();
        ps["LightPosition"] = val({0.3, 0.7}); ps["Intensity"] = val({1});
        im = rig.render(rig.filtered("solid:0,0,0", kV + "lensflare", ps));
        check(im && lit(im, 30) > 20, "Lens Flare draws light", strfmt("%d lit", lit(im, 30)));
        // VEGAS's own presets travel with them
        check(ofx::builtinPresetPackage().find("Weak Fixed Radius") != std::string::npos, "VEGAS's presets for the stand-ins are in the preset package");
    }

    // ---- pictures to look at: every built-in, on a test pattern for the effects (builtins_test <samples> <folder>)
    if (argc > 2) {
        ofx::setTextRasterizer(blockText);
        for (const auto& info : ofx::builtinInfos()) {
            Project p;
            if (info.generator) p = rig.generated(info.id);
            else {
                Json tp = Json::object();
                tp["TestPatternType"] = val({5});
                p = rig.generated(kV + "testpattern", tp);
                p.tracks[0].events[0].fx.push_back(slot(p, info.id));
            }
            ImagePtr img = rig.render(p);
            if (!img) continue;
            std::string name = info.id.substr(info.id.find(':') + 1);
            FILE* f = std::fopen((std::string(argv[2]) + "/" + name + ".ppm").c_str(), "wb");
            if (!f) continue;
            std::fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
            for (int y = 0; y < img->h; ++y)
                for (int x = 0; x < img->w; ++x) {
                    const Px q = pixel(img, x, y);
                    const unsigned char rgb[3] = {(unsigned char)q.r, (unsigned char)q.g, (unsigned char)q.b};
                    std::fwrite(rgb, 1, 3, f);
                }
            std::fclose(f);
        }
    }
    // ---- pictures of the presets of one generator (builtins_test <samples> <folder> <generator id>): <name>.ppm each
    if (argc > 3) {
        ofx::setTextRasterizer(blockText);
        const std::string& xml = ofx::builtinPresetPackage();
        size_t pos = 0;
        const std::string want = "<OfxPreset plugin=\"" + std::string(argv[3]) + "\"";
        while ((pos = xml.find(want, pos)) != std::string::npos) {
            const size_t end = xml.find("</OfxPreset>", pos);
            const std::string one = xml.substr(pos, end - pos);
            pos = end;
            const size_t n0 = one.find("name=\"") + 6;
            const std::string name = one.substr(n0, one.find('"', n0) - n0);
            Json ps = Json::object();
            size_t q = one.find("<OfxPlugin>");
            while ((q = one.find("<OfxParamType", q + 1)) != std::string::npos) {
                const size_t a = one.find("name=\"", q) + 6;
                const std::string pn = one.substr(a, one.find('"', a) - a);
                const size_t v0 = one.find("<OfxParamValue>", a) + 15;
                const std::string v = one.substr(v0, one.find("</OfxParamValue>", v0) - v0);
                if (one.compare(q, 18, "<OfxParamTypeStrin") == 0) { ps[pn] = str(v); continue; }
                std::vector<double> nums;
                size_t at = 0;
                while (at < v.size()) {
                    char* e2 = nullptr;
                    const double d = std::strtod(v.c_str() + at, &e2);
                    if (e2 == v.c_str() + at) {
                        if (v.compare(at, 4, "true") == 0) { nums.push_back(1); at += 4; }
                        else if (v.compare(at, 5, "false") == 0) { nums.push_back(0); at += 5; }
                        else ++at;
                        continue;
                    }
                    nums.push_back(d);
                    at = (size_t)(e2 - v.c_str());
                }
                Json e = Json::object(), arr = Json::array();
                for (double d : nums) arr.push(d);
                e["v"] = arr;
                ps[pn] = e;
            }
            Project p = rig.generated(argv[3], ps);
            ImagePtr img = rig.render(p, 0.5);
            if (!img) continue;
            std::string safe = name;
            for (char& ch : safe) if (ch == ' ' || ch == '(' || ch == ')') ch = '_';
            FILE* f = std::fopen((std::string(argv[2]) + "/" + safe + ".ppm").c_str(), "wb");
            if (!f) continue;
            std::fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
            for (int y = 0; y < img->h; ++y)
                for (int x = 0; x < img->w; ++x) {
                    const Px qq = pixel(img, x, y);
                    const unsigned char rgb[3] = {(unsigned char)qq.r, (unsigned char)qq.g, (unsigned char)qq.b};
                    std::fwrite(rgb, 1, 3, f);
                }
            std::fclose(f);
        }
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
