// Credit Roll maths, read from VEGAS's own code (sftrans1.dll CCred / DXSCredits: setup 0x1801f5250, scrolling draw 0x1801f5c90, timed draw 0x1801f60d0,
// line parser 0x1801e74b0, the in / out effect functions 0x1801f7530..0x1801f8a70). Pure arithmetic on floats and ints, nothing from Qt or GDI, so the
// tests can run it alone. What is not here: the glyph rasteriser (VEGAS's own text engine on GDI; builtins_gen.cpp draws with GDI on Windows and with Reno's
// text rasteriser elsewhere).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace vfxcredit {

// ---- the presets: the eight records the effect's constructor builds (0x180199100), straight from the DLL (colours as R, G, B, A)
struct StyleRec {
    const char* face;
    int size;               // points, at VEGAS's reference frame of 480 lines
    int bold, italic;
    float col[4];
    int align;              // 0 left, 1 right, 2 centre
    double before, after;   // the space before / after a line of this style, in frame heights
};
struct Preset {
    const char* name;
    int timed, back;
    double left, right, timedY, gap;
    int in, out, page;
    float bg[4];
    const char* leader;
    StyleRec st[5];         // 0 title, 1 sub-item, 2 left column, 3 leader, 4 right column
};
inline const Preset* presets(int& n)
{
    static const Preset P[] = {
    {"Plain Scrolling on Black", 0, 0, 0.1, 0.9, 0.3, 0.05, 0, 0, 0, {0.0f, 0.0f, 0.0f, 1.0f}, "",
     {{"Arial", 20, 1, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Arial", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Arial", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Arial", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Arial", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05}}},
    {"Scrolling on Transparent", 0, 0, 0.1, 0.9, 0.3, 0.05, 0, 0, 0, {0.0f, 0.0f, 0.0f, 0.0f}, ".",
     {{"Arial", 36, 1, 0, {1.0f, 0.0f, 0.0f, 1.0f}, 2, 0, 0.1},
      {"Verdana", 24, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Verdana", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 0, 0, 0.01},
      {"Verdana", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.01},
      {"Verdana", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 1, 0, 0.01}}},
    {"Timed, Fade on Black", 1, 0, 0.1, 0.9, 0.3, 0.05, 1, 1, 2, {0.0f, 0.0f, 0.0f, 1.0f}, ". ",
     {{"Arial", 36, 1, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.1},
      {"Arial", 24, 1, 0, {0.800000012f, 0.800000012f, 0.800000012f, 1.0f}, 2, 0, 0.05},
      {"Arial", 24, 0, 0, {0.699999988f, 0.699999988f, 0.699999988f, 1.0f}, 2, 0, 0.01},
      {"Arial", 24, 0, 0, {0.699999988f, 0.699999988f, 0.699999988f, 1.0f}, 2, 0, 0.01},
      {"Arial", 24, 0, 0, {0.699999988f, 0.699999988f, 0.699999988f, 1.0f}, 2, 0, 0.01}}},
    {"Timed, Zoom In/Out on Blue", 1, 0, 0.1, 0.9, 0.3, 0.05, 11, 11, 0, {0.0f, 0.0f, 1.0f, 0.5f}, ".",
     {{"Arial", 36, 1, 0, {0.800000012f, 0.800000012f, 1.0f, 1.0f}, 2, 0, 0.1},
      {"Verdana", 24, 1, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Times New Roman", 24, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 1, 0, 0.01},
      {"Times New Roman", 24, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.01},
      {"Times New Roman", 24, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 0, 0, 0.01}}},
    {"Scrolling, Left Side, on Semi-Black", 0, 0, 0.05, 0.65, 0.3, 0.05, 0, 0, 0, {0.0f, 0.0f, 0.0f, 0.5f}, ".",
     {{"Verdana", 36, 1, 0, {0.800000012f, 1.0f, 0.800000012f, 1.0f}, 2, 0, 0.1},
      {"Arial", 20, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.05},
      {"Arial", 16, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 1, 0, 0.01},
      {"Arial", 16, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.01},
      {"Arial", 16, 0, 0, {1.0f, 1.0f, 1.0f, 1.0f}, 0, 0, 0.01}}},
    {"Timed, Scroll Right, on Frosty White", 1, 0, 0.05, 0.95, 0.3, 0.05, 3, 4, 3, {1.0f, 1.0f, 1.0f, 0.850000024f}, "-",
     {{"Verdana", 48, 1, 1, {0.0f, 0.0f, 1.0f, 0.850000024f}, 2, 0, 0.1},
      {"Arial", 24, 0, 1, {0.0f, 0.0f, 0.0f, 0.850000024f}, 2, 0, 0.05},
      {"Arial", 24, 0, 0, {0.0f, 0.0f, 0.0f, 0.850000024f}, 1, 0, 0.01},
      {"Arial", 24, 0, 0, {0.0f, 0.0f, 0.0f, 0.850000024f}, 2, 0, 0.01},
      {"Arial", 24, 0, 0, {0.0f, 0.0f, 0.0f, 0.850000024f}, 0, 0, 0.01}}},
    {"Timed, Wipe In/Out, on Black", 1, 0, 0.1, 0.9, 0.1, 0.05, 13, 13, 0, {0.0f, 0.0f, 0.0f, 1.0f}, ".",
     {{"Arial", 42, 1, 1, {1.0f, 1.0f, 1.0f, 1.0f}, 2, 0, 0.1},
      {"Verdana", 24, 0, 1, {1.0f, 0.0f, 0.0f, 1.0f}, 2, 0, 0.05},
      {"Verdana", 20, 0, 0, {0.800000012f, 0.800000012f, 0.800000012f, 1.0f}, 0, 0, 0.01},
      {"Verdana", 20, 0, 0, {0.800000012f, 0.800000012f, 0.800000012f, 1.0f}, 2, 0, 0.01},
      {"Verdana", 20, 0, 0, {0.800000012f, 0.800000012f, 0.800000012f, 1.0f}, 1, 0, 0.01}}},
    {"Backward Scrolling, Primary Colors", 0, 1, 0.1, 0.9, 0.3, 0.05, 0, 0, 0, {0.0f, 0.0f, 0.0f, 0.0f}, ".",
     {{"Verdana", 36, 1, 0, {0.0f, 0.0f, 1.0f, 1.0f}, 2, 0, 0.1},
      {"Verdana", 24, 0, 0, {0.0f, 1.0f, 0.0f, 1.0f}, 2, 0, 0.01},
      {"Verdana", 20, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, 0, 0, 0.03},
      {"Verdana", 20, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, 2, 0, 0.03},
      {"Verdana", 20, 0, 0, {1.0f, 0.0f, 0.0f, 1.0f}, 1, 0, 0.03}}}
    };
    n = (int)(sizeof(P) / sizeof(P[0]));
    return P;
}
inline const char* defaultText() { return " Title Text\r\nSubitem Text\r\nItem Left\tItem Right\r\nLonger Item Left\tLonger Item Right\r\n"; }

// ---- the text: lines end at a line feed (VEGAS: CR LF); a leading space is a title, a tab splits two columns, a pair of quotes keeps spaces, ~ is a quote
struct Line {
    int type = 1;           // 0 title, 1 sub-item, 2 two columns
    std::string a, b;
};
inline std::string unquote(std::string s)
{
    if (s.size() >= 2 && s[0] == '"') s = s.substr(1, s.size() - 2);
    for (size_t k = 0; k < s.size(); ++k)
        if (s[k] == '~') s[k] = '"';
    return s;
}
inline std::vector<Line> parse(const std::string& text)
{
    std::vector<Line> out;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();     // (VEGAS's parser 0x1801e74b0 drops a last line with no line end; its credits editor always ends lines, Reno's text box may not, so the line is kept)
        std::string s = text.substr(pos, end - pos);
        pos = end + 1;
        if (!s.empty() && s.back() == '\r') s.pop_back();
        Line l;
        const size_t tab = s.find('\t');
        if (!s.empty() && s[0] == ' ') {
            l.type = 0;
            l.a = unquote(s.substr(1));
        } else if (tab != std::string::npos) {
            l.type = 2;
            l.a = unquote(s.substr(0, tab));
            size_t k = tab;
            while (k < s.size() && s[k] == '\t') ++k;
            l.b = unquote(s.substr(k));
        } else
            l.a = unquote(s);
        out.push_back(l);
    }
    return out;
}

// ---- pages (timed styles): the page of every line, and how many pages there are. mode 0: a title starts a page; 1: a page per line; 2: a title and the first
// item on the first page, then a page per item, the title above them shown with each (see fixTitle); 3: a title or a sub-item starts a page.
inline int pagesOf(const std::vector<Line>& L, int mode, std::vector<int>& pg)
{
    pg.assign(L.size(), 0);
    auto kind = [&](size_t i) { return i < L.size() ? L[i].type : -1; };
    int page = 0;
    bool lead = mode == 2 && kind(0) == 0;
    for (size_t i = 0; i < L.size(); ++i) {
        pg[i] = page;
        const int nx = kind(i + 1);
        if (mode == 0) {
            if (nx == 0) ++page;
        } else if (mode == 1) {
            if (nx != -1) ++page;
        } else if (mode == 2) {
            if (nx == 0) { ++page; lead = true; }
            else if (nx != -1) { if (!lead) ++page; lead = false; }
        } else if (mode == 3) {
            if (nx == 0 || nx == 1) ++page;
        }
    }
    return page + 1;
}
// mode 2: the title above the items of page 'cur' is shown with them
inline void fixTitle(const std::vector<Line>& L, std::vector<int>& pg, int cur)
{
    bool seen = false;
    for (int i = (int)L.size() - 1; i >= 0; --i) {
        if (pg[(size_t)i] == cur && L[(size_t)i].type != 0) seen = true;
        else if (seen && L[(size_t)i].type == 0) { pg[(size_t)i] = cur; break; }
    }
}

// ---- layout: the height of a line is its style's (text independent: GDI's ascent + descent at the font size, times the frame height / 480); two-column lines
// take the tallest of their three styles
inline int styleOfType(int type) { return type < 0 ? 0 : type > 2 ? 2 : type; }
inline double lineHeight(int type, const double h[5])
{
    return type == 0 ? h[0] : type == 1 ? h[1] : std::max(h[2], std::max(h[3], h[4]));
}
struct Placed {
    double y0 = 0, y1 = 0;
};
// y0 / y1 of every line of 'which' (all lines, or one page), the first starting at 'start'; returns where the block ends. 'tail': space after the last line too
inline double layout(const std::vector<Line>& L, const std::vector<int>* pg, int page, const double h[5], const double before[3], const double after[3], double H,
                     double start, bool tail, std::vector<Placed>& out)
{
    out.assign(L.size(), Placed());
    double y = start;
    bool first = true;
    size_t last = L.size();
    for (size_t i = 0; i < L.size(); ++i)
        if (!pg || (*pg)[i] == page) last = i;
    for (size_t i = 0; i < L.size(); ++i) {
        if (pg && (*pg)[i] != page) continue;
        const int s = styleOfType(L[i].type);
        if (!first) y += H * before[s];
        out[i].y0 = y;
        y += lineHeight(L[i].type, h);
        out[i].y1 = y;
        if (tail || i != last) y += H * after[s];
        first = false;
    }
    return y;
}

// ---- the in / out effects. An effect id (0 none, 1..10 the "pre" ones that move or fade the text, 11..15 the "post" ones that work on the finished page) and
// the part of the page time it takes (D); phase() gives how far into it we are and whether it is the in or the out
enum Fx { FxNone = 0, FxFade, FxLeft, FxRight, FxUp, FxDown, FxZoom, FxWipeHR, FxWipeH, FxWipeVR, FxWipeV };
inline int fxKind(int id)
{
    static const int K[16] = {FxNone, FxFade, FxFade, FxLeft, FxRight, FxUp, FxDown, FxLeft, FxRight, FxUp, FxDown, FxZoom, FxWipeHR, FxWipeH, FxWipeVR, FxWipeV};
    return id >= 0 && id < 16 ? K[id] : FxNone;
}
inline bool fxIsPost(int id) { return id >= 11 && id <= 15; }
inline double fxDuration(int id)
{
    static const double D[16] = {0, 0.25, 0.1, 0.25, 0.25, 0.25, 0.25, 0.5, 0.5, 0.5, 0.5, 0.25, 0.25, 0.25, 0.25, 0.25};
    return id >= 0 && id < 16 ? D[id] : 0.0;
}
inline bool phase(double D, double frac, double& t, bool& in)
{
    if (D <= 0) return false;
    if (D >= frac) { t = frac / D; in = true; return true; }
    if (D >= 1.0 - frac) { t = 1.0 - (1.0 - frac) / D; in = false; return true; }
    return false;
}

// colour mix of 0x1801f7530: A towards B by s, weighted by their alphas (straight colours); nothing left when both are nearly clear
inline void mixColour(const float A[4], const float B[4], double s, float out[4])
{
    const float wa = (float)((1.0 - s) * A[3]), wb = (float)(s * B[3]);
    const float tot = wb + wa;
    if (!(tot > 1e-5f)) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    out[0] = (wa * A[0] + wb * B[0]) / tot;
    out[1] = (wa * A[1] + wb * B[1]) / tot;
    out[2] = (wa * A[2] + wb * B[2]) / tot;
    out[3] = (float)(A[3] + (B[3] - A[3]) * (float)s);
}

struct Box {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;      // right / bottom excluded
    bool empty() const { return x1 <= x0 || y1 <= y0; }
    void clip(int W, int H) { x0 = std::max(0, x0); y0 = std::max(0, y0); x1 = std::min(W, x1); y1 = std::min(H, y1); }
    void add(int a, int b, int c, int d)
    {
        if (c <= a || d <= b) return;
        if (empty()) { x0 = a; y0 = b; x1 = c; y1 = d; return; }
        x0 = std::min(x0, a); y0 = std::min(y0, b); x1 = std::max(x1, c); y1 = std::max(y1, d);
    }
};

// The wipe of 0x1801f7ec0 (horizontal) and 0x1801f8400 (vertical) on the premultiplied float picture 'px' (W x H, 4 floats a pixel): a soft edge 5 % of the
// box wide moves across the box. clearFirst: the side the edge has passed (before it, from the start of the box) becomes the background; else the side ahead
// of it does.
inline void wipe(float* px, int W, int H, const Box& b, bool vertical, bool clearFirst, double t, const float bg[4])
{
    if (b.empty() || W <= 0 || H <= 0) return;
    const int lo = vertical ? b.y0 : b.x0, hi = vertical ? b.y1 : b.x1;
    const int len = hi - lo;
    const int hw = (int)((double)len * 0.05 * 0.5);
    const int cx = (int)((double)(len + 2 * hw) * t) + lo - hw;
    const int a0 = std::max(lo, cx - hw), a1 = std::min(hi, cx + hw);
    for (int y = std::max(0, b.y0); y < std::min(H, b.y1); ++y)
        for (int x = std::max(0, b.x0); x < std::min(W, b.x1); ++x) {
            const int u = vertical ? y : x;
            float* p = px + ((size_t)y * W + x) * 4;
            if (clearFirst) {
                if (u < a0) { for (int k = 0; k < 4; ++k) p[k] = bg[k]; continue; }
                if (u >= a1) continue;
            } else {
                if (u >= a1) { for (int k = 0; k < 4; ++k) p[k] = bg[k]; continue; }
                if (u < a0) continue;
            }
            int a = hw > 0 ? ((u - (cx - hw)) * 255) / (2 * hw) : 255;
            a = std::max(0, std::min(255, a));
            const float w = (clearFirst ? a : 255 - a) / 255.f;
            for (int k = 0; k < 4; ++k) p[k] = bg[k] + (p[k] - bg[k]) * w;
        }
}

// The weights of VEGAS's resampler (0x1802566d0, table built at its first call): a Catmull-Rom cubic of four taps for each of 16384 phases, as 14-bit integers
// (the float weight times 16384, cut towards zero). Taps are the pixels idx-1 .. idx+2 around the point; every statement is its own float operation so that no
// compiler fuses them (the cut at an exact integer must match).
struct CubicTable {
    int w[16384][4];
    CubicTable()
    {
        for (int i = 0; i < 16384; ++i) {
            volatile float x = (float)i * 6.1035156e-05f;
            float a = x + 1.0f, b = a * 0.5f;
            b = b - 2.5f; b = b * a;
            float w0 = -4.0f - b; w0 = w0 * a; w0 = w0 - (-2.0f);
            float c = x * 1.5f; c = c - 2.5f; c = c * x; c = c * x; float w1 = c + 1.0f;
            float y = 1.0f - x;
            float d = y * 1.5f; d = d - 2.5f; d = d * y; d = d * y; float w2 = d + 1.0f;
            float e = 2.0f - x, f = e * 0.5f;
            f = f - 2.5f; f = f * e;
            float w3 = -4.0f - f; w3 = w3 * e; w3 = w3 - (-2.0f);
            w[i][0] = (int)(w0 * 16384.0f); w[i][1] = (int)(w1 * 16384.0f); w[i][2] = (int)(w2 * 16384.0f); w[i][3] = (int)(w3 * 16384.0f);
        }
    }
};
inline const CubicTable& cubicTable() { static const CubicTable t; return t; }

// 0x1802566d0 on 4-byte pixels: the sw x sh part of 'src' (row stride 'ss' bytes) is brought down to dw x dh with the cubic above, first along the columns
// (rounded back to whole numbers, not clamped), then along the rows (rounded, clamped to 0..255). The point of output pixel i is (0x2000 + i * ((n << 14) / m))
// in 14-bit fixed point; the taps are the pixels at integer part -2 .. +1, a pixel outside is the nearest one.
inline void cubicShrink(uint8_t* dst, int dw, int dh, const uint8_t* src, int ss, int sw, int sh)
{
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    const CubicTable& T = cubicTable();
    const int stepX = (int)(((int64_t)sw << 14) / dw), stepY = (int)(((int64_t)sh << 14) / dh);
    auto clampi = [](int v, int n) { return v < 0 ? 0 : v >= n ? n - 1 : v; };
    int posY = 0x2000;
    std::vector<int> col(4);
    for (int y = 0; y < dh; ++y, posY += stepY) {
        const int* wy = T.w[posY & 0x3fff];
        const int iy = posY >> 14;
        const uint8_t* rows[4];
        for (int k = 0; k < 4; ++k) rows[k] = src + (size_t)clampi(iy - 2 + k, sh) * ss;
        int posX = 0x2000;
        for (int x = 0; x < dw; ++x, posX += stepX) {
            const int* wx = T.w[posX & 0x3fff];
            const int ix = posX >> 14;
            int v[4][4];                                   // [tap][channel]: the column filtered along the rows
            for (int k = 0; k < 4; ++k) {
                const int cx = clampi(ix - 2 + k, sw) * 4;
                for (int c = 0; c < 4; ++c) {
                    const int s = rows[0][cx + c] * wy[0] + rows[1][cx + c] * wy[1] + rows[2][cx + c] * wy[2] + rows[3][cx + c] * wy[3] + 0x2000;
                    v[k][c] = s / 16384;
                }
            }
            for (int c = 0; c < 4; ++c) {
                const int s = v[0][c] * wx[0] + v[1][c] * wx[1] + v[2][c] * wx[2] + v[3][c] * wx[3] + 0x2000;
                const int o = s / 16384;
                dst[((size_t)y * dw + x) * 4 + c] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o);
            }
        }
    }
}

