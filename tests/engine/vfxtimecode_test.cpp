// Timecode (vfxtimecode.h): the time strings of the twelve formats, the drop-frame count, and the layout of the box with the plain-size font model
// (compared with what VEGAS's own picture shows at 320 x 180: box columns 220..292, rows 151..162 at the defaults). Standalone: g++ -std=c++17 vfxtimecode_test.cpp
#include "../../src/ofx/vfxtimecode.h"
#include <cstdio>
using namespace reno::ofx::vfxtc;
static int fails = 0;
static void check(bool ok, const char* what, const std::string& got = "")
{
    if (!ok) { ++fails; std::printf("FAIL %s %s\n", what, got.c_str()); }
}
static std::string at(int fmt, long long frame, double fps = 30.0)
{
    return text(fmt, (long long)std::floor((double)frame * 1e7 / fps + 1e-6), fps);
}
int main()
{
    // frame 10 of 30 fps (what the capture shows)
    const char* want[12] = {"00:00:00.333", "0.333", "00:00:00.10", "10.000", "0+10", "0+10", "00:00:00:07", "00:00:00:08", "00:00:00:08", "00:00:00:09", "00:00:00;09", "00:00:00:10"};
    for (int f = 0; f < 12; ++f) check(at(f, 10) == want[f], "format at frame 10", at(f, 10));
    // drop frame: the numbers 0 and 1 are skipped at the start of every minute but the tenth; 1800 frames of 29.97 are a minute less 2 frames
    int tc[4];
    frameTimecode(2, 1800, tc);
    check(tc[1] == 1 && tc[2] == 0 && tc[3] == 2, "drop frame 1800 is 00:01:00;02");
    frameTimecode(2, 17982, tc);
    check(tc[1] == 10 && tc[2] == 0 && tc[3] == 0, "drop frame 17982 is 00:10:00;00");
    frameTimecode(1, 108000 + 30 * 61 + 7, tc);
    check(tc[0] == 1 && tc[1] == 1 && tc[2] == 1 && tc[3] == 7, "30 fps count");
    check(at(8, 1800 + 1800 / 30 * 0, 29.97).size() == 11, "drop string shape");
    // the plain-size layout at 320 x 180 (cell height 15), right and bottom: the box
    const Font f = modelFont(15, 1.0, 1.0);
    const Layout L = layout(f, "00:00:00;09", 320, 180, 2, 2);
    check(L.ok && L.width == 65 && L.H == 18 && L.D == 0 && L.maxOy == 9, "metrics of the plain font");
    check(L.ox + 1 == 220 && L.ox + L.wb - 1 == 292 && 180 - L.oy - L.hb == 151 && 180 - L.oy - 2 == 162, "box 220..292 x 151..162");
    const Layout Lt = layout(f, "00:00:00;09", 320, 180, 0, 0);
    check(Lt.ox + 1 == 29 && 180 - Lt.oy - 2 == 27, "left top: 29, bottom row 27");
    const Layout Lm = layout(f, "00:00:00;09", 320, 180, 1, 1);
    check(Lm.ox + 1 == 125 && 180 - Lm.oy - 2 == 94, "middle: 125, bottom row 94");
    // nonsense input must not crash
    const Font g = modelFont(0, NAN, -5.0);
    check(layout(g, std::string(300, '8'), 0, 0, 7, -3).ok == false, "empty frame");
    layout(g, "x", 1, 1, 9, 9);
    check(text(99, -5, 0).size() > 0, "bad format");
    std::printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
