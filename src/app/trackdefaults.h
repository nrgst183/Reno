// VEGAS's Set Default Track Properties (track header right-click menu): the chosen properties of a track
// (audio: volume, height, track effects; video: compositing mode, height, track effects) become what new
// tracks of that kind start with. "Restore original defaults" forgets them. Saved in QSettings "trackDefaults/...".
#pragma once
#include <QString>
#include <QStringList>
#include <cstdint>

class QMenu;
class QWidget;

namespace reno {
namespace app {

void installTrackDefaults();
bool runSetDefaultTrackProperties(QWidget* parent, uint64_t trackId);
void addTrackDefaultsItem(QMenu* menu, uint64_t trackId, QWidget* parent);

// For tests: the saved properties for audio (false) or video (true) tracks ("volume", "height", "fx",
// "composite"), and saving them without the dialog (empty list restores the defaults).
QStringList savedTrackDefaults(bool video);
void saveTrackDefaults(uint64_t trackId, const QStringList& properties);

}  // namespace app
}  // namespace reno
