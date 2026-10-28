#include "playback.h"
#include <QFile>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>
#include <QThread>
#include <QCoreApplication>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <set>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <timeapi.h>
#  pragma comment(lib, "winmm.lib")
#endif

namespace reno {
namespace app {

QImage toQImage(const Image& img)
{
    QImage q(img.w, img.h, QImage::Format_RGB32);
    for (int y = 0; y < img.h; ++y) std::memcpy(q.scanLine(y), img.row(img.h - 1 - y), (size_t)img.w * 4);
    return q;
}

namespace {
// Keeps time without a sound card: takes sound at the rate it would have been played.
class SilentOutput : public AudioOutput {
public:
    ~SilentOutput() override { stop(); }
    bool start(int sampleRate, Pull pull) override
    {
        stop();
        run_ = true;
        thread_ = std::thread([this, sampleRate, pull] {
            std::vector<float> l(4096), r(4096);
            auto t0 = std::chrono::steady_clock::now();
            int64_t taken = 0;
            while (run_) {
                const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                const int64_t due = (int64_t)(elapsed * sampleRate);
                while (run_ && taken < due) {
                    const int n = (int)std::min<int64_t>(4096, due - taken);
                    pull(l.data(), r.data(), n);
                    taken += n;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
        return true;
    }
    void stop() override
    {
        run_ = false;
        if (thread_.joinable()) thread_.join();
    }
    std::string name() const override { return "no sound device (keeping time only)"; }

private:
    std::atomic<bool> run_{false};
    std::thread thread_;
};
}

std::unique_ptr<AudioOutput> createSilentOutput() { return std::unique_ptr<AudioOutput>(new SilentOutput()); }

static const int kMixBlock = 1024;
static const int kChunk = 8192;             // varispeed: the timeline's sound is rendered in pieces this long
static const size_t kChunks = 8;            // and this many are kept (reading backwards or round a loop reuses them)
// seconds the first sound waits at most for the mixer to get kAhead ahead (a slow start leaves silence before the
// sound, not gaps in it)
static const double kPreroll = 0.4;
// seconds of sound kept mixed ahead of the device (more, once the device has run dry: ahead_)
static const double kAhead = 0.12;
static const double kAheadMost = 0.5;
static const size_t kCacheFrames = 24;
static const int kMaxVideoThreads = 8;
static const double kAheadSeconds = 2.0;        // how far ahead of the position idle frame threads render into the RAM preview
static const double kAheadMostSeconds = 20.0;   // (more where frames are slow: three times the slowest average ahead, at most this)
static const size_t kRenderWindow = 16;         // frames in the overall average render time
static const int kCutsAhead = 6;                // coming cuts looked at for starting their first frames early
// tests and measurements: RENO_PREWARM=0 / RENO_PLAY_AHEAD=0 / RENO_SHARE_FRAMES=0 turn off decoding ahead /
// rendering ahead / sharing frames
static bool envOff(const char* name)
{
    const char* v = std::getenv(name);
    return v && v[0] == '0';
}
// events starting within this long (in playing time) get their file read ahead for
static const double kPrewarmSeconds = 1.5;
static const double kPrewarmKeep = 2.5;         // and their reader is kept for them this long
// seconds of the timeline ahead of the position whose effects get instances for each frame thread
static const double kLaneLookAhead = 3.0;
static const double kRenderMargin = 0.03;   // seconds: a frame rendered ahead is meant to be ready this long before its time
static const double kEarly = 0.005;         // seconds: a frame may be shown this much before its time (the tick's grain)

static double steadyNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
static const double kMeterFallDb = 24.0;    // how fast the strip meters fall back, dB per second

Playback::Playback() : app_(AppState::get()), video_(pool_, app_.fx), audio_(pool_, app_.fx)
{
    setMainThreadPoster([](std::function<void()> fn) { AppState::post(std::move(fn)); }, [] {
        QCoreApplication* app = QCoreApplication::instance();
        return !app || QThread::currentThread() == app->thread();
    });
    setNestedProjectReadyHook([](const std::string&) { AppState::get().notify(kMediaPictures); });
    pool_.setOpener([this](const std::string& path, std::string* err) {
        return app_.mediaOpener ? app_.mediaOpener(path, err) : openMediaOrProject(path, err);
    });
    pool_.setUseProxies(true);
    // Frames from the readers are shared by all frame threads (and readers of one file), so a frame is decoded once.
    // RENO_SHARE_FRAMES=0 gives each reader its own, for comparing.
    if (!envOff("RENO_SHARE_FRAMES")) pool_.setShareFrames(true);
    meters_.reserve(256);
    out_ = createAudioOutput();
    timer_ = new QTimer(QCoreApplication::instance());
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(15);
    QObject::connect(timer_, &QTimer::timeout, [this] { tick(); });
    presentTimer_ = new QTimer(QCoreApplication::instance());
    presentTimer_->setTimerType(Qt::PreciseTimer);
    presentTimer_->setSingleShot(true);
    QObject::connect(presentTimer_, &QTimer::timeout, [this] { present(); });
    videoThreads_.emplace_back([this] {
        lowerThreadPriority();
        videoThread(0);
    });
    idleWarmThread_ = std::thread([this] { idleWarmLoop(); });
    listener_ = app_.addListener([this, shownPath = std::make_shared<std::string>(),
                                  media = std::make_shared<std::map<uint64_t, std::pair<bool, double>>>()](int changes) {
        if ((changes & kFile) && app_.path() != *shownPath) {
            *shownPath = app_.path();
            pool_.clear();
        }
        if (changes & kProject) {
            // Project Media's Refresh: a file that couldn't be read and is there now, or one that changed (another
            // length), is opened afresh. Otherwise a failed reader is never retried and an open one shows the old
            // file.
            bool reread = false;
            std::map<uint64_t, std::pair<bool, double>> now;
            for (const MediaItem& m : app_.project().media) {
                const std::pair<bool, double> v(m.offline, m.info.duration);
                now[m.id] = v;
                auto it = media->find(m.id);
                if (it != media->end() && ((it->second.first && !m.offline) || it->second.second != v.second)) reread = true;
            }
            media->swap(now);
            if (reread) pool_.clear();
        }
        if (changes & (kProject | kFxParams)) {
            if (changes & kProject) {
                std::lock_guard<std::mutex> lk(audioMutex_);
                if (playing_) {
                    queue_.clear();
                    queued_ = 0;
                    mixPos_ = heardPos_.load();
                }
            }
            if ((changes & kProject) && playing_) {
                std::lock_guard<std::mutex> lk(videoMutex_);
                newPlayGeneration();
            }
            chunksStale_ = true;
            clearRamPreview();
            checkPrerendered();
            if (!playing_) requestFrame();
        }
        if ((changes & kCursor) && !playing_) requestFrame();
        if (((changes & (kFile | kCursor)) && !playing_) || ((changes & kProject) && !playing_)) warmIdleSoon();
        if (changes & kMediaPictures) {
            clearRamPreview();
            if (!playing_) requestFrame();
        }
    });
}

Playback::~Playback()
{
    setNestedProjectReadyHook(nullptr);
    releaseUnusedNestedProjects();
    *alive_ = false;
    app_.removeListener(listener_);
    pause();
    if (audioThread_.joinable()) audioThread_.join();
    {
        std::lock_guard<std::mutex> lk(idleWarmMutex_);
        idleWarmQuit_ = true;
    }
    idleWarmStop_ = true;
    idleWarmCv_.notify_all();
    if (idleWarmThread_.joinable()) idleWarmThread_.join();
    warmStop_ = true;
    if (warmThread_.joinable()) warmThread_.join();
    {
        std::lock_guard<std::mutex> lk(prewarmMutex_);
        prewarmQuit_ = true;
    }
    prewarmCancel_ = true;
    prewarmStop_ = true;
    prewarmCv_.notify_all();
    for (std::thread& t : prewarmThreads_) if (t.joinable()) t.join();
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        videoQuit_ = true;
    }
    videoCv_.notify_all();
    for (std::thread& t : videoThreads_)
        if (t.joinable()) t.join();
    clearPrerendered(true);
    delete timer_;
    delete presentTimer_;
}

void Playback::play()
{
    if (playing_) return;
    playAt(normalRate_);
}

void Playback::setNormalRate(double r)
{
    if (!(std::fabs(r) >= 0.01)) r = 1.0;
    normalRate_ = std::max(-kMaxRate, std::min(kMaxRate, r));
    app_.notify(kPlayback);
}

double Playback::nextShuttleRate(double current, int dir)
{
    static const double steps[] = {1.0, 1.5, 2.0, 4.0, 10.0, 20.0};
    if (dir == 0) return 0.0;
    const double along = current * dir;
    if (along < 0.999) return dir * 1.0;
    for (double s : steps)
        if (s > along + 1e-6) return dir * s;
    return dir * steps[5];
}

// While previewCursor() is playing the loop region is not followed; otherwise a cursor near or outside it was
// "previewed" by looping around, never reaching where the preview stops. Kept here because there is one Playback in
// the program.
static std::atomic<const void*> gPreviewOnce{nullptr};

static bool loopsNow(const void* pb, const AppState& app)
{
    return gPreviewOnce.load() != pb && app.loopOn && app.loopEnd > app.loopStart;
}

void Playback::previewCursor(double duration)
{
    if (playing_) pause();
    gPreviewOnce = this;
    const double c = app_.cursor();
    duration = std::max(0.1, duration);
    rate_ = 1.0;
    playStart_ = c;
    stopAt_ = c + duration / 2;
    startAt(std::max(0.0, c - duration / 2));
}

void Playback::playAt(double rate)
{
    stopAt_ = -1;
    if (gPreviewOnce.load() == this) gPreviewOnce = nullptr;
    if (!(std::fabs(rate) >= 0.01)) {
        pause();
        return;
    }
    rate = std::max(-kMaxRate, std::min(kMaxRate, rate));
    if (playing_) {
        if (rate == rate_) return;
        std::lock_guard<std::mutex> lk(audioMutex_);
        rate_ = rate;
        queue_.clear();
        queued_ = 0;
        mixPos_ = heardPos_.load();
        reachedEnd_ = false;
        reachedStart_ = false;
        audioCv_.notify_all();
        {
            std::lock_guard<std::mutex> vl(videoMutex_);
            newPlayGeneration();
            clock_.pos = clockPos();
            clock_.wall = steadyNow();
            clock_.rate = rate;
        }
        app_.notify(kPlayback);
        return;
    }
    rate_ = rate;
    playStart_ = app_.cursor();
    startAt(playStart_);
}

void Playback::warmIdleSoon()
{
    idleWarmStop_ = true;
    {
        std::lock_guard<std::mutex> lk(idleWarmMutex_);
        ++idleWarmGen_;
        idleWarmAt_ = app_.cursor();
    }
    idleWarmCv_.notify_all();
}

void Playback::idleWarmLoop()
{
    lowerThreadPriority();
    uint64_t done = 0;
    std::unique_lock<std::mutex> lk(idleWarmMutex_);
    while (!idleWarmQuit_) {
        idleWarmCv_.wait(lk, [&] { return idleWarmQuit_ || idleWarmGen_ != done; });
        if (idleWarmQuit_) break;
        const uint64_t g = idleWarmGen_;
        if (idleWarmCv_.wait_for(lk, std::chrono::milliseconds(300), [&] { return idleWarmQuit_ || idleWarmGen_ != g; })) continue;
        done = g;
        const double at = idleWarmAt_;
        lk.unlock();
        if (!playing_) {
            idleWarmStop_ = false;
            std::shared_ptr<const Project> p = app_.snapshot();
            pool_.prewarmAudio(*p, at, 3.0, &idleWarmStop_);
            if (!idleWarmStop_ && !playing_) audio_.prefetch(*p, (int64_t)std::llround(at * p->settings.sampleRate), (int64_t)(3.0 * p->settings.sampleRate));
        }
        lk.lock();
    }
}

void Playback::startAt(double from)
{
    if (QScreen* sc = QGuiApplication::primaryScreen()) {
        const double hz = sc->refreshRate();
        displayHz_ = (hz >= 24.0 && hz <= 1000.0) ? hz : 60.0;
    }
    if (const char* e = std::getenv("RENO_DISPLAY_HZ")) {
        const double hz = std::atof(e);
        if (hz >= 24.0 && hz <= 1000.0) displayHz_ = hz;
    }
    idleWarmStop_ = true;
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        if (stoppedCancel_) stoppedCancel_->store(true);
    }
    if (audioThread_.joinable()) audioThread_.join();
    std::shared_ptr<const Project> p = app_.snapshot();
    rate_ = rate_ == 0 ? 1.0 : rate_;
    const int sampleRate = p->settings.sampleRate;
    rateHz_ = sampleRate;
    const bool loop = loopsNow(this, app_);
    if (loop && (from < app_.loopStart || from >= app_.loopEnd)) from = rate_ > 0 ? app_.loopStart : app_.loopEnd;
    {
        std::lock_guard<std::mutex> lk(audioMutex_);
        queue_.clear();
        queued_ = 0;
        ahead_ = kAhead;
        heardSound_ = false;
        prerollLeft_ = (int64_t)(kPreroll * sampleRate);
        mixPos_ = rate_ == 1.0 ? (double)std::llround(from * sampleRate) : from * sampleRate;
        heardPos_ = mixPos_;
        handed_.clear();
        handed_.push_back(HandedRun{handedFrames_, mixPos_, 0.0});
        reachedEnd_ = false;
        reachedStart_ = false;
    }
    chunksStale_ = true;
    // A 5.1 project on a six-channel sound card plays in 5.1 (unless Preferences asks for the fold-down); everything
    // else plays in stereo, a 5.1 project folded down.
    const bool six = p->settings.surround() && !app_.prefs.downmixOutput && out_->maxChannels() >= 6;
    outChannels_ = six ? 6 : 2;
    audioRun_ = true;
    warmStop_ = true;
    if (warmThread_.joinable()) warmThread_.join();
    warmStop_ = false;
    warmThread_ = std::thread([this, p, from] {
        lowerThreadPriority();
        pool_.prewarmAudio(*p, from, 4.0, &warmStop_);
    });
    audioThread_ = std::thread([this] { audioThread(); });
    bool started = six && out_->startChannels(rateHz_, 6, [this](float* const* ch, int c, int n) { pullChannels(ch, c, n); });
    if (!started) {
        outChannels_ = 2;
        started = out_->start(rateHz_, [this](float* l, float* r, int n) { pull(l, r, n); });
    }
    if (!started) {
        out_ = createSilentOutput();
        out_->start(rateHz_, [this](float* l, float* r, int n) { pull(l, r, n); });
        app_.status("The sound device could not be started; playing without sound.");
    }
    // Frames: Preferences' number of threads (else every processor Reno may use) render ahead when frames take longer
    // than they last, if every effect allows several threads (OpenFX render thread safety) and no nested project
    // needs one thread.
    int threads = app_.prefs.renderThreads > 0 ? app_.prefs.renderThreads : processorCount();
    threads = std::max(1, std::min(kMaxVideoThreads, threads));
    threadsAsked_ = threads;
    threadsCheckedAt_ = steadyNow();
    aheadOn_ = !envOff("RENO_PLAY_AHEAD");
    lanesDone_ = false;
    {
        std::lock_guard<std::mutex> lk(prewarmMutex_);
        prewarmDone_.clear();
    }
    prewarmCancel_ = false;
    prewarmStop_ = false;
    if (prewarmThreads_.empty() && processorCount() >= 2 && !envOff("RENO_PREWARM")) {
        const int n = std::max(1, std::min(3, processorCount() / 2));
        for (int i = 0; i < n; ++i) prewarmThreads_.emplace_back([this] { prewarmLoop(); });
    }
    ram_.setLimit((size_t)std::max(0, app_.prefs.ramPreviewMb) * 1024 * 1024);
    useRenderThreads(*p);
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        newPlayGeneration();
        wanted_ = false;
        draftShown_ = false;
        renderEst_ = std::min(renderEst_, 0.5);
        dynScale_ = 1.0;
        fastFrames_ = 0;
    }
    playShown_ = 0;
    dueFrames_ = 0;
    lastDueFrame_ = p->frameAt(from);
    shownAt_.clear();
    playBegan_ = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    playing_ = true;
    playingNow_ = true;
    setPreviewPlaying(true);
    lastTickPos_ = from;
    setPlayClock(from);
    videoCv_.notify_all();
#ifdef _WIN32
    // Windows wakes timers and waiting threads only every 15.6 ms unless asked for 1 ms; without that, frames come at
    // about 24 a second whatever the project's rate.
    if (!timerPeriodSet_) { timeBeginPeriod(1); timerPeriodSet_ = true; }
#endif
    diagWrite(true);
    timer_->start();
    app_.notify(kPlayback);
}

// How many frame threads playback uses: the number asked for (threadsAsked_), or one if the project can't be rendered
// on several threads at once (parallelSafe). Called when playing starts and every half second while held to one
// because nested projects' effects are still being created.
void Playback::useRenderThreads(const Project& p)
{
    int threads = threadsAsked_;
    if (threads > 1 && !parallelSafe(p)) threads = 1;
    while ((int)videoThreads_.size() < threads) {
        const int index = (int)videoThreads_.size();
        videoThreads_.emplace_back([this, index] {
            lowerThreadPriority();
            videoThread(index);
        });
    }
    if (threads > workers_.load() || !playing_) workers_ = threads;
}

void Playback::pause()
{
    stopAt_ = -1;
    if (gPreviewOnce.load() == this) gPreviewOnce = nullptr;
    if (!playing_) return;
    playing_ = false;
    playingNow_ = false;
    setPreviewPlaying(false);
    prewarmCancel_ = true;
    prewarmStop_ = true;
    shownAt_.clear();
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        newPlayGeneration();
    }
    videoCv_.notify_all();
    timer_->stop();
    presentTimer_->stop();
#ifdef _WIN32
    if (timerPeriodSet_) { timeEndPeriod(1); timerPeriodSet_ = false; }
#endif
    out_->stop();
    audioRun_ = false;
    audioCv_.notify_all();
    // Not waited for here: a block of a heavy project (nested projects in nested projects) can take a while to mix
    // and the UI would stand still. It stops after that block; playing again, or the end, waits for it.
    {
        std::lock_guard<std::mutex> lk(audioMutex_);
        queue_.clear();
        queued_ = 0;
    }
    peakL_ = 0;
    peakR_ = 0;
    {
        std::lock_guard<std::mutex> lk(meterMutex_);
        meters_.clear();
    }
    app_.notify(kPlayback);
    requestFrame();
}

void Playback::stop()
{
    const bool was = playing_;
    pause();
    if (was) app_.setCursor(playStart_);
}

void Playback::jumpTo(double t)
{
    t = std::max(0.0, t);
    if (!playing_) {
        app_.setCursor(t);
        return;
    }
    const bool preview = stopAt_ >= 0;
    const double rate = rate_;
    pause();
    app_.setCursor(t);
    if (preview) return;
    rate_ = rate;
    playStart_ = t;
    startAt(t);
}

void Playback::toggle()
{
    if (playing_) stop();
    else play();
}

double Playback::position()
{
    const double latency = out_ ? out_->latencySeconds() : 0.0;
    {
        std::lock_guard<std::mutex> lk(audioMutex_);
        if (!handed_.empty()) {
            // The device asks for sound in pieces (on Windows often 40 ms or more). Between two asks it keeps
            // playing, so the time since the last ask is added, up to the piece's length. Without this the position
            // moves in steps of a piece, and frames with it.
            double since = 0;
            if (playingNow_ && pullWall_ > 0 && pullFrames_ > 0)
                since = std::min(std::max(0.0, steadyNow() - pullWall_), (double)pullFrames_ / rateHz_) * rateHz_;
            const double now = (double)handedFrames_ - latency * rateHz_ + since;
            const HandedRun* run = &handed_.front();
            for (const HandedRun& h : handed_)
                if ((double)h.dev <= now) run = &h;
                else break;
            const double frames = std::max(0.0, now - (double)run->dev);
            const double heard = (run->timeline + frames * run->rate) / rateHz_;
            return heard < 0 ? 0 : heard;
        }
    }
    const double heard = heardPos_.load() / rateHz_ - latency * rate_;
    return heard < 0 ? 0 : heard;
}

void Playback::tick()
{
    if (!playing_) return;
    double pos = position();
    const double end = app_.project().length();
    const bool loop = loopsNow(this, app_);
    if (stopAt_ >= 0 && pos >= stopAt_) {
        stopAt_ = -1;
        stop();
        return;
    }
    if (rate_ > 0 && reachedEnd_.load() && pos >= end - 0.02 && !loop) {
        pause();
        app_.setCursor(end);
        return;
    }
    if (rate_ < 0 && reachedStart_.load() && pos <= 0.02 && !loop) {
        pause();
        app_.setCursor(0);
        return;
    }
    // The cursor never steps back while playing: the heard position can come out a fraction of a sample before the
    // last one (latency estimate moving), which would flick the time display.
    {
        const double fpsNow = app_.project().settings.fps() > 0 ? app_.project().settings.fps() : 30.0;
        const double back = rate_ > 0 ? lastTickPos_ - pos : pos - lastTickPos_;
        if (lastTickPos_ >= 0 && back > 0 && back < 0.5 / fpsNow) pos = lastTickPos_;
        lastTickPos_ = pos;
    }
    app_.setCursor(pos);
    if (threadsAsked_ > workers_.load() && steadyNow() - threadsCheckedAt_ > 0.5) {
        threadsCheckedAt_ = steadyNow();
        useRenderThreads(*app_.snapshot());
    }
    // The effects coming up get instances for the other frame threads, a little at a time (4 ms a tick at most:
    // plug-ins are made on this thread and it must keep the UI going). Checked every quarter second as the position
    // moves on.
    if (workers_.load() > 1 && (!lanesDone_ || steadyNow() - lanesCheckedAt_ > 0.25)) {
        lanesCheckedAt_ = steadyNow();
        lanesDone_ = app_.fx.prepareLanes(*app_.snapshot(), workers_.load(), std::max(0.0, pos), kLaneLookAhead, 4.0);
    }
    setPlayClock(pos);
    videoCv_.notify_all();
    ++diag_.ticks;
    present();
    if (steadyNow() - diag_.lastWrite >= 1.0) diagWrite(false);
}

void Playback::diagWrite(bool first)
{
    const double now = steadyNow();
    const double span = first || diag_.lastWrite <= 0 ? 1.0 : now - diag_.lastWrite;
    diag_.lastWrite = now;
    const QString path = QCoreApplication::applicationDirPath() + "/playback-log.txt";
    QFile f(path);
    if (!f.open(first ? (QIODevice::WriteOnly | QIODevice::Truncate) : (QIODevice::WriteOnly | QIODevice::Append))) return;
    std::string line;
    if (first) {
        const Project& p = app_.project();
        char b[512];
        std::snprintf(b, sizeof b, "play at %.3f s: project %dx%d %.3f fps; screen %.1f Hz -> every %d frame(s); preview %dx%d (scale %.3f, auto %d); "
                      "threads asked %d, using %d; sound '%s' latency %.1f ms; RAM preview %d MB\n",
                      position(), p.settings.width, p.settings.height, p.settings.fps(), displayHz_, std::max(1, clock_.step), previewW_.load(),
                      previewH_.load(), frameScale(p, dynScale_), autoSize_.load() ? 1 : 0, threadsAsked_, workers_.load(), out_ ? out_->name().c_str() : "-",
                      out_ ? out_->latencySeconds() * 1000.0 : 0.0, app_.prefs.ramPreviewMb);
        line = b;
    } else {
        const int rendered = diag_.rendered.exchange(0), pulls = diag_.pulls.exchange(0), pf = diag_.pullFrames.exchange(0);
        const double ms = diag_.renderMs.exchange(0);
        char b[1024];
        std::snprintf(b, sizeof b, "%6.2f s: shown %d, ticks %d, presents %d, frames the position passed %d | rendering: started %d, done %d (avg %.1f ms), "
                      "ready %d, late %d, from RAM %d, ahead %d/%d, read ahead %d | sound: %d asks of avg %d samples, longest gap %.1f ms, silence %d samples, mixed %d samples in %.0f ms, rate %.2f | est %.1f ms, threads %d, dyn %.2f, dropped %llu\n",
                      position(), diag_.shown, diag_.ticks, diag_.presents, diag_.positions, diag_.started.exchange(0), rendered,
                      rendered ? ms / rendered : 0.0, diag_.ready.exchange(0), diag_.late.exchange(0), diag_.ramHits.exchange(0), diag_.aheadDone.exchange(0), diag_.aheadStarted.exchange(0), diag_.prewarmed.exchange(0), pulls,
                      pulls ? pf / pulls : 0, diag_.maxPullGap.exchange(0) * 1000.0, diag_.silence.exchange(0), diag_.mixed.exchange(0),
                      diag_.mixMs.exchange(0), rate_, renderEst_ * 1000.0, workers_.load(), dynScale_,
                      (unsigned long long)droppedFrames());
        line = b;
        (void)span;
    }
    diag_.shown = diag_.ticks = diag_.presents = diag_.positions = 0;
    f.write(line.c_str());
}

// Varispeed: one chunk of the timeline's sound (kChunk samples from chunk x kChunk) at normal speed, rendered
// when first needed and kept for a while. Before the timeline's start it is silence. Audio thread only.
const Playback::Chunk* Playback::chunkAt(const Project& p, int64_t chunk, std::vector<AudioEngine::StripPeak>** peaks)
{
    ++chunkStamp_;
    audio_.bypassFx = app_.bypassAudioFx.load();
    if (chunks_.capacity() < kChunks) chunks_.reserve(kChunks);
    const int chans = outChannels_.load();
    for (Chunk& c : chunks_)
        if (c.index == chunk && c.channels == chans) {
            c.stamp = chunkStamp_;
            if (peaks) *peaks = &c.peaks;
            return &c;
        }
    Chunk* slot = nullptr;
    if (chunks_.size() < kChunks) {
        chunks_.emplace_back();
        slot = &chunks_.back();
    } else {
        slot = &chunks_[0];
        for (Chunk& c : chunks_)
            if (c.stamp < slot->stamp) slot = &c;
    }
    slot->index = chunk;
    slot->stamp = chunkStamp_;
    slot->l.assign((size_t)kChunk, 0.f);
    slot->r.assign((size_t)kChunk, 0.f);
    slot->channels = chans;
    for (auto& x : slot->ext) x.assign(chans == 6 ? (size_t)kChunk : 0, 0.f);
    slot->peaks.clear();
    if (chunk >= 0) {
        for (int k = 0; k < kChunk; k += kMixBlock) {
            if (chans == 6) {
                float* o[6] = {slot->l.data() + k, slot->r.data() + k, slot->ext[0].data() + k, slot->ext[1].data() + k, slot->ext[2].data() + k,
                               slot->ext[3].data() + k};
                mixBlock(p, chunk * (int64_t)kChunk + k, kMixBlock, o, 6);
            } else {
                audio_.render(p, chunk * (int64_t)kChunk + k, kMixBlock, slot->l.data() + k, slot->r.data() + k, true);
            }
            std::vector<AudioEngine::StripPeak> pk;
            audio_.stripPeaks(pk);
            for (const auto& x : pk) {
                bool found = false;
                for (auto& y : slot->peaks)
                    if (y.id == x.id) {
                        y.left = std::max(y.left, x.left);
                        y.right = std::max(y.right, x.right);
                        y.channels = x.channels;
                        for (int c = 0; c < 6; ++c) y.ch[c] = std::max(y.ch[c], x.ch[c]);
                        found = true;
                    }
                if (!found) slot->peaks.push_back(x);
            }
        }
    }
    if (peaks) *peaks = &slot->peaks;
    return slot;
}

double Playback::mixVarispeed(const Project& p, double at, int n, float* l, float* r, std::vector<AudioEngine::StripPeak>& peaks)
{
    const double rate = rate_;
    const int nch = outChannels_.load() == 6 ? 6 : 2;
    float* outs[6] = {l, r, nullptr, nullptr, nullptr, nullptr};
    if (nch == 6)
        for (int c = 0; c < 4; ++c) {
            if ((int)varispeedExt_[c].size() < n) varispeedExt_[c].resize((size_t)n);
            outs[2 + c] = varispeedExt_[c].data();
        }
    const Chunk* last = nullptr;
    auto sample = [&](int64_t i, float* v) {
        const int64_t c = i >= 0 ? i / kChunk : -((-i + kChunk - 1) / kChunk);
        if (!last || last->index != c) {
            std::vector<AudioEngine::StripPeak>* pk = nullptr;
            last = chunkAt(p, c, &pk);
            if (pk && peaks.empty()) peaks = *pk;
        }
        const size_t k = (size_t)(i - c * kChunk);
        v[0] = last->l[k];
        v[1] = last->r[k];
        for (int q = 2; q < nch; ++q) v[q] = last->ext[q - 2].size() > k ? last->ext[q - 2][k] : 0.f;
    };
    for (int k = 0; k < n; ++k) {
        const double x = at + k * rate;
        const double fl = std::floor(x);
        const int64_t i = (int64_t)fl;
        const float f = (float)(x - fl);
        float a[6], b[6], c[6], d[6];
        sample(i - 1, a);
        sample(i, b);
        sample(i + 1, c);
        sample(i + 2, d);
        for (int ch = 0; ch < nch; ++ch)
            outs[ch][k] = b[ch] + 0.5f * f * (c[ch] - a[ch] + f * (2.f * a[ch] - 5.f * b[ch] + 4.f * c[ch] - d[ch] + f * (3.f * (b[ch] - c[ch]) + d[ch] - a[ch])));
    }
    return at + n * rate;
}

// Six-channel playback: a block of the mix in the device's channels, a 5.1 project's own six or (if the project was
// made stereo while playing) its stereo with the rest silent.
void Playback::mixBlock(const Project& p, int64_t at, int n, float* const* out, int channels)
{
    struct Timed {
        Diag& d; int n; std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
        ~Timed() {
            d.mixMs = d.mixMs.load() + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            d.mixed += n;
        }
    } timed{diag_, n};
    if (channels == 6 && p.settings.surround()) {
        audio_.renderChannels(p, at, n, out, true);
        return;
    }
    audio_.render(p, at, n, out[0], out[1], true);
    for (int c = 2; c < channels; ++c) std::memset(out[c], 0, (size_t)n * sizeof(float));
}

void Playback::audioThread()
{
    raiseThreadPriority();
    std::vector<float> l((size_t)kMixBlock), r((size_t)kMixBlock);
    std::vector<float> ext[4];
    for (auto& x : ext) x.assign((size_t)kMixBlock, 0.f);
    int64_t prefetchFrom = 0;
    bool prefetched = false;
    while (audioRun_) {
        double at, rate;
        {
            std::unique_lock<std::mutex> lk(audioMutex_);
            const size_t aheadFrames = (size_t)(ahead_.load() * rateHz_);
            audioCv_.wait_for(lk, std::chrono::milliseconds(20), [&] { return !audioRun_ || queued_ < aheadFrames; });
            if (!audioRun_) break;
            if (queued_ >= aheadFrames) continue;
            at = mixPos_;
            rate = rate_;
        }
        const double asked = at;
        const int chans = outChannels_.load();
        std::shared_ptr<const Project> p = app_.snapshot();
        const bool bypassAudio = app_.bypassAudioFx.load();
        if (audio_.bypassFx != bypassAudio) {
            audio_.bypassFx = bypassAudio;
            chunksStale_ = true;
        }
        int n = kMixBlock;
        const bool loop = loopsNow(this, app_);
        Block b;
        b.rate = rate;
        double next;
        if (rate == 1.0) {
            int64_t ai = (int64_t)std::llround(at);
            const int64_t loopA = (int64_t)std::llround(app_.loopStart * rateHz_), loopB = (int64_t)std::llround(app_.loopEnd * rateHz_);
            if (loop && ai < loopB) n = (int)std::min<int64_t>(n, loopB - ai);
            if (loop && ai >= loopB) { ai = loopA; n = (int)std::min<int64_t>(n, std::max<int64_t>(1, loopB - loopA)); }
            const int64_t endSample = (int64_t)std::llround(p->length() * rateHz_);
            if (!loop && ai >= endSample) reachedEnd_ = true;
            // Events that are costly to make (time-stretched, pitch-shifted, resampled) are made on a worker thread a
            // couple of seconds ahead of the mixer, and again at the start of the loop region when its end is near.
            if (!prefetched || ai < prefetchFrom || ai >= prefetchFrom + rateHz_ / 4) {
                prefetched = true;
                prefetchFrom = ai;
                const int64_t window = (int64_t)(2.5 * rateHz_);
                audio_.prefetch(*p, ai, window);
                if (loop && ai + window > loopB && loopB > loopA) audio_.prefetch(*p, loopA, ai + window - loopB);
            }
            if (chans == 6) {
                float* o[6] = {l.data(), r.data(), ext[0].data(), ext[1].data(), ext[2].data(), ext[3].data()};
                mixBlock(*p, ai, n, o, 6);
            } else {
                const auto m0 = std::chrono::steady_clock::now();
                audio_.render(*p, ai, n, l.data(), r.data(), true);
                diag_.mixMs = diag_.mixMs.load() + std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m0).count();
                diag_.mixed += n;
            }
            audio_.stripPeaks(b.peaks);
            if (metronomeOn_) addClicks(*p, ai, n, l.data(), r.data());
            b.timeline = (double)ai;
            next = (double)(ai + n);
        } else {
            // another rate: read through the timeline's sound at that rate. A block never crosses the loop region's
            // ends (so every sample in it is at timeline + k x rate); past one it starts at the other.
            if (chunksStale_.exchange(false)) chunks_.clear();
            const double loopA = app_.loopStart * rateHz_, loopB = app_.loopEnd * rateHz_;
            if (loop && rate > 0) {
                if (at >= loopB) at = loopA + std::fmod(at - loopB, std::max(1.0, loopB - loopA));
                n = (int)std::max(1.0, std::min((double)n, std::ceil((loopB - at) / rate)));
            } else if (loop && rate < 0) {
                if (at < loopA) at = loopB - std::fmod(loopA - at, std::max(1.0, loopB - loopA));
                n = (int)std::max(1.0, std::min((double)n, std::ceil((at - loopA) / -rate)));
            }
            const double endSample = p->length() * rateHz_;
            if (!loop && rate > 0 && at >= endSample) reachedEnd_ = true;
            if (!loop && rate < 0 && at <= 0) reachedStart_ = true;
            if (std::fabs(rate) > kMutedRate) {
                std::fill(l.begin(), l.begin() + n, 0.f);           // VEGAS is quiet at its fastest shuttle speeds
                std::fill(r.begin(), r.begin() + n, 0.f);
                for (auto& x : ext) std::fill(x.begin(), x.begin() + n, 0.f);
                next = at + n * rate;
            } else {
                next = mixVarispeed(*p, at, n, l.data(), r.data(), b.peaks);
                if (chans == 6)
                    for (int c = 0; c < 4; ++c) std::memcpy(ext[c].data(), varispeedExt_[c].data(), (size_t)n * sizeof(float));
            }
            b.timeline = at;
        }
        b.l.assign(l.begin(), l.begin() + n);
        b.r.assign(r.begin(), r.begin() + n);
        if (chans == 6)
            for (int c = 0; c < 4; ++c) b.ext[c].assign(ext[c].begin(), ext[c].begin() + n);
        std::lock_guard<std::mutex> lk(audioMutex_);
        if (mixPos_ != asked || rate_ != rate) continue;
        queue_.push_back(std::move(b));
        queued_ += (size_t)n;
        mixPos_ = next;
    }
}

// The device's thread: hand over what has been mixed (silence if the mixer has fallen behind).
void Playback::setMetronome(bool on, std::vector<float> normal, std::vector<float> accent)
{
    {
        std::lock_guard<std::mutex> lk(metronomeMutex_);
        clickNormal_ = std::move(normal);
        clickAccent_ = std::move(accent);
    }
    metronomeOn_ = on;
}

void Playback::addClicks(const Project& p, int64_t at, int n, float* l, float* r)
{
    const double tempo = p.settings.tempo > 1 ? p.settings.tempo : 120.0;
    const int perMeasure = std::max(1, p.settings.beatsPerMeasure);
    const double beat = 60.0 / tempo * 4.0 / std::max(1, p.settings.noteGetsBeat) * rateHz_;
    if (!(beat > 8)) return;
    std::lock_guard<std::mutex> lk(metronomeMutex_);
    const int clickLen = (int)(0.03 * rateHz_);
    const int64_t longest = std::max<int64_t>(clickLen, (int64_t)std::max(clickNormal_.size(), clickAccent_.size()));
    int64_t k = (int64_t)std::floor((double)(at - longest) / beat);
    for (;; ++k) {
        const int64_t start = (int64_t)std::llround(k * beat);
        if (start >= at + n) break;
        if (k < 0) continue;
        const bool accent = k % perMeasure == 0;
        const std::vector<float>& own = accent ? clickAccent_ : clickNormal_;
        for (int64_t s = std::max(start, at); s < at + n; ++s) {
            const int64_t i = s - start;
            float v;
            if (!own.empty()) {
                if (i >= (int64_t)own.size()) break;
                v = own[(size_t)i];
            } else {
                if (i >= clickLen) break;
                const double t = (double)i / rateHz_;
                v = (float)(0.5 * std::sin(2.0 * 3.14159265358979 * (accent ? 1500.0 : 1000.0) * t) * std::exp(-t * 120.0));
            }
            l[s - at] += v;
            r[s - at] += v;
        }
    }
}

void Playback::pull(float* l, float* r, int frames)
{
    float* out[2] = {l, r};
    pullChannels(out, 2, frames);
}

void Playback::pullChannels(float* const* out, int channels, int frames)
{
    float* l = out[0];
    float* r = out[1];
    int done = 0;
    float pl = 0, pr = 0;
    const float fall = (float)std::pow(10.0, -kMeterFallDb * frames / (20.0 * rateHz_));
    {
        std::lock_guard<std::mutex> lk(audioMutex_);
        std::lock_guard<std::mutex> mk(meterMutex_);
        for (auto& m : meters_) {
            m.left *= fall;
            m.right *= fall;
            for (float& c : m.ch) c *= fall;
        }
        // the first sound waits until the mixer is ahead, for a short time at most: otherwise the first blocks of a
        // project that is slow to start would be followed by gaps
        const bool holdBack = !heardSound_ && prerollLeft_ > 0 && !reachedEnd_ && queued_ < (size_t)(ahead_.load() * rateHz_);
        if (holdBack) prerollLeft_ -= frames;
        while (!holdBack && done < frames && !queue_.empty()) {
            Block& b = queue_.front();
            for (const auto& bp : b.peaks) {
                AudioEngine::StripPeak* m = nullptr;
                for (auto& x : meters_)
                    if (x.id == bp.id) { m = &x; break; }
                if (!m) {
                    meters_.push_back(bp);
                    continue;
                }
                m->left = std::max(m->left, bp.left);
                m->right = std::max(m->right, bp.right);
                m->channels = bp.channels;
                for (int c = 0; c < 6; ++c) m->ch[c] = std::max(m->ch[c], bp.ch[c]);
            }
            const size_t n = std::min(b.l.size() - b.used, (size_t)(frames - done));
            {
                const double t = b.timeline + (double)b.used * b.rate;
                const int64_t dev = handedFrames_ + done;
                bool joined = false;
                if (!handed_.empty()) {
                    const HandedRun& last = handed_.back();
                    joined = last.rate == b.rate && std::fabs(last.timeline + (double)(dev - last.dev) * last.rate - t) < 1e-6;
                }
                if (!joined) {
                    handed_.push_back(HandedRun{dev, t, b.rate});
                    while (handed_.size() > 256) handed_.pop_front();
                }
            }
            std::memcpy(l + done, b.l.data() + b.used, n * sizeof(float));
            std::memcpy(r + done, b.r.data() + b.used, n * sizeof(float));
            for (int c = 2; c < channels; ++c) {
                const std::vector<float>& src = b.ext[c - 2];
                if (src.size() >= b.used + n) std::memcpy(out[c] + done, src.data() + b.used, n * sizeof(float));
                else std::memset(out[c] + done, 0, n * sizeof(float));
            }
            b.used += n;
            done += (int)n;
            queued_ -= n;
            heardPos_ = b.timeline + (double)b.used * b.rate;
            if (b.used == b.l.size()) queue_.pop_front();
        }
    }
    {
        std::lock_guard<std::mutex> lk(audioMutex_);
        if (done > 0) heardSound_ = true;
        if (heardSound_ && done < frames) diag_.silence += frames - done;
        // the device ran dry: the mixer keeps more mixed ahead from now on, so a slow stretch (a new file opened, a
        // seek in a long file) is not heard again
        if (done < frames && heardSound_ && ahead_ < kAheadMost) ahead_ = std::min(kAheadMost, ahead_.load() * 1.5);
        if (done < frames && !handed_.empty()) {
            const HandedRun& last = handed_.back();
            const double t = last.timeline + (double)(handedFrames_ + done - last.dev) * last.rate;
            handed_.push_back(HandedRun{handedFrames_ + done, t, 0.0});
            while (handed_.size() > 256) handed_.pop_front();
        }
        handedFrames_ += frames;
        const double nowPull = steadyNow();
        if (diag_.lastPull > 0 && nowPull - diag_.lastPull > diag_.maxPullGap.load()) diag_.maxPullGap = nowPull - diag_.lastPull;
        diag_.lastPull = nowPull;
        ++diag_.pulls;
        diag_.pullFrames += frames;
        pullWall_ = nowPull;
        pullFrames_ = frames;
    }
    if (done < frames)
        for (int c = 0; c < channels; ++c) std::memset(out[c] + done, 0, (size_t)(frames - done) * sizeof(float));
    if (app_.muteAllAudio)
        for (int c = 0; c < channels; ++c) std::memset(out[c], 0, (size_t)done * sizeof(float));
    const float* tl = l;
    const float* tr = r;
    if (channels == 6 && done > 0) {
        for (auto& x : fold_)
            if ((int)x.size() < done) x.resize((size_t)done);
        const float* six[6] = {out[0], out[1], out[2], out[3], out[4], out[5]};
        surroundDownmix(six, done, fold_[0].data(), fold_[1].data());
        tl = fold_[0].data();
        tr = fold_[1].data();
    }
    for (int k = 0; k < done; ++k) {
        pl = std::max(pl, std::fabs(tl[k]));
        pr = std::max(pr, std::fabs(tr[k]));
    }
    peakL_ = std::max(pl, peakL_.load() * 0.9f);
    peakR_ = std::max(pr, peakR_.load() * 0.9f);
    if (done > 0) {
        std::lock_guard<std::mutex> lk(tapMutex_);
        if (tap_) tap_(tl, tr, done);
    }
    if (dim_) {
        for (int c = 0; c < channels; ++c)
            for (int k = 0; k < done; ++k) out[c][k] *= kDimGain;
    }
    if (mono_ && channels == 2)
        for (int k = 0; k < done; ++k) out[0][k] = out[1][k] = 0.5f * (out[0][k] + out[1][k]);
    audioCv_.notify_one();
}

void Playback::setOutputTap(OutputTap tap)
{
    std::lock_guard<std::mutex> lk(tapMutex_);
    tap_ = std::move(tap);
}

void Playback::stripLevels(std::vector<AudioEngine::StripPeak>& out) const
{
    std::lock_guard<std::mutex> lk(meterMutex_);
    out.assign(meters_.begin(), meters_.end());
}

void Playback::requestFrame()
{
    if (playing_) {
        videoCv_.notify_all();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        if (stoppedCancel_) stoppedCancel_->store(true);
        wanted_ = true;
        wantedTime_ = app_.cursor();
        wantedFull_ = false;
        const double now = steadyNow();
        if (doneSerial_ >= wantedSerial_) pendingSince_ = now;
        ++wantedSerial_;
        draftShown_ = false;
        requestTimes_.push_back(now);
        while (requestTimes_.size() > 8 || (!requestTimes_.empty() && requestTimes_.front() < now - 1.0)) requestTimes_.pop_front();
    }
    videoCv_.notify_all();
}

double Playback::renderingFor() const
{
    const double now = steadyNow();
    if (playing_) {
        const double last = shownAt_.empty() ? playBegan_ : std::max(playBegan_, shownAt_.back());
        const double fps = clock_.fps > 0 ? clock_.fps : 30.0;
        return std::max(0.0, now - last - 1.0 / (fps * std::max(0.05, std::fabs(clock_.rate))));
    }
    std::lock_guard<std::mutex> lk(videoMutex_);
    return doneSerial_ >= wantedSerial_ ? 0.0 : std::max(0.0, now - pendingSince_);
}

void Playback::setFullQuality(bool on)
{
    if (fullQuality_.exchange(on) == on) return;
    pool_.setUseProxies(!on);
    requestFrame();
}

void Playback::setPreviewSize(int width, int height, bool autoSize)
{
    const int w = std::max(0, width), h = std::max(0, height);
    const bool wChanged = previewW_.exchange(w) != w;
    const bool hChanged = previewH_.exchange(h) != h;
    const bool changed = wChanged || hChanged;
    autoSize_ = autoSize;
    if (changed && playing_) requestFrame();
}

// Frames shown a second while playing: over the last second, or - when they come slower than three a second - over
// the last four (so that a frame every two seconds reads 0.5 rather than flickering between 0 and 1).
double Playback::previewFrameRate() const
{
    if (!playing_ || shownAt_.empty()) return 0.0;
    const double now = steadyNow();
    const double played = now - playBegan_;
    if (played < 0.1) return 0.0;
    size_t last1 = 0, last4 = 0;
    for (double t : shownAt_) {
        if (t > now - 1.0) ++last1;
        if (t > now - 4.0) ++last4;
    }
    if (last1 >= 3) return (double)last1 / std::min(1.0, played);
    return (double)last4 / std::min(4.0, played);
}

// The fraction of the project's size to render a playing frame at: the preview's own size (never larger than the
// project's), times Auto's step down 'dyn'.
double Playback::frameScale(const Project& p, double dyn)
{
    double s = 1.0;
    const int w = previewW_.load(), h = previewH_.load();
    if (w > 0 && h > 0 && p.settings.width > 0 && p.settings.height > 0)
        s = std::min(1.0, std::max((double)w / p.settings.width, (double)h / p.settings.height));
    if (autoSize_) s *= dyn;
    s = std::max(s, std::max(1.0 / 16.0, 16.0 / std::max(16, p.settings.width)));
    return s >= 0.999 ? 1.0 : s;
}

// ---- frames while playing
// The frame threads know where playing is from the clock the UI sets every tick (the position heard then, and when).

void Playback::setPlayClock(double pos)
{
    const Project& p = app_.project();
    std::lock_guard<std::mutex> lk(videoMutex_);
    clock_.pos = pos;
    clock_.wall = steadyNow();
    clock_.rate = rate_;
    clock_.fps = p.settings.fps() > 0 ? p.settings.fps() : 30.0;
    {
        const double shown = clock_.fps * std::max(0.01, std::fabs(rate_));
        clock_.step = std::max(1, (int)std::floor(shown / std::max(24.0, displayHz_) + 0.3));
    }
    clock_.length = p.length();
    clock_.loop = loopsNow(this, app_);
    clock_.loopStart = app_.loopStart;
    clock_.loopEnd = app_.loopEnd;
}

double Playback::wrapTime(double t) const
{
    const PlayClock& c = clock_;
    const double L = c.loopEnd - c.loopStart;
    if (!c.loop || !(L > 0)) return t;
    if (t >= c.loopEnd) t = c.loopStart + std::fmod(t - c.loopStart, L);
    else if (t < c.loopStart) t = c.loopEnd - std::fmod(c.loopStart - t, L);
    return std::min(std::max(t, c.loopStart), c.loopEnd);
}

double Playback::clockPos() const
{
    const PlayClock& c = clock_;
    const double since = std::max(0.0, std::min(0.1, steadyNow() - c.wall));
    return wrapTime(c.pos + c.rate * since);
}

double Playback::aheadOf(double from, double to) const
{
    const PlayClock& c = clock_;
    double d = (to - from) * (c.rate < 0 ? -1.0 : 1.0);
    const double L = c.loopEnd - c.loopStart;
    if (c.loop && L > 0) {
        if (d < -L / 2) d += L;
        else if (d > L / 2) d -= L;
    }
    return d;
}

double Playback::frameStart(int64_t f) const { return (double)(clock_.rate < 0 ? f + 1 : f) / clock_.fps; }
double Playback::frameEnd(int64_t f) const { return (double)(clock_.rate < 0 ? f : f + 1) / clock_.fps; }

int64_t Playback::nextFrame(int64_t g) const
{
    const PlayClock& c = clock_;
    g += (c.rate < 0 ? -1 : 1) * std::max(1, c.step);
    if (c.loop && c.loopEnd > c.loopStart) {
        const int64_t first = (int64_t)std::floor(c.loopStart * c.fps + 1e-6), last = std::max(first, (int64_t)std::ceil(c.loopEnd * c.fps - 1e-6) - 1);
        if (g > last) g = first;
        if (g < first) g = last;
    }
    return g;
}

bool Playback::frameTaken(int64_t g) const
{
    if (g == shownFrame_) return true;
    for (const PlayJob& j : inFlight_) if (j.frame == g) return true;
    for (const ReadyFrame& r : ready_) if (r.frame == g) return true;
    return false;
}

void Playback::abandonLateJobs(double pos, double lateTol)
{
    const double pe = prewarmEnd_.load();
    if (pe >= 0 && aheadOf(pe, pos) > 0) prewarmStop_ = true;
    for (PlayJob& j : inFlight_)
        if (!j.late && aheadOf(frameEnd(j.frame), pos) > lateTol) {
            j.late = true;
            j.lateAt = steadyNow();
            j.cancel->store(true);
        }
}

// True if the project's frames can be rendered on several threads at once. Per-plug-in thread safety is handled
// elsewhere (FxRuntime::prepareLanes, RenderGate); only nested projects whose effects aren't created yet block it.
bool Playback::parallelSafe(const Project& p) const
{
    return nestedVideoParallelSafe(p, true);
}

void Playback::newPlayGeneration()
{
    ++playGen_;
    for (PlayJob& j : inFlight_) j.cancel->store(true);
    inFlight_.clear();
    ready_.clear();
    shownFrame_ = -1;
}

// How long frames take to render now: the average of the last kRenderWindow frames (renderEst_). Playing looks ahead
// by this much where it knows nothing better (estimateAt). A late frame that was stopped soon after being told to
// ('stoppedEarly') did not get to finish: it counts as twice what it took, at least twice the average.
void Playback::noteRenderTime(double seconds, bool lateCancelled, bool stoppedEarly)
{
    if (lateCancelled && stoppedEarly) seconds = std::max(seconds * 2.0, renderEst_ * 2.0);
    recentRender_.push_back(std::min(seconds, 30.0));
    while (recentRender_.size() > kRenderWindow) recentRender_.pop_front();
    double sum = 0;
    for (double v : recentRender_) sum += v;
    renderEst_ = std::min(30.0, sum / (double)recentRender_.size());
}

// ---- Look-ahead by segment
// The times where what is on screen changes as a whole: each video event's start and end (and the project's start),
// from the project as it is now. The averages learnt for the old cuts are forgotten when the project changes.
void Playback::refreshCuts(const Project& p)
{
    const uint64_t stamp = frameStamp();
    if (stamp == cutsStamp_) return;
    cutsStamp_ = stamp;
    cuts_.clear();
    cuts_.push_back(0.0);
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Video || t.mute) continue;
        for (const Event& e : t.events) {
            if (e.mute) continue;
            cuts_.push_back(std::max(0.0, e.start));
            cuts_.push_back(std::max(0.0, e.start + e.length));
        }
    }
    std::sort(cuts_.begin(), cuts_.end());
    std::vector<double> u;
    for (double c : cuts_)
        if (u.empty() || c - u.back() > 1e-6) u.push_back(c);
    cuts_.swap(u);
    segStats_.clear();
}

