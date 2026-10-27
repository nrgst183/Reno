// Reno's eFX De-Esser, Gate, Stereo Delay, Tremolo/Pan and Vocal Strip (see efx.h), written from MAGIX's manual and
// the behaviour of the presets, with the real ones' 31 settings. Tempo-synced settings follow the project's tempo
// (projectTempo(), set by the mixer; 120 BPM outside a mix).
#include "efx.h"
#include <cstring>

namespace reno {
namespace efx {
namespace {

// the project's tempo; not below 40 BPM, which is what the delay lines are made long enough for
double tempoBpm() { return std::max(40.0, projectTempo().bpm); }

// A level follower with its own attack and release (one-pole, on a rectified value).
struct Follower {
    double v = 0, up = 0.1, down = 0.001;
    void set(double fs, double attackMs, double releaseMs)
    {
        up = 1.0 - std::exp(-1.0 / (std::max(0.01, attackMs) * 0.001 * fs));
        down = 1.0 - std::exp(-1.0 / (std::max(0.01, releaseMs) * 0.001 * fs));
    }
    inline double next(double x)
    {
        v += (x - v) * (x > v ? up : down);
        return v;
    }
};

// A per-channel delay of a fixed number of samples.
struct Hold2 {
    std::vector<float> b[2];
    size_t pos = 0, n = 1;
    void size(size_t samples)
    {
        n = std::max<size_t>(1, samples);
        b[0].assign(n, 0.f);
        b[1].assign(n, 0.f);
        pos = 0;
    }
    void clear() { for (auto& v : b) std::fill(v.begin(), v.end(), 0.f); pos = 0; }
    // pushes (l, r), returns the pair that went in 'n' samples ago
    inline void step(float& l, float& r)
    {
        const float ol = b[0][pos], orr = b[1][pos];
        b[0][pos] = l;
        b[1][pos] = r;
        if (++pos >= n) pos = 0;
        l = ol;
        r = orr;
    }
};

// ======================================================================================================= De-Esser
// A dynamic notch on sibilants: a band-pass at the chosen frequency listens, and where the band is well above its own
// average level it is turned down (by up to Reduction) in the signal path. There is no threshold: it is the band
// against its average, so quiet and loud voices are treated alike.
class DeEsser : public EfxPlugin {
public:
    DeEsser() : EfxPlugin("eFX DeEsser", 0x4D583238u, deEsserDefs()) {}

protected:
    void prepare() override
    {
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        fast_.set(sr_, 0.2, 8);
        slow_.set(sr_, 0.2, 8);
        gain_.set(sr_, 30, 0.5);          // rises with the long time, falls with the short one
        key_ = -1;
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) band_[c].reset();
        fast_.v = slow_.v = 0;
        gain_.v = 1;
        gIn_.primed = gOut_.primed = false;
        sigmaSlow_ = 0;
    }
    void run(float* l, float* r, int n) override
    {
        const double fc = real(5), reduceDb = real(6), q = real(20);
        const bool listen = on(10);
        if (fc * 1000.0 + q != key_) {
            key_ = fc * 1000.0 + q;
            for (int c = 0; c < 2; ++c) band_[c].bandpass(sr_, fc, 0.35 + q * 2.0);
        }
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            const float xl = l[k] * gi, xr = r[k] * gi;
            const float bl = band_[0].process(xl), br = band_[1].process(xr);
            const double lev = std::max(std::fabs((double)bl), std::fabs((double)br));
            const double fast = fast_.next(lev);
            // share of the whole sound the band makes up: speech keeps a small part there, a sibilant most of it
            const double whole = slow_.next(std::max(std::fabs((double)xl), std::fabs((double)xr)));
            const double ratio = fast / (whole + 1e-9);
            double amount = std::max(0.0, std::min(1.0, (ratio - 0.3) / 0.4));
            if (whole < 1e-4) amount = 0;
            const double target = std::pow(10.0, -reduceDb * amount / 20.0);
            const double g = gain_.next(target);
            const float go = (float)gOut_.next();
            if (listen) { l[k] = bl * go; r[k] = br * go; continue; }
            // band turned down by (1 - g): a notch that deepens with the sibilant
            l[k] = (xl - (1.f - (float)g) * bl) * go;
            r[k] = (xr - (1.f - (float)g) * br) * go;
        }
    }

private:
    Smooth gIn_, gOut_;
    Biquad band_[2];
    Follower fast_, slow_, gain_;
    double key_ = -1, sigmaSlow_ = 0;
};

// ========================================================================================================== Gate
// A gate with a side-chain filter: the (filtered) level against the threshold opens it (attack) or, after the hold
// time, closes it down to Range below (release). The soft knee rounds the step. Look Ahead holds the sound back so
// the gate opens before a hit arrives.
class Gate : public EfxPlugin {
public:
    Gate() : EfxPlugin("eFX Gate", 0x4D583239u, gateDefs()) {}
    int latencySamples() override { return lookSamples(); }

protected:
    void prepare() override
    {
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        det_.set(sr_, 0.05, 20);
        hold_.size(1);
        key_ = -1;
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { sc_[c].reset(); hp_[c].reset(); }
        det_.v = 0;
        gainDb_ = -200;
        open_ = false;
        holdLeft_ = 0;
        hold_.clear();
        gIn_.primed = gOut_.primed = false;
    }
    void run(float* l, float* r, int n) override
    {
        const double thrDb = real(6), rangeDb = real(7);
        const bool listen = on(10), knee = on(11);
        const int type = choice(15);
        const double scFreq = real(5), hpFreq = real(21);
        const int look = lookSamples();
        const double holdMs = real(17) / 100.0 * 100.0;
        const double key = type * 1e6 + scFreq * 10 + hpFreq * 0.001;
        if (key != key_) {
            key_ = key;
            for (int c = 0; c < 2; ++c) {
                if (type == 1) sc_[c].lowpass(sr_, scFreq);
                else if (type == 2) sc_[c].bandpass(sr_, scFreq, 0.7);
                else if (type == 3) sc_[c].highpass(sr_, scFreq);
                if (hpFreq >= 0.5) hp_[c].highpass(sr_, hpFreq);
            }
        }
        const bool hpOn = hpFreq >= 0.5;
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        const double attackCoef = 1.0 - std::exp(-1.0 / (std::max(0.02, real(8)) * 0.001 * sr_));
        const double releaseCoef = 1.0 - std::exp(-1.0 / (std::max(1.0, real(9)) * 0.001 * sr_));
        const long holdSamples = (long)(holdMs * 0.001 * sr_);
        const double kneeDb = knee ? 12.0 : 0.0;
        if (look > 0 && hold_.n != (size_t)look) hold_.size((size_t)look);
        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            float xl = l[k] * gi, xr = r[k] * gi;
            float dl = xl, dr = xr;
            if (hpOn) { dl = hp_[0].process(dl); dr = hp_[1].process(dr); }
            if (type != 0) { dl = sc_[0].process(dl); dr = sc_[1].process(dr); }
            const double lev = det_.next(std::max(std::fabs((double)dl), std::fabs((double)dr)));
            const double levDb = lev > 1e-9 ? 20.0 * std::log10(lev) : -180.0;
            // open above the threshold (a little hysteresis stops chattering); the soft knee spreads the opening over kneeDb
            double wantDb;
            if (kneeDb > 0) {
                const double t = std::max(0.0, std::min(1.0, (levDb - (thrDb - kneeDb * 0.5)) / kneeDb));
                wantDb = -rangeDb * (1.0 - t * t * (3.0 - 2.0 * t));
                open_ = t > 0.5;
            } else {
                if (levDb > thrDb) { open_ = true; holdLeft_ = holdSamples; }
                else if (open_ && levDb < thrDb - 2.0) {
                    if (holdLeft_ > 0) --holdLeft_;
                    else open_ = false;
                } else if (open_) holdLeft_ = holdSamples;
                wantDb = open_ ? 0.0 : -rangeDb;
            }
            if (kneeDb > 0 && holdSamples > 0) {
                if (wantDb > gainDb_) holdLeft_ = holdSamples;
                else if (holdLeft_ > 0) { --holdLeft_; wantDb = gainDb_; }
            }
            if (gainDb_ < -150) gainDb_ = wantDb;
            gainDb_ += (wantDb - gainDb_) * (wantDb > gainDb_ ? attackCoef : releaseCoef);
            const float g = (float)std::pow(10.0, gainDb_ / 20.0);
            const float go = (float)gOut_.next();
            if (listen) { l[k] = dl * go; r[k] = dr * go; continue; }
            if (look > 0) hold_.step(xl, xr);
            l[k] = xl * g * go;
            r[k] = xr * g * go;
        }
    }

private:
    int lookSamples() const
    {
        const double ms = real(20);
        return ms < 0.02 ? 0 : std::max(1, (int)std::lround(ms * 0.001 * sr_));
    }
    Smooth gIn_, gOut_;
    Biquad sc_[2], hp_[2];
    Follower det_;
    Hold2 hold_;
    double key_ = -1, gainDb_ = -200;
    bool open_ = false;
    long holdLeft_ = 0;
};

