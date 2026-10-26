// Tools > Video > Red Eye Reduction (see redeye.h).
#include "redeye.h"
#include "playback.h"
#include "../engine/builtinfx.h"
#include "../engine/engine.h"
#include "strconv.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cstring>
#include <cmath>

namespace reno {
namespace app {

namespace {
const double kMinZoom = 0.05, kMaxZoom = 16.0;
}  // namespace

RedEyeView::RedEyeView(QWidget* parent) : QWidget(parent)
{
    setObjectName("redEyeView");
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 220);
}

void RedEyeView::setPicture(const QImage& img)
{
    picture_ = img;
    fitted_ = true;
    zoomToFit();
}

std::vector<MediaItem::RedEyeArea> RedEyeView::areas() const
{
    const MediaItem* m = AppState::get().project().findMedia(media_);
    return m ? m->redEye : std::vector<MediaItem::RedEyeArea>();
}

void RedEyeView::commit(const std::vector<MediaItem::RedEyeArea>& list, const std::string& what)
{
    const uint64_t id = media_;
    AppState::get().edit(what, [&](Project& p) {
        if (MediaItem* m = p.findMedia(id)) m->redEye = list;
    });
}

void RedEyeView::zoomToFit()
{
    if (picture_.isNull() || width() < 10 || height() < 10) return;
    zoom_ = std::max(kMinZoom, std::min(kMaxZoom, std::min((double)width() / picture_.width(), (double)height() / picture_.height())));
    fitted_ = true;
    setScroll(QPointF(0, 0));
    update();
}

void RedEyeView::setZoom(double z, QPointF about)
{
    if (picture_.isNull()) return;
    z = std::max(kMinZoom, std::min(kMaxZoom, z));
    if (about.x() < 0) about = QPointF(width() / 2.0, height() / 2.0);
    const QPointF pic = toPicture(about);
    zoom_ = z;
    fitted_ = false;
    setScroll(pic - about / zoom_);
    update();
}

void RedEyeView::setScroll(QPointF s)
{
    const double vw = width() / zoom_, vh = height() / zoom_;
    const double pw = picture_.width(), ph = picture_.height();
    s.setX(pw <= vw ? (pw - vw) / 2 : std::max(0.0, std::min(pw - vw, s.x())));
    s.setY(ph <= vh ? (ph - vh) / 2 : std::max(0.0, std::min(ph - vh, s.y())));
    scroll_ = s;
    update();
}

QPointF RedEyeView::toView(QPointF pic) const { return (pic - scroll_) * zoom_; }
QPointF RedEyeView::toPicture(QPointF view) const { return view / zoom_ + scroll_; }

QRectF RedEyeView::boxRect(const MediaItem::RedEyeArea& a) const
{
    const double pw = picture_.width(), ph = picture_.height();
    return QRectF(toView(QPointF(a.x * pw, a.y * ph)), toView(QPointF((a.x + a.w) * pw, (a.y + a.h) * ph)));
}

int RedEyeView::boxAt(QPointF v, int* edge) const
{
    const std::vector<MediaItem::RedEyeArea> list = areas();
    for (int k = (int)list.size() - 1; k >= 0; --k) {
        const QRectF r = boxRect(list[(size_t)k]);
        if (!r.adjusted(-4, -4, 4, 4).contains(v)) continue;
        int e = 0;
        if (std::fabs(v.x() - r.left()) <= 4) e |= 1;
        if (std::fabs(v.x() - r.right()) <= 4) e |= 2;
        if (std::fabs(v.y() - r.top()) <= 4) e |= 4;
        if (std::fabs(v.y() - r.bottom()) <= 4) e |= 8;
        if (edge) *edge = e;
        return k;
    }
    return -1;
}

void RedEyeView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(40, 40, 40));
    if (picture_.isNull()) {
        p.setPen(QColor(200, 200, 200));
        p.drawText(rect(), Qt::AlignCenter, "Pick a still picture's event, then Tools > Video > Red Eye Reduction.");
        return;
    }
    std::vector<MediaItem::RedEyeArea> list = areas();
    if (drag_ == 1 || drag_ == 2 || drag_ == 3) {
        if (drag_ == 1) list.push_back(dragBox_);
        else if (selected_ >= 0 && selected_ < (int)list.size()) list[(size_t)selected_] = dragBox_;
    }
    QImage shown = picture_;
    if (!list.empty()) {
        ImagePtr img = Image::make(picture_.width(), picture_.height(), PixelDepth::Byte, false);
        const QImage src = picture_.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        for (int y = 0; y < img->h; ++y) std::memcpy(img->row(img->h - 1 - y), src.constScanLine(y), (size_t)img->w * 4);
        ImagePtr fixed = applyRedEye(img, list);
        if (fixed) shown = toQImage(*fixed);
    }
    p.setRenderHint(QPainter::SmoothPixmapTransform, zoom_ < 1.0);
    p.drawImage(QRectF(toView(QPointF(0, 0)), toView(QPointF(picture_.width(), picture_.height()))), shown);
    p.setRenderHint(QPainter::Antialiasing);
    for (size_t k = 0; k < list.size(); ++k) {
        const QRectF r = boxRect(list[k]);
        const bool sel = (int)k == selected_ || (drag_ == 1 && k + 1 == list.size());
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 160), 3));
        p.drawRect(r);
        p.setPen(QPen(sel ? QColor(255, 190, 60) : QColor(240, 240, 240), 1, sel ? Qt::SolidLine : Qt::DashLine));
        p.drawRect(r);
        p.setPen(QPen(QColor(255, 255, 255, 110), 1, Qt::DotLine));
        p.drawEllipse(r);
    }
}

