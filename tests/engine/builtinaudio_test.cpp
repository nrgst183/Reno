// Tests of the built-in audio effects (src/audiofx/builtinaudio.cpp): creation through createAudioPlugin, the catalog,
// output sanity, state save / load, presets, Pitch Shift accuracy, delay timing, hard clipping, bad input.
//
//   builtinaudio_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/engine/catalog.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static const double kSr = 48000;

static std::unique_ptr<AudioPlugin> make(const std::string& uid)
{
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = uid;
    std::string err;
    std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &err);
    if (p) p->activate(kSr, 512);
    return p;
}

// runs 'secs' of a sine through it in blocks of 512; returns the left channel
static std::vector<float> run(AudioPlugin& p, double freq, double secs, float amp = 0.5f)
{
    const int n = (int)(secs * kSr);
    std::vector<float> l(n), r(n);
    for (int k = 0; k < n; ++k) l[k] = r[k] = amp * (float)std::sin(2 * 3.14159265358979 * freq * k / kSr);
    for (int at = 0; at < n; at += 512) {
        const int m = std::min(512, n - at);
        p.process(l.data() + at, r.data() + at, m, at, true);
    }
    return l;
}

// the strongest frequency between lo and hi (Goertzel, 1 Hz steps) in the last half of x
static double peakFreq(const std::vector<float>& x, double lo, double hi)
{
    const size_t from = x.size() / 2;
    double best = 0, bestF = 0;
    for (double f = lo; f <= hi; f += 1.0) {
        const double w = 2 * 3.14159265358979 * f / kSr, c = 2 * std::cos(w);
        double s1 = 0, s2 = 0;
        for (size_t k = from; k < x.size(); ++k) {
            const double s0 = x[k] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (pw > best) { best = pw; bestF = f; }
    }
    return bestF;
}

static double rms(const std::vector<float>& x)
{
    double s = 0;
    for (float v : x) s += (double)v * v;
    return x.empty() ? 0 : std::sqrt(s / x.size());
}

static int setByName(AudioPlugin& p, const std::string& name, double value, double min, double max)
{
    for (int k = 0; k < p.paramCount(); ++k)
        if (p.paramName(k) == name) { p.paramSet(k, (float)((value - min) / (max - min))); return k; }
    return -1;
}

int main()
{
    const std::vector<AudioPluginDesc> all = builtinAudioPlugins();
    // (the exact reproductions of VEGAS's track effects, "reno.trk...", have their own tests: vegasfx_<name>_test)
    const auto isTrk = [](const std::string& uid) { return uid.compare(0, 8, "reno.trk") == 0; };
    // (so are the ExpressFX ones of sfxpfx2, "reno.xp...": vegasfx_xp<name>_test; they get the generic checks below, not the count)
    const auto isXp = [](const std::string& uid) { return uid.compare(0, 7, "reno.xp") == 0; };
    size_t standIns = 0;
    for (const AudioPluginDesc& d : all) if (!isTrk(d.uid) && !isXp(d.uid)) ++standIns;
    check(standIns == 20, "list: twenty built-in audio effects besides the track and ExpressFX ones (Time Stretch added)", std::to_string(standIns));
    for (const AudioPluginDesc& d : all) {
        if (isTrk(d.uid)) continue;
        std::unique_ptr<AudioPlugin> p = make(d.uid);
        check(p != nullptr && p->desc.name == d.name && p->desc.vendor == kBuiltinAudioVendor, d.name + ": made");
        if (!p) continue;
        bool finite = true;
        std::vector<float> out = run(*p, 440, 1.0);
        for (float v : out) if (!std::isfinite(v)) finite = false;
        check(finite && rms(out) > 0.01, d.name + ": processes (finite, not silent)", std::to_string(rms(out)));
        check(p->paramCount() >= 3 && !p->paramName(0).empty() && !p->paramText(0).empty(), d.name + ": parameters named and worded");
        // settings survive a save and a load into a new one
        for (int k = 0; k < p->paramCount(); ++k) p->paramSet(k, 0.37f);
        const std::string st = p->saveState();
        std::unique_ptr<AudioPlugin> q = make(d.uid);
        bool same = q && q->loadState(st);
        for (int k = 0; same && k < p->paramCount(); ++k)
            if (std::fabs(p->paramGet(k) - q->paramGet(k)) > 0.02f) same = false;
        check(same, d.name + ": state round trip");
        check(q && !q->loadState("garbage") && !q->loadState(""), d.name + ": a foreign state is refused");
        // presets
        bool presets = p->programCount() > 0;
        for (int k = 0; k < p->programCount(); ++k) presets = presets && !p->programName(k).empty() && p->setProgram(k) && p->currentProgram() == k;
        check(presets && !p->setProgram(-1) && !p->setProgram(999), d.name + ": presets");
        // bad values: NaN settings ignored, out of range clamped, NaN / huge input does not poison what follows
        p->paramSet(0, std::nanf(""));
        p->paramSet(1, 7.f);
        p->paramSet(-1, 0.5f);
        p->paramSet(999, 0.5f);
        check(p->paramGet(1) <= 1.0001f && p->paramGet(1) >= 0.f && p->paramName(999).empty(), d.name + ": bad parameter values");
        std::vector<float> l(512, std::nanf("")), r(512, 1e30f);
        p->process(l.data(), r.data(), 512, 0, true);
        bool ok = true;
        for (int k = 0; k < 512; ++k) if (!std::isfinite(l[k]) || !std::isfinite(r[k])) ok = false;
        p->process(nullptr, nullptr, 512, 512, true);
        p->process(l.data(), r.data(), 0, 512, true);
        check(ok, d.name + ": NaN and huge input give finite output");
        // not activated: still safe
        std::unique_ptr<AudioPlugin> raw = builtinAudioCreate(d, nullptr);
        std::vector<float> a(256, 0.1f), b(256, 0.1f);
        raw->process(a.data(), b.data(), 256, 0, true);
        check(std::isfinite(a[100]), d.name + ": process before activate");
    }
    check(builtinAudioCreate(AudioPluginDesc(), nullptr) == nullptr, "unknown uid refused");

    // the catalog lists them
    Catalog::get().scanInProcess({}, {});
    int listed = 0;
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.format == "BUILTIN" && e.ok && !isTrk(e.desc.uid) && !isXp(e.desc.uid)) ++listed;
    check(listed == 9, "catalog: listed", std::to_string(listed));

    // Pitch Shift (the exact reproduction of VEGAS's, with Preserve duration the sound trails by its latency): 440 Hz up 12
    // -> 880, down 7 -> 293.7, +3 semitones 50 cents -> 538.8
    {
        auto p = make("reno.pitchshift");
        setByName(*p, "Preserve Duration", 1, 0, 1);
        setByName(*p, "Semitones", 12, -50, 50);
        const double f = peakFreq(run(*p, 440, 2.5), 200, 1200);
        check(std::fabs(f / 880 - 1) <= 0.01, "pitch shift: an octave up", std::to_string(f));
        check(p->latencySamples() > 20000, "pitch shift: reports its latency", std::to_string(p->latencySamples()));
        auto q = make("reno.pitchshift");
        setByName(*q, "Preserve Duration", 1, 0, 1);
        setByName(*q, "Semitones", -7, -50, 50);
        const double g = peakFreq(run(*q, 440, 2.0), 200, 1200);
        check(std::fabs(g / 293.66 - 1) <= 0.01, "pitch shift: a fifth down", std::to_string(g));
        auto c = make("reno.pitchshift");
        setByName(*c, "Preserve Duration", 1, 0, 1);
        setByName(*c, "Semitones", 3, -50, 50);
        setByName(*c, "Cents", 50, -100, 100);
        const double h = peakFreq(run(*c, 440, 2.0), 200, 1200);
        check(std::fabs(h / (440 * std::pow(2.0, 3.5 / 12)) - 1) <= 0.01, "pitch shift: semitones and cents", std::to_string(h));
        // 0: the tone as it was, its level kept
        auto z = make("reno.pitchshift");
        setByName(*z, "Preserve Duration", 1, 0, 1);
        std::vector<float> o = run(*z, 440, 2.0);
        check(std::fabs(peakFreq(o, 200, 1200) - 440) <= 2 && std::fabs(rms(o) - 0.5 / std::sqrt(2.0)) < 0.05, "pitch shift: 0 leaves the tone");
        // the box unticked: Reno still keeps the length (see PitchEngine), so the same latency and a tone all the way
        auto w = make("reno.pitchshift");
        setByName(*w, "Semitones", 12, -50, 50);
        check(w->latencySamples() > 20000, "pitch shift: without Preserve duration the sound is still held in step");
        check(std::fabs(peakFreq(run(*w, 440, 2.5), 200, 1200) / 880 - 1) <= 0.01, "pitch shift: an octave up without Preserve duration");
    }
    // Simple Delay: an impulse comes back after the delay time
    {
        auto p = make("reno.simpledelay");
        setByName(*p, "Delay Time", 100, 1, 5000);
        setByName(*p, "Multiple Delays", 0, 0, 1);
        std::vector<float> l(9600, 0.f), r(9600, 0.f);
        l[0] = r[0] = 1.f;
        for (int at = 0; at < 9600; at += 480) p->process(l.data() + at, r.data() + at, 480, at, true);
        int peak = 1;
        for (int k = 1; k < 9600; ++k) if (std::fabs(l[k]) > std::fabs(l[peak])) peak = k;
        check(std::abs(peak - 4800) <= 2 && std::fabs(l[peak] - 0.5f) < 0.02f, "delay: the echo 100 ms later at -6 dB", std::to_string(peak));
        // a jump in time (a seek) starts it afresh: no echo of before
        std::vector<float> s(4800, 0.f), t(4800, 0.f);
        p->process(s.data(), t.data(), 4800, 100000, true);
        check(rms(s) < 1e-6, "delay: a seek clears the echoes");
    }
    // Distortion: hard clip stays within its output level
    {
        auto p = make("reno.distortion");
        setByName(*p, "Shape", 1, 0, 3);
        setByName(*p, "Drive", 40, 0, 60);
        setByName(*p, "Effect out", 0, -60, 24);
        setByName(*p, "Tone", 20000, 500, 20000);
        std::vector<float> o = run(*p, 200, 0.5, 0.9f);
        float mx = 0;
        for (float v : o) mx = std::max(mx, std::fabs(v));
        check(mx <= 1.05f && rms(o) > 0.8, "distortion: hard clip squares the tone within +-1", std::to_string(mx) + " " + std::to_string(rms(o)));
        // Bit crush to 1 bit: two levels only
        auto b = make("reno.distortion");
        b->setProgram(3);       // Bit crusher
        setByName(*b, "Crush bits", 1, 1, 16);
        setByName(*b, "Tone", 20000, 500, 20000);
        std::vector<float> c = run(*b, 200, 0.5, 0.9f);
        check(rms(c) > 0.1, "distortion: bit crush sounds");
    }
    // Amplitude modulation at 100% depth, square: half the time silent
    {
        auto p = make("reno.ampmod");
        p->setProgram(5);       // Hard square
        setByName(*p, "Amplitude", -80, -80, 0);
        setByName(*p, "Modulation Frequency", 4, 0.001, 5000);
        std::vector<float> o = run(*p, 1000, 1.0);
        int quiet = 0;
        for (size_t k = 0; k < o.size(); k += 480) {
            float m = 0;
            for (size_t j = k; j < std::min(o.size(), k + 480); ++j) m = std::max(m, std::fabs(o[j]));
            if (m < 1e-3) ++quiet;
        }
        check(quiet >= 40 && quiet <= 60, "amplitude modulation: a square gate", std::to_string(quiet));
    }
    // Tempo sync follows the project's tempo (ProjectTempoScope, which the mixer puts around every effect chain): a square
    // gate that follows a quarter note changes 8 times in 2 seconds at 120 BPM, 4 times at 60 and 16 at 240.
    {
        auto transitions = [&](double bpm) {
            auto p = make("reno.ampmod");
            p->setProgram(5);       // Hard square
            setByName(*p, "Amplitude", -80, -80, 0);
            setByName(*p, "Modulation Period", 13, 0, 18);          // Quarter Note
            setByName(*p, "Modulation Period Multiplier", 1, 0.001, 32);
            setByName(*p, "Tempo Sync", 1, 0, 1);
            const ProjectTempoScope scope(bpm, 4);
            std::vector<float> o = run(*p, 1000, 2.0);
            int last = -1, n = 0;
            for (size_t k = 0; k + 480 <= o.size(); k += 480) {
                float m = 0;
                for (size_t j = k; j < k + 480; ++j) m = std::max(m, std::fabs(o[j]));
                const int quiet = m < 1e-3 ? 1 : 0;
                if (last >= 0 && quiet != last) ++n;
                last = quiet;
            }
            return n;
        };
        const int a = transitions(120.0), b = transitions(60.0), c = transitions(240.0);
        check(a >= 7 && a <= 9 && b >= 3 && b <= 5 && c >= 15 && c <= 17, "amplitude modulation: tempo sync follows the project's tempo", std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(c));
    }
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
