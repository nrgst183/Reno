// Interlaced media: the reader reports the field order and deinterlaces (blend / interpolate) when asked.
//
//   interlace_test <out dir>
//
// Makes its test clips with the ffmpeg program (skipped, with a note, where there is none): a picture whose even
// lines (the upper field) are white and odd lines black, stored as interlaced (top field first) in a lossless
// FFV1 .mkv and as interlaced H.264, and the same picture stored as progressive.
#include "../../src/media/media.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

static int grey(const ImagePtr& img, int x, int y)     // y from the top; the green channel
{
    if (!img || x < 0 || y < 0 || x >= img->w || y >= img->h) return -1;
    return img->row(img->h - 1 - y)[x * 4 + 1];
}

// The lines' values down the middle column, as text.
static std::string column(const ImagePtr& img, int n)
{
    std::string s;
    for (int y = 0; y < n; ++y) s += strfmt("%d ", grey(img, img ? img->w / 2 : 0, y));
    return s;
}

static bool make(const std::string& path, const std::string& args)
{
    const std::string pattern = "color=black:s=320x240:d=1:r=30,format=gray,geq=lum='if(mod(Y\\,2)\\,0\\,255)',format=yuv444p";
    const std::string cmd = "ffmpeg -v error -y -f lavfi -i \"" + pattern + "\" " + args + " \"" + path + "\"";
    return std::system(cmd.c_str()) == 0 && fileExists(path);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: interlace_test <out dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    if (std::system("ffmpeg -version > /dev/null 2>&1") != 0) {
        std::printf("NOTE: no ffmpeg program here to make the interlaced clips; skipped\n");
        return 0;
    }
    struct Clip { const char* file; const char* args; bool interlaced; };
    const Clip clips[] = {
        {"il_ffv1.mkv", "-c:v ffv1 -field_order tt", true},
        {"il_h264.mp4", "-c:v libx264 -flags +ilme+ildct -x264-params tff=1 -qp 0", true},
        {"prog_ffv1.mkv", "-c:v ffv1 -field_order progressive", false},
    };
    for (const Clip& c : clips) {
        const std::string path = pathJoin(dir, c.file);
        if (!make(path, c.args)) {
            check(false, strfmt("%s can be made", c.file));
            continue;
        }
        std::string err;
        std::unique_ptr<MediaReader> r = openMedia(path, &err);
        if (!r) {
            check(false, strfmt("%s opens", c.file), err);
            continue;
        }
        const std::string n = c.file;
        check(r->info().fieldOrder == (c.interlaced ? "upper" : "none"), n + " reports its field order", r->info().fieldOrder);
        ImagePtr as = r->videoFrame(0.5, 320, 240);
        check(grey(as, 160, 10) > 245 && grey(as, 160, 11) < 10, n + " as it is: the lines alternate", column(as, 6));
        r->setDeinterlace(1);
        ImagePtr blend = r->videoFrame(0.5, 320, 240);
        if (c.interlaced) {
            check(std::abs(grey(blend, 160, 10) - 128) < 8 && std::abs(grey(blend, 160, 11) - 128) < 8, n + " blended: the two fields mix into grey", column(blend, 6));
        } else {
            check(grey(blend, 160, 10) > 245 && grey(blend, 160, 11) < 10, n + " is progressive: deinterlacing leaves it alone", column(blend, 6));
        }
        r->setDeinterlace(2);
        ImagePtr interp = r->videoFrame(0.5, 320, 240);
        if (c.interlaced)
            check(grey(interp, 160, 10) > 245 && grey(interp, 160, 11) > 245, n + " interpolated: the upper field's (white) lines fill the frame", column(interp, 6));
        ImagePtr small = r->videoFrame(0.5, 160, 120);
        if (c.interlaced)
            check(small && small->w == 160 && grey(small, 80, 30) > 245 && grey(small, 80, 31) > 245, n + " deinterlaced before it is scaled down", column(small, 6));
        r->setDeinterlace(0);
        ImagePtr again = r->videoFrame(0.5, 320, 240);
        check(grey(again, 160, 10) > 245 && grey(again, 160, 11) < 10, n + " back to as it is", column(again, 6));
    }
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
