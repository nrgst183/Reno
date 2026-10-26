// Built-in versions of VEGAS Pro 18's Color Grading, Auto Looks, Color Match and LUT Filter (all in VEGAS's Vfx1 bundle). Identifiers and
// parameter names are the ones the plug-in declares (read from Vfx1.ofx's strings, Vfx1.xml and its preset file), so projects load and save their values.
//
//   lutfilter    LUT Filter: a .cube (or .3dl) table applied with linear or tetrahedral interpolation, mixed in by Strength (Gain).
//   autolooks    AutoLooks: 21 named film looks (Look 1..21 = VEGAS's Resources\AutoLooks 1.cube .. 21.cube). The real tables are used when that folder
//                is found (VEGAS installed); otherwise each look is a grade made up by Reno in the spirit of its name (a guess).
//   colormatch   Color Match: moves the clip's colours towards a reference picture's. "internaldata" holds the statistics VEGAS captured (decoded from
//                its presets: mean and std of L, a, b for the source, then the target); the transfer is Reinhard's in Lab: out = (x - targetMean) x
//                sourceStd / targetStd + sourceMean per channel, mixed in by Strength, L left alone unless "Match brightness" is on. Without a captured
//                target the clip's own frame is the target.
//   colorgrading Color Grading: the panel's result stored as parameters (Lift, Gamma, Gain, Offset with R G B Y U V, Saturation, Exposure, levels,
//                hue/sat/value, Bezier curves, LUT slots). Parameter names are exact; defaults, ranges and how they combine are guesses (VEGAS's maths
//                is not published): out = (gain x (c + lift x (1 - c)))^(1 / gamma) + offset per channel after input levels and exposure, then
//                saturation, hue/sat/value, curves, output levels and LUT slots.
//
// All four work per pixel, so they are correct at any render scale; they are thread safe and row-parallel, with no allocation per pixel.
#include "builtins_impl.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>

