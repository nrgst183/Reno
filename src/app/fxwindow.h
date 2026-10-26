// The effects window: the effect chain of one thing (event, track, video output, master bus) and the settings of the picked effect.
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

class Playback;

class FxWindow : public QWidget {
public:
    explicit FxWindow(Playback* playback, QWidget* parent = nullptr);
    ~FxWindow() override;

    // Shows the chain of 'target' and picks effect 'slotId' (0: the last, or none if empty).
    void showTarget(const FxTarget& target, uint64_t slotId = 0);
    FxTarget target() const;
    // Shows only effects 'slotIds' of the event's chain (with 'lockChain': no add/remove/reorder buttons). The Take FX window of Apply
    // Non-Real-Time Event FX uses this. An empty list shows the whole chain again.
    void setOnlySlots(const std::vector<uint64_t>& slotIds, bool lockChain);
    // A video event's effect applied before or after Event Pan/Crop (right-click menu; undoable).
    void setEffectBeforePanCrop(uint64_t slotId, bool before);
    // The OpenFX effect whose controls are showing changed (slotId 0: none), for its on-screen controls over the Video Preview.
    std::function<void(uint64_t slotId, const FxTarget& target)> onEffectShown;
    // A built-in effect's button (Color Grading panel's) was pressed: its own panel is wanted.
    std::function<void(uint64_t slotId, const FxTarget& target)> onOpenBuiltin;

protected:
    void closeEvent(QCloseEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
