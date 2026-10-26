// Checks that the speed-ups do not change the output:
//  - MediaPool shares readers between events and gives the same pictures and sound as private readers;
//  - the mixer's cross-fade weights over the events that can overlap equal those over the whole track;
//  - fitImage's copy for an already fitted picture equals resampling;
//  - FrameOptions::scale renders a smaller picture of the same frame.
//
//   perf_test <testfx.ofx> <scratch folder>
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static bool samePixels(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b) return !a && !b;
    if (a->w != b->w || a->h != b->h || a->depth != b->depth) return false;
    for (int y = 0; y < a->h; ++y)
        if (std::memcmp(a->row(y), b->row(y), (size_t)a->w * a->pixelBytes())) return false;
    return true;
}

// A clip whose every frame is different (the frame number in its colours and a moving bar), with a tone that rises.
static bool writeClip(const std::string& path, int frames, int gop)
{
    EncodeSettings es;
    es.path = path;
    es.width = 160;
    es.height = 120;
    es.fpsNum = 30;
    es.keyframeSeconds = gop / 30.0;
    es.videoFamily = "h264";
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    const int spf = 48000 / 30;
    std::vector<float> l((size_t)spf), r((size_t)spf);
    for (int f = 0; f < frames; ++f) {
        ImagePtr img = Image::make(160, 120, PixelDepth::Byte, false);
        for (int y = 0; y < 120; ++y)
            for (int x = 0; x < 160; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                const bool bar = ((x + f * 3) % 160) < 20;
                d[0] = (uint8_t)(f * 5);
                d[1] = (uint8_t)(bar ? 255 : (f * 11) & 255);
                d[2] = (uint8_t)(y * 2);
                d[3] = 255;
            }
        if (!w->writeVideo(*img)) return false;
        for (int k = 0; k < spf; ++k) {
            const double t = (double)(f * spf + k) / 48000.0;
            l[(size_t)k] = (float)(0.4 * std::sin(2 * 3.14159265358979 * (200 + 100 * t) * t));
            r[(size_t)k] = (float)(0.3 * std::sin(2 * 3.14159265358979 * (300 + 50 * t) * t));
        }
        if (!w->writeAudio(l.data(), r.data(), spf)) return false;
    }
    return w->finish(nullptr);
}

static void testSharedVideo(const std::string& clip)
{
    MediaItem m;
    m.id = 5;
    m.path = clip;
    std::string err;
    std::unique_ptr<MediaReader> probe = openMedia(clip, &err);
    check(probe != nullptr, "the test clip opens", err);
    if (!probe) return;
    m.info = probe->info();
    MediaPool pool;
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> at(0.0, m.info.duration);
    // a stutter: 40 events, each a few frames, of a handful of places in the file (repeated), and some far apart
    int bad = 0, asked = 0;
    for (int ev = 0; ev < 40; ++ev) {
        const double base = (ev % 3 == 2) ? at(rng) : 1.0 + (ev % 4) * 0.2;
        for (int f = 0; f < 4; ++f) {
            const double t = base + f / 30.0;
            ImagePtr shared = pool.videoFrame(1000 + ev, m, t, 160, 120);
            std::unique_ptr<MediaReader> own = openMedia(clip, nullptr);
            ImagePtr fresh = own ? own->videoFrame(t, 160, 120) : nullptr;
            ++asked;
            if (!samePixels(shared, fresh)) ++bad;
        }
    }
    check(bad == 0, "a stutter's frames from shared readers are a fresh reader's", strfmt("%d of %d differ", bad, asked));
    // two events of the file playing at once, at different places (layered copies): frame after frame
    bad = asked = 0;
    for (int f = 0; f < 30; ++f) {
        for (int ev = 0; ev < 3; ++ev) {
            const double t = 0.5 + ev * 2.7 + f / 30.0;
            ImagePtr shared = pool.videoFrame(2000 + ev, m, t, 80, 60);
            std::unique_ptr<MediaReader> own = openMedia(clip, nullptr);
            ImagePtr fresh = own ? own->videoFrame(t, 80, 60) : nullptr;
            ++asked;
            if (!samePixels(shared, fresh)) ++bad;
        }
    }
    check(bad == 0, "layered events' frames from shared readers are a fresh reader's", strfmt("%d of %d differ", bad, asked));
    // backwards (a reversed event) and the thumbnails' lane at the same time
    bad = asked = 0;
    for (int f = 0; f < 20; ++f) {
        const double t = 4.0 - f / 30.0;
        ImagePtr a = pool.videoFrame(3000, m, t, 160, 120);
        ImagePtr b = pool.videoFrame(3000 ^ 0x8000000000000000ull, m, t * 0.5, 40, 30);
        std::unique_ptr<MediaReader> own = openMedia(clip, nullptr);
        ++asked;
        if (!samePixels(a, own->videoFrame(t, 160, 120))) ++bad;
        std::unique_ptr<MediaReader> own2 = openMedia(clip, nullptr);
        if (!samePixels(b, own2->videoFrame(t * 0.5, 40, 30))) ++bad;
    }
    check(bad == 0, "backwards, with thumbnails between, from shared readers", strfmt("%d of %d differ", bad, asked));
    // a file that cannot be opened: nothing, every time, and no crash
    MediaItem gone = m;
    gone.id = 6;
    gone.path = "/nonexistent/perf_test_missing.mp4";
    bool none = true;
    for (int k = 0; k < 5; ++k) none = none && !pool.videoFrame(100 + k, gone, 0.5, 64, 64);
    check(none, "a missing file gives no picture to any event");
}

