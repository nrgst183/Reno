// The GPU versions of 3D Shuffle, 3D Blinds, 3D Fly In/Out and 3D Cascade, which the CPU draws as VEGAS's own rasteriser does (vfx3d.h, the front ends in vfx3dfx.h).
//
// The planner works out on the CPU, with the CPU's own functions, everything that is sequential integer code or does not depend on the pictures: the polygons (shufflePlan,
// blindsPlan, flyPlan, cascadePlan), their edge tables (detail::setup, detail::buildEdges) and, for every polygon and every picture row, the sort of the edges that cross the
// row and the strips it is cut into (the loop of drawPolygon(), with the strips kept instead of drawn). The kernels (tools/gpukernels_3d.py) do the rest, one thread per
// picture row: the strips swept pixel by pixel, the summed-area table of the pictures, the light and the blend, in the CPU's single and fixed-point arithmetic. The result is
// the CPU's, bit for bit, in 8-bit and float pictures (the CPU draws both on bytes).
// A frame the planner cannot have the same (a strip that starts left of the picture on a row below the first writes into the row above, which one thread per row cannot
// reproduce) is left to the CPU: the planner says false.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "vk3d.h"
#include "vfx3dfx.h"
#include "vkshaders/push_layouts.h"

namespace reno {
namespace ofx {
namespace builtin {

namespace {

typedef gpu::Plan Plan;
typedef gpu::Pass Pass;
namespace P = gpu::push;
namespace fx = vfx3d::fx;
namespace d3 = vfx3d::detail;
typedef std::map<std::string, std::vector<double>> PM;

const char* const kPrefix = "com.vegascreativesoftware:";
std::string fullId(const char* id) { return std::string(kPrefix) + id; }

inline float bitsOf(int32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

// the incoming frame goes up with the plan as the first w * h * 4 floats of a table, then 'tail' (as vktrans2.cpp's toTableTail)
int toTableTail(Ctx& c, Plan& p, std::vector<float> tail)
{
    if (c.src2Bytes) return p.addFrameTable(c.src2Bytes, c.src2RowBytes, c.src2Bgra, std::move(tail));
    if (c.src2.px.size() != (size_t)p.w * p.h * 4) return -1;
    std::vector<float> t = std::move(c.src2.px);
    t.insert(t.end(), tail.begin(), tail.end());
    return p.addTable(std::move(t));
}

double finiteIn(double v, double lo, double hi) { return v != v ? lo : std::min(hi, std::max(lo, v)); }
int indexIn(int v, int n) { return std::min(n - 1, std::max(0, v)); }

const int kSpanWords = 21;

void putSpan(std::vector<float>& t, const d3::Span& s)
{
    for (int k = 0; k < 8; ++k) t.push_back(bitsOf(s.xy[k]));
    t.push_back(s.uwT); t.push_back(s.uwTdy); t.push_back(s.vwT); t.push_back(s.vwTdy); t.push_back(s.iwT); t.push_back(s.iwTdy);
    t.push_back(s.uwB); t.push_back(s.vwB); t.push_back(s.iwB);
    t.push_back(s.iwDx); t.push_back(s.uwDx); t.push_back(s.vwDx);
    t.push_back(bitsOf(s.minx));
}

// The fragment records of the CPU rasteriser (vfx3d::Scratch) live on from row to row and polygon to polygon, and a pixel whose mask is not written again keeps what its
// record held (a strip that no edge cuts through the pixel; a first segment that starts at the pixel's right border leaves the rows at that border alone). One thread per
// row cannot carry that over from the row above, so the planner runs the CPU's own record handling (renderRow() without the colours) over every call in drawing order and,
// where a call would read a mask row it has not written and the record held something there, hands the kernel the records as they stand at the start of the call.
struct Pool {
    d3::Frag frag[40];
    Pool() { std::memset(this, 0, sizeof *this); }
};

// buildMask() of the header with a note of the rows it leaves alone: 'dirty' has a bit for each row of the record that still holds what the call began with
void buildMaskTrack(const d3::Frag& F, vfx3d::i32* M, const vfx3d::i32* snap, uint32_t& dirty, bool& need)
{
    using vfx3d::i32;
    const int n = F.segCount;
    if (n <= 0) {
        for (int r = 0; r < 32; ++r)
            if (((dirty >> r) & 1) && snap[r]) need = true;
        return;
    }
    for (int k = 0; k < n; ++k) {
        i32 c[4] = {F.segA[k][0][0], F.segA[k][0][1], F.segA[k][1][0], F.segA[k][1][1]};
        for (int q = 0; q < 4; ++q) {
            if (c[q] < 0) c[q] = 0;
            else if (c[q] > 0x4000) c[q] = 0x4000;
        }
        i32 xt, xb, yt, yb;
        if (c[3] > c[1]) { yb = c[3]; yt = c[1]; xt = c[0]; xb = c[2]; }
        else { yb = c[1]; yt = c[3]; xt = c[2]; xb = c[0]; }
        const i32 rowB = yb >> 9, rowT = yt >> 9;
        const i32 nr = rowB - rowT;
        if (k == 0)
            for (int r = 0; r < rowT; ++r) { M[r] = 0; dirty &= ~(1u << r); }
        const i32 dxr = (nr - 1) > 0 ? (xb - xt) / (nr - 1) : 0;
        i32 x = xt;
        for (int r = rowT; r < rowB; ++r) {
            const int sh = x >> 9;
            if (sh < 32) {
                const vfx3d::u32 m = 0xffffffffu >> (sh & 31);
                if (k == 0) { M[r] = (i32)m; dirty &= ~(1u << r); }
                else M[r] ^= (i32)m;
            } else if (k == 0 && ((dirty >> r) & 1) && snap[r]) {
                need = true;
            }
            if (r == rowT) x = vfx3d::wrapAdd(x, vfx3d::cvtt((double)((rowT << 9) - yt + 0x200) * (double)dxr * 0.00006103515625));
            else x = vfx3d::wrapAdd(x, dxr);
        }
        if (k == 0)
            for (int r = rowB; r < 32; ++r) { M[r] = 0; dirty &= ~(1u << r); }
    }
}

// renderRow() without the colours: moves the records on; snap gets the masks of the first nsp records as they were at the start. need: the call used a row it did not write.
void simulateCall(Pool& pool, d3::Span* sp, int nsp, int y, int w, vfx3d::i32 (*snap)[32], bool& need)
{
    using vfx3d::i32;
    uint32_t dirty[40];
    for (int j = 0; j < nsp && j < 36; ++j) {
        std::memcpy(snap[j], pool.frag[j].mask, 128);
        dirty[j] = 0xffffffffu;
    }
    need = false;
    i32 x = sp[0].minx >> 14;
    for (int i = 1; i < nsp; ++i) {
        const i32 m = sp[i].minx >> 14;
        if (m < x) x = m;
    }
    i32 bnd = (sp[0].minx + 0x4000) & ~0x3fff;
    if (x >= w) return;
    d3::Frag* frags = pool.frag;
    int nextSp = 0, nalloc = 0;
    d3::Frag* head = nullptr;
    for (;;) {
        while (nextSp < nsp && sp[nextSp].minx < bnd && nalloc < 40) {
            d3::Frag* A = &frags[nalloc++];
            i32 keep[32];
            std::memcpy(keep, A->mask, sizeof keep);
            std::memset(A, 0, sizeof(d3::Frag));
            std::memcpy(A->mask, keep, sizeof keep);
            A->next = nullptr;
            A->sp = &sp[nextSp];
            if (!head) head = A;
            else {
                d3::Frag* t = head;
                while (t->next) t = t->next;
                t->next = A;
            }
            d3::fragInit(sp[nextSp], *A, y);
            ++nextSp;
        }
        i32 M[32];
        bool minit = false;
        d3::Frag** link = &head;
        d3::Frag* A = head;
        while (A) {
            if (A->segCount != -1) {
                if (!minit) {
                    std::memset(M, 0, sizeof M);
                    minit = true;
                }
                const int j = (int)(A - frags);
                buildMaskTrack(*A, A->mask, snap[j], dirty[j], need);
                for (int i = 0; i < 32; ++i) {
                    A->mask[i] &= ~M[i];
                    M[i] |= A->mask[i];
                }
            }
            d3::fragAdvance(*A->sp, *A);
            if (A->finished == 0) {
                link = &A->next;
                A = A->next;
            } else {
                A = A->next;
                *link = A;
            }
        }
        ++x;
        bnd += 0x4000;
        if (x >= w) break;
        if (nextSp == nsp && head == nullptr) break;
    }
}

// What the planner collects over all polygons: the strips, the calls of renderRow() (a row's strips together, or each on its own) and the records' start state where it matters
struct CallRec {
    int32_t firstSpan, nsp, stale;      // stale: index in 'stale' of nsp * 32 words, or -1
};
struct Collected {
    std::vector<d3::Span> spans;
    std::vector<CallRec> calls;
    std::vector<int32_t> stale;
    Pool pool;
};

struct PolyRows {
    int y0 = 0, y1 = 0;
    std::vector<int32_t> groups;        // per row: the first call, the number of calls
    float nrm[3] = {0, 0, 0};
    int src = 0;
};

// one call of renderRow(): kept (spans copied, start state noted); false when the frame must be left to the CPU (a start left of the picture below its first row writes into
// the row above or further, which one thread per row cannot reproduce)
bool addCall(Collected& C, d3::Span* sp, int nsp, int y, int w)
{
    vfx3d::i32 x = sp[0].minx >> 14;
    for (int i = 1; i < nsp; ++i) x = std::min(x, sp[i].minx >> 14);
    if ((x < 0 && y > 0) || x < -2) return false;
    vfx3d::i32 snap[36][32];
    bool need = false;
    simulateCall(C.pool, sp, nsp, y, w, snap, need);
    CallRec cr;
    cr.firstSpan = (int32_t)C.spans.size();
    cr.nsp = nsp;
    cr.stale = -1;
    if (need) {
        cr.stale = (int32_t)C.stale.size();
        for (int j = 0; j < nsp; ++j)
            for (int r = 0; r < 32; ++r) C.stale.push_back(snap[j][r]);
    }
    for (int k = 0; k < nsp; ++k) C.spans.push_back(sp[k]);
    C.calls.push_back(cr);
    return true;
}

// what drawPolygon() does for one polygon, kept as lists
bool polygonRows(Collected& C, const vfx3d::Poly& poly, const float nrm[3], int src, int w, int h, PolyRows& out, bool& usable)
{
    usable = false;
    if (poly.n < 3 || poly.n > 36) return true;
    d3::Setup S;
    std::memset(&S, 0, sizeof S);
    d3::setup(S, poly, poly.n);
    d3::Edge E[40];
    vfx3d::i32 ymin, ymax;
    const int ne = d3::buildEdges(E, 36, S, poly, ymin, ymax);
    if (ne < 2) return true;
    usable = true;
    out.src = src;
    std::memcpy(out.nrm, nrm, 12);
    const int y0 = ymin < 0 ? 0 : ymin;
    const int y1 = ymax < h ? ymax : h;
    out.y0 = y0;
    out.y1 = std::max(y0, y1);
    d3::Slot slots[40];
    std::memset(slots, 0, sizeof slots);
    d3::Span spans[40];
    for (int y = y0; y < y1; ++y) {
        int cnt = 0;
        for (int k = 0; k < ne; ++k)
            if (E[k].ymin <= y && E[k].ymax > y) slots[cnt++].e = &E[k];
        const int firstCall = (int)C.calls.size();
        if (cnt >= 2) {
            const int partial = d3::sortEdges(slots, cnt, y);
            if (!partial) {
                const float fy = (float)y, fy1 = (float)(y + 1);
                const int np = ((cnt - 1) >> 1) + 1;
                for (int q = 0; q < np; ++q) {
                    const d3::Edge* L = slots[2 * q].e;
                    const d3::Edge* R = slots[2 * q + 1].e;
                    if (!L || !R) continue;
                    d3::Span sp;
                    std::memset(&sp, 0, sizeof sp);
                    d3::fillSpan(sp, L, R, fy, fy1, S);
                    if (!addCall(C, &sp, 1, y, w)) return false;
                }
            } else {
                const int ns = d3::buildSpans(spans, 36, slots, cnt, y, S);
                if (ns > 0 && !addCall(C, spans, ns, y, w)) return false;
            }
        }
        out.groups.push_back(firstCall);
        out.groups.push_back((int)C.calls.size() - firstCall);
    }
    return true;
}

struct Words {
    uint32_t hi, lo;
    explicit Words(double v)
    {
        uint64_t b;
        std::memcpy(&b, &v, 8);
        hi = (uint32_t)(b >> 32);
        lo = (uint32_t)b;
    }
};

// 3D Cascade's copy of the strips that have landed (blit::copyRect): the numbers the kernel works from; false: nothing is copied
struct RectCopy {
    int v[32];
    bool any = false;
};
RectCopy rectCopyParams(int w, int h, int l, int t, int r, int b);

// the common part: the table, the buffers and the passes
bool plan3d(Ctx& c, Plan& p, const fx::Plan& FP, int initKind, const RectCopy* rc)
{
    const int W = (int)p.w, H = (int)p.h;
    if (W < 1 || H < 1 || W > 16384 || H > 16384) return false;
    std::unique_ptr<Collected> C(new Collected());
    std::vector<PolyRows> polys;
    for (const fx::Item& it : FP.items) {
        PolyRows pr;
        bool usable = false;
        if (!polygonRows(*C, it.poly, it.nrm, it.src ? 1 : 0, W, H, pr, usable)) return false;
        if (usable) polys.push_back(std::move(pr));
    }
    if (std::getenv("RENO_VK3D_DEBUG")) {
        size_t nst = 0;
        for (const CallRec& cr : C->calls) nst += cr.stale >= 0;
        std::fprintf(stderr, "vk3d: %zu polygons, %zu calls, %zu strips, %zu calls with a start state\n", polys.size(), C->calls.size(), C->spans.size(), nst);
    }
    // the table after the frame: the polygon headers (8 words each), each polygon's rows (first call, number of calls), the calls (first strip, number of strips, start state),
    // the strips, the start states of the records, the numbers of the copied rectangle
    const size_t frameFloats = (size_t)W * H * 4;
    const size_t np = polys.size();
    size_t groupWords = 0;
    for (const PolyRows& pr : polys) groupWords += pr.groups.size();
    const size_t hdrWords = np * 8;
    const size_t callWords = C->calls.size() * 3;
    const size_t spanWords = C->spans.size() * kSpanWords;
    const size_t groupAbs = frameFloats + hdrWords, callAbs = groupAbs + groupWords, spanAbs = callAbs + callWords, staleAbs = spanAbs + spanWords;
    std::vector<float> tail;
    tail.reserve(hdrWords + groupWords + callWords + spanWords + C->stale.size() + 64);
    tail.resize(hdrWords, 0.f);
    size_t gOff = groupAbs;
    for (size_t k = 0; k < np; ++k) {
        const PolyRows& pr = polys[k];
        float* h = &tail[k * 8];
        h[0] = bitsOf(pr.y0);
        h[1] = bitsOf(pr.y1);
        h[2] = bitsOf((int32_t)gOff);
        h[4] = pr.nrm[0]; h[5] = pr.nrm[1]; h[6] = pr.nrm[2];
        h[7] = bitsOf(pr.src);
        gOff += pr.groups.size();
    }
    for (const PolyRows& pr : polys)
        for (int32_t g : pr.groups) tail.push_back(bitsOf(g));
    for (const CallRec& cr : C->calls) {
        tail.push_back(bitsOf(cr.firstSpan));
        tail.push_back(bitsOf(cr.nsp));
        tail.push_back(bitsOf(cr.stale < 0 ? -1 : (int32_t)(staleAbs + (size_t)cr.stale)));
    }
    for (const d3::Span& s : C->spans) putSpan(tail, s);
    for (int32_t v : C->stale) tail.push_back(bitsOf(v));
    uint32_t rcOff = 0;
    if (rc && rc->any) {
        rcOff = (uint32_t)(frameFloats + tail.size());
        for (int k = 0; k < 32; ++k) tail.push_back(bitsOf(rc->v[k]));
    }
    if (frameFloats + tail.size() >= 0x7fffffffu) return false;
    const int t = toTableTail(c, p, std::move(tail));
    if (t < 0) return false;
    const int sat = p.addBuf((uint32_t)W, (uint32_t)(2 * H), 4);
    const int surf = p.addBuf((uint32_t)W, (uint32_t)H, 4);
    {
        P::D3Lev pc = {p.w, p.h};
        Pass& ps = p.add("d3_lev", 0, sat, p.w, p.h);
        ps.table = t;
        ps.setPush(pc);
    }
    {
        P::D3Init pc = {p.w, p.h, (uint32_t)initKind, 0};
        p.add("d3_init", sat, surf, p.w, p.h).setPush(pc);
    }
    if (rc && rc->any) {
        P::D3Copyrect pc = {p.w, p.h, rcOff};
        Pass& ps = p.add("d3_copyrect", sat, surf, p.w, p.h);
        ps.table = t;
        ps.setPush(pc);
    }
    {
        P::D3Satrow pc = {p.w, p.h};
        p.add("d3_satrow", sat, sat, p.h * 2, 1).setPush(pc);
        P::D3Satcol pq = {p.w, p.h};
        p.add("d3_satcol", sat, sat, p.w * 2, 1).setPush(pq);
    }
    if (np > 0) {
        P::D3Poly pc;
        std::memset(&pc, 0, sizeof pc);
        pc.w = p.w; pc.h = p.h;
        pc.npoly = (uint32_t)np;
        pc.polyOff = (uint32_t)frameFloats;
        pc.callOff = (uint32_t)callAbs;
        pc.spanOff = (uint32_t)spanAbs;
        pc.mode = (FP.lightAmt > 0.0f) ? 2u : 1u;
        pc.fadeFix = vfx3d::cvtt(FP.fade * 16384.0);
        pc.lx = FP.lightX; pc.ly = FP.lightY; pc.lz = FP.lightZ; pc.lint = FP.lightAmt;
        const Words cx(16384.0 * (double)W), cy(16384.0 * (double)H);
        pc.cxh = cx.hi; pc.cxl = cx.lo; pc.cyh = cy.hi; pc.cyl = cy.lo;
        Pass& ps = p.add("d3_poly", sat, surf, p.h, 1);
        ps.table = t;
        ps.setPush(pc);
    }
    {
        P::D3Fin pc = {p.w, p.h};
        p.add("d3_fin", surf, 1, p.w, p.h).setPush(pc);
    }
    return true;
}

// ---- the settings, read as the CPU's front ends do (builtins_trans.cpp: blinds3d, cascade3d, fly3d, shuffle3d)
bool planShuffle3d(Ctx& c, Plan& p)
{
    const double light = finiteIn(c.d("SpecularLight"), 0.0, 1.0);
    return plan3d(c, p, fx::shufflePlan((int)p.w, (int)p.h, 1.0, light, finiteIn(c.progress, 0.0, 1.0)), 0, nullptr);
}

bool planBlinds3d(Ctx& c, Plan& p)
{
    fx::BlindsSettings S;
    S.divisions = indexIn(c.i("Divisions"), 129);
    S.extraSpins = indexIn(c.i("ExtraSpins"), 9);
    S.stagger = finiteIn(c.d("Stagger"), 0.0, 1.0);
    S.light = finiteIn(c.d("SpecularLight"), 0.0, 1.0);
    S.direction = indexIn(c.i("Direction"), 4);
    return plan3d(c, p, fx::blindsPlan((int)p.w, (int)p.h, 1.0, S, finiteIn(c.progress, 0.0, 1.0)), 0, nullptr);
}

bool planFly3d(Ctx& c, Plan& p)
{
    fx::FlySettings S;
    S.farX = finiteIn(c.d("FarXPosition"), 0.0, 1.0);
    S.farY = finiteIn(c.d("FarYPosition"), 0.0, 1.0);
    S.farZ = finiteIn(c.d("FarZPosition"), 0.0, 16.0);
    S.rotX = finiteIn(c.d("XRotations"), 0.0, 16.0);
    S.rotY = finiteIn(c.d("YRotations"), 0.0, 16.0);
    S.rotZ = finiteIn(c.d("ZRotations"), 0.0, 16.0);
    S.light = finiteIn(c.d("SpecularLight"), 0.0, 1.0);
    S.in = 1 - indexIn(c.i("Direction"), 2);
    // (flyInOut(): the surface starts as the picture that is not flying)
    return plan3d(c, p, fx::flyPlan((int)p.w, (int)p.h, 1.0, S, finiteIn(c.progress, 0.0, 1.0)), S.in == 0 ? 2 : 1, nullptr);
}

bool planCascade3d(Ctx& c, Plan& p)
{
    fx::CascadeSettings S;
    S.divisions = indexIn(c.i("Divisions"), 65);
    S.direction = indexIn(c.i("Direction"), 4);
    S.twist = finiteIn(c.d("Twist"), 0.0, 1.0);
    S.light = finiteIn(c.d("SpecularLight"), 0.0, 1.0);
    const int W = (int)p.w, H = (int)p.h;
    const fx::CascadePlan C = fx::cascadePlan(W, H, 1.0, S, finiteIn(c.progress, 0.0, 1.0));
    RectCopy rc;
    std::memset(rc.v, 0, sizeof rc.v);
    fx::Frame f;
    f.w = W; f.h = H; f.asp = 1.0; f.a = f.b = nullptr; f.out = nullptr;
    int l, t, r, b;
    if (fx::landedRect(f, C, l, t, r, b)) rc = rectCopyParams(W, H, l, t, r, b);
    return plan3d(c, p, C.plan, 1, &rc);
}

// blit::copyRect(): its set-up (the rectangle against the picture, the steps, the first and last row and column), done here; the kernel does the pixels
RectCopy rectCopyParams(int w, int h, int l, int t, int r, int b)
{
    namespace bl = fx::blit;
    RectCopy out;
    std::memset(out.v, 0, sizeof out.v);
    const double k14 = 6.103515625e-05;
    const bl::Rect R = {l, t, r, b};
    const bl::Rect D = R;
    const bl::Rect S1 = bl::isect(R, bl::Rect{0, 0, w << 14, h << 14});
    if (S1.x1 - S1.x0 < 0x4000 || S1.y1 - S1.y0 < 0x4000) return out;
    if (D.x1 - D.x0 < 0x400 || D.y1 - D.y0 < 0x400) return out;
    const int scx = bl::dtoi((S1.x1 - S1.x0) * 16384.0 / (D.x1 - D.x0)), scy = bl::dtoi((S1.y1 - S1.y0) * 16384.0 / (D.y1 - D.y0));
    if (scx <= 0 || scy <= 0) return out;
    const bl::Rect D1 = bl::isect(D, bl::Rect{0, 0, w << 14, h << 14});
    if (D1.x1 - D1.x0 < 0x400 || D1.y1 - D1.y0 < 0x400) return out;
    const bl::Rect M = {D.x0, D.y0, bl::dtoi((S1.x1 - S1.x0) * 16384.0 / scx) + D.x0, bl::dtoi((S1.y1 - S1.y0) * 16384.0 / scy) + D.y0};
    const bl::Rect Fr = bl::isect(M, D1);
    const bl::Rect sr = {bl::dtoi((Fr.x0 - D.x0) * (double)scx * k14) + S1.x0, bl::dtoi((Fr.y0 - D.y0) * (double)scy * k14) + S1.y0,
                         bl::dtoi((Fr.x1 - D.x0) * (double)scx * k14) + S1.x0, bl::dtoi((Fr.y1 - D.y0) * (double)scy * k14) + S1.y0};
    const bl::Rect S = bl::isect(sr, S1);
    const int sx = D1.x1 - D1.x0 < 0x8000 ? 0 : std::max(0, bl::dtoi((S.x1 - S.x0 - 0x4000) * 16384.0 / (D1.x1 - D1.x0 - 0x4000)));
    const int sy = D1.y1 - D1.y0 < 0x8000 ? 0 : std::max(0, bl::dtoi((S.y1 - S.y0 - 0x4000) * 16384.0 / (D1.y1 - D1.y0 - 0x4000)));
    const int px0 = bl::fl14(D1.x0), px1 = bl::ce14(D1.x1), py0 = bl::fl14(D1.y0), py1 = bl::ce14(D1.y1);
    int frc = ((py0 + 1) << 14) - D1.y0;
    const int lrc = D1.y1 - (py1 << 14) + 0x4000;
    int fcc = ((px0 + 1) << 14) - D1.x0;
    const int lcc = D1.x1 - (px1 << 14) + 0x4000;
    const int scol = bl::ce14(S.x1), srow = bl::ce14(S.y1);
    if (px1 - px0 < 2) fcc = D1.x1 - D1.x0;
    const int nrows = py1 - py0;
    if (nrows < 2) frc = D1.y1 - D1.y0;
    const int last = scol - 1;
    if (S.x0 < 0 || S.y0 < 0 || scol > w || srow > h || px0 < 0 || px1 > w || py0 < 0 || py1 > h) return out;
    out.any = true;
    int* v = out.v;
    v[0] = px0; v[1] = px1; v[2] = py0; v[3] = py1;
    v[4] = S.x0; v[5] = S.y0; v[6] = sx; v[7] = sy;
    v[8] = last; v[9] = srow; v[10] = fcc; v[11] = lcc;
    v[12] = frc; v[13] = lrc;
    // the first row of the dst rows between: y = k * sy + fs + S.y0
    const int fs = bl::dtoi(frc * (double)sy * k14);
    v[14] = fs;
    // the row of the last dst row (nrows > 1): (nrows - 2) * sy + fs + S.y0, less one step and a correction when there are more than two rows
    int ylast = (nrows - 2) * sy + fs + S.y0;
    if (nrows > 2) ylast += -sy - bl::dtoi(lrc * (double)sy * -k14);
    v[15] = ylast;
    // columns: the first step and the last one's correction (edge rows and rows between alike)
    v[16] = bl::dtoi(sx * (double)fcc * k14);
    v[17] = (px1 - px0 > 2) ? (-sx - bl::dtoi(lcc * (double)sx * -k14)) : 0;
    // the alphas of the edge rows: [first row, last row] x [first column, middle, last column]
    const int rcovs[2] = {frc, lrc};
    for (int e = 0; e < 2; ++e) {
        v[18 + e * 3] = (bl::dtoi(rcovs[e] * (double)fcc * k14) * 255) >> 14;
        v[19 + e * 3] = (rcovs[e] * 255) >> 14;
        v[20 + e * 3] = (bl::dtoi(lcc * (double)rcovs[e] * k14) * 255) >> 14;
    }
    v[24] = (fcc * 255) >> 14;
    v[25] = (lcc * 255) >> 14;
    return out;
}

// ---- GPU check cases
struct CaseSet { const char* id; std::vector<GpuCheckCase> cases; };

const std::vector<CaseSet>& caseSets()
{
    static const std::vector<CaseSet> sets = [] {
        std::vector<CaseSet> v;
        const double progs[] = {0.0, 0.07, 0.2, 0.4, 0.5, 0.63, 0.85, 1.0};
        {
            CaseSet s{"3dshuffle", {}};
            for (double pr : progs) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}}));
            for (double li : {0.0, 0.3}) s.cases.push_back(GpuCheckCase("light" + std::to_string(li), PM{{"Transition", {0.3}}, {"SpecularLight", {li}}}));
            for (double pr : {0.001, 0.49, 0.51, 0.999}) s.cases.push_back(GpuCheckCase("edge p" + std::to_string(pr), PM{{"Transition", {pr}}, {"SpecularLight", {0.6}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"3dflyinout", {}};
            for (int dir : {0, 1})
                for (double pr : progs) s.cases.push_back(GpuCheckCase("dir" + std::to_string(dir) + " p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Direction", {(double)dir}}}));
            for (int dir : {0, 1}) {
                s.cases.push_back(GpuCheckCase("corner dir" + std::to_string(dir), PM{{"Transition", {0.45}}, {"Direction", {(double)dir}}, {"FarXPosition", {0.0}}, {"FarYPosition", {1.0}}, {"FarZPosition", {1.0}}}));
                s.cases.push_back(GpuCheckCase("far dir" + std::to_string(dir), PM{{"Transition", {0.6}}, {"Direction", {(double)dir}}, {"FarXPosition", {1.0}}, {"FarYPosition", {0.0}}, {"FarZPosition", {16.0}}}));
                s.cases.push_back(GpuCheckCase("near dir" + std::to_string(dir), PM{{"Transition", {0.3}}, {"Direction", {(double)dir}}, {"FarZPosition", {0.0}}, {"XRotations", {0.0}}, {"YRotations", {0.0}}, {"ZRotations", {0.0}}}));
                s.cases.push_back(GpuCheckCase("spin dir" + std::to_string(dir), PM{{"Transition", {0.5}}, {"Direction", {(double)dir}}, {"XRotations", {3.5}}, {"YRotations", {0.3}}, {"ZRotations", {0.7}}, {"SpecularLight", {0.4}}}));
                s.cases.push_back(GpuCheckCase("no light dir" + std::to_string(dir), PM{{"Transition", {0.35}}, {"Direction", {(double)dir}}, {"SpecularLight", {0.0}}}));
            }
            v.push_back(s);
        }
        {
            CaseSet s{"3dblinds", {}};
            for (double pr : progs) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}}));
            for (int dir = 0; dir < 4; ++dir)
                for (double pr : {0.0, 0.3, 0.7, 1.0})
                    s.cases.push_back(GpuCheckCase("dir" + std::to_string(dir) + " p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Direction", {(double)dir}}, {"Divisions", {5}}, {"Stagger", {0.4}}}));
            for (int dv : {0, 1, 2, 3, 16, 60, 128})
                s.cases.push_back(GpuCheckCase("divisions" + std::to_string(dv), PM{{"Transition", {0.5}}, {"Divisions", {(double)dv}}}));
            for (int sp : {0, 1, 3, 8})
                s.cases.push_back(GpuCheckCase("spins" + std::to_string(sp), PM{{"Transition", {0.4}}, {"ExtraSpins", {(double)sp}}, {"Divisions", {4}}}));
            for (double st : {0.0, 0.5, 1.0})
                s.cases.push_back(GpuCheckCase("stagger" + std::to_string(st), PM{{"Transition", {0.55}}, {"Stagger", {st}}}));
            s.cases.push_back(GpuCheckCase("no light", PM{{"Transition", {0.45}}, {"SpecularLight", {0.0}}}));
            s.cases.push_back(GpuCheckCase("dim light vertical", PM{{"Transition", {0.45}}, {"SpecularLight", {0.35}}, {"Direction", {2}}}));
            v.push_back(s);
        }
        {
            CaseSet s{"3dcascade", {}};
            for (double pr : progs) s.cases.push_back(GpuCheckCase("p" + std::to_string(pr), PM{{"Transition", {pr}}}));
            for (int dir = 0; dir < 4; ++dir)
                for (double pr : {0.0, 0.3, 0.7, 1.0})
                    s.cases.push_back(GpuCheckCase("dir" + std::to_string(dir) + " p" + std::to_string(pr), PM{{"Transition", {pr}}, {"Direction", {(double)dir}}, {"Divisions", {5}}, {"Twist", {0.4}}}));
            for (int dv : {0, 1, 2, 3, 10, 20, 64})
                s.cases.push_back(GpuCheckCase("divisions" + std::to_string(dv), PM{{"Transition", {0.5}}, {"Divisions", {(double)dv}}}));
            for (int dir = 0; dir < 4; ++dir)
                for (double tw : {0.0, 1.0})
                    s.cases.push_back(GpuCheckCase("twist" + std::to_string(tw) + " dir" + std::to_string(dir), PM{{"Transition", {0.45}}, {"Twist", {tw}}, {"Direction", {(double)dir}}}));
            s.cases.push_back(GpuCheckCase("no light", PM{{"Transition", {0.45}}, {"SpecularLight", {0.0}}}));
            s.cases.push_back(GpuCheckCase("landed strips", PM{{"Transition", {0.9}}, {"Divisions", {7}}}));
            v.push_back(s);
        }
        return v;
    }();
    return sets;
}

}  // namespace

const std::vector<ExtraGpuEntry>& threeDGpuEntries()
{
    static const std::vector<ExtraGpuEntry> v = {{"3dshuffle", planShuffle3d}, {"3dblinds", planBlinds3d}, {"3dflyinout", planFly3d}, {"3dcascade", planCascade3d}};
    return v;
}

void threeDGpuCases(const std::string& id, std::vector<GpuCheckCase>& out)
{
    for (const CaseSet& s : caseSets())
        if (id == fullId(s.id)) out.insert(out.end(), s.cases.begin(), s.cases.end());
}

bool threeDGpuFloatTolerance(const std::string& id, double& tol)
{
    for (const CaseSet& s : caseSets())
        if (id == fullId(s.id)) { tol = 0.0; return true; }
    return false;
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
