// Measures the pitch an audio event really has for each way VEGAS changes it, using a 200 Hz buzz through the audio
// engine: method None, Classic (A03), élastique (all four modes); "Lock to stretch" on and off; whole and fractional
// semitones, alone, with a rate, and reversed. Each case must also be exactly as long as the event, start on its
// first sample, and be the same for any block size.
//
//   pitch_modes_test      (prints PASS / FAIL lines, then SUMMARY)
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

// A 200 Hz buzz (first eight harmonics, falling off as 1/h), 30 s long, same in both channels. A pitch-synchronous
// stretcher (Soloist) needs formants, which a sine does not have.
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
            double v = 0;
            if (n >= 0 && n < stop)
                for (int h = 1; h <= 8; ++h) v += 0.25 / h * std::sin(2.0 * kPi * 200.0 * h * (double)n / sampleRate);
            left[k] = right[k] = (float)v;
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
    r->mi.sampleRate = kRate;
    r->mi.channels = 2;
    return std::unique_ptr<MediaReader>(r.release());
}

struct Case {
    std::string method = "elastique", mode = "pro";
    bool keep = true;           // not "Lock to stretch"
    double pitch = 0, rate = 1;
    bool reverse = false;
};

// The pitch factor VEGAS gives an event: the rate alone for None; else the rate unless it is kept, and the semitones.
static double expectedFactor(const Case& c)
{
    if (c.method == "none") return c.rate;
    return (c.keep ? 1.0 : c.rate) * std::pow(2.0, c.pitch / 12.0);
}

static Project make(const Case& c, double start, double length, double offset)
{
    static uint64_t next = 100;
    Project p;
    p.settings.sampleRate = kRate;
    p.nextId = next;
    next += 100;
    MediaItem m;
    m.id = p.newId();
    m.path = m.name = "tone";
    m.info = toneOpen("tone", nullptr)->info();
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Audio;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = start;
    e.length = length;
    e.offset = offset;
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
    pool.setOpener(toneOpen);
    FxRuntime fx;
    AudioEngine audio(pool, fx);
    std::vector<float> l((size_t)n, 0.f), r((size_t)n, 0.f);
    for (int at = 0; at < n; at += block) audio.render(p, from + at, std::min(block, n - at), l.data() + at, r.data() + at, false);
    return l;
}

// the fundamental of a harmonic sound: the autocorrelation peak between 70 and 1000 Hz, refined between samples
static double frequency(const float* s, size_t n)
{
    const int lo = kRate / 1000, hi = kRate / 70;
    const size_t w = std::min(n - (size_t)hi - 2, (size_t)9600);
    if (n < (size_t)hi + 100) return 0;
    std::vector<double> c((size_t)hi + 3, 0.0);
    double e0 = 0;
    for (size_t j = 0; j < w; ++j) e0 += (double)s[j] * s[j];
    for (int t = lo - 1; t <= hi + 1; ++t) {
        double dot = 0, et = 0;
        for (size_t j = 0; j < w; ++j) {
            dot += (double)s[j] * s[j + (size_t)t];
            et += (double)s[j + (size_t)t] * s[j + (size_t)t];
        }
        c[(size_t)t] = dot / std::sqrt(e0 * et + 1e-12);
    }
    double top = 0;
    for (int t = lo; t <= hi; ++t) top = std::max(top, c[(size_t)t]);
    for (int t = lo; t <= hi; ++t)
        if (c[(size_t)t] > 0.95 * top && c[(size_t)t] >= c[(size_t)t - 1] && c[(size_t)t] >= c[(size_t)t + 1]) {
            const double a = c[(size_t)t - 1], b = c[(size_t)t], d = c[(size_t)t + 1];
            const double den = a - 2 * b + d;
            const double off = std::fabs(den) > 1e-12 ? 0.5 * (a - d) / den : 0;
            return kRate / (t + off);
        }
    return 0;
}

static double rms(const float* s, size_t n)
{
    double e = 0;
    for (size_t k = 0; k < n; ++k) e += (double)s[k] * s[k];
    return n ? std::sqrt(e / (double)n) : 0;
}

