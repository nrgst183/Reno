// Tests where a pitch-shifted audio event puts its sound.
//
// VEGAS's élastique answers a pitch change late (or early): against VEGAS's own renders, an event pitched by n
// semitones (factor f = 2^(n/12)) lines up with an ideal shifter only when moved by  L x (f - 1)  output samples,
// L being about 1050 for Efficient and 2500 for Pro (see elastiqueDelaySamples in stretch.cpp). Sound that needs
// no pitch shift (rate alone, method None, Classic) has no such delay. Checks, on a train of noise bursts:
//   - bursts of a pitched event land where VEGAS puts them (+1 semitone: 63 samples later, +2: 131, +3: 199,
//     -1: 59 earlier, -3: 167 earlier for Efficient; +1: 152 later, -1: 136 earlier for Pro);
//   - unpitched, rate-only and method None events are not moved (Classic: timing_samples_test);
//   - the same sound for any block size, and the event ends on its last sample;
//   - the stretcher's delay parameter moves the sound by exactly that many samples.
//
//   pitch_align_test      (prints PASS / FAIL lines, then SUMMARY)
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include "../../src/engine/stretch.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

// Bursts of noise (20 ms, Hann-shaped, a different noise each) every 0.25 s from media time 1.0 s; the same in both channels.
static const double kFirstBurst = 1.0, kBurstEvery = 0.25;
static const int kBurstHalf = 480;

static double noiseAt(int burst, int k)
{
    uint32_t x = (uint32_t)(burst * 7919 + k * 104729 + 12345);
    x ^= x >> 15; x *= 0x2c1b3c6dU; x ^= x >> 12; x *= 0x297a2d39U; x ^= x >> 15;
    return (double)x / 2147483648.0 - 1.0;
}

class Bursts : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            double v = 0;
            const double t = (double)n / sampleRate;
            const int b = (int)std::floor((t - kFirstBurst) / kBurstEvery + 0.5);
            if (b >= 0) {
                const int64_t centre = (int64_t)std::llround((kFirstBurst + b * kBurstEvery) * sampleRate);
                const int64_t d = n - centre;
                if (d >= -kBurstHalf && d < kBurstHalf) v = 0.5 * noiseAt(b, (int)d + kBurstHalf) * 0.5 * (1 + std::cos(kPi * (double)d / kBurstHalf));
            }
            left[k] = right[k] = (float)v;
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> burstsOpen(const std::string& path, std::string*)
{
    std::unique_ptr<Bursts> r(new Bursts());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 30;
    r->mi.sampleRate = kRate;
    r->mi.channels = 2;
    return std::unique_ptr<MediaReader>(r.release());
}

struct Case {
    std::string method = "elastique", mode = "efficient";
    bool keep = true;
    double pitch = 0, rate = 1;
    bool reverse = false;
};

static const double kStart = 0.5, kLength = 2.0, kOffset = 0.9;

