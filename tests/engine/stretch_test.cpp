// Tests and measurements of the time-stretchers (Time stretch / pitch shift methods) at rates 0.5, 0.8, 1.25, 2 and 4:
// pitch kept (cents), duration, joins between blocks of any size, seek, reversed events, pitch shifting, formants,
// and processor time.
//
//   stretch_test            (prints a table of the measurements, then SUMMARY)
#include "../../src/engine/engine.h"
#include "../../src/engine/engine_internal.h"
#include "../../src/engine/stretch.h"
#include <algorithm>
#include <chrono>
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

// Made-up media (60 s long):
//   "tone:<hz>:<seconds>"            a sine at half volume for that long, then silence
//   "harm:<f0>:<formant hz>:<s>"     a buzz of harmonics of f0 shaped by one resonance (a vowel-like formant)
//   "beat:<seconds>"                 a 440 Hz tone with a sharp 2 ms click every 0.25 s
class Synth : public MediaReader {
public:
    MediaInfo mi;
    int kind = 0;
    double freq = 440, until = 10, formant = 1000;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return nullptr; }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        const int64_t stop = (int64_t)std::llround(until * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            double v = 0;
            if (n >= 0 && n < stop) {
                const double t = (double)n / sampleRate;
                if (kind == 0) v = 0.5 * std::sin(2.0 * kPi * freq * t);
                else if (kind == 1) {
                    for (int h = 1; h * freq < 6000; ++h) {
                        const double f = h * freq, d = (f - formant) / 250.0;
                        v += 0.25 * std::sin(2.0 * kPi * f * t) / (1.0 + d * d);
                    }
                } else {
                    v = 0.3 * std::sin(2.0 * kPi * 440 * t);
                    const double ph = std::fmod(t, 0.25);
                    if (ph < 0.002) v += 0.6 * (1.0 - ph / 0.002);
                }
            }
            left[k] = (float)v;
            right[k] = (float)(kind == 0 ? v * 0.8 : v);
        }
        return true;
    }
};

static std::unique_ptr<MediaReader> synthOpen(const std::string& path, std::string* error)
{
    std::unique_ptr<Synth> r(new Synth());
    r->mi.path = path;
    r->mi.hasAudio = true;
    r->mi.duration = 60;
    r->mi.sampleRate = kRate;
    r->mi.channels = 2;
    if (path.compare(0, 5, "tone:") == 0) std::sscanf(path.c_str() + 5, "%lf:%lf", &r->freq, &r->until);
    else if (path.compare(0, 5, "harm:") == 0) { r->kind = 1; std::sscanf(path.c_str() + 5, "%lf:%lf:%lf", &r->freq, &r->formant, &r->until); }
    else if (path.compare(0, 5, "beat:") == 0) { r->kind = 2; std::sscanf(path.c_str() + 5, "%lf", &r->until); }
    else { if (error) *error = "unknown"; return nullptr; }
    return std::unique_ptr<MediaReader>(r.release());
}

struct Setup {
    Project p;
    uint64_t track = 0, event = 0;
    explicit Setup(const std::string& media)
    {
        p.settings.sampleRate = kRate;
        // (ids never repeat between set-ups: the engine keeps readers and state by them)
        static uint64_t next = 100;
        p.nextId = next;
        next += 100;
        MediaItem m;
        m.id = p.newId();
        m.path = m.name = media;
        std::string e;
        m.info = synthOpen(media, &e)->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        track = t.id;
        Event ev;
        ev.id = p.newId();
        ev.mediaId = m.id;
        ev.start = 0;
        ev.length = 10;
        ev.loop = false;
        event = ev.id;
        t.events.push_back(ev);
        p.tracks.push_back(t);
    }
    Event& ev() { return *p.findEvent(event); }
};

static void play(AudioEngine& audio, const Project& p, double from, int n, std::vector<float>& l, std::vector<float>& r, int block)
{
    l.assign((size_t)n, 0.f);
    r.assign((size_t)n, 0.f);
    const int64_t s0 = (int64_t)std::llround(from * kRate);
    for (int at = 0; at < n; at += block) audio.render(p, s0 + at, std::min(block, n - at), l.data() + at, r.data() + at, true);
}

