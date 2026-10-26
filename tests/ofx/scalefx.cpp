// scalefx - a test OpenFX plug-in for Reno's multi-resolution support (tests/ofx/scaletest.cpp).
//
// "Test Disc" says it supports multi-resolution (SCALEFX_NOMULTIRES in the environment makes it say 0) and draws a
// white disc over its source. Radius is a number of pixels at full size and Centre a 2D point in canonical coordinates,
// so the effect scales both by the render scale it was given, as the spec says. What it was handed on the last render
// is kept for the test to read (scalefx_last).
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEff = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;

struct Seen {
    double scaleX = 0, scaleY = 0;
    int bx1 = 0, by1 = 0, bx2 = 0, by2 = 0;      // the source picture's bounds
    int rx1 = 0, ry1 = 0, rx2 = 0, ry2 = 0;      // the render window
    int rodx2 = 0, rody2 = 0;                    // the picture's region of definition
    double radius = 0, cx = 0, cy = 0;           // the settings
    double drawnRadius = 0, drawnCx = 0, drawnCy = 0;   // what the plug-in drew, in pixels
    int renders = 0, idCalls = 0;
    double idScale = 0;                          // the render scale isIdentity got
    int hostMulti = -1;                          // what the host said about multi-resolution
};
static std::mutex gMu;
static Seen gSeen;

static void fetch()
{
    gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
    gEff = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
    gParam = (const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
}

struct Inst {
    OfxImageClipHandle src = nullptr, dst = nullptr;
    OfxParamHandle radius = nullptr, centre = nullptr;
};

static OfxStatus action(const char* a, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!std::strcmp(a, kOfxActionLoad)) {
        fetch();
        int v = -1;
        gProp->propGetInt(gHost->host, kOfxImageEffectPropSupportsMultiResolution, 0, &v);
        std::lock_guard<std::mutex> lk(gMu);
        gSeen.hostMulti = v;
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxActionDescribe)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Test Disc");
        gProp->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Reno Test");
        gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);
        gProp->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
        // (not set at all, it would be 1: the OpenFX default; SCALEFX_DEFAULTMULTIRES leaves it out to show that)
        if (!std::getenv("SCALEFX_DEFAULTMULTIRES"))
            gProp->propSetInt(p, kOfxImageEffectPropSupportsMultiResolution, 0, std::getenv("SCALEFX_NOMULTIRES") ? 0 : 1);
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxImageEffectActionDescribeInContext)) {
        OfxPropertySetHandle cp;
        gEff->clipDefine(e, kOfxImageEffectSimpleSourceClipName, &cp);
        gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        gEff->clipDefine(e, kOfxImageEffectOutputClipName, &cp);
        gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        OfxParamSetHandle ps;
        gEff->getParamSet(e, &ps);
        OfxPropertySetHandle pp;
        gParam->paramDefine(ps, kOfxParamTypeDouble, "Radius", &pp);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 0, 100.0);
        gParam->paramDefine(ps, kOfxParamTypeDouble2D, "Centre", &pp);
        gProp->propSetString(pp, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
        gProp->propSetString(pp, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesCanonical);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 0, 320.0);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 1, 180.0);
        return kOfxStatOK;
    }
    if (!std::strcmp(a, kOfxActionCreateInstance)) {
        Inst* d = new Inst();
        OfxParamSetHandle ps;
        gEff->getParamSet(e, &ps);
        gEff->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &d->src, nullptr);
        gEff->clipGetHandle(e, kOfxImageEffectOutputClipName, &d->dst, nullptr);
        gParam->paramGetHandle(ps, "Radius", &d->radius, nullptr);
        gParam->paramGetHandle(ps, "Centre", &d->centre, nullptr);
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
    if (!std::strcmp(a, kOfxImageEffectActionIsIdentity)) {
        double s = 0;
        gProp->propGetDouble(in, kOfxImageEffectPropRenderScale, 0, &s);
        std::lock_guard<std::mutex> lk(gMu);
        ++gSeen.idCalls;
        gSeen.idScale = s;
        return kOfxStatReplyDefault;
    }
    if (!std::strcmp(a, kOfxImageEffectActionRender)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* dp = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &dp);
        Inst* d = (Inst*)dp;
        if (!d) return kOfxStatFailed;
        double t = 0, sc[2] = {1, 1};
        int win[4] = {0, 0, 0, 0};
        gProp->propGetDouble(in, kOfxPropTime, 0, &t);
        gProp->propGetDoubleN(in, kOfxImageEffectPropRenderScale, 2, sc);
        gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, win);
        OfxPropertySetHandle si = nullptr, so = nullptr;
        if (gEff->clipGetImage(d->src, t, nullptr, &si) != kOfxStatOK) return kOfxStatFailed;
        if (gEff->clipGetImage(d->dst, t, nullptr, &so) != kOfxStatOK) { gEff->clipReleaseImage(si); return kOfxStatFailed; }
        void *sdata = nullptr, *odata = nullptr;
        int sb[4] = {0, 0, 0, 0}, ob[4] = {0, 0, 0, 0}, srb = 0, orb = 0, rod[4] = {0, 0, 0, 0};
        char* depth = nullptr;
        gProp->propGetPointer(si, kOfxImagePropData, 0, &sdata);
        gProp->propGetPointer(so, kOfxImagePropData, 0, &odata);
        gProp->propGetIntN(si, kOfxImagePropBounds, 4, sb);
        gProp->propGetIntN(so, kOfxImagePropBounds, 4, ob);
        gProp->propGetIntN(si, kOfxImagePropRegionOfDefinition, 4, rod);
        gProp->propGetInt(si, kOfxImagePropRowBytes, 0, &srb);
        gProp->propGetInt(so, kOfxImagePropRowBytes, 0, &orb);
        gProp->propGetString(so, kOfxImageEffectPropPixelDepth, 0, &depth);
        const bool flt = depth && !std::strcmp(depth, kOfxBitDepthFloat);
        double radius = 0, c[2] = {0, 0};
        gParam->paramGetValueAtTime(d->radius, t, &radius);
        gParam->paramGetValueAtTime(d->centre, t, &c[0], &c[1]);
        // the settings are in full-size pixels: the picture is smaller by the render scale
        const double r = radius * sc[0], cx = c[0] * sc[0], cy = c[1] * sc[1];
        const int bpp = flt ? 16 : 4;
        for (int y = win[1]; y < win[3]; ++y)
            for (int x = win[0]; x < win[2]; ++x) {
                const uint8_t* ps = (const uint8_t*)sdata + (ptrdiff_t)(y - sb[1]) * srb + (ptrdiff_t)(x - sb[0]) * bpp;
                uint8_t* po = (uint8_t*)odata + (ptrdiff_t)(y - ob[1]) * orb + (ptrdiff_t)(x - ob[0]) * bpp;
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                if (dx * dx + dy * dy <= r * r) {
                    if (flt) for (int k = 0; k < 4; ++k) ((float*)po)[k] = 1.f;
                    else std::memset(po, 255, 4);
                } else std::memcpy(po, ps, (size_t)bpp);
            }
        {
            std::lock_guard<std::mutex> lk(gMu);
            gSeen.scaleX = sc[0]; gSeen.scaleY = sc[1];
            gSeen.bx1 = sb[0]; gSeen.by1 = sb[1]; gSeen.bx2 = sb[2]; gSeen.by2 = sb[3];
            gSeen.rx1 = win[0]; gSeen.ry1 = win[1]; gSeen.rx2 = win[2]; gSeen.ry2 = win[3];
            gSeen.rodx2 = rod[2]; gSeen.rody2 = rod[3];
            gSeen.radius = radius; gSeen.cx = c[0]; gSeen.cy = c[1];
            gSeen.drawnRadius = r; gSeen.drawnCx = cx; gSeen.drawnCy = cy;
            ++gSeen.renders;
        }
        gEff->clipReleaseImage(si);
        gEff->clipReleaseImage(so);
        return kOfxStatOK;
    }
    return kOfxStatReplyDefault;
}

