// Automatic preview proxies (Preferences > Video, "Automatically create preview proxies for long-GOP video").
// Long-GOP files are slow to seek in, so edits with many tiny events stall the preview. A proxy (media/proxy.cpp: at most 960 x 540,
// a key frame every quarter second) makes those reads cheap; the preview uses it below Best quality.
// plan() decides which files need one; Worker makes them on a low-priority thread, one at a time, waiting during playback.
// Proxies go where media.h's videoProxyPath says; one made beside the media by VEGAS or the user counts as made.
// Nothing runs unless the application turns the worker on (prefsapply.h), so tests and reno_cli never make proxies. Header-only.
#pragma once
#include "../engine/engine.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace reno {
namespace app {
namespace autoproxy {

// One file that should get a proxy.
struct Candidate {
    std::string path;
    double score = 0;               // how much the project jumps about in it (see plan)
    int events = 0;                 // video events that use it (nested projects' too)
    double shortest = 0;            // shortest of them, seconds
    VideoProxyAssessment why;       // what the file's index says (media.h)
};

// Reads a nested project's file (the application passes the .veg loader).
using ProjectLoader = std::function<bool(const std::string& path, Project& out)>;
// Looks at a file (default: media.h's assessVideoProxy). Tests give their own.
using Assessor = std::function<VideoProxyAssessment(const std::string& path, const MediaInfo& info)>;

// What plan() looks for.
struct PlanOptions {
    bool longGop = true;            // long-GOP files that would gain from a proxy (Preferences > Video)
    bool fourK = false;             // media larger than 4K whatever its codec
    Assessor assess;                // default: media.h's assessVideoProxy
    std::function<bool(const std::string&)> skip;       // files to leave out (ones that failed)
    // Files asked for by name (new media): path -> true to make it whatever its index says.
    std::map<std::string, bool> extra;
};

// Every video file the project uses (including nested projects) that has no proxy yet and would gain from one, in the order to
// make them. A file's score adds up its events: each is a seek plus a decode from the previous key frame, so an event counts
// 1 + 1 / (length in seconds, at least 0.1). Files asked for by name come first when forced, last when no event uses them.
inline std::vector<Candidate> plan(const Project& root, const ProjectLoader& load, const PlanOptions& opt = PlanOptions())
{
    std::map<std::string, Candidate> found;
    std::map<std::string, MediaInfo> infos;
    std::set<std::string> seenProjects;
    std::function<void(const Project&, int)> walk = [&](const Project& p, int depth) {
        std::map<uint64_t, const MediaItem*> byId;
        std::vector<std::string> nested;       // projects a video event of this one plays
        for (const MediaItem& m : p.media) {
            byId[m.id] = &m;
            if (!m.generated && m.info.hasVideo && !m.info.isStill && !m.path.empty() && !isProjectFile(m.path) && !infos.count(m.path)) infos[m.path] = m.info;
        }
        for (const Track& t : p.tracks) {
            if (t.type != TrackType::Video) continue;
            for (const Event& e : t.events) {
                auto it = byId.find(e.mediaId);
                if (it == byId.end()) continue;
                const MediaItem& m = *it->second;
                if (!m.generated && !m.offline && !m.path.empty() && isProjectFile(m.path)) {
                    if (std::find(nested.begin(), nested.end(), m.path) == nested.end()) nested.push_back(m.path);
                    continue;
                }
                if (m.generated || m.offline || m.path.empty() || !m.info.hasVideo || m.info.isStill) continue;
                Candidate& c = found[m.path];
                c.path = m.path;
                ++c.events;
                const double len = std::max(0.1, e.length);
                c.score += 1.0 + 1.0 / len;
                c.shortest = c.events == 1 ? e.length : std::min(c.shortest, e.length);
            }
        }
        // nested projects its events play: what they use counts too
        if (depth >= 8 || !load) return;
        for (const std::string& path : nested) {
            if (!seenProjects.insert(path).second || !fileExists(path)) continue;
            Project inner;
            if (load(path, inner)) walk(inner, depth + 1);
        }
    };
    walk(root, 0);
    // (media asked for by name that no event uses: tiny score, so after the ones the project plays)
    for (const auto& x : opt.extra) {
        auto ii = infos.find(x.first);
        if (ii == infos.end() || found.count(x.first)) continue;
        Candidate c;
        c.path = x.first;
        found[x.first] = c;
    }
    std::vector<Candidate> out;
    for (auto& kv : found) {
        Candidate c = kv.second;
        if (opt.skip && opt.skip(c.path)) continue;
        if (hasVideoProxy(c.path)) continue;
        auto ii = infos.find(c.path);
        MediaInfo info;
        if (ii != infos.end()) info = ii->second;
        if (ii == infos.end() || info.videoCodec.empty() || !(info.duration > 0) || info.width <= 0) {
            // a file the project does not describe fully (a nested project's own files) is described by opening it
            std::string err;
            std::unique_ptr<MediaReader> r = openMedia(c.path, &err);
            if (!r) continue;
            info = r->info();
        }
        if (!info.hasVideo || info.isStill || !(info.duration > 0)) continue;
        const auto ex = opt.extra.find(c.path);
        const bool forced = ex != opt.extra.end() && ex->second;
        if (forced) {
            c.why.worth = true;
            c.why.reason = "asked for";
            c.score += 1e6;                         // before every other
        } else if (opt.fourK && (info.width > 3840 || info.height > 2160)) {
            c.why.worth = true;
            c.why.reason = "greater than 4K";
        } else if (opt.longGop) {
            c.why = opt.assess ? opt.assess(c.path, info) : assessVideoProxy(c.path, info);
        }
        if (!c.why.worth) continue;
        out.push_back(c);
    }
    std::sort(out.begin(), out.end(), [](const Candidate& a, const Candidate& b) { return a.score != b.score ? a.score > b.score : a.path < b.path; });
    return out;
}

// What the status bar shows.
struct Status {
    bool enabled = false;
    bool working = false;           // a proxy is being made (or waited for)
    bool paused = false;            // ... and playback has it waiting
    int done = 0;                   // finished in this batch
    int total = 0;                  // done + the one in hand + those queued
    std::string current;            // file name being made
    double fraction = 0;            // of the current one
};

class Worker {
public:
    static Worker& get()
    {
        static Worker w;
        return w;
    }

