// The generic plug-in behind Reno's versions of the sfppack1 effects: see vegaspack1.h.
#include "vegaspack1.h"
#include "vegaschorus.h"
#include "vegasmultitap.h"
#include "vegaspitch.h"
#include "vegasreverb.h"
#include "vegassimpledelay.h"
#include "vegasxpdistor.h"
#include "vegasxpflange.h"
#include "vegasxpreverb.h"
#include "vegasxpstutter.h"
#include "vegasxpampmod.h"
#include "vegasxpchorus.h"
#include "vegasxpdelay.h"
#include "vegasxpeq.h"
#include "vegasxpdynamics.h"
#include "vegasxpnoisegate.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace reno {
namespace vegpack1 {

// ------------------------------------------------------------------------------------------ tempo
namespace {
std::atomic<double> g_bpm{120.0};
std::atomic<int> g_beats{4};
}  // namespace

Tempo tempo()
{
    Tempo t;
    t.flags = 5;
    t.bpm = g_bpm.load();
    t.num = g_beats.load();
    t.den = 4;
    return t;
}

void setTempo(double bpm, int beatsPerMeasure)
{
    if (bpm >= 1.0 && bpm <= 1000.0) g_bpm = bpm;
    if (beatsPerMeasure >= 1 && beatsPerMeasure <= 64) g_beats = beatsPerMeasure;
}

// ------------------------------------------------------------------------------------------ registry
const std::vector<const Info*>& allInfos()
{
    static const std::vector<const Info*> all = {
        &xpAmpModInfo(),
        &xpChorusInfo(),
        &xpEqInfo(),
        &xpDelayInfo(),
        &xpDynamicsInfo(),
        &xpNoiseGateInfo(),
        &simpleDelayInfo(),
        &chorusInfo(),
        &multiTapInfo(),
        &reverbInfo(),
        &pitchInfo(),
        &xpDistorInfo(),
        &xpFlangeInfo(),
        &xpReverbInfo(),
        &xpStutterInfo(),
    };
    return all;
}

const Info* findInfo(const std::string& uid)
{
    for (const Info* i : allInfos())
        if (uid == i->uid) return i;
    return nullptr;
}

std::string uidForDx(const std::string& dxUid)
{
    const std::string u = toLower(dxUid);
    for (const Info* i : allInfos())
        if (u.find(i->clsid) != std::string::npos) return i->uid;
    return std::string();
}

std::string dxForUid(const std::string& uid)
{
    const Info* i = findInfo(uid);
    if (!i) return std::string();
    std::string c = i->clsid;
    for (char& ch : c) ch = (char)std::toupper((unsigned char)ch);
    return "dshow:{" + c + "}";
}

// ------------------------------------------------------------------------------------------ saved form
std::string encode(const Info& info, const int32_t* settings)
{
    std::string b(4 + 16 + (size_t)info.nInts * 4, '\0');
    const uint32_t len = (uint32_t)b.size();
    std::memcpy(&b[0], &len, 4);
    std::memcpy(&b[4], info.persistGuid, 16);
    std::memcpy(&b[20], settings, (size_t)info.nInts * 4);
    return "S:" + base64Encode(b.data(), b.size());
}

bool decode(const Info& info, const std::string& st, std::vector<int32_t>* out)
{
    if (st.size() < 3 || st.compare(0, 2, "S:") != 0) return false;
    const std::string b = base64Decode(st.substr(2));
    if (b.size() < 20 + (size_t)info.nInts * 4 || std::memcmp(b.data() + 4, info.persistGuid, 16) != 0) return false;
    out->assign((size_t)info.nInts, 0);
    std::memcpy(out->data(), b.data() + 20, (size_t)info.nInts * 4);
    return true;
}

// ------------------------------------------------------------------------------------------ the plug-in
namespace {

std::vector<std::string> splitChoices(const char* s)
{
    std::vector<std::string> v;
    std::string cur;
    const char sep = std::strchr(s, '|') ? '|' : ',';       // names that hold commas are separated by '|'
    for (const char* p = s; *p; ++p) {
        if (*p == sep) { v.push_back(cur); cur.clear(); }
        else cur.push_back(*p);
    }
    if (!cur.empty() || (s && *s)) v.push_back(cur);
    return v;
}

class PackPlugin : public AudioPlugin {
public:
    explicit PackPlugin(const Info& info) : info_(info), s_((size_t)info.nInts)
    {
        engine_ = info.make();
        applyAll(info_.presets[0].v.data());
        program_ = 0;
    }
    bool activate(double sampleRate, int maxBlock) override
    {
        (void)maxBlock;
        rate_ = sampleRate > 1000 ? (uint32_t)std::lround(sampleRate) : 48000u;
        started_ = false;
        return true;
    }
    void deactivate() override {}
    void process(float* left, float* right, int frames, int64_t samplePos, bool playing) override
    {
        (void)playing;
        if (!left || !right || frames <= 0) return;
        if (rate_ == 0) activate(48000, frames);
        std::vector<int32_t>& cur = cur_;
        cur.resize((size_t)info_.nInts);
        for (int k = 0; k < info_.nInts; ++k) cur[(size_t)k] = s_[(size_t)k].load();
        if (!started_ || samplePos != expected_) {
            // a jump in time (seek, loop, first block): VEGAS starts the effect again with silent buffers
            engine_->setup(cur.data(), rate_, false);
            applied_ = cur;
            started_ = true;
        } else if (cur != applied_) {
            engine_->setup(cur.data(), rate_, true);
            applied_ = cur;
        }
        expected_ = samplePos + frames;
        if (info_.bypassSlot >= 0 && cur[(size_t)info_.bypassSlot] != 0) return;
        engine_->process(left, right, frames);
        for (int k = 0; k < frames; ++k) {
            if (!std::isfinite(left[k])) left[k] = 0.f;
            if (!std::isfinite(right[k])) right[k] = 0.f;
        }
    }
    int paramCount() override { return (int)info_.params.size(); }
    std::string paramName(int i) override { return valid(i) ? info_.params[(size_t)i].name : std::string(); }
    double real(int i) const
    {
        const ParamDesc& p = info_.params[(size_t)i];
        if (p.kind == Dbl) {
            int32_t w[2] = {s_[(size_t)p.slot].load(), s_[(size_t)p.slot + 1].load()};
            double d;
            std::memcpy(&d, w, 8);
            return d;
        }
        return (double)s_[(size_t)p.slot].load() / (double)p.scale;
    }
    std::string paramText(int i) override
    {
        if (!valid(i)) return std::string();
        const ParamDesc& p = info_.params[(size_t)i];
        const double v = real(i);
        if (p.kind == Bool) return v >= 0.5 ? "On" : "Off";
        if (p.kind == Choice) {
            const std::vector<std::string> c = splitChoices(p.choices);
            const int k = std::max(0, std::min((int)c.size() - 1, (int)std::lround(v)));
            return c.empty() ? std::string() : c[(size_t)k];
        }
        if (p.infAtMin && v <= p.lo + 1e-9) return "-Inf.";
        if (p.atMaxText && v >= p.hi - 1e-9) return p.atMaxText;
        char b[64];
        std::snprintf(b, sizeof b, "%.*f%s%s", p.decimals, v, *p.unit ? " " : "", p.unit);
        return b;
    }
    float paramGet(int i) override
    {
        if (!valid(i)) return 0.f;
        const ParamDesc& p = info_.params[(size_t)i];
        return (float)((real(i) - p.lo) / (p.hi - p.lo));
    }
    bool paramRange(int i, double* lo, double* hi) override
    {
        if (!valid(i)) return false;
        if (lo) *lo = info_.params[(size_t)i].lo;
        if (hi) *hi = info_.params[(size_t)i].hi;
        return true;
    }
    void paramSet(int i, float v) override
    {
        if (!valid(i) || !std::isfinite(v)) return;
        const ParamDesc& p = info_.params[(size_t)i];
        double x = p.lo + std::max(0.f, std::min(1.f, v)) * (p.hi - p.lo);
        if (p.kind == Bool || p.kind == Choice) x = std::round(x);
        setSlot(p, x);
        if (info_.derive) {
            std::vector<int32_t> v((size_t)info_.nInts);
            for (int k = 0; k < info_.nInts; ++k) v[(size_t)k] = s_[(size_t)k].load();
            info_.derive(v.data(), p.slot);
            for (int k = 0; k < info_.nInts; ++k) s_[(size_t)k] = v[(size_t)k];
        }
        program_ = -1;
    }
    std::string saveState() override
    {
        std::vector<int32_t> v((size_t)info_.nInts);
        for (int k = 0; k < info_.nInts; ++k) v[(size_t)k] = s_[(size_t)k].load();
        return encode(info_, v.data());
    }
    bool loadState(const std::string& st) override
    {
        std::vector<int32_t> v;
        if (decode(info_, st, &v)) {
            applyAll(v.data());
            program_ = -1;
            return true;
        }
        // settings saved by older Reno versions (other parameters): start from VEGAS's defaults
        if (st.compare(0, 18, "reno-builtin-audio") == 0) {
            applyAll(info_.presets[0].v.data());
            program_ = 0;
            return true;
        }
        return false;
    }
    int latencySamples() override
    {
        std::vector<int32_t> v((size_t)info_.nInts);
        for (int k = 0; k < info_.nInts; ++k) v[(size_t)k] = s_[(size_t)k].load();
        return engine_->latency(v.data(), rate_ ? rate_ : 48000u);
    }
    int programCount() override { return (int)info_.presets.size(); }
    std::string programName(int i) override { return i >= 0 && i < programCount() ? info_.presets[(size_t)i].name : std::string(); }
    int currentProgram() override { return program_; }
    bool setProgram(int i) override
    {
        if (i < 0 || i >= programCount()) return false;
        applyAll(info_.presets[(size_t)i].v.data());
        program_ = i;
        return true;
    }

private:
    const Info& info_;
    std::unique_ptr<Engine> engine_;
    std::vector<std::atomic<int32_t>> s_;
    std::vector<int32_t> cur_, applied_;
    uint32_t rate_ = 0;
    bool started_ = false;
    int64_t expected_ = -1;
    int program_ = -1;

