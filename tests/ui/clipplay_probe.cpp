// Measures how a real project plays (Playback with a stand-in sound device pulled in real time): pictures a second,
// longest gap between pictures, dropped frames, and with the profiler on, frames decoded vs. reads asked for per media file, and seeks.
//   clipplay_probe <project> <from s> <seconds> [WxH preview=480x270] [threads]
// Environment: RENO_DISPLAY_HZ (screen rate, e.g. 165), PP_PIECE (sound block).
#include "../../src/app/playback.h"
#include "../../src/engine/profile.h"
#include "uitestlib.h"
#include <QApplication>
#include <QFileInfo>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <thread>
using namespace reno; using namespace reno::app;
class ManualOutput : public AudioOutput {
public:
    Pull pull;
    bool start(int, Pull p) override { pull = std::move(p); return true; }
    void stop() override { pull = Pull(); }
    std::string name() const override { return "test"; }
};
int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    uitest::setup({}, {});
    AppState& app = AppState::get();
    std::string err;
    if (argc < 4 || !app.open(argv[1], &err)) { printf("open failed: %s\n", err.c_str()); return 1; }
    const double from = atof(argv[2]), secs = atof(argv[3]);
    int pw = 480, ph = 270;
    if (argc > 4) sscanf(argv[4], "%dx%d", &pw, &ph);
    if (argc > 5) app.prefs.renderThreads = atoi(argv[5]);
    {  // media named by a path from the machine the project was made on: look in the project's folder by file name
        std::string dir = argv[1];
        const size_t k = dir.find_last_of('/');
        dir = k == std::string::npos ? "." : dir.substr(0, k);
        std::map<std::string, std::string> relink;
        for (const MediaItem& m : app.project().media) {
            if (!m.offline || m.generated) continue;
            const size_t b = m.path.find_last_of("/\\");
            const std::string cand = dir + "/" + (b == std::string::npos ? m.path : m.path.substr(b + 1));
            if (QFileInfo::exists(QString::fromStdString(cand))) relink[m.path] = cand;
        }
        std::map<uint64_t, MediaInfo> infos;
        for (const MediaItem& m : app.project().media) {
            auto it = relink.find(m.path);
            if (it == relink.end() || !app.mediaOpener) continue;
            std::string e2;
            std::unique_ptr<MediaReader> r = app.mediaOpener(it->second, &e2);
            if (r) infos[m.id] = r->info();
        }
        size_t gone = 0;
        app.edit("Relink", [&](Project& p) {
            for (MediaItem& m : p.media) {
                auto it = relink.find(m.path);
                if (it == relink.end()) { if (m.offline && !m.generated) ++gone; continue; }
                m.path = it->second;
                m.offline = false;
                auto inf = infos.find(m.id);
                if (inf != infos.end()) {
                    const double sub = m.subclipLength;
                    m.info = inf->second;
                    if (sub > 0) m.info.duration = std::max(0.0, std::min(sub, inf->second.duration - m.subclipStart));
                }
            }
        });
        printf("relinked %zu media by name, %zu stay offline\n", relink.size(), gone);
    }
    if (getenv("PP_LIST")) {  // list the video events (start, length, rate, reversed, media)
        const Project& pr = app.project();
        for (const Track& t : pr.tracks) {
            if (t.type != TrackType::Video) continue;
            for (const Event& e : t.events) {
                const MediaItem* m = pr.findMedia(e.mediaId);
                printf("  v %s%.3f len %.3f off %.3f rate %.3f%s %s\n", t.mute ? "(muted) " : "", e.start, e.length, e.offset, e.rate, e.reverse ? " REV" : "",
                       m ? (m->path + (m->offline ? " [OFFLINE]" : "")).c_str() : "?");
            }
        }
        return 0;
    }
    Playback pb;
    ManualOutput* out = new ManualOutput();
    pb.setAudioOutput(std::unique_ptr<AudioOutput>(out));
    pb.setPreviewSize(pw, ph, getenv("PP_AUTO") != nullptr);
    int shown = 0;
    double lastAt = 0, worst = 0, worstSteady = 0;
    int slowGaps = 0;
    double playStart = 0;
    std::vector<int> perSecond;
    auto now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    pb.onFrame = [&](const QImage&, double, double) {
        ++shown;
        const double t = now();
        if (lastAt > 0) {
            worst = std::max(worst, t - lastAt);
            if (t - playStart > 1.5) {  // the first moments open the files
                worstSteady = std::max(worstSteady, t - lastAt);
                if (t - lastAt > 0.05) ++slowGaps;
            }
        }
        lastAt = t;
    };
    app.setCursor(from);
    for (int k = 0; k < 300; ++k) { QCoreApplication::processEvents(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    shown = 0;
    if (getenv("PP_PROFILE")) { profile::reset(); profile::setEnabled(true); }
    playStart = now();
    pb.play();
    const int piece = getenv("PP_PIECE") ? atoi(getenv("PP_PIECE")) : 240;
    std::vector<float> l(piece), r(piece);
    const auto t0 = std::chrono::steady_clock::now();
    const auto until = t0 + std::chrono::milliseconds((int)(secs * 1000));
    auto next = t0;
    int lastShown = 0;
    auto nextSec = t0 + std::chrono::seconds(1);
    lastAt = 0;
    while (std::chrono::steady_clock::now() < until) {
        QCoreApplication::processEvents();
        if (std::chrono::steady_clock::now() >= next && out->pull) {
            out->pull(l.data(), r.data(), piece);
            next += std::chrono::microseconds(piece * 1000000LL / 48000);
        }
        if (std::chrono::steady_clock::now() >= nextSec) { perSecond.push_back(shown - lastShown); lastShown = shown; nextSec += std::chrono::seconds(1); }
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    const double fps = app.project().settings.fps();
    printf("project %.3f fps; played %.1f s from %.1f at %dx%d, threads %d: %d pictures shown (%.1f a second), dropped %llu, worst gap %.0f ms (after 1.5 s: %.0f ms, %d gaps over 50 ms), rendered %llu (+%llu ahead)\n",
           fps, secs, from, pw, ph, app.prefs.renderThreads, shown, shown / secs, (unsigned long long)pb.droppedFrames(), worst * 1000, worstSteady * 1000, slowGaps,
           (unsigned long long)pb.picturesRendered(), (unsigned long long)pb.picturesRenderedAhead());
    printf("shown per second:");
    for (int n : perSecond) printf(" %d", n);
    printf("\n");
    pb.stop();
    if (profile::enabled()) {
        profile::setEnabled(false);
        printf("%s", profile::formatReport(profile::snapshot(), secs * 1000).c_str());
    }
    return 0;
}
