// Remaining VEGAS Pro video effects not in Reno's other built-in files, and the groups VEGAS gathers settings in.
//   applyVegasGroups   adds groups (Border in the transitions, Luma / Chroma / Composite in Broadcast Colors, Shadow / Glow / Emboss in Layer
//                      Dimensionality ...) to existing built-ins, with the labels of VEGAS's resource files (Vfx1.xml). Group membership is read from
//                      the resource file order (a group first, its members after) and the settings' names (BorderSize, BorderFeather, BorderColor ...).
//   addFiltersVegasRest  360 Stabilization (com.magix.ofx.vr.stabilization), Scene Rotation (com.magixsoftware:panorotation), Displacement Map,
//                      Height Map, Histogram and Gradient Wipe.
#include "builtins_impl.h"
#include "builtins_vfxdata.h"
#include "vkfilters.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace reno {
namespace ofx {
namespace builtin {

namespace {

const char* const kGroup = "Reno Built-in";
const char* const kVegas = "com.vegascreativesoftware:";

// One group to add to an effect: its name and label, and its settings (names starting with 'prefix', plus those in 'names' separated by '|').
struct GroupSpec { const char* effect; const char* name; const char* label; const char* prefix; const char* names; };

const GroupSpec kGroups[] = {
    {"barndoor", "Border", "Border", "Border", ""},
    {"push", "Border", "Border", "Border", ""},
    {"slide", "Border", "Border", "Border", ""},
    {"spiral", "Border", "Border", "Border", ""},
    {"split", "Border", "Border", "Border", ""},
    {"squeeze", "Border", "Border", "Border", ""},
    {"swap", "Border", "Border", "Border", ""},
    {"zoom", "Border", "Border", "Border", ""},
    {"dissolve", "ColorBleedSpeed", "Color bleed speed", "ColorBleedSpeed", ""},
    {"dissolve", "ColorMorphSpeed", "Color morph speed", "ColorMorphSpeed", ""},
    {"crosseffect", "EffectSettings", "Effect settings", "EffectSettings", ""},
    {"blackbarfill", "Foreground", "Foreground", "Foreground ", ""},
    {"blackbarfill", "Background", "Background", "Background ", ""},
    {"broadcastcolors", "Luma", "Luma", "Luma", ""},
    {"broadcastcolors", "Chroma", "Chroma", "Chroma", ""},
    {"broadcastcolors", "Composite", "Composite", "Composite", ""},
    {"bumpmapfilter", "Location", "Location", "Location", ""},
    {"channelblend", "Group RED", "Red Output", "RED ", ""},
    {"channelblend", "Group GREEN", "Green Output", "GREEN ", ""},
    {"channelblend", "Group BLUE", "Blue Output", "BLUE ", ""},
    {"channelblend", "Group ALPHA", "Alpha Output", "ALPHA ", ""},
    {"deform", "Squeeze", "Squeeze", "Squeeze", ""},
    {"deform", "Shear", "Shear", "Shear", ""},
    {"deform", "Curve", "Curve", "Curve", ""},
    {"filmeffects", "EnableParticles", "Enable particles", "Enable", ""},
    {"glintvelvetmatter", "Mask", "Mask", "Mask", ""},
    {"raysvelvetmatter", "Mask", "Mask", "Mask", ""},
    {"layerdimensionality", "Shadow", "Shadow", "Shadow", ""},
    {"layerdimensionality", "Glow", "Glow", "Glow", ""},
    {"layerdimensionality", "Emboss", "Emboss", "",
     "ForegroundAmount|BackgroundAmount|LightIntensity|LightAmbience|EmbossRoundness|Shininess|ForegroundOpacity|LightLocation"},
    {"lightrays", "group0", "Group0", "group0", ""},
    {"lightrays", "group1", "Group1", "group1", ""},
    {"lightrays", "group2", "Group2", "group2", ""},
    {"pictureinpicture", "Points", "Corner points", "Corner", ""},
    {"softcontrastvelvetmatter", "Vignette", "Vignette", "Vignette", ""},
};

bool listed(const std::string& names, const std::string& n)
{
    size_t at = 0;
    while (at <= names.size()) {
        size_t e = names.find('|', at);
        if (e == std::string::npos) e = names.size();
        if (e > at && names.compare(at, e - at, n) == 0 && e - at == n.size()) return true;
        at = e + 1;
    }
    return false;
}


// ------------------------------------------------------------------ helpers
Def make(const std::string& id, const char* label, const char* group, const char* what, std::vector<PDef> params, void (*fn)(Ctx&))
{
    Def d;
    d.id = id;
    d.label = label;
    d.group = kGroup;
    (void)group;
    d.description = std::string(what) + " (Reno's own, standing in for VEGAS " + label + ").";
    d.params = std::move(params);
    d.render = fn;
    return d;
}

void copyFrame(Ctx& c)
{
    if (c.out.px.size() == c.src.px.size()) c.out.px = c.src.px;
}

// A 3 x 3 rotation (row by row) of directions: x to the right, y up, z ahead.
struct Rot {
    double m[9];
    void apply(double x, double y, double z, double& ox, double& oy, double& oz) const
    {
        ox = m[0] * x + m[1] * y + m[2] * z;
        oy = m[3] * x + m[4] * y + m[5] * z;
        oz = m[6] * x + m[7] * y + m[8] * z;
    }
};
Rot mul(const Rot& a, const Rot& b)
{
    Rot r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = a.m[i * 3] * b.m[j] + a.m[i * 3 + 1] * b.m[3 + j] + a.m[i * 3 + 2] * b.m[6 + j];
    return r;
}
// About the vertical axis: a direction straight ahead is turned by 'a' to the right (its longitude grows by a).
Rot rotYaw(double a) { const double c = std::cos(a), s = std::sin(a); return Rot{{c, 0, s, 0, 1, 0, -s, 0, c}}; }
// About the sideways axis: straight ahead is turned up by 'a' (its latitude grows by a).
Rot rotPitch(double a) { const double c = std::cos(a), s = std::sin(a); return Rot{{1, 0, 0, 0, c, -s, 0, s, c}}; }
// About the line of sight: a direction to the right is turned down by 'a'.
Rot rotRoll(double a) { const double c = std::cos(a), s = std::sin(a); return Rot{{c, s, 0, -s, c, 0, 0, 0, 1}}; }

// An equirectangular picture seen through a rotation: output pixel (x, y) of a W x H picture takes the colour of the direction the rotation turns
// it to. The picture is the part [px, px + pw) x [py, py + ph) of the frame (stereo halves).
// Where output pixel (x, y) of a pw x ph picture reads from: the two pixels either way (x0, y0 and the next) and the weights of the second ones. cl, sl: the cos and sin of the row's latitude.
struct PanoTap { int x0, y0; float tx, ty; };
inline PanoTap panoTap(const Rot& rot, double wm, int pw, int ph, int x, double cl, double sl)
{
    const double lon = ((double)x / wm - 0.5) * 2 * kPi;
    double dx, dy, dz;
    rot.apply(cl * std::sin(lon), sl, cl * std::cos(lon), dx, dy, dz);
    const double u = std::atan2(dx, dz) / (2 * kPi) + 0.5, v = 0.5 - std::asin(clampd(dy, -1, 1)) / kPi;
    const double fx = u * pw - 0.5, fy = clampd(v * ph - 0.5, -1.0, (double)ph);
    PanoTap t;
    t.x0 = (int)std::floor(fx);
    t.tx = (float)(fx - t.x0);
    t.y0 = (int)std::floor(fy);
    t.ty = (float)(fy - t.y0);
    return t;
}

void rotatePanorama(const Buf& src, Buf& out, const Rot& rot, int px, int py, int pw, int ph)
{
    // Measured on VEGAS: output pixel (x, y) is at longitude x / (W - 1) and latitude y / (H - 1) (both edges included, so a pole is the first/last
    // row) and a direction is read at u * W - 0.5, v * H - 0.5, so even an unturned picture is resampled by 1/(W - 1). Beyond the poles the picture
    // continues as the pole row's average colour (VEGAS's poles are noisy and cannot be matched).
    float poleTop[4] = {0, 0, 0, 0}, poleBot[4] = {0, 0, 0, 0};
    for (int k = 0; k < pw; ++k)
        for (int c4 = 0; c4 < 4; ++c4) {
            poleTop[c4] += src.at(px + k, py)[c4] / (float)pw;
            poleBot[c4] += src.at(px + k, py + ph - 1)[c4] / (float)pw;
        }
    const double wm = std::max(1, pw - 1), hm = std::max(1, ph - 1);
    forRows(ph, pw, [&](int y) {
        const double lat = (0.5 - (double)y / hm) * kPi;
        const double cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < pw; ++x) {
            const PanoTap t = panoTap(rot, wm, pw, ph, x, cl, sl);
            const int x0 = t.x0, x1 = x0 + 1, y0 = t.y0, y1 = y0 + 1;
            const float tx = t.tx, ty = t.ty;
            auto row = [&](int xx, int yy) -> const float* {
                if (yy < 0) return poleTop;
                if (yy > ph - 1) return poleBot;
                xx = ((xx % pw) + pw) % pw;
                return src.at(px + xx, py + yy);
            };
            const float *p00 = row(x0, y0), *p10 = row(x1, y0), *p01 = row(x0, y1), *p11 = row(x1, y1);
            float* o = out.at(px + x, py + y);
            for (int k = 0; k < 4; ++k) {
                const float top = p00[k] + (p10[k] - p00[k]) * tx, bot = p01[k] + (p11[k] - p01[k]) * tx;
                o[k] = top + (bot - top) * ty;
            }
        }
    });
}

// The taps of every output pixel of a pw x ph picture (x0, y0, tx, ty as floats), for the GPU plan.
void panoramaMap(const Rot& rot, int pw, int ph, std::vector<float>& tab)
{
    const double wm = std::max(1, pw - 1), hm = std::max(1, ph - 1);
    tab.assign((size_t)pw * ph * 4, 0.f);
    forRows(ph, pw, [&](int y) {
        const double lat = (0.5 - (double)y / hm) * kPi;
        const double cl = std::cos(lat), sl = std::sin(lat);
        for (int x = 0; x < pw; ++x) {
            const PanoTap t = panoTap(rot, wm, pw, ph, x, cl, sl);
            float* o = &tab[((size_t)y * pw + (size_t)x) * 4];
            o[0] = (float)t.x0; o[1] = (float)t.y0; o[2] = t.tx; o[3] = t.ty;
        }
    });
}

// The pictures of a stereo frame: none (one), top and bottom, side by side.
void stereoParts(int mode, int W, int H, int& n, int px[2], int py[2], int& pw, int& ph)
{
    n = mode == 0 ? 1 : 2;
    pw = mode == 2 ? W / 2 : W;
    ph = mode == 1 ? H / 2 : H;
    for (int k = 0; k < 2; ++k) { px[k] = mode == 2 ? k * pw : 0; py[k] = mode == 1 ? k * ph : 0; }
}

// ------------------------------------------------------------------ Scene Rotation
// The 360 picture turned: x, y and z angles in degrees (VEGAS's help: x clockwise, y to the right, z up); which way round each goes is a guess.
const char* const kRotX = "mx.ofx.plugin.rotate.x.angle";
const char* const kRotY = "mx.ofx.plugin.rotate.y.angle";
const char* const kRotZ = "mx.ofx.plugin.rotate.z.angle";
const char* const kRotStereo = "mx.ofx.plugin.rotate.stereo";

// VEGAS draws this effect with the 360 plug-in's OpenCL kernel "rotateEquirect" (ofxRotation.ofx.bundle, ofx.ocl.executable carries its source); this is
// that kernel in float, line by line, with what the GPU adds to it:
//  - a pixel (x, y) of a W x H picture is at longitude (2 x / (W - 1) - 1) pi and latitude (1 - 2 y / (H - 1)) pi / 2 (both edges included);
//  - the direction is turned by the matrix Rx(x) Ry(-y) Rz(z) (float, angles as float radians) unless all angles are 0, and read back at
//    u = lon / pi, v = 2 lat / pi: image position (u + 1) / 2 (W - 1), (H - 1) - (v + 1) / 2 (H - 1), then divided by the image size - 1 and handed to
//    the sampler as a normalized coordinate, so the texel is at u W - 0.5: even an unturned picture is resampled by 1 / (W - 1);
//  - the sampler is linear and repeats (mirrors for stereo frames); the GPU's weights are whole 1/256s, its result is cut to 12 bits and read as
//    (r12 * 255 + 2047) / 4096 levels (measured on VEGAS's pictures: 99.8 to 100 % of the pixels equal, the rest one level off);
//  - the output is opaque.
// The GPU's native sin / cos are not exact where the kernel looks at them hardest (the poles, where cos(pi / 2) comes out as +0, and the picture's
// seam, where sin(pi) comes out as +3.8e-7 and sin(-pi) as -0); those values were found from VEGAS's pole rows and seam columns and are used as they are.
// Where the kernel reads for one output pixel: the texel (x0, y0) of the sampler (rows from the bottom) and the weights of the next ones
struct SceneTap { int x0, y0; float fx, fy; };
struct SceneGeom {
    int Wi = 0, Hi = 0, w = 0, h = 0, stereo = 0;
    bool rotate = false, levels = false, ok = false;
    float m[9];
    float hh = 0;
    std::vector<float> cosL, sinL, cosP, sinP;
    static constexpr float kPiF = 3.14159274101257324f, kHalfPiF = 1.57079637050628662f;
    void init(int Wi_, int Hi_, const float m_[9], bool rotate_, int stereo_, bool levels_)
    {
        Wi = Wi_; Hi = Hi_; stereo = stereo_; rotate = rotate_; levels = levels_;
        for (int i = 0; i < 9; ++i) m[i] = m_[i];
        w = Wi; h = Hi;
        if (stereo == 1) h = Hi / 2;
        else if (stereo == 2) w = Wi / 2;
        ok = !(w < 2 || h < 2 || Wi < 2 || Hi < 2);
        if (!ok) return;
        // the angles of a picture's columns and rows (the same for every picture of a stereo frame)
        cosL.resize((size_t)w); sinL.resize((size_t)w); cosP.resize((size_t)h); sinP.resize((size_t)h);
        for (int ix = 0; ix < w; ++ix) {
            const float nx = (float)ix / (float)(w - 1) * 2.0f - 1.0f;
            const float lon = nx * kPiF;
            cosL[(size_t)ix] = (float)std::cos((double)lon);
            float sl = (float)std::sin((double)lon);
            if (lon >= kPiF) sl = 3.8e-7f;
            else if (lon <= -kPiF) sl = -0.0f;
            sinL[(size_t)ix] = sl;
        }
        hh = (float)(h - 1);
        for (int iy = 0; iy < h; ++iy) {
            const float ny = (hh - (float)iy) / hh * 2.0f - 1.0f;
            const float lat = ny * kHalfPiF;
            float cp = (float)std::cos((double)lat);
            if (std::fabs(lat) >= kHalfPiF) cp = 0.0f;
            cosP[(size_t)iy] = cp;
            sinP[(size_t)iy] = (float)std::sin((double)lat);
        }
    }
    // false for a pixel the kernel does not write (the odd line or column of a stereo frame)
    bool tap(int gx, int gy, SceneTap& t) const
    {
        const int iy = stereo == 1 && gy >= h ? gy - h : gy;
        if (iy >= h) return false;
        const int ix = stereo == 2 && gx >= w ? gx - w : gx;
        if (ix >= w) return false;
        const float cp = cosP[(size_t)iy], sp = sinP[(size_t)iy];
        float X = cp * cosL[(size_t)ix], Y = sp, Z = cp * sinL[(size_t)ix];
        if (rotate) {
            const float xn = m[0] * X + m[1] * Y + m[2] * Z;
            const float yn = m[3] * X + m[4] * Y + m[5] * Z;
            const float zn = m[6] * X + m[7] * Y + m[8] * Z;
            X = xn; Y = yn; Z = zn;
        }
        const float hyp = std::sqrt(X * X + Z * Z);
        const float lo = (float)std::atan2((double)Z, (double)X), la = (float)std::atan2((double)Y, (double)hyp);
        float nx = lo / kPiF, ny = 2.0f * la / kPiF;
        nx = std::min(1.0f, std::max(-1.0f, nx));
        ny = std::min(1.0f, std::max(-1.0f, ny));
        float px = (nx + 1.0f) / 2.0f * (float)(w - 1);
        float py = hh - (ny + 1.0f) / 2.0f * hh;
        if (stereo == 1 && gy >= h) py += (float)h;
        if (stereo == 2 && gx >= w) px += (float)w;
        const float pym = (float)(Hi - 1) - py;                              // the kernel's rows run from the bottom
        const float u = px / (float)(Wi - 1), v = pym / (float)(Hi - 1);
        const float sx = u * (float)Wi - 0.5f, sy = v * (float)Hi - 0.5f;
        const float fxs = std::floor(sx), fys = std::floor(sy);
        t.x0 = (int)fxs;
        t.y0 = (int)fys;
        t.fx = sx - fxs;
        t.fy = sy - fys;
        if (levels) {
            t.fx = std::floor(t.fx * 256.0f + 0.5f) / 256.0f;
            t.fy = std::floor(t.fy * 256.0f + 0.5f) / 256.0f;
        }
        return true;
    }
};

void sceneRotateKernel(const Buf& src, Buf& out, const float m[9], bool rotate, int stereo, bool levels)
{
    const int Wi = src.w, Hi = src.h;
    SceneGeom G;
    G.init(Wi, Hi, m, rotate, stereo, levels);
    if (!G.ok) { out.px = src.px; return; }
    const bool mirror = stereo != 0;
    auto wrap = [mirror](int i, int n) {
        if (!mirror) { i %= n; return i < 0 ? i + n : i; }
        i %= 2 * n;
        if (i < 0) i += 2 * n;
        return i < n ? i : 2 * n - 1 - i;
    };
    forRows(Hi, Wi, [&](int gy) {
        for (int gx = 0; gx < Wi; ++gx) {
            SceneTap t;
            if (!G.tap(gx, gy, t)) continue;
            const int x0 = t.x0, y0 = t.y0;
            const float fx = t.fx, fy = t.fy;
            // texel rows run from the bottom: row m of the sampler is row Hi - 1 - m of the (top first) buffer
            const float* t00 = src.at(wrap(x0, Wi), Hi - 1 - wrap(y0, Hi));
            const float* t10 = src.at(wrap(x0 + 1, Wi), Hi - 1 - wrap(y0, Hi));
            const float* t01 = src.at(wrap(x0, Wi), Hi - 1 - wrap(y0 + 1, Hi));
            const float* t11 = src.at(wrap(x0 + 1, Wi), Hi - 1 - wrap(y0 + 1, Hi));
            float* o = out.at(gx, gy);
            for (int k = 0; k < 3; ++k) {
                const float top = t00[k] * (1.0f - fx) + t10[k] * fx, bot = t01[k] * (1.0f - fx) + t11[k] * fx;
                float r = top * (1.0f - fy) + bot * fy;
                if (levels) {
                    const long long r12 = (long long)std::floor(std::min(std::max(r, 0.0f), 1.0f) * 4096.0f);
                    r = (float)((r12 * 255 + 2047) / 4096) / 255.0f;
                }
                o[k] = r;
            }
            o[3] = 1.0f;
        }
    });
}

// the matrix as the plug-in builds it: Rx(x) Ry(-y) Rz(z), float angles (degrees * 0.0174532924f), products summed in order
void sceneRotationMatrix(Ctx& c, float M[9], bool& rotate)
{
    const float k = 0.01745329238474369f;
    const float ax = (float)c.d(kRotX) * k, ay = -(float)c.d(kRotY) * k, az = (float)c.d(kRotZ) * k;
    rotate = c.d(kRotX) != 0.0 || c.d(kRotY) != 0.0 || c.d(kRotZ) != 0.0;
    auto cs = [](float a) { return (float)std::cos((double)a); };
    auto sn = [](float a) { return (float)std::sin((double)a); };
    const float A[9] = {1, 0, 0, 0, cs(ax), -sn(ax), 0, sn(ax), cs(ax)};
    const float B[9] = {cs(ay), 0, sn(ay), 0, 1, 0, -sn(ay), 0, cs(ay)};
    const float C[9] = {cs(az), sn(az), 0, -sn(az), cs(az), 0, 0, 0, 1};
    auto mul3 = [](const float* a, const float* b, float* r) {
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) {
                float sum = 0.0f;
                for (int q = 0; q < 3; ++q) { const float pr = a[i * 3 + q] * b[q * 3 + j]; sum = sum + pr; }
                r[i * 3 + j] = sum;
            }
    };
    float AB[9];
    mul3(A, B, AB);
    mul3(AB, C, M);
}

void sceneRotation(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W < 2 || H < 2) return;
    float M[9];
    bool rotate;
    sceneRotationMatrix(c, M, rotate);
    sceneRotateKernel(c.src, c.out, M, rotate, std::max(0, std::min(2, c.i(kRotStereo))), c.levels);
}

