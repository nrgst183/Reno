#include "startup.h"
#include "logbook.h"
#include "vprefs.h"
#include "prefsapply.h"
#include "appstate.h"
#include "logo.h"
#include "mainwindow.h"
#include "timeline.h"
#include "../engine/catalog.h"
#include "../engine/fxnames.h"
#include "../engine/fxruntime.h"
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLabel>
#include <QLinearGradient>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>

namespace reno {
namespace app {

namespace {

// Splash screen colors: Reno's dark grey and the logo's orange.
const QColor kSplashTop(30, 30, 32), kSplashBottom(18, 18, 19), kSplashOrange(234, 96, 0), kSplashText(225, 225, 228),
    kSplashDim(150, 150, 156), kSplashTrack(48, 48, 52);

SplashScreen* gSplash = nullptr;
PluginLoadProgress* gLoad = nullptr;

QString elide(const QFont& f, const QString& s, int width)
{
    return QFontMetrics(f).elidedText(s, Qt::ElideMiddle, width);
}

}  // namespace

SplashScreen::SplashScreen() : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint)
{
    setObjectName("splashScreen");
    setWindowTitle("Reno");
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedSize(560, 320);
    logo_ = renoLogo(112);
    skip_ = new QPushButton("Skip", this);
    skip_->setObjectName("splashSkip");
    skip_->setToolTip("Go on to Reno now: the plug-ins not checked yet are checked in the background");
    skip_->setCursor(Qt::PointingHandCursor);
    skip_->setStyleSheet(QString("QPushButton { color: %1; background: %2; border: 1px solid %3; padding: 3px 14px; }"
                                 "QPushButton:hover { background: %3; color: white; }")
                             .arg(kSplashText.name(), kSplashTrack.name(), kSplashOrange.name()));
    skip_->adjustSize();
    skip_->hide();
    QObject::connect(skip_, &QPushButton::clicked, this, [this] {
        if (onSkip) onSkip();
    });
    busy_ = new QTimer(this);
    busy_->setInterval(40);
    QObject::connect(busy_, &QTimer::timeout, this, [this] {
        busyPhase_ = (busyPhase_ + 1) % 100;
        update();
    });
    if (QScreen* s = QGuiApplication::primaryScreen()) move(s->availableGeometry().center() - rect().center());
    gSplash = this;
}

SplashScreen::~SplashScreen()
{
    if (gSplash == this) gSplash = nullptr;
}

SplashScreen* SplashScreen::current() { return gSplash; }

void SplashScreen::setStatus(const QString& line, const QString& detail)
{
    if (line == line_ && detail == detail_) return;
    line_ = line;
    detail_ = detail;
    if (isVisible()) repaint();
}

void SplashScreen::setProgress(int done, int total)
{
    done = std::max(0, std::min(done, total));
    if (done == done_ && total == total_ && (total > 0) != busy_->isActive()) return;
    done_ = done;
    total_ = total;
    if (total > 0) busy_->stop();
    else if (!busy_->isActive()) busy_->start();
    if (isVisible()) repaint();
}

void SplashScreen::showSkip(bool on)
{
    skip_->setVisible(on);
}

void SplashScreen::resizeEvent(QResizeEvent*)
{
    skip_->move(width() - skip_->width() - 24, height() - skip_->height() - 18);
}

void SplashScreen::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    const QRect r = rect();
    QLinearGradient bg(r.topLeft(), r.bottomLeft());
    bg.setColorAt(0, kSplashTop);
    bg.setColorAt(1, kSplashBottom);
    p.fillRect(r, bg);
    p.fillRect(QRect(0, 0, r.width(), 4), kSplashOrange);
    p.setPen(QPen(kSplashOrange, 1));
    p.drawRect(r.adjusted(0, 0, -1, -1));

    const int x0 = 32;
    p.drawImage(QPoint(x0, 36), logo_);
    QFont big = font();
    big.setFamilies({"Segoe UI", "Arial", "DejaVu Sans"});
    big.setPixelSize(46);
    big.setWeight(QFont::DemiBold);
    p.setFont(big);
    p.setPen(kSplashText);
    const int tx = x0 + logo_.width() + 26;
    p.drawText(QPoint(tx, 92), "Reno");
    const int nameW = QFontMetrics(big).horizontalAdvance("Reno ");
    p.setPen(kSplashOrange);
    p.drawText(QPoint(tx + nameW, 92), QString(renoVersion()));
    QFont small = font();
    small.setFamilies({"Segoe UI", "Arial", "DejaVu Sans"});
    small.setPixelSize(13);
    p.setFont(small);
    p.setPen(kSplashDim);
    p.drawText(QPoint(tx + 2, 118), "Video editing with OpenFX and VST plug-ins");

