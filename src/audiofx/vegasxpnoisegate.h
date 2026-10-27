// VEGAS's ExpressFX Noise Gate (sfxpfx3_x64.dll, CXpNoisgt, DirectX class 0000000a-0f56-11d2-9887-00a0c969725b). Its settings
// (attack, release, threshold), its presets and its sound are those of the sfppack2 Noise Gate (vegasnoisegate.h: the same
// 20 ms block gate, checked against this DLL's own code too); only the saved form's GUID and the class differ. The sound
// comes out one block late (latency()).
#pragma once
#include "vegasnoisegate.h"
#include "vegaspack1.h"
#include <algorithm>
#include <vector>

namespace reno {
namespace vegpack1 {

class XpNoiseGateEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        vegng::Settings s;
        s.attack = p[0];
        s.release = p[1];
        s.threshold = p[2];
        dsp_.setSettings(s);
        if (!keep || !formatted_ || rate_ != rate) {
            rate_ = rate ? rate : 48000u;
            dsp_.setFormat((double)rate_, 2);
            dsp_.restart();
            formatted_ = true;
            const int b = dsp_.block();
            q_[0].assign((size_t)b * 3, 0.f);
            q_[1].assign((size_t)b * 3, 0.f);
            head_ = 0;
            count_ = b;                      // the first block comes out silent
        }
        dsp_.applyChanges();
    }
    int latency(const int32_t*, uint32_t rate) const override
    {
        const int b = (int)((double)(rate ? rate : 48000u) * 0.02);
        return b < 1 ? 1 : b;
    }
    void process(float* left, float* right, int frames) override
    {
        if (!formatted_) return;
        const size_t cap = q_[0].size();
        const int block = dsp_.block();
        for (int k = 0; k < frames; ++k) {
            const float in[2] = {left[k], right[k]};
            if (dsp_.push(in)) {
                for (int i = 0; i < block; ++i) {
                    const size_t at = (head_ + (size_t)count_ + (size_t)i) % cap;
                    q_[0][at] = dsp_.outAt(0, i);
                    q_[1][at] = dsp_.outAt(1, i);
                }
                count_ += block;
            }
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
    vegng::Dsp dsp_;
    std::vector<float> q_[2];
    size_t head_ = 0;
    int count_ = 0;
    uint32_t rate_ = 0;
    bool formatted_ = false;
};

inline const Info& xpNoiseGateInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpnoisegate";
        i.name = "ExpressFX Noise Gate";
        i.clsid = "0000000a-0f56-11d2-9887-00a0c969725b";
        const uint8_t g[16] = {0xba, 0xe8, 0x04, 0x52, 0x57, 0x46, 0x33, 0x47, 0xa6, 0xeb, 0x1e, 0x65, 0xf7, 0x6c, 0x99, 0xa4};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 3;
        i.bypassSlot = -1;
        i.params = {
            {"Threshold", -80, 0, -20, "dB", 2, 10, Num, "", 1, true},
            {"Attack", 1, 500, 3, "ms", 0, 1, Num, "", 0, false},
            {"Release", 1, 5000, 100, "ms", 1, 1, Num, "", 0, false},
        };
        i.presets = {
            {"(Default)", {3, 100, -200}},
            {"Default all parameters", {3, 100, -200}},
            {"Strong beats only", {1, 300, -60}},
            {"Long decay time", {10, 1000, -380}},
            {"Noise gate 1", {10, 500, -600}},
            {"Noise gate 2", {1, 500, -320}},
            {"Slow attack", {300, 1000, -400}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpNoiseGateEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
