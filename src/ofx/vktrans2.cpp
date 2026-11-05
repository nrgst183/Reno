// The older (DirectX-era) VEGAS transitions on the GPU: for each, the planner works out on the CPU what does not depend on the pictures exactly as the CPU code does
// (builtins_trans.cpp, vfxvenetian.h, vfxportals.h, vfxgradwipe.h: the same functions are called) and hands it to the kernel (tools/gpukernels_trans2.py), which
// does the per-pixel part. The frame's result is identical to the CPU's in 8-bit and float pictures.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "vktrans2.h"
#include "vfxvenetian.h"
#include "vfxportals.h"
#include "vfxgradwipe.h"
#include "vkfilters.h"
#include "builtins_deriche.h"
#include "vkshaders/push_layouts.h"

namespace reno {
namespace ofx {
namespace builtin {

std::string gradientWipeDllPath();      // builtins_vegasrest.cpp: where Gradient Wipe's pictures are read from
int gradientWipeShape(int vi);          // (and the procedural gradient used without them)

namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;
typedef std::map<std::string, std::vector<double>> PM;

const char* const kPrefix = "com.vegascreativesoftware:";
std::string fullId(const char* id) { return std::string(kPrefix) + id; }

// The incoming frame goes up with the plan as the first w * h * 4 floats of a table (top row first, as Ctx::src2 holds it), then 'tail' (numbers for the kernel).
// Returns the table's index, -1 when the plan cannot have it.
int toTableTail(Ctx& c, Plan& p, std::vector<float> tail)
{
    if (c.src2Bytes) return p.addFrameTable(c.src2Bytes, c.src2RowBytes, c.src2Bgra, std::move(tail));
    if (c.src2.px.size() != (size_t)p.w * p.h * 4) return -1;
    std::vector<float> t = std::move(c.src2.px);
    t.insert(t.end(), tail.begin(), tail.end());
    return p.addTable(std::move(t));
}

// the words of a push block: a double is two words, high half first (the kernels' SF(S, hi, lo))
struct Words {
    uint32_t q[8];
    Words() { std::memset(q, 0, sizeof(q)); }
    void d(int k, double v)
    {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        q[k] = (uint32_t)(b >> 32);
        q[k + 1] = (uint32_t)b;
    }
};

// an int kept in a table as the bits of a float
inline float bitsOf(int v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// ---------------------------------------------------------------------------------------------------- Venetian Blinds (venetian() in builtins_trans.cpp)
}  // namespace

bool planVenetian(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    if (W <= 0 || H <= 0) return false;
    vfxvb::Params vp;
    vp.blinds = std::max(1.0, c.d("NumberOfBlinds"));
    vp.angleDeg = c.d("Angle");
    vp.feather = c.d("Feather");
    vp.progress = c.progress;
    const vfxvb::Setup s = vfxvb::setup(vp, W, H);
    std::vector<uint8_t> tbl = vfxvb::profile(vp, s);
    if (s.blur && s.progress14 != 0 && s.n > 0) {
        Buf b;
        b.resize(s.n, 1);
        for (int i = 0; i < s.n; ++i) b.at(i, 0)[0] = tbl[(size_t)i] / 255.f;
        vegasDericheBlur(b, s.blurSize, 0.0, true);
        for (int i = 0; i < s.n; ++i) tbl[(size_t)i] = (uint8_t)std::min(255, std::max(0, (int)std::lrintf(b.at(i, 0)[0] * 255.f)));
    }
    // (the position of a pixel is its row's part less its column's: the CPU's rowBase and the double product of positionAt, per row and per column)
    std::vector<float> tail;
    tail.reserve((size_t)H + (size_t)W + tbl.size());
    for (int y = 0; y < H; ++y) tail.push_back(bitsOf(vfxvb::rowBase(s, y)));
    for (int x = 0; x < W; ++x) {
        const int r14 = vfxvb::add32(-(s.w << 13), x * 0x4000);
        tail.push_back(bitsOf(vfxvb::cvt32((double)r14 * (double)s.cosI * -6.103515625e-05)));
    }
    for (uint8_t v : tbl) tail.push_back((float)v);
    const int t = toTableTail(c, p, std::move(tail));
    if (t < 0) return false;
    const uint32_t base = (uint32_t)W * (uint32_t)H * 4u;
    P::Venetian pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    pc.levels = c.levels ? 1u : 0u;
    pc.n = (int)tbl.size();
    pc.rowOff = base;
    pc.colOff = base + (uint32_t)H;
    pc.tblOff = base + (uint32_t)H + (uint32_t)W;
    Pass& ps = p.add("venetian", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Portals (portalsFx() in builtins_trans.cpp)
bool planPortals(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    if (W <= 0 || H <= 0) return false;
    vfxportals::Props pr;
    pr.seed = c.i("RandomPatternSeed");
    pr.squares = std::min(256, std::max(0, c.i("Squares")));
    pr.maxTransparency = clampd(c.d("MaxTransparency"), 0, 1);
    pr.maxOffset = clampd(c.d("MaxOffset"), 0, 1);
    pr.maxScale = clampd(c.d("MaxScale"), 0, 1);
    pr.borderSize = clampd(c.d("BorderSize"), 0, 1);
    const Color bc = c.color("BorderColor");
    pr.borderColor[0] = (float)clampd(bc.r, 0, 1);
    pr.borderColor[1] = (float)clampd(bc.g, 0, 1);
    pr.borderColor[2] = (float)clampd(bc.b, 0, 1);
    // (the same layout, stretches and borders as the CPU draws, as a list: the header's render() with a list to fill draws nothing)
    std::vector<vfxportals::Op> ops;
    vfxportals::render(pr, clampd(c.progress, 0, 1), 1.0, nullptr, nullptr, W, H, nullptr, &ops);
    if (ops.empty()) return false;
    const int kW = 20;
    std::vector<float> tail;
    tail.reserve(ops.size() * kW);
    for (const vfxportals::Op& o : ops) {
        const int v[kW] = {o.mix, o.src, o.px0, o.px1, o.py0, o.py1, o.firstRowCov, o.lastRowCov, o.firstColCov, o.lastColCov, o.sx0, o.sy0, o.xstep, o.ystep, o.step1, o.syStart,
                           o.sw1, o.sh1, o.alpha14, (int)((uint32_t)o.cpx[0] | ((uint32_t)o.cpx[1] << 8) | ((uint32_t)o.cpx[2] << 16) | ((uint32_t)o.cpx[3] << 24))};
        for (int k = 0; k < kW; ++k) tail.push_back(bitsOf(v[k]));
    }
    const int t = toTableTail(c, p, std::move(tail));
    if (t < 0) return false;
    P::Portals pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    pc.nops = (uint32_t)ops.size();
    pc.opOff = (uint32_t)W * (uint32_t)H * 4u;
    Pass& ps = p.add("portals", 0, 1, p.w, p.h);
    ps.table = t;
    ps.setPush(pc);
    return true;
}

// ---------------------------------------------------------------------------------------------------- Gradient Wipe (gradientWipe() in builtins_vegasrest.cpp)
namespace {

// The Gaussian of gaussianBlurPlane() on a plane buffer: across (sigmaX), then down (sigmaY); an axis with sigma 0.3 or less is left out. Returns the buffer holding the result.
int addPlaneBlur(Plan& p, int from, double sigmaX, double sigmaY)
{
    const uint32_t W = p.bufs[(size_t)from].w, H = p.bufs[(size_t)from].h;
    const bool bx = sigmaX > 0.3, by = sigmaY > 0.3;
    int cur = from;
    if (bx) {
        const std::vector<float> k = gaussKernelTable(sigmaX);
        const int dst = p.addBuf(W, H, 1);
        Pass& ps = p.add("blurp_h", cur, dst, W, H);
        ps.table = p.addTable(k);
        P::BlurpH pc = {W, H, (uint32_t)k.size() - 1};
        ps.setPush(pc);
        cur = dst;
    }
    if (by) {
        const std::vector<float> k = gaussKernelTable(sigmaY);
        const int dst = p.addBuf(W, H, 1);
        Pass& ps = p.add("blurp_v", cur, dst, W, H);
        ps.table = p.addTable(k);
        P::BlurpV pc = {W, H, (uint32_t)k.size() - 1};
        ps.setPush(pc);
        cur = dst;
    }
    return cur;
}

// the last pass: the mask plane 'mask' and the two frames mixed
bool addGradientMix(Ctx& c, Plan& p, int mask, bool blurred, bool hf, bool vf, bool hm, bool vm)
{
    const int t = toTableTail(c, p, std::vector<float>());
    if (t < 0) return false;
    P::GwMix pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    pc.levels = c.levels ? 1u : 0u;
    pc.blurred = blurred ? 1u : 0u;
    pc.hf = hf; pc.vf = vf; pc.hm = hm; pc.vm = vm;
    Pass& ps = p.add("gw_mix", 0, 1, p.w, p.h);
    ps.in1 = mask;
    ps.table = t;
    ps.setPush(pc);
    return true;
}

// the taps of a stretch for the kernels: for every output index its first source index, tap count and the offset of its weights, then the weights
void addTaps(std::vector<float>& t, const std::vector<vgw::StretchSetup::Tap>& taps)
{
    std::vector<float> w;
    for (const auto& tp : taps) t.push_back((float)tp.first);
    for (const auto& tp : taps) t.push_back((float)tp.w.size());
    for (const auto& tp : taps) {
        t.push_back((float)w.size());
        w.insert(w.end(), tp.w.begin(), tp.w.end());
    }
    t.insert(t.end(), w.begin(), w.end());
}

}  // namespace

bool planGradientWipe(Ctx& c, Plan& p)
{
    const int W = (int)p.w, H = (int)p.h;
    if (W <= 0 || H <= 0) return false;
    const int vi = std::max(0, std::min(27, c.i("Gradient")));
    const int shape = gradientWipeShape(vi);
    const bool hf = c.b("HorizontalFlip"), vf = c.b("VerticalFlip"), hm = c.b("HorizontalMirror"), vm = c.b("VerticalMirror");
    const bool reverse = c.i("Direction") == 1, stretch = c.b("AutoContrastStretch"), keepAspect = c.b("MaintainAspectRatio");
    const double blend = clampd(c.d("ThresholdBlend"), 0, 1) * 0.5, blur = clampd(c.d("GradientBlur"), 0, 1);
    const double prog = clampd(c.progress, 0, 1);
    if (const std::shared_ptr<const vgw::Picture> pic = vgw::loadPicture(gradientWipeDllPath(), vi)) {
        // VEGAS's own picture: the canvas, the threshold table and the stretch's taps as the CPU makes them; the card does the threshold and the stretch
        vgw::Settings st;
        st.progress = prog;
        st.blend = blend;
        st.reverse = reverse;
        st.autoContrast = stretch;
        st.keepAspect = vgw::desc(vi).keepAspect != keepAspect;
        std::vector<uint8_t> unused;
        double visW = W, visH = H;
        vgw::Canvas cv;
        vgw::frameMask(*pic, st, W, H, unused, &visW, &visH, &cv);
        vgw::StretchSetup ss;
        vgw::stretchMean(std::vector<uint8_t>(), cv.w, cv.h, cv.x0, cv.y0, cv.vw, cv.vh, W, H, unused, &ss);
        if (cv.w <= 0 || cv.h <= 0 || (!ss.whole && ss.ry1 < ss.ry0)) return false;
        uint8_t lut[256];
        vgw::maskTable(st.progress, st.blend, st.reverse, lut);
        std::vector<float> t;
        for (int g = 0; g < 256; ++g) t.push_back((float)lut[g]);
        const uint32_t cvOff = (uint32_t)t.size();
        for (size_t i = 0; i < cv.bytes.size(); i += 3) {
            uint32_t v = cv.bytes[i];
            if (i + 1 < cv.bytes.size()) v |= (uint32_t)cv.bytes[i + 1] << 8;
            if (i + 2 < cv.bytes.size()) v |= (uint32_t)cv.bytes[i + 2] << 16;
            t.push_back((float)v);
        }
        uint32_t tx0 = 0, ty0 = 0;
        if (!ss.whole) {
            tx0 = (uint32_t)t.size();
            addTaps(t, ss.tx);
            ty0 = (uint32_t)t.size();
            addTaps(t, ss.ty);
        }
        const int tab = p.addTable(std::move(t));
        int mask = p.addBuf((uint32_t)W, (uint32_t)H, 1);
        if (ss.whole) {
            P::GwWhole pc;
            std::memset(&pc, 0, sizeof(pc));
            pc.w = p.w; pc.h = p.h;
            pc.cvOff = cvOff; pc.cw = (uint32_t)cv.w; pc.ch = (uint32_t)cv.h;
            pc.n = (uint32_t)std::max(0, ss.n); pc.m = (uint32_t)std::max(0, ss.m);
            pc.ix = ss.ix; pc.iy = ss.iy;
            if (pc.n == 0 || pc.m == 0) return false;
            Pass& ps = p.add("gw_whole", 0, mask, p.w, p.h);
            ps.table = tab;
            ps.setPush(pc);
        } else {
            const uint32_t rows = (uint32_t)(ss.ry1 - ss.ry0 + 1);
            const int mid = p.addBuf((uint32_t)W, rows, 1);
            P::GwMid pm;
            std::memset(&pm, 0, sizeof(pm));
            pm.w = p.w; pm.h = rows;
            pm.cvOff = cvOff; pm.cw = (uint32_t)cv.w; pm.ch = (uint32_t)cv.h;
            pm.tx0 = tx0; pm.ry0 = ss.ry0;
            Pass& a = p.add("gw_mid", 0, mid, p.w, rows);
            a.table = tab;
            a.setPush(pm);
            P::GwTaps pt;
            std::memset(&pt, 0, sizeof(pt));
            pt.w = p.w; pt.h = p.h;
            pt.ty0 = ty0; pt.ry0 = ss.ry0; pt.ry1 = ss.ry1;
            Pass& b = p.add("gw_taps", mid, mask, p.w, p.h);
            b.table = tab;
            b.setPush(pt);
        }
        const bool blurred = blur > 0.001;
        if (blurred) {
            const double sp = vgw::blurSigma(blur, std::max(visW, visH));
            mask = addPlaneBlur(p, mask, sp * W / std::max(1.0, visW), sp * H / std::max(1.0, visH));
        }
        return addGradientMix(c, p, mask, blurred, hf, vf, hm, vm);
    }
    // Reno's own gradients: the procedural one in doubles, the blur, the contrast stretch (the plane's smallest and largest value), and the mix
    const double aspect = keepAspect ? (double)W / std::max(1, H) : 1.0;
    int cur = p.addBuf((uint32_t)W, (uint32_t)H, 1);
    {
        Words k;
        k.d(0, aspect);
        P::GwProc pc;
        std::memset(&pc, 0, sizeof(pc));
        pc.w = p.w; pc.h = p.h;
        pc.shape = (uint32_t)shape;
        pc.hf = hf; pc.vf = vf; pc.hm = hm; pc.vm = vm; pc.keep = keepAspect ? 1u : 0u;
        pc.q0 = k.q[0]; pc.q1 = k.q[1];
        p.add("gw_proc", 0, cur, p.w, p.h).setPush(pc);
    }
    if (blur > 0.001) cur = addPlaneBlur(p, cur, blur * 0.1 * W, blur * 0.1 * W);
    if (stretch) {
        const int rm = p.addBuf(2, (uint32_t)H, 1);
        P::GwRowmm pr = {1u, p.h, p.w};
        p.add("gw_rowmm", cur, rm, 1, p.h).setPush(pr);
        const int mm = p.addBuf(2, 1, 1);
        P::GwMm pq = {p.h, 1u};
        p.add("gw_mm", rm, mm, 1, 1).setPush(pq);
        const int nb = p.addBuf((uint32_t)W, (uint32_t)H, 1);
        P::GwNorm pn = {p.w, p.h};
        Pass& ps = p.add("gw_norm", cur, nb, p.w, p.h);
        ps.in1 = mm;
        ps.setPush(pn);
        cur = nb;
    }
    const int t = toTableTail(c, p, std::vector<float>());
    if (t < 0) return false;
    Words k;
    k.d(0, prog);
    k.d(2, blend);
    P::GwPmix pc;
    std::memset(&pc, 0, sizeof(pc));
    pc.w = p.w; pc.h = p.h;
    pc.reverse = reverse ? 1u : 0u;
    pc.soft = blend > 1e-4 ? 1u : 0u;
    pc.q0 = k.q[0]; pc.q1 = k.q[1]; pc.q2 = k.q[2]; pc.q3 = k.q[3];
    Pass& ps = p.add("gw_pmix", 0, 1, p.w, p.h);
    ps.in1 = cur;
    ps.table = t;
    ps.setPush(pc);
    return true;
}

namespace {

struct CaseSet { const char* id; std::vector<GpuCheckCase> cases; };

const std::vector<CaseSet>& caseSets()
{
    static const std::vector<CaseSet> sets = [] {
        std::vector<CaseSet> v;
        {
            CaseSet s{"venetianblinds", {}};
            for (double pr : {0.0, 0.2, 0.5, 0.8, 1.0}) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}, {"NumberOfBlinds", {6}}}));
            for (double an : {0.0, 30.0, 45.0, 90.0, 135.0, 200.0, 270.0, 330.0, 380.0})
                s.cases.push_back(GpuCheckCase("angle" + std::to_string((int)an), PM{{"Transition", {0.45}}, {"Angle", {an}}, {"NumberOfBlinds", {5}}}));
            s.cases.push_back(GpuCheckCase("one blind (0 asked)", PM{{"Transition", {0.4}}, {"NumberOfBlinds", {0}}}));
            s.cases.push_back(GpuCheckCase("many blinds", PM{{"Transition", {0.5}}, {"NumberOfBlinds", {60}}, {"Angle", {20}}}));
            for (double fe : {0.02, 0.2, 0.7, 1.0}) s.cases.push_back(GpuCheckCase("feather" + std::to_string(fe), PM{{"Transition", {0.5}}, {"Feather", {fe}}, {"NumberOfBlinds", {8}}, {"Angle", {35}}}));
            s.cases.push_back(GpuCheckCase("feather no progress", PM{{"Transition", {0.0}}, {"Feather", {0.5}}, {"NumberOfBlinds", {8}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"portals", {}};
            const std::vector<double> col = {0.9, 0.3, 0.1};
            for (double pr : {0.0, 0.1, 0.25, 0.4, 0.5, 0.52, 0.6, 0.75, 0.9, 1.0}) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}}));
            for (int sd : {0, 83, 166, 12345, 2000000000})
                s.cases.push_back(GpuCheckCase("seed" + std::to_string(sd), PM{{"Transition", {0.45}}, {"RandomPatternSeed", {(double)sd}}}));
            for (int sq : {0, 1, 2, 12, 24, 100})
                for (double pr : {0.3, 0.8}) s.cases.push_back(GpuCheckCase("squares" + std::to_string(sq) + " p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Squares", {(double)sq}}}));
            s.cases.push_back(GpuCheckCase("no border", PM{{"Transition", {0.4}}, {"BorderSize", {0.0}}}));
            s.cases.push_back(GpuCheckCase("big border", PM{{"Transition", {0.4}}, {"BorderSize", {1.0}}, {"BorderColor", col}}));
            s.cases.push_back(GpuCheckCase("white border", PM{{"Transition", {0.7}}, {"BorderSize", {0.5}}, {"BorderColor", {1.0, 1.0, 1.0}}}));
            s.cases.push_back(GpuCheckCase("opaque", PM{{"Transition", {0.45}}, {"MaxTransparency", {1.0}}}));
            s.cases.push_back(GpuCheckCase("faint", PM{{"Transition", {0.45}}, {"MaxTransparency", {0.15}}, {"BorderColor", col}}));
            s.cases.push_back(GpuCheckCase("no offset or scale", PM{{"Transition", {0.45}}, {"MaxOffset", {0.0}}, {"MaxScale", {0.0}}}));
            s.cases.push_back(GpuCheckCase("big offset and scale", PM{{"Transition", {0.45}}, {"MaxOffset", {1.0}}, {"MaxScale", {1.0}}}));
            s.cases.push_back(GpuCheckCase("Mondrian-like", PM{{"Transition", {0.35}}, {"Squares", {24}}, {"MaxOffset", {0.5}}, {"MaxScale", {0.5}}, {"BorderSize", {0.5}}, {"BorderColor", col}}));
            v.push_back(s);
        }
        {
            // (with VEGAS's pictures when RENO_VIDPCORE_DLL names vidpcore.dll, else Reno's own gradients: the same cases run both ways)
            CaseSet s{"gradientwipe", {}};
            for (int g = 0; g < 28; ++g) s.cases.push_back(GpuCheckCase("gradient" + std::to_string(g), PM{{"Gradient", {(double)g}}, {"Transition", {0.5}}, {"ThresholdBlend", {0.1}}}));
            for (double pr : {0.0, 0.05, 0.3, 0.7, 0.95, 1.0}) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Gradient", {23}}, {"Transition", {pr}}, {"ThresholdBlend", {0.2}}}));
            for (double bl : {0.0, 0.0001, 0.3, 1.0}) s.cases.push_back(GpuCheckCase("blend" + std::to_string(bl), PM{{"Gradient", {5}}, {"Transition", {0.5}}, {"ThresholdBlend", {bl}}}));
            for (int g : {0, 15, 19, 23}) {
                s.cases.push_back(GpuCheckCase("reverse g" + std::to_string(g), PM{{"Gradient", {(double)g}}, {"Transition", {0.4}}, {"Direction", {1}}, {"ThresholdBlend", {0.1}}}));
                s.cases.push_back(GpuCheckCase("contrast g" + std::to_string(g), PM{{"Gradient", {(double)g}}, {"Transition", {0.4}}, {"AutoContrastStretch", {1}}, {"ThresholdBlend", {0.1}}}));
                s.cases.push_back(GpuCheckCase("aspect g" + std::to_string(g), PM{{"Gradient", {(double)g}}, {"Transition", {0.6}}, {"MaintainAspectRatio", {1}}}));
                s.cases.push_back(GpuCheckCase("blur g" + std::to_string(g), PM{{"Gradient", {(double)g}}, {"Transition", {0.5}}, {"GradientBlur", {0.4}}, {"ThresholdBlend", {0.1}}}));
            }
            for (double bl : {0.0005, 0.1, 0.8, 1.0}) s.cases.push_back(GpuCheckCase("blur" + std::to_string(bl), PM{{"Gradient", {23}}, {"Transition", {0.5}}, {"GradientBlur", {bl}}}));
            s.cases.push_back(GpuCheckCase("blur and contrast", PM{{"Gradient", {10}}, {"Transition", {0.5}}, {"GradientBlur", {0.3}}, {"AutoContrastStretch", {1}}, {"Direction", {1}}}));
            s.cases.push_back(GpuCheckCase("flip h", PM{{"Gradient", {13}}, {"Transition", {0.5}}, {"HorizontalFlip", {1}}}));
            s.cases.push_back(GpuCheckCase("flip v", PM{{"Gradient", {15}}, {"Transition", {0.5}}, {"VerticalFlip", {1}}}));
            s.cases.push_back(GpuCheckCase("flips", PM{{"Gradient", {19}}, {"Transition", {0.5}}, {"HorizontalFlip", {1}}, {"VerticalFlip", {1}}}));
            s.cases.push_back(GpuCheckCase("mirror h", PM{{"Gradient", {13}}, {"Transition", {0.5}}, {"HorizontalMirror", {1}}}));
            s.cases.push_back(GpuCheckCase("mirror v", PM{{"Gradient", {15}}, {"Transition", {0.5}}, {"VerticalMirror", {1}}}));
            s.cases.push_back(GpuCheckCase("mirrors and flips", PM{{"Gradient", {19}}, {"Transition", {0.5}}, {"HorizontalMirror", {1}}, {"VerticalMirror", {1}}, {"HorizontalFlip", {1}}, {"VerticalFlip", {1}}}));
            s.cases.push_back(GpuCheckCase("mirror h flip v blur", PM{{"Gradient", {23}}, {"Transition", {0.5}}, {"HorizontalMirror", {1}}, {"VerticalFlip", {1}}, {"GradientBlur", {0.2}}}));
            v.push_back(s);
        }
        return v;
    }();
    return sets;
}

}  // namespace

void trans2Cases(const std::string& id, std::vector<GpuCheckCase>& out)
{
    for (const CaseSet& s : caseSets())
        if (id == fullId(s.id)) out.insert(out.end(), s.cases.begin(), s.cases.end());
}

bool trans2FloatTolerance(const std::string& id, double& tol)
{
    static const struct { const char* id; double tol; } known[] = {
        {"venetianblinds", 0.0},
        {"portals", 0.0},
        {"gradientwipe", 0.0},
    };
    for (const auto& k : known)
        if (id == fullId(k.id)) { tol = k.tol; return true; }
    return false;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
