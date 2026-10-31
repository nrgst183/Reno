// The Video Preview window: image view with overlays and split view, the OpenGL used for effects' on-screen controls,
// the glyph buttons, and the preview widget.
#include "panels_internal.h"
#include "panels_glyph.h"
#include "readoutlabel.h"
#include "vprefs.h"
#include "audiotools.h"
#include "../engine/colormgmt.h"
#include "../engine/captions.h"
#include "previewextras.h"
#include "../engine/timefmt.h"
#include "../engine/fxnames.h"
#include "logo.h"
#include "fxlibrary.h"
#include "fxthumbs.h"
#include "playback.h"
#include "timeline.h"
#include "../media/media.h"
#include "lazyrefresh.h"
#include "strconv.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QInputDialog>
#include <QDesktopServices>
#include <QProcess>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QLocale>
#include <QStyle>
#include <QTreeWidgetItemIterator>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QCursor>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QDir>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QListView>
#include <QDrag>
#include <QActionGroup>
#include <QSplitter>
#include <QFileSystemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#if QT_CONFIG(opengl)
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>
#elif !defined(_WIN32)
#include <dlfcn.h>
#endif

namespace reno {
namespace app {

// Effects draw their on-screen controls (OpenFX overlays) with their own OpenGL calls, so a current context is needed.
namespace {

#ifdef _WIN32
#define RENO_GLAPI __stdcall
#else
#define RENO_GLAPI
#endif

struct GLFns {
    void(RENO_GLAPI* Viewport)(int, int, int, int) = nullptr;
    void(RENO_GLAPI* MatrixMode)(unsigned) = nullptr;
    void(RENO_GLAPI* LoadIdentity)() = nullptr;
    void(RENO_GLAPI* Ortho)(double, double, double, double, double, double) = nullptr;
    void(RENO_GLAPI* ClearColor)(float, float, float, float) = nullptr;
    void(RENO_GLAPI* Clear)(unsigned) = nullptr;
    void(RENO_GLAPI* Enable)(unsigned) = nullptr;
    void(RENO_GLAPI* Disable)(unsigned) = nullptr;
    void(RENO_GLAPI* GenTextures)(int, unsigned*) = nullptr;
    void(RENO_GLAPI* DeleteTextures)(int, const unsigned*) = nullptr;
    void(RENO_GLAPI* BindTexture)(unsigned, unsigned) = nullptr;
    void(RENO_GLAPI* TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void*) = nullptr;
    void(RENO_GLAPI* TexParameteri)(unsigned, unsigned, int) = nullptr;
    void(RENO_GLAPI* PixelStorei)(unsigned, int) = nullptr;
    void(RENO_GLAPI* Begin)(unsigned) = nullptr;
    void(RENO_GLAPI* End)() = nullptr;
    void(RENO_GLAPI* TexCoord2f)(float, float) = nullptr;
    void(RENO_GLAPI* Vertex2f)(float, float) = nullptr;
    void(RENO_GLAPI* Color4f)(float, float, float, float) = nullptr;
    void(RENO_GLAPI* MultMatrixd)(const double*) = nullptr;
    void(RENO_GLAPI* Finish)() = nullptr;
    void(RENO_GLAPI* ReadPixels)(int, int, int, int, unsigned, unsigned, void*) = nullptr;
    unsigned(RENO_GLAPI* GetError)() = nullptr;

    template <class F>
    bool resolve(F&& lookup)
    {
        bool ok = true;
        auto get = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(lookup(name));
            ok = ok && fn;
        };
        get(Viewport, "glViewport");
        get(MatrixMode, "glMatrixMode");
        get(LoadIdentity, "glLoadIdentity");
        get(Ortho, "glOrtho");
        get(ClearColor, "glClearColor");
        get(Clear, "glClear");
        get(Enable, "glEnable");
        get(Disable, "glDisable");
        get(GenTextures, "glGenTextures");
        get(DeleteTextures, "glDeleteTextures");
        get(BindTexture, "glBindTexture");
        get(TexImage2D, "glTexImage2D");
        get(TexParameteri, "glTexParameteri");
        get(PixelStorei, "glPixelStorei");
        get(Begin, "glBegin");
        get(End, "glEnd");
        get(TexCoord2f, "glTexCoord2f");
        get(Vertex2f, "glVertex2f");
        get(Color4f, "glColor4f");
        get(MultMatrixd, "glMultMatrixd");
        get(Finish, "glFinish");
        get(ReadPixels, "glReadPixels");
        get(GetError, "glGetError");
        return ok;
    }
};

// The offscreen frame the overlays are drawn in. One for the application, created when first needed.
class OverlayGL {
public:
    static OverlayGL& get()
    {
        static OverlayGL* g = new OverlayGL();
        return *g;
    }
    bool available() { init(); return ok_; }
    // Makes the context current with a frame of 'size' device pixels to draw in. False if there is no OpenGL.
    bool begin(const QSize& size);
    QImage end();
    GLFns gl;

private:
    void init();
    bool tried_ = false, ok_ = false;
    QSize size_;
#if QT_CONFIG(opengl)
    QOpenGLContext* ctx_ = nullptr;
    QOffscreenSurface* surface_ = nullptr;
    std::unique_ptr<QOpenGLFramebufferObject> fbo_;
#elif !defined(_WIN32)
    typedef void* (*EglProc)(const char*);
    void *egl_ = nullptr, *libgl_ = nullptr, *dpy_ = nullptr, *cfg_ = nullptr, *ctx_ = nullptr, *surf_ = nullptr;
    void* (*mkpb_)(void*, void*, const int*) = nullptr;
    unsigned (*destroySurf_)(void*, void*) = nullptr;
    unsigned (*makeCurrent_)(void*, void*, void*, void*) = nullptr;
#endif
};

#if QT_CONFIG(opengl)
void OverlayGL::init()
{
    if (tried_) return;
    tried_ = true;
    ctx_ = new QOpenGLContext();
    QSurfaceFormat f = QSurfaceFormat::defaultFormat();
    f.setProfile(QSurfaceFormat::CompatibilityProfile);
    ctx_->setFormat(f);
    if (!ctx_->create()) return;
    surface_ = new QOffscreenSurface();
    surface_->setFormat(ctx_->format());
    surface_->create();
    if (!surface_->isValid() || !ctx_->makeCurrent(surface_)) return;
    ok_ = gl.resolve([this](const char* n) { return (void*)ctx_->getProcAddress(n); });
    ctx_->doneCurrent();
}

bool OverlayGL::begin(const QSize& size)
{
    init();
    if (!ok_ || size.isEmpty() || !ctx_->makeCurrent(surface_)) return false;
    if (!fbo_ || fbo_->size() != size) {
        fbo_.reset();
        fbo_.reset(new QOpenGLFramebufferObject(size, QOpenGLFramebufferObject::CombinedDepthStencil));
    }
    if (!fbo_->isValid() || !fbo_->bind()) {
        ctx_->doneCurrent();
        return false;
    }
    size_ = size;
    return true;
}

QImage OverlayGL::end()
{
    gl.Finish();
    QImage img = fbo_->toImage();
    fbo_->release();
    ctx_->doneCurrent();
    return img;
}
#elif !defined(_WIN32)
void OverlayGL::init()
{
    if (tried_) return;
    tried_ = true;
    egl_ = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    libgl_ = dlopen("libGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!egl_ || !libgl_) return;
    EglProc gpa = (EglProc)dlsym(egl_, "eglGetProcAddress");
    auto platform = (void* (*)(unsigned, void*, const intptr_t*))(gpa ? gpa("eglGetPlatformDisplayEXT") : nullptr);
    auto initialize = (unsigned (*)(void*, int*, int*))dlsym(egl_, "eglInitialize");
    auto choose = (unsigned (*)(void*, const int*, void**, int, int*))dlsym(egl_, "eglChooseConfig");
    auto bind = (unsigned (*)(unsigned))dlsym(egl_, "eglBindAPI");
    auto mkctx = (void* (*)(void*, void*, void*, const int*))dlsym(egl_, "eglCreateContext");
    mkpb_ = (void* (*)(void*, void*, const int*))dlsym(egl_, "eglCreatePbufferSurface");
    destroySurf_ = (unsigned (*)(void*, void*))dlsym(egl_, "eglDestroySurface");
    makeCurrent_ = (unsigned (*)(void*, void*, void*, void*))dlsym(egl_, "eglMakeCurrent");
    if (!platform || !initialize || !choose || !bind || !mkctx || !mkpb_ || !destroySurf_ || !makeCurrent_) return;
    dpy_ = platform(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, nullptr, nullptr);
    int major = 0, minor = 0;
    if (!dpy_ || !initialize(dpy_, &major, &minor)) return;
    const int attrs[] = {0x3033 /* SURFACE_TYPE */, 0x0001 /* PBUFFER */, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8,
                         0x3040 /* RENDERABLE_TYPE */, 0x0008 /* OPENGL */, 0x3038};
    int n = 0;
    if (!choose(dpy_, attrs, &cfg_, 1, &n) || n < 1) return;
    bind(0x30A2 /* EGL_OPENGL_API */);
    ctx_ = mkctx(dpy_, cfg_, nullptr, nullptr);
    if (!ctx_) return;
    ok_ = gl.resolve([this](const char* name) { return dlsym(libgl_, name); });
}

bool OverlayGL::begin(const QSize& size)
{
    init();
    if (!ok_ || size.isEmpty()) return false;
    if (!surf_ || size != size_) {
        if (surf_) destroySurf_(dpy_, surf_);
        const int pa[] = {0x3057 /* WIDTH */, size.width(), 0x3056 /* HEIGHT */, size.height(), 0x3038};
        surf_ = mkpb_(dpy_, cfg_, pa);
        size_ = size;
    }
    return surf_ && makeCurrent_(dpy_, surf_, surf_, ctx_);
}

QImage OverlayGL::end()
{
    QImage img(size_, QImage::Format_RGBA8888);
    gl.Finish();
    gl.PixelStorei(0x0D05 /* GL_PACK_ALIGNMENT */, 4);
    gl.ReadPixels(0, 0, size_.width(), size_.height(), 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, img.bits());
    makeCurrent_(dpy_, nullptr, nullptr, nullptr);
    return img.mirrored();
}
#else
void OverlayGL::init() { tried_ = true; }
bool OverlayGL::begin(const QSize&) { return false; }
QImage OverlayGL::end() { return QImage(); }
#endif

}  // namespace

QImage drawWithGL(const QSize& pixels, const QColor& background, double left, double right, double bottom, double top,
                  const std::function<void()>& draw)
{
    OverlayGL& og = OverlayGL::get();
    if (pixels.isEmpty() || !og.begin(pixels)) return QImage();
    GLFns& gl = og.gl;
    gl.Viewport(0, 0, pixels.width(), pixels.height());
    gl.ClearColor((float)background.redF(), (float)background.greenF(), (float)background.blueF(), 1.f);
    gl.Clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
    gl.MatrixMode(0x1701 /* GL_PROJECTION */);
    gl.LoadIdentity();
    gl.Ortho(left, right, bottom, top, -1, 1);
    gl.MatrixMode(0x1700 /* GL_MODELVIEW */);
    gl.LoadIdentity();
    if (draw) draw();
    while (gl.GetError() != 0) {}
    return og.end();
}

namespace {

int ofxKeySym(const QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Backspace: return 0xff08;
    case Qt::Key_Tab: return 0xff09;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Enter: return 0xff8d;
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Shift: return 0xffe1;
    case Qt::Key_Control: return 0xffe3;
    case Qt::Key_Meta: return 0xffe7;
    case Qt::Key_Alt: return 0xffe9;
    case Qt::Key_Space: return 0x20;
    default: break;
    }
    if (e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F35) return 0xffbe + (e->key() - Qt::Key_F1);
    const QList<uint> text = e->text().toUcs4();
    if (!text.isEmpty() && text[0] >= 0x20) return text[0] < 0x100 ? (int)text[0] : (int)(0x01000000 | text[0]);
    if (e->key() >= Qt::Key_A && e->key() <= Qt::Key_Z) return 'a' + (e->key() - Qt::Key_A);
    if (e->key() > 0 && e->key() < 0x100) return e->key();
    return 0;
}

}  // namespace

namespace {

QImage channelView(const QImage& in, int channel)
{
    if (channel <= 0 || channel > 7 || in.isNull()) return in;
    QImage img = in.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb* line = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const QRgb c = line[x];
            const int r = qRed(c), g = qGreen(c), b = qBlue(c), a = qAlpha(c);
            switch (channel) {
            case 1: line[x] = qRgb(r, 0, 0); break;
            case 2: line[x] = qRgb(0, g, 0); break;
            case 3: line[x] = qRgb(0, 0, b); break;
            case 4: line[x] = qRgb(r, r, r); break;
            case 5: line[x] = qRgb(g, g, g); break;
            case 6: line[x] = qRgb(b, b, b); break;
            default: line[x] = qRgb(a, a, a); break;
            }
        }
    }
    return img;
}

