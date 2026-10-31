// Timeline pointers (cursors.h) over an event's edges (trim; Ctrl stretch, Alt slip-trim), fade handles, Alt (slip) and the
// envelope tools; sideways wheel scrolls instead of zooming; a default-width header hides Arm for Record and Project Media's
// tool bar shows pictures.
//
//   cursors_test <output folder> [file.ofx ...]
//
// Saves cursors.png (every pointer, light and dark, at 2x) and media_bar.png.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/cursors.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QKeyEvent>
#include <QPainter>
#include <QSettings>
#include <QWheelEvent>

using namespace reno;
using namespace reno::app;
using uitest::check;

static qint64 keyOf(const QCursor& c) { return c.shape() == Qt::BitmapCursor ? c.pixmap().cacheKey() : -(qint64)c.shape() - 1; }
static bool showing(QWidget* w, EditCursor k) { return keyOf(w->cursor()) == keyOf(editCursor(k)); }

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoCursorsTest");
    QApplication::setApplicationName("cursors_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("cursors_out");
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, std::vector<std::string>());

    // ---- the pointers: each made once, all different
    {
        const int n = (int)EditCursor::Count;
        QImage sheet(n * 72, 144, QImage::Format_ARGB32);
        sheet.fill(QColor(200, 200, 200));
        QPainter p(&sheet);
        p.fillRect(QRect(0, 72, sheet.width(), 72), QColor(45, 45, 45));
        std::set<qint64> keys;
        for (int k = 0; k < n; ++k) {
            const QCursor& c = editCursor((EditCursor)k);
            keys.insert(c.pixmap().cacheKey());
            const QPixmap pm = c.pixmap();
            for (int row = 0; row < 2; ++row) p.drawPixmap(QRect(k * 72 + 4, row * 72 + 4, 64, 64), pm);
            p.setPen(Qt::red);
            p.drawPoint(k * 72 + 4 + c.hotSpot().x() * 2, 4 + c.hotSpot().y() * 2);
        }
        p.end();
        sheet.save(out + "/cursors.png");
        check((int)keys.size() == n && keyOf(editCursor(EditCursor::TrimLeft)) == keyOf(editCursor(EditCursor::TrimLeft)), "every pointer is its own picture, made once");
    }

    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    uint64_t vt = 0, at = 0, ev = 0;
    app.edit("Cursors", [&](Project& p) {
        Track v, s;
        v.id = p.newId();
        v.type = TrackType::Video;
        s.id = p.newId();
        s.type = TrackType::Audio;
        vt = v.id;
        at = s.id;
        Event e;
        e.id = p.newId();
        ev = e.id;
        e.mediaId = media;
        e.start = 2.0;
        e.length = 4.0;
        e.fadeIn = 1.0;
        e.name = "synth:1";
        v.events.push_back(e);
        Event a = e;
        a.id = p.newId();
        a.fadeIn = 0;
        s.events.push_back(a);
        p.tracks.push_back(v);
        p.tracks.push_back(s);
    });

    MainWindow w;
    w.resize(1400, 900);
    w.show();
    uitest::settle(300);
    TimelineWidget* tl = w.timeline();
    tl->setFocus();
    uitest::settle(50);
    const QRect vr = tl->trackRect(vt);
    const int midY = vr.center().y() + 6;
    const int x0 = tl->xAt(2.0), x1 = tl->xAt(6.0), xm = tl->xAt(4.5);
    auto hoverAt = [&](QPoint at, Qt::KeyboardModifiers mods = Qt::NoModifier) {
        uitest::mouseMove(tl, at, Qt::NoButton, mods);
        uitest::settle(10);
    };

    // ---- over an event's edges
    hoverAt(QPoint(x0 + 1, midY));
    check(showing(tl, EditCursor::TrimLeft), "over an event's left edge: the trim pointer (a bracket opening into the event)");
    hoverAt(QPoint(x0 + 1, midY), Qt::ControlModifier);
    check(showing(tl, EditCursor::StretchLeft), "with Ctrl: the stretch pointer");
    hoverAt(QPoint(x0 + 1, midY), Qt::AltModifier);
    check(showing(tl, EditCursor::SlipTrimLeft), "with Alt: the slip-trim pointer");
    hoverAt(QPoint(x1 - 1, midY));
    check(showing(tl, EditCursor::TrimRight), "over its right edge: the trim pointer the other way round");
    {
        // Ctrl pressed and released without moving the mouse changes the pointer at once (the system sends the state before the key)
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Control, Qt::NoModifier);
        QApplication::sendEvent(tl, &press);
        check(showing(tl, EditCursor::StretchRight), "Ctrl pressed over the edge: the stretch pointer at once");
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Control, Qt::ControlModifier);
        QApplication::sendEvent(tl, &release);
        check(showing(tl, EditCursor::TrimRight), "and let go: the trim pointer again");
    }
    // ---- in the middle: the ordinary arrow; with Alt the slip pointer
    hoverAt(QPoint(xm, midY));
    check(tl->cursor().shape() == Qt::ArrowCursor, "in the middle of an event: the arrow");
    hoverAt(QPoint(xm, midY), Qt::AltModifier);
    check(showing(tl, EditCursor::Slip), "with Alt: the slip pointer");
    // ---- the fade-in handle (at the top edge where the fade ends)
    {
        bool found = false;
        const int fx = tl->xAt(3.0);
        for (int dy = 0; dy < 8 && !found; ++dy) {
            hoverAt(QPoint(fx, vr.top() + dy));
            found = showing(tl, EditCursor::FadeIn);
        }
        check(found, "over the fade-in handle: the fade pointer");
    }
    // ---- the Zoom tool over the tracks
    tl->options().tool = EditTool::Zoom;
    tl->optionsChanged();
    hoverAt(QPoint(tl->xAt(8.0), midY));
    check(showing(tl, EditCursor::Zoom), "the Zoom tool: the magnifier");
    tl->options().tool = EditTool::Envelope;
    tl->optionsChanged();
    hoverAt(QPoint(tl->xAt(8.5), midY));
    check(showing(tl, EditCursor::EnvelopeTool), "the Envelope tool: its pointer");
    tl->options().tool = EditTool::Normal;
    tl->optionsChanged();
    hoverAt(QPoint(tl->xAt(9.0), midY));
    check(tl->cursor().shape() == Qt::ArrowCursor, "the Normal tool over an empty track: the arrow again");

    // ---- the wheel: tilted sideways it scrolls, upright it zooms
    {
        const double s0 = tl->viewStartTime(), span0 = tl->viewEndTime() - tl->viewStartTime();
        const QPoint at(tl->xAt(4.0), midY);
        QWheelEvent side(QPointF(at), QPointF(tl->mapToGlobal(at)), QPoint(0, 0), QPoint(-240, 0), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QApplication::sendEvent(tl, &side);
        uitest::settle(20);
        const double span1 = tl->viewEndTime() - tl->viewStartTime();
        check(tl->viewStartTime() > s0 && std::fabs(span1 - span0) < 1e-6, "a wheel tilted sideways scrolls the view sideways and leaves the zoom",
              std::to_string(s0) + " -> " + std::to_string(tl->viewStartTime()) + ", " + std::to_string(span0) + " -> " + std::to_string(span1));
        uitest::wheel(tl, at, 120);
        uitest::settle(20);
        check(tl->viewEndTime() - tl->viewStartTime() < span1 * 0.95, "the wheel zooms (VEGAS)");
    }

    // ---- the Arm for Record button: VEGAS 18 leaves it out of a header of its default width
    {
        check(tl->headerPartRect(at, TimelineWidget::PartArm).isEmpty(), "no Arm for Record button in a header of the default width");
        app.armedTracks.insert(at);
        app.notify(kProject);
        uitest::settle(50);
        check(!tl->headerPartRect(at, TimelineWidget::PartArm).isEmpty(), "but an armed track shows it (lit)");
        app.armedTracks.clear();
        app.notify(kProject);
        tl->setHeaderWidth(260);
        uitest::settle(50);
        check(!tl->headerPartRect(at, TimelineWidget::PartArm).isEmpty(), "and a wider header has room for it");
        tl->setHeaderWidth(161);
    }

    // ---- double-clicking the time selection bar between two markers selects the time between them (VEGAS)
    {
        app.edit("Markers", [&](Project& p) {
            Project::Marker m1, m2;
            m1.id = p.newId();
            m2.id = p.newId();
            m1.time = 1.0;
            m2.time = 5.0;
            p.markers.push_back(m1);
            p.markers.push_back(m2);
        });
        app.setLoop(0.0, 0.0);
        tl->zoomToFit();
        uitest::settle(30);
        const QRect bar = tl->loopBarRect();
        uitest::mouseDoubleClick(tl, QPoint(tl->xAt(3.0), bar.center().y()));
        uitest::settle(30);
        check(std::fabs(app.loopStart - 1.0) < 1e-6 && std::fabs(app.loopEnd - 5.0) < 1e-6, "double-clicking the time selection bar between markers selects the time between them",
              std::to_string(app.loopStart) + " " + std::to_string(app.loopEnd));
    }

    // ---- Project Media's tool bar: pictures
    QAbstractButton* import = w.findChild<QAbstractButton*>("importMedia");
    QWidget* bin = import ? import->parentWidget() : nullptr;
    if (import) {
        check(import->text().isEmpty() && !import->icon().isNull() && import->toolTip().startsWith("Import Media"), "Project Media's buttons are pictures with tool tips");
        uitest::shot(bin, out + "/media_bar.png");
    } else {
        check(false, "Project Media's Import Media button is there");
    }
    (void)ev;
    return uitest::summary();
}
