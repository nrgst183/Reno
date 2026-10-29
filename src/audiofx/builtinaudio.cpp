// Reno's built-in audio effects: stand-ins for common VEGAS DirectX effects (Pitch
// Shift, Chorus, Flange/Wah-wah, Distortion, Vibrato, Amplitude Modulation, Simple Delay, Reverb), with VEGAS's
// names and similar settings, for machines where VEGAS's own are missing. They are ordinary audio plug-ins to the
// rest of Reno (format "BUILTIN"): the effects window shows their sliders and presets and the project stores
// their settings. The sound is Reno's own.
#include "audioplugin.h"
#include "vegasdistortion.h"
#include "vegaspack1.h"
#include "vegas_trkfx.h"
#include "vegaspack3_all.h"
#include "vegasfx_pack2.h"          // the sfppack2 / sfresfilter reproductions
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <memory>
#include <mutex>
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
    void setRaw(int i, double x) { if (valid(i) && std::isfinite(x)) values_[i] = (float)x; }       // exactly x, not through 0..1
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

// ------------------------------------------------------------------------------------------ Distortion
// VEGAS's Distortion, worked out from sfppack3_x64.dll: two transfer graphs (one for samples >= 0, one for the rest)
// become gain tables in 0.1 dB steps, then a slew limit, a one-pole low-pass and the dry/wet mix. Same settings and
// saved form as VEGAS's (vegasdistortion.h).
class Distortion : public BuiltinAudio {
public:
    Distortion()
        : BuiltinAudio({{"Slew Rate Enable", 0, 1, 0, "", {"Off", "On"}, 0}, {"Slew Rate", 1, 100, 100, "", {}, 0},
                        {"Low-pass Filter Enable", 0, 1, 0, "", {"Off", "On"}, 0}, {"Low-pass Filter Frequency", 100, 10000, 400, "Hz", {}, 0},
                        {"Dry Out", -60, 0, -60, "dB", {}, 1}, {"Wet Out", -60, 24, 0, "dB", {}, 1}},
                       {})
    {
        applyAll(vegdist::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if ((i == 4 || i == 5) && v(i) <= -59.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    // VEGAS keeps these as whole numbers (dB in tenths)
    void paramSet(int i, float v) override
    {
        double lo = 0, hi = 1;
        if (!paramRange(i, &lo, &hi) || !std::isfinite(v)) return;
        double x = lo + std::max(0.f, std::min(1.f, v)) * (hi - lo);
        x = (i == 4 || i == 5) ? std::round(x * 10) / 10 : std::round(x);
        BuiltinAudio::paramSet(i, (float)((x - lo) / (hi - lo)));
    }
    std::string saveState() override { return vegdist::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegdist::Settings s;
        if (vegdist::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        // settings saved by older Reno versions (other parameters): start from VEGAS's defaults
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {
            applyAll(vegdist::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegdist::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegdist::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(vegdist::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { applied_ = false; }
    void reset() override
    {
        s1_ = s2_ = 0;
        for (double& x : lp_) x = 0;
    }
    void run(float* l, float* r, int n) override
    {
        update();
        // VEGAS gets the samples interleaved; its slew state is shared by the channels, so it runs L, R, L, R...
        // (a NaN or infinite input would index the gain table out of range and poison the state: it counts as silence)
        for (int k = 0; k < n; ++k) {
            l[k] = (float)sample(std::isfinite(l[k]) ? (double)l[k] : 0.0, 0);
            r[k] = (float)sample(std::isfinite(r[k]) ? (double)r[k] : 0.0, 1);
        }
    }

private:
    std::mutex graphMutex_;
    vegdist::Settings graph_;           // the graphs, polarity mode and the settings that are not parameters
    std::atomic<int> graphVersion_{1};
    int program_ = 0;

    // processing state (audio thread)
    bool applied_ = false;
    int usedVersion_ = 0;
    int used_[6] = {};
    double sr_used_ = 0;
    double tpos_[841] = {}, tneg_[841] = {};
    double dry_ = 0, wet_ = 1, maxDelta_ = 0, a_ = 0, b_ = 1;
    bool slewOn_ = false;
    double s1_ = 0, s2_ = 0;
    double lp_[2] = {0, 0};

    vegdist::Settings settings()
    {
        vegdist::Settings s;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            s = graph_;
        }
        s.slewOn = (int)std::lround(v(0));
        s.slewRate = (int)std::lround(v(1));
        s.lpOn = (int)std::lround(v(2));
        s.lpFreq = (int)std::lround(v(3));
        s.dry = (int)std::lround(v(4) * 10);
        s.wet = (int)std::lround(v(5) * 10);
        return s;
    }
    void applyAll(const vegdist::Settings& in)
    {
        vegdist::Settings s = in;
        vegdist::sanitize(s);
        auto setReal = [this](int i, double x) {
            double lo = 0, hi = 1;
            paramRange(i, &lo, &hi);
            paramSet(i, (float)((x - lo) / (hi - lo)));
        };
        setReal(0, s.slewOn);
        setReal(1, s.slewRate);
        setReal(2, s.lpOn);
        setReal(3, s.lpFreq);
        setReal(4, s.dry / 10.0);
        setReal(5, s.wet / 10.0);
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            graph_ = s;
        }
        ++graphVersion_;
        program_ = -1;
    }

    // One graph as VEGAS turns it into gains: entry j is the gain at -j/10 dB in, in tenths of a dB, between the
    // points by straight lines; then made linear (exactly -84 dB: silence).
    static void buildTable(double* t, const vegdist::Point* p, int n)
    {
        for (int j = 0; j < 841; ++j) t[j] = 0;
        auto fill = [t](int a, int b) {
            if (a > b) std::swap(a, b);
            if (b - a == 0) return;
            const double slope = (t[b] - t[a]) / (double)(b - a);
            for (int k = a + 1; k < b; ++k) t[k] = (double)(k - a) * slope + t[a];
        };
        for (int i = 0; i + 1 < n; ++i) {
            const vegdist::Point p0 = p[i], p1 = p[i + 1];
            t[-p0.x] = (double)(p0.y - p0.x);
            t[-p1.x] = (double)(p1.y - p1.x);
            if (p1.y == vegdist::kInf && p0.y == vegdist::kInf) {
                t[-p1.x] = vegdist::kInf;
                t[-p0.x] = vegdist::kInf;
            }
            if (i == 0 && p[0].y == vegdist::kInf && t[-p1.x] < -200.0) t[-p0.x] = vegdist::kInf;
            fill(-p1.x, -p0.x);
        }
        for (int j = 0; j < 841; ++j) t[j] = t[j] == -840.0 ? 0.0 : std::pow(10.0, t[j] / 200.0);
    }

    void update()
    {
        const int ver = graphVersion_.load();
        const int now[6] = {(int)std::lround(v(0)), (int)std::lround(v(1)), (int)std::lround(v(2)), (int)std::lround(v(3)),
                            (int)std::lround(v(4) * 10), (int)std::lround(v(5) * 10)};
        const bool paramsSame = applied_ && sr_used_ == sr_ && std::equal(now, now + 6, used_);
        if (paramsSame && ver == usedVersion_) return;
        if (ver != usedVersion_ || !applied_) {
            std::unique_lock<std::mutex> lk(graphMutex_, std::try_to_lock);
            if (lk.owns_lock()) {
                buildTable(tpos_, graph_.pos, graph_.posCount);
                buildTable(tneg_, graph_.neg, graph_.negCount);
                usedVersion_ = ver;
            } else if (applied_ && paramsSame) {
                return;     // the page is writing the graph: next block
            }
        }
        std::copy(now, now + 6, used_);
        sr_used_ = sr_;
        slewOn_ = now[0] != 0;
        maxDelta_ = (double)now[1] * 0.0005;
        dry_ = now[4] <= -600 ? 0.0 : std::pow(10.0, (double)now[4] / 200.0);
        wet_ = now[5] <= -600 ? 0.0 : std::pow(10.0, (double)now[5] / 200.0);
        const int rate = (int)std::lround(sr_);
        if (now[2] && now[3] <= rate / 2) {
            a_ = std::exp((double)now[3] * -6.28318530718 / (double)rate);
            b_ = 1.0 - a_;
        } else {
            a_ = 0;
            b_ = 1;
        }
        applied_ = true;
    }

    // the gain for x from table t, in a line between its 0.1 dB entries
    static double lookup(double x, const double* t)
    {
        const double a = std::fabs(x);
        double pos;
        if (a < 3.051758e-05) pos = -840.0;
        else {
            const double l = std::log10(a) * 200.0;
            pos = l < -840.0 ? -840.0 : (l > 0.0 ? 0.0 : l);
        }
        const int i = (int)pos;
        const double t0 = t[-i];
        if (pos > -840.0) return (t[-i + 1] - t0) * ((double)i - pos) + t0;
        return t0;
    }

    double sample(double x, int ch)
    {
        const double dryPart = x * dry_;
        const double g = lookup(x, x >= 0.0 ? tpos_ : tneg_);
        double d = x * g - s1_;
        const double prev = s1_ - s2_;
        if (slewOn_) {
            const double lo = prev - maxDelta_, hi = maxDelta_ + prev;
            if (d < lo) d = lo;
            else if (d > hi) d = hi;
        }
        s2_ = s1_;
        const double y = s1_ + d;
        s1_ = y;
        double o = y * b_ + lp_[ch] * a_;
        if (std::fabs(o) < 2e-11) o = 0;
        lp_[ch] = o;
        return o * wet_ + dryPart;
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

// ---- Reno's versions of the other sfppack3_x64.dll effects (Amplitude Modulation, Flange/Wah-wah, Gapper/Snipper, Smooth/Enhance, Vibrato)
#include "builtin_pack3.inc"
// VEGAS sfppack2 / sfresfilter reproductions
#include "vegasfx_pack2.inc"

struct Kind {
    const char* uid;
    const char* name;
    std::unique_ptr<AudioPlugin> (*make)();
};
template <class T> std::unique_ptr<AudioPlugin> makeOne() { return std::unique_ptr<AudioPlugin>(new T()); }

#include "builtinaudio_vegastrk.inc"

const Kind kKinds[] = {
    {"reno.flangewah", "Flange/Wah-wah", makeOne<Flange>},
    {"reno.distortion", "Distortion", makeOne<Distortion>},
    {"reno.vibrato", "Vibrato", makeOne<Vibrato>},
    {"reno.ampmod", "Amplitude Modulation", makeOne<AmpMod>},
    {"reno.gapsnip", "Gapper/Snipper", makeOne<GapperSnipper>},
    {"reno.smooth", "Smooth/Enhance", makeOne<SmoothEnhance>},
    {"reno.moderneq", "Modern Equalizer", makeOne<ModernEq>},
    VEGASTRK_KINDS
    {"reno.noisegate", "Noise Gate", makeOne<NoiseGate>},
    RENO_PACK2_KINDS          // the other sfppack2 / sfresfilter reproductions (vegasfx_pack2.inc)
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
    // the effects of VEGAS's sfppack1 (vegaspack1.cpp): Simple Delay...
    for (const vegpack1::Info* i : vegpack1::allInfos()) {
        AudioPluginDesc d;
        d.format = "BUILTIN";
        d.uid = i->uid;
        d.name = i->name;
        d.vendor = kBuiltinAudioVendor;
        d.numInputs = d.numOutputs = 2;
        out.push_back(d);
    }
    return out;
}

std::unique_ptr<AudioPlugin> builtinAudioCreate(const AudioPluginDesc& desc, std::string* error)
{
    if (std::unique_ptr<AudioPlugin> p = vegpack1::create(desc.uid)) {
        p->desc = desc;
        if (p->desc.name.empty()) p->desc.name = vegpack1::findInfo(desc.uid)->name;
        p->desc.vendor = kBuiltinAudioVendor;
        return p;
    }
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
