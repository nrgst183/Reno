// Built-in version of VEGAS Pro 18's Flicker Control (de.magix:FlickerReducer, MagixCVFx.ofx): removes flicker (brightness that jumps from frame to frame:
// mains lamps, a camera's auto exposure) or adds it. Parameters as the plug-in declares them: Mode (Reduction / Addition), Frequency (Addition), Model (Temporal blend /
// Histogram smoothing), TimeWindow (frames), Sensitivity, Threshold, Type (Highlights, Midtones, Shadow, All), UseMotion and ShowMask; the corners and histogram settings
// are what VEGAS keeps of its own analysis ("Calculate") and are kept as they are.
//
// What VEGAS's plug-in does (read from its code and its OpenCL kernels, vfxflicker.h; checked bit for bit against VEGAS's pictures of 12-frame clips, see below):
//  - Temporal blend: the output is a weighted mean of the frames from time - TimeWindow / 2 to time + (TimeWindow - TimeWindow / 2) that the clip has (at most 6), with the plug-in's
//    window weights (vfxfl::windowWeights: all equal at Sensitivity 1), divided by the sum of the weights that are left (so a clip's ends do not darken). Type 'All' takes
//    the weights as they are; Highlights and Midtones weight every frame but the middle one at each pixel by how much that pixel is of the tone (highlights 1 - (1 - v)^2,
//    midtones 1 - 4 (v - .5)^2, v the largest channel); Shadow is the kernel's plain weighted sum. UseMotion samples the other frames at a position from the Motion Tracking
//    corners (a position outside the frame: the frame has no weight there).
//  - Addition: every channel gets sin(2 pi Frequency floor(time) / fps) * Sensitivity * 0.1 * alpha (times the tone, for Highlights, Midtones and Shadow) added (a frequency of 1
//    or less is VEGAS's random 'celluloid' flicker, which cannot be reproduced).
//  - Show mask: 5 x the largest channel difference between the result and the frame itself, as grey.
//  - Nothing is done (the source comes through) when Sensitivity is 0, in Reduction when TimeWindow is 0, and for Histogram smoothing until Calculate has been pressed.
//  - Histogram smoothing (after Calculate): Reno's own method (VEGAS's analysis is stored in the project, which is not read): the frame's brightness histogram is matched
//    to the average of the histograms round it.
// Neighbouring frames' brightness (for the histogram model) is cached per instance (one more frame each time while playing).
#include "builtins_analysis_internal.h"
#include "builtins_more_internal.h"
#include "vfxflicker.h"
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

// ------------------------------------------------------------------------------------------------ the OpenCL kernels' temporal blend and Addition (vfxflicker.h)
// Whether a frame time is one the clip has (the plug-in's own test against the clip's frame range; no range stated: all are).
inline bool inClip(const Ctx& c, double t)
{
    if (!(c.clipLast > c.clipFirst)) return true;
    return t > c.clipFirst - 0.5 && c.clipLast - 0.5 > t;       // (the host's frame range is [first, first + duration): the last frame is clipLast - 1)
}

// The map the plug-in makes from the Motion Tracking corners of the frame itself to those of another frame (here all frames have the corners of this one, the only ones the
// settings give): the identity for a proper quadrangle. For corners that cannot be solved (their default is -1, -1 for all four) the plug-in's solver (not decoded) gives a map under
// which, in VEGAS's pictures, only the left column of the frame finds a position in the frame (the bottom left pixel, for every row of it) and the rest has none: a
// huge x factor with everything else 0 reproduces that (GUESS about the numbers, measured on one picture).
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
    if (bad) { m[0] = 1e30f; m[8] = 1.f; }
    else m[0] = m[4] = m[8] = 1.f;
}

// The picture as the kernels read it: an 8-bit frame is bytes / 255.
inline float inLevel(float v, bool q)
{
    if (!q) return v;
    return (float)(int)(std::min(1.f, std::max(0.f, v)) * 255.f + 0.5f) / 255.f;
}

