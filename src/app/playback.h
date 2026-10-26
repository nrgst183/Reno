// Playing the project: sound to the sound card, frames to the preview.
//
// Threads besides the UI thread: one mixes sound a little ahead of what is heard, the sound card's own thread takes
// the mixed sound, and one or more render frames. All work from snapshots of the project.
//
// Frames work as in VEGAS: the sound and the clock never wait for a frame. While playing, each frame is rendered for
// the time the sound will have reached when it is ready, held until then, and shown only while it is still that time.
// A frame that misses its time is dropped; one already late is never started.
#pragma once
#include "appstate.h"
#include <QImage>
#include <thread>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <cmath>
#include <algorithm>

namespace reno {
namespace app {

// Where sound goes. pull(left, right, frames) is called from the device's thread.
class AudioOutput {
public:
    virtual ~AudioOutput() {}
    typedef std::function<void(float* left, float* right, int frames)> Pull;
    virtual bool start(int sampleRate, Pull pull) = 0;
    virtual void stop() = 0;
    virtual double latencySeconds() const { return 0.0; }
    virtual std::string name() const = 0;
    // More than two channels (a 5.1 project on a sound card with six): pull(ch, channels, frames) fills 'channels'
    // buffers, for 6 in the order L R C LFE Ls Rs. maxChannels() is what the device can play (2 for stereo).
    typedef std::function<void(float* const* ch, int channels, int frames)> PullChannels;
    virtual int maxChannels() const { return 2; }
    virtual bool startChannels(int sampleRate, int channels, PullChannels pull) { (void)sampleRate; (void)channels; (void)pull; return false; }
};
int audioOutputChannels(const std::string& deviceId);
// Gain applied to everything going to the sound card (1: as mixed). The timeline lowers it while a scrub burst plays
// (Preferences > Reno > Editing, Scrub audio volume) and restores it to 1 afterwards.
void setOutputGain(float gain);
float outputGain();
std::unique_ptr<AudioOutput> createAudioOutput();
std::unique_ptr<AudioOutput> createSilentOutput();

// The Dynamic RAM Preview's frames: every frame the preview renders (not drafts) and the Shift+B range is kept here,
// keyed by frame number and render scale (a bigger frame may serve a smaller request, not the reverse). Over the
// limit, the frames farthest from the cursor and least recently used go first; Shift+B frames are pinned until the
// project changes. Thread-safe.
class RamFrameStore {
public:
    void setLimit(size_t bytes);                    // 0: off (nothing is kept)
    size_t limit() const;
    bool find(uint64_t stamp, int64_t frame, double scale, QImage& out);
    bool has(uint64_t stamp, int64_t frame, double scale) const;
    // Keeps a frame. 'current': the stamp is the project's current one (a newer stamp empties the store; an older one is
    // refused). Returns false if the frame wasn't kept (off, stale, or no room that isn't pinned).
    bool put(uint64_t stamp, bool current, int64_t frame, double scale, const QImage& image, bool pinned);
    void setCursor(int64_t frame);
    void clear();
    size_t frames() const;
    size_t bytes() const;
    uint64_t evicted() const;
    std::vector<std::pair<int64_t, double>> keys() const;
private:
    struct Entry { QImage image; uint64_t used = 0; bool pinned = false; };
    typedef std::pair<int64_t, int> Key;
    static int scaleKey(double s) { return (int)std::lround(std::max(0.001, std::min(1.0, s)) * 1000.0); }
    void evictLocked(const Key* keep);
    mutable std::mutex m_;
    std::map<Key, Entry> map_;
    size_t limit_ = 0, bytes_ = 0;
    uint64_t stamp_ = 0, tick_ = 0, evicted_ = 0;
    bool haveStamp_ = false;
    int64_t cursor_ = 0;
};

class Playback {
public:
    Playback();
    ~Playback();

    void play();
    void pause();
    void stop();
    // The cursor goes to 't'. Playing, playback continues from there at the rate it had ('t' is also where Stop goes
    // back to); stopped, only the cursor moves. A cursor preview in progress ends where the cursor goes instead.
    void jumpTo(double t);
    void toggle();
    bool playing() const { return playing_; }
    double position();