// 0x18025f570: the 3x3 smoothing laid over the shrunk picture. Corners weigh A, edges B, the middle C, all whole numbers, divided by their sum (cut); the edge
// pixels use their own neighbours twice. A = int(z + 0.5), B = int(2z + 0.5), C = int((1 - z) * 20 + 0.5) + 4: a small z leaves the picture alone.
inline void smooth3(uint8_t* dst, const uint8_t* src, int w, int h, double z)
{
    const int A = (int)(z + 0.5), B = (int)((z + z) + 0.5), C = (int)((1.0 - z) * 20.0 + 0.5) + 4;
    const int tot = C + 4 * (A + B);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const int y0 = y > 0 ? y - 1 : y, y1 = y + 1 < h ? y + 1 : y, x0 = x > 0 ? x - 1 : x, x1 = x + 1 < w ? x + 1 : x;
            for (int c = 0; c < 4; ++c) {
                auto g = [&](int yy, int xx) { return (int)src[((size_t)yy * w + xx) * 4 + c]; };
                const int s = A * (g(y0, x0) + g(y0, x1) + g(y1, x0) + g(y1, x1)) + B * (g(y0, x) + g(y1, x) + g(y, x0) + g(y, x1)) + C * g(y, x);
                const int o = s / tot;
                dst[((size_t)y * w + x) * 4 + c] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o);
            }
        }
}