void RedEyeView::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (fitted_) zoomToFit();
    else setScroll(scroll_);
}

void RedEyeView::mousePressEvent(QMouseEvent* e)
{
    if (picture_.isNull() || !media_) return;
    setFocus(Qt::MouseFocusReason);
    const QPointF v = e->position();
    int edge = 0;
    const int k = boxAt(v, &edge);
    const std::vector<MediaItem::RedEyeArea> list = areas();
    if (e->button() == Qt::RightButton) {
        if (k < 0) return;
        selected_ = k;
        update();
        QMenu m(this);
        QObject::connect(m.addAction("&Delete"), &QAction::triggered, this, [this, k] {
            std::vector<MediaItem::RedEyeArea> l = areas();
            if (k < (int)l.size()) l.erase(l.begin() + k);
            selected_ = -1;
            commit(l, "Delete Red Eye Area");
        });
        QObject::connect(m.addAction("Delete &All"), &QAction::triggered, this, [this] {
            selected_ = -1;
            commit(std::vector<MediaItem::RedEyeArea>(), "Delete All Red Eye Areas");
        });
        if (onMenu) onMenu(&m);
        else m.exec(e->globalPosition().toPoint());
        return;
    }
    if (e->button() != Qt::LeftButton) return;
    const QPointF pic = toPicture(v);
    pressPic_ = pic;
    if (k >= 0) {
        selected_ = k;
        startBox_ = dragBox_ = list[(size_t)k];
        drag_ = edge ? 3 : 2;
        dragEdge_ = edge;
    } else {
        selected_ = -1;
        drag_ = 1;
        dragBox_ = MediaItem::RedEyeArea();
        dragBox_.x = pic.x() / picture_.width();
        dragBox_.y = pic.y() / picture_.height();
    }
    update();
}

