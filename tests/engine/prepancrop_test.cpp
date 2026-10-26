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
    r->mi.width = 400;
    r->mi.height = 200;
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
    check(is(pixel(img, 60, 58), invTL) && is(pixel(img, 60 + 7, 58 + 7), invTL) && is(pixel(img, 30, 29), kTL) && is(pixel(img, 100, 50), kTL),
          "an effect before pan/crop works on the whole media, and is zoomed in with it",
          pxs(pixel(img, 60, 58)) + " / " + pxs(pixel(img, 67, 65)) + " / " + pxs(pixel(img, 30, 29)));

    // with no pan/crop at all it still works (on the whole media, fitted to the frame)
    p.tracks[0].events[0].panCrop.clear();
    img = video.render(p, 0, fo);
    const int invBL[3] = {255 - kBL[0], 255 - kBL[1], 255 - kBL[2]};
    check(is(pixel(img, 30, 29), invTL) && is(pixel(img, 30, 80), kBL) && !is(pixel(img, 30, 80), invBL),
          "an effect before pan/crop with no pan/crop: the whole picture", pxs(pixel(img, 30, 29)));

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
