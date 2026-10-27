// VEGAS Time Stretch (sfppack1's CSfTime), float32 stereo path: a waveform-similarity overlap-add (SOLA) that reads the
// sound in frames of a mode-dependent length, moves along the input by (half a frame / the stretch factor) and puts
// the best-matching piece after the one before, so the sound comes out longer (stretch above 100 %) or shorter. Translated
// function by function from the plug-in's engine (the names of the members say which field of the engine object they
// are, e.g. f0d0_ = [this + 0xd0]), same float32 arithmetic in the same order, so the output is the same samples.
//
// The engine is a stream: how many frames come out for the frames that go in follows the stretch factor, and the sound
// comes out a frame late (it needs a whole frame before it starts). The plug-in's own calls are kept: run() is the
// filter's function that takes a block of input and a buffer for output and tells how much of each it used.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "vegaspitch.h"

namespace reno {
namespace vegpack1 {

// ExpressFX Time Stretch (sfxpfx3's CXpTime) runs the same engine with five modes of its own.
inline const PitchMode& expressTimeMode(int i)
{
    static const PitchMode t[5] = {{300, 15, 2, 15, 2500}, {90, 15, 2, 30, 2500}, {150, 25, 2, 30, 2500}, {30, 5, 2, 3, 1000}, {70, 9, 2, 8, 2000}};
    return t[std::max(0, std::min(4, i))];
}

class TimeCore {
public:
    explicit TimeCore(bool express = false) : express_(express) { setRate(48000); }

    // The filter's "media type is set" (0x1800081f0): the engine starts again with the settings it has.
    void setRate(uint32_t r)
    {
        rate_ = r;
        prepareStream();
        f5c_ = 0;
    }
    // The property setters (0x1800041a0 final length, 0x180004350 mode): a changed value marks the engine as changed;
    // a changed mode also asks for everything to be set up again (buffers and window).
    void setSettings(double d30, int32_t mode)
    {
        if (d30 < 5000.0) d30 = 5000.0;
        else if (d30 > 50000.0) d30 = 50000.0;
        if (d30_ != d30) {
            d30_ = d30;
            f50_ = 1;
        }
        if (mode_ != mode) {
            mode_ = mode;
            f54_ |= 1;
            f50_ = 1;
        }
    }
    double ratio() const { return fa0_; }
    int frameSamples() const { return fd0_; }
    int hopOut() const { return fd4_; }

    // 0x180007ea0. mode: 0 first block after a discontinuity, 1 a following block, 2 end of the stream (call until it says
    // it is done). inCnt: frames available in / frames used; outCnt: room for output / frames made. Result as the plug-in's
    // (1 where it has nothing more to give at the end of the stream).
    int run(int mode, const float* in, int* inCnt, float* out, int* outCnt)
    {
        const bool flush = mode == 2;
        f98_ = flush ? 1 : 0;
        const bool changed = f50_ != 0;                 // 0x180003810: takes the settings, clears the flag
        f50_ = 0;
        const double s30 = d30_;
        const int32_t sm = mode_;
        if (mode == 0 && f5c_ != 0) {
            prepareStream();
        } else if ((changed || f90_ == 0) && !flush) {
            init(s30, sm);
        }
        f5c_ = 1;
        int availIn = inCnt ? *inCnt : 0;
        int cap = outCnt ? *outCnt : 0;
        const int r = core(in, &availIn, out, &cap);
        if (inCnt) *inCnt = availIn;
        if (outCnt) *outCnt = cap;
        return r;
    }

private:
    bool express_ = false;
    uint32_t rate_ = 48000;
    double d30_ = 10000.0;                  // [this + 0x30]: final length in hundredths of a percent
    int32_t mode_ = 0;                      // [this + 0x38]
    int f50_ = 1, f54_ = 1, f5c_ = 0, f90_ = 0, f98_ = 0;
    int rec_[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};      // 0xa8..0xcc: the mode's record
    int fd0_ = 0, fd4_ = 0, fd8_ = 1, fdc_ = 0, f110_ = 1, f114_ = 0, f13c_ = 0;
    double fa0_ = 1.0;
    float f118_ = 0.f;
    int f11c_ = 0, f124_ = 0, f128_ = 0, f12c_ = 0, f130_ = 0, f134_ = 0, f138_ = 0, f140_ = 0;
    std::vector<float> e0_, e8_, f0_;

    static int cvtt(double x)
    {
        if (!(x > -2147483649.0 && x < 2147483648.0)) return INT32_MIN;      // what cvttsd2si gives out of range
        return (int)x;
    }