// The cut at or before 't' (where t's segment begins)
double Playback::segmentStart(double t) const
{
    auto it = std::upper_bound(cuts_.begin(), cuts_.end(), t + 1e-9);
    return it == cuts_.begin() ? 0.0 : *(it - 1);
}

// The first cut after 't' in the playing direction 'dir' (for dir < 0: the start of t's segment, unless t is on it),
// or a negative number if there is none.
double Playback::nextCut(double t, int dir) const
{
    if (dir >= 0) {
        auto it = std::upper_bound(cuts_.begin(), cuts_.end(), t + 1e-9);
        return it == cuts_.end() ? -1.0 : *it;
    }
    auto it = std::lower_bound(cuts_.begin(), cuts_.end(), t - 1e-9);
    return it == cuts_.begin() ? -1.0 : *(it - 1);
}

// How long a frame at 't' is expected to take: the average for its segment once a frame of it has been rendered, the
// overall average until then.
double Playback::estimateAt(double t) const
{
    auto it = segStats_.find((int64_t)std::llround(segmentStart(t) * 1000.0));
    if (it != segStats_.end() && it->second.n > 0) return it->second.avg;
    return renderEst_;
}

void Playback::noteSegmentTime(double t, double seconds)
{
    SegStat& st = segStats_[(int64_t)std::llround(segmentStart(t) * 1000.0)];
    // a plain average of the first few, then a running one (the last ~8 frames count most)
    st.n = std::min(st.n + 1, 1000);
    const double w = st.n <= 8 ? 1.0 / st.n : 1.0 / 8.0;
    st.avg += w * (std::min(seconds, 30.0) - st.avg);
}

