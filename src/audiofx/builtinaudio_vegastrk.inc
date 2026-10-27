// Reno's versions of the effects VEGAS puts on every track and bus: Volume, Pan, Track Noise Gate, Track Compressor,
// Dither and Track EQ. Included by builtinaudio.cpp inside its unnamed namespace, after the base class. The settings,
// presets, saved forms and the processing are in vegas_*.h (worked out from VEGAS's sffrgpnv_x64.dll and
// sftrkfx1_x64.dll); a setting saved by an older Reno ("reno-builtin-audio") loads as the defaults.

// Shared by these effects: setting a value to exactly what a saved form says (the 0..1 steps of paramSet() cannot hit
// every float).
class TrkEffect : public BuiltinAudio {
public:
    using BuiltinAudio::BuiltinAudio;

protected:
    void setReal(int i, double x)
    {
        double lo = 0, hi = 1;
        paramRange(i, &lo, &hi);
        setRaw(i, std::max(lo, std::min(hi, x)));
    }
};

// ------------------------------------------------------------------------------------------ Volume
// One gain, 0 to 2 (the dialog's trackbar goes 0..2000 thousandths; a typed dB value can go above, to 2000), kept as
// the plug-in keeps it: a float, multiplied into every sample.
class TrkVolume : public BuiltinAudio {
public:
    TrkVolume() : BuiltinAudio({{"Volume", 0, 2, 1, "", {}, 3}}, {}) {}

    std::string paramText(int i) override
    {
        if (i != 0) return std::string();
        const float g = gain_.load();
        if (!(g > 0.f)) return "-Inf.";
        const double db = 20.0 * std::log10((double)g);
        if (db <= -60.0) return "-Inf.";
        char b[32];
        std::snprintf(b, sizeof b, "%.1f dB", db);
        return b;
    }
    float paramGet(int i) override { return i == 0 ? std::min(1.f, gain_.load() / 2.f) : 0.f; }
    void paramSet(int i, float v) override
    {
        if (i == 0 && std::isfinite(v)) gain_ = std::max(0.f, std::min(1.f, v)) * 2.f;
    }
    std::string saveState() override { return vegforge::encode(vegforge::volume(), gain_.load()); }
    bool loadState(const std::string& st) override
    {
        float g;
        if (vegforge::decode(vegforge::volume(), st, &g)) {
            gain_ = g;
            program_ = -1;
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {      // an older Reno's settings: the default
            gain_ = 1.f;
            program_ = 0;
            return true;
        }
        return false;
    }
    int programCount() override { return 2; }
    std::string programName(int i) override { return i >= 0 && i < 2 ? vegforge::presetName(i) : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= 2) return false;
        gain_ = vegforge::volume().neutral;
        program_ = i;
        return true;
    }

protected:
    void run(float* l, float* r, int n) override { vegforge::applyVolume(l, r, n, gain_.load()); }

private:
    std::atomic<float> gain_{1.f};
    int program_ = 0;
};

// ------------------------------------------------------------------------------------------ Pan
// One setting, -1 to 1: from 0 up the right channel is turned down by (1 - pan) and what is taken goes to the left;
// below 0 the left is turned down by (1 + pan) and goes to the right (so +1 is all left).
class TrkPan : public TrkEffect {
public:
    TrkPan() : TrkEffect({{"Pan", -1, 1, 0, "", {}, 3}}, {}) {}

    std::string paramText(int i) override
    {
        if (i != 0) return std::string();
        const double p = v(0) * 100.0;
        char b[48];
        if (p > 0) std::snprintf(b, sizeof b, "Left: %.1f %%", p);
        else if (p < 0) std::snprintf(b, sizeof b, "Right: %.1f %%", -p);
        else std::snprintf(b, sizeof b, "Center");
        return b;
    }
    std::string saveState() override { return vegforge::encode(vegforge::pan(), (float)v(0)); }
    bool loadState(const std::string& st) override
    {
        float p;
        if (vegforge::decode(vegforge::pan(), st, &p)) {
            setReal(0, p);
            program_ = -1;
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {      // an older Reno's settings: the default
            setReal(0, 0);
            program_ = 0;
            return true;
        }
        return false;
    }
    int programCount() override { return 2; }
    std::string programName(int i) override { return i >= 0 && i < 2 ? vegforge::presetName(i) : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= 2) return false;
        setReal(0, vegforge::pan().neutral);
        program_ = i;
        return true;
    }

protected:
    void run(float* l, float* r, int n) override { vegforge::applyPan(l, r, n, (float)v(0)); }

private:
    int program_ = 0;
};

