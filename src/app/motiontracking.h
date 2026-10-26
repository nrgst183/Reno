// VEGAS's Motion Tracking (Alt+M): follows something in a video event over time and turns the movement into Track Motion keyframes
// or the event's own Pan/Crop keyframes (to keep it centred or hold it still: stabilizing).
// The window shows the selected video event's media at the cursor. Drag a box round what to follow; Track Forward / Backward follow it
// frame by frame (src/engine/tracker.h) to the end or start of the event (or time selection), or until lost. Then choose where the
// movement goes and Apply (one undo step). Positions are on the event's media; for Track Motion they are taken as the
// media fills the frame (fitted).
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

class Playback;

class MotionTrackingWindow : public QWidget {
public:
    explicit MotionTrackingWindow(Playback* playback, QWidget* parent = nullptr);
    ~MotionTrackingWindow() override;

    uint64_t trackedEvent() const;          // event being tracked (the selected video event), 0 for none
    // The box to follow at the cursor, in fractions of the media's image (centre and size).
    void setBox(double cx, double cy, double w, double h);
    bool hasBox() const;

    struct Point {
        int64_t frame = 0;                  // project frame
        double time = 0;                    // timeline seconds
        double x = 0, y = 0;                // box centre, fractions of the media's image
    };
    // Follows the box from the cursor frame by frame, forward (+1) or backward (-1). progress(fraction) returns false to
    // stop. Returns frames followed; 'lost' says whether it stopped because the box was lost.
    int track(int direction, const std::function<bool(double)>& progress = nullptr, bool* lost = nullptr);
    // VEGAS 18's other track buttons: one frame forward (+1) or back (-1) from the cursor (the cursor moves there), and
    // both directions. They return frames followed.
    int trackOneFrame(int direction);
    int trackBoth(const std::function<bool(double)>& progress = nullptr);
    // VEGAS's Precision: 0 Fast, 1 Balanced (default), 2 Accurate: the size the images are followed at.
    void setPrecision(int p);
    int precision() const;
    std::vector<Point> points() const;      // in time order
    void clearPoints();

    // The movement as keyframes (one undo step each). False if nothing was tracked or no such track.
    bool applyToTrackMotion(uint64_t trackId);
    bool applyToPanCrop(bool stabilize, double zoom);
    // VEGAS 18's motion transfer to an effect: keyframes of a position setting (OpenFX 2D position) of effect 'slotId'
    // on event 'eventId'
    // (the tracked one or one playing at the same time). One undo step. False if nothing was tracked or no such setting.
    bool applyToFxParam(uint64_t eventId, uint64_t slotId, const std::string& paramName);
    // Events motion can be transferred to (the tracked one first, then video events playing at the same time) and each
    // one's effect position settings: (slot id, setting name, "effect: setting" label).
    struct FxTarget2D { uint64_t slot = 0; std::string param, label; };
    std::vector<uint64_t> transferEvents() const;
    std::vector<FxTarget2D> positionSettings(uint64_t eventId) const;

    QWidget* picture() const;               // for tests

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