namespace reno {
namespace ofx {
namespace builtin {
namespace {

const char* const kGroup = "Reno Built-in";

Def make(const char* id, const char* label, const char* what, std::vector<PDef> params, void (*fn)(Ctx&))
{
    Def d;
    d.id = id;
    d.label = label;
    d.group = kGroup;
    d.description = std::string(what) + " (Reno's own, standing in for VEGAS " + label + ").";
    d.params = std::move(params);
    d.render = fn;
    return d;
}

inline void copyFrame(Ctx& c)
{
    if (c.out.px.size() == c.src.px.size()) c.out.px = c.src.px;
}

// ------------------------------------------------------------------ colour tables
struct Lut {
    int n = 0;
    std::vector<float> rgb;
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
};
typedef std::shared_ptr<const Lut> LutPtr;

// A file name (UTF-8) as the file system wants it.
inline std::filesystem::path fsPath(const std::string& utf8)
{
#ifdef _WIN32
    return std::filesystem::path(widen(utf8));
#else
    return std::filesystem::path(utf8);
#endif
}

// One colour (straight RGB) through a table, trilinear or tetrahedral. Colours outside the table's domain are held at its edge.
void lutApply(const Lut& L, float c[3], bool tetra)
{
    const int n = L.n;
    float p[3];
    for (int k = 0; k < 3; ++k) {
        const float span = L.dmax[k] - L.dmin[k];
        float v = span > 1e-12f ? (c[k] - L.dmin[k]) / span : 0.f;
        v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
        p[k] = v * (float)(n - 1);
    }
    const int i0[3] = {std::min(n - 2, (int)p[0]), std::min(n - 2, (int)p[1]), std::min(n - 2, (int)p[2])};
    const float f[3] = {p[0] - (float)i0[0], p[1] - (float)i0[1], p[2] - (float)i0[2]};
    const float* base = &L.rgb[(((size_t)i0[2] * n + (size_t)i0[1]) * n + (size_t)i0[0]) * 3];
    const size_t sr = 3, sg = (size_t)n * 3, sb = (size_t)n * n * 3;
    auto at = [&](int r, int g, int b, int k) { return base[(size_t)r * sr + (size_t)g * sg + (size_t)b * sb + (size_t)k]; };
    for (int k = 0; k < 3; ++k) {
        float out;
        if (!tetra) {
            const float c00 = at(0, 0, 0, k) * (1 - f[0]) + at(1, 0, 0, k) * f[0], c10 = at(0, 1, 0, k) * (1 - f[0]) + at(1, 1, 0, k) * f[0];
            const float c01 = at(0, 0, 1, k) * (1 - f[0]) + at(1, 0, 1, k) * f[0], c11 = at(0, 1, 1, k) * (1 - f[0]) + at(1, 1, 1, k) * f[0];
            const float c0 = c00 * (1 - f[1]) + c10 * f[1], c1 = c01 * (1 - f[1]) + c11 * f[1];
            out = c0 * (1 - f[2]) + c1 * f[2];
        } else {
            const float fr = f[0], fg = f[1], fb = f[2];
            const float c000 = at(0, 0, 0, k), c111 = at(1, 1, 1, k);
            if (fr >= fg) {
                if (fg >= fb) out = c000 + fr * (at(1, 0, 0, k) - c000) + fg * (at(1, 1, 0, k) - at(1, 0, 0, k)) + fb * (c111 - at(1, 1, 0, k));
                else if (fr >= fb) out = c000 + fr * (at(1, 0, 0, k) - c000) + fb * (at(1, 0, 1, k) - at(1, 0, 0, k)) + fg * (c111 - at(1, 0, 1, k));
                else out = c000 + fb * (at(0, 0, 1, k) - c000) + fr * (at(1, 0, 1, k) - at(0, 0, 1, k)) + fg * (c111 - at(1, 0, 1, k));
            } else {
                if (fb >= fg) out = c000 + fb * (at(0, 0, 1, k) - c000) + fg * (at(0, 1, 1, k) - at(0, 0, 1, k)) + fr * (c111 - at(0, 1, 1, k));
                else if (fb >= fr) out = c000 + fg * (at(0, 1, 0, k) - c000) + fb * (at(0, 1, 1, k) - at(0, 1, 0, k)) + fr * (c111 - at(0, 1, 1, k));
                else out = c000 + fg * (at(0, 1, 0, k) - c000) + fr * (at(1, 1, 0, k) - at(0, 1, 0, k)) + fb * (c111 - at(1, 1, 0, k));
            }
        }
        c[k] = out;
    }
}

bool numbers(const std::string& line, double* v, int count)
{
    const char* s = line.c_str();
    for (int i = 0; i < count; ++i) {
        char* e = nullptr;
        v[i] = std::strtod(s, &e);
        if (e == s) return false;
        s = e;
    }
    return true;
}

// A 1D table (three curves) turned into a 3D one (the curves do not mix channels, so 33 points a side are plenty).
LutPtr lutFrom1D(const std::vector<float>& d, int m, const float dmin[3], const float dmax[3])
{
    auto L = std::make_shared<Lut>();
    const int n = 33;
    L->n = n;
    L->rgb.resize((size_t)n * n * n * 3);
    for (int k = 0; k < 3; ++k) { L->dmin[k] = dmin[k]; L->dmax[k] = dmax[k]; }
    auto look = [&](int ch, float t) {
        const float p = std::min(1.f, std::max(0.f, t)) * (float)(m - 1);
        const int i = std::min(m - 2, (int)p);
        const float f = p - (float)i;
        return d[(size_t)i * 3 + (size_t)ch] * (1 - f) + d[(size_t)(i + 1) * 3 + (size_t)ch] * f;
    };
    size_t o = 0;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                L->rgb[o++] = look(0, (float)r / (n - 1));
                L->rgb[o++] = look(1, (float)g / (n - 1));
                L->rgb[o++] = look(2, (float)b / (n - 1));
            }
    return L;
}

// Reads a .cube file (1D or 3D) or a .3dl file. Null if it is neither or is too big.
LutPtr readLutFile(const std::string& path)
{
    std::error_code ec;
    const auto sz = std::filesystem::file_size(fsPath(path), ec);
    if (ec || sz == 0 || sz > (64u << 20)) return nullptr;
    std::ifstream f(fsPath(path), std::ios::binary);
    if (!f) return nullptr;
    int size3 = 0, size1 = 0;
    float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    std::vector<float> data;
    std::vector<std::string> lines3dl;
    std::string line;
    bool is3dl = false;
    int meshBits = 0;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        size_t a = 0;
        while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
        if (a >= line.size() || line[a] == '#') continue;
        const char* s = line.c_str() + a;
        const char c0 = s[0];
        if ((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z')) {
            if (!std::strncmp(s, "LUT_3D_SIZE", 11)) size3 = std::atoi(s + 11);
            else if (!std::strncmp(s, "LUT_1D_SIZE", 11)) size1 = std::atoi(s + 11);
            else if (!std::strncmp(s, "DOMAIN_MIN", 10)) { double v[3]; if (numbers(s + 10, v, 3)) for (int k = 0; k < 3; ++k) dmin[k] = (float)v[k]; }
            else if (!std::strncmp(s, "DOMAIN_MAX", 10)) { double v[3]; if (numbers(s + 10, v, 3)) for (int k = 0; k < 3; ++k) dmax[k] = (float)v[k]; }
            continue;
        }
        std::vector<double> nums;
        {
            std::istringstream is(line.substr(a));
            double x;
            while (nums.size() < 200 && (is >> x)) nums.push_back(x);
        }
        if (nums.size() == 3) {
            if ((int)data.size() > 3 * 130 * 130 * 130) return nullptr;
            data.push_back((float)nums[0]); data.push_back((float)nums[1]); data.push_back((float)nums[2]);
        } else if (data.empty() && !is3dl && nums.size() >= 4 && nums.size() <= 129) {
            is3dl = true;
            size3 = (int)nums.size();
        } else {
            return nullptr;
        }
    }
    if (is3dl) {
        if (data.size() == (size_t)size3 * size3 * size3 * 3 && size3 >= 2) {
            float mx = 0;
            for (float v : data) mx = std::max(mx, v);
            meshBits = mx > 4095.5f ? 16 : (mx > 1023.5f ? 12 : (mx > 255.5f ? 10 : 8));
            const float top = (float)((1u << meshBits) - 1);
            for (float& v : data) v /= top;
            auto L = std::make_shared<Lut>();
            L->n = size3;
            L->rgb.resize(data.size());
            for (int r = 0; r < size3; ++r)
                for (int g = 0; g < size3; ++g)
                    for (int b = 0; b < size3; ++b)
                        for (int k = 0; k < 3; ++k)
                            L->rgb[(((size_t)b * size3 + g) * size3 + r) * 3 + k] = data[(((size_t)r * size3 + g) * size3 + b) * 3 + k];
            return L;
        }
        return nullptr;
    }
    if (size3 >= 2 && size3 <= 129 && data.size() >= (size_t)size3 * size3 * size3 * 3) {
        auto L = std::make_shared<Lut>();
        L->n = size3;
        L->rgb.assign(data.begin(), data.begin() + (size_t)size3 * size3 * size3 * 3);
        for (int k = 0; k < 3; ++k) { L->dmin[k] = dmin[k]; L->dmax[k] = dmax[k]; }
        return L;
    }
    if (size1 >= 2 && size1 <= 65536 && data.size() >= (size_t)size1 * 3) return lutFrom1D(data, size1, dmin, dmax);
    return nullptr;
}

// Files read are kept (by path and modified time) so a frame does not read the file again.
LutPtr cachedLut(const std::string& path)
{
    if (path.empty()) return nullptr;
    static std::mutex mu;
    static std::map<std::string, std::pair<long long, LutPtr>> cache;
    long long stamp = 0;
    {
        std::error_code ec;
        const auto t = std::filesystem::last_write_time(fsPath(path), ec);
        if (ec) return nullptr;
        stamp = (long long)t.time_since_epoch().count();
    }
    std::lock_guard<std::mutex> lk(mu);
    auto it = cache.find(path);
    if (it != cache.end() && it->second.first == stamp) return it->second.second;
    if (cache.size() > 64) cache.clear();
    LutPtr L = readLutFile(path);
    cache[path] = {stamp, L};
    return L;
}

// ------------------------------------------------------------------ LUT Filter
void lutFilter(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double gain = clampd(c.d("Gain"), 0, 1);
    const bool tetra = c.i("Interpolation") == 1;
    const LutPtr L = gain > 0 ? cachedLut(c.s("LUTFilename")) : nullptr;
    if (!L || W <= 0 || H <= 0) { copyFrame(c); return; }
    const float g = (float)gain;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, gg, b, a;
            unpremul(c.src.at(x, y), r, gg, b, a);
            float* o = c.out.at(x, y);
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            float v[3] = {r, gg, b};
            lutApply(*L, v, tetra);
            premul(o, r + (v[0] - r) * g, gg + (v[1] - gg) * g, b + (v[2] - b) * g, a);
        }
    });
}