// ------------------------------------------------------------------ 360 Stabilization
// VEGAS analyses the whole clip ("Analyze Motion Data"). Reno does it on small pictures: for up to 120 frames spread over the clip, the shift of the content between frames is found by comparing the luma of
// 128 x 64 copies (256 x 128 with Refinement, subpixel) along the picture (turn about the vertical axis) and up and down (sideways axis), leaving out the poles as asked. The running total is the camera
// path, kept in the hidden "Motion data" setting so a project holds it; drawing averages it over "Smoothing" frames and turns the picture by the difference. Roll is not looked for.
const char* const kStStereo = "mx.ofx.stab.plugin.stereo";
const char* const kStAnalyze = "mx.ofx.stab.plugin.btn.analyze";
const char* const kStClear = "mx.ofx.stab.plugin.btn.clear";
const char* const kStExclude = "mx.ofx.stab.plugin.exclude.bottom";
const char* const kStRefine = "mx.ofx.stab.plugin.ssd.deviation";
const char* const kStSmooth = "mx.ofx.stab.plugin.smoothness";
const char* const kStData = "mx.ofx.stab.plugin.motion";

// The path as text: "first step count" then yaw and pitch (radians, the content's turn since the first sample) per sample.
struct MotionPath {
    double first = 0, step = 1;
    std::vector<double> yaw, pitch;
    bool ok() const { return yaw.size() >= 2 && yaw.size() == pitch.size() && step > 0; }
    void at(double t, double& y, double& p) const
    {
        const double f = clampd((t - first) / step, 0, (double)(yaw.size() - 1));
        const size_t i0 = std::min(yaw.size() - 2, (size_t)f);
        const double u = f - (double)i0;
        y = yaw[i0] + (yaw[i0 + 1] - yaw[i0]) * u;
        p = pitch[i0] + (pitch[i0 + 1] - pitch[i0]) * u;
    }
};

