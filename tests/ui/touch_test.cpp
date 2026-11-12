// The touch interface: switches the main window to it, drives the touch timeline with pretend fingers (taps, trims,
// scrubbing, flicks, pinches, press-and-hold moves) and the tools and their sheets, and checks what they did to the
// project. Saves pictures of it on a phone held upright and sideways and on a tablet both ways.
//
//   touch_test <output folder> [testfx.ofx ...]
//
// QT_QPA_PLATFORM=offscreen. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/touch_shell.h"
#include "../../src/app/touch_timeline.h"
#include "../../src/app/touch_widgets.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QElapsedTimer>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QPointingDevice>
#include <QSettings>
#include <QTouchEvent>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <cmath>
#include <cstdio>
#include <map>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
AppState& st() { return AppState::get(); }

// Fingers on a widget, put down through the window system as a touch screen's are (so Qt finds the widget under them
// and gives each point its position there). Every change reports all the fingers that are down.
class Fingers {
public:
    explicit Fingers(QWidget* w) : w_(w)
    {
        static QPointingDevice* dev = nullptr;
        if (!dev) {
            dev = new QPointingDevice("touch test", 4242, QInputDevice::DeviceType::TouchScreen, QPointingDevice::PointerType::Finger,
                                      QInputDevice::Capability::Position | QInputDevice::Capability::Area, 10, 0);
            QWindowSystemInterface::registerInputDevice(dev);
        }
        dev_ = dev;
    }
    void down(int id, QPointF p) { send(id, p, QEventPoint::Pressed); }
    void move(int id, QPointF p) { send(id, p, QEventPoint::Updated); }
    void up(int id) { send(id, pos_[id], QEventPoint::Released); }
    void drag(QPointF from, QPointF to, int steps = 10, int msPerStep = 0)
    {
        down(0, from);
        for (int k = 1; k <= steps; ++k) {
            move(0, from + (to - from) * (double)k / steps);
            if (msPerStep) uitest::settle(msPerStep);
        }
        up(0);
    }
    void tap(QPointF p)
    {
        down(0, p);
        up(0);
    }

private:
    void send(int id, QPointF p, QEventPoint::State state)
    {
        pos_[id] = p;
        QList<QWindowSystemInterface::TouchPoint> pts;
        QWindow* win = w_->window()->windowHandle();
        for (const auto& f : pos_) {
            QWindowSystemInterface::TouchPoint tp;
            tp.id = f.first;
            tp.state = f.first == id ? state : QEventPoint::Stationary;
            const QPointF g = w_->mapToGlobal(f.second);
            tp.area = QRectF(g - QPointF(2, 2), QSizeF(4, 4));
            tp.normalPosition = QPointF(0.5, 0.5);
            tp.pressure = state == QEventPoint::Released && f.first == id ? 0 : 1;
            pts.push_back(tp);
        }
        if (state == QEventPoint::Released) pos_.erase(id);
        QWindowSystemInterface::handleTouchEvent<QWindowSystemInterface::SynchronousDelivery>(win, dev_, pts);
    }
    QWidget* w_;
    const QPointingDevice* dev_;
    std::map<int, QPointF> pos_;
};

struct Ids {
    uint64_t v1 = 0, a1 = 0;
    uint64_t e1 = 0, e2 = 0, e3 = 0, a3 = 0;
} ids;

// A small project: three clips on a video track, the last one with its sound on an audio track (grouped).
void buildProject()
{
    AppState& app = st();
    app.newProject();
    std::string err;
    const uint64_t clip = app.importMedia("synth:1", &err);
    const uint64_t clip2 = app.importMedia("synth:2", &err);
    app.edit("Build", [&](Project& p) {
        Track v;
        v.id = p.newId();
        v.type = TrackType::Video;
        ids.v1 = v.id;
        Track a;
        a.id = p.newId();
        a.type = TrackType::Audio;
        ids.a1 = a.id;
        auto ev = [&](Track& t, uint64_t m, double start, double len, double off, uint64_t group = 0) {
            Event e;
            e.id = p.newId();
            e.mediaId = m;
            e.start = start;
            e.length = len;
            e.offset = off;
            e.group = group;
            t.events.mut().push_back(e);
            return e.id;
        };
        ids.e1 = ev(v, clip, 0.0, 3.0, 0.0);
        ids.e2 = ev(v, clip2, 3.0, 3.0, 1.0);
        const uint64_t g = p.newId();
        ids.e3 = ev(v, clip, 6.0, 4.0, 2.0, g);
        ids.a3 = ev(a, clip, 6.0, 4.0, 2.0, g);
        p.tracks.push_back(v);
        p.tracks.push_back(a);
    });
    app.setSelection({});
    app.setCursor(1.0);
}

