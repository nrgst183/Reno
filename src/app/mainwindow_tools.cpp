// The main window's added windows and tools; see mainwindow_tools.h.
#include "mainwindow_tools.h"
#include "pollrate.h"
#include "vprefs.h"
#include "timeline.h"
#include "panels.h"
#include "trimmer.h"
#include "mixer.h"
#include "recorder.h"
#include "loudnesswin.h"
#include "pluginmanager.h"
#include "dashboard.h"
#include "deviceexplorer.h"
#include "externalmonitor.h"
#include "colorgrading.h"
#include "motiontracking.h"
#include "tts.h"
#include "midisync.h"
#include "surroundpanner.h"
#include "fxwindow.h"
#include <QGuiApplication>
#include <QScreen>
#include "toolwin.h"
#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QLineEdit>
#include <QFileInfo>
#include <QRegularExpression>
#include <QApplication>
#include <QMessageBox>
#include <QInputDialog>
#include <QPushButton>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDialog>
#include <QDockWidget>
#include <QFileDialog>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QProgressDialog>
#include <QStatusBar>
#include <QTimer>
#include <cmath>

namespace reno {
namespace app {

using namespace tw;

namespace {

QAction* keyAction(MainWindow* w, const char* name, const QString& text, const QList<QKeySequence>& keys, std::function<void()> fn)
{
    QAction* a = new QAction(text, w);
    a->setObjectName(name);
    if (!keys.isEmpty()) a->setShortcuts(keys);
    QObject::connect(a, &QAction::triggered, w, [fn] { fn(); });
    w->addAction(a);
    return a;
}

QList<QKeySequence> seq(std::initializer_list<const char*> list)
{
    QList<QKeySequence> out;
    for (const char* k : list) out << QKeySequence(QString::fromUtf8(k));
    return out;
}

// A menu bar menu by its title ("Tools", "View"...).
QMenu* menuNamed(QMainWindow* w, const QString& title)
{
    for (QAction* a : w->menuBar()->actions())
        if (a->menu() && a->text().remove('&') == title) return a->menu();
    return nullptr;
}

// Input level of the armed tracks in the status bar: two thin bars (left over right), green to red.
class InputMeter : public QWidget {
public:
    explicit InputMeter(Recorder* r, QWidget* parent) : QWidget(parent), rec_(r)
    {
        setObjectName("inputMeter");
        setFixedSize(150, 16);
        timer_ = new QTimer(this);
        timer_->setInterval(50);
        QObject::connect(timer_, &QTimer::timeout, this, [this] {
            rec_->inputLevel(l_, r_);
            update();
            if (rate_) rate_->ticked(rec_->recording() || l_ > 1e-4f || r_ > 1e-4f);
        });
        timer_->start();
        rate_ = new PollRate(this, timer_, 50);
    }
    float left() const { return l_; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), gap());
        const int x0 = 28, w = width() - x0 - 2;
        p.setPen(QColor(200, 200, 200));
        QFont f = font();
        f.setPixelSize(9);
        p.setFont(f);
        p.drawText(QRect(2, 0, x0 - 4, height()), Qt::AlignVCenter | Qt::AlignLeft, rec_->recording() ? "REC" : "In");
        for (int k = 0; k < 2; ++k) {
            const float v = k == 0 ? l_ : r_;
            const double db = v > 1e-6f ? 20.0 * std::log10(v) : -60.0;
            const double frac = std::max(0.0, std::min(1.0, (db + 60.0) / 60.0));
            const QRect groove(x0, 2 + k * 6, w, 5);
            p.fillRect(groove, QColor(20, 20, 20));
            QLinearGradient g(groove.left(), 0, groove.right(), 0);
            g.setColorAt(0, QColor(40, 170, 80));
            g.setColorAt(0.8, QColor(220, 200, 50));
            g.setColorAt(1, QColor(230, 60, 50));
            p.fillRect(QRect(groove.left(), groove.top(), (int)std::lround(groove.width() * frac), groove.height()), g);
        }
        if (rec_->recording()) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(230, 50, 50));
            p.drawEllipse(QPointF(width() - 6, height() / 2.0), 3, 3);
        }
    }

private:
    Recorder* rec_;
    QTimer* timer_ = nullptr;
    PollRate* rate_ = nullptr;
    float l_ = 0, r_ = 0;
};

