// The whole main window driven by thousands of random (but valid) user actions with a fixed seed, to find crashes, sanitizer
// reports, hangs and broken projects. Build under ASan + UBSan.
//
//   monkey_test <output folder> [testfx.ofx] [--seed N | --seeds A-B] [--steps N] [--samples folder] [--all-samples] [--verbose]
//   (the sample VEGAS projects default to the folder named by $RENO_SAMPLES; without them those parts SKIP)
//               [--standins <perf-media folder> [--real <folder>]] [--stale] [--every [N]]
//   --all-samples: open every sample project in turn first, with 20 random steps on each.
//   --standins: open the samples from copies of their folders in which every file they use is a stand-in clip of the same kind
//     (perf-media's clip480/720/1080.mp4, tone.wav, still.png, as load_lag_test does), so they play and show thumbnails and
//     waveforms instead of being offline. --real: a folder laid out like the samples folder holding some of their real media.
//   --stale: after every step, check what the window shows against what it would show drawn afresh: the timeline on screen
//     against a whole repaint and against a new timeline of the same size and view, the preview's picture against one rendered
//     now, and Edit Details, Project Media and the mixer against new ones.
//   --every [N]: instead of random steps, run every menu command, shortcut and AppState editing command in turn, on the demo
//     project and on N samples (default 2), each with the stale check, undone after. --every-sample <part of a name> selects
//     samples; --every-from N starts at the Nth command; --every-no-demo uses the samples only.
//
// Each step is a menu command (every QAction of the window), a keyboard shortcut (VEGAS's keys, pressed through the window
// system), a mouse gesture on the timeline (moving, trimming, slipping, stretching, fades, envelope points, modifier clicks,
// right-click menus), undo / redo runs, play / stop / shuttle, the look switched, the window resized, media added, or a sample
// project (copied from the read-only folder) opened, saved as a .veg in a temporary folder and opened again. Modal dialogs and
// menus that come up are poked at a little and then accepted or cancelled by a timer.
//
// After every step the project is checked: events sorted on each track, no negative or non-finite times, unique ids, references
// to media, buses and tracks that exist. After a step that made undo steps, Undo must give back exactly the previous project (as
// JSON) and Redo the next. Every so often the project is saved (as .reno and .veg) and loaded back: the .reno must load as the
// same project, and a .veg saved, loaded, saved and loaded again must load the same both times.
//
// A step log is written to <output folder>/monkey_steps_<seed>.log (flushed every step, so after a crash its last line is the
// step that crashed). A watchdog stops the program with the main thread's stack (SIGABRT) if a step hangs.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/app/panels.h"
#include "../../src/app/mixer.h"
#include "../../src/vegas/vegas.h"
#include "../../src/vegas/vegformat.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QBackingStore>
#include <QLayout>
#include <QScrollBar>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QKeyEvent>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTimer>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <map>
#include <pthread.h>
#include <set>
#include <thread>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