// ------------------------------------------------------------------ AutoLooks
// One look as a simple grade, Reno's stand-in for VEGAS's table. Applied to a colour c in order: c = c x gain + lift; shadow and highlight tints (more
// shadow tint where dark, highlight tint where bright); contrast (S-curve mixed in); c^curve (above 1 darkens the middle); saturation about the luma;
// fade (blacks lifted).
struct Look {
    const char* name;
    float lift[3], gain[3], curve, contrast, sat, fade, shadow[3], high[3];
};
const Look kLooks[21] = {
    // the films the names allude to are a guess
    {"Conception", {-.01f, 0, .02f}, {.92f, .98f, 1.08f}, 1.0f, .25f, .80f, .02f, {-.02f, 0, .04f}, {-.01f, .01f, .03f}},
    {"Crime City", {0, 0, 0}, {1.10f, .95f, .95f}, 1.1f, .70f, .25f, 0, {0, 0, 0}, {.02f, 0, 0}},
    {"Grand Berlin Hotel", {.03f, 0, .02f}, {1.08f, .96f, .98f}, 1.0f, .10f, 1.10f, .04f, {.03f, 0, .02f}, {.03f, .01f, 0}},
    {"Hug Club", {0, 0, 0}, {.98f, 1.04f, .90f}, 1.1f, .35f, .60f, 0, {0, .02f, 0}, {.01f, .02f, -.02f}},
    {"Losing Private Brian", {0, 0, 0}, {.98f, 1.0f, 1.02f}, 1.0f, .50f, .40f, .01f, {0, 0, .01f}, {0, 0, 0}},
    {"Modfather", {0, 0, 0}, {1.12f, .98f, .78f}, 1.25f, .30f, .85f, 0, {.02f, .01f, -.01f}, {.04f, .02f, -.04f}},
    {"Nade Gunner", {0, 0, 0}, {1, 1, 1}, 1.1f, .35f, 1.10f, 0, {-.03f, .02f, .05f}, {.06f, .02f, -.05f}},
    {"Nemesis", {0, 0, 0}, {.88f, 1.08f, .90f}, 1.0f, .35f, .85f, 0, {-.02f, .03f, 0}, {-.01f, .02f, -.01f}},
    {"Pirates of the Atlantic", {0, 0, 0}, {1.08f, 1.02f, .88f}, 1.0f, .30f, 1.15f, 0, {-.01f, .02f, .03f}, {.03f, .01f, -.03f}},
    {"The Tomb", {0, 0, 0}, {.90f, .95f, 1.02f}, 1.3f, .35f, .75f, 0, {-.01f, .01f, .02f}, {0, 0, .01f}},
    {"The Wobbit", {0, 0, 0}, {1.04f, 1.06f, .90f}, 1.0f, .15f, 1.25f, 0, {0, .01f, 0}, {.03f, .02f, -.03f}},
    {"Winter is Coming", {0, 0, 0}, {.90f, .99f, 1.12f}, 1.0f, .25f, .55f, 0, {-.01f, 0, .03f}, {-.01f, .01f, .04f}},
    {"90s Music Video", {0, 0, 0}, {1.05f, 1.0f, 1.05f}, 1.0f, .40f, 1.50f, 0, {0, 0, .05f}, {.05f, .03f, -.03f}},
    {"Beer and Clothing in Las Vegas", {0, 0, 0}, {1.10f, 1.05f, .80f}, 1.0f, .20f, 1.30f, 0, {.02f, .01f, -.02f}, {.04f, .03f, -.04f}},
    {"Dan's Labyrinth", {0, 0, 0}, {.90f, 1.04f, .98f}, 1.2f, .30f, .80f, 0, {-.02f, .02f, .02f}, {0, .01f, 0}},
    {"Incarnation", {0, 0, 0}, {.92f, 1.04f, 1.10f}, 1.0f, .20f, 1.35f, 0, {-.01f, .01f, .03f}, {-.02f, .02f, .03f}},
    {"Lutney Lope", {0, 0, 0}, {1.02f, 1.0f, 1.03f}, 1.0f, -.10f, .85f, .08f, {0, 0, .02f}, {.02f, .01f, 0}},
    {"Muffelo 33", {0, 0, 0}, {1.05f, 1.0f, .90f}, 1.0f, .10f, .70f, .06f, {.01f, 0, -.01f}, {.03f, .02f, -.02f}},
    {"Rad Jack 2015", {0, 0, 0}, {1, 1, 1}, 1.0f, .45f, 1.40f, 0, {-.05f, 0, .08f}, {.08f, .03f, -.08f}},
    {"The Block", {0, 0, 0}, {.96f, .99f, 1.04f}, 1.0f, .20f, .50f, 0, {0, 0, .01f}, {0, 0, .01f}},
    {"The Book of Lie", {0, 0, 0}, {1.10f, 1.0f, .80f}, 1.0f, .50f, .35f, .02f, {.01f, 0, -.01f}, {.04f, .02f, -.04f}},
};

