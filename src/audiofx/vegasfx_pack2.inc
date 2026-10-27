// The reproductions of VEGAS's sfppack2 / sfresfilter effects (included by builtinaudio.cpp inside its anonymous
// namespace, after the BuiltinAudio base class). Each one has its settings, saved form and presets in its own
// vegas<name>.h, so this file is only the glue to the plug-in interface.

// ------------------------------------------------------------------------------------------ Noise Gate
// VEGAS's Noise Gate (block gate: see vegasnoisegate.h). It collects 20 ms blocks before it outputs them, so the
// sound comes out one block late (latencySamples) and the first block is silent.
class NoiseGate : public BuiltinAudio {
public:
    NoiseGate()
        : BuiltinAudio({{"Attack time", 1, 500, 3, "ms", {}, 0}, {"Release time", 1, 5000, 100, "ms", {}, 0},
                        {"Threshold level", -80, 0, -20, "dB", {}, 1}},
                       {})
    {
        applyAll(vegng::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if (i == 2 && v(2) <= -79.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    std::string saveState() override { return vegng::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegng::Settings s;
        if (vegng::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {     // older Reno: other settings; start from VEGAS's defaults
            applyAll(vegng::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegng::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegng::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(vegng::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }
    int latencySamples() override { return dsp_.latency(); }

protected:
    void prepare() override
    {
        dsp_.setSettings(settings());
        dsp_.setFormat(sr_, 2);
        ready_[0].assign((size_t)dsp_.block(), 0.f);
        ready_[1].assign((size_t)dsp_.block(), 0.f);
        at_ = 0;
    }
    void reset() override
    {
        dsp_.restart();
        std::fill(ready_[0].begin(), ready_[0].end(), 0.f);
        std::fill(ready_[1].begin(), ready_[1].end(), 0.f);
        at_ = 0;
    }
    void run(float* l, float* r, int n) override
    {
        dsp_.setSettings(settings());
        dsp_.applyChanges();
        const int block = dsp_.block();
        for (int k = 0; k < n; ++k) {
            const float in[2] = {l[k], r[k]};
            l[k] = ready_[0][(size_t)at_];
            r[k] = ready_[1][(size_t)at_];
            if (dsp_.push(in)) {
                for (int i = 0; i < block; ++i) {
                    ready_[0][(size_t)i] = dsp_.outAt(0, i);
                    ready_[1][(size_t)i] = dsp_.outAt(1, i);
                }
            }
            if (++at_ >= block) at_ = 0;
        }
    }

private:
    vegng::Dsp dsp_;
    std::vector<float> ready_[2];       // the last complete block, output while the next one is collected
    int at_ = 0;
    int program_ = 0;

    vegng::Settings settings() const
    {
        vegng::Settings s;
        s.attack = (int)std::lround(v(0));
        s.release = (int)std::lround(v(1));
        s.threshold = (int)std::lround(v(2) * 10);
        return s;
    }
    void applyAll(const vegng::Settings& in)
    {
        vegng::Settings s = in;
        vegng::sanitize(s);
        auto setReal = [this](int i, double x) {
            double lo = 0, hi = 1;
            paramRange(i, &lo, &hi);
            paramSet(i, (float)((x - lo) / (hi - lo)));
        };
        setReal(0, s.attack);
        setReal(1, s.release);
        setReal(2, s.threshold / 10.0);
        program_ = -1;
    }
};


// ------------------------------------------------------------------------------------------ Graphic EQ
// VEGAS's Graphic EQ (an FIR filter designed from a graph, see vegasgraphiceq.h). The sound comes out half a filter
// size late (latencySamples: 128, 512 or 2048 frames by the accuracy setting) and the first block is silent. The
// settings are the accuracy and the output gain (the two parameters) and the envelope and band faders (the graph part
// of the saved form, edited by the page).
class GraphicEq : public BuiltinAudio {
public:
    GraphicEq()
        : BuiltinAudio({{"Accuracy", 0, 2, 1, "", {"Low (fast, good for high bands)", "Medium (great for mid and high bands)", "High (slow, great for all bands)"}, 0},
                        {"Output gain", -60, 20, 0, "dB", {}, 1}},
                       {})
    {
        applyAll(veggeq::presets()[0].s);
        program_ = 0;
    }

    std::string saveState() override { return veggeq::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        veggeq::Settings s;
        if (veggeq::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {     // older Reno: other settings; start from VEGAS's defaults
            applyAll(veggeq::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)veggeq::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? veggeq::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(veggeq::presets()[(size_t)i].s);       // the preset has the accuracy and the output gain too
        program_ = i;
        return true;
    }
    int latencySamples() override { return veggeq::filterSize((int)std::lround(v(0))) / 2; }

protected:
    void prepare() override
    {
        dsp_.setSettings(settings());
        dsp_.setFormat(sr_, 2);
        sizeBuffers();
    }
    void reset() override
    {
        dsp_.restart();
        std::fill(ready_[0].begin(), ready_[0].end(), 0.f);
        std::fill(ready_[1].begin(), ready_[1].end(), 0.f);
        at_ = 0;
    }
    void run(float* l, float* r, int n) override
    {
        dsp_.setSettings(settings());
        dsp_.applyChanges();
        if (dsp_.block() != (int)ready_[0].size()) sizeBuffers();      // a new accuracy: the filter starts again
        const int block = dsp_.block();
        for (int k = 0; k < n; ++k) {
            const float in[2] = {l[k], r[k]};
            l[k] = ready_[0][(size_t)at_];
            r[k] = ready_[1][(size_t)at_];
            if (dsp_.push(in)) {
                for (int i = 0; i < block; ++i) {
                    ready_[0][(size_t)i] = dsp_.outAt(0, i);
                    ready_[1][(size_t)i] = dsp_.outAt(1, i);
                }
            }
            if (++at_ >= block) at_ = 0;
        }
    }

private:
    veggeq::Dsp dsp_;
    std::vector<float> ready_[2];       // the last complete block, output while the next one is collected
    int at_ = 0;
    int program_ = 0;
    std::mutex graphMutex_;
    veggeq::Settings graph_;             // the envelope and the band faders (the accuracy and the output gain are parameters)

    void sizeBuffers()
    {
        ready_[0].assign((size_t)dsp_.block(), 0.f);
        ready_[1].assign((size_t)dsp_.block(), 0.f);
        at_ = 0;
    }
    veggeq::Settings settings()
    {
        veggeq::Settings s;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            s = graph_;
        }
        s.accuracy = (int)std::lround(v(0));
        s.outGain = (int)std::lround(v(1) * 10);
        veggeq::sanitize(s);
        return s;
    }
    void applyAll(const veggeq::Settings& in)
    {
        veggeq::Settings s = in;
        veggeq::sanitize(s);
        auto setReal = [this](int i, double x) {
            double lo = 0, hi = 1;
            paramRange(i, &lo, &hi);
            paramSet(i, (float)((x - lo) / (hi - lo)));
        };
        setReal(0, s.accuracy);
        setReal(1, s.outGain / 10.0);
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            graph_ = s;
        }
        program_ = -1;
    }
};

// ------------------------------------------------------------------------------------------ FIR equalizers
// The base of the sfppack2 equalizers that use the FIR engine (vegasfir.h): the sound comes out one block late (a block
// is half the filter size: latencySamples) and the first block is silent. The effect says how its settings become a
// design (wantDesign). As in the plug-in's engine, the settings are only looked at when 5 filter sizes of sound have
// gone through since the last time (so a change takes up to that long to be heard), except for a new accuracy, which
// starts the filter again at once.
class FirEffect : public BuiltinAudio {
public:
    using BuiltinAudio::BuiltinAudio;
    int latencySamples() override { return veggeq::filterSize(have_ ? haveAccuracy_ : wantAccuracy()) / 2; }

protected:
    // the settings now as a design: its accuracy (0..2), a key that changes with any setting, and the design itself
    virtual void wantDesign(int* accuracy, std::vector<int32_t>* key, vegfir::Design* design) = 0;
    int wantAccuracy()
    {
        int a = 1;
        std::vector<int32_t> k;
        vegfir::Design d;
        wantDesign(&a, &k, &d);
        return a;
    }
    void prepare() override
    {
        have_ = false;
        take();
        dsp_.setFormat(sr_, 2);
        sizeBuffers();
    }
    void reset() override
    {
        dsp_.restart();
        std::fill(ready_[0].begin(), ready_[0].end(), 0.f);
        std::fill(ready_[1].begin(), ready_[1].end(), 0.f);
        at_ = 0;
        take();
    }
    void run(float* l, float* r, int n) override
    {
        int acc = 1;
        std::vector<int32_t> key;
        vegfir::Design design;
        wantDesign(&acc, &key, &design);
        if (!have_ || acc != haveAccuracy_ || since_ >= 5 * veggeq::filterSize(haveAccuracy_)) {
            dsp_.setDesign(acc, key, design);
            have_ = true;
            haveAccuracy_ = acc;
            since_ = 0;
        }
        since_ += n;
        dsp_.applyChanges();
        if (dsp_.block() != (int)ready_[0].size()) sizeBuffers();      // a new accuracy: the filter starts again
        const int block = dsp_.block();
        for (int k = 0; k < n; ++k) {
            const float in[2] = {l[k], r[k]};
            l[k] = ready_[0][(size_t)at_];
            r[k] = ready_[1][(size_t)at_];
            if (dsp_.push(in)) {
                for (int i = 0; i < block; ++i) {
                    ready_[0][(size_t)i] = dsp_.outAt(0, i);
                    ready_[1][(size_t)i] = dsp_.outAt(1, i);
                }
            }
            if (++at_ >= block) at_ = 0;
        }
    }
    vegfir::Dsp dsp_;
    int program_ = 0;
    // sets the parameter whose range is lo..hi to the real value x
    void setReal(int i, double x)
    {
        double lo = 0, hi = 1;
        paramRange(i, &lo, &hi);
        paramSet(i, (float)((x - lo) / (hi - lo)));
    }

private:
    std::vector<float> ready_[2];       // the last complete block, output while the next one is collected
    int at_ = 0;
    bool have_ = false;                 // the engine has a design (and haveAccuracy_ is its accuracy)
    int haveAccuracy_ = 1;
    long since_ = 0;                    // frames since the settings were last looked at
    void take()
    {
        int acc = 1;
        std::vector<int32_t> key;
        vegfir::Design design;
        wantDesign(&acc, &key, &design);
        dsp_.setDesign(acc, key, design);
        have_ = true;
        haveAccuracy_ = acc;
        since_ = 0;
    }
    void sizeBuffers()
    {
        ready_[0].assign((size_t)dsp_.block(), 0.f);
        ready_[1].assign((size_t)dsp_.block(), 0.f);
        at_ = 0;
    }
};

// ------------------------------------------------------------------------------------------ Parametric EQ
// VEGAS's Parametric EQ (one filter: shelf, band-pass or band-notch/boost, see vegaspmeq.h). The parameters are the
// plug-in's six settings in its order.
class ParametricEq : public FirEffect {
public:
    ParametricEq()
        : FirEffect({{"Filter style", 0, 3, 2, "", {"Low-frequency shelf", "High-frequency shelf", "Band-pass", "Band-notch/boost"}, 0},
                     {"Frequency", 20, 22050, 650, "Hz", {}, 0},
                     {"Width", 0.1, 5, 1, "oct.", {}, 1},
                     {"Amount", -60, 20, -4, "dB", {}, 1},
                     {"Output gain", -60, 20, 0, "dB", {}, 1},
                     {"Accuracy", 0, 2, 1, "", {"Low (Fast, good for high frequencies)", "Medium (great for mid and high frequencies)", "High (Slow, great for all frequencies)"}, 0}},
                    {})
    {
        applyAll(vegpm::presets()[0].s);
        program_ = 0;
    }

    std::string saveState() override { return vegpm::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegpm::Settings s;
        if (vegpm::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {     // older Reno: other settings; start from VEGAS's defaults
            applyAll(vegpm::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegpm::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegpm::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(vegpm::presets()[(size_t)i].s);       // the preset has the accuracy and the output gain too
        program_ = i;
        return true;
    }

protected:
    void wantDesign(int* accuracy, std::vector<int32_t>* key, vegfir::Design* design) override
    {
        const vegpm::Settings s = settings();
        *accuracy = s.accuracy;
        *key = {s.style, s.freq, s.width, s.amount, s.outGain, s.accuracy};
        *design = [s](double* m, int n, int rate) { return vegpm::design(m, n, rate, s); };
    }

private:
    vegpm::Settings settings() const
    {
        vegpm::Settings s;
        s.style = (int)std::lround(v(0));
        s.freq = (int)std::lround(v(1));
        s.width = (int)std::lround(v(2) * 10);
        s.amount = (int)std::lround(v(3) * 10);
        s.outGain = (int)std::lround(v(4) * 10);
        s.accuracy = (int)std::lround(v(5));
        vegpm::sanitize(s);
        return s;
    }
    void applyAll(const vegpm::Settings& in)
    {
        vegpm::Settings s = in;
        vegpm::sanitize(s);
        setReal(0, s.style);
        setReal(1, s.freq);
        setReal(2, s.width / 10.0);
        setReal(3, s.amount / 10.0);
        setReal(4, s.outGain / 10.0);
        setReal(5, s.accuracy);
        program_ = -1;
    }
};

// ------------------------------------------------------------------------------------------ Paragraphic EQ
// VEGAS's Paragraphic EQ (four band filters, a low and a high shelf and a dry/wet mix, see vegaspgeq.h): recursive
// filters, frame by frame, no delay. The parameters are the plug-in's settings in its order (its last setting, which the
// sound does not use, is kept for the saved form).
class ParagraphicEq : public BuiltinAudio {
public:
    ParagraphicEq()
        : BuiltinAudio(makeParams(), {})
    {
        applyAll(vegpg::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if ((i == 0 || i == 18 || i == 19) && v(i) <= -24.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    std::string saveState() override { return vegpg::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegpg::Settings s;
        if (vegpg::decode(st, &s)) {
            applyAll(s);
            return true;
        }
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {     // older Reno: other settings; start from VEGAS's defaults
            applyAll(vegpg::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegpg::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegpg::presets()[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(vegpg::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override
    {
        dsp_.setRate((int)(sr_ + 0.5));
        dsp_.setSettings(settings());
        dsp_.restart();
    }
    void reset() override { dsp_.restart(); }
    void run(float* l, float* r, int n) override
    {
        dsp_.setSettings(settings());
        dsp_.process(l, r, n);
    }

private:
    vegpg::Dsp dsp_;
    int program_ = 0;
    std::atomic<int> extra_{4};             // the plug-in's last setting (0..4), only kept

    static std::vector<Param> makeParams()
    {
        static const char* const gains[4] = {"Band 1 gain", "Band 2 gain", "Band 3 gain", "Band 4 gain"};
        static const char* const widths[4] = {"Band 1 width", "Band 2 width", "Band 3 width", "Band 4 width"};
        static const char* const freqs[4] = {"Band 1 center frequency", "Band 2 center frequency", "Band 3 center frequency", "Band 4 center frequency"};
        static const double def[4] = {100, 300, 1000, 5000};
        std::vector<Param> p;
        p.push_back({"Dry out", -25, 25, -25, "dB", {}, 1});
        p.push_back({"Wet out", -25, 25, 0, "dB", {}, 1});
        for (int k = 0; k < 4; ++k) p.push_back({gains[k], -25, 25, 0, "dB", {}, 1});
        for (int k = 0; k < 4; ++k) p.push_back({widths[k], 0.3, 2.5, 1, "oct.", {}, 1});
        for (int k = 0; k < 4; ++k) p.push_back({freqs[k], 20, 15000, def[k], "Hz", {}, 0});
        p.push_back({"Low shelf enable", 0, 1, 0, "", {"Off", "On"}, 0});
        p.push_back({"High shelf enable", 0, 1, 0, "", {"Off", "On"}, 0});
        p.push_back({"Low shelf frequency", 20, 15000, 300, "Hz", {}, 0});
        p.push_back({"High shelf frequency", 20, 20000, 500, "Hz", {}, 0});
        p.push_back({"Low shelf gain", -25, 25, 0, "dB", {}, 1});
        p.push_back({"High shelf gain", -25, 25, 0, "dB", {}, 1});
        return p;
    }
    vegpg::Settings settings() const
    {
        vegpg::Settings s;
        s.dry = (int)std::lround(v(0) * 10);
        s.wet = (int)std::lround(v(1) * 10);
        for (int k = 0; k < 4; ++k) {
            s.gain[k] = (int)std::lround(v(2 + k) * 10);
            s.width[k] = (int)std::lround(v(6 + k) * 10);
            s.freq[k] = (int)std::lround(v(10 + k));
        }
        s.lowOn = (int)std::lround(v(14));
        s.highOn = (int)std::lround(v(15));
        s.lowFreq = (int)std::lround(v(16));
        s.highFreq = (int)std::lround(v(17));
        s.lowGain = (int)std::lround(v(18) * 10);
        s.highGain = (int)std::lround(v(19) * 10);
        s.extra = extra_.load();
        vegpg::sanitize(s);
        return s;
    }
    void applyAll(const vegpg::Settings& in)
    {
        vegpg::Settings s = in;
        vegpg::sanitize(s);
        auto setReal = [this](int i, double x) {
            double lo = 0, hi = 1;
            paramRange(i, &lo, &hi);
            paramSet(i, (float)((x - lo) / (hi - lo)));
        };
        setReal(0, s.dry / 10.0);
        setReal(1, s.wet / 10.0);
        for (int k = 0; k < 4; ++k) {
            setReal(2 + k, s.gain[k] / 10.0);
            setReal(6 + k, s.width[k] / 10.0);
            setReal(10 + k, s.freq[k]);
        }
        setReal(14, s.lowOn);
        setReal(15, s.highOn);
        setReal(16, s.lowFreq);
        setReal(17, s.highFreq);
        setReal(18, s.lowGain / 10.0);
        setReal(19, s.highGain / 10.0);
        extra_.store(s.extra);
        program_ = -1;
    }
};

// the uids the built-in effects list takes from this file (one line each, continued with a backslash)
#define RENO_PACK2_KINDS \
    {"reno.graphiceq", "Graphic EQ", makeOne<GraphicEq>}, \
    {"reno.parametriceq", "Parametric EQ", makeOne<ParametricEq>}, \
    {"reno.paragraphiceq", "Paragraphic EQ", makeOne<ParagraphicEq>},
