// On-screen controls (OpenFX overlay interacts) of the built-ins in builtins_mask*.cpp: a dot to drag for Lens Correction's DistCenter and Smart Zoom's
// LocationAndZoom, and the shapes, points and curve handles of Bezier Masking's masks. They are written against the OpenFX interact and parameter suites like any
// plug-in's; OpenGL 1.x calls are looked up by name (as panels_preview.cpp does), nothing is linked.
//
// Bezier Masking edits the first mask whose Edit Mode is on. An Oval, Rectangle or Diamond is dragged (inside it) to move its
// Location. A Curve, by the Edit Tool:
//   click on an empty place     adds a point to the outline being drawn (drag to pull out its curve handles);
//   click on the first point    closes the outline;
//   drag a point or a handle    moves it (a smooth point's other handle follows);
//   Add, Split: click on a closed outline   adds a point there;       Delete: click on a point   removes it;
//   Delete / Backspace          removes the selected point;     C   makes it a corner (no handles);     S   makes it smooth;
//   Enter                       closes the outline;             Escape   lets go of the selection.
#include "builtins_mask_internal.h"
#include "ofxInteract.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

// (the name the host gives a key press's X11 key symbol under: ofxinternal.h)
#ifndef kRenoOfxPropKeySym
#define kRenoOfxPropKeySym "kOfxPropKeySym"
#endif

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  define RENO_GLCALL __stdcall
#else
#  include <dlfcn.h>
#  define RENO_GLCALL
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace {

// ------------------------------------------------------------------ OpenGL 1.x, by name
struct GL {
    void(RENO_GLCALL* Begin)(unsigned) = nullptr;
    void(RENO_GLCALL* End)() = nullptr;
    void(RENO_GLCALL* Vertex2d)(double, double) = nullptr;
    void(RENO_GLCALL* Color4f)(float, float, float, float) = nullptr;
    void(RENO_GLCALL* LineWidth)(float) = nullptr;
    void(RENO_GLCALL* Enable)(unsigned) = nullptr;
    void(RENO_GLCALL* BlendFunc)(unsigned, unsigned) = nullptr;
    bool ok = false;
};

void* glSym(const char* name)
{
#ifdef _WIN32
    static HMODULE lib = LoadLibraryA("opengl32.dll");
    return lib ? (void*)GetProcAddress(lib, name) : nullptr;
#else
    static void* lib = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    void* f = lib ? dlsym(lib, name) : nullptr;
    if (!f) {
        static void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        typedef void* (*Gpa)(const char*);
        Gpa gpa = egl ? (Gpa)dlsym(egl, "eglGetProcAddress") : nullptr;
        if (gpa) f = gpa(name);
    }
    return f;
#endif
}

GL gTestGL;

GL& gl()
{
    if (gTestGL.ok) return gTestGL;
    static GL g = [] {
        GL x;
        auto get = [](auto& fn, const char* n) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(glSym(n));
            return fn != nullptr;
        };
        bool ok = true;
        ok = get(x.Begin, "glBegin") && ok;
        ok = get(x.End, "glEnd") && ok;
        ok = get(x.Vertex2d, "glVertex2d") && ok;
        ok = get(x.Color4f, "glColor4f") && ok;
        ok = get(x.LineWidth, "glLineWidth") && ok;
        ok = get(x.Enable, "glEnable") && ok;
        ok = get(x.BlendFunc, "glBlendFunc") && ok;
        x.ok = ok;
        return x;
    }();
    return g;
}

const unsigned kLines = 0x0001, kLineStrip = 0x0003, kLineLoop = 0x0002, kQuads = 0x0007, kBlend = 0x0BE2, kSrcAlpha = 0x0302, kOneMinusSrcAlpha = 0x0303;