// frequency of a steady tone from its first and last upward zero crossings
static double frequency(const float* s, size_t n)
{
    double first = -1, last = -1;
    int count = 0;
    for (size_t k = 1; k < n; ++k)
        if (s[k - 1] < 0 && s[k] >= 0) {
            const double t = (double)(k - 1) + s[k - 1] / (s[k - 1] - s[k]);
            if (first < 0) first = t;
            last = t;
            ++count;
        }
    return count < 2 ? 0 : (count - 1) * (double)kRate / (last - first);
}

// the fundamental of a harmonic sound (autocorrelation peak between 50 and 1000 Hz, refined)
static double fundamental(const float* s, size_t n)
{
    const int lo = kRate / 1000, hi = kRate / 50;
    const size_t w = std::min(n - (size_t)hi - 1, (size_t)8192);
    double best = -1;
    int bt = 0;
    std::vector<double> c((size_t)hi + 2, 0.0);
    double e0 = 0;
    for (size_t j = 0; j < w; ++j) e0 += (double)s[j] * s[j];
    for (int t = lo; t <= hi + 1; ++t) {
        double dot = 0, et = 0;
        for (size_t j = 0; j < w; ++j) {
            dot += (double)s[j] * s[j + (size_t)t];
            et += (double)s[j + (size_t)t] * s[j + (size_t)t];
        }
        c[(size_t)t] = dot / std::sqrt(e0 * et + 1e-12);
    }
    // the first peak that is nearly as high as the highest (not a multiple of the period)
    double top = 0;
    for (int t = lo; t <= hi; ++t) top = std::max(top, c[(size_t)t]);
    for (int t = lo + 1; t < hi; ++t)
        if (c[(size_t)t] > 0.9 * top && c[(size_t)t] >= c[(size_t)t - 1] && c[(size_t)t] >= c[(size_t)t + 1]) { bt = t; best = c[(size_t)t]; break; }
    if (best < 0) return 0;
    const double a = c[(size_t)bt - 1], b = c[(size_t)bt], d = c[(size_t)bt + 1];
    const double den = a - 2 * b + d;
    const double off = std::fabs(den) > 1e-12 ? 0.5 * (a - d) / den : 0;
    return kRate / (bt + off);
}

static double amplitudeAt(const float* s, size_t n, double f)
{
    double re = 0, im = 0;
    for (size_t k = 0; k < n; ++k) {
        const double w = 0.5 - 0.5 * std::cos(2.0 * kPi * k / n);
        re += w * s[k] * std::cos(2.0 * kPi * f * k / kRate);
        im += w * s[k] * std::sin(2.0 * kPi * f * k / kRate);
    }
    return 4.0 * std::sqrt(re * re + im * im) / n;
}

// where the formant (the strongest harmonic region) of a harmonic sound with fundamental f0 is: the
// amplitude-weighted mean of the harmonics' frequencies, squared amplitudes
static double formantOf(const float* s, size_t n, double f0)
{
    double num = 0, den = 0;
    for (int h = 1; h * f0 < 5000; ++h) {
        const double a = amplitudeAt(s, n, h * f0);
        num += h * f0 * a * a * a * a;
        den += a * a * a * a;
    }
    return den > 0 ? num / den : 0;
}

static double rmsOf(const float* s, size_t n)
{
    double e = 0;
    for (size_t k = 0; k < n; ++k) e += (double)s[k] * s[k];
    return n ? std::sqrt(e / n) : 0;
}

// largest sample-to-sample step relative to a clean sine of that frequency and level
static double stepRatio(const float* s, size_t n, double f, double amp)
{
    double m = 0;
    for (size_t k = 1; k < n; ++k) m = std::max(m, (double)std::fabs(s[k] - s[k - 1]));
    return m / (amp * 2.0 * kPi * f / kRate);
}

