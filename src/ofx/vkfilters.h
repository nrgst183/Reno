// Vulkan plans of the built-in FILTERS that are bit-identical to their CPU code (the transitions and the older effects are in vkeffects.cpp).
// The CPU row functions stay the truth: where a plan needs numbers the CPU code works out, that code exports them from here.
#pragma once
#include "vkeffects.h"
#include <map>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

// builtins_filters.cpp: Sepia's integer settings
struct SepiaSettings {
    int toGrey = 0;
    int colour[3] = {0, 0, 0};
    int lut[256] = {};
    bool useLut = false, greyMix = false;
};
void sepiaSettingsGpu(Ctx& c, SepiaSettings& o);

// builtins_cookie.cpp: Cookie Cutter's set-up numbers (single precision, as VEGAS has them)
struct CookieSetup {
    int shape = 0, repX = 1, repY = 1, cw = 1, ch = 1;
    bool cutSection = false, noBorder = true, noFeather = true;
    float fw2 = 0, cxCell = 0, cyCell = 0, ratio = 1, tIn = 0, tOut = 0, rIn = 0, m1 = 0, ovalW = 0, ovalH = 0, hwF = 0, hhF = 0, boxHi = 0, boxLo = 0, nTout = 0;
    float hx = 0, hy = 0, ix0 = 0, iy0 = 0, br = 0, bg = 0, bb = 0;
    int colour[4] = {0, 0, 0, 255};
};
void cookieSetup(Ctx& c, int W, int H, CookieSetup& o);
void convolutionKernel8Setup(Ctx& c, float* A, float& off, bool& alphaPlain);   // builtins_filters2.cpp

// builtins_filters.cpp: Chroma Keyer's set-up numbers
struct ChromaKeyerSetup {
    double kh = 0, ks = 0, kl = 0;
    float lowF = 0, hiF = 0, krF = 0, kgF = 0, kbF = 0, invRange = 0, coefs[25] = {};
    bool blur = false, maskOnly = false;
    int despill = 0;
};
void chromaKeyerGpuSetup(Ctx& c, int W, int H, ChromaKeyerSetup& o);

// builtins_filters.cpp: Border's set-up numbers and per-row bevel spans
struct BorderSpans { int end[5]; int cov[5]; int face[5][4]; bool hasFace[5]; };
struct BorderGeo {
    int W = 0, H = 0, type = 0, xi = 0, xr = 0, yb = 0, yt = 0, rads[3] = {0, 0, 0}, F[4][4] = {};
    float Wf = 0, Hf = 0, bx = 0, by = 0, cc[4] = {0, 0, 0, 0};
    BorderSpans ys = {};
};
void borderGeometry(Ctx& c, int W, int H, BorderGeo& g);
void borderBevelRow(const BorderGeo& g, int y, BorderSpans& xs);

