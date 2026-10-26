// pitch_align_samples_test: where Reno puts pitch-shifted sound against VEGAS's own, on VEGAS's renders.
//
//   pitch_align_samples_test [samples folder [cmp folder]]
//     (defaults $RENO_SAMPLES and $RENO_CMP; whatever is missing is skipped; both are only read)
//
// 1. The sample projects with pitched sound events, all of them short (stutters), against the audio proxies VEGAS keeps beside them
//    ("<project>.veg.sfap0", the mix of the whole project; Wave64, 32-bit float): the whole mix must correlate to VEGAS's,
//    and so must each pitched event of 40 ms or more that no other sound overlaps. (Only projects whose media is there are used.)
// 2. VEGAS's render of the sweep project (cmp/vegas/nofx_loud_sweep.wav, 1 s events of speech, all three élastique modes at
//    +-1 and +-3 semitones): over the first 30 ms of an event Reno's waveform must correlate to VEGAS's the way two
//    copies of one sound do (0.95 at the right delay).
#include "../../src/vegas/vegas.h"
#include "../../src/engine/engine.h"
#include "../../src/core/base.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include "../testdata.h"

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : " - ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A Wave64 file of 32-bit float stereo (VEGAS's .sfap0): left and right. Empty if it is not one.
static bool readW64(const std::string& path, std::vector<float>& L, std::vector<float>& R)
{
    std::string data;
    if (!readFile(path, data) || data.size() < 40 || data.compare(0, 4, "riff") != 0) return false;
    size_t at = 40;
    int channels = 0, bits = 0, format = 0;
    while (at + 24 <= data.size()) {
        uint64_t size = 0;
        std::memcpy(&size, data.data() + at + 16, 8);
        if (size < 24 || at + size > data.size() + 8) break;
        const std::string id = data.substr(at, 4);
        const char* body = data.data() + at + 24;
        if (id == "fmt ") {
            uint16_t f = 0, c = 0, b = 0;
            std::memcpy(&f, body, 2);
            std::memcpy(&c, body + 2, 2);
            std::memcpy(&b, body + 14, 2);
            format = f;
            channels = c;
            bits = b;
        } else if (id == "data" && channels == 2 && bits == 32 && (format == 3 || format == 0xfffe)) {
            const size_t n = std::min<size_t>((size_t)(size - 24), data.size() - at - 24) / 8;
            L.resize(n);
            R.resize(n);
            for (size_t k = 0; k < n; ++k) {
                std::memcpy(&L[k], body + k * 8, 4);
                std::memcpy(&R[k], body + k * 8 + 4, 4);
            }
            return true;
        }
        at += (size_t)((size + 7) & ~(uint64_t)7);
    }
    return false;
}

// A 32-bit float PCM .wav (VEGAS's own renders in the cmp folder): left and right.
static bool readWav(const std::string& path, std::vector<float>& L, std::vector<float>& R)
{
    std::string d;
    if (!readFile(path, d)) return false;
    size_t at = 12;
    int ch = 2, bits = 32;
    while (at + 8 <= d.size()) {
        uint32_t sz;
        std::memcpy(&sz, d.data() + at + 4, 4);
        if (d.compare(at, 4, "fmt ") == 0) {
            uint16_t c, b;
            std::memcpy(&c, d.data() + at + 10, 2);
            std::memcpy(&b, d.data() + at + 22, 2);
            ch = c;
            bits = b;
        }
        if (d.compare(at, 4, "data") == 0 && bits == 32 && ch == 2) {
            const size_t n = std::min<size_t>(sz, d.size() - at - 8) / 8;
            L.resize(n);
            R.resize(n);
            for (size_t k = 0; k < n; ++k) {
                std::memcpy(&L[k], d.data() + at + 8 + k * 8, 4);
                std::memcpy(&R[k], d.data() + at + 8 + k * 8 + 4, 4);
            }
            return true;
        }
        at += 8 + sz + (sz & 1);
    }
    return false;
}

// Reno's stereo mix of the whole project (effects bypassed, no quick fades), n samples
static void renoMix(const Project& pr, size_t n, std::vector<float>& L, std::vector<float>& R)
{
    Project p = pr;
    p.quickFade = 0;
    FxRuntime fx;
    fx.prepare(p);
    MediaPool pool;
    AudioEngine audio(pool, fx);
    audio.bypassFx = true;
    FinalRenderScope final;
    L.assign(n, 0.f);
    R.assign(n, 0.f);
    std::vector<float> l(1024), r(1024);
    for (size_t s = 0; s < n; s += 1024) {
        const int k = (int)std::min<size_t>(1024, n - s);
        audio.render(p, (int64_t)s, k, l.data(), r.data(), true);
        std::memcpy(&L[s], l.data(), (size_t)k * sizeof(float));
        std::memcpy(&R[s], r.data(), (size_t)k * sizeof(float));
    }
    fx.clear();
}

// Correlation of the two stereo signals over [a, b)
static double corr(const std::vector<float>& rl, const std::vector<float>& rr, const std::vector<float>& vl, const std::vector<float>& vr, int64_t a, int64_t b)
{
    double dot = 0, er = 0, ev = 0;
    for (int64_t k = a; k < b && k < (int64_t)vl.size() && k < (int64_t)rl.size(); ++k) {
        dot += (double)rl[(size_t)k] * vl[(size_t)k] + (double)rr[(size_t)k] * vr[(size_t)k];
        er += (double)rl[(size_t)k] * rl[(size_t)k] + (double)rr[(size_t)k] * rr[(size_t)k];
        ev += (double)vl[(size_t)k] * vl[(size_t)k] + (double)vr[(size_t)k] * vr[(size_t)k];
    }
    return dot / (std::sqrt(er * ev) + 1e-20);
}

