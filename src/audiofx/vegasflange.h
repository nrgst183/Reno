// VEGAS's Flange/Wah-wah DirectX effect (sfppack3_x64.dll, CLSID f09f6980-7845-11d0-aebc-00a0c9053912): its saved settings,
// presets and the sound itself, translated from the plug-in's own code (CSfFlange's engine). Reno's built-in Flange/Wah-wah uses
// the same settings and saved form, so a project's VEGAS Flange/Wah-Wah loads into it and it saves back as VEGAS Flange/Wah-Wah.
//
// Three effects in one plug-in:
//   Flange: a short delay (about a millisecond plus or minus the depth) swept by a sine at the rate, mixed back into the sound;
//   Phaser and Wah-Wah: a two-pole resonator whose centre frequency is swept up and down over a range around the "center
//           frequency" (the range from the depth, the sharpness from the resonance), the phaser mixing its all-pass output with
//           the sound, the wah-wah giving the difference.
// The rate is a frequency or, with tempo sync, a note length.
#pragma once
#include "vegaspack3.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace reno {
namespace vegflange {

const char* const kPersistGuid = "32cf44d6-72e5-4794-a1bc-c8d4a7f51703";
using Tempo = vegpack3::Tempo;

// The plug-in's own layout (what it saves, after its length and a GUID): 0x2c bytes.
struct Settings {
    int32_t effect = 0;                 // 0 Flange, 1 Phaser, 2 Wah-Wah
    int32_t dryOut = -800;              // tenths of a dB, -800 (-Inf.) .. 120
    int32_t wetOut = 0;                 // tenths of a dB, -800 (-Inf.) .. 120
    int32_t rate = 10000;               // Hz times 1000: 0.001 .. 20
    int32_t depth = 10;                 // 1 .. 100 (%)
    int32_t centerFreq = 100;           // Hz, 100 .. 5000 (Phaser and Wah-Wah)
    int32_t resonance = 1;              // 1 .. 100 (%) (Phaser and Wah-Wah)
    int32_t bypass = 0;
    int32_t period = 18;                // tempo sync: note length 0 (64th note triplet) .. 17 (dotted half note), 18 (measures)
    int32_t multiplier = 1000;          // times 1000: 0.001 .. 32
    int32_t tempoSync = 0;
};
static_assert(sizeof(Settings) == 0x2c, "VEGAS Flange/Wah-wah's settings are 0x2c bytes");

inline void sanitize(Settings& s)
{
    s.effect = std::max(0, std::min(2, (int)s.effect));
    s.dryOut = std::max(-800, std::min(120, (int)s.dryOut));
    s.wetOut = std::max(-800, std::min(120, (int)s.wetOut));
    s.rate = std::max(1, std::min(20000, (int)s.rate));
    s.depth = std::max(1, std::min(100, (int)s.depth));
    s.centerFreq = std::max(100, std::min(5000, (int)s.centerFreq));
    s.resonance = std::max(1, std::min(100, (int)s.resonance));
    s.bypass = s.bypass ? 1 : 0;
    s.period = std::max(0, std::min(18, (int)s.period));
    s.multiplier = std::max(1, std::min(32000, (int)s.multiplier));
    s.tempoSync = s.tempoSync ? 1 : 0;
}
inline std::string encode(const Settings& s) { return vegpack3::encode(kPersistGuid, &s, sizeof s); }
inline bool decode(const std::string& st, Settings* out)
{
    Settings s;
    if (!vegpack3::decode(kPersistGuid, st, &s, sizeof s)) return false;
    sanitize(s);
    *out = s;
    return true;
}

struct Preset {
    const char* name;
    Settings s;
};
inline Settings make(int effect, int dry, int wet, int rate, int depth, int center, int resonance)
{
    Settings s;
    s.effect = effect;
    s.dryOut = dry;
    s.wetOut = wet;
    s.rate = rate;
    s.depth = depth;
    s.centerFreq = center;
    s.resonance = resonance;
    return s;
}
// VEGAS's own presets, in its order (the plug-in stores a resonance of 0 for the flange ones, which it does not use: it is 1 here)
inline const std::vector<Preset>& presets()
{
    static const std::vector<Preset> list = {
        {"Default all parameters", make(0, -800, 0, 10000, 10, 100, 1)},
        {"Phaser 1", make(1, -800, 0, 500, 50, 1000, 50)},
        {"Phaser 2", make(1, -800, 0, 4500, 50, 1000, 50)},
        {"Phaser 3", make(1, -800, 0, 10000, 50, 2000, 40)},
        {"Wah-wah 1", make(2, -800, 0, 1000, 50, 1000, 50)},
        {"Wah-wah 2", make(2, -800, 0, 2000, 50, 1000, 80)},
        {"Wah-wah 3", make(2, -150, 0, 200, 50, 500, 100)},
        {"Wah-wah 4", make(2, -800, 0, 500, 70, 3000, 50)},
        {"Wah-wah 5", make(2, -800, 0, 10000, 50, 2000, 50)},
        {"1 Hz flange", make(0, -50, -60, 1000, 85, 100, 1)},
        {"Fast flange", make(0, -60, -60, 5000, 50, 100, 1)},
        {"Slow flange", make(0, -30, -20, 100, 100, 100, 1)},
        {"Slow flange 2", make(0, -25, -25, 500, 85, 100, 1)},
        {"Warble 1", make(0, -50, -50, 20000, 85, 100, 1)},
        {"Warble 2", make(0, -50, -50, 20000, 20, 100, 1)},
    };
    return list;
}

// ---------------------------------------------------------------------------------------------------------------
// The sound. The plug-in's engine keeps, for the flange, a delay line of 6500 frames and a sine oscillator (the recurrence
// y(n) = 2 cos(w) y(n-1) - y(n-2)); for the phaser and wah-wah two tables of up to 2000 coefficients, the place in them and its
// step (it goes up and down), and per channel the last two outputs of the resonator.
class Engine {
public:
    // The settings the plug-in is going to use from its next setup on.
    void setSettings(const Settings& s)
    {
        cur_ = s;
        changed_ = 1;
    }
    // The format: sample rate (the sound is stereo float). Sets the engine up from the settings with no tempo and starts afresh.
    void setFormat(int rate)
    {
        rate_ = (unsigned)rate;
        setup(cur_, nullptr, false);
        changed_ = 0;
        needTempo_ = true;
    }

