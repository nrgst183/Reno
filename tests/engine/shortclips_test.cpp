// Tests that very short events (stutters) play and show exactly where VEGAS puts them.
//
//   shortclips_test [folder with .veg projects] [output folder]
//   (defaults: $RENO_SAMPLES and the current folder; the project tests are skipped when the projects are
//    missing; RENO_SAMPLE_VEG names a single project to use instead. Media is made up: a sound whose sample n is (n + 1) / 1e6, and a picture holding the media time it was asked for.)
//
// - the mixer: for every audio event, the first and last sample it plays (timeToSample) and the file sample each one
//   reads, with no fade (Quickfade off) and no gap or overlap between events that butt;
// - playback = render: the mix pulled in blocks of any size from any start equals the one made in one go;
// - exportProject writes the sound as long as the project (whole samples), not to the last picture's end;
// - the picture: each project frame shows the event covering the frame's start, at the media time of that start;
// - the media pool does not carry another event's read on from a sample or two off.
#include "../../src/engine/engine.h"
#include "../../src/vegas/vegas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <map>
#include <vector>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// Sound: sample n is (n mod 8191 + 1) / 8192 on both sides (so what is heard says which file sample it is). Picture: 16 x 16, its
// colour holds the time it was asked for in 0.1 ms (B, G, R = low, middle, high byte).
class MadeUp : public MediaReader {
public:
    MediaInfo mi;
    int64_t lastEnd = -1;
    MadeUp()
    {
        mi.hasAudio = mi.hasVideo = true;
        mi.sampleRate = 48000;
        mi.channels = 2;
        mi.duration = 3000.0;
        mi.width = mi.height = 16;
        mi.fps = 30;
    }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int, int) override
    {
        ImagePtr img = Image::make(16, 16, PixelDepth::Byte, true);
        const int64_t n = (int64_t)std::llround(seconds * 10000.0);
        for (int y = 0; y < 16; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < 16; ++x, d += 4) {
                d[0] = (uint8_t)(n & 255);
                d[1] = (uint8_t)((n >> 8) & 255);
                d[2] = (uint8_t)((n >> 16) & 255);
                d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            left[k] = right[k] = (n < 0 || sampleRate != mi.sampleRate) ? 0.f : (float)((double)(n % 8191 + 1) / 8192.0);
        }
        return true;
    }
};

static MediaPool::Opener madeUpOpener()
{
    return [](const std::string& path, std::string* err) -> std::unique_ptr<MediaReader> {
        if (path.compare(0, 7, "madeup:") == 0) return std::unique_ptr<MediaReader>(new MadeUp());
        return openMedia(path, err);
    };
}

// The project with every setting that is not the point of the test put to neutral: media made up, no effects, fades,
// envelopes or gains, tracks at unity, pitch and rate 1, no crossfades or quickfades.
static void neutral(Project& p)
{
    for (auto& m : p.media) {
        m.path = "madeup:1";
        m.offline = false;
        m.generated = false;
        m.info = MadeUp().mi;
        m.info.hasVideo = true;
    }
    p.autoCrossfades = false;
    p.quickFade = 0;
    p.masterVolume = 1.0;
    p.masterPan = 0;
    p.masterMute = false;
    p.audioOut.clear();
    p.videoOut.clear();
    p.masterEnvelopes.clear();
    p.videoEnvelopes.clear();
    for (auto& t : p.tracks) {
        t.fx.clear();
        t.envelopes.clear();
        t.level = 1.0;
        t.motion.clear();
        t.pan = 0;
        t.mute = false;
        t.solo = false;
        t.panType = "add";
        t.invertPhase = false;
        for (auto& e : t.events) {
            e.fx.clear();
            e.envelopes.clear();
            e.fadeIn = e.fadeOut = 0;
            e.gain = 1.0;
            e.normalize = false;
            e.pitch = 0;
            e.rate = 1.0;
            e.reverse = false;
            e.mute = false;
            e.invertPhase = false;
            e.channels = "both";
            e.resample = "off";
            e.panCrop.clear();
            e.keepPitch = false;
            e.mediaChannel = -1;
            e.transition = FxSlot();
        }
    }
    p.buses.clear();
}

// The sound the project should make (mono: both sides are the same): every event plays the file sample
// first + (s - firstSample) at each sample it covers, and where events overlap they add up (no crossfades).
static std::vector<double> expectedMix(const Project& p, int64_t n)
{
    const int rate = p.settings.sampleRate;
    std::vector<double> mix((size_t)n, 0.0);
    for (const auto& t : p.tracks) {
        if (t.type != TrackType::Audio) continue;
        for (const auto& e : t.events) {
            const MediaItem* m = p.findMedia(e.mediaId);
            if (!m) continue;
            const int64_t a = timeToSample(e.start, rate), b = timeToSample(e.end(), rate);
            const int64_t from = (int64_t)std::floor(e.offset * m->info.sampleRate + 0.5 + 1e-7);
            for (int64_t s = std::max<int64_t>(0, a); s < std::min(b, n); ++s) mix[(size_t)s] += (double)((from + (s - a)) % 8191 + 1) / 8192.0;
        }
    }
    return mix;
}

