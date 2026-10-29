#include "engine.h"
#include <cstdlib>
#include "engine_internal.h"
#include "../media/vegasfiles.h"
#include "../core/pixrows.h"
#include "profile.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <tuple>
#include <set>
#include <filesystem>
#include <chrono>
#include <condition_variable>
#include <list>
#include <deque>
#include <limits>
#include <thread>

namespace reno {

// ====================================================================== MediaPool
static const size_t kMaxVideoReaders = 12;
static const size_t kMaxAudioReaders = 32;
static const size_t kVideoReadersPerMedia = 12;
static const size_t kAudioReadersPerMedia = 12;
// A reader not used for this many of the pool's requests is taken to be idle.
static const uint64_t kIdleUses = 24;
// A thread that wants a frame up to this much ahead of (or kWaitBehindSeconds before) the one a busy reader of the
// file is reading waits for that reader (MediaPool::get).
static const double kWaitAheadSeconds = 0.6;
static const int kHarvestFrames = 24;               // frames a read gives the shared frames besides the one asked for
static const double kWaitBehindSeconds = 0.4;

// ---------------------------------------------------------------------- shared frames
// What a read is for: the file (its path too, for Replace Media), the size asked for and the reader settings that
// change the picture.
static uint64_t shapeOf(const MediaItem& m, int w, int h, int deinterlace)
{
    uint64_t x = std::hash<std::string>()(m.path) * 0x9E3779B97F4A7C15ull;
    x ^= (uint64_t)(uint32_t)w * 0xC2B2AE3D27D4EB4Full + ((uint64_t)(uint32_t)h << 17) + ((uint64_t)deinterlace << 40);
    x ^= std::hash<std::string>()(m.fieldOrderOverride) * 0x165667B19E3779F9ull;
    return x;
}

// The frames made by the video readers of one pool, each good for a stretch of times of its media (MediaReader::videoFrameSpan).
class MediaPool::FrameShare {
public:
    explicit FrameShare(size_t bytes) : limit_(bytes) {}
    ImagePtr find(uint64_t media, uint64_t shape, double seconds)
    {
        std::lock_guard<std::mutex> lk(m_);
        auto g = groups_.find(Key{media, shape});
        if (g == groups_.end()) return nullptr;
        auto it = g->second.upper_bound(seconds);       // the first that starts after 'seconds'...
        if (it == g->second.begin()) return nullptr;
        --it;                                           // ...so this is the last that starts at or before it
        if (!(seconds < it->second.until)) return nullptr;
        it->second.used = ++clock_;
        return it->second.img;
    }
    // 'generation': generation() when the frame was asked for. A frame asked for before the last clear() (a project
    // closed while it was being decoded) is not kept: it would stay until the next project's frames push it out.
    void put(uint64_t media, uint64_t shape, double from, double until, const ImagePtr& img, uint64_t generation)
    {
        if (!img || !(from <= until)) return;
        std::lock_guard<std::mutex> lk(m_);
        if (generation != generation_) return;
        auto& g = groups_[Key{media, shape}];
        auto it = g.lower_bound(from);
        if (it != g.begin()) {
            auto b = std::prev(it);
            if (b->second.until > from) it = b;
        }
        while (it != g.end() && it->first < until) {
            bytes_ -= it->second.img->byteSize();
            it = g.erase(it);
        }
        Span sp;
        sp.until = until;
        sp.img = img;
        sp.used = ++clock_;
        g[from] = sp;
        bytes_ += img->byteSize();
        if (bytes_ > limit_) evict();
    }
    void clear()
    {
        std::lock_guard<std::mutex> lk(m_);
        groups_.clear();
        bytes_ = 0;
        ++generation_;
    }
    uint64_t generation() const
    {
        std::lock_guard<std::mutex> lk(m_);
        return generation_;
    }
    size_t bytes() const
    {
        std::lock_guard<std::mutex> lk(m_);
        return bytes_;
    }
private:
    struct Key {
        uint64_t media, shape;
        bool operator<(const Key& o) const { return media != o.media ? media < o.media : shape < o.shape; }
    };
    struct Span {
        double until = 0;
        ImagePtr img;
        uint64_t used = 0;
    };
    // Frees the least recently used frames down to three quarters of the limit (frames being of about one size here, the
    // count to free is worked out by the average).
    void evict()
    {
        std::vector<uint64_t> used;
        for (auto& g : groups_)
            for (auto& sp : g.second) used.push_back(sp.second.used);
        if (used.empty()) return;
        std::sort(used.begin(), used.end());
        const size_t avg = std::max<size_t>(1, bytes_ / used.size());
        const size_t cut = std::min(used.size(), (bytes_ - limit_ / 4 * 3) / avg + 1);
        const uint64_t below = used[cut - 1];
        for (auto g = groups_.begin(); g != groups_.end();) {
            for (auto it = g->second.begin(); it != g->second.end();) {
                if (it->second.used <= below) {
                    bytes_ -= it->second.img->byteSize();
                    it = g->second.erase(it);
                } else {
                    ++it;
                }
            }
            if (g->second.empty()) g = groups_.erase(g);
            else ++g;
        }
    }
    mutable std::mutex m_;
    std::map<Key, std::map<double, Span>> groups_;
    size_t bytes_ = 0, limit_;
    uint64_t clock_ = 0;
    uint64_t generation_ = 0;
};

void MediaPool::setShareFrames(bool on, size_t bytes)
{
    std::lock_guard<std::mutex> lk(mutex_);
    share_ = on ? std::make_shared<FrameShare>(bytes) : nullptr;
}

MediaPool::MediaPool()
{
    opener_ = [](const std::string& path, std::string* error) { return openMediaOrProject(path, error); };
}

void MediaPool::setOpener(Opener o)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (o) opener_ = std::move(o);
    video_.clear();
    audio_.clear();
    if (share_) share_->clear();
}

void MediaPool::setReaderLimits(size_t video, size_t audio)
{
    std::lock_guard<std::mutex> lk(mutex_);
    maxVideo_ = video;
    maxAudio_ = audio;
}

void MediaPool::clear()
{
    std::lock_guard<std::mutex> lk(mutex_);
    video_.clear();
    audio_.clear();
    if (share_) share_->clear();
}

MediaPool::EntryPtr MediaPool::get(std::vector<EntryPtr>& list, size_t limit, size_t perMedia, uint64_t user, const MediaItem& m,
                                   const std::function<int(Entry&)>& cost, std::unique_lock<std::mutex>& lock, double reserveSeconds, double want,
                                   uint64_t shape)
{
    const double clock = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const uint64_t lane = user & 0xC000000000000000ull;
    EntryPtr e, waitFor;
    Opener opener;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t now = ++uses_;
        double waitGap = kWaitAheadSeconds;
        // Rate the readers of this media that are free now: the cheapest wins (the user's own one, then the one used
        // last, between equals).
        EntryPtr best, own, oldestFree, oldest, idle;
        int bestCost = 3;
        size_t count = 0;
        for (const EntryPtr& x : list) {
            if (x->media != m.id || x->lane != lane) continue;
            ++count;
            if (x->lastUser == user && (!own || x->lastUse > own->lastUse)) own = x;
            if ((!oldest || x->lastUse < oldest->lastUse) && !(x->reservedUntil > clock && x->lastUser != user)) oldest = x;
            std::unique_lock<std::mutex> xl(x->mutex, std::try_to_lock);
            if (!xl.owns_lock()) {                          // busy for another thread (or still being opened)
                // Busy reading a time just before the one wanted.
                if (std::isfinite(want) && x->busyShape.load() == shape) {
                    const double at = x->busyWant.load();
                    if (std::isfinite(at) && want >= at - kWaitBehindSeconds && want - at < waitGap) {
                        waitGap = std::max(0.0, want - at);
                        waitFor = x;
                    }
                }
                continue;
            }
            if (x->failed || !x->reader) {                  // the file cannot be opened
                e = x;
                break;
            }
            const bool kept = x->reservedUntil > clock && x->lastUser != user;
            if (!kept && (!oldestFree || x->lastUse < oldestFree->lastUse)) oldestFree = x;
            if (!kept && x->lastUse + kIdleUses < now && (!idle || x->lastUse < idle->lastUse)) idle = x;
            const int c = cost(*x);
            bool better = c < bestCost;
            if (!better && c == bestCost && best) {
                const bool xOwn = x->lastUser == user, bOwn = best->lastUser == user;
                better = (xOwn && !bOwn) || (xOwn == bOwn && x->lastUse > best->lastUse);
            }
            if (better) {
                best = x;
                bestCost = c;
            }
        }
        if (!e) {
            if (best && bestCost <= 1) e = best;            // has it at hand, or decodes on to it
            else if (waitFor) e = waitFor;                  // another thread's reader is on its way there
            else if (own) e = own;                          // the event's own reader seeks (as it always did)
            else if (idle) e = idle;                        // one no event is using now seeks
            else if (count >= perMedia && (oldestFree || oldest)) e = oldestFree ? oldestFree : oldest;
        }
        if (!e) {
            e = std::make_shared<Entry>();
            e->media = m.id;
            e->lane = lane;
            list.push_back(e);
            opener = opener_;
            // too many readers: the one used longest ago goes (a thread still using it keeps it until it is done)
            while (list.size() > limit) {
                size_t drop = list.size();
                for (size_t k = 0; k < list.size(); ++k)
                    if (list[k]->reservedUntil <= clock && (drop == list.size() || list[k]->lastUse < list[drop]->lastUse)) drop = k;
                if (drop == list.size()) {
                    drop = 0;
                    for (size_t k = 1; k < list.size(); ++k)
                        if (list[k]->reservedUntil > list[drop]->reservedUntil) drop = k;
                }
                if (list[drop] == e) break;
                list.erase(list.begin() + (std::ptrdiff_t)drop);
            }
        }
        const uint64_t before = e->lastUser;
        e->lastUser = user;
        e->lastUse = now;
        if (reserveSeconds > 0) e->reservedUntil = std::max(e->reservedUntil, clock + reserveSeconds);
        else if (before == user) e->reservedUntil = 0;
        if (!opener) opener = opener_;
    }
    if (e == waitFor) {
        // not holding the pool's lock: the reader's thread is decoding.
        std::unique_lock<std::mutex> xl(e->mutex, std::try_to_lock);
        for (int spin = 0; !xl.owns_lock(); ++spin) {
            if (mediaReadHints().cancelled()) return nullptr;
            std::this_thread::sleep_for(std::chrono::microseconds(spin < 20 ? 50 : 300));
            (void)xl.try_lock();
        }
        lock = std::move(xl);
        ++stats_.videoWaited;
    } else {
        lock = std::unique_lock<std::mutex>(e->mutex);
    }
    e->busyShape = shape;
    e->busyWant = want;
    // The media's file is another one now.
    if (e->reader && !e->reader->info().path.empty() &&
        (m.sequenceFps > 0 ? e->reader->info().path != m.readPath() : e->reader->info().path != m.path)) {
        e->reader.reset();
        e->failed = false;
        e->nextStart = 0;
        e->rate = 0;
        e->first = -1;
        e->count = 0;
        e->deinterlace = 0;
        e->field.clear();
    }
    if (opener && !e->reader && !e->failed) {
        // opened outside the pool's lock (it can take a while); the entry's own lock keeps others out meanwhile
        if (&list == &video_) ++stats_.videoOpened;
        else ++stats_.audioOpened;
        std::string err;
        e->reader = opener(m.readPath(), &err);
        if (!e->reader) {
            e->failed = true;
            logf("media: %s: %s", m.path.c_str(), err.c_str());
        }
    }
    return e;
}