    // One call of the plug-in's own process function on 'frames' interleaved stereo frames, in place. 'tempo' is what the
    // project says (used with tempo sync). end: the end of the stream (nothing is processed). Returns 1 at the end, else 0.
    int process(float* data, int* frames, const Tempo* tempo, bool end = false)
    {
        if (end) {
            *frames = 0;
            return 1;
        }
        if (changed_ || needTempo_) {
            setup(cur_, tempo, true);
            changed_ = 0;
            needTempo_ = false;
        }
        run(data, *frames);
        return 0;
    }

private:
    static const int kDelay = 6500;     // frames in the flange's delay line
    static const int kTable = 2000;     // coefficients in the tables
    static const int kChannels = 2;

    // the sine oscillator of the plug-in (CSineOscillator)
    struct Osc {
        double omega = 0, phase = 0, coef = 0, y = 0, y1 = 0, y2 = 0;
        int n = 0, count = 0, limit = 0x800;
        void set(double w, double p, int at)
        {
            omega = w;
            phase = p;
            restart(at);
        }
        void restart(int at)
        {
            coef = std::cos(omega) * 2.0;
            n = at;
            count = 0;
            y2 = std::sin((double)(at - 2) * omega + phase);
            y1 = std::sin((double)(at - 1) * omega + phase);
            y = 0;
        }
        double nth(int at)
        {
            if (count > limit || at != n) restart(at);
            const double a = y1;
            ++n;
            ++count;
            const double r = a * coef - y2;
            y2 = a;
            y = r;
            y1 = r;
            return r;
        }
    };

    double wet_ = 0, dry_ = 0;
    int effect_ = 0;
    std::vector<double> delay_ = std::vector<double>((size_t)kDelay * kChannels, 0.0);
    int wpos_ = 0;
    double state_[kChannels] = {0, 0};
    double gain_[kChannels] = {0, 0};
    int n_ = 0;
    double center_ = 48, period_ = 0, step_ = 0, depth_ = 0, unit_ = 48;
    Osc osc_;
    // phaser / wah-wah
    double tbl1_[kTable] = {}, tbl2_[kTable] = {};
    int entries_ = 0;
    double pos_ = 0, move_ = 0;
    double s1_[kChannels] = {0, 0}, s2_[kChannels] = {0, 0};
    int changed_ = 0, needTempo_ = 0;
    unsigned rate_ = 48000;
    Settings cur_;

    static double constant(uint64_t bits)
    {
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }

