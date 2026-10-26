// Preset thumbnails as in VEGAS's Video FX, Transitions and Media Generators windows: each preset drawn by the real effect on
// Reno's own test image (a little moving landscape, so thumbnails can be animated).
#pragma once
#include "../core/json.h"
#include <QImage>
#include <string>

namespace reno {
namespace app {

// Reno's test image at 'size', 'time' seconds into its scene. 'which' 0 or 1 picks one of two images (transitions go
// from first to second).
QImage samplePicture(const QSize& size, double time, int which = 0);

// The effect 'pluginId' with settings 'state' (FxSlot::state form; null: defaults) drawn at 'size' on the test image. 'context' is
// "Filter", "Transition" or "Generator". 'phase' 0..1 is how far through the animation. Null image if the effect cannot draw.
// Main thread. Cached.
QImage fxThumbnail(const std::string& pluginId, const std::string& context, const Json& state, const QSize& size, double phase = 0.5,
                   std::string* error = nullptr);

}  // namespace app
}  // namespace reno
