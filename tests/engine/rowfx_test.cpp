// The colour effects that are row functions (Def::rows: Invert, Black and White, Sepia, Brightness and Contrast,
// Color Corrector (Secondary)) must give each pixel the value they give it alone (a 1x1 picture), bit for bit (-0 counts as
// 0), on widths that are not a multiple of four and pixels of every kind, for 8-bit and float pictures, and in the engine.
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

// ------------------------------------------------------------------ the one-pixel-at-a-time code
// The effects' own arithmetic follows VEGAS's (integer steps on 8-bit pictures) and no longer has a separate plain copy to
// compare with, so the reference is the effect itself on a 1x1 picture, pixel by pixel. A row function must give a pixel the
// same value in any row, at any x, with any neighbours and for any width (not only multiples of four).
static Params withDefaults(const Def& d, const Params& over)
{
    Params p;
    for (const PDef& pd : d.params) p[pd.name] = pd.def;
    for (const auto& kv : over) p[kv.first] = kv.second;
    return p;
}
static void refByPixel(const Def& d, const Buf& s, Buf& o, const Params& store, bool levels)
{
    o.resize(s.w, s.h);
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            Params one = store;
            Ctx c;
            c.testParams = &one;
            c.levels = levels;
            c.src.resize(1, 1);
            std::memcpy(c.src.at(0, 0), s.at(x, y), sizeof(float) * 4);
            c.out.resize(1, 1);
            c.frameW = 1;
            c.frameH = 1;
            d.render(c);
            std::memcpy(o.at(x, y), c.out.at(0, 0), sizeof(float) * 4);
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
};

