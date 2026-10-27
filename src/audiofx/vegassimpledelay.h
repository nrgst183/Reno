// VEGAS's Simple Delay (sfppack1_x64.dll, CSfEchos, DirectX class 54f29260-79b1-11d0-aebc-00a0c9053912): its settings,
// presets and sound. The engine below is the plug-in's own code (its "apply settings" routine and the float sample loop)
// translated line by line.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>

namespace reno {
namespace vegpack1 {

// Names of the "note" lengths of a Tempo sync setting, shortest first; kNoteQuarters is each in quarter notes.
#define VEGPACK1_NOTES "64th Note Triplet,64th Note,Dotted 64th Note,32nd Note Triplet,32nd Note,Dotted 32nd Note,16th Note Triplet,16th Note,Dotted 16th Note,8th Note Triplet,8th Note,Dotted 8th Note,Quarter Note Triplet,Quarter Note,Dotted Quarter Note,Half Note Triplet,Half Note,Dotted Half Note"
inline const double* noteQuarters()
{
    static const double t[18] = {0.041666666666666664, 0.0625, 0.09375, 0.08333333333333333, 0.125, 0.1875, 0.16666666666666666, 0.25, 0.375,
                                 0.3333333333333333, 0.5, 0.75, 0.6666666666666666, 1.0, 1.5, 1.3333333333333333, 2.0, 3.0};
    return t;
}

// The settings structure (12 ints): 0 multiple delays on, 1 delay in ms, 2 decay in tenths of a second, 3 delay out
// and 4 dry out in tenths of a dB (-800 and below: none), 5 bypass, 6 delay note, 7 delay multiplier (x1000), 8 tempo
// sync delay, 9 decay note (18: measures), 10 decay multiplier (x1000), 11 tempo sync decay.
class SimpleDelayEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        const Tempo t = tempo();
        const Tempo* tp = &t;
        // the two gains (dB in tenths -> amplitude)
        gDelay_ = p[3] > -800 ? std::pow(10.0, (double)p[3] / 200.0) : 0.0;
        gDry_ = p[4] > -800 ? std::pow(10.0, (double)p[4] / 200.0) : 0.0;
        const bool fresh = !keep;
        const double mult = (double)p[7] / 1000.0;
        int64_t d;
        if (p[8] != 0 && tp) {
            double spb = 2.0;
            if (tp->flags & 1) spb = 60.0 / tp->bpm;
            spb *= noteQuarters()[std::max(0, std::min(17, (int)p[6]))];
            spb *= mult;
            d = (int64_t)(spb * (double)rate + 0.5);
        } else {
            d = ((int64_t)p[1] * (int64_t)rate + 500) / 1000;
        }
        uint32_t delayLen = (uint32_t)d;
        if (d < 0 || d > (int64_t)rate * 60) delayLen = rate * 60;
        if (delayLen <= 1) delayLen = 2;
        const int chans = 2;
        if (p[0] == 0) {
            decayLen_ = 0;
        } else {
            int64_t dl;
            if (p[11] != 0 && tp) {
                const double m = (double)p[10] / 1000.0;
                double spb = 2.0;
                if (tp->flags & 1) spb = 60.0 / tp->bpm;
                double x;
                if (p[9] == 18) {
                    double beats = 4.0;
                    if (tp->flags & 4) beats = (4.0 / (double)tp->den) * (double)tp->num;
                    x = beats * spb;
                } else {
                    x = spb * noteQuarters()[std::max(0, std::min(17, (int)p[9]))];
                }
                x *= m;
                dl = (int64_t)((double)rate * (double)chans * x);
            } else {
                dl = (int64_t)((double)rate * (double)chans * (double)p[2] / 10.0);
            }
            decayLen_ = (uint32_t)std::max<int64_t>(0, std::min<int64_t>(dl, (int64_t)rate * 2 * 60));
        }
        const double ratio = (double)decayLen_ / (double)(uint32_t)(chans * delayLen);
        if (0.1 > ratio) fb_ = 0;
        else fb_ = std::pow(0.0316, 1.0 / ratio);
        if (delayLen_ == delayLen) {
            if (fresh)
                for (int c = 0; c < chans; ++c) {
                    if (line_[c].size() == delayLen) std::fill(line_[c].begin(), line_[c].end(), 0.0);
                    else line_[c].assign(delayLen, 0.0);
                    idx_[c] = 0;
                }
        } else {
            for (int c = 0; c < chans; ++c) {
                line_[c].assign(delayLen, 0.0);
                idx_[c] = 0;
            }
            delayLen_ = delayLen;
        }
    }
    void process(float* left, float* right, int n) override
    {
        if (delayLen_ == 0) return;
        float* ch[2] = {left, right};
        for (int c = 0; c < 2; ++c) {
            double* line = line_[c].data();
            uint32_t idx = idx_[c];
            float* b = ch[c];
            for (int k = 0; k < n; ++k) {
                const double x = (double)b[k];
                const double y = line[idx];
                line[idx] = y * fb_ + x;
                const double out = x * gDry_ + y * gDelay_;
                if (++idx >= delayLen_) idx = 0;
                b[k] = (float)out;
            }
            idx_[c] = idx;
        }
    }

private:
    double gDelay_ = 0, gDry_ = 0, fb_ = 0;
    uint32_t delayLen_ = 0, decayLen_ = 0;
    std::vector<double> line_[2];
    uint32_t idx_[2] = {0, 0};
};

