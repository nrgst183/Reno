// The thread that reads media for the timeline's thumbnails and waveforms.
#include "timeline_internal.h"

namespace reno {
namespace app {

namespace tl {

Loader::Loader(MediaPool::Opener opener, std::function<void()> onReady, bool rebuild) : s_(std::make_shared<Shared>())
{
    pool_.setOpener(std::move(opener));
    s_->onReady = std::move(onReady);
    s_->rebuild = rebuild;
    std::shared_ptr<Shared> s = s_;
    MediaPool* pool = &pool_;
    thread_ = std::thread([s, pool] {
        lowerThreadPriority();
        run(s, pool);
    });
}

Loader::~Loader()
{
    {
        std::lock_guard<std::mutex> lk(s_->mutex);
        s_->quit = true;
    }
    s_->cv.notify_all();
    if (thread_.joinable()) thread_.join();
    s_->onReady = nullptr;
}

void Loader::requestThumb(const ThumbKey& key, const MediaItem& m, double seconds, int w, int h)
{
    if (!pending_.insert(key).second) return;
    Job j;
    j.key = key;
    j.media = lightCopy(m);
    j.seconds = seconds;
    j.w = w;
    j.h = h;
    {
        std::lock_guard<std::mutex> lk(s_->mutex);
        s_->thumbs.push_front(std::move(j));
        while (s_->thumbs.size() > 300) {
            pending_.erase(s_->thumbs.back().key);
            dropped_.insert(s_->thumbs.back().key.media);
            s_->thumbs.pop_back();
        }
    }
    s_->cv.notify_one();
}

// The waveform of 'm' (reading starts on the first request). 'user' tells files apart. first >= 0: the file's own
// channel(s) 'first' ('count' of them) rather than its stereo mix (Event::mediaChannel).
std::shared_ptr<Peaks> Loader::peaks(int user, const MediaItem& m, int first, int count)
{
    if (first < 0) count = 0;
    const std::tuple<int, int, int> key(user, first, count);
    auto it = peaks_.find(key);
    if (it != peaks_.end()) return it->second;
    std::shared_ptr<Peaks> pk;
    // A file of unknown length (a .veg names its media without it) is examined first on the loader's thread; its waveform
    // is requested again afterwards (the timeline is redrawn then).
    MediaInfo info = m.info;
    if (info.hasAudio && !(info.duration > 0) && cacheable(m)) {
        std::lock_guard<std::mutex> lk(s_->mutex);
        auto p = s_->probed.find(m.path);
        if (p == s_->probed.end()) {
            if (s_->probing.insert(m.path).second) {
                s_->probes.push_back(lightCopy(m));
                s_->cv.notify_one();
            }
            return nullptr;
        }
        const MediaInfo& got = p->second;
        info.duration = got.duration;
        if (info.sampleRate <= 0) info.sampleRate = got.sampleRate;
        if (info.channels <= 0) info.channels = got.channels;
        if (info.audioCodec.empty()) info.audioCodec = got.audioCodec;
        info.hasAudio = got.hasAudio;
    }
    const double seconds = std::min(info.duration, 12.0 * 3600.0);
    if (info.hasAudio && seconds > 0) {
        pk = std::make_shared<Peaks>();
        int64_t n = std::max<int64_t>(1, (int64_t)std::ceil(seconds * kPeakPerSecond));
        for (;;) {
            Peaks::Level l;
            l.lo.assign((size_t)n, 0);
            l.hi.assign((size_t)n, 0);
            pk->levels.push_back(std::move(l));
            if (n <= 64) break;
            n = (n + kPeakStep - 1) / kPeakStep;
        }
        Wave w;
        w.media = lightCopy(m);
        w.media.info = info;
        w.user = ((uint64_t)(unsigned)user << 8) | (uint64_t)(first + 1);
        w.first = first;
        w.count = count;
        w.peaks = pk;
        w.done.assign(pk->levels.size(), 0);
        {
            std::lock_guard<std::mutex> lk(s_->mutex);
            s_->waves.push_back(std::move(w));
        }
        s_->cv.notify_one();
    }
    peaks_[key] = pk;
    return pk;
}

// The length of 'm': the project's, or when it has none (a .veg's media) the file's own once the loader has looked (0
// until then).
double Loader::duration(const MediaItem& m)
{
    if (m.info.duration > 0) return m.info.duration;
    std::lock_guard<std::mutex> lk(s_->mutex);
    auto p = s_->probed.find(m.path);
    return p == s_->probed.end() ? 0.0 : p->second.duration;
}

std::vector<std::string> Loader::takeProbed()
{
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(s_->mutex);
    out.swap(s_->probedNew);
    return out;
}

std::set<int> Loader::takeDropped()
{
    std::set<int> out;
    out.swap(dropped_);
    return out;
}

void Loader::forgetMedia(int user, const std::string& path)
{
    for (auto it = peaks_.begin(); it != peaks_.end();) {
        if (std::get<0>(it->first) == user) it = peaks_.erase(it);
        else ++it;
    }
    std::lock_guard<std::mutex> lk(s_->mutex);
    s_->probed.erase(path);
}

void Loader::forgetFiles()
{
    pool_.clear();
    std::lock_guard<std::mutex> lk(s_->mutex);
    s_->rebuild = false;
}

std::vector<Loader::Done> Loader::take()
{
    std::vector<Done> out;
    {
        std::lock_guard<std::mutex> lk(s_->mutex);
        out.swap(s_->finished);
    }
    for (const Done& d : out) pending_.erase(d.key);
    return out;
}

MediaItem Loader::lightCopy(const MediaItem& m)
{
    MediaItem c;
    c.id = m.id;
    c.path = m.path;
    c.name = m.name;
    c.info = m.info;
    return c;
}

void Loader::wake(const std::shared_ptr<Shared>& s)
{
    {
        std::lock_guard<std::mutex> lk(s->mutex);
        if (s->posted) return;
        s->posted = true;
    }
    AppState::post([s] {
        {
            std::lock_guard<std::mutex> lk(s->mutex);
            s->posted = false;
        }
        if (s->onReady) s->onReady();
    });
}

void Loader::coarsen(Peaks& pk, size_t level, int64_t from, int64_t to, int64_t below)
{
    const Peaks::Level& src = pk.levels[level - 1];
    Peaks::Level& dst = pk.levels[level];
    for (int64_t k = from; k < to; ++k) {
        int8_t lo = 127, hi = -127;
        const int64_t end = std::min<int64_t>((k + 1) * kPeakStep, below);
        for (int64_t j = k * kPeakStep; j < end; ++j) {
            lo = std::min(lo, src.lo[(size_t)j]);
            hi = std::max(hi, src.hi[(size_t)j]);
        }
        if (lo > hi) lo = hi = 0;
        dst.lo[(size_t)k] = lo;
        dst.hi[(size_t)k] = hi;
    }
}

bool Loader::waveStep(Wave& w, MediaPool& pool, std::vector<float>& l, std::vector<float>& r)
{
    Peaks& pk = *w.peaks;
    const int64_t total = (int64_t)pk.levels[0].lo.size();
    const int buckets = (int)std::min<int64_t>(2000, total - w.done[0]);
    bool ok = buckets > 0;
    if (ok) {
        const int frames = buckets * kPeakBucket;
        l.assign((size_t)frames, 0.f);
        r.assign((size_t)frames, 0.f);
        ok = pool.audio(w.user, w.media, w.done[0] * kPeakBucket, frames, kPeakRate, l.data(), r.data(), w.first, w.count);
        if (!ok) w.failed = true;
    }
    if (ok) {
        Peaks::Level& l0 = pk.levels[0];
        for (int b = 0; b < buckets; ++b) {
            float lo = 1.f, hi = -1.f;
            const size_t at = (size_t)b * kPeakBucket;
            for (size_t k = at; k < at + kPeakBucket; ++k) {
                lo = std::min(lo, std::min(l[k], r[k]));
                hi = std::max(hi, std::max(l[k], r[k]));
            }
            l0.lo[(size_t)(w.done[0] + b)] = pack(lo);
            l0.hi[(size_t)(w.done[0] + b)] = pack(hi);
        }
        w.done[0] += buckets;
        levelsFollow(w);
        if (w.done[0] < total) return false;
    }
    levelsFinish(w, ok);
    return true;
}

void Loader::levelsFollow(Wave& w)
{
    Peaks& pk = *w.peaks;
    for (size_t lev = 1; lev < pk.levels.size(); ++lev) {
        const int64_t whole = w.done[lev - 1] / kPeakStep;
        coarsen(pk, lev, w.done[lev], whole, w.done[lev - 1]);
        w.done[lev] = whole;
    }
    pk.filled.store(w.done[0]);
}

// At the end (or if the file couldn't be read; the rest stays silent): finish the coarser levels, whose last stretch
// may cover less than a full group of the finer one.
void Loader::levelsFinish(Wave& w, bool ok)
{
    Peaks& pk = *w.peaks;
    for (size_t lev = 1; lev < pk.levels.size(); ++lev) {
        const int64_t n = (int64_t)pk.levels[lev].lo.size();
        coarsen(pk, lev, w.done[lev], n, ok ? w.done[lev - 1] : (int64_t)pk.levels[lev - 1].lo.size());
        w.done[lev] = n;
    }
    pk.complete.store(true);
}

void Loader::fillFromPeakFile(Wave& w, const vegasfiles::PeakFile& pf, int64_t ready, bool finished)
{
    Peaks& pk = *w.peaks;
    Peaks::Level& l0 = pk.levels[0];
    const int64_t total = (int64_t)l0.lo.size(), nb = pf.blocks();
    int c0 = 0, c1 = pf.channels;
    if (w.first >= 0 && pf.channels > 1) {
        c0 = std::min(w.first, pf.channels - 1);
        c1 = std::min(pf.channels, c0 + std::max(1, w.count));
    }
    auto pack16 = [](int v) { return (int8_t)std::max(-127L, std::min(127L, std::lround(v * 127.0 / 32768.0))); };
    const double perBucket = (double)w.rate / kPeakPerSecond;
    int64_t j = w.done[0];
    for (; j < total; ++j) {
        int8_t lo = 0, hi = 0;
        if (pf.frames > 0 && nb > 0) {
            const double f0 = j * perBucket, f1 = (j + 1) * perBucket;
            const int64_t b0 = (int64_t)std::floor(f0 * nb / pf.frames);
            const int64_t b1 = std::min(nb, std::max(b0 + 1, (int64_t)std::ceil(f1 * nb / pf.frames)));
            if (!finished && b1 > ready) break;
            if (b0 < nb) {
                int mn = INT_MAX, mx = INT_MIN;
                for (int64_t b = b0; b < b1; ++b)
                    for (int c = c0; c < c1; ++c) {
                        mn = std::min(mn, (int)pf.lo(b, c));
                        mx = std::max(mx, (int)pf.hi(b, c));
                    }
                if (mn <= mx) {
                    lo = pack16(mn);
                    hi = pack16(mx);
                }
            }
        }
        l0.lo[(size_t)j] = lo;
        l0.hi[(size_t)j] = hi;
    }
    w.done[0] = j;
    levelsFollow(w);
    if (finished) levelsFinish(w, true);
}

// The waveform from a current VEGAS peak file beside the media (made from the file as it is now, of about its length,
// with the wanted channels). For a nested project, VEGAS's .sfk is of its audio proxy's sound: only used while that
// proxy is current.
bool Loader::loadPeakFile(Wave& w)
{
    const MediaItem& m = w.media;
    if (!mediacache::vegasPeakFiles().load() || !cacheable(m) || m.info.sampleRate <= 0) return false;
    const bool project = isProjectFile(m.path);
    if (project && !audioProxyCurrent(m.path)) return false;
    const int64_t proxyTime = project ? vegasfiles::fileTime(vegasfiles::audioProxyPath(m.path)) : 0;
    for (const std::string& file : vegasfiles::peakFileCandidates(m.path)) {
        vegasfiles::PeakFile pf;
        if (!vegasfiles::readPeakFile(file, pf) || !vegasfiles::peakFileCurrent(pf, m.path)) continue;
        if (w.first >= 0 && pf.channels > 1 && w.first >= pf.channels) continue;
        const double seconds = (double)pf.frames / m.info.sampleRate;
        if (std::fabs(seconds - m.info.duration) > std::max(0.5, 0.02 * m.info.duration)) continue;
        if (project && vegasfiles::fileTime(file) < proxyTime) continue;
        w.rate = m.info.sampleRate;
        fillFromPeakFile(w, pf, pf.blocks(), true);
        return true;
    }
    return false;
}

// Whether the waveform is made VEGAS's way (sound read at the file's own rate, VEGAS's peaks made from it, the waveform
// from those): for files of one or two channels at an ordinary rate.
void Loader::chooseNative(Wave& w)
{
    const MediaInfo& mi = w.media.info;
    if (!cacheable(w.media) || mi.channels < 1 || mi.channels > 2 || mi.sampleRate < 4000 || mi.sampleRate > 384000 || !(mi.duration > 0)) return;
    const double exact = mi.duration * mi.sampleRate;
    const int64_t frames = isProjectFile(w.media.path) ? (int64_t)std::floor(exact + 1e-6) : (int64_t)std::llround(exact);
    if (frames <= 0) return;
    w.native = true;
    w.rate = mi.sampleRate;
    w.builder = std::make_shared<vegasfiles::PeakBuilder>(mi.channels, frames, vegasfiles::peakRounding(mi.audioCodec));
}

bool Loader::nativeStep(Wave& w, MediaPool& pool, std::vector<float>& l, std::vector<float>& r)
{
    vegasfiles::PeakBuilder& b = *w.builder;
    const int64_t frames = b.peaks().frames, at = b.given();
    const int n = (int)std::min<int64_t>(frames - at, (int64_t)w.rate * 2);
    bool ok = n > 0;
    if (ok) {
        l.assign((size_t)n, 0.f);
        r.assign((size_t)n, 0.f);
        ok = pool.audio(w.user, w.media, at, n, w.rate, l.data(), r.data());
        if (!ok) w.failed = true;
    }
    if (ok) {
        const float* ch[2] = {l.data(), r.data()};
        b.add(ch, n);
        if (b.given() < frames) {
            fillFromPeakFile(w, b.peaks(), b.blocksDone(), false);
            return false;
        }
    }
    fillFromPeakFile(w, b.peaks(), b.blocksDone(), true);
    return true;
}

// Keeps a VEGAS-style waveform in a .sfk beside the media; where that can't be written (read-only folder), in Reno's cache.
void Loader::keepNative(const Wave& w)
{
    bool kept = false;
    if (mediacache::vegasPeakFiles().load()) {
        const vegasfiles::PeakFile pf = w.builder->finish(vegasfiles::fileTime(w.media.path));
        kept = vegasfiles::writePeakFile(vegasfiles::peakFilePath(w.media.path), pf);
    }
    if (!kept && !w.cacheFile.isEmpty()) savePeaks(w);
}

bool Loader::loadPeaks(Wave& w)
{
    QByteArray data;
    if (!mediacache::read(w.cacheFile, data)) return false;
    Peaks& pk = *w.peaks;
    const QByteArray magic(peaksMagic());
    if (!data.startsWith(magic)) return false;
    size_t at = (size_t)magic.size();
    auto take = [&](void* to, size_t n) {
        if (at + n > (size_t)data.size()) return false;
        std::memcpy(to, data.constData() + at, n);
        at += n;
        return true;
    };
    uint32_t levels = 0;
    if (!take(&levels, 4) || levels != pk.levels.size()) return false;
    for (Peaks::Level& lv : pk.levels) {
        uint64_t n = 0;
        if (!take(&n, 8) || n != lv.lo.size()) return false;
    }
    for (Peaks::Level& lv : pk.levels)
        if (!take(lv.lo.data(), lv.lo.size()) || !take(lv.hi.data(), lv.hi.size())) return false;
    pk.filled.store((int64_t)pk.levels[0].lo.size());
    pk.complete.store(true);
    return true;
}

void Loader::savePeaks(const Wave& w)
{
    const Peaks& pk = *w.peaks;
    QByteArray data(peaksMagic());
    const uint32_t levels = (uint32_t)pk.levels.size();
    data.append((const char*)&levels, 4);
    for (const Peaks::Level& lv : pk.levels) {
        const uint64_t n = lv.lo.size();
        data.append((const char*)&n, 8);
    }
    for (const Peaks::Level& lv : pk.levels) {
        data.append((const char*)lv.lo.data(), (qsizetype)lv.lo.size());
        data.append((const char*)lv.hi.data(), (qsizetype)lv.hi.size());
    }
    mediacache::write(w.cacheFile, data);
}

// While the preview plays, a thumbnail that is not in the cache is decoded at most this often (seconds).
static const double kThumbPlayingGap = 0.25;

void Loader::run(std::shared_ptr<Shared> s, MediaPool* pool)
{
    std::vector<float> l, r;
    std::unique_ptr<Wave> wave;
    auto lastWake = std::chrono::steady_clock::now();
    auto lastDecode = lastWake - std::chrono::seconds(10);
    mediacache::Signatures sigs;
    mediacache::trim("thumbs", (qint64)std::max(0, vprefs::number("prefs/thumbCacheMb")) * 1024 * 1024);
    mediacache::trim("peaks", (qint64)std::max(0, vprefs::number("prefs/peakCacheMb")) * 1024 * 1024);
    for (;;) {
        Job job;
        bool thumb = false, started = false;
        {
            std::unique_lock<std::mutex> lk(s->mutex);
            s->cv.wait(lk, [&] { return s->quit || !s->thumbs.empty() || wave || !s->waves.empty() || !s->probes.empty(); });
            if (s->quit) return;
            if (!s->probes.empty()) {
                MediaItem m = std::move(s->probes.front());
                s->probes.pop_front();
                lk.unlock();
                MediaInfo got;
                std::string err;
                std::unique_ptr<MediaReader> r = openMediaOrProject(m.path, &err);
                if (r) got = r->info();
                lk.lock();
                s->probed[m.path] = got;
                s->probing.erase(m.path);
                s->probedNew.push_back(m.path);
                lk.unlock();
                wake(s);
                continue;
            }
            if (!s->thumbs.empty()) {
                job = std::move(s->thumbs.front());
                s->thumbs.pop_front();
                thumb = true;
            } else if (!wave) {
                wave.reset(new Wave(std::move(s->waves.front())));
                s->waves.pop_front();
                started = true;
            }
        }
        if (started) {
            started = false;
            wave->leftOut = nestedEffectsLeftOut();
            bool rebuild;
            {
                std::lock_guard<std::mutex> lk(s->mutex);
                rebuild = s->rebuild;
            }
            if (!rebuild && loadPeakFile(*wave)) {
                wave.reset();
                wake(s);
                continue;
            }
            const std::string sig = cacheable(wave->media) ? sigs.get(wave->media.path) : std::string();
            if (!sig.empty())
                wave->cacheFile = mediacache::fileFor("peaks", sig + strfmt("|%d|%d|%d|%d|%d|%zu", wave->first, wave->count, kPeakRate, kPeakBucket, kPeakStep,
                                                                             wave->peaks->levels[0].lo.size()), ".rpk");
            if (!rebuild && !wave->cacheFile.isEmpty() && loadPeaks(*wave)) {
                wave.reset();
                wake(s);
                continue;
            }
            chooseNative(*wave);
        }
        if (thumb) {
            Done d;
            d.key = job.key;
            const std::string sig = cacheable(job.media) ? sigs.get(job.media.path) : std::string();
            const QString file = sig.empty() ? QString()
                                             : mediacache::fileFor("thumbs", sig + strfmt("|%d|%d|%lld|%.6f", job.w, job.h, (long long)job.key.ms, job.seconds), ".png");
            if (file.isEmpty() || !mediacache::readImage(file, d.image)) {
                // Thumbnails need not be exact, and must not take the processors from playing: while the preview plays,
                // at most one is decoded every kThumbPlayingGap; and each is the nearest frame at hand (the key frame a
                // seek lands on when the one asked for is well past it: MediaReadHints::quick), not decoded on to.
                if (previewPlaying()) {
                    const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastDecode).count();
                    if (since < kThumbPlayingGap) {
                        std::unique_lock<std::mutex> lk(s->mutex);
                        s->thumbs.push_front(std::move(job));
                        s->cv.wait_for(lk, std::chrono::duration<double>(kThumbPlayingGap - since), [&] { return s->quit; });
                        continue;
                    }
                }
                lastDecode = std::chrono::steady_clock::now();
                const uint64_t leftOut = nestedEffectsLeftOut();
                MediaReadHintsScope quick(nullptr, true);
                ImagePtr img = pool->videoFrame((uint64_t)job.key.media, job.media, job.seconds, job.w, job.h);
                if (img && img->w > 0 && img->h > 0 && img->depth == PixelDepth::Byte) d.image = imageToQt(*img);
                if (!file.isEmpty() && !d.image.isNull() && nestedEffectsLeftOut() == leftOut) mediacache::writeImage(file, d.image);
            }
            {
                std::lock_guard<std::mutex> lk(s->mutex);
                s->finished.push_back(std::move(d));
            }
            wake(s);
            continue;
        }
        const bool finished = wave->native ? nativeStep(*wave, *pool, l, r) : waveStep(*wave, *pool, l, r);
        if (finished) {
            if (!wave->failed && nestedEffectsLeftOut() == wave->leftOut) {
                if (wave->native) keepNative(*wave);
                else if (!wave->cacheFile.isEmpty()) savePeaks(*wave);
            }
            wave.reset();
        }
        const auto now = std::chrono::steady_clock::now();
        if (finished || now - lastWake > std::chrono::milliseconds(120)) {
            lastWake = now;
            wake(s);
        }
    }
}

}  // namespace tl

}  // namespace app
}  // namespace reno