    // Whether Preferences control the worker. The main window sets this on start; tests and reno_cli call setModes() themselves.
    void setPreferencesControl(bool on) { prefsControl_ = on; }
    bool preferencesControl() const { return prefsControl_.load(); }
    // What the worker looks for: long-GOP files, media larger than 4K. Either turns it on; neither turns it off.
    void setModes(bool longGop, bool fourK, bool hinted)
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            longGop_ = longGop;
            fourK_ = fourK;
        }
        setEnabled(longGop || fourK || hinted);
        std::lock_guard<std::mutex> lk(m_);
        if (enabled_ && project_) {
            haveProject_ = true;
            changedAt_ = std::chrono::steady_clock::now();
            start();
            cv_.notify_all();
        }
    }
    // New media (any thread): made soon, whether or not the project uses it yet. 'force': whatever its index says.
    void hint(const std::string& path, bool force)
    {
        std::lock_guard<std::mutex> lk(m_);
        bool& f = hinted_[path];
        f = f || force;
    }

    // Turns the worker on or off. Off drops the queue and stops a proxy being made (nothing left behind).
    void setEnabled(bool on)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (enabled_ == on) return;
        enabled_ = on;
        enabledFast_ = on;
        if (!on) {
            queue_.clear();
            planned_.clear();
            cancel_ = true;
            haveProject_ = false;
        } else if (project_) {
            haveProject_ = true;
            changedAt_ = std::chrono::steady_clock::now();
            start();
        }
        cv_.notify_all();
    }
    bool enabled() const { return enabledFast_.load(); }

    // How nested projects are read. Set once by the application.
    void setLoader(ProjectLoader l)
    {
        std::lock_guard<std::mutex> lk(m_);
        loader_ = std::move(l);
    }
    // For tests: what looks at a file and what makes the proxy (default: media.h's).
    void setAssessor(Assessor a)
    {
        std::lock_guard<std::mutex> lk(m_);
        assessor_ = std::move(a);
        assessed_.clear();
    }
    typedef std::function<bool(const std::string& path, const std::function<bool(double)>& progress, std::string* error)> Maker;
    void setMaker(Maker mk)
    {
        std::lock_guard<std::mutex> lk(m_);
        maker_ = std::move(mk);
    }
    // Delay after a change before the project is looked at (ms), so a burst of edits is looked at once.
    void setSettleMs(int ms) { settleMs_ = std::max(0, ms); }
    // Called on the worker's thread when a proxy was made or failed.
    void setDoneHook(std::function<void(const std::string& path, bool made, const std::string& error)> h)
    {
        std::lock_guard<std::mutex> lk(m_);
        doneHook_ = std::move(h);
    }

    // The open project changed (any thread; cheap).
    void projectChanged(const std::shared_ptr<const Project>& p)
    {
        if (!enabledFast_.load()) {
            std::lock_guard<std::mutex> lk(m_);
            project_ = p;           // kept: turning the worker on looks at it
            return;
        }
        std::lock_guard<std::mutex> lk(m_);
        project_ = p;
        haveProject_ = true;
        changedAt_ = std::chrono::steady_clock::now();
        start();
        cv_.notify_all();
    }

    Status status()
    {
        std::lock_guard<std::mutex> lk(m_);
        Status s;
        s.enabled = enabled_;
        s.working = !current_.empty();
        s.paused = s.working && pausedNow_;
        s.done = doneCount_;
        s.total = doneCount_ + (int)queue_.size() + (current_.empty() ? 0 : 1);
        s.current = pathFileName(current_);
        s.fraction = fraction_;
        return s;
    }
    // Files waiting, in order (not the one being made).
    std::vector<std::string> queued()
    {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> out;
        for (const Candidate& c : queue_) out.push_back(c.path);
        return out;
    }
    // Every file the last look found, including ones made since.
    std::vector<std::string> planned()
    {
        std::lock_guard<std::mutex> lk(m_);
        return planned_;
    }
    // Waits until the project has been looked at and nothing is queued or being made, or 'seconds' pass (tests).
    bool waitIdle(double seconds)
    {
        std::unique_lock<std::mutex> lk(m_);
        return idleCv_.wait_for(lk, std::chrono::milliseconds((int64_t)(seconds * 1000)), [this] { return idleNow(); });
    }
    // Forgets failed files so they are tried again (new project, or Create Video Proxy by hand).
    void forgetFailures()
    {
        std::lock_guard<std::mutex> lk(m_);
        failed_.clear();
        assessed_.clear();
    }
    // Program closing: turns the worker off and waits a few seconds for the proxy under way to stop, so its part-written
    // file is removed.
    // Preferences are untouched; the next main window turns it on again.
    void stopAndWait(double seconds = 3.0)
    {
        setEnabled(false);
        std::unique_lock<std::mutex> lk(m_);
        idleCv_.wait_for(lk, std::chrono::milliseconds((int64_t)(seconds * 1000)), [this] { return current_.empty(); });
    }
    ~Worker()
    {
        {
            std::lock_guard<std::mutex> lk(m_);
            quit_ = true;
            cancel_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    bool idleNow() const { return quit_ || !enabled_ || (!haveProject_ && queue_.empty() && current_.empty()); }

    void start()
    {
        if (!thread_.joinable()) thread_ = std::thread([this] { run(); });
    }

    // (lock held, worker thread) Looks at the project again if it changed and settled: the queue becomes what plan() says, and a proxy
    // under way that is no longer needed is stopped. Unlocked while it works.
    void lookAtProject(std::unique_lock<std::mutex>& lk)
    {
        if (!haveProject_ || !enabled_ || quit_) return;
        std::shared_ptr<const Project> p = project_;
        ProjectLoader load = loader_;
        PlanOptions po;
        po.longGop = longGop_;
        po.fourK = fourK_;
        po.extra = hinted_;
        Assessor inner = assessor_;
        // a file is looked at once: its index is read from disk
        po.assess = [this, inner](const std::string& path, const MediaInfo& info) {
            {
                std::lock_guard<std::mutex> g(m_);
                auto it = assessed_.find(path);
                if (it != assessed_.end()) return it->second;
            }
            const VideoProxyAssessment a = inner ? inner(path, info) : assessVideoProxy(path, info);
            std::lock_guard<std::mutex> g(m_);
            assessed_[path] = a;
            return a;
        };
        haveProject_ = false;
        const std::set<std::string> failed = failed_;
        po.skip = [&](const std::string& path) { return failed.count(path) != 0; };
        lk.unlock();
        std::vector<Candidate> found;
        if (p) found = plan(*p, load, po);
        lk.lock();
        if (quit_) return;
        if (!enabled_ || haveProject_) {
            idleCv_.notify_all();
            return;                 // turned off, or changed again meanwhile: look again
        }
        if (queue_.empty() && current_.empty()) doneCount_ = 0;        // new batch counts from 0
        planned_.clear();
        for (const Candidate& c : found) planned_.push_back(c.path);
        // the one being made is not queued again; if no longer needed, it is stopped
        queue_.clear();
        bool needed = false;
        for (const Candidate& c : found) {
            if (c.path == current_) needed = true;
            else queue_.push_back(c);
        }
        if (!current_.empty()) cancel_ = !needed;
        idleCv_.notify_all();
    }

    void run()
    {
        lowerThreadPriority();
        std::unique_lock<std::mutex> lk(m_);
        for (;;) {
            cv_.wait(lk, [this] { return quit_ || (enabled_ && (haveProject_ || !queue_.empty())); });
            if (quit_) return;
            if (haveProject_) {
                // wait until no change has come for a moment
                const auto settle = std::chrono::milliseconds(settleMs_.load());
                if (std::chrono::steady_clock::now() - changedAt_ < settle) {
                    cv_.wait_until(lk, changedAt_ + settle);
                    continue;
                }
                lookAtProject(lk);
                if (quit_) return;
                continue;
            }
            if (queue_.empty()) continue;
            const Candidate next = queue_.front();
            queue_.erase(queue_.begin());
            current_ = next.path;
            fraction_ = 0;
            cancel_ = false;
            Maker maker = maker_;
            auto doneHook = doneHook_;
            lk.unlock();
            std::string err;
            bool made = false;
            if (hasVideoProxy(next.path)) made = true;
            else {
                auto progress = [this](double f) {
                    std::unique_lock<std::mutex> g(m_);
                    fraction_ = f;
                    // the project may have changed: what it needs now decides whether this one goes on
                    if (haveProject_ && std::chrono::steady_clock::now() - changedAt_ >= std::chrono::milliseconds(settleMs_.load())) lookAtProject(g);
                    // playback comes first: wait while it runs
                    while (!quit_ && !cancel_ && enabled_ && previewPlaying()) {
                        pausedNow_ = true;
                        cv_.wait_for(g, std::chrono::milliseconds(200));
                    }
                    pausedNow_ = false;
                    return !quit_ && !cancel_ && enabled_;
                };
                {
                    std::unique_lock<std::mutex> g(m_);
                    while (!quit_ && !cancel_ && enabled_ && previewPlaying()) {
                        pausedNow_ = true;
                        cv_.wait_for(g, std::chrono::milliseconds(200));
                    }
                    pausedNow_ = false;
                }
                const bool go = [&] {
                    std::lock_guard<std::mutex> g(m_);
                    return !quit_ && !cancel_ && enabled_;
                }();
                if (go) {
                    if (maker) made = maker(next.path, progress, &err);
                    else {
                        VideoProxyOptions opt;
                        opt.encoderThreads = 1;     // the encoder would take every processor; one keeps up with the decoder
                        made = createVideoProxy(next.path, opt, progress, &err);
                    }
                }
            }
            lk.lock();
            const bool stopped = cancel_ || quit_ || !enabled_;
            current_.clear();
            fraction_ = 0;
            if (made) ++doneCount_;
            else if (!stopped) failed_.insert(next.path);
            if (queue_.empty()) doneCount_ = 0;         // batch is over
            auto hook = doneHook_;
            idleCv_.notify_all();
            if (hook && !stopped) {
                lk.unlock();
                hook(next.path, made, err);
                lk.lock();
            }
        }
    }

    std::mutex m_;
    std::condition_variable cv_, idleCv_;
    bool quit_ = false, enabled_ = false, haveProject_ = false, cancel_ = false, pausedNow_ = false;
    std::atomic<bool> enabledFast_{false}, prefsControl_{false};
    std::atomic<int> settleMs_{1200};
    std::chrono::steady_clock::time_point changedAt_;
    std::shared_ptr<const Project> project_;
    ProjectLoader loader_;
    Assessor assessor_;
    Maker maker_;
    std::function<void(const std::string&, bool, const std::string&)> doneHook_;
    std::vector<Candidate> queue_;
    std::vector<std::string> planned_;
    std::set<std::string> failed_;
    std::map<std::string, bool> hinted_;
    bool longGop_ = true, fourK_ = false;
    std::map<std::string, VideoProxyAssessment> assessed_;
    std::string current_;
    double fraction_ = 0;
    int doneCount_ = 0;
    std::thread thread_;
};

}  // namespace autoproxy
}  // namespace app
}  // namespace reno
