// Titles & Text with the text split into lines, words and characters that move on their own (VEGAS's animations, vfxtitleanim.h) and with fonts, sizes, bold and italic
// that change inside the text. The layout is the one of vfxtitles.h (place) done per character: every character has its own face and size, a line is as high as its
// tallest ascent plus the tallest descent / line gap of its characters (with one size that is the font's line height, as before).
//
// Drawing: each character's outline is moved by the chain of the animated transforms (the whole text, its line, its word, the character), each about the centre of
// its own box, and drawn with the colours / outline of the style; a character that is not fully opaque or is blurred goes through a small layer of its own.
// The glow and blur of the whole text are applied to the finished layer.
//
// What is read from VEGAS's code and what is not: the maths of the transforms is the engine's (see vfxtitleanim.h); the boxes the centres refer to (the 9 places of a
// line / word / character box, "NearBaseline" ... "FarTop") are taken as the advance box of the characters and the line's ascent / descent, which is a GUESS, and so are
// the units of the blur and glow amounts (fractions of the width, as the shadow's blur).
#pragma once

#include "../core/rmath.h"
#include "vfxtitleanim.h"
#include "vfxtitles.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace reno {
namespace ofx {
namespace vfxtt {

struct CpFmt {
    double points = 0;                  // 0: the style's
    bool bold = false, italic = false;
    std::string font;                   // empty: the style's
};

struct RBox { double x0 = 0, x1 = 0; };

struct RGlyph {
    std::vector<TextSeg> segs;          // pixels, y down, at rest
    int line = 0, word = 0, ch = 0;
};

struct Rich {
    std::vector<RGlyph> glyphs;
    vfxta::Shape shape;
    // boxes in pixels at rest
    double bx = 0, by = 0, bw = 0, bh = 0, firstBase = 0, emPx = 0;
    double lastBase = 0, pitch = 0;      // the last line's baseline and the distance one more line would be below it (pixels)
    int nCp = 0;                        // the characters of the text (a line break counts one)
    struct Ln { double x0 = 0, x1 = 0, base = 0, top = 0, bot = 0; };
    std::vector<Ln> lines;
    std::vector<std::vector<RBox>> words;
    std::vector<std::vector<std::vector<RBox>>> chars;
};

// 'hidden' (by text index, may be null): characters that are not drawn at all (opacity 0 of the character or its word). When there are any, the block is
// placed by the vertical extent of the ink of the others instead of by the lines' boxes (the capture of an animation that has revealed one line only).
inline bool placeRich(const Style& S, const std::vector<CpFmt>& fmt, int W, int H, Rich& out, const std::vector<char>* hidden = nullptr)
{
    out = Rich();
    std::vector<uint32_t> cps;
    utf8Decode(S.text, cps);
    std::shared_ptr<TextFace> baseFace = face(S.font, S.bold, S.italic);
    if (!baseFace || W <= 0 || H <= 0) return false;
    struct Ch { uint32_t cp; int textIndex; double x, adv, E, asc, dsc; TextGlyph g; };
    std::vector<std::vector<Ch>> L;
    std::vector<int> lineFirst(1, 0);
    std::vector<double> lineA, lineD;
    std::map<std::string, std::map<uint32_t, TextGlyph>> caches;
    std::vector<std::shared_ptr<TextFace>> faces(cps.size());
    for (size_t i = 0; i < cps.size(); ++i) {
        if (cps[i] == '\r') continue;
        if (cps[i] == '\n') { lineFirst.push_back((int)i + 1); continue; }
        const CpFmt f = i < fmt.size() ? fmt[i] : CpFmt();
        const bool bold = S.bold || f.bold, italic = S.italic || f.italic;
        const std::string font = f.font.empty() ? S.font : f.font;
        std::shared_ptr<TextFace> F = (font == S.font && bold == S.bold && italic == S.italic) ? baseFace : face(font, bold, italic);
        if (!F) F = baseFace;
        faces[i] = F;
    }
    {
        size_t line = 0;
        std::vector<std::vector<size_t>> idx(1);
        for (size_t i = 0; i < cps.size(); ++i) {
            if (cps[i] == '\r') continue;
            if (cps[i] == '\n') { ++line; idx.emplace_back(); continue; }
            idx[line].push_back(i);
        }
        L.resize(idx.size());
        lineA.assign(idx.size(), 0.0);
        lineD.assign(idx.size(), 0.0);
        for (size_t li = 0; li < idx.size(); ++li) {
            double x = 0;
            double A = 0, D = 0;
            bool any = false;
            for (size_t k = 0; k < idx[li].size(); ++k) {
                const size_t i = idx[li][k];
                const CpFmt f = i < fmt.size() ? fmt[i] : CpFmt();
                const double pts = f.points > 0 ? f.points : S.points;
                const double E = std::max(0.0, pts) / 10.0;
                TextFace& F = *faces[i];
                const uint32_t cp = cps[i] == '\t' ? ' ' : cps[i];
                Ch ch;
                ch.cp = cp;
                ch.textIndex = (int)i;
                ch.E = E;
                ch.asc = F.ascent * E;
                ch.dsc = (F.descent + F.lineGap) * E;
                std::map<uint32_t, TextGlyph>& cache = caches[std::to_string((size_t)(const void*)&F)];
                auto it = cache.find(cp);
                if (it == cache.end()) {
                    TextGlyph g;
                    if (!F.glyph(cp, g)) g = TextGlyph();
                    it = cache.emplace(cp, g).first;
                }
                ch.g = it->second;
                ch.adv = ch.g.adv * E;
                ch.x = x;
                A = any ? std::max(A, ch.asc) : ch.asc;
                D = any ? std::max(D, ch.dsc) : ch.dsc;
                any = true;
                double kern = 0;
                if (k + 1 < idx[li].size() && faces[idx[li][k + 1]].get() == &F) {
                    const uint32_t nx = cps[idx[li][k + 1]] == '\t' ? ' ' : cps[idx[li][k + 1]];
                    kern = F.kern(cp, nx) * E;
                }
                x += ch.adv + kern + S.tracking;
                L[li].push_back(ch);
            }
            if (!any) { A = baseFace->ascent * S.points / 10.0; D = (baseFace->descent + baseFace->lineGap) * S.points / 10.0; }
            lineA[li] = A;
            lineD[li] = D;
        }
    }
    const size_t nl = L.size();
    std::vector<double> width(nl, 0.0), shift(nl, 0.0);
    for (size_t li = 0; li < nl; ++li) {
        double lo = 0, hi = 0;
        for (const Ch& ch : L[li]) { lo = std::min(lo, ch.x); hi = std::max(hi, ch.x + ch.adv); }
        for (Ch& ch : L[li]) ch.x -= lo;
        width[li] = hi - lo;
    }
    double blockW = 0;
    for (double w : width) blockW = std::max(blockW, w);
    if (nl == 1) blockW = width[0];
    const double ls = S.lineSpacing;
    std::vector<double> bl(nl, 0.0);
    for (size_t i = 1; i < nl; ++i) bl[i] = bl[i - 1] + (lineD[i - 1] + lineA[i]) * ls;       // (the baselines of all lines)
    double top = -lineA[0], bottom = bl.back() + lineD.back();
    if (hidden) {
        bool any = false;
        for (char h : *hidden) any = any || h;
        if (any) {
            double it = 1e30, ib = -1e30;
            for (size_t li = 0; li < nl; ++li)
                for (const Ch& ch : L[li]) {
                    if (ch.textIndex < (int)hidden->size() && (*hidden)[ch.textIndex]) continue;
                    for (const TextSeg& sg : ch.g.segs) {
                        const int n = sg.op == 'C' ? 3 : (sg.op == 'Z' ? 0 : 1);
                        for (int k = 0; k < n; ++k) {
                            const double qy = bl[li] - sg.p[2 * k + 1] * ch.E;
                            it = std::min(it, qy);
                            ib = std::max(ib, qy);
                        }
                    }
                }
            if (it < ib) { top = it; bottom = ib; }
        }
    }
    const int col = std::max(0, std::min(2, S.anchor % 3)), row = std::max(0, std::min(2, S.anchor / 3));
    const double ax = col * 0.5 * blockW, ay = top + row * 0.5 * (bottom - top);
    const double upx = H / 20.0;
    const double sc = std::max(0.0, S.scale);
    const double locx = S.locX * W, locy = (1.0 - S.locY) * H;
    out.emPx = S.points / 10.0 * sc * upx;
    out.bw = blockW * sc * upx;
    out.bh = (bottom - top) * sc * upx;
    out.bx = locx - col * 0.5 * out.bw;
    out.by = locy - row * 0.5 * out.bh;
    auto PX = [&](double qx) { return locx + sc * upx * (qx - ax); };
    auto PY = [&](double qy) { return locy + sc * upx * (qy - ay); };
    out.firstBase = PY(bl[0]);
    out.lastBase = PY(bl.back());
    out.pitch = sc * upx * (lineD.back() + lineA.back()) * ls;
    out.lines.resize(nl);
    out.words.resize(nl);
    out.chars.resize(nl);
    out.shape.lines.resize(nl);
    out.nCp = (int)cps.size();
    for (size_t li = 0; li < nl; ++li) {
        const double off = (blockW - width[li]) * (S.lineAlign == 1 ? 0.5 : (S.lineAlign == 2 ? 1.0 : 0.0));
        Rich::Ln& ln = out.lines[li];
        ln.base = PY(bl[li]);
        ln.top = PY(bl[li] - lineA[li]);
        ln.bot = PY(bl[li] + lineD[li]);
        ln.x0 = PX(off);
        ln.x1 = PX(off + width[li]);
        out.shape.lines[li].textIndex = lineFirst[li];
        // words: runs of characters that are not blanks
        int wi = -1;
        bool inWord = false;
        for (const Ch& ch : L[li]) {
            const bool blank = ch.cp == ' ';
            if (blank) { inWord = false; continue; }
            if (!inWord) {
                out.shape.lines[li].words.emplace_back();
                out.shape.lines[li].words.back().textIndex = ch.textIndex;
                out.words[li].emplace_back();
                out.words[li].back().x0 = PX(off + ch.x);
                out.chars[li].emplace_back();
                ++wi;
                inWord = true;
            }
            out.words[li][wi].x1 = PX(off + ch.x + ch.adv);
            RBox b;
            b.x0 = PX(off + ch.x);
            b.x1 = PX(off + ch.x + ch.adv);
            out.chars[li][wi].push_back(b);
            const int ci = (int)out.shape.lines[li].words[wi].chars.size();
            out.shape.lines[li].words[wi].chars.push_back(ch.textIndex);
            if (ch.g.segs.empty()) continue;
            RGlyph g;
            g.line = (int)li; g.word = wi; g.ch = ci;
            g.segs.reserve(ch.g.segs.size());
            for (const TextSeg& s : ch.g.segs) {
                TextSeg t;
                t.op = s.op;
                const int n = s.op == 'C' ? 3 : (s.op == 'Z' ? 0 : 1);
                for (int k = 0; k < n; ++k) {
                    t.p[2 * k] = (float)PX(off + ch.x + s.p[2 * k] * ch.E);
                    t.p[2 * k + 1] = (float)PY(bl[li] - s.p[2 * k + 1] * ch.E);
                }
                g.segs.push_back(t);
            }
            out.glyphs.push_back(std::move(g));
        }
    }
    return true;
}

// The state of no animation: every node as it is.
inline void identityState(const vfxta::Shape& sh, vfxta::State& st)
{
    st = vfxta::State();
    const size_t nl = sh.lines.size();
    st.line.assign(nl, vfxta::Sub());
    st.word.assign(nl, std::vector<vfxta::Sub>());
    st.chr.assign(nl, std::vector<std::vector<vfxta::Sub>>());
    for (size_t l = 0; l < nl; ++l) {
        st.word[l].assign(sh.lines[l].words.size(), vfxta::Sub());
        st.chr[l].resize(sh.lines[l].words.size());
        for (size_t w = 0; w < sh.lines[l].words.size(); ++w) st.chr[l][w].assign(sh.lines[l].words[w].chars.size(), vfxta::Sub());
    }
}

// The characters a state does not draw at all (opacity 0 of the character or of its word), by text index.
inline std::vector<char> hiddenChars(const Rich& R, const vfxta::State& st)
{
    std::vector<char> h((size_t)std::max(0, R.nCp), 0);
    for (size_t l = 0; l < R.shape.lines.size(); ++l)
        for (size_t w = 0; w < R.shape.lines[l].words.size(); ++w)
            for (size_t c = 0; c < R.shape.lines[l].words[w].chars.size(); ++c) {
                const int ti = R.shape.lines[l].words[w].chars[c];
                if (ti >= 0 && ti < (int)h.size() && st.word[l][w].opacity * st.chr[l][w][c].opacity <= 0.002) h[(size_t)ti] = 1;
            }
    return h;
}

// ------------------------------------------------------------------ transforms
struct Aff {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;      // x' = a x + c y + e, y' = b x + d y + f
};
inline Aff affMul(const Aff& M, const Aff& N)             // N first, then M
{
    Aff r;
    r.a = M.a * N.a + M.c * N.b;
    r.b = M.b * N.a + M.d * N.b;
    r.c = M.a * N.c + M.c * N.d;
    r.d = M.b * N.c + M.d * N.d;
    r.e = M.a * N.e + M.c * N.f + M.e;
    r.f = M.b * N.e + M.d * N.f + M.f;
    return r;
}
// A blur or glow amount is a fraction of the picture's width (across) or height (down) times this: the glow of Double Flash Glow spreads about twice as far as the plain
// fraction would (GUESS: the filters' reference scale is not read; fits the glow's fall-off in the capture and the blur of the line-by-line animations).
static const double kBlurRef = 2.0;
inline double safeNum(double v, double lim, double def)
{
    if (!(v == v)) return def;
    return std::max(-lim, std::min(lim, v));
}

// The pivot of a node: one of the nine places of its box, or the custom one (units from the box middle).
inline void pivotOf(const vfxta::Sub& s, double x0, double x1, double top, double base, double bot, double u, double& px, double& py)
{
    const int c = s.center;
    if (c >= 0 && c <= 8) {
        const int r = c / 3, k = c % 3;
        px = k == 0 ? x0 : (k == 1 ? 0.5 * (x0 + x1) : x1);
        py = r == 0 ? base : (r == 1 ? 0.5 * (top + bot) : top);
        return;
    }
    px = 0.5 * (x0 + x1) + safeNum(s.cust[0], 1e3, 0) * u;
    py = 0.5 * (top + bot) - safeNum(s.cust[1], 1e3, 0) * u;
}

// EditableMatrix: the offset (canvas units, y up), then about the pivot the rotation (clockwise), the scale and the shear.
inline Aff subAff(const vfxta::Sub& s, double px, double py, double u)
{
    Aff T1;
    T1.e = safeNum(s.off[0], 1e4, 0) * u;
    T1.f = -safeNum(s.off[1], 1e4, 0) * u;
    Aff Tp, Tn;
    Tp.e = px; Tp.f = py;
    Tn.e = -px; Tn.f = -py;
    const double rot = safeNum(s.rot, 1e3, 0);
    Aff R;
    R.a = rm::cos(rot); R.b = rm::sin(rot); R.c = -rm::sin(rot); R.d = rm::cos(rot);
    Aff Sc;
    Sc.a = safeNum(s.scale[0], 1e3, 1);
    Sc.d = safeNum(s.scale[1], 1e3, 1);
    Aff Sh;
    Sh.b = safeNum(s.shear[1], 1e3, 0);                // (the sign: a positive Shear.X leans the text to the right as it is seen; fits the Menace capture)
    Sh.c = safeNum(s.shear[0], 1e3, 0);
    return affMul(T1, affMul(Tp, affMul(R, affMul(Sc, affMul(Sh, Tn)))));
}

// ------------------------------------------------------------------ drawing
inline void drawGlyphInto(const Placed& P, const Style& S, double pen, Layer& L)
{
#ifdef _WIN32
    if (drawGdip(P, S, pen, L)) return;
#endif
    drawOwn(P, S, pen, L);
}

// Gaussian blur of the four channels of a layer (premultiplied bytes).
inline void blurLayer(Layer& L, double sigmaX, double sigmaY)
{
    if (!(sigmaX > 0.05 || sigmaY > 0.05)) return;
    const size_t N = (size_t)L.w * L.h;
    std::vector<float> pl(N);
    for (int k = 0; k < 4; ++k) {
        for (size_t i = 0; i < N; ++i) pl[i] = (float)L.px[i * 4 + k];
        builtin::cgGaussianBlurPlane(pl.data(), L.w, L.h, sigmaX, sigmaY);
        for (size_t i = 0; i < N; ++i) L.px[i * 4 + k] = (uint8_t)std::max(0, std::min(255, (int)std::floor(pl[i] + 0.5f)));
    }
}

// Lays 'src' (premultiplied, at (ox, oy) of the frame layer) over the frame layer at opacity op.
inline void overLayer(Layer& dst, const Layer& src, int ox, int oy, double op)
{
    const int o = (int)std::floor(std::max(0.0, std::min(1.0, op)) * 256.0 + 0.5);
    for (int y = 0; y < src.h; ++y) {
        const int fy = oy + y;
        if (fy < 0 || fy >= dst.h) continue;
        for (int x = 0; x < src.w; ++x) {
            const int fx = ox + x;
            if (fx < 0 || fx >= dst.w) continue;
            const uint8_t* s = &src.px[((size_t)y * src.w + x) * 4];
            if (!s[3]) continue;
            uint8_t* d = &dst.px[((size_t)fy * dst.w + fx) * 4];
            const int sa = (s[3] * o) >> 8;
            const int ia = 255 - sa;
            for (int k = 0; k < 3; ++k) d[k] = (uint8_t)std::min(255, ((s[k] * o) >> 8) + mul255(d[k], ia));
            d[3] = (uint8_t)std::min(255, sa + mul255(d[3], ia));
        }
    }
}

struct AnimOut {
    double opacity = 1;                 // of the whole text
};

// Draws the animated text into 'L' (reset to W x H). 'st' is the evaluated state of every node.
inline AnimOut drawRich(const Style& S, const Rich& R, const vfxta::State& st, int W, int H, Layer& L)
{
    AnimOut ao;
    L.reset(W, H);
    const double u = H / 20.0;                                // pixels per canvas unit of the picture
    const double sigmaK = 1.0;
    // the boxes of the nodes
    const vfxta::Sub& nodeS = st.node;
    double npx, npy;
    pivotOf(nodeS, R.bx, R.bx + R.bw, R.by, R.firstBase, R.by + R.bh, u, npx, npy);
    const Aff nodeA = subAff(nodeS, npx, npy, u);
    ao.opacity = safeNum(nodeS.opacity, 1.0, 1.0);
    if (ao.opacity < 0) ao.opacity = 0;
    std::vector<Aff> lineA(R.lines.size());
    for (size_t l = 0; l < R.lines.size(); ++l) {
        double px, py;
        pivotOf(st.line[l], R.lines[l].x0, R.lines[l].x1, R.lines[l].top, R.lines[l].base, R.lines[l].bot, u, px, py);
        lineA[l] = subAff(st.line[l], px, py, u);
    }
    for (const RGlyph& g : R.glyphs) {
        const int l = g.line, w = g.word, c = g.ch;
        const int ti = R.shape.lines[l].words[w].chars[c];
        {
            const double nn = (double)R.nCp, a = safeNum(nodeS.selL, 1e3, 0), b = safeNum(nodeS.selR, 1e3, 1);
            if (ti < (int)std::floor(a * nn + 1e-9) || ti >= (int)std::floor(b * nn + 1e-9)) continue;
        }
        const Rich::Ln& ln = R.lines[l];
        const RBox& wb = R.words[l][w];
        const RBox& cb = R.chars[l][w][c];
        const vfxta::Sub& ws = st.word[l][w];
        const vfxta::Sub& cs = st.chr[l][w][c];
        double px, py;
        pivotOf(ws, wb.x0, wb.x1, ln.top, ln.base, ln.bot, u, px, py);
        const Aff wa = subAff(ws, px, py, u);
        pivotOf(cs, cb.x0, cb.x1, ln.top, ln.base, ln.bot, u, px, py);
        const Aff ca = subAff(cs, px, py, u);
        const Aff M = affMul(nodeA, affMul(lineA[l], affMul(wa, ca)));
        double op = safeNum(st.line[l].opacity, 4, 1) * safeNum(ws.opacity, 4, 1) * safeNum(cs.opacity, 4, 1);
        op = std::max(0.0, std::min(1.0, op));
        if (op <= 0.0) continue;
        double hb = 0, vb = 0;
        if (st.line[l].blurOn) { hb += st.line[l].hblur; vb += st.line[l].vblur; }
        if (ws.blurOn) { hb += ws.hblur; vb += ws.vblur; }
        if (cs.blurOn) { hb += cs.hblur; vb += cs.vblur; }
        hb = std::max(0.0, safeNum(hb, 10, 0));
        vb = std::max(0.0, safeNum(vb, 10, 0));
        // the transformed outline
        Placed P;
        P.emPx = R.emPx;
        P.glyphs.emplace_back();
        std::vector<TextSeg>& out = P.glyphs.back();
        out.reserve(g.segs.size());
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        bool ok = true;
        for (const TextSeg& s : g.segs) {
            TextSeg t;
            t.op = s.op;
            const int n = s.op == 'C' ? 3 : (s.op == 'Z' ? 0 : 1);
            for (int k = 0; k < n; ++k) {
                const double x = s.p[2 * k], y = s.p[2 * k + 1];
                const double X = M.a * x + M.c * y + M.e, Y = M.b * x + M.d * y + M.f;
                if (!(std::fabs(X) < 1e6 && std::fabs(Y) < 1e6)) { ok = false; break; }
                t.p[2 * k] = (float)X; t.p[2 * k + 1] = (float)Y;
                x0 = std::min(x0, X); x1 = std::max(x1, X); y0 = std::min(y0, Y); y1 = std::max(y1, Y);
            }
            if (!ok) break;
            out.push_back(t);
        }
        if (!ok || out.empty()) continue;
        const double det = std::fabs(M.a * M.d - M.b * M.c);
        const double pen = S.outline > 0 ? S.outline / 100.0 * R.emPx * std::sqrt(det) : 0.0;
        const double bk = kBlurRef;
        const double sx = hb > 0 ? builtin::more::cgSigmaForDiameter(hb * bk * W) * sigmaK : 0.0;
        const double sy = vb > 0 ? builtin::more::cgSigmaForDiameter(vb * bk * H) * sigmaK : 0.0;
        const bool plain = op >= 0.999 && sx <= 0.05 && sy <= 0.05;
        if (plain) {
            if (x1 < -pen - 2 || y1 < -pen - 2 || x0 > W + pen + 2 || y0 > H + pen + 2) continue;
            drawGlyphInto(P, S, pen, L);
            continue;
        }
        const int mx = (int)std::ceil(3.0 * sx) + 2, my = (int)std::ceil(3.0 * sy) + 2;
        const double padv = pen + 2;
        const int lx0 = (int)std::max(-(double)mx, std::floor(x0 - padv) - mx), ly0 = (int)std::max(-(double)my, std::floor(y0 - padv) - my);
        const int lx1 = (int)std::min((double)W + mx, std::ceil(x1 + padv) + mx), ly1 = (int)std::min((double)H + my, std::ceil(y1 + padv) + my);
        if (lx1 <= lx0 || ly1 <= ly0) continue;
        Layer T;
        T.reset(lx1 - lx0, ly1 - ly0);
        for (TextSeg& t : out) {
            const int n = t.op == 'C' ? 3 : (t.op == 'Z' ? 0 : 1);
            for (int k = 0; k < n; ++k) { t.p[2 * k] -= (float)lx0; t.p[2 * k + 1] -= (float)ly0; }
        }
        drawGlyphInto(P, S, pen, T);
        blurLayer(T, sx, sy);
        overLayer(L, T, lx0, ly0, op);
    }
    return ao;
}

// The glow (FilterLibrary.Glow.Process): the layer's alpha is blurred (Gaussian, the diameter 'amount' x the width across and x the height down), run through a gradient
// from "nothing" (0) to the glow colour (1) and put under the layer (over it for an inner glow). 'argb': the colour as the item stores it (alpha first, straight).
// The gradient runs between all-zero and the colour, so a pixel of blurred alpha t is the colour times t with alpha t x the colour's alpha.
inline void glowLayer(Layer& L, double amount, const double argb[4], bool inner = false)
{
    const double gk = kBlurRef;
    if (!(amount > 0) || L.w <= 0 || L.h <= 0) return;
    amount = std::min(amount, 1.0);
    const size_t N = (size_t)L.w * L.h;
    std::vector<float> a(N);
    for (size_t i = 0; i < N; ++i) a[i] = (float)L.px[i * 4 + 3];
    builtin::cgGaussianBlurPlane(a.data(), L.w, L.h, builtin::more::cgSigmaForDiameter(amount * gk * L.w), builtin::more::cgSigmaForDiameter(amount * gk * L.h));
    const double ca = std::max(0.0, std::min(1.0, argb[0]));
    for (size_t i = 0; i < N; ++i) {
        const double t = std::max(0.0, std::min(255.0, std::floor((double)a[i] + 0.5))) / 255.0;
        if (t <= 0) continue;
        const double ga = t * ca;                              // the glow's alpha
        uint8_t* d = &L.px[i * 4];
        double g[3];
        for (int c = 0; c < 3; ++c) g[c] = t * std::max(0.0, std::min(1.0, argb[3 - c])) * ca;      // premultiplied (the gradient runs from all zero to the colour), bytes are b g r
        const double da = d[3] / 255.0;
        if (!inner) {
            for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::min(255.0, std::floor(d[c] + g[c] * (1.0 - da) * 255.0 + 0.5));
            d[3] = (uint8_t)std::min(255.0, std::floor(d[3] + ga * (1.0 - da) * 255.0 + 0.5));
        } else {
            for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::min(255.0, std::floor(g[c] * 255.0 + d[c] * (1.0 - ga) + 0.5));
            d[3] = (uint8_t)std::min(255.0, std::floor(ga * 255.0 + d[3] * (1.0 - ga) + 0.5));
        }
    }
}

}  // namespace vfxtt
}  // namespace ofx
}  // namespace reno