// ------------------------------------------------------------------------------------------ Track Noise Gate
// Attack and release in ms and the threshold in tenths of a dB (-80.0 is "-Inf.": the gate does nothing), as the plug-in
// keeps them; veggate::Gate is its code. A change of settings while the sound runs keeps the gate's state, as in the DLL.
class TrkGate : public TrkEffect {
public:
    TrkGate()
        : TrkEffect({{"Threshold Level", -80, 0, -80, "dB", {}, 1}, {"Attack Time", 0, 500, 3, "ms", {}, 0}, {"Release Time", 1, 5000, 100, "ms", {}, 0},
                     {"Bypass", 0, 1, 0, "", {"Off", "On"}, 0}},
                    {})
    {
        applyAll(veggate::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if (i == 0 && v(0) <= -79.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    std::string saveState() override { return veggate::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        veggate::Settings s;
        if (veggate::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {      // an older Reno's settings: the default
            applyAll(veggate::presets()[0].s);
            program_ = 0;
            return true;
        }
        return false;
    }
    int programCount() override { return (int)veggate::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? veggate::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(veggate::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { started_ = false; }
    void reset() override { started_ = false; }
    void run(float* l, float* r, int n) override
    {
        const veggate::Settings s = settings();
        if (!started_) {
            gate_.prepare(s, sr_, false);
            gate_.start(sr_);
            started_ = true;
            used_ = s;
        } else if (std::memcmp(&s, &used_, sizeof s) != 0) {
            gate_.prepare(s, sr_, true);
            used_ = s;
        }
        if (gate_.bypass) return;
        // the DLL takes the samples as frames of all channels: the loudest of the pair opens or closes the gate for both
        for (int k = 0; k < n; ++k) {
            float f[2] = {l[k], r[k]};
            gate_.frame(f, 2);
            l[k] = f[0];
            r[k] = f[1];
        }
    }

private:
    veggate::Gate gate_;
    veggate::Settings used_;
    bool started_ = false;
    int program_ = 0;

    veggate::Settings settings()
    {
        veggate::Settings s;
        s.threshold = (int)std::lround(v(0) * 10);
        s.attack = (int)std::lround(v(1));
        s.release = (int)std::lround(v(2));
        s.bypass = (int)std::lround(v(3));
        veggate::sanitize(s);
        return s;
    }
    void applyAll(const veggate::Settings& in)
    {
        veggate::Settings s = in;
        veggate::sanitize(s);
        setReal(0, s.threshold / 10.0);
        setReal(1, s.attack);
        setReal(2, s.release);
        setReal(3, s.bypass);
        program_ = -1;
    }
};
// ------------------------------------------------------------------------------------------ Track Compressor
// Input and output gain and threshold in dB, the amount (x:1), attack and release in ms and two switches, as the plug-in keeps
// them; vegcomp::Comp is its code. (The plug-in also saves a bypass flag, which has no setting of its own: it is kept through
// loading and saving.) A change of settings while the sound runs keeps the running gain, as in the DLL.
class TrkComp : public TrkEffect {
public:
    TrkComp()
        : TrkEffect({{"Input Gain", -60, 25, 0, "dB", {}, 1}, {"Output Gain", -60, 25, 0, "dB", {}, 1}, {"Threshold", -60, 0, 0, "dB", {}, 1},
                     {"Amount", 1, 50, 1, ":1", {}, 1}, {"Attack Time", 0, 500, 15, "ms", {}, 1}, {"Release Time", 0, 5000, 250, "ms", {}, 1},
                     {"Automatic Gain Compensation", 0, 1, 1, "", {"Off", "On"}, 0}, {"Smooth Saturation", 0, 1, 0, "", {"Off", "On"}, 0}},
                    {})
    {
        applyAll(vegcomp::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if (i <= 1 && v(i) <= -59.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    std::string saveState() override { return vegcomp::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegcomp::Settings s;
        if (vegcomp::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {      // an older Reno's settings: the default
            applyAll(vegcomp::presets()[0].s);
            program_ = 0;
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegcomp::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegcomp::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(vegcomp::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { started_ = false; }
    void reset() override { started_ = false; }
    void run(float* l, float* r, int n) override
    {
        const vegcomp::Settings s = settings();
        if (!started_) {
            comp_.prepare(s, sr_, false);
            started_ = true;
            used_ = s;
        } else if (std::memcmp(&s, &used_, sizeof s) != 0) {
            comp_.prepare(s, sr_, true);
            used_ = s;
        }
        float pin[2] = {0.f, 0.f};
        for (int k = 0; k < n; ++k) {
            pin[0] = std::max(pin[0], std::fabs(l[k]));
            pin[1] = std::max(pin[1], std::fabs(r[k]));
        }
        comp_.process(l, r, n);
        float pout[2] = {0.f, 0.f};
        for (int k = 0; k < n; ++k) {
            pout[0] = std::max(pout[0], std::fabs(l[k]));
            pout[1] = std::max(pout[1], std::fabs(r[k]));
        }
        const float lv[4] = {pin[0], pin[1], pout[0], pout[1]};
        for (int k = 0; k < 4; ++k) {
            float old = meter_[k].load();
            while (lv[k] > old && !meter_[k].compare_exchange_weak(old, lv[k])) {}
        }
        // the compression: the gain the sound got against the gain it gets below the threshold
        if (!comp_.neutral && !comp_.bypass && comp_.g > 0.0 && comp_.c218 > 0.0) {
            const float red = (float)std::min(1.0, comp_.g / comp_.c218);
            float old = meter_[4].load();
            while (red < old && !meter_[4].compare_exchange_weak(old, red)) {}
        }
    }

public:
    // meters: 0, 1 the input's loudest left and right sample, 2, 3 the output's, 4 the smallest compression gain (1 = none)
    int meterCount() override { return 5; }
    float meterTake(int i) override
    {
        if (i < 0 || i >= 5) return 0.f;
        if (i < 4) return meter_[i].exchange(0.f);
        const float g = meter_[4].exchange(2.f);
        return g > 1.5f ? 1.f : g;
    }

private:
    std::atomic<float> meter_[5] = {{0.f}, {0.f}, {0.f}, {0.f}, {2.f}};
    vegcomp::Comp comp_;
    vegcomp::Settings used_;
    bool started_ = false;
    int program_ = 0;
    int bypass_ = 0;

    vegcomp::Settings settings()
    {
        vegcomp::Settings s;
        s.inputGain = (float)v(0);
        s.outputGain = (float)v(1);
        s.threshold = (float)v(2);
        s.amount = (float)v(3);
        s.attack = (float)v(4);
        s.release = (float)v(5);
        s.autoGain = (int)std::lround(v(6));
        s.smooth = (int)std::lround(v(7));
        s.bypass = bypass_;
        vegcomp::sanitize(s);
        return s;
    }
    void applyAll(const vegcomp::Settings& in)
    {
        vegcomp::Settings s = in;
        vegcomp::sanitize(s);
        setReal(0, s.inputGain);
        setReal(1, s.outputGain);
        setReal(2, s.threshold);
        setReal(3, s.amount);
        setReal(4, s.attack);
        setReal(5, s.release);
        setReal(6, s.autoGain);
        setReal(7, s.smooth);
        bypass_ = s.bypass;
        program_ = -1;
    }
};
// ------------------------------------------------------------------------------------------ Dither
// Word length, noise type and noise shaping, picked from lists in the plug-in's page (it has no automatable settings: all
// three live in its saved form). vegdither::Dither is the DLL's code, with its random generator; where the DLL seeds that
// from the clock, this one starts from the same seed every time, so a render repeats.
class TrkDither : public BuiltinAudio {
public:
    TrkDither() : BuiltinAudio({}, {}) {}

    std::string saveState() override { return vegdither::encode(unpack(packed_.load())); }
    bool loadState(const std::string& st) override
    {
        vegdither::Settings s;
        if (vegdither::decode(st, &s)) {
            packed_ = pack(s);
            program_ = -1;
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {      // an older Reno's settings: the default
            packed_ = pack(vegdither::presets()[0].s);
            program_ = 0;
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegdither::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegdither::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        packed_ = pack(vegdither::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { started_ = false; }
    void reset() override { started_ = false; }
    void run(float* l, float* r, int n) override
    {
        const uint32_t now = packed_.load();
        if (!started_ || now != used_) {
            dither_.prepare(unpack(now), 12345u);
            started_ = true;
            used_ = now;
        }
        float* ch[2] = {l, r};
        dither_.process(ch, 2, n);
    }

private:
    vegdither::Dither dither_;
    std::atomic<uint32_t> packed_{pack(vegdither::presets()[0].s)};
    uint32_t used_ = 0;
    bool started_ = false;
    int program_ = 0;

    static uint32_t pack(const vegdither::Settings& in)
    {
        vegdither::Settings s = in;
        vegdither::sanitize(s);
        return (uint32_t)s.depth | ((uint32_t)s.type << 8) | ((uint32_t)s.shape << 16);
    }
    static vegdither::Settings unpack(uint32_t p)
    {
        vegdither::Settings s;
        s.depth = (int32_t)(p & 0xff);
        s.type = (int32_t)((p >> 8) & 0xff);
        s.shape = (int32_t)((p >> 16) & 0xff);
        return s;
    }
};

// ------------------------------------------------------------------------------------------ Track EQ
// Four bands (low shelf, band or high shelf) with frequency, gain, bandwidth, roll-off and an enable each, and the six
// channel switches the plug-in saves (VEGAS's own code ignores them for a stereo stream, and so does this); vegeq::Eq is the
// DLL's filter. A change of settings while the sound runs keeps what the filters hold, as in the DLL.
class TrkEq : public TrkEffect {
public:
    TrkEq() : TrkEffect(paramList(), {})
    {
        applyAll(vegeq::presets()[0].s);
        program_ = 0;
    }

    std::string saveState() override { return vegeq::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegeq::Settings s;
        if (vegeq::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {      // an older Reno's settings: the default
            applyAll(vegeq::presets()[0].s);
            program_ = 0;
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegeq::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegeq::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(vegeq::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { started_ = false; }
    void reset() override { started_ = false; }
    void run(float* l, float* r, int n) override
    {
        const vegeq::Settings s = settings();
        if (!started_) {
            eq_ = vegeq::Eq();      // the DLL's start: every section empty
            eq_.prepare(s, sr_, false);
            started_ = true;
            used_ = s;
        } else if (std::memcmp(&s, &used_, sizeof s) != 0) {
            eq_.prepare(s, sr_, true);
            used_ = s;
        }
        eq_.process(l, r, n);
    }

private:
    vegeq::Eq eq_;
    vegeq::Settings used_;
    bool started_ = false;
    int program_ = 0;
    int selected_ = 0, unused_ = 0;

    static std::vector<Param> paramList()
    {
        static std::vector<std::string> names;
        if (names.empty()) {
            const char* kinds[6] = {"Frequency", "Gain", "Bandwidth", "Rolloff", "Mode", "Enable"};
            for (int k = 0; k < 6; ++k)
                for (int b = 1; b <= 4; ++b) names.push_back("Band " + std::to_string(b) + " " + kinds[k]);
            for (const char* c : {"Left Enable", "Right Enable", "Center Enable", "Left Surround Enable", "Right Surround Enable", "LFE Enable"}) names.push_back(c);
        }
        double def[vegeq::kParams];
        vegeq::toParams(vegeq::presets()[0].s, def);
        std::vector<Param> p;
        for (int i = 0; i < vegeq::kParams; ++i) {
            const int kind = i / 4;
            const char* n = names[(size_t)i].c_str();
            if (i >= 24) p.push_back({n, 0, 1, 1, "", {"Off", "On"}, 0});
            else if (kind == 0) p.push_back({n, 20, 20000, def[i], "Hz", {}, 0});
            else if (kind == 1) p.push_back({n, -25, 15, def[i], "dB", {}, 1});      // (the dialog stops at -15; some presets save -25: the same full cut)
            else if (kind == 2) p.push_back({n, 0.1, 4, def[i], "octaves", {}, 1});
            else if (kind == 3) p.push_back({n, 1, 4, def[i], "dB/octave", {}, 0});
            else if (kind == 4) p.push_back({n, 0, 2, def[i], "", {"Low Shelf", "Band", "High Shelf"}, 0});
            else p.push_back({n, 0, 1, def[i], "", {"Off", "On"}, 0});
        }
        return p;
    }
    vegeq::Settings settings()
    {
        double x[vegeq::kParams];
        for (int i = 0; i < vegeq::kParams; ++i) x[i] = v(i);
        return vegeq::fromParams(x, selected_, unused_);
    }
    void applyAll(const vegeq::Settings& in)
    {
        double x[vegeq::kParams];
        vegeq::toParams(in, x);
        for (int i = 0; i < vegeq::kParams; ++i) setReal(i, x[i]);
        selected_ = std::max(0, std::min(3, (int)in.selected));
        unused_ = in.unused;
        program_ = -1;
    }
};

#define VEGASTRK_KINDS \
    {"reno.trkvolume", "Volume", makeOne<TrkVolume>}, \
    {"reno.trkpan", "Pan", makeOne<TrkPan>}, \
    {"reno.trkgate", "Track Noise Gate", makeOne<TrkGate>}, \
    {"reno.trkcomp", "Track Compressor", makeOne<TrkComp>}, \
    {"reno.trkdither", "Dither", makeOne<TrkDither>}, \
    {"reno.trkeq", "Track EQ", makeOne<TrkEq>}, \
    /* (the end of the list) */
