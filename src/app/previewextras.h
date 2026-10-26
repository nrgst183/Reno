// Video Preview extras: the Closed Captioning overlays (CC1..CC4) and the 360 degree view
// (equirectangular image through a camera; drag to turn, wheel to zoom).
#pragma once
#include <QImage>
#include <QRect>

class QPainter;

namespace reno {
namespace app {

// Draws the captions of channel 'channel' (1..4) at 't' seconds over 'target': CEA-608's
// 32x15 grid inside the title-safe area.
void drawCaptions(QPainter& p, const QRect& target, int channel, double t);

// View of an equirectangular image through a camera turned 'yaw' degrees right and 'pitch' up,
// 'fov' degrees across, rendered at the size of 'out'.
QImage view360(const QImage& equirect, const QSize& out, double yaw, double pitch, double fov);

}  // namespace app
}  // namespace reno