// ================================================================================================== Stereo Delay
// Two delay lines with feedback: Feedback right of the middle runs the sides apart (dual), left of it the echoes
// swap sides (ping-pong). Feedback goes through a low cut and a damping low-pass. Analog tape adds soft saturation of
// the feedback, a slow wobble of the delay and hiss; Analog BBD adds a bandwidth falling with the delay, fewer
// stages, a compander (squeezed before the delay, opened after), clock drift and hiss.
class StereoDelay : public EfxPlugin {
public:
    StereoDelay() : EfxPlugin("eFX StereoDelay", 0x4D583234u, stereoDelayDefs()) {}

protected:
    void prepare() override
    {
        const size_t len = (size_t)(8.0 * 60.0 / 40.0 * 2.0 * sr_) + 64;     // two bars at 40 BPM (the slowest tempo used), plus room to wobble
        line_[0].resize(len);
        line_[1].resize(len);
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        mix_.setTime(sr_, 5);
        dl_.setTime(sr_, 30);
        dr_.setTime(sr_, 30);
        key_ = -1;
    }
    void reset() override
    {
        line_[0].clear();
        line_[1].clear();
        for (int c = 0; c < 2; ++c) { lc_[c].reset(); dp_[c].reset(); bw_[c].reset(); }
        gIn_.primed = gOut_.primed = mix_.primed = dl_.primed = dr_.primed = false;
        wob_ = 0;
        noise_ = 12345u;
        drift_ = 0;
        driftTarget_ = 0;
        env_ = 0;
    }

