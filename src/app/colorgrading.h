// VEGAS's Color Grading panel (View > Window > Color Grading, Alt+G; Alt+D, C arranges the window for it): colour wheels
// (Lift, Gamma, Gain,
// Offset; drag towards a colour, slider below for overall amount), sliders (Exposure, Contrast, Saturation, Temperature,
// Tint) and curves (Master,
// Red, Green, Blue: click to add a point, drag, double-click to remove). It grades the selected video event, video track
// or the video output through
// Reno's built-in "Color Grading" effect (src/engine/builtinfx.h), added at the end of that one's effects on the first
// change. A drag is one undo
// step; Reset restores everything. Double-click a wheel or a slider's label to reset it.
#pragma once
#include "appstate.h"
#include <QImage>
#include <QRectF>
#include <QWidget>
#include <functional>
#include <memory>

class QHBoxLayout;

namespace reno {
namespace app {

class ColorGradingPanel : public QWidget {
public:
    explicit ColorGradingPanel(QWidget* parent = nullptr);
    ~ColorGradingPanel() override;

    enum Apply { SelectedEvent, SelectedTrack, VideoOutput };
    void setApplyTo(Apply a);
    Apply applyTo() const;
    // What is being graded (None if nothing suitable is selected) and the grading effect on it (0: none yet).
    FxTarget target() const;
    uint64_t slot() const;
    // Shows the grading effect in 'slotId' (from the Video FX window's button) and grades what it is on.
    void showSlot(uint64_t slotId, const FxTarget& target);

    // Changes a setting the way the controls do (one undo step each). Names: "exposure", "contrast", "saturation",
    // "temperature", "tint"; wheels "lift", "gamma", "gain", "offset" with x, y, master.
    void setValue(const std::string& name, double v);
    void setWheel(const std::string& name, double x, double y, double master);
    void setCurve(int channel, const std::vector<std::pair<double, double>>& points);     // 0 master, 1 r, 2 g, 3 b
    void resetAll();
    Json state() const;                     // the grade as it is now
    // VEGAS's section 3 buttons besides Bypass and Reset All.
    // Export LUT: the grade as a 3D LUT (.cube, 33 points a side) readable by other programs and VEGAS's LUT Filter.
    // False with *error if it could not be written.
    bool exportLut(const std::string& path, std::string* error) const;
    // Bezier Mask and Broadcast Colors: VEGAS's plug-in added to what is graded (when installed), and opened.
    bool addPlugin(const std::string& pluginId);
    std::function<void(const FxTarget&, uint64_t slotId)> onOpenFx;     // opens the effects window on a new effect

    // --- VP23: the pages after the wheels and curves. setField names: blackIn whiteIn blackOut whiteOut brightness contrastCenter logExposure hue
    // hslSaturation luminance rlLow rlHigh. A LUT source is "preset:<id>" or "file:<path>" ("" none). pickColour: which
    // 1 black point, 2 white point,
    // 3 neutral tone, from the image 'region'.
    void setField(const std::string& name, double v);
    void setLut(bool camera, const std::string& source, double strength, bool tetrahedral);
    void setHslCurve(int kind, const std::vector<std::pair<double, double>>& points);
    void pickColour(int which, const QImage& region);
    // Set by the main window: gives the preview window the function that takes the mouse over the image while a picker
    // button is on (empty function removes it).
    std::function<void(std::function<void(int state, const QRectF& area, const QImage& region)>)> setPickerHook;

    // Pin/Unpin (VEGAS 18's button in the Finishing column): pinned, the panel keeps grading what it grades now instead
    // of following the selection (top line says "(pinned)"); changing "Grade:" or deleting it unpins.
    void setPinned(bool on);
    bool pinned() const;
    // Auto Color Balance menu (0 colour balance, 1 black point, 2 white point): the main window gives the current image here.
    void autoAdjust(int kind);
    std::function<QImage()> frameSource;

    // For tests: the widgets.
    QWidget* wheel(int k) const;            // 0 lift .. 3 offset
    QWidget* curveEditor() const;

protected:
    bool eventFilter(QObject* o, QEvent* e) override;  // a double-click on a slider's label resets it

private:
    void buildPages(QHBoxLayout* root);
    void pickedRegion(int state, const QRectF& area, const QImage& region);
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
