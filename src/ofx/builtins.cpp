// Built-in OpenFX plug-ins: the plumbing (entry points, describing, parameters, frames in and out), their presets, and VEGAS's rich text read
// for Titles & Text. The effects are in builtins_gen.cpp (media generators) and builtins_filters.cpp (video effects).
#include "builtins_impl.h"
#include "vegas_ext.h"
#include "../core/base.h"
#include "../core/pixrows.h"
#include "vkeffects.h"
#include "ofxProperty.h"
#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <map>
#include <mutex>
#include <utility>

namespace reno {
namespace ofx {

const char* const kBuiltinBinaryPath = "builtin:reno";
bool isBuiltinPath(const std::string& path) { return path == kBuiltinBinaryPath; }

namespace builtin {
namespace {

const OfxHost* gHost = nullptr;
const OfxPropertySuiteV1* gProp = nullptr;
const OfxImageEffectSuiteV1* gEff = nullptr;
const OfxParameterSuiteV1* gParam = nullptr;

const std::vector<Def>& defs()
{
    static const std::vector<Def> all = [] {
        std::vector<Def> v;
        addGenerators(v);
        addFilters(v);
        addFilters2(v);
        addFiltersDistort(v);
        addFiltersCookie(v);
        addFiltersMore(v);
        addTransitions(v);
        addFiltersVr(v);
        addFiltersMagix(v);
        addFiltersColor(v);
        addFiltersMask(v);
        addFiltersAnalysis(v);
        addFiltersVegasRest(v);
        applyVegasGroups(v);
        attachGpuPlanners(v);
        // effects whose render function writes every output pixel without reading it (checked one by one): the output need not be cleared first
        static const char* const fullOut[] = {"mirror", "sharpen", "convolutionkernel", "newsprint", "spherize", "chromakeyer", "swirl", "pinchpunch", "wave", "colorcurves", "layerdimensionality"};
        for (Def& d : v)
            for (const char* id : fullOut)
                if (d.id == std::string("com.vegascreativesoftware:") + id) d.overwritesOut = true;
        return v;
    }();
    return all;
}

typedef OfxPropertySetHandle PS;
int propInt(PS h, const char* n, int def = 0) { int v = def; return (h && gProp->propGetInt(h, n, 0, &v) == kOfxStatOK) ? v : def; }
double propDouble(PS h, const char* n, int idx = 0, double def = 0) { double v = def; return (h && gProp->propGetDouble(h, n, idx, &v) == kOfxStatOK) ? v : def; }
std::string propString(PS h, const char* n, int idx = 0)
{
    char* v = nullptr;
    if (!h || gProp->propGetString(h, n, idx, &v) != kOfxStatOK || !v) return std::string();
    return v;
}

void defineParam(OfxParamSetHandle set, const PDef& p)
{
    const char* type = kOfxParamTypeDouble;
    switch (p.type) {
    case PT::Double: type = kOfxParamTypeDouble; break;
    case PT::Int: type = kOfxParamTypeInteger; break;
    case PT::Bool: type = kOfxParamTypeBoolean; break;
    case PT::Choice: type = kOfxParamTypeChoice; break;
    case PT::RGBA: type = kOfxParamTypeRGBA; break;
    case PT::RGB: type = kOfxParamTypeRGB; break;
    case PT::Double2D: type = kOfxParamTypeDouble2D; break;
    case PT::String: type = kOfxParamTypeString; break;
    case PT::Custom: type = kOfxParamTypeCustom; break;
    case PT::Group: type = kOfxParamTypeGroup; break;
    case PT::Double3D: type = kOfxParamTypeDouble3D; break;
    case PT::Button: type = kOfxParamTypePushButton; break;
    case PT::Image: type = "OfxParamTypeImage"; break;      // kVegasParamTypeImage
    }
    PS props = nullptr;
    if (gParam->paramDefine(set, type, p.name.c_str(), &props) != kOfxStatOK || !props) return;
    gProp->propSetString(props, kOfxPropLabel, 0, p.label.c_str());
    if (!p.hint.empty()) gProp->propSetString(props, kOfxParamPropHint, 0, p.hint.c_str());
    if (!p.parent.empty()) gProp->propSetString(props, kOfxParamPropParent, 0, p.parent.c_str());
    if (p.secret) gProp->propSetInt(props, kOfxParamPropSecret, 0, 1);
    switch (p.type) {
    case PT::Group: case PT::Button: case PT::Image:
        break;
    case PT::Custom:
        gProp->propSetString(props, kOfxParamPropDefault, 0, p.sdef.c_str());
        break;
    case PT::String:
        gProp->propSetString(props, kOfxParamPropDefault, 0, p.sdef.c_str());
        gProp->propSetString(props, kOfxParamPropStringMode, 0, p.multiline ? kOfxParamStringIsMultiLine : kOfxParamStringIsSingleLine);
        break;
    case PT::Int: case PT::Bool: case PT::Choice:
        gProp->propSetInt(props, kOfxParamPropDefault, 0, p.def.empty() ? 0 : (int)p.def[0]);
        if (p.type == PT::Int) {
            gProp->propSetInt(props, kOfxParamPropMin, 0, (int)p.min);
            gProp->propSetInt(props, kOfxParamPropMax, 0, (int)p.max);
            gProp->propSetInt(props, kOfxParamPropDisplayMin, 0, (int)(std::isnan(p.dmin) ? p.min : p.dmin));
            gProp->propSetInt(props, kOfxParamPropDisplayMax, 0, (int)(std::isnan(p.dmax) ? p.max : p.dmax));
        }
        for (size_t k = 0; k < p.choices.size(); ++k) gProp->propSetString(props, kOfxParamPropChoiceOption, (int)k, p.choices[k].c_str());
        break;
    default: {
        for (size_t k = 0; k < p.def.size(); ++k) gProp->propSetDouble(props, kOfxParamPropDefault, (int)k, p.def[k]);
        if (p.type == PT::Double || p.type == PT::Double2D || p.type == PT::Double3D) {
            const int n = p.type == PT::Double ? 1 : p.type == PT::Double2D ? 2 : 3;
            for (int k = 0; k < n; ++k) {
                gProp->propSetDouble(props, kOfxParamPropMin, k, p.min);
                gProp->propSetDouble(props, kOfxParamPropMax, k, p.max);
                gProp->propSetDouble(props, kOfxParamPropDisplayMin, k, std::isnan(p.dmin) ? p.min : p.dmin);
                gProp->propSetDouble(props, kOfxParamPropDisplayMax, k, std::isnan(p.dmax) ? p.max : p.dmax);
            }
            if (p.normalised) {
                gProp->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeXYAbsolute);
                gProp->propSetString(props, kOfxParamPropDefaultCoordinateSystem, 0, kOfxParamCoordinatesNormalised);
            }
        }
        break;
    }
    }
    if (p.type != PT::Group && p.type != PT::Button && p.type != PT::Image) gProp->propSetInt(props, kOfxParamPropAnimates, 0, 1);
}

// A frame the host handed over, as the framework reads it.
struct View {
    PS h = nullptr;
    uint8_t* data = nullptr;
    int rowBytes = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    bool isFloat = false, bgra = false;
    bool ok() const { return data && x2 > x1 && y2 > y1; }
};

View viewOf(PS img)
{
    View v;
    v.h = img;
    if (!img) return v;
    void* d = nullptr;
    gProp->propGetPointer(img, kOfxImagePropData, 0, &d);
    v.data = (uint8_t*)d;
    v.rowBytes = propInt(img, kOfxImagePropRowBytes);
    int b[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) gProp->propGetInt(img, kOfxImagePropBounds, k, &b[k]);
    v.x1 = b[0]; v.y1 = b[1]; v.x2 = b[2]; v.y2 = b[3];
    const std::string depth = propString(img, kOfxImageEffectPropPixelDepth);
    v.isFloat = depth == kOfxBitDepthFloat;
    if (!v.isFloat && depth != kOfxBitDepthByte) v.data = nullptr;          // 16-bit is never asked for
    if (propString(img, kOfxImageEffectPropComponents) != kOfxImageComponentRGBA) v.data = nullptr;
    v.bgra = !v.isFloat && propString(img, kVegasPropPixelOrder) == kVegasPixelOrderBGRA;
    return v;
}

// The host's frame into a Buf of the frame's size (bounds x1..x2, y1..y2 of the frame; the rest transparent).
void load(const View& v, Buf& b, int W, int H, int fx1, int fy1)
{
    // a frame that covers the whole frame overwrites every value, so the buffer need not be cleared
    const bool covers = v.ok() && v.x1 <= fx1 && v.x2 >= fx1 + W && v.y1 <= fy1 && v.y2 >= fy1 + H;
    if (covers) b.ensure(W, H);
    else b.resize(W, H);
    if (!v.ok()) return;
    const int y0 = std::max(v.y1, fy1), y1 = std::min(v.y2, fy1 + H);
    const int x0 = std::max(v.x1, fx1), x1 = std::min(v.x2, fx1 + W);
    if (y1 <= y0 || x1 <= x0) return;
    parallelRows(y1 - y0, (size_t)(x1 - x0) * 4, [&](int first, int end) {
        for (int y = y0 + first; y < y0 + end; ++y) {
            const uint8_t* row = v.data + (ptrdiff_t)(y - v.y1) * v.rowBytes;
            float* dst = b.at(0, H - 1 - (y - fy1)) + (size_t)(x0 - fx1) * 4;
            if (v.isFloat) {
                std::memcpy(dst, (const float*)row + (size_t)(x0 - v.x1) * 4, (size_t)(x1 - x0) * 4 * sizeof(float));
            } else {
                pix::bytesRowToFloat(row + (size_t)(x0 - v.x1) * 4, dst, x1 - x0, v.bgra);
            }
        }
    });
}

// A value in 0..255 rounded to the nearest whole number, halves up: what std::lround gives, without a library call.
inline int roundNonNegative(float v)
{
    const int i = (int)v;
    return i + ((v - (float)i) >= 0.5f ? 1 : 0);
}

// colourAboveAlpha: the colour channels are kept up to 1 rather than to alpha (Def::colourAboveAlpha).
void store(const Buf& b, View& v, int fx1, int fy1, bool colourAboveAlpha = false)
{
    if (!v.ok()) return;
    const bool inside = v.x1 >= fx1 && v.x2 <= fx1 + b.w && v.y1 >= fy1 && v.y2 <= fy1 + b.h;
    parallelRows(v.y2 - v.y1, (size_t)(v.x2 - v.x1) * 4, [&](int first, int end) {
        for (int y = v.y1 + first; y < v.y1 + end; ++y) {
            uint8_t* row = v.data + (ptrdiff_t)(y - v.y1) * v.rowBytes;
            const int by = b.h - 1 - (y - fy1);
            if (inside && !v.isFloat && !colourAboveAlpha) {
                pix::floatRowToBytes(b.at(v.x1 - fx1, by), row, v.x2 - v.x1, v.bgra);
                continue;
            }
            for (int x = v.x1; x < v.x2; ++x) {
                const int bx = x - fx1;
                float p[4] = {0, 0, 0, 0};
                if (inside || (bx >= 0 && bx < b.w && by >= 0 && by < b.h)) std::memcpy(p, b.at(bx, by), sizeof p);
                // premultiplied: a colour channel never above alpha, so an 8-bit frame stays valid
                if (!(p[3] >= 0.f)) p[3] = 0.f;
                if (v.isFloat) {
                    float* o = (float*)row + (size_t)(x - v.x1) * 4;
                    for (int k = 0; k < 4; ++k) o[k] = std::isfinite(p[k]) ? p[k] : 0.f;
                } else {
                    uint8_t* o = row + (size_t)(x - v.x1) * 4;
                    const float a = clamp01(p[3]);
                    const float top = colourAboveAlpha ? 1.f : a;
                    auto q = [&](float c) { c = std::isfinite(c) ? std::min(std::max(c, 0.f), top) : 0.f; return (uint8_t)roundNonNegative(c * 255.f); };
                    o[v.bgra ? 2 : 0] = q(p[0]); o[1] = q(p[1]); o[v.bgra ? 0 : 2] = q(p[2]); o[3] = (uint8_t)roundNonNegative(a * 255.f);
                }
            }
        }
    });
}

OfxStatus describe(const Def& d, OfxImageEffectHandle effect)
{
    PS props = nullptr;
    gEff->getPropertySet(effect, &props);
    if (!props) return kOfxStatErrBadHandle;
    gProp->propSetString(props, kOfxPropLabel, 0, d.label.c_str());
    gProp->propSetString(props, kOfxImageEffectPluginPropGrouping, 0, d.group.c_str());
    gProp->propSetString(props, kOfxPropPluginDescription, 0, d.description.c_str());
    gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 0,
                       d.generator ? kOfxImageEffectContextGenerator : d.transition ? kOfxImageEffectContextTransition
                       : d.generalOnly ? kOfxImageEffectContextGeneral : kOfxImageEffectContextFilter);
    // VEGAS describes Layer Dimensionality as a filter and as a general effect, so the built-in does too
    if (d.general && !d.generator && !d.transition && !d.generalOnly) gProp->propSetString(props, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
    gProp->propSetString(props, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);
    gProp->propSetInt(props, kOfxImageEffectPluginPropSingleInstance, 0, 0);
    if (d.overlay) gProp->propSetPointer(props, kOfxImageEffectPluginPropOverlayInteractV1, 0, (void*)d.overlay);
    gProp->propSetString(props, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
    gProp->propSetInt(props, kOfxImageEffectPluginPropHostFrameThreading, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultiResolution, 0, d.multiRes ? 1 : 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsTiles, 0, 0);
    gProp->propSetInt(props, kOfxImageEffectPropTemporalClipAccess, 0, d.temporal ? 1 : 0);
    gProp->propSetInt(props, kOfxImageEffectPropSupportsMultipleClipDepths, 0, 0);
    return kOfxStatOK;
}

OfxStatus describeInContext(const Def& d, OfxImageEffectHandle effect)
{
    PS clip = nullptr;
    if (gEff->clipDefine(effect, kOfxImageEffectOutputClipName, &clip) == kOfxStatOK && clip)
        gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    if (d.transition) {
        for (const char* name : {kOfxImageEffectTransitionSourceFromClipName, kOfxImageEffectTransitionSourceToClipName}) {
            clip = nullptr;
            if (gEff->clipDefine(effect, name, &clip) == kOfxStatOK && clip)
                gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        }
    } else if (!d.generator) {
        clip = nullptr;
        if (gEff->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &clip) == kOfxStatOK && clip)
            gProp->propSetString(clip, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
    }
    OfxParamSetHandle set = nullptr;
    gEff->getParamSet(effect, &set);
    if (!set) return kOfxStatErrBadHandle;
    if (d.transition) {
        PS tp = nullptr;
        if (gParam->paramDefine(set, kOfxParamTypeDouble, kOfxImageEffectTransitionParamName, &tp) == kOfxStatOK && tp) {
            gProp->propSetString(tp, kOfxPropLabel, 0, "Transition");
            gProp->propSetDouble(tp, kOfxParamPropDefault, 0, 0.0);
            gProp->propSetDouble(tp, kOfxParamPropMin, 0, 0.0);
            gProp->propSetDouble(tp, kOfxParamPropMax, 0, 1.0);
            gProp->propSetDouble(tp, kOfxParamPropDisplayMin, 0, 0.0);
            gProp->propSetDouble(tp, kOfxParamPropDisplayMax, 0, 1.0);
            gProp->propSetInt(tp, kOfxParamPropAnimates, 0, 1);
        }
    }
    for (const PDef& p : d.params) defineParam(set, p);
    return kOfxStatOK;
}

// Float frames are kept per thread between renders: a full-size frame is tens of megabytes and allocating it each time costs as much in page faults
// as the effect itself. Frames bigger than 1080p are not kept.
struct Scratch {
    std::vector<float> src, src2, out;
    static void keep(std::vector<float>& from, std::vector<float>& to)
    {
        if (from.capacity() <= (size_t)10 * 1000 * 1000) to = std::move(from);          // 40 MB at most each: a 1080p frame, not a 4K one
        else to = std::vector<float>();
        from = std::vector<float>();
    }
};
thread_local Scratch gScratch;

OfxStatus render(const Def& d, OfxImageEffectHandle effect, PS inArgs)
{
    if (!d.render) return kOfxStatFailed;
    Ctx c;
    c.src.px = std::move(gScratch.src);
    c.src2.px = std::move(gScratch.src2);
    c.out.px = std::move(gScratch.out);
    if (d.generator) c.src.px.clear();                              // nothing is loaded into them: empty, as new
    if (!d.transition) c.src2.px.clear();
    struct GiveBack {
        Ctx& c;
        ~GiveBack() {
            Scratch::keep(c.src.px, gScratch.src);
            Scratch::keep(c.src2.px, gScratch.src2);
            Scratch::keep(c.out.px, gScratch.out);
        }
    } giveBack{c};
    c.time = propDouble(inArgs, kOfxPropTime);
    {
        // a host that does not say, or says nonsense: the project's size
        const double rs = propDouble(inArgs, kOfxImageEffectPropRenderScale, 0, 1.0);
        c.scale = (rs > 0.001 && rs <= 1.0) ? rs : 1.0;
    }
    gEff->getParamSet(effect, &c.params);
    {
        PS eprops = nullptr;
        gEff->getPropertySet(effect, &eprops);
        c.fps = propDouble(eprops, kOfxImageEffectPropFrameRate);
        if (!(c.fps > 0 && c.fps < 10000)) c.fps = 0;
    }
    OfxImageClipHandle outClip = nullptr;
    if (gEff->clipGetHandle(effect, kOfxImageEffectOutputClipName, &outClip, nullptr) != kOfxStatOK || !outClip) return kOfxStatFailed;
    PS outImg = nullptr;
    if (gEff->clipGetImage(outClip, c.time, nullptr, &outImg) != kOfxStatOK || !outImg) return kOfxStatFailed;
    View ov = viewOf(outImg);
    if (!ov.ok()) {
        gEff->clipReleaseImage(outImg);
        return kOfxStatErrImageFormat;
    }
    const int fx1 = ov.x1, fy1 = ov.y1;
    c.frameW = ov.x2 - ov.x1;
    c.frameH = ov.y2 - ov.y1;
    OfxStatus st = kOfxStatOK;
    if (d.transition) {
        OfxImageClipHandle fromClip = nullptr, toClip = nullptr;
        PS fromImg = nullptr, toImg = nullptr;
        if (gEff->clipGetHandle(effect, kOfxImageEffectTransitionSourceFromClipName, &fromClip, nullptr) == kOfxStatOK && fromClip)
            gEff->clipGetImage(fromClip, c.time, nullptr, &fromImg);
        if (gEff->clipGetHandle(effect, kOfxImageEffectTransitionSourceToClipName, &toClip, nullptr) == kOfxStatOK && toClip)
            gEff->clipGetImage(toClip, c.time, nullptr, &toImg);
        c.progress = clampd(c.d(kOfxImageEffectTransitionParamName), 0, 1);
        // On the GPU (Vulkan) when the transition has a kernel and both frames are of one kind covering the whole frame: the outgoing frame goes up
        // as is, the incoming one as floats with the plan (see vkeffects.cpp); else the CPU below
        const View fv = viewOf(fromImg), tv = viewOf(toImg);
        if (d.gpu && fv.ok() && tv.ok() && ov.ok() && fv.isFloat == ov.isFloat && fv.bgra == ov.bgra && fv.x1 <= fx1 && fv.x2 >= fx1 + c.frameW && fv.y1 <= fy1 &&
            fv.y2 >= fy1 + c.frameH && ov.x1 == fx1 && ov.y1 == fy1 && fv.rowBytes > 0 && ov.rowBytes > 0) {
            load(tv, c.src2, c.frameW, c.frameH, fx1, fy1);
            const size_t bpp = fv.isFloat ? 16 : 4;
            GpuFrame gf;
            gf.in = fv.data + (ptrdiff_t)(fy1 - fv.y1) * fv.rowBytes + (size_t)(fx1 - fv.x1) * bpp;
            gf.inRowBytes = (size_t)fv.rowBytes;
            gf.out = ov.data;
            gf.outRowBytes = (size_t)ov.rowBytes;
            gf.isFloat = fv.isFloat;
            gf.bgra = fv.bgra;
            if (gpuRender(d, c, gf)) {
                if (fromImg) gEff->clipReleaseImage(fromImg);
                if (toImg) gEff->clipReleaseImage(toImg);
                gEff->clipReleaseImage(outImg);
                return kOfxStatOK;
            }
        }
        load(fv, c.src, c.frameW, c.frameH, fx1, fy1);
        load(tv, c.src2, c.frameW, c.frameH, fx1, fy1);
        if (fromImg) gEff->clipReleaseImage(fromImg);
        if (toImg) gEff->clipReleaseImage(toImg);
    } else if (!d.generator) {
        OfxImageClipHandle srcClip = nullptr;
        PS srcImg = nullptr;
        if (gEff->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &srcClip, nullptr) == kOfxStatOK && srcClip)
            gEff->clipGetImage(srcClip, c.time, nullptr, &srcImg);
        const View sv = viewOf(srcImg);
        c.levels = !sv.isFloat;
        // On the GPU (Vulkan) when the effect has kernels and the frames fit the plan: the same picture, or within a level of it; on any failure
        // it returns false and the CPU path below runs
        if (d.gpu && sv.ok() && ov.ok() && sv.isFloat == ov.isFloat && sv.bgra == ov.bgra && sv.x1 <= fx1 && sv.x2 >= fx1 + c.frameW && sv.y1 <= fy1 &&
            sv.y2 >= fy1 + c.frameH && ov.x1 == fx1 && ov.y1 == fy1) {
            const size_t bpp = sv.isFloat ? 16 : 4;
            GpuFrame gf;
            gf.in = sv.data + (ptrdiff_t)(fy1 - sv.y1) * sv.rowBytes + (size_t)(fx1 - sv.x1) * bpp;
            gf.inRowBytes = (size_t)sv.rowBytes;
            gf.out = ov.data;
            gf.outRowBytes = (size_t)ov.rowBytes;
            gf.isFloat = sv.isFloat;
            gf.bgra = sv.bgra;
            gf.inSerial = (uint64_t)propDouble(srcImg, "RenoImageSerial");
            gf.outSerial = (uint64_t)propDouble(outImg, "RenoImageSerial");
            if (sv.rowBytes > 0 && ov.rowBytes > 0 && gpuRender(d, c, gf)) {
                if (srcImg) gEff->clipReleaseImage(srcImg);
                gEff->clipReleaseImage(outImg);
                return kOfxStatOK;
            }
        }
        if (d.rows && sv.ok() && !sv.isFloat && !ov.isFloat && sv.x1 <= fx1 && sv.x2 >= fx1 + c.frameW && sv.y1 <= fy1 && sv.y2 >= fy1 + c.frameH) {
            RowFn rowFn;
            bool have = false;
            try { have = d.rows(c, rowFn) && rowFn; } catch (...) { have = false; }
            if (have) {
                const int W = c.frameW, H = c.frameH;
                parallelRows(H, (size_t)W * 8, [&](int first, int end) {
                    thread_local std::vector<float> rowIn, rowOut;
                    rowIn.resize((size_t)W * 4);
                    rowOut.resize((size_t)W * 4);
                    for (int y = ov.y1 + first; y < ov.y1 + end; ++y) {
                        const uint8_t* srow = sv.data + (ptrdiff_t)(y - sv.y1) * sv.rowBytes + (size_t)(fx1 - sv.x1) * 4;
                        pix::bytesRowToFloat(srow, rowIn.data(), W, sv.bgra);
                        rowFn(rowIn.data(), rowOut.data(), W, H - 1 - (y - fy1));
                        pix::floatRowToBytes(rowOut.data(), ov.data + (ptrdiff_t)(y - ov.y1) * ov.rowBytes, W, ov.bgra);
                    }
                });
                if (srcImg) gEff->clipReleaseImage(srcImg);
                gEff->clipReleaseImage(outImg);
                return kOfxStatOK;
            }
        }
        load(sv, c.src, c.frameW, c.frameH, fx1, fy1);
        if (srcImg) gEff->clipReleaseImage(srcImg);
        if (d.temporal) {
            c.instance = effect;
            const int W = c.frameW, H = c.frameH;
            c.fetch = [srcClip, W, H, fx1, fy1](double t, Buf& into) {
                PS img = nullptr;
                if (!srcClip || gEff->clipGetImage(srcClip, t, nullptr, &img) != kOfxStatOK || !img) return false;
                const View v = viewOf(img);
                const bool ok = v.ok();
                if (ok) load(v, into, W, H, fx1, fy1);
                gEff->clipReleaseImage(img);
                return ok;
            };
        }
    }
    if (d.overwritesOut || d.rows) c.out.ensure(c.frameW, c.frameH);
    else c.out.resize(c.frameW, c.frameH);
    try {
        d.render(c);
    } catch (...) {
        st = kOfxStatFailed;
    }
    if (st == kOfxStatOK) store(c.out, ov, fx1, fy1, d.colourAboveAlpha);
    gEff->clipReleaseImage(outImg);
    return st;
}

// A setting changed (by the user): the effect's own function, if it has one, is told.
OfxStatus instanceChanged(const Def& d, OfxImageEffectHandle effect, PS inArgs)
{
    if (!d.changed) return kOfxStatReplyDefault;
    if (propString(inArgs, kOfxPropType) != kOfxTypeParameter) return kOfxStatReplyDefault;
    Change ch;
    ch.effect = effect;
    ch.param = propString(inArgs, kOfxPropName);
    ch.time = propDouble(inArgs, kOfxPropTime);
    gEff->getParamSet(effect, &ch.ctx.params);
    ch.ctx.time = ch.time;
    {
        PS eprops = nullptr;
        gEff->getPropertySet(effect, &eprops);
        ch.ctx.fps = propDouble(eprops, kOfxImageEffectPropFrameRate);
        if (!(ch.ctx.fps > 0 && ch.ctx.fps < 10000)) ch.ctx.fps = 0;
    }
    if (!ch.ctx.params) return kOfxStatReplyDefault;
    try {
        d.changed(ch);
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatOK;
}

OfxStatus dispatch(int index, const char* action, const void* handle, PS inArgs, PS /*outArgs*/)
{
    const auto& all = defs();
    if (index < 0 || index >= (int)all.size() || !action) return kOfxStatFailed;
    if (!gProp || !gEff || !gParam) return kOfxStatErrMissingHostFeature;
    const Def& d = all[(size_t)index];
    OfxImageEffectHandle effect = (OfxImageEffectHandle)handle;
    if (!std::strcmp(action, kOfxActionLoad) || !std::strcmp(action, kOfxActionUnload)) return kOfxStatOK;
    if (!std::strcmp(action, kOfxActionDescribe)) return describe(d, effect);
    if (!std::strcmp(action, kOfxImageEffectActionDescribeInContext)) return describeInContext(d, effect);
    if (!std::strcmp(action, kOfxActionDestroyInstance)) {
        if (d.destroy) d.destroy(handle);
        return kOfxStatOK;
    }
    if (!std::strcmp(action, kOfxActionCreateInstance)) return kOfxStatOK;
    if (!std::strcmp(action, kOfxImageEffectActionRender)) return render(d, effect, inArgs);
    if (!std::strcmp(action, kOfxActionInstanceChanged)) return instanceChanged(d, effect, inArgs);
    return kOfxStatReplyDefault;
}

// One entry point per plug-in (an OpenFX entry point carries no data of its own to say which effect it is).
template <int N>
OfxStatus entry(const char* action, const void* handle, PS inArgs, PS outArgs) { return dispatch(N, action, handle, inArgs, outArgs); }

const int kMaxPlugins = 256;
template <int... I>
std::vector<OfxPluginEntryPoint*> entries(std::integer_sequence<int, I...>) { return {&entry<I>...}; }

std::vector<OfxPlugin>& plugins()
{
    static std::vector<OfxPlugin> list = [] {
        std::vector<OfxPlugin> v;
        const auto points = entries(std::make_integer_sequence<int, kMaxPlugins>());
        const auto& all = defs();
        for (size_t k = 0; k < all.size() && k < points.size(); ++k) {
            OfxPlugin p;
            p.pluginApi = kOfxImageEffectPluginApi;
            p.apiVersion = 1;
            p.pluginIdentifier = all[k].id.c_str();
            p.pluginVersionMajor = 1;
            p.pluginVersionMinor = 0;
            p.setHost = [](OfxHost* h) { builtinSetHost(h); };
            p.mainEntry = points[k];
            v.push_back(p);
        }
        return v;
    }();
    return list;
}

}  // namespace

// ------------------------------------------------------------------ shorthands
PDef dbl(const std::string& name, const std::string& label, double def, double min, double max, const std::string& parent)
{
    PDef p; p.type = PT::Double; p.name = name; p.label = label; p.def = {def}; p.min = min; p.max = max; p.parent = parent; return p;
}
PDef intp(const std::string& name, const std::string& label, int def, int min, int max, const std::string& parent)
{
    PDef p; p.type = PT::Int; p.name = name; p.label = label; p.def = {(double)def}; p.min = min; p.max = max; p.parent = parent; return p;
}
PDef boolean(const std::string& name, const std::string& label, bool def, const std::string& parent)
{
    PDef p; p.type = PT::Bool; p.name = name; p.label = label; p.def = {def ? 1.0 : 0.0}; p.parent = parent; return p;
}
PDef choice(const std::string& name, const std::string& label, int def, std::vector<std::string> options, const std::string& parent)
{
    PDef p; p.type = PT::Choice; p.name = name; p.label = label; p.def = {(double)def}; p.choices = std::move(options); p.parent = parent; return p;
}
PDef rgba(const std::string& name, const std::string& label, double r, double g, double b, double a, const std::string& parent)
{
    PDef p; p.type = PT::RGBA; p.name = name; p.label = label; p.def = {r, g, b, a}; p.parent = parent; return p;
}
PDef rgb(const std::string& name, const std::string& label, double r, double g, double b, const std::string& parent)
{
    PDef p; p.type = PT::RGB; p.name = name; p.label = label; p.def = {r, g, b}; p.parent = parent; return p;
}
PDef point(const std::string& name, const std::string& label, double x, double y, const std::string& parent)
{
    PDef p; p.type = PT::Double2D; p.name = name; p.label = label; p.def = {x, y}; p.min = -10; p.max = 10; p.dmin = 0; p.dmax = 1;
    p.normalised = true; p.parent = parent; return p;
}
PDef text(const std::string& name, const std::string& label, const std::string& def, bool multiline)
{
    PDef p; p.type = PT::String; p.name = name; p.label = label; p.sdef = def; p.multiline = multiline; return p;
}
PDef custom(const std::string& name, const std::string& label, const std::string& def)
{
    PDef p; p.type = PT::Custom; p.name = name; p.label = label; p.sdef = def; p.secret = true; return p;
}
PDef group(const std::string& name, const std::string& label)
{
    PDef p; p.type = PT::Group; p.name = name; p.label = label; return p;
}
PDef point3(const std::string& name, const std::string& label, double x, double y, double z, double min, double max, const std::string& parent)
{
    PDef p; p.type = PT::Double3D; p.name = name; p.label = label; p.def = {x, y, z}; p.min = min; p.max = max; p.parent = parent; return p;
}
PDef button(const std::string& name, const std::string& label, const std::string& parent)
{
    PDef p; p.type = PT::Button; p.name = name; p.label = label; p.parent = parent; return p;
}

PDef imageParam(const std::string& name, const std::string& label, const std::string& parent)
{
    PDef p; p.type = PT::Image; p.name = name; p.label = label; p.parent = parent; return p;
}

// ------------------------------------------------------------------ parameter values
static OfxParamHandle handleOf(OfxParamSetHandle set, const char* name)
{
    OfxParamHandle h = nullptr;
    if (!set || gParam->paramGetHandle(set, name, &h, nullptr) != kOfxStatOK) return nullptr;
    return h;
}

static const std::vector<double>* testValue(const Ctx& c, const char* name)
{
    if (!c.testParams) return nullptr;
    auto it = c.testParams->find(name);
    return it == c.testParams->end() || it->second.empty() ? nullptr : &it->second;
}

const Def* findDef(const std::string& id)
{
    for (const Def& d : defs())
        if (d.id == id) return &d;
    return nullptr;
}

double Ctx::d(const char* name) const
{
    double v = 0;
    if (const auto* t = testValue(*this, name)) return std::isfinite((*t)[0]) ? (*t)[0] : 0.0;
    if (OfxParamHandle h = handleOf(params, name)) gParam->paramGetValueAtTime(h, time, &v);
    return std::isfinite(v) ? v : 0.0;
}
int Ctx::i(const char* name) const
{
    int v = 0;
    if (const auto* t = testValue(*this, name)) return (int)std::lround((*t)[0]);
    if (OfxParamHandle h = handleOf(params, name)) gParam->paramGetValueAtTime(h, time, &v);
    return v;
}
Color Ctx::color(const char* name) const
{
    Color c;
    if (const auto* t = testValue(*this, name)) {
        c.r = (*t)[0]; c.g = t->size() > 1 ? (*t)[1] : 0; c.b = t->size() > 2 ? (*t)[2] : 0; c.a = t->size() > 3 ? (*t)[3] : 1;
        c.a = clampd(c.a, 0, 1);
        return c;
    }
    OfxParamHandle h = handleOf(params, name);
    if (!h) return c;
    PS props = nullptr;
    gParam->paramGetPropertySet(h, &props);
    const std::string type = propString(props, kOfxParamPropType);
    if (type == kOfxParamTypeRGB) {
        gParam->paramGetValueAtTime(h, time, &c.r, &c.g, &c.b);
        c.a = 1;
    } else {
        gParam->paramGetValueAtTime(h, time, &c.r, &c.g, &c.b, &c.a);
    }
    auto fin = [](double& v) { if (!std::isfinite(v)) v = 0; };
    fin(c.r); fin(c.g); fin(c.b); fin(c.a);
    c.a = clampd(c.a, 0, 1);
    return c;
}
void Ctx::xy(const char* name, double& x, double& y) const
{
    x = y = 0;
    if (const auto* t = testValue(*this, name)) { x = (*t)[0]; y = t->size() > 1 ? (*t)[1] : 0; return; }
    if (OfxParamHandle h = handleOf(params, name)) gParam->paramGetValueAtTime(h, time, &x, &y);
    if (!std::isfinite(x)) x = 0;
    if (!std::isfinite(y)) y = 0;
}
void Ctx::xyz(const char* name, double& x, double& y, double& z) const
{
    x = y = z = 0;
    if (const auto* t = testValue(*this, name)) {
        x = (*t)[0]; y = t->size() > 1 ? (*t)[1] : (t->size() == 1 ? x : 0); z = t->size() > 2 ? (*t)[2] : (t->size() == 1 ? x : 0);
        return;
    }
    if (OfxParamHandle h = handleOf(params, name)) gParam->paramGetValueAtTime(h, time, &x, &y, &z);
    if (!std::isfinite(x)) x = 0;
    if (!std::isfinite(y)) y = 0;
    if (!std::isfinite(z)) z = 0;
}
std::string Ctx::s(const char* name) const
{
    if (testStrings) {
        auto it = testStrings->find(name);
        if (it != testStrings->end()) return it->second;
    }
    char* v = nullptr;
    if (OfxParamHandle h = handleOf(params, name)) gParam->paramGetValueAtTime(h, time, &v);
    return v ? std::string(v) : std::string();
}

// ------------------------------------------------------------------ changes
// (writing a setting from the effect: the host's paramSetValue; with keyframes it sets a key at the instance's time)
void Change::setD(const char* name, double v) const { if (OfxParamHandle h = handleOf(ctx.params, name)) gParam->paramSetValue(h, v); }
void Change::setI(const char* name, int v) const { if (OfxParamHandle h = handleOf(ctx.params, name)) gParam->paramSetValue(h, v); }
void Change::setXY(const char* name, double x, double y) const { if (OfxParamHandle h = handleOf(ctx.params, name)) gParam->paramSetValue(h, x, y); }
void Change::setXYZ(const char* name, double x, double y, double z) const { if (OfxParamHandle h = handleOf(ctx.params, name)) gParam->paramSetValue(h, x, y, z); }
void Change::setS(const char* name, const std::string& v) const { if (OfxParamHandle h = handleOf(ctx.params, name)) gParam->paramSetValue(h, v.c_str()); }
void Change::setColor(const char* name, const Color& c) const
{
    OfxParamHandle h = handleOf(ctx.params, name);
    if (!h) return;
    PS props = nullptr;
    gParam->paramGetPropertySet(h, &props);
    if (propString(props, kOfxParamPropType) == kOfxParamTypeRGB) gParam->paramSetValue(h, c.r, c.g, c.b);
    else gParam->paramSetValue(h, c.r, c.g, c.b, c.a);
}

bool Change::source(double frame, Buf& out) const
{
    out = Buf();
    if (!ctx.params || !effect) return false;
    OfxImageClipHandle clip = nullptr;
    if (gEff->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &clip, nullptr) != kOfxStatOK || !clip) return false;
    PS img = nullptr;
    if (gEff->clipGetImage(clip, frame, nullptr, &img) != kOfxStatOK || !img) return false;
    const View v = viewOf(img);
    bool ok = false;
    if (v.ok()) {
        load(v, out, v.x2 - v.x1, v.y2 - v.y1, v.x1, v.y1);
        ok = true;
    }
    gEff->clipReleaseImage(img);
    return ok;
}

void Change::clipRange(double& first, double& last) const
{
    first = last = 0;
    if (!effect) return;
    OfxImageClipHandle clip = nullptr;
    if (gEff->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &clip, nullptr) != kOfxStatOK || !clip) return;
    PS cp = nullptr;
    if (gEff->clipGetPropertySet(clip, &cp) != kOfxStatOK || !cp) return;
    first = propDouble(cp, kOfxImageEffectPropFrameRange, 0, 0);
    last = propDouble(cp, kOfxImageEffectPropFrameRange, 1, 0);
}

// ------------------------------------------------------------------ frame helpers
void Buf::sampleEdge(double x, double y, float out[4]) const
{
    out[0] = out[1] = out[2] = out[3] = 0.f;
    if (w <= 0 || h <= 0 || !std::isfinite(x) || !std::isfinite(y)) return;
    x -= 0.5; y -= 0.5;
    if (x < -1 || y < -1 || x > w || y > h) return;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            const int xx = x0 + i, yy = y0 + j;
            if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
            const float wgt = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            const float* p = at(xx, yy);
            for (int k = 0; k < 4; ++k) out[k] += p[k] * wgt;
        }
}

