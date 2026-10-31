// The main window: menus, tool bars, the timeline with the transport bar under it, the docked windows, the effects window and the
// keyboard shortcuts. It follows VEGAS Pro 18's default layout and default shortcuts. Help > Keyboard Shortcuts shows
// the same list (from
// shortcutList()), searchable. "Window" keys work wherever the focus is in the main window, except that keys without
// Ctrl or Alt go to a text box
// while one is being typed in; "Timeline" keys act when the timeline has the focus (click it, or Alt+0).
//
//  Menu / group       Command                                              Keys
//  -----------------  ---------------------------------------------------  ------------------------------------------
//  File               New... (opens Project Properties)                    Ctrl+N
//                     New (without Project Properties)                     Ctrl+Shift+N
//                     Open... (Reno and VEGAS projects)                    Ctrl+O
//                     Close                                                Ctrl+F4
//                     Save / Save As... / Save Version                     Ctrl+S / Ctrl+Shift+S / Ctrl+Alt+S
//                     Import > Media... (Reno's own key)                   Ctrl+I
//                     Properties...                                        Alt+Enter
//                     Exit                                                 Alt+F4
//  Edit               Undo                                                 Ctrl+Z or Alt+Backspace
//                     Redo                                                 Ctrl+Shift+Z or Ctrl+Y
//                     Cut / Copy / Paste                                   Ctrl+X or Shift+Delete / Ctrl+C or
//                                                                          Ctrl+Insert / Ctrl+V or Shift+Insert
//                     Paste Insert / Paste Repeat...                       Ctrl+Shift+V / Ctrl+B
//                     Delete                                               Delete
//                     Select All / Unselect All                            Ctrl+A / Ctrl+Shift+A
//                     Split                                                S
//                     Trim (to the time selection)                         Ctrl+T
//                     Trim Start / Trim End (to the cursor)                Alt+[ / Alt+]
//                     Rename (the selected event, else the track)          F2
//  Edit > Navigate    Go To... / Set End of Time Selection...              Ctrl+G / Ctrl+Shift+G
//                     Go to Start / Go to End                              Ctrl+Home or W / Ctrl+End
//                     Previous / Next Marker                               Ctrl+Left / Ctrl+Right
//                     Previous / Next Frame                                Alt+Left / Alt+Right
//                     Previous / Next Edit Point (edges and fades)         Ctrl+Alt+Left / Ctrl+Alt+Right
//                     Select Event Start / End (else the event edges)     [ / ]
//                     Cursor to Previous / Next Selected Event Edge        Ctrl+[ / Ctrl+]
//                     Time Select Previous / Next Selected Event           Ctrl+Shift+[ / Ctrl+Shift+]
//                     Center View Around Cursor                            \ (backslash)
//                     Restore Previous Time Selection                      Backspace
//                     Move Cursor to Playback Position                     Alt+Down
//                     Go to Marker or Region 1..10                         1 ... 9, 0
//                     (timeline) Start / End of Time Selection or View     Home / End
//                     (timeline) Cursor Left / Right a pixel (a frame)     Left / Right
//                     (timeline) Extend the Time Selection                 Shift+Left / Shift+Right
//                     (timeline) Cursor by Grid Marks                      Page Up / Page Down
//                     (timeline) Set In / Out of the Time Selection        I / O
//  Edit > Group       Create New / Remove From / Clear / Select All        G / U / Ctrl+U / Shift+G
//  Edit > Tool        Normal / Next / Previous editing tool                Ctrl+D / D / Shift+D
//  Edit > Pitch       Raise / Lower a semitone                             = / -
//                     Raise / Lower a cent                                 Ctrl+= / Ctrl+-
//                     Raise / Lower an octave                              Shift+= / Shift+-
//                     Reset                                                Ctrl+Shift+= or Ctrl+Shift+-
//  (timeline keypad)  Move selected events a frame / a pixel / a track     Num 1, 3 / Num 4, 6 / Num 8, 2
//                     Select Event Start / End (edge trimming)             Num 7 / Num 9
//  View               Focus to Track View                                  Alt+0
//                     Master Bus, Video Preview, Project Media,            Alt+3, Alt+4, Alt+5,
//                     Project Notes, Transitions, Video FX,                Alt+6, Alt+7, Alt+8,
//                     Media Generators, Mixing Console, Video Scopes       Alt+9, Ctrl+Alt+5, Ctrl+Alt+2
//                     Explorer, Edit Details                               Alt+1, Ctrl+Alt+4
//                     Next / Previous Window                               F6 or Ctrl+Tab / Shift+F6 or Ctrl+Shift+Tab
//                     Default Layout / Load / Save Layout 0..9             Alt+D, D / Alt+D, 0..9 / Ctrl+Alt+D, 0..9
//                     Zoom In / Out (small steps, timeline)                Up / Down
//                     Zoom In (to the selection) / Out (large steps)       Ctrl+Up / Ctrl+Down
//                     Increase / Decrease Track Height                     Ctrl+Shift+Up / Ctrl+Shift+Down
//                     Minimize / Restore All Tracks                        ` (grave accent)
//                     Maximize Timeline                                    Ctrl+F11 or Ctrl+Alt+`
//                     Set Track Heights to Default                         Ctrl+`
//                     Minimize / Restore the Window Docking Area           F11 or Alt+`
//                     Zoom In Until Each Thumbnail Is a Frame              Alt+Up
//                     Previous / Next Track                                Alt+Shift+Up / Alt+Shift+Down
//                     Show Waveforms / Event Buttons / Event Headers       Ctrl+Shift+W / Ctrl+Shift+C / Ctrl+Alt+M
//                     Show Envelopes / Show Bus Tracks                     Ctrl+Shift+E / B or Ctrl+Shift+B
//                     (timeline) Shortcut Menu                             Shift+F10 or the menu key
//  Insert             Audio Track / Video Track                            Ctrl+Q / Ctrl+Shift+Q
//                     Marker / Region                                      M / R
//                     Track Volume / Pan Envelope: insert, show, hide      V / P
//                     ... remove it                                        Shift+V / Shift+P
//                     Next / Previous Envelope of the Track                E / Shift+E
//  Options            Quantize to Frames / Enable Snapping                 Alt+F8 / F8
//                     Snap to Grid / Snap to Markers                       Ctrl+F8 / Shift+F8
//                     Auto Ripple / Ignore Event Grouping                  Ctrl+L / Ctrl+Shift+U
//                     Loop Playback                                        Q or Ctrl+Shift+L
//  Tracks             Mute / Solo the selected track                       Z / X
//                     ... and take the others out of mute / solo           Shift+Z / Shift+X
//  Playback           Play / Stop (back to where it began)                 Space
//                     Stop                                                 Esc
//                     Play / Pause                                         Enter or Ctrl+F12
//                     Play From Start                                      Shift+Space or Shift+F12
//                     Play from Any Window                                 Ctrl+Space or F12
//                     Shuttle: J back, K pause, L forward                  J / K / L (see below)
//
// Space and Esc stop as VEGAS does: back to where playing began, or where it stopped if Preferences say the cursor
// follows playback. J, K and L
// shuttle as in VEGAS: L plays forward, each press faster (1, 1.5, 2, 4, 10, 20 times), J the same backwards; K slows to
// a quarter speed, then J and L
// step the rate by a quarter. The Rate slider under the track headers shuttles at the rate it is dragged to (-2 .. 2)
// and springs back (stopping)
// when released; double-clicking its label sets the rate Play plays at. Edge trimming from the keyboard works as in
// VEGAS: [ / ] (Num 7 / 9)
// select the event's start / end, the keypad's 1, 3, 4, 6 then trim it (Ctrl: time stretch, Alt: slip trim, Ctrl+Alt:
// slide the edit, Shift: ignore
// grouping), Num 5 works on both sides of an edit, Esc leaves; F3 / F9 jog. Keys for the windows and tools added in
// mainwindow_tools.cpp are made
// there and listed below. VEGAS defaults Reno does not have yet are listed as "not available", with the reason.
#include "cachedscroll.h"
#include "audioproxyui.h"
#include "mainwindow.h"
#include "mainwindow_internal.h"
#include "logo.h"
#include "readoutlabel.h"
#include "vprefs.h"
#include "prefsapply.h"
#include "vegasextras.h"
#include "man2dialogs.h"
#include "outputcmds.h"
#include "startup.h"
#include "recovery.h"
#include "../engine/timefmt.h"
#include "pluginchooser.h"
#include "timeline.h"
#include "fxwindow.h"
#include "mixer.h"
#include "editdialogs.h"
#include "mainwindow_tools.h"
#include "../vegas/vegas.h"
#include "renderdialog.h"
#include "keyboard.h"
#include "trackdefaults.h"
#include "smalldialogs.h"
#include <QAbstractSpinBox>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QGuiApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QDockWidget>
#include <QFileDialog>
#include <QCheckBox>
#include <QGridLayout>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QProcess>
#include <QHash>
#include <QIcon>
#include <QProxyStyle>
#include <QPushButton>
#include <QProgressDialog>
#include <QStyleOption>
#include <QRegularExpression>
#include <QSettings>
#include <QSlider>
#include <QStatusBar>
#include <QStyleHints>
#include <QStorageInfo>
#include <QStyleFactory>
#include <QTextDocument>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include "automationctl.h"
#include <QToolBar>
#include <QToolButton>
#include <QTabBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWhatsThis>
#include <QTabWidget>
#include <QTextBrowser>
#include <QListWidget>
#include <QDateTime>
#include <QScrollArea>
#include <QSignalBlocker>
#include <cmath>
#include <atomic>
#include <thread>
#include <algorithm>
#include <map>
#include <set>

namespace reno {
namespace app {

// ================================================================================================ the theme
namespace {

// VEGAS Pro 18's dark scheme, measured from its window.
const QColor kGap(35, 35, 35), kDark(45, 45, 45), kBar(55, 55, 55), kTool(70, 70, 70), kRim(130, 130, 130);

// Fusion with scroll bars drawn as VEGAS draws them.
class RenoStyle : public QProxyStyle {
public:
    RenoStyle() : QProxyStyle(QStyleFactory::create("Fusion")) {}

private:
    mutable QHash<int, QIcon> icons_;

public:

    int pixelMetric(PixelMetric m, const QStyleOption* o, const QWidget* w) const override
    {
        if (m == PM_ScrollBarExtent) return 17;
        // VEGAS's least scroll box is 20 pixels; the timeline's horizontal bar sizes its own (TimelineWidget updateBars)
        if (m == PM_ScrollBarSliderMin) {
            const auto* cb = dynamic_cast<const CachedScrollBar*>(w);
            return cb && cb->onEdgeDrag ? 20 : 24;
        }
        // VEGAS keeps a dragged scroll box with the mouse however far it goes; Windows' hint would snap it back
        if (m == PM_MaximumDragDistance) return -1;
        return QProxyStyle::pixelMetric(m, o, w);
    }

    // Fusion reads and decodes the title bar button pictures of a dock (close, float) from Qt's resources every time a dock
    // is made or floated: about a fifth of the start-up time with the 30 or so docks of the default layout. They never
    // change, so each is read once.
    QIcon standardIcon(StandardPixmap sp, const QStyleOption* o, const QWidget* w) const override
    {
        switch (sp) {
        case SP_TitleBarCloseButton:
        case SP_TitleBarNormalButton:
        case SP_TitleBarMinButton:
        case SP_TitleBarMaxButton:
        case SP_DockWidgetCloseButton: {
            auto it = icons_.find((int)sp);
            if (it == icons_.end()) it = icons_.insert((int)sp, QProxyStyle::standardIcon(sp, o, w));
            return it.value();
        }
        default: return QProxyStyle::standardIcon(sp, o, w);
        }
    }

    // Docked window tab close button: a plain light cross as in VEGAS (Fusion's boxed image showed as a red box).
    void drawPrimitive(PrimitiveElement pe, const QStyleOption* o, QPainter* p, const QWidget* w) const override
    {
        if (pe != PE_IndicatorTabClose || !o) {
            QProxyStyle::drawPrimitive(pe, o, p, w);
            return;
        }
        const bool hot = (o->state & State_MouseOver) && (o->state & State_Enabled);
        const bool down = (o->state & State_Sunken) != 0;
        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(o->rect).adjusted(1, 1, -1, -1);
        if (hot || down) {
            p->setPen(Qt::NoPen);
            p->setBrush(down ? QColor(255, 255, 255, 70) : QColor(255, 255, 255, 38));
            p->drawRoundedRect(r, 2.0, 2.0);
        }
        const QPointF c = r.center();
        const double a = std::min(r.width(), r.height()) * 0.22;
        p->setPen(QPen(hot ? QColor(255, 255, 255) : QColor(200, 200, 200), 1.5, Qt::SolidLine, Qt::RoundCap));
        p->drawLine(QPointF(c.x() - a, c.y() - a), QPointF(c.x() + a, c.y() + a));
        p->drawLine(QPointF(c.x() + a, c.y() - a), QPointF(c.x() - a, c.y() + a));
        p->restore();
    }

    void drawComplexControl(ComplexControl cc, const QStyleOptionComplex* o, QPainter* p, const QWidget* w) const override
    {
        const QStyleOptionSlider* s = qstyleoption_cast<const QStyleOptionSlider*>(o);
        if (cc != CC_ScrollBar || !s) {
            QProxyStyle::drawComplexControl(cc, o, p, w);
            return;
        }
        const bool horizontal = s->orientation == Qt::Horizontal;
        p->save();
        p->fillRect(s->rect, kDark);
        const QColor ink = (s->state & State_Enabled) ? QColor(220, 220, 220) : QColor(110, 110, 110);
        for (SubControl sc : {SC_ScrollBarSubLine, SC_ScrollBarAddLine}) {
            const QRect r = subControlRect(cc, s, sc, w);
            if (r.isEmpty()) continue;
            if ((s->activeSubControls & sc) && (s->state & State_Sunken)) p->fillRect(r, kTool);
            const QPointF c = QRectF(r).center();
            const double a = 3.5;
            QPolygonF tri;
            const bool back = sc == SC_ScrollBarSubLine;
            if (horizontal) {
                if (back) tri << QPointF(c.x() - a * 0.6, c.y()) << QPointF(c.x() + a * 0.6, c.y() - a) << QPointF(c.x() + a * 0.6, c.y() + a);
                else tri << QPointF(c.x() + a * 0.6, c.y()) << QPointF(c.x() - a * 0.6, c.y() - a) << QPointF(c.x() - a * 0.6, c.y() + a);
            } else {
                if (back) tri << QPointF(c.x(), c.y() - a * 0.6) << QPointF(c.x() - a, c.y() + a * 0.6) << QPointF(c.x() + a, c.y() + a * 0.6);
                else tri << QPointF(c.x(), c.y() + a * 0.6) << QPointF(c.x() - a, c.y() - a * 0.6) << QPointF(c.x() + a, c.y() - a * 0.6);
            }
            p->setRenderHint(QPainter::Antialiasing, true);
            p->setPen(Qt::NoPen);
            p->setBrush(ink);
            p->drawPolygon(tri);
            p->setRenderHint(QPainter::Antialiasing, false);
        }
        const QRect h = subControlRect(cc, s, SC_ScrollBarSlider, w);
        if (!h.isEmpty()) {
            const bool hot = (s->activeSubControls & SC_ScrollBarSlider) && (s->state & (State_MouseOver | State_Sunken));
            const QRect r = horizontal ? h.adjusted(1, 1, -1, -1) : h.adjusted(1, 1, -1, -1);
            p->fillRect(r, hot ? QColor(86, 86, 86) : kTool);
            const QColor mark = kDark;
            if (horizontal) {
                p->fillRect(QRect(r.left() + 6, r.top() + 4, 2, r.height() - 8), mark);
                p->fillRect(QRect(r.right() - 7, r.top() + 4, 2, r.height() - 8), mark);
            } else {
                const int cy = r.center().y();
                p->fillRect(QRect(r.left() + 4, cy - 2, r.width() - 8, 2), mark);
                p->fillRect(QRect(r.left() + 4, cy + 2, r.width() - 8, 2), mark);
            }
        }
        p->restore();
    }
};

}  // namespace

// Dark colours for the whole application: VEGAS Pro 18's scheme, narrow headers, Segoe UI 8 pt, dark menus.
void applyTheme()
{
    static bool first = true;
    // Dark everywhere: Qt 6.8 gives windows a dark title bar when told the app is dark; Windows' own styles ignore the palette.
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Dark);
#endif
    if (first) QApplication::setStyle(new RenoStyle());
    first = false;
    QPalette p;
    const QColor text(220, 220, 220), accent(18, 132, 214);
    p.setColor(QPalette::Window, kBar);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, kDark);
    p.setColor(QPalette::AlternateBase, QColor(50, 50, 50));
    p.setColor(QPalette::ToolTipBase, QColor(45, 45, 45));
    p.setColor(QPalette::ToolTipText, QColor(230, 230, 230));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, kTool);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, QColor(110, 180, 245));
    p.setColor(QPalette::PlaceholderText, QColor(140, 140, 140));
    p.setColor(QPalette::Light, QColor(96, 96, 96));
    p.setColor(QPalette::Midlight, QColor(80, 80, 80));
    p.setColor(QPalette::Mid, QColor(48, 48, 48));
    p.setColor(QPalette::Dark, kGap);
    p.setColor(QPalette::Shadow, QColor(14, 14, 14));
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
        p.setColor(QPalette::Disabled, r, QColor(118, 118, 118));
    QApplication::setPalette(p);
    // VEGAS text is Segoe UI 8 pt (a sans-serif of the same widths elsewhere)
    QFont f = QApplication::font();
#ifdef _WIN32
    f.setFamily("Segoe UI");
    f.setPointSizeF(8.25);
#else
    f.setPixelSize(10);
#endif
    QApplication::setFont(f);
    // Menus are dark (VEGAS 18 leaves them to Windows, which draws them light), at the system menu text size.
#ifdef _WIN32
    const QString menuFont = "font-family: 'Segoe UI'; font-size: 9pt;";
#else
    const QString menuFont = "font-size: 11px;";
#endif
    qApp->setStyleSheet(
        "QToolTip { color: #e6e6e6; background-color: #2d2d2d; border: 1px solid #6e6e6e; padding: 1px; }"
        "QMenuBar { background: #2d2d2d; color: #dcdcdc; border: none; padding: 2px 0px 3px 0px; " + menuFont + " }"
        "QMenuBar::item { padding: 2px 5px; background: transparent; color: #dcdcdc; }"
        "QMenuBar::item:selected { background: #464646; }"
        "QMenuBar::item:pressed { background: #1284d6; color: #ffffff; }"
        "QMenu { background: #2d2d2d; color: #dcdcdc; border: 1px solid #5a5a5a; padding: 2px; " + menuFont + " }"
        "QMenu::item { padding: 3px 28px 3px 26px; color: #dcdcdc; background: transparent; }"
        "QMenu::item:selected { background: #1284d6; color: #ffffff; }"
        "QMenu::item:disabled { color: #787878; }"
        "QMenu::item:disabled:selected { background: #3c3c3c; color: #787878; }"
        "QMenu::separator { height: 1px; background: #4a4a4a; margin: 3px 4px 3px 26px; }"
        "QMenu::indicator { width: 13px; height: 13px; left: 6px; }"
        "QToolBar { background: #464646; border: none; spacing: 0px; padding: 3px 4px; }"
        "QToolBar::separator { background: #2d2d2d; width: 1px; margin: 4px 5px; }"
        "QToolButton { border: 1px solid transparent; border-radius: 0px; padding: 2px; color: #dcdcdc; }"
        "QToolButton:hover { background: #5a5a5a; border-color: #6e6e6e; }"
        "QToolButton:pressed { background: #2e8fd6; }"
        "QToolButton:checked { background: #12b0ff; border-color: #12b0ff; }"
        "QToolButton[popupMode=\"1\"] { padding-right: 11px; }"
        "QToolButton::menu-button { border: none; width: 10px; }"
        "QToolButton::menu-arrow { width: 7px; height: 7px; }"
        "QMainWindow::separator { background: #232323; width: 7px; height: 7px; }"
        "QMainWindow::separator:hover { background: #3a3a3a; }"
        "QDockWidget { color: #dcdcdc; }"
        "QDockWidget::title { background: #464646; padding: 2px 6px; text-align: left; }"
        "QTabBar { background: #232323; }"
        "QTabBar::tab { background: #2d2d2d; color: #b4b4b4; padding: 2px 10px 3px 10px; border: none; margin-right: 2px; }"
        "QTabBar::tab:selected { background: #464646; color: #e6e6e6; }"
        "QTabBar::tab:hover:!selected { background: #383838; }"
        "QStatusBar { background: #373737; color: #dcdcdc; border-top: 1px solid #464646; }"
        "QStatusBar::item { border: none; }"
        "QStatusBar QLabel { padding: 0px 4px; }"
        "QHeaderView::section { background: #3c3c3c; color: #d6d6d6; border: none; border-right: 1px solid #2d2d2d; border-bottom: 1px solid #2d2d2d; padding: 3px 5px; }");
}

