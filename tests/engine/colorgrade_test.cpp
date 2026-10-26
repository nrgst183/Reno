// Tests of the built-in colour grade (src/engine/builtinfx.cpp): each control, defaults, 32-bit, bypass, and
// saving and loading the settings.
//
//   colorgrade_test
#include "../../src/engine/engine.h"
#include "../../src/engine/builtinfx.h"
#include <cstdio>
#include <cmath>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}
static std::string rgb(const float c[3]) { return strfmt("%.4f %.4f %.4f", c[0], c[1], c[2]); }
static bool near3(const float c[3], double r, double g, double b, double eps = 1e-3) { return std::fabs(c[0] - r) < eps && std::fabs(c[1] - g) < eps && std::fabs(c[2] - b) < eps; }

// A solid still: "solid:b,g,r"
class Solid : public MediaReader {
public:
    MediaInfo mi;
    int c[3] = {0, 0, 0};
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        ImagePtr img = Image::make(std::min(maxW, 64), std::min(maxH, 36), PixelDepth::Byte, false);
        for (int y = 0; y < img->h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < img->w; ++x, d += 4) { d[0] = (uint8_t)c[0]; d[1] = (uint8_t)c[1]; d[2] = (uint8_t)c[2]; d[3] = 255; }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};
static std::unique_ptr<MediaReader> open(const std::string& path, std::string*)
{
    std::unique_ptr<Solid> r(new Solid());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 64;
    r->mi.height = 36;
    std::sscanf(path.c_str(), "solid:%d,%d,%d", &r->c[0], &r->c[1], &r->c[2]);
    return std::unique_ptr<MediaReader>(r.release());
}

