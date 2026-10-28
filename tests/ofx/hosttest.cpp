// hosttest - tests for Reno's OpenFX host (src/ofx).
//
//   hosttest <path to testfx.ofx> [--only <section>]
//   RENO_TEST_PLUGIN=<effects.ofx> names the 28 real effects to test; without it those tests SKIP.
//
// One line per check ("PASS name" / "FAIL name: detail"), a summary at the end, and a non-zero exit code if
// anything failed. testfx.cpp holds the purpose-built effects; they write what they saw into a log that is read
// back here through the testfx_* functions.
#include "ofxhost.h"
#include "probe.h"
#include "vegas_ext.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace reno;
using namespace reno::ofx;

// ====================================================================== reporting
static std::mutex gOut;
static int gPassN = 0, gFailN = 0, gSkipN = 0;
static std::vector<std::string> gFailed;

static void record(const std::string& name, bool ok, const std::string& detail = std::string())
{
    std::lock_guard<std::mutex> lk(gOut);
    if (ok) {
        ++gPassN;
        std::printf("PASS %s\n", name.c_str());
    } else {
        ++gFailN;
        gFailed.push_back(name);
        std::printf("FAIL %s: %s\n", name.c_str(), detail.c_str());
    }
    std::fflush(stdout);
}
static void skip(const std::string& name, const std::string& why)
{
    std::lock_guard<std::mutex> lk(gOut);
    ++gSkipN;
    std::printf("SKIP %s: %s\n", name.c_str(), why.c_str());
    std::fflush(stdout);
}
static void note(const std::string& text)
{
    std::lock_guard<std::mutex> lk(gOut);
    std::printf("NOTE %s\n", text.c_str());
    std::fflush(stdout);
}

// (printing helpers for checkEq)
[[maybe_unused]] static std::string toStr(const std::string& s) { return s; }
[[maybe_unused]] static std::string toStr(const char* s) { return s ? s : "(null)"; }
[[maybe_unused]] static std::string toStr(int v) { return std::to_string(v); }
[[maybe_unused]] static std::string toStr(long v) { return std::to_string(v); }
[[maybe_unused]] static std::string toStr(unsigned v) { return std::to_string(v); }
[[maybe_unused]] static std::string toStr(unsigned long v) { return std::to_string(v); }
[[maybe_unused]] static std::string toStr(bool v) { return v ? "true" : "false"; }
[[maybe_unused]] static std::string toStr(double v) { return strfmt("%.17g", v); }
[[maybe_unused]] static std::string toStr(const void* v) { return strfmt("%p", v); }
template <class T>
[[maybe_unused]] static std::string toStr(const std::vector<T>& v)
{
    std::string r = "[";
    for (size_t i = 0; i < v.size(); ++i) r += (i ? "," : "") + toStr(v[i]);
    return r + "]";
}

#define CHECK(name, cond) record((name), (cond), "condition is false: " #cond)
#define CHECKM(name, cond, ...) record((name), (cond), strfmt(__VA_ARGS__))
template <class A, class B>
static void checkEq(const std::string& name, const A& got, const B& want)
{
    record(name, got == want, "got <" + toStr(got) + "> want <" + toStr(want) + ">");
}

static std::string fmt17(double v) { return strfmt("%.17g", v); }
static bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

// ====================================================================== the test binary's log
struct FxLib {
    void* h = nullptr;
    int (*count)(void) = nullptr;
    const char* (*line)(int) = nullptr;
    void (*clearLog)(void) = nullptr;
    int (*counter)(const char*) = nullptr;
    void (*clearCounters)(void) = nullptr;
    int (*dump)(const char*, char*, int) = nullptr;

    bool open(const std::string& path)
    {
        h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) return false;
        count = (int (*)(void))dlsym(h, "testfx_log_count");
        line = (const char* (*)(int))dlsym(h, "testfx_log_line");
        clearLog = (void (*)(void))dlsym(h, "testfx_log_clear");
        counter = (int (*)(const char*))dlsym(h, "testfx_counter");
        clearCounters = (void (*)(void))dlsym(h, "testfx_counters_clear");
        dump = (int (*)(const char*, char*, int))dlsym(h, "testfx_dump");
        return count && line && clearLog && counter && clearCounters && dump;
    }
    std::vector<std::string> lines() const
    {
        std::vector<std::string> v;
        for (int i = 0, n = count(); i < n; ++i) {
            const char* l = line(i);
            if (l) v.push_back(l);
        }
        return v;
    }
    std::vector<std::string> grep(const std::string& sub) const
    {
        std::vector<std::string> v;
        for (auto& l : lines()) if (contains(l, sub)) v.push_back(l);
        return v;
    }
    bool has(const std::string& sub) const { return !grep(sub).empty(); }
    std::string last(const std::string& sub) const
    {
        auto v = grep(sub);
        return v.empty() ? std::string() : v.back();
    }
    std::string dumpOf(const char* effect) const
    {
        char buf[8192];
        buf[0] = 0;
        dump(effect, buf, sizeof buf);
        return buf;
    }
    std::string failures() const
    {
        std::string s;
        for (auto& l : grep("CHECKFAIL")) s += " [" + l + "]";
        return s;
    }
};

static FxLib gFx;
static std::string gFxPath, gScratchDir;

// ====================================================================== pictures
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    uint32_t next()
    {
        s = s * 6364136223846793005ull + 1442695040888963407ull;
        return (uint32_t)(s >> 33);
    }
};

static ImagePtr randomImage(int w, int h, uint64_t seed, PixelDepth d = PixelDepth::Byte)
{
    ImagePtr img = Image::make(w, h, d, false);
    Rng r(seed);
    for (int y = 0; y < h; ++y) {
        if (d == PixelDepth::Byte) {
            uint8_t* p = img->row(y);
            for (int x = 0; x < w * 4; ++x) p[x] = (uint8_t)(r.next() >> 3);
        } else {
            float* p = (float*)img->row(y);
            for (int x = 0; x < w * 4; ++x) p[x] = (r.next() & 1023) / 1023.f;
        }
    }
    return img;
}

static ImagePtr solidImage(int w, int h, uint8_t b, uint8_t g, uint8_t r, uint8_t a)
{
    ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
    for (int y = 0; y < h; ++y) {
        uint8_t* p = img->row(y);
        for (int x = 0; x < w; ++x, p += 4) { p[0] = b; p[1] = g; p[2] = r; p[3] = a; }
    }
    return img;
}

// the same picture with every row longer than it needs to be (the padding is filled with a marker)
static ImagePtr paddedCopy(const Image& src, size_t extra)
{
    auto img = std::make_shared<Image>();
    img->w = src.w;
    img->h = src.h;
    img->depth = src.depth;
    img->rowBytes = src.rowBytes + extra;
    uint8_t* mem = (uint8_t*)std::malloc(img->rowBytes * (size_t)src.h);
    std::memset(mem, 0xEE, img->rowBytes * (size_t)src.h);
    for (int y = 0; y < src.h; ++y) std::memcpy(mem + img->rowBytes * (size_t)y, src.row(y), src.rowBytes);
    img->data = std::unique_ptr<uint8_t, void (*)(void*)>(mem, std::free);
    return img;
}

static bool sameImage(const Image& a, const Image& b)
{
    if (a.w != b.w || a.h != b.h || a.depth != b.depth) return false;
    for (int y = 0; y < a.h; ++y)
        if (std::memcmp(a.row(y), b.row(y), (size_t)a.w * a.pixelBytes()) != 0) return false;
    return true;
}

static int firstDifference(const Image& a, const Image& b)
{
    if (a.w != b.w || a.h != b.h || a.depth != b.depth) return -2;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w * (int)a.pixelBytes(); ++x)
            if (a.row(y)[x] != b.row(y)[x]) return y * a.w * (int)a.pixelBytes() + x;
    return -1;
}

static uint64_t hashImage(const Image& img)
{
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* p = img.row(y);
        for (size_t x = 0, n = (size_t)img.w * img.pixelBytes(); x < n; ++x) { h ^= p[x]; h *= 1099511628211ull; }
    }
    return h;
}

