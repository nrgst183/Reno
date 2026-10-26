// testfx - a small OpenFX plug-in binary built only to exercise Reno's OpenFX host.
//
// Every effect is written straight against the C API in third_party/openfx (no support library) and is deliberately
// simple so the host test can predict its output exactly. The effects write what they see (values read back,
// statuses returned by the host, the order of actions) into a log the host test reads through the testfx_* functions
// exported at the bottom of the file. The binary has no dependency on Reno.
#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxInteract.h"
#include "ofxMemory.h"
#include "ofxMessage.h"
#include "ofxMultiThread.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// ------------------------------------------------------------------ VEGAS names (as the host spells them)
#define VEGAS_HOST_NAME            "com.vegascreativesoftware.vegas"     // VEGAS Pro 14 and later
#define SONY_VEGAS_HOST_NAME       "com.sonycreativesoftware.vegas"      // VEGAS Pro 13 and older
#define V_PIXEL_ORDER              "OfxImageEffectPropPixelOrder"
#define V_ORDER_BGRA               "OfxImagePixelOrderBGRA"
#define V_CONTEXT                  "OfxImageEffectPropVegasContext"
#define V_VIEWS_TO_RENDER          "OfxImageEffectPropViewsToRender"
#define V_RENDER_VIEW              "OfxImageEffectPropRenderView"
#define V_RENDER_QUALITY           "OfxImageEffectPropRenderQuality"
#define V_ACTION_ABOUT             "OfxImageEffectActionInvokeAbout"
#define V_HWND_SUITE               "OfxHWndInteractSuite"
#define V_PROGRESS_SUITE           "OfxVegasProgressSuite"
#define V_STEREO_SUITE             "OfxVegasStereoscopicImageEffectSuite"
#define V_KEYFRAME_SUITE           "OfxVegasKeyframeSuite"
#define V_HWND_ENTRY_PROP          "OfxImageEffectPluginPropHWndInteractV1"
#define V_HWND_PARENT              "OfxHWndInteractPropParent"
#define V_HWND_MIN_SIZE            "OfxHWndInteractPropMinSize"
#define V_HWND_PREF_SIZE           "OfxHWndInteractPropPrefferedSize"
#define V_HWND_LOCATION            "OfxHWndInteractPropLocation"
#define V_HWND_CREATE              "OfxHWndInteractActionCreateWindow"
#define V_HWND_MOVE                "OfxHWndInteractActionMoveWindow"
#define V_HWND_DISPOSE             "OfxHWndInteractActionDisposeWindow"
#define V_HWND_SHOW                "OfxHWndInteractActionShowWindow"

// The suite layouts the host publishes (the function order is what a plug-in relies on).
struct VHwndSuite { OfxStatus (*getPropertySet)(OfxInteractHandle, OfxPropertySetHandle*); OfxStatus (*update)(OfxInteractHandle); };
struct VProgressSuite1 { OfxStatus (*start)(void*, const char*, const char*); OfxStatus (*update)(void*, double); OfxStatus (*end)(void*); };
struct VProgressSuite2 { OfxStatus (*start)(void*, const char*, const char*, int); OfxStatus (*update)(void*, double); OfxStatus (*end)(void*); };
struct VStereoSuite { OfxStatus (*getImage)(OfxImageClipHandle, OfxTime, int, OfxRectD*, OfxPropertySetHandle*); };
struct VKeyframeSuite {
    OfxStatus (*getInterp)(OfxParamHandle, OfxTime, char**);
    OfxStatus (*getSlopes)(OfxParamHandle, OfxTime, ...);
    OfxStatus (*setInterp)(OfxParamHandle, OfxTime, const char*);
    OfxStatus (*setSlopes)(OfxParamHandle, OfxTime, ...);
};

// ------------------------------------------------------------------ log shared with the test
static std::mutex gLogMutex;
static std::deque<std::string> gLogLines;      // (a deque: pointers to lines stay valid while more are added)

static void logf(const char* fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(gLogMutex);
    gLogLines.push_back(buf);
}

static std::mutex gCounterMutex;
static std::map<std::string, int> gCounters;
static void bump(const char* name, int by = 1)
{
    std::lock_guard<std::mutex> lk(gCounterMutex);
    gCounters[name] += by;
}

static std::mutex gDumpMutex;
static std::map<std::string, std::string> gDumps;     // last value dump per effect
static void setDump(const char* effect, const std::string& s)
{
    std::lock_guard<std::mutex> lk(gDumpMutex);
    gDumps[effect] = s;
}

// ------------------------------------------------------------------ suites
static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEff = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;
static const OfxMemorySuiteV1* gMem = nullptr;
static const OfxMultiThreadSuiteV1* gMT = nullptr;
static const OfxMessageSuiteV1* gMsg1 = nullptr;
static const OfxMessageSuiteV2* gMsg2 = nullptr;
static const OfxInteractSuiteV1* gInteract = nullptr;
static const VHwndSuite* gVHwnd = nullptr;
static const VProgressSuite1* gVProg1 = nullptr;
static const VProgressSuite2* gVProg2 = nullptr;
static const VStereoSuite* gVStereo = nullptr;
static const VKeyframeSuite* gVKey = nullptr;
static bool gHostIsVegas = false;

static const void* suite(const char* name, int version)
{
    return gHost && gHost->fetchSuite ? gHost->fetchSuite(gHost->host, name, version) : nullptr;
}

static void fetchSuites()
{
    if (!gHost) return;
    gProp = (const OfxPropertySuiteV1*)suite(kOfxPropertySuite, 1);
    gEff = (const OfxImageEffectSuiteV1*)suite(kOfxImageEffectSuite, 1);
    gParam = (const OfxParameterSuiteV1*)suite(kOfxParameterSuite, 1);
    gMem = (const OfxMemorySuiteV1*)suite(kOfxMemorySuite, 1);
    gMT = (const OfxMultiThreadSuiteV1*)suite(kOfxMultiThreadSuite, 1);
    gMsg2 = (const OfxMessageSuiteV2*)suite(kOfxMessageSuite, 2);
    gMsg1 = (const OfxMessageSuiteV1*)suite(kOfxMessageSuite, 1);
    gInteract = (const OfxInteractSuiteV1*)suite(kOfxInteractSuite, 1);
}

// ------------------------------------------------------------------ property helpers (quiet when something is missing)
typedef OfxPropertySetHandle PS;

static int gi(PS h, const char* n, int idx = 0, int def = 0)
{
    int v = def;
    if (gProp->propGetInt(h, n, idx, &v) != kOfxStatOK) return def;
    return v;
}
static double gd(PS h, const char* n, int idx = 0, double def = 0)
{
    double v = def;
    if (gProp->propGetDouble(h, n, idx, &v) != kOfxStatOK) return def;
    return v;
}
static const char* gs(PS h, const char* n, int idx = 0)
{
    char* v = nullptr;
    if (gProp->propGetString(h, n, idx, &v) != kOfxStatOK || !v) return "";
    return v;
}
static void* gp(PS h, const char* n, int idx = 0)
{
    void* v = nullptr;
    if (gProp->propGetPointer(h, n, idx, &v) != kOfxStatOK) return nullptr;
    return v;
}
static void si(PS h, const char* n, int idx, int v) { gProp->propSetInt(h, n, idx, v); }
static void sd(PS h, const char* n, int idx, double v) { gProp->propSetDouble(h, n, idx, v); }
static void ss(PS h, const char* n, int idx, const char* v) { gProp->propSetString(h, n, idx, v); }
static void sp(PS h, const char* n, int idx, void* v) { gProp->propSetPointer(h, n, idx, v); }

static PS effectProps(OfxImageEffectHandle e)
{
    PS p = nullptr;
    gEff->getPropertySet(e, &p);
    return p;
}

// ------------------------------------------------------------------ status checks
// A check that fails is logged and counted; effects that run checks return kOfxStatFailed if any failed,
// so the host test sees it twice (the render fails and the log says which call).
struct Checks {
    int bad = 0;
    void expect(const char* what, OfxStatus got, OfxStatus want)
    {
        if (got != want) { logf("CHECKFAIL %s: got %d want %d", what, got, want); ++bad; }
    }
    void expectAny(const char* what, OfxStatus got, std::initializer_list<OfxStatus> want)
    {
        for (OfxStatus w : want) if (got == w) return;
        logf("CHECKFAIL %s: got %d", what, got);
        ++bad;
    }
    void require(const char* what, bool ok)
    {
        if (!ok) { logf("CHECKFAIL %s", what); ++bad; }
    }
};

// ------------------------------------------------------------------ pictures
struct Img {
    PS h = nullptr;
    uint8_t* base = nullptr;
    int rb = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool isFloat = false, bgra = false;
    OfxStatus st = kOfxStatFailed;
    bool ok() const { return st == kOfxStatOK && base; }
    uint8_t* px(int x, int y) const { return base + (ptrdiff_t)(y - y1) * rb + (ptrdiff_t)(x - x1) * (isFloat ? 16 : 4); }
    int ri() const { return (!isFloat && bgra) ? 2 : 0; }      // where red lives in a pixel
    int bi() const { return (!isFloat && bgra) ? 0 : 2; }      // where blue lives in a pixel
};

static Img getImg(OfxImageClipHandle clip, double t)
{
    Img im;
    bump("getImage");
    im.st = gEff->clipGetImage(clip, t, nullptr, &im.h);
    if (im.st != kOfxStatOK) { im.h = nullptr; return im; }
    im.base = (uint8_t*)gp(im.h, kOfxImagePropData);
    int b[4] = {0, 0, 0, 0};
    gProp->propGetIntN(im.h, kOfxImagePropBounds, 4, b);
    im.x1 = b[0]; im.y1 = b[1]; im.x2 = b[2]; im.y2 = b[3];
    im.rb = gi(im.h, kOfxImagePropRowBytes);
    im.isFloat = !strcmp(gs(im.h, kOfxImageEffectPropPixelDepth), kOfxBitDepthFloat);
    // Blue-first 8-bit pictures: the property is absent on a host that is not VEGAS-like, and then the order is the standard one.
    im.bgra = !strcmp(gs(im.h, V_PIXEL_ORDER), V_ORDER_BGRA);
    return im;
}

static void releaseImg(Img& im)
{
    if (im.h) { bump("releaseImage"); gEff->clipReleaseImage(im.h); }
    im.h = nullptr;
}

static inline float* F(uint8_t* p) { return (float*)p; }
static inline uint8_t clampByte(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

struct Win { int x1, y1, x2, y2; };
static Win renderWindow(PS in)
{
    int w[4] = {0, 0, 0, 0};
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, w);
    return Win{w[0], w[1], w[2], w[3]};
}

// ------------------------------------------------------------------ per-instance data
struct Inst {
    OfxImageEffectHandle eff = nullptr;
    OfxParamSetHandle ps = nullptr;
    OfxImageClipHandle src = nullptr, src2 = nullptr, dst = nullptr;
    std::map<std::string, OfxParamHandle> pm;
    int pushCount = 0;
    int renderCount = 0;
};

static Inst* inst(OfxImageEffectHandle e)
{
    PS p = effectProps(e);
    return p ? (Inst*)gp(p, kOfxPropInstanceData) : nullptr;
}

static OfxParamHandle P(Inst* d, const char* name)
{
    auto it = d->pm.find(name);
    return it == d->pm.end() ? nullptr : it->second;
}
static double getD(Inst* d, const char* n, double t)
{
    double v = 0;
    gParam->paramGetValueAtTime(P(d, n), t, &v);
    return v;
}
static int getI(Inst* d, const char* n, double t)
{
    int v = 0;
    gParam->paramGetValueAtTime(P(d, n), t, &v);
    return v;
}

static void defineClip(OfxImageEffectHandle e, const char* name, bool optional = false, bool temporal = false)
{
    PS cp = nullptr;
    gEff->clipDefine(e, name, &cp);
    ss(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    if (optional) si(cp, kOfxImageClipPropOptional, 0, 1);
    if (temporal) si(cp, kOfxImageEffectPropTemporalClipAccess, 0, 1);
}

static PS defineParam(OfxParamSetHandle ps, const char* type, const char* name, const char* label)
{
    PS pp = nullptr;
    OfxStatus st = gParam->paramDefine(ps, type, name, &pp);
    if (st != kOfxStatOK) { logf("define %s failed: %d", name, st); return nullptr; }
    ss(pp, kOfxPropLabel, 0, label);
    return pp;
}

// the usual start of an instance: remember the handles
static OfxStatus makeInstance(OfxImageEffectHandle e, const char* params[], int nParams)
{
    // a plug-in that takes its time to start (Reno's splash_test): TESTFX_SLOW_CREATE_MS milliseconds
    if (const char* ms = std::getenv("TESTFX_SLOW_CREATE_MS")) std::this_thread::sleep_for(std::chrono::milliseconds(std::atoi(ms)));
    Inst* d = new Inst();
    d->eff = e;
    gEff->getParamSet(e, &d->ps);
    gEff->clipGetHandle(e, kOfxImageEffectOutputClipName, &d->dst, nullptr);
    gEff->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &d->src, nullptr);
    for (int k = 0; k < nParams; ++k) {
        OfxParamHandle h = nullptr;
        if (gParam->paramGetHandle(d->ps, params[k], &h, nullptr) == kOfxStatOK) d->pm[params[k]] = h;      // (not every effect has every name)
    }
    sp(effectProps(e), kOfxPropInstanceData, 0, d);
    bump("createInstance");
    return kOfxStatOK;
}

static OfxStatus freeInstance(OfxImageEffectHandle e, const char* who)
{
    Inst* d = inst(e);
    delete d;
    sp(effectProps(e), kOfxPropInstanceData, 0, nullptr);
    bump("destroyInstance");
    logf("%s: destroyInstance", who);
    return kOfxStatOK;
}

