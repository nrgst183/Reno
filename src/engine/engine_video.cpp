// The compositor: a project's frame at a moment.
#include "engine.h"
#include "../core/pixrows.h"
#include "blendrows.h"
#include "opticalflow.h"
#include "profile.h"
#include "colormgmt.h"
#include "engine_internal.h"
#include "composememo.h"
#include "builtinfx.h"
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <limits>
#include <cstring>
#include <cstdlib>
#include <tuple>

namespace reno {

// ====================================================================== frames
static double frameNumber(double seconds, double fps)
{
    const double f = seconds * fps;
    const double r = std::floor(f + 0.5);
    return std::fabs(f - r) < 1e-4 ? r : f;        // a time meant to be on a frame is that frame exactly
}

static const double kDegrees = 3.14159265358979323846 / 180.0;

// ---------------------------------------------------------------------- keyframes
// The value of a list of keys (sorted by time or not) at 'time'.
template <class Key, class Mix, class Path>
static Key keyAt(const std::vector<Key>& keys, double time, Mix mix, Path path)
{
    if (keys.size() == 1) return keys[0];
    std::vector<const Key*> order;
    order.reserve(keys.size());
    for (const Key& k : keys) order.push_back(&k);
    std::stable_sort(order.begin(), order.end(), [](const Key* a, const Key* b) { return a->time < b->time; });
    if (time <= order.front()->time) return *order.front();
    if (time >= order.back()->time) return *order.back();
    for (size_t k = 0; k + 1 < order.size(); ++k) {
        const Key& a = *order[k];
        const Key& b = *order[k + 1];
        if (time < b.time) {
            const double span = b.time - a.time;
            const double f = span > 0 ? keyCurveShape(a.curve, (time - a.time) / span) : 1.0;
            Key out = mix(a, b, f);
            out.time = time;
            out.curve = a.curve;
            path(out, k > 0 ? *order[k - 1] : a, a, b, k + 2 < order.size() ? *order[k + 2] : b, f);
            return out;
        }
    }
    return *order.back();
}
template <class Key, class Mix>
static Key keyAt(const std::vector<Key>& keys, double time, Mix mix)
{
    return keyAt(keys, time, mix, [](Key&, const Key&, const Key&, const Key&, const Key&, double) {});
}

static double lerp(double a, double b, double f) { return a + (b - a) * f; }

static PanCropKey panCropAt(const std::vector<PanCropKey>& keys, double time)
{
    if (keys.empty()) return PanCropKey();
    return keyAt(keys, time, [](const PanCropKey& a, const PanCropKey& b, double f) {
        PanCropKey k;
        k.cx = lerp(a.cx, b.cx, f);
        k.cy = lerp(a.cy, b.cy, f);
        k.w = lerp(a.w, b.w, f);
        k.h = lerp(a.h, b.h, f);
        k.rotation = lerp(a.rotation, b.rotation, f);
        k.smoothness = a.smoothness;
        k.pivotX = lerp(a.pivotX, b.pivotX, f);
        k.pivotY = lerp(a.pivotY, b.pivotY, f);
        return k;
    }, [](PanCropKey& o, const PanCropKey& p0, const PanCropKey& p1, const PanCropKey& p2, const PanCropKey& p3, double f) {
        if (p1.smoothness > 0) smoothPath(p1.smoothness, p0.cx, p0.cy, p1.cx, p1.cy, p2.cx, p2.cy, p3.cx, p3.cy, f, o.cx, o.cy);
    });
}

static MotionKey motionAt(const std::vector<MotionKey>& keys, double time)
{
    if (keys.empty()) return MotionKey();
    return keyAt(keys, time, [](const MotionKey& a, const MotionKey& b, double f) {
        MotionKey k;
        k.x = lerp(a.x, b.x, f);
        k.y = lerp(a.y, b.y, f);
        k.w = lerp(a.w, b.w, f);
        k.h = lerp(a.h, b.h, f);
        k.rotation = lerp(a.rotation, b.rotation, f);
        k.z = lerp(a.z, b.z, f);
        k.d = lerp(a.d, b.d, f);
        k.rotX = lerp(a.rotX, b.rotX, f);
        k.rotY = lerp(a.rotY, b.rotY, f);
        k.oriX = lerp(a.oriX, b.oriX, f);
        k.oriY = lerp(a.oriY, b.oriY, f);
        k.oriZ = lerp(a.oriZ, b.oriZ, f);
        k.offX = lerp(a.offX, b.offX, f);
        k.offY = lerp(a.offY, b.offY, f);
        k.offZ = lerp(a.offZ, b.offZ, f);
        k.smoothness = a.smoothness;
        return k;
    }, [](MotionKey& o, const MotionKey& p0, const MotionKey& p1, const MotionKey& p2, const MotionKey& p3, double f) {
        if (p1.smoothness > 0) smoothPath(p1.smoothness, p0.x, p0.y, p1.x, p1.y, p2.x, p2.y, p3.x, p3.y, f, o.x, o.y);
    });
}

// Track Motion's 2D Shadow / 2D Glow at a time (everything, the colour's channels too, goes in a line between keys).
static GlowKey glowAt(const std::vector<GlowKey>& keys, double time)
{
    if (keys.empty()) return GlowKey();
    return keyAt(keys, time, [](const GlowKey& a, const GlowKey& b, double f) {
        GlowKey k;
        k.x = lerp(a.x, b.x, f);
        k.y = lerp(a.y, b.y, f);
        k.w = lerp(a.w, b.w, f);
        k.h = lerp(a.h, b.h, f);
        k.blur = lerp(a.blur, b.blur, f);
        k.intensity = lerp(a.intensity, b.intensity, f);
        uint32_t c = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            const double v = lerp((double)((a.color >> sh) & 0xff), (double)((b.color >> sh) & 0xff), f);
            c |= (uint32_t)std::min(255.0, std::max(0.0, std::floor(v + 0.5))) << sh;
        }
        k.color = c;
        return k;
    });
}

// Three box blurs one after another (close to a Gaussian) of a w x h plane, radius r, in place.
static void blurPlane(std::vector<float>& a, int w, int h, int r)
{
    if (r <= 0 || w <= 0 || h <= 0) return;
    auto pass = [&](float* base, int n, size_t stride, std::vector<float>& line) {
        // running sum over [i - r, i + r], edges count as transparent
        double sum = 0;
        for (int i = 0; i < std::min(n, r); ++i) sum += base[(size_t)i * stride];
        const float norm = 1.0f / (float)(2 * r + 1);
        for (int i = 0; i < n; ++i) {
            if (i + r < n) sum += base[(size_t)(i + r) * stride];
            line[(size_t)i] = (float)sum * norm;
            if (i - r >= 0) sum -= base[(size_t)(i - r) * stride];
        }
        for (int i = 0; i < n; ++i) base[(size_t)i * stride] = line[(size_t)i];
    };
    for (int it = 0; it < 3; ++it) {
        parallelRows(h, (size_t)w * 4, [&](int ya, int yb) {
            std::vector<float> line((size_t)std::max(w, h));
            for (int y = ya; y < yb; ++y) pass(a.data() + (size_t)y * w, w, 1, line);
        });
        parallelRows(w, (size_t)h * 4, [&](int xa, int xb) {
            std::vector<float> line((size_t)std::max(w, h));
            for (int x = xa; x < xb; ++x) pass(a.data() + x, h, (size_t)w, line);
        });
    }
}

// Lays 'shape''s alpha, blurred, in one colour, under 'dst' (both premultiplied, the same size and depth): VEGAS's
// 2D Shadow and 2D Glow.
static void underlayShape(Image& dst, const Image& shape, const GlowKey& g, int frameH)
{
    if (dst.w != shape.w || dst.h != shape.h || dst.depth != shape.depth) return;
    const int w = dst.w, h = dst.h;
    const bool fl = dst.depth != PixelDepth::Byte;
    std::vector<float> a((size_t)w * h);
    parallelRows(h, (size_t)w * 2, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            const uint8_t* row = shape.row(y);
            for (int x = 0; x < w; ++x)
                a[(size_t)y * w + x] = fl ? reinterpret_cast<const float*>(row)[x * 4 + 3] : row[x * 4 + 3] / 255.0f;
        }
    });
    blurPlane(a, w, h, (int)std::lround(std::min(1.0, std::max(0.0, g.blur)) * frameH / 20.0));
    const float op = (float)(std::min(1.0, std::max(0.0, g.intensity)) * ((g.color >> 24) & 0xff) / 255.0);
    const float cr = ((g.color >> 16) & 0xff) / 255.0f, cg = ((g.color >> 8) & 0xff) / 255.0f, cb = (g.color & 0xff) / 255.0f;
    parallelRows(h, (size_t)w * 4, [&](int ya, int yb) {
      for (int y = ya; y < yb; ++y) {
        uint8_t* row = dst.row(y);
        for (int x = 0; x < w; ++x) {
            const float sa = std::min(1.0f, a[(size_t)y * w + x] * op);
            if (sa <= 0.0f) continue;
            if (fl) {
                float* px = reinterpret_cast<float*>(row) + x * 4;
                const float keep = 1.0f - std::min(1.0f, std::max(0.0f, px[3]));
                px[0] += cr * sa * keep; px[1] += cg * sa * keep; px[2] += cb * sa * keep; px[3] += sa * keep;
            } else {
                uint8_t* px = row + x * 4;
                const float keep = 1.0f - px[3] / 255.0f;
                const float add[4] = {cb * sa, cg * sa, cr * sa, sa};      // BGRA
                for (int c = 0; c < 4; ++c) px[c] = (uint8_t)std::min(255.0f, px[c] + add[c] * keep * 255.0f + 0.5f);
            }
        }
      }
    });
}

// Whether the shortcuts that give the same pictures faster are used (see setComposeFastPaths); a test turns them off to compare.
static std::atomic<bool> gComposeFast{true};
static bool composeMemoOn() { return gComposeFast.load(); }
void setComposeFastPaths(bool on) { gComposeFast.store(on); }
static std::atomic<uint64_t> gFastAxisWarps{0}, gFastWarps{0}, gFastCopies{0}, gFastOwned{0};
static std::atomic<uint64_t> gMemoHits;
void composeFastStats(uint64_t& axisWarps, uint64_t& warps, uint64_t& layerCopies, uint64_t& framesNotCopied, uint64_t& memoHits)
{
    axisWarps = gFastAxisWarps.load(); warps = gFastWarps.load(); layerCopies = gFastCopies.load(); framesNotCopied = gFastOwned.load(); memoHits = gMemoHits.load();
}

// Finished warps (Pan/Crop, Track Motion) and frame blends kept for the next picture of a preview.
class WarpMemo {
public:
    void clear()
    {
        std::lock_guard<std::mutex> lk(mu_);
        list_.clear();
        bytes_ = 0;
    }
    ImagePtr find(const ImagePtr& a, const ImagePtr& b, const double* k, int n)
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (Entry& e : list_)
            if (e.a == a && e.b == b && e.n == n && std::memcmp(e.k, k, sizeof(double) * (size_t)n) == 0) { e.used = ++clock_; return e.out; }
        return ImagePtr();
    }
    void put(const ImagePtr& a, const ImagePtr& b, const double* k, int n, const ImagePtr& out)
    {
        if (!out || !a) return;
        const size_t cost = out->byteSize() + a->byteSize() + (b ? b->byteSize() : 0);
        if (cost > kMaxBytes / 2) return;
        std::lock_guard<std::mutex> lk(mu_);
        for (const Entry& e : list_)
            if (e.a == a && e.b == b && e.n == n && std::memcmp(e.k, k, sizeof(double) * (size_t)n) == 0) return;
        while (!list_.empty() && (bytes_ + cost > kMaxBytes || list_.size() >= kMaxEntries)) {
            size_t oldest = 0;
            for (size_t i = 1; i < list_.size(); ++i) if (list_[i].used < list_[oldest].used) oldest = i;
            bytes_ -= list_[oldest].cost;
            list_.erase(list_.begin() + (std::ptrdiff_t)oldest);
        }
        Entry e;
        e.a = a; e.b = b; e.out = out; e.n = n; e.cost = cost; e.used = ++clock_;
        std::memcpy(e.k, k, sizeof(double) * (size_t)n);
        bytes_ += cost;
        list_.push_back(std::move(e));
    }
private:
    struct Entry {
        ImagePtr a, b, out;
        double k[16];
        int n = 0;
        size_t cost = 0;
        uint64_t used = 0;
    };
    static const size_t kMaxBytes = 64u << 20;
    static const size_t kMaxEntries = 24;
    std::mutex mu_;
    std::vector<Entry> list_;
    size_t bytes_ = 0;
    uint64_t clock_ = 0;
};
static WarpMemo& warpMemo() { static WarpMemo m; return m; }
void clearComposeMemo() { warpMemo().clear(); }

// ---------------------------------------------------------------------- resampling
// How carefully frames are resampled (Project Properties' render quality, or a draft preview).
enum Quality { kDraft = 0, kPreview = 1, kGood = 2, kBest = 3 };

static Quality qualityOf(const std::string& s)
{
    if (s == "draft") return kDraft;
    if (s == "preview") return kPreview;
    if (s == "best") return kBest;
    return kGood;
}

// An affine map: out = (m[0]*x + m[1]*y + m[2], m[3]*x + m[4]*y + m[5]).
struct Affine {
    double m[6] = {1, 0, 0, 0, 1, 0};
    void apply(double x, double y, double& ox, double& oy) const
    {
        ox = m[0] * x + m[1] * y + m[2];
        oy = m[3] * x + m[4] * y + m[5];
    }
    // this after 'first': (this o first)(p) = this(first(p))
    Affine after(const Affine& f) const
    {
        Affine r;
        r.m[0] = m[0] * f.m[0] + m[1] * f.m[3];
        r.m[1] = m[0] * f.m[1] + m[1] * f.m[4];
        r.m[2] = m[0] * f.m[2] + m[1] * f.m[5] + m[2];
        r.m[3] = m[3] * f.m[0] + m[4] * f.m[3];
        r.m[4] = m[3] * f.m[1] + m[4] * f.m[4];
        r.m[5] = m[3] * f.m[2] + m[4] * f.m[5] + m[5];
        return r;
    }
};

// std::floor without the library call (the values are far inside the range of a 64-bit integer; anything else goes the library's way)
static inline double floorFast(double x)
{
    if (x > -4e18 && x < 4e18) {
        const double t = (double)(long long)x;
        return t > x ? t - 1.0 : t;
    }
    return std::floor(x);
}

// Reads 'src' (premultiplied 8-bit, bottom row first) at a point given top row first, with pixel centres on whole
// numbers.
static inline void sampleNearest(const Image& src, double x, double y, float out[4])
{
    int ix = (int)floorFast(x + 0.5), iy = (int)floorFast(y + 0.5);
    ix = std::min(src.w - 1, std::max(0, ix));
    iy = std::min(src.h - 1, std::max(0, iy));
    const uint8_t* p = src.row(src.h - 1 - iy) + (size_t)ix * 4;
    for (int k = 0; k < 4; ++k) out[k] = p[k];
}

#ifdef RENO_PIXROWS_SSE2
// The bilinear sample of an 8-bit frame as a register (the four channels; the same products and sums, in the same
// order, as sampleBilinear's plain loop).
static inline __m128 sampleBilinearV(const Image& src, double x, double y)
{
    const double fx0 = floorFast(x), fy0 = floorFast(y);
    const float fx = (float)(x - fx0), fy = (float)(y - fy0);
    int x0 = (int)fx0, y0 = (int)fy0;
    int x1 = x0 + 1, y1 = y0 + 1;
    if ((unsigned)x0 >= (unsigned)(src.w - 1) || (unsigned)y0 >= (unsigned)(src.h - 1)) {
        x0 = std::min(src.w - 1, std::max(0, x0));
        x1 = std::min(src.w - 1, std::max(0, x1));
        y0 = std::min(src.h - 1, std::max(0, y0));
        y1 = std::min(src.h - 1, std::max(0, y1));
    }
    const uint8_t* r0 = src.row(src.h - 1 - y0);
    const uint8_t* r1 = src.row(src.h - 1 - y1);
    const float wa = (1 - fx) * (1 - fy), wb = fx * (1 - fy), wc = (1 - fx) * fy, wd = fx * fy;
    const __m128i zero = _mm_setzero_si128();
    auto px = [&](const uint8_t* q) {
        int v;
        std::memcpy(&v, q, 4);
        return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(v), zero), zero));
    };
    __m128 r = _mm_mul_ps(px(r0 + (size_t)x0 * 4), _mm_set1_ps(wa));
    r = _mm_add_ps(r, _mm_mul_ps(px(r0 + (size_t)x1 * 4), _mm_set1_ps(wb)));
    r = _mm_add_ps(r, _mm_mul_ps(px(r1 + (size_t)x0 * 4), _mm_set1_ps(wc)));
    return _mm_add_ps(r, _mm_mul_ps(px(r1 + (size_t)x1 * 4), _mm_set1_ps(wd)));
}
#endif

static inline void sampleBilinear(const Image& src, double x, double y, float out[4])
{
#ifdef RENO_PIXROWS_SSE2
    _mm_storeu_ps(out, sampleBilinearV(src, x, y));
#else
    const double fx0 = floorFast(x), fy0 = floorFast(y);
    const float fx = (float)(x - fx0), fy = (float)(y - fy0);
    int x0 = (int)fx0, y0 = (int)fy0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = std::min(src.w - 1, std::max(0, x0));
    x1 = std::min(src.w - 1, std::max(0, x1));
    y0 = std::min(src.h - 1, std::max(0, y0));
    y1 = std::min(src.h - 1, std::max(0, y1));
    const uint8_t* r0 = src.row(src.h - 1 - y0);
    const uint8_t* r1 = src.row(src.h - 1 - y1);
    const uint8_t* a = r0 + (size_t)x0 * 4;
    const uint8_t* b = r0 + (size_t)x1 * 4;
    const uint8_t* c = r1 + (size_t)x0 * 4;
    const uint8_t* d = r1 + (size_t)x1 * 4;
    const float wa = (1 - fx) * (1 - fy), wb = fx * (1 - fy), wc = (1 - fx) * fy, wd = fx * fy;
    for (int k = 0; k < 4; ++k) out[k] = a[k] * wa + b[k] * wb + c[k] * wc + d[k] * wd;
#endif
}

// The same for float frames (values as stored, 0..1).
static inline void sampleNearestF(const Image& src, double x, double y, float out[4])
{
    int ix = (int)floorFast(x + 0.5), iy = (int)floorFast(y + 0.5);
    ix = std::min(src.w - 1, std::max(0, ix));
    iy = std::min(src.h - 1, std::max(0, iy));
    const float* p = (const float*)src.row(src.h - 1 - iy) + (size_t)ix * 4;
    for (int k = 0; k < 4; ++k) out[k] = p[k];
}

static inline void sampleBilinearF(const Image& src, double x, double y, float out[4])
{
    const double fx0 = floorFast(x), fy0 = floorFast(y);
    const float fx = (float)(x - fx0), fy = (float)(y - fy0);
    int x0 = (int)fx0, y0 = (int)fy0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = std::min(src.w - 1, std::max(0, x0));
    x1 = std::min(src.w - 1, std::max(0, x1));
    y0 = std::min(src.h - 1, std::max(0, y0));
    y1 = std::min(src.h - 1, std::max(0, y1));
    const float* r0 = (const float*)src.row(src.h - 1 - y0);
    const float* r1 = (const float*)src.row(src.h - 1 - y1);
    const float* a = r0 + (size_t)x0 * 4;
    const float* b = r0 + (size_t)x1 * 4;
    const float* c = r1 + (size_t)x0 * 4;
    const float* d = r1 + (size_t)x1 * 4;
    const float wa = (1 - fx) * (1 - fy), wb = fx * (1 - fy), wc = (1 - fx) * fy, wd = fx * fy;
    for (int k = 0; k < 4; ++k) out[k] = a[k] * wa + b[k] * wb + c[k] * wc + d[k] * wd;
}

