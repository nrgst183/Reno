// VEGAS's ExpressFX Graphic EQ (sfxpfx3_x64.dll, CXpGeq, DirectX class 00000009-0f56-11d2-9887-00a0c969725b): its settings,
// presets and sound, from the DLL's own code (the design, the block convolver and the framework that feeds it).
//
// The sound is a 2047 tap FIR filter by overlap-add, the same machinery as the Graphic EQ of sfppack2 (vegasgraphiceq.h)
// with a fixed size: the sound comes in blocks of 2048 frames, each is zero padded to 4096, transformed, multiplied by
// the filter's spectrum, transformed back and added to the tail of the last block; the output gain is applied at the end.
// The filter comes from ten band levels: each band lays a trapezoid on an envelope of up to 64 points (x in 1/1000 octaves
// above 20 Hz, y a slider position), which is turned into a magnitude response, an impulse response (windowed) and its
// spectrum (see veggeq). The plug-in notices a change of the settings only every 20480 frames of the sound it is given
// (a counter of the frames offered to it), then designs the filter again at once.
//
// The framework gives the block out as soon as it is complete, so the effect holds the sound back by one block
// (latency()). Each call is replayed as the DLL sees it (the sound offered in, room for as much out), see call().
#pragma once
#include "vegaspack1.h"
#include "vegasfft.h"
#include "vegasgraphiceq.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace reno {
namespace vegpack1 {
namespace xpgeq {

inline int toInt(double x)      // cvttsd2si
{
    if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
    return (int)x;
}

// The envelope of the ten band levels (tenths of a dB): the plug-in's 0x18000fd30. p has room for 64 points.
inline int bandsToPoints(const int32_t* bands, int32_t (*p)[2])
{
    using namespace veggeq;
    int ebx = 2;
    p[0][0] = 0; p[0][1] = 0; p[1][0] = 10000; p[1][1] = 0;
    auto put = [&](int key, int y) {
        int at = 0;
        while (at < ebx && p[at][0] < key) ++at;
        if (p[at][0] != key) {
            for (int k = ebx; k > at; --k) { p[k][0] = p[k - 1][0]; p[k][1] = p[k - 1][1]; }
            ++ebx;
            p[at][0] = key;
        }
        p[at][1] = y;
    };
    for (int band = 0; band < 10; ++band) {
        const int X = band * 1000;
        int rdx = 0, r10 = 0, r11 = 0, esi = ebx - 1;
        for (int i = 0; i < ebx; ++i) {
            if (p[i][0] < X) rdx = i;
            if (p[i][0] == X - 250) r11 = i;
        }
        for (int i = ebx - 1; i >= 0; --i) {
            if (p[i][0] > X + 1000) esi = i;
            if (p[i][0] == X + 1250) r10 = i;
        }
        const bool needLeft = !(r11 > 0), needRight = !(r10 > 0);
        double slopeL = 0.0;
        if (band != 0) slopeL = (double)(p[rdx + 1][1] - p[rdx][1]) / (double)(p[rdx + 1][0] - p[rdx][0]);
        const int yAtX = toInt((double)(X - p[rdx][0]) * slopeL) + p[rdx][1];
        const int r8y = p[esi - 1][1], edx = p[esi - 1][0];
        const double slopeR = (double)(p[esi][1] - r8y) / (double)(p[esi][0] - edx);
        const int yRight = toInt(slopeR * (double)(X + 1000 - edx)) + r8y;
        {   // the points inside the band's cell go (never the first or the last)
            int r9 = ebx - 1, r8 = ebx - 1;
            for (; r9 > 0; --r9) {
                const int x = p[r9][0];
                if (x < X || x > X + 1000 || r9 == r8) continue;
                if (r9 < r8)
                    for (int k = r9; k < r8; ++k) { p[k][0] = p[k + 1][0]; p[k][1] = p[k + 1][1]; }
                --ebx;
                --r8;
            }
        }
        const int yGain = posOfTenths(bands[band]);
        if (band != 0) put(X + 250, yGain);
        if (band != 9) put(X + 750, yGain);
        if (needLeft && band != 0) put(X, yAtX);
        if (needRight && band != 9) put(X + 1000, yRight);
        if (band == 0) p[0][1] = p[1][1];
        else if (band == 9) p[ebx - 1][1] = p[ebx - 2][1];
        cleanPoints(p, &ebx);
    }
    return ebx;
}

// The settings structure (12 ints, what the plug-in saves): 0 a mode 0..2 that the sound does not use, 1..10 the band
// levels in tenths of a dB (-480 = -Inf. .. 240), 11 the output gain in tenths of a dB (-600 .. 200).
inline void sanitize(int32_t* s)
{
    s[0] = std::max(0, std::min(2, (int)s[0]));
    for (int i = 1; i <= 10; ++i) s[i] = std::max(-480, std::min(240, (int)s[i]));
    s[11] = std::max(-600, std::min(200, (int)s[11]));
}

class XpGeqEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        std::memcpy(live_, p, sizeof live_);
        sanitize(live_);
        const uint32_t r = rate ? rate : 48000u;
        if (!keep || !built_ || r != rate_) {
            rate_ = r;
            fresh();
        } else if (std::memcmp(live_, copy_, sizeof live_) != 0) {
            dirty_ = true;
        }
    }
    int latency(const int32_t*, uint32_t) const override { return kHop; }
    void process(float* left, float* right, int frames) override
    {
        if (!built_ || frames <= 0) return;
        std::vector<float> inL(left, left + frames), inR(right, right + frames);
        std::vector<float> oL((size_t)frames), oR((size_t)frames);
        int offered = frames;
        int off = 0;
        while (offered > 0) {
            int nIn = offered, nOut = frames;
            call(inL.data() + off, inR.data() + off, &nIn, oL.data(), oR.data(), &nOut);
            for (int i = 0; i < nOut; ++i) { qL_.push_back(oL[(size_t)i]); qR_.push_back(oR[(size_t)i]); }
            off += nIn;
            offered -= nIn;
            if (nIn == 0 && nOut == 0) break;
        }
        for (int k = 0; k < frames; ++k) {
            if (qHead_ < qL_.size()) {
                left[k] = qL_[qHead_];
                right[k] = qR_[qHead_];
                ++qHead_;
            } else {
                left[k] = right[k] = 0.f;
            }
        }
        if (qHead_ > 65536) {
            qL_.erase(qL_.begin(), qL_.begin() + (long)qHead_);
            qR_.erase(qR_.begin(), qR_.begin() + (long)qHead_);
            qHead_ = 0;
        }
    }

private:
    static const int kHop = 2048, kFft = 4096, kTaps = 2047;
    bool built_ = false, dirty_ = false;
    uint32_t rate_ = 48000;
    int32_t live_[12] = {}, copy_[12] = {};
    double gain_ = 1.0;
    std::vector<double> filter_;
    std::vector<float> work_[2], tail_[2];
    std::vector<float> qL_, qR_;
    size_t qHead_ = 0;
    uint32_t polled_ = 0, pollEvery_ = 20480;
    int inFill_ = 0, outPos_ = 0, state_ = 0;
    int offIn_ = 0, offOut_ = 0;

