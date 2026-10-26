// The small framework Reno's built-in OpenFX plug-ins are written with (see builtins.h). Not part of the host: they talk to it only through the OpenFX suites.
#pragma once
#include "builtins.h"
#include "ofxImageEffect.h"
#include "ofxParam.h"
#include "../core/pixrows.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <cstring>
#include <map>

namespace reno {
namespace gpu { struct Plan; }          // (vkplan.h)
namespace ofx {
namespace builtin {

enum class PT { Double, Int, Bool, Choice, RGBA, RGB, Double2D, String, Group, Custom, Double3D, Button, Image };

// One parameter as the effect defines it.
struct PDef {
    PT type = PT::Double;
    std::string name, label;
    std::vector<double> def;
    double min = -1e9, max = 1e9;
    double dmin = NAN, dmax = NAN;
    std::vector<std::string> choices;
    std::string parent;                 // the group it is in
    std::string hint;
    std::string sdef;                   // String: the default text
    bool multiline = false;
    bool normalised = false;
    bool secret = false;
};

// Shorthands for the definitions.
PDef dbl(const std::string& name, const std::string& label, double def, double min, double max, const std::string& parent = std::string());
PDef intp(const std::string& name, const std::string& label, int def, int min, int max, const std::string& parent = std::string());
PDef boolean(const std::string& name, const std::string& label, bool def, const std::string& parent = std::string());
PDef choice(const std::string& name, const std::string& label, int def, std::vector<std::string> options, const std::string& parent = std::string());
PDef rgba(const std::string& name, const std::string& label, double r, double g, double b, double a, const std::string& parent = std::string());
PDef rgb(const std::string& name, const std::string& label, double r, double g, double b, const std::string& parent = std::string());
PDef point(const std::string& name, const std::string& label, double x, double y, const std::string& parent = std::string());
PDef text(const std::string& name, const std::string& label, const std::string& def, bool multiline);
PDef group(const std::string& name, const std::string& label);
// A position or direction in three numbers (the same range for each), and a button (no value: pressing it calls Def::changed with its name).
PDef point3(const std::string& name, const std::string& label, double x, double y, double z, double min, double max, const std::string& parent = std::string());
PDef button(const std::string& name, const std::string& label, const std::string& parent = std::string());
// VEGAS's image setting (OfxParamTypeImage: Color Match's pictures, Style Transfer's reference): it has no value of its own to read; the
// host describes it only when its VEGAS extensions are on (otherwise the setting is left out).
PDef imageParam(const std::string& name, const std::string& label, const std::string& parent = std::string());
// A setting VEGAS keeps as a text it does not show (a Custom parameter: a curve's points, a mesh's points): read with Ctx::s, hidden
// in the effects window, kept in the project and in the .veg as it is.
PDef custom(const std::string& name, const std::string& label, const std::string& def);

// A frame the effects work on: straight floats would lose the edges of soft alpha, so the pixels are RGBA floats
// premultiplied by alpha, top row first (OpenFX's rows go bottom first: the framework turns them over).
struct Buf {
    int w = 0, h = 0;
    std::vector<float> px;              // w * h * 4
    void resize(int W, int H)
    {
        w = W;
        h = H;
        px.resize((size_t)W * H * 4);
        if (!px.empty()) std::memset(px.data(), 0, px.size() * sizeof(float));
    }
    void ensure(int W, int H) { w = W; h = H; px.resize((size_t)W * H * 4); }
    float* at(int x, int y) { return &px[((size_t)y * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[((size_t)y * w + x) * 4]; }
    inline void sample(double x, double y, float out[4]) const;
    // Clamped to the edges instead.
    inline void sampleClamped(double x, double y, float out[4]) const;
    void sampleEdge(double x, double y, float out[4]) const;
};

inline void Buf::sample(double x, double y, float out[4]) const
{
    const double xs = x - 0.5, ys = y - 0.5;
    // (all four pixels inside: x0 in 0 .. w - 2; written so that a NaN fails the test)
    if (xs >= 0.0 && ys >= 0.0 && xs < (double)(w - 1) && ys < (double)(h - 1)) {
        const int x0 = (int)xs, y0 = (int)ys;           // (a floor, as the values are not negative)
        const float fx = (float)(xs - x0), fy = (float)(ys - y0);
        const float ix = 1 - fx, iy = 1 - fy;
        const float w00 = ix * iy, w10 = fx * iy, w01 = ix * fy, w11 = fx * fy;
        const float* p00 = &px[((size_t)y0 * w + x0) * 4];
        const float* p01 = p00 + (size_t)w * 4;
#ifdef RENO_PIXROWS_SSE2
        __m128 acc = _mm_add_ps(_mm_setzero_ps(), _mm_mul_ps(_mm_loadu_ps(p00), _mm_set1_ps(w00)));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(p00 + 4), _mm_set1_ps(w10)));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(p01), _mm_set1_ps(w01)));
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(p01 + 4), _mm_set1_ps(w11)));
        _mm_storeu_ps(out, acc);
#else
        for (int k = 0; k < 4; ++k) {
            float v = 0.f;
            v += p00[k] * w00;
            v += p00[4 + k] * w10;
            v += p01[k] * w01;
            v += p01[4 + k] * w11;
            out[k] = v;
        }
#endif
        return;
    }
    sampleEdge(x, y, out);
}