void runRowFn(Ctx& c, const RowFn& fn)
{
    const int W = c.out.w, H = c.out.h;
    forRows(H, W, [&](int y) { fn(c.src.at(0, y), c.out.at(0, y), W, y); });
}

void runRows(Ctx& c, bool (*prepare)(Ctx&, RowFn&))
{
    RowFn fn;
    if (prepare(c, fn)) runRowFn(c, fn);
}

void forRows(int h, int w, const std::function<void(int y)>& fn)
{
    parallelRows(h, (size_t)std::max(1, w) * 8, [&](int first, int end) {
        for (int y = first; y < end; ++y) fn(y);
    });
}

// A blur along one line of values with a Gaussian of 'sigma' (taps to 3 sigma), edges clamped.
static void blurLine(const float* in, float* out, int n, int stride, const std::vector<float>& kernel)
{
    const int r = (int)kernel.size() - 1;
    for (int i = 0; i < n; ++i) {
        float s = in[(size_t)i * stride] * kernel[0];
        for (int k = 1; k <= r; ++k) {
            const int a = std::max(0, i - k), b = std::min(n - 1, i + k);
            s += (in[(size_t)a * stride] + in[(size_t)b * stride]) * kernel[(size_t)k];
        }
        out[(size_t)i * stride] = s;
    }
}

