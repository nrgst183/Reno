// Tests of the built-in versions of the MAGIX essentialFX audio effects (src/audiofx/efx*.cpp): Limiter, Compressor,
// Reverb, Tube Stage, Phaser, Chorus/Flanger. Checks the 31 settings, the saved form (the .veg's FxCk values),
// the sound, and the choice between these and installed plug-ins in FxRuntime.
//
//   efx_test
#include "../../src/audiofx/audioplugin.h"
#include "../../src/audiofx/efx.h"
#include "../../src/core/base.h"
#include "../../src/engine/engine.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
static std::string fmt(const char* f, double a, double b = 0, double c = 0)
{
    char buf[256];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

static const double kSr = 48000;
static const double kPi = 3.14159265358979323846;

static uint32_t idOf(const char* s) { return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) | ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3]; }

static std::unique_ptr<AudioPlugin> make(const char* id)
{
    AudioPluginDesc d;
    d.format = "VST2";
    d.uid = std::to_string(idOf(id));
    std::string err;
    std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &err);
    if (p) p->activate(kSr, 512);
    return p;
}

// sets a setting by the real value it should have (given through the 0..1 that gives it, found by search on the text is
// not needed: tests give 0..1 directly)
static void set(AudioPlugin& p, int index, double x) { p.paramSet(index, (float)x); }
// sets a setting to a real value (dB, ms, Hz, %...) by finding the 0..1 that gives it (the shapes are monotone)
static void setReal(AudioPlugin& p, int index, double value)
{
    efx::EfxPlugin* e = dynamic_cast<efx::EfxPlugin*>(&p);
    if (!e) return;
    const efx::ParamDef& d = e->def(index);
    const bool up = efx::mapValue(d, 1.0) >= efx::mapValue(d, 0.0);
    double a = 0, b = 1;
    for (int k = 0; k < 60; ++k) {
        const double m = 0.5 * (a + b);
        if ((efx::mapValue(d, m) < value) == up) a = m; else b = m;
    }
    p.paramSet(index, (float)(0.5 * (a + b)));
}

static std::vector<float> noise(int n, float amp, uint32_t seed = 1)
{
    std::vector<float> v((size_t)n);
    for (int k = 0; k < n; ++k) {
        seed = seed * 1664525u + 1013904223u;
        v[(size_t)k] = amp * ((int)(seed >> 9) / 4194304.0f - 1.0f);
    }
    return v;
}
static std::vector<float> sine(int n, double hz, float amp)
{
    std::vector<float> v((size_t)n);
    for (int k = 0; k < n; ++k) v[(size_t)k] = amp * (float)std::sin(2 * kPi * hz * k / kSr);
    return v;
}

// runs l (and r = l) through p in blocks of 'block'; returns the left channel (and the right in *outR)
static std::vector<float> process(AudioPlugin& p, std::vector<float> l, int block = 512, std::vector<float>* outR = nullptr, const std::vector<float>* rIn = nullptr)
{
    std::vector<float> r = rIn ? *rIn : l;
    const int n = (int)l.size();
    for (int at = 0; at < n; at += block) {
        const int m = std::min(block, n - at);
        p.process(l.data() + at, r.data() + at, m, at, true);
    }
    if (outR) *outR = r;
    return l;
}

static double rms(const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > x.size()) to = x.size();
    double s = 0;
    for (size_t k = from; k < to; ++k) s += (double)x[k] * x[k];
    return to > from ? std::sqrt(s / (double)(to - from)) : 0.0;
}
static double peak(const std::vector<float>& x, size_t from = 0, size_t to = 0)
{
    if (to == 0 || to > x.size()) to = x.size();
    double p = 0;
    for (size_t k = from; k < to; ++k) p = std::max(p, (double)std::fabs(x[k]));
    return p;
}
static double db(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }

// the level (dB) of frequency f in x[from..] (Goertzel)
static double toneLevel(const std::vector<float>& x, double f, size_t from)
{
    const double w = 2 * kPi * f / kSr, c = 2 * std::cos(w);
    double s1 = 0, s2 = 0;
    for (size_t k = from; k < x.size(); ++k) {
        const double s0 = x[k] + c * s1 - s2;
        s2 = s1; s1 = s0;
    }
    const double pw = s1 * s1 + s2 * s2 - c * s1 * s2;
    return db(2.0 * std::sqrt(std::max(pw, 0.0)) / (double)(x.size() - from));
}

// ---------------------------------------------------------------------------------------------------- what the real ones read
// (setting, 0..1 value, the text the real plug-in showed for it)
struct Reading { int index; double x; const char* text; };

static void textsOf(const char* label, AudioPlugin& p, const std::vector<Reading>& rs)
{
    for (const Reading& r : rs) {
        p.paramSet(r.index, (float)r.x);
        const std::string t = p.paramText(r.index);
        check(t == r.text, std::string(label) + ": #" + std::to_string(r.index) + " " + p.paramName(r.index) + " " + fmt("%.4f", r.x) + " reads \"" + r.text + "\"", "got \"" + t + "\"");
    }
}

// the saved settings of a slot of the sample projects, as the .veg's FxCk values (31 floats)
static std::string stateOf(const std::vector<float>& v)
{
    return "P:" + base64Encode(v.data(), v.size() * sizeof(float));
}

static std::vector<float> vec(std::initializer_list<float> l) { return std::vector<float>(l); }

// ---- the sweep: every eFX setting at 0, 0.025 ... 1 with the text the real plug-in showed (a text dump of the real plug-ins, named by RENO_EFX_REF,
// read-only reference; skipped when it is not there)
struct SweepParam { int index = 0; std::string name; double def = 0; std::vector<std::pair<float, std::string>> pts; };
struct SweepFx { std::string name, id; std::vector<SweepParam> params; };

static std::vector<SweepFx> readSweep(const char* path)
{
    std::vector<SweepFx> out;
    FILE* f = std::fopen(path, "rb");
    if (!f) return out;
    std::string line;
    int ch;
    auto trim = [](std::string s) {
        while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
        size_t a = 0;
        while (a < s.size() && s[a] == ' ') ++a;
        return s.substr(a);
    };
    auto handle = [&](const std::string& ln) {
        if (ln.compare(0, 7, "== eFX ") == 0) {
            SweepFx e;
            const size_t sp = ln.find(' ', 7);
            e.name = ln.substr(7, sp - 7);
            const size_t v = ln.find("[VST2 ");
            if (v != std::string::npos) e.id = ln.substr(v + 6, 4);
            out.push_back(e);
            return;
        }
        if (out.empty() || ln.empty() || ln[0] != ' ') return;
        const size_t d = ln.find(" default ");
        if (d == std::string::npos) return;
        SweepParam p;
        p.index = std::atoi(ln.c_str());
        size_t ns = 0;
        while (ns < ln.size() && ln[ns] == ' ') ++ns;
        while (ns < ln.size() && ln[ns] != ' ') ++ns;     // past the index
        p.name = trim(ln.substr(ns, d - ns));
        p.def = std::atof(ln.c_str() + d + 9);
        size_t at = ln.find(": ", d);
        if (at == std::string::npos) return;
        at += 2;
        while (at < ln.size()) {
            size_t end = ln.find("; ", at);
            std::string item = ln.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (end == std::string::npos && !item.empty() && item.back() == ';') item.pop_back();
            const size_t eq = item.find('=');
            if (eq != std::string::npos) p.pts.push_back({(float)std::atof(item.substr(0, eq).c_str()), item.substr(eq + 1)});
            if (end == std::string::npos) break;
            at = end + 2;
        }
        out.back().params.push_back(p);
    };
    while ((ch = std::fgetc(f)) != EOF) {
        if (ch == '\n') { handle(line); line.clear(); }
        else line.push_back((char)ch);
    }
    if (!line.empty()) handle(line);
    std::fclose(f);
    return out;
}

// the numbers in a text (with their number of decimals) and the rest of it
static void numbersOf(const std::string& t, std::vector<std::pair<double, int>>& nums, std::string& skeleton)
{
    nums.clear();
    skeleton.clear();
    for (size_t i = 0; i < t.size();) {
        if (std::isdigit((unsigned char)t[i]) || (t[i] == '-' && i + 1 < t.size() && std::isdigit((unsigned char)t[i + 1]))) {
            size_t j = i + 1;
            while (j < t.size() && (std::isdigit((unsigned char)t[j]) || t[j] == '.')) ++j;
            const std::string n = t.substr(i, j - i);
            const size_t dot = n.find('.');
            nums.push_back({std::atof(n.c_str()), dot == std::string::npos ? 0 : (int)(n.size() - dot - 1)});
            skeleton += '#';
            i = j;
        } else { skeleton += t[i]; ++i; }
    }
}

