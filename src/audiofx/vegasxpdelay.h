// VEGAS's ExpressFX Delay (sfxpfx2_x64.dll, CXpDelay, DirectX class 00000001-0f56-11d2-9887-00a0c969725b): its settings,
// presets and sound. The engine below is the plug-in's own code (its "apply settings" routine and the float sample loop)
// translated line by line, down to how the ring buffer's window moves when the delay time changes in the middle of a
// stream.
#pragma once
#include "vegaspack1.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace reno {
namespace vegpack1 {

// Windows' MulDiv: a * b / c rounded to the nearest, -1 for a zero divisor
inline int32_t xpMulDiv(int32_t a, int32_t b, int32_t c)
{
    if (c == 0) return -1;
    const int64_t p = (int64_t)a * (int64_t)b;
    const int64_t h = c > 0 ? c / 2 : -((int64_t)c / 2);
    return (int32_t)(((p >= 0) == (c > 0) ? p + h : p - h) / c);
}

// The settings structure (3 ints): 0 the mix, delayed sound against the original, in tenths of a percent (0 to 1000),
// 1 delay in tenths of a ms (0 to 10000), 2 decay (feedback) in tenths of a percent (0 to 900).
class XpDelayEngine : public Engine {
public:
    void setup(const int32_t* p, uint32_t rate, bool keep) override
    {
        if (rate < 1000) rate = 1000;
        const int32_t r = (int32_t)rate;
        // the mix: the delayed sound's share and the original's
        wet_ = (double)p[0] / 1000.0;
        dry_ = 1.0 - wet_;
        int32_t len[2];
        len[0] = len[1] = xpMulDiv(p[1], r, 10000);
        decay_ = (double)(uint32_t)p[2] / 1000.0;
        // the buffers (one second and a little more per channel); a new sample rate starts the effect over
        if (!allocated_ || rate_ != rate) {
            rate_ = rate;
            const int32_t n = xpMulDiv(10001, r, 10000);
            for (int c = 0; c < 2; ++c) {
                line_[c].assign((size_t)n, 0.0);
                max_[c] = n - 1;
                begin_[c] = end_[c] = cur_[c] = 0;
                len_[c] = 0;
            }
            allocated_ = true;
        } else if (!keep) {
            for (int c = 0; c < 2; ++c) std::fill(line_[c].begin(), line_[c].end(), 0.0);
        }
        // the delay of each channel: the window of the ring buffer [begin, end] grows at its end or shrinks at its start
        for (int c = 0; c < 2; ++c) {
            if (len[c] <= 1) len[c] = 2;
            const int32_t nw = len[c], old = len_[c];
            if (nw > old) {
                int32_t e = end_[c] - old + nw;
                end_[c] = e;
                if (e > max_[c]) end_[c] = e - max_[c];
            } else if (nw < old) {
                const int32_t b0 = begin_[c];
                int32_t b = b0 - nw + old;
                begin_[c] = b;
                if (b > max_[c]) begin_[c] = b - max_[c];
                if (b0 == begin_[c]) {
                    int32_t at = b0;
                    do {
                        line_[c][(size_t)at] = 0.0;
                        ++at;
                        if (at > max_[c]) at = 0;
                    } while (at == begin_[c]);
                }
            }
            len_[c] = nw;
        }
    }
    void process(float* left, float* right, int frames) override
    {
        if (!allocated_) return;
        float* ch[2] = {left, right};
        for (int c = 0; c < 2; ++c) {
            double* line = line_[c].data();
            int32_t cur = cur_[c];
            const int32_t b = begin_[c], e = end_[c];
            float* buf = ch[c];
            for (int k = 0; k < frames; ++k) {
                const double x = (double)buf[k];
                const double y = line[cur];
                line[cur] = y * decay_;
                line[cur] = x + line[cur];
                if (++cur > e) cur = b;
                const double out = x * dry_ + y * wet_;
                float f = (float)out;
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                if ((bits & 0x7f800000u) < 0x2f800000u) f = 0.f;
                buf[k] = f;
            }
            cur_[c] = cur;
        }
    }

private:
    double wet_ = 0, dry_ = 1, decay_ = 0;
    uint32_t rate_ = 0;
    bool allocated_ = false;
    std::vector<double> line_[2];
    int32_t max_[2] = {0, 0}, begin_[2] = {0, 0}, end_[2] = {0, 0}, cur_[2] = {0, 0}, len_[2] = {0, 0};
};

inline const Info& xpDelayInfo()
{
    static const Info info = [] {
        Info i;
        i.uid = "reno.xpdelay";
        i.name = "ExpressFX Delay";
        i.clsid = "00000001-0f56-11d2-9887-00a0c969725b";
        // the effect's own GUID, written before its settings: 40986928-0f56-11d2-9887-00a0c969725b
        const uint8_t g[16] = {0x28, 0x69, 0x98, 0x40, 0x56, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        std::copy(g, g + 16, i.persistGuid);
        i.nInts = 3;
        i.bypassSlot = -1;
        i.params = {
            {"Delay", 0, 1000, 250, "ms", 1, 10, Num, "", 1, false},
            {"Decay", 0, 90, 0, "%", 2, 10, Num, "", 1, false},
            {"Mix", 0, 100, 50, "%", 0, 10, Num, "", 1, false},
        };
        i.presets = {
            {"(Default)", {500, 2500, 0}},
            {"Default all parameters", {500, 2500, 0}},
            {"1/2 second delay", {500, 5000, 0}},
            {"Phased", {500, 30, 0}},
            {"Robot", {1000, 100, 900}},
            {"Echo chamber", {350, 2500, 500}},
            {"Grand canyon", {250, 10000, 250}},
            {"Dreamscape", {400, 1000, 700}},
        };
        i.make = [] { return std::unique_ptr<Engine>(new XpDelayEngine()); };
        return i;
    }();
    return info;
}

}  // namespace vegpack1
}  // namespace reno
