// VEGAS's Titles & Text (the managed ScriptPortal.MediaSoftware.TextGen.* assemblies behind TitlesAndText.ofx), as read from their IL:
//
//  Canvas and units. The picture is 20 canvas units high (and 20 x width / height wide), the middle being the origin: one unit is height / 20 pixels. A font size of
//  fs half points (RTF \fsNN, i.e. NN/2 points) is an em of NN/20 units, so 48 point text is 2.4 units (a tenth of the height 4.8 times); Scale multiplies everything,
//  Location is where the anchor point of the text block goes (x from the left, y from the BOTTOM, fractions of the picture).
//  Layout (WPF's TextFormatter, so ideal design-unit metrics, not hinted): each character advances by its glyph advance + kerning (em units) + Tracking (canvas units,
//  for every character, the last one included); a line is the font's line spacing (ascent + descent + line gap) high with its baseline the ascent below the line's top;
//  line i's baseline lies i x line height x LineSpacing below the first one (the lines' positions, not their gaps, are scaled). The block is the widest line wide (lines
//  aligned left / middle / right inside it by the RTF \ql \qc \qr) and from the first line's top to the last one's bottom; the anchor point (nine choices) is a point of that block,
//  and Scale grows the text about it.
//  Drawing (System.Drawing / GDI+ over a 32bpp premultiplied bitmap): the glyph outlines (WPF's GetGlyphOutline, unhinted design outlines, as cubic Beziers) are filled with the text
//  colour (SmoothingMode AntiAlias, 8 x 4 samples, so coverage comes in 32nds) and then stroked with a pen of OutlineWidth / 100 em, miter limit 2, centred on the outline.
//  Colours are cut to bytes by truncation (c x 255).
//  Shadow (FilterLibrary.DropShadow, an image effect of the text layer): the layer's alpha is blurred (CoreGraphics.Native's byte Gaussian, diameter = ShadowBlur / 10 x width), turned into the
//  premultiplied shadow colour and put UNDER the layer, offset by (ShadowOffsetX, ShadowOffsetY) / 10 x width (both across the WIDTH) to the right and down.
// On Windows the glyph outlines come from GDI and the drawing from GDI+, as in VEGAS; elsewhere from the application's font source (Qt) and Reno's own 8 x 4 scan converter.
#pragma once
#include "builtins.h"
#include "builtins_cgblur.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace reno {
namespace ofx {
namespace builtin {
namespace more {
double cgSigmaForDiameter(double px);   // builtins_velvet.cpp
}
}
namespace vfxtt {
// where the scan converter takes pixel centres: 0.5 puts them on whole coordinates (what CoreGraphics's native drawing did for the masks), 0 on the half
inline double& centreShift() { static double v = 0.5; return v; }

// ------------------------------------------------------------------ the text and its numbers
struct Style {
    std::string text;                   // UTF-8, lines divided by '\n'
    std::string font = "Verdana";
    bool bold = false, italic = false;
    double points = 48;
    int lineAlign = 0;                  // 0 left, 1 middle, 2 right
    double scale = 1, tracking = 0, lineSpacing = 1;
    int anchor = 4;                     // 0..8: row (top, middle, bottom) x 3 + column (left, middle, right)
    double locX = 0.5, locY = 0.5;      // fractions of the picture, y from the bottom
    double outline = 0;                 // OutlineWidth (hundredths of an em)
    uint8_t fill[4] = {255, 255, 255, 255}, stroke[4] = {255, 255, 255, 255};   // r g b a
};

// A move of the whole block (the animations): about its middle scaled by (sx, sy) then shifted by (dx, dy) pixels.
struct Move {
    double dx = 0, dy = 0, sx = 1, sy = 1;
};

inline void utf8Decode(const std::string& s, std::vector<uint32_t>& out)
{
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        uint32_t cp = c;
        int n = 0;
        if (c >= 0xF0) { cp = c & 7; n = 3; }
        else if (c >= 0xE0) { cp = c & 15; n = 2; }
        else if (c >= 0xC0) { cp = c & 31; n = 1; }
        ++i;
        for (; n > 0 && i < s.size(); --n, ++i) cp = (cp << 6) | ((unsigned char)s[i] & 63);
        out.push_back(cp);
    }
}

// ------------------------------------------------------------------ faces
#ifdef _WIN32
// A font as GDI gives it at 2048 logical units to the em: the design outlines (GGO_NATIVE | GGO_UNHINTED), advances, kerning pairs (the 'kern' table) and hhea's line metrics,
// which is what WPF's GlyphTypeface reads.
class GdiFace : public TextFace {
public:
    GdiFace(const std::string& family, bool bold, bool italic)
    {
        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) return;
        LOGFONTW lf = {};
        lf.lfHeight = -2048;
        lf.lfWeight = bold ? 700 : 400;
        lf.lfItalic = italic ? 1 : 0;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfOutPrecision = OUT_TT_ONLY_PRECIS;
        lf.lfQuality = ANTIALIASED_QUALITY;
        std::wstring w;
        for (size_t i = 0; i < family.size();) {         // UTF-8 to UTF-16
            std::vector<uint32_t> cps;
            utf8Decode(family.substr(i), cps);
            for (uint32_t cp : cps) {
                if (cp >= 0x10000) { cp -= 0x10000; w += (wchar_t)(0xD800 + (cp >> 10)); w += (wchar_t)(0xDC00 + (cp & 0x3FF)); }
                else w += (wchar_t)cp;
            }
            break;
        }
        for (size_t i = 0; i < w.size() && i < 31; ++i) lf.lfFaceName[i] = w[i];
        font_ = CreateFontIndirectW(&lf);
        if (!font_) return;
        old_ = SelectObject(dc_, font_);
        double upem = 2048;
        unsigned char head[54] = {};
        if (GetFontData(dc_, 0x64616568u, 0, head, 54) == 54) {          // 'head'
            const int u = (head[18] << 8) | head[19];
            if (u > 0) upem = u;
        }
        unsigned char hhea[36] = {};
        if (GetFontData(dc_, 0x61656868u, 0, hhea, 36) == 36) {          // 'hhea'
            ascent = (short)((hhea[4] << 8) | hhea[5]) / upem;
            descent = -(short)((hhea[6] << 8) | hhea[7]) / upem;
            lineGap = (short)((hhea[8] << 8) | hhea[9]) / upem;
        } else {
            TEXTMETRICW tm = {};
            GetTextMetricsW(dc_, &tm);
            ascent = tm.tmAscent / 2048.0;
            descent = tm.tmDescent / 2048.0;
            lineGap = tm.tmExternalLeading / 2048.0;
        }
        ok_ = ascent + descent > 0;
        const DWORD n = GetKerningPairsW(dc_, 0, nullptr);
        if (n > 0 && n < 200000) {
            std::vector<KERNINGPAIR> kp(n);
            GetKerningPairsW(dc_, n, kp.data());
            for (const KERNINGPAIR& k : kp) kern_[((uint32_t)k.wFirst << 16) | k.wSecond] = k.iKernAmount / 2048.0;
        }
    }
    ~GdiFace() override
    {
        if (dc_ && old_) SelectObject(dc_, old_);
        if (font_) DeleteObject(font_);
        if (dc_) DeleteDC(dc_);
    }
    bool ok() const { return ok_; }
    bool glyph(uint32_t cp, TextGlyph& g) override
    {
        std::lock_guard<std::mutex> lk(m_);
        g = TextGlyph();
        if (!ok_ || cp > 0xFFFF) return ok_;
        GLYPHMETRICS gm = {};
        MAT2 id = {};
        id.eM11.value = 1;
        id.eM22.value = 1;
        const DWORD size = GetGlyphOutlineW(dc_, cp, GGO_NATIVE | GGO_UNHINTED, &gm, 0, nullptr, &id);
        if (size == GDI_ERROR) return false;
        g.adv = gm.gmCellIncX / 2048.0;
        if (size == 0) return true;
        std::vector<unsigned char> raw(size);
        if (GetGlyphOutlineW(dc_, cp, GGO_NATIVE | GGO_UNHINTED, &gm, size, raw.data(), &id) == GDI_ERROR) return false;
        auto fx = [](const POINTFX& p, float& x, float& y) {
            x = (float)((p.x.value + p.x.fract / 65536.0) / 2048.0);
            y = (float)((p.y.value + p.y.fract / 65536.0) / 2048.0);
        };
        size_t at = 0;
        while (at + sizeof(TTPOLYGONHEADER) <= raw.size()) {
            const TTPOLYGONHEADER* h = (const TTPOLYGONHEADER*)&raw[at];
            const size_t end = std::min(raw.size(), at + h->cb);
            if (h->cb < sizeof(TTPOLYGONHEADER)) break;
            TextSeg s;
            s.op = 'M';
            fx(h->pfxStart, s.p[0], s.p[1]);
            g.segs.push_back(s);
            float cx = s.p[0], cy = s.p[1];
            const float sx = cx, sy = cy;
            size_t q = at + sizeof(TTPOLYGONHEADER);
            while (q + 4 <= end) {
                const TTPOLYCURVE* c = (const TTPOLYCURVE*)&raw[q];
                const POINTFX* pts = c->apfx;
                const int cnt = c->cpfx;
                if (q + 4 + (size_t)cnt * sizeof(POINTFX) > end) break;
                if (c->wType == TT_PRIM_LINE) {
                    for (int i = 0; i < cnt; ++i) {
                        TextSeg l;
                        l.op = 'L';
                        fx(pts[i], l.p[0], l.p[1]);
                        g.segs.push_back(l);
                        cx = l.p[0]; cy = l.p[1];
                    }
                } else if (c->wType == TT_PRIM_QSPLINE) {
                    for (int i = 0; i < cnt - 1; ++i) {         // off-curve points with the on-curve midpoints between them implied
                        float qx, qy, ex, ey;
                        fx(pts[i], qx, qy);
                        if (i == cnt - 2) fx(pts[i + 1], ex, ey);
                        else {
                            float nx, ny;
                            fx(pts[i + 1], nx, ny);
                            ex = (qx + nx) * 0.5f; ey = (qy + ny) * 0.5f;
                        }
                        TextSeg b;                              // quadratic to cubic
                        b.op = 'C';
                        b.p[0] = cx + 2.f / 3.f * (qx - cx); b.p[1] = cy + 2.f / 3.f * (qy - cy);
                        b.p[2] = ex + 2.f / 3.f * (qx - ex); b.p[3] = ey + 2.f / 3.f * (qy - ey);
                        b.p[4] = ex; b.p[5] = ey;
                        g.segs.push_back(b);
                        cx = ex; cy = ey;
                    }
                } else if (c->wType == TT_PRIM_CSPLINE) {
                    for (int i = 0; i + 2 < cnt; i += 3) {
                        TextSeg b;
                        b.op = 'C';
                        fx(pts[i], b.p[0], b.p[1]);
                        fx(pts[i + 1], b.p[2], b.p[3]);
                        fx(pts[i + 2], b.p[4], b.p[5]);
                        g.segs.push_back(b);
                        cx = b.p[4]; cy = b.p[5];
                    }
                }
                q += 4 + (size_t)cnt * sizeof(POINTFX);
            }
            if (cx != sx || cy != sy) {
                TextSeg l;
                l.op = 'L';
                l.p[0] = sx; l.p[1] = sy;
                g.segs.push_back(l);
            }
            TextSeg z;
            z.op = 'Z';
            g.segs.push_back(z);
            at = end;
        }
        return true;
    }
    double kern(uint32_t a, uint32_t b) override
    {
        if (a > 0xFFFF || b > 0xFFFF) return 0;
        auto it = kern_.find((a << 16) | b);
        return it == kern_.end() ? 0 : it->second;
    }

private:
    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ old_ = nullptr;
    bool ok_ = false;
    std::mutex m_;
    std::map<uint32_t, double> kern_;
};
#endif

