// Reno's eFX Reverb, Tube Stage, Phaser and Chorus/Flanger (see efx.h), written from MAGIX's manual and the
// behaviour of the presets, with the real ones' 31 settings.
#include "efx.h"
#include <cstring>

namespace reno {
namespace efx {
namespace {


// Deterministic noise for the slow drifts (same on every run, so renders repeat).
struct Noise {
    uint32_t s = 12345u;
    float next() { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 32768.f - 1.f; }
};

// A Schroeder all-pass: passes every frequency at the same level and smears the sound in time (diffusion).
struct AllPass {
    Delay d;
    int len = 1;
    void resize(int maxLen) { d.resize((size_t)maxLen + 4); }
    float process(float x, float g)
    {
        const float old = d.tap((double)len);
        const float v = x - g * old;
        d.push(v);
        return old + g * v;
    }
};

// ======================================================================================================== Reverb

// An algorithmic stereo reverb: pre-delay and a low cut, a chain of all-pass diffusers (Diffusion) and a feedback
// delay network of eight lines mixed by a Hadamard matrix. Each line's feedback is set so the tail falls 60 dB in
// Time seconds, a low-pass in every line makes the highs die sooner (Damping), and the read points drift slowly
// (Modulation). Left feeds and is heard from half the lines, right from the other half, which keeps the stereo
// image. The four modes differ in line lengths (Size scales them) and diffusion density.
class Reverb : public EfxPlugin {
public:
    Reverb() : EfxPlugin("eFX Reverb", 0x4D583233u, reverbDefs()) {}

protected:
    void prepare() override
    {
        const size_t maxLine = (size_t)(0.45 * sr_) + 16;
        for (auto& l : line_) l.resize(maxLine);
        const size_t maxPre = (size_t)(0.25 * sr_) + 16;
        pre_[0].resize(maxPre); pre_[1].resize(maxPre);
        for (int c = 0; c < 2; ++c)
            for (auto& a : ap_[c]) a.resize((int)(0.06 * sr_));
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        mix_.setTime(sr_, 5);
        lowCutF_ = -1;
        mode_ = -1;
    }
    void reset() override
    {
        for (auto& l : line_) l.clear();
        pre_[0].clear(); pre_[1].clear();
        for (int c = 0; c < 2; ++c) {
            for (auto& a : ap_[c]) a.d.clear();
            hp_[c].reset();
        }
        for (int i = 0; i < 8; ++i) lp_[i] = 0;
        gIn_.primed = gOut_.primed = mix_.primed = false;
        lfoPhase_ = 0;
    }