// The next frame for frame thread 'index' to start, if any (videoMutex_ held): the frame the sound will be at once it
// is rendered (render time plus a little to spare), unless taken already (then the next), and not so far ahead that
// it would wait.
bool Playback::planPlayJob(int index, std::unique_lock<std::mutex>& lk, int64_t& frame, std::shared_ptr<std::atomic<bool>>& cancel,
                           double& dyn, double& waitFor, bool& aheadJob)
{
    (void)lk;
    aheadJob = false;
    const PlayClock& c = clock_;
    waitFor = 0.01;
    const double fps = c.fps;
    const int step = std::max(1, c.step);
    const double period = step / fps;
    const double ar = std::max(0.01, std::fabs(c.rate));
    const int dir = c.rate < 0 ? -1 : 1;
    const double pos = clockPos();
    const double lateTol = std::max(period, 0.02 * ar);
    abandonLateJobs(pos, lateTol);
    ramToReady();
    const int workers = std::max(1, workers_.load());
    std::shared_ptr<const Project> proj = app_.snapshot();
    refreshCuts(*proj);
    // the average render time where the frame aimed at lies: aimed at by the average here, then again by the average
    // of the segment that lands in (a slow clip coming up pulls the aim further out)
    double R = estimateAt(pos);
    R = std::max(R, estimateAt(wrapTime(pos + dir * ar * (R + kRenderMargin))));
    const bool behind = R * ar > period * 0.8;
    const int using_ = behind ? workers : 1;
    size_t waiting = 0;
    for (const ReadyFrame& r : ready_) if (r.ms > 0) ++waiting;
    // ---- The next clips: the first frame after each coming cut is started its own segment's average (plus the margin)
    // before the cut, so it is there when the clip begins even when the clip renders slowly. Looked at: the cuts within
    // the longest of those averages (a few at most).
    {
        double t = pos;
        for (int k = 0; k < kCutsAhead; ++k) {
            const double cut = nextCut(t, dir);
            if (cut < 0 || (!c.loop && cut >= c.length) || (c.loop && (cut <= c.loopStart || cut >= c.loopEnd))) break;
            const double until = aheadOf(pos, cut) / ar;          // seconds of playing until the cut
            if (until <= 0) { t = cut; continue; }
            const double e = estimateAt(dir > 0 ? cut : std::max(0.0, cut - 1e-6));
            const double lead = e * using_ + kRenderMargin + period / ar;
            if (until > std::max(lead, R * using_ + kRenderMargin) + kAheadSeconds) break;
            if (until <= lead && (index == 0 || behind || workers > 1) && waiting <= (size_t)using_ + 1) {
                int64_t g = (int64_t)std::floor((dir > 0 ? cut : cut - 1e-6) * fps + 1e-6);
                if (step > 1) g = dir > 0 ? (g + step - 1) / step * step : g / step * step;
                if (g >= 0 && !frameTaken(g) && aheadOf(pos, frameEnd(g)) > 0 && !ram_.has(frameStamp(), g, frameScale(*proj, dynScale_))) {
                    frame = g;
                    cancel = std::make_shared<std::atomic<bool>>(false);
                    dyn = dynScale_;
                    aheadJob = false;
                    ++diag_.cutsAhead;
                    return true;
                }
            }
            t = cut;
        }
    }
    double u = wrapTime(pos + dir * ar * (R + kRenderMargin));
    int64_t f = (int64_t)std::floor(u * fps + 1e-6);
    if (step > 1) f = dir > 0 ? (f + step - 1) / step * step : f / step * step;
    for (int k = 0; k < 16 && frameTaken(f); ++k) f = nextFrame(f);
    const bool fOk = !(frameTaken(f) || f < 0 || (!c.loop && (double)f / fps >= c.length)) && aheadOf(pos, frameEnd(f)) > 0;
    waitFor = 0.01;
    if (fOk && (index == 0 || behind) && waiting <= (size_t)using_) {
        const double ahead = aheadOf(pos, frameStart(f)) / ar;
        const double reach = R * using_ + kRenderMargin + period / ar + 0.01;
        if (ahead <= reach) {
            frame = f;
            cancel = std::make_shared<std::atomic<bool>>(false);
            dyn = dynScale_;
            aheadJob = false;
            return true;
        }
        waitFor = std::max(0.001, std::min(0.01, ahead - reach));
    }
    // ---- Rendering ahead
    // A thread with nothing to render for the next moments renders the frames after them into the RAM preview (up to
    // kAheadSeconds), so a slow stretch eats into frames already made instead of dropping frames.
    if (ram_.limit() == 0 || !aheadOn_) return false;
    const std::shared_ptr<const Project>& sp = proj;
    const double scale = frameScale(*sp, dynScale_);
    const size_t frameBytes = (size_t)std::max(1.0, sp->settings.width * scale) * (size_t)std::max(1.0, sp->settings.height * scale) * 4;
    const int64_t room = (int64_t)(ram_.limit() / 2 / std::max<size_t>(1, frameBytes));
    // how far ahead: kAheadSeconds, or longer where frames are slow (three times the slowest average coming up), as far
    // as half the RAM preview holds
    double slowest = R;
    {
        double t = pos;
        for (int k = 0; k < kCutsAhead; ++k) {
            const double cut = nextCut(t, dir);
            if (cut < 0 || aheadOf(pos, cut) / ar > std::max(kAheadSeconds, 3.0 * slowest)) break;
            slowest = std::max(slowest, estimateAt(dir > 0 ? cut : std::max(0.0, cut - 1e-6)));
            t = cut;
        }
    }
    const double aheadSeconds = std::min(kAheadMostSeconds, std::max(kAheadSeconds, 3.0 * slowest));
    const int64_t horizon = std::min<int64_t>(room, (int64_t)std::ceil(aheadSeconds * ar * fps / step));
    if (horizon < 4) return false;
    const uint64_t stamp = frameStamp();
    // Where to start: from the frame the average says (pos + R), and from each coming cut that is within reach, the
    // stretch after it that its own average says it needs. The start point whose frames are due soonest after the time
    // it takes to make them goes first.
    struct StartPoint { double t; double slack; int64_t count; };
    std::vector<StartPoint> starts;
    starts.push_back({wrapTime(pos + dir * ar * (std::max(R, 0.0) + kRenderMargin)), 0.0, horizon});
    {
        double t = pos;
        for (int k = 0; k < kCutsAhead; ++k) {
            const double cut = nextCut(t, dir);
            if (cut < 0 || (!c.loop && cut >= c.length)) break;
            const double until = aheadOf(pos, cut) / ar;
            if (until > aheadSeconds) break;
            const double e = estimateAt(dir > 0 ? cut : std::max(0.0, cut - 1e-6));
            if (until > 0)
                starts.push_back({dir > 0 ? cut : cut - 1e-6, until - e, std::max<int64_t>(4, (int64_t)std::ceil((e + kRenderMargin) * ar * fps / step) + 1)});
            t = cut;
        }
    }
    std::stable_sort(starts.begin() + 1, starts.end(), [](const StartPoint& a, const StartPoint& b) { return a.slack < b.slack; });
    // a coming clip whose frames take longer than the time left until it starts comes before the plain run ahead
    std::stable_sort(starts.begin(), starts.end(), [](const StartPoint& a, const StartPoint& b) { return (a.slack < 0) > (b.slack < 0); });
    for (const StartPoint& sp0 : starts) {
        int64_t g = (int64_t)std::floor(sp0.t * fps + 1e-6);
        if (step > 1) g = dir > 0 ? (g + step - 1) / step * step : g / step * step;
        for (int64_t k = 0; k < std::min(horizon, sp0.count); ++k, g = nextFrame(g)) {
            if (g < 0 || (!c.loop && (double)g / fps >= c.length)) break;
            if (aheadOf(pos, frameStart(g)) / ar > aheadSeconds) break;
            if (aheadOf(pos, frameEnd(g)) <= 0 || frameTaken(g)) continue;
            if (ram_.has(stamp, g, scale)) continue;
            frame = g;
            cancel = std::make_shared<std::atomic<bool>>(false);
            dyn = dynScale_;
            aheadJob = true;
            waitFor = 0.0;
            return true;
        }
    }
    waitFor = std::min(waitFor, 0.02);
    return false;
}

