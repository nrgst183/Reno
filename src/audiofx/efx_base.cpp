// Shared parts of Reno's eFX effects (see efx.h): reading and wording a setting's 0..1, the base class with the 31
// settings and their saved form, and the choice between these and the installed MAGIX plug-ins.
#include "efx.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace reno {

const char* const kEfxVendor = "Reno (reproduction of MAGIX eFX)";

namespace efx {

// ---------------------------------------------------------------------------------------------------- settings
ParamDef pUnused() { return ParamDef(); }

ParamDef pLin(const char* name, float def, double lo, double hi, Fmt fmt)
{
    ParamDef d;
    d.name = name; d.def = def; d.kind = Kind::Lin; d.lo = lo; d.hi = hi; d.fmt = fmt;
    return d;
}

ParamDef pRat(const char* name, float def, double lo, double hi, double a, Fmt fmt)
{
    ParamDef d;
    d.name = name; d.def = def; d.kind = Kind::Rat; d.lo = lo; d.hi = hi; d.a = a; d.fmt = fmt;
    return d;
}

ParamDef pPow(const char* name, float def, double lo, double hi, double p, Fmt fmt)
{
    ParamDef d;
    d.name = name; d.def = def; d.kind = Kind::Pow; d.lo = lo; d.hi = hi; d.a = p; d.fmt = fmt;
    return d;
}

ParamDef pGain(const char* name, float def)
{
    ParamDef d;
    d.name = name; d.def = def; d.kind = Kind::Gain; d.fmt = Fmt::DB;
    return d;
}

ParamDef pSteps(const char* name, float def, const char* const* labels, int n, const double* thresholds)
{
    ParamDef d;
    d.name = name; d.def = def; d.kind = Kind::Steps; d.labels = labels; d.nlabels = n; d.thresholds = thresholds;
    return d;
}

ParamDef pFixed(const char* name, float def, const char* text)
{
    ParamDef d;
    d.name = name; d.def = def; d.kind = Kind::Fixed; d.fixed = text;
    return d;
}

double mapValue(const ParamDef& d, double x)
{
    if (!std::isfinite(x)) x = 0;
    x = std::max(0.0, std::min(1.0, x));
    switch (d.kind) {
    case Kind::Lin: return d.lo + x * (d.hi - d.lo);
    case Kind::Rat: return d.lo + (d.hi - d.lo) * x / (d.a - (d.a - 1.0) * x);
    case Kind::Pow: return d.lo + (d.hi - d.lo) * std::pow(x, d.a);
    case Kind::Gain: return x <= 0 ? -200.0 : 20.0 * std::log10(x);
    case Kind::Steps: {
        int k = 0;
        for (int i = 0; i + 1 < d.nlabels; ++i) if ((float)x > (float)d.thresholds[i]) k = i + 1;       // in float, like a plug-in's 0..1
        return k;
    }
    case Kind::Fixed: return x;
    default: return 0;
    }
}

namespace {
// a number cut off at the decimal point (as the real one's percentages do), forgiving float noise
long cut(double v) { return (long)std::trunc(v + (v < 0 ? -1e-4 : 1e-4)); }
const char* const kMicro = "\xC2\xB5";
}

std::string formatNumber(Fmt f, double v)
{
    char b[64];
    const double a = std::fabs(v);
    switch (f) {
    case Fmt::DB:
        if (v < -150) return "-inf";
        if (a < 0.005) return "0dB";
        std::snprintf(b, sizeof b, a >= 10 ? "%.1fdB" : "%.2fdB", v);
        return b;
    case Fmt::MSOFF:
        if (v < 0.02) return "Off";
        return formatNumber(Fmt::MS, v);
    case Fmt::MS:
        if (v < 0.9995) { std::snprintf(b, sizeof b, "%.0f%ss", v * 1000.0, kMicro); return b; }
        if (v < 9.95) { std::snprintf(b, sizeof b, "%.1fms", v); return b; }
        if (v < 999.5) { std::snprintf(b, sizeof b, "%.0fms", v); return b; }
        std::snprintf(b, sizeof b, "%.2fs", v / 1000.0);
        return b;
    case Fmt::MSP:
        std::snprintf(b, sizeof b, v < 9.995 ? "%.2fms" : "%.1fms", v);
        return b;
    case Fmt::SEC:
        std::snprintf(b, sizeof b, v < 9.995 ? "%.2fs" : "%.1fs", v);
        return b;
    case Fmt::HZ:
        if (v < 999.5) { std::snprintf(b, sizeof b, "%.0fHz", v); return b; }
        std::snprintf(b, sizeof b, "%.2fkHz", v / 1000.0);
        return b;
    case Fmt::HZR:
        std::snprintf(b, sizeof b, v < 9.995 ? "%.2fHz" : "%.1fHz", v);
        return b;
    case Fmt::KHZ2: std::snprintf(b, sizeof b, "%.2fkHz", v / 1000.0); return b;
    case Fmt::HZSP:
        std::snprintf(b, sizeof b, v < 9.995 ? "%.2f Hz" : "%.0f Hz", v);
        return b;
    case Fmt::HZOFF:
        if (v < 0.5) return "Off";
        std::snprintf(b, sizeof b, "%.0fHz", v);
        return b;
    case Fmt::PCT: std::snprintf(b, sizeof b, "%ld%%", cut(v)); return b;
    case Fmt::PCT1: std::snprintf(b, sizeof b, "%.1f%%", a < 0.05 ? 0.0 : v); return b;
    case Fmt::DEG: std::snprintf(b, sizeof b, "%.0f\xC2\xB0", a < 0.5 ? 0.0 : v); return b;
    case Fmt::NUM2: std::snprintf(b, sizeof b, "%.2f", v); return b;
    case Fmt::RATIO:
        std::snprintf(b, sizeof b, v < 9.95 ? "%.1f : 1" : "%.0f : 1", v);
        return b;
    case Fmt::PEAKRMS: {
        if (v < 1e-6) return "Peak";
        if (v > 100 - 1e-6) return "RMS";
        std::snprintf(b, sizeof b, "%02ld : %02ld", 100 - cut(v), cut(v));
        return b;
    }
    case Fmt::SOFTHARD:
        if (v < 1e-6) return "Soft";
        if (v > 100 - 1e-6) return "Hard";
        std::snprintf(b, sizeof b, "%ld%%", cut(v));
        return b;
    case Fmt::PINGDUAL:
        if (v < 0) std::snprintf(b, sizeof b, "%ld%% ping", cut(-v));
        else std::snprintf(b, sizeof b, "%ld%% dual", cut(v));
        return b;
    default: return std::string();
    }
}

std::string mapText(const ParamDef& d, double x)
{
    switch (d.kind) {
    case Kind::Unused: return std::string();
    case Kind::Fixed: return d.fixed;
    case Kind::Steps: {
        const int k = (int)mapValue(d, x);
        return k >= 0 && k < d.nlabels ? d.labels[k] : "";
    }
    default: return formatNumber(d.fmt, mapValue(d, x));
    }
}

// ---------------------------------------------------------------------------------------------------- filters
void Biquad::highpass(double fs, double f, double q)
{
    f = std::max(5.0, std::min(f, fs * 0.45));
    const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = (1 + c) / 2 / a0; b1 = -(1 + c) / a0; b2 = (1 + c) / 2 / a0;
    a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::lowpass(double fs, double f, double q)
{
    f = std::max(5.0, std::min(f, fs * 0.45));
    const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = (1 - c) / 2 / a0;
    a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::bandpass(double fs, double f, double q)
{
    f = std::max(5.0, std::min(f, fs * 0.45));
    const double w = 2 * kPi * f / fs, c = std::cos(w), al = std::sin(w) / (2 * q), a0 = 1 + al;
    b0 = al / a0; b1 = 0; b2 = -al / a0;
    a1 = -2 * c / a0; a2 = (1 - al) / a0;
}
void Biquad::lowShelf(double fs, double f, double gainDb)
{
    f = std::max(5.0, std::min(f, fs * 0.45));
    const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const double al = s / 2 * std::sqrt(2.0), t = 2 * std::sqrt(A) * al;
    const double a0 = (A + 1) + (A - 1) * c + t;
    b0 = A * ((A + 1) - (A - 1) * c + t) / a0;
    b1 = 2 * A * ((A - 1) - (A + 1) * c) / a0;
    b2 = A * ((A + 1) - (A - 1) * c - t) / a0;
    a1 = -2 * ((A - 1) + (A + 1) * c) / a0;
    a2 = ((A + 1) + (A - 1) * c - t) / a0;
}
void Biquad::highShelf(double fs, double f, double gainDb)
{
    f = std::max(5.0, std::min(f, fs * 0.45));
    const double A = std::pow(10.0, gainDb / 40), w = 2 * kPi * f / fs, c = std::cos(w), s = std::sin(w);
    const double al = s / 2 * std::sqrt(2.0), t = 2 * std::sqrt(A) * al;
    const double a0 = (A + 1) - (A - 1) * c + t;
    b0 = A * ((A + 1) + (A - 1) * c + t) / a0;
    b1 = -2 * A * ((A - 1) + (A + 1) * c) / a0;
    b2 = A * ((A + 1) + (A - 1) * c - t) / a0;
    a1 = 2 * ((A - 1) - (A + 1) * c) / a0;
    a2 = ((A + 1) - (A - 1) * c - t) / a0;
}

// ---------------------------------------------------------------------------------------------------- base class
EfxPlugin::EfxPlugin(const char* displayName, uint32_t vstId, std::vector<ParamDef> defs) : id_(vstId), defs_(std::move(defs))
{
    defs_.resize(kParams);
    for (int k = 0; k < kParams; ++k) values_[(size_t)k].store(defs_[(size_t)k].def);
    desc.format = "VST2";
    desc.uid = std::to_string(vstId);
    desc.name = displayName;
    desc.vendor = kEfxVendor;
    desc.numInputs = desc.numOutputs = 2;
    desc.hasEditor = false;
}

bool EfxPlugin::activate(double sampleRate, int maxBlock)
{
    sr_ = sampleRate >= 8000 ? sampleRate : 48000;
    maxBlock_ = maxBlock > 0 ? maxBlock : 512;
    prepare();
    reset();
    expected_ = -1;
    wasActive_ = true;
    return true;
}

void EfxPlugin::process(float* left, float* right, int frames, int64_t samplePos, bool playing)
{
    (void)playing;
    if (!left || !right || frames <= 0) return;
    if (sr_ <= 0) activate(48000, frames);
    // a jump in time (seek, loop) restarts the effect so nothing from elsewhere carries over
    if (expected_ >= 0 && samplePos != expected_) reset();
    expected_ = samplePos + frames;
    for (int k = 0; k < frames; ++k) {
        if (!std::isfinite(left[k])) left[k] = 0.f;
        if (!std::isfinite(right[k])) right[k] = 0.f;
    }
    if (!on(kActive)) { wasActive_ = false; return; }
    if (!wasActive_) { reset(); wasActive_ = true; }
    run(left, right, frames);
    for (int k = 0; k < frames; ++k) {
        if (!std::isfinite(left[k])) left[k] = 0.f;
        if (!std::isfinite(right[k])) right[k] = 0.f;
    }
}

std::string EfxPlugin::paramName(int i) { return i >= 0 && i < kParams ? defs_[(size_t)i].name : std::string(); }
std::string EfxPlugin::paramText(int i)
{
    if (i < 0 || i >= kParams) return std::string();
    return mapText(defs_[(size_t)i], raw(i));
}
float EfxPlugin::paramGet(int i) { return i >= 0 && i < kParams ? raw(i) : 0.f; }
void EfxPlugin::paramSet(int i, float v)
{
    if (i < 0 || i >= kParams || !std::isfinite(v)) return;
    values_[(size_t)i].store(std::max(0.f, std::min(1.f, v)));
}
bool EfxPlugin::on(int i) const
{
    if (i < 0 || i >= kParams) return false;
    const ParamDef& d = defs_[(size_t)i];
    if (d.kind == Kind::Steps) return mapValue(d, raw(i)) >= 0.5;
    return raw(i) >= 0.5f;
}
double EfxPlugin::real(int i) const { return i >= 0 && i < kParams ? mapValue(defs_[(size_t)i], raw(i)) : 0.0; }

std::string EfxPlugin::saveState()
{
    float v[kParams];
    for (int k = 0; k < kParams; ++k) v[k] = raw(k);
    return "P:" + base64Encode(v, sizeof v);
}

bool EfxPlugin::loadState(const std::string& s)
{
    if (s.size() < 2 || s[0] != 'P' || s[1] != ':') return false;
    const std::string bytes = base64Decode(s.substr(2));
    const size_t n = std::min<size_t>(bytes.size() / sizeof(float), kParams);
    for (size_t k = 0; k < n; ++k) {
        float f;
        std::memcpy(&f, bytes.data() + k * sizeof(float), sizeof f);
        if (std::isfinite(f)) values_[k].store(f);      // kept as saved, even odd Version values
    }
    return n > 0;
}

// VST preset files, all big-endian: 'CcnK', length of the rest, 'FxCk' (a program of parameters; 'FxBk' a bank),
// format version, plug-in id, its version, parameter (program) count, then a 28-byte name and the floats (bank:
// version 1 has 128 reserved bytes after the count, version 2 the current program and 124 reserved bytes).
static void put32(std::string& s, uint32_t v) { for (int k = 3; k >= 0; --k) s.push_back((char)((v >> (8 * k)) & 0xFF)); }
static uint32_t get32(const std::string& s, size_t at) { uint32_t v = 0; for (int k = 0; k < 4; ++k) v = (v << 8) | (uint8_t)s[at + (size_t)k]; return v; }

std::string EfxPlugin::saveVstPresetFile(bool bank)
{
    std::string prog;
    put32(prog, 0x4678436Bu);                           // 'FxCk'
    put32(prog, 1);
    put32(prog, id_);
    put32(prog, 1);
    put32(prog, (uint32_t)kParams);
    char name[28] = {0};
    std::snprintf(name, sizeof name, "%s", "Reno");
    prog.append(name, sizeof name);
    for (int k = 0; k < kParams; ++k) {
        const float f = raw(k);
        uint32_t u;
        std::memcpy(&u, &f, 4);
        put32(prog, u);
    }
    std::string out;
    out.append("CcnK", 4);
    if (!bank) {
        put32(out, (uint32_t)prog.size());
        out += prog;
        return out;
    }
    // (a bank of one program: the program is written whole, 'CcnK' and all)
    std::string body;
    put32(body, 0x4678426Bu);                           // 'FxBk'
    put32(body, 2);
    put32(body, id_);
    put32(body, 1);
    put32(body, 1);                                     // programs
    put32(body, 0);                                     // current program
    body.append(124, '\0');
    std::string whole = "CcnK";
    put32(whole, (uint32_t)prog.size());
    whole += prog;
    body += whole;
    put32(out, (uint32_t)body.size());
    out += body;
    return out;
}

bool EfxPlugin::loadVstPresetFile(const std::string& bytes, std::string* error)
{
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (bytes.size() < 56 || bytes.compare(0, 4, "CcnK") != 0) return fail("this is not a VST preset file");
    size_t at = 8;
    uint32_t kind = get32(bytes, at);
    if (kind == 0x4678426Bu) {                          // a bank: its first program
        if (bytes.size() < 8 + 160 + 56) return fail("the bank is cut short");
        const uint32_t version = get32(bytes, 12);
        if (get32(bytes, 16) != id_) return fail("this file is for another plug-in");
        const size_t first = 8 + (version >= 2 ? 4 * 6 + 124 : 4 * 5 + 128);
        if (bytes.size() < first + 8 + 4 || bytes.compare(first, 4, "CcnK") != 0) return fail("the bank holds no program");
        at = first + 8;
        kind = get32(bytes, at);
    }
    if (kind != 0x4678436Bu) return fail("this kind of preset (with the plug-in's own data) is not for Reno's version of the effect");
    if (bytes.size() < at + 4 * 5 + 28) return fail("the program is cut short");
    if (get32(bytes, at + 8) != id_) return fail("this file is for another plug-in");
    const uint32_t n = get32(bytes, at + 16);
    const size_t data = at + 20 + 28;
    if ((size_t)n > (bytes.size() - data) / 4) return fail("the program is cut short");
    for (uint32_t k = 0; k < n && k < (uint32_t)kParams; ++k) {
        const uint32_t u = get32(bytes, data + 4 * (size_t)k);
        float f;
        std::memcpy(&f, &u, 4);
        if (std::isfinite(f)) values_[k].store(f);
    }
    return true;
}

}  // namespace efx

// ---------------------------------------------------------------------------------------------------- selection
namespace {
std::atomic<bool> gPreferEfx{true};

struct EfxKind {
    const char* id;
    std::unique_ptr<efx::EfxPlugin> (*make)();
};
const EfxKind kEfx[] = {
    {"MX2A", efx::makeLimiter},
    {"MX27", efx::makeCompressor},
    {"MX23", efx::makeReverb},
    {"MX2B", efx::makeTubeStage},
    {"MX22", efx::makePhaser},
    {"MX21", efx::makeChorusFlanger},
    {"MX28", efx::makeDeEsser},
    {"MX29", efx::makeGate},
    {"MX24", efx::makeStereoDelay},
    {"MX32", efx::makeTremoloPan},
    {"MX30", efx::makeVocalStrip},
};

uint32_t idOf(const char* four)
{
    return ((uint32_t)(uint8_t)four[0] << 24) | ((uint32_t)(uint8_t)four[1] << 16) | ((uint32_t)(uint8_t)four[2] << 8) | (uint32_t)(uint8_t)four[3];
}
}  // namespace

bool efxPreferBuiltin() { return gPreferEfx.load(); }
void setEfxPreferBuiltin(bool on) { gPreferEfx.store(on); }

bool efxReproducedId(const std::string& vst2Uid)
{
    char* end = nullptr;
    const unsigned long v = std::strtoul(vst2Uid.c_str(), &end, 10);
    if (vst2Uid.empty() || (end && *end)) return false;
    for (const auto& k : kEfx)
        if (idOf(k.id) == (uint32_t)v) return true;
    return false;
}

bool efxIsReproduction(const AudioPluginDesc& desc) { return desc.vendor == kEfxVendor; }

std::unique_ptr<AudioPlugin> efxCreate(const AudioPluginDesc& desc, std::string* error)
{
    if (desc.format == "VST2") {
        char* end = nullptr;
        const unsigned long v = std::strtoul(desc.uid.c_str(), &end, 10);
        if (!desc.uid.empty() && !(end && *end))
            for (const auto& k : kEfx)
                if (idOf(k.id) == (uint32_t)v) {
                    std::unique_ptr<efx::EfxPlugin> p = k.make();
                    const std::string name = p->desc.name;
                    p->desc = desc;                     // the slot's own description (its file, if the plug-in is installed)
                    if (p->desc.name.empty()) p->desc.name = name;
                    p->desc.vendor = kEfxVendor;
                    p->desc.numInputs = p->desc.numOutputs = 2;
                    p->desc.hasEditor = false;
                    p->desc.bridged32 = false;
                    return p;
                }
    }
    if (error) *error = "Reno has no reproduction of the plug-in " + desc.uid;
    return nullptr;
}

}  // namespace reno
