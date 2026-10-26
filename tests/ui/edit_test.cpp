// Editing commands that follow VEGAS Pro: keyboard edge trimming, jog, slip / slide / time stretch (keypad and mouse), locked
// events, takes, Smart Split, cut and transition conversion, post-edit ripple, CD and command markers, Select Loop Region,
// Snap to All Events, Automatic Crossfades, track groups, multicamera, Open in Audio Editor, and help.
// Keys go in through the window system like a keyboard's (as in keys_test), so Qt's shortcut handling decides where they land.
//
//   edit_test <output folder>
//
// Needs keys_test's extra include folders. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/engine/engine_internal.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QWhatsThis>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;

struct Ids {
    uint64_t v1 = 0, v2 = 0, a1 = 0, a2 = 0;
    uint64_t clip = 0, clip2 = 0, pic = 0, snd = 0;
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

// As in keys_test:
//   V1: e1 clip 1..5 (grouped with e1a on A1), e2 still 7..10
//   V2: e3 clip 2..7 (offset 1), e5 clip 10..14, e6 clip 12..16 (offset 3)
//   A1: e1a clip sound 1..5        A2: e4 sound only 0.5..6.5
void buildProject()
{
    st().newProject();
    std::string err;
    ids.clip = st().importMedia("synth:1", &err);
    ids.clip2 = st().importMedia("synth:2", &err);
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
        gW->timeline()->exitEditMode();
        gW->timeline()->options() = TimelineOptions();
        gW->timeline()->optionsChanged();
        gW->timeline()->zoomToFit();
        gW->timeline()->setSelectedTracks(std::set<uint64_t>());
        gW->timeline()->setTrackListFocus(false);
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
const Track* track(uint64_t id)
{
    for (const Track& t : st().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

QString textOf(int key, Qt::KeyboardModifiers mods)
{
    if (mods & (Qt::ControlModifier | Qt::AltModifier)) return QString();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar((mods & Qt::ShiftModifier) ? key : key - Qt::Key_A + 'a'));
    if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) return QString(QChar(key));
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

void press(const char* seq)
{
    const QKeySequence ks(QString::fromUtf8(seq));
    for (int k = 0; k < ks.count(); ++k) press(ks[k].key(), ks[k].keyboardModifiers());
}

// A keypad key (Num Lock on), with modifiers.
void kp(int key, Qt::KeyboardModifiers mods = Qt::NoModifier) { press(key, mods | Qt::KeypadModifier); }

QAction* action(const char* name)
{
    for (QAction* a : gW->findChildren<QAction*>())
        if (a->objectName() == name) return a;
    return nullptr;
}

QPoint at(TimelineWidget* w, uint64_t trackId, double t, int y = -1)
{
    const QRect r = w->trackRect(trackId);
    return QPoint(w->xAt(t), y < 0 ? r.center().y() + 6 : r.top() + y);
}

// Answers the next modal dialog called 'name' with 'fn' (which fills it in), then OK.
void answer(const char* name, std::function<void(QDialog*)> fn)
{
    QTimer::singleShot(150, [name, fn] {
        for (QWidget* t : QApplication::topLevelWidgets())
            if (QDialog* d = qobject_cast<QDialog*>(t))
                if (d->isVisible() && d->objectName() == name) {
                    fn(d);
                    d->accept();
                }
    });
}

// A tiny WAV (a second of silence at 8 kHz) for the audio editor test.
bool writeWav(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const int rate = 8000, n = 8000;
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); u32(36 + n * 2); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    f.write("data", 4); u32(n * 2);
    f.write(QByteArray(n * 2, 0));
    return true;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEditTest");
    QApplication::setApplicationName("edit_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("edit");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1600, 900);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    buildProject();
    TimelineWidget* tl = w.timeline();
    uitest::settle(100);
    const double F = fr();

    // ---------------------------------------------------------------- keyboard edge trimming
    {
        press("Alt+0");
        st().selectOnly(ids.e3);
        press("]");
        check(tl->edgeTrimming() && tl->trimEvent() == ids.e3 && tl->trimSide() == 1 && near(st().cursor(), 7.0),
              "] with an event selected selects its end for trimming (the cursor goes there)", num(st().cursor()));
        kp(Qt::Key_3);
        check(near(ev(ids.e3)->length, 5.0 + F) && near(st().cursor(), 7.0 + F), "Num 3 trims the selected end a frame longer", num(ev(ids.e3)->length));
        kp(Qt::Key_1);
        check(near(ev(ids.e3)->length, 5.0), "Num 1 a frame shorter");
        kp(Qt::Key_6);
        check(ev(ids.e3)->length > 5.0 + F * 0.5, "Num 6 a pixel longer (at least a frame while quantizing)", num(ev(ids.e3)->length));
        kp(Qt::Key_4);
        check(near(ev(ids.e3)->length, 5.0, 1e-6), "Num 4 a pixel shorter", num(ev(ids.e3)->length));
        kp(Qt::Key_3, Qt::ControlModifier);
        check(near(ev(ids.e3)->length, 5.0 + F) && near(ev(ids.e3)->rate, 5.0 / (5.0 + F), 1e-9), "Ctrl+Num 3 time-stretches the edge (the rate changes)",
              num(ev(ids.e3)->rate));
        press("Ctrl+Z");
        kp(Qt::Key_3, Qt::AltModifier);
        check(near(ev(ids.e3)->length, 5.0 + F) && near(ev(ids.e3)->offset, 1.0 - F) && near(ev(ids.e3)->start, 2.0),
              "Alt+Num 3 slip-trims the end (the media moves with it)", num(ev(ids.e3)->offset));
        press("Ctrl+Z");
        press("[");
        check(tl->trimEvent() == ids.e3 && tl->trimSide() == -1 && near(st().cursor(), 2.0), "[ again: the previous edge on the track (the event's start)",
              num(st().cursor()));
        kp(Qt::Key_3);
        check(near(ev(ids.e3)->start, 2.0 + F) && near(ev(ids.e3)->offset, 1.0 + F) && near(ev(ids.e3)->length, 5.0 - F),
              "Num 3 on a start trims it, the picture staying where it was", num(ev(ids.e3)->start) + " " + num(ev(ids.e3)->offset));
        press("Ctrl+Z");
        press("Esc");
        check(!tl->edgeTrimming(), "Esc leaves edge trimming");
        // a grouped event's edge takes its group's along; Shift does not
        st().selectOnly(ids.e1);
        press("]");
        kp(Qt::Key_3);
        check(near(ev(ids.e1)->length, 4.0 + F) && near(ev(ids.e1a)->length, 4.0 + F), "trimming a grouped event's end trims its sound too");
        kp(Qt::Key_1, Qt::ShiftModifier);
        check(near(ev(ids.e1)->length, 4.0) && near(ev(ids.e1a)->length, 4.0 + F), "Shift+Num 1 trims ignoring the group");
        press("Ctrl+Z");
        press("Ctrl+Z");
        press("Esc");
        // F9 / F3 jog: in edge trimming a frame's trim, else the cursor moves a frame
        st().selectOnly(ids.e5);
        press("]");
        press("F9");
        check(near(ev(ids.e5)->length, 4.0 + F), "F9 in edge trimming trims the edge a frame", num(ev(ids.e5)->length));
        press("Ctrl+Z");
        press("Esc");
        st().setSelection(std::set<uint64_t>());
        st().setCursor(3.0);
        // a jog plays a moment of sound; the cursor shows what is heard until it ends, so wait for it
        auto jogDone = [&] {
            for (int k = 0; k < 100 && w.playback()->playing(); ++k) uitest::settle(20);
        };
        press("F9");
        jogDone();
        check(near(st().cursor(), 3.0 + F), "F9 jogs the cursor a frame on", num(st().cursor()));
        press("F3");
        jogDone();
        check(near(st().cursor(), 3.0), "F3 a frame back", num(st().cursor()));
        w.playback()->pause();
        // with nothing selected, [ and ] and Num 7 / 9 still go to the event edges
        st().setCursor(0.0);
        kp(Qt::Key_9);
        check(!tl->edgeTrimming() && near(st().cursor(), 0.5), "Num 9 with nothing selected: the cursor to the next event edge", num(st().cursor()));
    }

    // ---------------------------------------------------------------- expanded edit mode: both sides of an edit
    {
        buildProject();
        press("Alt+0");
        st().currentTrack = ids.v2;
        st().setCursor(13.9);
        kp(Qt::Key_5);
        check(tl->edgeTrimming() && tl->trimEvent() == ids.e5 && tl->trimSide() == 0, "Num 5: expanded edit mode on the edit nearest the cursor (e5 into e6)");
        kp(Qt::Key_3);
        check(near(ev(ids.e5)->end(), 14.0 + F) && near(ev(ids.e6)->start, 12.0 + F) && near(ev(ids.e6)->offset, 3.0 + F),
              "Num 3 slides the cross-fade: both edges move a frame", num(ev(ids.e5)->end()) + " " + num(ev(ids.e6)->start));
        kp(Qt::Key_7);
        kp(Qt::Key_1);
        check(near(ev(ids.e5)->end(), 14.0) && near(ev(ids.e6)->start, 12.0 + F), "Num 7 picks the end of the event before: Num 1 trims only it");
        kp(Qt::Key_9);
        kp(Qt::Key_9);
        kp(Qt::Key_1);
        check(near(ev(ids.e6)->start, 12.0) && near(ev(ids.e5)->end(), 14.0), "Num 9 the start of the one after");
        kp(Qt::Key_5);
        check(!tl->edgeTrimming(), "Num 5 again leaves");
        // slide keys from an edge: Ctrl+Alt+Num moves the edit (the neighbour's edge goes along)
        st().selectOnly(ids.e5);
        press("]");
        kp(Qt::Key_3, Qt::ControlModifier | Qt::AltModifier);
        check(near(ev(ids.e5)->end(), 14.0 + F) && near(ev(ids.e6)->start, 12.0 + F), "Ctrl+Alt+Num 3 on an edge slides the edit");
        press("Esc");
    }

    // ---------------------------------------------------------------- keypad on selected events: slip, slide, stretch
    {
        buildProject();
        press("Alt+0");
        st().selectOnly(ids.e3);
        kp(Qt::Key_3, Qt::AltModifier);
        check(near(ev(ids.e3)->start, 2.0) && near(ev(ids.e3)->offset, 1.0 - F), "Alt+Num 3 slips the media inside the selected event", num(ev(ids.e3)->offset));
        kp(Qt::Key_3, Qt::ControlModifier | Qt::AltModifier);
        check(near(ev(ids.e3)->start, 2.0 + F) && near(ev(ids.e3)->offset, 1.0), "Ctrl+Alt+Num 3 slides it over its media", num(ev(ids.e3)->start));
        kp(Qt::Key_6, Qt::ControlModifier);
        check(ev(ids.e3)->length > 5.0 && ev(ids.e3)->rate < 1.0, "Ctrl+Num 6 time-stretches its end", num(ev(ids.e3)->rate));
        st().selectOnly(ids.e1);
        kp(Qt::Key_3, Qt::ControlModifier);
        check(near(ev(ids.e1)->rate, ev(ids.e1a)->rate, 1e-12) && ev(ids.e1)->rate < 1.0 && near(ev(ids.e1a)->length, 4.0 + F),
              "stretching a clip's picture stretches its sound alike (same rate and length)", num(ev(ids.e1a)->rate));
        kp(Qt::Key_3, Qt::ShiftModifier);
        check(near(ev(ids.e1)->start, 1.0 + F) && near(ev(ids.e1a)->start, 1.0), "Shift+Num 3 moves ignoring the group");
    }

    // ---------------------------------------------------------------- the mouse: slip, slide, cross-fade, split trim, ranges
    {
        buildProject();
        const double pps = tl->xAt(1.0) - tl->xAt(0.0);
        st().selectOnly(ids.e3);
        uitest::mouseDrag(tl, at(tl, ids.v2, 4.0), at(tl, ids.v2, 4.0) + QPoint((int)std::lround(pps * 0.5), 0), 8, Qt::AltModifier);
        check(near(ev(ids.e3)->start, 2.0) && ev(ids.e3)->offset < 0.9, "Alt+drag inside an event slips its media", num(ev(ids.e3)->offset));
        press("Ctrl+Z");
        uitest::mouseDrag(tl, at(tl, ids.v2, 4.0), at(tl, ids.v2, 4.0) + QPoint((int)std::lround(pps * 0.5), 0), 8, Qt::ControlModifier | Qt::AltModifier);
        check(ev(ids.e3)->start > 2.3 && near(ev(ids.e3)->offset - 1.0, ev(ids.e3)->start - 2.0, 1e-6), "Ctrl+Alt+drag slides the event over its media",
              num(ev(ids.e3)->start));
        press("Ctrl+Z");
        // Ctrl+Alt over the cross-fade of e5 and e6 moves it
        uitest::mouseDrag(tl, at(tl, ids.v2, 13.0), at(tl, ids.v2, 13.0) + QPoint((int)std::lround(pps * 0.5), 0), 8, Qt::ControlModifier | Qt::AltModifier);
        check(ev(ids.e6)->start > 12.3 && ev(ids.e5)->end() > 14.3 && near(ev(ids.e6)->start - 12.0, ev(ids.e5)->end() - 14.0, 1e-6),
              "Ctrl+Alt+drag on a cross-fade moves the cross-fade", num(ev(ids.e6)->start) + " " + num(ev(ids.e5)->end()));
        press("Ctrl+Z");
        // Ctrl+Alt+Shift+click splits where clicked
        const size_t before = eventCount();
        uitest::mouseClick(tl, at(tl, ids.v2, 4.0), Qt::LeftButton, Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
        check(eventCount() == before + 1 && ev(ids.e3)->end() < 4.2, "Ctrl+Alt+Shift+click splits the event there", num(ev(ids.e3)->end()));
        press("Ctrl+Z");
        // Shift+click: the range from the event clicked last
        uitest::mouseClick(tl, at(tl, ids.v1, 3.0));
        uitest::mouseClick(tl, at(tl, ids.v2, 6.0), Qt::LeftButton, Qt::ShiftModifier);
        check(st().selection().count(ids.e1) && st().selection().count(ids.e3) && !st().selection().count(ids.e5), "Shift+click selects the range of events",
              strfmt("%d selected", (int)st().selection().size()));
        // Ctrl+Shift+drag over events makes a time selection instead of moving them
        uitest::mouseDrag(tl, at(tl, ids.v2, 3.0), at(tl, ids.v2, 5.0), 8, Qt::ControlModifier | Qt::ShiftModifier);
        check(near(ev(ids.e3)->start, 2.0) && st().loopEnd > st().loopStart + 1.5, "Ctrl+Shift+drag over an event selects time", num(st().loopStart) + " " + num(st().loopEnd));
        // Ctrl+drag of an edge: grouped picture and sound get the same rate
        st().setLoop(0, 0);
        st().selectOnly(ids.e1);
        uitest::mouseDrag(tl, QPoint(tl->xAt(5.0) - 1, at(tl, ids.v1, 3.0).y()), QPoint(tl->xAt(6.0), at(tl, ids.v1, 3.0).y()), 8, Qt::ControlModifier);
        check(ev(ids.e1)->rate < 0.95 && near(ev(ids.e1)->rate, ev(ids.e1a)->rate, 1e-12) && near(ev(ids.e1)->length, ev(ids.e1a)->length),
              "Ctrl+drag of a grouped edge: picture and sound stretched alike", num(ev(ids.e1)->rate) + " " + num(ev(ids.e1a)->rate));
        tl->update();
        uitest::settle(50);
        uitest::shot(&w, out + "/edit_mouse.png");
    }

    // ---------------------------------------------------------------- locked events
    {
        buildProject();
        press("Alt+0");
        st().selectOnly(ids.e3);
        if (QAction* lock = action("lockEvent")) lock->trigger();
        check(ev(ids.e3)->locked, "Edit > Switches > Lock locks the selected event");
        press("Del");
        check(ev(ids.e3) != nullptr, "a locked event is not deleted");
        kp(Qt::Key_3);
        check(near(ev(ids.e3)->start, 2.0), "nor moved by the keypad");
        st().setCursor(4.0);
        press("S");
        check(near(ev(ids.e3)->end(), 7.0), "nor split");
        uitest::mouseDrag(tl, at(tl, ids.v2, 4.0), at(tl, ids.v2, 5.0));
        check(near(ev(ids.e3)->start, 2.0), "nor dragged");
        press("]");
        kp(Qt::Key_3);
        check(near(ev(ids.e3)->length, 5.0), "nor trimmed");
        press("Esc");
        tl->update();
        uitest::settle(50);
        uitest::shot(tl, out + "/edit_locked.png");
        st().selectOnly(ids.e3);
        if (QAction* lock = action("lockEvent")) lock->trigger();
        check(!ev(ids.e3)->locked, "and Lock again unlocks it");
    }

    // ---------------------------------------------------------------- takes
    {
        buildProject();
        press("Alt+0");
        check(st().addTakes(ids.e3, std::vector<uint64_t>{ids.clip2, ids.snd}) == 1 && ev(ids.e3)->takes.size() == 1,
              "Add Take: media with pictures becomes another take of a video event (sound-only media does not)");
        st().selectOnly(ids.e3);
        press("T");
        check(ev(ids.e3)->mediaId == ids.clip2 && ev(ids.e3)->activeTake == 1 && ev(ids.e3)->takes[0].mediaId == ids.clip, "T: the next take", ev(ids.e3)->name);
        press("Shift+T");
        check(ev(ids.e3)->mediaId == ids.clip && ev(ids.e3)->activeTake == 0 && near(ev(ids.e3)->offset, 1.0), "Shift+T: the previous one");
        press("Ctrl+Shift+I");
        check(tl->options().showTakeInfo, "Ctrl+Shift+I shows the active take's information");
        // a split keeps the other takes in step
        st().setCursor(4.0);
        press("S");
        const Track* t = track(ids.v2);
        const Event* right = nullptr;
        for (const Event& e : t->events)
            if (near(e.start, 4.0)) right = &e;
        check(right && right->takes.size() == 1 && near(right->takes[0].offset, 2.0), "a split moves the other takes' offsets along",
              right && !right->takes.empty() ? num(right->takes[0].offset) : std::string("no right half"));
        press("Ctrl+Z");
        // Choose Active... and Delete Active
        st().selectOnly(ids.e3);
        answer("chooseTakeDialog", [](QDialog* d) {
            if (QListWidget* l = d->findChild<QListWidget*>("takeList")) l->setCurrentRow(1);
        });
        if (QAction* a = action("takeChoose")) a->trigger();
        check(ev(ids.e3)->mediaId == ids.clip2, "Edit > Take > Choose Active...");
        if (QAction* a = action("takeDeleteActive")) a->trigger();
        check(ev(ids.e3)->mediaId == ids.clip && ev(ids.e3)->takes.empty(), "Edit > Take > Delete Active");
        // dropping media with Ctrl on an event adds it as a take
        QMimeData md;
        md.setData(kMimeMedia, QByteArray::number((qulonglong)ids.clip2));
        const QPoint p = at(tl, ids.v2, 4.0);
        QDragEnterEvent de(p, Qt::CopyAction, &md, Qt::LeftButton, Qt::ControlModifier);
        QApplication::sendEvent(tl, &de);
        QDragMoveEvent dm(p, Qt::CopyAction, &md, Qt::LeftButton, Qt::ControlModifier);
        QApplication::sendEvent(tl, &dm);
        QDropEvent dr(QPointF(p), Qt::CopyAction, &md, Qt::LeftButton, Qt::ControlModifier);
        QApplication::sendEvent(tl, &dr);
        check(ev(ids.e3)->takes.size() == 1 && ev(ids.e3)->takes[0].mediaId == ids.clip2 && ev(ids.e3)->mediaId == ids.clip, "Ctrl+drop of media on an event adds a take");
        tl->update();
        uitest::settle(50);
        uitest::shot(tl, out + "/edit_takes.png");
    }

    // ---------------------------------------------------------------- Smart Split
    {
        buildProject();
        st().selectOnly(ids.e3);
        st().setLoop(3.0, 4.0);
        const size_t before = eventCount();
        press("Alt+S");
        const Track* t = track(ids.v2);
        const Event* r = nullptr;
        for (const Event& e : t->events)
            if (e.id != ids.e3 && e.mediaId == ids.clip && e.start < 3.0 && e.start > 2.5) r = &e;
        check(eventCount() == before + 1 && near(ev(ids.e3)->end(), 3.0) && r && near(r->start, 3.0 - 5 * F) && near(r->offset, 3.0) &&
                  r->group == ev(ids.e3)->group && r->group != 0,
              "Alt+S takes the time selection out and joins the rest with a five-frame overlap, grouped",
              r ? num(r->start) + " " + num(r->offset) : std::string("no right part"));
        press("Ctrl+Z");
        check(eventCount() == before && near(ev(ids.e3)->end(), 7.0), "and Undo puts it back");
    }

    // ---------------------------------------------------------------- converting cuts and transitions
    {
        buildProject();
        st().edit("Cut", [](Project& p) { if (Event* e = p.findEvent(ids.e5)) e->length = 2.0; });  // e5 10..12 meets e6 at 12
        press("Alt+0");
        st().selectOnly(ids.e5);
        st().setCursor(12.0);
        kp(Qt::Key_Slash);
        check(near(ev(ids.e5)->end(), 12.5) && near(ev(ids.e6)->start, 11.5) && near(ev(ids.e6)->offset, 2.5),
              "Num / turns the cut into a one-second cross-fade centred on it", num(ev(ids.e5)->end()) + " " + num(ev(ids.e6)->start));
        kp(Qt::Key_Slash, Qt::ControlModifier);
        check(near(ev(ids.e5)->end(), 12.0) && near(ev(ids.e6)->start, 12.0) && near(ev(ids.e6)->offset, 3.0), "Ctrl+Num / turns it back into a cut",
              num(ev(ids.e5)->end()) + " " + num(ev(ids.e6)->start));
        kp(Qt::Key_Asterisk);
        check(ev(ids.e6)->start < 12.0, "Num * makes a transition too (Dissolve when installed)");
    }

    // ---------------------------------------------------------------- post-edit ripple
    {
        buildProject();
        press("Alt+0");
        st().selectOnly(ids.e1);
        press("]");
        kp(Qt::Key_3);
        press("Esc");
        press("F");
        check(near(ev(ids.e2)->start, 7.0 + F) && near(ev(ids.e4)->start, 0.5), "F after trimming an end: later events on those tracks follow",
              num(ev(ids.e2)->start));
        press("F");
        check(near(ev(ids.e2)->start, 7.0 + F), "F again does nothing (only straight after an edit)");
        st().selectOnly(ids.e1);
        press("Del");
        press("F");
        check(near(ev(ids.e2)->start, 3.0), "F after a delete closes the gap", num(ev(ids.e2)->start));
        buildProject();
        st().selectOnly(ids.e5);
        press("Del");
        st().edit("Marker", [](Project& p) { Project::Marker m; m.id = p.newId(); m.time = 15.0; p.markers.push_back(m); });
        press("Del");  // nothing selected: no edit
        st().selectOnly(ids.e3);
        press("Del");
        press("Ctrl+Shift+F");
        check(near(ev(ids.e6)->start, 7.0) && near(ev(ids.e2)->start, 2.0) && near(st().project().markers[0].time, 10.0),
              "Ctrl+Shift+F ripples all tracks, markers and regions", num(ev(ids.e6)->start) + " " + num(ev(ids.e2)->start));
    }

    // ---------------------------------------------------------------- loop region, CD layout and command markers
    {
        buildProject();
        st().setLoop(3.0, 4.0);
        st().setLoop(0, 0);
        press("Shift+Q");
        check(near(st().loopStart, 3.0) && near(st().loopEnd, 4.0), "Shift+Q selects the loop region again");
        st().setLoop(1.0, 2.0);
        press("N");
        st().setLoop(0, 0);
        st().setCursor(5.0);
        press("Shift+N");
        const auto& ms = st().project().markers;
        check(ms.size() == 2 && ms[0].kind == "cdtrack" && near(ms[0].time, 1.0) && near(ms[0].length, 1.0) && ms[1].kind == "cdindex" && near(ms[1].time, 5.0),
              "N inserts a CD track region over the time selection, Shift+N a CD index at the cursor");
        st().setCursor(0.0);
        press(".");
        check(near(st().cursor(), 1.0), "Period: the next CD index or region", num(st().cursor()));
        press(".");
        press(".");
        check(near(st().cursor(), 5.0), "and on to the index", num(st().cursor()));
        press("Ctrl+,");
        check(near(st().cursor(), 1.0), "Ctrl+Comma: the previous CD track", num(st().cursor()));
        press(",");
        check(near(st().cursor(), 0.0) || near(st().cursor(), 1.0), "Comma goes back");
        st().setCursor(6.0);
        answer("commandMarkerDialog", [](QDialog* d) {
            if (QComboBox* c = d->findChild<QComboBox*>("command")) c->setCurrentText("URL");
            if (QLineEdit* e = d->findChild<QLineEdit*>("parameter")) e->setText("https://example.com");
        });
        press("C");
        bool found = false;
        for (const auto& m : st().project().markers)
            if (m.kind == "command" && m.command == "URL" && m.name == "https://example.com" && near(m.time, 6.0)) found = true;
        check(found, "C inserts a command marker at the cursor");
        tl->update();
        uitest::settle(50);
        uitest::shot(tl, out + "/edit_markers.png");
    }

    // ---------------------------------------------------------------- options: snap to all events, automatic crossfades
    {
        buildProject();
        press("Ctrl+Shift+F8");
        check(!tl->options().snapToAllEvents && action("snapAllEvents") && !action("snapAllEvents")->isChecked(), "Ctrl+Shift+F8 turns Snap to All Events off");
        press("Ctrl+Shift+F8");
        check(tl->options().snapToAllEvents, "and on");
        const Track* v2 = track(ids.v2);
        const Event* e5 = ev(ids.e5);
        const Event* e6 = ev(ids.e6);
        const double cross = overlapWeight(*v2, *e5, 13.0, true);
        check(cross > 0.2 && cross < 0.8 && overlapWeight(*v2, *e5, 13.0, false) == 0.0 && overlapWeight(*v2, *e6, 13.0, false) == 1.0,
              "without cross-fades the later event covers the earlier one", num(cross));
        const QImage with = w.playback()->renderNow(13.0);
        press("Ctrl+Shift+X");
        check(!st().autoCrossfades() && !st().project().autoCrossfades && !st().snapshot()->autoCrossfades, "Ctrl+Shift+X turns Automatic Crossfades off");
        const QImage without = w.playback()->renderNow(13.0);
        check(!with.isNull() && !without.isNull() && with != without, "and the overlap is no longer cross-faded in the picture");
        st().edit("Change", [](Project& p) { p.settings.title = "x"; });
        press("Ctrl+Z");
        check(!st().project().autoCrossfades, "the option stays as it is through Undo");
        press("Ctrl+Shift+X");
        check(st().autoCrossfades() && st().project().autoCrossfades, "and back on");
    }

    // ---------------------------------------------------------------- track list: Tab, volume and pan keys, groups
    {
        buildProject();
        press("Alt+0");
        st().currentTrack = ids.a1;
        tl->setTrackListFocus(true);
        press("Ctrl+Right");
        check(near(20.0 * std::log10(track(ids.a1)->level), 0.5, 1e-6) && near(st().cursor(), 0.0), "Ctrl+Right with the track list focused: volume up half a decibel",
              num(track(ids.a1)->level));
        press("Ctrl+Shift+Left");
        check(near(track(ids.a1)->pan, -0.01), "Ctrl+Shift+Left: pan left", num(track(ids.a1)->pan));
        press(Qt::Key_Tab);
        check(!tl->trackListFocused() && tl->hasFocus(), "Tab gives the keyboard back to the timeline");
        st().edit("Marker", [](Project& p) { Project::Marker m; m.id = p.newId(); m.time = 3.0; p.markers.push_back(m); });
        press("Ctrl+Right");
        check(near(st().cursor(), 3.0), "where Ctrl+Right goes to the next marker again", num(st().cursor()));
        press(Qt::Key_Tab);
        check(tl->trackListFocused(), "Tab again: the track list");
        check(tl->selectedTracks() == std::set<uint64_t>{ids.a1}, "only the current track is selected", strfmt("%d", (int)tl->selectedTracks().size()));
        tl->update();
        uitest::settle(50);
        uitest::shot(tl, out + "/edit_tracklist.png");
        press(Qt::Key_Tab);
        // group two tracks, then mute and solo the group
        st().currentTrack = ids.v1;
        tl->setSelectedTracks(std::set<uint64_t>{ids.v1, ids.v2});
        tl->groupTracks(true);
        check(track(ids.v1)->trackGroup != 0 && track(ids.v1)->trackGroup == track(ids.v2)->trackGroup && track(ids.a1)->trackGroup == 0,
              "Group Selected Tracks makes a track group");
        press("Ctrl+Alt+Z");
        check(track(ids.v1)->mute && track(ids.v2)->mute && !track(ids.a1)->mute, "Ctrl+Alt+Z mutes the track group");
        press("Ctrl+Alt+Z");
        check(!track(ids.v1)->mute && !track(ids.v2)->mute, "and again unmutes it");
        st().edit("Solo", [](Project& p) { p.findTrack(ids.a2)->solo = true; });
        press("Ctrl+Alt+Shift+X");
        check(track(ids.v1)->solo && track(ids.v2)->solo && !track(ids.a2)->solo, "Ctrl+Alt+Shift+X solos the group only");
        tl->collapseTrackGroup(true);
        check(tl->trackRect(ids.v2).height() == 0 && tl->trackRect(ids.v1).height() > 0, "Collapse shows the group as its first track");
        tl->collapseTrackGroup(false);
        check(tl->trackRect(ids.v2).height() > 0, "Expand shows it again");
        // Shift+F11: track list as narrow as it goes, and back
        const int wide = tl->headerWidth();
        press("Shift+F11");
        check(tl->headerWidth() < wide, "Shift+F11 minimizes the track list", strfmt("%d -> %d", wide, tl->headerWidth()));
        press("Shift+Alt+`");
        check(tl->headerWidth() == wide, "Shift+Alt+` restores it");
        // wheel button held and mouse moved scrolls the view
        tl->zoomIn();
        tl->zoomIn();
        const double v0 = tl->viewStartTime();
        const QPoint mid = tl->trackRect(ids.v2).center();
        uitest::mousePress(tl, mid, Qt::MiddleButton);
        uitest::mouseMove(tl, mid + QPoint(150, 0), Qt::MiddleButton);
        uitest::settle(300);
        uitest::mouseRelease(tl, mid + QPoint(150, 0), Qt::MiddleButton);
        check(tl->viewStartTime() > v0, "the wheel button held and the mouse moved right scrolls the view on", num(v0) + " -> " + num(tl->viewStartTime()));
        // the wheel over a volume slider moves it
        const QRect lv = tl->headerPartRect(ids.a2, TimelineWidget::PartLevel);
        const double was = track(ids.a2)->level;
        // over its knob; elsewhere the wheel scrolls the headers
        for (int x = lv.left(); x <= lv.right() && !(track(ids.a2)->level > was); x += 3) uitest::wheel(tl, QPoint(x, lv.center().y()), 120);
        check(!lv.isEmpty() && track(ids.a2)->level > was, "the wheel over a track's volume knob turns it up", num(track(ids.a2)->level));
    }

    // ---------------------------------------------------------------- multicamera
    {
        st().newProject();
        std::string err;
        ids.clip = st().importMedia("synth:1", &err);
        ids.clip2 = st().importMedia("synth:2", &err);
        st().edit("Cameras", [&](Project& p) {
            for (int k = 0; k < 2; ++k) {
                Track t;
                t.id = p.newId();
                t.type = TrackType::Video;
                t.name = k ? "Camera 2" : "Camera 1";
                p.tracks.push_back(t);
                addEvent(p, t.id, k ? ids.clip2 : ids.clip, 0.0, 10.0);
            }
        });
        st().setSelection(std::set<uint64_t>());
        if (QAction* a = action("createMulticam")) a->trigger();
        const auto& tracks = st().project().tracks;
        check(tracks.size() == 1 && tracks[0].events.size() == 1 && tracks[0].events[0].takes.size() == 1 && tracks[0].events[0].mediaId == ids.clip,
              "Create Multicamera Track: one track, each camera a take", strfmt("%d tracks", (int)tracks.size()));
        press("Ctrl+Shift+D");
        check(tl->options().multicam, "Ctrl+Shift+D enables multicamera editing");
        press("Alt+0");
        st().setCursor(2.0);
        press("2");
        check(st().project().tracks[0].events[0].mediaId == ids.clip2, "2 (stopped) shows camera 2 at the cursor");
        st().setCursor(4.0);
        press("Ctrl+1");
        const Track& mt = st().project().tracks[0];
        check(mt.events.size() == 2 && mt.events[1].mediaId == ids.clip && near(mt.events[1].start, 3.0) && near(mt.events[1].offset, 3.0) &&
                  near(mt.events[0].end(), 4.0),
              "Ctrl+1 cuts to camera 1 there with a one-second cross-fade",
              mt.events.size() == 2 ? num(mt.events[1].start) + " " + num(mt.events[1].offset) : strfmt("%d events", (int)mt.events.size()));
        press("Ctrl+Shift+D");
        check(!tl->options().multicam, "and Ctrl+Shift+D turns it off");
    }

    // ---------------------------------------------------------------- Open in Audio Editor, peaks, help
    {
        buildProject();
        const QString wav = QDir(out).absoluteFilePath("edit_tone.wav");
        const QString script = QDir(out).absoluteFilePath("fake_editor.sh");
        const QString got = QDir(out).absoluteFilePath("fake_editor.out");
        QFile::remove(got);
        writeWav(wav);
        {
            QFile f(script);
            f.open(QIODevice::WriteOnly);
            f.write(QString("#!/bin/sh\necho \"$1\" > '%1'\n").arg(got).toUtf8());
            f.close();
            f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        }
        std::string err;
        const uint64_t m = st().importMedia(wav.toStdString(), &err);
        uint64_t e = 0;
        st().edit("Tone", [&](Project& p) { e = addEvent(p, ids.a2, m, 8.0, 1.0); });
        st().prefs.audioEditor = script.toStdString();
        st().selectOnly(e);
        press("Ctrl+E");
        for (int k = 0; k < 40 && !QFileInfo::exists(got); ++k) uitest::settle(50);
        QFile g(got);
        const QString said = g.open(QIODevice::ReadOnly) ? QString::fromUtf8(g.readAll()).trimmed() : QString();
        check(m && said == QDir::toNativeSeparators(wav), "Ctrl+E opens the event's media file in the audio editor", said.toStdString());

        press("Shift+F5");
        QLabel* status = w.findChild<QLabel*>("statusText");
        check(status && status->text().contains("Rebuilding"), "Shift+F5 rebuilds the audio peaks", status ? status->text().toStdString() : std::string());

        press("F1");
        QDialog* help = w.findChild<QDialog*>("helpWindow");
        QTextBrowser* keys = help ? help->findChild<QTextBrowser*>("helpShortcuts") : nullptr;
        check(help && help->isVisible() && keys && keys->toPlainText().contains("Smart Split"), "F1 opens the help, with the shortcut list");
        if (help) {
            uitest::shot(help, out + "/edit_help.png");
            help->hide();
        }
        press("Shift+F1");
        check(QWhatsThis::inWhatsThisMode(), "Shift+F1: What's This mode");
        QWhatsThis::leaveWhatsThisMode();
    }

    // ---------------------------------------------------------------- the shortcut list: every VEGAS key Reno does
    {
        int window = 0, timeline = 0, panel = 0, missing = 0;
        for (const ShortcutInfo& s : shortcutList()) {
            if (s.kind == ShortcutInfo::Window) ++window;
            else if (s.kind == ShortcutInfo::Timeline) ++timeline;
            else if (s.kind == ShortcutInfo::Panel) ++panel;
            else ++missing;
        }
        std::printf("shortcut list: %d window, %d timeline, %d in other windows, %d not available\n", window, timeline, panel, missing);
        check(missing <= 25, "the shortcut list has few keys left that Reno cannot do", strfmt("%d", missing));
    }
    return uitest::summary();
}
