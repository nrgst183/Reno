// The touch interface's look: sizes, colours, fonts and the icons it draws itself (no image files, sharp at any
// screen scale). Sizes are in Qt's device-independent pixels, which on phones and tablets are the platform's dp.
#pragma once
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QPixmap>
#include <QRectF>

class QPainter;
class QWidget;

namespace reno {
namespace app {
namespace touch {

// ---- sizes
// Fingers need about 48 dp to hit a control reliably; nothing that is tapped is smaller.
constexpr int kTarget = 48;
constexpr int kBarH = 52;               // top bar and transport row
constexpr int kToolH = 68;              // the tool bar at the bottom (icon over label)
constexpr int kRulerH = 30;
constexpr int kHeaderW = 52;            // the track column at the timeline's left
constexpr int kRadius = 6;              // corners of events and buttons
constexpr int kHandleW = 18;            // a selected event's trim handles (they reach further out than they are drawn)

// Track heights by kind; a tablet (the shell's wide layout) gets taller ones.
int videoTrackHeight(bool tablet);
int audioTrackHeight(bool tablet);

// ---- colours
namespace color {
const QColor bg(18, 18, 20);
const QColor bar(28, 28, 31);
const QColor sheet(36, 36, 40);
const QColor raised(48, 48, 54);
const QColor line(58, 58, 64);
const QColor text(236, 236, 240);
const QColor dim(154, 154, 163);
const QColor faint(100, 100, 108);
const QColor accent(18, 132, 214);      // Reno's highlight (the desktop palette's)
const QColor accentLight(90, 175, 245);
const QColor danger(229, 72, 77);
const QColor playhead(255, 255, 255);
const QColor video(44, 78, 112);
const QColor videoEdge(78, 128, 176);
const QColor audio(30, 90, 76);
const QColor audioEdge(60, 150, 124);
const QColor wave(120, 224, 186);
const QColor text3(92, 64, 140);         // text and other generated media
const QColor text3Edge(146, 112, 206);
const QColor snap(255, 196, 40);
}  // namespace color

// Fonts at a pixel size (the desktop font is VEGAS's 11 px, too small to read on a phone held at arm's length).
QFont uiFont(int px, bool bold = false);

// ---- icons
enum class Icon {
    Play, Pause, Undo, Redo, Split, Delete, Add, Text, Music, Fx, Speed, Volume, Opacity, Fade, Crop, Copy, Paste,
    Duplicate, More, Back, Export, Check, Close, Transition, Track, Mute, Solo, Lock, Magnet, ToStart, ToEnd,
    Menu, Search, Folder, Save, Media, Settings, Reverse, Freeze, Desktop, Eye, EyeOff, Up, Down, Left, Right,
    Fullscreen, Motion, Keyframe, Marker, Group, Loop, Record
};
void drawIcon(QPainter& p, Icon icon, const QRectF& r, const QColor& c);
QIcon icon(Icon which, const QColor& c = color::text, int size = 24);
// The icon drawn once at a size and screen scale, for widgets that paint many of them each frame.
const QPixmap& iconPixmap(Icon which, const QColor& c, int size, qreal dpr);

// The stylesheet the shell sets on itself, so the desktop's (VEGAS-sized tool buttons, light dialogs) doesn't apply.
QString styleSheet();

}  // namespace touch
}  // namespace app
}  // namespace reno
