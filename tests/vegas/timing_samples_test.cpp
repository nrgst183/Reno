// timing_samples_test: compares Reno's mix of the sample VEGAS projects with VEGAS's own mix of them.
//
//   timing_samples_test [folder]      (default $RENO_SAMPLES, searched recursively; it is only read)
//
// VEGAS Pro 18 keeps its mix of a project used as media in "<project>.veg.sfap0" next to it (Wave64, 32-bit float,
// stereo, 48 kHz). The projects with such a file next to them, whose media is there too, are checked (the others are
// skipped, and so is everything when there are none).
//
// For every plain event (no pitch change, no stretching) of the first audio track that no other sound event overlaps, the
// lag between Reno's mix and VEGAS's is found by cross-correlation within +-2500 samples; it must be 0 or 1. The rest is
// under half a sample: VEGAS resamples each event's 44.1 kHz sound afresh from its first sample, Reno reads one continuous
// resampling of the file. A project whose sound events are all Classic-stretched .wav files must match VEGAS's sample for sample (to 3%).
// Event first samples are checked against VEGAS's grid too.
#include "../../src/vegas/vegas.h"
#include "../../src/engine/engine.h"
#include <cmath>
#include <cstdio>
#include <cstring>
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

// A Wave64 file of 32-bit float stereo: the samples' mono mix (left + right) / 2. Empty if it is not one.
static std::vector<double> readW64(const std::string& path)
{
    std::vector<double> out;
    std::string data;
    if (!readFile(path, data) || data.size() < 40 || data.compare(0, 4, "riff") != 0) return out;
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
            out.resize(n);
            for (size_t k = 0; k < n; ++k) {
                float l = 0, r = 0;
                std::memcpy(&l, body + k * 8, 4);
                std::memcpy(&r, body + k * 8 + 4, 4);
                out[k] = 0.5 * ((double)l + r);
            }
            return out;
        }
        at += (size_t)((size + 7) & ~(uint64_t)7);
    }
    return out;
}

// Reno's mix of the whole project (mono), as long as VEGAS's.
static std::vector<double> renoMix(const Project& p, size_t n)
{
    MediaPool pool;
    FxRuntime fx;
    AudioEngine audio(pool, fx);
    std::vector<double> out(n, 0.0);
    std::vector<float> l(1024), r(1024);
    for (size_t s = 0; s < n; s += 1024) {
        const int k = (int)std::min<size_t>(1024, n - s);
        audio.render(p, (int64_t)s, k, l.data(), r.data(), false);
        for (int i = 0; i < k; ++i) out[s + (size_t)i] = 0.5 * ((double)l[(size_t)i] + r[(size_t)i]);
    }
    return out;
}

// The lag (samples Reno's mix is later than VEGAS's) that lines up [a, b) best, within +-maxLag, and how well. The
// nearest of equally good lags wins (a stutter repeats the same sound: its events line up one event apart as well).
static int bestLag(const std::vector<double>& reno, const std::vector<double>& vegas, int64_t a, int64_t b, int maxLag, double* corr)
{
    double ev = 0;
    for (int64_t k = a; k < b; ++k) ev += vegas[(size_t)k] * vegas[(size_t)k];
    int best = 0;
    double top = -2;
    for (int i = 0; i <= 2 * maxLag; ++i) {
        const int lag = (i % 2 ? 1 : -1) * ((i + 1) / 2);
        double dot = 0, er = 0;
        for (int64_t k = a; k < b; ++k) {
            const int64_t j = k + lag;
            const double x = (j >= 0 && j < (int64_t)reno.size()) ? reno[(size_t)j] : 0.0;
            dot += vegas[(size_t)k] * x;
            er += x * x;
        }
        const double c = dot / (std::sqrt(ev * er) + 1e-20);
        if (c > top + 0.002) { top = c; best = lag; }
    }
    if (corr) *corr = top;
    return best;
}

static bool plain(const Event& e) { return std::fabs(e.pitch) < 1e-9 && std::fabs(e.rate - 1.0) < 1e-9 && !e.reverse; }

