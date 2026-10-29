// Tools > Audio > Apply Non-Real-Time Event FX as VEGAS does it: the Plug-In Chooser, the Take FX window (settings and Preview),
// the Apply Non Real-Time FX window (name, folder, format, template, Render), the new files named "<media> Take <n>" becoming
// the events' active takes (old ones kept), one undo step, Cancel and failures leaving the project as it was, several events at
// once, the formats, and the file's sound being the event's sound through the effects.
//
//   nrt_test <output folder>
//
// Set HOME to a scratch folder. QT_QPA_PLATFORM=offscreen.
#include "../../src/app/audiotools.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/app/timeline.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMenuBar>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
std::string str(const QString& s) { return s.toStdString(); }
const Event* ev(uint64_t id) { return AppState::get().project().findEvent(id); }
const MediaItem* media(uint64_t id) { return AppState::get().project().findMedia(id); }
std::string num(double v) { return QString::number(v, 'g', 6).toStdString(); }

AudioPluginDesc builtinAudio(const std::string& uid)
{
    for (const AudioFxEntry& e : Catalog::get().audio())
        if (e.desc.uid == uid) return e.desc;
    AudioPluginDesc d;
    d.format = "BUILTIN";
    d.uid = uid;
    return d;
}

// ---- driving windows that run in their own event loops: a timer looks for the one showing and acts once
struct Driver {
    std::function<void(QDialog*)> take, apply;
    std::function<void(PluginChooser*)> chooser;
    QDialog* sawTake = nullptr;
    int takes = 0, applies = 0, choosers = 0;
    QTimer timer;
    Driver()
    {
        timer.setInterval(20);
        QObject::connect(&timer, &QTimer::timeout, [this] {
            QWidget* m = QApplication::activeModalWidget();
            QDialog* d = qobject_cast<QDialog*>(m);
            if (!d) return;
            if (d->objectName() == "nonRealTimeTakeFxDialog" && take) {
                ++takes;
                auto f = take;
                take = nullptr;
                f(d);
            } else if (d->objectName() == "nonRealTimeFxDialog" && apply) {
                ++applies;
                auto f = apply;
                apply = nullptr;
                f(d);
            }
        });
        timer.start();
    }
};
Driver* gDrv = nullptr;

void clickOk(QDialog* d)
{
    if (QDialogButtonBox* bb = d->findChild<QDialogButtonBox*>())
        if (QPushButton* ok = bb->button(QDialogButtonBox::Ok)) { ok->click(); return; }
    d->accept();
}
void clickRender(QDialog* d)
{
    if (QPushButton* r = d->findChild<QPushButton*>("nrtRender")) r->click();
    else d->accept();
}
void clickCancel(QDialog* d)
{
    if (QDialogButtonBox* bb = d->findChild<QDialogButtonBox*>())
        if (QPushButton* c = bb->button(QDialogButtonBox::Cancel)) { c->click(); return; }
    d->reject();
}

// The chooser takes Distortion (or the effect whose label contains 'what') and OK.
void chooseFx(const QStringList& what)
{
    PluginChooser::onShown = [what](PluginChooser* c) {
        PluginChooser::onShown = nullptr;
        ++gDrv->choosers;
        for (const QString& w : what)
            for (const QString& label : c->listed())
                if (label.contains(w, Qt::CaseInsensitive)) { c->addByLabel(label); break; }
        c->findChild<QPushButton*>("chooserOk")->click();
    };
}

// The menu command, as the user runs it, with the events selected.
std::vector<bool> gPreview;
void runCommand(const std::set<uint64_t>& sel)
{
    AppState& app = AppState::get();
    app.setSelection(sel);
    NrtHooks h;
    h.preview = [](bool on, double) { gPreview.push_back(on); };
    runNonRealTimeEventFx(nullptr, h);
    uitest::settle(50);
}