// (how alike Reno's pitch shifter and VEGAS's are: the weakest of the projects this was fitted on)
static const double kMinMix = 0.30, kMinEvent = 0.20;

static void projects(const std::string& folder)
{
    int ran = 0;
    for (const std::string& veg : testVegFiles(folder)) {
        Project p;
        const VegasImportResult r = fileExists(veg + ".sfap0") ? importVegasProject(veg, p) : VegasImportResult();
        std::vector<float> vl, vr;
        bool media = r.ok;
        for (const auto& m : p.media) media = media && !m.offline;
        // (projects whose pitched sound events are all short, under 0.25 s: stutters)
        bool pitched = false, allShort = true;
        for (const auto& t : p.tracks)
            for (const auto& e : t.events)
                if (t.type == TrackType::Audio && std::fabs(e.pitch) >= 0.5) {
                    pitched = true;
                    allShort = allShort && e.length < 0.25;
                }
        pitched = pitched && allShort;
        // (the proxy holds VEGAS's mix with its effects, nested projects included; Reno's here has them bypassed, so projects with
        // sound effects, buses or nested projects are left out)
        bool plainMix = p.buses.empty() && p.audioOut.empty();
        for (const auto& m : p.media) plainMix = plainMix && !endsWithNoCase(m.path, ".veg");
        if (!r.ok || !media || !pitched || !plainMix || !readW64(veg + ".sfap0", vl, vr)) continue;
        ++ran;
        const std::string c = pathFileName(veg);
        const size_t n = std::min<size_t>(vl.size(), 48000 * 20);
        std::vector<float> rl, rr;
        renoMix(p, n, rl, rr);
        const double all = corr(rl, rr, vl, vr, 0, (int64_t)n);
        check(all >= kMinMix, strfmt("%s: the whole mix correlates to VEGAS's (at least %.2f)", c.c_str(), kMinMix), strfmt("%.3f", all));
        // the pitched events one by one (those nothing else overlaps)
        for (const auto& t : p.tracks)
            for (const auto& e : t.events) {
                if (t.type != TrackType::Audio || std::fabs(e.pitch) < 0.5) continue;
                bool alone = true;
                for (const auto& t2 : p.tracks)
                    if (t2.type == TrackType::Audio)
                        for (const auto& o : t2.events)
                            if (&o != &e && o.start < e.end() - 1e-6 && o.end() > e.start + 1e-6) alone = false;
                const int64_t a = timeToSample(e.start, 48000), b = std::min<int64_t>(timeToSample(e.end(), 48000), (int64_t)n);
                if (!alone || b - a < 1920) continue;          // (events of 40 ms and longer: a shorter one has too little to correlate)
                const double cc = corr(rl, rr, vl, vr, a, b);
                check(cc >= kMinEvent, strfmt("%s: the event %+g semitones at %.3f s correlates to VEGAS's (at least %.2f)", c.c_str(), e.pitch, e.start, kMinEvent), strfmt("%.3f", cc));
            }
    }
    if (!ran) std::printf("SKIP: no sample project with pitched sound, a VEGAS audio proxy (.sfap0) and its media under %s\n", folder.c_str());
}

static void sweep(const std::string& cmp)
{
    std::vector<float> vl, vr;
    const std::string veg = cmp + "/loud/loud_sweep.veg", wav = cmp + "/vegas/nofx_loud_sweep.wav";
    Project p;
    std::string err;
    if (!fileExists(veg) || !readWav(wav, vl, vr) || !loadProjectFile(veg, p, &err)) {
        std::printf("SKIP loud sweep (not there)\n");
        return;
    }
    std::vector<float> rl, rr;
    renoMix(p, (size_t)(1.5 * 48000 * 42), rl, rr);              // the speech events: 3 modes x 14 events, 1.5 s apart
    struct Want { int mode, index; const char* what; double min; };
    // events within a mode: r1.25 r1.5 r2 r3 r4 r0.5 p-12 p-7 p-3 p-1 p+1 p+3 p+7 p+12
    const Want wants[] = {{0, 10, "Efficient +1", 0.90}, {0, 11, "Efficient +3", 0.85}, {0, 9, "Efficient -1", 0.80}, {0, 8, "Efficient -3", 0.55},
                          {1, 10, "Pro +1", 0.90},       {1, 11, "Pro +3", 0.85},       {1, 9, "Pro -1", 0.90},       {1, 8, "Pro -3", 0.75},
                          {2, 10, "Soloist +1", 0.85}};
    for (const Want& w : wants) {
        const int64_t a = (int64_t)std::llround(1.5 * (w.mode * 14 + w.index) * 48000);
        const double cc = corr(rl, rr, vl, vr, a, a + 1440);
        check(cc >= w.min, strfmt("loud sweep speech, %s semitones: the first 30 ms correlate to VEGAS's (at least %.2f)", w.what, w.min), strfmt("%.3f", cc));
    }
}

int main(int argc, char** argv)
{
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    projects(argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES"));
    sweep(argc > 2 ? argv[2] : testDataDir("RENO_CMP"));
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