    // 0x180007a80: the engine starts again (after the format is set, after a discontinuity)
    void prepareStream()
    {
        f54_ = 1;
        const bool dummy = f50_ != 0;       // 0x180003810 again: takes the settings
        (void)dummy;
        f50_ = 0;
        init(d30_, mode_);
        f11c_ = 1;
        f140_ = 0;
        f5c_ = 0;
    }

    // 0x1800073a0
    void init(double s30, int32_t mode)
    {
        if (f54_ == 0) {
            const double ratio = s30 / 10000.0;
            fa0_ = ratio;
            int r8 = cvtt((double)fd4_ / ratio + 0.5);
            if (r8 < 1) r8 = 1;
            else if (r8 > fd0_) r8 = fd0_;
            fd8_ = r8;
            f13c_ = fd0_ - r8;
            return;
        }
        const PitchMode& pm = express_ ? expressTimeMode(mode) : pitchMode(mode);
        // the plug-in keeps the whole 40-byte record; the engine reads these fields of it
        const int frameMs = pm.frameMs, searchMs = pm.searchMs, stepMs = pm.step, fadeMs = pm.fadeMs, thr = pm.thresholdPct;
        rec_[0] = frameMs;
        rec_[2] = searchMs;
        rec_[3] = stepMs;
        rec_[4] = fadeMs;
        rec_[6] = thr;
        fd0_ = (int)((uint32_t)((int32_t)rate_ * (int32_t)frameMs) / 1000u);
        e0_.assign((size_t)2 * (size_t)fd0_, 0.f);
        e8_.assign((size_t)2 * (size_t)fd0_, 0.f);
        f0_.assign((size_t)fd0_, 1.0f);
        int dc = (int)((uint32_t)((int32_t)fadeMs * (int32_t)rate_) / 1000u);
        if (dc < 0) dc = 0;
        else {
            const int lim = fd0_ / 2 - 1;
            if (dc > lim) dc = lim;
        }
        fdc_ = dc;
        if (dc > 1) {
            const int half = fd0_ / 2;
            for (int i = 0; i < dc; ++i) {
                float f = (float)i;
                f = f / (float)dc;
                f = f * f0_[(size_t)i];
                f0_[(size_t)i] = f;
                f0_[(size_t)(half + dc - i)] = f0_[(size_t)i];
            }
            for (int k = half + dc + 1; k < fd0_; ++k) f0_[(size_t)k] = 0.f;
        }
        const int half = fd0_ / 2;
        f138_ = half;
        fd4_ = half;
        int r11 = half;
        const double ratio = s30 / 10000.0;
        fa0_ = ratio;
        int ecx = cvtt((double)r11 / ratio + 0.5);
        if (ecx < 1) ecx = 1;
        else if (ecx > fd0_) ecx = fd0_;
        fd8_ = ecx;
        r11 -= fdc_;
        f114_ = r11;
        const int search = (int)((uint32_t)((int32_t)searchMs * (int32_t)rate_) / 1000u);
        f110_ = stepMs;
        if (r11 > search) f114_ = search;
        if (rate_ < 0x7d00u) {
            const uint32_t prod = (uint32_t)((int32_t)stepMs * (int32_t)rate_);
            const uint32_t q = (uint32_t)(((uint64_t)prod * 0xbe37c63bull) >> 32) >> 15;
            f110_ = (int)q;
            if ((int)q < 1) f110_ = 1;
        }
        f118_ = (float)thr / 100.0f;
        f124_ = 0;
        f12c_ = 0;
        f134_ = 0;
        f90_ = 1;
        f13c_ = fd0_ - fd8_;
        f54_ = 0;
    }