// ------------------------------------------------------------------ describe helpers
static void describeBasics(OfxImageEffectHandle e, const char* label, const char* group, const char* description,
                           bool byte, bool flt, std::initializer_list<const char*> contexts, bool temporal = false)
{
    PS p = effectProps(e);
    ss(p, kOfxPropLabel, 0, label);
    ss(p, kOfxPropShortLabel, 0, label);
    ss(p, kOfxPropLongLabel, 0, label);
    ss(p, kOfxImageEffectPluginPropGrouping, 0, group);
    ss(p, kOfxPropPluginDescription, 0, description);
    int k = 0;
    for (const char* c : contexts) ss(p, kOfxImageEffectPropSupportedContexts, k++, c);
    k = 0;
    if (byte) ss(p, kOfxImageEffectPropSupportedPixelDepths, k++, kOfxBitDepthByte);
    if (flt) ss(p, kOfxImageEffectPropSupportedPixelDepths, k++, kOfxBitDepthFloat);
    // (Reno's lanes_test: TESTFX_SAFETY=unsafe or instance makes every effect say it is "unsafe" or "instance safe")
    const char* safety = std::getenv("TESTFX_SAFETY");
    ss(p, kOfxImageEffectPluginRenderThreadSafety, 0,
       safety && !std::strcmp(safety, "unsafe") ? kOfxImageEffectRenderUnsafe : (safety && !std::strcmp(safety, "instance") ? kOfxImageEffectRenderInstanceSafe : kOfxImageEffectRenderFullySafe));
    si(p, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    // (Reno's fxscale_test: with TESTFX_MULTIRES set every effect says it can render at a fraction of the project's size)
    si(p, kOfxImageEffectPropSupportsMultiResolution, 0, std::getenv("TESTFX_MULTIRES") ? 1 : 0);
    si(p, kOfxImageEffectPropSupportsTiles, 0, 0);
    si(p, kOfxImageEffectPropTemporalClipAccess, 0, temporal ? 1 : 0);
}

// A slow effect (Reno's framedrop_test): with TESTFX_SLOW_RENDER_MS set, Test Invert takes that many milliseconds a
// frame. With TESTFX_SLOW_RENDER_ABORT set too it asks the host's abort() every 5 ms meanwhile, as a well-behaved
// plug-in does, and gives up when told to (counted as "renderAborted"); without it, it never looks. False: aborted.
static bool slowRender(Inst* d)
{
    const char* ms = std::getenv("TESTFX_SLOW_RENDER_MS");
    if (!ms) return true;
    const bool asks = std::getenv("TESTFX_SLOW_RENDER_ABORT") != nullptr;
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::atoi(ms));
    while (std::chrono::steady_clock::now() < until) {
        if (asks && gEff->abort(d->eff)) {
            bump("renderAborted");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(asks ? 5 : 20));
    }
    bump("slowRender");
    return true;
}

// ====================================================================== 1. invert
// Out colour = alpha - colour (so a premultiplied picture stays valid), alpha unchanged. "ZeroRed" also zeroes the
// channel the effect believes is red, which shows which order the host really hands over.
static OfxStatus renderInvert(Inst* d, double t, PS in)
{
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    if (!s.ok() || !o.ok()) { releaseImg(s); releaseImg(o); return kOfxStatFailed; }
    const int zero = getI(d, "ZeroRed", t);
    Win w = renderWindow(in);
    if (std::getenv("TESTFX_MULTIRES"))
        logf("scaled: renderScale=%g,%g image=%d,%d,%d,%d rod=%d,%d window=%d,%d,%d,%d", gd(in, kOfxImageEffectPropRenderScale, 0), gd(in, kOfxImageEffectPropRenderScale, 1),
             s.x1, s.y1, s.x2, s.y2, gi(s.h, kOfxImagePropRegionOfDefinition, 2), gi(s.h, kOfxImagePropRegionOfDefinition, 3), w.x1, w.y1, w.x2, w.y2);
    logf("invert: depth=%s srcOrder=%s dstOrder=%s rowBytes=%d window=%d,%d,%d,%d", s.isFloat ? "float" : "byte",
         s.bgra ? "BGRA" : "RGBA", o.bgra ? "BGRA" : "RGBA", s.rb, w.x1, w.y1, w.x2, w.y2);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            uint8_t* a = s.px(x, y);
            uint8_t* b = o.px(x, y);
            if (!s.isFloat) {
                int al = a[3];
                for (int c = 0; c < 3; ++c) b[c] = clampByte(al - a[c]);
                b[3] = a[3];
                if (zero) b[o.ri()] = 0;
            } else {
                float al = F(a)[3];
                for (int c = 0; c < 3; ++c) F(b)[c] = al - F(a)[c];
                F(b)[3] = al;
                if (zero) F(b)[0] = 0.f;
            }
        }
    releaseImg(s);
    releaseImg(o);
    return kOfxStatOK;
}

// ====================================================================== 2. temporal
// Average of the source at time-1, time and time+1. A frame the host cannot supply counts as transparent black.
static OfxStatus renderTemporal(Inst* d, double t, PS in)
{
    Img s[3];
    OfxStatus st[3];
    for (int k = 0; k < 3; ++k) {
        s[k] = getImg(d->src, t + k - 1);
        st[k] = s[k].st;
    }
    Img o = getImg(d->dst, t);
    logf("temporal: t=%g status=%d,%d,%d", t, st[0], st[1], st[2]);
    if (!o.ok()) { for (auto& i : s) releaseImg(i); releaseImg(o); return kOfxStatFailed; }
    Win w = renderWindow(in);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            uint8_t* b = o.px(x, y);
            for (int c = 0; c < 4; ++c) {
                if (!o.isFloat) {
                    int sum = 0;
                    for (int k = 0; k < 3; ++k) if (s[k].ok()) sum += s[k].px(x, y)[c];
                    b[c] = (uint8_t)((sum + 1) / 3);
                } else {
                    float sum = 0.f;
                    for (int k = 0; k < 3; ++k) if (s[k].ok()) sum += F(s[k].px(x, y))[c];
                    F(b)[c] = sum / 3.f;
                }
            }
        }
    for (auto& i : s) releaseImg(i);
    releaseImg(o);
    return kOfxStatOK;
}

// ====================================================================== 3. float only / byte only
static OfxStatus renderFloatOnly(Inst* d, double t, PS in)
{
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    if (!s.ok() || !o.ok()) { releaseImg(s); releaseImg(o); return kOfxStatFailed; }
    PS clipProps = nullptr;
    gEff->clipGetPropertySet(d->src, &clipProps);
    logf("floatonly: imageDepth=%s clipDepth=%s order=%s", s.isFloat ? "float" : "byte", gs(clipProps, kOfxImageEffectPropPixelDepth),
         s.bgra ? "BGRA" : "RGBA");
    if (!s.isFloat || !o.isFloat) { releaseImg(s); releaseImg(o); return kOfxStatFailed; }
    Win w = renderWindow(in);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            float* a = F(s.px(x, y));
            float* b = F(o.px(x, y));
            b[0] = a[0];
            b[1] = a[1] * 0.5f;
            b[2] = a[2];
            b[3] = a[3];
        }
    releaseImg(s);
    releaseImg(o);
    return kOfxStatOK;
}

static OfxStatus renderByteOnly(Inst* d, double t, PS in)
{
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    if (!s.ok() || !o.ok()) { releaseImg(s); releaseImg(o); return kOfxStatFailed; }
    PS clipProps = nullptr;
    gEff->clipGetPropertySet(d->src, &clipProps);
    logf("byteonly: imageDepth=%s clipDepth=%s order=%s", s.isFloat ? "float" : "byte", gs(clipProps, kOfxImageEffectPropPixelDepth),
         s.bgra ? "BGRA" : "RGBA");
    if (s.isFloat || o.isFloat) { releaseImg(s); releaseImg(o); return kOfxStatFailed; }
    Win w = renderWindow(in);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            uint8_t* a = s.px(x, y);
            uint8_t* b = o.px(x, y);
            memcpy(b, a, 4);
            b[o.bi()] = clampByte(a[s.bi()] + 10);          // blue + 10, wherever blue is
        }
    releaseImg(s);
    releaseImg(o);
    return kOfxStatOK;
}

// ====================================================================== plain copy (used by several awkward effects)
static OfxStatus copyImage(const Img& s, const Img& o, Win w)
{
    for (int y = w.y1; y < w.y2; ++y)
        memcpy(o.px(w.x1, y), s.px(w.x1, y), (size_t)(w.x2 - w.x1) * (s.isFloat ? 16 : 4));
    return kOfxStatOK;
}

static OfxStatus renderCopy(Inst* d, double t, PS in)
{
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    OfxStatus st = kOfxStatFailed;
    if (s.ok() && o.ok()) st = copyImage(s, o, renderWindow(in));
    releaseImg(s);
    releaseImg(o);
    return st;
}

// ====================================================================== kinds of simple filters
enum Kind { K_INVERT, K_TEMPORAL, K_FLOATONLY, K_BYTEONLY, K_LEAKY, K_FAILRENDER, K_BADSUITE, K_BADCALLS };

// ---- 16. badcalls: invalid handles, unknown properties, bad indexes -> documented error codes
static void badCallsDescribeChecks(OfxImageEffectHandle e)
{
    Checks c;
    OfxParamSetHandle ps = nullptr;
    gEff->getParamSet(e, &ps);
    PS pp = nullptr;
    c.expect("define first", gParam->paramDefine(ps, kOfxParamTypeDouble, "Dup", &pp), kOfxStatOK);
    c.expect("define dup", gParam->paramDefine(ps, kOfxParamTypeDouble, "Dup", &pp), kOfxStatErrExists);
    c.expect("define unknown type", gParam->paramDefine(ps, "OfxParamTypeNoSuchThing", "Weird", &pp), kOfxStatErrUnknown);
    c.expect("define null set", gParam->paramDefine(nullptr, kOfxParamTypeDouble, "X", &pp), kOfxStatErrBadHandle);
    c.expect("define StrChoice (known, unsupported)", gParam->paramDefine(ps, kOfxParamTypeStrChoice, "Str", &pp), kOfxStatErrUnsupported);
    c.expect("paramGetHandle in describe, unknown", gParam->paramGetHandle(ps, "NoSuch", nullptr, nullptr), kOfxStatErrUnknown);
    c.expect("clipDefine null", gEff->clipDefine(nullptr, "X", &pp), kOfxStatErrBadHandle);
    logf("badcalls: describe checks bad=%d", c.bad);
    bump("describeCheckFailures", c.bad);
}

