// Engine tests for: optical flow, Slow Motion's rate and resample override, the Color Grading
// panel's extra settings (LUTs, RL wheels, utilities, HSL curves, pickers) and the track effects' "after compositing" flag.
//
//   man3_engine_test
#include "../../src/engine/engine.h"
#include "../../src/engine/builtinfx.h"
#include "../../src/engine/opticalflow.h"
#include <cstdio>
#include <cmath>
#include <fstream>

using namespace reno;

static bool near(double a, double b) { return std::fabs(a - b) < 0.02; }

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A float picture with a bright square at (x0, 20).
static ImagePtr square(int x0)
{
    ImagePtr img = Image::make(96, 48, PixelDepth::Byte, true);
    for (int y = 0; y < img->h; ++y) {
        uint8_t* d = img->row(y);
        for (int x = 0; x < img->w; ++x, d += 4) {
            const bool in = x >= x0 && x < x0 + 16 && y >= 16 && y < 32;
            d[0] = d[1] = d[2] = in ? 255 : 20;
            d[3] = 255;
        }
    }
    return img;
}
static double centreX(const Image& img)
{
    double sx = 0, n = 0;
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* d = img.row(y);
        for (int x = 0; x < img.w; ++x, d += 4)
            if (d[0] > 140) { sx += x; n += 1; }
    }
    return n ? sx / n : -1;
}

static void opticalFlow()
{
    ImagePtr a = square(10), b = square(34);
    ImagePtr out = Image::make(96, 48, PixelDepth::Byte, true);
    check(interpolateOpticalFlow(*out, *a, *b, 0.5f), "optical flow: runs");
    const double c = centreX(*out);
    // a plain blend would give two half-bright squares (centre 30 with the brightness half); the flow gives one square near x = 30
    check(std::fabs(c - (10 + 8 + 12)) < 4.5, "optical flow: the square is half way", strfmt("centre %.1f", c));
    ImagePtr o0 = Image::make(96, 48, PixelDepth::Byte, true), o1 = Image::make(96, 48, PixelDepth::Byte, true);
    interpolateOpticalFlow(*o0, *a, *b, 0.f);
    interpolateOpticalFlow(*o1, *a, *b, 1.f);
    check(std::fabs(centreX(*o0) - 18) < 1.5, "optical flow: t = 0 is the first frame", strfmt("%.1f", centreX(*o0)));
    check(std::fabs(centreX(*o1) - 42) < 1.5, "optical flow: t = 1 is the second frame", strfmt("%.1f", centreX(*o1)));
    const FlowField f = estimateFlow(*a, *b);
    double sum = 0;
    int n = 0;
    for (size_t i = 0; i < f.dx.size(); ++i) {
        const int bx = (int)i % f.gw, by = (int)i / f.gw;
        if (bx * f.step >= 12 && bx * f.step < 24 && by * f.step >= 16 && by * f.step < 32) { sum += f.dx[i]; ++n; }
    }
    check(n > 0 && std::fabs(sum / n - 24) < 6, "optical flow: the flow on the square is about 24 pixels", strfmt("%.1f", n ? sum / n : -1));
    ImagePtr small = Image::make(10, 10, PixelDepth::Byte, true);
    check(!interpolateOpticalFlow(*out, *a, *small, 0.5f), "optical flow: sizes that do not fit are refused");
}

static void slowMotion()
{
    Event e;
    e.start = 0;
    e.length = 10;
    check(eventFxRate(e) == 1.0 && eventFxResample(e).empty(), "slow motion: no effect, no change");
    FxSlot s;
    s.id = 1;
    s.kind = "ofx";
    s.pluginId = kSlowMotionId;
    e.fx.push_back(s);
    check(std::fabs(eventFxRate(e) - 0.5) < 1e-9, "slow motion: default rate 0.5");
    check(eventFxResample(e) == "optical", "slow motion: default quality medium follows the motion");
    Json v = Json::array();
    v.push(Json(0.25));
    Json st = Json::object();
    st["params"]["PlaybackRate"]["v"] = v;
    e.fx[0].state = st;
    check(std::fabs(eventFxRate(e) - 0.25) < 1e-9, "slow motion: the rate is read from the state");
    MediaItem m;
    m.info.hasVideo = true;
    m.info.duration = 100;
    check(std::fabs(eventMediaTime(e, &m, 4.0) - 1.0) < 1e-6, "slow motion: 4 s of the event is 1 s of the media", strfmt("%.4f", eventMediaTime(e, &m, 4.0)));
    e.fx[0].bypass = true;
    check(std::fabs(eventMediaTime(e, &m, 4.0) - 4.0) < 1e-6, "slow motion: bypassed it does nothing");
    // saved and loaded
    e.fx[0].bypass = false;
    e.fx[0].afterComposite = true;
    Project p;
    Track t;
    t.id = 5;
    t.type = TrackType::Video;
    t.fx.push_back(e.fx[0]);
    p.tracks.push_back(t);
    Project q;
    check(q.fromJson(p.toJson()) && !q.tracks.empty() && !q.tracks[0].fx.empty() && q.tracks[0].fx[0].afterComposite, "track effect after compositing: saved and loaded");
}

