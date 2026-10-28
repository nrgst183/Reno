// Tests of 3D Source Alpha compositing with 3D Track Motion: depth, perspective, rotation, crossing cards, 2D tracks
// separating 3D tracks, compositing parents. Made-up media: "quad:" (four coloured quarters) and "solid:" (one colour).
//
//   motion3d_test [output folder]      (with a folder: the pictures are saved there as .ppm)
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cmath>
#include <cstring>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// The quarters of "quad:" media, as B, G, R.
static const int kTL[3] = {0, 0, 255};          // red
static const int kTR[3] = {0, 255, 0};          // green
static const int kBL[3] = {255, 0, 0};          // blue
static const int kBR[3] = {255, 255, 255};      // white
static const int kRed[3] = {0, 0, 255}, kGreen[3] = {0, 255, 0}, kBlue[3] = {255, 0, 0};

class TestReader : public MediaReader {
public:
    MediaInfo mi;
    std::string kind;
    int solid[4] = {0, 0, 0, 255};
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);
            for (int x = 0; x < w; ++x, d += 4) {
                if (kind == "quad") {
                    const int* c = (y < h / 2) ? (x < w / 2 ? kTL : kTR) : (x < w / 2 ? kBL : kBR);
                    d[0] = (uint8_t)c[0]; d[1] = (uint8_t)c[1]; d[2] = (uint8_t)c[2]; d[3] = 255;
                } else {
                    for (int k = 0; k < 4; ++k) d[k] = (uint8_t)solid[k];
                }
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

static std::unique_ptr<MediaReader> testOpen(const std::string& path, std::string* error)
{
    std::unique_ptr<TestReader> r(new TestReader());
    r->mi.path = path;
    r->mi.hasVideo = true;
    if (path.compare(0, 5, "quad:") == 0) {
        r->kind = "quad";
        r->mi.width = 400; r->mi.height = 200; r->mi.fps = 30; r->mi.duration = 10;
    } else if (path.compare(0, 6, "solid:") == 0) {
        r->kind = "solid";
        r->mi.width = 400; r->mi.height = 200; r->mi.isStill = true;
        std::sscanf(path.c_str() + 6, "%d,%d,%d,%d", &r->solid[0], &r->solid[1], &r->solid[2], &r->solid[3]);
    } else {
        if (error) *error = "unknown test media";
        return nullptr;
    }
    return std::unique_ptr<MediaReader>(r.release());
}

static uint64_t addMedia(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = path;
    std::string e;
    m.info = testOpen(path, &e)->info();
    p.media.push_back(m);
    return m.id;
}

// A video track (at the bottom) with one event of the media from 0 to 5 s.
static uint64_t addTrack(Project& p, uint64_t media, const std::string& composite = "3dsrcalpha", int level = 0)
{
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    t.composite = composite;
    t.compositeLevel = level;
    if (media) {
        Event e;
        e.id = p.newId();
        e.mediaId = media;
        e.length = 5;
        t.events.push_back(e);
    }
    p.tracks.push_back(t);
    return t.id;
}

struct Px { int b, g, r, a; };
static Px pixel(const ImagePtr& img, int x, int y)
{
    if (!img) return {-1, -1, -1, -1};
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2], d[3]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d a=%d", p.b, p.g, p.r, p.a); }
static bool is(const Px& p, const int* c, int tol = 2)
{
    return std::abs(p.b - c[0]) <= tol && std::abs(p.g - c[1]) <= tol && std::abs(p.r - c[2]) <= tol;
}
static bool black(const Px& p) { return p.b <= 1 && p.g <= 1 && p.r <= 1; }
// How many rows of column x are not black.
static int columnHeight(const ImagePtr& img, int x)
{
    int n = 0;
    for (int y = 0; y < img->h; ++y)
        if (!black(pixel(img, x, y))) ++n;
    return n;
}
static int rowWidth(const ImagePtr& img, int y)
{
    int n = 0;
    for (int x = 0; x < img->w; ++x)
        if (!black(pixel(img, x, y))) ++n;
    return n;
}
static bool same(const ImagePtr& a, const ImagePtr& b, int tol, std::string* where)
{
    if (!a || !b || a->w != b->w || a->h != b->h) { *where = "sizes"; return false; }
    for (int y = 0; y < a->h; ++y)
        for (int x = 0; x < a->w; ++x) {
            const Px p = pixel(a, x, y), q = pixel(b, x, y);
            if (std::abs(p.b - q.b) > tol || std::abs(p.g - q.g) > tol || std::abs(p.r - q.r) > tol || std::abs(p.a - q.a) > tol) {
                *where = strfmt("(%d, %d) ", x, y) + pxs(p) + " vs " + pxs(q);
                return false;
            }
        }
    return true;
}

static std::string gOut;
static void save(const ImagePtr& img, const std::string& name)
{
    if (gOut.empty() || !img) return;
    FILE* f = std::fopen((gOut + "/" + name + ".ppm").c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
    for (int y = 0; y < img->h; ++y)
        for (int x = 0; x < img->w; ++x) {
            const Px p = pixel(img, x, y);
            const unsigned char c[3] = {(unsigned char)p.r, (unsigned char)p.g, (unsigned char)p.b};
            std::fwrite(c, 1, 3, f);
        }
    std::fclose(f);
}

// VEGAS's camera (a vertical field of view of 50 degrees), as a fraction of the 200 pixel wide test frame (100 high)
static double camFraction() { return trackMotion3dCamera(200, 100, 1.0) / 200.0; }
static MotionKey key3d(double z, double rotX = 0, double rotY = 0, double rotZ = 0)
{
    MotionKey k;
    k.z = z;
    k.rotX = rotX;
    k.rotY = rotY;
    k.rotation = rotZ;
    return k;
}

int main(int argc, char** argv)
{
    if (argc > 1) gOut = argv[1];
    MediaPool pool;
    pool.setOpener(testOpen);
    FxRuntime fx;
    VideoEngine video(pool, fx);
    FrameOptions fo;

    // ---------------------------------------------------------------- one track: 2D placement, depth, turning
    {
        Project p;
        p.settings.width = 200;
        p.settings.height = 100;
        const uint64_t q = addMedia(p, "quad:");
        const uint64_t tid = addTrack(p, q, "srcalpha");
        auto tr = [&]() -> Track& { return *p.findTrack(tid); };

        // a 2D placement looks the same in 3D Source Alpha
        MotionKey k;
        k.x = 0.4; k.y = 0.55; k.w = 0.5; k.h = 0.6; k.rotation = 20;
        tr().motion = {k};
        ImagePtr flat = video.render(p, 0, fo);
        tr().composite = "3dsrcalpha";
        ImagePtr deep = video.render(p, 0, fo);
        std::string where;
        check(same(flat, deep, 3, &where), "a 2D placement (no Z, no turn in depth) looks the same in 3D Source Alpha", where);
        tr().motion.clear();
        flat = video.render(p, 0, fo);
        check(is(pixel(flat, 2, 2), kTL) && is(pixel(flat, 197, 97), kBR) && is(pixel(flat, 102, 52), kBR), "without keys the 3D track fills the frame",
              pxs(pixel(flat, 2, 2)));

        // further away: smaller, about the middle (VEGAS's camera is 0.5 H / tan(25 degrees) = 1.0723 frame heights away: twice that, half the size)
        tr().motion = {key3d(-camFraction())};
        ImagePtr img = video.render(p, 0, fo);
        save(img, "away");
        check(is(pixel(img, 55, 28), kTL) && is(pixel(img, 145, 72), kBR) && black(pixel(img, 45, 50)) && black(pixel(img, 155, 50)) &&
                  black(pixel(img, 100, 22)),
              "Position Z away from the camera by the camera's distance shows the picture at half the size", pxs(pixel(img, 45, 50)));
        check(rowWidth(img, 50) >= 98 && rowWidth(img, 50) <= 102 && columnHeight(img, 60) >= 48 && columnHeight(img, 60) <= 52,
              "exactly half: 100 x 50", strfmt("%d x %d", rowWidth(img, 50), columnHeight(img, 60)));
        tr().motion = {key3d(camFraction() / 2)};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTL) && is(pixel(img, 180, 80), kBR) && is(pixel(img, 98, 48), kTL) && is(pixel(img, 102, 52), kBR),
              "towards the camera by half its distance: twice the size", pxs(pixel(img, 20, 20)));
        // behind the camera: nothing
        tr().motion = {key3d(1.5)};
        img = video.render(p, 0, fo);
        check(black(pixel(img, 100, 50)) && black(pixel(img, 10, 10)), "a picture behind the camera is not seen", pxs(pixel(img, 100, 50)));

        // turned about the up-down axis: the side that goes away is shorter (perspective)
        tr().motion = {key3d(0, 0, 50)};
        img = video.render(p, 0, fo);
        save(img, "turnY50");
        const int left = columnHeight(img, 80), right = columnHeight(img, 120);
        check(left > 100 - 1 && right < 90 && right > 50, "Rotation Y: the right side goes away and looks shorter, the left comes closer and looks taller",
              strfmt("left %d, right %d", left, right));
        check(is(pixel(img, 70, 25), kTL) && is(pixel(img, 125, 70), kBR), "and the picture still faces the camera the right way", pxs(pixel(img, 70, 25)));
        tr().motion = {key3d(0, 0, 90)};
        img = video.render(p, 0, fo);
        check(black(pixel(img, 60, 50)) && black(pixel(img, 140, 50)) && rowWidth(img, 50) <= 3, "turned 90 degrees it is seen edge on",
              strfmt("%d wide", rowWidth(img, 50)));
        tr().motion = {key3d(0, 0, 180)};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTR) && is(pixel(img, 180, 20), kTL), "turned 180 degrees its back shows: mirrored", pxs(pixel(img, 20, 20)));
        tr().motion = {key3d(0, 50, 0)};
        img = video.render(p, 0, fo);
        save(img, "turnX50");
        check(rowWidth(img, 15) > rowWidth(img, 75) + 10, "Rotation X: the top comes towards the camera and looks wider than the bottom",
              strfmt("top %d, bottom %d", rowWidth(img, 15), rowWidth(img, 75)));
        // orientation turns the picture about its own centre; rotation about the rotation centre (offset)
        MotionKey o = key3d(-camFraction());
        o.oriZ = 90;
        tr().motion = {o};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 85, 30), kBL) && is(pixel(img, 115, 30), kTL), "Orientation Z turns the picture about its centre", pxs(pixel(img, 85, 30)));
        MotionKey r = key3d(0);
        r.w = r.h = 0.5;
        r.offX = 0.25;          // turning about a point a quarter of the frame to the right of the picture's centre
        r.rotation = 180;
        tr().motion = {r};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 175, 62), kTR) && black(pixel(img, 100, 50)), "Rotation turns about the rotation offset: half a turn moves the picture across",
              pxs(pixel(img, 175, 62)));
        // keyframes: the 3D values are interpolated
        MotionKey a = key3d(0), b = key3d(-2 * camFraction());
        b.time = 2;
        tr().motion = {a, b};
        img = video.render(p, 1.0, fo);
        check(rowWidth(img, 50) >= 98 && rowWidth(img, 50) <= 102, "Position Z is interpolated between keyframes (half way: half the size)",
              strfmt("%d", rowWidth(img, 50)));
        // opacity, best quality and 32-bit float
        tr().motion = {key3d(-camFraction())};
        tr().level = 0.5;
        img = video.render(p, 0, fo);
        check(std::abs(pixel(img, 60, 30).r - 128) <= 3, "the track's level applies to its 3D picture", pxs(pixel(img, 60, 30)));
        tr().level = 1;
        p.settings.renderQuality = "best";
        img = video.render(p, 0, fo);
        check(is(pixel(img, 55, 28), kTL) && black(pixel(img, 45, 50)), "best quality draws it too", pxs(pixel(img, 55, 28)));
        p.settings.renderQuality = "draft";
        img = video.render(p, 0, fo);
        check(is(pixel(img, 55, 28), kTL) && black(pixel(img, 45, 50)), "and draft", pxs(pixel(img, 55, 28)));
        p.settings.renderQuality = "good";
        p.settings.pixelFormat = "32bit";
        ImagePtr f = video.render(p, 0, fo);
        if (f && f->depth != PixelDepth::Byte) f = f->converted(PixelDepth::Byte);
        check(f && is(pixel(f, 55, 28), kTL, 3) && black(pixel(f, 45, 50)), "and in 32-bit float projects", f ? pxs(pixel(f, 55, 28)) : "none");
    }

    // ---------------------------------------------------------------- VEGAS's camera
    {
        // vegas180k.dll's tCAMERASETTINGS::Init with dDefaultFieldOfView (0.8726646 rad): z = -1 / tan(fov / 2) in half-frame-height units
        const double D = trackMotion3dCamera(1920, 1080, 1.0);
        check(std::fabs(D - 540.0 / std::tan(0.8726646259971648 / 2)) < 1e-9, "the camera is cot(25 degrees) half frame heights away (a 50 degree vertical field of view)",
              strfmt("%.4f", D));
        check(std::fabs(trackMotion3dCamera(200, 100, 1.0) * 2 - trackMotion3dCamera(400, 200, 1.0)) < 1e-9, "and its distance follows the frame's height, not its width");
        // a frame wider than 16:9 still has the same vertical view: a card at the frame plane fills the frame
        Project p;
        p.settings.width = 400;
        p.settings.height = 200;
        const uint64_t red = addMedia(p, "solid:0,0,255,255");
        const uint64_t tid = addTrack(p, red, "3dsrcalpha");
        p.findTrack(tid)->motion = {key3d(0)};
        ImagePtr img = video.render(p, 0, fo);
        check(is(pixel(img, 2, 2), kRed) && is(pixel(img, 397, 197), kRed), "a card at the frame plane fills the frame", pxs(pixel(img, 2, 2)));
        // one camera distance away (in a frame twice as big): half the size, whatever the frame's size
        p.findTrack(tid)->motion = {key3d(-trackMotion3dCamera(400, 200, 1.0) / 400.0)};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 110, 60), kRed) && is(pixel(img, 290, 140), kRed) && black(pixel(img, 90, 100)) && black(pixel(img, 310, 100)),
              "one camera distance away it is half the size in a bigger frame too", pxs(pixel(img, 90, 100)));
        // 3D has no 2D Shadow or Glow in VEGAS (the keyframes are deleted when converting): leftover ones are not drawn
        GlowKey g;
        g.x = 0.1; g.y = 0.1; g.blur = 0; g.intensity = 1;
        g.color = 0xFF00FF00;
        p.findTrack(tid)->motion = {key3d(-camFraction())};
        ImagePtr without = video.render(p, 0, fo);
        p.findTrack(tid)->shadow2d = {g};
        p.findTrack(tid)->glow2d = {g};
        ImagePtr with = video.render(p, 0, fo);
        std::string where;
        check(same(without, with, 0, &where), "a 3D track's leftover 2D Shadow and Glow keyframes are not drawn", where);
    }

    // ---------------------------------------------------------------- several 3D tracks in one space
    {
        Project p;
        p.settings.width = 200;
        p.settings.height = 100;
        const uint64_t red = addMedia(p, "solid:0,0,255,255"), green = addMedia(p, "solid:0,255,0,255"), blue = addMedia(p, "solid:255,0,0,255");
        const uint64_t a = addTrack(p, red);            // top
        const uint64_t b = addTrack(p, green);          // below it
        p.findTrack(b)->motion = {key3d(0, 0, 45)};     // turned: its left half comes in front of the top track
        ImagePtr img = video.render(p, 0, fo);
        save(img, "crossing");
        check(is(pixel(img, 70, 50), kGreen) && is(pixel(img, 130, 50), kRed),
              "two 3D tracks cross: the lower track's picture shows where it is nearer the camera, the upper one's elsewhere",
              pxs(pixel(img, 70, 50)) + " / " + pxs(pixel(img, 130, 50)));
        check(is(pixel(img, 100, 30), kRed), "where they are at the same depth the upper track shows", pxs(pixel(img, 100, 30)));
        // a lower track nearer the camera covers an upper one
        p.findTrack(a)->motion = {key3d(-1)};
        p.findTrack(b)->motion = {key3d(0)};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 100, 50), kGreen), "a lower 3D track nearer the camera covers the upper one", pxs(pixel(img, 100, 50)));
        // a 2D track between them keeps them apart: the upper is laid over the finished 3D picture below
        Track mid;
        mid.id = p.newId();
        mid.type = TrackType::Video;
        p.tracks.insert(p.tracks.begin() + 1, mid);
        img = video.render(p, 0, fo);
        check(is(pixel(img, 100, 50), kRed) && is(pixel(img, 20, 50), kGreen), "a 2D track between 3D tracks keeps them apart (VEGAS's rule)",
              pxs(pixel(img, 100, 50)));
        p.tracks.erase(p.tracks.begin() + 1);
        // a 2D track under 3D tracks is simply under their picture
        addTrack(p, blue, "srcalpha");
        p.findTrack(b)->motion = {key3d(-1)};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 10, 10), kBlue) && is(pixel(img, 100, 50), kRed), "a 2D track below shows round the 3D pictures", pxs(pixel(img, 10, 10)));
    }

    // ---------------------------------------------------------------- compositing parents in 3D
    {
        Project p;
        p.settings.width = 200;
        p.settings.height = 100;
        const uint64_t red = addMedia(p, "solid:0,0,255,255"), green = addMedia(p, "solid:0,255,0,255"), q = addMedia(p, "quad:");
        const uint64_t par = addTrack(p, 0, "3dsrcalpha", 0);       // a parent with no events
        const uint64_t c1 = addTrack(p, green, "3dsrcalpha", 1);    // its children: the upper one at the frame's plane
        const uint64_t c2 = addTrack(p, red, "3dsrcalpha", 1);      // the lower one nearer the camera, half size
        p.findTrack(par)->parentComposite = "3dsrcalpha";
        MotionKey near = key3d(0.1);
        near.w = near.h = 0.5;
        p.findTrack(c2)->motion = {near};
        ImagePtr img = video.render(p, 0, fo);
        check(is(pixel(img, 100, 50), kRed) && is(pixel(img, 10, 10), kGreen), "a 3D parent's 3D children share its space: the nearer child shows in front",
              pxs(pixel(img, 100, 50)));
        // Parent Motion moves the whole group in 3D, the children keeping their own depths
        p.findTrack(par)->parentMotion = {key3d(-camFraction())};
        img = video.render(p, 0, fo);
        save(img, "parent");
        const int gw = rowWidth(img, 30);
        check(black(pixel(img, 45, 50)) && is(pixel(img, 55, 30), kGreen) && is(pixel(img, 100, 50), kRed) && gw >= 98 && gw <= 102,
              "Parent Motion takes the whole group away from the camera (the far child at half the size)", strfmt("%d wide", gw));
        // the near child: 100 x 50 (half the frame), 20 nearer than the far one: seen at D / (D + D - 20) of that
        int redW = 0;
        for (int x = 0; x < 200; ++x)
            if (is(pixel(img, x, 50), kRed, 30)) ++redW;
        check(std::abs(redW - (int)std::lround(100 * trackMotion3dCamera(200, 100, 1.0) / (2 * trackMotion3dCamera(200, 100, 1.0) - 20))) <= 2, "and the nearer child keeps its distance in front of the other",
              strfmt("%d wide", redW));
        // a parent in 3D whose children are not all in 3D: the group is laid flat and moved as one picture
        p.findTrack(c1)->composite = "srcalpha";
        p.findTrack(c1)->events[0].mediaId = q;
        p.findTrack(par)->parentMotion = {key3d(0, 0, 180)};
        img = video.render(p, 0, fo);
        check(is(pixel(img, 20, 20), kTR) && is(pixel(img, 180, 20), kTL), "a 3D parent with a 2D child turns its whole (flat) group in 3D",
              pxs(pixel(img, 20, 20)));
        // a 2D parent: its 3D children are drawn in 3D and the group is laid flat
        p.findTrack(c1)->composite = "3dsrcalpha";
        p.findTrack(c1)->events[0].mediaId = green;
        p.findTrack(par)->parentComposite = "srcalpha";
        p.findTrack(par)->parentMotion.clear();
        img = video.render(p, 0, fo);
        check(is(pixel(img, 100, 50), kRed) && is(pixel(img, 10, 10), kGreen), "a 2D parent's 3D children are still drawn in 3D", pxs(pixel(img, 100, 50)));
    }

    // ---------------------------------------------------------------- saving
    {
        Project p;
        const uint64_t q = addMedia(p, "quad:");
        const uint64_t tid = addTrack(p, q);
        MotionKey k = key3d(-0.5, 10, 20, 30);
        k.d = 2; k.oriX = 1; k.oriY = 2; k.oriZ = 3; k.offX = 0.1; k.offY = 0.2; k.offZ = 0.3;
        MotionKey flat;
        flat.time = 1;
        p.findTrack(tid)->motion = {k, flat};
        p.findTrack(tid)->parentMotion = {k};
        MediaItem::Marker m;
        m.time = 1.5;
        m.name = "Chorus";
        p.media[0].markers.push_back(m);
        m.time = 3;
        m.length = 2;
        m.name = "Take 2";
        p.media[0].markers.push_back(m);
        Project back;
        check(back.fromJson(p.toJson()), "a project with 3D keys reads back");
        const Track* t = back.findTrack(tid);
        const bool ok = t && t->motion.size() == 2 && t->motion[0].z == -0.5 && t->motion[0].rotX == 10 && t->motion[0].rotY == 20 &&
                        t->motion[0].rotation == 30 && t->motion[0].d == 2 && t->motion[0].oriX == 1 && t->motion[0].oriY == 2 &&
                        t->motion[0].oriZ == 3 && t->motion[0].offX == 0.1 && t->motion[0].offY == 0.2 && t->motion[0].offZ == 0.3 &&
                        !t->motion[1].has3d() && t->parentMotion.size() == 1 && t->parentMotion[0].z == -0.5;
        check(ok, "every 3D Track Motion value (and Parent Motion's) is kept");
        check(back.media.size() == 1 && back.media[0].markers.size() == 2 && back.media[0].markers[1].length == 2 &&
                  back.media[0].markers[1].name == "Take 2" && back.media[0].markers[0].time == 1.5,
              "media markers and regions saved from the Trimmer are kept");
        Project p2;
        const uint64_t q2 = addMedia(p2, "quad:");
        const uint64_t t2 = addTrack(p2, q2, "srcalpha");
        MotionKey plain;
        p2.findTrack(t2)->motion = {plain};
        const Json j = p2.toJson();
        check(j.get("tracks").at(0).get("motion").at(0).size() == 7, "a 2D key is written as before (seven numbers)");
    }

    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
