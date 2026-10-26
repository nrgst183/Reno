// Audio proxy and preview proxy progress on the status bar; see audioproxyui.h.
#include "audioproxyui.h"
#include "appstate.h"
#include "autoproxy.h"
#include "prefsapply.h"
#include "vprefs.h"
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QPointer>
#include <QProgressBar>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>
#include <set>

namespace reno {
namespace app {

namespace {
// Status bar piece: "Building audio proxy for x.veg", a bar and a cancel button. Hidden when idle.
class ProxyProgress : public QWidget {
public:
    explicit ProxyProgress(QWidget* parent) : QWidget(parent)
    {
        setObjectName("audioProxyProgress");
        QHBoxLayout* l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 6, 0);
        l->setSpacing(4);
        cancel_ = new QToolButton(this);
        cancel_->setObjectName("cancelAudioProxy");
        cancel_->setText(QString::fromUtf8("×"));
        cancel_->setAutoRaise(true);
        cancel_->setFixedSize(16, 16);
        cancel_->setToolTip("Cancel building the audio proxy (Tools > Rebuild Offline Nested Audio builds it later)");
        bar_ = new QProgressBar(this);
        bar_->setObjectName("audioProxyBar");
        bar_->setRange(0, 1000);
        bar_->setTextVisible(false);
        bar_->setFixedSize(90, 10);
        text_ = new QLabel(this);
        text_->setObjectName("audioProxyText");
        l->addWidget(cancel_);
        l->addWidget(bar_);
        l->addWidget(text_);
        QObject::connect(cancel_, &QToolButton::clicked, this, [] {
            stopAudioProxyBuilds(false, true);
            AppState::get().status("Building the audio proxy was cancelled.");
        });
        hide();
    }