// Draws the image fitted to the window on a dark background at the chosen preview quality: reduced to the preview
// size first, then scaled to fit (smooth, or pixel repetition for Draft).
class RenderingIndicator : public QWidget {
public:
    explicit RenderingIndicator(QWidget* parent) : QWidget(parent)
    {
        setObjectName("previewRendering");
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(26, 26);
        setToolTip("Rendering...");
        hide();
    }
    int step = 0;
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 140));
        p.drawEllipse(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));
        const QPointF c = QRectF(rect()).center();
        for (int k = 0; k < 8; ++k) {
            const int age = (step - k + 8) % 8;
            const double a = k * 0.7853981633974483;
            QPen pen(QColor(235, 235, 240, std::max(50, 255 - age * 30)), 2.4, Qt::SolidLine, Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(c + QPointF(std::cos(a), std::sin(a)) * 4.5, c + QPointF(std::cos(a), std::sin(a)) * 9.0);
        }
    }
};

class PictureView : public QWidget {
public:
    explicit PictureView(QWidget* parent) : QWidget(parent)
    {
        setMinimumSize(160, 90);
        setAutoFillBackground(false);
        setAttribute(Qt::WA_OpaquePaintEvent);
    }
    ~PictureView() override
    {
        *alive_ = false;
        {
            std::lock_guard<std::mutex> lk(fitMutex_);
            fitQuit_ = true;
        }
        fitCv_.notify_all();
        if (fitThread_.joinable()) fitThread_.join();
        setOverlay(0, FxTarget());
    }
    void setImage(const QImage& im)
    {
        image_ = im;
        ++imageGen_;
        overlayDirty_ = true;
        if (fitAheadOfPaint()) return;
        update();
    }
    // A new frame at the size of the one shown, plain view (no controls, zoom, split, captions or 360 mode): paint it
    // from the worker's result instead of painting the old fitted frame again.
    bool fitAheadOfPaint()
    {
        if (!isVisible() || image_.isNull() || scaled_.isNull() || slot_ || zoomed() || split || captionChannel || show360()) return false;
        const QRect t = target();
        const qreal dpr = devicePixelRatioF();
        const QSize px((int)(t.width() * dpr), (int)(t.height() * dpr));
        if (scaled_.size() != px || (image_.size() == px && !channel)) return false;
        fitInBackground(previewSize(), px, dpr);
        return true;
    }

    static QImage fitted(const QImage& image, QSize ps, QSize px, PreviewQuality quality, int channel, qreal dpr)
    {
        QImage small = (ps.width() < image.width() || ps.height() < image.height())
                           ? image.scaled(ps, Qt::IgnoreAspectRatio, quality == PreviewQuality::Draft ? Qt::FastTransformation : Qt::SmoothTransformation)
                           : image;
        if (channel) small = channelView(small, channel);
        const bool smooth = quality != PreviewQuality::Draft;
        QImage out = small.size() == px ? small : small.scaled(px, Qt::IgnoreAspectRatio, smooth ? Qt::SmoothTransformation : Qt::FastTransformation);
        out.setDevicePixelRatio(dpr);
        return out;
    }

    // Fits the newest image on a worker thread and shows it when done; a newer image that arrives meanwhile is fitted
    // next. The thread is created once and kept.
    void fitInBackground(const QSize& ps, const QSize& px, qreal dpr)
    {
        if (scaling_) return;
        scaling_ = true;
        {
            std::lock_guard<std::mutex> lk(fitMutex_);
            fitJob_.image = image_;
            fitJob_.gen = imageGen_;
            fitJob_.ps = ps;
            fitJob_.px = px;
            fitJob_.quality = quality;
            fitJob_.channel = channel;
            fitJob_.dpr = dpr;
            fitHave_ = true;
        }
        if (!fitThread_.joinable()) fitThread_ = std::thread([this] { fitLoop(); });
        fitCv_.notify_one();
    }
    void fitLoop()
    {
        for (;;) {
            FitJob job;
            {
                std::unique_lock<std::mutex> lk(fitMutex_);
                fitCv_.wait(lk, [this] { return fitHave_ || fitQuit_; });
                if (fitQuit_) return;
                job = std::move(fitJob_);
                fitJob_ = FitJob();
                fitHave_ = false;
            }
            QImage out = fitted(job.image, job.ps, job.px, job.quality, job.channel, job.dpr);
            job.image = QImage();
            Alive alive = alive_;
            const uint64_t gen = job.gen;
            const QSize px = job.px;
            AppState::post([this, alive, out, gen, px] {
                if (!*alive) return;
                scaling_ = false;
                if (out.size() == px && gen > scaledGen_) {
                    scaled_ = out;
                    scaledGen_ = gen;
                    overlayDirty_ = true;
                }
                update();
            });
        }
    }
    const QImage& image() const { return image_; }
    QSize sizeHint() const override { return QSize(480, 270); }

    PreviewQuality quality = PreviewQuality::Good;
    PreviewSize size = PreviewSize::Auto;
    bool safeAreas = false, grid = false;
    int channel = 0;
    // right-click menu switches: fit the image to the window (else one preview pixel per screen pixel), use the
    // project's pixel shape, and drop to Preview (Auto) while playing
    bool scaleToFit = true, simulateAspect = false, adjustForPlayback = false;
    // Split-screen view: inside splitArea (fractions of the image, from the top left) the other image is shown: the
    // frame without effects (FX Bypassed) or the clipboard image. A drag over the image sets the area (Ctrl+drag when
    // an effect's controls are on it).
    bool split = false, splitClipboard = false;
    QRectF splitArea = QRectF(0, 0, 0.5, 1);
    QImage splitImage;
    std::function<void(const QPoint&)> onContextMenu;
    std::function<void()> onSplitAreaChanged;
    // Zoom Video Preview (VEGAS 23): the image is drawn at zoomPct percent of the fitted size, panned by panOffset
    // (pixels). Display only, never a render. zoomBypassed: 100% for now.
    int zoomPct = 100;
    QPointF panOffset;
    bool zoomBypassed = false;
    std::function<void()> onZoomChanged;
    double zoomFactor() const { return zoomBypassed ? 1.0 : zoomPct / 100.0; }
    bool zoomed() const { return zoomFactor() != 1.0; }
    void setZoom(int pct, const QPointF* around = nullptr)
    {
        pct = std::max(10, std::min(1000, pct));
        zoomBypassed = false;
        if (pct == zoomPct && !around) { update(); return; }
        QPointF frac(0.5, 0.5);
        const QRect t0 = target();
        if (around && !t0.isEmpty()) frac = QPointF((around->x() - t0.left()) / t0.width(), (around->y() - t0.top()) / t0.height());
        zoomPct = pct;
        if (pct == 100) {
            panOffset = QPointF();
        } else if (around) {
            panOffset = QPointF();
            const QRect t1 = target();
            if (!t1.isEmpty()) panOffset = QPointF(around->x() - frac.x() * t1.width() + t1.width() / 2.0 - width() / 2.0 - (t1.left() + t1.width() / 2.0 - width() / 2.0),
                                                   around->y() - frac.y() * t1.height() + t1.height() / 2.0 - height() / 2.0 - (t1.top() + t1.height() / 2.0 - height() / 2.0));
        }
        clampPan();
        scaled_ = QImage();
        overlayDirty_ = true;
        update();
        if (onZoomChanged) onZoomChanged();
    }
    void bypassZoom(bool on)
    {
        zoomBypassed = on && zoomPct != 100;
        scaled_ = QImage();
        overlayDirty_ = true;
        update();
        if (onZoomChanged) onZoomChanged();
    }
    void clampPan()
    {
        panOffset = QPointF(std::max(-(double)width() * 2, std::min((double)width() * 2, panOffset.x())), std::max(-(double)height() * 2, std::min((double)height() * 2, panOffset.y())));
        const QRect t = target();
        if (t.isEmpty()) return;
        const double mx = (t.width() + width()) / 2.0 - 40, my = (t.height() + height()) / 2.0 - 40;
        panOffset = QPointF(std::max(-mx, std::min(mx, panOffset.x())), std::max(-my, std::min(my, panOffset.y())));
    }
    // Overlays > Closed Captioning: the caption channel drawn over the image (1..4; 0 none), and the 360° mode with
    // its camera yaw (right), pitch (up) and field of view (previewextras.h)
    int captionChannel = 0;
    bool mode360 = false;
    double yaw360 = 0, pitch360 = 0, fov360 = 90;
    bool show360() const { return mode360 && AppState::get().project().settings.output360 && !image_.isNull(); }
    void qualityChanged()
    {
        scaled_ = QImage();
        overlayDirty_ = true;
        update();
    }

    QRect target() const
    {
        if (image_.isNull()) return QRect();
        double wide = 1.0;
        if (simulateAspect) {
            const double par = AppState::get().project().settings.pixelAspect;
            if (par > 0.05 && par < 20) wide = par;
        }
        QSize t;
        if (scaleToFit) {
            t = QSize((int)std::lround(image_.width() * wide), image_.height()).scaled(QWidget::size(), Qt::KeepAspectRatio);
        } else {
            const QSize ps = previewSize();
            const qreal dpr = devicePixelRatioF();
            t = QSize(std::max(1, (int)std::lround(ps.width() * wide / dpr)), std::max(1, (int)std::lround(ps.height() / dpr)));
        }
        const double z = zoomFactor();
        if (z != 1.0) {
            const QSize zs(std::max(1, (int)std::lround(t.width() * z)), std::max(1, (int)std::lround(t.height() * z)));
            return QRect(QPoint((int)std::lround(width() / 2.0 + panOffset.x() - zs.width() / 2.0), (int)std::lround(height() / 2.0 + panOffset.y() - zs.height() / 2.0)), zs);
        }
        return QRect(QPoint((width() - t.width()) / 2, (height() - t.height()) / 2), t);
    }

    QSize previewSize() const
    {
        QSize base;
        {
            const ProjectSettings& s = AppState::get().project().settings;
            base = QSize(s.width, s.height);
        }
        if (base.isEmpty()) base = image_.isNull() ? QSize() : image_.size();
        QSize sz = base;
        PreviewQuality quality = this->quality;
        PreviewSize size = this->size;
        if (adjustForPlayback && playback && playback->playing()) {
            if (quality != PreviewQuality::Draft) quality = PreviewQuality::Preview;
            size = PreviewSize::Auto;
        }
        switch (size) {
        case PreviewSize::Auto: sz = base.scaled(QWidget::size() * devicePixelRatioF(), Qt::KeepAspectRatio); break;
        case PreviewSize::Full: break;
        case PreviewSize::Half: sz = base / 2; break;
        case PreviewSize::Quarter: sz = base / 4; break;
        }
        // Draft is a quarter of the size. Preview keeps the project size, as VEGAS does, and only uses cheaper
        // scaling and sampling.
        if (quality == PreviewQuality::Draft) sz /= 4;
        return QSize(std::max(1, sz.width()), std::max(1, sz.height()));
    }

    Playback* playback = nullptr;
    bool showOverlays = true;
    uint64_t overlaySlot() const { return slot_; }
    FxTarget overlayTarget() const { return fxTarget_; }

