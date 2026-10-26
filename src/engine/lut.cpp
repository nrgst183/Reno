// 3D colour LUTs for the Color Grading panel (VEGAS Pro 23's Camera LUT and Look LUT): .cube files and Reno's own
// presets. See builtinfx.h.
#include "builtinfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

namespace reno {

// ------------------------------------------------------------------------------------------ applying a table
void Lut3D::apply(float c[3], bool tetra) const
{
    if (n < 2 || rgb.size() < (size_t)n * n * n * 3) return;
    float p[3];
    for (int k = 0; k < 3; ++k) {
        const double span = domainMax[k] - domainMin[k];
        const double v = span > 1e-12 ? (c[k] - domainMin[k]) / span : 0.0;
        p[k] = (float)(std::min(1.0, std::max(0.0, v)) * (n - 1));
    }
    const int i0[3] = {std::min(n - 2, (int)p[0]), std::min(n - 2, (int)p[1]), std::min(n - 2, (int)p[2])};
    const float f[3] = {p[0] - (float)i0[0], p[1] - (float)i0[1], p[2] - (float)i0[2]};
    auto at = [&](int r, int g, int b, int k) { return rgb[(((size_t)(i0[2] + b) * n + (size_t)(i0[1] + g)) * n + (size_t)(i0[0] + r)) * 3 + (size_t)k]; };
    for (int k = 0; k < 3; ++k) {
        float out;
        if (!tetra) {
            const float c00 = at(0, 0, 0, k) * (1 - f[0]) + at(1, 0, 0, k) * f[0], c10 = at(0, 1, 0, k) * (1 - f[0]) + at(1, 1, 0, k) * f[0];
            const float c01 = at(0, 0, 1, k) * (1 - f[0]) + at(1, 0, 1, k) * f[0], c11 = at(0, 1, 1, k) * (1 - f[0]) + at(1, 1, 1, k) * f[0];
            const float c0 = c00 * (1 - f[1]) + c10 * f[1], c1 = c01 * (1 - f[1]) + c11 * f[1];
            out = c0 * (1 - f[2]) + c1 * f[2];
        } else {
            // tetrahedral: the cube is cut into six tetrahedra by the order of the fractions
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

// ------------------------------------------------------------------------------------------ .cube files
std::shared_ptr<const Lut3D> readCubeLut(const std::string& path, std::string* error)
{
    static std::mutex mu;
    static std::map<std::string, std::pair<long long, std::shared_ptr<const Lut3D>>> cache;
    long long stamp = 0;
    try {
        stamp = (long long)std::filesystem::file_size(path) * 1000003LL + (long long)std::filesystem::last_write_time(path).time_since_epoch().count() % 1000003LL;
    } catch (...) {
        if (error) *error = "The LUT file could not be opened.";
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(path);
        if (it != cache.end() && it->second.first == stamp) return it->second.second;
    }
    std::ifstream in(path);
    if (!in) {
        if (error) *error = "The LUT file could not be opened.";
        return nullptr;
    }
    int size3 = 0, size1 = 0;
    double dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
    std::vector<float> values;
    std::string line;
    while (std::getline(in, line)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        std::istringstream ls(line);
        std::string word;
        if (!(ls >> word)) continue;
        if (word == "LUT_3D_SIZE") ls >> size3;
        else if (word == "LUT_1D_SIZE") ls >> size1;
        else if (word == "DOMAIN_MIN") ls >> dmin[0] >> dmin[1] >> dmin[2];
        else if (word == "DOMAIN_MAX") ls >> dmax[0] >> dmax[1] >> dmax[2];
        else if (word == "TITLE" || word == "LUT_1D_INPUT_RANGE" || word == "LUT_3D_INPUT_RANGE") continue;
        else {
            char* end = nullptr;
            const double r = std::strtod(word.c_str(), &end);
            if (end == word.c_str() || *end) continue;
            double g = 0, b = 0;
            if (!(ls >> g >> b)) continue;
            values.push_back((float)r);
            values.push_back((float)g);
            values.push_back((float)b);
        }
    }
    auto lut = std::make_shared<Lut3D>();
    for (int k = 0; k < 3; ++k) { lut->domainMin[k] = dmin[k]; lut->domainMax[k] = dmax[k]; }
    if (size3 >= 2 && size3 <= 129 && values.size() >= (size_t)size3 * size3 * size3 * 3) {
        lut->n = size3;
        lut->rgb.assign(values.begin(), values.begin() + (std::ptrdiff_t)size3 * size3 * size3 * 3);
    } else if (size1 >= 2 && size1 <= 65536 && values.size() >= (size_t)size1 * 3) {
        // a 1D table: each channel through its own curve
        const int n = 33;
        lut->n = n;
        lut->rgb.resize((size_t)n * n * n * 3);
        auto curve = [&](int k, double x) {
            const double f = std::min(1.0, std::max(0.0, x)) * (size1 - 1);
            const int i = std::min(size1 - 2, (int)f);
            const double t = f - i;
            return values[(size_t)i * 3 + (size_t)k] * (1 - t) + values[(size_t)(i + 1) * 3 + (size_t)k] * t;
        };
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    float* o = &lut->rgb[(((size_t)b * n + (size_t)g) * n + (size_t)r) * 3];
                    o[0] = (float)curve(0, (double)r / (n - 1));
                    o[1] = (float)curve(1, (double)g / (n - 1));
                    o[2] = (float)curve(2, (double)b / (n - 1));
                }
    } else {
        if (error) *error = "This is not a .cube LUT file (no LUT_3D_SIZE table that fits).";
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(mu);
        if (cache.size() > 16) cache.clear();
        cache[path] = std::make_pair(stamp, std::shared_ptr<const Lut3D>(lut));
    }
    return lut;
}

// ------------------------------------------------------------------------------------------ Reno's presets
namespace {

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// BT.709's camera curve (scene-linear to the signal)
float oetf709(float l)
{
    if (l <= 0.f) return 0.f;
    return l < 0.018f ? 4.5f * l : 1.099f * std::pow(l, 0.45f) - 0.099f;
}

// Apple Log's published curve (Apple Log Profile white paper): signal to scene-linear, in Rec. 2020 primaries
float appleLogToLinear(float p)
{
    const float R0 = -0.05641088f, Rt = 0.01f, c = 47.28711236f, beta = 0.00964052f, gamma = 0.08550479f, delta = 0.69336945f;
    const float Pt = c * (Rt - R0) * (Rt - R0);
    return p >= Pt ? std::sqrt(p / c) + R0 : std::exp2((p - delta) / gamma) - beta;
}
// Rec. 2020 primaries to Rec. 709
void rec2020To709(float c[3])
{
    const float r = 1.6605f * c[0] - 0.5876f * c[1] - 0.0728f * c[2];
    const float g = -0.1246f * c[0] + 1.1329f * c[1] - 0.0083f * c[2];
    const float b = -0.0182f * c[0] - 0.1006f * c[1] + 1.1187f * c[2];
    c[0] = r; c[1] = g; c[2] = b;
}
void appleLogLinear(float c[3])
{
    for (int k = 0; k < 3; ++k) c[k] = appleLogToLinear(c[k]);
    rec2020To709(c);
    for (int k = 0; k < 3; ++k) c[k] = clampf(c[k], 0.f, 4.f);
}
void appleLog709(float c[3])
{
    appleLogLinear(c);
    for (int k = 0; k < 3; ++k) c[k] = clampf(oetf709(c[k]), 0.f, 1.f);
}

// Samsung does not publish its log curve; this is a generic log with mid grey.
float genericLogToLinear(float p) { return 0.18f * std::exp2((p - 0.42f) * 14.f) - 0.0006f; }
void samsungLogLinear(float c[3]) { for (int k = 0; k < 3; ++k) c[k] = clampf(genericLogToLinear(c[k]), 0.f, 4.f); }
void samsungLog709(float c[3]) { samsungLogLinear(c); for (int k = 0; k < 3; ++k) c[k] = clampf(oetf709(c[k]), 0.f, 1.f); }

// Sony S-Log3, Panasonic V-Log and ARRI LogC (EI 800): the published curves only (no change of primaries)
float sLog3ToLinear(float x)
{
    return x >= 171.2102946929f / 1023.f ? std::pow(10.f, (x * 1023.f - 420.f) / 261.5f) * 0.19f - 0.01f : (x * 1023.f - 95.f) * 0.01125f / (171.2102946929f - 95.f);
}
float vLogToLinear(float x) { return x < 0.181f ? (x - 0.125f) / 5.6f : std::pow(10.f, (x - 0.598206f) / 0.241514f) - 0.00873f; }
float logC3ToLinear(float x)
{
    const float cut = 0.149658f, a = 5.555556f, b = 0.052272f, c = 0.24719f, d = 0.385537f, e = 5.367655f, f = 0.092809f;
    return x > e * cut + f ? (std::pow(10.f, (x - d) / c) - b) / a : (x - f) / e;
}
void sLog3Rec709(float c[3]) { for (int k = 0; k < 3; ++k) c[k] = clampf(oetf709(clampf(sLog3ToLinear(c[k]), 0.f, 8.f)), 0.f, 1.f); }
void vLogRec709(float c[3]) { for (int k = 0; k < 3; ++k) c[k] = clampf(oetf709(clampf(vLogToLinear(c[k]), 0.f, 8.f)), 0.f, 1.f); }
void logCRec709(float c[3]) { for (int k = 0; k < 3; ++k) c[k] = clampf(oetf709(clampf(logC3ToLinear(c[k]), 0.f, 8.f)), 0.f, 1.f); }

float luma709(const float c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }
void mixGrey(float c[3], float amount)
{
    const float y = luma709(c);
    for (int k = 0; k < 3; ++k) c[k] = y + (c[k] - y) * amount;
}
float sCurve(float v, float k) { v = clampf(v, 0.f, 1.f); return v + k * (v * v * (3.f - 2.f * v) - v); }

void lookWarmFilm(float c[3])
{
    for (int k = 0; k < 3; ++k) c[k] = sCurve(c[k], 0.5f);
    c[0] = clampf(c[0] * 1.06f + 0.015f, 0.f, 1.f);
    c[2] = clampf(c[2] * 0.9f, 0.f, 1.f);
    mixGrey(c, 0.92f);
}
void lookCoolBlue(float c[3])
{
    for (int k = 0; k < 3; ++k) c[k] = sCurve(c[k], 0.3f);
    c[0] = clampf(c[0] * 0.92f, 0.f, 1.f);
    c[2] = clampf(c[2] * 1.08f + 0.01f, 0.f, 1.f);
}
void lookTealOrange(float c[3])
{
    const float y = luma709(c);
    const float sh = 1.f - clampf(y * 1.6f, 0.f, 1.f), hi = clampf((y - 0.35f) * 1.6f, 0.f, 1.f);
    c[0] = clampf(c[0] + 0.10f * hi - 0.08f * sh, 0.f, 1.f);
    c[1] = clampf(c[1] + 0.02f * sh, 0.f, 1.f);
    c[2] = clampf(c[2] + 0.10f * sh - 0.08f * hi, 0.f, 1.f);
    for (int k = 0; k < 3; ++k) c[k] = sCurve(c[k], 0.3f);
}
void lookBleachBypass(float c[3])
{
    const float y = luma709(c);
    for (int k = 0; k < 3; ++k) {
        const float o = c[k] < 0.5f ? 2.f * y * c[k] : 1.f - 2.f * (1.f - y) * (1.f - c[k]);
        c[k] = clampf(c[k] * 0.4f + o * 0.6f, 0.f, 1.f);
    }
    mixGrey(c, 0.6f);
}
void lookFaded(float c[3])
{
    for (int k = 0; k < 3; ++k) c[k] = clampf(0.08f + c[k] * 0.84f, 0.f, 1.f);
    mixGrey(c, 0.85f);
}
void lookBlackAndWhite(float c[3])
{
    const float y = sCurve(luma709(c), 0.6f);
    c[0] = c[1] = c[2] = y;
}
void lookSepia(float c[3])
{
    const float y = luma709(c);
    c[0] = clampf(y * 1.07f + 0.04f, 0.f, 1.f);
    c[1] = clampf(y * 0.95f + 0.01f, 0.f, 1.f);
    c[2] = clampf(y * 0.74f, 0.f, 1.f);
}
void lookCrossProcess(float c[3])
{
    c[0] = clampf(sCurve(c[0], 0.9f) * 1.05f, 0.f, 1.f);
    c[1] = clampf(sCurve(c[1], 0.3f), 0.f, 1.f);
    c[2] = clampf(0.1f + c[2] * 0.7f + 0.2f * (1.f - c[2]) * c[2], 0.f, 1.f);
}
void lookVivid(float c[3])
{
    mixGrey(c, 1.45f);
    for (int k = 0; k < 3; ++k) c[k] = sCurve(clampf(c[k], 0.f, 1.f), 0.35f);
}
void lookNoir(float c[3])
{
    const float y = sCurve(luma709(c), 0.9f);
    c[0] = clampf(y * 0.97f, 0.f, 1.f);
    c[1] = clampf(y * 0.99f, 0.f, 1.f);
    c[2] = clampf(y * 1.04f, 0.f, 1.f);
}
void lookDayForNight(float c[3])
{
    for (int k = 0; k < 3; ++k) c[k] *= 0.55f;
    const float y = luma709(c);
    c[0] = clampf(y * 0.7f + c[0] * 0.3f, 0.f, 1.f);
    c[1] = clampf(y * 0.85f + c[1] * 0.2f + 0.01f, 0.f, 1.f);
    c[2] = clampf(y * 1.1f + c[2] * 0.4f + 0.04f, 0.f, 1.f);
}

}  // namespace

const std::vector<LutPreset>& lutPresets()
{
    static const std::vector<LutPreset> list = {
        {"apple-log-709", "Apple Log to Rec.709", true, appleLog709},
        {"apple-log-linear", "Apple Log to Linear", true, appleLogLinear},
        {"samsung-log-709", "Samsung Log to Rec.709", true, samsungLog709},
        {"samsung-log-linear", "Samsung Log to Linear", true, samsungLogLinear},
        {"slog3-709", "Sony S-Log3 to Rec.709", true, sLog3Rec709},
        {"vlog-709", "Panasonic V-Log to Rec.709", true, vLogRec709},
        {"logc-709", "ARRI LogC to Rec.709", true, logCRec709},
        {"look-warm-film", "Warm Film", false, lookWarmFilm},
        {"look-cool-blue", "Cool Blue", false, lookCoolBlue},
        {"look-teal-orange", "Teal and Orange", false, lookTealOrange},
        {"look-bleach-bypass", "Bleach Bypass", false, lookBleachBypass},
        {"look-faded", "Faded Matte", false, lookFaded},
        {"look-bw", "Black and White", false, lookBlackAndWhite},
        {"look-sepia", "Sepia", false, lookSepia},
        {"look-cross-process", "Cross Process", false, lookCrossProcess},
        {"look-vivid", "Vivid", false, lookVivid},
        {"look-noir", "Noir", false, lookNoir},
        {"look-day-for-night", "Day for Night", false, lookDayForNight},
    };
    return list;
}

std::shared_ptr<const Lut3D> lutOfPreset(const std::string& id, int n)
{
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<const Lut3D>> cache;
    n = std::min(65, std::max(2, n));
    const std::string key = id + "#" + std::to_string(n);
    {
        std::lock_guard<std::mutex> lk(mu);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    const LutPreset* pr = nullptr;
    for (const LutPreset& p : lutPresets())
        if (p.id == id) pr = &p;
    if (!pr) return nullptr;
    auto lut = std::make_shared<Lut3D>();
    lut->n = n;
    lut->rgb.resize((size_t)n * n * n * 3);
    for (int b = 0; b < n; ++b)
        for (int g = 0; g < n; ++g)
            for (int r = 0; r < n; ++r) {
                float c[3] = {(float)r / (n - 1), (float)g / (n - 1), (float)b / (n - 1)};
                pr->fn(c);
                float* o = &lut->rgb[(((size_t)b * n + (size_t)g) * n + (size_t)r) * 3];
                for (int k = 0; k < 3; ++k) o[k] = std::isfinite(c[k]) ? c[k] : 0.f;
            }
    std::lock_guard<std::mutex> lk(mu);
    cache[key] = lut;
    return lut;
}

}  // namespace reno
