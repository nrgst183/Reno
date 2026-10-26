// The audio mixer: VEGAS's Mixing Console (a strip per track, bus and master with meters, faders, pan, mute/solo,
// effects and routing) and the small Master Bus panel.
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

class QMenu;
class QAction;

namespace reno {
namespace app {

class Playback;

class MixerWindow : public QWidget {
public:
    explicit MixerWindow(Playback* playback, QWidget* parent = nullptr);
    ~MixerWindow() override;
    // The user asked for the effects of a track, a bus (FxTarget kind Bus) or the master.
    std::function<void(const FxTarget& target, uint64_t slotId)> onOpenFx;
    // A track strip's Arm for Record button (the main window arms through the Recorder; unset: AppState::armedTracks).
    std::function<void(uint64_t trackId)> onArmTrack;
    // The Dim Output button (Dim Master Bus Output, Ctrl+Shift+F12; unset: Playback::setDim).
    std::function<void(bool on)> onDim;
    // Properties > Audio Properties (the main window's Project Properties).
    std::function<void()> onAudioProperties;
    // Double-click on a strip's small surround panner (5.1 projects): opens the Surround Panner for that track or bus.
    std::function<void(uint64_t id)> onOpenSurround;

    // The console's own keys as VEGAS's (they come before the main window's shortcuts while it has focus): F2 rename the
    // selected channel,
    // Ctrl+Q insert an audio track, D / N / W default, narrow and wide channels, Shift+H hide selected channels, Shift+C Channel List,
    // Shift+R View Controls, Q show all, U buses, A audio tracks, T master, E assignable FX and P preview bus (Reno has neither),
    // Shift+L control region labels, Shift+T fader ticks, F faders, I insert FX, H I/O, M meters, S sends, Up / Down
    // selected faders half a dB.
    // True if 'key' is one of them (and was done).
    bool handleKey(int key, Qt::KeyboardModifiers mods);

protected:
    bool event(QEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    bool eventFilter(QObject* o, QEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// Insert > Audio Assignable FX (and the console's Insert Assignable FX): a new assignable effects chain (a Bus with
// 'assignable', named FX 1, FX 2 ...). One undo step. Returns its id.
uint64_t insertAssignableFx();
// VEGAS's Downmix Output menu (console and Master Bus window): 5.1 Surround / Stereo / Mono Output. Stereo is
// Preferences' downmixOutput (from the next playback start); Mono is Playback's.
void fillDownmixMenu(QMenu* m, Playback* pb);
// Tests: the console's insert FX and FX button menus are passed here instead of shown (return the item to trigger, or null).
std::function<QAction*(QMenu*)>& mixerMenuHookForTests();

// VEGAS's Master Bus window: master fader and meters, docked beside the preview.
class MasterBusWidget : public QWidget {
public:
    explicit MasterBusWidget(Playback* playback, QWidget* parent = nullptr);
    ~MasterBusWidget() override;
    std::function<void(const FxTarget& target, uint64_t slotId)> onOpenFx;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