std::string motionText(const MotionPath& m)
{
    std::string t = strfmt("%.3f %.6f %d", m.first, m.step, (int)m.yaw.size());
    for (size_t i = 0; i < m.yaw.size(); ++i) t += strfmt(" %.6f %.6f", m.yaw[i], m.pitch[i]);
    return t;
}

bool readMotion(const std::string& text, MotionPath& m)
{
    m = MotionPath();
    const char* s = text.c_str();
    char* e = nullptr;
    m.first = std::strtod(s, &e); if (e == s) return false; s = e;
    m.step = std::strtod(s, &e); if (e == s) return false; s = e;
    const long n = std::strtol(s, &e, 10); if (e == s || n < 2 || n > 100000) return false; s = e;
    for (long i = 0; i < n; ++i) {
        const double y = std::strtod(s, &e); if (e == s) return false; s = e;
        const double p = std::strtod(s, &e); if (e == s) return false; s = e;
        if (!std::isfinite(y) || !std::isfinite(p)) return false;
        m.yaw.push_back(y);
        m.pitch.push_back(p);
    }
    return m.ok();
}

// The luma of a frame's picture (the part [px, px + pw) x [py, py + ph)) as w x h values, by averaging boxes.
std::vector<float> smallLuma(const Buf& b, int px, int py, int pw, int ph, int w, int h)
{
    std::vector<float> out((size_t)w * h, 0.f);
    for (int y = 0; y < h; ++y) {
        const int y0 = py + y * ph / h, y1 = std::max(y0 + 1, py + (y + 1) * ph / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = px + x * pw / w, x1 = std::max(x0 + 1, px + (x + 1) * pw / w);
            double acc = 0;
            int n = 0;
            for (int yy = y0; yy < y1 && yy < b.h; ++yy)
                for (int xx = x0; xx < x1 && xx < b.w; ++xx) {
                    const float* p = b.at(xx, yy);
                    acc += luma(p[0], p[1], p[2]);        // (premultiplied: a transparent pixel is dark)
                    ++n;
                }
            out[(size_t)y * w + x] = n ? (float)(acc / n) : 0.f;
        }
    }
    return out;
}