    // Plays at 'rate' times normal speed from where it is (from the cursor if stopped), backwards when negative;
    // continues from
    // the place being heard if already playing. 0 pauses. The sound follows the rate (pitch included) except beyond
    // kMutedRate.
    void playAt(double rate);
    double rate() const { return playing_ ? rate_ : 0.0; }
    // VEGAS's Normal Rate indicator (under the Rate slider): the rate Play plays at.
    double normalRate() const { return normalRate_; }
    void setNormalRate(double r);
    // J / L pressed while playing at 'current': VEGAS's steps (1, 1.5, 2, 4, 10, 20 times, either way). dir +1 is L
    // (forward), -1 is J (backward); a press against the current direction starts that way at 1.
    static double nextShuttleRate(double current, int dir);
    static constexpr double kMutedRate = 4.0;   // faster than this (10x, 20x) the sound is muted, as in VEGAS
    static constexpr double kMaxRate = 20.0;

    // Asks for the frame at the cursor (or at the playback position). The newest request wins.
    void requestFrame();
    // How long the preview has been waiting for a frame, in seconds (0: not waiting). Stopped: since the latest
    // request until its frame is shown, fails, or a newer request is rendered. Playing: how far past its time the
    // next frame is. For the preview's "rendering" indicator. UI thread.
    double renderingFor() const;
    std::function<void(const QImage& image, double seconds, double renderMs)> onFrame;
    QImage lastFrame() const { return lastFrame_; }
    // How many frames have been shown (handed to onFrame) since the start, and how long the last one took to render
    // (0 when it came from a cache). For the Dashboard's frame rate. While playing a frame is counted once.
    uint64_t framesShown() const { return framesShown_; }
    double lastRenderMs() const { return lastRenderMs_; }
    // Frames dropped while playing (since playing last began): frames whose time came and went without being shown. For
    // the preview's frame rate readout and the Dashboard. UI thread.
    uint64_t droppedFrames() const { return dueFrames_ > playShown_ ? dueFrames_ - playShown_ : 0; }
    // How many frames playback renders at once now: 1, or more while frames take longer than they last and the effects
    // allow it (see Preferences::renderThreads).
    int renderThreadsInUse() const { return workers_.load(); }

    float peakLeft() const { return peakL_.load(); }
    float peakRight() const { return peakR_.load(); }
    // The meters of the mixer's strips as they are being heard: for each audio track and bus (by id) and the master (id 0),
    // the loudest sample per side after its fader, falling back gently (about 24 dB a second) when it gets quieter.
    void stripLevels(std::vector<AudioEngine::StripPeak>& out) const;
    std::string audioDeviceName() const { return out_ ? out_->name() : std::string(); }
    // How many channels go to the sound card now: 6 while a 5.1 project plays on a device that has six (and Preferences
    // doesn't ask for the fold-down), else 2 (a 5.1 project folded down to stereo).
    int outputChannels() const { return outChannels_.load(); }

    // VEGAS's Preview Cursor Position (Num 0): plays 'duration' seconds centered on the cursor, then restores the cursor.
    void previewCursor(double duration = 1.0);
    // VEGAS's Dim Master Bus Output: everything heard is 20 dB quieter (the meters still show the real levels).
    void setDim(bool on) { dim_ = on; }
    bool dimmed() const { return dim_; }
    // VEGAS's Downmix Output to mono (the Master Bus window's and Mixing Console's Downmix Output button): left and right
    // are mixed together on both sides. Folding 5.1 down to stereo is Preferences' downmixOutput.
    void setMonoOutput(bool on) { mono_ = on; }
    bool monoOutput() const { return mono_; }
    static constexpr float kDimGain = 0.1f;

    // VEGAS's Options > Metronome: clicks at the project's tempo, mixed into what is heard while playing at normal
    // speed (and recording), with an accent on each measure's first beat. Not in renders. 'normal' / 'accent' are the
    // click sounds at the playing sample rate (empty: Reno's own short beeps).
    void setMetronome(bool on, std::vector<float> normal = {}, std::vector<float> accent = {});
    bool metronome() const { return metronomeOn_.load(); }
    void addClicks(const Project& p, int64_t at, int n, float* l, float* r);

    // What is being heard (the master output as played, before Dim), handed to 'tap' on the sound device's thread as it
    // goes to the device. For the Loudness Meters. An empty function removes the tap.
    typedef std::function<void(const float* left, const float* right, int frames)> OutputTap;
    void setOutputTap(OutputTap tap);

    // VEGAS's Dynamic RAM Preview: renders every frame from 'from' to 'to' now (progress(fraction) is called between
    // frames and returns false to stop) and keeps them, so the stretch then plays at full speed however slow it is to
    // render. Kept until the project changes. Returns the number of frames made.
    int64_t ramPreview(double from, double to, const std::function<bool(double)>& progress);
    void clearRamPreview();
    size_t ramPreviewFrames() const;
    size_t ramPreviewBytes() const { return ram_.bytes(); }
    RamFrameStore& ramStore() { return ram_; }
    // For tests: how many frames were rendered (not taken from a cache or the RAM preview) since the start. Doesn't count
    // frames made ahead for the RAM preview by idle threads.
    uint64_t picturesRendered() const { return rendered_.load(); }
    uint64_t picturesRenderedAhead() const { return renderedAhead_.load(); }

