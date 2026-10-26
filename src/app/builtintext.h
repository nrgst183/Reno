// Text drawing for the built-in Titles & Text generator (ofx/builtins.h); the engine has no fonts, so the application lends Qt's.
// Call once a QGuiApplication exists.
#pragma once

namespace reno {
namespace app {

void installBuiltinTextRasterizer();

}  // namespace app
}  // namespace reno
