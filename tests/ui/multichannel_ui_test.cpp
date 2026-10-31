// Multichannel work in the UI:
//   - a multichannel file put on the timeline as VEGAS does it (Preferences > Audio: one mono event per channel on tracks of their
//     own, a stereo event per pair, or one event mixed down), new tracks panned to their speakers;
//   - Preferences > Audio and Audio Device (channel count, Downmix), Audio Event Properties' Media channels, Project Media's
//     Properties showing the channels;
//   - the small surround panner in the audio track headers of a 5.1 project, and its double-click;
//   - six-channel playback to a six-channel sound card (a stand-in device), the fold-down on a stereo one and with Downmix, at
//     the normal rate and another;
//   - Render As's AC-3 and 5.1 AAC templates.
//
//   multichannel_ui_test <output folder>
//
// Build: uibuild multichannel_ui_test tests/ui/multichannel_ui_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen. The folder gets the pictures.
#include "../../src/app/editdialogs.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/renderdialog.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QStyleFactory>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
const double kPi = 3.14159265358979323846;

void shot(QWidget* w, const char* name) { w->grab().save(gOut + "/" + name + ".png"); }

void theme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(220, 220, 220), mid(62, 62, 66), accent(0, 122, 204);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    QApplication::setPalette(p);
}

// A 6-channel WAVE file (5.1): each channel a tone of its own.
bool writeSix(const std::string& path)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    es.sampleRate = 48000;
    es.audioChannels = 6;
    es.audioFamily = "pcm";
    std::string err;
    std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
    if (!w) return false;
    const double freq[6] = {300, 500, 700, 60, 1100, 1300};
    std::vector<float> ch[6];
    for (auto& c : ch) c.resize(4800);
    for (int b = 0; b < 30; ++b) {
        for (int c = 0; c < 6; ++c)
            for (int k = 0; k < 4800; ++k) ch[c][(size_t)k] = (float)(0.3 * std::sin(2.0 * kPi * freq[c] * (b * 4800 + k) / 48000.0));
        const float* p[6] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data(), ch[4].data(), ch[5].data()};
        w->writeAudioChannels(p, 6, 4800);
    }
    return w->finish(&err);
}

// The events made from 'media', in track order.
std::vector<std::pair<const Track*, const Event*>> eventsOf(uint64_t media)
{
    std::vector<std::pair<const Track*, const Event*>> out;
    for (const Track& t : AppState::get().project().tracks)
        for (const Event& e : t.events)
            if (e.mediaId == media) out.emplace_back(&t, &e);
    return out;
}

