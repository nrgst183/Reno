// VEGAS Pro 23 additions: adjustment events, unquantized edge display, Normal tool mouse behaviour.
//   man2b_test <output folder>
// Build: uibuild man2b_test tests/ui/man2b_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QSettings>
#include "../../src/media/media.h"
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {
// A 10 second picture that is red, then green from 3.0 s, then blue from 7.5 s (cuts at 3.0 and 7.5), with slow brightness drift
struct CutReader : MediaReader {
    MediaInfo mi;
    CutReader() { mi.hasVideo = true; mi.width = 160; mi.height = 90; mi.fps = 25; mi.duration = 10; }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double t, int, int) override {
        ImagePtr img = Image::make(160, 90);
        const int drift = (int)(t * 2);
        uint8_t b = 0, g = 0, r = 0;
        if (t < 3.0) r = 200 + drift; else if (t < 7.5) g = 200 + drift; else b = 200 + drift;
        for (int y = 0; y < 90; ++y)
            for (int x = 0; x < 160; ++x) {
                uint8_t* px = img->row(y) + x * 4;
                px[0] = b; px[1] = g; px[2] = r; px[3] = 255;
            }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

AppState& st() { return AppState::get(); }

const Track* trk(uint64_t id)
{
    for (const Track& t : st().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

void adjustmentTests()
{
    st().newProject();
    st().edit("Build", [&](Project& p) {
        for (int k = 0; k < 2; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k == 0 ? TrackType::Video : TrackType::Audio;
            p.tracks.push_back(t);
        }
    });
    const uint64_t v = st().project().tracks[0].id, a = st().project().tracks[1].id;
    const uint64_t id = st().insertEmptyEvent(v, 2.0, 3.0, true);
    check(id != 0 && trk(v)->events.size() == 1 && trk(v)->events[0].adjustment, "insertEmptyEvent(adjustment) makes an adjustment event");
    check(trk(v)->events[0].mediaId == 0 && std::fabs(trk(v)->events[0].length - 3.0) < 1e-9, "it has no media and the given length");
    const uint64_t plainId = st().insertEmptyEvent(v, 6.0, 1.0);
    check(plainId != 0 && !trk(v)->events[1].adjustment, "a plain empty event is not an adjustment event");

    // saved and loaded again
    Json j = st().project().toJson();
    Project q;
    std::string err;
    q.fromJson(j, &err);
    check(q.findTrack(v) && q.findTrack(v)->events.size() == 2 && q.findTrack(v)->events[0].adjustment && !q.findTrack(v)->events[1].adjustment, "the flag survives the project JSON", err);

    // the Insert menu command works on the current video track and refuses an audio track
    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1000, 400);
    w.show();
    st().currentTrack = a;
    const size_t before = trk(v)->events.size() + trk(a)->events.size();
    st().setCursor(10.0);
    st().setLoop(0, 0);
    w.insertAdjustmentEvent();
    check(trk(v)->events.size() + trk(a)->events.size() == before, "Insert Adjustment Event on an audio track does nothing");
    st().currentTrack = v;
    w.insertAdjustmentEvent();
    check(trk(v)->events.size() == 3 && trk(v)->events.back().adjustment && std::fabs(trk(v)->events.back().start - 10.0) < 1e-9, "on a video track it adds one at the cursor");
    st().undo();
    check(trk(v)->events.size() == 2, "and undo removes it");
    st().setLoop(20.0, 22.0);
    w.insertAdjustmentEvent();
    check(trk(v)->events.size() == 3 && std::fabs(trk(v)->events.back().start - 20.0) < 1e-9 && std::fabs(trk(v)->events.back().length - 2.0) < 1e-9, "with a time selection it covers the selection");
    st().setLoop(0, 0);
}

void sceneTests()
{
    st().mediaOpener = [](const std::string& path, std::string*) -> std::unique_ptr<MediaReader> {
        if (path == "cuts.fake") return std::make_unique<CutReader>();
        return nullptr;
    };
    st().newProject();
    std::string err;
    const uint64_t m = st().importMedia("cuts.fake", &err);
    check(m != 0, "the synthetic three-shot clip imports", err);
    st().edit("Build", [&](Project& p) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        p.tracks.push_back(t);
        Event e;
        e.id = p.newId();
        e.mediaId = m;
        e.start = 1.0;
        e.length = 10.0;
        e.name = "cuts";
        p.tracks[0].events.push_back(e);
    });
    const uint64_t eid = st().project().tracks[0].events[0].id;
    std::vector<double> cuts = st().detectScenes(eid, 50);
    check(cuts.size() == 2 && std::fabs(cuts[0] - 4.0) < 0.1 && std::fabs(cuts[1] - 8.5) < 0.1, "detectScenes finds the two cuts (timeline 4.0 and 8.5)", cuts.empty() ? "none" : std::to_string(cuts.size()) + " first " + std::to_string(cuts[0]));
    st().setSelection({eid});
    st().snapping = false;
    const int n = st().splitAtScenes(50);
    check(n == 2 && st().project().tracks[0].events.size() == 3, "splitAtScenes cuts the event in three", std::to_string(st().project().tracks[0].events.size()));
    st().mediaOpener = nullptr;
}

void optionTests()
{
    Preferences defaults;
    check(defaults.rightMouseSelect, "the right-mouse selection preference defaults to VEGAS 18 behaviour");
    TimelineOptions o;
    check(!o.showUnquantized, "Show Unquantized Event Edges is off by default");
    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1000, 400);
    w.show();
    w.options().showUnquantized = true;
    w.optionsChanged();
    uitest::settle(100);
    QImage im = uitest::shot(&w, "man2b_unquantized.png");
    check(!im.isNull(), "the timeline draws with unquantized edges on");
}
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("man2b_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("man2b_shots");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    adjustmentTests();
    sceneTests();
    optionTests();
    return uitest::summary();
}
