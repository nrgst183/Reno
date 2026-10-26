// Built-in version of VEGAS Pro 18's Flicker Control (de.magix:FlickerReducer, MagixCVFx.ofx): removes flicker (brightness that jumps from frame to frame:
// mains lamps, a camera's auto exposure) or adds it. Parameters as the plug-in declares them: Mode (Removal / Addition), Frequency (Addition), Model (Neighbor blend /
// Histogram smoothing), TimeWindow (frames looked at each side), Sensitivity, Threshold, Type (Highlights, Midtones, Shadow, All), UseMotion and ShowMask; the corners and
// histogram settings are what VEGAS keeps of its own analysis ("Calculate") and are kept as they are.
//
// Reno's own method (VEGAS's is not published):
//  - Neighbor blend: each frame's brightness over a 32 x 18 grid of cells (weighted toward the chosen tones) is compared with the average of the frames round it; the
//    ratio, where the cells agree with the picture as a whole (so a moving subject is left alone), smoothed over the grid, is the gain applied (more where the tone is the
//    chosen one). UseMotion shifts the neighbours onto the frame first.
//  - Histogram smoothing: the frame's brightness histogram is matched to the average of the histograms round it.
//  - Addition: the picture gets brighter and darker (up to 25 % x Sensitivity) in the chosen tones at Frequency Hz.
// Neighbouring frames' brightness is cached per instance (one more frame each time while playing).
#include "builtins_analysis_internal.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {
namespace {

const int kGX = 32, kGY = 18;

struct FrameStat {
    Gray tiny;                      // 64 wide
    Signature sig;
    std::vector<float> hist;
};

struct FlickerState {
    std::mutex mu;
    std::map<long, std::shared_ptr<FrameStat>> frames;
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

// The brightness of a frame over the grid: for each cell the mean grey weighted toward the tones, and the weight there is. 'shift' moves
// where the frame is read (a neighbour put onto the current frame).
void cellMeans(const Gray& g, int type, double shx, double shy, float mean[kGX * kGY], float weight[kGX * kGY])
{
    const int w = g.w, h = g.h;
    for (int j = 0; j < kGY; ++j)
        for (int i = 0; i < kGX; ++i) {
            const int x0 = i * w / kGX, x1 = std::max(x0 + 1, (i + 1) * w / kGX), y0 = j * h / kGY, y1 = std::max(y0 + 1, (j + 1) * h / kGY);
            double sy = 0, sw = 0;
            for (int y = y0; y < std::min(y1, h); ++y)
                for (int x = x0; x < std::min(x1, w); ++x) {
                    const float v = (shx == 0 && shy == 0) ? g.at(x, y) : g.sample(x + shx, y + shy);
                    const float wt = toneWeight(type, v) + 0.002f;
                    sy += wt * v;
                    sw += wt;
                }
            mean[j * kGX + i] = sw > 0 ? (float)(sy / sw) : 0.f;
            weight[j * kGX + i] = (float)sw;
        }
}

void flickerRender(Ctx& c)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0 || c.src.px.size() != c.out.px.size()) return;
    const int mode = c.i("Mode");                                   // 0 removal, 1 addition
    const int model = c.i("Model");
    const int type = std::max(0, std::min(3, c.i("Type")));
    const double sens = clampd(c.d("Sensitivity"), 0, 3);
    const double thr = clampd(c.d("Threshold"), 0, 1);
    const bool showMask = c.b("ShowMask");
    const bool useMotion = c.b("UseMotion");
    const int tw = std::max(0, std::min(3, (int)std::lround(c.d("TimeWindow"))));      // (a Double in VEGAS: whole frames here)

    std::vector<float> gainGrid;                                    // kGX * kGY, or empty
    std::vector<float> lut;
    float globalGain = 1.f;                                         // Addition

