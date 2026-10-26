// VEGAS's Dashboard (View > Window > Dashboard, Ctrl+Alt+8): tiles for the time at the cursor, preview frame rate, master peak
// meters, project properties and counts, recording, and disk space. Tiles can be hidden from the right-click menu (kept between runs).
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

class Playback;
class Recorder;

class Dashboard : public QWidget {
public:
    Dashboard(Playback* playback, Recorder* recorder, QWidget* parent = nullptr);
    ~Dashboard() override;

    enum Tile { Timecode, FrameRate, MasterPeak, ProjectInfo, Recording, Disk, TileCount };
    void setTileShown(Tile t, bool on);
    bool tileShown(Tile t) const;
    // For tests: the text a tile shows now, and the measured frame rate.
    QString tileText(Tile t) const;
    double measuredFps() const;
    void update1();                         // reads everything now (the window does it 10 times a second)

protected:
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
