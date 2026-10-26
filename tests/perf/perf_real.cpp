// perf_real: times how Reno previews, seeks and plays real projects (real media, the .veg as VEGAS saved it) at engine
// level, with the same calls the editor's picture and sound threads make.
//   open        time to read the .veg
//   seek        time to a finished picture after a seek to a random place: cold (nothing open: a new pool, which opens
//               the files), warm (readers open, other places seeked before) and again (the same place twice in a row)
//   preview     60 sequential pictures at the busiest 2 s, as playback asks, at the Auto preview size (half) and at Best
//               (full size, motion blur / supersampling): ms a frame, frames a second
//   audio       the playback audio path (AudioEngine::render in 1024-sample blocks): realtime factor, cpu per second of
//               sound, the first block after "play" (cold) and after a seek (warm), and a simulated sound device pulling
//               10 ms blocks from a mixer thread that keeps 0.12 s ahead (as Playback does): blocks that were late
//   scrubbing   pictures at places moving forwards then backwards in steps of 3 frames: ms a picture
// Prints "PERF <project> <measure> <value> <unit>".
//
//   perf_real [--quick] [--only seek,preview,audio,open] [--seed N] project.veg...
#include "../../src/engine/engine.h"
#include "../../src/vegas/vegas.h"
#include <chrono>
#include <ctime>
#include <set>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <algorithm>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <atomic>
#include <map>
#include <csignal>
#include <sys/time.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <cxxabi.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>

using namespace reno;

