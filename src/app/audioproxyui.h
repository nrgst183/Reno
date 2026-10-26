// Audio proxies of nested projects in the interface: status bar progress with a cancel button, proxies built when
// a nested project is opened or added, and Tools > Rebuild Offline Nested Audio. The proxies themselves: engine.h.
#pragma once
class QMainWindow;

namespace reno {
namespace app {

// Puts the progress on 'w's status bar and starts watching the project. Call once, after the status bar exists.
void installAudioProxyUi(QMainWindow* w);
// Tools > Rebuild Offline Nested Audio: builds missing, stale or cancelled proxies.
void rebuildOfflineNestedAudio();
// Reno is closing: stops the build in progress (removing its part-written file). 'serve': keep handling interface events
// while waiting (the builder may need this thread); otherwise waits two seconds at most.
void stopAudioProxies(bool serve = true);

}  // namespace app
}  // namespace reno