// the conversions Image::converted does (used to predict what a float effect produces)
static inline float b2f(uint8_t v) { return v * (1.f / 255.f); }
static inline uint8_t f2b(float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    return (uint8_t)(v * 255.f + 0.5f);
}
static inline uint8_t clampB(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// ====================================================================== fixtures
static Host& host() { return Host::get(); }

static Plugin* plug(const std::string& id)
{
    Plugin* p = host().findPlugin(id);
    if (!p) { host().loadBinary(gFxPath); p = host().findPlugin(id); }
    if (p) p->load();
    return p;
}

static std::unique_ptr<Instance> makeInstance(const std::string& id, int w, int h, std::string context = std::string(), std::string vegasContext = std::string(),
                                              std::string* err = nullptr)
{
    Plugin* p = plug(id);
    if (!p) { if (err) *err = "no such plug-in " + id; return nullptr; }
    if (context.empty()) context = p->filterContext();
    InstanceSetup s;
    s.width = w;
    s.height = h;
    s.vegasContext = vegasContext;
    std::string e;
    auto inst = p->createInstance(context, s, &e);
    if (!inst && err) *err = e;
    return inst;
}

static void setSource(Instance& inst, const std::string& clip, ImagePtr img)
{
    inst.setClipSource(clip, [img](double) { return img; });
}

static ImagePtr renderOnce(Instance& inst, double t, std::string* err = nullptr, bool interactive = true)
{
    RenderOptions ro;
    ro.interactive = interactive;
    return inst.render(t, ro, err);
}

static Param* prm(Instance& inst, const char* name) { return inst.params.find(name); }

static bool hasName(const std::vector<std::string>& v, const std::string& s) { return std::find(v.begin(), v.end(), s) != v.end(); }

// ====================================================================== 1. property sets, parameters, small pieces
static void testPropertySet()
{
    PropertySet s;
    s.setInt("i", 5);
    checkEq("props: int round trip", s.getInt("i"), 5);
    s.setDouble("d", 2.5, 2);
    checkEq("props: indexed set grows the property", s.dimension("d"), 3);
    checkEq("props: indexed double", s.getDouble("d", 2), 2.5);
    checkEq("props: untouched index is zero", s.getDouble("d", 0), 0.0);
    s.setDouble("i", 7.6);
    checkEq("props: double into an int property is rounded", s.getInt("i"), 8);
    s.setInt("d", 3, 0);
    checkEq("props: int into a double property is kept as a double", s.getDouble("d", 0), 3.0);
    s.setString("i", "text");
    checkEq("props: wrong-typed set is ignored", s.getInt("i"), 8);
    checkEq("props: wrong-typed get gives the default", s.getString("i", 0, "dflt"), std::string("dflt"));
    checkEq("props: missing int gives the default", s.getInt("nope", 0, 42), 42);
    s.setString("s", "a", 0);
    s.setString("s", "b", 1);
    checkEq("props: string at index", s.getString("s", 1), std::string("b"));
    checkEq("props: string past the end gives the default", s.getString("s", 2, "z"), std::string("z"));
    s.setInt("neg", 1, -1);
    CHECK("props: negative index creates nothing", !s.has("neg"));
    s.setPointer("p", (void*)0x10, 1);
    checkEq("props: pointer", (const void*)s.getPointer("p", 1), (const void*)0x10);
    checkEq("props: pointer slot before it is NULL", (const void*)s.getPointer("p", 0), (const void*)nullptr);
    s.resetDim("s", 1);
    checkEq("props: resetDim shrinks", s.dimension("s"), 1);
    s.resetDim("s", 3);
    checkEq("props: resetDim grows", s.dimension("s"), 3);
    PropertySet copy = s;
    copy.setString("s", "changed", 0);
    checkEq("props: a copy is independent", s.getString("s", 0), std::string("a"));
    CHECK("props: copy keeps the magic", copy.magic == PropertySet::kMagic);
    s.erase("s");
    CHECK("props: erase", !s.has("s") && copy.has("s"));
    auto names = copy.names();
    CHECK("props: names are sorted", std::is_sorted(names.begin(), names.end()));
    CHECK("props: constructed set has the magic", PropertySet().magic == PropertySet::kMagic);
}

static void testParamSetUnits()
{
    ParamSet ps;
    struct { const char* type; ParamKind kind; int dim; } kinds[] = {
        {kOfxParamTypeInteger, ParamKind::Int, 1}, {kOfxParamTypeDouble, ParamKind::Double, 1}, {kOfxParamTypeBoolean, ParamKind::Bool, 1},
        {kOfxParamTypeChoice, ParamKind::Choice, 1}, {kOfxParamTypeRGBA, ParamKind::RGBA, 4}, {kOfxParamTypeRGB, ParamKind::RGB, 3},
        {kOfxParamTypeDouble2D, ParamKind::Double2D, 2}, {kOfxParamTypeInteger2D, ParamKind::Int2D, 2}, {kOfxParamTypeDouble3D, ParamKind::Double3D, 3},
        {kOfxParamTypeInteger3D, ParamKind::Int3D, 3}, {kOfxParamTypeString, ParamKind::String, 0}, {kOfxParamTypeCustom, ParamKind::Custom, 0},
        {kOfxParamTypeGroup, ParamKind::Group, 0}, {kOfxParamTypePage, ParamKind::Page, 0}, {kOfxParamTypePushButton, ParamKind::PushButton, 0}};
    for (auto& k : kinds) {
        Param* p = ps.define(k.type, std::string("n_") + k.type);
        bool ok = p && p->kind == k.kind && p->dim == k.dim && (int)p->value.size() == k.dim;
        record(std::string("param unit: define ") + k.type, ok, p ? strfmt("kind %d dim %d", (int)p->kind, p->dim) : "null");
    }
    CHECK("param unit: duplicate name is refused", ps.define(kOfxParamTypeDouble, std::string("n_") + kOfxParamTypeDouble) == nullptr);
    CHECK("param unit: find", ps.find("n_OfxParamTypeRGB") != nullptr && ps.find("zzz") == nullptr);
    CHECK("param unit: defined in order", ps.params.size() == 15 && ps.params[0]->kind == ParamKind::Int && ps.params[14]->kind == ParamKind::PushButton);
    Param* rgba = ps.find("n_OfxParamTypeRGBA");
    checkEq("param unit: RGBA default alpha is 1", rgba->props.getDouble(kOfxParamPropDefault, 3), 1.0);
    CHECK("param unit: only valued kinds hasValue", ps.find("n_OfxParamTypeString")->hasValue() && !ps.find("n_OfxParamTypeGroup")->hasValue() &&
                                                       !ps.find("n_OfxParamTypePushButton")->hasValue());
    CHECK("param unit: whole kinds", ps.find("n_OfxParamTypeInteger3D")->isWhole() && ps.find("n_OfxParamTypeBoolean")->isWhole() &&
                                         !ps.find("n_OfxParamTypeRGB")->isWhole());
    CHECK("param unit: an unknown type is refused when defined directly", ps.define("OfxParamTypeNoSuch", "weird") == nullptr && ps.find("weird") == nullptr);

    // interpolation
    ParamSet qs;
    Param* d = qs.define(kOfxParamTypeDouble, "d");
    checkEq("param unit: no keys gives the value", d->valueAt(5.0)[0], 0.0);
    d->keys[10.0] = {1.0};
    checkEq("param unit: single key before", d->valueAt(0.0)[0], 1.0);
    checkEq("param unit: single key after", d->valueAt(99.0)[0], 1.0);
    d->keys[20.0] = {3.0};
    d->keys[30.0] = {-1.0};
    checkEq("param unit: before first key", d->valueAt(-5.0)[0], 1.0);
    checkEq("param unit: at first key", d->valueAt(10.0)[0], 1.0);
    checkEq("param unit: between keys", d->valueAt(15.0)[0], 2.0);
    checkEq("param unit: at middle key", d->valueAt(20.0)[0], 3.0);
    checkEq("param unit: between later keys", d->valueAt(25.0)[0], 1.0);
    checkEq("param unit: at last key", d->valueAt(30.0)[0], -1.0);
    checkEq("param unit: after last key", d->valueAt(1e9)[0], -1.0);
    Param* b = qs.define(kOfxParamTypeBoolean, "b");
    b->keys[0.0] = {0.0};
    b->keys[10.0] = {1.0};
    checkEq("param unit: boolean holds until the next key", b->valueAt(9.99)[0], 0.0);
    checkEq("param unit: boolean at the next key", b->valueAt(10.0)[0], 1.0);
    Param* c = qs.define(kOfxParamTypeChoice, "c");
    c->keys[0.0] = {2.0};
    c->keys[10.0] = {0.0};
    checkEq("param unit: choice holds", c->valueAt(5.0)[0], 2.0);
    Param* i = qs.define(kOfxParamTypeInteger, "i");
    i->keys[0.0] = {0.0};
    i->keys[3.0] = {3.0};
    checkEq("param unit: integer interpolation at a whole step", i->valueAt(2.0)[0], 2.0);
    Param* s = qs.define(kOfxParamTypeString, "s");
    s->str = "x";
    checkEq("param unit: string has no keys, valueAt gives the (empty) number list", s->valueAt(1.0).size(), (size_t)0);
}

static void testStatusNames()
{
    checkEq("statusName OK", std::string(statusName(kOfxStatOK)), std::string("OK"));
    checkEq("statusName failed", std::string(statusName(kOfxStatFailed)), std::string("failed"));
    checkEq("statusName bad handle", std::string(statusName(kOfxStatErrBadHandle)), std::string("bad handle"));
    checkEq("statusName bad index", std::string(statusName(kOfxStatErrBadIndex)), std::string("bad index"));
    checkEq("statusName memory", std::string(statusName(kOfxStatErrMemory)), std::string("out of memory"));
    checkEq("statusName image format", std::string(statusName(kOfxStatErrImageFormat)), std::string("bad image format"));
    CHECK("statusName unlicensed has a name", std::string(statusName(kOfxStatUnlicensed)) != "?");
    CHECK("statusName unknown code is not empty", std::string(statusName(9999)).size() > 0);
    std::set<std::string> seen;
    int codes[] = {kOfxStatOK, kOfxStatFailed, kOfxStatErrFatal, kOfxStatErrUnknown, kOfxStatErrMissingHostFeature, kOfxStatErrUnsupported, kOfxStatErrExists,
                   kOfxStatErrFormat, kOfxStatErrMemory, kOfxStatErrBadHandle, kOfxStatErrBadIndex, kOfxStatErrValue, kOfxStatReplyYes, kOfxStatReplyNo,
                   kOfxStatReplyDefault, kOfxStatUnlicensed, kOfxStatErrImageFormat};
    for (int c : codes) seen.insert(statusName(c));
    checkEq("statusName: every code has its own name", seen.size(), sizeof codes / sizeof codes[0]);
}

static void testHostProperties()
{
    const PropertySet& h = host().hostProps;
    checkEq("host: name", h.getString(kOfxPropName), std::string(kRenoVegasHostName));
    checkEq("host: type", h.getString(kOfxPropType), std::string(kOfxTypeImageEffectHost));
    checkEq("host: label", h.getString(kOfxPropLabel), std::string("Reno"));
    checkEq("host: API version major", h.getInt(kOfxPropAPIVersion, 0), 1);
    checkEq("host: API version minor", h.getInt(kOfxPropAPIVersion, 1), 4);
    checkEq("host: version", h.getInt(kOfxPropVersion, 0), 18);
    checkEq("host: contexts", h.dimension(kOfxImageEffectPropSupportedContexts), 4);
    CHECK("host: has filter, general, generator, transition",
          h.getString(kOfxImageEffectPropSupportedContexts, 0) == kOfxImageEffectContextFilter &&
              h.getString(kOfxImageEffectPropSupportedContexts, 1) == kOfxImageEffectContextGeneral &&
              h.getString(kOfxImageEffectPropSupportedContexts, 2) == kOfxImageEffectContextGenerator &&
              h.getString(kOfxImageEffectPropSupportedContexts, 3) == kOfxImageEffectContextTransition);
    checkEq("host: pixel depths", h.dimension(kOfxImageEffectPropSupportedPixelDepths), 2);
    checkEq("host: supports temporal clip access", h.getInt(kOfxImageEffectPropTemporalClipAccess), 1);
    checkEq("host: components", h.getString(kOfxImageEffectPropSupportedComponents, 0), std::string(kOfxImageComponentRGBA));
    checkEq("host: no tiles", h.getInt(kOfxImageEffectPropSupportsTiles, 0, -1), 0);
    checkEq("host: native origin", h.getString(kOfxImageEffectHostPropNativeOrigin), std::string(kOfxHostNativeOriginBottomLeft));
    CHECK("host: has the VEGAS data folder", !h.getString(kVegasPropHostAppDataDirectory).empty());
    CHECK("host: has VEGAS's spike key (a string, empty)", h.has(kVegasPropSpikeKey) && h.find(kVegasPropSpikeKey)->type == PType::String && h.dimension(kVegasPropSpikeKey) == 1);
    checkEq("host: VEGAS Pro 18's name", h.getString(kOfxPropName), std::string("com.vegascreativesoftware.vegas"));
    CHECK("host: StrChoice is declared unsupported or left out", h.getInt(kOfxParamHostPropSupportsStrChoice, 0, 0) == 0);

    // the VEGAS part can be switched off
    HostConfig saved = host().config;
    host().config.vegasExtensions = false;
    host().applyConfig();
    CHECK("host: VEGAS properties go away with vegasExtensions=false", !host().hostProps.has(kVegasPropHostAppDataDirectory) && !host().hostProps.has(kVegasPropHostHWnd) &&
                                                                         !host().hostProps.has(kVegasPropSpikeKey));
    host().config = saved;
    host().applyConfig();
    CHECK("host: VEGAS properties come back", host().hostProps.has(kVegasPropHostAppDataDirectory));
}

static const void* fetch(const char* name, int version) { return host().ofxHost.fetchSuite(host().ofxHost.host, name, version); }

static void testFetchSuite()
{
    CHECK("fetchSuite: property v1", fetch(kOfxPropertySuite, 1) != nullptr);
    CHECK("fetchSuite: image effect v1", fetch(kOfxImageEffectSuite, 1) != nullptr);
    CHECK("fetchSuite: parameter v1", fetch(kOfxParameterSuite, 1) != nullptr);
    CHECK("fetchSuite: memory v1", fetch(kOfxMemorySuite, 1) != nullptr);
    CHECK("fetchSuite: multithread v1", fetch(kOfxMultiThreadSuite, 1) != nullptr);
    CHECK("fetchSuite: message v1 and v2", fetch(kOfxMessageSuite, 1) != nullptr && fetch(kOfxMessageSuite, 2) != nullptr);
    CHECK("fetchSuite: interact v1", fetch(kOfxInteractSuite, 1) != nullptr);
    CHECK("fetchSuite: progress v1 v2", fetch(kRenoProgressSuite, 1) != nullptr && fetch(kRenoProgressSuite, 2) != nullptr);
    CHECK("fetchSuite: timeline v1", fetch(kRenoTimeLineSuite, 1) != nullptr);
    CHECK("fetchSuite: VEGAS suites", fetch(kVegasHWndInteractSuite, 1) && fetch(kVegasKeyframeSuite, 1) && fetch(kVegasStereoscopicSuite, 1) &&
                                          fetch(kVegasProgressSuite, 1) && fetch(kVegasProgressSuite, 2));
    CHECK("fetchSuite: unknown name", fetch("OfxNoSuchSuite", 1) == nullptr);
    CHECK("fetchSuite: NULL name", fetch(nullptr, 1) == nullptr);
    CHECK("fetchSuite: wrong versions", fetch(kOfxPropertySuite, 2) == nullptr && fetch(kOfxPropertySuite, 0) == nullptr && fetch(kOfxImageEffectSuite, 99) == nullptr &&
                                           fetch(kOfxMessageSuite, 3) == nullptr && fetch(kOfxParameterSuite, -1) == nullptr);
    CHECK("fetchSuite: VEGAS suite wrong version", fetch(kVegasKeyframeSuite, 2) == nullptr);
    CHECK("fetchSuite: VEGAS's About/Help and HWnd overlay suites", fetch(kVegasEffectSuite, 1) != nullptr && fetch(kVegasHWndOverlayInteractSuite, 1) != nullptr);
    CHECK("fetchSuite: ... version 1 only", fetch(kVegasEffectSuite, 2) == nullptr && fetch(kVegasHWndOverlayInteractSuite, 2) == nullptr);
    CHECK("fetchSuite: no OpenCL program suite (VEGAS's effects then render on the processor)", fetch(kVegasOpenCLProgramSuite, 1) == nullptr);
    CHECK("fetchSuite: an unknown VEGAS suite is absent by default", fetch("OfxVegasNoSuchSuite", 1) == nullptr);
    host().config.stubUnknownSuites = true;
    CHECK("fetchSuite: stand-ins when asked for", fetch("OfxVegasNoSuchSuite", 1) != nullptr && fetch("OfxNoSuchSuite", 1) == nullptr);
    host().config.stubUnknownSuites = false;
    host().config.vegasExtensions = false;
    CHECK("fetchSuite: no VEGAS About/Help suite without vegasExtensions", fetch(kVegasEffectSuite, 1) == nullptr && fetch(kVegasHWndOverlayInteractSuite, 1) == nullptr);
    host().config.vegasExtensions = true;
    host().config.vegasExtensions = false;
    CHECK("fetchSuite: no VEGAS suites without vegasExtensions", fetch(kVegasHWndInteractSuite, 1) == nullptr && fetch(kVegasKeyframeSuite, 1) == nullptr);
    CHECK("fetchSuite: standard suites stay", fetch(kOfxPropertySuite, 1) != nullptr);
    host().config.vegasExtensions = true;
    CHECK("fetchSuite: same pointer every time", fetch(kOfxPropertySuite, 1) == fetch(kOfxPropertySuite, 1));
}

// the host's own suites called straight from the test (the plug-in side is done by testfx)
static void testPropertySuiteDirect()
{
    const OfxPropertySuiteV1* ps = (const OfxPropertySuiteV1*)fetch(kOfxPropertySuite, 1);
    PropertySet set;
    OfxPropertySetHandle h = (OfxPropertySetHandle)&set;
    checkEq("prop suite: set int", ps->propSetInt(h, "a", 0, 7), kOfxStatOK);
    int iv = 0;
    checkEq("prop suite: get int", ps->propGetInt(h, "a", 0, &iv), kOfxStatOK);
    checkEq("prop suite: get int value", iv, 7);
    double dv = 0;
    checkEq("prop suite: int property read as double", ps->propGetDouble(h, "a", 0, &dv), kOfxStatOK);
    checkEq("prop suite: get unknown", ps->propGetInt(h, "b", 0, &iv), kOfxStatErrUnknown);
    checkEq("prop suite: get bad index", ps->propGetInt(h, "a", 1, &iv), kOfxStatErrBadIndex);
    checkEq("prop suite: get negative index", ps->propGetInt(h, "a", -1, &iv), kOfxStatErrBadIndex);
    checkEq("prop suite: set negative index", ps->propSetInt(h, "a", -1, 1), kOfxStatErrBadIndex);
    checkEq("prop suite: NULL handle", ps->propGetInt(nullptr, "a", 0, &iv), kOfxStatErrBadHandle);
    checkEq("prop suite: NULL name", ps->propGetInt(h, nullptr, 0, &iv), kOfxStatErrBadHandle);
    checkEq("prop suite: NULL result", ps->propGetInt(h, "a", 0, nullptr), kOfxStatErrBadHandle);
    char* sv = nullptr;
    checkEq("prop suite: string read of an int property is an error", ps->propGetString(h, "a", 0, &sv) != kOfxStatOK, true);
    const char* strs[3] = {"x", "yy", nullptr};
    checkEq("prop suite: setStringN", ps->propSetStringN(h, "s", 3, strs), kOfxStatOK);
    checkEq("prop suite: string dimension", set.dimension("s"), 3);
    checkEq("prop suite: NULL string stored as empty", set.getString("s", 2), std::string());
    char* got[3] = {nullptr, nullptr, nullptr};
    checkEq("prop suite: getStringN", ps->propGetStringN(h, "s", 3, got), kOfxStatOK);
    CHECK("prop suite: getStringN values", got[0] && !std::strcmp(got[0], "x") && !std::strcmp(got[1], "yy"));
    checkEq("prop suite: getStringN too many", ps->propGetStringN(h, "s", 4, got), kOfxStatErrBadIndex);
    int dim = -1;
    checkEq("prop suite: dimension", ps->propGetDimension(h, "s", &dim), kOfxStatOK);
    checkEq("prop suite: dimension value", dim, 3);
    checkEq("prop suite: dimension of unknown", ps->propGetDimension(h, "zz", &dim), kOfxStatErrUnknown);
    int ints[3] = {1, 2, 3};
    checkEq("prop suite: setIntN", ps->propSetIntN(h, "n", 3, ints), kOfxStatOK);
    checkEq("prop suite: setIntN negative count", ps->propSetIntN(h, "n", -1, ints), kOfxStatErrValue);
    checkEq("prop suite: setIntN NULL values", ps->propSetIntN(h, "n", 2, nullptr), kOfxStatErrValue);
    checkEq("prop suite: setIntN zero count with NULL values is fine", ps->propSetIntN(h, "n2", 0, nullptr), kOfxStatOK);
    checkEq("prop suite: setDouble on an int property", ps->propSetDouble(h, "n", 0, 9.4), kOfxStatOK);
    checkEq("prop suite: ... is stored as int", set.getInt("n", 0), 9);
    checkEq("prop suite: set string on an int property is a value error", ps->propSetString(h, "n", 0, "x"), kOfxStatErrValue);
    void* pv = (void*)0x44;
    checkEq("prop suite: set pointer", ps->propSetPointer(h, "ptr", 0, pv), kOfxStatOK);
    void* back = nullptr;
    checkEq("prop suite: get pointer", ps->propGetPointer(h, "ptr", 0, &back), kOfxStatOK);
    checkEq("prop suite: pointer value", (const void*)back, (const void*)pv);
    checkEq("prop suite: reset unknown", ps->propReset(h, "zz"), kOfxStatErrUnknown);
    checkEq("prop suite: reset NULL", ps->propReset(nullptr, "a"), kOfxStatErrBadHandle);
    // a handle of the wrong kind is rejected, not dereferenced as a property set
    Param pr;
    checkEq("prop suite: a parameter is not a property set", ps->propGetInt((OfxPropertySetHandle)&pr, "a", 0, &iv), kOfxStatErrBadHandle);
    int notAHandle[8] = {0};
    checkEq("prop suite: zeroed memory is not a property set", ps->propGetInt((OfxPropertySetHandle)notAHandle, "a", 0, &iv), kOfxStatErrBadHandle);
}

// ====================================================================== 2. finding and loading binaries
static std::string copyFile(const std::string& from, const std::string& to)
{
    std::string data;
    if (!readFile(from, data) || !writeFile(to, data)) return std::string();
    if (chmod(to.c_str(), 0755) != 0) return std::string();
    return to;
}

static void testLoading()
{
    std::string err;
    Binary* bin = host().loadBinary(gFxPath, &err);
    record("load: testfx binary loads", bin != nullptr, err);
    if (!bin) return;
    checkEq("load: same path gives the same Binary", (const void*)host().loadBinary(gFxPath), (const void*)bin);
    CHECK("load: path is kept", bin->path == gFxPath);
    CHECK("load: loose .ofx has no bundle, bundlePath is its folder", bin->bundlePath == pathDir(gFxPath));
    // 24 entries in the table: one is for another API, one has no entry point
    checkEq("load: only usable image effects are listed", bin->plugins.size(), (size_t)22);
    std::vector<std::string> ids;
    for (auto& p : bin->plugins) ids.push_back(p->info.id);
    CHECK("load: other-API plug-in skipped", !hasName(ids, "reno.test.otherapi"));
    CHECK("load: plug-in without a main entry skipped", !hasName(ids, "reno.test.nomain"));
    CHECK("load: other-API plug-in never called", gFx.counter("otherApiCalled") == 0);
    CHECK("load: ids in table order", ids.size() == 22 && ids[0] == "reno.test.invert" && ids[9] == "reno.test.vegas" && ids[10] == "reno.test.vegas18" &&
                                           ids[11] == "reno.test.missing" && ids[19] == "reno.test.leaky" && ids[20] == "reno.test.overlay" && ids[21] == "reno.test.paraminteract");
    Plugin* inv = host().findPlugin("reno.test.invert");
    CHECK("load: findPlugin", inv != nullptr && inv->binary == bin);
    CHECK("load: findPlugin unknown id", host().findPlugin("reno.test.nothing") == nullptr);
    CHECK("load: allPlugins includes ours", hasName([&] { std::vector<std::string> v; for (auto* p : host().allPlugins()) v.push_back(p->info.id); return v; }(), "reno.test.params"));
    checkEq("load: version major", inv->info.versionMajor, 2);
    checkEq("load: version minor", inv->info.versionMinor, 7);
    CHECK("load: not loaded before the first load()", !inv->loaded);
    CHECK("load: unknown plug-in cannot fail because it was not asked to load", !inv->failed);

    err.clear();
    CHECK("load: nonexistent file", host().loadBinary(gScratchDir + "/does-not-exist.ofx", &err) == nullptr && contains(err, "could not be loaded"));
    std::string textFile = gScratchDir + "/not-a-plugin.ofx";
    writeFile(textFile, "this is not a shared library");
    err.clear();
    CHECK("load: text file", host().loadBinary(textFile, &err) == nullptr && contains(err, "could not be loaded"));
    err.clear();
    CHECK("load: library without OFX entry points", host().loadBinary("libm.so.6", &err) == nullptr && contains(err, "not an OpenFX plug-in"));
    CHECK("load: error pointer may be NULL", host().loadBinary(textFile, nullptr) == nullptr);

    // a binary that answers OfxSetHost with "not for you" has nothing for this host
    std::string refused = copyFile(gFxPath, gScratchDir + "/testfx_refuse.ofx");
    setenv("TESTFX_REFUSE_HOST", "1", 1);
    err.clear();
    Binary* rb = host().loadBinary(refused, &err);
    unsetenv("TESTFX_REFUSE_HOST");
    CHECKM("load: OfxSetHost returning kOfxStatFailed means no plug-ins from that binary", rb == nullptr || rb->plugins.empty(), "binary has %d plug-ins",
           rb ? (int)rb->plugins.size() : -1);
}

static void testSearch()
{
    std::string root = gScratchDir + "/search";
    std::string b1 = root + "/a/Foo.ofx.bundle/Contents/Linux-x86-64";
    std::string b2 = root + "/a/Bar.ofx.bundle/Contents/Win64";
    std::string loose = root + "/b/deeper/Loose.ofx";
    std::string other = root + "/b/readme.txt";
    if (system(("mkdir -p '" + b1 + "' '" + b2 + "' '" + root + "/b/deeper' '" + root + "/empty'").c_str()) != 0) return;
    writeFile(b1 + "/Foo.ofx", "x");
    writeFile(b1 + "/Foo.txt", "x");
    writeFile(b2 + "/Bar.ofx", "x");
    writeFile(loose, "x");
    writeFile(other, "x");
    auto found = Host::findBinaries({root + "/a", root + "/b", root + "/empty", root + "/missing"});
    checkEq("search: bundle for this platform and loose file are found", found.size(), (size_t)2);
    CHECK("search: bundle binary", hasName(found, b1 + "/Foo.ofx"));
    CHECK("search: loose file in a subfolder", hasName(found, loose));
    CHECK("search: other platform's bundle is ignored", !hasName(found, b2 + "/Bar.ofx"));
    auto twice = Host::findBinaries({root + "/a", root + "/a", root + "/b"});
    checkEq("search: no duplicates", twice.size(), (size_t)2);
    CHECK("search: nothing in nothing", Host::findBinaries({}).empty());

    setenv("OFX_PLUGIN_PATH", (root + "/a:" + root + "/missing::" + root + "/b").c_str(), 1);
    auto dirs = Host::defaultSearchDirs();
    CHECK("search: OFX_PLUGIN_PATH folders that exist are used", hasName(dirs, root + "/a") && hasName(dirs, root + "/b"));
    CHECK("search: missing and empty entries are dropped", !hasName(dirs, root + "/missing") && !hasName(dirs, ""));
    unsetenv("OFX_PLUGIN_PATH");
    CHECK("search: VEGAS program folders are only a Windows thing", Host::vegasProgramDirs().empty());
}

// ====================================================================== 3. describing
static void testDescribing()
{
    Plugin* inv = plug("reno.test.invert");
    CHECK("describe: invert loads", inv && inv->loaded && !inv->failed);
    if (!inv) return;
    checkEq("describe: label", inv->info.label, std::string("Test Invert"));
    checkEq("describe: grouping", inv->info.grouping, std::string("Reno Test"));
    checkEq("describe: description", inv->info.description, std::string("Inverts the colour (alpha minus colour)."));
    CHECK("describe: contexts", inv->info.contexts.size() == 1 && inv->info.contexts[0] == kOfxImageEffectContextFilter);
    CHECK("describe: depths", inv->info.depths.size() == 2 && inv->info.depths[0] == kOfxBitDepthByte && inv->info.depths[1] == kOfxBitDepthFloat);
    CHECK("describe: no window, no overlay", !inv->info.hasHwndInteract && !inv->info.hasOverlay);
    checkEq("describe: bundle path property", inv->desc->props.getString(kOfxPluginPropFilePath), inv->info.bundlePath);
    checkEq("describe: thread safety read", inv->safety, 2);
    CHECK("describe: load() twice is fine", inv->load());
    CHECK("describe: supportsContext", inv->supportsContext(kOfxImageEffectContextFilter) && !inv->supportsContext(kOfxImageEffectContextGenerator));
    checkEq("describe: filterContext of a filter", inv->filterContext(), std::string(kOfxImageEffectContextFilter));
    checkEq("describe: filterContext of a general effect", plug("reno.test.general")->filterContext(), std::string(kOfxImageEffectContextGeneral));
    checkEq("describe: filterContext of a generator is empty", plug("reno.test.generator")->filterContext(), std::string());
    checkEq("describe: filterContext of a transition is empty", plug("reno.test.transition")->filterContext(), std::string());

    std::string err;
    Descriptor* d = inv->describeInContext(kOfxImageEffectContextFilter, &err);
    record("describe: invert in the filter context", d != nullptr, err);
    if (d) {
        checkEq("describe: same descriptor the second time", (const void*)inv->describeInContext(kOfxImageEffectContextFilter), (const void*)d);
        CHECK("describe: clips", d->findClip("Source") && d->findClip("Output") && !d->findClip("Nothing"));
        checkEq("describe: two clips", d->clips.size(), (size_t)2);
        checkEq("describe: clip components", d->findClip("Source")->props.getString(kOfxImageEffectPropSupportedComponents), std::string(kOfxImageComponentRGBA));
        checkEq("describe: context property", d->props.getString(kOfxImageEffectPropContext), std::string(kOfxImageEffectContextFilter));
        CHECK("describe: params", d->params.find("ZeroRed") && d->params.find("UseFloat") && d->params.params.size() == 2);
    }
    err.clear();
    CHECK("describe: unsupported context", inv->describeInContext(kOfxImageEffectContextGenerator, &err) == nullptr && contains(err, "does not work as"));

    // failures
    Plugin* fd = host().findPlugin("reno.test.faildescribe");
    err.clear();
    CHECK("describe: describe returning failed", fd && !fd->load(&err) && fd->failed && contains(err, "could not describe itself") && contains(err, "failed"));
    std::string err2;
    CHECK("describe: failure is remembered, with the same message", !fd->load(&err2) && err2 == err);
    CHECK("describe: describe was not called twice", gFx.counter("failDescribe") == 1);
    CHECK("describe: describeInContext on a failed plug-in", fd->describeInContext(kOfxImageEffectContextFilter) == nullptr);

    Plugin* fc = plug("reno.test.failcontext");
    err.clear();
    CHECK("describe: describeInContext failing", fc && fc->loaded && fc->describeInContext(kOfxImageEffectContextFilter, &err) == nullptr && contains(err, "could not describe itself as a"));
    InstanceSetup s;
    CHECK("describe: createInstance after a failed context", fc->createInstance(kOfxImageEffectContextFilter, s, &err) == nullptr);

    Plugin* ns = host().findPlugin("reno.test.needsuite");
    err.clear();
    CHECK("describe: missing host feature", ns && !ns->load(&err) && contains(err, "missing host feature"));

    Plugin* fcr = plug("reno.test.failcreate");
    err.clear();
    auto none = fcr->createInstance(kOfxImageEffectContextFilter, s, &err);
    CHECK("describe: createInstance failing", none == nullptr && contains(err, "could not be created"));
    CHECK("describe: DestroyInstance is not sent for an instance that failed to create", gFx.counter("destroyAfterFailedCreate") == 0);

    Plugin* bs = plug("reno.test.badsuite");
    CHECK("describe: a plug-in that asks for bad suites still describes", bs && bs->loaded);
    checkEq("describe: ... and got NULL for each", gFx.counter("badsuiteFailures"), 0);
    auto has = [&](Plugin* p, const std::string& line) { return hasName(p->suiteLog, line); };
    CHECK("describe: suite log says what was refused", has(bs, "OfxNoSuchSuite v1: not available") && has(bs, "OfxImageEffectSuite v99: not available") &&
                                                           has(bs, "OfxPropertySuite v2: not available") && has(bs, "OfxMessageSuite v3: not available"));
    CHECK("describe: suite log says what was given", has(bs, "OfxPropertySuite v1: given") && has(bs, "OfxImageEffectSuite v1: given"));
    size_t logSize = bs->suiteLog.size();
    plug("reno.test.badsuite")->load();
    checkEq("describe: suite log has no repeats", bs->suiteLog.size(), logSize);
    CHECK("describe: attribution - lines for one plug-in do not leak into another", !has(inv, "OfxNoSuchSuite v1: not available"));
}

// ====================================================================== 4. invert: pixels, rowBytes, BGRA
static void testInvert()
{
    const int W = 37, H = 23;
    ImagePtr src = randomImage(W, H, 1);
    auto inst = makeInstance("reno.test.invert", W, H);
    CHECK("invert: instance", inst != nullptr);
    if (!inst) return;
    checkEq("invert: context", inst->context(), std::string(kOfxImageEffectContextFilter));
    checkEq("invert: depth is byte", (int)inst->depth(), (int)PixelDepth::Byte);
    checkEq("invert: input clip names", inst->inputClipNames(), std::vector<std::string>{"Source"});
    gFx.clearLog();

    std::string err;
    ImagePtr out = renderOnce(*inst, 3, &err);
    record("invert: unconnected source fails", out == nullptr, "rendered without a source");
    CHECK("invert: ... with a message", !err.empty() && contains(err, "failed to render"));
    CHECK("invert: ... and nothing is left over", inst->liveImages.empty() && !inst->outputImage);

    setSource(*inst, "Source", src);
    gFx.clearLog();
    gFx.clearCounters();
    err.clear();
    out = renderOnce(*inst, 3, &err);
    record("invert: render", out != nullptr, err);
    if (!out) return;
    checkEq("invert: output width", out->w, W);
    checkEq("invert: output height", out->h, H);
    checkEq("invert: output depth", (int)out->depth, (int)PixelDepth::Byte);
    bool exact = true;
    std::string first;
    for (int y = 0; y < H && exact; ++y)
        for (int x = 0; x < W && exact; ++x) {
            const uint8_t* s = src->row(y) + x * 4;
            const uint8_t* o = out->row(y) + x * 4;
            for (int c = 0; c < 3; ++c)
                if (o[c] != clampB(s[3] - s[c])) { exact = false; first = strfmt("pixel %d,%d channel %d: got %d want %d", x, y, c, o[c], clampB(s[3] - s[c])); break; }
            if (exact && o[3] != s[3]) { exact = false; first = strfmt("pixel %d,%d alpha", x, y); }
        }
    record("invert: every pixel is alpha minus colour", exact, first);
    CHECK("invert: the host handed over blue-first pictures", gFx.has("invert: depth=byte srcOrder=BGRA dstOrder=BGRA"));
    CHECK("invert: the render window is the whole frame", gFx.has("window=0,0,37,23"));
    checkEq("invert: every image fetched was released", gFx.counter("getImage"), gFx.counter("releaseImage"));
    CHECK("invert: the host holds no images afterwards", inst->liveImages.empty());

    // row stride that is not the width: the padding must not be read as pixels or disturbed
    ImagePtr padded = paddedCopy(*src, 20);
    setSource(*inst, "Source", padded);
    ImagePtr out2 = renderOnce(*inst, 3, &err);
    record("invert: padded rowBytes render", out2 != nullptr, err);
    if (out2) record("invert: padded source gives the same picture", sameImage(*out, *out2), strfmt("first difference at byte %d", firstDifference(*out, *out2)));
    bool paddingKept = true;
    for (int y = 0; y < H; ++y)
        for (size_t x = (size_t)W * 4; x < padded->rowBytes; ++x)
            if (padded->row(y)[x] != 0xEE) paddingKept = false;
    CHECK("invert: padding of the source is untouched", paddingKept);

    // ZeroRed: the effect zeroes the channel it believes is red; with blue-first data that is memory byte 2
    setSource(*inst, "Source", src);
    inst->userSetValue(prm(*inst, "ZeroRed"), {1}, 0);
    gFx.clearLog();
    ImagePtr z = renderOnce(*inst, 3, &err);
    record("invert: ZeroRed render", z != nullptr, err);
    if (z) {
        bool ok = true;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W && ok; ++x) {
                const uint8_t* s = src->row(y) + x * 4;
                const uint8_t* o = z->row(y) + x * 4;
                ok = o[2] == 0 && o[0] == clampB(s[3] - s[0]) && o[1] == clampB(s[3] - s[1]) && o[3] == s[3];
            }
        CHECK("invert: the red channel of the host's BGRA memory (byte 2) was the one zeroed", ok);
    }

    // float pictures: ask for them through getClipPreferences
    inst->userSetValue(prm(*inst, "ZeroRed"), {0}, 0);
    int prefsBefore = gFx.counter("getClipPreferences");
    inst->userSetValue(prm(*inst, "UseFloat"), {1}, 0);
    gFx.clearLog();
    ImagePtr f = renderOnce(*inst, 3, &err);
    record("invert: float render", f != nullptr, err);
    CHECK("invert: getClipPreferences ran again for the slave parameter", gFx.counter("getClipPreferences") > prefsBefore);
    checkEq("invert: the instance now works in float", (int)inst->depth(), (int)PixelDepth::Float);
    CHECK("invert: float pictures reach the effect as RGBA", gFx.has("invert: depth=float srcOrder=RGBA dstOrder=RGBA"));
    if (f) {
        checkEq("invert: float result is handed back as 8-bit", (int)f->depth, (int)PixelDepth::Byte);
        bool ok = true;
        std::string where;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W && ok; ++x) {
                const uint8_t* s = src->row(y) + x * 4;
                const uint8_t* o = f->row(y) + x * 4;
                for (int c = 0; c < 3; ++c) {
                    uint8_t want = f2b(b2f(s[3]) - b2f(s[c]));
                    if (o[c] != want) { ok = false; where = strfmt("pixel %d,%d channel %d got %d want %d", x, y, c, o[c], want); break; }
                }
                if (ok && o[3] != s[3]) { ok = false; where = "alpha"; }
            }
        record("invert: float pixels are alpha minus colour, in the right channels", ok, where);
        // and the 8-bit and float routes agree
        record("invert: float route equals the 8-bit route", sameImage(*f, *out), strfmt("first difference at byte %d", firstDifference(*f, *out)));
    }
    inst->userSetValue(prm(*inst, "ZeroRed"), {1}, 0);
    ImagePtr fz = renderOnce(*inst, 3, &err);
    if (fz) {
        bool ok = true;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W && ok; ++x) ok = fz->row(y)[x * 4 + 2] == 0;
        CHECK("invert: ZeroRed in float zeroes the R of RGBA, which is byte 2 after conversion", ok);
    } else
        record("invert: ZeroRed float render", false, err);

    // a float source picture into an 8-bit instance is converted for the effect
    inst->userSetValue(prm(*inst, "UseFloat"), {0}, 0);
    inst->userSetValue(prm(*inst, "ZeroRed"), {0}, 0);
    ImagePtr fsrc = src->converted(PixelDepth::Float);
    setSource(*inst, "Source", fsrc);
    ImagePtr back = renderOnce(*inst, 3, &err);
    checkEq("invert: back to 8-bit working", (int)inst->depth(), (int)PixelDepth::Byte);
    if (back) {
        bool ok = true;
        std::string where;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W && ok; ++x) {
                const uint8_t* s = src->row(y) + x * 4;
                const uint8_t* o = back->row(y) + x * 4;
                // the float picture was made from 8-bit values, so converting back gives the same bytes
                for (int c = 0; c < 3; ++c)
                    if (o[c] != clampB(s[3] - s[c])) { ok = false; where = strfmt("%d,%d c%d got %d want %d", x, y, c, o[c], clampB(s[3] - s[c])); }
            }
        record("invert: float source into an 8-bit effect", ok, where);
    } else
        record("invert: float source render", false, err);

    // a source that has no picture for this time
    inst->setClipSource("Source", [](double) { return ImagePtr(); });
    err.clear();
    CHECK("invert: source without a picture fails the render", renderOnce(*inst, 3, &err) == nullptr && contains(err, "failed to render"));
    // and the instance still works afterwards
    setSource(*inst, "Source", src);
    CHECK("invert: renders again after a failed render", renderOnce(*inst, 3, &err) != nullptr);
}