    void setOverlay(uint64_t slot, const FxTarget& t)
    {
        if (slot == slot_ && t == fxTarget_ && (!slot || live() == inst_)) return;
        letGo();
        slot_ = slot;
        fxTarget_ = t;
        inst_ = nullptr;
        bind();
        overlayChanged();
    }
    void overlayChanged()
    {
        overlayDirty_ = true;
        update();
    }
    bool overlayActive() { return active() != nullptr; }
    bool overlayDrawn() const { return !composite_.isNull(); }
    QPointF canonicalToView(double x, double y) const
    {
        if (!inst_ || target().isEmpty()) return QPointF();
        const QRectF t = target();
        const Placement m = placement();
        const double fx = m.m[0] * x + m.m[1] * y + m.m[2], fy = m.m[3] * x + m.m[4] * y + m.m[5];
        return QPointF(t.left() + fx * t.width() / inst_->setup.width, t.bottom() - fy * t.height() / inst_->setup.height);
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        if (image_.isNull()) {
            p.fillRect(rect(), QColor(45, 45, 45));
            p.setPen(QColor(120, 120, 120));
            p.drawText(rect(), Qt::AlignCenter, "No picture");
            return;
        }
        const QRect t = target();
        const qreal dpr = devicePixelRatioF();
        const QSize px((int)(t.width() * dpr), (int)(t.height() * dpr));
        const bool zoomedNow = zoomed();
        if (!zoomedNow && (scaled_.isNull() || scaled_.size() != px || scaledGen_ != imageGen_)) {
            const QSize ps = previewSize();
            const bool cheap = image_.size() == px && !channel;
            if (!cheap && !scaled_.isNull() && scaled_.size() == px && scaledGen_ != imageGen_) {
                fitInBackground(ps, px, dpr);
            } else {
                scaled_ = cheap ? image_ : fitted(image_, ps, px, quality, channel, dpr);
                if (cheap) scaled_.setDevicePixelRatio(dpr);
                scaledGen_ = imageGen_;
                overlayDirty_ = true;
            }
        }
        if (ofx::Instance* inst = active()) {
            const QSize all((int)std::lround(width() * dpr), (int)std::lround(height() * dpr));
            if (overlayDirty_ || composite_.size() != all) drawOverlay(inst, all);
        } else {
            composite_ = QImage();
        }
        if (show360()) {
            p.fillRect(rect(), QColor(45, 45, 45));
            const QImage v = view360(image_, px, yaw360, pitch360, fov360);
            p.drawImage(QRectF(t), channel ? channelView(v, channel) : v);
        } else if (!composite_.isNull()) {
            p.drawImage(QPoint(0, 0), composite_);
        } else {
            const QString bg = vprefs::text("prefs/previewBackground");
            if (bg == "checkerboard") {
                QPixmap tile(16, 16);
                tile.fill(QColor(200, 200, 200));
                QPainter tp(&tile);
                tp.fillRect(0, 0, 8, 8, QColor(150, 150, 150));
                tp.fillRect(8, 8, 8, 8, QColor(150, 150, 150));
                tp.end();
                p.fillRect(rect(), QBrush(tile));
            } else {
                const QColor fill = bg == "black" ? QColor(0, 0, 0) : bg == "white" ? QColor(255, 255, 255) : QColor(bg).isValid() ? QColor(bg) : QColor(45, 45, 45);
                if (!zoomedNow && !scaled_.hasAlphaChannel() && !scaled_.isNull()) {
                    QRegion round(rect());
                    round -= QRect(t.topLeft(), QSizeF(scaled_.deviceIndependentSize()).toSize());
                    for (const QRect& r : round) p.fillRect(r, fill);
                } else {
                    p.fillRect(rect(), fill);
                }
            }
            if (zoomedNow) {
                p.setClipRect(rect());
                p.setRenderHint(QPainter::SmoothPixmapTransform, quality != PreviewQuality::Draft);
                p.drawImage(t, channel ? channelView(image_, channel) : image_);
                p.setClipping(false);
            } else {
                p.drawImage(t.topLeft(), scaled_);
            }
        }
        if (split) {
            const QRectF a(t.left() + splitArea.left() * t.width(), t.top() + splitArea.top() * t.height(), splitArea.width() * t.width(), splitArea.height() * t.height());
            if (!splitImage.isNull()) {
                const QRectF src(splitArea.left() * splitImage.width(), splitArea.top() * splitImage.height(), splitArea.width() * splitImage.width(),
                                 splitArea.height() * splitImage.height());
                p.setRenderHint(QPainter::SmoothPixmapTransform, quality != PreviewQuality::Draft);
                p.drawImage(a, channel ? channelView(splitImage, channel) : splitImage, src);
            }
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, 160), 1.0));
            p.drawRect(a.adjusted(0.5, 0.5, -0.5, -0.5));
            p.setPen(QPen(QColor(255, 255, 255, 220), 1.0, Qt::DotLine));
            p.drawRect(a.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        const Preferences& pr = AppState::get().prefs;
        if (grid) {
            p.setPen(QPen(QColor(255, 255, 255, 90), 1.0));
            const int cols = std::max(1, pr.gridColumns), rows = std::max(1, pr.gridRows);
            for (int k = 1; k < cols; ++k) {
                const int x = t.left() + t.width() * k / cols;
                p.drawLine(x, t.top(), x, t.bottom());
            }
            for (int k = 1; k < rows; ++k) {
                const int y = t.top() + t.height() * k / rows;
                p.drawLine(t.left(), y, t.right(), y);
            }
        }
        if (safeAreas) {
            p.setPen(QPen(QColor(255, 255, 255, 150), 1.0, Qt::DashLine));
            p.setBrush(Qt::NoBrush);
            for (double f : {1.0 - pr.actionSafe / 100.0, 1.0 - pr.titleSafe / 100.0}) {
                const QSizeF s(t.width() * f, t.height() * f);
                p.drawRect(QRectF(QPointF(t.center().x() - s.width() / 2, t.center().y() - s.height() / 2), s));
            }
        }
        if (captionChannel) {
            const double at = playback && playback->playing() ? playback->position() : AppState::get().cursor();
            drawCaptions(p, t, captionChannel, at);
        }
    }

    void resizeEvent(QResizeEvent* e) override
    {
        overlayDirty_ = true;
        QWidget::resizeEvent(e);
    }

    // The pen goes to the effect's controls first; a setting change made through them is one undo step from pen down
    // to pen up.
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton && zoomed()) {
            panFrom_ = e->position();
            panStart_ = panOffset;
            panning_ = true;
            setCursor(Qt::ClosedHandCursor);
            e->accept();
            return;
        }
        if (picker && e->button() == Qt::LeftButton && !target().isEmpty()) {
            pickFrom_ = toPicture(e->position());
            picking_ = true;
            pickAt(1, pickFrom_);
            e->accept();
            return;
        }
        if (show360() && e->button() == Qt::LeftButton && !split && !active()) {
            drag360_ = true;
            from360_ = e->position();
            start360_ = QPointF(yaw360, pitch360);
            e->accept();
            return;
        }
        if (split && e->button() == Qt::LeftButton && !target().isEmpty() && ((e->modifiers() & Qt::ControlModifier) || !active())) {
            splitDrag_ = true;
            splitBefore_ = splitArea;
            splitFrom_ = toPicture(e->position());
            e->accept();
            return;
        }
        ofx::Instance* inst = e->button() == Qt::LeftButton ? active() : nullptr;
        if (!inst) return QWidget::mousePressEvent(e);
        beginEdit();
        if (pen(inst, kOfxInteractActionPenDown, e->position(), 1.0)) {
            held_ = true;
            setFocus(Qt::MouseFocusReason);
            e->accept();
        } else {
            endEdit();
            QWidget::mousePressEvent(e);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (panning_) {
            panOffset = panStart_ + (e->position() - panFrom_);
            clampPan();
            update();
            return;
        }
        if (picker && !target().isEmpty()) {
            pickAt(picking_ ? 2 : 0, toPicture(e->position()));
            e->accept();
            return;
        }
        if (drag360_) {
            const QPointF d = e->position() - from360_;
            const double k = fov360 / std::max(1, width());
            yaw360 = std::fmod(start360_.x() - d.x() * k + 540.0, 360.0) - 180.0;
            pitch360 = std::max(-89.0, std::min(89.0, start360_.y() + d.y() * k));
            update();
            return;
        }
        if (splitDrag_) {
            const QPointF to = toPicture(e->position());
            splitArea = QRectF(splitFrom_, to).normalized();
            update();
            return;
        }
        ofx::Instance* inst = active();
        if (inst) pen(inst, kOfxInteractActionPenMotion, e->position(), held_ ? 1.0 : 0.0);
        if (!held_) QWidget::mouseMoveEvent(e);
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        if (panning_ && e->button() == Qt::MiddleButton) {
            panning_ = false;
            unsetCursor();
            return;
        }
        if (picker && picking_ && e->button() == Qt::LeftButton) {
            picking_ = false;
            pickAt(3, toPicture(e->position()));
            e->accept();
            return;
        }
        if (drag360_ && e->button() == Qt::LeftButton) {
            drag360_ = false;
            return;
        }
        if (splitDrag_ && e->button() == Qt::LeftButton) {
            splitDrag_ = false;
            const QRectF a = QRectF(splitFrom_, toPicture(e->position())).normalized();
            splitArea = a.width() < 0.01 || a.height() < 0.01 ? splitBefore_ : a;
            update();
            if (onSplitAreaChanged) onSplitAreaChanged();
            return;
        }
        if (!held_ || e->button() != Qt::LeftButton) return QWidget::mouseReleaseEvent(e);
        held_ = false;
        if (ofx::Instance* inst = active()) pen(inst, kOfxInteractActionPenUp, e->position(), 0.0);
        endEdit();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (!key(e, e->isAutoRepeat() ? kOfxInteractActionKeyRepeat : kOfxInteractActionKeyDown)) QWidget::keyPressEvent(e);
    }
    // The wheel zooms (10% a step, 1% with Ctrl); the middle button pans, a double-click on it goes back to 100%
    void wheelEvent(QWheelEvent* e) override
    {
        if (show360()) {
            fov360 = std::max(20.0, std::min(140.0, fov360 * (e->angleDelta().y() > 0 ? 0.9 : 1.0 / 0.9)));
            update();
            e->accept();
            return;
        }
        if (image_.isNull() || e->angleDelta().y() == 0) return QWidget::wheelEvent(e);
        const int step = (e->modifiers() & Qt::ControlModifier) ? 1 : 10;
        const int cur = zoomBypassed ? 100 : zoomPct;
        const QPointF at = e->position();
        setZoom(cur + (e->angleDelta().y() > 0 ? step : -step), &at);
        e->accept();
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::MiddleButton) {
            zoomBypassed = false;
            setZoom(100);
            return;
        }
        QWidget::mouseDoubleClickEvent(e);
    }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        if (held_ || splitDrag_ || drag360_) return;
        if (onContextMenu) onContextMenu(e->globalPos());
    }
    void keyReleaseEvent(QKeyEvent* e) override
    {
        if (e->isAutoRepeat() || !key(e, kOfxInteractActionKeyUp)) QWidget::keyReleaseEvent(e);
    }
    void enterEvent(QEnterEvent* e) override
    {
        if (ofx::Instance* inst = active()) inst->overlayFocus(true, args(inst));
        QWidget::enterEvent(e);
    }
    void leaveEvent(QEvent* e) override
    {
        if (ofx::Instance* inst = active()) inst->overlayFocus(false, args(inst));
        QWidget::leaveEvent(e);
    }

public:
    // A picker (the Color Grading panel's eyedroppers): while set, the mouse over the image picks instead. It is told
    // the state (0 moving, 1 pressed, 2 dragged, 3 released) and the area as image fractions.
    std::function<void(int state, const QRectF& area, const QImage& region)> picker;
    void setPickerOn(bool on)
    {
        setMouseTracking(on);
        setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
        picking_ = false;
    }