// Media Properties' Alpha channel (MediaItem::alphaMode) applied to a frame as the reader made it (premultiplied by
// the alpha the file has, taking it to be straight).
static ImagePtr withAlphaMode(const ImagePtr& img, const std::string& mode)
{
    if (!img || (mode != "none" && mode != "premultiplied")) return img;
    ImagePtr out = img->clone();
    if (!out) return img;
    const bool none = mode == "none";
    parallelRows(out->h, (size_t)out->w * 6, [&](int ya, int yb) {
    for (int y = ya; y < yb; ++y) {
        if (out->depth == PixelDepth::Byte) {
            uint8_t* d = out->row(y);
            for (int x = 0; x < out->w; ++x, d += 4) {
                const int a = d[3];
                if (a == 255) continue;
                for (int c = 0; c < 3; ++c) d[c] = a ? (uint8_t)std::min(255, (d[c] * 255 + a / 2) / a) : 0;
                if (none) d[3] = 255;
                else for (int c = 0; c < 3; ++c) d[c] = std::min(d[c], d[3]);
            }
        } else {
            float* d = (float*)out->row(y);
            for (int x = 0; x < out->w; ++x, d += 4) {
                const float a = d[3];
                if (a >= 1.f) continue;
                for (int c = 0; c < 3; ++c) d[c] = a > 1e-6f ? d[c] / a : 0.f;
                if (none) d[3] = 1.f;
            }
        }
    }
    });
    return out;
}


// Media Properties' Pixel aspect ratio and Rotation overrides applied to a frame as the reader made it.
static ImagePtr reshapedMedia(const ImagePtr& img, double f, int rot)
{
    if (!img || img->w < 1 || img->h < 1) return img;
    ImagePtr cur = img;
    int nw = img->w, nh = img->h;
    if (f > 1.0005) nw = std::max(1, (int)std::lround(img->w * f));
    else if (f < 0.9995) nh = std::max(1, (int)std::lround(img->h / f));
    if (nw != img->w || nh != img->h) {
        ImagePtr out = Image::make(nw, nh, img->depth, false);
        if (!out) return img;
        const bool byte = img->depth == PixelDepth::Byte;
        parallelRows(nh, (size_t)nw * 40, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            const double sy = std::min((double)img->h - 1, std::max(0.0, (y + 0.5) * img->h / nh - 0.5));
            const int y0 = (int)sy, y1 = std::min(img->h - 1, y0 + 1);
            const double fy = sy - y0;
            for (int x = 0; x < nw; ++x) {
                const double sx = std::min((double)img->w - 1, std::max(0.0, (x + 0.5) * img->w / nw - 0.5));
                const int x0 = (int)sx, x1 = std::min(img->w - 1, x0 + 1);
                const double fx = sx - x0;
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](int xx, int yy) -> double {
                        return byte ? (double)img->row(yy)[xx * 4 + c] : (double)((const float*)img->row(yy))[xx * 4 + c];
                    };
                    const double v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
                    if (byte) out->row(y)[x * 4 + c] = (uint8_t)std::min(255.0, std::max(0.0, v + 0.5));
                    else ((float*)out->row(y))[x * 4 + c] = (float)v;
                }
            }
        }
        });
        cur = out;
    }
    if (rot == 90 || rot == 180 || rot == 270) {
        const int w = cur->w, h = cur->h;
        const int ow = rot == 180 ? w : h, oh = rot == 180 ? h : w;
        ImagePtr out = Image::make(ow, oh, cur->depth, false);
        if (!out) return cur;
        const size_t px = cur->pixelBytes();
        // row 0 is the bottom row: clockwise 90 puts the pixel at (x, y) at (y, w - 1 - x); 270 at (h - 1 - y, x).
        parallelRows(h, (size_t)w * 8, [&](int ya, int yb) {
        for (int y = ya; y < yb; ++y) {
            const uint8_t* src = cur->row(y);
            for (int x = 0; x < w; ++x) {
                int dx, dy;
                if (rot == 90) { dx = y; dy = w - 1 - x; }
                else if (rot == 180) { dx = w - 1 - x; dy = h - 1 - y; }
                else { dx = h - 1 - y; dy = x; }
                std::memcpy(out->row(dy) + (size_t)dx * px, src + (size_t)x * px, px);
            }
        }
        });
        cur = out;
    }
    return cur;
}

void MediaPool::setUseProxies(bool on) { useProxies_ = on; }

MediaItem MediaPool::proxyFor(const MediaItem& m)
{
    MediaItem pm;
    if (!useProxies_ || m.generated || m.path.empty() || !m.info.hasVideo || m.info.isStill || isProjectFile(m.path)) return pm;
    bool has = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const uint64_t g = videoProxyGeneration();
        if (g != proxyGeneration_) {
            // proxies were made or removed: look again, and drop the readers of the old ones (a thread still using
            // one keeps it until it is done)
            proxyKnown_.clear();
            proxyGeneration_ = g;
            video_.erase(std::remove_if(video_.begin(), video_.end(), [](const EntryPtr& x) { return (x->media & 0x2000000000000000ull) != 0; }),
                         video_.end());
        }
        auto it = proxyKnown_.find(m.path);
        if (it == proxyKnown_.end()) it = proxyKnown_.emplace(m.path, hasVideoProxy(m.path)).first;
        has = it->second;
    }
    if (!has) return pm;
    pm.id = m.id | 0x2000000000000000ull;
    pm.path = videoProxyPath(m.path);
    pm.info = m.info;
    return pm;
}

ImagePtr MediaPool::videoFrame(uint64_t user, const MediaItem& m0, double seconds, int w, int h, int deinterlace, PixelDepth depth, double reserveSeconds)
{
    profile::MediaScope prof(m0.path);
    const MediaItem proxy = proxyFor(m0);
    const MediaItem& m = proxy.id ? proxy : m0;
    // Media Properties' Pixel aspect ratio and Rotation: the reader is asked for a frame whose reshaped self fits the box
    const double f = m0.pixelFactor();
    const int rot = m0.rotationOverride;
    int bw = w, bh = h;
    if (!(f == 1.0 && !rot)) {
        if (rot == 90 || rot == 270) std::swap(bw, bh);
        if (f > 1.0005) bw = std::max(1, (int)std::lround(bw / f));
        else if (f < 0.9995) bh = std::max(1, (int)std::lround(bh * f));
    }
    std::shared_ptr<FrameShare> share = (depth == PixelDepth::Float && m.info.bitDepth > 8) ? nullptr : share_;
    const uint64_t shape = share ? shapeOf(m, bw, bh, deinterlace) : 0;
    const uint64_t generation = share ? share->generation() : 0;
    auto finish = [&](const ImagePtr& got) {
        if (f == 1.0 && !rot) return withAlphaMode(got, m0.alphaMode);
        return withAlphaMode(reshapedMedia(got, f, rot), m0.alphaMode);
    };
    if (share && reserveSeconds <= 0) {
        if (ImagePtr hit = share->find(m.id, shape, seconds)) {
            ++stats_.videoShared;
            return finish(hit);
        }
    }
    std::unique_lock<std::mutex> lk;
    EntryPtr e = get(video_, maxVideo_ ? maxVideo_ : kMaxVideoReaders, maxVideo_ ? std::min(maxVideo_, kVideoReadersPerMedia) : kVideoReadersPerMedia, user, m,
                     [&](Entry& x) { return x.deinterlace != deinterlace ? 2 : x.reader->videoCost(seconds, w, h); }, lk, reserveSeconds,
                     share && reserveSeconds <= 0 ? seconds : std::numeric_limits<double>::quiet_NaN(), shape);
    if (!e) return nullptr;
    struct Idle {
        Entry& e;
        ~Idle() { e.busyWant = std::numeric_limits<double>::quiet_NaN(); }
    } idle{*e};
    if (!e->reader && proxy.id) {
        // a proxy that cannot be read: the original from now on
        lk = std::unique_lock<std::mutex>();
        {
            std::lock_guard<std::mutex> g(mutex_);
            proxyKnown_[m0.path] = false;
        }
        return videoFrame(user, m0, seconds, w, h, deinterlace, depth, reserveSeconds);
    }
    if (!e->reader) return nullptr;
    // waited for another thread's reader: what it made may be the frame.
    if (share && reserveSeconds <= 0) {
        if (ImagePtr hit = share->find(m.id, shape, seconds)) {
            ++stats_.videoShared;
            return finish(hit);
        }
    }
    if (deinterlace != e->deinterlace) {
        e->reader->setDeinterlace(deinterlace);
        e->deinterlace = deinterlace;
    }
    if (m0.fieldOrderOverride != e->field) {
        e->reader->setFieldOrder(m0.fieldOrderOverride);
        e->field = m0.fieldOrderOverride;
    }
    if (depth == PixelDepth::Float && e->reader->info().bitDepth > 8) return finish(e->reader->videoFrameInDepth(seconds, bw, bh, depth));
    double from = 1.0, until = 0.0;
    ImagePtr got = share ? e->reader->videoFrameSpan(seconds, bw, bh, &from, &until) : e->reader->videoFrame(seconds, bw, bh);
    if (share && got && from <= until) share->put(m.id, shape, from, until, got, generation);
    if (share && got && !mediaReadHints().cancelled()) {
        const uint64_t id = m.id;
        e->reader->harvestKept(bw, bh, kHarvestFrames, [&](double f, double u, const ImagePtr& img) { share->put(id, shape, f, u, img, generation); });
    }
    return finish(got);
}

void MediaPool::prewarmAudio(const Project& p, double from, double span, const std::atomic<bool>* stop)
{
    std::map<uint64_t, int> events;                         // media -> how many of the events playing want its sound
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Audio || t.mute) continue;
        for (const Event& ev : t.events)
            if (!ev.mute && ev.start < from + span && ev.start + ev.length > from) ++events[ev.mediaId];
    }
    const int rate = p.settings.sampleRate;
    for (const auto& w : events) {
        if (stop && stop->load()) return;
        const MediaItem* m = p.findMedia(w.first);
        if (!m || m->offline || m->generated || m->info.isStill || !m->info.hasAudio || rate <= 0 || isProjectFile(m->path)) continue;
        const size_t perMedia = maxAudio_ ? std::min(maxAudio_, kAudioReadersPerMedia) : kAudioReadersPerMedia;
        size_t have = 0;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            for (const EntryPtr& x : audio_)
                if (x->media == m->id && x->reader) ++have;
        }
        const size_t want = std::min<size_t>((size_t)w.second, perMedia);
        std::vector<EntryPtr> made;
        for (size_t i = have; i < want; ++i) {
            if (stop && stop->load()) break;
            std::unique_lock<std::mutex> lk;
            EntryPtr e = get(audio_, maxAudio_ ? maxAudio_ : kMaxAudioReaders, perMedia, (1ull << 60) + i, *m, [](Entry&) { return 2; }, lk);
            if (!e->reader || !e->reader->info().hasAudio) break;
            float a = 0, b = 0;
            e->reader->audio(0, 1, rate, &a, &b);
            e->rate = rate;
            e->first = -1;
            e->count = 0;
            e->nextStart = 1;
            e->audioUser = 0;
            made.push_back(e);
        }
        std::lock_guard<std::mutex> lk(mutex_);
        for (const EntryPtr& e : made) {
            e->lastUse = 0;
            e->lastUser = 0;
        }
    }
}

