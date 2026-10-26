// The compositor's fast paths must give exactly the same pictures as the plain path. Random projects are rendered with
// the fast paths on and off (setComposeFastPaths) and compared byte for byte.
//
//   compose_fast_test [count]
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <random>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// Made-up media "noise:W,H,seed,fps[,still]": a premultiplied picture of random colour and alpha (some solid parts, some see-through).
class NoiseReader : public MediaReader {
public:
    MediaInfo mi;
    int seed = 1;
    bool still = false;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min(1.0, std::min((double)maxW / mi.width, (double)maxH / mi.height));
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        const int frame = still ? 0 : (int)std::floor(seconds * mi.fps + 1e-6);
        std::mt19937 rng((unsigned)(seed * 7919 + frame));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                const unsigned r = rng();
                unsigned a = (r >> 24) & 255;
                if ((y / 8 + x / 8) % 3 == 0) a = 255;                      // (patches of solid)
                if ((y / 8 + x / 8) % 7 == 3) a = 0;                        // (and of nothing)
                for (int k = 0; k < 3; ++k) d[k] = (uint8_t)((((r >> (k * 8)) & 255) * a + 127) / 255);
                d[3] = (uint8_t)a;
            }
        }
        return img;
    }
    // (like a real reader: the stretch of times a frame is good for, so that the pool shares it)
    ImagePtr videoFrameSpan(double seconds, int maxW, int maxH, double* from, double* until) override
    {
        if (still) { *from = 0; *until = 1e9; }
        else {
            const double f = std::floor(seconds * mi.fps + 1e-6);
            *from = f / mi.fps;
            *until = (f + 1) / mi.fps;
        }
        return videoFrame(seconds, maxW, maxH);
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string* error)
{
    int w = 0, h = 0, seed = 0, fps = 30;
    if (path.compare(0, 6, "noise:") != 0 || std::sscanf(path.c_str() + 6, "%d,%d,%d,%d", &w, &h, &seed, &fps) != 4) {
        if (error) *error = "unknown test media";
        return nullptr;
    }
    std::unique_ptr<NoiseReader> r(new NoiseReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    r->mi.width = w; r->mi.height = h; r->mi.fps = fps; r->mi.duration = 10;
    r->still = path.find(",still") != std::string::npos;
    r->mi.isStill = r->still;
    r->seed = seed;
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

static uint64_t hashImage(const ImagePtr& img)
{
    if (!img) return 0;
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < img->h; ++y) {
        const uint8_t* r = img->row(y);
        for (size_t i = 0; i < (size_t)img->w * img->pixelBytes(); ++i) { h ^= r[i]; h *= 1099511628211ull; }
    }
    return h ^ ((uint64_t)img->w << 32) ^ (uint64_t)img->h;
}

// fitImage makes its frame without clearing it first and clears only the bars: the bars must be empty, whatever was in memory.
static void fitImageBars()
{
    std::mt19937 rng(777);
    for (int n = 0; n < 200; ++n) {
        const PixelDepth d = n % 5 == 0 ? PixelDepth::Float : PixelDepth::Byte;
        const int sw = 20 + (int)(rng() % 200), sh = 20 + (int)(rng() % 150);
        const int w = 30 + (int)(rng() % 250), h = 30 + (int)(rng() % 200);
        ImagePtr src = Image::make(sw, sh, d, false);
        for (size_t i = 0; i < src->byteSize(); ++i) src->data.get()[i] = d == PixelDepth::Float ? 0x3f : (uint8_t)(1 + rng() % 255);
        // (fill the memory the new frame may reuse with something that is not zero)
        for (int k = 0; k < 4; ++k) { ImagePtr junk = Image::make(w, h, d, false); std::memset(junk->data.get(), 0xAB, junk->byteSize()); }
        ImagePtr out = fitImage(*src, w, h);
        bool ok = out && out->w == w && out->h == h;
        if (ok) {
            // the picture lies in a rectangle of the output; what is outside it must be zero, and some of the inside must not be
            int minx = w, maxx = -1, miny = h, maxy = -1;
            const size_t px = out->pixelBytes();
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x) {
                    bool any = false;
                    for (size_t b = 0; b < px; ++b) any = any || out->row(y)[(size_t)x * px + b] != 0;
                    if (any) { minx = std::min(minx, x); maxx = std::max(maxx, x); miny = std::min(miny, y); maxy = std::max(maxy, y); }
                }
            const double s = std::min((double)w / sw, (double)h / sh);
            const int dw = std::min(w, std::max(1, (int)(sw * s + 0.5))), dh = std::min(h, std::max(1, (int)(sh * s + 0.5)));
            ok = maxx >= minx && maxx - minx + 1 <= dw && maxy - miny + 1 <= dh && (w - dw) / 2 <= minx && (h - dh) / 2 <= miny && maxx < (w - dw) / 2 + dw && maxy < (h - dh) / 2 + dh;
        }
        check(ok, strfmt("fitImage %dx%d into %dx%d (%s): the bars are empty", sw, sh, w, h, d == PixelDepth::Float ? "float" : "8-bit"));
    }
}

