// Reno's versions of the sfppack3_x64.dll effects other than Distortion: Amplitude Modulation, Flange/Wah-wah,
// Gapper/Snipper, Smooth/Enhance and Vibrato. This file is part of builtinaudio.cpp (it is included inside its
// anonymous namespace, after BuiltinAudio). Each effect has its settings, saved form, presets and sound in
// vegas<name>.h; the classes here give them the plug-in interface: parameters (the effect's own automatable
// ones, in its order, with its names), the VEGAS saved form, VEGAS's presets.

// What the effects below share: setting a parameter to a real value, the current preset.
class Pack3Effect : public BuiltinAudio {
public:
    using BuiltinAudio::BuiltinAudio;
    int currentProgram() override { return program_; }
    // the block's place in the stream is what tempo sync needs (run() does not get it)
    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        pos_ = samplePos;
        BuiltinAudio::process(left, right, frames, samplePos, playing);
    }

protected:
    int program_ = 0;
    int64_t pos_ = 0;
    void setReal(int i, double x)
    {
        double lo = 0, hi = 1;
        paramRange(i, &lo, &hi);
        paramSet(i, (float)((x - lo) / (hi - lo)));
    }
    int intValue(int i) const { return (int)std::lround(v(i)); }
    // older Reno versions saved their stand-ins' settings as text: those start from the defaults
    static bool isOldState(const std::string& st) { return st.compare(0, 18, "reno-builtin-audio") == 0; }
};

// ------------------------------------------------------------------------------------------ Smooth/Enhance
// One parameter, Operation (-5 smooth .. 5 enhance); the sound is vegassmooth.h's one-pole filter.
class SmoothEnhance : public Pack3Effect {
public:
    SmoothEnhance() : Pack3Effect({{"Operation", -5, 5, 0, "", {}, 0}}, {}) {}