static void setHostFn(OfxHost* h) { gHost = h; }
static OfxPlugin gPlugins[] = {
    {kOfxImageEffectPluginApi, 1, "reno.test.disc", 1, 0, setHostFn, action},
};

extern "C" {
OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin* OfxGetPlugin(int n) { return n == 0 ? &gPlugins[0] : nullptr; }
// What the last render saw, as doubles: scale x,y, source bounds x1 y1 x2 y2, window x1 y1 x2 y2, rod x2 y2, radius, cx, cy,
// drawn radius, drawn cx, cy, renders, isIdentity calls, its scale, the host's multi-resolution answer.
OfxExport void scalefx_last(double* o)
{
    std::lock_guard<std::mutex> lk(gMu);
    const Seen& s = gSeen;
    const double v[] = {s.scaleX, s.scaleY, (double)s.bx1, (double)s.by1, (double)s.bx2, (double)s.by2, (double)s.rx1, (double)s.ry1, (double)s.rx2, (double)s.ry2,
                        (double)s.rodx2, (double)s.rody2, s.radius, s.cx, s.cy, s.drawnRadius, s.drawnCx, s.drawnCy, (double)s.renders, (double)s.idCalls, s.idScale, (double)s.hostMulti};
    for (size_t i = 0; i < sizeof v / sizeof v[0]; ++i) o[i] = v[i];
}
}