static std::vector<float> gaussKernel(double sigma)
{
    const int r = (int)std::min(2000.0, std::ceil(sigma * 3));
    std::vector<float> k((size_t)r + 1);
    double sum = 0;
    for (int i = 0; i <= r; ++i) { k[(size_t)i] = (float)std::exp(-0.5 * i * i / (sigma * sigma)); sum += (i ? 2 : 1) * k[(size_t)i]; }
    for (auto& v : k) v = (float)(v / sum);
    return k;
}

#ifdef RENO_PIXROWS_SSE2
// The picture a blur pass writes into, kept by the calling thread between blurs (a fresh 33 MB vector per pass was dominated by clearing and page
// faults; the pass writes every value it uses). The blurred picture is swapped in for the old one, which becomes the next pass's scratch.
static std::vector<float>& blurScratch(size_t n, bool size = true)
{
    static thread_local std::vector<float> v;
    if (size && v.size() != n) v.resize(n);
    return v;
}
static void trimBlurScratch()
{
    std::vector<float>& v = blurScratch(0, false);
    if (v.capacity() > (size_t)1 << 24) std::vector<float>().swap(v);       // nothing above 64 MB is kept
}
// The kernel with each weight in four lanes (one aligned load per tap).
static std::vector<float> spreadKernel(const std::vector<float>& k)
{
    std::vector<float> out(k.size() * 4 + 4);
    for (size_t i = 0; i < k.size(); ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + (size_t)j] = k[i];
    return out;
}
static inline const float* tapAt(const std::vector<float>& kb, int k)
{
    // the vector's storage is not 16-byte aligned for sure: read unaligned
    return kb.data() + (size_t)k * 4;
}

