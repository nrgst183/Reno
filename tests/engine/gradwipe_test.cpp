// Tests of the VEGAS Gradient Wipe's mask and mix (src/ofx/vfxgradwipe.h).
//
//   gradwipe_test
//
// The arithmetic is checked alone; the reading of VEGAS's gradient pictures runs when a vidpcore.dll is at hand (the file RENO_VIDPCORE_DLL names or an installed VEGAS)
// and is skipped otherwise.
#include "../../src/ofx/vfxgradwipe.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace reno::ofx;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s%s%s\n", name.c_str(), detail.empty() ? "" : "  ", detail.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static void be32(std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s)); }
static void chunk(std::vector<uint8_t>& png, const char* tag, const std::vector<uint8_t>& body)
{
    be32(png, (uint32_t)body.size());
    png.insert(png.end(), tag, tag + 4);
    png.insert(png.end(), body.begin(), body.end());
    be32(png, 0);   // (the checksum is not looked at)
}

int main()
{
    // a PNG of 3 x 2 RGBA with a stored (uncompressed) zlib block and the Sub filter on the second row
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}, ihdr;
        be32(ihdr, 3); be32(ihdr, 2);
        ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});
        chunk(png, "IHDR", ihdr);
        std::vector<uint8_t> raw = {0, 1, 10, 3, 4, 5, 20, 7, 8, 9, 30, 11, 12,
                                    1, 0, 100, 0, 0, 0, 5, 0, 0, 0, 5, 0, 0};
        std::vector<uint8_t> z = {0x78, 0x01, 0x01, (uint8_t)raw.size(), 0, (uint8_t)~raw.size(), 0xff};
        z.insert(z.end(), raw.begin(), raw.end());
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        vgw::Picture p;
        const bool ok = vgw::png::decode(png.data(), png.size(), p);
        check(ok && p.w == 3 && p.h == 2, "a stored PNG is read");
        check(ok && p.g[0] == 10 && p.g[1] == 20 && p.g[2] == 30 && p.g[3] == 100 && p.g[4] == 105 && p.g[5] == 110, "  the green bytes, the Sub filter undone");
    }
    // the mask table
    {
        uint8_t t[256];
        vgw::maskTable(0.0, 0.0, false, t);
        check(t[0] == 0 && t[1] == 255 && t[255] == 255, "mask at progress 0: all of the first picture except the darkest byte");
        vgw::maskTable(1.0, 0.0, false, t);
        { bool none = true; for (int i = 0; i < 256; ++i) none = none && t[i] == 0; check(none, "  and at progress 1 nothing of it"); }
        vgw::maskTable(0.5, 0.0, false, t);
        check(t[127] == 0 && t[128] == 0 && t[129] == 255, "  a hard edge at the progress");
        vgw::maskTable(0.5, 0.2, false, t);
        bool mono = true;
        for (int i = 1; i < 256; ++i) mono = mono && t[i] >= t[i - 1];
        check(mono && t[60] == 0 && t[200] == 255 && t[128] > 0 && t[128] < 255, "  a soft edge rises between the ends");
        uint8_t r[256];
        vgw::maskTable(0.5, 0.2, true, r);
        int worst = 0;
        for (int i = 0; i < 256; ++i) worst = std::max(worst, std::abs((int)r[i] + (int)t[i] - 255));
        check(worst <= 1, "  Direction Reverse is the mirror image of it", std::to_string(worst));
    }
    // the mix
    {
        check(vgw::weight14(255) == 16384 && vgw::weight14(0) == 0 && vgw::weight14(128) == 8224, "the weight of the first picture in 14 bits");
        check(vgw::mix8(200, 40, 16384) == 200 && vgw::mix8(200, 40, 0) == 40 && vgw::mix8(200, 40, 8192) == 120, "  and the mix of two bytes");
        check(vgw::foldIndex(0, 8, false, false) == 0 && vgw::foldIndex(7, 8, true, false) == 0 && vgw::foldIndex(4, 8, true, false) == 3 && vgw::foldIndex(1, 8, false, true) == 6 &&
                  vgw::foldIndex(7, 8, true, true) == 7,
              "Mirror folds about the middle, then Flip turns over");
    }
    // the stretch of a mask: means rounded
    {
        std::vector<uint8_t> m = {0, 0, 0, 255, 255, 0, 255, 255}, o;      // 4 x 2
        vgw::stretchMean(m, 4, 2, 0, 0, 4, 2, 2, 1, o);
        check(o.size() == 2 && o[0] == 64 && o[1] == 191, "a mask is stretched by the mean of the pixels it covers, rounded", std::to_string(o[0]) + " " + std::to_string(o[1]));
    }
    // VEGAS's pictures
    {
        const std::string dll = vgw::findDll({});
        if (dll.empty()) std::printf("SKIP  the pictures of VEGAS (no vidpcore.dll: set RENO_VIDPCORE_DLL)\n");
        else {
            int ok = 0;
            for (int i = 0; i < 28; ++i) {
                const auto p = vgw::loadPicture(dll, i);
                if (p && p->w >= 16 && p->h >= 16 && p->g.size() == (size_t)p->w * p->h) ++ok;
            }
            check(ok == 28, "all 28 gradient pictures are read from the DLL", std::to_string(ok));
            const auto lr = vgw::loadPicture(dll, 13);
            if (lr) {
                // Linear Left-Right: the picture grows from left to right
                const int y = lr->h / 2;
                check(lr->g[(size_t)y * lr->w + 2] < lr->g[(size_t)y * lr->w + lr->w - 3], "  Linear Left-Right rises from left to right");
                vgw::Settings s;
                s.keepAspect = false;
                s.progress = 0.5;
                std::vector<uint8_t> m;
                vgw::frameMask(*lr, s, 64, 36, m);
                check(m[(size_t)18 * 64 + 4] == 0 && m[(size_t)18 * 64 + 60] == 255, "  its mask at half way: the left has arrived, the right not");
            }
        }
    }
    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
