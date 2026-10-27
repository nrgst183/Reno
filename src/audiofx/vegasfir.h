// The FIR engine of VEGAS's sfppack2 equalizers (Graphic EQ, Parametric EQ, Paragraphic EQ), as the plug-ins' own code
// has it: the effect designs a magnitude response over n bins (n = 256, 1024 or 4096 by the accuracy setting), the
// engine turns it into a one sided, windowed impulse response (veggeq::impulse) and its spectrum, and the sound
// goes through it in blocks of n/2 frames (zero padded, transformed, multiplied, transformed back and added to the tail
// of the last block). The effect only supplies the design: a function that fills the n bins and gives back a gain that
// is applied to the output (1.0 for most). vegasgraphiceq.h has the same engine with its own design in it.
#pragma once
#include "vegasgraphiceq.h"
#include <functional>
#include <vector>

namespace reno {
namespace vegfir {

// Fills m[0..n) with the response of the effect at this rate, returns the gain for the output
using Design = std::function<double(double* m, int n, int rate)>;

class Dsp {
public:
    // The format is set once (the plug-in's set-media-type): rate and channel count (1..2 here; the DLL takes more).
    void setFormat(double rate, int channels)
    {
        rate_ = std::max(8000, (int)(rate + 0.5));
        ch_ = std::max(1, std::min(kMaxCh, channels));
        built_ = false;
        applyChanges();
    }
    // The design in use: 'key' says when it changed (any change redesigns the filter), 'accuracy' is 0..2
    void setDesign(int accuracy, const std::vector<int32_t>& key, const Design& d)
    {
        accuracy = std::max(0, std::min(2, accuracy));
        if (!hasSet_ || accuracy != accuracy_ || key != key_) {
            accuracy_ = accuracy;
            key_ = key;
            design_ = d;
            hasSet_ = true;
            dirty_ = true;
        }
    }
    int block() const { return hop_; }
    int latency() const { return hop_; }

    // The start of a stream, or after a jump in time: nothing of the sound before stays.
    void restart()
    {
        if (!built_) return;
        for (int c = 0; c < kMaxCh; ++c) {
            std::fill(ovl_[c].begin(), ovl_[c].end(), 0.f);
            std::fill(buf_[c].begin(), buf_[c].end(), 0.f);
        }
        fill_ = 0;
    }

    // Takes the settings that were changed since the last call (the plug-in does this at each buffer it gets): the
    // filter is designed again, and the next block that is complete is made with it. A new accuracy starts again.
    void applyChanges()
    {
        if (!hasSet_) return;
        if (built_ && !dirty_) return;
        const int n = veggeq::filterSize(accuracy_);
        const bool resize = !built_ || n != n_;
        n_ = n;
        hop_ = n / 2;
        taps_ = hop_ - 1;
        makeFilter();
        if (resize) {
            for (int c = 0; c < kMaxCh; ++c) {
                buf_[c].assign((size_t)n_, 0.f);
                ovl_[c].assign((size_t)taps_, 0.f);
                out_[c].assign((size_t)hop_, 0.f);
            }
            fill_ = 0;
        }
        built_ = true;
        dirty_ = false;
    }

    // One frame in. Returns true once a block (block() frames) is complete and processed: its samples are then in
    // outAt(c, 0..block()-1); the next push starts the next block.
    bool push(const float* frame)
    {
        if (!built_) return false;
        for (int c = 0; c < ch_; ++c) buf_[c][(size_t)fill_] = frame[c];
        if (++fill_ < hop_) return false;
        processBlock();
        fill_ = 0;
        return true;
    }
    float outAt(int c, int i) const { return out_[c][(size_t)i]; }

private:
    static const int kMaxCh = 2;
    int rate_ = 48000, ch_ = 2;
    int n_ = 0x400, hop_ = 0x200, taps_ = 0x1ff;
    int fill_ = 0;
    int accuracy_ = 1;
    bool built_ = false, dirty_ = false, hasSet_ = false;
    std::vector<int32_t> key_;
    Design design_;
    double gain_ = 1.0;
    std::vector<double> filter_;                 // the filter's spectrum (n_ doubles)
    std::vector<double> m_, x_, tw_;             // design scratch
    std::vector<float> buf_[kMaxCh], ovl_[kMaxCh], out_[kMaxCh];

    void makeFilter()
    {
        m_.assign(2 * (size_t)n_, 0.0);
        gain_ = design_ ? design_(m_.data(), n_, rate_) : 1.0;
        veggeq::impulse(m_.data(), n_, taps_, x_, tw_);
        filter_.assign((size_t)n_, 0.0);
        for (int j = 0; j < taps_; ++j) filter_[(size_t)j] = (double)n_ * m_[(size_t)j];
        vegfft::rfftDfwd(filter_.data(), n_, 1.0);
    }

    // the spectrum times the filter's: bins 0 and n/2 are real, the others complex
    void multiply(float* b) const
    {
        const double* f = filter_.data();
        b[0] = (float)((double)b[0] * f[0]);
        b[1] = (float)((double)b[1] * f[1]);
        for (int k = 1; k < n_ / 2; ++k) {
            const double re = (double)b[2 * k], im = (double)b[2 * k + 1];
            const double fr = f[2 * k], fi = f[2 * k + 1];
            const double nre = re * fr - im * fi;
            const double nim = im * fr + re * fi;
            b[2 * k] = (float)nre;
            b[2 * k + 1] = (float)nim;
        }
    }

    void processBlock()
    {
        for (int c = 0; c < ch_; ++c) {
            float* b = buf_[c].data();
            float* o = ovl_[c].data();
            std::fill(b + hop_, b + n_, 0.f);
            vegfft::rfftF(b, n_ / 2, true, 1.0f);
            multiply(b);
            vegfft::rfftF(b, n_ / 2, false, 1.0f);
            for (int i = 0; i < taps_; ++i) {
                b[i] = b[i] + o[i];
                o[i] = b[hop_ + i];
            }
            for (int i = 0; i < hop_; ++i) out_[c][(size_t)i] = (float)((double)b[i] * gain_);
        }
    }
};

}  // namespace vegfir
}  // namespace reno
