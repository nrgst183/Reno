// Presses every keyboard shortcut the main window implements (VEGAS Pro defaults) and checks what each did to the project, cursor,
// selection and window. Keys go in through the window system like a keyboard's, so Qt's shortcut handling decides where they land
// (an ambiguous key does nothing and fails here).
//
//   keys_test <output folder> [testfx.ofx ...]
//
// Needs Qt's window-system interface, hence the extra include folders:
//   EXTRA_CFLAGS="-I$Q/include/QtGui/6.8.3 -I$Q/include/QtGui/6.8.3/QtGui -I$Q/include/QtCore/6.8.3 -I$Q/include/QtCore/6.8.3/QtCore"
//   uibuild keys_test tests/ui/keys_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QAction>
#include <QFileDialog>
#include <QSlider>
#include <QRadioButton>
#include <QSpinBox>
#include <QLabel>
#include <QApplication>
#include <QPushButton>
#include <QComboBox>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <cmath>
#include <cstdio>
#include <map>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;

struct Ids {
    uint64_t v1 = 0, v2 = 0, a1 = 0, a2 = 0;
    uint64_t clip = 0, pic = 0, snd = 0;
    uint64_t e1 = 0, e1a = 0, e2 = 0, e3 = 0, e4 = 0, e5 = 0, e6 = 0;
} ids;

uint64_t addEvent(Project& p, uint64_t track, uint64_t media, double start, double length, double offset = 0, uint64_t group = 0)
{
    Event e;
    e.id = p.newId();
    e.mediaId = media;
    e.start = start;
    e.length = length;
    e.offset = offset;
    e.group = group;
    e.name = p.findMedia(media)->name;
    p.findTrack(track)->events.push_back(e);
    p.findTrack(track)->sortEvents();
    return e.id;
}

// Same project as the timeline test:
//   V1: e1 clip 1..5 (grouped with e1a on A1), e2 still 7..10
//   V2: e3 clip 2..7 (offset 1), e5 clip 10..14, e6 clip 12..16
//   A1: e1a clip sound 1..5        A2: e4 sound only 0.5..6.5
void buildProject()
{
    st().newProject();
    std::string err;
    ids.clip = st().importMedia("synth:1", &err);
    ids.pic = st().importMedia("synthpic:3", &err);
    ids.snd = st().importMedia("synthaudio:2", &err);
    st().edit("Build", [&](Project& p) {
        const char* names[] = {"Titles", "Camera", "Dialogue", "Music"};
        for (int k = 0; k < 4; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k < 2 ? TrackType::Video : TrackType::Audio;
            t.name = names[k];
            p.tracks.push_back(t);
        }
        ids.v1 = p.tracks[0].id;
        ids.v2 = p.tracks[1].id;
        ids.a1 = p.tracks[2].id;
        ids.a2 = p.tracks[3].id;
        const uint64_t g = p.newId();
        ids.e1 = addEvent(p, ids.v1, ids.clip, 1.0, 4.0, 0, g);
        ids.e1a = addEvent(p, ids.a1, ids.clip, 1.0, 4.0, 0, g);
        ids.e2 = addEvent(p, ids.v1, ids.pic, 7.0, 3.0);
        ids.e3 = addEvent(p, ids.v2, ids.clip, 2.0, 5.0, 1.0);
        ids.e4 = addEvent(p, ids.a2, ids.snd, 0.5, 6.0);
        ids.e5 = addEvent(p, ids.v2, ids.clip, 10.0, 4.0);
        ids.e6 = addEvent(p, ids.v2, ids.clip, 12.0, 4.0, 3.0);
    });
    st().setSelection(std::set<uint64_t>());
    st().setCursor(0.0);
    st().setLoop(0, 0);
    st().loopOn = false;
    st().snapping = true;
    st().currentTrack = ids.v2;
    if (gW) {
        gW->timeline()->options() = TimelineOptions();
        gW->timeline()->optionsChanged();
        gW->timeline()->zoomToFit();
    }
}

const Event* ev(uint64_t id, uint64_t* track = nullptr)
{
    const Track* t = nullptr;
    const Event* e = st().project().findEvent(id, &t);
    if (track) *track = t ? t->id : 0;
    return e;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
std::string num(double v) { return strfmt("%.6f", v); }
double fr() { return 1.0 / st().project().settings.fps(); }
size_t eventCount()
{
    size_t n = 0;
    for (const Track& t : st().project().tracks) n += t.events.size();
    return n;
}

// The text a key types, for keys that type something.
QString textOf(int key, Qt::KeyboardModifiers mods)
{
    if (mods & (Qt::ControlModifier | Qt::AltModifier)) return QString();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar((mods & Qt::ShiftModifier) ? key : key - Qt::Key_A + 'a'));
    if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) return QString(QChar(key));
    if (key == Qt::Key_Return || key == Qt::Key_Enter) return QString("\r");
    return QString();
}

// A key pressed and released.
void press(int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    gW->activateWindow();
    QCoreApplication::processEvents();
    QWindow* win = QGuiApplication::focusWindow() ? QGuiApplication::focusWindow() : gW->windowHandle();
    const QString text = textOf(key, mods);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyPress, key, mods, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyRelease, key, mods, text);
    QCoreApplication::processEvents();
}

// "Ctrl+Shift+Q", "Alt+D, D": each key of the sequence in turn.
void press(const char* seq)
{
    const QKeySequence ks(QString::fromUtf8(seq));
    for (int k = 0; k < ks.count(); ++k) press(ks[k].key(), ks[k].keyboardModifiers());
}

QDockWidget* dockNamed(const char* name) { return gW->findChild<QDockWidget*>(name); }
// Shown and in front: a dock behind another one's tab is moved out of the window.
bool shownDock(const char* name)
{
    QDockWidget* d = dockNamed(name);
    return d && d->isVisible() && !d->isFloating() && gW->rect().intersects(QRect(d->mapTo(gW, QPoint(0, 0)), d->size()));
}

// Answers the next modal input dialog (Go To, Paste Repeat) with 'text' or 'number'.
void answerDialog(const QString& text, int number)
{
    QTimer::singleShot(150, [text, number] {
        for (QWidget* t : QApplication::topLevelWidgets()) {
            // Paste Repeat asks for the number of times, end to end
            if (t->objectName() == "pasteRepeat" && t->isVisible()) {
                if (QSpinBox* n = t->findChild<QSpinBox*>("times")) n->setValue(number);
                if (QRadioButton* r = t->findChild<QRadioButton*>("endToEnd")) r->setChecked(true);
                static_cast<QDialog*>(t)->accept();
                continue;
            }
            if (QInputDialog* d = qobject_cast<QInputDialog*>(t))
                if (d->isVisible()) {
                    if (d->inputMode() == QInputDialog::IntInput) d->setIntValue(number);
                    else d->setTextValue(text);
                    d->accept();
                }
        }
    });
}

