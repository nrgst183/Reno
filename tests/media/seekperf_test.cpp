// Tests the quick and cancellable picture reads (media.h MediaReadHints) on a real long-GOP file: a quick read far into
// it gives the nearest key frame at once and says so; the exact read after it decodes on from there and gives the same
// picture a fresh reader gives; a cancelled read gives null, and the next reads (forwards and backwards) are still
// right. Prints timings.
//
//   seekperf_test <video with key frames seconds apart>   (make one: ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=60
//                 -t 60 -c:v libx264 -g 600 long_gop.mp4)
#include "../../src/media/media.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>

using namespace reno;

static int passed = 0, failed = 0;
static void check(bool ok, const char* what, const std::string& detail = "")
{
    printf("%s %s%s%s\n", ok ? "PASS" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    (ok ? passed : failed)++;
}
static double now() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static bool same(const ImagePtr& a, const ImagePtr& b)
{
    if (!a || !b || a->w != b->w || a->h != b->h) return false;
    for (int y = 0; y < a->h; ++y)
        if (std::memcmp(a->row(y), b->row(y), (size_t)a->w * 4) != 0) return false;
    return true;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: seekperf_test <video>\n"); return 2; }
    std::string err;
    auto r = openMedia(argv[1], &err);
    auto ref = openMedia(argv[1], &err);
    if (!r || !ref) { printf("cannot open: %s\n", err.c_str()); return 2; }
    const double dur = r->info().duration;
    const int W = 640, H = 360;
    std::mt19937 rng(7);
    double quickWorst = 0, exactWorst = 0, plainWorst = 0;
    bool allSame = true, allApprox = true;
    for (int i = 0; i < 8; ++i) {
        const double t = std::uniform_real_distribution<double>(1.0, dur - 1.0)(rng);
        // a fresh reader's exact picture, and how long that takes
        auto fresh = openMedia(argv[1], &err);
        double a = now();
        ImagePtr want = fresh->videoFrame(t, W, H);
        plainWorst = std::max(plainWorst, now() - a);
        // quick, then exact, on the reader that wanders about
        bool approx = false;
        a = now();
        ImagePtr q;
        {
            MediaReadHintsScope h(nullptr, true);
            q = r->videoFrame(t, W, H);
            approx = h.approximated();
        }
        const double qt = now() - a;
        a = now();
        ImagePtr e = r->videoFrame(t, W, H);
        const double et = now() - a;
        quickWorst = std::max(quickWorst, qt);
        exactWorst = std::max(exactWorst, et);
        printf("  %7.2f s: quick %.0f ms%s, exact after it %.0f ms\n", t, qt * 1000, approx ? " (key frame)" : "", et * 1000);
        if (!q || !e || !same(e, want)) allSame = false;
        // (a target within kQuickGap of a key frame is exact at once: count those as fine)
        if (!approx && !same(q, want)) allApprox = false;
    }
    check(allSame, "the exact picture after a quick one is the one a fresh reader gives");
    check(allApprox, "a quick read that is not marked approximate is the exact picture");
    check(quickWorst < plainWorst * 0.5 || quickWorst < 0.08, "quick reads far into the file come back much sooner than a full decode",
          std::to_string(quickWorst * 1000) + " ms vs " + std::to_string(plainWorst * 1000) + " ms");

    // ---- cancel half way, then on and back
    {
        std::atomic<bool> stop{false};
        const double t = dur * 0.6 + 4.7;
        std::thread killer([&] { std::this_thread::sleep_for(std::chrono::milliseconds(60)); stop = true; });
        double a = now();
        ImagePtr c;
        {
            MediaReadHintsScope h(&stop, false);
            c = r->videoFrame(t, W, H);
        }
        const double ct = now() - a;
        killer.join();
        check(!c && ct < 0.5, "a read cancelled half way stops soon and gives nothing", std::to_string(ct * 1000) + " ms");
        for (double tt : {t, t + 0.31, t - 2.2, t - 0.05, dur * 0.2}) {
            ImagePtr got = r->videoFrame(tt, W, H);
            auto fresh = openMedia(argv[1], &err);
            ImagePtr want = fresh->videoFrame(tt, W, H);
            check(same(got, want), "after the cancelled read, a picture (forwards or back) is right", std::to_string(tt) + " s");
        }
        // cancelled while skipping pictures, then a picture between two decoded ones
        stop = false;
        std::thread k2([&] { std::this_thread::sleep_for(std::chrono::milliseconds(30)); stop = true; });
        const double t2 = dur * 0.3 + 8.9;
        {
            MediaReadHintsScope h(&stop, false);
            r->videoFrame(t2, W, H);
        }
        k2.join();
        for (double tt : {t2 - 7.9, t2 - 7.5, t2 - 3.0}) {
            ImagePtr got = r->videoFrame(tt, W, H);
            auto fresh = openMedia(argv[1], &err);
            check(same(got, fresh->videoFrame(tt, W, H)), "after a cancel while skipping, an earlier picture is right", std::to_string(tt) + " s");
        }
    }
    printf("plain worst %.0f ms, quick worst %.0f ms, exact-after-quick worst %.0f ms\n", plainWorst * 1000, quickWorst * 1000, exactWorst * 1000);
    printf("SUMMARY: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