private:
    bool picking_ = false;
    QPointF pickFrom_;
    void pickAt(int state, const QPointF& to)
    {
        QRectF area = (state == 0 || state == 1) ? QRectF(to, QSizeF(0, 0)) : QRectF(pickFrom_, to).normalized();
        QImage region(1, 1, QImage::Format_RGB32);
        region.fill(Qt::black);
        if (!image_.isNull()) {
            const int x0 = std::min(image_.width() - 1, (int)(area.left() * image_.width())), y0 = std::min(image_.height() - 1, (int)(area.top() * image_.height()));
            const int x1 = std::min(image_.width() - 1, std::max(x0, (int)(area.right() * image_.width()))), y1 = std::min(image_.height() - 1, std::max(y0, (int)(area.bottom() * image_.height())));
            region = image_.copy(x0, y0, x1 - x0 + 1, y1 - y0 + 1).convertToFormat(QImage::Format_RGB32);
            if (region.width() > 256 || region.height() > 256) region = region.scaled(std::min(256, region.width()), std::min(256, region.height()));
        }
        if (picker) picker(state, area, region);
    }
    QPointF toPicture(const QPointF& at) const
    {
        const QRectF t = target();
        if (t.isEmpty()) return QPointF();
        return QPointF(std::min(1.0, std::max(0.0, (at.x() - t.left()) / t.width())), std::min(1.0, std::max(0.0, (at.y() - t.top()) / t.height())));
    }
    bool splitDrag_ = false;
    QPointF splitFrom_;
    QRectF splitBefore_;
    bool panning_ = false;
    QPointF panFrom_, panStart_;
    bool drag360_ = false;
    QPointF from360_, start360_;

    ofx::Instance* live() const { return slot_ ? AppState::get().fx.video(slot_) : nullptr; }

    const FxSlot* slotInProject() const
    {
        const FxChain* chain = AppState::get().chainOf(AppState::get().project(), fxTarget_);
        if (chain)
            for (const FxSlot& f : *chain)
                if (f.id == slot_) return &f;
        return nullptr;
    }

    void bind()
    {
        ofx::Instance* inst = live();
        if (!inst || !inst->hasOverlay()) return;
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            Alive alive = alive_;
            inst->onOverlayRedraw = [this, alive] {
                AppState::post([this, alive] {
                    if (*alive) overlayChanged();
                });
            };
        }
        if (!inst->createOverlay()) return;
        inst_ = inst;
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus);
        if (underMouse())
            if (ofx::Instance* a = active()) a->overlayFocus(true, args(a));
    }

    void letGo()
    {
        endEdit();
        held_ = false;
        ofx::Instance* inst = live();
        if (inst && inst == inst_) {
            if (underMouse()) inst->overlayFocus(false, args(inst));
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            inst->onOverlayRedraw = nullptr;
            inst->disposeOverlay();
        }
        inst_ = nullptr;
        composite_ = QImage();
        setMouseTracking(false);
    }

    // The instance whose controls are drawn and get the mouse, or null. Needs an image, the effect on with its
    // overlay created, overlays shown, nothing playing, and for an event effect the cursor over the event.
    ofx::Instance* active()
    {
        if (!slot_ || !showOverlays || image_.isNull() || zoomed()) return nullptr;
        ofx::Instance* now = live();
        if (now != inst_) {
            inst_ = nullptr;
            composite_ = QImage();
            bind();
            now = inst_;
        }
        if (!now || !now->overlayMade()) return nullptr;
        if (playback && playback->playing()) return nullptr;
        const AppState& app = AppState::get();
        if (app.bypassAllFx) return nullptr;
        const FxSlot* fs = slotInProject();
        if (!fs || fs->bypass) return nullptr;
        if (fxTarget_.kind == FxTarget::Event) {
            const Event* ev = app.project().findEvent(fxTarget_.id);
            if (!ev || app.cursor() < ev->start - 1e-9 || app.cursor() >= ev->start + ev->length) return nullptr;
        }
        return now;
    }

    double fxTime() const
    {
        const Project& p = AppState::get().project();
        double t = p.frameTime(p.frameAt(AppState::get().cursor()));
        if (fxTarget_.kind == FxTarget::Event)
            if (const Event* e = p.findEvent(fxTarget_.id)) t -= e->start;
        if (t < 0) t = 0;
        const double f = t * p.settings.fps(), r = std::floor(f + 0.5);
        return std::fabs(f - r) < 1e-4 ? r : f;
    }

    // Where the effect's own frame is in the finished image (engine's effectPlacement: Pan/Crop, Track Motion, Parent
    // Motion) at the cursor: effect canonical -> frame canonical.
    struct Placement {
        double m[6] = {1, 0, 0, 0, 1, 0};
        double inv[6] = {1, 0, 0, 0, 1, 0};
        double scale[2] = {1, 1};
    };
    Placement placement() const
    {
        Placement pl;
        const AppState& app = AppState::get();
        const Project& p = app.project();
        if (!slot_ || !effectPlacement(p, slot_, p.frameTime(p.frameAt(app.cursor())), pl.m)) return pl;
        const double* m = pl.m;
        const double det = m[0] * m[4] - m[1] * m[3];
        if (!(std::fabs(det) > 1e-12)) {
            Placement id;
            return id;
        }
        pl.inv[0] = m[4] / det;
        pl.inv[1] = -m[1] / det;
        pl.inv[3] = -m[3] / det;
        pl.inv[4] = m[0] / det;
        pl.inv[2] = -(pl.inv[0] * m[2] + pl.inv[1] * m[5]);
        pl.inv[5] = -(pl.inv[3] * m[2] + pl.inv[4] * m[5]);
        pl.scale[0] = std::max(1e-6, std::hypot(m[0], m[3]));
        pl.scale[1] = std::max(1e-6, std::hypot(m[1], m[4]));
        return pl;
    }

    ofx::Instance::OverlayArgs args(ofx::Instance* inst) const
    {
        ofx::Instance::OverlayArgs a;
        const qreal dpr = devicePixelRatioF();
        const QRectF t = QRectF(target());
        const double cw = std::max(1, inst->setup.width), ch = std::max(1, inst->setup.height);
        const Placement pl = placement();
        a.pixelScale[0] = (t.width() > 0 ? cw / (t.width() * dpr) : 1.0) / pl.scale[0];
        a.pixelScale[1] = (t.height() > 0 ? ch / (t.height() * dpr) : 1.0) / pl.scale[1];
        const QSize ps = previewSize();
        a.renderScale[0] = std::min(1.0, ps.width() / cw);
        a.renderScale[1] = std::min(1.0, ps.height() / ch);
        a.viewport[0] = (int)std::lround(width() * dpr);
        a.viewport[1] = (int)std::lround(height() * dpr);
        for (double& c : a.background) c = 45.0 / 255.0;
        a.time = fxTime();
        return a;
    }

    bool pen(ofx::Instance* inst, const char* action, const QPointF& at, double pressure)
    {
        const ofx::Instance::OverlayArgs a = args(inst);
        const qreal dpr = devicePixelRatioF();
        const QRectF t = QRectF(target());
        const double fx = t.width() > 0 ? (at.x() - t.left()) * inst->setup.width / t.width() : 0.0;
        const double fy = t.height() > 0 ? (t.bottom() - at.y()) * inst->setup.height / t.height() : 0.0;
        const Placement pl = placement();
        const double x = pl.inv[0] * fx + pl.inv[1] * fy + pl.inv[2];
        const double y = pl.inv[3] * fx + pl.inv[4] * fy + pl.inv[5];
        const bool taken = inst->overlayPen(action, a, x, y, pressure, (int)std::lround(at.x() * dpr), (int)std::lround((height() - at.y()) * dpr));
        if (taken) overlayChanged();
        return taken;
    }

    bool key(QKeyEvent* e, const char* action)
    {
        ofx::Instance* inst = active();
        const int sym = inst ? ofxKeySym(e) : 0;
        if (!sym) return false;
        const bool open = beginEdit();
        const bool taken = inst->overlayKey(action, args(inst), sym, e->text().toStdString());
        if (open) endEdit();
        if (taken) overlayChanged();
        return taken;
    }

    bool beginEdit()
    {
        AppState& app = AppState::get();
        if (editOpen_ || !slot_ || app.fxEditOpen(slot_)) return false;
        app.beginFxEdit(slot_);
        editOpen_ = true;
        return true;
    }
    void endEdit()
    {
        if (!editOpen_) return;
        editOpen_ = false;
        const FxSlot* fs = slotInProject();
        const std::string name = fs && !fs->name.empty() ? fs->name : std::string("Effect");
        AppState::get().endFxEdit(slot_, "Edit \"" + name + "\" On Screen");
    }

    void drawOverlay(ofx::Instance* inst, const QSize& all)
    {
        overlayDirty_ = false;
        // a render holds the instance while it draws a frame: the frame is shown without the controls and they are
        // drawn again shortly
        std::unique_lock<std::recursive_mutex> lk(inst->mutex, std::try_to_lock);
        OverlayGL& og = OverlayGL::get();
        if (!lk.owns_lock()) {
            composite_ = QImage();
            QTimer::singleShot(30, this, [this] { overlayChanged(); });
            return;
        }
        if (!og.begin(all)) {
            composite_ = QImage();
            return;
        }
        GLFns& gl = og.gl;
        const qreal dpr = devicePixelRatioF();
        const QRectF t(QPointF(target().left() * dpr, target().top() * dpr), QSizeF(scaled_.width(), scaled_.height()));
        const float w = (float)all.width(), h = (float)all.height();
        gl.Viewport(0, 0, all.width(), all.height());
        gl.MatrixMode(0x1701 /* GL_PROJECTION */);
        gl.LoadIdentity();
        gl.Ortho(0, w, h, 0, -1, 1);
        gl.MatrixMode(0x1700 /* GL_MODELVIEW */);
        gl.LoadIdentity();
        gl.ClearColor(45 / 255.f, 45 / 255.f, 45 / 255.f, 1.f);
        gl.Clear(0x4000 /* GL_COLOR_BUFFER_BIT */);
        const QImage rgba = scaled_.convertToFormat(QImage::Format_RGBA8888);
        unsigned tex = 0;
        gl.GenTextures(1, &tex);
        gl.BindTexture(0x0DE1 /* GL_TEXTURE_2D */, tex);
        gl.PixelStorei(0x0CF5 /* GL_UNPACK_ALIGNMENT */, 4);
        gl.TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */);
        gl.TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
        gl.TexImage2D(0x0DE1, 0, 0x8058 /* GL_RGBA8 */, rgba.width(), rgba.height(), 0, 0x1908 /* GL_RGBA */, 0x1401, rgba.constBits());
        gl.Enable(0x0DE1);
        gl.Color4f(1, 1, 1, 1);
        gl.Begin(0x0007 /* GL_QUADS */);
        gl.TexCoord2f(0, 0);
        gl.Vertex2f((float)t.left(), (float)t.top());
        gl.TexCoord2f(1, 0);
        gl.Vertex2f((float)t.right(), (float)t.top());
        gl.TexCoord2f(1, 1);
        gl.Vertex2f((float)t.right(), (float)t.bottom());
        gl.TexCoord2f(0, 1);
        gl.Vertex2f((float)t.left(), (float)t.bottom());
        gl.End();
        gl.Disable(0x0DE1);
        gl.BindTexture(0x0DE1, 0);
        gl.DeleteTextures(1, &tex);
        const ofx::Instance::OverlayArgs a = args(inst);
        const Placement pl = placement();
        const double sx = 1.0 / (a.pixelScale[0] * pl.scale[0]), sy = 1.0 / (a.pixelScale[1] * pl.scale[1]);
        const double left = -t.left() / sx, bottom = -(h - t.bottom()) / sy;
        gl.MatrixMode(0x1701);
        gl.LoadIdentity();
        gl.Ortho(left, left + w / sx, bottom, bottom + h / sy, -1, 1);
        const double* m = pl.m;
        const double place[16] = {m[0], m[3], 0, 0, m[1], m[4], 0, 0, 0, 0, 1, 0, m[2], m[5], 0, 1};
        gl.MultMatrixd(place);
        gl.MatrixMode(0x1700);
        gl.LoadIdentity();
        inst->overlayDraw(a);
        while (gl.GetError() != 0) {}
        composite_ = og.end();
        composite_.setDevicePixelRatio(dpr);
    }

    QImage image_, scaled_;
    // Fitting a new image to the window runs on a worker thread and the old fitted image stays up meanwhile; smooth
    // scaling a 1080p or 4K image takes tens of milliseconds, too slow for the interface thread on every click or
    // played frame.
    uint64_t imageGen_ = 0, scaledGen_ = 0;
    bool scaling_ = false;
    struct FitJob {
        QImage image;
        QSize ps, px;
        PreviewQuality quality = PreviewQuality::Good;
        int channel = 0;
        qreal dpr = 1;
        uint64_t gen = 0;
    };
    std::mutex fitMutex_;
    std::condition_variable fitCv_;
    FitJob fitJob_;
    bool fitHave_ = false, fitQuit_ = false;
    std::thread fitThread_;
    QImage composite_;
    uint64_t slot_ = 0;
    FxTarget fxTarget_;
    ofx::Instance* inst_ = nullptr;
    bool overlayDirty_ = true, held_ = false, editOpen_ = false;
    Alive alive_ = makeAlive();
};