// ---- a small sampling profiler (--prof): every 1 ms of processor time the stack of the running thread is noted;
// at the end the functions with the most samples (alone, and with what they call) are listed. Link with -rdynamic.
namespace prof {
static const int kMax = 200000, kDepth = 24;
static void* stacks[kMax][kDepth];
static int depths[kMax];
static bool onMain[kMax];
static pid_t mainTid = 0;
static std::atomic<int> count{0};
extern "C" char __executable_start, etext;
static void handler(int, siginfo_t*, void* ucv)
{
    // (no backtrace(): it is not safe inside a signal. The interrupted instruction, then every word of the stack's top
    // that points into the program's own code - its callers, with a few stale ones among them)
    const int i = count.load(std::memory_order_relaxed);
    if (i >= kMax) return;
    ucontext_t* uc = (ucontext_t*)ucv;
    int d = 0;
    stacks[i][d++] = (void*)uc->uc_mcontext.gregs[REG_RIP];
    const uintptr_t* sp = (const uintptr_t*)uc->uc_mcontext.gregs[REG_RSP];
    const uintptr_t lo = (uintptr_t)&__executable_start, hi = (uintptr_t)&etext;
    for (int k = 0; k < 2048 && d < kDepth; ++k) {
        const uintptr_t w = sp[k];
        if (w >= lo && w < hi) stacks[i][d++] = (void*)w;
    }
    depths[i] = d;
    onMain[i] = syscall(SYS_gettid) == mainTid;
    count.store(i + 1, std::memory_order_relaxed);
}
static void start()
{
    count = 0;
    mainTid = (pid_t)syscall(SYS_gettid);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    // (wall-clock time of the calling thread: a thread waiting for decoder threads or for the disk shows where it waits)
    sigevent sev;
    std::memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGPROF;
    sev._sigev_un._tid = mainTid;
    static timer_t timer;
    static bool made = false;
    if (!made) { timer_create(CLOCK_MONOTONIC, &sev, &timer); made = true; }
    itimerspec its;
    its.it_interval.tv_sec = 0; its.it_interval.tv_nsec = 1000000;
    its.it_value = its.it_interval;
    timer_settime(timer, 0, &its, nullptr);
}
static std::string nameOf(void* a)
{
    Dl_info di;
    if (dladdr(a, &di) && di.dli_sname) {
        int st = 0;
        char* d = abi::__cxa_demangle(di.dli_sname, nullptr, nullptr, &st);
        std::string s = st == 0 && d ? d : di.dli_sname;
        std::free(d);
        if (s.size() > 90) s = s.substr(0, 90);
        return s;
    }
    if (dladdr(a, &di) && di.dli_fname) {
        const char* f = std::strrchr(di.dli_fname, '/');
        return std::string(f ? f + 1 : di.dli_fname) + "+0x" + [&] { char b[32]; std::snprintf(b, sizeof b, "%lx", (unsigned long)((char*)a - (char*)di.dli_fbase)); return std::string(b); }();
    }
    return "?";
}
struct Scope {
    const char* title; bool on;
    Scope(const char* t, bool o) : title(t), on(o) { if (on) start(); }
    ~Scope();
};
static void stop(const char* title, int top = 25)
{
    {
        sigevent sev; std::memset(&sev, 0, sizeof sev);
        // (the timer made in start() is a function-local static there: stop it by blocking further handling)
        std::signal(SIGPROF, SIG_IGN);
    }
    const int n = count.load();
    std::map<void*, std::string> cache;
    auto nm = [&](void* a) -> const std::string& {
        void* k = (char*)a - 1;
        auto it = cache.find(k);
        if (it == cache.end()) it = cache.emplace(k, nameOf(k)).first;
        return it->second;
    };
    std::map<std::string, int> self, incl;
    int mainN = 0;
    for (int i = 0; i < n; ++i) mainN += onMain[i] ? 1 : 0;
    std::printf("PROF %s: %d samples on the main thread (wall clock), %d on others\n", title, mainN, n - mainN);
    for (int i = 0; i < n; ++i) {
        if (!onMain[i] && !getenv("PROF_ALL")) continue;
        std::set<std::string> seen;
        for (int d = 0; d < depths[i]; ++d) {
            const std::string& s = nm(stacks[i][d]);
            if (d == 0) ++self[s];
            if (seen.insert(s).second) ++incl[s];
        }
    }
    auto dump = [&](const char* what, std::map<std::string, int>& m) {
        std::vector<std::pair<int, std::string>> v;
        for (auto& kv : m) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        std::printf("PROF %s (%d samples) -- %s\n", title, n, what);
        for (int i = 0; i < top && i < (int)v.size(); ++i) std::printf("PROF  %5.1f%%  %s\n", 100.0 * v[i].first / std::max(1, mainN), v[i].second.c_str());
    };
    dump("self", self);
    dump("inclusive", incl);
    std::fflush(stdout);
}
Scope::~Scope() { if (on) stop(title); }
}  // namespace prof

static double nowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double cpuMs() { timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6; }

static std::string gName;
static void report(const char* what, double v, const char* unit)
{
    std::printf("PERF %-26s %-30s %10.3f %s\n", gName.c_str(), what, v, unit);
    std::fflush(stdout);
}
static void reportStats(const std::string& what, std::vector<double> v)
{
    if (v.empty()) return;
    std::sort(v.begin(), v.end());
    double sum = 0;
    for (double d : v) sum += d;
    report((what + " median").c_str(), v[v.size() / 2], "ms");
    report((what + " mean").c_str(), sum / v.size(), "ms");
    report((what + " p90").c_str(), v[std::min(v.size() - 1, (size_t)(v.size() * 0.9))], "ms");
    report((what + " max").c_str(), v.back(), "ms");
}

// Places where video plays: random times inside random events of video tracks.
static std::vector<double> videoPlaces(const Project& p, int n, unsigned seed)
{
    std::vector<std::pair<double, double>> spans;
    for (const auto& t : p.tracks)
        if (t.type == TrackType::Video)
            for (const auto& e : t.events)
                if (e.length > 0.004 && e.mediaId) spans.push_back({e.start, e.end()});
    std::vector<double> out;
    if (spans.empty()) return out;
    std::mt19937 rng(seed);
    for (int k = 0; k < n; ++k) {
        const auto& s = spans[rng() % spans.size()];
        std::uniform_real_distribution<double> at(s.first, s.second - 1e-3);
        out.push_back(at(rng));
    }
    return out;
}

