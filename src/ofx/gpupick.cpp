#include "gpupick.h"
#include "vkeffects.h"
#include "../core/base.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

namespace reno {
namespace ofx {
namespace builtin {
namespace pick {

namespace {

typedef std::chrono::steady_clock Clock;
double msSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

const int kRevision = 1;                // raised when the kernels or this rule change so every card is measured again
const int kW = 1920, kH = 1080;
const int kMaxCases = 2;
const double kCpuMustWinBy = 1.25;      // the CPU is picked only when it is this much faster than the GPU run on its own (a chain on the card saves transfers)
const double kBudgetMs = 150000;        // the whole measurement stops here; what is not measured stays on the GPU

std::atomic<bool> gAuto(true);
std::atomic<bool> gMeasuring(false), gCancel(false), gGotAnswer(true);
std::mutex gMu;
std::shared_ptr<const std::map<std::string, bool>> gChoice;     // id -> use the GPU
std::vector<Entry> gEntries;
std::string gKey;
std::thread gThread;

std::string storePath()
{
    const std::string dir = userDataDir();
    return dir.empty() ? std::string() : pathJoin(dir, "gpu_pick.txt");
}

std::string makeKey(const std::string& renoVersion)
{
    const gpu::DeviceInfo d = gpu::currentDevice();
    char buf[96];
    std::snprintf(buf, sizeof buf, "|%04x:%04x|drv %u|api %u|rev %d|", d.vendorId, d.deviceId, d.driverVersion, d.apiVersion, kRevision);
    return d.name + buf + renoVersion;
}

void publish(const std::vector<Entry>& es, const std::string& key)
{
    auto m = std::make_shared<std::map<std::string, bool>>();
    for (const Entry& e : es) (*m)[e.id] = e.gpu;
    std::lock_guard<std::mutex> lk(gMu);
    gChoice = m;
    gEntries = es;
    gKey = key;
}

void save(const std::vector<Entry>& es, const std::string& key)
{
    const std::string path = storePath();
    if (path.empty()) return;
    std::string t = "reno-gpu-pick 1\nkey\t" + key + "\n";
    char buf[64];
    for (const Entry& e : es) {
        std::snprintf(buf, sizeof buf, "\t%d\t%.3f\t%.3f\t%d\t%d\n", e.gpu ? 1 : 0, e.cpuMs, e.gpuMs, e.cases, e.differs ? 1 : 0);
        t += e.id + buf;
    }
    writeFile(path, t);
}

bool load(const std::string& key, std::vector<Entry>& out)
{
    std::string text;
    const std::string path = storePath();
    if (path.empty() || !readFile(path, text)) return false;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line != "reno-gpu-pick 1") return false;
    if (!std::getline(in, line) || line != "key\t" + key) return false;
    out.clear();
    while (std::getline(in, line)) {
        const size_t t = line.find('\t');
        if (t == std::string::npos || t == 0) continue;
        Entry e;
        e.id = line.substr(0, t);
        int g = 1, cs = 0, df = 0;
        double c = 0, gm = 0;
        if (std::sscanf(line.c_str() + t, "\t%d\t%lf\t%lf\t%d\t%d", &g, &c, &gm, &cs, &df) < 3) continue;
        e.gpu = g != 0; e.cpuMs = c; e.gpuMs = gm; e.cases = cs; e.differs = df != 0;
        out.push_back(e);
    }
    return true;
}

// The measurement proper. false: stopped (cancelled, Vulkan switched off, a card error); nothing is published then.
bool measureAll(std::vector<Entry>& out, FILE* log)
{
    const Clock::time_point t00 = Clock::now();
    const size_t stride = (size_t)kW * 4;
    std::vector<uint8_t> in(stride * kH), a(stride * kH), b(stride * kH);
    makeGpuTestPicture(kW, kH, false, true, false, in.data(), stride);
    for (const std::string& id : gpuEffectIds()) {
        if (gCancel.load() || msSince(t00) > kBudgetMs) return !gCancel.load() && gpu::enabled();
        const Def* d = findDef(id);
        if (!d || !d->gpu) continue;
        Entry e;
        e.id = id;
        bool allCpuWins = true;
        int cases = 0;
        double worstGpu = 0;
        char worstParts[160] = "";
        const std::vector<GpuCheckCase> all = gpuCheckCases(*d);
        for (size_t k = 0; k < all.size() && cases < kMaxCases; ++k) {
            if (gCancel.load() || !gpu::enabled()) return false;
            const GpuTestSettings set(*d, all[k]);
            Ctx c1, c2;
            set.apply(c1);
            set.apply(c2);
            c1.fps = c2.fps = 25;
            gpu::RunStats st;
            // the GPU first: a case its plan declines is not one the choice is about
            if (!renderFrameGpu(*d, c2, kW, kH, false, true, in.data(), stride, b.data(), stride, &st)) {
                if (lastGpuFailure() == "declined") continue;
                return false;
            }
            double gpuBest = 1e30;
            gpu::RunStats bestSt;
            // (best of four back to back: the first of a burst can find the card at its idle clocks)
            double bestPlan = 0;
            for (int r = 0; r < 4; ++r) {
                const Clock::time_point t0 = Clock::now();
                if (!renderFrameGpu(*d, c2, kW, kH, false, true, in.data(), stride, b.data(), stride, &st)) return false;
                const double wall = msSince(t0);
                if (wall < gpuBest) { gpuBest = wall; bestSt = st; bestPlan = std::max(0.0, wall - st.totalMs); }
            }
            if (gpuBest >= worstGpu) { worstGpu = gpuBest; std::snprintf(worstParts, sizeof worstParts, "plan %.1f fill %.1f up %.1f kernels %.1f down %.1f sync %.1f read %.1f", bestPlan, bestSt.fillMs, bestSt.copyUpMs, bestSt.gpuMs, bestSt.copyDownMs, std::max(0.0, bestSt.waitMs - bestSt.copyUpMs - bestSt.gpuMs - bestSt.copyDownMs), bestSt.readMs); }
            double cpuBest = 1e30;
            for (int r = 0; r < 3; ++r) {
                const Clock::time_point t0 = Clock::now();
                if (!renderFrameCpu(*d, c1, kW, kH, false, true, in.data(), stride, a.data(), stride)) { cpuBest = 1e30; break; }
                const double ms = msSince(t0);
                if (r > 0) cpuBest = std::min(cpuBest, ms);
                if (r == 0 && ms > 700) { cpuBest = ms; break; }        // (one slow run is enough to know)
            }
            ++cases;
            if (std::memcmp(a.data(), b.data(), stride * kH) != 0) e.differs = true;
            e.cpuMs = std::max(e.cpuMs, cpuBest);
            e.gpuMs = std::max(e.gpuMs, gpuBest);
            if (!(cpuBest * kCpuMustWinBy < gpuBest)) allCpuWins = false;
        }
        e.cases = cases;
        if (cases == 0) continue;
        e.gpu = !(allCpuWins || e.differs);
        if (log)
            std::fprintf(log, "%-52s CPU %8.2f ms  GPU %8.2f ms  -> %s%s    [GPU ms: %s]\n", id.c_str(), e.cpuMs, e.gpuMs, e.gpu ? "GPU" : "CPU", e.differs ? "  (GPU picture differs from the CPU's)" : "", worstParts);
        out.push_back(e);
    }
    return true;
}

// Common to both entry points. Returns the number of effects, -1 when there is nothing to measure on.
int doMeasure(const std::string& renoVersion, bool again, FILE* log)
{
    std::string why;
    if (!gpu::enabled() || !gpu::ready(&why)) return -1;
    const gpu::DeviceInfo dev = gpu::currentDevice();
    if (dev.software && !std::getenv("RENO_VK_ALLOW_SOFTWARE")) return -1;        // a CPU implementation: only tests use it, and the CPU code is always quicker
    const std::string key = makeKey(renoVersion);
    std::vector<Entry> es;
    if (!again && load(key, es)) {
        publish(es, key);
        if (log) std::fprintf(log, "kept result for %s (%d effects); use --again to measure again\n", key.c_str(), (int)es.size());
        return (int)es.size();
    }
    if (!measureAll(es, log)) return -1;
    publish(es, key);
    save(es, key);
    return (int)es.size();
}

}  // namespace

void setAuto(bool on) { gAuto = on; }
bool isAuto() { return gAuto.load(); }

bool useGpu(const Def& d)
{
    if (!gAuto.load()) return true;
    std::shared_ptr<const std::map<std::string, bool>> m;
    {
        std::lock_guard<std::mutex> lk(gMu);
        m = gChoice;
    }
    if (!m) return true;
    const auto it = m->find(d.id);
    return it == m->end() ? true : it->second;
}

void startBackground(const std::string& renoVersion, bool again)
{
    if (gMeasuring.exchange(true)) return;
    if (gThread.joinable()) gThread.join();
    gCancel = false;
    gGotAnswer = false;
    gThread = std::thread([renoVersion, again] {
        try { doMeasure(renoVersion, again, nullptr); } catch (...) {}
        gGotAnswer = true;
        gMeasuring = false;
    });
}

bool measuring() { return gMeasuring.load(); }
std::string statusLine() { return gMeasuring.load() && !gGotAnswer.load() ? "Checking the graphics card..." : std::string(); }

void join()
{
    gCancel = true;
    if (gThread.joinable()) gThread.join();
    gCancel = false;
}

int runNow(const std::string& renoVersion, bool again, void* log)
{
    join();
    gMeasuring = true;
    int n = -1;
    try { n = doMeasure(renoVersion, again, (FILE*)log); } catch (...) {}
    gMeasuring = false;
    return n;
}

std::vector<Entry> results()
{
    std::lock_guard<std::mutex> lk(gMu);
    return gEntries;
}

std::string deviceKey()
{
    std::lock_guard<std::mutex> lk(gMu);
    return gKey;
}

namespace {
// (the thread is stopped before the program's globals go)
struct Joiner { ~Joiner() { gCancel = true; if (gThread.joinable()) gThread.join(); } } gJoiner;
}

}  // namespace pick
}  // namespace builtin
}  // namespace ofx
}  // namespace reno