    // 0x18000af10 (the float32 loop); the stereo case
    int core(const float* in, int* pIn, float* out, int* pOut)
    {
        const int availIn = *pIn;
        f128_ = 0;
        f130_ = 0;
        if (availIn == 0 && f124_ == 0 && f140_ != 0) {
            *pOut = 0;
            return 1;
        }
        const int ch = 2;
        const int capOut = *pOut;
        for (;;) {
            if (f124_ == 1) {
                // 0x18000ae70: the part of the output frame that fits
                if (f138_ != 0) {
                    int room = capOut - f130_;
                    const int left = f138_ - f134_;
                    if ((uint32_t)room >= (uint32_t)left) room = left;
                    std::memcpy(out + (size_t)f130_ * ch, e8_.data() + (size_t)f134_ * ch, (size_t)room * ch * 4);
                    f130_ += room;
                    f134_ += room;
                }
                if (f134_ != f138_) break;
                const bool flushing = f98_ != 0;
                f134_ = 0;
                if (!flushing) f12c_ = f13c_;
                std::memmove(e0_.data(), e0_.data() + (size_t)fd8_ * ch, (size_t)(fd0_ - fd8_) * ch * 4);
                f124_ = 0;
                std::memmove(e8_.data(), e8_.data() + (size_t)fd4_ * ch, (size_t)fdc_ * ch * 4);
                std::memset(e8_.data() + (size_t)fdc_ * ch, 0, (size_t)(fd0_ - fdc_) * ch * 4);
            }
            // 0x18000ade0: take input into the frame
            {
                int n = availIn - f128_;
                const int room = fd0_ - f12c_;
                if ((uint32_t)n >= (uint32_t)room) n = room;
                if (n > 0) std::memcpy(e0_.data() + (size_t)f12c_ * ch, in + (size_t)f128_ * ch, (size_t)n * ch * 4);
                f128_ += n;
                f12c_ += n;
            }
            if (f98_ != 0) {
                int ecx = f12c_;
                if (ecx < fd4_) {
                    if (f140_ != 0) break;
                    f140_ = 1;
                    if (ecx <= 0) break;
                    std::memset(e0_.data() + (size_t)ecx * ch, 0, (size_t)(fd0_ - ecx) * ch * 4);
                    if (express_) {
                        f138_ = f12c_;              // ExpressFX's older engine hands out all of what is left at the end
                    } else {
                        int edx = cvtt((double)f12c_ * fa0_);
                        if (edx < 0) edx = 0;
                        else if (edx > fd0_) edx = fd0_;
                        f138_ = edx;
                    }
                }
                f12c_ -= fd8_;
            }
            if (f12c_ != fd0_ && f98_ == 0) break;
            const int off = search(e0_.data(), e8_.data());
            overlapAdd(e0_.data(), e8_.data(), off);
            f124_ = 1;
        }
        *pIn = f128_;
        *pOut = f130_;
        return 0;
    }

    // 0x18000a0b0: where in the new frame the sound fits best on the end of the output so far
    int search(const float* x, const float* y) const
    {
        const int step = f110_;
        const unsigned srch = (unsigned)f114_;
        const int esi = 2 * step;
        float eL = 0.f, eR = 0.f;                       // xmm4, xmm3
        if (srch != 0) {
            const float* p = y;
            unsigned e = 0;
            do {
                float l = p[0];
                float r = p[1];
                p += esi;
                e += (unsigned)step;
                r *= r;
                l *= l;
                eR += r;
                eL += l;
            } while (e < srch);
        }
        const float tL = eL * f118_;
        const float tR = eR * f118_;
        int best = 0;
        float bestV = 0.f;                              // xmm5
        if (srch != 0) {
            unsigned cand = 0;
            size_t xo = 0;
            do {
                const float* xp = x + xo;
                const float* yp = y;
                float sL = 0.f, sR = 0.f;
                unsigned r8 = 0;
                do {
                    float a = yp[0];
                    a = a + xp[0];
                    r8 += (unsigned)step;
                    a *= a;
                    sL += a;
                    float b = yp[1];
                    b = b + xp[1];
                    yp += esi;
                    xp += esi;
                    b *= b;
                    sR += b;
                } while (r8 < srch);
                const float sum = sR + sL;
                if (sL > tL && sR > tR) return (int)cand;
                if (sum > bestV) {
                    bestV = sum;
                    best = (int)cand;
                }
                cand += (unsigned)step;
                xo += (size_t)esi;
            } while (cand < srch);
        }
        return best;
    }

