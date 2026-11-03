// Vulkan plans of Defocus and Starburst (vkfft.h). The kernels (tools/gpukernels_fft.py) are the CPU's FFT convolution butterfly for butterfly in software float64.
#include "vkfft.h"
#include "vkshaders/push_layouts.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {

// the device's largest storage buffer; before the device has started (the first plan of a session is built before it), the
// least any device Reno accepts has (vkdevice.cpp: 64 MB), so a plan doesn't decline just because Vulkan isn't up yet
static size_t storageRange()
{
    const size_t r = gpu::currentDevice().maxStorageRange;
    return r ? r : (size_t)64 << 20;
}
namespace {

namespace P = gpu::push;
using gpu::Plan;
using gpu::Pass;
typedef std::map<std::string, std::vector<double>> PM;

struct D2 { uint32_t h, l; };
D2 d2(double d)
{
    uint64_t b;
    std::memcpy(&b, &d, 8);
    return D2{(uint32_t)(b >> 32), (uint32_t)b};
}

float bitsAsFloat(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// a double as two table words (the bits of its halves, held in floats)
void pushD(std::vector<float>& t, double d)
{
    const D2 b = d2(d);
    t.push_back(bitsAsFloat(b.h));
    t.push_back(bitsAsFloat(b.l));
}

// the twiddles (re, im pairs) as complex numbers of four words
std::vector<float> twiddleWords(const std::vector<double>& a, const std::vector<double>& b)
{
    std::vector<float> t;
    t.reserve((a.size() + b.size()) * 2);
    for (double d : a) pushD(t, d);
    for (double d : b) pushD(t, d);
    return t;
}

// a buffer of at least n words
int wordsBuf(Plan& p, size_t n)
{
    return p.addBuf(1024, (uint32_t)((n + 4095) / 4096), 4);
}

// a flat dispatch of n threads: the width, and the height that covers them
void flat(size_t n, uint32_t& wd, uint32_t& ht)
{
    wd = (uint32_t)std::min<size_t>(2048, std::max<size_t>(16, (n + 15) / 16 * 16));
    ht = (uint32_t)((n + wd - 1) / wd);
}

// The 2-D transform of 'count' n x n arrays in the buffer 'data' (rows, then columns), one pass per stage, as tools/gpukernels_fft.py describes: the forward transform takes natural
// order and leaves the scrambled one, the inverse the other way round. 'perStage': the table holds the twiddles stage by stage (len / 2 - 1 numbers before the stage of 'len', the
// inverse's after the n - 1 numbers of the forward one) instead of one table of n / 2 of the largest stage that every stage strides through (the inverse's after the forward's n / 2).
void fftPasses(Plan& p, int data, int tab, uint32_t count, uint32_t n, bool inverse, bool perStage)
{
    uint32_t L = 0;
    while ((1u << L) < n) ++L;
    const size_t nt = (size_t)count * n * n / 2;
    uint32_t wd, ht;
    flat(nt, wd, ht);
    for (uint32_t vert = 0; vert < 2; ++vert)
        for (uint32_t lg = 1; lg <= L; ++lg) {
            const uint32_t len = 1u << lg;
            uint32_t off, step;
            if (perStage) {
                off = (inverse ? n - 1 : 0) + len / 2 - 1;
                step = 1;
            } else {
                off = inverse ? n / 2 : 0;
                step = n / len;
            }
            Pass& ps = p.add("ff_stage", 0, data, wd, ht);
            ps.table = tab;
            ps.setPush(P::FfStage{(uint32_t)nt, wd, ht, n, L, lg, inverse ? 0u : 1u, vert, off, step, 0u});
        }
}

// ---------------------------------------------------------------------------------------------------------- Defocus
bool planDefocus(Ctx& c, Plan& p)
{
    if (!c.openCl || p.w == 0 || p.h == 0) return false;           // (the older model of VEGAS's CPU path is the CPU's)
    const int W = (int)p.w, H = (int)p.h;
    const double pct = std::min(100.0, std::max(0.0, c.d("Radius")));
    const double radius = pct / 100.0 * (double)std::max(W, H);
    const int shape = std::max(0, std::min(4, c.i("IrisShape")));
    const double orient = c.d("Orientation");
    const double curvature = clampd(c.d("DiscCurvature") / 100.0, -1.0, 1.0);
    const double thresh = clampd(c.d("BloomThreshold") / 100.0, 0, 2), strength = std::max(0.0, c.d("BloomStrength"));
    DefocusGpuSetup su;
    defocusGpuSetup(W, H, radius, shape, orient, curvature, thresh, strength, su);
    const int img = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("df_prep", 0, img, p.w, p.h);
        ps.setPush(P::DfPrep{p.w, p.h, c.b("ReduceFlicker") ? 1u : 0u, (!su.passThrough && su.bloom) ? 1u : 0u, su.bloomT, su.bloomK});
    }
    const uint32_t q8 = c.host8 ? 1u : 0u;
    if (su.passThrough) {
        p.add("df_finish", img, 1, p.w, p.h).setPush(P::DfFinish{p.w, p.h, q8, 0u});
        return true;
    }
    const uint32_t T = (uint32_t)su.T;
    const size_t tileWords = (size_t)2 * T * T * 4;
    const size_t limit = std::min<size_t>(16u * 1024 * 1024, std::max<size_t>(1, storageRange() / 4));
    const size_t perBatch = std::max<size_t>(1, limit / tileWords);
    if (tileWords > limit || (size_t)T * T * 4 > limit) return false;
    const size_t tiles = (size_t)su.nx * su.ny;
    const size_t bt = std::min(perBatch, tiles);
    const int tw = p.addTable(twiddleWords(su.fw, su.inv));
    const int kf = wordsBuf(p, (size_t)T * T * 4);
    const int dat = wordsBuf(p, bt * tileWords);
    const int res = p.addBuf(p.w, p.h, 4);
    {   // the iris's transform
        uint32_t wd, ht;
        flat((size_t)T * T, wd, ht);
        Pass& ps = p.add("df_kinit", 0, kf, wd, ht);
        ps.table = p.addTable(su.mask);
        ps.setPush(P::DfKinit{T * T, wd, ht, T, (uint32_t)su.half, 0u});
        fftPasses(p, kf, tw, 1, T, false, false);
    }
    const D2 sc = d2(su.scale);
    for (size_t t0 = 0; t0 < tiles; t0 += bt) {
        const size_t n = std::min(bt, tiles - t0);
        const size_t nt = n * T * T;
        uint32_t wd, ht;
        flat(nt, wd, ht);
        {
            Pass& ps = p.add("df_load", img, dat, wd, ht);
            ps.setPush(P::DfLoad{(uint32_t)nt, wd, ht, T, (uint32_t)su.nx, (uint32_t)su.step, -su.ov, (uint32_t)t0, su.N, p.w, p.h});
        }
        fftPasses(p, dat, tw, (uint32_t)(2 * n), T, false, false);
        {
            uint32_t w2, h2;
            const size_t nm = n * 2 * T * T;
            flat(nm, w2, h2);
            Pass& ps = p.add("ff_mul", 0, dat, w2, h2);
            ps.in1 = kf;
            ps.setPush(P::FfMul{(uint32_t)nm, w2, h2, T * T, 0u});
        }
        fftPasses(p, dat, tw, (uint32_t)(2 * n), T, true, false);
        {
            Pass& ps = p.add("df_store", dat, res, wd, ht);
            ps.setPush(P::DfStore{(uint32_t)nt, wd, ht, T, (uint32_t)su.nx, (uint32_t)su.step, -su.ov, (uint32_t)t0, su.ov, p.w, p.h, sc.h, sc.l});
        }
    }
    p.add("df_finish", res, 1, p.w, p.h).setPush(P::DfFinish{p.w, p.h, q8, 0u});
    return true;
}

// ---------------------------------------------------------------------------------------------------------- Starburst
bool planStarburst(Ctx& c, Plan& p)
{
    if (p.w == 0 || p.h == 0) return false;
    StarburstGpuSetup su;
    if (!more::starburstGpuSetup(c, (int)p.w, (int)p.h, su)) return false;
    if (su.passThrough) {
        if (su.effectOnly) p.add("fill_px", 0, 1, p.w, p.h).setPush(P::FillPx{p.w, p.h, 0.f, 0.f, 0.f, 0.f});
        else p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    const uint32_t N = (uint32_t)su.N;
    uint32_t lN = 0;
    while ((1u << lN) < N) ++lN;
    const size_t planeWords = (size_t)N * N * 4;
    const size_t limit = std::min<size_t>(16u * 1024 * 1024, std::max<size_t>(1, storageRange() / 4));
    if (2 * planeWords > limit) return false;
    // the picture's bytes, softened and blurred
    int img = p.addBuf(p.w, p.h, 4);
    {
        Pass& ps = p.add("sb_prep", 0, img, p.w, p.h);
        std::vector<float> m = su.soft.empty() ? std::vector<float>(1, 0.f) : su.soft;
        ps.table = p.addTable(std::move(m));
        ps.setPush(P::SbPrep{p.w, p.h, su.soft.empty() ? 0u : 1u, 0u});
    }
    if (su.reduceFlicker) {
        const int b = p.addBuf(p.w, p.h, 4);
        p.add("sb_rf", img, b, p.w, p.h).setPush(P::SbRf{p.w, p.h});
        img = b;
    }
    const int ab = wordsBuf(p, 2 * planeWords), kb = wordsBuf(p, planeWords);
    std::vector<float> tw;
    {
        std::vector<float> t = twiddleWords(su.fw, su.inv);
        tw.swap(t);
    }
    const int twt = p.addTable(tw);
    uint32_t wd, ht;
    {   // the planes of the picture (A: R and G, B: B and alpha)
        const size_t nt = (size_t)N * N;
        flat(nt, wd, ht);
        Pass& ps = p.add("sb_load", img, ab, wd, ht);
        ps.setPush(P::SbLoad{(uint32_t)nt, wd, ht, N, lN, p.w, p.h, su.thr ? 1u : 0u, su.boost ? 1u : 0u, su.thrLevel, su.boostT, su.boostS, 0u});
    }
    {   // the star
        const size_t nt = (size_t)N * N;
        flat(nt, wd, ht);
        Pass& ps = p.add("sb_kinit", 0, kb, wd, ht);
        std::vector<float> st;
        st.reserve(su.starBytes.size() + 1);
        for (uint32_t w : su.starBytes) st.push_back(bitsAsFloat(w));
        if (st.empty()) st.push_back(0.f);
        ps.table = p.addTable(std::move(st));
        ps.setPush(P::SbKinit{(uint32_t)nt, wd, ht, N, lN, (uint32_t)su.sn, su.starScale});
    }
    fftPasses(p, kb, twt, 1, N, false, true);
    fftPasses(p, ab, twt, 2, N, false, true);
    {
        const size_t nm = (size_t)2 * N * N;
        flat(nm, wd, ht);
        Pass& ps = p.add("ff_mul", 0, ab, wd, ht);
        ps.in1 = kb;
        ps.setPush(P::FfMul{(uint32_t)nm, wd, ht, N * N, 0u});
    }
    fftPasses(p, ab, twt, 2, N, true, true);
    {
        const D2 nm = d2(su.norm), op = d2(su.opacity);
        Pass& ps = p.add("sb_final", 0, 1, p.w, p.h);
        ps.in1 = ab;
        ps.setPush(P::SbFinal{p.w, p.h, N, su.effectOnly ? 1u : 0u, c.host8 ? 1u : 0u, 0u, nm.h, nm.l, op.h, op.l});
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------- Flicker Control
bool planFlicker(Ctx& c, Plan& p)
{
    analysis::FlickerGpuSetup su;
    if (!analysis::flickerGpuSetup(c, (int)p.w, (int)p.h, su) || su.kind == 0) return false;
    if (su.kind == 1) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    const uint32_t flags = (su.motion ? 1u : 0u) | (su.showMask ? 2u : 0u) | (su.q8 ? 4u : 0u);
    if (su.kind == 3) {
        Pass& ps = p.add("fl_add", 0, 1, p.w, p.h);
        ps.setPush(P::FlAdd{p.w, p.h, (uint32_t)su.type, flags & ~1u, su.amount, 0u});
        return true;
    }
    if (su.frames.size() * sizeof(float) > storageRange()) return false;
    std::vector<float> tab = su.frames;
    if (tab.size() < 4) tab.resize(4, 0.f);
    Pass& ps = p.add("fl_blend", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    P::FlBlend pc = {p.w, p.h, (uint32_t)su.type, (uint32_t)su.cen, (uint32_t)su.nf, (uint32_t)su.selfk, (uint32_t)su.before, flags};
    pc.m0 = su.map[0]; pc.m1 = su.map[1]; pc.m2 = su.map[2]; pc.m3 = su.map[3]; pc.m4 = su.map[4]; pc.m5 = su.map[5]; pc.m6 = su.map[6]; pc.m7 = su.map[7]; pc.m8 = su.map[8];
    pc.w0 = su.fw[0]; pc.w1 = su.fw[1]; pc.w2 = su.fw[2]; pc.w3 = su.fw[3]; pc.w4 = su.fw[4]; pc.w5 = su.fw[5];
    ps.setPush(pc);
    return true;
}

}  // namespace

const std::vector<FilterGpuEntry>& fftGpuEntries()
{
    static const std::vector<FilterGpuEntry> v = {
        {"com.vegascreativesoftware:defocusvelvetmatter", planDefocus, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("small", PM{{"Radius", {2.0}}}), GpuCheckCase("hexagon", PM{{"Radius", {6.5}}, {"IrisShape", {2}}, {"Orientation", {20}}}),
          GpuCheckCase("pentagon curved", PM{{"Radius", {5}}, {"IrisShape", {1}}, {"DiscCurvature", {60}}, {"Orientation", {200}}}),
          GpuCheckCase("octagon dim rim", PM{{"Radius", {7}}, {"IrisShape", {4}}, {"DiscCurvature", {-70}}}),
          GpuCheckCase("no bloom", PM{{"BloomThreshold", {100}}, {"BloomStrength", {0}}}), GpuCheckCase("strong bloom", PM{{"BloomThreshold", {40}}, {"BloomStrength", {15}}}),
          GpuCheckCase("reduce flicker", PM{{"ReduceFlicker", {1}}, {"Radius", {4}}}), GpuCheckCase("flicker bloom heptagon", PM{{"ReduceFlicker", {1}}, {"IrisShape", {3}}, {"Radius", {3}}, {"BloomThreshold", {60}}}),
          GpuCheckCase("tiny iris", PM{{"Radius", {0.3}}}), GpuCheckCase("tiny iris flicker", PM{{"Radius", {0.5}}, {"ReduceFlicker", {1}}}), GpuCheckCase("off", PM{{"Radius", {0}}}),
          GpuCheckCase("huge", PM{{"Radius", {11.2}}, {"DiscCurvature", {30}}})}},
        {"de.magix:FlickerReducer", planFlicker, 0.0,
         {GpuCheckCase("default", PM{{"__time", {10}}}), GpuCheckCase("window 6", PM{{"TimeWindow", {6}}, {"__time", {20}}}), GpuCheckCase("window 1", PM{{"TimeWindow", {1}}, {"__time", {20}}}),
          GpuCheckCase("window 60", PM{{"TimeWindow", {60}}, {"__time", {100}}}), GpuCheckCase("window 3 low sens", PM{{"TimeWindow", {3}}, {"Sensitivity", {0.3}}, {"__time", {30}}}),
          GpuCheckCase("sens 0.5", PM{{"TimeWindow", {4}}, {"Sensitivity", {0.5}}, {"__time", {30}}}), GpuCheckCase("sens 2", PM{{"TimeWindow", {2}}, {"Sensitivity", {2.0}}, {"__time", {30}}}),
          GpuCheckCase("highlights", PM{{"Type", {0}}, {"TimeWindow", {4}}, {"__time", {40}}}), GpuCheckCase("midtones", PM{{"Type", {1}}, {"TimeWindow", {4}}, {"__time", {40}}}),
          GpuCheckCase("shadow", PM{{"Type", {2}}, {"TimeWindow", {4}}, {"__time", {40}}}), GpuCheckCase("show mask", PM{{"ShowMask", {1}}, {"TimeWindow", {3}}, {"__time", {50}}}),
          GpuCheckCase("motion (corners unset)", PM{{"UseMotion", {1}}, {"TimeWindow", {4}}, {"__time", {50}}}),
          GpuCheckCase("motion corners", PM{{"UseMotion", {1}}, {"TimeWindow", {5}}, {"__time", {50}}, {"CornerTL", {0.1, 0.9}}, {"CornerTR", {0.9, 0.9}}, {"CornerBR", {0.9, 0.1}}, {"CornerBL", {0.1, 0.1}}}),
          GpuCheckCase("motion mask", PM{{"UseMotion", {1}}, {"ShowMask", {1}}, {"TimeWindow", {2}}, {"__time", {5}}, {"CornerTL", {0.0, 1.0}}, {"CornerTR", {1.0, 1.0}}, {"CornerBR", {1.0, 0.0}}, {"CornerBL", {0.0, 0.0}}}),
          GpuCheckCase("start of the clip", PM{{"TimeWindow", {6}}, {"__time", {1}}}), GpuCheckCase("histogram model", PM{{"Model", {1}}, {"TimeWindow", {3}}}),
          GpuCheckCase("no window", PM{{"TimeWindow", {0}}}), GpuCheckCase("no sensitivity", PM{{"Sensitivity", {0}}}),
          GpuCheckCase("add 55 Hz", PM{{"Mode", {1}}, {"Frequency", {55}}, {"Sensitivity", {0.5}}, {"__time", {7}}}), GpuCheckCase("add 50 Hz highlights", PM{{"Mode", {1}}, {"Frequency", {50}}, {"Type", {0}}, {"Sensitivity", {1.0}}, {"__time", {11}}}),
          GpuCheckCase("add midtones", PM{{"Mode", {1}}, {"Frequency", {23}}, {"Type", {1}}, {"Sensitivity", {0.8}}, {"__time", {5}}}), GpuCheckCase("add shadow mask", PM{{"Mode", {1}}, {"Frequency", {40}}, {"Type", {2}}, {"ShowMask", {1}}, {"__time", {3}}}),
          GpuCheckCase("add celluloid", PM{{"Mode", {1}}, {"Frequency", {0}}, {"Sensitivity", {0.3}}, {"__time", {0}}}), GpuCheckCase("add celluloid late", PM{{"Mode", {1}}, {"Frequency", {1}}, {"Sensitivity", {0.9}}, {"__time", {64}}})}},
        {"com.vegascreativesoftware:starburstvelvetmatter", planStarburst, 0.0,
         {GpuCheckCase("default", PM{{"Strength", {60}}}), GpuCheckCase("threshold", PM{{"Strength", {80}}, {"Threshold", {50}}, {"Brilliance", {300}}}),
          GpuCheckCase("six points", PM{{"Strength", {70}}, {"Points", {6}}, {"Radius", {10}}, {"Orientation", {10}}}), GpuCheckCase("two points", PM{{"Strength", {70}}, {"Points", {2}}, {"Radius", {8}}}),
          GpuCheckCase("centre burst", PM{{"Strength", {60}}, {"CenterBurst", {70}}, {"Radius", {12}}}), GpuCheckCase("soften", PM{{"Strength", {60}}, {"SoftenEdges", {40}}}),
          GpuCheckCase("flicker", PM{{"Strength", {60}}, {"ReduceFlicker", {1}}, {"Threshold", {30}}}), GpuCheckCase("effect only", PM{{"Strength", {90}}, {"EffectOnly", {1}}, {"Threshold", {20}}}),
          GpuCheckCase("big", PM{{"Strength", {50}}, {"Radius", {30}}}), GpuCheckCase("small", PM{{"Strength", {100}}, {"Radius", {2}}, {"Brilliance", {1500}}}),
          GpuCheckCase("everything", PM{{"Strength", {75}}, {"Threshold", {35}}, {"Points", {5}}, {"CenterBurst", {40}}, {"SoftenEdges", {70}}, {"ReduceFlicker", {1}}, {"Orientation", {123}}})}},
    };
    return v;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