// blurLine over a row of 'n' RGBA pixels, four channels at once; each channel gets exactly the sums blurLine gives it.
static void blurRowRGBA(const float* in, float* out, int n, const std::vector<float>& kern, const std::vector<float>& kb)
{
    const int r = (int)kern.size() - 1;
    const __m128 k0 = _mm_loadu_ps(tapAt(kb, 0));
    // one pixel, with the taps clamped to the row (or not, inside)
    auto one = [&](int i) {
        __m128 s = _mm_mul_ps(_mm_loadu_ps(in + (size_t)i * 4), k0);
        if (i >= r && i + r <= n - 1) {
            for (int k = 1; k <= r; ++k)
                s = _mm_add_ps(s, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(in + (size_t)(i - k) * 4), _mm_loadu_ps(in + (size_t)(i + k) * 4)), _mm_loadu_ps(tapAt(kb, k))));
        } else {
            for (int k = 1; k <= r; ++k) {
                const int a = std::max(0, i - k), b = std::min(n - 1, i + k);
                s = _mm_add_ps(s, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(in + (size_t)a * 4), _mm_loadu_ps(in + (size_t)b * 4)), _mm_loadu_ps(tapAt(kb, k))));
            }
        }
        _mm_storeu_ps(out + (size_t)i * 4, s);
    };
    int i = 0;
    for (; i < n && i < r; ++i) one(i);
    for (; i + 3 + r <= n - 1; i += 4) {
        __m128 s0 = _mm_mul_ps(_mm_loadu_ps(in + (size_t)i * 4), k0), s1 = _mm_mul_ps(_mm_loadu_ps(in + (size_t)(i + 1) * 4), k0);
        __m128 s2 = _mm_mul_ps(_mm_loadu_ps(in + (size_t)(i + 2) * 4), k0), s3 = _mm_mul_ps(_mm_loadu_ps(in + (size_t)(i + 3) * 4), k0);
        for (int k = 1; k <= r; ++k) {
            const float* pa = in + (size_t)(i - k) * 4;
            const float* pb = in + (size_t)(i + k) * 4;
            const __m128 w = _mm_loadu_ps(tapAt(kb, k));
            s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa), _mm_loadu_ps(pb)), w));
            s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 4), _mm_loadu_ps(pb + 4)), w));
            s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 8), _mm_loadu_ps(pb + 8)), w));
            s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 12), _mm_loadu_ps(pb + 12)), w));
        }
        _mm_storeu_ps(out + (size_t)i * 4, s0);
        _mm_storeu_ps(out + (size_t)(i + 1) * 4, s1);
        _mm_storeu_ps(out + (size_t)(i + 2) * 4, s2);
        _mm_storeu_ps(out + (size_t)(i + 3) * 4, s3);
    }
    for (; i < n; ++i) one(i);
}