const Event* ev(uint64_t id)
{
    // a stand-in when an event is gone (a failed step undid more than it should), so later checks fail instead of crashing
    static Event none;
    const Event* e = st().project().findEvent(id);
    return e ? e : &none;
}

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }

QAbstractButton* button(QWidget* root, const QString& name) { return root->findChild<QAbstractButton*>(name); }

// Widgets of the touch interface's own classes (which have no Q_OBJECT, so findChild can't look for them by class).
template <class T>
std::vector<T*> kids(QObject* root)
{
    std::vector<T*> out;
    for (QWidget* w : root->findChildren<QWidget*>())
        if (T* t = dynamic_cast<T*>(w)) out.push_back(t);
    return out;
}
template <class T>
T* kid(QObject* root, const QString& name)
{
    for (T* t : kids<T>(root))
        if (t->objectName() == name) return t;
    return nullptr;
}

void click(QWidget* w)
{
    if (!w) return;
    uitest::mouseClick(w, QPoint(w->width() / 2, w->height() / 2));
    uitest::settle(30);
}

// Waits for an open sheet's slide-in.
touch::Sheet* openSheet(TouchShell* shell)
{
    uitest::settle(260);
    return shell->sheet();
}

void closeSheet(TouchShell* shell)
{
    shell->closeSheet();
    uitest::settle(220);
}

