// The Preferences dialog reads VEGAS's preferences through vprefs' cache. A setting written to QSettings behind the cache's back
// (the Trimmer's Shift+S, "Auto-Save Trimmer Markers/Regions") left the dialog showing the old state and a later Preferences > OK
// writing it back. Everything that changes one of these goes through vprefs::set.
//
//   prefs_cache_test <output folder>
#include "../../src/app/trimmer.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QApplication>
#include <QSettings>

using namespace reno::app;
using uitest::check;

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoPrefsCacheTest");
    QApplication::setApplicationName("prefs_cache_test");
    QSettings().clear();
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    check(!vprefs::on("trimmer/autoSaveMarkers"), "auto-save of Trimmer markers starts off");
    TrimmerWindow tr;
    tr.setAutoSaveMarkers(true);
    check(vprefs::on("trimmer/autoSaveMarkers"), "after Shift+S the Preferences' view of it (vprefs) says on");
    TrimmerWindow again;
    check(vprefs::on("trimmer/autoSaveMarkers"), "a new Trimmer still sees it on");
    tr.setAutoSaveMarkers(false);
    check(!vprefs::on("trimmer/autoSaveMarkers"), "and off again");
    QSettings().clear();
    return uitest::summary();
}