// How far the content of 'cur' lies from 'prev': cur(x, y) is prev(x - mx, y - my). Columns wrap, rows do not; pixels are weighted by the cosine
// of latitude, rows in [r0, r1) only; shifts searched |mx| <= sx, |my| <= sy.
void findShift(const std::vector<float>& prev, const std::vector<float>& cur, int w, int h, int r0, int r1, int sx, int sy, bool subpixel, double& mx, double& my)
{
    std::vector<double> wt((size_t)h);
    for (int y = 0; y < h; ++y) wt[(size_t)y] = std::cos((0.5 - (y + 0.5) / h) * kPi);
    const int nx = 2 * sx + 1, ny = 2 * sy + 1;
    std::vector<double> cost((size_t)nx * ny, 1e30);
    for (int dy = -sy; dy <= sy; ++dy)
        for (int dx = -sx; dx <= sx; ++dx) {
            double acc = 0, wsum = 0;
            for (int y = std::max(r0, std::max(0, dy)); y < std::min(r1, std::min(h, h + dy)); ++y) {
                const float* c = &cur[(size_t)y * w];
                const float* p = &prev[(size_t)(y - dy) * w];
                double row = 0;
                for (int x = 0; x < w; ++x) {
                    const int xs = ((x - dx) % w + w) % w;
                    row += std::fabs(c[x] - p[xs]);
                }
                acc += row * wt[(size_t)y];
                wsum += wt[(size_t)y] * w;
            }
            if (wsum > 0) cost[(size_t)(dy + sy) * nx + (dx + sx)] = acc / wsum;
        }
    int bi = 0;
    for (size_t i = 1; i < cost.size(); ++i) if (cost[i] < cost[(size_t)bi]) bi = (int)i;
    int bx = bi % nx, by = bi / nx;
    mx = bx - sx;
    my = by - sy;
    if (subpixel) {
        auto parab = [](double a, double b, double c2) { const double d = a - 2 * b + c2; return std::fabs(d) < 1e-12 ? 0.0 : clampd(0.5 * (a - c2) / d, -0.5, 0.5); };
        if (bx > 0 && bx < nx - 1) mx += parab(cost[(size_t)by * nx + bx - 1], cost[(size_t)by * nx + bx], cost[(size_t)by * nx + bx + 1]);
        if (by > 0 && by < ny - 1) my += parab(cost[(size_t)(by - 1) * nx + bx], cost[(size_t)by * nx + bx], cost[(size_t)(by + 1) * nx + bx]);
    }
}

