// Preset thumbnails drawn by the real effects (see fxthumbs.h).
#include "fxthumbs.h"
#include "playback.h"
#include "../engine/fxruntime.h"
#include "../ofx/ofxhost.h"
#include "../ofx/vegas_ext.h"
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <cmath>
#include <cstring>
#include <map>

namespace reno {
namespace app {

namespace {

std::map<std::string, QImage>& cache()
{
    static std::map<std::string, QImage> c;
    return c;
}

// A QImage as the host's 8-bit image (BGRA, bottom row first).
ImagePtr toImage(const QImage& in)
{
    const QImage q = in.convertToFormat(QImage::Format_ARGB32);
    ImagePtr img = Image::make(q.width(), q.height(), PixelDepth::Byte, false);
    for (int y = 0; y < q.height(); ++y) std::memcpy(img->row(q.height() - 1 - y), q.constScanLine(y), (size_t)q.width() * 4);
    return img;
}

}  // namespace

QImage samplePicture(const QSize& size, double time, int which)
{
    QImage img(size, QImage::Format_ARGB32);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double w = size.width(), h = size.height();
    // the sky
    QLinearGradient sky(0, 0, 0, h);
    if (which == 0) {
        sky.setColorAt(0, QColor(70, 130, 210));
        sky.setColorAt(0.65, QColor(250, 205, 150));
    } else {
        sky.setColorAt(0, QColor(40, 30, 90));
        sky.setColorAt(0.65, QColor(220, 90, 110));
    }
    p.fillRect(img.rect(), sky);
    // the sun, going across
    const double sx = w * (0.15 + 0.7 * std::fmod(time / 4.0, 1.0));
    p.setPen(Qt::NoPen);
    p.setBrush(which == 0 ? QColor(255, 236, 140) : QColor(255, 250, 230));
    p.drawEllipse(QPointF(sx, h * 0.3), h * 0.11, h * 0.11);
    // two lines of hills
    for (int layer = 0; layer < 2; ++layer) {
        QPainterPath hill;
        hill.moveTo(0, h);
        for (int k = 0; k <= 40; ++k) {
            const double x = w * k / 40.0;
            const double y = h * (0.62 + layer * 0.12) - h * 0.08 * std::sin(x / w * 6.3 * (layer + 1) + layer * 1.7 + time * 0.3 * (layer + 1));
            hill.lineTo(x, y);
        }
        hill.lineTo(w, h);
        hill.closeSubpath();
        p.setBrush(layer == 0 ? (which == 0 ? QColor(60, 140, 70) : QColor(70, 60, 110)) : (which == 0 ? QColor(30, 95, 45) : QColor(40, 30, 70)));
        p.drawPath(hill);
    }
    // a red post and a white sign: sharp edges and pure colours for the effects
    p.setBrush(QColor(210, 40, 40));
    p.drawRect(QRectF(w * 0.72, h * 0.5, w * 0.03, h * 0.4));
    p.setBrush(QColor(245, 245, 245));
    p.drawRect(QRectF(w * 0.66, h * 0.44, w * 0.15, h * 0.1));
    p.end();
    return img;
}

QImage fxThumbnail(const std::string& pluginId, const std::string& context, const Json& state, const QSize& size, double phase, std::string* error)
{
    const std::string key = pluginId + "|" + context + "|" + state.dump() + "|" + std::to_string(size.width()) + "x" + std::to_string(size.height()) + "|" +
                            std::to_string((int)std::lround(phase * 1000));
    auto it = cache().find(key);
    if (it != cache().end()) return it->second;
    QImage out;
    std::string err;
    ofx::Plugin* pl = findVideoPlugin(pluginId, &err);
    const std::string ctx = "OfxImageEffectContext" + (context.empty() ? std::string("Filter") : context);
    std::string useCtx = ctx;
    if (pl && context == "Filter" && !pl->supportsContext(ctx)) useCtx = pl->filterContext();
    if (pl && !useCtx.empty() && pl->supportsContext(useCtx)) {
        ofx::InstanceSetup setup;
        setup.width = size.width();
        setup.height = size.height();
        setup.fps = 30;
        setup.durationFrames = 120;
        setup.vegasContext = context == "Generator" ? kVegasContextGenerator : context == "Transition" ? kVegasContextEventFadeIn : kVegasContextEvent;
        std::unique_ptr<ofx::Instance> inst = pl->createInstance(useCtx, setup, &err);
        if (inst) {
            if (state.isObject()) inst->loadState(state);
            const double seconds = phase * 4.0;
            const ImagePtr a = toImage(samplePicture(size, seconds, 0)), b = toImage(samplePicture(size, seconds, 1));
            if (context == "Transition") {
                inst->setClipSource(kOfxImageEffectTransitionSourceFromClipName, [a](double) { return a; });
                inst->setClipSource(kOfxImageEffectTransitionSourceToClipName, [b](double) { return b; });
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                if (ofx::Param* tp = inst->params.find(kOfxImageEffectTransitionParamName)) {
                    tp->keys.clear();
                    tp->value.assign((size_t)std::max(1, tp->dim), std::max(0.0, std::min(1.0, phase)));
                }
            } else {
                for (const std::string& clip : inst->inputClipNames()) inst->setClipSource(clip, [a](double) { return a; });
            }
            ofx::RenderOptions opt;
            opt.interactive = true;
            if (ImagePtr r = inst->render(phase * 119.0, opt, &err)) {
                if (r->depth != PixelDepth::Byte) r = r->converted(PixelDepth::Byte);
                if (r) out = toQImage(*r);
            }
        }
    } else if (err.empty()) {
        err = "cannot be used this way";
    }
    if (error) *error = err;
    if (cache().size() > 2000) cache().clear();
    cache()[key] = out;
    return out;
}

}  // namespace app
}  // namespace reno
