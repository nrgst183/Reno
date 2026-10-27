// Built-in versions of VEGAS Pro 18's two stabilisers:
//
//   de.magix:Stabilize                   "VEGAS Video Stabilization" (MagixCVFx.ofx): Basic / Professional / Expert controls: method (Fast: model fitting,
//                                        Accurate: temporal consistency), motion to compensate (shift, shift + rotation, + scale, affine), the region and grid
//                                        feature points come from, Smooth motion (smoothing time in seconds) or Freeze motion (hold the Reference frame), which
//                                        components are corrected (Translate X/Y, Angle, Scale X/Y, Shear), Auto Zoom and extra scale / rotation / translation
//                                        to hide borders, edge filling, and motion blur from the camera movement.
//   com.vegascreativesoftware:Stabilize  "VEGAS Stabilize Video" (Stabilize.ofx, the older plug-in): Pan smoothing, Stabilization amount (View scale),
//                                        Rolling shutter correction.
//
// VEGAS analyses the whole clip once ("Analyze Motion") and keeps the result in the effect's hidden settings. Reno cannot read those, so it analyses on demand:
// movement between neighbouring frames comes from builtins_analysis_motion.cpp and is cached per instance; a frame's picture is made from the camera path
// round it (the frames a smoothing time wide around it: playing on costs one new frame each, the first frame after a seek the whole window). The path is
// Gaussian-smoothed over the window and the picture moved by the difference; at the clip's ends the path continues by reflection so the first and last
// frames are not pulled sideways.
//
// Parameter names, labels, choices, defaults and ranges are the plug-ins' own; what they do with them is worked out from VEGAS's manual, and the rest is Reno's own.
#include "builtins_analysis_internal.h"
#include "../core/base.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace reno {
namespace ofx {
namespace builtin {
namespace analysis {
namespace {

// ------------------------------------------------------------------ settings
struct Settings {
    bool freeze = false;
    double seconds = 2.5;               // smoothing time
    int refFrame = 0;
    EstParams ep;
    int analysisW = 320;
    bool lock[6] = {true, true, true, true, true, true};
    double rotAmt = 1, scaleAmt = 1;
    bool autoZoom = true;
    double addScale = 0, addRotDeg = 0, addTx = 0.5, addTy = 0.5;
    int fill = 0;                       // 0 transparent, 1 mirror, 2 repeat, 3 clamp
    bool blur = false;
    double blurThr = 10, blurMax = 19;
    bool rolling = false;
    bool showPoints = false;
    int minTrackLen = 1;
    uint64_t sig = 0;
};

uint64_t mixHash(uint64_t h, double v)
{
    uint64_t b;
    std::memcpy(&b, &v, sizeof b);
    h ^= b + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

void finishSignature(Settings& s)
{
    uint64_t h = 1469598103934665603ull;
    const EstParams& e = s.ep;
    for (double v : {e.roiCx, e.roiCy, e.roiW, e.roiH, (double)e.gridX, (double)e.gridY, (double)e.perCell, (double)e.model, (double)e.sad, (double)e.lk, (double)e.accurate, (double)s.analysisW})
        h = mixHash(h, v);
    s.sig = h;
}

// The new plug-in's settings.
Settings readVideoStabilization(const Ctx& c)
{
    Settings s;
    EstParams& e = s.ep;
    e.accurate = c.i("Presets") == 1;
    e.model = std::max(0, std::min(3, c.i("MotionCompensation")));
    e.roiW = clampd(c.d("XScale"), 0.05, 1);
    e.roiH = clampd(c.d("YScale"), 0.05, 1);
    double lx, ly;
    c.xy("Location", lx, ly);
    e.roiCx = clampd(lx, 0, 1);
    e.roiCy = clampd(1 - ly, 0, 1);
    e.gridX = std::max(2, c.i("NumGridsX"));
    e.gridY = std::max(2, c.i("NumGridsY"));
    e.perCell = std::max(1, c.i("PointsPerGridCell"));
    switch (c.i("ChoiceTrackerType")) {
    case 1: e.sad = true; e.lk = false; break;
    case 2: e.sad = true; e.lk = true; break;
    default: e.sad = false; e.lk = true; break;         // Lucas-Kanade
    }
    switch (c.i("Resolution")) {
    case 0: s.analysisW = 448; break;
    case 2: s.analysisW = 320; break;
    default: s.analysisW = 320; break;
    }
    s.freeze = c.i("StabilizeMode") == 1 || c.b("IsStaticCamera");
    s.seconds = clampd(c.d("SmoothingMagnitude"), 0, 1000);
    s.refFrame = c.i("StaticReference");
    static const char* const locks[6] = {"LockTranslateX", "LockTranslateY", "LockAngle", "LockScaleX", "LockScaleY", "LockShear"};
    for (int k = 0; k < 6; ++k) s.lock[k] = c.b(locks[k]);
    s.autoZoom = c.b("AvoidBlackBorders");
    s.addScale = c.d("AdditionalScale");
    s.addRotDeg = c.d("AdditionalRotation");
    c.xy("AdditionalTranslation", s.addTx, s.addTy);
    s.fill = std::max(0, std::min(3, c.i("Zoom_Filling")));
    s.blur = c.b("MotionBlur");
    s.blurThr = std::max(0.0, c.d("MotionBlurThreshold"));
    s.blurMax = std::max(0.0, c.d("MotionBlurMaxMagnitude"));
    s.showPoints = c.b("ShowFeatures");
    s.minTrackLen = std::max(1, c.i("ShowFeaturesMinLen"));
    finishSignature(s);
    return s;
}

// The old plug-in's: Pan smoothing is how long the path is smoothed over, Stabilization amount how much of the turning and
// zooming is corrected (a shift always is), the borders are always zoomed away.
Settings readStabilizeVideo(const Ctx& c)
{
    Settings s;
    s.ep.model = 2;
    s.ep.accurate = true;
    s.analysisW = 320;
    s.freeze = false;
    s.seconds = 0.15 + 2.85 * clampd(c.d("PanSmoothing"), 0, 1);
    const double amount = clampd(c.d("ViewScale"), 0, 1);
    s.rotAmt = s.scaleAmt = std::min(1.0, 2.0 * amount);
    s.autoZoom = true;
    s.fill = 3;
    s.rolling = c.b("RollingShutterCorrection");
    finishSignature(s);
    return s;
}

// ------------------------------------------------------------------ what is kept for an instance
struct PairRec {
    Xf xf;
    bool ok = false;
};

struct State {
    std::mutex mu;
    uint64_t sig = 0;
    int grayW = 0;
    std::map<long, PairRec> pairs;                          // frame k to frame k+1
    std::map<long, std::shared_ptr<Gray>> grays;
    std::map<long, Signature> sigs;
    std::map<long, PairRec> toRef;
    long refKey = -1000000000L;
    std::shared_ptr<Gray> refGray;
};

std::mutex gStatesMutex;
std::map<const void*, std::shared_ptr<State>>& states()
{
    static std::map<const void*, std::shared_ptr<State>> m;
    return m;
}

std::shared_ptr<State> stateFor(const void* inst)
{
    std::lock_guard<std::mutex> lk(gStatesMutex);
    auto& m = states();
    auto it = m.find(inst);
    if (it != m.end()) return it->second;
    if (m.size() > 64) m.erase(m.begin());      // (instances that were never told they were gone)
    auto st = std::make_shared<State>();
    m[inst] = st;
    return st;
}

void forgetInstance(const void* inst)
{
    std::lock_guard<std::mutex> lk(gStatesMutex);
    states().erase(inst);
}

// ------------------------------------------------------------------ the path
struct Comp { double tx = 0, ty = 0, th = 0, sx = 1, sy = 1, sh = 0; };

Comp decompose(const Xf& m)
{
    Comp c;
    c.tx = m.tx;
    c.ty = m.ty;
    c.sx = std::hypot(m.a, m.c);
    if (!(c.sx > 1e-6)) { c.sx = 1; return c; }
    c.th = std::atan2(m.c, m.a);
    const double ct = m.a / c.sx, st = m.c / c.sx;
    const double k = m.b * ct + m.d * st;
    c.sy = -m.b * st + m.d * ct;
    if (!(c.sy > 1e-6)) { c.sy = 1; c.sh = 0; return c; }
    c.sh = k / c.sy;
    return c;
}

Xf compose(const Comp& c)
{
    const double ct = std::cos(c.th), st = std::sin(c.th);
    const double k = c.sh * c.sy;
    Xf m;
    m.a = ct * c.sx;
    m.b = ct * k - st * c.sy;
    m.c = st * c.sx;
    m.d = st * k + ct * c.sy;
    m.tx = c.tx;
    m.ty = c.ty;
    return m;
}

struct Vec6 {
    double v[6];
    Vec6() { v[0] = 1; v[1] = 0; v[2] = 0; v[3] = 1; v[4] = 0; v[5] = 0; }
    explicit Vec6(const Xf& x) { v[0] = x.a; v[1] = x.b; v[2] = x.c; v[3] = x.d; v[4] = x.tx; v[5] = x.ty; }
    Xf xf() const { Xf x; x.a = v[0]; x.b = v[1]; x.c = v[2]; x.d = v[3]; x.tx = v[4]; x.ty = v[5]; return x; }
};

const PairRec* pairAt(const State& st, long k)
{
    auto it = st.pairs.find(k);
    return it == st.pairs.end() ? nullptr : &it->second;
}

// The camera's path round frame n: where the content of frame n is in each of the frames n + j (j in [jlo, jhi]).
void pathAround(const State& st, long n, int jlo, int jhi, std::vector<Xf>& out)
{
    out.assign((size_t)(jhi - jlo + 1), Xf());
    auto A = [&](int j) -> Xf& { return out[(size_t)(j - jlo)]; };
    for (int j = 1; j <= jhi; ++j) {
        const PairRec* p = pairAt(st, n + j - 1);
        A(j) = (p && p->ok) ? p->xf * A(j - 1) : A(j - 1);
    }
    for (int j = -1; j >= jlo; --j) {
        const PairRec* p = pairAt(st, n + j);
        A(j) = (p && p->ok) ? p->xf.inverse() * A(j + 1) : A(j + 1);
    }
}

// The transform that takes frame n's content to the smoothed camera's view of it (the stabilised picture q shows frame n at S^-1(q)): the Gaussian-weighted
// average of the path over j in [-hw, hw], continued by reflection beyond the frames there are (fLo..fHi).
Xf smoothedAt(const State& st, long n, int hw, double sigma, long fLo, long fHi)
{
    const int jlo = (int)std::max<long>(-hw, fLo - n), jhi = (int)std::min<long>(hw, fHi - n);
    if (jhi <= jlo && jlo == 0 && jhi == 0) return Xf();
    std::vector<Xf> path;
    pathAround(st, n, jlo, jhi, path);
    double acc[6] = {0, 0, 0, 0, 0, 0}, sw = 0;
    for (int j = -hw; j <= hw; ++j) {
        double w = std::exp(-0.5 * (double)j * j / (sigma * sigma));
        Vec6 val;
        if (j >= jlo && j <= jhi) {
            val = Vec6(path[(size_t)(j - jlo)]);
        } else if (j > jhi) {
            const int idx = std::max(jlo, 2 * jhi - j);        // the point reflection about the last frame
            const Vec6 e(path[(size_t)(jhi - jlo)]), m(path[(size_t)(idx - jlo)]);
            for (int k = 0; k < 6; ++k) val.v[k] = 2 * e.v[k] - m.v[k];
        } else {
            const int idx = std::min(jhi, 2 * jlo - j);
            const Vec6 e(path[0]), m(path[(size_t)(idx - jlo)]);
            for (int k = 0; k < 6; ++k) val.v[k] = 2 * e.v[k] - m.v[k];
        }
        for (int k = 0; k < 6; ++k) acc[k] += w * val.v[k];
        sw += w;
    }
    Vec6 r;
    if (sw > 0) for (int k = 0; k < 6; ++k) r.v[k] = acc[k] / sw;
    return r.xf();
}

// Which components of the transform are corrected, and how much of the turn and the scale.
Xf maskComponents(const Xf& s, const Settings& set)
{
    Comp c = decompose(s);
    if (!set.lock[0]) c.tx = 0;
    if (!set.lock[1]) c.ty = 0;
    if (!set.lock[2]) c.th = 0;
    if (!set.lock[3]) c.sx = 1;
    if (!set.lock[4]) c.sy = 1;
    if (!set.lock[5]) c.sh = 0;
    c.th *= set.rotAmt;
    c.sx = std::exp(std::log(std::max(c.sx, 1e-3)) * set.scaleAmt);
    c.sy = std::exp(std::log(std::max(c.sy, 1e-3)) * set.scaleAmt);
    return compose(c);
}

// The picture-making maps. Output q (frame units) shows the stabilised picture, which shows the frame at p = S^-1(E^-1(q)); E is the zoom, turn and shift added to hide the borders.
struct Extras {
    double zoom = 1, rot = 0, tu = 0, tv = 0;
    Xf forward() const
    {
        Xf e;
        const double ct = std::cos(rot) * zoom, st = std::sin(rot) * zoom;
        e.a = ct; e.b = st; e.c = -st; e.d = ct;
        e.tx = tu; e.ty = tv;
        return e;
    }
};

// The smallest zoom that keeps the picture inside the frame's edges for the stabilising map S (aspect = width / height).
double zoomNeeded(const Xf& S, const Extras& ex, double aspect)
{
    Extras one = ex;
    one.zoom = 1;
    const Xf Sinv = S.inverse();
    const Xf E1inv = one.forward().inverse();
    const double hw = aspect / 2, hh = 0.5;
    double z = 1;
    const double cs[4][2] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
    for (int i = 0; i < 4; ++i) {
        double u, v;
        E1inv.apply(cs[i][0], cs[i][1], u, v);
        const double wx = Sinv.a * u + Sinv.b * v, wy = Sinv.c * u + Sinv.d * v;
        const double bx = Sinv.tx, by = Sinv.ty;
        auto need = [&](double w, double b, double half) {
            if (w > 0) { const double room = half - b; return room > 0.02 ? w / room : 2.0; }
            if (w < 0) { const double room = half + b; return room > 0.02 ? -w / room : 2.0; }
            return 1.0;
        };
        z = std::max(z, std::max(need(wx, bx, hw), need(wy, by, hh)));
    }
    return std::min(z, 2.0);
}

// ------------------------------------------------------------------ the picture
inline int wrapIndex(int i, int n, int mode)
{
    if (i >= 0 && i < n) return i;
    if (mode == 1) {
        const int m = ((i % (2 * n)) + 2 * n) % (2 * n);
        return m < n ? m : 2 * n - 1 - m;
    }
    if (mode == 2) return ((i % n) + n) % n;        // repeat
    return i < 0 ? 0 : n - 1;                       // clamp to edge
}

// Bilinear sample of a premultiplied frame at continuous (x, y) (pixel centres at +0.5) with the filling mode for outside.
inline void sampleFilled(const Buf& b, double x, double y, int mode, float o[4])
{
    if (mode == 0) { b.sample(x, y, o); return; }
    x -= 0.5; y -= 0.5;
    if (!std::isfinite(x) || !std::isfinite(y)) { o[0] = o[1] = o[2] = o[3] = 0.f; return; }
    x = std::max(-1e6, std::min(1e6, x));
    y = std::max(-1e6, std::min(1e6, y));
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = (float)(x - x0), fy = (float)(y - y0);
    const int xa = wrapIndex(x0, b.w, mode), xb = wrapIndex(x0 + 1, b.w, mode), ya = wrapIndex(y0, b.h, mode), yb = wrapIndex(y0 + 1, b.h, mode);
    const float* p00 = b.at(xa, ya);
    const float* p10 = b.at(xb, ya);
    const float* p01 = b.at(xa, yb);
    const float* p11 = b.at(xb, yb);
    for (int k = 0; k < 4; ++k) o[k] = (p00[k] * (1 - fx) + p10[k] * fx) * (1 - fy) + (p01[k] * (1 - fx) + p11[k] * fx) * fy;
}

// Draws the picture: output pixel (x, y) is the source at M(u, v), (u, v) the pixel in frame units. 'rs' is the rolling shutter correction (extra source shift per
// unit of v, in pixels), 'blur' the length (pixels) and direction of the motion blur (0: none).
void warpFrame(const Buf& src, Buf& out, const Xf& M, int fill, double rsx, double rsy, double blurLen, double bdx, double bdy)
{
    const int W = out.w, H = out.h;
    const double Hd = (double)H;
    int taps = 1;
    if (blurLen > 0.3) taps = std::max(2, std::min(16, (int)std::ceil(blurLen) + 1));
    forRows(H, W, [&](int y) {
        const double v = (y + 0.5 - H / 2.0) / Hd;
        float* o = out.at(0, y);
        for (int x = 0; x < W; ++x, o += 4) {
            const double u = (x + 0.5 - W / 2.0) / Hd;
            double pu, pv;
            M.apply(u, v, pu, pv);
            double sx = pu * Hd + W / 2.0 + rsx * v, sy = pv * Hd + H / 2.0 + rsy * v;
            if (taps == 1) {
                sampleFilled(src, sx, sy, fill, o);
            } else {
                float acc[4] = {0, 0, 0, 0}, t[4];
                for (int i = 0; i < taps; ++i) {
                    const double f = ((i + 0.5) / taps - 0.5) * blurLen;
                    sampleFilled(src, sx + bdx * f, sy + bdy * f, fill, t);
                    for (int k = 0; k < 4; ++k) acc[k] += t[k];
                }
                for (int k = 0; k < 4; ++k) o[k] = acc[k] / taps;
            }
        }
    });
}

void drawDot(Buf& b, double x, double y, int r, const float col[3])
{
    const int cx = (int)std::lround(x), cy = (int)std::lround(y);
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const int xx = cx + dx, yy = cy + dy;
            if (xx < 0 || yy < 0 || xx >= b.w || yy >= b.h) continue;
            float* p = b.at(xx, yy);
            p[0] = col[0]; p[1] = col[1]; p[2] = col[2]; p[3] = 1.f;
        }
}

void drawSegment(Buf& b, double x0, double y0, double x1, double y1, const float col[3])
{
    const int steps = (int)std::min(4000.0, std::ceil(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)))) + 1;
    for (int i = 0; i <= steps; ++i) {
        const double t = (double)i / steps;
        drawDot(b, x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, 0, col);
    }
}

// ------------------------------------------------------------------ finding the movement
// The frame at 'key' in grey: from the cache, or from the host (null when there is none: the clip does not reach there).
std::shared_ptr<Gray> frameGray(State& st, Ctx& c, long key, long n0, double time0, int aw, std::set<long>& absent)
{
    auto it = st.grays.find(key);
    if (it != st.grays.end()) return it->second;
    if (absent.count(key) || !c.fetch) return nullptr;
    Buf tmp;
    if (!c.fetch(time0 + (double)(key - n0), tmp) || tmp.w != c.src.w || tmp.h != c.src.h) { absent.insert(key); return nullptr; }
    auto g = std::make_shared<Gray>(makeGray(tmp, aw));
    if (g->w <= 0 || g->alpha < 0.02) { absent.insert(key); return nullptr; }
    st.grays[key] = g;
    return g;
}

void trimGrays(State& st, long n0)
{
    while (st.grays.size() > 160) {
        auto far = st.grays.begin();
        long fd = -1;
        for (auto it = st.grays.begin(); it != st.grays.end(); ++it) {
            const long d = std::labs(it->first - n0);
            if (d > fd) { fd = d; far = it; }
        }
        st.grays.erase(far);
    }
    while (st.pairs.size() > 6000) {
        auto first = st.pairs.begin(), last = std::prev(st.pairs.end());
        st.pairs.erase(std::labs(first->first - n0) >= std::labs(last->first - n0) ? first : last);
    }
    while (st.toRef.size() > 6000) {
        auto first = st.toRef.begin(), last = std::prev(st.toRef.end());
        st.toRef.erase(std::labs(first->first - n0) >= std::labs(last->first - n0) ? first : last);
    }
}

// Makes sure the movement between the frames from lo to hi (as many as exist) is known. Gives the frames there are, fLo..fHi.
void ensurePairs(State& st, Ctx& c, long n0, double time0, long lo, long hi, const Settings& S, const std::shared_ptr<Gray>& g0, long& fLo, long& fHi)
{
    std::set<long> absent;
    struct Job { long key; std::shared_ptr<Gray> a, b; PairRec rec; };
    std::vector<Job> jobs;
    fHi = n0;
    for (long k = n0 + 1; k <= hi; ++k) {
        if (pairAt(st, k - 1)) { fHi = k; continue; }
        std::shared_ptr<Gray> a = (k - 1 == n0) ? g0 : frameGray(st, c, k - 1, n0, time0, S.analysisW, absent);
        std::shared_ptr<Gray> b = frameGray(st, c, k, n0, time0, S.analysisW, absent);
        if (!a || !b || a->w != b->w || a->h != b->h) break;
        jobs.push_back({k - 1, a, b, PairRec()});
        fHi = k;
    }
    fLo = n0;
    for (long k = n0 - 1; k >= lo; --k) {
        if (pairAt(st, k)) { fLo = k; continue; }
        std::shared_ptr<Gray> a = frameGray(st, c, k, n0, time0, S.analysisW, absent);
        std::shared_ptr<Gray> b = (k + 1 == n0) ? g0 : frameGray(st, c, k + 1, n0, time0, S.analysisW, absent);
        if (!a || !b || a->w != b->w || a->h != b->h) break;
        jobs.push_back({k, a, b, PairRec()});
        fLo = k;
    }
    if (!jobs.empty()) {
        EstParams ep = S.ep;
        ep.keepPoints = false;
        parallelTasks((int)jobs.size(), [&](int i) {
            PairResult r;
            if (estimatePair(*jobs[(size_t)i].a, *jobs[(size_t)i].b, ep, r)) {
                jobs[(size_t)i].rec.xf = r.xf;
                jobs[(size_t)i].rec.ok = true;
            }
        });
        for (const Job& j : jobs) st.pairs[j.key] = j.rec;
    }
}

// Freeze motion: the movement of frame n to the reference frame, found directly (a still camera drifts little from the reference, and
// nothing accumulates).
void ensureRef(State& st, Ctx& c, long n0, double time0, long lo, long hi, long refKey, const Settings& S, const std::shared_ptr<Gray>& g0, long& fLo, long& fHi)
{
    std::set<long> absent;
    if (st.refKey != refKey || !st.refGray) {
        st.toRef.clear();
        st.refKey = refKey;
        st.refGray = frameGray(st, c, refKey, n0, time0, S.analysisW, absent);
    }
    fLo = fHi = n0;
    if (!st.refGray) return;
    struct Job { long key; std::shared_ptr<Gray> a; PairRec rec; };
    std::vector<Job> jobs;
    for (long k = n0; k <= hi; ++k) {
        if (st.toRef.count(k)) { fHi = k; continue; }
        std::shared_ptr<Gray> a = (k == n0) ? g0 : frameGray(st, c, k, n0, time0, S.analysisW, absent);
        if (!a || a->w != st.refGray->w || a->h != st.refGray->h) break;
        jobs.push_back({k, a, PairRec()});
        fHi = k;
    }
    for (long k = n0 - 1; k >= lo; --k) {
        if (st.toRef.count(k)) { fLo = k; continue; }
        std::shared_ptr<Gray> a = frameGray(st, c, k, n0, time0, S.analysisW, absent);
        if (!a || a->w != st.refGray->w || a->h != st.refGray->h) break;
        jobs.push_back({k, a, PairRec()});
        fLo = k;
    }
    if (!jobs.empty()) {
        EstParams ep = S.ep;
        ep.keepPoints = false;
        const Gray& ref = *st.refGray;
        parallelTasks((int)jobs.size(), [&](int i) {
            if (jobs[(size_t)i].key == refKey) { jobs[(size_t)i].rec.ok = true; return; }
            PairResult r;
            if (estimatePair(*jobs[(size_t)i].a, ref, ep, r)) {
                jobs[(size_t)i].rec.xf = r.xf;
                jobs[(size_t)i].rec.ok = true;
            }
        });
        for (const Job& j : jobs) st.toRef[j.key] = j.rec;
    }
}

// ------------------------------------------------------------------ the effect
void stabilizeRender(Ctx& c, const Settings& S, bool oldPlugin)
{
    const int W = c.out.w, H = c.out.h;
    if (W <= 0 || H <= 0 || c.src.px.size() != c.out.px.size()) return;
    // VEGAS's plug-ins leave the picture as it is until the motion has been analysed (the Analyze / Apply button sets these flags); checked against VEGAS's own output.
    if (!(oldPlugin ? (c.b("HasM10StabData") || c.b("HasM20StabData")) : c.b("HasMotionData"))) {
        c.out.px = c.src.px;
        return;
    }
    const double fps = c.fps > 0 ? c.fps : 30.0;
    const double aspect = (double)W / H;
    Xf S0;
    double zoom = 1;
    double velX = 0, velY = 0;
    double rawX = 0, rawY = 0;
    std::vector<FeatPt> feat;
    const bool temporal = c.fetch && c.instance && S.seconds >= 0;

    Extras ex;
    ex.rot = S.addRotDeg * kPi / 180.0;
    ex.tu = (S.addTx - 0.5) * aspect;
    ex.tv = -(S.addTy - 0.5);

    if (temporal) {
        std::shared_ptr<State> stp = stateFor(c.instance);
        State& st = *stp;
        std::lock_guard<std::mutex> lk(st.mu);
        const long n0 = (long)std::lround(c.time);
        std::shared_ptr<Gray> g0 = std::make_shared<Gray>(makeGray(c.src, S.analysisW));
        if (st.sig != S.sig || st.grayW != g0->w) {
            if (st.sig != S.sig) { st.pairs.clear(); st.toRef.clear(); st.refKey = -1000000000L; st.refGray.reset(); }
            st.grays.clear();
            st.sigs.clear();
            st.sig = S.sig;
            st.grayW = g0->w;
        }
        // the picture here is not what it was when the movement was found (the effects before this one were changed, or the clip): start again
        {
            const Signature sg = signatureOf(*g0);
            auto it = st.sigs.find(n0);
            if (it != st.sigs.end() && !sameSignature(it->second, sg)) {
                st.pairs.clear(); st.toRef.clear(); st.grays.clear(); st.sigs.clear(); st.refGray.reset(); st.refKey = -1000000000L;
            }
            st.sigs[n0] = sg;
            if (st.sigs.size() > 4000) st.sigs.erase(st.sigs.begin());
        }
        st.grays[n0] = g0;
        const double t0 = c.time;
        const int hw = (int)std::max(1.0, std::min(60.0, std::floor(S.seconds * fps / 2.0 + 0.5)));
        const double sigma = std::max(0.7, hw / 2.0);
        const int zr = S.autoZoom ? 12 : 0;
        long fLo = n0, fHi = n0;
        if (S.freeze) {
            const long ref = (long)std::max(0, S.refFrame);
            ensureRef(st, c, n0, t0, n0 - zr, n0 + zr, ref, S, g0, fLo, fHi);
        } else {
            ensurePairs(st, c, n0, t0, n0 - hw - zr, n0 + hw + zr, S, g0, fLo, fHi);
        }
        trimGrays(st, n0);
        auto mapFor = [&](long n) -> Xf {
            Xf s;
            if (S.freeze) {
                auto it = st.toRef.find(n);
                if (it != st.toRef.end() && it->second.ok) s = it->second.xf;
            } else {
                s = smoothedAt(st, n, hw, sigma, fLo, fHi);
            }
            return maskComponents(s, S);
        };
        S0 = mapFor(n0);
        if (S.autoZoom) {
            const int zw = 8, za = 4;
            std::map<long, double> zs;
            auto zAt = [&](long n) {
                auto it = zs.find(n);
                if (it != zs.end()) return it->second;
                const double z = (n >= fLo && n <= fHi) ? zoomNeeded(mapFor(n), ex, aspect) : 1.0;
                zs[n] = z;
                return z;
            };
            double acc = 0;
            int cnt = 0;
            for (int a = -za; a <= za; ++a) {
                double m = 1;
                for (int b = -zw; b <= zw; ++b) m = std::max(m, zAt(n0 + a + b));
                acc += m;
                ++cnt;
            }
            zoom = std::max(1.0, acc / cnt);
        }
        if (!S.freeze) {
            double sx = 0, sy = 0, sw = 0;
            for (long k = std::max(n0 - hw, fLo); k < std::min(n0 + hw, fHi); ++k) {
                const PairRec* p = pairAt(st, k);
                if (!p || !p->ok) continue;
                sx += p->xf.tx; sy += p->xf.ty; sw += 1;
            }
            if (sw > 0) { velX = sx / sw * H; velY = sy / sw * H; }
            const PairRec* pr = pairAt(st, n0);
            if (pr && pr->ok) { rawX = pr->xf.tx * H; rawY = pr->xf.ty * H; }
            else if ((pr = pairAt(st, n0 - 1)) && pr->ok) { rawX = pr->xf.tx * H; rawY = pr->xf.ty * H; }
        }
        if (S.showPoints && !S.freeze) {
            std::set<long> absent;
            std::shared_ptr<Gray> g1 = frameGray(st, c, n0 + 1, n0, t0, S.analysisW, absent);
            if (g1 && g1->w == g0->w && g1->h == g0->h) {
                EstParams ep = S.ep;
                ep.keepPoints = true;
                PairResult r;
                if (estimatePair(*g0, *g1, ep, r)) feat = std::move(r.pts);
            }
        }
    }
    ex.zoom = (S.autoZoom ? zoom : 1.0) * (1.0 + std::max(-0.9, S.addScale));
    const Xf E = ex.forward();
    const Xf M = S0.inverse() * E.inverse();
    double rsx = 0, rsy = 0;
    if (S.rolling) { rsx = rawX * 0.6; rsy = rawY * 0.6; }
    double blurLen = 0, bdx = 0, bdy = 0;
    if (S.blur) {
        const double sp = std::hypot(velX, velY);
        if (sp > S.blurThr && sp > 1e-6) {
            blurLen = std::min(sp, S.blurMax) * 0.5;
            bdx = velX / sp; bdy = velY / sp;
        }
    }
    warpFrame(c.src, c.out, M, S.fill, rsx, rsy, blurLen, bdx, bdy);
    if (!feat.empty()) {
        const float green[3] = {0.1f, 1.f, 0.2f}, red[3] = {1.f, 0.2f, 0.1f};
        const Xf fwd = E * S0;
        const double Hd = (double)H;
        for (const FeatPt& f : feat) {
            if (!f.inlier && S.minTrackLen > 1) continue;
            double qx, qy, ex2, ey2;
            fwd.apply(f.x, f.y, qx, qy);
            fwd.apply(f.x + f.vx, f.y + f.vy, ex2, ey2);
            const double x0 = qx * Hd + W / 2.0, y0 = qy * Hd + H / 2.0, x1 = ex2 * Hd + W / 2.0, y1 = ey2 * Hd + H / 2.0;
            const float* col = f.inlier ? green : red;
            drawSegment(c.out, x0, y0, x1, y1, col);
            drawDot(c.out, x0, y0, std::max(1, (int)std::lround(c.px(1.5))), col);
        }
    }
    (void)oldPlugin;
}

void videoStabilization(Ctx& c) { stabilizeRender(c, readVideoStabilization(c), false); }
void stabilizeVideo(Ctx& c) { stabilizeRender(c, readStabilizeVideo(c), true); }

void destroyState(const void* inst) { forgetInstance(inst); }

// The Analyze / Clear buttons. VEGAS's Analyze Motion finds the camera movement through the clip and keeps it in the effect's hidden settings; Reno keeps it
// in the instance's cache (the one a render fills as it plays), so the button fills it now for the whole clip (at most 3000 frames from its start) and Clear throws it away.
void analyzeClip(Change& ch, bool oldPlugin)
{
    double first = 0, last = 0;
    ch.clipRange(first, last);
    if (!(last > first)) return;
    const Settings S = oldPlugin ? readStabilizeVideo(ch.ctx) : readVideoStabilization(ch.ctx);
    Ctx c;
    c.params = ch.ctx.params;
    c.fps = ch.ctx.fps;
    c.instance = ch.effect;
    c.fetch = [&ch](double t, Buf& into) { return ch.source(t, into); };
    const long f0 = (long)std::lround(first), f1 = std::min<long>((long)std::lround(last) - 1, f0 + 2999);
    std::shared_ptr<State> stp = stateFor(ch.effect);
    State& st = *stp;
    std::lock_guard<std::mutex> lk(st.mu);
    if (st.sig != S.sig) {
        st.pairs.clear(); st.toRef.clear(); st.grays.clear(); st.sigs.clear(); st.refGray.reset(); st.refKey = -1000000000L;
        st.sig = S.sig;
    }
    long n = f0;
    while (n <= f1) {
        c.time = (double)n;
        if (!ch.source(c.time, c.src)) break;
        auto g0 = std::make_shared<Gray>(makeGray(c.src, S.analysisW));
        if (g0->w <= 0 || g0->alpha < 0.02) break;
        if (st.grayW != g0->w) { st.grays.clear(); st.sigs.clear(); st.grayW = g0->w; }
        st.grays[n] = g0;
        long lo = n, hi = n;
        const long top = std::min(f1, n + 99);
        if (S.freeze) ensureRef(st, c, n, c.time, n, top, (long)std::max(0, S.refFrame), S, g0, lo, hi);
        else ensurePairs(st, c, n, c.time, n, top, S, g0, lo, hi);
        trimGrays(st, n);
        if (hi <= n) break;
        n = hi;
    }
    ch.setI(oldPlugin ? "HasM20StabData" : "HasMotionData", 1);
}

void stabilizeChanged(Change& ch)
{
    if (ch.param == "AnalyzeMotionBasic" || ch.param == "AnalyzeMotion") {
        analyzeClip(ch, false);
    } else if (ch.param == "CLearMotionBasic" || ch.param == "CLearMotion") {
        forgetInstance(ch.effect);
        ch.setI("HasMotionData", 0);
    }
}

void stabilizeVideoChanged(Change& ch)
{
    if (ch.param == "AnalyzeButton") analyzeClip(ch, true);
}

}  // namespace

void addStabilizeEffects(std::vector<Def>& out)
{
    {
        std::vector<PDef> ps;
        ps.push_back(choice("Views", "User View", 0, {"Basic", "Professional", "Expert"}));
        ps.push_back(group("GroupParameters", "Parameters"));
        ps.push_back(button("AnalyzeMotionBasic", "Stabilize", "GroupParameters"));
        ps.push_back(button("CLearMotionBasic", "Reset", "GroupParameters"));       // (sic: VEGAS's spelling of the name)
        ps.push_back(choice("Presets", "Method", 1, {"Fast", "Accurate"}, "GroupParameters"));
        ps.push_back(choice("MotionCompensation", "Motion Compensation", 2, {"Translation", "Translation + Rotation", "Translation + Rotation + Scale", "Warp"}, "GroupParameters"));
        ps.push_back(group("GroupExpertMode", "Expert Mode"));
        ps.push_back(dbl("XScale", "Width (Region of Interest)", 0.85, 0.01, 1.0, "GroupExpertMode"));
        ps.push_back(dbl("YScale", "Height (Region of Interest)", 0.8, 0.01, 1.0, "GroupExpertMode"));
        ps.push_back(point("Location", "Location", 0.5, 0.5, "GroupExpertMode"));
        ps.back().secret = true;      // (not in VEGAS's own list of the parameters it shows)
        ps.push_back(intp("NumGridsX", "Number of Grids in X", 14, 4, 100, "GroupExpertMode"));
        ps.push_back(intp("NumGridsY", "Number of Grids in Y", 9, 4, 100, "GroupExpertMode"));
        ps.push_back(intp("PointsPerGridCell", "Number of Points per Grid Cell", 2, 2, 100, "GroupExpertMode"));
        ps.push_back(choice("ChoiceTrackerType", "Tracker Type", 0, {"Lucas-Kanade", "Sum of Absolute Differences", "Hybrid"}, "GroupExpertMode"));
        ps.push_back(choice("Resolution", "Resolution", 1, {"Full", "Half", "Quarter"}, "GroupExpertMode"));
        ps.back().secret = true;      // (not in VEGAS's own list of the parameters it shows)
        ps.push_back(group("GroupSmoothing", "Stabilization options"));
        ps.push_back(choice("StabilizeMode", "Mode", 0, {"Smooth motion", "Freeze motion"}, "GroupSmoothing"));
        ps.back().secret = true;      // (not in VEGAS's own list of the parameters it shows)
        ps.push_back(dbl("SmoothingMagnitude", "Smoothing", 2.5, 0, 1000, "GroupSmoothing"));
        ps.back().dmax = 10;
        ps.push_back(button("AnalyzeMotion", "Analyze Motion", "GroupSmoothing"));
        ps.push_back(button("CLearMotion", "Clear Motion Data", "GroupSmoothing"));
        ps.push_back(intp("StaticReference", "Reference Frame Number", 0, 0, 1000000, "GroupSmoothing"));
        ps.push_back(group("GroupLockState", "Smoothing Parameters"));          // (sic: VEGAS's own spelling)
        ps.push_back(boolean("LockTranslateX", "Translate X", true, "GroupLockState"));
        ps.push_back(boolean("LockTranslateY", "Translate Y", true, "GroupLockState"));
        ps.push_back(boolean("LockAngle", "Rotation Angle", true, "GroupLockState"));
        ps.push_back(boolean("LockScaleX", "Scale X", true, "GroupLockState"));
        ps.push_back(boolean("LockScaleY", "Scale Y", true, "GroupLockState"));
        ps.push_back(boolean("LockShear", "Shear", true, "GroupLockState"));
        ps.push_back(group("GroupBorders", "Borders"));
        ps.push_back(boolean("AvoidBlackBorders", "Auto Zoom", true, "GroupBorders"));
        ps.push_back(dbl("AdditionalScale", "Additional Zoom", 0, 0, 1, "GroupBorders"));
        ps.back().dmax = 0.5;
        ps.push_back(dbl("AdditionalRotation", "Additional Rotation", 0, -180, 180, "GroupBorders"));
        ps.push_back(point("AdditionalTranslation", "Additional Translation", 0.5, 0.5, "GroupBorders"));
        ps.push_back(choice("Zoom_Filling", "Edge Filling", 0, {"Transparent", "Mirror Padding", "Repeat", "Clamp to Edge"}, "GroupBorders"));
        ps.push_back(rgba("Zoom_ColorPicker", "Color", 0, 0, 1, 1, "GroupBorders"));
        ps.back().secret = true;            // (VEGAS keeps it for an edge filling it no longer offers)
        ps.push_back(group("GroupMotionBlur", "Motion Blur"));
        ps.push_back(boolean("MotionBlur", "Add Motion Blur", false, "GroupMotionBlur"));
        ps.push_back(dbl("MotionBlurThreshold", "Threshold", 10, 0, 1000, "GroupMotionBlur"));
        ps.back().dmax = 100;
        ps.push_back(dbl("MotionBlurMaxMagnitude", "Maximal Magnitude", 19, 0, 1000, "GroupMotionBlur"));
        ps.back().secret = true;      // (not in VEGAS's own list of the parameters it shows)
        ps.back().dmax = 39;
        ps.push_back(group("GroupVisualization", "Visualization"));
        ps.push_back(boolean("ShowFeatures", "Show Points", false, "GroupVisualization"));
        ps.push_back(intp("ShowFeaturesMinLen", "Minimum track length", 1, 1, 1000, "GroupVisualization"));
        // what VEGAS keeps of its analysis (the algorithm's settings and the motion found): kept in the project as they are
        const size_t hiddenFrom = ps.size();
        ps.push_back(intp("LastState", "Last state", 0, 0, 1000));
        ps.push_back(intp("GeometricModel", "Geometric model", 4, 0, 1000));
        ps.push_back(intp("Tracker", "Tracker", 2, 0, 1000));
        ps.push_back(intp("TrackerWinSize", "Tracker window size", 16, 0, 1000));
        ps.push_back(intp("TrackerLevel", "Tracker level", 4, 0, 1000));
        ps.push_back(intp("Detector", "Detector", 0, 0, 1000));
        ps.push_back(intp("PointHandling", "Point handling", 1, 0, 1000));
        ps.push_back(intp("Ransac", "Ransac", 0, 0, 1000));
        ps.push_back(intp("Smoothing", "Smoothing", 0, 0, 1000));
        ps.push_back(intp("TempoMode", "Tempo mode", 1, 0, 1000));
        ps.push_back(intp("SmoothingFrameLength", "Smoothing frame length", 75, 0, 100000));
        ps.push_back(boolean("IsStaticCamera", "Static Camera", false));
        ps.push_back(boolean("ClearState", "Clear state", false));
        ps.push_back(boolean("HasMotionData", "Has motion data", false));
        ps.push_back(custom("MotionData", "Motion data", ""));
        ps.push_back(intp("HasCorrectionData", "Has correction data", 0, 0, 1000));
        ps.push_back(custom("CorrectionData", "Correction data", ""));
        ps.push_back(dbl("FrameNo", "Frame number", 0, -1e9, 1e9));
        ps.push_back(custom("TrackData", "Track data", ""));
        for (size_t k = hiddenFrom; k < ps.size(); ++k) ps[k].secret = true;
        Def d = make("de.magix:Stabilize", "Video Stabilization",
                     "Steadies a shaky picture: the camera's path is followed from frame to frame and smoothed (or frozen), the picture moved to match and zoomed in to hide the edges. Reno analyses as it plays instead of in a separate Analyze Motion step",
                     ps, videoStabilization);
        d.changed = stabilizeChanged;
        d.temporal = true;
        d.destroy = destroyState;
        out.push_back(std::move(d));
    }
    {
        std::vector<PDef> ps;
        ps.push_back(dbl("PanSmoothing", "Pan smoothing", 0.5, 0, 1));
        ps.push_back(dbl("ViewScale", "Stabilization amount", 0.5, 0, 1));
        ps.push_back(boolean("RollingShutterCorrection", "Rolling shutter correction", false));
        ps.push_back(button("AnalyzeButton", "Apply"));
        const size_t hiddenFrom = ps.size();
        ps.push_back(boolean("HasM10StabData", "Has M10 stabilization data", false));
        ps.push_back(custom("M10StabData", "M10 stabilization data", ""));
        ps.push_back(boolean("HasM20StabData", "Has M20 stabilization data", false));
        ps.push_back(custom("M20StabDataHeader", "M20 stabilization data header", ""));
        ps.push_back(custom("M20StabData", "M20 stabilization data", ""));
        ps.push_back(intp("M20DataFramesPerBlob", "M20 data frames per blob", 0, 0, 1000000));
        ps.push_back(dbl("CurrentPanSmoothing", "Current pan smoothing", 0, 0, 1));
        ps.push_back(dbl("CurrentViewScale", "Current view scale", 0, 0, 1));
        ps.push_back(boolean("CurrentRollingShutterCorrection", "Current rolling shutter correction", false));
        for (size_t k = hiddenFrom; k < ps.size(); ++k) ps[k].secret = true;
        Def d = make("com.vegascreativesoftware:Stabilize", "Stabilize Video",
                     "Removes camera shake: the camera's path is followed and smoothed, the picture moved to match and zoomed in to hide the edges. Reno analyses as it plays instead of in a separate Analyze step",
                     ps, stabilizeVideo);
        d.changed = stabilizeVideoChanged;
        d.temporal = true;
        d.destroy = destroyState;
        out.push_back(std::move(d));
    }
}

}  // namespace analysis
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
