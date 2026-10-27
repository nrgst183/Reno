// VEGAS's ExpressFX Dynamics (sfxpfx3_x64.dll, CXpDyn, DirectX class 00000008-0f56-11d2-9887-00a0c969725b): its settings,
// presets and sound, from the DLL's own code ("apply settings", the block routine and the framework that feeds it).
//
// The sound is a block compressor / limiter: 20 ms of the sound are collected (both channels), the loudest sample of the
// block is looked up in a gain curve (a table of 841 entries, one per tenth of a dB from 0 dB down to -84 dB, made from
// the Threshold and Ratio), the gain is smoothed from the last block's gain towards the table's (one pole, a different
// coefficient for attack and release) and applied to the block as a ramp, one gain for both channels. The block comes out
// when it is complete, so the effect delays the sound by one block (latency()).
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace reno {
namespace vegpack1 {

// The settings structure (5 ints): 0 Threshold in tenths of a dB (-800..-1), 1 Ratio in tenths (10..500, 500 = Inf.:1),
// 2 Attack in tenths of a ms (0..5000), 3 Release in tenths of a ms (0..50000), 4 Auto gain compensate.
class XpDynamicsEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        rate_ = rate ? rate : 48000u;
        block_ = (int)((double)rate_ * 0.02);
        if (block_ < 1) block_ = 1;
        prepare(p);
        peak_[0] = peak_[1] = 0.0;            // the plug-in forgets the peaks of the block being collected
        if (!keep || in_[0].size() != (size_t)block_) {
            in_[0].assign((size_t)block_, 0.f);
            in_[1].assign((size_t)block_, 0.f);
            q_[0].assign((size_t)block_ * 3, 0.f);
            q_[1].assign((size_t)block_ * 3, 0.f);
            fill_ = 0;
            env_ = 1.0;
            mode_ = 0;
            head_ = 0;
            count_ = block_;                  // the first block comes out silent
        }
    }
    int latency(const int32_t*, uint32_t rate) const override
    {
        const int b = (int)((double)(rate ? rate : 48000u) * 0.02);
        return b < 1 ? 1 : b;
    }
    void process(float* left, float* right, int frames) override
    {
        if (in_[0].empty()) return;
        const size_t cap = q_[0].size();
        for (int k = 0; k < frames; ++k) {
            const float x0 = left[k], x1 = right[k];
            in_[0][(size_t)fill_] = x0;
            in_[1][(size_t)fill_] = x1;
            peakOf(0, (double)x0);
            peakOf(1, (double)x1);
            ++fill_;
            if (fill_ >= block_) {
                processBlock();
                for (int i = 0; i < block_; ++i) {
                    const size_t at = (head_ + (size_t)count_ + (size_t)i) % cap;
                    q_[0][at] = in_[0][(size_t)i];
                    q_[1][at] = in_[1][(size_t)i];
                }
                count_ += block_;
                fill_ = 0;
                peak_[0] = peak_[1] = 0.0;
            }
            // one frame out: the oldest one
            if (count_ > 0) {
                left[k] = q_[0][head_];
                right[k] = q_[1][head_];
                head_ = (head_ + 1) % cap;
                --count_;
            } else {
                left[k] = right[k] = 0.f;
            }
        }
    }

private:
    uint32_t rate_ = 48000;
    int block_ = 960;
    // the plug-in's prepared values
    double thr_ = 0, atk_ = 1, rel_ = 1, gscale_ = 1, gmax_ = 50.0;
    int atkS_ = 0, relS_ = 0;
    double table_[841];
    // streaming state
    std::vector<float> in_[2], q_[2];
    int fill_ = 0, count_ = 0;
    size_t head_ = 0;
    double peak_[2] = {0, 0};
    double env_ = 1.0;
    int mode_ = 0;

    void peakOf(int c, double x)
    {
        if (x > peak_[c]) peak_[c] = x;
        else if (-x > peak_[c]) peak_[c] = -x;
    }
    static int toInt(double x)      // cvttsd2si
    {
        if (!(x > -2147483649.0 && x < 2147483648.0)) return (int)0x80000000u;
        return (int)x;
    }
    // the plug-in's linear fill of the gain curve between two entries
    void fillBetween(int a, int b)
    {
        int lo = a, hi = b;
        if (a > b) { lo = b; hi = a; }
        const double span = (double)(hi - lo);
        if (span == 0.0) return;
        const double step = (table_[hi] - table_[lo]) / span;
        for (int k = lo + 1; k < hi; ++k) table_[k] = (double)(k - lo) * step + table_[lo];
    }

    // "apply settings": times in samples, smoothing coefficients, and the gain curve in linear gain
    void prepare(const int32_t* s)
    {
        const double rate = (double)rate_;
        const double attack = (double)s[2] / 10000.0, release = (double)s[3] / 10000.0;
        atkS_ = toInt(rate * attack);
        relS_ = toInt(rate * release);
        thr_ = std::pow(10.0, (double)s[0] / 200.0);
        if (release > 0.02) rel_ = 1.0 - std::pow(0.1, 0.02 / release);
        else rel_ = 1.0;
        if (attack > 0.02) atk_ = 1.0 - std::pow(0.1, 0.02 / attack);
        else atk_ = 1.0;
        gmax_ = 50.0;
        gscale_ = 1.0;
        if (s[4] != 0) gscale_ *= 0.98;
        buildTable(s);
        for (int i = 0; i < 841; ++i) {
            const double v = table_[i];
            table_[i] = (v == -840.0) ? 0.0 : std::pow(10.0, v / 200.0);
        }
    }
    // the curve in tenths of a dB first: gain change against input level (entry i is the level -i/10 dB)
    void buildTable(const int32_t* s)
    {
        const int i0 = -s[0];
        table_[840] = 0.0;
        table_[i0] = 0.0;
        fillBetween(i0, 840);
        table_[i0] = 0.0;
        int edi;
        if (s[1] == 500) edi = s[0];
        else edi = toInt((1.0 - 10.0 / (double)s[1]) * (double)s[0] - 0.5);
        if (edi < -840) edi = -840;
        else if (edi > 0) edi = 0;
        table_[0] = (double)edi;
        if (edi == -840 && s[0] == edi) {
            table_[0] = -840.0;
            table_[i0] = -840.0;
        }
        fillBetween(0, i0);
        if (edi < 0 && s[4] != 0)
            for (int k = 0; k < 841; ++k) table_[k] -= (double)edi;
    }

    // the plug-in's block routine (float version), for the block in in_[]
    void processBlock()
    {
        const int n = block_;
        double pk = peak_[0];
        if (peak_[1] > pk) pk = peak_[1];
        mode_ = (pk >= thr_) ? 2 : 0;
        double lvl;
        if (3.051758e-05 > pk) lvl = -840.0;
        else lvl = std::log10(pk) * 200.0;
        if (-840.0 > lvl) lvl = -840.0;
        else if (lvl > 0.0) lvl = 0.0;
        const int ic = toInt(lvl);
        double g = table_[-ic];
        if (lvl > -840.0) {
            double d = table_[-ic + 1] - g;
            d *= ((double)ic - lvl);
            g += d;
        }
        const double envOld = env_;
        double target;
        double base;
        int cnt;
        if (mode_ != 0) {
            target = g * atk_;
            cnt = atkS_;
            base = envOld * (1.0 - atk_);
        } else {
            target = g * rel_;
            cnt = relS_;
            base = envOld * (1.0 - rel_);
        }
        double e = target + base;
        if (e > gmax_) e = gmax_;
        double start = envOld * gscale_;
        env_ = e;
        double end = e * gscale_;
        if (0.0002 > end) {
            end = 0.0;
            env_ = 0.0;
        } else if (end > 5000.0) {
            end = 5000.0;
            env_ = 5000.0;
        }
        double slope;
        if (cnt > n) slope = (end - start) / (double)n;
        else if (cnt != 0) slope = (end - start) / (double)cnt;
        else {
            start = end;
            slope = 0.0;
        }
        double cur = start;
        float* a = in_[0].data();
        float* b = in_[1].data();
        for (int i = 0; i < n; ++i) {
            a[i] = (float)((double)a[i] * cur);
            b[i] = (float)((double)b[i] * cur);
            if ((slope > 0.0 && end > cur) || (0.0 > slope && cur > end)) cur += slope;
        }
    }
};

