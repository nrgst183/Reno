// Tests of pre-pan/crop event effects (FxSlot::beforePanCrop) in the compositor and of effectPlacement (where an
// effect's own frame lands in the finished one). The test overlay effect marks its "Center" with a 9x9 inverted
// square, which must be found where effectPlacement says.
//
//   placement_test <testfx.ofx>
#include "../../src/engine/engine.h"
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

// "quad:" 400 x 200: red top left, green top right, blue bottom left, white bottom right (B, G, R).
static const int kTL[3] = {0, 0, 255}, kTR[3] = {0, 255, 0}, kBL[3] = {255, 0, 0}, kBR[3] = {255, 255, 255};

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

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string*)
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
// (x, y from the top left)
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return {-1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d", p.b, p.g, p.r); }
static bool is(const Px& p, const int* c, int tol = 3) { return std::abs(p.b - c[0]) <= tol && std::abs(p.g - c[1]) <= tol && std::abs(p.r - c[2]) <= tol; }
static bool isInv(const Px& p, const int* c, int tol = 3) { return std::abs(p.b - (255 - c[0])) <= tol && std::abs(p.g - (255 - c[1])) <= tol && std::abs(p.r - (255 - c[2])) <= tol; }

static void apply(const double m[6], double x, double y, double& ox, double& oy)
{
    ox = m[0] * x + m[1] * y + m[2];
    oy = m[3] * x + m[4] * y + m[5];
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: placement_test testfx.ofx\n"); return 2; }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    MediaPool pool;
    pool.setOpener(testOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;

    // a 200 x 200 frame: the 400 x 200 media fits in it as 200 x 100, with bars above and below
    Project p;
    p.settings.width = 200;
    p.settings.height = 200;
    MediaItem m;
    m.id = p.newId();
    m.path = "quad:";
    m.info = testOpen("quad:", &err)->info();
    p.media.push_back(m);
    Track tr;
    tr.id = p.newId();
    tr.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 10;
    tr.events.push_back(e);
    p.tracks.push_back(tr);
    const uint64_t eid = e.id;
    auto ev = [&]() -> Event& { return *p.findEvent(eid); };

    // ---------------------------------------------------------------- pre-pan/crop effects
    FxSlot inv;
    inv.id = p.newId();
    inv.kind = "ofx";
    inv.pluginId = "reno.test.invert";
    inv.beforePanCrop = true;
    ev().fx.push_back(inv);
    fx.prepare(p);
    ImagePtr img = video.render(p, 1.0, fo);
    check(isInv(pixel(img, 20, 60), kTL) && isInv(pixel(img, 180, 140), kBR) && is(pixel(img, 100, 20), (const int[]){0, 0, 0}),
          "a pre-pan/crop effect works on the media fitted into the frame (inverted; the bars stay black)", pxs(pixel(img, 20, 60)) + " / " + pxs(pixel(img, 100, 20)));
    PanCropKey k;
    k.cx = 0.75;
    k.cy = 0.75;
    k.w = 0.5;
    k.h = 0.5;
    ev().panCrop.assign(1, k);
    img = video.render(p, 1.0, fo);
    check(isInv(pixel(img, 100, 100), kBR) && isInv(pixel(img, 10, 60), kBR) && isInv(pixel(img, 190, 140), kBR),
          "and Pan/Crop then shows a box of what it made (the bottom right quarter, inverted, filling the width)", pxs(pixel(img, 100, 100)));
    ev().fx[0].beforePanCrop = false;
    img = video.render(p, 1.0, fo);
    check(isInv(pixel(img, 100, 100), kBR) && is(pixel(img, 100, 20), (const int[]){0, 0, 0}),
          "the same effect after Pan/Crop gives the same box (its see-through bars stay so)", pxs(pixel(img, 100, 20)));
    FrameOptions skip;
    skip.skipEffects = true;
    ev().fx[0].beforePanCrop = true;
    img = video.render(p, 1.0, skip);
    check(is(pixel(img, 100, 100), kBR), "with every effect bypassed it is left out", pxs(pixel(img, 100, 100)));
    ev().fx.clear();
    ev().panCrop.clear();

    // ---------------------------------------------------------------- effectPlacement against the picture
    FxSlot ov;
    ov.id = p.newId();
    ov.kind = "ofx";
    ov.pluginId = "reno.test.overlay";
    ev().fx.push_back(ov);
    fx.prepare(p);
    ofx::Instance* inst = fx.video(ov.id);
    check(inst != nullptr, "the overlay test effect is made", fx.error(ov.id));
    if (!inst) {
        std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
        return 1;
    }
    ofx::Param* center = inst->params.find("Center");
    auto setCenter = [&](double x, double y) { inst->userSetValue(center, {x, y}, 0); };
    // Where the mark (centred on canonical (cx, cy)) should be: effectPlacement, from the bottom left -> top left.
    auto expectAt = [&](double cx, double cy, double& X, double& Y) {
        double mm[6];
        effectPlacement(p, ov.id, 1.0, mm);
        double bx, by;
        apply(mm, cx + 0.5, cy + 0.5, bx, by);     // (the middle of the mark's centre pixel)
        X = bx;
        Y = p.settings.height - by;
    };
    auto markAt = [&](double X, double Y, const int* under) {
        img = video.render(p, 1.0, fo);
        return isInv(pixel(img, (int)std::floor(X), (int)std::floor(Y)), under);
    };
    double X, Y, mm[6];

    // no motion: where it is
    setCenter(60, 120);
    check(effectPlacement(p, ov.id, 1.0, mm) && mm[0] == 1 && mm[1] == 0 && mm[2] == 0 && mm[3] == 0 && mm[4] == 1 && mm[5] == 0,
          "an event effect with no motion is placed as it is");
    expectAt(60, 120, X, Y);
    check(markAt(X, Y, kTL), "and its mark is there", strfmt("%.1f,%.1f %s", X, Y, pxs(pixel(img, (int)X, (int)Y)).c_str()));

    // Track Motion: the track at half size in the bottom right quarter
    MotionKey mk;
    mk.x = 0.75;
    mk.y = 0.75;
    mk.w = 0.5;
    mk.h = 0.5;
    p.tracks[0].motion.assign(1, mk);
    setCenter(50, 130);                 // (top-down 50,70: in the media's red quarter)
    expectAt(50, 130, X, Y);
    check(std::fabs(X - 125.25) < 0.6 && std::fabs(Y - 134.75) < 0.6, "Track Motion moves and scales an event effect's frame", strfmt("%.2f,%.2f", X, Y));
    check(markAt(X, Y, kTL) && !isInv(pixel(img, (int)X + 8, (int)Y), kTL), "and the mark is drawn there (and only there)",
          pxs(pixel(img, (int)X, (int)Y)) + " / " + pxs(pixel(img, (int)X + 8, (int)Y)));
    // turned 90 degrees
    p.tracks[0].motion[0].rotation = 90;
    expectAt(50, 130, X, Y);
    check(markAt(X, Y, kTL), "turned by Track Motion: the mark follows", strfmt("%.1f,%.1f %s", X, Y, pxs(pixel(img, (int)X, (int)Y)).c_str()));
    p.tracks[0].motion.clear();

    // a compositing parent's Parent Motion moves its child's effects too
    {
        Track parent;
        parent.id = p.newId();
        parent.type = TrackType::Video;
        MotionKey pk;
        pk.x = 0.25;
        pk.y = 0.25;
        pk.w = 0.5;
        pk.h = 0.5;
        parent.parentMotion.push_back(pk);
        p.tracks.insert(p.tracks.begin(), parent);
        p.tracks[1].compositeLevel = 1;
        setCenter(150, 120);            // (in the media's green quarter)
        expectAt(150, 120, X, Y);
        check(std::fabs(X - 75.25) < 0.6 && std::fabs(Y - 39.75) < 0.6, "a compositing parent's Parent Motion moves its child's effect frame", strfmt("%.2f,%.2f", X, Y));
        check(markAt(X, Y, kTR), "and the mark is drawn there", pxs(pixel(img, (int)X, (int)Y)));
        p.tracks.erase(p.tracks.begin());
        p.tracks[0].compositeLevel = 0;
    }

    // a pre-pan/crop effect follows Pan/Crop: a 2x zoom on the middle of the media
    ev().fx[0].beforePanCrop = true;
    k.cx = 0.5;
    k.cy = 0.5;
    k.w = 0.5;
    k.h = 0.5;
    ev().panCrop.assign(1, k);
    // The effect works on the media at its own size and its canonical coordinates are the project's, so the point (120, 110)
    // of the 200 x 200 frame is the media fraction (0.6, 0.45); after the 2x zoom on the middle: (140, 90) (the picture
    // is the 200 x 100 band in the middle of the frame)
    setCenter(120, 110);
    expectAt(120, 110, X, Y);
    check(std::fabs(X - 140.5) < 0.6 && std::fabs(Y - 89.5) < 1.1, "Pan/Crop moves and scales a pre-pan/crop effect's frame", strfmt("%.2f,%.2f", X, Y));
    check(markAt(X, Y, kTR) && markAt(X + 3, Y, kTR) && !markAt(X + 7, Y, kTR), "and the mark is drawn there, at the media's own pixel size (zoomed 1:1 here)", pxs(pixel(img, (int)X, (int)Y)));
    // turned and moved off centre
    k.cx = 0.3;
    k.cy = 0.6;
    k.rotation = 30;
    ev().panCrop.assign(1, k);
    setCenter(70, 85);
    expectAt(70, 85, X, Y);
    img = video.render(p, 1.0, fo);
    const Px at = pixel(img, (int)std::floor(X), (int)std::floor(Y));
    check(at.r >= 0 && !(is(at, kTL) || is(at, kTR) || is(at, kBL) || is(at, kBR)), "a turned box: the mark is still where the placement says", strfmt("%.1f,%.1f %s", X, Y, pxs(at).c_str()));
    // with Track Motion as well
    mk.rotation = 0;
    p.tracks[0].motion.assign(1, mk);
    k.rotation = 0;
    k.cx = 0.5;
    k.cy = 0.5;
    ev().panCrop.assign(1, k);
    setCenter(120, 110);
    expectAt(120, 110, X, Y);
    check(std::fabs(X - 170.5) < 0.8 && std::fabs(Y - 144.75) < 1.1 && markAt(X, Y, kTR), "Pan/Crop and Track Motion together", strfmt("%.2f,%.2f", X, Y));
    p.tracks[0].motion.clear();

    // the video output's effects and unknown slots
    check(!effectPlacement(p, 999999, 0, mm) && mm[0] == 1 && mm[4] == 1, "an unknown slot: false, the identity");
    FxSlot out;
    out.id = p.newId();
    out.kind = "ofx";
    out.pluginId = "reno.test.invert";
    p.videoOut.push_back(out);
    p.tracks[0].motion.assign(1, mk);
    check(effectPlacement(p, out.id, 0, mm) && mm[0] == 1 && mm[2] == 0, "the video output's effects are not moved");
    FxSlot trk;
    trk.id = p.newId();
    trk.kind = "ofx";
    trk.pluginId = "reno.test.invert";
    p.tracks[0].fx.push_back(trk);
    check(effectPlacement(p, trk.id, 0, mm) && std::fabs(mm[0] - 0.5) < 1e-9 && std::fabs(mm[2] - 100) < 1e-9, "a track's effects move with its Track Motion",
          strfmt("%g %g %g / %g %g %g", mm[0], mm[1], mm[2], mm[3], mm[4], mm[5]));

    fx.releaseExcept({});
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
