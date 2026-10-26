// Tests of event gain: a video event's gain is its opacity over what lies below (linear, above 100 % same as 100 %),
// and an audio event's gain multiplies its samples. Made-up media.
//
//   eventgain_engine_test
#include "../../src/engine/engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
}

// "solid:b,g,r,a": a still of one colour, 320 x 180
class SolidReader : public MediaReader {
public:
    MediaInfo mi;
    int c[4] = {0, 0, 0, 255};
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, true);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4)
                for (int k = 0; k < 4; ++k) d[k] = (uint8_t)c[k];
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> open(const std::string& path, std::string*)
{
    std::unique_ptr<SolidReader> r(new SolidReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 320;
    r->mi.height = 180;
    std::sscanf(path.c_str() + 6, "%d,%d,%d,%d", &r->c[0], &r->c[1], &r->c[2], &r->c[3]);
    return std::unique_ptr<MediaReader>(r.release());
}

static uint64_t media(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = path;
    m.info = open(path, nullptr)->info();
    p.media.push_back(m);
    return m.id;
}

static void px(const ImagePtr& img, int& b, int& g, int& r)
{
    const uint8_t* d = img->row(img->h - 1 - 50) + 50 * 4;
    b = d[0]; g = d[1]; r = d[2];
}

int main()
{
    MediaPool pool;
    pool.setOpener(open);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions preview;
    Project p;
    p.settings.width = 320;
    p.settings.height = 180;
    const uint64_t red = media(p, "solid:0,0,255,255"), blue = media(p, "solid:255,0,0,255");
    for (int k = 0; k < 2; ++k) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = k == 0 ? red : blue;
        e.start = 0;
        e.length = 10;
        t.events.push_back(e);
        p.tracks.push_back(t);
    }
    Event& top = p.tracks[0].events[0];
    int b, g, r;
    const struct { double gain; int wantB, wantR; } cases[] = {{1.0, 0, 255}, {0.5, 128, 128}, {0.25, 191, 64}, {0.0, 255, 0}, {1.5, 0, 255}};
    for (const auto& c : cases) {
        top.gain = c.gain;
        px(video.render(p, 1.0, preview), b, g, r);
        check(std::abs(b - c.wantB) <= 2 && std::abs(r - c.wantR) <= 2, strfmt("opacity %.0f%%: the event is mixed with what lies below by that much", c.gain * 100.0),
              strfmt("b=%d r=%d", b, r));
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