    if (mode == 1) {
        const double fps = c.fps > 0 ? c.fps : 30.0;
        const double freq = clampd(c.d("Frequency"), 0, 1000);
        const double amp = 0.25 * sens;
        globalGain = (float)(amp * std::sin(2 * kPi * freq * c.time / fps));
    } else if (tw > 0 && c.fetch && c.instance) {
        std::shared_ptr<FlickerState> stp = stateFor(c.instance);
        FlickerState& st = *stp;
        std::lock_guard<std::mutex> lk(st.mu);
        const long n0 = (long)std::lround(c.time);
        std::shared_ptr<FrameStat> s0 = makeStat(c.src);
        // a frame whose picture is not the one kept for it: the frames before it were changed: start again
        {
            auto it = st.frames.find(n0);
            if (it != st.frames.end() && !sameSignature(it->second->sig, s0->sig)) st.frames.clear();
        }
        st.frames[n0] = s0;
        const int win = (model == 1) ? tw * 2 : tw;
        std::vector<std::shared_ptr<FrameStat>> nb;
        nb.push_back(s0);
        std::vector<double> shiftX, shiftY;
        shiftX.push_back(0); shiftY.push_back(0);
        for (int dir = -1; dir <= 1; dir += 2) {
            for (int j = 1; j <= win; ++j) {
                const long key = n0 + dir * j;
                std::shared_ptr<FrameStat> f;
                auto it = st.frames.find(key);
                if (it != st.frames.end()) f = it->second;
                else {
                    Buf tmp;
                    if (!c.fetch(c.time + (double)(dir * j), tmp) || tmp.w != c.src.w || tmp.h != c.src.h) break;
                    f = makeStat(tmp);
                    if (f->tiny.alpha < 0.02) break;
                    st.frames[key] = f;
                }
                if (f->tiny.alpha < 0.02) break;
                nb.push_back(f);
                double dx = 0, dy = 0;
                if (useMotion && model == 0) globalShift(s0->tiny, f->tiny, 3, dx, dy);
                shiftX.push_back(dx); shiftY.push_back(dy);
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
            if (model == 1) {
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
            } else {
                std::vector<float> mean0(kGX * kGY), w0(kGX * kGY), tmean(kGX * kGY, 0.f), mj(kGX * kGY), wj(kGX * kGY);
                cellMeans(s0->tiny, type, 0, 0, mean0.data(), w0.data());
                for (size_t k = 0; k < nb.size(); ++k) {
                    if (k == 0) { for (int q = 0; q < kGX * kGY; ++q) tmean[(size_t)q] += mean0[(size_t)q]; continue; }
                    cellMeans(nb[k]->tiny, type, shiftX[k], shiftY[k], mj.data(), wj.data());
                    for (int q = 0; q < kGX * kGY; ++q) tmean[(size_t)q] += mj[(size_t)q];
                }
                for (float& v : tmean) v /= (float)nb.size();
                double gs0 = 0, gst = 0, gsw = 0;
                for (int q = 0; q < kGX * kGY; ++q) { gs0 += w0[(size_t)q] * mean0[(size_t)q]; gst += w0[(size_t)q] * tmean[(size_t)q]; gsw += w0[(size_t)q]; }
                const double g0 = (gs0 > 1e-6) ? clampd(gst / gs0, 0.4, 2.5) : 1.0;
                gainGrid.assign(kGX * kGY, 1.f);
                for (int q = 0; q < kGX * kGY; ++q) {
                    double g = mean0[(size_t)q] > 0.01f ? tmean[(size_t)q] / mean0[(size_t)q] : g0;
                    g = clampd(g, 0.4, 2.5);
                    if (std::fabs(g - g0) > 0.2) g = g0;                // not flicker: something moved there
                    gainGrid[(size_t)q] = (float)g;
                }
                std::vector<float> tmp(gainGrid.size());
                for (int pass = 0; pass < 2; ++pass) {
                    for (int j = 0; j < kGY; ++j)
                        for (int i = 0; i < kGX; ++i) {
                            double s = 0, sw = 0;
                            for (int dj = -1; dj <= 1; ++dj)
                                for (int di = -1; di <= 1; ++di) {
                                    const int jj = std::max(0, std::min(kGY - 1, j + dj)), ii = std::max(0, std::min(kGX - 1, i + di));
                                    const double wt = (di == 0 ? 2.0 : 1.0) * (dj == 0 ? 2.0 : 1.0);
                                    s += wt * gainGrid[(size_t)(jj * kGX + ii)];
                                    sw += wt;
                                }
                            tmp[(size_t)(j * kGX + i)] = (float)(s / sw);
                        }
                    gainGrid = tmp;
                }
                const double dead = thr * 0.1;
                for (float& g : gainGrid) {
                    double d = (g - 1.0) * sens;
                    const double m = std::max(0.0, std::fabs(d) - dead);
                    d = d < 0 ? -m : m;
                    g = (float)(1.0 + d);
                }
            }
        }
    }

    const bool haveGrid = !gainGrid.empty(), haveLut = !lut.empty();
    forRows(H, W, [&](int y) {
        const float* s = c.src.at(0, y);
        float* o = c.out.at(0, y);
        const double gyf = (y + 0.5) * kGY / H - 0.5;
        const int gy0 = std::max(0, std::min(kGY - 1, (int)std::floor(gyf))), gy1 = std::min(kGY - 1, gy0 + 1);
        const float fy = (float)std::max(0.0, std::min(1.0, gyf - gy0));
        for (int x = 0; x < W; ++x, s += 4, o += 4) {
            const float a = s[3];
            const float yv = luma(s[0], s[1], s[2]);
            const float wt = toneWeight(type, yv);
            float gain = 1.f;
            if (mode == 1) {
                gain = 1.f + globalGain * wt;
            } else if (haveGrid) {
                const double gxf = (x + 0.5) * kGX / W - 0.5;
                const int gx0 = std::max(0, std::min(kGX - 1, (int)std::floor(gxf))), gx1 = std::min(kGX - 1, gx0 + 1);
                const float fx = (float)std::max(0.0, std::min(1.0, gxf - gx0));
                const float g = (gainGrid[(size_t)(gy0 * kGX + gx0)] * (1 - fx) + gainGrid[(size_t)(gy0 * kGX + gx1)] * fx) * (1 - fy) +
                                (gainGrid[(size_t)(gy1 * kGX + gx0)] * (1 - fx) + gainGrid[(size_t)(gy1 * kGX + gx1)] * fx) * fy;
                gain = 1.f + (g - 1.f) * (type == 3 ? 1.f : wt);
            } else if (haveLut) {
                const float pos = std::min(255.f, std::max(0.f, yv * 255.f));
                const int i0 = (int)pos, i1 = std::min(255, i0 + 1);
                const float g = lut[(size_t)i0] * (1 - (pos - i0)) + lut[(size_t)i1] * (pos - i0);
                gain = 1.f + (g - 1.f) * (float)sens * wt;
            }
            if (showMask) {
                const float m = clamp01(std::fabs(gain - 1.f) * 6.f);
                o[0] = o[1] = o[2] = m; o[3] = 1.f;
            } else {
                o[0] = s[0] * gain; o[1] = s[1] * gain; o[2] = s[2] * gain; o[3] = a;
                // (premultiplied: never more colour than alpha in a frame that is not a float one is left to the framework's clamp)
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
    double first = 0, last = 0;
    ch.clipRange(first, last);
    if (!(last > first)) return;
    std::shared_ptr<FlickerState> stp = stateFor(ch.effect);
    FlickerState& st = *stp;
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
    ps.push_back(dbl("TimeWindow", "Analysis Window", 2, 0, 3));
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