    // The plug-in's setup; keep: carry on, else start afresh (the delay line, the oscillator and the states are cleared).
    void setup(const Settings& s, const Tempo* tempo, bool keep)
    {
        effect_ = s.effect;
        wet_ = s.wetOut > -800 ? std::pow(10.0, (double)s.wetOut / 200.0) : 0.0;
        dry_ = s.dryOut > -800 ? std::pow(10.0, (double)s.dryOut / 200.0) : 0.0;
        if (!keep) reset();
        if (effect_ == 1 || effect_ == 2) setupResonator(s, tempo);
        else setupFlange(s, tempo);
    }

    void reset()
    {
        std::fill(delay_.begin(), delay_.end(), 0.0);
        wpos_ = 0;
        for (int c = 0; c < kChannels; ++c) state_[c] = 0;
        n_ = 0;
        unit_ = (double)vegpack3::cvtt((double)rate_ * 10.0 / 10000.0);
        center_ = unit_;
        osc_ = Osc();
        osc_.set(0.0, 0.0, 0);
        std::fill(tbl1_, tbl1_ + kTable, 0.0);
        std::fill(tbl2_, tbl2_ + kTable, 0.0);
        for (int c = 0; c < kChannels; ++c) s1_[c] = s2_[c] = 0;
        pos_ = 0;
    }

    void setupFlange(const Settings& s, const Tempo* tempo)
    {
        static const double kGain[16] = {0.9, 0.95, 0.7, 0.9, 0.8, 1.0, 1.0, 0.7, 0.7, 1.0, 0.8, 1.0, 1.0, 0.8, 0.9, 0.7};
        for (int c = 0; c < kChannels; ++c) gain_[c] = kGain[c & 15] * wet_;
        depth_ = (double)vegpack3::cvtt((double)s.depth / 100.0 * unit_);
        double period;
        if (s.tempoSync != 0 && tempo != nullptr) {
            period = vegpack3::syncCycle(*tempo, s.period, s.multiplier, rate_);
        } else {
            period = (double)vegpack3::cvtt(1000.0 / (double)s.rate * (double)rate_);
        }
        period_ = period;
        step_ = constant(0x401921fb54442eeaull) / period;      // 6.28318530718
        wet_ = wet_ * 0.9;
        osc_.set(step_, 0.0, n_);
    }

    void setupResonator(const Settings& s, const Tempo* tempo)
    {
        int half;
        if (s.tempoSync != 0 && tempo != nullptr) {
            double len = vegpack3::syncCycle(*tempo, s.period, s.multiplier, rate_);
            len = len * 0.5;
            len = len + 0.5;
            half = vegpack3::cvtt(len);
            // (the plug-in leaves the number of table entries as it was)
        } else {
            const uint32_t twice = (uint32_t)s.rate * 2u;
            const uint32_t h = (uint32_t)((uint32_t)rate_ * 1000u) / twice;
            half = (int)h;
            entries_ = half > kTable ? kTable : half;
        }
        const int entries = entries_;
        if (0.0 > move_) move_ = -((double)entries / (double)half);
        else move_ = (double)entries / (double)half;
        // the sharpness: 1 + 0.008 (resonance - 50)^2, upside down below 50
        double q;
        {
            const double d = (double)(s.resonance - 50);
            q = d * d * 0.008 + 1.0;
            if (s.resonance < 50) q = 1.0 / q;
        }
        // the range: 1 + 0.002 (depth - 50)^2, upside down above 50
        double r;
        {
            const double d = (double)(s.depth - 50);
            r = d * d * 0.002 + 1.0;
            if (!((double)s.depth <= 50.0)) r = 1.0 / r;
        }
        const double f = (double)s.centerFreq / (double)rate_;
        double lo, stride = 0.0;
        if (r == 0.0) {
            lo = f;
        } else {
            const double ratio = std::pow(2.0, 1.0 / r);
            lo = f / ratio;
            double hi = ratio * f;
            if (lo > hi) hi = lo;
            else if (hi > 0.5) hi = 0.5;
            stride = (hi - lo) / (double)entries;
        }
        const double mPi = constant(0xc00921fb54432708ull);        // -3.14159265356
        const double twoPi = constant(0x401921fb54442eeaull);      // 6.28318530718
        const double mTwoPi = constant(0xc01921fb54442eeaull);
        for (int k = 0; k < entries; ++k) {
            const double e1 = std::exp(lo * mPi / q);
            const double c = std::cos(lo * twoPi);
            tbl1_[k] = e1 * (c * 2.0);
            tbl2_[k] = std::exp(lo * mTwoPi / q);
            lo += stride;
        }
    }

