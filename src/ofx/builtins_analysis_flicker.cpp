// Built-in version of VEGAS Pro 18's Flicker Control (de.magix:FlickerReducer, MagixCVFx.ofx): removes flicker (brightness that jumps from frame to frame:
// mains lamps, a camera's auto exposure) or adds it. Parameters as the plug-in declares them: Mode (Reduction / Addition), Frequency (Addition), Model (Temporal blend /
// Histogram smoothing), TimeWindow (frames), Sensitivity, Threshold, Type (Highlights, Midtones, Shadow, All), UseMotion and ShowMask; the corners and histogram settings
// are what VEGAS keeps of its own analysis ("Calculate") and are kept as they are.
//
// What VEGAS's plug-in does (read from its code, checked against its output for every setting of the reference captures):
//  - Temporal blend: the output is a weighted mean of the TimeWindow + 1 frames from time - TimeWindow / 2 to time + (TimeWindow - TimeWindow / 2). The weights are made as
//    its code makes them (windowWeights()): binomial-like numbers (from whole-number divisions, in float) blended with a flat mean by Sensitivity - and in float, that blend
//    cancels to nothing for the middle frames of a long window, which is why a window of 60 frames shows only 4 frames at each end. A frame outside the clip is left out and
//    its weight is lost (the picture darkens). Type 'All' takes the weights as they are; the other Types weight every other frame at each pixel by how much that pixel is of
//    the tone (highlights 1 - (1 - v)^2, midtones 1 - 4 (v - .5)^2, shadows 1 - v^2, v the largest channel) and divide by the sum, so only the matching tones are blended.
//    UseMotion samples the other frames at a position from the Motion Tracking corners (without them: the corner pixel of each frame).
//  - Addition: every channel gets sin(2 pi Frequency time / fps) * Sensitivity * 0.1 * 255 levels added (more where the tone is the chosen one; a frequency of 1 or less is
//    VEGAS's random 'celluloid' flicker). Pixels with no alpha are left empty.
//  - Show mask: 5 x the largest channel difference between the result and the source, as grey.
//  - Nothing is done (the source comes through) when Sensitivity is 0, in Reduction when TimeWindow is 0, and for Histogram smoothing until Calculate has been pressed.
//  - Histogram smoothing (after Calculate): Reno's own method (VEGAS's analysis is stored in the project, which is not read): the frame's brightness histogram is matched
//    to the average of the histograms round it.
// Neighbouring frames' brightness (for the histogram model) is cached per instance (one more frame each time while playing).
#include "builtins_analysis_internal.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {
namespace {

struct FrameStat {
    Gray tiny;                      // 64 wide
    Signature sig;
    std::vector<float> hist;
};

struct FlickerState {
    std::mutex mu;
    std::map<long, std::shared_ptr<FrameStat>> frames;
    bool calculated = false;        // Calculate has been pressed (the histogram model does nothing before)
};

std::mutex gMutex;
std::map<const void*, std::shared_ptr<FlickerState>>& states()
{
    static std::map<const void*, std::shared_ptr<FlickerState>> m;
    return m;
}
std::shared_ptr<FlickerState> stateFor(const void* inst)
{
    std::lock_guard<std::mutex> lk(gMutex);
    auto& m = states();
    auto it = m.find(inst);
    if (it != m.end()) return it->second;
    if (m.size() > 64) m.erase(m.begin());
    auto s = std::make_shared<FlickerState>();
    m[inst] = s;
    return s;
}
void destroyState(const void* inst)
{
    std::lock_guard<std::mutex> lk(gMutex);
    states().erase(inst);
}

// How much a tone takes part: Type 0 highlights, 1 midtones, 2 shadows, 3 all.
inline float toneWeight(int type, float y)
{
    switch (type) {
    case 0: return (float)smooth01((y - 0.4) / 0.4);
    case 1: { const float d = (y - 0.5f) / 0.27f; return std::exp(-0.5f * d * d); }
    case 2: return 1.f - (float)smooth01((y - 0.15) / 0.45);
    default: return 1.f;
    }
}

std::shared_ptr<FrameStat> makeStat(const Buf& b)
{
    auto s = std::make_shared<FrameStat>();
    s->tiny = makeGray(b, 64);
    s->sig = signatureOf(s->tiny);
    s->hist.assign(256, 0.f);
    const size_t n = (size_t)b.w * b.h;
    if (n) {
        // (every 2nd pixel of every 2nd row on a big picture: a histogram does not need them all)
        const int step = (b.w * (size_t)b.h > 700000) ? 2 : 1;
        double cnt = 0;
        for (int y = 0; y < b.h; y += step) {
            const float* p = b.at(0, y);
            for (int x = 0; x < b.w; x += step, p += 4 * step) {
                const float l = luma(p[0], p[1], p[2]);
                const int bin = l <= 0.f ? 0 : (l >= 1.f ? 255 : (int)(l * 255.f + 0.5f));
                s->hist[(size_t)bin] += 1.f;
                cnt += 1;
            }
        }
        if (cnt > 0) for (float& h : s->hist) h = (float)(h / cnt);
    }
    return s;
}

// ------------------------------------------------------------------------------------------------ VEGAS's weights over the frames of the window
// What the plug-in's window function does, in float and in the same order: 'tw' + 1 weights for the frames time - tw / 2 ... time + (tw - tw / 2); the frame itself is
// index 'before'. The first loop makes numbers like binomial coefficients (each factor a whole-number division, as the plug-in does it), the second blends them with the flat
// mean by the sensitivity (below one half: pulls them toward the middle frame), then they are divided by their sum.
void windowWeights(int tw, float sens, int& before, int& after, std::vector<float>& w)
{
    before = tw / 2;
    after = tw - before;
    const int n = tw + 1;
    w.assign((size_t)n, 0.f);
    const float flat = 1.0f / ((float)tw + 1.0f);
    if (n == 2) {
        w[0] = 0.666f;
        w[1] = 0.333f;
    } else {
        for (int i = 0; i < n; ++i) {
            const int e = (2 * i <= tw) ? i : tw - i;
            float v = 1.0f;
            for (int k = 1; k <= e; ++k) v *= (float)((tw - e + k) / k);
            w[(size_t)i] = v;
        }
    }
    float sum = 0.f;
    for (int i = 0; i < n; ++i) {
        float a, b;
        const float x = w[(size_t)i];
        if (0.5f <= sens) {
            const float d = flat - (x + x);
            a = d;
            b = flat - d;
        } else {
            a = (i == before) ? x - 2.0f : x - 0.0f;
            b = x - a * 0.5f;
        }
        const float r = a * sens + b;
        w[(size_t)i] = r;
        sum += r;
    }
    if (sum != 0.f)
        for (int i = 0; i < n; ++i) w[(size_t)i] = w[(size_t)i] / sum;
}

// A level (0 .. 255 as a float) made a whole one as the plug-in does: below 0 is 0, above 254.5 is 255, else rounded (halves up); not a number is 0.
inline float toLevel(float x)
{
    if (!(x == x)) return 0.f;
    if (0.f > x) return 0.f;
    const float v = std::min(254.5f, x);
    return (float)(int)(v + 0.5f);
}

// How much a pixel of the tone takes part (the plug-in's tone curves); m is the largest channel, 0 .. 1; 'type' 0 highlights, 1 midtones, 2 shadows (3: all, 1).
inline float toneFactor(int type, float m)
{
    if (type == 2) return 1.0f - m * m;
    if (type == 1) { const float d = m - 0.5f; return 1.0f - (d * d) * 4.0f; }
    if (type == 0) { const float t = 1.0f - m; return 1.0f - t * t; }
    return 1.0f;
}

// What the frame of a window is kept as while the pixels are worked out: whole levels (8-bit pictures) or floats.
struct WFrame {
    std::vector<uint8_t> b;
    std::vector<float> f;
    float w = 0.f;
};

// Whether a frame time is one the clip has (the plug-in's own test against the clip's frame range; no range stated: all are).
inline bool inClip(const Ctx& c, double t)
{
    if (!(c.clipLast > c.clipFirst)) return true;
    return t > c.clipFirst - 0.5 && c.clipLast + 0.5 > t;
}

// The 3 x 3 map the plug-in makes from the Motion Tracking corners of the frame itself to those of another frame (here all frames have the corners of this one, the only ones the
// settings give): the identity for a proper quadrangle; for one that cannot be solved (the corners' default is -1, -1 for all four) the plug-in's solver leaves 0 0 0 0 0 0 0 0 1.
void cornerMap(const Ctx& c, float m[9])
{
    double px[4], py[4];
    const char* names[4] = {"CornerTL", "CornerTR", "CornerBR", "CornerBL"};
    for (int k = 0; k < 4; ++k) {
        double x = 0, y = 0;
        c.xy(names[k], x, y);
        px[k] = x * c.src.w;
        py[k] = y * c.src.h;
    }
    bool bad = false;
    for (int a = 0; a < 4 && !bad; ++a)
        for (int b = a + 1; b < 4 && !bad; ++b)
            for (int d = b + 1; d < 4; ++d) {
                const double cr = (px[b] - px[a]) * (py[d] - py[a]) - (py[b] - py[a]) * (px[d] - px[a]);
                if (std::fabs(cr) < 1e-9) { bad = true; break; }
            }
    for (int k = 0; k < 9; ++k) m[k] = 0.f;
    if (bad) m[8] = 1.f;
    else m[0] = m[4] = m[8] = 1.f;
}

// Temporal blend: the output as the plug-in makes it ('out' gets whole levels as floats / 255 on an 8-bit picture).
void temporalBlend(Ctx& c, int tw, float sens, int type, bool useMotion)
{
    const int W = c.src.w, H = c.src.h;
    const bool lv = c.levels;
    const float scale = lv ? 255.f : 1.f;
    int before = 0, after = 0;
    std::vector<float> wts;
    windowWeights(tw, sens, before, after, wts);
    // the frames that are in the clip, with their weights (a frame that is not has its weight lost); frames with no weight need not be read at all
    std::vector<WFrame> fr;
    const size_t npx = (size_t)W * H;
    int listIdx = 0;
    int centerKept = -1;
    for (int i = 0; i <= tw; ++i) {
        const double t = c.time - before + i;
        if (!inClip(c, t)) continue;
        const bool isCenter = listIdx == before;
        ++listIdx;
        const float wt = wts[(size_t)i];
        if (wt == 0.f && !isCenter) continue;
        Buf tmp;
        const Buf* b = nullptr;
        if (std::fabs(t - c.time) < 1e-9) b = &c.src;
        else if (c.fetch && c.fetch(t, tmp) && tmp.w == W && tmp.h == H) b = &tmp;
        else continue;                          // (the clip has no such frame: as if it were outside)
        WFrame f;
        f.w = wt;
        if (lv) {
            f.b.resize(npx * 4);
            for (size_t k = 0; k < npx * 4; ++k) {
                const float v = b->px[k] * 255.f + 0.5f;
                f.b[k] = (uint8_t)(v <= 0.f ? 0.f : (v >= 255.f ? 255.f : v));
            }
        } else {
            f.f = b->px;
        }
        if (isCenter) centerKept = (int)fr.size();
        fr.push_back(std::move(f));
    }
    const int nf = (int)fr.size();
    float map[9];
    cornerMap(c, map);
    const bool mapIdentity = map[0] == 1.f && map[4] == 1.f && map[8] == 1.f;
    const int cap = std::min(nf, 64);
    // (the effect's coordinates: row 0 at the bottom)
    forRows(H, W, [&](int y) {
        float* o = c.out.at(0, y);
        const int prow = H - 1 - y;
        for (int x = 0; x < W; ++x, o += 4) {
            float c0[64], c1[64], c2[64], wt[64];
            float a0 = 0.f, a1 = 0.f, a2 = 0.f, al = 0.f;
            float sum = 0.f;
            const size_t pix = (size_t)y * W + x;
            for (int k = 0; k < cap; ++k) {
                size_t src = pix;
                if (useMotion && k != centerKept && !mapIdentity) {
                    const float fx = (float)x, fy = (float)prow;
                    const float den = map[6] * fx + map[7] * fy + map[8];
                    const float inv = (float)(1.0 / (double)den);
                    const float xp = ((map[0] * fx + map[1] * fy) + map[2]) * inv;
                    const float yp = ((map[3] * fx + map[4] * fy) + map[5]) * inv;
                    if (xp > -0.4f && (float)W - 0.6f > xp && yp > -0.4f && (float)H - 0.6f > yp) {
                        const int cx = (int)std::floor(xp + 0.5f), cy = (int)std::floor(yp + 0.5f);
                        src = (size_t)(H - 1 - std::max(0, std::min(H - 1, cy))) * W + (size_t)std::max(0, std::min(W - 1, cx));
                    }
                }
                float p0, p1, p2, pa;
                if (lv) {
                    const uint8_t* q = &fr[(size_t)k].b[src * 4];
                    p0 = (float)q[0]; p1 = (float)q[1]; p2 = (float)q[2]; pa = (float)q[3];
                } else {
                    const float* q = &fr[(size_t)k].f[src * 4];
                    p0 = q[0]; p1 = q[1]; p2 = q[2]; pa = q[3];
                }
                if (k == centerKept) al = pa;
                const float w = fr[(size_t)k].w;
                if (type == 3) {
                    a0 += w * p0; a1 += w * p1; a2 += w * p2;
                } else {
                    float m = std::max(p0, std::max(p1, p2));
                    m = m / scale;
                    const float tf = (k == centerKept) ? 1.0f : toneFactor(type, m);
                    const float ww = tf * w;
                    wt[k] = ww;
                    sum += ww;
                    c0[k] = p0; c1[k] = p1; c2[k] = p2;
                }
            }
            if (type != 3)
                for (int k = 0; k < cap; ++k) {
                    const float q = wt[k] / sum;
                    a0 += q * c0[k]; a1 += q * c1[k]; a2 += q * c2[k];
                }
            if (lv) {
                o[0] = toLevel(a0) / 255.f; o[1] = toLevel(a1) / 255.f; o[2] = toLevel(a2) / 255.f;
                o[3] = al / 255.f;
            } else {
                o[0] = a0; o[1] = a1; o[2] = a2; o[3] = al;
            }
        }
    });
}

// Addition: the plug-in's flicker added to the levels.
void additionRender(Ctx& c, int type, double sens)
{
    const int W = c.src.w, H = c.src.h;
    const bool lv = c.levels;
    const double fps = c.fps > 0 ? c.fps : 30.0;
    const double freq = c.d("Frequency");
    const double t = std::floor(c.time);
    double amount = 0;
    if (freq > 1.0) {
        amount = std::sin(freq * 6.283185307179586 * t / fps) * (sens * 0.1);
    } else {
        // 'Celluloid': the plug-in draws a random number of 1000 trials with chance 'sensitivity' (times 1e-4) and puts it in only when the whole second is a multiple of another
        // random number (about fps / sens trials, chance 1 - 0.9 sens) plus one - for most of a clip, never. The draws here are from a hash of the frame number.
        const double p = clampd(sens, 0.0, 1.0);
        uint64_t hsh = (uint64_t)(int64_t)std::llround(c.time) * 0x9E3779B97F4A7C15ULL + 0x1234567ULL;
        auto rnd = [&hsh]() {
            hsh ^= hsh >> 33; hsh *= 0xff51afd7ed558ccdULL; hsh ^= hsh >> 33; hsh *= 0xc4ceb9fe1a85ec53ULL; hsh ^= hsh >> 33;
            return ((double)(hsh >> 11) + 0.5) / 9007199254740992.0;
        };
        auto normal = [&]() { return std::sqrt(-2.0 * std::log(rnd())) * std::cos(2.0 * kPi * rnd()); };
        const double nA = std::floor(1000.0 * p + std::sqrt(1000.0 * p * (1 - p)) * normal() + 0.5);
        const int trialsB = p > 0 ? (int)(fps / p) : 0;
        const double pB = 1.0 - p * 0.9;
        const double nB = std::floor(trialsB * pB + std::sqrt(std::max(0.0, trialsB * pB * (1 - pB))) * normal() + 0.5);
        const long secs = (long)(t / fps);
        if (secs % ((long)std::max(0.0, nB) + 1) == 0) amount = std::max(0.0, nA) * 0.0001;
    }
    const float scale = lv ? 255.f : 1.f;
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            float p0 = s[0] * scale, p1 = s[1] * scale, p2 = s[2] * scale, pa = s[3] * scale;
            if (lv) { p0 = std::floor(p0 + 0.5f); p1 = std::floor(p1 + 0.5f); p2 = std::floor(p2 + 0.5f); pa = std::floor(pa + 0.5f); }
            if (pa == 0.f) { o[0] = o[1] = o[2] = o[3] = 0.f; continue; }
            const float m = std::max(p0, std::max(p1, p2)) / scale;
            float f = 1.0f;
            if (type == 2) f = 1.0f - m * m;
            else if (type == 1) { const double d = (double)m - 0.5; f = (float)(1.0 - (d * 4.0) * d); }
            else if (type == 0) { const float u = 1.0f - m; f = 1.0f - u * u; }
            const double add = ((double)f * amount) * (double)pa;         // (levels; a float picture: alpha 0 .. 1 stands for 0 .. 255)
            const double mx = lv ? 255.0 : 1.0;
            auto one = [&](float v) {
                double x2 = (double)v + add;
                x2 = x2 < 0.0 ? 0.0 : std::min(mx, x2);
                if (lv) return (float)(int)((float)x2 + 0.5f) / 255.f;
                return (float)x2;
            };
            o[0] = one(p0); o[1] = one(p1); o[2] = one(p2); o[3] = s[3];
        }
    });
}

