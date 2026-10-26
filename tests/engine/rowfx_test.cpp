// The colour effects that are row functions (Def::rows: Invert, Black and White, Sepia, Brightness and Contrast,
// Color Corrector (Secondary), Color Curves) must match the plain one-pixel-at-a-time code (copied here) bit for bit
// (-0 counts as 0), on widths that are not a multiple of four and pixels of every kind, and in the engine.
//
//   rowfx_test
#include "../../src/engine/engine.h"
#include "../../src/engine/fxruntime.h"
#include "../../src/media/media.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/builtins_impl.h"
#include "../../src/ofx/ofxhost.h"
#include "../../src/core/pixrows.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace reno;
using namespace reno::ofx::builtin;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

typedef std::map<std::string, std::vector<double>> Params;
static const std::string kV = "com.vegascreativesoftware:";

// ------------------------------------------------------------------ the plain code
static void unpremulRef(const float* p, float& r, float& g, float& b, float& a)
{
    a = p[3];
    if (a > 1e-6f) { r = p[0] / a; g = p[1] / a; b = p[2] / a; } else { r = g = b = 0.f; }
}
template <class F>
static void perPixelRef(const Buf& src, Buf& out, F fn)
{
    for (int y = 0; y < src.h; ++y)
        for (int x = 0; x < src.w; ++x) {
            float r, g, b, a;
            unpremulRef(src.at(x, y), r, g, b, a);
            float* o = out.at(x, y);
            if (a <= 0.f) { std::memset(o, 0, sizeof(float) * 4); continue; }
            fn(r, g, b);
            o[0] = r * a; o[1] = g * a; o[2] = b * a; o[3] = a;
        }
}
static float lumaRef(float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; }
static float clampRef(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static double clampdRef(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static const double kPiRef = 3.14159265358979323846;

static Params withDefaults(const Def& d, const Params& over)
{
    Params p;
    for (const PDef& pd : d.params) p[pd.name] = pd.def;
    for (const auto& kv : over) p[kv.first] = kv.second;
    return p;
}
static double dv(const Params& p, const char* n) { return p.at(n)[0]; }

static void refInvert(const Buf& s, Buf& o, const Params& p)
{
    const float k = (float)clampdRef(dv(p, "BlendAmount"), 0, 1);
    perPixelRef(s, o, [&](float& r, float& g, float& b) { r += (1 - 2 * r) * k; g += (1 - 2 * g) * k; b += (1 - 2 * b) * k; });
}
static void refBlackAndWhite(const Buf& s, Buf& o, const Params& p)
{
    const float k = (float)clampdRef(dv(p, "BlendAmount"), 0, 1);
    perPixelRef(s, o, [&](float& r, float& g, float& b) { const float l = lumaRef(r, g, b); r += (l - r) * k; g += (l - g) * k; b += (l - b) * k; });
}
static void refSepia(const Buf& s, Buf& o, const Params& p)
{
    const std::vector<double>& col = p.at("Color");
    const float strength = (float)clampdRef(dv(p, "BlendingStrength"), 0, 1), falloff = (float)clampdRef(dv(p, "BlendingFalloff"), 0, 1);
    perPixelRef(s, o, [&](float& r, float& g, float& b) {
        const float l = lumaRef(r, g, b);
        const float w = strength * (1.f - falloff * std::fabs(2.f * l - 1.f));
        r = clampRef(l + ((float)col[0] - 0.5f) * w);
        g = clampRef(l + ((float)col[1] - 0.5f) * w);
        b = clampRef(l + ((float)col[2] - 0.5f) * w);
    });
}
static void refBrightness(const Buf& s, Buf& o, const Params& p)
{
    const float bright = (float)dv(p, "Brightness"), contrast = (float)clampdRef(dv(p, "Contrast"), -1, 1), center = (float)dv(p, "ContrastCenter");
    const float f = contrast >= 0 ? 1.f / std::max(0.01f, 1.f - contrast) : 1.f + contrast;
    perPixelRef(s, o, [&](float& r, float& g, float& b) {
        r = (r + bright - center) * f + center;
        g = (g + bright - center) * f + center;
        b = (b + bright - center) * f + center;
    });
}

// Color Corrector (Secondary), as it was
static void toYuvRef(float r, float g, float b, float& y, float& u, float& v) { y = 0.299f * r + 0.587f * g + 0.114f * b; u = 0.492f * (b - y); v = 0.877f * (r - y); }
static void fromYuvRef(float y, float u, float v, float& r, float& g, float& b)
{
    r = y + v / 0.877f;
    b = y + u / 0.492f;
    g = (y - 0.299f * r - 0.114f * b) / 0.587f;
}
static float levelsOfRef(float c, float gain, float invGamma, float offset)
{
    c *= gain;
    c = c > 0.f ? (invGamma == 1.f ? c : std::pow(c, invGamma)) : c;
    return c + offset;
}
static float inRangeRef(float x, float lo, float hi, float smooth)
{
    if (hi < lo) std::swap(lo, hi);
    if (x >= lo && x <= hi) return 1.f;
    const float d = x < lo ? lo - x : x - hi;
    return smooth > 1e-6f ? std::max(0.f, 1.f - d / smooth) : 0.f;
}
static void refSecondary(const Buf& src, Buf& out, const Params& p)
{
    const double ca = p.at("Chrominance")[0], cm = p.at("Chrominance")[1];
    const double a0 = ca * kPiRef / 180.0, k0 = clampdRef(cm, -2, 2) * 0.21;
    const float cu = (float)(std::cos(a0) * k0), cv = (float)(std::sin(a0) * k0);
    const double rot = dv(p, "RotateHue") * kPiRef / 180.0;
    const float cr = (float)std::cos(rot), sr = (float)std::sin(rot);
    const float sat = (float)clampdRef(dv(p, "Saturation"), 0, 100);
    const float invGamma = 1.f / (float)clampdRef(dv(p, "Gamma"), 0.01, 100);
    const float gain = (float)clampdRef(dv(p, "Gain"), -100, 100);
    const float offset = (float)(clampdRef(dv(p, "Offset"), -1000, 1000) / 255.0);
    const float alpha = (float)clampdRef(dv(p, "Alpha"), 0, 1);
    const bool showMask = dv(p, "ShowMask") != 0, invert = dv(p, "InvertMask") != 0;
    const bool useL = dv(p, "EnableLimitLuminance") != 0, useS = dv(p, "EnableLimitSaturation") != 0, useH = dv(p, "EnableLimitHue") != 0;
    const float l0 = (float)dv(p, "LimitLuminanceLow"), l1 = (float)dv(p, "LimitLuminanceHigh"), ls = (float)std::max(0.0, dv(p, "LimitLuminanceSmooth"));
    const float s0 = (float)dv(p, "LimitSaturationLow"), s1 = (float)dv(p, "LimitSaturationHigh"), ss = (float)std::max(0.0, dv(p, "LimitSaturationSmooth"));
    const float hc = (float)dv(p, "LimitHueCenter"), hw = (float)std::max(0.0, dv(p, "LimitHueWidth")), hs = (float)std::max(0.0, dv(p, "LimitHueSmooth"));
    for (int yy = 0; yy < src.h; ++yy)
        for (int xx = 0; xx < src.w; ++xx) {
            float r, g, b, a;
            unpremulRef(src.at(xx, yy), r, g, b, a);
            float* o = out.at(xx, yy);
            if (a <= 0.f) { std::memset(o, 0, sizeof(float) * 4); continue; }
            float Y, U, V;
            toYuvRef(r, g, b, Y, U, V);
            float m = 1.f;
            if (useL) m *= inRangeRef(Y * 255.f, l0, l1, ls);
            if (useS) m *= inRangeRef(std::sqrt(U * U + V * V) * 255.f, s0, s1, ss);
            if (useH && hw < 360.f) {
                float hue = std::atan2(V, U) * (float)(180.0 / kPiRef);
                float d = std::fabs(std::fmod(hue - hc, 360.f));
                if (d > 180.f) d = 360.f - d;
                m *= inRangeRef(d, 0.f, hw * 0.5f, hs);
            }
            if (invert) m = 1.f - m;
            if (showMask) { o[0] = m * a; o[1] = m * a; o[2] = m * a; o[3] = a; continue; }
            if (m <= 0.f) { std::memcpy(o, src.at(xx, yy), sizeof(float) * 4); continue; }
            const float u2 = (U * cr - V * sr) * sat + cu, v2 = (U * sr + V * cr) * sat + cv;
            float nr, ng, nb;
            fromYuvRef(Y, u2, v2, nr, ng, nb);
            nr = levelsOfRef(nr, gain, invGamma, offset);
            ng = levelsOfRef(ng, gain, invGamma, offset);
            nb = levelsOfRef(nb, gain, invGamma, offset);
            const float A = a * (1.f + (alpha - 1.f) * m);
            o[0] = (r + (nr - r) * m) * A; o[1] = (g + (ng - g) * m) * A; o[2] = (b + (nb - b) * m) * A; o[3] = A;
        }
}

// ------------------------------------------------------------------ pictures
// Pixels of every kind: smooth colour, noise, bars, and runs of special ones (no alpha, tiny alpha, soft alpha,
// colour above alpha, negative and large values).
static Buf trickyPicture(int W, int H, uint32_t seed)
{
    Buf b;
    b.resize(W, H);
    auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / 16777216.f; };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            float* p = b.at(x, y);
            float r = rnd(), g = rnd(), bl = rnd(), a = 1.f;
            if (x % 29 == 3) a = 0.f;
            else if (x % 31 == 5) a = 1e-7f;
            else if (x % 11 == 2) a = 0.1f + 0.8f * rnd();
            else if (x % 13 == 4) a = 0.5f;
            p[0] = r * a; p[1] = g * a; p[2] = bl * a; p[3] = a;
            if (y % 7 == 3 && x % 5 == 1) { p[0] = 1.3f; p[1] = -0.2f; }             // above alpha / negative
            if (y % 9 == 4 && x % 6 == 2) { p[2] = 4.f; p[3] = 1.f; }
            if (y % 17 == 5 && x % 8 == 0) { p[0] = p[1] = p[2] = 0.5f; p[3] = 1.f; }  // exact grey
        }
    return b;
}