// blurLine over a row of single values, four outputs at a time where the taps stay inside the row.
static void blurRowPlane(const float* in, float* out, int n, const std::vector<float>& kern, const std::vector<float>& kb)
{
    const int r = (int)kern.size() - 1;
    int i = 0;
    const __m128 k0 = _mm_loadu_ps(tapAt(kb, 0));
    for (; i < n; ) {
        if (i >= r && i + 15 + r <= n - 1) {
            // sixteen outputs at a time (four sums that do not wait for each other)
            __m128 s0 = _mm_mul_ps(_mm_loadu_ps(in + i), k0), s1 = _mm_mul_ps(_mm_loadu_ps(in + i + 4), k0);
            __m128 s2 = _mm_mul_ps(_mm_loadu_ps(in + i + 8), k0), s3 = _mm_mul_ps(_mm_loadu_ps(in + i + 12), k0);
            for (int k = 1; k <= r; ++k) {
                const float* pa = in + i - k;
                const float* pb = in + i + k;
                const __m128 w = _mm_loadu_ps(tapAt(kb, k));
                s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa), _mm_loadu_ps(pb)), w));
                s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 4), _mm_loadu_ps(pb + 4)), w));
                s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 8), _mm_loadu_ps(pb + 8)), w));
                s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 12), _mm_loadu_ps(pb + 12)), w));
            }
            _mm_storeu_ps(out + i, s0);
            _mm_storeu_ps(out + i + 4, s1);
            _mm_storeu_ps(out + i + 8, s2);
            _mm_storeu_ps(out + i + 12, s3);
            i += 16;
        } else if (i >= r && i + 3 + r <= n - 1) {
            __m128 s = _mm_mul_ps(_mm_loadu_ps(in + i), k0);
            for (int k = 1; k <= r; ++k)
                s = _mm_add_ps(s, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(in + i - k), _mm_loadu_ps(in + i + k)), _mm_loadu_ps(tapAt(kb, k))));
            _mm_storeu_ps(out + i, s);
            i += 4;
        } else {
            float s = in[i] * kern[0];
            for (int k = 1; k <= r; ++k) {
                const int a = std::max(0, i - k), b = std::min(n - 1, i + k);
                s += (in[a] + in[b]) * kern[(size_t)k];
            }
            out[i] = s;
            ++i;
        }
    }
}