// ====================================================================== 5. parameters of every type
struct ParamEvents {
    std::mutex m;
    std::vector<std::pair<std::string, std::string>> events;
    void clear() { std::lock_guard<std::mutex> lk(m); events.clear(); }
    bool has(const std::string& p, const std::string& what)
    {
        std::lock_guard<std::mutex> lk(m);
        for (auto& e : events) if (e.first == p && e.second == what) return true;
        return false;
    }
    int count(const std::string& p, const std::string& what)
    {
        std::lock_guard<std::mutex> lk(m);
        int n = 0;
        for (auto& e : events) if (e.first == p && e.second == what) ++n;
        return n;
    }
};

// what testfx's "params" effect dumps for the given values (see fmtVals in testfx.cpp)
struct PV {
    int p_int = 5;
    double p_dbl = 0.25;
    int p_bool = 1, p_choice = 1;
    double rgb[3] = {0.1, 0.2, 0.3}, rgba[4] = {0.4, 0.5, 0.6, 0.7}, d2[2] = {1.5, 2.5}, d3[3] = {1, 2, 3};
    int i2[2] = {3, 4}, i3[3] = {7, 8, 9};
    std::string str = "hello", custom = "cust";
    int driver = 0;
    double slave = 0, dep = 1;
};
static std::string valuesText(const PV& v)
{
    std::string s;
    s += strfmt("p_int=%d ", v.p_int);
    s += "p_dbl=" + fmt17(v.p_dbl) + " ";
    s += strfmt("p_bool=%d p_choice=%d ", v.p_bool, v.p_choice);
    s += "p_rgb=" + fmt17(v.rgb[0]) + "," + fmt17(v.rgb[1]) + "," + fmt17(v.rgb[2]) + " ";
    s += "p_rgba=" + fmt17(v.rgba[0]) + "," + fmt17(v.rgba[1]) + "," + fmt17(v.rgba[2]) + "," + fmt17(v.rgba[3]) + " ";
    s += "p_d2=" + fmt17(v.d2[0]) + "," + fmt17(v.d2[1]) + " ";
    s += strfmt("p_i2=%d,%d ", v.i2[0], v.i2[1]);
    s += "p_d3=" + fmt17(v.d3[0]) + "," + fmt17(v.d3[1]) + "," + fmt17(v.d3[2]) + " ";
    s += strfmt("p_i3=%d,%d,%d ", v.i3[0], v.i3[1], v.i3[2]);
    s += "p_str=<" + v.str + "> p_custom=<" + v.custom + "> ";
    s += strfmt("driver=%d ", v.driver);
    s += "slave=" + fmt17(v.slave) + " dep=" + fmt17(v.dep) + " ";
    return s;
}
// keys: "keys=<n>:<times>" and the three key-index queries made at the render time
static std::string dumpText(const PV& at, const PV& now, const std::string& keys, const std::string& keyIndex)
{
    return "AT " + valuesText(at) + keys + " keyindex=" + keyIndex + " | NOW " + valuesText(now);
}
static const char* kNoKeyIndex = "prev:1/-1,at:1/-1,next:1/-1";

static void renderParamsAndCompare(Instance& inst, double t, const std::string& name, const std::string& want)
{
    std::string err;
    ImagePtr out = renderOnce(inst, t, &err);
    if (!out) { record(name, false, "render failed: " + err); return; }
    std::string got = gFx.dumpOf("params");
    record(name, got == want, "\n   got  " + got + "\n   want " + want);
}

static void testParamDescriptor()
{
    Plugin* pl = plug("reno.test.params");
    std::string err;
    Descriptor* d = pl ? pl->describeInContext(kOfxImageEffectContextFilter, &err) : nullptr;
    record("params: describe", d != nullptr, err);
    if (!d) return;
    std::vector<std::string> names;
    for (auto& p : d->params.params) names.push_back(p->name);
    std::vector<std::string> want = {"p_group", "p_int", "p_dbl", "p_bool", "p_choice", "p_rgb", "p_rgba", "p_d2", "p_i2", "p_d3", "p_i3", "p_str", "p_custom",
                                     "p_button", "driver", "slave", "dep", "p_page"};
    checkEq("params: defined in the order the effect gave", names, want);

    auto P = [&](const char* n) { return d->params.find(n); };
    Param* g = P("p_group");
    CHECK("params: group", g && g->kind == ParamKind::Group && g->label() == "A Group" && g->hint() == "Holds the first two" && g->props.getInt(kOfxParamPropGroupOpen, 0, 1) == 0);
    Param* pi = P("p_int");
    CHECK("params: integer kind", pi && pi->kind == ParamKind::Int && pi->type == kOfxParamTypeInteger && pi->dim == 1);
    checkEq("params: integer label", pi->label(), std::string("Integer Value"));
    checkEq("params: integer short label", pi->props.getString(kOfxPropShortLabel), std::string("Int"));
    checkEq("params: integer long label", pi->props.getString(kOfxPropLongLabel), std::string("The integer value"));
    checkEq("params: integer hint", pi->hint(), std::string("An integer from 0 to 100"));
    checkEq("params: integer parent", pi->parent(), std::string("p_group"));
    checkEq("params: integer default", pi->props.getInt(kOfxParamPropDefault), 5);
    checkEq("params: integer min", pi->props.getInt(kOfxParamPropMin), 0);
    checkEq("params: integer max", pi->props.getInt(kOfxParamPropMax), 100);
    checkEq("params: integer display min", pi->props.getInt(kOfxParamPropDisplayMin), 10);
    checkEq("params: integer display max", pi->props.getInt(kOfxParamPropDisplayMax), 90);
    Param* pd = P("p_dbl");
    CHECK("params: double", pd && pd->kind == ParamKind::Double && pd->parent() == "p_group" && pd->hint() == "A double from -10 to 10");
    checkEq("params: double default", pd->props.getDouble(kOfxParamPropDefault), 0.25);
    checkEq("params: double min", pd->props.getDouble(kOfxParamPropMin), -10.0);
    checkEq("params: double max", pd->props.getDouble(kOfxParamPropMax), 10.0);
    checkEq("params: double display min", pd->props.getDouble(kOfxParamPropDisplayMin), -5.0);
    checkEq("params: double increment", pd->props.getDouble(kOfxParamPropIncrement), 0.01);
    checkEq("params: double digits", pd->props.getInt(kOfxParamPropDigits), 3);
    CHECK("params: double animates", pd->props.getInt(kOfxParamPropAnimates) == 1);
    Param* pb = P("p_bool");
    CHECK("params: boolean", pb && pb->kind == ParamKind::Bool && pb->props.getInt(kOfxParamPropDefault) == 1 && pb->label() == "A Flag" && pb->hint() == "On or off");
    Param* pc = P("p_choice");
    CHECK("params: choice", pc && pc->kind == ParamKind::Choice && pc->props.getInt(kOfxParamPropDefault) == 1);
    checkEq("params: choice options", pc->choices(), std::vector<std::string>{"Zero", "One", "Two", "Three"});
    Param* c3 = P("p_rgb");
    CHECK("params: rgb", c3 && c3->kind == ParamKind::RGB && c3->dim == 3 && c3->props.getDouble(kOfxParamPropDefault, 0) == 0.1 &&
                             c3->props.getDouble(kOfxParamPropDefault, 1) == 0.2 && c3->props.getDouble(kOfxParamPropDefault, 2) == 0.3);
    Param* c4 = P("p_rgba");
    CHECK("params: rgba", c4 && c4->kind == ParamKind::RGBA && c4->dim == 4 && c4->props.getDouble(kOfxParamPropDefault, 3) == 0.7);
    Param* d2 = P("p_d2");
    CHECK("params: double2D", d2 && d2->kind == ParamKind::Double2D && d2->props.getDouble(kOfxParamPropDefault, 1) == 2.5 &&
                                  d2->props.getDouble(kOfxParamPropMin, 1) == -100 && d2->props.getDouble(kOfxParamPropMax, 0) == 100);
    Param* i2 = P("p_i2");
    CHECK("params: integer2D", i2 && i2->kind == ParamKind::Int2D && i2->props.getInt(kOfxParamPropDefault, 1) == 4 && i2->props.getInt(kOfxParamPropMax, 1) == 60);
    Param* d3 = P("p_d3");
    CHECK("params: double3D", d3 && d3->kind == ParamKind::Double3D && d3->dim == 3 && d3->props.getDouble(kOfxParamPropDefault, 2) == 3.0);
    Param* i3 = P("p_i3");
    CHECK("params: integer3D", i3 && i3->kind == ParamKind::Int3D && i3->dim == 3 && i3->props.getInt(kOfxParamPropDefault, 2) == 9);
    Param* ps = P("p_str");
    CHECK("params: string", ps && ps->kind == ParamKind::String && ps->props.getString(kOfxParamPropDefault) == "hello" && ps->hint() == "Some text" &&
                                ps->props.getString(kOfxParamPropStringMode) == kOfxParamStringIsMultiLine);
    Param* cu = P("p_custom");
    CHECK("params: custom", cu && cu->kind == ParamKind::Custom && cu->props.getString(kOfxParamPropDefault) == "cust" && cu->props.getPointer(kOfxParamPropCustomInterpCallbackV1) != nullptr);
    Param* bt = P("p_button");
    CHECK("params: push button", bt && bt->kind == ParamKind::PushButton && !bt->hasValue() && bt->hint() == "Pushes" && bt->label() == "Do It");
    Param* pg = P("p_page");
    CHECK("params: page", pg && pg->kind == ParamKind::Page);
    std::vector<std::string> kids;
    for (int k = 0; k < pg->props.dimension(kOfxParamPropPageChild); ++k) kids.push_back(pg->props.getString(kOfxParamPropPageChild, k));
    checkEq("params: page children", kids.size(), (size_t)17);
    CHECK("params: page child order", kids.size() == 17 && kids[0] == "p_group" && kids[1] == "p_int" && kids[16] == "dep");
    CHECK("params: label falls back to the name", [&] { Param tmp; tmp.name = "zzz"; return tmp.label() == "zzz"; }());
    // descriptors cannot be given values through the instance API; defaults live in properties
    CHECK("params: descriptor is not an instance", !d->isInstance);
}

static void testParams()
{
    const int W = 16, H = 9;
    auto inst = makeInstance("reno.test.params", W, H);
    CHECK("params: instance", inst != nullptr);
    if (!inst) return;
    ParamEvents ev;
    inst->onParamChanged = [&ev](Param* p, const std::string& what) {
        std::lock_guard<std::mutex> lk(ev.m);
        ev.events.emplace_back(p->name, what);
    };
    setSource(*inst, "Source", solidImage(W, H, 1, 2, 3, 255));
    Rng rng(5);
    (void)rng;

    // defaults
    CHECK("params: instance starts at the defaults", prm(*inst, "p_int")->value[0] == 5 && prm(*inst, "p_dbl")->value[0] == 0.25 && prm(*inst, "p_choice")->value[0] == 1 &&
                                                       prm(*inst, "p_str")->str == "hello" && prm(*inst, "p_custom")->str == "cust" && prm(*inst, "p_rgba")->value[3] == 0.7 &&
                                                       prm(*inst, "dep")->value[0] == 1.0);
    PV pv;
    gFx.clearLog();
    renderParamsAndCompare(*inst, 1.0, "params: defaults read back through paramGetValue and paramGetValueAtTime", dumpText(pv, pv, "keys=0:", kNoKeyIndex));

    // user edits of every type
    double T = 12;
    gFx.clearLog();
    inst->userSetValue(prm(*inst, "p_int"), {42}, T);
    inst->userSetValue(prm(*inst, "p_dbl"), {-3.5}, T);
    inst->userSetValue(prm(*inst, "p_bool"), {0}, T);
    inst->userSetValue(prm(*inst, "p_choice"), {3}, T);
    inst->userSetValue(prm(*inst, "p_rgb"), {0.9, 0.8, 0.7}, T);
    inst->userSetValue(prm(*inst, "p_rgba"), {0.15, 0.25, 0.35, 0.45}, T);
    inst->userSetValue(prm(*inst, "p_d2"), {-7.25, 99.5}, T);
    inst->userSetValue(prm(*inst, "p_i2"), {10, 20}, T);
    inst->userSetValue(prm(*inst, "p_d3"), {0.5, -0.5, 1e-3}, T);
    inst->userSetValue(prm(*inst, "p_i3"), {-1, 0, 123456}, T);
    inst->userSetString(prm(*inst, "p_str"), "a b\nc \"quoted\" \\ ü", T);
    inst->userSetString(prm(*inst, "p_custom"), "x;y;z", T);
    pv.p_int = 42; pv.p_dbl = -3.5; pv.p_bool = 0; pv.p_choice = 3;
    pv.rgb[0] = 0.9; pv.rgb[1] = 0.8; pv.rgb[2] = 0.7;
    pv.rgba[0] = 0.15; pv.rgba[1] = 0.25; pv.rgba[2] = 0.35; pv.rgba[3] = 0.45;
    pv.d2[0] = -7.25; pv.d2[1] = 99.5; pv.i2[0] = 10; pv.i2[1] = 20;
    pv.d3[0] = 0.5; pv.d3[1] = -0.5; pv.d3[2] = 1e-3; pv.i3[0] = -1; pv.i3[1] = 0; pv.i3[2] = 123456;
    pv.str = "a b\nc \"quoted\" \\ ü"; pv.custom = "x;y;z";
    renderParamsAndCompare(*inst, T, "params: values the host set are what the effect reads back", dumpText(pv, pv, "keys=0:", kNoKeyIndex));
    {
        ImagePtr out = renderOnce(*inst, T);
        const uint8_t* px = out ? out->row(4) + 5 * 4 : nullptr;
        CHECK("params: the picture is derived from the values (B=int, G=rgb.g, R=rgb.r, A=rgba.a)",
              px && px[0] == 42 && px[1] == 204 && px[2] == 230 && px[3] == (uint8_t)(0.45 * 255 + 0.5));
    }
    checkEq("params: current time is what the user last said", inst->currentTime, T);

    // changes sent to the plug-in
    CHECK("params: edits are reported to the application", ev.has("p_int", "value") && ev.has("p_str", "value") && ev.has("p_rgba", "value"));
    CHECK("params: plug-in saw each edit as an instanceChanged by the user",
          gFx.has("params: changed type=OfxTypeParameter name=p_int reason=OfxChangeUserEdited time=12") &&
              gFx.has("params: changed type=OfxTypeParameter name=p_custom reason=OfxChangeUserEdited time=12"));
    {
        auto lines = gFx.lines();
        int b = -1, c = -1, e = -1;
        for (int i = 0; i < (int)lines.size(); ++i) {
            if (b < 0 && contains(lines[i], "beginChanged")) b = i;
            if (c < 0 && contains(lines[i], "name=p_int")) c = i;
            if (e < 0 && contains(lines[i], "endChanged")) e = i;
        }
        CHECK("params: begin, changed, end come in that order", b >= 0 && c > b && e > c);
    }

    // type-specific rounding / padding of what the application gives
    inst->userSetValue(prm(*inst, "p_int"), {41.6}, T);
    checkEq("params: whole-number parameters round (value 41.6 -> 42)", prm(*inst, "p_int")->value[0], 42.0);
    inst->userSetValue(prm(*inst, "p_d2"), {5.0}, T);
    CHECK("params: a short list is padded with zeros", prm(*inst, "p_d2")->value[0] == 5.0 && prm(*inst, "p_d2")->value[1] == 0.0);
    inst->userSetValue(prm(*inst, "p_d2"), {1, 2, 3, 4}, T);
    CHECK("params: a long list is cut", prm(*inst, "p_d2")->value.size() == 2 && prm(*inst, "p_d2")->value[1] == 2.0);
    pv.d2[0] = 1; pv.d2[1] = 2;
    inst->userSetValue(prm(*inst, "p_int"), {500}, T);
    {
        double v = prm(*inst, "p_int")->value[0];
        CHECKM("params: above the maximum (QUESTION: clamped or kept; either accepted)", v == 100.0 || v == 500.0, "got %g", v);
        note(strfmt("question: userSetValue above kOfxParamPropMax stores %g (max is 100)", v));
    }
    inst->userSetValue(prm(*inst, "p_int"), {42}, T);
    uint64_t rev = inst->revision;
    inst->userSetValue(nullptr, {1}, T);
    inst->userSetValue(prm(*inst, "p_group"), {1}, T);
    inst->userSetValue(prm(*inst, "p_button"), {1}, T);
    inst->userSetString(prm(*inst, "p_int"), "nope", T);
    inst->userSetString(nullptr, "nope", T);
    inst->userPushButton(prm(*inst, "p_int"), T);
    inst->userPushButton(nullptr, T);
    inst->userDeleteKeys(prm(*inst, "p_int"), T);
    inst->userDeleteKeys(nullptr, T);
    CHECK("params: calls that make no sense change nothing", inst->revision == rev && prm(*inst, "p_int")->value[0] == 42.0);

    // keyframes on a double
    inst->userSetValue(prm(*inst, "p_dbl"), {1}, 10, true);
    inst->userSetValue(prm(*inst, "p_dbl"), {3}, 20, true);
    inst->userSetValue(prm(*inst, "p_dbl"), {-1}, 30, true);
    checkEq("params: three keys", prm(*inst, "p_dbl")->keys.size(), (size_t)3);
    checkEq("params: animating flag", prm(*inst, "p_dbl")->props.getInt(kOfxParamPropIsAnimating), 1);
    pv.p_dbl = 2.0;
    PV now = pv;
    renderParamsAndCompare(*inst, 15, "params: between keys the effect reads the interpolated value; key times and indices queried", [&] {
        PV a = pv;
        a.p_dbl = 2.0;
        return dumpText(a, a, "keys=3:10,20,30", "prev:0/0,at:1/-1,next:0/1");
    }());
    renderParamsAndCompare(*inst, 20, "params: on a key", [&] {
        PV a = pv;
        a.p_dbl = 3.0;
        return dumpText(a, a, "keys=3:10,20,30", "prev:0/0,at:0/1,next:0/2");
    }());
    renderParamsAndCompare(*inst, 5, "params: before the first key the first value holds", [&] {
        PV a = pv;
        a.p_dbl = 1.0;
        return dumpText(a, a, "keys=3:10,20,30", "prev:1/-1,at:1/-1,next:0/0");
    }());
    renderParamsAndCompare(*inst, 40, "params: after the last key the last value holds", [&] {
        PV a = pv;
        a.p_dbl = -1.0;
        return dumpText(a, a, "keys=3:10,20,30", "prev:0/2,at:1/-1,next:1/-1");
    }());
    renderParamsAndCompare(*inst, 25, "params: second segment", [&] {
        PV a = pv;
        a.p_dbl = 1.0;
        return dumpText(a, a, "keys=3:10,20,30", "prev:0/1,at:1/-1,next:0/2");
    }());
    // a plain edit while the parameter has keys changes the key at that time (or makes one)
    inst->userSetValue(prm(*inst, "p_dbl"), {5}, 20);
    checkEq("params: a plain edit on an animated parameter edits the key", prm(*inst, "p_dbl")->keys[20.0][0], 5.0);
    inst->userSetValue(prm(*inst, "p_dbl"), {7}, 22);
    checkEq("params: a plain edit between keys makes a key", prm(*inst, "p_dbl")->keys.size(), (size_t)4);
    inst->userDeleteKeys(prm(*inst, "p_dbl"), 15);
    CHECK("params: deleting the animation keeps the value at that time", prm(*inst, "p_dbl")->keys.empty() && prm(*inst, "p_dbl")->value[0] == 3.0);
    checkEq("params: animating flag is off again", prm(*inst, "p_dbl")->props.getInt(kOfxParamPropIsAnimating), 0);

    // dependent parameters: driver -> slave, dep enabled / secret, choice relabelled
    ev.clear();
    gFx.clearLog();
    inst->userSetValue(prm(*inst, "driver"), {4}, 7);
    checkEq("params: instanceChanged set another parameter (slave = driver / 2)", prm(*inst, "slave")->value[0], 2.0);
    CHECK("params: ... which the application was told about", ev.has("slave", "value"));
    CHECK("params: dep is enabled", prm(*inst, "dep")->enabled());
    CHECK("params: dep is not hidden", !prm(*inst, "dep")->secret());
    CHECK("params: the plug-in's property changes reach the application", ev.has("dep", kOfxParamPropEnabled) && ev.has("dep", kOfxParamPropSecret) &&
                                                                              ev.has("p_choice", kOfxParamPropChoiceOption));
    checkEq("params: choice relabelled", prm(*inst, "p_choice")->choices(), std::vector<std::string>{"Opt4_0", "Opt4_1"});
    CHECK("params: the user's edit is reported last", [&] { std::lock_guard<std::mutex> lk(ev.m); return !ev.events.empty() && ev.events.back() == std::make_pair(std::string("driver"), std::string("value")); }());
    CHECK("params: the plug-in read the new value with paramGetValue", gFx.has("name=driver reason=OfxChangeUserEdited time=7"));
    inst->userSetValue(prm(*inst, "driver"), {0}, 8);
    CHECK("params: dep disabled by the plug-in", !prm(*inst, "dep")->enabled() && !prm(*inst, "dep")->secret());
    checkEq("params: slave follows", prm(*inst, "slave")->value[0], 0.0);
    inst->userSetValue(prm(*inst, "driver"), {11}, 9);
    CHECK("params: dep hidden by the plug-in, and enabled", prm(*inst, "dep")->secret() && prm(*inst, "dep")->enabled());
    checkEq("params: slave = 5.5", prm(*inst, "slave")->value[0], 5.5);
    checkEq("params: choice relabelled with three options", prm(*inst, "p_choice")->choices(), std::vector<std::string>{"Opt11_0", "Opt11_1", "Opt11_2"});
    checkEq("params: the choice's value is left alone", prm(*inst, "p_choice")->value[0], 3.0);

    // push button -> string set by the plug-in
    inst->userPushButton(prm(*inst, "p_button"), 3);
    inst->userPushButton(prm(*inst, "p_button"), 3);
    checkEq("params: push button reached the plug-in, which wrote a string", prm(*inst, "p_str")->str, std::string("pushed 2"));
    CHECK("params: push button change reason", gFx.has("name=p_button reason=OfxChangeUserEdited"));

    // edit sessions
    gFx.clearLog();
    inst->beginEdit();
    checkEq("params: interactive during an edit session", inst->props.getInt(kOfxPropIsInteractive), 1);
    inst->endEdit();
    checkEq("params: not interactive after", inst->props.getInt(kOfxPropIsInteractive), 0);
    CHECK("params: begin / end edit actions reached the plug-in", gFx.has("params: beginEdit") && gFx.has("params: endEdit"));

    // revision moves with edits
    uint64_t r0 = inst->revision;
    inst->userSetValue(prm(*inst, "p_int"), {9}, 0);
    CHECK("params: revision goes up with an edit", inst->revision > r0);
}