    void fresh()
    {
        std::memcpy(copy_, live_, sizeof copy_);
        dirty_ = false;
        design();
        for (int c = 0; c < 2; ++c) {
            work_[c].assign((size_t)kFft, 0.f);
            tail_[c].assign((size_t)kTaps, 0.f);
        }
        polled_ = 0;
        inFill_ = outPos_ = state_ = 0;
        qL_.assign((size_t)kHop, 0.f);
        qR_.assign((size_t)kHop, 0.f);
        qHead_ = 0;
        built_ = true;
    }

    // the plug-in's 0x1800101a0
    void design()
    {
        gain_ = std::pow(10.0, (double)copy_[11] / 200.0);
        int32_t pt[80][2] = {};
        const int cnt = bandsToPoints(copy_ + 1, pt);
        std::vector<double> m(2 * (size_t)kFft + 16, 0.0), x, tw;
        veggeq::response(m.data(), kFft, pt, cnt, (int)rate_);
        veggeq::impulse(m.data(), kFft, kTaps, x, tw);
        filter_.assign((size_t)kFft + 16, 0.0);
        for (int j = 0; j < kTaps; ++j) filter_[(size_t)j] = (double)kFft * m[(size_t)j];
        vegfft::rfftDfwd(filter_.data(), kFft, 1.0);
    }

    void multiply(float* b) const
    {
        const double* f = filter_.data();
        b[0] = (float)((double)b[0] * f[0]);
        b[1] = (float)((double)b[1] * f[1]);
        for (int k = 1; k < kFft / 2; ++k) {
            const double re = (double)b[2 * k], im = (double)b[2 * k + 1];
            const double fr = f[2 * k], fi = f[2 * k + 1];
            const double nre = re * fr - im * fi;
            const double nim = im * fr + re * fi;
            b[2 * k] = (float)nre;
            b[2 * k + 1] = (float)nim;
        }
    }
    void convolve()
    {
        for (int c = 0; c < 2; ++c) {
            float* b = work_[c].data();
            float* o = tail_[c].data();
            std::fill(b + kHop, b + kFft, 0.f);
            vegfft::rfftF(b, kFft / 2, true, 1.0f);
            multiply(b);
            vegfft::rfftF(b, kFft / 2, false, 1.0f);
            for (int i = 0; i < kTaps; ++i) {
                b[i] = b[i] + o[i];
                o[i] = b[kHop + i];
            }
        }
    }

