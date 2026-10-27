// The mixer: a project's sound.
#include "engine.h"
#include "engine_internal.h"
#include "automation.h"
#include "stretch.h"
#include "../core/base.h"
#include "../media/vegasfiles.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <thread>
#include <condition_variable>
#include <deque>
#include <set>
#include <chrono>
#include <cstdio>

namespace reno {

namespace {

const double kPi = 3.14159265358979323846;
const int kReverseChunk = 32768;            // samples read at a time when playing backwards

int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// The pan law used everywhere (tracks, buses, master, events): the far side is turned down, the near side left alone.
float panLeft(double level, double pan) { return (float)(level * (pan > 0 ? 1.0 - pan : 1.0)); }
float panRight(double level, double pan) { return (float)(level * (pan < 0 ? 1.0 + pan : 1.0)); }

// ------------------------------------------------------------------------------------------ envelopes
// Envelopes are worked out for every sample (not once a block), so a ramp is a ramp and not a staircase that clicks.

// The envelope if it has something to say (it exists and has points), else null.
const Envelope* activeEnvelope(const Envelopes& list, const char* kind, uint64_t target = 0)
{
    const Envelope* e = findEnvelope(list, kind, target);
    return (e && !e->points.empty()) ? e : nullptr;
}

// The envelope's value at timeline samples s0, s0 + 1, ...
void envelopeSamples(const Envelope& env, int64_t s0, int rate, double origin, int n, double* out)
{
    const double t0 = (double)s0 / rate - origin, t1 = (double)(s0 + n) / rate - origin;
    const std::vector<EnvPoint>& pts = env.points;
    if (t0 >= pts.back().time || t1 <= pts.front().time) {
        std::fill(out, out + n, t0 >= pts.back().time ? pts.back().value : pts.front().value);
        return;
    }
    for (int k = 0; k < n; ++k) out[k] = env.valueAt((double)(s0 + k) / rate - origin, 0.0);
}

// An event's channel mapping (VEGAS's Channels: Both, Left Only, Right Only, Swap, Combine), in place.
void mapChannels(const std::string& channels, float* l, float* r, int n)
{
    if (channels.empty() || channels == "both") return;
    for (int k = 0; k < n; ++k) {
        const float a = l[k], b = r[k];
        if (channels == "left") r[k] = a;                       // the left channel on both sides
        else if (channels == "right") l[k] = b;
        else if (channels == "swap") { l[k] = b; r[k] = a; }
        else if (channels == "combine") l[k] = r[k] = 0.5f * (a + b);     // a mono mix (half each, so it cannot clip)
    }
}

// Which parameter of an audio plug-in an "fx" envelope moves: by its name, else (a number) by its index. -1: none.
int findParam(AudioPlugin* pl, const std::string& name)
{
    const int n = pl->paramCount();
    for (int i = 0; i < n; ++i)
        if (pl->paramName(i) == name) return i;
    if (!name.empty() && std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        const int i = std::atoi(name.c_str());
        if (i >= 0 && i < n) return i;
    }
    return -1;
}

// How often automated plug-in settings are brought up to date (samples): ~1.3 ms at 48 kHz.
const int kAutomationBlock = 64;

// The "fx" envelopes of 'envs' (the chain owner's) that move a parameter of the effect in 'slot', with the
// parameter's index in 'pl'.
using FxAutomation = std::vector<std::pair<const Envelope*, int>>;
FxAutomation fxAutomationOf(AudioPlugin* pl, const Envelopes& envs, const FxSlot& slot)
{
    FxAutomation autos;
    if (slot.bypassAutomation) return autos;
    for (const Envelope& e : envs)
        if (e.kind == "fx" && e.target == slot.id && !e.points.empty()) {
            const int idx = findParam(pl, e.param);
            if (idx >= 0) autos.emplace_back(&e, idx);
        }
    return autos;
}

// Brings the automated settings of 'pl' up to the envelopes' values at timeline sample 'sample'.
void setAutomatedParams(AudioPlugin* pl, const FxAutomation& autos, int64_t sample, int rate)
{
    for (const auto& a : autos)
        pl->paramSet(a.second, (float)std::min(1.0, std::max(0.0, a.first->valueAt((double)sample / rate, 0.0))));
}

// Reads media samples [idx, idx + n) at 'rate' in order, going round when the event loops ('period' > 0).
bool readForward(MediaPool& pool, const Event& e, const MediaItem& m, int rate, int64_t period, int64_t idx, int n, float* l, float* r)
{
    int done = 0;
    bool got = false;
    while (done < n) {
        int chunk = n - done;
        int64_t at = idx + done;
        if (period > 0) {
            at %= period;
            if (at < 0) at += period;
            chunk = (int)std::min<int64_t>(chunk, period - at);
        }
        if (pool.audio(e.id, m, at, chunk, rate, l + done, r + done, e.mediaChannel, e.mediaChannels)) got = true;
        done += chunk;
    }
    return got;
}

}  // namespace

// What an event needs carried from one render() to the next.
struct AudioEngine::EventState {
    // what it was made for: anything different and it starts again
    uint64_t media = 0;
    int pickFirst = -1, pickCount = 0;      // the media's channels it reads
    int readRate = 0;
    double offset = 0, rate = 0, length = 0, start = 0, pitch = 0, formant = 0;
    bool reverse = false, loop = false, keepPitch = false, preserveFormants = false;
    std::string method, mode;
    uint64_t lastUsed = 0;
    int64_t pdcNext = INT64_MIN;            // plug-in delay compensation

    // playing backwards: a stretch of the media read forwards (cheap) and handed out from its end
    std::vector<float> cl, cr;
    int64_t cacheStart = 0;
    bool cacheOk = false, cacheGot = false;

    // the cached sound this event is played from, if it is a kind that is cached: the entry, the media it was
    // made for and when that was last looked at
    std::weak_ptr<AudioEngine::CachedEvent> cached;
    std::string cacheKey;
    std::string mediaPath;
    int64_t mediaStampAt = 0;
    std::chrono::steady_clock::time_point stampChecked;
    int cacheKind = 0;              // 0 not cached, 1 stretched, 2 resampled

    // time-stretching: carries on only when asked for the very next sample
    std::unique_ptr<TimeStretcher> stretch;
    int64_t nextOut = 0;            // the timeline sample the stretcher would produce next
    int64_t origin = 0;             // the media sample its input index 0 is
    bool got = false;

    bool same(const Event& e, int r) const
    {
        return media == e.mediaId && pickFirst == e.mediaChannel && pickCount == e.mediaChannels && readRate == r && offset == e.offset && rate == e.rate && length == e.length && start == e.start &&
               pitch == e.pitch && reverse == e.reverse && loop == e.loop && keepPitch == e.keepPitch && method == e.stretchMethod &&
               mode == e.stretchMode && formant == e.formant && preserveFormants == e.preserveFormants;
    }
    void reset(const Event& e, int r)
    {
        media = e.mediaId;
        pickFirst = e.mediaChannel;
        pickCount = e.mediaChannels;
        readRate = r;
        offset = e.offset;
        rate = e.rate;
        length = e.length;
        start = e.start;
        pitch = e.pitch;
        reverse = e.reverse;
        loop = e.loop;
        keepPitch = e.keepPitch;
        method = e.stretchMethod;
        mode = e.stretchMode;
        formant = e.formant;
        preserveFormants = e.preserveFormants;
        cacheOk = false;
        stretch.reset();
        got = false;
        cached.reset();
        cacheKey.clear();
        mediaPath.clear();
        cacheKind = 0;
    }

    // Media samples idx, idx - 1, ... (n of them) into l and r: the media played backwards.
    bool readBackward(MediaPool& pool, const Event& e, const MediaItem& m, int64_t period, int64_t idx, int n, float* l, float* r)
    {
        bool any = false;
        int k = 0;
        while (k < n) {
            int64_t w = idx - k;
            if (period > 0) {
                w %= period;
                if (w < 0) w += period;
            }
            if (!cacheOk || w < cacheStart || w >= cacheStart + kReverseChunk) {
                // whole blocks on a fixed grid, so the same sample always comes from the same read
                if (cl.size() < (size_t)kReverseChunk) {
                    cl.resize((size_t)kReverseChunk);
                    cr.resize((size_t)kReverseChunk);
                }
                cacheStart = floorDiv(w, kReverseChunk) * kReverseChunk;
                cacheGot = pool.audio(e.id, m, cacheStart, kReverseChunk, readRate, cl.data(), cr.data(), e.mediaChannel, e.mediaChannels);
                cacheOk = true;
            }
            const int run = (int)std::min<int64_t>(n - k, w - cacheStart + 1);
            const float* sl = cl.data() + (w - cacheStart);
            const float* sr = cr.data() + (w - cacheStart);
            for (int q = 0; q < run; ++q) {
                l[k + q] = sl[-q];
                r[k + q] = sr[-q];
            }
            any = any || cacheGot;
            k += run;
        }
        return any;
    }
};

struct AudioEngine::BusMix {
    std::vector<float> l, r;
    int out = -1;                   // index of the bus it goes to
    int depth = 0;                  // how many buses lie between it and the master
    bool leadsToSolo = false;       // it is soloed, or goes on into a soloed bus
};

// ====================================================================== cached events
// An event whose sound is costly to make - time-stretched or pitch-shifted.
struct AudioEngine::EventPlan {
    bool none = false, stretched = false, inside = false, formants = false;
    bool resampledPlain = false;        // plain tape-like event of a file whose rate is not the project's
    double pitchFactor = 1.0;
    int rate = 0, readRate = 0;
    int64_t period = 0, firstSample = 0, total = 0;
    int cacheKind = 0;                  // 0 none, 1 stretched, 2 resampled
};

namespace {
const int64_t kCatchUpSeconds = 8;          // a stretched event is made up to here from where it has got to
const int64_t kMaxResampledSeconds = 30;    // longer plain events are not cached
const int kFillStep = 4096;

AudioEngine::EventPlan planOf(const Event& e, const MediaItem& m, int rate)
{
    AudioEngine::EventPlan pl;
    pl.rate = rate;
    pl.none = e.stretchMethod == "none";
    pl.pitchFactor = pl.none ? e.rate : (e.keepPitch ? 1.0 : e.rate) * std::pow(2.0, e.pitch / 12.0);
    pl.formants = !pl.none && stretchKeepsFormants(e.stretchMethod, e.stretchMode, e.preserveFormants) &&
                  (std::fabs(pl.pitchFactor - 1.0) > 1e-9 || (e.stretchMode == "pro" && std::fabs(e.formant) > 1e-9));
    pl.stretched = !pl.none && (std::fabs(pl.pitchFactor - e.rate) > 1e-9 || pl.formants);
    pl.inside = pl.stretched && stretcherShiftsPitch(e.stretchMethod, e.stretchMode, e.preserveFormants);
    pl.readRate = pl.inside ? rate : std::max(1000, (int)std::llround(rate / pl.pitchFactor));
    pl.period = (e.loop && m.info.duration > 0) ? std::max<int64_t>(1, (int64_t)std::llround(m.info.duration * pl.readRate)) : 0;
    pl.firstSample = timeToSample(e.start, rate);
    pl.total = std::max<int64_t>(0, timeToSample(e.end(), rate) - pl.firstSample);
    pl.resampledPlain = !e.reverse && !pl.stretched && std::fabs(e.rate - 1.0) < 1e-12 && pl.readRate == rate && m.info.sampleRate > 0 &&
                        m.info.sampleRate != rate && !m.generated && !m.info.isStill;
    if (!m.generated && !m.path.empty() && !m.offline && m.info.hasAudio) {
        if (pl.stretched) pl.cacheKind = 1;
        else if (pl.resampledPlain && pl.total <= kMaxResampledSeconds * (int64_t)rate) pl.cacheKind = 2;
    }
    return pl;
}


// How much later VEGAS's élastique sound comes than Reno's stretcher makes it when the event's pitch is shifted
// (StretchParams::delay): none for a rate alone, None and Classic.
double pitchDelaySamples(const Event& e, const AudioEngine::EventPlan& pl)
{
    if (!pl.stretched || e.stretchMethod != "elastique") return 0.0;
    return elastiqueDelaySamples(e.stretchMode, e.pitch);
}

struct EventCacheStore {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<AudioEngine::CachedEvent>> map;
    std::map<std::string, std::pair<std::chrono::steady_clock::time_point, int64_t>> stamps;
    std::atomic<size_t> bytes{0};
    std::atomic<size_t> limit{(size_t)256 << 20};
    std::atomic<uint64_t> tick{0};
};
EventCacheStore& eventStore()
{
    static EventCacheStore* s = new EventCacheStore();
    return *s;
}

// mediaStamp(path), asked for again only every couple of seconds
int64_t cachedStamp(const std::string& path)
{
    EventCacheStore& st = eventStore();
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(st.mutex);
        auto it = st.stamps.find(path);
        if (it != st.stamps.end() && now - it->second.first < std::chrono::seconds(2)) return it->second.second;
    }
    const int64_t v = mediaStamp(path);
    std::lock_guard<std::mutex> lk(st.mutex);
    st.stamps[path] = {now, v};
    return v;
}

std::string cacheKeyOf(const Event& e, const MediaItem& m, const AudioEngine::EventPlan& pl, int64_t stamp)
{
    char buf[480];
    std::snprintf(buf, sizeof buf, "|%lld|%d|%d|%d|%lld|%.17g|%.17g|%.17g|%d%d%d|%s|%s|%.17g|%d|%d|%d|%d|%.17g|%d", (long long)stamp, pl.cacheKind, pl.rate, pl.readRate,
                  (long long)pl.total, e.offset, e.rate, e.pitch, e.keepPitch ? 1 : 0, e.reverse ? 1 : 0, e.loop ? 1 : 0, e.stretchMethod.c_str(),
                  e.stretchMode.c_str(), e.formant, e.preserveFormants ? 1 : 0, e.mediaChannel, e.mediaChannels, m.info.sampleRate, m.info.duration,
                  m.info.channels);
    return m.path + buf;
}
}  // namespace