// Frames already in the RAM preview (played, scrubbed, made ahead, or from Shift+B) are ready at once: the next few
// go straight to the ready frames, so replayed or pre-rendered stretches play from memory. videoMutex_ held.
void Playback::ramToReady(bool post)
{
    if (ram_.limit() == 0 || ram_.frames() == 0) return;
    const PlayClock& c = clock_;
    const double fps = c.fps;
    const int step = std::max(1, c.step);
    const int dir = c.rate < 0 ? -1 : 1;
    const double pos = clockPos();
    std::shared_ptr<const Project> sp = app_.snapshot();
    const double scale = frameScale(*sp, dynScale_);
    const uint64_t stamp = frameStamp();
    ram_.setCursor((int64_t)std::floor(pos * fps + 1e-6));
    int64_t g = (int64_t)std::floor(pos * fps + 1e-6);
    if (step > 1) g = dir > 0 ? (g + step - 1) / step * step : g / step * step;
    bool added = false;
    for (int k = 0; k < 12; ++k, g = nextFrame(g)) {
        if (g < 0 || (!c.loop && (double)g / fps >= c.length)) break;
        if (aheadOf(pos, frameEnd(g)) <= 0 || frameTaken(g)) continue;
        QImage im;
        if (!ram_.find(stamp, g, scale, im)) continue;
        ready_.push_back(ReadyFrame{g, im, 0.0, (double)im.width() / std::max(1, sp->settings.width)});
        ++diag_.ramHits;
        added = true;
    }
    if (added && post) {
        std::shared_ptr<std::atomic<bool>> alive = alive_;
        AppState::post([this, alive] {
            if (*alive) present();
        });
    }
}

