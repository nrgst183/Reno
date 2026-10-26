// Tests of the motion tracker (src/engine/tracker.cpp): a textured square moving over a textured background is
// followed to within a fraction of a pixel, also when moving fast or when the light changes; a vanished square is lost.
//
//   tracker_test
#include "../../src/engine/tracker.h"
#include <cstdio>
#include <cmath>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static float noise(int x, int y, int seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.f;
}

// A frame: a smooth textured background, and a 24x24 square of its own texture centred at (sx, sy), drawn with
// sub-pixel placement; 'gain' scales the brightness of everything.
static ImagePtr frame(double sx, double sy, double gain = 1.0, bool square = true)
{
    const int W = 320, H = 180;
    ImagePtr img = Image::make(W, H, PixelDepth::Byte, false);
    for (int y = 0; y < H; ++y) {
        uint8_t* d = img->row(H - 1 - y);
        for (int x = 0; x < W; ++x, d += 4) {
            double v = 0.3 + 0.25 * noise(x / 4, y / 4, 1) + 0.05 * noise(x, y, 2);
            if (square) {
                const double u = x - (sx - 12), w = y - (sy - 12);       // (in the square's own pixels)
                if (u >= 0 && u < 24 && w >= 0 && w < 24) {
                    const int iu = (int)std::floor(u), iw = (int)std::floor(w);
                    const double fu = u - iu, fw = w - iw;
                    auto tex = [](int a, int b) { return 0.2 + 0.7 * noise(a / 3, b / 3, 7); };
                    v = tex(iu, iw) * (1 - fu) * (1 - fw) + tex(iu + 1, iw) * fu * (1 - fw) + tex(iu, iw + 1) * (1 - fu) * fw + tex(iu + 1, iw + 1) * fu * fw;
                }
            }
            const int c = (int)std::lround(std::min(1.0, v * gain) * 255.0);
            d[0] = d[1] = d[2] = (uint8_t)c;
            d[3] = 255;
        }
    }
    return img;
}

int main()
{
    // grey pictures
    {
        ImagePtr f = frame(100, 80);
        double s = 0;
        GrayImage g = toGray(*f, 160, &s);
        check(g.w == 160 && g.h == 90 && std::fabs(s - 0.5) < 1e-9, "toGray makes a smaller grey picture", strfmt("%dx%d %.3f", g.w, g.h, s));
        GrayImage h = halfSize(g);
        check(h.w == 80 && h.h == 45, "halfSize halves it");
    }
    // a slow, smooth path
    {
        auto path = [](int t, double& x, double& y) {
            x = 60 + 2.3 * t;
            y = 70 + 1.1 * t + 12 * std::sin(t / 6.0);
        };
        double x, y;
        path(0, x, y);
        GrayImage g0 = toGray(*frame(x, y), 0);
        BoxTracker tr;
        check(tr.start(g0, x - 0.5, y - 0.5, 24, 24), "the tracker takes a box");
        double cx = x - 0.5, cy = y - 0.5, worst = 0, score = 0;
        bool all = true;
        for (int t = 1; t <= 60; ++t) {
            path(t, x, y);
            GrayImage g = toGray(*frame(x, y), 0);
            if (!tr.step(g, cx, cy, &score)) { all = false; break; }
            worst = std::max(worst, std::hypot(cx - (x - 0.5), cy - (y - 0.5)));
        }
        check(all && worst < 0.5, "it follows a square over 60 frames to within half a pixel", strfmt("worst %.3f px, last score %.3f", worst, score));
    }
    // fast moves (14 pixels a frame) and changing light
    {
        double x = 40, y = 40;
        BoxTracker tr;
        tr.start(toGray(*frame(x, y), 0), x - 0.5, y - 0.5, 24, 24);
        double cx = x - 0.5, cy = y - 0.5, worst = 0;
        bool all = true;
        for (int t = 1; t <= 15; ++t) {
            x += 14;
            y += (t % 2 ? 9 : -6);
            const double gain = 1.0 + 0.3 * std::sin(t / 3.0);
            if (!tr.step(toGray(*frame(x, y, gain), 0), cx, cy)) { all = false; break; }
            worst = std::max(worst, std::hypot(cx - (x - 0.5), cy - (y - 0.5)));
        }
        check(all && worst < 0.75, "fast moves (14 px a frame) with the light changing", strfmt("worst %.3f px", worst));
    }
    // gone: lost
    {
        BoxTracker tr;
        tr.start(toGray(*frame(100, 90), 0), 99.5, 89.5, 24, 24);
        double cx = 99.5, cy = 89.5, score = 0;
        const bool found = tr.step(toGray(*frame(0, 0, 1.0, false), 0), cx, cy, &score);
        check(!found && std::fabs(cx - 99.5) < 1e-9, "a square that is gone is lost (and the box stays put)", strfmt("score %.3f", score));
    }
    // nonsense
    {
        BoxTracker tr;
        GrayImage flat;
        flat.w = 50;
        flat.h = 50;
        flat.px.assign(2500, 0.5f);
        check(!tr.start(flat, 25, 25, 10, 10), "a flat box cannot be tracked");
        check(!tr.start(flat, -5, 25, 10, 10) && !tr.start(flat, 25, 25, 80, 80), "nor one outside the picture or bigger than it");
        double cx = 0, cy = 0;
        BoxTracker none;
        check(!none.step(flat, cx, cy), "a tracker that was never started finds nothing");
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
