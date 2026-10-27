// The built-in effects on the GPU: which effects have Vulkan kernels (vkeffects.cpp), and how a frame is sent to them from the built-in render function
// (builtins.cpp). The kernels are tools/gpukernels.py's; each effect's compute passes are planned from its settings by a function in vkeffects.cpp that
// mirrors what the CPU code reads.
#pragma once
#include "builtins_impl.h"
#include "vkgpu.h"
#include <map>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

// A frame as the host holds it (see gpu::FrameIO).
struct GpuFrame {
    const uint8_t* in = nullptr;
    size_t inRowBytes = 0;
    uint8_t* out = nullptr;
    size_t outRowBytes = 0;
    bool isFloat = false, bgra = true;
    uint64_t inSerial = 0, outSerial = 0;
    bool noInput = false;       // a generator
};

// Renders effect 'd' (its Def::gpu) on the GPU: false when it cannot (the GPU is off or absent, the settings need the CPU,
// the card said no), and nothing was written then.
bool gpuRender(const Def& d, Ctx& c, GpuFrame& f);

// builtins_magix.cpp: the three 1024-value tables of Color Curves (the master curve folded in), as its row function looks them up.
bool colorCurvesGpuTables(Ctx& c, std::vector<float>& out);
bool colorCurves8GpuTables(Ctx& c, std::vector<float>& out);   // (8-bit pictures: the 768 byte-table entries)

// builtins_gen.cpp: what the generators work out in double precision for their kernels
void solidColorGpu(Ctx& c, float p[4]);
void checkerboardGpu(Ctx& c, int W, int H, std::vector<float>& tab, float pa[4], float pb[4]);
void checkerboardGpuD(Ctx& c, int W, int H, std::vector<double>& sx, std::vector<double>& sy, double pa[4], double pb[4]);
bool checkerboard8Gpu(Ctx& c, int W, int H, std::vector<int>& t);
void testPatternGpu(Ctx& c, int W, int H, int& type, int& barW, std::vector<float>& tab);

// builtins_color.cpp: LUT Filter and Color Grading, as the kernels need them (false: the CPU just copies the frame)
struct ColorGradingGpu { uint32_t flags = 0; float f[21] = {0}; std::vector<float> tab; };
bool lutFilterGpu(Ctx& c, int& n, float dmin[3], float dmax[3], float& gain, bool& tetra, std::vector<float>& tab);
bool colorGradingGpu(Ctx& c, ColorGradingGpu& o);
bool autoLooksGpu(Ctx& c, int& n, float dmin[3], float dmax[3], float& g8, std::vector<float>& tab);      // AutoLooks (builtins_color.cpp)
int meshWarpGpu(Ctx& c, int& cols, int& rows, std::vector<float>& pts);
namespace more { void saturationAdjust8Gpu(Ctx& c, std::vector<float>& t); int lensFlareGpu(Ctx& c, int W, int H, std::vector<float>& tab, int& nflares, float& blend); }
struct Ccs8Gpu { int mode = 0, flags = 0; bool show = false, inv = false; float tintU = 0, tintV = 0, satF = 1, alphaF = 1, rot[4] = {1, 0, 0, 1}; std::vector<float> tab; };
void ccs8Gpu(Ctx& c, Ccs8Gpu& o);      // Color Corrector (Secondary), 8-bit (builtins_filters2.cpp)
int colorCorrector8Gpu(Ctx& c, std::vector<float>& tab);     // (builtins_filters2.cpp: returns the flags)
struct ColorMatchGpu { std::vector<float> rowsTable, paramTable; };
bool colorMatchGpu(Ctx& c, int W, int H, ColorMatchGpu& o);

// VEGAS's Deriche Gaussian as passes (rows into a scratch buffer, then columns into 'to'), for the transition planners (vktrans.cpp); sigmas in pixels, levels for 8-bit pictures.
void gpuDericheBlur(gpu::Plan& p, int from, int to, double sigmaX, double sigmaY, bool levels);

// ---- for the tests and reno_cli vkcheck
// The ids (with the "com.vegascreativesoftware:" part) of the built-in effects that have GPU kernels.
std::vector<std::string> gpuEffectIds();
// The settings of an effect at their defaults, in the form Ctx::testParams takes.
std::map<std::string, std::vector<double>> defaultTestParams(const Def& d);
// The effect on one frame, on the CPU (its render function, as render() in builtins.cpp loads, runs and stores) or on the GPU.
// 'c' holds the settings (testParams); the frame is w x h, rows bottom first, 8-bit BGRA/RGBA or float RGBA.
bool renderFrameCpu(const Def& d, Ctx& c, int w, int h, bool isFloat, bool bgra, const uint8_t* in, size_t inStride, uint8_t* out, size_t outStride);
bool renderFrameGpu(const Def& d, Ctx& c, int w, int h, bool isFloat, bool bgra, const uint8_t* in, size_t inStride, uint8_t* out, size_t outStride, gpu::RunStats* stats = nullptr, uint64_t inSerial = 0, uint64_t outSerial = 0);

// How far a float value may be from the CPU's (8-bit pictures: 1 level for every effect): 1e-4 + 1e-5 |v|, 2e-3 for effects that work out positions
// in the picture (a float cannot hold a coordinate of a thousand pixels better than 1e-4).
double gpuFloatTolerance(const Def& d);

// Settings to compare the CPU and the GPU at, for one effect: the defaults and a few that reach the other branches of its code.
struct GpuCheckCase {
    std::string name;
    std::map<std::string, std::vector<double>> params;      // overrides of the defaults
    std::map<std::string, std::string> strings;
    GpuCheckCase() {}
    GpuCheckCase(const std::string& n, const std::map<std::string, std::vector<double>>& p) : name(n), params(p) {}
    GpuCheckCase(const std::string& n, const std::map<std::string, std::vector<double>>& p, const std::map<std::string, std::string>& s) : name(n), params(p), strings(s) {}
};
// The settings of one check: the effect's defaults with the case's overrides; apply() hands them to a Ctx (which must not outlive it).
struct GpuTestSettings {
    std::map<std::string, std::vector<double>> params;
    std::map<std::string, std::string> strings;
    GpuTestSettings(const Def& d, const GpuCheckCase& cs);
    void apply(Ctx& c) const { c.testParams = &params; c.testStrings = &strings; }
};
std::vector<GpuCheckCase> gpuCheckCases(const Def& d);
// The test picture: a frame w x h with colours of every kind (ramps, hard edges, soft and zero alpha, noise), premultiplied,
// in the host's layout (rows bottom first, 'stride' bytes apart; 8-bit BGRA/RGBA or float RGBA). 'hdr' adds values above 1.
void makeGpuTestPicture(int w, int h, bool isFloat, bool bgra, bool hdr, uint8_t* out, size_t stride);

// vktrans.cpp: the planners of the transitions and generators (they take over the ids they list from vkeffects.cpp's own table), their test cases and float tolerances.
struct ExtraGpuEntry { const char* id; bool (*plan)(Ctx&, gpu::Plan&); };
const std::vector<ExtraGpuEntry>& extraGpuEntries();
void extraGpuCases(const std::string& fullId, std::vector<GpuCheckCase>& out);
bool extraGpuFloatTolerance(const std::string& fullId, double& tol);

// reno_cli vkcheck (vkcheck.cpp): args are the words after "vkcheck". Returns the program's exit code.
int runVkCheck(const std::vector<std::string>& args);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
