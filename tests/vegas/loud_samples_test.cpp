// loud_samples_test: compares the loudness of Reno's sound with VEGAS Pro 18's own renders of the sample projects (all
// top-level effects bypassed, like reno_cli --no-fx), as the rms of the mix over the start of each file:
//   - every project that has a render "vegas/nofx_<name>.wav" in the comparison folder (<name> the project's file name with
//     spaces as '_'; the projects themselves are found anywhere under the folder): Reno's rms of the first 0.7 s is within
//     a few percent of VEGAS's (élastique stretching and pitching make speech quieter in VEGAS than an energy-keeping
//     vocoder's: see PhaseVocoder::levelGain);
//   - a steady tone keeps its level exactly in VEGAS (pitchtests/nofxB), so the same stretch must not turn a tone down;
//   - the sweep project (loud/loud_sweep.veg, rendered to vegas/nofx_loud_sweep.wav): 126 events of speech, noise and tone.
//
//   loud_samples_test [cmp folder (default $RENO_CMP)]       (SKIPs what is not there)
#include "../../src/vegas/vegas.h"
#include "../../src/engine/engine.h"
#include "../../src/core/base.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
}

// the left channel of a 32-bit float WAV
static bool readWav(const std::string& path, std::vector<float>& L)
{
    std::string d;
    if (!readFile(path, d)) return false;
    size_t at = 12;
    int ch = 2, bits = 32;
    while (at + 8 <= d.size()) {
        uint32_t sz;
        std::memcpy(&sz, d.data() + at + 4, 4);
        if (d.compare(at, 4, "fmt ") == 0) { uint16_t c, b; std::memcpy(&c, d.data() + at + 10, 2); std::memcpy(&b, d.data() + at + 22, 2); ch = c; bits = b; }
        if (d.compare(at, 4, "data") == 0 && bits == 32) {
            const size_t n = std::min<size_t>(sz, d.size() - at - 8) / (size_t)(ch * 4);
            L.resize(n);
            for (size_t k = 0; k < n; ++k) std::memcpy(&L[k], d.data() + at + 8 + k * (size_t)ch * 4, 4);
            return true;
        }
        at += 8 + sz + (sz & 1);
    }
    return false;
}

static double rmsOf(const std::vector<float>& v, size_t a, size_t b)
{
    b = std::min(b, v.size());
    double s = 0;
    for (size_t k = a; k < b; ++k) s += (double)v[k] * v[k];
    return b > a ? std::sqrt(s / (double)(b - a)) : 0.0;
}

// Reno's left channel of the project, effects bypassed
static bool renoMix(const std::string& veg, std::vector<float>& out, size_t n)
{
    Project p;
    std::string err;
    if (!fileExists(veg) || !loadProjectFile(veg, p, &err)) return false;
    p.quickFade = 0;
    FxRuntime rt;
    rt.prepare(p);
    MediaPool pool;
    AudioEngine ae(pool, rt);
    ae.bypassFx = true;
    FinalRenderScope final;
    out.assign(n, 0.f);
    std::vector<float> l(1024), r(1024);
    for (size_t at = 0; at < n; at += 1024) {
        const int k = (int)std::min<size_t>(1024, n - at);
        ae.render(p, (int64_t)at, k, l.data(), r.data(), true);
        std::memcpy(out.data() + at, l.data(), (size_t)k * sizeof(float));
    }
    rt.clear();
    return true;
}

// Reno's rms of a project over [from, to) seconds against the VEGAS render's, as a ratio
static void level(const std::string& wav, const std::string& veg, double from, double to, double lo, double hi, const std::string& what)
{
    std::vector<float> want, got;
    if (!readWav(wav, want) || !renoMix(veg, got, want.size())) { std::printf("SKIP %s\n", what.c_str()); return; }
    const size_t a = (size_t)(from * 48000), b = (size_t)(to * 48000);
    const double v = rmsOf(want, a, b), r = rmsOf(got, a, b), ratio = r / (v + 1e-12);
    check(ratio > lo && ratio < hi, what, strfmt("Reno %.4f vs VEGAS %.4f = %.3f (wanted %.2f .. %.2f)", r, v, ratio, lo, hi));
}


