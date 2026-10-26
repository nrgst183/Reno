// External Monitor Preview; see externalmonitor.h.
#include "externalmonitor.h"
#include "playback.h"
#include "vprefs.h"
#include <QAction>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QWindow>

namespace reno {
namespace app {

namespace {
// Preferences > Preview Device: "Apply deinterlace filter" (each odd line becomes the mean of its neighbours) and
// "Adjust levels from studio RGB to computer RGB" (16..235 stretched to 0..255).
QImage withDeviceFilters(const QImage& src, bool deinterlace, bool studioLevels)
{
    if (src.isNull() || (!deinterlace && !studioLevels)) return src;
    QImage img = src.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width(), h = img.height();
    if (deinterlace) {
        for (int y = 1; y < h; y += 2) {
            uchar* line = img.scanLine(y);
            const uchar* above = img.scanLine(y - 1);
            const uchar* below = y + 1 < h ? img.scanLine(y + 1) : above;
            for (int k = 0; k < w * 4; ++k) line[k] = (uchar)((above[k] + below[k] + 1) / 2);
        }
    }
    if (studioLevels) {
        uchar lut[256];
        for (int v = 0; v < 256; ++v) lut[v] = (uchar)std::max(0, std::min(255, (v - 16) * 255 / 219));
        for (int y = 0; y < h; ++y) {
            uchar* line = img.scanLine(y);
            for (int x = 0; x < w; ++x, line += 4) {
                line[0] = lut[line[0]];
                line[1] = lut[line[1]];
                line[2] = lut[line[2]];
            }
        }
    }
    return img;
}
}  // namespace

struct ExternalMonitor::Impl {
    Playback* pb = nullptr;
    QWidget* main = nullptr;
    QTimer* timer = nullptr;
    QImage image;
    qint64 key = 0;
    QString wanted;

    QScreen* screen() const
    {
        const QList<QScreen*> all = QGuiApplication::screens();
        if (all.isEmpty()) return nullptr;
        if (!wanted.isEmpty())
            for (QScreen* s : all)
                if (s->name() == wanted) return s;
        // Display adapter preference: a display of its own choosing; -1: one other than Reno's
        const int adapter = vprefs::number("prefs/pdAdapter");
        if (adapter >= 0 && adapter < all.size()) return all[adapter];
        // first display the main window is not on, else the main window's own
        QScreen* mainScreen = main && main->windowHandle() ? main->windowHandle()->screen() : QGuiApplication::primaryScreen();
        for (QScreen* s : all)
            if (s != mainScreen) return s;
        return mainScreen ? mainScreen : all.first();
    }
};

ExternalMonitor::ExternalMonitor(Playback* playback, QWidget* mainWindow) : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), d(new Impl())
{
    setObjectName("externalMonitor");
    setWindowTitle("Reno - External Monitor");
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setCursor(Qt::BlankCursor);
    d->pb = playback;
    d->main = mainWindow;
    d->wanted = QSettings().value("externalMonitor/screen").toString();
    d->timer = new QTimer(this);
    d->timer->setInterval(10);
    Impl* impl = d.get();
    // newest preview image whenever it changes (same as the Video Preview)
    QObject::connect(d->timer, &QTimer::timeout, this, [this, impl] {
        if (!impl->pb) return;
        const QImage img = impl->pb->lastFrame();
        if (img.cacheKey() == impl->key) return;
        impl->key = img.cacheKey();
        impl->image = withDeviceFilters(img, vprefs::on("prefs/pdDeinterlace"), vprefs::on("prefs/pdStudioRgb"));
        update();
    });
}

ExternalMonitor::~ExternalMonitor() {}

void ExternalMonitor::toggle()
{
    if (isOpen()) close();
    else open();
}

void ExternalMonitor::open()
{
    // Device "(None)"
    if (vprefs::text("prefs/pdDevice") == "none") {
        AppState::get().status("Preview Device is (None) in Options > Preferences, so there is no external preview.");
        return;
    }
    QScreen* s = d->screen();
    if (s) {
        setScreen(s);
        setGeometry(s->geometry());
    }
    d->key = 0;
    d->timer->start();
    showFullScreen();
    raise();
    if (d->pb) d->pb->requestFrame();
}

void ExternalMonitor::close()
{
    d->timer->stop();
    hide();
    if (d->main) d->main->activateWindow();
}

bool ExternalMonitor::isOpen() const { return isVisible(); }

void ExternalMonitor::setScreenName(const QString& name)
{
    d->wanted = name;
    QSettings().setValue("externalMonitor/screen", name);
    if (isOpen()) open();
}

QString ExternalMonitor::screenName() const { return d->wanted; }

QString ExternalMonitor::screenUsed() const
{
    QScreen* s = d->screen();
    return s ? s->name() : QString();
}

QImage ExternalMonitor::shown() const { return d->image; }

void ExternalMonitor::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), Qt::black);
    if (d->image.isNull()) return;
    // "Scale output to fit display" cleared: frame shown at its own size, centred
    const QSize sz = vprefs::on("prefs/pdScale") ? d->image.size().scaled(size(), Qt::KeepAspectRatio) : d->image.size();
    const QRect r(QPoint((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(r, d->image);
}

void ExternalMonitor::keyPressEvent(QKeyEvent* e)
{
    if (e->key() == Qt::Key_Escape) {
        close();
        return;
    }
    // main window keys (Space, J/K/L, Alt+Shift+4...) work here too
    if (d->main) {
        const QKeySequence ks(e->keyCombination());
        for (QAction* a : d->main->actions())
            for (const QKeySequence& k : a->shortcuts())
                if (k == ks && a->isEnabled()) {
                    a->trigger();
                    return;
                }
    }
    QWidget::keyPressEvent(e);
}

void ExternalMonitor::mouseDoubleClickEvent(QMouseEvent*) { close(); }

}  // namespace app
}  // namespace reno