QLineEdit* visibleEdit(QWidget* in)
{
    for (QLineEdit* e : in->findChildren<QLineEdit*>())
        if (e->isVisible()) return e;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoKeysTest");
    QApplication::setApplicationName("keys_test");
    QSettings().clear();
    // M and R would open the new marker's name box (Preferences > General: off here, so keys go to the timeline)
    QSettings().setValue("prefs/autoNameMarkers", false);
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("keys");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());

    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1600, 900);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    buildProject();
    TimelineWidget* tl = w.timeline();
    Playback* pb = w.playback();
    uitest::settle(100);

    // ---------------------------------------------------------------- the list and the actions agree
    {
        std::map<QString, QStringList> owners;
        for (QAction* a : w.findChildren<QAction*>()) {
            // a window's own keys (Edit Details' Ctrl+C / Ctrl+V) only work while that window has focus
            if (a->shortcutContext() == Qt::WidgetShortcut || a->shortcutContext() == Qt::WidgetWithChildrenShortcut) continue;
            for (const QKeySequence& k : a->shortcuts()) owners[k.toString()] << a->objectName();
        }
        std::string clashes;
        for (const auto& o : owners)
            if (o.second.size() > 1) clashes += o.first.toStdString() + " (" + o.second.join(", ").toStdString() + ") ";
        check(clashes.empty(), "no key belongs to two commands", clashes);
        std::string missing;
        int windowKeys = 0;
        for (const ShortcutInfo& s : shortcutList()) {
            if (s.kind != ShortcutInfo::Window) continue;
            const QString keys = QString::fromUtf8(s.keys);
            if (keys.contains("...")) continue;            // the numbered ranges are checked below
            if (keys.isEmpty()) continue;  // a command done with the mouse, like the Rate slider
            for (const QString& alt : keys.split(" or ")) {
                ++windowKeys;
                if (!owners.count(QKeySequence(alt.trimmed()).toString())) missing += alt.toStdString() + " ";
            }
        }
        check(missing.empty() && windowKeys > 100, "every window key in the shortcut list is bound", missing + strfmt("(%d keys)", windowKeys));
        for (int n = 0; n <= 9; ++n) {
            if (!owners.count(QKeySequence(QString::number(n)).toString())) missing += strfmt("%d ", n);
            if (!owners.count(QKeySequence(QString("Alt+D, %1").arg(n)).toString())) missing += strfmt("Alt+D,%d ", n);
            if (!owners.count(QKeySequence(QString("Ctrl+Alt+D, %1").arg(n)).toString())) missing += strfmt("Ctrl+Alt+D,%d ", n);
        }
        check(missing.empty(), "the number keys and the layout keys are bound", missing);
        // the menus show VEGAS's names and the keys
        QStringList menus;
        for (QAction* m : w.menuBar()->actions()) menus << m->text().remove('&');
        check(menus.join(",") == "File,Edit,View,Insert,Tools,Options,Help", "the menu bar is VEGAS's", menus.join(",").toStdString());
    }

    // ---------------------------------------------------------------- playback
    {
        st().setCursor(1.0);
        press("Space");
        check(pb->playing(), "Space plays");
        uitest::settle(400);
        press("Space");
        check(!pb->playing() && near(st().cursor(), 1.0), "Space again stops and goes back to where it began", num(st().cursor()));
        press(Qt::Key_Return);
        uitest::settle(300);
        const double at = st().cursor();
        press(Qt::Key_Return);
        check(!pb->playing() && at > 1.05 && near(st().cursor(), at, 0.1), "Enter plays and pauses where it is", num(st().cursor()));
        press("Ctrl+F12");
        check(pb->playing(), "Ctrl+F12 plays too");
        press("Esc");
        check(!pb->playing(), "Esc stops");
        st().setCursor(5.0);
        press("Shift+Space");
        check(pb->playing() && st().cursor() < 1.0, "Shift+Space plays from the start", num(st().cursor()));
        press("Esc");
        st().setLoop(3.0, 6.0);
        st().setCursor(8.0);
        press("Shift+F12");
        check(pb->playing() && st().cursor() >= 3.0 && st().cursor() < 4.0, "Shift+F12 plays from the start of the time selection", num(st().cursor()));
        press("Esc");
        st().setLoop(0, 0);
        st().setCursor(2.0);
        press("Ctrl+Space");
        check(pb->playing(), "Ctrl+Space plays");
        press("F12");
        check(!pb->playing() && near(st().cursor(), 2.0), "F12 stops");
        press("L");
        check(pb->playing() && pb->rate() == 1.0, "L plays", num(pb->rate()));
        press("L");
        check(pb->playing() && pb->rate() == 1.5, "L again plays faster (1.5)", num(pb->rate()));
        press("L");
        press("L");
        check(pb->rate() == 4.0, "and each press a step faster (2, 4)", num(pb->rate()));
        uitest::settle(300);
        press("K");
        check(pb->playing() && pb->rate() == 0.25, "K slows playing to a quarter speed", num(pb->rate()));
        press("L");
        check(pb->playing() && pb->rate() == 0.5, "then L adds a quarter", num(pb->rate()));
        press("J");
        press("J");
        check(!pb->playing(), "J takes a quarter off each press: at 0 it pauses", num(pb->rate()));
        const double k = st().cursor();
        press("J");
        check(pb->playing() && pb->rate() == -0.25, "and on through 0 to backwards", num(pb->rate()));
        uitest::settle(300);
        check(st().cursor() < k, "the cursor goes back", num(k) + " -> " + num(st().cursor()));
        press("Space");
        press("L");
        check(pb->playing() && pb->rate() == 1.0, "stopped with Space: L plays at normal speed again", num(pb->rate()));
        press("L");
        check(pb->rate() == 1.5, "L again plays faster (1.5)", num(pb->rate()));
        press("J");
        check(pb->rate() == -1.0, "J while going forwards plays backwards at normal speed", num(pb->rate()));
        press("K");
        check(pb->playing() && pb->rate() == -0.25, "K while going backwards slows to a quarter backwards", num(pb->rate()));
        press("Space");
        {
            // the Rate slider: dragged, it plays at the rate it is at; let go, it springs back and stops
            QSlider* rs = gW->findChild<QSlider*>("rateSlider");
            QLabel* rl = gW->findChild<QLabel*>("rateLabel");
            if (rs && rl) {
                rs->setSliderDown(true);
                rs->setSliderPosition(-150);
                QCoreApplication::processEvents();
                check(pb->playing() && pb->rate() == -1.5 && rl->text() == "Rate: -1.50", "dragging the Rate slider to -1.5 plays backwards at 1.5 times",
                      num(pb->rate()) + " " + rl->text().toStdString());
                rs->setSliderPosition(50);
                QCoreApplication::processEvents();
                check(pb->rate() == 0.5 && rl->text() == "Rate: 0.50", "and to 0.5 at half speed", num(pb->rate()));
                rs->setSliderDown(false);
                QCoreApplication::processEvents();
                check(!pb->playing() && rs->value() == 0 && rl->text() == "Rate: 0.00", "letting it go springs it back and stops", rl->text().toStdString());
                pb->setNormalRate(2.0);
                const double from = st().cursor();
                press("Space");
                check(pb->playing() && pb->rate() == 2.0, "Play plays at the normal rate set under the slider", num(pb->rate()));
                press("Esc");
                check(near(st().cursor(), from), "and stops back where it began");
                pb->setNormalRate(1.0);
            } else {
                check(false, "the Rate slider and its label are there");
            }
        }
        {
            // Num 0: a second centred on the cursor, then back to it
            st().setCursor(3.0);
            tl->setFocus();
            press(Qt::Key_0, Qt::KeypadModifier);
            uitest::settle(150);
            check(pb->playing() && st().cursor() < 3.0 && st().cursor() > 2.4, "Num 0 previews round the cursor (from half a second before it)", num(st().cursor()));
            for (int k = 0; k < 60 && pb->playing(); ++k) uitest::settle(25);
            check(!pb->playing() && near(st().cursor(), 3.0), "and stops after a second, back at the cursor", num(st().cursor()));
            // Ctrl+Shift+F12: dim
            press("Ctrl+Shift+F12");
            check(pb->dimmed(), "Ctrl+Shift+F12 dims the master output");
            press("Ctrl+Shift+F12");
            check(!pb->dimmed(), "and again undims it");
            // Shift+B: dynamic RAM preview of the time selection
            st().setLoop(1.0, 2.0);
            press("Shift+B");
            uitest::settle(100);
            // (at least the selection's 30: frames played earlier stay in the RAM preview too, as many as the earlier playing made)
            check(pb->ramPreviewFrames() >= 30 && pb->playing() && st().loopOn, "Shift+B renders the time selection's 30 frames into memory and plays it round",
                  strfmt("%d frames", (int)pb->ramPreviewFrames()));
            press("Esc");
            st().loopOn = false;
            st().edit("Touch", [](Project& p) { p.tracks[0].name = "Titles!"; });
            check(pb->ramPreviewFrames() == 0, "a change to the project lets the RAM preview go");
            // Ctrl+M: render to new track (the dialog is given a file in the output folder)
            const QString file = QDir(out).absoluteFilePath("rendered_to_track.mp4");
            QFile::remove(file);
            {
                // polled every 100 ms until it shows, gives up after 20 s
                QTimer* answer = new QTimer(gW);
                std::shared_ptr<int> tries = std::make_shared<int>(0);
                QObject::connect(answer, &QTimer::timeout, [answer, file, tries] {
                    for (QWidget* t : QApplication::topLevelWidgets())
                        if (t->objectName() == "renderDialog" && t->isVisible()) {
                            answer->stop();
                            answer->deleteLater();
                            if (auto* folder = t->findChild<QComboBox*>("outputFolder")) folder->setEditText(QFileInfo(file).absolutePath());
                            if (auto* name = t->findChild<QLineEdit*>("outputName")) name->setText(QFileInfo(file).completeBaseName());
                            if (auto* go = t->findChild<QPushButton*>("renderButton")) go->click();
                            return;
                        }
                    if (++*tries > 200) {
                        answer->stop();
                        answer->deleteLater();
                    }
                });
                answer->start(100);
            }
            const size_t tracksBefore = st().project().tracks.size();
            press("Ctrl+M");
            uitest::settle(200);
            const Project& pr = st().project();
            const bool added = pr.tracks.size() == tracksBefore + 2 && pr.tracks[0].type == TrackType::Video && pr.tracks[0].events.size() == 1 &&
                               near(pr.tracks[0].events[0].start, 1.0) && std::fabs(pr.tracks[0].events[0].length - 1.0) < 0.05;
            check(QFileInfo::exists(file) && added, "Ctrl+M renders the time selection and puts it on new tracks at the top, where it was",
                  strfmt("%d tracks (was %d); top: %s, %d events, %.3f + %.3f", (int)pr.tracks.size(), (int)tracksBefore,
                         pr.tracks[0].type == TrackType::Video ? "video" : "audio", (int)pr.tracks[0].events.size(),
                         pr.tracks[0].events.empty() ? -1.0 : pr.tracks[0].events[0].start, pr.tracks[0].events.empty() ? -1.0 : pr.tracks[0].events[0].length));
            st().setLoop(0, 0);
        }
        st().setCursor(1.0);
        press("Space");
        uitest::settle(300);
        press("Alt+Down");
        const double here = st().cursor();
        press("Space");
        check(st().cursor() >= here - 0.05 && here > 1.05, "Alt+Down: the cursor stays where playing had got to", num(here) + " " + num(st().cursor()));
        press("Q");
        check(st().loopOn, "Q turns loop playback on");
        press("Ctrl+Shift+L");
        check(!st().loopOn, "Ctrl+Shift+L turns it off");
    }

    // ---------------------------------------------------------------- cursor and navigation
    {
        buildProject();
        press("Alt+0");
        check(tl->hasFocus(), "Alt+0 gives the timeline the focus");
        st().setCursor(4.0);
        press("Ctrl+Home");
        check(near(st().cursor(), 0.0), "Ctrl+Home goes to the start");
        press("Ctrl+End");
        check(near(st().cursor(), 16.0), "Ctrl+End goes to the end");
        press("W");
        check(near(st().cursor(), 0.0), "W goes to the start");
        st().setCursor(2.0);
        press("Alt+Right");
        check(near(st().cursor(), 2.0 + fr()), "Alt+Right: next frame");
        press("Alt+Left");
        press("Alt+Left");
        check(near(st().cursor(), 2.0 - fr()), "Alt+Left: previous frame");
        st().setCursor(2.0);
        press("Right");
        check(near(st().cursor(), 2.0 + fr()), "Right moves a frame (at this zoom a pixel is less)", num(st().cursor()));
        press("Left");
        check(near(st().cursor(), 2.0), "Left moves back");
        press("Shift+Right");
        press("Shift+Right");
        check(near(st().loopStart, 2.0) && near(st().loopEnd, 2.0 + 2 * fr()), "Shift+Right makes a time selection", num(st().loopStart) + " " + num(st().loopEnd));
        press("Home");
        check(near(st().cursor(), 2.0), "Home: start of the time selection");
        press("End");
        check(near(st().cursor(), 2.0 + 2 * fr()), "End: its end");
        st().setLoop(0, 0);
        press("Home");
        check(near(st().cursor(), tl->viewStartTime()), "Home with no time selection: the start of the view");
        st().setCursor(4.0);
        press("I");
        st().setCursor(6.0);
        press("O");
        check(near(st().loopStart, 4.0) && near(st().loopEnd, 6.0), "I and O set the time selection");
        st().setLoop(0, 0);
        st().setCursor(0.0);
        press("PgDown");
        const double grid = st().cursor();
        check(grid > 0.1, "Page Down moves the cursor to the next grid mark", num(grid));
        press("PgDown");
        check(near(st().cursor(), 2 * grid, 1e-6), "and the next", num(st().cursor()));
        press("PgUp");
        check(near(st().cursor(), grid, 1e-6), "Page Up goes back one");
        // event edges and edit points
        st().setCursor(0.0);
        press("]");
        check(near(st().cursor(), 0.5), "] goes to the next event edge", num(st().cursor()));
        press("]");
        check(near(st().cursor(), 1.0), "and the next");
        press("[");
        check(near(st().cursor(), 0.5), "[ goes back");
        st().edit("Fade", [](Project& p) { if (Event* e = p.findEvent(ids.e2)) e->fadeIn = 1.0; });
        st().setCursor(7.0);
        press("Ctrl+Alt+Right");
        check(near(st().cursor(), 8.0), "Ctrl+Alt+Right stops at the end of a fade too", num(st().cursor()));
        press("Ctrl+Alt+Left");
        check(near(st().cursor(), 7.0), "Ctrl+Alt+Left goes back to the edge");
        st().selectOnly(ids.e5);
        st().setCursor(0.0);
        press("Ctrl+]");
        check(near(st().cursor(), 10.0), "Ctrl+] goes to the next edge of a selected event", num(st().cursor()));
        press("Ctrl+]");
        check(near(st().cursor(), 14.0), "and its end");
        press("Ctrl+[");
        check(near(st().cursor(), 10.0), "Ctrl+[ goes back");
        st().setSelection(std::set<uint64_t>{ids.e3, ids.e5});
        st().setCursor(0.0);
        press("Ctrl+Shift+]");
        check(near(st().loopStart, 2.0) && near(st().loopEnd, 7.0), "Ctrl+Shift+] time-selects the next selected event", num(st().loopStart) + " " + num(st().loopEnd));
        press("Ctrl+Shift+]");
        check(near(st().loopStart, 10.0) && near(st().loopEnd, 14.0), "and then the one after");
        press("Ctrl+Shift+[");
        check(near(st().loopStart, 2.0) && near(st().loopEnd, 7.0), "Ctrl+Shift+[ goes back");
        press("Backspace");
        check(near(st().loopStart, 10.0) && near(st().loopEnd, 14.0), "Backspace restores the time selection before", num(st().loopStart) + " " + num(st().loopEnd));
        press("Backspace");
        check(near(st().loopStart, 2.0) && near(st().loopEnd, 7.0), "and the one before that");
        st().setLoop(0, 0);
        // zoom
        tl->zoomToFit();
        const double span0 = tl->viewEndTime() - tl->viewStartTime();
        press("Up");
        check(tl->viewEndTime() - tl->viewStartTime() < span0 * 0.8, "Up zooms in");
        press("Down");
        check(near(tl->viewEndTime() - tl->viewStartTime(), span0, span0 * 0.02), "Down zooms out");
        st().setLoop(5.0, 6.0);
        press("Ctrl+Up");
        check(tl->viewStartTime() <= 5.0 && tl->viewEndTime() >= 6.0 && tl->viewEndTime() - tl->viewStartTime() < 1.5, "Ctrl+Up zooms to the time selection",
              num(tl->viewStartTime()) + " " + num(tl->viewEndTime()));
        const double span1 = tl->viewEndTime() - tl->viewStartTime();
        press("Ctrl+Down");
        check(tl->viewEndTime() - tl->viewStartTime() > span1 * 3.0, "Ctrl+Down zooms out in a big step");
        st().setLoop(0, 0);
        for (int k = 0; k < 4; ++k) tl->zoomIn();
        st().setCursor(9.0);
        press("\\");
        check(near((tl->viewStartTime() + tl->viewEndTime()) * 0.5, 9.0, 0.05), "\\ centres the view on the cursor", num(tl->viewStartTime()) + " " + num(tl->viewEndTime()));
        tl->zoomToFit();
        // Go To
        // VEGAS types the time in place: a box over the time display or the selection's end
        auto typeInPlace = [](const QString& text) {
            uitest::settle(50);
            QLineEdit* e = nullptr;
            for (QWidget* t : QApplication::topLevelWidgets())
                if (!e) e = t->findChild<QLineEdit*>("inPlaceEdit");
            if (!e) return false;
            e->setText(text);
            uitest::key(e, Qt::Key_Return);
            uitest::settle(50);
            return true;
        };
        press("Ctrl+G");
        check(typeInPlace("00:00:05;15") && near(st().cursor(), 5.5), "Ctrl+G goes to a typed time", num(st().cursor()));
        press("Ctrl+Shift+G");
        typeInPlace("00:00:08;00");
        check(near(st().loopStart, 5.5) && near(st().loopEnd, 8.0), "Ctrl+Shift+G sets the end of the time selection", num(st().loopStart) + " " + num(st().loopEnd));
        st().setLoop(0, 0);
    }

    // ---------------------------------------------------------------- markers and regions
    {
        buildProject();
        st().setCursor(3.0);
        press("M");
        st().setCursor(9.0);
        press("M");
        check(st().project().markers.size() == 2 && near(st().project().markers[0].time, 3.0) && !st().project().markers[0].region, "M inserts a marker at the cursor");
        check(st().undoName() == "Insert Marker", "one undo step named Insert Marker", st().undoName());
        press("M");
        check(st().project().markers.size() == 2, "a second M at the same place does nothing");
        st().setLoop(11.0, 13.0);
        press("R");
        check(st().project().markers.size() == 3 && st().project().markers[2].region && near(st().project().markers[2].length, 2.0), "R makes a region of the time selection");
        st().setLoop(0, 0);
        st().setCursor(0.0);
        press("1");
        check(near(st().cursor(), 3.0), "1 goes to marker 1");
        press("2");
        check(near(st().cursor(), 9.0), "2 goes to marker 2");
        press("3");
        check(near(st().cursor(), 11.0) && near(st().loopStart, 11.0) && near(st().loopEnd, 13.0), "3 selects region 3");
        st().setLoop(0, 0);
        press("4");
        check(near(st().cursor(), 11.0), "a number with no marker does nothing");
        st().setCursor(0.0);
        press("Ctrl+Right");
        check(near(st().cursor(), 3.0), "Ctrl+Right: next marker");
        press("Ctrl+Right");
        press("Ctrl+Right");
        check(near(st().cursor(), 11.0), "... and the region's start");
        press("Ctrl+Right");
        check(near(st().cursor(), 13.0), "... and its end");
        press("Ctrl+Right");
        check(near(st().cursor(), 16.0), "past the last: the end of the project");
        press("Ctrl+Left");
        check(near(st().cursor(), 13.0), "Ctrl+Left: previous marker");
        // with the mouse: click a tag, drag it, double-click to name it, right-click to delete it
        const uint64_t m1 = st().project().markers[0].id;
        QRect tag = tl->markerTagRect(m1);
        check(!tag.isEmpty() && tl->markerBarRect().contains(tag.center()), "a marker's tag is in the marker bar");
        st().setCursor(0.0);
        uitest::mouseClick(tl, tag.center());
        check(near(st().cursor(), 3.0), "clicking a marker's tag takes the cursor there");
        const QPoint from(tag.left() + 2, tag.center().y());
        uitest::mouseDrag(tl, from, QPoint(tl->xAt(4.5) + 2, from.y()), 8, Qt::ShiftModifier);
        check(near(st().project().markers[0].time, 4.5, fr()) && st().undoName() == "Move Marker", "dragging a tag moves the marker",
              num(st().project().markers[0].time) + " " + st().undoName());
        tag = tl->markerTagRect(m1);
        uitest::mouseDoubleClick(tl, tag.center());
        QLineEdit* edit = visibleEdit(tl);
        check(edit != nullptr, "double-clicking a tag opens a box for its name");
        if (edit) {
            edit->setText("Drop");
            uitest::key(edit, Qt::Key_Return);
            uitest::settle(20);
            check(st().project().markers[0].name == "Drop", "Enter names the marker", st().project().markers[0].name);
        }
        uitest::settle(50);
        uitest::shot(&w, out + "/keys_markers.png");
        // the region's end tag: dragging it changes the region's length
        const uint64_t r3 = st().project().markers[2].id;
        (void)r3;
        const int endX = tl->xAt(13.0);
        uitest::mouseDrag(tl, QPoint(endX - 3, tl->markerBarRect().center().y()), QPoint(tl->xAt(14.0) - 3, tl->markerBarRect().center().y()), 8, Qt::ShiftModifier);
        check(near(st().project().markers[2].length, 3.0, fr()) && near(st().project().markers[2].time, 11.0), "dragging a region's end tag changes its length",
              num(st().project().markers[2].length));
        // right-click: Delete
        tag = tl->markerTagRect(m1);
        bool picked = false;
        QTimer::singleShot(150, [&] {
            if (QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : menu->actions())
                    if (a->text() == "Delete") { a->trigger(); picked = true; }
                menu->close();
            }
        });
        QContextMenuEvent ce(QContextMenuEvent::Mouse, tag.center(), tl->mapToGlobal(tag.center()));
        QApplication::sendEvent(tl, &ce);
        uitest::settle(50);
        check(picked && st().project().markers.size() == 2, "right-click > Delete deletes a marker");
    }

    // ---------------------------------------------------------------- editing
    {
        buildProject();
        {
            QWidget* f = QApplication::focusWidget();
            std::printf("(focus before editing: %s %s)\n", f ? f->metaObject()->className() : "none", f ? f->objectName().toUtf8().constData() : "");
        }
        const size_t n0 = eventCount();
        press("Ctrl+A");
        check(st().selection().size() == n0, "Ctrl+A selects every event");
        press("Ctrl+Shift+A");
        check(st().selection().empty(), "Ctrl+Shift+A unselects them");
        st().selectOnly(ids.e2);
        press("Ctrl+C");
        st().currentTrack = ids.v1;
        st().setCursor(11.0);
        press("Ctrl+V");
        check(eventCount() == n0 + 1, "Ctrl+C, Ctrl+V pastes a copy at the cursor");
        press("Ctrl+Z");
        check(eventCount() == n0, "Ctrl+Z undoes");
        press("Ctrl+Y");
        check(eventCount() == n0 + 1, "Ctrl+Y redoes");
        press("Alt+Backspace");
        check(eventCount() == n0, "Alt+Backspace undoes");
        press("Ctrl+Shift+Z");
        check(eventCount() == n0 + 1, "Ctrl+Shift+Z redoes");
        press("Ctrl+Z");
        st().selectOnly(ids.e2);
        press("Ctrl+Insert");
        st().setCursor(11.0);
        press("Shift+Insert");
        check(eventCount() == n0 + 1, "Ctrl+Insert, Shift+Insert copy and paste too");
        press("Ctrl+Z");
        st().selectOnly(ids.e2);
        press("Shift+Delete");
        check(eventCount() == n0 - 1 && !ev(ids.e2), "Shift+Delete cuts");
        press("Ctrl+Z");
        st().selectOnly(ids.e2);
        press("Delete");
        check(eventCount() == n0 - 1 && !ev(ids.e2), "Delete deletes");
        press("Ctrl+Z");
        // split
        st().setSelection(std::set<uint64_t>());
        st().setCursor(3.0);
        press("S");
        check(eventCount() == n0 + 4, "S splits every event under the cursor", strfmt("%d", (int)eventCount()));
        press("Ctrl+Z");
        // trims
        st().selectOnly(ids.e3);
        st().setCursor(3.0);
        press("Alt+[");
        check(near(ev(ids.e3)->start, 3.0) && near(ev(ids.e3)->length, 4.0) && near(ev(ids.e3)->offset, 2.0), "Alt+[ trims the start to the cursor",
              num(ev(ids.e3)->start) + " " + num(ev(ids.e3)->offset));
        st().setCursor(6.0);
        press("Alt+]");
        check(near(ev(ids.e3)->start, 3.0) && near(ev(ids.e3)->end(), 6.0), "Alt+] trims the end");
        press("Ctrl+Z");
        press("Ctrl+Z");
        st().selectOnly(ids.e1);
        st().setCursor(2.0);
        press("Alt+[");
        check(near(ev(ids.e1)->start, 2.0) && near(ev(ids.e1a)->start, 2.0), "a grouped event's partner is trimmed with it");
        press("Ctrl+Z");
        st().selectOnly(ids.e3);
        st().setLoop(4.0, 5.0);
        press("Ctrl+T");
        check(near(ev(ids.e3)->start, 4.0) && near(ev(ids.e3)->end(), 5.0) && near(ev(ids.e3)->offset, 3.0), "Ctrl+T trims to the time selection",
              num(ev(ids.e3)->start) + " " + num(ev(ids.e3)->end()));
        press("Ctrl+Z");
        st().setLoop(0, 0);
        // paste insert and paste repeat
        st().selectOnly(ids.e5);
        press("Ctrl+C");
        st().currentTrack = ids.v2;
        st().setCursor(10.0);
        press("Ctrl+Shift+V");
        check(near(ev(ids.e5)->start, 14.0) && near(ev(ids.e6)->start, 16.0), "Ctrl+Shift+V pastes and pushes later events along",
              num(ev(ids.e5)->start) + " " + num(ev(ids.e6)->start));
        press("Ctrl+Z");
        press("Ctrl+Z");
        st().selectOnly(ids.e2);
        press("Ctrl+C");
        st().currentTrack = ids.v1;
        st().setCursor(20.0);
        const size_t before = eventCount();
        answerDialog(QString(), 3);
        press("Ctrl+B");
        bool spaced = false;
        for (const Event& e : st().project().tracks[0].events)
            if (near(e.start, 26.0)) spaced = true;
        check(eventCount() == before + 3 && spaced, "Ctrl+B pastes three times, one after another", strfmt("%d", (int)(eventCount() - before)));
        press("Ctrl+Z");
        press("Ctrl+Z");
        // groups
        st().setSelection(std::set<uint64_t>{ids.e2, ids.e3});
        press("G");
        check(ev(ids.e2)->group && ev(ids.e2)->group == ev(ids.e3)->group, "G groups");
        press("U");
        check(!ev(ids.e2)->group && !ev(ids.e3)->group, "U takes them out of the group");
        st().selectOnly(ids.e1);
        press("Shift+G");
        check(st().selection() == std::set<uint64_t>{ids.e1, ids.e1a}, "Shift+G selects the whole group");
        st().selectOnly(ids.e1);
        press("Ctrl+U");
        check(!ev(ids.e1)->group && !ev(ids.e1a)->group, "Ctrl+U clears the group");
        press("Ctrl+Z");
        // pitch
        st().selectOnly(ids.e4);
        press("=");
        check(near(ev(ids.e4)->pitch, 1.0), "= raises the pitch a semitone");
        press("Ctrl+=");
        check(near(ev(ids.e4)->pitch, 1.01), "Ctrl+= a cent");
        press("Shift+=");
        check(near(ev(ids.e4)->pitch, 13.01), "Shift+= an octave");
        press("-");
        press("Ctrl+-");
        press("Shift+-");
        check(near(ev(ids.e4)->pitch, 0.0), "- , Ctrl+- and Shift+- lower it again", num(ev(ids.e4)->pitch));
        press("=");
        press("Ctrl+Shift+=");
        check(near(ev(ids.e4)->pitch, 0.0), "Ctrl+Shift+= resets it");
        // the keypad
        press("Alt+0");
        st().selectOnly(ids.e2);
        press(Qt::Key_3, Qt::KeypadModifier);
        check(near(ev(ids.e2)->start, 7.0 + fr()), "Num 3 moves the selected event a frame later");
        press(Qt::Key_1, Qt::KeypadModifier);
        check(near(ev(ids.e2)->start, 7.0), "Num 1 a frame earlier");
        press(Qt::Key_6, Qt::KeypadModifier);
        check(ev(ids.e2)->start > 7.0, "Num 6 a pixel later");
        press(Qt::Key_2, Qt::KeypadModifier);
        uint64_t track = 0;
        ev(ids.e2, &track);
        check(track == ids.v2, "Num 2 a track down");
        press(Qt::Key_8, Qt::KeypadModifier);
        ev(ids.e2, &track);
        check(track == ids.v1, "Num 8 a track up");
        st().setCursor(0.0);
        press(Qt::Key_9, Qt::KeypadModifier);
        check(near(st().cursor(), ev(ids.e2)->end()) && tl->edgeTrimming() && tl->trimSide() == 1, "Num 9 with an event selected selects its end for trimming",
              num(st().cursor()) + strfmt(" edge %d side %d event %d/%d", (int)tl->edgeTrimming(), tl->trimSide(), (int)tl->trimEvent(), (int)ids.e2));
        press("Esc");
        st().setSelection(std::set<uint64_t>());
        st().setCursor(0.0);
        press(Qt::Key_9, Qt::KeypadModifier);
        check(near(st().cursor(), 0.5), "Num 9 with nothing selected goes to the next event edge");
        // rename
        st().selectOnly(ids.e3);
        press("F2");
        QLineEdit* edit = visibleEdit(tl);
        check(edit != nullptr, "F2 opens a box to rename the selected event");
        if (edit) {
            edit->setText("Interview");
            uitest::key(edit, Qt::Key_Return);
            uitest::settle(20);
            check(ev(ids.e3)->name == "Interview", "and names it", ev(ids.e3)->name);
        }
    }

    // ---------------------------------------------------------------- auto ripple, grouping, snapping, quantizing, tools
    {
        buildProject();
        press("Ctrl+L");
        check(tl->options().autoRipple, "Ctrl+L turns auto ripple on");
        st().selectOnly(ids.e3);
        press("Delete");
        check(near(ev(ids.e5)->start, 5.0) && near(ev(ids.e6)->start, 7.0) && near(ev(ids.e2)->start, 7.0), "with auto ripple, Delete closes the gap on that track",
              num(ev(ids.e5)->start) + " " + num(ev(ids.e6)->start));
        press("Ctrl+Z");
        // all tracks and markers
        st().setCursor(12.5);
        press("M");
        tl->options().ripple = RippleMode::AllTracks;
        tl->optionsChanged();
        st().selectOnly(ids.e3);
        press("Delete");
        check(near(ev(ids.e2)->start, 2.0) && near(st().project().markers[0].time, 7.5), "in All Tracks mode every track and the markers close up",
              num(ev(ids.e2)->start) + " " + num(st().project().markers[0].time));
        press("Ctrl+Z");
        tl->options().ripple = RippleMode::AffectedTracks;
        tl->optionsChanged();
        // a move ripples what comes after it on the track
        st().selectOnly(ids.e5);
        const QRect v2 = tl->trackRect(ids.v2);
        uitest::mouseDrag(tl, QPoint(tl->xAt(11.0), v2.top() + 50), QPoint(tl->xAt(12.0), v2.top() + 50), 8, Qt::ShiftModifier);
        check(near(ev(ids.e5)->start, 11.0, fr()) && near(ev(ids.e6)->start, 13.0, fr()), "with auto ripple, moving an event moves what follows it",
              num(ev(ids.e5)->start) + " " + num(ev(ids.e6)->start));
        press("Ctrl+Z");
        // trimming an end does too
        const QRect v1 = tl->trackRect(ids.v1);
        st().selectOnly(ids.e1);
        uitest::mouseDrag(tl, QPoint(tl->xAt(5.0) - 1, v1.top() + 50), QPoint(tl->xAt(6.0) - 1, v1.top() + 50), 8, Qt::ShiftModifier);
        check(near(ev(ids.e1)->end(), 6.0, fr()) && near(ev(ids.e2)->start, 8.0, fr()), "with auto ripple, trimming an end moves what follows",
              num(ev(ids.e1)->end()) + " " + num(ev(ids.e2)->start));
        press("Ctrl+Z");
        press("Ctrl+L");
        check(!tl->options().autoRipple, "Ctrl+L turns it off");
        // ignore grouping
        press("Ctrl+Shift+U");
        check(tl->options().ignoreGrouping, "Ctrl+Shift+U ignores event grouping");
        st().selectOnly(ids.e1);
        st().setCursor(2.0);
        press("Alt+[");
        check(near(ev(ids.e1)->start, 2.0) && near(ev(ids.e1a)->start, 1.0), "... a trim leaves the grouped partner alone");
        press("Ctrl+Z");
        press("Ctrl+Shift+U");
        // snapping and quantizing
        const bool snap = st().snapping;
        press("F8");
        check(st().snapping != snap, "F8 switches snapping");
        press("F8");
        check(st().snapping == snap, "and back");
        press("Ctrl+F8");
        check(tl->options().snapToGrid, "Ctrl+F8: snap to grid");
        press("Shift+F8");
        check(!tl->options().snapToMarkers, "Shift+F8: snap to markers (on by default) off");
        press("Alt+F8");
        check(!tl->options().quantize, "Alt+F8: quantize to frames off");
        press("Alt+0");
        st().setCursor(2.0);
        press("Right");
        check(st().cursor() > 2.0 && st().cursor() < 2.0 + fr() * 0.9, "without quantizing, Right moves a pixel", num(st().cursor()));
        press("Alt+F8");
        press("Ctrl+F8");
        press("Shift+F8");
        // tools
        press("D");
        check(tl->options().tool == EditTool::Envelope, "D: the next edit tool (Envelope, as in VEGAS)");
        press("D");
        check(tl->options().tool == EditTool::Selection, "D again: Selection");
        press("D");
        check(tl->options().tool == EditTool::Zoom, "D again: Zoom");
        press("Shift+D");
        check(tl->options().tool == EditTool::Selection, "Shift+D: back");
        // the selection tool draws a rectangle
        const QRect r1 = tl->trackRect(ids.v1), r2 = tl->trackRect(ids.v2);
        uitest::mouseDrag(tl, QPoint(tl->xAt(1.5), r1.top() + 30), QPoint(tl->xAt(4.0), r2.top() + 30));
        check(st().selection() == std::set<uint64_t>{ids.e1, ids.e3}, "the selection tool selects what its rectangle touches", strfmt("%d", (int)st().selection().size()));
        press("Ctrl+D");
        check(tl->options().tool == EditTool::Normal, "Ctrl+D: the normal tool");
        tl->options().tool = EditTool::Zoom;
        tl->optionsChanged();
        tl->zoomToFit();
        uitest::mouseDrag(tl, QPoint(tl->xAt(4.0), r1.top() + 30), QPoint(tl->xAt(6.0), r1.top() + 30));
        check(near(tl->viewStartTime(), 4.0, 0.05) && near(tl->viewEndTime(), 6.0, 0.1), "the zoom tool zooms to what is dragged across",
              num(tl->viewStartTime()) + " " + num(tl->viewEndTime()));
        press("Ctrl+D");
        tl->zoomToFit();
    }

    // ---------------------------------------------------------------- envelopes
    {
        buildProject();
        st().currentTrack = ids.a1;
        auto has = [](size_t track, const char* kind) -> const Envelope* { return findEnvelope(st().project().tracks[track].envelopes, kind); };
        press("V");
        check(has(2, "volume") && has(2, "volume")->visible, "V puts a volume envelope on the selected track");
        press("V");
        check(has(2, "volume") && !has(2, "volume")->visible, "V again hides it");
        press("P");
        check(has(2, "pan") != nullptr, "P: a pan envelope");
        press("E");
        check(has(2, "volume")->visible, "E brings the next envelope to the front (showing it)");
        press("Shift+V");
        press("Shift+P");
        check(!has(2, "volume") && !has(2, "pan"), "Shift+V and Shift+P take them off");
        st().currentTrack = ids.v1;
        press("V");
        check(has(0, "composite") != nullptr, "V on a video track: composite level");
        press("Ctrl+Shift+E");
        check(!tl->options().showEnvelopes, "Ctrl+Shift+E hides the envelopes");
        press("Ctrl+Shift+E");
        check(tl->options().showEnvelopes, "and shows them");
        press("B");
        check(tl->options().showBusTracks, "B shows the bus tracks");
        press("B");
        check(!tl->options().showBusTracks, "and hides them");
        press("Ctrl+Shift+T");
        check(tl->options().showFadeLengths, "Ctrl+Shift+T shows event fade lengths");
        press("Ctrl+Shift+T");
    }

    // ---------------------------------------------------------------- tracks
    {
        buildProject();
        const size_t n = st().project().tracks.size();
        press("Ctrl+Q");
        check(st().project().tracks.size() == n + 1 && st().project().tracks.back().type == TrackType::Audio, "Ctrl+Q inserts an audio track");
        press("Ctrl+Shift+Q");
        check(st().project().tracks.size() == n + 2 && st().project().tracks[2].type == TrackType::Video, "Ctrl+Shift+Q inserts a video track (under the others)");
        buildProject();
        st().currentTrack = ids.v2;
        press("Z");
        check(st().project().tracks[1].mute, "Z mutes the selected track");
        press("Z");
        check(!st().project().tracks[1].mute, "Z again unmutes it");
        press("X");
        check(st().project().tracks[1].solo, "X solos it");
        st().currentTrack = ids.a1;
        press("Shift+X");
        check(st().project().tracks[2].solo && !st().project().tracks[1].solo, "Shift+X solos this one only");
        press("Shift+Z");
        check(st().project().tracks[2].mute, "Shift+Z mutes this one only");
        st().currentTrack = ids.v1;
        const int h0 = tl->trackRect(ids.v1).height();
        press("Ctrl+Shift+Up");
        check(tl->trackRect(ids.v1).height() > h0, "Ctrl+Shift+Up makes the tracks taller");
        press("Ctrl+Shift+Down");
        check(tl->trackRect(ids.v1).height() == h0, "Ctrl+Shift+Down shorter");
        press("`");
        check(tl->trackRect(ids.v1).height() < h0 / 2, "` minimizes the selected track");
        press("`");
        check(tl->trackRect(ids.v1).height() == h0, "and restores it");
        press("Ctrl+Shift+Up");
        press("`");
        press("Ctrl+`");
        check(tl->trackRect(ids.v1).height() == h0 && tl->trackRect(ids.v2).height() == h0, "Ctrl+` puts every track back to the default height");
        st().currentTrack = ids.v2;
        press("Alt+Shift+Down");
        check(st().currentTrack == ids.a1, "Alt+Shift+Down selects the next track");
        press("Alt+Shift+Up");
        press("Alt+Shift+Up");
        check(st().currentTrack == ids.v1, "Alt+Shift+Up the previous one");
        press("Ctrl+Shift+C");
        check(!tl->options().showEventButtons, "Ctrl+Shift+C hides the event buttons");
        press("Ctrl+Shift+C");
        check(tl->options().showEventButtons, "and shows them again");
        press("Ctrl+Alt+M");
        check(tl->options().eventHeaders, "Ctrl+Alt+M shows the event headers");
        press("Ctrl+Alt+M");
        tl->zoomToFit();
        st().setCursor(3.0);
        press("Alt+Up");
        check(tl->viewEndTime() - tl->viewStartTime() < 1.0 && tl->viewStartTime() < 3.0 && tl->viewEndTime() > 3.0, "Alt+Up zooms in until a thumbnail is a frame",
              num(tl->viewStartTime()) + " " + num(tl->viewEndTime()));
        tl->zoomToFit();
        // Shift+F10 (or the menu key): the selected event's menu
        st().selectOnly(ids.e3);
        bool eventMenu = false;
        QTimer::singleShot(150, [&] {
            if (QMenu* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : menu->actions()) eventMenu = eventMenu || a->text().startsWith("Split");
                menu->close();
            }
        });
        QContextMenuEvent kce(QContextMenuEvent::Keyboard, QPoint(5, 5), tl->mapToGlobal(QPoint(5, 5)));
        QApplication::sendEvent(tl, &kce);
        uitest::settle(50);
        check(eventMenu, "the menu key opens the selected event's menu");
        press("Ctrl+Shift+W");
        check(!tl->options().showWaveforms, "Ctrl+Shift+W hides the waveforms");
        press("Ctrl+Shift+W");
    }

    // ---------------------------------------------------------------- windows and layouts
    {
        check(shownDock("dockPreview") && shownDock("dockMaster") && shownDock("dockMedia") && !shownDock("dockNotes") && !shownDock("dockMixer"),
              "the default layout: Project Media, the Video Preview and the Master Bus");
        QDockWidget* preview = dockNamed("dockPreview");
        QDockWidget* master = dockNamed("dockMaster");
        check(preview && master && preview->geometry().right() < master->geometry().left() && preview->geometry().bottom() < tl->mapTo(&w, QPoint(0, 0)).y() &&
              preview->width() > 2 * master->width(),
              "the docking area is over the timeline: the preview, and the master bus at its right");
        check(dockNamed("dockMedia")->geometry().right() < preview->geometry().left(), "the windows' tabs at the left of the preview");
        press("Alt+7");
        check(shownDock("dockTransitions"), "Alt+7 brings Transitions to the front");
        press("Alt+8");
        check(shownDock("dockVideoFx"), "Alt+8 Video FX");
        press("Alt+9");
        check(shownDock("dockGenerators"), "Alt+9 Media Generators");
        press("Alt+6");
        check(shownDock("dockNotes"), "Alt+6 Project Notes");
        press("Ctrl+Alt+5");
        check(shownDock("dockMixer"), "Ctrl+Alt+5 the Mixing Console");
        press("Alt+5");
        check(shownDock("dockMedia"), "Alt+5 Project Media");
        check(w.tabifiedDockWidgets(dockNamed("dockMedia")).contains(dockNamed("dockMixer")) && w.tabifiedDockWidgets(dockNamed("dockMedia")).contains(dockNamed("dockNotes")),
              "... in the tab group with the others");
        dockNamed("dockPreview")->hide();
        press("Alt+4");
        check(shownDock("dockPreview"), "Alt+4 shows the Video Preview");
        dockNamed("dockMaster")->hide();
        press("Alt+3");
        check(shownDock("dockMaster"), "Alt+3 the Master Bus");
        press("Ctrl+Alt+D, 3");
        press("Alt+D, D");
        check(!shownDock("dockMixer") && !shownDock("dockNotes"), "Alt+D, D: back to the default layout");
        press("Alt+D, 3");
        check(dockNamed("dockMixer")->isVisible(), "Ctrl+Alt+D, 3 saved a layout that Alt+D, 3 brings back");
        press("Alt+D, D");
        press("Ctrl+F11");
        check(!shownDock("dockMaster") && !shownDock("dockPreview"), "Ctrl+F11 maximizes the timeline");
        press("Ctrl+F11");
        check(shownDock("dockMaster") && shownDock("dockPreview"), "and gives the windows back");
        press("F11");
        check(!shownDock("dockPreview"), "F11 minimizes the docking area");
        press("Alt+`");
        check(shownDock("dockPreview"), "Alt+` restores it");
        press("Alt+0");
        press("F6");
        check(!tl->hasFocus() && QApplication::focusWidget(), "F6 moves to the next window");
        press("Shift+F6");
        check(tl->hasFocus(), "Shift+F6 back to the timeline");
    }

    // ---------------------------------------------------------------- typing in a text box keeps its keys
    {
        // the Project Notes window: a note's text box, added and removed again
        QLineEdit* notes = nullptr;
        if (QWidget* nw = w.findChild<QWidget*>("projectNotes"))
            if (QPushButton* add = nw->findChild<QPushButton*>("addNote")) {
                add->click();
                uitest::settle(30);
                const QList<QLineEdit*> edits = nw->findChildren<QLineEdit*>("noteText");
                if (!edits.isEmpty()) notes = edits.last();
            }
        bool guarded = false;
        if (notes) {
            QKeyEvent over(QEvent::ShortcutOverride, Qt::Key_S, Qt::NoModifier, "s");
            QApplication::sendEvent(notes, &over);
            guarded = over.isAccepted();
        }
        check(guarded, "a text box keeps plain keys while typing (S does not split)");
        QKeyEvent ctrl(QEvent::ShortcutOverride, Qt::Key_Q, Qt::ControlModifier | Qt::ShiftModifier);
        if (notes) QApplication::sendEvent(notes, &ctrl);
        check(!ctrl.isAccepted(), "but not keys with Ctrl");
        if (notes && AppState::get().undoName() == "Add Note") AppState::get().undo();
    }

    // ---------------------------------------------------------------- files
    {
        buildProject();
        const QString path = QDir(out).absoluteFilePath("keys_project.reno");
        QFile::remove(path);
        QFile::remove(QDir(out).absoluteFilePath("keys_project_01.reno"));
        std::string err;
        st().save(path.toStdString(), &err);
        st().edit("Change", [](Project& p) { p.settings.title = "changed"; });
        press("Ctrl+S");
        check(!st().dirty(), "Ctrl+S saves");
        press("Ctrl+Alt+S");
        check(QFileInfo(QString::fromStdString(st().path())).fileName() == "keys_project_01.reno" && QFileInfo::exists(QDir(out).absoluteFilePath("keys_project_01.reno")),
              "Ctrl+Alt+S saves a numbered version", st().path());
        press("Ctrl+Shift+N");
        check(st().project().tracks.empty() && st().path().empty(), "Ctrl+Shift+N: a new project");
        w.openProject(path);
        check(st().project().tracks.size() == 4 && st().path() == path.toStdString(), "a saved project opens again");
        press("Ctrl+F4");
        check(st().project().tracks.empty(), "Ctrl+F4 closes it");
    }

    // ---------------------------------------------------------------- the Mixing Console's own keys win while it has the focus
    {
        press("Ctrl+Alt+5");
        uitest::settle(100);
        QDockWidget* md = dockNamed("dockMixer");
        QWidget* mixer = md ? md->findChild<QWidget*>("mixer") : nullptr;
        check(mixer && md->isVisible(), "Ctrl+Alt+5 shows the Mixing Console");
        if (mixer) {
            press(Qt::Key_D);
            mixer->setFocus();
            QCoreApplication::processEvents();
            const size_t markers = st().project().markers.size(), tracks = st().project().tracks.size();
            const int width = mixer->property("channelWidth").toInt();
            press(Qt::Key_N);
            press(Qt::Key_M);
            press(Qt::Key_M);
            check(mixer->property("channelWidth").toInt() == 0 && width == 1 && st().project().markers.size() == markers,
                  "with the console focused, N narrows its channels and M is its meters (not Insert Marker)", strfmt("width %d -> %d, markers %d -> %d", width, mixer->property("channelWidth").toInt(), (int)markers, (int)st().project().markers.size()));
            press("Ctrl+Q");
            check(st().project().tracks.size() == tracks + 1 && st().undoName() == "Insert Audio Track", "and Ctrl+Q inserts an audio track there too");
            st().undo();
            press(Qt::Key_D);
        }
    }

    // ---------------------------------------------------------------- the shortcut list
    {
        w.showShortcuts();
        uitest::settle(100);
        QWidget* dlg = nullptr;
        for (QWidget* t : QApplication::topLevelWidgets())
            if (t->objectName() == "shortcutsDialog" && t->isVisible()) dlg = t;
        QTreeWidget* tree = dlg ? dlg->findChild<QTreeWidget*>("shortcutList") : nullptr;
        QLineEdit* search = dlg ? dlg->findChild<QLineEdit*>("shortcutSearch") : nullptr;
        int all = 0;
        if (tree)
            for (int k = 0; k < tree->topLevelItemCount(); ++k) all += tree->topLevelItem(k)->childCount();
        check(tree && all == (int)shortcutList().size(), "Help > Keyboard Shortcuts lists every key", strfmt("%d", all));
        if (search) search->setText("ripple");
        int found = 0;
        if (tree)
            for (int k = 0; k < tree->topLevelItemCount(); ++k) found += tree->topLevelItem(k)->childCount();
        check(found > 0 && found < all, "and can be searched", strfmt("%d", found));
        if (dlg) uitest::shot(dlg, out + "/keys_shortcuts.png");
        if (dlg) dlg->hide();
    }
    return uitest::summary();
}