// How much of a pixel lies inside an edge 'dist' pixels away (negative: outside), as 0..1.
static inline double edgeCover(double dist) { return std::min(1.0, std::max(0.0, dist + 0.5)); }

#ifdef RENO_PIXROWS_SSE2
// The warp of an 8-bit frame when the maps keep rows and columns apart (no turn or shear: the x of a source point
// depends only on the x of the pixel, the y only on its y).
struct AxisWarp {
    struct Col {                        // one sample position along an axis
        alignas(16) float w4[4] = {1, 0, 1, 0};     // the bilinear weights
        int a = 0, b = 0;               // the two taps
        std::ptrdiff_t oa = 0, ob = 0;  // the same as byte offsets into the converted rows
        float f = 0, g = 1;             // the bilinear weights of 'b' and of 'a' (1 - f)
        double cover = 1, soft = 1;     // the edge cover of the source's edge
        bool in = false;                // well inside everything: cover 1
        bool dead = true;               // cover 0 whatever the other axis is: no sample
    };
    const Image* src = nullptr;
    bool clip = false, nearest = false;
    int sub = 1;
    float subW = 1.f;
    double inX = 0, inY = 0, inU = 0, inV = 0, mw = 0, mh = 0, dsx = 1, dsy = 1, dux = 1, dvy = 1;
    Affine tl, ts;
    std::vector<Col> cols;              // dw * sub samples, the sub samples of a pixel together
    int cLo = 0, cHi = 0;               // the columns of the source any sample takes

    // the sample at 'p' along an axis; the others are that axis's coefficients, limits and the source's length on it
    void make(Col& c, double p, double tl0, double tl2, double ts0, double ts2, double size, double ds, double dloc, double inS, double inL, int len) const
    {
        const double u = tl0 * p + tl2;
        const double m = ts0 * u + ts2;
        c.in = m >= inS && m <= size - inS && (!clip || std::fabs(u) <= inL);
        c.cover = 1; c.soft = 1;
        if (c.in) {
            c.dead = false;
        } else {
            c.cover = edgeCover(std::min(m, (double)len - m) / ds);
            if (clip) c.soft = edgeCover((0.5 - std::fabs(u)) / dloc);
            c.dead = c.cover == 0.0 || c.soft == 0.0;
        }
        if (c.dead) { c.a = c.b = 0; c.f = 0; c.g = 1; return; }
        const double x = m - 0.5;
        if (nearest) {
            const int i = (int)floorFast(x + 0.5);
            c.a = c.b = std::min(len - 1, std::max(0, i));
            c.f = 0; c.g = 1;
            return;
        }
        const double fx0 = floorFast(x);
        c.f = (float)(x - fx0);
        c.g = 1 - c.f;
        const int x0 = (int)fx0, x1 = x0 + 1;
        c.a = std::min(len - 1, std::max(0, x0));
        c.b = std::min(len - 1, std::max(0, x1));
    }

    void prepare(int dw)
    {
        cols.resize((size_t)dw * (size_t)sub);
        int lo = src->w, hi = 0;
        for (int X = 0; X < dw; ++X)
            for (int sx = 0; sx < sub; ++sx) {
                Col& c = cols[(size_t)X * sub + sx];
                make(c, X + (sx + 0.5) / sub, tl.m[0], tl.m[2], ts.m[0], ts.m[2], mw, dsx, dux, inX, inU, src->w);
                c.w4[0] = c.g; c.w4[1] = c.f; c.w4[2] = c.g; c.w4[3] = c.f;
                if (!c.dead) { lo = std::min(lo, std::min(c.a, c.b)); hi = std::max(hi, std::max(c.a, c.b) + 1); }
            }
        cLo = lo < hi ? lo : 0;
        cHi = lo < hi ? hi : 0;
        for (Col& c : cols) { c.oa = (std::ptrdiff_t)(c.a - cLo) * 16; c.ob = (std::ptrdiff_t)(c.b - cLo) * 16; }
    }

    static inline __m128 px(const uint8_t* q)
    {
        int v;
        std::memcpy(&v, q, 4);
        return _mm_cvtepi32_ps(_mm_unpacklo_epi16(_mm_unpacklo_epi8(_mm_cvtsi32_si128(v), _mm_setzero_si128()), _mm_setzero_si128()));
    }

    // Source rows turned into floats (the columns [cLo, cHi) of them), kept while the next rows of the picture need
    // them again.
    struct Scratch {
        std::vector<float> buf;
        int idx[4] = {-1, -1, -1, -1};      // which source row
        int slots = 0, span = 0;
    };
    void setup(Scratch& s) const
    {
        s.slots = 2 * sub;
        s.span = cHi - cLo;
        s.buf.resize((size_t)s.slots * (size_t)std::max(1, s.span) * 4);
        for (int k = 0; k < 4; ++k) s.idx[k] = -1;
    }
    // the converted row y (counted from the top); 'need' are the rows the picture's row needs now (so that none of them is dropped)
    const float* fetch(Scratch& s, int y, const int* need, int nn) const
    {
        for (int k = 0; k < s.slots; ++k)
            if (s.idx[k] == y) return s.buf.data() + (size_t)k * s.span * 4;
        int v = 0;
        for (; v < s.slots; ++v) {
            bool used = false;
            for (int j = 0; j < nn; ++j) used = used || need[j] == s.idx[v];
            if (!used) break;
        }
        if (v >= s.slots) v = 0;
        s.idx[v] = y;
        float* d = s.buf.data() + (size_t)v * s.span * 4;
        const uint8_t* q = src->row(src->h - 1 - y) + (size_t)cLo * 4;
        const __m128i zero = _mm_setzero_si128();
        int i = 0;
        for (; i + 4 <= s.span; i += 4) {
            const __m128i b = _mm_loadu_si128((const __m128i*)(q + (size_t)i * 4));
            const __m128i lo = _mm_unpacklo_epi8(b, zero), hi = _mm_unpackhi_epi8(b, zero);
            _mm_storeu_ps(d + (size_t)i * 4, _mm_cvtepi32_ps(_mm_unpacklo_epi16(lo, zero)));
            _mm_storeu_ps(d + (size_t)i * 4 + 4, _mm_cvtepi32_ps(_mm_unpackhi_epi16(lo, zero)));
            _mm_storeu_ps(d + (size_t)i * 4 + 8, _mm_cvtepi32_ps(_mm_unpacklo_epi16(hi, zero)));
            _mm_storeu_ps(d + (size_t)i * 4 + 12, _mm_cvtepi32_ps(_mm_unpackhi_epi16(hi, zero)));
        }
        for (; i < s.span; ++i) _mm_storeu_ps(d + (size_t)i * 4, px(q + (size_t)i * 4));
        return d;
    }

    // one sample from converted rows: the taps of column c in the two rows (wr: the row weights, 1 - wy, 1 - wy, wy, wy)
    static inline __m128 sampleF(const char* r0, const char* r1, const Col& c, __m128 wr)
    {
        const __m128 w = _mm_mul_ps(_mm_load_ps(c.w4), wr);         // wa wb wc wd, each the same product as before
        __m128 r = _mm_mul_ps(_mm_loadu_ps((const float*)(r0 + c.oa)), _mm_shuffle_ps(w, w, 0x00));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps((const float*)(r0 + c.ob)), _mm_shuffle_ps(w, w, 0x55)));
        r = _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps((const float*)(r1 + c.oa)), _mm_shuffle_ps(w, w, 0xAA)));
        return _mm_add_ps(r, _mm_mul_ps(_mm_loadu_ps((const float*)(r1 + c.ob)), _mm_shuffle_ps(w, w, 0xFF)));
    }

    template <int SUB, bool NEAR_>
    void row(int Y, uint8_t* drow, int xa, int xb, Scratch& scr) const
    {
        Col rc[2];
        const uint8_t* r0[2] = {nullptr, nullptr};
        const char* f0[2] = {nullptr, nullptr};
        const char* f1[2] = {nullptr, nullptr};
        __m128 wr[2];
        bool rowIn = true, rowDead = true;
        int need[4], nn = 0;
        for (int sy = 0; sy < SUB; ++sy) {
            make(rc[sy], Y + (sy + 0.5) / SUB, tl.m[4], tl.m[5], ts.m[4], ts.m[5], mh, dsy, dvy, inY, inV, src->h);
            rowIn = rowIn && rc[sy].in;
            rowDead = rowDead && rc[sy].dead;
            if (!rc[sy].dead) { need[nn++] = rc[sy].a; need[nn++] = rc[sy].b; }
        }
        for (int sy = 0; sy < SUB; ++sy) {
            if (rc[sy].dead) continue;
            if (NEAR_) {
                r0[sy] = src->row(src->h - 1 - rc[sy].a);
            } else {
                f0[sy] = (const char*)fetch(scr, rc[sy].a, need, nn);
                f1[sy] = (const char*)fetch(scr, rc[sy].b, need, nn);
                wr[sy] = _mm_setr_ps(rc[sy].g, rc[sy].g, rc[sy].f, rc[sy].f);
            }
        }
        const __m128 half = _mm_set1_ps(0.5f), top = _mm_set1_ps(255.f), bot = _mm_setzero_ps();
        const Col* cp = cols.data() + (size_t)xa * SUB;
        for (int X = xa; X < xb; ++X, cp += SUB) {
            __m128 acc = _mm_setzero_ps();
            bool allIn = rowIn;
            for (int sx = 0; sx < SUB; ++sx) allIn = allIn && cp[sx].in;
            auto smp = [&](int sy, const Col& c) -> __m128 {
                if (NEAR_) return px(r0[sy] + (size_t)c.a * 4);
                return sampleF(f0[sy], f1[sy], c, wr[sy]);
            };
            if (allIn) {
                if (SUB == 1) {
                    acc = smp(0, cp[0]);
                } else {
                    __m128 t = smp(0, cp[0]);
                    t = _mm_add_ps(t, smp(0, cp[1]));
                    t = _mm_add_ps(t, smp(1, cp[0]));
                    t = _mm_add_ps(t, smp(1, cp[1]));
                    acc = _mm_mul_ps(t, _mm_set1_ps(subW));
                }
            } else if (!rowDead) {
                for (int sy = 0; sy < SUB; ++sy) {
                    if (rc[sy].dead) continue;
                    for (int sx = 0; sx < SUB; ++sx) {
                        const Col& c = cp[sx];
                        if (c.dead) continue;
                        double cover;
                        if (c.in && rc[sy].in) {
                            cover = 1.0;
                        } else {
                            cover = c.cover * rc[sy].cover;
                            if (clip) cover *= c.soft * rc[sy].soft;
                        }
                        if (cover <= 0.0) continue;
                        const __m128 fv = _mm_set1_ps((float)cover * subW);
                        acc = _mm_add_ps(acc, _mm_mul_ps(smp(sy, c), fv));
                    }
                }
            }
            __m128 a = _mm_add_ps(acc, half);
            a = _mm_min_ps(_mm_max_ps(a, bot), top);
            const __m128i i32 = _mm_cvttps_epi32(a);
            const __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(i32, i32), _mm_setzero_si128());
            const uint32_t word = (uint32_t)_mm_cvtsi128_si32(b8);
            const __m128i alv = _mm_set1_epi8((char)(word >> 24));
            const uint32_t out = (uint32_t)_mm_cvtsi128_si32(_mm_min_epu8(b8, alv));
            std::memcpy(drow + (size_t)X * 4, &out, 4);
        }
    }
};
#endif

// Draws 'src' into 'dst' (cleared first).
static void warp(const Image& src, Image& dst, const Affine& toLocal, const Affine& toSource, bool clipLocal, Quality q)
{
    if (src.w < 1 || src.h < 1 || src.depth != dst.depth) {
        dst.clear();
        return;
    }
    const bool fl = src.depth == PixelDepth::Float;
    const Affine full = toSource.after(toLocal);
    // how far one pixel of dst moves things in the local square and in src (for the soft edges)
    const double dux = std::max(1e-9, std::hypot(toLocal.m[0], toLocal.m[1]));
    const double dvy = std::max(1e-9, std::hypot(toLocal.m[3], toLocal.m[4]));
    const double dsx = std::max(1e-9, std::hypot(full.m[0], full.m[1]));
    const double dsy = std::max(1e-9, std::hypot(full.m[3], full.m[4]));
    const int sub = q == kBest ? 2 : 1;                         // best: 2x2 samples per pixel
    const float subW = 1.f / (float)(sub * sub);
    // Where a pixel can have any cover at all: a sample counts only where the source point.
    struct Limit { double a, b, c, lo, hi; };          // lo < a * px + b * py + c < hi
    Limit lim[8];
    int nLim = 0;
    bool finite = true;
    for (double v : {full.m[0], full.m[1], full.m[2], full.m[3], full.m[4], full.m[5], toLocal.m[0], toLocal.m[1], toLocal.m[2], toLocal.m[3], toLocal.m[4], toLocal.m[5]})
        if (!std::isfinite(v) || std::fabs(v) > 1e12) finite = false;
    const double mw = src.w, mh = src.h;
    if (finite) {
        const double e = 1e-6;
        lim[nLim++] = {full.m[0], full.m[1], full.m[2], -0.5 * dsx - e * (mw + dsx) - 1e-6, mw + 0.5 * dsx + e * (mw + dsx) + 1e-6};
        lim[nLim++] = {full.m[3], full.m[4], full.m[5], -0.5 * dsy - e * (mh + dsy) - 1e-6, mh + 0.5 * dsy + e * (mh + dsy) + 1e-6};
        if (clipLocal) {
            lim[nLim++] = {toLocal.m[0], toLocal.m[1], toLocal.m[2], -0.5 - 0.5 * dux - 1e-6, 0.5 + 0.5 * dux + 1e-6};
            lim[nLim++] = {toLocal.m[3], toLocal.m[4], toLocal.m[5], -0.5 - 0.5 * dvy - 1e-6, 0.5 + 0.5 * dvy + 1e-6};
        }
    }
    const double inner = 1e-6;
    const double inX = 0.5 * dsx * (1 + inner) + inner, inY = 0.5 * dsy * (1 + inner) + inner;
    const double inU = 0.5 - 0.5 * dux * (1 + inner) - inner, inV = 0.5 - 0.5 * dvy * (1 + inner) - inner;
    const int dw = dst.w;
#ifdef RENO_PIXROWS_SSE2
    // rows and columns apart (no turn): the per-column work is done once (AxisWarp)
    AxisWarp axis;
    const bool axisFast = gComposeFast.load() && !fl && finite && toLocal.m[1] == 0 && toLocal.m[3] == 0 && toSource.m[1] == 0 && toSource.m[3] == 0 && dsx < 1e6 && dsy < 1e6;
    gFastWarps.fetch_add(1, std::memory_order_relaxed);
    if (axisFast) gFastAxisWarps.fetch_add(1, std::memory_order_relaxed);
    if (axisFast) {
        axis.src = &src; axis.clip = clipLocal; axis.nearest = q == kDraft; axis.sub = sub; axis.subW = subW;
        axis.inX = inX; axis.inY = inY; axis.inU = inU; axis.inV = inV; axis.mw = mw; axis.mh = mh;
        axis.dsx = dsx; axis.dsy = dsy; axis.dux = dux; axis.dvy = dvy; axis.tl = toLocal; axis.ts = toSource;
        axis.prepare(dw);
    }
#endif
    parallelRows(dst.h, (size_t)dst.w * 32 * (size_t)(sub * sub), [&](int Y0, int Y1) {
#ifdef RENO_PIXROWS_SSE2
    AxisWarp::Scratch scratch;
    if (axisFast && !axis.nearest) axis.setup(scratch);
#endif
    for (int Y = Y0; Y < Y1; ++Y) {
        uint8_t* drow = dst.row(dst.h - 1 - Y);
        const size_t pxBytes = fl ? sizeof(float) * 4 : 4;
        int xa = 0, xb = dw;                                    // the run [xa, xb) to work out
        for (int k = 0; k < nLim && xa < xb; ++k) {
            const Limit& L = lim[k];
            // over the row's samples (py between Y and Y + 1), the constant part ranges over [c0, c1]
            const double cA = L.b * Y + L.c, cB = L.b * (Y + 1.0) + L.c;
            const double c0 = std::min(cA, cB), c1 = std::max(cA, cB);
            if (std::fabs(L.a) < 1e-12) {
                if (!(c1 > L.lo && c0 < L.hi)) xb = xa;
                continue;
            }
            double p0 = (L.lo - c1) / L.a, p1 = (L.hi - c0) / L.a;
            if (p0 > p1) std::swap(p0, p1);
            // samples are at X + (0.5 .. sub - 0.5) / sub, within [X, X + 1]
            xa = std::max(xa, (int)std::min((double)dw, std::max(0.0, std::floor(p0 - 1.0) - 1.0)));
            xb = std::min(xb, (int)std::max(0.0, std::min((double)dw, std::ceil(p1) + 2.0)));
        }
        if (xb <= xa) { xa = xb = 0; }
        if (xa > 0) std::memset(drow, 0, (size_t)xa * pxBytes);
        if (xb < dw) std::memset(drow + (size_t)xb * pxBytes, 0, (size_t)(dw - xb) * pxBytes);
#ifdef RENO_PIXROWS_SSE2
        if (axisFast) {
            if (axis.nearest) axis.row<1, true>(Y, drow, xa, xb, scratch);
            else if (sub == 1) axis.row<1, false>(Y, drow, xa, xb, scratch);
            else axis.row<2, false>(Y, drow, xa, xb, scratch);
            continue;
        }
#endif
        for (int X = xa; X < xb; ++X) {
#ifdef RENO_PIXROWS_SSE2
            if (sub == 1 && !fl && q != kDraft) {
                double u, v, mx, my;
                toLocal.apply(X + 0.5, Y + 0.5, u, v);
                toSource.apply(u, v, mx, my);
                if (mx >= inX && mx <= mw - inX && my >= inY && my <= mh - inY && (!clipLocal || (std::fabs(u) <= inU && std::fabs(v) <= inV))) {
                    __m128 a = _mm_add_ps(sampleBilinearV(src, mx - 0.5, my - 0.5), _mm_set1_ps(0.5f));
                    a = _mm_min_ps(_mm_max_ps(a, _mm_setzero_ps()), _mm_set1_ps(255.f));
                    const __m128i i32 = _mm_cvttps_epi32(a);
                    const __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(i32, i32), _mm_setzero_si128());
                    const uint32_t word = (uint32_t)_mm_cvtsi128_si32(b8);
                    const uint32_t al = word >> 24;
                    const __m128i alv = _mm_set1_epi8((char)al);
                    const uint32_t out = (uint32_t)_mm_cvtsi128_si32(_mm_min_epu8(b8, alv));
                    std::memcpy(drow + (size_t)X * 4, &out, 3);
                    drow[(size_t)X * 4 + 3] = (uint8_t)al;
                    continue;
                }
            }
#endif
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < sub; ++sy)
                for (int sx = 0; sx < sub; ++sx) {
                    const double px = X + (sx + 0.5) / sub, py = Y + (sy + 0.5) / sub;
                    double u, v, mx, my;
                    toLocal.apply(px, py, u, v);
                    toSource.apply(u, v, mx, my);
                    double cover;
                    if (mx >= inX && mx <= mw - inX && my >= inY && my <= mh - inY && (!clipLocal || (std::fabs(u) <= inU && std::fabs(v) <= inV))) {
                        cover = 1.0;
                    } else {
                        cover = edgeCover(std::min(mx, src.w - mx) / dsx) * edgeCover(std::min(my, src.h - my) / dsy);
                        if (clipLocal) cover *= edgeCover((0.5 - std::fabs(u)) / dux) * edgeCover((0.5 - std::fabs(v)) / dvy);
                    }
                    if (cover <= 0.0) continue;
                    float s[4];
                    if (fl) {
                        if (q == kDraft) sampleNearestF(src, mx - 0.5, my - 0.5, s);
                        else sampleBilinearF(src, mx - 0.5, my - 0.5, s);
                    } else if (q == kDraft) sampleNearest(src, mx - 0.5, my - 0.5, s);
                    else sampleBilinear(src, mx - 0.5, my - 0.5, s);
                    const float f = (float)cover * subW;
                    for (int k = 0; k < 4; ++k) acc[k] += s[k] * f;
                }
            if (fl) {
                float* d = (float*)drow + (size_t)X * 4;
                for (int k = 0; k < 4; ++k) d[k] = acc[k];
                continue;
            }
            uint8_t* d = drow + (size_t)X * 4;
            for (int k = 0; k < 4; ++k) {
                const float v = acc[k] + 0.5f;
                d[k] = (uint8_t)(v >= 255.f ? 255 : (v <= 0.f ? 0 : (int)v));
            }
            for (int k = 0; k < 3; ++k) if (d[k] > d[3]) d[k] = d[3];
        }
    }
    });
}