std::string prerenderFolder()
{
    std::string dir = AppState::get().prefs.prerenderFolder;
    if (dir.empty()) dir = pathJoin(userDataDir(), "Prerendered");
    QDir().mkpath(qs(dir));
    return dir;
}

}

MainWindow::Tools::~Tools()
{
    AppState::get().removeListener(listener);
    openInTrimmer = nullptr;
    previewMedia = nullptr;
    openProjectFile = nullptr;
    onProxiesMade = nullptr;
    delete monitor;
    monitor = nullptr;
    midi.reset();
    recorder.reset();
}

void MainWindow::buildToolWindows()
{
    tools_.reset(new Tools());
    Tools* t = tools_.get();
    AppState& app = AppState::get();
    Playback* pb = playback_.get();
    QMenu* toolsMenu = menuNamed(this, "Tools");
    t->transport = dynamic_cast<TransportBar*>(findChild<QWidget*>("transportBar"));

    auto addWindow = [this](QDockWidget* d, const char* actionName, const char* key) {
        QAction* toggle = d->toggleViewAction();
        toggle->setText(QString("%1\t%2").arg(d->windowTitle(), key));
        if (windowMenu_) windowMenu_->addAction(toggle);
        keyAction(this, actionName, QString("Show %1").arg(d->windowTitle()), seq({key}), [this, d] { showDock(d); });
        d->hide();
    };

    t->trimmer = new TrimmerWindow(this);
    t->trimmerDock = dock("Trimmer", "dockTrimmer", t->trimmer);
    addWindow(t->trimmerDock, "showTrimmer", "Alt+2");
    t->trimmer->onWillPlay = [pb] { pb->pause(); };
    t->trimmer->onPreviewPicture = [this](const QImage& img) { if (preview_) preview_->showPicture(img); };
    openInTrimmer = [this, t](uint64_t mediaId, const QString& path) {
        showDock(t->trimmerDock);
        if (mediaId) t->trimmer->openMedia(mediaId);
        else t->trimmer->openFile(path);
        t->trimmer->setFocus();
    };
    // Explorer's and Project Media's Start / Stop Preview: the file plays in the Trimmer (VEGAS: "Video is previewed in
    // the Trimmer window"); a project double-clicked in the Explorer opens
    previewMedia = [this, t](uint64_t mediaId, const QString& path, bool start) {
        if (!start) { t->trimmer->stop(); return; }
        if (!t->trimmerDock->isVisible()) showDock(t->trimmerDock);
        if (mediaId ? t->trimmer->openMedia(mediaId) : t->trimmer->openFile(path)) t->trimmer->play();
    };
    openProjectFile = [this](const QString& path) {
        if (maybeSave()) openProject(path);
    };
    onProxiesMade = [pb] { pb->requestFrame(); };
    trimmerSelection = [t](uint64_t& mediaId, double& in, double& out) {
        mediaId = t->trimmer->currentMedia();
        in = t->trimmer->selectionIn();
        out = t->trimmer->selectionOut();
        return mediaId != 0 && out > in;
    };

    // ---- recording (Ctrl+R; Ctrl+Alt+R arms the selected track; Ctrl+Alt+Shift+R arms it and asks for the folder)
    t->recorder.reset(new Recorder(pb));
    Recorder* rec = t->recorder.get();
    t->inputMeter = new InputMeter(rec, this);
    statusBar()->addPermanentWidget(t->inputMeter);
    t->inputMeter->hide();
    rec->onChanged = [t, rec] {
        t->inputMeter->setVisible(rec->inputOpen() || rec->recording());
        t->inputMeter->setToolTip(QString("Recording input: %1").arg(qs(rec->inputName())));
        if (t->transport) t->transport->setRecording(rec->recording());
    };
    t->toggleRecord = [rec, &app] {
        if (rec->recording()) {
            rec->stop();
            return;
        }
        std::string err;
        if (!rec->start(&err)) app.status("Record: " + err);
    };
    timeline_->onArmTrack = [rec](uint64_t id) { rec->setArmed(id, !rec->armed(id)); };
    if (MixerWindow* mixer = mixerDock_ ? dynamic_cast<MixerWindow*>(mixerDock_->findChild<QWidget*>("mixer")) : nullptr) mixer->onArmTrack = [rec](uint64_t id) { rec->setArmed(id, !rec->armed(id)); };
    if (t->transport) t->transport->setRecordHandler([t] { t->toggleRecord(); });
    QAction* record = keyAction(this, "record", "&Record", seq({"Ctrl+R"}), [t] { t->toggleRecord(); });
    QAction* arm = keyAction(this, "armTrack", "&Arm Track for Record", seq({"Ctrl+Alt+R"}), [rec] { rec->armSelected(); });
    QAction* armPath = keyAction(this, "armTrackPath", "Arm for Record and Set Recording &Folder...", seq({"Ctrl+Alt+Shift+R"}), [this, rec, &app] {
        const QString dir = QFileDialog::getExistingDirectory(this, "Recorded Files Folder", qs(rec->recordFolder()));
        if (dir.isEmpty()) return;
        app.prefs.recordFolder = ss(dir);
        app.prefs.save();
        if (!rec->armed(app.currentTrack)) rec->armSelected();
        app.status("Recordings go to " + ss(dir));
    });

    t->loudness = new LoudnessWindow(pb, this);
    t->loudnessDock = dock("Loudness Meters", "dockLoudness", t->loudness);
    addWindow(t->loudnessDock, "showLoudness", "Ctrl+Alt+7");

    // ---- the Surround Panner (Ctrl+Alt+3): the current track's (or bus's) panner in a 5.1 project; a double-click on a Mixing Console strip's small panner shows that track's or bus's
    t->surround = new SurroundPannerWindow(this);
    t->surroundDock = dock("Surround Panner", "dockSurround", t->surround);
    addWindow(t->surroundDock, "showSurroundPanner", "Ctrl+Alt+3");
    if (MixerWindow* mixer = mixerDock_ ? dynamic_cast<MixerWindow*>(mixerDock_->findChild<QWidget*>("mixer")) : nullptr)
        mixer->onOpenSurround = [this, t](uint64_t id) {
            t->surround->showPanner(id);
            showDock(t->surroundDock);
        };
    if (TimelineWidget* tl = timeline_)
        tl->onOpenSurround = [this, t](uint64_t id) {
            t->surround->showPanner(id);
            showDock(t->surroundDock);
        };

    t->plugins = new PluginManager(this);
    t->plugins->onActivated = [this](FxBrowser::Kind kind, const std::string& data) { browserActivated(kind, data); };
    t->pluginsDock = dock("Plug-In Manager", "dockPlugins", t->plugins);
    addWindow(t->pluginsDock, "showPluginManager", "Ctrl+Alt+1");

    t->dashboard = new Dashboard(pb, rec, this);
    t->dashboardDock = dock("Dashboard", "dockDashboard", t->dashboard);
    addWindow(t->dashboardDock, "showDashboard", "Ctrl+Alt+8");
    t->devices = new DeviceExplorer(this);
    t->devicesDock = dock("Device Explorer", "dockDevices", t->devices);
    addWindow(t->devicesDock, "showDeviceExplorer", "Ctrl+Alt+6");

    t->monitor = new ExternalMonitor(pb, this);
    QAction* monitor = keyAction(this, "externalMonitor", "Preview on E&xternal Monitor", seq({"Alt+Shift+4"}), [t] { t->monitor->toggle(); });
    if (QMenu* view = menuNamed(this, "View")) {
        view->addSeparator();
        view->addAction(monitor);
        QMenu* displays = view->addMenu("External Monitor &Display");
        displays->setObjectName("externalMonitorDisplays");
        QObject::connect(displays, &QMenu::aboutToShow, this, [t, displays] {
            displays->clear();
            QAction* any = displays->addAction("Another display than Reno's");
            any->setCheckable(true);
            any->setChecked(t->monitor->screenName().isEmpty());
            QObject::connect(any, &QAction::triggered, displays, [t] { t->monitor->setScreenName(QString()); });
            displays->addSeparator();
            for (QScreen* sc : QGuiApplication::screens()) {
                const QString name = sc->name();
                QAction* a = displays->addAction(QString("%1 (%2 x %3)").arg(name).arg(sc->geometry().width()).arg(sc->geometry().height()));
                a->setCheckable(true);
                a->setChecked(t->monitor->screenName() == name);
                QObject::connect(a, &QAction::triggered, displays, [t, name] { t->monitor->setScreenName(name); });
            }
        });
    }

    t->grading = new ColorGradingPanel(this);
    t->grading->onOpenFx = [this](const FxTarget& target, uint64_t slot) { openFx(target, slot); };
    t->grading->setPickerHook = [this](std::function<void(int, const QRectF&, const QImage&)> fn) { preview()->setPicker(std::move(fn)); };
    t->grading->frameSource = [pb] { return pb->renderNow(AppState::get().cursor()); };
    t->gradingDock = dock("Color Grading", "dockColorGrading", t->grading);
    addWindow(t->gradingDock, "showColorGrading", "Alt+G");
    fxWindow_->onOpenBuiltin = [this, t](uint64_t slotId, const FxTarget& target) {
        showDock(t->gradingDock);
        t->grading->showSlot(slotId, target);
    };
    t->colorGradingLayout = [this, t] {
        if (timelineMaximized_)
            if (QAction* m = findChild<QAction*>("maximizeTimeline")) m->setChecked(false);
        QDockWidget* preview = findChild<QDockWidget*>("dockPreview");
        QDockWidget* scopes = findChild<QDockWidget*>("dockScopes");
        if (!preview || !scopes) return;
        for (QDockWidget* d : docks_) {
            d->setFloating(false);
            removeDockWidget(d);
        }
        addDockWidget(Qt::TopDockWidgetArea, preview);
        splitDockWidget(preview, scopes, Qt::Horizontal);
        splitDockWidget(preview, t->gradingDock, Qt::Vertical);
        for (QDockWidget* d : docks_)
            if (d != preview && d != scopes && d != t->gradingDock) tabifyDockWidget(t->gradingDock, d);
        for (QDockWidget* d : docks_) d->setVisible(d == preview || d == scopes || d == t->gradingDock);
        t->gradingDock->raise();
        const int W = std::max(800, width()), H = std::max(500, height());
        resizeDocks({preview, scopes}, {W * 62 / 100, W * 38 / 100}, Qt::Horizontal);
        resizeDocks({preview, t->gradingDock}, {H * 48 / 100, H * 32 / 100}, Qt::Vertical);
        currentLayout_ = "Color Grading Layout";
        updateDockTabs();
    };
    keyAction(this, "colorGradingLayout", "&Color Grading Layout", seq({"Alt+D, C"}), [t] { t->colorGradingLayout(); });

    t->tracking = new MotionTrackingWindow(pb, this);
    t->trackingDock = dock("Motion Tracking", "dockMotionTracking", t->tracking);
    addWindow(t->trackingDock, "showMotionTracking", "Alt+M");

    t->speech = new TextToSpeechWindow(this);
    t->speechDock = dock("Text to Speech", "dockTextToSpeech", t->speech);
    addWindow(t->speechDock, "showTextToSpeech", "Ctrl+Shift+P");

    t->midi.reset(new MidiSync(pb));
    MidiSync* midi = t->midi.get();
    auto midiToggle = [this, &app](const char* name, const QString& text, const char* key, std::function<bool(bool, std::string*)> set) {
        QAction* a = keyAction(this, name, text, seq({key}), [] {});
        a->setCheckable(true);
        QObject::disconnect(a, &QAction::triggered, nullptr, nullptr);
        QObject::connect(a, &QAction::triggered, this, [a, set, &app](bool on) {
            std::string err;
            if (!set(on, &err)) {
                a->setChecked(false);
                app.status(ss(a->text().remove('&')) + ": " + err);
            }
        });
        return a;
    };
    QAction* mtc = midiToggle("generateMtc", "Generate MIDI &Timecode", "F7", [midi](bool on, std::string* e) { return midi->setGenerateTimecode(on, e); });
    QAction* clock = midiToggle("generateClock", "Generate MIDI &Clock", "Shift+F7", [midi](bool on, std::string* e) { return midi->setGenerateClock(on, e); });
    QAction* chase = midiToggle("triggerFromMtc", "T&rigger from MIDI Timecode", "Ctrl+F7", [midi](bool on, std::string* e) { return midi->setChase(on, e); });
    midi->onChanged = [mtc, clock, chase, midi] {
        mtc->setChecked(midi->generatingTimecode());
        clock->setChecked(midi->generatingClock());
        chase->setChecked(midi->chasing());
    };
    if (QMenu* options = menuNamed(this, "Options")) {
        options->addSeparator();
        QMenu* tc = options->addMenu("T&imecode");
        tc->setObjectName("timecodeMenu");
        tc->addActions({mtc, clock, chase});
        tc->addSeparator();
        for (int dir = 0; dir < 2; ++dir) {
            QMenu* devs = tc->addMenu(dir == 0 ? "MIDI &Output" : "MIDI &Input");
            QObject::connect(devs, &QMenu::aboutToShow, this, [devs, midi, dir] {
                devs->clear();
                const std::vector<std::string> names = dir == 0 ? midiOutputNames() : midiInputNames();
                const std::vector<int> shown = availableMidiPorts(names, dir == 0);
                if (shown.empty()) devs->addAction(dir == 0 ? "(no MIDI outputs)" : "(no MIDI inputs)")->setEnabled(false);
                for (int k : shown) {
                    QAction* a = devs->addAction(qs(names[(size_t)k]));
                    a->setCheckable(true);
                    a->setChecked(k == (dir == 0 ? midi->outputDevice() : midi->inputDevice()));
                    QObject::connect(a, &QAction::triggered, devs, [midi, dir, k] {
                        if (dir == 0) midi->setOutputDevice(k);
                        else midi->setInputDevice(k);
                    });
                }
            });
        }
    }

    t->prerender = [this, pb, &app]() -> bool {
        if (!(app.loopEnd > app.loopStart)) {
            app.status("Selective Prerender: make a time selection first.");
            return false;
        }
        pb->pause();
        QProgressDialog progress("Prerendering video...", "Cancel", 0, 1000, this);
        progress.setWindowTitle("Selectively Prerender Video");
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(300);
        std::string err;
        const bool ok = pb->prerender(app.loopStart, app.loopEnd, prerenderFolder(), [&](double f) {
            progress.setValue((int)(f * 1000));
            QCoreApplication::processEvents();
            return !progress.wasCanceled();
        }, &err);
        progress.setValue(1000);
        app.status(ok ? strfmt("Prerendered %s: it plays from the file until something there changes.", timecode(app.loopEnd - app.loopStart, app.project().settings.fps()).c_str())
                      : "Selective Prerender: " + err);
        return ok;
    };
    QAction* prerender = keyAction(this, "selectivePrerender", "Selectively &Prerender Video...", seq({"Shift+M"}), [t] { t->prerender(); });
    QAction* cleanUp = keyAction(this, "cleanPrerender", "Clean &Up Prerendered Video", QList<QKeySequence>(), [pb, &app] {
        const size_t n = pb->prerenderedRanges().size();
        if (n > 0 && vprefs::on("prefs/confirmCleanPrerender") &&
            QMessageBox::question(QApplication::activeWindow(), "Clean Up Prerendered Video", QString("Delete the %1 prerendered stretch%2 and their files?").arg(n).arg(n == 1 ? "" : "es")) != QMessageBox::Yes)
            return;
        pb->clearPrerendered(true);
        app.status(strfmt("%d prerendered stretch%s deleted.", (int)n, n == 1 ? "" : "es"));
    });

    // ---- Insert Adjustment Track (Ctrl+Shift+Alt+Q): a video track whose effects work on everything under it, put above the selected video track (else at the top), with its Track FX open to choose an effect
    QAction* adjTrack = keyAction(this, "adjustmentTrack", "Ad&justment Track", seq({"Ctrl+Shift+Alt+Q"}), [this, &app] {
        uint64_t made = 0;
        app.edit("Insert Adjustment Track", [&](Project& p) {
            size_t at = 0;
            for (size_t k = 0; k < p.tracks.size(); ++k)
                if (p.tracks[k].id == app.currentTrack && p.tracks[k].type == TrackType::Video) at = k;
            Track tr;
            tr.id = p.newId();
            tr.type = TrackType::Video;
            tr.adjustment = true;
            tr.name = "Adjustment Track";
            if (at < p.tracks.size()) tr.compositeLevel = p.tracks[at].compositeLevel;
            p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)at, tr);
            made = tr.id;
        });
        app.currentTrack = made;
        app.notify(kSelection);
        FxTarget target;
        target.kind = FxTarget::Track;
        target.id = made;
        openFx(target, 0);
    });
    if (QMenu* insert = menuNamed(this, "Insert")) {
        QList<QAction*> acts = insert->actions();
        QAction* before = nullptr;
        for (int k = 0; k + 1 < acts.size(); ++k)
            if (acts[k]->objectName() == "videoTrack") before = acts[k + 1];
        insert->insertAction(before, adjTrack);
    }

    if (toolsMenu) {
        toolsMenu->addSeparator();
        toolsMenu->addActions({record, arm, armPath});
        toolsMenu->addSeparator();
        toolsMenu->addActions({prerender, cleanUp});
    }

    t->listener = app.addListener([t](int changes) {
        if ((changes & kPlayback) && t->transport && t->recorder) t->transport->setRecording(t->recorder->recording());
    });
}

}
}
