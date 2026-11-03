// Credit Roll's text on Windows: VEGAS's text engine asks GDI for the font (lfHeight = -MulDiv(points, 96, 72) at its 480-line reference frame, weight 400 or
// 700, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS), takes the line height as (tmAscent + tmDescent) times the frame height / 480, and draws each glyph from
// GetGlyphOutlineW (GGO_GRAY8_BITMAP, the matrix scaling by the same factor). Elsewhere these return false and the caller uses Reno's own text rasteriser.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
namespace builtin {

struct CreditGdiRun {
    int w = 0, h = 0;               // the mask
    int ox = 0, oy = 0;             // where the text cell starts in it
    std::vector<uint8_t> a;         // coverage 0..255
    double adv = 0;                 // the width of the text (the glyphs' advances)
};
bool creditGdiHeight(const std::string& face, int points, bool bold, bool italic, double scale, double& height);
bool creditGdiRun(const std::string& text, const std::string& face, int points, bool bold, bool italic, double scale, CreditGdiRun& out);

}  // namespace builtin
}  // namespace ofx
}  // namespace reno
