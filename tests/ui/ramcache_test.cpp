// The Dynamic RAM Preview keeps every picture the preview renders, as VEGAS does: replaying a section plays from memory (nothing
// rendered, nothing dropped), the memory is kept to Preferences' size by dropping the pictures farthest from the cursor and least
// recently used, a change to the project empties it, and pictures are kept by the size they were rendered at. First the store on
// its own (RamFrameStore), then Playback with a stand-in reader that takes 60 ms a picture.
//
//   ramcache_test
// Build: uibuild ramcache_test tests/ui/ramcache_test.cpp
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QElapsedTimer>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

static QImage pic(int w, int h, int mark)
{
    QImage q(w, h, QImage::Format_ARGB32);
    q.fill(QColor(mark % 256, 0, 0));
    return q;
}

static bool hasKey(RamFrameStore& s, int64_t f, double scale)
{
    for (const auto& k : s.keys())
        if (k.first == f && std::fabs(k.second - scale) < 1e-6) return true;
    return false;
}

static void storeTests()
{
    const size_t one = (size_t)pic(100, 100, 0).sizeInBytes();      // 40000
    {
        RamFrameStore s;
        check(!s.put(1, true, 0, 1.0, pic(100, 100, 0), false), "a store with no limit (0 MB) keeps nothing");
        s.setLimit(10 * one);
        check(s.put(1, true, 0, 1.0, pic(100, 100, 0), false) && s.frames() == 1 && s.bytes() == one, "a picture is kept and weighed");
        s.setLimit(0);
        check(s.frames() == 0 && s.bytes() == 0, "turning the store off drops everything");
    }
    {
        // scale keys: a half-size picture is never given for a full-size request; a full one may serve a smaller one
        RamFrameStore s;
        s.setLimit(20 * one);
        s.put(1, true, 5, 0.5, pic(50, 50, 1), false);
        QImage got;
        check(!s.find(1, 5, 1.0, got), "a half-size picture is not given when a full one is wanted");
        check(s.find(1, 5, 0.5, got) && got.width() == 50, "the half-size picture is given for a half-size request");
        check(!s.find(1, 6, 0.5, got), "another frame is not there");
        s.put(1, true, 5, 1.0, pic(100, 100, 2), false);
        check(s.find(1, 5, 1.0, got) && got.width() == 100, "the full picture is given for a full request");
        check(s.find(1, 5, 0.5, got) && got.width() == 50, "the exact half-size picture is still the one given for half");
        check(s.find(1, 5, 0.25, got), "a bigger picture serves a smaller request");
        check(s.frames() == 2 && hasKey(s, 5, 0.5) && hasKey(s, 5, 1.0), "both sizes are kept under the one frame");
        RamFrameStore t;
        t.setLimit(20 * one);
        t.put(1, true, 7, 1.0, pic(100, 100, 2), false);
        check(t.find(1, 7, 0.5, got) && got.width() == 100, "only a full picture kept: it serves the smaller request");
    }
    {
        // invalidation: a newer stamp empties the store; an older one is refused
        RamFrameStore s;
        s.setLimit(20 * one);
        s.put(1, true, 1, 1.0, pic(100, 100, 0), false);
        s.put(1, true, 2, 1.0, pic(100, 100, 0), false);
        QImage got;
        check(!s.find(2, 1, 1.0, got), "pictures of another stamp are not given");
        check(s.put(2, true, 3, 1.0, pic(100, 100, 0), false) && s.frames() == 1 && !s.find(1, 1, 1.0, got), "a picture of a newer stamp empties the store");
        check(!s.put(1, false, 4, 1.0, pic(100, 100, 0), false) && s.frames() == 1, "a late picture of the old stamp is refused");
        check(s.find(2, 3, 1.0, got), "the new stamp's picture is there");
        s.clear();
        check(s.frames() == 0 && s.bytes() == 0, "clear empties it");
    }
    {
        // eviction: the farthest from the cursor goes first
        RamFrameStore s;
        s.setLimit(5 * one);
        s.setCursor(12);
        for (int f : {10, 11, 12, 13, 14}) s.put(1, true, f, 1.0, pic(100, 100, f), false);
        check(s.frames() == 5 && s.bytes() == 5 * one, "five pictures fit");
        check(!s.put(1, true, 100, 1.0, pic(100, 100, 0), false) && s.frames() == 5, "a picture far from the cursor is the one dropped, not kept at another's cost");
        s.setCursor(100);
        check(s.put(1, true, 100, 1.0, pic(100, 100, 0), false), "the cursor moved there: it is kept");
        check(!hasKey(s, 10, 1.0) && hasKey(s, 14, 1.0) && s.frames() == 5, "and the one farthest from the cursor (10) went", strfmt("%zu", s.frames()));
        check(s.bytes() <= 5 * one, "never over the limit");
    }
    {
        // eviction: of two as far from the cursor, the one used longer ago goes
        RamFrameStore s;
        s.setLimit(5 * one);
        s.setCursor(100);
        for (int f : {90, 110, 100, 101, 99}) s.put(1, true, f, 1.0, pic(100, 100, f), false);
        QImage got;
        s.find(1, 90, 1.0, got);  // used just now
        s.put(1, true, 102, 1.0, pic(100, 100, 0), false);
        check(hasKey(s, 90, 1.0) && !hasKey(s, 110, 1.0), "of two as far from the cursor, the one used longer ago goes", strfmt("%zu kept", s.frames()));
        check(s.evicted() >= 1, "evictions are counted");
    }
    {
        // pinned (Shift+B) pictures stay; the others make room for them
        RamFrameStore s;
        s.setLimit(4 * one);
        s.setCursor(0);
        for (int f : {0, 1, 2, 3}) s.put(1, true, f, 1.0, pic(100, 100, f), false);
        check(s.put(1, true, 50, 1.0, pic(100, 100, 0), true) && hasKey(s, 50, 1.0), "a pinned picture takes the place of one that is not");
        for (int f : {51, 52, 53}) s.put(1, true, f, 1.0, pic(100, 100, f), true);
        check(s.frames() == 4 && hasKey(s, 50, 1.0) && hasKey(s, 53, 1.0), "four pinned pictures fill it");
        check(!s.put(1, true, 1, 1.0, pic(100, 100, 0), false), "nothing unpinned is kept when only pinned ones are there");
        check(!s.put(1, true, 54, 1.0, pic(100, 100, 0), true) && s.frames() == 4 && s.bytes() == 4 * one, "a pinned picture with no room is refused");
        check(!s.put(1, true, 9, 1.0, pic(100, 100, 0), false), "an unpinned one cannot push pinned ones out");
    }
    {
        // the frames that go are the ones the plain rule picks (the worst score searched for again after each one goes),
        // over random puts, finds, cursor moves and smaller limits; and a much smaller limit over thousands of frames is quick
        struct Ref {
            struct E { size_t bytes; uint64_t used; bool pinned; };
            std::map<std::pair<int64_t, int>, E> m;
            size_t limit = 0, bytes = 0;
            uint64_t tick = 0;
            int64_t cursor = 0;
            void evict()
            {
                while (bytes > limit) {
                    auto worst = m.end();
                    double ws = -1;
                    for (auto it = m.begin(); it != m.end(); ++it) {
                        if (it->second.pinned) continue;
                        const double sc = (double)std::llabs(it->first.first - cursor) + 0.5 * (double)(tick - it->second.used);
                        if (sc > ws) { ws = sc; worst = it; }
                    }
                    if (worst == m.end()) break;
                    bytes -= worst->second.bytes;
                    m.erase(worst);
                }
            }
        };
        const size_t one = (size_t)pic(16, 16, 0).sizeInBytes();
        RamFrameStore s;
        Ref r;
        s.setLimit(60 * one);
        r.limit = 60 * one;
        uint32_t seed = 12345;
        auto rnd = [&](int n) { seed = seed * 1664525u + 1013904223u; return (int)((seed >> 8) % (uint32_t)n); };
        const QImage px = pic(16, 16, 1);
        bool same = true;
        for (int step = 0; step < 4000 && same; ++step) {
            const int what = rnd(10);
            if (what < 6) {
                const int64_t f = rnd(200);
                const bool pin = rnd(20) == 0;
                if (s.put(1, true, f, 1.0, px, pin)) {}
                // the reference does what put does: keep, evict, and take back the new one if still over
                const std::pair<int64_t, int> key(f, 1000);
                auto it = r.m.find(key);
                if (it != r.m.end()) { it->second.used = ++r.tick; it->second.pinned = it->second.pinned || pin; }
                else { r.m[key] = Ref::E{one, ++r.tick, pin}; r.bytes += one; }
                r.evict();
                if (r.bytes > r.limit) { auto me = r.m.find(key); if (me != r.m.end()) { r.bytes -= me->second.bytes; r.m.erase(me); } }
            } else if (what < 8) {
                const int64_t f = rnd(200);
                QImage out;
                if (s.find(1, f, 1.0, out)) {
                    auto it = r.m.lower_bound(std::make_pair(f, 1000));
                    if (it != r.m.end() && it->first.first == f) it->second.used = ++r.tick;
                }
            } else if (what < 9) {
                const int64_t c = rnd(200);
                s.setCursor(c);
                r.cursor = c;
            } else {
                const size_t lim = (size_t)(20 + rnd(60)) * one;
                s.setLimit(lim);
                r.limit = lim;
                r.evict();
            }
            std::vector<std::pair<int64_t, double>> a = s.keys();
            same = a.size() == r.m.size();
            size_t k = 0;
            for (auto it = r.m.begin(); same && it != r.m.end(); ++it, ++k) same = a[k].first == it->first.first;
        }
        check(same, "the frames dropped are the ones the plain rule drops");
        RamFrameStore big;
        big.setLimit(100000 * one);
        for (int f = 0; f < 8000; ++f) big.put(1, true, f, 1.0, px, false);
        QElapsedTimer t;
        t.start();
        big.setLimit(100 * one);
        const double ms = t.nsecsElapsed() / 1e6;
        std::printf("a limit for 100 of 8000 frames: %.1f ms\n", ms);
        check(big.frames() == 100 && ms < 50, "a much smaller limit over 8000 frames is quick", strfmt("%.1f ms", ms));
    }
}

