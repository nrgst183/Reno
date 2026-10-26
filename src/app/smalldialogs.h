// Small VEGAS dialogs that replace plain input boxes.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class QWidget;

namespace reno {
namespace app {

// VEGAS's Paste Repeat (Edit > Paste Repeat, Ctrl+B): how many times, and the spacing: end to end (each copy where
// the one before ends) or even (a copy every so many seconds, frames, measures or beats from the cursor). The last
// answers are offered again (QSettings pasteRepeat).
struct PasteRepeatChoice {
    int times = 2;
    bool even = false;
    double every = 1.0;
};
bool askPasteRepeat(QWidget* parent, PasteRepeatChoice& out);
extern std::function<void(QWidget*)> pasteRepeatDialogShown;

}  // namespace app
}  // namespace reno

class QMainWindow;
class QString;

namespace reno {
namespace app {

// VEGAS's Options > Export Preferences / Import Preferences: Reno's settings (preferences, keyboard map, track
// defaults, the
// windows' remembered options, all but the main window's size and place) to an .ini file and back on another machine.
int exportPreferences(const QString& path);                         // settings written, -1 if the file could not be
// settings read, -1 if not a Reno preference file
int importPreferences(QMainWindow* window, const QString& path, QString* error = nullptr);
void addPreferenceFileItems(QMainWindow* window);

// VEGAS's Insert > Slideshow... (Slideshow Creator): images placed one after another on a video track, each the same length
// (Time per image, or Total length shared out), overlapping by Overlap seconds, with an Effect: Fades (cross-fades)
// or Random.
struct SlideshowSettings {
    std::vector<std::string> files;
    bool useTotal = false;
    double perPicture = 5.0, total = 60.0, overlap = 1.0;
    std::string effect = "Fades";
    bool atCursor = true;
    unsigned seed = 1;
};
const std::vector<std::string>& slideshowEffects();
// Makes the slideshow (one undo step, "Insert Slideshow"): the events made, in order (empty with *error if none
// of the files could be read). Files that cannot be read are left out and named in *skipped.
std::vector<uint64_t> makeSlideshow(const SlideshowSettings& s, std::string* error, std::vector<std::string>* skipped = nullptr);
// The Slideshow Creator dialog (Insert > Slideshow...). For tests: called with the dialog when it is shown.
void runSlideshowCreator(QWidget* parent);
extern std::function<void(QWidget*)> slideshowDialogShown;

}  // namespace app
}  // namespace reno