// ------------------------------------------------------------------ the host's suites
const OfxPropertySuiteV1* propSuite()
{
    const OfxHost* h = builtinHostPtr();
    return h && h->fetchSuite ? (const OfxPropertySuiteV1*)h->fetchSuite(h->host, kOfxPropertySuite, 1) : nullptr;
}
const OfxImageEffectSuiteV1* effSuite()
{
    const OfxHost* h = builtinHostPtr();
    return h && h->fetchSuite ? (const OfxImageEffectSuiteV1*)h->fetchSuite(h->host, kOfxImageEffectSuite, 1) : nullptr;
}
const OfxParameterSuiteV1* parSuite()
{
    const OfxHost* h = builtinHostPtr();
    return h && h->fetchSuite ? (const OfxParameterSuiteV1*)h->fetchSuite(h->host, kOfxParameterSuite, 1) : nullptr;
}

// What an interact action works on: the effect's parameters, the time, the size of the project's frame and the canonical units per screen pixel.
struct View {
    const OfxPropertySuiteV1* prop = nullptr;
    const OfxParameterSuiteV1* par = nullptr;
    OfxParamSetHandle set = nullptr;
    double time = 0, w = 1, h = 1, psx = 1, psy = 1;
    OfxParamHandle handle(const char* name) const
    {
        OfxParamHandle p = nullptr;
        return par && set && par->paramGetHandle(set, name, &p, nullptr) == kOfxStatOK ? p : nullptr;
    }
    std::string str(const char* name) const
    {
        char* v = nullptr;
        OfxParamHandle p = handle(name);
        if (!p || par->paramGetValueAtTime(p, time, &v) != kOfxStatOK || !v) return std::string();
        return v;
    }
    int integer(const char* name, int def) const
    {
        int v = def;
        OfxParamHandle p = handle(name);
        if (!p || par->paramGetValueAtTime(p, time, &v) != kOfxStatOK) return def;
        return v;
    }
    bool point(const char* name, double& x, double& y) const
    {
        OfxParamHandle p = handle(name);
        return p && par->paramGetValueAtTime(p, time, &x, &y) == kOfxStatOK;
    }
    bool point3(const char* name, double& x, double& y, double& z) const
    {
        OfxParamHandle p = handle(name);
        return p && par->paramGetValueAtTime(p, time, &x, &y, &z) == kOfxStatOK;
    }
    void setPoint3(const char* name, double x, double y, double z) const
    {
        OfxParamHandle p = handle(name);
        if (p) par->paramSetValue(p, x, y, z);
    }
    double real(const char* name, double def) const
    {
        double v = def;
        OfxParamHandle p = handle(name);
        if (!p || par->paramGetValueAtTime(p, time, &v) != kOfxStatOK) return def;
        return v;
    }
    void setStr(const char* name, const std::string& s) const
    {
        OfxParamHandle p = handle(name);
        if (p) par->paramSetValue(p, s.c_str());
    }
    void setPoint(const char* name, double x, double y) const
    {
        OfxParamHandle p = handle(name);
        if (p) par->paramSetValue(p, x, y);
    }
};

bool makeView(OfxPropertySetHandle in, View& v)
{
    v.prop = propSuite();
    v.par = parSuite();
    const OfxImageEffectSuiteV1* eff = effSuite();
    if (!v.prop || !v.par || !eff || !in) return false;
    void* fx = nullptr;
    if (v.prop->propGetPointer(in, kOfxPropEffectInstance, 0, &fx) != kOfxStatOK || !fx) return false;
    if (eff->getParamSet((OfxImageEffectHandle)fx, &v.set) != kOfxStatOK || !v.set) return false;
    v.prop->propGetDouble(in, kOfxPropTime, 0, &v.time);
    OfxPropertySetHandle ep = nullptr;
    if (eff->getPropertySet((OfxImageEffectHandle)fx, &ep) == kOfxStatOK && ep) {
        v.prop->propGetDouble(ep, kOfxImageEffectPropProjectSize, 0, &v.w);
        v.prop->propGetDouble(ep, kOfxImageEffectPropProjectSize, 1, &v.h);
    }
    if (!(v.w >= 1)) v.w = 1;
    if (!(v.h >= 1)) v.h = 1;
    v.prop->propGetDouble(in, kOfxInteractPropPixelScale, 0, &v.psx);
    v.prop->propGetDouble(in, kOfxInteractPropPixelScale, 1, &v.psy);
    if (!(v.psx > 0)) v.psx = 1;
    if (!(v.psy > 0)) v.psy = 1;
    return true;
}