static OfxStatus renderBadCalls(Inst* d, double t, PS in)
{
    Checks c;
    PS ep = effectProps(d->eff);
    char* sv = nullptr;
    int iv = 0;
    double dv = 0;
    void* pv = nullptr;
    // properties
    c.expect("getInt null set", gProp->propGetInt(nullptr, kOfxPropName, 0, &iv), kOfxStatErrBadHandle);
    c.expect("getString null set", gProp->propGetString(nullptr, kOfxPropName, 0, &sv), kOfxStatErrBadHandle);
    c.expect("setInt null set", gProp->propSetInt(nullptr, "X", 0, 1), kOfxStatErrBadHandle);
    c.expect("getDimension null set", gProp->propGetDimension(nullptr, kOfxPropName, &iv), kOfxStatErrBadHandle);
    c.expect("reset null set", gProp->propReset(nullptr, kOfxPropName), kOfxStatErrBadHandle);
    c.expect("getInt unknown", gProp->propGetInt(ep, "OfxNoSuchProperty", 0, &iv), kOfxStatErrUnknown);
    c.expect("getString unknown", gProp->propGetString(ep, "OfxNoSuchProperty", 0, &sv), kOfxStatErrUnknown);
    c.expect("getDouble unknown", gProp->propGetDouble(ep, "OfxNoSuchProperty", 0, &dv), kOfxStatErrUnknown);
    c.expect("getPointer unknown", gProp->propGetPointer(ep, "OfxNoSuchProperty", 0, &pv), kOfxStatErrUnknown);
    c.expect("getDimension unknown", gProp->propGetDimension(ep, "OfxNoSuchProperty", &iv), kOfxStatErrUnknown);
    c.expect("reset unknown", gProp->propReset(ep, "OfxNoSuchProperty"), kOfxStatErrUnknown);
    c.expect("getString index past end", gProp->propGetString(ep, kOfxPropType, 5, &sv), kOfxStatErrBadIndex);
    c.expect("getString negative index", gProp->propGetString(ep, kOfxPropType, -1, &sv), kOfxStatErrBadIndex);
    c.expect("getDouble index past end", gProp->propGetDouble(ep, kOfxImageEffectPropProjectSize, 2, &dv), kOfxStatErrBadIndex);
    c.expect("setDouble negative index", gProp->propSetDouble(ep, kOfxImageEffectPropProjectSize, -1, 1.0), kOfxStatErrBadIndex);
    c.expect("setString negative index", gProp->propSetString(ep, "OfxTestScratch", -1, "x"), kOfxStatErrBadIndex);
    c.expect("setIntN negative count", gProp->propSetIntN(ep, "OfxTestScratch", -1, &iv), kOfxStatErrValue);
    {
        int two[2] = {0, 0};
        c.expect("getIntN count past dimension", gProp->propGetIntN(ep, kOfxImageEffectPropProjectSize, 3, (int*)(void*)two), kOfxStatErrBadIndex);
    }
    // a wrong kind of handle where a property set is wanted
    c.expect("getInt on a param handle", gProp->propGetInt((PS)(void*)P(d, "Probe"), kOfxPropName, 0, &iv), kOfxStatErrBadHandle);
    c.expect("getInt on a clip handle", gProp->propGetInt((PS)(void*)d->src, kOfxPropName, 0, &iv), kOfxStatErrBadHandle);
    // parameters
    OfxParamHandle ph = nullptr;
    c.expect("paramGetHandle null set", gParam->paramGetHandle(nullptr, "Probe", &ph, nullptr), kOfxStatErrBadHandle);
    c.expect("paramGetHandle unknown", gParam->paramGetHandle(d->ps, "NoSuchParam", &ph, nullptr), kOfxStatErrUnknown);
    c.expect("paramGetValue null", gParam->paramGetValue(nullptr, &dv), kOfxStatErrBadHandle);
    c.expect("paramGetValueAtTime null", gParam->paramGetValueAtTime(nullptr, 0.0, &dv), kOfxStatErrBadHandle);
    c.expect("paramSetValue null", gParam->paramSetValue(nullptr, 1.0), kOfxStatErrBadHandle);
    c.expect("paramSetValueAtTime null", gParam->paramSetValueAtTime(nullptr, 0.0, 1.0), kOfxStatErrBadHandle);
    c.expect("paramGetValue on a clip handle", gParam->paramGetValue((OfxParamHandle)(void*)d->src, &dv), kOfxStatErrBadHandle);
    c.expect("paramGetPropertySet null", gParam->paramGetPropertySet(nullptr, (PS*)&pv), kOfxStatErrBadHandle);
    c.expect("paramSetGetPropertySet null", gParam->paramSetGetPropertySet(nullptr, (PS*)&pv), kOfxStatErrBadHandle);
    {
        unsigned int n = 99;
        double kt = 0;
        int idx = 123;
        c.expect("paramGetNumKeys null", gParam->paramGetNumKeys(nullptr, &n), kOfxStatErrBadHandle);
        c.expect("paramGetNumKeys", gParam->paramGetNumKeys(P(d, "Probe"), &n), kOfxStatOK);
        c.require("probe has no keys", n == 0);
        c.expect("paramGetKeyTime past end", gParam->paramGetKeyTime(P(d, "Probe"), 7, &kt), kOfxStatErrBadIndex);
        c.expect("paramGetKeyTime null", gParam->paramGetKeyTime(nullptr, 0, &kt), kOfxStatErrBadHandle);
        c.expect("paramDeleteKey no key", gParam->paramDeleteKey(P(d, "Probe"), 5.0), kOfxStatErrBadIndex);
        c.expect("paramDeleteKey null", gParam->paramDeleteKey(nullptr, 5.0), kOfxStatErrBadHandle);
        c.expect("paramDeleteAllKeys null", gParam->paramDeleteAllKeys(nullptr), kOfxStatErrBadHandle);
        c.expect("paramGetKeyIndex no key", gParam->paramGetKeyIndex(P(d, "Probe"), 5.0, 0, &idx), kOfxStatFailed);
        c.require("paramGetKeyIndex sets -1 when nothing was found", idx == -1);
        c.expect("paramGetKeyIndex null", gParam->paramGetKeyIndex(nullptr, 5.0, 0, &idx), kOfxStatErrBadHandle);
        c.expect("paramCopy null", gParam->paramCopy(nullptr, P(d, "Probe"), 0.0, nullptr), kOfxStatErrBadHandle);
        c.expect("paramEditBegin null", gParam->paramEditBegin(nullptr, "x"), kOfxStatErrBadHandle);
        c.expect("paramEditEnd null", gParam->paramEditEnd(nullptr), kOfxStatErrBadHandle);
        c.expect("paramDefine on an instance", gParam->paramDefine(d->ps, kOfxParamTypeDouble, "Late", nullptr), kOfxStatErrBadHandle);
    }
    // clips and images
    {
        OfxImageClipHandle ch = nullptr;
        PS ih = nullptr;
        OfxRectD rod;
        c.expectAny("clipGetHandle unknown name", gEff->clipGetHandle(d->eff, "NoSuchClip", &ch, nullptr), {kOfxStatErrBadHandle, kOfxStatErrUnknown});
        c.expect("clipGetHandle null effect", gEff->clipGetHandle(nullptr, "Source", &ch, nullptr), kOfxStatErrBadHandle);
        c.expect("clipGetPropertySet null", gEff->clipGetPropertySet(nullptr, &ih), kOfxStatErrBadHandle);
        c.expect("clipGetImage null clip", gEff->clipGetImage(nullptr, t, nullptr, &ih), kOfxStatErrBadHandle);
        c.expect("clipGetImage on a param handle", gEff->clipGetImage((OfxImageClipHandle)(void*)P(d, "Probe"), t, nullptr, &ih), kOfxStatErrBadHandle);
        c.expect("clipReleaseImage null", gEff->clipReleaseImage(nullptr), kOfxStatErrBadHandle);
        c.expect("clipReleaseImage on a non-image property set", gEff->clipReleaseImage(ep), kOfxStatErrBadHandle);
        c.expect("clipGetRegionOfDefinition null", gEff->clipGetRegionOfDefinition(nullptr, t, &rod), kOfxStatErrBadHandle);
        c.expect("clipGetRegionOfDefinition", gEff->clipGetRegionOfDefinition(d->src, t, &rod), kOfxStatOK);
        c.expect("getPropertySet null", gEff->getPropertySet(nullptr, &ih), kOfxStatErrBadHandle);
        c.expect("getParamSet null", gEff->getParamSet(nullptr, (OfxParamSetHandle*)&pv), kOfxStatErrBadHandle);
        c.require("abort on null effect is 0", gEff->abort(nullptr) == 0);
        // an image released twice
        Img im = getImg(d->src, t);
        c.expect("getImage", im.st, kOfxStatOK);
        if (im.h) {
            c.expect("release", gEff->clipReleaseImage(im.h), kOfxStatOK);
            c.expect("release again", gEff->clipReleaseImage(im.h), kOfxStatErrBadHandle);
        }
    }
    // image memory
    {
        OfxImageMemoryHandle mh = (OfxImageMemoryHandle)(void*)0x1;
        void* ptr = (void*)0x1;
        c.expect("imageMemoryAlloc huge", gEff->imageMemoryAlloc(d->eff, (size_t)-1 / 2, &mh), kOfxStatErrMemory);
        c.require("handle is NULL after a failed alloc", mh == nullptr);
        c.expect("imageMemoryAlloc null out", gEff->imageMemoryAlloc(d->eff, 16, nullptr), kOfxStatErrBadHandle);
        c.expect("imageMemoryLock null", gEff->imageMemoryLock(nullptr, &ptr), kOfxStatErrBadHandle);
        c.require("pointer is NULL after a failed lock", ptr == nullptr);
        c.expect("imageMemoryUnlock null", gEff->imageMemoryUnlock(nullptr), kOfxStatErrBadHandle);
        c.expect("imageMemoryFree null", gEff->imageMemoryFree(nullptr), kOfxStatErrBadHandle);
        OfxImageMemoryHandle ok = nullptr;
        c.expect("imageMemoryAlloc", gEff->imageMemoryAlloc(nullptr, 64, &ok), kOfxStatOK);       // the instance may be NULL
        if (ok) {
            void* p1 = nullptr;
            void* p2 = nullptr;
            c.expect("lock", gEff->imageMemoryLock(ok, &p1), kOfxStatOK);
            c.expect("lock nested", gEff->imageMemoryLock(ok, &p2), kOfxStatOK);
            c.require("memory is 16 byte aligned", p1 && ((uintptr_t)p1 % 16) == 0);
            c.require("nested locks give the same pointer", p1 == p2);
            if (p1) memset(p1, 0x5a, 64);
            c.expect("unlock", gEff->imageMemoryUnlock(ok), kOfxStatOK);
            c.expect("unlock", gEff->imageMemoryUnlock(ok), kOfxStatOK);
            c.expect("free", gEff->imageMemoryFree(ok), kOfxStatOK);
            c.expect("free twice", gEff->imageMemoryFree(ok), kOfxStatErrBadHandle);
            c.expect("lock after free", gEff->imageMemoryLock(ok, &p1), kOfxStatErrBadHandle);
        }
        void* m = nullptr;
        c.expect("memory alloc huge", gMem->memoryAlloc(nullptr, (size_t)-1 / 2, &m), kOfxStatErrMemory);
        c.require("memory alloc huge leaves NULL", m == nullptr);
        c.expectAny("memory free NULL", gMem->memoryFree(nullptr), {kOfxStatOK, kOfxStatErrBadHandle});
        int onStack = 0;
        c.expect("memory free of a pointer that is not the host's", gMem->memoryFree(&onStack), kOfxStatErrBadHandle);
        void* once = nullptr;
        c.expect("memory alloc", gMem->memoryAlloc(nullptr, 24, &once), kOfxStatOK);
        c.expect("memory free", gMem->memoryFree(once), kOfxStatOK);
        c.expect("memory free twice", gMem->memoryFree(once), kOfxStatErrBadHandle);
    }
    // threads and mutexes
    {
        unsigned int n = 0;
        OfxMutexHandle mh = nullptr;
        c.expect("multiThread NULL func", gMT->multiThread(nullptr, 2, nullptr), kOfxStatFailed);
        c.expect("multiThreadNumCPUs NULL", gMT->multiThreadNumCPUs(nullptr), kOfxStatFailed);
        c.expect("multiThreadIndex NULL", gMT->multiThreadIndex(nullptr), kOfxStatFailed);
        c.expect("multiThreadIndex", gMT->multiThreadIndex(&n), kOfxStatOK);
        c.require("index is 0 outside a spawned thread", n == 0);
        c.require("not a spawned thread", gMT->multiThreadIsSpawnedThread() == 0);
        c.expect("mutexLock NULL", gMT->mutexLock(nullptr), kOfxStatErrBadHandle);
        c.expect("mutexUnLock NULL", gMT->mutexUnLock(nullptr), kOfxStatErrBadHandle);
        c.expect("mutexTryLock NULL", gMT->mutexTryLock(nullptr), kOfxStatErrBadHandle);
        c.expect("mutexDestroy NULL", gMT->mutexDestroy(nullptr), kOfxStatErrBadHandle);
        c.expect("mutexCreate NULL", gMT->mutexCreate(nullptr, 0), kOfxStatFailed);
        c.expect("mutexCreate", gMT->mutexCreate(&mh, 0), kOfxStatOK);
        c.expect("tryLock", gMT->mutexTryLock(mh), kOfxStatOK);
        c.expect("unlock", gMT->mutexUnLock(mh), kOfxStatOK);
        c.expect("destroy", gMT->mutexDestroy(mh), kOfxStatOK);
    }
    // messages
    {
        c.expect("message NULL handle (allowed)", gMsg2->message(nullptr, kOfxMessageLog, nullptr, "hello %d", 1), kOfxStatOK);
        c.expect("setPersistentMessage NULL handle", gMsg2->setPersistentMessage(nullptr, kOfxMessageError, nullptr, "x"), kOfxStatErrBadHandle);
        c.expect("setPersistentMessage garbage handle", gMsg2->setPersistentMessage(ep, kOfxMessageError, nullptr, "x"), kOfxStatErrBadHandle);
        c.expect("clearPersistentMessage NULL handle", gMsg2->clearPersistentMessage(nullptr), kOfxStatErrBadHandle);
    }
    // the picture still has to come out, whatever the host said
    bump("badcallsFailures", c.bad);
    logf("badcalls: render checks bad=%d", c.bad);
    OfxStatus st = renderCopy(d, t, in);
    return c.bad ? kOfxStatFailed : st;
}

// ---- 17. leaky: never releases its images
static OfxStatus renderLeaky(Inst* d, double t, PS in)
{
    Img s = getImg(d->src, t), o = getImg(d->dst, t);          // (no release, on purpose)
    Img extra1 = getImg(d->src, t - 1), extra2 = getImg(d->src, t + 1);
    (void)extra1; (void)extra2;
    if (!s.ok() || !o.ok()) return kOfxStatFailed;
    return copyImage(s, o, renderWindow(in));
}

// ---- 13. failrender
static OfxStatus renderFailing(Inst* d, double t, PS in)
{
    const int mode = getI(d, "Mode", t);
    logf("failrender: mode=%d", mode);
    switch (mode) {
    case 0:
        if (gMsg2) gMsg2->setPersistentMessage(d->eff, kOfxMessageError, nullptr, "render broke on purpose");
        return kOfxStatFailed;
    case 1: return kOfxStatErrMemory;
    case 2: return kOfxStatReplyDefault;
    case 3: return kOfxStatErrFatal;
    default:
        if (gMsg2) gMsg2->clearPersistentMessage(d->eff);
        return renderCopy(d, t, in);
    }
}

static OfxStatus filterAction(Kind k, const char* who, const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); bump("load"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionUnload)) { bump("unload"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        switch (k) {
        case K_INVERT: describeBasics(e, "Test Invert", "Reno Test", "Inverts the colour (alpha minus colour).", true, true, {kOfxImageEffectContextFilter}); break;
        case K_TEMPORAL: describeBasics(e, "Test Temporal Average", "Reno Test", "Averages three frames.", true, true, {kOfxImageEffectContextFilter}, true); break;
        case K_FLOATONLY: describeBasics(e, "Test Float Only", "Reno Test", "Floating point pictures only.", false, true, {kOfxImageEffectContextFilter}); break;
        case K_BYTEONLY: describeBasics(e, "Test Byte Only", "Reno Test", "8-bit pictures only.", true, false, {kOfxImageEffectContextFilter}); break;
        case K_LEAKY: describeBasics(e, "Test Leaky", "Reno Test Awkward", "Never releases an image.", true, true, {kOfxImageEffectContextFilter}, true); break;
        case K_FAILRENDER: describeBasics(e, "Test Failing Render", "Reno Test Awkward", "Fails in render.", true, true, {kOfxImageEffectContextFilter}); break;
        case K_BADSUITE: describeBasics(e, "Test Bad Suites", "Reno Test Awkward", "Asks for suites that do not exist.", true, true, {kOfxImageEffectContextFilter}); break;
        case K_BADCALLS: describeBasics(e, "Test Bad Calls", "Reno Test Awkward", "Makes calls it should not.", true, true, {kOfxImageEffectContextFilter}); break;
        }
        if (k == K_BADSUITE) {
            // unknown suite, wrong versions of real suites, a missing name: each must come back NULL
            Checks c;
            c.require("unknown suite", suite("OfxNoSuchSuite", 1) == nullptr);
            c.require("property suite v2", suite(kOfxPropertySuite, 2) == nullptr);
            c.require("property suite v0", suite(kOfxPropertySuite, 0) == nullptr);
            c.require("image effect suite v99", suite(kOfxImageEffectSuite, 99) == nullptr);
            c.require("parameter suite v0", suite(kOfxParameterSuite, 0) == nullptr);
            c.require("parameter suite v-1", suite(kOfxParameterSuite, -1) == nullptr);
            c.require("message suite v3", suite(kOfxMessageSuite, 3) == nullptr);
            c.require("memory suite v2", suite(kOfxMemorySuite, 2) == nullptr);
            c.require("multithread suite v2", suite(kOfxMultiThreadSuite, 2) == nullptr);
            c.require("NULL suite name", gHost->fetchSuite(gHost->host, nullptr, 1) == nullptr);
            c.require("empty suite name", suite("", 1) == nullptr);
            c.require("lower case suite name", suite("ofxpropertysuite", 1) == nullptr);
            c.require("VEGAS keyframe suite v7", suite(V_KEYFRAME_SUITE, 7) == nullptr);
            c.require("real suite still there", suite(kOfxPropertySuite, 1) != nullptr);
            logf("badsuite: describe bad=%d", c.bad);
            bump("badsuiteFailures", c.bad);
            if (c.bad) return kOfxStatFailed;
        }
        if (k == K_BADCALLS) badCallsDescribeChecks(e);
        if (k == K_INVERT || k == K_TEMPORAL) ss(effectProps(e), kOfxImageEffectPropClipPreferencesSlaveParam, 0, "UseFloat");
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName, false, k == K_TEMPORAL || k == K_LEAKY);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        if (k == K_INVERT) {
            PS pp = defineParam(ps, kOfxParamTypeBoolean, "ZeroRed", "Zero red");
            si(pp, kOfxParamPropDefault, 0, 0);
        }
        if (k == K_INVERT || k == K_TEMPORAL) {
            // asks for floating point pictures through getClipPreferences when on
            PS pp = defineParam(ps, kOfxParamTypeBoolean, "UseFloat", "Use float");
            si(pp, kOfxParamPropDefault, 0, 0);
        }
        if (k == K_FAILRENDER) {
            PS pp = defineParam(ps, kOfxParamTypeChoice, "Mode", "Mode");
            const char* modes[] = {"Failed", "Out of memory", "Default reply", "Fatal", "Fine"};
            for (int i = 0; i < 5; ++i) ss(pp, kOfxParamPropChoiceOption, i, modes[i]);
            si(pp, kOfxParamPropDefault, 0, 0);
        }
        if (k == K_BADCALLS) {
            PS pp = defineParam(ps, kOfxParamTypeDouble, "Probe", "Probe");
            sd(pp, kOfxParamPropDefault, 0, 0.5);
        }
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {"ZeroRed", "Mode", "Probe", "UseFloat"};
        if (k == K_BADSUITE) {
            // it works with the suites it got; the ones it asked for in error stay NULL
            if (suite("OfxNoSuchSuite", 1) != nullptr) return kOfxStatFailed;
        }
        return makeInstance(e, p, 4);
    }
    if (!strcmp(action, kOfxImageEffectActionGetClipPreferences) && (k == K_INVERT || k == K_TEMPORAL)) {
        Inst* d = inst(e);
        if (!d) return kOfxStatFailed;
        int useFloat = 0;
        gParam->paramGetValue(P(d, "UseFloat"), &useFloat);
        bump("getClipPreferences");
        if (!useFloat) return kOfxStatReplyDefault;
        ss(out, "OfxImageClipPropDepth_Output", 0, kOfxBitDepthFloat);
        ss(out, "OfxImageClipPropDepth_Source", 0, kOfxBitDepthFloat);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, who);
    if (!strcmp(action, kOfxImageEffectActionBeginSequenceRender)) { bump("beginSequence"); return kOfxStatOK; }
    if (!strcmp(action, kOfxImageEffectActionEndSequenceRender)) { bump("endSequence"); return kOfxStatOK; }
    if (!strcmp(action, kOfxImageEffectActionGetFramesNeeded) && (k == K_TEMPORAL || k == K_LEAKY)) {
        double t = gd(in, kOfxPropTime);
        double r[2] = {t - 1, t + 1};
        gProp->propSetDoubleN(out, "OfxImageClipPropFrameRange_Source", 2, r);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        if (!d) return kOfxStatFailed;
        double t = gd(in, kOfxPropTime);
        ++d->renderCount;
        bump("render");
        switch (k) {
        case K_INVERT: return slowRender(d) ? renderInvert(d, t, in) : kOfxStatFailed;
        case K_TEMPORAL: return renderTemporal(d, t, in);
        case K_FLOATONLY: return renderFloatOnly(d, t, in);
        case K_BYTEONLY: return renderByteOnly(d, t, in);
        case K_LEAKY: return renderLeaky(d, t, in);
        case K_FAILRENDER: return renderFailing(d, t, in);
        case K_BADSUITE: return renderCopy(d, t, in);
        case K_BADCALLS: return renderBadCalls(d, t, in);
        }
    }
    return kOfxStatReplyDefault;
}