    // delay of a note setting in samples (1/32 .. 2 bars at the tempo)
    double delaySamples(int step) const
    {
        static const double beats[] = {0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 2.0, 4.0, 8.0};
        return beats[std::max(0, std::min(12, step))] * 60.0 / tempoBpm() * sr_;
    }

    void run(float* l, float* r, int n) override
    {
        const int mode = choice(1);
        const double fb = real(8) / 100.0;                  // -1 (ping-pong) .. 1 (dual)
        const bool ping = fb < 0;
        const double fbAmt = std::min(0.97, std::fabs(fb));
        const double damp = real(7) / 100.0;
        const double lowCut = real(20);
        const double noiseAmt = mode == 0 ? 0.0 : dbToLin(real(21));
        const double stages = real(22);
        const double compander = real(23) / 100.0;
        const double driftAmt = real(24) / 100.0;
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        mix_.set(real(9) / 100.0);
        const double wantL = delaySamples((int)real(5)), wantR = delaySamples((int)real(6));
        dl_.set(wantL);
        dr_.set(wantR);
        const double key = mode * 1e9 + damp * 1e6 + lowCut + stages * 1e-3;
        if (key != key_) {
            key_ = key;
            const double dampHz = 20000.0 * std::pow(0.04, damp);               // 20kHz .. 800Hz
            double bwHz = 20000.0;
            if (mode == 1) bwHz = 9000.0;
            if (mode == 2) bwHz = std::min(8000.0, 1.6e5 / std::sqrt(stages) * 1.0);   // fewer stages, duller
            for (int c = 0; c < 2; ++c) {
                dp_[c].lowpass(sr_, dampHz);
                if (lowCut > 10.5) lc_[c].highpass(sr_, lowCut);
                bw_[c].lowpass(sr_, bwHz);
            }
        }
        const bool lcOn = lowCut > 10.5;
        const double wobHz = mode == 1 ? 0.6 : 0.3;
        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            float xl = l[k] * gi, xr = r[k] * gi;
            // wobble (tape) or drifting clock (BBD) moves the delay a little
            wob_ += 2.0 * kPi * wobHz / sr_;
            if (wob_ > 2.0 * kPi) wob_ -= 2.0 * kPi;
            double mod = 0;
            if (mode == 1) mod = std::sin(wob_) * 0.0006 * sr_ * 0.5;
            if (mode == 2) {
                if ((noiseTick_++ & 2047) == 0) driftTarget_ = nextNoise() * driftAmt * 0.004;
                drift_ += (driftTarget_ - drift_) * 0.0005;
            }
            const double dL = dl_.next() * (1.0 + (mode == 2 ? drift_ : 0.0)) + mod;
            const double dR = dr_.next() * (1.0 + (mode == 2 ? drift_ : 0.0)) + mod;
            float yl = line_[0].tap(dL), yr = line_[1].tap(dR);
            if (mode == 2) {
                // the compander opens what was squeezed on the way in
                env_ += (std::max(std::fabs(yl), std::fabs(yr)) - env_) * 0.002;
                const float ex = 1.f + (float)(compander * 0.8 * std::min(1.0, env_ * 4.0));
                yl *= ex; yr *= ex;
            }
            if (noiseAmt > 0) { yl += (float)(nextNoise() * noiseAmt * 0.3); yr += (float)(nextNoise() * noiseAmt * 0.3); }
            // feedback path
            float fl = yl, fr = yr;
            fl = dp_[0].process(fl); fr = dp_[1].process(fr);
            if (lcOn) { fl = lc_[0].process(fl); fr = lc_[1].process(fr); }
            if (mode != 0) { fl = bw_[0].process(fl); fr = bw_[1].process(fr); }
            float inL = xl, inR = xr;
            if (ping) {
                inL += (float)(fbAmt * fr);
                inR += (float)(fbAmt * fl);
            } else {
                inL += (float)(fbAmt * fl);
                inR += (float)(fbAmt * fr);
            }
            if (mode == 1) { inL = std::tanh(inL * 1.2f) / 1.2f; inR = std::tanh(inR * 1.2f) / 1.2f; }
            if (mode == 2 && compander > 0) {
                // squeezed before the delay: the louder, the less gain
                const float sq = 1.f - (float)(compander * 0.4 * std::min(1.0, env_ * 4.0));
                inL *= sq; inR *= sq;
            }
            line_[0].push(inL);
            line_[1].push(inR);
            const double m = mix_.next();
            const float go = (float)gOut_.next();
            l[k] = (float)((xl * (1.0 - m) + yl * m) * go);
            r[k] = (float)((xr * (1.0 - m) + yr * m) * go);
        }
    }

