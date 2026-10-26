// Tests of image sequences (src/media/imageseq.cpp, ffmpeg_reader.cpp, MediaItem::sequenceFps) and Media Properties'
// Alpha channel override (MediaItem::alphaMode, applied by MediaPool).
//
//   imageseq_test <scratch folder>
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cmath>
#include <filesystem>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A 64 x 36 picture of one grey level, as a binary PPM.
static void writePicture(const std::string& path, int level)
{
    std::string s = "P6\n64 36\n255\n";
    for (int k = 0; k < 64 * 36; ++k) { s += (char)level; s += (char)level; s += (char)level; }
    writeFile(path, s);
}

// A half-transparent picture as a reader gives it: straight (r, g, b) = (200, 100, 50) at alpha 128, premultiplied.
class HalfClear : public MediaReader {
public:
    MediaInfo mi;
    HalfClear() { mi.hasVideo = true; mi.isStill = true; mi.width = 16; mi.height = 9; }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override
    {
        ImagePtr img = Image::make(16, 9, PixelDepth::Byte, false);
        for (int y = 0; y < 9; ++y)
            for (int x = 0; x < 16; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = 25; d[1] = 50; d[2] = 100; d[3] = 128;       // b, g, r premultiplied by 128/255
            }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static int grey(const ImagePtr& img)
{
    if (!img) return -1;
    return img->row(img->h / 2)[(img->w / 2) * 4 + 1];
}

int main(int argc, char** argv)
{
    const std::string dir = (argc > 1 ? std::string(argv[1]) : std::string("imageseq_out")) + "/seq";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    for (int k = 1; k <= 10; ++k) writePicture(dir + strfmt("/shot_%04d.ppm", k), k * 20);
    for (int k = 1; k <= 12; ++k) writePicture(dir + strfmt("/plain%d.ppm", k), 100);
    writePicture(dir + "/gap_01.ppm", 10);
    writePicture(dir + "/gap_02.ppm", 10);
    writePicture(dir + "/gap_04.ppm", 10);
    writePicture(dir + "/nonumber.ppm", 10);
    writePicture(dir + "/100%_0001.ppm", 50);
    writePicture(dir + "/100%_0002.ppm", 60);

    // ---- the numbering
    check(imageSequenceLength(dir + "/shot_0001.ppm") == 10, "ten numbered pictures in a row", std::to_string(imageSequenceLength(dir + "/shot_0001.ppm")));
    check(imageSequenceLength(dir + "/shot_0004.ppm") == 7, "from the fourth: seven");
    check(imageSequenceLength(dir + "/plain1.ppm") == 12, "numbers without zeros in front (1 ... 12)", std::to_string(imageSequenceLength(dir + "/plain1.ppm")));
    check(imageSequenceLength(dir + "/gap_01.ppm") == 2, "a missing number ends the sequence");
    check(imageSequenceLength(dir + "/nonumber.ppm") == 0 && imageSequenceLength(dir + "/missing_0001.ppm") == 0, "no number, or no file: none");
    std::string pattern;
    int start = 0;
    check(imageSequencePattern(dir + "/shot_0003.ppm", pattern, start) && pattern == dir + "/shot_%04d.ppm" && start == 3, "FFmpeg's pattern and first number", pattern);
    check(imageSequencePattern(dir + "/plain7.ppm", pattern, start) && pattern == dir + "/plain%d.ppm" && start == 7, "an unpadded pattern", pattern);
    check(imageSequencePattern(dir + "/100%_0001.ppm", pattern, start) && pattern == dir + "/100%%_%04d.ppm", "a % in the name is kept as it is", pattern);
    std::string first;
    double fps = 0;
    const std::string spec = imageSequenceSpec(dir + "/shot_0001.ppm", 25);
    check(parseImageSequenceSpec(spec, first, fps) && first == dir + "/shot_0001.ppm" && std::fabs(fps - 25) < 1e-9, "the spec reads back");
    check(!parseImageSequenceSpec(dir + "/shot_0001.ppm", first, fps) && !parseImageSequenceSpec("imgseq:0:x", first, fps) &&
              !parseImageSequenceSpec("imgseq:abc", first, fps),
          "a plain path, a rate of 0, a broken spec: not a sequence");

    // ---- reading it
    std::string err;
    auto r = openMedia(spec, &err);
    check(r && r->info().hasVideo && !r->info().isStill && r->info().width == 64 && r->info().height == 36, "it opens as a video", err);
    if (r) {
        check(std::fabs(r->info().fps - 25) < 0.01 && std::fabs(r->info().duration - 0.4) < 0.045, "at the rate asked: ten pictures at 25 fps last 0.4 s",
              strfmt("%.3f fps, %.3f s", r->info().fps, r->info().duration));
        bool all = true;
        std::string got;
        for (int k = 0; k < 10; ++k) {
            const int g = grey(r->videoFrame((k + 0.5) / 25.0, 64, 36));
            got += strfmt(" %d", g);
            if (std::abs(g - (k + 1) * 20) > 2) all = false;
        }
        check(all, "each picture at its time", got);
        // backwards (a seek back)
        const int g3 = grey(r->videoFrame(3.5 / 25.0, 64, 36));
        check(std::abs(g3 - 80) <= 2, "and after seeking back", std::to_string(g3));
    }
    auto p100 = openMedia(imageSequenceSpec(dir + "/100%_0001.ppm", 10), &err);
    check(p100 && std::abs(grey(p100->videoFrame(0.15, 64, 36)) - 60) <= 2, "a name with % in it", err);
    auto from4 = openMedia(imageSequenceSpec(dir + "/shot_0004.ppm", 25), &err);
    check(from4 && std::abs(grey(from4->videoFrame(0.02, 64, 36)) - 80) <= 2, "starting from a later picture", err);
    auto none = openMedia(imageSequenceSpec(dir + "/nonumber.ppm", 25), &err);
    check(!none && !err.empty(), "a picture with no number: an error", err);

    // ---- in a project
    Project p;
    p.settings.width = 64;
    p.settings.height = 36;
    p.settings.fpsNum = 25;
    MediaItem mi;
    mi.id = p.newId();
    mi.path = dir + "/shot_0001.ppm";
    mi.sequenceFps = 25;
    check(mi.readPath() == spec, "a media item's read path is its spec");
    mi.info = r ? r->info() : MediaInfo();
    p.media.push_back(mi);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = mi.id;
    e.length = 0.4;
    t.events.push_back(e);
    p.tracks.push_back(t);
    MediaPool pool;
    FxRuntime fx;
    VideoEngine ve(pool, fx);
    FrameOptions fo;
    const int a = grey(ve.render(p, 0.5 / 25, fo)), b = grey(ve.render(p, 6.5 / 25, fo));
    check(std::abs(a - 20) <= 2 && std::abs(b - 140) <= 2, "on the timeline it plays picture by picture", strfmt("%d, %d", a, b));
    Project q;
    check(q.fromJson(p.toJson()) && q.media.size() == 1 && q.media[0].sequenceFps == 25 && q.media[0].path == mi.path, "a Reno project keeps it");
    // ---- Media Properties' Alpha channel
    {
        MediaPool ap;
        ap.setOpener([](const std::string&, std::string*) { return std::unique_ptr<MediaReader>(new HalfClear()); });
        MediaItem h;
        h.id = 77;
        h.path = "halfclear";
        h.info = HalfClear().info();
        auto px = [&](const std::string& mode) {
            h.alphaMode = mode;
            ImagePtr img = ap.videoFrame(1, h, 0, 16, 9);
            const uint8_t* d = img ? img->row(4) + 8 * 4 : nullptr;
            return d ? strfmt("%d %d %d %d", d[2], d[1], d[0], d[3]) : std::string("none");
        };
        check(px("") == "100 50 25 128" && px("straight") == "100 50 25 128", "Alpha channel Undefined / Straight: as the reader gives it", px(""));
        check(px("none") == "199 100 50 255", "None: the file's colours (100 / 128 x 255 = 199), opaque", px("none"));
        check(px("premultiplied") == "128 100 50 128", "Premultiplied: the reader's multiplying undone (kept within alpha)", px("premultiplied"));
        h.alphaMode = "none";
        Project r;
        r.media.push_back(h);
        Project r2;
        check(r2.fromJson(r.toJson()) && r2.media[0].alphaMode == "none", "a Reno project keeps it");
        Json bad = r.toJson();
        Project r3;
        std::string txt = bad.dump();
        const size_t at = txt.find("\"none\"", txt.find("alphaMode"));
        if (at != std::string::npos) txt.replace(at, 6, "\"weird\"");
        Json j;
        check(Json::parse(txt, j) && r3.fromJson(j) && r3.media[0].alphaMode.empty(), "an unknown setting is read as Undefined");
    }

    fx.releaseExcept({});
    std::filesystem::remove_all(dir);
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
