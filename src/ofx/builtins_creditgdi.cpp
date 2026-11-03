#include "creditgdi.h"
#include <algorithm>
#include <cmath>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace reno {
namespace ofx {
namespace builtin {

#ifdef _WIN32
namespace {

struct GdiFont {
    HDC dc = nullptr;
    HFONT font = nullptr;
    HGDIOBJ old = nullptr;
    GdiFont() {}
    GdiFont(const GdiFont&) = delete;
    GdiFont& operator=(const GdiFont&) = delete;
    ~GdiFont()
    {
        if (dc && old) SelectObject(dc, old);
        if (font) DeleteObject(font);
        if (dc) DeleteDC(dc);
    }
    bool open(const std::string& face, int points, bool bold, bool italic)
    {
        dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        LOGFONTW lf = {};
        int h = MulDiv(points, 96, 72);
        lf.lfHeight = h > 0 ? -h : -1;
        lf.lfWeight = bold ? 700 : 400;
        lf.lfItalic = italic ? 1 : 0;
        lf.lfCharSet = 1;
        lf.lfOutPrecision = 7;
        const int n = MultiByteToWideChar(CP_UTF8, 0, face.data(), (int)face.size(), nullptr, 0);
        std::wstring w((size_t)std::max(0, n), L'\0');
        if (n > 0) MultiByteToWideChar(CP_UTF8, 0, face.data(), (int)face.size(), &w[0], n);
        for (size_t i = 0; i + 1 < (size_t)LF_FACESIZE && i < w.size(); ++i) lf.lfFaceName[i] = w[i];
        font = CreateFontIndirectW(&lf);
        if (!font) return false;
        old = SelectObject(dc, font);
        return old != nullptr;
    }
};

}  // namespace
#endif

bool creditGdiHeight(const std::string& face, int points, bool bold, bool italic, double scale, double& height)
{
#ifdef _WIN32
    GdiFont f;
    if (!f.open(face, points, bold, italic)) return false;
    TEXTMETRICW tm = {};
    if (!GetTextMetricsW(f.dc, &tm)) return false;
    height = (double)(tm.tmAscent + tm.tmDescent) * scale;
    return true;
#else
    (void)face; (void)points; (void)bold; (void)italic; (void)scale; (void)height;
    return false;
#endif
}

bool creditGdiRun(const std::string& text, const std::string& face, int points, bool bold, bool italic, double scale, CreditGdiRun& out)
{
#ifdef _WIN32
    GdiFont f;
    if (!f.open(face, points, bold, italic)) return false;
    TEXTMETRICW tm = {};
    if (!GetTextMetricsW(f.dc, &tm)) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    std::wstring w((size_t)std::max(0, n), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &w[0], n);
    MAT2 m = {};
    {
        double ip = 0;
        const double fr = std::modf(scale, &ip);
        m.eM11.value = (short)ip;
        m.eM11.fract = (WORD)(int)(fr * 65536.0);
        m.eM22 = m.eM11;
    }
    struct G {
        int x, y, bw, bh;
        std::vector<unsigned char> raw;
        int stride;
    };
    std::vector<G> gl;
    int pen = 0;
    const int baseline = (int)std::lround((double)tm.tmAscent * scale);
    for (size_t i = 0; i < w.size(); ++i) {
        GLYPHMETRICS gm = {};
        const DWORD size = GetGlyphOutlineW(f.dc, w[i], GGO_GRAY8_BITMAP, &gm, 0, nullptr, &m);
        if (size == GDI_ERROR) continue;
        if (size > 0 && gm.gmBlackBoxX > 0 && gm.gmBlackBoxY > 0) {
            G g;
            g.raw.resize(size);
            if (GetGlyphOutlineW(f.dc, w[i], GGO_GRAY8_BITMAP, &gm, size, g.raw.data(), &m) != GDI_ERROR) {
                g.bw = (int)gm.gmBlackBoxX;
                g.bh = (int)gm.gmBlackBoxY;
                g.x = pen + gm.gmptGlyphOrigin.x;
                g.y = baseline - gm.gmptGlyphOrigin.y;
                g.stride = (g.bw + 3) & ~3;
                gl.push_back(std::move(g));
            }
        }
        pen += gm.gmCellIncX;
    }
    const int pad = 2;
    const double hgt = (double)(tm.tmAscent + tm.tmDescent) * scale;
    out.ox = out.oy = pad;
    out.adv = (double)pen;
    if (pen > 60000 || hgt > 8000 || (double)pen * hgt > 6e7) return false;       // (nonsense sizes: the caller falls back to its own rasteriser)
    out.w = std::max(1, pen) + 2 * pad;
    out.h = std::max(1, (int)std::lround(hgt)) + 2 * pad;
    out.a.assign((size_t)out.w * out.h, 0);
    for (size_t k = 0; k < gl.size(); ++k) {
        const G& g = gl[k];
        for (int y = 0; y < g.bh; ++y)
            for (int x = 0; x < g.bw; ++x) {
                const int dx = pad + g.x + x, dy = pad + g.y + y;
                if (dx < 0 || dy < 0 || dx >= out.w || dy >= out.h) continue;
                const size_t at = (size_t)y * g.stride + x;
                const int v = at < g.raw.size() ? (int)g.raw[at] * 4 : 0;
                unsigned char& d = out.a[(size_t)dy * out.w + dx];
                d = (unsigned char)std::min(255, std::max((int)d, v));
            }
    }
    return true;
#else
    (void)text; (void)face; (void)points; (void)bold; (void)italic; (void)scale; (void)out;
    return false;
#endif
}

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