// The path of a clip from the small lumas of its sample frames (w x h each, in order).
MotionPath analyseMotion(const std::vector<std::vector<float>>& lumas, int w, int h, double first, double step, int exclude, bool refine)
{
    MotionPath m;
    m.first = first;
    m.step = step;
    if (lumas.size() < 2) return m;
    const int cut = h * 3 / 20;
    const int r0 = exclude == 0 ? cut : 0, r1 = exclude <= 1 ? h - cut : h;
    double ay = 0, ap = 0;
    m.yaw.push_back(0);
    m.pitch.push_back(0);
    for (size_t k = 1; k < lumas.size(); ++k) {
        double mx, my;
        findShift(lumas[k - 1], lumas[k], w, h, r0, r1, w / 8, h / 8, refine, mx, my);
        ay += mx * 2 * kPi / w;
        ap += -my * kPi / h;
        m.yaw.push_back(ay);
        m.pitch.push_back(ap);
    }
    return m;
}

void stabilizationChanged(Change& ch)
{
    if (ch.param == kStClear) { ch.setS(kStData, ""); return; }
    if (ch.param != kStAnalyze) return;
    double first, last;
    ch.clipRange(first, last);
    if (!(last > first + 1)) return;
    const double span = last - first;
    const double step = std::max(1.0, std::ceil(span / 120.0));
    const bool refine = ch.b(kStRefine);
    const int w = refine ? 256 : 128, h = refine ? 128 : 64;
    std::vector<std::vector<float>> lumas;
    for (double t = first; t < last - 0.5; t += step) {
        Buf f;
        if (!ch.source(t, f) || f.w < 8 || f.h < 8) return;
        int n, px[2], py[2], pw, ph;
        stereoParts(ch.i(kStStereo), f.w, f.h, n, px, py, pw, ph);
        lumas.push_back(smallLuma(f, px[0], py[0], pw, ph, w, h));
    }
    const MotionPath m = analyseMotion(lumas, w, h, first, step, ch.i(kStExclude), refine);
    if (m.ok()) ch.setS(kStData, motionText(m));
}

// What a 360 Stabilization render does: 0 nothing to do (the frame is copied, or left alone under 2 pixels: 2), 1 the picture is turned by rot.
int stabilizationSetup(Ctx& c, int W, int H, Rot& rot, int& n, int px[2], int py[2], int& pw, int& ph)
{
    MotionPath m;
    if (W < 2 || H < 2 || !readMotion(c.s(kStData), m)) return 0;
    double y0, p0;
    m.at(c.time, y0, p0);
    const double win = std::max(0, c.i(kStSmooth));
    double ys = y0, ps = p0;
    if (win > 0.5) {
        const int taps = 33;
        ys = ps = 0;
        for (int k = 0; k < taps; ++k) {
            double y1, p1;
            m.at(c.time + (k / (taps - 1.0) - 0.5) * win, y1, p1);
            ys += y1; ps += p1;
        }
        ys /= taps; ps /= taps;
    }
    const double corrYaw = ys - y0, corrPitch = ps - p0;       // the content must move by this
    if (corrYaw == 0 && corrPitch == 0) return 0;
    rot = mul(rotYaw(-corrYaw), rotPitch(-corrPitch));
    stereoParts(c.i(kStStereo), W, H, n, px, py, pw, ph);
    return 1;
}

void stabilization(Ctx& c)
{
    Rot rot;
    int n, px[2], py[2], pw, ph;
    if (!stabilizationSetup(c, c.out.w, c.out.h, rot, n, px, py, pw, ph)) { copyFrame(c); return; }
    for (int k = 0; k < n; ++k) rotatePanorama(c.src, c.out, rot, px[k], py[k], pw, ph);
}

// ------------------------------------------------------------------ Displacement Map and Height Map (General context)
// VEGAS's are General-context effects taking the picture and a second input (the map). Reno's host gives an effect one picture, so the map is the
// picture itself: Displacement Map moves each pixel sideways and up/down by the chosen channels (0.5 is no move), Height Map treats the channel as a
// height and lifts each pixel by it. Both are Reno's own reading of VEGAS's settings.
float channelOf(const float* p, int which)
{
    const float a = p[3] > 1e-5f ? 1.f / p[3] : 0.f;
    return clamp01(which == 0 ? p[0] * a : which == 1 ? p[1] * a : p[2] * a);
}