private:
    double nextNoise()
    {
        noise_ = noise_ * 1664525u + 1013904223u;
        return (double)((noise_ >> 8) & 0xFFFF) / 32768.0 - 1.0;
    }
    Delay line_[2];
    Smooth gIn_, gOut_, mix_, dl_, dr_;
    Biquad lc_[2], dp_[2], bw_[2];
    double key_ = -1, wob_ = 0, drift_ = 0, driftTarget_ = 0, env_ = 0;
    uint32_t noise_ = 12345u;
    uint32_t noiseTick_ = 0;
};

// ================================================================================================== Tremolo / Pan
// Each side's level follows an oscillator (Rate), from sine (Shape 0%) to square (100%); Phase turns the right
// oscillator against the left (at 180 degrees the sound runs from side to side: an auto-pan); Depth is how much.
class TremoloPan : public EfxPlugin {
public:
    TremoloPan() : EfxPlugin("eFX TremoloPan", 0x4D583332u, tremoloPanDefs()) {}

protected:
    void prepare() override { gIn_.setTime(sr_, 5); gOut_.setTime(sr_, 5); depth_.setTime(sr_, 10); }
    void reset() override { phase_ = 0; gIn_.primed = gOut_.primed = depth_.primed = false; }
    void run(float* l, float* r, int n) override
    {
        const double rate = real(5), shape = real(6) / 100.0, off = real(7) * kPi / 180.0;
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        depth_.set(real(8) / 100.0);
        const double inc = 2.0 * kPi * rate / sr_;
        const double k = 1.0 + shape * 14.0;            // how hard the sine is pushed towards a square
        const double norm = std::tanh(k);
        for (int i = 0; i < n; ++i) {
            const double d = depth_.next();
            const double sl = std::tanh(k * std::sin(phase_)) / norm;
            const double sr = std::tanh(k * std::sin(phase_ + off)) / norm;
            const float gl = (float)(1.0 - d * 0.5 * (1.0 - sl)), gr = (float)(1.0 - d * 0.5 * (1.0 - sr));
            const float gi = (float)gIn_.next(), go = (float)gOut_.next();
            l[i] *= gi * gl * go;
            r[i] *= gi * gr * go;
            phase_ += inc;
            if (phase_ > 2.0 * kPi) phase_ -= 2.0 * kPi;
        }
    }

private:
    double phase_ = 0;
    Smooth gIn_, gOut_, depth_;
};