// blendImages at 0 and 1 is one of the two pictures, as the general formula gives it (also onto one of them).
static void blendEnds()
{
    std::mt19937 rng(99);
    for (int n = 0; n < 40; ++n) {
        const int w = 5 + (int)(rng() % 40), h = 3 + (int)(rng() % 20);
        ImagePtr a = Image::make(w, h, PixelDepth::Byte, false), b = Image::make(w, h, PixelDepth::Byte, false);
        for (size_t i = 0; i < a->byteSize(); ++i) { a->data.get()[i] = (uint8_t)rng(); b->data.get()[i] = (uint8_t)rng(); }
        for (int end = 0; end < 2; ++end) {
            const float t = end ? 1.f : 0.f;
            ImagePtr dst = Image::make(w, h, PixelDepth::Byte, false);
            blendImages(*dst, *a, *b, t);
            check(std::memcmp(dst->data.get(), (end ? b : a)->data.get(), dst->byteSize()) == 0, strfmt("blendImages at %g is one picture", t));
            ImagePtr onA = a->clone();
            blendImages(*onA, *onA, *b, t);
            check(std::memcmp(onA->data.get(), (end ? b : a)->data.get(), onA->byteSize()) == 0, strfmt("blendImages at %g onto the first", t));
        }
    }
}