QLabel* infoLabel(QWidget* parent, const char* name, const QString& tip)
{
    QLabel* l = new QLabel(parent);
    l->setObjectName(name);
    l->setToolTip(tip);
    QPalette pal = l->palette();
    pal.setColor(QPalette::WindowText, QColor(220, 220, 220));
    l->setPalette(pal);
    return l;
}

const char* qualityName(PreviewQuality q)
{
    switch (q) {
    case PreviewQuality::Draft: return "Draft";
    case PreviewQuality::Preview: return "Preview";
    case PreviewQuality::Good: return "Good";
    case PreviewQuality::Best: return "Best";
    }
    return "Good";
}

const char* sizeName(PreviewSize s)
{
    switch (s) {
    case PreviewSize::Auto: return "Auto";
    case PreviewSize::Full: return "Full";
    case PreviewSize::Half: return "Half";
    case PreviewSize::Quarter: return "Quarter";
    }
    return "Auto";
}

}  // namespace

namespace {

// Split-screen view's "FX Bypassed" frame: the frame rendered again without effects, on its own thread (only the
// latest request is made). No plug-in is called, so it can't disturb them.
class BypassRenderer {
public:
    typedef std::function<void(const QImage&, double)> Deliver;
    explicit BypassRenderer(Deliver deliver) : deliver_(std::move(deliver)) { thread_ = std::thread([this] { run(); }); }
    ~BypassRenderer()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void request(double seconds)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            want_ = seconds;
            asked_ = true;
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        AppState& app = AppState::get();
        MediaPool pool;
        if (app.mediaOpener) pool.setOpener(app.mediaOpener);
        VideoEngine engine(pool, app.fx);
        for (;;) {
            double t;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return quit_ || asked_; });
                if (quit_) return;
                asked_ = false;
                t = want_;
            }
            std::shared_ptr<const Project> p = app.snapshot();
            FrameOptions fo;
            fo.skipEffects = true;
            ImagePtr img;
            try {
                img = engine.render(*p, t, fo);
            } catch (...) {
            }
            if (img) deliver_(toQImage(*img), t);
        }
    }
    Deliver deliver_;
    std::mutex mutex_;
    std::condition_variable cv_;
    double want_ = 0;
    bool asked_ = false, quit_ = false;
    std::thread thread_;
};

}  // namespace

struct PreviewWidget::Impl {
    PreviewWidget* owner = nullptr;
    Playback* pb = nullptr;
    std::unique_ptr<ListenerHandle> busyListener;      // wakes the rendering-indicator check up after a change
    PictureView* view = nullptr;
    GlyphButton *bypass = nullptr, *overlays = nullptr, *play = nullptr;
    QToolButton* qualityButton = nullptr;
    QLabel *project = nullptr, *previewInfo = nullptr, *frame = nullptr, *display = nullptr;
    // under the image
    QWidget* under = nullptr;
    QWidget *pause = nullptr, *stop = nullptr, *more = nullptr;
    GlyphButton *toStart = nullptr, *loop = nullptr;
    QLabel* captions[4] = {nullptr, nullptr, nullptr, nullptr};
    QWidget* rules[2] = {nullptr, nullptr};
    bool arranged = false;
    std::unique_ptr<ListenerHandle> listener;
    double lastMs = -1;
    QElapsedTimer sizesShown;
    QWidget* bar = nullptr;
    GlyphButton* split = nullptr;
    bool statusShown = true;
    QToolButton* button360 = nullptr;
    GlyphButton *record = nullptr, *playStart = nullptr, *toEnd = nullptr, *prevFrame = nullptr, *nextFrame = nullptr;
    QStringList transportSet;
    bool transportShown = true;
    void setTransportSet(const QStringList& set);
    void editTransportSet();
    void setTransportShown(bool on);
    void sync360();
    Alive alive = makeAlive();
    std::unique_ptr<BypassRenderer> bypassRenderer;

    void refreshSplit(double seconds)
    {
        if (!view->split) return;
        if (view->splitClipboard) {
            const QImage clip = QGuiApplication::clipboard()->image();
            view->splitImage = clip.isNull() || view->image().isNull() ? clip : clip.scaled(view->image().size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            view->update();
            return;
        }
        if (!bypassRenderer) {
            const Alive a = alive;
            Impl* self = this;
            bypassRenderer.reset(new BypassRenderer([a, self](const QImage& img, double) {
                AppState::post([a, self, img] {
                    if (!a->load() || !self->view->split || self->view->splitClipboard) return;
                    self->view->splitImage = img;
                    self->view->update();
                });
            }));
        }
        bypassRenderer->request(seconds);
    }
    void setSplit(bool on)
    {
        view->split = on;
        split->setChecked(on);
        if (QSettings().value("preview/retainSplit", false).toBool()) QSettings().setValue("preview/splitOn", on);
        if (!on) view->splitImage = QImage();
        else refreshSplit(AppState::get().cursor());
        view->update();
    }
    void showSplitMenu();
    void showContextMenu(const QPoint& global);
    void setStatusShown(bool on)
    {
        statusShown = on;
        for (QWidget* w : std::initializer_list<QWidget*>{project, previewInfo, frame, display, captions[0], captions[1], captions[2], captions[3]}) w->setVisible(on);
        QSettings().setValue("preview/statusBar", on);
    }

    static QString format(int w, int h, double fps) { return QString("%1x%2x32, %3p").arg(w).arg(h).arg(fps, 0, 'f', 3); }

    void showTime()
    {
        const AppState& app = AppState::get();
        setReadoutText(frame, QString::number((qlonglong)app.project().frameAt(app.cursor())));
    }
    void arrange();
    void showFormat()
    {
        const ProjectSettings& s = AppState::get().project().settings;
        setReadoutText(project, format(s.width, s.height, s.fps()));
        showSizes();
        sync360();
    }
    void showSizes()
    {
        const QSize ps = view->previewSize();
        const ProjectSettings& s = AppState::get().project().settings;
        // While playing, frames are rendered at the preview's size and the rate shown is the real one, below the
        // project's when frames are dropped, as in VEGAS. Auto only fits the window and shrinks images that can't
        // keep up.
        if (pb) pb->setPreviewSize(ps.width(), ps.height(), view->adjustForPlayback);
        const bool playingNow = pb && pb->playing();
        const double shownFps = playingNow ? pb->previewFrameRate() : 0.0;
        if (playingNow && !view->image().isNull()) setReadoutText(previewInfo, format(view->image().width(), view->image().height(), shownFps));
        else setReadoutText(previewInfo, format(ps.width(), ps.height(), s.fps()));
        if (playingNow || (pb && pb->droppedFrames() > 0))
            previewInfo->setToolTip(QString("The size the preview is rendered at, and the frames shown a second while playing (the project has %1): "
                                            "%2 frames dropped%3 - pictures not ready in time are not shown, the sound never waits for them")
                                        .arg(s.fps(), 0, 'f', 3).arg((qulonglong)pb->droppedFrames()).arg(playingNow ? "" : " the last time it played"));
        else
            previewInfo->setToolTip("The size the preview is rendered at, for the preview quality chosen");
        const QRect t = view->target();
        setReadoutText(display, t.isEmpty() ? QString("-") : QString("%1x%2x32").arg(t.width()).arg(t.height()));
        QPalette pal = display->palette();
        pal.setColor(QPalette::WindowText, !t.isEmpty() && t.width() < ps.width() ? QColor(240, 128, 128) : QColor(220, 220, 220));
        display->setPalette(pal);
        qualityButton->setText(QString("%1 (%2) \u25BE").arg(qualityName(view->quality)).arg(sizeName(view->size)));
        qualityButton->setToolTip(QString("Preview Quality: %1 (%2)").arg(qualityName(view->quality)).arg(sizeName(view->size)));
    }
    void showRenderMs()
    {
        frame->setToolTip(lastMs < 0 ? QString("The frame at the cursor") : QString("The frame at the cursor (rendered in %1 ms)").arg(lastMs, 0, 'f', lastMs < 10 ? 1 : 0));
    }
    void syncButtons()
    {
        bypass->setChecked(AppState::get().bypassAllFx);
        if (loop) loop->setChecked(AppState::get().loopOn);
        play->setPlaying(pb->playing());
    }
    void showMenu(QWidget* under);
    void toggleOverlay(bool grid);
};

void PreviewWidget::Impl::setTransportSet(const QStringList& set)
{
    transportSet = set;
    QSettings().setValue("preview/transportButtons", set);
    arranged = false;
    arrange();
}

void PreviewWidget::Impl::editTransportSet()
{
    QDialog dlg(owner);
    dlg.setObjectName("visibleButtonSet");
    dlg.setWindowTitle("Edit Visible Button Set");
    auto* v = new QVBoxLayout(&dlg);
    const struct { const char* key; const char* text; } all[] = {
        {"record", "Record"}, {"loop", "Loop Playback"}, {"playFromStart", "Play from Start"}, {"play", "Play"}, {"pause", "Pause"},
        {"stop", "Stop"}, {"goToStart", "Go to Start"}, {"goToEnd", "Go to End"}, {"previousFrame", "Previous Frame"}, {"nextFrame", "Next Frame"}};
    std::vector<std::pair<QString, QCheckBox*>> boxes;
    for (const auto& a : all) {
        auto* c = new QCheckBox(a.text, &dlg);
        c->setObjectName(QString("button_") + a.key);
        c->setChecked(transportSet.contains(a.key));
        v->addWidget(c);
        boxes.push_back(std::make_pair(QString(a.key), c));
    }
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    QStringList set;
    for (const auto& b : boxes)
        if (b.second->isChecked()) set << b.first;
    setTransportSet(set);
}

void PreviewWidget::Impl::setTransportShown(bool on)
{
    transportShown = on;
    QSettings().setValue("preview/transportBar", on);
    arranged = false;
    arrange();
}

void PreviewWidget::Impl::sync360()
{
    if (!button360) return;
    const bool is360 = AppState::get().project().settings.output360;
    button360->setEnabled(is360);
    button360->setChecked(is360 && view->mode360);
}

// Lays out what is under the image: small transport in the middle, sizes at the left, frame and display size at the
// right.
void PreviewWidget::Impl::arrange()
{
    if (arranged) return;
    arranged = true;
    delete under->layout();
    for (QLabel* l : {project, previewInfo, frame, display}) releaseReadout(l);
    for (QWidget* w : {(QWidget*)toStart, (QWidget*)loop}) w->setVisible(false);
    for (GlyphButton* b : {toStart, play, (GlyphButton*)pause, (GlyphButton*)stop, loop, (GlyphButton*)more}) {
        b->setFixedSize(24, 22);
    }
    for (GlyphButton* b : {record, playStart, toEnd, prevFrame, nextFrame})
        if (b) b->setVisible(false);
    QPalette owner_pal = owner->palette();
    owner_pal.setColor(QPalette::Window, QColor(70, 70, 70));
    owner->setPalette(owner_pal);
    for (QWidget* r : rules) {
        QPalette lp = r->palette();
        lp.setColor(QPalette::Window, QColor(130, 130, 130));
        r->setPalette(lp);
    }
    const QColor dim = QColor(220, 220, 220);
    for (QLabel* l : {project, previewInfo, frame}) {
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, dim);
        l->setPalette(pal);
    }
    for (QLabel* l : captions) {
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, owner->palette().color(QPalette::WindowText));
        l->setPalette(pal);
    }
    auto* uv = new QVBoxLayout(under);
    uv->setContentsMargins(3, 4, 8, 2);
    uv->setSpacing(0);
    auto* transport = new QHBoxLayout();
    transport->setSpacing(0);
    transport->addStretch(1);
    {
        const struct { const char* key; QWidget* w; } order[] = {
            {"record", record}, {"loop", loop}, {"playFromStart", playStart}, {"play", play}, {"pause", pause}, {"stop", stop},
            {"goToStart", toStart}, {"goToEnd", toEnd}, {"previousFrame", prevFrame}, {"nextFrame", nextFrame}};
        for (const auto& o : order) {
            if (!o.w) continue;
            const bool on = transportShown && transportSet.contains(o.key);
            o.w->setVisible(on);
            if (on) transport->addWidget(o.w);
        }
        more->setVisible(transportShown);
        transport->addWidget(more);
    }
    transport->addStretch(1);
    uv->addLayout(transport);
    uv->addStretch(1);
    auto* halves = new QHBoxLayout();
    halves->setContentsMargins(0, 0, 0, 0);
    halves->setSpacing(0);
    QGridLayout* grids[2];
    for (int k = 0; k < 2; ++k) {
        grids[k] = new QGridLayout();
        grids[k]->setContentsMargins(k == 0 ? 0 : 3, 0, 0, 0);
        grids[k]->setHorizontalSpacing(8);
        grids[k]->setVerticalSpacing(0);
        grids[k]->setColumnStretch(2, 1);
        halves->addLayout(grids[k], 1);
    }
    grids[0]->addWidget(captions[0], 0, 0);
    grids[0]->addWidget(captions[1], 1, 0);
    grids[1]->addWidget(captions[2], 0, 0);
    grids[1]->addWidget(captions[3], 1, 0);
    grids[0]->addWidget(project, 0, 1);
    grids[0]->addWidget(previewInfo, 1, 1);
    grids[1]->addWidget(frame, 0, 1);
    grids[1]->addWidget(display, 1, 1);
    uv->addLayout(halves);
    under->setFixedHeight(60);
}