int main()
{
    // ---- the colour maths
    {
        ColorGrade g;
        check(g.isIdentity(), "a new grade changes nothing");
        float c[3] = {0.2f, 0.5f, 0.8f};
        g.apply(c);
        check(near3(c, 0.2, 0.5, 0.8), "and leaves a colour as it was", rgb(c));
        g.exposure = 1;
        float e[3] = {0.1f, 0.2f, 0.3f};
        g.apply(e);
        check(near3(e, 0.2, 0.4, 0.6), "exposure +1 stop doubles", rgb(e));
        g = ColorGrade();
        g.saturation = 0;
        float s[3] = {1.0f, 0.0f, 0.0f};
        g.apply(s);
        check(near3(s, 0.2126, 0.2126, 0.2126), "saturation 0: grey of the same luma (Rec. 709)", rgb(s));
        g = ColorGrade();
        g.contrast = 2;
        float k[3] = {0.25f, 0.5f, 0.75f};
        g.apply(k);
        check(near3(k, 0.0, 0.5, 1.0), "contrast 2 stretches about mid grey", rgb(k));
        g = ColorGrade();
        g.temperature = 100;
        float t[3] = {0.5f, 0.5f, 0.5f};
        g.apply(t);
        check(t[0] > 0.6f && t[2] < 0.4f && std::fabs(t[1] - 0.5f) < 1e-4, "warmer: more red, less blue", rgb(t));
        g = ColorGrade();
        g.tint = 100;
        float m[3] = {0.5f, 0.5f, 0.5f};
        g.apply(m);
        check(m[1] < 0.4f && std::fabs(m[0] - 0.5f) < 1e-4, "tint towards magenta: less green", rgb(m));
        // wheels: gain pushed towards red (top of the wheel) brightens red and dims the others
        g = ColorGrade();
        g.gain.y = 1;
        float w[3] = {0.5f, 0.5f, 0.5f};
        g.apply(w);
        check(w[0] > 0.6f && w[1] < 0.5f && w[2] < 0.5f, "the gain wheel towards red", rgb(w));
        g = ColorGrade();
        g.lift.master = 0.2;
        float l0[3] = {0, 0, 0}, l1[3] = {1, 1, 1};
        g.apply(l0);
        g.apply(l1);
        check(near3(l0, 0.2, 0.2, 0.2) && near3(l1, 1, 1, 1), "lift raises black and leaves white", rgb(l0) + " / " + rgb(l1));
        g = ColorGrade();
        g.gamma.master = 2;
        float gm[3] = {0.25f, 0, 1};
        g.apply(gm);
        check(near3(gm, 0.5, 0, 1), "gamma 2 lifts the middle (0.25 -> 0.5), not the ends", rgb(gm));
        g = ColorGrade();
        g.offset.master = -0.1;
        float of[3] = {0.5f, 0.5f, 0.5f};
        g.apply(of);
        check(near3(of, 0.4, 0.4, 0.4), "offset shifts everything", rgb(of));
        // curves
        g = ColorGrade();
        g.curve[0] = {{0, 1}, {1, 0}};
        g.prepare();
        float cv[3] = {0.25f, 0.5f, 1.0f};
        g.apply(cv);
        check(near3(cv, 0.75, 0.5, 0.0), "a master curve from top left to bottom right inverts", rgb(cv));
        g = ColorGrade();
        g.curve[3] = {{0, 0}, {0.5, 0.25}, {1, 1}};
        g.prepare();
        float cb[3] = {0.5f, 0.5f, 0.5f};
        g.apply(cb);
        check(near3(cb, 0.5, 0.5, 0.25, 2e-3), "the blue curve changes blue only, through its points", rgb(cb));
        // the curve does not overshoot between its points
        std::vector<std::pair<double, double>> pts = {{0, 0}, {0.4, 0.5}, {0.6, 0.5}, {1, 1}};
        bool mono = true;
        double prev = -1;
        for (int i = 0; i <= 100; ++i) {
            const double v = curveThrough(pts, i / 100.0);
            if (v < prev - 1e-12) mono = false;
            prev = v;
        }
        check(mono && std::fabs(curveThrough(pts, 0.5) - 0.5) < 1e-9, "curves keep to their points without overshooting");
        // JSON
        g = ColorGrade();
        g.exposure = 0.5;
        g.lift = {0.3, -0.2, 0.1};
        g.curve[2] = {{0, 0.1}, {1, 0.9}};
        const ColorGrade back = ColorGrade::fromJson(g.toJson());
        check(back.exposure == 0.5 && back.lift.x == 0.3 && back.lift.y == -0.2 && back.lift.master == 0.1 && back.curve[2].size() == 2 && back.curve[2][1].second == 0.9,
              "its settings come back from JSON");
        Json bad = Json::object();
        bad["exposure"] = std::nan("");
        bad["contrast"] = 1e9;
        const ColorGrade b2 = ColorGrade::fromJson(bad);
        check(b2.exposure == 0 && b2.contrast == 4, "nonsense settings are kept within reason");
    }

    // ---- in the compositor
    MediaPool pool;
    pool.setOpener(open);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;
    Project p;
    p.settings.width = 64;
    p.settings.height = 36;
    MediaItem mi;
    mi.id = p.newId();
    mi.path = "solid:64,128,192";      // b, g, r
    std::string err;
    mi.info = open(mi.path, &err)->info();
    p.media.push_back(mi);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = mi.id;
    e.length = 5;
    FxSlot cg = makeBuiltinSlot(kColorGradeId, p.newId());
    check(cg.kind == "builtin" && cg.name == "Color Grading" && isVideoFxSlot(cg), "a built-in Color Grading slot");
    ColorGrade g;
    g.saturation = 0;
    cg.state = g.toJson();
    e.fx.push_back(cg);
    t.events.push_back(e);
    p.tracks.push_back(t);
    fx.prepare(p);
    ImagePtr img = video.render(p, 1.0, fo);
    const uint8_t* px = img->row(10) + 40;
    const int luma = (int)std::lround(0.2126 * 192 + 0.7152 * 128 + 0.0722 * 64);
    check(std::abs(px[0] - luma) <= 1 && std::abs(px[1] - luma) <= 1 && std::abs(px[2] - luma) <= 1, "on an event it greys the picture (no plug-in needed)",
          strfmt("%d %d %d (luma %d)", px[0], px[1], px[2], luma));
    p.tracks[0].events[0].fx[0].bypass = true;
    img = video.render(p, 1.0, fo);
    px = img->row(10) + 40;
    check(px[0] == 64 && px[1] == 128 && px[2] == 192, "bypassed, it changes nothing");
    p.tracks[0].events[0].fx[0].bypass = false;
    FrameOptions skip;
    skip.skipEffects = true;
    img = video.render(p, 1.0, skip);
    px = img->row(10) + 40;
    check(px[0] == 64, "nor with every effect bypassed");
    // on the video output, in a 32-bit project: brighter than white stays so between effects
    {
        Project q = p;
        q.tracks[0].events[0].fx.clear();
        q.settings.pixelFormat = "32bit";
        ColorGrade up;
        up.exposure = 2;           // x4
        FxSlot a = makeBuiltinSlot(kColorGradeId, q.newId());
        a.state = up.toJson();
        ColorGrade down;
        down.exposure = -2;        // x1/4
        FxSlot b = makeBuiltinSlot(kColorGradeId, q.newId());
        b.state = down.toJson();
        q.videoOut.push_back(a);
        q.videoOut.push_back(b);
        img = video.render(q, 1.0, fo);
        px = img->row(10) + 40;
        check(std::abs(px[0] - 64) <= 1 && std::abs(px[1] - 128) <= 1 && std::abs(px[2] - 192) <= 1, "32-bit: four times brighter then a quarter is the picture again (nothing clipped)",
              strfmt("%d %d %d", px[0], px[1], px[2]));
        q.settings.pixelFormat = "8bit";
        img = video.render(q, 1.0, fo);
        px = img->row(10) + 40;
        check(px[2] == 64 && px[1] == 64, "8-bit: clipped at white in between (as an 8-bit project is)", strfmt("%d %d %d", px[0], px[1], px[2]));
    }
    // a track's and an adjustment track's
    {
        Project q = p;
        q.tracks[0].events[0].fx.clear();
        FxSlot a = makeBuiltinSlot(kColorGradeId, q.newId());
        ColorGrade inv;
        inv.curve[0] = {{0, 1}, {1, 0}};
        a.state = inv.toJson();
        q.tracks[0].fx.push_back(a);
        img = video.render(q, 1.0, fo);
        px = img->row(10) + 40;
        check(std::abs(px[0] - 191) <= 1 && std::abs(px[2] - 63) <= 1, "on a track", strfmt("%d %d %d", px[0], px[1], px[2]));
    }
    // saved and loaded
    {
        Json j = p.toJson();
        Project q;
        check(q.fromJson(j) && q.tracks[0].events[0].fx.size() == 1 && q.tracks[0].events[0].fx[0].kind == "builtin" &&
                  q.tracks[0].events[0].fx[0].pluginId == kColorGradeId && ColorGrade::fromJson(q.tracks[0].events[0].fx[0].state).saturation == 0,
              "a project keeps its built-in effects and their settings");
    }
    fx.releaseExcept({});
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