void displacementMap(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double sh = c.d("ScaleHorizontal") * W, sv = c.d("ScaleVertical") * H;
    const bool wrap = c.i("EdgePixelHandling") == 1;
    const int cx = std::min(2, std::max(0, c.i("ChannelsXaxisChannel"))), cy = std::min(2, std::max(0, c.i("ChannelsYaxisChannel")));
    const bool ix = c.b("ChannelsXaxisInvert"), iy = c.b("ChannelsYaxisInvert"), alphaScales = c.b("AlphaChannelScalesRGBChannels");
    const bool flip = c.b("FlipInputSurfaces");
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const float* m = c.src.at(x, flip ? H - 1 - y : y);
            float vx = channelOf(m, cx), vy = channelOf(m, cy);
            if (ix) vx = 1.f - vx;
            if (iy) vy = 1.f - vy;
            float k = 1.f;
            if (alphaScales) k = m[3];
            const double sx = x + 0.5 + (vx - 0.5) * 2 * sh * k, sy = y + 0.5 + (vy - 0.5) * 2 * sv * k;
            float* o = c.out.at(x, y);
            if (wrap) {
                const double wx = sx - std::floor(sx / W) * W, wy = sy - std::floor(sy / H) * H;
                c.src.sample(wx, wy, o);
            } else {
                c.src.sample(sx, sy, o);
            }
        }
    });
}

void heightMap(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double amp = c.d("HeightModifiersAmplitude"), elev = c.d("HeightModifiersElevation"), scl = c.d("HeightModifiersHeightScale");
    const bool wrap = c.i("EdgePixelHandling") == 1;
    const int ch = c.i("SourceChannel");
    const bool inv = c.b("InvertDataSource"), alphaScales = c.b("AlphaChannelScalesRGBChannels"), flip = c.b("FlipInputSurfaces");
    std::vector<float> hp((size_t)W * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            const float* p = c.src.at(x, flip ? H - 1 - y : y);
            float v = ch == 0 ? luma(channelOf(p, 0), channelOf(p, 1), channelOf(p, 2)) : channelOf(p, ch - 1);
            if (inv) v = 1.f - v;
            if (alphaScales) v *= p[3];
            hp[(size_t)y * W + x] = v;
        }
    const double sigma = std::max(0, c.i("Smoothness")) * 0.001 * W;
    if (sigma > 0.3) gaussianBlurPlane(hp, W, H, sigma, sigma);
    const double lift = amp * scl * 0.2 * H, base = elev * scl * 0.2 * H;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            const double dy = base + lift * hp[(size_t)y * W + x];
            double sy = y + 0.5 + dy;
            float* o = c.out.at(x, y);
            if (wrap) sy -= std::floor(sy / H) * H;
            c.src.sample(x + 0.5, sy, o);
        }
    });
}

// ------------------------------------------------------------------ Histogram
// The picture with its histogram drawn over it: Type is the channel (luma, red, green, blue, alpha, or Lab's L, a, b), Center where the graph is,
// Transparency its background's see-through amount, Logarithmic scale the bar heights, HDR how far over 1.0 the horizontal axis goes (1: usual range).
void rgbToLabApprox(float r, float g, float b, float& L, float& A, float& B)
{
    auto lin = [](float v) { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
    const float lr = lin(clamp01(r)), lg = lin(clamp01(g)), lb = lin(clamp01(b));
    const float X = (0.4124f * lr + 0.3576f * lg + 0.1805f * lb) / 0.95047f, Y = 0.2126f * lr + 0.7152f * lg + 0.0722f * lb, Z = (0.0193f * lr + 0.1192f * lg + 0.9505f * lb) / 1.08883f;
    auto f = [](float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; };
    const float fx = f(X), fy = f(Y), fz = f(Z);
    L = 116.f * fy - 16.f; A = 500.f * (fx - fy); B = 200.f * (fy - fz);
}

void histogramFx(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    copyFrame(c);
    if (W < 8 || H < 8) return;
    const int type = c.i("Type");
    const double hdr = std::max(1.0, c.d("HDR"));
    const int N = 128;
    std::vector<double> bins((size_t)N, 0.0);
    for (size_t i = 0; i + 3 < c.src.px.size(); i += 4) {
        const float a = c.src.px[i + 3];
        float v;
        if (type == 4) v = a;
        else {
            if (!(a > 1e-5f)) continue;
            const float r = c.src.px[i] / a, g = c.src.px[i + 1] / a, b = c.src.px[i + 2] / a;
            if (type == 0) v = luma(r, g, b);
            else if (type == 1) v = r;
            else if (type == 2) v = g;
            else if (type == 3) v = b;
            else {
                float L, A, B;
                rgbToLabApprox(r, g, b, L, A, B);
                v = type == 5 ? L / 100.f : type == 6 ? A / 256.f + 0.5f : B / 256.f + 0.5f;
            }
        }
        const double t = clampd(v / hdr, 0, 0.99999);
        bins[(size_t)(t * N)] += 1;
    }
    double mx = 0;
    for (double b : bins) mx = std::max(mx, b);
    if (mx <= 0) return;
    const bool logScale = c.b("LogarithmicScale");
    double cxn, cyn;
    c.xy("Center", cxn, cyn);
    const int bw = std::max(16, W / 3), bh = std::max(12, H / 4);
    const int x0 = (int)std::lround(cxn * W - bw * 0.5), y0 = (int)std::lround((1.0 - cyn) * H - bh * 0.5);
    const float back = 1.f - (float)clampd(c.d("Transparency"), 0, 1);
    for (int y = std::max(0, y0); y < std::min(H, y0 + bh); ++y)
        for (int x = std::max(0, x0); x < std::min(W, x0 + bw); ++x) {
            const int b0 = std::min(N - 1, (x - x0) * N / bw), b1 = std::max(b0, std::min(N - 1, (x - x0 + 1) * N / bw - 1));
            double bv = 0;
            for (int bi = b0; bi <= b1; ++bi) bv = std::max(bv, bins[(size_t)bi]);
            double hgt = bv / mx;
            if (logScale) hgt = std::log1p(bv) / std::log1p(mx);
            const double fromBottom = 1.0 - (double)(y - y0 + 0.5) / bh;
            float* o = c.out.at(x, y);
            const float k = back * 0.6f;
            o[0] *= 1.f - k; o[1] *= 1.f - k; o[2] *= 1.f - k; o[3] = o[3] + (1.f - o[3]) * k;
            if (fromBottom <= hgt) {
                const float cr = type == 1 ? 1.f : (type == 2 ? 0.3f : (type == 3 ? 0.3f : 1.f));
                const float cg = type == 2 ? 1.f : (type == 1 ? 0.3f : (type == 3 ? 0.3f : 1.f));
                const float cb = type == 3 ? 1.f : (type == 1 ? 0.3f : (type == 2 ? 0.3f : 1.f));
                o[0] = cr * 0.9f; o[1] = cg * 0.9f; o[2] = cb * 0.9f; o[3] = 1.f;
            }
        }
}

// ------------------------------------------------------------------ Gradient Wipe
// A wipe where the picture arrives where a gradient is below the progress. The gradient is a diagonal ramp from upper left to lower right (VEGAS
// lets the user pick a picture; Reno's host cannot ask for one). Flip turns it over, Mirror folds it about the middle, Gradient blur softens it,
// Threshold blend is the edge softness, Direction Reverse starts at the other end, Auto contrast stretch spreads it over 0..1.
void gradientWipe(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const bool hf = c.b("HorizontalFlip"), vf = c.b("VerticalFlip"), hm = c.b("HorizontalMirror"), vm = c.b("VerticalMirror");
    const bool reverse = c.i("Direction") == 1, stretch = c.b("AutoContrastStretch"), keepAspect = c.b("MaintainAspectRatio");
    const double blend = clampd(c.d("ThresholdBlend"), 0, 1) * 0.5, blur = clampd(c.d("GradientBlur"), 0, 1);
    const double p = clampd(c.progress, 0, 1);
    const double aspect = keepAspect ? (double)W / std::max(1, H) : 1.0;
    std::vector<float> g((size_t)W * H);
    for (int y = 0; y < H; ++y) {
        double v = (y + 0.5) / H;
        if (vf) v = 1.0 - v;
        if (vm) v = v < 0.5 ? v * 2 : (1.0 - v) * 2;
        for (int x = 0; x < W; ++x) {
            double u = (x + 0.5) / W;
            if (hf) u = 1.0 - u;
            if (hm) u = u < 0.5 ? u * 2 : (1.0 - u) * 2;
            // (with the aspect kept, the diagonal runs at 45 degrees of the picture, not corner to corner)
            double val = keepAspect ? (u * aspect + v) / (aspect + 1.0) : (u + v) * 0.5;
            g[(size_t)y * W + x] = (float)val;
        }
    }
    if (blur > 0.001) gaussianBlurPlane(g, W, H, blur * 0.1 * W, blur * 0.1 * W);
    if (stretch) {
        float lo = 1e9f, hi = -1e9f;
        for (float v : g) { lo = std::min(lo, v); hi = std::max(hi, v); }
        if (hi - lo > 1e-6f) for (float& v : g) v = (v - lo) / (hi - lo);
    }
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            double v = g[(size_t)y * W + x];
            if (reverse) v = 1.0 - v;
            const double t = blend > 1e-4 ? clampd((p * (1.0 + blend) - v) / blend, 0, 1) : (p > v ? 1.0 : 0.0);
            const float* a = c.src.at(x, y);
            const float* b = c.src2.at(x, y);
            float* o = c.out.at(x, y);
            for (int k = 0; k < 4; ++k) o[k] = a[k] + (b[k] - a[k]) * (float)t;
        }
    });
}

}  // namespace