// Interface thread, every tick and when a frame is ready: shows the ready frame whose time it is, drops those whose
// time has gone, keeps those still to come.
void Playback::present()
{
    if (!playing_) return;
    const double pos = position();
    const Project& p = app_.project();
    ReadyFrame pick;
    bool have = false;
    double nextDue = -1;
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        ramToReady(false);
        const PlayClock& c = clock_;
        const double fps = c.fps;
        const int step = std::max(1, c.step);
        const double period = step / fps;
        const double ar = std::max(0.01, std::fabs(c.rate));
        const double lateTol = std::max(period, 0.02 * ar);
        const int64_t fnow = p.frameAt(pos);
        if (lastDueFrame_ >= 0 && fnow != lastDueFrame_) {
            int64_t d = std::llabs(fnow - lastDueFrame_);
            const int64_t loopFrames = (int64_t)std::llround((c.loopEnd - c.loopStart) * fps);
            if (c.loop && loopFrames > 0 && d > loopFrames / 2) d = std::max<int64_t>(0, loopFrames - d);
            if (step > 1) {
                d = std::llabs(fnow / step - lastDueFrame_ / step);
                if (c.loop && loopFrames > 0 && d > loopFrames / step / 2) d = std::max<int64_t>(0, loopFrames / step - d);
            }
            dueFrames_ += (uint64_t)d;
        }
        lastDueFrame_ = fnow;
        abandonLateJobs(pos, lateTol);
        int best = -1;
        double bestSince = 0;
        auto beforeShown = [&](int64_t f) { return shownFrame_ >= 0 && aheadOf(frameStart(shownFrame_), frameStart(f)) <= 0; };
        for (size_t k = 0; k < ready_.size(); ++k) {
            const double since = aheadOf(frameStart(ready_[k].frame), pos);
            if (since < -kEarly * ar) continue;
            if (beforeShown(ready_[k].frame)) continue;
            if (aheadOf(frameEnd(ready_[k].frame), pos) > lateTol) continue;
            if (best < 0 || since < bestSince) {
                best = (int)k;
                bestSince = since;
            }
        }
        if (best >= 0) {
            pick = ready_[(size_t)best];
            have = true;
            shownFrame_ = pick.frame;
        }
        for (size_t k = ready_.size(); k-- > 0;) {
            const double since = aheadOf(frameStart(ready_[k].frame), pos);
            if ((int)k == best || aheadOf(frameEnd(ready_[k].frame), pos) > lateTol || (best >= 0 && since >= bestSince) ||
                beforeShown(ready_[k].frame))
                ready_.erase(ready_.begin() + (std::ptrdiff_t)k);
        }
        for (const ReadyFrame& r : ready_) {
            const double until = aheadOf(pos, frameStart(r.frame)) / ar;
            if (until >= 0 && (nextDue < 0 || until < nextDue)) nextDue = until;
        }
    }
    ++diag_.presents;
    {
        const int64_t f = p.frameAt(pos);
        if (f != diag_.lastFrame) { ++diag_.positions; diag_.lastFrame = f; }
    }
    if (have) { ++diag_.shown; showFrame(pick.image, p.frameTime(pick.frame), pick.ms, pick.scale, true); }
    if (nextDue >= 0 && presentTimer_) presentTimer_->start(std::max(0, (int)std::ceil(nextDue * 1000.0)));
}

