// glfx - a test OpenFX plug-in for Reno's OpenGL render suite (tests/ofx/gputest.cpp).
//
// "Test GL Tint" multiplies the red of its source by the Red setting. With OpenGL (kOfxImageEffectPropOpenGLEnabled 1
// in the render arguments) it loads the source as a texture and draws a textured quad into the frame buffer the host
// bound, with the fixed-function pipeline; without, it works on the pictures in memory. Both give the same picture.
// It logs what the host did and when (glfx_event). Environment variables make it misbehave:
//   GLFX_NOGL        it says it cannot render with OpenGL ("false")
//   GLFX_NEEDED      it says "needed" (when the host has OpenGL)
//   GLFX_FAIL_ATTACH ContextAttached fails
//   GLFX_FAIL_RENDER the render fails when it is asked to use OpenGL
//   GLFX_CRASH_GL    the render crashes (a null write) when asked to use OpenGL; only with GLFX_ALLOW_CRASH
//   GLFX_NO_FREE     it does not free its textures (the host frees them)
//   GLFX_NO_LOAD_OUT it does not ask for the output texture
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "ofxProperty.h"
#include "ofxGPURender.h"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static OfxHost* gHost = nullptr;
static const OfxPropertySuiteV1* gProp = nullptr;
static const OfxImageEffectSuiteV1* gEff = nullptr;
static const OfxParameterSuiteV1* gParam = nullptr;
static const OfxImageEffectOpenGLRenderSuiteV1* gGL = nullptr;
static std::string gHostGL;

static std::mutex gMu;
static std::vector<std::string> gEvents;
static int gAttach = 0, gDetach = 0, gGlRenders = 0, gCpuRenders = 0;
static void event(const char* f, ...)
{
    char b[400];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    std::lock_guard<std::mutex> lk(gMu);
    gEvents.push_back(b);
}

// ---- OpenGL, loaded from the system library (the host made the context current; this effect calls what it needs)
typedef unsigned int GLenum;
typedef unsigned int GLuint;
typedef int GLint;
typedef int GLsizei;
typedef float GLfloat;
typedef double GLdouble;
typedef unsigned int GLbitfield;
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_ENV 0x2300
#define GL_TEXTURE_ENV_MODE 0x2200
#define GL_MODULATE 0x2100
#define GL_QUADS 0x0007
#define GL_PROJECTION 0x1701
#define GL_MODELVIEW 0x1700
#define GL_BLEND 0x0BE2
#define GL_DEPTH_TEST 0x0B71
#define GL_LIGHTING 0x0B50
#define GL_SCISSOR_TEST 0x0C11
#define GL_ALPHA_TEST 0x0BC0
#define GL_CULL_FACE 0x0B44
#define GL_TEXTURE0 0x84C0
#define GL_NO_ERROR 0
struct GLApi {
    void (*Enable)(GLenum) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    void (*BindTexture)(GLenum, GLuint) = nullptr;
    void (*TexEnvi)(GLenum, GLenum, GLint) = nullptr;
    void (*Begin)(GLenum) = nullptr;
    void (*End)() = nullptr;
    void (*TexCoord2f)(GLfloat, GLfloat) = nullptr;
    void (*Vertex2f)(GLfloat, GLfloat) = nullptr;
    void (*Color4f)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*MatrixMode)(GLenum) = nullptr;
    void (*LoadIdentity)() = nullptr;
    void (*Ortho)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = nullptr;
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*ActiveTexture)(GLenum) = nullptr;
    void (*Flush)() = nullptr;
    GLenum (*GetError)() = nullptr;
    bool ok = false;
};
static GLApi gl;
static void* (*gEglCurrent)() = nullptr;
static void loadGL()
{
    if (gl.ok) return;
    void* h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    void* e = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_GLOBAL);
    if (e) gEglCurrent = (void* (*)())dlsym(e, "eglGetCurrentContext");
    if (!h) return;
#define S(m, n) *(void**)&gl.m = dlsym(h, "gl" n)
    S(Enable, "Enable"); S(Disable, "Disable"); S(BindTexture, "BindTexture"); S(TexEnvi, "TexEnvi"); S(Begin, "Begin"); S(End, "End");
    S(TexCoord2f, "TexCoord2f"); S(Vertex2f, "Vertex2f"); S(Color4f, "Color4f"); S(MatrixMode, "MatrixMode"); S(LoadIdentity, "LoadIdentity");
    S(Ortho, "Ortho"); S(Viewport, "Viewport"); S(ActiveTexture, "ActiveTexture"); S(Flush, "Flush"); S(GetError, "GetError");