inline void Buf::sampleClamped(double x, double y, float out[4]) const
{
    if (w <= 0 || h <= 0 || !std::isfinite(x) || !std::isfinite(y)) { out[0] = out[1] = out[2] = out[3] = 0.f; return; }
    const double cx = x < 0.5 ? 0.5 : (x > w - 0.5 ? w - 0.5 : x), cy = y < 0.5 ? 0.5 : (y > h - 0.5 ? h - 0.5 : y);
    sample(cx, cy, out);
}

struct Color { double r = 0, g = 0, b = 0, a = 1; };

// What a render is given: the time, the parameters' values then, the source (filters) and the frame to fill.
struct Ctx {
    OfxParamSetHandle params = nullptr;
    double time = 0;                    // frames
    double fps = 0;
    int frameW = 0, frameH = 0;
    // The render scale: pixels of this frame per pixel of the project's frame (1: project size; 0.5: half-size preview). Sizes an effect measures in
    // pixels (radius, distance, line width, text size) are multiplied by it with px(); fractions of the frame need nothing.
    double scale = 1.0;
    // The picture is 8-bit: effects that VEGAS cuts to whole levels (truncating, as VEGAS's own do) do so; a float picture is left as computed.
    bool levels = true;
    double px(double v) const { return v * scale; }
    // For effects that take pixel INDICES (not centres) as positions: the index in this frame of the point that is full-size pixel index x (0 at scale 1);
    // add it to the index of a pixel here.
    double shift() const { return 0.5 * (1.0 - scale); }
    bool interactive = true;
    Buf src;
    Buf src2;
    double progress = 0;
    Buf out;
    const std::map<std::string, std::vector<double>>* testParams = nullptr;
    const std::map<std::string, std::string>* testStrings = nullptr;
    double d(const char* name) const;
    int i(const char* name) const;
    bool b(const char* name) const { return i(name) != 0; }
    Color color(const char* name) const;        // RGBA or RGB (alpha 1)
    void xy(const char* name, double& x, double& y) const;
    void xyz(const char* name, double& x, double& y, double& z) const;
    std::string s(const char* name) const;
    const void* instance = nullptr;
    std::function<bool(double time, Buf& into)> fetch;
};

