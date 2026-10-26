// Tests of the audio cache for costly events (time-stretched, pitch-shifted, resampled) and the worker that fills it
// (AudioEngine::prefetch): cached output equals uncached output for any block size, entering an event mid-way,
// the cache limit, and changed media files.
//
//   audiocache_test <scratch folder>
#include "../../src/engine/engine.h"
#include "../../src/engine/stretch.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <filesystem>
#include <fstream>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : " - ", detail.c_str());
    if (!ok) ++gFail;
    std::fflush(stdout);
}

static const double kPi = 3.14159265358979323846;
static const int kRate = 48000;
static float gGain = 1.0f;          // what the "file" holds: changed together with the file's time to play a changed file

// A buzz that wanders in pitch (so that a wrong position is heard as a wrong sound), at the rate asked for, 30 s long.
class Tone : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        const int64_t stop = (int64_t)std::llround(30.0 * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            const double t = (double)n / sampleRate;
            double v = 0;
            if (n >= 0 && n < stop)
                for (int h = 1; h <= 5; ++h) v += 0.2 / h * std::sin(2.0 * kPi * (180.0 + 20.0 * std::sin(t * 3.0)) * h * t);
            left[k] = (float)v * gGain;
            right[k] = (float)(v * 0.5) * gGain;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> toneOpen(const std::string& path, std::string*)
{
    std::unique_ptr<Tone> r(new Tone());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 30;
    r->mi.sampleRate = path.find("44k") != std::string::npos ? 44100 : kRate;
    r->mi.channels = 2;
    return std::unique_ptr<MediaReader>(r.release());
}

static std::string gDir;

static Project makeProject()
{
    Project p;
    p.settings.sampleRate = kRate;
    p.nextId = 100;
    MediaItem a, b;
    a.id = p.newId();
    a.path = gDir + "/tone48.bin";
    a.name = "tone48";
    a.info = toneOpen(a.path, nullptr)->info();
    b.id = p.newId();
    b.path = gDir + "/tone44k.bin";
    b.name = "tone44";
    b.info = toneOpen(b.path, nullptr)->info();
    p.media.push_back(a);
    p.media.push_back(b);
    Track t1, t2;
    t1.id = p.newId();
    t1.type = TrackType::Audio;
    t2.id = p.newId();
    t2.type = TrackType::Audio;
    struct Spec { double start, length, offset, rate, pitch; const char* method; const char* mode; bool keep, reverse; int media; };
    const Spec specs[] = {
        {0.10, 1.20, 2.00, 0.80, 0.0, "elastique", "pro", true, false, 0},             // phase vocoder, slowed down
        {0.40, 0.90, 5.00, 1.30, 0.0, "classic", "A03", true, false, 0},               // Classic, sped up, overlapping the first
        {1.10, 0.80, 1.00, 1.00, 3.0, "elastique", "soloist-mono", true, false, 0},    // pitched up
        {0.00, 1.60, 3.00, 1.00, 0.0, "elastique", "pro", true, false, 1},             // plain, a 44.1 kHz file (resampled)
        {1.50, 0.70, 4.00, 1.10, 0.0, "elastique", "efficient", true, true, 0},        // stretched and reversed
    };
    for (const Spec& s : specs) {
        Event e;
        e.id = p.newId();
        e.mediaId = p.media[(size_t)s.media].id;
        e.start = s.start;
        e.length = s.length;
        e.offset = s.offset;
        e.rate = s.rate;
        e.pitch = s.pitch;
        e.loop = false;
        e.reverse = s.reverse;
        e.stretchMethod = s.method;
        e.stretchMode = s.mode;
        e.keepPitch = s.keep;
        (s.media == 0 ? t1 : t2).events.push_back(e);
    }
    p.tracks.push_back(t1);
    p.tracks.push_back(t2);
    return p;
}

struct Out { std::vector<float> l, r; };

static Out renderRange(const Project& p, int64_t from, int64_t n, int block, bool freshEngine = true, AudioEngine* use = nullptr, MediaPool* usePool = nullptr)
{
    MediaPool localPool;
    FxRuntime fx;
    MediaPool& pool = usePool ? *usePool : localPool;
    if (!usePool) pool.setOpener(toneOpen);
    std::unique_ptr<AudioEngine> own;
    if (!use) own.reset(new AudioEngine(pool, fx));
    AudioEngine& audio = use ? *use : *own;
    (void)freshEngine;
    Out o;
    o.l.assign((size_t)n, 0.f);
    o.r.assign((size_t)n, 0.f);
    for (int64_t at = 0; at < n; at += block)
        audio.render(p, from + at, (int)std::min<int64_t>(block, n - at), o.l.data() + at, o.r.data() + at, true);
    return o;
}

static bool same(const Out& a, const Out& b, int64_t aFrom = 0, int64_t bFrom = 0, int64_t n = -1)
{
    if (n < 0) n = (int64_t)std::min(a.l.size(), b.l.size());
    return std::memcmp(a.l.data() + aFrom, b.l.data() + bFrom, (size_t)n * sizeof(float)) == 0 &&
           std::memcmp(a.r.data() + aFrom, b.r.data() + bFrom, (size_t)n * sizeof(float)) == 0;
}

static double energy(const Out& a, int64_t from, int64_t n)
{
    double e = 0;
    for (int64_t i = from; i < from + n; ++i) e += (double)a.l[(size_t)i] * a.l[(size_t)i];
    return e;
}

int main(int argc, char** argv)
{
    gDir = argc > 1 ? argv[1] : "audiocache_out";
    std::filesystem::create_directories(gDir);
    for (const char* n : {"tone48.bin", "tone44k.bin"}) std::ofstream(gDir + "/" + n) << "x";
    const Project p = makeProject();
    const int64_t total = (int64_t)(2.4 * kRate);

    // the reference: no cache at all, block by block (what the mixer always did)
    AudioEngine::setEventCacheLimit(0);
    const Out ref = renderRange(p, 0, total, 1024);
    check(energy(ref, 0, total) > 1.0, "the project makes sound");
    AudioEngine::setEventCacheLimit((size_t)256 << 20);
    AudioEngine::clearEventCache();

    // 1. the cache gives the same sound, whatever the blocks
    {
        const Out a = renderRange(p, 0, total, 1024);
        check(same(ref, a), "played from the cache (1024-sample blocks) = made block by block");
        check(AudioEngine::eventCacheBytes() > 0, "the events were kept", std::to_string(AudioEngine::eventCacheBytes() / 1024) + " KB");
        const Out b = renderRange(p, 0, total, 333);
        check(same(ref, b), "same from the cache with odd blocks (333)");
        AudioEngine::clearEventCache();
        const Out c = renderRange(p, 0, total, 333);
        check(same(ref, c), "same made into the cache with odd blocks (333)");
        const Out d = renderRange(p, 0, total, 7000);
        check(same(ref, d), "same with big blocks (7000)");
    }

    // 2. entering events in the middle sounds as playing from the start
    {
        AudioEngine::clearEventCache();
        bool allSame = true;
        for (double at : {0.30, 0.62, 1.05, 1.31, 1.9}) {
            const int64_t s = (int64_t)(at * kRate);
            const Out m = renderRange(p, s, 4096, 1024);
            allSame = allSame && same(ref, m, s, 0, 4096);
            AudioEngine::clearEventCache();
        }
        check(allSame, "a seek into the middle of stretched, resampled and reversed events sounds as playing from the start");
        // a seek, then play on: one engine
        MediaPool pool;
        pool.setOpener(toneOpen);
        FxRuntime fx;
        AudioEngine audio(pool, fx);
        const Out first = renderRange(p, (int64_t)(0.7 * kRate), 2048, 1024, true, &audio, &pool);
        const Out jump = renderRange(p, (int64_t)(1.3 * kRate), 2048, 1024, true, &audio, &pool);
        const Out back = renderRange(p, (int64_t)(0.7 * kRate), 2048, 1024, true, &audio, &pool);
        check(same(ref, first, (int64_t)(0.7 * kRate), 0, 2048) && same(ref, jump, (int64_t)(1.3 * kRate), 0, 2048) && same(first, back),
              "seeking forwards and back on one engine gives the same sound every time");
    }

    // 3. prefetch makes the events on the worker thread
    {
        AudioEngine::clearEventCache();
        MediaPool pool;
        pool.setOpener(toneOpen);
        FxRuntime fx;
        AudioEngine audio(pool, fx);
        audio.prefetch(p, 0, total);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        size_t last = 0;
        int steady = 0;
        while (std::chrono::steady_clock::now() < until && steady < 8) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const size_t now = AudioEngine::eventCacheBytes();
            steady = (now == last && now > 0) ? steady + 1 : 0;
            last = now;
        }
        // (four stretched or resampled events: about 1.2 + 0.9 + 0.8 + 1.6 + 0.7 s of stereo floats)
        const size_t expect = (size_t)((1.2 + 0.9 + 0.8 + 1.6 + 0.7) * kRate) * 8;
        check(last >= expect - expect / 100 && last <= expect + expect / 100, "prefetch made all the events ahead", std::to_string(last) + " of " + std::to_string(expect) + " bytes");
        const Out a = renderRange(p, 0, total, 1024, true, &audio, &pool);
        check(same(ref, a), "played after prefetch = made block by block");
    }

    // 4. the limit
    {
        AudioEngine::clearEventCache();
        const size_t limit = (size_t)300 * 1024;
        AudioEngine::setEventCacheLimit(limit);
        const Out a = renderRange(p, 0, total, 1024);
        check(same(ref, a), "with a small cache: the same sound");
        check(AudioEngine::eventCacheBytes() <= limit + (size_t)(4096 * 8 * 8), "the cache stays near its limit", std::to_string(AudioEngine::eventCacheBytes()) + " of " + std::to_string(limit));
        AudioEngine::setEventCacheLimit((size_t)256 << 20);
    }

    // 5. a changed file is not played from the old sound
    {
        AudioEngine::clearEventCache();
        MediaPool pool;
        pool.setOpener(toneOpen);
        FxRuntime fx;
        AudioEngine audio(pool, fx);
        const Out a = renderRange(p, 0, 8192, 1024, true, &audio, &pool);
        gGain = 0.5f;
        for (const char* n : {"tone48.bin", "tone44k.bin"}) {
            const std::string f = gDir + "/" + n;
            std::filesystem::last_write_time(f, std::filesystem::last_write_time(f) + std::chrono::seconds(30));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4500));      // (the file is looked at again every couple of seconds)
        const Out b = renderRange(p, 0, 8192, 1024, true, &audio, &pool);
        gGain = 1.0f;
        const double ea = energy(a, 0, 8192), eb = energy(b, 0, 8192);
        check(eb < 0.35 * ea && eb > 0.0, "a media file that changed is played as it is now", "energy " + std::to_string(eb / ea));
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
