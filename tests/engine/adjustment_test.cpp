// Tests of adjustment tracks (Track::adjustment) in the compositor: effects, level, compositing mode, muting,
// compositing groups, and effects that look at other frames.
//
//   adjustment_test <testfx.ofx>
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

// "solid:b,g,r,a" a still of one premultiplied colour; "frames:" every pixel blue = frame number.
class TestReader : public MediaReader {
public:
    MediaInfo mi;
    bool frames = false;
    int solid[4] = {0, 0, 0, 255};
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        const int frame = std::max(0, std::min(299, (int)std::floor(seconds * 30.0 + 1e-6)));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                if (frames) { d[0] = (uint8_t)frame; d[1] = 7; d[2] = 200; d[3] = 255; }
                else for (int k = 0; k < 4; ++k) d[k] = (uint8_t)solid[k];
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
    r->mi.width = 320;
    r->mi.height = 180;
    if (path.compare(0, 7, "frames:") == 0) {
        r->frames = true;
        r->mi.fps = 30;
        r->mi.duration = 10;
    } else if (path.compare(0, 6, "solid:") == 0) {
        r->mi.isStill = true;
        std::sscanf(path.c_str() + 6, "%d,%d,%d,%d", &r->solid[0], &r->solid[1], &r->solid[2], &r->solid[3]);
    } else {
        if (error) *error = "unknown";
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

static uint64_t addTrack(Project& p, bool adjustment = false)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    t.adjustment = adjustment;
    p.tracks.push_back(t);
    return t.id;
}

static void addEvent(Project& p, uint64_t track, uint64_t media, double start, double length)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    p.findTrack(track)->events.push_back(e);
}

static uint64_t addFx(Project& p, uint64_t track, const char* id)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    p.findTrack(track)->fx.push_back(s);
    return s.id;
}

