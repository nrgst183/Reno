// Tests of video proxies (src/media/proxy.cpp, MediaPool::setUseProxies): a proxy is made next to the file as
// "<file>.sfvp0", small, with the same frame rate and length; a pool using proxies reads it and falls back to the
// original when it is removed or unreadable; a pool not using them (renders) never does; cancelling leaves nothing behind.
//
//   proxy_test <scratch folder>
#include "../../src/engine/engine.h"
#include <cstdio>
#include <cmath>
#include <filesystem>

using namespace reno;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A video of 'frames' pictures: the left half the colour (b, g, r), a white box that moves across the right half.
static bool makeVideo(const std::string& path, int w, int h, int fps, int frames, int b, int g, int r)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = w;
    es.height = h;
    es.fpsNum = fps;
    es.fpsDen = 1;
    es.videoFamily = "h264";
    es.encodeMode = "cpu";
    es.rateControl = "quality";
    es.quality = 18;
    std::string err;
    auto out = createMediaWriter(es, &err);
    if (!out) { std::printf("cannot write %s: %s\n", path.c_str(), err.c_str()); return false; }
    for (int f = 0; f < frames; ++f) {
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                const bool box = x >= w / 2 + f * 4 && x < w / 2 + f * 4 + w / 10 && y > h / 3 && y < 2 * h / 3;
                if (x < w / 2) { d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; }
                else { const uint8_t v = box ? 255 : 0; d[0] = d[1] = d[2] = v; }
                d[3] = 255;
            }
        }
        if (!out->writeVideo(*img)) return false;
    }
    return out->finish(&err);
}