// Seeking in a long-GOP file decodes only what the picture wanted needs (the B-frames before it are skipped): the
// pictures must be exactly those decoding the file from its start, one after another, gives.
static void testSeekSkipping(const std::string& clip)
{
    std::unique_ptr<MediaReader> seq = openMedia(clip, nullptr);
    if (!seq) return;
    const double fps = seq->info().fps > 0 ? seq->info().fps : 30.0;
    const int frames = (int)std::floor(seq->info().duration * fps + 1e-6);
    std::vector<ImagePtr> all;
    for (int f = 0; f < frames; ++f) all.push_back(seq->videoFrame((f + 0.5) / fps, 160, 120)->clone());
    std::mt19937 rng(5);
    std::uniform_int_distribution<int> pick(0, frames - 1);
    int bad = 0, asked = 0;
    std::unique_ptr<MediaReader> r = openMedia(clip, nullptr);
    for (int k = 0; k < 60; ++k) {
        const int f = pick(rng);
        ++asked;
        if (!samePixels(r->videoFrame((f + 0.5) / fps, 160, 120), all[(size_t)f])) ++bad;
    }
    // and jumps forward a little over a second at a time (decoded on, with skipping, rather than seeked)
    for (int f = 3; f < frames; f += 37) {
        ++asked;
        if (!samePixels(r->videoFrame((f + 0.5) / fps, 160, 120), all[(size_t)f])) ++bad;
    }
    check(bad == 0, "pictures reached by seeking and skipping are the ones decoded in order", strfmt("%d of %d differ", bad, asked));
    // a ping-pong: a stretch forwards, the same stretch backwards (from the pictures kept on the way), forwards again
    bad = asked = 0;
    std::unique_ptr<MediaReader> pp = openMedia(clip, nullptr);
    for (int round = 0; round < 3; ++round)
        for (int k = 0; k <= 30; ++k) {
            const int f = (round % 2 == 0) ? 100 + k : 130 - k;
            ++asked;
            if (!samePixels(pp->videoFrame((f + 0.5) / fps, 160, 120), all[(size_t)f])) ++bad;
        }
    check(bad == 0, "a stretch forwards, backwards and forwards again: the pictures decoded in order", strfmt("%d of %d differ", bad, asked));
}

