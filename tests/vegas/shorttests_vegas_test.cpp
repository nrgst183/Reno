// shorttests_vegas_test: compares Reno's mix of the very short test projects with VEGAS's own renders of them.
//
//   shorttests_vegas_test [folder with short_<mode>.veg and the media (default $RENO_CMP/shorttests)] [folder with vegas/short_<mode>.wav]
//
// short_1a/1b/1c/1d: one event of a 44.1 kHz AAC file in a 48 kHz project (4.4 ms, 1 s, offsets +10 us and +20 us);
// 2butt, 2gap, 2over: three of them butted, 1 sample apart, 1 sample overlapping. VEGAS reads the file from the
// offset's own sample with a cold start, at positions offset + k x 44100/48000 - 0.25 samples; Reno's mix must be
// within -35 dB of VEGAS's render. Skipped when the folders or media are missing.
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
}

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

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : testDataDir("RENO_CMP") + "/shorttests";
    const std::string wavDir = argc > 2 ? argv[2] : pathJoin(dir + "/..", "vegas");
    int ran = 0;
    for (const char* mode : {"1a", "1b", "1c", "1d", "2butt", "2gap", "2over"}) {
        const std::string veg = pathJoin(dir, std::string("short_") + mode + ".veg"), wav = pathJoin(wavDir, std::string("short_") + mode + ".wav");
        Project p;
        std::vector<float> want;
        if (!fileExists(veg) || !importVegasProject(veg, p).ok || !readWav(wav, want)) { std::printf("SKIP %s\n", mode); continue; }
        bool media = true;
        for (auto& m : p.media) {
            const std::string np = pathJoin(dir, m.path.substr(m.path.find_last_of("\\/") + 1));
            auto rd = openMedia(np, nullptr);
            if (!rd) { media = false; break; }
            m.path = np;
            m.info = rd->info();
            m.offline = false;
        }
        if (!media) { std::printf("SKIP %s (no media)\n", mode); continue; }
        ++ran;
        p.quickFade = 0;
        MediaPool pool;
        FxRuntime fx;
        AudioEngine audio(pool, fx);
        const size_t n = want.size();
        std::vector<float> l(n), r(n);
        for (size_t s = 0; s < n; s += 1024) audio.render(p, (int64_t)s, (int)std::min<size_t>(1024, n - s), l.data() + s, r.data() + s, false);
        double e = 0, g = 0;
        for (size_t k = 0; k < n; ++k) { const double d = (double)want[k] - l[k]; e += d * d; g += (double)want[k] * want[k]; }
        const double snr = 10 * std::log10((g + 1e-30) / (e + 1e-30));
        check(snr > 35.0, std::string("short_") + mode + ": Reno's mix is within -35 dB of VEGAS's render", strfmt("%.1f dB", snr));
    }
    // the pictures (short_5m: 0.5-frame events at frame 4k + 0.1 and 4k + 2.9; short_8m: 1.5-frame events at frame 4k + 0.5, ending
    // exactly on frame 4k + 2): which frames show anything - as VEGAS's frames (PNGs of its render) do: 5m on frames 3, 7, 11 ...
    // (the events covering a frame's start), 8m on frames 1, 5, 9 ... (the end tick is rounded up, so frame 4k + 2 shows nothing)
    for (const char* mode : {"5m", "8m"}) {
        const std::string veg = pathJoin(dir, std::string("short_") + mode + ".veg");
        Project p;
        if (!fileExists(veg) || !importVegasProject(veg, p).ok) { std::printf("SKIP %s\n", mode); continue; }
        bool media = true;
        for (auto& m : p.media) {
            const std::string np = pathJoin(dir, m.path.substr(m.path.find_last_of("\\/") + 1));
            auto rd = openMedia(np, nullptr);
            if (!rd) { media = false; break; }
            m.path = np;
            m.info = rd->info();
            m.offline = false;
        }
        if (!media) { std::printf("SKIP %s (no media)\n", mode); continue; }
        ++ran;
        MediaPool pool;
        FxRuntime fx;
        VideoEngine video(pool, fx);
        FrameOptions fo;
        fo.interactive = false;
        const bool m5 = std::string(mode) == "5m";
        int wrong = 0, frames = m5 ? 39 : 38;
        for (int k = 0; k < frames; ++k) {
            ImagePtr img = video.render(p, p.frameTime(k) + 1e-7, fo);
            bool lit = false;
            for (int y = 0; img && y < img->h && !lit; ++y) {
                const uint8_t* row = img->row(y);
                for (int x = 0; x < img->w * 4; x += 4) if (row[x] | row[x + 1] | row[x + 2]) { lit = true; break; }
            }
            const bool want = m5 ? (k % 4 == 3) : (k % 4 == 1);
            if (lit != want) ++wrong;
        }
        check(wrong == 0, std::string("short_") + mode + ": the frames that show an event are VEGAS's", strfmt("%d of %d frames differ", wrong, frames));
    }
    if (!ran) std::printf("SKIP: no short test projects in %s\n", dir.c_str());
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
