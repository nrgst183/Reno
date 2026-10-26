// Tests of frame resampling in the compositor: the event switches Use Project Resample Mode / Force Resample /
// Disable Resample (Event::resample) and Project Properties' Resample mode (smart, force, off). Resampled, a picture
// between two media frames is a blend of the two; Smart does it only for events whose frames cannot line up with
// the project's.
//
//   resample_test
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

// "frames:<fps>": 10 s of 64 x 36 pictures at that frame rate, every pixel blue = 4 x the frame number (mod 256),
// green 7, red 200. "still:" one grey picture.
class TestReader : public MediaReader {
public:
    MediaInfo mi;
    bool still = false;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        int frame = still ? 0 : (int)std::floor(seconds * mi.fps + 1e-6);
        frame = std::max(0, std::min(frame, (int)(mi.fps * 10) - 1));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, true);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                d[0] = still ? 100 : (uint8_t)((frame * 4) & 255);
                d[1] = 7;
                d[2] = 200;
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string*)
{
    std::unique_ptr<TestReader> r(new TestReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    r->mi.width = 64;
    r->mi.height = 36;
    if (path.rfind("still:", 0) == 0) {
        r->still = true;
        r->mi.isStill = true;
    } else {
        r->mi.fps = std::atof(path.c_str() + 7);
        r->mi.duration = 10;
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

// A 64 x 36 project at 'fps' with one event of the media from 0 for 5 seconds.
// (each project's ids are new: the media pool keeps readers by event and media)
static uint64_t gNextId = 1;
static Project oneEvent(const std::string& media, int fps)
{
    Project p;
    p.nextId = gNextId;
    gNextId += 100;
    p.settings.width = 64;
    p.settings.height = 36;
    p.settings.fpsNum = fps;
    p.settings.fpsDen = 1;
    const uint64_t m = addMedia(p, media);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m;
    e.start = 0;
    e.length = 5;
    e.loop = false;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

static Event& ev(Project& p) { return p.tracks[0].events[0]; }

int main()
{
    MediaPool pool;
    pool.setOpener(testOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;
    fo.interactive = false;
    // the blue of the middle of the picture at project frame k
    auto blueAt = [&](const Project& p, int k) {
        ImagePtr img = video.render(p, p.frameTime(k), fo);
        if (!img) return -1;
        return (int)img->row(img->h / 2)[(img->w / 2) * 4];
    };

    // ---- the same frame rate, rate 1: every project frame is a media frame, nothing blended
    {
        Project p = oneEvent("frames:30", 30);
        check(p.settings.resample == "smart" && ev(p).resample == "project", "new projects resample smartly; events follow the project");
        check(blueAt(p, 5) == 20 && blueAt(p, 6) == 24, "30p in 30p: the media's own frames", strfmt("%d %d", blueAt(p, 5), blueAt(p, 6)));
        ev(p).resample = "force";
        check(blueAt(p, 5) == 20, "Force Resample: a time on a media frame is that frame", strfmt("%d", blueAt(p, 5)));
    }
    // ---- half speed: every other project frame falls between two media frames
    {
        Project p = oneEvent("frames:30", 30);
        ev(p).rate = 0.5;
        // project frame 3 is media time 0.05 s = media frame 1.5: half of frame 1 (4) and half of frame 2 (8)
        check(std::abs(blueAt(p, 3) - 6) <= 1, "Smart: a slowed-down event blends the frames either side", strfmt("%d", blueAt(p, 3)));
        check(blueAt(p, 4) == 8, "and shows a media frame as it is where one falls", strfmt("%d", blueAt(p, 4)));
        ev(p).resample = "off";
        check(blueAt(p, 3) == 4, "Disable Resample: the frame showing at that time", strfmt("%d", blueAt(p, 3)));
        ev(p).resample = "project";
        p.settings.resample = "off";
        check(blueAt(p, 3) == 4, "Use Project Resample Mode with the project's Disable: no blending", strfmt("%d", blueAt(p, 3)));
        ev(p).resample = "force";
        check(std::abs(blueAt(p, 3) - 6) <= 1, "the event's Force Resample wins over the project's Disable", strfmt("%d", blueAt(p, 3)));
        p.settings.resample = "force";
        ev(p).resample = "off";
        check(blueAt(p, 3) == 4, "the event's Disable Resample wins over the project's Force", strfmt("%d", blueAt(p, 3)));
    }
    // ---- other frame rates
    {
        Project p = oneEvent("frames:24", 30);
        // project frame 1 = 1/30 s = media frame 0.8: 20% of frame 0 (0) and 80% of frame 1 (4)
        const int b = blueAt(p, 1);
        check(b >= 3 && b <= 4 && b != 0, "Smart: 24p in a 30p project is blended", strfmt("%d", b));
        ev(p).resample = "off";
        check(blueAt(p, 1) == 0, "Disable Resample: 24p in 30p shows the nearest earlier frame", strfmt("%d", blueAt(p, 1)));
    }
    {
        Project p = oneEvent("frames:60", 30);
        check(blueAt(p, 5) == 40, "Smart: 60p in a 30p project lines up (every other frame), nothing blended", strfmt("%d", blueAt(p, 5)));
        ev(p).rate = 0.75;
        // frame 2: 2/30 x 0.75 s = media frame 3.0 exactly; frame 1: 1.5
        check(std::abs(blueAt(p, 1) - 6) <= 1, "60p slowed to 75%: blended", strfmt("%d", blueAt(p, 1)));
    }
    // ---- a velocity envelope makes Smart blend; a still never is
    {
        Project p = oneEvent("frames:30", 30);
        Envelope v;
        v.kind = "velocity";
        EnvPoint a;
        a.time = 0;
        a.value = 0.5;
        v.points.push_back(a);
        ev(p).envelopes.push_back(v);
        check(std::abs(blueAt(p, 3) - 6) <= 1, "Smart: an event with a velocity envelope is blended", strfmt("%d", blueAt(p, 3)));
    }
    {
        Project p = oneEvent("still:", 30);
        ev(p).resample = "force";
        ev(p).rate = 0.37;
        check(blueAt(p, 3) == 100, "a still is never blended", strfmt("%d", blueAt(p, 3)));
    }
    // ---- 32-bit projects blend in float
    {
        Project p = oneEvent("frames:30", 30);
        p.settings.pixelFormat = "32bit";
        ev(p).rate = 0.5;
        ImagePtr img = video.render(p, p.frameTime(3), fo);
        double blue = -1;
        if (img && img->depth == PixelDepth::Float) blue = ((const float*)img->row(img->h / 2))[(img->w / 2) * 4 + 2] * 255.0;  // (RGBA)
        else if (img) blue = img->row(img->h / 2)[(img->w / 2) * 4];
        check(std::fabs(blue - 6.0) < 1.0, "32-bit project: blended too", strfmt("%.3f (%s)", blue, img && img->depth == PixelDepth::Float ? "float" : "8-bit"));
    }
    // ---- the switch is kept in Reno's own project files
    {
        Project p = oneEvent("frames:30", 30);
        ev(p).resample = "force";
        Project q;
        std::string err;
        check(q.fromJson(p.toJson(), &err) && q.tracks.size() == 1 && q.tracks[0].events[0].resample == "force", "Force Resample is saved and read back", err);
        ev(p).resample = "off";
        q = Project();
        check(q.fromJson(p.toJson(), &err) && q.tracks[0].events[0].resample == "off", "Disable Resample is saved and read back");
        ev(p).resample = "project";
        q = Project();
        check(q.fromJson(p.toJson(), &err) && q.tracks[0].events[0].resample == "project", "Use Project Resample Mode is the default");
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