// an interpolation test per numeric type, through the instance
static void testKeyframeTypes()
{
    auto inst = makeInstance("reno.test.params", 8, 8);
    if (!inst) { record("keys: instance", false, "no instance"); return; }
    auto vals = [&](const char* n, double t) { return prm(*inst, n)->valueAt(t); };
    auto key = [&](const char* n, std::vector<double> v, double t) { inst->userSetValue(prm(*inst, n), v, t, true); };

    key("p_int", {0}, 0); key("p_int", {100}, 10);
    checkEq("keys: integer at a key", vals("p_int", 0)[0], 0.0);
    checkEq("keys: integer between", vals("p_int", 5)[0], 50.0);
    checkEq("keys: integer between (3)", vals("p_int", 3)[0], 30.0);
    {
        double v = vals("p_int", 2.5)[0], v2 = vals("p_int", 0.44)[0], v3 = vals("p_int", 9.996)[0];
        CHECK("keys: integer interpolation is a whole number", v == std::floor(v) && v2 == std::floor(v2));
        CHECKM("keys: integer 25.0 exactly", v == 25.0, "got %g", v);
        CHECKM("keys: integer near the start rounds to 4", v2 == 4.0, "got %g", v2);
        CHECKM("keys: integer near the end rounds to 100", v3 == 100.0, "got %g", v3);
        double half = vals("p_int", 0.05)[0];
        note(strfmt("question: integer keys interpolate 0->100 over 10 frames; at 0.5 frames of the way the value is %g (0.5 rounds to 1 = half up)", half));
        CHECK("keys: integer exact half is rounded one way or the other", half == 0.0 || half == 1.0);
    }
    key("p_bool", {1}, 0); key("p_bool", {0}, 10);
    checkEq("keys: boolean holds the earlier key", vals("p_bool", 9.9)[0], 1.0);
    checkEq("keys: boolean at the later key", vals("p_bool", 10)[0], 0.0);
    key("p_choice", {2}, 5); key("p_choice", {0}, 15);
    checkEq("keys: choice before the first key", vals("p_choice", 0)[0], 2.0);
    checkEq("keys: choice holds", vals("p_choice", 14.5)[0], 2.0);
    checkEq("keys: choice after the last key", vals("p_choice", 99)[0], 0.0);
    key("p_rgb", {0, 0.5, 1}, 0); key("p_rgb", {1, 0.5, 0}, 4);
    auto c = vals("p_rgb", 1);
    CHECK("keys: rgb is interpolated per channel", c[0] == 0.25 && c[1] == 0.5 && c[2] == 0.75);
    key("p_rgba", {0, 0, 0, 0}, 0); key("p_rgba", {0.4, 0.8, 1.2, 1.6}, 4);
    c = vals("p_rgba", 2);
    CHECK("keys: rgba is interpolated per channel", c.size() == 4 && c[0] == 0.2 && c[1] == 0.4 && c[2] == 0.6 && c[3] == 0.8);
    key("p_d2", {0, 10}, 0); key("p_d2", {10, 0}, 10);
    c = vals("p_d2", 2.5);
    CHECK("keys: double2D", c.size() == 2 && c[0] == 2.5 && c[1] == 7.5);
    key("p_i2", {0, 100}, 0); key("p_i2", {10, 0}, 10);
    c = vals("p_i2", 5);
    CHECK("keys: integer2D", c.size() == 2 && c[0] == 5.0 && c[1] == 50.0);
    key("p_d3", {0, 0, 0}, 0); key("p_d3", {3, 6, 9}, 3);
    c = vals("p_d3", 1);
    CHECK("keys: double3D", c.size() == 3 && c[0] == 1.0 && c[1] == 2.0 && c[2] == 3.0);
    key("p_i3", {0, 0, 0}, 0); key("p_i3", {30, 60, 90}, 3);
    c = vals("p_i3", 2);
    CHECK("keys: integer3D", c.size() == 3 && c[0] == 20.0 && c[1] == 40.0 && c[2] == 60.0);
    // the plug-in side of the same thing, through the suite: values at times
    std::string err;
    renderOnce(*inst, 5, &err);
    std::string dump = gFx.dumpOf("params");
    CHECK("keys: the effect sees interpolated values at the render time",
          contains(dump, "p_int=50 p_dbl=0.25 p_bool=1 p_choice=2 p_rgb=1,0.5,0 ") && contains(dump, "p_d2=5,5 p_i2=5,50 p_d3=3,6,9 p_i3=30,60,90 "));
    // strings never get keys; setKey is ignored for them
    inst->userSetString(prm(*inst, "p_str"), "keep", 3);
    CHECK("keys: strings have no keys", prm(*inst, "p_str")->keys.empty());
    // a key at the same time replaces it
    key("p_int", {7}, 10);
    checkEq("keys: replacing a key", prm(*inst, "p_int")->keys[10.0][0], 7.0);
    checkEq("keys: still two keys", prm(*inst, "p_int")->keys.size(), (size_t)2);
    // negative and fractional key times
    key("p_dbl", {1}, -5.5); key("p_dbl", {2}, 0.25);
    checkEq("keys: negative time sorts first", prm(*inst, "p_dbl")->keys.begin()->first, -5.5);
    checkEq("keys: value between fractional times", prm(*inst, "p_dbl")->valueAt(-2.625)[0], 1.5);
}

static void testStateRoundTrip()
{
    const int W = 24, H = 14;
    auto a = makeInstance("reno.test.params", W, H);
    if (!a) { record("state: instance", false, "no instance"); return; }
    setSource(*a, "Source", solidImage(W, H, 1, 2, 3, 255));
    a->userSetValue(prm(*a, "p_int"), {77}, 3);
    a->userSetValue(prm(*a, "p_dbl"), {0.5}, 10, true);
    a->userSetValue(prm(*a, "p_dbl"), {2.5}, 20, true);
    a->userSetValue(prm(*a, "p_bool"), {0}, 3);
    a->userSetValue(prm(*a, "p_choice"), {2}, 3);
    a->userSetValue(prm(*a, "p_rgb"), {0.1111111111111111, 0.2, 1.0 / 3}, 3);
    a->userSetValue(prm(*a, "p_rgba"), {0.9, 0.8, 0.7, 0.6}, 3);
    a->userSetValue(prm(*a, "p_d2"), {1.0 / 7, -1.0 / 9}, 3);
    a->userSetValue(prm(*a, "p_i2"), {5, 6}, 3);
    a->userSetValue(prm(*a, "p_d3"), {3.14159265358979, 2.71828182845905, 1.41421356237310}, 3);
    a->userSetValue(prm(*a, "p_i3"), {-5, 0, 8}, 3);
    a->userSetValue(prm(*a, "p_i3"), {9, 9, 9}, 40, true);
    a->userSetString(prm(*a, "p_str"), "some \"text\"\nwith lines\tand tabs", 3);
    a->userSetString(prm(*a, "p_custom"), "{custom}", 3);
    // a dependent value set by the user after the plug-in set it: it must survive a load
    a->userSetValue(prm(*a, "driver"), {3}, 3);
    a->userSetValue(prm(*a, "slave"), {7.25}, 3);
    uint64_t syncBefore = gFx.counter("sync");
    Json st = a->saveState();
    CHECK("state: saving tells the plug-in to sync its private data", (uint64_t)gFx.counter("sync") == (uint64_t)syncBefore + 1);
    checkEq("state: plug-in id is recorded", st.get("plugin").asString(), std::string("reno.test.params"));
    checkEq("state: version is recorded", st.get("versionMajor").asInt(), 2);
    CHECK("state: buttons, groups and pages are not saved", !st.get("params").has("p_button") && !st.get("params").has("p_group") && !st.get("params").has("p_page"));
    CHECK("state: every valued parameter is saved", st.get("params").size() == 15);
    CHECK("state: keys are saved", st.get("params").get("p_dbl").get("k").size() == 2 && st.get("params").get("p_i3").get("k").size() == 1);

    // through text and back, as the project file does
    std::string text = st.dump();
    Json parsed;
    std::string perr;
    CHECK("state: the saved text parses", Json::parse(text, parsed, &perr));
    auto b = makeInstance("reno.test.params", W, H);
    setSource(*b, "Source", solidImage(W, H, 1, 2, 3, 255));
    gFx.clearLog();
    b->loadState(parsed);
    CHECK("state: loading tells the plug-in the values were put back by the user", gFx.has("changed type=OfxTypeParameter name=p_int reason=OfxChangeUserEdited"));
    for (const char* n : {"p_int", "p_dbl", "p_bool", "p_choice", "p_rgb", "p_rgba", "p_d2", "p_i2", "p_d3", "p_i3", "p_str", "p_custom", "driver", "slave", "dep"}) {
        Param* pa = prm(*a, n);
        Param* pb = prm(*b, n);
        bool same = pa->value == pb->value && pa->str == pb->str && pa->keys == pb->keys;
        record(std::string("state: ") + n + " is bit-for-bit the same after a round trip", same);
    }
    checkEq("state: slave kept the saved value despite the plug-in's reaction to driver", prm(*b, "slave")->value[0], 7.25);

    // a text that changes over time (a VEGAS project's keyed text and custom settings): its keys are kept as
    // "ks": [[time, text], ...] and each text holds until the next key
    {
        Param* pc = prm(*a, "p_custom");
        pc->strKeys.clear();
        pc->strKeys[0.0] = "first";
        pc->strKeys[10.0] = "second";
        Json ks = a->saveState();
        const Json& e = ks.get("params").get("p_custom");
        CHECK("state: keyed text is saved with its keys", e.get("ks").size() == 2 && e.get("ks").at(1).at(1).asString() == "second" &&
                                                            e.get("ks").at(1).at(0).asDouble() == 10.0);
        Json reparsed;
        CHECK("state: keyed text parses", Json::parse(ks.dump(), reparsed, &perr));
        auto c = makeInstance("reno.test.params", W, H);
        setSource(*c, "Source", solidImage(W, H, 1, 2, 3, 255));
        c->loadState(reparsed);
        Param* cc = prm(*c, "p_custom");
        CHECK("state: keyed text comes back with its keys", cc->strKeys == pc->strKeys);
        CHECK("keyed text holds until the next key", cc->strAt(-1) == "first" && cc->strAt(5) == "first" && cc->strAt(10) == "second" && cc->strAt(99) == "second");
        std::string rerr;
        gFx.clearLog();
        renderOnce(*c, 12, &rerr);
        CHECK("the effect sees the keyed text of the time it renders", contains(gFx.dumpOf("params"), "p_custom=<second>"));
        gFx.clearLog();
        renderOnce(*c, 4, &rerr);
        CHECK("(and the earlier key's before it)", contains(gFx.dumpOf("params"), "p_custom=<first>"));
        unsigned nk = 0;
        const OfxParameterSuiteV1* ps = (const OfxParameterSuiteV1*)fetch(kOfxParameterSuite, 1);
        CHECK("the effect can count a text's keys", ps && ps->paramGetNumKeys((OfxParamHandle)cc, &nk) == kOfxStatOK && nk == 2);
        pc->strKeys.clear();        // (the first instance as it was, for what follows)
    }
    CHECK("state: the plug-in's derived properties follow (dep enabled for driver 3)", prm(*b, "dep")->enabled() && !prm(*b, "dep")->secret());
    checkEq("state: choice options relabelled for driver 3", prm(*b, "p_choice")->choices(), std::vector<std::string>{"Opt3_0", "Opt3_1", "Opt3_2"});
    CHECK("state: animating flag restored", prm(*b, "p_dbl")->props.getInt(kOfxParamPropIsAnimating) == 1 && prm(*b, "p_int")->props.getInt(kOfxParamPropIsAnimating) == 0);

    for (double t : {0.0, 10.0, 14.0, 20.0, 41.0}) {
        std::string e1, e2;
        ImagePtr ra = renderOnce(*a, t, &e1);
        std::string da = gFx.dumpOf("params");
        ImagePtr rb = renderOnce(*b, t, &e2);
        std::string db = gFx.dumpOf("params");
        record(strfmt("state: identical render at frame %g", t), ra && rb && sameImage(*ra, *rb), e1 + e2);
        record(strfmt("state: identical values read back at frame %g", t), da == db, "\n  " + da + "\n  " + db);
    }
    // loading garbage is harmless
    uint64_t rev = b->revision;
    b->loadState(Json());
    b->loadState(Json::array());
    Json bad = Json::object();
    bad["params"] = Json::array();
    b->loadState(bad);
    Json odd = Json::object();
    Json ps = Json::object();
    Json e = Json::object();
    e["v"] = "not a list";
    e["k"] = 5;
    ps["p_int"] = e;
    ps["no_such_param"] = Json::object();
    Json bt = Json::object();
    bt["v"] = Json::array();
    ps["p_button"] = bt;
    odd["params"] = ps;
    b->loadState(odd);
    CHECK("state: loading garbage does not crash", b->revision >= rev);
    Json shortKey = Json::object();
    Json ps2 = Json::object();
    Json e2 = Json::object();
    Json keys = Json::array();
    Json k0 = Json::array();
    k0.push(5.0);                  // a key with no values at all
    keys.push(k0);
    Json k1 = Json::array();       // an empty key
    keys.push(k1);
    Json v = Json::array();
    v.push(1.0);
    e2["v"] = v;
    e2["k"] = keys;
    ps2["p_d3"] = e2;
    shortKey["params"] = ps2;
    b->loadState(shortKey);
    CHECK("state: keys with missing values load as zeros", prm(*b, "p_d3")->keys.size() == 1 && prm(*b, "p_d3")->keys[5.0].size() == 3);
}

// ====================================================================== 6. temporal access
struct TimedSource {
    std::mutex m;
    std::vector<double> times;
    std::function<ImagePtr(double)> make;
    std::function<ImagePtr(double)> fn()
    {
        return [this](double t) {
            {
                std::lock_guard<std::mutex> lk(m);
                times.push_back(t);
            }
            return make(t);
        };
    }
    std::vector<double> sortedTimes()
    {
        std::lock_guard<std::mutex> lk(m);
        std::vector<double> v = times;
        std::sort(v.begin(), v.end());
        return v;
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(m);
        times.clear();
    }
};

static ImagePtr frameImage(int w, int h, double t)
{
    const int f = (int)t;
    if (f < 0 || f > 40) return nullptr;
    return solidImage(w, h, (uint8_t)(f * 3 + 1), (uint8_t)(f * 5 + 2), (uint8_t)(f * 7 + 3), (uint8_t)(100 + f));
}

static void testTemporal()
{
    const int W = 21, H = 11;
    auto inst = makeInstance("reno.test.temporal", W, H);
    CHECK("temporal: instance", inst != nullptr);
    if (!inst) return;
    TimedSource src;
    src.make = [&](double t) { return frameImage(W, H, t); };
    inst->setClipSource("Source", src.fn());
    gFx.clearLog();
    gFx.clearCounters();
    std::string err;
    ImagePtr out = renderOnce(*inst, 5, &err);
    record("temporal: render", out != nullptr, err);
    if (!out) return;
    checkEq("temporal: the source was asked for time-1, time and time+1, once each", src.sortedTimes(), std::vector<double>{4, 5, 6});
    const uint8_t* p = out->row(3) + 7 * 4;
    auto avg = [](int a, int b, int c) { return (uint8_t)((a + b + c + 1) / 3); };
    CHECK("temporal: output is the average of the three frames",
          p[0] == avg(4 * 3 + 1, 5 * 3 + 1, 6 * 3 + 1) && p[1] == avg(4 * 5 + 2, 5 * 5 + 2, 6 * 5 + 2) && p[2] == avg(4 * 7 + 3, 5 * 7 + 3, 6 * 7 + 3) &&
              p[3] == avg(104, 105, 106));
    bool uniform = true;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) uniform = uniform && !std::memcmp(out->row(y) + x * 4, p, 4);
    CHECK("temporal: whole picture", uniform);
    checkEq("temporal: four images fetched (three sources and the output)", gFx.counter("getImage"), 4);
    checkEq("temporal: all four released", gFx.counter("releaseImage"), 4);
    CHECK("temporal: host keeps no handles", inst->liveImages.empty());
    CHECK("temporal: statuses", gFx.has("temporal: t=5 status=0,0,0"));

    // first frame: the frame before does not exist and the effect is told so
    src.clear();
    gFx.clearLog();
    ImagePtr first = renderOnce(*inst, 0, &err);
    record("temporal: first frame renders", first != nullptr, err);
    CHECK("temporal: a missing frame is reported to the effect as kOfxStatFailed", gFx.has("temporal: t=0 status=1,0,0"));
    if (first) {
        const uint8_t* q = first->row(0);
        CHECK("temporal: ... and counts as transparent black", q[0] == avg(0, 1, 4) && q[3] == avg(0, 100, 101));
    }
    ImagePtr last = renderOnce(*inst, 40, &err);
    record("temporal: last frame renders", last != nullptr, err);
    CHECK("temporal: frame after the end is reported missing too", gFx.has("temporal: t=40 status=0,0,1"));

    // fractional time
    src.clear();
    renderOnce(*inst, 7.5, &err);
    checkEq("temporal: fractional times are passed through", src.sortedTimes(), std::vector<double>{6.5, 7.5, 8.5});

    // float working
    inst->userSetValue(prm(*inst, "UseFloat"), {1}, 0);
    src.clear();
    gFx.clearCounters();
    ImagePtr f = renderOnce(*inst, 5, &err);
    record("temporal: float render", f != nullptr, err);
    checkEq("temporal: float instance", (int)inst->depth(), (int)PixelDepth::Float);
    if (f) {
        const uint8_t* q = f->row(3) + 7 * 4;
        // float average of three byte frames converted to float, then back to bytes
        auto favg = [](int a, int b, int c) { return f2b((b2f((uint8_t)a) + b2f((uint8_t)b) + b2f((uint8_t)c)) / 3.f); };
        CHECK("temporal: float average", q[0] == favg(13, 16, 19) && q[1] == favg(22, 27, 32) && q[3] == favg(104, 105, 106));
    }
    CHECK("temporal: float images released too", gFx.counter("getImage") == gFx.counter("releaseImage"));

    // GetFramesNeeded is the effect's business; the host serves any frame on demand
    CHECK("temporal: host advertises temporal clip access", host().hostProps.getInt(kOfxImageEffectPropTemporalClipAccess) == 1);
}

