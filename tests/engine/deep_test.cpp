// Tests of decoding deep video (10 and 12 bit) at full precision for 32-bit projects (MediaReader::videoFrameInDepth,
// MediaPool, compositor). 8-bit projects and media must be unchanged.
//
//   deep_test <work folder>        (needs the ffmpeg program to make the 10-bit clip; skipped without it)
#include "../../src/engine/engine.h"
#include "../../src/engine/builtinfx.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <set>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : ".";
    const std::string clip = pathJoin(dir, "grad10.mkv"), clip8 = pathJoin(dir, "grad8.mkv");
    const std::string cmd = "ffmpeg -loglevel error -y -f lavfi -i color=c=black:s=1024x64:d=1:r=30 -vf \"format=yuv444p10le,geq=lum='64+X*876/1023':cb=512:cr=512\" "
                            "-c:v ffv1 -pix_fmt yuv444p10le \"" + clip + "\" && ffmpeg -loglevel error -y -i \"" + clip + "\" -c:v ffv1 -pix_fmt yuv444p \"" + clip8 + "\"";
    if (std::system(cmd.c_str()) != 0 || !fileExists(clip)) {
        std::printf("NOTE the ffmpeg program could not make the 10-bit clip: nothing checked\nSUMMARY: 0 passed, 0 failed\n");
        return 0;
    }
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(clip, &err);
    check(r && r->info().bitDepth == 10, "a 10-bit clip says it has 10 bits", r ? strfmt("%d", r->info().bitDepth) : err);
    if (!r) return 1;
    ImagePtr b = r->videoFrame(0.0, 1024, 64);
    ImagePtr f = r->videoFrameInDepth(0.0, 1024, 64, PixelDepth::Float);
    check(b && f && f->depth == PixelDepth::Float && f->w == 1024 && f->h == 64, "it decodes to float at the size asked for");
    if (!b || !f) return 1;
    std::set<int> steps8, stepsF;
    double worst = 0;
    for (int x = 0; x < 1024; ++x) {
        const uint8_t* pb = b->row(32) + x * 4;
        const float* pf = (const float*)f->row(32) + x * 4;
        steps8.insert(pb[1]);
        stepsF.insert((int)std::lround(pf[1] * 4095));
        worst = std::max(worst, std::fabs(pf[1] - pb[1] / 255.0));
    }
    check(steps8.size() <= 256 && stepsF.size() > 700, "the ramp keeps its 10-bit steps in float (the 8-bit picture has at most 256)",
          strfmt("%d steps in 8-bit, %d in float", (int)steps8.size(), (int)stepsF.size()));
    check(worst < 1.5 / 255.0, "and float and 8-bit agree to within a step", strfmt("%.5f", worst));
    check(std::fabs(((const float*)f->row(32))[0] - 0.0) < 0.002 && std::fabs(((const float*)f->row(32))[1023 * 4] - 1.0) < 0.005,
          "black is 0 and white 1 (video levels opened up)", strfmt("%.4f %.4f", ((const float*)f->row(32))[0], ((const float*)f->row(32))[1023 * 4]));
    std::unique_ptr<MediaReader> r8 = openMedia(clip8, &err);
    check(r8 && r8->info().bitDepth == 8, "an 8-bit clip says 8");
    if (r8) {
        ImagePtr f8 = r8->videoFrameInDepth(0.0, 1024, 64, PixelDepth::Float);
        std::set<int> s8;
        for (int x = 0; x < 1024; ++x) s8.insert((int)std::lround(((const float*)f8->row(32))[x * 4 + 1] * 4095));
        check(f8 && f8->depth == PixelDepth::Float && s8.size() <= 256, "and gives its 8-bit picture, in float");
    }

    // the compositor: a 32-bit project brightening the dark end 8 times keeps the steps apart; an 8-bit one cannot
    Project p;
    p.settings.width = 1024;
    p.settings.height = 64;
    MediaItem mi;
    mi.id = p.newId();
    mi.path = clip;
    mi.info = r->info();
    p.media.push_back(mi);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = mi.id;
    e.length = 1;
    t.events.push_back(e);
    p.tracks.push_back(t);
    FxSlot up = makeBuiltinSlot(kColorGradeId, p.newId());
    ColorGrade g;
    g.exposure = 3;
    up.state = g.toJson();
    p.videoOut.push_back(up);
    MediaPool pool;
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;
    auto distinctDark = [&](const Project& q) {
        ImagePtr out = video.render(q, 0.0, fo);
        std::set<int> s;
        for (int x = 0; x < 128; ++x) s.insert(out->row(32)[x * 4 + 1]);       // (the darkest eighth: brought up to full)
        return (int)s.size();
    };
    const int deep = distinctDark([&] { Project q = p; q.settings.pixelFormat = "32bit"; return q; }());
    const int shallow = distinctDark(p);
    check(deep > 100 && shallow < 40, "a 32-bit project keeps the 10-bit detail through an 8x brightening (8-bit: banding)",
          strfmt("%d levels in 32-bit, %d in 8-bit", deep, shallow));
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
