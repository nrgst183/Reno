// Tests of Event Pan/Crop's mask in the engine (Event::mask, maskOn, maskApplyToFx; MaskKey in project.h): shapes,
// feathering, opacity, panning, keyframes, Apply to FX, and the project's JSON.
//
//   mask_test
#include "../../src/engine/engine.h"
#include "../../src/engine/builtinfx.h"
#include <cstdio>
#include <cmath>
#include <algorithm>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// a plain red 200 x 100 picture (BGRA)
static ImagePtr redPicture()
{
    ImagePtr img = Image::make(200, 100, PixelDepth::Byte, true);
    for (int y = 0; y < 100; ++y) {
        uint8_t* row = img->row(y);
        for (int x = 0; x < 200; ++x) { row[x * 4] = 20; row[x * 4 + 1] = 30; row[x * 4 + 2] = 200; row[x * 4 + 3] = 255; }
    }
    return img;
}

class StillReader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int, int) override { return redPicture(); }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static int redAt(const ImagePtr& img, int x, int y) { return img ? img->row(img->h - 1 - y)[x * 4 + 2] : -1; }

static MaskPath rect(double l, double t, double r, double b)
{
    MaskPath m;
    for (auto c : {std::make_pair(l, t), std::make_pair(r, t), std::make_pair(r, b), std::make_pair(l, b)}) {
        MaskPoint q;
        q.x = c.first;
        q.y = c.second;
        m.points.push_back(q);
    }
    return m;
}

