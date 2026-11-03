// Vulkan plans of the effects that convolve by FFT in double precision on the CPU: Defocus and Starburst. The FFT runs on the card in software float64 (tools/gpukernels_fft.py),
// butterfly for butterfly the CPU's; what the CPU works out once per frame in other ways (the iris and star pictures, the twiddle factors, the tile grid, the Soften edges mask) is
// worked out by the CPU's own code (the Setup functions below) and handed over.
#pragma once
#include "vkfilters.h"

namespace reno {
namespace ofx {
namespace builtin {

// Defocus (defocusRenderGpu in builtins_defocus.cpp): what it works out before the tiles
struct DefocusGpuSetup {
    bool passThrough = false;       // no iris to convolve with: the (Reduce flicker blurred) picture is only clamped
    bool bloom = false;
    float bloomT = 0.f, bloomK = 1.f;
    int half = 0, T = 0, ov = 0, nx = 0, ny = 0, step = 0, N = 1;
    std::vector<float> mask;        // (2 half)^2 byte values, row by row
    double scale = 0;
    std::vector<double> fw, inv;    // the twiddles of the forward and inverse transform: T / 2 complex numbers (re, im) each
};
void defocusGpuSetup(int W, int H, double radiusPx, int shape, double orientDeg, double curvature, double thresh, double strength, DefocusGpuSetup& o);

// Starburst, the OpenCL path (starburstVelvet in builtins_velvet.cpp)
struct StarburstGpuSetup {
    int N = 1;
    std::vector<float> soft;        // the Soften edges mask (a float per pixel, top row first); empty: none
    bool passThrough = false;       // Strength too small to show: the picture itself (Effect only: nothing)
    bool reduceFlicker = false, thr = false, boost = false, effectOnly = false;
    float thrLevel = 0.f, boostT = 0.f, boostS = 0.f;
    int sn = 0;                     // the star's bitmap is sn x sn bytes (0: no star)
    std::vector<uint32_t> starBytes;    // four bytes to a word
    float starScale = 0.f;          // (float)(scale) the bytes are multiplied by
    double norm = 1.0, opacity = 1.0;
    std::vector<double> fw, inv;    // the twiddles of the forward and inverse transform: stage by stage, N - 1 complex numbers (re, im) each
};
namespace more { bool starburstGpuSetup(Ctx& c, int W, int H, StarburstGpuSetup& o); }

// Flicker Control (flickerRender in builtins_analysis_flicker.cpp): kind 0 stays on the CPU, 1 the frame passes through, 2 the temporal blend, 3 Addition. The blend takes up to 6 frames: the frame
// itself is the slot selfk (the card's own picture), the others are fetched by the CPU and sent as 'frames' (W x H x 4 floats each, in the slots' order).
namespace analysis {
struct FlickerGpuSetup {
    int kind = 0, type = 0, nf = 0, selfk = -1, cen = 0, before = 0;
    bool showMask = false, q8 = false, motion = false;
    float map[9] = {0}, fw[6] = {0}, amount = 0.f;
    std::vector<float> frames;
};
bool flickerGpuSetup(Ctx& c, int W, int H, FlickerGpuSetup& o);
}

const std::vector<FilterGpuEntry>& fftGpuEntries();     // vkfft.cpp (appended to filterGpuEntries())

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