// What a Def::changed function is given when a setting changes (a button pressed or a value edited): the setting's name, the time, and ways to read and
// write the effect's settings and fetch the source clip's frame at any time. Only effects that analyse the picture or keep two settings in step have one.
struct Change {
    OfxImageEffectHandle effect = nullptr;
    std::string param;                  // the setting that changed
    double time = 0;                    // frames
    Ctx ctx;
    double d(const char* name) const { return ctx.d(name); }
    int i(const char* name) const { return ctx.i(name); }
    bool b(const char* name) const { return ctx.i(name) != 0; }
    Color color(const char* name) const { return ctx.color(name); }
    void xy(const char* name, double& x, double& y) const { ctx.xy(name, x, y); }
    void xyz(const char* name, double& x, double& y, double& z) const { ctx.xyz(name, x, y, z); }
    std::string s(const char* name) const { return ctx.s(name); }
    void setD(const char* name, double v) const;
    void setI(const char* name, int v) const;
    void setColor(const char* name, const Color& c) const;      // RGBA or RGB
    void setXY(const char* name, double x, double y) const;
    void setXYZ(const char* name, double x, double y, double z) const;
    void setS(const char* name, const std::string& v) const;
    // The source clip's frame at 'frame' (as an effect's render gets it, premultiplied, top row first); false when the clip has none.
    bool source(double frame, Buf& out) const;
    // The source clip's frame range [first, last) in frames (0, 0 when not known).
    void clipRange(double& first, double& last) const;
};

// A filter whose output row depends only on the same row of its input and its settings (a colour effect) can be given as a row function: from 'n'
// premultiplied RGBA float pixels 's' it makes 'n' pixels 'o' (different memory); 'y' is the row number from the top. The host runs it straight from the
// 8-bit frame to the 8-bit result a row at a time (see render() in builtins.cpp). The effect's render function runs the same function over every row of
// Ctx::src, so both give the same values.
typedef std::function<void(const float* s, float* o, int n, int y)> RowFn;

struct Def {
    std::string id, label, group, description;
    bool generator = false;
    bool transition = false;
    std::vector<PDef> params;
    void (*render)(Ctx&) = nullptr;
    void (*changed)(Change&) = nullptr;
    bool general = false;
    bool generalOnly = false;
    bool multiRes = true;
    OfxPluginEntryPoint* overlay = nullptr;
    bool temporal = false;
    void (*destroy)(const void* instance) = nullptr;
    // Makes the row function from the settings (Ctx::params, time, scale... are set; src and out are empty and must not be used).
    // False: this time the effect needs the whole picture (render does the work).
    bool (*rows)(Ctx& c, RowFn& fn) = nullptr;
    // The render function writes every pixel of Ctx::out without reading it first, so the host need not clear it (set by id in builtins.cpp for checked effects).
    bool overwritesOut = false;
    // An 8-bit result keeps colour channels above the alpha (VEGAS's Border bevel leaves colour where its alpha is 0): stored up to 255, not cut to the alpha.
    bool colourAboveAlpha = false;
    // The effect on the GPU (set by vkeffects.cpp for effects with kernels): makes the compute passes that do what render() does, from the settings alone
    // (Ctx::src and out are empty). False: this time the effect needs the CPU.
    bool (*gpu)(Ctx& c, gpu::Plan& plan) = nullptr;
};

// The built-in with this identifier (tests call its render function directly on a Ctx); null when there is none.
const Def* findDef(const std::string& id);

void addGenerators(std::vector<Def>& out);
void addFilters(std::vector<Def>& out);
void addFilters2(std::vector<Def>& out);     // builtins_filters2.cpp
void addFiltersDistort(std::vector<Def>& out);
void addFiltersCookie(std::vector<Def>& out);
void addFiltersMore(std::vector<Def>& out);      // builtins_more.cpp: more of VEGAS's stock effects
void addTransitions(std::vector<Def>& out);      // builtins_trans.cpp: VEGAS's stock transitions
void addFiltersVr(std::vector<Def>& out);
void addFiltersMagix(std::vector<Def>& out);
void addFiltersColor(std::vector<Def>& out);
void addFiltersMask(std::vector<Def>& out);
void addFiltersAnalysis(std::vector<Def>& out);
void addFiltersVegasRest(std::vector<Def>& out);
void attachGpuPlanners(std::vector<Def>& all);
void applyVegasGroups(std::vector<Def>& all);

