// gputest - Reno's OpenFX host renders effects with OpenGL (src/ofx/gpurender.*, glcontext.*).
//
//   gputest <glfx.ofx> [mode]
//     (default)  OpenGL works: the tests below
//     nogpu      RENO_NO_GPU set before anything: the host says OpenGL is not supported and effects render on the processor
// The test effect (glfx.cpp) draws with OpenGL when asked to and on the processor otherwise, to the same picture.
#include "ofxhost.h"
#include "gpurender.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <thread>
#include <vector>

using namespace reno;
using namespace reno::ofx;

static std::atomic<int> gPass{0}, gFail{0};
static std::mutex gOut;
static void check(const std::string& name, bool ok, const std::string& detail = std::string())
{
    std::lock_guard<std::mutex> lk(gOut);
    if (ok) ++gPass; else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    std::fflush(stdout);
}
static std::string fmt(const char* f, ...)
{
    char b[600];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

static int (*evCount)() = nullptr;
static const char* (*evLine)(int) = nullptr;
static void (*evClear)() = nullptr;
static void (*counts)(int*) = nullptr;
static std::vector<std::string> events()
{
    std::vector<std::string> v;
    for (int i = 0, n = evCount(); i < n; ++i) v.push_back(evLine(i));
    return v;
}
static int findEv(const std::vector<std::string>& v, const std::string& prefix, int from = 0)
{
    for (int i = from; i < (int)v.size(); ++i) if (v[(size_t)i].rfind(prefix, 0) == 0) return i;
    return -1;
}
static int countEv(const std::vector<std::string>& v, const std::string& prefix)
{
    int n = 0;
    for (const auto& s : v) n += s.rfind(prefix, 0) == 0;
    return n;
}

static ImagePtr makeSource(int w, int h, PixelDepth d, int seed = 0)
{
    ImagePtr img = Image::make(w, h, d, true);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img->row(y) + (size_t)x * img->pixelBytes();
            const double r = ((x + seed * 7) % w) / (double)(w - 1), g = y / (double)(h - 1), b = ((x + y) % 64) / 63.0;
            const double a = 0.5 + 0.5 * r;
            if (d == PixelDepth::Byte) { p[0] = (uint8_t)(b * a * 255); p[1] = (uint8_t)(g * a * 255); p[2] = (uint8_t)(r * a * 255); p[3] = (uint8_t)(a * 255); }
            else { float* f = (float*)p; f[0] = (float)(r * a); f[1] = (float)(g * a); f[2] = (float)(b * a); f[3] = (float)a; }
        }
    return img;
}

// the largest difference in levels (0..255) between two pictures
static double maxDiff(const Image& a, const Image& b)
{
    if (a.w != b.w || a.h != b.h || a.depth != b.depth) return 1e9;
    double mx = 0;
    const bool flt = a.depth == PixelDepth::Float;
    for (int y = 0; y < a.h; ++y)
        for (int x = 0; x < a.w; ++x)
            for (int c = 0; c < 4; ++c) {
                const uint8_t* p = a.row(y) + (size_t)x * a.pixelBytes();
                const uint8_t* q = b.row(y) + (size_t)x * b.pixelBytes();
                const double d = flt ? std::fabs(((const float*)p)[c] - ((const float*)q)[c]) * 255.0 : std::abs((int)p[c] - (int)q[c]);
                if (d > mx) mx = d;
            }
    return mx;
}

static Plugin* gPlugin = nullptr;
static std::unique_ptr<Instance> make(int w, int h)
{
    InstanceSetup s;
    s.width = w;
    s.height = h;
    std::string err;
    auto i = gPlugin->createInstance(gPlugin->filterContext(), s, &err);
    if (!i) std::printf("  createInstance: %s\n", err.c_str());
    return i;
}