struct Method { const char* method; const char* mode; const char* label; };

int main()
{
    // the engine reads made-up media
    FxRuntime fx;
    MediaPool pool;
    pool.setOpener(synthOpen);
    AudioEngine audio(pool, fx);

    const Method methods[] = {
        {"elastique", "pro", "elastique Pro"},
        {"elastique", "efficient", "elastique Efficient"},
        {"elastique", "soloist-mono", "elastique Soloist (Monophonic)"},
        {"elastique", "soloist-speech", "elastique Soloist (Speech)"},
        {"classic", "A01", "Classic A01 Music 1"},
        {"classic", "A03", "Classic A03 Music 3"},
        {"classic", "A07", "Classic A07 Speech 1"},
        {"classic", "A17", "Classic A17 Drums"},
    };
    const double rates[] = {0.5, 0.8, 1.25, 2, 4};

    std::printf("\nMeasurements (48 kHz). Pitch: a 440 Hz sine (Soloist modes: a 200 Hz buzz) kept at each rate,\n"
                "error in cents; buzz: a 150 Hz buzz with a formant, its fundamental's error in cents.\n"
                "Duration: a 2 s sound at that rate must end at 2/rate s; error in ms. Step: steepest sample step / a clean sine's.\n\n");
    std::printf("%-32s %6s %10s %10s %10s %8s\n", "method", "rate", "cents", "buzz cents", "end (ms)", "step");
    double worstCents = 0, worstEnd = 0;
    for (const Method& mt : methods) {
        const bool solo = std::strncmp(mt.mode, "soloist", 7) == 0;
        for (double rate : rates) {
            // pitch
            Setup a(solo ? "harm:200:1000:30" : "tone:440:30");
            a.ev().rate = rate;
            a.ev().keepPitch = true;
            a.ev().stretchMethod = mt.method;
            a.ev().stretchMode = mt.mode;
            a.ev().length = 8.0 / rate + 1;
            std::vector<float> l, r;
            play(audio, a.p, 1.0, kRate, l, r, 1024);
            const double f = solo ? fundamental(l.data(), l.size()) : frequency(l.data(), l.size());
            const double want = solo ? 200.0 : 440.0;
            const double cents = f > 0 ? 1200.0 * std::log2(f / want) : 9999;
            const double step = solo ? 0 : stepRatio(l.data(), l.size(), 440, 0.5);
            // and a buzz (harmonics of 150 Hz, a formant at 1 kHz) for every method
            Setup c("harm:150:1000:30");
            c.ev().rate = rate;
            c.ev().keepPitch = true;
            c.ev().stretchMethod = mt.method;
            c.ev().stretchMode = mt.mode;
            c.ev().length = 8.0 / rate + 1;
            std::vector<float> cl, cr;
            play(audio, c.p, 1.0, kRate, cl, cr, 1024);
            const double fb = fundamental(cl.data(), cl.size());
            const double buzzCents = fb > 0 ? 1200.0 * std::log2(fb / 150.0) : 9999;
            // duration: the sound lasts 2 s of media
            Setup b(solo ? "harm:200:1000:2" : "tone:440:2");
            b.ev().rate = rate;
            b.ev().keepPitch = true;
            b.ev().stretchMethod = mt.method;
            b.ev().stretchMode = mt.mode;
            b.ev().length = 2.0 / rate + 1.0;
            const int total = (int)std::lround((2.0 / rate + 0.6) * kRate);
            std::vector<float> bl, br;
            play(audio, b.p, 0, total, bl, br, 1024);
            // where the level (10 ms windows) last falls below half its steady level
            const double steady = rmsOf(bl.data() + (size_t)(0.1 * kRate), (size_t)(0.5 * kRate / std::max(1.0, rate)));
            const int win = kRate / 100;
            double endAt = 0;
            for (int at = 0; at + win <= total; at += win / 4)
                if (rmsOf(bl.data() + at, (size_t)win) > 0.5 * steady) endAt = (at + win * 0.5) / kRate;
            const double endErr = (endAt - 2.0 / rate) * 1000.0;
            std::printf("%-32s %6.2f %10.2f %10.2f %10.1f %8.2f\n", mt.label, rate, cents, buzzCents, endErr, step);
            check(std::fabs(buzzCents) < 15.0, strfmt("%s at %.2fx keeps a buzz's pitch", mt.label, rate), strfmt("%.2f cents", buzzCents));
            worstCents = std::max(worstCents, std::fabs(cents));
            worstEnd = std::max(worstEnd, std::fabs(endErr));
            const double maxCents = solo ? 15.0 : 2.0;
            check(std::fabs(cents) < maxCents, strfmt("%s at %.2fx keeps the pitch", mt.label, rate), strfmt("%.2f cents", cents));
            // (Classic lays whole splices down, centred where the stretch says: a splice can run on past the end by up to
            // half its length, VEGAS's "may echo")
            const ClassicPreset* cp = std::strcmp(mt.method, "classic") == 0 ? classicPreset(mt.mode) : nullptr;
            const double allowed = cp ? cp->halfSpliceMs + 30.0 : 40.0;
            check(std::fabs(endErr) < allowed, strfmt("%s at %.2fx: the sound ends on time", mt.label, rate), strfmt("%.1f ms (allowed %.0f)", endErr, allowed));
            if (!solo) check(step < 1.6, strfmt("%s at %.2fx: no clicks", mt.label, rate), strfmt("steepest step %.2fx a clean sine's", step));
        }
    }
    std::printf("INFO worst pitch error %.2f cents, worst end error %.1f ms\n\n", worstCents, worstEnd);

    // ---------------------------------------------------------------- blocks, seeks, reverse
    for (const Method& mt : methods) {
        Setup a("beat:30");
        a.ev().rate = 1.37;
        a.ev().keepPitch = true;
        a.ev().pitch = 2;
        a.ev().stretchMethod = mt.method;
        a.ev().stretchMode = mt.mode;
        std::vector<float> l1, r1, l2, r2, l3, r3;
        play(audio, a.p, 0.5, 60000, l1, r1, 60000);
        play(audio, a.p, 0.5, 60000, l2, r2, 441);
        play(audio, a.p, 0.5, 60000, l3, r3, 4096);
        double worst = 0;
        for (size_t k = 0; k < l1.size(); ++k) worst = std::max({worst, (double)std::fabs(l1[k] - l2[k]), (double)std::fabs(r1[k] - r3[k])});
        check(worst < 1e-6, strfmt("%s: the same sound whatever the block size (no joins)", mt.label), strfmt("largest difference %g", worst));

        // a seek: the stretcher starts again, at full level straight away
        Setup s(std::strncmp(mt.mode, "soloist", 7) == 0 ? "harm:200:1000:30" : "tone:440:30");
        s.ev().rate = 1.5;
        s.ev().keepPitch = true;
        s.ev().stretchMethod = mt.method;
        s.ev().stretchMode = mt.mode;
        std::vector<float> j1, j2, c1, c2;
        play(audio, s.p, 4.0, 2 * kRate, c1, c2, 1024);
        const double steadyLevel = rmsOf(c1.data() + kRate, (size_t)kRate);
        play(audio, s.p, 1.0, 4800, j1, j2, 4800);
        play(audio, s.p, 5.0, 9600, j1, j2, 512);
        const double lvl = rmsOf(j1.data(), 2400);
        check(lvl > 0.8 * steadyLevel, strfmt("%s: after a seek the sound is there at once", mt.label),
              strfmt("level %.3f in the first 50 ms (steady %.3f)", lvl, steadyLevel));

        // reversed, stretched
        Setup v("tone:440:30");
        v.ev().rate = 0.8;
        v.ev().keepPitch = true;
        v.ev().reverse = true;
        v.ev().stretchMethod = mt.method;
        v.ev().stretchMode = mt.mode;
        std::vector<float> vl, vr;
        play(audio, v.p, 2.0, kRate, vl, vr, 777);
        const double fv = frequency(vl.data(), vl.size());
        check(std::fabs(1200.0 * std::log2(fv / 440.0)) < 15.0, strfmt("%s: a reversed stretched event keeps its pitch", mt.label), strfmt("%.2f Hz", fv));
    }

    // ---------------------------------------------------------------- pitch change, lock to stretch, None
    {
        const double semis[] = {-7, -3, 2, 5, 12};
        for (const Method& mt : methods) {
            const bool solo = std::strncmp(mt.mode, "soloist", 7) == 0;
            double worst = 0;
            for (double st : semis) {
                Setup a(solo ? "harm:200:1000:30" : "tone:440:30");
                a.ev().rate = 1.0;
                a.ev().keepPitch = true;
                a.ev().pitch = st;
                a.ev().stretchMethod = mt.method;
                a.ev().stretchMode = mt.mode;
                std::vector<float> l, r;
                play(audio, a.p, 1.0, kRate, l, r, 1000);
                const double want = (solo ? 200.0 : 440.0) * std::pow(2.0, st / 12.0);
                const double f = solo ? fundamental(l.data(), l.size()) : frequency(l.data(), l.size());
                worst = std::max(worst, std::fabs(1200.0 * std::log2(f / want)));
            }
            check(worst < (solo ? 15.0 : 2.0), strfmt("%s: pitch change -7..+12 semitones", mt.label), strfmt("worst %.2f cents", worst));
        }
        // lock to stretch (keepPitch off): the rate's pitch, like tape
        Setup a("tone:440:30");
        a.ev().rate = 2.0;
        a.ev().keepPitch = false;
        std::vector<float> l, r;
        play(audio, a.p, 1.0, kRate / 2, l, r, 1000);
        check(std::fabs(frequency(l.data(), l.size()) - 880) < 0.5, "lock to stretch: double speed is an octave up", strfmt("%.2f Hz", frequency(l.data(), l.size())));
        // None: no stretching, no pitch change - even when asked to keep the pitch
        a.ev().stretchMethod = "none";
        a.ev().keepPitch = true;
        a.ev().pitch = 5;
        play(audio, a.p, 1.0, kRate / 2, l, r, 1000);
        check(std::fabs(frequency(l.data(), l.size()) - 880) < 0.5, "method None: the rate changes the pitch, the pitch change is not used",
              strfmt("%.2f Hz", frequency(l.data(), l.size())));
        // invert phase
        a.ev().stretchMethod = "elastique";
        a.ev().keepPitch = false;
        a.ev().pitch = 0;
        a.ev().rate = 1.0;
        std::vector<float> l2, r2;
        play(audio, a.p, 1.0, 4800, l, r, 1000);
        a.ev().invertPhase = true;
        play(audio, a.p, 1.0, 4800, l2, r2, 1000);
        double sum = 0;
        for (size_t k = 0; k < l.size(); ++k) sum = std::max(sum, (double)std::fabs(l[k] + l2[k]));
        check(sum < 1e-6 && rmsOf(l.data(), l.size()) > 0.2, "invert phase turns the sound upside down", strfmt("%g", sum));
    }

    // ---------------------------------------------------------------- formants
    {
        // a buzz at 200 Hz with its formant at 1200 Hz, seven semitones up
        auto measure = [&](const char* mode, bool preserve, double formantShift, double& f0, double& fm) {
            Setup a("harm:200:1200:30");
            a.ev().rate = 1.0;
            a.ev().keepPitch = true;
            a.ev().pitch = 7;
            a.ev().stretchMethod = "elastique";
            a.ev().stretchMode = mode;
            a.ev().preserveFormants = preserve;
            a.ev().formant = formantShift;
            std::vector<float> l, r;
            play(audio, a.p, 2.0, 16384, l, r, 1024);
            f0 = fundamental(l.data(), l.size());
            fm = formantOf(l.data(), l.size(), f0);
        };
        double f0, fm;
        const double up = 1200.0 * std::pow(2.0, 7 / 12.0);
        measure("pro", false, 0, f0, fm);
        std::printf("INFO Pro +7 st, formants not kept: f0 %.1f Hz, formant %.0f Hz (moves with the pitch to %.0f)\n", f0, fm, up);
        check(std::fabs(fm - up) < 200, "Pro without formant preservation: the formant moves with the pitch", strfmt("%.0f Hz", fm));
        measure("pro", true, 0, f0, fm);
        std::printf("INFO Pro +7 st, formants kept: f0 %.1f Hz, formant %.0f Hz (was 1200)\n", f0, fm);
        check(std::fabs(fm - 1200) < 200 && std::fabs(1200.0 * std::log2(f0 / (200.0 * std::pow(2.0, 7 / 12.0)))) < 15,
              "Pro with formant preservation: the pitch moves, the formant stays", strfmt("f0 %.1f Hz, formant %.0f Hz", f0, fm));
        measure("pro", true, -5, f0, fm);
        const double down = 1200.0 * std::pow(2.0, -5 / 12.0);
        std::printf("INFO Pro +7 st, formants kept and shifted -5 st: formant %.0f Hz (want %.0f)\n", fm, down);
        check(std::fabs(fm - down) < 180, "Pro formant shift -5 semitones moves the formant down", strfmt("%.0f Hz (want %.0f)", fm, down));
        measure("soloist-mono", true, 0, f0, fm);
        std::printf("INFO Soloist (Monophonic) +7 st, formants kept: f0 %.1f Hz, formant %.0f Hz\n", f0, fm);
        check(std::fabs(fm - 1200) < 250 && std::fabs(1200.0 * std::log2(f0 / (200.0 * std::pow(2.0, 7 / 12.0)))) < 20,
              "Soloist (Monophonic) with formant preservation keeps the formant", strfmt("f0 %.1f Hz, formant %.0f Hz", f0, fm));
        measure("soloist-speech", false, 0, f0, fm);
        std::printf("INFO Soloist (Speech) +7 st: f0 %.1f Hz, formant %.0f Hz\n", f0, fm);
        check(std::fabs(fm - 1200) < 250, "Soloist (Speech) keeps a voice's formant", strfmt("%.0f Hz", fm));
        // VEGAS: no formant shift for Efficient (nothing changes)
        measure("efficient", true, -5, f0, fm);
        check(std::fabs(fm - up) < 200, "Efficient has no formant settings (the formant moves with the pitch)", strfmt("%.0f Hz", fm));
    }

    // ---------------------------------------------------------------- processor time
    {
        std::printf("\nProcessor time per second of stereo sound (this machine, sanitizer build), stretching 1.25x:\n");
        for (const Method& mt : methods) {
            Setup a("beat:60");
            a.ev().rate = 1.25;
            a.ev().keepPitch = true;
            a.ev().stretchMethod = mt.method;
            a.ev().stretchMode = mt.mode;
            a.ev().length = 40;
            std::vector<float> l, r;
            const auto t0 = std::chrono::steady_clock::now();
            play(audio, a.p, 0, kRate * 10, l, r, 1024);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            // the same without stretching (reading and mixing alone)
            a.ev().keepPitch = false;
            const auto t1 = std::chrono::steady_clock::now();
            play(audio, a.p, 0, kRate * 10, l, r, 1024);
            const double base = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
            std::printf("  %-32s %7.1f ms per second (%.1f ms without stretching)\n", mt.label, (secs) * 100.0, base * 100.0);
            check(secs < 10.0, strfmt("%s runs faster than real time", mt.label), strfmt("%.2f s for 10 s", secs));
        }
    }

    // ---------------------------------------------------------------- the presets' table
    check(classicPresets().size() == 19 && classicPreset("A01") && classicPreset("A19") && !classicPreset("A20"), "19 Classic presets, A01 to A19");
    check(elastiqueModes().size() == 4, "four elastique modes");

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
