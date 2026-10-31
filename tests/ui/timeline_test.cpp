// Drives the timeline with pretend mouse, keys, wheel and drops, and checks what it did to the project. Also saves pictures of it in
// several states.
//
//   timeline_test <output folder> [testfx.ofx ...]
//
// QT_QPA_PLATFORM=offscreen. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/timeline.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QContextMenuEvent>
#include <QDir>
#include <QDragEnterEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

// A point in a track's header for a right-click that gets the track's menu: not on its pan slider (whose menu is VEGAS's pan types).
QPoint headerMenuSpot(TimelineWidget& w, uint64_t track)
{
    const QRect h = w.headerRect(track);
    QPoint p(100, h.top() + 60);
    const QRect pan = w.headerPartRect(track, TimelineWidget::PartPan);
    if (!pan.isEmpty() && pan.adjusted(-8, -3, 8, 3).contains(p)) p.setY(pan.top() - 6);
    return p;
}


AppState& st() { return AppState::get(); }

struct Ids {
    uint64_t v1 = 0, v2 = 0, a1 = 0, a2 = 0;           // tracks, top to bottom
    uint64_t clip = 0, pic = 0, snd = 0;                // media
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

// Two video and two audio tracks:
//   V1: e1 clip 1..5 (grouped with e1a on A1), e2 still 7..10
//   V2: e3 clip 2..7 (offset 1), e5 clip 10..14, e6 clip 12..16 (overlapping e5: a cross-fade)
//   A1: e1a clip sound 1..5
//   A2: e4 sound only 0.5..6.5
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

// A point on an event's track at time t; 'y' from the top of the track's row.
QPoint at(TimelineWidget& w, uint64_t track, double t, int y)
{
    return QPoint(w.xAt(t), w.trackRect(track).top() + y);
}

void dragEnter(QWidget* w, QMimeData* md, QPoint pos, bool* accepted = nullptr)
{
    QDragEnterEvent e(pos, Qt::CopyAction | Qt::MoveAction, md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
    if (accepted) *accepted = e.isAccepted();
}

// Qt only delivers moves and drops to a widget that took the drag's DragEnter, as a real drag always starts
bool dragMove(QWidget* w, QMimeData* md, QPoint pos)
{
    dragEnter(w, md, pos);
    QDragMoveEvent e(pos, Qt::CopyAction | Qt::MoveAction, md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
    return e.isAccepted();
}

void dropAt(QWidget* w, QMimeData* md, QPoint pos)
{
    dragMove(w, md, pos);
    QDropEvent e(QPointF(pos), Qt::CopyAction | Qt::MoveAction, md, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
    QCoreApplication::processEvents();
}

// Right-clicks and picks the menu item called 'text' (or 'sub' > 'text').
bool menuPick(QWidget* w, QPoint pos, const QString& text, const QString& sub = QString())
{
    bool found = false;
    QTimer::singleShot(120, [&] {
        QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!m)
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QMenu* mm = qobject_cast<QMenu*>(t))
                    if (mm->isVisible()) m = mm;
        if (!m) return;
        QMenu* from = m;
        if (!sub.isEmpty())
            for (QAction* a : m->actions())
                if (a->menu() && a->text() == sub) from = a->menu();
        for (QAction* a : from->actions())
            if (a->text().split('\t')[0] == text && a->isEnabled()) {
                a->trigger();
                found = true;
                break;
            }
        m->close();
    });
    QContextMenuEvent e(QContextMenuEvent::Mouse, pos, w->mapToGlobal(pos));
    QApplication::sendEvent(w, &e);
    uitest::settle(50);
    return found;
}

// How many pixels in a rectangle differ clearly from 'base'.
int differing(const QImage& img, const QRect& r, QColor base)
{
    int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (std::abs(c.red() - base.red()) + std::abs(c.green() - base.green()) + std::abs(c.blue() - base.blue()) > 60) ++n;
        }
    return n;
}

void writeWav(const QString& path)
{
    const int rate = 8000, n = rate;
    QByteArray b;
    auto u32 = [&](uint32_t v) { for (int k = 0; k < 4; ++k) b.append((char)((v >> (8 * k)) & 0xff)); };
    auto u16 = [&](uint16_t v) { b.append((char)(v & 0xff)); b.append((char)(v >> 8)); };
    b.append("RIFF");
    u32(36 + n * 2);
    b.append("WAVEfmt ");
    u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
    b.append("data");
    u32(n * 2);
    for (int k = 0; k < n; ++k) u16((uint16_t)(int16_t)(8000 * std::sin(k * 0.2)));
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) f.write(b);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("timeline_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("shots");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());

    buildProject();
    Playback playback;
    TimelineWidget w(&playback);
    int fxCalls = 0;
    FxTarget fxTarget;
    uint64_t fxSlot = 0;
    w.onOpenFx = [&](const FxTarget& t, uint64_t slot) { ++fxCalls; fxTarget = t; fxSlot = slot; };
    w.resize(1400, 520);
    w.show();
    w.zoomToFit();
    uitest::settle(300);
    const Project& p = st().project();
    const double fr = 1.0 / p.settings.fps();
    // where the event area begins: right of the track headers (and of the time display over them)
    const int L = w.timeDisplayRect().width();

    // ---------------------------------------------------------------- the view
    {
        check(w.xAt(0.0) == L && w.headerRect(ids.v1).right() + 1 == L, "zero is at the left of the event area", strfmt("%d", w.xAt(0.0)));
        check(w.xAt(16.0) < 1400 && w.xAt(16.0) > 1250, "zoom to fit shows the whole project", strfmt("%d", w.xAt(16.0)));
        check(near(w.timeAt(w.xAt(5.0)), 5.0, 0.02), "timeAt and xAt agree");
        // VEGAS's default heights
        check(w.trackRect(ids.v1).height() == 64 && w.trackRect(ids.a1).height() == 68, "tracks start at VEGAS's default heights (video 64, audio 68)");
        check(w.trackRect(ids.v1).top() == w.rulerRect().bottom() + 1 && w.trackRect(ids.v2).top() == w.trackRect(ids.v1).bottom() + 1, "tracks are stacked under the ruler");
        check(w.markerBarRect().bottom() < w.loopBarRect().top() && w.loopBarRect().bottom() < w.rulerRect().top(), "the marker bar, the time selection bar and the ruler, top to bottom");
        uitest::settle(400);
        const QImage img = uitest::shot(&w, out + "/tl_fit.png");
        // pictures and waveforms come from the loader thread
        const QRect v = w.trackRect(ids.v2);
        const QRect thumbArea(w.xAt(3.0), v.top() + 25, 60, 50);
        check(differing(img, thumbArea, QColor(18, 18, 20)) > 1500, "thumbnails were loaded and drawn", strfmt("%d", differing(img, thumbArea, QColor(18, 18, 20))));
        const QRect a = w.trackRect(ids.a2);
        const QRect waveArea(w.xAt(1.0), a.top() + 20, 120, 40);
        const QColor body = img.pixelColor(w.xAt(1.0) + 3, a.top() + 22);
        check(differing(img, waveArea, QColor(40, 60, 40)) > 600, "the waveform was drawn", strfmt("%d", differing(img, waveArea, QColor(40, 60, 40))));
        (void)body;
    }

    // ---------------------------------------------------------------- clicking
    {
        uitest::mouseClick(&w, at(w, ids.v2, 4.0, 60));
        check(st().selection() == std::set<uint64_t>{ids.e3}, "a click selects the event");
        check(st().currentTrack == ids.v2, "a click makes the event's track the current track");
        check(near(st().cursor(), p.quantize(w.timeAt(w.xAt(4.0)))), "a click moves the cursor there", num(st().cursor()));
        uitest::mouseClick(&w, at(w, ids.v1, 8.5, 60), Qt::LeftButton, Qt::ControlModifier);
        check(st().selection() == std::set<uint64_t>{ids.e3, ids.e2}, "Ctrl-click adds to the selection");
        uitest::mouseClick(&w, at(w, ids.v1, 8.5, 60), Qt::LeftButton, Qt::ControlModifier);
        check(st().selection() == std::set<uint64_t>{ids.e3}, "Ctrl-click again takes it out");
        // as in VEGAS, Shift+click selects the range from the event clicked last (e2, Ctrl+clicked above) to this one: the events between
        // the two in time, on the tracks between them
        uitest::mouseClick(&w, at(w, ids.a2, 3.0, 40), Qt::LeftButton, Qt::ShiftModifier);
        std::set<uint64_t> range{ids.e3};
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (e.start >= 0.5 - 1e-9 && e.end() <= 10.0 + 1e-9) range.insert(e.id);
        check(st().selection() == range, "Shift-click selects the range of events", strfmt("%d selected", (int)st().selection().size()));
        uitest::mouseClick(&w, at(w, ids.v1, 6.0, 40));
        check(st().selection().empty(), "a click on empty space clears the selection");
        check(st().currentTrack == ids.v1 && near(st().cursor(), p.quantize(w.timeAt(w.xAt(6.0)))), "and moves the cursor and the current track there");
        // snapping: a click just beside an event's edge lands on it
        uitest::mouseClick(&w, QPoint(w.xAt(7.0) - 7, w.trackRect(ids.v1).top() + 40));
        check(near(st().cursor(), 7.0), "a click near an edge snaps the cursor to it", num(st().cursor()));
        st().snapping = false;
        uitest::mouseClick(&w, QPoint(w.xAt(7.0) - 7, w.trackRect(ids.v1).top() + 40));
        check(!near(st().cursor(), 7.0) && near(st().cursor(), p.quantize(st().cursor())), "without snapping it stays on the frame clicked", num(st().cursor()));
        st().snapping = true;
    }

    // ---------------------------------------------------------------- moving events
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        st().setCursor(0.0);
        st().setSelection(std::set<uint64_t>());
        uitest::mouseDrag(&w, at(w, ids.v1, 8.5, 60), at(w, ids.v1, 10.0, 60));
        check(near(ev(ids.e2)->start, 8.5), "dragging an event moves it", num(ev(ids.e2)->start));
        check(st().undoName() == "Move Events", "one undo step named Move Events", st().undoName());
        st().undo();
        check(near(ev(ids.e2)->start, 7.0), "undo puts it back");

        // grouped: the sound goes with the picture
        uitest::mouseDrag(&w, at(w, ids.v1, 3.0, 60), at(w, ids.v1, 4.0, 60));
        check(near(ev(ids.e1)->start, 2.0) && near(ev(ids.e1a)->start, 2.0), "a grouped event's partner moves with it",
              num(ev(ids.e1)->start) + " " + num(ev(ids.e1a)->start));
        st().undo();

        // several selected
        st().setSelection(std::set<uint64_t>{ids.e2, ids.e3});
        uitest::mouseDrag(&w, at(w, ids.v2, 4.0, 60), at(w, ids.v2, 4.5, 60));
        check(near(ev(ids.e3)->start, 2.5) && near(ev(ids.e2)->start, 7.5), "all the selected events move together",
              num(ev(ids.e3)->start) + " " + num(ev(ids.e2)->start));
        st().undo();

        // nothing before zero
        st().selectOnly(ids.e1);
        uitest::mouseDrag(&w, at(w, ids.v1, 3.0, 60), at(w, ids.v1, 0.2, 60), 12, Qt::ShiftModifier);
        check(near(ev(ids.e1)->start, 0.0) && near(ev(ids.e1a)->start, 0.0), "a move cannot put an event before zero", num(ev(ids.e1)->start));
        st().undo();

        // to another track of the same kind
        uint64_t tr = 0;
        st().selectOnly(ids.e2);
        const int rowH = w.trackRect(ids.v1).height();
        uitest::mouseDrag(&w, at(w, ids.v1, 8.5, 60), at(w, ids.v1, 8.5, 60 + rowH));
        ev(ids.e2, &tr);
        check(tr == ids.v2 && near(ev(ids.e2)->start, 7.0), "dragging down a track moves it to that track");
        st().undo();
        // a little way down is not far enough (and a drag that changes nothing leaves no undo step: the redo steps from the undo above remain)
        uitest::mouseDrag(&w, at(w, ids.v1, 8.5, 20), at(w, ids.v1, 8.5, 45));
        ev(ids.e2, &tr);
        check(tr == ids.v1 && near(ev(ids.e2)->start, 7.0), "a small vertical wobble keeps the track");
        check(st().canRedo(), "a drag that changes nothing leaves no undo step");
        // a video event cannot go onto an audio track
        uitest::mouseDrag(&w, at(w, ids.v1, 8.5, 60), QPoint(w.xAt(8.5), w.trackRect(ids.a2).top() + 30));
        ev(ids.e2, &tr);
        check(tr == ids.v2, "dragged over audio tracks it stops at the nearest video track");
        st().undo();
        // grouped picture and sound: the picture changes track, the sound keeps its own
        st().selectOnly(ids.e1);
        uitest::mouseDrag(&w, at(w, ids.v1, 3.0, 60), at(w, ids.v1, 3.0, 60 + rowH));
        uint64_t tr2 = 0;
        ev(ids.e1, &tr);
        ev(ids.e1a, &tr2);
        check(tr == ids.v2 && tr2 == ids.a1, "a grouped move to another track keeps the sound on its own track");
        st().undo();

        // snapping to the cursor while moving, and Shift to stop it
        st().setCursor(12.0 + 2 * fr);
        st().selectOnly(ids.e2);
        const QPoint from = at(w, ids.v1, 8.5, 60);
        QPoint to = from + QPoint((int)std::lround((12.0 - 7.0) * (w.xAt(10.0) - w.xAt(0.0)) / 10.0) + 4, 0);
        // (press away from the cursor so the click does not move it: the click puts the cursor where it was pressed)
        uitest::mouseDrag(&w, from, to);
        // the press moved the cursor to 8.5; the event's start passes 12, its end passes 15 ... it snaps to the
        // edges of e5/e6 (12, 14) on the next track
        check(near(ev(ids.e2)->start, 12.0), "a moved event snaps to another event's edge", num(ev(ids.e2)->start));
        st().undo();
        uitest::mouseDrag(&w, from, to, 8, Qt::ShiftModifier);
        check(!near(ev(ids.e2)->start, 12.0) && near(ev(ids.e2)->start, p.quantize(ev(ids.e2)->start)), "with Shift it does not snap, but stays on frames",
              num(ev(ids.e2)->start));
        st().undo();

        // Escape in the middle of a drag
        const double before = ev(ids.e2)->start;
        uitest::mousePress(&w, at(w, ids.v1, 8.5, 60));
        uitest::mouseMove(&w, at(w, ids.v1, 9.5, 60));
        uitest::mouseMove(&w, at(w, ids.v1, 11.5, 60));
        const bool moved = !near(ev(ids.e2)->start, before);
        uitest::key(&w, Qt::Key_Escape);
        uitest::mouseMove(&w, at(w, ids.v1, 12.5, 60));
        uitest::mouseRelease(&w, at(w, ids.v1, 12.5, 60));
        check(moved && near(ev(ids.e2)->start, before), "Escape cancels a drag (and the drag follows the mouse live until then)");
    }