// ---------------------------------------------------------------------- 3D track motion
// VEGAS's 3D Source Alpha: a track's frame is a flat card placed in 3D space by its 3D Track Motion and seen through
// the project's camera. VEGAS's camera (vegas180k.dll's tCAMERASETTINGS::Init): it looks along +z from z = -cot(fov / 2), in
// units where the frame at z = 0 is 2 high (-1 to 1), through a vertical field of view of "Default Field of View" (a
// hidden setting, dDefaultFieldOfView, default 0.8726646 radians = 50 degrees). So the camera sees the unmoved frame
// exactly filling the view, and it is cot(25 degrees) / 2 = 1.0723 frame heights away.
static const double kCameraFov = 0.8726646259971648;       // radians, vertical
static double cameraDistance(int H) { return 0.5 * H / std::tan(kCameraFov * 0.5); }

struct Vec3 {
    double x = 0, y = 0, z = 0;
    Vec3() {}
    Vec3(double a, double b, double c) : x(a), y(b), z(c) {}
    Vec3 operator+(const Vec3& o) const { return Vec3(x + o.x, y + o.y, z + o.z); }
    Vec3 operator-(const Vec3& o) const { return Vec3(x - o.x, y - o.y, z - o.z); }
    Vec3 operator*(double f) const { return Vec3(x * f, y * f, z * f); }
};
static double dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec3 cross(const Vec3& a, const Vec3& b) { return Vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }

// A 3 x 3 matrix, rows first.
struct Mat3 {
    double m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    Vec3 operator*(const Vec3& v) const
    {
        return Vec3(m[0] * v.x + m[1] * v.y + m[2] * v.z, m[3] * v.x + m[4] * v.y + m[5] * v.z, m[6] * v.x + m[7] * v.y + m[8] * v.z);
    }
    Mat3 operator*(const Mat3& o) const
    {
        Mat3 r;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = m[i * 3] * o.m[j] + m[i * 3 + 1] * o.m[3 + j] + m[i * 3 + 2] * o.m[6 + j];
        return r;
    }
};

// The turn of a key's angles (degrees, in MotionKey's senses).
static Mat3 turn3d(double rx, double ry, double rz)
{
    const double cx = std::cos(rx * kDegrees), sx = std::sin(rx * kDegrees);
    const double cy = std::cos(ry * kDegrees), sy = std::sin(ry * kDegrees);
    const double cz = std::cos(rz * kDegrees), sz = std::sin(rz * kDegrees);
    Mat3 X, Y, Z;
    X.m[4] = cx; X.m[5] = -sx; X.m[7] = sx; X.m[8] = cx;
    Y.m[0] = cy; Y.m[2] = -sy; Y.m[6] = sy; Y.m[8] = cy;
    Z.m[0] = cz; Z.m[1] = -sz; Z.m[3] = sz; Z.m[4] = cz;
    return Z * Y * X;
}

// Where a key puts a point of the unmoved frame's space.
static Vec3 motion3d(const MotionKey& k, const Vec3& p, double Wd, double H)
{
    const Vec3 scaled(p.x * k.w, p.y * k.h, p.z * k.d);
    const Vec3 oriented = turn3d(k.oriX, k.oriY, k.oriZ) * scaled;
    const Vec3 pivot(k.offX * Wd, k.offY * Wd, -k.offZ * Wd * k.d);
    const Vec3 turned = pivot + turn3d(k.rotX, k.rotY, k.rotation) * (oriented - pivot);
    return turned + Vec3((k.x - 0.5) * Wd, (k.y - 0.5) * H, -k.z * Wd);
}

// A card in the 3D space: its frame.
struct Card3d {
    ImagePtr frame;
    std::vector<MotionKey> moves;
    float opacity = 1.f;
    int order = 0;                  // the track's place
};

// The card's middle and its across and down sides (u from -0.5 to 0.5 goes along 'across', v along 'down').
static void cardPlace(const Card3d& c, double Wd, double H, Vec3& centre, Vec3& across, Vec3& down)
{
    auto place = [&](double u, double v) {
        Vec3 p(u * Wd, v * H, 0);
        for (const MotionKey& k : c.moves) p = motion3d(k, p, Wd, H);
        return p;
    };
    centre = place(0, 0);
    across = place(1, 0) - centre;
    down = place(0, 1) - centre;
}

// Draws a card into 'dst' (W x H frame pixels, cleared first) as the camera sees it, and the distance of the card
// along each pixel's ray into 'depth'.
static void warp3d(const Image& src, const Card3d& card, double par, Image& dst, std::vector<float>& depth, Quality q)
{
    dst.clear();
    depth.assign((size_t)dst.w * dst.h, std::numeric_limits<float>::infinity());
    if (src.w < 1 || src.h < 1 || src.depth != dst.depth) return;
    const double W = dst.w, H = dst.h, Wd = W * par;
    const double D = cameraDistance(dst.h);
    Vec3 O, A, B;
    cardPlace(card, Wd, H, O, A, B);
    const Vec3 n = cross(A, B);
    const double an = dot(A, cross(B, n)), bn = dot(B, cross(n, A));
    if (!(std::fabs(an) > 1e-12) || !(std::fabs(bn) > 1e-12)) return;
    const Vec3 aStar = cross(B, n) * (1.0 / an), bStar = cross(n, A) * (1.0 / bn);
    const Vec3 C(0, 0, -D);
    const Vec3 CO = C - O;
    const double k = -dot(n, CO);           // n . (O - C)
    // The ray through frame point (px, py): r = (px * par - Wd / 2, py - H / 2, D) = R0 + px * Rx + py * Ry.
    const Vec3 R0(-Wd / 2, -H / 2, D), Rx(par, 0, 0), Ry(0, 1, 0);
    // t = k / (n . r); u = (CO . a*) + t (r . a*) = [(CO . a*)(n . r) + k (r . a*)] / (n . r); the same for v.
    auto lin = [&](const Vec3& v) { return std::array<double, 3>{{dot(v, R0), dot(v, Rx), dot(v, Ry)}}; };
    const auto N = lin(n), Ua = lin(aStar), Vb = lin(bStar);
    const double cu = dot(CO, aStar), cv = dot(CO, bStar);
    double U[3], V[3];
    for (int i = 0; i < 3; ++i) {
        U[i] = cu * N[i] + k * Ua[i];
        V[i] = cv * N[i] + k * Vb[i];
    }
    // the card -> the frame's pixels (shrunk first with a proper average if it is seen much smaller, so that it
    // does not sparkle); how much smaller: at the card's middle
    const Image* from = &src;
    ImagePtr small;
    double sw = src.w, sh = src.h, ox = 0, oy = 0;
    {
        const double cz = O.z + D;
        if (cz > 1e-6) {
            const double px = (O.x * D / cz + Wd / 2) / par, py = O.y * D / cz + H / 2;
            const double c = N[0] + N[1] * px + N[2] * py;
            if (std::fabs(c) > 1e-12) {
                const double u = (U[0] + U[1] * px + U[2] * py) / c, v = (V[0] + V[1] * px + V[2] * py) / c;
                const double dux = (U[1] - N[1] * u) / c, duy = (U[2] - N[2] * u) / c;
                const double dvx = (V[1] - N[1] * v) / c, dvy = (V[2] - N[2] * v) / c;
                const double shrink = std::sqrt(std::fabs(dux * dvy - duy * dvx) * src.w * src.h);
                if (shrink > 1.8 && q != kDraft) {
                    small = fitImage(src, std::max(1, (int)std::ceil(src.w / shrink)), std::max(1, (int)std::ceil(src.h / shrink)));
                    if (small) {
                        const double f = std::min((double)small->w / src.w, (double)small->h / src.h);
                        ox = (small->w - src.w * f) / 2;
                        oy = (small->h - src.h * f) / 2;
                        sw = src.w * f;
                        sh = src.h * f;
                        from = small.get();
                    }
                }
            }
        }
    }
    const bool fl = src.depth == PixelDepth::Float;
    const int sub = q == kBest ? 2 : 1;
    const float subW = 1.f / (float)(sub * sub);
    const double nearT = 0.02;
    parallelRows(dst.h, (size_t)dst.w * 40 * (size_t)(sub * sub), [&](int Ya, int Yb) {
    for (int Y = Ya; Y < Yb; ++Y) {
        uint8_t* drow = dst.row(dst.h - 1 - Y);
        float* zrow = depth.data() + (size_t)Y * dst.w;
        for (int X = 0; X < dst.w; ++X) {
            float acc[4] = {0, 0, 0, 0};
            double nearest = std::numeric_limits<double>::infinity();
            for (int sy = 0; sy < sub; ++sy)
                for (int sx = 0; sx < sub; ++sx) {
                    const double px = X + (sx + 0.5) / sub, py = Y + (sy + 0.5) / sub;
                    const double c = N[0] + N[1] * px + N[2] * py;
                    if (!(std::fabs(c) > 1e-12)) continue;
                    const double t = k / c;
                    if (!(t > nearT)) continue;
                    const double u = (U[0] + U[1] * px + U[2] * py) / c, v = (V[0] + V[1] * px + V[2] * py) / c;
                    // soft edges: how far inside the card's square the pixel is, in pixels
                    const double gu = std::max(1e-9, std::hypot((U[1] - N[1] * u) / c, (U[2] - N[2] * u) / c));
                    const double gv = std::max(1e-9, std::hypot((V[1] - N[1] * v) / c, (V[2] - N[2] * v) / c));
                    const double cover = edgeCover((0.5 - std::fabs(u)) / gu) * edgeCover((0.5 - std::fabs(v)) / gv);
                    if (cover <= 0.0) continue;
                    const double mx = ox + (u + 0.5) * sw, my = oy + (v + 0.5) * sh;
                    float s4[4];
                    if (fl) {
                        if (q == kDraft) sampleNearestF(*from, mx - 0.5, my - 0.5, s4);
                        else sampleBilinearF(*from, mx - 0.5, my - 0.5, s4);
                    } else if (q == kDraft) sampleNearest(*from, mx - 0.5, my - 0.5, s4);
                    else sampleBilinear(*from, mx - 0.5, my - 0.5, s4);
                    const float f = (float)cover * subW;
                    for (int i = 0; i < 4; ++i) acc[i] += s4[i] * f;
                    nearest = std::min(nearest, t);
                }
            if (nearest == std::numeric_limits<double>::infinity()) continue;
            zrow[X] = (float)nearest;
            if (fl) {
                float* d = (float*)drow + (size_t)X * 4;
                for (int i = 0; i < 4; ++i) d[i] = acc[i];
                continue;
            }
            uint8_t* d = drow + (size_t)X * 4;
            for (int i = 0; i < 4; ++i) {
                const float v = acc[i] + 0.5f;
                d[i] = (uint8_t)(v >= 255.f ? 255 : (v <= 0.f ? 0 : (int)v));
            }
            for (int i = 0; i < 3; ++i) if (d[i] > d[3]) d[i] = d[3];
        }
    }
    });
}

// Lays drawn cards over 'canvas' pixel by pixel, the furthest first (so that cards that cross each other are cut
// where they meet), each at its opacity.
static void layCards(Image& canvas, const std::vector<ImagePtr>& pics, const std::vector<std::vector<float>>& depths,
                     const std::vector<Card3d>& cards)
{
    const size_t n = pics.size();
    const bool fl = canvas.depth == PixelDepth::Float;
    parallelRows(canvas.h, (size_t)canvas.w * (8 + 8 * n), [&](int Ya, int Yb) {
    std::vector<size_t> at;
    at.reserve(n);
    for (int Y = Ya; Y < Yb; ++Y) {
        const int row = canvas.h - 1 - Y;
        for (int X = 0; X < canvas.w; ++X) {
            const size_t px = (size_t)Y * canvas.w + X;
            at.clear();
            for (size_t i = 0; i < n; ++i)
                if (depths[i][px] != std::numeric_limits<float>::infinity()) at.push_back(i);
            if (at.empty()) continue;
            // furthest first; at the same depth the lower track first (a few cards: a plain insertion sort)
            for (size_t a = 1; a < at.size(); ++a)
                for (size_t b = a; b > 0; --b) {
                    const float za = depths[at[b - 1]][px], zb = depths[at[b]][px];
                    const bool same = std::fabs(za - zb) <= 1e-6f * std::max(1.f, std::fabs(za));
                    const bool swap = same ? cards[at[b - 1]].order > cards[at[b]].order : za < zb;
                    if (!swap) break;
                    std::swap(at[b - 1], at[b]);
                }
            if (fl) {
                float* d = (float*)canvas.row(row) + (size_t)X * 4;
                for (size_t i : at) {
                    const float* s = (const float*)pics[i]->row(row) + (size_t)X * 4;
                    const float o = cards[i].opacity, sa = s[3] * o;
                    for (int c = 0; c < 4; ++c) d[c] = s[c] * o + d[c] * (1.f - sa);
                }
                continue;
            }
            uint8_t* d = canvas.row(row) + (size_t)X * 4;
            using pix::div255;
            for (size_t i : at) {
                const uint8_t* s = pics[i]->row(row) + (size_t)X * 4;
                const unsigned op = (unsigned)(std::min(1.f, std::max(0.f, cards[i].opacity)) * 255.f + 0.5f);
                const unsigned a = op == 255 ? s[3] : div255(s[3] * op), inv = 255 - a;
                for (int c = 0; c < 4; ++c) {
                    const unsigned v = (op == 255 ? s[c] : div255(s[c] * op)) + div255(d[c] * inv);
                    d[c] = (uint8_t)(v > 255 ? 255 : v);
                }
            }
        }
    }
    });
}

void trackMotion3dPoint(const std::vector<MotionKey>& moves, double u, double v, int W, int H, double par, double out[3])
{
    const double Wd = W * par;
    Vec3 p(u * Wd, v * H, 0);
    for (const MotionKey& k : moves) p = motion3d(k, p, Wd, H);
    out[0] = p.x;
    out[1] = p.y;
    out[2] = p.z;
}

double trackMotion3dCamera(int W, int H, double par)
{
    (void)W;
    (void)par;
    return cameraDistance(H);
}

MotionKey motionKeyAt(const std::vector<MotionKey>& keys, double t) { return motionAt(keys, t); }

// ---------------------------------------------------------------------- compositing modes
// VEGAS's compositing modes (the Blend enum is in blendrows.h).

static Blend blendOf(const std::string& s)
{
    static const struct { const char* name; Blend b; } names[] = {
        {"add", Blend::Add}, {"subtract", Blend::Subtract}, {"multiply", Blend::Multiply}, {"screen", Blend::Screen},
        {"overlay", Blend::Overlay}, {"hardlight", Blend::HardLight}, {"dodge", Blend::Dodge}, {"burn", Blend::Burn},
        {"darken", Blend::Darken}, {"lighten", Blend::Lighten}, {"difference", Blend::Difference},
        {"diffsquared", Blend::DiffSquared}, {"differencesquared", Blend::DiffSquared}, {"cut", Blend::Cut},
        // "srcalpha", "3dsrcalpha" (where laid flat; 3D tracks are drawn by Pass::drawCards) and "custom".
    };
    for (const auto& n : names)
        if (s == n.name) return n.b;
    return Blend::Over;
}

// The colour a mode gives for a top colour s over a bottom colour d (both straight, 0..1).
static inline float blendChannel(Blend m, float s, float d)
{
    switch (m) {
    case Blend::Add: return std::min(1.f, s + d);
    case Blend::Subtract: return std::max(0.f, d - s);
    case Blend::Multiply: return s * d;
    case Blend::Screen: return s + d - s * d;
    case Blend::Overlay: return d <= 0.5f ? 2.f * s * d : 1.f - 2.f * (1.f - s) * (1.f - d);
    case Blend::HardLight: return s <= 0.5f ? 2.f * s * d : 1.f - 2.f * (1.f - s) * (1.f - d);
    case Blend::Dodge: return d <= 0.f ? 0.f : (s >= 1.f ? 1.f : std::min(1.f, d / (1.f - s)));
    case Blend::Burn: return d >= 1.f ? 1.f : (s <= 0.f ? 0.f : 1.f - std::min(1.f, (1.f - d) / s));
    case Blend::Darken: return std::min(s, d);
    case Blend::Lighten: return std::max(s, d);
    case Blend::Difference: return std::fabs(s - d);
    case Blend::DiffSquared: return (s - d) * (s - d);
    default: return s;
    }
}

// Lays 'src' over 'dst' (both premultiplied 8-bit, same size) with a compositing mode, src scaled by 'opacity'.
static void blendLayerFloat(Image& dst, const Image& src, float opacity, Blend mode);

template <Blend M>
static void blendRows(Image& dst, const Image& src, float opacity, int y0, int y1)
{
    const float k = 1.f / 255.f;
    for (int y = y0; y < y1; ++y) {
        uint8_t* d = dst.row(y);
        const uint8_t* s = src.row(y);
        const int done = pix::blendModeRow<M>(d, s, dst.w, opacity);
        d += (size_t)done * 4;
        s += (size_t)done * 4;
        for (int x = done; x < dst.w; ++x, d += 4, s += 4) {
            const float sa = s[3] * k * opacity;
            if (sa <= 0.f) continue;
            const float da = d[3] * k;
            float out[4];
            if (M == Blend::Cut) {
                for (int c = 0; c < 4; ++c) out[c] = d[c] * k * (1.f - sa);
            } else {
                for (int c = 0; c < 3; ++c) {
                    const float sp = s[c] * k * opacity, dp = d[c] * k;
                    const float sc = std::min(1.f, sp / sa);
                    const float dc = da > 0.f ? std::min(1.f, dp / da) : 0.f;
                    out[c] = sp * (1.f - da) + dp * (1.f - sa) + sa * da * blendChannel(M, sc, dc);
                }
                out[3] = sa + da - sa * da;
            }
            for (int c = 0; c < 4; ++c) {
                const float v = out[c] * 255.f + 0.5f;
                d[c] = (uint8_t)(v >= 255.f ? 255 : (v <= 0.f ? 0 : (int)v));
            }
            for (int c = 0; c < 3; ++c) if (d[c] > d[3]) d[c] = d[3];
        }
    }
}

static void blendLayer(Image& dst, const Image& src, float opacity, Blend mode)
{
    if (dst.w != src.w || dst.h != src.h || dst.depth != src.depth) return;
    if (opacity <= 0.f) return;
    if (mode == Blend::Over) { compositeOver(dst, src, opacity); return; }
    if (dst.depth == PixelDepth::Float) { blendLayerFloat(dst, src, opacity, mode); return; }
    opacity = std::min(1.f, opacity);
    void (*rows)(Image&, const Image&, float, int, int) = nullptr;
    switch (mode) {
    case Blend::Add: rows = blendRows<Blend::Add>; break;
    case Blend::Subtract: rows = blendRows<Blend::Subtract>; break;
    case Blend::Multiply: rows = blendRows<Blend::Multiply>; break;
    case Blend::Screen: rows = blendRows<Blend::Screen>; break;
    case Blend::Overlay: rows = blendRows<Blend::Overlay>; break;
    case Blend::HardLight: rows = blendRows<Blend::HardLight>; break;
    case Blend::Dodge: rows = blendRows<Blend::Dodge>; break;
    case Blend::Burn: rows = blendRows<Blend::Burn>; break;
    case Blend::Darken: rows = blendRows<Blend::Darken>; break;
    case Blend::Lighten: rows = blendRows<Blend::Lighten>; break;
    case Blend::Difference: rows = blendRows<Blend::Difference>; break;
    case Blend::DiffSquared: rows = blendRows<Blend::DiffSquared>; break;
    case Blend::Cut: rows = blendRows<Blend::Cut>; break;
    default: rows = blendRows<Blend::Over>; break;
    }
    parallelRows(dst.h, (size_t)dst.w * 16, [&](int y0, int y1) { rows(dst, src, opacity, y0, y1); });
}