#undef S
    gl.ok = gl.Enable && gl.BindTexture && gl.Begin && gl.End && gl.Ortho && gl.Viewport && gl.ActiveTexture && gl.Color4f;
}

struct Inst {
    OfxImageClipHandle src = nullptr, dst = nullptr;
    OfxParamHandle red = nullptr;
    std::vector<void*> attachedCtx;      // every context the host attached it to
    bool attachedTo(void* c) const { for (void* x : attachedCtx) if (x == c) return true; return false; }
};

static int getInt(OfxPropertySetHandle h, const char* n) { int v = 0; gProp->propGetInt(h, n, 0, &v); return v; }

static OfxStatus renderCPU(Inst* d, OfxPropertySetHandle in, double t, double red)
{
    OfxPropertySetHandle si = nullptr, so = nullptr;
    if (gEff->clipGetImage(d->src, t, nullptr, &si) != kOfxStatOK) return kOfxStatFailed;
    if (gEff->clipGetImage(d->dst, t, nullptr, &so) != kOfxStatOK) { gEff->clipReleaseImage(si); return kOfxStatFailed; }
    void *sd = nullptr, *od = nullptr;
    int sb[4] = {0}, ob[4] = {0}, srb = 0, orb = 0, win[4] = {0};
    char *depth = nullptr, *order = nullptr;
    gProp->propGetPointer(si, kOfxImagePropData, 0, &sd);
    gProp->propGetPointer(so, kOfxImagePropData, 0, &od);
    gProp->propGetIntN(si, kOfxImagePropBounds, 4, sb);
    gProp->propGetIntN(so, kOfxImagePropBounds, 4, ob);
    gProp->propGetInt(si, kOfxImagePropRowBytes, 0, &srb);
    gProp->propGetInt(so, kOfxImagePropRowBytes, 0, &orb);
    gProp->propGetString(so, kOfxImageEffectPropPixelDepth, 0, &depth);
    gProp->propGetString(so, "OfxImageEffectPropPixelOrder", 0, &order);
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, win);
    const bool flt = depth && !strcmp(depth, kOfxBitDepthFloat);
    const bool bgra = order && !strcmp(order, "OfxImagePixelOrderBGRA");
    const int bpp = flt ? 16 : 4;
    const int ri = (!flt && bgra) ? 2 : 0;
    for (int y = win[1]; y < win[3]; ++y)
        for (int x = win[0]; x < win[2]; ++x) {
            const uint8_t* p = (const uint8_t*)sd + (ptrdiff_t)(y - sb[1]) * srb + (ptrdiff_t)(x - sb[0]) * bpp;
            uint8_t* q = (uint8_t*)od + (ptrdiff_t)(y - ob[1]) * orb + (ptrdiff_t)(x - ob[0]) * bpp;
            memcpy(q, p, (size_t)bpp);
            if (flt) ((float*)q)[0] = (float)(((const float*)p)[0] * red);
            else q[ri] = (uint8_t)std::lround(p[ri] * red);
        }
    gEff->clipReleaseImage(si);
    gEff->clipReleaseImage(so);
    return kOfxStatOK;
}

