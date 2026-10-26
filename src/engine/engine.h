// Turning a project into frames and sound: the compositor, the mixer and the file export.
#pragma once
#include <functional>
#include "project.h"
#include "fxruntime.h"
#include "../media/media.h"
#include <map>
#include <list>
#include <mutex>
#include <atomic>
#include <limits>

namespace reno {

// ---------------------------------------------------------------- nested projects (a project used as media)
// Reads a project file (.veg, .reno ...) into 'out'. The application registers a loader because the .veg reader lives
// outside the engine; without one, .reno files are read with Project::load.
using ProjectFileLoader = std::function<bool(const std::string& path, Project& out, std::string* error)>;
void setProjectFileLoader(ProjectFileLoader loader);
// Older way to create a nested project's plug-in instances: runs 'fn' on the UI thread and waits. Without a runner,
// 'fn' runs on the calling thread.
using MainThreadRunner = std::function<bool(const std::function<void()>& fn)>;
void setMainThreadRunner(MainThreadRunner runner);
// Nested projects create their plug-in instances on the UI thread a few at a time, without anybody waiting (see
// engine.cpp). Without a poster the instances are created on the thread that first needs them.
using MainThreadPoster = std::function<void(std::function<void()> fn)>;
void setMainThreadPoster(MainThreadPoster poster, std::function<bool()> isMainThread);
// Called on the UI thread once all effects of the nested project at 'path' exist.
void setNestedProjectReadyHook(std::function<void(const std::string& path)> hook);
// Increases whenever some nested project's effects have all been created (for frame caches).
uint64_t nestedProjectsGeneration();
// Nested projects stay loaded for a while after their last reader is gone. AppState calls it when another project is
// opened.
void releaseUnusedNestedProjects();
// Text for disk-cache keys (thumbnails, waveforms): the file's size and modification time, and for a project file
// also those of the project files it uses.
std::string mediaSignature(const std::string& path);
// How many frames or blocks of sound this thread has rendered from nested projects while their effects were still
// being created.
uint64_t nestedEffectsLeftOut();
// True if the frames of the nested projects used by p's video events can be rendered on several threads at once:
// their effects exist, and none says it is not render-thread-safe.
bool nestedVideoParallelSafe(const Project& p, bool allowUnsafe = false);
// While one of these is alive on a thread, that thread's output is final (a file being written): nested projects wait
// for their effects instead of leaving them out. exportProject, renderAs and non-interactive VideoEngine::render hold one.
class FinalRenderScope {
public:
    explicit FinalRenderScope(bool on = true);
    ~FinalRenderScope();
    FinalRenderScope(const FinalRenderScope&) = delete;
    FinalRenderScope& operator=(const FinalRenderScope&) = delete;
private:
    bool on_;
};
// ---- Audio proxies of nested projects. As in VEGAS, the sound of a project used as media is rendered once in the
// background into "<project>.veg.sfap0" (VEGAS's own format, see media/vegasfiles.h) and read from there by playback,
// scrubbing, waveforms and renders.
void setAudioProxyOptions(bool use, bool create);
// Progress callback, called on the builder thread: project path, fraction done (0..1) and state:
// 0 building, 1 finished, 2 failed (with a message), 3 stopped.
void setAudioProxyProgressHook(std::function<void(const std::string& path, double fraction, int state, const std::string& message)> hook);
// Queues a proxy build for the project at 'path'. 'force' rebuilds even if the proxy is current (Tools > Rebuild
// Offline Nested Audio builds missing and stale proxies; Rebuild Audio Peaks rebuilds all).
void buildAudioProxy(const std::string& path, bool force = false);
// The project files used by 'p' (directly or nested) whose proxy is missing or stale.
std::vector<std::string> nestedProjectsNeedingProxies(const Project& p);
// Changes when the file at 'path' changes (for a project file: or anything it uses). 0 if the file doesn't exist.
// Sound caches are checked against it.
int64_t mediaStamp(const std::string& path);
// True if the project at 'path' has a current proxy.
bool audioProxyCurrent(const std::string& path);
// Stops the running build and drops the queued ones (another project opened, Reno closing). 'cancelled' means the
// user cancelled (VEGAS's status bar): those projects get no proxy until one is requested with 'force', and are mixed
// directly meanwhile.
void stopAudioProxyBuilds(bool wait, bool cancelled = false);
// Waits until no build is running or queued, or 'seconds' have passed. Returns true if none is left. (tests)
bool waitForAudioProxies(double seconds);

// True for the name of a project file (.veg, .reno) rather than a media file.
bool isProjectFile(const std::string& path);
// A reader that renders another project's frames and sound like a media file. Returns null with *error set if the
// project can't be read.
std::unique_ptr<MediaReader> openProjectMedia(const std::string& path, std::string* error = nullptr);
// openMedia, or openProjectMedia for a project file. This is what MediaPool opens files with by default.
std::unique_ptr<MediaReader> openMediaOrProject(const std::string& path, std::string* error = nullptr);

// Opens media files for the engine. Any file can be replaced by a made-up one (tests, offline media): see setOpener.
class MediaPool {
public:
    typedef std::function<std::unique_ptr<MediaReader>(const std::string& path, std::string* error)> Opener;
    MediaPool();
    void setOpener(Opener o);               // default: reno::openMediaOrProject

