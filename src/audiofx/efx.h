// Reno's versions of VEGAS Pro's MAGIX "essentialFX" (eFX) audio effects: Limiter, Compressor, Reverb, Tube Stage,
// Phaser, Chorus/Flanger, De-Esser, Gate, Stereo Delay, Tremolo/Pan, Vocal Strip. The real ones are VST2 plug-ins
// tied to MAGIX licensing that only VEGAS satisfies, so outside VEGAS they run restricted. These are written from
// the effects' manuals and the behaviour of their presets, not from the plug-ins: the same 31 settings (numbers,
// names, 0..1 meaning and wording), so a project's saved settings load and save unchanged, but the sound is Reno's own.
// Files: efx.h (shared parts), efx_base.cpp (base class, settings, selection), efx_defs.cpp (settings of each
// effect), efx_dynamics.cpp (Limiter, Compressor), efx_modfx.cpp (Reverb, Tube Stage, Phaser, Chorus/Flanger),
// efx_more.cpp (De-Esser, Gate, Stereo Delay, Tremolo/Pan, Vocal Strip).
#pragma once
#include "audioplugin.h"
#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace reno {
namespace efx {

const double kPi = 3.14159265358979323846;

// ---- how one setting's 0..1 is read -------------------------------------------------------------------------
// Each setting has one of a few shapes, fitted to the real plug-in's readings (its text at 41 steps of 0..1):
//   Lin   lo + (hi-lo) * x
//   Rat   lo + (hi-lo) * x / (a - (a-1) x)      a skewed line: a > 1 keeps the low end fine (frequencies, times)
//   Pow   lo + (hi-lo) * x^p
//   Gain  20 log10 x in dB (silent, "-inf", at 0)
//   Steps index into a list of names; 'thresholds' holds where each next name begins (above the value)
// plus a number format (Fmt). Refit with the sweep in tests (efx_test checks all 41 steps).
enum class Fmt {
    None,
    DB,         // "-16.8dB" "-5.57dB" "0dB" "12.0dB" and "-inf"
    MS,         // "526us" "4.9ms" "10ms" "1.26s" (with the micro sign)
    MSP,        // "4.81ms" "10.6ms" "103.4ms" (predelay)
    MSOFF,      // "Off" below 20us, else as MS
    SEC,        // "0.56s" "15.0s"
    HZ,         // "850Hz" "1.04kHz"
    HZR,        // "1.54Hz" "15.0Hz"
    KHZ2,       // "2.00kHz" "12.00kHz"
    HZSP,       // "0.10 Hz" "13 Hz"
    HZOFF,      // "Off" at the lowest, else "25Hz"
    PCT,        // "38%" (cut off, not rounded)
    PCT1,       // "-10.0%"
    DEG,        // "180" and the degree sign
    NUM2,       // "2.30"
    RATIO,      // "2.1 : 1" "10 : 1"
    PEAKRMS,    // "Peak" "90 : 10" "RMS" (the value is the percentage of RMS)
    SOFTHARD,   // "Soft" "38%" "Hard" (the value is the percentage of hard)
    PINGDUAL    // "20% ping" "0% dual" "100% dual" (the value is -100..100)
};

enum class Kind { Unused, Lin, Rat, Pow, Gain, Steps, Fixed };

struct ParamDef {
    const char* name = "-";             // "-" for a setting the effect does not use
    float def = 0.f;                    // the 0..1 value a new effect starts with
    Kind kind = Kind::Unused;
    double lo = 0, hi = 1, a = 1;       // Lin / Rat / Pow (a is Rat's skew or Pow's exponent)
    const char* const* labels = nullptr;    // Steps
    const double* thresholds = nullptr;     // Steps: nlabels-1 values, the name k+1 starts above thresholds[k]
    int nlabels = 0;
    Fmt fmt = Fmt::None;
    const char* fixed = "";             // Fixed: the wording that is always shown
};

ParamDef pUnused();
ParamDef pLin(const char* name, float def, double lo, double hi, Fmt fmt);
ParamDef pRat(const char* name, float def, double lo, double hi, double a, Fmt fmt);
ParamDef pPow(const char* name, float def, double lo, double hi, double p, Fmt fmt);
ParamDef pGain(const char* name, float def);
ParamDef pSteps(const char* name, float def, const char* const* labels, int n, const double* thresholds);
ParamDef pFixed(const char* name, float def, const char* text);

double mapValue(const ParamDef& d, double x);           // the real value (dB, ms, Hz, choice index...)
std::string mapText(const ParamDef& d, double x);       // the text the real plug-in shows
std::string formatNumber(Fmt f, double v);

// the 31 settings of each effect (efx_defs.cpp): the eleven eFX effects of VEGAS Pro 18
std::vector<ParamDef> limiterDefs();
std::vector<ParamDef> compressorDefs();
std::vector<ParamDef> reverbDefs();
std::vector<ParamDef> tubeStageDefs();
std::vector<ParamDef> phaserDefs();
std::vector<ParamDef> chorusFlangerDefs();
std::vector<ParamDef> deEsserDefs();
std::vector<ParamDef> gateDefs();
std::vector<ParamDef> stereoDelayDefs();
std::vector<ParamDef> tremoloPanDefs();
std::vector<ParamDef> vocalStripDefs();

// ---- small DSP parts ----------------------------------------------------------------------------------------
inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double lin) { return 20.0 * std::log10(lin > 1e-12 ? lin : 1e-12); }
inline float flushDenormal(float v) { return std::fabs(v) < 1e-20f ? 0.f : v; }

