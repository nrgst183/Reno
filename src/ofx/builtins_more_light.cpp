// Built-in light effects (Lens Flare, Light Rays, Gradient Map); see builtins_more.cpp. (Rays, Glint, Starburst and Fill Light
// are VEGAS's managed CoreGraphics filters: builtins_velvet.cpp.)
#include "vkfilters.h"
#include "builtins_more_internal.h"
#include "builtins_fast.h"
#include "vegas8.h"
#include "vfxlightrays.h"
#include "vfxgradmap.h"
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace more {

// ------------------------------------------------------------------ light
// Light Rays, from VEGAS's own code (sftrans1.dll, read and checked bit for bit against its render function; see vfxlightrays.h for what it does): the frame
// is taken as 8-bit pixels (blue first), drawn from the light source's pixel outwards in four quarters, each pixel a mean of samples of the result drawn so
// far on the way to the light, and mixed with the original; then the tint (as bytes) and Blend. VEGAS draws the final-quality way (bilinear samples). The light source's
// y is taken from the bottom of the frame as the other OpenFX positions are (VEGAS's own code counts rows down from the top). The noise generator is a
// state VEGAS keeps between frames; here it starts from the event time stamp so that a frame always comes out the same.
void lightRays(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double lx, ly;
    c.xy("LightSource", lx, ly);
    const Color tint = c.color("TintColor");
    vfxlr::Params pr;
    pr.lightX = lx;
    pr.lightY = 1 - ly;
    pr.sensitivity = clampd(c.d("group0Sensitivity"), 0, 1);
    pr.strength = clampd(c.d("group0Strength"), 0, 1);
    pr.boundRadius = c.i("group1BoundRadius") != 0;
    pr.radiusX = clampd(c.d("group1RadiusX"), 0, 1);
    pr.radiusY = clampd(c.d("group1RadiusY"), 0, 1);
    pr.feather = clampd(c.d("group1Feather"), 0, 1);
    // (VEGAS hands the colour over as bytes, cut: 0.983333 is 250, not 251)
    pr.tintR = (int)(clampd(tint.r, 0, 1) * 255.0) / 255.0;
    pr.tintG = (int)(clampd(tint.g, 0, 1) * 255.0) / 255.0;
    pr.tintB = (int)(clampd(tint.b, 0, 1) * 255.0) / 255.0;
    pr.blend = clampd(c.d("group2Blend"), 0, 1);
    pr.noise = clampd(c.d("group2Noise"), 0, 1);
    std::vector<uint8_t> in((size_t)W * H * 4), out;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* s = c.src.at(x, y);
            uint8_t* d = &in[((size_t)y * W + x) * 4];
            d[0] = (uint8_t)v8::toLevel(s[2]);
            d[1] = (uint8_t)v8::toLevel(s[1]);
            d[2] = (uint8_t)v8::toLevel(s[0]);
            d[3] = (uint8_t)v8::toLevel(s[3]);
        }
    out = in;
    int32_t seed = (int32_t)((uint32_t)c.legacySeedTime() & 0xffffu);
    vfxlr::render(pr, W, H, W, in.data(), out.data(), seed, true);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const uint8_t* d = &out[((size_t)y * W + x) * 4];
            float* o = c.out.at(x, y);
            o[0] = v8::fromLevel(d[2]);
            o[1] = v8::fromLevel(d[1]);
            o[2] = v8::fromLevel(d[0]);
            o[3] = v8::fromLevel(d[3]);
        }
}

// Gradient Map, from VEGAS's own code (vfx1.dll, read and checked bit for bit against its render function on random gradients and pictures; see vfxgradmap.h): the stops
// ("GradientStops": position r g b a;...) make an alpha curve and a colour curve through VEGAS's key-frame spline, a grey picks the gradient's colour and alpha from a 256-entry table
// (8-bit frames; float frames read the curves at the grey itself), and Amount and Mode (Blend / Add) put it over the picture. No stops: VEGAS's "Reset to None", the picture as it was.
void gradientMap(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    c.out.px = c.src.px;
    if (W <= 0 || H <= 0) return;
    vfxgm::Gradient g;
    g.amount = clampd(c.d("Amount"), 0, 1);
    g.mode = c.i("Mode") == 1 ? 1 : 0;
    g.stops = vfxgm::parseStops(c.s("GradientStops"));
    if (g.stops.empty()) return;
    if (c.levels) {
        // VEGAS's 8-bit routine: the frame as premultiplied bytes, blue first
        std::vector<uint8_t> in((size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* s = c.src.at(x, y);
                uint8_t* d = &in[((size_t)y * W + x) * 4];
                d[0] = (uint8_t)v8::toLevel(s[2]);
                d[1] = (uint8_t)v8::toLevel(s[1]);
                d[2] = (uint8_t)v8::toLevel(s[0]);
                d[3] = (uint8_t)v8::toLevel(s[3]);
            }
        vfxgm::run8(in.data(), W * 4, W, H, g);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const uint8_t* d = &in[((size_t)y * W + x) * 4];
                float* o = c.out.at(x, y);
                o[0] = v8::fromLevel(d[2]);
                o[1] = v8::fromLevel(d[1]);
                o[2] = v8::fromLevel(d[0]);
                o[3] = v8::fromLevel(d[3]);
            }
    } else {
        std::vector<float> in((size_t)W * H * 4);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* s = c.src.at(x, y);
                float* d = &in[((size_t)y * W + x) * 4];
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
            }
        vfxgm::runFloat(in.data(), W * 16, W, H, g);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                const float* d = &in[((size_t)y * W + x) * 4];
                float* o = c.out.at(x, y);
                o[0] = d[2]; o[1] = d[1]; o[2] = d[0]; o[3] = d[3];
            }
    }
}

}  // namespace more
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