void RedEyeView::mouseMoveEvent(QMouseEvent* e)
{
    const QPointF v = e->position();
    if (!drag_) {
        int edge = 0;
        const int k = boxAt(v, &edge);
        Qt::CursorShape c = Qt::CrossCursor;
        if (k >= 0) {
            if ((edge & 3) && (edge & 12)) c = ((edge & 1) != 0) == ((edge & 4) != 0) ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor;
            else if (edge & 3) c = Qt::SizeHorCursor;
            else if (edge & 12) c = Qt::SizeVerCursor;
            else c = Qt::SizeAllCursor;
        }
        setCursor(c);
        return;
    }
    const double pw = picture_.width(), ph = picture_.height();
    const QPointF pic = toPicture(v);
    const double dx = (pic.x() - pressPic_.x()) / pw, dy = (pic.y() - pressPic_.y()) / ph;
    if (drag_ == 1) {
        const double cx = pressPic_.x() / pw, cy = pressPic_.y() / ph;
        const double hw = std::fabs(dx), hh = std::fabs(dy);
        dragBox_.x = cx - hw;
        dragBox_.y = cy - hh;
        dragBox_.w = 2 * hw;
        dragBox_.h = 2 * hh;
    } else if (drag_ == 2) {
        dragBox_ = startBox_;
        dragBox_.x += dx;
        dragBox_.y += dy;
    } else {
        MediaItem::RedEyeArea b = startBox_;
        double l = b.x, t = b.y, r = b.x + b.w, btm = b.y + b.h;
        if (dragEdge_ & 1) l = std::min(r - 2 / pw, l + dx);
        if (dragEdge_ & 2) r = std::max(l + 2 / pw, r + dx);
        if (dragEdge_ & 4) t = std::min(btm - 2 / ph, t + dy);
        if (dragEdge_ & 8) btm = std::max(t + 2 / ph, btm + dy);
        dragBox_.x = l;
        dragBox_.y = t;
        dragBox_.w = r - l;
        dragBox_.h = btm - t;
    }
    update();
}

void RedEyeView::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !drag_) return;
    const int was = drag_;
    drag_ = 0;
    std::vector<MediaItem::RedEyeArea> list = areas();
    const double pw = std::max(1, picture_.width()), ph = std::max(1, picture_.height());
    if (was == 1) {
        if (dragBox_.w * pw >= 3 && dragBox_.h * ph >= 3) {
            list.push_back(dragBox_);
            selected_ = (int)list.size() - 1;
            commit(list, "Red Eye Reduction");
        }
    } else if (selected_ >= 0 && selected_ < (int)list.size()) {
        const MediaItem::RedEyeArea& o = list[(size_t)selected_];
        if (o.x != dragBox_.x || o.y != dragBox_.y || o.w != dragBox_.w || o.h != dragBox_.h) {
            list[(size_t)selected_] = dragBox_;
            commit(list, was == 2 ? "Move Red Eye Area" : "Size Red Eye Area");
        }
    }
    update();
}

void RedEyeView::wheelEvent(QWheelEvent* e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0) return;
    setZoom(zoom_ * std::pow(1.25, steps), e->position());
}

