// Tests of event effects applied before Event Pan/Crop (FxSlot::beforePanCrop): they see the whole media picture and
// the pan/crop applies to their output; other effects see the panned/cropped picture. Made-up media with four
// coloured quarters, and the test effect that inverts a 9 x 9 pixel square at its "Center".
//
//   prepancrop_test <testfx.ofx>
#include "../../src/engine/engine.h"
#include <cmath>
#include <cstdio>
#include <cstring>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
    std::fflush(stdout);
}

static const int kTL[3] = {0, 0, 255};          // red (B, G, R)
static const int kTR[3] = {0, 255, 0};
static const int kBL[3] = {255, 0, 0};
static const int kBR[3] = {255, 255, 255};

// "quad:" 400 x 200, four solid quarters
class QuadReader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);
            for (int x = 0; x < w; ++x, d += 4) {
                const int* c = (y < h / 2) ? (x < w / 2 ? kTL : kTR) : (x < w / 2 ? kBL : kBR);
                d[0] = (uint8_t)c[0]; d[1] = (uint8_t)c[1]; d[2] = (uint8_t)c[2]; d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> quadOpen(const std::string& path, std::string*)
{
    std::unique_ptr<QuadReader> r(new QuadReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    const bool square = path.rfind("sq:", 0) == 0;          // "sq:" 300 x 300, the same four quarters
    r->mi.width = square ? 300 : 400;
    r->mi.height = square ? 300 : 200;
    r->mi.fps = 30;
    r->mi.duration = 10;
    return std::unique_ptr<MediaReader>(r.release());
}

struct Px { int b, g, r; };
static Px pixel(const ImagePtr& img, int x, int y)          // (from the top left)
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return {-1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2]};
}
static bool is(const Px& p, const int* c, int tol = 3)
{
    return std::abs(p.b - c[0]) <= tol && std::abs(p.g - c[1]) <= tol && std::abs(p.r - c[2]) <= tol;
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d", p.b, p.g, p.r); }

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: prepancrop_test testfx.ofx\n"); return 2; }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    MediaPool pool;
    pool.setOpener(quadOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;

    Project p;
    p.settings.width = 200;
    p.settings.height = 100;
    MediaItem m;
    m.id = p.newId();
    m.path = m.name = "quad:";
    m.info = quadOpen("quad:", nullptr)->info();
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 5;
    // pan/crop: the top left quarter, zoomed in 2x
    PanCropKey k;
    k.cx = 0.25; k.cy = 0.25; k.w = 0.5; k.h = 0.5;
    e.panCrop.push_back(k);
    // the effect inverts a 9 x 9 square at (30, 70) of the frame (OpenFX coordinates: from the bottom left)
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = "reno.test.overlay";
    Json params = Json::object();
    Json c = Json::object();
    Json v = Json::array();
    v.push(30.0);
    v.push(70.0);
    c["v"] = v;
    params["Center"] = c;
    s.state = Json::object();
    s.state["plugin"] = s.pluginId;
    s.state["params"] = params;
    e.fx.push_back(s);
    t.events.push_back(e);
    p.tracks.push_back(t);
    fx.prepare(p);
    const int invTL[3] = {255 - kTL[0], 255 - kTL[1], 255 - kTL[2]};

    // after pan/crop (VEGAS's default): the square is where the effect put it, 9 pixels across
    ImagePtr img = video.render(p, 0, fo);
    check(is(pixel(img, 30, 29), invTL) && is(pixel(img, 30 + 7, 29 + 7), kTL) && is(pixel(img, 100, 50), kTL),
          "an effect after pan/crop works on the panned/cropped picture", pxs(pixel(img, 30, 29)) + " / " + pxs(pixel(img, 37, 36)));

    // before pan/crop: it works on the whole media picture, then the quarter it is in is zoomed in 2x with it
    p.tracks[0].events[0].fx[0].beforePanCrop = true;
    img = video.render(p, 0, fo);
    check(is(pixel(img, 60, 58), invTL) && is(pixel(img, 63, 62), invTL) && is(pixel(img, 30, 29), kTL) && is(pixel(img, 100, 50), kTL),
          "an effect before pan/crop works on the whole media, and is zoomed in with it",
          pxs(pixel(img, 60, 58)) + " / " + pxs(pixel(img, 63, 62)) + " / " + pxs(pixel(img, 30, 29)));

    // with no pan/crop at all it still works (on the whole media, fitted to the frame)
    p.tracks[0].events[0].panCrop.clear();
    img = video.render(p, 0, fo);
    const int invBL[3] = {255 - kBL[0], 255 - kBL[1], 255 - kBL[2]};
    check(is(pixel(img, 30, 29), invTL) && is(pixel(img, 30, 80), kBL) && !is(pixel(img, 30, 80), invBL),
          "an effect before pan/crop with no pan/crop: the whole picture", pxs(pixel(img, 30, 29)));

    // the effect gets the media at its own size, not fitted into the frame: a square 300 x 300 picture in the 200 x 100 frame is
    // 300 x 300 for the effect (it is told the project is that size, so the Center (30, 70) of 200 x 100 becomes (45, 210)
    // in it: 0.15 across and 0.3 down), and the frame shows the effect's picture fitted afterwards (100 x 100 in the middle)
    {
        Project q = p;
        MediaItem sm;
        sm.id = q.newId();
        sm.path = sm.name = "sq:";
        sm.info = quadOpen("sq:", nullptr)->info();
        q.media.push_back(sm);
        q.tracks[0].events[0].mediaId = sm.id;
        q.tracks[0].events[0].panCrop.clear();
        q.tracks[0].events[0].fx.resize(1);
        q.tracks[0].events[0].fx[0].beforePanCrop = true;
        fx.prepare(q);
        ImagePtr r = video.render(q, 0, fo);
        // fitted: x 50..150; the square (9 of 300 across) is at 0.15 across and 0.3 down of it: frame (65, 30)
        check(is(pixel(r, 65, 30), invTL) && is(pixel(r, 72, 30), kTL) && is(pixel(r, 65, 65), kBL) && is(pixel(r, 80, 25), kTL) && is(pixel(r, 120, 25), kTR) && pixel(r, 30, 50).r == 0,
              "a pre-pan/crop effect works on the media at its own size (300 x 300), then it is fitted into the frame", pxs(pixel(r, 65, 30)));
        // and the effect's overlay placement follows: the effect's canonical point (30, 70) (what its Center says) lands at the frame pixel (65, 30) from the top
        double pm[6];
        check(effectPlacement(q, q.tracks[0].events[0].fx[0].id, 0, pm), "the placement of such an effect is known");
        const double fx0 = pm[0] * 30 + pm[1] * 70 + pm[2], fy0 = pm[3] * 30 + pm[4] * 70 + pm[5];
        check(std::fabs(fx0 - 65) < 1.5 && std::fabs(fy0 - (100 - 30)) < 1.5, "and its overlay is placed where the effect draws", strfmt("%.2f %.2f", fx0, fy0));
    }

    // two effects: one before and one after; switching the one before off leaves the one after
    FxSlot s2 = s;
    s2.id = p.newId();
    s2.beforePanCrop = false;
    Json c2 = Json::object();
    Json v2 = Json::array();
    v2.push(150.0);
    v2.push(20.0);
    c2["v"] = v2;
    s2.state["params"]["Center"] = c2;
    p.tracks[0].events[0].fx.push_back(s2);
    p.tracks[0].events[0].panCrop.push_back(k);
    fx.prepare(p);
    img = video.render(p, 0, fo);
    // (the second's square: x 150, y from the top 99 - 20 = 79; on the zoomed top left quarter it is plain red)
    check(is(pixel(img, 60, 58), invTL) && is(pixel(img, 150, 79), invTL) && is(pixel(img, 150 + 7, 79 + 7), kTL),
          "one effect before and one after pan/crop", pxs(pixel(img, 150, 79)));
    p.tracks[0].events[0].fx[0].bypass = true;
    img = video.render(p, 0, fo);
    check(is(pixel(img, 60, 58), kTL) && is(pixel(img, 150, 79), invTL), "the one before switched off: only the one after shows", pxs(pixel(img, 60, 58)));

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