bool MediaPool::audio(uint64_t user, const MediaItem& m, int64_t start, int frames, int rate, float* left, float* right, int firstChannel,
                      int channelCount)
{
    const int first = firstChannel >= 0 ? firstChannel : -1, count = firstChannel >= 0 ? channelCount : 0;
    std::unique_lock<std::mutex> lk;
    EntryPtr e = get(audio_, maxAudio_ ? maxAudio_ : kMaxAudioReaders, maxAudio_ ? std::min(maxAudio_, kAudioReadersPerMedia) : kAudioReadersPerMedia, user, m,
                     [&](Entry& x) { return (x.rate == rate && x.first == first && x.count == count && std::llabs(start - x.nextStart) <= (x.audioUser == user ? 2 : 0)) ? 0 : 2; },
                     lk);
    if (!e->reader || !e->reader->info().hasAudio) return false;
    ++stats_.audioReads;
    if (!(e->rate == rate && e->first == first && e->count == count && std::llabs(start - e->nextStart) <= (e->audioUser == user ? 2 : 0))) ++stats_.audioSeeks;
    if (rate == m.info.sampleRate) ++stats_.audioNative;
    // A read of the same event that lands within a couple of samples of where its last one ended is meant to carry
    // straight on.
    if (e->rate == rate && e->first == first && e->count == count && e->audioUser == user && std::llabs(start - e->nextStart) <= 2) start = e->nextStart;
    e->audioUser = user;
    const bool ok = firstChannel >= 0 ? e->reader->audioPick(start, frames, rate, firstChannel, channelCount, left, right)
                                      : e->reader->audio(start, frames, rate, left, right);
    e->rate = rate;
    e->first = first;
    e->count = count;
    e->nextStart = start + frames;
    return ok;
}

// ====================================================================== shared helpers
template <class List, class Get>
static double overlapWeightOf(const List& events, Get get, const Event& e, double t, bool crossfade)
{
    if (!crossfade) {
        for (const auto& item : events) {
            const Event& o = get(item);
            if (o.id == e.id || o.mute || o.end() <= t || o.start > t) continue;
            if (o.start > e.start || (o.start == e.start && o.id > e.id)) return 0.0;
        }
        return 1.0;
    }
    double w = 1.0;
    for (const auto& item : events) {
        const Event& o = get(item);
        if (o.id == e.id || o.mute) continue;
        if (o.end() <= t || o.start > t) continue;                  // not playing now
        if (o.start < e.start || (o.start == e.start && o.id < e.id)) {
            // 'o' began first: this event is fading in over the stretch they share
            // (shaped by its fade-in curve, as VEGAS shapes a cross-fade; linear gives exactly the straight ramp)
            if (o.end() < e.end()) {
                const double span = o.end() - e.start;
                if (span > 0) w = std::min(w, e.fadeInCurve == CurveLinear ? std::max(0.0, (t - e.start) / span) : curveShape(e.fadeInCurve, (t - e.start) / span));
            }
        } else {
            // 'o' began later: this event fades out under it.
            if (o.end() > e.end()) {
                const double span = e.end() - o.start;
                const int c = o.fadeInRecipCurve >= 0 ? o.fadeInRecipCurve : e.fadeOutCurve;
                if (span > 0)
                    w = std::min(w, c == CurveLinear ? std::max(0.0, 1.0 - (t - o.start) / span)
                                                     : 1.0 - curveShape(c, (t - o.start) / span));
            } else {
                w = 0.0;                                            // 'o' sits wholly inside this event and covers it
            }
        }
    }
    return w;
}

// How much of an event shows at time t because of the events it overlaps on its track: where two overlap, the earlier
// one fades out as the later one fades in.
double overlapWeight(const Track& tr, const Event& e, double t, bool crossfade)
{
    return overlapWeightOf(tr.events, [](const Event& o) -> const Event& { return o; }, e, t, crossfade);
}

double overlapWeightAmong(const std::vector<const Event*>& others, const Event& e, double t, bool crossfade)
{
    return overlapWeightOf(others, [](const Event* o) -> const Event& { return *o; }, e, t, crossfade);
}

void overlappingEvents(const Track& tr, const Event& e, std::vector<const Event*>& out)
{
    out.clear();
    const double a = e.start - 1e-6, b = e.end() + 1e-6;
    for (const auto& o : tr.events)
        if (o.id != e.id && !o.mute && o.start <= b && o.end() >= a) out.push_back(&o);
}

// dst += src * f (8-bit: clamped)
void addScaled(Image& dst, const Image& src, float f)
{
    if (dst.w != src.w || dst.h != src.h || f <= 0.f || dst.depth != src.depth) return;
    if (dst.depth == PixelDepth::Float) {
        f = std::min(f, 1.f);
        parallelRows(dst.h, (size_t)dst.w * 4, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                float* d = (float*)dst.row(y);
                const float* s = (const float*)src.row(y);
                for (int i = 0, n = dst.w * 4; i < n; ++i) d[i] += s[i] * f;
            }
        });
        return;
    }
    const unsigned m = (unsigned)(std::min(f, 1.f) * 256.f + 0.5f);
    parallelRows(dst.h, (size_t)dst.w * 4, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) pix::addScaledRow(dst.row(y), src.row(y), dst.w * 4, m);
    });
}

