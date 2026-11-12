// The touch interface's timeline. The playhead stays in the middle and the tracks move under it, as phone editors do:
// a finger dragged across the tracks scrubs (and flicking keeps going), two fingers pinch to zoom about the playhead.
// A tap selects an event (an empty place clears the selection); a selected event shows handles at its edges to trim
// it and moves when dragged; holding a finger on an unselected event picks it up to move it. Moves and trims snap to
// the playhead, other events' edges and markers. Edits go through AppState's drag steps, so each is one undo step.
#pragma once
#include <QWidget>
#include <cstdint>
#include <functional>
#include <memory>

namespace reno {
namespace app {

class Playback;

class TouchTimeline : public QWidget {
public:
    explicit TouchTimeline(Playback* playback, QWidget* parent = nullptr);
    ~TouchTimeline() override;

    void setTablet(bool tablet);                // taller tracks
    double pixelsPerSecond() const;
    void setPixelsPerSecond(double pps);        // clamped
    void zoomToFit();                           // the whole project in view
    void stopFling();
    // Select several: taps add events to the selection or take them out of it, instead of selecting one alone.
    void setMultiSelect(bool on);
    bool multiSelect() const;

    std::function<void(uint64_t trackId)> onTrackTapped;    // the track's header
    std::function<void(uint64_t eventId)> onEventDoubleTapped;
    std::function<void()> onAddTrack;                       // the "+" row under the tracks
    std::function<void()> onEmptyTapped;                    // the hint shown in an empty project

    // Where things are drawn (widget coordinates; for tests and the shell's pop-ups).
    QRectF eventRect(uint64_t eventId) const;
    QRectF handleRect(uint64_t eventId, bool left) const;   // what a finger can grab, empty if not selected
    QRectF trackRect(uint64_t trackId) const;               // the track's row, header included
    double playheadX() const;
    double xAtTime(double seconds) const;
    double timeAtX(double x) const;

protected:
    bool event(QEvent* e) override;
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