void penPosition(const View& v, OfxPropertySetHandle in, double& x, double& y)
{
    x = y = 0;
    v.prop->propGetDouble(in, kOfxInteractPropPenPosition, 0, &x);
    v.prop->propGetDouble(in, kOfxInteractPropPenPosition, 1, &y);
}

void square(GL& g, double cx, double cy, double rx, double ry)
{
    g.Begin(kQuads);
    g.Vertex2d(cx - rx, cy - ry);
    g.Vertex2d(cx + rx, cy - ry);
    g.Vertex2d(cx + rx, cy + ry);
    g.Vertex2d(cx - rx, cy + ry);
    g.End();
}

// ------------------------------------------------------------------ a dot to drag
struct DotState {
    bool dragging = false;
};
std::mutex gDotMutex;
std::map<const void*, DotState> gDots;

// The dot works on a Double2D parameter (which = 0: DistCenter) or on the first two numbers of a Double3D one (which = 1: LocationAndZoom).
template <int which>
OfxStatus pointEntry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle /*out*/)
{
    const char* const kName = which == 0 ? "DistCenter" : "LocationAndZoom";
    auto read = [&](const View& v, double& x, double& y, double& z) {
        z = 0;
        if (which == 0) v.point(kName, x, y); else v.point3(kName, x, y, z);
    };
    auto write = [&](const View& v, double x, double y) {
        if (which == 0) { v.setPoint(kName, x, y); return; }
        double ox = 0, oy = 0, z = 0;
        v.point3(kName, ox, oy, z);
        v.setPoint3(kName, x, y, z);
    };
    if (!action) return kOfxStatReplyDefault;
    try {
        if (!std::strcmp(action, kOfxActionDescribe) || !std::strcmp(action, kOfxActionCreateInstance)) return kOfxStatOK;
        if (!std::strcmp(action, kOfxActionDestroyInstance)) {
            std::lock_guard<std::mutex> lk(gDotMutex);
            gDots.erase(handle);
            return kOfxStatOK;
        }
        View v;
        if (!makeView(in, v)) return kOfxStatReplyDefault;
        double nx = 0.5, ny = 0.5, nz = 0;
        read(v, nx, ny, nz);
        const double cx = nx * v.w, cy = ny * v.h;
        if (!std::strcmp(action, kOfxInteractActionDraw)) {
            GL& g = gl();
            if (!g.ok) return kOfxStatReplyDefault;
            g.Enable(kBlend);
            g.BlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
            const double lx = 14 * v.psx, ly = 14 * v.psy;
            for (int pass = 0; pass < 2; ++pass) {
                g.LineWidth(pass == 0 ? 3.f : 1.5f);
                if (pass == 0) g.Color4f(0, 0, 0, 0.6f); else g.Color4f(1.f, 1.f, 0.6f, 1.f);
                g.Begin(kLines);
                g.Vertex2d(cx - lx, cy); g.Vertex2d(cx + lx, cy);
                g.Vertex2d(cx, cy - ly); g.Vertex2d(cx, cy + ly);
                g.End();
            }
            g.Color4f(0, 0, 0, 0.6f);
            square(g, cx, cy, 5 * v.psx, 5 * v.psy);
            g.Color4f(1.f, 1.f, 0.6f, 1.f);
            square(g, cx, cy, 3.5 * v.psx, 3.5 * v.psy);
            return kOfxStatOK;
        }
        if (!std::strcmp(action, kOfxInteractActionPenDown)) {
            double x, y;
            penPosition(v, in, x, y);
            if (std::fabs(x - cx) / v.psx > 12 || std::fabs(y - cy) / v.psy > 12) return kOfxStatReplyDefault;
            std::lock_guard<std::mutex> lk(gDotMutex);
            gDots[handle].dragging = true;
            return kOfxStatOK;
        }
        if (!std::strcmp(action, kOfxInteractActionPenMotion)) {
            std::lock_guard<std::mutex> lk(gDotMutex);
            auto it = gDots.find(handle);
            if (it == gDots.end() || !it->second.dragging) return kOfxStatReplyDefault;
            double x, y;
            penPosition(v, in, x, y);
            write(v, clampd(x / v.w, 0, 1), clampd(y / v.h, 0, 1));
            return kOfxStatOK;
        }
        if (!std::strcmp(action, kOfxInteractActionPenUp)) {
            std::lock_guard<std::mutex> lk(gDotMutex);
            auto it = gDots.find(handle);
            if (it == gDots.end() || !it->second.dragging) return kOfxStatReplyDefault;
            it->second.dragging = false;
            return kOfxStatOK;
        }
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

// ------------------------------------------------------------------ Bezier Masking's points
enum Drag { None, MovePoint, MoveIn, MoveOut, PullNew, MoveShape };
struct BezState {
    int sel = -1;
    Drag drag = None;
    bool mirror = false;
    double lastX = 0, lastY = 0;
};
std::mutex gBezMutex;
std::map<const void*, BezState> gBez;

// A mask's placement, from its settings (a curve is also scaled by AnchorScale; the other shapes are sized by Width and Height).
MaskXf placement(const View& v, int mask, bool curve)
{
    MaskXf xf;
    xf.W = v.w;
    xf.H = v.h;
    v.point(maskParam("Location", mask).c_str(), xf.lx, xf.ly);
    xf.ang = v.real(maskParam("Angle", mask).c_str(), 0) * kPi / 180.0;
    if (curve) v.point(maskParam("AnchorScale", mask).c_str(), xf.sx, xf.sy);
    return xf;
}

// The mask's outline as it is on the picture (false: none yet). 'curve' says whether it is a Curve-type mask whose points are kept.
bool loadPath(const View& v, int mask, BezPath& p, bool* curve = nullptr)
{
    p.pts.clear();
    p.closed = true;
    const int type = std::max(0, std::min(3, v.integer(maskParam("Type", mask).c_str(), 0)));
    if (curve) *curve = type == 3;
    if (type == 3) {
        BezPath raw;
        if (!parseBezPath(v.str(maskParam("AnchorList", mask).c_str()), raw)) return false;
        p = xfPath(raw, placement(v, mask, true), false);
        return true;
    }
    const double w = v.real(maskParam("Width", mask).c_str(), 0.3), h = v.real(maskParam("Height", mask).c_str(), 0.3);
    p = xfPath(shapeBez(type, w, h), placement(v, mask, false), false);
    return true;
}
// A Curve mask's outline back into its settings (as the points sit before the placement).
void savePath(const View& v, int mask, const BezPath& p)
{
    v.setStr(maskParam("AnchorList", mask).c_str(), p.pts.empty() ? std::string() : formatBezPath(xfPath(p, placement(v, mask, true), true)));
}

bool hasIn(const BezPoint& b) { return std::fabs(b.ix - b.x) + std::fabs(b.iy - b.y) > 1e-9; }
bool hasOut(const BezPoint& b) { return std::fabs(b.ox - b.x) + std::fabs(b.oy - b.y) > 1e-9; }

// Whether the handles of a point run opposite to each other along a line (a smooth point).
bool isSmooth(const BezPoint& b, double w, double h)
{
    if (!hasIn(b) || !hasOut(b)) return false;
    const double ax = (b.ix - b.x) * w, ay = (b.iy - b.y) * h, bx = (b.ox - b.x) * w, by = (b.oy - b.y) * h;
    const double la = std::hypot(ax, ay), lb = std::hypot(bx, by);
    if (la < 1e-9 || lb < 1e-9) return false;
    return (ax * bx + ay * by) / (la * lb) < -0.98;
}

// Distance (in screen pixels) of a place from a point.
double screenDist(const View& v, double x, double y, double px, double py) { return std::hypot((x - px) * v.w / v.psx, (y - py) * v.h / v.psy); }

void drawPoly(GL& g, const View& v, const BezPath& p, float r, float gr, float b, float a, float width)
{
    std::vector<double> xy;
    flattenBezPath(p, v.w, v.h, xy);
    if (xy.size() < 4) return;
    g.LineWidth(width);
    g.Color4f(r, gr, b, a);
    g.Begin(p.closed && p.pts.size() >= 3 ? kLineLoop : kLineStrip);
    for (size_t i = 0; i + 1 < xy.size(); i += 2) g.Vertex2d(xy[i], v.h - xy[i + 1]);
    g.End();
}

OfxStatus bezierEntry(const char* action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle /*out*/)
{
    if (!action) return kOfxStatReplyDefault;
    try {
        if (!std::strcmp(action, kOfxActionDescribe) || !std::strcmp(action, kOfxActionCreateInstance)) return kOfxStatOK;
        if (!std::strcmp(action, kOfxActionDestroyInstance)) {
            std::lock_guard<std::mutex> lk(gBezMutex);
            gBez.erase(handle);
            return kOfxStatOK;
        }
        View v;
        if (!makeView(in, v)) return kOfxStatReplyDefault;
        int active = -1;
        for (int m = 0; m < 5 && active < 0; ++m)
            if (v.integer(maskParam("EditMode", m).c_str(), 0) != 0) active = m;
        const bool editing = active >= 0;
        std::lock_guard<std::mutex> lk(gBezMutex);
        BezState& st = gBez[handle];
        BezPath path;
        bool curve = false;
        const bool have = editing && loadPath(v, active, path, &curve);
        const int tool = editing ? std::max(0, std::min(3, v.integer(maskParam("EditTool", active).c_str(), 0))) : 0;

        if (!std::strcmp(action, kOfxInteractActionDraw)) {
            GL& g = gl();
            if (!g.ok || !editing) return kOfxStatReplyDefault;
            g.Enable(kBlend);
            g.BlendFunc(kSrcAlpha, kOneMinusSrcAlpha);
            for (int m = 0; m < 5; ++m) {
                if (m == active || v.integer(maskParam("Enable", m).c_str(), 0) == 0) continue;
                BezPath other;
                if (loadPath(v, m, other) && other.pts.size() >= 2) drawPoly(g, v, other, 1.f, 1.f, 1.f, 0.45f, 1.f);
            }
            if (!have) return kOfxStatOK;
            drawPoly(g, v, path, 0.f, 0.f, 0.f, 0.55f, 3.5f);
            drawPoly(g, v, path, 1.f, 0.9f, 0.2f, 1.f, 1.5f);
            if (!curve) return kOfxStatOK;
            const double rx = 4 * v.psx, ry = 4 * v.psy;
            g.LineWidth(1.f);
            for (size_t i = 0; i < path.pts.size(); ++i) {
                const BezPoint& b = path.pts[i];
                g.Color4f(0.7f, 0.9f, 1.f, 0.9f);
                g.Begin(kLines);
                if (hasIn(b)) { g.Vertex2d(b.x * v.w, b.y * v.h); g.Vertex2d(b.ix * v.w, b.iy * v.h); }
                if (hasOut(b)) { g.Vertex2d(b.x * v.w, b.y * v.h); g.Vertex2d(b.ox * v.w, b.oy * v.h); }
                g.End();
                if (hasIn(b)) square(g, b.ix * v.w, b.iy * v.h, 3 * v.psx, 3 * v.psy);
                if (hasOut(b)) square(g, b.ox * v.w, b.oy * v.h, 3 * v.psx, 3 * v.psy);
            }
            for (size_t i = 0; i < path.pts.size(); ++i) {
                const BezPoint& b = path.pts[i];
                const bool sel = (int)i == st.sel;
                g.Color4f(0, 0, 0, 0.8f);
                square(g, b.x * v.w, b.y * v.h, rx + 1.5 * v.psx, ry + 1.5 * v.psy);
                if (sel) g.Color4f(1.f, 0.35f, 0.2f, 1.f);
                else if (i == 0 && !path.closed) g.Color4f(0.3f, 1.f, 0.4f, 1.f);
                else g.Color4f(1.f, 1.f, 1.f, 1.f);
                square(g, b.x * v.w, b.y * v.h, rx, ry);
            }
            return kOfxStatOK;
        }

        if (!editing) return kOfxStatReplyDefault;

        if (!std::strcmp(action, kOfxInteractActionPenDown)) {
            double x, y;
            penPosition(v, in, x, y);
            const double nx = clampd(x / v.w, -0.5, 1.5), ny = clampd(y / v.h, -0.5, 1.5);
            st.lastX = nx;
            st.lastY = ny;
            const double kHit = 8;
            if (!curve) {
                std::vector<double> xy;
                flattenBezPath(path, v.w, v.h, xy);
                bool inside = false;
                const double qx = nx * v.w, qy = v.h - ny * v.h;
                for (size_t i = 0, n = xy.size() / 2, j = n ? n - 1 : 0; i < n; j = i++) {
                    const double xi = xy[2 * i], yi = xy[2 * i + 1], xj = xy[2 * j], yj = xy[2 * j + 1];
                    if (((yi > qy) != (yj > qy)) && qx < (xj - xi) * (qy - yi) / (yj - yi) + xi) inside = !inside;
                }
                if (!inside) return kOfxStatReplyDefault;
                st.drag = MoveShape;
                return kOfxStatOK;
            }
            if (have && st.sel >= 0 && st.sel < (int)path.pts.size()) {
                const BezPoint& b = path.pts[(size_t)st.sel];
                if (hasOut(b) && screenDist(v, nx, ny, b.ox, b.oy) < kHit) { st.drag = MoveOut; st.mirror = isSmooth(b, v.w, v.h); return kOfxStatOK; }
                if (hasIn(b) && screenDist(v, nx, ny, b.ix, b.iy) < kHit) { st.drag = MoveIn; st.mirror = isSmooth(b, v.w, v.h); return kOfxStatOK; }
            }
            if (have) {
                for (size_t i = 0; i < path.pts.size(); ++i) {
                    if (screenDist(v, nx, ny, path.pts[i].x, path.pts[i].y) >= kHit) continue;
                    if (i == 0 && !path.closed && path.pts.size() >= 3) {
                        path.closed = true;
                        savePath(v, active, path);
                        st.sel = 0;
                        st.drag = None;
                        return kOfxStatOK;
                    }
                    if (tool == 2) {
                        path.pts.erase(path.pts.begin() + (long)i);
                        st.sel = std::min(st.sel, (int)path.pts.size() - 1);
                        st.drag = None;
                        savePath(v, active, path);
                        return kOfxStatOK;
                    }
                    st.sel = (int)i;
                    st.drag = MovePoint;
                    return kOfxStatOK;
                }
            }
            if (have && path.closed) {
                const size_t n = path.pts.size();
                double bestD = 6;
                int bestSeg = -1;
                double bestT = 0;
                for (size_t i = 0; i < n; ++i) {
                    const BezPoint& a = path.pts[i];
                    const BezPoint& b = path.pts[(i + 1) % n];
                    for (int k = 1; k < 40; ++k) {
                        const double t = k / 40.0, u = 1 - t;
                        const double px = u * u * u * a.x + 3 * u * u * t * a.ox + 3 * u * t * t * b.ix + t * t * t * b.x;
                        const double py = u * u * u * a.y + 3 * u * u * t * a.oy + 3 * u * t * t * b.iy + t * t * t * b.y;
                        const double d = screenDist(v, nx, ny, px, py);
                        if (d < bestD) { bestD = d; bestSeg = (int)i; bestT = t; }
                    }
                }
                if (bestSeg >= 0 && (tool == 1 || tool == 3)) {
                    BezPoint& a = path.pts[(size_t)bestSeg];
                    BezPoint& b = path.pts[((size_t)bestSeg + 1) % n];
                    const double t = bestT;
                    auto lerp2 = [&](double x0, double y0, double x1, double y1, double& ox, double& oy) { ox = x0 + (x1 - x0) * t; oy = y0 + (y1 - y0) * t; };
                    BezPoint s;
                    const bool straight = !hasOut(a) && !hasIn(b);
                    if (straight) {
                        lerp2(a.x, a.y, b.x, b.y, s.x, s.y);
                        s.ix = s.ox = s.x;
                        s.iy = s.oy = s.y;
                    } else {
                        double q0x, q0y, q1x, q1y, q2x, q2y, r0x, r0y, r1x, r1y;
                        lerp2(a.x, a.y, a.ox, a.oy, q0x, q0y);
                        lerp2(a.ox, a.oy, b.ix, b.iy, q1x, q1y);
                        lerp2(b.ix, b.iy, b.x, b.y, q2x, q2y);
                        lerp2(q0x, q0y, q1x, q1y, r0x, r0y);
                        lerp2(q1x, q1y, q2x, q2y, r1x, r1y);
                        lerp2(r0x, r0y, r1x, r1y, s.x, s.y);
                        s.ix = r0x; s.iy = r0y; s.ox = r1x; s.oy = r1y;
                        a.ox = q0x; a.oy = q0y;
                        b.ix = q2x; b.iy = q2y;
                    }
                    path.pts.insert(path.pts.begin() + bestSeg + 1, s);
                    st.sel = bestSeg + 1;
                    st.drag = MovePoint;
                    savePath(v, active, path);
                    return kOfxStatOK;
                }
                // a click on nothing, in a finished outline: nothing selected
                st.sel = -1;
                st.drag = None;
                return kOfxStatReplyDefault;
            }
            if (!have) { path.pts.clear(); path.closed = false; }
            BezPoint b;
            b.x = b.ix = b.ox = nx;
            b.y = b.iy = b.oy = ny;
            path.pts.push_back(b);
            st.sel = (int)path.pts.size() - 1;
            st.drag = PullNew;
            savePath(v, active, path);
            return kOfxStatOK;
        }

        if (!std::strcmp(action, kOfxInteractActionPenMotion)) {
            if (st.drag == MoveShape) {
                double x, y;
                penPosition(v, in, x, y);
                const double nx = clampd(x / v.w, -0.5, 1.5), ny = clampd(y / v.h, -0.5, 1.5);
                const std::string loc = maskParam("Location", active);
                double lx = 0.5, ly = 0.5;
                v.point(loc.c_str(), lx, ly);
                v.setPoint(loc.c_str(), lx + (nx - st.lastX), ly + (ny - st.lastY));
                st.lastX = nx;
                st.lastY = ny;
                return kOfxStatOK;
            }
            if (st.drag == None || !have || st.sel < 0 || st.sel >= (int)path.pts.size()) return kOfxStatReplyDefault;
            double x, y;
            penPosition(v, in, x, y);
            const double nx = clampd(x / v.w, -0.5, 1.5), ny = clampd(y / v.h, -0.5, 1.5);
            BezPoint& b = path.pts[(size_t)st.sel];
            if (st.drag == MovePoint) {
                const double dx = nx - st.lastX, dy = ny - st.lastY;
                b.x += dx; b.y += dy; b.ix += dx; b.iy += dy; b.ox += dx; b.oy += dy;
            } else if (st.drag == PullNew) {
                b.ox = nx; b.oy = ny;
                b.ix = 2 * b.x - nx; b.iy = 2 * b.y - ny;
            } else {
                const bool out = st.drag == MoveOut;
                double& hx = out ? b.ox : b.ix;
                double& hy = out ? b.oy : b.iy;
                double& ox = out ? b.ix : b.ox;                  // the other handle
                double& oy = out ? b.iy : b.oy;
                const double olen = std::hypot((ox - b.x) * v.w, (oy - b.y) * v.h);
                hx = nx; hy = ny;
                if (st.mirror) {
                    const double dx = (b.x - nx) * v.w, dy = (b.y - ny) * v.h, l = std::hypot(dx, dy);
                    if (l > 1e-9) { ox = b.x + dx / l * olen / v.w; oy = b.y + dy / l * olen / v.h; }
                }
            }
            st.lastX = nx;
            st.lastY = ny;
            savePath(v, active, path);
            return kOfxStatOK;
        }

        if (!std::strcmp(action, kOfxInteractActionPenUp)) {
            if (st.drag == None) return kOfxStatReplyDefault;
            st.drag = None;
            return kOfxStatOK;
        }

        if (!std::strcmp(action, kOfxInteractActionKeyDown) || !std::strcmp(action, kOfxInteractActionKeyRepeat)) {
            if (!curve) return kOfxStatReplyDefault;
            int sym = 0;
            v.prop->propGetInt(in, kRenoOfxPropKeySym, 0, &sym);
            const bool isDown = !std::strcmp(action, kOfxInteractActionKeyDown);
            if ((sym == 0xffff || sym == 0xff08) && have && st.sel < 0) {          // nothing selected: the whole outline goes
                v.setStr(maskParam("AnchorList", active).c_str(), std::string());
                st.drag = None;
                return kOfxStatOK;
            }
            if ((sym == 0xffff || sym == 0xff08) && have && st.sel >= 0 && st.sel < (int)path.pts.size()) {
                path.pts.erase(path.pts.begin() + st.sel);
                st.sel = std::min(st.sel, (int)path.pts.size() - 1);
                st.drag = None;
                savePath(v, active, path);
                return kOfxStatOK;
            }
            if (!isDown) return kOfxStatReplyDefault;
            if ((sym == 0xff0d || sym == 0xff8d) && have && !path.closed && path.pts.size() >= 3) {
                path.closed = true;
                savePath(v, active, path);
                return kOfxStatOK;
            }
            if (sym == 0xff1b) { st.sel = -1; st.drag = None; return kOfxStatOK; }
            if ((sym == 'c' || sym == 'C' || sym == 's' || sym == 'S') && have && st.sel >= 0 && st.sel < (int)path.pts.size()) {
                const size_t n = path.pts.size();
                BezPoint& b = path.pts[(size_t)st.sel];
                if (sym == 'c' || sym == 'C') {
                    b.ix = b.ox = b.x;
                    b.iy = b.oy = b.y;
                } else if (n >= 3) {
                    const BezPoint& prev = path.pts[((size_t)st.sel + n - 1) % n];
                    const BezPoint& next = path.pts[((size_t)st.sel + 1) % n];
                    const double tx = (next.x - prev.x) / 6.0, ty = (next.y - prev.y) / 6.0;
                    b.ix = b.x - tx; b.iy = b.y - ty;
                    b.ox = b.x + tx; b.oy = b.y + ty;
                }
                savePath(v, active, path);
                return kOfxStatOK;
            }
        }
    } catch (...) {
        return kOfxStatFailed;
    }
    return kOfxStatReplyDefault;
}

}  // namespace

OfxPluginEntryPoint* maskOverlayPoint(const char* param)
{
    return std::strcmp(param, "LocationAndZoom") == 0 ? &pointEntry<1> : &pointEntry<0>;
}

OfxPluginEntryPoint* maskOverlayBezier() { return &bezierEntry; }

#ifndef _WIN32
void maskOverlayUseGL(const MaskOverlayGL* g)
{
    gTestGL = GL();
    if (!g) return;
    gTestGL.Begin = g->Begin;
    gTestGL.End = g->End;
    gTestGL.Vertex2d = g->Vertex2d;
    gTestGL.Color4f = g->Color4f;
    gTestGL.LineWidth = g->LineWidth;
    gTestGL.Enable = g->Enable;
    gTestGL.BlendFunc = g->BlendFunc;
    gTestGL.ok = true;
}
#endif

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
