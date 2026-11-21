// Tests of nested projects' preview files (engine.h buildNestedPreview, media/nestedcache.h): the file keeps every frame
// exactly, can be read in any order from several threads, is built deepest project first, is read by previews and
// gives the same pictures as rendering, is never read by a final render, goes stale when anything it is made from
// changes, and a stopped build leaves nothing behind.
//
//   nestedcache_test <scratch folder>
// (built with the other tests: cmake -DRENO_WITH_TESTS=ON, target nestedcache_test)
#include "../../src/engine/engine.h"
#include "../../src/media/nestedcache.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace reno;

static int gPass = 0, gFail = 0;
static bool check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) {
        ++gPass;
        std::printf("PASS %s\n", name.c_str());
    } else {
        ++gFail;
        std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    }
    std::fflush(stdout);
    return ok;
}

static bool sameBytes(const Image& a, const Image& b)
{
    if (a.w != b.w || a.h != b.h || a.depth != b.depth) return false;
    a.sync();
    b.sync();
    for (int y = 0; y < a.h; ++y)
        if (std::memcmp(a.row(y), b.row(y), (size_t)a.w * a.pixelBytes()) != 0) return false;
    return true;
}

// Frame k of a made-up clip: every byte different from its neighbours', partly transparent (premultiplied).
static ImagePtr madeUp(int w, int h, int k)
{
    ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t* d = img->row(y) + x * 4;
            const int a = (x * 7 + y * 3 + k * 11) & 255;
            d[0] = (uint8_t)(((x + k * 5) & 255) * a / 255);
            d[1] = (uint8_t)(((y * 2 + k) & 255) * a / 255);
            d[2] = (uint8_t)(((x ^ y) + k * 9 & 255) * a / 255);
            d[3] = (uint8_t)a;
        }
    return img;
}

static bool writeStill(const std::string& path)
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = 320;
    es.height = 180;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, nullptr);
    if (!w) return false;
    ImagePtr img = Image::make(320, 180, PixelDepth::Byte, false);
    for (int y = 0; y < 180; ++y)
        for (int x = 0; x < 320; ++x) {
            uint8_t* d = img->row(y) + x * 4;
            d[0] = (uint8_t)(x * 3 / 4);
            d[1] = (uint8_t)(y + 40);
            d[2] = (uint8_t)((x + y) / 2);
            d[3] = 255;
        }
    return w->writeVideo(*img) && w->finish(nullptr);
}

static MediaItem mediaItem(Project& p, const std::string& path)
{
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    std::unique_ptr<MediaReader> r = openMediaOrProject(path, nullptr);
    if (r) m.info = r->info();
    else m.offline = true;
    return m;
}

// A 320 x 180, 30 fps project showing 'media' for two seconds (slightly to the right, so that its pictures differ
// from the media's).
static Project showing(const std::string& media)
{
    Project p;
    p.settings.width = 320;
    p.settings.height = 180;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    p.settings.sampleRate = 48000;
    MediaItem m = mediaItem(p, media);
    p.media.push_back(m);
    Track t;
    t.id = p.newId();
    t.type = TrackType::Video;
    Event e;
    e.id = p.newId();
    e.mediaId = m.id;
    e.start = 0;
    e.length = 2.0;
    t.events.push_back(e);
    p.tracks.push_back(t);
    return p;
}

