// vegassnap_test: things found by comparing Reno's pictures with VEGAS's own snapshots (tools/vegcov_render.py), with made-up
// media (no sample data needed).
//
//  1. A picture enlarged to the frame (a still, or a video whose size is known) is scaled the way VEGAS does: the first and
//     last pixel of the frame sit on the first and last pixel of the picture (x * (w - 1) / (frameW - 1)), bilinear.
//  2. The same for Event Pan/Crop that is not turned (a window of the picture enlarged to the frame): the first and last pixel
//     sit on the first and last pixel centre of the window. A turned window keeps the evenly spread pixel centres.
//  3. A video track's Mute envelope holds each point's value, and any value above 0 mutes (VEGAS mutes at a held 0.25).
//  4. A cross-fade made by the later event's fade-in (as long as the overlap) is applied once: half and half in the middle.
//
//   vegassnap_test
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
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

// A made-up picture 4 pixels wide and 2 high (rows alike): the columns' grey levels are 0, 200, 0, 100.
// "still:" is a still image (its size is not known to the project: like a still read from a .veg), anything else a video of
// the same size whose size the project knows.
static const int kLevel[4] = {0, 200, 0, 100};

class ColumnsReader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        // a size of 0 asks for the picture as it is; a smaller box gets it scaled down to fit (here: nearest pixel); a box that is
        // bigger than the picture gets the picture (the engine asks for at least 16 x 16 and does its own enlarging)
        int w = 4, h = 2;
        if (maxW >= 1 && maxH >= 1 && (maxW < 4 || maxH < 2)) {
            const double s = std::min((double)maxW / 4, (double)maxH / 2);
            w = std::max(1, (int)std::lround(4 * s));
            h = std::max(1, (int)std::lround(2 * s));
        }
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);
            for (int x = 0; x < w; ++x, d += 4) {
                const int v = kLevel[std::min(3, x * 4 / w)];
                d[0] = d[1] = d[2] = (uint8_t)v;
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> columnsOpen(const std::string& path, std::string*)
{
    std::unique_ptr<ColumnsReader> r(new ColumnsReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    r->mi.width = 4;
    r->mi.height = 2;
    r->mi.fps = 30;
    r->mi.duration = 10;
    r->mi.isStill = path.rfind("still:", 0) == 0;
    return std::unique_ptr<MediaReader>(r.release());
}

static int grey(const ImagePtr& img, int x, int y)           // (from the top left; -1 outside)
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return -1;
    return img->row(img->h - 1 - y)[x * 4 + 1];
}

static Project makeProject(const std::string& path, bool knownSize, int frameW, int frameH)
{
    Project p;
    p.settings.width = frameW;
    p.settings.height = frameH;
    MediaItem m;
    m.id = p.newId();
    m.path = m.name = path;
    m.info = columnsOpen(path, nullptr)->info();
    if (!knownSize) m.info.width = m.info.height = 0;       // a still in a .veg: only a pan/crop record holds its size
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
    return p;
}

int main()
{
    MediaPool pool;
    pool.setOpener(columnsOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;

    // 1. enlarged to the frame: 4 x 2 into 12 x 6 (3x). Pixel x of the frame shows the picture at x * 3 / 11, between its columns.
    for (int variant = 0; variant < 2; ++variant) {
        const bool still = variant == 1;
        Project p = makeProject(still ? "still:a" : "video:a", !still, 12, 6);
        fx.prepare(p);
        ImagePtr img = video.render(p, 0, fo);
        bool ok = img && img->w == 12 && img->h == 6;
        std::string got;
        for (int x = 0; x < 12; ++x) {
            const double s = x * 3.0 / 11.0;
            const int c = (int)std::floor(s);
            const double f = s - c;
            const double want = kLevel[c] * (1 - f) + kLevel[std::min(3, c + 1)] * f;
            const int g = grey(img, x, 2);
            got += std::to_string(g) + " ";
            if (std::fabs(g - want) > 1.5) ok = false;
        }
        check(ok, still ? "a still enlarged to the frame: first and last pixel on the picture's first and last" : "a video enlarged to the frame: first and last pixel on the picture's first and last", got);
        check(grey(img, 0, 2) == 0 && grey(img, 11, 2) == 100, "the frame's first and last columns are the picture's first and last");
    }

    // 2. Pan/Crop, not turned: the window's middle two columns (200, 0) over 12 pixels: x * 1 / 11 across them (a 2 pixel window)
    {
        Project p = makeProject("video:a", true, 12, 6);
        PanCropKey k;
        k.cx = 0.5; k.cy = 0.5; k.w = 0.5; k.h = 1.0;           // columns 1 and 2, the whole height
        p.tracks[0].events[0].panCrop.push_back(k);
        p.tracks[0].events[0].keepAspect = false;               // stretched to the frame
        fx.prepare(p);
        ImagePtr img = video.render(p, 0, fo);
        bool ok = img != nullptr;
        std::string got;
        for (int x = 0; x < 12; ++x) {
            const double f = x / 11.0;
            const double want = 200 * (1 - f);                  // from column 1 (200) to column 2 (0)
            const int g = grey(img, x, 3);
            got += std::to_string(g) + " ";
            if (std::fabs(g - want) > 2.0) ok = false;
        }
        check(ok, "Pan/Crop zoom: first and last pixel on the window's first and last pixel", got);
    }

    // 3. Mute envelope: held values; above 0 mutes
    {
        Project p = makeProject("video:a", true, 4, 2);
        Envelope env;
        env.kind = "mute";
        EnvPoint a, b;
        a.time = 0; a.value = 0; a.curve = CurveLinear;
        b.time = 2; b.value = 0.25; b.curve = CurveLinear;
        env.points.push_back(a);
        env.points.push_back(b);
        p.tracks[0].envelopes.push_back(env);
        fx.prepare(p);
        ImagePtr on = video.render(p, 1.0, fo);                 // the held 0: shown
        ImagePtr off = video.render(p, 2.5, fo);                // the held 0.25: muted
        check(grey(on, 1, 0) == 200, "mute envelope at a held 0: the track shows", std::to_string(grey(on, 1, 0)));
        check(grey(off, 1, 0) <= 0, "mute envelope at a held 0.25: the track is muted", std::to_string(grey(off, 1, 0)));
    }

    // 4. cross-fade: A 0..4 (level 200 column), B 3..8 with a fade-in of 1 s = the overlap; mid-overlap is 0.5/0.5
    {
        Project p = makeProject("video:a", true, 4, 2);
        Event b = p.tracks[0].events[0];
        Event& a = p.tracks[0].events[0];
        a.start = 0; a.length = 4;
        b.id = p.newId(); b.start = 3; b.length = 5; b.fadeIn = 1.0; b.fadeInCurve = CurveLinear;
        p.tracks[0].events.push_back(b);
        const Event& ev = p.tracks[0].events[1];
        const double w = overlapFade(p.tracks[0], ev, 3.5, true);
        const double wa = overlapFade(p.tracks[0], p.tracks[0].events[0], 3.5, true);
        check(std::fabs(w - 0.5) < 1e-9 && std::fabs(wa - 0.5) < 1e-9, "cross-fade with a fade-in as long as the overlap: 0.5 and 0.5", std::to_string(wa) + " " + std::to_string(w));
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
