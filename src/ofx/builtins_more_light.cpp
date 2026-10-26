// Built-in light effects (Lens Flare, Light Rays, Gradient Map); see builtins_more.cpp. (Rays, Glint, Starburst and Fill Light
// are VEGAS's managed CoreGraphics filters: builtins_velvet.cpp.)
#include "builtins_more_internal.h"
#include "builtins_fast.h"
#include "builtins_vec.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ light
// Light Rays: the bright parts streak away from the light source.
void lightRays(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    double lx, ly;
    c.xy("LightSource", lx, ly);
    const Color tint = c.color("TintColor");
    const double sens = clampd(c.d("group0Sensitivity"), 0, 1), strength = c.d("group0Strength") * 3;
    const double blend = clampd(c.d("group2Blend"), 0, 1);
    const double px0 = lx * W, py0 = (1 - ly) * H;
    Buf ray;
    marchTowardsCentre(c.src, ray, px0, py0, 0.6, 40, [&](const float* p, float* o) {
        const float a = p[3] > 1e-4f ? 1.f / p[3] : 0.f;
        const float l = luma(p[0] * a, p[1] * a, p[2] * a);
        const float w = std::max(0.f, l - (float)(1.0 - sens));
        for (int k = 0; k < 3; ++k) o[k] = p[k] * a * w;
        o[3] = 0.f;
    });
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* s = c.src.at(x, y);
            const float* acc = ray.at(x, y);
            float* o = c.out.at(x, y);
            const double tc[3] = {tint.r, tint.g, tint.b};
            for (int k = 0; k < 3; ++k) o[k] = std::min(1.f, s[k] + acc[k] * (float)(strength * tc[k]) * (1.f - (float)blend * 0.5f));
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