struct AudioEngine::CachedEvent {
    std::mutex mutex;
    EventPlan plan;
    Event e;
    MediaItem m;
    EventState st;                          // the stretcher, and a reversed event's reading
    std::vector<float> l, r;                // the sound made so far: event samples [0, filled)
    std::atomic<int64_t> filled{0};
    bool got = false;
    std::atomic<uint64_t> lastUse{0};
    bool started = false;
    ~CachedEvent() { eventStore().bytes.fetch_sub((size_t)filled.load() * 2 * sizeof(float)); }
};

namespace {
void trimEventCache()
{
    EventCacheStore& st = eventStore();
    if (st.bytes.load() <= st.limit.load()) return;
    std::lock_guard<std::mutex> lk(st.mutex);
    std::vector<std::pair<uint64_t, std::string>> order;
    for (const auto& kv : st.map) order.emplace_back(kv.second->lastUse.load(), kv.first);
    std::sort(order.begin(), order.end());
    const size_t goal = st.limit.load() / 10 * 8;
    for (const auto& o : order) {
        if (st.bytes.load() <= goal) break;
        auto it = st.map.find(o.second);
        if (it != st.map.end()) st.map.erase(it);
    }
}

std::shared_ptr<AudioEngine::CachedEvent> findOrMakeCached(const std::string& key, const Event& e, const MediaItem& m, const AudioEngine::EventPlan& pl,
                                                          const std::function<void(AudioEngine::CachedEvent&)>& init)
{
    EventCacheStore& st = eventStore();
    std::lock_guard<std::mutex> lk(st.mutex);
    auto it = st.map.find(key);
    if (it != st.map.end()) {
        it->second->lastUse = ++st.tick;
        return it->second;
    }
    auto c = std::make_shared<AudioEngine::CachedEvent>();
    c->plan = pl;
    c->e.id = e.id;
    c->e.mediaId = e.mediaId;
    c->e.start = e.start;
    c->e.length = e.length;
    c->e.offset = e.offset;
    c->e.rate = e.rate;
    c->e.loop = e.loop;
    c->e.reverse = e.reverse;
    c->e.pitch = e.pitch;
    c->e.keepPitch = e.keepPitch;
    c->e.stretchMethod = e.stretchMethod;
    c->e.stretchMode = e.stretchMode;
    c->e.formant = e.formant;
    c->e.preserveFormants = e.preserveFormants;
    c->e.mediaChannel = e.mediaChannel;
    c->e.mediaChannels = e.mediaChannels;
    c->m = m;
    c->st.reset(c->e, pl.readRate);
    c->lastUse = ++st.tick;
    init(*c);
    st.map[key] = c;
    return c;
}
}  // namespace

void AudioEngine::setEventCacheLimit(size_t bytes)
{
    eventStore().limit = bytes;
    if (bytes == 0) clearEventCache();
}
size_t AudioEngine::eventCacheBytes() { return eventStore().bytes.load(); }
void AudioEngine::clearEventCache()
{
    EventCacheStore& st = eventStore();
    std::lock_guard<std::mutex> lk(st.mutex);
    st.map.clear();
}

// Makes the cached event's sound up to event sample 'upTo', a step at a time.
void AudioEngine::fillCached(CachedEvent& c, int64_t upTo, const std::atomic<bool>* stop, bool oneStep)
{
    upTo = std::min(upTo, c.plan.total);
    for (;;) {
        if (stop && stop->load()) return;
        std::unique_lock<std::mutex> lk(c.mutex);
        const int64_t at = c.filled.load();
        if (at >= upTo) return;
        const int n = (int)std::min<int64_t>(oneStep ? (int64_t)1 << 30 : (int64_t)kFillStep, upTo - at);
        c.l.resize((size_t)(at + n));
        c.r.resize((size_t)(at + n));
        float* l = c.l.data() + at;
        float* r = c.r.data() + at;
        std::fill(l, l + n, 0.f);
        std::fill(r, r + n, 0.f);
        const EventPlan& pl = c.plan;
        if (pl.cacheKind == 2) {
            if (readResampledEvent(c.e, c.m, pl.rate, at, n, l, r)) c.got = true;
        } else {
            if (!c.started) {
                // the stretcher starts at the event's first sample, as it does for an event played from its start
                StretchParams sp;
                sp.method = c.e.stretchMethod;
                sp.mode = c.e.stretchMode;
                sp.rate = pl.rate;
                sp.alpha = (double)pl.rate / ((double)pl.readRate * c.e.rate);
                sp.pitch = pl.inside ? pl.pitchFactor : 1.0;
                sp.resampled = pl.inside ? 1.0 : (double)pl.rate / pl.readRate;
                sp.preserveFormants = pl.formants;
                sp.formant = c.e.formant;
                sp.delay = pitchDelaySamples(c.e, pl);
                c.st.stretch = makeStretcher(sp);
                const double from = mediaSampleTime(c.e.offset, c.m);
                const double mt = c.e.reverse ? c.e.offset + c.e.length * c.e.rate : from;
                c.st.origin = (int64_t)std::llround(mt * pl.readRate) - (c.e.reverse ? 1 : 0);
                c.st.got = false;
                c.started = true;
            }
            EventState& st = c.st;
            const StretchPull pull = [&](int64_t i, int k, float* a, float* b) {
                bool ok;
                if (c.e.reverse) ok = st.readBackward(pool_, c.e, c.m, pl.period, st.origin - i, k, a, b);
                else ok = readForward(pool_, c.e, c.m, pl.readRate, pl.period, st.origin + i, k, a, b);
                st.got = st.got || ok;
            };
            st.stretch->process(l, r, n, pull);
            c.got = st.got;
        }
        c.filled.store(at + n);
        eventStore().bytes.fetch_add((size_t)n * 2 * sizeof(float));
        lk.unlock();
        trimEventCache();
    }
}

int AudioEngine::cachedEventSound(EventState& st, const Event& e, const MediaItem& m, const EventPlan& pl, int64_t rel, int n, float* l, float* r)
{
    if (eventStore().limit.load() == 0 || pl.cacheKind == 0 || n <= 0 || rel < 0 || rel + n > pl.total) return -1;
    const auto now = std::chrono::steady_clock::now();
    if (st.cacheKind != pl.cacheKind || now - st.stampChecked > std::chrono::seconds(2)) {
        const int64_t stamp = cachedStamp(m.path);
        st.stampChecked = now;
        if (stamp == 0) {
            st.cacheKind = 0;
            return -1;
        }
        if (st.cacheKind != pl.cacheKind || stamp != st.mediaStampAt) {
            st.mediaStampAt = stamp;
            st.cacheKind = pl.cacheKind;
            st.cacheKey = cacheKeyOf(e, m, pl, stamp);
            st.cached.reset();
        }
    }
    if (st.cacheKind == 0) return -1;
    std::shared_ptr<CachedEvent> c = st.cached.lock();
    if (!c) {
        c = findOrMakeCached(st.cacheKey, e, m, pl, [](CachedEvent&) {});
        st.cached = c;
    }
    c->lastUse = ++eventStore().tick;
    if (pl.cacheKind == 1 && rel > c->filled.load() + kCatchUpSeconds * (int64_t)pl.rate) return -1;
    // A plain event of a file at another rate is worked out from any place on its own.
    if (pl.cacheKind == 2 && rel > c->filled.load()) return -1;
    fillCached(*c, rel + n, nullptr, pl.cacheKind == 2);
    std::lock_guard<std::mutex> lk(c->mutex);
    if (c->filled.load() < rel + n) return -1;
    std::memcpy(l, c->l.data() + rel, (size_t)n * sizeof(float));
    std::memcpy(r, c->r.data() + rel, (size_t)n * sizeof(float));
    return c->got ? 1 : 0;
}

// ---- the worker that makes upcoming events ahead of the mixer
struct AudioEngine::Prefetcher {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::pair<std::shared_ptr<CachedEvent>, int64_t>> jobs;
    std::set<CachedEvent*> queued;
    std::atomic<bool> stop{false};
    std::thread thread;
};

AudioEngine::AudioEngine(MediaPool& pool, FxRuntime& fx) : pool_(pool), fx_(fx) {}

AudioEngine::~AudioEngine()
{
    if (prefetcher_) {
        {
            std::lock_guard<std::mutex> lk(prefetcher_->mutex);
            prefetcher_->stop = true;
            prefetcher_->jobs.clear();
        }
        prefetcher_->cv.notify_all();
        if (prefetcher_->thread.joinable()) prefetcher_->thread.join();
    }
}

void AudioEngine::prefetch(const Project& p, int64_t from, int64_t frames)
{
    if (frames <= 0 || eventStore().limit.load() == 0) return;
    const int rate = p.settings.sampleRate;
    if (rate <= 0) return;
    const double t0 = (double)from / rate, t1 = (double)(from + frames) / rate;
    struct Job { std::shared_ptr<CachedEvent> c; int64_t upTo; };
    std::vector<Job> todo;
    for (const auto& tr : p.tracks) {
        if (tr.type != TrackType::Audio || tr.mute) continue;
        for (const auto& e : tr.events) {
            if (e.mute || e.end() <= t0 || e.start >= t1 || e.rate <= 0) continue;
            const MediaItem* m = p.findMedia(e.mediaId);
            if (!m || m->generated || m->offline || !m->info.hasAudio) continue;
            const EventPlan pl = planOf(e, *m, rate);
            if (pl.cacheKind == 0 || pl.total <= 0) continue;
            const int64_t stamp = cachedStamp(m->path);
            if (stamp == 0) continue;
            const int64_t rel0 = std::max<int64_t>(0, from - pl.firstSample);
            const int64_t relEnd = std::min(pl.total, from + frames - pl.firstSample);
            if (relEnd <= rel0) continue;
            std::shared_ptr<CachedEvent> c = findOrMakeCached(cacheKeyOf(e, *m, pl, stamp), e, *m, pl, [](CachedEvent&) {});
            if (c->filled.load() >= relEnd) continue;
            if (pl.cacheKind == 1 && rel0 > c->filled.load() + kCatchUpSeconds * (int64_t)rate) continue;
            todo.push_back(Job{c, relEnd});
        }
    }
    if (todo.empty()) return;
    std::lock_guard<std::mutex> once(prefetchMutex_);
    if (!prefetcher_) prefetcher_.reset(new Prefetcher());
    Prefetcher& pf = *prefetcher_;
    {
        std::lock_guard<std::mutex> lk(pf.mutex);
        for (const Job& j : todo)
            if (pf.queued.insert(j.c.get()).second) pf.jobs.emplace_back(j.c, j.upTo);
        if (!pf.thread.joinable() && !pf.stop.load()) {
            pf.thread = std::thread([this, &pf] {
                lowerThreadPriority();
                for (;;) {
                    std::pair<std::shared_ptr<CachedEvent>, int64_t> job;
                    {
                        std::unique_lock<std::mutex> lk2(pf.mutex);
                        pf.cv.wait(lk2, [&] { return pf.stop.load() || !pf.jobs.empty(); });
                        if (pf.stop.load()) return;
                        job = pf.jobs.front();
                        pf.jobs.pop_front();
                    }
                    fillCached(*job.first, job.second, &pf.stop);
                    std::lock_guard<std::mutex> lk2(pf.mutex);
                    pf.queued.erase(job.first.get());
                }
            });
        }
    }
    pf.cv.notify_one();
}