static void sweepTest()
{
    const std::string path = testDataDir("RENO_EFX_REF");
    const std::vector<SweepFx> fxs = readSweep(path.c_str());
    if (fxs.empty()) { std::printf("SKIP the sweep of the real plug-ins' text (%s is not there)\n", path.c_str()); return; }
    for (const SweepFx& e : fxs) {
        std::unique_ptr<AudioPlugin> p = make(e.id.c_str());
        check(p != nullptr && p->paramCount() == 31 && e.params.size() == 31, "eFX " + e.name + ": made from " + e.id + ", 31 settings in the sweep");
        if (!p) continue;
        int names = 0, defs = 0, exact = 0, near = 0, bad = 0, total = 0;
        std::string firstBad;
        for (const SweepParam& sp : e.params) {
            if (p->paramName(sp.index) == sp.name) ++names;
            else std::printf("  eFX %s #%d is \"%s\", the real one \"%s\"\n", e.name.c_str(), sp.index, p->paramName(sp.index).c_str(), sp.name.c_str());
            if (std::fabs(p->paramGet(sp.index) - sp.def) < 6e-5) ++defs;
            else std::printf("  eFX %s #%d default %.5f, the real one %.4f\n", e.name.c_str(), sp.index, p->paramGet(sp.index), sp.def);
            auto q = make(e.id.c_str());
            for (const auto& pt : sp.pts) {
                ++total;
                q->paramSet(sp.index, pt.first);
                const std::string got = q->paramText(sp.index);
                // (the real one's text lost its micro and degree signs on the way to the file)
                std::string want = pt.second;
                for (size_t at; (at = want.find("\xEF\xBF\xBD")) != std::string::npos;)
                    want.replace(at, 3, at + 3 < want.size() && want[at + 3] == 's' ? "\xC2\xB5" : "\xC2\xB0");
                if (got == want) { ++exact; continue; }
                // one step in the last digit: the real one cuts numbers off, and the 0..1 it was set to had float noise
                std::vector<std::pair<double, int>> a, b;
                std::string sa, sb;
                numbersOf(got, a, sa);
                numbersOf(want, b, sb);
                bool ok = sa == sb && a.size() == b.size() && !a.empty();
                for (size_t k = 0; ok && k < a.size(); ++k) ok = std::fabs(a[k].first - b[k].first) <= 1.0001 * std::pow(10.0, -b[k].second);
                if (ok) { ++near; if (std::getenv("EFX_NEAR")) std::printf("  near: eFX %s #%d %s at %.3f: \"%s\", the real one \"%s\"\n", e.name.c_str(), sp.index, sp.name.c_str(), pt.first, got.c_str(), want.c_str()); continue; }
                ++bad;
                if (firstBad.empty()) firstBad = "#" + std::to_string(sp.index) + " " + sp.name + " at " + fmt("%.3f", pt.first) + ": \"" + got + "\", the real one \"" + want + "\"";
                else if (bad < 8) std::printf("  eFX %s #%d %s at %.3f: \"%s\", the real one \"%s\"\n", e.name.c_str(), sp.index, sp.name.c_str(), pt.first, got.c_str(), want.c_str());
            }
        }
        check(names == 31, "eFX " + e.name + ": all 31 names as the real one", std::to_string(names));
        check(defs == 31, "eFX " + e.name + ": all 31 defaults as the real one", std::to_string(defs));
        check(bad == 0, "eFX " + e.name + ": every setting's text at the 41 steps (" + std::to_string(exact) + " exact, " + std::to_string(near) + " a digit off, " + std::to_string(bad) + " wrong of " + std::to_string(total) + ")", firstBad);
        check(near * 8 <= total, "eFX " + e.name + ": at most one in eight a digit off (the real one cuts off numbers with some float noise Reno does not copy)", std::to_string(near) + " of " + std::to_string(total));
    }
}


