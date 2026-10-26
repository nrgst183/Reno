#include "profile.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>

namespace reno {
namespace profile {

std::atomic<bool> g_on{false};

namespace {
std::mutex g_m;
std::map<std::string, PluginRow> g_plugins;
std::map<std::string, MediaRow> g_media;
std::set<std::pair<std::string, int64_t>> g_decoded;
uint64_t g_frames = 0;
double g_frameMs = 0;

typedef std::chrono::steady_clock Clock;
double msSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// the scopes open on this thread: the time of the ones inside each (taken off its self time)
struct Open { double childMs = 0; };
thread_local std::vector<Open*> t_stack;
int threadNumber()
{
    static std::atomic<int> next{0};
    static thread_local int mine = -1;
    if (mine < 0) mine = next.fetch_add(1);
    return mine;
}
}  // namespace

void setEnabled(bool on) { g_on.store(on); }

void reset()
{
    std::lock_guard<std::mutex> lk(g_m);
    g_plugins.clear();
    g_media.clear();
    g_decoded.clear();
    g_frames = 0;
    g_frameMs = 0;
}

PluginScope::PluginScope(const std::string& id, const std::string& label, bool scaled, int safety)
    : on_(enabled()), id_(&id), label_(&label), scaled_(scaled), safety_(safety)
{
    if (!on_) return;
    t0_ = Clock::now();
    t_stack.push_back(new Open());
}

PluginScope::~PluginScope()
{
    if (!on_) return;
    const double incl = msSince(t0_);
    Open* o = t_stack.back();
    t_stack.pop_back();
    const double self = std::max(0.0, incl - o->childMs);
    delete o;
    if (!t_stack.empty()) t_stack.back()->childMs += incl;
    std::lock_guard<std::mutex> lk(g_m);
    PluginRow& r = g_plugins[*id_];
    if (r.id.empty()) {
        r.id = *id_;
        r.label = *label_;
    }
    r.selfMs += self;
    r.inclMs += incl;
    ++r.calls;
    (scaled_ ? r.scaledCalls : r.fullCalls)++;
    r.safety = safety_;
}

void PluginScope::noteWait(const std::string& id, double ms)
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(g_m);
    PluginRow& r = g_plugins[id];
    if (r.id.empty()) r.id = id;
    r.waitMs += ms;
}

MediaScope::MediaScope(const std::string& path) : on_(enabled()), path_(&path)
{
    if (!on_) return;
    t0_ = Clock::now();
    t_stack.push_back(new Open());
}

MediaScope::~MediaScope()
{
    if (!on_) return;
    const double incl = msSince(t0_);
    Open* o = t_stack.back();
    t_stack.pop_back();
    const double self = std::max(0.0, incl - o->childMs);
    delete o;
    if (!t_stack.empty()) t_stack.back()->childMs += incl;
    std::lock_guard<std::mutex> lk(g_m);
    MediaRow& r = g_media[*path_];
    r.path = *path_;
    r.selfMs += self;
    ++r.calls;
    if (incl > 50.0) {
        ++r.slowReads;
        r.slowMs += incl;
    }
    r.worstMs = std::max(r.worstMs, incl);
    ++r.byThread[threadNumber()].second;
}

void noteDecode(const std::string& path, int64_t pts)
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(g_m);
    MediaRow& r = g_media[path];
    r.path = path;
    ++r.decodes;
    if (!g_decoded.insert({path, pts}).second) ++r.again;
    ++r.byThread[threadNumber()].first;
}

void noteSeek(const std::string& path, double ms)
{
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(g_m);
    MediaRow& r = g_media[path];
    r.path = path;
    r.seekMs += ms;
    ++r.seeks;
}

FrameScope::FrameScope() : on_(enabled())
{
    if (on_) t0_ = Clock::now();
}

FrameScope::~FrameScope()
{
    if (!on_) return;
    const double ms = msSince(t0_);
    std::lock_guard<std::mutex> lk(g_m);
    ++g_frames;
    g_frameMs += ms;
}

Report snapshot()
{
    Report r;
    std::lock_guard<std::mutex> lk(g_m);
    r.frames = g_frames;
    r.frameMs = g_frameMs;
    for (const auto& kv : g_plugins) {
        r.plugins.push_back(kv.second);
        r.pluginSelfMs += kv.second.selfMs;
    }
    for (const auto& kv : g_media) {
        r.media.push_back(kv.second);
        r.mediaSelfMs += kv.second.selfMs;
    }
    std::sort(r.plugins.begin(), r.plugins.end(), [](const PluginRow& a, const PluginRow& b) { return a.selfMs > b.selfMs; });
    std::sort(r.media.begin(), r.media.end(), [](const MediaRow& a, const MediaRow& b) { return a.selfMs > b.selfMs; });
    return r;
}

