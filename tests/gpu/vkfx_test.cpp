// Compares CPU and GPU for every built-in effect that has a Vulkan kernel: the same test picture through the effect's
// render function and through its compute passes, at the defaults and at settings that reach its other branches. 8-bit
// pictures must agree within one level per channel, float ones within 1e-4. Skips (with a message) when there is no
// Vulkan driver; set VK_ICD_FILENAMES to SwiftShader's json and RENO_VK_ALLOW_SOFTWARE=1 to run it on a CPU implementation.
#include "../../src/ofx/vkeffects.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

static int gFail = 0, gChecks = 0;
static double gFloatTol = -1;   // VKTOLF=x: the float pictures' tolerance for every effect (0: identical)
static int gDeclined = 0;
static int gStrict8 = 0;  // 8-bit pictures must be identical (VKTOL8=n loosens it for investigation)

int main(int argc, char** argv)
{
    std::string only;
    if (const char* t = std::getenv("VKTOL8")) gStrict8 = std::atoi(t);
    if (const char* t = std::getenv("VKTOLF")) gFloatTol = std::atof(t);
    for (int k = 1; k < argc; ++k) if (!std::strcmp(argv[k], "--only") && k + 1 < argc) only = argv[++k];
    gpu::setEnabled(true);
    std::string why;
    if (!gpu::ready(&why)) {
        std::printf("vkfx_test: SKIPPED - %s\n", why.c_str());
        return 0;
    }
    std::printf("using %s\n", gpu::status().c_str());
    {
        const gpu::DeviceInfo cur = gpu::currentDevice();
        std::printf("float controls (RoundingModeRTE + DenormPreserve, 32-bit) in the kernels: %s (driver reports: known %d, RTE %d, denorm preserve %d)\n", cur.floatControlsUsed ? "on" : "off", cur.fcKnown ? 1 : 0, cur.fcRteF32 ? 1 : 0, cur.fcDenormPreserveF32 ? 1 : 0);
    }
    const int sizes[][2] = {{160, 90}, {97, 61}};
    for (const std::string& id : gpuEffectIds()) {
        if (!only.empty() && id.find(only) == std::string::npos) continue;
        const Def* d = findDef(id);
        if (!d || !d->gpu) { std::printf("FAIL %s: no Def with a GPU plan\n", id.c_str()); ++gFail; continue; }
        for (const GpuCheckCase& cs : gpuCheckCases(*d)) {
            if (std::getenv("VKCASE") && cs.name != std::getenv("VKCASE")) continue;
            // (the size is the outer loop: the 8-bit and the float run of the same settings and size follow one another, which finds a cache that forgets the format)
            for (const auto& sz : sizes) {
                for (int mode = 0; mode < 3; ++mode) {            // 8-bit BGRA, 8-bit RGBA, float
                    const bool isFloat = mode == 2, bgra = mode != 1;
                    const int w = sz[0], h = sz[1];
                    const size_t bpp = isFloat ? 16 : 4, stride = (size_t)w * bpp;
                    std::vector<uint8_t> in(stride * h), a(stride * h, 0), b(stride * h, 0);
                    makeGpuTestPicture(w, h, isFloat, bgra, true, in.data(), stride);
                    const GpuTestSettings set(*d, cs);
                    Ctx c1, c2;
                    set.apply(c1);
                    set.apply(c2);
                    c1.fps = c2.fps = 25;
                    ++gChecks;
                    if (!renderFrameCpu(*d, c1, w, h, isFloat, bgra, in.data(), stride, a.data(), stride)) { std::printf("FAIL %s/%s: CPU render failed\n", id.c_str(), cs.name.c_str()); ++gFail; continue; }
                    gpu::RunStats st;
                    if (!renderFrameGpu(*d, c2, w, h, isFloat, bgra, in.data(), stride, b.data(), stride, &st)) {
                        if (!isFloat || cs.name.find("(float on the CPU)") != std::string::npos) { std::printf("DECLINED %s/%s %s: the plan hands this 8-bit picture to the CPU\n", id.c_str(), cs.name.c_str(), bgra ? "BGRA" : "RGBA"); ++gDeclined; continue; }
                        std::printf("FAIL %s/%s: GPU render failed\n", id.c_str(), cs.name.c_str());
                        ++gFail;
                        continue;
                    }
                    if (isFloat && std::getenv("VKDUMP")) { FILE* f = std::fopen(std::getenv("VKDUMP"), "wb"); std::fwrite(in.data(), 1, in.size(), f); std::fwrite(b.data(), 1, b.size(), f); std::fwrite(a.data(), 1, a.size(), f); std::fclose(f); }
                    if (!isFloat && std::getenv("VKPIX") && w * h > 0) { size_t q = (size_t)std::atoi(std::getenv("VKPIX")) * 4; if (q + 3 < b.size()) std::printf("  [%s %dx%d pixel %d: in %d %d %d %d cpu %d %d %d %d gpu %d %d %d %d]\n", cs.name.c_str(), w, h, std::atoi(std::getenv("VKPIX")), in[q],in[q+1],in[q+2],in[q+3],a[q],a[q+1],a[q+2],a[q+3],b[q],b[q+1],b[q+2],b[q+3]); }
                    double worst = 0;
                    size_t bad = 0, n = 0;
                    if (isFloat) {
                        const float* fa = (const float*)a.data();
                        const float* fb = (const float*)b.data();
                        for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
                            const double dd = std::fabs((double)fa[i] - (double)fb[i]);
                            if (dd > worst) worst = dd;
                            if (dd > (gFloatTol >= 0 ? gFloatTol : gpuFloatTolerance(*d) + 1e-5 * std::fabs((double)fa[i]))) { if (!bad && std::getenv("VKDBG")) { const float* fi = (const float*)in.data(); std::printf("  first bad at %zu: in %.9g cpu %g gpu %g | pixel in %.9g %.9g %.9g %.9g gpu %.9g %.9g %.9g %.9g\n", i, fi[i], fa[i], fb[i], fi[i/4*4], fi[i/4*4+1], fi[i/4*4+2], fi[i/4*4+3], fb[i/4*4], fb[i/4*4+1], fb[i/4*4+2], fb[i/4*4+3]); } ++bad; }
                            ++n;
                        }
                    } else {
                        for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
                            const int dd = std::abs((int)a[i] - (int)b[i]);
                            if (dd > worst) worst = dd;
                            if (dd > gStrict8) { if (!bad && std::getenv("VKDBG")) std::printf("  first bad at %zu (pixel %zu x=%zu ch %zu): in %d cpu %d gpu %d  [pixel in %d %d %d %d cpu %d %d %d %d gpu %d %d %d %d]\n", i, i / 4, (i / 4) % w, i % 4, (int)in[i], (int)a[i], (int)b[i], in[i/4*4],in[i/4*4+1],in[i/4*4+2],in[i/4*4+3],a[i/4*4],a[i/4*4+1],a[i/4*4+2],a[i/4*4+3],b[i/4*4],b[i/4*4+1],b[i/4*4+2],b[i/4*4+3]); ++bad; }
                            ++n;
                        }
                    }
                    if (bad) {
                        ++gFail;
                        std::printf("FAIL %s/%s %s %dx%d: worst difference %g, %zu of %zu values out of tolerance\n", id.c_str(), cs.name.c_str(),
                                    isFloat ? "float" : (bgra ? "8-bit BGRA" : "8-bit RGBA"), w, h, worst, bad, n);
                    }
                }
            }
        }
        std::printf("  %s done\n", id.c_str());
        // the device is made afresh for the next effect: a software device (SwiftShader) keeps every compiled kernel in
        // memory, and the whole list of them no longer fits in a test machine's memory
        gpu::shutdown();
    }
    std::printf("vkfx_test: %d comparisons, %d failures, %d 8-bit pictures declined to the CPU\n", gChecks, gFail, gDeclined);
    return gFail ? 1 : 0;
}