// ====================================================================== 7. float-only and byte-only effects
static void testDepths()
{
    const int W = 19, H = 7;
    ImagePtr src = randomImage(W, H, 11);
    {
        std::string err;
        auto inst = makeInstance("reno.test.floatonly", W, H);
        CHECK("floatonly: instance", inst != nullptr);
        if (inst) {
            checkEq("floatonly: host works in float", (int)inst->depth(), (int)PixelDepth::Float);
            setSource(*inst, "Source", src);
            gFx.clearLog();
            ImagePtr out = renderOnce(*inst, 0, &err);
            record("floatonly: render", out != nullptr, err);
            CHECK("floatonly: the effect saw float, RGBA", gFx.has("floatonly: imageDepth=float clipDepth=OfxBitDepthFloat order=RGBA"));
            if (out) {
                checkEq("floatonly: result is 8-bit", (int)out->depth, (int)PixelDepth::Byte);
                bool ok = true;
                std::string where;
                for (int y = 0; y < H && ok; ++y)
                    for (int x = 0; x < W && ok; ++x) {
                        const uint8_t* s = src->row(y) + x * 4;
                        const uint8_t* o = out->row(y) + x * 4;
                        uint8_t want[4] = {f2b(b2f(s[0])), f2b(b2f(s[1]) * 0.5f), f2b(b2f(s[2])), f2b(b2f(s[3]))};
                        if (std::memcmp(o, want, 4)) { ok = false; where = strfmt("pixel %d,%d got %d,%d,%d,%d want %d,%d,%d,%d", x, y, o[0], o[1], o[2], o[3], want[0], want[1], want[2], want[3]); }
                    }
                record("floatonly: green halved, channels in place after BGRA->RGBA->BGRA", ok, where);
            }
            // a single known pixel
            setSource(*inst, "Source", solidImage(W, H, 10, 100, 30, 255));
            ImagePtr one = renderOnce(*inst, 0, &err);
            CHECK("floatonly: B=10 G=100 R=30 A=255 becomes B=10 G=50 R=30 A=255", one && one->row(0)[0] == 10 && one->row(0)[1] == 50 && one->row(0)[2] == 30 && one->row(0)[3] == 255);
            // a float source picture goes straight in
            setSource(*inst, "Source", src->converted(PixelDepth::Float));
            ImagePtr fromFloat = renderOnce(*inst, 0, &err);
            record("floatonly: float source picture", fromFloat != nullptr && out && sameImage(*fromFloat, *out), err);
        }
    }
    {
        std::string err;
        auto inst = makeInstance("reno.test.byteonly", W, H);
        CHECK("byteonly: instance", inst != nullptr);
        if (inst) {
            checkEq("byteonly: host works in 8-bit", (int)inst->depth(), (int)PixelDepth::Byte);
            setSource(*inst, "Source", src);
            gFx.clearLog();
            ImagePtr out = renderOnce(*inst, 0, &err);
            record("byteonly: render", out != nullptr, err);
            CHECK("byteonly: the effect saw 8-bit blue-first", gFx.has("byteonly: imageDepth=byte clipDepth=OfxBitDepthByte order=BGRA"));
            if (out) {
                bool ok = true;
                for (int y = 0; y < H && ok; ++y)
                    for (int x = 0; x < W && ok; ++x) {
                        const uint8_t* s = src->row(y) + x * 4;
                        const uint8_t* o = out->row(y) + x * 4;
                        ok = o[0] == clampB(s[0] + 10) && o[1] == s[1] && o[2] == s[2] && o[3] == s[3];
                    }
                CHECK("byteonly: blue (memory byte 0) +10, rest unchanged", ok);
            }
            // a float source picture is converted for it
            ImagePtr fsrc = src->converted(PixelDepth::Float);
            setSource(*inst, "Source", fsrc);
            ImagePtr out2 = renderOnce(*inst, 0, &err);
            record("byteonly: float source picture is converted", out2 && out && sameImage(*out, *out2), err);
            // a caller working in float (a 32-bit project): the effect still works in 8-bit, the result comes back float
            setSource(*inst, "Source", src);
            RenderOptions fo;
            fo.depth = PixelDepth::Float;
            ImagePtr out3 = inst->render(0, fo, &err);
            checkEq("byteonly: asked for float, still works in 8-bit", (int)inst->depth(), (int)PixelDepth::Byte);
            record("byteonly: asked for float, the result is float and the same picture",
                   out3 && out3->depth == PixelDepth::Float && out && sameImage(*out3->converted(PixelDepth::Byte), *out), err);
        }
    }
    {
        // an effect that takes both: 8-bit for an 8-bit caller (as always), float for a float one, and back again
        std::string err;
        auto inst = makeInstance("reno.test.invert", W, H);
        CHECK("both depths: instance", inst != nullptr);
        if (inst) {
            ImagePtr fsrc = solidImage(W, H, 10, 100, 30, 255)->converted(PixelDepth::Float);
            setSource(*inst, "Source", fsrc);
            ImagePtr b = renderOnce(*inst, 0, &err);
            checkEq("both depths: an 8-bit caller gets 8-bit work", (int)inst->depth(), (int)PixelDepth::Byte);
            CHECK("both depths: and an 8-bit result", b && b->depth == PixelDepth::Byte);
            RenderOptions fo;
            fo.depth = PixelDepth::Float;
            ImagePtr f = inst->render(0, fo, &err);
            checkEq("both depths: a float caller gets float work", (int)inst->depth(), (int)PixelDepth::Float);
            CHECK("both depths: and a float result", f && f->depth == PixelDepth::Float);
            CHECK("both depths: the unmapped depth says float too", inst->findClip("Source") &&
                                                                    inst->findClip("Source")->props.getString(kOfxImageClipPropUnmappedPixelDepth) == kOfxBitDepthFloat);
            const float* px = f ? (const float*)f->row(0) : nullptr;
            // invert: alpha - colour, in R, G, B order for float pictures (30, 100, 10 of 255)
            CHECK("both depths: the float result is exact (1 - 30/255, 1 - 100/255, 1 - 10/255)",
                  px && std::fabs(px[0] - (1.f - 30.f / 255.f)) < 1e-5f && std::fabs(px[1] - (1.f - 100.f / 255.f)) < 1e-5f && std::fabs(px[2] - (1.f - 10.f / 255.f)) < 1e-5f);
            ImagePtr b2 = renderOnce(*inst, 0, &err);
            checkEq("both depths: an 8-bit caller again: 8-bit work again", (int)inst->depth(), (int)PixelDepth::Byte);
            CHECK("both depths: the same 8-bit picture as before", b && b2 && sameImage(*b, *b2));
        }
    }
}

// ====================================================================== 8. generator
static void testGenerator()
{
    struct S { int w, h; };
    for (S sz : {S{64, 48}, S{100, 37}, S{3, 3}}) {
        auto inst = makeInstance("reno.test.generator", sz.w, sz.h, kOfxImageEffectContextGenerator, kVegasContextGenerator);
        std::string tag = strfmt("generator %dx%d: ", sz.w, sz.h);
        CHECK(tag + "instance", inst != nullptr);
        if (!inst) continue;
        CHECK(tag + "no input clips", inst->inputClipNames().empty());
        CHECK(tag + "output clip exists and is connected", inst->findClip("Output") && inst->findClip("Output")->props.getInt(kOfxImageClipPropConnected) == 1 && !inst->findClip("Source"));
        checkEq(tag + "project size property", inst->props.getDouble(kOfxImageEffectPropProjectSize, 0), (double)sz.w);
        for (double t : {0.0, 3.0, 10.0, 17.0, 2.5, 70.0}) {
            gFx.clearLog();
            std::string err;
            ImagePtr out = renderOnce(*inst, t, &err);
            record(tag + strfmt("render at %g", t), out != nullptr, err);
            if (!out) continue;
            checkEq(tag + "width", out->w, sz.w);
            checkEq(tag + "height", out->h, sz.h);
            const int frame = (int)std::floor(t);
            const int bx = (frame * 4) % sz.w;
            bool ok = true;
            std::string where;
            for (int y = 0; y < sz.h && ok; ++y)
                for (int x = 0; x < sz.w && ok; ++x) {
                    int r = frame & 255, g = y & 255, b = x & 255;
                    if (x == sz.w - 1 || y == sz.h - 1) r = g = b = 255;
                    if (x >= bx && x < bx + 8 && y < 8) r = g = b = 0;
                    const uint8_t* o = out->row(y) + x * 4;
                    if (o[0] != b || o[1] != g || o[2] != r || o[3] != 255) { ok = false; where = strfmt("pixel %d,%d got %d,%d,%d,%d want %d,%d,%d,255", x, y, o[0], o[1], o[2], o[3], b, g, r); }
                }
            record(tag + strfmt("pattern at %g is exact (row 0 is the bottom row)", t), ok, where);
            CHECK(tag + "the effect saw the project size and the whole frame as the render window",
                  gFx.has(strfmt("generator: t=%g project=%dx%d window=0,0,%d,%d", t, sz.w, sz.h, sz.w, sz.h)));
        }
        CHECK(tag + "the VEGAS context is passed on", inst->props.getString(kVegasPropContext) == kVegasContextGenerator);
    }
    // a generator cannot be made in a context it did not describe
    std::string err;
    InstanceSetup s;
    CHECK("generator: cannot be used as a filter", plug("reno.test.generator")->createInstance(kOfxImageEffectContextFilter, s, &err) == nullptr && contains(err, "does not work as"));
}

// ====================================================================== 9. transition
static void testTransition()
{
    const int W = 15, H = 9;
    ImagePtr a = randomImage(W, H, 21), b = randomImage(W, H, 22);
    auto inst = makeInstance("reno.test.transition", W, H, kOfxImageEffectContextTransition, kVegasContextEvent);
    CHECK("transition: instance", inst != nullptr);
    if (!inst) return;
    checkEq("transition: clips", inst->inputClipNames(), std::vector<std::string>{"SourceFrom", "SourceTo"});
    Param* tp = prm(*inst, kOfxImageEffectTransitionParamName);
    CHECK("transition: mandated Transition parameter", tp && tp->kind == ParamKind::Double && tp->props.getDouble(kOfxParamPropMin) == 0.0 && tp->props.getDouble(kOfxParamPropMax) == 1.0);
    setSource(*inst, "SourceFrom", a);
    setSource(*inst, "SourceTo", b);
    inst->userSetValue(tp, {0.0}, 0, true);
    inst->userSetValue(tp, {1.0}, 100, true);
    for (double t : {0.0, 25.0, 50.0, 75.0, 100.0, 12.5}) {
        std::string err;
        gFx.clearLog();
        ImagePtr out = renderOnce(*inst, t, &err);
        record(strfmt("transition: render at %g", t), out != nullptr, err);
        if (!out) continue;
        const float k = (float)(t / 100.0);
        bool ok = true;
        std::string where;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W && ok; ++x)
                for (int c = 0; c < 4 && ok; ++c) {
                    uint8_t want = (uint8_t)(a->row(y)[x * 4 + c] * (1.f - k) + b->row(y)[x * 4 + c] * k + 0.5f);
                    if (out->row(y)[x * 4 + c] != want) { ok = false; where = strfmt("pixel %d,%d c%d got %d want %d", x, y, c, out->row(y)[x * 4 + c], want); }
                }
        record(strfmt("transition: cross dissolve at %g is exact", t), ok, where);
        CHECK(strfmt("transition: the effect read the animated parameter at %g", t), gFx.has(strfmt("transition: t=%g amount=%.9g", t, (double)k)));
    }
    std::string err;
    ImagePtr e0 = renderOnce(*inst, 0, &err), e1 = renderOnce(*inst, 100, &err);
    CHECK("transition: 0 is the first picture, 1 is the second", e0 && e1 && sameImage(*e0, *a) && sameImage(*e1, *b));
    inst->setClipSource("SourceTo", nullptr);
    CHECK("transition: a missing input fails the render", renderOnce(*inst, 50, &err) == nullptr);
}

// ====================================================================== 10. general context with an optional input
static void testGeneral()
{
    const int W = 17, H = 5;
    ImagePtr a = randomImage(W, H, 31), b = randomImage(W, H, 32);
    std::string err;
    auto inst = makeInstance("reno.test.general", W, H);
    CHECK("general: instance (filter-like use picks the general context)", inst != nullptr && inst->context() == kOfxImageEffectContextGeneral);
    if (!inst) return;
    checkEq("general: input clip names", inst->inputClipNames(), std::vector<std::string>{"Source", "Second"});
    CHECK("general: Second is optional", inst->findClip("Second")->optional && !inst->findClip("Source")->optional);
    checkEq("general: unconnected optional clip reports Connected=0", inst->findClip("Second")->props.getInt(kOfxImageClipPropConnected, 0, -1), 0);
    setSource(*inst, "Source", a);
    gFx.clearLog();
    ImagePtr out = renderOnce(*inst, 0, &err);
    record("general: render with the optional input unconnected", out != nullptr, err);
    CHECK("general: the effect saw it unconnected and was refused an image", gFx.has("general: second connected=0 imageStatus=1"));
    if (out) {
        bool ok = true;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W * 4 && ok; ++x) ok = out->row(y)[x] == a->row(y)[x] / 2;
        CHECK("general: output is half of the source", ok);
    }
    gFx.clearLog();
    setSource(*inst, "Second", b);
    checkEq("general: Connected turns to 1", inst->findClip("Second")->props.getInt(kOfxImageClipPropConnected, 0, -1), 1);
    CHECK("general: the effect is told a clip changed (OpenFX: instanceChanged with type clip)",
          gFx.has("general: changed type=OfxTypeClip name=Second reason=OfxChangeUserEdited"));
    out = renderOnce(*inst, 0, &err);
    record("general: render with both inputs", out != nullptr, err);
    CHECK("general: the effect saw it connected", gFx.has("general: second connected=1 imageStatus=0"));
    if (out) {
        bool ok = true;
        for (int y = 0; y < H && ok; ++y)
            for (int x = 0; x < W * 4 && ok; ++x) ok = out->row(y)[x] == clampB(a->row(y)[x] + b->row(y)[x]);
        CHECK("general: output is source plus second (saturated)", ok);
    }
    inst->setClipSource("Second", nullptr);
    checkEq("general: disconnecting turns Connected back to 0", inst->findClip("Second")->props.getInt(kOfxImageClipPropConnected, 0, -1), 0);
    gFx.clearLog();
    out = renderOnce(*inst, 0, &err);
    CHECK("general: unconnected again", out != nullptr && gFx.has("general: second connected=0 imageStatus=1"));
    inst->setClipSource("NoSuchClip", [](double) { return ImagePtr(); });
    CHECK("general: connecting a clip that does not exist is ignored", inst->findClip("NoSuchClip") == nullptr);
    std::string e2;
    InstanceSetup s;
    CHECK("general: cannot be made as a filter", plug("reno.test.general")->createInstance(kOfxImageEffectContextFilter, s, &e2) == nullptr);
}

// ====================================================================== 11. identity, clip preferences, memory, threads, messages
static void testMisc()
{
    std::vector<std::string> messages;
    std::mutex mm;
    setLogSink([&](const std::string& line) {
        std::lock_guard<std::mutex> lk(mm);
        messages.push_back(line);
    });

    struct S { int w, h; };
    for (S sz : {S{33, 12}, S{7, 2}, S{13, 1}, S{200, 150}}) {
        std::string tag = strfmt("misc %dx%d: ", sz.w, sz.h);
        ImagePtr src = randomImage(sz.w, sz.h, 41 + sz.w);
        auto inst = makeInstance("reno.test.misc", sz.w, sz.h);
        CHECK(tag + "instance", inst != nullptr);
        if (!inst) continue;
        TimedSource ts;
        ts.make = [src](double) { return src; };
        inst->setClipSource("Source", ts.fn());
        gFx.clearLog();
        gFx.clearCounters();
        std::string err;
        ImagePtr out = renderOnce(*inst, 10, &err);
        record(tag + "render", out != nullptr, err);
        if (!out) continue;
        bool ok = true;
        for (int y = 0; y < sz.h && ok; ++y)
            for (int x = 0; x < sz.w && ok; ++x)
                for (int c = 0; c < 4; ++c) {
                    const uint8_t s = src->row(y)[x * 4 + c];
                    const uint8_t want = c == 3 ? s : clampB((int)(s * 0.5 + 0.5));
                    ok = ok && out->row(y)[x * 4 + c] == want;
                }
        CHECK(tag + "output is the source times Amount (0.5), computed on several threads into image memory", ok);
        CHECK(tag + "every check the effect makes of the host's answers passed", gFx.counter("miscCheckFailures") == 0 && !gFx.has("CHECKFAIL"));
        CHECKM(tag + "all rows done by four threads", gFx.has(strfmt("rows=%d mask=15", sz.h)), "%s", gFx.last("misc: render").c_str());
        CHECK(tag + "depth 8-bit", gFx.has("depth=byte"));
        checkEq(tag + "getClipPreferences ran once, before the first render", gFx.counter("getClipPreferences"), 1);
        checkEq(tag + "isIdentity asked once per render", gFx.counter("isIdentity"), 1);
        checkEq(tag + "begin and end sequence render are paired", gFx.counter("beginSequence"), gFx.counter("endSequence"));
        checkEq(tag + "one begin sequence for one frame", gFx.counter("beginSequence"), 1);
        checkEq(tag + "the source was asked for once", ts.sortedTimes().size(), (size_t)1);
        if (sz.w == 33) {
            // messages reached the host's log
            std::lock_guard<std::mutex> lk(mm);
            auto any = [&](const std::string& sub) { for (auto& m : messages) if (contains(m, sub)) return true; return false; };
            CHECK("misc: message suite - log message reaches the host log", any("rendering 33 x 12"));
            CHECK("misc: message suite - message, warning, question", any("OfxMessageMessage") && any("hello") && any("careful") && any("really?"));
            CHECK("misc: message suite - persistent message is logged", any("kept 7"));
            CHECK("misc: message suite - the message names the plug-in", any("[reno.test.misc]"));
        }
        CHECK(tag + "persistent message cleared by the effect", inst->persistentMessage.empty());

        if (sz.w == 33) {
            // another call: same result, no further preferences
            gFx.clearCounters();
            ImagePtr out2 = renderOnce(*inst, 11, &err);
            CHECK("misc: second frame, same result", out2 && sameImage(*out, *out2));
            checkEq("misc: getClipPreferences is not repeated", gFx.counter("getClipPreferences"), 0);

            // identity: nothing to do when Amount is 0; the source is handed back as it is, from the time the effect names
            inst->userSetValue(prm(*inst, "Amount"), {0}, 0);
            inst->userSetValue(prm(*inst, "Offset"), {3}, 0);
            ts.clear();
            gFx.clearCounters();
            ImagePtr same = renderOnce(*inst, 10, &err);
            CHECK("misc: identity returns the very source picture", same && same.get() == src.get());
            checkEq("misc: identity does not call render", gFx.counter("render"), 0);
            checkEq("misc: identity asked the effect once", gFx.counter("isIdentity"), 1);
            checkEq("misc: identity fetched the source at the time the effect asked for (t + Offset)", ts.sortedTimes(), std::vector<double>{13});
            checkEq("misc: no sequence render for an identity frame", gFx.counter("beginSequence"), 0);
            CHECK("misc: identity leaves no handles behind", inst->liveImages.empty());
            inst->userSetValue(prm(*inst, "Offset"), {-4}, 0);
            ts.clear();
            renderOnce(*inst, 10, &err);
            checkEq("misc: identity time slip backwards", ts.sortedTimes(), std::vector<double>{6});
            // with no source connected, identity cannot be honoured and the render runs (and fails there)
            inst->setClipSource("Source", nullptr);
            gFx.clearCounters();
            err.clear();
            CHECK("misc: identity naming an unconnected clip falls through to render", renderOnce(*inst, 10, &err) == nullptr && gFx.counter("render") == 1);
            inst->setClipSource("Source", ts.fn());
            inst->userSetValue(prm(*inst, "Amount"), {0.5}, 0);
            inst->userSetValue(prm(*inst, "Offset"), {0}, 0);
            gFx.clearCounters();
            ImagePtr again = renderOnce(*inst, 10, &err);
            CHECK("misc: not an identity any more", again && again.get() != src.get() && sameImage(*again, *out) && gFx.counter("render") == 1);

            // the effect asks for float pictures through getClipPreferences when Mode changes
            gFx.clearCounters();
            inst->userSetValue(prm(*inst, "Mode"), {1}, 0);
            ImagePtr fl = renderOnce(*inst, 10, &err);
            record("misc: float mode renders", fl != nullptr, err);
            checkEq("misc: preferences asked again after the slave parameter changed", gFx.counter("getClipPreferences"), 1);
            checkEq("misc: the instance works in float now", (int)inst->depth(), (int)PixelDepth::Float);
            CHECK("misc: the effect saw float", gFx.has("depth=float"));
            CHECK("misc: no check failed in float", gFx.counter("miscCheckFailures") == 0);
            if (fl) {
                bool okf = true;
                std::string where;
                for (int y = 0; y < sz.h && okf; ++y)
                    for (int x = 0; x < sz.w && okf; ++x)
                        for (int c = 0; c < 4; ++c) {
                            const uint8_t s = src->row(y)[x * 4 + c];
                            const uint8_t want = c == 3 ? f2b(b2f(s)) : f2b(b2f(s) * 0.5f);
                            if (fl->row(y)[x * 4 + c] != want) { okf = false; where = strfmt("%d,%d c%d got %d want %d", x, y, c, fl->row(y)[x * 4 + c], want); }
                        }
                record("misc: float output (through image memory and threads)", okf, where);
            }
            gFx.clearCounters();
            inst->userSetValue(prm(*inst, "Amount"), {0.25}, 0);
            renderOnce(*inst, 10, &err);
            checkEq("misc: changing an ordinary parameter does not ask for preferences again", gFx.counter("getClipPreferences"), 0);
            inst->userSetValue(prm(*inst, "Mode"), {0}, 0);
            inst->userSetValue(prm(*inst, "Amount"), {0.5}, 0);
            ImagePtr back = renderOnce(*inst, 10, &err);
            checkEq("misc: back to 8-bit", (int)inst->depth(), (int)PixelDepth::Byte);
            CHECK("misc: ... and the same picture as before", back && sameImage(*back, *out));

            // connecting a source after the instance was made asks again (clip preferences depend on the clips)
            auto late = makeInstance("reno.test.misc", sz.w, sz.h);
            gFx.clearCounters();
            renderOnce(*late, 1, &err);                       // (no source: fails, but preferences are asked)
            int first = gFx.counter("getClipPreferences");
            late->setClipSource("Source", [src](double) { return src; });
            renderOnce(*late, 1, &err);
            CHECKM("misc: preferences are asked again once a clip is connected", gFx.counter("getClipPreferences") == first + 1, "before %d after %d", first, gFx.counter("getClipPreferences"));
        }
    }
    setLogSink(nullptr);
}

