// Vulkan plans of the geometry effects whose set-up lives in builtins_distort.cpp / builtins_stereo.cpp: Swirl, Pinch/Punch, Wave and Stereoscopic 3D Adjust.
// Each kernel (tools/gpukernels_geom.py) is the CPU code (vfxswirl.h, vfxwave.h, vfxstereo.h) in single and software-double arithmetic.
#include "vkfilters.h"
#include "vkshaders/push_layouts.h"
#include <cmath>
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

bool planSwirlPinch(Ctx& c, Plan& p, uint32_t mode)
{
    SwirlAreaGpu a;
    swirlAreaGpu(c, (int)p.w, (int)p.h, a);
    // (swirl8 / swirlF multiply the amount by 15 inside; pinch uses it as it is)
    const double amt = mode == 0 ? a.amount * 15.0 : a.amount;
    const D2 k13 = d2(a.k13), sy = d2(a.syInv), rx = d2(a.rx), ry = d2(a.ry), am = d2(amt);
    P::SwirlpinchVf q = {p.w, p.h, (c.host8 || c.levels) ? 1u : 0u, mode, a.cx, a.cy, k13.h, k13.l, sy.h, sy.l, rx.h, rx.l, ry.h, ry.l, am.h, am.l};
    p.add("swirlpinch_vf", 0, 1, p.w, p.h).setPush(q);
    return true;
}
bool planSwirl(Ctx& c, Plan& p) { return planSwirlPinch(c, p, 0); }
bool planPinch(Ctx& c, Plan& p) { return planSwirlPinch(c, p, 1); }

