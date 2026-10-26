// Reno's built-in audio effects: stand-ins for common VEGAS DirectX effects (Pitch
// Shift, Chorus, Flange/Wah-wah, Distortion, Vibrato, Amplitude Modulation, Simple Delay, Reverb), with VEGAS's
// names and similar settings, for machines where VEGAS's own are missing. They are ordinary audio plug-ins to the
// rest of Reno (format "BUILTIN"): the effects window shows their sliders and presets and the project stores
// their settings. The sound is Reno's own.
#include "audioplugin.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <memory>
#include <sstream>

namespace reno {

namespace {

const double kPi = 3.14159265358979323846;

struct Param {
    const char* name;
    double min, max, def;
    const char* unit;           // shown after the value ("dB", "ms"...)
    std::vector<const char*> choices;   // a choice (the value is the index)
    int decimals = 1;
};

struct Preset {
    const char* name;
    std::vector<double> values;     // in parameter order (fewer: the rest at their defaults)
};

// A ring buffer read at fractional delays (linear interpolation).
struct DelayLine {
    std::vector<float> buf;
    size_t pos = 0;
    void resize(size_t n) { buf.assign(std::max<size_t>(n, 4), 0.f); pos = 0; }
    void clear() { std::fill(buf.begin(), buf.end(), 0.f); }
    void push(float v)
    {
        buf[pos] = v;
        if (++pos >= buf.size()) pos = 0;
    }
    // the sample written 'd' samples ago (1 = the latest), interpolated for fractional d
    float at(double d) const
    {
        const size_t n = buf.size();
        d = std::max(1.0, std::min(d, (double)n - 2));
        const double r = (double)pos - d;
        double rf = std::floor(r);
        const double frac = r - rf;
        long i0 = (long)rf;
        while (i0 < 0) i0 += (long)n;
        const size_t a = (size_t)i0 % n, b = (a + 1) % n;
        return (float)(buf[a] * (1 - frac) + buf[b] * frac);
    }
};

class BuiltinAudio : public AudioPlugin {
public:
    BuiltinAudio(std::vector<Param> params, std::vector<Preset> presets) : params_(std::move(params)), presets_(std::move(presets)), values_(params_.size())
    {
        for (size_t k = 0; k < params_.size(); ++k) values_[k] = (float)params_[k].def;
    }
    bool activate(double sampleRate, int maxBlock) override
    {
        (void)maxBlock;
        sr_ = sampleRate > 1000 ? sampleRate : 48000;
        prepare();
        reset();
        return true;
    }
    void deactivate() override {}
    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        if (!left || !right || frames <= 0) return;
        if (sr_ <= 0) activate(48000, frames);
        // a jump in time (seek, loop) restarts the effect so no echo from elsewhere carries over
        if (samplePos != expected_) reset();
        expected_ = samplePos + frames;
        (void)playing;
        run(left, right, frames);
        for (int k = 0; k < frames; ++k) {
            if (!std::isfinite(left[k])) left[k] = 0.f;
            if (!std::isfinite(right[k])) right[k] = 0.f;
        }
    }
    int paramCount() override { return (int)params_.size(); }
    std::string paramName(int i) override { return valid(i) ? params_[i].name : std::string(); }
    std::string paramText(int i) override
    {
        if (!valid(i)) return std::string();
        const Param& p = params_[i];
        const double v = values_[i].load();
        if (!p.choices.empty()) {
            const int c = std::max(0, std::min((int)p.choices.size() - 1, (int)std::lround(v)));
            return p.choices[c];
        }
        char b[64];
        std::snprintf(b, sizeof b, "%.*f%s%s", p.decimals, v, *p.unit ? " " : "", p.unit);
        return b;
    }
    float paramGet(int i) override
    {
        if (!valid(i)) return 0.f;
        const Param& p = params_[i];
        return (float)((values_[i].load() - p.min) / (p.max - p.min));
    }
    bool paramRange(int i, double* lo, double* hi) override
    {
        if (!valid(i)) return false;
        if (lo) *lo = params_[i].min;
        if (hi) *hi = params_[i].max;
        return true;
    }
    void paramSet(int i, float v) override
    {
        if (!valid(i) || !std::isfinite(v)) return;
        const Param& p = params_[i];
        double x = p.min + std::max(0.f, std::min(1.f, v)) * (p.max - p.min);
        if (!p.choices.empty()) x = std::round(x);
        values_[i] = (float)x;
    }
    // "name=value" lines, so settings survive a change in the parameters' order
    std::string saveState() override
    {
        std::ostringstream o;
        o << "reno-builtin-audio 1\n";
        for (size_t k = 0; k < params_.size(); ++k) o << params_[k].name << '=' << values_[k].load() << '\n';
        return o.str();
    }
    bool loadState(const std::string& s) override
    {
        if (s.compare(0, 18, "reno-builtin-audio") != 0) return false;
        std::istringstream in(s);
        std::string line;
        std::getline(in, line);
        while (std::getline(in, line)) {
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string name = line.substr(0, eq);
            char* end = nullptr;
            const double v = std::strtod(line.c_str() + eq + 1, &end);
            if (!std::isfinite(v)) continue;
            for (size_t k = 0; k < params_.size(); ++k)
                if (name == params_[k].name) values_[k] = (float)std::max(params_[k].min, std::min(params_[k].max, v));
        }
        return true;
    }
    int programCount() override { return (int)presets_.size(); }
    std::string programName(int i) override { return i >= 0 && i < (int)presets_.size() ? presets_[i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= (int)presets_.size()) return false;
        for (size_t k = 0; k < params_.size(); ++k)
            values_[k] = (float)(k < presets_[i].values.size() ? presets_[i].values[k] : params_[k].def);
        program_ = i;
        return true;
    }

protected:
    double v(int i) const { return values_[i].load(); }
    double sr_ = 0;
    virtual void prepare() {}       // buffers for sr_
    virtual void reset() {}         // silence in the buffers, filters at rest
    virtual void run(float* l, float* r, int n) = 0;

private:
    bool valid(int i) const { return i >= 0 && i < (int)params_.size(); }
    std::vector<Param> params_;
    std::vector<Preset> presets_;
    std::vector<std::atomic<float>> values_;
    int program_ = -1;
    int64_t expected_ = -1;
};

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// ------------------------------------------------------------------------------------------ Pitch Shift
// Two read heads sweep a short delay line at the new speed, each faded in and out with a raised cosine while the
// other takes over: pitch changes, length stays (VEGAS's Pitch Shift works this way, with a window size).
class PitchShift : public BuiltinAudio {
public:
    PitchShift()
        : BuiltinAudio({{"Semitones", -24, 24, 0, "semitones", {}, 0}, {"Cents", -100, 100, 0, "cents", {}, 0},
                        {"Window", 10, 100, 40, "ms", {}, 0}, {"Dry out", -60, 0, -60, "dB", {}, 1}, {"Effect out", -60, 6, 0, "dB", {}, 1}},
                       {{"Up an octave", {12}}, {"Down an octave", {-12}}, {"Up a fifth", {7}}, {"Chipmunk", {9, 0, 20}}, {"Monster", {-7, 0, 60}},
                        {"Detuned double", {0, 12, 40, 0, 0}}})
    {
    }

protected:
    DelayLine dl_[2];
    double phase_ = 0;
    void prepare() override
    {
        for (auto& d : dl_) d.resize((size_t)(sr_ * 0.25));
    }
    void reset() override
    {
        for (auto& d : dl_) d.clear();
        phase_ = 0;
    }
    void run(float* l, float* r, int n) override
    {
        const double ratio = std::pow(2.0, (v(0) + v(1) / 100.0) / 12.0);
        const double win = std::max(16.0, v(2) / 1000.0 * sr_);
        const float dry = (float)(v(3) <= -59.9 ? 0.0 : dbToGain(v(3))), wet = (float)(v(4) <= -59.9 ? 0.0 : dbToGain(v(4)));
        // head A's delay goes from 'win' down to 0 (or up, for lower notes) once per window
        const double step = (1.0 - ratio) / win;
        float* io[2] = {l, r};
        for (int k = 0; k < n; ++k) {
            phase_ += step;
            phase_ -= std::floor(phase_);
            const double pa = phase_, pb = std::fmod(phase_ + 0.5, 1.0);
            const double ga = std::sin(kPi * pa), gb = std::sin(kPi * pb);
            for (int c = 0; c < 2; ++c) {
                dl_[c].push(io[c][k]);
                const float a = dl_[c].at(1 + pa * win), b = dl_[c].at(1 + pb * win);
                const float out = (float)(a * ga * ga + b * gb * gb);
                io[c][k] = io[c][k] * dry + out * wet;
            }
        }
    }
};

// ------------------------------------------------------------------------------------------ Chorus
class Chorus : public BuiltinAudio {
public:
    Chorus()
        : BuiltinAudio({{"Dry out", -60, 0, 0, "dB", {}, 1}, {"Chorus out", -60, 0, -3, "dB", {}, 1}, {"Voices", 1, 4, 2, "", {}, 0},
                        {"Delay", 5, 50, 20, "ms", {}, 1}, {"Depth", 0, 15, 4, "ms", {}, 1}, {"Rate", 0.05, 10, 0.8, "Hz", {}, 2},
                        {"Feedback", 0, 90, 0, "%", {}, 0}, {"Stereo spread", 0, 100, 50, "%", {}, 0}},
                       {{"Subtle doubling", {0, -6, 1, 15, 2, 0.4, 0, 30}}, {"Thick chorus", {0, -3, 4, 22, 6, 0.9, 10, 80}},
                        {"Seasick", {-60, 0, 2, 25, 12, 3.5, 20, 100}}, {"Robot", {0, -2, 1, 10, 0, 0.1, 80, 0}}})
    {
    }

protected:
    DelayLine dl_[2];
    double lfo_ = 0;
    void prepare() override { for (auto& d : dl_) d.resize((size_t)(sr_ * 0.1)); }
    void reset() override { for (auto& d : dl_) d.clear(); lfo_ = 0; }
    void run(float* l, float* r, int n) override
    {
        const float dry = (float)(v(0) <= -59.9 ? 0.0 : dbToGain(v(0))), wet = (float)(v(1) <= -59.9 ? 0.0 : dbToGain(v(1)));
        const int voices = std::max(1, std::min(4, (int)std::lround(v(2))));
        const double base = v(3) / 1000.0 * sr_, depth = v(4) / 1000.0 * sr_, inc = v(5) / sr_, fb = v(6) / 100.0, spread = v(7) / 100.0;
        float* io[2] = {l, r};
        for (int k = 0; k < n; ++k) {
            lfo_ += inc;
            lfo_ -= std::floor(lfo_);
            for (int c = 0; c < 2; ++c) {
                double sum = 0;
                for (int vo = 0; vo < voices; ++vo) {
                    const double ph = lfo_ + (double)vo / voices + (c ? spread * 0.25 : 0.0);
                    const double d = base + depth * 0.5 * (1 + std::sin(2 * kPi * ph));
                    sum += dl_[c].at(d);
                }
                const float x = io[c][k];
                const float ch = (float)(sum / std::sqrt((double)voices));
                dl_[c].push((float)(x + ch * fb));
                io[c][k] = x * dry + ch * wet;
            }
        }
    }
};

// ------------------------------------------------------------------------------------------ Flange/Wah-wah
class FlangeWah : public BuiltinAudio {
public:
    FlangeWah()
        : BuiltinAudio({{"Effect", 0, 2, 0, "", {"Flange", "Wah-wah", "Phaser"}, 0}, {"Rate", 0.05, 10, 0.5, "Hz", {}, 2},
                        {"Depth", 0, 100, 70, "%", {}, 0}, {"Delay / frequency", 0, 100, 40, "%", {}, 0},
                        {"Feedback / resonance", 0, 95, 50, "%", {}, 0}, {"Dry out", -60, 0, -6, "dB", {}, 1}, {"Effect out", -60, 0, -6, "dB", {}, 1}},
                       {{"Jet flange", {0, 0.2, 90, 30, 80}}, {"Fast wah", {1, 4, 80, 50, 70, -60, 0}}, {"Slow wah", {1, 0.7, 90, 40, 80, -60, 0}},
                        {"Phaser", {2, 0.4, 80, 50, 60}}})
    {
    }

protected:
    DelayLine dl_[2];
    double lfo_ = 0;
    // state-variable filters (wah) and all-pass stages (phaser)
    double lp_[2] = {0, 0}, bp_[2] = {0, 0};
    double ap_[2][6] = {{0}}, last_[2] = {0, 0};
    void prepare() override { for (auto& d : dl_) d.resize((size_t)(sr_ * 0.05)); }
    void reset() override
    {
        for (auto& d : dl_) d.clear();
        lfo_ = 0;
        lp_[0] = lp_[1] = bp_[0] = bp_[1] = last_[0] = last_[1] = 0;
        std::memset(ap_, 0, sizeof ap_);
    }
    void run(float* l, float* r, int n) override
    {
        const int mode = (int)std::lround(v(0));
        const double inc = v(1) / sr_, depth = v(2) / 100.0, centre = v(3) / 100.0, fb = v(4) / 100.0;
        const float dry = (float)(v(5) <= -59.9 ? 0.0 : dbToGain(v(5))), wet = (float)(v(6) <= -59.9 ? 0.0 : dbToGain(v(6)));
        float* io[2] = {l, r};
        for (int k = 0; k < n; ++k) {
            lfo_ += inc;
            lfo_ -= std::floor(lfo_);
            for (int c = 0; c < 2; ++c) {
                const double sw = 0.5 * (1 + std::sin(2 * kPi * (lfo_ + (c ? 0.1 : 0.0))));    // 0..1
                const float x = io[c][k];
                double y = x;
                if (mode == 0) {
                    // a 0.1 .. 10 ms delay swept, fed back
                    const double ms = 0.1 + (centre * 5.0) * (1 - depth) + (centre * 5.0 + 5.0 * depth) * sw * depth;
                    const double d = std::max(1.0, ms / 1000.0 * sr_);
                    const float del = dl_[c].at(d);
                    dl_[c].push((float)(x + del * fb));
                    y = del;
                } else if (mode == 1) {
                    // a band-pass swept between 300 Hz and 3 kHz
                    const double lo = 200 + 600 * centre, hi = lo * (1 + 9 * depth);
                    const double f = lo * std::pow(hi / lo, sw);
                    const double g = 2 * std::sin(kPi * std::min(f, sr_ * 0.24) / sr_);
                    const double q = 1.0 - 0.95 * fb;   // damping (lower: sharper)
                    const double hp = x - lp_[c] - q * bp_[c];
                    bp_[c] += g * hp;
                    lp_[c] += g * bp_[c];
                    y = bp_[c] * (1.5 + 2 * fb);
                } else {
                    // six first-order all-passes swept together, fed back
                    const double f = (200 + 1800 * centre) * std::pow(2.0, 3 * depth * (sw - 0.5));
                    const double t = std::tan(kPi * std::min(f, sr_ * 0.24) / sr_), a = (t - 1) / (t + 1);
                    double s = x + last_[c] * fb;
                    for (int st = 0; st < 6; ++st) {
                        const double o = a * s + ap_[c][st];
                        ap_[c][st] = s - a * o;
                        s = o;
                    }
                    last_[c] = s;
                    y = s;
                }
                io[c][k] = (float)(x * dry + y * wet);
            }
        }
    }
};

// ------------------------------------------------------------------------------------------ Distortion
class Distortion : public BuiltinAudio {
public:
    Distortion()
        : BuiltinAudio({{"Drive", 0, 60, 20, "dB", {}, 1}, {"Shape", 0, 3, 0, "", {"Soft clip", "Hard clip", "Fold", "Bit crush"}, 0},
                        {"Tone", 500, 20000, 8000, "Hz", {}, 0}, {"Crush bits", 1, 16, 6, "bits", {}, 0},
                        {"Dry out", -60, 0, -60, "dB", {}, 1}, {"Effect out", -60, 24, -6, "dB", {}, 1}},
                       {{"Warm", {10, 0, 6000, 6, -60, -3}}, {"Fuzz", {35, 1, 9000, 6, -60, -9}}, {"Earrape", {60, 1, 20000, 6, -60, 12}},
                        {"Bit crusher", {0, 3, 20000, 4, -60, 0}}, {"Wave folder", {24, 2, 12000, 6, -60, -6}}})
    {
    }

protected:
    double lp_[2] = {0, 0};
    double hold_[2] = {0, 0};
    int count_ = 0;
    void reset() override { lp_[0] = lp_[1] = hold_[0] = hold_[1] = 0; count_ = 0; }
    void run(float* l, float* r, int n) override
    {
        const double drive = dbToGain(v(0));
        const int shape = (int)std::lround(v(1));
        const double a = 1 - std::exp(-2 * kPi * std::min(v(2), sr_ * 0.45) / sr_);
        const double levels = std::pow(2.0, std::lround(v(3)) - 1);
        const float dry = (float)(v(4) <= -59.9 ? 0.0 : dbToGain(v(4))), wet = (float)(v(5) <= -59.9 ? 0.0 : dbToGain(v(5)));
        // bit crush also lowers the sample rate with Drive (0 dB: unchanged, 60 dB: a 64th)
        const int every = shape == 3 ? std::max(1, (int)std::lround(std::pow(2.0, v(0) / 10.0))) : 1;
        float* io[2] = {l, r};
        for (int k = 0; k < n; ++k) {
            const bool take = (count_++ % every) == 0;
            for (int c = 0; c < 2; ++c) {
                const float x = io[c][k];
                double y;
                switch (shape) {
                    case 1: y = std::max(-1.0, std::min(1.0, x * drive)); break;
                    case 2: {
                        // folded back at +-1, repeatedly
                        double t = x * drive * 0.25 + 0.25;
                        t -= std::floor(t);
                        y = 4 * std::fabs(t - 0.5) - 1;
                        break;
                    }
                    case 3:
                        if (take) hold_[c] = std::round(x * levels) / levels;
                        y = hold_[c];
                        break;
                    default: y = std::tanh(x * drive); break;
                }
                lp_[c] += a * (y - lp_[c]);
                io[c][k] = (float)(x * dry + lp_[c] * wet);
            }
        }
    }
};

// ------------------------------------------------------------------------------------------ Vibrato
class Vibrato : public BuiltinAudio {
public:
    Vibrato()
        : BuiltinAudio({{"Rate", 0.1, 20, 5, "Hz", {}, 2}, {"Depth", 0, 200, 40, "cents", {}, 0}, {"Waveform", 0, 2, 0, "", {"Sine", "Triangle", "Square"}, 0},
                        {"Stereo phase", 0, 180, 0, "degrees", {}, 0}, {"Output gain", -60, 24, 0, "dB", {}, 1}},
                       {{"Gentle", {5, 20}}, {"Opera", {6, 60}}, {"Warped tape", {0.6, 120, 1}}, {"Seasick", {1.5, 200}}, {"Trill", {8, 100, 2}}})
    {
    }

protected:
    DelayLine dl_[2];
    double lfo_ = 0;
    void prepare() override { for (auto& d : dl_) d.resize((size_t)(sr_ * 0.2)); }
    void reset() override { for (auto& d : dl_) d.clear(); lfo_ = 0; }
    static double wave(int w, double ph)
    {
        ph -= std::floor(ph);
        if (w == 1) return ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph;
        if (w == 2) return ph < 0.5 ? 1 : -1;
        return std::sin(2 * kPi * ph);
    }
    void run(float* l, float* r, int n) override
    {
        const double rate = v(0), inc = rate / sr_;
        // a delay swinging by D samples at rate f shifts pitch by at most 2*pi*f*D/sr: D for the requested cents
        const double maxRatio = std::pow(2.0, v(1) / 1200.0) - 1;
        const double depth = std::min(maxRatio * sr_ / (2 * kPi * std::max(0.1, rate)), sr_ * 0.09);
        const int w = (int)std::lround(v(2));
        const double off = v(3) / 360.0;
        const float out = (float)(v(4) <= -59.9 ? 0.0 : dbToGain(v(4)));      // Output gain, applied last
        float* io[2] = {l, r};
        for (int k = 0; k < n; ++k) {
            lfo_ += inc;
            lfo_ -= std::floor(lfo_);
            for (int c = 0; c < 2; ++c) {
                dl_[c].push(io[c][k]);
                io[c][k] = dl_[c].at(2 + depth * (1 + wave(w, lfo_ + (c ? off : 0.0)))) * out;
            }
        }
    }
};

// ------------------------------------------------------------------------------------------ Amplitude Modulation
class AmpMod : public BuiltinAudio {
public:
    AmpMod()
        : BuiltinAudio({{"Rate", 0.1, 2000, 6, "Hz", {}, 2}, {"Depth", 0, 100, 100, "%", {}, 0}, {"Waveform", 0, 3, 0, "", {"Sine", "Triangle", "Square", "Sawtooth"}, 0},
                        {"Stereo pan", 0, 1, 0, "", {"Off", "On"}, 0}, {"Output", -24, 12, 0, "dB", {}, 1}},
                       {{"Tremolo", {6, 60}}, {"Helicopter", {14, 100, 2}}, {"Stutter gate", {8, 100, 2}}, {"Ring modulator", {440, 100}},
                        {"Auto pan", {1, 100, 0, 1}}})
    {
    }

protected:
    double ph_ = 0;
    void reset() override { ph_ = 0; }
    void run(float* l, float* r, int n) override
    {
        const double inc = v(0) / sr_, depth = v(1) / 100.0, out = dbToGain(v(4));
        const int w = (int)std::lround(v(2));
        const bool pan = v(3) > 0.5;
        for (int k = 0; k < n; ++k) {
            ph_ += inc;
            ph_ -= std::floor(ph_);
            double m;
            if (w == 1) m = ph_ < 0.5 ? 4 * ph_ - 1 : 3 - 4 * ph_;
            else if (w == 2) m = ph_ < 0.5 ? 1 : -1;
            else if (w == 3) m = 1 - 2 * ph_;
            else m = std::sin(2 * kPi * ph_);
            if (v(0) > 100 && !pan) {
                // fast: a ring modulator (the carrier multiplies the sound)
                const double g = (1 - depth) + depth * m;
                l[k] = (float)(l[k] * g * out);
                r[k] = (float)(r[k] * g * out);
            } else {
                const double gl = 1 - depth * 0.5 * (1 - m);
                const double gr = pan ? 1 - depth * 0.5 * (1 + m) : gl;
                l[k] = (float)(l[k] * gl * out);
                r[k] = (float)(r[k] * gr * out);
            }
        }
    }
};

// ------------------------------------------------------------------------------------------ Simple Delay
class SimpleDelay : public BuiltinAudio {
public:
    SimpleDelay()
        : BuiltinAudio({{"Dry out", -60, 0, 0, "dB", {}, 1}, {"Delay out", -60, 0, -6, "dB", {}, 1}, {"Delay time", 1, 2000, 300, "ms", {}, 0},
                        {"Feedback", 0, 95, 30, "%", {}, 0}, {"Ping-pong", 0, 1, 0, "", {"Off", "On"}, 0}},
                       {{"Slap back", {0, -6, 90, 0}}, {"Echo canyon", {0, -4, 450, 55}}, {"Ping-pong", {0, -6, 250, 50, 1}}, {"Infinite", {0, -3, 600, 95}}})
    {
    }

protected:
    DelayLine dl_[2];
    void prepare() override { for (auto& d : dl_) d.resize((size_t)(sr_ * 2.1)); }
    void reset() override { for (auto& d : dl_) d.clear(); }
    void run(float* l, float* r, int n) override
    {
        const float dry = (float)(v(0) <= -59.9 ? 0.0 : dbToGain(v(0))), wet = (float)(v(1) <= -59.9 ? 0.0 : dbToGain(v(1)));
        const double d = std::max(1.0, v(2) / 1000.0 * sr_);
        const float fb = (float)(v(3) / 100.0);
        const bool pp = v(4) > 0.5;
        for (int k = 0; k < n; ++k) {
            const float el = dl_[0].at(d), er = dl_[1].at(d);
            const float xl = l[k], xr = r[k];
            if (pp) {
                dl_[0].push((xl + xr) * 0.5f + er * fb);
                dl_[1].push(el * fb);
            } else {
                dl_[0].push(xl + el * fb);
                dl_[1].push(xr + er * fb);
            }
            l[k] = xl * dry + el * wet;
            r[k] = xr * dry + er * wet;
        }
    }
};

// ------------------------------------------------------------------------------------------ Reverb
// Schroeder-Moorer: eight damped combs and four all-passes per side (Freeverb's tunings).
class Reverb : public BuiltinAudio {
public:
    Reverb()
        : BuiltinAudio({{"Room size", 0, 100, 60, "%", {}, 0}, {"Damping", 0, 100, 50, "%", {}, 0}, {"Width", 0, 100, 100, "%", {}, 0},
                        {"Dry out", -60, 0, 0, "dB", {}, 1}, {"Reverb out", -60, 0, -9, "dB", {}, 1}},
                       {{"Small room", {35, 60, 80, 0, -12}}, {"Hall", {80, 40, 100, 0, -8}}, {"Cathedral", {95, 25, 100, -3, -4}},
                        {"Only the reverb", {85, 40, 100, -60, 0}}})
    {
    }

protected:
    struct Comb { DelayLine d; double store = 0; };
    Comb comb_[2][8];
    DelayLine ap_[2][4];
    void prepare() override
    {
        static const int combs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617}, aps[4] = {556, 441, 341, 225};
        const double s = sr_ / 44100.0;
        for (int c = 0; c < 2; ++c) {
            for (int k = 0; k < 8; ++k) comb_[c][k].d.resize((size_t)((combs[k] + (c ? 23 : 0)) * s) + 2);
            for (int k = 0; k < 4; ++k) ap_[c][k].resize((size_t)((aps[k] + (c ? 23 : 0)) * s) + 2);
        }
    }
    void reset() override
    {
        for (auto& side : comb_) for (auto& cb : side) { cb.d.clear(); cb.store = 0; }
        for (auto& side : ap_) for (auto& a : side) a.clear();
    }
    void run(float* l, float* r, int n) override
    {
        const double room = 0.7 + 0.28 * v(0) / 100.0, damp = 0.4 * v(1) / 100.0, width = v(2) / 100.0;
        const float dry = (float)(v(3) <= -59.9 ? 0.0 : dbToGain(v(3))), wet = (float)(v(4) <= -59.9 ? 0.0 : dbToGain(v(4)));
        const double w1 = width / 2 + 0.5, w2 = (1 - width) / 2;
        for (int k = 0; k < n; ++k) {
            const double in = (l[k] + r[k]) * 0.015;
            double out[2];
            for (int c = 0; c < 2; ++c) {
                double s = 0;
                for (Comb& cb : comb_[c]) {
                    const double y = cb.d.at((double)cb.d.buf.size() - 2);
                    cb.store = y * (1 - damp) + cb.store * damp;
                    cb.d.push((float)(in + cb.store * room));
                    s += y;
                }
                for (DelayLine& a : ap_[c]) {
                    const double b = a.at((double)a.buf.size() - 2);
                    a.push((float)(s + b * 0.5));
                    s = b - s;
                }
                out[c] = s;
            }
            const float yl = (float)(out[0] * w1 + out[1] * w2), yr = (float)(out[1] * w1 + out[0] * w2);
            l[k] = l[k] * dry + yl * wet;
            r[k] = r[k] * dry + yr * wet;
        }
    }
};