// Vertical blur of 'h' rows of 'len' floats (a plane, or RGBA pixels as 4 floats each): row y of 'out' comes from rows y - r .. y + r of 'in' (edges
// clamped), four values at a time; same sums as blurLine.
static void blurRowsDown(const float* in, float* out, int len, int h, const std::vector<float>& kern, const std::vector<float>& kb)
{
    const int r = (int)kern.size() - 1;
    forRows(h, len * (int)kern.size(), [&](int y) {
        float* o = out + (size_t)y * len;
        const __m128 k0 = _mm_loadu_ps(tapAt(kb, 0));
        const float* mid = in + (size_t)y * len;
        std::vector<const float*> up((size_t)r + 1), down((size_t)r + 1);
        for (int k = 1; k <= r; ++k) {
            up[(size_t)k] = in + (size_t)std::max(0, y - k) * len;
            down[(size_t)k] = in + (size_t)std::min(h - 1, y + k) * len;
        }
        for (int x0 = 0; x0 < len; x0 += 256) {
            const int x1 = std::min(len, x0 + 256);
            int x = x0;
            // sixteen values at a time (four sums that do not wait for each other; each is the plain sum of its values)
            for (; x + 16 <= x1; x += 16) {
                __m128 s0 = _mm_mul_ps(_mm_loadu_ps(mid + x), k0), s1 = _mm_mul_ps(_mm_loadu_ps(mid + x + 4), k0);
                __m128 s2 = _mm_mul_ps(_mm_loadu_ps(mid + x + 8), k0), s3 = _mm_mul_ps(_mm_loadu_ps(mid + x + 12), k0);
                for (int k = 1; k <= r; ++k) {
                    const float* pa = up[(size_t)k] + x;
                    const float* pb = down[(size_t)k] + x;
                    const __m128 w = _mm_loadu_ps(tapAt(kb, k));
                    s0 = _mm_add_ps(s0, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa), _mm_loadu_ps(pb)), w));
                    s1 = _mm_add_ps(s1, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 4), _mm_loadu_ps(pb + 4)), w));
                    s2 = _mm_add_ps(s2, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 8), _mm_loadu_ps(pb + 8)), w));
                    s3 = _mm_add_ps(s3, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(pa + 12), _mm_loadu_ps(pb + 12)), w));
                }
                _mm_storeu_ps(o + x, s0);
                _mm_storeu_ps(o + x + 4, s1);
                _mm_storeu_ps(o + x + 8, s2);
                _mm_storeu_ps(o + x + 12, s3);
            }
            for (; x + 4 <= x1; x += 4) {
                __m128 s = _mm_mul_ps(_mm_loadu_ps(mid + x), k0);
                for (int k = 1; k <= r; ++k)
                    s = _mm_add_ps(s, _mm_mul_ps(_mm_add_ps(_mm_loadu_ps(up[(size_t)k] + x), _mm_loadu_ps(down[(size_t)k] + x)), _mm_loadu_ps(tapAt(kb, k))));
                _mm_storeu_ps(o + x, s);
            }
            for (; x < x1; ++x) {
                float s = mid[x] * kern[0];
                for (int k = 1; k <= r; ++k) s += (up[(size_t)k][x] + down[(size_t)k][x]) * kern[(size_t)k];
                o[x] = s;
            }
        }
    });
}
#endif

