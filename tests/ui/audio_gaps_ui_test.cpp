// Tests for the Mixing Console's input buses, split master faders and Preview bus, and for recording from a track's own
// input (Record Input), with takes, punch-in and loop passes (src/app/mixer.cpp, src/app/recorder.cpp).
//
//   audio_gaps_ui_test <output folder>
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mixer.h"
#include "../../src/app/playback.h"
#include "../../src/app/recorder.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

template <class T>
T* kid(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

QRect rectOf(QWidget* w, const char* prop)
{
    if (!w) return QRect();
    w->grab();
    return w->property(prop).toRect();
}
void click(QWidget* w)
{
    if (w) uitest::mouseClick(w, w->rect().center());
}

AppState& st() { return AppState::get(); }

const InputBus* inputBus(uint64_t id) { return const_cast<Project&>(st().project()).findInputBus(id); }
const Track* trackOf(uint64_t id) { return const_cast<Project&>(st().project()).findTrack(id); }

// Opens a tool button's menu as a click would (its contents are made when it is about to show) and triggers the action
// with this name, anywhere in its submenus.
bool pickFrom(QToolButton* b, const QString& action)
{
    if (!b || !b->menu()) return false;
    QMenu* m = b->menu();
    m->aboutToShow();
    for (QAction* a : m->findChildren<QAction*>(action)) {
        a->trigger();
        return true;
    }
    return false;
}
bool hasAction(QToolButton* b, const QString& action)
{
    if (!b || !b->menu()) return false;
    b->menu()->aboutToShow();
    return !b->menu()->findChildren<QAction*>(action).isEmpty();
}

// A pretend sound card: every device gives its channels as tones, its own level for each: 0.1 + 0.05 per channel for
// "micA", 0.4 + 0.05 per channel for "micB" (4 channels), 0.2 for the default.
int devChannels(const std::string& id) { return id == "micB" ? 4 : 2; }
float devAmp(const std::string& id, int c) { return (id == "micA" ? 0.1f : id == "micB" ? 0.4f : 0.2f) + 0.05f * (float)c; }

class DevInput : public AudioInput {
public:
    bool start(int, Push, std::string*) override { return false; }
    bool startDevice(const std::string& id, int, int rate, PushN push, std::string*) override
    {
        id_ = id;
        rate_ = rate;
        push_ = push;
        timer_ = new QTimer();
        timer_->setInterval(10);
        clock_.start();
        QObject::connect(timer_, &QTimer::timeout, [this] {
            const int64_t due = clock_.elapsed() * (int64_t)rate_ / 1000;
            const int n = (int)(due - sent_);
            if (n <= 0) return;
            const int ch = devChannels(id_);
            std::vector<float> x((size_t)n * (size_t)ch);
            for (int k = 0; k < n; ++k)
                for (int c = 0; c < ch; ++c)
                    x[(size_t)k * (size_t)ch + (size_t)c] = devAmp(id_, c) * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * (double)(sent_ + k) / rate_);
            sent_ += n;
            push_(x.data(), ch, n);
        });
        timer_->start();
        return true;
    }
    void stop() override
    {
        delete timer_;
        timer_ = nullptr;
    }
    std::string name() const override { return "Pretend Card"; }
    int channelCount() const override { return devChannels(id_); }

private:
    std::string id_;
    int rate_ = 48000;
    PushN push_;
    QTimer* timer_ = nullptr;
    QElapsedTimer clock_;
    int64_t sent_ = 0;
};

// The loudest sample of a recorded event's file (left and right) in its middle.
bool peaksOf(const Event& e, float& l, float& r)
{
    const MediaItem* m = st().project().findMedia(e.mediaId);
    if (!m) return false;
    std::string err;
    std::unique_ptr<MediaReader> rd = openMedia(m->path, &err);
    if (!rd) return false;
    std::vector<float> L(4800), R(4800);
    if (!rd->audio(4800, 4800, 48000, L.data(), R.data())) return false;
    l = r = 0;
    for (size_t i = 0; i < L.size(); ++i) {
        l = std::max(l, std::fabs(L[i]));
        r = std::max(r, std::fabs(R[i]));
    }
    return true;
}

bool near(double a, double b, double eps = 0.03) { return std::fabs(a - b) <= eps; }