void flickerRender(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0 || c.src.px.size() != c.out.px.size()) return;
    const int mode = c.i("Mode");                                   // 0 reduction, 1 addition
    const int model = c.i("Model");
    const int type = std::max(0, std::min(3, c.i("Type")));
    const double sens = std::max(0.0, c.d("Sensitivity"));
    const bool showMask = c.b("ShowMask");
    const bool useMotion = c.b("UseMotion");
    const int tw = std::max(0, std::min(60, (int)c.d("TimeWindow")));

    bool calculated = false;
    std::shared_ptr<FlickerState> stp;
    if (model == 1 && c.instance) {
        stp = stateFor(c.instance);
        calculated = stp->calculated;
    }
    // (the plug-in does nothing in these cases: the source comes through, with no mask)
    const bool identity = sens < 2.220446049250313e-16 || (mode != 1 && (tw == 0 || (model == 1 && !calculated)));
    if (identity) {
        c.out.px = c.src.px;
        return;
    }
    bool done = false;
    if (mode == 1) {
        additionRender(c, type, sens);
        done = true;
    } else if (model == 0) {
        temporalBlend(c, tw, (float)sens, type, useMotion);
        done = true;
    }
    if (done) {
        if (showMask) {
            const float scale = c.levels ? 255.f : 1.f;
            forRows(H, W, [&](int y) {
                const float* s = c.src.at(0, y);
                float* o = c.out.at(0, y);
                for (int x = 0; x < W; ++x, s += 4, o += 4) {
                    float d = 0.f;
                    for (int k = 0; k < 3; ++k) {
                        float a = o[k] * scale, b = s[k] * scale;
                        if (c.levels) { a = std::floor(a + 0.5f); b = std::floor(b + 0.5f); }
                        d = std::max(d, std::fabs(a - b));
                    }
                    float v = d * 5.f;
                    v = v < 0.f ? 0.f : std::min(scale, v);
                    if (c.levels) v = (float)(int)(v + 0.5f);
                    o[0] = o[1] = o[2] = v / scale;
                    o[3] = s[3];
                }
            });
        }
        return;
    }
    if (!stp) return;
    // Histogram smoothing after Calculate: Reno's own method (see the top)
    std::vector<float> lut;
    {
        FlickerState& st = *stp;
        std::lock_guard<std::mutex> lk(st.mu);
        const long n0 = (long)std::lround(c.time);
        std::shared_ptr<FrameStat> s0 = makeStat(c.src);
        {
            auto it = st.frames.find(n0);
            if (it != st.frames.end() && !sameSignature(it->second->sig, s0->sig)) st.frames.clear();
        }
        st.frames[n0] = s0;
        const int win = tw * 2;
        std::vector<std::shared_ptr<FrameStat>> nb;
        nb.push_back(s0);
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int j = 1; j <= win; ++j) {
                const long key = n0 + dir * j;
                std::shared_ptr<FrameStat> f;
                auto it = st.frames.find(key);
                if (it != st.frames.end()) f = it->second;
                else {
                    Buf tmp;
                    if (!c.fetch || !c.fetch(c.time + (double)(dir * j), tmp) || tmp.w != c.src.w || tmp.h != c.src.h) break;
                    f = makeStat(tmp);
                    if (f->tiny.alpha < 0.02) break;
                    st.frames[key] = f;
                }
                if (f->tiny.alpha < 0.02) break;
                nb.push_back(f);
            }
        }
        while (st.frames.size() > 400) {
            auto far = st.frames.begin();
            long fd = -1;
            for (auto it = st.frames.begin(); it != st.frames.end(); ++it)
                if (std::labs(it->first - n0) > fd) { fd = std::labs(it->first - n0); far = it; }
            st.frames.erase(far);
        }
        if (nb.size() >= 2) {
            std::vector<double> avg(256, 0.0), mine(256, 0.0);
            double a = 0, m = 0;
            for (int k = 0; k < 256; ++k) {
                m += s0->hist[(size_t)k];
                mine[(size_t)k] = m;
                double e = 0;
                for (const auto& f : nb) e += f->hist[(size_t)k];
                a += e / nb.size();
                avg[(size_t)k] = a;
            }
            lut.assign(256, 1.f);
            int u = 0;
            for (int k = 0; k < 256; ++k) {
                const double cv = mine[(size_t)k];
                while (u < 255 && avg[(size_t)u] < cv) ++u;
                double t = u;
                if (u > 0 && avg[(size_t)u] > avg[(size_t)u - 1]) t = u - 1 + (cv - avg[(size_t)u - 1]) / (avg[(size_t)u] - avg[(size_t)u - 1]);
                lut[(size_t)k] = (float)clampd((t + 0.5) / (k + 0.5), 0.5, 2.0);
            }
            std::vector<float> sm(256);
            for (int k = 0; k < 256; ++k) {
                double s = 0, sw = 0;
                for (int d = -4; d <= 4; ++d) {
                    const int q = std::max(0, std::min(255, k + d));
                    const double wt = 5 - std::fabs((double)d);
                    s += wt * lut[(size_t)q];
                    sw += wt;
                }
                sm[(size_t)k] = (float)(s / sw);
            }
            lut = sm;
        }
    }
    const bool haveLut = !lut.empty();
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            const float yv = luma(s[0], s[1], s[2]);
            const float wt = toneWeight(type, yv);
            float gain = 1.f;
            if (haveLut) {
                const float pos = std::min(255.f, std::max(0.f, yv * 255.f));
                const int i0 = (int)pos, i1 = std::min(255, i0 + 1);
                const float g = lut[(size_t)i0] * (1 - (pos - i0)) + lut[(size_t)i1] * (pos - i0);
                gain = 1.f + (g - 1.f) * (float)sens * wt;
            }
            if (showMask) {
                const float m = clamp01(std::fabs(gain - 1.f) * 6.f);
                o[0] = o[1] = o[2] = m; o[3] = 1.f;
            } else {
                o[0] = s[0] * gain; o[1] = s[1] * gain; o[2] = s[2] * gain; o[3] = s[3];
            }
        }
    });
}