void AudioEngine::stripPeaks(std::vector<StripPeak>& out) const
{
    std::lock_guard<std::mutex> lk(peakMutex_);
    out.assign(sharedPeaks_.begin(), sharedPeaks_.end());
}

AudioEngine::EventState& AudioEngine::stateOf(const Event& e)
{
    std::lock_guard<std::mutex> lk(eventsMutex_);
    std::shared_ptr<EventState>& s = events_[e.id];
    if (!s) s = std::make_shared<EventState>();
    s->lastUsed = renders_;
    return *s;
}

bool AudioEngine::eventSound(const Project& p, const Event& e, const MediaItem& m, int64_t s, int n, float* l, float* r)
{
    const bool got = eventSoundRaw(p, e, m, s, n, l, r);
    if (got && e.invertPhase)
        for (int k = 0; k < n; ++k) {
            l[k] = -l[k];
            r[k] = -r[k];
        }
    return got;
}

// The event's sound for its samples [first, first + n).
bool AudioEngine::readResampledEvent(const Event& e, const MediaItem& m, int rate, int64_t first, int n, float* l, float* r)
{
    static constexpr int kTaps = 32;                        // the sinc's width in file samples
    static constexpr int kPhases = 2048;                    // fractions of a sample the weights are made for
    static const std::vector<float> table = []() -> std::vector<float> {
        std::vector<float> t((size_t)(kPhases + 1) * (size_t)kTaps);
        for (int f = 0; f <= kPhases; ++f)
            for (int q = 0; q < kTaps; ++q) {
                const double d = (double)(q - kTaps / 2 + 1) - (double)f / kPhases;        // file sample q
                const double x = d * 3.14159265358979323846;
                const double w = 0.5 + 0.5 * std::cos(3.14159265358979323846 * d / (kTaps / 2));
                t[(size_t)f * kTaps + (size_t)q] = (float)((std::fabs(d) < 1e-12 ? 1.0 : std::sin(x) / x) * w);
            }
        return t;
    }();
    const int fileRate = m.info.sampleRate;
    const double ratio = (double)fileRate / rate;
    const double shift = -0.25;
    const int64_t o = (int64_t)std::floor(e.offset * fileRate + 0.5 + 1e-7);       // the offset's own file sample
    // the file samples needed: floor(position) - 15 .. floor(position) + 16, none before the offset's (a cold start)
    const double p0 = (double)first * ratio + shift, p1 = (double)(first + n - 1) * ratio + shift;
    const int64_t lo = std::max<int64_t>(0, (int64_t)std::floor(p0) - kTaps / 2 + 1), hi = (int64_t)std::floor(p1) + kTaps / 2;
    const int cnt = (int)(hi - lo + 1);
    std::vector<float> xl((size_t)cnt), xr((size_t)cnt);
    const int64_t period = (e.loop && m.info.duration > 0) ? std::max<int64_t>(1, (int64_t)std::llround(m.info.duration * fileRate)) : 0;
    if (!readForward(pool_, e, m, fileRate, period, o + lo, cnt, xl.data(), xr.data())) return false;
    for (int i = 0; i < n; ++i) {
        const double p = (double)(first + i) * ratio + shift;
        const double fl = std::floor(p);
        const int ph = std::min(kPhases, (int)((p - fl) * kPhases + 0.5));
        const float* w = &table[(size_t)ph * kTaps];
        const int64_t j0 = (int64_t)fl - kTaps / 2 + 1;
        float al = 0, ar = 0;
        for (int q = 0; q < kTaps; ++q) {
            const int64_t j = j0 + q;
            if (j < 0) continue;                              // nothing before the offset
            const size_t at = (size_t)(j - lo);
            al += w[q] * xl[at];
            ar += w[q] * xr[at];
        }
        l[i] = al;
        r[i] = ar;
    }
    return true;
}


bool AudioEngine::eventSoundRaw(const Project& p, const Event& e, const MediaItem& m, int64_t s, int n, float* l, float* r)
{
    const int rate = p.settings.sampleRate;
    // The pitch the event should have: the rate's (as tape) unless it keeps its pitch, then the semitones on top.
    const EventPlan plan = planOf(e, m, rate);
    const bool stretched = plan.stretched;
    const bool inside = plan.inside;
    const bool formants = plan.formants;
    const double pitchFactor = plan.pitchFactor;
    const int readRate = plan.readRate;
    const int64_t period = plan.period;

    // Events that are costly to make play from the cache (see "cached events"); anything it cannot serve goes on as before.
    if (plan.cacheKind != 0 && eventStore().limit.load() != 0) {
        EventState& cs = stateOf(e);
        if (!cs.same(e, readRate) || cs.mediaPath != m.path) {
            cs.reset(e, readRate);
            cs.mediaPath = m.path;
        }
        const int got = cachedEventSound(cs, e, m, plan, s - plan.firstSample, n, l, r);
        if (got >= 0) return got > 0;
    }

    // As VEGAS: the event's first sample (timeToSample) plays the media from the event's offset rounded to a sample
    // of the file, and each sample after it one sample.
    const int64_t firstSample = timeToSample(e.start, rate);
    const double from = mediaSampleTime(e.offset, m);
    if (!e.reverse && !stretched) {
        // the usual case: tape-like, straight from the file
        const double mt = from + (double)(s - firstSample) / rate * e.rate;      // seconds into the media
        const int64_t pos = (int64_t)std::llround(mt * readRate);
        // A file whose rate is not the project's: VEGAS resamples each event from the file's own samples, starting at
        // the offset's sample with nothing before it.
        if (!e.reverse && std::fabs(e.rate - 1.0) < 1e-12 && readRate == rate && m.info.sampleRate > 0 && m.info.sampleRate != rate && !m.generated &&
            !m.info.isStill && n > 0)
            return readResampledEvent(e, m, rate, s - firstSample, n, l, r);
        return readForward(pool_, e, m, readRate, period, pos, n, l, r);
    }

    EventState& st = stateOf(e);
    if (!st.same(e, readRate)) st.reset(e, readRate);
    if (!stretched) {
        // backwards, sample for sample: timeline sample s plays media sample end - 1 - (s - first)
        const int64_t endPos = (int64_t)std::llround((e.offset + e.length * e.rate) * readRate);
        return st.readBackward(pool_, e, m, period, endPos - 1 - (s - firstSample), n, l, r);
    }

    if (!st.stretch || st.nextOut != s) {
        // a jump (or the first block): start the stretcher afresh here
        StretchParams sp;
        sp.method = e.stretchMethod;
        sp.mode = e.stretchMode;
        sp.rate = rate;
        sp.alpha = (double)rate / ((double)readRate * e.rate);
        sp.pitch = inside ? pitchFactor : 1.0;
        sp.resampled = inside ? 1.0 : (double)rate / readRate;
        sp.preserveFormants = formants;
        sp.formant = e.formant;
        sp.delay = pitchDelaySamples(e, plan);
        st.stretch = makeStretcher(sp);
        double mt;
        if (e.reverse) mt = e.offset + (e.length - (double)(s - firstSample) / rate) * e.rate;
        else mt = from + (double)(s - firstSample) / rate * e.rate;
        st.origin = (int64_t)std::llround(mt * readRate) - (e.reverse ? 1 : 0);
        st.got = false;
    }
    const StretchPull pull = [&](int64_t i, int k, float* a, float* b) {
        bool ok;
        if (e.reverse) ok = st.readBackward(pool_, e, m, period, st.origin - i, k, a, b);
        else ok = readForward(pool_, e, m, readRate, period, st.origin + i, k, a, b);
        st.got = st.got || ok;
    };
    st.stretch->process(l, r, n, pull);
    st.nextOut = s + n;
    return st.got;
}

// ====================================================================== 5.1 surround
// The panning law (Reno's own: VEGAS does not publish its own).
void surroundGains(const SurroundPan& sp, double x, double y, double center, float g[6])
{
    x = std::min(1.0, std::max(-1.0, std::isfinite(x) ? x : 0.0));
    y = std::min(1.0, std::max(-1.0, std::isfinite(y) ? y : 0.0));
    center = std::min(1.0, std::max(0.0, std::isfinite(center) ? center : 1.0));
    const double a = (x + 1.0) * kPi / 4.0, b = (y + 1.0) * kPi / 4.0;
    const double gl = std::cos(a), gr = std::sin(a), gf = std::sin(b), gb = std::cos(b);
    double l3 = 0, c3 = 0, r3 = 0;
    if (x <= 0) {
        const double f = (x + 1.0) * kPi / 2.0;
        l3 = std::cos(f);
        c3 = std::sin(f);
    } else {
        const double f = x * kPi / 2.0;
        c3 = std::cos(f);
        r3 = std::sin(f);
    }
    double pw[6];
    pw[ChL] = gf * gf * ((1.0 - center) * gl * gl + center * l3 * l3);
    pw[ChR] = gf * gf * ((1.0 - center) * gr * gr + center * r3 * r3);
    pw[ChC] = gf * gf * center * c3 * c3;
    pw[ChLFE] = 0;
    pw[ChLs] = gb * gb * gl * gl;
    pw[ChRs] = gb * gb * gr * gr;
    const double s = std::min(1.0, std::max(0.0, sp.smoothness));
    if (s < 1.0) {
        const double k = 1.0 + 7.0 * (1.0 - s);
        double before = 0, after = 0;
        for (int c = 0; c < 6; ++c) {
            before += pw[c];
            pw[c] = std::pow(pw[c], k);
            after += pw[c];
        }
        if (after > 1e-30)
            for (int c = 0; c < 6; ++c) pw[c] *= before / after;
    }
    for (int c = 0; c < 6; ++c) g[c] = (sp.speakerMutes & (1u << c)) ? 0.f : (float)std::sqrt(std::max(0.0, pw[c]));
    g[ChLFE] = (sp.lfe && !(sp.speakerMutes & (1u << ChLFE))) ? (float)std::max(0.0, sp.lfeLevel) : 0.f;
}

void surroundPointAt(const SurroundPan& sp, double t, double& x, double& y, double& center)
{
    x = sp.x;
    y = sp.y;
    center = sp.center;
    const std::vector<SurroundKey>& k = sp.keys;
    if (k.empty()) return;
    size_t i = 0;
    while (i + 1 < k.size() && k[i + 1].time <= t) ++i;
    const SurroundKey& a = k[i];
    if (t <= k.front().time || i + 1 >= k.size() || a.curve == 1) {
        const SurroundKey& h = t <= k.front().time ? k.front() : a;
        x = h.x;
        y = h.y;
        center = h.center;
        return;
    }
    const SurroundKey& b = k[i + 1];
    double f = b.time > a.time ? (t - a.time) / (b.time - a.time) : 1.0;
    if (a.curve == 2) f = f * f * (3.0 - 2.0 * f);
    x = a.x + (b.x - a.x) * f;
    y = a.y + (b.y - a.y) * f;
    center = a.center + (b.center - a.center) * f;
}