int main(int argc, char** argv)
{
    fitImageBars();
    blendEnds();
    const int count = argc > 1 ? std::atoi(argv[1]) : 120;
    MediaPool pool;
    pool.setOpener(testOpen);
    pool.setShareFrames(true);                  // (as the playing preview has it: the same frame is the same picture every time)
    FxRuntime fx;
    VideoEngine video(pool, fx);
    std::mt19937 rng(12345);
    auto uni = [&](double a, double b) { return a + (b - a) * (double)(rng() % 100001) / 100000.0; };
    auto pick = [&](int n) { return (int)(rng() % (unsigned)n); };
    const char* qualities[] = {"draft", "preview", "good", "best"};
    const char* modes[] = {"srcalpha", "srcalpha", "srcalpha", "add", "multiply", "screen"};
    int differentPictures = 0, rendered = 0;
    for (int n = 0; n < count; ++n) {
        Project p;
        p.settings.width = 96 + pick(5) * 32;
        p.settings.height = 54 + pick(4) * 24;
        p.settings.renderQuality = qualities[pick(4)];
        const int tracks = 1 + pick(3);
        std::vector<uint64_t> trackIds;
        for (int ti = 0; ti < tracks; ++ti) {
            Track t;
            t.id = p.newId();
            t.type = TrackType::Video;
            if (ti > 0 && pick(4) == 0) t.compositeLevel = 1;               // (a child of the track above: a compositing group)
            const int style = pick(8);
            t.level = style == 0 ? 0.5 : (style == 1 ? 0.999 : 1.0);
            t.composite = modes[pick(6)];
            if (ti == tracks - 1 && pick(5) == 0) t.mute = true;
            const int fpsChoices[] = {30, 30, 24, 60, 25};
            const uint64_t media = addMedia(p, strfmt("noise:%d,%d,%d,%d%s", 80 + pick(300), 60 + pick(200), pick(1000), fpsChoices[pick(5)], pick(5) == 0 ? ",still" : ""));
            // one to three events cut from the same spot of the media (a stutter: they show the same frames one after another)
            const int copies = 1 + pick(3);
            const double offset = 0.1 * pick(10);
            const bool samePlacement = pick(2) == 0;
            std::vector<PanCropKey> boxes;
            const int nboxes = pick(4);                                      // 0: none (the whole frame)
            for (int b = 0; b < nboxes; ++b) {
                PanCropKey k;
                k.time = b * 0.5;
                k.cx = uni(0.05, 0.95); k.cy = uni(0.05, 0.95);
                k.w = uni(0.1, 1.6) * (pick(5) == 0 ? -1 : 1);                // (a negative width: mirrored)
                k.h = uni(0.1, 1.6) * (pick(7) == 0 ? -1 : 1);
                if (pick(3) == 0) { k.cx = 0.5; k.cy = 0.5; k.w = 1; k.h = 1; }
                boxes.push_back(k);
            }
            const bool keepAspect = pick(3) != 0, fill = pick(4) == 0;
            for (int c = 0; c < copies; ++c) {
                Event e;
                e.id = p.newId();
                e.mediaId = media;
                e.start = c * 0.4;
                e.length = c + 1 == copies ? 4.0 : 0.4;
                e.offset = offset;
                e.keepAspect = keepAspect;
                e.stretchToFill = fill;
                e.panCrop = boxes;
                if (!samePlacement && c > 0) for (PanCropKey& k : e.panCrop) k.cx = std::min(0.95, k.cx + 0.1);
                t.events.push_back(e);
            }
            if (pick(2) == 0) {                                                // Track Motion (no turn)
                MotionKey m;
                m.x = uni(0.1, 0.9); m.y = uni(0.1, 0.9);
                m.w = uni(0.2, 1.7); m.h = uni(0.2, 1.7);
                if (pick(4) == 0) { m.w = 1; m.h = 1; m.x = 0.5 + (pick(3) - 1) * 0.25; m.y = 0.5; }
                t.motion.push_back(m);
            }
            if (ti == 1 && pick(5) == 0) t.adjustment = true;
            p.tracks.push_back(t);
            trackIds.push_back(t.id);
        }
        FrameOptions fo;
        fo.interactive = pick(2) == 0;
        const double scales[] = {1.0, 0.5, 0.25};
        fo.scale = scales[pick(3)];
        const double times[] = {0.1 * pick(4), 0.4 + 0.1 * pick(4), 0.8 + 0.1 * pick(4), 0.1 * pick(4), 0.1 * pick(4), 0.4 + 0.1 * pick(4), 0.4 + 0.1 * pick(4)};
        std::vector<ImagePtr> plain, quick;
        setComposeFastPaths(false);
        for (double at : times) plain.push_back(video.render(p, at, fo));
        setComposeFastPaths(true);
        for (double at : times) quick.push_back(video.render(p, at, fo));
        for (size_t i = 0; i < plain.size(); ++i) {
            const ImagePtr& a = plain[i];
            const ImagePtr& b = quick[i];
            ++rendered;
            const std::string what = strfmt("random project %d (%dx%d %s, %d tracks, scale %.2f, %s, t %.1f)", n, p.settings.width, p.settings.height,
                                            p.settings.renderQuality.c_str(), tracks, fo.scale, fo.interactive ? "preview" : "final", times[i]);
            check(a && b, what + ": both frames made");
            if (a && b) {
                const bool same = a->w == b->w && a->h == b->h && std::memcmp(a->data.get(), b->data.get(), a->byteSize()) == 0;
                check(same, what + ": same bytes", strfmt("%016llx vs %016llx", (unsigned long long)hashImage(a), (unsigned long long)hashImage(b)));
                if (a->w > 0) ++differentPictures;
            }
        }
    }
    check(differentPictures == rendered, "every render made a picture");
    uint64_t axisWarps = 0, warps = 0, copies = 0, notCopied = 0, memoHits = 0;
    composeFastStats(axisWarps, warps, copies, notCopied, memoHits);
    std::printf("shortcuts taken: %llu of %llu warps per column, %llu layers copied onto a canvas, %llu frames handed on uncopied, %llu warps/blends remembered\n",
                (unsigned long long)axisWarps, (unsigned long long)warps, (unsigned long long)copies, (unsigned long long)notCopied, (unsigned long long)memoHits);
    check(axisWarps > 100 && copies > 20 && notCopied > 20 && memoHits > 20, "the shortcuts were really exercised");
    std::printf("compose_fast_test: %d passed, %d failed (%d projects)\n", gPass, gFail, rendered);
    return gFail ? 1 : 0;
}