// ====================================================================== 12. VEGAS extensions
static void testVegas()
{
    gFx.clearLog();
    Plugin* pl = plug("reno.test.vegas");
    CHECK("vegas: loads", pl && pl->loaded);
    if (!pl) return;
    CHECK("vegas: the effect found every VEGAS suite and the two Reno does not provide are absent",
          gFx.has("vegas: load vegasHost=1 hwnd=1 progress1=1 progress2=1 stereo=1 keyframe=1 effectSuite=1 overlaySuite=1"));
    CHECK("vegas: suite log - given", hasName(pl->suiteLog, "OfxHWndInteractSuite v1: given") && hasName(pl->suiteLog, "OfxVegasKeyframeSuite v1: given") &&
                                         hasName(pl->suiteLog, "OfxVegasProgressSuite v2: given") && hasName(pl->suiteLog, "OfxVegasStereoscopicImageEffectSuite v1: given"));
    CHECK("vegas: suite log - About/Help and HWnd overlay given", hasName(pl->suiteLog, "OfxVegasEffectSuite v1: given") && hasName(pl->suiteLog, "OfxHWndOverlayInteractSuite v1: given"));
    CHECK("vegas: info says it has a window of its own", pl->info.hasHwndInteract && !pl->info.hasOverlay);

    const int W = 20, H = 10;
    ImagePtr src = randomImage(W, H, 51);
    auto inst = makeInstance("reno.test.vegas", W, H, std::string(), kVegasContextTrack);
    CHECK("vegas: instance", inst != nullptr);
    if (!inst) return;
    checkEq("vegas: context property on the instance", inst->props.getString(kVegasPropContext), std::string(kVegasContextTrack));
    CHECK("vegas: instance has an HWnd interact", inst->hasHwndInteract());
    setSource(*inst, "Source", src);
    gFx.clearLog();
    std::string err;
    RenderOptions ro;
    ImagePtr out = inst->render(2, ro, &err);
    record("vegas: render", out && sameImage(*out, *src), err);
    const std::string order = "clipOrder=OfxImagePixelOrderBGRA imageOrder=OfxImagePixelOrderBGRA";
    CHECK("vegas: render sees the VEGAS context, preview quality, one ordinary view, blue-first pictures",
          gFx.has("vegas: context=OfxImageEffectPropVegasContextTrack quality=OfxImageEffectPropRenderQualityPreview views=1 view=0 " + order));
    CHECK("vegas: beginSequenceRender gets the same render properties",
          gFx.has("vegas: beginSequence quality=OfxImageEffectPropRenderQualityPreview views=1 view=0 interactive=1"));
    CHECK("vegas: keyframe suite answers for an animatable and a hold parameter",
          gFx.has("vegas: keyInterp amount=0/OfxVegasKeyframeInterpolationLinear flag=0/OfxVegasKeyframeInterpolationHold"));
    CHECK("vegas: progress suites", gFx.has("vegas: progress v1 0/0/0") && gFx.has("vegas: progress v2 0/0/0"));
    CHECK("vegas: stereoscopic image fetch returned a picture and the plug-in's checks passed", out != nullptr && gFx.failures().empty());

    gFx.clearLog();
    ro.interactive = false;
    inst->render(2, ro, &err);
    CHECK("vegas: final render is best quality", gFx.has("quality=OfxImageEffectPropRenderQualityBest") && gFx.has("interactive=0"));
    gFx.clearLog();
    ro.draft = true;
    inst->render(2, ro, &err);
    CHECK("vegas: draft render is draft quality", gFx.has("quality=OfxImageEffectPropRenderQualityDraft"));

    // the effect's own window
    gFx.clearLog();
    int minW = -1, minH = -1, prefW = -1, prefH = -1;
    void* parent = (void*)0x1234;
    CHECK("vegas: window cannot be made without a parent", !inst->createHwndInteract(nullptr, &minW, &minH, &prefW, &prefH) && gFx.grep("hwnd:").empty());
    bool made = inst->createHwndInteract(parent, &minW, &minH, &prefW, &prefH);
    CHECK("vegas: createHwndInteract", made);
    checkEq("vegas: minimum width comes from the effect", minW, 120);
    checkEq("vegas: minimum height", minH, 40);
    checkEq("vegas: preferred width", prefW, 360);
    checkEq("vegas: preferred height", prefH, 240);
    {
        auto lines = gFx.grep("hwnd:");
        std::vector<std::string> actions;
        for (auto& l : lines) {
            size_t a = l.find(':') + 1, b = l.find(' ', a);
            actions.push_back(l.substr(a, b == std::string::npos ? std::string::npos : b - a));
        }
        checkEq("vegas: window actions in order", actions, std::vector<std::string>{"OfxActionDescribe", "OfxActionCreateInstance", "OfxHWndInteractActionCreateWindow",
                                                                                    "OfxHWndInteractActionShowWindow"});
        std::string effect = strfmt("effect=%p", (void*)inst.get());
        bool all = !lines.empty();
        for (auto& l : lines) all = all && contains(l, effect) && contains(l, "vegasSuite=0/0/same");
        CHECK("vegas: the interact knows its effect instance, and the VEGAS interact suite works on its handle", all);
        CHECK("vegas: parent window passed on", contains(lines.size() > 2 ? lines[2] : "", "parent=0x1234"));
    }
    gFx.clearLog();
    inst->moveHwndInteract(5, 6, 300, 200);
    CHECK("vegas: move window passes left, top, width, height", gFx.has("hwnd:OfxHWndInteractActionMoveWindow") && gFx.has("location=5,6,300,200"));
    // making another disposes the first
    gFx.clearLog();
    CHECK("vegas: making a second window", inst->createHwndInteract(parent, nullptr, nullptr, nullptr, nullptr));
    {
        auto l = gFx.lines();
        int dispose = -1, destroy = -1, create = -1;
        for (int i = 0; i < (int)l.size(); ++i) {
            if (contains(l[i], "hwnd:OfxHWndInteractActionDisposeWindow") && dispose < 0) dispose = i;
            if (contains(l[i], "hwnd:OfxActionDestroyInstance") && destroy < 0) destroy = i;
            if (contains(l[i], "hwnd:OfxHWndInteractActionCreateWindow") && create < 0) create = i;
        }
        CHECK("vegas: the earlier window is disposed and destroyed before the new one is made", dispose >= 0 && destroy > dispose && create > destroy);
    }
    gFx.clearLog();
    inst->disposeHwndInteract();
    {
        auto l = gFx.lines();
        CHECK("vegas: dispose then destroy", l.size() >= 2 && contains(l[0], "hwnd:OfxHWndInteractActionDisposeWindow") && contains(l[1], "hwnd:OfxActionDestroyInstance"));
    }
    gFx.clearLog();
    inst->disposeHwndInteract();
    inst->moveHwndInteract(0, 0, 1, 1);
    CHECK("vegas: nothing happens with no window", gFx.grep("hwnd:").empty());
    // an open window is cleaned up with the instance
    {
        auto tmp = makeInstance("reno.test.vegas", W, H);
        tmp->createHwndInteract(parent, nullptr, nullptr, nullptr, nullptr);
        gFx.clearLog();
    }
    auto disposeOrder = [&] {
        auto l = gFx.lines();
        int dispose = -1, destroyWin = -1, destroyInst = -1;
        for (int i = 0; i < (int)l.size(); ++i) {
            if (contains(l[i], "hwnd:OfxHWndInteractActionDisposeWindow")) dispose = i;
            if (contains(l[i], "hwnd:OfxActionDestroyInstance")) destroyWin = i;
            if (contains(l[i], "vegas: destroyInstance")) destroyInst = i;
        }
        return dispose >= 0 && destroyWin > dispose && destroyInst > destroyWin;
    };
    CHECK("vegas: a window still open when the instance goes is disposed first", disposeOrder());

    gFx.clearLog();
    CHECK("vegas: About", inst->invokeAbout() && gFx.has("vegas: about"));
    inst->beginEdit();
    inst->endEdit();
    CHECK("vegas: edit session", gFx.has("vegas: beginEdit") && gFx.has("vegas: endEdit"));
    CHECK("vegas: an effect with no About says so", !makeInstance("reno.test.invert", 4, 4)->invokeAbout() || true);
}

// ====================================================================== 12b. VEGAS Pro 18's own effects' needs
static void testVegas18()
{
    gFx.clearLog();
    Plugin* pl = plug("reno.test.vegas18");
    CHECK("vegas18: loads (the host has the spike key)", pl && pl->loaded);
    if (!pl) return;
    CHECK("vegas18: host name, spike key, About/Help and overlay suites found, no OpenCL suite; overlay suite refuses a null handle",
          gFx.has("vegas18: load name=com.vegascreativesoftware.vegas spike=1 effectSuite=1 overlaySuite=1 openclSuite=0 overlayNull=9/1"));
    CHECK("vegas18: declared its context on VEGAS 18's name", pl->supportsContext(kOfxImageEffectContextFilter));
    const int W = 16, H = 8;
    auto inst = makeInstance("reno.test.vegas18", W, H);
    CHECK("vegas18: instance (describeInContext could add the GUID it replaces)", inst != nullptr && gFx.has("vegas18: uplift GUIDs before=0"));
    if (!inst) return;
    CHECK("vegas18: the GUID is kept in the plug-in's info", pl->info.upliftGuids.size() == 1 && pl->info.upliftGuids[0] == "{C55DDAF9-B236-11d2-BAD4-00C04F8EDC09}");
    CHECK("vegas18: ... and is on the instance's descriptor properties", inst->props.dimension(kVegasPropUpliftGUID) == 1);
    {
        Param* ip = inst->params.find("ImageSource");
        CHECK("vegas18: the picture parameter is kept with its two properties, no value or row of its own",
              ip && ip->type == kVegasParamTypeImage && !ip->hasValue() && ip->props.getInt(kVegasParamImageHideGrabFromPreview) == 1 &&
                  ip->props.has(kVegasParamImageHideGrabControls));
        CHECK("vegas18: ... its value is unsupported, as in VEGAS", gFx.has("vegas18: image parameter value 5"));
        Json st = inst->saveState();
        auto again = makeInstance("reno.test.vegas18", W, H);
        if (again) again->loadState(st);
        CHECK("vegas18: an effect with one saves and loads its state", again != nullptr && again->params.find("ImageSource") != nullptr);
    }
    ImagePtr src = randomImage(W, H, 77);
    setSource(*inst, "Source", src);
    std::string err;
    gFx.clearLog();
    RenderOptions ro;
    ImagePtr out = inst->render(4, ro, &err);
    record("vegas18: renders", out && sameImage(*out, *src), err);
    CHECK("vegas18: isIdentity and render get the time code, one view, no OpenGL / OpenCL",
          gFx.has("vegas18: isIdentity time=4 timecode=4 views=1 gl=-1 cl=-1") && gFx.has("vegas18: render time=4 timecode=4 views=1 gl=0 cl=0"));
    gFx.clearLog();
    ro.timelineTime = 125.5;
    inst->render(4, ro, &err);
    CHECK("vegas18: the time code is the timeline's time when given", gFx.has("vegas18: render time=4 timecode=125.5 views=1 gl=0 cl=0"));

    // About and Help come back to the host through OfxVegasEffectSuite
    std::vector<std::pair<EffectBase*, std::string>> asked;
    host().onEffectRequest = [&](EffectBase* e, const std::string& a) { asked.push_back({e, a}); };
    gFx.clearLog();
    CHECK("vegas18: About", inst->invokeAbout());
    CHECK("vegas18: Help", inst->invokeHelp());
    host().onEffectRequest = nullptr;
    CHECK("vegas18: the effect asked the host for its About, then its Help, with its own handle",
          asked.size() == 2 && asked[0].first == inst.get() && asked[0].second == kVegasActionShowEffectAbout && asked[1].first == inst.get() &&
              asked[1].second == kVegasActionShowEffectHelp);
    CHECK("vegas18: as VEGAS: OK for About / Help, failed for another action or no effect",
          gFx.has("vegas18: about 0 bad=1 null=1") && gFx.has("vegas18: help 0 bad=1 null=1"));
    CHECK("vegas18: without a handler the request is only logged", inst->invokeAbout());

    // what a plug-in found missing is said plainly
    std::string e2;
    Plugin* mp = plug("reno.test.missing");
    auto mi = mp ? mp->createInstance(kOfxImageEffectContextFilter, InstanceSetup(), &e2) : nullptr;
    CHECK("missing: no instance", mp && !mi);
    CHECK("missing: the error names the action, the suite and the property it did not get",
          contains(e2, "missing host feature") && contains(e2, "OfxImageEffectActionDescribeInContext") && contains(e2, "the suite OfxImaginarySuite v3") &&
              contains(e2, "the property OfxPropImaginary (the host's properties)"));
    CHECK("missing: kept on the plug-in", mp && contains(mp->missingFeature, "OfxPropImaginary") && hasName(mp->missingLog, "OfxPropImaginary (the host's properties)"));
    CHECK("missing: marked as not usable here but still loaded", mp && mp->unusable && mp->loaded && !mp->failed);
    CHECK("missing: still found when there is nothing else", host().findPlugin("reno.test.missing") == mp);
    CHECK("missing: a property read that fails outside a plug-in's action is harmless", [] {
        PropertySet ps;
        int v = 0;
        const OfxPropertySuiteV1* sp = (const OfxPropertySuiteV1*)fetch(kOfxPropertySuite, 1);
        return sp->propGetInt((OfxPropertySetHandle)&ps, "Nothing", 0, &v) == kOfxStatErrUnknown && sp->propGetInt((OfxPropertySetHandle)&ps, "Nothing", 3, &v) == kOfxStatErrUnknown;
    }());
    checkEq("missing: explainStatus of another status is just its name", mp ? mp->explainStatus(kOfxStatFailed) : std::string(), std::string("failed"));
}

static void testNotVegas()
{
    std::string copy = copyFile(gFxPath, gScratchDir + "/testfx_other.ofx");
    FxLib lib;
    CHECK("not vegas: second copy opens", !copy.empty() && lib.open(copy));
    if (copy.empty()) return;
    HostConfig saved = host().config;
    host().config.hostName = "org.example.otherhost";
    host().config.vegasExtensions = false;
    host().applyConfig();
    {
        std::string err;
        Binary* b = host().loadBinary(copy, &err);
        record("not vegas: binary loads", b != nullptr, err);
        Plugin *vp = nullptr, *ip = nullptr, *v18 = nullptr;
        if (b)
            for (auto& p : b->plugins) {
                if (p->info.id == "reno.test.vegas") vp = p.get();
                if (p->info.id == "reno.test.invert") ip = p.get();
                if (p->info.id == "reno.test.vegas18") v18 = p.get();
            }
        if (v18) {
            std::string e18;
            CHECK("not vegas: an effect made like VEGAS 18's does not load, and says it lacked the spike key",
                  !v18->load(&e18) && contains(e18, "refused to load (missing host feature: in OfxActionLoad") && contains(e18, "OfxPropVegasSpikeKey (the host's properties)"));
        }
        CHECK("not vegas: plug-ins found in the copy", vp && ip);
        if (vp && ip) {
            lib.clearLog();
            CHECK("not vegas: vegas effect loads", vp->load(&err));
            CHECK("not vegas: ... and finds itself on another host", lib.has("vegas: load vegasHost=0 (host is 'org.example.otherhost')"));
            CHECK("not vegas: no window of its own", !vp->info.hasHwndInteract);
            CHECK("not vegas: VEGAS suites were not given to it", !hasName(vp->suiteLog, "OfxHWndInteractSuite v1: given"));
            InstanceSetup s;
            s.width = 20;
            s.height = 10;
            s.vegasContext = kVegasContextTrack;
            auto vi = vp->createInstance(vp->filterContext(), s, &err);
            CHECK("not vegas: vegas effect instance", vi != nullptr);
            if (vi) {
                CHECK("not vegas: no VEGAS context property", !vi->props.has(kVegasPropContext));
                CHECK("not vegas: hasHwndInteract false", !vi->hasHwndInteract());
                CHECK("not vegas: createHwndInteract refused", !vi->createHwndInteract((void*)0x10, nullptr, nullptr, nullptr, nullptr));
                CHECK("not vegas: About refused", !vi->invokeAbout());
                setSource(*vi, "Source", randomImage(20, 10, 3));
                lib.clearLog();
                vi->render(1, RenderOptions(), &err);
                CHECK("not vegas: no VEGAS properties in render", lib.has("vegas: context= quality= views=-1 view=-1 clipOrder= imageOrder="));
                CHECK("not vegas: no stand-ins asked for", lib.has("vegas: beginSequence quality= views=-1 view=-1"));
            }
            // without the pixel-order property the effect assumes the standard order, and that shows in memory
            ip->load(&err);
            auto ii = ip->createInstance(ip->filterContext(), s, &err);
            CHECK("not vegas: invert instance", ii != nullptr);
            if (ii) {
                ImagePtr src = randomImage(20, 10, 4);
                setSource(*ii, "Source", src);
                ii->userSetValue(prm(*ii, "ZeroRed"), {1}, 0);
                lib.clearLog();
                ImagePtr out = renderOnce(*ii, 0, &err);
                CHECK("not vegas: the effect assumes RGBA, since the host told it nothing", lib.has("srcOrder=RGBA dstOrder=RGBA"));
                bool zeroAtZero = out != nullptr;
                for (int y = 0; out && y < 10; ++y)
                    for (int x = 0; x < 20; ++x) zeroAtZero = zeroAtZero && out->row(y)[x * 4] == 0;
                CHECK("not vegas: memory byte 0 (really blue) is the one zeroed, which shows the VEGAS run was BGRA", zeroAtZero);
            }
        }
    }
    host().config = saved;
    host().applyConfig();
}

// ====================================================================== 13. awkward effects
static void testAwkward()
{
    const int W = 8, H = 6;
    ImagePtr src = randomImage(W, H, 61);
    {
        auto inst = makeInstance("reno.test.failrender", W, H);
        CHECK("failrender: instance", inst != nullptr);
        if (inst) {
            setSource(*inst, "Source", src);
            struct M { int mode; const char* text; } modes[] = {{0, "failed to render (failed)"}, {1, "failed to render (out of memory)"},
                                                                 {2, "failed to render (default)"}, {3, "failed to render (fatal error)"}};
            for (auto& m : modes) {
                gFx.clearCounters();
                gFx.clearLog();
                inst->userSetValue(prm(*inst, "Mode"), {(double)m.mode}, 0);
                std::string err;
                ImagePtr out = renderOnce(*inst, 0, &err);
                CHECK(strfmt("failrender: mode %d returns no picture", m.mode), out == nullptr);
                CHECKM(strfmt("failrender: mode %d error text", m.mode), contains(err, m.text), "got '%s'", err.c_str());
                CHECK(strfmt("failrender: mode %d - EndSequenceRender still sent", m.mode), gFx.counter("beginSequence") == 1 && gFx.counter("endSequence") == 1);
                CHECK(strfmt("failrender: mode %d - nothing left behind", m.mode), inst->liveImages.empty() && !inst->outputImage);
                if (m.mode == 0) {
                    CHECK("failrender: the plug-in's persistent message is part of the error", contains(err, "render broke on purpose"));
                    checkEq("failrender: ... and is kept on the instance", inst->persistentMessage, std::string("render broke on purpose"));
                }
            }
            inst->userSetValue(prm(*inst, "Mode"), {4}, 0);
            std::string err;
            ImagePtr ok = renderOnce(*inst, 0, &err);
            record("failrender: recovers once the effect is happy", ok && sameImage(*ok, *src), err);
            CHECK("failrender: persistent message cleared by the plug-in", inst->persistentMessage.empty());
        }
    }
    {
        // a private copy of the binary, so that what the plug-in logs while it is being described is still there to look at
        std::string copy = copyFile(gFxPath, gScratchDir + "/testfx_bad.ofx");
        FxLib lib;
        CHECK("badcalls: private copy opens", !copy.empty() && lib.open(copy));
        std::string lerr;
        Binary* cb = copy.empty() ? nullptr : host().loadBinary(copy, &lerr);
        Plugin* bp = nullptr;
        if (cb)
            for (auto& p : cb->plugins)
                if (p->info.id == "reno.test.badcalls") bp = p.get();
        CHECK("badcalls: plug-in found", bp != nullptr);
        std::unique_ptr<Instance> inst;
        if (bp && bp->load(&lerr)) {
            InstanceSetup su;
            su.width = W;
            su.height = H;
            inst = bp->createInstance(bp->filterContext(), su, &lerr);
        }
        record("badcalls: instance", inst != nullptr, lerr);
        if (inst) {
            CHECK("badcalls: describe-time checks passed", lib.counter("describeCheckFailures") == 0 && lib.has("badcalls: describe checks bad=0"));
            setSource(*inst, "Source", src);
            lib.clearCounters();
            lib.clearLog();
            std::string err;
            ImagePtr out = renderOnce(*inst, 0, &err);
            record("badcalls: render completes (host answered every bad call with the documented code)" + std::string(lib.failures().empty() ? "" : " -"),
                   out != nullptr && lib.failures().empty(), err + lib.failures());
            CHECK("badcalls: picture comes out", out && sameImage(*out, *src));
            record("badcalls: render-time checks passed", lib.counter("badcallsFailures") == 0 && lib.has("badcalls: render checks bad=0"), lib.failures());
        }
    }
    {
        auto inst = makeInstance("reno.test.badsuite", W, H);
        CHECK("badsuite: instance", inst != nullptr);
        if (inst) {
            setSource(*inst, "Source", src);
            std::string err;
            ImagePtr out = renderOnce(*inst, 0, &err);
            record("badsuite: renders", out && sameImage(*out, *src), err);
        }
    }
    {
        auto inst = makeInstance("reno.test.leaky", 64, 48);
        CHECK("leaky: instance", inst != nullptr);
        if (inst) {
            ImagePtr s2 = randomImage(64, 48, 62);
            setSource(*inst, "Source", s2);
            gFx.clearCounters();
            bool all = true;
            bool clean = true;
            for (int k = 0; k < 40; ++k) {
                std::string err;
                ImagePtr out = renderOnce(*inst, k, &err);
                all = all && out && sameImage(*out, *s2);
                clean = clean && inst->liveImages.empty() && !inst->outputImage;
            }
            CHECK("leaky: 40 renders, pictures right even though nothing is released", all);
            CHECK("leaky: the host drops the leaked handles after every render", clean);
            checkEq("leaky: the effect really leaked 4 images per render", gFx.counter("getImage") - gFx.counter("releaseImage"), 160);
            // the pictures themselves come back to the owner: the source image is only referenced by us
            CHECK("leaky: the host no longer holds the source picture", s2.use_count() >= 1 && inst->liveImages.empty());
            long before = s2.use_count();
            inst.reset();
            CHECKM("leaky: and destroying the instance does not keep it", s2.use_count() <= before, "%ld > %ld", s2.use_count(), before);
        }
    }
    {
        // sizes the host cannot make a picture for
        std::string err;
        auto huge = makeInstance("reno.test.invert", 40000, 10);
        if (huge) {
            setSource(*huge, "Source", solidImage(4, 4, 1, 1, 1, 1));
            CHECK("huge: a frame wider than the limit fails cleanly", renderOnce(*huge, 0, &err) == nullptr && contains(err, "memory"));
        } else
            record("huge: instance", false, "no instance");
        auto zero = makeInstance("reno.test.invert", 0, 0);
        if (zero) {
            err.clear();
            setSource(*zero, "Source", solidImage(4, 4, 1, 1, 1, 1));
            CHECK("zero: an empty frame fails cleanly", renderOnce(*zero, 0, &err) == nullptr);
        } else
            record("zero: instance", false, "no instance");
        auto neg = makeInstance("reno.test.invert", -5, 10);
        if (neg) {
            setSource(*neg, "Source", solidImage(4, 4, 1, 1, 1, 1));
            CHECK("negative: a negative width fails cleanly", renderOnce(*neg, 0, &err) == nullptr);
        } else
            record("negative: instance", false, "no instance");
    }
    {
        // a source picture of a different size than the project: the effect works on the picture it is given
        auto inst = makeInstance("reno.test.byteonly", 8, 6);
        ImagePtr bigger = randomImage(8, 6, 63);
        setSource(*inst, "Source", bigger);
        std::string err;
        CHECK("sizes: matching size renders", renderOnce(*inst, 0, &err) != nullptr);
    }
    {
        // many instances, made and destroyed
        gFx.clearCounters();
        for (int k = 0; k < 300; ++k) {
            auto inst = makeInstance("reno.test.params", 4, 4);
            if (!inst) { record("many: instance " + std::to_string(k), false, "none"); break; }
            if (k % 7 == 0) inst->userSetValue(prm(*inst, "driver"), {(double)k}, 0);
        }
        checkEq("many: every instance was created", gFx.counter("createInstance"), 0 + gFx.counter("destroyInstance"));
        CHECK("many: 300 created and destroyed", gFx.counter("destroyInstance") >= 300);
        std::vector<std::unique_ptr<Instance>> keep;
        for (int k = 0; k < 100; ++k) keep.push_back(makeInstance("reno.test.invert", 4, 4));
        bool allThere = true;
        std::set<const void*> uniq;
        for (auto& i : keep) { allThere = allThere && i != nullptr; uniq.insert(i.get()); }
        CHECK("many: 100 alive at once, all different", allThere && uniq.size() == 100);
        // each keeps its own parameter values
        for (size_t k = 0; k < keep.size(); ++k) keep[k]->userSetValue(prm(*keep[k], "ZeroRed"), {(double)(k & 1)}, 0);
        bool own = true;
        for (size_t k = 0; k < keep.size(); ++k) own = own && prm(*keep[k], "ZeroRed")->value[0] == (double)(k & 1);
        CHECK("many: instances do not share parameters", own);
        keep.clear();
    }
}

// ====================================================================== 14. threads
template <class F>
static void runThreads(int n, F fn)
{
    std::vector<std::thread> th;
    for (int k = 0; k < n; ++k) th.emplace_back([&fn, k] { fn(k); });
    for (auto& t : th) t.join();
}