    // VEGAS's Selective Prerender (Shift+M): renders frames 'from'..'to' into a video file in 'folder' (as a final
    // render). That stretch then plays from the file until something that shows there changes (see
    // prerenderFingerprint). Rendering a stretch again replaces any it overlaps. progress(fraction) returns false to
    // stop; returns false with *error if rendering failed or was stopped.
    bool prerender(double from, double to, const std::string& folder, const std::function<bool(double)>& progress, std::string* error);
    struct PrerenderedRange { double from = 0, to = 0; std::string path; bool valid = false; };
    std::vector<PrerenderedRange> prerenderedRanges() const;
    void clearPrerendered(bool deleteFiles);
    // True if the frame at 'seconds' comes from a prerendered file now; and how many frames have been taken from them
    // (tests).
    bool prerenderedAt(double seconds) const;
    uint64_t prerenderedFramesShown() const { return preShown_.load(); }
    // A number that changes when anything a prerendered stretch depends on changes: the video tracks and their events over
    // [from, to], the effects' current settings, the video output and the settings.
    static uint64_t prerenderFingerprint(const Project& p, FxRuntime& fx, double from, double to);

    // The preview at Best quality: frames with motion blur, supersampling and interlaced fields as a render has them
    // (FrameOptions::subFrames). Several renders per frame, so slower.
    void setFullQuality(bool on);
    bool fullQuality() const { return fullQuality_; }

    // The size the preview shows frames at (Preview Quality and size, PictureView::previewSize; 0 x 0: the project's)
    // and whether it is Auto. While playing, frames are rendered at that size instead of the project's
    // (FrameOptions::scale), as VEGAS does for Preview or Half. With Auto, when frames can't be made as fast as they
    // play, it goes smaller (half, then a quarter) and back up when they can. A paused frame is always rendered at
    // the project's size.
    void setPreviewSize(int width, int height, bool autoSize);
    // Frames shown per second over the last second of playing (0 when not playing): VEGAS's preview frame rate, lower than
    // the project's when frames are dropped.
    double previewFrameRate() const;
    double renderScale() const { return lastScale_.load(); }

    QImage renderNow(double seconds);
    static QImage blackFrame(const Project& p);
    void setAudioOutput(std::unique_ptr<AudioOutput> out) { if (!playing_ && out) out_ = std::move(out); }

private:
    void tick();
    void startAt(double from);
    void audioThread();
    // Varispeed: 'n' samples at the playback rate from timeline position 'at' (fractional samples), into l and r.
    // Returns the position after them (wrapped around the loop region if there is one).
    double mixVarispeed(const Project& p, double at, int n, float* l, float* r, std::vector<AudioEngine::StripPeak>& peaks);
    void videoThread();
    void pull(float* l, float* r, int frames);
    void pullChannels(float* const* out, int channels, int frames);
    // Six-channel playback: what the device takes, and the mix of a block in the device's channels.
    std::atomic<int> outChannels_{2};
    void mixBlock(const Project& p, int64_t at, int n, float* const* out, int channels);
    std::vector<float> varispeedExt_[4];
    std::vector<float> fold_[2];

    AppState& app_;
    MediaPool pool_;
    VideoEngine video_;
    AudioEngine audio_;
    std::unique_ptr<AudioOutput> out_;
    int listener_ = 0;
    class QTimer* timer_ = nullptr;
    class QTimer* presentTimer_ = nullptr;
    bool timerPeriodSet_ = false;
    bool playing_ = false;
    double playStart_ = 0;