void lookApply(const Look& k, float c[3])
{
    for (int i = 0; i < 3; ++i) c[i] = c[i] * k.gain[i] + k.lift[i];
    const float l = std::min(1.f, std::max(0.f, luma(c[0], c[1], c[2])));
    const float ws = (1 - l) * (1 - l), wh = l * l;
    for (int i = 0; i < 3; ++i) c[i] += k.shadow[i] * ws + k.high[i] * wh;
    for (int i = 0; i < 3; ++i) {
        float v = std::min(1.f, std::max(0.f, c[i]));
        v = v + k.contrast * (v * v * (3 - 2 * v) - v);
        v = std::pow(v, k.curve);
        c[i] = v;
    }
    const float l2 = luma(c[0], c[1], c[2]);
    for (int i = 0; i < 3; ++i) {
        float v = l2 + (c[i] - l2) * k.sat;
        v = k.fade + v * (1 - k.fade);
        c[i] = std::min(1.f, std::max(0.f, v));
    }
}

LutPtr bakeLook(const Look& k)
{
    auto L = std::make_shared<Lut>();
    const int n = 33;
    L->n = n;
    L->rgb.resize((size_t)n * n * n * 3);
    size_t o = 0;
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float c[3] = {(float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1)};
                lookApply(k, c);
                L->rgb[o++] = c[0]; L->rgb[o++] = c[1]; L->rgb[o++] = c[2];
            }
    return L;
}

// Where VEGAS keeps its tables on this machine, if installed: the "AutoLooks" folder under the plug-in's Resources (the plug-in is Vfx1.ofx).
std::string autolooksFolder(const std::string& hint)
{
    std::vector<std::string> dirs;
    if (!hint.empty()) dirs.push_back(hint);
    if (const char* e = std::getenv("RENO_AUTOLOOKS_DIR")) dirs.push_back(e);
    dirs.push_back("C:/Program Files/VEGAS/VEGAS Pro 18.0/OFX Video Plug-Ins/Vfx1.ofx.bundle/Contents/Resources/AutoLooks");
    dirs.push_back("C:/Program Files/VEGAS/VEGAS Pro 18.0/OFX Video Plug-Ins/Vfx1.ofx.bundle/Contents/Win64/Resources/AutoLooks");
    dirs.push_back("C:/Program Files/VEGAS/VEGAS Pro 18.0/OFX Video Plug-Ins/Vfx1.ofx.bundle/Contents/Win64/AutoLooks");
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!d.empty() && std::filesystem::is_regular_file(fsPath(d + "/1.cube"), ec)) return d;
    }
    return std::string();
}

// The table for look 1..21: VEGAS's own file when found, else Reno's baked grade.
LutPtr autolooksTable(int look, const std::string& hint)
{
    static std::mutex mu;
    static LutPtr baked[21];
    std::lock_guard<std::mutex> lk(mu);
    const std::string dir = autolooksFolder(hint);
    if (!dir.empty()) {
        if (LutPtr L = cachedLut(dir + "/" + std::to_string(look) + ".cube")) return L;
    }
    if (!baked[look - 1]) baked[look - 1] = bakeLook(kLooks[look - 1]);
    return baked[look - 1];
}

void autoLooks(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const int look = c.i("autolooksLook");
    if (look < 1 || look > 21 || W <= 0 || H <= 0) { copyFrame(c); return; }
    const LutPtr L = autolooksTable(look, c.s("autolooksFilename"));
    if (!L) { copyFrame(c); return; }
    const float gain = (float)clampd(c.d("Gain"), 0, 1);          // VEGAS's "Strength"
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            float* o = c.out.at(x, y);
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            float v[3] = {r, g, b};
            lutApply(*L, v, true);
            premul(o, r + (v[0] - r) * gain, g + (v[1] - g) * gain, b + (v[2] - b) * gain, a);
        }
    });
}