// The same in float (32-bit projects): nothing is rounded or clipped to 0..1 on the way out.
static void blendLayerFloat(Image& dst, const Image& src, float opacity, Blend mode)
{
    opacity = std::min(1.f, opacity);
    parallelRows(dst.h, (size_t)dst.w * 16, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y) {
        float* d = (float*)dst.row(y);
        const float* s = (const float*)src.row(y);
        for (int x = 0; x < dst.w; ++x, d += 4, s += 4) {
            const float sa = s[3] * opacity;
            if (sa <= 0.f) continue;
            const float da = d[3];
            if (mode == Blend::Cut) {
                for (int c = 0; c < 4; ++c) d[c] = d[c] * (1.f - sa);
                continue;
            }
            float out[4];
            for (int c = 0; c < 3; ++c) {
                const float sp = s[c] * opacity, dp = d[c];
                const float sc = sp / sa;
                const float dc = da > 0.f ? dp / da : 0.f;
                const float m = mode == Blend::Add ? std::max(0.f, sc + dc) : blendChannel(mode, std::min(1.f, sc), std::min(1.f, dc));
                out[c] = sp * (1.f - da) + dp * (1.f - sa) + sa * da * m;
            }
            out[3] = sa + da - sa * da;
            for (int c = 0; c < 4; ++c) d[c] = out[c];
        }
    }
    });
}

// VEGAS's Multiply (Mask) as a compositing parent uses it: the parent's frame multiplies its children's, every
// channel of them including how solid they are (premultiplied).
static void maskMultiply(Image& group, const Image& mask, float opacity)
{
    if (group.w != mask.w || group.h != mask.h || group.depth != mask.depth || opacity <= 0.f) return;
    opacity = std::min(1.f, opacity);
    const bool fl = group.depth == PixelDepth::Float;
    parallelRows(group.h, (size_t)group.w * 8, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        if (fl) {
            float* g = (float*)group.row(y);
            const float* m = (const float*)mask.row(y);
            for (int i = 0, n = group.w * 4; i < n; ++i) g[i] *= 1.f - opacity + opacity * m[i];
        } else {
            uint8_t* g = group.row(y);
            const uint8_t* m = mask.row(y);
            const unsigned op = (unsigned)(opacity * 255.f + 0.5f);
            for (int i = 0, n = group.w * 4; i < n; ++i) {
                const unsigned f = (255 - op) * 255 + op * m[i];               // 0 .. 255*255
                g[i] = (uint8_t)((g[i] * f + 32512) / 65025);
            }
        }
    }
    });
}

// VEGAS's fade to colour (the "fadecolor" envelope): 'v' 0..1 towards the top colour, 0..-1 towards the bottom one
// (ARGB).
static void fadeToColour(Image& img, double v, uint32_t top, uint32_t bottom)
{
    if (!(std::fabs(v) > 1e-6)) return;
    const uint32_t c = v > 0 ? top : bottom;
    const float k = (float)std::min(1.0, std::fabs(v));
    const float cr = ((c >> 16) & 255) / 255.f, cg = ((c >> 8) & 255) / 255.f, cb = (c & 255) / 255.f;
    parallelRows(img.h, (size_t)img.w * 8, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        if (img.depth == PixelDepth::Float) {
            float* d = (float*)img.row(y);
            for (int x = 0; x < img.w; ++x, d += 4) {
                d[0] = d[0] * (1.f - k) + cr * k * d[3];
                d[1] = d[1] * (1.f - k) + cg * k * d[3];
                d[2] = d[2] * (1.f - k) + cb * k * d[3];
            }
        } else {
            uint8_t* d = img.row(y);
            for (int x = 0; x < img.w; ++x, d += 4) {
                const float a = d[3];
                const float o[3] = {d[0] * (1.f - k) + cb * k * a, d[1] * (1.f - k) + cg * k * a, d[2] * (1.f - k) + cr * k * a};
                for (int q = 0; q < 3; ++q) {
                    const float w = std::min(a, o[q]) + 0.5f;
                    d[q] = (uint8_t)(w <= 0.f ? 0 : (w >= 255.f ? 255 : (int)w));
                }
            }
        }
    }
    });
}

// Makes a frame solid (onto black), in place.
static void makeOpaque(Image& img)
{
    parallelRows(img.h, (size_t)img.w * 2, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            if (img.depth == PixelDepth::Float) {
                float* d = (float*)img.row(y);
                for (int x = 0; x < img.w; ++x) d[x * 4 + 3] = 1.f;
            } else {
                pix::opaqueRow(img.row(y), img.w);
            }
        }
    });
}

// Where fitImage puts a frame of mw x mh into a w x h frame, as a rectangle from the top left (the same rounding).
struct FitRect { double x = 0, y = 0, w = 0, h = 0; };
static FitRect fittedMediaRect(double mw, double mh, int w, int h)
{
    FitRect r;
    if (!(mw > 0 && mh > 0)) { r.w = w; r.h = h; return r; }
    const double s = std::min((double)w / mw, (double)h / mh);
    const int dw = std::min(w, std::max(1, (int)(mw * s + 0.5))), dh = std::min(h, std::max(1, (int)(mh * s + 0.5)));
    r.x = (w - dw) / 2;
    const int bottom = (h - dh) / 2;
    r.y = h - bottom - dh;
    r.w = dw;
    r.h = dh;
    return r;
}

// Event Pan/Crop's box at time t, and the size it is drawn at in a W x H frame of pixel aspect 'par'.
struct BoxPlacement {
    PanCropKey box;
    double dw = 0, dh = 0;                  // the size it is drawn at in the frame, in frame pixels
};
static BoxPlacement placeBoxOf(const Event& e, double t, double mediaW, double mediaH, int W, int H, double par)
{
    BoxPlacement bp;
    bp.box = panCropAt(e.panCrop, t - e.start);
    const double bw = std::max(1e-6, std::fabs(bp.box.w)), bh = std::max(1e-6, std::fabs(bp.box.h));
    bp.dw = W;
    bp.dh = H;
    if (e.keepAspect && mediaW > 0 && mediaH > 0) {
        const double boxAspect = (bw * mediaW) / (bh * mediaH);       // as seen (media pixels are square)
        const double frameAspect = (W * par) / H;
        // fitted inside the frame (bars where the shapes differ), or with Stretch to fill frame covering it (the
        // frame past the frame's edges is cut off)
        if ((boxAspect > frameAspect) != e.stretchToFill) bp.dh = W * par / boxAspect;
        else bp.dw = H * boxAspect / par;
    }
    // (the rotation centre, pivotX / pivotY, is where the Pan/Crop window turns the box about; the key's centre is
    // where the box is once turned, as VEGAS keeps it)
    return bp;
}

// VEGAS's Reduce Interlace Flicker switch: each row mixed with the rows above and below it.
static ImagePtr reduceFlicker(const Image& src)
{
    if (src.h < 3 || src.w < 1) return src.clone();
    ImagePtr out = Image::make(src.w, src.h, src.depth, false);
    if (!out) return src.clone();
    parallelRows(src.h, (size_t)src.w * 8, [&](int r0, int r1) {
    for (int y = r0; y < r1; ++y) {
        const int ya = std::max(0, y - 1), yb = std::min(src.h - 1, y + 1);
        if (src.depth == PixelDepth::Byte) {
            const uint8_t *a = src.row(ya), *b = src.row(y), *c = src.row(yb);
            uint8_t* d = out->row(y);
            for (size_t k = 0; k < (size_t)src.w * 4; ++k) d[k] = (uint8_t)((a[k] + 2 * b[k] + c[k] + 2) / 4);
        } else {
            const float *a = (const float*)src.row(ya), *b = (const float*)src.row(y), *c = (const float*)src.row(yb);
            float* d = (float*)out->row(y);
            for (size_t k = 0; k < (size_t)src.w * 4; ++k) d[k] = 0.25f * a[k] + 0.5f * b[k] + 0.25f * c[k];
        }
    }
    });
    return out;
}

// The inverse of an affine map (the identity if it has none).
static Affine inverseOf(const Affine& a)
{
    const double det = a.m[0] * a.m[4] - a.m[1] * a.m[3];
    Affine r;
    if (!(std::fabs(det) > 1e-15)) return r;
    r.m[0] = a.m[4] / det;
    r.m[1] = -a.m[1] / det;
    r.m[3] = -a.m[3] / det;
    r.m[4] = a.m[0] / det;
    r.m[2] = -(r.m[0] * a.m[2] + r.m[1] * a.m[5]);
    r.m[5] = -(r.m[3] * a.m[2] + r.m[4] * a.m[5]);
    return r;
}

// ---------------------------------------------------------------------- Event Pan/Crop's mask
// Media fraction (0..1 across the media's frame, from the top left) -> frame pixel.
static Affine maskToFrame(const Event& e, const MediaItem* m, double t, int W, int H, double par)
{
    double mw = 0, mh = 0;
    if (m) m->shownSize(mw, mh);
    if (!m || m->generated || !(mw > 0 && mh > 0)) { mw = W; mh = H; }
    const BoxPlacement bp = placeBoxOf(e, t, mw, mh, W, H, par);
    const PanCropKey& b = bp.box;
    const double c = std::cos(b.rotation * kDegrees), sn = std::sin(b.rotation * kDegrees);
    Affine fromSquare;
    fromSquare.m[0] = c * b.w; fromSquare.m[1] = -sn * b.h * mh / mw; fromSquare.m[2] = b.cx;
    fromSquare.m[3] = sn * b.w * mw / mh; fromSquare.m[4] = c * b.h; fromSquare.m[5] = b.cy;
    Affine toFrame;
    toFrame.m[0] = bp.dw; toFrame.m[2] = 0.5 * W;
    toFrame.m[4] = bp.dh; toFrame.m[5] = 0.5 * H;
    return toFrame.after(inverseOf(fromSquare));
}

// The outline of a closed Bezier path as a polygon in frame pixels (curved segments cut into short lines).
static std::vector<std::pair<double, double>> maskOutline(const MaskPath& path, const Affine& toFrame)
{
    std::vector<std::pair<double, double>> poly;
    const size_t n = path.points.size();
    for (size_t i = 0; i < n; ++i) {
        const MaskPoint& a = path.points[i];
        const MaskPoint& b = path.points[(i + 1) % n];
        double x0, y0, x1, y1, x2, y2, x3, y3;
        toFrame.apply(a.x, a.y, x0, y0);
        toFrame.apply(a.x + a.outX, a.y + a.outY, x1, y1);
        toFrame.apply(b.x + b.inX, b.y + b.inY, x2, y2);
        toFrame.apply(b.x, b.y, x3, y3);
        const bool straight = a.outX == 0 && a.outY == 0 && b.inX == 0 && b.inY == 0;
        const double len = std::hypot(x1 - x0, y1 - y0) + std::hypot(x2 - x1, y2 - y1) + std::hypot(x3 - x2, y3 - y2);
        const int steps = straight ? 1 : std::max(2, std::min(64, (int)(len / 4.0)));
        for (int k = 0; k < steps; ++k) {
            const double u = (double)k / steps, v = 1.0 - u;
            const double w0 = v * v * v, w1 = 3 * v * v * u, w2 = 3 * v * u * u, w3 = u * u * u;
            poly.push_back({w0 * x0 + w1 * x1 + w2 * x2 + w3 * x3, w0 * y0 + w1 * y1 + w2 * y2 + w3 * y3});
        }
    }
    return poly;
}

// A polygon's coverage of each pixel of a W x H frame.
static void fillPolygon(const std::vector<std::pair<double, double>>& poly, int W, int H, bool aa, std::vector<float>& cov)
{
    cov.assign((size_t)W * H, 0.f);
    if (poly.size() < 3) return;
    double ylo = 1e300, yhi = -1e300;
    for (const auto& q : poly) {
        if (!std::isfinite(q.first) || !std::isfinite(q.second)) return;
        ylo = std::min(ylo, q.second);
        yhi = std::max(yhi, q.second);
    }
    const int y0 = std::max(0, (int)std::floor(ylo)), y1 = std::min(H - 1, (int)std::ceil(yhi));
    const int sub = aa ? 4 : 1;
    parallelRows(y1 - y0 + 1, (size_t)(W + 64) * (size_t)sub, [&](int band0, int band1) {
    std::vector<std::pair<double, int>> xs;
    for (int y = y0 + band0; y < y0 + band1; ++y) {
        float* row = &cov[(size_t)y * W];
        for (int s = 0; s < sub; ++s) {
            const double sy = y + (s + 0.5) / sub;
            xs.clear();
            for (size_t i = 0; i < poly.size(); ++i) {
                const auto& a = poly[i];
                const auto& b = poly[(i + 1) % poly.size()];
                if ((a.second <= sy) == (b.second <= sy)) continue;
                const double x = a.first + (sy - a.second) / (b.second - a.second) * (b.first - a.first);
                xs.push_back({x, b.second > a.second ? 1 : -1});
            }
            if (xs.size() < 2) continue;
            std::sort(xs.begin(), xs.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                wind += xs[i].second;
                if (wind == 0) continue;
                double xa = std::max(0.0, std::min((double)W, xs[i].first)), xb = std::max(0.0, std::min((double)W, xs[i + 1].first));
                if (!aa) { xa = std::floor(xa + 0.5); xb = std::floor(xb + 0.5); }
                if (xb <= xa) continue;
                const float wgt = 1.0f / sub;
                const int ia = (int)std::floor(xa), ib = (int)std::floor(xb);
                if (ia == ib) { if (ia < W) row[ia] += (float)(xb - xa) * wgt; continue; }
                row[ia] += (float)(ia + 1 - xa) * wgt;
                for (int k = ia + 1; k < ib && k < W; ++k) row[k] += wgt;
                if (ib < W) row[ib] += (float)(xb - ib) * wgt;
            }
        }
        for (int x = 0; x < W; ++x) row[x] = std::min(1.0f, row[x]);
    }
    });
}

// Three box blurs across and down (near enough a Gaussian), 'r' pixels each, the edges of the frame repeated.
static void blurCoverage(std::vector<float>& c, int W, int H, int r)
{
    if (r < 1 || W < 1 || H < 1) return;
    for (int pass = 0; pass < 3; ++pass) {
        for (int dir = 0; dir < 2; ++dir) {
            const int n = dir == 0 ? W : H, lines = dir == 0 ? H : W;
            const size_t step = dir == 0 ? 1 : (size_t)W;
            parallelRows(lines, (size_t)n * 6, [&](int l0, int l1) {
            std::vector<float> line((size_t)n);
            for (int l = l0; l < l1; ++l) {
                float* base = dir == 0 ? &c[(size_t)l * W] : &c[(size_t)l];
                for (int i = 0; i < n; ++i) line[(size_t)i] = base[(size_t)i * step];
                double sum = 0;
                for (int k = -r; k <= r; ++k) sum += line[(size_t)std::min(n - 1, std::max(0, k))];
                const double inv = 1.0 / (2 * r + 1);
                for (int i = 0; i < n; ++i) {
                    base[(size_t)i * step] = (float)(sum * inv);
                    sum += line[(size_t)std::min(n - 1, i + r + 1)] - line[(size_t)std::max(0, i - r)];
                }
            }
            });
        }
    }
}

// The whole mask at a moment: each enabled closed path filled, feathered, and put together in order.
static std::vector<float> maskCoverage(const std::vector<MaskPath>& paths, const Affine& toFrame, int W, int H)
{
    std::vector<float> out;
    bool first = true;
    std::vector<float> c;
    for (const MaskPath& path : paths) {
        if (!path.enabled || !path.closed || path.points.size() < 3) continue;
        if (first) {
            out.assign((size_t)W * H, path.negative ? 1.f : 0.f);
            first = false;
        }
        fillPolygon(maskOutline(path, toFrame), W, H, path.antiAlias, c);
        const double r = path.feather / 100.0 * 0.25 * std::min(W, H);
        if (path.featherType != 0 && r >= 0.75) {
            blurCoverage(c, W, H, std::max(1, (int)std::lround(r / 3.0)));
            // the blur's edge is at half way: Out keeps all of the inside and fades outside, In fades inside only
            if (path.featherType == 2 || path.featherType == 1) {
                const bool outer = path.featherType == 2;
                parallelRows(H, (size_t)W * 2, [&](int ya, int yb) {
                    for (size_t i = (size_t)ya * W, e = (size_t)yb * W; i < e; ++i) c[i] = outer ? std::min(1.f, 2.f * c[i]) : std::max(0.f, 2.f * c[i] - 1.f);
                });
            }
        }
        const float op = (float)std::min(1.0, std::max(0.0, path.opacity));
        parallelRows(H, (size_t)W * 2, [&](int ya, int yb) {
            const size_t i0 = (size_t)ya * W, i1 = (size_t)yb * W;
            if (path.negative)
                for (size_t i = i0; i < i1; ++i) out[i] *= 1.f - op * c[i];
            else
                for (size_t i = i0; i < i1; ++i) out[i] = std::max(out[i], op * c[i]);
        });
    }
    return out;
}

// A copy of 'image' (frame-sized, premultiplied) with each pixel multiplied by the coverage (top row first).
static ImagePtr multiplyByCoverage(const Image& image, const std::vector<float>& cov)
{
    if (cov.size() != (size_t)image.w * image.h) return nullptr;
    ImagePtr out = image.clone();
    if (!out) return nullptr;
    parallelRows(out->h, (size_t)out->w * 4, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        const float* cr = &cov[(size_t)(out->h - 1 - y) * out->w];
        if (out->depth == PixelDepth::Byte) {
            uint8_t* d = out->row(y);
            for (int x = 0; x < out->w; ++x, d += 4) {
                const float f = cr[x];
                if (f >= 1.f) continue;
                for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(d[k] * f + 0.5f);
            }
        } else {
            float* d = (float*)out->row(y);
            for (int x = 0; x < out->w; ++x, d += 4) {
                const float f = cr[x];
                if (f >= 1.f) continue;
                for (int k = 0; k < 4; ++k) d[k] *= f;
            }
        }
    }
    });
    return out;
}

// 'a' where the coverage is 0, 'b' where it is 1, in between mixed (both frame-sized, premultiplied).
static ImagePtr mixByCoverage(const Image& a, const Image& b, const std::vector<float>& cov)
{
    if (cov.size() != (size_t)a.w * a.h || a.w != b.w || a.h != b.h || a.depth != b.depth) return nullptr;
    ImagePtr out = b.clone();
    if (!out) return nullptr;
    parallelRows(out->h, (size_t)out->w * 4, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        const float* cr = &cov[(size_t)(out->h - 1 - y) * out->w];
        if (out->depth == PixelDepth::Byte) {
            const uint8_t* s = a.row(y);
            uint8_t* d = out->row(y);
            for (int x = 0; x < out->w; ++x, d += 4, s += 4) {
                const float f = cr[x];
                if (f >= 1.f) continue;
                for (int k = 0; k < 4; ++k) d[k] = (uint8_t)(s[k] + (d[k] - s[k]) * f + 0.5f);
            }
        } else {
            const float* s = (const float*)a.row(y);
            float* d = (float*)out->row(y);
            for (int x = 0; x < out->w; ++x, d += 4, s += 4) {
                const float f = cr[x];
                if (f >= 1.f) continue;
                for (int k = 0; k < 4; ++k) d[k] = s[k] + (d[k] - s[k]) * f;
            }
        }
    }
    });
    return out;
}