static void directTests()
{
    const std::vector<Case> cases = {
        {"invert", kV + "invert", {}},
        {"invert half", kV + "invert", {{"BlendAmount", {0.37}}}},
        {"black and white", kV + "blackandwhite", {}},
        {"black and white 30%", kV + "blackandwhite", {{"BlendAmount", {0.3}}}},
        {"sepia", kV + "sepia", {}},
        {"sepia green", kV + "sepia", {{"Color", {0.2, 0.9, 0.1}}, {"BlendingStrength", {1}}, {"BlendingFalloff", {0.2}}}},
        {"brightness", kV + "brightnessandcontrast", {}},
        {"brightness up, contrast up", kV + "brightnessandcontrast", {{"Brightness", {0.2}}, {"Contrast", {0.5}}}},
        {"brightness down, contrast down", kV + "brightnessandcontrast", {{"Brightness", {-0.3}}, {"Contrast", {-0.6}}, {"ContrastCenter", {0.4}}}},
        {"secondary default", kV + "colorcorrectorsecondary", {}},
        {"secondary tint", kV + "colorcorrectorsecondary", {{"Chrominance", {120, 0.7}}, {"Saturation", {1.4}}, {"Gain", {1.2}}, {"Offset", {10}}}},
        {"secondary gamma", kV + "colorcorrectorsecondary", {{"Gamma", {1.7}}, {"RotateHue", {35}}, {"Alpha", {0.6}}}},
        {"secondary luminance", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"LimitLuminanceLow", {80}}, {"LimitLuminanceHigh", {200}}, {"LimitLuminanceSmooth", {30}}, {"RotateHue", {90}}}},
        {"secondary luminance reversed", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"LimitLuminanceLow", {200}}, {"LimitLuminanceHigh", {80}}, {"Saturation", {0}}}},
        {"secondary saturation", kV + "colorcorrectorsecondary", {{"EnableLimitSaturation", {1}}, {"LimitSaturationLow", {20}}, {"LimitSaturationHigh", {120}}, {"LimitSaturationSmooth", {15}}, {"Gain", {0.5}}}},
        {"secondary hue", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {80}}, {"LimitHueSmooth", {40}}, {"Saturation", {2}}}},
        {"secondary hue across zero", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {350}}, {"LimitHueWidth", {120}}, {"Alpha", {0.3}}, {"Offset", {-20}}}},
        {"secondary all limits", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"EnableLimitSaturation", {1}}, {"EnableLimitHue", {1}}, {"LimitLuminanceLow", {30}}, {"LimitLuminanceHigh", {220}}, {"LimitSaturationLow", {10}}, {"LimitSaturationHigh", {150}}, {"LimitHueCenter", {200}}, {"LimitHueWidth", {200}}, {"LimitHueSmooth", {20}}, {"Gamma", {0.6}}, {"Chrominance", {250, 0.4}}}},
        {"secondary inverted mask", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {80}}, {"InvertMask", {1}}, {"Saturation", {0.2}}}},
        {"secondary show mask", kV + "colorcorrectorsecondary", {{"EnableLimitLuminance", {1}}, {"LimitLuminanceHigh", {120}}, {"LimitLuminanceSmooth", {40}}, {"ShowMask", {1}}}},
    };
    const int sizes[][2] = {{1, 1}, {3, 2}, {4, 4}, {7, 5}, {33, 17}, {130, 9}, {301, 40}};
    for (const Case& cs : cases) {
        const Def* d = findDef(cs.id);
        if (!d) { check(false, std::string(cs.name) + ": built-in exists"); continue; }
        check(d->rows != nullptr, std::string(cs.name) + ": is a row function");
        for (const bool lv : {true, false}) {
        bool ok = true;
        std::string why;
        for (const auto& sz : sizes) {
            const Buf src = trickyPicture(sz[0], sz[1], 99 + (uint32_t)sz[0]);
            Params store = withDefaults(*d, cs.over);
            Ctx c;
            c.testParams = &store;
            c.levels = lv;                  // (an 8-bit picture takes VEGAS's integer steps, a float one the float arithmetic)
            c.src = src;
            c.out.resize(sz[0], sz[1]);
            c.frameW = sz[0];
            c.frameH = sz[1];
            d->render(c);
            Buf want;
            want.resize(sz[0], sz[1]);
            refByPixel(*d, src, want, store, lv);
            if (!sameBits(c.out, want, why)) { ok = false; why = std::to_string(sz[0]) + "x" + std::to_string(sz[1]) + ": " + why; break; }
        }
        check(ok, std::string(cs.name) + (lv ? " (8-bit steps)" : " (float)") + ": same floats as pixel by pixel", why);
        }
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
        {"invert", kV + "invert", {{"BlendAmount", {0.8}}}},
        {"black and white", kV + "blackandwhite", {}},
        {"sepia", kV + "sepia", {}},
        {"brightness and contrast", kV + "brightnessandcontrast", {{"Brightness", {0.1}}, {"Contrast", {0.4}}}},
        {"secondary", kV + "colorcorrectorsecondary", {{"EnableLimitHue", {1}}, {"LimitHueCenter", {100}}, {"LimitHueWidth", {160}}, {"Saturation", {1.6}}, {"Gamma", {1.3}}}},
    };
    // (a finished frame is always 8-bit, and so are the pictures effects get from it: the integer steps are the ones in use)
    const bool isFloat = false;
    {
        for (const Case& cs : cases) {
            const std::string name = std::string("engine: ") + cs.name;
            const Def* d = findDef(cs.id);
            ImagePtr plain = renderWith(cs.id, cs.over, false), with = renderWith(cs.id, cs.over, true);
            if (!d || !plain || !with) { check(false, name + " renders"); continue; }
            const PixelDepth depth = isFloat ? PixelDepth::Float : PixelDepth::Byte;
            if (plain->depth != depth || with->depth != depth || plain->w != with->w || plain->h != with->h) { check(false, name + " is a picture of the project's depth"); continue; }
            // the picture without the effect as the host loads it (bottom row first), the effect pixel by pixel over it
            Buf src;
            src.resize(plain->w, plain->h);
            for (int y = 0; y < plain->h; ++y) {
                const uint8_t* row = plain->row(plain->h - 1 - y);
                if (isFloat) { std::memcpy(src.at(0, y), row, (size_t)plain->w * 16); continue; }
                for (int x = 0; x < plain->w; ++x) {
                    float* q = src.at(x, y);
                    q[0] = row[x * 4 + 2] * (1.f / 255.f); q[1] = row[x * 4 + 1] * (1.f / 255.f); q[2] = row[x * 4] * (1.f / 255.f); q[3] = row[x * 4 + 3] * (1.f / 255.f);
                }
            }
            Params store = withDefaults(*d, cs.over);
            Buf want;
            refByPixel(*d, src, want, store, !isFloat);
            size_t bad = 0;
            for (int y = 0; y < with->h; ++y) {
                const float* exp = want.at(0, with->h - 1 - y);
                if (isFloat) {
                    const float* got = reinterpret_cast<const float*>(with->row(y));
                    bool same = true;
                    for (int i = 0; i < with->w * 4 && same; ++i) {
                        float u = got[i], v = exp[i];
                        if (u == 0.f) u = 0.f;              // (-0 is 0)
                        if (v == 0.f) v = 0.f;
                        same = std::memcmp(&u, &v, 4) == 0 || (std::isnan(u) && std::isnan(v));
                    }
                    if (!same) ++bad;
                } else {
                    std::vector<uint8_t> bytes((size_t)with->w * 4);
                    pix::floatRowToBytes(exp, bytes.data(), with->w, true);
                    if (std::memcmp(bytes.data(), with->row(y), bytes.size()) != 0) ++bad;
                }
            }
            check(bad == 0, name + " in a project is the pixel-by-pixel picture", std::to_string(bad) + " rows differ");
        }
    }
}

int main()
{
    directTests();
    engineTests();
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