// ====================================================================== projects used as media
// A project used as media is read once: one shared copy serves every event, reader and thread that uses it, and is
// read again only when its file changes (size or time).
namespace {
std::mutex gNestedMutex;
ProjectFileLoader gProjectLoader;
MainThreadRunner gMainRunner;
MainThreadPoster gMainPoster;
std::function<bool()> gIsMainThread;
std::function<void(const std::string&)> gReadyHook;
std::atomic<uint64_t> gNestedGeneration{1};     // goes up whenever some nested project's effects have all been made
std::atomic<uint64_t> gNestedFileGeneration{1}; // goes up whenever a project file read as media is found saved again
std::atomic<int64_t> gNestedCheckedAt{0};       // (ms, steady clock) when those files were last looked at
thread_local int gNestedDepth = 0;              // nested projects being rendered on this thread, one inside another
thread_local int gFinalDepth = 0;               // final renders going on on this thread
thread_local uint64_t gLeftOut = 0;             // renders on this thread that left a nested project's effects out
const int kMaxNestedDepth = 16;
const double kStepMs = 6.0;                     // how long one piece of making instances may hold the interface

bool loadNestedProject(const std::string& path, Project& out, std::string* error)
{
    ProjectFileLoader loader;
    {
        std::lock_guard<std::mutex> lk(gNestedMutex);
        loader = gProjectLoader;
    }
    std::string err;
    if (loader) {
        Project p;
        if (loader(path, p, &err)) {
            out = std::move(p);
            return true;
        }
    }
    if (endsWithNoCase(path, ".reno")) {
        Project p;
        std::string e2;
        if (p.load(path, &e2)) {
            out = std::move(p);
            return true;
        }
        if (err.empty()) err = e2;
    }
    if (error) *error = err.empty() ? pathFileName(path) + " cannot be read as a project" : err;
    return false;
}

// A path as one name for one file (so that a project named two ways is still found to contain itself).
std::string samePath(const std::string& p)
{
    try {
        std::filesystem::path fp = std::filesystem::u8path(p);
        std::string s = std::filesystem::weakly_canonical(fp).u8string();
#ifdef _WIN32
        s = toLower(s);
#endif
        return s;
    } catch (...) {
        return p;
    }
}

// A file's size and time of its last change: when either differs, it has been saved again.
struct Stamp {
    int64_t size = -1;
    int64_t time = 0;
    bool operator==(const Stamp& o) const { return size == o.size && time == o.time; }
};

Stamp stampOf(const std::string& path)
{
    Stamp s;
    try {
        const std::filesystem::path fp = std::filesystem::u8path(path);
        std::error_code ec;
        const auto size = std::filesystem::file_size(fp, ec);
        if (!ec) s.size = (int64_t)size;
        const auto t = std::filesystem::last_write_time(fp, ec);
        if (!ec) s.time = (int64_t)t.time_since_epoch().count();
    } catch (...) {
    }
    return s;
}

// A .veg names its media without their length, rate and channels (only a subclip's length).
void completeMediaInfo(Project& p)
{
    for (auto& m : p.media) {
        if (m.generated || m.offline || m.info.isStill || m.info.duration > 0 || m.path.empty() || m.path.compare(0, 6, "synth:") == 0) continue;
        if (isProjectFile(m.path) || !fileExists(m.path)) continue;
        std::unique_ptr<MediaReader> r = openMedia(m.path, nullptr);
        if (!r) continue;
        const MediaInfo& in = r->info();
        m.info.duration = in.duration;
        m.info.sampleRate = in.sampleRate;
        m.info.channels = in.channels;
        m.info.channelNames = in.channelNames;
        m.info.channelLayout = in.channelLayout;
        m.info.fps = in.fps;
        if (m.info.width <= 0) {
            m.info.width = in.width;
            m.info.height = in.height;
        }
        m.info.hasAudio = m.info.hasAudio || in.hasAudio;
        m.info.hasVideo = m.info.hasVideo || in.hasVideo;
    }
}

// The one copy of each project file read as media.
struct LoadedProject {
    std::mutex mutex;                           // held while it is read
    bool loaded = false;
    Stamp stamp;
    std::shared_ptr<const Project> project;     // null if it could not be read
    std::string error;
    std::vector<std::string> uses;              // the project files it uses as media
};
std::mutex gLoadedMutex;
std::map<std::string, std::shared_ptr<LoadedProject>> gLoaded;

std::shared_ptr<const Project> sharedProject(const std::string& path, std::string* error, std::vector<std::string>* uses = nullptr)
{
    const std::string key = samePath(path);
    const Stamp stamp = stampOf(path);
    std::shared_ptr<LoadedProject> e;
    {
        std::lock_guard<std::mutex> lk(gLoadedMutex);
        std::shared_ptr<LoadedProject>& slot = gLoaded[key];
        if (!slot) slot = std::make_shared<LoadedProject>();
        e = slot;
    }
    std::lock_guard<std::mutex> lk(e->mutex);
    if (!e->loaded || !(e->stamp == stamp)) {
        Project p;
        std::string err;
        e->uses.clear();
        if (loadNestedProject(path, p, &err)) {
            completeMediaInfo(p);
            for (const auto& m : p.media)
                if (!m.generated && isProjectFile(m.path)) e->uses.push_back(m.path);
            e->project = std::make_shared<const Project>(std::move(p));
            e->error.clear();
        } else {
            e->project.reset();
            e->error = err;
        }
        e->stamp = stamp;
        e->loaded = true;
    }
    if (!e->project && error) *error = e->error;
    if (uses) *uses = e->uses;
    return e->project;
}

// Does the project at 'path' come back to the first project in 'chain' (the projects on the way there), or, with
// 'chain' empty, to itself - directly or through the projects it uses?
bool containsLoop(const std::string& path, std::vector<std::string>& chain, std::map<std::string, std::vector<std::string>>& seen, std::string* which)
{
    const std::string me = samePath(path);
    if (!chain.empty() && chain.front() == me) {
        if (which) *which = chain.back();
        return true;
    }
    for (const auto& c : chain)
        if (c == me) return false;          // a circle that does not come back to the first
    auto it = seen.find(me);
    if (it == seen.end()) {
        std::vector<std::string> uses;
        sharedProject(path, nullptr, &uses);
        it = seen.emplace(me, uses).first;
    }
    if (chain.size() > 64) return false;
    chain.push_back(me);
    const std::vector<std::string> uses = it->second;
    for (const auto& u : uses)
        if (containsLoop(u, chain, seen, which)) return true;
    chain.pop_back();
    return false;
}

bool onMainThread()
{
    std::function<bool()> f;
    {
        std::lock_guard<std::mutex> lk(gNestedMutex);
        f = gIsMainThread;
    }
    return !f || f();
}

// Hands 'fn' to the interface's thread; false if there is none to hand it to.
bool postToMain(std::function<void()> fn)
{
    MainThreadPoster post;
    {
        std::lock_guard<std::mutex> lk(gNestedMutex);
        post = gMainPoster;
    }
    if (!post) return false;
    post(std::move(fn));
    return true;
}

// The plug-in instances of a nested project's effects.
struct NestedFx {
    std::shared_ptr<const Project> project;
    std::string path;
    bool video = true, audio = true;            // which kinds of effect
    FxRuntime fx;
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false, ready = false;        // under mutex
    size_t next = 0;                            // the next slot to make
    std::atomic<size_t> progress{0};
    std::mutex making;                          // held while they are made all at once on some thread
    std::chrono::steady_clock::time_point retryAt;
};
using NestedFxPtr = std::shared_ptr<NestedFx>;

void postReadyHook(const std::string& path);

// True if a project file read as media has been saved again since it was read, in another Reno window (Open Nested
// Timeline) or by VEGAS.
void checkNestedFiles()
{
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t last = gNestedCheckedAt.load();
    if (now - last < 1500 || !gNestedCheckedAt.compare_exchange_strong(last, now)) return;
    std::vector<std::pair<std::string, std::shared_ptr<LoadedProject>>> loaded;
    {
        std::lock_guard<std::mutex> lk(gLoadedMutex);
        for (const auto& kv : gLoaded) loaded.push_back(kv);
    }
    std::vector<std::string> changed;
    for (const auto& kv : loaded) {
        std::unique_lock<std::mutex> lk(kv.second->mutex, std::try_to_lock);
        if (!lk.owns_lock() || !kv.second->loaded) continue;
        if (!(stampOf(kv.first) == kv.second->stamp)) changed.push_back(kv.first);
    }
    if (changed.empty()) return;
    gNestedFileGeneration.fetch_add(1);
    gNestedGeneration.fetch_add(1);
    for (const std::string& path : changed) postReadyHook(path);
}

NestedFxPtr makeNestedFx(std::shared_ptr<const Project> p, const std::string& path, bool video, bool audio)
{
    NestedFx* f = new NestedFx();
    f->project = std::move(p);
    f->path = path;
    f->video = video;
    f->audio = audio;
    // plug-in instances are let go of on the thread they were made on: the interface's, when there is one.
    return NestedFxPtr(f, [](NestedFx* x) {
        if (!onMainThread() && postToMain([x] { delete x; })) return;
        delete x;
    });
}

void markReady(const NestedFxPtr& f, bool tell)
{
    {
        std::lock_guard<std::mutex> lk(f->mutex);
        if (f->ready) return;
        f->ready = true;
    }
    f->cv.notify_all();
    gNestedGeneration.fetch_add(1);
    if (tell) postReadyHook(f->path);
}

void postReadyHook(const std::string& path)
{
    std::function<void(const std::string&)> hook;
    {
        std::lock_guard<std::mutex> lk(gNestedMutex);
        hook = gReadyHook;
    }
    if (!hook) return;
    if (!postToMain([hook, path] { hook(path); })) hook(path);
}

// One piece of making a nested project's instances, on the interface's thread; the next piece is handed over again,
// so that the interface gets on with everything else in between.
thread_local bool tInStep = false;
void prepareStep(NestedFxPtr f)
{
    if (f.use_count() == 1) return;             // nobody wants them any more
    const bool done = f->fx.prepareSome(*f->project, f->next, kStepMs, f->video, f->audio);
    f->progress = f->next;
    if (!done) {
        // a poster that runs what it is given at once - the application is shutting down - would come straight back.
        if (!tInStep) {
            tInStep = true;
            const bool posted = postToMain([f] { prepareStep(f); });
            tInStep = false;
            if (posted) return;
        }
        while (!f->fx.prepareSome(*f->project, f->next, 1e9, f->video, f->audio)) {}
        f->progress = f->next;
    }
    markReady(f, true);
}

// Whether the instances are all made (starting to make them the first time).
bool nestedFxReady(const NestedFxPtr& f, bool wait)
{
    {
        std::lock_guard<std::mutex> lk(f->mutex);
        if (f->ready) return true;
    }
    MainThreadRunner runner;
    bool poster = false;
    {
        std::lock_guard<std::mutex> lk(gNestedMutex);
        runner = gMainRunner;
        poster = (bool)gMainPoster;
    }
    if (runner && !poster) {
        const auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lk(f->mutex);
            if (now < f->retryAt) return false;
        }
        const bool ran = runner([&f] {
            while (!f->fx.prepareSome(*f->project, f->next, 1e9, f->video, f->audio)) {}
            f->progress = f->next;
        });
        if (ran) {
            markReady(f, false);
            return true;
        }
        std::lock_guard<std::mutex> lk(f->mutex);
        f->retryAt = now + std::chrono::seconds(5);
        return false;
    }
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(f->mutex);
        if (f->ready) return true;
        if (!f->started) start = f->started = true;
    }
    const bool main = onMainThread();
    bool noPoster = false;
    if (start) {
        if (main && !wait) {
            if (postToMain([f] { prepareStep(f); })) return false;
            noPoster = true;
        } else if (!main) {
            if (postToMain([f] { prepareStep(f); })) {
                if (!wait) return false;
            } else {
                noPoster = true;
            }
        }
    }
    if (noPoster || (main && (wait || start))) {
        // no interface thread to make them on (the command-line tools, tests), or this is it and it needs them now:
        // made here, at once (as they always were then; one thread at a time)
        std::lock_guard<std::mutex> lk(f->making);
        while (!f->fx.prepareSome(*f->project, f->next, 1e9, f->video, f->audio)) {}
        f->progress = f->next;
        markReady(f, main && !noPoster);
        return true;
    }
    if (!wait) return false;
    if (main) return false;
    // a final render on another thread: waits while the interface makes them - and goes on without them if it stops
    // getting to it (it may be waiting for this render)
    std::unique_lock<std::mutex> lk(f->mutex);
    size_t seen = f->progress.load();
    auto since = std::chrono::steady_clock::now();
    while (!f->ready) {
        f->cv.wait_for(lk, std::chrono::milliseconds(100));
        const auto now = std::chrono::steady_clock::now();
        if (f->progress.load() != seen) {
            seen = f->progress.load();
            since = now;
        } else if (now - since > std::chrono::seconds(20)) {
            logf("nested project %s: its effects are not ready; rendering without them", pathFileName(f->path).c_str());
            return false;
        }
    }
    return true;
}

// One mixer of a nested project's sound: the audio effects' instances.
struct NestedAudio {
    int rate = 0;
    int64_t next = std::numeric_limits<int64_t>::min();     // the sample after the last one it mixed
    MediaPool pool;
    NestedFxPtr fx;
    std::unique_ptr<AudioEngine> engine;
};

// What every reader of one nested project shares: the project as read, the readers of its media, its video effects,
// the frames made last, and the sound mixers not in use.
struct NestedShared {
    std::string path;
    std::shared_ptr<const Project> project;
    MediaPool pool;
    NestedFxPtr videoFx;

    struct CachedFrame { int64_t frame; uint64_t generation; ImagePtr img; };
    std::mutex mutex;                           // guards frames and idleAudio
    std::list<CachedFrame> frames;
    std::vector<std::unique_ptr<NestedAudio>> idleAudio;

    // Its audio proxy (see currentProxy below), as last found, and when that was.
    std::mutex proxyMutex;
    std::shared_ptr<vegasfiles::AudioProxyFile> proxy;
    uint64_t proxyGeneration = 0;
    std::chrono::steady_clock::time_point proxyChecked;
    int64_t proxyVegTime = 0, proxyFileTime = 0;

    ImagePtr cachedFrame(int64_t frame, int w, int h, uint64_t generation)
    {
        std::lock_guard<std::mutex> lk(mutex);
        for (auto it = frames.begin(); it != frames.end(); ++it)
            if (it->frame == frame && it->img->w == w && it->img->h == h && it->generation == generation) {
                frames.splice(frames.begin(), frames, it);
                return frames.front().img;
            }
        return nullptr;
    }
    void keepFrame(int64_t frame, uint64_t generation, const ImagePtr& img)
    {
        std::lock_guard<std::mutex> lk(mutex);
        frames.push_front({frame, generation, img});
        while (frames.size() > 12) frames.pop_back();
    }

    // A mixer for one reader. A final render always gets a new one; otherwise an idle one is reused only if it stopped
    // exactly where the reader starts, so that it goes on as it would have for the reader that left it.
    std::unique_ptr<NestedAudio> takeAudio(int rate, int64_t at, bool fresh = false)
    {
        if (!fresh) {
            std::lock_guard<std::mutex> lk(mutex);
            size_t pick = idleAudio.size();
            for (size_t k = 0; k < idleAudio.size(); ++k)
                if (idleAudio[k]->rate == rate && idleAudio[k]->next == at) pick = k;
            if (pick < idleAudio.size()) {
                std::unique_ptr<NestedAudio> a = std::move(idleAudio[pick]);
                idleAudio.erase(idleAudio.begin() + (std::ptrdiff_t)pick);
                return a;
            }
        }
        std::unique_ptr<NestedAudio> a(new NestedAudio());
        a->pool.setReaderLimits(2, 12);
        a->rate = rate;
        std::shared_ptr<const Project> p = project;
        if (p->settings.sampleRate != rate) {
            auto copy = std::make_shared<Project>(*p);
            copy->settings.sampleRate = rate;
            p = copy;
        }
        a->fx = makeNestedFx(p, path, false, true);
        a->engine.reset(new AudioEngine(a->pool, a->fx->fx));
        return a;
    }
    void closeFiles()
    {
        pool.clear();
        std::lock_guard<std::mutex> lk(mutex);
        for (auto& a : idleAudio) a->pool.clear();
    }