static Project make(const Case& c)
{
    static uint64_t next = 100;
    Project p;
    p.settings.sampleRate = kRate;
    p.nextId = next;
    next += 100;
    MediaItem m;
    m.id = p.newId();
    m.path = m.name = "bursts";
    m.info = burstsOpen("bursts", nullptr)->info();
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = kStart;
    e.length = kLength;
    e.offset = kOffset;
    e.rate = c.rate;
    e.loop = false;
    e.reverse = c.reverse;
    e.stretchMethod = c.method;
    e.stretchMode = c.mode;
    e.keepPitch = c.keep;
    e.pitch = c.pitch;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

static std::vector<float> renderAll(const Project& p, int64_t from, int n, int block)
{
    MediaPool pool;
    pool.setOpener(burstsOpen);
    FxRuntime fx;
    AudioEngine audio(pool, fx);
    std::vector<float> l((size_t)n, 0.f), r((size_t)n, 0.f);
    for (int at = 0; at < n; at += block) audio.render(p, from + at, std::min(block, n - at), l.data() + at, r.data() + at, false);
    return l;
}

// The mean distance (samples) of the bursts' centres of energy from where an ideal shifter would have them, over the bursts
// well inside the event, and the number of bursts used.
static double burstShift(const Case& c, int* used)
{
    const Project p = make(c);
    const int64_t s0 = (int64_t)std::llround(kStart * kRate), len = (int64_t)std::llround(kLength * kRate);
    const int pad = 4800;
    const std::vector<float> out = renderAll(p, s0 - pad, (int)len + 2 * pad, 777);
    double sum = 0;
    int n = 0;
    for (int b = 0;; ++b) {
        const double mt = kFirstBurst + b * kBurstEvery;                    // the burst's media time
        const double at = kStart + (mt - kOffset) / c.rate;                 // where an ideal stretcher puts it, on the timeline
        const int64_t centre = (int64_t)std::llround(at * kRate) - (s0 - pad);
        if (at > kStart + kLength - 0.25) break;
        if (at < kStart + 0.2) continue;
        double e = 0, m = 0;
        for (int k = -2400; k <= 2400; ++k) {
            const int64_t i = centre + k;
            if (i < 0 || i >= (int64_t)out.size()) continue;
            const double v = (double)out[(size_t)i] * out[(size_t)i];
            e += v;
            m += v * k;
        }
        if (e < 1e-9) continue;
        sum += m / e;
        ++n;
    }
    if (used) *used = n;
    return n ? sum / n : 0;
}

static std::string name(const Case& c)
{
    return c.method + (c.method == "none" ? "" : "/" + c.mode) + strfmt(" pitch %+g rate %g", c.pitch, c.rate) + (c.keep ? "" : " locked");
}

int main()
{
    // 1. VEGAS's numbers (measured on its renders), within +-18 samples (the bursts' own scatter is about 10 samples)
    struct Want { const char* mode; double pitch, shift, tol; };
    const Want wants[] = {{"efficient", 1, 63, 18},  {"efficient", 2, 131, 18},  {"efficient", 3, 199, 18},  {"efficient", -1, -59, 18}, {"efficient", -3, -167, 18},
                          {"pro", 1, 152, 18},       {"pro", -1, -136, 18},      {"pro", 3, 490, 30}};
    for (const Want& w : wants) {
        Case c;
        c.mode = w.mode;
        c.pitch = w.pitch;
        int n = 0;
        const double got = burstShift(c, &n);
        check(n >= 5 && std::fabs(got - w.shift) < w.tol, name(c) + strfmt(": the sound is %+g samples from the ideal place, as in VEGAS", w.shift),
              strfmt("%+.1f over %d bursts", got, n));
    }
    // 2. the law behind it, over the whole range, for every élastique mode
    for (const char* m : {"efficient", "pro", "soloist-mono", "soloist-speech"}) {
        bool ok = std::fabs(elastiqueDelaySamples(m, 0)) < 1e-9;
        for (double st : {-24.0, -12.0, -7.0, -1.0, 0.5, 1.0, 3.0, 7.0, 12.0, 24.0}) {
            const double d = elastiqueDelaySamples(m, st);
            ok = ok && std::isfinite(d) && (st > 0 ? d >= 0 : d <= 0) && std::fabs(d) < 3000;
        }
        ok = ok && elastiqueDelaySamples(m, 2) > elastiqueDelaySamples(m, 1) && elastiqueDelaySamples(m, -2) < elastiqueDelaySamples(m, -1);
        check(ok, std::string("elastiqueDelaySamples ") + m + ": zero without a pitch change, later for up, earlier for down, growing, bounded");
    }
    check(elastiqueDelaySamples("pro", 1) > 2 * elastiqueDelaySamples("efficient", 1), "Pro answers a pitch change later than Efficient");
    // 3. nothing else is moved
    {
        Case c;
        c.rate = 1.5;
        int n = 0;
        double got = burstShift(c, &n);
        check(n >= 3 && std::fabs(got) < 14, name(c) + ": a rate alone is not moved", strfmt("%+.1f over %d bursts", got, n));
        c.mode = "pro";
        got = burstShift(c, &n);
        check(n >= 3 && std::fabs(got) < 14, name(c) + ": a rate alone is not moved", strfmt("%+.1f over %d bursts", got, n));
        Case t;
        t.method = "none";
        t.rate = 1.5;
        t.keep = false;
        got = burstShift(t, &n);
        check(n >= 3 && std::fabs(got) < 3, name(t) + ": tape-like sound is not moved", strfmt("%+.1f over %d bursts", got, n));
    }
    // 4. the same whatever the block size, and the event still ends on its last sample
    for (const char* m : {"efficient", "pro", "soloist-speech"}) {
        Case c;
        c.mode = m;
        c.pitch = 3;
        const Project p = make(c);
        const int64_t s0 = (int64_t)std::llround(kStart * kRate), len = (int64_t)std::llround(kLength * kRate);
        const int n = (int)len + 4800;
        const std::vector<float> a = renderAll(p, s0 - 2400, n, 64), b = renderAll(p, s0 - 2400, n, 5000);
        double worst = 0;
        for (int k = 0; k < n; ++k) worst = std::max(worst, (double)std::fabs(a[(size_t)k] - b[(size_t)k]));
        check(worst < 1e-5, name(c) + ": the same in blocks of 64 and 5000 samples", strfmt("largest difference %g", worst));
        double before = 0, after = 0;
        for (int k = 0; k < 2400; ++k) before = std::max(before, (double)std::fabs(a[(size_t)k]));
        for (int k = (int)len + 2400; k < n; ++k) after = std::max(after, (double)std::fabs(a[(size_t)k]));
        check(before == 0.0 && after == 0.0, name(c) + ": silent before the event and from its end on", strfmt("before %g, after %g", before, after));
    }
    // 5. the stretcher's delay parameter: sound later by exactly that many samples (and earlier for a negative one)
    {
        std::vector<double> x((size_t)kRate * 4, 0.0);
        for (int k = -480; k < 480; ++k) x[(size_t)(96000 + k)] = noiseAt(3, k + 480) * 0.5 * (1 + std::cos(kPi * k / 480.0));
        const StretchPull pull = [&](int64_t i, int k, float* l, float* r) {
            for (int j = 0; j < k; ++j) l[j] = r[j] = (i + j >= 0 && i + j < (int64_t)x.size()) ? (float)x[(size_t)(i + j)] : 0.f;
        };
        auto centroid = [&](double delay) {
            StretchParams sp;
            sp.mode = "efficient";
            sp.rate = kRate;
            sp.alpha = 1.0;
            sp.pitch = 1.0;
            sp.resampled = 1.0;
            sp.delay = delay;
            auto st = makeStretcher(sp);
            std::vector<float> l((size_t)kRate * 3), r((size_t)kRate * 3);
            for (size_t at = 0; at < l.size(); at += 1000) st->process(l.data() + at, r.data() + at, (int)std::min<size_t>(1000, l.size() - at), pull);
            double e = 0, m = 0;
            for (size_t k = 60000; k < 132000; ++k) {
                const double v = (double)l[k] * l[k];
                e += v;
                m += v * (double)k;
            }
            return m / e;
        };
        const double base = centroid(0);
        for (double d : {200.0, -150.0}) {
            const double got = centroid(d) - base;
            check(std::fabs(got - d) < 2.0, strfmt("StretchParams::delay %+g moves the sound by that much", d), strfmt("%+.2f", got));
        }
    }
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
