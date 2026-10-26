// Tests of the compositor's Event Pan/Crop, Track Motion, compositing modes, reversed events and render
// quality, with made-up media whose picture has four differently coloured quarters.
//
//   video_motion_test <testfx.ofx>
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// The quarters of "quad:" media, as B, G, R (all solid).
static const int kTL[3] = {0, 0, 255};          // red
static const int kTR[3] = {0, 255, 0};          // green
static const int kBL[3] = {255, 0, 0};          // blue
static const int kBR[3] = {255, 255, 255};      // white

// Made-up media:
//   "quad:"            400 x 200, 10 s at 30 fps: four solid quarters (above)
//   "frames:"          320 x 180, 10 s at 30 fps: every pixel (blue = frame number, green = 7, red = 200)
//   "solid:b,g,r,a"    320 x 180 still: one premultiplied colour
// Every reader keeps the picture's shape when asked for a size, as real readers do, and remembers the last
// size it was asked for.
static int gLastAskW = 0, gLastAskH = 0;

class TestReader : public MediaReader {
public:
    MediaInfo mi;
    std::string kind;
    int solid[4] = {0, 0, 0, 255};
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        gLastAskW = maxW;
        gLastAskH = maxH;
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        int frame = (int)std::floor(seconds * 30.0 + 1e-6);
        frame = std::max(0, std::min(frame, 299));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);           // (y counted from the top)
            for (int x = 0; x < w; ++x, d += 4) {
                if (kind == "quad") {
                    const int* c = (y < h / 2) ? (x < w / 2 ? kTL : kTR) : (x < w / 2 ? kBL : kBR);
                    d[0] = (uint8_t)c[0]; d[1] = (uint8_t)c[1]; d[2] = (uint8_t)c[2]; d[3] = 255;
                } else if (kind == "frames") {
                    d[0] = (uint8_t)frame; d[1] = 7; d[2] = 200; d[3] = 255;
                } else {
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)solid[k];
                }
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string* error)
{
    std::unique_ptr<TestReader> r(new TestReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    if (path.compare(0, 5, "quad:") == 0) {
        r->kind = "quad";
        r->mi.width = 400; r->mi.height = 200; r->mi.fps = 30; r->mi.duration = 10;
    } else if (path.compare(0, 7, "frames:") == 0) {
        r->kind = "frames";
        r->mi.width = 320; r->mi.height = 180; r->mi.fps = 30; r->mi.duration = 10;
    } else if (path.compare(0, 6, "solid:") == 0) {
        r->kind = "solid";
        r->mi.width = 320; r->mi.height = 180; r->mi.isStill = true;
        std::sscanf(path.c_str() + 6, "%d,%d,%d,%d", &r->solid[0], &r->solid[1], &r->solid[2], &r->solid[3]);
    } else {
        if (error) *error = "unknown test media";
        return nullptr;
    }
    return std::unique_ptr<MediaReader>(r.release());
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = path;
    std::string e;
    m.info = testOpen(path, &e)->info();
    p.media.push_back(m);
    return m.id;
}

static Track& addTrack(Project& p)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    p.tracks.push_back(t);
    return p.tracks.back();
}

static Event& addEvent(Project& p, Track& t, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    t.events.push_back(e);
    t.sortEvents();
    for (auto& x : t.events) if (x.id == e.id) return x;
    return t.events.back();
}

struct Px { int b, g, r, a; };
// (x, y counted from the top left, as on screen)
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool is(const Px& p, const int* c, int tol = 2)
{
    return std::abs(p.b - c[0]) <= tol && std::abs(p.g - c[1]) <= tol && std::abs(p.r - c[2]) <= tol;
}
static bool black(const Px& p) { return p.b <= 1 && p.g <= 1 && p.r <= 1; }
static bool mixed(const Px& p, const int* a, const int* b) { return !is(p, a, 8) && !is(p, b, 8); }