    // ---------------------------------------------------------------- trimming, fades, stretching
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        const QRect v2 = w.trackRect(ids.v2);
        st().setCursor(0.0);
        // left edge of e3 (2..7, offset 1): one second later
        uitest::mouseDrag(&w, QPoint(w.xAt(2.0) + 1, v2.top() + 40), QPoint(w.xAt(3.0) + 1, v2.top() + 40));
        const Event* e3 = ev(ids.e3);
        check(near(e3->start, 3.0) && near(e3->length, 4.0) && near(e3->offset, 2.0), "trimming the left edge moves start, length and offset together",
              num(e3->start) + " " + num(e3->length) + " " + num(e3->offset));
        check(st().undoName() == "Trim Event", "named Trim Event", st().undoName());
        st().undo();
        // right edge, past the start: stops at one frame
        uitest::mouseDrag(&w, QPoint(w.xAt(7.0) - 1, v2.top() + 40), QPoint(w.xAt(0.5), v2.top() + 40), 8, Qt::ShiftModifier);
        check(near(ev(ids.e3)->length, fr) && near(ev(ids.e3)->start, 2.0), "a trim never leaves less than one frame", num(ev(ids.e3)->length));
        st().undo();
        // grouped: the sound is trimmed with the picture
        const QRect v1 = w.trackRect(ids.v1);
        uitest::mouseDrag(&w, QPoint(w.xAt(5.0) - 1, v1.top() + 40), QPoint(w.xAt(4.0) - 1, v1.top() + 40));
        check(near(ev(ids.e1)->length, 3.0) && near(ev(ids.e1a)->length, 3.0), "trimming a grouped event trims its partner",
              num(ev(ids.e1)->length) + " " + num(ev(ids.e1a)->length));
        st().undo();
        // fades
        uitest::mouseDrag(&w, QPoint(w.xAt(7.0) + 3, v1.top() + 5), QPoint(w.xAt(8.0) + 3, v1.top() + 5));
        check(near(ev(ids.e2)->fadeIn, 1.0, fr * 0.51) && near(ev(ids.e2)->start, 7.0), "dragging the top-left handle makes a fade in", num(ev(ids.e2)->fadeIn));
        check(st().undoName() == "Fade In", "named Fade In", st().undoName());
        uitest::mouseDrag(&w, QPoint(w.xAt(10.0) - 3, v1.top() + 5), QPoint(w.xAt(9.0) - 3, v1.top() + 5));
        check(near(ev(ids.e2)->fadeOut, 1.0, fr * 0.51), "dragging the top-right handle makes a fade out", num(ev(ids.e2)->fadeOut));
        // the fade-in handle now sits where the fade ends
        uitest::mouseDrag(&w, QPoint(w.xAt(8.0), v1.top() + 5), QPoint(w.xAt(7.5), v1.top() + 5));
        check(near(ev(ids.e2)->fadeIn, 0.5, fr * 0.51), "a fade handle is found where the fade ends", num(ev(ids.e2)->fadeIn));
        uitest::mouseDrag(&w, QPoint(w.xAt(7.5), v1.top() + 5), QPoint(w.xAt(12.0), v1.top() + 5));
        check(near(ev(ids.e2)->fadeIn + ev(ids.e2)->fadeOut, 3.0, 1e-6), "fades never add up to more than the event", num(ev(ids.e2)->fadeIn));
        st().undo();
        st().undo();
        st().undo();
        st().undo();
        check(near(ev(ids.e2)->fadeIn, 0.0) && near(ev(ids.e2)->fadeOut, 0.0), "fades undo");
        // stretch: Ctrl + right edge doubles the length and halves the rate
        uitest::mouseDrag(&w, QPoint(w.xAt(7.0) - 1, v2.top() + 40), QPoint(w.xAt(12.0) - 1, v2.top() + 40), 8, Qt::ControlModifier);
        const Event* s = ev(ids.e3);
        check(near(s->length, 10.0) && near(s->rate, 0.5) && near(s->start, 2.0), "Ctrl-dragging the right edge stretches (rate follows the length)",
              num(s->length) + " " + num(s->rate));
        check(st().undoName() == "Stretch Event", "named Stretch Event", st().undoName());
        st().undo();
        // and the limits on the rate
        uitest::mouseDrag(&w, QPoint(w.xAt(7.0) - 1, v2.top() + 40), QPoint(w.xAt(2.05), v2.top() + 40), 8, Qt::ControlModifier | Qt::ShiftModifier);
        check(near(ev(ids.e3)->rate, 10.0, 1e-6) && near(ev(ids.e3)->length, 0.5), "a stretch keeps the rate at 10 or under", num(ev(ids.e3)->rate));
        st().undo();
        // left edge stretch
        uitest::mouseDrag(&w, QPoint(w.xAt(2.0) + 1, v2.top() + 40), QPoint(w.xAt(4.5) + 1, v2.top() + 40), 8, Qt::ControlModifier);
        check(near(ev(ids.e3)->start, 4.5) && near(ev(ids.e3)->rate, 2.0) && near(ev(ids.e3)->offset, 1.0), "Ctrl-dragging the left edge stretches too",
              num(ev(ids.e3)->start) + " " + num(ev(ids.e3)->rate));
        st().undo();
    }

    // ---------------------------------------------------------------- ruler, loop region, double-click
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        const int rulerY = w.rulerRect().center().y(), stripY = w.loopBarRect().center().y();
        uitest::mouseClick(&w, QPoint(w.xAt(5.5), rulerY));
        check(near(st().cursor(), p.quantize(w.timeAt(w.xAt(5.5)))), "a click on the ruler moves the cursor", num(st().cursor()));
        uitest::mouseDrag(&w, QPoint(w.xAt(5.5), rulerY), QPoint(w.xAt(9.0), rulerY));
        check(near(st().cursor(), p.quantize(w.timeAt(w.xAt(9.0)))) || near(st().cursor(), 9.0), "dragging in the ruler moves the cursor along", num(st().cursor()));
        uitest::mouseDrag(&w, QPoint(w.xAt(3.3), stripY), QPoint(w.xAt(8.6), stripY));
        check(st().loopEnd > st().loopStart && near(st().loopStart, p.quantize(w.timeAt(w.xAt(3.3))), fr) && near(st().loopEnd, p.quantize(w.timeAt(w.xAt(8.6))), fr),
              "dragging in the strip above the ruler sets the loop region", num(st().loopStart) + " " + num(st().loopEnd));
        // the ends can be dragged
        uitest::mouseDrag(&w, QPoint(w.xAt(st().loopEnd), stripY), QPoint(w.xAt(10.0) - 3, stripY));
        check(near(st().loopEnd, 10.0), "a loop region's end can be dragged (and snaps)", num(st().loopEnd));
        uitest::mouseDrag(&w, QPoint(w.xAt(1.3), rulerY), QPoint(w.xAt(2.6), rulerY), 8, Qt::ControlModifier);
        check(near(st().loopStart, p.quantize(w.timeAt(w.xAt(1.3))), fr) && near(st().loopEnd, p.quantize(w.timeAt(w.xAt(2.6))), fr),
              "Ctrl-dragging in the ruler sets the loop region");
        uitest::mouseDoubleClick(&w, at(w, ids.v2, 13.0, 60));
        check(near(st().loopStart, 12.0) && near(st().loopEnd, 16.0) && st().selection() == std::set<uint64_t>{ids.e6},
              "double-clicking an event selects it and sets the loop region to it", num(st().loopStart) + " " + num(st().loopEnd));
        const int steps = (int)st().project().tracks.size();
        uitest::mouseDoubleClick(&w, at(w, ids.v1, 12.0, 60));
        check((int)st().project().tracks.size() == steps, "double-clicking empty space is harmless");
        // dragging in empty space marks out a region (VEGAS's time selection)
        uitest::mouseDrag(&w, at(w, ids.a1, 6.3, 30), at(w, ids.a1, 9.7, 30), 8, Qt::ShiftModifier);
        check(near(st().loopStart, p.quantize(w.timeAt(w.xAt(6.3))), fr) && near(st().loopEnd, p.quantize(w.timeAt(w.xAt(9.7))), fr),
              "dragging in empty track space sets the loop region", num(st().loopStart) + " " + num(st().loopEnd));
    }

    // ---------------------------------------------------------------- wheel and keys
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        const QPoint m(w.xAt(8.0), w.trackRect(ids.v1).top() + 40);
        const double t0 = w.timeAt(m.x()), span0 = w.timeAt(1300) - w.timeAt(300);
        uitest::wheel(&w, m, 120);
        const double span1 = w.timeAt(1300) - w.timeAt(300);
        check(span1 < span0 * 0.9 && near(w.timeAt(m.x()), t0, 0.02), "the wheel zooms in about the mouse", num(span0) + " " + num(span1) + " " + num(w.timeAt(m.x())));
        uitest::wheel(&w, m, -120);
        check(near(w.timeAt(1300) - w.timeAt(300), span0, span0 * 0.01), "and back out");
        uitest::wheel(&w, m, -120 * 30);
        const double spanMax = w.timeAt(1300) - w.timeAt(300);
        check(spanMax < 16.0 * 4, "zooming out stops not far past the whole project", num(spanMax));
        w.zoomToFit();
        for (int k = 0; k < 40; ++k) uitest::wheel(&w, m, 120);
        const double spanMin = (w.timeAt(1390) - w.timeAt(L));
        check(spanMin >= 3.5 * fr && spanMin < 5 * fr, "zooming in stops at a few frames across", num(spanMin / fr));
        w.zoomToFit();
        w.zoomIn();
        w.zoomIn();
        const double left0 = w.timeAt(L);
        uitest::wheel(&w, m, -120, Qt::ShiftModifier);
        check(w.timeAt(L) > left0, "Shift+wheel scrolls sideways", num(left0) + " " + num(w.timeAt(L)));
        w.zoomToFit();

        st().setCursor(2.0);
        uitest::key(&w, Qt::Key_Right);
        check(near(st().cursor(), 2.0 + fr), "Right moves the cursor a frame", num(st().cursor()));
        uitest::key(&w, Qt::Key_Left);
        uitest::key(&w, Qt::Key_Left);
        check(near(st().cursor(), 2.0 - fr), "Left moves it back a frame");
        // (as in VEGAS: Shift+arrows make a time selection from the cursor)
        uitest::key(&w, Qt::Key_Right, Qt::ShiftModifier);
        uitest::key(&w, Qt::Key_Right, Qt::ShiftModifier);
        check(near(st().cursor(), 2.0 + fr, 1e-9) && near(st().loopStart, 2.0 - fr, 1e-9) && near(st().loopEnd, 2.0 + fr, 1e-9),
              "Shift+Right extends a time selection from the cursor", num(st().cursor()) + " " + num(st().loopStart) + " " + num(st().loopEnd));
        uitest::key(&w, Qt::Key_Home);
        check(near(st().cursor(), 2.0 - fr, 1e-9), "Home goes to the start of the time selection", num(st().cursor()));
        uitest::key(&w, Qt::Key_End);
        check(near(st().cursor(), 2.0 + fr, 1e-9), "End goes to its end", num(st().cursor()));
        st().setLoop(0, 0);
        uitest::key(&w, Qt::Key_End, Qt::ControlModifier);
        check(near(st().cursor(), 16.0), "Ctrl+End goes to the end of the project");
        uitest::key(&w, Qt::Key_Home, Qt::ControlModifier);
        check(near(st().cursor(), 0.0), "Ctrl+Home goes to the start");
        uitest::key(&w, Qt::Key_Right, Qt::ControlModifier | Qt::AltModifier);
        check(near(st().cursor(), 0.5), "Ctrl+Alt+Right goes to the next event edge", num(st().cursor()));
        const double s0 = w.timeAt(1300) - w.timeAt(300);
        uitest::key(&w, Qt::Key_Up);
        check(w.timeAt(1300) - w.timeAt(300) < s0 * 0.8, "Up zooms in");
        uitest::key(&w, Qt::Key_Down);
        check(near(w.timeAt(1300) - w.timeAt(300), s0, s0 * 0.01), "Down zooms out");
        st().setCursor(4.0);
        uitest::key(&w, Qt::Key_I, Qt::NoModifier, "i");
        st().setCursor(6.0);
        uitest::key(&w, Qt::Key_O, Qt::NoModifier, "o");
        check(near(st().loopStart, 4.0) && near(st().loopEnd, 6.0), "I and O set the loop region at the cursor");
        // the cursor is brought into view
        for (int k = 0; k < 6; ++k) w.zoomIn();
        st().setCursor(14.0);
        w.scrollToCursor();
        check(w.xAt(14.0) >= L && w.xAt(14.0) < 1400, "scrollToCursor brings the cursor into view", strfmt("%d", w.xAt(14.0)));
        // dragging past the right edge scrolls
        const double leftBefore = w.timeAt(L);
        st().selectOnly(ids.e6);
        uitest::mousePress(&w, QPoint(800, w.trackRect(ids.v2).top() + 60));
        uitest::mouseMove(&w, QPoint(820, w.trackRect(ids.v2).top() + 60));
        uitest::mouseMove(&w, QPoint(1395, w.trackRect(ids.v2).top() + 60));
        uitest::settle(300);
        uitest::mouseRelease(&w, QPoint(1395, w.trackRect(ids.v2).top() + 60));
        check(w.timeAt(L) > leftBefore + 0.02, "dragging an event past the right edge scrolls the view", num(leftBefore) + " " + num(w.timeAt(L)));
        st().undo();
        w.zoomToFit();
    }

    // ---------------------------------------------------------------- track headers
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        const QRect h3 = w.headerRect(ids.a1);
        auto part = [&](uint64_t track, TimelineWidget::HeaderPart pt) { return w.headerPartRect(track, pt); };
        uitest::mouseClick(&w, QPoint(100, h3.top() + h3.height() - 6));
        check(st().currentTrack == ids.a1, "a click on a header makes that the current track");
        uitest::mouseClick(&w, part(ids.v1, TimelineWidget::PartMute).center());
        check(st().project().tracks[0].mute, "the mute button mutes the track");
        uitest::mouseClick(&w, part(ids.v1, TimelineWidget::PartSolo).center());
        check(st().project().tracks[0].solo, "the solo button solos it");
        uitest::shot(&w, out + "/tl_mute_solo.png");
        st().undo();
        st().undo();
        // level: from the knob, half the slider's width to the left is half way down for a video track
        const QRect lv = part(ids.v1, TimelineWidget::PartLevel);
        const int ly = lv.center().y();
        uitest::mouseDrag(&w, QPoint(lv.right() + 1, ly), QPoint(lv.right() + 1 - lv.width() / 2, ly));
        check(near(st().project().tracks[0].level, 0.5, 0.02), "the level slider sets a video track's opacity", num(st().project().tracks[0].level));
        check(st().undoName() == "Track Opacity", "one undo step for the slider drag", st().undoName());
        uitest::mouseDoubleClick(&w, QPoint(lv.center().x(), ly));
        check(near(st().project().tracks[0].level, 1.0), "double-clicking the slider puts it back to 100%");
        // volume
        const QRect vol = part(ids.a1, TimelineWidget::PartLevel);
        const double knob = vol.left() + vol.width() * std::cbrt(0.25);
        uitest::mouseDrag(&w, QPoint((int)std::lround(knob), vol.center().y()), QPoint((int)std::lround(knob) + 15, vol.center().y()));
        check(st().project().tracks[2].level > 1.5, "the volume slider goes above 0 dB", num(st().project().tracks[2].level));
        uitest::shot(&w, out + "/tl_volume.png");
        uitest::mouseDoubleClick(&w, QPoint((int)std::lround(knob) + 15, vol.center().y()));
        check(near(st().project().tracks[2].level, 1.0), "and resets to 0 dB");
        // pan
        const QRect pan = part(ids.a1, TimelineWidget::PartPan);
        uitest::mouseDrag(&w, QPoint(pan.center().x(), pan.center().y()), QPoint(pan.center().x() + pan.width() / 4, pan.center().y()));
        check(near(st().project().tracks[2].pan, 0.5, 0.03), "the pan slider pans", num(st().project().tracks[2].pan));
        uitest::mouseDoubleClick(&w, QPoint(pan.center().x() + pan.width() / 4, pan.center().y()));
        check(near(st().project().tracks[2].pan, 0.0), "double-click centres it");
        // FX buttons
        fxCalls = 0;
        // the header's FX button shows once the headers are wide enough, as in VEGAS
        check(part(ids.v1, TimelineWidget::PartFx).isEmpty(), "at the default width the headers show no FX button");
        const int headerW = w.headerWidth();
        w.setHeaderWidth(260);
        check(!part(ids.v1, TimelineWidget::PartFx).isEmpty() && !part(ids.v1, TimelineWidget::PartMotion).isEmpty(), "wider headers show FX and Track Motion");
        uitest::mouseClick(&w, part(ids.v1, TimelineWidget::PartFx).center());
        w.setHeaderWidth(headerW);
        check(fxCalls == 1 && fxTarget.kind == FxTarget::Track && fxTarget.id == ids.v1, "the header's FX button opens the track's effects");
        uitest::mouseClick(&w, w.eventButtonRect(ids.e3, false).center());
        check(fxCalls == 2 && fxTarget.kind == FxTarget::Event && fxTarget.id == ids.e3, "the event's FX button opens the event's effects",
              strfmt("%d %d", fxCalls, (int)fxTarget.kind));
        // renaming
        const QPoint nameAt = part(ids.v1, TimelineWidget::PartName).center();
        uitest::mouseDoubleClick(&w, nameAt);
        QLineEdit* edit = nullptr;
        for (QLineEdit* e : w.findChildren<QLineEdit*>())
            if (e->isVisible()) edit = e;
        check(edit != nullptr, "double-clicking a track's name opens a box to type in");
        if (edit) {
            edit->setText("Lower Thirds");
            uitest::shot(&w, out + "/tl_rename.png");
            uitest::key(edit, Qt::Key_Return);
            uitest::settle(20);
            check(st().project().tracks[0].name == "Lower Thirds" && !edit->isVisible(), "Enter renames the track", st().project().tracks[0].name);
            check(st().undoName() == "Rename Track", "named Rename Track");
            uitest::mouseDoubleClick(&w, nameAt);
            edit->setText("Nope");
            uitest::key(edit, Qt::Key_Escape);
            check(st().project().tracks[0].name == "Lower Thirds" && !edit->isVisible(), "Escape keeps the old name");
        }
    }

    // ---------------------------------------------------------------- the right-click menu
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        st().setCursor(13.0);
        check(menuPick(&w, at(w, ids.v2, 15.0, 60), "Mute", "Switches") && ev(ids.e6)->mute, "the menu mutes an event");
        check(st().selection() == std::set<uint64_t>{ids.e6}, "right-clicking an event selects it");
        check(menuPick(&w, at(w, ids.v2, 15.0, 60), "Loop", "Switches") && !ev(ids.e6)->loop, "and switches looping off");
        check(menuPick(&w, at(w, ids.v2, 15.0, 60), "50%", "Playback Rate") && near(ev(ids.e6)->rate, 0.5), "and sets the playback rate");
        check(menuPick(&w, at(w, ids.v2, 15.0, 60), "Reset Rate") && near(ev(ids.e6)->rate, 1.0), "Reset Rate");
        st().undo();
        st().undo();
        st().undo();
        st().undo();
        const size_t n = st().project().tracks[1].events.size();
        check(menuPick(&w, at(w, ids.v2, 15.0, 60), "Split") && st().project().tracks[1].events.size() == n + 1, "Split splits at the cursor");
        st().undo();
        fxCalls = 0;
        check(menuPick(&w, at(w, ids.v2, 15.0, 60), "Video Event FX...") && fxCalls == 1 && fxTarget.id == ids.e6, "Video Event FX... opens the effects");
        const size_t tracks = st().project().tracks.size();
        check(menuPick(&w, at(w, ids.a1, 12.0, 30), "Insert Audio Track") && st().project().tracks.size() == tracks + 1 &&
              st().project().tracks[2].type == TrackType::Audio && st().currentTrack == st().project().tracks[2].id,
              "Insert Audio Track puts one above the track clicked on");
        check(menuPick(&w, headerMenuSpot(w, st().project().tracks[2].id), "Delete Track") && st().project().tracks.size() == tracks,
              "Delete Track from a header");
        st().undo();
        st().undo();
        check(menuPick(&w, headerMenuSpot(w, ids.a2), "Rename"), "Rename from the menu");
        uitest::settle(30);
        bool open = false;
        for (QLineEdit* e : w.findChildren<QLineEdit*>()) open = open || e->isVisible();
        check(open, "opens the name box");
        w.setFocus();
        uitest::settle(10);
    }

    // ---------------------------------------------------------------- drops
    {
        buildProject();
        w.zoomToFit();
        uitest::settle(20);
        // media from the media panel, onto an empty stretch of V1
        QMimeData md;
        md.setData(kMimeMedia, QByteArray::number((qulonglong)ids.clip));
        bool accepted = false;
        const QPoint target = at(w, ids.v1, 11.0, 40);
        dragEnter(&w, &md, target, &accepted);
        check(accepted && dragMove(&w, &md, target), "media is taken over the track area");
        uitest::shot(&w, out + "/tl_drop.png");
        check(!dragMove(&w, &md, QPoint(100, w.headerRect(ids.v1).top() + 40)), "but not over the headers");
        dropAt(&w, &md, target);
        const std::set<uint64_t> sel = st().selection();
        bool onV1 = false, onAudio = false;
        for (uint64_t id : sel) {
            uint64_t tr = 0;
            const Event* e = ev(id, &tr);
            if (e && tr == ids.v1 && near(e->start, p.quantize(w.timeAt(target.x())))) onV1 = true;
            if (e && (tr == ids.a1 || tr == ids.a2)) onAudio = true;
        }
        check(sel.size() == 2 && onV1 && onAudio, "dropped media lands where it was dropped, picture and sound, and is selected");
        st().undo();

        // a video effect onto a video event
        QMimeData fx;
        fx.setData(kMimeVideoFx, QByteArray("reno.test.nosuch\nNo Such Effect"));
        fxCalls = 0;
        dragEnter(&w, &fx, at(w, ids.v2, 4.0, 60), &accepted);
        check(dragMove(&w, &fx, at(w, ids.v2, 4.0, 60)), "a video effect is taken over a video event");
        uitest::shot(&w, out + "/tl_drop_fx.png");
        check(!dragMove(&w, &fx, at(w, ids.a2, 4.0, 30)), "but not over an audio event");
        dropAt(&w, &fx, at(w, ids.v2, 4.0, 60));
        check(ev(ids.e3)->fx.size() == 1 && fxCalls == 1 && fxTarget.kind == FxTarget::Event && fxSlot == ev(ids.e3)->fx[0].id,
              "dropped on a video event it is added to it, and its window opened");
        // onto a video track's header
        dropAt(&w, &fx, QPoint(60, w.headerRect(ids.v1).top() + 40));
        check(st().project().tracks[0].fx.size() == 1 && fxTarget.kind == FxTarget::Track, "dropped on a video track's header it goes on the track");
        // an audio effect onto an audio track
        AudioPluginDesc desc;
        desc.format = "VST2";
        desc.path = "/nowhere/gain.so";
        desc.uid = "1234";
        desc.name = "Test Gain";
        QMimeData afx;
        afx.setData(kMimeAudioFx, QByteArray::fromStdString(desc.toJson().dump()));
        check(!dragMove(&w, &afx, at(w, ids.v1, 4.0, 60)), "an audio effect is not taken over a video track");
        dropAt(&w, &afx, at(w, ids.a2, 12.0, 30));
        check(st().project().tracks[3].fx.size() == 1 && st().project().tracks[3].fx[0].name == "Test Gain", "dropped on an audio track it goes on the track");
        // a generator
        QMimeData gen;
        gen.setData(kMimeGenerator, QByteArray("reno.test.gen\nColor Bars"));
        check(!dragMove(&w, &gen, at(w, ids.a1, 12.0, 30)), "a generator is not taken over an audio track");
        dropAt(&w, &gen, at(w, ids.v1, 11.0, 40));
        bool generated = false;
        for (const Event& e : st().project().tracks[0].events)
            if (const MediaItem* m = st().project().findMedia(e.mediaId))
                if (m->generated && m->name == "Color Bars" && near(e.start, p.quantize(w.timeAt(w.xAt(11.0))), fr)) generated = true;
        check(generated, "a generator dropped on a video track makes media and puts it there");
        // a transition onto the overlap
        QMimeData tr;
        tr.setData(kMimeTransition, QByteArray("reno.test.wipe\nWipe"));
        check(!dragMove(&w, &tr, at(w, ids.v2, 4.0, 60)), "a transition is not taken where nothing overlaps");
        check(dragMove(&w, &tr, at(w, ids.v2, 13.0, 60)), "a transition is taken over an overlap");
        uitest::shot(&w, out + "/tl_drop_transition.png");
        dropAt(&w, &tr, at(w, ids.v2, 13.0, 60));
        check(ev(ids.e6)->transition.pluginId == "reno.test.wipe" && ev(ids.e5)->transition.kind.empty(), "and set on the later event");
        check(menuPick(&w, at(w, ids.v2, 13.0, 60), "Remove Transition") && ev(ids.e6)->transition.kind.empty(), "Remove Transition from the menu");
        st().undo();
        // files from the desktop
        const QString wav = QDir(out).absoluteFilePath("drop.wav");
        writeWav(wav);
        QMimeData files;
        files.setUrls(QList<QUrl>() << QUrl::fromLocalFile(wav));
        dragEnter(&w, &files, at(w, ids.a1, 9.0, 30), &accepted);
        check(accepted && dragMove(&w, &files, at(w, ids.a1, 9.0, 30)), "files are taken");
        dropAt(&w, &files, at(w, ids.a1, 9.0, 30));
        bool fileEvent = false;
        for (const Event& e : st().project().tracks[2].events)
            if (const MediaItem* m = st().project().findMedia(e.mediaId))
                if (m->path == wav.toStdString()) fileEvent = true;
        check(fileEvent, "a sound file dropped on an audio track is imported and put there");
        uitest::settle(500);
        uitest::shot(&w, out + "/tl_after_drops.png");
    }

    // ---------------------------------------------------------------- looks: loops, zoom levels, many events
    {
        // e5 runs past its media's end: notches where the media starts again
        st().edit("Long", [&](Project& pr) {
            if (Event* e = pr.findEvent(ids.e5)) e->length = 2.0;
            if (Event* e = pr.findEvent(ids.e2)) { e->fadeIn = 0.8; e->fadeOut = 1.2; e->mute = true; }
            if (Event* e = pr.findEvent(ids.e4)) { e->length = 14.0; e->offset = 6.0; e->gain = 0.6; }
            if (Event* e = pr.findEvent(ids.e6)) { e->transition.id = pr.newId(); e->transition.kind = "ofx"; e->transition.pluginId = "x"; e->transition.name = "Cross Dissolve"; e->start = 11.0; }
        });
        st().setSelection(std::set<uint64_t>{ids.e3, ids.e4});
        st().setLoop(10.0, 13.0);
        st().loopOn = true;
        st().setCursor(4.25);
        w.zoomToFit();
        uitest::settle(600);
        uitest::shot(&w, out + "/tl_look.png");
        // zoomed in on the cross-fade
        st().setCursor(11.5);
        for (int k = 0; k < 3; ++k) w.zoomIn();
        w.scrollToCursor();
        uitest::settle(600);
        uitest::shot(&w, out + "/tl_zoomin.png");
        for (int k = 0; k < 8; ++k) w.zoomIn();
        uitest::settle(600);
        uitest::shot(&w, out + "/tl_zoomframes.png");
        for (int k = 0; k < 30; ++k) w.zoomOut();
        uitest::settle(300);
        uitest::shot(&w, out + "/tl_zoomout.png");

        // three hundred events
        st().edit("Many", [&](Project& pr) {
            for (int k = 0; k < 300; ++k) {
                const uint64_t track = (k % 2) ? ids.v2 : ids.a2;
                const uint64_t media = (k % 2) ? ids.clip : ids.snd;
                addEvent(pr, track, media, 20.0 + k * 0.7, 0.6, (k % 7) * 0.5);
            }
        });
        w.zoomToFit();
        uitest::settle(200);
        QElapsedTimer t;
        t.start();
        for (int k = 0; k < 5; ++k) w.grab();
        const double ms = t.elapsed() / 5.0;
        check(ms < 120.0, "painting with 300+ events is quick", strfmt("%.1f ms a paint", ms));
        std::printf("(a whole paint with 300+ events takes %.1f ms)\n", ms);
        uitest::settle(800);
        uitest::shot(&w, out + "/tl_many.png");
        for (int k = 0; k < 5; ++k) w.zoomIn();
        st().setCursor(120.0);
        w.scrollToCursor();
        uitest::settle(800);
        uitest::shot(&w, out + "/tl_many_zoom.png");
    }

    // ---------------------------------------------------------------- more tracks than fit; the snap line
    {
        buildProject();
        for (int k = 0; k < 6; ++k) st().addTrack(k % 2 ? TrackType::Audio : TrackType::Video);
        w.zoomToFit();
        uitest::settle(200);
        QScrollBar* vbar = nullptr;
        for (QScrollBar* b : w.findChildren<QScrollBar*>())
            if (b->orientation() == Qt::Vertical) vbar = b;
        check(vbar && vbar->isVisible(), "a vertical scroll bar appears when the tracks do not fit");
        const int top0 = w.trackRect(st().project().tracks[0].id).top();
        uitest::wheel(&w, QPoint(700, 200), -240, Qt::ControlModifier);
        const int top1 = w.trackRect(st().project().tracks[0].id).top();
        check(top1 < top0 && vbar && vbar->value() > 0, "Ctrl+wheel scrolls the tracks up and down", strfmt("%d %d", top0, top1));
        uitest::wheel(&w, QPoint(80, 200), 240);
        check(w.trackRect(st().project().tracks[0].id).top() == top0, "the wheel over the headers scrolls them too");
        uitest::wheel(&w, QPoint(80, 200), -120 * 50);
        const QRect last = w.trackRect(st().project().tracks.back().id);
        check(last.bottom() < w.height() - 14 && last.bottom() > w.height() - 120, "scrolled to the bottom, the last track and the space under it show",
              strfmt("%d", last.bottom()));
        uitest::shot(&w, out + "/tl_vscroll.png");
        uitest::wheel(&w, QPoint(80, 200), 120 * 50);

        // the line that shows what an edge snapped to, in the middle of a drag
        st().selectOnly(ids.e2);
        const QPoint from = at(w, ids.v1, 8.5, 60);
        uitest::mousePress(&w, from);
        uitest::mouseMove(&w, from + QPoint(10, 0));
        uitest::mouseMove(&w, from + QPoint(w.xAt(14.0) - w.xAt(10.0) + 3, 0));
        uitest::settle(200);
        check(near(ev(ids.e2)->start, 11.0), "while dragging, the end snaps to the edge of an event on another track", num(ev(ids.e2)->start));
        uitest::shot(&w, out + "/tl_snapline.png");
        uitest::mouseRelease(&w, from + QPoint(w.xAt(14.0) - w.xAt(10.0) + 3, 0));
        // grouping from the menu
        st().setSelection(std::set<uint64_t>{ids.e2, ids.e3});
        check(menuPick(&w, at(w, ids.v2, 4.0, 60), "Create New", "Group") && ev(ids.e2)->group != 0 && ev(ids.e2)->group == ev(ids.e3)->group, "Group > Create New from the menu");
        check(menuPick(&w, at(w, ids.v2, 4.0, 60), "Remove From", "Group") && ev(ids.e2)->group == 0, "Group > Remove From from the menu");
    }

    // ---------------------------------------------------------------- the cursor during playback
    {
        buildProject();
        w.zoomToFit();
        for (int k = 0; k < 4; ++k) w.zoomIn();
        st().setCursor(w.timeAt(1380));
        playback.play();
        uitest::settle(700);
        playback.pause();
        check(w.xAt(st().cursor()) >= L && w.xAt(st().cursor()) < 1400 && w.timeAt(L) > 0.1, "the view follows the cursor while playing",
              num(st().cursor()) + " " + num(w.timeAt(L)));
    }

    // ---------------------------------------------------------------- an empty project
    {
        st().newProject();
        w.zoomToFit();
        uitest::settle(50);
        uitest::shot(&w, out + "/tl_empty.png");
        QMimeData md;
        std::string err;
        const uint64_t m = st().importMedia("synth:4", &err);
        md.setData(kMimeMedia, QByteArray::number((qulonglong)m));
        dropAt(&w, &md, QPoint(500, 120));
        check(st().project().tracks.size() == 2 && st().project().tracks[0].events.size() == 1 && st().project().tracks[1].events.size() == 1,
              "media dropped on an empty project makes a video and an audio track for it");
        check(menuPick(&w, QPoint(700, 400), "Insert Video Track") && st().project().tracks.size() == 3, "the menu below the tracks inserts tracks");
    }

    // ---------------------------------------------------------------- going away while busy
    {
        TimelineWidget* t = new TimelineWidget(&playback);
        t->resize(900, 300);
        t->show();
        t->zoomToFit();
        for (int k = 0; k < 6; ++k) t->zoomIn();
        t->grab();                  // asks for thumbnails and waveforms
        delete t;
        uitest::settle(300);
        check(true, "a timeline can be destroyed while its loader is busy");
    }
    return uitest::summary();
}
