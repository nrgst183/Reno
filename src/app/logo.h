// Reno's logo, drawn in code like VEGAS Pro's icon, with an orange "R" and the version. Used for the window icon, the About box
// and (rendered to resources/reno.ico by tools/make_logo.cpp) the Windows program icon.
#pragma once
#include <QIcon>
#include <QImage>

namespace reno {
namespace app {

// Reno's version ("1.0"), shown in the logo and About box.
const char* renoVersion();
// The logo at size x size pixels. Small sizes leave out the version.
QImage renoLogo(int size);
// The logo at the sizes Windows and Qt ask for.
QIcon renoIcon();

}  // namespace app
}  // namespace reno