    // Sound: a queue of mixed blocks, each knowing which timeline sample it starts at and how fast it moves through the
    // timeline (the playback rate: sample k of the block is timeline sample timeline + k * rate).
    struct Block { double timeline; double rate = 1.0; std::vector<float> l, r; size_t used = 0; std::vector<AudioEngine::StripPeak> peaks;
                   std::vector<float> ext[4]; };     // ext: C LFE Ls Rs for six-channel playback (l and r are then L and R)
    std::mutex audioMutex_;
    std::condition_variable audioCv_;
    std::deque<Block> queue_;
    size_t queued_ = 0;
    std::atomic<double> ahead_{0.12};
    std::atomic<bool> heardSound_{false};
    int64_t prerollLeft_ = 0;
    double mixPos_ = 0;
    std::atomic<double> heardPos_{0};
    // What the device was handed, as runs of samples: device frame 'dev' (counted from the start of playing) played
    // timeline
    // sample 'timeline', continuing at 'rate' per frame. position() looks up the frame being heard now, so the cursor
    // follows.
    struct HandedRun { int64_t dev = 0; double timeline = 0; double rate = 1; };
    std::deque<HandedRun> handed_;
    int64_t handedFrames_ = 0;
    // Diagnostics for playback-log.txt beside reno.exe: a line a second while playing with what the frame threads, the
    // sound device and the UI did, for investigating a slow preview.
    struct Diag {
        std::atomic<int> prewarmed{0}, aheadStarted{0}, aheadDone{0}, started{0}, rendered{0}, late{0}, ready{0}, ramHits{0}, pulls{0}, pullFrames{0}, silence{0}, mixed{0};
        std::atomic<double> renderMs{0}, maxPullGap{0}, mixMs{0};
        int ticks = 0, presents = 0, shown = 0, positions = 0;
        int64_t lastFrame = -1;
        double lastWrite = 0, lastPull = 0;
    } diag_;
    void diagWrite(bool first);
    double pullWall_ = 0;
    int pullFrames_ = 0;
    double rate_ = 1.0;
    double normalRate_ = 1.0;
    std::atomic<bool> reachedStart_{false};
    struct Chunk { int64_t index = -1; std::vector<float> l, r; std::vector<AudioEngine::StripPeak> peaks; uint64_t stamp = 0;
                   std::vector<float> ext[4]; int channels = 2; };
    std::vector<Chunk> chunks_;
    const Chunk* chunkAt(const Project& p, int64_t chunk, std::vector<AudioEngine::StripPeak>** peaks);
    uint64_t chunkStamp_ = 0;
    std::atomic<bool> chunksStale_{false};
    std::atomic<bool> audioRun_{false};
    std::atomic<bool> reachedEnd_{false};
    std::thread audioThread_;
    std::thread warmThread_;
    // Idle warm-up: a little after a project is opened or the cursor is moved, while nothing plays, opens the files
    // the sound from there needs and renders the costly events (stretched, resampled) there, so playing from there
    // starts at once. One thread below normal priority; playing cuts it short.
    void warmIdleSoon();
    void idleWarmLoop();
    std::thread idleWarmThread_;
    std::mutex idleWarmMutex_;
    std::condition_variable idleWarmCv_;
    uint64_t idleWarmGen_ = 0;
    double idleWarmAt_ = 0;
    bool idleWarmQuit_ = false;
    std::atomic<bool> idleWarmStop_{false};
    std::atomic<bool> warmStop_{false};
    int rateHz_ = 48000;
    std::atomic<float> peakL_{0}, peakR_{0};
    mutable std::mutex meterMutex_;
    std::vector<AudioEngine::StripPeak> meters_;