void Playback::showFrame(const QImage& image, double seconds, double ms, double scale, bool counted)
{
    lastFrame_ = image;
    ++framesShown_;
    lastRenderMs_ = ms;
    lastScale_ = scale;
    if (counted) {
        ++playShown_;
        const double now = steadyNow();
        shownAt_.push_back(now);
        while (!shownAt_.empty() && shownAt_.front() < now - 4.0) shownAt_.pop_front();
    }
    if (onFrame) onFrame(image, seconds, ms);
}

uint64_t Playback::frameStamp() const
{
    return app_.revision() * 1000003ull + app_.fx.videoRevision() + (app_.bypassAllFx ? 7 : 0) + (app_.muteAllVideo ? 31 : 0) + (fullQuality_ ? 13 : 0) +
           videoProxyGeneration() * 7919ull +
           nestedProjectsGeneration() * 104729ull;
}

void RamFrameStore::setLimit(size_t bytes)
{
    std::lock_guard<std::mutex> lk(m_);
    limit_ = bytes;
    if (limit_ == 0) {
        map_.clear();
        bytes_ = 0;
    } else {
        evictLocked(nullptr);
    }
}

size_t RamFrameStore::limit() const { std::lock_guard<std::mutex> lk(m_); return limit_; }

bool RamFrameStore::find(uint64_t stamp, int64_t frame, double scale, QImage& out)
{
    std::lock_guard<std::mutex> lk(m_);
    if (!haveStamp_ || stamp_ != stamp) return false;
    auto it = map_.lower_bound(Key(frame, scaleKey(scale)));
    if (it == map_.end() || it->first.first != frame) return false;
    it->second.used = ++tick_;
    out = it->second.image;
    return true;
}

bool RamFrameStore::has(uint64_t stamp, int64_t frame, double scale) const
{
    std::lock_guard<std::mutex> lk(m_);
    if (!haveStamp_ || stamp_ != stamp) return false;
    auto it = map_.lower_bound(Key(frame, scaleKey(scale)));
    return it != map_.end() && it->first.first == frame;
}

bool RamFrameStore::put(uint64_t stamp, bool current, int64_t frame, double scale, const QImage& image, bool pinned)
{
    if (image.isNull()) return false;
    std::lock_guard<std::mutex> lk(m_);
    if (limit_ == 0) return false;
    if (!haveStamp_ || stamp_ != stamp) {
        if (!current) return false;
        map_.clear();
        bytes_ = 0;
        stamp_ = stamp;
        haveStamp_ = true;
    }
    const size_t sz = (size_t)image.sizeInBytes();
    if (sz > limit_) return false;
    const Key key(frame, scaleKey(scale));
    auto it = map_.find(key);
    if (it != map_.end()) {
        bytes_ -= (size_t)it->second.image.sizeInBytes();
        it->second.image = image;
        it->second.used = ++tick_;
        it->second.pinned = it->second.pinned || pinned;
    } else {
        Entry e;
        e.image = image;
        e.used = ++tick_;
        e.pinned = pinned;
        map_[key] = e;
    }
    bytes_ += sz;
    evictLocked(nullptr);
    if (bytes_ > limit_) {
        auto me = map_.find(key);
        if (me != map_.end()) {
            bytes_ -= (size_t)me->second.image.sizeInBytes();
            map_.erase(me);
        }
        return false;
    }
    return map_.count(key) != 0;
}

// Over the limit: the frame with the worst score goes, until it fits. Score: how far its frame is from the cursor
// plus how long since it was used (in uses of the store), counted alike. Pinned frames stay.
void RamFrameStore::evictLocked(const Key*)
{
    while (bytes_ > limit_) {
        auto worst = map_.end();
        double worstScore = -1;
        for (auto it = map_.begin(); it != map_.end(); ++it) {
            if (it->second.pinned) continue;
            const double score = (double)std::llabs(it->first.first - cursor_) + 0.5 * (double)(tick_ - it->second.used);
            if (score > worstScore) {
                worstScore = score;
                worst = it;
            }
        }
        if (worst == map_.end()) break;
        bytes_ -= (size_t)worst->second.image.sizeInBytes();
        map_.erase(worst);
        ++evicted_;
    }
}