void surroundPlacePoint(SurroundPan& sp, double x, double y, double cursor, double frame)
{
    x = std::max(-1.0, std::min(1.0, x));
    y = std::max(-1.0, std::min(1.0, y));
    if (sp.keys.empty()) {
        if (sp.motion != "y") sp.x = x;
        if (sp.motion != "x") sp.y = y;
        return;
    }
    double cx, cy, cc;
    surroundPointAt(sp, cursor, cx, cy, cc);
    SurroundKey* at = nullptr;
    for (auto& k : sp.keys)
        if (std::fabs(k.time - cursor) <= frame * 0.5) at = &k;
    if (!at) {
        SurroundKey k;
        k.time = cursor;
        k.x = cx;
        k.y = cy;
        k.center = cc;
        sp.keys.push_back(k);
        std::stable_sort(sp.keys.begin(), sp.keys.end(), [](const SurroundKey& a, const SurroundKey& b) { return a.time < b.time; });
        for (auto& q : sp.keys)
            if (q.time == cursor) at = &q;
    }
    if (!at) return;
    if (sp.motion != "y") at->x = x;
    if (sp.motion != "x") at->y = y;
}

void surroundDownmix(const float* const six[6], int frames, float* left, float* right)
{
    const float h = 0.70710678f;
    for (int k = 0; k < frames; ++k) {
        const float c = six[ChC][k] * h;
        left[k] = six[ChL][k] + c + six[ChLs][k] * h;
        right[k] = six[ChR][k] + c + six[ChRs][k] * h;
    }
}

// A stereo sound into the six channels.
void AudioEngine::panSurround(const SurroundPan& sp, int64_t start, int rate, int frames, const float* l, const float* r, const float* gain,
                              float level, float* const* six)
{
    float g0[6], g1[6];
    double x0, y0, c0, x1, y1, c1;
    surroundPointAt(sp, (double)start / rate, x0, y0, c0);
    surroundPointAt(sp, (double)(start + frames) / rate, x1, y1, c1);
    const double fold0 = std::fabs(x0), fold1 = std::fabs(x1);
    if (sp.route != "surround") {
        for (int c = 0; c < 6; ++c) g0[c] = 0.f;
        if (sp.route == "front") g0[ChL] = g0[ChR] = 1.f;
        else if (sp.route == "rear") g0[ChLs] = g0[ChRs] = 1.f;
        else if (sp.route == "center") g0[ChC] = 1.f;
        else if (sp.route == "lfe") g0[ChLFE] = 1.f;
        for (int c = 0; c < 6; ++c)
            if (sp.speakerMutes & (1u << c)) g0[c] = 0.f;
        for (int c = 0; c < 6; ++c) g1[c] = g0[c];
    } else {
        surroundGains(sp, x0, y0, c0, g0);
        surroundGains(sp, x1, y1, c1, g1);
    }
    const bool fixed = sp.route != "surround";
    const bool moving = std::memcmp(g0, g1, sizeof(g0)) != 0 || fold0 != fold1;
    const float inv = frames > 0 ? 1.f / (float)frames : 0.f;
    for (int k = 0; k < frames; ++k) {
        const float f = moving ? (float)k * inv : 0.f;
        const float gg = gain ? gain[k] : level;
        const float a = l[k] * gg, b = r[k] * gg;
        const float mono = 0.5f * (a + b);
        float L = a, R = b;
        if (!fixed) {
            // folding: the far side's sound joins the near side as the point moves out to it
            const float fold = (float)(fold0 + (fold1 - fold0) * f);
            const bool leftSide = (x0 + x1) <= 0;
            if (leftSide) L = (a + fold * b) / (1.f + fold);
            else R = (b + fold * a) / (1.f + fold);
        } else if (sp.route == "front" || sp.route == "rear") {
            L = a;
            R = b;
        }
        auto G = [&](int c) { return moving ? g0[c] + (g1[c] - g0[c]) * f : g0[c]; };
        if (sp.route == "rear") {
            six[ChLs][k] += L * G(ChLs);
            six[ChRs][k] += R * G(ChRs);
            continue;
        }
        six[ChL][k] += L * G(ChL);
        six[ChR][k] += R * G(ChR);
        six[ChC][k] += 0.5f * (L + R) * G(ChC);
        six[ChLFE][k] += mono * G(ChLFE);
        six[ChLs][k] += L * G(ChLs);
        six[ChRs][k] += R * G(ChRs);
    }
}

// ====================================================================== sound
namespace {

// Scratch space for the envelopes' per-sample values (per thread: several engines may render at once).
struct EnvScratch {
    std::vector<double> a, b, c;
    std::vector<float> gl, gr;
    void size(int n)
    {
        if ((int)a.size() < n) {
            a.resize((size_t)n);
            b.resize((size_t)n);
            c.resize((size_t)n);
            gl.resize((size_t)n);
            gr.resize((size_t)n);
        }
    }
};
thread_local EnvScratch tScratch;

// A send of a track to a bus (its own, or one that a send envelope makes).
struct SendTo { uint64_t bus; double level; bool preFader; const Envelope* env; };

// Helper threads that mix tracks and buses in parallel.
class MixPool {
public:
    static MixPool& get()
    {
        static MixPool* p = new MixPool();
        return *p;
    }
    int helpers() const { return (int)threads_.size(); }
    // fn(0) .. fn(n-1), at most 'maxHelpers' other threads helping; returns when all have finished.
    void run(int n, int maxHelpers, const std::function<void(int)>& fn)
    {
        if (n <= 0) return;
        if (n == 1 || maxHelpers <= 0 || threads_.empty()) {
            for (int i = 0; i < n; ++i) fn(i);
            return;
        }
        auto job = std::make_shared<Job>();
        job->fn = &fn;
        job->n = n;
        job->helpersLeft = std::min(maxHelpers, n - 1);
        {
            std::lock_guard<std::mutex> lk(m_);
            jobs_.push_back(job);
        }
        if (job->helpersLeft >= (int)threads_.size()) cv_.notify_all();
        else
            for (int i = 0; i < job->helpersLeft; ++i) cv_.notify_one();
        work(*job);
        {
            std::lock_guard<std::mutex> lk(m_);
            for (auto it = jobs_.begin(); it != jobs_.end(); ++it)
                if (*it == job) {
                    jobs_.erase(it);
                    break;
                }
        }
        std::unique_lock<std::mutex> lk(job->m);
        job->cv.wait(lk, [&] { return job->done.load() >= job->n; });
    }

private:
    struct Job {
        const std::function<void(int)>* fn = nullptr;
        int n = 0;
        std::atomic<int> helpersLeft{0};
        std::atomic<int> next{0};
        std::atomic<int> done{0};
        std::mutex m;
        std::condition_variable cv;
    };
    MixPool()
    {
        const int want = processorCount() - 1;
        for (int i = 0; i < want; ++i) threads_.emplace_back([this] { loop(); });
        for (std::thread& t : threads_) t.detach();
    }
    static void work(Job& job)
    {
        for (;;) {
            const int i = job.next.fetch_add(1);
            if (i >= job.n) return;
            try {
                (*job.fn)(i);
            } catch (...) {
            }
            if (job.done.fetch_add(1) + 1 >= job.n) {
                std::lock_guard<std::mutex> lk(job.m);
                job.cv.notify_all();
            }
        }
    }
    void loop()
    {
        raiseThreadPriority();
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lk(m_);
                for (;;) {
                    for (const auto& j : jobs_)
                        if (j->next.load() < j->n && j->helpersLeft.fetch_sub(1) > 0) {
                            job = j;
                            break;
                        }
                    if (job) break;
                    cv_.wait(lk);
                }
            }
            work(*job);
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::vector<std::shared_ptr<Job>> jobs_;
    std::vector<std::thread> threads_;
};

}  // namespace

// What a track or a bus makes in a mix, kept apart until it is added in its turn (see mix()).
struct AudioEngine::Work {
    std::vector<float> tl, trb, el, er, panL, panR, sg, gainL, gainR, unmuted, tapL, tapR;
    std::vector<SendTo> sends;
    std::vector<const Event*> others;               // the events an event overlaps (overlappingEvents)
    // the sound it adds to a bus (dest >= 0) or the master (-1): the very products that are added
    struct Contrib {
        int dest = -1;
        std::vector<float> l, r;
    };
    std::vector<Contrib> contribs;
    size_t used = 0;
    StripPeak peak;
    const Track* track = nullptr;
    bool surroundDirect = false, hasSg = false;
    float level = 0;
    int64_t srcOut = 0;             // plug-in delay compensation
    enum BusMode { BusNormal, BusMuted, BusSurround } busMode = BusNormal;

    void begin(int frames, uint64_t id)
    {
        used = 0;
        peak = StripPeak();
        peak.id = id;
        surroundDirect = hasSg = false;
        srcOut = 0;
        busMode = BusNormal;
        track = nullptr;
        for (std::vector<float>* v : {&tl, &trb, &el, &er, &gainL, &gainR})
            if ((int)v->size() < frames) v->resize((size_t)frames);
    }
    Contrib& add(int dest, int frames)
    {
        if (used == contribs.size()) contribs.emplace_back();
        Contrib& c = contribs[used++];
        c.dest = dest;
        if ((int)c.l.size() < frames) {
            c.l.resize((size_t)frames);
            c.r.resize((size_t)frames);
        }
        return c;
    }
};

namespace {

// Per-sample left and right gains of a fader with volume, pan and mute envelopes (any of which may be null)
// over 'level' and 'pan', into gl and gr.
void faderGains(const Envelope* vol, const Envelope* pan, const Envelope* mute, double level, double basePan, int64_t start, int rate, int n,
                EnvScratch& s, float* gl, float* gr)
{
    if (vol) envelopeSamples(*vol, start, rate, 0.0, n, s.a.data());
    if (pan) envelopeSamples(*pan, start, rate, 0.0, n, s.b.data());
    if (mute) envelopeSamples(*mute, start, rate, 0.0, n, s.c.data());
    for (int k = 0; k < n; ++k) {
        double lev = level;
        if (vol) lev *= std::max(0.0, s.a[(size_t)k]);
        if (mute && s.c[(size_t)k] >= 0.5) lev = 0.0;
        double pp = basePan;
        if (pan) pp = std::min(1.0, std::max(-1.0, basePan + s.b[(size_t)k]));
        gl[k] = panLeft(lev, pp);
        gr[k] = panRight(lev, pp);
    }
}

}  // namespace

void AudioEngine::render(const Project& p, int64_t start, int frames, float* left, float* right, bool playing)
{
    if (!p.settings.surround()) {
        mix(p, start, frames, left, right, nullptr, playing);
        return;
    }
    // 5.1: the six channels, folded down to stereo
    float* six[6];
    for (int c = 0; c < 6; ++c) {
        if ((int)six_[c].size() < std::max(1, frames)) six_[c].resize((size_t)std::max(1, frames));
        six[c] = six_[c].data();
    }
    mix(p, start, frames, left, right, six, playing);
    if (frames > 0) surroundDownmix(six, frames, left, right);
}

void AudioEngine::renderChannels(const Project& p, int64_t start, int frames, float* const* out, bool playing)
{
    // in a 5.1 project 'left' and 'right' are only cleared, never mixed into: they may be the first two channels.
    mix(p, start, frames, out[0], out[1], p.settings.surround() ? out : nullptr, playing);
}