// ================================================================================================ the shortcut list
const std::vector<ShortcutInfo>& shortcutList()
{
    typedef ShortcutInfo S;
    static const std::vector<ShortcutInfo> list = {
        {"File", "New... (with Project Properties)", "Ctrl+N", S::Window, ""},
        {"File", "New (without Project Properties)", "Ctrl+Shift+N", S::Window, ""},
        {"File", "Open...", "Ctrl+O", S::Window, "VEGAS projects (.veg), which are Reno's own; Reno's older .reno files open too"},
        {"File", "Close", "Ctrl+F4", S::Window, ""},
        {"File", "Save", "Ctrl+S", S::Window, ""},
        {"File", "Save As...", "Ctrl+Shift+S", S::Window, ""},
        {"File", "Save Version", "Ctrl+Alt+S", S::Window, "saves as name_01, name_02..."},
        {"File", "Import Media...", "Ctrl+I", S::Window, "Reno's own key"},
        {"File", "Import > Image Sequence...", "", S::Window, "numbered pictures (name_0001.png ...) as one video at a frame rate you choose"},
        {"File", "Properties...", "Alt+Enter", S::Window, "Video, Audio, Ruler, Summary; \"Start all new projects with these settings\""},
        {"File", "Render As...", "", S::Window, "VEGAS 18's dialog: folder and name, formats with VEGAS's templates, search, favourites, Customize Template, Render Options"},
        {"File", "Exit", "Alt+F4", S::Window, ""},
        {"Edit", "Undo", "Ctrl+Z or Alt+Backspace", S::Window, ""},
        {"Edit", "Redo", "Ctrl+Shift+Z or Ctrl+Y", S::Window, ""},
        {"Edit", "Cut", "Ctrl+X or Shift+Delete", S::Window, "ripples with Auto Ripple on; locked events are copied but stay"},
        {"Edit", "Copy", "Ctrl+C or Ctrl+Insert", S::Window, ""},
        {"Edit", "Paste", "Ctrl+V or Shift+Insert", S::Window, ""},
        {"Edit", "Paste Insert", "Ctrl+Shift+V", S::Window, "pushes later events along"},
        {"Edit", "Paste Repeat...", "Ctrl+B", S::Window, "how many times, end to end or evenly spaced (seconds, frames, measures or beats)"},
        {"Edit", "Delete", "Delete", S::Window, "ripples with Auto Ripple on; locked events stay"},
        {"Edit", "Select All", "Ctrl+A", S::Window, ""},
        {"Edit", "Unselect All", "Ctrl+Shift+A", S::Window, ""},
        {"Edit", "Split", "S", S::Window, "not locked events"},
        {"Edit", "Smart Split", "Alt+S", S::Window, "removes the time selection from the selected events and joins the rest with a five-frame cross-fade"},
        {"Edit", "Detect Scenes...", "", S::Window, "splits the selected video events where the picture cuts to another shot"},
        {"Edit", "Trim (to the time selection)", "Ctrl+T", S::Window, ""},
        {"Edit", "Trim Start (to the cursor)", "Alt+[", S::Window, ""},
        {"Edit", "Trim End (to the cursor)", "Alt+]", S::Window, ""},
        {"Edit", "Rename the selected event (else the track)", "F2", S::Window, ""},
        {"Edit", "Take: Next Take", "T", S::Window, "the selected events (and those grouped with them that have takes)"},
        {"Edit", "Take: Previous Take", "Shift+T", S::Window, ""},
        {"Edit", "Take: Add / Choose Active / Delete Active / Delete / Rename Active", "", S::Window, "Edit > Take; or drop media on an event with Ctrl held to add it as takes"},
        {"Edit", "Switches: Lock", "", S::Window, "Edit > Switches > Lock, or the Lock button: locked events cannot be moved, trimmed, split or deleted"},
        {"Edit", "Paste Event Attributes / Selectively Paste Event Attributes", "", S::Window, "the copied event's effects, pan/crop, envelopes, fades, level, rate and switches onto the selected events (all, or those ticked)"},
        {"Edit", "Event Heal / Close Gaps", "", S::Window, "Edit menu or an event's menu: split pieces joined again; the gaps before the selected events closed"},
        {"Edit", "Switches: Invert Phase / Normalize / Maintain Aspect Ratio / Reduce Interlace Flicker", "", S::Window, "Edit > Switches or an event's menu (Normalize measures the event's sound and raises its peak to -0.1 dB)"},
        {"Edit", "Group: Cut All / Copy All / Delete All", "", S::Window, "the whole group of the selected events"},
        {"Edit", "Undo All / Clear Edit History", "", S::Window, ""},
        {"Edit", "Select All After Cursor / Select Events to End", "", S::Window, "Edit > Select, or an event's or track's menu"},
        {"Edit", "Editing Tool: Shuffle / Slip / Slide / Time Stretch/Compress / Split Trim", "", S::Window, "Edit > Editing Tool or the normal tool button's drop-down: changes what a plain drag on an event or edge does (Alt, Ctrl+Alt, Ctrl+Alt+Shift and Ctrl drags still work with any tool)"},
        {"Tools", "Audio: Open Copy in Audio Editor / Edit Source Project", "", S::Window, "opens a copy of the event's file in the audio editor and the event plays the copy; a nested project's .veg opens as in Open Nested Timeline"},
        {"Insert", "Slideshow...", "", S::Window, "VEGAS's Slideshow Creator: pictures one after another at the cursor or the project's end, with optional transitions and Pan / Zoom (one undo step)"},
        {"Insert", "Add Missing Streams for Selected Event", "", S::Window, "the file's other stream (sound or picture) as its own event at the same place, grouped with it"},
        {"Edit", "Create Subclip (an event's menu)", "", S::Window, "the part of the file the event plays becomes a subclip in Project Media, which the event (and those grouped with it) then play"},
        {"Options", "Grid Spacing: Ruler Marks / Minutes / Seconds / Milliseconds / Frames / Measures / Quarter, 8th, 16th, 32nd Notes (and triplets)", "", S::Window, "sets the grid lines, Snap to Grid and Page Up / Down steps (notes follow the project tempo)"},
        {"Options", "Ruler Format: Samples / Time / Seconds / Time & Frames / Absolute Frames / Measures & Beats / Feet and Frames 16mm, 35mm / SMPTE Film Sync IVTC, Film Sync, EBU, Non-Drop, Drop, 30 / Audio CD Time", "", S::Window, "VEGAS's fifteen time formats for the ruler, time display, readouts and typed times (also in the right-click Time Format)"},
        {"Options", "Ruler Format: Set Time at Cursor", "", S::Window, "type the time the cursor should read; the ruler's start moves to match (not in Audio CD Time)"},
        {"View", "Time display: Text Color / Background Color", "", S::Window, "the time display's right-click menu (with its Time Format)"},
        {"Navigate", "Cursor / selection end / selection length readouts", "", S::Window, "double-click a readout under the timeline to type a time into it (Enter puts it into effect, Esc leaves it)"},
        {"Edit", "Event: Insert/Remove Envelope > Freeze Frame at Cursor; Synchronize > By Moving / By Slipping", "", S::Window, "video event menu: holds the frame at the cursor to the event's end (velocity envelope); Synchronize moves or slips an event back in step with its grouped event"},
        {"Edit", "Track: Cut / Copy / Paste / Delete", "", S::Window, "track header menu: the selected tracks (Ctrl+click headers) with events, effects and envelopes; Paste puts the copies under the clicked track"},
        {"Options", "Preferences > Video: Thumbnails to show in video events / Show source frame numbers on event thumbnails as", "", S::Window, "All, Head-Center-Tail, Head-Tail, Head or None; the media's frame number, time or timecode on each picture"},
        {"File", "Restore the auto-saved project after a crash", "", S::Window, "on start, when the last run did not close (VEGAS's question): its auto-saved copy opens untitled, for Save As"},
        {"View", "Event Media Markers: Show Marker Labels; Event Length", "", S::Window, "View menu: the media markers' names; each event's length in its bottom right corner"},
        {"Edit", "Switches: Use Project Resample Mode / Force Resample / Disable Resample", "", S::Window, "Force blends the two media frames either side of each project frame, Disable shows the nearest one, Use Project follows Project Properties' Resample mode"},
        {"Insert", "Empty Event / Text / Generated Media / Time", "", S::Window, "Insert menu (Empty Event over the time selection on the selected track; Time at the cursor)"},
        {"Insert", "Generated Media without VEGAS: Solid Color, Color Gradient, Checkerboard, Noise Texture, Test Pattern, Titles & Text", "", S::Window,
         "Reno's own built-in generators (and 22 of VEGAS's most-used video effects: Gaussian Blur, Chroma Keyer, Color Corrector (Secondary)...), with VEGAS's names and settings, wherever VEGAS's OpenFX plug-ins are not installed or do not load"},
        {"Project Media", "Create Video Proxy / Match Project Video Settings (right-click a file)", "", S::Window,
         "a small copy (<file>.sfvp0, as VEGAS names them) that the Video Preview plays below Best quality; renders use the original"},
        {"Project Media", "Properties > Media > Alpha channel", "", S::Window, "VEGAS's override of a file's transparency: Undefined (as the file says), None, Straight, Premultiplied"},
        {"Tools", "Audio: Apply Non-Real-Time Event FX...", "", S::Window, "also in the audio event's right-click menu: runs the selected audio events through a chain of plug-ins into new files \"<media> Take 2\", \"Take 3\"... that become the events' active takes; bind a key in Options > Customize Keyboard"},
        {"Tools", "Clean Project Media", "", S::Window, "the media no event uses taken out of the project"},
        {"Tools", "Rebuild Offline Nested Audio", "", S::Window, "rebuilds missing, outdated or cancelled audio proxies (.sfap0) of nested projects; until then their sound is mixed as it plays"},
        {"Tools", "Scripting: Run Script", "", S::Window, "runs a VEGAS script (.cs, .js, .vb or compiled .dll) against Reno's ScriptPortal.Vegas as one undo step (needs .NET Framework 4 on Windows)"},
        {"Tools", "Scripting: Rescan Script Menu Folder; the scripts in VEGAS's Script Menu folders", "", S::Window, "scripts in Documents\\Vegas Script Menu, VEGAS's Script Menu folders and Reno's tools\\scripts are listed under Tools > Scripting; each can get keys in Options > Customize Keyboard (Script.<name>)"},
        {"Edit", "Open in Audio Editor", "Ctrl+E", S::Window, "the program set in Options > Preferences > Editing (asked for the first time)"},
        {"Edit", "Render to New Track", "Ctrl+M", S::Window, "renders the time selection (or whole project) with any Render As template; the file goes on new tracks at the top"},
        {"Edit", "Nested Timelines: Create Nested Timeline", "Alt+C", S::Window, "saves the selected events as a .veg next to the project and puts one event of it in their place"},
        {"Edit", "Nested Timelines: Open Nested Timeline", "Alt+N", S::Window, "opens the selected event's .veg in another Reno"},
        {"Edit", "Nested Timelines: Open Parent", "Alt+P", S::Window, "brings forward the Reno with the project this nested timeline was opened from (or opens it)"},
        {"Edit", "Rebuild Audio Peaks", "Shift+F5", S::Window, "the waveforms (and pictures) read again from the files"},
        {"Edit", "Convert Cut to Cross-fade", "Num+/", S::Window, "the cut at the cursor on the selected events' tracks (else the current track), as long as Preferences > Editing says"},
        {"Edit", "Convert Cut to Dissolve", "Num+*", S::Window, "with the Dissolve transition if installed"},
        {"Edit", "Convert Cut to Linear Wipe", "Num+-", S::Window, "with the Linear Wipe transition if installed"},
        {"Edit", "Convert Transition to Cut", "Ctrl+Num+/", S::Window, "the cross-fade or transition at the cursor"},
        {"Edit", "Post-Edit Ripple: Affected Tracks", "F", S::Window, "right after a trim, move, delete or paste: what follows on those tracks closes up or makes room"},
        {"Edit", "Post-Edit Ripple: Affected Tracks, Bus Tracks, Markers, and Regions", "Ctrl+F", S::Window, ""},
        {"Edit", "Post-Edit Ripple: All Tracks, Markers, and Regions", "Ctrl+Shift+F", S::Window, ""},
        {"Edit", "Select Event Start (edge trimming)", "[", S::Window, "with an event selected; again: the previous edge on its track; with none selected: the cursor goes to the previous event edge"},
        {"Edit", "Select Event End (edge trimming)", "]", S::Window, "with an event selected; again: the next edge; with none selected: the cursor goes to the next event edge"},
        {"Navigate", "Go To...", "Ctrl+G", S::Window, "type a time in the time display over the track headers (ruler format or seconds); Enter goes there"},
        {"Navigate", "Set End of Time Selection...", "Ctrl+Shift+G", S::Window, "typed in place in the selection's end under the timeline"},
        {"Navigate", "Go to Start", "Ctrl+Home or W", S::Window, ""},
        {"Navigate", "Go to End", "Ctrl+End", S::Window, ""},
        {"Navigate", "Start of Time Selection (or of the View)", "Home", S::Timeline, ""},
        {"Navigate", "End of Time Selection (or of the View)", "End", S::Timeline, ""},
        {"Navigate", "Previous Marker", "Ctrl+Left", S::Window, "markers and region ends; then the project's start (track list focused: track volume down)"},
        {"Navigate", "Next Marker", "Ctrl+Right", S::Window, "markers and region ends; then the project's end (track list focused: track volume up)"},
        {"Navigate", "Previous Frame", "Alt+Left", S::Window, ""},
        {"Navigate", "Next Frame", "Alt+Right", S::Window, ""},
        {"Navigate", "Previous Edit Point", "Ctrl+Alt+Left", S::Window, "event edges and fade ends"},
        {"Navigate", "Next Edit Point", "Ctrl+Alt+Right", S::Window, "event edges and fade ends"},
        {"Navigate", "Cursor to Previous Selected Event Edge", "Ctrl+[", S::Window, "in edge-trimming mode the edge to trim goes along"},
        {"Navigate", "Cursor to Next Selected Event Edge", "Ctrl+]", S::Window, ""},
        {"Navigate", "Time Select Previous Selected Event", "Ctrl+Shift+[", S::Window, ""},
        {"Navigate", "Time Select Next Selected Event", "Ctrl+Shift+]", S::Window, ""},
        {"Navigate", "Cursor Left / Right", "Left or Right", S::Timeline, "a pixel, at least a frame while quantizing"},
        {"Navigate", "Extend the Time Selection", "Shift+Left or Shift+Right", S::Timeline, ""},
        {"Navigate", "Cursor by Grid Marks", "Page Up or Page Down", S::Timeline, ""},
        {"Navigate", "Set In / Out of the Time Selection", "I or O", S::Timeline, ""},
        {"Navigate", "Center View Around Cursor", "\\", S::Window, ""},
        {"Navigate", "Restore Previous Time Selection", "Backspace", S::Window, "the last five"},
        {"Navigate", "Move Cursor to Playback Position", "Alt+Down", S::Window, ""},
        {"Navigate", "Go to Marker or Region 1 ... 10", "1, 2 ... 9, 0", S::Window, "a region is selected (with multicamera editing on, 1 ... 9 choose a camera)"},
        {"Navigate", "Jog Left", "F3", S::Window, "a frame back, with its sound; in edge-trimming mode the edge is trimmed a frame"},
        {"Navigate", "Jog Right", "F9", S::Window, ""},
        {"Navigate", "Select Loop Region", "Shift+Q", S::Window, "the last loop region becomes the time selection again"},
        {"Navigate", "Previous CD Track", "Ctrl+,", S::Window, ""},
        {"Navigate", "Next CD Track", "Ctrl+.", S::Window, ""},
        {"Navigate", "Previous CD Index or Region", ",", S::Window, ""},
        {"Navigate", "Next CD Index or Region", ".", S::Window, ""},
        {"Group", "Create New", "G", S::Window, ""},
        {"Group", "Remove From", "U", S::Window, ""},
        {"Group", "Clear", "Ctrl+U", S::Window, ""},
        {"Group", "Select All", "Shift+G", S::Window, ""},
        {"Editing Tool", "Normal", "Ctrl+D", S::Window, ""},
        {"Editing Tool", "Next Tool", "D", S::Window, "Normal, Envelope (only envelopes change; a drag selects their points), Selection, Zoom"},
        {"Editing Tool", "Previous Tool", "Shift+D", S::Window, ""},
        {"Pitch", "Raise a Semitone", "=", S::Window, "selected audio events"},
        {"Pitch", "Lower a Semitone", "-", S::Window, ""},
        {"Pitch", "Raise a Cent", "Ctrl+=", S::Window, ""},
        {"Pitch", "Lower a Cent", "Ctrl+-", S::Window, ""},
        {"Pitch", "Raise an Octave", "Shift+=", S::Window, ""},
        {"Pitch", "Lower an Octave", "Shift+-", S::Window, ""},
        {"Pitch", "Reset", "Ctrl+Shift+= or Ctrl+Shift+-", S::Window, ""},
        {"Keypad", "Move Selected Events a Frame", "Num 1 or Num 3", S::Timeline, "(in edge-trimming mode: trim the edge a frame)"},
        {"Keypad", "Move Selected Events a Pixel", "Num 4 or Num 6", S::Timeline, "(in edge-trimming mode: trim the edge a pixel)"},
        {"Keypad", "Move Selected Events a Track", "Num 8 or Num 2", S::Timeline, ""},
        {"Keypad", "Select Event Start / End (edge trimming)", "Num 7 or Num 9", S::Timeline, "as [ and ]; in expanded edit mode, one side of the edit"},
        {"Keypad", "Expanded Edit Mode", "Num 5", S::Timeline, "both sides of the edit nearest the cursor (or of the selected edge) at once; again to leave (Esc too)"},
        {"Keypad", "Time Stretch a Frame / a Pixel", "Ctrl+Num 1, 3, 4, 6", S::Timeline, "the selected edge, else the selected events' ends: the rate changes, grouped events alike"},
        {"Keypad", "Slip Trim / Slip a Frame / a Pixel", "Alt+Num 1, 3, 4, 6", S::Timeline, "the selected edge with its media; else the media slips inside the selected events"},
        {"Keypad", "Slide the Edit (Cross-fade) / Slide Events", "Ctrl+Alt+Num 1, 3, 4, 6", S::Timeline, "the edit at the selected edge; else the selected events slide over their media"},
        {"Keypad", "Trim / Move Ignoring Grouping", "Shift+Num 1, 3, 4, 6", S::Timeline, ""},
        {"Keypad", "Slide the Edit Ignoring Grouping", "Ctrl+Alt+Shift+Num 1, 3, 4, 6", S::Timeline, ""},
        {"Keypad", "Nudge Envelope Points (while held with the mouse)", "Num 8, 2, 4, 6", S::Timeline, ""},
        {"View", "Focus to Track View", "Alt+0", S::Window, ""},
        {"View", "Explorer", "Alt+1", S::Window, "drag media files from it, or double-click one to put it at the cursor"},
        {"View", "Trimmer", "Alt+2", S::Window, "double-click media in Project Media or the Explorer to show it; I / O set in and out, A (or Enter) adds the selection at the cursor, Shift+A so it ends there, Ctrl+H / Ctrl+Shift+H / H the history"},
        {"View", "Master Bus", "Alt+3", S::Window, ""},
        {"View", "Video Preview", "Alt+4", S::Window, "its tool bar: Project Video Properties, External Monitor, Video Output FX, Split-Screen View (drag over the picture for the area; FX Bypassed or Clipboard), Preview Quality, Overlays (safe areas, grid, Red / Green / Blue, as grayscale, Alpha, Closed Captioning CC1-CC4, 360\u00B0 Mode), 360\u00B0 Preview (a 360\u00B0 project: drag to look round, wheel to zoom), snapshots; Split-Screen's Retain Split Screen Setting; the transport's hamburger: the other buttons and Edit Visible Button Set; right-click: Simulate Device Aspect Ratio, Scale Video to Fit, Adjust Size and Quality for Optimal Playback, Display at Project Size, Show Toolbar / Status Bar / Transport Bar"},
        {"View", "Project Media", "Alt+5", S::Window, "media bins (drag media onto one), smart bins, By Type, search, Detailed / List / Thumbnails, Remove All Unused Media, Properties (tags, comment), Replace, Create Subclip from the Trimmer's selection"},
        {"View", "Project Notes", "Alt+6", S::Window, ""},
        {"View", "Transitions", "Alt+7", S::Window, "the picked transition's presets as animated thumbnails; drag one onto an overlap"},
        {"View", "Video FX", "Alt+8", S::Window, "video effects and Filter Packages, with the picked effect's presets as thumbnails; drag one onto an event or track"},
        {"View", "Media Generators", "Alt+9", S::Window, "with each generator's presets as thumbnails; drag one onto the timeline"},
        {"View", "Plug-In Manager", "Ctrl+Alt+1", S::Window, "every plug-in found, in folders (favourites, video FX, transitions, generators, VST / VST3 / DirectX, not working) with the scan result; drag them onto the timeline"},
        {"View", "Video Scopes", "Ctrl+Alt+2", S::Window, "vectorscope, waveform (Composite or Luminance, in IRE), RGB parade, histogram (Luminance, Red, Green, Blue, Alpha, Luminance/R/G/B, Lightness/a/b, with the mean and standard deviation); the value under the pointer; Settings: 7.5 IRE Setup, Studio RGB (16 to 235)"},
        {"View", "Surround Panner", "Ctrl+Alt+3", S::Window, "5.1 projects (Project Properties > Audio > Master bus mode): the current track's or bus's surround panner (pan point, Center, Smoothness, Enable LFE, speaker mutes, keyframes)"},
        {"View", "Edit Details", "Ctrl+Alt+4", S::Window, "a table of the events, markers, regions or media; double-click (or F2) a start, end, length, name, fade or Muted cell to change it in place"},
        {"View", "Mixing Console", "Ctrl+Alt+5", S::Window, ""},
        {"View", "Device Explorer", "Ctrl+Alt+6", S::Window, "removable drives and camera cards (AVCHD, XAVC S, XDCAM, P2, DCIM) and their clips; Import copies them beside the project and adds them to Project Media"},
        {"View", "Loudness Meters", "Ctrl+Alt+7", S::Window, "M, S, I, LRA and true peak meters with over-target indicators; right-click or Options > Loudness Meters: EBU R 128 / ATSC A 85 Mode, EBU +9 / +18, Absolute, Show True Peak Meter, True Peak Blocking Filter, Reset Clip, Reset Metering Engine"},
        {"View", "Dashboard", "Ctrl+Alt+8", S::Window, "timecode, playback frame rate, master peaks, project, recording and disk space tiles (right-click to choose them)"},
        {"View", "Next Window", "F6 or Ctrl+Tab", S::Window, ""},
        {"View", "Previous Window", "Shift+F6 or Ctrl+Shift+Tab", S::Window, ""},
        {"View", "Default Layout", "Alt+D, D", S::Window, ""},
        {"View", "Load Layout 0 ... 9", "Alt+D, 0 ... Alt+D, 9", S::Window, ""},
        {"View", "Save Layout 0 ... 9", "Ctrl+Alt+D, 0 ... Ctrl+Alt+D, 9", S::Window, ""},
        {"View", "Large Preview Layout", "Alt+D, V", S::Window, "the preview over the timeline with the Master Bus beside it (the VEGAS 18 window you sent)"},
        {"View", "Audio Mixing Layout", "Alt+D, A", S::Window, "the Mixing Console in front, wide"},
        {"View", "Color Grading Layout", "Alt+D, C", S::Window, "the preview large with the Video Scopes beside it and the Color Grading panel under it"},
        {"View", "Zoom In / Out (small steps)", "Up or Down", S::Timeline, ""},
        {"View", "Zoom In (to the time selection if any)", "Ctrl+Up", S::Window, ""},
        {"View", "Zoom Out (large steps)", "Ctrl+Down", S::Window, ""},
        {"View", "Increase Track Height", "Ctrl+Shift+Up", S::Window, ""},
        {"View", "Decrease Track Height", "Ctrl+Shift+Down", S::Window, ""},
        {"View", "Minimize / Restore All Tracks", "`", S::Window, "every track to its smallest height, and back (VEGAS's Minimize All Tracks)"},
        {"View", "Resize a Track", "drag the bottom edge of its header", S::Timeline, "all the selected tracks together if it is one of them; the heights are saved in the project"},
        {"View", "Minimize / Maximize / Restore Track Height", "the track's menu", S::Timeline, "the track at its smallest, or as tall as the timeline, and back (the keyboard can be given them)"},
        {"View", "Maximize Timeline", "Ctrl+F11 or Ctrl+Alt+` or Ctrl+Alt+Shift+F11", S::Window, ""},
        {"View", "Show / Hide the Top Docking Area", "Alt+F11", S::Window, "windows docked along the top edge (Ctrl+Alt+F11 is the bottom one, Ctrl+Shift+F11 the right one)"},
        {"View", "Show / Hide the Bottom Docking Area", "Ctrl+Alt+F11", S::Window, ""},
        {"View", "Show / Hide the Right Docking Area", "Ctrl+Shift+F11", S::Window, ""},
        {"View", "Show Waveforms", "Ctrl+Shift+W", S::Window, ""},
        {"View", "Show Event Buttons", "Ctrl+Shift+C", S::Window, "Pan/Crop, FX and the menu on each event"},
        {"View", "Show Event Headers", "Ctrl+Alt+M", S::Window, "events' names on tracks of any height"},
        {"View", "Set Track Heights to Default", "Ctrl+`", S::Window, ""},
        {"View", "Minimize / Restore the Window Docking Area", "F11 or Alt+`", S::Window, ""},
        {"View", "Zoom In Until Each Thumbnail Is a Frame", "Alt+Up", S::Window, ""},
        {"View", "Previous / Next Track", "Alt+Shift+Up or Alt+Shift+Down", S::Window, "the selected track"},
        {"View", "Shortcut Menu", "Shift+F10", S::Timeline, "of the selected event, else of the selected track"},
        {"View", "Show Envelopes", "Ctrl+Shift+E", S::Window, ""},
        {"View", "Show Event Fade Lengths", "Ctrl+Shift+T", S::Window, "on the selected events"},
        {"View", "Event Media Markers", "Ctrl+Shift+K", S::Window, "the markers (chapters) kept in the media files, on their events"},
        {"View", "Active Take Information", "Ctrl+Shift+I", S::Window, "the take's number and name (and a changed rate) on each event"},
        {"View", "Video Event Edge Edit Frames", "Ctrl+Shift+O", S::Window, "while an edge is trimmed or an edit slid, the preview shows the frame at the edge (one frame, not VEGAS's split screen)"},
        {"View", "Minimize / Restore the Track List", "Shift+F11 or Shift+Alt+`", S::Window, ""},
        {"View", "Zoom Waveforms Vertically", "Shift+Up or Shift+Down", S::Timeline, ""},
        {"View", "Focus Between Track List and Timeline", "Tab", S::Timeline, "the track list: Ctrl+Left / Right volume, Ctrl+Shift+Left / Right pan of the selected tracks"},
        {"View", "Color Grading", "Alt+G", S::Window, "wheels, sliders and curves for the selected event, track or the video output (a built-in effect: no plug-in needed)"},
        {"View", "Motion Tracking", "Alt+M", S::Window, "draw a box round something in the selected video event, track it forward or back, and apply it as Track Motion or the event's Pan/Crop (follow or stabilize)"},
        {"View", "Text to Speech", "Ctrl+Shift+P", S::Window, "Windows' own voices speak the text into a WAV put on the timeline at the cursor (Windows only)"},
        {"View", "Show Bus Tracks", "B or Ctrl+Shift+B", S::Window, "the video output, master and bus tracks, with their envelopes"},
        {"Insert", "Audio Track", "Ctrl+Q", S::Window, ""},
        {"Insert", "Video Track", "Ctrl+Shift+Q", S::Window, ""},
        {"Insert", "Marker", "M", S::Window, ""},
        {"Insert", "Region", "R", S::Window, "over the time selection"},
        {"Insert", "Command Marker", "C", S::Window, "a command (URL, TEXT, Caption, 608CC1-608CC4 closed captions...) and its parameter at the cursor; plain caption text becomes a pop-on caption ([BR]: new line)"},
        {"File", "Import > Closed Captioning...", "", S::Window, "Scenarist (.scc), SubRip (.srt), SAMI (.smi), RealText (.rt), QuickTime / transcript (.txt), MacCaption (.mcc) and DVD Architect (.sub) captions as 608CC command markers"},
        {"File", "Export > XDCAM Disc...", "", S::NotAvailable, "needs an XDCAM drive or deck"},
        {"Edit", "Edit in VEGAS Effects (event right-click menu)", "", S::NotAvailable, "needs the separate VEGAS Effects application; Media FX... and Video Event FX... apply the same OpenFX effects in Reno"},
        {"Tools", "Burn Disc (Track-at-Once / Disc-at-Once Audio CD, DVD, Blu-ray Disc), Print Video to Tape, Print Video to HDV Tape", "", S::NotAvailable, "Reno does not drive disc burners or tape decks; render the file and burn it with another program"},
        {"Tools", "Relink Wireless Adapter Media... / Lay Out Audio CD from Events / Generate Loudness Log", "", S::Window, "relinks proxy clips (C0001S03.MP4, *_proxy) to full-resolution files in a folder; CD track regions over audio events; writes the selection's loudness to <project>_loud.txt"},
        {"Tools", "Scripting: Promote Media Closed Captioning / Export Closed Captioning for DVD Architect, QuickTime, Windows Media Player, YouTube", "", S::Window, "the selected events' media's .scc captions as command markers; the project's captions to .sub, .txt + .smil, .smi + .asx, .srt"},
        {"Edit Details", "Commands / Audio CD Track List; Ctrl+K (insert the selected caption at the cursor); Ctrl+C / Ctrl+V", "Ctrl+K", S::Panel, "Ctrl+K moves the selected command to the playback position and selects the next (caption timing); pasted tab-separated lines become command markers"},
        {"Options", "Mute All Audio / Mute All Video", "", S::Window, "playback leaves every audio / video track out (renders keep them)"},
        {"Options", "External Control", "", S::NotAvailable, "control surfaces and Vegas Pro Connect are not supported"},
        {"Insert", "Audio CD Track Region", "N", S::Window, "over the time selection (or from the cursor to the next CD track)"},
        {"Insert", "Audio CD Track Index", "Shift+N", S::Window, "at the cursor"},
        {"Insert", "Insert / Show / Hide Track Volume Envelope", "V", S::Window, "a video track's composite level"},
        {"Insert", "Remove Track Volume Envelope", "Shift+V", S::Window, ""},
        {"Insert", "Insert / Show / Hide Track Pan Envelope", "P", S::Window, ""},
        {"Insert", "Remove Track Pan Envelope", "Shift+P", S::Window, ""},
        {"Insert", "Next / Previous Envelope of the Track", "E or Shift+E", S::Window, "brings it to the front (VEGAS cycles through effect automation envelopes)"},
        {"Options", "Quantize to Frames", "Alt+F8", S::Window, ""},
        {"Options", "Enable Snapping", "F8", S::Window, ""},
        {"Options", "Snap to Grid", "Ctrl+F8", S::Window, ""},
        {"Options", "Snap to Markers", "Shift+F8", S::Window, ""},
        {"Options", "Snap to All Events", "Ctrl+Shift+F8", S::Window, "off: edges snap only to events on the same tracks"},
        {"Options", "Auto Ripple", "Ctrl+L", S::Window, "deletes, cuts, moves and end trims"},
        {"Options", "Automatic Crossfades", "Ctrl+Shift+X", S::Window, "off: where events overlap the later one covers the earlier (and both sounds play)"},
        {"Options", "Ignore Event Grouping", "Ctrl+Shift+U", S::Window, ""},
        {"Options", "Quickfade Audio Edits", "", S::Window, "on (as in VEGAS): audio event edges with no fade get a short fade (Preferences > Editing: 10 ms) so cuts do not click"},
        {"Options", "Loop Playback", "Q or Ctrl+Shift+L", S::Window, ""},
        {"Options", "Customize Keyboard...", "", S::Window, "change, add or remove any menu command's keys (conflicts shown, Locate); keyboard maps can be saved, chosen, deleted, imported and exported. Timeline and docked window keys are listed but fixed"},
        {"Options", "Export Preferences... / Import Preferences...", "", S::Window, "all Reno settings (preferences, keyboard map, track defaults, window options) to an .ini file and back"},
        {"Tracks", "Mute Selected Track", "Z", S::Window, ""},
        {"Tracks", "Mute Selected Track Only", "Shift+Z", S::Window, ""},
        {"Tracks", "Solo Selected Track", "X", S::Window, ""},
        {"Tracks", "Solo Selected Track Only", "Shift+X", S::Window, ""},
        {"Tracks", "Mute Track Group", "Ctrl+Alt+Z", S::Window, "the current track's group"},
        {"Tracks", "Mute Track Group Only", "Ctrl+Alt+Shift+Z", S::Window, "and the other tracks out of mute"},
        {"Tracks", "Solo Track Group", "Ctrl+Alt+X", S::Window, ""},
        {"Tracks", "Solo Track Group Only", "Ctrl+Alt+Shift+X", S::Window, ""},
        {"Tracks", "Track Group: Group / Ungroup Selected Tracks, Collapse, Expand", "", S::Window, "in a track's menu (select several tracks with Ctrl+click on their headers)"},
        {"Tracks", "Track Volume (or Level) from the Keyboard", "Ctrl+Left or Ctrl+Right", S::Window, "while the track list has the keyboard (Tab): half a decibel (video: 1%) a press"},
        {"Tracks", "Track Pan from the Keyboard", "Ctrl+Shift+Left or Ctrl+Shift+Right", S::Window, "while the track list has the keyboard (Tab)"},
        {"Tracks", "Insert Adjustment Track", "Ctrl+Shift+Alt+Q", S::Window, "a video track whose effects apply to every track under it; its Track FX open"},
        {"Playback", "Play / Stop", "Space", S::Window, "stop goes back to where playing began"},
        {"Playback", "Stop", "Esc", S::Window, "(also leaves edge-trimming mode)"},
        {"Playback", "Play / Pause", "Enter or Ctrl+F12", S::Window, ""},
        {"Playback", "Play From Start", "Shift+Space or Shift+F12", S::Window, ""},
        {"Playback", "Play from Any Window", "Ctrl+Space or F12", S::Window, ""},
        {"Playback", "Shuttle Back", "J", S::Window, "plays backwards; pressed again, faster (1, 1.5, 2, 4, 10, 20 times; quiet above 4)"},
        {"Playback", "Slow Motion", "K", S::Window, "plays at a quarter speed; then L / J speed up / slow down a quarter at a time (through 0 to backwards). Space stops"},
        {"Playback", "Shuttle Forward", "L", S::Window, "plays; pressed again, faster (1, 1.5, 2, 4, 10, 20 times; quiet above 4)"},
        {"Playback", "Record", "Ctrl+R", S::Window, "records into the armed audio tracks (else the selected one or a new one) from the cursor; again (or Space) stops"},
        {"Playback", "Arm Track for Record", "Ctrl+Alt+R", S::Window, "the selected audio track; its input level shows in the status bar"},
        {"Playback", "Arm for Record and Set Recording Folder", "Ctrl+Alt+Shift+R", S::Window, "Reno records every track into the one folder (Preferences > Audio Device)"},
        {"Playback", "Preview Cursor Position", "Num+0", S::Window, "plays a second centred on the cursor, then puts the cursor back"},
        {"Playback", "Dynamic RAM Preview", "Shift+B", S::Window, "renders the time selection's frames into memory (up to Preferences > Video > Dynamic RAM Preview max) and plays them looped"},
        {"Playback", "Selective Prerender", "Shift+M", S::Window, "renders the time selection to a file that plays instead until something there changes (Tools > Clean Up Prerendered Video deletes them)"},
        {"Playback", "Enable Multicamera Editing", "Ctrl+Shift+D", S::Window, "Tools > Multicamera > Create Multicamera Track makes the track: each camera is a take"},
        {"Playback", "Multicamera: Choose Camera", "1 ... 9", S::Window, "stopped: the event at the cursor shows that camera; playing: cuts there (the preview shows one camera, not VEGAS's grid)"},
        {"Playback", "Multicamera: Cross-fade to Camera", "Ctrl+1 ... Ctrl+9", S::Window, "a cut with a cross-fade (as long as Preferences > Editing say)"},
        {"Playback", "Dim Master Bus Output", "Ctrl+Shift+F12", S::Window, "turns what is heard down by 20 dB, and back"},
        {"Playback", "External Monitor Preview", "Alt+Shift+4", S::Window, "the preview full screen on another display (View > External Monitor Display chooses it); again, or Esc, closes it"},
        {"Playback", "Generate MIDI Timecode", "F7", S::Window, "sends MTC quarter frames (and full frames when playing starts or jumps) to the MIDI output chosen in Options > Timecode (Windows)"},
        {"Playback", "Generate MIDI Clock", "Shift+F7", S::Window, "24 clocks a beat at the project's tempo, with Start / Continue, Song Position and Stop"},
        {"Playback", "Trigger from MIDI Timecode", "Ctrl+F7", S::Window, "plays from where MIDI timecode coming in says, and stops when it stops"},
        {"Playback", "Playback Rate", "", S::Window, "the Rate slider under the track headers shuttles while dragged (-2 .. 2); double-click its label to set the Play rate, the slider to reset it to 1"},
        {"Mouse", "Event / Track FX Button", "click", S::Timeline, "with no effects yet: opens the Plug-In Chooser, then the effects window; the Plug-In Chain button opens the chooser again"},
        {"Mouse", "Select a Range of Events", "Shift+click", S::Timeline, "from the event clicked last"},
        {"Mouse", "Add or Take Away an Event", "Ctrl+click", S::Timeline, ""},
        {"Mouse", "Time Select over Events", "Ctrl+Shift+drag", S::Timeline, ""},
        {"Mouse", "Drag Without Snapping", "Shift+drag", S::Timeline, ""},
        {"Mouse", "Slip the Media Inside an Event", "Alt+drag an event", S::Timeline, ""},
        {"Mouse", "Slip Trim (the media moves with the edge)", "Alt+drag an edge", S::Timeline, ""},
        {"Mouse", "Slip Trim the End", "Alt+Shift+drag an event", S::Timeline, ""},
        {"Mouse", "Trim Ignoring Grouping", "Shift+drag an edge", S::Timeline, ""},
        {"Mouse", "Slide the Edit (both events' edges)", "Ctrl+Alt+drag an edge", S::Timeline, "with Shift: ignoring grouping"},
        {"Mouse", "Slide an Event Over Its Media", "Ctrl+Alt+drag an event", S::Timeline, ""},
        {"Mouse", "Move a Cross-fade", "Ctrl+Alt+drag a cross-fade", S::Timeline, ""},
        {"Mouse", "Right-drag an event: Move Here / Copy Here / Shuffle Events", "right-drag", S::Timeline, "the event moves as you drag; on release a menu asks what to do (a right click without a drag opens the event menu)"},
        {"Mouse", "Split / Split Trim", "Ctrl+Alt+Shift+click or drag", S::Timeline, "split where clicked; dragged, the part that way is trimmed"},
        {"Mouse", "Time Stretch", "Ctrl+drag an edge", S::Timeline, "grouped picture and sound get the same rate"},
        {"Mouse", "Envelope Points: Fine / Normal / Time Only", "Ctrl+drag, Ctrl+Alt+drag, Alt+drag", S::Timeline, ""},
        {"Mouse", "Fine Slider Changes", "Ctrl+drag a slider", S::Timeline, ""},
        {"Mouse", "Move a Region Keeping Its Length", "Alt+drag its tag", S::Timeline, ""},
        {"Mouse", "Add Media as Takes", "Ctrl+drop on an event", S::Timeline, ""},
        {"Mouse", "Select the Time Between Two Markers", "Double-click the time selection bar between them", S::Timeline, "region edges count as markers"},
        {"Mouse", "Change Keyframe Spacing", "Alt+drag keyframes", S::Panel, "in the Pan/Crop and Track Motion keyframe strips: keyframes between the dragged one and the first (the last, for the first) spread out or close up in proportion; those beyond it move with it"},
        {"Mouse Wheel", "Zoom In / Out", "Wheel", S::Timeline, ""},
        {"Mouse Wheel", "Scroll Up and Down", "Ctrl+wheel", S::Timeline, ""},
        {"Mouse Wheel", "Scroll Sideways", "Shift+wheel, or the wheel tilted sideways", S::Timeline, "(two fingers across a touch pad too)"},
        {"Mouse Wheel", "Cursor in Small Steps", "Ctrl+Shift+wheel", S::Timeline, "in edge-trimming mode, trims a pixel"},
        {"Mouse Wheel", "Cursor by Frames", "Ctrl+Alt+Shift+wheel", S::Timeline, "in edge-trimming mode, trims a frame"},
        {"Mouse Wheel", "Move a Fader or Slider", "Wheel over it (Ctrl: finely)", S::Timeline, "the track headers' volume and pan"},
        {"Mouse Wheel", "Auto-scroll", "Wheel button held and the mouse moved", S::Timeline, "the further from where it was pressed, the faster"},
        {"Multimedia Keyboard", "Play / Pause", "Toggle Media Play/Pause or Media Play", S::Window, ""},
        {"Multimedia Keyboard", "Play From Start", "Shift+Toggle Media Play/Pause or Shift+Media Play", S::Window, ""},
        {"Multimedia Keyboard", "Stop", "Media Stop", S::Window, ""},
        {"Multimedia Keyboard", "Mute Track / Mute Track Only", "Volume Mute or Shift+Volume Mute", S::Window, ""},
        {"Multimedia Keyboard", "Solo Track / Solo Track Only", "Ctrl+Volume Mute or Ctrl+Shift+Volume Mute", S::Window, ""},
        {"Multimedia Keyboard", "Track Volume (or Level)", "Ctrl+Volume Up or Ctrl+Volume Down", S::Window, ""},
        {"Multimedia Keyboard", "Track Pan", "Shift+Volume Up or Shift+Volume Down", S::Window, ""},
        {"Multimedia Keyboard", "Next / Previous Track", "Media Next or Media Previous", S::Window, ""},
        {"Trimmer", "Play / Stop", "Space", S::Panel, ""},
        {"Trimmer", "Set In / Set Out", "I or O", S::Panel, "the selection of the media to add"},
        {"Trimmer", "Previous / Next Frame", "Left or Right", S::Panel, ""},
        {"Trimmer", "Go to Start / End of the Media", "Home or End", S::Panel, ""},
        {"Trimmer", "Add to Timeline (at the Cursor)", "A or Enter", S::Panel, "the selection (else the whole media) starts at the cursor"},
        {"Trimmer", "Add to Timeline (Ending at the Cursor)", "Shift+A", S::Panel, ""},
        {"Trimmer", "History: Sort / Remove Current / Clear", "H, Ctrl+H, Ctrl+Shift+H", S::Panel, ""},
        {"Trimmer", "Select the Timeline's Time Selection Length After / Before the Cursor", "T or Shift+T", S::Panel, "kept inside the file"},
        {"Trimmer", "Select Stream: Video and Audio, Video Only, Audio Only", "Tab", S::Panel, "what Add (A, Shift+A) and a drag put on the timeline"},
        {"Trimmer", "Video Larger / Audio Larger", "Ctrl+Shift+Up or Ctrl+Shift+Down", S::Panel, "the picture or the waveform"},
        {"Trimmer", "Open in Audio Editor", "Ctrl+E", S::Panel, "the Trimmer's file, in the editor chosen in Preferences > Editing"},
        {"Trimmer", "Insert Marker / Insert Region", "M or R", S::Panel, "at the Trimmer's cursor; a region over its selection"},
        {"Trimmer", "Save Markers/Regions", "S", S::Panel, "the Trimmer's markers and regions are kept with the media in the project and shown on its events (View > Event Media Markers)"},
        {"Trimmer", "Auto-Save Trimmer Markers/Regions", "Shift+S", S::Panel, "on: each marker or region is saved as it is added"},
        {"Trimmer", "Show Video in Preview Window", "Ctrl+P", S::Panel, "on: the Trimmer's picture (and what it plays) shows in the Video Preview too"},
        {"Event Pan/Crop", "Mask: draw a rectangle / oval (Shift: square / circle; Ctrl: from the centre)", "Rectangle or Oval tool, drag", S::Panel, "the Mask row's tools; a shape drawn turns the Mask switch on"},
        {"Event Pan/Crop", "Mask: add Bezier points / close the path", "Anchor tool: click (drag: curve handles); click the first point, double-click, Enter or Esc", S::Panel, ""},
        {"Event Pan/Crop", "Mask: move a shape, a point or a handle", "Normal tool: drag (Alt: one handle alone)", S::Panel, "the Split Tangent tool pulls handles out of a point; the Anchor Deletion tool removes points"},
        {"Event Pan/Crop", "Mask: delete the chosen shape (or point)", "Delete", S::Panel, "right-click a shape: Delete, Duplicate, Flip, Positive / Negative, Enabled, Reset Mask"},
        {"Track Motion", "Lock Aspect Ratio", "A", S::Panel, "also in Event Pan/Crop (as all of these)"},
        {"Track Motion", "Scale About Center", "C", S::Panel, ""},
        {"Track Motion", "Edit in Object Space", "O", S::Panel, "moving keeps to the box's own turned axes"},
        {"Track Motion", "Prevent Movement X / Y", "X or Y", S::Panel, ""},
        {"Track Motion", "Prevent Scaling X / Y", "Shift+X or Shift+Y", S::Panel, ""},
        {"Track Motion", "Enable Snapping", "F8", S::Panel, "to a grid of sixteenths of the frame"},
        {"Track Motion", "Enable Rotation", "Shift+F8", S::Panel, ""},
        {"Track Motion", "Previous / Next Keyframe", "Ctrl+Left or Ctrl+Right", S::Panel, "also in Event Pan/Crop"},
        {"Track Motion", "Copy / Paste Keyframes", "Ctrl+C or Ctrl+V", S::Panel, "the selected keyframes (Ctrl or Shift+click selects several) are pasted at the cursor; also in the keyframe right-click menu"},
        {"Track Motion", "Delete the Selected Keyframes", "Delete", S::Panel, "with the keyframe strip clicked"},
        {"Track Motion", "Copy Keyframes by Dragging", "Ctrl+drag keyframes", S::Panel, "the selected keyframes are copied to where they are dropped"},
        {"Event Pan/Crop", "Mask: Rounded Rectangle / Half Oval", "Alt+drag", S::Panel, "with the Rectangle or the Oval Mask Creation Tool"},
        {"Event Pan/Crop", "Mask: Select / Unselect a Shape", "Alt+Shift+click", S::Panel, "a selected shape has handles to scale it (Shift: keep proportions, Ctrl: about the centre) and to rotate it (Shift: 15 degree steps)"},
        {"Event Pan/Crop", "Mask: Duplicate a Shape", "Ctrl+drag", S::Panel, "also Duplicate in the shape's right-click menu"},
        {"Track Motion", "3D Layout: Single Pane, Split, Four, Large Pane and Two", "1 ... 6", S::Panel, "with the track in 3D Source Alpha: the 3D workspace's panes (Camera, Front, Back, Left, Right, Top, Bottom, Perspective)"},
        {"Track Motion", "Prevent Movement Z / Prevent Scaling Z", "Z or Shift+Z", S::Panel, "in 3D Source Alpha"},
        {"Surround Panner", "Move the pan point forward or back / left or right", "Up/Down or Page Up/Page Down or wheel; Left/Right or Shift+Page Up/Page Down or Shift+wheel", S::Panel, "with the Surround Panner window's room focused; Ctrl: finer"},
        {"Surround Panner", "Move the pan point to a corner, edge or the centre", "Numeric Keypad 1 ... 9 (Ctrl+1, 3, 7, 9: the corners of the largest circle)", S::Panel, "Num Lock on"},
        {"Surround Panner", "Drag along a line through the centre / round a circle / round the largest circle", "Shift+drag, Alt+drag or Alt+wheel, Alt+Shift+drag or Alt+Shift+wheel", S::Panel, "with Move Freely"},
        {"Mixing Console", "Rename the Selected Channel", "F2", S::Panel, "click a channel's name to select it (Ctrl+click: several); a double-click on the name renames it too"},
        {"Mixing Console", "Insert Audio Track", "Ctrl+Q", S::Panel, ""},
        {"Mixing Console", "Channel Width: Default, Narrow, Wide", "D or N or W", S::Panel, "also the View Controls' Channel Width slider"},
        {"Mixing Console", "Hide the Selected Channels", "Shift+H", S::Panel, "ticking them in the Channel List shows them again"},
        {"Mixing Console", "Show / Hide the Channel List", "Shift+C", S::Panel, "a tick per channel: shown or hidden"},
        {"Mixing Console", "Show / Hide the View Controls", "Shift+R", S::Panel, ""},
        {"Mixing Console", "Show All Channels", "Q", S::Panel, ""},
        {"Mixing Console", "Show / Hide Bus, Audio Track, Master Channels", "U or A or T", S::Panel, ""},
        {"Mixing Console", "Show / Hide Assignable FX Channels", "E", S::Panel, "the assignable effects chains' strips (Insert > Audio Assignable FX)"},
        {"Video FX", "AI Speech to Text", "", S::NotAvailable, "needs VEGAS's cloud service; Insert > Subtitles from a text or CSV file, or from regions, makes the same subtitle events"},
        {"Video FX", "AI Colorization", "", S::Window, "Reno's Colorization guesses sky, foliage and warm tones (no AI models, so the result differs); Color Grading and Gradient Map colour by hand"},
        {"Video FX", "AI Smart Mask", "", S::NotAvailable, "needs VEGAS's AI models; Bezier Masking and Pan/Crop masks cut a subject out by hand"},
        {"Video FX", "AI Style Transfer", "", S::Window, "Reno's Style Transfer is a painterly approximation named after VEGAS's 17 styles, not VEGAS's neural models"},
        {"Video FX", "AI Upscale", "", S::NotAvailable, "needs VEGAS's AI models; Pan/Crop with Optical Flow resampling and the Unsharp Mask effect are the local stand-ins"},
        {"Video FX", "AI Z-Depth", "", S::NotAvailable, "needs VEGAS's AI models"},
        {"Video FX", "AI Dehaze / Sharpen / Smoothen", "", S::NotAvailable, "need VEGAS's AI models; the Unsharp Mask, Soft Contrast and Median effects do similar work without AI"},
        {"Video FX", "AI Auto Reframe", "", S::NotAvailable, "needs VEGAS's AI models; Pan/Crop with keyframes and Motion Tracking reframe by hand"},
        {"Insert", "Input Bus", "", S::Window, "adds an input bus to the Mixing Console: sound from a sound card input, with effects, a fader and an output; tracks record from it (Record Input)"},
        {"Mixing Console", "Show / Hide Preview Bus Channel", "P", S::Panel, "the Preview bus: a fader and meters for what the Trimmer and Start Preview play"},
        {"Mixing Console", "Show / Hide Control Region Labels", "Shift+L", S::Panel, ""},
        {"Mixing Console", "Show / Hide Fader Ticks", "Shift+T", S::Panel, "the dB scale beside the faders"},
        {"Mixing Console", "Show / Hide Faders, Insert FX, I/O, Meters, Sends", "F or I or H or M or S", S::Panel, "the control regions (the faders region has pan, mute, solo and arm for record)"},
        {"Mixing Console", "Selected Channels' Faders Up / Down", "Up or Down", S::Panel, "half a decibel a press, one undo step each (the master's left and right faders move together unless split with its L/R button)"},
        {"Effects window", "Animate: the keyframe strip under an OpenFX effect's controls", "", S::Panel, "click: move the cursor; drag a keyframe to move it; double-click a row to add one; right-click one: Linear / Fast / Slow / Smooth / Sharp / Hold, Delete; the buttons go to the first / previous / next / last keyframe and create or delete one"},
        {"Effects window", "Delete the picked keyframe", "Delete", S::Panel, "in the keyframe strip"},
        {"Effects window", "Preset list of an audio effect; Load / Save .fxp, .fxb", "", S::Panel, "the plug-in's programs and your presets (Save Preset / Delete Preset); VST2: the ... button"},
        {"Plug-In Chooser", "Create New Folder / Rename / Delete; Add to Folder; Rename Package", "", S::Panel, "right-click the folders, or a plug-in or package in the list (drag one onto your folder to put it there)"},
        {"Plug-In Chooser", "Undo / Redo a change to the chain", "Ctrl+Z / Ctrl+Y", S::Panel, "also the arrows at the end of the chain bar"},
        {"Plug-In Chooser", "Up One Level; Delete (your folder or package, or a plug-in out of your folder)", "Backspace; Delete", S::Panel, "a double-click on a folder in the list opens it, on a plug-in adds it to the chain"},
        {"Red Eye Reduction", "Scroll left / right, up / down (Shift: further)", "arrows", S::Panel, "Tools > Video > Red Eye Reduction or a still's event menu: drag from the middle of an eye to box it, drag a box to move it or its edges to size it, right-click to delete; wheel or + / - zoom"},
        {"Red Eye Reduction", "Jump to the top / bottom of the picture", "Page Up/Down or Shift+Home/End", S::Panel, ""},
        {"Red Eye Reduction", "Jump to the left / right edge of the picture", "Home/End or Shift+Page Up/Down", S::Panel, ""},
        {"Red Eye Reduction", "Jump to the middle across / up and down", "\\ or Num *, Shift+\\ or Shift+Num *", S::Panel, ""},
        {"Red Eye Reduction", "Jump to the middle of the picture", "C", S::Panel, ""},
        {"Help", "Contents and Index", "F1", S::Window, "the read-me and this list"},
        {"Help", "What's This?", "Shift+F1", S::Window, "then click a command or a window"},
    };
    return list;
}