void freshProject(uint64_t& a1, uint64_t& a2)
{
    st().newProject();
    st().edit("Clear", [&](Project& p) { p.tracks.clear(); });
    a1 = st().addTrack(TrackType::Audio);
    a2 = st().addTrack(TrackType::Audio);
    st().armedTracks.clear();
    st().setSelection(std::set<uint64_t>());
    st().setLoop(0, 0);
    st().loopOn = false;
    st().setCursor(0.0);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    QApplication::setOrganizationName("RenoAudioGapsTest");
    QApplication::setApplicationName("audio_gaps_ui_test");
    QSettings().clear();
    gOut = argc > 1 ? QString(argv[1]) : QString("audiogaps_out");
    QDir().mkpath(gOut);
    uitest::setup({}, {});
    st().prefs.recordFolder = QDir(gOut).filePath("recorded").toStdString();
    vprefs::set("prefs/promptKeepRecorded", false);

    static std::vector<InputDeviceInfo> devices = {{"micA", "Mic A", 2}, {"micB", "Interface B", 4}};
    setInputDevicesForTest(&devices);

    st().newProject();
    std::string err;
    const uint64_t m1 = st().importMedia("synthaudio:1", &err);
    st().addMediaToTimeline(m1, 0.0);
    const uint64_t t2 = st().addTrack(TrackType::Audio);
    uint64_t t1 = 0;
    for (const auto& t : st().project().tracks)
        if (t.id != t2) t1 = t.id;

    Playback pb;
    pb.setAudioOutput(createSilentOutput());
    MixerWindow mixer(&pb);
    std::vector<std::pair<int, uint64_t>> opened;
    mixer.onOpenFx = [&](const FxTarget& t, uint64_t) { opened.push_back(std::make_pair((int)t.kind, t.id)); };
    mixer.resize(1100, 640);
    mixer.show();
    mixer.activateWindow();
    uitest::settle(150);

    // ================================================================================== input buses
    QToolButton* insert = kid<QToolButton>(&mixer, "insertInputBus");
    check(insert && insert->isEnabled(), "Insert Input Bus is a working button");
    click(insert);
    uitest::settle(50);
    click(insert);
    uitest::settle(80);
    check(st().project().inputBuses.size() == 2 && st().project().inputBuses[0].name == "Input Bus A" && st().project().inputBuses[1].name == "Input Bus B" &&
              st().undoName() == "Insert Input Bus",
          "Insert Input Bus adds Input Bus A and B (one undo step each)");
    const uint64_t ibA = st().project().inputBuses.size() > 0 ? st().project().inputBuses[0].id : 0;
    const uint64_t ibB = st().project().inputBuses.size() > 1 ? st().project().inputBuses[1].id : 0;
    QWidget* sa = mixer.findChild<QWidget*>(QString("strip:input:%1").arg(ibA));
    QWidget* sb = mixer.findChild<QWidget*>(QString("strip:input:%1").arg(ibB));
    check(sa && sb && sa->isVisible(), "and they get strips");
    check(sa && st().project().inputBuses.size() == 2 && sa->geometry().left() < mixer.findChild<QWidget*>(QString("strip:track:%1").arg(t1))->geometry().left(),
          "the input buses' strips are at the left, before the tracks'");

    // the input
    QToolButton* src = kid<QToolButton>(sa, "inputSource");
    check(src && hasAction(src, "src:micB:2:2") && hasAction(src, "src:micA:1:1") && !hasAction(src, "src:micA:1:2"),
          "the Input button lists the sound card inputs: mono channels, stereo pairs (not past the last channel)");
    check(src && !hasAction(src, "monitor:on") && !hasAction(src, "inputBus:1"), "an input bus's menu has no input busses or monitoring");
    check(pickFrom(src, "src:micB:2:2"), "choosing Interface B's inputs 3 + 4");
    check(inputBus(ibA) && inputBus(ibA)->device == "micB" && inputBus(ibA)->first == 2 && inputBus(ibA)->channels == 2 && st().undoName() == "Input Bus Input",
          "sets the bus's device, first channel and width");
    check(pickFrom(src, "src:micA:1:1") && inputBus(ibA)->device == "micA" && inputBus(ibA)->first == 1 && inputBus(ibA)->channels == 1, "a mono channel");
    uitest::settle(30);
    check(src && src->text().contains("Mic A") && src->text().contains("In 2"), "and the button names it", src ? src->text().toStdString() : "");

    // fader, mute, solo, output
    {
        QWidget* fader = kid<QWidget>(sa, "fader");
        const QRect cap = rectOf(fader, "cap");
        const double before = inputBus(ibA)->level;
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, -40));
        check(inputBus(ibA)->level > before * 1.3 && st().undoName() == "Input Bus Level", "dragging an input bus's fader up turns it up", strfmt("%.3f -> %.3f", before, inputBus(ibA)->level));
        uitest::mouseDoubleClick(fader, rectOf(fader, "cap").center());
        check(inputBus(ibA)->level == 1.0, "double-click puts it back to 0 dB");
        click(kid<QToolButton>(sa, "mute"));
        check(inputBus(ibA)->mute && !inputBus(ibB)->mute, "M mutes it");
        click(kid<QToolButton>(sa, "solo"));
        check(inputBus(ibA)->solo, "S solos it");
        click(kid<QToolButton>(sa, "solo"));
        click(kid<QToolButton>(sa, "mute"));
        check(!inputBus(ibA)->mute && !inputBus(ibA)->solo, "and they switch back");
        QComboBox* out = kid<QComboBox>(sa, "output");
        check(out && out->count() >= 2 && out->itemText(out->count() - 1) == "Off", "the output list ends with Off");
        if (out) {
            out->setCurrentIndex(out->count() - 1);
            emit out->activated(out->count() - 1);
        }
        check(inputBus(ibA)->outputOff && st().undoName() == "Input Bus Output", "Off silences the bus");
        if (out) {
            out->setCurrentIndex(0);
            emit out->activated(0);
        }
        check(!inputBus(ibA)->outputOff && inputBus(ibA)->output == 0, "Master sends it to the master again");
    }
    // a bus to send it to
    click(kid<QToolButton>(&mixer, "insertBus"));
    uitest::settle(60);
    const uint64_t busA = st().project().buses.empty() ? 0 : st().project().buses[0].id;
    // (the strips were made again: a new bus changes the console's shape)
    sa = mixer.findChild<QWidget*>(QString("strip:input:%1").arg(ibA));
    sb = mixer.findChild<QWidget*>(QString("strip:input:%1").arg(ibB));
    check(sa && sb, "the input buses' strips are still there after a bus is added");
    {
        QComboBox* out = kid<QComboBox>(sa, "output");
        int idx = -1;
        for (int i = 0; out && i < out->count(); ++i)
            if (out->itemData(i).toULongLong() == busA) idx = i;
        check(idx > 0, "the output list offers the buses");
        if (idx > 0) {
            out->setCurrentIndex(idx);
            emit out->activated(idx);
        }
        check(inputBus(ibA)->output == busA, "choosing one sends the input bus to it");
    }
    // effects
    {
        opened.clear();
        click(kid<QToolButton>(sa, "fx"));
        check(opened.size() == 1 && opened[0].first == (int)FxTarget::InputBus && opened[0].second == ibA, "the FX button opens the input bus's effects chain");
        FxTarget t;
        t.kind = FxTarget::InputBus;
        t.id = ibA;
        check(st().chainOf(st().project(), t) == &st().project().findInputBus(ibA)->fx && st().targetName(t) == "Input Bus: Input Bus A", "the chain is the bus's own");
    }
    // the show toggle and Delete
    {
        QToolButton* show = kid<QToolButton>(&mixer, "showInputBuses");
        check(show && show->isEnabled() && show->isChecked(), "the Inputs toggle in the View Controls works");
        show->setChecked(false);
        uitest::settle(30);
        check(!sa->isVisible() && !sb->isVisible(), "unchecking it hides the input buses");
        show->setChecked(true);
        uitest::settle(30);
        check(sa->isVisible(), "and checking it shows them");
    }
    uitest::shot(&mixer, gOut + "/console_input_buses.png");

    // ================================================================================== a track's Record Input
    {
        QWidget* ts = mixer.findChild<QWidget*>(QString("strip:track:%1").arg(t1));
        QToolButton* tsrc = kid<QToolButton>(ts, "inputSource");
        check(tsrc != nullptr, "a track strip has a Record Input button in its I/O region");
        check(hasAction(tsrc, "src:micB:0:2") && hasAction(tsrc, QString("inputBus:%1").arg((qulonglong)ibA)) && hasAction(tsrc, "monitor:on") && hasAction(tsrc, "monitor:auto"),
              "its menu: the inputs, the input busses and Input Monitoring (Off, On, Auto)");
        check(pickFrom(tsrc, "src:micB:2:1") && trackOf(t1)->recordInput.device == "micB" && trackOf(t1)->recordInput.first == 2 && trackOf(t1)->recordInput.channels == 1 &&
                  st().undoName() == "Record Input",
              "choosing Interface B's input 3 records from it, mono");
        const bool picked = pickFrom(tsrc, "monitor:on");
        check(picked && trackOf(t1)->recordInput.monitor == "on" && trackOf(t1)->recordInput.device == "micB", "Input Monitoring On keeps the input",
              strfmt("%d %s [%s] %d %d %s", (int)picked, trackOf(t1)->recordInput.monitor.c_str(), trackOf(t1)->recordInput.device.c_str(), trackOf(t1)->recordInput.first,
                     trackOf(t1)->recordInput.channels, st().undoName().c_str()));
        check(pickFrom(tsrc, QString("inputBus:%1").arg((qulonglong)ibA)) && trackOf(t1)->recordInput.inputBus == ibA, "an input bus can be the Record Input");
        uitest::settle(30);
        check(tsrc->text() == "Input Bus A", "and the button names it", tsrc->text().toStdString());
        check(pickFrom(tsrc, "src:micA:0:2") && trackOf(t1)->recordInput.inputBus == 0, "choosing an input again takes the input bus off");
        // the track right-click menu of the timeline has it too: TimelineWidget builds it with fillRecordInputMenu
    }
    // Delete Bus with an input bus selected: tracks recording from it record from their own input again
    {
        st().edit("From B", [&](Project& p) { p.findTrack(t1)->recordInput.inputBus = ibB; });
        QWidget* hdr = sb->findChild<QWidget*>("header");
        if (hdr) uitest::mouseClick(hdr, QPoint(hdr->width() / 2, 12));
        uitest::settle(40);
        check(mixer.property("selectedStrips").toStringList().contains(QString("strip:input:%1").arg((qulonglong)ibB)), "clicking an input bus's name selects it");
        click(kid<QToolButton>(&mixer, "deleteBus"));
        uitest::settle(80);
        check(st().project().inputBuses.size() == 1 && st().project().inputBuses[0].id == ibA && st().undoName() == "Delete Input Bus" && st().project().buses.size() == 1,
              "Delete Bus deletes the selected input bus (not a bus)");
        check(trackOf(t1)->recordInput.inputBus == 0, "a track that recorded from it records from its own input again");
        st().undo();
        uitest::settle(60);
        check(st().project().inputBuses.size() == 2 && trackOf(t1)->recordInput.inputBus == ibB, "and Undo brings both back");
    }

    // ================================================================================== split master faders
    {
        QWidget* ms = mixer.findChild<QWidget*>("strip:master");
        QToolButton* split = kid<QToolButton>(ms, "masterSplit");
        QWidget* fader = kid<QWidget>(ms, "fader");
        check(split && fader, "the master strip has an L/R split button");
        st().edit("Master", [](Project& p) { p.masterVolume = 1.0; p.masterVolumeR = -1.0; });
        rectOf(fader, "cap");
        check(!fader->property("split").toBool(), "linked: one fader");
        click(split);
        uitest::settle(30);
        check(st().project().masterVolumeR == st().project().masterVolume && st().project().masterVolumeR >= 0 && st().undoName() == "Split Master Fader", "L/R splits it: the right fader starts where the left is");
        const QRect cap = rectOf(fader, "cap"), cap2 = rectOf(fader, "cap2");
        check(fader->property("split").toBool() && cap2.isValid() && cap2.center().x() > cap.center().x() + 10 && cap2.center().y() == cap.center().y(), "two faders side by side, both at 0 dB");
        uitest::mouseDrag(fader, cap2.center(), cap2.center() + QPoint(0, -40));
        check(st().project().masterVolumeR > 1.3 && st().project().masterVolume == 1.0 && st().undoName() == "Master Volume", "dragging the right one turns up only the right channel",
              strfmt("%.3f %.3f", st().project().masterVolume, st().project().masterVolumeR));
        const QRect capL = rectOf(fader, "cap");
        uitest::mouseDrag(fader, capL.center(), capL.center() + QPoint(0, 40));
        check(st().project().masterVolume < 0.8 && st().project().masterVolumeR > 1.3, "and the left one only the left", strfmt("%.3f %.3f", st().project().masterVolume, st().project().masterVolumeR));
        uitest::mouseDoubleClick(fader, rectOf(fader, "cap2").center());
        check(st().project().masterVolumeR == 1.0 && st().project().masterVolume < 0.8, "double-click puts the right one back to 0 dB");
        uitest::shot(ms, gOut + "/console_master_split.png");
        click(split);
        uitest::settle(30);
        check(st().project().masterVolumeR < 0 && !rectOf(fader, "cap").isNull() && !fader->property("split").toBool(), "L/R again links them: one fader (the left one's value for both)");
    }

    // ================================================================================== the Preview bus
    {
        QToolButton* show = kid<QToolButton>(&mixer, "showPreview");
        check(show && show->isEnabled() && !show->isChecked(), "the Preview toggle works (off at first)");
        QWidget* prev = mixer.findChild<QWidget*>("strip:preview");
        check(prev && !prev->isVisible(), "no Preview strip until it is shown");
        mixer.handleKey(Qt::Key_P, Qt::NoModifier);
        uitest::settle(30);
        check(show->isChecked() && prev->isVisible(), "P shows the Preview bus's strip");
        const bool could = st().canUndo();
        const std::string undoWas = st().undoName();
        QWidget* fader = kid<QWidget>(prev, "fader");
        const QRect cap = rectOf(fader, "cap");
        previewBus().gain.store(1.0f);
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, 50));
        check(previewBus().gain.load() < 0.8f && previewBus().gain.load() > 0.0f, "dragging its fader changes the Preview volume", strfmt("%.3f", previewBus().gain.load()));
        check(st().canUndo() == could && st().undoName() == undoWas, "which is not part of the project (no undo step)");
        uitest::mouseDoubleClick(fader, rectOf(fader, "cap").center());
        check(previewBus().gain.load() == 1.0f, "double-click puts it back to 0 dB");
        click(kid<QToolButton>(prev, "mute"));
        check(previewBus().mute.load(), "M mutes the Preview bus");
        float l[4] = {1, 1, 1, 1}, r[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        previewBus().process(l, r, 4);
        check(l[0] == 0.f && r[0] == 0.f, "muted, what a preview plays is silent");
        click(kid<QToolButton>(prev, "mute"));
        previewBus().gain.store(0.5f);
        float l2[4] = {1, 1, 1, 1}, r2[4] = {0.5f, 0.5f, 0.5f, 0.5f};
        previewBus().peakL.store(0.f);
        previewBus().peakR.store(0.f);
        previewBus().process(l2, r2, 4);
        check(near(l2[0], 0.5, 1e-6) && near(r2[0], 0.25, 1e-6), "the fader scales what a preview plays");
        check(near(previewBus().peakL.load(), 0.5, 1e-6) && near(previewBus().peakR.load(), 0.25, 1e-6), "and the peaks are noted for the meter");
        uitest::settle(120);
        uitest::shot(&mixer, gOut + "/console_preview.png");
        previewBus().gain.store(1.0f);
        mixer.handleKey(Qt::Key_P, Qt::NoModifier);
        uitest::settle(30);
        check(!prev->isVisible(), "P again hides it");
    }

    // ================================================================================== recording from each track's own input
    {
        uint64_t a1 = 0, a2 = 0;
        freshProject(a1, a2);
        Recorder rec(&pb);
        rec.inputFactory = [] { return std::unique_ptr<AudioInput>(new DevInput()); };
        st().edit("Inputs", [&](Project& p) {
            RecordInput r1;
            r1.device = "micA";
            r1.first = 0;
            r1.channels = 1;
            p.findTrack(a1)->recordInput = r1;
            RecordInput r2;
            r2.device = "micB";
            r2.first = 2;
            r2.channels = 2;
            p.findTrack(a2)->recordInput = r2;
        });
        rec.setArmed(a1, true);
        rec.setArmed(a2, true);
        check(rec.inputOpen(), "arming opens the inputs");
        std::string e;
        check(rec.start(&e), "recording starts with two tracks armed on different inputs", e);
        uitest::settle(900);
        rec.stop();
        uitest::settle(100);
        check(rec.lastFiles().size() == 2, "one file for each input", strfmt("%d", (int)rec.lastFiles().size()));
        const Track* x1 = trackOf(a1);
        const Track* x2 = trackOf(a2);
        check(x1 && x2 && x1->events.size() == 1 && x2->events.size() == 1, "and an event on each armed track");
        float l = 0, r = 0;
        if (x1 && !x1->events.empty() && peaksOf(x1->events[0], l, r)) {
            check(near(l, 0.1) && near(r, 0.1), "the first track recorded Mic A's first channel, mono", strfmt("%.3f %.3f", l, r));
            const MediaItem* mi = st().project().findMedia(x1->events[0].mediaId);
            std::string er;
            std::unique_ptr<MediaReader> rd = mi ? openMedia(mi->path, &er) : nullptr;
            check(rd && rd->info().channels == 1, "as a mono file", rd ? strfmt("%d channels", rd->info().channels) : er);
        } else {
            check(false, "the first track's recording could be read");
        }
        if (x2 && !x2->events.empty() && peaksOf(x2->events[0], l, r)) {
            check(near(l, 0.5) && near(r, 0.55), "the second recorded Interface B's inputs 3 + 4, stereo", strfmt("%.3f %.3f", l, r));
        } else {
            check(false, "the second track's recording could be read");
        }
        check(st().undoName() == "Record", "the recording is one undo step");
        // two tracks on the same input share the file
        freshProject(a1, a2);
        st().edit("Inputs", [&](Project& p) {
            RecordInput r;
            r.device = "micB";
            r.first = 0;
            r.channels = 2;
            p.findTrack(a1)->recordInput = r;
            p.findTrack(a2)->recordInput = r;
        });
        rec.setArmed(a1, true);
        rec.setArmed(a2, true);
        check(rec.start(&e), "recording starts", e);
        uitest::settle(500);
        rec.stop();
        uitest::settle(100);
        check(rec.lastFiles().size() == 1 && trackOf(a1)->events.size() == 1 && trackOf(a2)->events.size() == 1 && trackOf(a1)->events[0].mediaId == trackOf(a2)->events[0].mediaId,
              "tracks on the same input share one file", strfmt("%d files", (int)rec.lastFiles().size()));
        rec.setArmed(a1, false);
        rec.setArmed(a2, false);

        // ---------------------------------------------------------------- takes: recording into a selected event
        freshProject(a1, a2);
        const uint64_t media = st().importMedia("synthaudio:1", &err);
        st().addMediaToTimeline(media, 0.0, a1);
        uint64_t ev = 0;
        st().edit("Short", [&](Project& p) {
            Event& x = p.findTrack(a1)->events[0];
            x.length = 0.8;
            ev = x.id;
        });
        st().setSelection(std::set<uint64_t>{ev});
        st().edit("Input", [&](Project& p) { p.findTrack(a1)->recordInput.device = "micA"; });
        rec.setArmed(a1, true);
        st().setCursor(0.0);
        check(rec.start(&e), "recording into a selected event starts", e);
        for (int k = 0; k < 60 && rec.recording(); ++k) uitest::settle(50);
        uitest::settle(150);
        const Event* te = st().project().findEvent(ev);
        check(!rec.recording(), "it stops by itself at the event's end");
        check(te && te->takes.size() == 1 && te->activeTake == 1 && te->mediaId != media && te->takes[0].mediaId == media && near(te->start, 0.0, 1e-6),
              "the recording is a new take of the event; the old sound is its first take", te ? strfmt("%d takes, active %d", (int)te->takes.size(), te->activeTake) : "gone");
        check(te && near(te->length, 0.8, 0.05) && trackOf(a1)->events.size() == 1, "the event keeps its place and length", te ? strfmt("%.3f", te->length) : "");
        // switching takes (T) brings the old sound back
        st().edit("Take", [&](Project& p) { AppState::setActiveTake(*p.findEvent(ev), 0); });
        te = st().project().findEvent(ev);
        check(te && te->mediaId == media, "T / the take menu switch back to the old take");

        // ---------------------------------------------------------------- punch-in: a time selection
        freshProject(a1, a2);
        st().edit("Input", [&](Project& p) { p.findTrack(a1)->recordInput.device = "micA"; });
        rec.setArmed(a1, true);
        st().setLoop(1.0, 1.6);
        st().loopOn = false;
        st().setCursor(0.2);
        check(rec.start(&e), "punch-in: recording with a time selection starts", e);
        for (int k = 0; k < 60 && rec.recording(); ++k) uitest::settle(50);
        uitest::settle(150);
        const Track* pt = trackOf(a1);
        check(!rec.recording() && pt && pt->events.size() == 1 && near(pt->events[0].start, 1.0, 0.06) && near(pt->events[0].length, 0.6, 0.1),
              "it records only inside the selection: the new event is the selection's", pt && !pt->events.empty() ? strfmt("%.3f +%.3f", pt->events[0].start, pt->events[0].length) : "no event");
        rec.setArmed(a1, false);

        // ---------------------------------------------------------------- loop passes make takes
        freshProject(a1, a2);
        const uint64_t media2 = st().importMedia("synthaudio:1", &err);
        st().addMediaToTimeline(media2, 0.0, a1);
        uint64_t ev2 = 0;
        st().edit("Short", [&](Project& p) {
            Event& x = p.findTrack(a1)->events[0];
            x.length = 0.5;
            ev2 = x.id;
        });
        st().setSelection(std::set<uint64_t>{ev2});
        st().edit("Input", [&](Project& p) { p.findTrack(a1)->recordInput.device = "micA"; });
        rec.setArmed(a1, true);
        st().setLoop(0.0, 0.5);
        st().loopOn = true;
        st().setCursor(0.0);
        check(rec.start(&e), "loop recording starts", e);
        uitest::settle(1700);
        rec.stop();
        uitest::settle(150);
        const Event* le = st().project().findEvent(ev2);
        check(le && le->takes.size() >= 3 && le->activeTake == (int)le->takes.size(), "each pass of the loop is a take; the last is active",
              le ? strfmt("%d takes, active %d", (int)le->takes.size(), le->activeTake) : "gone");
        st().loopOn = false;
        rec.setArmed(a1, false);

        // ---------------------------------------------------------------- Recorded Files with several inputs
        freshProject(a1, a2);
        vprefs::set("prefs/promptKeepRecorded", true);
        mixer.show();
        mixer.raise();
        mixer.activateWindow();
        uitest::settle(100);
        check(QApplication::activeWindow() != nullptr, "(a window is active, so the dialog can be shown)");
        st().edit("Inputs", [&](Project& p) {
            p.findTrack(a1)->recordInput.device = "micA";
            p.findTrack(a2)->recordInput.device = "micB";
        });
        rec.setArmed(a1, true);
        rec.setArmed(a2, true);
        int listed = -1;
        QTimer poll;
        poll.setInterval(40);
        QObject::connect(&poll, &QTimer::timeout, [&] {
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->objectName() == "recordedFilesDialog" && w->isVisible()) {
                    QListWidget* list = w->findChild<QListWidget*>("recordedFiles");
                    listed = list ? list->count() : -2;
                    if (QPushButton* done = w->findChild<QPushButton*>("done")) done->click();
                }
        });
        poll.start();
        check(rec.start(&e), "recording two inputs with Prompt to keep files on", e);
        uitest::settle(500);
        rec.stop();
        uitest::settle(100);
        poll.stop();
        check(listed == 2, "the Recorded Files dialog lists a file for each input", strfmt("%d", listed));
        check(trackOf(a1)->events.size() == 1 && trackOf(a2)->events.size() == 1, "and Done keeps both");
        vprefs::set("prefs/promptKeepRecorded", false);
        rec.setArmed(a1, false);
        rec.setArmed(a2, false);
    }

    setInputDevicesForTest(nullptr);
    return uitest::summary();
}