// The state of one AudioEngine::mix() call and its steps: set up the buses, mix the tracks, mix the buses, apply the
// master.
struct AudioBlockMixer {
    AudioEngine& eng;
    const Project& p;
    int64_t start;
    const int frames;
    float* const left;
    float* const right;
    float* const* const six;        // 5.1: the six channels, else null
    const bool playing;
    const int rate;
    double t0, t1;                  // the block's time span in seconds
    EnvScratch& es;                 // the calling thread's scratch
    std::vector<float>& gainL;      // per-sample master fader gains
    std::vector<float>& gainR;
    bool anySolo = false;
    int threads = 1;                // how many threads this render may use
    int width = 1;                  // the most tasks that ran at once
    std::vector<int> audioTracks;   // indices of the project's audio tracks
    std::atomic<uint64_t> leftOut{0};
    const bool primed;              // a block that only primes plug-in delay compensation
    // Plug-in delay compensation.
    int64_t lambda = 0;
    std::vector<int64_t> trackD, busIn, busLat, busOut;
    int64_t masterIn = 0, masterLat = 0;

    AudioBlockMixer(AudioEngine& e, const Project& proj, int64_t startSample, int n, float* l, float* r, float* const* sixCh, bool isPlaying,
                    bool prime = false)
        : eng(e), p(proj), start(startSample), frames(n), left(l), right(r), six(sixCh), playing(isPlaying),
          rate(proj.settings.sampleRate), t0((double)startSample / rate), t1((double)(startSample + n) / rate), es(tScratch),
          gainL(es.gl), gainR(es.gr), primed(prime)
    {
        es.size(frames);
        eng.peaks_.clear();
    }

    void run()
    {
        setupBuses();
        planDelays();
        if (lambda > 0 && !primed) {
            // the mix runs ahead by lambda; after a jump the paths are first filled with the lambda samples before
            start += lambda;
            t0 = (double)start / rate;
            t1 = (double)(start + frames) / rate;
            if (eng.pdcNext_ != start) prime();
        }
        threads = eng.parallelMix ? (eng.mixThreads > 0 ? eng.mixThreads : processorCount()) : 1;
        threads = std::min(threads, MixPool::get().helpers() + 1);
        mixTracks();
        mixBuses();
        eng.lastMixWidth = width;
        if (six) mixMaster51();
        else mixMasterStereo();
        if (!primed) {
            eng.pdcNext_ = lambda > 0 ? start + frames : INT64_MIN;
            eng.publishPeaks();
            eng.forgetIdleEvents();
        }
    }

    // ---- plug-in delay compensation

    // The latency of the effects in a chain, as runChainAt would run them.
    int chainLatency(const FxChain& chain) const
    {
        if (eng.bypassFx) return 0;
        int64_t sum = 0;
        for (const auto& slot : chain) {
            if (slot.kind != "audio" || slot.bypass) continue;
            AudioPlugin* pl = eng.fx_.audio(slot.id);
            if (pl) sum += std::max(0, pl->latencySamples());
        }
        return (int)std::min<int64_t>(sum, 1 << 24);
    }

    // Works out how late each path is.
    void planDelays()
    {
        lambda = 0;
        const int nb = (int)p.buses.size();
        trackD.assign(p.tracks.size(), 0);
        busIn.assign((size_t)nb, 0);
        busLat.assign((size_t)nb, 0);
        busOut.assign((size_t)nb, 0);
        masterIn = masterLat = 0;
        if (six) return;
        std::vector<int64_t> tl(p.tracks.size(), 0), bl((size_t)nb, 0);
        bool any = false;
        for (size_t i = 0; i < p.tracks.size(); ++i)
            if (p.tracks[i].type == TrackType::Audio && !p.tracks[i].events.empty()) any = (tl[i] = chainLatency(p.tracks[i].fx)) > 0 || any;
        for (int i = 0; i < nb; ++i) any = (bl[(size_t)i] = chainLatency(p.buses[(size_t)i].fx)) > 0 || any;
        const int64_t ml = chainLatency(p.audioOut);
        if (!any && ml <= 0) return;
        std::vector<int64_t> in((size_t)nb, -1);
        int64_t mIn = -1;
        auto feed = [&](int dest, int64_t d) {
            if (dest >= 0) in[(size_t)dest] = std::max(in[(size_t)dest], d);
            else mIn = std::max(mIn, d);
        };
        AudioEngine::Work tmp;
        for (size_t i = 0; i < p.tracks.size(); ++i) {
            const Track& tr = p.tracks[i];
            if (tr.type != TrackType::Audio || tr.events.empty()) continue;
            trackD[i] = tl[i];
            feed(busIndex(tr.output), tl[i]);
            collectSends(tr, tmp);
            for (const auto& sd : tmp.sends) {
                const int bi = busIndex(sd.bus);
                if (bi >= 0) feed(bi, tl[i]);
            }
        }
        for (int bi : eng.busOrder_) {
            if (in[(size_t)bi] < 0) continue;       // nothing goes into it
            busIn[(size_t)bi] = in[(size_t)bi];
            busLat[(size_t)bi] = bl[(size_t)bi];
            busOut[(size_t)bi] = in[(size_t)bi] + bl[(size_t)bi];
            feed(eng.buses_[(size_t)bi]->out, busOut[(size_t)bi]);
        }
        masterIn = std::max<int64_t>(0, mIn);
        masterLat = ml;
        lambda = masterIn + masterLat;
    }

    // Runs 'lambda' samples before 'start' through the whole mix, throwing the sound away, so that the delay lines and
    // the effects hold what they would after playing up to here.
    void prime()
    {
        const int chunk = 4096;
        eng.pdcL_.assign((size_t)chunk, 0.f);
        eng.pdcR_.assign((size_t)chunk, 0.f);
        for (int64_t pos = start - lambda; pos < start;) {
            const int n = (int)std::min<int64_t>(chunk, start - pos);
            std::fill(eng.pdcL_.begin(), eng.pdcL_.begin() + n, 0.f);
            std::fill(eng.pdcR_.begin(), eng.pdcR_.begin() + n, 0.f);
            AudioBlockMixer sub(eng, p, pos, n, eng.pdcL_.data(), eng.pdcR_.data(), nullptr, playing, true);
            sub.run();
            pos += n;
        }
        eng.peaks_.clear();
        setupBuses();
    }

    // Delays a contribution by d samples (keeping the tail for the next block).
    void delayContrib(AudioEngine::Work::Contrib& c, bool isBus, uint64_t id, size_t q, int64_t d)
    {
        AudioEngine::PdcLine& ln = eng.pdc_[{id | (isBus ? (1ULL << 63) : 0), q}];
        if (ln.l.size() != (size_t)d) {
            ln.l.assign((size_t)d, 0.f);
            ln.r.assign((size_t)d, 0.f);
            ln.pos = 0;
        } else if (ln.next != start) {
            std::fill(ln.l.begin(), ln.l.end(), 0.f);
            std::fill(ln.r.begin(), ln.r.end(), 0.f);
            ln.pos = 0;
        }
        ln.used = eng.renders_;
        size_t pos = ln.pos;
        const size_t sz = (size_t)d;
        for (int k = 0; k < frames; ++k) {
            const float xl = c.l[(size_t)k], xr = c.r[(size_t)k];
            c.l[(size_t)k] = ln.l[pos];
            c.r[(size_t)k] = ln.r[pos];
            ln.l[pos] = xl;
            ln.r[pos] = xr;
            if (++pos == sz) pos = 0;
        }
        ln.pos = pos;
        ln.next = start + frames;
    }

    // ---- effect chains

    // Runs an effect chain over a buffer holding timeline samples s0 .. s0 + count.
    void runChainAt(const FxChain& chain, const Envelopes* envs, float* l, float* r, int64_t s0, int count)
    {
        if (eng.bypassFx) return;
        const ProjectTempoScope tempoScope(p.settings.tempo, p.settings.beatsPerMeasure);    // for the effects with tempo sync
        for (const auto& slot : chain) {
            if (slot.kind != "audio" || slot.bypass) continue;
            AudioPlugin* pl = eng.fx_.audio(slot.id);
            if (!pl) continue;
            const FxAutomation autos = envs ? fxAutomationOf(pl, *envs, slot) : FxAutomation();
            const int step = autos.empty() ? FxRuntime::kAudioBlock : kAutomationBlock;
            for (int at = 0; at < count; at += step) {
                setAutomatedParams(pl, autos, s0 + at, rate);
                pl->process(l + at, r + at, std::min(step, count - at), s0 + at, playing);
            }
        }
    }
    void runChain(const FxChain& chain, const Envelopes* envs, float* l, float* r) { runChainAt(chain, envs, l, r, start, frames); }

    // ---- buses

    int busIndex(uint64_t id) const
    {
        if (id == 0) return -1;
        for (size_t i = 0; i < p.buses.size(); ++i)
            if (p.buses[i].id == id) return (int)i;
        return -1;
    }

    // Works out where each bus goes (a bus that would feed back into itself goes to the master instead) and in which
    // order they are mixed.
    void setupBuses()
    {
        const int nb = (int)p.buses.size();
        auto& buses = eng.buses_;
        while ((int)buses.size() < nb) buses.push_back(std::make_shared<AudioEngine::BusMix>());
        for (int i = 0; i < nb; ++i) {
            AudioEngine::BusMix& b = *buses[(size_t)i];
            if ((int)b.l.size() < frames) {
                b.l.resize((size_t)frames);
                b.r.resize((size_t)frames);
            }
            std::memset(b.l.data(), 0, (size_t)frames * sizeof(float));
            std::memset(b.r.data(), 0, (size_t)frames * sizeof(float));
            b.out = busIndex(p.buses[(size_t)i].output);
            int cur = b.out;
            for (int step = 0; cur >= 0 && step < nb; ++step) {
                if (cur == i) {
                    b.out = -1;
                    break;
                }
                cur = busIndex(p.buses[(size_t)cur].output);
            }
        }
        anySolo = p.anySolo(TrackType::Audio);
        for (int i = 0; i < nb; ++i) {
            AudioEngine::BusMix& b = *buses[(size_t)i];
            b.depth = 0;
            b.leadsToSolo = false;
            for (int cur = i; cur >= 0 && b.depth <= nb; cur = buses[(size_t)cur]->out) {
                if (p.buses[(size_t)cur].solo) b.leadsToSolo = true;
                ++b.depth;
            }
            if (p.buses[(size_t)i].solo) anySolo = true;
        }
        eng.busOrder_.clear();
        for (int i = 0; i < nb; ++i) eng.busOrder_.push_back(i);
        std::stable_sort(eng.busOrder_.begin(), eng.busOrder_.end(), [&](int a, int b) { return buses[(size_t)a]->depth > buses[(size_t)b]->depth; });
    }

    // ---- running tasks

    // A rough count of the work in a track (events heard in this block, effects), to decide whether handing it out is worth it.
    int64_t trackUnits(const Track& tr) const
    {
        int64_t u = 0;
        for (const auto& e : tr.events) {
            if (e.mute || e.end() <= t0 || e.start >= t1 || e.rate <= 0) continue;
            u += 1 + ((e.rate != 1.0 || e.pitch != 0.0 || e.formant != 0.0) ? 6 : 0);
            for (const auto& s : e.fx) u += s.kind == "audio" && !s.bypass ? 6 : 0;
        }
        for (const auto& s : tr.fx) u += s.kind == "audio" && !s.bypass ? 6 : 0;
        return u;
    }

    // Runs n tasks on the pool (and this thread).
    void runTogether(int n, const std::function<void(int)>& fn)
    {
        const NestedThreadState ns = nestedThreadState();
        MixPool::get().run(n, threads - 1, [&](int j) {
            const NestedThreadState mine = nestedThreadState();
            NestedThreadState st = ns;
            st.leftOut = 0;
            setNestedThreadState(st);
            fn(j);
            leftOut += nestedThreadState().leftOut;
            setNestedThreadState(mine);
        });
        NestedThreadState cur = nestedThreadState();
        cur.leftOut += leftOut.exchange(0);
        setNestedThreadState(cur);
    }

    void ensureWork(size_t n)
    {
        while (eng.work_.size() < n) eng.work_.push_back(std::make_shared<AudioEngine::Work>());
    }

    // ---- tracks

