// scaletest - checks that Reno's OpenFX host renders a multi-resolution effect at the preview's size.
//
//   scaletest <scalefx.ofx> [multi|nomulti|default]
//     multi    (default) the effect says SupportsMultiResolution 1
//     nomulti  it says 0 (SCALEFX_NOMULTIRES): always rendered at the project's size
//     default  it says nothing (SCALEFX_DEFAULTMULTIRES): the OpenFX default is 1
// The effect draws a disc whose radius and centre are in full-size pixels and scales them by the render scale; the host
// must hand it pictures, render windows and regions of definition of the scaled size, and the render scale in every
// action that has one.
#include "ofxhost.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <dlfcn.h>
#include <string>

using namespace reno;
using namespace reno::ofx;

static int gPass = 0, gFail = 0;
static void check(const char* name, bool ok, const std::string& detail = std::string())
{
    if (ok) ++gPass; else ++gFail;
    std::printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
}
static std::string fmt(const char* f, ...)
{
    char b[512];
    va_list a;
    va_start(a, f);
    vsnprintf(b, sizeof b, f, a);
    va_end(a);
    return b;
}

typedef void (*LastFn)(double*);

// the mean absolute difference (0..255 per channel) between 'small' and 'big' averaged down by f
static double diffToReduced(const Image& small, const Image& big, int f, double* mx)
{
    double sum = 0;
    long n = 0;
    *mx = 0;
    const bool flt = small.depth == PixelDepth::Float;
    for (int y = 0; y < small.h; ++y)
        for (int x = 0; x < small.w; ++x)
            for (int c = 0; c < 4; ++c) {
                double acc = 0;
                for (int j = 0; j < f; ++j)
                    for (int i = 0; i < f; ++i) {
                        const uint8_t* p = big.row(y * f + j) + (size_t)(x * f + i) * big.pixelBytes();
                        acc += flt ? ((const float*)p)[c] * 255.0 : p[c];
                    }
                acc /= f * f;
                const uint8_t* q = small.row(y) + (size_t)x * small.pixelBytes();
                const double v = flt ? ((const float*)q)[c] * 255.0 : q[c];
                const double d = std::fabs(v - acc);
                sum += d;
                if (d > *mx) *mx = d;
                ++n;
            }
    return sum / (double)n;
}