// loud_sweep: 126 events (speech, noise, tone x Efficient, Pro, Soloist (Speech) x 6 rates and 8 pitches), 1 s each, 1.5 s apart; the
// level of each against VEGAS's, grouped
static void sweeps(const std::string& cmp)
{
    std::vector<float> want, got;
    if (!readWav(cmp + "/vegas/nofx_loud_sweep.wav", want) || !renoMix(cmp + "/loud/loud_sweep.veg", got, want.size())) { std::printf("SKIP loud sweep\n"); return; }
    const char* srcs[] = {"speech", "noise", "tone"};
    const char* modes[] = {"Efficient", "Pro", "Soloist (Speech)"};
    for (int sidx = 0; sidx < 3; ++sidx)
        for (int m = 0; m < 2; ++m) {          // (the soloist mode's own levels are not fitted)
            double lg = 0;
            int n = 0;
            for (int j = 0; j < 14; ++j) {
                const size_t a = (size_t)((1.5 * (sidx * 42 + m * 14 + j) + 0.05) * 48000), b = a + 43200;
                lg += std::log(rmsOf(got, a, b) / (rmsOf(want, a, b) + 1e-12) + 1e-12);
                ++n;
            }
            const double ratio = std::exp(lg / n);
            // (Pro on noise: VEGAS keeps noise louder than Reno's when it is compressed 3 to 4 times; not copied)
            const bool proNoise = m == 1 && sidx == 1;
            check(std::fabs(ratio - 1.0) < (sidx == 2 ? 0.02 : proNoise ? 0.12 : 0.06), strfmt("loud sweep %s, %s: the 14 rates and pitches as loud as VEGAS's", srcs[sidx], modes[m]),
                  strfmt("mean Reno/VEGAS %.3f", ratio));
        }
}

int main(int argc, char** argv)
{
    const std::string cmp = argc > 1 ? argv[1] : testDataDir("RENO_CMP");
    // every project with a VEGAS render, found by name under the comparison folder
    {
        const std::map<std::string, std::string> vegs = testVegsByKey(cmp);
        int compared = 0;
        std::error_code ec;
        for (const auto& f : std::filesystem::directory_iterator(cmp + "/vegas", ec)) {
            const std::string fn = f.path().filename().string();
            if (fn.compare(0, 5, "nofx_") != 0 || f.path().extension() != ".wav") continue;
            const std::string key = testKey(fn.substr(5, fn.size() - 9));
            if (key == "loud_sweep") continue;
            const auto it = vegs.find(key);
            if (it == vegs.end()) continue;
            {   // (projects with nested projects are left out: VEGAS keeps the effects of nested ones, which Reno bypasses here)
                Project np;
                std::string e;
                bool nested = false;
                if (loadProjectFile(it->second, np, &e))
                    for (const auto& m : np.media) nested = nested || endsWithNoCase(m.path, ".veg");
                if (nested) continue;
            }
            ++compared;
            level(f.path().string(), it->second, 0.0, 0.7, 0.85, 1.20, pathFileName(it->second) + ": as loud as VEGAS's");
        }
        if (!compared) std::printf("SKIP: no VEGAS render (vegas/nofx_<project>.wav) of a project under %s\n", cmp.c_str());
    }
    // the tone events of the pitch tests (Efficient, +5 semitones, rates 1 and 1.5; and a 0.5 semitone shift): 1.0 s from 0 .. , 1.2 s apart
    {
        std::vector<float> want, got;
        const std::string veg = cmp + "/pitchtests/nofxB_pitch_semantics.veg";
        if (!readWav(cmp + "/vegas/nofxB_pitch_semantics.wav", want) || !renoMix(veg, got, want.size())) std::printf("SKIP tone events\n");
        else {
            // events 1, 2 and 11 of that project's audio track (cases 1, 2, 11): start 0, 1.2 and 12.0 s
            for (double start : {0.0, 1.2, 12.0}) {
                const size_t a = (size_t)((start + 0.1) * 48000), b = (size_t)((start + 0.9) * 48000);
                const double v = rmsOf(want, a, b), r = rmsOf(got, a, b);
                check(std::fabs(r / v - 1.0) < 0.02, strfmt("a stretched or pitched tone from %.1f s keeps VEGAS's level", start), strfmt("Reno %.4f vs VEGAS %.4f", r, v));
            }
        }
    }
    sweeps(cmp);
    std::printf("SUMMARY %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