// A filter planned in vkfilters.cpp: its id (without the prefix), the planner, the float tolerance (0: bit-identical) and its check cases.
// (A null planner: the effect is planned in vkeffects.cpp, this entry only adds check cases.)
struct VignetteSetup { float cxp = 0, cyp = 0, cs = 1, sn = 0, g = 1, expo = 1, rInner = 0, ramp = 1, ca = 1, cf[3] = {0, 0, 0}; int cb[4] = {0, 0, 0, 0}; bool invert = false; };
struct PipSetup { int mode = 0; double sx = 1, sy = 1, cs = 1, sn = 0, shear = 0, op = 1, lxW = 0, lyH = 0, axW = 0, ayH = 0, hm[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; float M[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; };
struct MedianSetup { int kx = 1, ky = 1, r12 = 0; long long whole = 0; std::vector<int> cells, steps; };       // triples (dy, dx, weight) / (dy, dx, change)
void medianIntSetup(int W, int H, double rxd, double ryd, bool round, double offset, MedianSetup& m);       // builtins_vegasmedian.cpp
struct NewsGeo { bool ok = false; int W = 0, H = 0, mode = 0, sw = 0, sh = 0, nbx = 0, nby = 0, par = 0, fadeT = -1; double dot = 0, P = 0, x2 = 0, y2 = 0, fxo = 0, fyo = 0;
                 long long dRx = 0, dRy = 0, sx = 0, sy = 0, nrm = 0, maxX = 0, maxY = 0; };
struct NewsAxisCell { int S = 0, E = 0, F = 0, L = 0; };
void newsprintAxis(int dR, int step, int cells, std::vector<NewsAxisCell>& out);
void newsprintGeometry(Ctx& c, int W, int H, NewsGeo& g);      // builtins_newsprint.cpp
void newsprintCoverTable(int* t);
struct BbfSetup { double fx = 0, fy = 0, bx = 0, by = 0, fs = 1, bs = 1, blur = 0, shadow = 0, sigma = 0; bool bgWhole = false, fgWhole = false, bgBlurred = false; int bf = 1, sf = 1; };
struct Bbf8Setup { float tab[256]; float mBg[9], mFg[9]; double bsize = 0, shadowSize = 0; float s8 = 0; };
struct LdSetup {      // Layer Dimensionality's settings, shared by the CPU effect and the GPU plan (layerDimensionality() in builtins_more_creative.cpp)
    int shadowType = 0, glowType = 0;
    float fgOp = 1.f, amount = 0.f;
    double sx = 0, sy = 0, bsz = 0;                // inner shadow: the alpha's sub-pixel shift, the Deriche size
    int ox = 0, oy = 0;                            // other shadows: the whole-pixel shift
    double blur = 0;
    float gc[3] = {0, 0, 0};
    double glowRad = 0;
    bool emboss = false;
    double eox = 0, eoy = 0, sigma = 0;
    float gain = 0.f;
};
struct LensSetup { bool plain = true; float cxp = 0, cyp = 0, d = 1, x14 = 0, x3 = 0, x13 = 0, s = 1, iw = 0, ih = 0; };
bool lensSetupGpu(Ctx& c, int W, int H, LensSetup& o);      // builtins_mask.cpp (lensCorrection())
struct RbSetup { bool pass = true, q8 = false; int type = 0, cx = 0, cy = 0; double s0 = 0, s1 = 0, wf = 0, iwf = 0; long long wi = 0, iwi = 0; };
struct RpixSetup { bool pass = true, q8 = false; int gw = 1, gh = 1, sx = 0, sy = 0, bw = 1, bh = 1; float c0 = 0, c1 = 0; double inv = 1; };
struct FillLightSetup { bool copy = true, blur = false; uint8_t lut[256][4]; double sigma = 0; float fop = 0; };
struct ScSetup { int ext = 0; int stl[256]; bool blur = false, trim = false, tint = false, vscale = false; double sigma = 0; int tl[256]; int tl3[256][3]; int oq = 0, vq = 255; std::vector<float> vmask; };
struct DbGeom { int mode = 0; double sigma = 0, t = 0; bool cols = false, rev = false; int SW = 0, SH = 0; };
struct GlintSetup { int maskMode = 0, n = 0; bool effectOnly = false, flicker = false; std::vector<float> mask; int tlut[256]; float scale = 1.f; uint8_t clut[256][4]; std::vector<DbGeom> streaks; };
namespace more { bool glintSetup(Ctx& c, int W, int H, GlintSetup& o); void shearParamsGpu(int lines, double t, std::vector<float>& tab); }   // builtins_velvet.cpp
struct PolarGpu { int nT = 0, rN = 0; double px0 = 0, py0 = 0; std::vector<float> cs, sn; std::vector<int> rEnd; };
bool polarGeomGpu(int W, int H, double px0, double py0, double density, PolarGpu& o);   // builtins_fast2.cpp
struct LrSetup { double px0 = 0, py0 = 0; float sensF = 0, kk[3] = {0, 0, 0}, fb = 1; };
namespace more { void lightRaysSetup(Ctx& c, int W, int H, LrSetup& o); }   // builtins_more_light.cpp
struct PanoSetup { bool copy = false; int n = 1, px[2] = {0, 0}, py[2] = {0, 0}, pw = 0, ph = 0; std::vector<float> map; };
bool panoramaSetupGpu(Ctx& c, int W, int H, PanoSetup& o);   // builtins_vegasrest.cpp (360 Stabilization)
struct SceneSetup { int stereo = 0; bool levels = false; std::vector<float> map; };
bool sceneRotationSetupGpu(Ctx& c, int W, int H, SceneSetup& o);   // builtins_vegasrest.cpp (Scene Rotation)
struct RaysSetup { struct Pass { std::vector<int> xi, yi; std::vector<float> xf, yf; }; int maskMode = 0; bool alpha = false, effectOnly = false, flicker = false, noise = false; std::vector<float> mask, tex, nxf, nyf; std::vector<int> nxi, nyi; float tlut[256]; uint8_t clut[256][4]; std::vector<Pass> passes; };
namespace more { bool raysSetup(Ctx& c, int W, int H, RaysSetup& o); bool softContrastSetup(Ctx& c, int W, int H, ScSetup& o); bool fillLightSetup(Ctx& c, int W, FillLightSetup& o); void radialBlurSetup(Ctx& c, int W, int H, RbSetup& g); bool radialBlurSteps(const RbSetup& g, int W, int H, std::vector<uint32_t>& rec, std::vector<uint32_t>& starts, int maxSteps); void rpixSetup(Ctx& c, int W, int H, RpixSetup& o); void ldSetup(Ctx& c, int W, int H, LdSetup& o); void bbf8Setup(Ctx& c, int W, int H, Bbf8Setup& o); void rank8GpuKernel(Ctx& c, int W, int H, std::vector<float>& q, int& nx, int& ny); bool rank8Applies(int W, int H); void bbfSetup(Ctx& c, int W, int H, BbfSetup& o); void pipSetup(Ctx& c, int W, int H, PipSetup& o); void vignetteSetup(Ctx& c, int W, int H, VignetteSetup& o); void labAdjustGpuTables(std::vector<float>& t); void labAdjust8GpuTable(std::vector<float>& t); void broadcastColors8GpuTable(Ctx& c, std::vector<float>& t); void whiteBalanceSettings(Ctx& c, std::vector<float>& tab, bool& shift, bool& hasData); }   // builtins_more_colour.cpp

struct FilterGpuEntry {
    const char* id;
    bool (*plan)(Ctx&, gpu::Plan&);
    double floatTol;
    std::vector<GpuCheckCase> cases;
};
const std::vector<FilterGpuEntry>& filterGpuEntries();
void gpuAddDeriche(gpu::Plan& p, int from, int to, double sigmaX, double sigmaY, bool levels, bool rawOut = false);   // VEGAS's Deriche Gaussian (kernel "deriche")
std::vector<float> gaussKernelTable(double sigma);      // builtins.cpp: the float taps of the generic Gaussian (index 0 = centre)
void tinyPlanetMapGpu(Ctx& c, int W, int H, std::vector<uint32_t>& idx);      // builtins_magix.cpp: the source pixel of every output pixel of VEGAS's Tiny Planet
bool stitchMapGpu(Ctx& c, int W, int H, std::vector<float>& rec, bool& levels, float& noGamma);      // builtins_vr.cpp
bool stereoTapsGpu(Ctx& c, int W, int H, std::vector<float>& rec, unsigned& full, unsigned& part);      // builtins_stereo.cpp
void bezierMaskGpu(Ctx& c, int W, int H, int& mode, bool& invert, float& blend, std::string& key, std::vector<float>* total);      // builtins_mask_bezier.cpp
bool gpuPlanMinMax8(Ctx& c, gpu::Plan& p);      // 8-bit Min and Max (the rank filter), for planMinMax in vkeffects.cpp
void gpuPushDouble(std::vector<float>& tab, double v);      // a double as two floats holding its bits (tables read by ld_sf in the kernels)

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