void RamFrameStore::setCursor(int64_t frame) { std::lock_guard<std::mutex> lk(m_); cursor_ = frame; }
void RamFrameStore::clear() { std::lock_guard<std::mutex> lk(m_); map_.clear(); bytes_ = 0; haveStamp_ = false; }
size_t RamFrameStore::frames() const { std::lock_guard<std::mutex> lk(m_); return map_.size(); }
size_t RamFrameStore::bytes() const { std::lock_guard<std::mutex> lk(m_); return bytes_; }
uint64_t RamFrameStore::evicted() const { std::lock_guard<std::mutex> lk(m_); return evicted_; }
std::vector<std::pair<int64_t, double>> RamFrameStore::keys() const
{
    std::lock_guard<std::mutex> lk(m_);
    std::vector<std::pair<int64_t, double>> v;
    for (const auto& kv : map_) v.push_back({kv.first.first, kv.first.second / 1000.0});
    return v;
}

int64_t Playback::ramPreview(double from, double to, const std::function<bool(double)>& progress)
{
    std::shared_ptr<const Project> p = app_.snapshot();
    const uint64_t stamp = frameStamp();
    ram_.setLimit((size_t)std::max(0, app_.prefs.ramPreviewMb) * 1024 * 1024);
    const int64_t first = p->frameAt(std::max(0.0, from)), last = std::max(first, p->frameAt(to - 1e-6));
    int64_t made = 0;
    FrameOptions fo;
    fo.skipEffects = app_.bypassAllFx;
    fo.subFrames = fullQuality_;
    for (int64_t f = first; f <= last; ++f) {
        if (progress && !progress((double)(f - first) / (double)(last - first + 1))) break;
        if (ram_.has(stamp, f, 1.0)) {
            ++made;
            continue;
        }
        ImagePtr img = video_.render(*p, p->frameTime(f), fo);
        if (!img) break;
        QImage q = toQImage(*img);
        if (frameStamp() != stamp) break;
        if (!ram_.put(stamp, true, f, 1.0, q, true)) break;
        ++made;
    }
    return made;
}

void Playback::clearRamPreview()
{
    ram_.clear();
}

size_t Playback::ramPreviewFrames() const
{
    return ram_.frames();
}

static void removeFile(const std::string& path) { QFile::remove(QString::fromStdString(path)); }

uint64_t Playback::prerenderFingerprint(const Project& p, FxRuntime& fx, double from, double to)
{
    // A copy with only what can show between 'from' and 'to': the video tracks (only events reaching into the stretch
    // plus a little either side, for effects that look at neighboring frames), their media, the video output and the
    // settings.
    Project q;
    q.settings = p.settings;
    q.videoOut = p.videoOut;
    q.videoEnvelopes = p.videoEnvelopes;
    q.videoFadeTop = p.videoFadeTop;
    q.videoFadeBottom = p.videoFadeBottom;
    const double margin = 2.0 / std::max(1.0, p.settings.fps());
    std::set<uint64_t> media;
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Video) continue;
        Track c = t;
        c.events.clear();
        for (const Event& e : t.events)
            if (e.start < to + margin && e.end() > from - margin) {
                c.events.push_back(e);
                media.insert(e.mediaId);
            }
        c.height = 0;
        q.tracks.push_back(c);
    }
    for (const MediaItem& m : p.media)
        if (media.count(m.id)) q.media.push_back(m);
    std::string text = q.toJson().dump();
    uint64_t revs = 0;
    q.forEachFx([&](const FxSlot& s) {
        if (ofx::Instance* inst = fx.video(s.id)) revs = revs * 1000003ull + inst->revision.load() + s.id;
    });
    text += strfmt("|%llu|%.6f|%.6f", (unsigned long long)revs, from, to);
    return (uint64_t)std::hash<std::string>()(text);
}

bool Playback::prerender(double from, double to, const std::string& folder, const std::function<bool(double)>& progress, std::string* error)
{
    std::shared_ptr<const Project> p = app_.snapshot();
    if (!(to > from)) {
        if (error) *error = "there is no time selection to prerender";
        return false;
    }
    from = p->frameTime(p->frameAt(std::max(0.0, from)));
    const uint64_t fp = prerenderFingerprint(*p, app_.fx, from, to);
    EncodeSettings es;
    es.audio = false;
    es.video = true;
    es.videoCodec = "mjpeg";
    es.videoKbps = (int)std::min<double>(400000.0, std::max(8000.0, p->settings.width * (double)p->settings.height * p->settings.fps() * 1.6 / 1000.0));
    std::string path;
    for (int n = 1; n < 100000; ++n) {
        path = pathJoin(folder, strfmt("prerender_%llx_%d.avi", (unsigned long long)fp, n));
        if (!fileExists(path)) break;
    }
    es.path = path;
    std::string err;
    const bool ok = exportProject(*p, app_.fx, es, from, to,
                                  [&](const ExportProgress& ep) { return !progress || progress(ep.fraction); }, &err, app_.mediaOpener);
    if (!ok) {
        removeFile(path);
        if (error) *error = err;
        return false;
    }
    std::string rerr;
    std::unique_ptr<MediaReader> reader = app_.mediaOpener ? app_.mediaOpener(path, &rerr) : openMedia(path, &rerr);
    if (!reader || !reader->info().hasVideo) {
        if (error) *error = "the prerendered file could not be read back: " + rerr;
        return false;
    }
    auto seg = std::make_shared<Prerendered>();
    seg->from = from;
    seg->to = to;
    seg->firstFrame = p->frameAt(from);
    seg->frames = std::max<int64_t>(1, (int64_t)std::ceil((to - from) * p->settings.fps() - 1e-6));
    seg->path = path;
    seg->fingerprint = fp;
    seg->reader = std::move(reader);
    {
        std::lock_guard<std::mutex> lk(preMutex_);
        std::vector<std::shared_ptr<Prerendered>> keep;
        for (auto& o : pre_) {
            if (o->to > from && o->from < to) {
                o->reader.reset();
                removeFile(o->path);
            } else {
                keep.push_back(o);
            }
        }
        keep.push_back(seg);
        pre_.swap(keep);
    }
    requestFrame();
    return true;
}

void Playback::checkPrerendered()
{
    std::vector<std::shared_ptr<Prerendered>> segs;
    {
        std::lock_guard<std::mutex> lk(preMutex_);
        segs = pre_;
    }
    if (segs.empty()) return;
    std::shared_ptr<const Project> p = app_.snapshot();
    for (auto& s : segs) {
        const bool now = prerenderFingerprint(*p, app_.fx, s->from, s->to) == s->fingerprint;
        std::lock_guard<std::mutex> lk(preMutex_);
        s->valid = now;
    }
}

std::vector<Playback::PrerenderedRange> Playback::prerenderedRanges() const
{
    std::vector<PrerenderedRange> out;
    std::lock_guard<std::mutex> lk(preMutex_);
    for (const auto& s : pre_) {
        PrerenderedRange r;
        r.from = s->from;
        r.to = s->to;
        r.path = s->path;
        r.valid = s->valid;
        out.push_back(r);
    }
    return out;
}

void Playback::clearPrerendered(bool deleteFiles)
{
    std::vector<std::shared_ptr<Prerendered>> old;
    {
        std::lock_guard<std::mutex> lk(preMutex_);
        old.swap(pre_);
        for (auto& s : old) s->reader.reset();
    }
    if (deleteFiles)
        for (auto& s : old) removeFile(s->path);
    requestFrame();
}

bool Playback::prerenderedAt(double seconds) const
{
    const Project& p = app_.project();
    const int64_t f = p.frameAt(seconds);
    std::lock_guard<std::mutex> lk(preMutex_);
    for (const auto& s : pre_)
        if (s->valid && f >= s->firstFrame && f < s->firstFrame + s->frames) return true;
    return false;
}

// The frame thread: a frame from a prerendered file, if one holds it and is still what the project shows.
QImage Playback::prerenderedFrame(const Project& p, int64_t frame)
{
    std::lock_guard<std::mutex> lk(preMutex_);
    std::shared_ptr<Prerendered> seg;
    for (auto& s : pre_)
        if (s->valid && frame >= s->firstFrame && frame < s->firstFrame + s->frames) seg = s;
    if (!seg || !seg->reader) return QImage();
    const double fps = p.settings.fps();
    ImagePtr img = seg->reader->videoFrame((frame - seg->firstFrame + 0.25) / fps, p.settings.width, p.settings.height);
    if (!img || img->w != p.settings.width || img->h != p.settings.height) return QImage();
    ++preShown_;
    return toQImage(*img);
}

QImage Playback::blackFrame(const Project& p)
{
    QImage black(std::max(1, p.settings.width), std::max(1, p.settings.height), QImage::Format_ARGB32);
    black.fill(Qt::black);
    return black;
}

QImage Playback::renderNow(double seconds)
{
    std::shared_ptr<const Project> p = app_.snapshot();
    FrameOptions fo;
    fo.skipEffects = app_.bypassAllFx;
    fo.subFrames = fullQuality_;
    if (app_.muteAllVideo) return blackFrame(*p);
    ImagePtr img = video_.render(*p, seconds, fo);
    return img ? toQImage(*img) : QImage();
}

// A frame: from the frames kept (the last few, the Dynamic RAM Preview, a prerendered stretch) or rendered now.
// 'rendered' says it was rendered, in 'ms'. Null if it could not be made or was no longer wanted ('cancel').
QImage Playback::renderFrame(const Project& p, double t, int64_t frame, uint64_t stamp, double scale, bool draft,
                               const std::atomic<bool>* cancel, double& ms, bool& rendered, bool quick, bool* approximated, bool setCursor)
{
    ms = 0;
    rendered = false;
    if (approximated) *approximated = false;
    QImage image;
    {
        std::lock_guard<std::mutex> lk(videoMutex_);
        for (const auto& c : cache_)
            if (c.frame == frame && c.stamp == stamp && (c.scale == scale || c.scale == 1.0)) image = c.image;
    }
    if (setCursor) ram_.setCursor(frame);
    if (image.isNull()) ram_.find(stamp, frame, scale, image);
    if (image.isNull() && !app_.bypassAllFx) image = prerenderedFrame(p, frame);
    if (!image.isNull()) return image;
    if (app_.muteAllVideo) {
        rendered = false;
        QImage black = blackFrame(p);
        return scale > 0 && scale < 1.0 ? black.scaled(std::max(1, (int)(black.width() * scale)), std::max(1, (int)(black.height() * scale))) : black;
    }
    const auto t0 = std::chrono::steady_clock::now();
    FrameOptions fo;
    fo.skipEffects = app_.bypassAllFx;
    fo.subFrames = fullQuality_;
    fo.scale = scale;
    fo.draft = draft;
    fo.cancel = cancel;
    // the readers stop when 'cancel' is set and, if quick is set, may give the nearest frame at hand for one a long
    // decode away (MediaReadHints)
    bool approx = false;
    ImagePtr img;
    {
        MediaReadHintsScope hints(cancel, quick);
        img = video_.render(p, t, fo);
        approx = hints.approximated();
    }
    if (approximated) *approximated = approx;
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    rendered = true;
    ++rendered_;
    if (!img || (cancel && cancel->load())) return QImage();
    image = toQImage(*img);
    if (!draft && !approx) {
        const size_t maxBytes = (size_t)std::max(0, app_.prefs.ramPreviewMb) * 1024 * 1024;
        if (ram_.limit() != maxBytes) ram_.setLimit(maxBytes);
        ram_.put(stamp, stamp == frameStamp(), frame, scale, image, false);
        std::lock_guard<std::mutex> lk(videoMutex_);
        cache_.push_back({frame, stamp, image, scale});
        while (cache_.size() > kCacheFrames) cache_.pop_front();
    }
    return image;
}