    mutable std::mutex videoMutex_;
    std::condition_variable videoCv_;
    bool videoQuit_ = false;
    std::vector<std::thread> videoThreads_;
    std::atomic<int> workers_{1};
    int threadsAsked_ = 1;
    double threadsCheckedAt_ = 0;
    // Decoding ahead: while playing, a thread or two read the first frame of each event about to start
    // (VideoEngine::prewarmEvent), so a seek into a long-GOP file is done before the event's turn instead of stalling
    // the frame.
    std::vector<std::thread> prewarmThreads_;
    std::mutex prewarmMutex_;
    std::condition_variable prewarmCv_;
    bool prewarmQuit_ = false;
    std::set<uint64_t> prewarmDone_;
    uint64_t prewarmStamp_ = 0;
    std::atomic<bool> prewarmStop_{true};
    std::atomic<double> prewarmEnd_{-1};
    std::atomic<bool> prewarmCancel_{true};
    void prewarmLoop();
    bool lanesDone_ = false;
    double lanesCheckedAt_ = 0;
    void renderStoppedFrame(std::unique_lock<std::mutex>& lk);
    void renderPlayingFrame(int index, std::unique_lock<std::mutex>& lk);
    void adaptPreviewScale(double ms);
    void useRenderThreads(const Project& p);
    int workersAllowed_ = 1;
    bool wanted_ = false;
    double wantedTime_ = 0;
    bool wantedFull_ = false;
    uint64_t wantedSerial_ = 0, shownSerial_ = 0;
    uint64_t doneSerial_ = 0;
    double pendingSince_ = 0;
    std::deque<double> requestTimes_;
    double pausedRenderMs_ = 0;
    bool draftShown_ = false;
    double draftTime_ = 0, draftAt_ = 0;
    // The stopped frame being rendered: set when a newer one is requested, so a long decode into the middle of a big
    // file gives way at once (MediaReadHints::cancel).
    std::shared_ptr<std::atomic<bool>> stoppedCancel_;
    struct PlayJob { int64_t frame; std::shared_ptr<std::atomic<bool>> cancel; bool late = false; double lateAt = 0; bool ahead = false; };
    struct ReadyFrame { int64_t frame; QImage image; double ms; double scale; };
    // What playing is at, for the frame threads (set by the UI thread at play and every tick): the position at a
    // moment (steady clock seconds), the rate, the loop, and the project's frame rate and length. step: when the
    // project has more frames per second than the screen can show (a 165 fps project on a 60 Hz screen), only every
    // step-th frame is rendered and shown.
    struct PlayClock { double pos = 0, wall = 0, rate = 1, fps = 30, length = 0; bool loop = false; double loopStart = 0, loopEnd = 0; int step = 1; };
    double displayHz_ = 60.0;
    PlayClock clock_;
    uint64_t playGen_ = 0;
    std::vector<PlayJob> inFlight_;
    std::deque<ReadyFrame> ready_;
    int64_t shownFrame_ = -1;
    double renderEst_ = 0.0;
    uint64_t playShown_ = 0, dueFrames_ = 0;
    int64_t lastDueFrame_ = -1;
    void newPlayGeneration();
    void setPlayClock(double pos);
    void present();
    void showFrame(const QImage& image, double seconds, double ms, double scale, bool counted);
    bool planPlayJob(int index, std::unique_lock<std::mutex>& lk, int64_t& frame, std::shared_ptr<std::atomic<bool>>& cancel,
                     double& dyn, double& waitFor, bool& aheadJob);
    void ramToReady(bool post = true);
    std::atomic<bool> aheadOn_{true};
    void noteRenderTime(double seconds, bool lateCancelled, bool stoppedEarly);
    double clockPos() const;
    double wrapTime(double t) const;
    double aheadOf(double from, double to) const;
    // Frame 'f' is current from frameStart(f) until frameEnd(f), in the playing direction. nextFrame steps by the shown
    // frame step in that direction, wrapping around the loop. frameTaken: shown, being rendered or ready already.
    double frameStart(int64_t f) const;
    double frameEnd(int64_t f) const;
    int64_t nextFrame(int64_t f) const;
    bool frameTaken(int64_t f) const;
    void abandonLateJobs(double pos, double lateTol);
    bool parallelSafe(const Project& p) const;
    void videoThread(int index);
    QImage renderFrame(const Project& p, double t, int64_t frame, uint64_t stamp, double scale, bool draft,
                         const std::atomic<bool>* cancel, double& ms, bool& rendered, bool quick = false, bool* approximated = nullptr, bool setCursor = true);
    QImage lastFrame_;
    std::atomic<uint64_t> rendered_{0};
    std::atomic<uint64_t> renderedAhead_{0};
    uint64_t framesShown_ = 0;
    double lastRenderMs_ = 0;
    struct Cached { int64_t frame; uint64_t stamp; QImage image; double scale = 1.0; };
    std::atomic<int> previewW_{0}, previewH_{0};
    std::atomic<bool> autoSize_{false};
    std::atomic<bool> playingNow_{false};
    std::atomic<double> lastScale_{1.0};
    double dynScale_ = 1.0;
    int fastFrames_ = 0;
    std::deque<double> shownAt_;
    double playBegan_ = 0;
    double frameScale(const Project& p, double dyn);
    std::deque<Cached> cache_;
    std::atomic<bool> fullQuality_{false};
    std::atomic<bool> dim_{false};
    std::atomic<bool> metronomeOn_{false};
    std::mutex metronomeMutex_;
    std::vector<float> clickNormal_, clickAccent_;
    std::atomic<bool> mono_{false};
    std::mutex tapMutex_;
    OutputTap tap_;
    double lastTickPos_ = -1;
    double stopAt_ = -1;
    RamFrameStore ram_;
    uint64_t frameStamp() const;
    struct Prerendered {
        double from = 0, to = 0;
        int64_t firstFrame = 0, frames = 0;
        std::string path;
        uint64_t fingerprint = 0;
        bool valid = true;
        std::unique_ptr<MediaReader> reader;
    };
    mutable std::mutex preMutex_;
    std::vector<std::shared_ptr<Prerendered>> pre_;
    std::atomic<uint64_t> preShown_{0};
    void checkPrerendered();
    QImage prerenderedFrame(const Project& p, int64_t frame);
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

// A frame from the engine (bottom row first, BGRA) as a QImage (top row first).
QImage toQImage(const Image& img);

}  // namespace app
}  // namespace reno