#define FILTER_ENTRY(NAME, KIND, WHO) \
    static OfxStatus NAME(const char* a, const void* h, OfxPropertySetHandle i, OfxPropertySetHandle o) { return filterAction(KIND, WHO, a, h, i, o); }
FILTER_ENTRY(entryInvert, K_INVERT, "invert")
FILTER_ENTRY(entryTemporal, K_TEMPORAL, "temporal")
FILTER_ENTRY(entryFloatOnly, K_FLOATONLY, "floatonly")
FILTER_ENTRY(entryByteOnly, K_BYTEONLY, "byteonly")
FILTER_ENTRY(entryLeaky, K_LEAKY, "leaky")
FILTER_ENTRY(entryFailRender, K_FAILRENDER, "failrender")
FILTER_ENTRY(entryBadSuite, K_BADSUITE, "badsuite")
FILTER_ENTRY(entryBadCalls, K_BADCALLS, "badcalls")

// ====================================================================== 4. params: one parameter of every type
static const char* kAllParams[] = {"p_int", "p_dbl", "p_bool", "p_choice", "p_rgb", "p_rgba", "p_d2", "p_i2", "p_d3", "p_i3",
                                   "p_str", "p_custom", "p_group", "p_page", "p_button", "driver", "slave", "dep"};

static OfxStatus customInterp(OfxParamSetHandle, PS, PS) { return kOfxStatReplyDefault; }

static void describeParams(OfxImageEffectHandle e)
{
    OfxParamSetHandle ps = nullptr;
    gEff->getParamSet(e, &ps);
    PS pp;
    pp = defineParam(ps, kOfxParamTypeGroup, "p_group", "A Group");
    ss(pp, kOfxParamPropHint, 0, "Holds the first two");
    si(pp, kOfxParamPropGroupOpen, 0, 0);

    pp = defineParam(ps, kOfxParamTypeInteger, "p_int", "Integer Value");
    ss(pp, kOfxPropShortLabel, 0, "Int");
    ss(pp, kOfxPropLongLabel, 0, "The integer value");
    ss(pp, kOfxParamPropHint, 0, "An integer from 0 to 100");
    ss(pp, kOfxParamPropParent, 0, "p_group");
    si(pp, kOfxParamPropDefault, 0, 5);
    si(pp, kOfxParamPropMin, 0, 0);
    si(pp, kOfxParamPropMax, 0, 100);
    si(pp, kOfxParamPropDisplayMin, 0, 10);
    si(pp, kOfxParamPropDisplayMax, 0, 90);

    pp = defineParam(ps, kOfxParamTypeDouble, "p_dbl", "Double Value");
    ss(pp, kOfxParamPropHint, 0, "A double from -10 to 10");
    ss(pp, kOfxParamPropParent, 0, "p_group");
    sd(pp, kOfxParamPropDefault, 0, 0.25);
    sd(pp, kOfxParamPropMin, 0, -10.0);
    sd(pp, kOfxParamPropMax, 0, 10.0);
    sd(pp, kOfxParamPropDisplayMin, 0, -5.0);
    sd(pp, kOfxParamPropDisplayMax, 0, 5.0);
    sd(pp, kOfxParamPropIncrement, 0, 0.01);
    si(pp, kOfxParamPropDigits, 0, 3);

    pp = defineParam(ps, kOfxParamTypeBoolean, "p_bool", "A Flag");
    ss(pp, kOfxParamPropHint, 0, "On or off");
    si(pp, kOfxParamPropDefault, 0, 1);

    pp = defineParam(ps, kOfxParamTypeChoice, "p_choice", "A Choice");
    const char* opts[] = {"Zero", "One", "Two", "Three"};
    for (int i = 0; i < 4; ++i) ss(pp, kOfxParamPropChoiceOption, i, opts[i]);
    si(pp, kOfxParamPropDefault, 0, 1);

    pp = defineParam(ps, kOfxParamTypeRGB, "p_rgb", "Colour");
    sd(pp, kOfxParamPropDefault, 0, 0.1); sd(pp, kOfxParamPropDefault, 1, 0.2); sd(pp, kOfxParamPropDefault, 2, 0.3);

    pp = defineParam(ps, kOfxParamTypeRGBA, "p_rgba", "Colour With Alpha");
    sd(pp, kOfxParamPropDefault, 0, 0.4); sd(pp, kOfxParamPropDefault, 1, 0.5); sd(pp, kOfxParamPropDefault, 2, 0.6); sd(pp, kOfxParamPropDefault, 3, 0.7);

    pp = defineParam(ps, kOfxParamTypeDouble2D, "p_d2", "Point");
    sd(pp, kOfxParamPropDefault, 0, 1.5); sd(pp, kOfxParamPropDefault, 1, 2.5);
    sd(pp, kOfxParamPropMin, 0, -100.0); sd(pp, kOfxParamPropMin, 1, -100.0);
    sd(pp, kOfxParamPropMax, 0, 100.0); sd(pp, kOfxParamPropMax, 1, 100.0);

    pp = defineParam(ps, kOfxParamTypeInteger2D, "p_i2", "Size");
    si(pp, kOfxParamPropDefault, 0, 3); si(pp, kOfxParamPropDefault, 1, 4);
    si(pp, kOfxParamPropMin, 0, 0); si(pp, kOfxParamPropMin, 1, 0);
    si(pp, kOfxParamPropMax, 0, 50); si(pp, kOfxParamPropMax, 1, 60);

    pp = defineParam(ps, kOfxParamTypeDouble3D, "p_d3", "Vector");
    sd(pp, kOfxParamPropDefault, 0, 1.0); sd(pp, kOfxParamPropDefault, 1, 2.0); sd(pp, kOfxParamPropDefault, 2, 3.0);

    pp = defineParam(ps, kOfxParamTypeInteger3D, "p_i3", "Cell");
    si(pp, kOfxParamPropDefault, 0, 7); si(pp, kOfxParamPropDefault, 1, 8); si(pp, kOfxParamPropDefault, 2, 9);

    pp = defineParam(ps, kOfxParamTypeString, "p_str", "Text");
    ss(pp, kOfxParamPropDefault, 0, "hello");
    ss(pp, kOfxParamPropStringMode, 0, kOfxParamStringIsMultiLine);
    ss(pp, kOfxParamPropHint, 0, "Some text");

    pp = defineParam(ps, kOfxParamTypeCustom, "p_custom", "Custom");
    ss(pp, kOfxParamPropDefault, 0, "cust");
    sp(pp, kOfxParamPropCustomInterpCallbackV1, 0, (void*)customInterp);

    pp = defineParam(ps, kOfxParamTypePushButton, "p_button", "Do It");
    ss(pp, kOfxParamPropHint, 0, "Pushes");

    pp = defineParam(ps, kOfxParamTypeInteger, "driver", "Driver");
    si(pp, kOfxParamPropDefault, 0, 0);
    si(pp, kOfxParamPropMin, 0, 0);
    si(pp, kOfxParamPropMax, 0, 1000);

    pp = defineParam(ps, kOfxParamTypeDouble, "slave", "Slave");
    sd(pp, kOfxParamPropDefault, 0, 0.0);

    pp = defineParam(ps, kOfxParamTypeDouble, "dep", "Dependent");
    sd(pp, kOfxParamPropDefault, 0, 1.0);

    pp = defineParam(ps, kOfxParamTypePage, "p_page", "Main");
    const char* kids[] = {"p_group", "p_int", "p_dbl", "p_bool", "p_choice", "p_rgb", "p_rgba", "p_d2", "p_i2", "p_d3", "p_i3",
                          "p_str", "p_custom", "p_button", "driver", "slave", "dep"};
    for (int i = 0; i < 17; ++i) ss(pp, kOfxParamPropPageChild, i, kids[i]);
}

static std::string fmtVals(Inst* d, const char* name, bool now, double t)
{
    // "name=v1,v2" with every value at full precision; ints print as ints
    OfxParamHandle h = P(d, name);
    PS pr = nullptr;
    gParam->paramGetPropertySet(h, &pr);
    const char* type = gs(pr, kOfxParamPropType);
    char buf[256];
    double v[4] = {0, 0, 0, 0};
    int iv[3] = {0, 0, 0};
    auto get = [&](auto*... out) { return now ? gParam->paramGetValue(h, out...) : gParam->paramGetValueAtTime(h, t, out...); };
    if (!strcmp(type, kOfxParamTypeInteger) || !strcmp(type, kOfxParamTypeBoolean) || !strcmp(type, kOfxParamTypeChoice)) {
        get(&iv[0]);
        snprintf(buf, sizeof buf, "%s=%d", name, iv[0]);
    } else if (!strcmp(type, kOfxParamTypeInteger2D)) {
        get(&iv[0], &iv[1]);
        snprintf(buf, sizeof buf, "%s=%d,%d", name, iv[0], iv[1]);
    } else if (!strcmp(type, kOfxParamTypeInteger3D)) {
        get(&iv[0], &iv[1], &iv[2]);
        snprintf(buf, sizeof buf, "%s=%d,%d,%d", name, iv[0], iv[1], iv[2]);
    } else if (!strcmp(type, kOfxParamTypeDouble)) {
        get(&v[0]);
        snprintf(buf, sizeof buf, "%s=%.17g", name, v[0]);
    } else if (!strcmp(type, kOfxParamTypeDouble2D)) {
        get(&v[0], &v[1]);
        snprintf(buf, sizeof buf, "%s=%.17g,%.17g", name, v[0], v[1]);
    } else if (!strcmp(type, kOfxParamTypeDouble3D) || !strcmp(type, kOfxParamTypeRGB)) {
        get(&v[0], &v[1], &v[2]);
        snprintf(buf, sizeof buf, "%s=%.17g,%.17g,%.17g", name, v[0], v[1], v[2]);
    } else if (!strcmp(type, kOfxParamTypeRGBA)) {
        get(&v[0], &v[1], &v[2], &v[3]);
        snprintf(buf, sizeof buf, "%s=%.17g,%.17g,%.17g,%.17g", name, v[0], v[1], v[2], v[3]);
    } else if (!strcmp(type, kOfxParamTypeString) || !strcmp(type, kOfxParamTypeCustom)) {
        char* s = nullptr;
        get(&s);
        snprintf(buf, sizeof buf, "%s=<%s>", name, s ? s : "(null)");
    } else {
        snprintf(buf, sizeof buf, "%s=?", name);
    }
    return buf;
}

static OfxStatus renderParams(Inst* d, double t, PS in)
{
    static const char* valued[] = {"p_int", "p_dbl", "p_bool", "p_choice", "p_rgb", "p_rgba", "p_d2", "p_i2", "p_d3", "p_i3", "p_str", "p_custom",
                                   "driver", "slave", "dep"};
    std::string at, now;
    for (const char* n : valued) {
        at += fmtVals(d, n, false, t) + " ";
        now += fmtVals(d, n, true, t) + " ";
    }
    // keyframe queries on p_dbl
    char buf[512];
    unsigned int nk = 0;
    gParam->paramGetNumKeys(P(d, "p_dbl"), &nk);
    std::string keys = "keys=" + std::to_string(nk) + ":";
    for (unsigned int k = 0; k < nk; ++k) {
        double kt = -1;
        gParam->paramGetKeyTime(P(d, "p_dbl"), k, &kt);
        snprintf(buf, sizeof buf, "%s%.17g", k ? "," : "", kt);
        keys += buf;
    }
    int idx[3];
    OfxStatus ks[3];
    for (int k = 0; k < 3; ++k) {
        idx[k] = 12345;
        ks[k] = gParam->paramGetKeyIndex(P(d, "p_dbl"), t, k - 1, &idx[k]);      // before, at, after
    }
    snprintf(buf, sizeof buf, " keyindex=prev:%d/%d,at:%d/%d,next:%d/%d", ks[0], idx[0], ks[1], idx[1], ks[2], idx[2]);
    keys += buf;
    std::string all = "AT " + at + keys + " | NOW " + now;
    setDump("params", all);
    logf("params: render t=%g", t);

    Img o = getImg(d->dst, t);
    if (!o.ok()) { releaseImg(o); return kOfxStatFailed; }
    double rgb[3], rgba[4];
    int iv = 0;
    gParam->paramGetValueAtTime(P(d, "p_rgb"), t, &rgb[0], &rgb[1], &rgb[2]);
    gParam->paramGetValueAtTime(P(d, "p_rgba"), t, &rgba[0], &rgba[1], &rgba[2], &rgba[3]);
    gParam->paramGetValueAtTime(P(d, "p_int"), t, &iv);
    Win w = renderWindow(in);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            uint8_t* b = o.px(x, y);
            if (!o.isFloat) {
                b[o.ri()] = clampByte((int)(rgb[0] * 255.0 + 0.5));
                b[1] = clampByte((int)(rgb[1] * 255.0 + 0.5));
                b[o.bi()] = clampByte(iv);
                b[3] = clampByte((int)(rgba[3] * 255.0 + 0.5));
            } else {
                F(b)[0] = (float)rgb[0]; F(b)[1] = (float)rgb[1]; F(b)[2] = (float)iv / 255.f; F(b)[3] = (float)rgba[3];
            }
        }
    releaseImg(o);
    return kOfxStatOK;
}