    // The frame of media 'm' at 'seconds' into it, fitted inside w x h. 'user' (an event id) gets a reader of its
    // own, so two events playing different parts of one file don't fight over it. 'deinterlace' (0 none, 1 blend, 2
    // interpolate) applies if the media is interlaced (MediaReader::setDeinterlace).
    ImagePtr videoFrame(uint64_t user, const MediaItem& m, double seconds, int w, int h, int deinterlace = 0, PixelDepth depth = PixelDepth::Byte,
                        double reserveSeconds = 0);
    // Opens the readers that the sound of events playing in [from, from + span) will need (one per event, at most the
    // pool's per-file limit), so playback doesn't stall opening files.
    void prewarmAudio(const Project& p, double from, double span, const std::atomic<bool>* stop = nullptr);
    // Sound from media 'm': 'frames' samples at 'rate', starting at sample 'start' (at that rate) of the file.
    bool audio(uint64_t user, const MediaItem& m, int64_t start, int frames, int rate, float* left, float* right, int firstChannel = -1,
               int channelCount = 0);
    void clear();
    // Counters for measurements (tests/perf): reads requested, how many needed a reader that had to seek, files opened.
    // videoShared: frames given from the shared frames (setShareFrames) without asking any reader; videoWaited: reads that waited for
    // another thread's reader to get to the frame instead of opening or seeking another one.
    struct Stats { uint64_t audioReads = 0, audioSeeks = 0, audioNative = 0, audioOpened = 0, videoReads = 0, videoSeeks = 0, videoOpened = 0, videoShared = 0, videoWaited = 0; };
    Stats stats() const
    {
        Stats s;
        s.videoShared = stats_.videoShared;
        s.videoWaited = stats_.videoWaited;
        s.audioReads = stats_.audioReads; s.audioSeeks = stats_.audioSeeks; s.audioNative = stats_.audioNative; s.audioOpened = stats_.audioOpened;
        s.videoReads = stats_.videoReads; s.videoSeeks = stats_.videoSeeks; s.videoOpened = stats_.videoOpened;
        return s;
    }
    // At most this many video / audio readers are open at once (0: the defaults). Pools of nested projects keep fewer.
    void setReaderLimits(size_t video, size_t audio);
    // Whether frames of video files that have a proxy (media.h: Create Video Proxy) come from the proxy.
    void setUseProxies(bool on);
    bool useProxies() const { return useProxies_.load(); }
    // Playing (the preview's pool; renders and exports never do).
    void setShareFrames(bool on, size_t bytes = 96u * 1024u * 1024u);
    bool shareFrames() const { return share_ != nullptr; }

private:
    // One open reader. Readers aren't tied to one user. An event takes the reader of its media that has the frame (or
    // sound) it wants closest: one another event left just there (a stutter repeating a few frames, the next piece of
    // a chopped-up clip) or the one it used last.
    struct Entry {
        std::unique_ptr<MediaReader> reader;
        std::mutex mutex;
        bool failed = false;
        uint64_t media = 0;
        uint64_t lane = 0;                  // the user's top two bits: thumbnails and dialogs keep readers apart from playback's
        uint64_t lastUser = 0;
        uint64_t lastUse = 0;               // the pool's count of requests at its last use (for least recently used)
        double reservedUntil = 0;           // video: kept for the event that decoded ahead on it until then (steady clock seconds)
        int64_t nextStart = 0;              // audio: where the last read ended
        uint64_t audioUser = 0;             // audio: the event that read it last (only that one is carried on by a sample or two)
        int rate = 0;
        int first = -1, count = 0;          // audio: the channels the last read picked
        std::atomic<double> busyWant{std::numeric_limits<double>::quiet_NaN()};   // video: the time being read now (NaN: idle or not known yet)
        std::atomic<uint64_t> busyShape{0};                                         // ... and the size and settings it is read at (shapeOf)
        int deinterlace = 0;                // video: what the reader was last told to do with interlaced frames
        std::string field;                  // video: the Field order override it was last told (MediaItem::fieldOrderOverride)
    };
    using EntryPtr = std::shared_ptr<Entry>;
    // Finds the reader 'user' should use for media 'm' (returned with 'lock' held).
    EntryPtr get(std::vector<EntryPtr>& list, size_t limit, size_t perMedia, uint64_t user, const MediaItem& m,
                 const std::function<int(Entry&)>& cost, std::unique_lock<std::mutex>& lock, double reserveSeconds = 0,
                 double want = std::numeric_limits<double>::quiet_NaN(), uint64_t shape = 0);
    class FrameShare;
    std::shared_ptr<FrameShare> share_;          // (set once, before playing: setShareFrames)
    Opener opener_;
    std::mutex mutex_;
    std::atomic<bool> useProxies_{false};
    struct AtomicStats { std::atomic<uint64_t> audioReads{0}, audioSeeks{0}, audioNative{0}, audioOpened{0}, videoReads{0}, videoSeeks{0}, videoOpened{0}, videoShared{0}, videoWaited{0}; };
    AtomicStats stats_;                          // (threads count at the same time)
    std::map<std::string, bool> proxyKnown_;   // media path -> it has a proxy (under mutex_; forgotten when proxies change)
    uint64_t proxyGeneration_ = 0;
    // The media item to read frames from: 'm' itself or its proxy (a different id, so their readers stay separate).
    MediaItem proxyFor(const MediaItem& m);
    std::vector<EntryPtr> video_, audio_;
    uint64_t uses_ = 0;