void RedEyeView::keyPressEvent(QKeyEvent* e)
{
    const bool shift = (e->modifiers() & Qt::ShiftModifier) != 0;
    const bool keypad = (e->modifiers() & Qt::KeypadModifier) != 0;
    const double vw = width() / zoom_, vh = height() / zoom_;
    const double step = (shift ? 0.5 : 0.1);
    const double pw = picture_.width(), ph = picture_.height();
    QPointF s = scroll_;
    switch (e->key()) {
    case Qt::Key_Left: s.rx() -= vw * step; break;
    case Qt::Key_Right: s.rx() += vw * step; break;
    case Qt::Key_Up: s.ry() -= vh * step; break;
    case Qt::Key_Down: s.ry() += vh * step; break;
    case Qt::Key_PageUp:
        if (shift) s.rx() = 0;
        else s.ry() = 0;
        break;
    case Qt::Key_PageDown:
        if (shift) s.rx() = pw - vw;
        else s.ry() = ph - vh;
        break;
    case Qt::Key_Home:
        if (shift) s.ry() = 0;
        else s.rx() = 0;
        break;
    case Qt::Key_End:
        if (shift) s.ry() = ph - vh;
        else s.rx() = pw - vw;
        break;
    case Qt::Key_Backslash:
    case Qt::Key_Bar:
        if (shift || e->key() == Qt::Key_Bar) s.ry() = (ph - vh) / 2;
        else s.rx() = (pw - vw) / 2;
        break;
    case Qt::Key_Asterisk:
        if (!keypad && !shift) { QWidget::keyPressEvent(e); return; }
        if (shift) s.ry() = (ph - vh) / 2;
        else s.rx() = (pw - vw) / 2;
        break;
    case Qt::Key_C:
        s = QPointF((pw - vw) / 2, (ph - vh) / 2);
        break;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        setZoom(zoom_ * 1.25);
        return;
    case Qt::Key_Minus:
        setZoom(zoom_ / 1.25);
        return;
    case Qt::Key_Delete:
        if (selected_ >= 0) {
            std::vector<MediaItem::RedEyeArea> l = areas();
            if (selected_ < (int)l.size()) l.erase(l.begin() + selected_);
            selected_ = -1;
            commit(l, "Delete Red Eye Area");
        }
        return;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    fitted_ = false;
    setScroll(s);
}

RedEyeWindow::RedEyeWindow(QWidget* parent) : QDialog(parent)
{
    setObjectName("redEyeWindow");
    setWindowTitle("Red Eye Reduction");
    resize(760, 560);
    QVBoxLayout* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    v->setSpacing(4);
    QHBoxLayout* top = new QHBoxLayout();
    QLabel* hint = new QLabel("Drag from the middle of a red eye to draw a box around it. Drag a box to move it or its edges to resize it. Right-click to delete it.", this);
    hint->setWordWrap(true);
    hint->setObjectName("redEyeHint");
    top->addWidget(hint, 1);
    auto button = [&](const char* name, const QString& text, const QString& tip) {
        QToolButton* b = new QToolButton(this);
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        top->addWidget(b);
        return b;
    };
    QToolButton* in = button("redEyeZoomIn", "+", "Zoom in (the mouse wheel zooms about the pointer)");
    QToolButton* out = button("redEyeZoomOut", "-", "Zoom out");
    QToolButton* fit = button("redEyeFit", "Fit", "The whole picture");
    v->addLayout(top);
    view_ = new RedEyeView(this);
    v->addWidget(view_, 1);
    QHBoxLayout* bottom = new QHBoxLayout();
    bottom->addStretch(1);
    QPushButton* close = new QPushButton("Close", this);
    close->setObjectName("redEyeClose");
    bottom->addWidget(close);
    v->addLayout(bottom);
    QObject::connect(in, &QToolButton::clicked, this, [this] { view_->setZoom(view_->zoom() * 1.25); });
    QObject::connect(out, &QToolButton::clicked, this, [this] { view_->setZoom(view_->zoom() / 1.25); });
    QObject::connect(fit, &QToolButton::clicked, this, [this] { view_->zoomToFit(); });
    QObject::connect(close, &QPushButton::clicked, this, [this] { hide(); });
    listener_ = AppState::get().addListener([this](int changes) {
        if (!(changes & kProject) || !mediaId_) return;
        if (!AppState::get().project().findMedia(mediaId_)) {
            mediaId_ = 0;
            view_->setMedia(0);
            view_->setPicture(QImage());
            setWindowTitle("Red Eye Reduction");
        }
        view_->update();
    });
}

RedEyeWindow::~RedEyeWindow() { AppState::get().removeListener(listener_); }

uint64_t RedEyeWindow::stillOfSelection(std::string* why)
{
    const AppState& app = AppState::get();
    const Project& p = app.project();
    bool video = false;
    for (uint64_t id : app.selection()) {
        const Track* t = nullptr;
        const Event* e = p.findEvent(id, &t);
        if (!e || !t || t->type != TrackType::Video) continue;
        video = true;
        const MediaItem* m = p.findMedia(e->mediaId);
        if (m && !m->generated && !m->offline && m->info.isStill) return m->id;
    }
    if (why) *why = video ? "Red Eye Reduction works on still pictures (photos): the selected event is not one."
                          : "Select a still picture's event first: Red Eye Reduction corrects the picture.";
    return 0;
}

bool RedEyeWindow::showMedia(uint64_t mediaId)
{
    AppState& app = AppState::get();
    const MediaItem* m = app.project().findMedia(mediaId);
    if (!m || m->generated || m->offline || !m->info.isStill || !m->info.hasVideo) return false;
    MediaPool pool;
    pool.setOpener(app.mediaOpener);
    ImagePtr pic = pool.videoFrame(1, *m, 0.0, 2048, 2048);
    if (!pic) return false;
    mediaId_ = mediaId;
    view_->setMedia(mediaId);
    view_->setPicture(toQImage(*pic));
    setWindowTitle("Red Eye Reduction - " + qs(m->name.empty() ? m->path : m->name));
    return true;
}

}  // namespace app
}  // namespace reno