void gaussianBlurPlane(std::vector<float>& plane, int w, int h, double sigmaX, double sigmaY)
{
    if (w <= 0 || h <= 0 || plane.size() < (size_t)w * h) return;
    if (sigmaX > 0.3) {
        const auto k = gaussKernel(sigmaX);
#ifdef RENO_PIXROWS_SSE2
        const auto kb = spreadKernel(k);
        std::vector<float>& outp = blurScratch(plane.size());
        forRows(h, w * (int)k.size(), [&](int y) { blurRowPlane(&plane[(size_t)y * w], &outp[(size_t)y * w], w, k, kb); });
        plane.swap(outp);
        trimBlurScratch();
#else
        forRows(h, w * (int)k.size(), [&](int y) {
            std::vector<float> tmp((size_t)w);
            blurLine(&plane[(size_t)y * w], tmp.data(), w, 1, k);
            std::memcpy(&plane[(size_t)y * w], tmp.data(), sizeof(float) * (size_t)w);
        });
#endif
    }
    if (sigmaY > 0.3) {
        const auto k = gaussKernel(sigmaY);
#ifdef RENO_PIXROWS_SSE2
        const auto kb = spreadKernel(k);
        std::vector<float>& outp = blurScratch(plane.size());
        blurRowsDown(plane.data(), outp.data(), w, h, k, kb);
        plane.swap(outp);
        trimBlurScratch();
#else
        forRows(w, h * (int)k.size(), [&](int x) {
            std::vector<float> col((size_t)h), tmp((size_t)h);
            for (int y = 0; y < h; ++y) col[(size_t)y] = plane[(size_t)y * w + x];
            blurLine(col.data(), tmp.data(), h, 1, k);
            for (int y = 0; y < h; ++y) plane[(size_t)y * w + x] = tmp[(size_t)y];
        });
#endif
    }
}

void gaussianBlur(Buf& b, double sigmaX, double sigmaY, const bool channels[4])
{
    if (b.w <= 0 || b.h <= 0) return;
#ifdef RENO_PIXROWS_SSE2
    if ((!channels || (channels[0] && channels[1] && channels[2] && channels[3])) && b.px.size() >= (size_t)b.w * b.h * 4) {
        const int w = b.w, h = b.h;
        if (sigmaX > 0.3) {
            const auto k = gaussKernel(sigmaX);
            const auto kb = spreadKernel(k);
            std::vector<float>& outp = blurScratch((size_t)w * h * 4);
            forRows(h, w * (int)k.size(), [&](int y) { blurRowRGBA(&b.px[(size_t)y * w * 4], &outp[(size_t)y * w * 4], w, k, kb); });
            b.px.swap(outp);
            trimBlurScratch();
        }
        if (sigmaY > 0.3) {
            const auto k = gaussKernel(sigmaY);
            const auto kb = spreadKernel(k);
            std::vector<float>& outp = blurScratch((size_t)w * h * 4);
            blurRowsDown(b.px.data(), outp.data(), w * 4, h, k, kb);
            b.px.swap(outp);
            trimBlurScratch();
        }
        return;
    }
#endif
    for (int c = 0; c < 4; ++c) {
        if (channels && !channels[c]) continue;
        std::vector<float> plane((size_t)b.w * b.h);
        for (size_t k = 0; k < plane.size(); ++k) plane[k] = b.px[k * 4 + (size_t)c];
        gaussianBlurPlane(plane, b.w, b.h, sigmaX, sigmaY);
        for (size_t k = 0; k < plane.size(); ++k) b.px[k * 4 + (size_t)c] = plane[k];
    }
}

// VEGAS's Gaussian Blur: sigma 96 * Range + 0.45 pixels in a 320 wide frame (fitted; same across and down), never under 0.6 (even at Range 0 the picture
// softens a little: a 3 tap kernel 1/6 2/3 1/6).
// Pixelate's cells along one axis of n pixels (vertical counted from the bottom row): 'starts' holds each cell's first pixel and n at the end, K the
// averaging window. p = the setting, scale = the render scale. Arithmetic is in floats like VEGAS: where j * n / N is a whole number, the cell
// starts one pixel earlier or not by float noise.
void pixelateAxis(int n, double p, double scale, std::vector<int>& starts, int& K)
{
    starts.clear();
    if (n <= 0) { starts.push_back(0); K = 1; return; }
    if (!(p > 0)) {
        for (int i = 0; i <= n; ++i) starts.push_back(i);
        K = 1;
        return;
    }
    int N = 1;
    float S = (float)n;
    if (p < 1) {
        const float size = (float)(scale * 2.0) / (1.f - (float)p);
        N = std::min(n, (int)std::floor((float)n / size) + 1);
        S = 1.f / ((float)N / (float)n);
    }
    for (int j = 0; j < N; ++j) starts.push_back(std::min(n - 1, std::max(j ? starts.back() + 1 : 0, (int)std::floor((float)j * S))));
    starts.push_back(n);
    K = p < 1 ? std::max(1, (int)std::floor(S)) : n;
}

double gaussianBlurSigma(double range, int frameW)
{
    return std::max(0.6, 96.0 * std::max(0.0, range) + 0.45) * frameW / 320.0;
}

void gaussianBlurFrom(const Buf& src, Buf& dst, double sigmaX, double sigmaY)
{
#ifdef RENO_PIXROWS_SSE2
    if (src.w > 0 && src.h > 0 && src.px.size() >= (size_t)src.w * src.h * 4 && (sigmaX > 0.3 || sigmaY > 0.3)) {
        const int w = src.w, h = src.h;
        dst.ensure(w, h);
        const float* in = src.px.data();
        if (sigmaX > 0.3) {
            const auto k = gaussKernel(sigmaX);
            const auto kb = spreadKernel(k);
            float* out = dst.px.data();
            forRows(h, w * (int)k.size(), [&](int y) { blurRowRGBA(in + (size_t)y * w * 4, out + (size_t)y * w * 4, w, k, kb); });
            in = out;
        }
        if (sigmaY > 0.3) {
            const auto k = gaussKernel(sigmaY);
            const auto kb = spreadKernel(k);
            if (in == dst.px.data()) {
                std::vector<float>& outp = blurScratch((size_t)w * h * 4);
                blurRowsDown(in, outp.data(), w * 4, h, k, kb);
                dst.px.swap(outp);
                trimBlurScratch();
            } else {
                blurRowsDown(in, dst.px.data(), w * 4, h, k, kb);
            }
        }
        return;
    }
#endif
    dst = src;
    gaussianBlur(dst, sigmaX, sigmaY);
}

void fillThrough(Buf& b, const std::vector<float>& cover, const Color& c)
{
    if (cover.size() < (size_t)b.w * b.h) return;
    forRows(b.h, b.w, [&](int y) {
        for (int x = 0; x < b.w; ++x) {
            const float k = clamp01(cover[(size_t)y * b.w + x]) * (float)c.a;
            if (k <= 0.f) continue;
            float* p = b.at(x, y);
            p[0] = (float)c.r * k + p[0] * (1 - k);
            p[1] = (float)c.g * k + p[1] * (1 - k);
            p[2] = (float)c.b * k + p[2] * (1 - k);
            p[3] = k + p[3] * (1 - k);
        }
    });
}

}  // namespace builtin

// ------------------------------------------------------------------ the "binary"
int builtinNumberOfPlugins() { return (int)builtin::plugins().size(); }

OfxPlugin* builtinPlugin(int index)
{
    auto& v = builtin::plugins();
    return index >= 0 && index < (int)v.size() ? &v[(size_t)index] : nullptr;
}