    void run(float* l, float* r, int n) override
    {
        const int mode = choice(1);
        const double size = real(5) / 100.0;
        const double rt60 = real(6);
        const double damping = real(7) / 100.0;
        const double modAmt = real(8) / 100.0;
        const double diffusion = real(21) / 100.0;
        const double preMs = real(20);
        const double lowCut = real(22);
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        mix_.set(real(9) / 100.0);

        // line lengths for this mode and size
        static const double kBase[4][8] = {
            {19.3, 23.7, 29.1, 31.9, 37.3, 41.1, 43.7, 47.9},      // Plate
            {11.3, 13.7, 17.1, 19.3, 23.9, 27.1, 29.9, 33.1},      // Room
            {31.7, 37.1, 41.3, 47.9, 53.9, 61.1, 67.3, 73.7},      // Hall
            {23.9, 29.3, 34.1, 38.3, 44.9, 49.7, 55.1, 59.9}};     // Plate II Classic
        static const double kScaleLo[4] = {0.5, 0.4, 0.5, 0.5}, kScaleHi[4] = {1.5, 1.6, 2.0, 1.5};
        const int m = std::max(0, std::min(3, mode));
        const double scale = kScaleLo[m] + (kScaleHi[m] - kScaleLo[m]) * size;
        double len[8];
        for (int i = 0; i < 8; ++i) len[i] = std::max(8.0, kBase[m][i] * 0.001 * sr_ * scale);
        float g[8];
        for (int i = 0; i < 8; ++i) g[i] = (float)std::pow(10.0, -3.0 * len[i] / (std::max(0.05, rt60) * sr_));
        // damping: cutoff of each line's low-pass
        const double fc = 1000.0 + 17000.0 * (1.0 - damping) * (1.0 - damping);
        const float lpA = (float)std::exp(-2.0 * kPi * fc / sr_);
        // diffusion: number of all-passes, their length and gain
        static const int kApCount[4] = {4, 3, 3, 4};
        static const double kApMs[4][4] = {{4.1, 5.9, 8.3, 11.1}, {3.1, 4.7, 6.7, 0}, {5.3, 7.9, 10.9, 0}, {6.1, 9.3, 12.7, 17.9}};
        static const double kApGain[4] = {0.75, 0.7, 0.6, 0.78};
        const int nap = kApCount[m];
        const float apG = (float)(kApGain[m] * diffusion);
        for (int c = 0; c < 2; ++c)
            for (int a = 0; a < 4; ++a) ap_[c][a].len = std::max(1, (int)std::lround(kApMs[m][a] * 0.001 * sr_ * (0.7 + 0.6 * size) * (c ? 1.07 : 1.0)));
        if (lowCut != lowCutF_) {
            lowCutF_ = lowCut;
            for (int c = 0; c < 2; ++c) hp_[c].highpass(sr_, lowCut);
        }
        const double preSamples = std::max(1.0, preMs * 0.001 * sr_);
        const double modDepth = modAmt * 0.0005 * sr_;       // up to 0.5 ms of drift
        const double lfoInc = 2.0 * kPi * 0.35 / sr_;

        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            const float inL = l[k] * gi, inR = r[k] * gi;
            // pre-delay, low cut, diffusion
            pre_[0].push(inL); pre_[1].push(inR);
            float xl = pre_[0].tap(preSamples), xr = pre_[1].tap(preSamples);
            xl = hp_[0].process(xl); xr = hp_[1].process(xr);
            for (int a = 0; a < nap; ++a) { xl = ap_[0][a].process(xl, apG); xr = ap_[1][a].process(xr, apG); }
            // the lines
            lfoPhase_ += lfoInc;
            if (lfoPhase_ > 2 * kPi) lfoPhase_ -= 2 * kPi;
            float y[8];
            for (int i = 0; i < 8; ++i) {
                const double mod = modDepth * std::sin(lfoPhase_ * (0.6 + 0.13 * i) + 1.7 * i);
                float v = line_[i].tap(len[i] + mod);
                lp_[i] = v * (1.f - lpA) + lp_[i] * lpA;
                y[i] = lp_[i] * g[i];
            }
            // Hadamard mixing (8x8), scaled to keep the energy
            float a0 = y[0] + y[1], a1 = y[0] - y[1], a2 = y[2] + y[3], a3 = y[2] - y[3];
            float a4 = y[4] + y[5], a5 = y[4] - y[5], a6 = y[6] + y[7], a7 = y[6] - y[7];
            float b0 = a0 + a2, b1 = a1 + a3, b2 = a0 - a2, b3 = a1 - a3;
            float b4 = a4 + a6, b5 = a5 + a7, b6 = a4 - a6, b7 = a5 - a7;
            const float s = 0.35355339f;
            float f[8] = {(b0 + b4) * s, (b1 + b5) * s, (b2 + b6) * s, (b3 + b7) * s, (b0 - b4) * s, (b1 - b5) * s, (b2 - b6) * s, (b3 - b7) * s};
            // input: left into the even lines, right into the odd ones
            static const float kSign[8] = {1, 1, -1, 1, 1, -1, -1, 1};
            for (int i = 0; i < 8; ++i) {
                const float in = ((i & 1) ? xr : xl) * 0.5f * kSign[i];
                line_[i].push(f[i] + in);
            }
            float wl = 0, wr = 0;
            for (int i = 0; i < 8; ++i) {
                if (i & 1) wr += y[i] * kSign[i];
                else wl += y[i] * kSign[i];
            }
            wl *= 0.5f; wr *= 0.5f;
            const float mx = (float)mix_.next(), go = (float)gOut_.next();
            l[k] = (inL * (1.f - mx) + wl * mx) * go;
            r[k] = (inR * (1.f - mx) + wr * mx) * go;
        }
        for (int i = 0; i < 8; ++i) lp_[i] = flushDenormal(lp_[i]);
    }

private:
    Delay line_[8];
    Delay pre_[2];
    AllPass ap_[2][4];
    Biquad hp_[2];
    float lp_[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    double lowCutF_ = -1, lfoPhase_ = 0;
    int mode_ = -1;
    Smooth gIn_, gOut_, mix_;
};

// ===================================================================================================== Tube Stage

// A half-band filter pair for doubling and halving the sample rate (31-tap windowed sinc; every other tap is zero).
struct HalfBand {
    static constexpr int kTaps = 31;
    float h[kTaps];
    HalfBand()
    {
        const int c = kTaps / 2;
        for (int i = 0; i < kTaps; ++i) {
            const int m = i - c;
            double v;
            if (m == 0) v = 0.5;
            else if (m % 2 == 0) v = 0.0;
            else v = std::sin(kPi * m / 2.0) / (kPi * m);
            const double w = 0.42 - 0.5 * std::cos(2 * kPi * i / (kTaps - 1)) + 0.08 * std::cos(4 * kPi * i / (kTaps - 1));
            h[i] = (float)(v * w);
        }
    }
};
const HalfBand& halfBand() { static const HalfBand h; return h; }

struct Up2 {
    float hist[HalfBand::kTaps] = {0};
    int pos = 0;
    void reset() { std::fill(hist, hist + HalfBand::kTaps, 0.f); pos = 0; }
    // one sample in, two out (the zero-stuffed sample filtered)
    void process(float x, float* out)
    {
        const HalfBand& hb = halfBand();
        for (int ph = 0; ph < 2; ++ph) {
            hist[pos] = ph == 0 ? x : 0.f;
            float acc = 0;
            int idx = pos;
            for (int i = 0; i < HalfBand::kTaps; ++i) {
                acc += hb.h[i] * hist[idx];
                if (--idx < 0) idx = HalfBand::kTaps - 1;
            }
            out[ph] = acc * 2.f;
            if (++pos >= HalfBand::kTaps) pos = 0;
        }
    }
};
struct Down2 {
    float hist[HalfBand::kTaps] = {0};
    int pos = 0;
    void reset() { std::fill(hist, hist + HalfBand::kTaps, 0.f); pos = 0; }
    // two in, one out
    float process(float a, float b)
    {
        const HalfBand& hb = halfBand();
        float out = 0;
        for (int ph = 0; ph < 2; ++ph) {
            hist[pos] = ph == 0 ? a : b;
            if (ph == 1) {
                float acc = 0;
                int idx = pos;
                for (int i = 0; i < HalfBand::kTaps; ++i) {
                    acc += hb.h[i] * hist[idx];
                    if (--idx < 0) idx = HalfBand::kTaps - 1;
                }
                out = acc;
            }
            if (++pos >= HalfBand::kTaps) pos = 0;
        }
        return out;
    }
};

// One or two tube gain stages between two soft tilt filters. Class A works the tube off-centre, so the two halves
// of the wave are treated differently (even and odd harmonics, the warm sound); class AB is symmetrical (odd
// harmonics only). With two stages the gain is split equally. Oversampling runs the stages at four times the rate
// so new harmonics do not fold back into the audible range.
class TubeStage : public EfxPlugin {
public:
    TubeStage() : EfxPlugin("eFX TubeStage", 0x4D583242u, tubeStageDefs()) {}
    // the half-band filters of the oversampling delay the sound (about 22 samples)
    int latencySamples() override { return on(10) ? 22 : 0; }

protected:
    void prepare() override
    {
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        eqKey_ = 1e9;
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) {
            preLo_[c].reset(); preHi_[c].reset(); postLo_[c].reset(); postHi_[c].reset();
            up1_[c].reset(); up2a_[c].reset(); up2b_[c].reset(); dn1_[c].reset(); dn2a_[c].reset(); dn2b_[c].reset();
            dcX_[c] = dcY_[c] = 0;
        }
        gIn_.primed = gOut_.primed = false;
    }