    size_t maxVideo_ = 0, maxAudio_ = 0;        // setReaderLimits (0: kMaxVideoReaders, kMaxAudioReaders)
};

struct FrameOptions {
    bool interactive = true;                // a preview (false: the final render)
    bool draft = false;
    bool skipEffects = false;               // leave every effect out (the "bypass all" button)
    bool transparent = false;               // leave the frame see-through where nothing covers it (not onto black)
    // Motion blur, supersampling and interlaced fields (the video output's "motionblur" and "supersample" envelopes
    // and Project Properties' field order) take several renders per frame.
    bool subFrames = false;
    // Frame size as a fraction of the project's frame (the preview's quality and size; VEGAS renders Half and Preview
    // at half size, which lets a busy project play smoothly).
    double scale = 1.0;
    // Set from another thread, at any time, when the frame is no longer wanted (playback has moved past it). Null
    // pointer: never cancelled.
    const std::atomic<bool>* cancel = nullptr;
    // Linear-light projects (colormgmt.h): the color space of the finished frame, overriding the project's View
    // transform (a render template's Color space).
    std::string colorSpace;
    // Set by the engine for the sub-frames of motion blur and supersampling: the time of the frame itself (-1: not a
    // sub-frame).
    double blurBase = -1;
    // Render the independent parts of a frame (tracks, the events of a cross-fade, motion-blur sub-frames) at the
    // same time on helper threads (base.h parallelTasks).
    bool parallel = true;
};

class VideoEngine {
public:
    VideoEngine(MediaPool& pool, FxRuntime& fx) : pool_(pool), fx_(fx) {}
    // The finished frame at 'seconds' on the timeline: project-sized, opaque, 8-bit BGRA, bottom row first.
    ImagePtr render(const Project& p, double seconds, const FrameOptions& opt);
    // An event's frame on its own (before track effects and compositing), for thumbnails. May be null.
    ImagePtr eventPicture(const Project& p, const Event& e, double seconds, int w, int h);
    // Decoding ahead (playback): reads the first frame of event 'e' from its media file, at the size render(p, .,
    // opt) would ask for, and discards it.
    bool prewarmEvent(const Project& p, const Event& e, const FrameOptions& opt, double keepSeconds, const std::atomic<bool>* stop);
    // The same for the frame event 'e' shows at timeline time 't' (at or after its start).
    bool prewarmEventAt(const Project& p, const Event& e, double t, const FrameOptions& opt, double keepSeconds, const std::atomic<bool>* stop);
    // Decoding ahead inside a nested project: 'e' plays a project file; the events of that project - and of the
    // projects nested in it, recursively - that come up while 'e' plays timeline time [from, to] get the frame they
    // show first in that stretch read ahead as prewarmEvent does, by the nested project's own readers (the ones its
    // frames are made with).
    int prewarmNested(const Project& p, const Event& e, double from, double to, const FrameOptions& opt, double keepSeconds,
                      const std::atomic<bool>* stop, const std::function<bool(uint64_t)>& claim, int most = 2);

private:
    struct Pass;
    // One frame at exactly 'seconds' (no motion blur or fields), flattened onto black, in the project's depth.
    ImagePtr renderOnce(const Project& p, double seconds, const FrameOptions& opt);
    // With motion blur and supersampling: several of those around 'seconds', averaged. Float when blended.
    ImagePtr renderBlurred(const Project& p, double seconds, const FrameOptions& opt);
    bool onlyReno(const Project& p) const;          // no OpenFX plug-in from a file in the project's video effects
    MediaPool& pool_;
    FxRuntime& fx_;
};

// ---------------------------------------------------------------- 5.1 surround (see SurroundPan in project.h)
// The master's channels in a 5.1 project, in this order (FFmpeg's and WAVE's 5.1 order).
enum SurroundChannel { ChL = 0, ChR = 1, ChC = 2, ChLFE = 3, ChLs = 4, ChRs = 5 };
// The gains (L R C LFE Ls Rs) of a surround panner for a sound at pan point (x, y) with this centre level, using the
// smoothness, speaker mutes and LFE setting of 'sp'. 'route' is ignored.
void surroundGains(const SurroundPan& sp, double x, double y, double center, float g[6]);
// The pan point and centre of 'sp' at timeline time t (from its keyframes, if it has any).
void surroundPointAt(const SurroundPan& sp, double t, double& x, double& y, double& center);
// Moves the pan point of 'sp' to (x, y), clamped to the room. With keyframes it edits the key at time 'cursor'
// (creating one if no key is within half a 'frame'); without, it moves the point itself. Its 'motion' keeps one axis
// fixed.
void surroundPlacePoint(SurroundPan& sp, double x, double y, double cursor, double frame);
// The standard (ITU-R BS.775) fold-down of 5.1 to stereo: L + 0.707 C + 0.707 Ls, R + 0.707 C + 0.707 Rs. The LFE is dropped.
void surroundDownmix(const float* const six[6], int frames, float* left, float* right);

class AudioEngine {
public:
    AudioEngine(MediaPool& pool, FxRuntime& fx);
    ~AudioEngine();
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    // 'frames' samples of the mix starting at timeline sample 'start' (at the project's sample rate). In a 5.1
    // project this is the mix folded down to stereo (surroundDownmix), as a stereo sound card plays it.
    void render(const Project& p, int64_t start, int frames, float* left, float* right, bool playing);
    // The mix in the project's own channels: projectChannels(p) buffers, either left and right or, in a 5.1 project,
    // the six channels L R C LFE Ls Rs (SurroundChannel).
    void renderChannels(const Project& p, int64_t start, int frames, float* const* out, bool playing);
    static int projectChannels(const Project& p) { return p.settings.surround() ? 6 : 2; }
    // Loudest sample of the last render(), per side (for the meters).
    float peakLeft = 0, peakRight = 0;
    // VEGAS's Options > Bypass All Audio FX: leaves out every audio effect (events', tracks', buses' and the master's).
    bool bypassFx = false;
    // Mixing on several processors: tracks that don't depend on each other (and buses the same number of steps from
    // the master) are mixed at the same time on a shared pool of helper threads and summed in track order, so the
    // result is exactly what one thread gives.
    bool parallelMix = true;
    int mixThreads = 0;
    // The most tracks / buses the last render() mixed at the same time (for tests and measurements).
    int lastMixWidth = 0;