// ------------------------------------------------------------------------------------------ Modern Equalizer
// VEGAS Pro 23's Modern Equalizer: up to eight bands (high pass, low pass, low shelf, high shelf, band pass,
// peaking), each on the stereo pair, left, right, mid or side signal, plus global Scale, Mono Below, Stereo Width and
// Output Gain. Parameter i of band b is b * 7 + i: Enabled, Type, Frequency, Gain, Q, Channel, Slope. Only the
// settings are VEGAS's; the filters are ordinary biquads.
const int kEqBands = 8, kEqPer = 7;
std::vector<Param> modernEqParams()
{
    static std::deque<std::string> names;       // the parameters point at these
    if (names.empty())
        for (int b = 1; b <= kEqBands; ++b)
            for (const char* n : {"Enabled", "Type", "Frequency", "Gain", "Q", "Channel", "Slope"}) names.push_back("Band " + std::to_string(b) + " " + n);
    const double defFreq[kEqBands] = {60, 150, 400, 1000, 2500, 6000, 10000, 16000};
    std::vector<Param> ps;
    for (int b = 0; b < kEqBands; ++b) {
        const auto nm = [&](int i) -> const std::string& { return names[(size_t)(b * kEqPer + i)]; };
        ps.push_back({nm(0).c_str(), 0, 1, 0, "", {"Off", "On"}, 0});
        ps.push_back({nm(1).c_str(), 0, 5, 5, "", {"High Pass", "Low Pass", "Low Shelving", "High Shelving", "Band Pass", "Peaking"}, 0});
        ps.push_back({nm(2).c_str(), 20, 20000, defFreq[b], "Hz", {}, 0});
        ps.push_back({nm(3).c_str(), -24, 24, 0, "dB", {}, 1});
        ps.push_back({nm(4).c_str(), 0.1, 18, 0.71, "", {}, 2});
        ps.push_back({nm(5).c_str(), 0, 4, 0, "", {"Stereo", "Left", "Right", "Mid", "Side"}, 0});
        ps.push_back({nm(6).c_str(), 0, 5, 1, "", {"6 dB/oct", "12 dB/oct", "18 dB/oct", "24 dB/oct", "30 dB/oct", "36 dB/oct"}, 0});
    }
    ps.push_back({"Global Scale", -100, 200, 100, "%", {}, 0});
    ps.push_back({"Mono Below", 0, 500, 0, "Hz", {}, 0});
    ps.push_back({"Stereo Width", 0, 200, 100, "%", {}, 0});
    ps.push_back({"Output Gain", -24, 24, 0, "dB", {}, 1});
    return ps;
}
// A preset: (parameter index, value) pairs on top of the defaults.
std::vector<double> eqPreset(std::initializer_list<std::pair<int, double>> set)
{
    std::vector<Param> ps = modernEqParams();
    std::vector<double> v;
    for (const Param& p : ps) v.push_back(p.def);
    for (const auto& kv : set) v[(size_t)kv.first] = kv.second;
    return v;
}
std::vector<Preset> modernEqPresets()
{
    // band b's Enabled, Type, Frequency, Gain, Q, Channel are b*7 + 0..5
    return {{"Flat", eqPreset({})},
            {"Voice: low cut", eqPreset({{0, 1}, {1, 0}, {2, 80}, {5, 0}})},
            {"Voice: presence", eqPreset({{0, 1}, {1, 0}, {2, 80}, {7, 1}, {8, 5}, {9, 3000}, {10, 4}, {11, 1.0}})},
            {"Bass boost", eqPreset({{0, 1}, {1, 2}, {2, 100}, {3, 5}})},
            {"Bright", eqPreset({{0, 1}, {1, 3}, {2, 8000}, {3, 4}})},
            {"Telephone", eqPreset({{0, 1}, {1, 0}, {2, 400}, {1 * 7 + 0, 1}, {1 * 7 + 1, 1}, {1 * 7 + 2, 3400}})},
            {"Wider highs (side)", eqPreset({{0, 1}, {1, 3}, {2, 8000}, {3, 4}, {5, 4}})},
            {"Tighten lows (mono below)", eqPreset({{56 + 1, 120}})}};
}