OfxStatus builtinSetHost(const OfxHost* host)
{
    builtin::gHost = host;
    if (!host || !host->fetchSuite) return kOfxStatErrMissingHostFeature;
    builtin::gProp = (const OfxPropertySuiteV1*)host->fetchSuite(host->host, kOfxPropertySuite, 1);
    builtin::gEff = (const OfxImageEffectSuiteV1*)host->fetchSuite(host->host, kOfxImageEffectSuite, 1);
    builtin::gParam = (const OfxParameterSuiteV1*)host->fetchSuite(host->host, kOfxParameterSuite, 1);
    return builtin::gProp && builtin::gEff && builtin::gParam ? kOfxStatOK : kOfxStatErrMissingHostFeature;
}

const OfxHost* builtinHostPtr() { return builtin::gHost; }

std::vector<BuiltinInfo> builtinInfos()
{
    std::vector<BuiltinInfo> out;
    for (const auto& d : builtin::defs()) {
        BuiltinInfo i;
        i.id = d.id;
        i.label = d.label;
        i.group = d.group;
        i.generator = d.generator;
        i.filter = !d.generator && !d.transition;
        i.transition = d.transition;
        out.push_back(i);
    }
    return out;
}

// ------------------------------------------------------------------ text
static std::mutex gTextMutex;
static TextRasterizer gText;

void setTextRasterizer(TextRasterizer fn)
{
    std::lock_guard<std::mutex> lk(gTextMutex);
    gText = std::move(fn);
}

bool rasterizeText(const TextRequest& req, TextMask& out)
{
    TextRasterizer fn;
    {
        std::lock_guard<std::mutex> lk(gTextMutex);
        fn = gText;
    }
    out = TextMask();
    if (!fn) return false;
    try {
        if (!fn(req, out)) return false;
    } catch (...) {
        return false;
    }
    if (out.w <= 0 || out.h <= 0 || out.a.size() < (size_t)out.w * out.h) { out = TextMask(); return false; }
    return true;
}

// Appends a Unicode code point as UTF-8.
static void putUtf8(std::string& s, uint32_t c)
{
    if (c < 0x80) s += (char)c;
    else if (c < 0x800) { s += (char)(0xC0 | (c >> 6)); s += (char)(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { s += (char)(0xE0 | (c >> 12)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
    else { s += (char)(0xF0 | (c >> 18)); s += (char)(0x80 | ((c >> 12) & 0x3F)); s += (char)(0x80 | ((c >> 6) & 0x3F)); s += (char)(0x80 | (c & 0x3F)); }
}

// Windows-1252's characters 0x80..0x9F (the rest of the code page is Latin-1).
static uint32_t cp1252(unsigned char c)
{
    static const uint16_t hi[32] = {0x20AC, 0x81, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D, 0x017D, 0x8F,
                                    0x90, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D, 0x017E, 0x0178};
    return (c >= 0x80 && c < 0xA0) ? hi[c - 0x80] : c;
}

RichText parseRichText(const std::string& in)
{
    RichText r;
    size_t start = 0;
    while (start < in.size() && (in[start] == ' ' || in[start] == '\r' || in[start] == '\n' || in[start] == '\t')) ++start;
    if (in.compare(start, 5, "{\\rtf") != 0) {
        for (char c : in) if (c != '\r') r.text += c;
        return r;
    }
    std::map<int, std::string> fonts;
    struct State { bool skip = false; bool fontTable = false; int ucSkip = 1; };
    std::vector<State> stack(1);
    int defaultFont = 0, firstFont = -1;
    std::string fontName;
    int curFontNo = 0;
    bool sawText = false;
    int pendingSkip = 0;
    const size_t n = in.size();
    for (size_t i = start; i < n; ++i) {
        const char c = in[i];
        State& st = stack.back();
        if (c == '{') { stack.push_back(st); continue; }
        if (c == '}') {
            if (st.fontTable && !fontName.empty()) { fonts[curFontNo] = fontName; fontName.clear(); }
            if (stack.size() > 1) stack.pop_back();
            continue;
        }
        if (c == '\r' || c == '\n') continue;
        if (c == '\\') {
            if (i + 1 >= n) break;
            const char nx = in[i + 1];
            if (nx == '\\' || nx == '{' || nx == '}') {
                if (!st.skip && !st.fontTable) { if (pendingSkip > 0) --pendingSkip; else { r.text += nx; sawText = true; } }
                ++i;
                continue;
            }
            if (nx == '\'') {
                if (i + 3 < n) {
                    const unsigned v = (unsigned)std::strtoul(in.substr(i + 2, 2).c_str(), nullptr, 16);
                    if (pendingSkip > 0) --pendingSkip;
                    else if (st.fontTable) fontName += (char)v;
                    else if (!st.skip) { putUtf8(r.text, cp1252((unsigned char)v)); sawText = true; }
                }
                i += 3;
                continue;
            }
            if (nx == '*') { st.skip = true; ++i; continue; }
            if (nx == '~') { if (!st.skip && !st.fontTable) r.text += ' '; ++i; continue; }
            if (nx == '-' || nx == '_') { ++i; continue; }
            size_t j = i + 1;
            std::string word;
            while (j < n && ((in[j] >= 'a' && in[j] <= 'z') || (in[j] >= 'A' && in[j] <= 'Z'))) word += in[j++];
            bool hasNum = false;
            long num = 0;
            size_t k = j;
            if (k < n && (in[k] == '-' || (in[k] >= '0' && in[k] <= '9'))) {
                const bool neg = in[k] == '-';
                if (neg) ++k;
                size_t digits = 0;
                while (k < n && in[k] >= '0' && in[k] <= '9' && digits < 10) { num = num * 10 + (in[k] - '0'); ++k; ++digits; }
                while (k < n && in[k] >= '0' && in[k] <= '9') ++k;
                if (neg) num = -num;
                hasNum = true;
                j = k;
            }
            if (j < n && in[j] == ' ') ++j;
            i = j - 1;
            if (word.empty()) continue;
            if (word == "fonttbl") { st.fontTable = true; continue; }
            if (word == "colortbl" || word == "stylesheet" || word == "info" || word == "pict" || word == "header" || word == "footer" ||
                word == "listtable" || word == "listoverridetable" || word == "generator") { st.skip = true; continue; }
            if (st.fontTable) {
                if (word == "f" && hasNum) { if (!fontName.empty()) fonts[curFontNo] = fontName; fontName.clear(); curFontNo = (int)num; }
                continue;
            }
            if (word == "deff" && hasNum) { defaultFont = (int)num; continue; }
            if (st.skip) continue;
            if (word == "par" || word == "line") { r.text += '\n'; continue; }
            if (word == "tab") { r.text += '\t'; continue; }
            if (word == "uc" && hasNum) { st.ucSkip = (int)std::max(0L, num); continue; }
            if (word == "u" && hasNum) {
                long cp = num < 0 ? num + 65536 : num;
                putUtf8(r.text, (uint32_t)cp);
                sawText = true;
                pendingSkip = st.ucSkip;
                continue;
            }
            if (!sawText) {
                if (word == "f" && hasNum && firstFont < 0) firstFont = (int)num;
                else if (word == "fs" && hasNum && r.points <= 0) r.points = num / 2.0;
                else if (word == "b") r.bold = !hasNum || num != 0;
                else if (word == "i") r.italic = !hasNum || num != 0;
                else if (word == "ul") r.underline = !hasNum || num != 0;
                else if (word == "ulnone") r.underline = false;
            }
            continue;
        }
        if (pendingSkip > 0) { --pendingSkip; continue; }
        if (st.fontTable) {
            if (c == ';') { fonts[curFontNo] = fontName; fontName.clear(); }
            else fontName += c;
            continue;
        }
        if (st.skip) continue;
        r.text += c;
        sawText = true;
    }
    while (!r.text.empty() && (r.text.back() == '\n' || r.text.back() == ' ')) r.text.pop_back();
    auto it = fonts.find(firstFont >= 0 ? firstFont : defaultFont);
    if (it != fonts.end()) {
        std::string f = it->second;
        while (!f.empty() && (f.front() == ' ')) f.erase(f.begin());
        while (!f.empty() && (f.back() == ' ' || f.back() == ';')) f.pop_back();
        r.font = f;
    }
    return r;
}

}  // namespace ofx
}  // namespace reno
