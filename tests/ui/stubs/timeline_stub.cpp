// Stand-in for src/app/timeline.cpp, so the other parts of the interface can be built and tried without it.
// Every function timeline.h declares is here: a flat grey area 50 pixels a second, tracks 60 pixels high under a
// 40-pixel ruler, headers 160 pixels wide; edits do nothing.
#include "../../../src/engine/timefmt.h"
#include "../../../src/app/timeline.h"
#include <QPainter>
#include <cstdio>
namespace reno { namespace app {
struct TimelineWidget::Impl {
    TimelineOptions options;
    int header = 160;
    double start = 0, pps = 50;
};
TimelineWidget::TimelineWidget(Playback*, QWidget* parent) : QWidget(parent), d(new Impl()) { setMinimumHeight(200); setFocusPolicy(Qt::StrongFocus); }
TimelineWidget::~TimelineWidget() {}
void TimelineWidget::update() { QWidget::update(); }
void TimelineWidget::keyReleaseEvent(QKeyEvent*) {}
int TimelineWidget::headerWidth() const { return d->header; }
void TimelineWidget::setHeaderWidth(int w) { d->header = w; if (onHeaderWidthChanged) onHeaderWidthChanged(w); update(); }
TimelineOptions& TimelineWidget::options() { return d->options; }
void TimelineWidget::optionsChanged() { if (onOptionsChanged) onOptionsChanged(); update(); }
void TimelineWidget::zoomIn() { d->pps *= 1.25; update(); }
void TimelineWidget::zoomOut() { d->pps /= 1.25; update(); }
void TimelineWidget::zoomToFit() {}
void TimelineWidget::zoomLarge(bool in) { d->pps *= in ? 2.0 : 0.5; update(); }
void TimelineWidget::scrollToCursor() {}
void TimelineWidget::centerOnCursor() {}
double TimelineWidget::viewStartTime() const { return d->start; }
double TimelineWidget::viewEndTime() const { return timeAt(width()); }
void TimelineWidget::deleteSelection() {}
void TimelineWidget::cutSelection() {}
void TimelineWidget::pasteInsert() {}
void TimelineWidget::trimStartToCursor() {}
void TimelineWidget::trimEndToCursor() {}
void TimelineWidget::trimToTimeSelection() {}
void TimelineWidget::nudgeSelection(int, int, int) {}
void TimelineWidget::insertMarker() {}
void TimelineWidget::insertRegion() {}
void TimelineWidget::goToMarker(int) {}
void TimelineWidget::cursorToMarker(int) {}
void TimelineWidget::cursorToEventEdge(int) {}
void TimelineWidget::cursorToEditPoint(int) {}
void TimelineWidget::cursorToSelectedEdge(int) {}
void TimelineWidget::timeSelectSelectedEvent(int) {}
void TimelineWidget::renameSelected() {}
void TimelineWidget::clearGroups() {}
void TimelineWidget::selectGroups() {}
void TimelineWidget::changeTrackHeights(int) {}
void TimelineWidget::toggleMinimizeTrack() {}
void TimelineWidget::resetTrackHeights() {}
void TimelineWidget::zoomToFrames() {}
bool TimelineWidget::cancelMouseDrag() { return false; }
double TimelineWidget::timeAt(int x) const { return d->start + (x - d->header) / d->pps; }
int TimelineWidget::xAt(double s) const { return d->header + (int)((s - d->start) * d->pps); }
QRect TimelineWidget::trackRect(uint64_t trackId) const
{
    const auto& tracks = AppState::get().project().tracks;
    for (size_t k = 0; k < tracks.size(); ++k)
        if (tracks[k].id == trackId) return QRect(d->header, 40 + (int)k * 60, std::max(0, width() - d->header), 60);
    return QRect();
}
QRect TimelineWidget::headerRect(uint64_t trackId) const
{
    const QRect r = trackRect(trackId);
    return r.isEmpty() ? QRect() : QRect(0, r.top(), d->header, r.height());
}
QRect TimelineWidget::headerPartRect(uint64_t, HeaderPart) const { return QRect(); }
QRect TimelineWidget::eventButtonRect(uint64_t, bool) const { return QRect(); }
QRect TimelineWidget::eventMenuRect(uint64_t) const { return QRect(); }
QRect TimelineWidget::markerTagRect(uint64_t) const { return QRect(); }
QRect TimelineWidget::loopBarRect() const { return QRect(d->header, 0, std::max(0, width() - d->header), 12); }
QRect TimelineWidget::markerBarRect() const { return QRect(d->header, 12, std::max(0, width() - d->header), 12); }
QRect TimelineWidget::rulerRect() const { return QRect(d->header, 24, std::max(0, width() - d->header), 16); }
QRect TimelineWidget::timeDisplayRect() const { return QRect(0, 0, d->header, 40); }
void TimelineWidget::paintEvent(QPaintEvent*) { QPainter p(this); p.fillRect(rect(), QColor(40, 40, 42)); p.setPen(Qt::gray); p.drawText(rect(), Qt::AlignCenter, "timeline (stand-in)"); }
void TimelineWidget::resizeEvent(QResizeEvent*) {}
void TimelineWidget::mousePressEvent(QMouseEvent*) {}
void TimelineWidget::mouseMoveEvent(QMouseEvent*) {}
void TimelineWidget::mouseReleaseEvent(QMouseEvent*) {}
void TimelineWidget::mouseDoubleClickEvent(QMouseEvent*) {}
void TimelineWidget::wheelEvent(QWheelEvent*) {}
void TimelineWidget::keyPressEvent(QKeyEvent*) {}
bool TimelineWidget::focusNextPrevChild(bool next) { return QWidget::focusNextPrevChild(next); }
void TimelineWidget::contextMenuEvent(QContextMenuEvent*) {}
void TimelineWidget::dragEnterEvent(QDragEnterEvent*) {}
void TimelineWidget::dragMoveEvent(QDragMoveEvent*) {}
void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) {}
void TimelineWidget::dropEvent(QDropEvent*) {}
void TimelineWidget::leaveEvent(QEvent*) {}
bool parseRulerTime(const Project& p, const QString& text, double* seconds)
{
    double shown = 0;
    if (!parseTimeText(p.settings, text.toStdString(), p.settings.rulerFormat, &shown)) return false;
    *seconds = shown - p.settings.rulerStart;
    return true;
}
void editTextInPlace(QWidget*, const QRect&, const QString&, std::function<void(const QString&)>) {}
void TimelineWidget::setTimeAtCursor() {}
std::string rulerTime(const Project& p, double seconds)
{
    return formatTime(p.settings, seconds + p.settings.rulerStart, p.settings.rulerFormat);
}
}}