    // One call of the plug-in's transform (0x1800112d0): *nIn frames offered, room for *nOut; on return how many were
    // taken and given. The settings are looked at when the frames offered since the last time reach 20480.
    void call(const float* inL, const float* inR, int* nIn, float* oL, float* oR, int* nOut)
    {
        const int offered = *nIn;
        bool changed = false;
        if (polled_ >= pollEvery_) {
            changed = dirty_;
            if (dirty_) { std::memcpy(copy_, live_, sizeof copy_); dirty_ = false; }
            polled_ = 0;
        }
        polled_ += (uint32_t)offered;
        if (changed) design();
        const int cap = *nOut;
        offIn_ = offOut_ = 0;
        if (offered == 0 && state_ == 0 && inFill_ == 0) { *nOut = 0; return; }
        for (;;) {
            if (state_ == 1) {
                while (offOut_ < cap && outPos_ < kHop) {
                    oL[offOut_] = (float)((double)work_[0][(size_t)outPos_] * gain_);
                    oR[offOut_] = (float)((double)work_[1][(size_t)outPos_] * gain_);
                    ++outPos_;
                    ++offOut_;
                }
                if (outPos_ < kHop) break;
                outPos_ = inFill_ = state_ = 0;
            }
            while (offIn_ < offered && inFill_ < kHop) {
                work_[0][(size_t)inFill_] = inL[offIn_];
                work_[1][(size_t)inFill_] = inR[offIn_];
                ++inFill_;
                ++offIn_;
            }
            if (inFill_ != kHop) break;
            convolve();
            state_ = 1;
        }
        *nIn = offIn_;
        *nOut = offOut_;
    }
};

}  // namespace xpgeq

inline const Info& xpGeqInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpgeq";
        i.name = "ExpressFX Graphic EQ";
        i.clsid = "00000009-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x96, 0xc7, 0xd7, 0xe2, 0x04, 0xd1, 0x28, 0x4b, 0x9f, 0xb3, 0x29, 0x65, 0xd5, 0x03, 0xd1, 0x10};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 12;
        i.bypassSlot = -1;
        static const char* const names[10] = {"Band 1 (28 Hz)",  "Band 2 (56 Hz)",  "Band 3 (113 Hz)", "Band 4 (225 Hz)", "Band 5 (450 Hz)",
                                              "Band 6 (900 Hz)",  "Band 7 (1.8 kHz)", "Band 8 (3.6 kHz)", "Band 9 (7.2 kHz)", "Band 10 (15 kHz)"};
        for (int b = 0; b < 10; ++b) i.params.push_back({names[b], -48, 24, 0, "dB", b + 1, 10, Num, "", 1, true});
        i.params.push_back({"Output gain", -60, 20, 0, "dB", 11, 10, Num, "", 1, false});
        auto P = [](const char* n, int m, std::initializer_list<int> bands, int out) {
            Preset p;
            p.name = n;
            p.v.push_back(m);
            for (int b : bands) p.v.push_back(b);
            p.v.push_back(out);
            return p;
        };
        i.presets = {
            P("(Default)", 1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0),
            P("Default all parameters", 1, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, 0),
            P("Boost bass around 125 Hz by 6 dB", 2, {0, 15, 60, 15, 0, 0, 0, 0, 0, 0}, 0),
            P("Boost high frequencies above 4 kHz by 6 dB", 2, {0, 0, 0, 0, 0, 0, 30, 60, 60, 60}, 0),
            P("Boost mid-range around 1 kHz by 6 dB", 1, {0, 0, 0, 0, 40, 60, 40, 0, 0, 0}, 0),
            P("Cut high frequencies above 4 kHz by 12 dB", 1, {0, 0, 0, 0, 0, 0, 0, -60, -120, -120}, 0),
            P("Cut low frequencies below 125 dB by -60 dB", 2, {-600, -600, -100, 0, 0, 0, 0, 0, 0, 0}, 0),
            P("Cut mid-range around 1 kHz by 6 dB", 1, {0, 0, 0, 0, -40, -60, -40, 0, 0, 0}, 0),
        };
        i.make = [] { return std::unique_ptr<Engine>(new xpgeq::XpGeqEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