inline const Info& xpDynamicsInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpdynamics";
        i.name = "ExpressFX Dynamics";
        i.clsid = "00000008-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x36, 0xed, 0xa0, 0x65, 0xa1, 0x90, 0xf6, 0x46, 0x99, 0xb7, 0xfc, 0x52, 0x58, 0x47, 0x09, 0x01};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 5;
        i.bypassSlot = -1;
        i.params = {
            {"Threshold", -80, -0.1, -20, "dB", 0, 10, Num, "", 1, false},
            {"Ratio", 1, 50, 1, ":1", 1, 10, Num, "", 1, false, "Inf.:1"},
            {"Attack", 0, 500, 1, "ms", 2, 10, Num, "", 1, false},
            {"Release", 0, 5000, 500, "ms", 3, 10, Num, "", 1, false},
            {"Auto gain compensate", 0, 1, 0, "", 4, 1, Bool, "", 0, false},
        };
        i.presets = {
            {"(Default)", {-200, 10, 10, 5000, 0}},
            {"Default all parameters", {-200, 10, 10, 5000, 0}},
            {"Limit levels to -6 dB", {-60, 500, 10, 5000, 0}},
            {"Maximize volume (-12 dB limiter with gain compensation)", {-120, 500, 50, 1000, 1}},
            {"2:1 compression starting at -18 dB", {-180, 20, 10, 5000, 1}},
            {"3:1 compression starting at -15 dB", {-150, 30, 10, 5000, 1}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpDynamicsEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