#if defined(__GNUC__) || defined(__clang__)
static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
#endif
static std::string fmt(const char* f, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

static std::string baseName(const std::string& p)
{
    const size_t k = p.find_last_of("/\\");
    return k == std::string::npos ? p : p.substr(k + 1);
}

std::string formatReport(const Report& r, double wallMs)
{
    std::string s;
    const double frames = (double)std::max<uint64_t>(1, r.frames);
    const double other = std::max(0.0, r.frameMs - r.pluginSelfMs - r.mediaSelfMs);
    s += fmt("Time per stage (summed over all frame threads: %.0f ms of work in %.0f ms elapsed)\n", r.frameMs, wallMs);
    s += fmt("  %-44s %10s %10s %6s\n", "", "total ms", "ms/frame", "share");
    auto row = [&](const char* name, double ms) {
        s += fmt("  %-44s %10.1f %10.2f %5.1f%%\n", name, ms, ms / frames, r.frameMs > 0 ? 100.0 * ms / r.frameMs : 0.0);
    };
    row("plug-ins (own time)", r.pluginSelfMs);
    row("media decoding", r.mediaSelfMs);
    row("compositing / other", other);
    if (r.pluginSelfMs + r.mediaSelfMs > r.frameMs * 1.02)
        s += "  (the first two rows can exceed the frame time because layers are decoded and rendered in parallel)\n";
    s += "\nPlug-ins, slowest first (own time, excluding the effects and files they pull from)\n";
    s += fmt("  %-28s %9s %7s %8s %9s %-13s %-22s %s\n", "plug-in", "total ms", "calls", "ms/call", "wait ms", "rendered at", "thread safety", "identifier");
    for (const PluginRow& p : r.plugins) {
        std::string name = p.label.empty() ? p.id : p.label;
        if (name.size() > 28) name = name.substr(0, 25) + "...";
        const char* at = p.scaledCalls && p.fullCalls ? "mixed" : p.scaledCalls ? "preview size" : "FULL size";
        const char* safe = p.safety == 0 ? "UNSAFE (one at a time)" : p.safety == 2 ? "fully safe" : "instance safe";
        s += fmt("  %-28s %9.1f %7llu %8.2f %9.1f %-13s %-22s %s\n", name.c_str(), p.selfMs, (unsigned long long)p.calls,
                 p.calls ? p.selfMs / (double)p.calls : 0.0, p.waitMs, at, safe, p.id.c_str());
    }
    if (r.plugins.empty()) s += "  (none rendered)\n";
    s += "\nMedia decoding, slowest first\n";
    s += fmt("  %-44s %9s %7s %8s %7s %9s %8s %9s %8s\n", "file", "total ms", "reads", "ms/read", "seeks", "seek ms", ">50 ms", "their ms", "worst");
    for (const MediaRow& m : r.media) {
        std::string name = baseName(m.path);
        if (name.size() > 44) name = name.substr(0, 41) + "...";
        s += fmt("  %-44s %9.1f %7llu %8.2f %7llu %9.1f %8llu %9.1f %8.1f\n", name.c_str(), m.selfMs, (unsigned long long)m.calls,
                 m.calls ? m.selfMs / (double)m.calls : 0.0, (unsigned long long)m.seeks, m.seekMs, (unsigned long long)m.slowReads, m.slowMs, m.worstMs);
    }
    if (r.media.empty()) s += "  (none)\n";
    s += "\nVideo frames decoded vs. reads, per file (and per thread: decoded/reads)\n";
    for (const MediaRow& m : r.media) {
        std::string name = baseName(m.path);
        if (name.size() > 44) name = name.substr(0, 41) + "...";
        s += fmt("  %-44s decoded %7llu (%llu of them again) for %7llu reads :", name.c_str(), (unsigned long long)m.decodes, (unsigned long long)m.again,
                 (unsigned long long)m.calls);
        for (const auto& t : m.byThread) s += fmt(" t%d %llu/%llu", t.first, (unsigned long long)t.second.first, (unsigned long long)t.second.second);
        s += "\n";
    }
    return s;
}

}  // namespace profile
}  // namespace reno
