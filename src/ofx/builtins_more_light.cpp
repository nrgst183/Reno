// Built-in light effects (Lens Flare, Light Rays, Gradient Map); see builtins_more.cpp. (Rays, Glint, Starburst and Fill Light
// are VEGAS's managed CoreGraphics filters: builtins_velvet.cpp.)
#include "vkfilters.h"
#include "builtins_more_internal.h"
#include "builtins_fast.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ light
// Light Rays: the bright parts streak away from the light source.
// What Light Rays works out from the settings alone (shared by the CPU code and the GPU plan)
void lightRaysSetup(Ctx& c, int W, int H, LrSetup& o)
{
    double lx, ly;
    c.xy("LightSource", lx, ly);
    const Color tint = c.color("TintColor");
    const double sens = clampd(c.d("group0Sensitivity"), 0, 1), strength = c.d("group0Strength") * 3;
    const double blend = clampd(c.d("group2Blend"), 0, 1);
    o.px0 = lx * W;
    o.py0 = (1 - ly) * H;
    o.sensF = (float)(1.0 - sens);
    const double tc[3] = {tint.r, tint.g, tint.b};
    for (int k = 0; k < 3; ++k) o.kk[k] = (float)(strength * tc[k]);
    o.fb = 1.f - (float)blend * 0.5f;
}

void lightRays(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    LrSetup su;
    lightRaysSetup(c, W, H, su);
    Buf ray;
    marchTowardsCentre(c.src, ray, su.px0, su.py0, 0.6, 40, [&](const float* p, float* o) {
        const float a = p[3] > 1e-4f ? 1.f / p[3] : 0.f;
        const float l = luma(p[0] * a, p[1] * a, p[2] * a);
        const float w = std::max(0.f, l - su.sensF);
        for (int k = 0; k < 3; ++k) o[k] = p[k] * a * w;
        o[3] = 0.f;
    });
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s = c.src.at(x, y);
            const float* acc = ray.at(x, y);
            float* o = c.out.at(x, y);
            for (int k = 0; k < 3; ++k) o[k] = std::min(1.f, s[k] + acc[k] * su.kk[k] * su.fb);
            o[3] = s[3];
        }
    });
}

void gradientMap(Ctx& c)
{
    const double amount = clampd(c.d("Amount"), 0, 1);
    const bool add = c.i("Mode") == 1;
    perPixel(c, [&](float& r, float& g, float& b, int, int) {
        const float l = luma(r, g, b);
        float gr = clamp01(l * 3), gg = clamp01(l * 3 - 1), gb = clamp01(l * 3 - 2);
        if (add) { r = clamp01(r + (gr - 0.5f) * (float)amount); g = clamp01(g + (gg - 0.5f) * (float)amount); b = clamp01(b + (gb - 0.5f) * (float)amount); }
        else { r += (gr - r) * (float)amount; g += (gg - g) * (float)amount; b += (gb - b) * (float)amount; }
    });
}

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
