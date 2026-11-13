// Reno's built-in audio effect pages for the effects window, hand-built in the style of VEGAS's DirectX effect pages (classic grey,
// vertical gain sliders, a graph in a white grid box, horizontal sliders with edit boxes, radio buttons, check boxes). Each control is
// one of the effect's settings (src/audiofx/builtinaudio.cpp), read and written only through DxHost.
#pragma once
#include <QString>
#include <QStringList>
#include <QWidget>
#include <cstdint>
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
    std::function<std::string()> state;                      // the plug-in's saved form (pages with more than settings)
    std::function<void(const std::string&, const std::string&)> setState;   // (saved form, undo name): one undo step unless a drag is open
    std::function<float(int)> meter;                         // a level meter's biggest value since the last call (AudioPlugin::meterTake)
    std::function<void(const std::string&)> beginEdit;       // like begin(), with the undo step's name
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

// The page of Reno's eFX effects (fxpages_efx.cpp), in the style of VEGAS's MAGIX eFX windows. Values are the plug-in's 0..1.
struct EfxHost {
    std::function<float(int)> get;
    std::function<void(int, float)> set;                     // one undo step, unless a drag or beginEdit() is open
    std::function<void(int)> begin;                          // a drag starts: one undo step until end()
    std::function<void(const std::string&)> beginEdit;       // the same, with the undo step's name
    std::function<void()> end;
    std::function<std::string()> state;                      // A / B compare: the plug-in's saved form
    std::function<void(const std::string&, const std::string&)> setState;
    std::function<float(int)> meter;                         // efx::EfxPlugin::meterTake
    std::function<void()> help;
    // the effects window's preset list: names (empty: a separator), the chosen one (-1: none), pick one (-1: none), save
    std::function<QStringList()> presetNames;
    std::function<int()> presetCurrent;
    std::function<void(int)> presetPick;
    std::function<void()> presetSave;
};
bool hasEfxPage(uint32_t vstId);
// The page (null: the effect has none). The control under the mouse is drawn by the page; it has no child widgets.
DxPage* makeEfxPage(QWidget* parent, uint32_t vstId, const EfxHost& host);

}  // namespace app
}  // namespace reno