static void colorGrade()
{
    ColorGrade g;
    check(g.isIdentity(), "grade: default is identity");
    // presets
    check(lutPresets().size() >= 15, "luts: presets listed", strfmt("%zu", lutPresets().size()));
    int cam = 0, look = 0;
    for (const LutPreset& lp : lutPresets()) (lp.camera ? cam : look)++;
    check(cam >= 5 && look >= 8, "luts: camera and look lists", strfmt("%d %d", cam, look));
    // a look LUT changes the picture, strength 0 does not
    const LutPreset* lk = nullptr;
    for (const LutPreset& lp : lutPresets()) if (!lp.camera) { lk = &lp; break; }
    ColorGrade h;
    h.lookLut.source = "preset:" + lk->id;
    check(!h.isIdentity(), "grade: a look LUT is not identity");
    h.prepare();
    float c[3] = {0.8f, 0.4f, 0.2f}, c0[3] = {0.8f, 0.4f, 0.2f};
    h.apply(c);
    check(std::fabs(c[0] - c0[0]) + std::fabs(c[1] - c0[1]) + std::fabs(c[2] - c0[2]) > 0.01, "luts: a look changes a colour");
    h.lookLut.strength = 0;
    h.prepare();
    float c2[3] = {0.8f, 0.4f, 0.2f};
    h.apply(c2);
    check(std::fabs(c2[0] - c0[0]) + std::fabs(c2[1] - c0[1]) + std::fabs(c2[2] - c0[2]) < 1e-4, "luts: strength 0 changes nothing");
    // a .cube file
    const std::string path = "/tmp/man3_test.cube";
    {
        std::ofstream f(path);
        f << "TITLE \"swap\"\nLUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b) for (int gg = 0; gg < 2; ++gg) for (int r = 0; r < 2; ++r) f << gg << " " << r << " " << b << "\n";   // red <-> green swapped
    }
    std::string err;
    auto lut = readCubeLut(path, &err);
    check(lut != nullptr, "luts: .cube read", err);
    if (lut) {
        float v[3] = {1.f, 0.f, 0.f};
        lut->apply(v, false);
        check(near(v[0], 0) && near(v[1], 1), "luts: .cube applied (trilinear)", strfmt("%.2f %.2f %.2f", v[0], v[1], v[2]));
        float w[3] = {0.7f, 0.2f, 0.4f};
        lut->apply(w, true);
        check(std::fabs(w[0] - 0.2) < 0.02 && std::fabs(w[1] - 0.7) < 0.02, "luts: .cube applied (tetrahedral)", strfmt("%.2f %.2f %.2f", w[0], w[1], w[2]));
    }
    check(readCubeLut("/tmp/does-not-exist.cube", &err) == nullptr && !err.empty(), "luts: a missing file is an error");
    // utilities
    ColorGrade u;
    u.brightness = 0.2;
    u.prepare();
    float x[3] = {0.3f, 0.3f, 0.3f};
    u.apply(x);
    check(x[0] > 0.3f, "utilities: brightness raises", strfmt("%.3f", x[0]));
    ColorGrade lv;
    lv.blackIn = 0.25;
    lv.whiteIn = 0.75;
    lv.prepare();
    float lo[3] = {0.25f, 0.25f, 0.25f}, hi[3] = {0.75f, 0.75f, 0.75f};
    lv.apply(lo);
    lv.apply(hi);
    check(std::fabs(lo[0]) < 0.02 && std::fabs(hi[0] - 1) < 0.02, "utilities: levels map input black and white", strfmt("%.3f %.3f", lo[0], hi[0]));
    ColorGrade sat;
    sat.hslSaturation = 0;
    sat.prepare();
    float s[3] = {0.9f, 0.2f, 0.1f};
    sat.apply(s);
    check(std::fabs(s[0] - s[1]) < 0.02 && std::fabs(s[1] - s[2]) < 0.02, "utilities: HSL saturation 0 is grey", strfmt("%.3f %.3f %.3f", s[0], s[1], s[2]));
    // RL wheels: shadows do not touch highlights
    ColorGrade rl;
    rl.shadows.master = 0.5;
    rl.prepare();
    float sh[3] = {0.05f, 0.05f, 0.05f}, hl[3] = {0.95f, 0.95f, 0.95f};
    rl.apply(sh);
    rl.apply(hl);
    check(sh[0] > 0.06f && std::fabs(hl[0] - 0.95f) < 0.02f, "RL wheels: shadows lift the dark, leave the light", strfmt("%.3f %.3f", sh[0], hl[0]));
    // HSL curves: hue vs sat down to nothing at red
    ColorGrade hc;
    hc.hslCurve[ColorGrade::HueVsSat] = {{0.0, 0.0}, {0.1, 0.0}, {0.2, 0.5}, {0.9, 0.5}};
    check(!hc.isIdentity(), "HSL curves: a curve is not identity");
    hc.prepare();
    float red[3] = {0.9f, 0.1f, 0.1f}, blue[3] = {0.1f, 0.1f, 0.9f};
    hc.apply(red);
    hc.apply(blue);
    check(std::fabs(red[0] - red[1]) < 0.15 && blue[2] - blue[0] > 0.6, "HSL curves: hue vs sat removes red, keeps blue", strfmt("%.2f %.2f / %.2f %.2f", red[0], red[1], blue[0], blue[2]));
    // pickers
    ColorGrade pk;
    const float dark[3] = {0.1f, 0.12f, 0.2f};
    pk.setBlackPoint(dark);
    pk.prepare();
    float d2[3] = {0.1f, 0.12f, 0.2f};
    pk.apply(d2);
    check(d2[0] + d2[1] + d2[2] < 0.12f, "picker: black point makes the colour black", strfmt("%.3f %.3f %.3f", d2[0], d2[1], d2[2]));
    ColorGrade pw;
    const float bright[3] = {0.8f, 0.7f, 0.6f};
    pw.setWhitePoint(bright);
    pw.prepare();
    float b2[3] = {0.8f, 0.7f, 0.6f};
    pw.apply(b2);
    check(std::fabs(b2[0] - 1) < 0.05 && std::fabs(b2[1] - 1) < 0.05 && std::fabs(b2[2] - 1) < 0.05, "picker: white point makes the colour white", strfmt("%.3f %.3f %.3f", b2[0], b2[1], b2[2]));
    ColorGrade pn;
    const float warm[3] = {0.7f, 0.5f, 0.3f};
    pn.setNeutralTone(warm);
    pn.prepare();
    float n2[3] = {0.7f, 0.5f, 0.3f};
    pn.apply(n2);
    check(std::fabs(n2[0] - n2[2]) < std::fabs(0.7f - 0.3f) * 0.5, "picker: neutral tone pulls the colour towards grey", strfmt("%.3f %.3f %.3f", n2[0], n2[1], n2[2]));
    // JSON
    ColorGrade all;
    all.cameraLut.source = "preset:" + lutPresets()[0].id;
    all.cameraLut.tetrahedral = true;
    all.rlLow = 0.3;
    all.shadows.x = 0.2;
    all.blackIn = 0.1;
    all.hue = 0.25;
    all.hslCurve[ColorGrade::LumVsSat] = {{0.0, 0.5}, {0.5, 0.8}, {1.0, 0.5}};
    const ColorGrade back = ColorGrade::fromJson(all.toJson());
    check(back.cameraLut == all.cameraLut && back.rlLow == 0.3 && back.shadows.x == 0.2 && back.blackIn == 0.1 && back.hue == 0.25 && back.hslCurve[ColorGrade::LumVsSat].size() == 3,
          "grade: all the new settings survive JSON");
    check(ColorGrade().toJson().dump().find("cameraLut") == std::string::npos, "grade: a neutral grade writes none of the new keys");
}

int main()
{
    opticalFlow();
    slowMotion();
    colorGrade();
    std::printf("%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