    void update(const std::string& path, double fraction, int state, const std::string& message)
    {
        if (state == 0) {
            text_->setText(QString("Building audio proxy for %1").arg(QString::fromStdString(pathFileName(path))));
            bar_->setValue((int)std::lround(std::min(1.0, std::max(0.0, fraction)) * 1000));
            show();
            return;
        }
        hide();
        if (state == 1) AppState::get().status("Audio proxy built for " + pathFileName(path) + ".");
        else if (state == 2) AppState::get().status("The audio proxy for " + pathFileName(path) + " could not be built: " + message);
    }

private:
    QToolButton* cancel_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* text_ = nullptr;
};

// Preview proxy piece (autoproxy.h): "Making preview proxies: 2 of 5 - name", a bar and a button that turns automatic
// proxies off. Hidden when idle.
class VideoProxyProgress : public QWidget {
public:
    explicit VideoProxyProgress(QWidget* parent) : QWidget(parent)
    {
        setObjectName("videoProxyProgress");
        QHBoxLayout* l = new QHBoxLayout(this);
        l->setContentsMargins(0, 0, 6, 0);
        l->setSpacing(4);
        cancel_ = new QToolButton(this);
        cancel_->setObjectName("cancelVideoProxy");
        cancel_->setText(QString::fromUtf8("×"));
        cancel_->setAutoRaise(true);
        cancel_->setFixedSize(16, 16);
        cancel_->setToolTip("Stop making preview proxies and turn automatic proxies off (Options > Preferences > Video)");
        bar_ = new QProgressBar(this);
        bar_->setObjectName("videoProxyBar");
        bar_->setRange(0, 1000);
        bar_->setTextVisible(false);
        bar_->setFixedSize(90, 10);
        text_ = new QLabel(this);
        text_->setObjectName("videoProxyText");
        l->addWidget(cancel_);
        l->addWidget(bar_);
        l->addWidget(text_);
        QObject::connect(cancel_, &QToolButton::clicked, this, [] {
            vprefs::set("prefs/autoProxyPreview", false);
            vprefs::set("prefs/autoProxy4k", false);
            vprefs::set("prefs/autoProxyLongGop", false);
            autoproxy::Worker::get().setModes(false, false, false);
            AppState::get().status("Automatic preview proxies are off (Options > Preferences > Video).");
        });
        timer_ = new QTimer(this);
        timer_->setInterval(400);
        QObject::connect(timer_, &QTimer::timeout, this, [this] { refresh(); });
        timer_->start();
        hide();
    }
    // Public for the tests.
    void refresh()
    {
        const autoproxy::Status st = autoproxy::Worker::get().status();
        if (!st.enabled || !st.working) {
            hide();
            return;
        }
        QString t = QString("%1: %2 of %3").arg(st.paused ? "Preview proxies paused while playing" : "Making preview proxies").arg(std::min(st.total, st.done + 1)).arg(st.total);
        t += " - " + QString::fromStdString(st.current);
        text_->setText(t);
        bar_->setValue((int)std::lround(std::min(1.0, std::max(0.0, st.fraction)) * 1000));
        show();
    }

private:
    QToolButton* cancel_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* text_ = nullptr;
    QTimer* timer_ = nullptr;
};

QPointer<ProxyProgress> gProgress;
std::set<std::string> gAsked;           // nested projects of the open project already asked for

// Project files the open project uses as media.
std::vector<std::string> nestedOfProject()
{
    std::vector<std::string> out;
    for (const MediaItem& m : AppState::get().project().media)
        if (!m.generated && !m.offline && isProjectFile(m.path)) out.push_back(m.path);
    return out;
}
}  // namespace

void installAudioProxyUi(QMainWindow* w)
{
    ProxyProgress* p = new ProxyProgress(w);
    gProgress = p;
    w->statusBar()->insertWidget(0, p);
    // preview proxies: Preferences control the worker; progress shows beside the audio one
    w->statusBar()->insertWidget(1, new VideoProxyProgress(w));
    autoproxy::Worker::get().setPreferencesControl(true);
    autoproxy::Worker::get().setModes(vprefs::on("prefs/autoProxyPreview"), vprefs::on("prefs/autoProxy4k"), vprefs::on("prefs/autoProxyLongGop"));
    autoproxy::Worker::get().projectChanged(AppState::get().snapshot());
    setAudioProxyProgressHook([](const std::string& path, double fraction, int state, const std::string& message) {
        // (called on the builder's thread, shown on the interface thread)
        AppState::post([path, fraction, state, message] {
            if (gProgress) gProgress->update(path, fraction, state, message);
        });
    });
    // Nested projects get proxies as soon as they are in the project; the builder builds missing or stale ones.
    AppState::get().addListener([](int changes) {
        if (!(changes & (kProject | kFile | kPrefs))) return;
        AppState& app = AppState::get();
        if (changes & kFile) gAsked.clear();
        if (!app.prefs.createAudioProxies) return;
        for (const std::string& path : nestedOfProject())
            if (gAsked.insert(path).second) buildAudioProxy(path, false);
    });
}

void rebuildOfflineNestedAudio()
{
    AppState& app = AppState::get();
    const std::vector<std::string> need = nestedProjectsNeedingProxies(app.project());
    if (need.empty()) {
        app.status(nestedOfProject().empty() ? "Rebuild Offline Nested Audio: the project has no nested projects."
                                             : "Rebuild Offline Nested Audio: all nested audio proxies are up to date.");
        return;
    }
    for (const std::string& path : need) buildAudioProxy(path, true);
    app.status(strfmt("Rebuilding the audio proxies of %d nested project%s.", (int)need.size(), need.size() == 1 ? "" : "s"));
}

void stopAudioProxies(bool serve)
{
    autoproxy::Worker::get().stopAndWait(serve ? 3.0 : 2.0);        // stops a preview proxy being made, removing its part-written file
    stopAudioProxyBuilds(false);
    if (!serve) {
        waitForAudioProxies(2.0);
        return;
    }
    // (the builder may be waiting on this thread for a nested project's effects)
    for (int k = 0; k < 200 && !waitForAudioProxies(0.05); ++k) QCoreApplication::processEvents();
}

}  // namespace app
}  // namespace reno