static OfxStatus paramsInstanceChanged(Inst* d, PS in)
{
    const char* type = gs(in, kOfxPropType);
    const char* name = gs(in, kOfxPropName);
    const char* reason = gs(in, kOfxPropChangeReason);
    double t = gd(in, kOfxPropTime);
    logf("params: changed type=%s name=%s reason=%s time=%g", type, name, reason, t);
    if (strcmp(type, kOfxTypeParameter)) return kOfxStatOK;
    if (!strcmp(name, "driver")) {
        int v = 0;
        gParam->paramGetValue(P(d, "driver"), &v);                   // allowed in instanceChanged
        gParam->paramSetValue(P(d, "slave"), v * 0.5);
        PS dep = nullptr;
        gParam->paramGetPropertySet(P(d, "dep"), &dep);
        si(dep, kOfxParamPropEnabled, 0, v != 0 ? 1 : 0);
        si(dep, kOfxParamPropSecret, 0, v > 10 ? 1 : 0);
        PS ch = nullptr;
        gParam->paramGetPropertySet(P(d, "p_choice"), &ch);
        gProp->propReset(ch, kOfxParamPropChoiceOption);
        const int n = 2 + (v & 1);
        for (int i = 0; i < n; ++i) {
            char lab[64];
            snprintf(lab, sizeof lab, "Opt%d_%d", v, i);
            ss(ch, kOfxParamPropChoiceOption, i, lab);
        }
    } else if (!strcmp(name, "p_button")) {
        ++d->pushCount;
        char s[64];
        snprintf(s, sizeof s, "pushed %d", d->pushCount);
        gParam->paramSetValue(P(d, "p_str"), s);
    }
    return kOfxStatOK;
}

static OfxStatus entryParams(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Parameters", "Reno Test", "One parameter of every type.", true, true, {kOfxImageEffectContextFilter});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        describeParams(e);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) return makeInstance(e, kAllParams, 18);
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "params");
    if (!strcmp(action, kOfxActionSyncPrivateData)) { bump("sync"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionBeginInstanceChanged)) { logf("params: beginChanged reason=%s", gs(in, kOfxPropChangeReason)); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionEndInstanceChanged)) { logf("params: endChanged reason=%s", gs(in, kOfxPropChangeReason)); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionBeginInstanceEdit)) { logf("params: beginEdit"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionEndInstanceEdit)) { logf("params: endEdit"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionInstanceChanged)) {
        Inst* d = inst(e);
        return d ? paramsInstanceChanged(d, in) : kOfxStatFailed;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        return d ? renderParams(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 5. generator
static OfxStatus renderGenerator(Inst* d, double t, PS in)
{
    PS ep = effectProps(d->eff);
    const int pw = (int)std::lround(gd(ep, kOfxImageEffectPropProjectSize, 0));
    const int ph = (int)std::lround(gd(ep, kOfxImageEffectPropProjectSize, 1));
    Img o = getImg(d->dst, t);
    if (!o.ok()) { releaseImg(o); return kOfxStatFailed; }
    Win w = renderWindow(in);
    logf("generator: t=%g project=%dx%d window=%d,%d,%d,%d bounds=%d,%d,%d,%d", t, pw, ph, w.x1, w.y1, w.x2, w.y2, o.x1, o.y1, o.x2, o.y2);
    const int frame = (int)std::floor(t);
    const int bx = (frame * 4) % pw;
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            int r = frame & 255, g = y & 255, b = x & 255;
            if (x == pw - 1 || y == ph - 1) r = g = b = 255;                 // a white border on the last column and row
            if (x >= bx && x < bx + 8 && y < 8) r = g = b = 0;               // a black block that moves along the bottom
            uint8_t* px = o.px(x, y);
            if (!o.isFloat) {
                px[o.ri()] = (uint8_t)r; px[1] = (uint8_t)g; px[o.bi()] = (uint8_t)b; px[3] = 255;
            } else {
                F(px)[0] = r / 255.f; F(px)[1] = g / 255.f; F(px)[2] = b / 255.f; F(px)[3] = 1.f;
            }
        }
    releaseImg(o);
    return kOfxStatOK;
}

static OfxStatus entryGenerator(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Generator", "Reno Test", "A pattern that depends on time and size.", true, true, {kOfxImageEffectContextGenerator});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        if (strcmp(gs(in, kOfxImageEffectPropContext), kOfxImageEffectContextGenerator)) return kOfxStatErrMissingHostFeature;
        defineClip(e, kOfxImageEffectOutputClipName);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        Inst* d = new Inst();
        d->eff = e;
        gEff->getParamSet(e, &d->ps);
        if (gEff->clipGetHandle(e, kOfxImageEffectOutputClipName, &d->dst, nullptr) != kOfxStatOK) { delete d; return kOfxStatFailed; }
        // a generator has no Source clip at all
        OfxImageClipHandle none = nullptr;
        if (gEff->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &none, nullptr) == kOfxStatOK) { delete d; return kOfxStatFailed; }
        sp(effectProps(e), kOfxPropInstanceData, 0, d);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "generator");
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        return d ? renderGenerator(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 6. transition
static OfxStatus renderTransition(Inst* d, double t, PS in)
{
    OfxImageClipHandle fromClip = nullptr, toClip = nullptr;
    gEff->clipGetHandle(d->eff, kOfxImageEffectTransitionSourceFromClipName, &fromClip, nullptr);
    gEff->clipGetHandle(d->eff, kOfxImageEffectTransitionSourceToClipName, &toClip, nullptr);
    Img a = getImg(fromClip, t), b = getImg(toClip, t), o = getImg(d->dst, t);
    if (!a.ok() || !b.ok() || !o.ok()) { releaseImg(a); releaseImg(b); releaseImg(o); return kOfxStatFailed; }
    const float k = (float)getD(d, kOfxImageEffectTransitionParamName, t);
    logf("transition: t=%g amount=%.9g", t, (double)k);
    Win w = renderWindow(in);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            uint8_t* pa = a.px(x, y);
            uint8_t* pb = b.px(x, y);
            uint8_t* po = o.px(x, y);
            for (int c = 0; c < 4; ++c) {
                if (!o.isFloat) po[c] = (uint8_t)(pa[c] * (1.f - k) + pb[c] * k + 0.5f);
                else F(po)[c] = F(pa)[c] * (1.f - k) + F(pb)[c] * k;
            }
        }
    releaseImg(a); releaseImg(b); releaseImg(o);
    return kOfxStatOK;
}

