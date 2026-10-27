// VEGAS's ExpressFX Chorus (sfxpfx2_x64.dll, CXpChorus, DirectX class 00000000-0f56-11d2-9887-00a0c969725b): its settings,
// presets and sound. The engine is the plug-in's own code translated line by line: its two "apply settings" routines (the
// full one that starts the voices over, the light one for a change of rate, depth or mix), its reset, and the routine that
// runs one voice over one stereo sample (up to three voices, one after the other, as the plug-in does), down to the
// quirks (the full routine clears only the first half of each delay line).
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace vegpack1 {

// The settings structure (4 ints): 0 chorus size (voices, 1 to 3), 1 modulation rate in mHz (1 to 20000), 2 modulation depth
// in % (1 to 100), 3 the mix, chorus against the original, in tenths of a percent (0 to 1000).
class XpChorusEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        if (!allocated_ || rate != rate_ || !keep || !prepared_) {
            // a new connection or a jump in the stream: the plug-in starts again with silent buffers and the settings it has
            if (!allocated_ || rate != rate_) bufLen_ = lineLength(rate);
            rate_ = rate;
            reset(p);
            return;
        }
        // a change of the settings in the middle of the stream
        if (p[0] != size_) needFull_ = true;
        apply(p);
    }

    void process(float* left, float* right, int frames) override
    {
        if (!allocated_) return;
        for (int k = 0; k < frames; ++k) {
            double l = (double)left[k], r = (double)right[k];
            voice(0, &l, &r);
            if (size_ == 2 || size_ == 3) voice(1, &l, &r);
            if (size_ == 3) voice(2, &l, &r);
            left[k] = (float)l;
            right[k] = (float)r;
        }
    }