static double busiest(const Project& p, double span)
{
    const double fps = p.settings.fps();
    const int64_t n = (int64_t)std::ceil(p.length() * fps) + 1;
    std::vector<int> active((size_t)std::max<int64_t>(1, n), 0);
    for (const auto& t : p.tracks)
        for (const auto& e : t.events) {
            const int64_t a = std::max<int64_t>(0, (int64_t)std::floor(e.start * fps)), b = std::min<int64_t>(n - 1, (int64_t)std::ceil(e.end() * fps));
            for (int64_t f = a; f <= b; ++f) ++active[(size_t)f];
        }
    const int64_t w = std::max<int64_t>(1, (int64_t)(span * fps));
    int64_t best = 0, sum = 0, bestSum = -1;
    for (int64_t f = 0; f < n; ++f) {
        sum += active[(size_t)f];
        if (f >= w) sum -= active[(size_t)(f - w)];
        if (sum > bestSum) { bestSum = sum; best = std::max<int64_t>(0, f - w + 1); }
    }
    return best / fps;
}

// ---- the simulated sound device: a mixer thread keeps 'ahead' seconds mixed in 1024-sample blocks (Playback::audioThread),
static bool gNoDevice = false;     // --no-device: only the block-by-block audio measurements (for a profiler)
static int gProxyMode = 0;         // --proxies: nested projects play from audio proxies built first (timed); --no-proxies: never
static bool gPrefetch = true;      // --no-prefetch: the device simulation does not make stretched events ahead on a worker (as Playback does)
static bool gKeepCache = false;    // --keep-cache: the device simulation starts with the events the earlier measurements made (a replay)
static bool gPreroll = true;       // --no-preroll: the simulated device starts at the first block, not when the mixer is ahead
static bool gPrewarm = true;       // --no-prewarm: the device simulation's first files are opened by the mixer

// the "device" takes 10 ms every 10 ms of wall time. A pull that finds fewer samples than it wants is a glitch.
struct DeviceSim {
    long pulls = 0, glitches = 0;
    double firstSoundMs = 0;
    double worstBlockMs = 0, maxQueueGapMs = 0;
};
static DeviceSim simulateDevice(const Project& p, MediaPool& pool, FxRuntime& rt, int64_t startSample, double seconds, std::atomic<bool>* videoLoad)
{
    DeviceSim res;
    const int rate = p.settings.sampleRate;
    const int block = 1024;
    AudioEngine ae(pool, rt);
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<std::vector<float>, size_t>> q;     // block, used
    size_t queued = 0;
    std::atomic<bool> run{true};
    const double t0 = nowMs();
    std::atomic<double> firstMixed{-1};
    // (as Playback::startAt does: the files the first events want are opened beside the mixer)
    std::atomic<bool> warmStop{false};
    std::thread warm([&] {
        if (gPrewarm) pool.prewarmAudio(p, (double)startSample / rate, 4.0, &warmStop);
    });
    std::thread mixer([&] {
        int64_t at = startSample;
        std::vector<float> l(block), r(block);
        int64_t pfFrom = 0;
        bool pf = false;
        while (run) {
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait_for(lk, std::chrono::milliseconds(5), [&] { return !run || queued < (size_t)(0.12 * rate); });
                if (!run) break;
                if (queued >= (size_t)(0.12 * rate)) continue;
            }
            if (gPrefetch && (!pf || at < pfFrom || at >= pfFrom + rate / 4)) {
                pf = true;
                pfFrom = at;
#ifndef RENO_NO_PREFETCH
                ae.prefetch(p, at, (int64_t)(2.5 * rate));
#endif
            }
            const double a = nowMs();
            ae.render(p, at, block, l.data(), r.data(), true);
            const double d = nowMs() - a;
            if (std::getenv("PERF_TRACE") && d > 30) std::fprintf(stderr, "slow block at %.3f s (%lld): %.1f ms, %.0f ms after start\n", (double)(at - startSample) / rate, (long long)at, d, nowMs() - t0);
            if (d > res.worstBlockMs) res.worstBlockMs = d;
            if (firstMixed < 0) firstMixed = nowMs() - t0;
            at += block;
            std::lock_guard<std::mutex> lk(m);
            q.push_back({std::vector<float>(l.begin(), l.end()), 0});
            queued += block;
        }
    });
    const int pull = rate / 100;
    const double begin = nowMs();
    long k = 0;
    bool started = false;
    while (true) {
        const double due = begin + (double)k * 10.0;
        const double wait = due - nowMs();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::microseconds((int64_t)(wait * 1000)));
        if (nowMs() - begin > seconds * 1000.0) break;
        {
            std::lock_guard<std::mutex> lk(m);
            // (as Playback does: the first sound waits, for 0.4 s at most, until the mixer is 0.12 s ahead)
            if (!started && queued >= (size_t)pull && (!gPreroll || queued >= (size_t)(0.12 * rate) || nowMs() - begin > 400.0)) {
                started = true;
                res.firstSoundMs = nowMs() - t0;
            }
            if (started) {
                ++res.pulls;
                size_t need = pull;
                if (queued < need) ++res.glitches;
                while (need > 0 && !q.empty()) {
                    auto& b = q.front();
                    const size_t n = std::min(need, b.first.size() - b.second);
                    b.second += n;
                    need -= n;
                    queued -= n;
                    if (b.second == b.first.size()) q.pop_front();
                }
            }
        }
        cv.notify_one();
        ++k;
        (void)videoLoad;
    }
    run = false;
    warmStop = true;
    warm.join();
    cv.notify_all();
    mixer.join();
    return res;
}