// ================================================================================================ small helpers
namespace {

QAction* make(QObject* parent, const char* name, const QString& text, const QList<QKeySequence>& keys, std::function<void()> fn)
{
    QAction* a = new QAction(text, parent);
    a->setObjectName(name);
    if (!keys.isEmpty()) a->setShortcuts(keys);
    QObject::connect(a, &QAction::triggered, parent, [fn] { fn(); });
    return a;
}

QList<QKeySequence> keys(std::initializer_list<const char*> list)
{
    QList<QKeySequence> out;
    for (const char* k : list) out << QKeySequence(QString::fromUtf8(k));
    return out;
}

// Keys without Ctrl or Alt belong to a text box while one is being typed in.
class TypingGuard : public QObject {
public:
    explicit TypingGuard(QObject* parent) : QObject(parent) {}

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() != QEvent::ShortcutOverride) return false;
        const QKeyEvent* k = static_cast<QKeyEvent*>(e);
        // keypad keys belong to the timeline while it has focus
        if ((k->modifiers() & Qt::KeypadModifier) && dynamic_cast<TimelineWidget*>(o)) {
            if (k->key() != Qt::Key_0) e->accept();
            return false;
        }
        if (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier)) return false;
        const bool typing = qobject_cast<QLineEdit*>(o) || qobject_cast<QTextEdit*>(o) || qobject_cast<QPlainTextEdit*>(o) || qobject_cast<QAbstractSpinBox*>(o);
        if (typing && !(k->key() >= Qt::Key_F1 && k->key() <= Qt::Key_F35)) e->accept();
        return false;
    }
};

