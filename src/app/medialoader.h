// The thread that reads media for the timeline's thumbnails and waveforms, and the waveform it fills in. Implemented in timeline_loader.cpp.
#pragma once
#include "../engine/engine.h"
#include "../media/vegasfiles.h"
#include <QImage>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace reno {
namespace app {
namespace tl {

// A waveform: the lowest and highest sample of each short stretch of a file's sound, at several levels of detail
// (each kPeakStep times coarser than the one before), so drawing a long file zoomed out is as quick as a short one.
// The loader thread fills it in from the start; 'filled' says how far it has got and the interface only reads below
// that, so no lock is needed.
const int kPeakRate = 6000;
const int kPeakBucket = 30;
const int kPeakStep = 8;
const double kPeakPerSecond = (double)kPeakRate / kPeakBucket;

struct Peaks {
    struct Level { std::vector<int8_t> lo, hi; };
    std::vector<Level> levels;
    std::atomic<int64_t> filled{0};
    std::atomic<bool> complete{false};

    int64_t ready(size_t level) const
    {
        if (complete.load()) return (int64_t)levels[level].lo.size();
        int64_t n = filled.load();
        for (size_t k = 0; k < level; ++k) n /= kPeakStep;
        return n;
    }
};

// The sound itself for a stretch of a file, for a waveform zoomed in past the peaks (VEGAS then draws every sample):
// 'rate' samples a second from sample 'start' (at that rate), left and right (the same channel twice for one channel).
struct WaveDetail {
    int rate = 0;
    int64_t start = 0;
    std::vector<float> l, r;
};

struct ThumbKey {
    int media = 0;
    int height = 0;
    int64_t ms = 0;
    bool operator<(const ThumbKey& o) const { return std::tie(media, height, ms) < std::tie(o.media, o.height, o.ms); }
};

inline QImage imageToQt(const Image& img)
{
    QImage q(img.w, img.h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.h; ++y) std::memcpy(q.scanLine(y), img.row(img.h - 1 - y), (size_t)img.w * 4);
    return q;
}

class Loader {
public:
    struct Done { ThumbKey key; QImage image; };

    Loader(MediaPool::Opener opener, std::function<void()> onReady, bool rebuild = false);

    ~Loader();

    void requestThumb(const ThumbKey& key, const MediaItem& m, double seconds, int w, int h);
    std::shared_ptr<Peaks> peaks(int user, const MediaItem& m, int first = -1, int count = 0);
    // The samples of 'frames' samples from 'start' at 'rate' of the sound peaks() would give, if read already; else they
    // are asked for (with some to spare either side, the last request for a file replacing one not yet read) and null
    // comes back until they are there (takeDetails).
    std::shared_ptr<const WaveDetail> detail(int user, const MediaItem& m, int first, int count, int rate, int64_t start, int frames);
    // Whether samples asked for by detail() came in since the last call.
    bool takeDetails();
    double duration(const MediaItem& m);
    int channels(const MediaItem& m);
    std::vector<std::string> takeProbed();
    std::set<int> takeDropped();
    void forgetMedia(int user, const std::string& path);
    void forgetFiles();
    std::vector<Done> take();

private:
    struct Job { ThumbKey key; MediaItem media; double seconds = 0; int w = 0, h = 0; };
    struct Wave {
        MediaItem media;
        uint64_t user = 0;
        int first = -1, count = 0;
        std::shared_ptr<Peaks> peaks;
        std::vector<int64_t> done;
        bool failed = false;
        QString cacheFile;
        uint64_t leftOut = 0;
        // VEGAS's peaks (media/vegasfiles.h): read from a .sfk beside the file, or, for a file of one or two channels,
        // made while its sound is read at its own rate (and then written there); the waveform is computed from them.
        bool native = false;
        int rate = 0;
        std::shared_ptr<vegasfiles::PeakBuilder> builder;
    };
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        bool quit = false;
        std::deque<Job> thumbs;
        std::deque<Wave> waves;
        std::vector<Done> finished;
        bool rebuild = false;
        std::deque<MediaItem> probes;
        std::set<std::string> probing;
        std::map<std::string, MediaInfo> probed;
        std::vector<std::string> probedNew;
        struct DetailJob {
            std::tuple<int, int, int> key;
            MediaItem media;
            uint64_t user = 0;
            int first = -1, count = 0, rate = 0, frames = 0;
            int64_t start = 0;
        };
        std::deque<DetailJob> details;
        std::vector<std::pair<std::tuple<int, int, int>, std::shared_ptr<const WaveDetail>>> detailsDone;
        bool posted = false;
        std::function<void()> onReady;
    };

    static MediaItem lightCopy(const MediaItem& m);
    static void wake(const std::shared_ptr<Shared>& s);

    static int8_t pack(float v) { return (int8_t)std::lround(std::min(1.0f, std::max(-1.0f, v)) * 127.0f); }

    static void coarsen(Peaks& pk, size_t level, int64_t from, int64_t to, int64_t below);
    static bool waveStep(Wave& w, MediaPool& pool, std::vector<float>& l, std::vector<float>& r);
    static void levelsFollow(Wave& w);
    static void levelsFinish(Wave& w, bool ok);

    // ---- VEGAS's peaks
    // Level 0 of the waveform from VEGAS's peaks of the file ('rate' its sample rate), as far as the first 'ready'
    // blocks reach (all of it when 'finished'). A stretch of the waveform takes the lowest and highest of the blocks
    // it overlaps, over the channels it is of.
    static void fillFromPeakFile(Wave& w, const vegasfiles::PeakFile& pf, int64_t ready, bool finished);
    static bool loadPeakFile(Wave& w);
    static void chooseNative(Wave& w);
    static bool nativeStep(Wave& w, MediaPool& pool, std::vector<float>& l, std::vector<float>& r);
    static void keepNative(const Wave& w);

    static const char* peaksMagic() { return "RenoPeaks1"; }

    static bool loadPeaks(Wave& w);
    static void savePeaks(const Wave& w);

    static bool cacheable(const MediaItem& m) { return !m.path.empty() && m.path.compare(0, 6, "synth:") != 0; }

    static void run(std::shared_ptr<Shared> s, MediaPool* pool);

    std::shared_ptr<Shared> s_;
    MediaPool pool_;
    std::thread thread_;
    std::set<ThumbKey> pending_;
    std::set<int> dropped_;
    std::map<std::tuple<int, int, int>, std::shared_ptr<Peaks>> peaks_;
    // the stretches read, most recently used first (many events may show different parts of one file: each keeps its
    // own), and the ones asked for and not read yet (rate, start, frames)
    struct KeptDetail { std::tuple<int, int, int> key; std::shared_ptr<const WaveDetail> detail; };
    std::list<KeptDetail> details_;
    size_t detailBytes_ = 0;
    std::map<std::tuple<int, int, int>, std::vector<std::tuple<int, int64_t, int>>> detailAsked_;
};

}  // namespace tl
}  // namespace app
}  // namespace reno