// ------------------------------------------------------------------ Color Match
// sRGB <-> CIE Lab (D65), with the transfer curves done by tables.
struct SrgbTables {
    float dec[4097];
    float enc[4097];
    SrgbTables()
    {
        for (int i = 0; i <= 4096; ++i) {
            const double v = i / 4096.0;
            dec[i] = (float)(v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4));
            enc[i] = (float)(v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1 / 2.4) - 0.055);
        }
    }
};
const SrgbTables& srgbTables()
{
    static const SrgbTables t;
    return t;
}
inline float tab(const float* t, float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    const float p = v * 4096.f;
    const int i = std::min(4095, (int)p);
    return t[i] + (t[i + 1] - t[i]) * (p - (float)i);
}
inline float labF(float t) { return t > 0.008856f ? std::cbrt(t) : 7.787f * t + 16.f / 116.f; }
inline float labFinv(float t) { const float t3 = t * t * t; return t3 > 0.008856f ? t3 : (t - 16.f / 116.f) / 7.787f; }
void toLab(const SrgbTables& T, float r, float g, float b, float lab[3])
{
    const float lr = tab(T.dec, r), lg = tab(T.dec, g), lb = tab(T.dec, b);
    const float X = (0.4124564f * lr + 0.3575761f * lg + 0.1804375f * lb) / 0.95047f;
    const float Y = 0.2126729f * lr + 0.7151522f * lg + 0.0721750f * lb;
    const float Z = (0.0193339f * lr + 0.1191920f * lg + 0.9503041f * lb) / 1.08883f;
    const float fx = labF(X), fy = labF(Y), fz = labF(Z);
    lab[0] = 116.f * fy - 16.f;
    lab[1] = 500.f * (fx - fy);
    lab[2] = 200.f * (fy - fz);
}
void fromLab(const SrgbTables& T, const float lab[3], float rgb[3])
{
    const float fy = (lab[0] + 16.f) / 116.f, fx = fy + lab[1] / 500.f, fz = fy - lab[2] / 200.f;
    const float X = labFinv(fx) * 0.95047f, Y = labFinv(fy), Z = labFinv(fz) * 1.08883f;
    const float lr = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
    const float lg = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
    const float lb = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
    rgb[0] = tab(T.enc, lr); rgb[1] = tab(T.enc, lg); rgb[2] = tab(T.enc, lb);
}

// The statistics of "internaldata": 12 little-endian doubles, base64: mean L, std L, mean a, std a, mean b, std b for the source
// then the same for the target (all zero if the target was not captured).
bool readMatchStats(const std::string& text, double st[12])
{
    const std::string b = base64Decode(text);
    if (b.size() < 12 * sizeof(double)) return false;
    for (int i = 0; i < 12; ++i) {
        std::memcpy(&st[i], b.data() + (size_t)i * 8, 8);
        if (!std::isfinite(st[i])) return false;
    }
    return true;
}

void colorMatch(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    const double strength = clampd(c.d("Strength"), 0, 1);
    double st[12] = {0};
    if (W <= 0 || H <= 0 || strength <= 0 || !c.b("SourceCaptured") || !readMatchStats(c.s("internaldata"), st) || (st[1] <= 1e-6 && st[3] <= 1e-6 && st[5] <= 1e-6)) {
        copyFrame(c);
        return;
    }
    const SrgbTables& T = srgbTables();
    double tgt[6];
    if (c.b("TargetCaptured") && (st[7] > 1e-6 || st[9] > 1e-6 || st[11] > 1e-6)) {
        for (int i = 0; i < 6; ++i) tgt[i] = st[6 + i];
    } else {
        const int gx = 160, gy = 90;
        double sum[3] = {0, 0, 0}, sq[3] = {0, 0, 0};
        long cnt = 0;
        for (int j = 0; j < gy; ++j)
            for (int i = 0; i < gx; ++i) {
                const int x = std::min(W - 1, (int)((i + 0.5) * W / gx)), y = std::min(H - 1, (int)((j + 0.5) * H / gy));
                float r, g, b, a;
                unpremul(c.src.at(x, y), r, g, b, a);
                if (a <= 0.f) continue;
                float lab[3];
                toLab(T, r, g, b, lab);
                for (int k = 0; k < 3; ++k) { sum[k] += lab[k]; sq[k] += (double)lab[k] * lab[k]; }
                ++cnt;
            }
        if (cnt < 4) { copyFrame(c); return; }
        for (int k = 0; k < 3; ++k) {
            tgt[2 * k] = sum[k] / cnt;
            tgt[2 * k + 1] = std::sqrt(std::max(0.0, sq[k] / cnt - tgt[2 * k] * tgt[2 * k]));
        }
    }
    // Per channel: out = (x - targetMean) x scale + sourceMean. A flat target (a standard deviation of nothing) is only shifted.
    float mulv[3], tm[3], sm[3];
    for (int k = 0; k < 3; ++k) {
        const double ss = st[2 * k + 1], ts = tgt[2 * k + 1];
        mulv[k] = (float)(ts > 0.05 ? clampd(ss / ts, 0, 8) : 1.0);
        tm[k] = (float)tgt[2 * k];
        sm[k] = (float)st[2 * k];
    }
    const bool lightness = c.b("Processlightness");
    const float s = (float)strength;
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            float* o = c.out.at(x, y);
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            float lab[3];
            toLab(T, r, g, b, lab);
            for (int k = lightness ? 0 : 1; k < 3; ++k) lab[k] = (lab[k] - tm[k]) * mulv[k] + sm[k];
            lab[0] = std::min(100.f, std::max(0.f, lab[0]));
            float v[3];
            fromLab(T, lab, v);
            premul(o, r + (v[0] - r) * s, g + (v[1] - g) * s, b + (v[2] - b) * s, a);
        }
    });
}