// The GPU plan's view of 360 Stabilization (rotatePanorama): copy true when the frame goes through, false return when the CPU does it
bool panoramaSetupGpu(Ctx& c, int W, int H, PanoSetup& o)
{
    Rot rot;
    o.copy = false;
    const int r = stabilizationSetup(c, W, H, rot, o.n, o.px, o.py, o.pw, o.ph);
    if (r == 0) {
        if (W < 2 || H < 2) return false;
        o.copy = true;
        return true;
    }
    // (a stereo frame whose halves do not fill it leaves pixels the CPU does not write)
    if (o.n * o.pw != W && o.pw != W) return false;
    if (o.n * o.ph != H && o.ph != H) return false;
    // (the map is picture-independent: the last one is kept while the turn and the size stay)
    static std::mutex mu;
    static std::vector<double> lastKey;
    static std::vector<float> lastMap;
    std::vector<double> key(rot.m, rot.m + 9);
    key.push_back(o.pw);
    key.push_back(o.ph);
    std::lock_guard<std::mutex> lock(mu);
    if (key != lastKey || lastMap.empty()) {
        panoramaMap(rot, o.pw, o.ph, lastMap);
        lastKey = key;
    }
    o.map = lastMap;
    return true;
}

// The GPU plan's view of Scene Rotation (sceneRotateKernel()): the taps of every pixel (x0, y0, fx, fy) from the CPU's own code, kept while the settings stay
bool sceneRotationSetupGpu(Ctx& c, int W, int H, SceneSetup& o)
{
    if (W < 2 || H < 2) return false;
    float M[9];
    bool rotate;
    sceneRotationMatrix(c, M, rotate);
    o.stereo = std::max(0, std::min(2, c.i(kRotStereo)));
    o.levels = c.levels;
    SceneGeom G;
    G.init(W, H, M, rotate, o.stereo, o.levels);
    if (!G.ok) return false;
    if (o.stereo == 1 && G.h * 2 != H) return false;           // (a line or column the kernel leaves alone)
    if (o.stereo == 2 && G.w * 2 != W) return false;
    static std::mutex mu;
    static std::vector<float> lastKey;
    static std::vector<float> lastMap;
    std::vector<float> key(M, M + 9);
    key.push_back((float)W); key.push_back((float)H); key.push_back((float)o.stereo); key.push_back(rotate ? 1.f : 0.f); key.push_back(o.levels ? 1.f : 0.f);
    std::lock_guard<std::mutex> lock(mu);
    if (key != lastKey || lastMap.empty()) {
        lastMap.assign((size_t)W * H * 4, 0.f);
        forRows(H, W, [&](int y) {
            for (int x = 0; x < W; ++x) {
                SceneTap t;
                if (!G.tap(x, y, t)) continue;
                float* e = &lastMap[((size_t)y * W + (size_t)x) * 4];
                e[0] = (float)t.x0; e[1] = (float)t.y0; e[2] = t.fx; e[3] = t.fy;
            }
        });
        lastKey = key;
    }
    o.map = lastMap;
    return true;
}