// Track Motion's frame pixel -> the frame's square (-0.5..0.5), as Pass::trackMotion draws it.
static Affine motionToLocal(const MotionKey& k, int W, int H, double par)
{
    const double Wd = W * par;
    const double cx = k.x * Wd, cy = k.y * H;
    const double rw = std::max(1e-9, std::fabs(k.w * Wd)) * (k.w < 0 ? -1 : 1), rh = std::max(1e-9, std::fabs(k.h * H)) * (k.h < 0 ? -1 : 1);
    const double c = std::cos(k.rotation * kDegrees), s = std::sin(k.rotation * kDegrees);
    Affine toLocal;
    toLocal.m[0] = c * par / rw;  toLocal.m[1] = s / rw;  toLocal.m[2] = (-c * cx - s * cy) / rw;
    toLocal.m[3] = -s * par / rh; toLocal.m[4] = c / rh;  toLocal.m[5] = (s * cx - c * cy) / rh;
    return toLocal;
}

// A W x H layer moved by motion keys at time t: layer pixel -> frame pixel (both from the top left).
static Affine motionPlacement(const std::vector<MotionKey>& keys, double t, int W, int H, double par)
{
    Affine id;
    if (keys.empty()) return id;
    const MotionKey k = motionAt(keys, t);
    // layer pixel -> the square ((x - W/2) / W, (y - H/2) / H) -> frame: the inverse of toLocal
    Affine toSquare;
    toSquare.m[0] = 1.0 / W; toSquare.m[2] = -0.5;
    toSquare.m[4] = 1.0 / H; toSquare.m[5] = -0.5;
    return inverseOf(motionToLocal(k, W, H, par)).after(toSquare);
}

bool effectPlacement(const Project& p, uint64_t slotId, double t, double out[6])
{
    Affine m;                                   // effect pixel -> frame pixel, both from the top left
    for (int k = 0; k < 6; ++k) out[k] = m.m[k];
    const int W = p.settings.width, H = p.settings.height;
    const double par = p.settings.pixelAspect > 0.01 && p.settings.pixelAspect < 100 ? p.settings.pixelAspect : 1.0;
    if (W <= 0 || H <= 0) return false;
    for (const FxSlot& s : p.videoOut)
        if (s.id == slotId) return true;
    // the video tracks, top first, with each one's place in them
    std::vector<const Track*> vt;
    for (const Track& tr : p.tracks)
        if (tr.type == TrackType::Video) vt.push_back(&tr);
    for (size_t i = 0; i < vt.size(); ++i) {
        const Track& tr = *vt[i];
        bool found = false;
        for (const FxSlot& s : tr.fx)
            if (s.id == slotId) found = true;
        for (const Event& e : tr.events) {
            for (const FxSlot& s : e.fx) {
                if (s.id != slotId) continue;
                found = true;
                if (!s.beforePanCrop) continue;
                // a pre-pan/crop effect: its frame is the media at its own size; Pan/Crop shows a box of that
                const MediaItem* mi = p.findMedia(e.mediaId);
                double mw = 0, mh = 0;
                if (mi) mi->shownSize(mw, mh);
                if (!mi || mi->generated || !(mw > 0 && mh > 0)) { mw = W; mh = H; }
                const FitRect r = fittedMediaRect(mw, mh, W, H);
                // effect pixel -> media fraction. The effect's canonical coordinates stay the project's (the overlay and its X / Y
                // settings use them); on the media's own frame they are scaled by media size / project size, so a fraction of the
                // project's frame is the same fraction of the media.
                Affine toFrac;
                toFrac.m[0] = 1.0 / W;
                toFrac.m[4] = 1.0 / H;
                if (e.panCrop.empty() && e.keepAspect && std::fabs(par - 1.0) < 1e-9) {
                    // no Pan/Crop: the media is fitted into the frame
                    Affine fit;
                    fit.m[0] = r.w; fit.m[2] = r.x;
                    fit.m[4] = r.h; fit.m[5] = r.y;
                    m = fit.after(toFrac);
                    continue;
                }
                // the box's square -> media fraction (as Pass::panCrop's toSource, divided by the frame's size)
                const BoxPlacement bp = placeBoxOf(e, t, mw, mh, W, H, par);
                const PanCropKey& b = bp.box;
                const double c = std::cos(b.rotation * kDegrees), sn = std::sin(b.rotation * kDegrees);
                Affine fromSquare;
                fromSquare.m[0] = c * b.w; fromSquare.m[1] = -sn * b.h * mh / mw; fromSquare.m[2] = b.cx;
                fromSquare.m[3] = sn * b.w * mw / mh; fromSquare.m[4] = c * b.h; fromSquare.m[5] = b.cy;
                // the square -> frame pixel (the inverse of panCrop's toLocal)
                Affine toFrame;
                toFrame.m[0] = bp.dw; toFrame.m[2] = 0.5 * W;
                toFrame.m[4] = bp.dh; toFrame.m[5] = 0.5 * H;
                m = toFrame.after(inverseOf(fromSquare)).after(toFrac);
            }
        }
        if (!found) continue;
        if (tr.adjustment) return true;
        m = motionPlacement(tr.motion, t, W, H, par).after(m);
        // the compositing parents above it, nearest first, move it too
        int level = std::max(0, tr.compositeLevel);
        for (size_t j = i; j-- > 0 && level > 0;) {
            const int l = std::max(0, vt[j]->compositeLevel);
            if (l < level) {
                m = motionPlacement(vt[j]->parentMotion, t, W, H, par).after(m);
                level = l;
            }
        }
        // in the convention asked for: from the bottom left (y' = H - y on both sides)
        Affine flip;
        flip.m[4] = -1;
        flip.m[5] = H;
        m = flip.after(m).after(flip);
        for (int k = 0; k < 6; ++k) out[k] = m.m[k];
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------- the preview's smaller frames
// The size a frame is made at (FrameOptions::scale): the project's, or that fraction of it (at least 1 x 1).
static void frameSizeFor(const Project& p, const FrameOptions& o, int& w, int& h)
{
    w = p.settings.width;
    h = p.settings.height;
    if (o.scale > 0.0 && o.scale < 0.999) {
        w = std::max(1, (int)std::lround(w * o.scale));
        h = std::max(1, (int)std::lround(h * o.scale));
    }
}

// A frame stretched to exactly w x h (its own shape not kept: for the few pixels' difference between a smaller
// preview frame and the project's).
static ImagePtr stretchImage(const Image& src, int w, int h)
{
    if (src.w == w && src.h == h) return src.clone();
    ImagePtr out = Image::make(w, h, src.depth, false);
    if (!out || src.w < 1 || src.h < 1) return out;
    const bool fl = src.depth != PixelDepth::Byte;
    const double sx = (double)src.w / w, sy = (double)src.h / h;
    // per output column: the source columns it covers and their weights (the same for every row), all in one array
    struct Taps { std::vector<int> x0, n, at; std::vector<float> wt; };
    auto taps = [](int n, int srcN, double s) {
        Taps t;
        t.x0.resize((size_t)n);
        t.n.resize((size_t)n);
        t.at.resize((size_t)n);
        std::vector<float> cur;
        for (int i = 0; i < n; ++i) {
            int x0, x1;
            cur.clear();
            if (s > 1.0) {                                          // shrinking: the box the pixel covers
                const double a = i * s, b = (i + 1) * s;
                x0 = std::max(0, (int)std::floor(a));
                x1 = std::min(srcN - 1, (int)std::ceil(b) - 1);
                for (int x = x0; x <= x1; ++x) cur.push_back((float)(std::min(b, x + 1.0) - std::max(a, (double)x)));
            } else {                                                // enlarging: the two nearest
                const double c = (i + 0.5) * s - 0.5;
                const int xf = (int)std::floor(c);
                const float f = (float)(c - xf);
                x0 = std::max(0, std::min(srcN - 1, xf));
                x1 = std::max(0, std::min(srcN - 1, xf + 1));
                if (x1 == x0) cur = {1.f};
                else cur = {1.f - f, f};
            }
            float sum = 0;
            for (float v : cur) sum += v;
            for (float& v : cur) v = sum > 0 ? v / sum : 0.f;
            if (cur.empty()) { x1 = x0; cur = {1.f}; }
            t.x0[(size_t)i] = x0;
            t.n[(size_t)i] = (int)cur.size();
            t.at[(size_t)i] = (int)t.wt.size();
            t.wt.insert(t.wt.end(), cur.begin(), cur.end());
        }
        return t;
    };
    const Taps tx = taps(w, src.w, sx), ty = taps(h, src.h, sy);
    parallelRows(h, (size_t)(src.w + w) * 4 * 4, [&](int ya, int yb) {
    std::vector<float> line((size_t)src.w * 4);
    for (int y = ya; y < yb; ++y) {
        const int vx0 = ty.x0[(size_t)y], vn = ty.n[(size_t)y];
        const float* vw = &ty.wt[(size_t)ty.at[(size_t)y]];
        const int nv = src.w * 4;
        // the rows it covers, each times its weight, added up in order
#ifdef RENO_PIXROWS_SSE2
        {
            const __m128i zero = _mm_setzero_si128();
            for (int k = 0; k < vn; ++k) {
                const __m128 wg = _mm_set1_ps(vw[k]);
                float* ln = line.data();
                if (fl) {
                    const float* sr = (const float*)src.row(vx0 + k);
                    for (int i = 0; i < nv; i += 4) {
                        const __m128 m = _mm_mul_ps(_mm_loadu_ps(sr + i), wg);
                        _mm_storeu_ps(ln + i, k ? _mm_add_ps(_mm_loadu_ps(ln + i), m) : _mm_add_ps(_mm_setzero_ps(), m));
                    }
                } else {
                    const uint8_t* sr = src.row(vx0 + k);
                    int i = 0;
                    for (; i + 16 <= nv; i += 16) {
                        const __m128i v = _mm_loadu_si128((const __m128i*)(sr + i));
                        const __m128i lo = _mm_unpacklo_epi8(v, zero), hi = _mm_unpackhi_epi8(v, zero);
                        const __m128 f0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(lo, zero)), f1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(lo, zero));
                        const __m128 f2 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(hi, zero)), f3 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(hi, zero));
                        if (k) {
                            _mm_storeu_ps(ln + i, _mm_add_ps(_mm_loadu_ps(ln + i), _mm_mul_ps(f0, wg)));
                            _mm_storeu_ps(ln + i + 4, _mm_add_ps(_mm_loadu_ps(ln + i + 4), _mm_mul_ps(f1, wg)));
                            _mm_storeu_ps(ln + i + 8, _mm_add_ps(_mm_loadu_ps(ln + i + 8), _mm_mul_ps(f2, wg)));
                            _mm_storeu_ps(ln + i + 12, _mm_add_ps(_mm_loadu_ps(ln + i + 12), _mm_mul_ps(f3, wg)));
                        } else {
                            _mm_storeu_ps(ln + i, _mm_mul_ps(f0, wg));
                            _mm_storeu_ps(ln + i + 4, _mm_mul_ps(f1, wg));
                            _mm_storeu_ps(ln + i + 8, _mm_mul_ps(f2, wg));
                            _mm_storeu_ps(ln + i + 12, _mm_mul_ps(f3, wg));
                        }
                    }
                    for (; i < nv; ++i) ln[i] = k ? ln[i] + sr[i] * vw[k] : sr[i] * vw[k];
                }
            }
        }
#else
        std::fill(line.begin(), line.end(), 0.f);
        for (int k = 0; k < vn; ++k) {
            const float wgt = vw[k];
            if (fl) {
                const float* sr = (const float*)src.row(vx0 + k);
                for (int i = 0; i < nv; ++i) line[(size_t)i] += sr[i] * wgt;
            } else {
                const uint8_t* sr = src.row(vx0 + k);
                for (int i = 0; i < nv; ++i) line[(size_t)i] += sr[i] * wgt;
            }
        }
#endif
        uint8_t* drow = out->row(y);
        for (int x = 0; x < w; ++x) {
            const int c0 = tx.x0[(size_t)x], cn = tx.n[(size_t)x];
            const float* cw = &tx.wt[(size_t)tx.at[(size_t)x]];
#ifdef RENO_PIXROWS_SSE2
            __m128 a = _mm_setzero_ps();
            for (int k = 0; k < cn; ++k) a = _mm_add_ps(a, _mm_mul_ps(_mm_loadu_ps(&line[(size_t)(c0 + k) * 4]), _mm_set1_ps(cw[k])));
            if (fl) {
                _mm_storeu_ps((float*)drow + (size_t)x * 4, a);
            } else {
                a = _mm_min_ps(_mm_max_ps(_mm_add_ps(a, _mm_set1_ps(0.5f)), _mm_setzero_ps()), _mm_set1_ps(255.f));
                const __m128i i32 = _mm_cvttps_epi32(a);
                const __m128i b8 = _mm_packus_epi16(_mm_packs_epi32(i32, i32), _mm_setzero_si128());
                const uint32_t word = (uint32_t)_mm_cvtsi128_si32(b8);
                const uint32_t o = (uint32_t)_mm_cvtsi128_si32(_mm_min_epu8(b8, _mm_set1_epi8((char)(word >> 24))));
                std::memcpy(drow + (size_t)x * 4, &o, 3);
                drow[(size_t)x * 4 + 3] = (uint8_t)(word >> 24);
            }
#else
            float acc[4] = {0, 0, 0, 0};
            for (int k = 0; k < cn; ++k)
                for (int j = 0; j < 4; ++j) acc[j] += line[(size_t)(c0 + k) * 4 + j] * cw[k];
            if (fl) {
                float* d = (float*)drow + (size_t)x * 4;
                for (int j = 0; j < 4; ++j) d[j] = acc[j];
            } else {
                uint8_t* d = drow + (size_t)x * 4;
                for (int j = 0; j < 4; ++j) d[j] = (uint8_t)std::min(255.f, std::max(0.f, acc[j] + 0.5f));
                for (int j = 0; j < 3; ++j) if (d[j] > d[3]) d[j] = d[3];
            }
#endif
        }
    }
    });
    return out;
}

// ====================================================================== the pass
struct VideoEngine::Pass {
    VideoEngine& eng;
    const Project& p;
    FrameOptions opt;
    int W, H;                               // the size frames are made at
    int PW, PH;                             // the project's frame size
    double fps;
    double par;                             // the frame's pixel aspect
    Quality quality;
    PixelDepth pd;                          // what frames are worked in
    int deinterlace;                        // what to do with interlaced media
    typedef std::tuple<int, uint64_t, int, int64_t> MemoKey;
    std::map<std::pair<const Track*, int64_t>, ImagePtr> layers;     // track layers made ahead
    std::map<MemoKey, ImagePtr> memo;
    std::mutex memoMutex;
    bool memoGet(const MemoKey& key, ImagePtr& out)
    {
        std::lock_guard<std::mutex> lk(memoMutex);
        auto it = memo.find(key);
        if (it == memo.end()) return false;
        out = it->second;
        return true;
    }
    void memoPut(const MemoKey& key, const ImagePtr& v)
    {
        std::lock_guard<std::mutex> lk(memoMutex);
        memo[key] = v;
    }
    double timeline = -1;                   // the frame's time on the timeline

    Pass(VideoEngine& e, const Project& pr, const FrameOptions& o)
        : eng(e), p(pr), opt(o), W(0), H(0), PW(pr.settings.width), PH(pr.settings.height), fps(pr.settings.fps()),
          par(pr.settings.pixelAspect > 0.01 && pr.settings.pixelAspect < 100 ? pr.settings.pixelAspect : 1.0),
          quality(o.draft ? kDraft : qualityOf(pr.settings.renderQuality)),
          pd(floatProject(pr.settings) ? PixelDepth::Float : PixelDepth::Byte),
          deinterlace(pr.settings.fieldOrder != "upper" && pr.settings.fieldOrder != "lower"
                          ? (pr.settings.deinterlace == "blend" ? 1 : (pr.settings.deinterlace == "interpolate" || pr.settings.deinterlace == "smart" ? 2 : 0)) : 0)
    {
        frameSizeFor(pr, o, W, H);
    }

    // A smaller preview (W x H less than the project's): frames for and from OpenFX effects, which always work at
    // the project's size, are stretched to it and back.
    bool scaled() const { return W != PW || H != PH; }
    ImagePtr toProjectSize(const ImagePtr& img) const { return (img && scaled() && (img->w != PW || img->h != PH)) ? stretchImage(*img, PW, PH) : img; }
    ImagePtr toFrameSize(const ImagePtr& img) const { return (img && scaled() && (img->w != W || img->h != H)) ? stretchImage(*img, W, H) : img; }

    static int64_t tick(double t) { return (int64_t)std::llround(t * 1.0e6); }

    ImagePtr blank() { return Image::make(W, H, pd, true); }
    // A frame in the depth the pass works in (8-bit media in a 32-bit project is turned into float once, here).
    ImagePtr inDepth(const ImagePtr& img) const { return (img && img->depth != pd) ? img->converted(pd) : img; }
    // A media frame brought into the linear light a 32-bit floating point (full range) project composites in, by its
    // Color space (colormgmt.h).
    ImagePtr intoLight(const MediaItem* m, const ImagePtr& img) const
    {
        if (!img || !linearLightProject(p.settings)) return img;
        return toSceneLinear(img, m && !m->generated ? m->colorSpace : std::string());
    }

    // 'inst': the effect about to be rendered.
    ofx::RenderOptions fxOptions(const ofx::Instance* inst = nullptr) const
    {
        ofx::RenderOptions ro;
        if (inst && scaled() && atPreviewSize(inst)) {
            ro.width = W;
            ro.height = H;
            ro.previewSizeForAll = !inst->scalable();
        }
        ro.interactive = opt.interactive;
        ro.draft = opt.draft;
        ro.depth = pd;
        if (timeline >= 0) ro.timelineTime = timeline * fps;
        ro.abort = opt.cancel;
        return ro;
    }
    // Whether effect 'inst' works at the frame's size when that is smaller than the project's.
    bool atPreviewSize(const ofx::Instance* inst) const
    {
        if (!inst) return false;
        if (inst->scalable()) return true;
        return opt.interactive && ofx::Host::get().config.previewSizeForAll && W <= PW && H <= PH;
    }

    // The frame is no longer wanted (FrameOptions::cancel): effects not begun yet are left out.
    bool cancelled() const { return opt.cancel && opt.cancel->load(); }

    // One effect: 'frame' is the time the effect is told, source(frame) gives its input at any time.
    ImagePtr runFx(const FxSlot& slot, double frame, const std::function<ImagePtr(double)>& source)
    {
        if (isBuiltinVideo(slot)) {
            // one of Reno's own effects (the Color Grading panel's): worked on a copy of its input
            ImagePtr in = source(frame);
            if (!in || slot.bypass || opt.skipEffects) return in;
            ImagePtr out = inDepth(in)->clone();
            if (out) applyBuiltin(slot, *out);
            return out;
        }
        ofx::Instance* inst = (slot.bypass || opt.skipEffects) ? nullptr : eng.fx_.video(slot.id);
        if (!inst || cancelled()) return source(frame);
        const bool atFrameSize = atPreviewSize(inst);
        auto src = [&](double f) -> ImagePtr {
            ImagePtr img = source(f);
            return atFrameSize ? toFrameSize(img ? img : blank()) : toProjectSize(img ? img : blank());
        };
        // The input at this time is made before the effect is locked for its render.
        source(frame);
        // One use of an instance at a time.
        ofx::RenderGate::Hold own(eng.fx_.renderLock(inst));
        if (!own.ok()) return source(frame);
        const std::vector<std::string> inputs = inst->inputClipNames();
        std::string main = inputs.empty() ? std::string() : inputs[0];
        for (const auto& n : inputs) if (n == kOfxImageEffectSimpleSourceClipName) main = n;
        if (!main.empty()) inst->setClipSource(main, src);
        std::string err;
        ImagePtr out = inst->render(frame, fxOptions(inst), &err);
        if (!main.empty()) inst->setClipSource(main, [](double) { return ImagePtr(); });
        if (!out) {
            if (!cancelled()) eng.fx_.setError(slot.id, err);
            return source(frame);
        }
        if (!eng.fx_.error(slot.id).empty()) eng.fx_.setError(slot.id, std::string());     // it has recovered
        return inDepth(toFrameSize(out));
    }