QToolButton* flatButton(QWidget* parent, const char* name, Ico ico, const QString& tip)
{
    QToolButton* b = new QToolButton(parent);
    b->setObjectName(name);
    b->setIcon(toolbarIcon(ico));
    b->setIconSize(QSize(16, 16));
    b->setAutoRaise(true);
    b->setToolTip(tip);
    b->setAccessibleName(tip);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

// A widget filled with one colour.
QWidget* filled(QWidget* parent, const char* name, const QColor& c)
{
    QWidget* w = new QWidget(parent);
    w->setObjectName(name);
    w->setAutoFillBackground(true);
    QPalette pal = w->palette();
    pal.setColor(QPalette::Window, c);
    w->setPalette(pal);
    return w;
}

QWidget* line(QWidget* parent, const QColor& c, bool horizontal = true)
{
    QWidget* w = filled(parent, "", c);
    if (horizontal) w->setFixedHeight(1);
    else w->setFixedWidth(1);
    return w;
}

// A docked window with VEGAS's tab (name, float and close buttons) under it. Shown while the window is alone in its place; otherwise the main window's tab row is used.
class DockPage : public QWidget {
public:
    DockPage(QDockWidget* dock, QWidget* content, QWidget* parent) : QWidget(parent)
    {
        setObjectName(dock->objectName() + "Page");
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(content, 1);
        strip_ = filled(this, "dockTabStrip", kGap);
        strip_->setFixedHeight(19);
        auto* sh = new QHBoxLayout(strip_);
        sh->setContentsMargins(4, 0, 0, 2);
        sh->setSpacing(0);
        QWidget* tab = filled(strip_, "dockTab", kTool);
        auto* th = new QHBoxLayout(tab);
        th->setContentsMargins(7, 0, 3, 0);
        th->setSpacing(1);
        QLabel* title = new QLabel(dock->windowTitle(), tab);
        title->setObjectName("dockTabTitle");
        th->addWidget(title);
        th->addSpacing(6);
        QToolButton* fl = flatButton(tab, "dockFloat", Ico::Float, "Float this window");
        QToolButton* cl = flatButton(tab, "dockClose", Ico::Close, "Close this window");
        for (QToolButton* b : {fl, cl}) {
            b->setIconSize(QSize(14, 14));
            b->setFixedSize(18, 17);
            th->addWidget(b);
        }
        sh->addWidget(tab, 0, Qt::AlignTop);
        sh->addStretch(1);
        v->addWidget(strip_);
        QObject::connect(fl, &QToolButton::clicked, dock, [dock] { dock->setFloating(!dock->isFloating()); });
        QObject::connect(cl, &QToolButton::clicked, dock, [dock] { dock->close(); });
    }
    void setTabShown(bool on) { strip_->setVisible(on); }

private:
    QWidget* strip_ = nullptr;
};

QWidget* masterPage(QWidget* parent, QWidget* masterBus, std::function<void()> showMixer, Playback* pb = nullptr)
{
    QWidget* page = filled(parent, "masterBusPage", kDark);
    auto* v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(line(page, kRim));
    QWidget* bar = filled(page, "masterBusToolBar", kTool);
    bar->setFixedHeight(29);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(4, 0, 4, 0);
    h->setSpacing(2);
    QToolButton* props = flatButton(bar, "masterProperties", Ico::Settings, "Audio Properties: the project's audio settings");
    QObject::connect(props, &QToolButton::clicked, page, [parent] { runProjectPropertiesAudio(parent); });
    h->addWidget(props);
    QToolButton* downmix = flatButton(bar, "masterDownmix", Ico::Downmix, "Downmix Output: hear the mix in stereo (a 5.1 project) or in mono");
    QMenu* downmixMenu = new QMenu(downmix);
    downmix->setMenu(downmixMenu);
    downmix->setPopupMode(QToolButton::InstantPopup);
    QObject::connect(downmixMenu, &QMenu::aboutToShow, page, [downmixMenu, pb] { fillDownmixMenu(downmixMenu, pb); });
    h->addWidget(downmix);
    QToolButton* dim = flatButton(bar, "masterDim", Ico::Dim, "Dim Output (Ctrl+Shift+F12): what is heard 20 dB quieter");
    QObject::connect(dim, &QToolButton::clicked, page, [parent] {
        if (QAction* a = parent->findChild<QAction*>("dimOutput")) a->trigger();
    });
    h->addWidget(dim);
    QToolButton* mixer = flatButton(bar, "showMixingConsole", Ico::Mixer, "Mixing Console (Ctrl+Alt+5)");
    QObject::connect(mixer, &QToolButton::clicked, page, [showMixer] { showMixer(); });
    h->addWidget(mixer);
    h->addStretch(1);
    v->addWidget(bar);
    v->addWidget(masterBus, 1);
    return page;
}

// Rate slider (VEGAS's scrub control) with a mark for the Play rate.
class RateSlider : public QSlider {
public:
    RateSlider(QWidget* parent) : QSlider(Qt::Horizontal, parent) { setAttribute(Qt::WA_Hover); }
    std::function<void()> onDoubleClick;
    double normalRate = 1.0;

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (onDoubleClick) onDoubleClick();
        e->accept();
    }
    void paintEvent(QPaintEvent*) override
    {
        {
            QPainter p(this);
            const int cy = height() / 2 - 1;
            p.fillRect(QRect(0, cy - 1, width(), 3), QColor(38, 38, 38));
            p.fillRect(QRect(0, cy - 1, width(), 1), QColor(130, 130, 130));
            const int x = 6 + QStyle::sliderPositionFromValue(minimum(), maximum(), value(), std::max(1, width() - 12));
            const QRectF k(x - 6.0, cy - 5.0, 12.0, 11.0);
            p.fillRect(k, QColor(24, 24, 24));
            p.fillRect(k.adjusted(1, 1, -1, -1), underMouse() || isSliderDown() ? QColor(255, 255, 255) : QColor(240, 240, 240));
            p.fillRect(QRectF(k.left() + 3.0, k.top() + 3.0, 2.0, 5.0), QColor(24, 24, 24));
            p.fillRect(QRectF(k.right() - 5.0, k.top() + 3.0, 2.0, 5.0), QColor(24, 24, 24));
            if (std::fabs(normalRate - 1.0) < 1e-9) return;
        }
        const int v = (int)std::lround(std::max(-2.0, std::min(2.0, normalRate)) * 100.0);
        const int x = 5 + QStyle::sliderPositionFromValue(minimum(), maximum(), v, std::max(1, width() - 11));
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xf0, 0x8c, 0x1e));
        const QPointF tri[3] = {QPointF(x, height() - 5), QPointF(x - 3, height()), QPointF(x + 3, height())};
        p.drawPolygon(tri, 3);
    }
};

class RateLabel : public QLabel {
public:
    RateLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {}
    std::function<void()> onDoubleClick;

protected:
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        if (onDoubleClick) onDoubleClick();
        e->accept();
    }
};

// "Rate: 0.00" scrub control: dragging plays at -2..2 (backwards to the left); releasing springs back to 0 and stops.
QWidget* ratePanel(QWidget* parent)
{
    QWidget* panel = filled(parent, "ratePanel", kBar);
    auto* h = new QHBoxLayout(panel);
    h->setContentsMargins(6, 0, 8, 2);
    h->setSpacing(4);
    QLabel* label = new RateLabel("Rate: 0.00", panel);
    label->setObjectName("rateLabel");
    QSlider* rate = new RateSlider(panel);
    rate->setObjectName("rateSlider");
    rate->setRange(-200, 200);
    rate->setValue(0);
    rate->setFixedWidth(56);
    rate->setFocusPolicy(Qt::NoFocus);
    rate->setStyleSheet("QSlider::groove:horizontal { height: 2px; background: #232323; border-top: 1px solid #828282; }"
                        "QSlider::handle:horizontal { width: 11px; height: 10px; margin: -5px 0px; background: #e6e6e6; border: 1px solid #181818; }");
    const QString tip = "Playback rate: drag to play faster, slower or backwards (springs back when let go). "
                        "Double-click the label to set the rate Play plays at (the orange mark), the slider to set it back to 1.";
    for (QWidget* w : {(QWidget*)panel, (QWidget*)label, (QWidget*)rate}) w->setToolTip(tip);
    h->addWidget(label);
    h->addStretch(1);
    h->addWidget(rate);
    return panel;
}

class DoubleClickFilter : public QObject {
public:
    DoubleClickFilter(QObject* parent, std::function<void()> f) : QObject(parent), fn(std::move(f)) {}
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::MouseButtonDblClick && fn) {
            fn();
            return true;
        }
        return false;
    }
    std::function<void()> fn;
};

// Reads a time typed in the Go To box (hh:mm:ss;ff, .fraction, or the ruler's format). -1 if unreadable.
double parseTime(const Project& p, const QString& text)
{
    double seconds = 0;
    if (!parseRulerTime(p, text, &seconds)) return -1;
    return seconds < 0 ? 0.0 : seconds;
}

}  // namespace


// ================================================================================================ the window
MainWindow::MainWindow() : a_(new Actions())
{
    setObjectName("MainWindow");
    setDockNestingEnabled(true);
    AppState& app = AppState::get();
    {
        // Loop Playback and Automatic Crossfades are restored before anything shows them.
        QSettings s;
        app.loopOn = s.value("options/loopPlayback", app.loopOn).toBool();
        app.setAutoCrossfades(s.value("options/autoCrossfades", app.autoCrossfades()).toBool());
    }
    playback_.reset(new Playback());
    qApp->installEventFilter(new TypingGuard(this));

    QWidget* center = new QWidget(this);
    center->setObjectName("timelineArea");
    auto* cl = new QVBoxLayout(center);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    timeline_ = new TimelineWidget(playback_.get(), center);
    cl->addWidget(timeline_, 1);
    setCentralWidget(center);
    timeline_->onOpenFx = [this](const FxTarget& t, uint64_t slot) { openFx(t, slot); };
    timeline_->onPanCrop = [this](uint64_t id) { openPanCrop(this, playback_.get(), id); };
    timeline_->onTrackMotion = [this](uint64_t id) { openTrackMotion(this, playback_.get(), id); };
    timeline_->onEventProperties = [this](uint64_t id) { runEventProperties(this, id); };
    timeline_->onSelectInMedia = [this](uint64_t mediaId) { if (mediaBin_) mediaBin_->selectMedia({mediaId}); };
    timeline_->onOptionsChanged = [this] { syncOptions(); };
    timeline_->onHeaderWidthChanged = [this](int w) { if (ratePanel_) ratePanel_->setFixedWidth(w); };
    timeline_->onTrackMenu = [this](QMenu* menu, uint64_t trackId) {
        const Track* t = nullptr;
        for (const Track& o : AppState::get().project().tracks)
            if (o.id == trackId) t = &o;
        if (t && t->type == TrackType::Video) compositingMenu(menu, trackId);
    };
    timeline_->onTrackCompositing = [this](QMenu* menu, uint64_t trackId) { compositingMenu(menu, trackId); };
    timeline_->onTrackDefaults = [this](QMenu* menu, uint64_t trackId) {
        addTrackDefaultsItem(menu, trackId, this);
    };

    fxWindow_ = new FxWindow(playback_.get(), this);
    fxWindow_->setWindowFlags(Qt::Tool);
    fxWindow_->resize(560, 620);
    if (QSettings().contains("fxWindow/geometry")) fxWindow_->restoreGeometry(QSettings().value("fxWindow/geometry").toByteArray());

    buildActions();
    cl->addWidget(buildBottomBar());
    buildMenus();
    buildToolBars();
    buildDocks();
    buildToolWindows();
    addOutputCommands(this);
    buildVegasMenus();
    installManualExtras(this);
    installKeyboardMap(this);
    installTrackDefaults();
    addPreferenceFileItems(this);

    // status bar: messages at the left; recording time left and free disk space at the right
    statusText_ = new QLabel(this);
    statusText_->setObjectName("statusText");
    diskText_ = new QLabel(this);
    diskText_->setObjectName("diskSpace");
    statusBar()->addWidget(statusText_, 1);
    statusBar()->addPermanentWidget(diskText_);
    statusBar()->setSizeGripEnabled(false);
    statusBar()->setFixedHeight(20);
    app.onStatus = [this](const std::string& s) { statusText_->setText(QString::fromStdString(s)); };
    installAudioProxyUi(this);
    QTimer* disk = new QTimer(this);
    disk->setInterval(30000);
    QObject::connect(disk, &QTimer::timeout, this, [this] { showDiskSpace(); });
    disk->start();
    AutomationHooks::playing = [this] { return playback_ && playback_->playing(); };
    AutomationHooks::position = [this] { return playback_ ? playback_->position() : 0.0; };
    QTimer* automationTimer = new QTimer(this);
    automationTimer->setInterval(30);
    // (runs only while automation takes are going, instead of waking the program 33 times a second)
    QObject::connect(automationTimer, &QTimer::timeout, this, [automationTimer] {
        if (!automationTick(AppState::get())) automationTimer->stop();
    });
    AutomationHooks::wake = [automationTimer] { if (!automationTimer->isActive()) automationTimer->start(); };

    listener_ = app.addListener([this](int changes) {
        if (changes & kPrefs) {
            applyDockOptions(this, docks_);
            bool wrongSide = false;
            for (QDockWidget* d : docks_)
                if (!d->isFloating() && d->isVisible() && dockWidgetArea(d) != dockArea()) wrongSide = true;
            if (wrongSide) {
                BuiltinLayout again = DefaultLayout;
                for (BuiltinLayout l : {LargePreviewLayout, AudioMixingLayout})
                    if (currentLayout_ == builtinLayoutName(l)) again = l;
                applyLayout(again);
            }
        }
        refresh(changes);
    });
    restyle();
    refresh(~0);
    syncOptions();
    resize(1600, 900);

    QSettings s;
    if (s.contains("window/geometry")) restoreGeometry(s.value("window/geometry").toByteArray());
    // Restore the layout from the last run (version 4; layouts saved by older versions are ignored).
    if (s.contains("window/state") && restoreState(s.value("window/state").toByteArray(), kLayoutVersion)) {
        startLayoutRestored_ = true;
        currentLayout_ = s.value("window/layout").toString();
    } else {
        applyLayout(DefaultLayout);
    }
    updateDockTabs();
    timeline_->setFocus();

    layoutSaver_ = new QTimer(this);
    layoutSaver_->setSingleShot(true);
    layoutSaver_->setInterval(500);
    QObject::connect(layoutSaver_, &QTimer::timeout, this, [this] { saveWindowLayout(); });
    struct LayoutWatch : QObject {
        QTimer* timer;
        explicit LayoutWatch(QObject* parent, QTimer* t) : QObject(parent), timer(t) {}
        bool eventFilter(QObject*, QEvent* e) override
        {
            switch (e->type()) {
            case QEvent::Resize:
            case QEvent::Move:
            case QEvent::Show:
            case QEvent::Hide:
            case QEvent::WindowStateChange: timer->start(); break;
            default: break;
            }
            return false;
        }
    };
    auto* watch = new LayoutWatch(this, layoutSaver_);
    installEventFilter(watch);
    fxWindow_->installEventFilter(watch);
    for (QDockWidget* d : findChildren<QDockWidget*>()) {
        d->installEventFilter(watch);
        QObject::connect(d, &QDockWidget::dockLocationChanged, layoutSaver_, [this] { layoutSaver_->start(); });
        QObject::connect(d, &QDockWidget::topLevelChanged, layoutSaver_, [this] { layoutSaver_->start(); });
    }
}

void MainWindow::saveWindowLayout()
{
    if (firstShow_) return;
    QSettings s;
    s.setValue("window/geometry", saveGeometry());
    s.setValue("window/state", timelineMaximized_ && !beforeMaximize_.isEmpty() ? beforeMaximize_ : saveState(kLayoutVersion));
    s.setValue("window/layout", currentLayout_);
    if (fxWindow_ && fxWindow_->isVisible()) s.setValue("fxWindow/geometry", fxWindow_->saveGeometry());
}

void MainWindow::restyle()
{
    if (QToolBar* main = findChild<QToolBar*>("mainToolBar"))
        main->setStyleSheet("QToolBar { border: none; padding: 4px 4px 3px 4px; background: qlineargradient(x1: 0, y1: 0, x2: 0, y2: 1, "
                            "stop: 0 #232323, stop: 0.0769 #232323, stop: 0.077 #828282, stop: 0.1025 #828282, stop: 0.1026 #464646, "
                            "stop: 0.923 #464646, stop: 0.9231 #232323, stop: 1 #232323); }");
    if (QSlider* rate = findChild<QSlider*>("rateSlider"))
        rate->setStyleSheet("QSlider::groove:horizontal { height: 2px; background: #232323; border-top: 1px solid #828282; }"
                            "QSlider::handle:horizontal { width: 11px; height: 10px; margin: -5px 0px; background: #e6e6e6; border: 1px solid #181818; }");
    const int widths[3] = {75, 72, 63};
    QLabel* labels[3] = {cursorText_, selEnd_, selLength_};
    for (int k = 0; k < 3; ++k) {
        if (!labels[k]) continue;
        labels[k]->setFont(QFont());
        labels[k]->setFixedWidth(widths[k]);
    }
    if (ratePanel_) ratePanel_->setFixedWidth(timeline_->headerWidth());
    update();
}

MainWindow::~MainWindow()
{
    AutomationHooks::wake = nullptr;
    if (layoutSaver_) layoutSaver_->stop();
    stopAudioProxies(false);
    tools_.reset();
    AppState::get().removeListener(listener_);
    AppState::get().onStatus = nullptr;
    delete fxWindow_;
    fxWindow_ = nullptr;
    delete shortcutsDialog_;
    shortcutsDialog_ = nullptr;
    for (QDockWidget* d : findChildren<QDockWidget*>()) delete d;
    delete centralWidget();
    timeline_ = nullptr;
    playback_.reset();
}

