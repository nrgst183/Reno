// Tests VEGAS's peak files (.sfk) and audio proxies (.sfap0): src/media/vegasfiles.h.
//
//   vegasfiles_test <scratch folder> [folder of VEGAS's own files]
//
// The fixtures (folder in RENO_VEGAS_FILES; not in the repository) are VEGAS Pro 18's own files: two short .wav files with the .sfk VEGAS made
// for each, and the audio proxy VEGAS made of a small nested project (nested.veg.sfap0). Checked: time stamps, names,
// the peaks Reno makes from each .wav (equal to VEGAS's), a peak file and audio proxy written by Reno from the same
// input (equal to VEGAS's byte for byte), staleness, reading a proxy from anywhere, and bad files refused. With a second
// folder, every .sfk in it whose media is there too is compared with the peaks Reno makes of that media (a report; .wav
// files must match in at least 98% of the numbers).
#include "../../src/media/media.h"
#include "../../src/media/vegasfiles.h"
#include "../testdata.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::vegasfiles;

static int gPass = 0, gFail = 0;
static bool check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) {
        ++gPass;
        std::printf("PASS %s\n", name.c_str());
    } else {
        ++gFail;
        std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
    std::fflush(stdout);
    return ok;
}

static std::string fixtures() { return testDataDir("RENO_VEGAS_FILES"); }

static int64_t fromUnixMs(int64_t ms) { return ms * 10000 + 116444736000000000LL; }

// The peaks Reno makes of a media file (its sound read at its own rate), over 'frames' samples (0: its length).
static bool peaksOf(const std::string& media, int64_t frames, int channels, PeakFile& out, std::string* err, int64_t skip = 0)
{
    std::unique_ptr<MediaReader> r = openMedia(media, err);
    if (!r || !r->info().hasAudio) return false;
    const int rate = r->info().sampleRate;
    if (frames <= 0) frames = (int64_t)std::llround(r->info().duration * rate);
    if (channels <= 0) channels = std::min(2, std::max(1, r->info().channels));
    PeakBuilder b(channels, frames, peakRounding(r->info().audioCodec));
    std::vector<float> l(65536), rr(65536);
    for (int64_t at = 0; at < frames;) {
        const int n = (int)std::min<int64_t>(65536, frames - at);
        r->audio(at + skip, n, rate, l.data(), rr.data());
        const float* ch[2] = {l.data(), rr.data()};
        b.add(ch, n);
        at += n;
    }
    out = b.finish(0);
    return b.given() == frames && b.blocksDone() == out.blocks();
}

// How many of two peak files' numbers differ, and by how much at most.
static void comparePeaks(const PeakFile& a, const PeakFile& b, int64_t& differing, int& most, int64_t& total)
{
    differing = 0;
    most = 0;
    total = (int64_t)std::min(a.minmax.size(), b.minmax.size());
    for (int64_t k = 0; k < total; ++k) {
        const int d = std::abs((int)a.minmax[(size_t)k] - (int)b.minmax[(size_t)k]);
        if (d) ++differing;
        most = std::max(most, d);
    }
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: vegasfiles_test <scratch folder> [folder of VEGAS files]\n");
        return 2;
    }
    const std::string scratch = argv[1], fx = fixtures();
#ifndef _WIN32
    // the fixtures were written in January in California: local time 8 hours behind UTC
    setenv("TZ", "PST8", 1);
    tzset();