    // One effect on a frame of its own size (a pre-pan/crop effect's: the media as it is). The effect is told the project is
    // that size, at render scale 1, as for a smaller preview; its input comes in that size too.
    ImagePtr runFxOwnSize(const FxSlot& slot, double frame, const std::function<ImagePtr(double)>& source)
    {
        if (isBuiltinVideo(slot)) return runFx(slot, frame, source);
        ofx::Instance* inst = (slot.bypass || opt.skipEffects) ? nullptr : eng.fx_.video(slot.id);
        if (!inst || cancelled()) return source(frame);
        ImagePtr probe = source(frame);
        if (!probe) return probe;
        const int w = probe->w, h = probe->h;
        auto src = [&](double f) -> ImagePtr {
            ImagePtr img = source(f);
            if (!img) {
                img = Image::make(w, h, pd, true);
                return img;
            }
            return (img->w == w && img->h == h) ? img : stretchImage(*img, w, h);
        };
        ofx::RenderGate::Hold own(eng.fx_.renderLock(inst));
        if (!own.ok()) return probe;
        const std::vector<std::string> inputs = inst->inputClipNames();
        std::string main = inputs.empty() ? std::string() : inputs[0];
        for (const auto& n : inputs) if (n == kOfxImageEffectSimpleSourceClipName) main = n;
        if (!main.empty()) inst->setClipSource(main, src);
        ofx::RenderOptions ro = fxOptions(inst);
        ro.width = w;
        ro.height = h;
        ro.mediaSize = true;
        ro.previewSizeForAll = false;
        std::string err;
        ImagePtr out = inst->render(frame, ro, &err);
        if (!main.empty()) inst->setClipSource(main, [](double) { return ImagePtr(); });
        if (!out) {
            if (!cancelled()) eng.fx_.setError(slot.id, err);
            return probe;
        }
        if (!eng.fx_.error(slot.id).empty()) eng.fx_.setError(slot.id, std::string());
        return inDepth(out);
    }

    // VEGAS's undersample rate: an event with a rate below 1 only gets a new frame every 1 / (rate x fps)
    // seconds (counted from its start); in between it holds the last one.
    double undersampled(const Event& e, double t) const
    {
        if (!(e.undersample > 0.01 && e.undersample < 0.999)) return t;
        const double step = 1.0 / (fps * e.undersample);
        const double u = t - e.start;
        return e.start + std::floor(u / step + 1e-6) * step;
    }

    // The time in the media for the frame of event e showing at timeline time t.
    double mediaTimeFor(const Event& e, const MediaItem* m, double t) const
    {
        const double frame = (e.undersample > 0.01 && e.undersample < 0.999) ? 1.0 / (fps * e.undersample) : 1.0 / fps;
        return eventMediaTime(e, m, e.reverse ? t + frame : t);
    }

    // Frame resampling (the event's Edit > Switches setting, else Project Properties' Resample mode): blend the two
    // media frames around the time.
    bool resampleOn(const Event& e, const MediaItem& m) const
    {
        if (m.generated || m.info.isStill || !(m.info.fps > 0.5) || !(fps > 0)) return false;
        const std::string fxMode = eventFxResample(e);
        const std::string& mode = !fxMode.empty() ? fxMode : (e.resample == "force" || e.resample == "off" || e.resample == "optical") ? e.resample : p.settings.resample;
        if (mode == "off") return false;
        if (mode == "force" || mode == "optical") return true;
        if (const Envelope* v = findEnvelope(e.envelopes, "velocity"))
            if (!v->points.empty()) return true;
        const double r = m.info.fps * std::fabs(e.rate) / fps;      // media frames per project frame
        const double n = std::round(r);
        return !(n >= 1.0 && std::fabs(r - n) < 1e-3);
    }

    // The event's media frame for media time mt (w x h at most), resampled as above.
    ImagePtr eventFrame(const Event& e, const MediaItem& m, double mt, int w, int h)
    {
        ImagePtr a = eng.pool_.videoFrame(e.id, m, mt, w, h, deinterlace, pd);
        if (a && !m.redEye.empty()) a = applyRedEye(a, m.redEye);
        if (!a || !resampleOn(e, m)) return a;
        const double mf = m.info.fps;
        const double f = mt * mf;
        const double i = std::floor(f + 1e-6);
        const double frac = f - i;
        if (!(frac > 0.01 && frac < 0.99)) {                        // (on a frame: nothing to blend; a hair before the next one it is that one:
            if (frac >= 0.99 && !(m.info.duration > 0 && (i + 1.5) / mf >= m.info.duration)) {          // VEGAS shows the next frame at 0.997)
                ImagePtr nx = eng.pool_.videoFrame(e.id, m, (i + 1.5) / mf, w, h, deinterlace, pd);
                if (nx && !m.redEye.empty()) nx = applyRedEye(nx, m.redEye);
                if (nx) return nx;
            }
            return a;
        }
        const double next = (i + 1.5) / mf;
        if (m.info.duration > 0 && next >= m.info.duration) return a;
        ImagePtr b = eng.pool_.videoFrame(e.id, m, next, w, h, deinterlace, pd);
        if (b && !m.redEye.empty()) b = applyRedEye(b, m.redEye);
        if (!b || b->w != a->w || b->h != a->h || b->depth != a->depth) return a;
        const std::string fxMode = eventFxResample(e);
        const std::string& mode = !fxMode.empty() ? fxMode : (e.resample == "force" || e.resample == "off" || e.resample == "optical") ? e.resample : p.settings.resample;
        const bool remember = opt.interactive && composeMemoOn() && m.redEye.empty();
        const double key[4] = {3, mode == "optical" ? 1.0 : 0.0, (double)(float)frac, (double)(int)a->depth};
        if (remember)
            if (ImagePtr hit = warpMemo().find(a, b, key, 4)) { gMemoHits.fetch_add(1, std::memory_order_relaxed); return hit; }
        ImagePtr out = Image::make(a->w, a->h, a->depth, false);
        if (!out) return a;
        if (mode == "optical") interpolateOpticalFlow(*out, *a, *b, (float)frac);       // Optical Flow resampling
        else blendImages(*out, *a, *b, (float)frac);
        if (remember) warpMemo().put(a, b, key, 4, out);
        return out;
    }

    // ---- pre-pan/crop event effects (VEGAS's "Pre" effects, FxSlot::beforePanCrop): they work on the event's media
    // as it is - fitted into the frame, keeping its shape, before Event Pan/Crop - and Pan/Crop is applied to what
    // they make.
    std::vector<size_t> preEffects(const Event& e) const
    {
        std::vector<size_t> out;
        if (opt.skipEffects) return out;
        for (size_t k = 0; k < e.fx.size(); ++k)
            if (e.fx[k].beforePanCrop && isVideoFxSlot(e.fx[k])) out.push_back(k);
        return out;
    }