// The face for a family and style: GDI on Windows, the application's font source elsewhere. Faces are kept (a font takes a while to open).
inline std::shared_ptr<TextFace> face(const std::string& family, bool bold, bool italic)
{
    static std::mutex m;
    static std::map<std::string, std::shared_ptr<TextFace>> cache;
    const std::string key = family + (bold ? "|b" : "|r") + (italic ? "i" : "r");
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    std::shared_ptr<TextFace> f;
#ifdef _WIN32
    {
        auto g = std::make_shared<GdiFace>(family, bold, italic);
        if (g->ok()) f = g;
    }
#else
    f = textFaceFromSource(family, bold, italic);
#endif
    if (cache.size() > 64) cache.clear();
    cache[key] = f;
    return f;
}

// ------------------------------------------------------------------ layout
struct Placed {
    // The glyphs' outlines in pixels (y down), one vector of segments per glyph, and the block's rectangle (pixels, top left) before any move.
    std::vector<std::vector<TextSeg>> glyphs;
    double bx = 0, by = 0, bw = 0, bh = 0;
    double emPx = 0;                    // one em in pixels (with Scale)
};

inline bool place(TextFace& F, const Style& S, int W, int H, const Move& mv, Placed& out)
{
    out = Placed();
    std::vector<std::string> lines;
    {
        size_t pos = 0;
        while (true) {
            const size_t e = S.text.find('\n', pos);
            lines.push_back(S.text.substr(pos, e == std::string::npos ? std::string::npos : e - pos));
            if (e == std::string::npos) break;
            pos = e + 1;
        }
    }
    const double E = S.points / 10.0;                           // the em in canvas units (before Scale)
    const double lineH = (F.ascent + F.descent + F.lineGap) * E;
    const double base = F.ascent * E;
    struct Ch { uint32_t cp; double x; TextGlyph g; };
    std::vector<std::vector<Ch>> L(lines.size());
    std::vector<double> width(lines.size(), 0.0);
    std::map<uint32_t, TextGlyph> cache;
    for (size_t li = 0; li < lines.size(); ++li) {
        std::vector<uint32_t> cps;
        utf8Decode(lines[li], cps);
        double x = 0;
        for (size_t i = 0; i < cps.size(); ++i) {
            const uint32_t cp = cps[i] == '\t' ? ' ' : cps[i];
            if (cp == '\r') continue;
            auto it = cache.find(cp);
            if (it == cache.end()) {
                TextGlyph g;
                if (!F.glyph(cp, g)) g = TextGlyph();
                it = cache.emplace(cp, g).first;
            }
            Ch ch;
            ch.cp = cp;
            ch.x = x;
            ch.g = it->second;
            L[li].push_back(ch);
            double kern = 0;
            if (i + 1 < cps.size()) kern = F.kern(cp, cps[i + 1] == '\t' ? ' ' : cps[i + 1]);
            x += (ch.g.adv + kern) * E + S.tracking;
        }
        // The line runs from its leftmost to its rightmost letter box (the last letter's tracking is not part of it, and letters laid backwards by a negative tracking stay inside).
        double lo = 0, hi = 0;
        for (const Ch& ch : L[li]) {
            lo = std::min(lo, ch.x);
            hi = std::max(hi, ch.x + ch.g.adv * E);
        }
        for (Ch& ch : L[li]) ch.x -= lo;
        width[li] = hi - lo;
    }
    double blockW = 0;
    for (double w : width) blockW = std::max(blockW, w);
    if (lines.size() == 1) blockW = width[0];
    const double ls = S.lineSpacing;
    // baselines (canvas units below the first baseline)
    std::vector<double> bl(lines.size(), 0.0);
    for (size_t i = 1; i < lines.size(); ++i) bl[i] = (double)i * lineH * ls;
    const double top = -base, bottom = bl.back() + (lineH - base);
    const int col = std::max(0, std::min(2, S.anchor % 3)), row = std::max(0, std::min(2, S.anchor / 3));
    const double ax = col * 0.5 * blockW, ay = top + row * 0.5 * (bottom - top);
    // canvas units to pixels
    const double upx = H / 20.0;
    const double sc = std::max(0.0, S.scale);
    const double locx = S.locX * W, locy = (1.0 - S.locY) * H;  // the anchor's pixel
    out.emPx = E * sc * upx;
    out.bw = blockW * sc * upx;
    out.bh = (bottom - top) * sc * upx;
    out.bx = locx - col * 0.5 * out.bw;
    out.by = locy - row * 0.5 * out.bh;
    const double cx = out.bx + out.bw * 0.5, cy = out.by + out.bh * 0.5;
    auto toPx = [&](double qx, double qy, float& px, float& py) {
        double x = locx + sc * upx * (qx - ax), y = locy + sc * upx * (qy - ay);
        x = cx + mv.sx * (x - cx) + mv.dx;
        y = cy + mv.sy * (y - cy) + mv.dy;
        px = (float)x;
        py = (float)y;
    };
    for (size_t li = 0; li < lines.size(); ++li) {
        const double off = (blockW - width[li]) * (S.lineAlign == 1 ? 0.5 : (S.lineAlign == 2 ? 1.0 : 0.0));
        for (const Ch& ch : L[li]) {
            if (ch.g.segs.empty()) continue;
            std::vector<TextSeg> segs;
            segs.reserve(ch.g.segs.size());
            for (const TextSeg& s : ch.g.segs) {
                TextSeg t;
                t.op = s.op;
                const int n = s.op == 'C' ? 3 : (s.op == 'Z' ? 0 : 1);
                for (int k = 0; k < n; ++k) toPx(off + ch.x + s.p[2 * k] * E, bl[li] - s.p[2 * k + 1] * E, t.p[2 * k], t.p[2 * k + 1]);
                segs.push_back(t);
            }
            out.glyphs.push_back(std::move(segs));
        }
    }
    return true;
}

