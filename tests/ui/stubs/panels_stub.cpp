// Stand-in for src/app/panels.cpp: every function panels.h declares, as labels and buttons that do the least.
#include "../../../src/app/panels.h"
#include "../../../src/app/playback.h"
#include <QLabel>
#include <QVBoxLayout>
namespace reno { namespace app {
const char* const kMimeMedia = "application/x-reno-media";
const char* const kMimeVideoFx = "application/x-reno-videofx";
const char* const kMimeAudioFx = "application/x-reno-audiofx";
const char* const kMimeGenerator = "application/x-reno-generator";
const char* const kMimeTransition = "application/x-reno-transition";
static QLabel* label(QWidget* w, const char* text) { auto* l = new QVBoxLayout(w); QLabel* t = new QLabel(text); l->addWidget(t); return t; }
struct PreviewWidget::Impl {
    PreviewQuality quality = PreviewQuality::Preview;
    PreviewSize size = PreviewSize::Full;
    uint64_t overlay = 0;
    bool showOverlays = true;
    QLabel* picture = nullptr;
};
PreviewWidget::PreviewWidget(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { d->picture = label(this, "preview (stand-in)"); }
PreviewWidget::~PreviewWidget() {}
void PreviewWidget::showEvent(QShowEvent* e) { QWidget::showEvent(e); }
void PreviewWidget::setQuality(PreviewQuality q, PreviewSize s) { d->quality = q; d->size = s; }
PreviewQuality PreviewWidget::quality() const { return d->quality; }
PreviewSize PreviewWidget::size() const { return d->size; }
QSize PreviewWidget::previewSize() const
{
    const ProjectSettings& s = AppState::get().project().settings;
    const int div = d->size == PreviewSize::Half ? 2 : (d->size == PreviewSize::Quarter ? 4 : 1);
    return QSize(s.width / div, s.height / div);
}
void PreviewWidget::setOverlayEffect(uint64_t slotId, const FxTarget&) { d->overlay = slotId; }
uint64_t PreviewWidget::overlayEffect() const { return d->overlay; }
bool PreviewWidget::showOverlays() const { return d->showOverlays; }
void PreviewWidget::setShowOverlays(bool on) { d->showOverlays = on; }
bool PreviewWidget::overlayActive() const { return false; }
bool PreviewWidget::overlayDrawn() const { return false; }
QWidget* PreviewWidget::pictureWidget() const { return d->picture; }
QPointF PreviewWidget::canonicalToView(double, double) const { return QPointF(); }
void PreviewWidget::setChannel(int) {}
int PreviewWidget::channel() const { return 0; }
void PreviewWidget::setSplitScreen(bool, bool) {}
bool PreviewWidget::splitScreen() const { return false; }
bool PreviewWidget::splitClipboard() const { return false; }
void PreviewWidget::setSplitArea(const QRectF&) {}
QRectF PreviewWidget::splitArea() const { return QRectF(0, 0, 0.5, 1); }
bool PreviewWidget::splitPictureReady() const { return false; }
void PreviewWidget::setDisplayOptions(bool, bool, bool) {}
bool PreviewWidget::simulateAspect() const { return false; }
bool PreviewWidget::scaleToFit() const { return true; }
bool PreviewWidget::adjustForPlayback() const { return false; }
void PreviewWidget::setToolbarVisible(bool) {}
bool PreviewWidget::toolbarVisible() const { return true; }
void PreviewWidget::setStatusBarVisible(bool) {}
bool PreviewWidget::statusBarVisible() const { return true; }
QRect PreviewWidget::pictureRect() const { return QRect(); }
struct VideoScopes::Impl { Scope scope = Waveform; Stats stats; };
VideoScopes::VideoScopes(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { label(this, "video scopes (stand-in)"); }
VideoScopes::~VideoScopes() {}
void VideoScopes::setScope(Scope s) { d->scope = s; }
VideoScopes::Scope VideoScopes::scope() const { return d->scope; }
void VideoScopes::setImage(const QImage&) {}
VideoScopes::Stats VideoScopes::stats() const { return d->stats; }
struct MediaExplorer::Impl { QString folder; };
MediaExplorer::MediaExplorer(QWidget* parent) : QWidget(parent), d(new Impl()) { label(this, "explorer (stand-in)"); }
MediaExplorer::~MediaExplorer() {}
void MediaExplorer::setFolder(const QString& path) { d->folder = path; }
QString MediaExplorer::folder() const { return d->folder; }
QStringList MediaExplorer::visibleFiles() const { return QStringList(); }
void MediaExplorer::addFiles(const QStringList&, bool) {}
struct EditDetails::Impl {};
EditDetails::EditDetails(QWidget* parent) : QWidget(parent), d(new Impl()) { label(this, "edit details (stand-in)"); }
EditDetails::~EditDetails() {}
void EditDetails::setShow(Show) {}
int EditDetails::rows() const { return 0; }
QString EditDetails::cell(int, int) const { return QString(); }
struct TransportBar::Impl {};
TransportBar::TransportBar(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { label(this, "transport (stand-in)"); }
TransportBar::~TransportBar() {}
void TransportBar::setRecordHandler(std::function<void()>) {}
void TransportBar::setRecording(bool) {}
std::function<void(uint64_t, const QString&)> openInTrimmer;
std::function<void()> onProxiesMade;
QImage drawWithGL(const QSize&, const QColor&, double, double, double, double, const std::function<void()>&) { return QImage(); }
void stopPlayback(Playback* pb) { if (pb && pb->playing()) pb->stop(); }
struct MediaBin::Impl {};
MediaBin::MediaBin(QWidget* parent) : QWidget(parent), d(new Impl()) { label(this, "media (stand-in)"); }
MediaBin::~MediaBin() {}
void MediaBin::importFiles() {}
void MediaBin::importImageSequence() {}
void MediaBin::setView(int) {}
int MediaBin::view() const { return 0; }
void MediaBin::showBin(const QString&) {}
QString MediaBin::currentBin() const { return "all"; }
void MediaBin::setSearch(const QString&) {}
std::vector<uint64_t> MediaBin::shownMedia() const { return std::vector<uint64_t>(); }
int MediaBin::removeUnusedMedia() { return 0; }
void MediaBin::newBin() {}
void MediaBin::moveToBin(const QString&, const std::vector<uint64_t>&) {}
void MediaBin::saveSmartBin(const QString&) {}
void MediaBin::deleteCurrentBin() {}
bool MediaBin::replaceMedia(uint64_t, const QString&) { return false; }
void MediaBin::refreshMedia(const std::vector<uint64_t>&) {}
void MediaBin::selectTimelineEvents(const std::vector<uint64_t>&) {}
std::function<bool(uint64_t&, double&, double&)> trimmerSelection;
uint64_t createSubclip(uint64_t, double, double, const std::string&) { return 0; }
struct FxBrowser::Impl {};
FxBrowser::FxBrowser(Kind, QWidget* parent) : QWidget(parent), d(new Impl()) { label(this, "plug-ins (stand-in)"); }
FxBrowser::~FxBrowser() {}
void runRenderDialog(QWidget*) {}
void runProjectProperties(QWidget*) {}
void runPluginScan(QWidget*, bool) {}
void watchBackgroundScan(QWidget*) {}
void showAbout(QWidget*) {}
}}
