// The shared video frames (MediaPool::FrameShare) must count their bytes right: a frame put over another at the same
// start, or a span with no width (a file that repeats a time stamp), used to leave the count too high, so the store
// thought it was full and threw good frames out.
//   frameshare_test
#include "../../src/engine/engine.h"
#include <cstdio>
#include <vector>

using namespace reno;

static int gFail = 0;
static void check(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++gFail;
}

typedef std::vector<std::pair<double, double>> Spans;
static void run(const Spans& s, size_t* bytes, size_t* entries, size_t limit = (size_t)1 << 30)
{
    MediaPool::testFrameShare(s, 16, 8, limit, bytes, entries);
}

int main()
{
    const size_t one = 16 * 8 * 4;
    size_t b, n;
    run({{0, 1}, {1, 2}, {2, 3}}, &b, &n);
    check(n == 3 && b == 3 * one, "three frames in a row");
    run({{5, 5}, {5, 5}}, &b, &n);
    check(b == n * one, "spans with no width: bytes match the frames held");
    run({{0, 1}, {5, 5}, {0, 1}}, &b, &n);
    check(b == n * one && n == 1, "a frame put again replaces itself");
    run({{0, 4}, {0, 2}, {1, 3}}, &b, &n);
    check(b == n * one, "overlapping frames: bytes match");
    run({{3, 3}, {3, 3}, {3, 3}, {3, 3}}, &b, &n, 3 * one);
    check(b == n * one, "no-width spans do not fill the limit");
    // many frames and a small limit: whatever is held is counted exactly
    Spans many;
    for (int i = 0; i < 200; ++i) many.push_back({i * 0.5, i * 0.5 + 0.5});
    run(many, &b, &n, 10 * one);
    check(b == n * one && b <= 10 * one, "eviction keeps the count exact");
    std::printf("%d failed\n", gFail);
    return gFail ? 1 : 0;
}
