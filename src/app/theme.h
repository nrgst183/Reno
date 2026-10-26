// The interface's two looks (Options > Preferences > Theme, View > Theme):
//   Reno (default): wide track headers with FX / M / S buttons and labeled sliders, events with a colored title bar,
// the big time display over the headers.
//   VEGAS: VEGAS Pro 18's dark scheme with narrow headers and VEGAS-style events.
#pragma once
#include <QColor>
#include <string>

namespace reno {
namespace app {

enum class UiTheme { Reno, Vegas };

inline UiTheme& uiThemeSetting()
{
    static UiTheme theme = UiTheme::Reno;
    return theme;
}
inline UiTheme uiTheme() { return uiThemeSetting(); }
inline bool renoTheme() { return uiTheme() == UiTheme::Reno; }

inline const char* themeKey(UiTheme t) { return t == UiTheme::Vegas ? "vegas" : "reno"; }
inline UiTheme themeFromKey(const std::string& key) { return key == "vegas" ? UiTheme::Vegas : UiTheme::Reno; }

namespace reno_colors {
const QColor kWindow(45, 45, 48), kBase(30, 30, 32), kMid(62, 62, 66), kAccent(0, 122, 204);
const QColor kAlt(38, 38, 41), kDeep(27, 27, 29), kLine(22, 22, 24), kTitle(55, 55, 58), kRim(90, 90, 96);
}

// The greys VEGAS's look uses for the bars and panels around the timeline and preview, mapped to Reno's when the
// Reno look is in effect (others are left alone), so code written with VEGAS's colors follows the look.
inline QColor themed(const QColor& vegas)
{
    if (!renoTheme() || vegas.red() != vegas.green() || vegas.green() != vegas.blue()) return vegas;
    switch (vegas.red()) {
    case 35: return reno_colors::kDeep;
    case 45: return reno_colors::kAlt;
    case 55: return reno_colors::kWindow;
    case 70: return reno_colors::kTitle;
    case 130: return reno_colors::kRim;
    default: return vegas;
    }
}

}  // namespace app
}  // namespace reno