// The sound of an event's span as the live mix gives it (left and right), and the file's.
std::vector<float> mixOf(double secs)
{
    AppState& app = AppState::get();
    static MediaPool pool;
    pool.setOpener(app.mediaOpener);
    AudioEngine eng(pool, app.fx);
    const Project& p = app.project();
    const int rate = p.settings.sampleRate, n = (int)(secs * rate);
    std::vector<float> l((size_t)n), r((size_t)n);
    for (int at = 0; at < n; at += 2048) {
        const int k = std::min(2048, n - at);
        eng.render(p, (int64_t)at, k, l.data() + at, r.data() + at, true);
    }
    std::vector<float> out;
    out.reserve((size_t)n * 2);
    for (int k = 0; k < n; ++k) {
        out.push_back(l[(size_t)k]);
        out.push_back(r[(size_t)k]);
    }
    return out;
}
std::vector<float> fileSound(const std::string& path, double secs, int rate)
{
    std::string e;
    std::unique_ptr<MediaReader> r = openMedia(path, &e);
    const int n = (int)(secs * rate);
    std::vector<float> l((size_t)n), rr((size_t)n), out;
    if (!r || !r->audio(0, n, rate, l.data(), rr.data())) return out;
    for (int k = 0; k < n; ++k) {
        out.push_back(l[(size_t)k]);
        out.push_back(rr[(size_t)k]);
    }
    return out;
}
double maxDiff(const std::vector<float>& a, const std::vector<float>& b)
{
    if (a.size() != b.size() || a.empty()) return 1e9;
    double m = 0;
    for (size_t k = 0; k < a.size(); ++k) m = std::max(m, (double)std::fabs(a[k] - b[k]));
    return m;
}
double energy(const std::vector<float>& a)
{
    double s = 0;
    for (float v : a) s += std::fabs(v);
    return s;
}