int main()
{
    MediaPool pool;
    pool.setOpener([](const std::string& path, std::string*) {
        std::unique_ptr<StillReader> r(new StillReader());
        r->mi.path = path;
        r->mi.hasVideo = true;
        r->mi.isStill = true;
        r->mi.width = 200;
        r->mi.height = 100;
        return std::unique_ptr<MediaReader>(r.release());
    });
    FxRuntime fx;
    VideoEngine video(pool, fx);
    Project p;
    p.settings.width = 200;
    p.settings.height = 100;
    MediaItem m;
    m.id = p.newId();
    m.path = "still";
    m.info.hasVideo = true;
    m.info.isStill = true;
    m.info.width = 200;
    m.info.height = 100;
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.length = 4;
    t.events.push_back(e);
    p.tracks.push_back(t);
    FrameOptions fopt;
    fopt.interactive = false;
    Event& ev = p.tracks[0].events[0];
    auto frame = [&](double at) { return video.render(p, at, fopt); };

    ImagePtr f = frame(1.0);
    check(redAt(f, 5, 5) == 200 && redAt(f, 100, 50) == 200, "no mask: the whole picture");

    // a positive rectangle over the middle of the media (x 0.25..0.75, y 0.2..0.8)
    MaskKey k;
    k.paths.push_back(rect(0.25, 0.2, 0.75, 0.8));
    ev.mask.push_back(k);
    f = frame(1.0);
    check(redAt(f, 5, 5) == 200, "a mask whose switch is off does nothing");
    ev.maskOn = true;
    check(maskActive(ev), "maskActive with the switch on and a closed path");
    f = frame(1.0);
    check(redAt(f, 100, 50) == 200 && redAt(f, 5, 5) == 0 && redAt(f, 195, 50) == 0 && redAt(f, 100, 10) == 0,
          "a rectangle: inside shows, outside is cut away", strfmt("%d %d %d %d", redAt(f, 100, 50), redAt(f, 5, 5), redAt(f, 195, 50), redAt(f, 100, 10)));
    check(redAt(f, 52, 50) == 200 && redAt(f, 47, 50) == 0, "the rectangle's left edge at a quarter of the width",
          strfmt("%d %d", redAt(f, 52, 50), redAt(f, 47, 50)));
    // half-covered pixel at the edge (x 50.0 exactly is a pixel boundary; y 20 is too): anti-aliased edge at 0.255
    ev.mask[0].paths[0] = rect(0.2525, 0.2, 0.75, 0.8);
    f = frame(1.0);
    check(redAt(f, 50, 50) > 60 && redAt(f, 50, 50) < 140, "an edge across a pixel covers part of it (anti-aliased)", std::to_string(redAt(f, 50, 50)));
    ev.mask[0].paths[0].antiAlias = false;
    f = frame(1.0);
    check(redAt(f, 50, 50) == 0 || redAt(f, 50, 50) == 200, "without anti-aliasing a pixel is in or out", std::to_string(redAt(f, 50, 50)));
    ev.mask[0].paths[0] = rect(0.25, 0.2, 0.75, 0.8);

    // negative
    ev.mask[0].paths[0].negative = true;
    f = frame(1.0);
    check(redAt(f, 100, 50) == 0 && redAt(f, 5, 5) == 200, "a negative path shows what is outside it");
    ev.mask[0].paths[0].negative = false;

    // a second, negative path cuts a hole in the first
    ev.mask[0].paths.push_back(rect(0.45, 0.4, 0.55, 0.6));
    ev.mask[0].paths[1].negative = true;
    f = frame(1.0);
    check(redAt(f, 100, 50) == 0 && redAt(f, 70, 50) == 200 && redAt(f, 5, 5) == 0, "a negative path after a positive one cuts a hole in it");
    ev.mask[0].paths[1].enabled = false;
    f = frame(1.0);
    check(redAt(f, 100, 50) == 200, "a path switched off does nothing");
    ev.mask[0].paths.pop_back();

    // opacity
    ev.mask[0].paths[0].opacity = 0.5;
    f = frame(1.0);
    check(std::abs(redAt(f, 100, 50) - 100) <= 2, "a path at 50% opacity lets half through", std::to_string(redAt(f, 100, 50)));
    ev.mask[0].paths[0].opacity = 1.0;

    // an oval of Bezier curves (as the Oval tool makes it), centre (0.5, 0.5), radii 0.25 x 0.4
    {
        MaskPath o;
        const double cx = 0.5, cy = 0.5, rx = 0.25, ry = 0.4, kk = 0.5523;
        auto pt = [](double x, double y, double ix, double iy) { MaskPoint q; q.x = x; q.y = y; q.inX = ix; q.inY = iy; q.outX = -ix; q.outY = -iy; return q; };
        o.points = {pt(cx, cy - ry, -kk * rx, 0), pt(cx + rx, cy, 0, -kk * ry), pt(cx, cy + ry, kk * rx, 0), pt(cx - rx, cy, 0, kk * ry)};
        ev.mask[0].paths[0] = o;
        f = frame(1.0);
        // a corner of the oval's bounding box (0.27, 0.13) is outside the ellipse; a point on the ellipse's inside near
        // 45 degrees (0.5 + 0.25*0.65, 0.5 + 0.4*0.65) is inside
        check(redAt(f, 100, 50) == 200 && redAt(f, 54, 13) == 0 && redAt(f, 132, 76) == 200, "an oval: curved, not its bounding box",
              strfmt("%d %d %d", redAt(f, 100, 50), redAt(f, 54, 13), redAt(f, 132, 76)));
        ev.mask[0].paths[0] = rect(0.25, 0.2, 0.75, 0.8);
    }

    // feather: Out keeps the inside whole and fades outside; In fades inside; None is hard
    ev.mask[0].paths[0].feather = 20;          // 0.2 * 0.25 * 100 = 5 pixels
    ev.mask[0].paths[0].featherType = 2;
    f = frame(1.0);
    const int outA = redAt(f, 48, 50), inA = redAt(f, 52, 50), farOut = redAt(f, 40, 50);
    check(inA == 200 && outA > 20 && outA < 200 && farOut == 0, "feather Out: soft outside the edge, the inside whole", strfmt("%d %d %d", inA, outA, farOut));
    ev.mask[0].paths[0].featherType = 1;
    f = frame(1.0);
    check(redAt(f, 48, 50) == 0 && redAt(f, 52, 50) > 0 && redAt(f, 52, 50) < 200 && redAt(f, 100, 50) == 200, "feather In: soft inside the edge",
          strfmt("%d %d", redAt(f, 48, 50), redAt(f, 52, 50)));
    ev.mask[0].paths[0].featherType = 3;
    f = frame(1.0);
    check(redAt(f, 48, 50) > 0 && redAt(f, 52, 50) < 200, "feather Both: soft on both sides", strfmt("%d %d", redAt(f, 48, 50), redAt(f, 52, 50)));
    ev.mask[0].paths[0].featherType = 0;
    f = frame(1.0);
    check(redAt(f, 48, 50) == 0 && redAt(f, 52, 50) == 200, "feather None: hard");
    ev.mask[0].paths[0].feather = 0;
    ev.mask[0].paths[0].featherType = 2;

    // the mask stays on the media when the event is zoomed in 2x on its middle (Pan/Crop box half the size)
    PanCropKey pc;
    pc.w = 0.5;
    pc.h = 0.5;
    ev.panCrop.push_back(pc);
    ev.mask[0].paths[0] = rect(0.4, 0.4, 0.6, 0.6);           // in the frame: x 0.3..0.7, y 0.3..0.7
    f = frame(1.0);
    check(redAt(f, 64, 50) == 200 && redAt(f, 56, 50) == 0 && redAt(f, 100, 34) == 200 && redAt(f, 100, 26) == 0,
          "panned and cropped, the mask stays on what it was drawn round", strfmt("%d %d %d %d", redAt(f, 64, 50), redAt(f, 56, 50), redAt(f, 100, 34), redAt(f, 100, 26)));
    ev.panCrop.clear();

    // keyframes: the rectangle moves from the left (x 0..0.2) at 0 s to the right (x 0.8..1) at 2 s
    ev.mask.clear();
    MaskKey a, b;
    a.time = 0;
    a.paths.push_back(rect(0.0, 0.0, 0.2, 1.0));
    b.time = 2;
    b.paths.push_back(rect(0.8, 0.0, 1.0, 1.0));
    ev.mask = {b, a};                       // (order does not matter)
    f = frame(1.0);
    check(redAt(f, 100, 50) == 200 && redAt(f, 20, 50) == 0 && redAt(f, 180, 50) == 0, "between keyframes the shape is half way", strfmt("%d %d", redAt(f, 100, 50), redAt(f, 20, 50)));
    f = frame(3.0);
    check(redAt(f, 190, 50) == 200 && redAt(f, 100, 50) == 0, "after the last keyframe it stays there");
    ev.mask[1].curve = 1;                   // hold
    f = frame(1.0);
    check(redAt(f, 20, 50) == 200 && redAt(f, 100, 50) == 0, "a hold keyframe keeps its shape until the next");
    {
        // different numbers of points: no in-between, the first key's shape until the next key
        std::vector<MaskKey> ks = {a, b};
        ks[0].curve = 0;
        ks[1].paths[0].points.pop_back();
        const std::vector<MaskPath> mid = maskAt(ks, 1.0);
        check(mid.size() == 1 && mid[0].points.size() == 4 && std::fabs(mid[0].points[0].x) < 1e-9, "shapes with different points are not in-betweened");
        check(maskAt({}, 1.0).empty(), "no keys: no mask");
    }

    // an open path (still being drawn) and a path of two points cut nothing
    ev.mask.clear();
    MaskKey o;
    o.paths.push_back(rect(0.25, 0.2, 0.75, 0.8));
    o.paths[0].closed = false;
    ev.mask.push_back(o);
    check(!maskActive(ev), "an open path is not a mask");
    f = frame(1.0);
    check(redAt(f, 5, 5) == 200, "an open path leaves the picture whole");
    ev.mask[0].paths[0].closed = true;
    ev.mask[0].paths[0].points.resize(2);
    check(!maskActive(ev), "two points are not a mask");
    ev.mask[0].paths[0] = rect(0.25, 0.2, 0.75, 0.8);
    // nonsense points do no harm
    ev.mask[0].paths.push_back(rect(1e300, -1e300, NAN, 5));
    f = frame(1.0);
    check(f && redAt(f, 100, 50) == 200, "nonsense points do no harm");
    ev.mask[0].paths.pop_back();

    // Apply to FX: the picture is not cut; the effect (a colour grade that takes red down) only inside the mask
    {
        FxSlot g = makeBuiltinSlot(kColorGradeId, p.newId());
        ColorGrade cg = ColorGrade::fromJson(g.state);
        cg.exposure = -3;
        g.state = cg.toJson();
        ev.fx.push_back(g);
        f = frame(1.0);
        check(redAt(f, 100, 50) < 60 && redAt(f, 5, 5) == 0, "without Apply to FX: graded and cut", strfmt("%d %d", redAt(f, 100, 50), redAt(f, 5, 5)));
        ev.maskApplyToFx = true;
        f = frame(1.0);
        check(redAt(f, 100, 50) < 60 && redAt(f, 5, 5) == 200, "Apply to FX: the effect inside the mask, the picture as it was outside",
              strfmt("%d %d", redAt(f, 100, 50), redAt(f, 5, 5)));
        fopt.skipEffects = true;
        f = frame(1.0);
        check(redAt(f, 100, 50) == 200 && redAt(f, 5, 5) == 200, "Apply to FX with the effects bypassed: the whole picture");
        fopt.skipEffects = false;
        ev.fx.clear();
    }

    // kept in the project's JSON
    {
        ev.mask[0].paths[0].featherType = 3;
        ev.mask[0].paths[0].feather = 12.5;
        ev.mask[0].paths[0].negative = true;
        ev.mask[0].paths[0].points[1].outX = 0.05;
        ev.mask[0].curve = 2;
        ev.mask[0].time = 0.5;
        Project q;
        std::string err;
        const bool ok = q.fromJson(p.toJson(), &err);
        const Event& qe = q.tracks[0].events[0];
        check(ok && qe.maskOn && qe.maskApplyToFx && qe.mask.size() == 1 && qe.mask[0].time == 0.5 && qe.mask[0].curve == 2 &&
                  qe.mask[0].paths.size() == 1 && qe.mask[0].paths[0].featherType == 3 && qe.mask[0].paths[0].feather == 12.5 &&
                  qe.mask[0].paths[0].negative && qe.mask[0].paths[0].points.size() == 4 && qe.mask[0].paths[0].points[1].outX == 0.05,
              "the mask is kept in the project's JSON", err);
        Project r;
        Event plain;
        check(!p.toJson().dump().empty() && plain.mask.empty() && !plain.maskOn, "an event starts with no mask");
        (void)r;
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