static void testSharedAudio(const std::string& clip)
{
    MediaItem m;
    m.id = 7;
    m.path = clip;
    std::unique_ptr<MediaReader> probe = openMedia(clip, nullptr);
    if (!probe) return;
    m.info = probe->info();
    MediaPool pool;
    const int n = 1024;
    std::vector<float> l(n), r(n), fl(n), fr(n);
    // events of the file read block after block, interleaved (two at once), each starting where the stutter puts it
    int bad = 0, asked = 0;
    struct Ev { uint64_t id; int64_t pos; };
    std::vector<Ev> evs;
    for (int k = 0; k < 12; ++k) evs.push_back({(uint64_t)(500 + k), (int64_t)(48000 * (0.3 + (k % 3) * 0.25))});
    std::vector<std::unique_ptr<MediaReader>> own;
    for (size_t k = 0; k < evs.size(); ++k) own.push_back(openMedia(clip, nullptr));
    for (size_t k = 0; k + 1 < evs.size(); k += 2)
        for (int b = 0; b < 6; ++b)
            for (size_t j = k; j < k + 2; ++j) {
                pool.audio(evs[j].id, m, evs[j].pos, n, 48000, l.data(), r.data());
                own[j]->audio(evs[j].pos, n, 48000, fl.data(), fr.data());
                evs[j].pos += n;
                ++asked;
                float worst = 0;
                for (int i = 0; i < n; ++i)
                    worst = std::max(worst, std::max(std::fabs(l[(size_t)i] - fl[(size_t)i]), std::fabs(r[(size_t)i] - fr[(size_t)i])));
                // (a reader that got there by reading on rather than by seeking can differ in the decoder's rounding -
                // as an event's own reader always could, depending on where it last played: far below hearing)
                if (worst > 1e-4f) {
                    ++bad;
                    if (getenv("PERF_TEST_VERBOSE")) std::printf("  event %zu block %d: differs by %g\n", j, b, worst);
                }
            }
    check(bad == 0, "sound from shared readers is a reader of its own's (to -80 dB)", strfmt("%d of %d blocks differ", bad, asked));
}

static void testOverlapWeights()
{
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    int bad = 0, asked = 0;
    for (int trial = 0; trial < 40; ++trial) {
        Track tr;
        tr.type = TrackType::Audio;
        double at = 0;
        for (int k = 0; k < 60; ++k) {
            Event e;
            e.id = (uint64_t)(k + 1);
            e.start = at;
            e.length = 0.05 + u(rng) * 0.4;
            e.mute = u(rng) < 0.1;
            e.fadeInCurve = (int)(u(rng) * 5);
            e.fadeOutCurve = (int)(u(rng) * 5);
            at += u(rng) < 0.3 ? 0.0 : u(rng) * 0.3;          // (some start together)
            tr.events.push_back(e);
        }
        std::vector<const Event*> others;
        for (const Event& e : tr.events) {
            overlappingEvents(tr, e, others);
            for (int s = 0; s <= 20; ++s) {
                const double t = e.start + e.length * s / 20.0;
                for (bool xf : {true, false}) {
                    ++asked;
                    if (overlapWeight(tr, e, t, xf) != overlapWeightAmong(others, e, t, xf)) ++bad;
                }
            }
        }
    }
    check(bad == 0, "cross-fade weights over the overlapping events are the whole track's", strfmt("%d of %d differ", bad, asked));
}

static void testFitImage()
{
    // a 4:3 picture fitted into a 16:9 frame of the same height: the picture itself in the middle, bars either side
    for (PixelDepth d : {PixelDepth::Byte, PixelDepth::Float}) {
        ImagePtr src = Image::make(120, 90, d, false);
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 120; ++x) {
                if (d == PixelDepth::Byte) {
                    uint8_t* p = src->row(y) + x * 4;
                    p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)(x ^ y); p[3] = 255;
                } else {
                    float* p = (float*)src->row(y) + x * 4;
                    p[0] = x / 119.f; p[1] = y / 89.f; p[2] = 0.25f; p[3] = 1.f;
                }
            }
        ImagePtr out = fitImage(*src, 160, 90);
        bool ok = out && out->w == 160 && out->h == 90;
        const size_t px = src->pixelBytes();
        for (int y = 0; ok && y < 90; ++y) {
            ok = std::memcmp(out->row(y) + 20 * px, src->row(y), 120 * px) == 0;
            for (int x = 0; ok && x < 160; ++x)
                if (x < 20 || x >= 140)
                    for (size_t b = 0; b < px; ++b) ok = ok && out->row(y)[x * px + b] == 0;
        }
        check(ok, std::string("fitImage of a picture already its fitted size: the picture with bars (") + (d == PixelDepth::Byte ? "8-bit" : "float") + ")");
    }
}