bool planWave(Ctx& c, Plan& p)
{
    double W6[6], k[4];
    if (!waveGpu(c, (int)p.w, (int)p.h, W6)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    waveKernelConstants(W6, (int)p.w, (int)p.h, k);
    const D2 ky = d2(k[0]), kx = d2(k[1]), ax = d2(k[2]), ay = d2(k[3]), p4 = d2(W6[4]), p5 = d2(W6[5]);
    P::WaveVf q = {p.w, p.h, (c.host8 || c.levels) ? 1u : 0u, 0u, ky.h, ky.l, kx.h, kx.l, ax.h, ax.l, ay.h, ay.l, p4.h, p4.l, p5.h, p5.l};
    p.add("wave_vf", 0, 1, p.w, p.h).setPush(q);
    return true;
}

bool planDeform(Ctx& c, Plan& p)
{
    DeformGpu d;
    if (!deformGpu(c, (int)p.w, (int)p.h, d)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    P::DeformVf q = {p.w, p.h, d.q8 ? 1u : 0u, d.mode, d.flags, d.bx0, d.by0, d.ow, d.oh, d.bg[0], d.bg[1], d.bg[2], d.bg[3]};
    Pass& ps = p.add("deform_vf", 0, 1, p.w, p.h);
    ps.table = p.addTable(std::move(d.tab));
    ps.setPush(q);
    return true;
}

}  // namespace

bool planStereo3dAdjust(Ctx& c, Plan& p)
{
    StereoGpu s;
    if (!stereoGpu(c, (int)p.w, (int)p.h, s)) {
        p.add("copy_px", 0, 1, p.w, p.h).setPush(P::CopyPx{p.w, p.h});
        return true;
    }
    D2 m[9];
    for (int i = 0; i < 9; ++i) m[i] = d2(s.mi[i]);
    const D2 pt = d2(s.part);
    P::StereoWarp q = {p.w, p.h, s.levels ? 1u : 0u, s.full, m[0].h, m[0].l, m[1].h, m[1].l, m[2].h, m[2].l, m[3].h, m[3].l, m[4].h, m[4].l, m[5].h, m[5].l, m[6].h, m[6].l, m[7].h, m[7].l, m[8].h, m[8].l, pt.h, pt.l};
    p.add("stereo_warp", 0, 1, p.w, p.h).setPush(q);
    return true;
}

const std::vector<FilterGpuEntry>& geomGpuEntries()
{
    static const std::vector<FilterGpuEntry> v = {
        {"com.vegascreativesoftware:swirl", planSwirl, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("strong", PM{{"Amount", {1.0}}}), GpuCheckCase("negative", PM{{"Amount", {-0.6}}, {"Center", {0.3, 0.6}}}),
          GpuCheckCase("big off centre", PM{{"Amount", {0.4}}, {"ScalingHorizontal", {2.0}}, {"Center", {0.8, 0.2}}}), GpuCheckCase("not proportional", PM{{"ScalingProportional", {0}}, {"ScalingHorizontal", {0.5}}, {"ScalingVertical", {1.5}}}),
          GpuCheckCase("small", PM{{"ScalingHorizontal", {0.2}}, {"Amount", {0.9}}}), GpuCheckCase("zero", PM{{"Amount", {0.0}}}), GpuCheckCase("no area", PM{{"ScalingHorizontal", {0.0}}, {"ScalingProportional", {1}}})}},
        {"com.vegascreativesoftware:pinchpunch", planPinch, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("punch", PM{{"Amount", {-1.0}}}), GpuCheckCase("pinch off centre", PM{{"Amount", {0.5}}, {"Center", {0.3, 0.7}}}),
          GpuCheckCase("big", PM{{"Amount", {0.8}}, {"ScalingHorizontal", {2.0}}}), GpuCheckCase("not proportional", PM{{"ScalingProportional", {0}}, {"ScalingHorizontal", {0.6}}, {"ScalingVertical", {1.4}}, {"Amount", {-0.5}}}),
          GpuCheckCase("zero", PM{{"Amount", {0.0}}}), GpuCheckCase("no area", PM{{"ScalingHorizontal", {0.0}}})}},
        {"com.vegascreativesoftware:wave", planWave, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("waves", PM{{"VerticalWaves", {3}}, {"HorizontalWaves", {4}}, {"VerticalAmplitude", {0.05}}, {"HorizontalAmplitude", {0.05}}}),
          GpuCheckCase("phase", PM{{"VerticalWaves", {2.5}}, {"HorizontalWaves", {1.5}}, {"VerticalAmplitude", {0.1}}, {"HorizontalAmplitude", {-0.08}}, {"VerticalPhase", {1.3}}, {"HorizontalPhase", {-2.0}}}),
          GpuCheckCase("big", PM{{"VerticalWaves", {40}}, {"HorizontalWaves", {25}}, {"VerticalAmplitude", {1.0}}, {"HorizontalAmplitude", {1.0}}}), GpuCheckCase("horizontal only", PM{{"VerticalWaves", {0}}, {"HorizontalWaves", {6}}, {"HorizontalAmplitude", {0.2}}}),
          GpuCheckCase("negative", PM{{"VerticalAmplitude", {-1.0}}, {"VerticalWaves", {5}}})}},
        {"deform", planDeform, 0.0,
         {GpuCheckCase("default", PM{}), GpuCheckCase("shear", PM{{"Amount", {1.0}}, {"ShearHorizontal", {0.4}}, {"ShearVertical", {0.25}}}),
          GpuCheckCase("squeeze", PM{{"Amount", {0.8}}, {"SqueezeLeft", {0.3}}, {"SqueezeTop", {0.2}}, {"SqueezeRight", {-0.2}}, {"SqueezeBottom", {0.5}}}),
          GpuCheckCase("curve", PM{{"Amount", {1.0}}, {"CurveTop", {0.5}}, {"CurveBottom", {0.3}}, {"CurveLeft", {0.4}}, {"CurveRight", {0.2}}}),
          GpuCheckCase("curve y only", PM{{"Amount", {1.0}}, {"CurveTop", {0.7}}, {"CurveBottom", {0.1}}}), GpuCheckCase("curve x only", PM{{"Amount", {0.6}}, {"CurveLeft", {0.8}}}),
          GpuCheckCase("all", PM{{"Amount", {1.0}}, {"ShearHorizontal", {0.2}}, {"ShearVertical", {0.1}}, {"SqueezeLeft", {0.3}}, {"SqueezeTop", {0.2}}, {"CurveTop", {0.4}}, {"CurveLeft", {0.3}}}),
          GpuCheckCase("centred", PM{{"Amount", {1.0}}, {"ShearHorizontal", {0.5}}, {"CenterImage", {1}}}),
          GpuCheckCase("full squeeze", PM{{"Amount", {1.0}}, {"SqueezeTop", {1.0}}, {"SqueezeLeft", {1.0}}}), GpuCheckCase("full both", PM{{"Amount", {1.0}}, {"SqueezeTop", {1.0}}, {"SqueezeBottom", {1.0}}}),
          GpuCheckCase("full curve", PM{{"Amount", {1.0}}, {"CurveTop", {1.0}}, {"CurveBottom", {1.0}}, {"CurveLeft", {1.0}}, {"CurveRight", {1.0}}}),
          GpuCheckCase("background", PM{{"Amount", {1.0}}, {"ShearHorizontal", {0.6}}, {"Background", {0.2, 0.5, 0.9, 0.7}}}), GpuCheckCase("zero amount", PM{{"Amount", {0.0}}, {"ShearHorizontal", {0.6}}})}},
    };
    return v;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
