// Shared definitions for the panels docked round the timeline; the panels themselves are in panels_*.cpp.
#include "panels_internal.h"

namespace reno {
namespace app {

const char* const kMimeMedia = "application/x-reno-media";
const char* const kMimeVideoFx = "application/x-reno-videofx";
const char* const kMimeAudioFx = "application/x-reno-audiofx";
const char* const kMimeGenerator = "application/x-reno-generator";
const char* const kMimeTransition = "application/x-reno-transition";
std::function<void(uint64_t mediaId, const QString& path)> openInTrimmer;
std::function<void(uint64_t mediaId, const QString& path, bool start)> previewMedia;
std::function<void(const QString& path)> openProjectFile;
std::function<void()> onProxiesMade;

Playback* gPlayback = nullptr;

}  // namespace app
}  // namespace reno