void MainWindow::buildActions()
{
    AppState& app = AppState::get();
    Actions& a = *a_;
    TimelineWidget* tl = timeline_;
    Playback* pb = playback_.get();
    auto add = [&](const char* name, const QString& text, const QList<QKeySequence>& k, std::function<void()> fn) {
        QAction* x = make(this, name, text, k, std::move(fn));
        if (*name) a.all[name] = x;
        addAction(x);
        return x;
    };
    auto checkable = [&](const char* name, const QString& text, const QList<QKeySequence>& k, std::function<void(bool)> fn) {
        QAction* x = new QAction(text, this);
        x->setObjectName(name);
        x->setCheckable(true);
        if (!k.isEmpty()) x->setShortcuts(k);
        QObject::connect(x, &QAction::toggled, this, [fn](bool on) { fn(on); });
        a.all[name] = x;
        addAction(x);
        return x;
    };
    auto moved = [tl] { tl->scrollToCursor(); };

    // ---- File
    add("new", "&New...", keys({"Ctrl+N"}), [this] {
        if (!maybeSave()) return;
        playback_->pause();
        dropPrerenders(playback_.get());
        AppState::get().newProject(true);
        runProjectPropertiesFull(this);
    });
    add("newPlain", "New (without Project Properties)", keys({"Ctrl+Shift+N"}), [this] {
        if (!maybeSave()) return;
        playback_->pause();
        dropPrerenders(playback_.get());
        AppState::get().newProject(true);
    });
    add("open", "&Open...", keys({"Ctrl+O"}), [this] {
        QStringList media;
        for (const std::string& e : mediaExtensions(MediaKind::Any)) media << "*." + QString::fromStdString(e);
        const QString f = QFileDialog::getOpenFileName(this, "Open", projectFolder(),
                                                       QString("All Projects and Media Files (*.veg *.reno %1);;VEGAS Pro projects (*.veg);;Reno projects (*.reno);;"
                                                               "All projects (*.veg *.reno);;Media files (%1);;All files (*)").arg(media.join(' ')));
        if (f.isEmpty()) return;
        if (!isProjectFile(f.toStdString()) && hasMediaExtension(f.toStdString())) {
            openProject(f);
            return;
        }
        if (!maybeSave()) return;
        openProject(f);
    });
    add("close", "&Close", keys({"Ctrl+F4"}), [this] {
        if (!maybeSave()) return;
        playback_->pause();
        dropPrerenders(playback_.get());
        AppState::get().newProject(true);
    });
    add("save", "&Save", keys({"Ctrl+S"}), [this] { saveProject(false); });
    add("saveAs", "Save &As...", keys({"Ctrl+Shift+S"}), [this] { saveProject(true); });
    add("saveVersion", "Save &Version", keys({"Ctrl+Alt+S"}), [this] { saveVersion(); });
    add("importMedia", "&Media...", keys({"Ctrl+I"}), [this] { if (mediaBin_) mediaBin_->importFiles(); });
    add("importImageSequence", "&Image Sequence...", QList<QKeySequence>(), [this] { if (mediaBin_) mediaBin_->importImageSequence(); });
    add("render", "&Render As...", QList<QKeySequence>(), [this] { playback_->pause(); runRenderDialog(this); });
    add("properties", "&Properties...", keys({"Alt+Return", "Alt+Enter"}), [this] { runProjectPropertiesFull(this); });
    add("exit", "E&xit", keys({"Alt+F4"}), [this] { close(); });

    // ---- Edit
    add("undo", "&Undo", keys({"Ctrl+Z", "Alt+Backspace"}), [&app] { app.undo(); });
    add("redo", "&Redo", keys({"Ctrl+Shift+Z", "Ctrl+Y"}), [&app] { app.redo(); });
    // Preferences > Editing "Cut, copy, and delete grouped events"
    auto withGroupsIfWanted = [this, &app] {
        if (!vprefs::on("prefs/groupedCutCopy")) return;
        if (QAction* ig = findChild<QAction*>("ignoreGrouping"))
            if (ig->isChecked()) return;
        std::set<uint64_t> sel = app.selection(), groups;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (sel.count(e.id) && e.group) groups.insert(e.group);
        if (groups.empty()) return;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (e.group && groups.count(e.group)) sel.insert(e.id);
        if (sel != app.selection()) app.setSelection(sel);
    };
    add("cut", "Cu&t", keys({"Ctrl+X", "Shift+Del"}), [this, tl, withGroupsIfWanted] { withGroupsIfWanted(); tl->cutSelection(); refresh(kSelection); });
    add("copy", "&Copy", keys({"Ctrl+C", "Ctrl+Ins"}), [this, &app, tl, withGroupsIfWanted] {
        if (tl->copyEnvelopePoints()) { refresh(kSelection); return; }
        withGroupsIfWanted();
        app.copySelection();
        refresh(kSelection);
    });
    add("paste", "&Paste", keys({"Ctrl+V", "Shift+Ins"}), [tl] { if (!tl->pasteEnvelopePoints()) tl->paste(); });
    add("pasteInsert", "Paste &Insert", keys({"Ctrl+Shift+V"}), [tl] { tl->pasteInsert(); });
    add("pasteRepeat", "Paste R&epeat...", keys({"Ctrl+B"}), [this] { pasteRepeat(); });
    add("delete", "&Delete", keys({"Del"}), [tl, withGroupsIfWanted] { withGroupsIfWanted(); tl->deleteSelection(); });
    add("selectAll", "Select &All", keys({"Ctrl+A"}), [&app] {
        std::set<uint64_t> all;
        for (const auto& t : app.project().tracks) for (const auto& e : t.events) all.insert(e.id);
        app.setSelection(all);
    });
    add("unselectAll", "U&nselect All", keys({"Ctrl+Shift+A"}), [&app] { app.setSelection(std::set<uint64_t>()); });
    add("split", "&Split", keys({"S"}), [&app] { app.splitAtCursor(); });
    add("trim", "T&rim", keys({"Ctrl+T"}), [tl] { tl->trimToTimeSelection(); });
    add("trimStart", "Trim Start", keys({"Alt+["}), [tl] { tl->trimStartToCursor(); });
    add("trimEnd", "Trim End", keys({"Alt+]"}), [tl] { tl->trimEndToCursor(); });
    add("rename", "Rena&me", keys({"F2"}), [tl] { tl->renameSelected(); });
    add("nestCreate", "&Create Nested Timeline", keys({"Alt+C"}), [this] { playback_->pause(); createNestedTimeline(this); });
    add("nestOpen", "&Open Nested Timeline", keys({"Alt+N"}), [this] { openNestedTimeline(this); });
    add("nestParent", "Open &Parent", keys({"Alt+P"}), [] { openParentTimeline(); });
    add("eventPanCrop", "Video Event &Pan/Crop...", QList<QKeySequence>(), [this, &app] {
        for (const Track& t : app.project().tracks)
            if (t.type == TrackType::Video)
                for (const Event& e : t.events)
                    if (app.selection().count(e.id)) { openPanCrop(this, playback_.get(), e.id); return; }
        app.status("Select a video event first.");
    });
    add("eventProperties", "Event Pr&operties...", QList<QKeySequence>(), [this, &app] {
        if (!app.selection().empty()) runEventProperties(this, *app.selection().begin());
    });
    auto switchAction = [&](const char* name, const QString& text, const std::string& what, std::function<bool(const Event&)> get, std::function<void(Event&, bool)> set) {
        QAction* x = add(name, text, QList<QKeySequence>(), [&app, what, get, set] {
            const std::set<uint64_t> sel = app.selection();
            bool all = !sel.empty();
            for (uint64_t id : sel) if (const Event* e = app.project().findEvent(id)) all = all && get(*e);
            app.edit(what, [&](Project& p) { for (uint64_t id : sel) if (Event* e = p.findEvent(id)) set(*e, !all); });
        });
        x->setCheckable(true);
        return x;
    };
    switchAction("switchMute", "&Mute", "Mute Events", [](const Event& e) { return e.mute; }, [](Event& e, bool v) { e.mute = v; });
    switchAction("switchLoop", "&Loop", "Loop Events", [](const Event& e) { return e.loop; }, [](Event& e, bool v) { e.loop = v; });
    switchAction("switchReverse", "&Reverse", "Reverse Events", [](const Event& e) { return e.reverse; }, [](Event& e, bool v) { e.reverse = v; });

    // ---- navigation
    add("goTo", "&Go To...", keys({"Ctrl+G"}), [this] { goToDialog(false); });
    add("setSelectionEnd", "Set &End of Time Selection...", keys({"Ctrl+Shift+G"}), [this] { goToDialog(true); });
    add("goStart", "Go to &Start", keys({"Ctrl+Home", "W"}), [pb, moved] { pb->jumpTo(0); moved(); });
    add("goEnd", "Go to E&nd", keys({"Ctrl+End"}), [pb, &app, moved] { pb->pause(); app.setCursor(app.project().length()); moved(); });
    // with the track list focused (Tab), Ctrl+Left / Right change track volume instead
    add("prevMarker", "Previous &Marker", keys({"Ctrl+Left"}), [tl] {
        if (tl->trackListFocused()) tl->changeTrackLevel(-1, false);
        else tl->cursorToMarker(-1);
    });
    add("nextMarker", "Next Mar&ker", keys({"Ctrl+Right"}), [tl] {
        if (tl->trackListFocused()) tl->changeTrackLevel(1, false);
        else tl->cursorToMarker(1);
    });
    add("trackPanLeft", "Track Pan Left (track list)", keys({"Ctrl+Shift+Left"}), [tl, &app] {
        if (tl->trackListFocused()) tl->changeTrackLevel(-1, true);
        else app.status("Ctrl+Shift+Left pans the selected tracks while the track list has the keyboard (Tab).");
    });
    add("trackPanRight", "Track Pan Right (track list)", keys({"Ctrl+Shift+Right"}), [tl, &app] {
        if (tl->trackListFocused()) tl->changeTrackLevel(1, true);
        else app.status("Ctrl+Shift+Right pans the selected tracks while the track list has the keyboard (Tab).");
    });
    auto frameStep = [pb, &app, moved](int dir) {
        const Project& p = app.project();
        pb->jumpTo(p.frameTime(std::max<int64_t>(0, p.frameAt(app.cursor()) + dir)));
        moved();
    };
    add("prevFrame", "Previous &Frame", keys({"Alt+Left"}), [frameStep] { frameStep(-1); });
    add("nextFrame", "Next F&rame", keys({"Alt+Right"}), [frameStep] { frameStep(1); });
    add("prevEditPoint", "Previous &Edit Point", keys({"Ctrl+Alt+Left"}), [tl] { tl->cursorToEditPoint(-1); });
    add("nextEditPoint", "Next Edit &Point", keys({"Ctrl+Alt+Right"}), [tl] { tl->cursorToEditPoint(1); });
    add("prevEdge", "Select Event &Start", keys({"["}), [tl] { tl->selectEdge(-1); });
    add("nextEdge", "Select Event &End", keys({"]"}), [tl] { tl->selectEdge(1); });
    add("expandedEdit", "Enter Expanded Edit &Mode\tNum 5", QList<QKeySequence>(), [tl] { tl->toggleExpandedEdit(); });
    add("exitEditMode", "Exit E&dit Mode", QList<QKeySequence>(), [tl] { tl->exitEditMode(); });
    add("jogLeft", "Jog Left", keys({"F3"}), [tl] { tl->jog(-1); });
    add("jogRight", "Jog Right", keys({"F9"}), [tl] { tl->jog(1); });
    add("selectLoopRegion", "&Select Loop Region", keys({"Shift+Q"}), [tl] { tl->selectLoopRegion(); });
    add("prevCdTrack", "Previous CD Track", keys({"Ctrl+,"}), [tl] { tl->cursorToCd(-1, true); });
    add("nextCdTrack", "Next CD Track", keys({"Ctrl+."}), [tl] { tl->cursorToCd(1, true); });
    add("prevCdIndex", "Previous CD Index or Region", keys({","}), [tl] { tl->cursorToCd(-1, false); });
    add("nextCdIndex", "Next CD Index or Region", keys({"."}), [tl] { tl->cursorToCd(1, false); });
    add("prevSelectedEdge", "Cursor to Previous Selected Event Edge", keys({"Ctrl+["}), [tl] { tl->cursorToSelectedEdge(-1); });
    add("nextSelectedEdge", "Cursor to Next Selected Event Edge", keys({"Ctrl+]"}), [tl] { tl->cursorToSelectedEdge(1); });
    add("timeSelectPrev", "Time Select Previous Selected Event", keys({"Ctrl+Shift+["}), [tl] { tl->timeSelectSelectedEvent(-1); });
    add("timeSelectNext", "Time Select Next Selected Event", keys({"Ctrl+Shift+]"}), [tl] { tl->timeSelectSelectedEvent(1); });
    add("centerCursor", "&Center View Around Cursor", keys({"\\"}), [tl] { tl->centerOnCursor(); });
    add("restoreSelection", "Restore Previous Time Selection", keys({"Backspace"}), [this, &app] {
        if (selectionHistory_.empty()) return;
        const std::pair<double, double> s = selectionHistory_.back();
        selectionHistory_.pop_back();
        restoringSelection_ = true;
        app.setLoop(s.first, s.second);
        restoringSelection_ = false;
    });
    add("cursorToPlayback", "Move Cursor to Playback Position", keys({"Alt+Down"}), [pb] {
        if (!pb->playing()) return;
        pb->pause();
        pb->play();
    });
    for (int n = 1; n <= 10; ++n) {
        const std::string name = strfmt("marker%d", n);
        // with multicamera editing on, 1..9 choose a camera instead
        a.all[name] = add("", QString("Go to Marker %1").arg(n), QList<QKeySequence>() << QKeySequence(QString::number(n % 10)), [tl, n] {
            if (tl->options().multicam && n <= 9) tl->multicamCut(n, false);
            else tl->goToMarker(n);
        });
        a.all[name]->setObjectName(QString::fromStdString(name));
        if (n <= 9) {
            const std::string cam = strfmt("multicamFade%d", n);
            a.all[cam] = add("", QString("Multicamera: Cross-fade to Camera %1").arg(n), QList<QKeySequence>() << QKeySequence(QString("Ctrl+%1").arg(n)), [tl, n, &app] {
                if (tl->options().multicam) tl->multicamCut(n, true);
                else app.status("Ctrl+1 ... 9 cross-fade between cameras while multicamera editing is on (Ctrl+Shift+D).");
            });
            a.all[cam]->setObjectName(QString::fromStdString(cam));
        }
    }

    // ---- groups, tools, pitch
    add("group", "&Create New", keys({"G"}), [&app] { app.groupSelection(true); });
    add("ungroup", "&Remove From", keys({"U"}), [&app] { app.groupSelection(false); });
    add("clearGroup", "C&lear", keys({"Ctrl+U"}), [tl] { tl->clearGroups(); });
    add("selectGroup", "&Select All", keys({"Shift+G"}), [tl] { tl->selectGroups(); });
    a.tools = new QActionGroup(this);
    auto tool = [&](const char* name, const QString& text, EditTool t, const QList<QKeySequence>& k) {
        QAction* x = add(name, text, k, [tl, t] {
            tl->options().tool = t;
            tl->optionsChanged();
        });
        x->setCheckable(true);
        a.tools->addAction(x);
        return x;
    };
    tool("toolNormal", "&Normal", EditTool::Normal, keys({"Ctrl+D"}))->setIcon(toolbarIcon(Ico::ToolNormal));
    tool("toolSelection", "&Selection", EditTool::Selection, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolSelect));
    tool("toolZoom", "&Zoom", EditTool::Zoom, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolZoom));
    auto unavailable = [&](const char* name, const QString& text, const QString& why, bool on) {
        QAction* x = add(name, text, QList<QKeySequence>(), [] {});
        x->setCheckable(on);
        x->setChecked(on);
        x->setEnabled(false);
        x->setData(why);
        return x;
    };
    tool("toolEnvelope", "&Envelope", EditTool::Envelope, QList<QKeySequence>());
    tool("toolShuffle", "S&huffle Tool", EditTool::Shuffle, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolShuffle));
    tool("toolSlip", "Sl&ip Tool", EditTool::Slip, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolSlip));
    tool("toolSlide", "Sli&de Tool", EditTool::Slide, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolSlide));
    tool("toolStretch", "Time Stretch/&Compress Tool", EditTool::Stretch, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolStretch));
    tool("toolSplitTrim", "Split &Trim Tool", EditTool::SplitTrim, QList<QKeySequence>())->setIcon(toolbarIcon(Ico::ToolSplitTrim));
    (void)unavailable;
    switchAction("lockEvent", "Loc&k", "Lock Events", [](const Event& e) { return e.locked; }, [](Event& e, bool v) { e.locked = v; });
    checkable("autoCrossfade", "&Automatic Crossfades", keys({"Ctrl+Shift+X"}), [&app](bool on) {
        app.setAutoCrossfades(on);
        QSettings().setValue("options/autoCrossfades", on);
        app.status(on ? "Automatic crossfades on: overlapping events cross-fade." : "Automatic crossfades off: where events overlap, the later one simply covers the earlier.");
    });
    checkable("quickFade", "Q&uickfade Audio Edits", QList<QKeySequence>(), [&app](bool on) {
        app.prefs.quickFadeEdits = on;
        app.applyPreferences();
        app.status(on ? "Quickfade audio edits on: audio events start and end with a short fade where they have none."
                      : "Quickfade audio edits off: audio events start and end abruptly where they have no fade.");
    });
    auto cycle = [this, tl](int dir) {
        // VEGAS's order: Normal, Envelope, Selection, Zoom
        const EditTool order[] = {EditTool::Normal, EditTool::Envelope, EditTool::Selection, EditTool::Zoom};
        int at = 0;
        for (int k = 0; k < 4; ++k) if (order[k] == tl->options().tool) at = k;
        const int next = (at + dir + 4) % 4;
        if (next == 0) {
            if (QToolButton* b = findChild<QToolButton*>("editToolButton"))
                if (QAction* x = b->defaultAction()) { x->trigger(); return; }
        }
        tl->options().tool = order[next];
        tl->optionsChanged();
    };
    add("toolNext", "Ne&xt Tool", keys({"D"}), [cycle] { cycle(1); });
    add("toolPrevious", "&Previous Tool", keys({"Shift+D"}), [cycle] { cycle(-1); });
    add("pitchUp", "Raise a &Semitone", keys({"="}), [this] { changePitch(1.0, false); });
    add("pitchDown", "Lower a S&emitone", keys({"-"}), [this] { changePitch(-1.0, false); });
    add("pitchUpCent", "Raise a &Cent", keys({"Ctrl+="}), [this] { changePitch(0.01, false); });
    add("pitchDownCent", "Lower a Ce&nt", keys({"Ctrl+-"}), [this] { changePitch(-0.01, false); });
    // Shift+= is + on most keyboards, Shift+- is _
    add("pitchUpOctave", "Raise an &Octave", keys({"Shift+=", "+"}), [this] { changePitch(12.0, false); });
    add("pitchDownOctave", "Lower an Oc&tave", keys({"Shift+-", "_"}), [this] { changePitch(-12.0, false); });
    add("pitchReset", "&Reset Pitch", keys({"Ctrl+Shift+=", "Ctrl+Shift+-", "Ctrl++", "Ctrl+_"}), [this] { changePitch(0, true); });

    // ---- View
    add("focusTimeline", "&Focus to Track View", keys({"Alt+0"}), [tl] { tl->setFocus(Qt::ShortcutFocusReason); });
    add("nextWindow", "Next Window", keys({"F6", "Ctrl+Tab"}), [this] { cycleFocus(1); });
    add("previousWindow", "Previous Window", keys({"Shift+F6", "Ctrl+Shift+Tab"}), [this] { cycleFocus(-1); });
    add("defaultLayout", "&Default Layout", keys({"Alt+D, D"}), [this] { applyLayout(DefaultLayout); });
    add("largePreviewLayout", "&Large Preview Layout", keys({"Alt+D, V"}), [this] { applyLayout(LargePreviewLayout); });
    add("audioMixingLayout", "&Audio Mixing Layout", keys({"Alt+D, A"}), [this] { applyLayout(AudioMixingLayout); });
    for (int n = 0; n < 10; ++n) {
        const std::string load = strfmt("loadLayout%d", n), save = strfmt("saveLayout%d", n);
        QAction* l = add("", QString("Layout %1").arg(n), QList<QKeySequence>() << QKeySequence(QString("Alt+D, %1").arg(n)), [this, n] {
            if (!recallLayout(n)) AppState::get().status(strfmt("No window layout has been saved as %d (Ctrl+Alt+D, %d saves one).", n, n));
        });
        l->setObjectName(QString::fromStdString(load));
        a.all[load] = l;
        a.loadLayouts.push_back(l);
        QAction* sv = add("", QString("Layout %1").arg(n), QList<QKeySequence>() << QKeySequence(QString("Ctrl+Alt+D, %1").arg(n)), [this, n] {
            QString name = QString("Layout %1").arg(n);
            for (const SavedLayout& x : savedLayoutList())
                if (x.key == n) name = x.name;
            saveLayout(name, n);
            AppState::get().status(strfmt("Window layout saved as %d (Alt+D, %d brings it back).", n, n));
        });
        sv->setObjectName(QString::fromStdString(save));
        a.all[save] = sv;
    }
    add("saveLayoutAs", "Save Layout &As...", QList<QKeySequence>(), [this] { saveLayoutAsDialog(); });
    add("saveCurrentLayout", "&Save Layout", QList<QKeySequence>(), [this] {
        for (const SavedLayout& x : savedLayoutList())
            if (x.name == currentLayout_) { saveLayout(x.name, x.key); return; }
    });
    add("reloadLayout", "&Reload Selected Layout", QList<QKeySequence>(), [this] { reloadLayout(); });
    add("organizeLayouts", "&Organize Layouts...", QList<QKeySequence>(), [this] { organizeLayouts(); });
    add("zoomIn", "Zoom &In\tUp", QList<QKeySequence>(), [tl] { tl->zoomIn(); });
    add("zoomOut", "Zoom &Out\tDown", QList<QKeySequence>(), [tl] { tl->zoomOut(); });
    add("zoomInLarge", "Zoom In (to the Time Selection)", keys({"Ctrl+Up"}), [tl] { tl->zoomLarge(true); });
    add("zoomOutLarge", "Zoom Out (Large Steps)", keys({"Ctrl+Down"}), [tl] { tl->zoomLarge(false); });
    add("zoomFit", "Zoom to &Fit", QList<QKeySequence>(), [tl] { tl->zoomToFit(); });
    add("tracksTaller", "Increase Track Height", keys({"Ctrl+Shift+Up"}), [tl] { tl->changeTrackHeights(1); });
    add("tracksDefault", "Default Track Heights", keys({"Ctrl+`"}), [tl] { tl->resetTrackHeights(); });
    add("zoomFrames", "Zoom In to Frames", keys({"Alt+Up"}), [tl] { tl->zoomToFrames(); });
    add("previousTrack", "Previous Track", keys({"Alt+Shift+Up"}), [this] { stepTrack(-1); });
    add("nextTrack", "Next Track", keys({"Alt+Shift+Down"}), [this] { stepTrack(1); });
    add("tracksShorter", "Decrease Track Height", keys({"Ctrl+Shift+Down"}), [tl] { tl->changeTrackHeights(-1); });
    // ` is Minimize All Tracks (again restores)
    add("minimizeTrack", "Minimize / Restore Track Height", QList<QKeySequence>(), [tl] { tl->toggleMinimizeTrack(); });
    add("maximizeTrack", "Maximize / Restore Track Height", QList<QKeySequence>(), [tl] { tl->toggleMaximizeTrack(); });
    checkable("maximizeTimeline", "Maximize Timeline", keys({"Ctrl+F11", "Ctrl+Alt+`", "Ctrl+Alt+Shift+F11"}), [this](bool on) {
        if (on == timelineMaximized_) return;
        timelineMaximized_ = on;
        if (on) {
            beforeMaximize_ = saveState(kLayoutVersion);
            for (QDockWidget* d : docks_) d->hide();
        } else {
            restoreState(beforeMaximize_, kLayoutVersion);
        }
    });
    checkable("showWaveforms", "Show &Waveforms", keys({"Ctrl+Shift+W"}), [tl](bool on) {
        tl->options().showWaveforms = on;
        tl->optionsChanged();
    });
    checkable("showEventButtons", "Show Event &Buttons", keys({"Ctrl+Shift+C"}), [tl](bool on) {
        tl->options().showEventButtons = on;
        tl->optionsChanged();
    });
    checkable("eventHeaders", "Show Event &Headers", keys({"Ctrl+Alt+M"}), [tl](bool on) {
        tl->options().eventHeaders = on;
        tl->optionsChanged();
    });
    add("dockingArea", "Minimize / Restore the Window Docking Area", keys({"F11", "Alt+`"}), [this] { a_->get("maximizeTimeline")->toggle(); });
    // VEGAS 23 docking areas: Alt+F11 top, Ctrl+Alt+F11 bottom, Ctrl+Shift+F11 right (docked windows hide and return); left has no key
    auto areaToggle = [this](Qt::DockWidgetArea area) {
        std::vector<QDockWidget*>& hidden = hiddenByArea_[(int)area];
        if (!hidden.empty()) {
            for (QDockWidget* d : hidden) d->show();
            hidden.clear();
            return;
        }
        for (QDockWidget* d : docks_)
            if (!d->isFloating() && d->isVisible() && dockWidgetArea(d) == area) hidden.push_back(d);
        for (QDockWidget* d : hidden) d->hide();
    };
    add("dockTop", "Show / Hide the &Top Docking Area", keys({"Alt+F11"}), [areaToggle] { areaToggle(Qt::TopDockWidgetArea); });
    add("dockBottom", "Show / Hide the &Bottom Docking Area", keys({"Ctrl+Alt+F11"}), [areaToggle] { areaToggle(Qt::BottomDockWidgetArea); });
    add("dockRight", "Show / Hide the &Right Docking Area", keys({"Ctrl+Shift+F11"}), [areaToggle] { areaToggle(Qt::RightDockWidgetArea); });
    add("dockLeft", "Show / Hide the &Left Docking Area", QList<QKeySequence>(), [areaToggle] { areaToggle(Qt::LeftDockWidgetArea); });

    // ---- Insert
    add("audioTrack", "&Audio Track", keys({"Ctrl+Q"}), [&app] { app.currentTrack = app.addTrack(TrackType::Audio); app.notify(kSelection); });
    add("videoTrack", "&Video Track", keys({"Ctrl+Shift+Q"}), [&app] { app.currentTrack = app.addTrack(TrackType::Video); app.notify(kSelection); });
    add("audioBus", "Audio &Bus", QList<QKeySequence>(), [&app] {
        app.edit("Insert Audio Bus", [](Project& p) {
            Bus b;
            b.id = p.newId();
            b.name = std::string("Bus ") + (char)('A' + (int)(p.buses.size() % 26));
            p.buses.push_back(b);
        });
    });
    add("generatedMedia", "&Generated Media...", QList<QKeySequence>(), [this] {
        insertGeneratedMedia(this, [this](const FxTarget& t) { openFx(t, 0); });
    });
    add("marker", "&Marker", keys({"M"}), [tl] { tl->insertMarker(); });
    add("region", "&Region", keys({"R"}), [tl] { tl->insertRegion(); });

    // ---- Tools
    add("videoOutFx", "Video &Output FX...", QList<QKeySequence>(), [this] { FxTarget t; t.kind = FxTarget::VideoOut; openFx(t, 0); });
    add("masterFx", "&Master Bus FX...", QList<QKeySequence>(), [this] { FxTarget t; t.kind = FxTarget::AudioOut; openFx(t, 0); });
    add("eventFx", "&Event FX...", QList<QKeySequence>(), [this, &app] {
        if (app.selection().empty()) return;
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = *app.selection().begin();
        openFx(t, 0);
    });
    add("trackFx", "&Track FX...", QList<QKeySequence>(), [this, &app] {
        if (!app.currentTrack) return;
        FxTarget t;
        t.kind = FxTarget::Track;
        t.id = app.currentTrack;
        openFx(t, 0);
    });
    add("trackMotion", "Track &Motion...", QList<QKeySequence>(), [this, &app] {
        const Project& p = app.project();
        for (const Track& t : p.tracks)
            if (t.id == app.currentTrack && t.type == TrackType::Video) { openTrackMotion(this, playback_.get(), t.id); return; }
        app.status("Select a video track first.");
    });
    checkable("bypassFx", "&Bypass All Video FX", QList<QKeySequence>(), [&app](bool on) {
        if (app.bypassAllFx == on) return;
        app.bypassAllFx = on;
        app.notify(kFxParams);
    });
    add("rescan", "Look for &New Plug-Ins", QList<QKeySequence>(), [this] { runPluginScan(this, false); });
    add("rescanAll", "&Check All Plug-Ins Again", QList<QKeySequence>(), [this] { runPluginScan(this, true); });

    // ---- Options
    add("preferences", "&Preferences...", QList<QKeySequence>(), [this] { runPreferences(this); });
    auto option = [&](const char* name, const QString& text, const QList<QKeySequence>& k, bool TimelineOptions::*field) {
        return checkable(name, text, k, [tl, field](bool on) {
            if (tl->options().*field == on) return;
            tl->options().*field = on;
            tl->optionsChanged();
        });
    };
    option("quantize", "Enable &Quantize to Frames", keys({"Alt+F8"}), &TimelineOptions::quantize);
    option("showUnquantized", "Show &Unquantized Event Edges", QList<QKeySequence>(), &TimelineOptions::showUnquantized);
    checkable("snapping", "Enable &Snapping", keys({"F8"}), [&app, this](bool on) {
        if (app.snapping == on) return;
        app.snapping = on;
        app.prefs.snapping = on;
        QSettings().setValue("prefs/snapping", on);
        syncOptions();
    });
    option("snapGrid", "Snap to &Grid", keys({"Ctrl+F8"}), &TimelineOptions::snapToGrid);
    option("snapMarkers", "Snap to &Markers", keys({"Shift+F8"}), &TimelineOptions::snapToMarkers);
    option("autoRipple", "Auto &Ripple", keys({"Ctrl+L"}), &TimelineOptions::autoRipple);
    option("ignoreGrouping", "&Ignore Event Grouping", keys({"Ctrl+Shift+U"}), &TimelineOptions::ignoreGrouping);
    // ---- envelopes
    option("lockEnvelopes", "Lock &Envelopes to Events", QList<QKeySequence>(), &TimelineOptions::lockEnvelopes);
    option("showEnvelopes", "Show &Envelopes", keys({"Ctrl+Shift+E"}), &TimelineOptions::showEnvelopes);
    option("showBusTracks", "Show &Bus Tracks", keys({"B", "Ctrl+Shift+B"}), &TimelineOptions::showBusTracks);
    option("showFadeLengths", "Show Event &Fade Lengths", keys({"Ctrl+Shift+T"}), &TimelineOptions::showFadeLengths);
    add("envVolume", "&Volume Envelope", keys({"V"}), [tl] { tl->toggleTrackEnvelope("volume"); });
    add("envVolumeRemove", "Remove Volume Envelope", keys({"Shift+V"}), [tl] { tl->removeTrackEnvelope("volume"); });
    add("envPan", "&Pan Envelope", keys({"P"}), [tl] { tl->toggleTrackEnvelope("pan"); });
    add("envPanRemove", "Remove Pan Envelope", keys({"Shift+P"}), [tl] { tl->removeTrackEnvelope("pan"); });
    add("envNext", "Next Envelope", keys({"E"}), [tl] { tl->cycleEnvelope(1); });
    add("envPrevious", "Previous Envelope", keys({"Shift+E"}), [tl] { tl->cycleEnvelope(-1); });
    a.ripple = new QActionGroup(this);
    const struct { const char* name; const char* text; RippleMode mode; } modes[] = {
        {"rippleTracks", "Affected Tracks", RippleMode::AffectedTracks},
        {"rippleMarkers", "Affected Tracks, Bus Tracks, Markers, and Regions", RippleMode::AffectedTracksAndMarkers},
        {"rippleAll", "All Tracks, Markers, and Regions", RippleMode::AllTracks}};
    for (const auto& m : modes) {
        const RippleMode mode = m.mode;
        QAction* x = add(m.name, m.text, QList<QKeySequence>(), [tl, mode] {
            tl->options().ripple = mode;
            tl->optionsChanged();
        });
        x->setCheckable(true);
        a.ripple->addAction(x);
    }
    checkable("loop", "&Loop Playback", keys({"Q", "Ctrl+Shift+L"}), [&app](bool on) {
        if (app.loopOn == on) return;
        app.loopOn = on;
        app.notify(kCursor);
    });

    // ---- tracks (no menu in VEGAS either)
    add("muteTrack", "Mute Selected Track", keys({"Z"}), [this] { muteSolo(false, false); });
    add("muteTrackOnly", "Mute Selected Track Only", keys({"Shift+Z"}), [this] { muteSolo(false, true); });
    add("soloTrack", "Solo Selected Track", keys({"X"}), [this] { muteSolo(true, false); });
    add("soloTrackOnly", "Solo Selected Track Only", keys({"Shift+X"}), [this] { muteSolo(true, true); });

    // ---- playback (Preferences > General: Space and F12 can be Play/Pause)
    add("playStop", "Play / Stop", keys({"Space"}), [pb] {
        if (pb->playing()) {
            if (vprefs::on("prefs/spacePlayPause")) pb->pause();
            else stopPlayback(pb);
        } else pb->play();
    });
    add("stop", "Stop", keys({"Esc"}), [pb, tl] {
        if (tl->cancelMouseDrag()) return;
        if (tl->edgeTrimming()) tl->exitEditMode();
        stopPlayback(pb);
    });
    add("playPause", "Play / Pause", keys({"Return", "Enter", "Ctrl+F12"}), [pb] {
        if (pb->playing()) pb->pause();
        else pb->play();
    });
    add("playFromStart", "Play From Start", keys({"Shift+Space", "Shift+F12"}), [pb, &app, moved] {
        pb->pause();
        app.setCursor(app.loopEnd > app.loopStart ? app.loopStart : 0.0);
        moved();
        pb->play();
    });
    add("playAny", "Play from Any Window", keys({"Ctrl+Space", "F12"}), [pb] {
        if (pb->playing()) {
            if (vprefs::on("prefs/spacePlayPause")) pb->pause();
            else stopPlayback(pb);
        } else pb->play();
    });
    add("previewCursor", "Preview Cursor Position", keys({"Num+0"}), [pb] { pb->previewCursor(AppState::get().prefs.cursorPreview); });
    checkable("dimOutput", "&Dim Master Bus Output", keys({"Ctrl+Shift+F12"}), [pb, &app](bool on) {
        pb->setDim(on);
        app.status(on ? "Master output dimmed by 20 dB." : "Master output at its full level again.");
    });
    add("ramPreview", "Build Dynamic &RAM Preview", keys({"Shift+B"}), [this] { buildRamPreview(); });
    add("renderToNewTrack", "Render to &New Track...", keys({"Ctrl+M"}), [this] { renderToNewTrack(); });
    add("shuttleBack", "Shuttle Back", keys({"J"}), [this] { shuttle(-1); })->setAutoRepeat(false);
    add("shuttlePause", "Slow Motion", keys({"K"}), [this] { shuttle(0); })->setAutoRepeat(false);
    add("shuttleForward", "Shuttle Forward", keys({"L"}), [this] { shuttle(1); })->setAutoRepeat(false);

    // ---- more editing (more of VEGAS's commands)
    add("smartSplit", "S&mart Split", keys({"Alt+S"}), [tl] { tl->smartSplit(); });
    add("detectScenes", "Detect &Scenes...", {}, [this, &app] {
        if (app.selection().empty()) { app.status("Detect Scenes: select a video event first."); return; }
        bool ok = false;
        const int s = QInputDialog::getInt(this, "Detect Scenes", "Sensitivity (higher finds more scene changes):", 50, 0, 100, 5, &ok);
        if (!ok) return;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        app.splitAtScenes(s);
        QApplication::restoreOverrideCursor();
    });
    add("takeNext", "&Next Take", keys({"T"}), [&app] { app.stepTakes(app.selection(), 1); });
    add("takePrevious", "&Previous Take", keys({"Shift+T"}), [&app] { app.stepTakes(app.selection(), -1); });
    add("takeAdd", "&Add...", QList<QKeySequence>(), [this, tl, &app] {
        if (app.selection().empty()) { app.status("Select the event to add takes to first."); return; }
        const QStringList files = QFileDialog::getOpenFileNames(this, "Add Takes", QSettings().value("media/importFolder", projectFolder()).toString(), "Media files (*)");
        if (!files.isEmpty()) QSettings().setValue("media/importFolder", QFileInfo(files.first()).absolutePath());
        std::vector<uint64_t> media;
        for (const QString& f : files) {
            std::string err;
            if (const uint64_t m = app.importMedia(f.toStdString(), &err)) media.push_back(m);
            else app.status("Could not open " + f.toStdString() + (err.empty() ? std::string() : ": " + err));
        }
        if (!media.empty()) tl->addTakesToSelected(media);
    });
    add("takeChoose", "&Choose Active...", QList<QKeySequence>(), [this] { chooseTakeDialog(false); });
    add("takeDeleteActive", "Delete &Active", QList<QKeySequence>(), [&app] {
        for (uint64_t id : app.selection()) app.deleteActiveTake(id);
    });
    add("takeDelete", "&Delete...", QList<QKeySequence>(), [this] { chooseTakeDialog(true); });
    add("takeRename", "&Rename Active", QList<QKeySequence>(), [this, &app] {
        if (app.selection().empty()) return;
        const uint64_t id = *app.selection().begin();
        const Event* e = app.project().findEvent(id);
        if (!e) return;
        bool ok = false;
        const QString name = QInputDialog::getText(this, "Rename Active Take", "Name:", QLineEdit::Normal, QString::fromStdString(e->name), &ok);
        if (ok) app.renameActiveTake(id, name.toStdString());
    });
    checkable("showTakeInfo", "Active Take &Information", keys({"Ctrl+Shift+I"}), [tl](bool on) {
        tl->options().showTakeInfo = on;
        tl->optionsChanged();
    });
    checkable("showMediaMarkers", "Event Media &Markers", keys({"Ctrl+Shift+K"}), [tl](bool on) {
        tl->options().showMediaMarkers = on;
        tl->optionsChanged();
    });
    checkable("edgeFrames", "Video Event Edge Edit &Frames", keys({"Ctrl+Shift+O"}), [tl](bool on) {
        tl->options().edgeFrames = on;
        tl->optionsChanged();
    });
    add("rebuildPeaks", "Rebuild Audio Pea&ks", keys({"Shift+F5"}), [tl] { tl->rebuildPeaks(); });
    add("trackList", "Minimize / Restore the Track List", keys({"Shift+F11", "Shift+Alt+`"}), [tl] { tl->toggleTrackList(); });
    add("cutToCrossfade", "Convert Cut to Cross-fade", keys({"Num+/"}), [tl] { tl->convertCutToTransition(0); });
    add("cutToDissolve", "Convert Cut to Dissolve", keys({"Num+*"}), [tl] { tl->convertCutToTransition(1); });
    add("cutToWipe", "Convert Cut to Linear Wipe", keys({"Num+-"}), [tl] { tl->convertCutToTransition(2); });
    add("transitionToCut", "Convert Transition to Cut", keys({"Ctrl+Num+/"}), [tl] { tl->convertTransitionToCut(); });
    add("postRippleTracks", "Affected &Tracks", keys({"F"}), [tl] { tl->postEditRipple(RippleMode::AffectedTracks); });
    add("postRippleBus", "Affected Tracks, &Bus Tracks, Markers, and Regions", keys({"Ctrl+F"}), [tl] { tl->postEditRipple(RippleMode::AffectedTracksAndMarkers); });
    add("postRippleMarkers", "Affected Tracks, &Markers, and Regions", QList<QKeySequence>(), [tl] { tl->postEditRipple(RippleMode::AffectedTracksAndMarkers); });
    add("postRippleAll", "&All Tracks, Markers, and Regions", keys({"Ctrl+Shift+F"}), [tl] { tl->postEditRipple(RippleMode::AllTracks); });
    option("snapAllEvents", "Snap to All Even&ts", keys({"Ctrl+Shift+F8"}), &TimelineOptions::snapToAllEvents);
    option("slipAllTakes", "Slip All Ta&kes", QList<QKeySequence>(), &TimelineOptions::slipAllTakes);
    add("cdTrack", "Audio CD Trac&k Region", keys({"N"}), [tl] { tl->insertCdTrack(); });
    add("cdIndex", "Audio CD Track Inde&x", keys({"Shift+N"}), [tl] { tl->insertCdIndex(); });
    add("commandMarker", "&Command...", keys({"C"}), [this] { insertCommandDialog(); });
    add("groupTracks", "&Group Selected Tracks", QList<QKeySequence>(), [tl] { tl->groupTracks(true); });
    add("ungroupTracks", "&Ungroup Selected Tracks", QList<QKeySequence>(), [tl] { tl->groupTracks(false); });
    add("collapseTrackGroup", "&Collapse", QList<QKeySequence>(), [tl] { tl->collapseTrackGroup(true); });
    add("expandTrackGroup", "E&xpand", QList<QKeySequence>(), [tl] { tl->collapseTrackGroup(false); });
    add("muteTrackGroup", "&Mute Track Group", keys({"Ctrl+Alt+Z"}), [tl] { tl->muteSoloTrackGroup(false, false); });
    add("muteTrackGroupOnly", "Mute Track Group Only", keys({"Ctrl+Alt+Shift+Z"}), [tl] { tl->muteSoloTrackGroup(false, true); });
    add("soloTrackGroup", "&Solo Track Group", keys({"Ctrl+Alt+X"}), [tl] { tl->muteSoloTrackGroup(true, false); });
    add("soloTrackGroupOnly", "Solo Track Group Only", keys({"Ctrl+Alt+Shift+X"}), [tl] { tl->muteSoloTrackGroup(true, true); });
    add("createMulticam", "&Create Multicamera Track", QList<QKeySequence>(), [tl] { tl->createMulticamTrack(); });
    checkable("multicam", "&Enable Multicamera Editing", keys({"Ctrl+Shift+D"}), [tl, &app](bool on) {
        tl->options().multicam = on;
        tl->optionsChanged();
        app.status(on ? "Multicamera editing: 1 ... 9 choose a camera at the cursor (a cut there while playing); Ctrl+1 ... 9 cross-fade to it."
                      : "Multicamera editing off: the number keys go to markers again.");
    });
    add("audioEditor", "&Open in Audio Editor", keys({"Ctrl+E"}), [this, &app] {
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (app.selection().count(e.id)) { openInAudioEditor(e.id); return; }
        app.status("Select an event first: Ctrl+E opens its media in the audio editor.");
    });
    timeline_->onAudioEditor = [this](uint64_t id) { openInAudioEditor(id); };
    add("mediaPlayPause", "Play / Pause (multimedia key)", keys({"Toggle Media Play/Pause", "Media Play"}), [pb] {
        if (pb->playing()) pb->pause();
        else pb->play();
    });
    add("mediaPlayFromStart", "Play From Start (multimedia key)", keys({"Shift+Toggle Media Play/Pause", "Shift+Media Play"}), [pb, &app, moved] {
        pb->pause();
        app.setCursor(app.loopEnd > app.loopStart ? app.loopStart : 0.0);
        moved();
        pb->play();
    });
    add("mediaStop", "Stop (multimedia key)", keys({"Media Stop"}), [pb] { stopPlayback(pb); });
    add("mediaMute", "Mute Track (multimedia key)", keys({"Volume Mute"}), [this] { muteSolo(false, false); });
    add("mediaMuteOnly", "Mute Track Only (multimedia key)", keys({"Shift+Volume Mute"}), [this] { muteSolo(false, true); });
    add("mediaSolo", "Solo Track (multimedia key)", keys({"Ctrl+Volume Mute"}), [this] { muteSolo(true, false); });
    add("mediaSoloOnly", "Solo Track Only (multimedia key)", keys({"Ctrl+Shift+Volume Mute"}), [this] { muteSolo(true, true); });
    add("mediaVolumeUp", "Track Volume Up (multimedia key)", keys({"Ctrl+Volume Up"}), [tl] { tl->changeTrackLevel(1, false); });
    add("mediaVolumeDown", "Track Volume Down (multimedia key)", keys({"Ctrl+Volume Down"}), [tl] { tl->changeTrackLevel(-1, false); });
    add("mediaPanRight", "Track Pan Right (multimedia key)", keys({"Shift+Volume Up"}), [tl] { tl->changeTrackLevel(1, true); });
    add("mediaPanLeft", "Track Pan Left (multimedia key)", keys({"Shift+Volume Down"}), [tl] { tl->changeTrackLevel(-1, true); });
    add("mediaNextTrack", "Next Track (multimedia key)", keys({"Media Next"}), [this] { stepTrack(1); });
    add("mediaPreviousTrack", "Previous Track (multimedia key)", keys({"Media Previous"}), [this] { stepTrack(-1); });

    // ---- Help
    add("helpContents", "&Contents and Index", keys({"F1"}), [this] { showHelp(); });
    add("whatsThis", "What's &This?", keys({"Shift+F1"}), [] { QWhatsThis::enterWhatsThisMode(); });
    add("shortcuts", "&Keyboard Shortcuts...", QList<QKeySequence>(), [this] { showShortcuts(); });
    add("about", "&About Reno", QList<QKeySequence>(), [this] { showAbout(this); });

    const struct { const char* name; Ico ico; } pics[] = {
        {"new", Ico::New}, {"open", Ico::Open}, {"save", Ico::Save}, {"saveAs", Ico::SaveAs}, {"render", Ico::Render}, {"properties", Ico::Properties},
        {"cut", Ico::Cut}, {"copy", Ico::Copy}, {"paste", Ico::Paste}, {"undo", Ico::Undo}, {"redo", Ico::Redo}, {"delete", Ico::Delete},
        {"trim", Ico::Trim}, {"trimStart", Ico::TrimStart}, {"trimEnd", Ico::TrimEnd}, {"split", Ico::Split}, {"ignoreGrouping", Ico::IgnoreGroup},
        {"autoRipple", Ico::Ripple}, {"snapping", Ico::Snap}, {"quantize", Ico::Quantize}, {"marker", Ico::Marker}, {"region", Ico::Region},
        {"toolEnvelope", Ico::ToolEnvelope}, {"lockEvent", Ico::Lock}, {"autoCrossfade", Ico::AutoCrossfade}, {"lockEnvelopes", Ico::LockEnvelopes},
        {"group", Ico::Group}, {"ungroup", Ico::Ungroup}, {"clearGroup", Ico::ClearGroup}, {"whatsThis", Ico::WhatsThis},
        {"showColorGrading", Ico::ColorGrading}, {"nestParent", Ico::NestParent}, {"nestOpen", Ico::NestOpen}, {"nestCreate", Ico::NestCreate}};
    for (const auto& pic : pics)
        if (QAction* x = a.get(pic.name)) x->setIcon(toolbarIcon(pic.ico));
    for (auto& kv : a.all) {
        QAction* x = kv.second;
        const QString text = x->text().split('\t')[0].remove('&');
        QString tip = x->shortcut().isEmpty() ? text : QString("%1 (%2)").arg(text, x->shortcut().toString(QKeySequence::NativeText));
        if (!x->data().toString().isEmpty()) tip += ": " + x->data().toString();
        x->setToolTip(tip);
        x->setWhatsThis(tip);
        x->setIconVisibleInMenu(false);
    }
}