static ImagePtr makeSource(int w, int h, PixelDepth d)
{
    ImagePtr img = Image::make(w, h, d, true);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* p = img->row(y) + (size_t)x * img->pixelBytes();
            const double r = x / (double)(w - 1), g = y / (double)(h - 1), b = 0.3;
            if (d == PixelDepth::Byte) { p[0] = (uint8_t)(b * 255); p[1] = (uint8_t)(g * 255); p[2] = (uint8_t)(r * 255); p[3] = 255; }
            else { float* f = (float*)p; f[0] = (float)r; f[1] = (float)g; f[2] = (float)b; f[3] = 1.f; }
        }
    return img;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::printf("usage: scaletest <scalefx.ofx> [multi|nomulti|default]\n"); return 2; }
    const std::string mode = argc > 2 ? argv[2] : "multi";
    if (mode == "nomulti") setenv("SCALEFX_NOMULTIRES", "1", 1);
    if (mode == "default") setenv("SCALEFX_DEFAULTMULTIRES", "1", 1);
    const bool wantScaled = mode != "nomulti";
    Host& host = Host::get();
    std::string err;
    check("the plug-in loads", host.loadBinary(argv[1], &err) != nullptr, err);
    Plugin* pl = host.findPlugin("reno.test.disc");
    if (!pl || !pl->load(&err)) { check("describe", false, err); return 1; }
    void* lib = dlopen(argv[1], RTLD_LAZY | RTLD_NOLOAD);
    LastFn last = lib ? (LastFn)dlsym(lib, "scalefx_last") : nullptr;
    check("the test hook is there", last != nullptr);
    if (!last) return 1;
    double L[24];
    last(L);
    check("the host says it supports multi-resolution", L[21] == 1, fmt("%g", L[21]));
    check(mode == "multi" ? "the effect said 1" : (mode == "nomulti" ? "the effect said 0: not scalable" : "the effect said nothing: scalable by the spec's default"),
          pl->info.multiResolution == wantScaled);

    const int W = 640, H = 360;
    InstanceSetup s;
    s.width = W;
    s.height = H;
    auto inst = pl->createInstance(pl->filterContext(), s, &err);
    check("an instance is made", inst != nullptr, err);
    if (!inst) return 1;
    check("Instance::scalable follows", inst->scalable() == wantScaled);

    for (PixelDepth depth : {PixelDepth::Byte, PixelDepth::Float}) {
        const char* dn = depth == PixelDepth::Byte ? "8-bit" : "float";
        // the effect's source is whatever size the caller made it for this scale
        auto render = [&](double scale, double* ms, ImagePtr* srcOut = nullptr, bool all = false) -> ImagePtr {
            const int w = (int)std::lround(W * scale), h = (int)std::lround(H * scale);
            const bool scaledRender = (inst->scalable() || all) && scale != 1.0;
            ImagePtr src = makeSource(scaledRender ? w : W, scaledRender ? h : H, depth);
            inst->setClipSource("Source", [src](double) { return src; });
            RenderOptions ro;
            ro.depth = depth;
            ro.width = w;
            ro.height = h;
            ro.previewSizeForAll = all;
            auto t0 = std::chrono::steady_clock::now();
            std::string e;
            ImagePtr out = inst->render(3.0, ro, &e);
            *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            if (!out) std::printf("  render error: %s\n", e.c_str());
            if (srcOut) *srcOut = src;
            return out;
        };
        double msFull = 0;
        ImagePtr full = render(1.0, &msFull);
        check(fmt("%s: at scale 1 the picture is the project's", dn).c_str(), full && full->w == W && full->h == H);
        last(L);
        check(fmt("%s: at scale 1 the effect got render scale 1, radius 100 drawn as 100", dn).c_str(), L[0] == 1 && L[1] == 1 && L[15] == 100 && L[16] == 320 && L[17] == 180 &&
                  L[8] == W && L[9] == H && L[10] == W && L[11] == H, fmt("scale %g,%g drawn r %g at %g,%g window to %g,%g rod to %g,%g", L[0], L[1], L[15], L[16], L[17], L[8], L[9], L[10], L[11]));
        if (!full) continue;
        const double scales[2] = {0.5, 0.25};
        for (int k = 0; k < 2; ++k) {
            const double sc = scales[k];
            const int f = k == 0 ? 2 : 4;
            double ms = 0, mx = 0;
            ImagePtr src;
            ImagePtr small = render(sc, &ms, &src);
            if (!small) { check(fmt("%s: a render at %g works", dn, sc).c_str(), false); continue; }
            last(L);
            if (wantScaled) {
                check(fmt("%s: at %g the picture is %dx%d", dn, sc, W / f, H / f).c_str(), small->w == W / f && small->h == H / f, fmt("%dx%d", small->w, small->h));
                check(fmt("%s: at %g the effect was told the render scale", dn, sc).c_str(), L[0] == sc && L[1] == sc, fmt("%g,%g", L[0], L[1]));
                check(fmt("%s: at %g the source bounds, render window and region of definition are the small size", dn, sc).c_str(),
                      L[2] == 0 && L[3] == 0 && L[4] == W / f && L[5] == H / f && L[8] == W / f && L[9] == H / f && L[10] == W / f && L[11] == H / f,
                      fmt("bounds %g,%g,%g,%g window to %g,%g rod to %g,%g", L[2], L[3], L[4], L[5], L[8], L[9], L[10], L[11]));
                check(fmt("%s: at %g the effect gets full-size pixel settings (radius 100, centre 320,180) and scales them", dn, sc).c_str(),
                      L[12] == 100 && L[13] == 320 && L[14] == 180 && L[15] == 100 * sc && L[16] == 320 * sc && L[17] == 180 * sc,
                      fmt("radius %g centre %g,%g; drew %g at %g,%g", L[12], L[13], L[14], L[15], L[16], L[17]));
                check(fmt("%s: at %g isIdentity was told the render scale too", dn, sc).c_str(), L[20] == sc, fmt("%g", L[20]));
            } else {
                check(fmt("%s: at %g it is left at the project's size and scale 1", dn, sc).c_str(), small->w == W && small->h == H && L[0] == 1 && L[1] == 1 && L[8] == W,
                      fmt("%dx%d scale %g", small->w, small->h, L[0]));
                // a preview with every effect at its size (RenderOptions::previewSizeForAll, as VEGAS): the effect gets
                // the small picture, render scale 1, a project of that size, and its canonical positions scaled to it
                ImagePtr shrunk = render(sc, &ms, nullptr, true);
                last(L);
                check(fmt("%s: at %g with every effect at the preview's size the picture is %dx%d at render scale 1", dn, sc, W / f, H / f).c_str(),
                      shrunk && shrunk->w == W / f && shrunk->h == H / f && L[0] == 1 && L[1] == 1,
                      fmt("%dx%d scale %g", shrunk ? shrunk->w : 0, shrunk ? shrunk->h : 0, L[0]));
                check(fmt("%s: at %g its bounds, render window and region of definition are the small size", dn, sc).c_str(),
                      L[4] == W / f && L[5] == H / f && L[8] == W / f && L[9] == H / f && L[10] == W / f && L[11] == H / f,
                      fmt("bounds to %g,%g window to %g,%g rod to %g,%g", L[4], L[5], L[8], L[9], L[10], L[11]));
                check(fmt("%s: at %g its position setting (XY absolute) is scaled to the preview, a plain number is not", dn, sc).c_str(),
                      std::fabs(L[13] - 320 * sc) < 1e-9 && std::fabs(L[14] - 180 * sc) < 1e-9 && L[12] == 100,
                      fmt("centre %g,%g radius %g", L[13], L[14], L[12]));
                double pw = 0, ph = 0;
                pw = inst->props.getDouble(kOfxImageEffectPropProjectSize, 0);
                ph = inst->props.getDouble(kOfxImageEffectPropProjectSize, 1);
                check(fmt("%s: after it the effect's project size is the project's again", dn).c_str(), pw == W && ph == H, fmt("%gx%g", pw, ph));
                ImagePtr again = render(1.0, &ms);
                last(L);
                check(fmt("%s: and a full-size render after it is as before", dn).c_str(), again && again->w == W && L[13] == 320 && L[16] == 320,
                      fmt("centre %g drawn %g", L[13], L[16]));
            }
            if (wantScaled) {
                const double d = diffToReduced(*small, *full, f, &mx);
                check(fmt("%s: at %g it looks like the full-size picture averaged down", dn, sc).c_str(), d < 4.0,
                      fmt("mean %.3f, largest %.0f levels; %.2f ms against %.2f ms at full size", d, mx, ms, msFull));
            }
        }
    }
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