// ------------------------------------------------------------------------------------------ random numbers
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x1234567ull) {}
    uint64_t next()
    {
        uint64_t z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    int below(int n) { return n <= 0 ? 0 : (int)(next() % (uint64_t)n); }
    double uniform() { return (next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double a, double b) { return a + (b - a) * uniform(); }
    bool chance(double p) { return uniform() < p; }
    template <class T> const T& pick(const std::vector<T>& v) { return v[(size_t)below((int)v.size())]; }
};

// ------------------------------------------------------------------------------------------ the run's state
MainWindow* gW = nullptr;
Rng* gRng = nullptr;
FILE* gLog = nullptr;
QString gOut, gTmp;
bool gVerbose = false;
bool gAllSamples = false;
std::string gStep;                              // what the current step is doing (for the logs)
std::atomic<int64_t> gLastProgress{0};          // ms since start when the main thread last moved on
std::atomic<bool> gStopWatchdog{false};
pthread_t gMainThread;
QElapsedTimer gClock;
std::map<std::string, int> gFailures;           // failure kind -> count (each kind printed in full only the first times)
std::map<std::string, int> gNoUndo;             // actions that changed the project without an undo step
bool gReset = false;                            // the step started a new project (the undo history begins again)

void progress() { gLastProgress = gClock.elapsed(); }

void logStep(const std::string& s)
{
    gStep = s;
    if (gLog) {
        std::fprintf(gLog, "%s\n", s.c_str());
        std::fflush(gLog);
    }
    if (gVerbose) std::printf("  %s\n", s.c_str());
    progress();
}

// A failure: counted by kind, the first few of each kind printed with the step that made it.
void fail(const std::string& kind, const std::string& detail)
{
    const int n = ++gFailures[kind];
    if (n <= 3) check(false, kind, detail + " [step: " + gStep + "]");
    if (gLog) {
        std::fprintf(gLog, "FAIL %s: %s\n", kind.c_str(), detail.c_str());
        std::fflush(gLog);
    }
}

// ------------------------------------------------------------------------------------------ invariants
bool finite(double v) { return std::isfinite(v); }

// What must hold of any project at any time. Returns the first problem found ("" if none).
std::string problems(const Project& p)
{
    std::set<uint64_t> ids;
    std::string out;
    auto id = [&](uint64_t v, const char* what) {
        if (!out.empty()) return;
        if (v == 0) out = strfmt("%s with id 0", what);
        else if (!ids.insert(v).second) out = strfmt("id %llu used twice (%s)", (unsigned long long)v, what);
        else if (v >= p.nextId) out = strfmt("%s id %llu not below nextId %llu", what, (unsigned long long)v, (unsigned long long)p.nextId);
    };
    auto envs = [&](const Envelopes& list, const std::string& where) {
        for (const Envelope& e : list) {
            for (size_t k = 0; k < e.points.size() && out.empty(); ++k) {
                if (!finite(e.points[k].time) || !finite(e.points[k].value)) out = where + " envelope " + e.kind + ": a point that is not a number";
                else if (k && e.points[k].time < e.points[k - 1].time - 1e-9) out = where + " envelope " + e.kind + strfmt(": points out of order (%g after %g)", e.points[k].time, e.points[k - 1].time);
            }
        }
    };
    for (const MediaItem& m : p.media) id(m.id, "media");
    for (const Bus& b : p.buses) {
        id(b.id, "bus");
        envs(b.envelopes, "bus");
        if (out.empty() && b.output && !p.findBus(b.output)) out = "a bus sends to a bus that is not there";
    }
    for (const auto& mk : p.markers) {
        id(mk.id, "marker");
        if (out.empty() && (!finite(mk.time) || !finite(mk.length) || mk.length < 0)) out = strfmt("marker at %g, length %g", mk.time, mk.length);
    }
    for (size_t ti = 0; ti < p.tracks.size() && out.empty(); ++ti) {
        const Track& t = p.tracks[ti];
        id(t.id, "track");
        const std::string tn = strfmt("track %d", (int)ti + 1);
        envs(t.envelopes, tn);
        if (out.empty() && t.output && !p.findBus(t.output)) out = tn + ": goes to a bus that is not there";
        for (const Send& s : t.sends)
            if (out.empty() && !p.findBus(s.bus)) out = tn + ": a send to a bus that is not there";
        for (size_t k = 1; k < t.motion.size() && out.empty(); ++k)
            if (t.motion[k].time < t.motion[k - 1].time - 1e-9) out = tn + ": track motion keys out of order";
        for (size_t ei = 0; ei < t.events.size() && out.empty(); ++ei) {
            const Event& e = t.events[ei];
            id(e.id, "event");
            const std::string en = tn + strfmt(" event %d", (int)ei + 1);
            if (!out.empty()) break;
            if (!finite(e.start) || !finite(e.length) || !finite(e.offset) || !finite(e.rate) || !finite(e.fadeIn) || !finite(e.fadeOut) || !finite(e.gain))
                out = en + ": a number that is not a number";
            else if (e.length <= 0) out = en + strfmt(": length %g", e.length);
            else if (e.start < -1e-9) out = en + strfmt(": starts at %g", e.start);
            else if (e.rate <= 0) out = en + strfmt(": rate %g", e.rate);
            else if (e.fadeIn < -1e-9 || e.fadeOut < -1e-9) out = en + strfmt(": fades %g %g", e.fadeIn, e.fadeOut);
            else if (ei && e.start < t.events[ei - 1].start - 1e-9) out = en + strfmt(": not in order (starts %g, the one before %g)", e.start, t.events[ei - 1].start);
            else if (e.mediaId && !p.findMedia(e.mediaId)) out = en + ": its media is not in the project";
            else if (e.activeTake < 0 || e.activeTake > (int)e.takes.size()) out = en + strfmt(": active take %d of %d", e.activeTake, (int)e.takes.size() + 1);
            for (const Take& tk : e.takes)
                if (out.empty() && tk.mediaId && !p.findMedia(tk.mediaId)) out = en + ": a take's media is not in the project";
            for (size_t k = 1; k < e.panCrop.size() && out.empty(); ++k)
                if (e.panCrop[k].time < e.panCrop[k - 1].time - 1e-9) out = en + ": pan/crop keys out of order";
            if (out.empty()) envs(e.envelopes, en);
        }
    }
    envs(p.masterEnvelopes, "master");
    envs(p.videoEnvelopes, "video output");
    // effect slots: their ids too (chains, transitions with an effect, generators)
    p.forEachFx([&](const FxSlot& s) {
        if (s.id) id(s.id, "effect");
    });
    return out;
}

// The project as JSON, with every effect's settings as its live instance has them (an effect slot's saved 'state' is only
// refreshed from the instance now and then - Copy, Save, Duplicate Track - outside undo, so use the instance's).
std::string json(const Project& p)
{
    Project c = p;
    st().fx.captureStates(c);
    return c.toJson().dump();
}

// Where two JSON texts part (a little of each), for the failure message.
std::string where(const std::string& a, const std::string& b)
{
    size_t i = 0;
    while (i < a.size() && i < b.size() && a[i] == b[i]) ++i;
    const size_t from = i > 60 ? i - 60 : 0;
    return strfmt("at %zu: ...%s... vs ...%s...", i, a.substr(from, 140).c_str(), b.substr(from, 140).c_str());
}

// ------------------------------------------------------------------------------------------ modal dialogs and menus
// A timer looks at what is open: a popup menu gets one of its commands picked or is closed; a modal dialog gets a few controls
// poked at and is accepted or cancelled (file dialogs are always cancelled; anything open after a while is closed).
QWidget* gModal = nullptr;
int gModalTicks = 0;
int gDialogs = 0, gMenus = 0;
std::set<std::string> gDeny;                    // commands never run (they start other programs or end the program)

bool denied(QAction* a)
{
    if (!a) return true;
    const std::string n = a->objectName().toStdString();
    QString t = a->text();
    t.remove('&');
    if (gDeny.count(n)) return true;
    for (const char* bad : {"Exit", "Open Nested Timeline", "Open Parent", "Audio Editor", "Open in ", "Contents and Index", "Online", "Check for Updates",
                            "Run Script", "Look for New Plug-Ins", "Rescan", "Explore", "Browse", "Open Containing Folder", "Show in Explorer", "Help"})
        if (t.contains(bad, Qt::CaseInsensitive)) return true;
    return false;
}

void pokeDialog(QWidget* d)
{
    Rng& r = *gRng;
    const int n = r.below(4);
    for (int k = 0; k < n; ++k) {
        switch (r.below(4)) {
            case 0: {
                QList<QCheckBox*> boxes = d->findChildren<QCheckBox*>();
                if (!boxes.isEmpty()) {
                    QCheckBox* b = boxes[r.below(boxes.size())];
                    if (b->isVisible() && b->isEnabled()) b->click();
                }
                break;
            }
            case 1: {
                QList<QComboBox*> combos = d->findChildren<QComboBox*>();
                if (!combos.isEmpty()) {
                    QComboBox* c = combos[r.below(combos.size())];
                    if (c->isVisible() && c->isEnabled() && c->count() > 0) c->setCurrentIndex(r.below(c->count()));
                }
                break;
            }
            case 2: {
                QList<QAbstractSpinBox*> spins = d->findChildren<QAbstractSpinBox*>();
                if (!spins.isEmpty()) {
                    QAbstractSpinBox* s = spins[r.below(spins.size())];
                    if (s->isVisible() && s->isEnabled()) s->stepBy(r.below(21) - 10);
                }
                break;
            }
            case 3: {
                QList<QAbstractSlider*> sliders = d->findChildren<QAbstractSlider*>();
                if (!sliders.isEmpty()) {
                    QAbstractSlider* s = sliders[r.below(sliders.size())];
                    if (s->isVisible() && s->isEnabled()) s->setValue(s->minimum() + r.below(s->maximum() - s->minimum() + 1));
                }
                break;
            }
        }
    }
}

void closeModal(QWidget* m)
{
    Rng& r = *gRng;
    ++gDialogs;
    if (gLog) {
        std::fprintf(gLog, "  dialog %s \"%s\"\n", m->metaObject()->className(), m->windowTitle().toUtf8().constData());
        std::fflush(gLog);
    }
    if (QFileDialog* f = qobject_cast<QFileDialog*>(m)) { f->reject(); return; }
    if (QProgressDialog* p = qobject_cast<QProgressDialog*>(m)) {
        if (gModalTicks > 50) p->cancel();
        return;
    }
    if (QMessageBox* b = qobject_cast<QMessageBox*>(m)) {
        QList<QAbstractButton*> buttons = b->buttons();
        if (buttons.isEmpty()) b->close();
        else buttons[r.below(buttons.size())]->click();
        return;
    }
    if (QInputDialog* in = qobject_cast<QInputDialog*>(m)) {
        if (r.chance(0.5)) in->accept();
        else in->reject();
        return;
    }
    if (QDialog* d = qobject_cast<QDialog*>(m)) {
        if (r.chance(0.6)) pokeDialog(d);
        if (r.chance(0.5)) d->accept();
        else d->reject();
        return;
    }
    m->close();
}

void lookAtModals()
{
    if (QWidget* pop = QApplication::activePopupWidget()) {
        ++gMenus;
        QMenu* menu = qobject_cast<QMenu*>(pop);
        if (menu && gRng->chance(0.6)) {
            std::vector<QAction*> acts;
            for (QAction* a : menu->actions())
                if (a->isEnabled() && a->isVisible() && !a->isSeparator() && !a->menu() && !denied(a)) acts.push_back(a);
            if (!acts.empty()) {
                QAction* a = gRng->pick(acts);
                if (gLog) {
                    std::fprintf(gLog, "  menu item \"%s\"\n", a->text().toUtf8().constData());
                    std::fflush(gLog);
                }
                // clicked as a user would (the menu triggers it, closes, and its exec() returns it), from the menu's own event loop, not this
                // timer's handler: Qt doesn't re-run a timer's handler while it is running, so a dialog the command opens couldn't be closed here
                QPointer<QMenu> mp(menu);
                QPointer<QAction> ap(a);
                QMetaObject::invokeMethod(menu, [mp, ap] {
                    if (!mp || !ap) return;
                    mp->setActiveAction(ap);
                    const QPoint c = mp->actionGeometry(ap).center();
                    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(c), QPointF(mp->mapToGlobal(c)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(mp, &release);
                    if (mp && mp->isVisible()) mp->close();
                }, Qt::QueuedConnection);
                return;
            }
        }
        pop->close();
        return;
    }
    QWidget* m = QApplication::activeModalWidget();
    if (!m) { gModal = nullptr; return; }
    if (m != gModal) { gModal = m; gModalTicks = 0; }
    ++gModalTicks;
    // from the dialog's own event loop, not this timer's handler (see above)
    QPointer<QWidget> mp(m);
    const bool stuck = gModalTicks > 400;  // about 8 s
    if (gModalTicks == 1500) {  // 30 s and still there: it will not close
        fail("a dialog cannot be closed", std::string(m->metaObject()->className()) + " \"" + m->windowTitle().toStdString() + "\"");
        if (QDialog* d = qobject_cast<QDialog*>(m)) d->done(QDialog::Rejected);
        else m->hide();
        return;
    }
    if (gModalTicks >= 2 || qobject_cast<QProgressDialog*>(m))
        QMetaObject::invokeMethod(m, [mp, stuck] {
            if (!mp || QApplication::activeModalWidget() != mp) return;
            if (stuck) mp->close();
            else closeModal(mp);
        }, Qt::QueuedConnection);
}

// ------------------------------------------------------------------------------------------ input
QString keyText(int key, Qt::KeyboardModifiers mods)
{
    if (mods & (Qt::ControlModifier | Qt::AltModifier)) return QString();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar((mods & Qt::ShiftModifier) ? key : key + 32));
    if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) return QString(QChar(key));
    return QString();
}

void pressKey(int key, Qt::KeyboardModifiers mods)
{
    gW->activateWindow();
    QWindow* win = QGuiApplication::focusWindow() ? QGuiApplication::focusWindow() : gW->windowHandle();
    const QString text = keyText(key, mods);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyPress, key, mods, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyRelease, key, mods, text);
    QCoreApplication::processEvents();
}