// ------------------------------------------------------------------ Color Grading
const char* const kDefaultCurve = "2:0.0:0.0:-0.2:0.0:0.2:0.2:1.0:1.0:-0.2:-0.2:0.2:0.0";

// A curve as VEGAS writes it: "N:" then per point six numbers separated by ':' - x, y, the handle going back (dx, dy) and the one going forward (dx, dy),
// all 0..1. Between two points it is the cubic Bezier with those handles. Gives 1025 entries (output at inputs 0..1 in steps of 1/1024); false if the
// text is no curve or is a straight line.
bool curveTableOf(const std::string& text, float* lut)
{
    const char* s = text.c_str();
    char* e = nullptr;
    const long n = std::strtol(s, &e, 10);
    if (e == s || *e != ':' || n < 2 || n > 256) return false;
    struct P { double x, y, ix, iy, ox, oy; };
    std::vector<P> pts((size_t)n);
    s = e;
    for (long i = 0; i < n; ++i) {
        double v[6];
        for (int k = 0; k < 6; ++k) {
            if (*s != ':') return false;
            ++s;
            v[k] = std::strtod(s, &e);
            if (e == s || !std::isfinite(v[k])) return false;
            s = e;
        }
        pts[(size_t)i] = {v[0], v[1], v[2], v[3], v[4], v[5]};
    }
    // the polyline of the whole curve (x never going back), then the table read off it
    std::vector<std::pair<double, double>> poly;
    poly.push_back({pts[0].x, pts[0].y});
    const int S = 48;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const P &a = pts[i], &b = pts[i + 1];
        const double px[4] = {a.x, a.x + a.ox, b.x + b.ix, b.x}, py[4] = {a.y, a.y + a.oy, b.y + b.iy, b.y};
        for (int k = 1; k <= S; ++k) {
            const double u = (double)k / S, v = 1 - u;
            const double w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
            const double x = std::max(poly.back().first, w0 * px[0] + w1 * px[1] + w2 * px[2] + w3 * px[3]);
            poly.push_back({x, w0 * py[0] + w1 * py[1] + w2 * py[2] + w3 * py[3]});
        }
    }
    size_t j = 0;
    bool identity = true;
    for (int i = 0; i <= 1024; ++i) {
        const double x = i / 1024.0;
        double y;
        if (x <= poly.front().first) y = poly.front().second;
        else if (x >= poly.back().first) y = poly.back().second;
        else {
            while (j + 1 < poly.size() && poly[j + 1].first < x) ++j;
            const auto &p0 = poly[j], &p1 = poly[std::min(j + 1, poly.size() - 1)];
            y = p1.first > p0.first ? p0.second + (p1.second - p0.second) * (x - p0.first) / (p1.first - p0.first) : p1.second;
        }
        lut[i] = (float)clampd(y, 0.0, 1.0);
        if (std::fabs(lut[i] - x) > 2e-4) identity = false;
    }
    return !identity;
}

inline float lookupCurve(const float* lut, float v)
{
    v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
    const float p = v * 1024.f;
    const int i = std::min(1023, (int)p);
    return lut[i] + (lut[i + 1] - lut[i]) * (p - (float)i);
}

// BT.709 chroma (U, V) as a change of R, G, B.
inline void uvToRgb(double u, double v, double out[3])
{
    out[0] = 1.5748 * v;
    out[1] = -0.1873 * u - 0.4681 * v;
    out[2] = 1.8556 * u;
}

