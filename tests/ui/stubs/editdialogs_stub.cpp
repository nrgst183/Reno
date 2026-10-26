// Stand-in for src/app/editdialogs.cpp: the dialogs do nothing; the compositing modes are the real list.
#include "../../../src/app/editdialogs.h"
namespace reno { namespace app {
void openPanCrop(QWidget*, Playback*, uint64_t) {}
void openTrackMotion(QWidget*, Playback*, uint64_t) {}
void runEventProperties(QWidget*, uint64_t) {}
void runProjectPropertiesFull(QWidget*) {}
// (runPreferences: prefsdialog.cpp, built with mainwindow or named as an extra file)
void runProjectPropertiesAudio(QWidget*) {}
const std::vector<CompositeMode>& compositeModes()
{
    static const std::vector<CompositeMode> modes = {
        {"srcalpha", "Source Alpha"}, {"add", "Add"}, {"subtract", "Subtract"}, {"cut", "Cut"},
        {"multiply", "Multiply (Mask)"}, {"overlay", "Overlay"}, {"screen", "Screen"}, {"hardlight", "Hard Light"},
        {"dodge", "Dodge"}, {"burn", "Burn"}, {"darken", "Darken"}, {"lighten", "Lighten"},
        {"difference", "Difference"}, {"diffsquared", "Difference Squared"},
    };
    return modes;
}
}}