static bool sameBits(const Buf& a, const Buf& b, std::string& why)
{
    if (a.w != b.w || a.h != b.h) { why = "size"; return false; }
    for (size_t i = 0; i < a.px.size(); ++i) {
        float x = a.px[i], y = b.px[i];
        if (x == 0.f) x = 0.f;              // (-0 is 0)
        if (y == 0.f) y = 0.f;
        uint32_t ux, uy;
        std::memcpy(&ux, &x, 4);
        std::memcpy(&uy, &y, 4);
        if (ux != uy && !(std::isnan(x) && std::isnan(y))) {
            char buf[200];
            std::snprintf(buf, sizeof buf, "pixel %zu channel %zu: %.9g against %.9g", i / 4, i % 4, (double)x, (double)y);
            why = buf;
            return false;
        }
    }
    return true;
}

struct Case {
    const char* name;
    std::string id;
    Params over;
    void (*ref)(const Buf&, Buf&, const Params&);
};

static void directTests()
{
    const std::vector<Case> cases = {
        {"invert", kV + "invert", {}, refInvert},
        {"invert half", kV + "invert", {{"BlendAmount", {0.37}}}, refInvert},
        {"black and white", kV + "blackandwhite", {}, refBlackAndWhite},
        {"black and white 30%", kV + "blackandwhite", {{"BlendAmount", {0.3}}}, refBlackAndWhite},
        {"sepia", kV + "sepia", {}, refSepia},
        {"sepia green", kV + "sepia", {{"Color", {0.2, 0.9, 0.1}}, {"BlendingStrength", {1}}, {"BlendingFalloff", {0.2}}}, refSepia},
        {"brightness", kV + "brightnessandcontrast", {}, refBrightness},
        {"brightness up, contrast up", kV + "brightnessandcontrast", {{"Brightness", {0.2}}, {"Contrast", {0.5}}}, refBrightness},
        {"brightness down, contrast down", kV + "brightnessandcontrast", {{"Brightness", {-0.3}}, {"Contrast", {-0.6}}, {"ContrastCenter", {0.4}}}, refBrightness},
        {"secondary default", kV + "colorcorrectorsecondary", {}, refSecondary},
        {"secondary tint", kV + "colorcorrectorsecondary", {{"Chrominance", {120, 0.7}}, {"Saturation", {1.4}}, {"Gain", {1.2}}, {"Offset", {10}}}, refSecondary},
        {"secondary gamma", kV + "colorcorrectorsecondary", {{"Gamma", {1.7}}, {"RotateHue", {35}}, {"Alpha", {0.6}}}, refSecondary},
        {"secondary luminance", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"LimitLuminanceLow", {80}}, {"LimitLuminanceHigh", {200}}, {"LimitLuminanceSmooth", {30}}, {"RotateHue", {90}}}, refSecondary},
        {"secondary luminance reversed", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"LimitLuminanceLow", {200}}, {"LimitLuminanceHigh", {80}}, {"Saturation", {0}}}, refSecondary},
        {"secondary saturation", kV + "colorcorrectorsecondary", {{"EnableLimitSaturation", {1}}, {"LimitSaturationLow", {20}}, {"LimitSaturationHigh", {120}}, {"LimitSaturationSmooth", {15}}, {"Gain", {0.5}}}, refSecondary},
        {"secondary hue", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {80}}, {"LimitHueSmooth", {40}}, {"Saturation", {2}}}, refSecondary},
        {"secondary hue across zero", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {350}}, {"LimitHueWidth", {120}}, {"Alpha", {0.3}}, {"Offset", {-20}}}, refSecondary},
        {"secondary all limits", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"EnableLimitSaturation", {1}}, {"EnableLimitHue", {1}}, {"LimitLuminanceLow", {30}}, {"LimitLuminanceHigh", {220}}, {"LimitSaturationLow", {10}}, {"LimitSaturationHigh", {150}}, {"LimitHueCenter", {200}}, {"LimitHueWidth", {200}}, {"LimitHueSmooth", {20}}, {"Gamma", {0.6}}, {"Chrominance", {250, 0.4}}}, refSecondary},
        {"secondary inverted mask", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {80}}, {"InvertMask", {1}}, {"Saturation", {0.2}}}, refSecondary},
        {"secondary show mask", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"LimitLuminanceHigh", {120}}, {"LimitLuminanceSmooth", {40}}, {"ShowMask", {1}}}, refSecondary},
    };
    const int sizes[][2] = {{1, 1}, {3, 2}, {4, 4}, {7, 5}, {33, 17}, {130, 9}, {301, 40}};
    for (const Case& cs : cases) {
        const Def* d = findDef(cs.id);
        if (!d) { check(false, std::string(cs.name) + ": built-in exists"); continue; }
        check(d->rows != nullptr, std::string(cs.name) + ": is a row function");
        bool ok = true;
        std::string why;
        for (const auto& sz : sizes) {
            const Buf src = trickyPicture(sz[0], sz[1], 99 + (uint32_t)sz[0]);
            Params store = withDefaults(*d, cs.over);
            Ctx c;
            c.testParams = &store;
            c.src = src;
            c.out.resize(sz[0], sz[1]);
            c.frameW = sz[0];
            c.frameH = sz[1];
            d->render(c);
            Buf want;
            want.resize(sz[0], sz[1]);
            cs.ref(src, want, store);
            if (!sameBits(c.out, want, why)) { ok = false; why = std::to_string(sz[0]) + "x" + std::to_string(sz[1]) + ": " + why; break; }
        }
        check(ok, std::string(cs.name) + ": same floats as the plain code", why);
    }
}