// The Calculate / Reset buttons. VEGAS's Calculate looks at the whole clip once; Reno keeps what it has seen of each frame in the instance's cache, so Calculate fills it from
// the clip now (up to 400 frames from the current one, the cache's size) and Reset throws it away.
void flickerChanged(Change& ch)
{
    if (ch.param == "Clear") {
        destroyState(ch.effect);
        return;
    }
    if (ch.param != "Calc") return;
    std::shared_ptr<FlickerState> stp = stateFor(ch.effect);
    FlickerState& st = *stp;
    {
        std::lock_guard<std::mutex> lk(st.mu);
        st.calculated = true;
    }
    double first = 0, last = 0;
    ch.clipRange(first, last);
    if (!(last > first)) return;
    const long n0 = (long)std::lround(ch.time);
    const long lo = std::max<long>((long)first, n0 - 200), hi = std::min<long>((long)last - 1, lo + 399);
    for (long n = lo; n <= hi; ++n) {
        {
            std::lock_guard<std::mutex> lk(st.mu);
            if (st.frames.count(n)) continue;
        }
        Buf b;
        if (!ch.source((double)n, b)) continue;
        std::shared_ptr<FrameStat> f = makeStat(b);
        std::lock_guard<std::mutex> lk(st.mu);
        st.frames[n] = f;
    }
}

}  // namespace