    // 0x18000a410: puts the frame (from 'off') on the output: the first dc frames are faded in and added to the tail of
    // the one before, the middle is copied, the last dc frames are faded out
    void overlapAdd(const float* x, float* y, int off)
    {
        const float* xs = x + (size_t)off * 2;
        float* yd = y;
        const float* w = f0_.data();
        const int dc = fdc_;
        if (f11c_ != 0) {
            for (int k = 0; k < dc; ++k) {
                yd[2 * k] = xs[2 * k] + yd[2 * k];
                yd[2 * k + 1] = xs[2 * k + 1] + yd[2 * k + 1];
            }
            f11c_ = 0;
        } else {
            for (int k = 0; k < dc; ++k) {
                float a = xs[2 * k];
                a = a * w[k];
                yd[2 * k] = a + yd[2 * k];
                float b = xs[2 * k + 1];
                b = b * w[k];
                yd[2 * k + 1] = b + yd[2 * k + 1];
            }
        }
        xs += (size_t)dc * 2;
        yd += (size_t)dc * 2;
        const int mid = fd0_ / 2 - dc;
        std::memmove(yd, xs, (size_t)mid * 8);
        yd += (size_t)mid * 2;
        xs += (size_t)mid * 2;
        const float* w2 = w + (dc + mid);
        for (int k = 0; k < dc; ++k) {
            float a = xs[2 * k];
            a = a * w2[k];
            yd[2 * k] = a + yd[2 * k];
            float b = xs[2 * k + 1];
            b = b * w2[k];
            yd[2 * k + 1] = b + yd[2 * k + 1];
        }
    }
};

// The effect inside Reno's fixed blocks: the plug-in's stream engine is fed each block the way the filter's Transform does
// and what it puts out is queued; the block gets the next frames of the queue (short of sound, silence). The sound comes
// out longer or shorter than it went in, which a track's effect chain (a block in, a block out) cannot show: with a
// stretch above 100 % the queue grows (the sound plays slower than the timeline, VEGAS's "out of sync" for effects that
// are not in place), below 100 % it holds less than the blocks need. VEGAS offers these effects in track and event chains too (its manual:
// non-in-place plug-ins play out of sync, with plug-in delay compensation), so they stay real-time here.
class TimeEngine : public Engine {
public:
    explicit TimeEngine(bool express = false) : express_(express), core_(express) {}
    void setup(const int32_t* s, uint32_t rate, bool keep) override
    {
        double d;
        std::memcpy(&d, s, 8);
        if (!(d == d)) d = 10000.0;
        int mode = std::max(0, std::min(express_ ? 4 : 18, (int)s[2]));
        core_.setSettings(d, mode);
        if (!keep) {
            if (rate != rate_ || !started_) {
                core_.setRate(rate);
                rate_ = rate;
            }
            started_ = true;
            disc_ = true;
            q_.clear();
            head_ = 0;
        }
    }
    bool changesLength(const int32_t*) const override { return true; }
    // The whole sound the way the filter gets it: blocks of input, then the end of the stream (the engine is asked for what
    // it still holds until it has nothing more). Nothing is cut or padded: as many frames come out as the engine makes.
    bool whole(const float* inL, const float* inR, int64_t frames, std::vector<float>& outL, std::vector<float>& outR) override
    {
        outL.clear();
        outR.clear();
        const int block = 4096;
        std::vector<float> in((size_t)2 * block), ob((size_t)2 * kChunk);
        int mode = 0;
        auto take = [&](int outC) {
            for (int k = 0; k < outC; ++k) {
                outL.push_back(ob[(size_t)2 * k]);
                outR.push_back(ob[(size_t)2 * k + 1]);
            }
        };
        for (int64_t at = 0; at < frames; at += block) {
            const int n = (int)std::min<int64_t>(block, frames - at);
            for (int k = 0; k < n; ++k) {
                in[(size_t)2 * k] = inL[at + k];
                in[(size_t)2 * k + 1] = inR[at + k];
            }
            int rem = n;
            const float* p = in.data();
            for (;;) {
                int inC = rem, outC = kChunk;
                core_.run(mode, p, &inC, ob.data(), &outC);
                mode = 1;
                take(outC);
                if (inC >= rem) break;
                rem -= inC;
                p += 2 * (size_t)inC;
            }
        }
        for (int k = 0; k < 100000; ++k) {
            int inC = 0, outC = kChunk;
            const int r = core_.run(2, nullptr, &inC, ob.data(), &outC);
            take(outC);
            if ((r == 1 && outC == 0) || outC == 0) break;
        }
        return true;
    }
    void process(float* left, float* right, int frames) override
    {
        if (frames <= 0) return;
        in_.resize((size_t)2 * (size_t)frames);
        for (int k = 0; k < frames; ++k) {
            in_[(size_t)2 * k] = left[k];
            in_[(size_t)2 * k + 1] = right[k];
        }
        ob_.resize((size_t)2 * kChunk);
        int mode = disc_ ? 0 : 1;
        disc_ = false;
        int rem = frames;
        const float* p = in_.data();
        for (;;) {
            int inC = rem, outC = kChunk;
            core_.run(mode, p, &inC, ob_.data(), &outC);
            if (outC > 0) q_.insert(q_.end(), ob_.begin(), ob_.begin() + (size_t)2 * outC);
            if (inC >= rem) break;
            rem -= inC;
            p += 2 * (size_t)inC;
            mode = 1;
        }
        const size_t have = (q_.size() - head_) / 2;
        const size_t take = std::min<size_t>((size_t)frames, have);
        for (size_t k = 0; k < take; ++k) {
            left[k] = q_[head_ + 2 * k];
            right[k] = q_[head_ + 2 * k + 1];
        }
        for (size_t k = take; k < (size_t)frames; ++k) left[k] = right[k] = 0.f;
        head_ += 2 * take;
        // a stretch that runs ahead of the playing position for minutes is cut to what is within a minute and a half of it
        const size_t keepMax = (size_t)2 * 48000 * 90;
        if (q_.size() - head_ > keepMax) head_ = q_.size() - keepMax;
        if (head_ > (1u << 20)) {
            q_.erase(q_.begin(), q_.begin() + (std::ptrdiff_t)head_);
            head_ = 0;
        }
    }

private:
    static constexpr int kChunk = 8192;
    bool express_ = false;
    TimeCore core_;
    uint32_t rate_ = 0;
    bool started_ = false;
    bool disc_ = true;
    std::vector<float> in_, ob_, q_;
    size_t head_ = 0;
};

// The two effects share their settings (8 ints: the final length as a double in hundredths of a percent, the mode, the
// initial tempo in hundredths of a bpm, the initial time as a double in samples at 44100 a second, the input format).
inline Info makeTimeInfo(bool express)
{
    Info i;
    if (express) {
        i.uid = "reno.xptimestretch";
        i.name = "ExpressFX Time Stretch";
        i.clsid = "0000000b-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x84, 0x03, 0x0f, 0xe7, 0xb1, 0x64, 0xc0, 0x44, 0x8f, 0x7c, 0x8a, 0xc0, 0xcc, 0x8a, 0x22, 0xde};
        std::copy(g, g + 16, i.persistGuid);
    } else {
        i.uid = "reno.timestretch";
        i.name = "Time Stretch";
        i.clsid = "b97c0f22-196d-11d1-b99b-00a0c9053912";
        const uint8_t g[16] = {0xa4, 0xb4, 0xf5, 0x5f, 0x8f, 0x85, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
        std::copy(g, g + 16, i.persistGuid);
    }
    i.nInts = 8;
    i.bypassSlot = -1;
    const char* modes18 =
        "A01. Music 1 (minimum flange, may echo)|A02. Music 2|A03. Music 3 (less echo)|A04. Music 4 (fast, good for bass)|"
        "A05. Music 5|A06. Music 6|A07. Speech 1|A08. Speech 2|A09. Speech 3 (fast)|A10. Solo instruments 1|"
        "A11. Solo instruments 2|A12. Solo instruments 3|A13. Solo instruments 4 (less echo)|A14. Solo instruments 5|"
        "A15. Solo instruments 6|A16. Solo instruments 7 (fast)|A17. Drums, unpitched (minimum echo)|A18. Drums (better for toms)|"
        "A19. Drums (tiny echo)";
    const char* modes4 = "Music|Speech|Solo instruments|Drums, unpitched (minimum echo)|Drums (better for toms)";
    i.params = {
        {"Final Length", 50, 500, 100, "%", 0, 100, Dbl, "", 1, false},
        {"Mode", 0, express ? 4.0 : 18.0, 0, "", 2, 1, Choice, express ? modes4 : modes18, 0, false},
        {"Initial Tempo", 0, 500, 120, "bpm", 3, 100, Num, "", 2, false},
        {"Initial Time", 0, 793800000.0, 2646000.0, "", 4, 1, Dbl, "", 0, false},
        {"Input Format", 0, 2, 0, "", 6, 1, Choice, "Percentage|Tempo (bpm)|Time (hr:mn:sc.xxx)", 0, false},
    };
    auto preset = [](const char* name, double d30) {
        std::vector<int32_t> v(8, 0);
        const double ini = 2646000.0;
        std::memcpy(&v[0], &d30, 8);
        v[3] = 12000;
        std::memcpy(&v[4], &ini, 8);
        return Preset{name, v};
    };
    i.presets = {
        preset("(Default)", 10000.0),
        preset("Default all parameters", 10000.0),
        preset("Time expand 110 % of original", 11000.0),
        preset("Time compress 85 % of original", 8500.0),
    };
    if (express) i.make = [] { return std::unique_ptr<Engine>(new TimeEngine(true)); };
    else i.make = [] { return std::unique_ptr<Engine>(new TimeEngine(false)); };
    return i;
}

inline const Info& timeInfo()
{
    static const Info info = makeTimeInfo(false);
    return info;
}

inline const Info& xpTimeInfo()
{
    static const Info info = makeTimeInfo(true);
    return info;
}

}  // namespace vegpack1
}  // namespace reno