    bool valid(int i) const { return i >= 0 && i < (int)info_.params.size(); }
    void setSlot(const ParamDesc& p, double real)
    {
        if (p.kind == Dbl) {
            real = std::max(p.lo, std::min(p.hi, real));
            int32_t w[2];
            std::memcpy(w, &real, 8);
            s_[(size_t)p.slot] = w[0];
            s_[(size_t)p.slot + 1] = w[1];
            return;
        }
        const long lo = std::lround(p.lo * p.scale), hi = std::lround(p.hi * p.scale);
        const long v = std::lround(real * p.scale);
        s_[(size_t)p.slot] = (int32_t)std::max(lo, std::min(hi, v));
    }
    // every slot a setting covers is clamped to its range; the others are kept as they are
    void applyAll(const int32_t* v)
    {
        for (int k = 0; k < info_.nInts; ++k) s_[(size_t)k] = v[k];
        for (const ParamDesc& p : info_.params) {
            if (p.kind == Dbl) {
                double d;
                std::memcpy(&d, v + p.slot, 8);
                setSlot(p, d);
            } else {
                setSlot(p, (double)v[p.slot] / (double)p.scale);
            }
        }
    }
};

}  // namespace

std::unique_ptr<AudioPlugin> create(const std::string& uid)
{
    const Info* i = findInfo(uid);
    if (!i) return nullptr;
    return std::unique_ptr<AudioPlugin>(new PackPlugin(*i));
}

}  // namespace vegpack1
}  // namespace reno
