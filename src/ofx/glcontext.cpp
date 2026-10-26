#include "glcontext.h"
#include "../core/base.h"
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <condition_variable>
#else
#  include <dlfcn.h>
#endif

namespace reno {
namespace ofx {
namespace gl {

void (*detachHook)(Instance*) = nullptr;

// ====================================================================== the platform
#ifdef _WIN32
// WGL: a hidden window of our own per context (never shown), a pixel format, wglCreateContext (the highest version the driver gives that still has everything old effects use: a compatibility context).
struct Context::Impl {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
};

namespace {
HMODULE gOpengl32 = nullptr;
typedef HGLRC(WINAPI* PFNCreate)(HDC);
typedef BOOL(WINAPI* PFNDelete)(HGLRC);
typedef BOOL(WINAPI* PFNMake)(HDC, HGLRC);
typedef PROC(WINAPI* PFNProc)(LPCSTR);
PFNCreate wglCreate = nullptr;
PFNDelete wglDelete = nullptr;
PFNMake wglMake = nullptr;
PFNProc wglProc = nullptr;

bool loadWgl(std::string* why)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (wglCreate) return true;
    gOpengl32 = LoadLibraryA("opengl32.dll");
    if (!gOpengl32) { *why = "opengl32.dll is not there"; return false; }
    wglCreate = (PFNCreate)GetProcAddress(gOpengl32, "wglCreateContext");
    wglDelete = (PFNDelete)GetProcAddress(gOpengl32, "wglDeleteContext");
    wglMake = (PFNMake)GetProcAddress(gOpengl32, "wglMakeCurrent");
    wglProc = (PFNProc)GetProcAddress(gOpengl32, "wglGetProcAddress");
    if (!wglCreate || !wglDelete || !wglMake || !wglProc) { wglCreate = nullptr; *why = "opengl32.dll has no WGL"; return false; }
    return true;
}

LRESULT CALLBACK hiddenProc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

// Windows belong to the thread that makes them and go with it when it ends, and a thread with windows must keep answering messages. Render threads come and go, so all hidden windows
// are made (and destroyed) by one thread of their own that does nothing else: requests are queued and the thread woken.
struct WindowRequest {
    bool destroy = false;
    HWND wnd = nullptr;
    HDC dc = nullptr;
    bool ok = false;
    std::string why;
    bool done = false;
};
struct WindowThread {
    std::mutex m;
    std::condition_variable cv;
    std::vector<WindowRequest*> queue;
    HANDLE wake = nullptr;
    std::thread th;
};

void handle(WindowRequest& r)
{
    if (r.destroy) {
        if (r.dc && r.wnd) ReleaseDC(r.wnd, r.dc);
        if (r.wnd) DestroyWindow(r.wnd);
        r.ok = true;
        return;
    }
    WNDCLASSA wc;
    std::memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = hiddenProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "RenoGLHost";
    RegisterClassA(&wc);
    r.wnd = CreateWindowExA(WS_EX_TOOLWINDOW, "RenoGLHost", "", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    if (!r.wnd) { r.why = "no window for the context"; return; }
    r.dc = GetDC(r.wnd);
    PIXELFORMATDESCRIPTOR pfd;
    std::memset(&pfd, 0, sizeof pfd);
    pfd.nSize = sizeof pfd;
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    const int fmt = ChoosePixelFormat(r.dc, &pfd);
    if (!fmt || !SetPixelFormat(r.dc, fmt, &pfd)) {
        r.why = "no pixel format";
        ReleaseDC(r.wnd, r.dc);
        DestroyWindow(r.wnd);
        r.wnd = nullptr;
        r.dc = nullptr;
        return;
    }
    r.ok = true;
}

WindowThread* windowThread()
{
    static WindowThread* t = [] {
        WindowThread* w = new WindowThread();       // never destroyed: it ends with the program
        w->wake = CreateEventA(nullptr, FALSE, FALSE, nullptr);
        w->th = std::thread([w] {
            for (;;) {
                MsgWaitForMultipleObjects(1, &w->wake, FALSE, INFINITE, QS_ALLINPUT);
                MSG msg;
                while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageA(&msg);
                }
                std::vector<WindowRequest*> q;
                {
                    std::lock_guard<std::mutex> lk(w->m);
                    q.swap(w->queue);
                }
                for (WindowRequest* r : q) {
                    handle(*r);
                    std::lock_guard<std::mutex> lk(w->m);
                    r->done = true;
                    w->cv.notify_all();
                }
            }
        });
        w->th.detach();
        return w;
    }();
    return t;
}

void askWindowThread(WindowRequest& r)
{
    WindowThread* w = windowThread();
    {
        std::lock_guard<std::mutex> lk(w->m);
        w->queue.push_back(&r);
    }
    SetEvent(w->wake);
    std::unique_lock<std::mutex> lk(w->m);
    w->cv.wait(lk, [&] { return r.done; });
}

bool createPlatform(Context::Impl& p, std::string* why)
{
    if (!loadWgl(why)) return false;
    WindowRequest r;
    askWindowThread(r);
    if (!r.ok) { *why = r.why; return false; }
    p.wnd = r.wnd;
    p.dc = r.dc;
    p.rc = wglCreate(p.dc);
    if (!p.rc) { *why = "wglCreateContext failed"; return false; }
    return true;
}
bool makeCurrentPlatform(Context::Impl& p) { return wglMake(p.dc, p.rc) != 0; }
void doneCurrentPlatform(Context::Impl&) { wglMake(nullptr, nullptr); }
void destroyPlatform(Context::Impl& p)
{
    if (p.rc) { wglMake(nullptr, nullptr); wglDelete(p.rc); }
    if (p.wnd) {
        WindowRequest r;
        r.destroy = true;
        r.wnd = p.wnd;
        r.dc = p.dc;
        askWindowThread(r);
    }
    p = Context::Impl();
}
void* getProc(const char* name)
{
    void* f = wglProc ? (void*)wglProc(name) : nullptr;
    const uintptr_t v = (uintptr_t)f;
    if (v == 0 || v == 1 || v == 2 || v == 3 || v == (uintptr_t)-1) f = gOpengl32 ? (void*)GetProcAddress(gOpengl32, name) : nullptr;
    return f;
}
}  // namespace

#else
// EGL with desktop OpenGL, on a 1x1 pbuffer (the surface is only there because some drivers want one).
typedef void* EGLDisplay;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef void* EGLConfig;
typedef int EGLint;
typedef unsigned int EGLBoolean;
typedef unsigned int EGLenum;

struct Context::Impl {
    EGLDisplay dpy = nullptr;
    EGLContext ctx = nullptr;
    EGLSurface surf = nullptr;
};

namespace {
void* gEgl = nullptr;
void* gLibGL = nullptr;
void* (*eglGetProcAddress_)(const char*) = nullptr;
EGLDisplay (*eglGetDisplay_)(void*) = nullptr;
EGLDisplay (*eglGetPlatformDisplay_)(EGLenum, void*, const EGLint*) = nullptr;
EGLBoolean (*eglInitialize_)(EGLDisplay, EGLint*, EGLint*) = nullptr;
EGLBoolean (*eglBindAPI_)(EGLenum) = nullptr;
EGLBoolean (*eglChooseConfig_)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
EGLContext (*eglCreateContext_)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
EGLSurface (*eglCreatePbufferSurface_)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
EGLBoolean (*eglMakeCurrent_)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
EGLBoolean (*eglDestroyContext_)(EGLDisplay, EGLContext) = nullptr;
EGLBoolean (*eglDestroySurface_)(EGLDisplay, EGLSurface) = nullptr;
EGLDisplay gDisplay = nullptr;
EGLConfig gConfig = nullptr;

bool loadEgl(std::string* why)
{
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    if (gDisplay) return true;
    if (!gEgl) {
        gEgl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!gEgl) { *why = "libEGL.so.1 is not there"; return false; }
    }
#define SYM(var, name) var = (decltype(var))dlsym(gEgl, name)
    SYM(eglGetProcAddress_, "eglGetProcAddress");
    SYM(eglGetDisplay_, "eglGetDisplay");
    SYM(eglInitialize_, "eglInitialize");
    SYM(eglBindAPI_, "eglBindAPI");
    SYM(eglChooseConfig_, "eglChooseConfig");
    SYM(eglCreateContext_, "eglCreateContext");
    SYM(eglCreatePbufferSurface_, "eglCreatePbufferSurface");
    SYM(eglMakeCurrent_, "eglMakeCurrent");
    SYM(eglDestroyContext_, "eglDestroyContext");
    SYM(eglDestroySurface_, "eglDestroySurface");
    SYM(eglGetPlatformDisplay_, "eglGetPlatformDisplay");
#undef SYM
    if (!eglGetProcAddress_ || !eglGetDisplay_ || !eglInitialize_ || !eglBindAPI_ || !eglChooseConfig_ || !eglCreateContext_ || !eglCreatePbufferSurface_ ||
        !eglMakeCurrent_) {
        *why = "libEGL lacks functions";
        return false;
    }
    EGLDisplay d = nullptr;
    if (eglGetPlatformDisplay_) {
        const EGLint none = 0x3038;     // EGL_NONE
        d = eglGetPlatformDisplay_(0x31DD, nullptr, &none);
        EGLint a = 0, b = 0;
        if (d && !eglInitialize_(d, &a, &b)) d = nullptr;
    }
    if (!d) {
        d = eglGetDisplay_(nullptr);
        EGLint a = 0, b = 0;
        if (!d || !eglInitialize_(d, &a, &b)) { *why = "no EGL display"; return false; }
    }
    if (!eglBindAPI_(0x30A2)) { *why = "no desktop OpenGL in EGL"; return false; }      // EGL_OPENGL_API
    const EGLint attrs[] = {0x3033, 0x0001 /* PBUFFER_BIT */, 0x3040, 0x0008 /* OPENGL_BIT */, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038};
    EGLConfig cfg = nullptr;
    EGLint n = 0;
    if (!eglChooseConfig_(d, attrs, &cfg, 1, &n) || n < 1) { *why = "no EGL configuration for OpenGL"; return false; }
    gConfig = cfg;
    gDisplay = d;
    return true;
}

bool createPlatform(Context::Impl& p, std::string* why)
{
    if (!loadEgl(why)) return false;
    eglBindAPI_(0x30A2);
    p.dpy = gDisplay;
    p.ctx = eglCreateContext_(gDisplay, gConfig, nullptr, nullptr);
    if (!p.ctx) { *why = "eglCreateContext failed"; return false; }
    const EGLint sa[] = {0x3057, 1, 0x3056, 1, 0x3038};         // WIDTH, HEIGHT, NONE
    p.surf = eglCreatePbufferSurface_(gDisplay, gConfig, sa);
    return true;
}
bool makeCurrentPlatform(Context::Impl& p)
{
    eglBindAPI_(0x30A2);
    return eglMakeCurrent_(p.dpy, p.surf, p.surf, p.ctx) != 0;
}
void doneCurrentPlatform(Context::Impl& p) { eglMakeCurrent_(p.dpy, nullptr, nullptr, nullptr); }
void destroyPlatform(Context::Impl& p)
{
    if (p.dpy) {
        eglMakeCurrent_(p.dpy, nullptr, nullptr, nullptr);
        if (p.ctx && eglDestroyContext_) eglDestroyContext_(p.dpy, p.ctx);
        if (p.surf && eglDestroySurface_) eglDestroySurface_(p.dpy, p.surf);
    }
    p = Context::Impl();
}
void* getProc(const char* name)
{
    void* f = eglGetProcAddress_ ? eglGetProcAddress_(name) : nullptr;
    if (!f) {
        if (!gLibGL) gLibGL = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (gLibGL) f = dlsym(gLibGL, name);
    }
    return f;
}
}  // namespace
#endif

// ====================================================================== the functions
namespace {
template <class F>
bool load(F& f, const char* name)
{
    f = (F)getProc(name);
    if (f) return true;
    for (const char* suffix : {"EXT", "ARB"}) {
        f = (F)getProc((std::string(name) + suffix).c_str());
        if (f) return true;
    }
    return false;
}

// With the context current. False (and 'why') if something the host needs is missing.
bool loadFuncs(Funcs& fn, std::string* why)
{
    bool ok = true;
    auto need = [&](bool got, const char* name) {
        if (!got) { ok = false; if (why->empty()) *why = std::string("OpenGL lacks gl") + name; }
    };
#define L(member, glname) need(load(fn.member, "gl" glname), glname)
    L(GetError, "GetError"); L(Finish, "Finish"); L(GenTextures, "GenTextures"); L(DeleteTextures, "DeleteTextures");
    L(BindTexture, "BindTexture"); L(TexImage2D, "TexImage2D"); L(TexParameteri, "TexParameteri"); L(PixelStorei, "PixelStorei");
    L(ReadPixels, "ReadPixels"); L(Viewport, "Viewport"); L(ClearColor, "ClearColor"); L(Clear, "Clear"); L(GetIntegerv, "GetIntegerv");
    L(GetString, "GetString"); L(ActiveTexture, "ActiveTexture"); L(BindBuffer, "BindBuffer"); L(UseProgram, "UseProgram");
    L(ReadBuffer, "ReadBuffer"); L(DrawBuffer, "DrawBuffer");
    L(GenFramebuffers, "GenFramebuffers"); L(DeleteFramebuffers, "DeleteFramebuffers"); L(BindFramebuffer, "BindFramebuffer");
    L(FramebufferTexture2D, "FramebufferTexture2D"); L(CheckFramebufferStatus, "CheckFramebufferStatus");
    L(GenRenderbuffers, "GenRenderbuffers"); L(DeleteRenderbuffers, "DeleteRenderbuffers"); L(BindRenderbuffer, "BindRenderbuffer");
    L(RenderbufferStorage, "RenderbufferStorage"); L(FramebufferRenderbuffer, "FramebufferRenderbuffer");
#undef L
    return ok;
}
}  // namespace

// ====================================================================== contexts and the pool
Context::~Context()
{
    if (impl) {
        destroyPlatform(*impl);
        delete impl;
    }
}

namespace {
struct Pool {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Context>> contexts;
    std::set<Context*> busy;
    int limit = 0;
    int nextId = 1;
    int creating = 0;
    int state = 0;              // 0 not tried, 1 works, -1 does not
    std::string why;
};
Pool& pool()
{
    static Pool* p = new Pool();        // never destroyed: effects may still be detaching at exit
    return *p;
}
thread_local Context* tLast = nullptr;
thread_local Context* tHeld = nullptr;       // the context this thread has leased (the innermost)

// Makes a new context, current on this thread, with the host's functions loaded. Null (and 'why') if it cannot.
std::unique_ptr<Context> makeContext(int id, std::string* why)
{
    std::unique_ptr<Context> c(new Context());
    c->id = id;
    c->impl = new Context::Impl();
    if (!createPlatform(*c->impl, why)) return nullptr;
    if (!makeCurrentPlatform(*c->impl)) { *why = "the context could not be made current"; return nullptr; }
    if (!loadFuncs(c->fn, why)) { doneCurrentPlatform(*c->impl); return nullptr; }
    const unsigned char* r = c->fn.GetString(RENDERER);
    c->renderer = r ? (const char*)r : "";
    if (c->renderer.find("GDI Generic") != std::string::npos) {
        *why = "only Microsoft's software OpenGL 1.1 is there";
        doneCurrentPlatform(*c->impl);
        return nullptr;
    }
    Funcs& f = c->fn;
    GLuint tex = 0, fbo = 0;
    f.GenTextures(1, &tex);
    f.BindTexture(TEXTURE_2D, tex);
    f.TexImage2D(TEXTURE_2D, 0, RGBA32F, 4, 4, 0, RGBA, FLOAT, nullptr);
    f.GenFramebuffers(1, &fbo);
    f.BindFramebuffer(FRAMEBUFFER, fbo);
    f.FramebufferTexture2D(FRAMEBUFFER, COLOR_ATTACHMENT0, TEXTURE_2D, tex, 0);
    const GLenum st = f.CheckFramebufferStatus(FRAMEBUFFER);
    f.BindFramebuffer(FRAMEBUFFER, 0);
    f.DeleteFramebuffers(1, &fbo);
    f.DeleteTextures(1, &tex);
    f.BindTexture(TEXTURE_2D, 0);
    while (f.GetError() != NO_ERROR_) {}
    if (st != FRAMEBUFFER_COMPLETE) {
        *why = "frame buffers with float textures do not work";
        doneCurrentPlatform(*c->impl);
        return nullptr;
    }
    return c;
}
}  // namespace

Lease& Lease::operator=(Lease&& o) noexcept
{
    if (this != &o) {
        release();
        ctx_ = o.ctx_;
        prev_ = o.prev_;
        o.ctx_ = nullptr;
        o.prev_ = nullptr;
    }
    return *this;
}

Lease Lease::acquire(Context* want)
{
    Pool& p = pool();
    Lease l;
    std::unique_lock<std::mutex> lk(p.m);
    if (p.state < 0) return l;
    if (p.limit <= 0) p.limit = (int)std::max(2u, std::thread::hardware_concurrency() + 1);
    for (;;) {
        Context* pick = nullptr;
        if (want) {
            if (!p.busy.count(want)) pick = want;
        } else {
            if (tLast) {
                // the one this thread had, if it still exists and is free
                bool exists = false;
                for (auto& c : p.contexts) exists = exists || c.get() == tLast;
                if (exists && !p.busy.count(tLast)) pick = tLast;
            }
            for (auto& c : p.contexts)
                if (!pick && !p.busy.count(c.get())) pick = c.get();
        }
        if (pick) {
            p.busy.insert(pick);
            lk.unlock();
            if (!makeCurrentPlatform(*pick->impl)) {
                lk.lock();
                p.busy.erase(pick);
                p.cv.notify_all();
                return l;
            }
            tLast = pick;
            l.ctx_ = pick;
            l.prev_ = tHeld;
            tHeld = pick;
            return l;
        }
        if (!want && ((int)p.contexts.size() + p.creating < p.limit || tHeld) && p.state >= 0) {
            const int id = p.nextId++;
            std::unique_ptr<Context> c;
            std::string why;
            ++p.creating;
            lk.unlock();
            c = makeContext(id, &why);
            lk.lock();
            --p.creating;
            if (!c) {
                if (p.contexts.empty()) {
                    p.state = -1;
                    p.why = why;
                }
                logf("gpu: could not make an OpenGL context: %s", why.c_str());
                p.cv.notify_all();
                return l;
            }
            Context* raw = c.get();
            p.contexts.push_back(std::move(c));
            p.state = 1;
            p.busy.insert(raw);
            tLast = raw;
            l.ctx_ = raw;
            l.prev_ = tHeld;
            tHeld = raw;
            return l;      // made current on this thread by makeContext
        }
        if (want) {
            bool exists = false;
            for (auto& c : p.contexts) exists = exists || c.get() == want;
            if (!exists) return l;
        } else if (p.contexts.empty() && p.creating == 0) {
            return l;
        }
        p.cv.wait(lk);
    }
}

void Lease::release()
{
    if (!ctx_) return;
    Pool& p = pool();
    doneCurrentPlatform(*ctx_->impl);
    {
        std::lock_guard<std::mutex> lk(p.m);
        p.busy.erase(ctx_);
    }
    p.cv.notify_all();
    ctx_ = nullptr;
    // the lease this thread held before is current again
    tHeld = prev_;
    if (prev_) makeCurrentPlatform(*prev_->impl);
    prev_ = nullptr;
}

bool available()
{
    Pool& p = pool();
    {
        std::lock_guard<std::mutex> lk(p.m);
        if (p.state != 0) return p.state > 0;
    }
    if (std::getenv("RENO_NO_GPU")) {
        std::lock_guard<std::mutex> lk(p.m);
        p.state = -1;
        p.why = "RENO_NO_GPU is set";
        return false;
    }
    Lease l = Lease::acquire();         // makes the first context
    return l.ok();
}

std::string unavailableReason()
{
    Pool& p = pool();
    std::lock_guard<std::mutex> lk(p.m);
    return p.why;
}

int contextCount()
{
    Pool& p = pool();
    std::lock_guard<std::mutex> lk(p.m);
    return (int)p.contexts.size();
}

void shutdown()
{
    Pool& p = pool();
    std::vector<Context*> all;
    {
        std::lock_guard<std::mutex> lk(p.m);
        for (auto& c : p.contexts) all.push_back(c.get());
    }
    for (Context* c : all) {
        Lease l = Lease::acquire(c);
        if (!l.ok()) continue;
        std::set<Instance*> att;
        {
            std::lock_guard<std::mutex> lk(c->mutex);
            att.swap(c->attached);
        }
        if (detachHook)
            for (Instance* i : att) detachHook(i);
    }
    std::unique_lock<std::mutex> lk(p.m);
    for (Context* c : all) {
        while (p.busy.count(c)) p.cv.wait(lk);
    }
    p.contexts.clear();
    p.busy.clear();
    p.state = 0;
    p.why.clear();
    tLast = nullptr;
}

}  // namespace gl
}  // namespace ofx
}  // namespace reno