    // The event's media as it is, at its own size (a generator's at the frame's), before any effect: what pre-pan/crop
    // effects work on, as in VEGAS. A smaller frame (preview) takes the same share of it.
    ImagePtr ownMedia(const Event& e, double t)
    {
        const auto key = std::make_tuple(9, e.id, 0, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        ImagePtr out;
        const MediaItem* m = p.findMedia(e.mediaId);
        if (m) {
            const double mt = mediaTimeFor(e, m, t);
            if (m->generated) {
                if (ofx::Instance* gen = eng.fx_.video(m->generator.id)) {
                    std::string err;
                    out = toFrameSize(gen->render(frameNumber(mt, fps), fxOptions(gen), &err));
                    if (!out && !cancelled()) eng.fx_.setError(m->generator.id, err);
                }
            } else if (m->info.hasVideo && !m->offline) {
                double mw = 0, mh = 0;
                m->shownSize(mw, mh);
                int ow = W, oh = H;
                if (mw > 0 && mh > 0) {
                    const double k = (PW > 0 && W < PW) ? (double)W / PW : 1.0;
                    ow = std::max(1, (int)std::lround(mw * k));
                    oh = std::max(1, (int)std::lround(mh * k));
                    const int most = 8192;
                    if (ow > most || oh > most) {
                        const double f = std::min((double)most / ow, (double)most / oh);
                        ow = std::max(1, (int)(ow * f));
                        oh = std::max(1, (int)(oh * f));
                    }
                }
                out = eventFrame(e, *m, mt, ow, oh);
            }
        }
        out = inDepth(out);
        memoPut(key, out);
        return out;
    }

    // The media at its own size with the first 'k' of the event's pre-pan/crop effects applied.
    ImagePtr preStage(const Event& e, const std::vector<size_t>& pre, size_t k, double t)
    {
        if (k == 0) return ownMedia(e, t);
        const auto key = std::make_tuple(8, e.id, (int)k, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = e.fx[pre[k - 1]];
        ImagePtr out = runFxOwnSize(slot, frameNumber(t - e.start, fps), [&](double f) { return preStage(e, pre, k - 1, e.start + f / fps); });
        memoPut(key, out);
        return out;
    }

    // The event's media at timeline time t, frame-sized, panned/cropped.
    ImagePtr mediaFrame(const Event& e, double t)
    {
        const auto key = std::make_tuple(0, e.id, 0, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        ImagePtr out;
        const MediaItem* m = p.findMedia(e.mediaId);
        const std::vector<size_t> pre = m ? preEffects(e) : std::vector<size_t>();
        if (m && !pre.empty()) {
            ImagePtr img = preStage(e, pre, pre.size(), t);
            const bool plain = e.panCrop.empty() && e.keepAspect && std::fabs(par - 1.0) < 1e-9;
            if (img && plain) out = (img->w == W && img->h == H) ? img : fitImage(*img, W, H);
            else if (img) {
                // Pan/Crop of what the effects made (the media's own shape; a generator's is the frame's)
                double mw = 0, mh = 0;
                m->shownSize(mw, mh);
                if (m->generated || !(mw > 0 && mh > 0)) { mw = W; mh = H; }
                out = panCrop(e, t, *img, mw, mh);
            }
            if (out && e.reduceFlicker) out = reduceFlicker(*out);
            out = intoLight(m, inDepth(out));
            out = masked(e, m, t, out);
            memoPut(key, out);
            return out;
        }
        if (m) {
            const double mt = mediaTimeFor(e, m, t);
            const bool plain = e.panCrop.empty() && e.keepAspect && std::fabs(par - 1.0) < 1e-9;
            if (m->generated) {
                ofx::Instance* gen = eng.fx_.video(m->generator.id);
                if (gen) {
                    std::string err;
                    ImagePtr f = toFrameSize(gen->render(frameNumber(mt, fps), fxOptions(gen), &err));
                    if (!f) { if (!cancelled()) eng.fx_.setError(m->generator.id, err); }
                    else out = plain ? f : panCrop(e, t, *f, W, H);
                }
            } else if (m->info.hasVideo && !m->offline) {
                if (plain) {
                    ImagePtr f = eventFrame(e, *m, mt, W, H);
                    if (f) out = (f->w == W && f->h == H) ? f : fitImage(*f, W, H);
                } else {
                    out = panCropMedia(e, *m, mt, t);
                }
            }
        }
        if (out && e.reduceFlicker) out = reduceFlicker(*out);
        out = intoLight(m, inDepth(out));
        out = masked(e, m, t, out);
        memoPut(key, out);
        return out;
    }

    // Event Pan/Crop's mask: the event's frame cut to it (unless the mask is for its effects only: eventStage).
    ImagePtr masked(const Event& e, const MediaItem* m, double t, const ImagePtr& image)
    {
        if (!image || e.maskApplyToFx || !maskActive(e) || image->w != W || image->h != H) return image;
        const std::vector<float> cov = maskCoverage(maskAt(e.mask, t - e.start), maskToFrame(e, m, t, W, H, par), W, H);
        if (cov.empty()) return image;
        ImagePtr out = multiplyByCoverage(*image, cov);
        return out ? out : image;
    }

    // Event Pan/Crop: the box of the media frame at this moment, turned, made to fill the frame (keeping the
    // box's shape, with transparent bars, when the event keeps its aspect).
    BoxPlacement placeBox(const Event& e, double t, double mediaW, double mediaH) const { return placeBoxOf(e, t, mediaW, mediaH, W, H, par); }

    ImagePtr panCropMedia(const Event& e, const MediaItem& m, double mt, double t)
    {
        double mw = 0, mh = 0;
        m.shownSize(mw, mh);
        // Ask for the media big enough that the box keeps its detail when zoomed in (no bigger than the media
        // really is, and within reason), and no bigger than needed when it is zoomed out.
        int rw = W, rh = H;
        if (mw > 0 && mh > 0) {
            const BoxPlacement bp = placeBox(e, t, mw, mh);
            const double bw = std::max(1e-6, std::fabs(bp.box.w)), bh = std::max(1e-6, std::fabs(bp.box.h));
            double scale = std::max(bp.dw / (bw * mw), bp.dh / (bh * mh));      // frame pixels per media pixel
            if (quality <= kPreview) scale = std::min(scale, std::min((double)W / mw, (double)H / mh) * (quality == kDraft ? 1.0 : 2.0));
            scale = std::min(1.0, scale);
            rw = (int)std::ceil(mw * scale);
            rh = (int)std::ceil(mh * scale);
            const int most = 8192;
            if (rw > most || rh > most) {
                const double f = std::min((double)most / rw, (double)most / rh);
                rw = (int)(rw * f);
                rh = (int)(rh * f);
            }
            rw = std::max(16, rw);
            rh = std::max(16, rh);
        }
        ImagePtr f = eventFrame(e, m, mt, rw, rh);
        if (!f) return nullptr;
        return panCrop(e, t, *f, mw > 0 ? mw : f->w, mh > 0 ? mh : f->h, f);
    }

    // Pan/crop of a frame of the media (any size; mediaW x mediaH is the media's real shape).
    ImagePtr panCrop(const Event& e, double t, const Image& image, double mediaW, double mediaH, const ImagePtr& hold = ImagePtr())
    {
        const BoxPlacement bp = placeBox(e, t, mediaW, mediaH);
        const PanCropKey& b = bp.box;
        const bool remember = hold && hold.get() == &image && opt.interactive && composeMemoOn();
        double key[16] = {1, (double)image.w, (double)image.h, mediaW, mediaH, b.cx, b.cy, b.w, b.h, b.rotation, bp.dw, bp.dh, (double)W, (double)H, (double)quality, (double)(int)image.depth};
        if (remember && std::fabs(b.w) >= 1e-9 && std::fabs(b.h) >= 1e-9)
            if (ImagePtr hit = warpMemo().find(hold, ImagePtr(), key, 16)) { gMemoHits.fetch_add(1, std::memory_order_relaxed); return hit; }
        ImagePtr out = Image::make(W, H, image.depth, false);
        if (!out) return nullptr;
        if (std::fabs(b.w) < 1e-9 || std::fabs(b.h) < 1e-9) {
            out->clear();
            return out;
        }
        // frame pixel -> the box's own square: centred in the frame, bp.dw x bp.dh
        Affine toLocal;
        toLocal.m[0] = 1.0 / bp.dw; toLocal.m[1] = 0; toLocal.m[2] = -0.5 * W / bp.dw;
        toLocal.m[3] = 0; toLocal.m[4] = 1.0 / bp.dh; toLocal.m[5] = -0.5 * H / bp.dh;
        // the box's square -> the frame's pixels: scaled to the box, turned clockwise, moved to its centre.
        const double pw = image.w, ph = image.h;
        const double c = std::cos(b.rotation * kDegrees), s = std::sin(b.rotation * kDegrees);
        const double ax = b.w * mediaW, ay = b.h * mediaH;          // box size in media units
        const double kx = pw / mediaW, ky = ph / mediaH;            // media units -> frame pixels
        Affine toSource;
        toSource.m[0] = c * ax * kx; toSource.m[1] = -s * ay * kx; toSource.m[2] = b.cx * pw;
        toSource.m[3] = s * ax * ky; toSource.m[4] = c * ay * ky;  toSource.m[5] = b.cy * ph;
        // shrinking a lot: shrink the frame first with a proper average, so that it does not sparkle
        ImagePtr small;
        const Image* from = &image;
        const Affine full = toSource.after(toLocal);
        const double shrink = std::sqrt(std::fabs(full.m[0] * full.m[4] - full.m[1] * full.m[3]));
        if (shrink > 1.8 && quality != kDraft) {
            small = fitImage(image, std::max(1, (int)std::ceil(image.w / shrink)), std::max(1, (int)std::ceil(image.h / shrink)));
            if (small) {
                const double f = std::min((double)small->w / image.w, (double)small->h / image.h);
                const double ox = (small->w - image.w * f) / 2, oy = (small->h - image.h * f) / 2;
                for (int k = 0; k < 3; ++k) { toSource.m[k] *= f; toSource.m[3 + k] *= f; }
                toSource.m[2] += ox;
                toSource.m[5] += oy;
                from = small.get();
            }
        }
        warp(*from, *out, toLocal, toSource, true, quality);
        if (remember) warpMemo().put(hold, ImagePtr(), key, 16, out);
        return out;
    }

    // Track Motion (or a compositing parent's Parent Motion): a frame moved, sized and turned within the frame by the
    // keys' place at time t (transparent around it).
    ImagePtr trackMotion(const std::vector<MotionKey>& keys, const Image& image, double t, const ImagePtr& hold = ImagePtr())
    {
        const MotionKey k = motionAt(keys, t);
        const bool remember = hold && hold.get() == &image && opt.interactive && composeMemoOn() && std::fabs(k.w) >= 1e-9 && std::fabs(k.h) >= 1e-9;
        double key[16] = {2, (double)image.w, (double)image.h, k.x, k.y, k.w, k.h, k.rotation, (double)W, (double)H, par, (double)quality, (double)(int)image.depth, 0, 0, 0};
        if (remember)
            if (ImagePtr hit = warpMemo().find(hold, ImagePtr(), key, 13)) { gMemoHits.fetch_add(1, std::memory_order_relaxed); return hit; }
        ImagePtr out = Image::make(W, H, image.depth, false);
        if (!out) return nullptr;
        if (std::fabs(k.w) < 1e-9 || std::fabs(k.h) < 1e-9) {
            out->clear();
            return out;
        }
        // Worked out in "seen" units (frame pixels widened by the pixel aspect) so that turning keeps shapes.
        const double Wd = W * par;
        const double cx = k.x * Wd, cy = k.y * H;
        const double rw = k.w * Wd, rh = k.h * H;
        const double c = std::cos(k.rotation * kDegrees), s = std::sin(k.rotation * kDegrees);
        // frame pixel -> seen (x*par, y) -> about the centre -> turned back -> the frame's square
        Affine toLocal;
        toLocal.m[0] = c * par / rw;  toLocal.m[1] = s / rw;  toLocal.m[2] = (-c * cx - s * cy) / rw;
        toLocal.m[3] = -s * par / rh; toLocal.m[4] = c / rh;  toLocal.m[5] = (s * cx - c * cy) / rh;
        Affine toSource;
        toSource.m[0] = image.w; toSource.m[1] = 0; toSource.m[2] = image.w * 0.5;
        toSource.m[3] = 0; toSource.m[4] = image.h; toSource.m[5] = image.h * 0.5;
        ImagePtr small;
        const Image* from = &image;
        const Affine full = toSource.after(toLocal);
        const double shrink = std::sqrt(std::fabs(full.m[0] * full.m[4] - full.m[1] * full.m[3]));
        if (shrink > 1.8 && quality != kDraft) {
            const int sw = std::max(1, (int)std::ceil(image.w / shrink)), sh = std::max(1, (int)std::ceil(image.h / shrink));
            small = fitImage(image, sw, sh);
            if (small) {
                const double f = std::min((double)small->w / image.w, (double)small->h / image.h);
                const double ox = (small->w - image.w * f) / 2, oy = (small->h - image.h * f) / 2;
                toSource.m[0] *= f; toSource.m[2] = toSource.m[2] * f + ox;
                toSource.m[4] *= f; toSource.m[5] = toSource.m[5] * f + oy;
                from = small.get();
            }
        }
        warp(*from, *out, toLocal, toSource, false, quality);
        if (remember) warpMemo().put(hold, ImagePtr(), key, 13, out);
        return out;
    }

    // Is a list of motion keys at time t where a frame is anyway (filling the frame, not turned)?
    static bool motionIsIdentity(const std::vector<MotionKey>& keys, double t)
    {
        if (keys.empty()) return true;
        const MotionKey k = motionAt(keys, t);
        const MotionKey none;
        return k.x == none.x && k.y == none.y && k.w == none.w && k.h == none.h && k.rotation == 0;
    }

    // The event with its first 'stage' effects applied (those before pan/crop are already in mediaFrame, and
    // are passed over here).
    ImagePtr eventStage(const Event& e, int stage, double t)
    {
        if (stage <= 0) return mediaFrame(e, t);
        const auto key = std::make_tuple(1, e.id, stage, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = e.fx[(size_t)stage - 1];
        ImagePtr out;
        if (!isVideoFxSlot(slot) || slot.beforePanCrop) out = eventStage(e, stage - 1, t);
        else {
            // effects on an event count time in frames from where the event starts
            out = runFx(slot, frameNumber(t - e.start, fps), [&](double f) { return eventStage(e, stage - 1, e.start + f / fps); });
        }
        // Event Pan/Crop's mask with Apply to FX: the effects show inside the mask, the frame without them outside
        if (out && stage == (int)e.fx.size() && e.maskApplyToFx && maskActive(e) && out->w == W && out->h == H) {
            ImagePtr plain = eventStage(e, 0, t);
            const std::vector<float> cov = plain ? maskCoverage(maskAt(e.mask, t - e.start), maskToFrame(e, p.findMedia(e.mediaId), t, W, H, par), W, H)
                                                 : std::vector<float>();
            if (!cov.empty() && plain->depth == out->depth && plain->w == W && plain->h == H)
                if (ImagePtr mixed = mixByCoverage(*plain, *out, cov)) out = mixed;
        }
        memoPut(key, out);
        return out;
    }

    // Whether the frame that starts at time t shows event e, in VEGAS's 100 ns ticks.
    static bool coversFrame(const Event& e, double t)
    {
        const int64_t f = (int64_t)std::ceil(t * 1.0e7 - 1e-3);
        const int64_t s = (int64_t)std::llround(e.start * 1.0e7), len = (int64_t)std::llround(e.length * 1.0e7);
        return s <= f && f < s + len;
    }

    // The track's events at time t mixed together (fades, cross-fades, transitions).
    ImagePtr trackBase(const Track& tr, double t)
    {
        const auto key = std::make_tuple(2, tr.id, 0, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        std::vector<const Event*> active;
        for (const auto& e : tr.events)
            if (!e.mute && !e.adjustment && coversFrame(e, t)) active.push_back(&e);
        ImagePtr out;
        // two events overlapping with an OpenFX transition chosen on the later one
        if (active.size() == 2 && active[1]->transition.kind == "ofx" && !active[1]->transition.bypass && !opt.skipEffects &&
            active[0]->end() < active[1]->end() && eng.fx_.video(active[1]->transition.id)) {
            const Event& a = *active[0];
            const Event& b = *active[1];
            ofx::Instance* inst = eng.fx_.video(b.transition.id);
            const double span = a.end() - b.start;
            double prog = span > 0 ? std::min(1.0, std::max(0.0, (t - b.start) / span)) : 1.0;
            // VEGAS's Transition Progress envelope, when the event has one, says how far it is instead.
            prog = std::min(1.0, std::max(0.0, envelopeValue(b.envelopes, "transition", t - b.start, prog)));
            const bool atFrameSize = atPreviewSize(inst);
            auto from = [&](double f) -> ImagePtr { ImagePtr i = eventStage(a, (int)a.fx.size(), undersampled(a, b.start + f / fps)); return atFrameSize ? toFrameSize(i ? i : blank()) : toProjectSize(i ? i : blank()); };
            auto to = [&](double f) -> ImagePtr { ImagePtr i = eventStage(b, (int)b.fx.size(), undersampled(b, b.start + f / fps)); return atFrameSize ? toFrameSize(i ? i : blank()) : toProjectSize(i ? i : blank()); };
            {
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                if (ofx::Param* tp = inst->params.find(kOfxImageEffectTransitionParamName))
                    if (tp->dim == 1) { tp->keys.clear(); tp->value[0] = prog; }
            }
            if (parallelOn() && inst->plugin && inst->plugin->safety != 0 && !usesUnsafeFx(tr)) {
                const double tf = b.start + frameNumber(t - b.start, fps) / fps;
                parallelTasks(2, [&](int i) { const FinalRenderScope helperFinal(!opt.interactive); const Event& ev = i ? b : a; eventStage(ev, (int)ev.fx.size(), undersampled(ev, tf)); });
            }
            ofx::RenderGate::Hold own(eng.fx_.renderLock(inst));
            // if waiting would deadlock with another frame's render, the transition is left out of this one.
            if (own.ok()) {
                inst->setClipSource(kOfxImageEffectTransitionSourceFromClipName, from);
                inst->setClipSource(kOfxImageEffectTransitionSourceToClipName, to);
                std::string err;
                out = inst->render(frameNumber(t - b.start, fps), fxOptions(inst), &err);
                inst->setClipSource(kOfxImageEffectTransitionSourceFromClipName, [](double) { return ImagePtr(); });
                inst->setClipSource(kOfxImageEffectTransitionSourceToClipName, [](double) { return ImagePtr(); });
                if (!out && !cancelled()) eng.fx_.setError(b.transition.id, err);
                out = inDepth(toFrameSize(out));
            }
        }
        if (!out) {
            // how much of each event shows (the events that show at all: their frames are made at the same time, see below)
            std::vector<std::pair<const Event*, double>> shown;
            for (const Event* e : active) {
                const double level = std::min(1.0, std::max(0.0, e->gain * envelopeValue(e->envelopes, "composite", t - e->start, 1.0)));
                const double w = overlapWeight(tr, *e, t, p.autoCrossfades) * eventFade(*e, t) * level;
                if (w > 0.0) shown.push_back(std::make_pair(e, w));
            }
            // Events that cross-fade (or are layered on one track) are independent until they are mixed.
            std::vector<ImagePtr> pics(shown.size());
            std::vector<char> made(shown.size(), 0);
            if (shown.size() >= 2 && parallelOn() && !usesUnsafeFx(tr)) {
                parallelTasks((int)shown.size(), [&](int i) {
                    const FinalRenderScope helperFinal(!opt.interactive);
                    const Event* e = shown[(size_t)i].first;
                    pics[(size_t)i] = eventStage(*e, opt.skipEffects ? 0 : (int)e->fx.size(), undersampled(*e, t));
                });
                std::fill(made.begin(), made.end(), 1);
            }
            for (size_t si = 0; si < shown.size(); ++si) {
                const Event* e = shown[si].first;
                const double w = shown[si].second;
                ImagePtr img = made[si] ? pics[si] : eventStage(*e, opt.skipEffects ? 0 : (int)e->fx.size(), undersampled(*e, t));
                if (!img) continue;
                if (!out && w >= 0.999) { out = img; continue; }       // the usual case: one event, fully shown
                if (!out) out = blank();
                else if (out.use_count() > 1 || out == img) out = out->clone();
                // a frame straight from the cache is never written to: 'out' is a fresh one from here on.
                if (out->w == img->w && out->h == img->h) addScaled(*out, *img, (float)w);
            }
        }
        memoPut(key, out);
        return out;
    }

    ImagePtr trackStage(const Track& tr, int stage, double t)
    {
        if (stage <= 0 || opt.skipEffects) return trackBase(tr, t);
        const auto key = std::make_tuple(3, tr.id, stage, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = tr.fx[(size_t)stage - 1];
        ImagePtr out;
        ImagePtr in = trackStage(tr, stage - 1, t);
        if (!isVideoFxSlot(slot) || !in) out = in;                     // nothing playing
        else out = runFx(slot, frameNumber(t, fps), [&](double f) { return trackStage(tr, stage - 1, f / fps); });
        memoPut(key, out);
        return out;
    }

    // How many of the track's effects work before it is composited (VEGAS's Composite button in its effect chain): the
    // chain keeps those first, then the ones with FxSlot::afterComposite.
    static int preCompositeCount(const Track& tr)
    {
        int n = 0;
        while (n < (int)tr.fx.size() && !tr.fx[(size_t)n].afterComposite) ++n;
        return n;
    }

    // The track's finished frame: its effects after compositing, if it has any, on the frame below.
    ImagePtr trackFrame(const Track& tr, double t) { return trackPost(tr, (int)tr.fx.size(), t); }

    ImagePtr trackPost(const Track& tr, int stage, double t)
    {
        const int pre = preCompositeCount(tr);
        if (stage <= pre || opt.skipEffects) return trackMoved(tr, t);
        const auto key = std::make_tuple(10, tr.id, stage, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = tr.fx[(size_t)stage - 1];
        ImagePtr in = trackPost(tr, stage - 1, t);
        ImagePtr out;
        if (!isVideoFxSlot(slot) || !in) out = in;
        else out = runFx(slot, frameNumber(t, fps), [&](double f) { return trackPost(tr, stage - 1, f / fps); });
        memoPut(key, out);
        return out;
    }

    // The track's frame after its effects before compositing, then where Track Motion puts it (with its 2D Shadow and Glow).
    ImagePtr trackMoved(const Track& tr, double t)
    {
        ImagePtr layer = trackStage(tr, preCompositeCount(tr), t);
        if (!layer) return layer;
        if (tr.shadow2d.empty() && tr.glow2d.empty()) {
            if (motionIsIdentity(tr.motion, t)) return layer;
            return trackMotion(tr.motion, *layer, t, layer);
        }
        ImagePtr moved = motionIsIdentity(tr.motion, t) ? layer->clone() : trackMotion(tr.motion, *layer, t);
        if (!moved) return layer;
        // the shape goes where the motion puts the frame, moved and sized by the shadow's.
        const MotionKey m = motionAt(tr.motion, t);
        for (const std::vector<GlowKey>* keys : {&tr.glow2d, &tr.shadow2d}) {
            if (keys->empty()) continue;
            const GlowKey g = glowAt(*keys, t);
            MotionKey k = m;
            k.x += g.x;
            k.y += g.y;
            k.w *= g.w;
            k.h *= g.h;
            ImagePtr shape = trackMotion(std::vector<MotionKey>{k}, *layer, t);
            if (shape) underlayShape(*moved, *shape, g, H);
        }
        return moved;
    }

    // ------------------------------------------------------------------ compositing the tracks
    // Is the track left out at time t (muted, another track soloed, or its "mute" envelope)?
    static bool trackOff(const Track& tr, double t, bool solo)
    {
        if (tr.mute || (solo && !tr.solo)) return true;
        return trackAutomationValue(tr, "mute", t) >= 0.5;
    }

    // How solid the track is laid on: its level times its "composite" envelope.
    static float trackOpacity(const Track& tr, double t)
    {
        return (float)std::min(1.0, std::max(0.0, tr.level * trackAutomationValue(tr, "composite", t)));
    }

    // The track's frame as it is laid over what is below: with its "fadecolor" envelope's colour.
    ImagePtr trackLayer(const Track& tr, double t)
    {
        {
            std::lock_guard<std::mutex> lk(memoMutex);
            auto it = layers.find(std::make_pair(&tr, tick(t)));
            if (it != layers.end()) return it->second;
        }
        return trackLayerNow(tr, t);
    }

    ImagePtr trackLayerNow(const Track& tr, double t)
    {
        const double tp = (tr.bypassMotionBlur && opt.blurBase >= 0) ? opt.blurBase : t;
        ImagePtr layer = trackFrame(tr, tp);
        if (!layer || layer->w != W || layer->h != H) return nullptr;
        const double fc = trackAutomationValue(tr, "fadecolor", t);
        if (std::fabs(fc) > 1e-6) {
            layer = layer->clone();
            if (layer) fadeToColour(*layer, fc, tr.fadeTop, tr.fadeBottom);
        }
        return inDepth(layer);
    }

    static int levelOf(const Track& tr) { return std::max(0, tr.compositeLevel); }

    // ------------------------------------------------------------------ the parts of a frame that stand alone
    // Whether the frame's independent parts may be made on several threads at once: asked for, and more than one
    // processor to do it on.
    bool parallelOn() const { return opt.parallel && processorCount() > 1; }

    // Whether one of the track's effects, transitions or generators is an OpenFX plug-in that says it is not thread
    // safe at all.
    bool usesUnsafeFx(const Track& tr) const
    {
        auto unsafe = [&](uint64_t id) {
            const ofx::Instance* inst = eng.fx_.video(id);
            return inst && inst->plugin && inst->plugin->safety == 0;
        };
        for (const FxSlot& f : tr.fx) if (isVideoFxSlot(f) && !isBuiltinVideo(f) && unsafe(f.id)) return true;
        for (const Event& e : tr.events) {
            for (const FxSlot& f : e.fx) if (isVideoFxSlot(f) && !isBuiltinVideo(f) && unsafe(f.id)) return true;
            if (e.transition.kind == "ofx" && unsafe(e.transition.id)) return true;
            if (const MediaItem* m = p.findMedia(e.mediaId)) if (m->generated && unsafe(m->generator.id)) return true;
        }
        return false;
    }

    // The tracks whose frame (trackLayer) compositeTracks is going to ask for, for the siblings vt[begin, end) at
    // time t: those it lays on the canvas with a frame of their own to make.
    void layersWanted(const std::vector<const Track*>& vt, size_t begin, size_t end, double t, bool solo, std::vector<const Track*>& out) const
    {
        for (const auto& sib : siblingsOf(vt, begin, end)) {
            const Track& tr = *vt[sib.first];
            const bool parent = sib.second > sib.first + 1;
            if (in3d(tr, parent)) continue;
            if (!parent) {
                if (tr.adjustment || trackOff(tr, t, solo)) continue;
                out.push_back(&tr);
                continue;
            }
            if (tr.mute) continue;
            layersWanted(vt, sib.first + 1, sib.second, t, solo && !tr.solo, out);
            if (!trackOff(tr, t, solo)) out.push_back(&tr);
        }
    }

    // Makes the frames of the video tracks at time t.
    void prefetchLayers(const std::vector<const Track*>& vt, double t, bool solo)
    {
        if (!parallelOn()) return;
        std::vector<const Track*> wanted, cands;
        layersWanted(vt, 0, vt.size(), t, solo, wanted);
        for (const Track* tr : wanted) {
            const double tp = (tr->bypassMotionBlur && opt.blurBase >= 0) ? opt.blurBase : t;
            bool any = false;
            for (const Event& e : tr->events) if (!e.mute && !e.adjustment && coversFrame(e, tp)) { any = true; break; }
            if (any && !usesUnsafeFx(*tr)) cands.push_back(tr);
        }
        if (cands.size() < 2) return;
        std::vector<ImagePtr> made(cands.size());
        parallelTasks((int)cands.size(), [&](int i) { const FinalRenderScope helperFinal(!opt.interactive); made[(size_t)i] = trackLayerNow(*cands[(size_t)i], t); });
        std::lock_guard<std::mutex> lk(memoMutex);
        for (size_t i = 0; i < cands.size(); ++i) layers[std::make_pair(cands[i], tick(t))] = made[i];
    }

    // Lays the video tracks vt[begin, end) over 'canvas', the lowest first.
    void compositeTracks(Image& canvas, const std::vector<const Track*>& vt, size_t begin, size_t end, double t, bool solo, bool* untouched = nullptr)
    {
        if (untouched && !gComposeFast.load()) { canvas.clear(); *untouched = false; }
        auto settle = [&] { if (untouched && *untouched) { canvas.clear(); *untouched = false; } };
        const std::vector<std::pair<size_t, size_t>> siblings = siblingsOf(vt, begin, end);
        for (size_t k = siblings.size(); k-- > 0;) {
            const Track& tr = *vt[siblings[k].first];
            const bool parent = siblings[k].second > siblings[k].first + 1;
            if (in3d(tr, parent)) {
                settle();
                // 3D Source Alpha: this track and the 3D ones right above it share one 3D space.
                size_t top = k;
                while (top > 0 && in3d(*vt[siblings[top - 1].first], siblings[top - 1].second > siblings[top - 1].first + 1)) --top;
                std::vector<Card3d> cards;
                for (size_t j = top; j <= k; ++j)
                    gatherCards(vt, siblings[j].first, siblings[j].second, t, solo, std::vector<MotionKey>(), cards);
                drawCards(canvas, cards);
                k = top;
                continue;
            }
            if (!parent && tr.adjustment) {
                // an adjustment track: its effects work on the frame so far (everything under it here)
                if (!trackOff(tr, t, solo)) { settle(); adjust(canvas, tr, vt, siblings[k].second, end, t, solo); }
                continue;
            }
            if (!parent) {
                if (trackOff(tr, t, solo)) continue;
                if (untouched && *untouched) for (const Event& e : tr.events) if (e.adjustment) { settle(); break; }
                adjustByEvents(canvas, tr, vt, siblings[k].second, end, t, solo);
                ImagePtr layer = trackLayer(tr, t);
                if (layer) {
                    const float op = trackOpacity(tr, t);
                    if (untouched && *untouched && op > 0.f && blendOf(tr.composite) == Blend::Over && layer->depth == PixelDepth::Byte && canvas.depth == PixelDepth::Byte &&
                        layer->w == canvas.w && layer->h == canvas.h && (unsigned)(std::min(1.f, op) * 255.f + 0.5f) == 255u) {
                        std::memcpy(canvas.data.get(), layer->data.get(), canvas.byteSize());
                        *untouched = false;
                        gFastCopies.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        settle();
                        blendLayer(canvas, *layer, op, blendOf(tr.composite));
                    }
                }
                continue;
            }
            if (tr.mute) continue;
            ImagePtr group = groupFrame(vt, siblings[k].first, siblings[k].second, t, solo);
            if (!group) continue;
            if (!motionIsIdentity(tr.parentMotion, t)) {
                ImagePtr moved = trackMotion(tr.parentMotion, *group, t);
                if (moved) group = moved;
            }
            settle();
            blendLayer(canvas, *group, 1.f, blendOf(tr.parentComposite));
        }
    }

    // The tracks vt[begin, end) as siblings, each with the end of its compositing children: (the track, the end).
    static std::vector<std::pair<size_t, size_t>> siblingsOf(const std::vector<const Track*>& vt, size_t begin, size_t end)
    {
        std::vector<std::pair<size_t, size_t>> siblings;
        for (size_t i = begin; i < end;) {
            size_t j = i + 1;
            while (j < end && levelOf(*vt[j]) > levelOf(*vt[i])) ++j;
            siblings.push_back(std::make_pair(i, j));
            i = j;
        }
        return siblings;
    }

    // A compositing parent's group (vt[i] with its children up to 'end'), before its Parent Motion.
    ImagePtr groupFrame(const std::vector<const Track*>& vt, size_t i, size_t end, double t, bool solo)
    {
        const Track& tr = *vt[i];
        ImagePtr group = Image::make(W, H, pd, false);
        if (!group) return group;
        bool untouched = true;
        compositeTracks(*group, vt, i + 1, end, t, solo && !tr.solo, &untouched);
        if (untouched) group->clear();
        if (!trackOff(tr, t, solo)) {
            if (tr.adjustment) adjust(*group, tr, vt, i + 1, end, t, solo);
            adjustByEvents(*group, tr, vt, i + 1, end, t, solo);
            ImagePtr own = trackLayer(tr, t);
            const Blend mode = blendOf(tr.composite);
            if (own && mode == Blend::Multiply) maskMultiply(*group, *own, trackOpacity(tr, t));
            else if (own) blendLayer(*group, *own, trackOpacity(tr, t), mode);
        }
        return group;
    }

    // ------------------------------------------------------------------ 3D Source Alpha
    // Is a sibling placed in 3D: a track in 3D Source Alpha, or a compositing parent whose Parent Composite Mode is?
    static bool in3d(const Track& tr, bool parent)
    {
        if (parent) return tr.parentComposite == "3dsrcalpha";
        return !tr.adjustment && tr.composite == "3dsrcalpha";
    }

    // The cards sibling vt[i] (with its children up to 'end') adds to a 3D space, placed further by 'outer' (the
    // Parent Motion of the parents round it, nearest first).
    void gatherCards(const std::vector<const Track*>& vt, size_t i, size_t end, double t, bool solo, const std::vector<MotionKey>& outer,
                     std::vector<Card3d>& cards)
    {
        const Track& tr = *vt[i];
        const int order = (int)(vt.size() - i);
        auto ownCard = [&](const std::vector<MotionKey>& after) {
            if (trackOff(tr, t, solo)) return;
            ImagePtr image = trackStage(tr, (int)tr.fx.size(), t);
            if (!image) return;
            const double fc = trackAutomationValue(tr, "fadecolor", t);
            if (std::fabs(fc) > 1e-6) {
                image = image->clone();
                if (image) fadeToColour(*image, fc, tr.fadeTop, tr.fadeBottom);
            }
            image = inDepth(image);
            if (!image) return;
            Card3d c;
            c.frame = image;
            c.moves.push_back(motionAt(tr.motion, t));
            c.moves.insert(c.moves.end(), after.begin(), after.end());
            c.opacity = trackOpacity(tr, t);
            c.order = order;
            cards.push_back(c);
        };
        if (end <= i + 1) {
            ownCard(outer);
            return;
        }
        std::vector<MotionKey> moves;
        moves.push_back(motionAt(tr.parentMotion, t));
        moves.insert(moves.end(), outer.begin(), outer.end());
        const std::vector<std::pair<size_t, size_t>> kids = siblingsOf(vt, i + 1, end);
        bool all = tr.composite == "3dsrcalpha" || trackOff(tr, t, solo);
        for (const auto& kid : kids)
            if (!in3d(*vt[kid.first], kid.second > kid.first + 1)) all = false;
        if (all) {
            for (const auto& kid : kids) gatherCards(vt, kid.first, kid.second, t, solo, moves, cards);
            ownCard(moves);
            return;
        }
        ImagePtr group = groupFrame(vt, i, end, t, solo);
        if (!group) return;
        Card3d c;
        c.frame = group;
        c.moves = moves;
        c.order = order;
        cards.push_back(c);
    }

    // Draws the cards of one 3D space over the canvas.
    void drawCards(Image& canvas, const std::vector<Card3d>& cards)
    {
        std::vector<ImagePtr> pics;
        std::vector<std::vector<float>> depths;
        std::vector<Card3d> drawn;
        for (const Card3d& c : cards) {
            if (!c.frame || c.opacity <= 0.f) continue;
            ImagePtr out = Image::make(W, H, canvas.depth, false);
            if (!out) continue;
            std::vector<float> z;
            warp3d(*c.frame, c, par, *out, z, quality);
            pics.push_back(out);
            depths.push_back(std::move(z));
            drawn.push_back(c);
        }
        if (!drawn.empty()) layCards(canvas, pics, depths, drawn);
    }

    // ------------------------------------------------------------------ adjustment tracks
    // VEGAS's adjustment tracks: a video track whose effects work on the frame of everything under it (within
    // its compositing group, if it is in one) rather than on events of its own (it has none; any it has are not
    // shown). The result takes the place of that frame: at the track's level (opacity), mixed with what it was;
    // with another compositing mode than Source Alpha, laid over it by that mode. Effects that look at other
    // moments get the frame under the track at those moments.
    //   'canvas' holds the tracks vt[below, end) composited at time t; it is changed in place.
    void adjust(Image& canvas, const Track& tr, const std::vector<const Track*>& vt, size_t below, size_t end, double t, bool solo)
    {
        if (opt.skipEffects || tr.fx.empty()) return;
        const float level = trackOpacity(tr, t);
        if (level <= 0.f) return;
        ImagePtr now = canvas.clone();
        if (!now) return;
        // the frame under the track at another moment (the one at t is the canvas as it is now)
        auto under = [&](double t2) -> ImagePtr {
            if (tick(t2) == tick(t)) return now;
            const auto key = std::make_tuple(7, tr.id, 0, tick(t2));
            ImagePtr hit;
            if (memoGet(key, hit)) return hit;
            ImagePtr c = Image::make(W, H, pd, false);
            if (c) {
                bool untouched = true;
                compositeTracks(*c, vt, below, end, t2, solo, &untouched);
                if (untouched) c->clear();
            }
            memoPut(key, c);
            return c;
        };
        ImagePtr out = adjustStage(tr, (int)tr.fx.size(), t, under);
        if (!out || out == now || out->w != canvas.w || out->h != canvas.h) return;
        out = inDepth(out);
        if (!out || out->depth != canvas.depth) return;
        const Blend mode = blendOf(tr.composite);
        if (mode == Blend::Over) blendImages(canvas, canvas, *out, std::min(1.f, level));
        else blendLayer(canvas, *out, level, mode);
    }

    // VEGAS Pro 23's adjustment events: each one on the track that lasts at time t works on the frame of the tracks
    // under it.
    void adjustByEvents(Image& canvas, const Track& tr, const std::vector<const Track*>& vt, size_t below, size_t end, double t, bool solo)
    {
        if (opt.skipEffects) return;
        for (const Event& e : tr.events) {
            if (!e.adjustment || e.mute || e.fx.empty() || !coversFrame(e, t)) continue;
            const double own = std::min(1.0, std::max(0.0, e.gain * envelopeValue(e.envelopes, "composite", t - e.start, 1.0)));
            const float level = (float)std::min(1.0, overlapWeight(tr, e, t, p.autoCrossfades) * eventFade(e, t) * own * trackOpacity(tr, t));
            if (level <= 0.f) continue;
            ImagePtr now = canvas.clone();
            if (!now) continue;
            auto under = [&](double t2) -> ImagePtr {
                if (tick(t2) == tick(t)) return now;
                const auto key = std::make_tuple(7, e.id, 0, tick(t2));
                ImagePtr hit;
                if (memoGet(key, hit)) return hit;
                ImagePtr c = Image::make(W, H, pd, false);
                if (c) {
                    bool untouched = true;
                    compositeTracks(*c, vt, below, end, t2, solo, &untouched);
                    if (untouched) c->clear();
                }
                memoPut(key, c);
                return c;
            };
            ImagePtr out = adjustEventStage(e, (int)e.fx.size(), t, under);
            if (!out || out == now || out->w != canvas.w || out->h != canvas.h) continue;
            out = inDepth(out);
            if (!out || out->depth != canvas.depth) continue;
            blendImages(canvas, canvas, *out, level);
        }
    }

    ImagePtr adjustEventStage(const Event& e, int stage, double t, const std::function<ImagePtr(double)>& under)
    {
        if (stage <= 0) return under(t);
        const auto key = std::make_tuple(6, e.id, stage, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = e.fx[(size_t)stage - 1];
        ImagePtr out;
        if (!isVideoFxSlot(slot)) out = adjustEventStage(e, stage - 1, t, under);
        else out = runFx(slot, frameNumber(t - e.start, fps), [&](double f) { return adjustEventStage(e, stage - 1, e.start + f / fps, under); });
        memoPut(key, out);
        return out;
    }

    // The frame under an adjustment track with its first 'stage' effects applied.
    ImagePtr adjustStage(const Track& tr, int stage, double t, const std::function<ImagePtr(double)>& under)
    {
        if (stage <= 0) return under(t);
        const auto key = std::make_tuple(6, tr.id, stage, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = tr.fx[(size_t)stage - 1];
        ImagePtr out;
        if (!isVideoFxSlot(slot)) out = adjustStage(tr, stage - 1, t, under);
        else out = runFx(slot, frameNumber(t, fps), [&](double f) { return adjustStage(tr, stage - 1, f / fps, under); });
        memoPut(key, out);
        return out;
    }

    // Every track laid over the ones below it.
    ImagePtr composite(double t)
    {
        const auto key = std::make_tuple(4, (uint64_t)0, 0, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        ImagePtr canvas = Image::make(W, H, pd, false);
        if (canvas) {
            bool untouched = true;
            std::vector<const Track*> vt;
            for (const Track& tr : p.tracks)
                if (tr.type == TrackType::Video) vt.push_back(&tr);
            prefetchLayers(vt, t, p.anySolo(TrackType::Video));
            compositeTracks(*canvas, vt, 0, vt.size(), t, p.anySolo(TrackType::Video), &untouched);
            if (untouched) canvas->clear();
        }
        memoPut(key, canvas);
        return canvas;
    }

    ImagePtr outStage(int stage, double t)
    {
        if (stage <= 0 || opt.skipEffects) return composite(t);
        const auto key = std::make_tuple(5, (uint64_t)0, stage, tick(t));
        ImagePtr hit;
        if (memoGet(key, hit)) return hit;
        const FxSlot& slot = p.videoOut[(size_t)stage - 1];
        ImagePtr out;
        if (!isVideoFxSlot(slot)) out = outStage(stage - 1, t);
        else out = runFx(slot, frameNumber(t, fps), [&](double f) { return outStage(stage - 1, f / fps); });
        memoPut(key, out);
        return out;
    }
};

ImagePtr VideoEngine::renderOnce(const Project& p, double seconds, const FrameOptions& opt)
{
    Pass pass(*this, p, opt);
    pass.timeline = seconds;
    ImagePtr top = pass.outStage((int)p.videoOut.size(), seconds);
    if (top && top->w == pass.W && top->h == pass.H) top = pass.inDepth(top);
    ImagePtr out;
    if (top && top->w == pass.W && top->h == pass.H && top->depth == PixelDepth::Byte) {
        bool mine = false;
        {
            std::lock_guard<std::mutex> lk(pass.memoMutex);
            auto it = pass.memo.find(std::make_tuple(4, (uint64_t)0, 0, Pass::tick(seconds)));
            mine = gComposeFast.load() && it != pass.memo.end() && it->second == top && top.use_count() == 2;
        }
        if (mine) gFastOwned.fetch_add(1, std::memory_order_relaxed);
        out = mine ? top : top->clone();
    } else {
        out = pass.blank();
        if (out && top && top->w == pass.W && top->h == pass.H) compositeOver(*out, *top, 1.f);
    }
    if (!out) return nullptr;
    if (!opt.transparent) makeOpaque(*out);                     // onto black: the finished frame is opaque
    return out;
}

// The weight motion blur of type 'type' gives a sub-frame at 's' (-1 .. 1, 0 the frame itself) of its reach.
static double blurWeight(const std::string& type, double s)
{
    const double a = std::fabs(s);
    if (type.find("box") != std::string::npos) return 1.0;
    if (type.find("pyramid") != std::string::npos) return std::max(0.0, 1.0 - a) + 1e-3;
    return std::exp(-0.5 * (a / 0.4) * (a / 0.4));             // Gaussian (the default)
}

// Whether the project's OpenFX effects, transitions and generators are all Reno's own (none from a plug-in file).
bool VideoEngine::onlyReno(const Project& p) const
{
    auto own = [&](const FxSlot& f) {
        if (!isVideoFxSlot(f) || isBuiltinVideo(f)) return true;
        const ofx::Instance* inst = fx_.video(f.id);
        return !inst || (inst->plugin && inst->plugin->binary && inst->plugin->binary->builtin);
    };
    for (const FxSlot& f : p.videoOut) if (!own(f)) return false;
    for (const Track& tr : p.tracks) {
        if (tr.type != TrackType::Video) continue;
        for (const FxSlot& f : tr.fx) if (!own(f)) return false;
        for (const Event& e : tr.events) {
            for (const FxSlot& f : e.fx) if (!own(f)) return false;
            if (e.transition.kind == "ofx" && !own(e.transition)) return false;
        }
    }
    for (const MediaItem& m : p.media) if (m.generated && !own(m.generator)) return false;
    return true;
}

ImagePtr VideoEngine::renderBlurred(const Project& p, double t, const FrameOptions& opt)
{
    const bool sub = !opt.interactive || opt.subFrames;
    const double blur = sub ? std::min(1.0, std::max(0.0, envelopeValue(p.videoEnvelopes, "motionblur", t, 0.0))) : 0.0;
    const int ss = sub ? (int)std::lround(std::min(8.0, std::max(1.0, envelopeValue(p.videoEnvelopes, "supersample", t, 1.0)))) : 1;
    if (!(blur > 1e-4) && ss <= 1) return renderOnce(p, t, opt);
    const double frame = 1.0 / p.settings.fps();
    // (time offset, weight) of each sub-frame
    std::vector<std::pair<double, double>> taps;
    if (blur > 1e-4) {
        // Motion blur: sub-frames over 'blur' frames either side of the frame.
        const std::string& type = p.settings.motionBlur;
        const bool asym = type.compare(0, 4, "asym") == 0;
        const int n = std::max(9, 2 * ss + 1);
        for (int k = 0; k < n; ++k) {
            const double s = asym ? -(double)k / n : -1.0 + (2.0 * k + 1.0) / n;
            taps.push_back(std::make_pair(s * blur * frame, blurWeight(type, s)));
        }
    } else {
        // Supersampling: 'ss' sub-frames spread evenly over the frame's own time, averaged.
        for (int k = 0; k < ss; ++k) taps.push_back(std::make_pair(k * frame / ss, 1.0));
    }
    double total = 0;
    for (const auto& tp : taps) total += tp.second;
    int fw = 0, fh = 0;
    frameSizeFor(p, opt, fw, fh);
    ImagePtr acc = Image::make(fw, fh, PixelDepth::Float, true);
    if (!acc || !(total > 0)) return renderOnce(p, t, opt);
    FrameOptions subOpt = opt;
    subOpt.blurBase = t;
    // The sub-frames stand alone until they are added up, in order: a few are made at the same time (a few only: each
    // is a float frame).
    const int inFlight = (opt.parallel && processorCount() > 1 && onlyReno(p)) ? std::min(4, processorCount()) : 1;
    for (size_t first = 0; first < taps.size(); first += (size_t)inFlight) {
        const size_t n = std::min((size_t)inFlight, taps.size() - first);
        std::vector<ImagePtr> imgs(n);
        parallelTasks((int)n, [&](int k) {
            const FinalRenderScope helperFinal(!opt.interactive);
            ImagePtr img = renderOnce(p, std::max(0.0, t + taps[first + (size_t)k].first), subOpt);
            if (img && img->depth != PixelDepth::Float) img = img->converted(PixelDepth::Float);
            imgs[(size_t)k] = img;
        });
        for (size_t k = 0; k < n; ++k) {
            const ImagePtr& img = imgs[k];
            if (!img) return nullptr;
            if (img->w != acc->w || img->h != acc->h) continue;
            const float f = (float)(taps[first + k].second / total);
            parallelRows(acc->h, (size_t)acc->w * 4 * 2, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    float* d = (float*)acc->row(y);
                    const float* s = (const float*)img->row(y);
                    for (int i = 0, m = acc->w * 4; i < m; ++i) d[i] += s[i] * f;
                }
            });
        }
    }
    return floatProject(p.settings) ? acc : acc->converted(PixelDepth::Byte);
}

ImagePtr VideoEngine::render(const Project& p, double seconds, const FrameOptions& opt)
{
    const profile::FrameScope profFrame;
    const FxRuntime::Hold hold(fx_);
    const FinalRenderScope finalScope(!opt.interactive);
    const double t = p.frameTime(p.frameAt(seconds));          // frames change on frame boundaries
    // the video output's "mute" envelope: black
    if (envelopeValue(p.videoEnvelopes, "mute", t, 0.0) >= 0.5) {
        int fw = 0, fh = 0;
        frameSizeFor(p, opt, fw, fh);
        ImagePtr out = Image::make(fw, fh, PixelDepth::Byte, true);
        if (out) makeOpaque(*out);
        return out;
    }
    ImagePtr image;
    const std::string& fo = p.settings.fieldOrder;
    if ((!opt.interactive || opt.subFrames) && (fo == "upper" || fo == "lower")) {
        // An interlaced project: each field is a frame of its own moment, half a frame apart, and the frame takes its
        // lines alternately from the two.
        ImagePtr second;
        if (opt.parallel && processorCount() > 1 && onlyReno(p)) {
            parallelTasks(2, [&](int i) {
                const FinalRenderScope helperFinal(!opt.interactive);
                if (i == 0) image = renderBlurred(p, t, opt);
                else second = renderBlurred(p, t + 0.5 / p.settings.fps(), opt);
            });
        } else {
            image = renderBlurred(p, t, opt);
            second = renderBlurred(p, t + 0.5 / p.settings.fps(), opt);
        }
        if (image && second && second->depth == image->depth && second->w == image->w && second->h == image->h) {
            const int firstParity = fo == "upper" ? 0 : 1;      // lines from the top, 0-based, that the first field has
            for (int y = 0; y < image->h; ++y)
                if (y % 2 != firstParity) std::memcpy(image->row(image->h - 1 - y), second->row(image->h - 1 - y), image->rowBytes);
        }
    } else {
        image = renderBlurred(p, t, opt);
    }
    if (!image) return nullptr;
    if (opt.cancel && opt.cancel->load()) return nullptr;
    // a linear-light project: the frame through the View transform (or a render template's Color space)
    if (linearLightProject(p.settings) && image->depth == PixelDepth::Float)
        fromSceneLinear(*image, opt.colorSpace.empty() ? p.settings.viewTransform : opt.colorSpace);
    // the video output's "fadecolor" envelope
    const double fc = envelopeValue(p.videoEnvelopes, "fadecolor", t, 0.0);
    if (std::fabs(fc) > 1e-6) fadeToColour(*image, fc, p.videoFadeTop, p.videoFadeBottom);
    if (image->depth != PixelDepth::Byte) image = image->converted(PixelDepth::Byte);
    return image;
}

ImagePtr VideoEngine::eventPicture(const Project& p, const Event& e, double seconds, int w, int h)
{
    const MediaItem* m = p.findMedia(e.mediaId);
    if (!m || m->generated || !m->info.hasVideo || m->offline) return nullptr;
    const double t = e.reverse ? seconds + 1.0 / p.settings.fps() : seconds;
    return pool_.videoFrame(e.id ^ 0x8000000000000000ull, *m, eventMediaTime(e, m, t), w, h);
}

// Decoding ahead: the frame at the first frame of the event, got as the compositor will get it and thrown away.
bool VideoEngine::prewarmEvent(const Project& p, const Event& e, const FrameOptions& opt, double keepSeconds, const std::atomic<bool>* stop)
{
    return prewarmEventAt(p, e, e.start, opt, keepSeconds, stop);
}

bool VideoEngine::prewarmEventAt(const Project& p, const Event& e, double at, const FrameOptions& opt, double keepSeconds, const std::atomic<bool>* stop)
{
    const MediaItem* m = p.findMedia(e.mediaId);
    if (!m || m->generated || m->offline || !m->info.hasVideo || m->info.isStill || e.mute || isProjectFile(m->path)) return false;
    if (stop && stop->load()) return false;
    Pass pass(*this, p, opt);
    const double t = p.frameTime(p.frameAt(std::max(at, e.start) + 1e-6));
    const double mt = pass.mediaTimeFor(e, m, std::max(t, e.start));
    MediaReadHintsScope hints(stop, false);
    pool_.videoFrame(e.id, *m, mt, pass.W, pass.H, pass.deinterlace, pass.pd, keepSeconds);
    return true;
}

}  // namespace reno
