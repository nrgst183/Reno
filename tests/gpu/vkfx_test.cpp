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

int main(int argc, char** argv)
{
    std::string only;
    for (int k = 1; k < argc; ++k) if (!std::strcmp(argv[k], "--only") && k + 1 < argc) only = argv[++k];
    gpu::setEnabled(true);
    std::string why;
    if (!gpu::ready(&why)) {
        std::printf("vkfx_test: SKIPPED - %s\n", why.c_str());
        return 0;
    }
    std::printf("using %s\n", gpu::status().c_str());
    const int sizes[][2] = {{160, 90}, {97, 61}};
    for (const std::string& id : gpuEffectIds()) {
        if (!only.empty() && id.find(only) == std::string::npos) continue;
        const Def* d = findDef(id);
        if (!d || !d->gpu) { std::printf("FAIL %s: no Def with a GPU plan\n", id.c_str()); ++gFail; continue; }
        for (const GpuCheckCase& cs : gpuCheckCases(*d)) {
            for (int mode = 0; mode < 3; ++mode) {            // 8-bit BGRA, 8-bit RGBA, float
                const bool isFloat = mode == 2, bgra = mode != 1;
                for (const auto& sz : sizes) {
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
                        std::printf("FAIL %s/%s: GPU render failed\n", id.c_str(), cs.name.c_str());
                        ++gFail;
                        continue;
                    }
                    double worst = 0;
                    size_t bad = 0, n = 0;
                    if (isFloat) {
                        const float* fa = (const float*)a.data();
                        const float* fb = (const float*)b.data();
                        for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
                            const double dd = std::fabs((double)fa[i] - (double)fb[i]);
                            if (dd > worst) worst = dd;
                            if (dd > gpuFloatTolerance(*d) + 1e-5 * std::fabs((double)fa[i])) ++bad;
                            ++n;
                        }
                    } else {
                        for (size_t i = 0; i < (size_t)w * h * 4; ++i) {
                            const int dd = std::abs((int)a[i] - (int)b[i]);
                            if (dd > worst) worst = dd;
                            if (dd > 1) ++bad;
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
    }
    std::printf("vkfx_test: %d comparisons, %d failures\n", gChecks, gFail);
    return gFail ? 1 : 0;
}