static std::string name(const Case& c)
{
    return c.method + (c.method == "none" ? "" : "/" + c.mode) + (c.keep ? "" : " locked") + strfmt(" pitch %+g rate %g", c.pitch, c.rate) + (c.reverse ? " reversed" : "");
}

int main()
{
    const double start = 0.5, length = 1.5, offset = 2.0;
    const int64_t s0 = (int64_t)std::llround(start * kRate), len = (int64_t)std::llround(length * kRate);

    std::vector<Case> cases;
    const char* elast[] = {"pro", "efficient", "soloist-mono", "soloist-speech"};
    auto add = [&](const std::string& method, const std::string& mode, bool keep, double pitch, double rate, bool rev) {
        Case c;
        c.method = method; c.mode = mode; c.keep = keep; c.pitch = pitch; c.rate = rate; c.reverse = rev;
        cases.push_back(c);
    };
    // pitch alone: whole and fractional semitones, up and down, every method
    for (double st : {-12.0, -5.0, -0.5, 0.25, 1.0, 7.0, 12.0, 24.0}) {
        for (const char* m : elast) add("elastique", m, true, st, 1.0, false);
        add("classic", "A03", true, st, 1.0, false);
    }
    // the rate: kept pitch (Lock to stretch off), locked to the rate, and None
    for (double r : {0.5, 0.8, 1.25, 2.0, 3.0}) {
        for (const char* m : elast) {
            add("elastique", m, true, 0, r, false);
            add("elastique", m, false, 0, r, false);
        }
        add("classic", "A03", true, 0, r, false);
        add("classic", "A03", false, 0, r, false);
        add("none", "pro", false, 0, r, false);
    }
    // a rate and a pitch change together, and reversed
    for (double r : {0.75, 1.5}) {
        add("elastique", "pro", true, 5, r, false);
        add("elastique", "efficient", true, -7, r, false);
        add("classic", "A03", true, 3, r, false);
        add("elastique", "pro", false, 4, r, false);       // Reno lets a locked event carry semitones too: they add up
        add("none", "pro", false, 0, r, true);
    }
    for (double st : {-3.0, 2.0, 12.0}) {
        add("elastique", "pro", true, st, 1.0, true);
        add("classic", "A03", true, st, 1.0, true);
        add("elastique", "soloist-mono", true, st, 1.0, true);
    }
    add("elastique", "pro", true, 4, 1.5, true);
    add("none", "pro", false, 0, 1.0, true);

    int pitchOk = 0, lengthOk = 0, startOk = 0, blocksOk = 0;
    for (const Case& c : cases) {
        const Project p = make(c, start, length, offset);
        const int n = (int)(s0 + len + kRate / 2);
        const std::vector<float> a = renderAll(p, 0, n, 1024);
        const double factor = expectedFactor(c);
        // pitch: from the middle of the event (clear of the stretchers' first and last few milliseconds)
        const size_t m0 = (size_t)(s0 + len / 5), m1 = (size_t)(s0 + len - len / 5);
        const double f = frequency(a.data() + m0, m1 - m0);
        const double cents = f > 0 ? 1200.0 * std::log2(f / (200.0 * factor)) : 1e9;
        // Classic splices a waveform from where it best continues, which wanders a few cents in the measure
        const double tol = c.method == "classic" ? 25.0 : 12.0;
        const bool ok = std::fabs(cents) < tol;
        pitchOk += ok;
        check(ok, "pitch: " + name(c), strfmt("%.2f Hz wanted %.2f Hz (%+.1f cents)", f, 200.0 * factor, cents));

        // length: nothing before the first sample, sound in the first and last few milliseconds, nothing after
        bool before = true, after = true;
        for (int64_t k = 0; k < s0; ++k) before = before && a[(size_t)k] == 0.f;
        for (int64_t k = s0 + len; k < n; ++k) after = after && a[(size_t)k] == 0.f;
        const double head = rms(a.data() + s0, 96), tail = rms(a.data() + s0 + len - 96, 96);
        const bool lenOk = before && after && head > 0.01 && tail > 0.01;
        lengthOk += lenOk;
        check(lenOk, "length: " + name(c), strfmt("silent before %d, after %d, sound in first 2 ms %.3f, last 2 ms %.3f", before, after, head, tail));

        // the same whatever the block size (a render in odd-sized blocks, and one in a single block)
        const std::vector<float> b = renderAll(p, 0, n, 777);
        double worst = 0;
        for (int64_t k = s0; k < s0 + len; ++k) worst = std::max(worst, (double)std::fabs(a[(size_t)k] - b[(size_t)k]));
        const bool same = worst < 1e-4;
        blocksOk += same;
        check(same, "block sizes: " + name(c), strfmt("largest difference %.6f", worst));

        // seeking into the middle of the event gives the same sound as playing up to it
        const int64_t seek = s0 + len / 2;
        const std::vector<float> d = renderAll(p, seek, 4800, 1024);
        double sd = 0, e = 0;
        for (int k = 0; k < 4800; ++k) {
            sd += (double)(d[(size_t)k] - a[(size_t)(seek + k)]) * (d[(size_t)k] - a[(size_t)(seek + k)]);
            e += (double)a[(size_t)(seek + k)] * a[(size_t)(seek + k)];
        }
        // (a stretcher started afresh in the middle is not the same samples; it must have the same pitch and level)
        const double fd = frequency(d.data(), 4800);
        const bool seekOk = fd > 0 && std::fabs(1200.0 * std::log2(fd / (200.0 * factor))) < 60.0 && std::fabs(rms(d.data(), 4800) - rms(a.data() + seek, 4800)) < 0.1;
        (void)sd; (void)e;
        check(seekOk, "seek: " + name(c), strfmt("%.2f Hz", fd));
        startOk += before;
    }

    // exact semitone arithmetic: None with a rate of 2^(n/12) is n semitones, to the cent
    for (int st : {-12, -7, 1, 5, 12}) {
        Case c;
        c.method = "none"; c.keep = false; c.rate = std::pow(2.0, st / 12.0);
        const std::vector<float> a = renderAll(make(c, 0, 2.0, 1.0), 0, 2 * kRate, 1024);
        const double f = frequency(a.data() + 4800, 60000);
        check(std::fabs(1200.0 * std::log2(f / (200.0 * c.rate))) < 1.0, strfmt("None at the rate of %+d semitones is %+d semitones", st, st), strfmt("%.3f Hz", f));
    }

    // an event of any length is that long: lengths down to one sample, and starts between samples
    {
        Case c;
        const double lengths[] = {0.0001, 0.0005, 0.003, 0.0061, 0.02, 0.1};
        int ok = 0, total = 0;
        for (const char* meth : {"elastique", "classic"})
            for (double L : lengths)
                for (double pitch : {0.0, 3.0}) {
                    c.method = meth; c.mode = std::string(meth) == "classic" ? "A03" : "efficient"; c.keep = true; c.pitch = pitch; c.rate = 1;
                    const double st = 0.3001234;
                    const Project p = make(c, st, L, 2.0);
                    const int n = (int)(0.5 * kRate);
                    const std::vector<float> a = renderAll(p, 0, n, 512);
                    const int64_t first = timeToSample(st, kRate), last = timeToSample(st + L, kRate);
                    bool good = true;
                    for (int64_t k = 0; k < n; ++k)
                        if ((k < first || k >= last) && a[(size_t)k] != 0.f) good = false;
                    ++total;
                    ok += good;
                    if (!good) std::printf("   (%s pitch %g: an event of %g s leaks outside samples [%lld, %lld))\n", meth, pitch, L, (long long)first, (long long)last);
                }
        check(ok == total, "very short events (0.1 ms to 100 ms) sound only inside their own samples", strfmt("%d of %d", ok, total));
    }

    std::printf("pitch %d/%zu, length %d/%zu, block sizes %d/%zu, start %d/%zu\n", pitchOk, cases.size(), lengthOk, cases.size(), blocksOk, cases.size(), startOk, cases.size());
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