QStringList files(const QString& dir, const QString& filter = "*")
{
    return QDir(dir).entryList({filter}, QDir::Files, QDir::Name);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoNrtTest");
    QApplication::setApplicationName("nrt_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: nrt_test <output folder>"); return 2; }
    gOut = args[1];
    QDir(gOut).removeRecursively();
    const QString mediaDir = gOut + "/media", projDir = gOut + "/proj";
    QDir().mkpath(mediaDir);
    QDir().mkpath(projDir);
    uitest::setup({}, {});
    Driver drv;
    gDrv = &drv;
    AppState& app = AppState::get();
    std::string err;

    // ---- a real sound file to work on: 2 seconds of the test tone
    const std::string tone = str(mediaDir + "/tone.wav");
    {
        app.newProject();
        const uint64_t m = app.importMedia("synthaudio:1", &err);
        const std::vector<uint64_t> placed = app.addMediaToTimeline(m, 0.0);
        app.edit("Short", [&](Project& p) { for (uint64_t id : placed) p.findEvent(id)->length = 2.0; });
        check(realTimeRenderTo(tone, &err), "setup: the tone file is made", err);
    }
    auto fresh = [&](int events, bool save) {
        app.newProject();
        std::vector<uint64_t> out;
        const uint64_t m = app.importMedia(tone, &err);
        for (int k = 0; k < events; ++k) {
            const std::vector<uint64_t> placed = app.addMediaToTimeline(m, 3.0 * k);
            out.insert(out.end(), placed.begin(), placed.end());
        }
        if (save) app.save(str(projDir + "/test.reno"), &err);
        return out;
    };

    // ================================================================== names and places
    {
        std::vector<uint64_t> A = fresh(1, false);
        check(nrtDefaultFolder(app.project(), A[0]) == str(QFileInfo(QString::fromStdString(tone)).absolutePath()),
              "folder: the media's folder while the project is unsaved", nrtDefaultFolder(app.project(), A[0]));
        const NrtFile f1 = nextNrtFile(app.project(), A[0], str(mediaDir), "wav");
        check(f1.takeName == "tone Take 2" && QFileInfo(QString::fromStdString(f1.path)).fileName() == "tone Take 2.wav", "naming: the first is '<media> Take 2'", f1.path);
        const NrtFile f2 = nextNrtFile(app.project(), A[0], str(mediaDir), "wav", {f1.path});
        check(f2.takeName == "tone Take 3", "naming: a reserved name counts up", f2.takeName);
        QFile x(mediaDir + "/tone Take 2.flac");
        x.open(QIODevice::WriteOnly);
        x.write("x");
        x.close();
        const NrtFile f3 = nextNrtFile(app.project(), A[0], str(mediaDir), "wav");
        check(f3.takeName == "tone Take 3", "naming: a file of that name in another format counts too (never overwritten)", f3.takeName);
        QFile::remove(mediaDir + "/tone Take 2.flac");
        A = fresh(1, true);
        check(nrtDefaultFolder(app.project(), A[0]) == str(QFileInfo(mediaDir).absoluteFilePath()), "folder: the media's own folder, saved project or not", nrtDefaultFolder(app.project(), A[0]));
        const auto fm = nrtFormats();
        check(fm.size() == 4 && fm[0].first == "wav" && fm[1].first == "w64" && fm[2].first == "aiff" && fm[3].first == "flac", "formats: Wave, Wave64, AIFF, FLAC", std::to_string(fm.size()));
        const auto tp = nrtTemplates("wav");
        check(tp.size() > 2 && tp[0] == "Project sample rate, 24 Bit, Stereo", "templates: the project's rate and 24 bit first, then Render As's", tp.empty() ? "" : tp[0]);
    }

    // ================================================================== the command in the menus
    {
        fresh(1, false);
        MainWindow w;
        w.resize(1280, 800);
        uitest::settle(300);
        QAction* a = w.findChild<QAction*>("nonRealTimeFx");
        check(a != nullptr, "the command 'nonRealTimeFx' exists (Customize Keyboard can bind it)");
        bool inTools = false;
        for (QAction* top : w.menuBar()->actions())
            if (top->menu() && top->text().remove('&') == "Tools")
                for (QAction* s : top->menu()->actions())
                    if (s->menu() && s->text().remove('&') == "Audio")
                        for (QAction* i : s->menu()->actions()) inTools = inTools || i == a;
        check(inTools, "Tools > Audio has Apply Non-Real-Time Event FX");
        TimelineWidget* tl = w.timeline();
        check(tl != nullptr, "the timeline is there");
    }

    // ================================================================== the whole flow for one event
    std::vector<uint64_t> A = fresh(1, true);
    const uint64_t E = A[0];
    const uint64_t original = ev(E)->mediaId;
    const std::string beforeJson = app.project().toJson().dump();
    // sound of the event through Amplitude Modulation (VEGAS's default Distortion passes the sound unchanged), from the live mix, to compare the file with
    std::vector<float> expect;
    {
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = E;
        const uint64_t slot = app.addAudioFx(t, builtinAudio("reno.ampmod"));
        uitest::settle(100);
        expect = mixOf(2.0);
        app.undo();
        uitest::settle(100);
        (void)slot;
    }
    check(app.project().toJson().dump() == beforeJson, "setup: the project is as before the reference was made");
    const size_t undo0 = app.undoCount();
    bool takeSeen = false;
    QStringList chain;
    chooseFx({"Amplitude Modulation"});
    drv.take = [&](QDialog* d) {
        takeSeen = true;
        QWidget* win = d->findChild<QWidget*>("nrtFxWindow");
        check(win != nullptr && win->isVisible(), "Take FX window: the effects window is in it");
        QPushButton* prev = d->findChild<QPushButton*>("nrtPreview");
        check(prev && prev->isEnabled() && prev->text() == "Preview", "Take FX window: a Preview button");
        if (prev) {
            prev->click();
            uitest::settle(50);
            check(gPreview.size() == 1 && gPreview[0] && prev->text() == "Stop", "Preview plays (the button says Stop)");
            prev->click();
            check(gPreview.size() == 2 && !gPreview[1], "Preview stops");
        }
        // the plug-ins' own settings are on the event for now
        for (const FxSlot& s : ev(E)->fx) chain << QString::fromStdString(s.name);
        uitest::shot(d, gOut + "/take_fx.png");
        clickOk(d);
    };
    drv.apply = [&](QDialog* d) {
        QLineEdit* name = d->findChild<QLineEdit*>("nrtFile");
        QLineEdit* folder = d->findChild<QLineEdit*>("nrtFolder");
        QComboBox* format = d->findChild<QComboBox*>("nrtFormat");
        QComboBox* templ = d->findChild<QComboBox*>("nrtTemplate");
        check(name && name->text() == "tone Take 2", "Apply window: the name is '<media> Take 2'", name ? str(name->text()) : "");
        check(folder && QFileInfo(folder->text()) == QFileInfo(mediaDir), "Apply window: the folder is the media's", folder ? str(folder->text()) : "");
        check(format && format->count() == 4 && format->currentText() == "Wave (Microsoft)" && templ && templ->count() > 2, "Apply window: format and template lists");
        uitest::shot(d, gOut + "/apply_fx.png");
        clickRender(d);
    };
    runCommand({E});
    check(takeSeen && drv.applies == 1 && drv.choosers == 1, "the flow: Plug-In Chooser, then Take FX, then the Apply window");
    check(chain.size() == 1 && chain[0].contains("Amplitude Modulation"), "Take FX window: the chosen plug-in is the chain", str(chain.join(",")));
    {
        const Event* e = ev(E);
        const MediaItem* nm = e ? media(e->mediaId) : nullptr;
        const std::string path = str(mediaDir + "/tone Take 2.wav");
        check(e && nm && e->mediaId != original && QFileInfo(QString::fromStdString(nm->path)) == QFileInfo(QString::fromStdString(path)), "the new take: the file 'tone Take 2.wav' beside the project",
              nm ? nm->path : "");
        check(e && e->name == "tone Take 2" && e->takes.size() == 1 && e->takes[0].mediaId == original && e->activeTake == 1, "the new take is active, named 'tone Take 2', the old one kept",
              e ? e->name : "");
        check(e && e->fx.empty(), "the effects are not kept on the event");
        check(app.undoCount() == undo0 + 1 && app.undoName() == "Apply Non-Real-Time Event FX", "one undo step", std::to_string(app.undoCount() - undo0));
        check(nm && std::fabs(nm->info.duration - 2.0) < 0.05, "the file is as long as the event", nm ? num(nm->info.duration) : "");
        const std::vector<float> got = fileSound(path, 2.0, app.project().settings.sampleRate);
        const double d = maxDiff(got, expect);
        check(!got.empty() && d < 2e-4 && energy(got) > 100, "the file's sound is the event's sound through the effects", num(d) + " energy " + num(energy(got)));
        const std::vector<float> dry = fileSound(tone, 2.0, app.project().settings.sampleRate);
        check(maxDiff(dry, got) > 1e-3, "the file differs from the dry source (the effect was applied)");
        // T / Shift+T show the original and the new one again (each press is its own undo step)
        const size_t c0 = app.undoCount();
        app.stepTakes({E}, 1);
        check(ev(E)->mediaId == original, "T: the original take is still there to choose");
        app.stepTakes({E}, 1);
        check(ev(E)->mediaId != original && ev(E)->name == "tone Take 2", "T again: back to the processed take");
        while (app.undoCount() > c0) app.undo();
        uitest::settle(50);
    }
    // undo: all of it in one step
    {
        app.undo();
        uitest::settle(50);
        check(app.project().toJson().dump() == beforeJson && app.undoCount() == undo0, "undo: the event and the media are as before, in one step",
              std::to_string(app.undoCount()) + " vs " + std::to_string(undo0));
        check(ev(E)->takes.empty() && ev(E)->mediaId == original && ev(E)->fx.empty(), "undo: no take, the original media, no effects");
        app.redo();
        uitest::settle(50);
        check(ev(E)->name == "tone Take 2" && ev(E)->takes.size() == 1 && app.undoCount() == undo0 + 1, "redo: the take is back");
    }

    // ================================================================== a second run: Take 2, the first kept
    {
        chooseFx({"Distortion"});
        drv.take = [](QDialog* d) { clickOk(d); };
        QString shown;
        drv.apply = [&](QDialog* d) {
            shown = d->findChild<QLineEdit*>("nrtFile")->text();
            clickRender(d);
        };
        runCommand({E});
        const Event* e = ev(E);
        check(shown == "tone Take 2 Take 2", "second run on the processed take: ' Take 2' is added again, as VEGAS names it", str(shown));
        check(e && e->name == "tone Take 2 Take 2" && e->takes.size() == 2, "second run: three takes, the new one active", e ? std::to_string(e->takes.size()) : "");
        bool hasOrig = false, hasFirst = false;
        for (const Take& t : e->takes) {
            hasOrig = hasOrig || t.mediaId == original;
            hasFirst = hasFirst || t.name == "tone Take 2";
        }
        check(hasOrig && hasFirst, "second run: the original and 'tone Take 2' both stay as takes");
        check(files(mediaDir, "tone Take *.wav") == QStringList({"tone Take 2 Take 2.wav", "tone Take 2.wav"}), "second run: nothing overwritten, a new file", str(files(mediaDir, "tone Take *.wav").join(",")));
        app.stepTakes({E}, 1);
        app.stepTakes({E}, 1);
        app.stepTakes({E}, 1);
        check(ev(E)->name == "tone Take 2 Take 2", "stepping through the three takes comes round");
        // the original made active and processed again: the next free number for its name
        app.stepTakes({E}, 1);
        while (ev(E)->mediaId != original) app.stepTakes({E}, 1);
        chooseFx({"Distortion"});
        drv.take = [](QDialog* d) { clickOk(d); };
        drv.apply = [&](QDialog* d) { shown = d->findChild<QLineEdit*>("nrtFile")->text(); clickRender(d); };
        runCommand({E});
        check(shown == "tone Take 3" && ev(E)->name == "tone Take 3" && ev(E)->takes.size() == 3, "the original processed again: 'tone Take 3', four takes", str(shown));
    }

    // ================================================================== Cancel and failures change nothing
    {
        app.newProject();
        A = fresh(1, true);
        const uint64_t e1 = A[0];
        const std::string json0 = app.project().toJson().dump();
        const size_t u0 = app.undoCount();
        const bool dirty0 = app.dirty();
        const QStringList before = files(mediaDir);
        // Cancel at the Plug-In Chooser
        PluginChooser::onShown = [](PluginChooser* c) { PluginChooser::onShown = nullptr; c->reject(); };
        runCommand({e1});
        check(app.project().toJson().dump() == json0 && app.undoCount() == u0, "Cancel in the Plug-In Chooser: nothing changed");
        // Cancel in the Take FX window: the effects put on for it go again, without an undo step
        chooseFx({"Distortion"});
        drv.take = [](QDialog* d) { clickCancel(d); };
        runCommand({e1});
        check(app.project().toJson().dump() == json0 && app.undoCount() == u0 && app.dirty() == dirty0 && !app.canRedo(),
              "Cancel in the Take FX window: the project is as it was, no undo step, nothing to redo", std::to_string(app.undoCount()) + "/" + std::to_string(u0));
        // Cancel in the Apply window
        chooseFx({"Distortion"});
        drv.take = [](QDialog* d) { clickOk(d); };
        drv.apply = [](QDialog* d) { clickCancel(d); };
        runCommand({e1});
        check(app.project().toJson().dump() == json0 && app.undoCount() == u0 && app.dirty() == dirty0 && files(mediaDir) == before, "Cancel in the Apply window: nothing changed, no file",
              std::to_string(app.undoCount()) + "/" + std::to_string(u0));
        // Cancel while rendering (the progress window's Cancel): the half-made file is deleted
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = e1;
        const uint64_t slot = app.addAudioFx(t, builtinAudio("reno.distortion"));
        FxChain ch;
        for (const FxSlot& s : ev(e1)->fx) if (s.id == slot) ch.push_back(s);
        const std::string json1 = app.project().toJson().dump();
        NrtOptions o;
        o.folder = str(mediaDir);
        o.progress = [](double f) { return f < 0.3; };
        const bool stopped = !applyNonRealTimeFxToEvents({e1}, ch, o, &err);
        check(stopped && err == "stopped" && files(mediaDir) == before && app.project().toJson().dump() == json1, "cancel while rendering: 'stopped', the half-made file is deleted, project unchanged", err);
        // a folder that cannot be made
        o = NrtOptions();
        o.folder = str(mediaDir + "/tone.wav/sub");
        check(!applyNonRealTimeFxToEvents({e1}, ch, o, &err) && app.project().toJson().dump() == json1, "a folder that cannot be made: an error, project unchanged", err);
        // a name that exists is never written over
        QFile x(mediaDir + "/mine.wav");
        x.open(QIODevice::WriteOnly);
        x.write("keep");
        x.close();
        o = NrtOptions();
        o.folder = str(mediaDir);
        o.fileName = "mine";
        check(!applyNonRealTimeFxToEvents({e1}, ch, o, &err) && err.find("already exists") != std::string::npos && QFileInfo(mediaDir + "/mine.wav").size() == 4 &&
                  app.project().toJson().dump() == json1,
              "a typed name that exists: refused, the file untouched, project unchanged", err);
        QFile::remove(mediaDir + "/mine.wav");
        // a typed name is used (and the take takes its name)
        o.fileName = "my result.wav";
        const bool typed = applyNonRealTimeFxToEvents({e1}, ch, o, &err);
        check(typed && QFileInfo(mediaDir + "/my result.wav").exists() && ev(e1)->name == "my result", "a typed name: the file and the take", err);
        // not an audio event / no events
        check(!applyNonRealTimeFxToEvents({}, ch, NrtOptions(), &err), "no events: an error");
    }

    // ================================================================== several events at once
    {
        A = fresh(3, true);
        for (const QString& f : files(mediaDir, "tone Take *")) QFile::remove(mediaDir + "/" + f);
        const std::string json0 = app.project().toJson().dump();
        const size_t u0 = app.undoCount();
        chooseFx({"Distortion"});
        int takeWindows = 0;
        drv.take = [&](QDialog* d) { ++takeWindows; clickOk(d); };
        QString nameText;
        bool nameDisabled = false;
        drv.apply = [&](QDialog* d) {
            QLineEdit* n = d->findChild<QLineEdit*>("nrtFile");
            nameText = n->text();
            nameDisabled = !n->isEnabled();
            clickRender(d);
        };
        runCommand({A[0], A[1], A[2]});
        check(takeWindows == 1 && drv.applies >= 1 && nameDisabled, "several events: one Take FX window and one Apply window for all", str(nameText));
        QStringList names;
        std::set<uint64_t> mediaIds;
        bool allActive = true, noFx = true;
        for (uint64_t id : A) {
            names << QString::fromStdString(ev(id)->name);
            mediaIds.insert(ev(id)->mediaId);
            allActive = allActive && ev(id)->takes.size() == 1;
            noFx = noFx && ev(id)->fx.empty();
        }
        names.sort();
        check(names == QStringList({"tone Take 2", "tone Take 3", "tone Take 4"}) && mediaIds.size() == 3, "several events: each gets its own file, Take 2, Take 3, Take 4", str(names.join(",")));
        check(allActive && noFx, "several events: each keeps its old take, no effects left on any");
        check(files(mediaDir, "tone Take *.wav").size() == 3, "several events: three files");
        check(app.undoCount() == u0 + 1, "several events: one undo step", std::to_string(app.undoCount() - u0));
        app.undo();
        uitest::settle(50);
        bool back = true;
        for (uint64_t id : A) back = back && ev(id)->takes.empty() && ev(id)->name != "tone Take 2";
        check(back && app.project().toJson().dump() == json0, "several events: undo puts all three back");
        // all three have the same sound (stateless effect: still all alike)
        // a cancel in the middle of several: the files already made are deleted
        for (const QString& f : files(mediaDir, "tone Take *")) QFile::remove(mediaDir + "/" + f);
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = A[0];
        const uint64_t slot = app.addAudioFx(t, builtinAudio("reno.distortion"));
        FxChain ch;
        for (const FxSlot& s : ev(A[0])->fx) if (s.id == slot) ch.push_back(s);
        const std::string json1 = app.project().toJson().dump();
        NrtOptions o;
        o.progress = [](double f) { return f < 0.5; };
        check(!applyNonRealTimeFxToEvents(A, ch, o, &err) && err == "stopped" && files(mediaDir, "tone Take *").isEmpty() && app.project().toJson().dump() == json1,
              "cancel in the middle of several events: every file made is deleted, project unchanged", err);
    }

    // ================================================================== formats and templates
    {
        A = fresh(1, true);
        for (const QString& f : files(mediaDir, "tone Take *")) QFile::remove(mediaDir + "/" + f);
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = A[0];
        struct Case { const char* format; const char* tmpl; const char* ext; int rate; };
        const Case cases[] = {{"w64", "", "w64", 0}, {"aiff", "44,100 Hz, 16 Bit, Stereo, PCM", "aif", 44100}, {"flac", "48,000 Hz, 24 Bit, Stereo, FLAC", "flac", 48000},
                              {"wav", "44,100 Hz, 16 Bit, Mono, PCM", "wav", 44100}};
        for (const Case& c : cases) {
            NrtOptions o;
            o.format.format = c.format;
            o.format.templateName = c.tmpl;
            const uint64_t again = app.addAudioFx(t, builtinAudio("reno.distortion"));
            FxChain use;
            for (const FxSlot& s : ev(A[0])->fx) if (s.id == again) use.push_back(s);
            const bool ok = applyNonRealTimeFxToEvents({A[0]}, use, o, &err);
            const Event* e = ev(A[0]);
            const MediaItem* nm = e ? media(e->mediaId) : nullptr;
            std::unique_ptr<MediaReader> r = nm ? openMedia(nm->path, &err) : nullptr;
            const int want = c.rate ? c.rate : app.project().settings.sampleRate;
            check(ok && nm && QFileInfo(QString::fromStdString(nm->path)).suffix() == c.ext && r && r->info().hasAudio && r->info().sampleRate == want,
                  std::string("format ") + c.format + " / " + (c.tmpl[0] ? c.tmpl : "default template") + ": a " + c.ext + " file at " + std::to_string(want) + " Hz",
                  nm ? nm->path + " " + std::to_string(r ? r->info().sampleRate : 0) + " " + err : err);
        }
        check(files(mediaDir, "tone Take *").size() == 4, "four formats: four files (each processes the take before: Take 2, Take 2 Take 2...)", str(files(mediaDir, "tone Take *").join(",")));
        // the Apply window's lists follow the format
        fresh(1, true);
        chooseFx({"Distortion"});
        drv.take = [](QDialog* d) { clickOk(d); };
        QString nm2, info;
        int templCount = 0;
        drv.apply = [&](QDialog* d) {
            QComboBox* f = d->findChild<QComboBox*>("nrtFormat");
            QComboBox* tm = d->findChild<QComboBox*>("nrtTemplate");
            f->setCurrentIndex(f->findText("FLAC Audio"));
            templCount = tm->count();
            tm->setCurrentIndex(tm->findText("44,100 Hz, 16 Bit, Stereo, FLAC"));
            nm2 = d->findChild<QLineEdit*>("nrtFile")->text();
            info = d->findChild<QLabel*>("nrtInfo")->text();
            clickRender(d);
        };
        runCommand({app.project().tracks.front().events.front().id});
        const Event* e = &app.project().tracks.front().events.front();
        const MediaItem* fm = media(e->mediaId);
        std::unique_ptr<MediaReader> r = fm ? openMedia(fm->path, &err) : nullptr;
        check(fm && QFileInfo(QString::fromStdString(fm->path)).suffix() == "flac" && r && r->info().sampleRate == 44100 && templCount == 4 && nm2 == "tone Take 3",
              "Apply window: FLAC and its 44,100 Hz template chosen -> a .flac at 44.1 kHz", fm ? fm->path : "");
    }

    // ================================================================== the effects with a state (a delay) start afresh for each event
    {
        A = fresh(2, true);
        for (const QString& f : files(mediaDir, "tone Take *")) QFile::remove(mediaDir + "/" + f);
        chooseFx({"Pitch Shift", "Distortion"});
        drv.take = [](QDialog* d) { clickOk(d); };
        drv.apply = [](QDialog* d) { clickRender(d); };
        runCommand({A[0], A[1]});
        const MediaItem* m0 = media(ev(A[0])->mediaId);
        const MediaItem* m1 = media(ev(A[1])->mediaId);
        check(m0 && m1 && m0->path != m1->path, "two effects in a chain on two events: two files");
        if (m0 && m1) {
            const int rate = app.project().settings.sampleRate;
            const std::vector<float> a = fileSound(m0->path, 1.5, rate), b = fileSound(m1->path, 1.5, rate);
            check(!a.empty() && maxDiff(a, b) < 1e-3, "the same sound through the same chain gives the same file for each event", num(maxDiff(a, b)));
        }
    }

    // ================================================================== the menu command and the right-click menu
    {
        A = fresh(1, true);
        MainWindow w;
        w.resize(1280, 800);
        uitest::settle(300);
        TimelineWidget* tl = w.timeline();
        QAction* a = w.findChild<QAction*>("nonRealTimeFx");
        app.selectOnly(A[0]);
        chooseFx({"Distortion"});
        drv.take = [](QDialog* d) { clickOk(d); };
        drv.apply = [](QDialog* d) { clickRender(d); };
        const size_t u0 = app.undoCount();
        if (a) a->trigger();
        uitest::settle(100);
        check(ev(A[0])->takes.size() == 1 && app.undoCount() == u0 + 1 && !gPreview.empty(), "the main window's command runs the whole thing (and Preview plays the project)");
        (void)tl;
    }

    return uitest::summary();
}