// =================================================================================================== Vocal Strip
// A channel strip for a voice in fader order: 24 dB/octave high-pass, a gentle gate, a de-esser (the detection
// frequency is the tweak), a compressor (one fader sets threshold and ratio together) and a tone control (tilt of
// lows against highs).
class VocalStrip : public EfxPlugin {
public:
    VocalStrip() : EfxPlugin("eFX VocalStrip", 0x4D583330u, vocalStripDefs()) {}

protected:
    void prepare() override
    {
        gIn_.setTime(sr_, 5);
        gOut_.setTime(sr_, 5);
        gateDet_.set(sr_, 0.1, 30);
        essFast_.set(sr_, 0.2, 8);
        essSlow_.set(sr_, 0.2, 8);
        essGain_.set(sr_, 30, 0.5);
        key_ = -1;
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { hp1_[c].reset(); hp2_[c].reset(); ess_[c].reset(); lo_[c].reset(); hi_[c].reset(); }
        gateDet_.v = essFast_.v = essSlow_.v = 0;
        gateGain_ = 1;
        essGain_.v = 1;
        grDb_ = 0;
        stress_ = 0;
        gIn_.primed = gOut_.primed = false;
    }
    void run(float* l, float* r, int n) override
    {
        const double hpHz = real(5), gateDb = real(6), essDb = real(7), comp = real(8), tone = real(9) / 100.0;
        const double essHz = real(20), attackMs = real(22), adapt = real(23);
        const bool makeup = on(21);
        const double key = hpHz + essHz * 7 + tone * 1000;
        if (key != key_) {
            key_ = key;
            for (int c = 0; c < 2; ++c) {
                hp1_[c].highpass(sr_, hpHz, 0.5412);        // two 2nd-order sections, 24 dB/octave
                hp2_[c].highpass(sr_, hpHz, 1.3066);
                ess_[c].bandpass(sr_, essHz, 1.2);
                lo_[c].lowShelf(sr_, 900, -tone * 3.0);
                hi_[c].highShelf(sr_, 900, tone * 3.0);
            }
        }
        const double thr = -(comp / 48.0) * 36.0, ratio = 1.0 + comp / 48.0 * 7.0;
        const double makeupDb = makeup ? -thr * (1.0 - 1.0 / ratio) * 0.6 : 0.0;
        const double atkCoef = std::exp(-1.0 / (std::max(0.05, attackMs) * 0.001 * sr_));
        const double relBase = 0.15;
        gIn_.set(dbToLin(real(3)));
        gOut_.set(dbToLin(real(4)));
        const double gateThr = gateDb;
        const bool gateOn = gateDb > -89.5;
        for (int k = 0; k < n; ++k) {
            const float gi = (float)gIn_.next();
            float xl = hp2_[0].process(hp1_[0].process(l[k] * gi));
            float xr = hp2_[1].process(hp1_[1].process(r[k] * gi));
            // gate: a smooth step of up to 24dB
            if (gateOn) {
                const double lev = gateDet_.next(std::max(std::fabs((double)xl), std::fabs((double)xr)));
                const double ldb = lev > 1e-9 ? 20.0 * std::log10(lev) : -180.0;
                const double t = std::max(0.0, std::min(1.0, (ldb - (gateThr - 6.0)) / 12.0));
                const double want = std::pow(10.0, -24.0 * (1.0 - t * t * (3.0 - 2.0 * t)) / 20.0);
                gateGain_ += (want - gateGain_) * (want > gateGain_ ? 0.01 : 0.0005);
                xl *= (float)gateGain_; xr *= (float)gateGain_;
            }
            // de-esser
            if (essDb > 0.05) {
                const float bl = ess_[0].process(xl), br = ess_[1].process(xr);
                const double fast = essFast_.next(std::max(std::fabs((double)bl), std::fabs((double)br)));
                const double whole = essSlow_.next(std::max(std::fabs((double)xl), std::fabs((double)xr)));
                double amount = std::max(0.0, std::min(1.0, (fast / (whole + 1e-9) - 0.3) / 0.4));
                if (whole < 1e-4) amount = 0;
                const double g = essGain_.next(std::pow(10.0, -essDb * amount / 20.0));
                xl -= (1.f - (float)g) * bl;
                xr -= (1.f - (float)g) * br;
            }
            // compressor
            if (comp > 0.05) {
                const double level = std::max(std::fabs((double)xl), std::fabs((double)xr));
                const double ldb = level > 1e-9 ? 20.0 * std::log10(level) : -180.0;
                const double over = ldb - thr;
                double target = 0;
                if (over > 3.0) target = (1.0 / ratio - 1.0) * over;
                else if (over > -3.0) target = (1.0 / ratio - 1.0) * (over + 3.0) * (over + 3.0) / 12.0;
                double coef;
                if (target < grDb_) coef = atkCoef;
                else coef = std::exp(-1.0 / (relBase * (1.0 + adapt * 0.6 * stress_) * sr_));
                grDb_ = target + coef * (grDb_ - target);
                const double depth = std::min(1.0, -grDb_ / 12.0);
                stress_ += (depth - stress_) * (depth > stress_ ? 0.0004 : 0.00004);
                const float g = (float)std::pow(10.0, (grDb_ + makeupDb) / 20.0);
                xl *= g; xr *= g;
            }
            if (tone != 0.0) {
                xl = hi_[0].process(lo_[0].process(xl));
                xr = hi_[1].process(lo_[1].process(xr));
            }
            const float go = (float)gOut_.next();
            l[k] = xl * go;
            r[k] = xr * go;
        }
    }

private:
    Smooth gIn_, gOut_;
    Biquad hp1_[2], hp2_[2], ess_[2], lo_[2], hi_[2];
    Follower gateDet_, essFast_, essSlow_, essGain_;
    double key_ = -1, gateGain_ = 1, grDb_ = 0, stress_ = 0;
};

}  // namespace

std::unique_ptr<EfxPlugin> makeDeEsser() { return std::unique_ptr<EfxPlugin>(new DeEsser()); }
std::unique_ptr<EfxPlugin> makeGate() { return std::unique_ptr<EfxPlugin>(new Gate()); }
std::unique_ptr<EfxPlugin> makeStereoDelay() { return std::unique_ptr<EfxPlugin>(new StereoDelay()); }
std::unique_ptr<EfxPlugin> makeTremoloPan() { return std::unique_ptr<EfxPlugin>(new TremoloPan()); }
std::unique_ptr<EfxPlugin> makeVocalStrip() { return std::unique_ptr<EfxPlugin>(new VocalStrip()); }

}  // namespace efx
}  // namespace reno