// "slow:" 20 s of 320x180 pictures that take 60 ms each to make.
class Reader : public MediaReader {
public:
    MediaInfo mi;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        int w = 320, h = 180;
        if (maxW > 0 && maxH > 0 && (maxW < w || maxH < h)) {
            const double s = std::min((double)maxW / w, (double)maxH / h);
            w = std::max(1, (int)std::lround(w * s));
            h = std::max(1, (int)std::lround(h * s));
        }
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        const int f = (int)std::floor(seconds * 30.0 + 1e-6);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* d = img->row(y) + x * 4;
                d[0] = (uint8_t)(f % 256); d[1] = (uint8_t)(x * 255 / w); d[2] = 90; d[3] = 255;
            }
        return img;
    }
    bool audio(int64_t, int frames, int, float* l, float* r) override
    {
        for (int k = 0; k < frames; ++k) l[k] = r[k] = 0.f;
        return false;
    }
};

static std::unique_ptr<MediaReader> opener(const std::string& path, std::string*)
{
    std::unique_ptr<Reader> r(new Reader());
    r->mi.path = path;
    r->mi.duration = 20;
    r->mi.hasVideo = true;
    r->mi.width = 320;
    r->mi.height = 180;
    r->mi.fps = 30;
    return std::unique_ptr<MediaReader>(r.release());
}

