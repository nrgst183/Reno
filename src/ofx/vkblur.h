// Vulkan plans of the blur and light effects that follow VEGAS's OpenCL kernels (GPU acceleration on: Ctx::openCl). Each mirrors the CPU function named beside it (vfx*.h, builtins_*.cpp),
// formula for formula, so the card's picture is the CPU's. The kernels are tools/gpukernels_blur.py's. The plans of the effects with an older model too (Ctx::openCl off) stay where they are.
#pragma once
#include "vkeffects.h"
#include "vfxgauss.h"
#include "vfxflare.h"
#include "vfxlayerdim.h"
#include "vkfilters.h"

namespace reno {
namespace ofx {
namespace builtin {

// vfxblurs.h: the two recursions (down the columns with cy, then along the rows with cx, the 12-bit store when q8) from buffer 'from' into 'to' (their sum)
void gpuAddGaussPass(gpu::Plan& p, int from, int to, const vfxgauss::Coefsf& cf, bool vert, unsigned flags, bool fused, uint32_t w = 0, uint32_t h = 0);
void gpuAddGaussHalves(gpu::Plan& p, int from, int to, const vfxgauss::Coefsf& cx, const vfxgauss::Coefsf& cy, bool q8);

// builtins_more_blur.cpp: the numbers Radial Blur's OpenCL kernels get from the host (radialBlurOpenCl): s0, s1, w for the recursion, v56 the centre, the Gaussian's coefficients
namespace more {
struct RbOclParams { float s0 = 0, s1 = 0, w = 0, v56x = 0, v56y = 0, sigma = 0; vfxgauss::Coefsf cf; };
void radialBlurOclParams(Ctx& c, int H, RbOclParams& r);
vfxld::Settings ldOclSettings(Ctx& c);       // builtins_more_creative.cpp: Layer Dimensionality's settings as the OpenCL host cuts them
bool lensFlareOclSetup(Ctx& c, vfxflare::Params& P, const vfxflare::Row*& rows, int& nrows, float* ray);       // builtins_lensflare.cpp
}

bool planLayerDimensionalityOcl(Ctx& c, gpu::Plan& p);
bool planLensFlareOcl(Ctx& c, gpu::Plan& p);
bool planRadialBlurOcl(Ctx& c, gpu::Plan& p, const RbSetup& g);       // (g from radialBlurSetup: not a pass-through)
bool planRadialPixelateOcl(Ctx& c, gpu::Plan& p);
bool planFillLightOcl(Ctx& c, gpu::Plan& p);
bool planNewsprintOcl(Ctx& c, gpu::Plan& p);
bool planCookieCutterOcl(Ctx& c, gpu::Plan& p);
bool planLinearBlurOcl(Ctx& c, gpu::Plan& p);
bool planGaussianBlurOcl(Ctx& c, gpu::Plan& p);
bool planUnsharpMaskOcl(Ctx& c, gpu::Plan& p);
bool planGlowOcl(Ctx& c, gpu::Plan& p);
bool planChromaBlurOcl(Ctx& c, gpu::Plan& p);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