private:
    // the plug-in's tables: each voice's modulation period relative to the first, and the gains of the left and right output
    static constexpr double kMult[3] = {1.0, 0.811731, 1.132329};
    static constexpr double kGainL = 0.9, kGainR = 0.95, kStep = 0.9;
    static constexpr double kTwoPi = 6.28318530718;

    static int lineLength(uint32_t rate)
    {
        double x = (double)(float)rate;
        x *= 2.0;
        x /= 10.0;
        x += 100.0;
        return (int)std::ceil(x);
    }

    // the plug-in's reset: new silent buffers, the voices at their start, then the full apply
    void reset(const int32_t* p)
    {
        for (int v = 0; v < 3; ++v) {
            buf_[v].assign((size_t)bufLen_ * 2, 0.0);
            w_[v] = 0;
            st_[v][0] = st_[v][1] = 0.0;
            ctr_[v] = 0;
        }
        allocated_ = true;
        needFull_ = true;
        apply(p);
    }

    void apply(const int32_t* p)
    {
        wet_ = (double)p[3] / 1000.0;
        dry_ = 1.0 - wet_;
        if (needFull_) {
            full(p);
            needFull_ = false;
            prepared_ = true;
        } else {
            light(p);
        }
    }

    void period(const int32_t* p)
    {
        if (p[1] != 0) periodSec_ = 1000.0 / (double)p[1];
        else periodSec_ = 1.0;
    }

    void full(const int32_t* p)
    {
        for (int v = 0; v < 3; ++v) std::fill(buf_[v].begin(), buf_[v].begin() + bufLen_, 0.0);      // (the first half only)
        size_ = p[0];
        period(p);
        maxDepth_ = (double)(int)((double)rate_ * 200.0 / 10000.0);
        depth_ = (double)(int)((double)p[2] / 100.0 * maxDepth_);
        const double rate = (double)rate_;
        for (int v = 0; v < 3; ++v) {
            w_[v] = 0;
            st_[v][0] = st_[v][1] = 0.0;
            off_[v] = (double)(int)(kMult[v] * maxDepth_);
            const double n = (double)(int)(rate * periodSec_ * kMult[v]);
            periodSamples_[v] = n;
            inc_[v] = kTwoPi / n;
        }
    }

    void light(const int32_t* p)
    {
        period(p);
        const double rate = (double)rate_;
        for (int v = 0; v < 3; ++v) {
            const double n = (double)(int)(rate * periodSec_ * kMult[v]);
            periodSamples_[v] = n;
            inc_[v] = kTwoPi / n;
        }
        depth_ = (double)(int)((double)p[2] / 100.0 * maxDepth_);
    }

    static bool tiny(double v) { return 2e-11 > std::fabs(v); }

    // one voice over one stereo sample (*pl, *pr)
    void voice(int v, double* pl, double* pr)
    {
        double xL, xR;
        if (v == 0) {
            xR = 1.0 * *pr;
            xL = 1.0 * *pl;
        } else {
            xL = *pl;
            xR = *pr;
        }
        const double s = std::sin((double)ctr_[v] * inc_[v]);
        double x = s * depth_ + off_[v];
        const double bd = (double)(uint32_t)bufLen_;
        if (!(x < bd)) x += (0.0 - bd);
        else if (0.0 > x) x += bd;
        // (the plug-in rounds x - 0.499999985 to the nearest whole number with the usual add-and-read trick)
        double tt = x - 0.499999985;
        tt += 6755399441055744.0;
        uint64_t bits;
        std::memcpy(&bits, &tt, 8);
        const int32_t n = (int32_t)(uint32_t)(bits & 0xffffffffu);
        const uint32_t m = (uint32_t)bufLen_ * 2u;
        const uint32_t curL = (uint32_t)(n + n);
        const uint32_t nextL = (curL + 2u) % m;
        const double f = x - (double)n;
        const std::vector<double>& b = buf_[v];
        // (right at the end of the line the plug-in reads one place past it, whatever the heap holds there: a tiny number)
        auto rd = [&](uint32_t k) { return k < m ? b[k] : 0.0; };
        double l1 = rd(nextL);
        l1 -= rd(curL);
        l1 *= f;
        l1 += rd(curL);
        const uint32_t curR = (curL + 1u) % m;
        l1 *= 1.0;
        double r0 = rd(curR);
        const uint32_t nextR = (nextL + 1u) % m;
        double r1 = rd(nextR);
        r1 -= r0;
        r1 *= f;
        r1 += r0;
        double sL = l1 + 0.0 * st_[v][0];
        if (tiny(sL)) sL = 0.0;
        st_[v][0] = sL;
        r1 *= 1.0;
        double sR = r1 + 0.0 * st_[v][1];
        if (tiny(sR)) sR = 0.0;
        st_[v][1] = sR;
        off_[v] -= 1.0;
        if (0.0 > off_[v]) off_[v] = (double)(uint32_t)(bufLen_ - 1);
        ++ctr_[v];
        if (!((double)ctr_[v] < periodSamples_[v])) ctr_[v] = 0;
        if (v == 0) {
            *pl = sL * kStep * kGainL + xL * 0.0;
            *pr = sR * kStep * kGainR + xR * 0.0;
        } else {
            *pl = sL * kGainL + xL;
            *pr = sR * kGainR + xR;
        }
        // into the voice's delay line (the feedback of this effect is nothing: the plug-in still multiplies)
        std::vector<double>& wb = buf_[v];
        const uint32_t at = ((uint32_t)w_[v] * 2u) % m;
        wb[at] = 0.0 * sL + xL;
        wb[(at + 1u) % m] = 0.0 * sR + xR;
        if (--w_[v] < 0) w_[v] = (int)((uint32_t)bufLen_ - 1u);
        // the mix with the original
        const double outL = dry_ * xL + wet_ * *pl;
        const double outR = dry_ * xR + wet_ * *pr;
        *pl = outL;
        *pr = outR;
    }

    bool allocated_ = false, prepared_ = false, needFull_ = true;
    uint32_t rate_ = 0;
    int bufLen_ = 0, size_ = 1;
    std::vector<double> buf_[3];
    int ctr_[3] = {0, 0, 0}, w_[3] = {0, 0, 0};
    double off_[3] = {0, 0, 0}, st_[3][2] = {{0, 0}, {0, 0}, {0, 0}}, periodSamples_[3] = {1, 1, 1}, inc_[3] = {0, 0, 0};
    double wet_ = 0, dry_ = 1, periodSec_ = 1, depth_ = 0, maxDepth_ = 0;
};
constexpr double XpChorusEngine::kMult[3];

inline const Info& xpChorusInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpchorus";
        i.name = "ExpressFX Chorus";
        i.clsid = "00000000-0f56-11d2-9887-00a0c969725b";
        // the effect's own GUID, written before its settings: 40986924-0f56-11d2-9887-00a0c969725b
        const uint8_t g[16] = {0x24, 0x69, 0x98, 0x40, 0x56, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 4;
        i.bypassSlot = -1;
        i.params = {
            {"Size", 1, 3, 1, "", 0, 1, Num, "", 0, false},
            {"Rate", 0.001, 20, 0.1, "Hz", 1, 1000, Num, "", 3, false},
            {"Depth", 1, 100, 50, "%", 2, 1, Num, "", 0, false},
            {"Mix", 0, 100, 50, "%", 3, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {1, 100, 50, 500}},
            {"Default all parameters", {1, 100, 50, 500}},
            {"Wacky 1", {1, 4000, 50, 1000}},
            {"Wacky 2", {1, 1000, 50, 1000}},
            {"Wacky 3", {3, 5000, 10, 750}},
            {"Wacky 4", {2, 10000, 50, 1000}},
            {"Slow light chorus", {1, 150, 25, 500}},
            {"Slow medium chorus", {2, 300, 25, 500}},
            {"Slow heavy chorus", {3, 300, 40, 750}},
            {"Fast light chorus", {1, 5000, 3, 400}},
            {"Fast medium chorus", {2, 5000, 5, 500}},
            {"Fast heavy chorus", {3, 4000, 7, 750}},
            {"Guitar", {3, 4000, 1, 1000}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpChorusEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