// ------------------------------------------------------------------ drawing
// A premultiplied 8-bit layer, rows top first, bytes b g r a (GDI+'s 32bppPARGB).
struct Layer {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    void reset(int W, int H) { w = W; h = H; px.assign((size_t)W * H * 4, 0); }
};

inline int mul255(int a, int b)
{
    const int t = a * b + 128;
    return (t + (t >> 8)) >> 8;
}

// src over dst for premultiplied bytes at coverage cov (0..255) with the colour's own alpha.
inline void blendPixel(uint8_t* d /* b g r a */, const uint8_t* rgba /* r g b a */, int cov)
{
    const int sa = mul255(cov, rgba[3]);
    if (sa <= 0) return;
    const int ia = 255 - sa;
    for (int k = 0; k < 3; ++k) d[k] = (uint8_t)std::min(255, mul255(rgba[2 - k], sa) + mul255(d[k], ia));
    d[3] = (uint8_t)std::min(255, sa + mul255(d[3], ia));
}

#ifdef _WIN32
// GDI+ through its flat API (loaded when first needed, so the program does not link to it).
struct Gdip {
    typedef int (WINAPI* StartupFn)(ULONG_PTR*, const void*, void*);
    typedef int (WINAPI* Bmp0Fn)(int, int, int, int, BYTE*, void**);
    typedef int (WINAPI* GfxFn)(void*, void**);
    typedef int (WINAPI* SetIntFn)(void*, int);
    typedef int (WINAPI* PathNewFn)(int, void**);
    typedef int (WINAPI* PathFn)(void*);
    typedef int (WINAPI* Line4Fn)(void*, float, float, float, float);
    typedef int (WINAPI* Bez8Fn)(void*, float, float, float, float, float, float, float, float);
    typedef int (WINAPI* SolidFn)(DWORD, void**);
    typedef int (WINAPI* FillFn)(void*, void*, void*);
    typedef int (WINAPI* DelFn)(void*);
    typedef int (WINAPI* PenNewFn)(DWORD, float, int, void**);
    typedef int (WINAPI* PenMiterFn)(void*, float);
    StartupFn startup = nullptr;
    Bmp0Fn bmp0 = nullptr;
    GfxFn gfx = nullptr;
    SetIntFn smoothing = nullptr;
    PathNewFn pathNew = nullptr;
    PathFn pathClose = nullptr, pathDel = nullptr;
    Line4Fn addLine = nullptr;
    Bez8Fn addBezier = nullptr;
    SolidFn solid = nullptr;
    FillFn fillPath = nullptr, drawPath = nullptr;
    DelFn delBrush = nullptr, delPen = nullptr, delGfx = nullptr, delImage = nullptr;
    PenNewFn penNew = nullptr;
    PenMiterFn penMiter = nullptr;
    bool ready = false;
    static Gdip& get()
    {
        static Gdip g = [] {
            Gdip x;
            HMODULE m = LoadLibraryW(L"gdiplus.dll");
            if (!m) return x;
            auto P = [&](const char* n) { return (void*)GetProcAddress(m, n); };
            x.startup = (StartupFn)P("GdiplusStartup");
            x.bmp0 = (Bmp0Fn)P("GdipCreateBitmapFromScan0");
            x.gfx = (GfxFn)P("GdipGetImageGraphicsContext");
            x.smoothing = (SetIntFn)P("GdipSetSmoothingMode");
            x.pathNew = (PathNewFn)P("GdipCreatePath");
            x.pathClose = (PathFn)P("GdipClosePathFigure");
            x.pathDel = (PathFn)P("GdipDeletePath");
            x.addLine = (Line4Fn)P("GdipAddPathLine");
            x.addBezier = (Bez8Fn)P("GdipAddPathBezier");
            x.solid = (SolidFn)P("GdipCreateSolidFill");
            x.fillPath = (FillFn)P("GdipFillPath");
            x.drawPath = (FillFn)P("GdipDrawPath");
            x.delBrush = (DelFn)P("GdipDeleteBrush");
            x.delPen = (DelFn)P("GdipDeletePen");
            x.delGfx = (DelFn)P("GdipDeleteGraphics");
            x.delImage = (DelFn)P("GdipDisposeImage");
            x.penNew = (PenNewFn)P("GdipCreatePen1");
            x.penMiter = (PenMiterFn)P("GdipSetPenMiterLimit");
            if (!(x.startup && x.bmp0 && x.gfx && x.smoothing && x.pathNew && x.pathClose && x.pathDel && x.addLine && x.addBezier && x.solid && x.fillPath && x.drawPath &&
                  x.delBrush && x.delPen && x.delGfx && x.delImage && x.penNew && x.penMiter)) return x;
            struct { UINT32 version; void* cb; BOOL noThread; BOOL noCodecs; } in = {1, nullptr, FALSE, FALSE};
            ULONG_PTR token = 0;
            if (x.startup(&token, &in, nullptr) == 0) x.ready = true;
            return x;
        }();
        return g;
    }
};