void applyVegasGroups(std::vector<Def>& all)
{
    for (const GroupSpec& g : kGroups) {
        const std::string id = std::string(kVegas) + g.effect;
        for (Def& d : all) {
            if (d.id != id) continue;
            bool have = false;
            for (const PDef& p : d.params) if (p.name == g.name) have = true;
            if (have) break;
            // the members: settings not in a group yet
            const std::string prefix = g.prefix, names = g.names;
            size_t first = d.params.size();
            std::vector<size_t> members;
            for (size_t k = 0; k < d.params.size(); ++k) {
                const PDef& p = d.params[k];
                if (p.type == PT::Group || !p.parent.empty()) continue;
                const bool in = (!prefix.empty() && p.name.compare(0, prefix.size(), prefix) == 0) || listed(names, p.name);
                if (!in) continue;
                members.push_back(k);
                first = std::min(first, k);
            }
            if (members.empty()) break;
            for (size_t k : members) d.params[k].parent = g.name;
            d.params.insert(d.params.begin() + (std::ptrdiff_t)first, group(g.name, g.label));
            break;
        }
    }
}

void addFiltersVegasRest(std::vector<Def>& out)
{
    // 360 Stabilization (VEGAS\360): the analysis is a button, its results a hidden setting
    {
        std::vector<PDef> ps = {choice(kStStereo, "Stereoscopy", 0, {"None", "Top-Bottom", "Side-by-Side"}),
                                button(kStAnalyze, "Analyze Motion Data"), button(kStClear, "Clear Motion Data"),
                                choice(kStExclude, "Exclude Views", 0, {"North and South Pole", "South Pole", "None"}),
                                boolean(kStRefine, "Refinement", false), intp(kStSmooth, "Smoothing", 30, 0, 120),
                                custom(kStData, "Motion data", "")};
        for (auto& p : ps) {
            if (p.name == kStStereo) p.hint = "You can select a preset for a stereoscopic model.";
            if (p.name == kStAnalyze) p.hint = "The analysis is performed to determine the motion data.";
            if (p.name == kStClear) p.hint = "All results of the analysis are discarded.";
            if (p.name == kStExclude) p.hint = "Views that are not included in the analysis.";
            if (p.name == kStRefine) p.hint = "Click to automatically refine the results.";
            if (p.name == kStSmooth) p.hint = "The Smoothing parameter is the parameter that helps to define how much the video is stabilized.";
        }
        Def d = make("com.magix.ofx.vr.stabilization", "360 Stabilization", "VEGAS\\360", "Steadies a 360 degree picture from the camera's analysed motion", ps, stabilization);
        d.changed = stabilizationChanged;
        d.temporal = true;
        out.push_back(d);
    }
    {
        std::vector<PDef> ps = {dbl(kRotX, "Turn around x-axis", 0, -180, 180), dbl(kRotY, "Turn around y-axis", 0, -180, 180), dbl(kRotZ, "Turn around z-axis", 0, -180, 180),
                                choice(kRotStereo, "Stereoscopy", 0, {"None", "Top-and-Bottom", "Side-by-Side"})};
        ps[0].hint = "Turn the field of view around x-axis.";
        ps[1].hint = "Turn the field of view around y-axis.";
        ps[2].hint = "Turn the field of view around z-axis.";
        ps[3].hint = "Select the target 3D frame format.";
        out.push_back(make("com.magixsoftware:panorotation", "Scene Rotation", "VEGAS\\360", "A 360 degree picture turned about its axes", ps, sceneRotation));
    }
    {
        std::vector<PDef> ps = {group("Scale", "Scale"), dbl("ScaleHorizontal", "Horizontal", 0.1, -1, 1, "Scale"), dbl("ScaleVertical", "Vertical", 0.1, -1, 1, "Scale"),
                                choice("EdgePixelHandling", "Edge pixel handling", 0, {"Transparent background", "Wrap pixels around"}),
                                group("Channels", "Channels"),
                                choice("ChannelsXaxisChannel", "X axis channel", 0, {"Red", "Green", "Blue"}, "Channels"), boolean("ChannelsXaxisInvert", "X axis invert", false, "Channels"),
                                choice("ChannelsYaxisChannel", "Y axis channel", 1, {"Red", "Green", "Blue"}, "Channels"), boolean("ChannelsYaxisInvert", "Y axis invert", false, "Channels"),
                                boolean("AlphaChannelScalesRGBChannels", "Alpha channel scales RGB channels", false), boolean("FlipInputSurfaces", "Flip input surfaces", false)};
        Def d = make(std::string(kVegas) + "displacementmap", "Displacement Map", "VEGAS", "Pixels moved by the picture's own colour channels", ps, displacementMap);
        d.general = true;
        out.push_back(d);
    }
    {
        std::vector<PDef> ps = {group("HeightModifiers", "Height modifiers"), dbl("HeightModifiersAmplitude", "Amplitude", 0.1, 0, 1, "HeightModifiers"),
                                dbl("HeightModifiersElevation", "Elevation", 0.1, 0, 1, "HeightModifiers"), dbl("HeightModifiersHeightScale", "Height scale", 1, 0, 2, "HeightModifiers"),
                                choice("EdgePixelHandling", "Edge pixel handling", 0, {"Transparent background", "Wrap pixels around"}),
                                choice("SourceChannel", "Source channel", 0, {"Intensity", "Red", "Green", "Blue"}), intp("Smoothness", "Smoothness", 14, 0, 100),
                                boolean("InvertDataSource", "Invert data source", false), boolean("AlphaChannelScalesRGBChannels", "Alpha channel scales RGB channels", false),
                                boolean("FlipInputSurfaces", "Flip input surfaces", false)};
        Def d = make(std::string(kVegas) + "heightmap", "Height Map", "VEGAS", "Pixels lifted by the picture's brightness", ps, heightMap);
        d.general = true;
        out.push_back(d);
    }
    {
        std::vector<PDef> ps = {choice("Type", "Type", 0, {"Luminance", "Red", "Green", "Blue", "Alpha", "L", "A", "B"}), point("Center", "Center", 0.5, 0.5),
                                dbl("Transparency", "Transparency", 0.5, 0, 1), boolean("LogarithmicScale", "Logarithmic scale", false), dbl("HDR", "HDR", 1, 1, 16)};
        out.push_back(make(std::string(kVegas) + "histogram", "Histogram", "VEGAS\\Utility", "A graph of the picture's histogram drawn over it", ps, histogramFx));
    }
    {
        Def d = vfxDef(std::string(kVegas) + "gradientwipe", gradientWipe);
        if (!d.id.empty()) {
            d.transition = true;
            d.group = kGroup;
            d.description = "A wipe along a gradient (Reno's own, standing in for VEGAS Gradient Wipe).";
            vfxTune(d, "ThresholdBlend", 0.1, 0, 1);
            out.push_back(d);
        }
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
