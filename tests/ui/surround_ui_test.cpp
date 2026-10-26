// 5.1 projects in the UI: the Surround Panner window (src/app/surroundpanner.cpp), the small surround panners of the Mixing
// Console's strips, the six-channel meters of the console's master strip and the Master Bus panel, and Project Properties' Master
// bus mode.
//
//   surround_ui_test <output folder>
//
// Build: uibuild surround_ui_test tests/ui/surround_ui_test.cpp mixer editdialogs src/app/surroundpanner.cpp
// QT_QPA_PLATFORM=offscreen. The folder gets the pictures.
#include "../../src/app/surroundpanner.h"
#include "../../src/app/mixer.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QTabWidget>
#include <QSettings>
#include <QSlider>
#include <QStyleFactory>
#include <QTimer>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

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

const SurroundPan& panOf(uint64_t id)
{
    static SurroundPan none;
    const SurroundPan* sp = surroundPanOf(AppState::get().project(), id);
    return sp ? *sp : none;
}
bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
std::string num(double v) { return std::to_string(v); }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    gOut = argc > 1 ? QString(argv[1]) : QString("surround_ui_out");
    QDir().mkpath(gOut);
    theme();
    QSettings().remove("mixer");
    uitest::setup({}, {});
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t m1 = app.importMedia("synthaudio:1", &err), m2 = app.importMedia("synthaudio:2", &err);
    app.addMediaToTimeline(m1, 0.0);
    const uint64_t t2 = app.addTrack(TrackType::Audio);
    app.addMediaToTimeline(m2, 0.0, t2);
    uint64_t t1 = 0;
    for (const auto& t : app.project().tracks)
        if (t.type == TrackType::Audio && t.id != t2) t1 = t.id;
    uint64_t bus = 0;
    app.edit("Setup", [&](Project& p) {
        p.findTrack(t1)->name = "Music";
        p.findTrack(t2)->name = "Dialogue";
        Bus b;
        b.id = p.newId();
        b.name = "Bus A";
        p.buses.push_back(b);
        bus = b.id;
    });
    app.currentTrack = t1;

    // ---------------------------------------------------------------- a stereo project: nothing to pan
    SurroundPannerWindow win;
    win.resize(330, 470);
    win.show();
    uitest::settle(100);
    QLabel* msg = win.findChild<QLabel*>("surroundMessage");
    check(msg && msg->isVisible() && msg->text().startsWith("No Surround Project") && !win.field()->isVisible(),
          "in a stereo project the window says there is no surround project");
    shot(&win, "panner_stereo_project");

    // ---------------------------------------------------------------- Project Properties > Audio > Master bus mode
    {
        bool done = false;
        QTimer::singleShot(200, [&] {
            for (QWidget* w : QApplication::topLevelWidgets()) {
                QDialog* d = qobject_cast<QDialog*>(w);
                if (!d || !d->isVisible()) continue;
                QComboBox* mode = d->findChild<QComboBox*>("masterBusMode");
                QCheckBox* lfe = d->findChild<QCheckBox*>("lfeLowpass");
                QComboBox* q = d->findChild<QComboBox*>("lfeQuality");
                if (!mode || !lfe || !q) continue;
                mode->setCurrentIndex(mode->findData("6"));
                q->setCurrentIndex(q->findData("good"));
                // its Audio tab, for the picture
                for (QTabWidget* tabs : d->findChildren<QTabWidget*>())
                    for (int i = 0; i < tabs->count(); ++i)
                        if (tabs->tabText(i).remove('&') == "Audio") tabs->setCurrentIndex(i);
                uitest::settle(50);
                d->grab().save(gOut + "/project_properties_audio.png");
                for (QPushButton* b : d->findChildren<QPushButton*>())
                    if (b->text().remove('&') == "OK") {
                        b->click();
                        done = true;
                        break;
                    }
            }
        });
        runProjectPropertiesFull(nullptr);
        uitest::settle(50);
        check(done && app.project().settings.audioChannels == 6 && app.project().settings.lfeQuality == "good" && app.project().settings.lfeLowpass,
              "Project Properties sets the Master bus mode to 5.1 Surround (and the LFE filter's quality)");
    }
    uitest::settle(50);
    check(win.field()->isVisible() && !msg->isVisible() && win.shownId() == t1, "in a 5.1 project the window shows the current track's panner");

    // ---------------------------------------------------------------- the keys
    SurroundField* f = win.field();
    f->setFocus();
    uitest::key(f, Qt::Key_7, Qt::KeypadModifier);
    check(near(panOf(t1).x, -1) && near(panOf(t1).y, 1), "keypad 7 puts the point at the front left", num(panOf(t1).x) + "," + num(panOf(t1).y));
    check(app.undoName() == "Surround Pan Position", "as an undo step", app.undoName());
    uitest::key(f, Qt::Key_Down);
    check(near(panOf(t1).y, 0.95, 1e-9), "Down moves it back", num(panOf(t1).y));
    uitest::key(f, Qt::Key_Right, Qt::ControlModifier);
    check(near(panOf(t1).x, -0.99, 1e-9), "Ctrl+Right moves it a little to the right", num(panOf(t1).x));
    uitest::key(f, Qt::Key_PageUp, Qt::ShiftModifier);
    check(near(panOf(t1).x, -0.94, 1e-9), "Shift+Page Up moves it right", num(panOf(t1).x));
    uitest::key(f, Qt::Key_9, Qt::KeypadModifier | Qt::ControlModifier);
    check(near(panOf(t1).x, std::sqrt(0.5), 1e-9) && near(panOf(t1).y, std::sqrt(0.5), 1e-9), "Ctrl+keypad 9: the front right of the largest circle");
    uitest::key(f, Qt::Key_5, Qt::KeypadModifier);
    check(near(panOf(t1).x, 0) && near(panOf(t1).y, 0), "keypad 5: the middle");
    uitest::wheel(f, f->rect().center(), 120);
    check(near(panOf(t1).y, 0.05, 1e-9), "the wheel moves it forward", num(panOf(t1).y));
    uitest::wheel(f, f->rect().center(), -240, Qt::ShiftModifier);
    check(near(panOf(t1).x, -0.1, 1e-9), "Shift+wheel moves it left", num(panOf(t1).x));

    // ---------------------------------------------------------------- the mouse
    {
        f->grab();
        // drag the point to where the rear right speaker is
        const QPointF from = f->pointPos();
        const QPointF to = f->speakerRect(ChRs).center() + QPointF(-14, -14);
        uitest::mouseDrag(f, from.toPoint(), to.toPoint());
        const SurroundPan& sp = panOf(t1);
        check(sp.x > 0.7 && sp.y < -0.7 && app.undoName() == "Surround Pan Position", "dragging the point moves it", num(sp.x) + "," + num(sp.y));
        app.undo();
        check(near(panOf(t1).x, -0.1, 1e-9) && near(panOf(t1).y, 0.05, 1e-9), "and the drag is one undo step");
        app.redo();
        // Alt+drag: round its circle (the distance from the middle stays)
        app.edit("Point", [&](Project& p) {
            p.findTrack(t1)->surround.x = 0.5;
            p.findTrack(t1)->surround.y = -0.3;
        });
        f->grab();
        const double r0 = std::hypot(panOf(t1).x, panOf(t1).y);
        const QPointF p0 = f->pointPos();
        uitest::mouseDrag(f, p0.toPoint(), (p0 + QPointF(-80, -60)).toPoint(), 8, Qt::AltModifier);
        const double r1 = std::hypot(panOf(t1).x, panOf(t1).y);
        check(std::fabs(r0 - r1) < 0.02 && panOf(t1).y > -0.6, "Alt+drag keeps it on its circle", num(r0) + " -> " + num(r1));
        // Shift+drag: along the line through the middle (the angle stays)
        f->grab();
        const double a0 = std::atan2(panOf(t1).y, panOf(t1).x);
        const QPointF p1 = f->pointPos();
        uitest::mouseDrag(f, p1.toPoint(), (p1 + QPointF(30, -50)).toPoint(), 8, Qt::ShiftModifier);
        const double a1 = std::atan2(panOf(t1).y, panOf(t1).x);
        check(std::fabs(a0 - a1) < 0.02, "Shift+drag keeps it on its line through the middle", num(a0) + " -> " + num(a1));
        // a click on a speaker mutes it for this sound
        uitest::mouseClick(f, f->speakerRect(ChC).center().toPoint());
        check(panOf(t1).speakerMutes == (1u << ChC) && app.undoName() == "Surround Pan Speaker Mutes", "a click on the centre speaker mutes it");
    }

    // ---------------------------------------------------------------- the controls
    {
        QSlider* center = win.findChild<QSlider*>("surroundCenter");
        QSlider* smooth = win.findChild<QSlider*>("surroundSmoothness");
        QCheckBox* lfe = win.findChild<QCheckBox*>("surroundLfe");
        QComboBox* route = win.findChild<QComboBox*>("surroundRoute");
        QComboBox* motion = win.findChild<QComboBox*>("surroundMotion");
        center->setValue(0);
        check(panOf(t1).center == 0, "Center to the bottom: -Inf (a phantom centre)");
        center->setValue(1000);
        check(near(panOf(t1).center, 1.0, 1e-9), "Center to the top: 0 dB");
        smooth->setValue(40);
        check(near(panOf(t1).smoothness, 0.4, 1e-9), "Smoothness");
        lfe->setChecked(true);
        check(panOf(t1).lfe, "Enable LFE");
        motion->setCurrentIndex(motion->findData("x"));
        const double y0 = panOf(t1).y;
        uitest::key(f, Qt::Key_Up);
        check(panOf(t1).motion == "x" && near(panOf(t1).y, y0), "Move left/right only keeps the point's front/back");
        motion->setCurrentIndex(motion->findData("free"));
        uitest::key(f, Qt::Key_1, Qt::KeypadModifier);
        uitest::settle(30);
        shot(&win, "panner_51");
        route->setCurrentIndex(route->findData("lfe"));
        check(panOf(t1).route == "lfe", "the route: LFE Only");
        uitest::settle(30);
        shot(&win, "panner_lfe_only");
        route->setCurrentIndex(route->findData("surround"));
        // keyframes: one at 0 s here, one at 1 s at the front right; between them the point moves
        app.setCursor(0.0);
        QPushButton* add = win.findChild<QPushButton*>("surroundAddKey");
        add->click();
        app.setCursor(1.0);
        uitest::key(f, Qt::Key_9, Qt::KeypadModifier);
        check(panOf(t1).keys.size() == 2 && near(panOf(t1).keys[1].x, 1) && near(panOf(t1).keys[0].x, -1), "keyframes: one added, moving the point at another time makes another",
              std::to_string(panOf(t1).keys.size()));
        app.setCursor(0.5);
        uitest::settle(30);
        const QPointF mid = win.field()->point();
        check(near(mid.x(), 0, 1e-9) && near(mid.y(), 0, 1e-9), "the window shows the point between keyframes at the cursor");
        shot(&win, "panner_keyframes");
        win.findChild<QPushButton*>("surroundDeleteKey")->click();
        check(panOf(t1).keys.size() == 1, "Delete Keyframe");
    }

    // ---------------------------------------------------------------- the bus, picked in the list
    {
        QComboBox* target = win.findChild<QComboBox*>("surroundTarget");
        target->setCurrentIndex(target->findData(QVariant::fromValue<qulonglong>(bus)));
        check(win.shownId() == bus, "a bus's panner can be shown");
        uitest::key(f, Qt::Key_3, Qt::KeypadModifier);
        check(near(panOf(bus).x, 1) && near(panOf(bus).y, -1), "and moved");
        target->setCurrentIndex(0);
    }

    // ---------------------------------------------------------------- the Mixing Console and the Master Bus panel in 5.1
    {
        app.edit("Pans", [&](Project& p) {
            p.findTrack(t1)->surround = SurroundPan();
            p.findTrack(t1)->surround.x = -1;
            p.findTrack(t1)->surround.y = 1;
            p.findTrack(t2)->surround = SurroundPan();
            p.findTrack(t2)->surround.route = "center";
        });
        Playback pb;
        MixerWindow mixer(&pb);
        uint64_t opened = 0;
        mixer.onOpenSurround = [&](uint64_t id) { opened = id; };
        mixer.resize(760, 560);
        mixer.show();
        MasterBusWidget master(&pb);
        master.resize(170, 460);
        master.show();
        uitest::settle(150);
        QWidget* s1 = mixer.findChild<QWidget*>(QString("strip:track:%1").arg(t1));
        SurroundField* mini = s1 ? dynamic_cast<SurroundField*>(s1->findChild<QWidget*>("surroundMini")) : nullptr;
        QWidget* pan = s1 ? s1->findChild<QWidget*>("pan") : nullptr;
        check(mini && mini->isVisible() && pan && !pan->isVisible(), "a track strip shows a small surround panner instead of the pan slider");
        if (mini) {
            uitest::mouseDoubleClick(mini, mini->rect().center());
            check(opened == t1, "a double-click on it opens the Surround Panner for that track");
        }
        app.setCursor(0.2);
        pb.play();
        uitest::settle(700);
        {
            // Music at the front left, Dialogue on the centre: L and C, nothing elsewhere
            std::vector<AudioEngine::StripPeak> lv;
            pb.stripLevels(lv);
            const AudioEngine::StripPeak* m = nullptr;
            for (const auto& x : lv)
                if (x.id == 0) m = &x;
            check(m && m->channels == 6 && m->ch[ChL] > 0.2f && m->ch[ChC] > 0.2f && m->ch[ChR] < 1e-4f && m->ch[ChLs] < 1e-4f && m->ch[ChRs] < 1e-4f,
                  "playback: the master's six channels carry what the panners say",
                  m ? strfmt("L %.3f R %.3f C %.3f LFE %.3f Ls %.3f Rs %.3f", m->ch[0], m->ch[1], m->ch[2], m->ch[3], m->ch[4], m->ch[5]) : std::string("none"));
        }
        QWidget* mf = mixer.findChild<QWidget*>("strip:master");
        QWidget* fm = mf ? mf->findChild<QWidget*>("fader") : nullptr;
        QWidget* mb = master.findChild<QWidget*>("masterFader");
        check(fm && fm->property("meterChannels").toInt() == 6 && mb && mb->property("meterChannels").toInt() == 6,
              "the master's meters show six channels while a 5.1 project plays");
        shot(&mixer, "mixer_51");
        shot(&master, "master_bus_51");
        pb.stop();
        uitest::settle(100);
        // back to stereo: the pan sliders return
        app.edit("Stereo", [&](Project& p) { p.settings.audioChannels = 2; });
        uitest::settle(100);
        check(mini && !mini->isVisible() && pan && pan->isVisible(), "in a stereo project the strip has its pan slider again");
    }
    return uitest::summary();
}