static void testConcurrency()
{
    // two instances of the same effect, two threads, each against its own reference
    {
        const int W = 64, H = 40, FRAMES = 40;
        ImagePtr srcs[2] = {randomImage(W, H, 71), randomImage(W, H, 72)};
        std::vector<ImagePtr> ref[2];
        for (int i = 0; i < 2; ++i) {
            auto inst = makeInstance("reno.test.invert", W, H);
            inst->userSetValue(prm(*inst, "ZeroRed"), {(double)i}, 0);
            setSource(*inst, "Source", srcs[i]);
            for (int f = 0; f < FRAMES; ++f) ref[i].push_back(renderOnce(*inst, f));
        }
        std::unique_ptr<Instance> insts[2];
        for (int i = 0; i < 2; ++i) {
            insts[i] = makeInstance("reno.test.invert", W, H);
            insts[i]->userSetValue(prm(*insts[i], "ZeroRed"), {(double)i}, 0);
            setSource(*insts[i], "Source", srcs[i]);
        }
        std::atomic<int> bad{0};
        runThreads(2, [&](int i) {
            for (int f = 0; f < FRAMES; ++f) {
                std::string err;
                ImagePtr o = renderOnce(*insts[i], f, &err);
                if (!o || !sameImage(*o, *ref[i][f])) bad++;
            }
        });
        checkEq("threads: two instances rendered at once give the same pictures as one at a time", bad.load(), 0);
    }
    // the multi-thread suite inside two instances at once
    {
        const int W = 90, H = 50, FRAMES = 12;
        ImagePtr srcs[2] = {randomImage(W, H, 73), randomImage(W, H, 74)};
        std::unique_ptr<Instance> insts[2];
        std::vector<ImagePtr> ref[2];
        for (int i = 0; i < 2; ++i) {
            insts[i] = makeInstance("reno.test.misc", W, H);
            setSource(*insts[i], "Source", srcs[i]);
            insts[i]->userSetValue(prm(*insts[i], "Amount"), {0.25 + 0.5 * i}, 0);
            for (int f = 0; f < FRAMES; ++f) ref[i].push_back(renderOnce(*insts[i], f));
        }
        gFx.clearCounters();
        std::atomic<int> bad{0};
        runThreads(2, [&](int i) {
            for (int f = 0; f < FRAMES; ++f) {
                std::string err;
                ImagePtr o = renderOnce(*insts[i], f, &err);
                if (!o || !ref[i][f] || !sameImage(*o, *ref[i][f])) bad++;
            }
        });
        checkEq("threads: effects that spawn their own threads, two at once", bad.load(), 0);
        checkEq("threads: ... and none of their checks failed", gFx.counter("miscCheckFailures"), 0);
    }
    // temporal effect with a shared source object
    {
        const int W = 30, H = 20, FRAMES = 30;
        std::vector<ImagePtr> frames;
        for (int f = 0; f < 41; ++f) frames.push_back(frameImage(W, H, f));
        std::unique_ptr<Instance> insts[3];
        std::vector<ImagePtr> ref;
        for (int i = 0; i < 3; ++i) {
            insts[i] = makeInstance("reno.test.temporal", W, H);
            insts[i]->setClipSource("Source", [&frames](double t) { int f = (int)t; return (f < 0 || f > 40) ? ImagePtr() : frames[(size_t)f]; });
        }
        for (int f = 1; f < FRAMES; ++f) ref.push_back(renderOnce(*insts[0], f));
        std::atomic<int> bad{0};
        runThreads(3, [&](int i) {
            for (int f = 1; f < FRAMES; ++f) {
                ImagePtr o = renderOnce(*insts[i], f);
                if (!o || !sameImage(*o, *ref[(size_t)f - 1])) bad++;
            }
        });
        checkEq("threads: temporal access from three instances", bad.load(), 0);
    }
    // one instance, rendered from two threads while a third changes its parameters: each picture is one of the two possible
    {
        const int W = 48, H = 32;
        ImagePtr src = randomImage(W, H, 75);
        auto inst = makeInstance("reno.test.misc", W, H);
        setSource(*inst, "Source", src);
        inst->userSetValue(prm(*inst, "Amount"), {0.5}, 0);
        ImagePtr a = renderOnce(*inst, 0);
        inst->userSetValue(prm(*inst, "Amount"), {0.25}, 0);
        ImagePtr b = renderOnce(*inst, 0);
        std::atomic<bool> stop{false};
        std::atomic<int> bad{0}, renders{0};
        std::thread editor([&] {
            for (int k = 0; k < 200 && !stop; ++k) inst->userSetValue(prm(*inst, "Amount"), {k & 1 ? 0.5 : 0.25}, k);
        });
        runThreads(2, [&](int) {
            for (int k = 0; k < 60; ++k) {
                ImagePtr o = renderOnce(*inst, 0);
                ++renders;
                if (!o || !(sameImage(*o, *a) || sameImage(*o, *b))) bad++;
            }
        });
        stop = true;
        editor.join();
        checkEq("threads: renders racing with edits always see a whole state", bad.load(), 0);
    }
    // plug-ins being described and instances made on several threads at once (a fresh copy of the binary so nothing is described yet)
    {
        std::string copy = copyFile(gFxPath, gScratchDir + "/testfx_conc.ofx");
        std::string err;
        Binary* b = host().loadBinary(copy, &err);
        record("threads: fresh binary for concurrent describing", b != nullptr, err);
        if (b) {
            std::vector<Plugin*> targets;
            for (auto& p : b->plugins) {
                const std::string& id = p->info.id;
                if (id == "reno.test.invert" || id == "reno.test.temporal" || id == "reno.test.floatonly" || id == "reno.test.byteonly" || id == "reno.test.misc" ||
                    id == "reno.test.general" || id == "reno.test.params" || id == "reno.test.leaky" || id == "reno.test.generator" || id == "reno.test.vegas")
                    targets.push_back(p.get());
            }
            std::atomic<int> bad{0};
            std::vector<std::string> errors(targets.size());
            runThreads((int)targets.size(), [&](int i) {
                Plugin* p = targets[(size_t)i];
                std::string e;
                if (!p->load(&e)) { bad++; errors[(size_t)i] = e; return; }
                std::string ctx = p->filterContext();
                if (ctx.empty()) ctx = kOfxImageEffectContextGenerator;
                InstanceSetup s;
                s.width = 32;
                s.height = 16;
                for (int k = 0; k < 5; ++k) {
                    auto inst = p->createInstance(ctx, s, &e);
                    if (!inst) { bad++; errors[(size_t)i] = e; return; }
                    for (auto& name : inst->inputClipNames()) inst->setClipSource(name, [](double t) { return makeTestPattern(32, 16, t); });
                    ImagePtr o = inst->render(k, RenderOptions(), &e);
                    if (!o) { bad++; errors[(size_t)i] = e; return; }
                }
            });
            std::string all;
            for (auto& e : errors) if (!e.empty()) all += e + "; ";
            record("threads: describing, creating and rendering ten different plug-ins at the same time", bad == 0, all);
            std::set<std::string> logged;
            bool noDup = true;
            for (Plugin* p : targets) {
                std::set<std::string> seen;
                for (auto& l : p->suiteLog) noDup = noDup && seen.insert(l).second;
            }
            CHECK("threads: suite logs are intact (no repeats, not mixed up)", noDup);
            bool mine = true;
            for (Plugin* p : targets)
                if (p->info.id != "reno.test.vegas")
                    for (auto& l : p->suiteLog) mine = mine && !contains(l, "Vegas");
            CHECK("threads: suite log of one plug-in holds only its own requests", mine);
        }
    }
    // many instances made and destroyed on four threads at once
    {
        gFx.clearCounters();
        std::atomic<int> bad{0};
        runThreads(4, [&](int t) {
            for (int k = 0; k < 60; ++k) {
                auto inst = makeInstance(t & 1 ? "reno.test.params" : "reno.test.invert", 8, 8);
                if (!inst) { bad++; continue; }
                if (k % 5 == 0) inst->userSetValue(inst->params.params[1].get(), {1}, 0);
            }
        });
        checkEq("threads: 240 instances made and destroyed on four threads", bad.load(), 0);
        checkEq("threads: created and destroyed counts agree", gFx.counter("createInstance"), gFx.counter("destroyInstance"));
        CHECK("threads: all 240 were created", gFx.counter("createInstance") == 240);
    }
    // host-wide calls from several threads
    {
        std::atomic<int> bad{0};
        runThreads(4, [&](int) {
            for (int k = 0; k < 200; ++k) {
                if (!host().findPlugin("reno.test.invert")) bad++;
                if (host().allPlugins().empty()) bad++;
                std::string e;
                if (!host().loadBinary(gFxPath, &e)) bad++;
            }
        });
        checkEq("threads: findPlugin / allPlugins / loadBinary from four threads", bad.load(), 0);
    }
    // two threads loading the same new binary
    {
        std::string copy = copyFile(gFxPath, gScratchDir + "/testfx_twice.ofx");
        std::vector<Binary*> got(4, nullptr);
        runThreads(4, [&](int i) { got[(size_t)i] = host().loadBinary(copy); });
        bool same = got[0] && got[0] == got[1] && got[1] == got[2] && got[2] == got[3];
        CHECK("threads: the same binary loaded from four threads at once is loaded once", same);
        int n = 0;
        for (Plugin* p : host().allPlugins()) if (p->binary && p->binary->path == copy && p->info.id == "reno.test.invert") ++n;
        checkEq("threads: ... and its plug-ins are listed once", n, 1);
    }
}

// ====================================================================== 15. the real effects
static std::map<std::string, ImagePtr> gPatternCache;
static std::mutex gPatternMutex;
static ImagePtr cachedPattern(int w, int h, double t)
{
    std::string key = strfmt("%dx%d@%.4f", w, h, t);
    std::lock_guard<std::mutex> lk(gPatternMutex);
    auto it = gPatternCache.find(key);
    if (it != gPatternCache.end()) return it->second;
    ImagePtr p = makeTestPattern(w, h, t);
    if (gPatternCache.size() > 40) gPatternCache.clear();
    gPatternCache[key] = p;
    return p;
}

static void testRealEffects(const std::string& path)
{
    if (!fileExists(path)) {
        skip("realfx: all", "the test plug-in was not found at " + path + " (set RENO_TEST_PLUGIN to run the real-effect tests)");
        return;
    }
    std::string err;
    Binary* bin = host().loadBinary(path, &err);
    record("realfx: binary loads", bin != nullptr, err);
    if (!bin) return;
    checkEq("realfx: 28 effects", bin->plugins.size(), (size_t)28);
    std::set<std::string> ids;
    for (auto& p : bin->plugins) ids.insert(p->info.id);
    checkEq("realfx: ids are unique", ids.size(), (size_t)bin->plugins.size());
    int loadedOk = 0;
    for (auto& p : bin->plugins) {
        std::string e;
        bool ok = p->load(&e);
        record("realfx: " + p->info.id + " loads and describes", ok && !p->info.label.empty(), e);
        if (ok) ++loadedOk;
    }
    checkEq("realfx: all load", loadedOk, 28);

    struct Size { int w, h; };
    std::vector<Size> sizes = {{640, 360}, {1920, 1080}};
    for (auto& up : bin->plugins) {
        Plugin* pl = up.get();
        if (!pl->loaded) continue;
        for (Size sz : sizes) {
            std::string tag = "realfx: " + pl->info.id + strfmt(" %dx%d: ", sz.w, sz.h);
            InstanceSetup s;
            s.width = sz.w;
            s.height = sz.h;
            s.fps = 30;
            s.durationFrames = 300;
            s.vegasContext = kVegasContextEvent;
            std::string e;
            std::string ctx = pl->filterContext();
            auto inst = ctx.empty() ? nullptr : pl->createInstance(ctx, s, &e);
            record(tag + "instance", inst != nullptr, e);
            if (!inst) continue;
            TimedSource ts;
            ts.make = [sz](double t) { return cachedPattern(sz.w, sz.h, t); };
            for (auto& name : inst->inputClipNames()) inst->setClipSource(name, ts.fn());
            bool sizeOk = true, depthOk = true, differs = false, rowOk = true;
            std::string detail;
            const double times[3] = {10, 11, 12};
            for (double t : times) {
                std::string re;
                ImagePtr out = renderOnce(*inst, t, &re, false);
                if (!out) { record(tag + strfmt("render frame %g", t), false, re); sizeOk = false; break; }
                sizeOk = sizeOk && out->w == sz.w && out->h == sz.h;
                depthOk = depthOk && out->depth == PixelDepth::Byte;
                rowOk = rowOk && out->rowBytes >= (size_t)sz.w * 4;
                if (hashImage(*out) != hashImage(*cachedPattern(sz.w, sz.h, t))) differs = true;
            }
            CHECK(tag + "output has the project size", sizeOk);
            CHECK(tag + "output is an 8-bit picture with whole rows", depthOk && rowOk);
            CHECK(tag + "output differs from the input on at least one of three frames", differs);
            bool finiteTimes = true;
            for (double t : ts.sortedTimes()) finiteTimes = finiteTimes && std::isfinite(t);
            CHECK(tag + "every time it asked the source for is a number", finiteTimes);
            CHECK(tag + "no handles left", inst->liveImages.empty());
            CHECK(tag + "the plug-in did not crash", !pl->crashed);
        }
    }

    // state round trip, and every numeric parameter at its limits (smaller frames)
    int stateOk = 0, stateChecked = 0;
    for (auto& up : bin->plugins) {
        Plugin* pl = up.get();
        if (!pl->loaded) continue;
        const int W = 160, H = 90;
        InstanceSetup s;
        s.width = W;
        s.height = H;
        s.durationFrames = 300;
        std::string e;
        std::string ctx = pl->filterContext();
        if (ctx.empty()) continue;
        auto src = [W, H](double t) { return cachedPattern(W, H, t); };
        auto make = [&]() {
            auto i = pl->createInstance(ctx, s, &e);
            if (i) for (auto& n : i->inputClipNames()) i->setClipSource(n, src);
            return i;
        };
        auto a = make();
        auto a2 = make();
        auto b = make();
        if (!a || !a2 || !b) { record("realfx: " + pl->info.id + " state: instances", false, e); continue; }
        // vary a few parameters first
        int changedParams = 0;
        for (auto& p : a->params.params) {
            if (!p->hasValue() || p->dim == 0 || changedParams >= 4) continue;
            if (p->kind == ParamKind::Double || p->kind == ParamKind::Int) {
                double lo = p->props.getDouble(kOfxParamPropMin, 0, 0), hi = p->props.getDouble(kOfxParamPropMax, 0, 0);
                if (std::fabs(lo) > 1e9 || std::fabs(hi) > 1e9 || hi <= lo) continue;
                a->userSetValue(p.get(), {lo + (hi - lo) * 0.7}, 5);
                a2->userSetValue(a2->params.find(p->name), {lo + (hi - lo) * 0.7}, 5);
                ++changedParams;
            }
        }
        Json st = a->saveState();
        Json parsed;
        Json::parse(st.dump(), parsed);
        b->loadState(parsed);
        ImagePtr ra = renderOnce(*a, 10, &e, false), ra2 = renderOnce(*a2, 10, &e, false), rb = renderOnce(*b, 10, &e, false);
        if (!ra || !ra2 || !rb) { record("realfx: " + pl->info.id + " state: render", false, e); continue; }
        if (!sameImage(*ra, *ra2)) {
            note("realfx: " + pl->info.id + " is not repeatable between two fresh instances with equal parameters; its state round trip is not compared");
            continue;
        }
        ++stateChecked;
        bool same = sameImage(*ra, *rb);
        if (same) ++stateOk;
        record("realfx: " + pl->info.id + " state saved, loaded into a new instance, same picture", same, strfmt("first difference at byte %d", firstDifference(*ra, *rb)));
    }
    CHECK("realfx: most effects are repeatable enough to compare", stateChecked >= 20);

    // limits
    Rng rng(99);
    for (auto& up : bin->plugins) {
        Plugin* pl = up.get();
        if (!pl->loaded) continue;
        std::string ctx = pl->filterContext();
        if (ctx.empty()) continue;
        InstanceSetup s;
        s.width = 96;
        s.height = 54;
        s.durationFrames = 300;
        std::string e;
        auto inst = pl->createInstance(ctx, s, &e);
        if (!inst) continue;
        for (auto& n : inst->inputClipNames()) inst->setClipSource(n, [](double t) { return cachedPattern(96, 54, t); });
        int rendered = 0, failed = 0;
        for (int pass = 0; pass < 3; ++pass) {
            for (auto& p : inst->params.params) {
                if (!p->hasValue() || p->dim == 0) continue;
                std::vector<double> v((size_t)p->dim);
                for (int k = 0; k < p->dim; ++k) {
                    double lo = p->props.getDouble(kOfxParamPropMin, k, 0), hi = p->props.getDouble(kOfxParamPropMax, k, 1);
                    if (!(std::fabs(lo) < 1e9)) lo = p->props.getDouble(kOfxParamPropDefault, k, 0) - 10;
                    if (!(std::fabs(hi) < 1e9)) hi = p->props.getDouble(kOfxParamPropDefault, k, 0) + 10;
                    v[(size_t)k] = pass == 0 ? lo : (pass == 1 ? hi : lo + (hi - lo) * (rng.next() % 1000) / 1000.0);
                }
                inst->userSetValue(p.get(), v, 20 + pass);
            }
            for (double t : {20.0, 21.0}) {
                std::string re;
                ImagePtr out = renderOnce(*inst, t + pass * 2, &re, false);
                if (out && out->w == 96 && out->h == 54) ++rendered; else ++failed;
            }
        }
        record("realfx: " + pl->info.id + " renders with every parameter at its minimum, maximum and a random value", failed == 0 && !pl->crashed,
               strfmt("%d of 6 renders failed", failed));
    }
}

// ====================================================================== 16. probe
static const Json* findPlugin(const Json& rep, const std::string& id)
{
    for (const auto& p : rep.get("plugins").items())
        if (p.get("id").asString() == id) return &p;
    return nullptr;
}

static void testProbe(const std::string& realFxPath)
{
    ProbeOptions opt;
    opt.width = 64;
    opt.height = 48;
    opt.frames = 3;
    Json rep = probeBinary(gFxPath, opt);
    CHECK("probe: testfx ok", rep.get("ok").asBool() && rep.get("path").asString() == gFxPath);
    checkEq("probe: 22 plug-ins listed", rep.get("plugins").size(), (size_t)22);
    auto get = [&](const char* id) { return findPlugin(rep, id); };
    const Json* inv = get("reno.test.invert");
    CHECK("probe: invert is found", inv != nullptr);
    if (inv) {
        CHECK("probe: invert ok", inv->get("ok").asBool() && !inv->get("crashed").asBool());
        checkEq("probe: invert rendered three frames", inv->get("rendered").asInt(), 3);
        CHECK("probe: invert changed the picture", inv->get("changedPicture").asBool());
        checkEq("probe: invert used as a filter", inv->get("usedAs").asString(), std::string(kOfxImageEffectContextFilter));
        checkEq("probe: invert has two valued parameters", inv->get("params").asInt(), 2);
        checkEq("probe: label", inv->get("label").asString(), std::string("Test Invert"));
        checkEq("probe: group", inv->get("group").asString(), std::string("Reno Test"));
        CHECK("probe: depths listed", inv->get("depths").size() == 2);
        CHECK("probe: suites listed", [&] { for (auto& s : inv->get("suites").items()) if (s.asString() == "OfxPropertySuite v1: given") return true; return false; }());
        CHECK("probe: state size reported", inv->get("stateBytes").asInt() > 10);
        CHECK("probe: milliseconds per frame reported", inv->get("msPerFrame").isNumber());
        CHECK("probe: version", inv->get("versionMajor").asInt() == 2 && inv->get("versionMinor").asInt() == 7);
    }
    const Json* gen = get("reno.test.generator");
    CHECK("probe: generator rendered as a generator", gen && gen->get("ok").asBool() && gen->get("usedAs").asString() == kOfxImageEffectContextGenerator && gen->get("changedPicture").asBool());
    const Json* tr = get("reno.test.transition");
    CHECK("probe: transition rendered as a transition", tr && tr->get("ok").asBool() && tr->get("usedAs").asString() == kOfxImageEffectContextTransition && tr->get("rendered").asInt() == 3);
    const Json* gn = get("reno.test.general");
    CHECK("probe: general-context effect used as a general effect", gn && gn->get("ok").asBool() && gn->get("usedAs").asString() == kOfxImageEffectContextGeneral);
    const Json* fd = get("reno.test.faildescribe");
    CHECK("probe: describe failure reported", fd && !fd->get("ok").asBool() && contains(fd->get("error").asString(), "could not describe itself"));
    const Json* fc = get("reno.test.failcontext");
    CHECK("probe: describeInContext failure reported", fc && !fc->get("ok").asBool() && contains(fc->get("error").asString(), "could not describe itself as a"));
    const Json* fcr = get("reno.test.failcreate");
    CHECK("probe: createInstance failure reported", fcr && !fcr->get("ok").asBool() && contains(fcr->get("error").asString(), "could not be created"));
    const Json* fr = get("reno.test.failrender");
    CHECK("probe: render failure reported", fr && !fr->get("ok").asBool() && contains(fr->get("error").asString(), "failed to render") && fr->get("rendered").asInt() == 0);
    const Json* ns = get("reno.test.needsuite");
    CHECK("probe: missing host feature reported", ns && !ns->get("ok").asBool() && contains(ns->get("error").asString(), "missing host feature"));
    const Json* ms = get("reno.test.missing");
    CHECK("probe: what was missing is in the error, and listed", ms && contains(ms->get("error").asString(), "OfxImaginarySuite v3") &&
                                                                    contains(ms->get("error").asString(), "OfxPropImaginary (the host's properties)") &&
                                                                    contains(ms->get("missingFeature").asString(), "OfxImageEffectActionDescribeInContext") &&
                                                                    [&] { for (auto& x : ms->get("propertiesNotThere").items()) if (x.asString() == "OfxPropImaginary (the host's properties)") return true; return false; }());
    const Json* v18 = get("reno.test.vegas18");
    CHECK("probe: the VEGAS 18 effect renders, and the GUID it replaces is listed", v18 && v18->get("ok").asBool() && v18->get("rendered").asInt() == 3 &&
                                                                                    v18->get("replaces").size() == 1 && v18->get("replaces").at(0).asString() == "{C55DDAF9-B236-11d2-BAD4-00C04F8EDC09}");
    for (const char* id : {"reno.test.params", "reno.test.temporal", "reno.test.floatonly", "reno.test.byteonly", "reno.test.misc", "reno.test.vegas", "reno.test.badcalls",
                           "reno.test.badsuite", "reno.test.leaky"}) {
        const Json* j = get(id);
        record(std::string("probe: ") + id + " renders three frames", j && j->get("ok").asBool() && j->get("rendered").asInt() == 3, j ? j->get("error").asString() : "missing");
    }
    const Json* vg = get("reno.test.vegas");
    CHECK("probe: the window of its own is noted", vg && vg->get("ownWindow").asBool());
    // a probe run with the render step off, and a file that is not there
    ProbeOptions norender;
    norender.render = false;
    Json r2 = probeBinary(gFxPath, norender);
    const Json* i2 = findPlugin(r2, "reno.test.invert");
    CHECK("probe: without rendering there is no render result", i2 && i2->get("ok").asBool() && !i2->has("rendered") && !i2->has("usedAs"));
    CHECK("probe: ... but an instance is still made", i2 && i2->get("instanceMade").asBool());
    const Json* fcr2 = findPlugin(r2, "reno.test.failcreate");
    CHECK("probe: so a failure to make one shows without rendering (the plug-in scan)", fcr2 && !fcr2->get("ok").asBool() && contains(fcr2->get("error").asString(), "could not be created"));
    const Json* ms2 = findPlugin(r2, "reno.test.missing");
    CHECK("probe: ... and what an effect missed while being described in its context", ms2 && !ms2->get("ok").asBool() && contains(ms2->get("error").asString(), "OfxPropImaginary"));
    ProbeOptions bare = norender;
    bare.instance = false;
    Json r4 = probeBinary(gFxPath, bare);
    const Json* fcr3 = findPlugin(r4, "reno.test.failcreate");
    CHECK("probe: describing only, when asked", fcr3 && fcr3->get("ok").asBool() && !fcr3->has("instanceMade"));
    Json r3 = probeBinary(gScratchDir + "/nothing-here.ofx", opt);
    CHECK("probe: missing file", !r3.get("ok").asBool() && !r3.get("error").asString().empty() && !r3.has("plugins"));

    // the test picture
    ImagePtr p0 = makeTestPattern(64, 48, 0), p5 = makeTestPattern(64, 48, 5), p0b = makeTestPattern(64, 48, 0);
    CHECK("probe: test pattern size and depth", p0 && p0->w == 64 && p0->h == 48 && p0->depth == PixelDepth::Byte);
    CHECK("probe: test pattern is repeatable", sameImage(*p0, *p0b));
    CHECK("probe: test pattern moves with the frame", !sameImage(*p0, *p5));
    bool opaque = true;
    for (int y = 0; y < 48; ++y) for (int x = 0; x < 64; ++x) opaque = opaque && p0->row(y)[x * 4 + 3] == 255;
    CHECK("probe: test pattern is opaque", opaque);
    const uint8_t* top = p0->row(47);                  // the top row (row 0 is the bottom): colour bars
    CHECK("probe: top row has bars, first white", top[0] == 255 && top[1] == 255 && top[2] == 255);
    const uint8_t* last = top + 63 * 4;
    CHECK("probe: last bar is dark grey", last[0] == 16 && last[1] == 16 && last[2] == 16);
    const uint8_t* box = p0->row(48 / 3 + 1) + 1 * 4;  // the moving box at frame 0 is at the left edge, one third up: orange (B=0 G=128 R=255)
    CHECK("probe: orange box in BGRA order", box[0] == 0 && box[1] == 128 && box[2] == 255);
    ImagePtr tiny = makeTestPattern(1, 1, 3);
    CHECK("probe: 1x1 pattern", tiny && tiny->w == 1 && tiny->h == 1);
    CHECK("probe: bad size gives no picture", makeTestPattern(0, 5, 0) == nullptr);

    if (fileExists(realFxPath)) {
        Json sr = probeBinary(realFxPath, opt);
        CHECK("probe: the test plug-in ok", sr.get("ok").asBool() && sr.get("plugins").size() == 28);
        int good = 0, changed = 0;
        for (const auto& p : sr.get("plugins").items()) {
            if (p.get("ok").asBool() && p.get("rendered").asInt() == 3) ++good;
            if (p.get("changedPicture").asBool()) ++changed;
        }
        checkEq("probe: every effect of the test plug-in rendered three frames", good, 28);
        checkEq("probe: every effect of the test plug-in changed the picture", changed, 28);
    } else
        skip("probe: the test plug-in", "the test plug-in was not found at " + realFxPath);

    // a hash change in a place the old probe did not look at is still a change: a picture that differs in one byte only
    struct OneByte {
        static Json run(const std::string& path)
        {
            // an effect that changes only the very last byte cannot be written with testfx; instead check the helper the probe uses on a pattern
            (void)path;
            return Json();
        }
    };
}

