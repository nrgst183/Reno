// The fonts VEGAS Pro draws its main window with, measured from a 100% screenshot of VEGAS Pro 18 (1920x1041):
// the window's own text (tabs, preview information, track header texts, Rate, cursor position, status line) is Tahoma
// 8.25 pt = 11 px at 96 dpi; the master bus scale and the timeline ruler labels are Tahoma 9 px, the label above the
// time display 10 px, the big time display Tahoma 20 px (15 pt). Only the menu bar is Windows's own menu font, Segoe UI
// 9 pt (12 px). Sizes are in pixels of a 96 dpi screen: Qt scales pixel sizes with the screen's device pixel ratio (and
// point sizes with its logical dpi), so at 125 % the 11 px text is 13.75 px, as VEGAS's 8.25 pt is.
// Other systems have no Tahoma and their stand-in fonts are wider, so there each size is one pixel smaller.
#pragma once
#include <QFont>
#include <QStringList>
#include <algorithm>

namespace reno {
namespace app {

inline int vegasPx(int px)
{
#ifdef _WIN32
    return px;
#else
    return std::max(7, px - 1);
#endif
}

// VEGAS's window font at 'px' (96 dpi pixels): Tahoma, with Segoe UI and the system sans-serif if it is missing.
inline QFont vegasUiFont(int px = 11, bool bold = false)
{
    QFont f;
    f.setFamilies(QStringList() << "Tahoma" << "Segoe UI" << "MS Shell Dlg 2");
    f.setStyleHint(QFont::SansSerif);   // what a system with none of them (Linux) draws instead
    f.setPixelSize(vegasPx(px));
    f.setBold(bold);
    return f;
}

}  // namespace app
}  // namespace reno