const Track* trackById(uint64_t id)
{
    for (const Track& t : AppState::get().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

void setSurround(bool on)
{
    AppState::get().edit("Master Bus Mode", [on](Project& p) { p.settings.audioChannels = on ? 6 : 2; });
}

// Puts the file on the timeline at 0 in a fresh project of that many channels, with the import mode set.
uint64_t freshImport(const std::string& file, const std::string& mode, bool surround, bool pan = true)
{
    AppState& app = AppState::get();
    app.newProject();
    setSurround(surround);
    app.prefs.multichannelImport = mode;
    app.prefs.panMultichannelTracks = pan;
    std::string err;
    const uint64_t m = app.importMedia(file, &err);
    app.addMediaToTimeline(m, 0.0);
    return m;
}

void testImport(const std::string& file)
{
    AppState& app = AppState::get();
    {
        const uint64_t m = freshImport(file, "tracks", false);
        const MediaItem* mi = app.project().findMedia(m);
        check(mi && mi->info.channels == 6 && mi->info.channelNames.size() == 6, "import: the 6-channel file is read with its channels' names");
        auto ev = eventsOf(m);
        bool picks = ev.size() == 6, tracks = true, group = true;
        for (size_t k = 0; k < ev.size(); ++k) {
            picks = picks && ev[k].second->mediaChannel == (int)k && ev[k].second->mediaChannels == 1;
            for (size_t j = 0; j < k; ++j) tracks = tracks && ev[k].first != ev[j].first;
            group = group && ev[k].second->group != 0 && ev[k].second->group == ev[0].second->group;
        }
        check(picks && tracks && group, "import (tracks): one mono event per channel, each on its own track, all grouped", std::to_string(ev.size()) + " events");
        if (ev.size() == 6)
            check(ev[0].first->pan == -1.0 && ev[1].first->pan == 1.0 && ev[2].first->pan == 0.0 && ev[3].first->pan == 0.0 && ev[4].first->pan == -1.0 &&
                      ev[5].first->pan == 1.0,
                  "import (tracks, stereo project): new tracks panned to their side (L, Ls left; R, Rs right; C, LFE centre)");
    }
    {
        const uint64_t m = freshImport(file, "tracks", true);
        auto ev = eventsOf(m);
        if (ev.size() == 6) {
            const SurroundPan &L = ev[0].first->surround, &C = ev[2].first->surround, &LFE = ev[3].first->surround, &Rs = ev[5].first->surround;
            check(L.x == -1 && L.y == 1 && C.x == 0 && C.y == 1 && LFE.route == "lfe" && Rs.x == 1 && Rs.y == -1 && ev[0].first->pan == 0.0,
                  "import (tracks, 5.1 project): new tracks' surround panners put on their speakers (LFE: LFE only)");
        } else {
            check(false, "import (tracks, 5.1 project): six events", std::to_string(ev.size()));
        }
    }
    {
        const uint64_t m = freshImport(file, "pairs", true);
        auto ev = eventsOf(m);
        const bool ok = ev.size() == 3 && ev[0].second->mediaChannel == 0 && ev[1].second->mediaChannel == 2 && ev[2].second->mediaChannel == 4 &&
                        ev[0].second->mediaChannels == 2 && ev[2].second->mediaChannels == 2;
        check(ok, "import (pairs): a stereo event per pair of channels (1/2, 3/4, 5/6)", std::to_string(ev.size()) + " events");
        if (ok) check(ev[0].first->surround.route == "front" && ev[2].first->surround.route == "rear" && ev[1].first->surround.route == "surround",
                      "import (pairs, 5.1): L/R to the front speakers, Ls/Rs to the rear, C/LFE left panned");
    }
    {
        const uint64_t m = freshImport(file, "tracks", true, false);
        auto ev = eventsOf(m);
        check(ev.size() == 6 && ev[0].first->surround.isDefault() && ev[5].first->surround.isDefault(),
              "import: with \"Set pan for new tracks from multichannel\" off, the tracks are left as they are");
    }
    {
        const uint64_t m = freshImport(file, "mixdown", false);
        auto ev = eventsOf(m);
        check(ev.size() == 1 && ev[0].second->mediaChannel == -1, "import (mixdown): one event, all the channels mixed down");
    }
    {
        // a stereo file is not touched by any of this
        app.newProject();
        app.prefs.multichannelImport = "tracks";
        std::string err;
        const uint64_t m = app.importMedia("synthaudio:1", &err);
        app.addMediaToTimeline(m, 0.0);
        auto ev = eventsOf(m);
        check(ev.size() == 1 && ev[0].second->mediaChannel == -1, "import: a stereo file is one event as always");
    }
    app.prefs.multichannelImport = "tracks";
    app.prefs.panMultichannelTracks = true;
}

// Presses a dialog's OK button (which applies what was chosen), else just closes it.
void clickOk(QDialog* d)
{
    for (QPushButton* b : d->findChildren<QPushButton*>())
        if (b->text().remove('&') == "OK") {
            b->click();
            return;
        }
    d->accept();
}

// Runs 'fn' on the first modal dialog that opens (then it should close it).
void onDialog(std::function<void(QDialog*)> fn)
{
    QTimer::singleShot(150, [fn] {
        for (QWidget* w : QApplication::topLevelWidgets())
            if (auto* d = qobject_cast<QDialog*>(w))
                if (d->isVisible()) {
                    fn(d);
                    return;
                }
    });
}

void testDialogs(const std::string& file)
{
    AppState& app = AppState::get();
    // as a first run has them: an earlier run's OK may have saved other choices
    app.prefs.multichannelImport = "tracks";
    app.prefs.panMultichannelTracks = true;
    app.prefs.downmixOutput = false;
    // Preferences > Audio and Audio Device
    onDialog([&](QDialog* d) {
        auto* tabs = d->findChild<QTabWidget*>();
        int audio = -1, device = -1;
        for (int k = 0; tabs && k < tabs->count(); ++k) {
            if (tabs->tabText(k) == "Audio") audio = k;
            if (tabs->tabText(k) == "Audio Device") device = k;
        }
        check(audio >= 0 && device == audio + 1, "preferences: an Audio tab before Audio Device");
        auto* mc = d->findChild<QComboBox*>("multichannelImport");
        auto* pan = d->findChild<QCheckBox*>("panMultichannelTracks");
        auto* label = d->findChild<QLabel*>("deviceChannels");
        auto* dm = d->findChild<QCheckBox*>("downmixOutput");
        check(mc && mc->count() == 3 && mc->currentData().toString() == "tracks" && pan && pan->isChecked(), "preferences: Add multichannel audio as (three ways) and Set pan");
        check(label && !label->text().isEmpty() && dm && !dm->isChecked(), "preferences: Audio Device shows the device's channels and Downmix",
              label ? label->text().toStdString() : "");
        if (tabs && audio >= 0) {
            tabs->setCurrentIndex(audio);
            d->resize(640, 420);
            uitest::settle(50);
            shot(d, "preferences_audio");
            tabs->setCurrentIndex(device);
            uitest::settle(50);
            shot(d, "preferences_audio_device");
        }
        if (mc) mc->setCurrentIndex(mc->findData("pairs"));
        if (dm) dm->setChecked(true);
        clickOk(d);
    });
    runPreferences(nullptr);
    check(app.prefs.multichannelImport == "pairs" && app.prefs.downmixOutput, "preferences: the choices are kept");
    app.prefs.multichannelImport = "tracks";
    app.prefs.downmixOutput = false;
    app.prefs.save();

    // Audio Event Properties: Media channels
    const uint64_t m = freshImport(file, "tracks", false);
    auto ev = eventsOf(m);
    if (ev.size() < 3) return;
    const uint64_t id = ev[2].second->id;
    onDialog([&](QDialog* d) {
        auto* mc = d->findChild<QComboBox*>("mediaChannels");
        check(mc && mc->count() == 10 && mc->currentData().toString() == "2:1" && mc->currentText() == "Channel 3 (C)",
              "event properties: Media channels lists all, each channel and each pair; this event's is Channel 3 (C)",
              mc ? (std::to_string(mc->count()) + " " + mc->currentText().toStdString()) : "none");
        if (mc) {
            QStringList all;
            for (int k = 0; k < mc->count(); ++k) all << mc->itemText(k);
            check(all.contains("Channels 5/6 (Ls/Rs)") && all.contains("All 6 channels, mixed to stereo"), "event properties: pairs are named by their speakers",
                  all.join(" | ").toStdString());
            mc->setCurrentIndex(mc->findData("4:2"));
        }
        d->resize(520, d->sizeHint().height());
        uitest::settle(50);
        shot(d, "event_properties_media_channels");
        clickOk(d);
    });
    runEventProperties(nullptr, id);
    const Event* e = app.project().findEvent(id);
    check(e && e->mediaChannel == 4 && e->mediaChannels == 2, "event properties: the event now plays channels 5/6");
    app.undo();
    e = app.project().findEvent(id);
    check(e && e->mediaChannel == 2 && e->mediaChannels == 1, "event properties: undone");

    // Project Media: Properties shows the channels
    MediaBin bin;
    bin.resize(700, 300);
    bin.show();
    uitest::settle(100);
    auto* list = bin.findChild<QTreeWidget*>("mediaList");
    QToolButton* props = nullptr;
    for (QToolButton* b : bin.findChildren<QToolButton*>("mediaProperties")) props = b;
    if (list) {
        for (int k = 0; k < list->topLevelItemCount(); ++k) list->topLevelItem(k)->setSelected(true);
        if (list->topLevelItemCount() > 0) list->setCurrentItem(list->topLevelItem(0));
    }
    bool seen = false;
    onDialog([&](QDialog* d) {
        QLabel* ch = d->findChild<QLabel*>("mediaChannels");
        seen = ch != nullptr;
        check(ch && ch->text().contains("6 (5.1") && ch->text().contains("4 LFE") && ch->text().contains("6 Rs"),
              "project media: Properties shows the channels by speaker", ch ? ch->text().toStdString() : "");
        auto* tabs = d->findChild<QTabWidget*>();
        if (tabs)
            for (int k = 0; k < tabs->count(); ++k)
                if (tabs->tabText(k) == "Media") tabs->setCurrentIndex(k);
        uitest::settle(50);
        shot(d, "media_properties_channels");
        d->accept();
    });
    if (props && props->isEnabled()) props->click();
    check(seen, "project media: the Properties dialog opened");
}

void testHeaders(const std::string& file, Playback& pb)
{
    AppState& app = AppState::get();
    freshImport(file, "tracks", true);
    // a stereo sound too, panned by hand, with keyframes
    std::string err;
    const uint64_t s = app.importMedia("synthaudio:1", &err);
    const uint64_t t7 = app.addTrack(TrackType::Audio);
    app.addMediaToTimeline(s, 0.0, t7);
    app.edit("Surround Pan", [&](Project& p) {
        if (Track* t = p.findTrack(t7)) {
            t->surround.x = 0.5;
            t->surround.y = -0.3;
            t->surround.speakerMutes = 1u << ChC;
        }
    });
    uint64_t firstAudio = 0;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Audio && !firstAudio) firstAudio = t.id;
    {
        const char* name = "vegas";
        TimelineWidget tl(&pb);
        tl.resize(1100, 640);
        tl.show();
        uitest::settle(150);
        const QRect sq = tl.headerPartRect(firstAudio, TimelineWidget::PartSurround);
        const QRect pan = tl.headerPartRect(firstAudio, TimelineWidget::PartPan);
        const QRect level = tl.headerPartRect(firstAudio, TimelineWidget::PartLevel);
        const QRect head = tl.headerRect(firstAudio);
        check(!sq.isEmpty() && pan.isEmpty() && head.contains(sq) && level.right() + 8 < sq.left() && sq.width() == sq.height(),
              std::string("headers (") + name + "): a 5.1 project's audio track shows the small surround panner instead of the pan slider",
              std::to_string(sq.x()) + "," + std::to_string(sq.y()) + " " + std::to_string(sq.width()));
        uint64_t opened = 0;
        tl.onOpenSurround = [&](uint64_t id) { opened = id; };
        uitest::mouseDoubleClick(&tl, sq.center());
        check(opened == firstAudio && app.currentTrack == firstAudio, std::string("headers (") + name + "): a double-click on it opens the Surround Panner for that track");
        // dragging in it moves the point (one undo step); a track with keyframes gets a key at the cursor
        const QRect sq7 = tl.headerPartRect(t7, TimelineWidget::PartSurround);
        if (!sq7.isEmpty()) {
            uitest::mouseDrag(&tl, sq7.center(), QPoint(sq7.left() + 4, sq7.top() + 4));
            const Track* t = trackById(t7);
            check(t && t->surround.x < -0.6 && t->surround.y > 0.6 && t->surround.keys.empty(),
                  std::string("headers (") + name + "): dragging the small panner's point moves the track's pan point",
                  t ? strfmt("%.2f, %.2f", t->surround.x, t->surround.y) : "");
            app.undo();
            t = trackById(t7);
            check(t && std::fabs(t->surround.x - 0.5) < 1e-9 && std::fabs(t->surround.y + 0.3) < 1e-9, std::string("headers (") + name + "): ... undone in one step");
            {
                app.edit("Surround Pan Keyframe", [&](Project& p) {
                    if (Track* tt = p.findTrack(t7)) {
                        SurroundKey k;
                        k.time = 0.0;
                        k.x = 0.5;
                        k.y = -0.3;
                        tt->surround.keys.push_back(k);
                    }
                });
                app.setCursor(2.0);
                uitest::mouseDrag(&tl, sq7.center(), QPoint(sq7.right() - 4, sq7.top() + 4));
                t = trackById(t7);
                check(t && t->surround.keys.size() == 2 && std::fabs(t->surround.keys[1].time - 2.0) < 1e-6 && t->surround.keys[1].x > 0.6 &&
                          t->surround.keys[0].x == 0.5,
                      "headers (vegas): with keyframes, the drag sets a key at the cursor");
                app.undo();
                app.undo();
                app.setCursor(0.0);
            }
        }
        // a fixed route has no point to drag
        app.edit("Surround Pan", [&](Project& p) { if (Track* tt = p.findTrack(firstAudio)) tt->surround.route = "lfe"; });
        const double fx = trackById(firstAudio)->surround.x, fy = trackById(firstAudio)->surround.y;
        uitest::mouseDrag(&tl, sq.center(), sq.bottomRight() - QPoint(3, 3));
        check(trackById(firstAudio)->surround.x == fx && trackById(firstAudio)->surround.y == fy,
              std::string("headers (") + name + "): a fixed route (LFE Only) is not dragged");
        uitest::settle(800);  // the waveforms of the picked channels are read meanwhile
        shot(&tl, "headers_51_vegas");
        // a picture of one header, larger
        const QRect hr = tl.headerRect(t7);
        if (!hr.isEmpty()) {
            QPixmap px = tl.grab(hr);
            px.scaled(px.width() * 2, px.height() * 2).save(gOut + "/header_51_" + name + "_zoom.png");
        }
        // a stereo project: the pan slider again
        setSurround(false);
        uitest::settle(80);
        check(tl.headerPartRect(firstAudio, TimelineWidget::PartSurround).isEmpty() && !tl.headerPartRect(firstAudio, TimelineWidget::PartPan).isEmpty(),
              std::string("headers (") + name + "): in stereo the pan slider is back");
        setSurround(true);
        uitest::settle(50);
    }
}

// A sound card stand-in: takes sound at the rate it would be played (like the silent output), in as many channels as it is started with, and keeps it.
class FakeDevice : public AudioOutput {
public:
    explicit FakeDevice(int maxCh) : max_(maxCh) {}
    ~FakeDevice() override { stop(); }
    bool start(int rate, Pull pull) override
    {
        stop();
        channels = 2;
        run([pull](float* const* ch, int, int n) { pull(ch[0], ch[1], n); }, rate);
        return true;
    }
    int maxChannels() const override { return max_; }
    bool startChannels(int rate, int ch, PullChannels pull) override
    {
        stop();
        if (ch > max_) return false;
        channels = ch;
        run(pull, rate);
        return true;
    }
    void stop() override
    {
        go_ = false;
        if (th_.joinable()) th_.join();
    }
    std::string name() const override { return "stand-in device"; }
    int channels = 0;
    std::mutex m;
    std::vector<float> got[6];

private:
    void run(PullChannels pull, int rate)
    {
        for (auto& g : got) g.clear();
        go_ = true;
        th_ = std::thread([this, pull, rate] {
            std::vector<float> b[6];
            for (auto& x : b) x.assign(1024, 0.f);
            float* p[6] = {b[0].data(), b[1].data(), b[2].data(), b[3].data(), b[4].data(), b[5].data()};
            const auto t0 = std::chrono::steady_clock::now();
            int64_t taken = 0;
            while (go_) {
                const int64_t due = (int64_t)(std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() * rate);
                while (go_ && taken < due) {
                    pull(p, channels, 1024);
                    std::lock_guard<std::mutex> lk(m);
                    for (int c = 0; c < channels; ++c) got[c].insert(got[c].end(), b[c].begin(), b[c].end());
                    taken += 1024;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });
    }
    int max_;
    std::atomic<bool> go_{false};
    std::thread th_;
};

double rmsOf(const std::vector<float>& v, size_t from)
{
    double e = 0;
    size_t n = 0;
    for (size_t k = from; k < v.size(); ++k, ++n) e += (double)v[k] * v[k];
    return n ? std::sqrt(e / n) : 0.0;
}

void testPlayback(Playback& pb)
{
    AppState& app = AppState::get();
    app.newProject();
    setSurround(true);
    std::string err;
    const uint64_t s = app.importMedia("synthaudio:2", &err);
    app.addMediaToTimeline(s, 0.0);
    uint64_t track = 0;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Audio) track = t.id;
    app.edit("Surround Pan", [&](Project& p) { if (Track* t = p.findTrack(track)) t->surround.route = "center"; });
    app.setCursor(1.0);
    auto playFor = [&](FakeDevice* dev, double rate) {
        pb.setAudioOutput(std::unique_ptr<AudioOutput>(dev));
        if (rate == 1.0) pb.play();
        else pb.playAt(rate);
        const int during = pb.outputChannels();
        uitest::settle(700);
        pb.stop();
        return during;
    };
    {
        FakeDevice* dev = new FakeDevice(6);
        const int during = playFor(dev, 1.0);
        std::lock_guard<std::mutex> lk(dev->m);
        const double c = rmsOf(dev->got[ChC], 4096), l = rmsOf(dev->got[ChL], 4096), lfe = rmsOf(dev->got[ChLFE], 4096);
        check(during == 6 && dev->channels == 6 && c > 0.05 && l < 1e-4 && lfe < 1e-4,
              "playback: a 5.1 project on a six-channel device plays its six channels (centre-only sound on C alone)",
              strfmt("channels %d, C %.4f, L %.5f, LFE %.5f", dev->channels, c, l, lfe));
    }
    {
        FakeDevice* dev = new FakeDevice(6);
        const int during = playFor(dev, 1.5);
        std::lock_guard<std::mutex> lk(dev->m);
        const double c = rmsOf(dev->got[ChC], 4096), l = rmsOf(dev->got[ChL], 4096);
        check(during == 6 && dev->channels == 6 && c > 0.05 && l < 1e-4, "playback: ... and at another rate (1.5x, the varispeed path)",
              strfmt("C %.4f, L %.5f", c, l));
    }
    {
        FakeDevice* dev = new FakeDevice(2);
        const int during = playFor(dev, 1.0);
        std::lock_guard<std::mutex> lk(dev->m);
        const double l = rmsOf(dev->got[0], 4096), r = rmsOf(dev->got[1], 4096);
        check(during == 2 && dev->channels == 2 && l > 0.03 && std::fabs(l - r) < 0.01, "playback: on a stereo device the 5.1 mix is folded down (C on both sides)",
              strfmt("L %.4f, R %.4f", l, r));
    }
    {
        app.prefs.downmixOutput = true;
        FakeDevice* dev = new FakeDevice(6);
        const int during = playFor(dev, 1.0);
        app.prefs.downmixOutput = false;
        check(during == 2 && dev->channels == 2, "playback: with Downmix on, a six-channel device gets the stereo fold-down");
    }
    {
        setSurround(false);
        FakeDevice* dev = new FakeDevice(6);
        const int during = playFor(dev, 1.0);
        std::lock_guard<std::mutex> lk(dev->m);
        check(during == 2 && dev->channels == 2 && rmsOf(dev->got[0], 4096) > 0.03, "playback: a stereo project plays in stereo on a six-channel device");
    }
    pb.setAudioOutput(createSilentOutput());
}

void testRenderTemplates()
{
    const QStringList names = renderTemplateNames();
    for (const char* n : {"Dolby Digital AC-3 Studio/Stereo DVD", "Dolby Digital AC-3 Studio/5.1 Surround DVD", "Dolby Digital AC-3 Studio/Stereo DVD, AGC on",
                          "Dolby Digital AC-3 Studio/5.1 Surround DVD, AGC on", "MAGIX AAC Audio/448 Kbps 5.1 Surround", "MAGIX AAC Audio/640 Kbps 5.1 Surround",
                          "MAGIX AVC/AAC MP4/Internet HD 1080p 29.97 fps, 5.1 Surround", "MAGIX AVC/AAC MP4/Internet HD 1080p 25 fps, 5.1 Surround"})
        check(names.contains(n), std::string("render: the template is there: ") + n);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    gOut = argc > 1 ? QString(argv[1]) : QString("multichannel_ui_out");
    QDir().mkpath(gOut);
    theme();
    uitest::setup({}, {});
    const std::string file = (gOut + "/six.wav").toStdString();
    check(writeSix(file), "a 6-channel test file is written");
    testImport(file);
    testDialogs(file);
    Playback pb;
    pb.setAudioOutput(createSilentOutput());
    testHeaders(file, pb);
    testPlayback(pb);
    testRenderTemplates();
    return uitest::summary();
}