void PreviewWidget::Impl::showMenu(QWidget* under)
{
    QMenu menu(owner);
    const struct { const char* key; const char* text; GlyphButton* b; } extra[] = {
        {"record", "Record", record}, {"loop", "Loop Playback", loop}, {"playFromStart", "Play from Start", playStart},
        {"play", "Play", play}, {"pause", "Pause", (GlyphButton*)pause}, {"stop", "Stop", (GlyphButton*)stop},
        {"goToStart", "Go to Start", toStart}, {"goToEnd", "Go to End", toEnd}, {"previousFrame", "Previous Frame", prevFrame},
        {"nextFrame", "Next Frame", nextFrame}};
    for (const auto& x : extra) {
        if (!x.b || (x.b->isVisible() && transportShown)) continue;
        GlyphButton* b = x.b;
        menu.addAction(x.text, owner, [b] { b->click(); });
    }
    menu.addAction("Edit Visible Button Set...", owner, [this] { editTransportSet(); });
    menu.addSeparator();
    QAction* copy = menu.addAction("Copy Snapshot to Clipboard");
    QAction* save = menu.addAction("Save Snapshot to File...");
    copy->setEnabled(!view->image().isNull());
    save->setEnabled(!view->image().isNull());
    menu.addSeparator();
    QAction* safe = menu.addAction("Safe Areas");
    safe->setCheckable(true);
    safe->setChecked(view->safeAreas);
    QAction* grid = menu.addAction("Grid");
    grid->setCheckable(true);
    grid->setChecked(view->grid);
    QAction* picked = menu.exec(under->mapToGlobal(QPoint(0, under->height())));
    if (picked == copy) QGuiApplication::clipboard()->setImage(view->image());
    else if (picked == save) owner->findChild<QAbstractButton*>("saveSnapshot")->click();
    else if (picked == safe) toggleOverlay(false);
    else if (picked == grid) toggleOverlay(true);
}

void PreviewWidget::Impl::showSplitMenu()
{
    QMenu menu(owner);
    QAction* fx = menu.addAction("FX Bypassed");
    fx->setCheckable(true);
    fx->setChecked(!view->splitClipboard);
    QAction* clip = menu.addAction("Clipboard");
    clip->setCheckable(true);
    clip->setChecked(view->splitClipboard);
    menu.addSeparator();
    QAction* left = menu.addAction("Select Left Half");
    QAction* right = menu.addAction("Select Right Half");
    QAction* all = menu.addAction("Select All");
    menu.addSeparator();
    QAction* retain = menu.addAction("Retain Split Screen Setting");
    retain->setObjectName("retainSplit");
    retain->setCheckable(true);
    retain->setChecked(QSettings().value("preview/retainSplit", false).toBool());
    QAction* picked = menu.exec(split->mapToGlobal(QPoint(0, split->height())));
    if (!picked) return;
    if (picked == retain) {
        QSettings().setValue("preview/retainSplit", retain->isChecked());
        QSettings().setValue("preview/splitOn", view->split);
        return;
    }
    if (picked == fx || picked == clip) owner->setSplitScreen(true, picked == clip);
    else if (picked == left || picked == right || picked == all)
        owner->setSplitArea(picked == left ? QRectF(0, 0, 0.5, 1) : picked == right ? QRectF(0.5, 0, 0.5, 1) : QRectF(0, 0, 1, 1));
}

void PreviewWidget::Impl::showContextMenu(const QPoint& global)
{
    QMenu menu(owner);
    menu.setObjectName("previewContextMenu");
    auto toggle = [&menu](const char* text, bool on) {
        QAction* a = menu.addAction(text);
        a->setCheckable(true);
        a->setChecked(on);
        return a;
    };
    QAction* aspect = toggle("Simulate Device Aspect Ratio", view->simulateAspect);
    QAction* fit = toggle("Scale Video to Fit Preview Window", view->scaleToFit);
    QAction* adjust = toggle("Adjust Size and Quality for Optimal Playback", view->adjustForPlayback);
    QAction* projectSize = toggle("Display at Project Size", view->size == PreviewSize::Full && !view->scaleToFit);
    menu.addSeparator();
    QAction* tools = toggle("Show Toolbar", !bar->isHidden());
    QAction* status = toggle("Show Status Bar", statusShown);
    QAction* transport = toggle("Show Transport Bar", transportShown);
    menu.addSeparator();
    QAction* copy = menu.addAction("Copy Snapshot to Clipboard");
    QAction* save = menu.addAction("Save Snapshot to File...");
    copy->setEnabled(!view->image().isNull());
    save->setEnabled(!view->image().isNull());
    menu.addSeparator();
    QAction* prefs = menu.addAction("Video Preview Preferences...");
    prefs->setEnabled((bool)owner->onPreferences);
    QAction* picked = menu.exec(global);
    if (picked == aspect) owner->setDisplayOptions(!view->simulateAspect, view->scaleToFit, view->adjustForPlayback);
    else if (picked == fit) owner->setDisplayOptions(view->simulateAspect, !view->scaleToFit, view->adjustForPlayback);
    else if (picked == adjust) owner->setDisplayOptions(view->simulateAspect, view->scaleToFit, !view->adjustForPlayback);
    else if (picked == tools) owner->setToolbarVisible(bar->isHidden());
    else if (picked == status) setStatusShown(!statusShown);
    else if (picked == transport) setTransportShown(!transportShown);
    else if (picked == projectSize) {
        if (projectSize->isChecked()) {
            owner->setQuality(view->quality, PreviewSize::Full);
            owner->setDisplayOptions(view->simulateAspect, false, view->adjustForPlayback);
        } else {
            owner->setDisplayOptions(view->simulateAspect, true, view->adjustForPlayback);
        }
    }
    else if (picked == copy) owner->findChild<QAbstractButton*>("copySnapshot")->click();
    else if (picked == save) owner->findChild<QAbstractButton*>("saveSnapshot")->click();
    else if (picked == prefs && owner->onPreferences) owner->onPreferences();
}

void PreviewWidget::Impl::toggleOverlay(bool grid)
{
    if (grid) view->grid = !view->grid;
    else view->safeAreas = !view->safeAreas;
    QSettings s;
    s.setValue("preview/safeAreas", view->safeAreas);
    s.setValue("preview/grid", view->grid);
    view->update();
}

