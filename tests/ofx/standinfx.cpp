// standinfx - test OpenFX plug-ins that stand in for VEGAS's own, to try "reno_cli vegasfx" (src/ofx/fxcompare.cpp)
// without VEGAS. Their identifiers are VEGAS's, so the run treats them as real plug-ins:
//   com.vegascreativesoftware:invert     a filter with Reno's built-in Invert's setting (BlendAmount): the same maths plus
//                                        a small offset (STANDINFX_BIAS in the environment, default 6) so the comparison
//                                        has something to measure
//   de.magix:standincrash                a filter that crashes (null dereference) when Level is above one half, so the
//                                        run has to go on after it; no built-in of this one
//   de.magix:standinhang                 a filter that never returns when Level is in the middle (0.4 to 0.6), so the
//                                        run has to stop it and go on
//   de.magix:standinblank                a filter that makes a transparent picture whatever it is given
//   de.magix:standinfail                 a filter whose render always fails
//   de.magix:standingenerator            a generator (a gradient), no built-in
//   de.magix:standinnoinstance           says it misses a host feature when an instance is made
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEff = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;

enum Mode { Invert, Crash, Blank, Fail, Generator, NoInstance, Hang };

struct Inst {
    OfxImageClipHandle src = nullptr, dst = nullptr;
    OfxParamHandle p = nullptr;
};

template <int MODE>
static OfxStatus action(const char* a, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!std::strcmp(a, kOfxActionLoad)) {
        gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
        gEff = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
        gParam = (const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxActionDescribe)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        static const char* const labels[] = {"VEGAS Invert (stand-in)", "Stand-in that crashes", "Stand-in that draws nothing", "Stand-in that fails", "Stand-in generator", "Stand-in without instance", "Stand-in that hangs"};
        gProp->propSetString(p, kOfxPropLabel, 0, labels[MODE]);
        gProp->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Reno Test");
        gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, MODE == Generator ? kOfxImageEffectContextGenerator : kOfxImageEffectContextFilter);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
        gProp->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxImageEffectActionDescribeInContext)) {
        OfxPropertySetHandle cp;
        if (MODE != Generator) {
            gEff->clipDefine(e, kOfxImageEffectSimpleSourceClipName, &cp);
            gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        }
        gEff->clipDefine(e, kOfxImageEffectOutputClipName, &cp);
        gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        OfxParamSetHandle ps;
        gEff->getParamSet(e, &ps);
        OfxPropertySetHandle pp;
        if (MODE == Invert || MODE == Crash || MODE == Hang) {
            const char* name = MODE == Invert ? "BlendAmount" : "Level";
            gParam->paramDefine(ps, kOfxParamTypeDouble, name, &pp);
            gProp->propSetDouble(pp, kOfxParamPropDefault, 0, MODE == Invert ? 1.0 : 0.0);
            gProp->propSetDouble(pp, kOfxParamPropMin, 0, 0.0);
            gProp->propSetDouble(pp, kOfxParamPropMax, 0, 1.0);
            gProp->propSetDouble(pp, kOfxParamPropDisplayMin, 0, 0.0);
            gProp->propSetDouble(pp, kOfxParamPropDisplayMax, 0, 1.0);
        }
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxActionCreateInstance)) {
        if (MODE == NoInstance) return kOfxStatErrMissingHostFeature;
        Inst* d = new Inst();
        OfxParamSetHandle ps;
        gEff->getParamSet(e, &ps);
        if (MODE != Generator) gEff->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &d->src, nullptr);
        gEff->clipGetHandle(e, kOfxImageEffectOutputClipName, &d->dst, nullptr);
        if (MODE == Invert) gParam->paramGetHandle(ps, "BlendAmount", &d->p, nullptr);
        if (MODE == Crash || MODE == Hang) gParam->paramGetHandle(ps, "Level", &d->p, nullptr);
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        gProp->propSetPointer(p, kOfxPropInstanceData, 0, d);
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxActionDestroyInstance)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* d = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &d);
        delete (Inst*)d;
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxImageEffectActionRender)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* dp = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &dp);
        Inst* d = (Inst*)dp;
        if (!d || MODE == Fail) return kOfxStatFailed;
        double t = 0, amount = 1;
        int win[4] = {0, 0, 0, 0};
        gProp->propGetDouble(in, kOfxPropTime, 0, &t);
        gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, win);
        if (d->p) gParam->paramGetValueAtTime(d->p, t, &amount);
        if (MODE == Crash && amount > 0.5) {
            volatile int* null = nullptr;
            *null = 1;
        }
        if (MODE == Hang && amount > 0.4 && amount < 0.6) {
            volatile unsigned long spin = 0;
            for (;;) spin = spin + 1;
        }
        OfxPropertySetHandle si = nullptr, so = nullptr;
        if (d->src && gEff->clipGetImage(d->src, t, nullptr, &si) != kOfxStatOK) return kOfxStatFailed;
        if (gEff->clipGetImage(d->dst, t, nullptr, &so) != kOfxStatOK) { if (si) gEff->clipReleaseImage(si); return kOfxStatFailed; }
        void *sdata = nullptr, *odata = nullptr;
        int sb[4] = {0, 0, 0, 0}, ob[4] = {0, 0, 0, 0}, srb = 0, orb = 0;
        if (si) {
            gProp->propGetPointer(si, kOfxImagePropData, 0, &sdata);
            gProp->propGetIntN(si, kOfxImagePropBounds, 4, sb);
            gProp->propGetInt(si, kOfxImagePropRowBytes, 0, &srb);
        }
        gProp->propGetPointer(so, kOfxImagePropData, 0, &odata);
        gProp->propGetIntN(so, kOfxImagePropBounds, 4, ob);
        gProp->propGetInt(so, kOfxImagePropRowBytes, 0, &orb);
        const char* bias = std::getenv("STANDINFX_BIAS");
        const int offset = bias ? std::atoi(bias) : 6;
        for (int y = win[1]; y < win[3]; ++y)
            for (int x = win[0]; x < win[2]; ++x) {
                uint8_t* po = (uint8_t*)odata + (ptrdiff_t)(y - ob[1]) * orb + (ptrdiff_t)(x - ob[0]) * 4;
                if (MODE == Generator) {
                    po[0] = (uint8_t)(x * 255 / (ob[2] - ob[0]));
                    po[1] = (uint8_t)(y * 255 / (ob[3] - ob[1]));
                    po[2] = 90;
                    po[3] = 255;
                } else if (MODE == Blank) {
                    std::memset(po, 0, 4);
                } else {
                    const uint8_t* ps = (const uint8_t*)sdata + (ptrdiff_t)(y - sb[1]) * srb + (ptrdiff_t)(x - sb[0]) * 4;
                    if (MODE == Invert) {
                        // premultiplied colour: the opposite of a colour is alpha - colour
                        for (int k = 0; k < 3; ++k) {
                            const double inv = (double)ps[3] - ps[k];
                            int v = (int)(ps[k] + (inv - ps[k]) * amount + 0.5) + (ps[3] ? offset : 0);
                            po[k] = (uint8_t)(v < 0 ? 0 : (v > ps[3] ? ps[3] : v));
                        }
                        po[3] = ps[3];
                    } else std::memcpy(po, ps, 4);
                }
            }
        if (si) gEff->clipReleaseImage(si);
        gEff->clipReleaseImage(so);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

