// Tests of Media Properties' Field order, Pixel aspect ratio and Rotation overrides (MediaItem::fieldOrderOverride,
// pixelAspectOverride, rotationOverride). Made-up media: four coloured quarters.
//
//   mediaprops_test
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
static const int kTR[3] = {0, 255, 0};          // green
static const int kBL[3] = {255, 0, 0};          // blue
static const int kBR[3] = {255, 255, 255};      // white
static std::string gField = "(never told)";
static int gFieldCalls = 0;

class QuadReader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    void setFieldOrder(const std::string& o) override { gField = o; ++gFieldCalls; }
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
static bool black(const Px& p) { return p.b <= 3 && p.g <= 3 && p.r <= 3; }
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d", p.b, p.g, p.r); }

static Project make(int w, int h, MediaItem** mOut)
{
    Project p;
    p.settings.width = w;
    p.settings.height = h;
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
    t.events.push_back(e);
    p.tracks.push_back(t);
    *mOut = &p.media[0];
    return p;
}

int main()
{
    MediaPool pool;
    pool.setOpener(quadOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;
    MediaItem* m = nullptr;

    // ---- shown size
    {
        MediaItem a;
        a.info.width = 720; a.info.height = 480; a.info.pixelAspect = 0.9091;
        double w = 0, h = 0;
        a.shownSize(w, h);
        check(w == 720 && h == 480, "no override: the size is the file's", strfmt("%g x %g", w, h));
        a.pixelAspectOverride = 1.0;
        a.shownSize(w, h);
        check(std::fabs(w / h - 1.5 * 1.1) < 0.01, "an override 1.0 over 0.9091: wider by the ratio", strfmt("%g x %g", w, h));
        a.pixelAspectOverride = 0;
        a.rotationOverride = 90;
        a.shownSize(w, h);
        check(w == 480 && h == 720, "rotation 90 turns the shape on its side", strfmt("%g x %g", w, h));
        a.rotationOverride = 180;
        a.shownSize(w, h);
        check(w == 720 && h == 480, "rotation 180 keeps the shape");
        a.fieldOrderOverride = "upper";
        check(a.fieldOrder() == "upper", "the field order is the override's");
    }

    // ---- pixel aspect ratio: 400 x 200 media with pixels twice as wide fills a 400 x 100 frame; with square ones it does not
    {
        Project p = make(400, 100, &m);
        ImagePtr img = video.render(p, 0, fo);
        check(black(pixel(img, 20, 50)) && is(pixel(img, 150, 20), kTL), "no override: a 2:1 picture in a 4:1 frame is pillarboxed",
              pxs(pixel(img, 20, 50)) + " / " + pxs(pixel(img, 150, 20)));
        p.media[0].pixelAspectOverride = 2.0;
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTL) && is(pixel(img, 380, 20), kTR) && is(pixel(img, 20, 80), kBL) && is(pixel(img, 380, 80), kBR),
              "pixel aspect ratio 2.0: the picture is twice as wide and fills the frame",
              pxs(pixel(img, 20, 20)) + " / " + pxs(pixel(img, 380, 80)));
    }

    // ---- rotation: clockwise 90 in a 200 x 400 frame
    {
        Project p = make(200, 400, &m);
        p.media[0].rotationOverride = 90;
        ImagePtr img = video.render(p, 0, fo);
        check(is(pixel(img, 150, 100), kTL) && is(pixel(img, 150, 300), kTR) && is(pixel(img, 50, 100), kBL) && is(pixel(img, 50, 300), kBR),
              "rotation 90 clockwise: the top left goes to the top right",
              pxs(pixel(img, 150, 100)) + " / " + pxs(pixel(img, 150, 300)) + " / " + pxs(pixel(img, 50, 100)) + " / " + pxs(pixel(img, 50, 300)));
        p.media[0].rotationOverride = 270;
        img = video.render(p, 0, fo);
        check(is(pixel(img, 50, 300), kTL) && is(pixel(img, 50, 100), kTR) && is(pixel(img, 150, 300), kBL) && is(pixel(img, 150, 100), kBR),
              "rotation 270: the top left goes to the bottom left",
              pxs(pixel(img, 50, 300)) + " / " + pxs(pixel(img, 50, 100)));
    }
    {
        Project p = make(400, 200, &m);
        p.media[0].rotationOverride = 180;
        ImagePtr img = video.render(p, 0, fo);
        check(is(pixel(img, 300, 150), kTL) && is(pixel(img, 100, 150), kTR) && is(pixel(img, 300, 50), kBL) && is(pixel(img, 100, 50), kBR),
              "rotation 180: the top left goes to the bottom right", pxs(pixel(img, 300, 150)));
    }

    // ---- field order reaches the reader
    {
        Project p = make(400, 200, &m);
        gField = "(never told)";
        gFieldCalls = 0;
        p.media[0].fieldOrderOverride = "lower";
        video.render(p, 0, fo);
        check(gField == "lower", "the reader is told the Field order override", gField);
        p.media[0].fieldOrderOverride.clear();
        video.render(p, 0, fo);
        check(gField.empty(), "and told again when it is taken off", gField);
        const int calls = gFieldCalls;
        video.render(p, 0, fo);
        check(gFieldCalls == calls, "but not at every frame");
    }

    // ---- kept in the project file
    {
        Project p = make(400, 200, &m);
        p.media[0].fieldOrderOverride = "none";
        p.media[0].pixelAspectOverride = 1.3333;
        p.media[0].rotationOverride = 270;
        p.media[0].info.pixelAspect = 0.9091;
        p.media[0].info.rotation = 90;
        p.media[0].info.fieldOrder = "lower";
        Project q;
        std::string err;
        const bool ok = q.fromJson(p.toJson(), &err);
        const MediaItem& r = q.media.at(0);
        check(ok && r.fieldOrderOverride == "none" && std::fabs(r.pixelAspectOverride - 1.3333) < 1e-9 && r.rotationOverride == 270 &&
                  std::fabs(r.info.pixelAspect - 0.9091) < 1e-9 && r.info.rotation == 90 && r.info.fieldOrder == "lower",
              "the overrides and what the file says are saved and loaded", err);
        Project plain = make(400, 200, &m);
        Project q2;
        q2.fromJson(plain.toJson(), &err);
        check(q2.media.at(0).fieldOrderOverride.empty() && q2.media.at(0).pixelAspectOverride == 0 && q2.media.at(0).rotationOverride == 0 &&
                  q2.media.at(0).info.pixelAspect == 1.0,
              "a media without overrides stays so");
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