    void giveBack(std::unique_ptr<NestedAudio> a)
    {
        if (!a) return;
        std::unique_ptr<NestedAudio> drop;
        {
            std::lock_guard<std::mutex> lk(mutex);
            if (idleAudio.size() < 4) idleAudio.push_back(std::move(a));
            else drop = std::move(a);
        }
    }
};
std::mutex gSharedMutex;
std::map<std::string, std::weak_ptr<NestedShared>> gShared;
// the nested projects used last, kept a while after their last reader has gone (readers come and go all the time)
std::deque<std::shared_ptr<NestedShared>>* gRecent = new std::deque<std::shared_ptr<NestedShared>>();
const size_t kRecentNested = 24;

std::shared_ptr<NestedShared> nestedShared(const std::string& path, std::string* error)
{
    std::shared_ptr<const Project> proj = sharedProject(path, error);
    if (!proj) {
        if (error && error->empty()) *error = pathFileName(path) + " cannot be read as a project";
        return nullptr;
    }
    const std::string key = samePath(path);
    std::shared_ptr<NestedShared> dropped;
    std::lock_guard<std::mutex> lk(gSharedMutex);
    std::weak_ptr<NestedShared>& slot = gShared[key];
    std::shared_ptr<NestedShared> sh = slot.lock();
    if (!sh || sh->project != proj) {
        sh = std::make_shared<NestedShared>();
        sh->pool.setReaderLimits(4, 8);
        sh->path = path;
        sh->project = proj;
        sh->videoFx = makeNestedFx(proj, path, true, false);
        slot = sh;
    }
    auto it = std::find(gRecent->begin(), gRecent->end(), sh);
    if (it != gRecent->end()) gRecent->erase(it);
    // The ones nobody reads now keep their project and effects, but not their open files (decoders hold a good deal
    // of memory, and dozens of nested projects can be in use by turns).
    for (const std::shared_ptr<NestedShared>& idle : *gRecent)
        if (idle.use_count() == 1) idle->closeFiles();
    gRecent->push_front(sh);
    if (gRecent->size() > kRecentNested) {
        dropped = std::move(gRecent->back());
        gRecent->pop_back();
    }
    return sh;
}

// ====================================================================== nested projects' audio proxies
// The sound of a nested project, rendered once into "<project>.sfap0" in VEGAS's own format.
std::atomic<bool> gProxyUse{true}, gProxyCreate{false};
std::atomic<uint64_t> gProxyGeneration{1};          // goes up whenever a proxy has been made

struct ProxyBook {
    std::mutex mutex;
    std::condition_variable cv, idle;
    std::deque<std::pair<std::string, bool>> queue;     // (path, force) waiting to be built
    std::set<std::string> waiting;                      // samePath of those, and of the one being built
    std::string building;                               // samePath of the one being built
    std::map<std::string, std::chrono::steady_clock::time_point> failedAt;     // not tried again for a while
    std::set<std::string> cancelled;                    // cancelled by the user: not built until forced
    std::map<std::string, std::vector<std::weak_ptr<vegasfiles::AudioProxyFile>>> open;    // proxies being read, by samePath
    std::function<void(const std::string&, double, int, const std::string&)> hook;
    bool stop = false;                                  // the build going on is to stop
    bool running = false;                               // the thread is there
    uint64_t stopGeneration = 0;
};
ProxyBook& proxyBook()
{
    static ProxyBook* b = new ProxyBook();
    return *b;
}

void tellProxy(const std::string& path, double fraction, int state, const std::string& message)
{
    std::function<void(const std::string&, double, int, const std::string&)> hook;
    {
        std::lock_guard<std::mutex> lk(proxyBook().mutex);
        hook = proxyBook().hook;
    }
    if (hook) hook(path, fraction, state, message);
}

// The newest time of last change of the files the project at 'path' uses.
int64_t newestDependency(const std::string& path, std::set<std::string>& seen, int depth)
{
    if (depth > 8 || seen.size() > 1024 || !seen.insert(samePath(path)).second) return 0;
    std::shared_ptr<const Project> p = sharedProject(path, nullptr);
    if (!p) return 0;
    int64_t newest = 0;
    for (const auto& m : p->media) {
        if (m.generated || m.path.empty() || m.path.compare(0, 6, "synth:") == 0) continue;
        newest = std::max(newest, vegasfiles::fileTime(m.path));
        if (isProjectFile(m.path)) newest = std::max(newest, newestDependency(m.path, seen, depth + 1));
    }
    return newest;
}

// The current proxy of the project at 'path', opened; null if there is none.
std::shared_ptr<vegasfiles::AudioProxyFile> openCurrentProxy(const std::string& path, int64_t* vegTime, int64_t* proxyTime)
{
    const std::string pp = vegasfiles::audioProxyPath(path);
    const int64_t vt = vegasfiles::fileTime(path), pt = vegasfiles::fileTime(pp);
    if (vegTime) *vegTime = vt;
    if (proxyTime) *proxyTime = pt;
    if (vt == 0 || pt == 0) return nullptr;
    vegasfiles::AudioProxyInfo info;
    if (!vegasfiles::readAudioProxyInfo(pp, info) || !vegasfiles::audioProxyCurrent(info, vt)) return nullptr;
    std::set<std::string> seen;
    if (newestDependency(path, seen, 0) > pt) return nullptr;      // something it uses has changed since
    auto f = std::make_shared<vegasfiles::AudioProxyFile>();
    if (!f->open(pp)) return nullptr;
    std::lock_guard<std::mutex> lk(proxyBook().mutex);
    auto& list = proxyBook().open[samePath(path)];
    list.erase(std::remove_if(list.begin(), list.end(), [](const auto& w) { return w.expired(); }), list.end());
    list.push_back(f);
    return f;
}

// ---- Proxies held in memory: where the folder can't be written (a read-only or full disk), the sound of a nested project
// is rendered in the background into memory instead and read from there the same way, so playing it isn't mixed over
// and over either.
struct MemProxy {
    int64_t vegTime = 0, depTime = 0;
    int rate = 0;
    std::shared_ptr<const std::vector<float>> l, r;
    uint64_t lastUse = 0;
};
std::mutex gMemProxyMutex;
std::map<std::string, MemProxy> gMemProxies;
uint64_t gMemProxyTick = 0;
const size_t kMemProxyLimit = (size_t)768 << 20;

size_t memProxyBytes()
{
    size_t n = 0;
    for (const auto& kv : gMemProxies) n += (kv.second.l ? kv.second.l->size() : 0) * 2 * sizeof(float);
    return n;
}

// The in-memory proxy of the project at 'path', if it is current (made from the project as it is and what it uses as they are).
std::shared_ptr<vegasfiles::AudioProxyFile> memoryProxy(const std::string& path)
{
    const int64_t vt = vegasfiles::fileTime(path);
    if (vt == 0) return nullptr;
    std::set<std::string> seen;
    const int64_t dep = newestDependency(path, seen, 0);
    MemProxy mp;
    {
        std::lock_guard<std::mutex> lk(gMemProxyMutex);
        auto it = gMemProxies.find(samePath(path));
        if (it == gMemProxies.end() || it->second.vegTime != vt || it->second.depTime != dep) return nullptr;
        it->second.lastUse = ++gMemProxyTick;
        mp = it->second;
    }
    auto f = std::make_shared<vegasfiles::AudioProxyFile>();
    if (!f->openMemory(mp.rate, mp.l, mp.r)) return nullptr;
    return f;
}

void proxyWorker();

void requestProxy(const std::string& path, bool force)
{
    ProxyBook& b = proxyBook();
    const std::string key = samePath(path);
    {
        std::lock_guard<std::mutex> lk(b.mutex);
        if (b.waiting.count(key)) return;
        if (!force && b.cancelled.count(key)) return;
        if (force) b.cancelled.erase(key);
        if (!force) {
            auto it = b.failedAt.find(key);
            if (it != b.failedAt.end() && std::chrono::steady_clock::now() - it->second < std::chrono::seconds(60)) return;
        } else {
            b.failedAt.erase(key);
        }
        b.waiting.insert(key);
        b.queue.emplace_back(path, force);
        if (!b.running) {
            b.running = true;
            std::thread(proxyWorker).detach();
        }
    }
    b.cv.notify_all();
}