static ImagePtr render(Instance& inst, const ImagePtr& src, PixelDepth d, double scale = 1.0, double* ms = nullptr, double red = -1, int pw = 0, int ph = 0)
{
    if (red >= 0) {
        std::lock_guard<std::recursive_mutex> lk(inst.mutex);
        Param* p = inst.params.find("Red");
        p->value[0] = red;
    }
    inst.setClipSource("Source", [src](double) { return src; });
    RenderOptions ro;
    ro.depth = d;
    ro.width = pw ? (int)std::lround(pw * scale) : 0;
    ro.height = ph ? (int)std::lround(ph * scale) : 0;
    auto t0 = std::chrono::steady_clock::now();
    std::string err;
    ImagePtr out = inst.render(1.0, ro, &err);
    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!out) std::printf("  render error: %s\n", err.c_str());
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: gputest <glfx.ofx> [nogpu]\n"); return 2; }
    const std::string mode = argc > 2 ? argv[2] : "gl";
    if (mode == "nogpu") setenv("RENO_NO_GPU", "1", 1);
    Host& host = Host::get();
    std::string err;
    check("the plug-in loads", host.loadBinary(argv[1], &err) != nullptr, err);
    gPlugin = host.findPlugin("reno.test.gltint");
    if (!gPlugin || !gPlugin->load(&err)) { check("describe", false, err); return 1; }
    void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_NOLOAD);
    evCount = (int (*)())dlsym(lib, "glfx_event_count");
    evLine = (const char* (*)(int))dlsym(lib, "glfx_event");
    evClear = (void (*)())dlsym(lib, "glfx_clear");
    counts = (void (*)(int*))dlsym(lib, "glfx_counts");
    check("the test hooks are there", evCount && evLine && evClear && counts);
    if (!(evCount && evLine && evClear && counts)) return 1;

    const int W = 320, H = 180;
    ImagePtr srcByte = makeSource(W, H, PixelDepth::Byte), srcFloat = makeSource(W, H, PixelDepth::Float);

    // ---------------------------------------------------------------------------------- no OpenGL here
    if (mode == "nogpu") {
        check("the host says OpenGL is not supported", host.hostProps.getString(kOfxImageEffectPropOpenGLRenderSupported) == "false");
        check("OpenGL is not available", !gl::available(), gl::unavailableReason());
        check("the OpenGL render suite is not given", gpuSuite() == nullptr);
        auto inst = make(W, H);
        evClear();
        ImagePtr out = render(*inst, srcByte, PixelDepth::Byte);
        auto ev = events();
        check("the effect renders on the processor", out && countEv(ev, "render cpu") == 1 && countEv(ev, "render gl") == 0 && countEv(ev, "attach") == 0);
        std::printf("\n%d passed, %d failed\n", gPass.load(), gFail.load());
        return gFail ? 1 : 0;
    }

    // ---------------------------------------------------------------------------------- the host
    check("OpenGL is available", gl::available(), gl::unavailableReason());
    if (!gl::available()) return 1;
    check("the host says OpenGL is supported", host.hostProps.getString(kOfxImageEffectPropOpenGLRenderSupported) == "true");
    check("the OpenGL render suite is given", gpuSuite() != nullptr);
    {
        auto ev = events();
        check("the effect saw the host's answer and the suite when it loaded", findEv(ev, "load host=true suite=yes") >= 0, ev.empty() ? "" : ev[0]);
    }
    check("the effect says it can render with OpenGL", make(W, H)->props.getString(kOfxImageEffectPropOpenGLRenderSupported) == "true");

    // ---------------------------------------------------------------------------------- GL against the processor
    for (PixelDepth d : {PixelDepth::Byte, PixelDepth::Float}) {
        const char* dn = d == PixelDepth::Byte ? "8-bit" : "float";
        const ImagePtr& src = d == PixelDepth::Byte ? srcByte : srcFloat;
        for (double scale : {1.0, 0.5}) {
            const int w = (int)std::lround(W * scale), h = (int)std::lround(H * scale);
            ImagePtr s = scale == 1.0 ? src : makeSource(w, h, d);
            auto inst = make(W, H);
            host.config.gpu = false;
            ImagePtr cpu = render(*inst, s, d, scale, nullptr, 0.4, W, H);
            host.config.gpu = true;
            evClear();
            ImagePtr gpu = render(*inst, s, d, scale, nullptr, 0.4, W, H);
            auto ev = events();
            check(fmt("%s at %g: rendered with OpenGL (attached once, one GL render)", dn, scale), gpu && countEv(ev, "attach") == 1 && countEv(ev, "render gl") == 1 && countEv(ev, "render cpu") == 0, fmt("%zu events", ev.size()));
            check(fmt("%s at %g: the picture is %dx%d", dn, scale, w, h), gpu && cpu && gpu->w == w && gpu->h == h && cpu->w == w);
            if (gpu && cpu) {
                const double mx = maxDiff(*gpu, *cpu);
                check(fmt("%s at %g: OpenGL and the processor give the same picture", dn, scale), mx <= (d == PixelDepth::Byte ? 1.01 : 0.02), fmt("largest difference %.3f levels", mx));
                // and it is not just the source
                check(fmt("%s at %g: and the red really changed", dn, scale), maxDiff(*gpu, *s) > 10, fmt("%.1f", maxDiff(*gpu, *s)));
            }
            evClear();
            render(*inst, s, d, scale, nullptr, 0.7, W, H);
            ev = events();
            check(fmt("%s at %g: a second frame is not attached again", dn, scale), countEv(ev, "attach") == 0 && countEv(ev, "render gl") == 1);
        }
    }
    // the effect's textures not freed (the host frees them), and the output texture never asked for
    {
        auto inst = make(W, H);
        host.config.gpu = false;
        ImagePtr cpu = render(*inst, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.3);
        host.config.gpu = true;
        setenv("GLFX_NO_FREE", "1", 1);
        setenv("GLFX_NO_LOAD_OUT", "1", 1);
        ImagePtr gpu = render(*inst, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.3);
        unsetenv("GLFX_NO_FREE");
        unsetenv("GLFX_NO_LOAD_OUT");
        check("an effect that frees nothing and draws into the bound frame buffer still works", gpu && cpu && maxDiff(*gpu, *cpu) <= 1.01);
        check("and it was not made to fall back", !gPlugin->glFailed.load());
    }

    // ---------------------------------------------------------------------------------- attach / detach
    {
        evClear();
        {
            auto inst = make(W, H);
            render(*inst, srcByte, PixelDepth::Byte);
            render(*inst, srcByte, PixelDepth::Byte);
        }
        auto ev = events();
        const int a = findEv(ev, "attach"), r = findEv(ev, "render gl"), dt = findEv(ev, "detach"), ds = findEv(ev, "destroy");
        check("order: attached, rendered, detached, then destroyed", a >= 0 && r > a && dt > r && ds > dt, fmt("attach %d render %d detach %d destroy %d", a, r, dt, ds));
        check("attached once, detached once", countEv(ev, "attach") == 1 && countEv(ev, "detach") == 1);
        check("detached with its own context current", ev.size() && countEv(ev, "detach") == 1 && ev[(size_t)dt].find("WRONG-CONTEXT") == std::string::npos && ev[(size_t)dt].find("ctx=(nil)") == std::string::npos, dt >= 0 ? ev[(size_t)dt] : "");
        check("rendered with a context current", r >= 0 && ev[(size_t)r].find("ctx=current") != std::string::npos);
    }

    // ---------------------------------------------------------------------------------- the effect fails
    {
        auto inst = make(W, H);
        host.config.gpu = false;
        ImagePtr cpu = render(*inst, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.25);
        host.config.gpu = true;
        evClear();
        setenv("GLFX_FAIL_RENDER", "1", 1);
        ImagePtr out = render(*inst, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.25);
        unsetenv("GLFX_FAIL_RENDER");
        auto ev = events();
        check("the render fails with OpenGL: that frame is rendered on the processor, correctly", out && cpu && maxDiff(*out, *cpu) == 0 && countEv(ev, "render gl") == 1 && countEv(ev, "render cpu") == 1);
        check("and the effect is not asked to use OpenGL again", gPlugin->glFailed.load());
        evClear();
        render(*inst, srcByte, PixelDepth::Byte);
        ev = events();
        check("the next frame is on the processor straight away", countEv(ev, "render gl") == 0 && countEv(ev, "render cpu") == 1);
        gPlugin->glFailed = false;
        inst.reset();
        // attaching fails
        inst = make(W, H);
        evClear();
        setenv("GLFX_FAIL_ATTACH", "1", 1);
        out = render(*inst, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.25);
        unsetenv("GLFX_FAIL_ATTACH");
        ev = events();
        check("attaching fails: the frame is rendered on the processor, correctly", out && cpu && maxDiff(*out, *cpu) == 0 && countEv(ev, "render gl") == 0 && countEv(ev, "render cpu") == 1);
        check("and the effect is switched to the processor", gPlugin->glFailed.load());
        gPlugin->glFailed = false;
    }
    // the preference off
    {
        auto inst = make(W, H);
        host.config.gpu = false;
        evClear();
        render(*inst, srcByte, PixelDepth::Byte);
        auto ev = events();
        check("GPU acceleration off: processor only", countEv(ev, "render gl") == 0 && countEv(ev, "render cpu") == 1 && countEv(ev, "attach") == 0);
        host.config.gpu = true;
    }

    // ---------------------------------------------------------------------------------- effects rendering inside each other
    {
        // a second effect whose source is the first one's picture: made while the second is loading its texture
        auto a = make(W, H), b = make(W, H);
        host.config.gpu = false;
        ImagePtr ca = render(*a, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.5);
        ImagePtr cb = render(*b, ca, PixelDepth::Byte, 1.0, nullptr, 0.5);
        host.config.gpu = true;
        evClear();
        {
            std::lock_guard<std::recursive_mutex> lk(a->mutex);
            a->params.find("Red")->value[0] = 0.5;
        }
        {
            std::lock_guard<std::recursive_mutex> lk2(b->mutex);
            b->params.find("Red")->value[0] = 0.5;
        }
        a->setClipSource("Source", [srcByte](double) { return srcByte; });
        b->setClipSource("Source", [&](double) -> ImagePtr {
            RenderOptions ro;
            return a->render(1.0, ro, nullptr);
        });
        RenderOptions ro;
        ImagePtr out = b->render(1.0, ro, nullptr);
        auto ev = events();
        check("nested: both rendered with OpenGL", countEv(ev, "render gl") == 2, fmt("%d", countEv(ev, "render gl")));
        check("nested: the picture is the processor's", out && cb && maxDiff(*out, *cb) <= 1.01);
        check("nested: every GL render had a context", countEv(ev, "render gl ctx=current") == 2);
    }

    // ---------------------------------------------------------------------------------- many threads
    {
        const int nThreads = 8, frames = 12;
        std::atomic<int> bad{0}, done{0};
        // references on the processor
        std::vector<ImagePtr> srcs, refs;
        host.config.gpu = false;
        {
            auto ri = make(W, H);
            for (int k = 0; k < frames; ++k) {
                srcs.push_back(makeSource(W, H, k % 2 ? PixelDepth::Float : PixelDepth::Byte, k));
                refs.push_back(render(*ri, srcs.back(), srcs.back()->depth, 1.0, nullptr, 0.1 + 0.07 * k));
            }
        }
        host.config.gpu = true;
        evClear();
        auto shared = make(W, H);       // one effect used by all the threads (one frame at a time through its lock)
        std::vector<std::thread> th;
        auto t0 = std::chrono::steady_clock::now();
        for (int t = 0; t < nThreads; ++t)
            th.emplace_back([&, t] {
                auto mine = make(W, H);
                for (int k = 0; k < frames; ++k) {
                    Instance& inst = (k % 3 == 0) ? *shared : *mine;
                    ImagePtr out;
                    {
                        // (the shared effect's settings and picture are set and used under its lock, as the engine does)
                        std::unique_lock<std::recursive_mutex> lk(inst.mutex);
                        inst.params.find("Red")->value[0] = 0.1 + 0.07 * k;
                        const ImagePtr s = srcs[(size_t)k];
                        inst.setClipSource("Source", [s](double) { return s; });
                        RenderOptions ro;
                        ro.depth = s->depth;
                        out = inst.render(1.0, ro, nullptr);
                    }
                    const double tol = srcs[(size_t)k]->depth == PixelDepth::Byte ? 1.01 : 0.02;
                    if (!out || maxDiff(*out, *refs[(size_t)k]) > tol) ++bad;
                    ++done;
                }
            });
        for (auto& x : th) x.join();
        const double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        auto ev = events();
        int c[4];
        counts(c);
        check(fmt("%d threads x %d frames with OpenGL: all done", nThreads, frames), done == nThreads * frames, fmt("%d", done.load()));
        check("and every picture is right", bad == 0, fmt("%d wrong", bad.load()));
        check("every render was with OpenGL and in a context", c[2] == nThreads * frames && c[3] == 0 && countEv(ev, "render gl ctx=current") == nThreads * frames, fmt("gl %d cpu %d", c[2], c[3]));
        check("no render was in a context other than the one it was attached to", countEv(ev, "render gl ctx=current OTHER") == 0);
        check("contexts were made for the threads, not more than the limit", gl::contextCount() >= 1 && gl::contextCount() <= (int)std::thread::hardware_concurrency() + 2, fmt("%d contexts, %.2f s", gl::contextCount(), sec));
        check("each instance attached once per context it used", countEv(ev, "attach") <= (nThreads + 1) * gl::contextCount(), fmt("%d attaches", countEv(ev, "attach")));
    }

    // ---------------------------------------------------------------------------------- shutting down
    {
        auto inst = make(W, H);
        render(*inst, srcByte, PixelDepth::Byte);
        evClear();
        gl::shutdown();
        auto ev = events();
        check("shutdown: effects still attached are detached", countEv(ev, "detach") >= 1, fmt("%d detaches", countEv(ev, "detach")));
        check("shutdown: no contexts are left", gl::contextCount() == 0);
        evClear();
        ImagePtr out = render(*inst, srcByte, PixelDepth::Byte, 1.0, nullptr, 0.5);
        ev = events();
        check("after it, rendering starts again (attached anew)", out && countEv(ev, "attach") == 1 && countEv(ev, "render gl") == 1);
    }

    // ---------------------------------------------------------------------------------- what the round trip costs
    for (int sz : {0, 1}) {
        const int bw = sz ? 1920 : 960, bh = sz ? 1080 : 540;
        for (PixelDepth d : {PixelDepth::Byte, PixelDepth::Float}) {
            ImagePtr s = makeSource(bw, bh, d);
            auto inst = make(bw, bh);
            double tg = 0, tc = 0, tmp = 0;
            host.config.gpu = true;
            render(*inst, s, d, 1.0, &tmp, 0.5);        // (the first one makes the context and attaches)
            const int n = 10;
            for (int k = 0; k < n; ++k) { render(*inst, s, d, 1.0, &tmp, 0.5); tg += tmp; }
            host.config.gpu = false;
            for (int k = 0; k < n; ++k) { render(*inst, s, d, 1.0, &tmp, 0.5); tc += tmp; }
            host.config.gpu = true;
            std::printf("NOTE %dx%d %s: OpenGL %.2f ms a frame (upload, draw, read back; llvmpipe, no real GPU), processor %.2f ms\n", bw, bh, d == PixelDepth::Byte ? "8-bit" : "float", tg / n, tc / n);
        }
    }
    gl::shutdown();
    std::printf("\n%d passed, %d failed\n", gPass.load(), gFail.load());
    return gFail ? 1 : 0;
}