    void run(float* l, float* r, int n) override
    {
        const double pre = real(5) / 100.0, post = real(7) / 100.0;
        const double gainDb = real(6);
        const double second = std::max(0.0, std::min(1.0, real(15) / 100.0 - 1.0));      // Stages 100%..200%: share of a second stage
        const double classAB = std::max(0.0, std::min(1.0, real(16) / 100.0));           // Class A (0%) .. AB (100%)
        const bool over = on(10);
        const double bias = real(20) / 10.0;
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        // tilt filters (a low shelf and a high shelf against each other, about 3 dB each way at the extremes)
        const double key = pre * 1000.0 + post;
        if (key != eqKey_) {
            eqKey_ = key;
            for (int c = 0; c < 2; ++c) {
                preLo_[c].lowShelf(sr_, 900, -pre * 3.0);  preHi_[c].highShelf(sr_, 900, pre * 3.0);
                postLo_[c].lowShelf(sr_, 900, -post * 3.0); postHi_[c].highShelf(sr_, 900, post * 3.0);
            }
        }
        const float drive = (float)std::pow(10.0, gainDb / (1.0 + second) / 20.0);
        const float b = (float)((1.0 - classAB) * 0.45 + 0.4 * bias * (1.0 - 0.6 * classAB));
        const float bt = std::tanh(b);
        float* io[2] = {l, r};
        for (int c = 0; c < 2; ++c) {
            float* x = io[c];
            Smooth gi = gIn_, go = gOut_;                   // each channel runs the same smoothing from the same start
            for (int k = 0; k < n; ++k) {
                float v = x[k] * (float)gi.next();
                v = preHi_[c].process(preLo_[c].process(v));
                if (over) {
                    float u2[2], u4[4];
                    up1_[c].process(v, u2);
                    up2a_[c].process(u2[0], u4);
                    up2b_[c].process(u2[1], u4 + 2);
                    for (int i = 0; i < 4; ++i) u4[i] = stage(u4[i], drive, (float)second, b, bt);
                    const float d0 = dn2a_[c].process(u4[0], u4[1]);
                    const float d1 = dn2b_[c].process(u4[2], u4[3]);
                    v = dn1_[c].process(d0, d1);
                } else {
                    v = stage(v, drive, (float)second, b, bt);
                }
                // removes the offset a biased tube leaves (15 Hz high-pass)
                const float y = v - dcX_[c] + 0.9980f * dcY_[c];
                dcX_[c] = v; dcY_[c] = flushDenormal(y);
                v = postHi_[c].process(postLo_[c].process(y));
                x[k] = v * (float)go.next();
            }
            if (c == 1) { gIn_ = gi; gOut_ = go; }
        }
    }

private:
    // one or two stages of the tube curve (a second stage flips the wave over, as a real one does)
    static float stage(float x, float drive, float second, float b, float bt)
    {
        const float unity = 1.f / (1.f - bt * bt);        // slope at the working point: a quiet sound passes at its own level
        float v = (std::tanh(drive * x + b) - bt) * unity;
        if (second > 0.f) {
            const float v2 = -(std::tanh(drive * v + b) - bt) * unity;
            v += second * (v2 - v);
        }
        return v;
    }