// ------------------------------------------------------------------ the engine's side
class Still : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        const Buf b = trickyPicture(std::min(maxW, mi.width), std::min(maxH, mi.height), 4242);
        ImagePtr img = Image::make(b.w, b.h, PixelDepth::Byte, false);
        for (int y = 0; y < b.h; ++y) {
            uint8_t* d = img->row(b.h - 1 - y);
            for (int x = 0; x < b.w; ++x, d += 4) {
                const float* p = b.at(x, y);
                // (opaque: the engine lays a clip over black, so the picture it sends through the effect is what the effect must see)
                auto q = [&](float v) { return (uint8_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f); };
                d[0] = q(p[2]); d[1] = q(p[1]); d[2] = q(p[0]); d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};
static std::unique_ptr<MediaReader> openStill(const std::string&, std::string*)
{
    std::unique_ptr<Still> r(new Still());
    r->mi.hasVideo = true;
    r->mi.isStill = true;
    r->mi.width = 203;
    r->mi.height = 91;
    return std::unique_ptr<MediaReader>(r.release());
}

static FxSlot slotOf(Project& p, const std::string& id, const Params& params)
{
    FxSlot s;
    s.id = p.newId();
    s.kind = "ofx";
    s.pluginId = id;
    s.name = id;
    Json pj = Json::object();
    for (const auto& kv : params) {
        Json e = Json::object(), a = Json::array();
        for (double x : kv.second) a.push(x);
        e["v"] = a;
        pj[kv.first] = e;
    }
    Json st = Json::object();
    st["params"] = pj;
    s.state = st;
    return s;
}

static ImagePtr renderWith(const std::string& id, const Params& params, bool withEffect)
{
    MediaPool pool;
    pool.setOpener(openStill);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    Project p;
    p.settings.width = 203;
    p.settings.height = 91;
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    p.tracks.push_back(t);
    MediaItem mi;
    mi.id = p.newId();
    mi.path = "still";
    std::string err;
    mi.info = openStill("still", &err)->info();
    p.media.push_back(mi);
    Event e;
    e.id = p.newId();
    e.mediaId = mi.id;
    e.start = 0;
    e.length = 5;
    if (withEffect) e.fx.push_back(slotOf(p, id, params));
    p.tracks[0].events.push_back(e);
    fx.prepare(p);
    FrameOptions fo;
    fo.parallel = false;
    ImagePtr img = video.render(p, 1.0, fo);
    fx.releaseExcept({});
    return img;
}

static void engineTests()
{
    const std::vector<Case> cases = {
        {"invert", kV + "invert", {{"BlendAmount", {0.8}}}, refInvert},
        {"black and white", kV + "blackandwhite", {}, refBlackAndWhite},
        {"sepia", kV + "sepia", {}, refSepia},
        {"brightness and contrast", kV + "brightnessandcontrast", {{"Brightness", {0.1}}, {"Contrast", {0.4}}}, refBrightness},
        {"secondary", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {160}}, {"Saturation", {1.6}}, {"Gamma", {1.3}}}, refSecondary},
    };
    for (const Case& cs : cases) {
        const Def* d = findDef(cs.id);
        ImagePtr plain = renderWith(cs.id, cs.over, false), with = renderWith(cs.id, cs.over, true);
        if (!d || !plain || !with) { check(false, std::string("engine: ") + cs.name + " renders"); continue; }
        // the picture without the effect as the host loads it, the plain code over it, stored as the host stores a frame
        Buf src;
        src.resize(plain->w, plain->h);
        for (int y = 0; y < plain->h; ++y) {
            const uint8_t* row = plain->row(plain->h - 1 - y);
            for (int x = 0; x < plain->w; ++x) {
                float* q = src.at(x, y);
                q[0] = row[x * 4 + 2] * (1.f / 255.f); q[1] = row[x * 4 + 1] * (1.f / 255.f); q[2] = row[x * 4] * (1.f / 255.f); q[3] = row[x * 4 + 3] * (1.f / 255.f);
            }
        }
        Params store = withDefaults(*d, cs.over);
        Buf want;
        want.resize(src.w, src.h);
        cs.ref(src, want, store);
        size_t bad = 0;
        for (int y = 0; y < with->h; ++y) {
            std::vector<uint8_t> exp((size_t)with->w * 4);
            pix::floatRowToBytes(want.at(0, with->h - 1 - y), exp.data(), with->w, true);
            if (std::memcmp(exp.data(), with->row(y), exp.size()) != 0) {
                if (!bad && std::getenv("ROWFX_DEBUG"))
                    for (int x = 0; x < with->w; ++x)
                        if (std::memcmp(&exp[x * 4], with->row(y) + x * 4, 4)) {
                            std::printf("  row %d x %d want %d %d %d %d got %d %d %d %d plain %d %d %d %d\n", y, x, exp[x*4], exp[x*4+1], exp[x*4+2], exp[x*4+3],
                                        with->row(y)[x*4], with->row(y)[x*4+1], with->row(y)[x*4+2], with->row(y)[x*4+3],
                                        plain->row(y)[x*4], plain->row(y)[x*4+1], plain->row(y)[x*4+2], plain->row(y)[x*4+3]);
                            break;
                        }
                ++bad;
            }
        }
        check(bad == 0, std::string("engine: ") + cs.name + " in a project is the plain code's picture", std::to_string(bad) + " rows differ");
    }
}

int main()
{
    directTests();
    engineTests();
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