// Color Corrector (Secondary)'s gamma curve ((Y + c) / (1 + c)) ^ a for a Gamma setting (builtins_filters2.cpp; the GPU planner uses it too).
void secondaryGammaCurve(double gamma, float& c, float& a);

// Levels' gamma curve as 4097 values over 0..1 (builtins_filters.cpp; the GPU planner uses it too).
void levelsGammaTable(double gamma, float* lut4097);
double levelsCurve(double t, double gamma);     // (the exact curve the table is taken from)

// Helpers shared by the effects.
const double kPi = 3.14159265358979323846;
inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline double lerp(double a, double b, double t) { return a + (b - a) * t; }
inline double smooth01(double t) { t = clampd(t, 0, 1); return t * t * (3 - 2 * t); }
// Rec. 709 luma of straight RGB.
inline float luma(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
// Fills a pixel (premultiplied) with a straight colour.
inline void put(float* p, const Color& c) { p[0] = (float)(c.r * c.a); p[1] = (float)(c.g * c.a); p[2] = (float)(c.b * c.a); p[3] = (float)c.a; }
inline Color mix(const Color& a, const Color& b, double t) { return Color{lerp(a.r, b.r, t), lerp(a.g, b.g, t), lerp(a.b, b.b, t), lerp(a.a, b.a, t)}; }
// Straight colour of a premultiplied pixel (0 alpha: black).
inline void unpremul(const float* p, float& r, float& g, float& b, float& a)
{
    a = p[3];
    if (a > 1e-6f) { r = p[0] / a; g = p[1] / a; b = p[2] / a; } else { r = g = b = 0.f; }
}
// A colour value (0..1) cut to whole 8-bit levels the way VEGAS does: truncated, not rounded (a small added amount keeps a value that is a whole level up to float noise on it).
// Linear Blur's taps (builtins_filters.cpp): n taps each way, the step (sx, sy) in pixels, the weights from the centre outwards (summing to 1 over both sides)
void linearBlurTaps(double len, double dx, double dy, int& n, double& sx, double& sy, std::vector<float>& wt);
void pixelateAxis(int n, double p, double scale, std::vector<int>& starts, int& K);
// Chroma Blur's kernel (builtins_more_blur.cpp): a box (triangle = false) of radius r or a triangle of half width r, normalised, 2n+1 taps.
std::vector<float> chromaBlurKernel(bool triangle, double r);
inline float floor8(float v) { v = !(v >= 0.f) ? 0.f : (v > 2.f ? 2.f : v); return std::floor(v * 255.f + 1e-3f) / 255.f; }
inline void premul(float* p, float r, float g, float b, float a) { p[0] = r * a; p[1] = g * a; p[2] = b * a; p[3] = a; }
// An effect's render function for a row function: runs it over every row of Ctx::src into Ctx::out (on all cores for big frames).
void runRows(Ctx& c, bool (*prepare)(Ctx&, RowFn&));
void runRowFn(Ctx& c, const RowFn& fn);
// Runs fn over every row of a frame (on all cores for big ones).
void forRows(int h, int w, const std::function<void(int y)>& fn);
// A Gaussian blur of every channel in place (sigma in pixels, separately across and down; 0: none that way).
void gaussianBlur(Buf& b, double sigmaX, double sigmaY, const bool channels[4] = nullptr);
// The blur of 'src' made in 'dst' (the same as copying src to dst and blurring that, without the copy; dst takes the size of src).
void gaussianBlurFrom(const Buf& src, Buf& dst, double sigmaX, double sigmaY);
// The sigma (pixels) VEGAS's Gaussian Blur uses for a Range setting in a frame this wide.
double gaussianBlurSigma(double range, int frameW);
// Same, of a single-channel plane (w x h floats).
void gaussianBlurPlane(std::vector<float>& plane, int w, int h, double sigmaX, double sigmaY);
// Composites a straight colour through a coverage plane (0..1, w x h) over the frame.
void fillThrough(Buf& b, const std::vector<float>& cover, const Color& c);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