#endif

    // ---------------------------------------------------------------- times
    // (peaks_a.wav was last changed 2023-01-13 20:03:35.982 UTC; a video file 2022-12-20 05:57:58.925)
    check(dosTime(fromUnixMs(1673640215982LL), false) == 0xa072562du && dosTime(fromUnixMs(1673640215982LL), true) == 0x6072562du,
          "MS-DOS stamps as VEGAS writes them: 20:03:35.98 UTC is 20:03:36 (seconds rounded up to even), local 12:03:36",
          strfmt("%08x %08x", dosTime(fromUnixMs(1673640215982LL), false), dosTime(fromUnixMs(1673640215982LL), true)));
    check(dosTime(fromUnixMs(1671515878925LL), false) == 0x2f405594u && dosTime(fromUnixMs(1671515878925LL), true) == 0xaf405593u,
          "05:57:58.93 rounds up into the next minute (05:58:00), and the local stamp into the day before (21:58:00)");
    check(dosTime(0, false) == 0, "no time, no stamp");

    // ---------------------------------------------------------------- names
    check(peakFilePath("C:/a/song.wav") == "C:/a/song.sfk" && peakFilePath("x/Song.WAV") == "x/Song.sfk" && peakFilePath("x/clip.mp4") == "x/clip.mp4.sfk" &&
              peakFilePath("x/nested.veg") == "x/nested.veg.sfk" && peakFilePath("x/a.wav.mp3") == "x/a.wav.mp3.sfk" && peakFilePath("x/noext") == "x/noext.sfk",
          "peak file names: .wav replaced by .sfk, anything else gets .sfk added");
    check(audioProxyPath("p/nested.veg") == "p/nested.veg.sfap0", "audio proxy name: <project>.sfap0");
    {
        const std::string d = pathJoin(scratch, "names");
        std::filesystem::create_directories(std::filesystem::u8path(d));
        for (const char* f : {"clip.mp4", "clip.mp4.sfk", "clip.mp4-0-629000000-1.sfk", "clip.mp4-0-631000000-1.sfk", "clip.mp4-a-b-c.sfk", "clip.mp4x.sfk", "other.mp4.sfk"})
            writeFile(pathJoin(d, f), "x");
        const std::vector<std::string> c = peakFileCandidates(pathJoin(d, "clip.mp4"));
        check(c.size() == 3 && pathFileName(c[0]) == "clip.mp4.sfk" && pathFileName(c[1]) == "clip.mp4-0-631000000-1.sfk" &&
                  pathFileName(c[2]) == "clip.mp4-0-629000000-1.sfk",
              "peak file candidates: the plain one, then VEGAS 18's '-<stream>-<length>-<n>' ones, longest first", strfmt("%d found", (int)c.size()));
    }

    // ---------------------------------------------------------------- .sfk: VEGAS's own
    if (testDataMissing(pathJoin(fx, "peaks_b.sfk"))) {
        std::printf("\n%d passed, %d failed\n", gPass, gFail);
        return gFail ? 1 : 0;
    }
    const std::string peaks_a = pathJoin(fx, "peaks_a.wav"), peaks_b = pathJoin(fx, "peaks_b.wav");
    PeakFile v1, v2;
    std::string err;
    check(readPeakFile(pathJoin(fx, "peaks_a.sfk"), v1, &err), "VEGAS's peaks_a.sfk is read", err);
    check(v1.channels == 2 && v1.blockFrames == 256 && v1.bits == 16 && v1.frames == 1568 && v1.blocks() == 7 && v1.utcStamp == 0xa072562du &&
              v1.localStamp == 0x6072562du,
          "its header: 2 channels, 256 samples a peak, 16 bits, 1568 samples, 7 blocks, the stamps");
    check(readPeakFile(pathJoin(fx, "peaks_b.sfk"), v2, &err) && v2.frames == 9408 && v2.blocks() == 37, "VEGAS's peaks_b.sfk is read (9408 samples, 37 blocks)", err);
    {
        int64_t first, end;
        v2.blockRange(3, first, end);
        check(first == 762 && end == 1018, "block 3 of 37 over 9408 samples is samples 762..1017: spread evenly, sharing a sample with the next",
              strfmt("%lld..%lld", (long long)first, (long long)end - 1));
    }
    {
        PeakFile r1, r2;
        const bool ok = peaksOf(peaks_a, v1.frames, 2, r1, &err) && peaksOf(peaks_b, v2.frames, 2, r2, &err);
        int64_t d1 = 0, d2 = 0, t1 = 0, t2 = 0;
        int m1 = 0, m2 = 0;
        if (ok) {
            comparePeaks(r1, v1, d1, m1, t1);
            comparePeaks(r2, v2, d2, m2, t2);
        }
        check(ok && d1 == 0 && d2 == 0 && t1 == (int64_t)v1.minmax.size() && t2 == (int64_t)v2.minmax.size(),
              "the peaks Reno makes of both .wav files are VEGAS's, number for number",
              strfmt("%lld and %lld of %lld and %lld differ (most %d, %d) %s", (long long)d1, (long long)d2, (long long)t1, (long long)t2, m1, m2, err.c_str()));
        // the same peaks, stamped as VEGAS stamped them, written: VEGAS's file byte for byte
        r2.utcStamp = v2.utcStamp;
        r2.localStamp = v2.localStamp;
        const std::string out = pathJoin(scratch, "peaks_b.sfk");
        std::string a, b;
        check(writePeakFile(out, r2) && readFile(out, a) && readFile(pathJoin(fx, "peaks_b.sfk"), b) && a == b,
              "a peak file Reno writes is VEGAS's, byte for byte", strfmt("%zu / %zu bytes", a.size(), b.size()));
        check(!fileExists(out + ".part"), "nothing left over from writing it");
    }
    {
        // stamped with the media's time of last change: current while the file is as it was
        const std::string media = pathJoin(scratch, "tone.wav");
        EncodeSettings es;
        es.path = media;
        es.video = false;
        std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
        std::vector<float> l(4800), r(4800);
        for (int k = 0; k < 4800; ++k) l[(size_t)k] = r[(size_t)k] = (float)(0.25 * std::sin(k * 0.05));
        const bool made = w && w->writeAudio(l.data(), r.data(), 4800) && w->finish(&err);
        PeakFile p;
        const bool built = made && peaksOf(media, 0, 0, p, &err);
        PeakFile stamped = p;
        const int64_t ft = fileTime(media);
        stamped.utcStamp = dosTime(ft, false);
        stamped.localStamp = dosTime(ft, true);
        check(built && ft > 0 && peakFileCurrent(stamped, media) && !peakFileCurrent(p, media), "a peak file is current when stamped with its media's time", err);
        PeakFile old = stamped;
        old.utcStamp = dosTime(ft - 40000000LL, false);        // made from the file as it was 4 s before
        check(!peakFileCurrent(old, media), "and stale when the media has changed since");
        int lo = 0, hi = 0;
        for (int64_t b = 0; b < p.blocks(); ++b) {
            lo = std::min(lo, (int)p.lo(b, 0));
            hi = std::max(hi, (int)p.hi(b, 1));
        }
        check(std::abs(lo + 8192) <= 2 && std::abs(hi - 8191) <= 2, "a quarter-scale sine peaks at about -8192 / 8191 (floor(sample * 32768))", strfmt("%d %d", lo, hi));
    }
    // bad peak files
    {
        PeakFile p;
        const std::string bad = pathJoin(scratch, "bad.sfk");
        std::string g;
        readFile(pathJoin(fx, "peaks_b.sfk"), g);
        writeFile(bad, g.substr(0, 100));
        const bool cut = !readPeakFile(bad, p);
        writeFile(bad, "SFPK");
        const bool tiny = !readPeakFile(bad, p);
        std::string h = g;
        h[20] = (char)200;      // 200 channels
        writeFile(bad, h);
        const bool chans = !readPeakFile(bad, p);
        check(cut && tiny && chans && !readPeakFile(pathJoin(scratch, "missing.sfk"), p), "cut-short, too small, nonsense and missing peak files are refused");
    }

    // ---------------------------------------------------------------- .sfap0: VEGAS's own
    const std::string vp = pathJoin(fx, "nested.veg.sfap0");
    const int64_t vegTime = 0x01d928502561bb1bLL;          // nested.veg's time of last change when VEGAS made it
    AudioProxyInfo info;
    check(readAudioProxyInfo(vp, info, &err), "VEGAS's audio proxy is read", err);
    check(info.format == 3 && info.channels == 2 && info.rate == 48000 && info.bits == 32 && info.frames == 3683 && info.dataOffset == 104,
          "its format: 32-bit float stereo at 48 kHz, 3683 samples from byte 104",
          strfmt("%d %d %d %d %lld %lld", info.format, info.channels, info.rate, info.bits, (long long)info.frames, (long long)info.dataOffset));
    check(info.stamped && info.stampVersion == 1 && info.sourceTime == vegTime && info.stampChannels == 2 && info.stampRate == 48000 && info.stampBits == 32,
          "its staleness mark: version 1, the project's FILETIME, the format");
    check(audioProxyCurrent(info, vegTime) && !audioProxyCurrent(info, vegTime + 1) && !audioProxyCurrent(info, 0),
          "current for the project as it was then; stale once the project has been saved again (even 100 ns later)");
    {
        AudioProxyInfo other = info;
        other.stampRate = 44100;
        check(!audioProxyCurrent(other, vegTime), "stale when made in another format");
    }
    AudioProxyFile pf;
    std::vector<float> L(3683), R(3683);
    check(pf.open(vp, &err) && pf.read(0, 3683, L.data(), R.data()), "its samples are read", err);
    {
        std::vector<float> a(100), b(100), c(300), d(300);
        const bool ok = pf.read(1000, 100, a.data(), b.data()) && pf.read(3583, 300, c.data(), d.data());
        bool same = ok;
        for (int k = 0; k < 100 && same; ++k) same = a[(size_t)k] == L[1000 + (size_t)k] && b[(size_t)k] == R[1000 + (size_t)k] && c[(size_t)k] == L[3583 + (size_t)k];
        for (int k = 100; k < 300 && same; ++k) same = c[(size_t)k] == 0.f && d[(size_t)k] == 0.f;
        std::vector<float> e(50), f(50);
        pf.read(-20, 50, e.data(), f.data());
        for (int k = 0; k < 20 && same; ++k) same = e[(size_t)k] == 0.f;
        same = same && e[20] == L[0];
        check(same, "read from anywhere: the same samples; silence before its start and after its end");
    }
    {
        // the same samples written by Reno with the same mark: VEGAS's file byte for byte
        const std::string out = pathJoin(scratch, "nested.veg.sfap0");
        AudioProxyWriter w;
        bool ok = w.open(out, 48000, &err);
        for (int at = 0; ok && at < 3683; at += 1000) ok = w.write(L.data() + at, R.data() + at, std::min(1000, 3683 - at));
        ok = ok && w.finish(vegTime, &err);
        std::string a, b;
        check(ok && readFile(out, a) && readFile(vp, b) && a == b, "an audio proxy Reno writes is VEGAS's, byte for byte",
              strfmt("%zu / %zu bytes %s", a.size(), b.size(), err.c_str()));
        check(!fileExists(out + ".part"), "nothing left over from writing it");
        // a longer one: the end time's text is longer, the summary list padded to 8 bytes as VEGAS pads it
        AudioProxyWriter w2;
        std::vector<float> z(48000, 0.f);
        ok = w2.open(out, 48000) && w2.write(z.data(), z.data(), 48000) && w2.write(z.data(), z.data(), 30001) && w2.finish(vegTime + 5);
        AudioProxyInfo i2;
        ok = ok && readAudioProxyInfo(out, i2) && i2.frames == 78001 && audioProxyCurrent(i2, vegTime + 5);
        std::string c;
        readFile(out, c);
        const size_t tail = 104 + 78001 * 8;
        check(ok && c.size() % 8 == 0 && c.size() == tail + 72 + 48 && c.find(std::string("1\0" "6\0" "2\0" "5\0" "0\0" "2\0" "0\0" "8\0", 16), tail) != std::string::npos,
              "a longer proxy: 78001 samples, end time 16250208 (100 ns), the list padded to 72 bytes");
        // stopped half way: nothing left behind, the old one untouched
        AudioProxyWriter w3;
        w3.open(out, 48000);
        w3.write(z.data(), z.data(), 1000);
        w3.abort();
        AudioProxyInfo i3;
        check(!fileExists(out + ".part") && readAudioProxyInfo(out, i3) && i3.frames == 78001, "a build stopped half way leaves nothing behind");
    }
    {
        // bad proxies: VEGAS's 0-byte file left by a cancelled build, a cut-short header, not Wave64
        const std::string bad = pathJoin(scratch, "bad.sfap0");
        AudioProxyInfo i;
        writeFile(bad, "");
        const bool empty = !readAudioProxyInfo(bad, i);
        std::string v;
        readFile(vp, v);
        writeFile(bad, v.substr(0, 60));
        const bool cut = !readAudioProxyInfo(bad, i);
        writeFile(bad, std::string("RIFF") + v.substr(4));
        const bool riff = !readAudioProxyInfo(bad, i);
        // samples cut short (a build that crashed): only the samples there are counted, and it has no mark
        writeFile(bad, v.substr(0, 104 + 800));
        const bool partial = readAudioProxyInfo(bad, i) && i.frames == 100 && !audioProxyCurrent(i, vegTime);
        AudioProxyFile f;
        check(empty && cut && riff && partial && !f.open(pathJoin(scratch, "missing.sfap0")), "empty, cut-short, non-Wave64 and missing proxies are refused (or stale)");
    }

    // ---------------------------------------------------------------- VEGAS's own files (optional)
    if (argc > 2) {
        const std::string dir = argv[2];
        for (const std::string& f : listDir(dir)) {
            if (!endsWithNoCase(f, ".sfk")) continue;
            PeakFile v;
            if (!readPeakFile(pathJoin(dir, f), v)) continue;
            // the media: "<x>.sfk" of "<x>.wav", "<name>.sfk", or "<name>-a-b-c.sfk"
            std::string base = f.substr(0, f.size() - 4), media;
            int a = 0;
            long long len = 0;
            int c = 0, used = 0;
            const size_t dash = base.rfind('-', base.rfind('-', base.rfind('-') - 1) - 1);
            if (dash != std::string::npos && std::sscanf(base.c_str() + dash, "-%d-%lld-%d%n", &a, &len, &c, &used) == 3 && used == (int)(base.size() - dash))
                base = base.substr(0, dash);
            if (fileExists(pathJoin(dir, base + ".wav"))) media = pathJoin(dir, base + ".wav");
            else if (fileExists(pathJoin(dir, base)) && !endsWithNoCase(base, ".veg")) media = pathJoin(dir, base);
            if (media.empty()) continue;
            PeakFile r;
            if (!peaksOf(media, v.frames, v.channels, r, &err)) continue;
            int64_t diff = 0, total = 0;
            int most = 0;
            comparePeaks(r, v, diff, most, total);
            int64_t near = 0;
            for (int64_t k = 0; k < total; ++k)
                if (std::abs((int)r.minmax[(size_t)k] - (int)v.minmax[(size_t)k]) <= 64) ++near;
            std::printf("  %s: %lld samples, %lld blocks: %lld of %lld numbers differ, %.1f%% within 64 (of 32768), most %d; stamp %s\n", f.c_str(),
                        (long long)v.frames, (long long)v.blocks(), (long long)diff, (long long)total, total ? 100.0 * near / total : 0.0, most,
                        peakFileCurrent(v, media) ? "current" : "stale (the copy here has another time)");
            if (!endsWithNoCase(media, ".wav")) {
                // VEGAS leaves out the AAC encoder's delay (2112 samples) that FFmpeg gives: the same peaks with it skipped
                PeakFile r2;
                if (peaksOf(media, v.frames, v.channels, r2, &err, 2112)) {
                    comparePeaks(r2, v, diff, most, total);
                    std::printf("    with Reno's first 2112 samples skipped: %lld of %lld differ (%.1f%% equal)\n", (long long)diff, (long long)total,
                                total ? 100.0 * (total - diff) / total : 0.0);
                }
            }
            if (endsWithNoCase(media, ".wav")) {
                // VEGAS leaves the very last block of some files empty: that one is not counted
                int64_t lastDiff = 0;
                for (int ch = 0; ch < v.channels; ++ch)
                    for (int k = 0; k < 2; ++k) {
                        const size_t at = (size_t)((v.blocks() - 1) * v.channels + ch) * 2 + k;
                        if (at < r.minmax.size() && r.minmax[at] != v.minmax[at]) ++lastDiff;
                    }
                // (VEGAS gives a block's edge sample to one block or the next with no rule found: a few numbers of some
                // files differ that way)
                check((diff - lastDiff) * 100 <= total * 2, "Reno's peaks of " + pathFileName(media) + " are VEGAS's (at least 98% of the numbers)",
                      strfmt("%lld of %lld differ", (long long)(diff - lastDiff), (long long)total));
            }
        }
    }

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