// Fills then strokes every glyph into the layer with GDI+, as VEGAS's MetaTextCharacterNode.DrawGlyph does (fill first, the stroke over it).
inline bool drawGdip(const Placed& P, const Style& S, double penPx, Layer& L)
{
    Gdip& G = Gdip::get();
    if (!G.ready || L.w <= 0 || L.h <= 0) return false;
    void* bmp = nullptr;
    // PixelFormat32bppPARGB = 0x000E200B; the bytes are b g r a in memory, which is how the layer is handed in and out
    if (G.bmp0(L.w, L.h, L.w * 4, 0x000E200B, (BYTE*)L.px.data(), &bmp) != 0 || !bmp) return false;
    void* g = nullptr;
    if (G.gfx(bmp, &g) != 0 || !g) { G.delImage(bmp); return false; }
    G.smoothing(g, 4);                                          // AntiAlias
    void* brush = nullptr;
    G.solid(((DWORD)S.fill[3] << 24) | ((DWORD)S.fill[0] << 16) | ((DWORD)S.fill[1] << 8) | S.fill[2], &brush);
    void* pen = nullptr;
    if (penPx > 0 && S.stroke[3] > 0) {
        G.penNew(((DWORD)S.stroke[3] << 24) | ((DWORD)S.stroke[0] << 16) | ((DWORD)S.stroke[1] << 8) | S.stroke[2], (float)penPx, 0, &pen);
        if (pen) G.penMiter(pen, 2.0f);
    }
    for (const auto& gl : P.glyphs) {
        void* path = nullptr;
        if (G.pathNew(1, &path) != 0 || !path) continue;       // FillMode.Winding
        float cx = 0, cy = 0;
        for (const TextSeg& s : gl) {
            if (s.op == 'M') { cx = s.p[0]; cy = s.p[1]; }
            else if (s.op == 'L') { G.addLine(path, cx, cy, s.p[0], s.p[1]); cx = s.p[0]; cy = s.p[1]; }
            else if (s.op == 'C') { G.addBezier(path, cx, cy, s.p[0], s.p[1], s.p[2], s.p[3], s.p[4], s.p[5]); cx = s.p[4]; cy = s.p[5]; }
            else G.pathClose(path);
        }
        if (brush && S.fill[3] > 0) G.fillPath(g, brush, path);
        if (pen) G.drawPath(g, pen, path);
        G.pathDel(path);
    }
    if (pen) G.delPen(pen);
    if (brush) G.delBrush(brush);
    G.delGfx(g);
    G.delImage(bmp);
    return true;
}
#endif

