// Vulkan plans of the filters and generators that follow VEGAS's own code on the CPU (TV Simulator, Film Grain, Film Effects, Light Rays, Color Gradient, ...).
// Each plan runs the per-pixel part of the CPU function named beside it on the card, from the numbers the CPU code works out per frame or per row; the kernels are
// tools/gpukernels_fx2.py's.
#pragma once
#include "vkfilters.h"
#include "vfxtv.h"
#include "vfxlightrays.h"
#include "vfxfilm.h"

namespace reno {
namespace ofx {
namespace builtin {

// builtins_more_creative.cpp: Film Grain's numbers of a frame (the noise table of 3 planes of 64 x 64, the table cells and fractions of each row and column)
struct FilmGrainSetup {
    double amount = 0, gran = 0, chroma = 0, stepX = 0, stepY = 0;
    std::vector<double> table, frac, colFrac;
    std::vector<int> rowA, rowB, colA, colB;
    int wt = 0;
};

namespace more {
void tvSimulatorGpu(Ctx& c, int W, int H, vftv::Rows8& o);       // builtins_more_creative.cpp
void filmGrainSetup(Ctx& c, int W, int H, FilmGrainSetup& g);
void filmEffectsGpu(Ctx& c, int W, int H, vfxfilm::Rec& rec, bool& bytes);   // builtins_more_creative.cpp (the frame with its picture work written down)
void lightRaysGpu(Ctx& c, int W, int H, vfxlr::Setup& o);       // builtins_more_light.cpp
}

// builtins_gen.cpp: Color Gradient's shape (v: sx, sy, ddx, ddy, dd, cx, cy, rx, ry, cos, sin) and stops (position, r, g, b, a each)
struct ColorGradientGpu { int type = 0; std::vector<double> stops; double v[11] = {0}; };
void colorGradientGpu(Ctx& c, ColorGradientGpu& o);

const std::vector<FilterGpuEntry>& fx2GpuEntries();     // vkfx2.cpp (appended to filterGpuEntries())

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
