// Tools > Video > Red Eye Reduction (VEGAS's window of the same name): corrects red eyes in a still image. Drag from
// the middle
// of an eye to draw a box; move a box by its inside, resize it by its edges; right-click deletes it (Delete / Delete All).
#pragma once
#include "appstate.h"
#include <QDialog>
#include <memory>

namespace reno {
namespace app {

class RedEyeView;

class RedEyeWindow : public QDialog {
public:
    explicit RedEyeWindow(QWidget* parent = nullptr);
    ~RedEyeWindow() override;

    // Shows the media's image (a still). False, leaving the window as it was, if it is not a still.
    bool showMedia(uint64_t mediaId);
    uint64_t media() const { return mediaId_; }
    RedEyeView* view() const { return view_; }

    // The media of the selected video event if it is a still (0 if not), and why not in *why.
    static uint64_t stillOfSelection(std::string* why);

private:
    RedEyeView* view_ = nullptr;
    uint64_t mediaId_ = 0;
    int listener_ = 0;
};

class RedEyeView : public QWidget {
public:
    explicit RedEyeView(QWidget* parent);
    void setPicture(const QImage& img);
    void setMedia(uint64_t id) { media_ = id; selected_ = -1; update(); }
    double zoom() const { return zoom_; }
    void setZoom(double z, QPointF about = QPointF(-1, -1));
    void zoomToFit();
    QPointF scroll() const { return scroll_; }
    void setScroll(QPointF s);
    QPointF toView(QPointF pic) const;
    QPointF toPicture(QPointF view) const;
    QSize pictureSize() const { return picture_.size(); }
    int selected() const { return selected_; }
    std::function<void(QMenu*)> onMenu;

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    std::vector<MediaItem::RedEyeArea> areas() const;
    void commit(const std::vector<MediaItem::RedEyeArea>& areas, const std::string& what);
    int boxAt(QPointF view, int* edge) const;            // edge: bit 1 left, 2 right, 4 top, 8 bottom
    QRectF boxRect(const MediaItem::RedEyeArea& a) const;

    QImage picture_;
    uint64_t media_ = 0;
    double zoom_ = 1.0;
    QPointF scroll_;
    bool fitted_ = true;
    int selected_ = -1;
    int drag_ = 0, dragEdge_ = 0;
    QPointF pressPic_;
    MediaItem::RedEyeArea dragBox_, startBox_;
};

}  // namespace app
}  // namespace reno