inline const Info& simpleDelayInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.simpledelay";
        i.name = "Simple Delay";
        i.clsid = "54f29260-79b1-11d0-aebc-00a0c9053912";
        const uint8_t g[16] = {0x09, 0x0b, 0xcb, 0x9a, 0xdb, 0xc5, 0x31, 0x44, 0xbd, 0x64, 0x74, 0x1b, 0xdd, 0xa7, 0x1a, 0xe2};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 12;
        i.bypassSlot = 5;
        i.params = {
            {"Delay Time", 1, 5000, 100, "ms", 1, 1, Num, "", 0, false},
            {"Decay Time", 0.1, 20, 0.1, "s", 2, 10, Num, "", 1, false},
            {"Multiple Delays", 0, 1, 0, "", 0, 1, Bool, "", 0, false},
            {"Dry Out", -80, 0, -6, "dB", 4, 10, Num, "", 1, true},
            {"Delay Out", -80, 0, -6, "dB", 3, 10, Num, "", 1, true},
            {"Bypass", 0, 1, 0, "", 5, 1, Bool, "", 0, false},
            {"Delay Time (Tempo)", 0, 17, 13, "", 6, 1, Choice, VEGPACK1_NOTES, 0, false},
            {"Delay Time Multiplier (Tempo)", 0.001, 4, 1, "", 7, 1000, Num, "", 3, false},
            {"Tempo Sync Delay", 0, 1, 0, "", 8, 1, Bool, "", 0, false},
            {"Decay Time (Tempo)", 0, 18, 18, "", 9, 1, Choice, VEGPACK1_NOTES ",Measures", 0, false},
            {"Decay Time Multiplier (Tempo)", 0.001, 32, 1, "", 10, 1000, Num, "", 3, false},
            {"Tempo Sync Decay", 0, 1, 0, "", 11, 1, Bool, "", 0, false},
        };
        i.presets = {
            {"(Default)", {0, 100, 1, -60, -60, 0, 13, 1000, 0, 18, 1000, 0}},
            {"Echo chamber", {1, 400, 20, -30, -90, 0, 13, 1000, 0, 18, 1000, 0}},
            {"Slap-back echo", {0, 400, 20, -120, -90, 0, 13, 1000, 0, 18, 1000, 0}},
            {"Grand Canyon", {1, 1000, 30, -140, -40, 0, 13, 1000, 0, 18, 1000, 0}},
            {"Shift selection forwards  (select one channel only)", {0, 1000, 20, 0, -800, 0, 13, 1000, 0, 18, 1000, 0}},
            {"Pseudo-stereo (select one channel only)", {0, 10, 1, 0, -800, 0, 13, 1000, 0, 18, 1000, 0}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new SimpleDelayEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