static OfxStatus entryTransition(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Dissolve", "Reno Test", "A cross dissolve.", true, true, {kOfxImageEffectContextTransition});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectTransitionSourceFromClipName);
        defineClip(e, kOfxImageEffectTransitionSourceToClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        PS pp = defineParam(ps, kOfxParamTypeDouble, kOfxImageEffectTransitionParamName, "Transition");
        sd(pp, kOfxParamPropDefault, 0, 0.0);
        sd(pp, kOfxParamPropMin, 0, 0.0);
        sd(pp, kOfxParamPropMax, 0, 1.0);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {kOfxImageEffectTransitionParamName};
        return makeInstance(e, p, 1);
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "transition");
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        return d ? renderTransition(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 7. general context with an optional second input
static OfxStatus renderGeneral(Inst* d, double t, PS in)
{
    OfxImageClipHandle second = nullptr;
    PS cp = nullptr;
    gEff->clipGetHandle(d->eff, "Second", &second, &cp);
    const int connected = gi(cp, kOfxImageClipPropConnected);
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    Img s2 = getImg(second, t);
    logf("general: second connected=%d imageStatus=%d", connected, s2.st);
    if (!s.ok() || !o.ok() || (connected && !s2.ok()) || (!connected && s2.st == kOfxStatOK)) {
        releaseImg(s); releaseImg(s2); releaseImg(o);
        return kOfxStatFailed;
    }
    Win w = renderWindow(in);
    for (int y = w.y1; y < w.y2; ++y)
        for (int x = w.x1; x < w.x2; ++x) {
            uint8_t* a = s.px(x, y);
            uint8_t* po = o.px(x, y);
            for (int c = 0; c < 4; ++c) {
                if (!o.isFloat) po[c] = connected ? clampByte(a[c] + s2.px(x, y)[c]) : (uint8_t)(a[c] / 2);
                else F(po)[c] = connected ? F(a)[c] + F(s2.px(x, y))[c] : F(a)[c] * 0.5f;
            }
        }
    releaseImg(s); releaseImg(s2); releaseImg(o);
    return kOfxStatOK;
}

static OfxStatus entryGeneral(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test General", "Reno Test", "General context with an optional second input.", true, true, {kOfxImageEffectContextGeneral});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, "Second", true);
        defineClip(e, kOfxImageEffectOutputClipName);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) return makeInstance(e, nullptr, 0);
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "general");
    if (!strcmp(action, kOfxActionInstanceChanged)) {
        logf("general: changed type=%s name=%s reason=%s", gs(in, kOfxPropType), gs(in, kOfxPropName), gs(in, kOfxPropChangeReason));
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        return d ? renderGeneral(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 8. misc: identity, clip preferences, memory, threads, messages
struct MtArg {
    Img src, dst;
    uint8_t* tmp;
    int tmpRowBytes;
    Win w;
    double amount;
    OfxMutexHandle mutex;
    int rowsDone = 0;                 // under the mutex
    unsigned int seenMask = 0;        // under the mutex
    std::atomic<int> badIndex{0};
    std::atomic<int> nestedStatus{0};
    std::atomic<int> notSpawned{0};
};

static void mtRows(unsigned int idx, unsigned int n, void* arg)
{
    MtArg* a = (MtArg*)arg;
    unsigned int reported = 99;
    if (gMT->multiThreadIndex(&reported) != kOfxStatOK || reported != idx) a->badIndex++;
    if (n > 1 && idx > 0 && gMT->multiThreadIsSpawnedThread() != 1) a->notSpawned++;
    if (idx == 0) {
        // calling multiThread from inside is an error
        a->nestedStatus = gMT->multiThread(mtRows, 2, arg);
    }
    gMT->mutexLock(a->mutex);
    a->seenMask |= 1u << idx;         // (noted even for a thread that gets no row when there are more threads than rows)
    gMT->mutexUnLock(a->mutex);
    for (int y = a->w.y1 + (int)idx; y < a->w.y2; y += (int)n) {
        uint8_t* s = a->src.px(a->w.x1, y);
        uint8_t* t = a->tmp + (size_t)(y - a->w.y1) * (size_t)a->tmpRowBytes;
        const int cnt = a->w.x2 - a->w.x1;
        if (!a->src.isFloat) {
            for (int x = 0; x < cnt; ++x)
                for (int c = 0; c < 4; ++c) t[x * 4 + c] = c == 3 ? s[x * 4 + 3] : clampByte((int)(s[x * 4 + c] * a->amount + 0.5));
        } else {
            for (int x = 0; x < cnt; ++x)
                for (int c = 0; c < 4; ++c) ((float*)t)[x * 4 + c] = c == 3 ? ((float*)s)[x * 4 + 3] : (float)(((float*)s)[x * 4 + c] * a->amount);
        }
        gMT->mutexLock(a->mutex);
        a->rowsDone++;
        gMT->mutexUnLock(a->mutex);
    }
}

static OfxStatus renderMisc(Inst* d, double t, PS in)
{
    Checks c;
    const double amount = getD(d, "Amount", t);
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    if (!s.ok() || !o.ok()) { releaseImg(s); releaseImg(o); return kOfxStatFailed; }
    PS ep = effectProps(d->eff);
    Win w = renderWindow(in);

    // memory suite
    void* m = nullptr;
    c.expect("memoryAlloc", gMem->memoryAlloc(d->eff, 1000, &m), kOfxStatOK);
    if (m) { memset(m, 0xab, 1000); c.expect("memoryFree", gMem->memoryFree(m), kOfxStatOK); }

    // image memory: a scratch copy of the window
    const int pb = s.isFloat ? 16 : 4;
    const int rowBytes = (w.x2 - w.x1) * pb;
    OfxImageMemoryHandle mh = nullptr;
    void* tmp = nullptr;
    c.expect("imageMemoryAlloc", gEff->imageMemoryAlloc(d->eff, (size_t)rowBytes * (size_t)(w.y2 - w.y1), &mh), kOfxStatOK);
    c.expect("imageMemoryLock", gEff->imageMemoryLock(mh, &tmp), kOfxStatOK);

    // multi-thread suite
    unsigned int cpus = 0;
    c.expect("multiThreadNumCPUs", gMT->multiThreadNumCPUs(&cpus), kOfxStatOK);
    c.require("at least one cpu", cpus >= 1);
    MtArg arg;
    arg.src = s; arg.dst = o; arg.tmp = (uint8_t*)tmp; arg.tmpRowBytes = rowBytes; arg.w = w; arg.amount = amount;
    c.expect("mutexCreate", gMT->mutexCreate(&arg.mutex, 0), kOfxStatOK);
    const unsigned int nThreads = 4;
    if (tmp) c.expect("multiThread", gMT->multiThread(mtRows, nThreads, &arg), kOfxStatOK);
    c.require("every row was done once", arg.rowsDone == (w.y2 - w.y1));
    c.require("every thread ran", arg.seenMask == (1u << nThreads) - 1);
    c.require("threads saw their own index", arg.badIndex == 0);
    c.require("spawned threads know it", arg.notSpawned == 0);
    c.require("multiThread inside multiThread is refused", arg.nestedStatus == kOfxStatErrExists);
    c.require("not spawned afterwards", gMT->multiThreadIsSpawnedThread() == 0);
    c.expect("mutexDestroy", gMT->mutexDestroy(arg.mutex), kOfxStatOK);
    if (tmp)
        for (int y = w.y1; y < w.y2; ++y) memcpy(o.px(w.x1, y), (uint8_t*)tmp + (size_t)(y - w.y1) * (size_t)rowBytes, (size_t)rowBytes);
    c.expect("imageMemoryUnlock", gEff->imageMemoryUnlock(mh), kOfxStatOK);
    c.expect("imageMemoryFree", gEff->imageMemoryFree(mh), kOfxStatOK);

    // message suite
    c.expect("message log", gMsg2->message(d->eff, kOfxMessageLog, "test.log", "rendering %d x %d", w.x2 - w.x1, w.y2 - w.y1), kOfxStatOK);
    c.expect("message message", gMsg2->message(d->eff, kOfxMessageMessage, nullptr, "hello"), kOfxStatOK);
    c.expect("message warning", gMsg2->message(d->eff, kOfxMessageWarning, nullptr, "careful"), kOfxStatOK);
    c.expectAny("message question", gMsg2->message(d->eff, kOfxMessageQuestion, nullptr, "really?"), {kOfxStatReplyYes, kOfxStatReplyNo, kOfxStatFailed});
    c.expect("message v1", gMsg1->message(d->eff, kOfxMessageLog, nullptr, "via v1"), kOfxStatOK);
    c.expect("setPersistentMessage", gMsg2->setPersistentMessage(d->eff, kOfxMessageWarning, "test.persist", "kept %d", 7), kOfxStatOK);
    c.expect("clearPersistentMessage", gMsg2->clearPersistentMessage(d->eff), kOfxStatOK);

    // properties the effect expects on the picture and the instance
    c.require("pixel aspect 1", gd(s.h, kOfxImagePropPixelAspectRatio) == 1.0);
    c.require("render scale 1", gd(in, kOfxImageEffectPropRenderScale, 0) == 1.0 && gd(in, kOfxImageEffectPropRenderScale, 1) == 1.0);
    c.require("image is RGBA", !strcmp(gs(s.h, kOfxImageEffectPropComponents), kOfxImageComponentRGBA));
    c.require("image is premultiplied", !strcmp(gs(s.h, kOfxImageEffectPropPreMultiplication), kOfxImagePreMultiplied));
    c.require("field none", !strcmp(gs(in, kOfxImageEffectPropFieldToRender), kOfxImageFieldNone));
    c.require("not aborted", gEff->abort(d->eff) == 0);
    c.require("instance is not interactive during a plain render", gi(ep, kOfxPropIsInteractive, 0, -1) != -1);
    logf("misc: render t=%g amount=%g depth=%s checks bad=%d rows=%d mask=%u", t, amount, s.isFloat ? "float" : "byte", c.bad, arg.rowsDone, arg.seenMask);
    bump("miscCheckFailures", c.bad);
    releaseImg(s);
    releaseImg(o);
    return c.bad ? kOfxStatFailed : kOfxStatOK;
}

static OfxStatus entryMisc(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Misc", "Reno Test", "Identity, clip preferences, memory, threads, messages.", true, true, {kOfxImageEffectContextFilter});
        ss(effectProps(e), kOfxImageEffectPropClipPreferencesSlaveParam, 0, "Mode");
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        PS pp = defineParam(ps, kOfxParamTypeDouble, "Amount", "Amount");
        sd(pp, kOfxParamPropDefault, 0, 0.5);
        sd(pp, kOfxParamPropMin, 0, 0.0);
        sd(pp, kOfxParamPropMax, 0, 2.0);
        pp = defineParam(ps, kOfxParamTypeInteger, "Offset", "Offset");
        si(pp, kOfxParamPropDefault, 0, 0);
        pp = defineParam(ps, kOfxParamTypeChoice, "Mode", "Mode");
        ss(pp, kOfxParamPropChoiceOption, 0, "8 bit");
        ss(pp, kOfxParamPropChoiceOption, 1, "Floating point");
        si(pp, kOfxParamPropDefault, 0, 0);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {"Amount", "Offset", "Mode"};
        return makeInstance(e, p, 3);
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "misc");
    if (!strcmp(action, kOfxImageEffectActionGetClipPreferences)) {
        Inst* d = inst(e);
        bump("getClipPreferences");
        if (!d) return kOfxStatFailed;
        int mode = 0;
        gParam->paramGetValue(P(d, "Mode"), &mode);
        logf("misc: getClipPreferences mode=%d", mode);
        if (mode == 1) {
            ss(out, "OfxImageClipPropDepth_Output", 0, kOfxBitDepthFloat);
            ss(out, "OfxImageClipPropDepth_Source", 0, kOfxBitDepthFloat);
        } else {
            ss(out, "OfxImageClipPropDepth_Output", 0, kOfxBitDepthByte);
            ss(out, "OfxImageClipPropDepth_Source", 0, kOfxBitDepthByte);
        }
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionIsIdentity)) {
        Inst* d = inst(e);
        if (!d) return kOfxStatFailed;
        double t = gd(in, kOfxPropTime);
        bump("isIdentity");
        if (getD(d, "Amount", t) == 0.0) {
            ss(out, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
            sd(out, kOfxPropTime, 0, t + getI(d, "Offset", t));
            return kOfxStatOK;
        }
        return kOfxStatReplyDefault;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        bump("render");
        return d ? renderMisc(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    if (!strcmp(action, kOfxImageEffectActionBeginSequenceRender)) { bump("beginSequence"); return kOfxStatOK; }
    if (!strcmp(action, kOfxImageEffectActionEndSequenceRender)) { bump("endSequence"); return kOfxStatOK; }
    return kOfxStatReplyDefault;
}

// ====================================================================== 9. VEGAS-aware
static OfxStatus vegasHwndEntry(const char* action, const void* handle, PS in, PS out)
{
    OfxInteractHandle ih = (OfxInteractHandle)handle;
    char line[512];
    snprintf(line, sizeof line, "hwnd:%s", action);
    std::string extra;
    PS props = nullptr;
    if (gInteract && gInteract->interactGetPropertySet(ih, &props) == kOfxStatOK && props) {
        char b[96];
        snprintf(b, sizeof b, " effect=%p", gp(props, kOfxPropEffectInstance));
        extra += b;
    } else {
        extra += " (no property set)";
    }
    if (gVHwnd) {
        PS p2 = nullptr;
        char b[96];
        OfxStatus st1 = gVHwnd->getPropertySet(ih, &p2);
        OfxStatus st2 = gVHwnd->update(ih);
        snprintf(b, sizeof b, " vegasSuite=%d/%d/%s", st1, st2, p2 == props ? "same" : "different");
        extra += b;
    }
    if (!strcmp(action, V_HWND_CREATE)) {
        char b[96];
        snprintf(b, sizeof b, " parent=%p", gp(in, V_HWND_PARENT));
        extra += b;
        si(out, V_HWND_MIN_SIZE, 0, 120);
        si(out, V_HWND_MIN_SIZE, 1, 40);
        si(out, V_HWND_PREF_SIZE, 0, 360);
        si(out, V_HWND_PREF_SIZE, 1, 240);
    } else if (!strcmp(action, V_HWND_MOVE)) {
        char b[96];
        snprintf(b, sizeof b, " location=%d,%d,%d,%d", gi(in, V_HWND_LOCATION, 0), gi(in, V_HWND_LOCATION, 1), gi(in, V_HWND_LOCATION, 2),
                 gi(in, V_HWND_LOCATION, 3));
        extra += b;
    }
    logf("%s%s", line, extra.c_str());
    if (!strcmp(action, kOfxActionDescribe) || !strcmp(action, kOfxActionCreateInstance) || !strcmp(action, kOfxActionDestroyInstance) ||
        !strcmp(action, V_HWND_CREATE) || !strcmp(action, V_HWND_MOVE) || !strcmp(action, V_HWND_DISPOSE) || !strcmp(action, V_HWND_SHOW))
        return kOfxStatOK;
    return kOfxStatReplyDefault;
}

static OfxStatus renderVegas(Inst* d, double t, PS in)
{
    Checks c;
    PS ep = effectProps(d->eff);
    PS clipProps = nullptr;
    gEff->clipGetPropertySet(d->src, &clipProps);
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    logf("vegas: context=%s quality=%s views=%d view=%d clipOrder=%s imageOrder=%s", gs(ep, V_CONTEXT), gs(in, V_RENDER_QUALITY),
         gi(in, V_VIEWS_TO_RENDER, 0, -1), gi(in, V_RENDER_VIEW, 0, -1), gs(clipProps, V_PIXEL_ORDER), gs(s.h, V_PIXEL_ORDER));
    if (gVStereo) {
        PS sh = nullptr;
        OfxStatus st = gVStereo->getImage(d->src, t, 0, nullptr, &sh);
        c.expect("stereoscopic getImage", st, kOfxStatOK);
        if (sh) gEff->clipReleaseImage(sh);
    }
    if (gVKey) {
        char* kind = nullptr;
        OfxStatus st = gVKey->getInterp(P(d, "Amount"), t, &kind);
        char* kind2 = nullptr;
        OfxStatus st2 = gVKey->getInterp(P(d, "Flag"), t, &kind2);
        logf("vegas: keyInterp amount=%d/%s flag=%d/%s", st, st == kOfxStatOK && kind ? kind : "-", st2, st2 == kOfxStatOK && kind2 ? kind2 : "-");
    }
    if (gVProg1) {
        OfxStatus a = gVProg1->start(d->eff, "working", "id1");
        OfxStatus b = gVProg1->update(d->eff, 0.5);
        OfxStatus e = gVProg1->end(d->eff);
        logf("vegas: progress v1 %d/%d/%d", a, b, e);
    }
    if (gVProg2) {
        OfxStatus a = gVProg2->start(d->eff, "working", "id2", 1);
        OfxStatus b = gVProg2->update(d->eff, 1.0);
        OfxStatus e = gVProg2->end(d->eff);
        logf("vegas: progress v2 %d/%d/%d", a, b, e);
    }
    OfxStatus st = kOfxStatFailed;
    if (s.ok() && o.ok()) st = copyImage(s, o, renderWindow(in));
    releaseImg(s);
    releaseImg(o);
    return c.bad ? kOfxStatFailed : st;
}

static OfxStatus entryVegas(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) {
        fetchSuites();
        gHostIsVegas = !strcmp(gs(gHost->host, kOfxPropName), VEGAS_HOST_NAME) || !strcmp(gs(gHost->host, kOfxPropName), SONY_VEGAS_HOST_NAME);
        if (gHostIsVegas) {
            gVHwnd = (const VHwndSuite*)suite(V_HWND_SUITE, 1);
            gVProg1 = (const VProgressSuite1*)suite(V_PROGRESS_SUITE, 1);
            gVProg2 = (const VProgressSuite2*)suite(V_PROGRESS_SUITE, 2);
            gVStereo = (const VStereoSuite*)suite(V_STEREO_SUITE, 1);
            gVKey = (const VKeyframeSuite*)suite(V_KEYFRAME_SUITE, 1);
            // VEGAS's About / Help and HWnd overlay suites
            const void* effectSuite = suite("OfxVegasEffectSuite", 1);
            const void* overlay = suite("OfxHWndOverlayInteractSuite", 1);
            logf("vegas: load vegasHost=1 hwnd=%d progress1=%d progress2=%d stereo=%d keyframe=%d effectSuite=%d overlaySuite=%d", gVHwnd != nullptr,
                 gVProg1 != nullptr, gVProg2 != nullptr, gVStereo != nullptr, gVKey != nullptr, effectSuite != nullptr, overlay != nullptr);
        } else {
            logf("vegas: load vegasHost=0 (host is '%s')", gs(gHost->host, kOfxPropName));
        }
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test VEGAS Aware", "Reno Test", "Uses the VEGAS extensions when the host has them.", true, true, {kOfxImageEffectContextFilter});
        if (gHostIsVegas) sp(effectProps(e), V_HWND_ENTRY_PROP, 0, (void*)vegasHwndEntry);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        PS pp = defineParam(ps, kOfxParamTypeDouble, "Amount", "Amount");
        sd(pp, kOfxParamPropDefault, 0, 1.0);
        pp = defineParam(ps, kOfxParamTypeBoolean, "Flag", "Flag");
        si(pp, kOfxParamPropDefault, 0, 0);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {"Amount", "Flag"};
        return makeInstance(e, p, 2);
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "vegas");
    if (!strcmp(action, V_ACTION_ABOUT)) { logf("vegas: about"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionBeginInstanceEdit)) { logf("vegas: beginEdit"); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionEndInstanceEdit)) { logf("vegas: endEdit"); return kOfxStatOK; }
    if (!strcmp(action, kOfxImageEffectActionBeginSequenceRender)) {
        logf("vegas: beginSequence quality=%s views=%d view=%d interactive=%d", gs(in, V_RENDER_QUALITY), gi(in, V_VIEWS_TO_RENDER, 0, -1),
             gi(in, V_RENDER_VIEW, 0, -1), gi(in, kOfxPropIsInteractive, 0, -1));
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        return d ? renderVegas(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 9b. like VEGAS Pro 18's own effects
// "Test VEGAS 18" uses the host the way VEGAS Pro 18's stock effects do (Filters.ofx, Vfx1.ofx, ...). What their copy of
// the OpenFX support library treats as required answers kOfxStatErrMissingHostFeature when the host lacks it:
//   load               the host's OfxPropVegasSpikeKey (a string; its value is not looked at)
//   describe           declares its context only when the host is called "com.vegascreativesoftware.vegas"
//   describeInContext  appends the GUID of the older effect it replaces to OfxImageEffectPropVegasUpliftGUID (asks
//                      the dimension first)
//   isIdentity, render OfxPropVegasTimeCode and OfxImageEffectPropViewsToRender among the arguments
//   About, Help        asks the host to show them through OfxVegasEffectSuite (VEGAS's effects do not check the
//                      suite is there; this one says "missing host feature" instead of crashing)
#define V18_UPLIFT_GUID "{C55DDAF9-B236-11d2-BAD4-00C04F8EDC09}"
struct VEffectSuite { OfxStatus (*action)(OfxImageEffectHandle, const char*); };
struct VOverlaySuite { OfxStatus (*getPropertySet)(void*, PS*); OfxStatus (*redraw)(void*); };
static const VEffectSuite* gVEffect = nullptr;
static const VOverlaySuite* gVOverlay = nullptr;
static bool gV18Host = false;

static OfxStatus entryVegas18(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) {
        fetchSuites();
        char* name = nullptr;
        char* spike = nullptr;
        if (gProp->propGetString(gHost->host, kOfxPropName, 0, &name) != kOfxStatOK || !name) return kOfxStatErrMissingHostFeature;
        const OfxStatus sk = gProp->propGetString(gHost->host, "OfxPropVegasSpikeKey", 0, &spike);
        gVEffect = (const VEffectSuite*)suite("OfxVegasEffectSuite", 1);
        gVOverlay = (const VOverlaySuite*)suite("OfxHWndOverlayInteractSuite", 1);
        const void* opencl = suite("OfxOpenCLProgramSuite", 1);
        gV18Host = !strcmp(name, VEGAS_HOST_NAME);
        int ovNull1 = -1, ovNull2 = -1;
        if (gVOverlay) {
            PS ps = nullptr;
            ovNull1 = gVOverlay->getPropertySet(nullptr, &ps);
            ovNull2 = gVOverlay->redraw(nullptr);
        }
        logf("vegas18: load name=%s spike=%d effectSuite=%d overlaySuite=%d openclSuite=%d overlayNull=%d/%d", name, sk == kOfxStatOK,
             gVEffect != nullptr, gVOverlay != nullptr, opencl != nullptr, ovNull1, ovNull2);
        return sk == kOfxStatOK ? kOfxStatOK : kOfxStatErrMissingHostFeature;
    }
    if (!strcmp(action, kOfxActionDescribe)) {
        if (gV18Host) describeBasics(e, "Test VEGAS 18", "Reno Test", "Uses the host as VEGAS Pro 18's own effects do.", true, true, {kOfxImageEffectContextFilter});
        else describeBasics(e, "Test VEGAS 18", "Reno Test", "Not on VEGAS 18: no contexts.", true, true, {});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        PS p = effectProps(e);
        int n = -1;
        if (gProp->propGetDimension(p, "OfxImageEffectPropVegasUpliftGUID", &n) != kOfxStatOK) return kOfxStatErrMissingHostFeature;
        if (gProp->propSetString(p, "OfxImageEffectPropVegasUpliftGUID", n, V18_UPLIFT_GUID) != kOfxStatOK) return kOfxStatErrMissingHostFeature;
        logf("vegas18: uplift GUIDs before=%d", n);
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        PS pp = defineParam(ps, kOfxParamTypeDouble, "Amount", "Amount");
        sd(pp, kOfxParamPropDefault, 0, 1.0);
        // VEGAS's picture parameter (as Color Match's "ImageSource"): its definition must work
        PS ip = nullptr;
        OfxStatus ist = gParam->paramDefine(ps, "OfxParamTypeImage", "ImageSource", &ip);
        if (ist != kOfxStatOK || !ip) return ist == kOfxStatOK ? kOfxStatFailed : ist;
        if (gProp->propSetInt(ip, "OfxParamImageHideGrabFromPreview", 0, 1) != kOfxStatOK || gProp->propSetInt(ip, "OfxParamImageHideGrabControls", 0, 0) != kOfxStatOK)
            return kOfxStatErrMissingHostFeature;
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {"Amount", "ImageSource"};
        OfxStatus st = makeInstance(e, p, 2);
        Inst* d = inst(e);
        if (st == kOfxStatOK && d) {
            void* v = nullptr;
            logf("vegas18: image parameter value %d", gParam->paramGetValue(P(d, "ImageSource"), &v));
        }
        return st;
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "vegas18");
    if (!strcmp(action, kOfxImageEffectActionIsIdentity) || !strcmp(action, kOfxImageEffectActionRender)) {
        const bool render = !strcmp(action, kOfxImageEffectActionRender);
        double tc = 0;
        int views = 0, gl = -1, cl = -1;
        if (gProp->propGetDouble(in, "OfxPropVegasTimeCode", 0, &tc) != kOfxStatOK) return kOfxStatErrMissingHostFeature;
        if (gProp->propGetInt(in, "OfxImageEffectPropViewsToRender", 0, &views) != kOfxStatOK) return kOfxStatErrMissingHostFeature;
        gProp->propGetInt(in, "OfxImageEffectPropOpenGLEnabled", 0, &gl);       // (read, not needed)
        gProp->propGetInt(in, "OfxImageEffectPropOpenCLEnabled", 0, &cl);
        const double t = gd(in, kOfxPropTime);
        logf("vegas18: %s time=%g timecode=%g views=%d gl=%d cl=%d", render ? "render" : "isIdentity", t, tc, views, gl, cl);
        if (!render) return kOfxStatReplyDefault;
        Inst* d = inst(e);
        if (!d) return kOfxStatFailed;
        Img s2 = getImg(d->src, t), o = getImg(d->dst, t);
        OfxStatus st = (s2.ok() && o.ok()) ? copyImage(s2, o, renderWindow(in)) : kOfxStatFailed;
        releaseImg(s2);
        releaseImg(o);
        return st;
    }
    if (!strcmp(action, V_ACTION_ABOUT) || !strcmp(action, "OfxImageEffectActionInvokeHelp")) {
        if (!gVEffect) return kOfxStatErrMissingHostFeature;
        const bool about = !strcmp(action, V_ACTION_ABOUT);
        OfxStatus st = gVEffect->action(e, about ? "OfxVegasActionShowEffectAbout" : "OfxVegasActionShowEffectHelp");
        OfxStatus bad = gVEffect->action(e, "OfxVegasActionNoSuchThing");
        OfxStatus nul = gVEffect->action(nullptr, "OfxVegasActionShowEffectAbout");
        logf("vegas18: %s %d bad=%d null=%d", about ? "about" : "help", st, bad, nul);
        return st;
    }
    return kOfxStatReplyDefault;
}

// "Test Missing" asks for a suite and a host property no host has, and gives up (kOfxStatErrMissingHostFeature) in
// describeInContext, so the host's account of what was missing can be checked.
static OfxStatus entryMissing(const char* action, const void* handle, PS, PS)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Missing", "Reno Test Awkward", "needs things no host has", true, false, {kOfxImageEffectContextFilter});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        const void* s = suite("OfxImaginarySuite", 3);
        char* v = nullptr;
        OfxStatus st = gProp->propGetString(gHost->host, "OfxPropImaginary", 0, &v);
        logf("missing: suite %s, property %d", s ? "given" : "absent", st);
        return kOfxStatErrMissingHostFeature;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 10. effects that fail in describe / describeInContext / createInstance
static OfxStatus entryFailDescribe(const char* action, const void*, PS, PS)
{
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) { bump("failDescribe"); return kOfxStatFailed; }
    return kOfxStatReplyDefault;
}

static OfxStatus entryFailContext(const char* action, const void* handle, PS, PS)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Fails In Context", "Reno Test Awkward", "describeInContext fails", true, false, {kOfxImageEffectContextFilter});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) { bump("failContext"); return kOfxStatFailed; }
    return kOfxStatReplyDefault;
}

static OfxStatus entryFailCreate(const char* action, const void* handle, PS, PS)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Fails To Create", "Reno Test Awkward", "createInstance fails", true, false, {kOfxImageEffectContextFilter});
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) { bump("failCreate"); return kOfxStatFailed; }
    if (!strcmp(action, kOfxActionDestroyInstance)) { bump("destroyAfterFailedCreate"); return kOfxStatOK; }
    return kOfxStatReplyDefault;
}