    // The sends of a track: its own, and a post-fader one at 0 dB for each send envelope to a bus it has no send for.
    void collectSends(const Track& tr, AudioEngine::Work& w) const
    {
        w.sends.clear();
        for (const auto& sd : tr.sends) w.sends.push_back(SendTo{sd.bus, sd.level, sd.preFader, trackEnvelope(tr, "send", sd.bus)});
        for (const Envelope& e : tr.envelopes)
            if (e.kind == "send" && !e.points.empty() && !tr.automationOff()) {
                bool have = false;
                for (const auto& sd : tr.sends) have = have || sd.bus == e.target;
                if (!have) w.sends.push_back(SendTo{e.target, 1.0, false, &e});
            }
    }

    // True if the track's sends reach some bus that is heard (solo as in VEGAS: with anything soloed, only soloed
    // tracks and buses that lead into a soloed bus are heard).
    bool anySendHeard(const Track& tr, const AudioEngine::Work& w) const
    {
        for (const auto& sd : w.sends) {
            const int bi = busIndex(sd.bus);
            if (bi >= 0 && sd.level > 0 && (!anySolo || tr.solo || eng.buses_[(size_t)bi]->leadsToSolo)) return true;
        }
        return false;
    }

    // Adds one event's part of this block.
    void addEventToTrack(const Track& tr, const Event& e, AudioEngine::Work& w, EnvScratch& scratch, int a, int n) const
    {
        // fades: the event's own, and the cross-fade with whatever it overlaps
        overlappingEvents(tr, e, w.others);
        bool overlaps = false;
        for (const Event* o : w.others)
            if (o->start < e.end() && o->end() > e.start) overlaps = true;
        // Automatic Crossfades off: overlapping sounds both play as they are
        if (!p.autoCrossfades) overlaps = false;
        // Quickfade Audio Edits: a short straight fade where the event starts or ends with no fade of its own and
        // nothing cross-fading with it there, so a cut does not click.
        double qIn = 0, qOut = 0;
        if (p.quickFade > 0) {
            const double q = std::min(p.quickFade, e.length * 0.5);
            bool overStart = false, overEnd = false;
            if (p.autoCrossfades)
                for (const auto& o : tr.events) {
                    if (o.id == e.id || o.mute || !(o.start < e.end() && o.end() > e.start)) continue;
                    if (o.start <= e.start && o.end() > e.start) overStart = true;
                    if (o.start < e.end() && o.end() >= e.end()) overEnd = true;
                }
            if (e.fadeIn <= 0 && !overStart) qIn = q;
            if (e.fadeOut <= 0 && !overEnd) qOut = q;
        }
        auto quick = [&](double t) {
            double f = 1.0;
            if (qIn > 0 && t - e.start < qIn) f = std::max(0.0, (t - e.start) / qIn);
            if (qOut > 0 && e.end() - t < qOut) f = std::min(f, std::max(0.0, (e.end() - t) / qOut));
            return f;
        };
        const bool fades = overlaps || e.fadeIn > 0 || e.fadeOut > 0 || qIn > 0 || qOut > 0;
        // Normalize: the measured gain on top of the event's own level
        const double level = e.gain * (e.normalize ? e.normalizeGain : 1.0);
        const float gain = (float)level;
        const Envelope* evVol = activeEnvelope(e.envelopes, "volume");
        const Envelope* evPan = activeEnvelope(e.envelopes, "pan");
        if (!evVol && !evPan) {
            for (int k = 0; k < n; ++k) {
                float g = gain;
                if (fades) {
                    const double t = (double)(start + a + k) / rate;
                    g *= (float)(eventFade(e, t) * quick(t));
                    if (overlaps) g *= (float)overlapWeightAmong(w.others, e, t);
                }
                w.tl[(size_t)(a + k)] += w.el[(size_t)k] * g;
                w.trb[(size_t)(a + k)] += w.er[(size_t)k] * g;
            }
            return;
        }
        // with the event's own volume and pan envelopes (their times count from the event's start)
        if (evVol) envelopeSamples(*evVol, start + a, rate, e.start, n, scratch.a.data());
        if (evPan) envelopeSamples(*evPan, start + a, rate, e.start, n, scratch.b.data());
        for (int k = 0; k < n; ++k) {
            double g = level;
            if (fades) {
                const double t = (double)(start + a + k) / rate;
                g *= eventFade(e, t) * quick(t);
                if (overlaps) g *= overlapWeightAmong(w.others, e, t);
            }
            if (evVol) g *= std::max(0.0, scratch.a[(size_t)k]);
            const double pp = evPan ? std::min(1.0, std::max(-1.0, scratch.b[(size_t)k])) : 0.0;
            w.tl[(size_t)(a + k)] += w.el[(size_t)k] * panLeft(g, pp);
            w.trb[(size_t)(a + k)] += w.er[(size_t)k] * panRight(g, pp);
        }
    }

    // Mixes the events of a track that sound in this block into w.tl / w.trb.
    bool sumTrackEvents(const Track& tr, AudioEngine::Work& w, EnvScratch& scratch)
    {
        bool any = false;
        for (const auto& e : tr.events) {
            if (e.mute || e.end() <= t0 || e.start >= t1 || e.rate <= 0) continue;
            const MediaItem* m = p.findMedia(e.mediaId);
            if (!m || m->generated || m->offline || !m->info.hasAudio) continue;
            // the part of this block the event covers (VEGAS's grid: the samples nearest the event's start and end)
            int a = (int)std::max<int64_t>(0, timeToSample(e.start, rate) - start);
            int b = (int)std::min<int64_t>(frames, timeToSample(e.end(), rate) - start);
            if (b <= a) continue;
            const int n = b - a;
            const int lat = e.fx.empty() ? 0 : chainLatency(e.fx);
            if (lat > 0) {
                // effects with a latency: the sound is read 'lat' samples ahead, so what comes out is the event's at
                // the right place.
                const int64_t endS = timeToSample(e.end(), rate);
                const int64_t at = start + a;
                if ((int)w.el.size() < std::max(n, lat)) {
                    w.el.resize((size_t)std::max(n, lat));
                    w.er.resize((size_t)std::max(n, lat));
                }
                auto readIn = [&](int64_t pos, int cnt) {
                    std::memset(w.el.data(), 0, (size_t)cnt * sizeof(float));
                    std::memset(w.er.data(), 0, (size_t)cnt * sizeof(float));
                    const int avail = (int)std::max<int64_t>(0, std::min<int64_t>(cnt, endS - pos));
                    if (avail <= 0) return true;
                    if (!eng.eventSound(p, e, *m, pos, avail, w.el.data(), w.er.data())) return false;
                    mapChannels(e.channels, w.el.data(), w.er.data(), avail);
                    return true;
                };
                AudioEngine::EventState& st = eng.stateOf(e);
                if (st.pdcNext != at + lat) {
                    readIn(at, lat);
                    runChainAt(e.fx, nullptr, w.el.data(), w.er.data(), at, lat);
                }
                st.pdcNext = at + lat + n;
                if (!readIn(at + lat, n)) continue;
                any = true;
                runChainAt(e.fx, nullptr, w.el.data(), w.er.data(), at + lat, n);
                addEventToTrack(tr, e, w, scratch, a, n);
                continue;
            }
            std::memset(w.el.data(), 0, (size_t)n * sizeof(float));
            std::memset(w.er.data(), 0, (size_t)n * sizeof(float));
            if (!eng.eventSound(p, e, *m, start + a, n, w.el.data(), w.er.data())) continue;
            any = true;
            mapChannels(e.channels, w.el.data(), w.er.data(), n);
            // the event's own effects (VEGAS's Audio Event FX), before its level, fades and envelopes
            if (!e.fx.empty()) runChainAt(e.fx, nullptr, w.el.data(), w.er.data(), start + a, n);
            addEventToTrack(tr, e, w, scratch, a, n);
        }
        return any;
    }

    // Auto Ducking: a listener track is lowered by the "duck" envelope of each controller track.
    void applyDucking(const Track& tr, AudioEngine::Work& w, EnvScratch& scratch, int64_t fs) const
    {
        if (!tr.duckListener) return;
        for (const auto& c : p.tracks) {
            if (!c.duckController || c.id == tr.id || c.type != TrackType::Audio) continue;
            const Envelope* d = activeEnvelope(c.envelopes, "duck");
            if (!d) continue;
            envelopeSamples(*d, fs, rate, 0.0, frames, scratch.a.data());
            for (int k = 0; k < frames; ++k) {
                const float g = (float)std::min(1.0, std::max(0.0, scratch.a[(size_t)k]));
                w.tl[(size_t)k] *= g;
                w.trb[(size_t)k] *= g;
            }
        }
    }

    // A track's fader after its effects: one gain per side (gl, gr) or, when it has volume, pan or mute envelopes,
    // one per sample (GL, GR).
    struct TrackFader {
        const Envelope* vol = nullptr;
        const Envelope* pan = nullptr;
        const Envelope* mute = nullptr;
        bool perSample = false;
        double level = 1.0;
        float gl = 1.f, gr = 1.f;
        const float* GL = nullptr;
        const float* GR = nullptr;
        const float* postL = nullptr;
        const float* postR = nullptr;
    };

    // Works out the track's fader.
    TrackFader makeTrackFader(const Track& tr, AudioEngine::Work& w, EnvScratch& scratch, int64_t fs) const
    {
        TrackFader f;
        // in Off automation mode the envelopes are left out; where there is none, the automation values apply
        f.vol = trackEnvelope(tr, "volume");
        f.pan = trackEnvelope(tr, "pan");
        f.mute = trackEnvelope(tr, "mute");
        f.perSample = f.vol || f.pan || f.mute;
        f.level = tr.level * (f.vol ? 1.0 : tr.autoLevel);
        const double trPan = std::min(1.0, std::max(-1.0, tr.pan + (f.pan ? 0.0 : tr.autoPan)));
        const bool matrixPan = !six && !tr.panType.empty() && tr.panType != "balance0";
        f.postL = w.tl.data();
        f.postR = w.trb.data();
        if (matrixPan) {
            w.panL.resize((size_t)frames);
            w.panR.resize((size_t)frames);
            if (f.pan) envelopeSamples(*f.pan, fs, rate, 0.0, frames, scratch.b.data());
            PanGains pg = panGains(tr.panType, 1.0, trPan);
            for (int k = 0; k < frames; ++k) {
                if (f.pan && (k == 0 || scratch.b[(size_t)k] != scratch.b[(size_t)k - 1]))
                    pg = panGains(tr.panType, 1.0, std::min(1.0, std::max(-1.0, trPan + scratch.b[(size_t)k])));
                const float l = w.tl[(size_t)k], r = w.trb[(size_t)k];
                w.panL[(size_t)k] = l * pg.ll + r * pg.rl;
                w.panR[(size_t)k] = l * pg.lr + r * pg.rr;
            }
            f.postL = w.panL.data();
            f.postR = w.panR.data();
        }
        f.gl = matrixPan ? (float)f.level : panLeft(f.level, trPan);
        f.gr = matrixPan ? (float)f.level : panRight(f.level, trPan);
        if (f.perSample) {
            faderGains(f.vol, matrixPan ? nullptr : f.pan, f.mute, f.level, matrixPan ? 0.0 : trPan, fs, rate, frames, scratch, w.gainL.data(), w.gainR.data());
            f.GL = w.gainL.data();
            f.GR = w.gainR.data();
        }
        return f;
    }