void addFlickerEffect(std::vector<Def>& out)
{
    std::vector<PDef> ps;
    ps.push_back(choice("Mode", "Mode", 0, {"Reduction", "Addition"}));
    ps.push_back(dbl("Frequency", "Flicker Frequency", 55, 0, 120));
    ps.push_back(choice("Model", "Reduction Mode", 0, {"Temporal blend", "Histogram smoothing"}));
    ps.push_back(dbl("TimeWindow", "Analysis Window", 2, 0, 60));
    ps.push_back(dbl("Sensitivity", "Sensitivity", 1.0, 0, 3));
    ps.back().dmax = 1.0;
    ps.push_back(dbl("Threshold", "Threshold", 0, 0, 1));
    ps.push_back(choice("Type", "Type", 3, {"Highlights", "Midtones", "Shadow", "All"}));
    ps.push_back(boolean("UseMotion", "Use motion compensation", false));
    ps.push_back(button("Calc", "Calculate"));
    ps.push_back(button("Clear", "Reset"));
    ps.push_back(boolean("ShowMask", "Show mask only", false));
    const size_t hiddenFrom = ps.size();
    ps.push_back(point("CornerTL", "Top left", -1, -1));
    ps.push_back(point("CornerTR", "Top right", -1, -1));
    ps.push_back(point("CornerBR", "Bottom right", -1, -1));
    ps.push_back(point("CornerBL", "Bottom left", -1, -1));
    ps.push_back(intp("HistogramSize", "Histogram size", 0, 0, 1000000));
    ps.push_back(custom("Histogram", "Histogram", ""));
    ps.push_back(custom("SmoothedHistogram", "Smoothed histogram", ""));
    for (size_t k = hiddenFrom; k < ps.size(); ++k) ps[k].secret = true;
    Def d = make("de.magix:FlickerReducer", "Flicker Control",
                 "Removes flicker (brightness that jumps from frame to frame) by comparing each frame with the ones round it, or adds it at a frequency", ps, flickerRender);
    d.temporal = true;
    d.destroy = destroyState;
    d.changed = flickerChanged;
    out.push_back(std::move(d));
}

}  // namespace analysis
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
