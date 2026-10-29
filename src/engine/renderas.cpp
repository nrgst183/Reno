// File > Render As's rendering: see renderas.h.
#include "renderas.h"
#include "smartplan.h"
#include <map>
#include "colormgmt.h"
#include "loudness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace reno {

namespace {

// Paths are UTF-8: on Windows the C library's narrow file functions take the ANSI code page instead, so a file whose
// name has characters outside it.
FILE* openUtf8(const std::string& path, const char* mode)
{
#ifdef _WIN32
    return _wfopen(widen(path).c_str(), widen(mode).c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void removeUtf8(const std::string& path)
{
#ifdef _WIN32
    _wremove(widen(path).c_str());
#else
    std::remove(path.c_str());
#endif
}

int normalRotation(int degrees)
{
    degrees %= 360;
    if (degrees < 0) degrees += 360;
    return degrees == 90 || degrees == 180 || degrees == 270 ? degrees : 0;
}

// Whether the encoder that was chosen can make two passes (it keeps statistics between them in a file).
bool encoderTakesTwoPasses(const std::string& description)
{
    return description.find("libx264") != std::string::npos || description.find("libx265") != std::string::npos;
}

}  // namespace

ImagePtr rotateImage(const ImagePtr& img, int degrees)
{
    degrees = normalRotation(degrees);
    if (!img || degrees == 0) return img;
    const int w = img->w, h = img->h;
    const bool swap = degrees != 180;
    ImagePtr out = Image::make(swap ? h : w, swap ? w : h, img->depth, false);
    if (!out) return nullptr;
    const size_t px = img->pixelBytes();
    // in top-down coordinates (row 0 at the top; Image rows go from the bottom)
    for (int y = 0; y < out->h; ++y) {
        uint8_t* dst = out->row(out->h - 1 - y);
        for (int x = 0; x < out->w; ++x) {
            int sx, sy;
            if (degrees == 90) { sx = y; sy = h - 1 - x; }              // clockwise
            else if (degrees == 270) { sx = w - 1 - y; sy = x; }        // counter-clockwise
            else { sx = w - 1 - x; sy = h - 1 - y; }
            std::memcpy(dst + (size_t)x * px, img->row(h - 1 - sy) + (size_t)sx * px, px);
        }
    }
    return out;
}

void renderOutputSize(const Project& p, const RenderSpec& spec, int* width, int* height)
{
    const int rot = spec.useRotation ? normalRotation(p.settings.outputRotation) : 0;
    int w = p.settings.width, h = p.settings.height;
    if (rot == 90 || rot == 270) std::swap(w, h);
    if (spec.width > 0 && spec.height > 0) {
        w = spec.width;
        h = spec.height;
    }
    if (width) *width = w;
    if (height) *height = h;
}

namespace {

// One block of the file's sound (the sound of one frame's time): the mix in the file's channels.
struct AudioBlock {
    int n = 0;
    std::vector<float> l, r;
    std::vector<std::vector<float>> chans;
};

// One File > Render As run: the project is rendered once, or twice for a two-pass encode.
class RenderAsJob {
public:
    RenderAsJob(const Project& project, FxRuntime& fx, const RenderSpec& spec, double from, double to,
                const std::function<bool(const ExportProgress&)>& progress, std::string* error, MediaPool::Opener opener,
                std::string* description)
        : p_(project), fx_(fx), spec_(spec), from_(from), to_(to), progress_(progress), errorOut_(error), opener_(opener),
          description_(description), meter_(project.settings.sampleRate)
    {
    }

    bool run();

private:
    bool fail(const std::string& m)
    {
        if (errorOut_) *errorOut_ = m;
        return false;
    }
    void prepare();
    void applyHdrAndChapters();
    bool openWriters();
    void planSmart();
    bool finishAll(std::string* e);
    void mixBlock(AudioEngine& audio, int64_t k, AudioBlock& blk);
    bool writeBlock(AudioBlock& blk);
    bool progressAfter(int64_t k);
    ImagePtr finishFrame(ImagePtr img) const;
    double timeOf(int64_t k) const { return from_ + (double)k * es_.fpsDen / es_.fpsNum; }
    int64_t sampleOfFrame(int64_t f) const { return (int64_t)std::floor((double)f * rate_ * es_.fpsDen / es_.fpsNum + 1e-6); }
    void tapBuses(AudioEngine& audio);
    bool renderSequential(MediaPool& pool, FrameOptions& fo);
    bool renderPipelined(MediaPool& pool, FrameOptions& fo, int workers);
    void mixMapped(AudioEngine& audio, int n, int64_t s0, AudioBlock& blk);

    Project p_;                     // the project as the template asks for it to be rendered
    FxRuntime& fx_;
    const RenderSpec& spec_;
    const double from_, to_;
    const std::function<bool(const ExportProgress&)>& progress_;
    std::string* errorOut_;
    MediaPool::Opener opener_;
    std::string* description_;

    EncodeSettings es_;             // the file's settings
    int rot_ = 0;                   // output rotation in degrees
    bool mapped_ = false;           // the sound goes through the template's channel mapping
    bool monoFiles_ = false;        // multiple mono: a file per channel
    int outChannels_ = 2;
    int64_t count_ = 0;             // frames to render
    int rate_ = 48000;              // sample rate of the project's sound
    int64_t firstSample_ = 0;
    int64_t audioEnd_ = 0;

    // Passes: the first of two measures only (no sound), into a file that is thrown away.
    std::string passLog_;
    int passes_ = 1;
    int pass_ = 0;                  // 0 single pass, 1 and 2 for a two-pass encode
    EncodeSettings pe_;             // settings of the current pass
    std::string firstPassFile_;
    std::unique_ptr<MediaWriter> writer_;
    std::vector<std::unique_ptr<MediaWriter>> monos_;
    SmartPlan smart_;               // frames copied from their source (single pass, spec_.smartRender)
    std::string smartNote_;

    LoudnessMeter meter_;
    bool measure_ = false;
    std::vector<LoudnessLogPoint> points_;      // the loudness every second, for the log
    std::string err_;
    std::map<uint64_t, std::pair<std::vector<float>, std::vector<float>>> busOut_;     // the mapping's buses, this block
};

// Fills the file's settings from the project and the template, and the numbers the rendering loop works from.
void RenderAsJob::prepare()
{
    if (spec_.quality == "draft" || spec_.quality == "preview" || spec_.quality == "good" || spec_.quality == "best") p_.settings.renderQuality = spec_.quality;
    if (!spec_.motionBlur.empty()) p_.settings.motionBlur = spec_.motionBlur;
    rot_ = spec_.useRotation ? normalRotation(p_.settings.outputRotation) : 0;
    const bool turned = rot_ == 90 || rot_ == 270;

    es_ = spec_.enc;
    renderOutputSize(p_, spec_, &es_.width, &es_.height);
    es_.fpsNum = spec_.fpsNum > 0 && spec_.fpsDen > 0 ? spec_.fpsNum : p_.settings.fpsNum;
    es_.fpsDen = spec_.fpsNum > 0 && spec_.fpsDen > 0 ? spec_.fpsDen : p_.settings.fpsDen;
    es_.sampleRate = p_.settings.sampleRate;                                    // the sound given is the project's...
    es_.outSampleRate = spec_.sampleRate > 0 ? spec_.sampleRate : 0;            // ...and the file's may differ
    es_.inWidth = turned ? p_.settings.height : p_.settings.width;
    es_.inHeight = turned ? p_.settings.width : p_.settings.height;
    es_.inPixelAspect = p_.settings.pixelAspect > 0 ? (turned ? 1.0 / p_.settings.pixelAspect : p_.settings.pixelAspect) : 1.0;
    es_.alpha = es_.alpha || spec_.transparent;
    es_.chapters.clear();
    es_.spherical = spec_.spherical;
    applyHdrAndChapters();

    // the channels of the sound: mapped, 5.1, or stereo (mapping, multiple mono, 5.1 as six channels)
    mapped_ = !spec_.channelMap.empty() && es_.audio;
    int mappedCount = 0;
    for (const RenderSpec::ChannelSource& c : spec_.channelMap) mappedCount += (c.channel >= 0 || c.mono) ? 1 : 2;
    if (mapped_) es_.audioChannels = std::min(32, std::max(1, mappedCount));
    monoFiles_ = spec_.multipleMono && es_.audio && !es_.video;
    outChannels_ = mapped_ ? es_.audioChannels : (monoFiles_ ? AudioEngine::projectChannels(p_) : (es_.audioChannels == 6 ? 6 : 2));

    const double outFps = (double)es_.fpsNum / es_.fpsDen;
    count_ = std::max<int64_t>(1, (int64_t)std::ceil((to_ - from_) * outFps - 1e-6));
    rate_ = p_.settings.sampleRate;
    firstSample_ = (int64_t)std::llround(from_ * rate_);
    audioEnd_ = es_.video ? INT64_MAX : std::max(firstSample_, (int64_t)std::floor(to_ * rate_ + 1e-6));

    if (spec_.twoPass && es_.video) {
        passLog_ = es_.path + ".renopass";
        passes_ = 2;
    }
    measure_ = !spec_.loudnessLog.empty() && es_.audio;
}

// HDR frames (the view transform or the template's color space is PQ or HLG) are tagged as such, with the Master
// Display.
void RenderAsJob::applyHdrAndChapters()
{
    if (es_.video && linearLightProject(p_.settings)) {
        const HdrCoding hc = hdrCodingOf(spec_.colorSpace.empty() ? p_.settings.viewTransform : spec_.colorSpace);
        es_.hdrTransfer = hc.transfer;
        es_.hdrPrimaries = hc.primaries;
        if (!hc.transfer.empty() && p_.settings.hdrMode != "off") {
            const MasterDisplay& m = p_.settings.masterDisplay;
            es_.mastering = true;
            es_.mdRed[0] = m.redX; es_.mdRed[1] = m.redY;
            es_.mdGreen[0] = m.greenX; es_.mdGreen[1] = m.greenY;
            es_.mdBlue[0] = m.blueX; es_.mdBlue[1] = m.blueY;
            es_.mdWhite[0] = m.whiteX; es_.mdWhite[1] = m.whiteY;
            es_.mdMaxNits = m.maxNits;
            es_.mdMinNits = m.minNits;
            es_.maxCll = m.maxCll;
            es_.maxFall = m.maxFall;
        }
    }
    if (spec_.markers) {
        int n = 0;
        for (const auto& m : p_.markers) {
            if (!m.kind.empty() && m.kind != "cdtrack") continue;       // command markers and CD indexes are not chapters
            if (m.time < from_ - 1e-9 || m.time >= to_) continue;
            ++n;
            es_.chapters.push_back({m.time - from_, m.name.empty() ? strfmt("%s %d", m.region ? "Region" : "Marker", n) : m.name});
        }
        std::sort(es_.chapters.begin(), es_.chapters.end(), [](const std::pair<double, std::string>& a, const std::pair<double, std::string>& b) { return a.first < b.first; });
    }
}

// Creates the writer (or the per-channel writers) for the current pass.
bool RenderAsJob::openWriters()
{
    writer_.reset();
    monos_.clear();
    if (monoFiles_) {
        for (const std::string& path : multipleMonoPaths(es_.path, outChannels_)) {
            EncodeSettings me = pe_;
            me.path = path;
            me.audioChannels = 1;
            std::unique_ptr<MediaWriter> mw = createMediaWriter(me, &err_);
            if (!mw) {
                for (auto& o : monos_) o->finish(nullptr);
                return false;
            }
            monos_.push_back(std::move(mw));
        }
    } else {
        writer_ = createMediaWriter(pe_, &err_);
        if (!writer_) return false;
    }
    return true;
}

bool RenderAsJob::finishAll(std::string* e)
{
    bool ok = true;
    if (writer_) ok = writer_->finish(e) && ok;
    for (auto& o : monos_) ok = o->finish(e) && ok;
    return ok;
}

// Lets the mixer report each bus's output into busOut_ (for a channel mapping that uses buses).
void RenderAsJob::tapBuses(AudioEngine& audio)
{
    if (!mapped_) return;
    audio.busTap = [this](uint64_t id, const float* bl, const float* br, int n) {
        auto& b = busOut_[id];
        b.first.assign(bl, bl + n);
        b.second.assign(br, br + n);
    };
}

// Fills blk.chans from the template's channel mapping: master channels, or buses through the tap.
void RenderAsJob::mixMapped(AudioEngine& audio, int n, int64_t s0, AudioBlock& blk)
{
    std::vector<float>& l = blk.l;
    std::vector<float>& r = blk.r;
    std::vector<std::vector<float>>& chans = blk.chans;
    // the master (its six channels too, when one of them is mapped alone), the buses through the tap
    busOut_.clear();
    bool six = false;
    for (const RenderSpec::ChannelSource& c : spec_.channelMap) six = six || (c.bus < 0 && c.channel >= 0);
    six = six && p_.settings.surround();
    std::vector<std::vector<float>> master6;
    if (six) {
        master6.assign(6, std::vector<float>((size_t)n, 0.f));
        float* m6[6];
        for (int c = 0; c < 6; ++c) m6[c] = master6[(size_t)c].data();
        audio.renderChannels(p_, s0, n, m6, true);
        surroundDownmix(m6, n, l.data(), r.data());
    } else {
        audio.render(p_, s0, n, l.data(), r.data(), true);
    }
    size_t at = 0;
    for (const RenderSpec::ChannelSource& c : spec_.channelMap) {
        const float* sl = nullptr;
        const float* sr = nullptr;
        if (c.bus < 0) {
            if (c.channel >= 0 && six) sl = sr = master6[(size_t)std::min(5, c.channel)].data();
            else if (c.channel >= 0) sl = sr = c.channel == 1 ? r.data() : l.data();
            else { sl = l.data(); sr = r.data(); }
        } else {
            auto it = busOut_.find((uint64_t)c.bus);
            if (it != busOut_.end() && (int)it->second.first.size() >= n) { sl = it->second.first.data(); sr = it->second.second.data(); }
        }
        const bool one = c.channel >= 0 || c.mono;
        for (int j = 0; j < n && at < chans.size(); ++j) {
            if (!sl) break;
            if (one) chans[at][(size_t)j] = c.channel >= 0 ? sl[j] : 0.5f * (sl[j] + sr[j]);
            else {
                chans[at][(size_t)j] = sl[j];
                if (at + 1 < chans.size()) chans[at + 1][(size_t)j] = sr[j];
            }
        }
        at += one ? 1 : 2;
    }
}

// Mixes the sound for frame 'k' into 'blk' (n = 0: there is none) and measures its loudness.
void RenderAsJob::mixBlock(AudioEngine& audio, int64_t k, AudioBlock& blk)
{
    blk.n = 0;
    // The sound ends at the whole sample before the end, as VEGAS's does, not at the last frame's end.
    const int64_t s0 = firstSample_ + sampleOfFrame(k);
    const int64_t s1 = std::min(firstSample_ + (pe_.video && k == count_ - 1 ? (int64_t)std::llround((double)(k + 1) * rate_ * es_.fpsDen / es_.fpsNum) : sampleOfFrame(k + 1)), audioEnd_);
    const int n = (int)std::max<int64_t>(0, s1 - s0);
    if (n <= 0) return;
    blk.n = n;
    std::vector<float>& l = blk.l;
    std::vector<float>& r = blk.r;
    std::vector<std::vector<float>>& chans = blk.chans;
    chans.resize((size_t)std::max(2, outChannels_));
    std::vector<float*> ptrs(chans.size());
    l.resize((size_t)n);
    r.resize((size_t)n);
    for (size_t c = 0; c < chans.size(); ++c) {
        chans[c].assign((size_t)n, 0.f);
        ptrs[c] = chans[c].data();
    }
    if (mapped_) {
        mixMapped(audio, n, s0, blk);
    } else if (outChannels_ == 6) {
        if (p_.settings.surround()) {
            audio.renderChannels(p_, s0, n, ptrs.data(), true);
            surroundDownmix(ptrs.data(), n, l.data(), r.data());
        } else {
            // a stereo project in a 5.1 file: its left and right, the rest silent
            audio.render(p_, s0, n, ptrs[0], ptrs[1], true);
            l.assign(ptrs[0], ptrs[0] + n);
            r.assign(ptrs[1], ptrs[1] + n);
        }
    } else {
        audio.render(p_, s0, n, l.data(), r.data(), true);
        chans[0].assign(l.begin(), l.end());
        chans[1].assign(r.begin(), r.end());
    }
    if (measure_ && pass_ != 1) {
        if (outChannels_ == 6 && !mapped_ && p_.settings.surround()) meter_.processChannels(ptrs.data(), 6, n);
        else meter_.process(l.data(), r.data(), n);
        const double at = (double)(s1 - firstSample_) / rate_;
        if (points_.empty() ? at >= 1.0 : at >= points_.back().time + 1.0) {
            LoudnessLogPoint pt;
            pt.time = from_ + std::floor(at);
            pt.momentary = meter_.momentary();
            pt.shortTerm = meter_.shortTerm();
            points_.push_back(pt);
        }
    }
}

bool RenderAsJob::writeBlock(AudioBlock& blk)
{
    if (blk.n <= 0) return true;
    std::vector<float*> ptrs(blk.chans.size());
    for (size_t c = 0; c < ptrs.size(); ++c) ptrs[c] = blk.chans[c].data();
    bool wrote = true;
    if (monoFiles_) {
        for (size_t c = 0; c < monos_.size() && c < ptrs.size(); ++c) wrote = monos_[c]->writeAudio(ptrs[c], ptrs[c], blk.n) && wrote;
    } else if (mapped_ || outChannels_ == 6) {
        wrote = writer_->writeAudioChannels(ptrs.data(), outChannels_, blk.n);
    } else {
        wrote = writer_->writeAudio(blk.l.data(), blk.r.data(), blk.n);
    }
    if (!wrote) {
        finishAll(&err_);
        err_ = err_.empty() ? "writing the sound failed" : err_;
    }
    return wrote;
}

// Reports progress after frame 'k' was written. Returns false (and cleans up) if the caller stopped the render.
bool RenderAsJob::progressAfter(int64_t k)
{
    if (!progress_) return true;
    ExportProgress ep;
    const int64_t done = (pass_ == 2 ? count_ : 0) + k + 1, all = count_ * passes_;
    ep.frame = done;
    ep.frames = all;
    ep.fraction = (double)done / (double)all;
    if (!progress_(ep)) {
        finishAll(nullptr);
        if (!firstPassFile_.empty()) removeUtf8(firstPassFile_);
        if (!passLog_.empty()) removeUtf8(passLog_);
        err_ = "stopped";
        return false;
    }
    return true;
}

// The frame as the file gets it: 8-bit and rotated. Null if out of memory.
ImagePtr RenderAsJob::finishFrame(ImagePtr img) const
{
    if (!img) return img;
    if (img->depth != PixelDepth::Byte) img = img->converted(PixelDepth::Byte);
    if (img && rot_) img = rotateImage(img, rot_);
    return img;
}

// Smart rendering: plan the copies, keep those this writer can take, tell it.
void RenderAsJob::planSmart()
{
    smart_ = SmartPlan();
    smartNote_.clear();
    if (!spec_.smartRender || pass_ == 1 || !writer_ || !pe_.video || !monos_.empty()) return;
    SmartPlan plan = planSmartRender(p_, from_, count_, es_.fpsNum, es_.fpsDen, es_.width, es_.height, rot_ != 0, spec_.transparent);
    std::string why = plan.why;
    int delay = 0;
    for (const SmartCopy& c : plan.copies) {
        std::string w;
        if (!writer_->smartCompatible(*c.source, &w)) {
            if (why.empty()) why = w;
            continue;
        }
        for (int g = c.firstGop; g < c.firstGop + c.gopCount; ++g) delay = std::max(delay, c.source->gops[(size_t)g].delay);
        smart_.copies.push_back(c);
    }
    if (smart_.copies.empty()) {
        smartNote_ = " (smart render: nothing copied" + (why.empty() ? std::string() : ", " + why) + ")";
        return;
    }
    writer_->beginSmart(delay);
    smartNote_ = strfmt(" (smart render: %lld of %lld frames copied)", (long long)smart_.copiedFrames(), (long long)count_);
}

// One frame at a time, the sound mixed right after each frame.
bool RenderAsJob::renderSequential(MediaPool& pool, FrameOptions& fo)
{
    VideoEngine video(pool, fx_);
    AudioEngine audio(pool, fx_);
    const FinalRenderScope finalScope;               // nested projects' sound waits for its effects too
    AudioBlock blk;
    tapBuses(audio);
    for (int64_t k = 0; k < count_; ++k) {
        if (const SmartCopy* sc = pe_.video ? smart_.at(k) : nullptr) {
            if (k == sc->firstFrame && !writer_->writeVideoCopy(*sc->source, sc->firstGop, sc->gopCount, &err_)) {
                writer_->finish(nullptr);
                return fail(err_.empty() ? "could not copy the frames" : err_);
            }
        } else if (pe_.video) {
            ImagePtr img = video.render(p_, timeOf(k) + 1e-7, fo);
            if (!img) return fail("out of memory");
            img = finishFrame(img);
            if (!img) return fail("out of memory");
            if (!writer_ || !writer_->writeVideo(*img)) {
                if (writer_) writer_->finish(&err_);
                return fail(err_.empty() ? "could not write the frame" : err_);
            }
        }
        if (pe_.audio) {
            mixBlock(audio, k, blk);
            if (!writeBlock(blk)) return fail(err_);
        }
        if (!progressAfter(k)) return fail(err_);
    }
    return true;
}

// Several frames at once, the sound beside them, the encoder fed in order.
bool RenderAsJob::renderPipelined(MediaPool& pool, FrameOptions& fo, int workers)
{
    const bool withAudio = pe_.audio;
    // Every worker but the first has instances of its own of the project's OpenFX effects.
    std::vector<std::unique_ptr<FxRuntime>> extra;
    if (workers > 1) {
        Project ps = p_;
        fx_.captureStates(ps);
        std::vector<uint64_t> wanted;
        p_.forEachFx([&](const FxSlot& s) { if (s.kind == "ofx" && fx_.video(s.id)) wanted.push_back(s.id); });
        for (int i = 1; i < workers; ++i) {
            std::unique_ptr<FxRuntime> rt(new FxRuntime());
            size_t next = 0;
            while (!rt->prepareSome(ps, next, 1.0e9, true, false)) {}
            bool same = true;
            for (uint64_t id : wanted) same = same && rt->video(id) != nullptr;
            if (!same) break;                   // an effect that cannot be made twice
            extra.push_back(std::move(rt));
        }
        workers = 1 + (int)extra.size();
    }
    // how many frames may wait to be written at most, and how many a worker takes at a go
    const int64_t frameBytes = std::max<int64_t>(1, (int64_t)es_.inWidth * es_.inHeight * 4);
    const int64_t window = std::max<int64_t>(2 * workers, std::min<int64_t>(16 * workers, (512ll << 20) / frameBytes));
    const int64_t run = std::max<int64_t>(1, std::min<int64_t>(8, window / (2 * workers)));
    const int64_t audioAhead = 64 + window;

    std::mutex m;
    std::condition_variable workerCv, writerCv;
    std::map<int64_t, ImagePtr> frames;         // rendered, waiting to be written (m)
    std::map<int64_t, AudioBlock> sounds;
    int64_t written = 0;                        // the frames written so far (m)
    std::string failure;                        // why a thread gave up (m)
    std::atomic<bool> stop(false);              // told to all: stop now (also the effects' abort())
    std::atomic<int64_t> nextRun(0);
    auto giveUp = [&](const std::string& why) {
        {
            std::lock_guard<std::mutex> lk(m);
            if (failure.empty()) failure = why;
            stop.store(true);
        }
        workerCv.notify_all();
        writerCv.notify_all();
    };
    fo.cancel = &stop;
    std::vector<std::thread> threads;
    for (int i = 0; i < workers; ++i) {
        FxRuntime& runtime = i == 0 ? fx_ : *extra[(size_t)i - 1];
        threads.emplace_back([&] {
            lowerThreadPriority();
            try {
                VideoEngine video(pool, runtime);
                for (;;) {
                    const int64_t first = nextRun.fetch_add(1) * run;
                    if (first >= count_) return;
                    for (int64_t k = first; k < std::min(count_, first + run); ++k) {
                        {
                            std::unique_lock<std::mutex> lk(m);
                            workerCv.wait(lk, [&] { return stop.load() || k < written + window; });
                            if (stop.load()) return;
                        }
                        ImagePtr img;
                        if (!smart_.at(k)) {
                            img = video.render(p_, timeOf(k) + 1e-7, fo);
                            if (stop.load()) return;
                            img = finishFrame(img);
                            if (!img) return giveUp("out of memory");
                        }
                        {
                            std::lock_guard<std::mutex> lk(m);
                            frames[k] = std::move(img);
                        }
                        writerCv.notify_all();
                    }
                }
            } catch (const std::exception& e) {
                giveUp(e.what());
            } catch (...) {
                giveUp("an unexpected error");
            }
        });
    }
    if (withAudio) {
        threads.emplace_back([&] {
            raiseThreadPriority();
            try {
                const FinalRenderScope scope;
                AudioEngine audio(pool, fx_);
                tapBuses(audio);
                for (int64_t k = 0; k < count_; ++k) {
                    {
                        std::unique_lock<std::mutex> lk(m);
                        workerCv.wait(lk, [&] { return stop.load() || k < written + audioAhead; });
                        if (stop.load()) return;
                    }
                    AudioBlock blk;
                    mixBlock(audio, k, blk);
                    {
                        std::lock_guard<std::mutex> lk(m);
                        sounds[k] = std::move(blk);
                    }
                    writerCv.notify_all();
                }
            } catch (const std::exception& e) {
                giveUp(e.what());
            } catch (...) {
                giveUp("an unexpected error");
            }
        });
    }
    // This thread is the encoder's: the frames and the sound come to it in order.
    bool good = true;
    for (int64_t k = 0; k < count_ && good; ++k) {
        ImagePtr img;
        AudioBlock blk;
        {
            std::unique_lock<std::mutex> lk(m);
            writerCv.wait(lk, [&] { return stop.load() || (frames.count(k) && (!withAudio || sounds.count(k))); });
            if (stop.load()) {
                good = false;
                break;
            }
            img = std::move(frames[k]);
            frames.erase(k);
            if (withAudio) {
                blk = std::move(sounds[k]);
                sounds.erase(k);
            }
            written = k + 1;
        }
        workerCv.notify_all();
        const SmartCopy* sc = smart_.at(k);
        if (sc && !img) {
            if (k == sc->firstFrame && !writer_->writeVideoCopy(*sc->source, sc->firstGop, sc->gopCount, &err_)) {
                writer_->finish(nullptr);
                if (err_.empty()) err_ = "could not copy the frames";
                good = false;
            } else if (withAudio && !writeBlock(blk)) {
                good = false;
            } else if (!progressAfter(k)) {
                good = false;
            }
        } else if (!writer_ || !writer_->writeVideo(*img)) {
            if (writer_) writer_->finish(&err_);
            if (err_.empty()) err_ = "could not write the frame";
            good = false;
        } else if (withAudio && !writeBlock(blk)) {
            good = false;
        } else if (!progressAfter(k)) {
            good = false;
        }
        if (!good) giveUp(err_);
    }
    {
        std::lock_guard<std::mutex> lk(m);
        stop.store(true);
    }
    workerCv.notify_all();
    for (std::thread& t : threads) t.join();
    fo.cancel = nullptr;
    if (!good && err_.empty()) {
        // a thread gave up (the encoder's own failures have their message in err_)
        finishAll(nullptr);
        err_ = failure.empty() ? "stopped" : failure;
    }
    if (!good) return fail(err_);
    return true;
}

bool RenderAsJob::run()
{
    if (!(to_ > from_)) return fail("nothing to render");
    prepare();

    for (pass_ = passes_ == 2 ? 1 : 0;;) {
        pe_ = es_;
        firstPassFile_.clear();
        if (pass_ == 1) {
            pe_.pass = 1;
            pe_.passLog = passLog_;
            pe_.audio = false;
            const size_t dot = es_.path.find_last_of('.');
            firstPassFile_ = es_.path + ".pass1" + (dot == std::string::npos ? std::string() : es_.path.substr(dot));
            pe_.path = firstPassFile_;
        } else if (pass_ == 2) {
            pe_.pass = 2;
            pe_.passLog = passLog_;
        }
        if (!openWriters()) return fail(err_);
        if (pass_ == 1 && writer_ && !encoderTakesTwoPasses(writer_->describe())) {
            // this encoder can't do two passes: one pass it is
            writer_->finish(nullptr);
            writer_.reset();
            removeUtf8(firstPassFile_);
            passes_ = 1;
            pass_ = 0;
            continue;
        }
        planSmart();
        if (description_ && pass_ != 1)
            *description_ = writer_ ? writer_->describe() + (pass_ == 2 ? " (two passes)" : "") + smartNote_ : (monos_.empty() ? std::string() : monos_[0]->describe() + strfmt(" (%d mono files)", (int)monos_.size()));

        MediaPool pool;
        if (opener_) pool.setOpener(opener_);
        FrameOptions fo;
        fo.interactive = false;
        fo.transparent = spec_.transparent;
        fo.colorSpace = spec_.colorSpace;

        // Several frames at once when the machine has more than one processor, there is more than one frame to
        // render, and no nested project is in the way.
        int wanted = processorCount();
        if (const char* e = std::getenv("RENO_RENDER_WORKERS")) wanted = std::max(1, std::min(64, std::atoi(e)));
        int workers = (int)std::max<int64_t>(1, std::min<int64_t>(wanted, count_));
        bool pipeline = pe_.video && workers > 1;
        if (pipeline)
            for (const MediaItem& m : p_.media)
                if (!m.offline && isProjectFile(m.path)) pipeline = false;
        if (pipeline) {
            // OpenFX effects that are "unsafe" (render thread safety) are never entered by two threads: one frame at a time
            bool safe = true;
            p_.forEachFx([&](const FxSlot& s) {
                if (!safe || s.bypass) return;
                if (ofx::Instance* inst = fx_.video(s.id))
                    if (!inst->plugin || inst->plugin->safety < 1) safe = false;
            });
            if (!safe) workers = 1;
        }
        if (pipeline) {
            if (!renderPipelined(pool, fo, workers)) return false;
        } else {
            if (!renderSequential(pool, fo)) return false;
        }
        if (!finishAll(&err_)) return fail(err_);
        if (!firstPassFile_.empty()) removeUtf8(firstPassFile_);
        if (pass_ == 1) {
            pass_ = 2;
            continue;
        }
        break;
    }
    if (!passLog_.empty()) {
        // x264 and x265 keep their statistics in this file and a few beside it
        for (const char* extra : {"", ".mbtree", ".temp", ".mbtree.temp", ".cutree", ".cutree.temp"}) removeUtf8(passLog_ + extra);
    }

    if (measure_) {
        FILE* f = openUtf8(spec_.loudnessLog, "wb");
        if (f) {
            const std::string text = loudnessLogText(es_.path, description_ ? *description_ : std::string(), spec_.loudnessAtsc, from_, to_, meter_, points_);
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
        }
    }
    return true;
}

}  // namespace

bool renderProjectAs(const Project& project, FxRuntime& fx, const RenderSpec& spec, double from, double to,
                     const std::function<bool(const ExportProgress&)>& progress, std::string* error, MediaPool::Opener opener,
                     std::string* description)
{
    RenderAsJob job(project, fx, spec, from, to, progress, error, opener, description);
    return job.run();
}

std::vector<std::string> multipleMonoPaths(const std::string& path, int channels)
{
    static const char* const names6[6] = {"Left", "Right", "Center", "LFE", "Left Surround", "Right Surround"};
    std::string stem = path, ext;
    const size_t dot = pathExtensionStart(path);
    if (dot != std::string::npos) {
        stem = path.substr(0, dot);
        ext = path.substr(dot);
    }
    std::vector<std::string> out;
    for (int c = 0; c < (channels == 6 ? 6 : 2); ++c) out.push_back(stem + " " + names6[c] + ext);
    return out;
}

}  // namespace reno