void rgbToHsv(float r, float g, float b, float& h, float& s, float& v)
{
    const float mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
    v = mx;
    s = mx > 1e-9f ? d / mx : 0.f;
    if (d < 1e-9f) { h = 0; return; }
    if (mx == r) h = (g - b) / d + (g < b ? 6.f : 0.f);
    else if (mx == g) h = (b - r) / d + 2.f;
    else h = (r - g) / d + 4.f;
    h *= 60.f;
}
void hsvToRgb(float h, float s, float v, float& r, float& g, float& b)
{
    h = std::fmod(h, 360.f);
    if (h < 0) h += 360.f;
    const float hh = h / 60.f;
    const int i = (int)hh;
    const float f = hh - (float)i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

const int kLutSlots = 2;

void colorGrading(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0) return;
    double lift[3], gamma[3], gain[3], off[3];
    {
        const char* ch[3] = {"R", "G", "B"};
        const struct { const char* n; double* v; } groups[4] = {{"Lift_", lift}, {"Gamma_", gamma}, {"Gain_", gain}, {"Offset_", off}};
        for (int gi = 0; gi < 4; ++gi) {
            const std::string base = groups[gi].n;
            const double y = c.d((base + "Y").c_str()), u = c.d((base + "U").c_str()), v = c.d((base + "V").c_str());
            double uv[3];
            uvToRgb(u, v, uv);
            const bool mult = gi == 1 || gi == 2;
            for (int k = 0; k < 3; ++k) {
                const double rgbv = c.d((base + ch[k]).c_str());
                groups[gi].v[k] = mult ? rgbv * y + uv[k] : rgbv + y + uv[k];
            }
        }
    }
    const double expo = std::exp2(c.d("ExposureAdjust")) * std::exp(c.d("ExposureLnAdjust"));
    const double inMin = c.d("InMinAdjust"), inMax = c.d("InMaxAdjust"), outMin = c.d("OutMinAdjust"), outMax = c.d("OutMaxAdjust");
    const double sat = c.d("SaturationAdjust"), hueShift = c.d("HueAdjust"), hsvSat = c.d("SatAdjust"), hsvVal = c.d("ValAdjust");
    float curves[4][1025];
    bool hasCurve[4];
    const char* curveNames[4] = {"Curve_R", "Curve_G", "Curve_B", "Curve_Y"};
    for (int k = 0; k < 4; ++k) hasCurve[k] = curveTableOf(c.s(curveNames[k]), curves[k]);
    LutPtr luts[kLutSlots];
    float lutGain[kLutSlots] = {0, 0};
    bool lutTetra[kLutSlots] = {false, false};
    for (int i = 0; i < kLutSlots; ++i) {
        const std::string sx = "_" + std::to_string(i);
        if (!c.b(("LUTEnable" + sx).c_str())) continue;
        lutGain[i] = (float)clampd(c.d(("LUTGain" + sx).c_str()), 0, 1);
        lutTetra[i] = c.i(("LUTInterpolation" + sx).c_str()) == 1;
        if (lutGain[i] > 0) luts[i] = cachedLut(c.s(("LUTFilename" + sx).c_str()));
    }
    const double inSpan = inMax - inMin;
    const float inScale = (float)(std::fabs(inSpan) > 1e-6 ? 1.0 / inSpan : 1.0);
    const bool levelsIn = std::fabs(inMin) > 1e-9 || std::fabs(inMax - 1) > 1e-9, levelsOut = std::fabs(outMin) > 1e-9 || std::fabs(outMax - 1) > 1e-9;
    const bool wheels = [&] {
        for (int k = 0; k < 3; ++k) if (lift[k] != 0 || gamma[k] != 1 || gain[k] != 1 || off[k] != 0) return true;
        return false;
    }();
    const bool hsv = std::fabs(hueShift) > 1e-9 || std::fabs(hsvSat - 1) > 1e-9 || std::fabs(hsvVal - 1) > 1e-9;
    const bool anyCurve = hasCurve[0] || hasCurve[1] || hasCurve[2] || hasCurve[3];
    const bool anyLut = (luts[0] && lutGain[0] > 0) || (luts[1] && lutGain[1] > 0);
    if (!levelsIn && !levelsOut && !wheels && !hsv && !anyCurve && !anyLut && std::fabs(expo - 1) < 1e-9 && std::fabs(sat - 1) < 1e-9) { copyFrame(c); return; }
    float invGamma[3];
    for (int k = 0; k < 3; ++k) invGamma[k] = (float)(1.0 / std::max(0.01, gamma[k]));
    forRows(H, W, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float r, g, b, a;
            unpremul(c.src.at(x, y), r, g, b, a);
            float* o = c.out.at(x, y);
            if (a <= 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            float v[3] = {r, g, b};
            if (levelsIn) for (int k = 0; k < 3; ++k) v[k] = (v[k] - (float)inMin) * inScale;
            if (expo != 1.0) for (int k = 0; k < 3; ++k) v[k] *= (float)expo;
            if (wheels) {
                for (int k = 0; k < 3; ++k) {
                    float t = (float)gain[k] * (v[k] + (float)lift[k] * (1.f - v[k]));
                    t = t >= 0.f ? std::pow(t, invGamma[k]) : -std::pow(-t, invGamma[k]);
                    v[k] = t + (float)off[k];
                }
            }
            if (sat != 1.0) {
                const float l = luma(v[0], v[1], v[2]);
                for (int k = 0; k < 3; ++k) v[k] = l + (v[k] - l) * (float)sat;
            }
            if (hsv) {
                float h, s, vv;
                rgbToHsv(std::min(1.f, std::max(0.f, v[0])), std::min(1.f, std::max(0.f, v[1])), std::min(1.f, std::max(0.f, v[2])), h, s, vv);
                h += (float)hueShift;
                s = std::min(1.f, std::max(0.f, s * (float)hsvSat));
                vv = std::max(0.f, vv * (float)hsvVal);
                hsvToRgb(h, s, vv, v[0], v[1], v[2]);
            }
            if (anyCurve) {
                for (int k = 0; k < 3; ++k) {
                    if (hasCurve[k]) v[k] = lookupCurve(curves[k], v[k]);
                    if (hasCurve[3]) v[k] = lookupCurve(curves[3], v[k]);
                }
            }
            if (levelsOut) for (int k = 0; k < 3; ++k) v[k] = (float)outMin + v[k] * (float)(outMax - outMin);
            for (int i = 0; i < kLutSlots; ++i) {
                if (!luts[i] || lutGain[i] <= 0) continue;
                float w[3] = {v[0], v[1], v[2]};
                lutApply(*luts[i], w, lutTetra[i]);
                for (int k = 0; k < 3; ++k) v[k] += (w[k] - v[k]) * lutGain[i];
            }
            premul(o, v[0], v[1], v[2], a);
        }
    });
}

}  // namespace