static std::vector<float> renderBlocks(const Project& p, int64_t from, int64_t n, int block, MediaPool& pool, FxRuntime& fx)
{
    AudioEngine audio(pool, fx);
    std::vector<float> out((size_t)n), l((size_t)block), r((size_t)block);
    for (int64_t s = 0; s < n; s += block) {
        const int k = (int)std::min<int64_t>(block, n - s);
        audio.render(p, from + s, k, l.data(), r.data(), true);
        std::memcpy(out.data() + s, l.data(), (size_t)k * sizeof(float));
    }
    return out;
}


int main(int argc, char** argv)
{
    const std::string folder = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    const std::string outDir = argc > 2 ? argv[2] : ".";
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    FxRuntime fx;

    // ------------------------------------------------------------------ small hand-made cases
    {
        // events a few samples long, back to back, one starting a sample before where the last one's media ended
        Project p;
        p.settings.sampleRate = 48000;
        p.settings.fpsNum = 140;
        p.settings.fpsDen = 1;
        MediaItem m;
        m.id = p.newId();
        m.path = "madeup:1";
        m.name = "madeup";
        m.info = MadeUp().mi;
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.panType = "add";
        double at = 0.0;
        const double lens[] = {0.0044352, 0.00001, 0.0000417, 0.0003, 0.0044352, 0.00002083, 0.01};
        const double offs[] = {10.0, 10.0 + 4.0 / 44100, 10.0 + 0.0044352 + 3.0 / 48000, 11.0, 11.0 + 1.0 / 48000, 12.0, 0.0};
        for (int k = 0; k < 7; ++k) {
            Event e;
            e.id = p.newId();
            e.mediaId = m.id;
            e.start = at;
            e.length = lens[k];
            e.offset = offs[k];
            e.keepPitch = false;
            setDefaultFadeCurves(e, TrackType::Audio);
            t.events.push_back(e);
            at += lens[k];
        }
        p.tracks.push_back(t);
        const int64_t n = timeToSample(at, 48000) + 20;
        MediaPool pool;
        pool.setOpener(madeUpOpener());
        const std::vector<float> got = renderBlocks(p, 0, n, 1024, pool, fx);
        const std::vector<double> want = expectedMix(p, n);
        int64_t bad = -1;
        for (int64_t s = 0; s < n && bad < 0; ++s)
            if (std::fabs((double)got[(size_t)s] - want[(size_t)s]) > 1e-9) bad = s;
        check(bad < 0, "tiny hand-made events (sub-sample, a few samples long): every sample is the expected file sample", bad < 0 ? "" : strfmt("sample %lld: %.9f, wanted %.9f", (long long)bad, got[(size_t)bad], want[(size_t)bad]));
        // a media pool that has just read an event's media up to a sample must not take another event's start a sample off
        {
            MediaPool pl;
            pl.setOpener(madeUpOpener());
            MediaItem mm = m;
            std::vector<float> a(10), b(10), c(10);
            pl.audio(1001, mm, 5000, 10, 48000, a.data(), b.data(), -1, 0);          // event 1 reads [5000, 5010)
            pl.audio(1002, mm, 5011, 10, 48000, a.data(), b.data(), -1, 0);          // event 2 starts a sample after where it ended
            check(std::fabs(a[0] - 5012.0 / 8192) < 1e-9, "another event starting a sample past where the last read ended reads from its own sample", strfmt("%.6f", a[0]));
            pl.audio(1002, mm, 5022, 10, 48000, a.data(), b.data(), -1, 0);          // the same event, a sample on: carried on
            check(std::fabs(a[0] - 5022.0/8192) < 1e-9 || std::fabs(a[0] - 5024.0/8192) < 1e-9, "the same event's next read is still carried on", strfmt("%.6f", a[0]));
        }
    }

    // ------------------------------------------------------------------ the sample projects: every .veg under the folder
    // (all of them, or just the one named by RENO_SAMPLE_VEG)
    std::vector<std::string> vegs = testVegFiles(folder);
    if (!testSampleVeg().empty()) vegs = {testSampleVeg()};
    int ran = 0;
    for (const std::string& veg : vegs) {
        const char* rel = veg.c_str();
        Project p;
        if (!fileExists(veg) || !importVegasProject(veg, p).ok) {
            std::printf("SKIP %s (not there)\n", rel);
            continue;
        }
        // (a project of short clips - stutters: at least a quarter of its sound events under 100 ms; others are not what this is about)
        {
            size_t sounds = 0, shorts = 0;
            for (const auto& t : p.tracks)
                if (t.type == TrackType::Audio)
                    for (const auto& e : t.events) { ++sounds; shorts += e.length < 0.1 ? 1 : 0; }
            if (shorts == 0 || shorts * 4 < sounds) continue;
        }
        ++ran;
        neutral(p);
        const int rate = p.settings.sampleRate;
        int64_t n = 0;
        size_t audioEvents = 0, tiny = 0, subSample = 0;
        for (const auto& t : p.tracks)
            if (t.type == TrackType::Audio)
                for (const auto& e : t.events) {
                    n = std::max(n, timeToSample(e.end(), rate) + 8);
                    ++audioEvents;
                    if (e.length < 0.1) ++tiny;
                    if (e.length * rate < 1.0) ++subSample;
                }
        n = std::min<int64_t>(n, 48000 * 40);
        MediaPool pool;
        pool.setOpener(madeUpOpener());
        const std::vector<float> got = renderBlocks(p, 0, n, 1024, pool, fx);
        const std::vector<double> want = expectedMix(p, n);
        int64_t bad = -1, badCount = 0;
        for (int64_t s = 0; s < n; ++s)
            if (std::fabs((double)got[(size_t)s] - want[(size_t)s]) > 1e-9) {
                if (bad < 0) bad = s;
                ++badCount;
            }
        const std::string name = pathFileName(veg);
        if (bad >= 0 && getenv("SC_DEBUG"))
            for (const auto& t : p.tracks)
                if (t.type == TrackType::Audio)
                    for (const auto& e : t.events)
                        if (timeToSample(e.start, rate) <= bad + 2 && timeToSample(e.end(), rate) >= bad - 2)
                            std::printf("   track %llu event %llu %lld..%lld (start %.9f len %.9f) off %.9f\n", (unsigned long long)t.id, (unsigned long long)e.id, (long long)timeToSample(e.start, rate), (long long)timeToSample(e.end(), rate), e.start, e.length, e.offset);
        check(bad < 0, strfmt("%s: all %lld samples of %zu audio events (%zu under 100 ms, %zu under a sample) are the expected file samples", name.c_str(), (long long)n, audioEvents, tiny, subSample),
              bad < 0 ? "" : strfmt("%lld wrong, first at %lld: %.9f, wanted %.9f", (long long)badCount, (long long)bad, got[(size_t)bad], want[(size_t)bad]));
        // first and last sample of each event, one by one
        size_t edgesWrong = 0, edges = 0;
        for (const auto& t : p.tracks) {
            if (t.type != TrackType::Audio) continue;
            for (const auto& e : t.events) {
                const int64_t a = timeToSample(e.start, rate), b = timeToSample(e.end(), rate);
                if (b <= a || b >= n || a < 1) continue;
                ++edges;
                // the sample before the first and the one after the last are not this event's (unless another covers them)
                bool covered = false;
                (void)covered;
                for (const auto& tt : p.tracks)
                  if (tt.type == TrackType::Audio)
                    for (const auto& o : tt.events)
                    if (o.id != e.id && timeToSample(o.start, rate) <= b && timeToSample(o.end(), rate) >= a - 1) covered = true;
                if (covered) continue;
                const double first = got[(size_t)a], last = got[(size_t)b - 1];
                const MediaItem* m = p.findMedia(e.mediaId);
                const int64_t from = (int64_t)std::floor(e.offset * m->info.sampleRate + 0.5 + 1e-7);
                bool ok = std::fabs(first - (double)(from % 8191 + 1) / 8192.0) < 1e-9 && std::fabs(last - (double)((from + (b - a) - 1) % 8191 + 1) / 8192.0) < 1e-9;
                if (!ok) ++edgesWrong;
            }
        }
        check(edgesWrong == 0, strfmt("%s: first and last sample of %zu events are their media's own", name.c_str(), edges), strfmt("%zu wrong", edgesWrong));
        // playback = render: other block sizes, and starting in the middle with a fresh engine (a seek, a loop)
        for (int block : {1, 7, 333, 1024, 4096}) {
            if (block == 1 && n > 60000) continue;
            const std::vector<float> a = renderBlocks(p, 0, std::min<int64_t>(n, block == 1 ? 6000 : n), block, pool, fx);
            bool same = true;
            for (size_t s = 0; s < a.size() && same; ++s) same = a[s] == got[s];
            check(same, strfmt("%s: blocks of %d samples give the same sound as 1024", name.c_str(), block));
        }
        {
            bool same = true;
            int64_t at = 0;
            for (int64_t start : {n / 3, n / 2 + 5, n - 1500}) {
                if (start < 0 || start + 1200 > n) continue;
                MediaPool pl;
                pl.setOpener(madeUpOpener());
                const std::vector<float> a = renderBlocks(p, start, 1200, 1024, pl, fx);
                for (size_t s = 0; s < a.size() && same; ++s) {
                    same = a[s] == got[(size_t)start + s];
                    at = start + (int64_t)s;
                }
            }
            check(same, strfmt("%s: a fresh engine started in the middle (a seek) makes the same sound as one from the start", name.c_str()), strfmt("sample %lld", (long long)at));
        }
        // the picture (nothing covering a frame shows black): the first video track with events
        const Track* vt = nullptr;
        for (const auto& t : p.tracks)
            if (t.type == TrackType::Video && !t.events.empty() && !vt) vt = &t;
        if (vt) {
            Project q = p;
            // (only that track: the others would cover it)
            for (auto& t : q.tracks)
                if (t.type == TrackType::Video && t.id != vt->id) t.events.clear();
            q.settings.width = q.settings.height = 16;
            MediaPool vp;
            vp.setOpener(madeUpOpener());
            VideoEngine video(vp, fx);
            FrameOptions fo;
            fo.interactive = false;
            const int64_t frames = std::min<int64_t>(q.frameAt(vt->events.back().end()) + 2, 1500);
            int64_t wrong = 0, shown = 0, hidden = 0, firstWrong = -1;
            double fps = q.settings.fps();
            for (int64_t f = 0; f < frames; ++f) {
                const double tf = q.frameTime(f);
                const Event* top = nullptr;
                for (const auto& e : vt->events)
                    { const int64_t ft = (int64_t)std::ceil(tf * 1e7 - 1e-3), st = std::llround(e.start * 1e7); if (st <= ft && ft < st + std::llround(e.length * 1e7)) top = &e; }      // (the last in the track is on top)
                ImagePtr img = video.render(q, tf + 1e-7, fo);
                if (!img) { ++wrong; continue; }
                const uint8_t* px = img->row(8) + 8 * 4;
                const int64_t code = px[0] | (px[1] << 8) | (px[2] << 16);
                const bool lit = px[3] > 0;
                if (!top) {
                    ++hidden;
                    if (code != 0) { ++wrong; if (firstWrong < 0) firstWrong = f; }
                } else {
                    ++shown;
                    const double mt = top->offset + (tf - top->start);
                    if (!lit || std::llabs(code - (int64_t)std::llround(mt * 10000.0)) > 1) { ++wrong; if (firstWrong < 0) firstWrong = f; }
                }
            }
            (void)fps;
            check(wrong == 0, strfmt("%s: %lld frames, %lld show the event covering the frame's start, %lld show nothing", name.c_str(), (long long)frames, (long long)shown, (long long)hidden),
                  strfmt("%lld wrong, first at frame %lld", (long long)wrong, (long long)firstWrong));
        }
        // the render writes the sound as long as the project (whole samples), and the same samples
        if (ran == 1) {
            Project r = p;
            r.tracks.erase(std::remove_if(r.tracks.begin(), r.tracks.end(), [](const Track& t) { return t.type != TrackType::Audio; }), r.tracks.end());
            EncodeSettings es;
            es.path = pathJoin(outDir, "shortclips_render.wav");
            es.video = false;
            es.audio = true;
            es.audioFamily = "pcm";
            es.audioBits = 32;
            es.fpsNum = r.settings.fpsNum;
            es.fpsDen = r.settings.fpsDen;
            std::string err;
            const bool ok = exportProject(r, fx, es, 0.0, r.length(), nullptr, &err, madeUpOpener());
            check(ok, name + ": rendering the sound to a .wav works", err);
            if (ok) {
                std::unique_ptr<MediaReader> rd = openMedia(es.path, &err);
                const int64_t want = (int64_t)std::floor(r.length() * rate + 1e-6);
                const int64_t have = rd ? (int64_t)std::llround(rd->info().duration * rate) : -1;
                check(std::llabs(have - want) <= 1, name + ": the rendered sound is the project's length in whole samples, not up to the last picture", strfmt("%lld samples, wanted %lld (a frame is %.0f)", (long long)have, (long long)want, rate / r.settings.fps()));
                if (rd) {
                    const int64_t m2 = std::min<int64_t>(want, n);
                    std::vector<float> l((size_t)m2), rr((size_t)m2);
                    rd->audio(0, (int)m2, rate, l.data(), rr.data());
                    int64_t diff = 0;
                    for (int64_t s = 0; s < m2; ++s)
                        if (std::fabs((double)l[(size_t)s] - (double)got[(size_t)s]) > 1e-6) ++diff;
                    check(diff == 0, name + ": the rendered file holds the same samples as the mix", strfmt("%lld differ", (long long)diff));
                }
            }
        }
    }
    if (!ran) std::printf("SKIP: no sample project (.veg) of short clips found in %s\n", folder.c_str());
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
