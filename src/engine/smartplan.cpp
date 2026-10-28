// Smart rendering's plan: see smartplan.h.
#include "smartplan.h"
#include "colormgmt.h"
#include "engine.h"
#include <algorithm>
#include <cmath>
#include <map>

namespace reno {

const SmartCopy* SmartPlan::at(int64_t k) const
{
    auto it = std::upper_bound(copies.begin(), copies.end(), k, [](int64_t v, const SmartCopy& c) { return v < c.firstFrame; });
    if (it == copies.begin()) return nullptr;
    --it;
    return k < it->firstFrame + it->frames ? &*it : nullptr;
}

namespace {

// As the video engine decides it: whether the frame that starts at time t shows event e, in 100 ns ticks.
bool coversFrame(const Event& e, double t)
{
    const int64_t f = (int64_t)std::ceil(t * 1.0e7 - 1e-3);
    const int64_t s = (int64_t)std::llround(e.start * 1.0e7), len = (int64_t)std::llround(e.length * 1.0e7);
    return s <= f && f < s + len;
}

bool nearInt(double v, double tol) { return std::fabs(v - std::round(v)) <= tol; }

// Why the project as a whole cannot have frames copied from it ("" if it can).
std::string projectBlocker(const Project& p, int outW, int outH, bool rotated, bool transparent)
{
    const ProjectSettings& s = p.settings;
    if (rotated) return "the output is rotated";
    if (transparent) return "the render keeps transparency";
    if (outW != s.width || outH != s.height) return "the file's picture size is not the project's";
    if (s.pixelFormat != "8bit") return "the project is 32-bit floating point";
    if (linearLightProject(s)) return "the project is color managed";
    if (s.stereo()) return "the project outputs stereoscopic 3D";
    if (s.fieldOrder == "upper" || s.fieldOrder == "lower") return "the project is interlaced (each field is rendered at its own moment)";
    if (s.deinterlace != "none") return "the project deinterlaces";
    if (!p.videoOut.empty()) return "the video output has effects";
    if (!p.videoEnvelopes.empty()) return "the video output has envelopes (fade to color, mute, motion blur...)";
    return std::string();
}

// Why a track's pictures cannot be copied as they are ("" if they can).
std::string trackBlocker(const Track& t)
{
    if (t.adjustment) return "an adjustment track is in the project";
    if (t.level != 1.0 || t.autoLevel != 1.0 || t.autoFade != 0.0 || t.autoMute) return "a video track's level is not full";
    if (t.composite != "srcalpha") return "a video track composites in another mode";
    if (!t.fx.empty()) return "a video track has effects";
    if (!t.motion.empty() || !t.parentMotion.empty()) return "a video track has track motion";
    if (!t.shadow2d.empty() || !t.glow2d.empty()) return "a video track has a 2D shadow or glow";
    if (!t.envelopes.empty()) return "a video track has envelopes";
    if (t.compositeLevel != 0) return "a video track is in a compositing group";
    return std::string();
}

// Why an event's pictures cannot be copied as they are ("" if they can). Its media is checked by the caller.
std::string eventBlocker(const Event& e)
{
    if (e.adjustment) return "an adjustment event is in the project";
    if (e.rate != 1.0 || e.reverse) return "an event is played at another speed or backwards";
    if (e.fadeIn != 0 || e.fadeOut != 0) return "an event has a fade";
    if (e.gain != 1.0) return "an event's opacity is not full";
    if (!e.fx.empty()) return "an event has effects";
    if (e.transition.kind != "") return "an event has a transition";
    if (!e.panCrop.empty()) return "an event has pan/crop";
    if (!e.mask.empty() || e.maskOn) return "an event has a mask";
    if (!e.envelopes.empty()) return "an event has envelopes";
    if (e.undersample != 1.0) return "an event is undersampled";
    if (e.reduceFlicker) return "an event reduces interlace flicker";
    if (e.resample != "project") return "an event resamples frames";
    return std::string();
}

// Why a media item's pictures cannot be copied as they are ("" if they can).
std::string mediaBlocker(const MediaItem& m, int projW, int projH)
{
    if (m.generated || m.offline || m.path.empty() || !m.info.hasVideo || m.info.isStill) return "an event's media is not a video file";
    if (isProjectFile(m.path)) return "an event's media is a nested project";
    if (m.sequenceFps > 0) return "an event's media is an image sequence";
    if (m.subclipLength > 0 || m.subclipStart > 0) return "an event's media is a subclip";
    if (!m.redEye.empty() || !m.alphaMode.empty() || !m.colorSpace.empty() || (m.stereoMode != "off" && !m.stereoMode.empty()))
        return "an event's media has properties (red eye, alpha, color space, stereoscopic 3D) that change its pictures";
    if (std::fabs(m.pixelFactor() - 1.0) > 0.0005 || m.rotationOverride != 0 || m.info.rotation != 0 || !m.fieldOrderOverride.empty())
        return "an event's media has its pixel shape, rotation or field order changed";
    if (m.info.width != projW || m.info.height != projH) return "an event's media is not the project's size";
    if (m.info.hasAlpha) return "an event's media has transparency";
    return std::string();
}

}  // namespace

SmartPlan planSmartRender(const Project& p, double from, int64_t count, int fpsNum, int fpsDen, int outW, int outH, bool rotated,
                          bool transparent, const SmartAnalyzer& analyze)
{
    SmartPlan plan;
    auto stop = [&](const std::string& why) {
        plan.copies.clear();
        plan.why = why;
        return plan;
    };
    const ProjectSettings& s = p.settings;
    if (s.fpsNum * (int64_t)fpsDen != (int64_t)fpsNum * s.fpsDen) return stop("the file's frame rate is not the project's");
    if (std::string b = projectBlocker(p, outW, outH, rotated, transparent); !b.empty()) return stop(b);
    if (!nearInt(from * s.fps(), 1e-6)) return stop("the render does not start on a frame");
    if (count < 1) return stop("nothing to render");

    // The events that can show: those of video tracks that are not muted. A solo track hides the others, which is not
    // worked out here.
    struct Cand {
        const Track* track;
        const Event* event;
    };
    std::vector<Cand> cands;
    for (const Track& t : p.tracks) {
        if (t.type != TrackType::Video) continue;
        if (t.solo) return stop("a video track is soloed");
        if (t.mute) continue;
        for (const Event& e : t.events) {
            if (e.mute) continue;
            cands.push_back({&t, &e});
        }
    }
    if (cands.empty()) return stop("no video event is in the render");

    std::map<std::string, std::shared_ptr<const SmartSource>> sources;
    std::string firstWhy;
    auto note = [&](const std::string& w) { if (firstWhy.empty()) firstWhy = w; };

    // Walk the frames; a run is a stretch of frames showing just one event, each the next of its source's frames.
    const double fps = s.fps();
    const int64_t firstProjectFrame = (int64_t)std::llround(from * fps);
    int64_t k = 0;
    while (k < count) {
        const double t = p.frameTime(firstProjectFrame + k);
        // the frame as the engine times it: seconds + a hair, then onto the frame grid
        const double tq = p.frameTime(p.frameAt(from + (double)k * fpsDen / fpsNum + 1e-7));
        (void)t;
        const Cand* only = nullptr;
        int covering = 0;
        for (const Cand& c : cands)
            if (coversFrame(*c.event, tq)) { ++covering; only = &c; }
        if (covering != 1) { ++k; continue; }
        const Event& e = *only->event;
        const MediaItem* m = p.findMedia(e.mediaId);
        std::string why = trackBlocker(*only->track);
        if (why.empty()) why = eventBlocker(e);
        if (why.empty()) why = m ? mediaBlocker(*m, s.width, s.height) : std::string("an event has no media");
        if (!why.empty()) { note(why); ++k; continue; }
        // how far the run goes: the same single event covers the next frames
        int64_t end = k + 1;
        while (end < count) {
            const double te = p.frameTime(p.frameAt(from + (double)end * fpsDen / fpsNum + 1e-7));
            int n = 0;
            for (const Cand& c : cands) if (coversFrame(*c.event, te)) ++n;
            if (n != 1 || !coversFrame(e, te)) break;
            ++end;
        }
        // the source of the run
        auto it = sources.find(m->path);
        if (it == sources.end()) {
            std::shared_ptr<const SmartSource> src;
            std::string w;
            if (!analyze(m->path, &src, &w)) { note(pathFileName(m->path) + ": " + w); sources[m->path] = nullptr; k = end; continue; }
            it = sources.emplace(m->path, src).first;
        }
        const std::shared_ptr<const SmartSource>& src = it->second;
        if (!src) { k = end; continue; }
        if ((int64_t)src->fpsNum * s.fpsDen != (int64_t)s.fpsNum * src->fpsDen) { note(pathFileName(m->path) + ": its frame rate is not the project's"); k = end; continue; }
        // the source's frame shown by the run's first frame: it must be exactly one, and the run must keep inside the media
        const double mt = eventMediaTime(e, m, tq);
        const double n0f = mt * (double)src->fpsNum / src->fpsDen;
        if (!nearInt(n0f, 0.02)) { note(pathFileName(m->path) + ": the event does not sit on the file's frames"); k = end; continue; }
        const int64_t n0 = (int64_t)std::llround(n0f);
        const int64_t runFrames = end - k;
        if (n0 < 0 || n0 + runFrames > src->frames) { note(pathFileName(m->path) + ": the event runs past the file's frames"); k = end; continue; }
        // whole groups inside [n0, n0 + runFrames)
        SmartCopy cur;
        for (size_t g = 0; g < src->gops.size(); ++g) {
            const SmartGop& gop = src->gops[g];
            if (gop.startFrame < n0 || gop.startFrame + gop.frames > n0 + runFrames) continue;
            const int64_t at = k + (gop.startFrame - n0);
            if (cur.frames > 0 && cur.firstFrame + cur.frames == at && (size_t)(cur.firstGop + cur.gopCount) == g) {
                cur.frames += gop.frames;
                ++cur.gopCount;
            } else {
                if (cur.frames > 0) plan.copies.push_back(cur);
                cur = SmartCopy();
                cur.firstFrame = at;
                cur.frames = gop.frames;
                cur.source = src;
                cur.firstGop = (int)g;
                cur.gopCount = 1;
            }
        }
        if (cur.frames > 0) plan.copies.push_back(cur);
        else note(pathFileName(m->path) + ": no whole group of pictures lies inside the event");
        k = end;
    }
    std::sort(plan.copies.begin(), plan.copies.end(), [](const SmartCopy& a, const SmartCopy& b) { return a.firstFrame < b.firstFrame; });
    if (plan.copies.empty()) plan.why = firstWhy.empty() ? "no frame shows just one untouched video event" : firstWhy;
    return plan;
}

}  // namespace reno
