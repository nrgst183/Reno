// The preview's quality (FrameOptions::quality, VEGAS's Draft / Preview / Good / Best) in the video engine: it sets how
// carefully frames are resampled in a preview, whatever the project's render quality, and a final render ignores it.
//
//   previewquality_test
#include "../../src/engine/engine.h"
#include "../../src/engine/fxruntime.h"
#include "../../src/media/media.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A 2560 x 1440 still of fine stripes and a gradient, at any size asked for; remembers the largest width asked.
static int gAskedW = 0;
class Stripes : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        const double k = std::min(1.0, std::min((double)maxW / mi.width, (double)maxH / mi.height));
        const int w = std::max(1, (int)std::lround(mi.width * k)), h = std::max(1, (int)std::lround(mi.height * k));
        gAskedW = std::max(gAskedW, w);
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                const double u = (double)x / w, v = (double)y / h;
                const bool stripe = ((int)std::floor(u * 400) + (int)std::floor(v * 230)) % 2 == 0;
                d[0] = (uint8_t)(stripe ? 230 : 20);
                d[1] = (uint8_t)std::lround(255 * u);
                d[2] = (uint8_t)std::lround(255 * v);
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};
static std::unique_ptr<MediaReader> openStripes(const std::string&, std::string*)
{
    std::unique_ptr<Stripes> r(new Stripes());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 2560;
    r->mi.height = 1440;
    return std::unique_ptr<MediaReader>(r.release());
}

static double meanDiff(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b || a->w != b->w || a->h != b->h) return 1e9;
    a->sync();
    b->sync();
    double s = 0;
    for (int y = 0; y < a->h; ++y)
        for (int x = 0; x < a->w * 4; ++x) s += std::abs((int)a->row(y)[x] - (int)b->row(y)[x]);
    return s / (a->w * a->h * 4.0);
}

int main()
{
    MediaPool pool;
    pool.setOpener(openStripes);
    FxRuntime fx;
    VideoEngine video(pool, fx);

    Project p;
    p.settings.width = 640;
    p.settings.height = 360;
    p.settings.renderQuality = "good";
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    MediaItem mi;
    mi.id = p.newId();
    mi.path = "synth:stripes";
    std::string err;
    mi.info = openStripes(mi.path, &err)->info();
    p.media.push_back(mi);
    Event e;
    e.id = p.newId();
    e.mediaId = mi.id;
    e.start = 0;
    e.length = 2;
    {
        // zoomed in on a quarter of the media and turned: the media is resampled (and asked for big)
        PanCropKey k;
        k.w = 0.25;
        k.h = 0.25;
        k.rotation = 10;
        e.panCrop.push_back(k);
    }
    t.events.push_back(e);
    p.tracks.push_back(t);
    fx.prepare(p);

    auto render = [&](bool interactive, int quality, int* asked = nullptr) {
        FrameOptions fo;
        fo.interactive = interactive;
        fo.quality = quality;
        fo.parallel = false;
        gAskedW = 0;
        ImagePtr img = video.render(p, 0.5, fo);
        if (img) img->sync();
        if (asked) *asked = gAskedW;
        return img;
    };

    int askedNone = 0, askedDraft = 0, askedPreview = 0, askedGood = 0, askedBest = 0;
    const ImagePtr none = render(true, -1, &askedNone);
    const ImagePtr draft = render(true, 0, &askedDraft);
    const ImagePtr preview = render(true, 1, &askedPreview);
    const ImagePtr good = render(true, 2, &askedGood);
    const ImagePtr best = render(true, 3, &askedBest);
    check(none && draft && preview && good && best, "every quality makes a frame");
    std::printf("  media width asked: project's %d, draft %d, preview %d, good %d, best %d\n", askedNone, askedDraft, askedPreview, askedGood, askedBest);
    check(meanDiff(none, good) == 0.0, "no quality set: the project's render quality (Good here)", std::to_string(meanDiff(none, good)));
    check(askedPreview < askedGood, "Preview reads the media smaller than Good (the cheaper path)", std::to_string(askedPreview) + " < " + std::to_string(askedGood));
    check(askedDraft <= askedPreview, "Draft reads it no larger than Preview", std::to_string(askedDraft));
    check(meanDiff(draft, good) > 0.5, "Draft samples coarsely (nearest), unlike Good", std::to_string(meanDiff(draft, good)));
    check(meanDiff(preview, good) > 0.0, "Preview is not the same work as Good", std::to_string(meanDiff(preview, good)));
    check(meanDiff(best, good) > 0.0, "Best resamples more carefully than Good", std::to_string(meanDiff(best, good)));

    // a final render takes the project's quality whatever the preview's
    const ImagePtr finalNone = render(false, -1);
    const ImagePtr finalDraft = render(false, 0);
    check(meanDiff(finalNone, finalDraft) == 0.0, "a final render ignores the preview's quality", std::to_string(meanDiff(finalNone, finalDraft)));

    // the project's render quality still applies when the preview sets none
    p.settings.renderQuality = "draft";
    const ImagePtr projDraft = render(true, -1);
    check(meanDiff(projDraft, draft) == 0.0, "no preview quality, project at Draft: the same as the preview's Draft", std::to_string(meanDiff(projDraft, draft)));
    const ImagePtr overGood = render(true, 2);
    check(meanDiff(overGood, good) == 0.0, "the preview's Good wins over a Draft project in a preview", std::to_string(meanDiff(overGood, good)));

    fx.releaseExcept({});
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