    // Renders ahead, on a worker thread of this engine, the sound of events that start in or play through the
    // timeline samples [from, from + frames) and are costly to make (time-stretched or pitch-shifted events, and
    // plain events of a file at another sample rate), into the cache those events play from, so the mixer finds them
    // ready.
    void prefetch(const Project& p, int64_t from, int64_t frames);
    // Sound of costly events is cached (a stretched event is rendered once from its first sample and played from
    // there, so every play, loop and seek sounds the same), up to this many bytes in all (default 384 MB; 0: no
    // caching, events are rendered as they play).
    static void setEventCacheLimit(size_t bytes);
    static size_t eventCacheBytes(); // bytes cached now
    static void clearEventCache();
    struct CachedEvent; // engine_audio.cpp
    struct EventPlan;

    // Loudest sample of one mixer strip in the last render(), per side, after its fader, pan and mute.
    struct StripPeak {
        uint64_t id = 0;
        float left = 0, right = 0;
        // The master of a 5.1 project: 6, with each channel's peak in 'ch' (L R C LFE Ls Rs); 'left' / 'right' are
        // the loudest channel on that side (L, C, LFE, Ls / R, C, LFE, Rs).
        int channels = 2;
        float ch[6] = {0, 0, 0, 0, 0, 0};
    };
    // Peaks of every audio track, bus and the master from the last render(). Thread-safe.
    void stripPeaks(std::vector<StripPeak>& out) const;
    // Render As's multichannel mapping: if set, receives each bus's output in every render (after its effects, fader and
    // stereo pan, before it reaches the master or its parent bus): 'frames' samples of left and right, silence if muted.
    std::function<void(uint64_t busId, const float* left, const float* right, int frames)> busTap;

private:
    friend struct AudioBlockMixer;
    struct EventState;
    struct BusMix;
    struct Work; // one track's or bus's mix, rendered on any thread (engine_audio.cpp)
    // An event's sound for timeline samples [s, s + n) of the block, before its gain and fades, into l and r.
    bool eventSound(const Project& p, const Event& e, const MediaItem& m, int64_t s, int n, float* l, float* r);
    bool eventSoundRaw(const Project& p, const Event& e, const MediaItem& m, int64_t s, int n, float* l, float* r);
    bool readResampledEvent(const Event& e, const MediaItem& m, int rate, int64_t first, int n, float* l, float* r); // a plain event of a file at another rate
    // The mix: the master into left/right or, for a 5.1 project ('six' set), into the six channels.
    void mix(const Project& p, int64_t start, int frames, float* left, float* right, float* const* six, bool playing);
    // 5.1: places a track's or bus's stereo sound (after its fader; per-sample gains in 'gain' or the constant 'level') into six channels.
    void panSurround(const SurroundPan& sp, int64_t start, int rate, int frames, const float* l, const float* r, const float* gain, float level,
                     float* const* six);
    // 5.1: the LFE low-pass filter (Project Properties) applied to x, the block of 'frames' samples at 'start'.
    void lowpassLfe(const Project& p, float* x, int64_t start, int frames);
    void publishPeaks(); // peaks_ is complete: hands it to other threads (stripPeaks)
    void forgetIdleEvents();                        // drops the state of events not heard for a while (every 64th render)
    std::vector<float> six_[6]; // render() of a 5.1 project: the six channels before the fold-down
    double lfeState_[4][2] = {}; // LFE low-pass filter state (up to four sections)
    int64_t lfeNext_ = -1; // the sample the filter expects next (anything else restarts it)
    EventState& stateOf(const Event& e);
    struct Prefetcher;
    std::unique_ptr<Prefetcher> prefetcher_;
    std::mutex prefetchMutex_; // prefetch may be called from two threads
    // Renders the cached sound of an event (see prefetch) up to event sample 'upTo' (counted from its first), one step at a time.
    void fillCached(CachedEvent& c, int64_t upTo, const std::atomic<bool>* stop = nullptr, bool oneStep = false);
    // Copies the event's cached sound for its samples [rel, rel + n) into l and r.
    int cachedEventSound(EventState& st, const Event& e, const MediaItem& m, const EventPlan& pl, int64_t rel, int n, float* l, float* r);