// ---- a plug-in that cannot work without a suite the host does not have
static OfxStatus entryNeedsSuite(const char* action, const void* handle, PS, PS)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        const void* s = suite("OfxOpenGLRenderSuite", 1);
        logf("needsuite: OpenGL render suite %s", s ? "given" : "absent");
        if (!s) return kOfxStatErrMissingHostFeature;
        describeBasics(e, "Test Needs A Suite", "Reno Test Awkward", "", true, false, {kOfxImageEffectContextFilter});
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 19. overlay: on-screen controls over the preview
// "Test Overlay" has a 2D position ("Center", in canonical pixels) and draws a filled 9x9 square of the picture's own
// colours inverted there. Its overlay interact draws, with legacy OpenGL, a red cross 40 screen pixels wide through the
// centre and a green 8-pixel square round it (sizes from the pixel scale, so they look the same at any zoom). Pressing
// the pen within 6 screen pixels of the centre and dragging moves "Center"; the arrow keys move it a pixel. Everything
// it is handed is written to the log. The OpenGL functions are looked up when first needed (the binary does not link
// with any OpenGL library).
#ifdef _WIN32
#include <windows.h>
#define TFX_GLAPI __stdcall
#else
#include <dlfcn.h>
#define TFX_GLAPI
#endif
struct TfxGL {
    bool tried = false, ok = false;
    void(TFX_GLAPI* Begin)(unsigned) = nullptr;
    void(TFX_GLAPI* End)() = nullptr;
    void(TFX_GLAPI* Vertex2d)(double, double) = nullptr;
    void(TFX_GLAPI* Color3f)(float, float, float) = nullptr;
    void(TFX_GLAPI* LineWidth)(float) = nullptr;
    void(TFX_GLAPI* GetDoublev)(unsigned, double*) = nullptr;
    void(TFX_GLAPI* GetIntegerv)(unsigned, int*) = nullptr;
};
static TfxGL gGL;
static bool loadGL()
{
    if (gGL.tried) return gGL.ok;
    gGL.tried = true;
#ifdef _WIN32
    HMODULE lib = LoadLibraryA("opengl32.dll");
    auto f = [lib](const char* n) { return lib ? (void*)GetProcAddress(lib, n) : nullptr; };
#else
    void* lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    auto f = [lib](const char* n) { return lib ? dlsym(lib, n) : nullptr; };
#endif
    gGL.Begin = (decltype(gGL.Begin))f("glBegin");
    gGL.End = (decltype(gGL.End))f("glEnd");
    gGL.Vertex2d = (decltype(gGL.Vertex2d))f("glVertex2d");
    gGL.Color3f = (decltype(gGL.Color3f))f("glColor3f");
    gGL.LineWidth = (decltype(gGL.LineWidth))f("glLineWidth");
    gGL.GetDoublev = (decltype(gGL.GetDoublev))f("glGetDoublev");
    gGL.GetIntegerv = (decltype(gGL.GetIntegerv))f("glGetIntegerv");
    gGL.ok = gGL.Begin && gGL.End && gGL.Vertex2d && gGL.Color3f && gGL.LineWidth && gGL.GetDoublev && gGL.GetIntegerv;
    return gGL.ok;
}

struct OverlayData {
    OfxImageEffectHandle eff = nullptr;
    bool dragging = false;
    double grabX = 0, grabY = 0;       // where in the handle the pen took hold (canonical)
};

static OfxStatus renderOverlayFx(Inst* d, double t, PS in)
{
    Img s = getImg(d->src, t), o = getImg(d->dst, t);
    OfxStatus st = kOfxStatFailed;
    if (s.ok() && o.ok()) {
        Win w = renderWindow(in);
        st = copyImage(s, o, w);
        double c[2] = {0, 0};
        gParam->paramGetValueAtTime(P(d, "Center"), t, &c[0], &c[1]);
        double rs[2] = {1, 1};
        gProp->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, rs);
        // (a centre far outside the picture draws nothing: kept within int range before rounding)
        const int cx = (int)std::floor(std::max(-1.0e6, std::min(1.0e6, c[0] * rs[0]))), cy = (int)std::floor(std::max(-1.0e6, std::min(1.0e6, c[1] * rs[1])));
        for (int y = std::max(w.y1, cy - 4); y <= std::min(w.y2 - 1, cy + 4); ++y)
            for (int x = std::max(w.x1, cx - 4); x <= std::min(w.x2 - 1, cx + 4); ++x) {
                uint8_t* po = o.px(x, y);
                for (int k = 0; k < 3; ++k) {
                    if (o.isFloat) F(po)[k] = F(po)[3] - F(po)[k];
                    else po[k] = (uint8_t)(po[3] - po[k]);
                }
            }
    }
    releaseImg(s);
    releaseImg(o);
    return st;
}