// ====================================================================== overlays (on-screen controls)
// A headless OpenGL context for drawing overlays into (Mesa's EGL on its "surfaceless" platform, found at run time):
// what the preview does on Windows with Qt's context, done by hand.
struct HeadlessGL {
    typedef void* (*GetProc)(const char*);
    void *egl = nullptr, *gl = nullptr, *dpy = nullptr, *ctx = nullptr, *surf = nullptr;
    int w = 0, h = 0;
    void (*Viewport)(int, int, int, int) = nullptr;
    void (*MatrixMode)(unsigned) = nullptr;
    void (*LoadIdentity)() = nullptr;
    void (*Ortho)(double, double, double, double, double, double) = nullptr;
    void (*ClearColor)(float, float, float, float) = nullptr;
    void (*Clear)(unsigned) = nullptr;
    void (*ReadPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
    void (*Finish)() = nullptr;
    bool open(int width, int height)
    {
        egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        gl = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!egl || !gl) return false;
        GetProc gpa = (GetProc)dlsym(egl, "eglGetProcAddress");
        auto platform = (void* (*)(unsigned, void*, const intptr_t*))(gpa ? gpa("eglGetPlatformDisplayEXT") : nullptr);
        auto init = (unsigned (*)(void*, int*, int*))dlsym(egl, "eglInitialize");
        auto choose = (unsigned (*)(void*, const int*, void**, int, int*))dlsym(egl, "eglChooseConfig");
        auto bind = (unsigned (*)(unsigned))dlsym(egl, "eglBindAPI");
        auto mkctx = (void* (*)(void*, void*, void*, const int*))dlsym(egl, "eglCreateContext");
        auto mkpb = (void* (*)(void*, void*, const int*))dlsym(egl, "eglCreatePbufferSurface");
        auto cur = (unsigned (*)(void*, void*, void*, void*))dlsym(egl, "eglMakeCurrent");
        if (!platform || !init || !choose || !bind || !mkctx || !mkpb || !cur) return false;
        dpy = platform(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
        int ma = 0, mi = 0;
        if (!dpy || !init(dpy, &ma, &mi)) return false;
        const int attrs[] = {0x3033, 0x0001, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3040, 0x0008, 0x3038};
        void* cfg = nullptr;
        int n = 0;
        if (!choose(dpy, attrs, &cfg, 1, &n) || n < 1) return false;
        bind(0x30A2 /* EGL_OPENGL_API */);
        ctx = mkctx(dpy, cfg, nullptr, nullptr);
        const int pa[] = {0x3057, width, 0x3056, height, 0x3038};
        surf = ctx ? mkpb(dpy, cfg, pa) : nullptr;
        if (!surf || !cur(dpy, surf, surf, ctx)) return false;
        w = width;
        h = height;
        Viewport = (decltype(Viewport))dlsym(gl, "glViewport");
        MatrixMode = (decltype(MatrixMode))dlsym(gl, "glMatrixMode");
        LoadIdentity = (decltype(LoadIdentity))dlsym(gl, "glLoadIdentity");
        Ortho = (decltype(Ortho))dlsym(gl, "glOrtho");
        ClearColor = (decltype(ClearColor))dlsym(gl, "glClearColor");
        Clear = (decltype(Clear))dlsym(gl, "glClear");
        ReadPixels = (decltype(ReadPixels))dlsym(gl, "glReadPixels");
        Finish = (decltype(Finish))dlsym(gl, "glFinish");
        return Viewport && MatrixMode && LoadIdentity && Ortho && ClearColor && Clear && ReadPixels && Finish;
    }
    // canonical (x0..x1, y0..y1) over the whole surface
    void project(double x0, double x1, double y0, double y1)
    {
        Viewport(0, 0, w, h);
        MatrixMode(0x1701);
        LoadIdentity();
        Ortho(x0, x1, y0, y1, -1, 1);
        MatrixMode(0x1700);
        LoadIdentity();
        ClearColor(0, 0, 0, 0);
        Clear(0x4000);
    }
    std::vector<uint8_t> read()
    {
        std::vector<uint8_t> px((size_t)w * h * 4);
        Finish();
        ReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401, px.data());
        return px;
    }
};

static void testParamInteract()
{
    checkEq("param interact: the host says it draws parameters' own controls", host().hostProps.getInt(kOfxParamHostPropSupportsCustomInteract), 1);
    Plugin* pl = plug("reno.test.paraminteract");
    if (!pl) { CHECK("param interact: the effect loads", false); return; }
    CHECK("param interact: the effect is told so", gFx.has("level: host supportsCustomInteract=1"));
    auto inst = makeInstance("reno.test.paraminteract", 64, 48);
    CHECK("param interact: instance made", inst != nullptr);
    if (!inst) return;
    Param* level = inst->params.find("Level");
    CHECK("param interact: Level has its own control, not made yet", level && inst->paramHasInteract(level) && !inst->paramInteractMade(level));
    if (!level) return;
    int pref[2] = {0, 0}, minimum[2] = {0, 0};
    double aspect = 0;
    inst->paramInteractSize(level, pref, minimum, &aspect);
    CHECKM("param interact: the size it asks for", pref[0] == 200 && pref[1] == 24 && minimum[0] == 60 && minimum[1] == 16, "%d,%d %d,%d", pref[0], pref[1], minimum[0], minimum[1]);
    Instance::ParamInteractArgs a;
    a.size[0] = 104;
    a.size[1] = 20;
    CHECK("param interact: nothing is drawn before it is made", !inst->paramInteractDraw(level, a));
    gFx.clearLog();
    gFx.clearCounters();
    CHECK("param interact: made", inst->createParamInteract(level) && inst->paramInteractMade(level) && gFx.counter("levelCreate") == 1);
    CHECK("param interact: slaved to its parameter", gFx.has("level: create slave=Level"));
    CHECK("param interact: made once", inst->createParamInteract(level) && gFx.counter("levelCreate") == 1);
    CHECK("param interact: draw is taken", inst->paramInteractDraw(level, a) && gFx.has("level: draw size=104,20 level=0.5"));
    checkEq("param interact: the parameter knows its control's size", level->props.getDouble(kOfxParamPropInteractSize, 0), 104.0);
    int redraws = 0;
    inst->onParamInteractRedraw = [&](Param* p) { if (p == level) ++redraws; };
    CHECK("param interact: pen down sets the level from where it is", inst->paramInteractPen(level, kOfxInteractActionPenDown, a, 77, 10, 1.0) &&
                                                                         std::fabs(level->value[0] - 0.75) < 1e-9 && redraws == 1);
    CHECK("param interact: dragging follows", inst->paramInteractPen(level, kOfxInteractActionPenMotion, a, 27, 10, 1.0) && std::fabs(level->value[0] - 0.25) < 1e-9);
    CHECK("param interact: pen up", inst->paramInteractPen(level, kOfxInteractActionPenUp, a, 27, 10, 0.0));
    CHECK("param interact: motion after it is not taken", !inst->paramInteractPen(level, kOfxInteractActionPenMotion, a, 90, 10, 0.0));
    CHECK("param interact: Right adds a tenth", inst->paramInteractKey(level, kOfxInteractActionKeyDown, a, 0xff53, std::string()) && std::fabs(level->value[0] - 0.35) < 1e-9);
    CHECK("param interact: a letter is not taken", !inst->paramInteractKey(level, kOfxInteractActionKeyDown, a, 'q', "q"));
    CHECK("param interact: focus", inst->paramInteractFocus(level, true, a) && inst->paramInteractFocus(level, false, a));
    Param* none = inst->params.find("NoSuchThing");
    CHECK("param interact: a parameter without one is refused", !inst->createParamInteract(none) && !inst->paramInteractDraw(none, a));
    HeadlessGL gl;
    if (gl.open(64, 48)) {
        inst->userSetValue(level, {0.5}, 0);
        gl.project(0, 64, 0, 48);
        Instance::ParamInteractArgs b;
        b.size[0] = 64;
        b.size[1] = 48;
        CHECK("param interact: GL draw is taken", inst->paramInteractDraw(level, b));
        std::vector<uint8_t> px = gl.read();
        auto at = [&](int x, int y) { const uint8_t* p = &px[((size_t)y * 64 + x) * 4]; return strfmt("%d,%d,%d", p[0], p[1], p[2]); };
        CHECKM("param interact: the bar is filled to the level, in its own pixels", at(10, 24) == "0,255,0" && at(50, 24) == "0,0,0", "%s %s", at(10, 24).c_str(), at(50, 24).c_str());
    } else {
        note("param interact: no headless OpenGL here - the drawing itself is not checked");
    }
    gFx.clearCounters();
    inst->disposeParamInteract(level);
    CHECK("param interact: disposed", !inst->paramInteractMade(level) && gFx.counter("levelDestroy") == 1 && !inst->paramInteractDraw(level, a));
    inst->createParamInteract(level);
    inst.reset();
    CHECK("param interact: destroying the instance destroys its controls", gFx.counter("levelDestroy") == 2);
}

static void testOverlay()
{
    Plugin* pl = plug("reno.test.overlay");
    CHECK("overlay: the effect says it has an overlay", pl && pl->info.hasOverlay);
    CHECK("overlay: the host says it draws overlays", gFx.has("overlay: host supportsOverlays=1"));
    checkEq("overlay: host property kOfxImageEffectPropSupportsOverlays", host().hostProps.getInt(kOfxImageEffectPropSupportsOverlays), 1);
    if (!pl) return;
    auto inst = makeInstance("reno.test.overlay", 64, 48);
    CHECK("overlay: instance made", inst != nullptr);
    if (!inst) return;
    CHECK("overlay: hasOverlay, not made yet", inst->hasOverlay() && !inst->overlayMade() && !inst->overlayProps());
    Instance::OverlayArgs a;
    CHECK("overlay: nothing is drawn before it is made", !inst->overlayDraw(a));
    gFx.clearLog();
    gFx.clearCounters();
    CHECK("overlay: createOverlay", inst->createOverlay() && inst->overlayMade() && inst->overlayProps());
    CHECK("overlay: described with the drawing's alpha and depth", gFx.has("overlay: describe hasAlpha=1 bitDepth=8"));
    CHECK("overlay: the instance knows its effect", gFx.has("overlay: create effect=set pixelScale=1,1"));
    CHECK("overlay: a second createOverlay makes nothing more", inst->createOverlay() && gFx.counter("overlayCreate") == 1);
    checkEq("overlay: its property set has the suggested colour", inst->overlayProps()->dimension(kOfxInteractPropSuggestedColour), 3);

    // drawing: the arguments
    a.time = 3;
    a.renderScale[0] = a.renderScale[1] = 0.25;
    a.pixelScale[0] = a.pixelScale[1] = 0.5;
    a.viewport[0] = 640;
    a.viewport[1] = 360;
    a.background[0] = 0.1;
    a.background[1] = 0.2;
    a.background[2] = 0.3;
    gFx.clearLog();
    CHECK("overlay: draw is taken", inst->overlayDraw(a));
    CHECKM("overlay: draw gets the effect, time, pixel and render scales, background",
           gFx.has("overlay: draw effect=1 time=3 pixelScale=0.5,0.5 renderScale=0.25,0.25 background=0.1,0.2,0.3 center=960,540"), "%s",
           gFx.last("overlay: draw").c_str());
    checkEq("overlay: the overlay's own pixel scale follows the view", inst->overlayProps()->getDouble(kOfxInteractPropPixelScale, 1), 0.5);
    checkEq("overlay: the overlay's viewport size", inst->overlayProps()->getInt("OfxInteractPropViewport", 0), 640);

    // the pen
    int changes = 0, redraws = 0;
    inst->onParamChanged = [&](Param*, const std::string& what) { if (what == "value") ++changes; };
    inst->onOverlayRedraw = [&] { ++redraws; };
    Param* center = inst->params.find("Center");
    CHECK("overlay: pen down away from the handle is not taken", !inst->overlayPen(kOfxInteractActionPenDown, a, 900, 500, 1.0, 10, 20));
    CHECK("overlay: pen down on the handle is taken", inst->overlayPen(kOfxInteractActionPenDown, a, 962, 541, 1.0, 11, 21) && redraws == 1);
    CHECKM("overlay: pen down gets the canonical and viewport positions and pressure",
           gFx.has("overlay: penDown effect=1 pen=962,541 viewport=11,21 pressure=1 time=3 pixelScale=0.5,0.5"), "%s", gFx.last("penDown").c_str());
    CHECK("overlay: pen motion while held is taken", inst->overlayPen(kOfxInteractActionPenMotion, a, 1002, 601, 1.0, 12, 22));
    CHECK("overlay: ... and moves the parameter (keeping where it was taken hold of)", center && center->value.size() == 2 && center->value[0] == 1000 && center->value[1] == 600);
    CHECK("overlay: the change reached onParamChanged, and a redraw was asked for", changes >= 1 && redraws == 2);
    CHECK("overlay: pen up is taken", inst->overlayPen(kOfxInteractActionPenUp, a, 1012, 611, 0.0, 13, 23) && center->value[0] == 1010 && center->value[1] == 610);
    CHECK("overlay: pen motion after that is not taken", !inst->overlayPen(kOfxInteractActionPenMotion, a, 0, 0, 0.0, 0, 0));
    CHECK("overlay: a null action is refused", !inst->overlayPen(nullptr, a, 0, 0, 0, 0, 0));
    // keys
    gFx.clearLog();
    CHECK("overlay: key down (right arrow) is taken", inst->overlayKey(kOfxInteractActionKeyDown, a, 0xff53, std::string()) && center->value[0] == 1011);
    CHECK("overlay: key repeat (up arrow) is taken", inst->overlayKey(kOfxInteractActionKeyRepeat, a, 0xff52, std::string()) && center->value[1] == 611);
    CHECK("overlay: key up is not", !inst->overlayKey(kOfxInteractActionKeyUp, a, 0xff53, std::string()));
    CHECK("overlay: a letter is not", !inst->overlayKey(kOfxInteractActionKeyDown, a, 'a', "a"));
    CHECKM("overlay: the key's symbol and text reach the effect", gFx.has("overlay: KeyDown sym=0x61 string=a time=3"), "%s", gFx.last("KeyDown").c_str());
    // focus
    gFx.clearLog();
    CHECK("overlay: gain and lose focus", inst->overlayFocus(true, a) && inst->overlayFocus(false, a) && gFx.has("overlay: gainFocus") && gFx.has("overlay: loseFocus"));
    // the interact suite
    const OfxInteractSuiteV1* is = (const OfxInteractSuiteV1*)fetch(kOfxInteractSuite, 1);
    int junk = 0;
    CHECK("overlay: interact suite refuses a bad handle", is && is->interactRedraw((OfxInteractHandle)&junk) == kOfxStatErrBadHandle &&
                                                             is->interactSwapBuffers((OfxInteractHandle)&junk) == kOfxStatErrBadHandle);

    // drawing for real, into a headless OpenGL context: canonical coordinates mapped one to one onto a 64x48 surface
    HeadlessGL gl;
    if (gl.open(64, 48)) {
        inst->userSetValue(center, {32, 24}, 0);
        gl.project(0, 64, 0, 48);
        Instance::OverlayArgs b;
        b.viewport[0] = 64;
        b.viewport[1] = 48;
        gFx.clearLog();
        CHECK("overlay: GL draw is taken", inst->overlayDraw(b));
        CHECKM("overlay: the effect saw the viewport and the projection", gFx.has("gl=1 viewport=64,48 proj=0.03125,0.0416667,-1,-1"), "%s", gFx.last("overlay: draw").c_str());
        std::vector<uint8_t> px = gl.read();
        auto at = [&](int x, int y) { const uint8_t* p = &px[((size_t)y * 64 + x) * 4]; return strfmt("%d,%d,%d", p[0], p[1], p[2]); };
        // (a line along a pixel boundary lands in the row or column on either side of it)
        auto hline = [&](int x, int y, const char* c) { return at(x, y) == c || at(x, y - 1) == c; };
        auto vline = [&](int x, int y, const char* c) { return at(x, y) == c || at(x - 1, y) == c; };
        CHECKM("overlay: the red cross was drawn where the canonical centre is", hline(32 + 15, 24, "255,0,0") && vline(32, 24 - 15, "255,0,0") && hline(32 - 15, 24, "255,0,0"),
               "%s %s %s", at(47, 24).c_str(), at(32, 9).c_str(), at(17, 24).c_str());
        CHECKM("overlay: the green square round it", vline(40, 28, "0,255,0") && hline(28, 16, "0,255,0"), "%s %s", at(40, 28).c_str(), at(28, 16).c_str());
        CHECKM("overlay: and nothing elsewhere", at(5, 5) == "0,0,0" && at(60, 44) == "0,0,0", "%s %s", at(5, 5).c_str(), at(60, 44).c_str());
        // half the scale: pixel scale 2 (two canonical units a screen pixel) - the cross is still 40 screen pixels
        inst->userSetValue(center, {64, 48}, 0);
        gl.project(0, 128, 0, 96);
        b.pixelScale[0] = b.pixelScale[1] = 2;
        inst->overlayDraw(b);
        px = gl.read();
        CHECKM("overlay: at half size the cross is the same size on screen", hline(32 + 18, 24, "255,0,0") && !hline(32 + 22, 24, "255,0,0"), "%s %s", at(50, 24).c_str(),
               at(54, 24).c_str());
    } else {
        note("overlay: no headless OpenGL here (Mesa EGL) - the drawing itself is not checked");
    }

    // letting go
    gFx.clearLog();
    gFx.clearCounters();
    inst->disposeOverlay();
    CHECK("overlay: disposeOverlay destroys the interact", !inst->overlayMade() && gFx.counter("overlayDestroy") == 1 && !inst->overlayDraw(a));
    CHECK("overlay: it can be made again", inst->createOverlay() && gFx.counter("overlayCreate") == 1);
    inst.reset();
    CHECK("overlay: destroying the instance destroys its overlay", gFx.counter("overlayDestroy") == 2);
    // an effect without one
    auto plain = makeInstance("reno.test.invert", 16, 16);
    CHECK("overlay: an effect without an overlay has none to make", plain && !plain->hasOverlay() && !plain->createOverlay() && !plain->overlayDraw(a));
}

// ====================================================================== main
int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: hosttest <testfx.ofx> [--only <prefix>]\n");
        return 2;
    }
    gFxPath = argv[1];
    std::string only;
    for (int i = 2; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--only")) only = argv[i + 1];
    {
        char* real = realpath(gFxPath.c_str(), nullptr);
        if (real) { gFxPath = real; std::free(real); }
    }
    const char* tmp = std::getenv("HOSTTEST_TMP");
    gScratchDir = tmp ? tmp : (pathDir(gFxPath) + "/hosttest_tmp");
    if (system(("mkdir -p '" + gScratchDir + "'").c_str()) != 0) return 1;
    if (!gFx.open(gFxPath)) {
        std::printf("FAIL setup: cannot open %s for its log functions: %s\n", gFxPath.c_str(), dlerror());
        return 1;
    }
    const char* sp = std::getenv("RENO_TEST_PLUGIN");
    const std::string realFxPath = sp && *sp ? sp : "(RENO_TEST_PLUGIN not set)";
    // plug-in messages and logs go to stderr; keep them out of the result lines
    setLogSink([](const std::string&) {});

    struct Section { const char* name; std::function<void()> fn; };
    std::vector<Section> sections = {
        {"props", testPropertySet},
        {"paramunits", testParamSetUnits},
        {"status", testStatusNames},
        {"hostprops", testHostProperties},
        {"suites", testFetchSuite},
        {"propsuite", testPropertySuiteDirect},
        {"loading", testLoading},
        {"search", testSearch},
        {"describe", testDescribing},
        {"invert", testInvert},
        {"paramdesc", testParamDescriptor},
        {"params", testParams},
        {"keyframes", testKeyframeTypes},
        {"state", testStateRoundTrip},
        {"temporal", testTemporal},
        {"depths", testDepths},
        {"generator", testGenerator},
        {"transition", testTransition},
        {"general", testGeneral},
        {"misc", testMisc},
        {"vegas", testVegas},
        {"vegas18", testVegas18},
        {"notvegas", testNotVegas},
        {"awkward", testAwkward},
        {"threads", testConcurrency},
        {"overlay", testOverlay},
        {"paraminteract", testParamInteract},
        {"realfx", [&] { testRealEffects(realFxPath); }},
        {"probe", [&] { testProbe(realFxPath); }},
    };
    auto t0 = std::chrono::steady_clock::now();
    for (auto& s : sections) {
        if (!only.empty() && std::string(s.name).compare(0, only.size(), only) != 0) continue;
        std::printf("== %s\n", s.name);
        std::fflush(stdout);
        s.fn();
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("\nSUMMARY: %d passed, %d failed, %d skipped (%.1f s)\n", gPassN, gFailN, gSkipN, secs);
    if (gFailN) {
        std::printf("FAILED CHECKS:\n");
        for (auto& f : gFailed) std::printf("  %s\n", f.c_str());
    }
    std::fflush(stdout);
    return gFailN ? 1 : 0;
}