class ModernEq : public BuiltinAudio {
public:
    ModernEq() : BuiltinAudio(modernEqParams(), modernEqPresets()) {}

protected:
    struct Bq {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {0, 0}, z2[2] = {0, 0};          // per signal: 0 is left or mid, 1 is right or side
        double run(int c, double x)
        {
            const double y = b0 * x + z1[c];
            z1[c] = b1 * x - a1 * y + z2[c];
            z2[c] = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    };
    Bq bq_[kEqBands][6];
    Bq side_;                       // the Mono Below high pass on the side signal
    void reset() override
    {
        for (auto& band : bq_) for (Bq& b : band) b.clear();
        side_.clear();
    }
    static void coefficients(Bq& q, int type, double freq, double gainDb, double Q, double sr)
    {
        freq = std::max(10.0, std::min(freq, sr * 0.45));
        Q = std::max(0.05, Q);
        const double w0 = 2 * kPi * freq / sr, cs = std::cos(w0), sn = std::sin(w0), A = std::pow(10.0, gainDb / 40.0);
        const double alpha = sn / (2 * Q), sa = 2 * std::sqrt(A) * (sn / 2 * std::sqrt(2.0));
        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (type) {
        case 0: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
        case 1: b0 = (1 - cs) / 2; b1 = 1 - cs; b2 = (1 - cs) / 2; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
        case 2:
            b0 = A * ((A + 1) - (A - 1) * cs + sa); b1 = 2 * A * ((A - 1) - (A + 1) * cs); b2 = A * ((A + 1) - (A - 1) * cs - sa);
            a0 = (A + 1) + (A - 1) * cs + sa; a1 = -2 * ((A - 1) + (A + 1) * cs); a2 = (A + 1) + (A - 1) * cs - sa;
            break;
        case 3:
            b0 = A * ((A + 1) + (A - 1) * cs + sa); b1 = -2 * A * ((A - 1) + (A + 1) * cs); b2 = A * ((A + 1) + (A - 1) * cs - sa);
            a0 = (A + 1) - (A - 1) * cs + sa; a1 = 2 * ((A - 1) - (A + 1) * cs); a2 = (A + 1) - (A - 1) * cs - sa;
            break;
        case 4: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * cs; a2 = 1 - alpha; break;
        default: b0 = 1 + alpha * A; b1 = -2 * cs; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * cs; a2 = 1 - alpha / A; break;
        }
        q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0; q.a1 = a1 / a0; q.a2 = a2 / a0;
    }
    void run(float* l, float* r, int n) override
    {
        struct Band { bool on; int type, route, stages; };
        Band bands[kEqBands];
        const double scale = v(kEqBands * kEqPer) / 100.0;
        bool needMs = v(kEqBands * kEqPer + 1) > 0 || std::fabs(v(kEqBands * kEqPer + 2) - 100.0) > 0.01;
        for (int b = 0; b < kEqBands; ++b) {
            const int o = b * kEqPer;
            bands[b].on = v(o) >= 0.5;
            bands[b].type = (int)std::lround(v(o + 1));
            bands[b].route = (int)std::lround(v(o + 5));
            const int slope = (int)std::lround(v(o + 6));       // 6 dB/oct steps; a biquad is 12
            bands[b].stages = (bands[b].type <= 1 || bands[b].type == 4) ? std::max(1, (slope + 1 + 1) / 2) : 1;
            if (bands[b].stages > 3) bands[b].stages = 3;
            if (!bands[b].on) continue;
            if (bands[b].route >= 3) needMs = true;
            for (int st = 0; st < bands[b].stages; ++st)
                coefficients(bq_[b][st], bands[b].type, v(o + 2), v(o + 3) * scale, st == 0 ? v(o + 4) : 0.71, sr_);
        }
        const double outGain = dbToGain(v(kEqBands * kEqPer + 3)), width = v(kEqBands * kEqPer + 2) / 100.0;
        const bool mono = v(kEqBands * kEqPer + 1) > 0;
        if (mono) coefficients(side_, 0, v(kEqBands * kEqPer + 1), 0, 0.71, sr_);
        for (int k = 0; k < n; ++k) {
            double x[2] = {l[k], r[k]};
            for (int b = 0; b < kEqBands; ++b) {
                if (!bands[b].on || bands[b].route >= 3) continue;
                for (int c = 0; c < 2; ++c) {
                    if ((bands[b].route == 1 && c == 1) || (bands[b].route == 2 && c == 0)) continue;
                    for (int st = 0; st < bands[b].stages; ++st) x[c] = bq_[b][st].run(c, x[c]);
                }
            }
            if (needMs) {
                double m = (x[0] + x[1]) * 0.5, sd = (x[0] - x[1]) * 0.5;
                for (int b = 0; b < kEqBands; ++b) {
                    if (!bands[b].on || bands[b].route < 3) continue;
                    const int c = bands[b].route == 3 ? 0 : 1;
                    double& sig = c == 0 ? m : sd;
                    for (int st = 0; st < bands[b].stages; ++st) sig = bq_[b][st].run(c, sig);
                }
                if (mono) sd = side_.run(1, sd);
                sd *= width;
                x[0] = m + sd;
                x[1] = m - sd;
            }
            l[k] = (float)(x[0] * outGain);
            r[k] = (float)(x[1] * outGain);
        }
    }
};

struct Kind {
    const char* uid;
    const char* name;
    std::unique_ptr<AudioPlugin> (*make)();
};
template <class T> std::unique_ptr<AudioPlugin> makeOne() { return std::unique_ptr<AudioPlugin>(new T()); }

const Kind kKinds[] = {
    {"reno.pitchshift", "Pitch Shift", makeOne<PitchShift>},
    {"reno.chorus", "Chorus", makeOne<Chorus>},
    {"reno.flangewah", "Flange/Wah-wah", makeOne<FlangeWah>},
    {"reno.distortion", "Distortion", makeOne<Distortion>},
    {"reno.vibrato", "Vibrato", makeOne<Vibrato>},
    {"reno.ampmod", "Amplitude Modulation", makeOne<AmpMod>},
    {"reno.simpledelay", "Simple Delay", makeOne<SimpleDelay>},
    {"reno.reverb", "Reverb", makeOne<Reverb>},
    {"reno.moderneq", "Modern Equalizer", makeOne<ModernEq>},
};

}  // namespace

const char* const kBuiltinAudioVendor = "Reno (built-in)";

std::vector<AudioPluginDesc> builtinAudioPlugins()
{
    std::vector<AudioPluginDesc> out;
    for (const Kind& k : kKinds) {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = k.uid;
        d.name = k.name;
        d.vendor = kBuiltinAudioVendor;
        d.numInputs = d.numOutputs = 2;
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<AudioPlugin> builtinAudioCreate(const AudioPluginDesc& desc, std::string* error)
{
    for (const Kind& k : kKinds)
        if (desc.uid == k.uid) {
            std::unique_ptr<AudioPlugin> p = k.make();
            p->desc = desc;
            if (p->desc.name.empty()) p->desc.name = k.name;
            p->desc.vendor = kBuiltinAudioVendor;
            return p;
        }
    if (error) *error = "Reno has no built-in audio effect " + desc.uid;
    return nullptr;
}

}  // namespace reno