struct Px { int b, g, r; };
static Px at(const ImagePtr& img, double fx, double fy)
{
    if (!img) return {-1, -1, -1};
    const int x = std::min(img->w - 1, (int)(fx * img->w)), y = std::min(img->h - 1, (int)(fy * img->h));
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2]};
}
static std::string pxs(const Px& p) { return strfmt("b=%d g=%d r=%d", p.b, p.g, p.r); }
static bool near(const Px& p, int b, int g, int r, int tol = 12) { return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol; }

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "proxy_out";
    std::filesystem::create_directories(dir);
    const std::string video = dir + "/clip.mp4", other = dir + "/other.mp4";
    std::filesystem::remove(videoProxyPath(video));
    if (!makeVideo(video, 1280, 720, 25, 50, 200, 40, 10) || !makeVideo(other, 640, 360, 25, 50, 10, 200, 10)) {
        check(false, "the test videos can be made");
        std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
        return 1;
    }
    check(!hasVideoProxy(video) && videoProxyPath(video) == video + ".sfvp0", "a proxy's name is VEGAS's: <file>.sfvp0");

    // ---- making one
    const uint64_t gen0 = videoProxyGeneration();
    int calls = 0;
    double lastDone = 0;
    std::string err;
    bool ok = createVideoProxy(video, [&](double f) { ++calls; lastDone = f; return true; }, &err);
    check(ok && hasVideoProxy(video), "Create Video Proxy makes <file>.sfvp0", err);
    check(calls > 2 && std::fabs(lastDone - 1) < 1e-9, "progress is reported up to the end", strfmt("%d calls, last %.3f", calls, lastDone));
    check(videoProxyGeneration() != gen0, "and readers are told proxies changed");
    check(!std::filesystem::exists(videoProxyPath(video) + ".part.mp4"), "no half-made file is left");
    {
        auto r = openMedia(videoProxyPath(video), &err);
        check(r && r->info().hasVideo && r->info().width == 960 && r->info().height == 540, "it is a video of at most 960 x 540, keeping the shape",
              r ? strfmt("%dx%d", r->info().width, r->info().height) : err);
        check(r && std::fabs(r->info().fps - 25) < 0.01 && std::fabs(r->info().duration - 2.0) < 0.05 && !r->info().hasAudio,
              "of the same frame rate and length, without sound", r ? strfmt("%.3f fps, %.3f s", r->info().fps, r->info().duration) : "");
        // its frames are the original's, at the same times (the box has moved the same)
        auto o = openMedia(video, &err);
        ImagePtr a = o ? o->videoFrame(1.02, 960, 540) : nullptr, b = r ? r->videoFrame(1.02, 960, 540) : nullptr;
        bool same = a && b;
        int worst = 0;
        for (double fx = 0.05; same && fx < 1; fx += 0.05) {
            const Px p = at(a, fx, 0.5), q = at(b, fx, 0.5);
            worst = std::max(worst, std::abs(p.r - q.r) + std::abs(p.g - q.g) + std::abs(p.b - q.b));
        }
        check(same && worst < 60, "its frames match the original's at the same times", strfmt("worst difference %d", worst));
    }

    // ---- stopped half way: nothing left (and an existing proxy stays)
    {
        const std::string third = dir + "/third.mp4";
        std::filesystem::copy_file(other, third, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::remove(videoProxyPath(third));
        int n = 0;
        ok = createVideoProxy(third, [&](double) { return ++n < 2; }, &err);
        check(!ok && !hasVideoProxy(third) && !std::filesystem::exists(videoProxyPath(third) + ".part.mp4"), "stopped, it leaves nothing behind", err);
    }
    // ---- files that cannot have one
    {
        ok = createVideoProxy(dir + "/no-such-file.mp4", nullptr, &err);
        check(!ok && !err.empty(), "a file that is not there: an error", err);
        const std::string junk = dir + "/junk.mp4";
        writeFile(junk, "not a video at all");
        ok = createVideoProxy(junk, nullptr, &err);
        check(!ok && !hasVideoProxy(junk), "a file that is not a video: an error", err);
    }

    // ---- used by a pool that asks for proxies, not by others
    Project p;
    p.settings.width = 1280;
    p.settings.height = 720;
    p.settings.fpsNum = 25;
    MediaItem mi;
    mi.id = p.newId();
    mi.path = video;
    mi.info = openMedia(video, &err)->info();
    p.media.push_back(mi);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = mi.id;
    e.length = 2;
    t.events.push_back(e);
    p.tracks.push_back(t);
    // (the proxy replaced by a video of another colour, so that it shows which one was read)
    std::filesystem::copy_file(other, videoProxyPath(video), std::filesystem::copy_options::overwrite_existing);
    videoProxiesChanged();
    FxRuntime fx;
    {
        MediaPool pool;
        check(!pool.useProxies(), "a pool does not use proxies unless asked (renders never do)");
        VideoEngine ve(pool, fx);
        FrameOptions fo;
        fo.scale = 0.5;
        ImagePtr img = ve.render(p, 0.5, fo);
        check(near(at(img, 0.2, 0.5), 200, 40, 10), "without proxies: the original", pxs(at(img, 0.2, 0.5)));
        pool.setUseProxies(true);
        img = ve.render(p, 0.6, fo);
        check(near(at(img, 0.2, 0.5), 10, 200, 10), "asked to: the proxy", pxs(at(img, 0.2, 0.5)));
        check(near(at(img, 0.02, 0.05), 10, 200, 10) && near(at(img, 0.47, 0.95), 10, 200, 10) && near(at(img, 0.53, 0.05), 0, 0, 0),
              "placed as the original (it fills the frame: its left half to the middle)", pxs(at(img, 0.02, 0.05)) + " / " + pxs(at(img, 0.47, 0.95)));
        // the proxy goes: the original again
        std::filesystem::remove(videoProxyPath(video));
        videoProxiesChanged();
        img = ve.render(p, 0.7, fo);
        check(near(at(img, 0.2, 0.5), 200, 40, 10), "the proxy removed: the original again", pxs(at(img, 0.2, 0.5)));
        // a proxy that cannot be read: the original
        writeFile(videoProxyPath(video), "garbage");
        videoProxiesChanged();
        img = ve.render(p, 0.8, fo);
        check(near(at(img, 0.2, 0.5), 200, 40, 10), "a proxy that cannot be read: the original", pxs(at(img, 0.2, 0.5)));
        std::filesystem::remove(videoProxyPath(video));
        videoProxiesChanged();
    }
    fx.releaseExcept({});
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