// Builds the proxy of the project at 'path'. 0 made, 1 failed, 2 stopped.
int buildProxy(const std::string& path, std::string* message)
{
    ProxyBook& b = proxyBook();
    uint64_t stopGen;
    {
        std::lock_guard<std::mutex> lk(b.mutex);
        stopGen = b.stopGeneration;
    }
    auto stopped = [&] {
        std::lock_guard<std::mutex> lk(b.mutex);
        return b.stop || b.stopGeneration != stopGen;
    };
    // the project file's time is taken before it is read: if it is saved meanwhile, the proxy is stale at once.
    const int64_t vegTime = vegasfiles::fileTime(path);
    std::string err;
    std::shared_ptr<const Project> proj = sharedProject(path, &err);
    if (!proj) {
        if (message) *message = err.empty() ? pathFileName(path) + " cannot be read as a project" : err;
        return 1;
    }
    const int rate = proj->settings.sampleRate > 0 ? proj->settings.sampleRate : 48000;
    // VEGAS's length: the project's length in whole samples (floor; 0.4450774 s at 48 kHz is 21363 samples)
    const int64_t frames = (int64_t)std::floor(proj->length() * rate + 1e-6);
    vegasfiles::AudioProxyWriter w;
    const std::string pp = vegasfiles::audioProxyPath(path);
    // Where the file cannot be written, the sound is kept in memory instead (if it is not too long for that)
    bool toMemory = false;
    std::shared_ptr<std::vector<float>> memL, memR;
    int64_t memDep = 0;
    if (!w.open(pp, rate, &err)) {
        if (frames <= 0 || (size_t)frames * 2 * sizeof(float) > kMemProxyLimit / 2) {
            if (message) *message = err;
            return 1;
        }
        toMemory = true;
        memL = std::make_shared<std::vector<float>>();
        memR = std::make_shared<std::vector<float>>();
        memL->reserve((size_t)frames);
        memR->reserve((size_t)frames);
        std::set<std::string> seen;
        memDep = newestDependency(path, seen, 0);
    }
    FinalRenderScope final;
    const uint64_t leftOut = nestedEffectsLeftOut();
    MediaPool pool;
    pool.setReaderLimits(2, 12);
    NestedFxPtr fx = makeNestedFx(proj, path, false, true);
    if (!nestedFxReady(fx, true)) {
        if (message) *message = "its audio effects could not be made";
        return 1;
    }
    {
        AudioEngine engine(pool, fx->fx);
        std::vector<float> l(4096), r(4096);
        auto lastTold = std::chrono::steady_clock::now();
        tellProxy(path, 0.0, 0, std::string());
        ++gNestedDepth;
        for (int64_t done = 0; done < frames;) {
            if (stopped()) {
                --gNestedDepth;
                w.abort();
                return 2;
            }
            const int n = (int)std::min<int64_t>(4096, frames - done);
            engine.render(*proj, done, n, l.data(), r.data(), true);
            if (toMemory) {
                memL->insert(memL->end(), l.begin(), l.begin() + n);
                memR->insert(memR->end(), r.begin(), r.begin() + n);
            } else if (!w.write(l.data(), r.data(), n)) {
                --gNestedDepth;
                w.abort();
                if (message) *message = "cannot write " + pathFileName(pp) + " (is the disk full?)";
                return 1;
            }
            done += n;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastTold > std::chrono::milliseconds(200)) {
                lastTold = now;
                tellProxy(path, (double)done / (double)frames, 0, std::string());
            }
        }
        --gNestedDepth;
    }
    if (toMemory) {
        if (nestedEffectsLeftOut() != leftOut) {
            if (message) *message = "the effects of a project it uses could not be made";
            return 1;
        }
        if (vegasfiles::fileTime(path) != vegTime) {
            if (message) *message = pathFileName(path) + " was saved again while its sound was being rendered";
            return 3;
        }
        {
            std::lock_guard<std::mutex> lk(gMemProxyMutex);
            MemProxy mp;
            mp.vegTime = vegTime;
            mp.depTime = memDep;
            mp.rate = rate;
            mp.l = memL;
            mp.r = memR;
            mp.lastUse = ++gMemProxyTick;
            gMemProxies[samePath(path)] = mp;
            while (memProxyBytes() > kMemProxyLimit && gMemProxies.size() > 1) {
                auto oldest = gMemProxies.begin();
                for (auto it = gMemProxies.begin(); it != gMemProxies.end(); ++it)
                    if (it->second.lastUse < oldest->second.lastUse) oldest = it;
                gMemProxies.erase(oldest);
            }
        }
        gProxyGeneration.fetch_add(1);
        return 0;
    }
    if (nestedEffectsLeftOut() != leftOut) {
        w.abort();
        if (message) *message = "the effects of a project it uses could not be made";
        return 1;
    }
    if (vegasfiles::fileTime(path) != vegTime) {
        w.abort();
        if (message) *message = pathFileName(path) + " was saved again while its sound was being rendered";
        return 3;
    }
    // the proxies of it being read are closed first (on Windows an open file cannot be replaced), then it is put in place
    {
        std::lock_guard<std::mutex> lk(b.mutex);
        auto it = b.open.find(samePath(path));
        if (it != b.open.end()) {
            for (auto& wp : it->second)
                if (auto f = wp.lock()) f->close();
            b.open.erase(it);
        }
    }
    const bool ok = w.finish(vegTime, &err);
    gProxyGeneration.fetch_add(1);
    if (!ok) {
        if (message) *message = err;
        return 1;
    }
    return 0;
}

void proxyWorker()
{
    lowerThreadPriority();
    ProxyBook& b = proxyBook();
    for (;;) {
        std::pair<std::string, bool> job;
        {
            std::unique_lock<std::mutex> lk(b.mutex);
            b.building.clear();
            b.idle.notify_all();
            if (!b.cv.wait_for(lk, std::chrono::seconds(30), [&] { return !b.queue.empty(); })) {
                b.running = false;
                return;
            }
            job = b.queue.front();
            b.queue.pop_front();
            b.building = samePath(job.first);
            b.stop = false;
        }
        // asked for, but current by now - VEGAS made it, or it was asked for twice.
        if (!job.second && (openCurrentProxy(job.first, nullptr, nullptr) || memoryProxy(job.first))) {
            std::lock_guard<std::mutex> lk(b.mutex);
            b.waiting.erase(b.building);
            continue;
        }
        std::string message;
        int r = buildProxy(job.first, &message);
        if (r == 1) logf("audio proxy for %s: %s", pathFileName(job.first).c_str(), message.c_str());
        {
            std::lock_guard<std::mutex> lk(b.mutex);
            if (r == 1) b.failedAt[b.building] = std::chrono::steady_clock::now();
            if (r == 3 && !b.stop) {
                b.queue.push_back(job);
                r = 2;
            } else {
                b.waiting.erase(b.building);
            }
        }
        tellProxy(job.first, r == 0 ? 1.0 : 0.0, r == 0 ? 1 : r == 1 ? 2 : 3, message);
    }
}

// The proxy a nested project's sound is read from now, or null (mixed instead).
std::shared_ptr<vegasfiles::AudioProxyFile> currentProxy(NestedShared& sh)
{
    if (!gProxyUse.load()) return nullptr;
    const auto now = std::chrono::steady_clock::now();
    const uint64_t gen = gProxyGeneration.load();
    std::lock_guard<std::mutex> lk(sh.proxyMutex);
    if (sh.proxyGeneration == gen && now - sh.proxyChecked < std::chrono::seconds(3)) return sh.proxy;
    // the project file or the proxy changed on disk: looked at afresh; otherwise only what they use is checked again.
    const bool recheck = sh.proxyGeneration != gen || !sh.proxy ||
                         vegasfiles::fileTime(sh.path) != sh.proxyVegTime ||
                         vegasfiles::fileTime(vegasfiles::audioProxyPath(sh.path)) != sh.proxyFileTime;
    sh.proxyChecked = now;
    sh.proxyGeneration = gen;
    if (!recheck) {
        std::set<std::string> seen;
        if (newestDependency(sh.path, seen, 0) <= sh.proxyFileTime) return sh.proxy;
    }
    sh.proxy = openCurrentProxy(sh.path, &sh.proxyVegTime, &sh.proxyFileTime);
    if (!sh.proxy) sh.proxy = memoryProxy(sh.path);
    if (!sh.proxy && gProxyCreate.load()) requestProxy(sh.path, false);
    return sh.proxy;
}
}  // namespace

// Decoding ahead inside nested projects (see engine.h).
namespace {
struct NestedWarm {
    double when;                    // timeline seconds
    double nestedAt;                // the time in its own project to read it at
    std::shared_ptr<NestedShared> sh;
    const Event* e;
    uint64_t key;
};
// The events of the project 'path' plays that come up while its time runs over [a, b].
void collectNestedWarm(const std::string& path, double a, double b, double at0, double rate, uint64_t keyBase, int depth,
                       std::vector<NestedWarm>& out)
{
    if (depth >= kMaxNestedDepth || !(b > a) || !(rate > 1e-6)) return;
    std::shared_ptr<NestedShared> sh = nestedShared(path, nullptr);
    if (!sh || !sh->project) return;
    const Project& proj = *sh->project;
    for (const Track& t : proj.tracks) {
        if (t.type != TrackType::Video || t.mute) continue;
        for (const Event& e : t.events) {
            if (e.mute || e.start >= b || e.start + e.length <= a) continue;
            const MediaItem* m = proj.findMedia(e.mediaId);
            if (!m || m->offline || !m->info.hasVideo) continue;
            const double from = std::max(a, e.start), to = std::min(b, e.start + e.length);
            const uint64_t key = (keyBase ^ (e.id + 0x9e3779b97f4a7c15ull + (keyBase << 6) + (keyBase >> 2))) * 0xff51afd7ed558ccdull;
            const double when = at0 + (from - a) / rate;
            if (isProjectFile(m->path)) {
                const double ea = eventMediaTime(e, m, from), eb = eventMediaTime(e, m, to);
                const double r = std::fabs(e.rate) > 1e-6 ? std::fabs(e.rate) * rate : rate;
                collectNestedWarm(m->path, std::min(ea, eb), std::max(ea, eb), when, r, key, depth + 1, out);
            } else if (!m->generated && !m->info.isStill) {
                out.push_back({when, from, sh, &e, key});
            }
        }
    }
}
}  // namespace

int VideoEngine::prewarmNested(const Project& p, const Event& e, double from, double to, const FrameOptions& opt, double keepSeconds,
                               const std::atomic<bool>* stop, const std::function<bool(uint64_t)>& claim, int most)
{
    const MediaItem* m = p.findMedia(e.mediaId);
    if (!m || m->offline || e.mute || !isProjectFile(m->path)) return 0;
    from = std::max(from, e.start);
    to = std::min(to, e.start + e.length);
    if (!(to > from)) return 0;
    const double a = eventMediaTime(e, m, from), b = eventMediaTime(e, m, to);
    const double rate = std::fabs(e.rate) > 1e-6 ? std::fabs(e.rate) : 1.0;
    std::vector<NestedWarm> list;
    collectNestedWarm(m->path, std::min(a, b), std::max(a, b), from, rate, e.id, 0, list);
    std::stable_sort(list.begin(), list.end(), [](const NestedWarm& x, const NestedWarm& y) { return x.when < y.when; });
    const int pw = std::max(1, p.settings.width), ph = std::max(1, p.settings.height);
    int done = 0;
    for (const NestedWarm& w : list) {
        if (done >= most || (stop && stop->load())) break;
        if (claim && !claim(w.key)) continue;
        const Project& np = *w.sh->project;
        FrameOptions fo = opt;
        const double fit = std::min((double)pw / std::max(1, np.settings.width), (double)ph / std::max(1, np.settings.height));
        fo.scale = std::min(1.0, std::max(0.01, opt.scale * fit));
        VideoEngine nested(w.sh->pool, w.sh->videoFx->fx);
        if (nested.prewarmEventAt(np, *w.e, w.nestedAt, fo, keepSeconds, stop)) ++done;
    }
    return done;
}

void setAudioProxyOptions(bool use, bool create)
{
    gProxyUse.store(use);
    gProxyCreate.store(create);
    gProxyGeneration.fetch_add(1);
}

void setAudioProxyProgressHook(std::function<void(const std::string& path, double fraction, int state, const std::string& message)> hook)
{
    std::lock_guard<std::mutex> lk(proxyBook().mutex);
    proxyBook().hook = std::move(hook);
}

void buildAudioProxy(const std::string& path, bool force)
{
    if (!isProjectFile(path)) return;
    requestProxy(path, force);
}

bool audioProxyCurrent(const std::string& path) { return (bool)openCurrentProxy(path, nullptr, nullptr); }

int64_t mediaStamp(const std::string& path)
{
    int64_t t = vegasfiles::fileTime(path);
    if (t != 0 && isProjectFile(path)) {
        std::set<std::string> seen;
        t = std::max(t, newestDependency(path, seen, 0));
        t = std::max(t, vegasfiles::fileTime(vegasfiles::audioProxyPath(path)));
    }
    return t;
}