    Smooth gIn_, gOut_;
    Biquad preLo_[2], preHi_[2], postLo_[2], postHi_[2];
    Up2 up1_[2], up2a_[2], up2b_[2];
    Down2 dn1_[2], dn2a_[2], dn2b_[2];
    float dcX_[2] = {0, 0}, dcY_[2] = {0, 0};
    double eqKey_ = 1e9;
};

// ======================================================================================================== Phaser

// A phaser: 4, 8 or 16 first-order all-pass stages whose corner frequency sweeps around the centre frequency with a
// sine LFO (the right channel's LFO offset by Phase Offset), part of the last stage fed back to the first, mixed with
// the dry sound: where the stages turn the phase half way round, the mix cancels (the notches).
class Phaser : public EfxPlugin {
public:
    Phaser() : EfxPlugin("eFX Phaser", 0x4D583232u, phaserDefs()) {}

protected:
    void prepare() override { gIn_.setTime(sr_, 5); gOut_.setTime(sr_, 5); mix_.setTime(sr_, 5); }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { std::fill(z_[c], z_[c] + 16, 0.f); fb_[c] = 0; }
        phase_ = 0;
        gIn_.primed = gOut_.primed = mix_.primed = false;
    }

    void run(float* l, float* r, int n) override
    {
        const int stages = choice(1) == 0 ? 4 : (choice(1) == 1 ? 8 : 16);
        const double rate = real(5);
        const double depth = real(6) / 100.0;
        const double offset = real(7) * kPi / 180.0;
        const double fbk = real(8) / 100.0 * 0.92;
        const double fc = real(20);
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        mix_.set(real(9) / 100.0);
        const double inc = 2.0 * kPi * rate / sr_;
        float* io[2] = {l, r};
        const double ph0 = phase_;
        for (int c = 0; c < 2; ++c) {
            double ph = ph0 + (c ? offset : 0.0);
            float* x = io[c];
            Smooth gi = gIn_, go = gOut_, mx = mix_;
            for (int k = 0; k < n; ++k) {
                // corner frequency, swept by up to +-2.5 octaves
                const double f = std::max(20.0, std::min(sr_ * 0.42, fc * std::pow(2.0, 2.5 * depth * std::sin(ph))));
                const double t = std::tan(kPi * f / sr_);
                const float a = (float)((t - 1.0) / (t + 1.0));
                ph += inc;
                const float in = x[k] * (float)gi.next();
                float v = in + (float)fbk * fb_[c];
                for (int s = 0; s < stages; ++s) {
                    const float y = a * v + z_[c][s];
                    z_[c][s] = v - a * y;
                    v = y;
                }
                fb_[c] = flushDenormal(v);
                const float m = (float)mx.next();
                x[k] = (in * (1.f - m) + v * m) * (float)go.next();
            }
            if (c == 1) { gIn_ = gi; gOut_ = go; mix_ = mx; }
        }
        phase_ = std::fmod(ph0 + inc * n, 2.0 * kPi);
    }

private:
    float z_[2][16];
    float fb_[2] = {0, 0};
    double phase_ = 0;
    Smooth gIn_, gOut_, mix_;
};

// ============================================================================================== Chorus / Flanger

// Chorus, flanger and ensemble from modulated delays. Chorus: a copy of each channel delayed about 12 ms, swept by
// the LFO (the right channel's LFO offset by Phase). Flanger: a delay of a few ms swept the same way, with feedback
// (positive or negative; the middle of the Feedback fader is none). Ensemble: three such voices per channel, LFOs
// 120 degrees apart, from two LFOs. A high-pass (Low Cut) keeps bass out of the copies.
class ChorusFlanger : public EfxPlugin {
public:
    ChorusFlanger() : EfxPlugin("eFX ChorusFlanger", 0x4D583231u, chorusFlangerDefs()) {}

protected:
    void prepare() override
    {
        for (int c = 0; c < 2; ++c) line_[c].resize((size_t)(0.06 * sr_) + 16);
        gIn_.setTime(sr_, 5); gOut_.setTime(sr_, 5); mix_.setTime(sr_, 5);
        lowCutF_ = -1;
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { line_[c].clear(); hp_[c].reset(); fb_[c] = 0; }
        phase_ = 0; phaseB_ = 0;
        gIn_.primed = gOut_.primed = mix_.primed = false;
    }