struct Px { int b, g, r, a; };
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool near(const Px& p, int b, int g, int r, int tol = 2) { return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol; }

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: adjustment_test testfx.ofx\n"); return 2; }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    MediaPool pool;
    pool.setOpener(testOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;

    // tracks top to bottom: [top: a small solid green square? no - a solid red track above], adjustment, blue below
    Project p;
    p.settings.width = 64;
    p.settings.height = 36;
    const uint64_t blue = addMedia(p, "solid:200,40,10,255");      // b, g, r
    const uint64_t red = addMedia(p, "solid:0,0,255,255");
    const uint64_t top = addTrack(p);                                 // red, over part of the time
    const uint64_t adj = addTrack(p, true);
    const uint64_t bottom = addTrack(p);
    addEvent(p, top, red, 5, 2);
    addEvent(p, bottom, blue, 0, 10);
    const uint64_t inv = addFx(p, adj, "reno.test.invert");
    fx.prepare(p);
    check(fx.video(inv) != nullptr, "the invert effect is made on the adjustment track", fx.error(inv));

    ImagePtr img = video.render(p, 1.0, fo);
    check(near(pixel(img, 10, 10), 255 - 200, 255 - 40, 255 - 10), "an adjustment track's effect works on the tracks under it", pxs(pixel(img, 10, 10)));
    img = video.render(p, 5.5, fo);
    check(near(pixel(img, 10, 10), 0, 0, 255), "a track above it is not touched", pxs(pixel(img, 10, 10)));
    p.findTrack(adj)->level = 0.5;
    img = video.render(p, 1.0, fo);
    check(near(pixel(img, 10, 10), 128, 128, 128, 3), "its level mixes the result with the picture under it (half: grey)", pxs(pixel(img, 10, 10)));
    p.findTrack(adj)->level = 1.0;
    p.findTrack(adj)->composite = "multiply";
    img = video.render(p, 1.0, fo);
    check(near(pixel(img, 10, 10), 200 * 55 / 255, 40 * 215 / 255, 10 * 245 / 255, 3), "its compositing mode lays the result over the picture (multiply)", pxs(pixel(img, 10, 10)));
    p.findTrack(adj)->composite = "srcalpha";
    p.findTrack(adj)->mute = true;
    img = video.render(p, 1.0, fo);
    check(near(pixel(img, 10, 10), 200, 40, 10), "muted, it does nothing", pxs(pixel(img, 10, 10)));
    p.findTrack(adj)->mute = false;
    p.findTrack(adj)->fx[0].bypass = true;
    img = video.render(p, 1.0, fo);
    check(near(pixel(img, 10, 10), 200, 40, 10), "with its effect bypassed, it does nothing", pxs(pixel(img, 10, 10)));
    p.findTrack(adj)->fx[0].bypass = false;
    FrameOptions skip;
    skip.skipEffects = true;
    img = video.render(p, 1.0, skip);
    check(near(pixel(img, 10, 10), 200, 40, 10), "with every effect bypassed, it does nothing", pxs(pixel(img, 10, 10)));
    // an event on the adjustment track is not shown
    addEvent(p, adj, red, 0, 3);
    img = video.render(p, 1.0, fo);
    check(near(pixel(img, 10, 10), 255 - 200, 255 - 40, 255 - 10), "events on an adjustment track are not shown", pxs(pixel(img, 10, 10)));
    p.findTrack(adj)->events.clear();
    // nothing under it: nothing to change (transparent stays black)
    {
        Project q = p;
        q.findTrack(bottom)->events.clear();
        img = video.render(q, 1.0, fo);
        const Px px = pixel(img, 10, 10);
        check(near(px, 0, 0, 0, 1) || near(px, 255, 255, 255, 1), "with nothing under it the frame is what the effect makes of nothing", pxs(px));
    }

    // inside a compositing group it works on the group's tracks only
    {
        Project q;
        q.settings = p.settings;
        const uint64_t b2 = addMedia(q, "solid:200,40,10,255");
        const uint64_t g2 = addMedia(q, "solid:0,255,0,255");
        const uint64_t parent = addTrack(q);
        const uint64_t childAdj = addTrack(q, true);
        const uint64_t child = addTrack(q);
        const uint64_t under = addTrack(q);
        q.findTrack(childAdj)->compositeLevel = 1;
        q.findTrack(child)->compositeLevel = 1;
        addEvent(q, child, g2, 0, 10);
        addEvent(q, under, b2, 0, 10);
        addFx(q, childAdj, "reno.test.invert");
        // the child's green covers only the left half: Track Motion squeezes it to the left
        MotionKey mk;
        mk.x = 0.25;
        mk.w = 0.5;
        q.findTrack(child)->motion.push_back(mk);
        (void)parent;
        fx.prepare(q);
        img = video.render(q, 1.0, fo);
        check(near(pixel(img, 8, 18), 255, 0, 255), "in a compositing group: the group's own track is changed (green inverted)", pxs(pixel(img, 8, 18)));
        check(near(pixel(img, 56, 18), 200, 40, 10), "and the track under the group is not", pxs(pixel(img, 56, 18)));
    }

    // an effect looking at the frames either side gets the picture under the track at those frames
    {
        Project q;
        q.settings = p.settings;
        const uint64_t fr = addMedia(q, "frames:");
        const uint64_t a = addTrack(q, true);
        const uint64_t b = addTrack(q);
        addEvent(q, b, fr, 0, 10);
        addFx(q, a, "reno.test.temporal");
        fx.prepare(q);
        img = video.render(q, 1.0, fo);
        check(std::abs(pixel(img, 5, 5).b - 30) <= 1, "a frame-averaging effect on it averages the frames under it (29, 30, 31)", pxs(pixel(img, 5, 5)));
        q.settings.pixelFormat = "32bit";
        img = video.render(q, 1.0, fo);
        check(std::abs(pixel(img, 5, 5).b - 30) <= 1, "and the same in a 32-bit project", pxs(pixel(img, 5, 5)));
    }

    // Project JSON keeps the flag
    {
        Project q;
        Json j = p.toJson();
        check(q.fromJson(j) && q.findTrack(adj) && q.findTrack(adj)->adjustment && !q.findTrack(top)->adjustment, "a saved project keeps which tracks are adjustment tracks");
    }
    fx.releaseExcept({});
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