// A second-order filter (RBJ's cookbook shapes), one channel's state per object.
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    void reset() { z1 = z2 = 0; }
    void highpass(double fs, double f, double q = 0.70710678);
    void lowpass(double fs, double f, double q = 0.70710678);
    void bandpass(double fs, double f, double q = 0.70710678);
    void lowShelf(double fs, double f, double gainDb);
    void highShelf(double fs, double f, double gainDb);
    inline float process(float x)
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float)y;
    }
};

// One-pole smoothing of a value that is set now and then (gains, mixes): block size does not matter.
struct Smooth {
    double v = 0, target = 0, coef = 0.001;
    bool primed = false;
    void setTime(double fs, double ms) { coef = 1.0 - std::exp(-1.0 / (0.001 * ms * fs + 1e-9)); }
    void set(double t) { target = t; if (!primed) { v = t; primed = true; } }
    void snap() { v = target; }
    inline double next() { v += (target - v) * coef; return v; }
};

// A delay line read at fractional positions (interpolated linearly).
struct Delay {
    std::vector<float> buf;
    size_t pos = 0;
    void resize(size_t n) { buf.assign(n < 8 ? 8 : n, 0.f); pos = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.f); pos = 0; }
    inline void push(float v) { buf[pos] = v; if (++pos >= buf.size()) pos = 0; }
    // the sample pushed 'd' samples ago (1 = the latest)
    inline float tap(double d) const
    {
        const size_t n = buf.size();
        if (d < 1.0) d = 1.0;
        if (d > (double)n - 2.0) d = (double)n - 2.0;
        const long di = (long)d;
        const float fr = (float)(d - (double)di);
        long a = (long)pos - di;
        if (a < 0) a += (long)n;
        long b = a - 1;
        if (b < 0) b += (long)n;
        return buf[(size_t)a] * (1.f - fr) + buf[(size_t)b] * fr;
    }
};

// ---- the base of the effects ----------------------------------------------------------------------------
constexpr int kParams = 31;     // what every eFX effect has
constexpr int kActive = 0;      // setting 0 of each: Active (on / off)
constexpr int kVersion = 30;    // setting 30 of each: a version the real plug-ins keep (kept as saved)

class EfxPlugin : public AudioPlugin {
public:
    EfxPlugin(const char* displayName, uint32_t vstId, std::vector<ParamDef> defs);

    bool activate(double sampleRate, int maxBlock) override;
    void deactivate() override {}
    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override;
    int latencySamples() override { return latency_; }

    int paramCount() override { return kParams; }
    std::string paramName(int i) override;
    std::string paramText(int i) override;
    float paramGet(int i) override;
    void paramSet(int i, float v) override;

    // "P:" and every setting as a 32-bit float: how Reno stores a VST2 program (and writes it to the .veg)
    std::string saveState() override;
    bool loadState(const std::string& s) override;

    // VST preset files (.fxp / .fxb): the 31 values of one program, as VEGAS and other hosts write them
    std::string saveVstPresetFile(bool bank) override;
    bool loadVstPresetFile(const std::string& bytes, std::string* error) override;

    const ParamDef& def(int i) const { return defs_[(size_t)i]; }
    // the real value of a setting as the DSP reads it (dB, ms, Hz, choice index)
    double real(int i) const;
    double sampleRate() const { return sr_; }
    uint32_t vstId() const { return id_; }

protected:
    virtual void prepare() {}                   // the sample rate (sr_) is known: size buffers, fit filters
    virtual void reset() {}                     // forget the sound so far
    virtual void run(float* l, float* r, int n) = 0;
    bool on(int i) const;
    float raw(int i) const { return values_[(size_t)i].load(std::memory_order_relaxed); }
    int choice(int i) const { return (int)std::lround(real(i)); }

    double sr_ = 48000;
    int latency_ = 0;
    int maxBlock_ = 512;

private:
    uint32_t id_;
    std::vector<ParamDef> defs_;
    std::array<std::atomic<float>, kParams> values_;
    int64_t expected_ = -1;
    bool wasActive_ = true;
};

std::unique_ptr<EfxPlugin> makeLimiter();
std::unique_ptr<EfxPlugin> makeCompressor();
std::unique_ptr<EfxPlugin> makeReverb();
std::unique_ptr<EfxPlugin> makeTubeStage();
std::unique_ptr<EfxPlugin> makePhaser();
std::unique_ptr<EfxPlugin> makeChorusFlanger();
std::unique_ptr<EfxPlugin> makeDeEsser();
std::unique_ptr<EfxPlugin> makeGate();
std::unique_ptr<EfxPlugin> makeStereoDelay();
std::unique_ptr<EfxPlugin> makeTremoloPan();
std::unique_ptr<EfxPlugin> makeVocalStrip();

}  // namespace efx
}  // namespace reno