// Temporal blend as the kernel does it: the frames time - tw / 2 ... time + (tw - tw / 2) that the clip has (the first 6 of them), each with its window weight; the weights of
// frames left out are not renormalised by the host but by the kernel's division (not for Shadow, which is a plain weighted sum). The frame itself is taken to be the slot with
// the number tw / 2 whichever frames were left out before it (so at the start of a clip another frame is the one that takes no tone weight - as in VEGAS's pictures).
// A time window of 1 works as one of 2 (VEGAS's picture of the Flicker Reduction preset shows 3 frames; the reason is not found).
void temporalBlend(Ctx& c, int tw, float sens, int type, bool useMotion, bool showMask)
{
    const int W = c.src.w, H = c.src.h;
    const bool q = c.host8;
    if (tw == 1) tw = 2;
    int before = 0, after = 0;
    std::vector<float> wts;
    vfxfl::windowWeights(tw, sens, before, after, wts);
    const size_t npx = (size_t)W * H;
    std::vector<Buf> fr;
    std::vector<float> fw;
    fr.reserve(6);
    for (int i = 0; i <= tw && fr.size() < 6; ++i) {
        const double t = c.time - before + i;
        if (!inClip(c, t)) continue;
        Buf b;
        if (std::fabs(t - c.time) < 1e-9) b = c.src;
        else if (!(c.fetch && c.fetch(t, b) && b.w == W && b.h == H)) continue;     // (the clip has no such frame: as if it were outside)
        if (q) for (float& v : b.px) v = inLevel(v, true);
        fr.push_back(std::move(b));
        fw.push_back(wts[(size_t)i]);
    }
    const int nf = (int)fr.size();
    if (nf == 0) { c.out.px = c.src.px; return; }
    int cen = before;
    if (useMotion && nf <= cen) cen = nf - 1;
    float map[9];
    cornerMap(c, map);
    // (the kernels' coordinates have row 0 at the bottom; Buf row 0 is the top)
    forRows(H, W, [&](int y) {
        float* o = c.out.at(0, y);
        const int ky = H - 1 - y;
        const float* self = c.src.at(0, y);
        for (int x = 0; x < W; ++x, o += 4) {
            vfxfl::Slot sl[6];
            float own[4];
            for (int k = 0; k < 4; ++k) own[k] = inLevel(self[x * 4 + k], q);
            for (int k = 0; k < 6; ++k) { sl[k].px = own; sl[k].w = 0.f; }
            for (int k = 0; k < nf; ++k) {
                const float* px = &fr[(size_t)k].px[((size_t)y * W + x) * 4];
                float w = fw[(size_t)k];
                if (useMotion && k != before) {
                    float xp, yp;
                    vfxfl::mapPoint(map, (float)x, (float)ky, xp, yp);
                    if (xp < 0 || xp > (float)(W - 1) || yp < 0 || yp > (float)(H - 1)) w = 0.f;      // (this pixel's own position, with no weight)
                    else {
                        const int cx = std::max(0, std::min(W - 1, (int)std::floor(xp))), cy = std::max(0, std::min(H - 1, (int)std::floor(yp)));
                        px = &fr[(size_t)k].px[((size_t)(H - 1 - cy) * W + cx) * 4];
                    }
                }
                sl[k].px = px;
                sl[k].w = w;
            }
            float r[4];
            vfxfl::blendPixel(sl, type, cen, r);
            if (showMask) {
                float m[4];
                vfxfl::maskPixel(sl[cen >= 0 && cen < 6 ? cen : 0].px, r, m);
                for (int k = 0; k < 4; ++k) r[k] = m[k];
            }
            for (int k = 0; k < 4; ++k) o[k] = q ? more::oclStore12(r[k]) : r[k];
        }
    });
    (void)npx;
}

// Addition: the plug-in's flicker added (the Addition kernel).
void additionRender(Ctx& c, int type, double sens, bool showMask)
{
    const int W = c.src.w, H = c.src.h;
    const bool q = c.host8;
    const double fps = c.fps > 0 ? c.fps : 30.0;
    const double freq = c.d("Frequency");
    const double t = std::floor(c.time);
    double amount = 0;
    const double sensF = (double)(float)sens;
    if (freq > 1.0) {
        amount = std::sin(freq * 6.283185307179586 * t / fps) * (sensF * 0.1);
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
    const float amt = (float)amount;
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            float p[4], r[4];
            for (int k = 0; k < 4; ++k) p[k] = inLevel(s[k], q);
            vfxfl::additionPixel(p, type, amt, r);
            if (showMask) { float m[4]; vfxfl::maskPixel(p, r, m); for (int k = 0; k < 4; ++k) r[k] = m[k]; }
            for (int k = 0; k < 4; ++k) o[k] = q ? more::oclStore12(r[k]) : r[k];
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
    const bool identity = sens < 2.220446049250313e-16 || (mode < 1 && (tw == 0 || (model == 1 && !calculated)));
    if (identity) {
        c.out.px = c.src.px;
        return;
    }
    bool done = false;
    if (mode >= 1) {
        additionRender(c, type, sens, showMask);
        done = true;
    } else if (model == 0) {
        temporalBlend(c, tw, (float)sens, type, useMotion, showMask);
        done = true;
    }
    if (done) {
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