int main(int argc, char** argv)
{
    bool quick = false, profile = false;
    unsigned seed = 1;
    std::set<std::string> only;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--quick")) quick = true;
        else if (!std::strcmp(argv[i], "--no-prewarm")) gPrewarm = false;
        else if (!std::strcmp(argv[i], "--no-device")) gNoDevice = true;
        else if (!std::strcmp(argv[i], "--no-prefetch")) gPrefetch = false;
        else if (!std::strcmp(argv[i], "--keep-cache")) gKeepCache = true;
        else if (!std::strcmp(argv[i], "--no-preroll")) gPreroll = false;
        else if (!std::strcmp(argv[i], "--proxies")) gProxyMode = 1;
        else if (!std::strcmp(argv[i], "--no-proxies")) gProxyMode = 2;
        else if (!std::strcmp(argv[i], "--prof")) profile = true;
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (unsigned)std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--only") && i + 1 < argc) {
            std::string s = argv[++i];
            size_t a = 0;
            while (a <= s.size()) { size_t b = s.find(',', a); if (b == std::string::npos) b = s.size(); only.insert(s.substr(a, b - a)); a = b + 1; }
        } else files.push_back(argv[i]);
    }
    auto want = [&](const char* w) { return only.empty() || only.count(w); };
    setProjectFileLoader([](const std::string& path, Project& out, std::string* error) { return loadProjectFile(path, out, error); });
    for (const std::string& path : files) {
        gName = pathFileName(path);
        if (gName.size() > 26) gName = gName.substr(0, 26);
        Project p;
        std::string err;
        double t0 = nowMs();
        if (!loadProjectFile(path, p, &err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); continue; }
        const double openMs = nowMs() - t0;
        if (want("open")) report("open", openMs, "ms");
        size_t events = 0;
        int offline = 0;
        for (const auto& t : p.tracks) events += t.events.size();
        for (const auto& m : p.media) offline += m.offline ? 1 : 0;
        std::printf("INFO %s: %dx%d %.3f fps, %zu tracks, %zu events, %zu media (%d offline), %.1f s\n", gName.c_str(), p.settings.width,
                    p.settings.height, p.settings.fps(), p.tracks.size(), events, p.media.size(), offline, p.length());
        FxRuntime rt;
        rt.prepare(p);
        if (gProxyMode == 2) setAudioProxyOptions(false, false);
        if (gProxyMode == 1) {
            setAudioProxyOptions(true, true);
            const double a = nowMs();
            const std::vector<std::string> need = nestedProjectsNeedingProxies(p);
            for (const std::string& n : need) buildAudioProxy(n, false);
            waitForAudioProxies(900);
            report("audio proxies built", nowMs() - a, "ms");
        }
        const double playSpan = std::min(quick ? 2.0 : 4.0, std::max(0.2, p.length()));     // (a short project plays whole)
        const double from = busiest(p, playSpan);

        if (want("seek")) {
            prof::Scope profScope("seek", profile);
            const std::vector<double> places = videoPlaces(p, quick ? 8 : 16, seed);
            if (!places.empty()) {
                FrameOptions fo;
                std::vector<double> cold, warm, again;
                for (double t : places) {
                    MediaPool pool;
                    VideoEngine ve(pool, rt);
                    const double a = nowMs();
                    ve.render(p, t, fo);
                    cold.push_back(nowMs() - a);
                }
                MediaPool pool;
                VideoEngine ve(pool, rt);
                for (double t : places) {
                    double a = nowMs();
                    ve.render(p, t, fo);
                    warm.push_back(nowMs() - a);
                    a = nowMs();
                    ve.render(p, t, fo);
                    again.push_back(nowMs() - a);
                }
                reportStats("seek cold", cold);
                reportStats("seek warm", warm);
                reportStats("seek same place", again);
            }
        }

        if (want("preview")) {
            prof::Scope profScope("preview", profile);
            struct Mode { const char* name; double scale; bool best; };
            for (Mode md : {Mode{"preview auto(half)", 0.5, false}, Mode{"preview best", 1.0, true}}) {
                MediaPool pool;
                VideoEngine ve(pool, rt);
                FrameOptions fo;
                fo.scale = md.scale;
                fo.subFrames = md.best;
                int n = std::max(10, std::min(quick ? 30 : 60, (int)((p.length() - from) * p.settings.fps())));
                if (const char* e = std::getenv("PERF_N")) n = std::max(1, std::atoi(e));
                const double fps = p.settings.fps();
                // (a warm-up frame opens the files: playback has begun before it matters)
                ve.render(p, from, fo);
                double total = 0, worst = 0;
                const double c0 = cpuMs();
                std::vector<double> each;
                for (int k = 0; k < n; ++k) {
                    const double a = nowMs();
                    ImagePtr img = ve.render(p, from + k / fps, fo);
                    const double d = nowMs() - a;
                    each.push_back(d);
                    total += d;
                    worst = std::max(worst, d);
                    if (!img) { std::printf("render failed\n"); break; }
                }
                char buf[96];
                std::snprintf(buf, sizeof buf, "%s frame", md.name);
                report(buf, total / n, "ms");
                std::snprintf(buf, sizeof buf, "%s cpu", md.name);
                report(buf, (cpuMs() - c0) / n, "ms");
                std::snprintf(buf, sizeof buf, "%s worst", md.name);
                report(buf, worst, "ms");
                std::snprintf(buf, sizeof buf, "%s fps", md.name);
                report(buf, 1000.0 * n / std::max(1e-6, total), "fps");
            }
            // scrubbing at the engine level: places moving forwards then backwards, 3 frames a step
            {
                MediaPool pool;
                VideoEngine ve(pool, rt);
                FrameOptions fo;
                const double fps = p.settings.fps();
                int steps = quick ? 20 : 40;
                if (const char* e = std::getenv("PERF_N")) steps = std::max(1, std::atoi(e));
                std::vector<double> fwd, back;
                ve.render(p, from, fo);
                for (int k = 0; k < steps; ++k) { const double a = nowMs(); ve.render(p, from + 3.0 * k / fps, fo); fwd.push_back(nowMs() - a); }
                for (int k = steps; k >= 0; --k) { const double a = nowMs(); ve.render(p, from + 3.0 * k / fps, fo); back.push_back(nowMs() - a); }
                reportStats("scrub forward step", fwd);
                reportStats("scrub backward step", back);
            }
        }

        if (want("audio")) {
            prof::Scope profScope("audio", profile);
            const int rate = p.settings.sampleRate;
            const int block = 1024;
            const double secs = playSpan;
            const int blocks = (int)(secs * rate / block);
            const int64_t s0 = (int64_t)std::llround(from * rate);
            std::vector<float> l(block), r(block);
            {
                MediaPool pool;
                AudioEngine ae(pool, rt);
                double a = nowMs();
                ae.render(p, s0, block, l.data(), r.data(), true);
                report("audio first block cold (play)", nowMs() - a, "ms");
                std::vector<double> each;
                const double c0 = cpuMs();
                double total = 0;
                for (int k = 1; k < blocks; ++k) {
                    a = nowMs();
                    ae.render(p, s0 + (int64_t)k * block, block, l.data(), r.data(), true);
                    const double d = nowMs() - a;
                    each.push_back(d);
                    total += d;
                }
                report("audio realtime factor", secs * 1000.0 / std::max(1e-6, total + 1), "x");
                report("audio cpu per second", (cpuMs() - c0) / secs, "ms/s");
                reportStats("audio block", each);
                const MediaPool::Stats st = pool.stats();
                report("audio pool reads", (double)st.audioReads, "reads");
                report("audio pool seeks", (double)st.audioSeeks, "reads");
                report("audio pool native-rate", (double)st.audioNative, "reads");
                report("audio pool files opened", (double)st.audioOpened, "files");
                {
                    // the same stretch again on the same pool (the readers open from the first time): a second play
                    std::vector<double> again2;
                    double total2 = 0;
                    const double c1 = cpuMs();
                    for (int k = 0; k < blocks; ++k) {
                        a = nowMs();
                        ae.render(p, s0 + (int64_t)k * block, block, l.data(), r.data(), true);
                        const double d = nowMs() - a;
                        again2.push_back(d);
                        total2 += d;
                    }
                    report("audio warm realtime factor", secs * 1000.0 / std::max(1e-6, total2), "x");
                    report("audio warm cpu per second", (cpuMs() - c1) / secs, "ms/s");
                    reportStats("audio warm block", again2);
                }
                // a seek while playing: the next block somewhere else (warm: readers open)
                std::mt19937 rng(seed);
                std::vector<double> seeks;
                for (int k = 0; k < (quick ? 6 : 12); ++k) {
                    const int64_t at = (int64_t)(std::uniform_real_distribution<double>(0, std::max(0.1, p.length() - 0.3))(rng) * rate);
                    a = nowMs();
                    ae.render(p, at, block, l.data(), r.data(), true);
                    seeks.push_back(nowMs() - a);
                }
                reportStats("audio first block after seek", seeks);
            }
            if (!gNoDevice) {
#ifndef RENO_NO_PREFETCH
                if (!gKeepCache) AudioEngine::clearEventCache();       // (the device simulation is a first play: nothing made before)
#endif
                MediaPool pool;
                const DeviceSim d = simulateDevice(p, pool, rt, s0, std::max(0.5, std::min(quick ? 3.0 : 6.0, p.length() - from)), nullptr);
                report("device first sound", d.firstSoundMs, "ms");
                report("device glitches", (double)d.glitches, "of pulls");
                report("device pulls", (double)d.pulls, "pulls");
                report("device worst block", d.worstBlockMs, "ms");
            }
        }
        rt.clear();
    }
    return 0;
}
