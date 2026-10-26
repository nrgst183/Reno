// A small sampling profiler (copied from perf_real): every 1 ms of wall time the stack of the main thread is noted; Scope("title", on) prints
// the functions with the most samples (alone, and with what they call) when it ends. Link with -rdynamic.
#pragma once
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <algorithm>
#include <cxxabi.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>

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
