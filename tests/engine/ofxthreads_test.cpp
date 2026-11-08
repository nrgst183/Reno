// The OpenFX multithread suite's multiThread: its threads are parked between calls (making them for every call took about
// 1 ms with 12 threads), but every call must still run all its parts at once (plug-ins wait in one part for the others),
// with the right indexes, from several callers at once, and the parked threads must end when nothing calls for a while.
//   ofxthreads_test [--quick]     (--quick skips the 20 s wait for the parked threads to end)
#include "../../src/ofx/ofxinternal.h"
#include "ofxMultiThread.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <vector>

using namespace reno::ofx;

static int gFail = 0;
static void check(bool ok, const char* name)
{
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++gFail;
}

static const OfxMultiThreadSuiteV1* gMT = nullptr;

// Every part waits (up to 5 s) for all the others to have started: only threads that run at the same time get through.
struct Barrier {
    std::atomic<unsigned> arrived{0};
    std::atomic<unsigned> timedOut{0};
    std::atomic<unsigned> badIndex{0};
    std::atomic<unsigned> notSpawned{0};
    std::atomic<unsigned> nestedOk{0};
    std::atomic<unsigned> seen[64];
    Barrier() { for (auto& s : seen) s = 0; }
};
static void barrierPart(unsigned int k, unsigned int n, void* arg)
{
    Barrier* b = (Barrier*)arg;
    unsigned int idx = 999;
    if (gMT->multiThreadIndex(&idx) != kOfxStatOK || idx != k) b->badIndex++;
    if (k > 0 && gMT->multiThreadIsSpawnedThread() != 1) b->notSpawned++;
    if (k < 64) b->seen[k]++;
    if (gMT->multiThread(barrierPart, 2, arg) == kOfxStatErrExists) b->nestedOk++;
    b->arrived++;
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (b->arrived.load() < n) {
        if (std::chrono::steady_clock::now() > until) { b->timedOut++; return; }
        std::this_thread::yield();
    }
}

static int threadCount()
{
    int n = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end; it.increment(ec)) ++n;
    return n;
}

static void noop(unsigned int, unsigned int, void* arg) { ((std::atomic<int>*)arg)->fetch_add(1); }

int main(int argc, char** argv)
{
    const bool quick = argc > 1 && !std::strcmp(argv[1], "--quick");
    gMT = (const OfxMultiThreadSuiteV1*)fetchSuiteImpl(nullptr, kOfxMultiThreadSuite, 1);
    if (!gMT) { std::printf("FAIL no multithread suite\n"); return 1; }
    const int before = threadCount();

    {
        bool allOk = true;
        for (int round = 0; round < 40 && allOk; ++round) {
            const unsigned n = 2 + (unsigned)(round % 7);
            Barrier b;
            const OfxStatus st = gMT->multiThread(barrierPart, n, &b);
            bool once = true;
            for (unsigned k = 0; k < n; ++k) once = once && b.seen[k].load() == 1;
            allOk = st == kOfxStatOK && b.arrived.load() == n && b.timedOut.load() == 0 && once && b.badIndex.load() == 0 && b.notSpawned.load() == 0 &&
                    b.nestedOk.load() == n;
        }
        check(allOk, "one call: every part runs once, all at the same time, with its own index; a call from a part is refused");
    }
    {
        std::atomic<int> bad{0};
        std::vector<std::thread> callers;
        for (int c = 0; c < 4; ++c)
            callers.emplace_back([&] {
                for (int round = 0; round < 25; ++round) {
                    Barrier b;
                    const unsigned n = 3 + (unsigned)(round % 4);
                    if (gMT->multiThread(barrierPart, n, &b) != kOfxStatOK || b.arrived.load() != n || b.timedOut.load() || b.badIndex.load()) bad++;
                }
            });
        for (auto& t : callers) t.join();
        check(bad.load() == 0, "four callers at once: each call gets threads of its own");
    }
    {
        std::atomic<int> ran{0};
        const int calls = 500;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < calls; ++i) gMT->multiThread(noop, 12, &ran);
        const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / calls;
        std::printf("  12 parts: %.1f us a call\n", us);
        check(ran.load() == calls * 12, "500 calls of 12 parts: all parts ran");
        check(threadCount() - before <= 4 * 6 + 1, "the parked threads are no more than the most used at once");
    }
    if (!quick) {
        std::this_thread::sleep_for(std::chrono::seconds(22));
        check(threadCount() <= before, "parked threads end when nothing has called for a while");
        std::atomic<int> ran{0};
        gMT->multiThread(noop, 5, &ran);
        check(ran.load() == 5, "after that, a call makes new threads");
    }
    std::printf("SUMMARY: %s\n", gFail ? "failed" : "passed");
    return gFail ? 1 : 0;
}