    // ---- the flange, one frame
    void flangeFrame(double* io)
    {
        const int ch = kChannels;
        const double o = osc_.nth(n_);
        double pos = o * depth_ + center_;
        if (pos >= 6500.0) pos = pos + (0.0 - 6500.0);
        else if (0.0 > pos) pos = pos + 6500.0;
        const int whole = vegpack3::cvtt(pos);
        const double frac = pos - (double)whole;
        int rd0 = (int)((uint32_t)whole * (uint32_t)ch);
        if (rd0 < 0) rd0 = 0;
        else {
            const int last = ch * kDelay - 1;
            if (rd0 > last) rd0 = last;
        }
        const int size = ch * kDelay;
        int rd1 = rd0 + ch;
        if (rd1 >= size) rd1 -= size;
        center_ = center_ - 1.0;
        if (0.0 > center_) center_ = constant(0x40b9630000000000ull);      // 6498
        ++n_;
        if ((double)n_ >= period_) n_ = 0;
        int ea = rd0, eb = rd1;
        for (int c = 0; c < ch; ++c) {
            const double fb = state_[c] * 0.0;
            double y = (delay_[(size_t)eb] - delay_[(size_t)ea]) * frac;
            y = y + delay_[(size_t)ea];
            y = y * 1.0 + fb;
            state_[c] = y;
            if (2e-11 > std::fabs(y)) state_[c] = 0.0;
            const double x = io[c];
            double out = y * wet_;
            const double dryPart = x * dry_;
            out = out * gain_[c];
            io[c] = out + dryPart;
            double w = y * 0.5;
            w = w + x;
            if (2e-11 > std::fabs(w)) w = 0.0;
            int wi = wpos_ * ch + c;
            if (wi >= size) wi -= size;
            delay_[(size_t)wi] = w;
            ++ea;
            if (ea >= size) ea -= size;
            ++eb;
            if (eb >= size) eb -= size;
        }
        --wpos_;
        if (wpos_ < 0) wpos_ = kDelay - 1;
    }

    // ---- the phaser and the wah-wah, one sample of one channel
    double resonatorSample(double x, int ch)
    {
        int i = vegpack3::cvtt(pos_);
        const int last = entries_ - 1;
        if (i < 0) i = 0;
        else i = std::min(i, last);
        int j = i + 1;
        if (j < 0) j = 0;
        else j = std::min(j, last);
        i = std::max(0, i);
        j = std::max(0, j);
        const double fi = (double)i;
        double b1, c2;
        if (pos_ == fi) {
            b1 = tbl1_[i];
            c2 = tbl2_[i];
        } else {
            const double fr = pos_ - fi;
            b1 = (tbl1_[j] - tbl1_[i]) * fr + tbl1_[i];
            c2 = (tbl2_[j] - tbl2_[i]) * fr + tbl2_[i];
        }
        const double before = pos_;
        const double nextPos = move_ + before;
        pos_ = nextPos;
        const double top = (double)last;
        if (nextPos >= top) {
            pos_ = top;
            move_ = move_ * -1.0;
        } else if (nextPos <= 0.0) {
            pos_ = 0.0;
            move_ = move_ * -1.0;
        }
        const double old2 = s2_[ch];
        const double old1 = s1_[ch];
        s2_[ch] = old1;
        const double t1 = old1 * b1;
        const double t2 = old2 * c2;
        double y = t1 + x;
        y = y - t2;
        double v = y * c2;
        v = v - t1;
        v = v + old2;
        if (2e-11 > std::fabs(y)) y = 0.0;
        s1_[ch] = y;
        double ap;
        if (effect_ == 1) {
            v = v + x;
            ap = v * 0.5;
        } else {
            ap = x - v;
        }
        const double dryPart = x * dry_;
        ap = ap * wet_;
        return dryPart + ap;
    }

    void run(float* data, int frames)
    {
        if (effect_ == 0) {
            for (int k = 0; k < frames; ++k) {
                double d[kChannels];
                for (int c = 0; c < kChannels; ++c) d[c] = (double)data[(size_t)k * kChannels + (size_t)c];
                flangeFrame(d);
                for (int c = 0; c < kChannels; ++c) data[(size_t)k * kChannels + (size_t)c] = (float)d[c];
            }
        } else if (effect_ == 1 || effect_ == 2) {
            // each channel is run through the table from the same place; what is left is where the last one ended
            const double p = pos_, m = move_;
            for (int c = 0; c < kChannels; ++c) {
                pos_ = p;
                move_ = m;
                for (int k = 0; k < frames; ++k) {
                    float* f = data + (size_t)k * kChannels + (size_t)c;
                    *f = (float)resonatorSample((double)*f, c);
                }
            }
        }
    }
};

}  // namespace vegflange
}  // namespace reno