class ManualOutput : public AudioOutput {
public:
    Pull pull;
    bool start(int, Pull p) override { pull = std::move(p); return true; }
    void stop() override { pull = Pull(); }
    std::string name() const override { return "test"; }
};

// Lets the pictures being made finish: waits until none was made for a while (at least 'ms'; at most a few seconds). A fixed wait
// is too short when the machine is busy: a picture of the paused cursor not finished when play starts is then rendered during the
// play, which the checks on replaying count as a failure.
static void settleRendered(Playback& pb, int ms)
{
    QElapsedTimer t;
    t.start();
    uint64_t last = pb.picturesRendered() + pb.picturesRenderedAhead();
    qint64 since = 0;
    do {
        uitest::settle(20);
        const uint64_t now = pb.picturesRendered() + pb.picturesRenderedAhead();
        if (now != last) { last = now; since = t.elapsed(); }
    } while (t.elapsed() < ms || (t.elapsed() - since < 400 && t.elapsed() < 8000));
}

// Plays on for 'seconds', taking the sound at about the pace of a sound card (which moves the playback on).
static void playFor(ManualOutput* out, double seconds)
{
    const int piece = 240;
    std::vector<float> l(piece), r(piece);
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    while (std::chrono::steady_clock::now() < until && out->pull) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        QCoreApplication::processEvents();
        if (out->pull) out->pull(l.data(), r.data(), piece);
    }
}

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    storeTests();

    AppState& app = AppState::get();
    app.mediaOpener = opener;
    app.edit("settings", [](Project& p) {
        p.settings.width = 320;
        p.settings.height = 180;
        p.settings.fpsNum = 30;
        p.settings.fpsDen = 1;
    });
    app.edit("clip", [&](Project& p) {
        MediaItem m;
        m.id = p.newId();
        m.path = "slow:";
        m.name = "slow";
        m.info = opener("slow:", nullptr)->info();
        p.media.push_back(m);
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.length = 20;
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
    app.prefs.ramPreviewMb = 1536;
    Playback pb;
    ManualOutput* out = new ManualOutput();
    pb.setAudioOutput(std::unique_ptr<AudioOutput>(out));
    pb.setPreviewSize(0, 0, false);

    // ---- a section played slowly (every picture can be made in time) is kept
    app.setCursor(1.0);
    pb.playAt(0.4);
    playFor(out, 2.8);
    pb.pause();
    settleRendered(pb, 300);
    const uint64_t made1 = pb.picturesRendered() + pb.picturesRenderedAhead();
    const size_t kept1 = pb.ramPreviewFrames();
    check(made1 >= 25, "playing slowly renders the section's pictures", strfmt("%llu rendered", (unsigned long long)made1));
    check(kept1 >= 25, "every picture rendered while playing is in the RAM preview", strfmt("%zu kept", kept1));
    check(pb.ramPreviewBytes() >= kept1 * 320 * 180 * 4, "the memory used counts the pictures' bytes", strfmt("%zu bytes", pb.ramPreviewBytes()));

    // ---- an unplayed section at the normal rate is too slow: pictures are dropped (the control)
    app.setCursor(8.0);
    pb.play();
    playFor(out, 1.2);
    const uint64_t droppedCold = pb.droppedFrames();
    pb.pause();
    settleRendered(pb, 300);
    check(droppedCold > 5, "a section not played before drops frames at the normal rate (60 ms a picture)", strfmt("%llu dropped", (unsigned long long)droppedCold));

    // ---- the first section again, at the normal rate: from memory, nothing rendered, nothing dropped
    app.setCursor(1.05);
    settleRendered(pb, 300);  // the paused picture is rendered and kept too
    const uint64_t before = pb.picturesRendered();
    const uint64_t shownBefore = pb.framesShown();
    pb.play();
    playFor(out, 0.8);
    const uint64_t renderedDuring = pb.picturesRendered() - before;
    const uint64_t dropped = pb.droppedFrames();
    const uint64_t shown = pb.framesShown() - shownBefore;
    const double fps = pb.previewFrameRate();
    pb.pause();
    uitest::settle(200);
    check(renderedDuring == 0, "replaying the section renders nothing", strfmt("%llu rendered", (unsigned long long)renderedDuring));
    check(dropped == 0, "replaying the section drops nothing", strfmt("%llu dropped", (unsigned long long)dropped));
    check(shown >= 20, "and the pictures are shown, one for each frame", strfmt("%llu shown in 0.8 s", (unsigned long long)shown));
    check(fps > 24.0, "at about the project's frame rate", strfmt("%.1f fps", fps));

    // ---- an edit empties it
    app.edit("settings", [](Project& p) { p.settings.width = 320; p.tracks[0].events[0].length = 19.5; });
    uitest::settle(100);
    app.setCursor(1.05);
    settleRendered(pb, 300);
    const uint64_t before2 = pb.picturesRendered();
    pb.play();
    playFor(out, 0.5);
    pb.pause();
    uitest::settle(200);
    check(pb.picturesRendered() - before2 >= 3, "after an edit the old pictures are not used: they are rendered again",
          strfmt("%llu rendered", (unsigned long long)(pb.picturesRendered() - before2)));

    // ---- a smaller preview keeps its pictures by their size; the paused picture is full size again
    pb.clearRamPreview();
    check(pb.ramPreviewFrames() == 0, "clearRamPreview empties it");
    pb.setPreviewSize(160, 90, false);
    app.setCursor(3.0);
    pb.playAt(0.4);
    playFor(out, 1.5);
    pb.pause();
    uitest::settle(500);
    size_t small = 0, full = 0;
    for (const auto& k : pb.ramStore().keys()) (k.second < 0.99 ? small : full)++;
    check(small >= 10, "pictures rendered at the preview's size are kept at that size", strfmt("%zu half-size", small));
    check(pb.lastFrame().width() == 320, "paused, the full-size picture is shown, not the half-size one that is kept", strfmt("%d", pb.lastFrame().width()));
    const uint64_t before3 = pb.picturesRendered();
    app.setCursor(3.1);
    pb.playAt(1.0);
    playFor(out, 0.5);
    const uint64_t renderedSmall = pb.picturesRendered() - before3;
    pb.pause();
    uitest::settle(200);
    pb.setPreviewSize(0, 0, false);
    check(renderedSmall <= 3, "the same section at the same size plays from memory", strfmt("%llu rendered", (unsigned long long)renderedSmall));
    const uint64_t before4 = pb.picturesRendered();
    app.setCursor(3.1);
    pb.play();
    playFor(out, 0.5);
    pb.pause();
    uitest::settle(200);
    check(pb.picturesRendered() - before4 >= 5, "at the full size the half-size pictures do not do: rendered again",
          strfmt("%llu rendered", (unsigned long long)(pb.picturesRendered() - before4)));

    // ---- the memory is kept to Preferences' size
    pb.clearRamPreview();
    app.prefs.ramPreviewMb = 1;                     // (4 pictures of 230 KB)
    app.setCursor(10.0);
    pb.playAt(0.4);
    playFor(out, 2.0);
    pb.pause();
    settleRendered(pb, 300);
    check(pb.ramPreviewBytes() <= 1024 * 1024 && pb.ramPreviewFrames() >= 2, "over the limit, the memory stays within it", strfmt("%zu bytes, %zu frames", pb.ramPreviewBytes(), pb.ramPreviewFrames()));
    int64_t newest = -1, oldest = 1 << 30;
    for (const auto& k : pb.ramStore().keys()) { newest = std::max(newest, k.first); oldest = std::min(oldest, k.first); }
    check(pb.ramStore().evicted() > 0 && newest >= 10 * 30 + 15 && oldest > 10 * 30 + 5, "the pictures kept are those nearest the cursor, which went on",
          strfmt("frames %lld .. %lld", (long long)oldest, (long long)newest));
    app.prefs.ramPreviewMb = 0;
    pb.clearRamPreview();
    app.setCursor(15.0);
    pb.playAt(0.4);
    playFor(out, 0.8);
    pb.pause();
    settleRendered(pb, 300);
    check(pb.ramPreviewFrames() == 0, "at 0 MB the store is off");

    // ---- Shift+B still makes a range
    app.prefs.ramPreviewMb = 1536;
    const int64_t made = pb.ramPreview(5.0, 5.5, nullptr);
    check(made == 15 && pb.ramPreviewFrames() >= 15, "Shift+B renders a range into the same store", strfmt("%lld", (long long)made));
    return uitest::summary();
}