    // Sends: before the fader they take the sound as the effects leave it, after it as the fader leaves it.
    void sendTrackToBuses(const Track& tr, AudioEngine::Work& w, EnvScratch& scratch, const TrackFader& f, int64_t fs)
    {
        std::vector<float>& unmuted = w.unmuted;
        if (f.mute) {
            unmuted.resize((size_t)frames);
            envelopeSamples(*f.mute, fs, rate, 0.0, frames, scratch.c.data());
            for (int k = 0; k < frames; ++k) unmuted[(size_t)k] = scratch.c[(size_t)k] >= 0.5 ? 0.f : 1.f;
        }
        for (const auto& sd : w.sends) {
            const int bi = busIndex(sd.bus);
            if (bi < 0 || sd.level <= 0 || !(!anySolo || tr.solo || eng.buses_[(size_t)bi]->leadsToSolo)) continue;
            AudioEngine::Work::Contrib& c = w.add(bi, frames);
            // before the fader: the sound before the pan; after it: as panned
            const float* inL = sd.preFader ? w.tl.data() : f.postL;
            const float* inR = sd.preFader ? w.trb.data() : f.postR;
            if (!sd.env && !f.perSample) {
                const float sl = (float)sd.level * (sd.preFader ? 1.f : f.gl), sr = (float)sd.level * (sd.preFader ? 1.f : f.gr);
                for (int k = 0; k < frames; ++k) {
                    c.l[(size_t)k] = inL[k] * sl;
                    c.r[(size_t)k] = inR[k] * sr;
                }
                continue;
            }
            if (sd.env) envelopeSamples(*sd.env, fs, rate, 0.0, frames, scratch.a.data());
            for (int k = 0; k < frames; ++k) {
                float s = (float)sd.level;
                if (sd.env) s *= (float)std::max(0.0, scratch.a[(size_t)k]);
                float sl = s, sr = s;
                if (!sd.preFader) {
                    sl *= f.GL ? f.GL[k] : f.gl;
                    sr *= f.GR ? f.GR[k] : f.gr;
                } else if (f.mute) {
                    sl *= unmuted[(size_t)k];
                    sr *= unmuted[(size_t)k];
                }
                c.l[(size_t)k] = inL[k] * sl;
                c.r[(size_t)k] = inR[k] * sr;
            }
        }
    }

    // Mixes one audio track into its Work: the events, phase invert, effects, ducking, fader, sends and output.
    void doTrack(const Track& tr, AudioEngine::Work& w)
    {
        w.begin(frames, tr.id);
        w.srcOut = trackD[(size_t)(&tr - p.tracks.data())];
        const int64_t fs = start - w.srcOut;        // the fader and what follows the effects see the sound this much late
        EnvScratch& scratch = tScratch;
        scratch.size(frames);
        collectSends(tr, w);
        const int outBus = busIndex(tr.output);
        // the automation's mute applies where the track has no mute envelope, or its automation is Off
        const bool trMute = tr.mute || (tr.autoMute && !trackEnvelope(tr, "mute"));
        const bool direct = !trMute && (!anySolo || tr.solo || (outBus >= 0 && eng.buses_[(size_t)outBus]->leadsToSolo));
        const bool anySend = !trMute && anySendHeard(tr, w);
        if (!direct && !anySend) return;
        std::memset(w.tl.data(), 0, (size_t)frames * sizeof(float));
        std::memset(w.trb.data(), 0, (size_t)frames * sizeof(float));
        const bool any = sumTrackEvents(tr, w, scratch);
        if (!any && tr.fx.empty()) return;
        // the track's Invert Phase switch: its sound upside down, before its effects and fader
        if (tr.invertPhase)
            for (int k = 0; k < frames; ++k) {
                w.tl[(size_t)k] = -w.tl[(size_t)k];
                w.trb[(size_t)k] = -w.trb[(size_t)k];
            }
        runChain(tr.fx, &tr.envelopes, w.tl.data(), w.trb.data());
        applyDucking(tr, w, scratch, fs);
        const TrackFader f = makeTrackFader(tr, w, scratch, fs);
        if (anySend) sendTrackToBuses(tr, w, scratch, f, fs);
        float pl = 0, pr = 0;
        if (six && outBus < 0) {
            // 5.1, straight to the master: the fader without the stereo pan, then the surround panner (when it is put in)
            float* sg = nullptr;
            if (f.perSample) {
                w.sg.resize((size_t)frames);
                faderGains(f.vol, nullptr, f.mute, f.level, 0.0, fs, rate, frames, scratch, w.sg.data(), w.gainR.data());
                sg = w.sg.data();
            }
            w.surroundDirect = direct;
            w.hasSg = sg != nullptr;
            w.level = (float)f.level;
            w.track = &tr;
            for (int k = 0; k < frames; ++k) {
                const float g = sg ? sg[k] : (float)f.level;
                pl = std::max(pl, std::fabs(w.tl[(size_t)k] * g));
                pr = std::max(pr, std::fabs(w.trb[(size_t)k] * g));
            }
            w.peak.left = pl;
            w.peak.right = pr;
            return;
        }
        if (direct) {
            AudioEngine::Work::Contrib& c = w.add(outBus, frames);
            if (f.GL) {
                for (int k = 0; k < frames; ++k) {
                    c.l[(size_t)k] = f.postL[k] * f.GL[k];
                    c.r[(size_t)k] = f.postR[k] * f.GR[k];
                }
            } else {
                for (int k = 0; k < frames; ++k) {
                    c.l[(size_t)k] = f.postL[k] * f.gl;
                    c.r[(size_t)k] = f.postR[k] * f.gr;
                }
            }
        }
        for (int k = 0; k < frames; ++k) {
            pl = std::max(pl, std::fabs(f.postL[k] * (f.GL ? f.GL[k] : f.gl)));
            pr = std::max(pr, std::fabs(f.postR[k] * (f.GR ? f.GR[k] : f.gr)));
        }
        w.peak.left = pl;
        w.peak.right = pr;
    }

    // Adds a mixed track's (or bus's) contributions into the buses / master they go to.
    void addContribs(AudioEngine::Work& w, bool isBus)
    {
        for (size_t q = 0; q < w.used; ++q) {
            AudioEngine::Work::Contrib& c = w.contribs[q];
            if (lambda > 0) {
                const int64_t d = (c.dest >= 0 ? busIn[(size_t)c.dest] : masterIn) - w.srcOut;
                if (d > 0) delayContrib(c, isBus, w.peak.id, ((size_t)(c.dest + 1) << 16) | q, d);
            }
            float* dl = c.dest >= 0 ? eng.buses_[(size_t)c.dest]->l.data() : left;
            float* dr = c.dest >= 0 ? eng.buses_[(size_t)c.dest]->r.data() : right;
            for (int k = 0; k < frames; ++k) {
                dl[k] += c.l[(size_t)k];
                dr[k] += c.r[(size_t)k];
            }
        }
    }

    void commitTrack(AudioEngine::Work& w)
    {
        addContribs(w, false);
        if (w.surroundDirect) eng.panSurround(w.track->surround, start, rate, frames, w.tl.data(), w.trb.data(), w.hasSg ? w.sg.data() : nullptr, w.level, six);
        eng.peaks_.push_back(w.peak);
    }

    // True if two audio tracks use the same plug-in instance or event state (the same ids in a project made by hand
    // or by a faulty import).
    bool tracksShareState() const
    {
        std::set<uint64_t> fxIds, evIds;
        for (int ti : audioTracks) {
            const Track& tr = p.tracks[(size_t)ti];
            for (const auto& s : tr.fx)
                if (s.kind == "audio" && !fxIds.insert(s.id).second) return true;
            for (const auto& e : tr.events) {
                if (!evIds.insert(e.id).second) return true;
                for (const auto& s : e.fx)
                    if (s.kind == "audio" && !fxIds.insert(s.id).second) return true;
            }
        }
        return false;
    }

    void mixTracks()
    {
        for (size_t i = 0; i < p.tracks.size(); ++i)
            if (p.tracks[i].type == TrackType::Audio) audioTracks.push_back((int)i);
        const int nTracks = (int)audioTracks.size();
        int64_t cost = 0;
        int busy = 0;
        for (int ti : audioTracks) {
            const int64_t u = trackUnits(p.tracks[(size_t)ti]);
            cost += u * frames;
            if (u > 0) ++busy;
        }
        const bool par = threads > 1 && busy >= 2 && cost >= 16000 && !tracksShareState();
        // A batch of tracks is mixed at once, then added in order.
        const int batch = par ? std::min(nTracks, threads * 2) : 1;
        ensureWork((size_t)batch);
        for (int b0 = 0; b0 < nTracks; b0 += batch) {
            const int n = std::min(batch, nTracks - b0);
            if (par && n > 1) {
                runTogether(n, [&](int j) { doTrack(p.tracks[(size_t)audioTracks[(size_t)(b0 + j)]], *eng.work_[(size_t)j]); });
                width = std::max(width, std::min(n, threads));
            } else {
                for (int j = 0; j < n; ++j) doTrack(p.tracks[(size_t)audioTracks[(size_t)(b0 + j)]], *eng.work_[(size_t)j]);
            }
            for (int j = 0; j < n; ++j) commitTrack(*eng.work_[(size_t)j]);
        }
    }

    // ---- buses

    // Mixes one bus into its Work: effects, fader and where it goes.
    void doBus(int i, AudioEngine::Work& w)
    {
        const Bus& bus = p.buses[(size_t)i];
        AudioEngine::BusMix& b = *eng.buses_[(size_t)i];
        w.begin(frames, bus.id);
        const int64_t bIn = busIn[(size_t)i], bLat = busLat[(size_t)i];
        w.srcOut = bIn + bLat;
        EnvScratch& scratch = tScratch;
        scratch.size(frames);
        // The effects run even on a muted bus, so a reverb's tail is where it should be when it is unmuted.
        static const Envelopes noEnvelopes;
        const Envelopes& benv = bus.automation && bus.automationMode == "off" ? noEnvelopes : bus.envelopes;
        const bool postFx = bus.fxPostFader && !(six && b.out < 0);
        if (!postFx) runChainAt(bus.fx, &benv, b.l.data(), b.r.data(), start - bIn, frames);
        if (bus.mute) {
            if (postFx) runChainAt(bus.fx, &benv, b.l.data(), b.r.data(), start - bIn, frames);
            w.busMode = AudioEngine::Work::BusMuted;
            return;
        }
        const Envelope* vol = activeEnvelope(benv, "volume");
        const Envelope* pan = activeEnvelope(benv, "pan");
        const Envelope* mute = activeEnvelope(benv, "mute");
        if (six && b.out < 0) {
            // 5.1, into the master: the bus's fader without its stereo pan, then its surround panner (when it is put in)
            float* sg = nullptr;
            if (vol || mute) {
                w.sg.resize((size_t)frames);
                faderGains(vol, nullptr, mute, bus.level, 0.0, start, rate, frames, scratch, w.sg.data(), w.gainR.data());
                sg = w.sg.data();
            }
            w.hasSg = sg != nullptr;
            w.level = (float)bus.level;
            if (eng.busTap) {
                // the bus's own two channels through its fader, before its surround panner
                w.tapL.resize((size_t)frames);
                w.tapR.resize((size_t)frames);
                for (int k = 0; k < frames; ++k) {
                    const float g = sg ? sg[k] : (float)bus.level;
                    w.tapL[(size_t)k] = b.l[(size_t)k] * g;
                    w.tapR[(size_t)k] = b.r[(size_t)k] * g;
                }
            }
            float pl = 0, pr = 0;
            for (int k = 0; k < frames; ++k) {
                const float g = sg ? sg[k] : (float)bus.level;
                pl = std::max(pl, std::fabs(b.l[(size_t)k] * g));
                pr = std::max(pr, std::fabs(b.r[(size_t)k] * g));
            }
            w.peak.left = pl;
            w.peak.right = pr;
            w.busMode = AudioEngine::Work::BusSurround;
            return;
        }
        const float gl = panLeft(bus.level, bus.pan), gr = panRight(bus.level, bus.pan);
        const bool perSample = vol || pan || mute;
        const int64_t bfs = start - (postFx ? bIn : bIn + bLat);       // the sound at the fader
        if (perSample) faderGains(vol, pan, mute, bus.level, bus.pan, bfs, rate, frames, scratch, w.gainL.data(), w.gainR.data());
        AudioEngine::Work::Contrib& c = w.add(b.out, frames);
        float pl = 0, pr = 0;
        if (postFx) {
            // the fader first, then the effects on what it lets through
            for (int k = 0; k < frames; ++k) {
                b.l[(size_t)k] *= perSample ? w.gainL[(size_t)k] : gl;
                b.r[(size_t)k] *= perSample ? w.gainR[(size_t)k] : gr;
            }
            runChainAt(bus.fx, &benv, b.l.data(), b.r.data(), start - bIn, frames);
        }
        if (eng.busTap) {
            w.tapL.resize((size_t)frames);
            w.tapR.resize((size_t)frames);
        }
        for (int k = 0; k < frames; ++k) {
            const float vl = postFx ? b.l[(size_t)k] : b.l[(size_t)k] * (perSample ? w.gainL[(size_t)k] : gl);
            const float vr = postFx ? b.r[(size_t)k] : b.r[(size_t)k] * (perSample ? w.gainR[(size_t)k] : gr);
            c.l[(size_t)k] = vl;
            c.r[(size_t)k] = vr;
            pl = std::max(pl, std::fabs(vl));
            pr = std::max(pr, std::fabs(vr));
            if (eng.busTap) {
                w.tapL[(size_t)k] = vl;
                w.tapR[(size_t)k] = vr;
            }
        }
        w.peak.left = pl;
        w.peak.right = pr;
        w.busMode = AudioEngine::Work::BusNormal;
    }

