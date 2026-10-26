// The small square buttons beside the timeline's scroll bars.
#include "timeline_internal.h"

namespace reno {
namespace app {

namespace tl {

BarButton::BarButton(QWidget* parent, const char* name, const QString& tip, std::function<void()> fn, Picture picture)
    : QAbstractButton(parent), picture_(std::move(picture))
{
    setObjectName(name);
    setToolTip(tip);
    setAccessibleName(tip);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAutoRepeat(true);
    QObject::connect(this, &QAbstractButton::clicked, this, [fn] { fn(); });
}

void BarButton::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    if (renoTheme()) p.fillRect(rect(), isDown() ? QColor(0, 122, 204) : (underMouse() ? QColor(62, 62, 66) : QColor(45, 45, 48)));
    else p.fillRect(rect(), isDown() ? QColor(90, 90, 90) : (underMouse() ? QColor(72, 72, 72) : QColor(55, 55, 55)));
    p.setRenderHint(QPainter::Antialiasing, true);
    picture_(p, QRectF(rect()));
}

}  // namespace tl

}  // namespace app
}  // namespace reno