    MediaPool& pool_;
    FxRuntime& fx_;
    std::vector<std::shared_ptr<Work>> work_; // buffers of the tracks / buses being mixed together
    std::mutex eventsMutex_; // guards events_, which tracks on several threads look events up in
    // Per-event processing (reversed reading, time-stretching) that carries over from one render() to the next.
    std::map<uint64_t, std::shared_ptr<EventState>> events_;
    uint64_t renders_ = 0;
    std::vector<std::shared_ptr<BusMix>> buses_;
    std::vector<int> busOrder_;
    std::vector<StripPeak> peaks_;                  // being filled by render()
    // Plug-in delay compensation (engine_audio.cpp): a delay line for each contribution that has to wait for a slower path
    struct PdcLine {
        std::vector<float> l, r;
        size_t pos = 0;
        int64_t next = INT64_MIN;                   // the block start it expects (anything else restarts it)
        uint64_t used = 0;
    };
    std::map<std::pair<uint64_t, size_t>, PdcLine> pdc_;   // (id of the track or bus, bit 63 for a bus; index of the contribution)
    int64_t pdcNext_ = INT64_MIN;                   // where the shifted mix continues (anything else is primed first)
    std::vector<float> pdcL_, pdcR_;                // the primed blocks' output, thrown away
    mutable std::mutex peakMutex_;
    std::vector<StripPeak> sharedPeaks_;            // the last finished set, for other threads
};

// Where the frames an effect works on end up in the finished frame at timeline time t, as an affine map (x' = m[0] x
// + m[1] y + m[2], y' = m[3] x + m[4] y + m[5]) from the effect's own canonical coordinates (project frame pixels,
// (0, 0) at the bottom left, as OpenFX overlays draw) to the finished frame's.
bool effectPlacement(const Project& p, uint64_t slotId, double t, double m[6]);

// 3D Track Motion's space (3D Source Alpha, see engine_video.cpp), so the Track Motion window can draw what the
// compositor does.
void trackMotion3dPoint(const std::vector<MotionKey>& moves, double u, double v, int W, int H, double par, double out[3]);
double trackMotion3dCamera(int W, double par);
// A Track Motion (or Parent Motion) key's place at time t as the compositor interpolates it between keyframes (the
// default place, filling the frame, if there are no keys).
MotionKey motionKeyAt(const std::vector<MotionKey>& keys, double t);

struct ExportProgress {
    double fraction = 0;
    int64_t frame = 0, frames = 0;
};

// Renders the project from 'from' to 'to' seconds into a file. progress() is called after every frame and returns
// false to stop.
bool exportProject(const Project& p, FxRuntime& fx, const EncodeSettings& settings, double from, double to,
                   const std::function<bool(const ExportProgress&)>& progress, std::string* error,
                   MediaPool::Opener opener = MediaPool::Opener(), std::string* description = nullptr);

}  // namespace reno