void MainWindow::buildMenus()
{
    Actions& a = *a_;
    auto g = [&](const char* n) { return a.get(n); };
    QMenu* file = menuBar()->addMenu("&File");
    file->addActions({g("new"), g("open"), g("close"), g("save"), g("saveAs"), g("saveVersion")});
    file->addSeparator();
    QMenu* imp = file->addMenu("&Import");
    imp->addActions({g("importMedia"), g("importImageSequence")});
    file->addAction(g("render"));
    file->addSeparator();
    file->addAction(g("properties"));
    file->addSeparator();
    QAction* recentStart = file->addSeparator();
    file->addAction(g("exit"));
    QObject::connect(file, &QMenu::aboutToShow, this, [this, file, recentStart] {
        for (QAction* x : file->actions())
            if (x->property("recentProject").isValid()) {
                file->removeAction(x);
                x->deleteLater();
            }
        QStringList recent = QSettings().value("recentProjects").toStringList();
        if (!vprefs::on("prefs/recentList")) recent.clear();
        while (recent.size() > std::max(1, vprefs::number("prefs/recentCount"))) recent.removeLast();
        QAction* before = nullptr;
        {
            const QList<QAction*> all = file->actions();
            const int i = all.indexOf(recentStart);
            before = i >= 0 && i + 1 < all.size() ? all[i + 1] : nullptr;
        }
        int n = 0;
        for (const QString& f : recent) {
            QAction* x = new QAction(QString("&%1 %2").arg(++n).arg(QDir::toNativeSeparators(f)), file);
            x->setProperty("recentProject", f);
            QObject::connect(x, &QAction::triggered, this, [this, f] {
                if (maybeSave()) openProject(f);
            });
            file->insertAction(before, x);
        }
        recentStart->setVisible(!recent.isEmpty());
    });

    QMenu* edit = menuBar()->addMenu("&Edit");
    edit->addActions({g("undo"), g("redo")});
    edit->addSeparator();
    edit->addActions({g("cut"), g("copy"), g("paste"), g("pasteInsert"), g("pasteRepeat"), g("delete")});
    edit->addSeparator();
    QMenu* select = edit->addMenu("Se&lect");
    select->addActions({g("selectAll"), g("unselectAll")});
    select->addSeparator();
    select->addActions({g("prevEdge"), g("nextEdge"), g("expandedEdit"), g("exitEditMode")});
    select->addSeparator();
    select->addAction(g("selectLoopRegion"));
    select->addSeparator();
    select->addActions({g("timeSelectPrev"), g("timeSelectNext"), g("restoreSelection")});
    QMenu* nav = edit->addMenu("&Navigate");
    nav->addActions({g("goTo"), g("setSelectionEnd")});
    nav->addSeparator();
    nav->addActions({g("goStart"), g("goEnd"), g("prevMarker"), g("nextMarker"), g("prevFrame"), g("nextFrame")});
    nav->addSeparator();
    nav->addActions({g("prevEditPoint"), g("nextEditPoint"), g("prevSelectedEdge"), g("nextSelectedEdge")});
    nav->addSeparator();
    nav->addActions({g("jogLeft"), g("jogRight"), g("prevCdTrack"), g("nextCdTrack"), g("prevCdIndex"), g("nextCdIndex")});
    nav->addSeparator();
    nav->addActions({g("centerCursor"), g("cursorToPlayback")});
    QMenu* postRipple = edit->addMenu("P&ost-Edit Ripple");
    postRipple->addActions({g("postRippleTracks"), g("postRippleBus"), g("postRippleMarkers"), g("postRippleAll")});
    QMenu* tools = edit->addMenu("Editing &Tool");
    tools->addActions({g("toolNormal"), g("toolShuffle"), g("toolSlip"), g("toolSlide"), g("toolStretch"), g("toolSplitTrim"), g("toolEnvelope"), g("toolSelection"), g("toolZoom")});
    tools->addSeparator();
    tools->addActions({g("toolNext"), g("toolPrevious")});
    edit->addSeparator();
    edit->addActions({g("trim"), g("trimStart"), g("trimEnd"), g("split"), g("smartSplit"), g("rename")});
    edit->addSeparator();
    QMenu* nest = edit->addMenu("Ne&sted Timelines");
    nest->addActions({g("nestCreate"), g("nestOpen"), g("nestParent")});
    edit->addSeparator();
    QMenu* group = edit->addMenu("&Group");
    group->addActions({g("group"), g("ungroup"), g("clearGroup"), g("selectGroup")});
    QMenu* sw = edit->addMenu("S&witches");
    sw->addActions({g("switchMute"), g("lockEvent"), g("switchLoop"), g("switchReverse")});
    QObject::connect(sw, &QMenu::aboutToShow, this, [this, sw] {
        AppState& app = AppState::get();
        const std::set<uint64_t> sel = app.selection();
        bool mute = !sel.empty(), loop = !sel.empty(), rev = !sel.empty(), lock = !sel.empty();
        for (uint64_t id : sel)
            if (const Event* e = app.project().findEvent(id)) {
                mute = mute && e->mute;
                loop = loop && e->loop;
                rev = rev && e->reverse;
                lock = lock && e->locked;
            }
        a_->get("lockEvent")->setChecked(lock);
        a_->get("switchMute")->setChecked(mute);
        a_->get("switchLoop")->setChecked(loop);
        a_->get("switchReverse")->setChecked(rev);
        for (QAction* x : sw->actions()) x->setEnabled(!sel.empty());
    });
    QMenu* take = edit->addMenu("Ta&ke");
    take->addActions({g("takeRename"), g("takeChoose")});
    take->addSeparator();
    take->addActions({g("takeNext"), g("takePrevious")});
    take->addSeparator();
    take->addActions({g("takeDeleteActive"), g("takeDelete")});
    take->addSeparator();
    take->addAction(g("takeAdd"));
    QMenu* pitch = edit->addMenu("&Pitch");
    pitch->addActions({g("pitchUp"), g("pitchDown"), g("pitchUpCent"), g("pitchDownCent"), g("pitchUpOctave"), g("pitchDownOctave"), g("pitchReset")});
    edit->addSeparator();
    edit->addActions({g("eventPanCrop"), g("eventProperties")});

    QMenu* view = menuBar()->addMenu("&View");
    windowMenu_ = view->addMenu("&Window");
    view->addAction(g("focusTimeline"));
    QMenu* layouts = view->addMenu("Window &Layouts");
    layouts->setObjectName("layoutsMenu");
    QObject::connect(layouts, &QMenu::aboutToShow, this, [this, layouts] { fillLayoutsMenu(layouts); });
    fillLayoutsMenu(layouts);
    view->addActions({g("nextWindow"), g("previousWindow")});
    view->addSeparator();
    view->addActions({g("zoomIn"), g("zoomOut"), g("zoomInLarge"), g("zoomOutLarge"), g("zoomFit")});
    view->addSeparator();
    view->addActions({g("zoomFrames"), g("previousTrack"), g("nextTrack")});
    view->addSeparator();
    view->addActions({g("tracksTaller"), g("tracksShorter"), g("tracksDefault"), g("minimizeTrack"), g("maximizeTimeline"), g("dockingArea"), g("dockTop"), g("dockBottom"), g("dockRight"), g("dockLeft"), g("trackList")});
    view->addSeparator();
    view->addActions({g("showWaveforms"), g("showEventButtons"), g("eventHeaders"), g("showEnvelopes"), g("showBusTracks"), g("showFadeLengths")});
    view->addActions({g("showMediaMarkers"), g("showTakeInfo"), g("edgeFrames")});
    view->addSeparator();
    view->addAction(g("rebuildPeaks"));
    a.toolbars = view->addMenu("&Toolbars");

    QMenu* insert = menuBar()->addMenu("&Insert");
    insert->addActions({g("audioTrack"), g("videoTrack"), g("audioBus")});
    insert->addSeparator();
    insert->addAction(g("generatedMedia"));
    insert->addSeparator();
    insert->addActions({g("marker"), g("region")});
    insert->addSeparator();
    insert->addActions({g("cdTrack"), g("cdIndex")});
    insert->addSeparator();
    insert->addAction(g("commandMarker"));
    insert->addSeparator();
    for (int video = 0; video < 2; ++video) {
        QMenu* env = insert->addMenu(video ? "&Video Envelopes" : "Audio &Envelopes");
        env->setObjectName(video ? "videoEnvelopesMenu" : "audioEnvelopesMenu");
        TimelineWidget* tl = timeline_;
        QObject::connect(env, &QMenu::aboutToShow, this, [tl, env, video] { tl->fillEnvelopeMenu(env, video != 0); });
        env->addAction("(select a track)")->setEnabled(false);
    }

    QMenu* toolsMenu = menuBar()->addMenu("&Tools");
    QMenu* video = toolsMenu->addMenu("&Video");
    video->addActions({g("videoOutFx"), g("trackMotion"), g("eventPanCrop"), g("bypassFx")});
    QMenu* audio = toolsMenu->addMenu("&Audio");
    audio->addAction(g("audioEditor"));
    audio->addSeparator();
    audio->addAction(g("masterFx"));
    toolsMenu->addActions({g("trackFx"), g("eventFx")});
    toolsMenu->addSeparator();
    toolsMenu->addActions({g("renderToNewTrack"), g("ramPreview"), g("dimOutput")});
    toolsMenu->addSeparator();
    QMenu* multicam = toolsMenu->addMenu("&Multicamera");
    multicam->addActions({g("createMulticam"), g("multicam")});
    toolsMenu->addSeparator();
    toolsMenu->addActions({g("rescan"), g("rescanAll")});

    QMenu* options = menuBar()->addMenu("&Options");
    options->addAction(g("preferences"));
    options->addSeparator();
    options->addActions({g("quantize"), g("showUnquantized"), g("snapping"), g("snapGrid"), g("snapMarkers"), g("snapAllEvents")});
    options->addSeparator();
    options->addAction(g("autoCrossfade"));
    options->addAction(g("quickFade"));
    options->addAction(g("autoRipple"));
    QMenu* rippleMenu = options->addMenu("Ripple &Mode");
    rippleMenu->addActions({g("rippleTracks"), g("rippleMarkers"), g("rippleAll")});
    options->addAction(g("ignoreGrouping"));
    options->addAction(g("lockEnvelopes"));
    options->addAction(g("slipAllTakes"));
    options->addSeparator();
    options->addAction(g("loop"));

    QMenu* help = menuBar()->addMenu("&Help");
    help->addActions({g("helpContents"), g("whatsThis")});
    help->addSeparator();
    help->addActions({g("shortcuts"), g("about")});
}

void MainWindow::buildToolBars()
{
    Actions& a = *a_;
    QToolBar* main = new QToolBar("Standard", this);
    main->setObjectName("mainToolBar");
    main->setIconSize(QSize(16, 16));
    main->setMovable(false);
    main->setFixedHeight(39);
    for (const char* n : {"new", "open", "save", "saveAs", "properties", "cut", "copy", "paste"}) main->addAction(a.get(n));
    for (const char* n : {"undo", "redo"}) {
        const bool undo = n[0] == 'u';
        QToolButton* b = new QToolButton(main);
        b->setObjectName(undo ? "undoButton" : "redoButton");
        b->setDefaultAction(a.get(n));
        b->setPopupMode(QToolButton::MenuButtonPopup);
        b->setFocusPolicy(Qt::NoFocus);
        QMenu* m = new QMenu(b);
        m->setObjectName(undo ? "undoMenu" : "redoMenu");
        QObject::connect(m, &QMenu::aboutToShow, m, [m, undo] {
            m->clear();
            AppState& app = AppState::get();
            if (!(undo ? app.canUndo() : app.canRedo())) {
                m->addAction(undo ? "Nothing to undo" : "Nothing to redo")->setEnabled(false);
                return;
            }
            auto steps = [undo](int n) {
                AppState& st = AppState::get();
                for (int k = 0; k < n && (undo ? st.canUndo() : st.canRedo()); ++k) undo ? st.undo() : st.redo();
            };
            // Drop-down lists the edit history, next step first; choosing one undoes it and everything above. Latest 40, then Undo All / Redo All.
            const size_t most = 40;
            const std::vector<std::string> names = undo ? app.undoNames(most) : app.redoNames(most);
            for (size_t k = 0; k < names.size(); ++k) {
                QAction* act = m->addAction(QString("%1 %2").arg(undo ? "Undo" : "Redo", QString::fromStdString(names[k])));
                act->setObjectName(QString("%1Step%2").arg(undo ? "undo" : "redo").arg(k + 1));
                act->setToolTip(k == 0 ? QString("%1 this step").arg(undo ? "Undoes" : "Redoes")
                                       : QString("%1 this step and the %2 above it (%3 in all)").arg(undo ? "Undoes" : "Redoes").arg(k).arg(k + 1));
                const int n = (int)k + 1;
                QObject::connect(act, &QAction::triggered, m, [steps, n] { steps(n); });
            }
            m->setToolTipsVisible(true);
            m->addSeparator();
            QObject::connect(m->addAction(undo ? "Undo All" : "Redo All"), &QAction::triggered, m, [steps] { steps(1 << 30); });
        });
        b->setMenu(m);
        main->addWidget(b);
    }
    addToolBar(Qt::TopToolBarArea, main);
    a.toolbars->addAction(main->toggleViewAction());
    QAction* status = a.toolbars->addAction("Status Bar");
    status->setObjectName("showStatusBar");
    status->setCheckable(true);
    status->setChecked(true);
    QObject::connect(status, &QAction::toggled, this, [this](bool on) { statusBar()->setVisible(on); });
}