// One straight channel of a mode, as the documented formula gives it (s on top of d, both 0..1).
static double modeChannel(const std::string& m, double s, double d)
{
    if (m == "add") return std::min(1.0, s + d);
    if (m == "subtract") return std::max(0.0, d - s);
    if (m == "multiply") return s * d;
    if (m == "screen") return 1 - (1 - s) * (1 - d);
    if (m == "overlay") return d <= 0.5 ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
    if (m == "hardlight") return s <= 0.5 ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
    if (m == "dodge") return d <= 0 ? 0 : (s >= 1 ? 1 : std::min(1.0, d / (1 - s)));
    if (m == "burn") return d >= 1 ? 1 : (s <= 0 ? 0 : 1 - std::min(1.0, (1 - d) / s));
    if (m == "darken") return std::min(s, d);
    if (m == "lighten") return std::max(s, d);
    if (m == "difference") return std::fabs(s - d);
    if (m == "diffsquared") return (s - d) * (s - d);
    return s;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: video_motion_test testfx.ofx\n"); return 2; }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);

    MediaPool pool;
    pool.setOpener(testOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;

    // ---------------------------------------------------------------- Event Pan/Crop
    {
        Project p;
        p.settings.width = 200;
        p.settings.height = 100;
        const uint64_t q = addMedia(p, "quad:");
        Track& v = addTrack(p);
        Event& e0 = addEvent(p, v, q, 0, 5);
        const uint64_t eid = e0.id;
        auto ev = [&]() -> Event& { return *p.findEvent(eid); };
        auto box = [&](double cx, double cy, double w, double h, double rot) {
            PanCropKey k;
            k.cx = cx; k.cy = cy; k.w = w; k.h = h; k.rotation = rot;
            ev().panCrop.assign(1, k);
        };

        ImagePtr img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTL) && is(pixel(img, 180, 20), kTR) && is(pixel(img, 20, 80), kBL) && is(pixel(img, 180, 80), kBR),
              "without pan/crop the whole picture fills the frame", pxs(pixel(img, 20, 20)));

        box(0.25, 0.25, 0.5, 0.5, 0);
        img = video.render(p, 0, fo);
        check(is(pixel(img, 2, 2), kTL) && is(pixel(img, 100, 50), kTL) && is(pixel(img, 197, 97), kTL),
              "a box round the top left quarter shows just that quarter, filling the frame", pxs(pixel(img, 197, 97)));
        box(0.75, 0.75, 0.5, 0.5, 0);
        img = video.render(p, 0, fo);
        check(is(pixel(img, 2, 2), kBR) && is(pixel(img, 197, 97), kBR), "and one round the bottom right quarter shows that", pxs(pixel(img, 2, 2)));

        box(0.5, 0.5, 0.5, 0.5, 0);            // zoom 2x on the middle
        img = video.render(p, 0, fo);
        check(is(pixel(img, 40, 20), kTL) && is(pixel(img, 160, 20), kTR) && is(pixel(img, 40, 80), kBL) && is(pixel(img, 160, 80), kBR),
              "zooming 2x on the middle keeps the four quarters about the middle", pxs(pixel(img, 160, 20)));
        check(is(pixel(img, 97, 20), kTL) && is(pixel(img, 103, 20), kTR), "and the edge between them stays in the middle", pxs(pixel(img, 97, 20)) + " / " + pxs(pixel(img, 103, 20)));
        check(gLastAskW >= 400 && gLastAskH >= 200, "zoomed in, the media is asked for at its full size to keep the detail", strfmt("%dx%d", gLastAskW, gLastAskH));
        box(0.5, 0.5, 1, 1, 0);
        img = video.render(p, 0, fo);
        check(gLastAskW <= 210 && gLastAskH <= 110, "not zoomed, it is asked for at the frame's size", strfmt("%dx%d", gLastAskW, gLastAskH));

        box(0.5, 0.5, 1, 1, 180);
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kBR) && is(pixel(img, 180, 80), kTL) && is(pixel(img, 180, 20), kBL),
              "a box turned 180 degrees shows the picture upside down", pxs(pixel(img, 20, 20)));
        // a square box turned 90 degrees clockwise: the picture turns the other way (the top right comes to the top left)
        box(0.5, 0.5, 0.5, 1, 90);
        ev().keepAspect = false;
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 15), kTR) && is(pixel(img, 180, 15), kBR) && is(pixel(img, 20, 85), kTL) && is(pixel(img, 180, 85), kBL),
              "a box turned 90 degrees clockwise turns the picture a quarter anticlockwise", pxs(pixel(img, 20, 15)) + " " + pxs(pixel(img, 180, 15)));
        ev().keepAspect = true;

        box(0.5, 0.5, -1, 1, 0);
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTR) && is(pixel(img, 180, 20), kTL), "a box with a negative width flips the picture left to right", pxs(pixel(img, 20, 20)));
        box(0.5, 0.5, 1, -1, 0);
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kBL) && is(pixel(img, 20, 80), kTL), "and one with a negative height flips it upside down", pxs(pixel(img, 20, 20)));

        // a square box (half the media's width, all its height): kept square, with bars either side
        box(0.5, 0.5, 0.5, 1, 0);
        img = video.render(p, 0, fo);
        check(black(pixel(img, 20, 50)) && black(pixel(img, 180, 50)) && is(pixel(img, 60, 25), kTL) && is(pixel(img, 140, 25), kTR) && is(pixel(img, 60, 75), kBL),
              "keeping the aspect, a square box is shown square with bars at the sides", pxs(pixel(img, 20, 50)) + " " + pxs(pixel(img, 60, 25)));
        check(black(pixel(img, 47, 50)) && is(pixel(img, 53, 50), kBL, 4), "and the bars are exactly a quarter of the frame each", pxs(pixel(img, 47, 50)) + " " + pxs(pixel(img, 53, 50)));
        ev().keepAspect = false;
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 25), kTL) && is(pixel(img, 180, 25), kTR), "not keeping it, the box is stretched to fill the frame", pxs(pixel(img, 20, 25)));
        ev().keepAspect = true;

        // a box larger than the picture: zoomed out, with transparent room round it
        box(0.5, 0.5, 2, 2, 0);
        img = video.render(p, 0, fo);
        check(black(pixel(img, 10, 10)) && is(pixel(img, 60, 30), kTL) && is(pixel(img, 140, 70), kBR) && black(pixel(img, 190, 90)),
              "a box twice the picture's size shows it at half size in the middle", pxs(pixel(img, 10, 10)) + " " + pxs(pixel(img, 60, 30)));

        // keyframes: from the top left quarter (at 0 s) to the top right one (at 1 s)
        PanCropKey a, b;
        a.time = 0; a.cx = 0.25; a.cy = 0.25; a.w = 0.5; a.h = 0.5;
        b = a; b.time = 1; b.cx = 0.75;
        ev().panCrop = {b, a};                                  // (in any order)
        img = video.render(p, 0.5, fo);
        check(is(pixel(img, 95, 50), kTL) && is(pixel(img, 105, 50), kTR), "half way between two keys the box is half way (linear)", pxs(pixel(img, 95, 50)) + " " + pxs(pixel(img, 105, 50)));
        // 0.3 s (frame 9) of the way: linear puts the box's centre at 0.4, so the edge is at x = 140
        img = video.render(p, 0.3, fo);
        check(is(pixel(img, 135, 50), kTL) && is(pixel(img, 145, 50), kTR), "at 0.3 s the edge is where linear movement puts it (x = 140)", pxs(pixel(img, 135, 50)) + " " + pxs(pixel(img, 145, 50)));
        ev().panCrop[1].curve = 2;                              // (the key at 0 s, sorted second above) smooth
        img = video.render(p, 0.3, fo);
        // smooth: 0.3 eases to 0.216, so the box's centre is at 0.358 and the edge lands at x = 156.8
        check(is(pixel(img, 145, 50), kTL) && is(pixel(img, 153, 50), kTL) && is(pixel(img, 161, 50), kTR), "a smooth key eases in (the edge lands at 156.8)", pxs(pixel(img, 153, 50)) + " " + pxs(pixel(img, 161, 50)));
        ev().panCrop[1].curve = 1;                              // hold
        img = video.render(p, 0.9, fo);
        check(is(pixel(img, 190, 50), kTL), "a hold key keeps its box until the next key", pxs(pixel(img, 190, 50)));
        img = video.render(p, 1.0, fo);
        check(is(pixel(img, 10, 50), kTR), "and the next key's box shows from its time on", pxs(pixel(img, 10, 50)));
        img = video.render(p, 3.0, fo);
        check(is(pixel(img, 10, 50), kTR), "after the last key its box stays", pxs(pixel(img, 10, 50)));
        // keys turning: 0 degrees at 0 s, 180 at 1 s; half way is 90
        a = PanCropKey(); a.w = 0.5; a.h = 1; a.time = 0;
        b = a; b.time = 1; b.rotation = 180;
        ev().panCrop = {a, b};
        ev().keepAspect = false;
        img = video.render(p, 0.5, fo);
        check(is(pixel(img, 20, 15), kTR) && is(pixel(img, 20, 85), kTL), "turning between keys is interpolated (90 degrees half way)", pxs(pixel(img, 20, 15)));
        ev().keepAspect = true;

        // render quality: zoomed right in on the edge between two quarters, draft shows whole pixels
        box(0.5, 0.25, 0.05, 0.05, 0);
        img = video.render(p, 0, fo);
        bool anyMixed = false;
        for (int x = 80; x < 120; ++x) anyMixed = anyMixed || mixed(pixel(img, x, 50), kTL, kTR);
        check(anyMixed, "good quality blends across the edge of a pixel blown up 10x");
        FrameOptions draft;
        draft.draft = true;
        img = video.render(p, 0, draft);
        anyMixed = false;
        for (int x = 80; x < 120; ++x) anyMixed = anyMixed || mixed(pixel(img, x, 50), kTL, kTR);
        check(!anyMixed && is(pixel(img, 90, 50), kTL) && is(pixel(img, 110, 50), kTR), "a draft preview takes the nearest pixel (cheaper, no blending)");
        p.settings.renderQuality = "draft";
        img = video.render(p, 0, fo);
        anyMixed = false;
        for (int x = 80; x < 120; ++x) anyMixed = anyMixed || mixed(pixel(img, x, 50), kTL, kTR);
        check(!anyMixed, "and so does the project's draft render quality");
        box(0.5, 0.5, 0.6, 0.6, 30);
        p.settings.renderQuality = "good";
        ImagePtr good = video.render(p, 0, fo);
        p.settings.renderQuality = "best";
        ImagePtr best = video.render(p, 0, fo);
        check(is(pixel(best, 40, 20), kTL) && is(pixel(best, 160, 80), kBR) && is(pixel(good, 40, 20), kTL),
              "best quality (several samples a pixel) shows a turned box the same away from the edges", pxs(pixel(best, 40, 20)));
        p.settings.renderQuality = "good";

        // effects on the event see the panned/cropped picture (VEGAS's default order)
        box(0.25, 0.25, 0.5, 0.5, 0);
        FxSlot inv;
        inv.id = p.newId();
        inv.kind = "ofx";
        inv.pluginId = "reno.test.invert";
        ev().fx.push_back(inv);
        fx.prepare(p);
        img = video.render(p, 0, fo);
        const int invTL[3] = {255 - kTL[0], 255 - kTL[1], 255 - kTL[2]};
        check(is(pixel(img, 190, 90), invTL), "event effects work on the panned/cropped picture", pxs(pixel(img, 190, 90)));
        ev().fx.clear();
        fx.releaseExcept({});
    }

    // ---------------------------------------------------------------- Track Motion
    pool.clear();                               // (each project below counts its ids from 1 again)
    {
        Project p;
        p.settings.width = 200;
        p.settings.height = 100;
        const uint64_t q = addMedia(p, "quad:");
        Track& v0 = addTrack(p);
        const uint64_t tid = v0.id;
        addEvent(p, v0, q, 0, 5);
        auto tr = [&]() -> Track& { return *p.findTrack(tid); };
        auto place = [&](double x, double y, double w, double h, double rot) {
            MotionKey k;
            k.x = x; k.y = y; k.w = w; k.h = h; k.rotation = rot;
            tr().motion.assign(1, k);
        };
        place(0.25, 0.25, 0.5, 0.5, 0);
        ImagePtr img = video.render(p, 0, fo);
        check(is(pixel(img, 25, 12), kTL) && is(pixel(img, 75, 12), kTR) && is(pixel(img, 25, 37), kBL) && is(pixel(img, 75, 37), kBR),
              "track motion at half size puts the whole picture in the top left quarter", pxs(pixel(img, 25, 12)));
        check(black(pixel(img, 150, 75)) && black(pixel(img, 103, 25)) && black(pixel(img, 25, 53)), "with nothing round it", pxs(pixel(img, 150, 75)));
        place(0.5, 0.5, 1, 1, 90);              // turned a quarter clockwise: the bottom left comes to the top left
        img = video.render(p, 0, fo);
        check(is(pixel(img, 60, 5), kBL) && is(pixel(img, 140, 5), kTL) && is(pixel(img, 60, 95), kBR) && is(pixel(img, 140, 95), kTR),
              "track motion turned 90 degrees turns the picture a quarter clockwise", pxs(pixel(img, 60, 5)) + " " + pxs(pixel(img, 140, 5)));
        check(black(pixel(img, 30, 50)) && black(pixel(img, 170, 50)), "and leaves the sides empty", pxs(pixel(img, 30, 50)));
        place(0.5, 0.5, 2, 2, 0);               // twice the size: the middle quarter of the picture fills the frame
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTL) && is(pixel(img, 180, 80), kBR), "track motion at twice the size shows the middle of the picture", pxs(pixel(img, 20, 20)));
        MotionKey a, b;
        a.time = 0; a.x = 0.25; a.w = 0.5; a.h = 0.5;
        b = a; b.time = 2; b.x = 0.75;
        tr().motion = {a, b};
        img = video.render(p, 1.0, fo);         // half way: x = 0.5, the picture is 100 wide in the middle
        check(black(pixel(img, 45, 50)) && is(pixel(img, 60, 30), kTL) && is(pixel(img, 140, 30), kTR) && black(pixel(img, 155, 50)),
              "track motion keys are interpolated (half way across at 1 s)", pxs(pixel(img, 45, 50)) + " " + pxs(pixel(img, 60, 30)));
        tr().motion.clear();
        img = video.render(p, 0, fo);
        check(is(pixel(img, 2, 2), kTL) && is(pixel(img, 197, 97), kBR), "without keys the track fills the frame", pxs(pixel(img, 2, 2)));
        // the track's level still applies with motion
        place(0.5, 0.5, 1, 1, 0.0001);
        tr().level = 0.5;
        img = video.render(p, 0, fo);
        check(std::abs(pixel(img, 20, 20).r - 128) <= 3, "the track's opacity applies to its moved picture", pxs(pixel(img, 20, 20)));
    }

    // ---------------------------------------------------------------- compositing modes
    pool.clear();
    {
        Project p;
        p.settings.width = 64;
        p.settings.height = 36;
        const int bottom[4] = {60, 120, 200, 255};
        const int top[4] = {200, 100, 40, 255};
        const uint64_t mb = addMedia(p, strfmt("solid:%d,%d,%d,%d", bottom[0], bottom[1], bottom[2], bottom[3]));
        const uint64_t mt = addMedia(p, strfmt("solid:%d,%d,%d,%d", top[0], top[1], top[2], top[3]));
        // half see-through top: premultiplied (128 alpha)
        const uint64_t mh = addMedia(p, "solid:100,50,20,128");
        Track& tt = addTrack(p);
        const uint64_t topTrack = tt.id;
        addEvent(p, tt, mt, 0, 5);
        Track& tb = addTrack(p);
        addEvent(p, tb, mb, 0, 5);
        const char* modes[] = {"add", "subtract", "multiply", "screen", "overlay", "hardlight", "dodge", "burn", "darken", "lighten", "difference", "diffsquared"};
        for (const char* m : modes) {
            p.findTrack(topTrack)->composite = m;
            const Px x = pixel(video.render(p, 0, fo), 10, 10);
            int want[3];
            for (int c = 0; c < 3; ++c) want[c] = (int)std::floor(modeChannel(m, top[c] / 255.0, bottom[c] / 255.0) * 255.0 + 0.5);
            check(std::abs(x.b - want[0]) <= 1 && std::abs(x.g - want[1]) <= 1 && std::abs(x.r - want[2]) <= 1,
                  strfmt("compositing mode %s gives the right colour", m), pxs(x) + strfmt(" (want b=%d g=%d r=%d)", want[0], want[1], want[2]));
        }
        p.findTrack(topTrack)->composite = "cut";
        Px x = pixel(video.render(p, 0, fo), 10, 10);
        check(black(x), "cut makes a hole where the track is solid", pxs(x));
        p.findTrack(topTrack)->composite = "srcalpha";
        x = pixel(video.render(p, 0, fo), 10, 10);
        check(x.b == top[0] && x.g == top[1] && x.r == top[2], "source alpha shows the solid top track", pxs(x));
        p.findTrack(topTrack)->composite = "something-new";
        x = pixel(video.render(p, 0, fo), 10, 10);
        check(x.b == top[0] && x.r == top[2], "an unknown mode is drawn as source alpha", pxs(x));

        // half see-through top: modes mix with what is below by the top's coverage
        p.findTrack(topTrack)->events[0].mediaId = mh;
        p.findTrack(topTrack)->composite = "add";
        x = pixel(video.render(p, 0, fo), 10, 10);
        {
            const double sa = 128 / 255.0;
            const int s[3] = {100, 50, 20};
            bool ok = true;
            std::string want;
            const int got[3] = {x.b, x.g, x.r};
            for (int c = 0; c < 3; ++c) {
                const double sc = s[c] / 255.0 / sa, dc = bottom[c] / 255.0;
                const double v = dc * (1 - sa) + sa * std::min(1.0, sc + dc);
                const int w = (int)std::floor(v * 255 + 0.5);
                want += strfmt(" %d", w);
                ok = ok && std::abs(got[c] - w) <= 1;
            }
            check(ok, "add with a half see-through track mixes by its coverage", pxs(x) + " want" + want);
        }
        p.findTrack(topTrack)->composite = "cut";
        x = pixel(video.render(p, 0, fo), 10, 10);
        check(std::abs(x.r - (int)std::floor(200 * (1 - 128 / 255.0) + 0.5)) <= 1, "cut with a half see-through track makes the picture below half see-through", pxs(x));
        // the top track's own level counts like its alpha
        p.findTrack(topTrack)->events[0].mediaId = mt;
        p.findTrack(topTrack)->composite = "multiply";
        p.findTrack(topTrack)->level = 0.5;
        x = pixel(video.render(p, 0, fo), 10, 10);
        {
            const double sa = std::floor(0.5 * 255) / 255.0;
            const int want = (int)std::floor((bottom[2] / 255.0 * (1 - sa) + sa * (top[2] / 255.0 * bottom[2] / 255.0)) * 255 + 0.5);
            check(std::abs(x.r - want) <= 2, "a mode with the track at half opacity is half way to the mode's colour", pxs(x) + strfmt(" want r=%d", want));
        }
    }

    // ---------------------------------------------------------------- reversed events
    pool.clear();
    {
        Project p;
        p.settings.width = 64;
        p.settings.height = 36;
        const uint64_t mf = addMedia(p, "frames:");
        Track& v = addTrack(p);
        Event& e = addEvent(p, v, mf, 1.0, 1.0);
        const uint64_t eid = e.id;
        e.reverse = true;
        bool all = true;
        std::string bad;
        for (int k = 0; k < 30; ++k) {
            const Px x = pixel(video.render(p, 1.0 + k / 30.0, fo), 5, 5);
            if (x.b != 29 - k) { all = false; bad += strfmt(" frame %d shows %d;", k, x.b); }
        }
        check(all, "a reversed event shows its media's frames in reverse, frame for frame (29 down to 0)", bad);
        p.findEvent(eid)->rate = 2.0;
        all = true;
        bad.clear();
        for (int k = 0; k < 30; ++k) {
            const Px x = pixel(video.render(p, 1.0 + k / 30.0, fo), 5, 5);
            if (x.b != 58 - 2 * k) { all = false; bad += strfmt(" frame %d shows %d;", k, x.b); }
        }
        check(all, "and at double speed it shows every other frame from 58 down to 0", bad);
        p.findEvent(eid)->rate = 1.0;
        p.findEvent(eid)->offset = 2.0;
        Px x = pixel(video.render(p, 1.0, fo), 5, 5);
        Px y = pixel(video.render(p, 1.0 + 29 / 30.0, fo), 5, 5);
        check(x.b == 89 && y.b == 60, "a reversed event with an offset plays from the end of its stretch back to the offset", pxs(x) + " / " + pxs(y));
        // an effect that looks at the frames either side sees the event's own (reversed) neighbours
        p.findEvent(eid)->offset = 0;
        FxSlot tmp;
        tmp.id = p.newId();
        tmp.kind = "ofx";
        tmp.pluginId = "reno.test.temporal";
        p.findEvent(eid)->fx.push_back(tmp);
        fx.prepare(p);
        check(fx.video(tmp.id) != nullptr, "the frame-averaging effect can be made", fx.error(tmp.id));
        x = pixel(video.render(p, 1.0 + 10 / 30.0, fo), 5, 5);
        check(std::abs(x.b - 19) <= 1, "an effect averaging the frames either side of a reversed frame gets 18, 19, 20", pxs(x));
        p.findEvent(eid)->loop = false;
        x = pixel(video.render(p, 1.0 + 29 / 30.0, fo), 5, 5);
        check(std::abs(x.b - 0) <= 1, "and at the reversed event's last frame (media frame 0) gets 1, 0, 0", pxs(x));
        // thumbnails show the same frame the compositor does
        ImagePtr th = video.eventPicture(p, *p.findEvent(eid), 1.0, 32, 18);
        check(th && pixel(th, 2, 2).b == 29, "a reversed event's thumbnail at its start is its last frame", th ? pxs(pixel(th, 2, 2)) : "none");
        p.findEvent(eid)->fx.clear();
        fx.releaseExcept({});
    }

    // ---------------------------------------------------------------- Track Motion's 2D Shadow and 2D Glow
    {
        Project p;
        p.nextId = 5000;                // (ids of their own: the media pool remembers readers by media id)
        p.settings.width = 200;
        p.settings.height = 100;
        const uint64_t q = addMedia(p, "quad:");
        Track& v = addTrack(p);
        addEvent(p, v, q, 0, 5);
        const uint64_t tid = v.id;
        auto tr = [&]() -> Track& { return *p.findTrack(tid); };
        // the picture at half size in the middle (x 50..150, y 25..75)
        MotionKey m;
        m.w = 0.5;
        m.h = 0.5;
        tr().motion = {m};
        ImagePtr plain = video.render(p, 1.0, fo);
        check(black(pixel(plain, 160, 85)) && is(pixel(plain, 60, 30), kTL), "(the picture moved to the middle at half size)");
        // a hard red shadow, 20 px right and 20 px down, at full intensity
        GlowKey sh;
        sh.x = 0.1;
        sh.y = 0.2;
        sh.blur = 0;
        sh.intensity = 1.0;
        sh.color = 0xFFFF0000;
        tr().shadow2d = {sh};
        ImagePtr img = video.render(p, 1.0, fo);
        const Px under = pixel(img, 160, 85), beside = pixel(img, 40, 85), above = pixel(img, 60, 30);
        check(under.r >= 250 && under.g <= 2 && under.b <= 2 && under.a >= 250, "2D Shadow: the shadow shows beside the picture, offset by its box", pxs(under));
        check(black(beside) && is(above, kTL), "and only there; the picture stays on top of it", pxs(beside) + " / " + pxs(above));
        // blurred, half intensity: soft and fainter
        tr().shadow2d[0].blur = 0.5;
        tr().shadow2d[0].intensity = 0.5;
        img = video.render(p, 1.0, fo);
        const Px soft = pixel(img, 160, 85), edge = pixel(img, 168, 85);
        check(soft.r > 60 && soft.r < 140 && edge.r > 0 && edge.r < soft.r, "a blurred shadow at half intensity is soft and fainter", pxs(soft) + " / " + pxs(edge));
        // keyed: from no offset to the offset
        GlowKey s0 = sh, s1 = sh;
        s0.x = s0.y = 0;
        s1.time = 2.0;
        tr().shadow2d = {s0, s1};
        img = video.render(p, 1.0, fo);
        check(pixel(img, 155, 80).r >= 250 && black(pixel(img, 165, 95)), "the shadow's box moves between its keyframes", pxs(pixel(img, 155, 80)));
        tr().shadow2d.clear();
        // a white glow round it
        GlowKey g;
        g.w = g.h = 1.2;
        g.blur = 0.1;
        g.intensity = 1.0;
        g.color = 0xFFFFFFFF;
        tr().glow2d = {g};
        img = video.render(p, 1.0, fo);
        const Px glow = pixel(img, 46, 50), far = pixel(img, 10, 10);
        check(glow.r > 40 && glow.g > 40 && glow.b > 40 && black(far) && is(pixel(img, 60, 30), kTL), "2D Glow: a glow round the picture's edges, the picture on top",
              pxs(glow) + " / " + pxs(far));
        // in a 32-bit project too
        p.settings.pixelFormat = "32bit";
        ImagePtr f = video.render(p, 1.0, fo);
        if (f && f->depth != PixelDepth::Byte) f = f->converted(PixelDepth::Byte);
        check(f && pixel(f, 46, 50).r > 40 && is(pixel(f, 60, 30), kTL, 3), "and in 32-bit float projects", f ? pxs(pixel(f, 46, 50)) : "none");
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
