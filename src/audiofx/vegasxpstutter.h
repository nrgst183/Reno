// VEGAS's ExpressFX Stutter (sfxpfx1_x64.dll, CXpStuttr, DirectX class 00000007-0f56-11d2-9887-00a0c969725b): its
// settings, presets and sound. The engine is the filter's own code (its "apply settings" routine and its per-sample
// routine, called frame by frame for each channel) translated line by line.
//
// The sound: it alternates a stretch of silence and a stretch of the sound; the sound fades in over the first and out
// over the last eighth of its length. With "Add space" on the second channel's stretches run on from the first one's
// (the plug-in's settings routine moves their positions together each time it runs: when the settings change).
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace vegpack1 {

// The settings structure (4 ints): 0 Silence and 1 Sound in hundredths of a millisecond (1..20000), 2 Add space,
// 3 the mix of the effect against the original in tenths of a percent.
class XpStutterEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        std::memcpy(s_, p, sizeof s_);
        rate_ = (int32_t)rate;
        if (!keep) {
            std::memset(flag_, 0, sizeof flag_);
            std::memset(cnt_, 0, sizeof cnt_);
            std::memset(pos_, 0, sizeof pos_);
            env_[0] = env_[1] = 0.0;
        }
        prep();
    }
    void process(float* left, float* right, int frames) override
    {
        for (int k = 0; k < frames; ++k) {
            left[k] = (float)sample((double)left[k], 0);
            right[k] = (float)sample((double)right[k], 1);
        }
    }

private:
    int32_t s_[4] = {100, 100, 0, 500};
    int32_t rate_ = 48000;
    double wet_ = 0, dry_ = 1, step_ = 1;
    int32_t n1_ = 1, n2_ = 1, fade_ = 1;
    int32_t flag_[2] = {0, 0}, cnt_[2] = {0, 0}, pos_[2] = {0, 0};
    double env_[2] = {0, 0};

    // Windows' MulDiv
    static int32_t mulDiv(int32_t a, int32_t b, int32_t c)
    {
        const int64_t n = (int64_t)a * b;
        return (int32_t)((n >= 0 ? n + c / 2 : n - c / 2) / c);
    }

    // the plug-in's "apply settings" routine (0x1800165c0)
    void prep()
    {
        wet_ = (double)s_[3] / 1000.0;
        dry_ = 1.0 - wet_;
        n1_ = mulDiv(s_[0], rate_, 100000);
        if (n1_ < 1) n1_ = 1;
        n2_ = mulDiv(s_[1], rate_, 100000);
        if (n2_ < 1) n2_ = 1;
        const double fadeLen = (double)n2_ * 0.125;
        double v = fadeLen + 1.5e-8;
        v += 6755399441055744.0;                // the low half of the sum's bits is the rounded number
        uint64_t bits;
        std::memcpy(&bits, &v, 8);
        fade_ = (int32_t)(uint32_t)bits;
        step_ = fade_ > 1 ? 1.0 / fadeLen : 1.0;
        // two channels
        if (s_[2] == 0) {
            cnt_[0] = cnt_[1];
            pos_[0] = pos_[1];
            env_[0] = env_[1];
            flag_[0] = flag_[1];
        } else {
            flag_[0] = flag_[1] == 0 ? 1 : 0;
            cnt_[0] = pos_[1];
            if (pos_[1] >= n1_) {
                pos_[0] -= n1_;
                int32_t c = pos_[0];
                cnt_[0] = 0;
                flag_[0] = 0;
                if (c >= fade_) {
                    if (c <= n2_ - fade_) {
                        env_[0] = 1.0;
                    } else {
                        c = c - n2_ + fade_;
                        const double e = 1.0 - (double)c * step_;
                        env_[0] = e;
                        if (0.0 > e) env_[0] = 0.0;
                    }
                } else {
                    const double e = (double)c * step_;
                    env_[0] = e;
                    if (e > 1.0) env_[0] = 1.0;
                }
            }
            pos_[0] = cnt_[1];
            if (cnt_[1] >= n2_) {
                cnt_[0] -= n2_;
                pos_[0] = 0;
                flag_[0] = 1;
                env_[0] = 0.0;
            }
        }
    }

    // the plug-in's per-sample routine (0x180016410)
    double sample(double x, int c)
    {
        double y;
        bool done = false;
        if (flag_[c] != 0) {
            ++cnt_[c];
            y = 0.0;
            if (cnt_[c] >= n1_) {
                flag_[c] = 0;
                done = true;
            }
        } else {
            if (pos_[c] < fade_) {
                double e = env_[c];
                y = e * x;
                e += step_;
                env_[c] = e;
                if (e > 1.0) env_[c] = 1.0;
            } else if (pos_[c] <= n2_ - fade_) {
                y = x;
            } else {
                const double e = env_[c];
                y = e * x;
                const double e2 = e - step_;
                env_[c] = e2;
                if (0.0 > e2) env_[c] = 0.0;
            }
            ++pos_[c];
            if (pos_[c] >= n2_) {
                flag_[c] = 1;
                done = true;
            }
        }
        if (done) {
            cnt_[c] = 0;
            pos_[c] = 0;
            env_[c] = 0.0;
        }
        return y * wet_ + x * dry_;
    }
};

inline const Info& xpStutterInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpstutter";
        i.name = "ExpressFX Stutter";
        i.clsid = "00000007-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0x4c, 0xd7, 0x9d, 0xf5, 0xe1, 0x14, 0xd2, 0x11, 0xb3, 0xb2, 0x00, 0xa0, 0xc9, 0x06, 0x42, 0xcc};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 4;
        i.bypassSlot = -1;
        i.params = {
            {"Silence", 0.01, 200, 1, "ms", 0, 100, Num, "", 2, false},
            {"Sound", 0.01, 200, 1, "ms", 1, 100, Num, "", 2, false},
            {"Add Space", 0, 1, 0, "", 2, 1, Bool, "", 0, false},
            {"Mix", 0, 100, 50, "%", 3, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {100, 100, 0, 500}},
            {"Default all parameters", {100, 100, 0, 500}},
            {"Crystal ball", {2000, 3500, 1, 1000}},
            {"Gravel", {3000, 2000, 0, 750}},
            {"Low bell", {100, 600, 1, 650}},
            {"Walkie-talkie", {100, 100, 1, 500}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpStutterEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
