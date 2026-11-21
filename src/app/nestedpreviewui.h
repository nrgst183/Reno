// Preview files of nested projects in the interface: built with a progress window when a project is opened, when Reno
// comes back to the front after a nested project was saved elsewhere, and from Tools > Rebuild Nested Project
// Previews. The files themselves: engine.h and media/nestedcache.h.
#pragma once
class QWidget;

namespace reno {
namespace app {

// Builds the preview files the open project's nested projects are missing or have out of date, deepest first, with
// a progress window over 'owner' (or on the status bar where no window can be shown). Nothing happens if Preferences
// turns them off, a build is already going on, or the user cancelled these very ones before (until a file they are
// made from changes). 'all' (the Tools menu) builds every one again, cancelled or not.
void checkNestedPreviews(QWidget* owner, bool all = false);
// Reno is closing: stops the build going on, handling interface events while it finishes the frame it is on (the builder
// may need this thread), for a few seconds at most.
void stopNestedPreviews();

}  // namespace app
}  // namespace reno