PreviewWidget::PreviewWidget(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    d->pb = playback;
    gPlayback = playback;
    setAutoFillBackground(true);
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(70, 70, 70));
    setPalette(pal);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto rule = [this](const QColor& c) {
        QWidget* w = new QWidget(this);
        w->setFixedHeight(1);
        w->setAutoFillBackground(true);
        QPalette lp = w->palette();
        lp.setColor(QPalette::Window, c);
        w->setPalette(lp);
        return w;
    };
    d->rules[0] = rule(QColor(130, 130, 130));
    root->addWidget(d->rules[0]);

    auto* bar = new QWidget(this);
    bar->setObjectName("previewToolBar");
    bar->setFixedHeight(29);
    auto* tb = new QHBoxLayout(bar);
    tb->setContentsMargins(4, 0, 4, 0);
    tb->setSpacing(2);
    auto* props = new GlyphButton(Glyph::Properties, "Project Video Properties", bar);
    props->setObjectName("projectProperties");
    d->bar = bar;
    auto* monitor = new GlyphButton(Glyph::Monitor, "Video Preview on External Monitor (Alt+Shift+4)", bar);
    monitor->setObjectName("externalMonitor");
    auto* outFx = new GlyphButton(Glyph::OutputFx, "Video Output FX", bar);
    outFx->setObjectName("videoOutputFx");
    auto* split = new GlyphButton(Glyph::Split, "Split-Screen View: the picture without effects (or the clipboard's) in part of the window; "
                                                "drag over the picture to choose the part. The arrow chooses what shows.", bar, 34);
    split->setObjectName("splitScreen");
    split->menuArrow = true;
    split->setCheckable(true);
    d->split = split;
    d->qualityButton = new QToolButton(bar);
    d->qualityButton->setObjectName("previewQuality");
    d->qualityButton->setPopupMode(QToolButton::InstantPopup);
    d->qualityButton->setFocusPolicy(Qt::NoFocus);
    d->qualityButton->setAutoRaise(true);
    d->qualityButton->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0px; } QToolButton { padding: 2px 3px; }");
    d->overlays = new GlyphButton(Glyph::Overlays, "Overlays: the effects' on-screen controls, safe areas, grid", bar, 34);
    d->overlays->setObjectName("overlays");
    d->overlays->menuArrow = true;
    auto* zoomBtn = new GlyphButton(Glyph::Zoom, "Zoom Video Preview: look closer at the picture (the mouse wheel zooms, the middle button pans)", bar, 34);
    zoomBtn->setObjectName("zoomPreview");
    zoomBtn->menuArrow = true;
    d->button360 = new QToolButton(bar);
    d->button360->setObjectName("preview360");
    d->button360->setText(QString::fromUtf8("360\u00B0"));
    d->button360->setCheckable(true);
    d->button360->setAutoRaise(true);
    d->button360->setFocusPolicy(Qt::NoFocus);
    d->button360->setToolTip(QString::fromUtf8("360\u00B0 Preview: look round a 360\u00B0 project's picture (drag to turn, the wheel to zoom); "
                                               "Project Properties' 360\u00B0 output makes a project a 360\u00B0 one"));
    auto* copy = new GlyphButton(Glyph::Copy, "Copy Snapshot to Clipboard", bar);
    copy->setObjectName("copySnapshot");
    auto* save = new GlyphButton(Glyph::Save, "Save Snapshot to File...", bar);
    save->setObjectName("saveSnapshot");
    d->bypass = new GlyphButton(Glyph::Bypass, "Bypass All Video FX (show the picture without any effects)", bar);
    d->bypass->setObjectName("bypassFx");
    d->bypass->setCheckable(true);
    for (QWidget* w : std::initializer_list<QWidget*>{props, monitor, outFx, split, d->qualityButton, d->overlays, zoomBtn, d->button360, copy, save, d->bypass}) tb->addWidget(w);
    tb->addStretch(1);
    root->addWidget(bar);

    d->view = new PictureView(this);
    {
        // rendering indicator over the frame's corner, checked a dozen times a second: shown once the preview has
        // waited more than 150 ms for an image
        auto* busy = new RenderingIndicator(d->view);
        QTimer* busyTimer = new QTimer(this);
        busyTimer->setInterval(70);
        PictureView* view = d->view;
        QObject::connect(busyTimer, &QTimer::timeout, this, [this, busy, view, playback, busyTimer] {
            // quiet: nothing playing and no picture awaited, so a slow check will do until something changes (below)
            const bool quiet = !(playback && (playback->playing() || playback->renderingFor() > 0)) && busy->isHidden();
            if (quiet != (busyTimer->interval() > 70)) busyTimer->setInterval(quiet ? 500 : 70);
            const bool on = isVisible() && playback && playback->renderingFor() > 0.15;
            if (on) {
                const QRect pic = view->target().isEmpty() ? view->rect() : view->target().intersected(view->rect());
                busy->move(std::max(0, pic.right() - busy->width() - 6), std::max(0, pic.top() + 6));
                busy->step = (busy->step + 1) % 8;
                if (busy->isHidden()) {
                    busy->show();
                    busy->raise();
                }
                busy->update();
            } else if (!busy->isHidden()) {
                busy->hide();
            }
        });
        busyTimer->start();
        // any change in the project, cursor or playback state: back to the quick check
        d->busyListener.reset(new ListenerHandle([busyTimer](int) { if (busyTimer->interval() != 70) busyTimer->setInterval(70); }));
    }
    d->view->setObjectName("previewPicture");
    root->addWidget(d->view, 1);
    d->rules[1] = rule(QColor(130, 130, 130));
    root->addWidget(d->rules[1]);

    auto* under = new QWidget(this);
    under->setObjectName("previewInfo");
    d->under = under;
    d->play = new GlyphButton(Glyph::Play, "Play (Space)", under, 24, 22);
    d->play->setObjectName("previewPlay");
    auto* pause = new GlyphButton(Glyph::Pause, "Pause (Enter)", under, 24, 22);
    pause->setObjectName("previewPause");
    auto* stop = new GlyphButton(Glyph::Stop, "Stop (Esc)", under, 24, 22);
    stop->setObjectName("previewStop");
    auto* more = new GlyphButton(Glyph::Menu, "Snapshots and overlays", under, 24, 22);
    more->setObjectName("previewMenu");
    d->pause = pause;
    d->stop = stop;
    d->more = more;
    d->toStart = new GlyphButton(Glyph::GoToStart, "Go To Start (Ctrl+Home)", under, 24, 22);
    d->toStart->setObjectName("previewGoToStart");
    d->loop = new GlyphButton(Glyph::Loop, "Loop Playback (Q)", under, 24, 22);
    d->loop->setObjectName("previewLoop");
    d->loop->setCheckable(true);
    d->record = new GlyphButton(Glyph::Record, "Record (Ctrl+R)", under, 24, 22);
    d->record->setObjectName("previewRecord");
    d->playStart = new GlyphButton(Glyph::PlayFromStart, "Play from Start (Shift+Space)", under, 24, 22);
    d->playStart->setObjectName("previewPlayFromStart");
    d->toEnd = new GlyphButton(Glyph::GoToEnd, "Go To End (Ctrl+End)", under, 24, 22);
    d->toEnd->setObjectName("previewGoToEnd");
    d->prevFrame = new GlyphButton(Glyph::PrevFrame, "Previous Frame (Left Arrow)", under, 24, 22);
    d->prevFrame->setObjectName("previewPrevFrame");
    d->nextFrame = new GlyphButton(Glyph::NextFrame, "Next Frame (Right Arrow)", under, 24, 22);
    d->nextFrame->setObjectName("previewNextFrame");
    for (GlyphButton* b : {d->prevFrame, d->nextFrame}) {
        b->setAutoRepeat(true);
        b->setAutoRepeatDelay(350);
        b->setAutoRepeatInterval(60);
    }
    {
        QSettings ts;
        d->transportSet = ts.value("preview/transportButtons", QStringList{"play", "pause", "stop"}).toStringList();
        d->transportShown = ts.value("preview/transportBar", true).toBool();
    }
    const char* captionText[4] = {"Project:", "Preview:", "Frame:", "Display:"};
    for (int k = 0; k < 4; ++k) d->captions[k] = new QLabel(captionText[k], under);
    d->project = infoLabel(under, "projectFormat", "The project's frame size and rate");
    d->previewInfo = infoLabel(under, "previewSize", "The size the preview is rendered at, for the preview quality chosen");
    d->frame = infoLabel(under, "frameNumber", "The frame at the cursor");
    d->display = infoLabel(under, "displaySize", "The size the picture takes in this window");
    d->arrange();
    root->addWidget(under);

    {
        QSettings s;
        const std::string q0 = AppState::get().prefs.previewQuality;
        d->view->quality = q0 == "draft" ? PreviewQuality::Draft : q0 == "good" ? PreviewQuality::Good : q0 == "best" ? PreviewQuality::Best : PreviewQuality::Preview;
        const int sv = s.value("preview/size", (int)PreviewSize::Full).toInt();
        if (sv >= 0 && sv <= 3) d->view->size = (PreviewSize)sv;
        d->view->safeAreas = s.value("preview/safeAreas", false).toBool();
        d->view->grid = s.value("preview/grid", false).toBool();
        d->view->showOverlays = s.value("preview/fxOverlays", true).toBool();
        d->view->scaleToFit = s.value("preview/scaleToFit", true).toBool();
        d->view->simulateAspect = s.value("preview/simulateAspect", false).toBool();
        d->view->adjustForPlayback = s.value("preview/adjustForPlayback", false).toBool();
        d->view->splitClipboard = s.value("preview/splitClipboard", false).toBool();
        const QRectF area = s.value("preview/splitArea", QRectF(0, 0, 0.5, 1)).toRectF();
        if (area.width() > 0.005 && area.height() > 0.005 && QRectF(0, 0, 1, 1).contains(area)) d->view->splitArea = area;
        bar->setVisible(s.value("preview/toolbar", true).toBool());
        if (!s.value("preview/statusBar", true).toBool()) d->setStatusShown(false);
    }
    d->view->playback = playback;
    if (playback) playback->setFullQuality(d->view->quality == PreviewQuality::Best);

    Impl* impl = d.get();
    QObject::connect(props, &QAbstractButton::clicked, this, [this] { if (onProjectProperties) onProjectProperties(); });
    QObject::connect(monitor, &QAbstractButton::clicked, this, [this] { if (onExternalMonitor) onExternalMonitor(); });
    QObject::connect(split, &QAbstractButton::clicked, this, [impl, split](bool on) {
        if (split->mapFromGlobal(QCursor::pos()).x() >= split->width() - 10 && split->rect().contains(split->mapFromGlobal(QCursor::pos()))) {
            split->setChecked(!on);
            impl->showSplitMenu();
            return;
        }
        impl->setSplit(on);
    });
    d->view->onContextMenu = [impl](const QPoint& global) { impl->showContextMenu(global); };
    d->view->onSplitAreaChanged = [impl] { QSettings().setValue("preview/splitArea", impl->view->splitArea); };
    QObject::connect(outFx, &QAbstractButton::clicked, this, [this] {
        FxTarget t;
        t.kind = FxTarget::VideoOut;
        if (onOpenFx) onOpenFx(t, 0);
    });
    QObject::connect(d->bypass, &QAbstractButton::clicked, [](bool on) {
        AppState::get().bypassAllFx = on;
        AppState::get().notify(kFxParams);
    });
    QMenu* qualities = new QMenu(d->qualityButton);
    QObject::connect(qualities, &QMenu::aboutToShow, this, [this, impl, qualities] {
        qualities->clear();
        for (PreviewQuality qv : {PreviewQuality::Draft, PreviewQuality::Preview, PreviewQuality::Good, PreviewQuality::Best}) {
            QMenu* sub = qualities->addMenu(qualityName(qv));
            for (PreviewSize sv : {PreviewSize::Auto, PreviewSize::Quarter, PreviewSize::Half, PreviewSize::Full}) {
                QAction* a = sub->addAction(sizeName(sv));
                a->setCheckable(true);
                a->setChecked(impl->view->quality == qv && impl->view->size == sv);
                QObject::connect(a, &QAction::triggered, this, [this, qv, sv] { setQuality(qv, sv); });
            }
        }
    });
    d->qualityButton->setMenu(qualities);
    QObject::connect(zoomBtn, &QAbstractButton::clicked, this, [this, impl, zoomBtn] {
        QMenu menu(this);
        menu.setObjectName("zoomMenu");
        PictureView* v = impl->view;
        QAction* in = menu.addAction("Zoom In");
        in->setObjectName("zoomIn");
        QAction* out = menu.addAction("Zoom Out");
        out->setObjectName("zoomOut");
        QAction* reset = menu.addAction("Reset Zoom (100%)");
        reset->setObjectName("zoomReset");
        menu.addSeparator();
        for (int pct : {25, 50, 100, 200, 400}) {
            QAction* a = menu.addAction(QString("%1%").arg(pct));
            a->setCheckable(true);
            a->setChecked(!v->zoomBypassed && v->zoomPct == pct);
            QObject::connect(a, &QAction::triggered, &menu, [v, pct] { v->setZoom(pct); });
        }
        menu.addSeparator();
        QAction* byp = menu.addAction("Bypass Zoom");
        byp->setObjectName("zoomBypass");
        byp->setCheckable(true);
        byp->setChecked(v->zoomBypassed);
        byp->setEnabled(v->zoomPct != 100);
        QObject::connect(in, &QAction::triggered, &menu, [v] { v->setZoom((v->zoomBypassed ? 100 : v->zoomPct) + 10); });
        QObject::connect(out, &QAction::triggered, &menu, [v] { v->setZoom((v->zoomBypassed ? 100 : v->zoomPct) - 10); });
        QObject::connect(reset, &QAction::triggered, &menu, [v] { v->zoomBypassed = false; v->setZoom(100); });
        QObject::connect(byp, &QAction::toggled, &menu, [v](bool on) { v->bypassZoom(on); });
        menu.exec(zoomBtn->mapToGlobal(QPoint(0, zoomBtn->height())));
    });
    QObject::connect(d->overlays, &QAbstractButton::clicked, this, [this, impl] {
        QMenu menu(this);
        QAction* fx = menu.addAction("Show Video FX Overlays");
        fx->setObjectName("showFxOverlays");
        fx->setCheckable(true);
        fx->setChecked(impl->view->showOverlays);
        fx->setToolTip("The on-screen controls of the effect open in the Video FX window");
        menu.addSeparator();
        QAction* safe = menu.addAction("Safe Areas");
        safe->setCheckable(true);
        safe->setChecked(impl->view->safeAreas);
        QAction* grid2 = menu.addAction("Grid");
        grid2->setCheckable(true);
        grid2->setChecked(impl->view->grid);
        menu.addSeparator();
        const char* names[] = {"Red", "Green", "Blue", "Red as Grayscale", "Green as Grayscale", "Blue as Grayscale", "Alpha as Grayscale"};
        QAction* channels[7];
        for (int k = 0; k < 7; ++k) {
            channels[k] = menu.addAction(names[k]);
            channels[k]->setCheckable(true);
            channels[k]->setChecked(impl->view->channel == k + 1);
        }
        menu.addSeparator();
        QMenu* cc = menu.addMenu("Closed Captioning");
        QAction* ccs[4];
        for (int k = 0; k < 4; ++k) {
            ccs[k] = cc->addAction(QString("CC%1").arg(k + 1));
            ccs[k]->setObjectName(QString("overlayCC%1").arg(k + 1));
            ccs[k]->setCheckable(true);
            ccs[k]->setChecked(impl->view->captionChannel == k + 1);
        }
        QAction* m360 = menu.addAction(QString::fromUtf8("360\u00B0 Mode"));
        m360->setObjectName("overlay360");
        m360->setCheckable(true);
        m360->setChecked(impl->view->mode360);
        m360->setEnabled(AppState::get().project().settings.output360);
        QAction* picked = menu.exec(impl->overlays->mapToGlobal(QPoint(0, impl->overlays->height())));
        for (int k = 0; k < 4; ++k)
            if (picked == ccs[k]) {
                setCaptionChannel(impl->view->captionChannel == k + 1 ? 0 : k + 1);
                return;
            }
        if (picked == m360) {
            set360Mode(!impl->view->mode360);
            return;
        }
        for (int k = 0; k < 7; ++k)
            if (picked == channels[k]) {
                setChannel(impl->view->channel == k + 1 ? 0 : k + 1);
                return;
            }
        if (picked == fx) setShowOverlays(!impl->view->showOverlays);
        else if (picked) impl->toggleOverlay(picked == grid2);
    });
    QObject::connect(copy, &QAbstractButton::clicked, [impl] {
        if (impl->view->image().isNull()) return;
        QGuiApplication::clipboard()->setImage(impl->view->image());
        if (impl->view->split && impl->view->splitClipboard) impl->refreshSplit(AppState::get().cursor());
    });
    QObject::connect(save, &QAbstractButton::clicked, this, [this, impl] {
        if (impl->view->image().isNull()) return;
        const AppState& app = AppState::get();
        const QString dir = QSettings().value("preview/snapshotFolder", app.path().empty() ? QDir::homePath() : QFileInfo(qs(app.path())).absolutePath()).toString();
        const QString suggested = QDir(dir).filePath(QString("Snapshot %1.png").arg((qlonglong)app.project().frameAt(app.cursor())));
        const QString f = QFileDialog::getSaveFileName(this, "Save Snapshot to File", suggested, "PNG pictures (*.png);;JPEG pictures (*.jpg)");
        if (!f.isEmpty()) QSettings().setValue("preview/snapshotFolder", QFileInfo(f).absolutePath());
        if (!f.isEmpty() && !impl->view->image().save(f)) { QMessageBox::warning(this, "Save Snapshot", QString("%1 could not be written.").arg(f)); return; }
        if (!f.isEmpty()) {
            std::string err;
            if (AppState::get().importMedia(f.toStdString(), &err)) AppState::get().status(QString("Snapshot saved and added to Project Media: %1").arg(QFileInfo(f).fileName()).toStdString());
        }
    });
    QObject::connect(d->play, &QAbstractButton::clicked, [impl] { if (!impl->pb->playing()) impl->pb->play(); });
    QObject::connect(pause, &QAbstractButton::clicked, [impl] { impl->pb->pause(); });
    QObject::connect(stop, &QAbstractButton::clicked, [impl] { stopPlayback(impl->pb); });
    QObject::connect(more, &QAbstractButton::clicked, [impl, more] { impl->showMenu(more); });
    auto command = [this](const char* name) {
        if (QWidget* w = window())
            if (QAction* a = w->findChild<QAction*>(name)) a->trigger();
    };
    QObject::connect(d->record, &QAbstractButton::clicked, this, [command] { command("record"); });
    QObject::connect(d->playStart, &QAbstractButton::clicked, this, [command] { command("playFromStart"); });
    QObject::connect(d->toEnd, &QAbstractButton::clicked, this, [command] { command("goEnd"); });
    QObject::connect(d->prevFrame, &QAbstractButton::clicked, this, [command] { command("prevFrame"); });
    QObject::connect(d->nextFrame, &QAbstractButton::clicked, this, [command] { command("nextFrame"); });
    QObject::connect(d->button360, &QToolButton::clicked, this, [this](bool on) { set360Mode(on); });
    d->view->captionChannel = std::max(0, std::min(4, QSettings().value("preview/captions", 0).toInt()));
    QObject::connect(d->toStart, &QAbstractButton::clicked, [impl] {
        impl->pb->jumpTo(0);
    });
    QObject::connect(d->loop, &QAbstractButton::clicked, [](bool on) {
        AppState::get().loopOn = on;
        AppState::get().notify(kCursor);
    });

    playback->onFrame = [impl](const QImage& image, double seconds, double ms) {
        // Preferences > Preview Device, "Display frames in video preview window during playback": when cleared, the
        // Video Preview keeps its image while playing (the time goes to the external monitor).
        if (impl->pb->playing() && !vprefs::on("prefs/pdShowInPreview")) {
            impl->lastMs = ms;
            return;
        }
        impl->view->setImage(image);
        if (impl->view->split) impl->refreshSplit(seconds);
        impl->lastMs = ms;
        impl->showRenderMs();
        impl->showSizes();
    };
    d->listener.reset(new ListenerHandle([impl](int changes) {
        if (changes & (kCursor | kProject)) impl->showTime();
        if ((changes & kCursor) && impl->pb->playing() && (!impl->sizesShown.isValid() || impl->sizesShown.elapsed() > 250)) {
            impl->sizesShown.start();
            impl->showSizes();
        }
        if (changes & kProject) impl->showFormat();
        if (changes & (kFxParams | kPlayback | kCursor)) impl->syncButtons();
        if (changes & kPrefs) {
            impl->arrange();
            QSettings s;
            const bool fit = s.value("preview/scaleToFit", true).toBool(), aspect = s.value("preview/simulateAspect", false).toBool();
            if (fit != impl->view->scaleToFit || aspect != impl->view->simulateAspect) {
                impl->view->scaleToFit = fit;
                impl->view->simulateAspect = aspect;
                impl->view->qualityChanged();
                impl->showSizes();
            }
            impl->view->update();
        }
        if (impl->view->overlaySlot() && (changes & (kFxParams | kCursor | kProject | kPlayback))) impl->view->overlayChanged();
    }));

    if (!playback->lastFrame().isNull()) d->view->setImage(playback->lastFrame());
    d->showTime();
    d->showFormat();
    d->showRenderMs();
    d->syncButtons();
    if (QSettings().value("preview/retainSplit", false).toBool() && QSettings().value("preview/splitOn", false).toBool()) d->setSplit(true);
}

