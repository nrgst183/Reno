// Tests of Red Eye Reduction in the engine (applyRedEye in builtinfx, MediaItem::redEye): red pixels in the areas
// are reduced, everything else is left alone, in 8-bit and float pictures.
//
//   redeye_test
#include "../../src/engine/engine.h"
#include "../../src/engine/builtinfx.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// 100 x 50, skin-coloured, with a red "pupil" (radius 6) at (30, 25) and one at (70, 25) (x, y from the top left)
static ImagePtr face()
{
    ImagePtr img = Image::make(100, 50, PixelDepth::Byte, true);
    for (int y = 0; y < 50; ++y) {
        uint8_t* row = img->row(49 - y);
        for (int x = 0; x < 100; ++x) {
            uint8_t* p = row + x * 4;
            const bool eye = (x - 30) * (x - 30) + (y - 25) * (y - 25) <= 36 || (x - 70) * (x - 70) + (y - 25) * (y - 25) <= 36;
            if (eye) { p[0] = 30; p[1] = 25; p[2] = 210; }       // (BGRA) red
            else { p[0] = 120; p[1] = 150; p[2] = 200; }         // skin: red, but not much more than green
            p[3] = 255;
        }
    }
    return img;
}

static int redAt(const ImagePtr& img, int x, int y) { return img->row(img->h - 1 - y)[x * 4 + 2]; }
static int greenAt(const ImagePtr& img, int x, int y) { return img->row(img->h - 1 - y)[x * 4 + 1]; }

class StillReader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return face(); }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

int main()
{
    ImagePtr img = face();
    // the area round the left eye only (fractions of the picture)
    MediaItem::RedEyeArea a;
    a.x = 0.20; a.y = 0.26; a.w = 0.20; a.h = 0.48;
    ImagePtr out = applyRedEye(img, {a});
    check(out && out != img, "a corrected copy (the picture itself is left alone)");
    check(redAt(img, 30, 25) == 210, "the original keeps its red");
    check(redAt(out, 30, 25) < 40 && greenAt(out, 30, 25) == 25, "the red eye in the area is no longer red", std::to_string(redAt(out, 30, 25)));
    check(redAt(out, 70, 25) == 210, "the other eye (no area) stays red");
    check(redAt(out, 22, 25) == 200 && redAt(out, 31, 15) == 200, "skin in the area is left alone (not red enough)");
    check(applyRedEye(img, {}) == img, "no areas: the same picture");
    MediaItem::RedEyeArea bad;
    bad.w = -1;
    check(applyRedEye(img, {bad}) != nullptr, "a nonsense area does no harm");
    MediaItem::RedEyeArea outside;
    outside.x = 5; outside.y = 5; outside.w = 0.5; outside.h = 0.5;
    check(redAt(applyRedEye(img, {outside}), 30, 25) == 210, "an area off the picture changes nothing");
    // float pictures (RGBA)
    ImagePtr f = img->converted(PixelDepth::Float);
    ImagePtr fo = applyRedEye(f, {a});
    const float* px = (const float*)fo->row(fo->h - 1 - 25) + 30 * 4;
    check(fo && px[0] < 0.2f, "float pictures too", std::to_string(px[0]));

    // through the compositor: an event of a still whose media has an area
    MediaPool pool;
    pool.setOpener([](const std::string& path, std::string*) {
        std::unique_ptr<StillReader> r(new StillReader());
        r->mi.path = path;
        r->mi.hasVideo = true;
        r->mi.isStill = true;
        r->mi.width = 100;
        r->mi.height = 50;
        return std::unique_ptr<MediaReader>(r.release());
    });
    FxRuntime fx;
    VideoEngine video(pool, fx);
    Project p;
    p.settings.width = 100;
    p.settings.height = 50;
    MediaItem m;
    m.id = p.newId();
    m.path = "still";
    m.info.hasVideo = true;
    m.info.isStill = true;
    m.info.width = 100;
    m.info.height = 50;
    m.redEye.push_back(a);
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
    FrameOptions fopt;
    fopt.interactive = false;
    ImagePtr frame = video.render(p, 1.0, fopt);
    check(frame && redAt(frame, 30, 25) < 40 && redAt(frame, 70, 25) > 200, "an event of the picture shows the correction",
          frame ? strfmt("%d %d", redAt(frame, 30, 25), redAt(frame, 70, 25)) : "no picture");
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