// The keys of VEGAS's shortcut table, as key presses ("Num 4" is the keypad's 4).
struct KeyPress { int key; Qt::KeyboardModifiers mods; std::string name; };
std::vector<std::vector<KeyPress>> shortcutKeys()
{
    std::vector<std::vector<KeyPress>> out;
    for (const ShortcutInfo& s : shortcutList()) {
        if (s.kind == ShortcutInfo::NotAvailable || !s.keys || !*s.keys) continue;
        const QStringList alts = QString(s.keys).split(" or ");
        for (QString a : alts) {
            a = a.trimmed();
            if (a.contains("click", Qt::CaseInsensitive) || a.contains("drag", Qt::CaseInsensitive) || a.contains("wheel", Qt::CaseInsensitive)) continue;
            std::vector<KeyPress> seq;
            bool ok = true;
            for (QString part : a.split(", ")) {
                Qt::KeyboardModifiers kp = Qt::NoModifier;
                if (part.contains("Num ")) {
                    part.replace("Num ", "");
                    kp = Qt::KeypadModifier;
                    if (part.endsWith("Enter")) part.replace("Enter", "Enter");
                }
                const QKeySequence ks(part);
                if (ks.isEmpty() || ks.count() != 1 || ks[0].key() == Qt::Key_unknown) { ok = false; break; }
                seq.push_back({(int)ks[0].key(), ks[0].keyboardModifiers() | kp, std::string(s.command) + " [" + a.toStdString() + "]"});
            }
            if (ok && !seq.empty()) out.push_back(seq);
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------ the steps
struct Context {
    std::vector<QAction*> actions;
    std::vector<std::vector<KeyPress>> keys;
    std::vector<QString> samples;               // copies of the user's projects
    std::vector<std::string> history;           // the project's JSON at each undo depth (as last seen)
    int step = 0;
    int opened = 0, saved = 0, undone = 0;
};

// Remembers the project at the current undo depth. A step that made several undo steps leaves the depths between unknown;
// one that lowered the depth without undoing (history cleared, a project opened) leaves all unknown.
void remember(Context& c, size_t depthBefore, bool undoStep)
{
    const size_t d = st().undoCount();
    c.history.resize(std::max(c.history.size(), d + 1));
    if (!undoStep) {
        if (d > depthBefore)
            for (size_t k = depthBefore + 1; k < d; ++k) c.history[k].clear();
        if (d < depthBefore)
            for (size_t k = 0; k < d; ++k) c.history[k].clear();
        c.history.resize(d + 1);
    }
    c.history[d] = json(st().project());
}

std::vector<const Event*> allEvents()
{
    std::vector<const Event*> out;
    for (const Track& t : st().project().tracks)
        for (const Event& e : t.events) out.push_back(&e);
    return out;
}

const Track* trackOf(uint64_t eventId)
{
    const Track* t = nullptr;
    st().project().findEvent(eventId, &t);
    return t;
}

void stepAction(Context& c)
{
    QAction* a = gRng->pick(c.actions);
    if (!a || !a->isEnabled()) return;
    logStep(strfmt("action %s \"%s\"", a->objectName().toUtf8().constData(), a->text().toUtf8().constData()));
    a->trigger();
}

void stepKeys(Context& c)
{
    const std::vector<KeyPress>& seq = gRng->pick(c.keys);
    logStep("keys " + seq[0].name);
    // timeline focused most of the time: the keys it acts on itself need it
    if (gRng->chance(0.7)) gW->timeline()->setFocus();
    for (const KeyPress& k : seq) pressKey(k.key, k.mods);
}

Qt::KeyboardModifiers randomMods()
{
    Rng& r = *gRng;
    Qt::KeyboardModifiers m = Qt::NoModifier;
    if (r.chance(0.2)) m |= Qt::ControlModifier;
    if (r.chance(0.2)) m |= Qt::AltModifier;
    if (r.chance(0.2)) m |= Qt::ShiftModifier;
    return m;
}

void stepMouse(Context&)
{
    Rng& r = *gRng;
    TimelineWidget* tl = gW->timeline();
    std::vector<const Event*> evs = allEvents();
    if (evs.empty() || r.chance(0.15)) {
        // somewhere at random: the ruler, the marker bar, headers, empty space
        const QPoint a(r.below(tl->width()), r.below(tl->height())), b(r.below(tl->width()), r.below(tl->height()));
        const int kind = r.below(5);
        logStep(strfmt("mouse anywhere %d (%d,%d)->(%d,%d)", kind, a.x(), a.y(), b.x(), b.y()));
        if (kind == 0) uitest::mouseClick(tl, a, Qt::LeftButton, randomMods());
        else if (kind == 1) uitest::mouseDrag(tl, a, b, 6, randomMods());
        else if (kind == 2) uitest::mouseDoubleClick(tl, a);
        else if (kind == 3) uitest::wheel(tl, a, (r.below(5) - 2) * 120, randomMods());
        else {
            uitest::mouseClick(tl, a, Qt::RightButton);
            QContextMenuEvent ev(QContextMenuEvent::Mouse, a, tl->mapToGlobal(a));
            QCoreApplication::sendEvent(tl, &ev);
        }
        return;
    }
    const Event& e = *r.pick(evs);
    const uint64_t eid = e.id;
    const Track* t = trackOf(eid);
    if (!t) return;
    // in view first, sometimes
    if (tl->xAt(e.start) > tl->width() || tl->xAt(e.end()) < tl->headerWidth() || r.chance(0.1)) tl->zoomToFit();
    const QRect tr = tl->trackRect(t->id);
    if (tr.isEmpty()) return;
    const int x0 = tl->xAt(e.start), x1 = tl->xAt(e.end());
    const int ymid = tr.center().y(), ytop = tr.top() + 3;
    const int dx = (int)r.range(-120, 120), dy = r.chance(0.3) ? (int)r.range(-tr.height() * 2, tr.height() * 2) : 0;
    const int kind = r.below(11);
    const Qt::KeyboardModifiers mods = r.chance(0.3) ? randomMods() : Qt::KeyboardModifiers(Qt::NoModifier);
    QPoint from, to;
    const char* what = "";
    switch (kind) {
        case 0: what = "move"; from = QPoint((x0 + x1) / 2, ymid); to = from + QPoint(dx, dy); break;
        case 1: what = "trim start"; from = QPoint(x0 + 1, ymid); to = from + QPoint(dx, 0); break;
        case 2: what = "trim end"; from = QPoint(x1 - 1, ymid); to = from + QPoint(dx, 0); break;
        case 3: what = "slip (Alt)"; from = QPoint((x0 + x1) / 2, ymid); to = from + QPoint(dx, 0); break;
        case 4: what = "stretch (Ctrl edge)"; from = QPoint(x1 - 1, ymid); to = from + QPoint(dx, 0); break;
        case 5: what = "fade in"; from = QPoint(x0 + 2, ytop); to = from + QPoint(std::abs(dx), 0); break;
        case 6: what = "fade out"; from = QPoint(x1 - 2, ytop); to = from - QPoint(std::abs(dx), 0); break;
        case 7: what = "click"; from = to = QPoint((x0 + x1) / 2, ymid); break;
        case 8: what = "double-click"; from = to = QPoint((x0 + x1) / 2, ymid); break;
        case 9: what = "right-click"; from = to = QPoint((x0 + x1) / 2, ymid); break;
        case 10: what = "envelope point"; break;
    }
    if (kind == 10) {
        // an envelope point of the event's track, dragged (or a point added by a double-click on the line)
        for (const Envelope& env : t->envelopes) {
            if (env.points.empty()) continue;
            EnvelopeRef ref;
            ref.owner.kind = EnvelopeOwner::Track;
            ref.owner.id = t->id;
            ref.kind = env.kind;
            ref.target = env.target;
            ref.param = env.param;
            const QPoint p = tl->envelopePointPos(ref, r.below((int)env.points.size()));
            if (p.x() < 0) continue;
            logStep(strfmt("mouse envelope %s point drag (%d,%d) by (%d,%d)", env.kind.c_str(), p.x(), p.y(), dx, dy / 4));
            uitest::mouseDrag(tl, p, p + QPoint(dx, dy / 4), 6, mods);
            return;
        }
        logStep("mouse envelope double-click on the track");
        uitest::mouseDoubleClick(tl, QPoint((x0 + x1) / 2, ymid));
        return;
    }
    Qt::KeyboardModifiers m = mods;
    if (kind == 3) m |= Qt::AltModifier;
    if (kind == 4) m |= Qt::ControlModifier;
    logStep(strfmt("mouse %s event %llu (%d,%d)->(%d,%d) mods %x", what, (unsigned long long)eid, from.x(), from.y(), to.x(), to.y(), (unsigned)m));
    if (kind == 7) uitest::mouseClick(tl, from, Qt::LeftButton, m);
    else if (kind == 8) uitest::mouseDoubleClick(tl, from);
    else if (kind == 9) {
        uitest::mouseClick(tl, from, Qt::RightButton, m);
        // the window system makes the context menu event of a right click; pretend clicks don't
        QContextMenuEvent ev(QContextMenuEvent::Mouse, from, tl->mapToGlobal(from), m);
        QCoreApplication::sendEvent(tl, &ev);
    }
    else if (r.chance(0.1)) {
        // a drag given up with Escape half-way
        uitest::mousePress(tl, from, Qt::LeftButton, m);
        uitest::mouseMove(tl, (from + to) / 2, Qt::LeftButton, m);
        pressKey(Qt::Key_Escape, Qt::NoModifier);
        uitest::mouseRelease(tl, (from + to) / 2, Qt::LeftButton, m);
    } else
        uitest::mouseDrag(tl, from, to, 2 + r.below(8), m);
}

void stepUndoRedo(Context& c)
{
    Rng& r = *gRng;
    const int undos = r.below(6), redos = r.below(undos + 2);
    logStep(strfmt("undo %d, redo %d", undos, redos));
    for (int k = 0; k < undos && st().canUndo(); ++k) {
        st().undo();
        ++c.undone;
        const size_t d = st().undoCount();
        if (d < c.history.size() && !c.history[d].empty()) {
            const std::string now = json(st().project());
            if (now != c.history[d]) fail("undo does not give back the project as it was", where(now, c.history[d]));
        }
    }
    for (int k = 0; k < redos && st().canRedo(); ++k) {
        st().redo();
        const size_t d = st().undoCount();
        if (d < c.history.size() && !c.history[d].empty()) {
            const std::string now = json(st().project());
            if (now != c.history[d]) fail("redo does not give back the project as it was", where(now, c.history[d]));
        }
    }
    QCoreApplication::processEvents();
}

void stepPlayback(Context&)
{
    Rng& r = *gRng;
    static const int keys[] = {Qt::Key_Space, Qt::Key_J, Qt::Key_K, Qt::Key_L, Qt::Key_Return, Qt::Key_Home, Qt::Key_End};
    const int key = keys[r.below(7)];
    logStep(strfmt("playback key %x", key));
    gW->timeline()->setFocus();
    pressKey(key, r.chance(0.2) ? Qt::ControlModifier : Qt::NoModifier);
    uitest::settle(r.below(150));
    if (r.chance(0.4)) gW->playback()->pause();
}

void stepResize(Context&)
{
    Rng& r = *gRng;
    const int w = 600 + r.below(1400), h = 400 + r.below(800);
    logStep(strfmt("resize %dx%d", w, h));
    gW->resize(w, h);
    uitest::settle(20);
}

void stepMedia(Context&)
{
    Rng& r = *gRng;
    static const char* kinds[] = {"synth:", "synthpic:", "synthaudio:"};
    const std::string path = kinds[r.below(3)] + std::to_string(1 + r.below(5));
    logStep("add media " + path + " at the cursor");
    std::string err;
    const uint64_t m = st().importMedia(path, &err);
    if (!m) return;
    std::vector<const Track*> tracks;
    for (const Track& t : st().project().tracks) tracks.push_back(&t);
    st().setCursor(r.range(0, std::max(1.0, st().project().length())));
    const uint64_t hint = (!tracks.empty() && r.chance(0.6)) ? r.pick(tracks)->id : 0;
    st().addMediaToTimeline(m, st().cursor(), hint);
}

void stepSelect(Context&)
{
    Rng& r = *gRng;
    std::vector<const Event*> evs = allEvents();
    std::set<uint64_t> sel;
    for (const Event* e : evs)
        if (r.chance(0.3)) sel.insert(e->id);
    logStep(strfmt("select %d events, cursor and time selection", (int)sel.size()));
    st().setSelection(sel);
    const double len = std::max(1.0, st().project().length());
    st().setCursor(r.range(0, len));
    if (r.chance(0.5)) {
        const double a = r.range(0, len), b = r.range(0, len);
        st().setLoop(std::min(a, b), std::max(a, b));
    }
    if (!st().project().tracks.empty()) st().currentTrack = st().project().tracks[(size_t)r.below((int)st().project().tracks.size())].id;
    st().notify(kSelection);
}

// Saves the project as a .reno and as a .veg (in the temporary folder) and loads them back.
void checkSaveLoad(Context& c, const std::string& tag)
{
    const Project& p = st().project();
    const std::string base = (gTmp + "/" + QString::fromStdString(tag)).toStdString();
    // .reno: exactly the same project
    {
        std::string err;
        if (!p.save(base + ".reno", &err)) fail(".reno save fails", err);
        else {
            Project q;
            if (!q.load(base + ".reno", &err)) fail(".reno load fails", err);
            else {
                const std::string a = json(p), b = json(q);
                if (a != b) fail(".reno saved and loaded is not the same project", where(a, b));
            }
        }
    }
    // .veg: saved, loaded, saved and loaded again: the same both times
    {
        Project copy = p;
        const VegasSaveResult r1 = saveVegasProject(copy, base + "_1.veg");
        if (!r1.ok) { fail(".veg save fails", r1.error); return; }
        Project q1, q2;
        std::string e;
        if (!loadProjectFile(base + "_1.veg", q1, &e)) { fail(".veg written does not load", e); return; }
        if (const std::string pr = problems(q1); !pr.empty()) fail("a .veg loads as a broken project", pr);
        const VegasSaveResult r2 = saveVegasProject(q1, base + "_2.veg");
        if (!r2.ok) { fail(".veg save of a loaded .veg fails", r2.error); return; }
        if (!loadProjectFile(base + "_2.veg", q2, &e)) { fail(".veg written twice does not load", e); return; }
        const std::string a = json(q1), b = json(q2);
        if (a != b) fail(".veg saved and loaded twice differs", where(a, b));
        ++c.saved;
    }
}

int gNextSample = -1;                           // --all-samples: the sample the next stepSample opens (-1: one at random)

void stepSample(Context& c)
{
    if (c.samples.empty()) return;
    const QString path = gNextSample >= 0 ? c.samples[(size_t)gNextSample % c.samples.size()] : gRng->pick(c.samples);
    gReset = true;
    logStep("open sample " + path.toStdString());
    if (st().dirty()) st().newProject();
    if (!gW->openProject(path)) { fail("a sample project does not open", path.toStdString()); return; }
    ++c.opened;
    uitest::settle(30);
    // saved with no change: the very same bytes (VEGAS keeps the project's own path in the file, so the bytes are made for the path the file has in it)
    {
        QFile a(path);
        const QByteArray raw = a.open(QIODevice::ReadOnly) ? a.readAll() : QByteArray();
        std::string savedAt;
        veg::ChunkTree tree;
        veg::readChunkTree(veg::Bytes((const uint8_t*)raw.constData(), (size_t)raw.size()), tree);
        for (const auto& k : tree.root.kids)
            if (!k.isList && k.id == veg::G::Header) savedAt = k.data.textZ(k.data.u32(0));
        std::vector<std::string> warnings;
        const std::string bytes = writeVegasData(st().project(), savedAt, &warnings);
        if (bytes.empty()) fail("an opened sample does not save", path.toStdString());
        else if (raw != QByteArray(bytes.data(), (int)bytes.size()))
            fail("an opened sample saved with no change is not the same file", path.toStdString());
    }
    checkSaveLoad(c, QFileInfo(path).completeBaseName().toStdString());
    // and the saved .veg opened in the window
    const QString again = gTmp + "/" + QFileInfo(path).completeBaseName() + "_1.veg";
    if (QFileInfo::exists(again)) {
        logStep("reopen saved " + again.toStdString());
        if (!gW->openProject(again)) fail("a saved sample does not open again", again.toStdString());
    }
}

void stepNewDemo(Context&)
{
    gReset = true;
    logStep("new demo project");
    uitest::buildDemoProject();
}

void stepFxWindow(Context&)
{
    Rng& r = *gRng;
    std::vector<const Event*> evs = allEvents();
    FxTarget t;
    const int k = r.below(4);
    if (k == 0 && !evs.empty()) { t.kind = FxTarget::Event; t.id = r.pick(evs)->id; }
    else if (k == 1 && !st().project().tracks.empty()) { t.kind = FxTarget::Track; t.id = st().project().tracks[(size_t)r.below((int)st().project().tracks.size())].id; }
    else if (k == 2) t.kind = FxTarget::VideoOut;
    else t.kind = FxTarget::AudioOut;
    logStep(strfmt("effects window for target %d %llu", (int)t.kind, (unsigned long long)t.id));
    // the Plug-In Chooser comes up first on an empty chain: pick something now and then
    PluginChooser::onShown = [](PluginChooser* ch) {
        PluginChooser::onShown = nullptr;
        if (gRng->chance(0.5)) {
            ch->openFolder("All");
            ch->addByLabel(gRng->chance(0.5) ? "Test Invert" : "Test Parameters");
            if (QPushButton* ok = ch->findChild<QPushButton*>("chooserOk")) { ok->click(); return; }
        }
        ch->reject();
    };
    gW->openFx(t, 0);
    PluginChooser::onShown = nullptr;
    uitest::settle(20);
    if (r.chance(0.5)) gW->fxWindow()->close();
}

void stepDock(Context&)
{
    Rng& r = *gRng;
    QList<QDockWidget*> docks = gW->findChildren<QDockWidget*>();
    if (docks.isEmpty()) return;
    QDockWidget* d = docks[r.below(docks.size())];
    logStep("dock " + d->objectName().toStdString() + (d->isVisible() ? " hide" : " show"));
    d->setVisible(!d->isVisible());
    uitest::settle(10);
}

// One of AppState's editing commands (what menus and keys call), with awkward values now and then: times on event edges and frame
// boundaries, zero and negative lengths, the first and last take, ids of things that are gone.
double edgyTime(Rng& r)
{
    std::vector<const Event*> evs = allEvents();
    const double len = std::max(1.0, st().project().length());
    switch (r.below(5)) {
        case 0: return 0.0;
        case 1: return len;
        case 2: if (!evs.empty()) { const Event* e = r.pick(evs); return r.chance(0.5) ? e->start : e->end(); } return 0.0;
        case 3: return st().project().quantize(r.range(0, len));
        default: return r.range(-0.5, len + 1.0);
    }
}

int gForceOp = -1;                              // --every: the model op to run (-1: one at random)

void stepModel(Context&)
{
    Rng& r = *gRng;
    AppState& app = st();
    std::vector<const Event*> evs = allEvents();
    std::set<uint64_t> sel;
    for (const Event* e : evs)
        if (r.chance(0.4)) sel.insert(e->id);
    const uint64_t someEvent = evs.empty() ? 0 : (r.chance(0.05) ? 999999 : r.pick(evs)->id);
    const std::vector<Track>& tracks = app.project().tracks;
    const uint64_t someTrack = tracks.empty() ? 0 : (r.chance(0.05) ? 999998 : tracks[(size_t)r.below((int)tracks.size())].id);
    std::vector<uint64_t> media;
    for (const MediaItem& m : app.project().media) media.push_back(m.id);
    const int op = gForceOp >= 0 ? gForceOp : r.below(24);
    logStep(strfmt("model op %d on %d selected, event %llu, track %llu", op, (int)sel.size(), (unsigned long long)someEvent, (unsigned long long)someTrack));
    app.setSelection(sel);
    switch (op) {
        case 0: app.setCursor(edgyTime(r)); app.splitAtCursor(); break;
        case 1: app.deleteSelection(); break;
        case 2: app.copySelection(); app.setCursor(edgyTime(r)); app.pasteAtCursor(); break;
        case 3: app.cutSelection(); app.setCursor(edgyTime(r)); app.pasteAtCursor(); break;
        case 4: app.groupSelection(r.chance(0.5)); break;
        case 5: app.setLocked(sel, r.chance(0.5)); break;
        case 6: app.stepTakes(sel, r.chance(0.5) ? 1 : -1); break;
        case 7: app.chooseTake(sel, r.below(4) - 1); break;
        case 8: if (!media.empty() && someEvent) app.addTakes(someEvent, {r.pick(media), r.pick(media)}); break;
        case 9: if (someEvent) { if (r.chance(0.5)) app.deleteActiveTake(someEvent); else app.deleteTakes(someEvent, {r.below(3), r.below(3)}); } break;
        case 10: app.copySelection(); app.pasteEventAttributes(sel, r.chance(0.5) ? AppState::AttrAll : 1 + r.below(511)); break;
        case 11: app.healEvents(sel); break;
        case 12: app.closeGaps(sel); break;
        case 13: app.insertTime(edgyTime(r), r.chance(0.2) ? 0.0 : r.range(-1.0, 5.0)); break;
        case 14: if (someTrack) app.insertEmptyEvent(someTrack, edgyTime(r), r.chance(0.2) ? 0.0 : r.range(-1.0, 4.0)); break;
        case 15: app.cleanProjectMedia(); break;
        case 16: if (someTrack) app.duplicateTrack(someTrack); break;
        case 17: if (someTrack && r.chance(0.3)) app.removeTrack(someTrack); else app.addTrack(r.chance(0.5) ? TrackType::Video : TrackType::Audio, r.below((int)tracks.size() + 2) - 1); break;
        case 18: app.setNormalize(sel, r.chance(0.5)); break;
        case 19: {
            FxTarget t;
            t.kind = someEvent && r.chance(0.5) ? FxTarget::Event : FxTarget::Track;
            t.id = t.kind == FxTarget::Event ? someEvent : someTrack;
            const FxChain* c = app.chainOf(app.project(), t);
            if (c && !c->empty()) {
                const uint64_t slot = (*c)[(size_t)r.below((int)c->size())].id;
                switch (r.below(3)) {
                    case 0: app.moveFx(t, slot, r.below(5) - 2); break;
                    case 1: app.setFxBypass(t, slot, r.chance(0.5)); break;
                    default: app.removeFx(t, slot); break;
                }
            } else if (t.id) app.addVideoFx(t, "reno.test.invert", "Test Invert");
            break;
        }
        case 20: if (someEvent) app.setTransition(someEvent, r.chance(0.5) ? std::string() : std::string("reno.test.transition"), "Test Transition"); break;
        case 21: {
            EnvelopeRef ref;
            ref.owner.kind = someEvent && r.chance(0.4) ? EnvelopeOwner::Event : EnvelopeOwner::Track;
            ref.owner.id = ref.owner.kind == EnvelopeOwner::Event ? someEvent : someTrack;
            static const char* kinds[] = {"volume", "pan", "mute", "composite", "fadecolor", "velocity", "transition"};
            ref.kind = kinds[r.below(7)];
            if (r.chance(0.6)) app.toggleEnvelope(ref);
            else app.removeEnvelope(ref);
            break;
        }
        case 22: app.setAutoCrossfades(r.chance(0.5)); break;
        default: if (r.chance(0.5)) app.undoAll(); else app.clearEditHistory(); break;
    }
}

// A click, double-click, drag or right-click in one of the window's other parts (Project Media, mixer, preview, Master Bus, effects
// browsers, transport...). Sent through the window system like real input: Qt finds the widget under the mouse for each event (a
// click can delete its widget) and makes the double-clicks and context menu events itself.
void windowMouse(QWindow* win, QPoint at, QEvent::Type type, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
{
    if (!win) return;
    QWindowSystemInterface::handleMouseEvent<QWindowSystemInterface::SynchronousDelivery>(win, QPointF(at), QPointF(win->mapToGlobal(at)), buttons, button,
                                                                                          type, mods);
    QCoreApplication::processEvents();
}

void stepWidget(Context&)
{
    Rng& r = *gRng;
    std::vector<QWidget*> leaves;
    for (QWidget* w : gW->findChildren<QWidget*>())
        if (w->isVisible() && w->isEnabled() && w->width() > 2 && w->height() > 2 && w != gW->timeline() && !gW->timeline()->isAncestorOf(w) &&
            w->findChildren<QWidget*>(Qt::FindDirectChildrenOnly).isEmpty() && w->window()->windowHandle())
            leaves.push_back(w);
    if (leaves.empty()) return;
    QWidget* w = r.pick(leaves);
    const QPoint a(r.below(w->width()), r.below(w->height())), b(r.below(w->width()), r.below(w->height()));
    const QPointer<QWindow> win(w->window()->windowHandle());
    const QPoint wa = w->mapTo(w->window(), a), wb = w->mapTo(w->window(), b);
    const int kind = r.below(5);
    const Qt::KeyboardModifiers mods = r.chance(0.3) ? randomMods() : Qt::KeyboardModifiers(Qt::NoModifier);
    logStep(strfmt("widget %s \"%s\" %d at (%d,%d)", w->metaObject()->className(), w->objectName().toUtf8().constData(), kind, a.x(), a.y()));
    if (kind == 0 || kind == 4 || kind == 1) {
        const int clicks = kind == 1 ? 2 : 1;
        for (int k = 0; k < clicks; ++k) {
            windowMouse(win, wa, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, mods);
            windowMouse(win, wa, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, mods);
        }
    } else if (kind == 2) {
        windowMouse(win, wa, QEvent::MouseButtonPress, Qt::LeftButton, Qt::LeftButton, mods);
        for (int k = 1; k <= 4; ++k) windowMouse(win, wa + (wb - wa) * k / 4, QEvent::MouseMove, Qt::NoButton, Qt::LeftButton, mods);
        windowMouse(win, wb, QEvent::MouseButtonRelease, Qt::LeftButton, Qt::NoButton, mods);
    } else {
        windowMouse(win, wa, QEvent::MouseButtonPress, Qt::RightButton, Qt::RightButton, mods);
        windowMouse(win, wa, QEvent::MouseButtonRelease, Qt::RightButton, Qt::NoButton, mods);
    }
}

// ------------------------------------------------------------------------------------------ stand-in media
// as load_lag_test: a copy of a sample's folder in which every file its projects use is a stand-in of the same kind
std::string baseNameOf(const std::string& p)
{
    const size_t k = p.find_last_of("\\/");
    return k == std::string::npos ? p : p.substr(k + 1);
}

std::string pickStandin(const MediaItem& m)
{
    const std::string low = toLower(m.path);
    auto ends = [&](const char* e) { return endsWithNoCase(low, e); };
    if (ends(".png") || ends(".jpg") || ends(".jpeg") || ends(".bmp") || ends(".gif") || ends(".tif") || m.info.isStill) return "still.png";
    if (ends(".wav") || ends(".mp3") || ends(".ogg") || ends(".flac") || ends(".m4a") || ends(".aac") || (m.info.hasAudio && !m.info.hasVideo))
        return "tone.wav";
    if (m.info.fps > 45) return "clip1080.mp4";
    if (m.info.height > 0 && m.info.height <= 480) return "clip480.mp4";
    return "clip720.mp4";
}

QString gRealRoot;                              // --real: a folder laid out as the samples' with some of their real media

// Copies every .veg of 'srcDir' to 'dest' with a stand-in under the name of every file they use (a nested project from another folder
// of the samples is copied in too). Returns the copies of srcDir's own projects.
std::vector<QString> makeStandinFolder(const QString& srcDir, const QString& samplesRoot, const QString& media, const QString& dest)
{
    std::vector<QString> mine;
    QDir().mkpath(dest);
    const QDir src(srcDir);
    std::vector<QString> todo;
    for (const QFileInfo& f : src.entryInfoList(QStringList() << "*.veg", QDir::Files)) {
        const QString to = dest + "/" + f.fileName();
        if (!QFileInfo::exists(to)) QFile::copy(f.absoluteFilePath(), to);
        QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
        mine.push_back(to);
        todo.push_back(f.absoluteFilePath());
    }
    std::set<QString> done;
    while (!todo.empty()) {
        const QString orig = todo.back();
        todo.pop_back();
        if (!done.insert(orig).second) continue;
        Project p;
        if (!importVegasProject(orig.toStdString(), p).ok) continue;
        for (const MediaItem& m : p.media) {
            if (m.generated || m.path.empty()) continue;
            const std::string name = baseNameOf(m.path);
            if (name.empty()) continue;
            const QString to = dest + "/" + QString::fromStdString(name);
            if (endsWithNoCase(name, ".veg")) {
                QString from = src.filePath(QString::fromStdString(name));
                if (!QFileInfo::exists(from)) {
                    from.clear();
                    QDirIterator it(samplesRoot, QStringList() << QString::fromStdString(name), QDir::Files, QDirIterator::Subdirectories);
                    if (it.hasNext()) from = it.next();
                }
                if (from.isEmpty()) continue;
                if (!QFileInfo::exists(to)) {
                    QFile::copy(from, to);
                    QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
                }
                todo.push_back(from);
                continue;
            }
            if (QFileInfo::exists(to) || QFileInfo(to).isSymLink()) continue;
            // the real file where the folder has it, else a stand-in
            QString real = src.filePath(QString::fromStdString(name));
            if (!QFileInfo::exists(real) && !gRealRoot.isEmpty())
                real = QDir(gRealRoot + "/" + QDir(samplesRoot).relativeFilePath(srcDir)).filePath(QString::fromStdString(name));
            QFile::link(QFileInfo::exists(real) ? QFileInfo(real).absoluteFilePath() : media + "/" + QString::fromStdString(pickStandin(m)), to);
        }
    }
    return mine;
}

// ------------------------------------------------------------------------------------------ stale pictures (--stale)
bool gStaleCheck = false;
int gStaleChecks = 0, gStaleSaved = 0;
std::map<std::string, int> gStaleKinds;         // "<what> after <step kind>" -> times

// Lets the window catch up: the preview's picture, the timeline's thumbnails and waveforms (until nothing new has come for a while, or maxMs).
void quiet(int maxMs)
{
    Playback* pb = gW->playback();
    QElapsedTimer t;
    t.start();
    uint64_t frames = pb->framesShown();
    int still = 0;
    while (t.elapsed() < maxMs) {
        uitest::settle(40);
        progress();
        const uint64_t f = pb->framesShown();
        if (f == frames) {
            if (++still >= 6) break;
        } else {
            still = 0;
            frames = f;
        }
    }
}

// How many 8 x 8 blocks have 12 or more pixels that differ by more than 'tol' in a channel (as timeline_perf_test's stale-screen
// check: a line clipped differently can come out a pixel apart, a patch is something left behind). -1: sizes differ.
int patchesBetween(const QImage& a0, const QImage& b0, int tol, QRect* where = nullptr)
{
    if (a0.size() != b0.size() || a0.isNull()) return -1;
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32), b = b0.convertToFormat(QImage::Format_ARGB32);
    const int bw = (a.width() + 7) / 8, bh = (a.height() + 7) / 8;
    std::vector<int> blocks((size_t)bw * bh, 0);
    QRect box;
    for (int y = 0; y < a.height(); ++y) {
        const QRgb* p = (const QRgb*)a.constScanLine(y);
        const QRgb* q = (const QRgb*)b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x)
            if (std::abs(qRed(p[x]) - qRed(q[x])) > tol || std::abs(qGreen(p[x]) - qGreen(q[x])) > tol || std::abs(qBlue(p[x]) - qBlue(q[x])) > tol)
                ++blocks[(size_t)(y / 8) * bw + x / 8];
    }
    int n = 0;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x)
            if (blocks[(size_t)y * bw + x] >= 12) {
                ++n;
                box |= QRect(x * 8, y * 8, 8, 8);
            }
    if (where) *where = box;
    return n;
}

void staleFound(const std::string& what, const QImage& shown, const QImage& fresh, int patches, const QRect& box)
{
    const std::string kind = gStep.substr(0, gStep.find(' '));
    ++gStaleKinds[what + " after " + kind];
    std::string pics;
    if (gStaleSaved < 60 && !shown.isNull()) {
        ++gStaleSaved;
        const QString base = gOut + QString("/stale_%1").arg(gStaleSaved);
        shown.save(base + "_shown.png");
        fresh.save(base + "_fresh.png");
        pics = strfmt(" (stale_%d_shown.png / _fresh.png)", gStaleSaved);
    }
    fail(what + " is not what it would be drawn afresh", strfmt("%d patches in %d,%d %dx%d", patches, box.x(), box.y(), box.width(), box.height()) + pics);
}

// The text of every list and table in a widget (the item views' models, in order).
QString viewsText(QWidget* w)
{
    QString s;
    for (QAbstractItemView* v : w->findChildren<QAbstractItemView*>()) {
        QAbstractItemModel* m = v->model();
        if (!m || !v->isVisibleTo(w)) continue;
        std::function<void(const QModelIndex&, int)> walk = [&](const QModelIndex& parent, int depth) {
            for (int r = 0; r < m->rowCount(parent) && s.size() < 400000; ++r) {
                for (int c = 0; c < m->columnCount(parent); ++c) s += m->data(m->index(r, c, parent)).toString() + "\t";
                s += "\n";
                if (depth < 4) walk(m->index(r, 0, parent), depth + 1);
            }
        };
        walk(QModelIndex(), 0);
        s += "|\n";
    }
    return s;
}

// Puts a new panel beside the old one: same size and same choices in its combo boxes.
void likeOld(QWidget* fresh, QWidget* old)
{
    fresh->setAttribute(Qt::WA_DontShowOnScreen);
    fresh->resize(old->size());
    fresh->show();
    QList<QComboBox*> a = old->findChildren<QComboBox*>(), b = fresh->findChildren<QComboBox*>();
    for (int k = 0; k < a.size() && k < b.size(); ++k) b[k]->setCurrentIndex(a[k]->currentIndex());
}

void comparePanelText(const char* name, QWidget* old, QWidget* fresh)
{
    likeOld(fresh, old);
    uitest::settle(60);
    const QString x = viewsText(old), y = viewsText(fresh);
    if (x != y) {
        const std::string a = x.toStdString(), b = y.toStdString();
        ++gStaleKinds[std::string(name) + " after " + gStep.substr(0, gStep.find(' '))];
        fail(std::string(name) + " does not show what a new one shows", where(a, b));
    }
}

// The widget in one of the window's docks (by the dock's name).
QWidget* dockWidget(const char* name)
{
    // the main window puts each in a page of its own, with the tab strip under it: the first thing in the page
    QDockWidget* d = gW->findChild<QDockWidget*>(name);
    QWidget* page = d ? d->widget() : nullptr;
    return page && page->layout() && page->layout()->count() ? page->layout()->itemAt(0)->widget() : nullptr;
}

QElapsedTimer startedTimer()
{
    QElapsedTimer t;
    t.start();
    return t;
}

void staleCheck()
{
    if (!gStaleCheck || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
    ++gStaleChecks;
    Playback* pb = gW->playback();
    const bool wasPlaying = pb->playing();
    if (wasPlaying) pb->pause();
    quiet(5000);
    TimelineWidget* tl = gW->timeline();
    // 1. the timeline on the screen (after all its partial repaints) against a whole repaint of it
    if (tl->isVisible()) {
        QBackingStore* bs = gW->backingStore();
        QPaintDevice* pd = bs ? bs->paintDevice() : nullptr;
        if (pd && pd->devType() == QInternal::Image) {
            QImage fresh, shown;
            QRect box;
            int n = 0;
            for (QElapsedTimer t = startedTimer(); t.elapsed() < 3000; uitest::settle(100)) {
                fresh = tl->grab().toImage();
                const QPoint at = tl->mapTo(gW, QPoint(0, 0)) * fresh.devicePixelRatio();
                shown = static_cast<QImage*>(pd)->copy(QRect(at, fresh.size()));
                n = patchesBetween(shown, fresh, 40, &box);
                if (n == 0) break;
            }
            if (n > 0) staleFound("the timeline on the screen", shown, fresh, n, box);
        }
    }
    // 2. the timeline against a new one of the same size and view (its own pictures, read afresh)
    if (tl->isVisible() && tl->width() > 400) {
        TimelineWidget fresh(pb);
        fresh.setAttribute(Qt::WA_DontShowOnScreen);
        fresh.options() = tl->options();
        fresh.optionsChanged();
        fresh.setHeaderWidth(tl->headerWidth());
        fresh.setSelectedTracks(tl->selectedTracks());
        fresh.resize(tl->size());
        fresh.show();
        for (TimelineWidget* t : {tl, &fresh}) {
            t->zoomToFit();
            if (QScrollBar* v = t->findChild<QScrollBar*>("timelineVScroll")) v->setValue(0);
        }
        quiet(5000);
        QImage a, b;
        QRect box;
        int n = 0;
        // the new one's pictures and waveforms come in a while; the old one's may be on their way too
        for (QElapsedTimer t = startedTimer(); t.elapsed() < 12000; uitest::settle(200)) {
            a = tl->grab().toImage();
            b = fresh.grab().toImage();
            n = patchesBetween(a, b, 40, &box);
            if (n == 0) break;
        }
        if (n > 0) staleFound("the timeline", a, b, n, box);
        fresh.hide();
    }
    // 3. the preview's picture against one rendered now (not after playing: pause() asks for a new one itself)
    if (!wasPlaying) {
        const QImage fresh = pb->renderNow(st().cursor());
        QImage shown;
        QRect box;
        int n = 0;
        // a picture can take a while under the sanitizers: what counts is that the right one comes
        for (QElapsedTimer t = startedTimer(); t.elapsed() < 10000; uitest::settle(150)) {
            shown = pb->lastFrame();
            if (shown.isNull() || fresh.isNull() || shown.size() != fresh.size()) {
                n = (!shown.isNull() && !fresh.isNull()) ? -1 : 0;
                continue;
            }
            n = patchesBetween(shown, fresh, 24, &box);
            if (n == 0) break;
        }
        if (n > 0) staleFound("the preview", shown, fresh, n, box);
        else if (n < 0) staleFound("the preview (its size)", shown, fresh, 0, QRect());
    }
    // 4. Edit Details and Project Media (their lists), the mixer (its picture), against new ones
    if (EditDetails* ed = static_cast<EditDetails*>(dockWidget("dockDetails")); ed && ed->isVisible()) {
        EditDetails fresh;
        comparePanelText("Edit Details", ed, &fresh);
    }
    if (MediaBin* mb = static_cast<MediaBin*>(dockWidget("dockMedia")); mb && mb->isVisible()) {
        MediaBin fresh;
        fresh.setView(mb->view());
        fresh.showBin(mb->currentBin());
        comparePanelText("Project Media", mb, &fresh);
    }
    if (MixerWindow* mx = static_cast<MixerWindow*>(dockWidget("dockMixer")); mx && mx->isVisible() && mx->width() > 50) {
        MixerWindow fresh(pb);
        likeOld(&fresh, mx);
        uitest::settle(100);
        const QImage a = mx->grab().toImage(), b = fresh.grab().toImage();
        QRect box;
        const int n = patchesBetween(a, b, 40, &box);
        if (n > 0) staleFound("the mixer", a, b, n, box);
    }
}

// ------------------------------------------------------------------------------------------ one run
// After a step: the invariants, and the undo / redo check for the steps it made.
void afterStep(Context& c, const std::string& before, size_t depthBefore, bool undoStep)
{
    const Project& p = st().project();
    if (const std::string pr = problems(p); !pr.empty()) fail("broken project", pr);
    const size_t d = st().undoCount();
    const std::string now = json(p);
    // a new project (File > New, Open..., or this test's own): earlier steps are gone, so nothing to compare
    // (a project not changed since it was opened or saved is taken to be one)
    if (gReset || (!undoStep && !st().dirty())) {
        gReset = false;
        c.history.assign(d + 1, std::string());
        c.history[d] = now;
        return;
    }
    if (!undoStep && d > depthBefore && d - depthBefore <= 4 && gRng->chance(0.5)) {
        // steps undone one by one give back the previous project; redone, the next
        const size_t n = d - depthBefore;
        for (size_t k = 0; k < n; ++k) st().undo();
        const std::string back = json(st().project());
        if (back != before) fail("undo does not give back the project as it was before the step", gStep + ": " + where(back, before));
        for (size_t k = 0; k < n; ++k) st().redo();
        const std::string again = json(st().project());
        if (again != now) fail("redo does not give back the project as it was after the step", gStep + ": " + where(again, now));
    } else if (!undoStep && d == depthBefore && now != before && st().redoCount() == 0) {
        ++gNoUndo[gStep.substr(0, gStep.find(" ("))];
    }
    remember(c, depthBefore, undoStep);
}

void collectCommands(Context& c);
int runSteps(Context& c, uint64_t seed, int steps);

int runSeed(uint64_t seed, int steps, const std::vector<QString>& samples)
{
    Rng rng(seed);
    gRng = &rng;
    Context c;
    c.samples = samples;
    collectCommands(c);
    std::printf("seed %llu: %d commands, %d keys, %d samples\n", (unsigned long long)seed, (int)c.actions.size(), (int)c.keys.size(), (int)c.samples.size());
    std::fflush(stdout);
    return runSteps(c, seed, steps);
}

void collectCommands(Context& c)
{
    c.keys = shortcutKeys();
    for (QAction* a : gW->findChildren<QAction*>())
        if (!a->isSeparator() && !a->menu() && !denied(a)) c.actions.push_back(a);
    // (the menus' own actions too, for those made by the menus)
    for (QMenu* m : gW->findChildren<QMenu*>())
        for (QAction* a : m->actions())
            if (!a->isSeparator() && !a->menu() && !denied(a) && std::find(c.actions.begin(), c.actions.end(), a) == c.actions.end()) c.actions.push_back(a);
    // keys that end or leave the program are not pressed
    c.keys.erase(std::remove_if(c.keys.begin(), c.keys.end(), [](const std::vector<KeyPress>& s) {
        const std::string& n = s[0].name;
        return n.find("Exit") != std::string::npos || n.find("Nested") != std::string::npos || n.find("Audio Editor") != std::string::npos ||
               n.find("Open in") != std::string::npos || n.find("Help") != std::string::npos || n.find("Parent") != std::string::npos ||
               n.find("Look for") != std::string::npos;
    }), c.keys.end());
}

int runSteps(Context& c, uint64_t seed, int steps)
{
    Rng& rng = *gRng;
    const QString logPath = gOut + QString("/monkey_steps_%1.log").arg(seed);
    gLog = std::fopen(logPath.toUtf8().constData(), "w");
    st().newProject();
    st().clearEditHistory();
    uitest::buildDemoProject();
    c.history.clear();
    remember(c, 0, false);
    // what each kind of step weighs
    struct Kind { int weight; void (*fn)(Context&); bool undo; };
    const std::vector<Kind> kinds = {
        {30, stepAction, false}, {22, stepKeys, false}, {30, stepMouse, false}, {8, stepUndoRedo, true}, {5, stepPlayback, false},
        {1, stepResize, false}, {5, stepMedia, false}, {6, stepSelect, false}, {3, stepSample, false},
        {1, stepNewDemo, false}, {3, stepFxWindow, false}, {2, stepDock, false}, {6, stepWidget, false}, {10, stepModel, false},
    };
    int total = 0;
    for (const Kind& k : kinds) total += k.weight;
    // --all-samples: each of the user's projects opened in turn (and checked), with a few random steps on each, first
    const int sampleRounds = gAllSamples ? (int)c.samples.size() : 0;
    for (c.step = 0; c.step < steps + sampleRounds * 21; ++c.step) {
        if (c.step < sampleRounds * 21 && c.step % 21 == 0) {
            gNextSample = c.step / 21;
            const std::string before = json(st().project());
            const size_t depth = st().undoCount();
            if (gLog) std::fprintf(gLog, "[%d] ", c.step);
            stepSample(c);
            gNextSample = -1;
            afterStep(c, before, depth, false);
            continue;
        }
        int w = rng.below(total);
        const Kind* kind = &kinds[0];
        for (const Kind& k : kinds) {
            if (w < k.weight) { kind = &k; break; }
            w -= k.weight;
        }
        // an empty project gets things to work on now and then
        if (allEvents().size() < 2 && rng.chance(0.5)) kind = &kinds[7];
        const std::string before = json(st().project());
        const size_t depth = st().undoCount();
        if (gLog) std::fprintf(gLog, "[%d] ", c.step);
        kind->fn(c);
        // modal dialogs opened by the step are handled by the timer while it runs; let queued work happen
        QCoreApplication::processEvents();
        if (st().dragging()) {
            fail("a drag is left open after the step", gStep);
            st().cancelDrag();
        }
        afterStep(c, before, depth, kind->undo);
        staleCheck();
        if (rng.chance(0.01)) checkSaveLoad(c, strfmt("seed%llu_step%d", (unsigned long long)seed, c.step));
        if (c.step % 250 == 0) {
            std::printf("  step %d: %d events, %d dialogs, %d menus\n", c.step, (int)allEvents().size(), gDialogs, gMenus);
            std::fflush(stdout);
        }
    }
    gW->playback()->pause();
    checkSaveLoad(c, strfmt("seed%llu_end", (unsigned long long)seed));
    std::printf("seed %llu done: %d samples opened, %d save/load checks, %d undone\n", (unsigned long long)seed, c.opened, c.saved, c.undone);
    if (gLog) std::fclose(gLog);
    gLog = nullptr;
    return 0;
}

// ------------------------------------------------------------------------------------------ --every
// The same start for every command: about half the events selected (every other one), the cursor in the middle of the first
// selected one, a time selection over it, its track current; the view fitted.
void prepareForCommand()
{
    std::vector<const Event*> evs = allEvents();
    std::set<uint64_t> sel;
    const Event* first = nullptr;
    for (size_t k = 0; k < evs.size(); k += 2) {
        sel.insert(evs[k]->id);
        if (!first) first = evs[k];
    }
    st().setSelection(sel);
    if (first) {
        st().setCursor(st().project().quantize(first->start + first->length / 2));
        st().setLoop(first->start, first->end());
        if (const Track* t = trackOf(first->id)) st().currentTrack = t->id;
    }
    st().notify(kSelection);
    gW->timeline()->exitEditMode();  // an edge or edit selected for trimming belongs to the view, not undo
    // same for the tracks' minimized and scaled heights, and a box typed into in place (time display, a name) left open by the
    // previous command: the same start for every command
    gW->timeline()->resetTrackHeights();
    for (QLineEdit* le : gW->timeline()->findChildren<QLineEdit*>())
        if (le->isVisible()) {
            QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QCoreApplication::sendEvent(le, &esc);
            le->hide();
        }
    gW->timeline()->zoomToFit();
    gW->timeline()->setFocus();
}

// One command of the sweep: prepared, run, checked, and undone (or the project reopened if it can't be undone).
void everyStep(Context& c, const std::string& name, const std::function<void()>& run, const std::function<void()>& reopen)
{
    prepareForCommand();
    quiet(1000);
    const std::string before = json(st().project());
    const size_t depth = st().undoCount();
    const std::string path = st().path();
    const uint64_t revision = st().revision();
    const std::set<uint64_t> selection = st().selection();
    const double cursor = st().cursor();
    QElapsedTimer took = startedTimer();
    if (gLog) std::fprintf(gLog, "[%d] ", c.step);
    logStep(name);
    run();
    QCoreApplication::processEvents();
    if (st().dragging()) {
        fail("a drag is left open after the step", gStep);
        st().cancelDrag();
    }
    if (gW->fxWindow() && gW->fxWindow()->isVisible()) gW->fxWindow()->close();
    if (const std::string pr = problems(st().project()); !pr.empty()) fail("broken project", pr);
    // windows are checked after a command that changed the project, selection or cursor; after the rest (opening a window,
    // changing the view) every tenth time
    const bool changed = st().revision() != revision || st().selection() != selection || st().cursor() != cursor || st().path() != path;
    if (changed || c.step % 10 == 0) staleCheck();
    ++c.step;
    // back to where it started
    if (st().path() == path && st().undoCount() > depth) {
        logStep(name + " undone");
        while (st().undoCount() > depth && st().canUndo()) st().undo();
        if (json(st().project()) == before) {
            staleCheck();  // undo is an edit too
            if (gLog) std::fprintf(gLog, "  (%lld ms)\n", (long long)took.elapsed());
            return;
        }
    }
    if (st().path() != path || json(st().project()) != before) {
        logStep(name + ": the project opened again");
        reopen();
    }
    if (gLog) std::fprintf(gLog, "  (%lld ms)\n", (long long)took.elapsed());
}

QStringList gEveryNames;                        // --every-sample: which samples (part of their names)
int gEveryFrom = 0;                             // --every-from: the first command (by its place in the sorted list)
bool gEveryNoDemo = false;                      // --every-no-demo: the samples only

int runEvery(const std::vector<QString>& samples, int sampleCount)
{
    Rng rng(1);
    gRng = &rng;
    Context c;
    collectCommands(c);
    std::sort(c.actions.begin(), c.actions.end(), [](QAction* a, QAction* b) { return a->objectName() + a->text() < b->objectName() + b->text(); });
    gLog = std::fopen((gOut + "/monkey_every.log").toUtf8().constData(), "w");
    // the projects: the demo project, then samples that have events (spread over the list)
    std::vector<QString> projects;
    if (!gEveryNoDemo) projects.push_back(QString());
    for (const QString& want : gEveryNames)
        for (const QString& f : samples)
            if (QFileInfo(f).fileName().contains(want, Qt::CaseInsensitive) && (int)projects.size() < sampleCount + (gEveryNoDemo ? 0 : 1)) {
                projects.push_back(f);
                break;
            }
    for (int k = 0; (int)projects.size() < sampleCount + (gEveryNoDemo ? 0 : 1) && k < sampleCount && !samples.empty(); ++k)
        projects.push_back(samples[(size_t)(k * (int)samples.size() / std::max(1, sampleCount)) % samples.size()]);
    std::printf("every: %d commands, %d keys, 24 model commands, on %d projects\n", (int)c.actions.size(), (int)c.keys.size(), (int)projects.size());
    std::fflush(stdout);
    for (const QString& proj : projects) {
        auto reopen = [&] {
            gW->playback()->pause();
            st().newProject();
            if (proj.isEmpty()) uitest::buildDemoProject();
            else if (!gW->openProject(proj)) fail("a sample project does not open", proj.toStdString());
            st().clearEditHistory();
            uitest::settle(50);
        };
        reopen();
        std::printf("every: project %s: %d events\n", proj.isEmpty() ? "(demo)" : qPrintable(QFileInfo(proj).fileName()), (int)allEvents().size());
        std::fflush(stdout);
        int index = 0;
        for (QAction* a : c.actions) {
            if (index++ < gEveryFrom && proj == projects.front()) continue;  // --every-from: carrying on a run
            if (!a || !a->isEnabled()) continue;
            QPointer<QAction> ap(a);
            everyStep(c, strfmt("action %s \"%s\"", a->objectName().toUtf8().constData(), a->text().toUtf8().constData()), [ap] { if (ap) ap->trigger(); }, reopen);
        }
        for (const std::vector<KeyPress>& seq : c.keys)
            everyStep(c, "keys " + seq[0].name, [&seq] { for (const KeyPress& k : seq) pressKey(k.key, k.mods); }, reopen);
        for (int op = 0; op < 24; ++op) {
            gForceOp = op;
            everyStep(c, strfmt("model op %d", op), [&c] { stepModel(c); }, reopen);
            gForceOp = -1;
        }
        std::printf("every: project done, %d steps so far, %d stale checks\n", c.step, gStaleChecks);
        std::fflush(stdout);
    }
    if (gLog) std::fclose(gLog);
    gLog = nullptr;
    return 0;
}

}  // namespace

int main(int argc, char** argv)
{
    // restarted by "Open Nested Timeline" or "Open Parent" with a project to open: not this test's business
    if (argc > 1 && (QString(argv[1]).endsWith(".veg") || QString(argv[1]).endsWith(".reno"))) return 0;
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMonkeyTest");
    QApplication::setApplicationName("monkey_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("monkey");
    QDir().mkpath(gOut);
    std::vector<std::string> ofx;
    uint64_t seedA = 1, seedB = 1;
    int steps = 2000;
    QString samplesDir = qEnvironmentVariable("RENO_SAMPLES");
    QString standins;
    int every = -1;                                 // --every: how many samples besides the demo project (-1: random steps)
    for (int i = 2; i < args.size(); ++i) {
        if (args[i] == "--seed" && i + 1 < args.size()) seedA = seedB = args[++i].toULongLong();
        else if (args[i] == "--seeds" && i + 1 < args.size()) {
            const QStringList ab = args[++i].split('-');
            seedA = ab[0].toULongLong();
            seedB = ab.size() > 1 ? ab[1].toULongLong() : seedA;
        } else if (args[i] == "--steps" && i + 1 < args.size()) steps = args[++i].toInt();
        else if (args[i] == "--samples" && i + 1 < args.size()) samplesDir = args[++i];
        else if (args[i] == "--verbose") gVerbose = true;
        else if (args[i] == "--all-samples") gAllSamples = true;
        else if (args[i] == "--standins" && i + 1 < args.size()) standins = args[++i];
        else if (args[i] == "--stale") gStaleCheck = true;
        else if (args[i] == "--real" && i + 1 < args.size()) gRealRoot = args[++i];
        else if (args[i] == "--every-sample" && i + 1 < args.size()) gEveryNames << args[++i];
        else if (args[i] == "--every-from" && i + 1 < args.size()) gEveryFrom = args[++i].toInt();
        else if (args[i] == "--every-no-demo") gEveryNoDemo = true;
        else if (args[i] == "--every") {
            every = 2;
            bool number = false;
            if (i + 1 < args.size()) args[i + 1].toInt(&number);
            if (number) every = std::max(0, args[++i].toInt());
        }
        else if (args[i].endsWith(".ofx")) ofx.push_back(args[i].toStdString());
    }
    uitest::setup(ofx, std::vector<std::string>());
    st().prefs.undoLevels = 1000000;
    st().prefs.autoSaveMinutes = 0;

    // the user's projects, copied (their folder is only read)
    gTmp = QDir(gOut).absoluteFilePath("tmp");
    QDir(gTmp).removeRecursively();
    QDir().mkpath(gTmp + "/samples");
    std::vector<QString> samples;
    if (!QFileInfo(samplesDir).isDir()) std::printf("SKIP: no sample projects (set RENO_SAMPLES or pass --samples): the demo project only\n");
    QDirIterator it(samplesDir.isEmpty() ? QString("(RENO_SAMPLES not set)") : samplesDir, QStringList() << "*.veg", QDir::Files, QDirIterator::Subdirectories);
    int n = 0;
    if (!standins.isEmpty()) {
        // each sample folder copied with stand-ins for its media
        std::set<QString> dirs;
        while (it.hasNext()) dirs.insert(QFileInfo(it.next()).absolutePath());
        for (const QString& d : dirs) {
            const QString dest = gTmp + QString("/samples/%1_%2").arg(n++).arg(QFileInfo(d).fileName());
            for (const QString& f : makeStandinFolder(d, samplesDir, standins, dest)) samples.push_back(f);
        }
    }
    while (standins.isEmpty() && it.hasNext()) {
        const QString f = it.next();
        const QString to = gTmp + QString("/samples/%1_%2").arg(n++).arg(QFileInfo(f).fileName());
        if (QFile::copy(f, to)) {
            QFile::setPermissions(to, QFile::ReadOwner | QFile::WriteOwner);
            samples.push_back(to);
        }
    }
    std::sort(samples.begin(), samples.end());

    MainWindow w;
    gW = &w;
    w.resize(1400, 900);
    w.show();
    uitest::settle(300);
    // (prefs applied after the window read them)
    st().prefs.undoLevels = 1000000;
    st().prefs.autoSaveMinutes = 0;

    QTimer modal;
    QObject::connect(&modal, &QTimer::timeout, [] { lookAtModals(); progress(); });
    modal.start(20);

    // the watchdog: no progress for 3 minutes (the modal timer counts as progress, so this is a step stuck outside the event loop):
    // print the main thread's stack and end
    gMainThread = pthread_self();
    gClock.start();
    progress();
    std::thread dog([] {
        while (!gStopWatchdog) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (gClock.elapsed() - gLastProgress > 180000) {
                std::fprintf(stderr, "HANG: no progress for 3 minutes in step: %s\n", gStep.c_str());
                std::fflush(stderr);
                pthread_kill(gMainThread, SIGABRT);
                return;
            }
        }
    });

    if (every >= 0) runEvery(samples, every);
    else
        for (uint64_t s = seedA; s <= seedB; ++s) runSeed(s, steps, samples);
    modal.stop();
    gStopWatchdog = true;
    dog.join();
    std::printf("dialogs handled: %d, menus: %d\n", gDialogs, gMenus);
    if (!gNoUndo.empty()) {
        std::printf("steps that changed the project without an undo step (not failures; for review):\n");
        for (const auto& kv : gNoUndo) std::printf("  %4d  %s\n", kv.second, kv.first.c_str());
    }
    for (const auto& kv : gFailures) std::printf("failure kind \"%s\": %d times\n", kv.first.c_str(), kv.second);
    if (gStaleCheck) {
        std::printf("stale checks: %d\n", gStaleChecks);
        for (const auto& kv : gStaleKinds) std::printf("  stale %4d  %s\n", kv.second, kv.first.c_str());
    }
    check(true, "the monkey finished");
    // (closing: nothing to save)
    st().newProject();
    return uitest::summary();
}
