// Reno's built-in audio effect pages for the effects window, hand-built in the style of VEGAS's DirectX effect pages (classic grey,
// vertical gain sliders, a graph in a white grid box, horizontal sliders with edit boxes, radio buttons, check boxes). Each control is
// one of the effect's settings (src/audiofx/builtinaudio.cpp), read and written only through DxHost.
#pragma once
#include <QString>
#include <QWidget>
#include <functional>
#include <string>

namespace reno {
namespace app {

// What a page needs of the plug-in. Values are real ones (dB, Hz, ms, a choice's index).
struct DxHost {
    std::function<bool(int, double*, double*)> range;      // a setting's smallest and biggest value
    std::function<double(int)> get;
    std::function<void(int, double)> set;                    // one undo step, unless a drag is open
    std::function<std::string(int)> name;                    // setting's name (for the undo step)
    std::function<void(int)> begin;                          // a drag starts: one undo step until end()
    std::function<void()> end;
};

class DxPage : public QWidget {
public:
    explicit DxPage(QWidget* parent) : QWidget(parent) {}
    virtual void refresh() = 0;                              // show the plug-in's values again
};

// True for effects that have a page (built-in ones: uid "reno.distortion" ...).
bool hasBuiltinPage(const std::string& uid);
// The page of effect 'uid' (null: none). Controls are named dxParam<index> (slider, check box, combo box),
// dxParam<index>Edit (box beside a slider) and dxParam<index>_<choice> (radio buttons).
DxPage* makeBuiltinPage(QWidget* parent, const std::string& uid, const DxHost& host);

}  // namespace app
}  // namespace reno
