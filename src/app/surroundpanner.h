// VEGAS's Surround Panner window (View > Window > Surround Panner, Ctrl+Alt+3) and the small panner in track headers
// and Mixing Console strips of a 5.1 project (Project Properties > Audio > Master bus mode).
//
// The window shows the panner of the current audio track (or a bus chosen in its list): the room from above with the
// five speakers, the LFE and the pan point. Drag the point (Shift: along the line through the middle, Alt: around a
// circle, Alt+Shift: around the largest circle, Ctrl: finer); with it selected, arrows, Page Up/Down, the wheel and
// the keypad move it. Click a speaker to mute it for this sound. See SurroundPan in src/engine/project.h for what the
// other controls do.
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

// The room and its pan point, for the window and (small, 'compact') for track headers and mixer strips.
// 'get' returns the panner shown, 'set' changes it (inside an AppState edit or drag); null 'get': nothing shown.
class SurroundField : public QWidget {
public:
    explicit SurroundField(bool compact, QWidget* parent = nullptr);
    std::function<const SurroundPan*()> get;
    std::function<void(Project&, const SurroundPan&)> set;
    std::function<void()> onOpenWindow;
    // the pan type (VEGAS's Pan Type) of the track or bus shown, and a way to change it (inside an AppState edit): the
    // shortcut menu of the panner offers them
    std::function<std::string()> panType;
    std::function<void(const std::string&)> setPanType;
    std::string undoName = "Surround Pan Position";
    QPointF point() const;
    QRectF speakerRect(int ch) const;
    QPointF pointPos() const;

    bool handleKey(int key, Qt::KeyboardModifiers mods);

protected:
    QSize sizeHint() const override;
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void contextMenuEvent(QContextMenuEvent* e) override;

private:
    QRectF room() const;
    QPointF toWidget(double x, double y) const;
    void toRoom(const QPointF& w, double& x, double& y) const;
    void moveTo(double x, double y, bool drag);
    bool compact_;
    bool dragging_ = false;
    QPointF pressRoom_;
    double pressX_ = 0, pressY_ = 0;
    bool selected_ = true;
};

class SurroundPannerWindow : public QWidget {
public:
    explicit SurroundPannerWindow(QWidget* parent = nullptr);
    ~SurroundPannerWindow() override;
    void showPanner(uint64_t id);
    uint64_t shownId() const;
    SurroundField* field() const;
    bool handleKey(int key, Qt::KeyboardModifiers mods);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void showEvent(QShowEvent* e) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

SurroundPan* surroundPanOf(Project& p, uint64_t id);
const SurroundPan* surroundPanOf(const Project& p, uint64_t id);
// The pan type (Track::panType, Bus::panType) of the track or bus whose panner this is; "" when there is none.
std::string surroundPanTypeOf(const Project& p, uint64_t id);
void setSurroundPanType(Project& p, uint64_t id, const std::string& type);
// The pan types VEGAS offers, as (key, name).
const std::vector<std::pair<std::string, std::string>>& surroundPanTypes();

}  // namespace app
}  // namespace reno