static OfxStatus renderGL(Inst* d, OfxPropertySetHandle in, double t, double red)
{
    if (!gGL || !gl.ok) return kOfxStatFailed;
    if (std::getenv("GLFX_FAIL_RENDER")) return kOfxStatFailed;
#ifdef GLFX_ALLOW_CRASH
    if (std::getenv("GLFX_CRASH_GL")) *(volatile int*)nullptr = 1;
#endif
    int win[4] = {0};
    gProp->propGetIntN(in, kOfxImageEffectPropRenderWindow, 4, win);
    OfxPropertySetHandle st = nullptr, dt = nullptr;
    if (gGL->clipLoadTexture(d->src, t, nullptr, nullptr, &st) != kOfxStatOK) return kOfxStatFailed;
    if (!std::getenv("GLFX_NO_LOAD_OUT") && gGL->clipLoadTexture(d->dst, t, nullptr, nullptr, &dt) != kOfxStatOK) return kOfxStatFailed;
    const int idx = getInt(st, kOfxImageEffectPropOpenGLTextureIndex), target = getInt(st, kOfxImageEffectPropOpenGLTextureTarget);
    if (target != GL_TEXTURE_2D || idx <= 0) return kOfxStatFailed;
    gl.Viewport(win[0], win[1], win[2] - win[0], win[3] - win[1]);
    gl.MatrixMode(GL_PROJECTION); gl.LoadIdentity(); gl.Ortho(0, 1, 0, 1, -1, 1);
    gl.MatrixMode(GL_MODELVIEW); gl.LoadIdentity();
    gl.Disable(GL_BLEND); gl.Disable(GL_DEPTH_TEST); gl.Disable(GL_LIGHTING); gl.Disable(GL_SCISSOR_TEST); gl.Disable(GL_CULL_FACE); gl.Disable(GL_ALPHA_TEST);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.Enable(GL_TEXTURE_2D);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)idx);
    gl.TexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    gl.Color4f((GLfloat)red, 1.f, 1.f, 1.f);
    gl.Begin(GL_QUADS);
    gl.TexCoord2f(0, 0); gl.Vertex2f(0, 0);
    gl.TexCoord2f(1, 0); gl.Vertex2f(1, 0);
    gl.TexCoord2f(1, 1); gl.Vertex2f(1, 1);
    gl.TexCoord2f(0, 1); gl.Vertex2f(0, 1);
    gl.End();
    gl.BindTexture(GL_TEXTURE_2D, 0);
    gl.Disable(GL_TEXTURE_2D);
    gl.Flush();
    if (!std::getenv("GLFX_NO_FREE")) {
        gGL->clipFreeTexture(st);
        if (dt) gGL->clipFreeTexture(dt);
    }
    return kOfxStatOK;
}

