// Tests and pictures for the Mixing Console and the Master Bus panel (src/app/mixer.cpp).
//
//   mixer_test <output folder>
//
// Build: uibuild mixer_test tests/ui/mixer_test.cpp mixer src/app/surroundpanner.cpp (the strips' small surround panners are there)
//
// QT_QPA_PLATFORM=offscreen. The folder gets the pictures.
#include "../../src/app/mixer.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QComboBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QSlider>
#include <QDir>
#include <QPalette>
#include <QStyleFactory>
#include <QToolButton>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

// The main window's dark theme (as applyTheme() in mainwindow.cpp sets it).
void theme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(220, 220, 220), mid(62, 62, 66), accent(0, 122, 204);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(38, 38, 41));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(p);
}

const Track* track(uint64_t id)
{
    for (const auto& t : AppState::get().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

QWidget* strip(QWidget* mixer, const QString& name) { return mixer->findChild<QWidget*>(name); }
QWidget* trackStrip(QWidget* mixer, uint64_t id) { return strip(mixer, QString("strip:track:%1").arg(id)); }
QWidget* busStrip(QWidget* mixer, uint64_t id) { return strip(mixer, QString("strip:bus:%1").arg(id)); }

template <class T>
T* kid(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

// Paints a widget (so the positions it publishes for tests are current) and returns one of them.
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

int countStrips(QWidget* mixer, const QString& prefix)
{
    int n = 0;
    for (QWidget* w : mixer->findChildren<QWidget*>())
        if (w->objectName().startsWith(prefix) && w->isVisibleTo(mixer)) ++n;
    return n;
}

// Undoes once and says whether that put 'get' back to 'before' (then redoes): the drag was one undo step.
bool oneStep(const std::function<double()>& get, double before, const std::string& name)
{
    AppState& app = AppState::get();
    if (app.undoName() != name) return false;
    const double after = get();
    app.undo();
    const bool ok = std::fabs(get() - before) < 1e-12;
    app.redo();
    return ok && std::fabs(get() - after) < 1e-12;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    gOut = argc > 1 ? QString(argv[1]) : QString("mixer_out");
    QDir().mkpath(gOut);
    theme();
    QSettings().remove("mixer");            // the console's view settings start as VEGAS's defaults
    uitest::setup({}, {});
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t m1 = app.importMedia("synthaudio:1", &err), m2 = app.importMedia("synthaudio:2", &err), m3 = app.importMedia("synthaudio:3", &err);
    app.addMediaToTimeline(m1, 0.0);
    const uint64_t t2 = app.addTrack(TrackType::Audio), t3 = app.addTrack(TrackType::Audio);
    app.addMediaToTimeline(m2, 0.0, t2);
    app.addMediaToTimeline(m3, 0.0, t3);
    uint64_t t1 = 0;
    for (const auto& t : app.project().tracks)
        if (t.id != t2 && t.id != t3) t1 = t.id;
    app.edit("Names", [&](Project& p) {
        p.findTrack(t1)->name = "Music";
        p.findTrack(t2)->name = "Voice";
        p.findTrack(t3)->name = "Sound Effects";
    });

    Playback pb;
    MixerWindow mixer(&pb);
    std::vector<std::pair<int, uint64_t>> opened;
    mixer.onOpenFx = [&](const FxTarget& t, uint64_t) { opened.push_back(std::make_pair((int)t.kind, t.id)); };
    mixer.resize(980, 600);
    mixer.show();
    MasterBusWidget masterBus(&pb);
    masterBus.onOpenFx = [&](const FxTarget& t, uint64_t) { opened.push_back(std::make_pair((int)t.kind, t.id)); };
    masterBus.resize(170, 460);
    masterBus.show();
    uitest::settle(150);

    check(trackStrip(&mixer, t1) && trackStrip(&mixer, t2) && trackStrip(&mixer, t3) && strip(&mixer, "strip:master"), "a strip for every audio track and the master");

    // ---------------------------------------------------------------- buses
    click(kid<QToolButton>(&mixer, "insertBus"));
    uitest::settle(50);
    click(kid<QToolButton>(&mixer, "insertBus"));
    uitest::settle(80);
    const Project& pr = app.project();
    check(pr.buses.size() == 2 && pr.buses[0].name == "Bus A" && pr.buses[1].name == "Bus B" && app.undoName() == "Insert Bus", "Insert Bus adds Bus A and Bus B");
    const uint64_t busA = pr.buses.size() > 0 ? pr.buses[0].id : 0, busB = pr.buses.size() > 1 ? pr.buses[1].id : 0;
    check(busStrip(&mixer, busA) && busStrip(&mixer, busB), "and they get strips");
    QWidget* s1 = trackStrip(&mixer, t1);
    check(s1 && s1->findChild<QWidget*>(QString("send:%1").arg(busA)) && s1->findChild<QWidget*>(QString("send:%1").arg(busB)), "track strips get a send for each bus");

    // ---------------------------------------------------------------- the fader
    {
        QWidget* fader = kid<QWidget>(s1, "fader");
        QRect cap = rectOf(fader, "cap");
        const double before = track(t1)->level;
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, -40));
        const double up = track(t1)->level;
        check(up > before * 1.3, "dragging a track's fader up turns the track up", strfmt("%.3f -> %.3f", before, up));
        check(oneStep([&] { return track(t1)->level; }, before, "Track Volume"), "and the drag is one undo step");
        cap = rectOf(fader, "cap");
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, 40), 8, Qt::ControlModifier);
        const double fine = track(t1)->level;
        check(fine < up && fine > before * 1.1, "Ctrl makes it finer", strfmt("%.3f -> %.3f", up, fine));
        cap = rectOf(fader, "cap");
        uitest::mouseDoubleClick(fader, cap.center());
        check(track(t1)->level == 1.0, "double-click puts it back to 0 dB", strfmt("%.4f", track(t1)->level));
        // the 0 dB detent: a drag that ends a hair away from 0 dB lands on it
        cap = rectOf(fader, "cap");
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, -30));
        cap = rectOf(fader, "cap");
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, 31));
        check(std::fabs(track(t1)->level - 1.0) < 1e-12, "0 dB catches the fader", strfmt("%.4f", track(t1)->level));
        // dragged all the way down: silence
        cap = rectOf(fader, "cap");
        uitest::mouseDrag(fader, cap.center(), cap.center() + QPoint(0, 600));
        check(track(t1)->level == 0.0, "all the way down is -Inf", strfmt("%.5f", track(t1)->level));
        uitest::mouseDoubleClick(fader, rectOf(fader, "cap").center());
        // a change from elsewhere (the timeline's header) moves the fader
        const int y0 = rectOf(fader, "cap").center().y();
        app.edit("Track Volume", [&](Project& p) { p.findTrack(t1)->level = 0.25; });
        uitest::settle(30);
        const int y1 = rectOf(fader, "cap").center().y();
        check(y1 > y0 + 20, "the fader follows changes made elsewhere", strfmt("%d -> %d", y0, y1));
        app.edit("Track Volume", [&](Project& p) { p.findTrack(t1)->level = 1.0; });
    }

    // ---------------------------------------------------------------- pan, mute, solo
    {
        QWidget* pan = kid<QWidget>(s1, "pan");
        rectOf(pan, "groove");
        const int kx = pan->property("knobX").toInt();
        const QRect g = pan->property("groove").toRect();
        uitest::mouseDrag(pan, QPoint(kx, g.center().y()), QPoint(kx + g.width() / 4, g.center().y()));
        const double p1 = track(t1)->pan;
        check(p1 > 0.4 && p1 < 0.6, "dragging the pan knob right pans right", strfmt("%.3f", p1));
        check(oneStep([&] { return track(t1)->pan; }, 0.0, "Track Pan"), "and is one undo step");
        uitest::mouseDoubleClick(pan, QPoint(pan->property("knobX").toInt(), g.center().y()));
        check(track(t1)->pan == 0.0, "double-click centres it");

        QToolButton* mute = kid<QToolButton>(s1, "mute");
        click(mute);
        check(track(t1)->mute && mute->isChecked(), "M mutes the track");
        app.edit("Mute Track", [&](Project& p) { p.findTrack(t1)->mute = false; });
        uitest::settle(20);
        check(!mute->isChecked(), "and follows the timeline's mute");
        QToolButton* solo = kid<QToolButton>(trackStrip(&mixer, t2), "solo");
        click(solo);
        check(track(t2)->solo && solo->isChecked(), "S solos the track");
        click(solo);
        QToolButton* busSolo = kid<QToolButton>(busStrip(&mixer, busA), "solo");
        click(busSolo);
        check(app.project().findBus(busA)->solo, "a bus can be soloed");
        click(busSolo);
        QToolButton* busMute = kid<QToolButton>(busStrip(&mixer, busB), "mute");
        click(busMute);
        check(app.project().findBus(busB)->mute && busMute->isChecked(), "and muted");
        click(busMute);
    }

    // ---------------------------------------------------------------- routing
    {
        QComboBox* out = kid<QComboBox>(trackStrip(&mixer, t2), "output");
        check(out && out->count() == 3 && out->itemText(0) == "Master" && out->itemText(1) == "Bus A", "a track's output menu lists the master and the buses",
              out ? strfmt("%d items", out->count()) : std::string());
        if (out) {
            out->setCurrentIndex(1);
            out->activated(1);
            uitest::settle(50);
        }
        check(track(t2)->output == busA && app.undoName() == "Track Output", "choosing Bus A routes the track to it");
        QComboBox* outA = kid<QComboBox>(busStrip(&mixer, busA), "output");
        if (outA) {
            outA->setCurrentIndex(1);
            outA->activated(1);
            uitest::settle(50);
        }
        check(app.project().findBus(busA)->output == busB, "a bus can go into another bus");
        QComboBox* outB = kid<QComboBox>(busStrip(&mixer, busB), "output");
        check(outB && outB->count() == 1, "and that one is not offered a way back into it (no loops)", outB ? strfmt("%d items", outB->count()) : std::string());

        // sends
        QWidget* send = trackStrip(&mixer, t3)->findChild<QWidget*>(QString("send:%1").arg(busB));
        const QRect bar = rectOf(send, "bar");
        uitest::mouseDrag(send, QPoint(bar.left() + 2, bar.center().y()), QPoint(bar.left() + bar.width() * 3 / 4, bar.center().y()));
        const Track* tr3 = track(t3);
        // VEGAS: a send to a bus is taken before the track's volume until switched; one to an assignable FX chain after it
        check(tr3->sends.size() == 1 && tr3->sends[0].bus == busB && tr3->sends[0].level > 0.1 && tr3->sends[0].preFader && app.undoName() == "Send Level",
              "dragging a send's level makes a pre-fader send", tr3->sends.empty() ? std::string("none") : strfmt("%.3f", tr3->sends[0].level));
        app.undo();
        check(track(t3)->sends.empty() || track(t3)->sends[0].level == 0, "in one undo step");
        app.redo();
        uitest::mouseClick(send, rectOf(send, "prePost").center());
        check(!track(t3)->sends.empty() && !track(t3)->sends[0].preFader, "Pre/Post switches it to post-fader");
    }

    // ---------------------------------------------------------------- effects buttons
    {
        opened.clear();
        click(kid<QToolButton>(trackStrip(&mixer, t1), "fx"));
        click(kid<QToolButton>(busStrip(&mixer, busA), "fx"));
        click(kid<QToolButton>(strip(&mixer, "strip:master"), "fx"));
        click(kid<QToolButton>(&masterBus, "masterFx"));
        check(opened.size() == 4 && opened[0] == std::make_pair((int)FxTarget::Track, t1) && opened[1] == std::make_pair((int)FxTarget::Bus, busA) &&
                  opened[2].first == (int)FxTarget::AudioOut && opened[3].first == (int)FxTarget::AudioOut,
              "the FX buttons open the track's, the bus's and the master's effects");
        // a strip with effects shows them on its button
        app.edit("Add FX", [&](Project& p) {
            FxSlot s;
            s.id = p.newId();
            s.kind = "audio";
            s.name = "Reverb";
            p.findBus(busA)->fx.push_back(s);
        });
        uitest::settle(30);
        QToolButton* fxA = kid<QToolButton>(busStrip(&mixer, busA), "fx");
        check(fxA && fxA->text().startsWith("Reverb") && fxA->property("active").toBool(), "and is lit when there are effects", fxA ? fxA->text().toStdString() : std::string());
    }

    // ---------------------------------------------------------------- the master
    {
        QWidget* mf = kid<QWidget>(strip(&mixer, "strip:master"), "fader");
        const QRect cap = rectOf(mf, "cap");
        uitest::mouseDrag(mf, cap.center(), cap.center() + QPoint(0, 50));
        const double mv = app.project().masterVolume;
        check(mv < 0.6 && oneStep([&] { return app.project().masterVolume; }, 1.0, "Master Volume"), "the master strip's fader sets the master volume in one step", strfmt("%.3f", mv));
        QWidget* bf = kid<QWidget>(&masterBus, "masterFader");
        const QRect bcap = rectOf(bf, "cap");
        check(std::abs(bcap.center().y() - bf->property("faderTop").toInt()) > 40, "the Master Bus panel's fader shows it too");
        uitest::mouseDoubleClick(bf, bcap.center());
        check(app.project().masterVolume == 1.0, "and puts it back to 0 dB with a double-click");
        click(kid<QToolButton>(&masterBus, "masterMute"));
        uitest::settle(20);
        check(app.project().masterMute && kid<QToolButton>(strip(&mixer, "strip:master"), "mute")->isChecked(), "the panel's M mutes the master, and the strip shows it");
        click(kid<QToolButton>(strip(&mixer, "strip:master"), "mute"));
        check(!app.project().masterMute && !kid<QToolButton>(&masterBus, "masterMute")->isChecked(), "and the strip's M unmutes it");
    }

    // ---------------------------------------------------------------- strips follow the project
    {
        const uint64_t t4 = app.addTrack(TrackType::Audio);
        uitest::settle(50);
        check(trackStrip(&mixer, t4) != nullptr, "a new audio track gets a strip");
        app.edit("Rename", [&](Project& p) { p.findTrack(t4)->name = "Dialogue"; });
        uitest::settle(30);
        QWidget* hdr = trackStrip(&mixer, t4) ? trackStrip(&mixer, t4)->childAt(QPoint(30, 12)) : nullptr;
        check(hdr && hdr->toolTip() == "Dialogue", "a renamed track's strip shows the new name");
        app.removeTrack(t4);
        uitest::settle(50);
        check(trackStrip(&mixer, t4) == nullptr, "a deleted track's strip goes");
        app.undo();
        uitest::settle(50);
        check(trackStrip(&mixer, t4) != nullptr, "and comes back with undo");
        app.removeTrack(t4);
        uitest::settle(50);
        // show / hide by kind
        click(kid<QToolButton>(&mixer, "showBuses"));
        uitest::settle(20);
        check(countStrips(&mixer, "strip:bus:") == 0 && countStrips(&mixer, "strip:track:") == 3, "buses' strips can be hidden");
        click(kid<QToolButton>(&mixer, "showBuses"));
        click(kid<QToolButton>(&mixer, "showMaster"));
        uitest::settle(20);
        check(!strip(&mixer, "strip:master")->isVisibleTo(&mixer), "and the master's");
        click(kid<QToolButton>(&mixer, "showMaster"));
        uitest::settle(20);
    }

    // ---------------------------------------------------------------- meters
    {
        app.edit("Levels", [&](Project& p) {
            p.findTrack(t1)->level = 0.7;
            p.findTrack(t1)->sends.clear();
            Send s;
            s.bus = busA;
            s.level = 0.6;
            p.findTrack(t1)->sends.push_back(s);
            p.findBus(busA)->fx.clear();
            p.findTrack(t3)->pan = -0.6;
        });
        app.setCursor(0.5);
        pb.play();
        uitest::settle(700);
        QWidget* f1 = kid<QWidget>(trackStrip(&mixer, t1), "fader");
        QWidget* fa = kid<QWidget>(busStrip(&mixer, busA), "fader");
        QWidget* fm = kid<QWidget>(strip(&mixer, "strip:master"), "fader");
        QWidget* fb = kid<QWidget>(&masterBus, "masterFader");
        const float l1 = f1->property("levelLeft").toFloat(), la = fa->property("levelLeft").toFloat(), lm = fm->property("levelLeft").toFloat(),
                    lb = fb->property("levelLeft").toFloat();
        check(pb.playing() && l1 > 0.05f && la > 0.02f && lm > 0.05f && lb > 0.05f, "the meters move while playing",
              strfmt("track %.3f bus %.3f master %.3f panel %.3f", l1, la, lm, lb));
        const float h1 = f1->property("holdLeft").toFloat();
        check(h1 >= l1, "and hold their peaks", strfmt("%.3f / %.3f", h1, l1));
        QWidget* f3 = kid<QWidget>(trackStrip(&mixer, t3), "fader");
        const float r3 = f3->property("levelRight").toFloat(), l3 = f3->property("levelLeft").toFloat();
        check(l3 > r3 * 1.5, "a track panned left shows more on the left", strfmt("%.3f / %.3f", l3, r3));
        uitest::shot(&mixer, gOut + "/mixer_playing.png");
        uitest::shot(&masterBus, gOut + "/masterbus_playing.png");
        pb.stop();
        uitest::settle(100);
        check(f1->property("levelLeft").toFloat() == 0.f && fb->property("levelLeft").toFloat() == 0.f, "and drop to nothing when stopped");
    }

    // ---------------------------------------------------------------- delete bus
    {
        QWidget* sa = busStrip(&mixer, busA);
        QWidget* hdr = sa ? sa->childAt(QPoint(40, 12)) : nullptr;
        if (hdr) uitest::mouseClick(hdr, QPoint(10, 10));
        uitest::settle(20);
        click(kid<QToolButton>(&mixer, "deleteBus"));
        uitest::settle(80);
        const Project& p = app.project();
        bool routed = false;
        for (const auto& t : p.tracks) {
            if (t.output == busA) routed = true;
            for (const auto& s : t.sends)
                if (s.bus == busA) routed = true;
        }
        check(p.buses.size() == 1 && p.buses[0].id == busB && !routed && app.undoName() == "Delete Bus", "Delete Bus deletes the selected bus");
        check(track(t2)->output == 0, "and what went to it goes to the master");
        check(busStrip(&mixer, busA) == nullptr && countStrips(&mixer, "strip:bus:") == 1, "its strip goes");
        app.undo();
        uitest::settle(50);
        check(app.project().buses.size() == 2 && track(t2)->output == busA && busStrip(&mixer, busA), "undo brings it back with its routing");
    }

    // ---------------------------------------------------------------- pictures
    {
        // a still picture with meters lit from a moment of playing, and a narrow one that scrolls
        app.edit("Tidy", [&](Project& p) {
            p.findTrack(t2)->pan = 0.35;
            p.findBus(busA)->level = 0.7;
        });
        app.setCursor(1.0);
        pb.play();
        uitest::settle(500);
        uitest::shot(&mixer, gOut + "/mixer.png");
        uitest::shot(&masterBus, gOut + "/masterbus.png");
        mixer.resize(520, 520);
        uitest::settle(80);
        uitest::shot(&mixer, gOut + "/mixer_narrow.png");
        masterBus.resize(150, 300);
        uitest::settle(80);
        uitest::shot(&masterBus, gOut + "/masterbus_small.png");
        pb.stop();
        uitest::settle(80);
        mixer.resize(980, 600);
        uitest::settle(80);
        uitest::shot(&mixer, gOut + "/mixer_stopped.png");
    }

    // ---------------------------------------------------------------- VEGAS's view options and keys
    {
        mixer.resize(1180, 640);
        mixer.setFocus();
        uitest::settle(60);
        QWidget* s1w = trackStrip(&mixer, t1);
        auto key = [&](int k, Qt::KeyboardModifiers m = Qt::NoModifier) {
            const bool ok = mixer.handleKey(k, m);
            uitest::settle(30);
            return ok;
        };
        // channel width
        const int w0 = s1w->width();
        key(Qt::Key_N);
        const int wn = s1w->width();
        key(Qt::Key_W);
        const int ww = s1w->width();
        key(Qt::Key_D);
        check(w0 == 100 && wn < 80 && ww > 120 && s1w->width() == 100, "N, W and D make the channels narrow, wide and default", strfmt("%d %d %d %d", w0, wn, ww, s1w->width()));
        check(mixer.findChild<QSlider*>("channelWidth")->value() == 1, "and the Channel Width slider shows it");
        key(Qt::Key_N);
        uitest::shot(&mixer, gOut + "/mixer_narrow_channels.png");
        key(Qt::Key_D);
        // control regions
        QWidget* fxRegion = kid<QWidget>(s1w, "region:fx");
        QWidget* sendRegion = kid<QWidget>(s1w, "region:sends");
        QWidget* ioRegion = kid<QWidget>(s1w, "region:io");
        QWidget* faderRegion = kid<QWidget>(s1w, "region:faders");
        QWidget* fader = kid<QWidget>(s1w, "fader");
        bool allShown = fxRegion->isVisibleTo(&mixer) && sendRegion->isVisibleTo(&mixer) && ioRegion->isVisibleTo(&mixer) && faderRegion->isVisibleTo(&mixer);
        key(Qt::Key_I);
        key(Qt::Key_S);
        key(Qt::Key_H);
        check(allShown && !fxRegion->isVisibleTo(&mixer) && !sendRegion->isVisibleTo(&mixer) && !ioRegion->isVisibleTo(&mixer), "I, S and H hide the Insert FX, Sends and I/O regions");
        check(!mixer.findChild<QToolButton*>("regionFx")->isChecked(), "and the View Controls show it");
        key(Qt::Key_F);
        check(!faderRegion->isVisibleTo(&mixer) && fader->isVisibleTo(&mixer) && !fader->property("cap").isNull(), "F hides the faders (the meters stay)");
        key(Qt::Key_M);
        check(!fader->isVisibleTo(&mixer), "and M the meters");
        uitest::shot(&mixer, gOut + "/mixer_regions_off.png");
        key(Qt::Key_M);
        key(Qt::Key_F);
        key(Qt::Key_I);
        key(Qt::Key_S);
        key(Qt::Key_H);
        check(fxRegion->isVisibleTo(&mixer) && sendRegion->isVisibleTo(&mixer) && ioRegion->isVisibleTo(&mixer) && faderRegion->isVisibleTo(&mixer) && fader->isVisibleTo(&mixer),
              "and pressed again they come back");
        // labels and ticks
        key(Qt::Key_L, Qt::ShiftModifier);
        check(kid<QWidget>(s1w, "label:fx")->isVisibleTo(&mixer) && kid<QWidget>(s1w, "label:sends")->isVisibleTo(&mixer), "Shift+L shows the control regions' labels");
        key(Qt::Key_T, Qt::ShiftModifier);
        uitest::shot(&mixer, gOut + "/mixer_labels_noticks.png");
        key(Qt::Key_L, Qt::ShiftModifier);
        key(Qt::Key_T, Qt::ShiftModifier);
        check(!kid<QWidget>(s1w, "label:fx")->isVisibleTo(&mixer), "and again hides them");
        // kinds of channels
        key(Qt::Key_U);
        check(countStrips(&mixer, "strip:bus:") == 0, "U hides the buses");
        key(Qt::Key_A);
        check(countStrips(&mixer, "strip:track:") == 0, "A hides the audio tracks");
        key(Qt::Key_T);
        check(!strip(&mixer, "strip:master")->isVisibleTo(&mixer), "T hides the master");
        key(Qt::Key_Q);
        check(countStrips(&mixer, "strip:bus:") == 2 && countStrips(&mixer, "strip:track:") == 3 && strip(&mixer, "strip:master")->isVisibleTo(&mixer), "Q shows every channel");
        // selecting, hiding one, the Channel List
        QWidget* hdr2 = trackStrip(&mixer, t2)->childAt(QPoint(40, 12));
        uitest::mouseClick(hdr2, QPoint(10, 10));
        check(mixer.property("selectedStrips").toStringList() == QStringList{QString("strip:track:%1").arg(t2)}, "a click on a channel's name selects it",
              mixer.property("selectedStrips").toStringList().join(",").toStdString());
        QWidget* hdr3 = trackStrip(&mixer, t3)->childAt(QPoint(40, 12));
        uitest::mouseClick(hdr3, QPoint(10, 10), Qt::LeftButton, Qt::ControlModifier);
        check(mixer.property("selectedStrips").toStringList().size() == 2, "Ctrl+click adds one");
        // Up / Down: the selected faders half a decibel
        const double v2 = track(t2)->level, v3 = track(t3)->level;
        key(Qt::Key_Up);
        const double up2 = track(t2)->level, up3 = track(t3)->level;
        check(std::fabs(20 * std::log10(up2 / v2) - 0.5) < 1e-6 && std::fabs(20 * std::log10(up3 / v3) - 0.5) < 1e-6 && app.undoName() == "Track Volume",
              "Up turns the selected channels up half a decibel, in one undo step", strfmt("%.4f %.4f", up2, up3));
        key(Qt::Key_Down);
        key(Qt::Key_Down);
        check(std::fabs(20 * std::log10(track(t2)->level / v2) + 0.5) < 1e-6, "Down turns them down");
        app.undo();
        app.undo();
        app.undo();
        check(std::fabs(track(t2)->level - v2) < 1e-12, "and each press undoes on its own");
        // Shift+H hides them; the Channel List shows them unticked; ticking brings one back
        key(Qt::Key_H, Qt::ShiftModifier);
        check(!trackStrip(&mixer, t2)->isVisibleTo(&mixer) && !trackStrip(&mixer, t3)->isVisibleTo(&mixer) && trackStrip(&mixer, t1)->isVisibleTo(&mixer),
              "Shift+H hides the selected channels");
        QListWidget* list = mixer.findChild<QListWidget*>("channelList");
        int unticked = 0;
        QListWidgetItem* item2 = nullptr;
        for (int i = 0; list && i < list->count(); ++i) {
            if (list->item(i)->checkState() == Qt::Unchecked) ++unticked;
            if (list->item(i)->data(Qt::UserRole).toString() == QString("strip:track:%1").arg(t2)) item2 = list->item(i);
        }
        check(list && list->count() == 7 && unticked == 2, "the Channel List lists every channel (the Preview bus too), the hidden ones unticked", list ? strfmt("%d / %d", list->count(), unticked) : std::string());
        if (item2) item2->setCheckState(Qt::Checked);
        uitest::settle(30);
        check(trackStrip(&mixer, t2)->isVisibleTo(&mixer), "ticking one shows it again");
        uitest::shot(&mixer, gOut + "/mixer_channel_list.png");
        key(Qt::Key_C, Qt::ShiftModifier);
        check(!mixer.findChild<QWidget*>("channelListPane")->isVisibleTo(&mixer), "Shift+C hides the Channel List");
        key(Qt::Key_R, Qt::ShiftModifier);
        check(!mixer.findChild<QWidget*>("consoleSide")->isVisibleTo(&mixer), "Shift+R the View Controls (and the side goes)");
        key(Qt::Key_C, Qt::ShiftModifier);
        key(Qt::Key_R, Qt::ShiftModifier);
        key(Qt::Key_Q);
        // rename: F2 on the selected channel, a double-click on a bus's name
        uitest::mouseClick(trackStrip(&mixer, t1)->childAt(QPoint(40, 12)), QPoint(10, 10));
        key(Qt::Key_F2);
        QLineEdit* ed = mixer.findChild<QLineEdit*>("renameEdit");
        check(ed && ed->text() == "Music", "F2 edits the selected channel's name");
        if (ed) {
            ed->setText("Score");
            QKeyEvent ret(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\r");
            QApplication::sendEvent(ed, &ret);
            uitest::settle(50);
        }
        check(track(t1)->name == "Score" && app.undoName() == "Rename Track", "Enter renames the track", track(t1)->name);
        QWidget* bh = busStrip(&mixer, busB)->childAt(QPoint(40, 12));
        uitest::mouseDoubleClick(bh, QPoint(10, 10));
        uitest::settle(20);
        ed = mixer.findChild<QLineEdit*>("renameEdit");
        if (ed) {
            ed->setText("Reverb Bus");
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(ed, &esc);
            uitest::settle(50);
        }
        check(ed && app.project().findBus(busB)->name == "Bus B", "a double-click edits a bus's name; Esc leaves it");
        // Ctrl+Q
        const size_t nt = app.project().tracks.size();
        key(Qt::Key_Q, Qt::ControlModifier);
        uitest::settle(60);
        check(app.project().tracks.size() == nt + 1 && app.undoName() == "Insert Audio Track" && countStrips(&mixer, "strip:track:") == 4, "Ctrl+Q inserts an audio track (and its channel)");
        app.undo();
        uitest::settle(60);
        // the effects in a strip: bypass and open
        app.edit("Add FX", [&](Project& p) {
            for (const char* n : {"EQ", "Compressor"}) {
                FxSlot s;
                s.id = p.newId();
                s.kind = "audio";
                s.name = n;
                p.findTrack(t1)->fx.push_back(s);
            }
        });
        uitest::settle(40);
        QWidget* fxs = kid<QWidget>(trackStrip(&mixer, t1), "fxSlots");
        const uint64_t comp = track(t1)->fx.size() == 2 ? track(t1)->fx[1].id : 0;
        uitest::mouseClick(fxs, QPoint(6, 15 + 7));
        check(track(t1)->fx.size() == 2 && track(t1)->fx[1].bypass, "the switch beside an effect bypasses it");
        uitest::mouseClick(fxs, QPoint(6, 15 + 7));
        check(!track(t1)->fx[1].bypass, "and turns it back on");
        opened.clear();
        uitest::mouseClick(fxs, QPoint(50, 15 + 7));
        // an empty row offers the plug-ins as a menu, the whole chain first (VEGAS's Add New Insert FX); handed to the test here
        mixerMenuHookForTests() = [](QMenu* m) -> QAction* {
            for (QAction* a : m->actions())
                if (a->text() == "Plug-In Chain...") return a;
            return nullptr;
        };
        uitest::mouseClick(fxs, QPoint(50, 30 + 7));
        mixerMenuHookForTests() = nullptr;
        check(opened.size() == 2 && opened[0].second == t1 && comp != 0, "a click on an effect's name opens it; on an empty row, the chain");
        // arm for record
        std::vector<uint64_t> armed;
        mixer.onArmTrack = [&](uint64_t id) {
            armed.push_back(id);
            if (app.armedTracks.count(id)) app.armedTracks.erase(id);
            else app.armedTracks.insert(id);
        };
        QToolButton* arm = kid<QToolButton>(trackStrip(&mixer, t2), "arm");
        click(arm);
        check(arm && armed.size() == 1 && armed[0] == t2 && arm->isChecked(), "a track's Arm button arms it for recording");
        click(arm);
        check(!arm->isChecked() && app.armedTracks.empty(), "and disarms it");
        // keys reach the console before the main window's shortcuts (ShortcutOverride)
        QKeyEvent so(QEvent::ShortcutOverride, Qt::Key_M, Qt::NoModifier, "m");
        QApplication::sendEvent(&mixer, &so);
        check(so.isAccepted(), "the console claims its keys while it has the focus");
        QKeyEvent so2(QEvent::ShortcutOverride, Qt::Key_J, Qt::NoModifier, "j");
        QApplication::sendEvent(&mixer, &so2);
        check(!so2.isAccepted(), "and leaves the others to the main window");
        // the settings are kept
        key(Qt::Key_W);
        key(Qt::Key_S);
        QSettings st;
        check(st.value("mixer/width").toInt() == 2 && st.value("mixer/regions").toString() == "101110", "the view settings are kept for next time",
              st.value("mixer/regions").toString().toStdString());
        app.setCursor(1.0);
        pb.play();
        uitest::settle(500);
        uitest::shot(&mixer, gOut + "/mixer_wide_nosends.png");
        pb.stop();
        key(Qt::Key_S);
        key(Qt::Key_D);
        uitest::settle(50);
        uitest::shot(&mixer, gOut + "/mixer_console.png");
    }

    mixer.hide();
    masterBus.hide();
    return uitest::summary();
}