std::vector<std::string> nestedProjectsNeedingProxies(const Project& p)
{
    std::vector<std::string> out;
    std::set<std::string> seen;
    std::function<void(const Project&, int)> walk = [&](const Project& q, int depth) {
        for (const auto& m : q.media) {
            if (m.generated || !isProjectFile(m.path) || !seen.insert(samePath(m.path)).second || !fileExists(m.path)) continue;
            if (!openCurrentProxy(m.path, nullptr, nullptr) && !memoryProxy(m.path)) out.push_back(m.path);
            if (depth < 8)
                if (std::shared_ptr<const Project> inner = sharedProject(m.path, nullptr)) walk(*inner, depth + 1);
        }
    };
    walk(p, 0);
    std::reverse(out.begin(), out.end());
    return out;
}

void stopAudioProxyBuilds(bool wait, bool cancelled)
{
    ProxyBook& b = proxyBook();
    std::unique_lock<std::mutex> lk(b.mutex);
    if (cancelled) {
        for (const auto& q : b.queue) b.cancelled.insert(samePath(q.first));
        if (!b.building.empty()) b.cancelled.insert(b.building);
    }
    b.queue.clear();
    b.waiting.clear();
    if (!b.building.empty()) b.waiting.insert(b.building);
    b.stop = true;
    ++b.stopGeneration;
    b.cv.notify_all();
    if (wait) b.idle.wait_for(lk, std::chrono::seconds(10), [&] { return b.building.empty(); });
}

bool waitForAudioProxies(double seconds)
{
    ProxyBook& b = proxyBook();
    std::unique_lock<std::mutex> lk(b.mutex);
    return b.idle.wait_for(lk, std::chrono::milliseconds((int64_t)(seconds * 1000)), [&] { return b.building.empty() && b.queue.empty(); });
}

namespace {
// Another project, read like a media file: its frame and sound rendered by engines of its own (see above).
class ProjectReader : public MediaReader {
public:
    ~ProjectReader() override
    {
        // a final render never reuses a mixer, so what an old one holds cannot reach the sound
        if (mixer_ && sh_ && gFinalDepth == 0) sh_->giveBack(std::move(mixer_));
    }

    bool open(const std::string& path, std::string* error)
    {
        fileGeneration_ = gNestedFileGeneration.load();
        sh_ = nestedShared(path, error);
        if (!sh_) return false;
        describe(path);
        return true;
    }

    // The project as its file is now: read again when it has been saved since (checkNestedFiles).
    void current()
    {
        checkNestedFiles();
        const uint64_t gen = gNestedFileGeneration.load();
        if (gen == fileGeneration_) return;
        fileGeneration_ = gen;
        std::shared_ptr<NestedShared> sh = nestedShared(info_.path, nullptr);
        if (!sh || sh == sh_) return;
        video_.reset();
        mixer_.reset();
        sh_ = sh;
        rs_ = Resampling();
        describe(info_.path);
    }

    void describe(const std::string& path)
    {
        const Project& proj = *sh_->project;
        info_ = MediaInfo();
        info_.path = path;
        bool video = false, audio = false;
        for (const auto& t : proj.tracks) {
            if (t.events.empty()) continue;
            if (t.type == TrackType::Video) video = true;
            else audio = true;
        }
        info_.hasVideo = video || proj.tracks.empty();
        info_.hasAudio = audio;
        info_.width = proj.settings.width;
        info_.height = proj.settings.height;
        info_.fps = proj.settings.fps();
        info_.duration = proj.length();
        info_.sampleRate = proj.settings.sampleRate;
        info_.channels = audio ? 2 : 0;
        info_.fieldOrder = proj.settings.fieldOrder;
        info_.videoCodec = video ? "project" : "";
        info_.audioCodec = audio ? "project" : "";
        video_.reset(new VideoEngine(sh_->pool, sh_->videoFx->fx));
    }

    const MediaInfo& info() const override { return info_; }

    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        current();
        if (!info_.hasVideo) return nullptr;
        if (!(seconds == seconds)) seconds = 0;
        const Project& proj = *sh_->project;
        // past the end: the last frame (as a file's reader does)
        const int64_t last = std::max<int64_t>(0, proj.frameAt(std::max(0.0, info_.duration - 1e-6)));
        const int64_t frame = std::min(last, std::max<int64_t>(0, proj.frameAt(seconds)));
        int ow = info_.width, oh = info_.height;
        double scale = 1.0;
        if (maxW > 0 && maxH > 0) {
            const double s = std::min((double)maxW / info_.width, (double)maxH / info_.height);
            ow = std::max(1, std::min(maxW, (int)std::lround(info_.width * s)));
            oh = std::max(1, std::min(maxH, (int)std::lround(info_.height * s)));
            if (gFinalDepth == 0) scale = std::min(1.0, s);
        }
        if (gNestedDepth >= kMaxNestedDepth) return nullptr;
        FrameOptions fo;
        // Made at the size it is shown at.
        fo.scale = scale;
        fo.transparent = true;
        const bool ready = nestedFxReady(sh_->videoFx, gFinalDepth > 0);
        const uint64_t gen = gNestedGeneration.load();
        if (ImagePtr have = sh_->cachedFrame(frame, ow, oh, gen)) return have;
        fo.skipEffects = !ready;
        if (fo.skipEffects) ++gLeftOut;
        const bool approxBefore = mediaReadHints().approximated;
        mediaReadHints().approximated = false;
        fo.cancel = mediaReadHints().cancel;
        ++gNestedDepth;
        ImagePtr img = video_->render(proj, proj.frameTime(frame), fo);
        --gNestedDepth;
        const bool approx = mediaReadHints().approximated;
        mediaReadHints().approximated = approxBefore || approx;
        if (!img) return nullptr;
        if (img->w != ow || img->h != oh) img = fitImage(*img, ow, oh);
        if (!img) return nullptr;
        if (!approx) sh_->keepFrame(frame, gen, img);
        return img;
    }

    int videoCost(double seconds, int maxW, int maxH) override
    {
        if (!info_.hasVideo || !(seconds == seconds)) return 0;
        const int64_t frame = std::max<int64_t>(0, sh_->project->frameAt(seconds));
        const uint64_t gen = gNestedGeneration.load();
        std::lock_guard<std::mutex> lk(sh_->mutex);
        for (const auto& c : sh_->frames)
            if (c.frame == frame && c.generation == gen && (maxW <= 0 || c.img->w <= maxW) && (maxH <= 0 || c.img->h <= maxH)) return 0;
        return 1;
    }

    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        current();
        if (frames <= 0) return info_.hasAudio;
        std::fill(left, left + frames, 0.f);
        std::fill(right, right + frames, 0.f);
        if (!info_.hasAudio || sampleRate <= 0) return false;
        if (gNestedDepth >= kMaxNestedDepth) return true;
        // The nested project is always mixed at its own sample rate, by a mixer of this reader's own while it has
        // one, and that is resampled to the rate asked for.
        proxy_ = currentProxy(*sh_);
        const int own = proxy_ ? proxy_->info().rate : sh_->project->settings.sampleRate > 0 ? sh_->project->settings.sampleRate : sampleRate;
        own_ = own;
        if (sampleRate == own) mix(start, frames, left, right);
        else resample(start, frames, sampleRate, own, left, right);
        proxy_.reset();
        return true;
    }

private:
    // The nested project's sound at its own rate, samples [start, start + frames): silence before its start and after its end.
    void mix(int64_t start, int frames, float* left, float* right)
    {
        if (proxy_ && proxy_->info().rate == own_ && proxy_->read(start, frames, left, right)) return;
        // The reader keeps its own mixer: two readers of one nested project sharing mixers (and their stretchers' and
        // effects' state) made the sound depend on which thread got to a mixer first.
        if (mixer_ && mixer_->rate != own_) mixer_.reset();
        if (!mixer_) mixer_ = sh_->takeAudio(own_, start, gFinalDepth > 0);
        NestedAudio* a = mixer_.get();
        if (!nestedFxReady(a->fx, gFinalDepth > 0)) ++gLeftOut;
        const int rate = a->rate;
        const Project& proj = *a->fx->project;
        // it lasts the whole samples before its end, as VEGAS's own mixes of nested projects (.sfap0) do.
        const int64_t end = (int64_t)std::floor(info_.duration * rate + 1e-7);
        int64_t from = start;
        int skip = 0;
        if (from < 0) {                             // before its start: silence
            skip = (int)std::min<int64_t>(frames, -from);
            from = 0;
        }
        int n = frames - skip;
        if (from + n > end) n = (int)std::max<int64_t>(0, end - from);
        ++gNestedDepth;
        for (int done = 0; done < n;) {
            const int piece = std::min(n - done, 2048);
            a->engine->render(proj, from + done, piece, left + skip + done, right + skip + done, true);
            done += piece;
        }
        --gNestedDepth;
        a->next = start + frames;
    }

    // A windowed sinc (Blackman, kHalfTaps zero crossings either side), tabulated.
    static const int kHalfTaps = 8;
    static const int kTableSteps = 512;         // table entries per zero crossing
    static const std::vector<float>& sincTable()
    {
        static const std::vector<float> table = [] {
            std::vector<float> t((size_t)kHalfTaps * kTableSteps + 2);
            const double pi = 3.14159265358979323846;
            for (size_t i = 0; i < t.size(); ++i) {
                const double u = (double)i / kTableSteps;
                const double sinc = u < 1e-9 ? 1.0 : std::sin(pi * u) / (pi * u);
                const double w = u >= kHalfTaps ? 0.0 : 0.42 + 0.5 * std::cos(pi * u / kHalfTaps) + 0.08 * std::cos(2.0 * pi * u / kHalfTaps);
                t[i] = (float)(sinc * w);
            }
            return t;
        }();
        return table;
    }

    // The sound at 'rate', made from the mix at 'own': output sample j comes from the mix at j * own / rate.
    void resample(int64_t start, int frames, int rate, int own, float* left, float* right)
    {
        const double ratio = (double)own / rate;
        const double fc = std::min(1.0, 1.0 / ratio);           // the cutoff, so that nothing folds back when sped up
        const int reach = (int)std::ceil(kHalfTaps / fc) + 1;
        if (rs_.ratio != ratio || start != rs_.nextOut) {
            rs_.ratio = ratio;
            rs_.l.clear();
            rs_.r.clear();
            rs_.first = (int64_t)std::floor((double)start * ratio) - reach;
        }
        const int64_t needEnd = (int64_t)std::floor((double)(start + frames) * ratio) + reach + 1;
        const int64_t have = rs_.first + (int64_t)rs_.l.size();
        if (needEnd > have) {
            const size_t old = rs_.l.size();
            const size_t more = (size_t)(needEnd - have);
            rs_.l.resize(old + more, 0.f);
            rs_.r.resize(old + more, 0.f);
            for (size_t done = 0; done < more;) {
                const int piece = (int)std::min<size_t>(more - done, 65536);
                mix(have + (int64_t)done, piece, rs_.l.data() + old + done, rs_.r.data() + old + done);
                done += (size_t)piece;
            }
        }
        const std::vector<float>& table = sincTable();
        const double step = fc * kTableSteps;
        const int64_t count = (int64_t)rs_.l.size();
        const double last = info_.duration * own;
        for (int i = 0; i < frames; ++i) {
            const double x = (double)(start + i) * ratio;
            if (x < 0 || x >= last) continue;
            const int64_t c = (int64_t)std::floor(x);
            double sl = 0, sr = 0, sw = 0;
            for (int64_t k = c - reach + 1; k <= c + reach; ++k) {
                const double pos = std::fabs((double)k - x) * step;
                const size_t ti = (size_t)pos;
                if (ti + 1 >= table.size()) continue;
                const double f = pos - (double)ti;
                const double w = table[ti] + (table[ti + 1] - table[ti]) * f;
                const int64_t at = k - rs_.first;
                if (at >= 0 && at < count) {
                    sl += w * rs_.l[(size_t)at];
                    sr += w * rs_.r[(size_t)at];
                }
                sw += w;
            }
            if (sw > 1e-9) {
                left[i] = (float)(sl / sw);
                right[i] = (float)(sr / sw);
            }
        }
        rs_.nextOut = start + frames;
        // what the next block cannot need goes
        const int64_t keepFrom = (int64_t)std::floor((double)rs_.nextOut * ratio) - reach - 1;
        if (keepFrom > rs_.first) {
            const size_t drop = (size_t)std::min<int64_t>(keepFrom - rs_.first, count);
            rs_.l.erase(rs_.l.begin(), rs_.l.begin() + (std::ptrdiff_t)drop);
            rs_.r.erase(rs_.r.begin(), rs_.r.begin() + (std::ptrdiff_t)drop);
            rs_.first += (int64_t)drop;
        }
    }

    struct Resampling {
        double ratio = 0;
        int64_t nextOut = std::numeric_limits<int64_t>::min();
        int64_t first = 0;                      // the mix sample rs_.l[0] is
        std::vector<float> l, r;
    };
    Resampling rs_;

    std::shared_ptr<NestedShared> sh_;
    uint64_t fileGeneration_ = 0;               // gNestedFileGeneration when sh_ was last made sure of
    MediaInfo info_;
    int own_ = 0;                               // the nested project's own sample rate
    std::shared_ptr<vegasfiles::AudioProxyFile> proxy_;
    std::unique_ptr<VideoEngine> video_;
    std::unique_ptr<NestedAudio> mixer_;        // this reader's own sound mixer
};
}  // namespace