void shotAt(MainWindow& w, QSize size, const QString& name)
{
    w.resize(size);
    uitest::settle(250);
    uitest::shot(&w, gOut + "/" + name + ".png");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTouchTest");
    QApplication::setApplicationName("touch_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("touch");
    QDir().mkpath(gOut);
    std::vector<std::string> fx;
    for (int k = 2; k < args.size(); ++k) fx.push_back(args[k].toStdString());
    // touch_test <out> --project file.veg: pictures and paint times of the touch timeline with a real project only
    QString projectFile;
    for (int k = 2; k + 1 < args.size(); ++k)
        if (args[k] == "--project") projectFile = args[k + 1];
    if (!projectFile.isEmpty()) fx.clear();
    uitest::setup(fx, std::vector<std::string>());
    AppState& app = st();
    if (!projectFile.isEmpty()) {
        MainWindow w;
        w.resize(412, 892);
        w.setTouchMode(true);
        w.show();
        check(w.openProject(projectFile), "the project opens");
        TouchTimeline* tl = w.touchShell()->timeline();
        tl->zoomToFit();
        app.setCursor(app.project().length() * 0.4);
        uitest::settle(4000);
        uitest::shot(&w, gOut + "/project_phone.png");
        for (double pps : {tl->pixelsPerSecond(), 30.0, 120.0, 400.0}) {
            tl->setPixelsPerSecond(pps);
            uitest::settle(1500);
            QImage img(tl->size() * 2, QImage::Format_ARGB32_Premultiplied);
            img.setDevicePixelRatio(2);
            QElapsedTimer t;
            t.start();
            const int n = 20;
            for (int k = 0; k < n; ++k) {
                app.setCursor(app.project().length() * 0.4 + k * 3.0 / pps);
                tl->render(&img);
            }
            std::printf("paint at %.1f px/s: %.2f ms (%d tracks)\n", pps, t.elapsed() / (double)n, (int)app.project().tracks.size());
            std::fflush(stdout);
            uitest::shot(&w, gOut + QString("/project_phone_%1.png").arg((int)pps));
        }
        w.resize(1180, 820);
        tl->zoomToFit();
        uitest::settle(3000);
        uitest::shot(&w, gOut + "/project_tablet.png");
        return uitest::summary();
    }
    buildProject();

    MainWindow w;
    w.resize(412, 892);
    w.setTouchMode(true);
    w.show();
    uitest::settle(400);
    TouchShell* shell = w.touchShell();
    check(shell && w.centralWidget() == shell, "the touch interface takes the window's place");
    check(w.menuBar()->isHidden(), "the menu bar is hidden");
    check(shell && w.preview() && shell->isAncestorOf(w.preview()), "the Video Preview is in the touch interface");
    if (!shell) return uitest::summary();
    TouchTimeline* tl = shell->timeline();
    tl->setPixelsPerSecond(60);
    uitest::settle(100);
    uitest::shot(&w, gOut + "/phone_start.png");
    Fingers f(tl);

    // ---- tapping selects, an empty place clears the selection
    {
        const QRectF r = tl->eventRect(ids.e1);
        check(!r.isEmpty(), "the first event is in view", "");
        f.tap(QPointF(std::max(r.left() + 20, (double)touch::kHeaderW + 10), r.center().y()));
        uitest::settle(50);
        check(app.selection().size() == 1 && app.selection().count(ids.e1), "a tap on an event selects it");
        check(shell->toolNames().contains("speed") && shell->toolNames().contains("delete"), "the tools for a selected event show");
        uitest::shot(&w, gOut + "/phone_selected.png");
        const QRectF track = tl->trackRect(ids.v1);
        f.tap(QPointF(tl->width() - 6, tl->height() - 8));
        uitest::settle(350);    // (not a double tap)
        check(app.selection().empty(), "a tap on an empty place clears the selection", "");
        (void)track;
    }

    // ---- scrubbing: a finger dragged across moves the playhead the other way
    {
        app.setCursor(2.0);
        const double pps = tl->pixelsPerSecond();
        const QRectF track = tl->trackRect(ids.a1);
        const QPointF from(tl->width() * 0.7, track.center().y());
        // (the finger stops before it lifts, so it doesn't flick on)
        f.down(0, from);
        for (int k = 1; k <= 12; ++k) {
            f.move(0, from - QPointF(10.0 * k, 0));
            uitest::settle(8);
        }
        uitest::settle(120);
        f.up(0);
        uitest::settle(30);
        check(near(app.cursor(), 2.0 + 120.0 / pps, 0.02), "dragging across scrubs", std::to_string(app.cursor()));
    }

    // ---- a flick keeps going after the finger lifts, and slows down
    {
        app.setCursor(1.0);
        const QRectF track = tl->trackRect(ids.a1);
        const QPointF from(tl->width() * 0.8, track.center().y() + 4);
        f.down(0, from);
        for (int k = 1; k <= 6; ++k) {
            f.move(0, from - QPointF(25.0 * k, 0));
            uitest::settle(10);
        }
        f.up(0);
        const double atLift = app.cursor();
        uitest::settle(300);
        const double later = app.cursor();
        uitest::settle(1500);
        check(later > atLift + 0.05, "a flick keeps scrolling after the finger lifts", std::to_string(atLift) + " -> " + std::to_string(later));
        const double end = app.cursor();
        uitest::settle(200);
        check(near(app.cursor(), end, 1e-9), "and comes to a stop");
    }

    // ---- pinching zooms about the playhead
    {
        app.setCursor(4.0);
        tl->setPixelsPerSecond(60);
        const double before = tl->pixelsPerSecond();
        const double y = tl->trackRect(ids.a1).center().y();
        const double cx = tl->playheadX();
        f.down(0, QPointF(cx - 40, y));
        f.down(1, QPointF(cx + 40, y));
        for (int k = 1; k <= 8; ++k) {
            f.move(0, QPointF(cx - 40 - 10 * k, y));
            f.move(1, QPointF(cx + 40 + 10 * k, y));
        }
        f.up(0);
        f.up(1);
        uitest::settle(30);
        check(near(tl->pixelsPerSecond(), before * 3.0, 0.5), "spreading two fingers zooms in", std::to_string(tl->pixelsPerSecond()));
        check(near(app.cursor(), 4.0), "pinching leaves the playhead where it was");
        tl->setPixelsPerSecond(60);
    }

    // ---- trimming a selected event by its handle, with its grouped sound
    {
        app.setCursor(8.0);
        app.selectOnly(ids.e3);
        uitest::settle(30);
        const QRectF h = tl->handleRect(ids.e3, false);
        check(!h.isEmpty(), "a selected event shows its handles");
        const double pps = tl->pixelsPerSecond();
        const double len = ev(ids.e3)->length;
        // ends 0.5 s sooner (away from anything to snap to)
        f.drag(h.center(), h.center() - QPointF(0.5 * pps, 0), 10);
        uitest::settle(30);
        check(near(ev(ids.e3)->length, len - 0.5, 0.02), "dragging the right handle trims the event", std::to_string(ev(ids.e3)->length));
        check(near(ev(ids.a3)->length, ev(ids.e3)->length), "its grouped sound is trimmed with it");
        app.undo();
        check(near(ev(ids.e3)->length, len), "the trim is one undo step");
        const QRectF hl = tl->handleRect(ids.e3, true);
        const double start = ev(ids.e3)->start, off = ev(ids.e3)->offset;
        f.drag(hl.center(), hl.center() + QPointF(0.5 * pps, 0), 10);
        uitest::settle(30);
        check(near(ev(ids.e3)->start, start + 0.5, 0.02) && near(ev(ids.e3)->offset, off + (ev(ids.e3)->start - start), 1e-6),
              "dragging the left handle trims the start and keeps the picture in place", std::to_string(ev(ids.e3)->start));
        app.undo();
    }

    // ---- snapping: an edge dragged near the playhead lands on it
    {
        app.setCursor(9.3);
        app.selectOnly(ids.e3);
        uitest::settle(30);
        const QRectF h = tl->handleRect(ids.e3, false);   // the end is at 10.0, 0.7 s after the playhead
        const double pps = tl->pixelsPerSecond();
        f.drag(h.center(), h.center() - QPointF(0.7 * pps - 5, 0), 10);    // 5 px short of the playhead
        uitest::settle(30);
        check(near(ev(ids.e3)->start + ev(ids.e3)->length, 9.3, 1e-6), "a trimmed edge snaps to the playhead",
              std::to_string(ev(ids.e3)->start + ev(ids.e3)->length));
        app.undo();
    }

    // ---- press and hold picks up an unselected event to move it
    {
        app.setSelection({});
        app.setCursor(4.5);
        uitest::settle(30);
        const QRectF r = tl->eventRect(ids.e2);
        const double pps = tl->pixelsPerSecond();
        const double start = ev(ids.e2)->start;
        const QPointF at(r.center().x(), r.center().y());
        f.down(0, at);
        uitest::settle(520);
        check(app.selection().count(ids.e2) == 1, "holding a finger on an event selects it");
        for (int k = 1; k <= 10; ++k) f.move(0, at + QPointF(0.03 * pps * k, 0));
        f.up(0);
        uitest::settle(30);
        check(near(ev(ids.e2)->start, start + 0.3, 0.03), "and dragging moves it", std::to_string(ev(ids.e2)->start));
        app.undo();
        check(near(ev(ids.e2)->start, start), "the move is one undo step");
    }

    // ---- a plain drag across an unselected event scrolls rather than moving it
    {
        app.setSelection({});
        app.setCursor(4.5);
        const QRectF r = tl->eventRect(ids.e2);
        const double start = ev(ids.e2)->start;
        f.drag(r.center(), r.center() - QPointF(60, 0), 6);
        uitest::settle(30);
        check(near(ev(ids.e2)->start, start) && app.cursor() > 4.6, "a quick drag over an unselected event scrolls");
    }

    // ---- tools: split, speed, fades, opacity, delete, duplicate
    {
        app.setSelection({ids.e2});
        app.setCursor(4.5);
        uitest::settle(30);
        size_t count = 0;
        for (const Track& t : app.project().tracks) if (t.id == ids.v1) count = t.events.size();
        shell->runTool("split");
        size_t after = 0;
        for (const Track& t : app.project().tracks) if (t.id == ids.v1) after = t.events.size();
        check(after == count + 1, "Split splits the selected event at the playhead");
        app.undo();

        app.selectOnly(ids.e3);
        uitest::settle(30);
        shell->runTool("speed");
        touch::Sheet* s = openSheet(shell);
        check(s && s->isVisible(), "Speed opens its sheet");
        uitest::shot(&w, gOut + "/phone_speed.png");
        QAbstractButton* two = nullptr;
        if (s)
            for (QAbstractButton* b : s->findChildren<QAbstractButton*>())
                if (b->text() == QString("2") + QChar(0xD7)) two = b;
        click(two);
        check(near(ev(ids.e3)->rate, 2.0) && near(ev(ids.e3)->length, 2.0), "2x makes the event twice as fast and half as long",
              std::to_string(ev(ids.e3)->rate) + " " + std::to_string(ev(ids.e3)->length));
        check(near(ev(ids.a3)->rate, 2.0) && near(ev(ids.a3)->length, 2.0), "and its grouped sound too");
        app.undo();
        // the speed slider: one undo step for a whole drag
        if (touch::Sheet* s2 = shell->sheet())
            if (touch::Slider* sl = kid<touch::Slider>(s2, "touchSpeed")) {
                const size_t steps = app.undoCount();
                uitest::mouseDrag(sl, QPoint(sl->width() / 2, 49), QPoint(sl->width() - 30, 49), 8);
                uitest::settle(30);
                check(ev(ids.e3)->rate > 1.5 && app.undoCount() == steps + 1, "dragging the speed slider is one undo step",
                      std::to_string(ev(ids.e3)->rate) + " steps " + std::to_string(app.undoCount() - steps) + " slider " + std::to_string(sl->value()) +
                          " sel " + std::to_string(app.selection().size()) + " w " + std::to_string(sl->width()));
                app.undo();
            }
        closeSheet(shell);

        shell->runTool("fade");
        s = openSheet(shell);
        if (touch::Slider* in = s ? kid<touch::Slider>(s, "touchFadeIn") : nullptr) {
            uitest::mouseDrag(in, QPoint(24, 49), QPoint(24 + (in->width() - 48) / 4, 49), 6);
            uitest::settle(30);
        }
        check(ev(ids.e3)->fadeIn > 0.5 && near(ev(ids.a3)->fadeIn, ev(ids.e3)->fadeIn), "the Fade in slider sets the fade (on the grouped sound too)",
              std::to_string(ev(ids.e3)->fadeIn));
        uitest::shot(&w, gOut + "/phone_fades.png");
        closeSheet(shell);
        app.undo();

        shell->runTool("volume");
        s = openSheet(shell);
        check(s && kid<touch::Slider>(s, "touchOpacity") && kid<touch::Slider>(s, "touchVolume"),
              "the level sheet of a clip with sound has opacity and volume", s ? std::to_string(kids<touch::Slider>(s).size()) + " sel " + std::to_string(app.selection().size()) : "no sheet");
        closeSheet(shell);

        size_t total = 0;
        for (const Track& t : app.project().tracks) total += t.events.size();
        app.selectOnly(ids.e1);
        shell->runTool("duplicate");
        size_t total2 = 0;
        for (const Track& t : app.project().tracks) total2 += t.events.size();
        check(total2 == total + 1, "Duplicate adds a copy");
        app.undo();
        app.selectOnly(ids.e1);
        shell->runTool("delete");
        click(button(shell, "touchUndo"));
        check(st().project().findEvent(ids.e1) != nullptr, "the undo button undoes");
        uitest::settle(50);
        uitest::shot(&w, gOut + "/phone_undo_toast.png");
        app.redo();
        check(!st().project().findEvent(ids.e1), "Delete removes the selected event");
        app.undo();
        check(st().project().findEvent(ids.e1) != nullptr, "and Undo brings it back");
    }

    // ---- Select several: taps add and take away
    {
        app.selectOnly(ids.e1);
        app.setCursor(3.0);
        uitest::settle(30);
        shell->runTool("several");
        check(tl->multiSelect(), "Select turns on selecting several");
        const QRectF r2 = tl->eventRect(ids.e2);
        f.tap(r2.center());
        uitest::settle(350);
        check(app.selection().size() == 2 && app.selection().count(ids.e1) && app.selection().count(ids.e2), "a tap adds an event to the selection");
        f.tap(r2.center() + QPointF(5, 0));
        uitest::settle(350);
        check(app.selection().size() == 1 && app.selection().count(ids.e1), "a second tap takes it out again");
        shell->runTool("done");
        check(!tl->multiSelect() && app.selection().empty(), "Done ends it");
    }

    // ---- an adjusting sheet closes when the selection changes
    {
        app.selectOnly(ids.e3);
        shell->runTool("speed");
        openSheet(shell);
        app.selectOnly(ids.e1);
        uitest::settle(250);
        check(!shell->sheet(), "the speed sheet closes when another event is selected");
    }

    // ---- effects: the list to add from, and an effect's own settings
    {
        app.selectOnly(ids.e1);
        uitest::settle(30);
        shell->runTool("effects");
        touch::Sheet* s = openSheet(shell);
        check(s != nullptr, "Effects opens its sheet");
        if (QAbstractButton* add = s ? button(s, "touchAddEffect") : nullptr) {
            click(add);
            s = openSheet(shell);
            int rows = 0;
            QAbstractButton* pick = nullptr;
            if (s)
                for (QAbstractButton* b : s->findChildren<QAbstractButton*>())
                    if (auto* tb = dynamic_cast<touch::Button*>(b))
                        if (tb->style() == touch::Button::Row) {
                            ++rows;
                            if (!pick && (b->text().contains("Brightness") || b->text().contains("Blur"))) pick = b;
                        }
            check(rows > 5, "the effects to add are listed", std::to_string(rows));
            uitest::shot(&w, gOut + "/phone_addfx.png");
            if (pick) {
                const size_t before = ev(ids.e1)->fx.size();
                click(pick);
                s = openSheet(shell);
                check(ev(ids.e1)->fx.size() == before + 1, "picking one adds it to the event",
                      std::to_string(before) + " -> " + std::to_string(ev(ids.e1)->fx.size()) + " picked " + pick->text().toStdString());
                int sliders = s ? (int)kids<touch::Slider>(s).size() : 0;
                check(sliders > 0, "and its settings show as sliders", std::to_string(sliders));
                uitest::shot(&w, gOut + "/phone_effect.png");
                if (sliders > 0) {
                    touch::Slider* sl = kids<touch::Slider>(s).front();
                    const double was = sl->value();
                    const size_t steps = app.undoCount();
                    uitest::mouseDrag(sl, QPoint(sl->width() / 2, 49), QPoint(sl->width() - 40, 49), 6);
                    uitest::settle(50);
                    check(sl->value() != was && app.undoCount() == steps + 1, "dragging a setting changes it in one undo step");
                }
            }
        }
        closeSheet(shell);
    }

    // ---- text
    {
        app.setSelection({});
        app.setCursor(1.0);
        uitest::settle(30);
        shell->runTool("text");
        touch::Sheet* s = openSheet(shell);
        QPlainTextEdit* edit = s ? s->findChild<QPlainTextEdit*>("touchTextEdit") : nullptr;
        check(edit != nullptr, "Text opens a sheet to type in");
        if (edit) {
            edit->setPlainText("Hello there");
            uitest::shot(&w, gOut + "/phone_text.png");
            click(button(s, "touchTextOk"));
            uitest::settle(250);
            const Event* t = app.selection().size() == 1 ? ev(*app.selection().begin()) : nullptr;
            const MediaItem* m = t ? app.project().findMedia(t->mediaId) : nullptr;
            const bool haveTitles = m != nullptr;
            check(!haveTitles || (m->generated && m->name == "Hello there" && near(t->start, 1.0, 1.0 / 29.0)),
                  "Add puts the text at the playhead", haveTitles ? "" : "(no Titles & Text generator here)");
            if (haveTitles) {
                shell->runTool("textstyle");
                touch::Sheet* style = openSheet(shell);
                check(style && kids<touch::Slider>(style).size() > 2, "Style shows the text's settings",
                      style ? std::to_string(kids<touch::Slider>(style).size()) : "no sheet");
                uitest::shot(&w, gOut + "/phone_text_style.png");
                closeSheet(shell);
                tl->setPixelsPerSecond(60);
                uitest::settle(300);
                uitest::shot(&w, gOut + "/phone_text_added.png");
                app.undo();
            }
        }
        closeSheet(shell);
    }

    // ---- the menu and every command
    {
        click(button(shell, "touchMenu"));
        touch::Sheet* s = openSheet(shell);
        check(s != nullptr, "the menu button opens the project menu");
        uitest::shot(&w, gOut + "/phone_menu.png");
        closeSheet(shell);
        app.setSelection({});
        uitest::settle(30);
        shell->runTool("commands");
        s = openSheet(shell);
        int rows = 0;
        if (s)
            for (QAbstractButton* b : s->findChildren<QAbstractButton*>())
                if (auto* tb = dynamic_cast<touch::Button*>(b))
                    if (tb->style() == touch::Button::Row) ++rows;
        check(rows > 150, "All commands lists the desktop menus' commands", std::to_string(rows));
        uitest::shot(&w, gOut + "/phone_commands.png");
        closeSheet(shell);
    }

    // ---- Export: a small video, with its progress, then the file
    {
        app.setSelection({});
        QSettings().setValue("touch/exportDir", gOut);
        click(button(shell, "touchExport"));
        touch::Sheet* s = openSheet(shell);
        check(s && s->objectName() == "touchExportSheet", "Export opens its sheet");
        uitest::shot(&w, gOut + "/phone_export.png");
        if (s) {
            click(button(s, "touchExportSize480"));
            click(button(s, "touchExportGo"));
            uitest::settle(300);
            touch::Sheet* prog = shell->sheet();
            check(prog && prog->objectName() == "touchExportProgress", "Export shows its progress");
            QElapsedTimer t;
            t.start();
            QString title;
            while (t.elapsed() < 120000) {
                uitest::settle(200);
                if (QAbstractButton* c = shell->sheet() ? button(shell->sheet(), "touchExportCancel") : nullptr)
                    if (c->text() == "Done") break;
            }
            uitest::shot(&w, gOut + "/phone_exported.png");
            const QStringList made = QDir(gOut).entryList(QStringList() << "*.mp4", QDir::Files);
            check(!made.isEmpty() && QFileInfo(gOut + "/" + made.front()).size() > 10000, "the video is made",
                  made.isEmpty() ? std::string("none") : made.front().toStdString());
            if (QAbstractButton* c = shell->sheet() ? button(shell->sheet(), "touchExportCancel") : nullptr) click(c);
            uitest::settle(250);
        }
        // the sound alone
        click(button(shell, "touchExport"));
        if (touch::Sheet* s2 = openSheet(shell)) {
            click(button(s2, "touchExportSound"));
            click(button(s2, "touchExportGo"));
            QElapsedTimer t;
            t.start();
            QAbstractButton* done = nullptr;
            while (t.elapsed() < 60000 && !done) {
                uitest::settle(100);
                if (QAbstractButton* c = shell->sheet() ? button(shell->sheet(), "touchExportCancel") : nullptr)
                    if (c->text() == "Done") done = c;
            }
            QLabel* status = shell->sheet() ? shell->sheet()->findChild<QLabel*>("touchExportStatus") : nullptr;
            check(done && status && status->text().startsWith("Saved"), "the sound alone is exported", status ? status->text().toStdString() : "");
            click(done);
            uitest::settle(250);
        }
        check(!shell->sheet(), "Done closes the export sheet");
    }

    // ---- play and pause from the transport
    {
        app.setCursor(2.0);
        click(button(shell, "touchPlay"));
        uitest::settle(400);
        check(w.playback()->playing(), "the play button plays");
        click(button(shell, "touchPlay"));
        uitest::settle(50);
        check(!w.playback()->playing() && app.cursor() > 2.0, "and pauses where it got to", std::to_string(app.cursor()));
    }

    // ---- the other shapes
    app.selectOnly(ids.e3);
    app.setCursor(5.0);
    shotAt(w, QSize(892, 412), "phone_landscape");
    check(shell->wide(), "a phone held sideways gets the wide layout");
    shell->runTool("speed");
    openSheet(shell);
    uitest::shot(&w, gOut + "/phone_landscape_speed.png");
    closeSheet(shell);
    shotAt(w, QSize(820, 1180), "tablet_portrait");
    check(!shell->wide() && shell->tablet(), "an upright tablet gets the tall layout with tablet sizes");
    shotAt(w, QSize(1180, 820), "tablet_landscape");
    app.setSelection({});
    shotAt(w, QSize(1180, 820), "tablet_landscape_nothing");

    // ---- paint time of the touch timeline (a full repaint, as when scrolling)
    {
        w.resize(412, 892);
        uitest::settle(200);
        QImage img(tl->size() * 2, QImage::Format_ARGB32_Premultiplied);
        img.setDevicePixelRatio(2);
        QElapsedTimer t;
        t.start();
        for (int k = 0; k < 30; ++k) {
            app.setCursor(1.0 + k * 0.1);
            tl->render(&img);
        }
        const double ms = t.elapsed() / 30.0;
        std::printf("touch timeline paint: %.2f ms at 2x\n", ms);
        check(ms < 16, "the touch timeline paints in under a frame", std::to_string(ms));
    }

    // ---- back to the desktop interface
    w.setTouchMode(false);
    uitest::settle(200);
    check(!w.touchShell() && !w.menuBar()->isHidden(), "the desktop interface comes back");
    check(w.preview() && !w.preview()->isHidden() && w.preview()->parentWidget() && w.preview()->parentWidget()->objectName() == "dockPreviewPage",
          "the Video Preview goes back to its docked window");
    w.resize(1366, 760);
    uitest::settle(300);
    uitest::shot(&w, gOut + "/desktop_after.png");
    return uitest::summary();
}