static ImagePtr previewFrame(const Project& p, double t, bool final)
{
    MediaPool pool;
    FxRuntime fx;
    VideoEngine engine(pool, fx);
    FrameOptions fo;
    fo.interactive = !final;
    fo.scale = 0.5;
    ImagePtr img = engine.render(p, t, fo);
    if (img) img->sync();
    return img;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::printf("usage: nestedcache_test <scratch folder>\n");
        return 2;
    }
    const std::string dir = argv[1];
    std::string err;

    // ---------------------------------------------------------------- the file
    {
        const std::string f = pathJoin(dir, "frames.renopreview");
        NestedCacheInfo info;
        info.width = 161;                   // (an odd size: rows and planes of odd lengths)
        info.height = 91;
        info.fpsNum = 30000;
        info.fpsDen = 1001;
        info.signature = 0x1234567890abcdefull;
        std::vector<ImagePtr> frames;
        for (int k = 0; k < 8; ++k) frames.push_back(madeUp(info.width, info.height, k));
        {
            NestedCacheWriter w;
            check(w.open(f, info, &err), "a preview file is opened for writing", err);
            for (const ImagePtr& img : frames) w.write(*img);
            NestedCacheInfo before;
            check(!readNestedCacheInfo(f + ".part", before), "an unfinished file is not taken for a preview file");
            check(w.finish(&err), "and finished", err);
        }
        NestedCacheInfo got;
        check(readNestedCacheInfo(f, got) && got.width == 161 && got.height == 91 && got.fpsNum == 30000 && got.fpsDen == 1001 &&
                  got.frames == 8 && got.signature == info.signature,
              "its header says what it holds", strfmt("%dx%d %d/%d %lld codec %d", got.width, got.height, got.fpsNum, got.fpsDen, (long long)got.frames, got.codec));
        check(got.codec == 1, "the frames are compressed (UT Video)");
        check(!fileExists(f + ".part"), "the part written is renamed into place");
        NestedCacheFile file;
        check(file.open(f, &err), "it is opened for reading", err);
        bool exact = true;
        for (int k : {5, 0, 7, 3, 3, 1, 6, 2, 4})       // any order, the same frame twice
            if (ImagePtr img = file.read(k)) exact = exact && sameBytes(*img, *frames[(size_t)k]);
            else exact = false;
        check(exact, "every frame comes back byte for byte, read in any order");
        check(!file.read(8) && !file.read(-1), "frames past its end are not there");
        std::atomic<bool> allSame{true};
        std::vector<std::thread> readers;
        for (int t = 0; t < 4; ++t)
            readers.emplace_back([&, t] {
                for (int n = 0; n < 40; ++n) {
                    const int k = (n * 3 + t) % 8;
                    ImagePtr img = file.read(k);
                    if (!img || !sameBytes(*img, *frames[(size_t)k])) allSame = false;
                }
            });
        for (auto& r : readers) r.join();
        check(allSame.load(), "and from four threads at once");
        file.close();
        {
            NestedCacheWriter w;
            w.open(pathJoin(dir, "dropped.renopreview"), info, &err);
            w.write(*frames[0]);
            w.abort();
        }
        check(!fileExists(pathJoin(dir, "dropped.renopreview.part")) && !fileExists(pathJoin(dir, "dropped.renopreview")),
              "a stopped file leaves nothing behind");
    }

    // ---------------------------------------------------------------- nested projects
    // the effects are made on the spot, as the command-line tools do
    setMainThreadRunner([](const std::function<void()>& fn) {
        fn();
        return true;
    });
    const std::string still = pathJoin(dir, "nc_still.bmp"), inner = pathJoin(dir, "nc_inner.reno"), middle = pathJoin(dir, "nc_middle.reno");
    for (const std::string& f : {inner, middle}) {
        std::remove(nestedCachePath(f).c_str());
        std::remove(nestedCacheFallbackPath(f).c_str());
    }
    check(writeStill(still), "the test picture is written");
    Project in = showing(still);
    check(in.save(inner, &err), "the innermost project is saved", err);
    check(showing(inner).save(middle, &err), "a project using it is saved", err);
    Project outer = showing(middle);

    std::vector<std::string> need = nestedPreviewsNeeded(outer);
    check(need.size() == 2 && pathFileName(need[0]) == "nc_inner.reno" && pathFileName(need[1]) == "nc_middle.reno",
          "the ones to build: the deepest first", need.size() == 2 ? need[0] + ", " + need[1] : strfmt("%zu of them", need.size()));

    setNestedPreviewUse(false);
    const ImagePtr live = previewFrame(outer, 0.5, false);
    const ImagePtr liveFinal = previewFrame(outer, 0.5, true);
    setNestedPreviewUse(true);

    int64_t told = 0;
    check(buildNestedPreview(inner, [&](int64_t done, int64_t, bool) { told = done; return true; }, &err) == 0, "the innermost one is built", err);
    check(told == 60, "its progress reaches every frame", strfmt("%lld", (long long)told));
    check(buildNestedPreview(middle, nullptr, &err) == 0, "then the one using it", err);
    check(nestedPreviewCurrent(inner) && nestedPreviewCurrent(middle) && nestedPreviewsNeeded(outer).empty(), "both are current");
    NestedCacheInfo ii;
    check(readNestedCacheInfo(nestedCachePath(inner), ii) && ii.width == 160 && ii.height == 90 && ii.frames == 60,
          "at half the project's size, one frame per frame", strfmt("%dx%d %lld", ii.width, ii.height, (long long)ii.frames));

    const ImagePtr fromFiles = previewFrame(outer, 0.5, false);
    check(live && fromFiles && sameBytes(*live, *fromFiles), "a preview read from them is the same, byte for byte, as rendering");

    // A file with other pictures but the right signature: previews show it, a final render does not.
    {
        NestedCacheInfo fake = ii;
        fake.signature = nestedPreviewSignature(inner);
        NestedCacheWriter w;
        w.open(nestedCachePath(inner) + ".fake", fake, &err);
        ImagePtr red = Image::make(160, 90, PixelDepth::Byte, false);
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) {
                uint8_t* d = red->row(y) + x * 4;
                d[0] = 0; d[1] = 0; d[2] = 255; d[3] = 255;
            }
        for (int k = 0; k < 60; ++k) w.write(*red);
        w.finish(&err);
    }
    setNestedPreviewUse(false);                 // (the files read so far are let go of)
    std::remove(nestedCachePath(inner).c_str());
    std::rename((nestedCachePath(inner) + ".fake").c_str(), nestedCachePath(inner).c_str());
    setNestedPreviewUse(true);
    {
        std::unique_ptr<MediaReader> r = openProjectMedia(inner, &err);
        ImagePtr p = r ? r->videoFrame(0.5, 160, 90) : nullptr;
        if (p) p->sync();
        check(p && p->row(45)[2] == 255 && p->row(45)[0] == 0, "a preview reads the file", p ? strfmt("%d %d", p->row(45)[0], p->row(45)[2]) : "no frame");
        ImagePtr small = r ? r->videoFrame(0.5, 80, 45) : nullptr;
        check(small && small->w == 80 && small->h == 45, "scaled down for a smaller preview");
        FinalRenderScope final;
        std::unique_ptr<MediaReader> r2 = openProjectMedia(inner, &err);
        ImagePtr q = r2 ? r2->videoFrame(0.5, 160, 90) : nullptr;
        if (q) q->sync();
        check(q && !(q->row(45)[2] == 255 && q->row(45)[0] == 0), "a final render never does");
    }
    const ImagePtr finalNow = previewFrame(outer, 0.5, true);
    check(liveFinal && finalNow && sameBytes(*liveFinal, *finalNow), "a final render of the outer project is the same as before there were files");

    // ---------------------------------------------------------------- stale when something it is made from changes
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    in.tracks[0].events[0].length = 1.5;
    check(in.save(inner, &err), "the innermost project is changed and saved again", err);
    check(!nestedPreviewCurrent(inner), "its file is stale at once");
    check(!nestedPreviewCurrent(middle), "and so is the file of the project using it");
    need = nestedPreviewsNeeded(outer);
    check(need.size() == 2 && pathFileName(need[0]) == "nc_inner.reno", "both are to be built again, the deepest first");

    // ---------------------------------------------------------------- stopped
    const int stopped = buildNestedPreview(inner, [](int64_t done, int64_t, bool) { return done < 5; }, &err);
    check(stopped == 2, "a build can be stopped", strfmt("%d", stopped));
    check(!fileExists(nestedCachePath(inner) + ".part") && !nestedPreviewCurrent(inner), "and leaves no file behind");
    check(buildNestedPreview(inner, nullptr, &err) == 0 && nestedPreviewCurrent(inner), "built again afterwards", err);

    std::printf("\n%d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