void PreviewWidget::setCaptionChannel(int channel)
{
    d->view->captionChannel = channel >= 1 && channel <= 4 ? channel : 0;
    QSettings().setValue("preview/captions", d->view->captionChannel);
    d->view->update();
}
int PreviewWidget::captionChannel() const { return d->view->captionChannel; }

void PreviewWidget::set360Mode(bool on)
{
    d->view->mode360 = on;
    d->sync360();
    d->view->update();
}
bool PreviewWidget::mode360() const { return d->view->mode360; }
void PreviewWidget::set360View(double yaw, double pitch, double fov)
{
    d->view->yaw360 = yaw;
    d->view->pitch360 = std::max(-89.0, std::min(89.0, pitch));
    d->view->fov360 = std::max(20.0, std::min(140.0, fov));
    d->view->update();
}
QStringList PreviewWidget::visibleTransportButtons() const { return d->transportSet; }
void PreviewWidget::setVisibleTransportButtons(const QStringList& set) { d->setTransportSet(set); }

PreviewWidget::~PreviewWidget()
{
    d->pb->onFrame = nullptr;
    d->alive->store(false);
    d->bypassRenderer.reset();
    if (gPlayback == d->pb) gPlayback = nullptr;
}

void PreviewWidget::setQuality(PreviewQuality quality, PreviewSize size)
{
    d->view->quality = quality;
    d->view->size = size;
    d->view->qualityChanged();
    if (d->pb) d->pb->setFullQuality(quality == PreviewQuality::Best);
    d->showSizes();
    QSettings s;
    s.setValue("preview/size", (int)size);
    const char* qk = quality == PreviewQuality::Draft ? "draft" : quality == PreviewQuality::Good ? "good" : quality == PreviewQuality::Best ? "best" : "preview";
    AppState::get().prefs.previewQuality = qk;
    s.setValue("prefs/previewQuality", QString(qk));
}

void PreviewWidget::setOverlayEffect(uint64_t slotId, const FxTarget& target) { d->view->setOverlay(slotId, target); }
uint64_t PreviewWidget::overlayEffect() const { return d->view->overlaySlot(); }
bool PreviewWidget::overlayActive() const { return d->view->overlayActive(); }
bool PreviewWidget::overlayDrawn() const { return d->view->overlayDrawn(); }
bool PreviewWidget::showOverlays() const { return d->view->showOverlays; }
void PreviewWidget::setShowOverlays(bool on)
{
    d->view->showOverlays = on;
    QSettings().setValue("preview/fxOverlays", on);
    d->view->overlayChanged();
}
QWidget* PreviewWidget::pictureWidget() const { return d->view; }
void PreviewWidget::setPicker(std::function<void(int state, const QRectF& area, const QImage& region)> fn)
{
    d->view->picker = fn;
    d->view->setPickerOn((bool)fn);
}

void PreviewWidget::setChannel(int channel)
{
    d->view->channel = channel >= 0 && channel <= 7 ? channel : 0;
    d->view->qualityChanged();
}
int PreviewWidget::channel() const { return d->view->channel; }

void PreviewWidget::setSplitScreen(bool on, bool clipboard)
{
    d->view->splitClipboard = clipboard;
    d->view->splitImage = QImage();
    QSettings().setValue("preview/splitClipboard", clipboard);
    d->setSplit(on);
}
bool PreviewWidget::splitScreen() const { return d->view->split; }
bool PreviewWidget::splitClipboard() const { return d->view->splitClipboard; }
void PreviewWidget::setSplitArea(const QRectF& area)
{
    const QRectF a = area.normalized().intersected(QRectF(0, 0, 1, 1));
    if (a.width() < 0.005 || a.height() < 0.005) return;
    d->view->splitArea = a;
    QSettings().setValue("preview/splitArea", a);
    d->view->update();
}
QRectF PreviewWidget::splitArea() const { return d->view->splitArea; }
bool PreviewWidget::splitPictureReady() const { return !d->view->splitImage.isNull(); }

void PreviewWidget::setDisplayOptions(bool simulateAspect, bool scaleToFit, bool adjustForPlayback)
{
    d->view->simulateAspect = simulateAspect;
    d->view->scaleToFit = scaleToFit;
    d->view->adjustForPlayback = adjustForPlayback;
    QSettings s;
    s.setValue("preview/simulateAspect", simulateAspect);
    s.setValue("preview/scaleToFit", scaleToFit);
    s.setValue("preview/adjustForPlayback", adjustForPlayback);
    d->view->qualityChanged();
    d->showSizes();
}
bool PreviewWidget::simulateAspect() const { return d->view->simulateAspect; }
bool PreviewWidget::scaleToFit() const { return d->view->scaleToFit; }
bool PreviewWidget::adjustForPlayback() const { return d->view->adjustForPlayback; }
void PreviewWidget::setToolbarVisible(bool on)
{
    d->bar->setVisible(on);
    QSettings().setValue("preview/toolbar", on);
}
bool PreviewWidget::toolbarVisible() const { return !d->bar->isHidden(); }
void PreviewWidget::setStatusBarVisible(bool on) { d->setStatusShown(on); }
bool PreviewWidget::statusBarVisible() const { return d->statusShown; }
QRect PreviewWidget::pictureRect() const { return d->view->target(); }

void PreviewWidget::showPicture(const QImage& img)
{
    if (!img.isNull()) {
        d->view->setImage(img);
        return;
    }
    if (!d->pb->lastFrame().isNull()) d->view->setImage(d->pb->lastFrame());
    d->pb->requestFrame();
}
QPointF PreviewWidget::canonicalToView(double x, double y) const { return d->view->canonicalToView(x, y); }

PreviewQuality PreviewWidget::quality() const { return d->view->quality; }
PreviewSize PreviewWidget::size() const { return d->view->size; }
QSize PreviewWidget::previewSize() const { return d->view->previewSize(); }

void PreviewWidget::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    if (d->view->image().isNull()) d->pb->requestFrame();
    d->showSizes();
}

}  // namespace app
}  // namespace reno