// The zoom of 0x1801f78e0: the box shrinks about its centre by z (0 whole, 1 gone), each side by int(size * z) / 2; what was in the box is brought down into what
// is left of it (cubicShrink, then smooth3), the rest of the box is the background. VEGAS works on the 8-bit picture: so does this, on the box.
inline void zoomBox(float* px, int W, int H, const Box& b, double z, const float bg[4])
{
    if (b.empty() || W <= 0 || H <= 0) return;
    const int bw = b.x1 - b.x0, bh = b.y1 - b.y0;
    const int hx = (int)((double)bw * z) / 2, hy = (int)((double)bh * z) / 2;
    const int ix0 = b.x0 + hx, ix1 = b.x1 - hx, iy0 = b.y0 + hy, iy1 = b.y1 - hy;
    const bool inner = ix1 > ix0 && iy1 > iy0;
    std::vector<uint8_t> src, tmp, out;
    int iw = 0, ih = 0;
    if (inner) {
        src.resize((size_t)bw * bh * 4);
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x)
                for (int k = 0; k < 4; ++k) {
                    const int fx = b.x0 + x, fy = b.y0 + y;
                    const float v = (fx < 0 || fy < 0 || fx >= W || fy >= H) ? bg[k] : px[((size_t)fy * W + fx) * 4 + k];
                    src[((size_t)y * bw + x) * 4 + k] = (uint8_t)std::max(0, std::min(255, (int)(v * 255.f + 0.5f)));
                }
        iw = ix1 - ix0; ih = iy1 - iy0;
        tmp.resize((size_t)iw * ih * 4); out.resize(tmp.size());
        cubicShrink(tmp.data(), iw, ih, src.data(), bw * 4, bw, bh);
        smooth3(out.data(), tmp.data(), iw, ih, z);
    }
    for (int y = std::max(0, b.y0); y < std::min(H, b.y1); ++y)
        for (int x = std::max(0, b.x0); x < std::min(W, b.x1); ++x) {
            float* p = px + ((size_t)y * W + x) * 4;
            if (!inner || x < ix0 || x >= ix1 || y < iy0 || y >= iy1) { for (int k = 0; k < 4; ++k) p[k] = bg[k]; continue; }
            const uint8_t* o = &out[((size_t)(y - iy0) * iw + (x - ix0)) * 4];
            for (int k = 0; k < 4; ++k) p[k] = o[k] / 255.f;
        }
}

}  // namespace vfxcredit
