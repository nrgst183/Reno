// Checks of the loudness meter (src/engine/loudness.cpp) against the EBU Tech 3341 / 3342 test signals.
//
//   loudness_test
// Build: g++ -std=c++17 -O1 -g tests/engine/loudness_test.cpp src/engine/loudness.cpp -o loudness_test
#include "../../src/engine/loudness.h"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using reno::LoudnessMeter;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) ++gPass;
    else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
}
static std::string num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.3f", v); return b; }

static const double kPi = 3.14159265358979323846;

// A stereo sine of 'freq' Hz at 'dbfs' (peak), 'seconds' long, fed to the meter in blocks of 'block'.
static void sine(LoudnessMeter& m, int rate, double freq, double dbfs, double seconds, double phase0 = 0, int block = 1000)
{
    const double a = std::pow(10.0, dbfs / 20.0);
    const int64_t n = (int64_t)std::llround(seconds * rate);
    std::vector<float> l((size_t)block), r((size_t)block);
    static int64_t at = 0;
    for (int64_t k = 0; k < n; k += block) {
        const int c = (int)std::min<int64_t>(block, n - k);
        for (int i = 0; i < c; ++i) {
            const float v = (float)(a * std::sin(2.0 * kPi * freq * (double)(at + i) / rate + phase0));
            l[(size_t)i] = v;
            r[(size_t)i] = v;
        }
        at += c;
        m.process(l.data(), r.data(), c);
    }
}

int main()
{
    // EBU Tech 3341 case 1: a 1 kHz stereo sine at -23 dBFS measures -23.0 LUFS (momentary, short-term, integrated)
    for (int rate : {48000, 44100, 96000}) {
        LoudnessMeter m(rate);
        sine(m, rate, 1000, -23.0, 20.0);
        check(std::fabs(m.integrated() + 23.0) < 0.1, std::to_string(rate) + " Hz: 1 kHz at -23 dBFS integrates to -23.0 LUFS", num(m.integrated()));
        check(std::fabs(m.momentary() + 23.0) < 0.1 && std::fabs(m.shortTerm() + 23.0) < 0.1, std::to_string(rate) + " Hz: and momentary and short-term are -23.0",
              num(m.momentary()) + " " + num(m.shortTerm()));
    }
    // case 2: -33 dBFS gives -33.0
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -33.0, 20.0);
        check(std::fabs(m.integrated() + 33.0) < 0.1, "1 kHz at -33 dBFS integrates to -33.0 LUFS", num(m.integrated()));
    }
    // case 3: 10 s at -36, 60 s at -23, 10 s at -36 - the relative gate leaves the quiet parts out: -23.0
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -36.0, 10.0);
        sine(m, 48000, 1000, -23.0, 60.0);
        sine(m, 48000, 1000, -36.0, 10.0);
        check(std::fabs(m.integrated() + 23.0) < 0.1, "-36 / -23 / -36 dBFS: the relative gate gives -23.0 LUFS", num(m.integrated()));
    }
    // case 5: 20 s at -26, 20.1 s at -20, 20 s at -26 integrates to -23.0
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -26.0, 20.0);
        sine(m, 48000, 1000, -20.0, 20.1);
        sine(m, 48000, 1000, -26.0, 20.0);
        check(std::fabs(m.integrated() + 23.0) < 0.1, "-26 / -20 / -26 dBFS integrates to -23.0 LUFS", num(m.integrated()));
    }
    // silence: below the absolute gate
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -200.0, 5.0);
        check(m.integrated() <= LoudnessMeter::kSilence + 1e-9 && m.momentary() <= LoudnessMeter::kSilence + 1e-9, "silence reads -70 (nothing)", num(m.integrated()));
    }
    // EBU Tech 3342 case 1: 20 s at -20 dBFS then 20 s at -30 dBFS: LRA 10 LU (+-1)
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -20.0, 20.0);
        sine(m, 48000, 1000, -30.0, 20.0);
        check(std::fabs(m.range() - 10.0) < 1.0, "Tech 3342 case 1: -20 then -30 dBFS: loudness range 10 LU", num(m.range()));
    }
    // case 2: -20 then -15: LRA 5
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -20.0, 20.0);
        sine(m, 48000, 1000, -15.0, 20.0);
        check(std::fabs(m.range() - 5.0) < 1.0, "Tech 3342 case 2: -20 then -15 dBFS: loudness range 5 LU", num(m.range()));
    }
    // a steady tone has no range
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 1000, -20.0, 20.0);
        check(m.range() < 0.2, "a steady tone: no loudness range", num(m.range()));
    }
    // true peak: a full-scale sine at a quarter of the rate, sampled between its peaks (45 degrees off), has samples
    // at 0.707 but a true peak of 0 dBTP (Tech 3341 case 15-ish: within the oversampling's accuracy)
    {
        LoudnessMeter m(48000);
        sine(m, 48000, 12000, -6.0, 2.0, kPi / 4);
        check(std::fabs(m.truePeak() + 6.0) < 0.6, "a -6 dBFS sine at 12 kHz sampled off its peaks: true peak about -6 dBTP (the samples say -9)", num(m.truePeak()));
        LoudnessMeter s(48000);
        sine(s, 48000, 1000, -1.0, 1.0);
        check(std::fabs(s.truePeak() + 1.0) < 0.1, "a -1 dBFS 1 kHz sine: true peak -1 dBTP", num(s.truePeak()));
    }
    // the K-weighting: 100 Hz counts less than 1 kHz; 4 kHz a little more (the shelf adds ~+4 dB at the top)
    {
        LoudnessMeter lo(48000), hi(48000);
        sine(lo, 48000, 100, -23.0, 10.0);
        sine(hi, 48000, 4000, -23.0, 10.0);
        check(lo.integrated() < -23.0 && hi.integrated() > -23.0 + 2.0, "K-weighting: 100 Hz reads lower and 4 kHz higher than 1 kHz",
              num(lo.integrated()) + " " + num(hi.integrated()));
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