// Decoding ahead (see prewarmThreads_): takes the soonest event about to start that nobody has read ahead for, and has its
// reader seek to and decode its first frame now.
void Playback::prewarmLoop()
{
    lowerThreadPriority();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(prewarmMutex_);
            if (prewarmQuit_) return;
            if (!playingNow_.load()) {
                prewarmCv_.wait_for(lk, std::chrono::milliseconds(100));
                continue;
            }
        }
        std::shared_ptr<const Project> p = app_.snapshot();
        double pos = 0, rate = 1, window = kPrewarmSeconds;
        {
            std::lock_guard<std::mutex> lk(videoMutex_);
            pos = clockPos();
            rate = clock_.rate;
            // events are read ahead further when frames are slow: twice the average render time plus a second
            window = std::max(kPrewarmSeconds, std::min(10.0, 2.0 * renderEst_ + 1.0));
        }
        const Event* pick = nullptr;
        // events playing a nested project now or soon: their own upcoming events are read ahead too (prewarmNested),
        // whichever comes up first
        std::vector<const Event*> nested;
        double nestedFirst = 1e300;
        if (rate > 0.01) {
            const uint64_t stamp = frameStamp();
            std::lock_guard<std::mutex> lk(prewarmMutex_);
            if (stamp != prewarmStamp_) {
                prewarmDone_.clear();
                prewarmStamp_ = stamp;
            }
            for (const Track& t : p->tracks) {
                if (t.type != TrackType::Video || t.mute) continue;
                for (const Event& e : t.events) {
                    if (e.mute) continue;
                    const MediaItem* m = p->findMedia(e.mediaId);
                    if (m && isProjectFile(m->path)) {
                        if (e.start + e.length > pos && (e.start - pos) / rate <= window) {
                            nested.push_back(&e);
                            nestedFirst = std::min(nestedFirst, std::max(e.start, pos));
                        }
                        continue;
                    }
                    const double ahead = (e.start - pos) / rate;
                    if (ahead < 0.03 || ahead > window || prewarmDone_.count(e.id)) continue;
                    if (!pick || e.start < pick->start) pick = &e;
                }
            }
            if (pick && pick->start <= nestedFirst) prewarmDone_.insert(pick->id);
            else pick = nullptr;
        }
        FrameOptions fo;
        fo.skipEffects = app_.bypassAllFx;
        fo.subFrames = fullQuality_;
        fo.scale = frameScale(*p, 1.0);
        if (!pick && !nested.empty()) {
            std::sort(nested.begin(), nested.end(), [](const Event* a, const Event* b) { return a->start < b->start; });
            if (!prewarmCancel_) prewarmStop_ = false;
            int got = 0;
            for (const Event* e : nested) {
                prewarmEnd_ = e->start + e->length;
                got += video_.prewarmNested(*p, *e, pos, pos + window * rate, fo, kPrewarmKeep, &prewarmStop_, [this](uint64_t key) {
                    std::lock_guard<std::mutex> lk(prewarmMutex_);
                    return prewarmDone_.insert(key ^ 0x4e45535445440000ull).second;
                });
                prewarmEnd_ = -1;
                if (got > 0) break;
            }
            diag_.prewarmed += got;
            if (got > 0) continue;
        }
        if (!pick) {
            std::unique_lock<std::mutex> lk(prewarmMutex_);
            if (!prewarmQuit_) prewarmCv_.wait_for(lk, std::chrono::milliseconds(25));
            continue;
        }
        // decoding ahead for an event ends as soon as play has gone past all of it (abandonLateJobs), rather than
        // going on to a frame nothing will show
        if (!prewarmCancel_) prewarmStop_ = false;
        prewarmEnd_ = pick->start + pick->length;
        if (video_.prewarmEvent(*p, *pick, fo, kPrewarmKeep, &prewarmStop_)) ++diag_.prewarmed;
        prewarmEnd_ = -1;
    }
}

void Playback::renderStoppedFrame(std::unique_lock<std::mutex>& lk)
{
    wanted_ = false;
    const double t = wantedTime_;
    const uint64_t serial = wantedSerial_;
    const bool full = wantedFull_;
    wantedFull_ = false;
    // Scrubbing a heavy project (requests coming fast, frames slow): a draft at the preview's size, quicker;
    // the full frame follows once the cursor rests.
    const double now = steadyNow();
    int recent = 0;
    for (double r : requestTimes_) if (r > now - 0.35) ++recent;
    const double lastMs = pausedRenderMs_;
    lk.unlock();
    std::shared_ptr<const Project> p = app_.snapshot();
    const double periodMs = 1000.0 / std::max(1.0, p->settings.fps());
    const bool draft = !full && recent >= 3 && lastMs > 1.5 * periodMs && !fullQuality_;
    const int64_t frame = p->frameAt(t);
    const double scale = draft ? std::min(0.5, frameScale(*p, 1.0)) : 1.0;
    double ms = 0;
    bool rendered = false;
    // A frame far into a big file with key frames seconds apart takes a long decode from the key frame. So the
    // nearest frame at hand (the key frame, quick) is shown first, then the exact one; a newer request stops the
    // decode at once (cancel). Only the newest request is decoded.
    const bool quick = !full;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    lk.lock();
    if (!quick) {
        stoppedCancel_ = cancel;
        if (wanted_ && wantedSerial_ != serial) cancel->store(true);
    }
    lk.unlock();
    bool approx = false;
    QImage image = renderFrame(*p, t, frame, frameStamp(), scale, draft, cancel.get(), ms, rendered, quick, &approx);
    lk.lock();
    if (stoppedCancel_ == cancel) stoppedCancel_.reset();
    if (cancel->load()) {
        doneSerial_ = std::max(doneSerial_, serial);
        return;
    }
    if (approx) {
        if (!wanted_ && !image.isNull() && !playingNow_) {
            wanted_ = true;
            wantedTime_ = t;
            wantedFull_ = true;
            ++wantedSerial_;
        }
    } else if (rendered && !draft) {
        pausedRenderMs_ = ms;
    }
    doneSerial_ = std::max(doneSerial_, serial);
    if (image.isNull() || serial < shownSerial_ || playingNow_) return;
    shownSerial_ = serial;
    draftShown_ = draft && serial == wantedSerial_;
    draftTime_ = t;
    draftAt_ = steadyNow();
    if (draftShown_) videoCv_.notify_all();
    const double shownScale = (double)image.width() / std::max(1, p->settings.width);
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    AppState::post([this, alive, image, t, ms, shownScale] {
        if (!*alive || playing_) return;
        showFrame(image, t, ms, shownScale, false);
    });
}

// Auto preview size: frames that take longer than a frame lasts on the threads rendering them mean frames are being
// dropped, so render smaller; a good run of quick ones, larger again (as VEGAS's Preview (Auto) does). 'ms' is how
// long the frame just rendered took.
void Playback::adaptPreviewScale(double ms)
{
    const double ar = std::max(0.25, std::fabs(clock_.rate));
    const double period = 1000.0 * std::max(1, clock_.step) / (clock_.fps * ar);
    const int n = renderEst_ * ar > 0.8 * std::max(1, clock_.step) / clock_.fps ? std::max(1, workers_.load()) : 1;
    const double each = ms / n;
    if (each > period * 1.1 && dynScale_ > 0.25) {
        dynScale_ *= 0.5;
        fastFrames_ = 0;
    } else if (each < period * 0.3 && dynScale_ < 1.0) {
        if (++fastFrames_ >= 24) {
            dynScale_ = std::min(1.0, dynScale_ * 2.0);
            fastFrames_ = 0;
        }
    } else {
        fastFrames_ = 0;
    }
}

// Playing: renders the next frame playing needs (or one to make ahead) and queues it to be shown at its time. Returns
// without rendering when there is nothing to start yet (videoMutex_ held on entry and exit).
void Playback::renderPlayingFrame(int index, std::unique_lock<std::mutex>& lk)
{
    int64_t frame = 0;
    std::shared_ptr<std::atomic<bool>> cancel;
    double dyn = 1.0, waitFor = 0.01;
    bool aheadJob = false;
    if (!planPlayJob(index, lk, frame, cancel, dyn, waitFor, aheadJob)) {
        videoCv_.wait_for(lk, std::chrono::duration<double>(waitFor));
        return;
    }
    const uint64_t gen = playGen_;
    PlayJob job{frame, cancel, false};
    job.ahead = aheadJob;
    inFlight_.push_back(job);
    lk.unlock();
    std::shared_ptr<const Project> p = app_.snapshot();
    const double scale = frameScale(*p, dyn);
    double ms = 0;
    bool rendered = false;
    ++diag_.started;
    if (aheadJob) ++diag_.aheadStarted;
    QImage image = renderFrame(*p, p->frameTime(frame), frame, frameStamp(), scale, false, cancel.get(), ms, rendered, false, nullptr, !aheadJob);
    if (rendered) { ++diag_.rendered; diag_.renderMs = diag_.renderMs.load() + ms; }
    if (rendered && aheadJob) { --rendered_; ++renderedAhead_; }
    lk.lock();
    bool late = false;
    double lateAt = 0;
    for (size_t k = 0; k < inFlight_.size(); ++k)
        if (inFlight_[k].cancel == cancel) {
            late = inFlight_[k].late;
            lateAt = inFlight_[k].lateAt;
            inFlight_.erase(inFlight_.begin() + (std::ptrdiff_t)k);
            break;
        }
    if (aheadJob) {
        // every frame made ahead teaches how long frames take in its segment (the stretch between two cuts)
        if (rendered && !cancel->load() && gen == playGen_) noteSegmentTime(p->frameTime(frame), ms / 1000.0);
        // Made for the RAM preview, which renderFrame put it in. Not a frame to show now, nor a measure for the overall
        // average: those are made while the other threads are busy too.
        if (!image.isNull() && !cancel->load()) ++diag_.aheadDone;
        return;
    }
    if (rendered && (!cancel->load() || late)) {
        const bool stoppedEarly = late && steadyNow() - lateAt < 0.05;
        noteRenderTime(ms / 1000.0, late, stoppedEarly);
        // and its segment's average: a frame that came too late still says how long that clip's frames take
        noteSegmentTime(p->frameTime(frame), ms / 1000.0);
        if (autoSize_ && !cancel->load()) adaptPreviewScale(ms);
    }
    if (gen != playGen_ || cancel->load() || image.isNull()) {
        if (cancel->load()) ++diag_.late;
        return;
    }
    ++diag_.ready;
    ready_.push_back(ReadyFrame{frame, image, ms, (double)image.width() / std::max(1, p->settings.width)});
    std::shared_ptr<std::atomic<bool>> alive = alive_;
    AppState::post([this, alive] {
        if (*alive) present();
    });
}

// Frame thread 'index' (the first always runs; more are started for rendering ahead: Preferences' rendering threads).
// Stopped, any of them takes the newest frame asked for; playing, each renders the next frame playing needs and hands
// it to the interface to be shown at its time.
void Playback::videoThread(int index)
{
    FxRuntime::setThreadLane(index);
    std::unique_lock<std::mutex> lk(videoMutex_);
    for (;;) {
        if (videoQuit_) return;
        if (wanted_ && !playingNow_) {
            renderStoppedFrame(lk);
            continue;
        }
        if (draftShown_ && !playingNow_ && index == 0) {
            const double left = draftAt_ + 0.25 - steadyNow();
            if (left <= 0) {
                draftShown_ = false;
                if (!wanted_) {
                    wanted_ = true;
                    wantedTime_ = draftTime_;
                    wantedFull_ = true;
                    if (doneSerial_ >= wantedSerial_) pendingSince_ = steadyNow();
                    ++wantedSerial_;
                }
                continue;
            }
            videoCv_.wait_for(lk, std::chrono::duration<double>(left));
            continue;
        }
        if (playingNow_ && index < workers_.load()) {
            renderPlayingFrame(index, lk);
            continue;
        }
        videoCv_.wait(lk);
    }
}

}  // namespace app
}  // namespace reno