// Under the timeline: playback rate, transport, edit tools and switches, cursor and selection readouts.
QWidget* MainWindow::buildBottomBar()
{
    Actions& a = *a_;
    QWidget* bar = new QWidget(this);
    bar->setObjectName("timelineBar");
    auto* outer = new QVBoxLayout(bar);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    auto* root = new QHBoxLayout();
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    outer->addLayout(root);
    outer->addWidget(line(bar, kGap));
    outer->addWidget(line(bar, kGap));
    ratePanel_ = ratePanel(bar);
    ratePanel_->setFixedWidth(timeline_->headerWidth());
    root->addWidget(ratePanel_);
    connectRatePanel();
    QWidget* right = new QWidget(bar);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(0);
    rv->addWidget(line(right, kGap));
    rv->addWidget(line(right, kRim));
    QWidget* tools = filled(right, "transportRow", kTool);
    tools->setFixedHeight(31);
    rv->addWidget(tools);
    root->addWidget(right, 1);

    auto* row = new QHBoxLayout(tools);
    row->setContentsMargins(4, 0, 6, 0);
    row->setSpacing(2);
    TransportBar* transport = new TransportBar(playback_.get(), tools);
    transport->onCursorMoved = [this] { timeline_->scrollToCursor(); };
    row->addWidget(transport);
    QToolBar* tb = new QToolBar("Timeline", tools);
    tb->setObjectName("timelineToolBar");
    tb->setIconSize(QSize(16, 16));
    tb->setStyleSheet("QToolBar { padding: 0px; }");
    tb->addSeparator();
    {
        QToolButton* b = new QToolButton(tb);
        b->setObjectName("editToolButton");
        b->setDefaultAction(a.get("toolNormal"));
        b->setPopupMode(QToolButton::MenuButtonPopup);
        b->setFocusPolicy(Qt::NoFocus);
        QMenu* m = new QMenu(b);
        m->addActions({a.get("toolNormal"), a.get("toolShuffle"), a.get("toolSlip"), a.get("toolSlide"), a.get("toolStretch"), a.get("toolSplitTrim")});
        b->setMenu(m);
        tb->addWidget(b);
    }
    for (const char* n : {"toolEnvelope", "toolSelection", "toolZoom"}) tb->addAction(a.get(n));
    tb->addSeparator();
    for (const char* n : {"delete", "trim", "trimStart", "trimEnd", "split", "lockEvent"}) tb->addAction(a.get(n));
    tb->addSeparator();
    for (const char* n : {"marker", "region"}) tb->addAction(a.get(n));
    tb->addSeparator();
    for (const char* n : {"snapping", "autoCrossfade"}) tb->addAction(a.get(n));
    {
        QToolButton* b = new QToolButton(tb);
        b->setObjectName("autoRippleButton");
        b->setDefaultAction(a.get("autoRipple"));
        b->setPopupMode(QToolButton::MenuButtonPopup);
        b->setFocusPolicy(Qt::NoFocus);
        QMenu* m = new QMenu(b);
        m->addActions({a.get("rippleTracks"), a.get("rippleMarkers"), a.get("rippleAll")});
        b->setMenu(m);
        tb->addWidget(b);
    }
    for (const char* n : {"lockEnvelopes", "ignoreGrouping"}) tb->addAction(a.get(n));
    tb->addSeparator();
    tb->addAction(a.get("quantize"));
    tb->addSeparator();
    for (const char* n : {"group", "ungroup", "clearGroup"}) tb->addAction(a.get(n));
    row->addWidget(tb);
    row->addStretch(1);
    auto readout = [&](const char* name, Ico ico, const QString& tip, int width) {
        QLabel* pic = new QLabel(tools);
        pic->setPixmap(toolbarIcon(ico).pixmap(QSize(16, 16)));
        pic->setToolTip(tip);
        QLabel* l = new QLabel(tools);
        l->setObjectName(name);
        l->setToolTip(tip);
        l->setFixedWidth(width);
        row->addWidget(pic);
        row->addSpacing(3);
        row->addWidget(l);
        return l;
    };
    cursorText_ = readout("cursorPosition", Ico::Cursor, "Cursor position (double-click to type a time)", 75);
    selEnd_ = readout("selectionEnd", Ico::SelectionEnd, "End of the time selection (double-click to type it)", 72);
    selLength_ = readout("selectionLength", Ico::SelectionLength, "Length of the time selection (double-click to type it)", 63);
    QLabel* typed[3] = {cursorText_, selEnd_, selLength_};
    for (int k = 0; k < 3; ++k) {
        QLabel* l = typed[k];
        l->installEventFilter(new DoubleClickFilter(l, [this, l, k] {
            AppState& app = AppState::get();
            const Project& p = app.project();
            const bool region = app.loopEnd > app.loopStart;
            const double v = k == 0 ? app.cursor() : k == 1 ? (region ? app.loopEnd : app.cursor()) : (region ? app.loopEnd - app.loopStart : 0.0);
            const QString text = QString::fromStdString(k == 2 ? formatTime(p.settings, v, p.settings.rulerFormat) : rulerTime(p, v));
            QRect r = l->geometry().adjusted(-2, -2, 2, 2);
            r.setWidth(std::max(r.width(), l->fontMetrics().horizontalAdvance(text) + 18));
            editTextInPlace(l->parentWidget(), r, text, [this, k](const QString& t) { typedTime(k, t); });
        }));
    }
    return bar;
}

// Docking area over the timeline, or under it when Preferences > Display "Display timeline at bottom" is cleared.
Qt::DockWidgetArea dockArea()
{
    return vprefs::on("prefs/timelineAtBottom") ? Qt::TopDockWidgetArea : Qt::BottomDockWidgetArea;
}

// Preferences > Display: tab position and "Allow floating windows to dock" (Ctrl does the opposite).
void applyDockOptions(QMainWindow* w, const std::vector<QDockWidget*>& docks)
{
    const QTabWidget::TabPosition pos = vprefs::on("prefs/tabsAtTop") ? QTabWidget::North : QTabWidget::South;
    w->setTabPosition(Qt::TopDockWidgetArea, pos);
    w->setTabPosition(Qt::BottomDockWidgetArea, pos);
    const bool dockable = vprefs::on("prefs/floatingDock");
    for (QDockWidget* d : docks)
        if (d->isFloating()) {
            const bool ctrl = QApplication::keyboardModifiers() & Qt::ControlModifier;
            d->setAllowedAreas(dockable != ctrl ? Qt::AllDockWidgetAreas : Qt::NoDockWidgetArea);
        } else {
            d->setAllowedAreas(Qt::AllDockWidgetAreas);
        }
}

QDockWidget* MainWindow::dock(const QString& title, const QString& name, QWidget* w)
{
    QDockWidget* d = new QDockWidget(title, this);
    d->setObjectName(name);
    d->setWidget(new DockPage(d, w, d));
    // Docked windows have no title bar (the tab names them); floating ones get one back.
    QWidget* noTitle = new QWidget(d);
    d->setTitleBarWidget(noTitle);
    QObject::connect(d, &QDockWidget::topLevelChanged, this, [this, d, noTitle](bool floating) {
        d->setTitleBarWidget(floating ? nullptr : noTitle);
        updateDockTabs();
    });
    QObject::connect(d, &QDockWidget::visibilityChanged, this, [this] { QTimer::singleShot(0, this, [this] { updateDockTabs(); }); });
    QObject::connect(d, &QDockWidget::dockLocationChanged, this, [this] { QTimer::singleShot(0, this, [this] { updateDockTabs(); }); });
    addDockWidget(dockArea(), d);
    docks_.push_back(d);
    QObject::connect(d, &QDockWidget::topLevelChanged, this, [this](bool) { applyDockOptions(this, docks_); });
    return d;
}

void MainWindow::updateDockTabs()
{
    // Tabs of windows sharing a place get a close button and a float/close menu, as in VEGAS.
    for (QTabBar* bar : findChildren<QTabBar*>(QString(), Qt::FindDirectChildrenOnly)) {
        if (bar->property("renoClosable").toBool()) continue;
        bar->setProperty("renoClosable", true);
        bar->setTabsClosable(true);
        bar->setContextMenuPolicy(Qt::CustomContextMenu);
        auto dockAt = [this, bar](int index) -> QDockWidget* {
            if (index < 0) return nullptr;
            const quintptr p = bar->tabData(index).value<quintptr>();
            for (QDockWidget* d : docks_)
                if (p && reinterpret_cast<quintptr>(d) == p) return d;
            for (QDockWidget* d : docks_)
                if (d->windowTitle() == bar->tabText(index)) return d;
            return nullptr;
        };
        QObject::connect(bar, &QTabBar::tabCloseRequested, this, [this, dockAt](int index) {
            if (QDockWidget* d = dockAt(index)) d->close();
            QTimer::singleShot(0, this, [this] { updateDockTabs(); });
        });
        QObject::connect(bar, &QWidget::customContextMenuRequested, this, [this, bar, dockAt](const QPoint& pos) {
            QDockWidget* d = dockAt(bar->tabAt(pos));
            if (!d) return;
            QMenu menu(this);
            menu.addAction("Float", [d] { d->setFloating(true); });
            menu.addAction("Close", [d] { d->close(); });
            menu.exec(bar->mapToGlobal(pos));
        });
    }
    for (QDockWidget* d : docks_) {
        DockPage* page = dynamic_cast<DockPage*>(d->widget());
        if (!page) continue;
        bool alone = !d->isFloating();
        for (QDockWidget* o : tabifiedDockWidgets(d))
            if (o->toggleViewAction()->isChecked()) alone = false;
        page->setTabShown(alone);
    }
}

void MainWindow::showDock(QDockWidget* d)
{
    if (timelineMaximized_) a_->get("maximizeTimeline")->setChecked(false);
    d->show();
    d->raise();
    if (d->widget()) d->widget()->setFocus(Qt::ShortcutFocusReason);
    updateDockTabs();
}

void MainWindow::buildDocks()
{
    setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
    setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
    setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
    setTabPosition(Qt::TopDockWidgetArea, QTabWidget::South);
    setTabPosition(Qt::BottomDockWidgetArea, QTabWidget::South);
    preview_ = new PreviewWidget(playback_.get(), this);
    preview_->onOpenFx = [this](const FxTarget& t, uint64_t slot) { openFx(t, slot); };
    preview_->onProjectProperties = [this] { runProjectPropertiesFull(this); };
    preview_->onExternalMonitor = [this] { if (QAction* a = findChild<QAction*>("externalMonitor")) a->trigger(); };
    preview_->onPreferences = [this] { runPreferences(this); };
    fxWindow_->onEffectShown = [this](uint64_t slotId, const FxTarget& t) { if (preview_) preview_->setOverlayEffect(slotId, t); };
    QDockWidget* preview = dock("Video Preview", "dockPreview", preview_);
    MasterBusWidget* masterBus = new MasterBusWidget(playback_.get(), this);
    masterBus->onOpenFx = [this](const FxTarget& t, uint64_t slot) { openFx(t, slot); };
    QDockWidget* master = dock("Master Bus", "dockMaster", masterPage(this, masterBus, [this] { if (mixerDock_) showDock(mixerDock_); }, playback_.get()));

    mediaBin_ = new MediaBin(this);
    QDockWidget* media = dock("Project Media", "dockMedia", mediaBin_);
    const struct { FxBrowser::Kind kind; const char* title; const char* name; } lists[] = {
        {FxBrowser::VideoFx, "Video FX", "dockVideoFx"}, {FxBrowser::AudioFx, "Audio FX", "dockAudioFx"},
        {FxBrowser::Generators, "Media Generators", "dockGenerators"}, {FxBrowser::Transitions, "Transitions", "dockTransitions"}};
    std::map<std::string, QDockWidget*> byName;
    for (const auto& l : lists) {
        FxBrowser* b = new FxBrowser(l.kind, this);
        b->onActivated = [this](FxBrowser::Kind k, const std::string& data) { browserActivated(k, data); };
        byName[l.name] = dock(l.title, l.name, b);
    }
    QDockWidget* notes = dock("Project Notes", "dockNotes", makeNotesPanel(this));
    MixerWindow* mixer = new MixerWindow(playback_.get(), this);
    mixer->onOpenFx = [this](const FxTarget& t, uint64_t slot) { openFx(t, slot); };
    mixer->onDim = [this](bool on) {
        if (QAction* a = findChild<QAction*>("dimOutput"))
            if (a->isChecked() != on) a->trigger();
    };
    mixer->onAudioProperties = [this] { runProjectPropertiesAudio(this); };
    mixerDock_ = dock("Mixing Console", "dockMixer", mixer);
    QDockWidget* scopes = dock("Video Scopes", "dockScopes", new VideoScopes(playback_.get(), this));
    QDockWidget* explorer = dock("Explorer", "dockExplorer", new MediaExplorer(this));
    QDockWidget* details = dock("Edit Details", "dockDetails", new EditDetails(this));

    const struct { QDockWidget* dock; const char* name; const char* key; } entries[] = {
        {master, "showMasterBus", "Alt+3"}, {preview, "showPreview", "Alt+4"}, {media, "showMedia", "Alt+5"}, {notes, "showNotes", "Alt+6"},
        {byName["dockTransitions"], "showTransitions", "Alt+7"}, {byName["dockVideoFx"], "showVideoFx", "Alt+8"},
        {byName["dockGenerators"], "showGenerators", "Alt+9"}, {mixerDock_, "showMixer", "Ctrl+Alt+5"}, {scopes, "showScopes", "Ctrl+Alt+2"},
        {explorer, "showExplorer", "Alt+1"}, {details, "showDetails", "Ctrl+Alt+4"},
        {byName["dockAudioFx"], "showAudioFx", ""}};
    for (const auto& e : entries) {
        QAction* toggle = e.dock->toggleViewAction();
        if (*e.key) toggle->setText(QString("%1\t%2").arg(e.dock->windowTitle(), e.key));
        windowMenu_->addAction(toggle);
        QDockWidget* d = e.dock;
        if (*e.key) {
            QAction* show = make(this, e.name, QString("Show %1").arg(d->windowTitle()), keys({e.key}), [this, d] { showDock(d); });
            a_->all[e.name] = show;
            addAction(show);
        }
    }
    updateDockTabs();
    applyDockOptions(this, docks_);
}

void MainWindow::showEvent(QShowEvent* e)
{
    QMainWindow::showEvent(e);
    if (firstShow_) {
        firstShow_ = false;
        if (!startLayoutRestored_) {
            applyLayout(DefaultLayout);
            const QSize was = size();
            QTimer::singleShot(300, this, [this, was] {
                if (size() != was && currentLayout_ == builtinLayoutName(DefaultLayout)) applyLayout(DefaultLayout);
            });
        }
    }
}

void MainWindow::compositingMenu(QMenu* menu, uint64_t trackId)
{
    const Track* t = nullptr;
    for (const Track& o : AppState::get().project().tracks)
        if (o.id == trackId) t = &o;
    if (!t) return;
    QMenu* modes = menu->addMenu("Compositing Mode");
    const std::string now = t->composite;
    for (const CompositeMode& m : compositeModes()) {
        QAction* x = modes->addAction(m.label);
        x->setCheckable(true);
        x->setChecked(now == m.id);
        const std::string id = m.id;
        QObject::connect(x, &QAction::triggered, menu, [trackId, id] {
            AppState::get().edit("Compositing Mode", [&](Project& p) { if (Track* tr = p.findTrack(trackId)) tr->composite = id; });
        });
    }
    // Compositing parent/child (track header buttons): a child is composited with its parent (nearest track above at a lower level) before the group is laid over the tracks below. A parent has Parent Composite Mode and Parent Motion.
    std::vector<const Track*> vt;
    size_t at = 0;
    for (const Track& o : AppState::get().project().tracks)
        if (o.type == TrackType::Video) {
            if (o.id == trackId) at = vt.size();
            vt.push_back(&o);
        }
    const int level = std::max(0, t->compositeLevel);
    const int above = at > 0 ? std::max(0, vt[at - 1]->compositeLevel) : -1;
    const bool hasChildren = at + 1 < vt.size() && std::max(0, vt[at + 1]->compositeLevel) > level;
    auto shiftLevel = [trackId](int by, const char* name) {
        AppState::get().edit(name, [&](Project& p) {
            int base = -1;
            for (Track& o : p.tracks) {
                if (o.type != TrackType::Video) continue;
                if (o.id == trackId) { base = std::max(0, o.compositeLevel); o.compositeLevel = std::max(0, base + by); continue; }
                if (base < 0) continue;
                if (std::max(0, o.compositeLevel) <= base) { base = -1; continue; }
                o.compositeLevel = std::max(0, o.compositeLevel + by);
            }
        });
    };
    QAction* parent = menu->addAction("Make Compositing Parent");
    parent->setEnabled(level > 0);
    QObject::connect(parent, &QAction::triggered, menu, [shiftLevel] { shiftLevel(-1, "Make Compositing Parent"); });
    QAction* child = menu->addAction("Make Compositing Child");
    child->setEnabled(at > 0 && level <= above);
    QObject::connect(child, &QAction::triggered, menu, [shiftLevel] { shiftLevel(1, "Make Compositing Child"); });
    if (hasChildren) {
        QMenu* pmodes = menu->addMenu("Parent Composite Mode");
        const std::string pnow = t->parentComposite;
        for (const CompositeMode& m : compositeModes()) {
            QAction* x = pmodes->addAction(m.label);
            x->setCheckable(true);
            x->setChecked(pnow == m.id);
            const std::string id = m.id;
            QObject::connect(x, &QAction::triggered, menu, [trackId, id] {
                AppState::get().edit("Parent Composite Mode", [&](Project& p) { if (Track* tr = p.findTrack(trackId)) tr->parentComposite = id; });
            });
        }
        QObject::connect(menu->addAction("Parent Motion..."), &QAction::triggered, menu, [this, trackId] { openParentMotion(this, playback_.get(), trackId); });
    }
}

void MainWindow::showDiskSpace()
{
    const std::string path = AppState::get().path();
    const QString dir = path.empty() ? QDir::homePath() : QFileInfo(QString::fromStdString(path)).absolutePath();
    const QStorageInfo disk(dir);
    if (!disk.isValid() || disk.bytesAvailable() < 0) {
        diskText_->clear();
        return;
    }
    const double gb = (double)disk.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
    diskText_->setText(gb >= 100 ? QString("Free Disk Space: %1 GB").arg(gb, 0, 'f', 0) : QString("Free Disk Space: %1 GB").arg(gb, 0, 'f', 1));
    diskText_->setToolTip(QString("Space left on the disk of %1").arg(QDir::toNativeSeparators(dir)));
}

static void splitPair(const std::string& data, std::string& id, std::string& label)
{
    const size_t nl = data.find('\n');
    id = data.substr(0, nl);
    label = nl == std::string::npos ? id : data.substr(nl + 1);
}

void MainWindow::browserActivated(FxBrowser::Kind kind, const std::string& data)
{
    AppState& app = AppState::get();
    std::string id, label;
    splitPair(data, id, label);
    if (kind == FxBrowser::VideoFx) {
        FxTarget t;
        for (uint64_t ev : app.selection()) {
            FxTarget c;
            c.kind = FxTarget::Event;
            c.id = ev;
            if (app.targetIsVideo(c)) { t = c; break; }
        }
        if (t.kind == FxTarget::None && app.currentTrack) {
            FxTarget c;
            c.kind = FxTarget::Track;
            c.id = app.currentTrack;
            if (app.targetIsVideo(c)) t = c;
        }
        if (t.kind == FxTarget::None) { app.status("Select a video event or a video track first, or drag the effect onto one."); return; }
        const uint64_t slot = isFxPackageData(data) ? addFxData(t, data) : app.addVideoFx(t, id, label);
        openFx(t, slot);
    } else if (kind == FxBrowser::AudioFx) {
        Json j;
        if (!Json::parse(data, j)) return;
        if (isFxPackageData(data)) {
            FxTarget t;
            t.kind = FxTarget::AudioOut;
            for (const Track& tr : app.project().tracks)
                if (tr.id == app.currentTrack && tr.type == TrackType::Audio) { t.kind = FxTarget::Track; t.id = tr.id; }
            openFx(t, addFxData(t, data));
            return;
        }
        FxTarget t;
        if (app.currentTrack) {
            FxTarget c;
            c.kind = FxTarget::Track;
            c.id = app.currentTrack;
            if (!app.targetIsVideo(c)) t = c;
        }
        if (t.kind == FxTarget::None) t.kind = FxTarget::AudioOut;
        const uint64_t slot = app.addAudioFx(t, AudioPluginDesc::fromJson(j));
        openFx(t, slot);
    } else if (kind == FxBrowser::Generators) {
        const uint64_t media = isFxPackageData(data) ? makeGeneratedMediaFromData(data) : app.addGeneratedMedia(id, label);
        if (media) app.setSelection([&] { std::set<uint64_t> s; for (uint64_t e : app.addMediaToTimeline(media, app.cursor(), app.currentTrack)) s.insert(e); return s; }());
    } else if (kind == FxBrowser::Transitions) {
        if (app.selection().empty()) { app.status("Select the later of two overlapping video events first, or drag the transition onto the overlap."); return; }
        for (uint64_t ev : app.selection()) {
            FxTarget c;
            c.kind = FxTarget::Event;
            c.id = ev;
            if (!app.targetIsVideo(c)) continue;
            if (isFxPackageData(data)) {
                c.kind = FxTarget::Transition;
                addFxData(c, data);
            } else {
                app.setTransition(ev, id, label);
            }
        }
    }
}

void MainWindow::openFx(const FxTarget& target, uint64_t slotId)
{
    if (!slotId && pluginChooserFirst(this, target, &slotId)) return;
    fxWindow_->showTarget(target, slotId);
    fxWindow_->show();
    fxWindow_->raise();
}

void MainWindow::showShortcuts()
{
    if (!shortcutsDialog_) shortcutsDialog_ = makeShortcutsDialog(this);
    shortcutsDialog_->show();
    shortcutsDialog_->raise();
}

void MainWindow::chooseTakeDialog(bool remove)
{
    AppState& app = AppState::get();
    const Event* e = nullptr;
    for (const Track& t : app.project().tracks)
        for (const Event& o : t.events)
            if (!e && app.selection().count(o.id)) e = &o;
    if (!e) { app.status("Select an event first."); return; }
    const uint64_t id = e->id;
    const std::vector<Take> all = AppState::takesOf(*e);
    QDialog dlg(this);
    dlg.setObjectName(remove ? "deleteTakesDialog" : "chooseTakeDialog");
    dlg.setWindowTitle(remove ? "Delete Takes" : "Choose Active Take");
    auto* root = new QVBoxLayout(&dlg);
    root->addWidget(new QLabel(remove ? "Tick the takes to delete:" : "The take the event shows:", &dlg));
    auto* list = new QListWidget(&dlg);
    list->setObjectName("takeList");
    for (size_t k = 0; k < all.size(); ++k) {
        const MediaItem* m = app.project().findMedia(all[k].mediaId);
        QListWidgetItem* it = new QListWidgetItem(QString("%1  %2%3").arg(k + 1).arg(QString::fromStdString(all[k].name),
                                                                        m && m->name != all[k].name ? QString("  (%1)").arg(QString::fromStdString(m->name)) : QString()), list);
        if (remove) {
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setCheckState(Qt::Unchecked);
        }
    }
    list->setCurrentRow(std::min(e->activeTake, (int)all.size() - 1));
    root->addWidget(list, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    root->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dlg, [&dlg, remove] { if (!remove) dlg.accept(); });
    if (dlg.exec() != QDialog::Accepted) return;
    if (remove) {
        std::set<int> gone;
        for (int k = 0; k < list->count(); ++k)
            if (list->item(k)->checkState() == Qt::Checked) gone.insert(k);
        if (!gone.empty()) app.deleteTakes(id, gone);
    } else if (list->currentRow() >= 0) {
        app.chooseTake(std::set<uint64_t>{id}, list->currentRow());
    }
}

void MainWindow::insertCommandDialog()
{
    // Command markers: VEGAS's Command Properties
    CommandProps c;
    c.position = AppState::get().cursor();
    if (!commandPropertiesDialog(this, c)) return;
    timeline_->insertCommandMarker(c.command, isCaptionCommand(c.command) ? captionMarkupOf(c.parameter) : c.parameter, c.comment, c.position);
}

bool MainWindow::openInAudioEditor(uint64_t eventId)
{
    AppState& app = AppState::get();
    const Event* e = app.project().findEvent(eventId);
    const MediaItem* m = e ? app.project().findMedia(e->mediaId) : nullptr;
    if (!m || m->generated || m->path.empty() || !QFileInfo::exists(QString::fromStdString(m->path))) {
        app.status("That event's media is not a file that can be opened in an audio editor.");
        return false;
    }
    if (app.prefs.audioEditor.empty()) {
        const QString exe = QFileDialog::getOpenFileName(this, "Choose the Audio Editor", QString(), "Programs (*.exe);;All files (*)");
        if (exe.isEmpty()) return false;
        app.prefs.audioEditor = exe.toStdString();
        app.applyPreferences();
    }
    const QString editor = QString::fromStdString(app.prefs.audioEditor);
    if (!QProcess::startDetached(editor, QStringList() << QDir::toNativeSeparators(QString::fromStdString(m->path)))) {
        app.status("Could not start the audio editor " + app.prefs.audioEditor + " (Options > Preferences > Editing sets it).");
        return false;
    }
    app.status("Opened " + m->name + " in " + QFileInfo(editor).fileName().toStdString() + ".");
    return true;
}