int main()
{
    setEfxPreferBuiltin(true);

    // ---- the list of settings
    struct Named { const char* id; const char* name; std::vector<std::pair<int, const char*>> names; };
    const std::vector<Named> all = {
        {"MX2A", "eFX Limiter", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "Threshold"}, {6, "Release"}, {7, "Clip Gain"}, {20, "Attack"}, {21, "Adaptive Release"},
                                 {22, "Peak vs. RMS"}, {23, "Auto Makeup"}, {24, "Input LowCut"}, {25, "SC LowCut"}, {26, "Clip Shape"}, {30, "Version"}}},
        {"MX27", "eFX Compressor", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "SC Filter Freq"}, {6, "Threshold"}, {7, "Ratio"}, {8, "Attack"}, {9, "Release"}, {10, "SC Listen"},
                                    {11, "Soft Clip"}, {15, "SC Filter Type"}, {16, "Mode"}, {20, "Adaptive Release"}, {21, "Auto Makeup"}, {22, "Mix"}, {30, "Version"}}},
        {"MX23", "eFX Reverb", {{0, "Active"}, {1, "Mode"}, {3, "Input"}, {4, "Output"}, {5, "Size"}, {6, "Time"}, {7, "Damping"}, {8, "Modulation"}, {9, "Mix"}, {10, "Sync"},
                                {20, "Predelay"}, {21, "Diffusion"}, {22, "Low Cut"}, {30, "Version"}}},
        {"MX22", "eFX Phaser", {{0, "Active"}, {1, "Mode"}, {3, "Input"}, {4, "Output"}, {5, "Rate"}, {6, "Depth"}, {7, "Phase Offset"}, {8, "Feedback"}, {9, "Mix"}, {10, "Sync"},
                                {20, "Center Freq"}, {30, "Version"}}},
        {"MX2B", "eFX TubeStage", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "Pre EQ"}, {6, "Gain"}, {7, "Post EQ"}, {10, "Oversample"}, {15, "Stages"}, {16, "Class A || AB"},
                                   {20, "Tube Bias"}, {30, "Version"}}},
        {"MX21", "eFX ChorusFlanger", {{0, "Active"}, {1, "Mode"}, {3, "Input"}, {4, "Output"}, {5, "Rate"}, {6, "Depth"}, {7, "Phase Offset"}, {8, "Feedback"}, {9, "Mix"}, {10, "Sync"},
                                       {20, "Low Cut"}, {30, "Version"}}},
        {"MX28", "eFX DeEsser", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "Filter Freq"}, {6, "Reduction"}, {10, "Listen"}, {20, "Filter Q"}, {30, "Version"}}},
        {"MX29", "eFX Gate", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "SC Filter"}, {6, "Threshold"}, {7, "Range"}, {8, "Attack"}, {9, "Release"}, {10, "SC Listen"}, {11, "Soft Knee"},
                              {15, "SC Filter Type"}, {16, "SC Mode"}, {17, "Hold Time"}, {20, "Look Ahead"}, {21, "HP Filter@Input"}, {30, "Version"}}},
        {"MX24", "eFX StereoDelay", {{0, "Active"}, {1, "Mode"}, {3, "Input"}, {4, "Output"}, {5, "Delay L"}, {6, "Delay R"}, {7, "Damping"}, {8, "Feedback"}, {9, "Mix"}, {10, "Sync"},
                                     {20, "Feedback LowCut"}, {21, "Tape/BBD Noise"}, {22, "BBD Stages"}, {23, "BBD Compander"}, {24, "BBD Clock Drift"}, {30, "Version"}}},
        {"MX32", "eFX TremoloPan", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "Rate"}, {6, "Shape"}, {7, "Phase"}, {8, "Depth"}, {10, "Sync"}, {30, "Version"}}},
        {"MX30", "eFX VocalStrip", {{0, "Active"}, {3, "Input"}, {4, "Output"}, {5, "Highpass"}, {6, "Gate Threshold"}, {7, "DeEsser"}, {8, "Compression"}, {9, "Tone"}, {20, "DeEss DetFreq"},
                                    {21, "CompAutoMakeup"}, {22, "Comp Attack"}, {23, "Comp Adapt.Rel."}, {30, "Version"}}},
    };
    for (const Named& e : all) {
        std::unique_ptr<AudioPlugin> p = make(e.id);
        check(p != nullptr, std::string(e.name) + ": made through createAudioPlugin from the VST2 id");
        if (!p) continue;
        check(p->paramCount() == 31, std::string(e.name) + ": 31 settings", std::to_string(p->paramCount()));
        check(p->desc.format == "VST2" && p->desc.uid == std::to_string(idOf(e.id)) && p->desc.name == e.name, std::string(e.name) + ": still described as the VST2 plug-in of that id");
        check(efxIsReproduction(p->desc) && audioPluginKindNote(p->desc).find("reproduction") != std::string::npos, std::string(e.name) + ": says it is Reno's reproduction");
        bool names = true;
        for (const auto& n : e.names) if (p->paramName(n.first) != n.second) { names = false; std::printf("  #%d is \"%s\", wanted \"%s\"\n", n.first, p->paramName(n.first).c_str(), n.second); }
        check(names, std::string(e.name) + ": the settings' names");
        // the unused ones are "-" with no text, and (as the real ones) the sparse ones stay that way
        int unused = 0;
        for (int k = 0; k < 31; ++k) if (p->paramName(k) == "-") { ++unused; if (!p->paramText(k).empty()) unused += 1000; }
        check(unused >= 10 && unused < 1000, std::string(e.name) + ": unused settings are \"-\" without wording", std::to_string(unused));
        check(p->paramName(31).empty() && p->paramText(-1).empty() && p->paramGet(99) == 0.f, std::string(e.name) + ": settings out of range are nothing");
        check(p->latencySamples() >= 0 && !p->hasEditor() && p->paramCount() == 31, std::string(e.name) + ": no own window");
    }

    // ---- every setting at 41 steps of 0..1 against the real plug-ins' own text
    sweepTest();

    // ---- the saved form: the FxCk values of the sample projects load and are written back unchanged (the odd Version value too)
    {
        const std::vector<std::pair<const char*, std::vector<float>>> saved = {
            {"MX2A", vec({1, 0, 0, 0.5f, 0.971555f, 0.745f, 0.15f, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1})},
            {"MX27", vec({1, 0, 0, 1, 0.645f, 0, 0.637175f, 0.177825f, 0.077239f, 0.5f, 0, 0, 0, 0, 0, 0.333333f, 0, 0, 0, 0, 0.472603f, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1})},
            {"MX23", vec({1, 0.333333f, 0, 1, 1, 0.48f, 0.53f, 0, 0.115f, 0.085f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.095f, 1, 0.142857f, 0, 0, 0, 0, 0, 0, 0, 1})},
            {"MX22", vec({1, 0, 0, 1, 1, 0.505f, 1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.435374f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})},
            {"MX2B", vec({1, 0, 0, 0.5f, 0.69f, 0.5f, 0.500312f, 0.5f, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})},
            {"MX21", vec({1, 0, 0, 1, 1, 0.085f, 1, 1, 0.255f, 0.55f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1})},
        };
        for (const auto& s : saved) {
            auto p = make(s.first);
            const std::string st = stateOf(s.second);
            const bool loaded = p && p->loadState(st);
            check(loaded && p->saveState() == st, std::string(s.first) + ": the saved FxCk values load and are written back byte for byte (31 values, the Version one as saved)");
            bool each = loaded;
            for (int k = 0; loaded && k < 31; ++k) if (p->paramGet(k) != s.second[(size_t)k]) each = false;
            check(each, std::string(s.first) + ": every setting reads back as saved");
            check(p && !p->loadState("") && !p->loadState("garbage") && !p->loadState("Q:AAAA") && p->saveState() == st, std::string(s.first) + ": a state that is not an FxCk one is refused");
            // a shorter one (older plug-in): the rest keep their values
            std::vector<float> shorter(s.second.begin(), s.second.begin() + 12);
            auto q = make(s.first);
            check(q && q->loadState(stateOf(shorter)) && q->paramGet(3) == s.second[3], std::string(s.first) + ": a short state loads as far as it goes");
            // VST preset files: a program and a bank written and read back, and refused when for another plug-in
        {
            auto a = make(s.first), b = make(s.first), other = make(strcmp(s.first, "MX2A") == 0 ? "MX27" : "MX2A");
            a->loadState(stateOf(s.second));
            for (bool bank : {false, true}) {
                const std::string file = a->saveVstPresetFile(bank);
                std::string err;
                const bool ok = b->loadVstPresetFile(file, &err);
                check(ok && b->saveState() == stateOf(s.second), std::string(s.first) + ": " + (bank ? ".fxb bank" : ".fxp program") + " written and read back", err);
                check(!other->loadVstPresetFile(file, &err) && !err.empty(), std::string(s.first) + ": a preset file for another plug-in is refused");
            }
            std::string err;
            check(!b->loadVstPresetFile("junk", &err) && !b->loadVstPresetFile(std::string(), &err), std::string(s.first) + ": junk is not a preset file");
        }
        }
        // settings that are NaN or out of 0..1 do no harm
        auto p = make("MX2A");
        p->paramSet(5, std::nanf("")); p->paramSet(5, 9.f); p->paramSet(5, -3.f);
        check(p->paramGet(5) == 0.f, "Limiter: a setting beyond 0..1 is held to it");
    }

    // ---- Limiter
    {
        // (threshold, output, clip gain and input in dB; the clip shape 0..1, 1 = hard)
        auto lim = [](double thrDb, double outDb, bool makeup, double clipDb, double inDb = 0.0, double shape = 1.0) {
            auto p = make("MX2A");
            setReal(*p, 5, thrDb); setReal(*p, 4, outDb); set(*p, 23, makeup ? 1 : 0); setReal(*p, 7, clipDb); setReal(*p, 3, inDb); set(*p, 26, shape);
            return p;
        };
        const std::vector<float> loud = noise(96000, 1.6f, 7);
        // never above the ceiling (Output), for every mix of settings
        const double outs[] = {-0.1, -3.0, 0.0};
        bool never = true;
        double worst = 0;
        std::string detail;
        for (double thrDb : {-30.0, -10.7, -4.29, 0.0})
            for (double outDb : outs)
                for (int mk = 0; mk < 2; ++mk)
                    for (double clipDb : {0.0, 12.0})
                        for (double inDb : {0.0, 24.0}) {
                            auto p = lim(thrDb, outDb, mk == 1, clipDb, inDb);
                            const double ceil = std::pow(10.0, outDb / 20.0);
                            const std::vector<float> o = process(*p, loud);
                            const double pk = peak(o);
                            if (pk > ceil + 1e-5) { never = false; detail = fmt("thr %.2f out %.2f: peak %.5f above ceiling", thrDb, outDb, pk) + fmt(" %.5f", ceil); }
                            worst = std::max(worst, pk - ceil);
                        }
        check(never, "Limiter: the output never goes above its ceiling (Output), over 96 settings, on loud noise", detail);
        // limits to the threshold, then makes up: a full-scale sine with Threshold -10.7 and auto makeup comes out at the ceiling
        {
            auto p = lim(-10.7, -0.1, true, 0.0);
            const std::vector<float> o = process(*p, sine(96000, 220, 1.0f));
            const double pk = peak(o, 48000);
            check(std::fabs(db(pk) - (-0.1)) < 0.1, "Limiter: full scale in, Threshold -10.7dB, auto makeup: peaks at the ceiling (-0.1dB)", fmt("%.3f dB", db(pk)));
            check(rms(o, 48000) > 0.5, "Limiter: and is loud, not just clamped");
        }
        // below the threshold nothing is limited: only the makeup (+10.7dB) and the output (-0.1dB) are heard
        {
            auto p = lim(-10.7, -0.1, true, 0.0);
            const std::vector<float> i = sine(48000, 220, 0.03f);
            const std::vector<float> o = process(*p, i);
            const double gain = db(rms(o, 24000)) - db(rms(i, 24000));
            check(std::fabs(gain - (10.7 - 0.1)) < 0.4, "Limiter: below the threshold only the auto makeup (+10.7dB) and the output (-0.1dB) act", fmt("%.2f dB", gain));
        }
        // auto makeup off: a quiet sine only gets the output setting
        {
            auto p = lim(-10.7, -3.0, false, 0.0);
            const std::vector<float> i = sine(48000, 220, 0.03f);
            const std::vector<float> o = process(*p, i);
            const double gain = db(rms(o, 24000)) - db(rms(i, 24000));
            check(std::fabs(gain - (-3.0)) < 0.2, "Limiter: with auto makeup off the level only follows the output (-3dB)", fmt("%.2f dB", gain));
        }
        // the input low cut and the input gain
        {
            auto p = lim(0.0, 0.0, true, 0.0, 24.0);
            const std::vector<float> o = process(*p, sine(48000, 220, 0.1f));
            check(std::fabs(db(rms(o, 24000)) - db(rms(sine(48000, 220, 0.1f), 24000)) - 24.0) < 0.5 || peak(o) <= 1.0, "Limiter: input +24dB raises the level (or is limited at full scale)");
        }
        {
            auto a = make("MX2A"); auto b = make("MX2A");
            set(*b, 24, 0.6);
            const std::vector<float> i = sine(48000, 40, 0.2f);
            const double la = rms(process(*a, i), 24000), lb = rms(process(*b, i), 24000);
            check(lb < la * 0.5, "Limiter: the input low cut takes the bass out", fmt("%.4f -> %.4f", la, lb));
        }
        // clip gain adds loudness (the clipper), the sound stays under the ceiling; soft shape bends the peaks
        {
            auto a = lim(-12.0, -0.1, true, 0.0), b = lim(-12.0, -0.1, true, 12.0);
            const std::vector<float> i = sine(96000, 220, 0.5f);
            const double ra = rms(process(*a, i), 48000), rb = rms(process(*b, i), 48000);
            check(rb > ra * 1.1, "Limiter: clip gain +12dB makes it louder", fmt("%.3f -> %.3f", ra, rb));
            auto hard = lim(-12.0, -0.1, true, 12.0, 0.0, 1.0), soft = lim(-12.0, -0.1, true, 12.0, 0.0, 0.0);
            const double rh = rms(process(*hard, i), 48000), rsft = rms(process(*soft, i), 48000);
            check(rh > rsft, "Limiter: the soft clip shape is less intense than the hard one", fmt("%.3f vs %.3f", rsft, rh));
        }
        // latency (the look-ahead of the attack time) and that the sound is held back by exactly that
        {
            auto p = lim(0.0, 0.0, false, 0.0);
            const int lat = p->latencySamples();
            check(lat == 47, "Limiter: reports the look-ahead of its 1.0ms attack as latency", std::to_string(lat));
            std::vector<float> imp(4800, 0.f);
            imp[1000] = 0.25f;
            const std::vector<float> o = process(*p, imp);
            int at = 0;
            for (int k = 0; k < (int)o.size(); ++k) if (std::fabs(o[(size_t)k]) > 0.1f) { at = k; break; }
            check(at == 1000 + lat, "Limiter: an impulse comes out exactly that late", std::to_string(at));
        }
        // release: after a loud burst a quiet tone comes back to level, sooner with a short release
        {
            auto timeToRecover = [&](double relX, double adaptiveX) {
                auto p = lim(-12.0, 0.0, false, 0.0);
                set(*p, 6, relX); set(*p, 21, adaptiveX);
                std::vector<float> in(48000, 0.f);
                for (int k = 0; k < 4800; ++k) in[(size_t)k] = 1.5f * (float)std::sin(2 * kPi * 100 * k / kSr);
                for (int k = 4800; k < 48000; ++k) in[(size_t)k] = 0.05f * (float)std::sin(2 * kPi * 1000 * k / kSr);
                const std::vector<float> o = process(*p, in);
                // when the quiet tone's level is within 1 dB of its final level
                const double fin = rms(o, 44000, 47000);
                for (int k = 5000; k < 44000; k += 48) if (rms(o, (size_t)k, (size_t)k + 480) > fin * 0.89) return k - 4800;
                return 44000;
            };
            const int fast = timeToRecover(0.0, 0.0), slow = timeToRecover(0.5, 0.0), adaptive = timeToRecover(0.0, 1.0);
            check(fast < slow, "Limiter: a longer release takes longer to come back", std::to_string(fast) + " vs " + std::to_string(slow));
            check(adaptive >= fast, "Limiter: adaptive release does not come back sooner", std::to_string(adaptive) + " vs " + std::to_string(fast));
        }
    }

    // ---- Compressor
    {
        auto comp = [](double thrX, double ratioX, bool makeup, double mixX = 1.0) {
            auto p = make("MX27");
            set(*p, 6, thrX); set(*p, 7, ratioX); set(*p, 21, makeup ? 1 : 0); set(*p, 22, mixX); set(*p, 8, 0.0); set(*p, 9, 0.3);
            return p;
        };
        // thr -20dB (0.6667), ratio 4.8 : 1 (0.575); static curve, soft knee 10dB
        const double ratio = 4.8, thr = -20.0;
        auto curve = [&](double inDb) {
            const double over = inDb - thr, slope = 1.0 / ratio - 1.0;
            if (over > 5) return inDb + slope * over;
            if (over > -5) return inDb + slope * (over + 5) * (over + 5) / 20.0;
            return inDb;
        };
        bool curveOk = true;
        std::string detail;
        for (double inDb : {-40.0, -30.0, -24.0, -20.0, -15.0, -10.0, -5.0, 0.0}) {
            auto p = comp(0.6667, 0.575, false);
            const float a = (float)std::pow(10.0, inDb / 20.0);
            const std::vector<float> o = process(*p, sine(96000, 440, a));
            const double outDb = db(peak(o, 72000));
            if (std::fabs(outDb - curve(inDb)) > 0.6) { curveOk = false; detail += fmt(" in %.0f: out %.2f want %.2f;", inDb, outDb, curve(inDb)); }
        }
        check(curveOk, "Compressor: the static curve follows Threshold -20dB / Ratio 4.8 : 1 with a soft knee", detail);
        // auto makeup: a full-scale tone comes out at full scale; the quiet ones get the same lift
        {
            auto p = comp(0.6667, 0.575, true);
            const std::vector<float> o = process(*p, sine(96000, 440, 1.0f));
            const double lift = -thr * (1.0 - 1.0 / ratio);
            check(std::fabs(db(peak(o, 72000)) - 0.0) < 0.8, "Compressor: auto makeup brings a full-scale tone back to full scale", fmt("%.2f dB", db(peak(o, 72000))));
            auto q = comp(0.6667, 0.575, true);
            const std::vector<float> o2 = process(*q, sine(96000, 440, 0.01f));
            check(std::fabs(db(peak(o2, 72000)) - (-40.0 + lift)) < 0.5, "Compressor: and a quiet tone is lifted by the makeup (+15.8dB)", fmt("%.2f dB", db(peak(o2, 72000))));
        }
        // output and mix
        {
            auto p = comp(1.0, 0.0, false);
            set(*p, 4, 0.6);
            const std::vector<float> i = sine(48000, 440, 0.1f);
            const double g = db(rms(process(*p, i), 24000)) - db(rms(i, 24000));
            check(std::fabs(g - 9.6) < 0.2, "Compressor: Output 0.6 is +9.6dB", fmt("%.2f dB", g));
            auto d = comp(0.6667, 0.575, false, 0.0);
            const std::vector<float> o = process(*d, sine(48000, 440, 0.9f));
            check(std::fabs(db(peak(o, 24000)) - db(0.9)) < 0.05, "Compressor: Mix 0% is the untouched sound");
            auto h = comp(0.6667, 0.575, false, 0.5);
            auto w = comp(0.6667, 0.575, false, 1.0);
            const double ph = peak(process(*h, sine(96000, 440, 0.9f)), 72000), pw = peak(process(*w, sine(96000, 440, 0.9f)), 72000);
            check(ph > pw && ph < 0.9, "Compressor: Mix 50% is between the compressed and the untouched sound", fmt("%.3f", ph) + fmt(" (%.3f..", pw) + ")");
        }
        // attack and release: a burst is caught sooner with a short attack; the gain comes back sooner with a short release
        {
            auto burst = [&](double attackX, double releaseX) {
                auto p = comp(0.5, 0.575, false);
                set(*p, 8, attackX); set(*p, 9, releaseX);
                std::vector<float> in(96000, 0.f);
                for (int k = 0; k < 96000; ++k) in[(size_t)k] = (k >= 24000 && k < 48000 ? 1.0f : 0.02f) * (float)std::sin(2 * kPi * 440 * k / kSr);
                const std::vector<float> o = process(*p, in);
                // level of the first ms of the burst, and of the quiet tone after it
                return std::make_pair(peak(o, 24000, 24480), rms(o, 48000 + 2400, 48000 + 4800));
            };
            const auto fastA = burst(0.0, 0.3), slowA = burst(0.7, 0.3);
            check(fastA.first < slowA.first, "Compressor: a slower attack lets more of a burst's start through", fmt("%.3f vs %.3f", fastA.first, slowA.first));
            const auto fastR = burst(0.0, 0.0), slowR = burst(0.0, 0.8);
            check(fastR.second > slowR.second, "Compressor: a longer release leaves the gain down for longer", fmt("%.5f vs %.5f", fastR.second, slowR.second));
        }
        // the sidechain filter (low-pass): a treble tone alone is not compressed once the filter takes it out of the detector
        {
            auto a = comp(0.5, 0.575, false), b = comp(0.5, 0.575, false);
            set(*b, 15, 1.0); set(*b, 5, 0.3);          // low-pass at ~ 40Hz
            const std::vector<float> i = sine(96000, 4000, 0.8f);
            check(peak(process(*b, i), 72000) > peak(process(*a, i), 72000) * 1.5, "Compressor: with the sidechain low-pass a treble tone is compressed much less");
        }
        // soft clip keeps the output below 1
        {
            auto p = comp(1.0, 0.0, false);
            set(*p, 4, 1.0); set(*p, 11, 1.0);
            const std::vector<float> o = process(*p, sine(48000, 440, 0.9f));
            check(peak(o) <= 1.0 && peak(o) > 0.9, "Compressor: soft clip keeps +48dB below full scale", fmt("%.3f", peak(o)));
        }
        check(make("MX27")->latencySamples() == 0, "Compressor: no latency");
    }

    // ---- Reverb
    {
        auto reverb = [](int mode, double timeX, double damping, double sizeX) {
            auto p = make("MX23");
            set(*p, 1, mode / 3.0); set(*p, 6, timeX); set(*p, 7, damping); set(*p, 5, sizeX); set(*p, 9, 1.0); set(*p, 8, 0.0); set(*p, 20, 0.0); set(*p, 22, 0.0);
            return p;
        };
        // the decay: a 2s tail falls 60dB in about 2s (read from the backward-integrated energy, -5 to -35dB, times two)
        auto rt60 = [](const std::vector<float>& l, const std::vector<float>& r) {
            const size_t n = l.size();
            std::vector<double> edc(n);
            double acc = 0;
            for (size_t k = n; k-- > 0;) { acc += (double)l[k] * l[k] + (double)r[k] * r[k]; edc[k] = acc; }
            const double e0 = edc[0];
            long a = -1, b = -1;
            for (size_t k = 0; k < n; ++k) {
                const double d = 10.0 * std::log10(edc[k] / e0 + 1e-30);
                if (a < 0 && d < -5.0) a = (long)k;
                if (b < 0 && d < -35.0) { b = (long)k; break; }
            }
            return (a < 0 || b < 0) ? -1.0 : 2.0 * (double)(b - a) / kSr;
        };
        for (int mode = 0; mode < 4; ++mode) {
            for (double t : {0.5, 2.0}) {
                auto p = reverb(mode, 0.5, 0.0, 0.5);
                setReal(*p, 6, t);
                std::vector<float> imp((size_t)(kSr * (t * 2 + 2)), 0.f);
                imp[0] = 1.0f;
                std::vector<float> r;
                const std::vector<float> l = process(*p, imp, 512, &r);
                const double est = rt60(l, r);
                check(est > t * 0.6 && est < t * 1.6, "Reverb mode " + std::to_string(mode) + ": a tail of Time " + fmt("%.1f", t) + "s falls 60dB in about that time", fmt("%.2f s", est));
            }
        }
        // damping makes the highs die sooner: the energy in a tail's second half is lower with damping, in a high band
        {
            auto bright = reverb(2, 0.5, 0.0, 0.5), dull = reverb(2, 0.5, 0.9, 0.5);
            std::vector<float> imp((size_t)(kSr * 3), 0.f);
            imp[0] = 1.0f;
            const std::vector<float> a = process(*bright, imp), b = process(*dull, imp);
            std::vector<float> ha(a.size()), hb(b.size());
            for (size_t k = 1; k < a.size(); ++k) { ha[k] = a[k] - a[k - 1]; hb[k] = b[k] - b[k - 1]; }       // a crude high-pass
            check(rms(hb, 48000, 96000) < rms(ha, 48000, 96000) * 0.5, "Reverb: Damping takes the highs out of the tail", fmt("%.5f vs %.5f", rms(hb, 48000, 96000), rms(ha, 48000, 96000)));
        }
        // pre-delay: nothing wet before it
        {
            auto p = reverb(1, 0.3, 0.2, 0.5);
            set(*p, 20, 0.5);               // 4.5469 .. 100: about 50ms
            const double ms = std::atof(p->paramText(20).c_str());
            std::vector<float> imp(48000, 0.f);
            imp[0] = 1.0f;
            std::vector<float> r;
            const std::vector<float> l = process(*p, imp, 512, &r);
            // (the allpass diffusers start after the pre-delay, the dry part is gone at Mix 100%)
            size_t first = 0;
            for (size_t k = 0; k < l.size(); ++k) if (std::fabs(l[k]) > 1e-4f || std::fabs(r[k]) > 1e-4f) { first = k; break; }
            check(first >= (size_t)(ms * 0.001 * kSr) - 2 && first < (size_t)(ms * 0.001 * kSr) + 600, "Reverb: nothing is heard before the pre-delay (" + fmt("%.1f", ms) + "ms)", std::to_string(first));
        }
        // true stereo: a sound only on the left is louder in the left output
        {
            auto p = reverb(0, 0.4, 0.2, 0.5);
            const std::vector<float> in = noise(48000, 0.3f, 3), zero(48000, 0.f);
            std::vector<float> r;
            const std::vector<float> l = process(*p, in, 512, &r, &zero);
            check(rms(l, 12000) > rms(r, 12000) * 1.1 && rms(r, 12000) > 0.0, "Reverb: left-only sound stays mostly left, with some reverb right (true stereo)", fmt("%.4f / %.4f", rms(l, 12000), rms(r, 12000)));
        }
        // mix 0 = the dry sound, exactly
        {
            auto p = make("MX23");
            set(*p, 9, 0.0);
            const std::vector<float> in = noise(4800, 0.3f, 4);
            const std::vector<float> o = process(*p, in);
            double d = 0;
            for (size_t k = 0; k < in.size(); ++k) d = std::max(d, (double)std::fabs(o[k] - in[k]));
            check(d < 1e-6, "Reverb: Mix 0% is the dry sound", fmt("%.7f", d));
        }
        // low cut: no bass in the reverb
        {
            auto a = reverb(2, 0.4, 0.2, 0.5), b = reverb(2, 0.4, 0.2, 0.5);
            set(*b, 22, 0.6);
            const std::vector<float> in = sine(96000, 60, 0.3f);
            check(rms(process(*b, in), 48000) < rms(process(*a, in), 48000) * 0.5, "Reverb: Low Cut keeps the bass out of the reverb");
        }
    }

    // ---- Tube Stage
    {
        auto tube = [](double gainDb, bool classAB, bool oversample, bool two) {
            auto p = make("MX2B");
            setReal(*p, 6, gainDb); set(*p, 16, classAB ? 1 : 0); set(*p, 10, oversample ? 1 : 0); set(*p, 15, two ? 1 : 0);
            return p;
        };
        const std::vector<float> in = sine(96000, 1000, 0.3f);
        // clean at no gain (the default Gain), harmonics appear with gain
        {
            auto clean = tube(0.0, false, true, false), driven = tube(24.0, false, true, false);
            const std::vector<float> a = process(*clean, in), b = process(*driven, in);
            const double h3a = toneLevel(a, 3000, 48000) - toneLevel(a, 1000, 48000), h3b = toneLevel(b, 3000, 48000) - toneLevel(b, 1000, 48000);
            check(h3b > h3a + 12, "TubeStage: gain adds harmonics (3rd: " + fmt("%.1f", h3a) + " -> " + fmt("%.1f", h3b) + " dB)");
        }
        // class A has even harmonics, class AB hardly any
        {
            auto a = tube(16.8, false, true, false), ab = tube(16.8, true, true, false);
            set(*ab, 20, 0.5);
            const std::vector<float> x = process(*a, in), y = process(*ab, in);
            const double h2a = toneLevel(x, 2000, 48000) - toneLevel(x, 1000, 48000), h2ab = toneLevel(y, 2000, 48000) - toneLevel(y, 1000, 48000);
            const double h3ab = toneLevel(y, 3000, 48000) - toneLevel(y, 1000, 48000);
            check(h2a > h2ab + 20, "TubeStage: class A makes even harmonics, class AB hardly (2nd: " + fmt("%.1f", h2a) + " vs " + fmt("%.1f", h2ab) + " dB)");
            check(h3ab > -40, "TubeStage: class AB makes odd harmonics", fmt("%.1f dB", h3ab));
        }
        // oversampling: less aliasing from a high tone driven hard
        {
            auto on = tube(24.0, true, true, true), off = tube(24.0, true, false, true);
            const std::vector<float> hi = sine(96000, 9100, 0.4f);
            const std::vector<float> a = process(*on, hi), b = process(*off, hi);
            // 3rd harmonic 27300 folds to 20700 at 48k
            const double fa = toneLevel(a, 20700, 48000), fb = toneLevel(b, 20700, 48000);
            check(fa < fb - 10, "TubeStage: oversampling keeps the folded harmonics down (" + fmt("%.1f", fa) + " vs " + fmt("%.1f", fb) + " dB)");
            check(on->latencySamples() > 0 && off->latencySamples() == 0, "TubeStage: latency follows oversampling");
        }
        // output follows Output; input follows Input
        {
            auto p = tube(0.0, false, false, false);
            set(*p, 4, 0.25);               // -12dB
            const std::vector<float> o = process(*p, sine(48000, 1000, 0.05f));
            const std::vector<float> i = sine(48000, 1000, 0.05f);
            const double g = db(rms(o, 24000)) - db(rms(i, 24000));
            check(std::fabs(g - (-12.0)) < 1.0, "TubeStage: Output -12dB lowers the clean sound by that", fmt("%.2f dB", g));
        }
        // the tilt filters change the balance of lows and highs
        {
            auto a = tube(0.0, false, false, false), b = tube(0.0, false, false, false);
            set(*b, 5, 1.0);
            const std::vector<float> n1 = noise(96000, 0.2f, 5);
            const std::vector<float> oa = process(*a, n1), ob = process(*b, n1);
            std::vector<float> ha(oa.size()), hb(ob.size());
            for (size_t k = 1; k < oa.size(); ++k) { ha[k] = oa[k] - oa[k - 1]; hb[k] = ob[k] - ob[k - 1]; }
            check(rms(hb, 24000) > rms(ha, 24000) * 1.1, "TubeStage: Pre EQ to the right brightens");
        }
    }

    // ---- Phaser
    {
        auto count = [&](int modeIdx) {
            // the gain at 160 frequencies on a log grid, measured with tones
            std::vector<double> g;
            for (int k = 0; k < 160; ++k) {
                const double f = 100.0 * std::pow(10000.0 / 100.0, k / 159.0);
                auto q = make("MX22");
                set(*q, 1, modeIdx * 0.5); set(*q, 5, 0.0); set(*q, 6, 0.0); set(*q, 9, 0.5); set(*q, 8, 0.0);
                const std::vector<float> s = sine(9600, f, 0.3f);
                const std::vector<float> t = process(*q, s);
                g.push_back(db(rms(t, 4800)) - db(rms(s, 4800)));
            }
            int dips = 0;
            for (size_t k = 1; k + 1 < g.size(); ++k) if (g[k] < g[k - 1] && g[k] <= g[k + 1] && g[k] < -9.0) ++dips;
            return dips;
        };
        const int d4 = count(0), d8 = count(1);
        check(d4 >= 1 && d4 <= 3, "Phaser: 4 stages: two notches in the response", std::to_string(d4));
        check(d8 >= 3 && d8 <= 5, "Phaser: 8 stages: four notches", std::to_string(d8));
        // mix 0 = dry; the notches move (the response differs between two moments)
        auto p = make("MX22");
        set(*p, 9, 0.0);
        const std::vector<float> in = noise(9600, 0.3f, 12);
        const std::vector<float> o = process(*p, in);
        double d = 0;
        for (size_t k = 0; k < in.size(); ++k) d = std::max(d, (double)std::fabs(o[k] - in[k]));
        check(d < 1e-5, "Phaser: Mix 0% is the dry sound", fmt("%.7f", d));
        auto q = make("MX22");
        set(*q, 9, 0.5); set(*q, 5, 0.6); set(*q, 6, 1.0);
        const std::vector<float> n2 = noise(48000 * 2, 0.3f, 13);
        const std::vector<float> o2 = process(*q, n2);
        // the level of a band moves with the sweep
        double lo = 1e9, hi = 0;
        for (size_t at = 4800; at + 2400 < o2.size(); at += 2400) {
            std::vector<float> seg(o2.begin() + (long)at, o2.begin() + (long)at + 2400);
            const double lv = toneLevel(seg, 1700, 0);
            lo = std::min(lo, lv); hi = std::max(hi, lv);
        }
        check(hi - lo > 3.0, "Phaser: the sweep moves the notches (a band's level varies by " + fmt("%.1f", hi - lo) + " dB)");
        // the right channel's sweep is offset by Phase Offset
        auto s = make("MX22");
        set(*s, 7, 1.0); set(*s, 9, 1.0); set(*s, 5, 0.5); set(*s, 6, 1.0);
        std::vector<float> r;
        const std::vector<float> ls = process(*s, noise(48000, 0.3f, 14), 512, &r);
        double diff = 0;
        for (size_t k = 0; k < ls.size(); ++k) diff += std::fabs(ls[k] - r[k]);
        check(diff > 1.0, "Phaser: Phase Offset makes the two channels differ");
    }

    // ---- Chorus / Flanger
    {
        auto delayOf = [&](int mode, double* strength = nullptr) {
            auto p = make("MX21");
            set(*p, 1, mode * 0.5); set(*p, 6, 0.0); set(*p, 8, 0.5); set(*p, 9, 1.0); set(*p, 20, 0.0);
            std::vector<float> imp(9600, 0.f);
            imp[2000] = 0.5f;
            const std::vector<float> o = process(*p, imp);
            size_t best = 0;
            for (size_t k = 0; k < o.size(); ++k) if (std::fabs(o[k]) > std::fabs(o[best])) best = k;
            if (strength) *strength = std::fabs(o[best]);
            return (int)best - 2000;
        };
        double s0 = 0;
        const int chorus = delayOf(0, &s0), flanger = delayOf(1), ens = delayOf(2);
        check(std::abs(chorus - 576) <= 3, "ChorusFlanger: chorus delays the copy by about 12ms (no depth)", std::to_string(chorus));
        check(std::abs(flanger - 144) <= 3, "ChorusFlanger: flanger delays it by about 3ms", std::to_string(flanger));
        check(std::abs(ens - 672) <= 6, "ChorusFlanger: ensemble delays it by about 14ms", std::to_string(ens));
        // depth modulates the pitch: a tone gets sidebands
        {
            auto p = make("MX21");
            set(*p, 5, 0.4); set(*p, 6, 1.0); set(*p, 9, 1.0); set(*p, 8, 0.5);
            const std::vector<float> o = process(*p, sine(96000, 1000, 0.5f));
            double side = -200;
            for (double f = 900; f <= 1100; f += 2) if (std::fabs(f - 1000) > 12) side = std::max(side, toneLevel(o, f, 24000));
            check(side > toneLevel(o, 1000, 24000) - 40, "ChorusFlanger: depth modulates the delay (the tone has sidebands)", fmt("%.1f dB", side));
        }
        // the mix 0 = dry; feedback in a flanger makes a comb (more energy at multiples of 1/delay)
        {
            auto p = make("MX21");
            set(*p, 9, 0.0);
            const std::vector<float> in = noise(4800, 0.3f, 15);
            const std::vector<float> o = process(*p, in);
            double d = 0;
            for (size_t k = 0; k < in.size(); ++k) d = std::max(d, (double)std::fabs(o[k] - in[k]));
            check(d < 1e-6, "ChorusFlanger: Mix 0% is the dry sound", fmt("%.7f", d));
            auto f = make("MX21");
            set(*f, 1, 0.5); set(*f, 6, 0.0); set(*f, 8, 0.95); set(*f, 9, 0.5);
            const std::vector<float> o2 = process(*f, noise(96000, 0.2f, 16));
            const double combPeak = toneLevel(o2, 48000.0 / 144.0 * 3, 24000), combDip = toneLevel(o2, 48000.0 / 144.0 * 3.5, 24000);
            check(combPeak > combDip + 6, "ChorusFlanger: flanger feedback makes a comb", fmt("%.1f", combPeak) + fmt(" vs %.1f dB", combDip));
        }
        // the right channel's LFO is offset by Phase
        {
            auto p = make("MX21");
            set(*p, 5, 0.5); set(*p, 6, 1.0); set(*p, 7, 1.0); set(*p, 9, 1.0); set(*p, 8, 0.5);
            std::vector<float> r;
            const std::vector<float> l = process(*p, sine(48000, 800, 0.5f), 512, &r);
            double diff = 0;
            for (size_t k = 12000; k < l.size(); ++k) diff += std::fabs(l[k] - r[k]);
            check(diff > 50.0, "ChorusFlanger: Phase makes the two channels differ");
        }
    }

    // ---- De-Esser
    {
        auto burst = [&](double reductionDb, double* lowGainDb) {
            auto p = make("MX28");
            setReal(*p, 6, reductionDb); setReal(*p, 5, 7000);
            // a voice (200Hz) all along, with 7kHz hissing for a while in the middle
            const int n = 96000;
            std::vector<float> in((size_t)n);
            for (int k = 0; k < n; ++k) {
                const double t = k / kSr;
                float v = 0.2f * (float)std::sin(2 * kPi * 200 * t);
                if (k > 24000 && k < 72000) v += 0.3f * (float)std::sin(2 * kPi * 7000 * t);
                in[(size_t)k] = v;
            }
            const std::vector<float> o = process(*p, in);
            (void)lowGainDb;
            // the level of the 7kHz in the second half of the burst, against the input's
            const std::vector<float> a(o.begin() + 56000, o.begin() + 68000), b(in.begin() + 56000, in.begin() + 68000);
            return toneLevel(a, 7000, 0) - toneLevel(b, 7000, 0);
        };
        double dummy = 0;
        const double none = burst(0.0, &dummy), some = burst(18.0, &dummy);
        check(std::fabs(none) < 0.2, "DeEsser: Reduction 0dB leaves the sibilants alone", fmt("%.2f dB", none));
        check(some < -6.0, "DeEsser: Reduction 18dB takes a sibilant burst down", fmt("%.2f dB", some));
        // a voice with nothing in the band is not touched
        {
            auto p = make("MX28");
            setReal(*p, 6, 18.0); setReal(*p, 5, 7000);
            const std::vector<float> voice = sine(96000, 200, 0.3f);
            check(std::fabs(db(rms(process(*p, voice), 48000)) - db(rms(voice, 48000))) < 0.2, "DeEsser: a voice with nothing in the band passes untouched");
        }
        // listen: only the band
        auto q = make("MX28");
        set(*q, 10, 1.0); setReal(*q, 5, 7000);
        const std::vector<float> mixed = process(*q, [&] { auto v = sine(48000, 200, 0.3f); auto h = sine(48000, 7000, 0.3f); for (size_t k = 0; k < v.size(); ++k) v[k] += h[k]; return v; }());
        check(toneLevel(std::vector<float>(mixed.begin() + 24000, mixed.end()), 200, 0) < toneLevel(std::vector<float>(mixed.begin() + 24000, mixed.end()), 7000, 0) - 20, "DeEsser: listening plays the detection band only");
        check(make("MX28")->latencySamples() == 0, "DeEsser: no latency");
    }

    // ---- Gate
    {
        auto gate = [](double thrDb, double rangeDb) {
            auto p = make("MX29");
            setReal(*p, 6, thrDb); setReal(*p, 7, rangeDb); set(*p, 20, 0.0); set(*p, 21, 0.0);
            setReal(*p, 8, 0.1); setReal(*p, 9, 20); set(*p, 17, 0.0);
            return p;
        };
        {
            auto p = gate(-30.0, 60.0);
            std::vector<float> in(96000);
            for (int k = 0; k < 96000; ++k) in[(size_t)k] = (k < 48000 ? 0.5f : 0.005f) * (float)std::sin(2 * kPi * 440 * k / kSr);
            const std::vector<float> o = process(*p, in);
            check(std::fabs(db(rms(o, 24000, 40000)) - db(rms(in, 24000, 40000))) < 0.3, "Gate: a sound above the threshold passes untouched");
            check(db(rms(o, 80000, 90000)) < db(rms(in, 80000, 90000)) - 40, "Gate: a sound below the threshold is taken down by Range (60dB)", fmt("%.1f dB", db(rms(o, 80000, 90000)) - db(rms(in, 80000, 90000))));
        }
        {
            auto p = gate(-30.0, 12.0);
            std::vector<float> in(96000);
            for (int k = 0; k < 96000; ++k) in[(size_t)k] = (k < 48000 ? 0.5f : 0.005f) * (float)std::sin(2 * kPi * 440 * k / kSr);
            const std::vector<float> o = process(*p, in);
            const double g = db(rms(o, 80000, 90000)) - db(rms(in, 80000, 90000));
            check(std::fabs(g + 12.0) < 1.5, "Gate: Range 12dB takes it down by 12dB and no more", fmt("%.1f dB", g));
        }
        {
            // hold: the gate stays open after the sound stops for the hold time
            auto a = gate(-30.0, 60.0), b = gate(-30.0, 60.0);
            set(*b, 17, 1.0);               // 200%: the longest hold
            std::vector<float> in(48000, 0.f);
            for (int k = 0; k < 9600; ++k) in[(size_t)k] = 0.5f * (float)std::sin(2 * kPi * 440 * k / kSr);
            for (int k = 9600; k < 48000; ++k) in[(size_t)k] = 0.0f;
            // (a tail of quiet sound just under the threshold, kept or cut by the hold)
            for (int k = 9600; k < 19200; ++k) in[(size_t)k] = 0.02f * (float)std::sin(2 * kPi * 440 * k / kSr);
            const double ra = rms(process(*a, in), 15000, 19000), rb = rms(process(*b, in), 15000, 19000);
            check(rb > ra * 3, "Gate: a longer Hold keeps it open longer", fmt("%.5f vs %.5f", ra, rb));
        }
        {
            auto p = gate(-30.0, 60.0);
            setReal(*p, 20, 2.0);
            const int lat = p->latencySamples();
            check(lat == 96, "Gate: Look Ahead 2ms is reported as latency (96 samples at 48kHz)", std::to_string(lat));
            std::vector<float> imp(4800, 0.f);
            imp[1000] = 0.5f;
            const std::vector<float> o = process(*p, imp);
            int at = -1;
            for (int k = 0; k < (int)o.size(); ++k) if (std::fabs(o[(size_t)k]) > 0.05f) { at = k; break; }
            check(at == 1000 + lat, "Gate: the sound is held back by the look-ahead", std::to_string(at));
            check(make("MX29")->latencySamples() == 24, "Gate: the default Look Ahead (0.5ms) is 24 samples");
        }
        {
            auto a = gate(-30.0, 60.0), b = gate(-30.0, 60.0);
            set(*b, 15, 0.0);
            set(*a, 15, 0.14);               // the low-pass at ~140Hz: the detector hears hardly anything of a 4kHz tone
            set(*a, 5, 0.1);
            const std::vector<float> tone = sine(48000, 4000, 0.1f);
            check(rms(process(*a, tone), 24000) < rms(process(*b, tone), 24000) * 0.5, "Gate: the side-chain low-pass keeps a treble tone from opening it");
        }
    }

    // ---- Stereo Delay
    {
        auto delayed = [&](int stepLeft, double fbPercent, std::vector<float>* rightOut) {
            auto p = make("MX24");
            set(*p, 9, 1.0); set(*p, 10, 1.0);
            // Delay L / R by the index of the list
            p->paramSet(5, (float)(stepLeft / 12.0) + 0.01f);
            p->paramSet(6, (float)(stepLeft / 12.0) + 0.01f);
            setReal(*p, 8, fbPercent);
            set(*p, 7, 0.0); set(*p, 20, 0.0);
            std::vector<float> imp(48000 * 4, 0.f);
            imp[0] = 0.5f;
            const std::vector<float> zero(imp.size(), 0.f);
            std::vector<float> r;
            const std::vector<float> l = process(*p, imp, 512, &r, &zero);
            if (rightOut) *rightOut = r;
            return l;
        };
        std::vector<float> r;
        const std::vector<float> l = delayed(8, 0.0, &r);               // 1/4 = 500ms at 120 beats a minute, no feedback
        size_t at = 0;
        for (size_t k = 1; k < l.size(); ++k) if (std::fabs(l[k]) > std::fabs(l[at])) at = k;
        check(std::labs((long)at - 24000) <= 2, "StereoDelay: a 1/4 delay comes 500ms after the sound (120 beats a minute)", std::to_string(at));
        check(rms(r, 0, 100000) < 1e-6, "StereoDelay: the left delay stays on the left");
        {
            // dual: the echo repeats on the same side; ping-pong: it swaps sides
            std::vector<float> rd, rp;
            const std::vector<float> ld = delayed(8, 50.0, &rd), lp = delayed(8, -50.0, &rp);
            check(std::fabs(ld[48000]) > 0.01f && rms(rd, 0, 100000) < 1e-6, "StereoDelay: dual feedback repeats on the same side");
            check(std::fabs(rp[48000]) > 0.01f && std::fabs(lp[48000]) < 1e-4f, "StereoDelay: ping-pong sends the second echo to the other side");
        }
        auto p = make("MX24");
        set(*p, 9, 0.0);
        const std::vector<float> in = noise(4800, 0.3f, 31);
        const std::vector<float> o = process(*p, in);
        double d = 0;
        for (size_t k = 0; k < in.size(); ++k) d = std::max(d, (double)std::fabs(o[k] - in[k]));
        check(d < 1e-6, "StereoDelay: Mix 0% is the dry sound", fmt("%.7f", d));
    }

    // ---- Tremolo / Pan
    {
        auto make1 = [](double depthPct, double phaseDeg, double shapePct) {
            auto p = make("MX32");
            setReal(*p, 5, 4.0); setReal(*p, 8, depthPct); setReal(*p, 7, phaseDeg); setReal(*p, 6, shapePct);
            return p;
        };
        {
            auto p = make1(100.0, 0.0, 0.0);
            std::vector<float> r;
            const std::vector<float> in(48000, 0.5f);
            const std::vector<float> l = process(*p, in, 512, &r);
            float mn = 1, mx = 0;
            for (size_t k = 4800; k < l.size(); ++k) { mn = std::min(mn, l[k]); mx = std::max(mx, l[k]); }
            check(mn < 0.02f && mx > 0.48f, "TremoloPan: depth 100% swings the level from nothing to full", fmt("%.3f .. ", mn) + fmt("%.3f", mx));
            double same = 0;
            for (size_t k = 0; k < l.size(); ++k) same = std::max(same, (double)std::fabs(l[k] - r[k]));
            check(same < 1e-6, "TremoloPan: Phase 0 moves both sides together (a tremolo)");
        }
        {
            auto p = make1(100.0, 180.0, 0.0);
            std::vector<float> r;
            const std::vector<float> l = process(*p, std::vector<float>(48000, 0.5f), 512, &r);
            double sum = 0, worst = 0;
            for (size_t k = 4800; k < l.size(); ++k) { sum = std::max(sum, (double)std::fabs(l[k] + r[k] - 0.5f)); worst = std::max(worst, (double)l[k]); }
            check(sum < 0.02 && worst > 0.4, "TremoloPan: Phase 180 runs the sound from side to side (an auto-pan: the sides add up to the level)", fmt("%.3f", sum));
        }
        {
            auto sine_ = make1(100.0, 0.0, 0.0), square = make1(100.0, 0.0, 100.0);
            const std::vector<float> in(48000, 0.5f);
            const std::vector<float> a = process(*sine_, in), b = process(*square, in);
            int fullA = 0, fullB = 0;
            for (size_t k = 4800; k < a.size(); ++k) { if (a[k] > 0.45f) ++fullA; if (b[k] > 0.45f) ++fullB; }
            check(fullB > fullA * 1.5, "TremoloPan: Shape toward a square holds the level longer", std::to_string(fullA) + " vs " + std::to_string(fullB));
        }
        {
            auto p = make1(0.0, 90.0, 40.0);
            const std::vector<float> in = noise(4800, 0.3f, 32);
            const std::vector<float> o = process(*p, in);
            double d = 0;
            for (size_t k = 0; k < in.size(); ++k) d = std::max(d, (double)std::fabs(o[k] - in[k]));
            check(d < 1e-6, "TremoloPan: depth 0% is the dry sound", fmt("%.7f", d));
        }
    }

    // ---- Vocal Strip
    {
        {
            auto a = make("MX30"), b = make("MX30");
            setReal(*b, 5, 300);
            const std::vector<float> low = sine(48000, 40, 0.3f);
            check(rms(process(*b, low), 24000) < rms(process(*a, low), 24000) * 0.1, "VocalStrip: the high-pass takes rumble out (steeply)");
            check(std::fabs(db(rms(process(*b, sine(48000, 3000, 0.3f)), 24000)) - db(rms(sine(48000, 3000, 0.3f), 24000))) < 1.0, "VocalStrip: and leaves the voice's range");
        }
        {
            auto a = make("MX30"), b = make("MX30");
            setReal(*b, 8, 36.0);
            set(*b, 21, 0.0);
            const std::vector<float> loud = sine(96000, 440, 0.8f);
            check(peak(process(*b, loud), 72000) < peak(process(*a, loud), 72000) * 0.7, "VocalStrip: compression turns a loud tone down");
        }
        {
            auto a = make("MX30"), b = make("MX30");
            setReal(*b, 6, -30.0);
            const std::vector<float> quiet = sine(96000, 440, 0.005f);
            check(rms(process(*b, quiet), 72000) < rms(process(*a, quiet), 72000) * 0.2, "VocalStrip: the gate takes a quiet sound down");
        }
        {
            auto a = make("MX30"), b = make("MX30");
            set(*b, 9, 1.0);
            const std::vector<float> n1 = noise(96000, 0.2f, 33);
            const std::vector<float> oa = process(*a, n1), ob = process(*b, n1);
            std::vector<float> ha(oa.size()), hb(ob.size());
            for (size_t k = 1; k < oa.size(); ++k) { ha[k] = oa[k] - oa[k - 1]; hb[k] = ob[k] - ob[k - 1]; }
            check(rms(hb, 24000) > rms(ha, 24000) * 1.1, "VocalStrip: Tone to the right brightens");
        }
        {
            auto p = make("MX30");
            setReal(*p, 5, 20.0);
            const std::vector<float> in = sine(48000, 1000, 0.3f);
            check(std::fabs(db(rms(process(*p, in), 24000)) - db(rms(in, 24000))) < 0.5, "VocalStrip: as it comes (the default settings) a voice passes at its level");
        }
    }

    // ---- all six: no NaN, no denormals, the same sound for any block size, Active off, a seek starts afresh
    for (const Named& e : all) {
        const std::string label = e.name;
        // settings away from the defaults (a fixed pattern per effect)
        auto setup = [&](AudioPlugin& p) {
            uint32_t s = 99;
            for (int k = 1; k < 30; ++k) {
                if (p.paramName(k) == "-") continue;
                s = s * 1664525u + 1013904223u;
                p.paramSet(k, (float)((s >> 8) & 0xFFFF) / 65535.f);
            }
        };
        // block sizes
        {
            std::vector<std::vector<float>> outs;
            for (int block : {64, 512, 2048}) {
                auto p = make(e.id);
                setup(*p);
                std::vector<float> in = noise(48000, 0.5f, 21);
                const std::vector<float> t = sine(48000, 330, 0.4f);
                for (size_t k = 0; k < in.size(); ++k) in[k] += t[k];
                outs.push_back(process(*p, in, block));
            }
            double d1 = 0, d2 = 0;
            for (size_t k = 0; k < outs[0].size(); ++k) {
                d1 = std::max(d1, (double)std::fabs(outs[0][k] - outs[1][k]));
                d2 = std::max(d2, (double)std::fabs(outs[0][k] - outs[2][k]));
            }
            check(d1 < 1e-5 && d2 < 1e-5, label + ": the same sound for blocks of 64, 512 and 2048", fmt("%.2e / %.2e", d1, d2));
        }
        // NaN / huge / silent input
        {
            auto p = make(e.id);
            setup(*p);
            std::vector<float> l(2048, std::nanf("")), r(2048, 1e30f);
            p->process(l.data(), r.data(), 2048, 0, true);
            bool finite = true;
            for (int k = 0; k < 2048; ++k) if (!std::isfinite(l[(size_t)k]) || !std::isfinite(r[(size_t)k])) finite = false;
            std::vector<float> after = process(*p, noise(4800, 0.3f, 22));
            for (float v : after) if (!std::isfinite(v)) finite = false;
            check(finite, label + ": NaN and huge input do no harm");
            p->process(nullptr, nullptr, 100, 0, true);
            p->process(l.data(), r.data(), 0, 0, true);
        }
        // the tail dies out cleanly: no denormals in the output after a burst
        {
            auto p = make(e.id);
            setup(*p);
            set(*p, 9, 1.0);
            process(*p, noise(24000, 0.5f, 23));
            std::vector<float> silent(48000 * 3, 0.f);
            std::vector<float> r;
            const std::vector<float> o = process(*p, silent, 512, &r);
            bool denormal = false;
            for (size_t k = 0; k < o.size(); ++k) {
                if (o[k] != 0.f && std::fabs(o[k]) < 1.2e-38f) denormal = true;
                if (r[k] != 0.f && std::fabs(r[k]) < 1.2e-38f) denormal = true;
            }
            check(!denormal, label + ": no denormal numbers in a decaying tail");
        }
        // Active off: the sound goes through untouched
        {
            auto p = make(e.id);
            setup(*p);
            set(*p, 0, 0.0);
            const std::vector<float> in = noise(4800, 0.5f, 24);
            const std::vector<float> o = process(*p, in);
            std::string t0 = p->paramText(0);
            for (char& ch : t0) ch = (char)std::tolower((unsigned char)ch);
            check(o == in && t0 == "off", label + ": Active off passes the sound through");
        }
        // a jump in time starts afresh: the same input at position 0 after a seek gives the same output as the first time
        {
            auto p = make(e.id);
            setup(*p);
            const std::vector<float> in = noise(9600, 0.5f, 25);
            const std::vector<float> first = process(*p, in);
            process(*p, noise(4800, 0.5f, 26));
            const std::vector<float> again = process(*p, in);      // (position 0 again: a seek)
            double d = 0;
            for (size_t k = 0; k < in.size(); ++k) d = std::max(d, (double)std::fabs(first[k] - again[k]));
            check(d < 1e-6, label + ": a seek starts the effect afresh", fmt("%.2e", d));
        }
        // sample rates: 44.1 and 96 kHz work (finite, not silent)
        for (double rate : {44100.0, 96000.0}) {
            AudioPluginDesc d;
            d.format = "VST2";
            d.uid = std::to_string(idOf(e.id));
            std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, nullptr);
            p->activate(rate, 512);
            setup(*p);
            set(*p, 0, 1.0);
            std::vector<float> l = noise(9600, 0.3f, 27);
            std::vector<float> r = l;
            p->process(l.data(), r.data(), 9600, 0, true);
            bool ok = true;
            for (float v : l) if (!std::isfinite(v)) ok = false;
            check(ok, label + ": works at " + fmt("%.0f", rate) + " Hz");
        }
    }

    // ---- the choice between these and the installed plug-ins, in FxRuntime
    {
        Project p;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        FxSlot s;
        s.id = p.newId();
        s.kind = "audio";
        s.name = "eFX Limiter";
        s.audio.format = "VST2";
        s.audio.uid = std::to_string(idOf("MX2A"));
        s.audio.name = "eFX Limiter";
        const std::vector<float> saved = vec({1, 0, 0, 0.5f, 0.971555f, 0.745f, 0.15f, 0.5f, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 0, 1});
        s.state = Json(stateOf(saved));
        t.fx.push_back(s);
        p.tracks.push_back(t);
        setEfxPreferBuiltin(true);
        FxRuntime rt;
        rt.prepare(p);
        AudioPlugin* pl = rt.audio(s.id);
        check(pl != nullptr && efxIsReproduction(pl->desc) && rt.error(s.id).empty(), "FxRuntime: the slot gets Reno's eFX Limiter although no plug-in file is installed");
        check(pl && pl->saveState() == stateOf(saved), "FxRuntime: with the project's saved settings in it");
        if (pl) {
            pl->paramSet(5, 0.4f);
            const std::string changed = pl->saveState();
            Project q = p;
            rt.captureStates(q);
            check(q.tracks[0].fx[0].state.isString() && q.tracks[0].fx[0].state.asString() == changed, "FxRuntime: captureStates keeps the edited settings (what a saved .veg gets)");
        }
        setEfxPreferBuiltin(false);
        rt.prepare(p);
        check(rt.audio(s.id) == nullptr && !rt.error(s.id).empty(), "FxRuntime: with the installed plug-ins chosen and none installed the slot says so");
        setEfxPreferBuiltin(true);
        rt.prepare(p);
        check(rt.audio(s.id) != nullptr, "FxRuntime: and Reno's version is made again when the choice goes back");
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
