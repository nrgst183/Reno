// Shared by the "analysis" group of built-ins (builtins_analysis*.cpp): Stabilize, Motion Tracker, Flicker Reducer, the denoisers, Colorization and Style Transfer.
#pragma once
#include "builtins_impl.h"

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {

extern const char* const kGroup;

// A Def with the group and the "standing in for VEGAS ..." note every effect of the group carries.
Def make(const char* id, const char* label, const char* what, std::vector<PDef> params, void (*fn)(Ctx&));

// A 2D affine map in the units motion is worked out in: x right, y down, the frame's centre the origin, the frame's HEIGHT one unit (independent of render size).
struct Xf {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
    void apply(double x, double y, double& ox, double& oy) const { ox = a * x + b * y + tx; oy = c * x + d * y + ty; }
    Xf operator*(const Xf& o) const;                        // this after o
    Xf inverse() const;                                     // the identity for a map that cannot be turned round
};

// Straight (not premultiplied) pixel of premultiplied float RGBA; Rec. 601 YUV as VEGAS's denoiser (matrix from its OpenCL kernels: 0.299 0.587 0.114 / -0.14713 -0.28886 0.436 /
// 0.615 -0.51499 -0.10001; back 1.13983, -0.39465, -0.58060, 2.03211).
inline void rgbToYuv(float r, float g, float b, float& y, float& u, float& v)
{
    y = 0.299f * r + 0.587f * g + 0.114f * b;
    u = -0.14713f * r - 0.28886f * g + 0.436f * b;
    v = 0.615f * r - 0.51499f * g - 0.10001f * b;
}
inline void yuvToRgb(float y, float u, float v, float& r, float& g, float& b)
{
    r = y + 1.13983f * v;
    g = y - 0.39465f * u - 0.58060f * v;
    b = y + 2.03211f * u;
}

// The groups' pieces, each in its own file; builtins_analysis.cpp calls them in order.
void addStabilizeEffects(std::vector<Def>& out);
void addFlickerEffect(std::vector<Def>& out);
void addDenoiseEffects(std::vector<Def>& out);
void addAiEffects(std::vector<Def>& out);

// Whole-frame motion between two frames (builtins_analysis_motion.cpp), shared by Stabilize, Flicker Reducer.
// A frame in grey, at analysis size.
struct Gray {
    int w = 0, h = 0;
    std::vector<float> p;           // top row first, 0..1
    double alpha = 1;
    float at(int x, int y) const { return p[(size_t)y * (size_t)w + (size_t)x]; }
    // Bilinear, pixel centres at whole numbers, edges clamped.
    float sample(double x, double y) const;
};
// A frame in grey at most 'maxW' wide (keeping its shape), averaging the pixels that fall into each new one.
Gray makeGray(const Buf& b, int maxW);
Gray halfGray(const Gray& g);
// A signature of a frame (a 16 x 9 grid of its mean grey): two pictures of the same frame at different sizes are close.
struct Signature { float v[16 * 9]; };
Signature signatureOf(const Gray& g);
bool sameSignature(const Signature& a, const Signature& b);

// How the movement of the whole picture between two frames is found (builtins_analysis_motion.cpp): patches on a grid in a region are followed from one frame to the next by block
// matching (a global shift first, at a coarse level, for fast movement), and a model (shift, shift and turn, similarity, or full affine) is fitted to the vectors with outliers thrown out.
struct FeatPt { float x, y, vx, vy; bool inlier; };
struct EstParams {
    double roiCx = 0.5, roiCy = 0.5, roiW = 0.85, roiH = 0.8;
    int gridX = 14, gridY = 9, perCell = 2;
    int model = 2;
    bool sad = false;
    bool lk = true;
    bool accurate = true;
    bool keepPoints = false;                                      // fill PairResult::pts
};
struct PairResult {
    Xf xf;
    double conf = 0;
    bool ok = false;
    std::vector<FeatPt> pts;
};
// A and B are the same size. False (xf the identity) when there is too little to follow.
bool estimatePair(const Gray& A, const Gray& B, const EstParams& ep, PairResult& out);
// The shift of the whole picture, found coarsely over the whole of both (|shift| <= range pixels): where A's content is in B.
bool globalShift(const Gray& A, const Gray& B, int range, double& dx, double& dy);

}  // namespace analysis
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