static void testScale(const std::string& clip)
{
    Project p;
    p.settings.width = 320;
    p.settings.height = 240;
    MediaItem m;
    m.id = p.newId();
    m.path = clip;
    std::unique_ptr<MediaReader> probe = openMedia(clip, nullptr);
    if (!probe) return;
    m.info = probe->info();
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 3;
    t.events.push_back(e);
    MotionKey k;
    k.w = 0.5;
    k.h = 0.5;
    k.x = 0.3;
    t.motion.push_back(k);
    p.tracks.push_back(t);
    FxRuntime fx;
    MediaPool pool;
    VideoEngine ve(pool, fx);
    FrameOptions full, half;
    half.scale = 0.5;
    ImagePtr a = ve.render(p, 1.0, full), b = ve.render(p, 1.0, half);
    check(a && a->w == 320 && a->h == 240, "scale 1: the project's size");
    check(b && b->w == 160 && b->h == 120, "scale 0.5: half the size", b ? strfmt("%dx%d", b->w, b->h) : std::string("none"));
    if (!a || !b) return;
    // the same picture: the full one averaged down by 2 x 2 is close to the half one
    double diff = 0;
    for (int y = 0; y < 120; ++y)
        for (int x = 0; x < 160; ++x)
            for (int c = 0; c < 3; ++c) {
                int s = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) s += a->row(y * 2 + dy)[(x * 2 + dx) * 4 + c];
                diff += std::fabs(s / 4.0 - b->row(y)[x * 4 + c]);
            }
    diff /= 160.0 * 120.0 * 3.0;
    check(diff < 6.0, "the half-size frame is the frame, smaller", strfmt("mean difference %.2f", diff));
    // with an OpenFX effect on the track (it works at the project's size): inverted at either size
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = "reno.test.invert";
    s.name = "Invert";
    p.tracks[0].fx.push_back(s);
    fx.prepare(p);
    if (!fx.video(s.id)) {
        check(false, "the test effect loads", fx.error(s.id));
        return;
    }
    ImagePtr ai = ve.render(p, 1.0, full), bi = ve.render(p, 1.0, half);
    check(ai && bi && bi->w == 160 && bi->h == 120, "with an effect, scale 0.5: half the size");
    if (!ai || !bi) return;
    // where the picture is (inside the track motion's box), the effect's result at half size matches the full one
    double d2 = 0;
    int cnt = 0;
    for (int y = 40; y < 80; ++y)
        for (int x = 30; x < 60; ++x)
            for (int c = 0; c < 3; ++c, ++cnt) {
                int sum = 0;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) sum += ai->row(y * 2 + dy)[(x * 2 + dx) * 4 + c];
                d2 += std::fabs(sum / 4.0 - bi->row(y)[x * 4 + c]);
            }
    d2 /= cnt;
    check(d2 < 8.0, "an effect's picture at half size is its full-size picture, smaller", strfmt("mean difference %.2f", d2));
    fx.clear();
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: perf_test <testfx.ofx> <scratch folder>\n");
        return 2;
    }
    std::string err;
    check(ofx::Host::get().loadBinary(argv[1], &err) != nullptr, "the test effects load", err);
    const std::string clip = std::string(argv[2]) + "/perf_test_clip.mp4";
    check(writeClip(clip, 180, 90), "a long-GOP test clip is written");
    testSharedVideo(clip);
    testSeekSkipping(clip);
    testSharedAudio(clip);
    testOverlapWeights();
    testFitImage();
    testScale(clip);
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
