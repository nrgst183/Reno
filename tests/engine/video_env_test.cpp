// Tests of the compositor's envelopes and video settings: velocity envelopes and undersample rate, composite /
// fade-to-colour / mute envelopes, compositing modes and parents, motion blur, supersampling, interlaced fields,
// and the 32-bit float pipeline. Made-up media.
//
//   video_env_test <testfx.ofx>
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// Made-up media (all 320 x 180):
//   "frames:"          10 s at 30 fps: every pixel is (blue = frame number, green = 7, red = 200)
//   "solid:b,g,r,a"    a still of one premultiplied colour
//   "half:"            a still: the left half solid white, the right half see-through
//   "move:"            10 s at 30 fps: black with a white bar 4 pixels wide whose left edge is at 100 + 300 x seconds
//                      (10 pixels a frame), so sub-frames move it too
class TestReader : public MediaReader {
public:
    MediaInfo mi;
    std::string kind;
    int solid[4] = {0, 0, 0, 255};
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        int frame = (int)std::floor(seconds * 30.0 + 1e-6);
        frame = std::max(0, std::min(frame, 299));
        const double bar = (100.0 + 300.0 * seconds) * w / mi.width;
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, true);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);
            for (int x = 0; x < w; ++x, d += 4) {
                if (kind == "frames") {
                    d[0] = (uint8_t)frame; d[1] = 7; d[2] = 200; d[3] = 255;
                } else if (kind == "half") {
                    if (x < w / 2) d[0] = d[1] = d[2] = d[3] = 255;
                } else if (kind == "move") {
                    const bool on = x >= std::floor(bar) && x < std::floor(bar) + 4;
                    d[0] = d[1] = d[2] = on ? 255 : 0;
                    d[3] = 255;
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
    r->mi.width = 320;
    r->mi.height = 180;
    const size_t colon = path.find(':');
    r->kind = colon == std::string::npos ? path : path.substr(0, colon);
    if (r->kind == "frames" || r->kind == "move") {
        r->mi.fps = 30;
        r->mi.duration = 10;
    } else if (r->kind == "solid" || r->kind == "half") {
        r->mi.isStill = true;
        if (r->kind == "solid") std::sscanf(path.c_str() + 6, "%d,%d,%d,%d", &r->solid[0], &r->solid[1], &r->solid[2], &r->solid[3]);
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
    std::string err;
    std::unique_ptr<MediaReader> r = testOpen(path, &err);
    if (r) m.info = r->info();
    p.media.push_back(m);
    return m.id;
}

static Track& addTrack(Project& p, int level = 0)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    t.compositeLevel = level;
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
    return t.events.back();
}

static Envelope env(const std::string& kind, std::initializer_list<EnvPoint> pts)
{
    Envelope e;
    e.kind = kind;
    e.points = pts;
    return e;
}

static EnvPoint pt(double t, double v, int curve = CurveLinear)
{
    EnvPoint p;
    p.time = t;
    p.value = v;
    p.curve = curve;
    return p;
}

struct Px { int b, g, r, a; };
static Px pixel(const ImagePtr& img, int x, int y)          // y counted from the top
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool near(const Px& p, int b, int g, int r, int tol = 2)
{
    return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol;
}

// The area under an envelope's line from 0 to u, by brute force (for checking the piecewise sums).
static double bruteIntegral(const Envelope& e, double u)
{
    const int n = 200000;
    double sum = 0;
    for (int k = 0; k < n; ++k) {
        const double t = (k + 0.5) * u / n;
        sum += std::min(10.0, std::max(-1.0, e.valueAt(t, 1.0))) * u / n;
    }
    return sum;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: video_env_test <testfx.ofx>\n");
        return 2;
    }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);

    MediaPool pool;
    pool.setOpener(testOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions preview;                   // interactive
    FrameOptions final;
    final.interactive = false;

    // ---------------------------------------------------------------- velocity
    {
        Project p;
        const uint64_t m = addMedia(p, "frames:");
        Track& t = addTrack(p);
        Event& e = addEvent(p, t, m, 1.0, 5.0);
        e.offset = 0.5;
        e.loop = false;
        const MediaItem* mi = p.findMedia(m);
        check(std::fabs(eventMediaTime(e, mi, 2.0) - 1.5) < 1e-9, "without a velocity envelope the media runs at the event's rate");

        e.envelopes = {env("velocity", {pt(0, 2.0)})};
        check(std::fabs(eventMediaTime(e, mi, 2.0) - 2.5) < 1e-9, "a steady velocity of 200% plays the media twice as fast");
        e.rate = 0.5;
        check(std::fabs(eventMediaTime(e, mi, 2.0) - 1.5) < 1e-9, "velocity multiplies the event's own rate");
        e.rate = 1.0;

        // 100% rising to 300% over two seconds: the area under the line
        e.envelopes = {env("velocity", {pt(0, 1.0), pt(2.0, 3.0)})};
        check(std::fabs(eventMediaTime(e, mi, 2.0) - (0.5 + 1.5)) < 1e-9, "a rising velocity: the media moves on by the area under the line (half way)",
              strfmt("%.6f", eventMediaTime(e, mi, 2.0)));
        check(std::fabs(eventMediaTime(e, mi, 3.0) - (0.5 + 4.0)) < 1e-9, "and at the second point", strfmt("%.6f", eventMediaTime(e, mi, 3.0)));
        check(std::fabs(eventMediaTime(e, mi, 4.0) - (0.5 + 7.0)) < 1e-9, "and after the last point it carries on at the last point's speed",
              strfmt("%.6f", eventMediaTime(e, mi, 4.0)));

        // every curve shape against a brute-force sum
        const char* names[] = {"linear", "fast", "slow", "smooth", "sharp", "hold"};
        for (int c = CurveLinear; c <= CurveHold; ++c) {
            Envelope v = env("velocity", {pt(0.3, 0.5, c), pt(1.7, 4.0, CurveSmooth), pt(2.5, -0.5, c), pt(3.4, 1.0)});
            Event ev;
            ev.envelopes = {v};
            double worst = 0;
            for (double u : {0.2, 0.9, 1.7, 2.1, 3.0, 4.5}) worst = std::max(worst, std::fabs(eventVelocityIntegral(ev, u) - bruteIntegral(v, u)));
            check(worst < 1e-4, strfmt("the piecewise sum of a velocity envelope with %s curves matches a brute-force sum", names[c]), strfmt("off by %g", worst));
        }
        {
            Event ev;
            ev.envelopes = {env("velocity", {pt(0, 20.0)})};
            check(std::fabs(eventVelocityIntegral(ev, 1.0) - 10.0) < 1e-9, "velocity is held to VEGAS's range (at most 1000%)");
            ev.envelopes = {env("velocity", {pt(0, -3.0)})};
            check(std::fabs(eventVelocityIntegral(ev, 1.0) + 1.0) < 1e-9, "and at least -100%");
        }

        // negative velocity plays backwards; past the start of the media it holds (not looping) or wraps (looping)
        e.envelopes = {env("velocity", {pt(0, 1.0), pt(1.0, 1.0, CurveHold), pt(1.0001, -1.0)})};
        const double back = eventMediaTime(e, mi, 3.0);
        check(std::fabs(back - (0.5 + 1.0 - 1.0)) < 1e-3, "a negative velocity plays the media backwards", strfmt("%.4f", back));
        check(std::fabs(eventMediaTime(e, mi, 4.0)) < 1e-9, "and a non-looping event holds the first frame when it backs past the start");
        e.loop = true;
        check(std::fabs(eventMediaTime(e, mi, 4.0) - 9.5) < 1e-3, "a looping one wraps round to the end", strfmt("%.4f", eventMediaTime(e, mi, 4.0)));
        e.loop = false;

        // reversed with a velocity envelope: the same stretch of media, played from its far end
        e.envelopes = {env("velocity", {pt(0, 1.0), pt(5.0, 3.0)})};
        const double fwdEnd = eventMediaTime(e, mi, 6.0 - 1e-9);
        e.reverse = true;
        check(std::fabs(eventMediaTime(e, mi, 1.0) - fwdEnd) < 1e-6 && std::fabs(eventMediaTime(e, mi, 6.0) - 0.5) < 1e-9,
              "a reversed event with a velocity envelope plays the same stretch from its end back to its start");
        e.reverse = false;

        // the compositor shows the right frames
        p.settings.width = 320;
        p.settings.height = 180;
        e.offset = 0;
        e.envelopes = {env("velocity", {pt(0, 2.0)})};
        ImagePtr img = video.render(p, 2.0, preview);
        check(pixel(img, 10, 10).b == 60, "the picture follows the velocity envelope (frame 60 one second in at 200%)", pxs(pixel(img, 10, 10)));
        e.envelopes = {env("velocity", {pt(0, -1.0)})};
        e.offset = 5.0;
        img = video.render(p, 2.0, preview);
        check(pixel(img, 10, 10).b == 120, "and backwards at -100% (frame 120 one second in from 5 s)", pxs(pixel(img, 10, 10)));
        e.envelopes.clear();
        e.offset = 0;

        // ------------------------------------------------------------ undersample
        e.undersample = 0.5;
        img = video.render(p, 1.0 + 1.0 / 30, preview);
        check(pixel(img, 10, 10).b == 0, "an undersample rate of 0.5 holds each picture for two frames (frame 1 shows frame 0)", pxs(pixel(img, 10, 10)));
        img = video.render(p, 1.0 + 2.0 / 30, preview);
        check(pixel(img, 10, 10).b == 2, "and the next new picture is frame 2", pxs(pixel(img, 10, 10)));
        e.undersample = 0.25;
        img = video.render(p, 1.0 + 7.0 / 30, preview);
        check(pixel(img, 10, 10).b == 4, "at 0.25 a new picture comes every fourth frame", pxs(pixel(img, 10, 10)));
        e.undersample = 1.0;
        img = video.render(p, 1.0 + 7.0 / 30, preview);
        check(pixel(img, 10, 10).b == 7, "at 1 every frame is new", pxs(pixel(img, 10, 10)));
    }

    // ---------------------------------------------------------------- composite, fade to colour and mute envelopes
    pool.clear();
    {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        const uint64_t red = addMedia(p, "solid:0,0,255,255");
        const uint64_t blue = addMedia(p, "solid:255,0,0,255");
        Track& top = addTrack(p);
        Event& e = addEvent(p, top, red, 0, 10);
        Track& below = addTrack(p);
        addEvent(p, below, blue, 0, 10);
        auto T = [&]() -> Track& { return p.tracks[0]; };
        auto E = [&]() -> Event& { return p.tracks[0].events[0]; };
        (void)e;

        ImagePtr img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 0, 0, 255), "the top track covers the one below", pxs(pixel(img, 50, 50)));

        E().envelopes = {env("composite", {pt(0, 1.0), pt(2.0, 0.0)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 128, 0, 128, 3), "an event's composite envelope at 50% mixes it half with the track below", pxs(pixel(img, 50, 50)));
        img = video.render(p, 2.5, preview);
        check(near(pixel(img, 50, 50), 255, 0, 0), "and at 0% it is gone", pxs(pixel(img, 50, 50)));
        E().envelopes.clear();

        T().envelopes = {env("composite", {pt(0, 0.25)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 191, 0, 64, 3), "a track's composite envelope sets its opacity (25%)", pxs(pixel(img, 50, 50)));
        T().level = 0.5;
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 223, 0, 32, 3), "times the track's own level", pxs(pixel(img, 50, 50)));
        T().level = 1.0;
        T().envelopes.clear();

        T().envelopes = {env("mute", {pt(0, 0), pt(2.0, 1.0)})};
        img = video.render(p, 1.0, preview);
        const Px before = pixel(img, 50, 50);
        img = video.render(p, 3.0, preview);
        check(near(before, 0, 0, 255) && near(pixel(img, 50, 50), 255, 0, 0), "a track's mute envelope mutes it from its point on (always held)",
              pxs(before) + " / " + pxs(pixel(img, 50, 50)));
        T().envelopes.clear();

        T().fadeTop = 0xFF00FF00;           // green
        T().fadeBottom = 0xFF000000;        // black
        T().envelopes = {env("fadecolor", {pt(0, 1.0)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 0, 255, 0), "a track's fade-to-colour envelope at +100% shows the top colour", pxs(pixel(img, 50, 50)));
        T().envelopes = {env("fadecolor", {pt(0, -0.5)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 0, 0, 128, 3), "and at -50% is half way to the bottom colour", pxs(pixel(img, 50, 50)));
        // a see-through part of the track stays see-through
        T().motion.resize(1);
        T().motion[0].w = 0.5;
        T().motion[0].h = 0.5;
        T().envelopes = {env("fadecolor", {pt(0, 1.0)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 160, 90), 0, 255, 0) && near(pixel(img, 10, 10), 255, 0, 0),
              "fading to colour colours only the track's picture, not the see-through part around it",
              pxs(pixel(img, 160, 90)) + " / " + pxs(pixel(img, 10, 10)));
        T().motion.clear();
        T().envelopes.clear();

        // the video output's envelopes
        p.videoFadeTop = 0xFFFFFFFF;
        p.videoFadeBottom = 0xFF000000;
        p.videoEnvelopes = {env("fadecolor", {pt(0, 0.0), pt(2.0, -1.0)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 0, 0, 128, 3), "the video output's fade-to-colour envelope fades the whole picture (half way to black)", pxs(pixel(img, 50, 50)));
        p.videoEnvelopes = {env("fadecolor", {pt(0, 1.0)})};
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 50, 50), 255, 255, 255) && pixel(img, 50, 50).a == 255, "and to the top colour (white), still solid", pxs(pixel(img, 50, 50)));
        p.videoEnvelopes = {env("mute", {pt(0, 1.0)})};
        img = video.render(p, 1.0, preview);
        check(img && near(pixel(img, 50, 50), 0, 0, 0) && pixel(img, 50, 50).a == 255, "the video output's mute envelope makes it black", pxs(pixel(img, 50, 50)));
        p.videoEnvelopes.clear();

        // ------------------------------------------------------------ compositing mode names
        T().level = 0.5;
        T().composite = "srcalpha";
        img = video.render(p, 1.0, preview);
        const Px over = pixel(img, 50, 50);
        for (const char* m : {"3dsrcalpha", "custom"}) {
            T().composite = m;
            img = video.render(p, 1.0, preview);
            check(near(pixel(img, 50, 50), over.b, over.g, over.r, 0), strfmt("%s is drawn as source alpha", m), pxs(pixel(img, 50, 50)));
        }
        T().composite = "diffsquared";
        const Px ds1 = pixel(video.render(p, 1.0, preview), 50, 50);
        T().composite = "differencesquared";
        const Px ds2 = pixel(video.render(p, 1.0, preview), 50, 50);
        check(near(ds1, ds2.b, ds2.g, ds2.r, 0) && !near(ds1, over.b, over.g, over.r, 3), "\"differencesquared\" is Difference Squared", pxs(ds2));
        T().composite = "srcalpha";
        T().level = 1.0;
    }

    // ---------------------------------------------------------------- compositing parents and children
    pool.clear();
    {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        const uint64_t half = addMedia(p, "half:");
        const uint64_t red = addMedia(p, "solid:0,0,255,255");
        const uint64_t blue = addMedia(p, "solid:255,0,0,255");
        const uint64_t green = addMedia(p, "solid:0,255,0,255");
        // track 0: the mask (left half white, right half see-through); 1: red, a child of it; 2: blue, below the group
        addEvent(p, addTrack(p, 0), half, 0, 10);
        addEvent(p, addTrack(p, 1), red, 0, 10);
        addEvent(p, addTrack(p, 0), blue, 0, 10);
        Track* mask = &p.tracks[0];
        mask->composite = "multiply";
        ImagePtr img = video.render(p, 1.0, preview);
        check(near(pixel(img, 60, 90), 0, 0, 255) && near(pixel(img, 260, 90), 255, 0, 0),
              "a Multiply (Mask) parent shows its child where the mask is white and the tracks below where it is see-through",
              pxs(pixel(img, 60, 90)) + " / " + pxs(pixel(img, 260, 90)));
        p.tracks[1].compositeLevel = 0;
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 60, 90), 0, 0, 255) && near(pixel(img, 260, 90), 0, 0, 255),
              "without the parent/child link the mask's multiply leaves the red track alone where the mask is see-through",
              pxs(pixel(img, 260, 90)));
        p.tracks[1].compositeLevel = 1;

        mask->level = 0.5;
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 260, 90), 128, 0, 128, 3), "the mask's level sets how strongly it masks", pxs(pixel(img, 260, 90)));
        mask->level = 1.0;

        mask->parentComposite = "add";
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 60, 90), 255, 0, 255) && near(pixel(img, 260, 90), 255, 0, 0),
              "the parent composite mode lays the finished group over the tracks below (Add)", pxs(pixel(img, 60, 90)));
        mask->parentComposite = "srcalpha";

        // parent motion moves the whole group; the parent's own track motion moves only the mask
        mask->parentMotion.resize(1);
        mask->parentMotion[0].w = 0.5;
        mask->parentMotion[0].h = 0.5;
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 100, 90), 0, 0, 255) && near(pixel(img, 20, 90), 255, 0, 0) && near(pixel(img, 180, 90), 255, 0, 0),
              "parent motion shrinks the whole masked group (red in the left of the middle, blue round it)",
              pxs(pixel(img, 100, 90)) + " / " + pxs(pixel(img, 20, 90)) + " / " + pxs(pixel(img, 180, 90)));
        mask->parentMotion.clear();
        mask->motion.resize(1);
        mask->motion[0].x = 1.0;            // the mask's centre to the right edge: its white half covers the right half
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 60, 90), 255, 0, 0) && near(pixel(img, 260, 90), 0, 0, 255),
              "the parent's own track motion moves the mask, and the child shows where it now is",
              pxs(pixel(img, 60, 90)) + " / " + pxs(pixel(img, 260, 90)));
        mask->motion.clear();

        // a muted parent: the children show unmasked
        mask->mute = true;
        img = video.render(p, 1.0, preview);
        check(near(pixel(img, 260, 90), 0, 0, 255), "muting the parent leaves its children unmasked", pxs(pixel(img, 260, 90)));
        mask->mute = false;

        // the children are composited together first: a green child above the red one, half see-through
        {
            Track g;
            g.id = p.newId();
            g.compositeLevel = 1;
            g.level = 0.5;
            p.tracks.insert(p.tracks.begin() + 1, g);
            addEvent(p, p.tracks[1], green, 0, 10);
            img = video.render(p, 1.0, preview);
            check(near(pixel(img, 60, 90), 0, 128, 128, 3) && near(pixel(img, 260, 90), 255, 0, 0),
                  "two children are composited together, then masked together", pxs(pixel(img, 60, 90)) + " / " + pxs(pixel(img, 260, 90)));
            // a grandchild: level 2 under the green child, which makes the green track a parent of its own
            p.tracks[2].compositeLevel = 2;
            p.tracks[1].composite = "srcalpha";
            img = video.render(p, 1.0, preview);
            check(near(pixel(img, 60, 90), 0, 128, 128, 3) && near(pixel(img, 260, 90), 255, 0, 0),
                  "nested groups (a child that is itself a parent) composite the same here", pxs(pixel(img, 60, 90)));
            p.tracks[1].composite = "multiply";
            img = video.render(p, 1.0, preview);
            // the green child now masks its own child (red): red x green is black where both are solid, and
            // the green track is half see-through, so half the red shows; then the outer mask as before
            check(near(pixel(img, 60, 90), 0, 0, 128, 3) && near(pixel(img, 260, 90), 255, 0, 0),
                  "a nested Multiply (Mask) parent masks only its own children", pxs(pixel(img, 60, 90)));
        }
    }

    // ---------------------------------------------------------------- motion blur and supersampling
    pool.clear();
    {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        const uint64_t mv = addMedia(p, "move:");
        addEvent(p, addTrack(p), mv, 0, 10);
        // at 1 s the bar's left edge is at 400 / 320 x ... (in frame pixels: the media is 320 wide like the frame)
        const double t = 0.5;               // bar at 100 + 150 = 250 .. 253
        ImagePtr sharp = video.render(p, t, final);
        check(pixel(sharp, 251, 90).r == 255 && pixel(sharp, 245, 90).r == 0 && pixel(sharp, 258, 90).r == 0,
              "without motion blur the moving bar is sharp", pxs(pixel(sharp, 245, 90)));
        p.videoEnvelopes = {env("motionblur", {pt(0, 1.0)})};
        ImagePtr blurred = video.render(p, t, final);
        auto row = [](const ImagePtr& img, int x0, int x1) {
            std::string s;
            for (int x = x0; x <= x1; x += 2) s += strfmt("%d ", pixel(img, x, 90).r);
            return s;
        };
        check(pixel(blurred, 245, 90).r > 10 && pixel(blurred, 258, 90).r > 10 && pixel(blurred, 251, 90).r < 250,
              "with the motion blur envelope at 100% it is smeared both ways along its path (Gaussian)", row(blurred, 236, 268));
        check(pixel(blurred, 235, 90).r < 4 && pixel(blurred, 268, 90).r < 4, "over about a frame's movement either side", row(blurred, 230, 272));
        ImagePtr pv = video.render(p, t, preview);
        check(pixel(pv, 245, 90).r == 0, "a normal preview leaves motion blur out (it costs several renders a frame)", row(pv, 236, 268));
        FrameOptions best = preview;
        best.subFrames = true;
        pv = video.render(p, t, best);
        check(pixel(pv, 245, 90).r > 20, "and a preview that asks for it (Best) has it", row(pv, 236, 268));

        p.settings.motionBlur = "asymbox";
        ImagePtr asym = video.render(p, t, final);
        check(pixel(asym, 245, 90).r > 20 && pixel(asym, 258, 90).r < 4, "the asymmetric types leave a trail behind it and a sharp leading edge", row(asym, 236, 268));
        p.settings.motionBlur = "box";
        ImagePtr box = video.render(p, t, final);
        p.settings.motionBlur = "pyramid";
        ImagePtr pyr = video.render(p, t, final);
        p.settings.motionBlur = "gaussian";
        check(pixel(pyr, 251, 90).r > pixel(box, 251, 90).r + 10 && pixel(box, 244, 90).r > pixel(pyr, 244, 90).r + 10,
              "a pyramid weights the frame itself more than a box does, and the far sub-frames less", row(box, 236, 266) + "| " + row(pyr, 236, 266));
        p.videoEnvelopes = {env("motionblur", {pt(0, 0.3)})};
        ImagePtr less = video.render(p, t, final);
        check(pixel(less, 245, 90).r < pixel(blurred, 245, 90).r && pixel(less, 240, 90).r < 4, "a smaller amount blurs over a shorter stretch", row(less, 236, 268));

        p.videoEnvelopes = {env("supersample", {pt(0, 4.0)})};
        ImagePtr ss = video.render(p, t, final);
        check(pixel(ss, 251, 90).r > 40 && pixel(ss, 251, 90).r < 90 && pixel(ss, 258, 90).r > 40 && pixel(ss, 245, 90).r == 0 && pixel(ss, 263, 90).r == 0,
              "supersampling at 4 averages four sub-frames over the frame's own time", row(ss, 244, 266));
        ImagePtr ssPv = video.render(p, t, preview);
        check(pixel(ssPv, 258, 90).r == 0, "and is left out of a normal preview", row(ssPv, 244, 266));
        p.videoEnvelopes.clear();

        // ------------------------------------------------------------ interlaced fields
        p.settings.fieldOrder = "upper";
        ImagePtr fields = video.render(p, t, final);
        // the upper field (lines 0, 2, 4 ... from the top) is at t; the lower one half a frame later (5 pixels on)
        check(pixel(fields, 251, 90).r == 255 && pixel(fields, 256, 90).r == 0 && pixel(fields, 256, 91).r == 255 && pixel(fields, 251, 91).r == 0,
              "an upper-field-first render takes even lines at the frame's time and odd lines half a frame later",
              strfmt("line 90: %s| line 91: %s", row(fields, 248, 260).c_str(), row(fields, 248, 260).c_str()));
        p.settings.fieldOrder = "lower";
        fields = video.render(p, t, final);
        check(pixel(fields, 251, 91).r == 255 && pixel(fields, 256, 90).r == 255 && pixel(fields, 251, 90).r == 0,
              "lower field first: the other way round", pxs(pixel(fields, 251, 91)));
        ImagePtr fp = video.render(p, t, preview);
        check(pixel(fp, 251, 90).r == 255 && pixel(fp, 251, 91).r == 255, "a preview shows whole frames", pxs(pixel(fp, 251, 91)));
        p.settings.fieldOrder = "none";
    }

    // ---------------------------------------------------------------- 32-bit floating point
    pool.clear();
    {
        Project p;
        p.settings.width = 320;
        p.settings.height = 180;
        const uint64_t grey = addMedia(p, "solid:204,204,204,255");          // 0.8
        const uint64_t quad = addMedia(p, "frames:");
        Track& a = addTrack(p);
        addEvent(p, a, grey, 0, 10);
        Track& b = addTrack(p);
        addEvent(p, b, grey, 0, 10);
        p.tracks[0].composite = "add";
        p.videoEnvelopes = {env("fadecolor", {pt(0, -0.5)})};
        p.videoFadeBottom = 0xFF000000;
        ImagePtr eight = video.render(p, 1.0, final);
        p.settings.pixelFormat = "32bit";
        ImagePtr fl = video.render(p, 1.0, final);
        check(eight && fl && eight->depth == PixelDepth::Byte && fl->depth == PixelDepth::Byte, "a 32-bit project's finished frames are still 8-bit BGRA");
        check(std::abs(pixel(eight, 50, 50).r - 128) <= 1 && std::abs(pixel(fl, 50, 50).r - 204) <= 1,
              "in 32-bit nothing is clipped between stages: 0.8 + 0.8 faded half way is 0.8 (8-bit: clipped to 1, then 0.5)",
              pxs(pixel(eight, 50, 50)) + " / " + pxs(pixel(fl, 50, 50)));
        p.videoEnvelopes.clear();
        p.tracks[0].composite = "srcalpha";

        // the same simple pictures come out the same
        p.tracks[1].events.clear();
        p.tracks[0].events[0].mediaId = quad;
        p.tracks[0].level = 0.6;
        p.tracks[0].motion.resize(1);
        p.tracks[0].motion[0].w = 0.7;
        p.tracks[0].motion[0].rotation = 20;
        p.settings.pixelFormat = "8bit";
        eight = video.render(p, 1.0, preview);
        p.settings.pixelFormat = "32bit";
        fl = video.render(p, 1.0, preview);
        int worst = 0;
        for (int y = 0; y < 180; y += 3)
            for (int x = 0; x < 320; x += 3) {
                const Px u = pixel(eight, x, y), v = pixel(fl, x, y);
                worst = std::max(worst, std::max(std::abs(u.b - v.b), std::max(std::abs(u.g - v.g), std::abs(u.r - v.r))));
            }
        check(worst <= 2, "a turned, see-through track looks the same in 32-bit as in 8-bit (to rounding)", strfmt("worst %d", worst));

        // effects get float pictures if they take them, 8-bit if they do not
        FxSlot inv;
        inv.id = p.newId();
        inv.kind = "ofx";
        inv.pluginId = "reno.test.invert";
        FxSlot byteOnly;
        byteOnly.id = p.newId();
        byteOnly.kind = "ofx";
        byteOnly.pluginId = "reno.test.byteonly";
        p.tracks[0].motion.clear();
        p.tracks[0].level = 1.0;
        p.tracks[0].events[0].fx = {inv, byteOnly};
        fx.prepare(p);
        fl = video.render(p, 1.0, preview);
        ofx::Instance* ii = fx.video(inv.id);
        ofx::Instance* bi = fx.video(byteOnly.id);
        check(ii && ii->depth() == PixelDepth::Float, "in a 32-bit project an effect that takes float pictures is given float");
        check(bi && bi->depth() == PixelDepth::Byte, "and one that only takes 8-bit is given 8-bit");
        const Px inverted = pixel(fl, 10, 10);
        p.settings.pixelFormat = "8bit";
        eight = video.render(p, 1.0, preview);
        check(ii && ii->depth() == PixelDepth::Byte, "back in an 8-bit project the effect works in 8-bit again");
        check(near(inverted, pixel(eight, 10, 10).b, pixel(eight, 10, 10).g, pixel(eight, 10, 10).r, 1) && inverted.b != 30,
              "and both give the same picture", pxs(inverted) + " / " + pxs(pixel(eight, 10, 10)));
        p.tracks[0].events[0].fx.clear();
        fx.releaseExcept({});
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