void MainWindow::showHelp()
{
    QDialog* dlg = findChild<QDialog*>("helpWindow");
    if (!dlg) {
        dlg = new QDialog(this);
        dlg->setObjectName("helpWindow");
        dlg->setWindowTitle("Reno Help");
        dlg->resize(820, 640);
        auto* root = new QVBoxLayout(dlg);
        auto* tabs = new QTabWidget(dlg);
        tabs->setObjectName("helpTabs");
        root->addWidget(tabs, 1);
        auto* readme = new QTextBrowser(tabs);
        readme->setObjectName("helpReadme");
        QString text;
        const QString exeDir = QCoreApplication::applicationDirPath();
        for (const QString& f : {exeDir + "/README.md", exeDir + "/../Resources/README.md", exeDir + "/../README.md", exeDir + "/../../README.md", QDir::currentPath() + "/README.md"}) {
            QFile file(f);
            if (file.open(QIODevice::ReadOnly)) {
                text = QString::fromUtf8(file.readAll());
                break;
            }
        }
        if (text.isEmpty()) text = "README.md was not found next to reno.exe or the folder above it.";
        // the README starts with an HTML <img> of the logo for GitHub; Qt's Markdown skips raw HTML, so make it a
        // Markdown image and draw the logo instead of looking for the PNG
        text.replace(QRegularExpression("^<p[^\\n]*<img [^\\n]*src=\"([^\"]+)\"[^\\n]*\\n"), "![Reno](\\1)\n");
        readme->setMarkdown(text);
        readme->document()->addResource(QTextDocument::ImageResource, QUrl("resources/reno_256.png"), renoLogo(96));
        readme->document()->markContentsDirty(0, readme->document()->characterCount());
        tabs->addTab(readme, "Read Me");
        auto* keysView = new QTextBrowser(tabs);
        keysView->setObjectName("helpShortcuts");
        QString html = "<h3>Keyboard shortcuts</h3><p>VEGAS Pro's default keys. Grey lines are things Reno cannot do yet. "
                       "\"Timeline\" keys work while the timeline has the keyboard (click it, or Alt+0).</p><table cellspacing=0 cellpadding=2>";
        std::string group;
        for (const ShortcutInfo& k : shortcutList()) {
            if (group != k.menu) {
                group = k.menu;
                html += QString("<tr><td colspan=3><br><b>%1</b></td></tr>").arg(QString::fromUtf8(k.menu).toHtmlEscaped());
            }
            const bool na = k.kind == ShortcutInfo::NotAvailable;
            const QString colour = na ? " style='color:#808080'" : "";
            QString note = QString::fromUtf8(k.note);
            if (k.kind == ShortcutInfo::Timeline) note = note.isEmpty() ? QString("timeline") : "timeline; " + note;
            if (k.kind == ShortcutInfo::Panel) {
                const QString where = QString("in the %1 window").arg(QString::fromUtf8(k.menu));
                note = note.isEmpty() ? where : where + "; " + note;
            }
            if (na) note = "not available: " + note;
            html += QString("<tr%1><td>%2</td><td><b>%3</b></td><td>%4</td></tr>")
                        .arg(colour, QString::fromUtf8(k.command).toHtmlEscaped(), QString::fromUtf8(k.keys).toHtmlEscaped(), note.toHtmlEscaped());
        }
        html += "</table><p>Shift+F1 (What's This): click a command or a window to be told what it is.</p>";
        keysView->setHtml(html);
        tabs->addTab(keysView, "Keyboard Shortcuts");
        auto* close = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
        root->addWidget(close);
        QObject::connect(close, &QDialogButtonBox::rejected, dlg, &QDialog::hide);
    }
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::cycleFocus(int dir)
{
    std::vector<QWidget*> stops;
    stops.push_back(timeline_);
    for (QDockWidget* d : docks_)
        if (d->isVisible() && d->widget() && rect().intersects(QRect(d->mapTo(this, QPoint(0, 0)), d->size()))) stops.push_back(d->widget());
    int at = 0;
    QWidget* f = QApplication::focusWidget();
    for (size_t k = 0; k < stops.size(); ++k)
        if (f && (f == stops[k] || stops[k]->isAncestorOf(f))) at = (int)k;
    const int n = (int)stops.size();
    QWidget* next = stops[(size_t)(((at + dir) % n + n) % n)];
    if (QDockWidget* d = qobject_cast<QDockWidget*>(next->parentWidget())) d->raise();
    next->setFocus(Qt::TabFocusReason);
}

void MainWindow::goToDialog(bool selectionEnd)
{
    AppState& app = AppState::get();
    const Project& p = app.project();
    const double now = selectionEnd && app.loopEnd > app.loopStart ? app.loopEnd : app.cursor();
    QWidget* over = timeline_;
    QRect r = timeline_->timeDisplayRect().adjusted(4, 14, -8, -2);
    if (selectionEnd && selEnd_ && selEnd_->isVisible()) {
        over = selEnd_->parentWidget();
        r = selEnd_->geometry().adjusted(-2, -2, 2, 2);
        r.setWidth(std::max(r.width(), selEnd_->fontMetrics().horizontalAdvance(QString::fromStdString(rulerTime(p, now))) + 18));
    }
    editTextInPlace(over, r, QString::fromStdString(rulerTime(p, now)), [this, selectionEnd](const QString& text) {
        typedTime(selectionEnd ? 1 : 0, text);
    });
}

// 'what': 0 moves the cursor, 1 sets the selection end, 2 its length.
void MainWindow::typedTime(int what, const QString& text)
{
    AppState& app = AppState::get();
    const Project& p = app.project();
    double t = 0;
    if (what == 2) {
        // a length: counted from zero, not from the ruler's start
        double shown = 0;
        if (!parseTimeText(p.settings, text.toStdString(), p.settings.rulerFormat, &shown) || shown < 0) t = -1;
        else t = shown;
    } else {
        t = parseTime(p, text);
    }
    if (t < 0) { app.status("That time could not be read: type it as the ruler shows times (" + rulerTime(p, app.cursor()) + "), or as a number of seconds."); return; }
    if (what == 0) {
        playback_->jumpTo(t);
        timeline_->scrollToCursor();
        return;
    }
    const double a = app.loopEnd > app.loopStart ? app.loopStart : app.cursor();
    if (what == 1) app.setLoop(std::min(a, t), std::max(a, t));
    else app.setLoop(a, a + t);
}

void MainWindow::pasteRepeat()
{
    AppState& app = AppState::get();
    if (!app.hasClipboard()) { app.status("Copy something first: Paste Repeat pastes what was copied."); return; }
    PasteRepeatChoice choice;
    if (!askPasteRepeat(this, choice)) return;
    const int times = choice.times;
    const std::set<uint64_t> before = app.selection();
    app.pasteAtCursor();
    const std::set<uint64_t> made = app.selection();
    if (made.empty() || made == before || times < 2) return;
    double first = 1e300, last = -1e300;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (made.count(e.id)) {
                first = std::min(first, e.start);
                last = std::max(last, e.end());
            }
    const double span = choice.even ? choice.every : last - first;
    std::set<uint64_t> all = made;
    app.edit("Paste Repeat", [&](Project& p) {
        for (int k = 1; k < times; ++k) {
            std::map<uint64_t, uint64_t> groups;
            for (Track& t : p.tracks) {
                std::vector<Event> copies;
                for (const Event& e : t.events) {
                    if (!made.count(e.id)) continue;
                    Event c = e;
                    c.id = p.newId();
                    c.start = e.start + span * k;
                    for (auto& s : c.fx) s.id = p.newId();
                    if (!c.transition.kind.empty()) c.transition.id = p.newId();
                    if (c.group) {
                        auto it = groups.find(c.group);
                        if (it == groups.end()) it = groups.emplace(c.group, p.newId()).first;
                        c.group = it->second;
                    }
                    copies.push_back(c);
                    all.insert(c.id);
                }
                for (const Event& c : copies) t.events.push_back(c);
                t.sortEvents();
            }
        }
    });
    app.setSelection(all);
}

// 'reset' puts the pitch back to none.
void MainWindow::changePitch(double semitones, bool reset)
{
    AppState& app = AppState::get();
    std::set<uint64_t> ids;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Audio)
            for (const Event& e : t.events)
                if (app.selection().count(e.id)) ids.insert(e.id);
    if (ids.empty()) { app.status("Select a sound event first: the pitch keys change the pitch of the selected audio events."); return; }
    double shown = 0;
    int tape = 0;       // events whose stretch method is None: pitch only follows the rate, as in VEGAS
    app.edit(reset ? "Reset Pitch" : "Pitch", [&](Project& p) {
        for (uint64_t id : ids)
            if (Event* e = p.findEvent(id)) {
                e->pitch = reset ? 0.0 : std::round((e->pitch + semitones) * 100.0) / 100.0;
                shown = e->pitch;
                if (e->stretchMethod == "none") ++tape;
            }
    });
    if (tape && !reset)
        app.status(strfmt("Pitch: %+.2f semitones - but %d of the events use the stretch method None, whose pitch only follows the playback rate: "
                          "choose élastique or Classic in the event's Properties (or change the rate) to hear it", shown, tape));
    else app.status(strfmt("Pitch: %+.2f semitones", shown));
}

void MainWindow::stepTrack(int dir)
{
    AppState& app = AppState::get();
    const std::vector<Track>& tracks = app.project().tracks;
    if (tracks.empty()) return;
    int at = -1;
    for (size_t k = 0; k < tracks.size(); ++k)
        if (tracks[k].id == app.currentTrack) at = (int)k;
    const int next = at < 0 ? 0 : std::max(0, std::min((int)tracks.size() - 1, at + dir));
    app.currentTrack = tracks[(size_t)next].id;
    app.notify(kSelection);
}

void MainWindow::muteSolo(bool solo, bool exclusive)
{
    AppState& app = AppState::get();
    const uint64_t id = app.currentTrack;
    if (!id) return;
    const std::set<uint64_t> ids = timeline_->selectedTracks();
    app.edit(solo ? "Solo Track" : "Mute Track", [&](Project& p) {
        Track* t = p.findTrack(id);
        if (!t) return;
        const bool on = exclusive ? true : !(solo ? t->solo : t->mute);
        for (Track& o : p.tracks) {
            bool& flag = solo ? o.solo : o.mute;
            if (o.id == id || ids.count(o.id)) flag = on;
            else if (exclusive) flag = false;
        }
    });
}

// J, K, L (VEGAS shuttle): L plays forward, each press faster; J the same backwards; K slows to a quarter speed, then J / L change the rate by a quarter.
void MainWindow::shuttle(int dir)
{
    // K: quarter speed (forwards when stopped); J / L then step the rate by a quarter, through 0 (pause) to backwards. Stopping another way leaves slow mode.
    Playback* pb = playback_.get();
    const bool inSlow = slowMode_ && (pb->playing() ? std::fabs(pb->rate() - slowRate_) < 1e-9 : std::fabs(slowRate_) < 1e-9);
    if (dir == 0) {
        slowMode_ = true;
        slowRate_ = (pb->playing() && pb->rate() < 0) ? -kJogRate : kJogRate;
        pb->playAt(slowRate_);
    } else if (inSlow) {
        slowRate_ = std::round((slowRate_ + dir * kJogRate) / kJogRate) * kJogRate;
        slowRate_ = std::max(-(double)Playback::kMaxRate, std::min((double)Playback::kMaxRate, slowRate_));
        if (std::fabs(slowRate_) < 1e-9) pb->pause();
        else pb->playAt(slowRate_);
    } else {
        slowMode_ = false;
        // Preferences > Editing "JKL / shuttle speed": half (Slow), as is (Medium) or doubled (Fast)
        const QString speed = vprefs::text("prefs/jklSpeed");
        const double scale = speed == "slow" ? 0.5 : speed == "fast" ? 2.0 : 1.0;
        const double next = Playback::nextShuttleRate(pb->rate() / scale, dir) * scale;
        pb->playAt(std::max(-(double)Playback::kMaxRate, std::min((double)Playback::kMaxRate, next)));
    }
    jogging_ = false;
    timeline_->scrollToCursor();
}

class ShuttleKeys : public QObject {
public:
    ShuttleKeys(QObject* parent, bool* held, std::function<void()> jogReleased)
        : QObject(parent), held_(held), jogReleased_(std::move(jogReleased)) {}

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        const QEvent::Type t = e->type();
        if (t != QEvent::KeyPress && t != QEvent::KeyRelease && t != QEvent::ShortcutOverride) return false;
        const QKeyEvent* k = static_cast<QKeyEvent*>(e);
        if (k->isAutoRepeat() || (k->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier | Qt::ShiftModifier))) return false;
        if (qobject_cast<QLineEdit*>(o) || qobject_cast<QTextEdit*>(o) || qobject_cast<QPlainTextEdit*>(o) || qobject_cast<QAbstractSpinBox*>(o)) return false;
        (void)held_;
        if ((k->key() == Qt::Key_J || k->key() == Qt::Key_L) && t == QEvent::KeyRelease && jogReleased_) jogReleased_();
        return false;
    }

private:
    bool* held_;
    std::function<void()> jogReleased_;
};

void MainWindow::connectRatePanel()
{
    Playback* pb = playback_.get();
    RateSlider* slider = dynamic_cast<RateSlider*>(ratePanel_->findChild<QSlider*>("rateSlider"));
    RateLabel* label = dynamic_cast<RateLabel*>(ratePanel_->findChild<QLabel*>("rateLabel"));
    if (!slider || !label) return;
    QObject::connect(slider, &QSlider::sliderMoved, this, [pb](int v) {
        const double r = v / 100.0;
        if (std::fabs(r) < 0.01) pb->pause();
        else pb->playAt(r);
    });
    QObject::connect(slider, &QSlider::sliderReleased, this, [pb, slider] {
        slider->setValue(0);
        pb->pause();
    });
    slider->onDoubleClick = [pb] {
        pb->setNormalRate(1.0);
        AppState::get().status("Play plays at the normal rate (1.00) again.");
    };
    label->onDoubleClick = [this, pb] {
        bool ok = false;
        const double r = QInputDialog::getDouble(this, "Playback Rate", "Rate Play plays at (negative: backwards):", pb->normalRate(),
                                                 -Playback::kMaxRate, Playback::kMaxRate, 2, &ok);
        if (ok) pb->setNormalRate(r);
    };
    if (!shuttleFilter_) {
        shuttleFilter_ = new ShuttleKeys(this, &shuttleKeyHeld_, [this] {
            if (jogging_) {
                jogging_ = false;
                playback_->pause();
            }
        });
        qApp->installEventFilter(shuttleFilter_);
    }
    updateRate();
}

void MainWindow::buildRamPreview()
{
    AppState& app = AppState::get();
    Playback* pb = playback_.get();
    if (!(app.loopEnd > app.loopStart)) {
        app.status("Dynamic RAM Preview: make a time selection first.");
        return;
    }
    pb->pause();
    const double a = app.loopStart, b = app.loopEnd;
    QProgressDialog progress("Building dynamic RAM preview...", "Cancel", 0, 1000, this);
    progress.setWindowTitle("Dynamic RAM Preview");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    const int64_t made = pb->ramPreview(a, b, [&](double f) {
        progress.setValue((int)(f * 1000));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    });
    progress.setValue(1000);
    const Project& p = app.project();
    const int64_t want = p.frameAt(b - 1e-6) - p.frameAt(a) + 1;
    if (progress.wasCanceled()) {
        app.status(QString("Dynamic RAM Preview stopped: %1 of %2 frames are in memory.").arg(made).arg(want).toStdString());
        return;
    }
    app.status(made < want ? QString("Dynamic RAM Preview: the first %1 of %2 frames fit in memory.").arg(made).arg(want).toStdString()
                           : QString("Dynamic RAM Preview: %1 frames in memory.").arg(made).toStdString());
    app.loopOn = true;
    syncOptions();
    app.setCursor(a);
    pb->play();
}

// Render to New Track (Ctrl+M): renders the time selection or project to an .mp4 beside the project and puts it on new tracks at the top.
void MainWindow::renderToNewTrack()
{
    AppState& app = AppState::get();
    playback_->pause();
    const Project& now = app.project();
    const bool sel = app.loopEnd > app.loopStart;
    const double from = sel ? app.loopStart : 0.0, to = sel ? app.loopEnd : now.length();
    if (!(to > from)) {
        app.status("Render to New Track: there is nothing to render.");
        return;
    }
    QString dir = app.path().empty() ? QDir::homePath() : QFileInfo(QString::fromStdString(app.path())).absolutePath();
    const QString stem = app.path().empty() ? QString("Untitled") : QFileInfo(QString::fromStdString(app.path())).completeBaseName();
    QString out;
    for (int n = 1; n < 1000; ++n) {
        out = QDir(dir).filePath(QString("%1 Rendered %2.mp4").arg(stem).arg(n, 2, 10, QChar('0')));
        if (!QFileInfo::exists(out)) break;
    }
    if (!runRenderToNewTrackDialog(this, from, to, out, &out)) return;
    EncodeSettings es;
    es.path = out.toStdString();
    std::string ierr;
    const uint64_t media = app.importMedia(es.path, &ierr);
    if (!media) {
        QMessageBox::warning(this, "Render to New Track", QString("The rendered file could not be read back: %1").arg(QString::fromStdString(ierr)));
        return;
    }
    const MediaItem* m = app.project().findMedia(media);
    if (!m) return;
    const MediaItem mi = *m;
    app.edit("Render to New Track", [&](Project& p) {
        const uint64_t group = (mi.info.hasVideo && mi.info.hasAudio) ? p.newId() : 0;
        auto addOn = [&](TrackType type, size_t at) {
            Track t;
            t.id = p.newId();
            t.type = type;
            if (type == TrackType::Audio) t.panType = "add";
            t.name = "Rendered";
            Event e;
            e.id = p.newId();
            e.mediaId = mi.id;
            e.start = from;
            e.length = mi.info.duration > 0 ? std::min(mi.info.duration, to - from) : to - from;
            e.group = group;
            e.name = mi.name;
            setDefaultFadeCurves(e, type);
            t.events.push_back(e);
            p.tracks.insert(p.tracks.begin() + (std::ptrdiff_t)std::min(at, p.tracks.size()), t);
        };
        if (mi.info.hasVideo) addOn(TrackType::Video, 0);
        if (mi.info.hasAudio) {
            size_t at = p.tracks.size();
            for (size_t k = 0; k < p.tracks.size(); ++k)
                if (p.tracks[k].type == TrackType::Audio) { at = k; break; }
            addOn(TrackType::Audio, at);
        }
    });
    app.status("Rendered to " + es.path);
}

void MainWindow::updateRate()
{
    if (!ratePanel_) return;
    Playback* pb = playback_.get();
    RateSlider* slider = dynamic_cast<RateSlider*>(ratePanel_->findChild<QSlider*>("rateSlider"));
    QLabel* label = ratePanel_->findChild<QLabel*>("rateLabel");
    if (label) label->setText(QString("Rate: %1").arg(pb->rate(), 0, 'f', 2));
    if (slider && std::fabs(slider->normalRate - pb->normalRate()) > 1e-9) {
        slider->normalRate = pb->normalRate();
        slider->update();
    }
}

// set while only the cursor moved (hundreds of times a second while playing)
static bool gOptionsSyncIfChanged = false;

void MainWindow::syncOptions()
{
    const TimelineOptions& o = timeline_->options();
    const AppState& app = AppState::get();
    {
        std::string sig;
        sig.reserve(64);
        auto flag = [&sig](bool v) { sig.push_back(v ? '1' : '0'); };
        sig.push_back((char)('a' + (int)o.tool));
        sig.push_back((char)('a' + (int)o.ripple));
        for (bool v : {o.lockEnvelopes, o.showEnvelopes, o.showBusTracks, o.showFadeLengths, o.quantize, o.showUnquantized, app.snapping, o.snapToGrid,
                       o.snapToMarkers, o.autoRipple, o.ignoreGrouping, o.showWaveforms, o.showEventButtons, o.eventHeaders, app.loopOn, app.bypassAllFx,
                       o.snapToAllEvents, o.showTakeInfo, o.showMediaMarkers, o.edgeFrames, o.slipAllTakes, o.multicam, app.autoCrossfades(),
                       app.prefs.quickFadeEdits})
            flag(v);
        const QByteArray was = property("optionsShown").toByteArray();
        const QByteArray now = QByteArray::fromStdString(sig);
        if (gOptionsSyncIfChanged && was == now) return;
        setProperty("optionsShown", now);
    }
    auto set = [this](const char* n, bool on) {
        if (QAction* x = a_->get(n)) {
            QSignalBlocker b(x);
            x->setChecked(on);
        }
    };
    set("toolNormal", o.tool == EditTool::Normal);
    set("toolSelection", o.tool == EditTool::Selection);
    set("toolZoom", o.tool == EditTool::Zoom);
    set("toolEnvelope", o.tool == EditTool::Envelope);
    set("toolShuffle", o.tool == EditTool::Shuffle);
    set("toolSlip", o.tool == EditTool::Slip);
    set("toolSlide", o.tool == EditTool::Slide);
    set("toolStretch", o.tool == EditTool::Stretch);
    set("toolSplitTrim", o.tool == EditTool::SplitTrim);
    if (QToolButton* b = findChild<QToolButton*>("editToolButton")) {
        const char* n = o.tool == EditTool::Shuffle ? "toolShuffle" : o.tool == EditTool::Slip ? "toolSlip" : o.tool == EditTool::Slide ? "toolSlide"
                      : o.tool == EditTool::Stretch ? "toolStretch" : o.tool == EditTool::SplitTrim ? "toolSplitTrim" : nullptr;
        if (n) b->setDefaultAction(a_->get(n));
        else if (o.tool == EditTool::Normal) b->setDefaultAction(a_->get("toolNormal"));
        else if (QAction* d = b->defaultAction()) {
            QSignalBlocker sb(d);
            d->setChecked(false);
        }
    }
    set("lockEnvelopes", o.lockEnvelopes);
    set("showEnvelopes", o.showEnvelopes);
    set("showBusTracks", o.showBusTracks);
    set("showFadeLengths", o.showFadeLengths);
    set("quantize", o.quantize);
    set("showUnquantized", o.showUnquantized);
    set("snapping", app.snapping);
    set("snapGrid", o.snapToGrid);
    set("snapMarkers", o.snapToMarkers);
    set("autoRipple", o.autoRipple);
    set("ignoreGrouping", o.ignoreGrouping);
    set("showWaveforms", o.showWaveforms);
    set("showEventButtons", o.showEventButtons);
    set("eventHeaders", o.eventHeaders);
    set("rippleTracks", o.ripple == RippleMode::AffectedTracks);
    set("rippleMarkers", o.ripple == RippleMode::AffectedTracksAndMarkers);
    set("rippleAll", o.ripple == RippleMode::AllTracks);
    set("loop", app.loopOn);
    // Loop Playback is switched in several places; saved from here
    {
        QSettings st;
        if (st.value("options/loopPlayback", false).toBool() != app.loopOn) st.setValue("options/loopPlayback", app.loopOn);
    }
    set("bypassFx", app.bypassAllFx);
    set("snapAllEvents", o.snapToAllEvents);
    set("showTakeInfo", o.showTakeInfo);
    set("showMediaMarkers", o.showMediaMarkers);
    set("edgeFrames", o.edgeFrames);
    set("slipAllTakes", o.slipAllTakes);
    set("multicam", o.multicam);
    set("autoCrossfade", app.autoCrossfades());
    set("quickFade", app.prefs.quickFadeEdits);
}

void MainWindow::refresh(int changes)
{
    AppState& app = AppState::get();
    Actions& a = *a_;
    if (changes & (kProject | kFile)) {
        const QString name = app.path().empty() ? QString("Untitled") : QFileInfo(QString::fromStdString(app.path())).fileName();
        setWindowTitle(QString("%1%2 - Reno").arg(name, app.dirty() ? "*" : ""));
        a.get("undo")->setEnabled(app.canUndo());
        a.get("redo")->setEnabled(app.canRedo());
        a.get("undo")->setText(app.canUndo() ? QString("&Undo %1").arg(QString::fromStdString(app.undoName())) : QString("&Undo"));
        a.get("redo")->setText(app.canRedo() ? QString("&Redo %1").arg(QString::fromStdString(app.redoName())) : QString("&Redo"));
        const ProjectSettings& s = app.project().settings;
        diskText_->setToolTip(QString("%1x%2, %3 fps, %4 Hz").arg(s.width).arg(s.height).arg(s.fps(), 0, 'f', s.fpsDen == 1 ? 0 : 3).arg(s.sampleRate));
        if (changes & kFile) showDiskSpace();
        a.get("render")->setEnabled(app.project().length() > 0);
    }
    if (changes & (kSelection | kProject)) {
        const bool sel = !app.selection().empty();
        for (const char* n : {"cut", "copy", "delete", "group", "ungroup", "clearGroup", "selectGroup", "eventFx", "eventProperties", "trim", "trimStart", "trimEnd", "lockEvent"})
            a.get(n)->setEnabled(sel);
        a.get("trackFx")->setEnabled(app.currentTrack != 0);
        a.get("paste")->setEnabled(app.hasClipboard() || timeline_->hasEnvelopeClip());
        if (timeline_->options().tool == EditTool::Envelope) a.get("copy")->setEnabled(true);
        a.get("pasteInsert")->setEnabled(app.hasClipboard());
        a.get("pasteRepeat")->setEnabled(app.hasClipboard());
        // kept current here too, so keys given in Customize Keyboard work before a menu opens
        for (const char* n : {"pasteAttributes", "selectivePaste"})
            if (QAction* x = vegasAction(n)) x->setEnabled(app.hasClipboard() && sel);
        for (const char* n : {"eventHeal", "closeGaps", "selectEventsToEnd", "groupCutAll", "groupCopyAll", "groupDeleteAll", "openInTrimmer"})
            if (QAction* x = vegasAction(n)) x->setEnabled(sel);
    }
    if (changes & (kCursor | kProject)) {
        const Project& p = app.project();
        const bool region = app.loopEnd > app.loopStart;
        const double a0 = region ? app.loopStart : app.cursor(), b0 = region ? app.loopEnd : app.cursor();
        (void)a0;
        setReadoutText(cursorText_, QString::fromStdString(rulerTime(p, app.cursor())), true);
        setReadoutText(selEnd_, region ? QString::fromStdString(rulerTime(p, b0)) : QString(), true);
        setReadoutText(selLength_, region ? QString::fromStdString(rulerTime(p, b0 - a0 - p.settings.rulerStart)) : QString(), true);
        const std::pair<double, double> now(app.loopStart, app.loopEnd);
        if (now != lastSelection_) {
            if (!restoringSelection_ && lastSelection_.second > lastSelection_.first) {
                selectionHistory_.push_back(lastSelection_);
                if (selectionHistory_.size() > 5) selectionHistory_.erase(selectionHistory_.begin());
            }
            lastSelection_ = now;
        }
    }
    if (changes & (kCursor | kFxParams | kPrefs)) {
        gOptionsSyncIfChanged = (changes & (kFxParams | kPrefs)) == 0;
        syncOptions();
        gOptionsSyncIfChanged = false;
    }
    if (changes & kPlayback) updateRate();
}

}  // namespace app
}  // namespace reno