void setProjectFileLoader(ProjectFileLoader loader)
{
    {
        std::lock_guard<std::mutex> lk(gNestedMutex);
        gProjectLoader = std::move(loader);
    }
    std::lock_guard<std::mutex> lk(gLoadedMutex);
    gLoaded.clear();
}

void setMainThreadRunner(MainThreadRunner runner)
{
    std::lock_guard<std::mutex> lk(gNestedMutex);
    gMainRunner = std::move(runner);
}

void setMainThreadPoster(MainThreadPoster poster, std::function<bool()> isMainThread)
{
    std::lock_guard<std::mutex> lk(gNestedMutex);
    gMainPoster = std::move(poster);
    gIsMainThread = std::move(isMainThread);
}

void setNestedProjectReadyHook(std::function<void(const std::string& path)> hook)
{
    std::lock_guard<std::mutex> lk(gNestedMutex);
    gReadyHook = std::move(hook);
}

uint64_t nestedProjectsGeneration()
{
    checkNestedFiles();
    return gNestedGeneration.load();
}

void releaseUnusedNestedProjects()
{
    std::deque<std::shared_ptr<NestedShared>> drop;
    {
        std::lock_guard<std::mutex> lk(gSharedMutex);
        drop.swap(*gRecent);
    }
}
uint64_t nestedEffectsLeftOut() { return gLeftOut; }
bool nestedVideoParallelSafe(const Project& p, bool allowUnsafe)
{
    std::set<std::string> seen;
    std::function<bool(const Project&, int)> walk = [&](const Project& q, int depth) -> bool {
        for (const MediaItem& m : q.media) {
            if (m.generated || m.offline || !isProjectFile(m.path)) continue;
            // only a project some video event shows is rendered by the frame threads.
            bool used = false;
            for (const Track& t : q.tracks) {
                if (t.type != TrackType::Video) continue;
                for (const Event& e : t.events) {
                    used = used || e.mediaId == m.id;
                    for (const Take& k : e.takes) used = used || k.mediaId == m.id;
                }
            }
            if (!used || !seen.insert(samePath(m.path)).second) continue;
            if (depth >= 8) return false;
            std::shared_ptr<NestedShared> sh = nestedShared(m.path, nullptr);
            if (!sh) continue;
            if (!sh->videoFx) return false;
            if (!nestedFxReady(sh->videoFx, false)) return false;
            bool safe = true;
            if (!allowUnsafe)
                sh->project->forEachFx([&](const FxSlot& s) {
                    if (!safe || s.bypass) return;
                    if (ofx::Instance* inst = sh->videoFx->fx.video(s.id))
                        if (!inst->plugin || inst->plugin->safety < 1) safe = false;
                });
            if (!safe || !walk(*sh->project, depth + 1)) return false;
        }
        return true;
    };
    return walk(p, 0);
}
// The per-thread nested-project counters, carried to the helper threads that mix a project's tracks.
NestedThreadState nestedThreadState()
{
    NestedThreadState st;
    st.depth = gNestedDepth;
    st.finalDepth = gFinalDepth;
    st.leftOut = gLeftOut;
    return st;
}
void setNestedThreadState(const NestedThreadState& st)
{
    gNestedDepth = st.depth;
    gFinalDepth = st.finalDepth;
    gLeftOut = st.leftOut;
}

static std::string signatureOf(const std::string& path, std::set<std::string>& seen)
{
    const Stamp st = stampOf(path);
    if (st.size < 0) return std::string();
    std::string sig = strfmt("%s:%lld:%lld", samePath(path).c_str(), (long long)st.size, (long long)st.time);
    if (!isProjectFile(path) || !seen.insert(samePath(path)).second || seen.size() > 256) return sig;
    std::vector<std::string> uses;
    sharedProject(path, nullptr, &uses);
    for (const auto& u : uses) sig += "|" + signatureOf(u, seen);
    return sig;
}

std::string mediaSignature(const std::string& path)
{
    std::set<std::string> seen;
    return signatureOf(path, seen);
}

FinalRenderScope::FinalRenderScope(bool on) : on_(on)
{
    if (on_) ++gFinalDepth;
}

FinalRenderScope::~FinalRenderScope()
{
    if (on_) --gFinalDepth;
}

bool isProjectFile(const std::string& path)
{
    return endsWithNoCase(path, ".veg") || endsWithNoCase(path, ".reno");
}

std::unique_ptr<MediaReader> openProjectMedia(const std::string& path, std::string* error)
{
    std::vector<std::string> chain;
    std::map<std::string, std::vector<std::string>> seen;
    std::string which;
    if (containsLoop(path, chain, seen, &which)) {
        if (error) *error = pathFileName(path) + " contains itself (" + pathFileName(which) + " uses it), so it cannot be used as media";
        return nullptr;
    }
    std::unique_ptr<ProjectReader> r(new ProjectReader());
    if (!r->open(path, error)) return nullptr;
    return std::unique_ptr<MediaReader>(r.release());
}

std::unique_ptr<MediaReader> openMediaOrProject(const std::string& path, std::string* error)
{
    if (isProjectFile(path)) return openProjectMedia(path, error);
    return openMedia(path, error);
}

// ====================================================================== export
bool exportProject(const Project& p, FxRuntime& fx, const EncodeSettings& settings, double from, double to,
                   const std::function<bool(const ExportProgress&)>& progress, std::string* error, MediaPool::Opener opener, std::string* description)
{
    auto fail = [&](const std::string& m) { if (error) *error = m; return false; };
    if (!(to > from)) return fail("nothing to render");
    FinalRenderScope finalScope;
    EncodeSettings es = settings;
    es.width = p.settings.width;
    es.height = p.settings.height;
    es.fpsNum = p.settings.fpsNum;
    es.fpsDen = p.settings.fpsDen;
    es.sampleRate = p.settings.sampleRate;
    std::string err;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
    if (!w) return fail(err);
    if (description) *description = w->describe();

    MediaPool pool;
    if (opener) pool.setOpener(opener);
    VideoEngine video(pool, fx);
    AudioEngine audio(pool, fx);
    const int rate = p.settings.sampleRate;
    const int64_t first = p.frameAt(from);
    const int64_t count = std::max<int64_t>(1, (int64_t)std::ceil((to - from) * p.settings.fps() - 1e-6));
    FrameOptions fo;
    fo.interactive = false;
    std::vector<float> l, r, six[6];
    auto sampleOfFrame = [&](int64_t f) { return (int64_t)std::floor((double)f * rate * p.settings.fpsDen / p.settings.fpsNum + 1e-6); };
    // with a frame the sound runs to the end of the last frame, as VEGAS's does; without, to the whole sample before the end.
    const int64_t audioEnd = es.video ? INT64_MAX : (int64_t)std::floor(to * rate + 1e-6);
    for (int64_t k = 0; k < count; ++k) {
        const int64_t f = first + k;
        if (es.video) {
            ImagePtr img = video.render(p, p.frameTime(f) + 1e-7, fo);
            if (!img) return fail("out of memory");
            if (!w->writeVideo(*img)) { w->finish(&err); return fail(err.empty() ? "could not write the frame" : err); }
        }
        if (es.audio) {
            const int64_t s0 = sampleOfFrame(f), s1 = std::min(es.video && k == count - 1 ? (int64_t)std::llround((double)(f + 1) * rate * p.settings.fpsDen / p.settings.fpsNum) : sampleOfFrame(f + 1), audioEnd);
            const int n = (int)std::max<int64_t>(0, s1 - s0);
            if (n > 0) {
                if (es.audioChannels == 6) {
                    // 5.1: the project's six channels (a stereo project's left and right, with silence in the rest)
                    float* ch[6];
                    for (int c = 0; c < 6; ++c) {
                        six[c].assign((size_t)n, 0.f);
                        ch[c] = six[c].data();
                    }
                    if (p.settings.surround()) audio.renderChannels(p, s0, n, ch, true);
                    else audio.render(p, s0, n, ch[0], ch[1], true);
                    if (!w->writeAudioChannels(ch, 6, n)) { w->finish(&err); return fail(err.empty() ? "writing the sound failed" : err); }
                } else {
                l.resize((size_t)n);
                r.resize((size_t)n);
                audio.render(p, s0, n, l.data(), r.data(), true);
                if (!w->writeAudio(l.data(), r.data(), n)) { w->finish(&err); return fail(err.empty() ? "writing the sound failed" : err); }
                }
            }
        }
        if (progress) {
            ExportProgress ep;
            ep.frame = k + 1;
            ep.frames = count;
            ep.fraction = (double)(k + 1) / (double)count;
            if (!progress(ep)) { w->finish(nullptr); return fail("stopped"); }
        }
    }
    if (!w->finish(&err)) return fail(err);
    return true;
}

}  // namespace reno