    void run(float* l, float* r, int n) override
    {
        const int mode = choice(1);
        const double rate = real(5);
        const double depth = real(6) / 100.0;
        const double offset = real(7) * kPi / 180.0;
        const double fbk = std::max(-0.95, std::min(0.95, real(8) / 100.0));
        const double lowCut = real(20);
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        mix_.set(real(9) / 100.0);
        if (lowCut != lowCutF_) {
            lowCutF_ = lowCut;
            for (int c = 0; c < 2; ++c) hp_[c].highpass(sr_, std::max(20.0, lowCut));
        }
        const double inc = 2.0 * kPi * rate / sr_, incB = inc * 2.37;
        // delay (ms) it swings around, and how far
        double baseMs, spanMs;
        if (mode == 1) { baseMs = 3.0; spanMs = 2.6 * depth; }
        else if (mode == 2) { baseMs = 14.0; spanMs = 6.0 * depth; }
        else { baseMs = 12.0; spanMs = 5.0 * depth; }
        const int voices = mode == 2 ? 3 : 1;
        float* io[2] = {l, r};
        const double ph0 = phase_, phB0 = phaseB_;
        for (int c = 0; c < 2; ++c) {
            float* x = io[c];
            double ph = ph0 + (c ? offset : 0.0), phB = phB0 + (c ? offset : 0.0);
            Smooth gi = gIn_, go = gOut_, mx = mix_;
            for (int k = 0; k < n; ++k) {
                const float in = x[k] * (float)gi.next();
                float wet = 0;
                for (int v = 0; v < voices; ++v) {
                    double lfo;
                    if (mode == 2) lfo = 0.7 * std::sin(ph + v * 2.0943951) + 0.3 * std::sin(phB + v * 2.0943951);
                    else lfo = std::sin(ph);
                    const double dms = baseMs + spanMs * lfo;
                    wet += line_[c].tap(std::max(1.0, dms * 0.001 * sr_));
                }
                wet /= (float)voices;
                wet = hp_[c].process(wet);
                line_[c].push(in + (float)fbk * fb_[c]);
                fb_[c] = flushDenormal(wet);
                ph += inc; phB += incB;
                const float m = (float)mx.next();
                x[k] = (in * (1.f - m) + wet * m) * (float)go.next();
            }
            if (c == 1) { gIn_ = gi; gOut_ = go; mix_ = mx; }
        }
        phase_ = std::fmod(ph0 + inc * n, 2.0 * kPi);
        phaseB_ = std::fmod(phB0 + incB * n, 2.0 * kPi);
    }

private:
    Delay line_[2];
    Biquad hp_[2];
    float fb_[2] = {0, 0};
    double phase_ = 0, phaseB_ = 0, lowCutF_ = -1;
    Smooth gIn_, gOut_, mix_;
};

}  // namespace

std::unique_ptr<EfxPlugin> makeReverb() { return std::unique_ptr<EfxPlugin>(new Reverb()); }
std::unique_ptr<EfxPlugin> makeTubeStage() { return std::unique_ptr<EfxPlugin>(new TubeStage()); }
std::unique_ptr<EfxPlugin> makePhaser() { return std::unique_ptr<EfxPlugin>(new Phaser()); }
std::unique_ptr<EfxPlugin> makeChorusFlanger() { return std::unique_ptr<EfxPlugin>(new ChorusFlanger()); }

}  // namespace efx
}  // namespace reno