void addFiltersColor(std::vector<Def>& out)
{
    {
        std::vector<PDef> ps = {text("LUTName", "Name", "", false), text("LUTFilename", "File", "", false), choice("Interpolation", "Interpolation", 0, {"Linear (good)", "Tetrahedral (best)"}),
                                dbl("Gain", "Strength", 1, 0, 1)};
        ps[3].hint = "How much of the LUT's result is mixed with the original: 0 is no LUT.";
        out.push_back(make("com.vegascreativesoftware:lutfilter", "LUT Filter", "A colour look-up table (.cube) applied to the picture", ps, lutFilter));
    }
    {
        std::vector<std::string> looks = {"none"};
        for (const Look& l : kLooks) looks.push_back(l.name);
        std::vector<PDef> ps = {choice("autolooksLook", "Look", 0, looks), dbl("Gain", "Strength", 1, 0, 1), text("autolooksFilename", "Tables folder", "", false)};
        ps[2].secret = true;
        Def d = make("com.vegascreativesoftware:autolooks", "AutoLooks", "One of 21 film looks (VEGAS's own tables when its folder is found, else grades made up by Reno that only suggest them)", ps, autoLooks);
        out.push_back(d);
    }
    {
        std::vector<PDef> ps = {dbl("Strength", "Strength", 1, 0, 1), boolean("Processlightness", "Match brightness", false), imageParam("ImageSource", "Source Image"),
                                imageParam("ImageTarget", "Target Image"), imageParam("ImageResult", "Result Image"), boolean("SourceCaptured", "Source Captured", false),
                                boolean("TargetCaptured", "Target Captured", false), custom("internaldata", "Statistics", "")};
        ps[5].secret = true;
        ps[6].secret = true;
        // the three pictures are VEGAS's OfxParamTypeImage settings: they hold no value the host can read, so the picture's colours are kept in
        // "internaldata" (the captured statistics)
        out.push_back(make("com.vegascreativesoftware:colormatch", "Color Match", "The clip's colours moved towards those of a reference picture (the Lab statistics VEGAS captured)", ps, colorMatch));
    }
    {
        std::vector<PDef> ps;
        const char* curveLabels[4] = {"Curve R", "Curve G", "Curve B", "Curve Y"};
        const char* curveNames[4] = {"Curve_R", "Curve_G", "Curve_B", "Curve_Y"};
        for (int k = 0; k < 4; ++k) ps.push_back(custom(curveNames[k], curveLabels[k], kDefaultCurve));
        const char* chans[6] = {"R", "G", "B", "Y", "U", "V"};
        struct G { const char* n; double def; double lo, hi; } groups[4] = {{"Lift", 0, -1, 1}, {"Gamma", 1, 0.01, 4}, {"Gain", 1, 0, 4}, {"Offset", 0, -1, 1}};
        for (const G& g : groups) {
            for (int k = 0; k < 6; ++k) {
                const bool master = k >= 3;
                const bool factor = (g.def == 1);
                const double def = (k == 3 && factor) ? 1.0 : (k < 3 ? g.def : 0.0);
                PDef p = dbl(std::string(g.n) + "_" + chans[k], std::string(g.n) + " " + chans[k], def, master && k > 3 ? -1 : g.lo, master && k > 3 ? 1 : g.hi);
                ps.push_back(p);
            }
        }
        ps.push_back(dbl("SaturationAdjust", "Saturation", 1, 0, 4));
        ps.push_back(dbl("ExposureAdjust", "Exposure +/-", 0, -10, 10));
        ps.push_back(dbl("ExposureLnAdjust", "Exposure (ln)", 0, -10, 10));
        ps.push_back(dbl("InMinAdjust", "Input Min", 0, -1, 1));
        ps.push_back(dbl("InMaxAdjust", "Input Max", 1, 0, 2));
        ps.push_back(dbl("OutMinAdjust", "Output Min", 0, -1, 1));
        ps.push_back(dbl("OutMaxAdjust", "Output Max", 1, 0, 2));
        ps.push_back(dbl("HueAdjust", "Hue", 0, -360, 360));
        ps.push_back(dbl("SatAdjust", "Sat", 1, 0, 4));
        ps.push_back(dbl("ValAdjust", "Val", 1, 0, 4));
        for (int i = 0; i < kLutSlots; ++i) {
            const std::string sx = "_" + std::to_string(i);
            ps.push_back(boolean("LUTEnable" + sx, "LUT enable", false));
            ps.push_back(intp("LUTInterpolation" + sx, "LUT interpolation", 0, 0, 1));
            ps.push_back(dbl("LUTGain" + sx, "LUT strength", 1, 0, 1));
            ps.push_back(text("LUTFilename" + sx, "LUT file", "", false));
        }
        ps.push_back(boolean("AutoAdjust", "Auto adjust", false));
        ps.push_back(intp("CSpaceVersion", "Colour space version", 0, 0, 1000));
        ps.push_back(text("CSpaceName", "Colour space", "", false));
        for (auto& p : ps)
            if (p.type == PT::Custom || p.name == "AutoAdjust" || p.name == "CSpaceVersion" || p.name == "CSpaceName" || p.name.compare(0, 3, "LUT") == 0 || p.name == "ExposureLnAdjust") p.secret = true;
        out.push_back(make("com.vegascreativesoftware:colorgrading", "Color Grading", "Lift, gamma, gain and offset wheels, curves, levels and LUTs of the Color Grading panel", ps, colorGrading));
    }
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
