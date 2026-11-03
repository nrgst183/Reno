// Vulkan plans of the filters and generators in vkfx2.h. The kernels (tools/gpukernels_fx2.py) are the CPU code's per-pixel part in the same arithmetic.
#include "vkfx2.h"
#include "vkshaders/push_layouts.h"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace reno {
namespace ofx {
namespace builtin {
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

// whole numbers in a table of floats: their bits
void pushInt(std::vector<float>& t, int32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    t.push_back(f);
}

// a double as two table words (the bits of its halves, held in floats): the kernels read them with ld_sf
void pushD(std::vector<float>& t, double d)
{
    const D2 b = d2(d);
    float f;
    std::memcpy(&f, &b.h, 4);
    t.push_back(f);
    std::memcpy(&f, &b.l, 4);
    t.push_back(f);
}

// ------------------------------------------------------------------ TV Simulator (vfxtv.h render8)
bool planTvSimulator(Ctx& c, Plan& p)
{
    vftv::Rows8 r;
    more::tvSimulatorGpu(c, (int)p.w, (int)p.h, r);
    std::vector<float> tab;
    tab.reserve(r.rows.size() + 768);
    for (int32_t v : r.rows) pushInt(tab, v);
    for (int i = 0; i < 256; ++i)
        for (int k = 0; k < 3; ++k) pushInt(tab, r.lut[i][k]);
    const int t = p.addTable(std::move(tab));
    int in1 = 0;
    if (r.jitter) {
        in1 = p.addBuf(p.w, p.h, 4);
        Pass& s = p.add("tv_shift", 0, in1, p.w, p.h);
        s.table = t;
        s.setPush(P::TvShift{p.w, p.h});
    }
    Pass& m = p.add("tv_main", 0, 1, p.w, p.h);
    m.in1 = in1;
    m.table = t;
    m.setPush(P::TvMain{p.w, p.h, r.mix14, r.xStart, r.xStep, (r.phos ? 1u : 0u) | (r.staticOn ? 2u : 0u) | (r.jitter ? 4u : 0u), 0u});
    return true;
}

// ------------------------------------------------------------------ Film Effects (filmEffects() in builtins_more_creative.cpp, pieces in vfxfilm.h)
// The particle simulation keeps its state from frame to frame on the CPU; it runs here and writes the picture work of the frame down (vfxfilm::Rec): the grey, the grain's
// numbers, and in order the tints, jitters and scattered additions. The card repeats that work, each step as one pass.
bool planFilmEffects(Ctx& c, Plan& p)
{
    vfxfilm::Rec rec;
    bool bytes = true;
    more::filmEffectsGpu(c, (int)p.w, (int)p.h, rec, bytes);
    if (rec.ops.size() + 4 > 16000) return false;
    const uint32_t by = bytes ? 1u : 0u;
    int cur = 1;
    {
        Pass& l = p.add("filmfx_load", 0, 1, p.w, p.h);
        l.setPush(P::FilmfxLoad{p.w, p.h, by, rec.grey ? 1u : 0u});
    }
    if (rec.grainOn) {
        std::vector<float> tab;
        tab.reserve(rec.words.size());
        for (uint32_t w : rec.words) pushInt(tab, (int32_t)w);
        Pass& g = p.add("filmfx_grain", cur, cur, p.w, p.h);
        g.table = p.addTable(std::move(tab));
        g.setPush(P::FilmfxGrain{p.w, p.h, by, rec.grainK, rec.grainAmount, 0u});
    }
    int other = -1;
    for (size_t oi = 0; oi < rec.ops.size(); ++oi) {
        const vfxfilm::Rec::Op& o = rec.ops[oi];
        if (o.kind == 0) {
            // (c4: blue, green, red, strength) -> (red, green, blue)
            int ci[3];
            float cf[3];
            for (int k = 0; k < 3; ++k) {
                const double v = o.c4[2 - k] * 255.0;
                ci[k] = (int)(0 > v ? 0.0 : std::min(255.0, v));
                cf[k] = (float)(0 > o.c4[2 - k] ? 0.0 : std::min(1.0, o.c4[2 - k]));
            }
            const double va = o.c4[3] * 255.0;
            const int a8 = (int)(0 > va ? 0.0 : std::min(255.0, va));
            const float af = (float)(0 > o.c4[3] ? 0.0 : std::min(1.0, o.c4[3]));
            Pass& t = p.add("filmfx_tint", cur, cur, p.w, p.h);
            t.setPush(P::FilmfxTint{p.w, p.h, by, a8, ci[0], ci[1], ci[2], af, cf[0], cf[1], cf[2], 0u});
        } else if (o.kind == 1) {
            if (other < 0) other = p.addBuf(p.w, p.h, 4);
            Pass& j = p.add("filmfx_jitter", cur, other, p.w, p.h);
            j.setPush(P::FilmfxJitter{p.w, p.h, by, o.dx, o.dy, 0u});
            std::swap(cur, other);
        } else {
            // the additions of each pixel hit, in the order they were made
            std::vector<std::pair<uint32_t, uint32_t>> order;     // (pixel, event) sorted by pixel, then event
            order.reserve(o.last - o.first);
            for (size_t e = o.first; e < o.last; ++e) {
                const vfxfilm::Rec::Ev& ev = rec.events[e];
                order.push_back(std::make_pair((uint32_t)ev.y * p.w + (uint32_t)ev.x, (uint32_t)e));
            }
            std::sort(order.begin(), order.end());
            std::vector<float> heads, nums;
            uint32_t ng = 0;
            for (size_t i = 0; i < order.size();) {
                size_t j = i;
                while (j < order.size() && order[j].first == order[i].first) ++j;
                pushInt(heads, (int32_t)order[i].first);
                pushInt(heads, (int32_t)nums.size());
                pushInt(heads, (int32_t)(j - i));
                for (size_t k = i; k < j; ++k) {
                    const vfxfilm::Rec::Ev& ev = rec.events[order[k].second];
                    if (bytes) pushInt(nums, ev.di);
                    else nums.push_back(ev.df);
                }
                ++ng;
                i = j;
            }
            if (ng == 0) continue;
            heads.insert(heads.end(), nums.begin(), nums.end());
            Pass& s = p.add("filmfx_scatter", cur, cur, ng, 1);
            s.table = p.addTable(std::move(heads));
            s.setPush(P::FilmfxScatter{p.w, p.h, by, ng});
        }
    }
    if (cur != 1) {
        Pass& j = p.add("filmfx_jitter", cur, 1, p.w, p.h);
        j.setPush(P::FilmfxJitter{p.w, p.h, by, 0, 0, 0u});
    }
    return true;
}

// ------------------------------------------------------------------ Color Gradient (colorGradient() in builtins_gen.cpp)
bool planColorGradient(Ctx& c, Plan& p)
{
    ColorGradientGpu g;
    colorGradientGpu(c, g);
    std::vector<float> tab;
    for (double d : g.stops) pushD(tab, d);
    D2 v[11];
    for (int i = 0; i < 11; ++i) v[i] = d2(g.v[i]);
    P::Cgrad q = {p.w, p.h, (uint32_t)g.type, (uint32_t)(g.stops.size() / 5), v[0].h, v[0].l, v[1].h, v[1].l, v[2].h, v[2].l, v[3].h, v[3].l, v[4].h, v[4].l, v[5].h, v[5].l,
                  v[6].h, v[6].l, v[7].h, v[7].l, v[8].h, v[8].l, v[9].h, v[9].l, v[10].h, v[10].l};
    Pass& ps = p.add("cgrad", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(q);
    return true;
}

// ------------------------------------------------------------------ Film Grain (filmGrain() in builtins_more_creative.cpp)
bool planFilmGrain(Ctx& c, Plan& p)
{
    FilmGrainSetup g;
    more::filmGrainSetup(c, (int)p.w, (int)p.h, g);
    std::vector<float> tab;
    tab.reserve(24576 + (size_t)(p.w + p.h) * 6);
    for (double d : g.table) pushD(tab, d);
    for (uint32_t x = 0; x < p.w; ++x) {
        pushInt(tab, g.colA[x]);
        pushInt(tab, g.colB[x]);
        pushD(tab, g.colFrac[x]);
        pushD(tab, 1 - g.colFrac[x]);
    }
    for (uint32_t y = 0; y < p.h; ++y) {
        pushInt(tab, g.rowA[y] * 64);
        pushInt(tab, g.rowB[y] * 64);
        pushD(tab, g.frac[y]);
        pushD(tab, 1 - g.frac[y]);
    }
    const D2 ch = d2(g.chroma), om = d2(1 - g.chroma);
    P::FilmGrain q = {p.w, p.h, g.chroma > 0 ? 1u : 0u, c.levels ? 1u : 0u, g.wt, (float)g.amount, ch.h, ch.l, om.h, om.l};
    Pass& ps = p.add("film_grain", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(tab));
    ps.setPush(q);
    return true;
}

// ------------------------------------------------------------------ Light Rays (vfxlr::render in vfxlightrays.h)
bool planLightRays(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    vfxlr::Setup su;
    more::lightRaysGpu(c, W, H, su);
    // the quarters are drawn in diagonals (see lr_zone): count the passes first
    size_t total = 1;
    for (int zone = 0; zone < 4; ++zone) {
        if ((zone == 1 || zone == 3) && su.lxc == W - 1) continue;
        if ((zone == 2 || zone == 3) && su.lyc == H - 1) continue;
        const int cols = (zone & 1) ? W - su.lxc - 1 : su.lxc + 1, rows = (zone & 2) ? H - su.lyc - 1 : su.lyc + 1;
        total += (size_t)cols + rows - 1;
    }
    if (total > 16000) return false;        // (a frame so large that the passes do not fit in one plan: the CPU draws it)
    std::vector<float> seeds;
    if (su.noiseQ != 0) {
        seeds.resize(su.seeds.size());
        for (size_t i = 0; i < seeds.size(); ++i) std::memcpy(&seeds[i], &su.seeds[i], 4);
    } else {
        seeds.assign(1, 0.f);
    }
    const int tab = p.addTable(std::move(seeds));
    const int R = p.addBuf(p.w, p.h, 4);
    const D2 irw = d2(su.invRW), irh = d2(su.invRH), inner = d2(su.inner), fi = d2(su.featherInv);
    const uint32_t flags = (su.bound ? 1u : 0u) | (su.noiseQ != 0 ? 2u : 0u) | (su.zone1 ? 4u : 0u) | (su.zone3 ? 8u : 0u);
    for (int zone = 0; zone < 4; ++zone) {
        if ((zone == 1 || zone == 3) && su.lxc == W - 1) continue;
        if ((zone == 2 || zone == 3) && su.lyc == H - 1) continue;
        const int cols = (zone & 1) ? W - su.lxc - 1 : su.lxc + 1, rows = (zone & 2) ? H - su.lyc - 1 : su.lyc + 1;
        for (int t = 0; t <= cols + rows - 2; ++t) {
            const int amin = std::max(0, t - cols + 1), amax = std::min(t, rows - 1);
            const uint32_t cnt = (uint32_t)(amax - amin + 1);
            Pass& ps = p.add("lr_zone", 0, R, cnt, 1);
            ps.table = tab;
            ps.setPush(P::LrZone{p.w, p.h, (uint32_t)zone, (uint32_t)t, (uint32_t)amin, cnt, su.lxc, su.lyc, flags, su.sensQ, su.strQ, su.noiseQ, 0u, irw.h, irw.l, irh.h, irh.l, inner.h, inner.l, fi.h, fi.l});
        }
    }
    const bool tint = su.blendQ != 0 || su.tintR != 0 || su.tintG != 0 || su.tintB != 0;
    Pass& f = p.add("lr_final", 0, 1, p.w, p.h);
    f.in1 = R;
    f.setPush(P::LrFinal{p.w, p.h, su.lxc, su.lyc, (tint ? 2u : 0u) | (su.zone1 ? 4u : 0u) | (su.zone3 ? 8u : 0u), su.blendQ, su.tintR, su.tintG, su.tintB, 0u});
    return true;
}

}  // namespace

const std::vector<FilterGpuEntry>& fx2GpuEntries()
{
    static const std::vector<FilterGpuEntry> v = {
        {"com.vegascreativesoftware:tvsimulator", planTvSimulator, 0.0,
         {GpuCheckCase("plain", PM{{"ApertureGrill", {0.0}}, {"Interlacing", {0.0}}, {"Phosphorescence", {0.0}}, {"ScanPhasing", {0.0}}}),
          GpuCheckCase("zoom", PM{{"DetailZoom", {0.5}}}), GpuCheckCase("zoom full", PM{{"DetailZoom", {1.0}}, {"ScanPhasing", {0.0}}}),
          GpuCheckCase("static", PM{{"Static", {0.4}}}), GpuCheckCase("static full", PM{{"Static", {1.0}}, {"ApertureGrill", {0.1}}, {"Phosphorescence", {0.0}}}),
          GpuCheckCase("grill", PM{{"ApertureGrill", {1.0}}, {"Interlacing", {1.0}}}), GpuCheckCase("phosphorescence", PM{{"Phosphorescence", {1.0}}}),
          GpuCheckCase("line sync", PM{{"LineSync", {0.9}}}), GpuCheckCase("line sync half", PM{{"LineSync", {0.5}}, {"DetailZoom", {0.3}}}),
          GpuCheckCase("line sync none", PM{{"LineSync", {0.0}}}),
          GpuCheckCase("vertical sync", PM{{"VerticalSync", {0.5}}, {"__time", {37}}}), GpuCheckCase("vertical sync none", PM{{"VerticalSync", {0.0}}, {"__time", {11}}}),
          GpuCheckCase("scan phasing", PM{{"ScanPhasing", {1.0}}, {"__time", {23}}}), GpuCheckCase("scan phasing zoom", PM{{"ScanPhasing", {0.5}}, {"DetailZoom", {0.6}}, {"__time", {5}}}),
          GpuCheckCase("everything", PM{{"LineSync", {0.3}}, {"VerticalSync", {0.7}}, {"ScanPhasing", {0.8}}, {"Static", {0.6}}, {"Phosphorescence", {0.5}}, {"DetailZoom", {0.2}}, {"__time", {123}}}),
          GpuCheckCase("late", PM{{"LineSync", {0.6}}, {"Static", {0.2}}, {"__time", {4000}}})}},
        {"com.vegascreativesoftware:lightrays", planLightRays, 0.0,
         {GpuCheckCase("strong", PM{{"group0Strength", {0.8}}, {"group0Sensitivity", {0.3}}}), GpuCheckCase("off centre", PM{{"LightSource", {0.2, 0.7}}, {"group0Strength", {0.6}}}),
          GpuCheckCase("corner", PM{{"LightSource", {0.0, 0.0}}, {"group0Strength", {0.7}}}), GpuCheckCase("opposite corner", PM{{"LightSource", {1.0, 1.0}}, {"group0Strength", {0.7}}}),
          GpuCheckCase("right edge", PM{{"LightSource", {1.0, 0.4}}, {"group0Strength", {0.5}}}), GpuCheckCase("bottom edge", PM{{"LightSource", {0.3, 0.0}}, {"group0Strength", {0.5}}}),
          GpuCheckCase("top edge", PM{{"LightSource", {0.6, 1.0}}, {"group0Strength", {0.5}}}), GpuCheckCase("left edge", PM{{"LightSource", {0.0, 0.5}}, {"group0Strength", {0.5}}}),
          GpuCheckCase("outside", PM{{"LightSource", {1.4, -0.3}}, {"group0Strength", {0.5}}}),
          GpuCheckCase("radius", PM{{"group1BoundRadius", {1}}, {"group1RadiusX", {0.3}}, {"group1RadiusY", {0.2}}, {"group1Feather", {0.4}}, {"group0Strength", {0.6}}}),
          GpuCheckCase("radius hard", PM{{"group1BoundRadius", {1}}, {"group1RadiusX", {0.5}}, {"group1RadiusY", {0.5}}, {"group1Feather", {0.0}}, {"LightSource", {0.7, 0.3}}}),
          GpuCheckCase("radius soft", PM{{"group1BoundRadius", {1}}, {"group1Feather", {1.0}}, {"group1RadiusX", {0.8}}, {"group1RadiusY", {0.6}}}),
          GpuCheckCase("tint", PM{{"TintColor", {0.9, 0.5, 0.2}}, {"group2Blend", {0.3}}}), GpuCheckCase("blend", PM{{"group2Blend", {0.8}}, {"group0Strength", {0.9}}}),
          GpuCheckCase("black tint", PM{{"TintColor", {0.0, 0.0, 0.0}}, {"group2Blend", {0.0}}}),
          GpuCheckCase("noise", PM{{"group2Noise", {0.6}}, {"group0Strength", {0.5}}}), GpuCheckCase("noise corner", PM{{"group2Noise", {1.0}}, {"LightSource", {1.0, 0.0}}, {"__time", {9}}}),
          GpuCheckCase("no strength", PM{{"group0Strength", {0.0}}}), GpuCheckCase("full sensitivity", PM{{"group0Sensitivity", {1.0}}, {"group0Strength", {1.0}}}),
          GpuCheckCase("no sensitivity", PM{{"group0Sensitivity", {0.0}}, {"group0Strength", {0.5}}})}},
        {"com.vegascreativesoftware:filmgrain", planFilmGrain, 0.0,
         {GpuCheckCase("fine", PM{{"Amount", {0.8}}, {"Granularity", {0.0}}}), GpuCheckCase("coarse", PM{{"Amount", {1.0}}, {"Granularity", {1.0}}}),
          GpuCheckCase("mid", PM{{"Amount", {0.5}}, {"Granularity", {0.3}}}), GpuCheckCase("chroma", PM{{"Chormaticity", {1.0}}, {"Amount", {0.7}}}),
          GpuCheckCase("chroma half", PM{{"Chormaticity", {0.35}}, {"Granularity", {0.6}}}), GpuCheckCase("no amount", PM{{"Amount", {0.0}}, {"Chormaticity", {0.5}}}),
          GpuCheckCase("tiny amount", PM{{"Amount", {0.004}}}), GpuCheckCase("another time", PM{{"Amount", {0.6}}, {"__time", {17}}}),
          GpuCheckCase("chroma another time", PM{{"Amount", {0.6}}, {"Chormaticity", {0.8}}, {"Granularity", {0.9}}, {"__time", {250}}})}},
        {"com.vegascreativesoftware:filmeffects", planFilmEffects, 0.0,
         {GpuCheckCase("grain", PM{{"Grain", {0.6}}}), GpuCheckCase("grain full", PM{{"Grain", {1.0}}}), GpuCheckCase("tint", PM{{"Tint", {0.5}}, {"Color", {0.2, 0.6, 0.9}}}),
          GpuCheckCase("tint full", PM{{"Tint", {1.0}}, {"Color", {1.0, 0.8, 0.1}}}), GpuCheckCase("grey", PM{{"ConvertToGreyscale", {1}}}),
          GpuCheckCase("grey grain tint", PM{{"ConvertToGreyscale", {1}}, {"Grain", {0.3}}, {"Tint", {0.4}}}),
          GpuCheckCase("dust", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {1.0}}}),
          GpuCheckCase("dust little", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {0.3}}, {"__time", {4}}}),
          GpuCheckCase("flicker", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {1}}, {"EnableParticlesAmount", {1.0}}}),
          GpuCheckCase("hair", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {2}}, {"EnableParticlesAmount", {1.0}}}),
          GpuCheckCase("jitter", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {3}}, {"EnableParticlesAmount", {1.0}}}),
          GpuCheckCase("jitter later", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {3}}, {"EnableParticlesAmount", {0.7}}, {"__time", {9}}}),
          GpuCheckCase("scratches", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {4}}, {"EnableParticlesAmount", {1.0}}}),
          GpuCheckCase("scratches later", PM{{"EnableEnableParticles", {1}}, {"EnableParticlesType", {4}}, {"EnableParticlesAmount", {0.5}}, {"__time", {25}}}),
          GpuCheckCase("very old film", PM{{"Grain", {0.15}}, {"Tint", {0.25}}, {"Color", {0.75, 0.716875, 0.3525}}, {"ConvertToGreyscale", {1}}, {"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {0.5}}}),
          GpuCheckCase("circa 1908", PM{{"Grain", {0.25}}, {"Tint", {0.35}}, {"Color", {0.74902, 0.717647, 0.352941}}, {"ConvertToGreyscale", {1}}, {"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {0.5}}, {"__time", {7}}}),
          GpuCheckCase("low quality colour", PM{{"Grain", {0.15}}, {"Tint", {0.35}}, {"Color", {0.352941, 0.717647, 0.74902}}, {"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {0.2}}, {"__time", {2}}}),
          GpuCheckCase("circa 1980", PM{{"Grain", {0.1}}, {"Tint", {0.2}}, {"Color", {0.352941, 0.717647, 0.74902}}, {"EnableEnableParticles", {1}}, {"EnableParticlesType", {0}}, {"EnableParticlesAmount", {0.1}}, {"__time", {30}}}),
          GpuCheckCase("everything", PM{{"Grain", {0.5}}, {"Tint", {0.3}}, {"ConvertToGreyscale", {1}}, {"EnableEnableParticles", {1}}, {"EnableParticlesType", {3}}, {"EnableParticlesAmount", {1.0}}, {"__time", {3}}})}},
        {"com.vegascreativesoftware:colorgradient", planColorGradient, 0.0,
         {GpuCheckCase("linear", PM{{"Start", {0.1, 0.2}}, {"End", {0.8, 0.7}}}), GpuCheckCase("linear colours", PM{{"Color1", {0.9, 0.2, 0.1, 1.0}}, {"Color2", {0.1, 0.3, 0.8, 0.4}}}),
          GpuCheckCase("linear no length", PM{{"Start", {0.4, 0.4}}, {"End", {0.4, 0.4}}}), GpuCheckCase("vertical", PM{{"Start", {0.5, 1.0}}, {"End", {0.5, 0.0}}}),
          GpuCheckCase("elliptical", PM{{"GradientType", {1}}, {"Radius", {0.4, 0.3}}}), GpuCheckCase("elliptical turned", PM{{"GradientType", {1}}, {"AspectRatioAngle", {35}}, {"Center", {0.3, 0.6}}, {"Radius", {0.5, 0.2}}}),
          GpuCheckCase("elliptical thin", PM{{"GradientType", {1}}, {"Radius", {0.0, 0.0}}}), GpuCheckCase("rectangular", PM{{"GradientType", {2}}, {"Radius", {0.3, 0.45}}}),
          GpuCheckCase("rectangular turned", PM{{"GradientType", {2}}, {"AspectRatioAngle", {-70}}, {"Center", {0.7, 0.4}}}),
          GpuCheckCase("three stops", PM{}, {{"GradientStops", "0 1 0 0 1; 0.5 0 1 0 0.5; 1 0 0 1 1"}}),
          GpuCheckCase("stops inside", PM{{"GradientType", {1}}}, {{"GradientStops", "0.25 1 0 0 1; 0.75 0 0 1 0.3"}}),
          GpuCheckCase("same positions", PM{{"GradientType", {2}}}, {{"GradientStops", "0.5 1 0 0 1; 0.5 0 1 0 1; 1 0 0 1 1"}}),
          GpuCheckCase("single stop", PM{}, {{"GradientStops", "0.5 0.2 0.8 0.4 0.6"}}),
          GpuCheckCase("nine stops", PM{{"Start", {0.0, 0.0}}, {"End", {1.0, 1.0}}}, {{"GradientStops", "0 0 0 0 1; 0.1 0.9 0.1 0.1 1; 0.2 0.1 0.9 0.1 0.7; 0.3 0.1 0.1 0.9 1; 0.45 1 1 0 0.4; 0.55 0 1 1 1; 0.7 1 0 1 1; 0.85 0.5 0.5 0.5 0.9; 1 1 1 1 1"}})}},
    };
    return v;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