static void setHostFn(OfxHost* h) { gHost = h; }
static OfxPlugin gPlugins[] = {
    {kOfxImageEffectPluginApi, 1, "com.vegascreativesoftware:invert", 1, 0, setHostFn, action<Invert>},
    {kOfxImageEffectPluginApi, 1, "de.magix:standincrash", 1, 0, setHostFn, action<Crash>},
    {kOfxImageEffectPluginApi, 1, "de.magix:standinblank", 1, 0, setHostFn, action<Blank>},
    {kOfxImageEffectPluginApi, 1, "de.magix:standinfail", 1, 0, setHostFn, action<Fail>},
    {kOfxImageEffectPluginApi, 1, "de.magix:standingenerator", 1, 0, setHostFn, action<Generator>},
    {kOfxImageEffectPluginApi, 1, "de.magix:standinnoinstance", 1, 0, setHostFn, action<NoInstance>},
    {kOfxImageEffectPluginApi, 1, "de.magix:standinhang", 1, 0, setHostFn, action<Hang>},
};

extern "C" {
OfxExport int OfxGetNumberOfPlugins(void) { return (int)(sizeof gPlugins / sizeof gPlugins[0]); }
OfxExport OfxPlugin* OfxGetPlugin(int n) { return n >= 0 && n < (int)(sizeof gPlugins / sizeof gPlugins[0]) ? &gPlugins[n] : nullptr; }
}