    std::string saveState() override
    {
        vegsmooth::Settings s;
        s.operation = intValue(0);
        s.bypass = bypass_;
        return vegsmooth::encode(s);
    }
    bool loadState(const std::string& st) override
    {
        vegsmooth::Settings s;
        if (vegsmooth::decode(st, &s)) {
            apply(s);
            return true;
        }
        if (isOldState(st)) {
            apply(vegsmooth::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegsmooth::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegsmooth::presets()[(size_t)i].name : std::string(); }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        apply(vegsmooth::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { setUp_ = false; }
    void reset() override { eng_.reset(); }
    void run(float* l, float* r, int n) override
    {
        if (bypass_) return;
        const int op = std::max(-5, std::min(5, intValue(0)));
        // the plug-in sets itself up again, and so clears its filters, when its settings change
        if (!setUp_ || op != eng_.operation()) {
            eng_.setup(op);
            setUp_ = true;
        }
        eng_.process(l, r, n);
    }

private:
    vegsmooth::Engine eng_;
    bool setUp_ = false;
    std::atomic<int> bypass_{0};

    void apply(const vegsmooth::Settings& in)
    {
        vegsmooth::Settings s = in;
        vegsmooth::sanitize(s);
        setReal(0, s.operation);
        bypass_ = s.bypass;
        program_ = -1;
    }
};

// ------------------------------------------------------------------------------------------ Gapper/Snipper
// The plug-in cycles: the sound passes for one period with faded edges, then Gapper inserts silence of the event's length
// (delaying the sound by it) or Snipper cuts that much out of it (vegasgapper.h). So the plug-in's output is longer or
// shorter than its input. Reno's effect chain gives and takes blocks of the same size, so the sound goes through a queue:
// the output block is the next samples of what the plug-in has delivered (silence where it has delivered less so far, as
// Snipper does: it needs more input than it gives output).
class GapperSnipper : public Pack3Effect {
public:
    GapperSnipper()
        : Pack3Effect({{"Effect", 0, 1, 0, "", {"Gapper (insert)", "Snipper (cut)"}, 0}, {"Frequency", 0.0001, 1000, 20, "Hz", {}, 4},
                       {"Length", 0.0001, 1, 0.075, "s", {}, 4}, {"Fade edges", 0, 50, 15, "%", {}, 0}},
                      {})
    {
    }

    std::string saveState() override { return veggap::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        veggap::Settings s;
        if (veggap::decode(st, &s)) {
            apply(s);
            return true;
        }
        if (isOldState(st)) {
            apply(veggap::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)veggap::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? veggap::presets()[(size_t)i].name : std::string(); }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        apply(veggap::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { used_ = false; }
    void reset() override
    {
        queueL_.clear();
        queueR_.clear();
        queueAt_ = 0;
        eng_.setup(settings(), (int)std::lround(sr_), true);
        used_ = true;
        usedSettings_ = settings();
    }
    void run(float* l, float* r, int n) override
    {
        const veggap::Settings now = settings();
        // the plug-in sets itself up again when its settings change, and carries on with the cycle it is in
        if (!used_ || std::memcmp(&now, &usedSettings_, sizeof now) != 0) {
            eng_.setup(now, (int)std::lround(sr_), !used_);
            used_ = true;
            usedSettings_ = now;
        }
        // feed the block; what the plug-in delivers goes to the queue
        const int cap = std::max(n, 4096);
        outL_.resize((size_t)cap);
        outR_.resize((size_t)cap);
        int at = 0;
        while (at < n) {
            const float* in[2] = {l + at, r + at};
            float* out[2] = {outL_.data(), outR_.data()};
            int took = 0, gave = 0;
            eng_.process(in, n - at, out, cap, &took, &gave);
            queueL_.insert(queueL_.end(), outL_.begin(), outL_.begin() + gave);
            queueR_.insert(queueR_.end(), outR_.begin(), outR_.begin() + gave);
            if (took == 0 && gave == 0) break;      // cannot happen; the plug-in always moves on
            at += took;
        }
        // the block: the oldest samples in the queue, silence for what is missing
        const size_t have = queueL_.size() - queueAt_;
        const int m = (int)std::min<size_t>(have, (size_t)n);
        for (int k = 0; k < m; ++k) {
            l[k] = queueL_[queueAt_ + (size_t)k];
            r[k] = queueR_[queueAt_ + (size_t)k];
        }
        for (int k = m; k < n; ++k) l[k] = r[k] = 0.f;
        queueAt_ += (size_t)m;
        // keep the queue short: drop what has been played, and what is more than half a minute behind (a Gapper that runs for long)
        if (queueAt_ > 65536 || queueAt_ == queueL_.size()) {
            queueL_.erase(queueL_.begin(), queueL_.begin() + (long)queueAt_);
            queueR_.erase(queueR_.begin(), queueR_.begin() + (long)queueAt_);
            queueAt_ = 0;
        }
        const size_t most = (size_t)(30 * sr_);
        if (queueL_.size() - queueAt_ > most) queueAt_ = queueL_.size() - most;
    }

private:
    veggap::Engine eng_;
    veggap::Settings usedSettings_;
    bool used_ = false;
    std::vector<float> queueL_, queueR_, outL_, outR_;
    size_t queueAt_ = 0;

    veggap::Settings settings()
    {
        veggap::Settings s;
        s.mode = intValue(0);
        s.frequency = (int)std::lround(v(1) * 10000.0);
        s.length = (int)std::lround(v(2) * 10000.0);
        s.fade = intValue(3);
        veggap::sanitize(s);
        return s;
    }
    void apply(const veggap::Settings& in)
    {
        veggap::Settings s = in;
        veggap::sanitize(s);
        setReal(0, s.mode);
        setReal(1, s.frequency / 10000.0);
        setReal(2, s.length / 10000.0);
        setReal(3, s.fade);
        program_ = -1;
    }
};

// ------------------------------------------------------------------------------------------ Vibrato
// The plug-in's own parameters (Semitones, Modulation Frequency, Blend Graph Edges, Output Gain) and, besides them, the graph
// (what the page edits; it is only in the saved form). The sound is vegasvibrato.h's engine. It plays the sound back at a
// changing speed, so what it delivers is longer or shorter than what it is given; as with Gapper/Snipper the sound goes
// through a queue that gives back the oldest samples, silence where less has been delivered. The plug-in reads one sample
// ahead (the last sample of a block comes out with the next one): the queue starts with one silent sample for it, so the
// blocks that follow are not broken.
class Vibrato : public Pack3Effect {
public:
    Vibrato()
        : Pack3Effect({{"Semitones", 0.01, 24, 12, "", {}, 2}, {"Modulation Frequency", 0.001, 1000, 1, "Hz", {}, 3},
                       {"Blend Graph Edges", 0, 1, 0, "", {"Off", "On"}, 0}, {"Output Gain", -80, 24, 0, "dB", {}, 1}},
                      {})
    {
        apply(vegvib::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if (i == 3 && v(3) <= -79.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    // VEGAS keeps these as whole numbers (semitones in hundredths, Hz in thousandths, dB in tenths): a setting is the
    // nearest of them, so what is set is what is saved and what an undo brings back
    void paramSet(int i, float v) override
    {
        double lo = 0, hi = 1;
        if (!paramRange(i, &lo, &hi) || !std::isfinite(v)) return;
        double x = lo + std::max(0.f, std::min(1.f, v)) * (hi - lo);
        static const double kStep[4] = {100.0, 1000.0, 1.0, 10.0};
        if (i >= 0 && i < 4 && i != 2) x = std::round(x * kStep[i]) / kStep[i];
        BuiltinAudio::paramSet(i, (float)((x - lo) / (hi - lo)));
    }
    std::string saveState() override { return vegvib::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegvib::Settings s;
        if (vegvib::decode(st, &s)) {
            apply(s);
            return true;
        }
        if (isOldState(st)) {
            apply(vegvib::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegvib::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegvib::presets()[(size_t)i].name : std::string(); }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        apply(vegvib::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { formatSet_ = false; }
    void reset() override
    {
        // a jump in time is a discontinuity for the plug-in: it starts again
        mode_ = 0;
        queueL_.assign(1, 0.f);
        queueR_.assign(1, 0.f);
        queueAt_ = 0;
    }
    void run(float* l, float* r, int n) override
    {
        if (bypass_) return;        // the plug-in's bypass: the sound goes through as it is
        const vegvib::Settings now = settings();
        if (!formatSet_) {
            eng_.setSettings(now);
            eng_.setFormat((int)std::lround(sr_));
            formatSet_ = true;
            usedSettings_ = now;
        } else if (std::memcmp(&now, &usedSettings_, sizeof now) != 0) {
            eng_.setSettings(now);
            usedSettings_ = now;
        }
        // the plug-in works on interleaved samples; at the speed of 4 (24 semitones up) a block gives a quarter of the frames, at a quarter 4 times
        const int cap = 4 * n + 16;
        in_.resize((size_t)n * 2);
        out_.resize((size_t)cap * 2);
        for (int k = 0; k < n; ++k) {
            in_[(size_t)k * 2] = l[k];
            in_[(size_t)k * 2 + 1] = r[k];
        }
        int at = 0;
        while (at < n) {
            int took = n - at, gave = cap;
            eng_.process(mode_, &in_[(size_t)at * 2], &took, out_.data(), &gave);
            mode_ = 1;
            for (int k = 0; k < gave; ++k) {
                queueL_.push_back(out_[(size_t)k * 2]);
                queueR_.push_back(out_[(size_t)k * 2 + 1]);
            }
            if (took <= 0 && gave == 0) break;
            at += took;
        }
        const size_t have = queueL_.size() - queueAt_;
        const int m = (int)std::min<size_t>(have, (size_t)n);
        for (int k = 0; k < m; ++k) {
            l[k] = queueL_[queueAt_ + (size_t)k];
            r[k] = queueR_[queueAt_ + (size_t)k];
        }
        for (int k = m; k < n; ++k) l[k] = r[k] = 0.f;
        queueAt_ += (size_t)m;
        if (queueAt_ > 65536 || queueAt_ == queueL_.size()) {
            queueL_.erase(queueL_.begin(), queueL_.begin() + (long)queueAt_);
            queueR_.erase(queueR_.begin(), queueR_.begin() + (long)queueAt_);
            queueAt_ = 0;
        }
        const size_t most = (size_t)(30 * sr_);
        if (queueL_.size() - queueAt_ > most) queueAt_ = queueL_.size() - most;
    }

private:
    vegvib::Engine eng_;
    vegvib::Settings usedSettings_;
    bool formatSet_ = false;
    int mode_ = 0;
    std::vector<float> in_, out_, queueL_, queueR_;
    size_t queueAt_ = 0;
    std::mutex graphMutex_;
    vegvib::Settings graph_;            // the graph (and the parameters as last set from a saved form)
    std::atomic<int> bypass_{0};

    vegvib::Settings settings()
    {
        vegvib::Settings s;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            s = graph_;
        }
        s.semitones = (int)std::lround(v(0) * 100.0);
        s.frequency = (int)std::lround(v(1) * 1000.0);
        s.blendEdges = intValue(2);
        s.gain = v(3) <= -79.95 ? -800 : (int)std::lround(v(3) * 10.0);
        s.bypass = bypass_;
        vegvib::sanitize(s);
        return s;
    }
    void apply(const vegvib::Settings& in)
    {
        vegvib::Settings s = in;
        vegvib::sanitize(s);
        setReal(0, s.semitones / 100.0);
        setReal(1, s.frequency / 1000.0);
        setReal(2, s.blendEdges);
        setReal(3, s.gain / 10.0);
        bypass_ = s.bypass;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            graph_ = s;
        }
        program_ = -1;
    }
};

// ------------------------------------------------------------------------------------------ Amplitude Modulation
// The plug-in's own parameters in its order (Amplitude, Modulation Frequency, Stereo Pan, Low-pass Filter Enable, Low-pass
// Filter Frequency, Blend Graph Edges, Dry Out, Wet Out, Modulation Period, Modulation Period Multiplier, Tempo Sync) and,
// besides them, the graph (what the page edits; it is only in the saved form). The sound is vegasampmod.h's engine, which works
// in place. Tempo sync follows the project's tempo in VEGAS; Reno's plug-in interface has none, so it is 120 beats per minute
// in 4/4 (as the other tempo-synced effects here), and the cycle's place comes from the position in the stream.
class AmpMod : public Pack3Effect {
public:
    AmpMod()
        : Pack3Effect({{"Amplitude", -80, 0, -48, "dB", {}, 1}, {"Modulation Frequency", 0.001, 5000, 1, "Hz", {}, 3},
                       {"Stereo Pan", 0, 100, 0, "", {}, 0}, {"Low-pass Filter Enable", 0, 1, 0, "", {"Off", "On"}, 0},
                       {"Low-pass Filter Frequency", 100, 10000, 400, "Hz", {}, 0}, {"Blend Graph Edges", 0, 1, 0, "", {"Off", "On"}, 0},
                       {"Dry Out", -80, 0, -80, "dB", {}, 1}, {"Wet Out", -80, 24, 0, "dB", {}, 1},
                       {"Modulation Period", 0, 18, 18, "", {"64th Note Triplet", "64th Note", "Dotted 64th Note", "32nd Note Triplet", "32nd Note",
                        "Dotted 32nd Note", "16th Note Triplet", "16th Note", "Dotted 16th Note", "8th Note Triplet", "8th Note", "Dotted 8th Note",
                        "Quarter Note Triplet", "Quarter Note", "Dotted Quarter Note", "Half Note Triplet", "Half Note", "Dotted Half Note", "Measures"}, 0},
                       {"Modulation Period Multiplier", 0.001, 32, 1, "", {}, 3}, {"Tempo Sync", 0, 1, 0, "", {"Off", "On"}, 0}},
                      {})
    {
        apply(vegamp::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if ((i == 6 || i == 7) && v(i) <= -79.95) return "-Inf.";
        return BuiltinAudio::paramText(i);
    }
    std::string saveState() override { return vegamp::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegamp::Settings s;
        if (vegamp::decode(st, &s)) {
            apply(s);
            return true;
        }
        if (isOldState(st)) {
            apply(vegamp::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegamp::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegamp::presets()[(size_t)i].name : std::string(); }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        apply(vegamp::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { formatSet_ = false; }
    void reset() override { formatSet_ = false; }      // a jump in time: the plug-in starts again (with tempo sync, at the cycle's place for the position)
    void run(float* l, float* r, int n) override
    {
        const vegamp::Settings now = settings();
        if (now.bypass) return;     // the plug-in's bypass: the sound goes through as it is
        if (!formatSet_) {
            eng_.setSettings(now);
            eng_.setFormat((int)std::lround(sr_));
            if (now.tempoSync) eng_.setPosition((double)pos_);
            formatSet_ = true;
            usedSettings_ = now;
        } else if (std::memcmp(&now, &usedSettings_, sizeof now) != 0) {
            eng_.setSettings(now);
            usedSettings_ = now;
        }
        const int chunk = 4096;
        buf_.resize((size_t)chunk * 2);
        for (int at = 0; at < n; at += chunk) {
            int cnt = std::min(chunk, n - at);
            for (int k = 0; k < cnt; ++k) {
                buf_[(size_t)k * 2] = l[at + k];
                buf_[(size_t)k * 2 + 1] = r[at + k];
            }
            eng_.process(buf_.data(), &cnt, &tempo_);
            for (int k = 0; k < cnt; ++k) {
                l[at + k] = buf_[(size_t)k * 2];
                r[at + k] = buf_[(size_t)k * 2 + 1];
            }
        }
    }

private:
    vegamp::Engine eng_;
    vegamp::Settings usedSettings_;
    vegamp::Tempo tempo_;               // 120 beats per minute, 4/4
    bool formatSet_ = false;
    std::vector<float> buf_;
    std::mutex graphMutex_;
    vegamp::Settings graph_;            // the graph (and the parameters as last set from a saved form)
    std::atomic<int> bypass_{0};

    vegamp::Settings settings()
    {
        vegamp::Settings s;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            s = graph_;
        }
        s.amplitude = (int)std::lround(v(0) * 10.0);
        s.frequency = (int)std::lround(v(1) * 1000.0);
        s.pan = intValue(2);
        s.lowPassOn = intValue(3);
        s.lowPassFreq = intValue(4);
        s.blendEdges = intValue(5);
        s.dryOut = v(6) <= -79.95 ? -800 : (int)std::lround(v(6) * 10.0);
        s.wetOut = v(7) <= -79.95 ? -800 : (int)std::lround(v(7) * 10.0);
        s.period = intValue(8);
        s.multiplier = (int)std::lround(v(9) * 1000.0);
        s.tempoSync = intValue(10);
        s.bypass = bypass_;
        vegamp::sanitize(s);
        return s;
    }
    void apply(const vegamp::Settings& in)
    {
        vegamp::Settings s = in;
        vegamp::sanitize(s);
        setReal(0, s.amplitude / 10.0);
        setReal(1, s.frequency / 1000.0);
        setReal(2, s.pan);
        setReal(3, s.lowPassOn);
        setReal(4, s.lowPassFreq);
        setReal(5, s.blendEdges);
        setReal(6, s.dryOut / 10.0);
        setReal(7, s.wetOut / 10.0);
        setReal(8, s.period);
        setReal(9, s.multiplier / 1000.0);
        setReal(10, s.tempoSync);
        bypass_ = s.bypass;
        {
            std::lock_guard<std::mutex> lk(graphMutex_);
            graph_ = s;
        }
        program_ = -1;
    }
};

// ------------------------------------------------------------------------------------------ Flange/Wah-wah
// VEGAS's Flange/Wah-wah (vegasflange.h): Flange, Phaser or Wah-Wah, the rate a frequency or (tempo sync) a note length.
class Flange : public Pack3Effect {
public:
    Flange()
        : Pack3Effect({{"Effect", 0, 2, 0, "", {"Flange", "Phaser", "Wah-Wah"}, 0}, {"Center Frequency", 100, 5000, 100, "Hz", {}, 0},
                       {"Resonance", 1, 100, 1, "%", {}, 0}, {"Rate", 0.001, 20, 10, "Hz", {}, 3}, {"Depth", 1, 100, 10, "%", {}, 0},
                       {"Wet Out", -80, 12, 0, "dB", {}, 1}, {"Dry Out", -80, 12, -80, "dB", {}, 1},
                       {"Period", 0, 18, 18, "", {"64th Note Triplet", "64th Note", "Dotted 64th Note", "32nd Note Triplet", "32nd Note",
                        "Dotted 32nd Note", "16th Note Triplet", "16th Note", "Dotted 16th Note", "8th Note Triplet", "8th Note", "Dotted 8th Note",
                        "Quarter Note Triplet", "Quarter Note", "Dotted Quarter Note", "Half Note Triplet", "Half Note", "Dotted Half Note", "Measures"}, 0},
                       {"Period Multiplier", 0.001, 32, 1, "", {}, 3}, {"Tempo Sync", 0, 1, 0, "", {"Off", "On"}, 0}},
                      {})
    {
        apply(vegflange::presets()[0].s);
        program_ = 0;
    }

    std::string paramText(int i) override
    {
        if ((i == 5 || i == 6) && v(i) <= -79.95) return "-Inf.";
        if (i == 5 || i == 6) {
            // tenths of a dB (a value that went through the 0..1 range is a hair off)
            char b[32];
            std::snprintf(b, sizeof b, "%.1f dB", std::lround(v(i) * 10.0) / 10.0 + 0.0);
            return b;
        }
        return BuiltinAudio::paramText(i);
    }
    std::string saveState() override { return vegflange::encode(settings()); }
    bool loadState(const std::string& st) override
    {
        vegflange::Settings s;
        if (vegflange::decode(st, &s)) {
            apply(s);
            return true;
        }
        if (isOldState(st)) {
            apply(vegflange::presets()[0].s);
            return true;
        }
        return false;
    }
    int programCount() override { return (int)vegflange::presets().size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? vegflange::presets()[(size_t)i].name : std::string(); }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        apply(vegflange::presets()[(size_t)i].s);
        program_ = i;
        return true;
    }

protected:
    void prepare() override { formatSet_ = false; }
    void reset() override { formatSet_ = false; }       // a jump in time: the plug-in starts again
    void run(float* l, float* r, int n) override
    {
        const vegflange::Settings now = settings();
        if (now.bypass) return;     // the plug-in's bypass: the sound goes through as it is
        if (!formatSet_) {
            eng_.setSettings(now);
            eng_.setFormat((int)std::lround(sr_));
            formatSet_ = true;
            usedSettings_ = now;
        } else if (std::memcmp(&now, &usedSettings_, sizeof now) != 0) {
            eng_.setSettings(now);
            usedSettings_ = now;
        }
        const int chunk = 4096;
        buf_.resize((size_t)chunk * 2);
        for (int at = 0; at < n; at += chunk) {
            int cnt = std::min(chunk, n - at);
            for (int k = 0; k < cnt; ++k) {
                buf_[(size_t)k * 2] = l[at + k];
                buf_[(size_t)k * 2 + 1] = r[at + k];
            }
            eng_.process(buf_.data(), &cnt, &tempo_);
            for (int k = 0; k < cnt; ++k) {
                l[at + k] = buf_[(size_t)k * 2];
                r[at + k] = buf_[(size_t)k * 2 + 1];
            }
        }
    }

private:
    vegflange::Engine eng_;
    vegflange::Settings usedSettings_;
    vegflange::Tempo tempo_;            // 120 beats per minute, 4/4
    bool formatSet_ = false;
    std::vector<float> buf_;
    std::atomic<int> bypass_{0};

    vegflange::Settings settings() const
    {
        vegflange::Settings s;
        s.effect = intValue(0);
        s.centerFreq = intValue(1);
        s.resonance = intValue(2);
        s.rate = (int)std::lround(v(3) * 1000.0);
        s.depth = intValue(4);
        s.wetOut = v(5) <= -79.95 ? -800 : (int)std::lround(v(5) * 10.0);
        s.dryOut = v(6) <= -79.95 ? -800 : (int)std::lround(v(6) * 10.0);
        s.period = intValue(7);
        s.multiplier = (int)std::lround(v(8) * 1000.0);
        s.tempoSync = intValue(9);
        s.bypass = bypass_;
        vegflange::sanitize(s);
        return s;
    }
    void apply(const vegflange::Settings& in)
    {
        vegflange::Settings s = in;
        vegflange::sanitize(s);
        setReal(0, s.effect);
        setReal(1, s.centerFreq);
        setReal(2, s.resonance);
        setReal(3, s.rate / 1000.0);
        setReal(4, s.depth);
        setReal(5, s.wetOut / 10.0);
        setReal(6, s.dryOut / 10.0);
        setReal(7, s.period);
        setReal(8, s.multiplier / 1000.0);
        setReal(9, s.tempoSync);
        bypass_ = s.bypass;
        program_ = -1;
    }
};
