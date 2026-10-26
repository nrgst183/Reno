// VEGAS's Loudness Meters (View > Window > Loudness Meters, Ctrl+Alt+7): loudness of what is heard, per EBU R 128 / ITU-R BS.1770
// (src/engine/loudness.h). Statistics at the left (Momentary, Short, Integrated, Loudness Range, over-target
// indicators), M, S, I and LRA
// meters and True peak meters at the right. Meters read in LU against the mode's target (EBU R 128: -23 LUFS; ATSC A/85: -24), or in
// LUFS with Absolute, on the EBU +9 or +18 scale. The right-click menu (and Options > Loudness Meters, fillLoudnessMenu) sets these.
// It listens to the master output during playback (Playback::setOutputTap); readings restart when playing starts. Reno's "Measure"
// renders the time selection (or project) the way Render As does and measures it at once.
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

class QMenu;

namespace reno {
namespace app {

class Playback;

class LoudnessWindow : public QWidget {
public:
    explicit LoudnessWindow(Playback* playback, QWidget* parent = nullptr);
    ~LoudnessWindow() override;

    struct Reading {
        double momentary = -70, shortTerm = -70, integrated = -70, range = 0, truePeak = -144;
        double maxMomentary = -70, maxShortTerm = -70, seconds = 0;
    };
    Reading reading() const;
    void reset();                           // Reset Metering Engine
    void resetClip();                       // Reset Clip: true peak indicator (and held peaks)
    // Renders the project's sound from 'from' to 'to' into a fresh meter (progress(fraction) returns false to stop). False if stopped.
    bool measure(double from, double to, const std::function<bool(double)>& progress = nullptr);
    void setTarget(double lufs);            // target (-23 EBU R 128, -24 ATSC A/85: setting one chooses that mode)
    double target() const;
    void setResetOnPlay(bool on);
    bool listening() const;                 // listening to the master output now

    // VEGAS's options (kept between runs; same as Options > Loudness Meters)
    void setAtscMode(bool atsc);            // ATSC A/85 Mode (else EBU R 128)
    bool atscMode() const;
    void setWideScale(bool wide);           // Loudness Scale: EBU +18 (else +9)
    bool wideScale() const;
    void setAbsolute(bool on);              // Absolute (-23 LUFS): LUFS, not LU
    bool absolute() const;
    void setShowTruePeak(bool on);          // Show True Peak Meter
    bool showTruePeak() const;
    void setPeakBlocking(bool on);          // True Peak Blocking Filter
    bool peakBlocking() const;
    bool integratedOver() const;            // over-target indicators
    bool truePeakOver() const;

protected:
    void showEvent(QShowEvent* e) override;
    void hideEvent(QHideEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Loudness Meters options as menu items (window right-click menu and Options > Loudness Meters, refilled on open). 'w'
// may be null: options are still set.
void fillLoudnessMenu(QMenu* menu, LoudnessWindow* w);

}  // namespace app
}  // namespace reno