static OfxStatus overlayInteract(const char* action, const void* handle, PS in, PS out)
{
    (void)out;
    OfxInteractHandle ih = (OfxInteractHandle)handle;
    PS ip = nullptr;
    if (!gInteract || gInteract->interactGetPropertySet(ih, &ip) != kOfxStatOK || !ip) {
        logf("overlay: %s without a property set", action);
        return kOfxStatFailed;
    }
    if (!strcmp(action, kOfxActionDescribe)) {
        logf("overlay: describe hasAlpha=%d bitDepth=%d", gi(ip, kOfxInteractPropHasAlpha), gi(ip, kOfxInteractPropBitDepth));
        return kOfxStatOK;
    }
    OverlayData* od = (OverlayData*)gp(ip, kOfxPropInstanceData);
    if (!strcmp(action, kOfxActionCreateInstance)) {
        od = new OverlayData();
        od->eff = (OfxImageEffectHandle)gp(ip, kOfxPropEffectInstance);
        sp(ip, kOfxPropInstanceData, 0, od);
        bump("overlayCreate");
        logf("overlay: create effect=%s pixelScale=%g,%g", od->eff ? "set" : "null", gd(ip, kOfxInteractPropPixelScale, 0), gd(ip, kOfxInteractPropPixelScale, 1));
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) {
        delete od;
        sp(ip, kOfxPropInstanceData, 0, nullptr);
        bump("overlayDestroy");
        logf("overlay: destroy");
        return kOfxStatOK;
    }
    if (!od) return kOfxStatFailed;
    Inst* d = inst(od->eff);
    if (!d) return kOfxStatFailed;
    const bool sameEffect = gp(in, kOfxPropEffectInstance) == (void*)od->eff;
    const double t = gd(in, kOfxPropTime);
    const double psx = gd(in, kOfxInteractPropPixelScale, 0, 1), psy = gd(in, kOfxInteractPropPixelScale, 1, 1);
    double c[2] = {0, 0};
    gParam->paramGetValueAtTime(P(d, "Center"), t, &c[0], &c[1]);
    if (!strcmp(action, kOfxInteractActionDraw)) {
        bump("overlayDraw");
        double proj[16] = {0};
        int vp[4] = {0, 0, 0, 0};
        const bool gl = loadGL();
        if (gl) {
            gGL.GetDoublev(0x0BA7 /* GL_PROJECTION_MATRIX */, proj);
            gGL.GetIntegerv(0x0BA2 /* GL_VIEWPORT */, vp);
        }
        logf("overlay: draw effect=%d time=%g pixelScale=%g,%g renderScale=%g,%g background=%g,%g,%g center=%g,%g gl=%d viewport=%d,%d proj=%g,%g,%g,%g",
             sameEffect, t, psx, psy, gd(in, kOfxImageEffectPropRenderScale, 0), gd(in, kOfxImageEffectPropRenderScale, 1),
             gd(in, kOfxInteractPropBackgroundColour, 0), gd(in, kOfxInteractPropBackgroundColour, 1), gd(in, kOfxInteractPropBackgroundColour, 2),
             c[0], c[1], gl, vp[2], vp[3], proj[0], proj[5], proj[12], proj[13]);
        if (!gl) return kOfxStatOK;
        gGL.LineWidth(1.0f);
        gGL.Color3f(1.0f, 0.0f, 0.0f);
        gGL.Begin(0x0001 /* GL_LINES */);
        gGL.Vertex2d(c[0] - 20 * psx, c[1]);
        gGL.Vertex2d(c[0] + 20 * psx, c[1]);
        gGL.Vertex2d(c[0], c[1] - 20 * psy);
        gGL.Vertex2d(c[0], c[1] + 20 * psy);
        gGL.End();
        gGL.Color3f(od->dragging ? 1.0f : 0.0f, 1.0f, 0.0f);
        gGL.Begin(0x0002 /* GL_LINE_LOOP */);
        gGL.Vertex2d(c[0] - 8 * psx, c[1] - 8 * psy);
        gGL.Vertex2d(c[0] + 8 * psx, c[1] - 8 * psy);
        gGL.Vertex2d(c[0] + 8 * psx, c[1] + 8 * psy);
        gGL.Vertex2d(c[0] - 8 * psx, c[1] + 8 * psy);
        gGL.End();
        if (gInteract) gInteract->interactSwapBuffers(ih);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxInteractActionPenDown) || !strcmp(action, kOfxInteractActionPenMotion) || !strcmp(action, kOfxInteractActionPenUp)) {
        const double x = gd(in, kOfxInteractPropPenPosition, 0), y = gd(in, kOfxInteractPropPenPosition, 1);
        const int vx = gi(in, kOfxInteractPropPenViewportPosition, 0), vy = gi(in, kOfxInteractPropPenViewportPosition, 1);
        const bool down = !strcmp(action, kOfxInteractActionPenDown), up = !strcmp(action, kOfxInteractActionPenUp);
        if (down) bump("overlayPenDown");
        else if (up) bump("overlayPenUp");
        else bump("overlayPenMotion");
        if (down || up || od->dragging)
            logf("overlay: %s effect=%d pen=%g,%g viewport=%d,%d pressure=%g time=%g pixelScale=%g,%g", down ? "penDown" : (up ? "penUp" : "penMotion"),
                 sameEffect, x, y, vx, vy, gd(in, kOfxInteractPropPenPressure), t, psx, psy);
        if (down) {
            if (std::fabs(x - c[0]) > 6 * psx || std::fabs(y - c[1]) > 6 * psy) return kOfxStatReplyDefault;
            od->dragging = true;
            od->grabX = x - c[0];
            od->grabY = y - c[1];
            gInteract->interactRedraw(ih);
            return kOfxStatOK;
        }
        if (!od->dragging) return kOfxStatReplyDefault;
        gParam->paramSetValue(P(d, "Center"), x - od->grabX, y - od->grabY);
        if (up) od->dragging = false;
        gInteract->interactRedraw(ih);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxInteractActionKeyDown) || !strcmp(action, kOfxInteractActionKeyUp) || !strcmp(action, kOfxInteractActionKeyRepeat)) {
        const int sym = gi(in, "kOfxPropKeySym");
        logf("overlay: %s sym=0x%x string=%s time=%g", action + strlen("OfxInteractAction"), sym, gs(in, "kOfxPropKeyString"), t);
        if (!strcmp(action, kOfxInteractActionKeyUp)) return kOfxStatReplyDefault;
        double dx = 0, dy = 0;
        if (sym == 0xff51) dx = -1;         // kOfxKey_Left
        else if (sym == 0xff53) dx = 1;     // kOfxKey_Right
        else if (sym == 0xff52) dy = 1;     // kOfxKey_Up
        else if (sym == 0xff54) dy = -1;    // kOfxKey_Down
        else return kOfxStatReplyDefault;
        gParam->paramSetValue(P(d, "Center"), c[0] + dx, c[1] + dy);
        gInteract->interactRedraw(ih);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxInteractActionGainFocus) || !strcmp(action, kOfxInteractActionLoseFocus)) {
        logf("overlay: %s", !strcmp(action, kOfxInteractActionGainFocus) ? "gainFocus" : "loseFocus");
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

static OfxStatus entryOverlay(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Overlay", "Reno Test", "A position with on-screen controls.", true, true, {kOfxImageEffectContextFilter});
        sp(effectProps(e), kOfxImageEffectPluginPropOverlayInteractV1, 0, (void*)overlayInteract);
        int over = 0;
        gProp->propGetInt(gHost->host, kOfxImageEffectPropSupportsOverlays, 0, &over);
        logf("overlay: host supportsOverlays=%d", over);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        PS pp = defineParam(ps, kOfxParamTypeDouble2D, "Center", "Center");
        ss(pp, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
        ss(pp, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesCanonical);
        sd(pp, kOfxParamPropDefault, 0, 960);
        sd(pp, kOfxParamPropDefault, 1, 540);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {"Center"};
        return makeInstance(e, p, 1);
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "overlay");
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        return d ? renderOverlayFx(d, gd(in, kOfxPropTime), in) : kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ====================================================================== 20. a setting with its own control
// "Test Param Interact" has one setting, "Level" (0..1), whose control the effect draws itself (an OpenFX custom
// parameter interact, kOfxParamPropInteractV1): a green bar filled to the level, with a white outline, drawn with legacy
// OpenGL in the control's own pixels. Pressing the pen sets the level to where it is across the control (dragging
// follows it); the Left and Right keys take a tenth off or add one. The picture is the source with its colour scaled by
// the level. What it is handed is written to the log.
struct LevelData {
    OfxImageEffectHandle eff = nullptr;
    bool dragging = false;
};

static OfxStatus levelInteract(const char* action, const void* handle, PS in, PS out)
{
    (void)out;
    OfxInteractHandle ih = (OfxInteractHandle)handle;
    PS ip = nullptr;
    if (!gInteract || gInteract->interactGetPropertySet(ih, &ip) != kOfxStatOK || !ip) return kOfxStatFailed;
    if (!strcmp(action, kOfxActionDescribe)) {
        logf("level: describe");
        return kOfxStatOK;
    }
    LevelData* ld = (LevelData*)gp(ip, kOfxPropInstanceData);
    if (!strcmp(action, kOfxActionCreateInstance)) {
        ld = new LevelData();
        ld->eff = (OfxImageEffectHandle)gp(ip, kOfxPropEffectInstance);
        sp(ip, kOfxPropInstanceData, 0, ld);
        bump("levelCreate");
        logf("level: create slave=%s", gs(ip, kOfxInteractPropSlaveToParam));
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) {
        delete ld;
        sp(ip, kOfxPropInstanceData, 0, nullptr);
        bump("levelDestroy");
        return kOfxStatOK;
    }
    if (!ld) return kOfxStatFailed;
    Inst* d = inst(ld->eff);
    if (!d) return kOfxStatFailed;
    const double t = gd(in, kOfxPropTime);
    const int vw = gi(in, "OfxInteractPropViewport", 0), vh = gi(in, "OfxInteractPropViewport", 1);
    double level = 0;
    gParam->paramGetValueAtTime(P(d, "Level"), t, &level);
    if (!strcmp(action, kOfxInteractActionDraw)) {
        bump("levelDraw");
        logf("level: draw size=%d,%d level=%g", vw, vh, level);
        if (!loadGL()) return kOfxStatOK;
        gGL.Color3f(0.0f, 1.0f, 0.0f);
        gGL.Begin(0x0007 /* GL_QUADS */);
        gGL.Vertex2d(2, 2);
        gGL.Vertex2d(2 + (vw - 4) * level, 2);
        gGL.Vertex2d(2 + (vw - 4) * level, vh - 2);
        gGL.Vertex2d(2, vh - 2);
        gGL.End();
        gGL.Color3f(1.0f, 1.0f, 1.0f);
        gGL.Begin(0x0002 /* GL_LINE_LOOP */);
        gGL.Vertex2d(1.5, 1.5);
        gGL.Vertex2d(vw - 1.5, 1.5);
        gGL.Vertex2d(vw - 1.5, vh - 1.5);
        gGL.Vertex2d(1.5, vh - 1.5);
        gGL.End();
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxInteractActionPenDown) || !strcmp(action, kOfxInteractActionPenMotion) || !strcmp(action, kOfxInteractActionPenUp)) {
        const double x = gd(in, kOfxInteractPropPenPosition, 0), y = gd(in, kOfxInteractPropPenPosition, 1);
        const bool down = !strcmp(action, kOfxInteractActionPenDown), up = !strcmp(action, kOfxInteractActionPenUp);
        if (down) {
            bump("levelPenDown");
            logf("level: penDown pen=%g,%g size=%d,%d", x, y, vw, vh);
            ld->dragging = true;
        }
        if (!ld->dragging) return kOfxStatReplyDefault;
        const double v = std::min(1.0, std::max(0.0, vw > 4 ? (x - 2) / (vw - 4) : 0.0));
        gParam->paramSetValue(P(d, "Level"), v);
        if (up) ld->dragging = false;
        gInteract->interactRedraw(ih);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxInteractActionKeyDown) || !strcmp(action, kOfxInteractActionKeyRepeat)) {
        const int sym = gi(in, "kOfxPropKeySym");
        double dv = 0;
        if (sym == 0xff51) dv = -0.1;
        else if (sym == 0xff53) dv = 0.1;
        else return kOfxStatReplyDefault;
        gParam->paramSetValue(P(d, "Level"), std::min(1.0, std::max(0.0, level + dv)));
        gInteract->interactRedraw(ih);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxInteractActionGainFocus) || !strcmp(action, kOfxInteractActionLoseFocus)) return kOfxStatOK;
    return kOfxStatReplyDefault;
}

static OfxStatus entryParamInteract(const char* action, const void* handle, PS in, PS out)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    (void)out;
    if (!strcmp(action, kOfxActionLoad)) { fetchSuites(); return kOfxStatOK; }
    if (!strcmp(action, kOfxActionDescribe)) {
        describeBasics(e, "Test Param Interact", "Reno Test", "A setting whose control the effect draws.", true, true, {kOfxImageEffectContextFilter});
        int custom = 0;
        gProp->propGetInt(gHost->host, kOfxParamHostPropSupportsCustomInteract, 0, &custom);
        logf("level: host supportsCustomInteract=%d", custom);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxImageEffectActionDescribeInContext)) {
        defineClip(e, kOfxImageEffectSimpleSourceClipName);
        defineClip(e, kOfxImageEffectOutputClipName);
        OfxParamSetHandle ps = nullptr;
        gEff->getParamSet(e, &ps);
        PS pp = defineParam(ps, kOfxParamTypeDouble, "Level", "Level");
        sd(pp, kOfxParamPropDefault, 0, 0.5);
        sd(pp, kOfxParamPropMin, 0, 0.0);
        sd(pp, kOfxParamPropMax, 0, 1.0);
        sp(pp, kOfxParamPropInteractV1, 0, (void*)levelInteract);
        si(pp, kOfxParamPropInteractPreferedSize, 0, 200);
        si(pp, kOfxParamPropInteractPreferedSize, 1, 24);
        sd(pp, kOfxParamPropInteractMinimumSize, 0, 60);
        sd(pp, kOfxParamPropInteractMinimumSize, 1, 16);
        return kOfxStatOK;
    }
    if (!strcmp(action, kOfxActionCreateInstance)) {
        const char* p[] = {"Level"};
        return makeInstance(e, p, 1);
    }
    if (!strcmp(action, kOfxActionDestroyInstance)) return freeInstance(e, "paraminteract");
    if (!strcmp(action, kOfxImageEffectActionRender)) {
        Inst* d = inst(e);
        if (!d) return kOfxStatFailed;
        const double t = gd(in, kOfxPropTime);
        Img s = getImg(d->src, t), o = getImg(d->dst, t);
        OfxStatus st = kOfxStatFailed;
        if (s.ok() && o.ok()) {
            Win w = renderWindow(in);
            st = copyImage(s, o, w);
            double level = 1;
            gParam->paramGetValueAtTime(P(d, "Level"), t, &level);
            for (int y = w.y1; y < w.y2; ++y)
                for (int x = w.x1; x < w.x2; ++x) {
                    uint8_t* po = o.px(x, y);
                    for (int k = 0; k < 3; ++k) {
                        if (o.isFloat) F(po)[k] = (float)(F(po)[k] * level);
                        else po[k] = (uint8_t)std::lround(po[k] * level);
                    }
                }
        }
        releaseImg(s);
        releaseImg(o);
        return st;
    }
    return kOfxStatReplyDefault;
}

// ---- a plug-in of another API: the host must pass it by
static OfxStatus entryOtherApi(const char*, const void*, PS, PS) { bump("otherApiCalled"); return kOfxStatFailed; }

// ====================================================================== the plug-in table
static void setHostFn(OfxHost* h)
{
    gHost = h;
}

#define PLUGIN(ID, ENTRY) {kOfxImageEffectPluginApi, 1, ID, 2, 7, setHostFn, ENTRY}
static OfxPlugin gPlugins[] = {
    PLUGIN("reno.test.invert", entryInvert),
    PLUGIN("reno.test.params", entryParams),
    PLUGIN("reno.test.temporal", entryTemporal),
    PLUGIN("reno.test.floatonly", entryFloatOnly),
    PLUGIN("reno.test.byteonly", entryByteOnly),
    PLUGIN("reno.test.generator", entryGenerator),
    PLUGIN("reno.test.transition", entryTransition),
    PLUGIN("reno.test.general", entryGeneral),
    PLUGIN("reno.test.misc", entryMisc),
    PLUGIN("reno.test.vegas", entryVegas),
    PLUGIN("reno.test.vegas18", entryVegas18),
    PLUGIN("reno.test.missing", entryMissing),
    PLUGIN("reno.test.faildescribe", entryFailDescribe),
    PLUGIN("reno.test.failcontext", entryFailContext),
    PLUGIN("reno.test.failcreate", entryFailCreate),
    PLUGIN("reno.test.failrender", entryFailRender),
    PLUGIN("reno.test.badsuite", entryBadSuite),
    PLUGIN("reno.test.needsuite", entryNeedsSuite),
    PLUGIN("reno.test.badcalls", entryBadCalls),
    PLUGIN("reno.test.leaky", entryLeaky),
    PLUGIN("reno.test.overlay", entryOverlay),
    PLUGIN("reno.test.paraminteract", entryParamInteract),
    {"OfxSomeOtherPluginApi", 1, "reno.test.otherapi", 1, 0, setHostFn, entryOtherApi},
    {kOfxImageEffectPluginApi, 1, "reno.test.nomain", 1, 0, setHostFn, nullptr},
};

extern "C" {

OfxExport int OfxGetNumberOfPlugins(void) { return (int)(sizeof gPlugins / sizeof gPlugins[0]); }

OfxExport OfxPlugin* OfxGetPlugin(int nth)
{
    if (nth < 0 || nth >= OfxGetNumberOfPlugins()) return nullptr;
    return &gPlugins[nth];
}

// A host that supports OfxSetHost calls it first. The test can ask this binary to say it has nothing for the host.
OfxExport OfxStatus OfxSetHost(const OfxHost* host)
{
    gHost = (OfxHost*)host;
    if (std::getenv("TESTFX_REFUSE_HOST")) return kOfxStatFailed;
    return kOfxStatOK;
}

// ---- what the host test reads
OfxExport int testfx_log_count(void)
{
    std::lock_guard<std::mutex> lk(gLogMutex);
    return (int)gLogLines.size();
}
OfxExport const char* testfx_log_line(int i)
{
    std::lock_guard<std::mutex> lk(gLogMutex);
    return (i >= 0 && i < (int)gLogLines.size()) ? gLogLines[(size_t)i].c_str() : nullptr;
}
OfxExport void testfx_log_clear(void)
{
    std::lock_guard<std::mutex> lk(gLogMutex);
    gLogLines.clear();
}
OfxExport int testfx_counter(const char* name)
{
    std::lock_guard<std::mutex> lk(gCounterMutex);
    auto it = gCounters.find(name ? name : "");
    return it == gCounters.end() ? 0 : it->second;
}
OfxExport void testfx_counters_clear(void)
{
    std::lock_guard<std::mutex> lk(gCounterMutex);
    gCounters.clear();
}
// The value dump of the last render of an effect ("params"); copied into the caller's buffer.
OfxExport int testfx_dump(const char* effect, char* buf, int bufSize)
{
    std::lock_guard<std::mutex> lk(gDumpMutex);
    auto it = gDumps.find(effect ? effect : "");
    if (it == gDumps.end() || !buf || bufSize < 1) return 0;
    snprintf(buf, (size_t)bufSize, "%s", it->second.c_str());
    return (int)it->second.size();
}

}  // extern "C"