    const int lineY = 206, w = r.width() - 2 * x0;
    QFont status = small;
    status.setPixelSize(14);
    p.setFont(status);
    p.setPen(kSplashText);
    p.drawText(QRect(x0, lineY, w, 20), Qt::AlignLeft | Qt::AlignVCenter, elide(status, line_, w));
    p.setFont(small);
    p.setPen(kSplashDim);
    p.drawText(QRect(x0, lineY + 22, w, 18), Qt::AlignLeft | Qt::AlignVCenter, elide(small, detail_, w));
    const QRect bar(x0, lineY + 50, w, 6);
    p.fillRect(bar, kSplashTrack);
    if (total_ > 0) p.fillRect(QRect(bar.x(), bar.y(), (int)((long long)bar.width() * done_ / total_), bar.height()), kSplashOrange);
    else {
        const int piece = bar.width() / 5;
        const double t = busyPhase_ / 50.0;
        const double f = t <= 1 ? t : 2 - t;
        p.fillRect(QRect(bar.x() + (int)((bar.width() - piece) * f), bar.y(), piece, bar.height()), kSplashOrange);
    }
    painted_ = detail_.isEmpty() ? line_ : line_ + "\n" + detail_;
    ++paints_;
}

QString pluginLoadName(const FxSlot& slot, QString* file)
{
    QString name, path;
    if (slot.kind == "audio") {
        name = QString::fromStdString(slot.audio.name.empty() ? slot.name : slot.audio.name);
        const std::string& f = slot.audio.format;
        if (!f.empty()) name += QString(" (%1)").arg(f == "VST2" ? "VST" : f == "DX" ? "DirectX" : QString::fromStdString(f));
        path = QString::fromStdString(slot.audio.path);
    } else {
        VideoFxEntry e;
        const bool known = Catalog::get().findVideo(slot.pluginId, &e);
        name = QString::fromStdString(fxSlotLabel(slot));
        if (known && !e.builtin) path = QString::fromStdString(e.path);
    }
    if (name.trimmed().isEmpty()) name = "a plug-in";
    if (file) *file = path.isEmpty() ? QString() : QFileInfo(path).fileName();
    return name;
}

std::function<void(const QString&)>& PluginLoadProgress::beforePlugin()
{
    static std::function<void(const QString&)> fn;
    return fn;
}

QDialog* PluginLoadProgress::currentWindow() { return gLoad ? gLoad->dialog_ : nullptr; }

PluginLoadProgress::PluginLoadProgress(QWidget* parent, const QString& what) : parent_(parent), what_(what)
{
    if (gLoad) return;
    outer_ = true;
    gLoad = this;
    startMs_ = QDateTime::currentMSecsSinceEpoch();
    AppState::get().fx.onMaking = [this](const FxSlot& slot, int done, int total) { making(slot, done, total); };
}

PluginLoadProgress::~PluginLoadProgress()
{
    if (!outer_) return;
    AppState::get().fx.onMaking = nullptr;
    gLoad = nullptr;
    delete dialog_;
}