    void commitBus(int i, AudioEngine::Work& w)
    {
        const Bus& bus = p.buses[(size_t)i];
        AudioEngine::BusMix& b = *eng.buses_[(size_t)i];
        if (w.busMode == AudioEngine::Work::BusMuted) {
            if (eng.busTap) {
                std::vector<float> zero((size_t)frames, 0.f);
                eng.busTap(bus.id, zero.data(), zero.data(), frames);
            }
        } else if (w.busMode == AudioEngine::Work::BusSurround) {
            if (eng.busTap) eng.busTap(bus.id, w.tapL.data(), w.tapR.data(), frames);
            eng.panSurround(bus.surround, start, rate, frames, b.l.data(), b.r.data(), w.hasSg ? w.sg.data() : nullptr, w.level, six);
        } else {
            addContribs(w, true);
            if (eng.busTap) eng.busTap(bus.id, w.tapL.data(), w.tapR.data(), frames);
        }
        eng.peaks_.push_back(w.peak);
    }

    // Mixes each bus into where it goes.
    void mixBuses()
    {
        const std::vector<int>& order = eng.busOrder_;
        for (size_t g0 = 0; g0 < order.size();) {
            size_t g1 = g0 + 1;
            while (g1 < order.size() && eng.buses_[(size_t)order[g1]]->depth == eng.buses_[(size_t)order[g0]]->depth) ++g1;
            const int n = (int)(g1 - g0);
            int64_t units = 0;
            for (size_t q = g0; q < g1; ++q)
                for (const auto& s : p.buses[(size_t)order[q]].fx) units += s.kind == "audio" && !s.bypass ? 6 : 0;
            bool sharedFx = false;
            {
                std::set<uint64_t> ids;
                for (size_t q = g0; q < g1; ++q)
                    for (const auto& s : p.buses[(size_t)order[q]].fx)
                        if (s.kind == "audio" && !ids.insert(s.id).second) sharedFx = true;
            }
            const bool par = threads > 1 && n >= 2 && units * frames >= 16000 && !sharedFx;
            ensureWork((size_t)(par ? n : 1));
            if (par) {
                runTogether(n, [&](int j) { doBus(order[g0 + (size_t)j], *eng.work_[(size_t)j]); });
                width = std::max(width, std::min(n, threads));
                for (int j = 0; j < n; ++j) commitBus(order[g0 + (size_t)j], *eng.work_[(size_t)j]);
            } else {
                for (size_t q = g0; q < g1; ++q) {
                    doBus(order[q], *eng.work_[0]);
                    commitBus(order[q], *eng.work_[0]);
                }
            }
            g0 = g1;
        }
    }

    // ---- the master

    // The master's effects in a 5.1 project.
    void runMasterFx51()
    {
        for (const auto& slot : p.audioOut) {
            if (slot.kind != "audio" || slot.bypass || eng.bypassFx) continue;
            AudioPlugin* main = eng.fx_.audio(slot.id);
            if (!main) continue;
            const FxAutomation autos = fxAutomationOf(main, p.masterEnvelopes, slot);
            const int step = autos.empty() ? FxRuntime::kAudioBlock : kAutomationBlock;
            if (main->channels() == 6) {
                // a plug-in that takes the six channels as one (VST3 5.1 buses, a VST2 6-in / 6-out plug-in)
                for (int at = 0; at < frames; at += step) {
                    setAutomatedParams(main, autos, start + at, rate);
                    float* io[6];
                    for (int c = 0; c < 6; ++c) io[c] = six[c] + at;
                    main->processChannels(io, 6, std::min(step, frames - at), start + at, playing);
                }
                continue;
            }
            const ProjectTempoScope tempoScope(p.settings.tempo, p.settings.beatsPerMeasure);
            for (int pair = 0; pair < 3; ++pair) {
                AudioPlugin* pl = pair == 0 ? main : eng.fx_.audioExtra(slot.id, pair);
                if (!pl) continue;          // not made yet: that pair passes as it is
                if (pair > 0) {
                    const int n = std::min(main->paramCount(), pl->paramCount());
                    for (int i = 0; i < n; ++i) {
                        const float v = main->paramGet(i);
                        if (pl->paramGet(i) != v) pl->paramSet(i, v);
                    }
                }
                for (int at = 0; at < frames; at += step) {
                    setAutomatedParams(pl, autos, start + at, rate);
                    pl->process(six[2 * pair] + at, six[2 * pair + 1] + at, std::min(step, frames - at), start + at, playing);
                }
            }
        }
    }

    void mixMaster51()
    {
        runMasterFx51();
        // the volume (and its envelopes) and mute: the same for all six; the master has no pan in 5.1
        const Envelope* mvol = activeEnvelope(p.masterEnvelopes, "volume");
        const Envelope* mmute = activeEnvelope(p.masterEnvelopes, "mute");
        const bool masterEnv = !p.masterMute && (mvol || mmute);
        if (masterEnv) faderGains(mvol, nullptr, mmute, p.masterVolume, 0.0, start, rate, frames, es, gainL.data(), gainR.data());
        const float mg = p.masterMute ? 0.f : (float)p.masterVolume;
        if (p.settings.lfeLowpass) eng.lowpassLfe(p, six[ChLFE], start, frames);
        AudioEngine::StripPeak master;
        master.channels = 6;
        for (int c = 0; c < 6; ++c) {
            float* x = six[c];
            float pk = 0;
            for (int k = 0; k < frames; ++k) {
                x[k] *= masterEnv ? gainL[(size_t)k] : mg;
                if (!std::isfinite(x[k])) x[k] = 0;
                pk = std::max(pk, std::fabs(x[k]));
            }
            master.ch[c] = pk;
        }
        master.left = std::max(std::max(master.ch[ChL], master.ch[ChC]), std::max(master.ch[ChLFE], master.ch[ChLs]));
        master.right = std::max(std::max(master.ch[ChR], master.ch[ChC]), std::max(master.ch[ChLFE], master.ch[ChRs]));
        eng.peakLeft = master.left;
        eng.peakRight = master.right;
        eng.peaks_.push_back(master);
    }

    void mixMasterStereo()
    {
        // VEGAS's master Pre FX / Post FX: by default the effects come before the master's volume
        if (!p.masterFxPostFader) runChainAt(p.audioOut, &p.masterEnvelopes, left, right, start - masterIn, frames);
        const float ml = p.masterMute ? 0.f : panLeft(p.masterVolume, p.masterPan);
        const float mr = p.masterMute ? 0.f : panRight(p.masterVolume, p.masterPan);
        const Envelope* mvol = activeEnvelope(p.masterEnvelopes, "volume");
        const Envelope* mmute = activeEnvelope(p.masterEnvelopes, "mute");
        const bool masterEnv = !p.masterMute && (mvol || mmute);
        if (masterEnv)
            faderGains(mvol, nullptr, mmute, p.masterVolume, p.masterPan, start - masterIn - (p.masterFxPostFader ? 0 : masterLat), rate, frames, es,
                       gainL.data(), gainR.data());
        float pl = 0, pr = 0;
        if (p.masterFxPostFader) {
            for (int k = 0; k < frames; ++k) {
                left[k] *= masterEnv ? gainL[(size_t)k] : ml;
                right[k] *= masterEnv ? gainR[(size_t)k] : mr;
            }
            runChainAt(p.audioOut, &p.masterEnvelopes, left, right, start - masterIn, frames);
        }
        for (int k = 0; k < frames; ++k) {
            if (!p.masterFxPostFader) {
                left[k] *= masterEnv ? gainL[(size_t)k] : ml;
                right[k] *= masterEnv ? gainR[(size_t)k] : mr;
            }
            if (!std::isfinite(left[k])) left[k] = 0;                // a misbehaving plug-in must not reach the speakers as noise
            if (!std::isfinite(right[k])) right[k] = 0;
            pl = std::max(pl, std::fabs(left[k]));
            pr = std::max(pr, std::fabs(right[k]));
        }
        eng.peakLeft = pl;
        eng.peakRight = pr;
        AudioEngine::StripPeak master;
        master.left = pl;
        master.right = pr;
        eng.peaks_.push_back(master);
    }
};

void AudioEngine::mix(const Project& p, int64_t start, int frames, float* left, float* right, float* const* six, bool playing)
{
    const FxRuntime::Hold hold(fx_);       // keeps the effects' instances alive while this block uses them
    std::memset(left, 0, (size_t)std::max(0, frames) * sizeof(float));
    std::memset(right, 0, (size_t)std::max(0, frames) * sizeof(float));
    if (six)
        for (int c = 0; c < 6; ++c) std::memset(six[c], 0, (size_t)std::max(0, frames) * sizeof(float));
    if (frames <= 0) return;
    ++renders_;
    AudioBlockMixer(*this, p, start, frames, left, right, six, playing).run();
}

void AudioEngine::publishPeaks()
{
    std::lock_guard<std::mutex> lk(peakMutex_);
    sharedPeaks_.assign(peaks_.begin(), peaks_.end());
}

void AudioEngine::forgetIdleEvents()
{
    if ((renders_ & 63) != 0) return;
    for (auto it = pdc_.begin(); it != pdc_.end();) {
        if (renders_ - it->second.used > 64) it = pdc_.erase(it);
        else ++it;
    }
    for (auto it = events_.begin(); it != events_.end();) {
        if (renders_ - it->second->lastUsed > 64) it = events_.erase(it);
        else ++it;
    }
}

// Butterworth, 8th order (Best), 4th (Good) or 2nd (Fast): sections of two poles each.
void AudioEngine::lowpassLfe(const Project& p, float* x, int64_t start, int frames)
{
    const int rate = p.settings.sampleRate;
    const int sections = p.settings.lfeQuality == "fast" ? 1 : (p.settings.lfeQuality == "good" ? 2 : 4);
    if (lfeNext_ != start) std::memset(lfeState_, 0, sizeof(lfeState_));
    const double fc = std::min(0.45 * rate, std::max(10.0, p.settings.lfeCutoff));
    const double w = 2.0 * kPi * fc / rate, cw = std::cos(w), sw = std::sin(w);
    for (int q = 0; q < sections; ++q) {
        const double Q = 1.0 / (2.0 * std::cos(kPi * (2.0 * q + 1.0) / (4.0 * sections)));
        const double alpha = sw / (2.0 * Q), a0 = 1.0 + alpha;
        const double b0 = (1.0 - cw) / 2.0 / a0, b1 = (1.0 - cw) / a0, b2 = b0, a1 = -2.0 * cw / a0, a2 = (1.0 - alpha) / a0;
        double z1 = lfeState_[q][0], z2 = lfeState_[q][1];
        for (int k = 0; k < frames; ++k) {
            const double in = x[k];
            const double out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            x[k] = (float)out;
        }
        lfeState_[q][0] = std::isfinite(z1) ? z1 : 0.0;
        lfeState_[q][1] = std::isfinite(z2) ? z2 : 0.0;
    }
    lfeNext_ = start + frames;
}

}  // namespace reno