// ---- Reno's own scan converter: nonzero winding, 8 x 4 samples per pixel (GDI+'s AntiAlias grid), pixel centres on whole coordinates plus a half.
struct Edge { double x0, y0, x1, y1; };

inline void flatten(const std::vector<TextSeg>& gl, std::vector<std::vector<std::pair<double, double>>>& polys)
{
    std::vector<std::pair<double, double>> cur;
    double cx = 0, cy = 0;
    for (const TextSeg& s : gl) {
        if (s.op == 'M') {
            if (cur.size() > 1) polys.push_back(cur);
            cur.clear();
            cx = s.p[0]; cy = s.p[1];
            cur.push_back({cx, cy});
        } else if (s.op == 'L') {
            cx = s.p[0]; cy = s.p[1];
            cur.push_back({cx, cy});
        } else if (s.op == 'C') {
            const double d = std::fabs(s.p[0] - cx) + std::fabs(s.p[1] - cy) + std::fabs(s.p[2] - s.p[0]) + std::fabs(s.p[3] - s.p[1]) + std::fabs(s.p[4] - s.p[2]) + std::fabs(s.p[5] - s.p[3]);
            const int n = std::max(2, std::min(32, (int)std::ceil(std::sqrt(d) * 1.2)));
            for (int i = 1; i <= n; ++i) {
                const double t = (double)i / n, u = 1 - t;
                const double a = u * u * u, b = 3 * u * u * t, c = 3 * u * t * t, e = t * t * t;
                cur.push_back({a * cx + b * s.p[0] + c * s.p[2] + e * s.p[4], a * cy + b * s.p[1] + c * s.p[3] + e * s.p[5]});
            }
            cx = s.p[4]; cy = s.p[5];
        } else {
            if (cur.size() > 1) polys.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() > 1) polys.push_back(cur);
}

// Adds the polygons' coverage (counts of 32 samples) into 'cnt' (w x h).
inline void scanFill(const std::vector<std::vector<std::pair<double, double>>>& polys, int w, int h, std::vector<uint8_t>& cnt)
{
    std::vector<Edge> E;
    double ymin = 1e30, ymax = -1e30;
    for (const auto& p : polys) {
        const size_t n = p.size();
        for (size_t i = 0; i < n; ++i) {
            const auto& a = p[i];
            const auto& b = p[(i + 1) % n];
            if (a.second == b.second) continue;
            E.push_back({a.first, a.second, b.first, b.second});
            ymin = std::min(ymin, std::min(a.second, b.second));
            ymax = std::max(ymax, std::max(a.second, b.second));
        }
    }
    if (E.empty()) return;
    const int y0 = std::max(0, (int)std::floor(ymin - 0.5)), y1 = std::min(h - 1, (int)std::ceil(ymax + 0.5));
    std::vector<std::pair<double, int>> hit;
    for (int y = y0; y <= y1; ++y) {
        for (int sy = 0; sy < 4; ++sy) {
            const double yy = y + (sy + 0.5) / 4.0 - centreShift();
            hit.clear();
            for (const Edge& e : E) {
                const double lo = std::min(e.y0, e.y1), hi = std::max(e.y0, e.y1);
                if (yy < lo || yy >= hi) continue;
                hit.push_back({e.x0 + (yy - e.y0) * (e.x1 - e.x0) / (e.y1 - e.y0), e.y1 > e.y0 ? 1 : -1});
            }
            if (hit.size() < 2) continue;
            std::sort(hit.begin(), hit.end());
            int wind = 0;
            for (size_t i = 0; i + 1 < hit.size(); ++i) {
                wind += hit[i].second;
                if (wind == 0) continue;
                // samples at (j + 0.5) / 8 - 0.5 inside [hit[i].x, hit[i+1].x)
                const double xa = hit[i].first, xb = hit[i + 1].first;
                int j0 = (int)std::ceil((xa + centreShift()) * 8.0 - 0.5), j1 = (int)std::ceil((xb + centreShift()) * 8.0 - 0.5);
                j0 = std::max(j0, 0);
                j1 = std::min(j1, w * 8);
                for (int j = j0; j < j1; ++j) {
                    uint8_t& c = cnt[(size_t)y * w + (j >> 3)];
                    if (c < 32) ++c;
                }
            }
        }
    }
}

// The stroke of a closed polyline as polygons for the scan converter: a quad for every segment and a miter (limit 2, else bevel) at every corner.
inline void strokePolys(const std::vector<std::pair<double, double>>& p, double width, std::vector<std::vector<std::pair<double, double>>>& out)
{
    const size_t n = p.size();
    if (n < 2 || width <= 0) return;
    const double hw = width * 0.5;
    auto orient = [](std::vector<std::pair<double, double>>& q) {
        double a = 0;
        for (size_t i = 0; i < q.size(); ++i) {
            const auto& u = q[i];
            const auto& v = q[(i + 1) % q.size()];
            a += u.first * v.second - v.first * u.second;
        }
        if (a < 0) std::reverse(q.begin(), q.end());
    };
    std::vector<std::pair<double, double>> pts;
    for (size_t i = 0; i < n; ++i)
        if (pts.empty() || std::fabs(p[i].first - pts.back().first) + std::fabs(p[i].second - pts.back().second) > 1e-9) pts.push_back(p[i]);
    while (pts.size() > 1 && std::fabs(pts.front().first - pts.back().first) + std::fabs(pts.front().second - pts.back().second) <= 1e-9) pts.pop_back();
    const size_t m = pts.size();
    if (m < 2) return;
    struct Dir { double x, y; };
    std::vector<Dir> dir(m);
    for (size_t i = 0; i < m; ++i) {
        const auto& a = pts[i];
        const auto& b = pts[(i + 1) % m];
        const double dx = b.first - a.first, dy = b.second - a.second, l = std::sqrt(dx * dx + dy * dy);
        dir[i] = {dx / l, dy / l};
    }
    for (size_t i = 0; i < m; ++i) {
        const auto& a = pts[i];
        const auto& b = pts[(i + 1) % m];
        const double nx = -dir[i].y * hw, ny = dir[i].x * hw;
        std::vector<std::pair<double, double>> q = {{a.first + nx, a.second + ny}, {b.first + nx, b.second + ny}, {b.first - nx, b.second - ny}, {a.first - nx, a.second - ny}};
        orient(q);
        out.push_back(q);
        // the corner at b between segment i and i + 1
        const Dir& d0 = dir[i];
        const Dir& d1 = dir[(i + 1) % m];
        const double cross = d0.x * d1.y - d0.y * d1.x, dot = d0.x * d1.x + d0.y * d1.y;
        if (std::fabs(cross) < 1e-9 && dot > 0) continue;
        const double side = cross > 0 ? -1.0 : 1.0;             // the outer side of the turn
        const std::pair<double, double> p0 = {b.first + side * -d0.y * hw, b.second + side * d0.x * hw};
        const std::pair<double, double> p1 = {b.first + side * -d1.y * hw, b.second + side * d1.x * hw};
        std::vector<std::pair<double, double>> c = {b, p0};
        // miter length / width = 1 / sin(half the angle between the segments); limit 2
        const double cosT = -dot;                               // between the outgoing directions' reverse
        const double sinHalf = std::sqrt(std::max(0.0, (1 - cosT) * 0.5));
        if (sinHalf > 1e-6 && 1.0 / sinHalf <= 2.0) {
            const double mx = (p0.first + p1.first) * 0.5 - b.first, my = (p0.second + p1.second) * 0.5 - b.second;
            const double ml = std::sqrt(mx * mx + my * my);
            if (ml > 1e-9) {
                const double k = hw / sinHalf / ml;
                c.push_back({b.first + mx * k, b.second + my * k});
            }
        }
        c.push_back(p1);
        orient(c);
        out.push_back(c);
    }
}

inline void drawOwn(const Placed& P, const Style& S, double penPx, Layer& L)
{
    const size_t N = (size_t)L.w * L.h;
    std::vector<uint8_t> cnt(N);
    for (const auto& gl : P.glyphs) {
        std::vector<std::vector<std::pair<double, double>>> polys;
        flatten(gl, polys);
        std::fill(cnt.begin(), cnt.end(), 0);
        if (S.fill[3] > 0) {
            scanFill(polys, L.w, L.h, cnt);
            for (size_t i = 0; i < N; ++i)
                if (cnt[i]) blendPixel(&L.px[i * 4], S.fill, (cnt[i] * 255 + 16) / 32);
        }
        if (penPx > 0 && S.stroke[3] > 0) {
            std::vector<std::vector<std::pair<double, double>>> sp;
            for (const auto& p : polys) strokePolys(p, penPx, sp);
            std::fill(cnt.begin(), cnt.end(), 0);
            scanFill(sp, L.w, L.h, cnt);
            for (size_t i = 0; i < N; ++i)
                if (cnt[i]) blendPixel(&L.px[i * 4], S.stroke, (cnt[i] * 255 + 16) / 32);
        }
    }
}

// Draws the text into a fresh layer (all zero where nothing is drawn).
inline bool draw(TextFace& F, const Style& S, int W, int H, const Move& mv, Layer& L, Placed* placedOut = nullptr)
{
    Placed P;
    if (W <= 0 || H <= 0 || !place(F, S, W, H, mv, P)) return false;
    L.reset(W, H);
    const double pen = S.outline > 0 ? S.outline / 100.0 * P.emPx * std::sqrt(std::fabs(mv.sx * mv.sy)) : 0.0;
#ifdef _WIN32
    if (drawGdip(P, S, pen, L)) { if (placedOut) *placedOut = P; return true; }
#endif
    drawOwn(P, S, pen, L);
    if (placedOut) *placedOut = P;
    return true;
}

// ------------------------------------------------------------------ the shadow (FilterLibrary.DropShadow)
// 'shadowRgba': the colour as floats 0..1 (r g b a, not premultiplied). Puts the blurred, offset shadow of the layer's alpha under it.
inline void dropShadow(Layer& L, const double shadowRgba[4], double amount, double dxUnits, double dyUnits)
{
    const int W = L.w, H = L.h;
    const size_t N = (size_t)W * H;
    std::vector<float> a(N);
    for (size_t i = 0; i < N; ++i) a[i] = (float)L.px[i * 4 + 3];
    const double diameter = amount * W;                         // unit type 0: fractions of the width
    const double sigma = diameter > 0 ? builtin::more::cgSigmaForDiameter(diameter) : 0.0;
    builtin::cgGaussianBlurPlane(a.data(), W, H, sigma, sigma);
    std::vector<uint8_t> ab(N);
    for (size_t i = 0; i < N; ++i) ab[i] = (uint8_t)builtin::cgBlurByte(a[i]);
    // premultiplied shadow colour ramp
    const double sa = std::max(0.0, std::min(1.0, shadowRgba[3]));
    float col[4];
    for (int k = 0; k < 3; ++k) col[k] = (float)(std::max(0.0, std::min(1.0, shadowRgba[k])) * sa);
    col[3] = (float)sa;
    const double ox = dxUnits * W, oy = dyUnits * W;
    auto shadowAt = [&](int x, int y, int k) -> double {
        // bilinear sample of the (offset) shadow picture at x - ox, y - oy
        const double fx = x - ox, fy = y - oy;
        const int ix = (int)std::floor(fx), iy = (int)std::floor(fy);
        const double tx = fx - ix, ty = fy - iy;
        auto at = [&](int xx, int yy) -> double { return xx < 0 || yy < 0 || xx >= W || yy >= H ? 0.0 : (double)ab[(size_t)yy * W + xx]; };
        const double v = (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
        return v / 255.0 * col[k == 3 ? 3 : 2 - k];
    };
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            uint8_t* d = &L.px[((size_t)y * W + x) * 4];
            const int ia = 255 - d[3];
            for (int k = 0; k < 4; ++k) {
                const int s = (int)std::floor(shadowAt(x, y, k) * 255.0 + 0.5);
                d[k] = (uint8_t)std::min(255, (int)d[k] + mul255(s, ia));
            }
        }
}

}  // namespace vfxtt
}  // namespace ofx
}  // namespace reno