void PluginLoadProgress::making(const FxSlot& slot, int done, int total)
{
    QString file;
    const QString name = pluginLoadName(slot, &file);
    const QString count = total > 1 ? QString("  (%1 of %2)").arg(done + 1).arg(total) : QString();
    ++shown_;
    logbook::write(logbook::Kind::Plugins, logbook::Basic, "Loading " + name + (file.isEmpty() ? QString() : " - " + file) + count);
    logbook::write(logbook::Kind::Plugins, logbook::Detailed,
                   QString("  kind %1, format %2").arg(QString::fromStdString(slot.kind), slot.kind == "audio" ? QString::fromStdString(slot.audio.format) : QString("OpenFX/built-in")));
    QString painted;
    if (SplashScreen* s = SplashScreen::current(); s && s->isVisible()) {
        // VEGAS's wording while plug-ins start ("Initializing VST plug-ins...", "Initializing DirectX plug-ins...")
        const char* kind = slot.kind != "audio" ? "video" : slot.audio.format == "DX" ? "DirectX" : slot.audio.format == "VST3" ? "VST3" : "VST";
        s->setStatus(QString("Initializing %1 plug-ins...").arg(kind),
                     "Loading " + name + (file.isEmpty() ? QString() : " - " + file) + count);
        s->setProgress(done, total);
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        painted = s->paintedStatus();
    } else {
        const bool own = file.isEmpty();
        if (!dialog_ && (!own || QDateTime::currentMSecsSinceEpoch() - startMs_ > 300)) {
            dialog_ = new QDialog(parent_ ? parent_->window() : nullptr, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint);
            dialog_->setObjectName("pluginLoadProgress");
            dialog_->setWindowTitle("Reno");
            dialog_->setWindowModality(Qt::ApplicationModal);
            dialog_->setMinimumWidth(440);
            auto* lay = new QVBoxLayout(dialog_);
            title_ = new QLabel(what_.isEmpty() ? QString("Loading plug-ins...") : what_ + "...", dialog_);
            title_->setObjectName("pluginLoadTitle");
            QFont f = title_->font();
            f.setBold(true);
            title_->setFont(f);
            line_ = new QLabel(dialog_);
            line_->setObjectName("pluginLoadLine");
            line_->setTextFormat(Qt::PlainText);
            bar_ = new QProgressBar(dialog_);
            bar_->setObjectName("pluginLoadBar");
            bar_->setTextVisible(false);
            lay->addWidget(title_);
            lay->addWidget(line_);
            lay->addWidget(bar_);
            dialog_->show();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        if (dialog_) {
            line_->setText(elide(line_->font(), "Loading " + name + (file.isEmpty() ? QString() : " - " + file) + count, 420));
            bar_->setRange(0, std::max(1, total));
            bar_->setValue(done);
            dialog_->repaint();
            QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
            painted = title_->text() + "\n" + line_->text();
        }
    }
    if (beforePlugin()) beforePlugin()(painted);
}

MainWindow* runStartup(const StartupOptions& o)
{
    SplashScreen* splash = new SplashScreen();
    splash->setStatus("Starting...");
    splash->setProgress(0, 0);
    if (o.showSplash) {
        splash->show();
        splash->raise();
    }
    QCoreApplication::processEvents();
    if (o.onSplash) o.onSplash(splash);

    splash->setStatus("Creating plug-in factory...");
    QCoreApplication::processEvents();
    prefsapply::apply();
    const bool haveCache = Catalog::get().loadCache();

    // new or changed plug-in files are tried (each in its own reno_cli, on the scan's thread), all of them the first
    // time. Skip goes on and leaves the scan running.
    const std::string exe = o.probeExe.empty() ? defaultProbeExe() : o.probeExe;
    bool background = false;
    // Preferences > Reno > Plug-ins "Look for new plug-ins when Reno starts" (if cleared, only the first time, when
    // there is no list)
    if (fileExists(exe) && (haveCache ? vprefs::on("prefs/scanAtStartup") : true) && ScanTask::get().start(exe, false, haveCache)) {
        splash->setStatus(haveCache ? "Scanning for new plug-ins..." : "Scanning for plug-ins (the first time Reno starts)...");
        bool skipped = false;
        QEventLoop loop;
        splash->onSkip = [&] {
            skipped = true;
            loop.quit();
        };
        QTimer poll;
        poll.setInterval(30);
        QElapsedTimer since;
        since.start();
        QObject::connect(&poll, &QTimer::timeout, [&] {
            const ScanTask::State s = ScanTask::get().state();
            if (!s.running) { loop.quit(); return; }
            if (s.total > 0) {
                splash->setStatus(haveCache ? "Scanning for new plug-ins..." : "Scanning for plug-ins (the first time Reno starts)...",
                                  s.what.empty() ? QString() : QString("Checking %1  (%2 of %3)").arg(QString::fromStdString(s.what)).arg(s.done + 1).arg(s.total));
                splash->setProgress(s.done, s.total);
            }
            if (since.elapsed() > 400) splash->showSkip(true);
        });
        poll.start();
        if (ScanTask::get().state().running) loop.exec();
        poll.stop();
        splash->onSkip = nullptr;
        splash->showSkip(false);
        background = skipped && ScanTask::get().state().running;
        if (!background) {
            ScanTask::get().join();
            AppState::get().notify(kCatalog);
        }
    }

    logbook::write(logbook::Kind::Plugins, logbook::Basic,
                   QString("Plug-in list ready: %1 video effects, %2 audio effects").arg(Catalog::get().video().size()).arg(Catalog::get().audio().size()));
    splash->setStatus("Creating windows...");
    splash->setProgress(0, 0);
    QCoreApplication::processEvents();
    MainWindow* w = o.makeWindow ? o.makeWindow() : nullptr;
    if (!w) {
        splash->close();
        delete splash;
        return nullptr;
    }

    bool opened = false;
    if (!o.openPath.isEmpty() && QFileInfo::exists(o.openPath)) {
        splash->setStatus("Initializing project...", QFileInfo(o.openPath).fileName());
        QCoreApplication::processEvents();
        opened = w->openProject(o.openPath);
    } else if (o.offerRecovery) {
        splash->setStatus("Looking for an auto-saved project...");
        QCoreApplication::processEvents();
        opened = w->offerRecovery();
    }
    if (!opened && o.openPath.isEmpty() && vprefs::on("prefs/openLastProject")) {
        const QString last = QSettings().value("prefs/projectOpenAtExit").toString();
        if (!last.isEmpty() && QFileInfo::exists(last)) {
            splash->setStatus("Initializing project...", QFileInfo(last).fileName());
            QCoreApplication::processEvents();
            opened = w->openProject(last);
        }
    }

    splash->setStatus("Ready");
    splash->setProgress(1, 1);
    w->show();
    if (opened) w->timeline()->zoomToFit();
    QCoreApplication::processEvents();
    splash->close();
    delete splash;
    if (background) watchBackgroundScan(w);
    if (!haveCache && !fileExists(exe)) QTimer::singleShot(100, w, [w] { runPluginScan(w, false); });
    return w;
}

}  // namespace app
}  // namespace reno