static OfxStatus action(const char* a, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle)
{
    OfxImageEffectHandle e = (OfxImageEffectHandle)handle;
    if (!strcmp(a, kOfxActionLoad)) {
        gProp = (const OfxPropertySuiteV1*)gHost->fetchSuite(gHost->host, kOfxPropertySuite, 1);
        gEff = (const OfxImageEffectSuiteV1*)gHost->fetchSuite(gHost->host, kOfxImageEffectSuite, 1);
        gParam = (const OfxParameterSuiteV1*)gHost->fetchSuite(gHost->host, kOfxParameterSuite, 1);
        gGL = (const OfxImageEffectOpenGLRenderSuiteV1*)gHost->fetchSuite(gHost->host, kOfxOpenGLRenderSuite, 1);
        char* v = nullptr;
        if (gProp->propGetString(gHost->host, kOfxImageEffectPropOpenGLRenderSupported, 0, &v) == kOfxStatOK && v) gHostGL = v;
        loadGL();
        event("load host=%s suite=%s", gHostGL.c_str(), gGL ? "yes" : "no");
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxActionDescribe)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        gProp->propSetString(p, kOfxPropLabel, 0, "Test GL Tint");
        gProp->propSetString(p, kOfxImageEffectPluginPropGrouping, 0, "Reno Test");
        gProp->propSetString(p, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthByte);
        gProp->propSetString(p, kOfxImageEffectPropSupportedPixelDepths, 1, kOfxBitDepthFloat);
        gProp->propSetString(p, kOfxImageEffectPluginRenderThreadSafety, 0, kOfxImageEffectRenderFullySafe);
        gProp->propSetInt(p, kOfxImageEffectPropSupportsMultiResolution, 0, 1);
        const char* mode = std::getenv("GLFX_NOGL") ? "false" : (std::getenv("GLFX_NEEDED") ? "needed" : "true");
        // (it says it can when the host does; one that needs OpenGL says so whatever the host says)
        if (strcmp(mode, "false") != 0 && (gHostGL == "true" || strcmp(mode, "needed") == 0)) gProp->propSetString(p, kOfxImageEffectPropOpenGLRenderSupported, 0, mode);
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxImageEffectActionDescribeInContext)) {
        OfxPropertySetHandle cp;
        gEff->clipDefine(e, kOfxImageEffectSimpleSourceClipName, &cp);
        gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        gEff->clipDefine(e, kOfxImageEffectOutputClipName, &cp);
        gProp->propSetString(cp, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);
        OfxParamSetHandle ps;
        gEff->getParamSet(e, &ps);
        OfxPropertySetHandle pp;
        gParam->paramDefine(ps, kOfxParamTypeDouble, "Red", &pp);
        gProp->propSetDouble(pp, kOfxParamPropDefault, 0, 0.5);
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxActionCreateInstance)) {
        Inst* d = new Inst();
        OfxParamSetHandle ps;
        gEff->getParamSet(e, &ps);
        gEff->clipGetHandle(e, kOfxImageEffectSimpleSourceClipName, &d->src, nullptr);
        gEff->clipGetHandle(e, kOfxImageEffectOutputClipName, &d->dst, nullptr);
        gParam->paramGetHandle(ps, "Red", &d->red, nullptr);
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        gProp->propSetPointer(p, kOfxPropInstanceData, 0, d);
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxActionDestroyInstance)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* d = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &d);
        delete (Inst*)d;
        event("destroy %p", handle);
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxActionOpenGLContextAttached)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* dp = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &dp);
        Inst* d = (Inst*)dp;
        void* ctx = gEglCurrent ? gEglCurrent() : nullptr;
        event("attach %p ctx=%p", handle, ctx);
        {
            std::lock_guard<std::mutex> lk(gMu);
            ++gAttach;
        }
        if (std::getenv("GLFX_FAIL_ATTACH")) return kOfxStatFailed;
        if (d) d->attachedCtx.push_back(ctx);
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxActionOpenGLContextDetached)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* dp = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &dp);
        Inst* d = (Inst*)dp;
        void* ctx = gEglCurrent ? gEglCurrent() : nullptr;
        event("detach %p ctx=%p%s", handle, ctx, (d && !d->attachedTo(ctx)) ? " WRONG-CONTEXT" : "");
        std::lock_guard<std::mutex> lk(gMu);
        ++gDetach;
        return kOfxStatOK;
    }
    if (!strcmp(a, kOfxImageEffectActionRender)) {
        OfxPropertySetHandle p;
        gEff->getPropertySet(e, &p);
        void* dp = nullptr;
        gProp->propGetPointer(p, kOfxPropInstanceData, 0, &dp);
        Inst* d = (Inst*)dp;
        if (!d) return kOfxStatFailed;
        double t = 0, red = 0.5;
        gProp->propGetDouble(in, kOfxPropTime, 0, &t);
        gParam->paramGetValueAtTime(d->red, t, &red);
        const int useGL = getInt(in, kOfxImageEffectPropOpenGLEnabled);
        void* ctx = gEglCurrent ? gEglCurrent() : nullptr;
        if (useGL) {
            event("render gl ctx=%s%s", ctx ? "current" : "NONE", !d->attachedTo(ctx) ? " OTHER-CONTEXT" : "");
            {
                std::lock_guard<std::mutex> lk(gMu);
                ++gGlRenders;
            }
            return renderGL(d, in, t, red);
        }
        event("render cpu");
        {
            std::lock_guard<std::mutex> lk(gMu);
            ++gCpuRenders;
        }
        return renderCPU(d, in, t, red);
    }
    return kOfxStatReplyDefault;
}

static void setHostFn(OfxHost* h) { gHost = h; }
static OfxPlugin gPlugins[] = {{kOfxImageEffectPluginApi, 1, "reno.test.gltint", 1, 0, setHostFn, action}};

extern "C" {
OfxExport int OfxGetNumberOfPlugins(void) { return 1; }
OfxExport OfxPlugin* OfxGetPlugin(int n) { return n == 0 ? &gPlugins[0] : nullptr; }
OfxExport int glfx_event_count(void) { std::lock_guard<std::mutex> lk(gMu); return (int)gEvents.size(); }
OfxExport const char* glfx_event(int i) { std::lock_guard<std::mutex> lk(gMu); return (i >= 0 && i < (int)gEvents.size()) ? gEvents[(size_t)i].c_str() : nullptr; }
OfxExport void glfx_clear(void) { std::lock_guard<std::mutex> lk(gMu); gEvents.clear(); gAttach = gDetach = gGlRenders = gCpuRenders = 0; }
OfxExport void glfx_counts(int* o) { std::lock_guard<std::mutex> lk(gMu); o[0] = gAttach; o[1] = gDetach; o[2] = gGlRenders; o[3] = gCpuRenders; }
}