int main(int argc, char** argv)
{
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    const std::string folder = argc > 1 ? argv[1] : testDataDir("RENO_SAMPLES");
    int ran = 0;
    for (const std::string& veg : testVegFiles(folder)) {
        const std::string sfap = veg + ".sfap0";
        const std::string c = pathFileName(veg);
        Project p;
        const VegasImportResult r = fileExists(sfap) ? importVegasProject(veg, p) : VegasImportResult();
        const std::vector<double> vegas = r.ok ? readW64(sfap) : std::vector<double>();
        bool media = r.ok;
        for (const auto& m : p.media) media = media && !m.offline;
        if (!r.ok || vegas.empty() || !media) continue;
        // (a project with nested projects is left out: the proxy holds their effects, which Reno's mix here bypasses)
        bool nested = false;
        for (const auto& m : p.media) nested = nested || endsWithNoCase(m.path, ".veg");
        if (nested) continue;
        ++ran;
        // the media's own length and rates were read from the file (VEGAS reads the file too)
        for (const auto& m : p.media)
            if (endsWithNoCase(m.path, ".mp4"))
                check(m.info.duration > 1.0 && m.info.sampleRate > 0 && m.info.audioDelaySamples >= 0 && m.info.fps > 1.0,
                      c + ": the .mp4's length, rates and AAC delay are known after opening",
                      strfmt("%.3f s, %d Hz, delay %d, %.3f fps", m.info.duration, m.info.sampleRate, m.info.audioDelaySamples, m.info.fps));
        const std::vector<double> reno = renoMix(p, vegas.size());
        // a project whose sound events are all Classic-stretched .wav files: VEGAS's stretcher, sample for sample
        {
            int sounds = 0, classic = 0;
            for (const auto& t : p.tracks)
                if (t.type == TrackType::Audio)
                    for (const auto& e : t.events) {
                        ++sounds;
                        const MediaItem* om = p.findMedia(e.mediaId);
                        classic += (e.stretchMethod == "classic" && std::fabs(e.rate - 1.0) > 1e-6 && om && endsWithNoCase(om->path, ".wav")) ? 1 : 0;
                    }
            if (sounds > 0 && classic == sounds) {
                double num = 0, den = 0;
                for (size_t k = 0; k < vegas.size(); ++k) {
                    num += (reno[k] - vegas[k]) * (reno[k] - vegas[k]);
                    den += vegas[k] * vegas[k];
                }
                double corr = 0;
                const int lag = bestLag(reno, vegas, 0, (int64_t)vegas.size(), 1000, &corr);
                check(lag == 0 && std::sqrt(num / std::max(den, 1e-20)) < 0.03, c + ": Classic-stretched events are VEGAS's sample for sample",
                      strfmt("lag %d, corr %.5f, difference %.4f of the signal", lag, corr, std::sqrt(num / std::max(den, 1e-20))));
                continue;
            }
        }
        const Track* tr = nullptr;
        for (const auto& t : p.tracks)
            if (!tr && t.type == TrackType::Audio) tr = &t;
        if (!tr) continue;
        // events that nothing else overlaps (so the mix is that event alone)
        auto alone = [&](const Event& e) {
            for (const auto& t : p.tracks)
                if (t.type == TrackType::Audio)
                    for (const auto& o : t.events)
                        if (&o != &e && o.start < e.end() - 1e-6 && o.end() > e.start + 1e-6) return false;
            return true;
        };
        int events = 0, aligned = 0;
        std::string lags;
        for (const auto& e : tr->events) {
            if (!plain(e) || !alone(e)) continue;
            const int64_t a = timeToSample(e.start, 48000) + 8, b = std::min<int64_t>(timeToSample(e.end(), 48000) - 8, (int64_t)vegas.size());
            if (b - a < 64) continue;
            ++events;
            double corr = 0;
            const int lag = bestLag(reno, vegas, a, b, 2500, &corr);
            lags += strfmt(" %+d(%.3f)", lag, corr);
            if (std::abs(lag) <= 1 && corr > 0.99) ++aligned;
        }
        if (events) check(aligned == events, strfmt("%s: all %d plain sound events line up with VEGAS's (within a sample)", c.c_str(), events), lags);
        // where the events start: the samples before an event's first sample are the event before (VEGAS's grid); the
        // first few after it differ a little (VEGAS resamples each event afresh), the ones before it must not
        int edges = 0, sharp = 0;
        for (const auto& e : tr->events) {
            const int64_t s = timeToSample(e.start, 48000);
            if (!plain(e) || !alone(e) || s < 8 || s + 8 >= (int64_t)vegas.size()) continue;
            double peak = 1e-9, before = 0;
            for (int64_t q = s - 40; q < s + 40; ++q)
                if (q >= 0 && q < (int64_t)vegas.size()) peak = std::max(peak, std::fabs(vegas[(size_t)q]));
            for (int64_t q = s - 3; q < s; ++q) before = std::max(before, std::fabs(reno[(size_t)q] - vegas[(size_t)q]));
            ++edges;
            if (before / peak < 0.05) ++sharp;
        }
        if (edges) check(sharp == edges, strfmt("%s: the %d event starts fall on the same sample as VEGAS's", c.c_str(), edges), strfmt("%d of %d", sharp, edges));
    }
    if (!ran) std::printf("SKIP: no sample project with a VEGAS audio proxy (.sfap0) and its media in %s\n", folder.c_str());
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
