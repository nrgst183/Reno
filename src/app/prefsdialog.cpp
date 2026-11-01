// Options > Preferences, as in VEGAS Pro 18: its tabs in order (General, Video, Preview Device, Audio, Audio Device,
// MIDI, VST
// Effects, Editing, Display, Sync, External Control & Automation, Deprecated Features, File I/O; no CD Settings) with
// VEGAS's
// wording and layout (from vegas180.exe's dialog resources 810-912), Default All on each tab.
//
// Values are Reno's Preferences (appstate.h) where the setting already existed there, otherwise vprefs.h's QSettings
// values.
// VEGAS settings for hardware and features Reno lacks (control surfaces, CD burning, GPU and stereoscopic 3D,
// ASIO...) are not shown.
#include "editdialogs.h"
#include "panels.h"
#include "midisync.h"
#include "playback.h"
#include "vprefs.h"
#include "../engine/catalog.h"
#include "strconv.h"
#include "logo.h"
#include "../ofx/gpupick.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QCursor>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStorageInfo>
#include <QTabWidget>
#include <QTabBar>
#include <QMouseEvent>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#ifdef RENO_HAVE_QTMULTIMEDIA
#include <QAudioDevice>
#include <QMediaDevices>
#endif
#include <cmath>
#include <cstring>
#include <map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif
#include <functional>

namespace reno {
namespace app {

namespace {


const char* const kVegasHost = "com.vegascreativesoftware.vegas";
const char* const kSonyHost = "com.sonycreativesoftware.vegas";
const char* const kPlainHost = "org.reno.host";

QLabel* note(const QString& text, QWidget* parent)
{
    QLabel* l = new QLabel(text, parent);
    l->setObjectName("note");
    l->setWordWrap(true);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, QColor(160, 160, 166));
    l->setPalette(p);
    QFont f = l->font();
    f.setPointSizeF(f.pointSizeF() * 0.92);
    l->setFont(f);
    return l;
}

QWidget* heading(const QString& text, QWidget* parent)
{
    QWidget* w = new QWidget(parent);
    QHBoxLayout* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 6, 0, 0);
    QLabel* l = new QLabel(text, w);
    QFrame* line = new QFrame(w);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    h->addWidget(l);
    h->addWidget(line, 1);
    return w;
}

QString freeSpace(const QString& folder)
{
    const QString d = QDir::fromNativeSeparators(folder.trimmed());
    const QStorageInfo st(d.isEmpty() ? QDir::tempPath() : d);
    if (!st.isValid() || st.bytesAvailable() <= 0) return QString("-");
    const double gb = (double)st.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
    return gb >= 1.0 ? QString("%1 Gigabytes").arg(gb, 0, 'f', 1) : QString("%1 Megabytes").arg(st.bytesAvailable() / (1024 * 1024));
}

// The computer's memory in MB (what the Dynamic RAM Preview can be set up to).
qint64 physicalMemoryMb()
{
#ifdef _WIN32
    MEMORYSTATUSEX m;
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) return (qint64)(m.ullTotalPhys / (1024 * 1024));
    return 0;
#else
    QFile f("/proc/meminfo");
    if (f.open(QIODevice::ReadOnly)) {
        const QList<QByteArray> parts = f.readLine().simplified().split(' ');
        if (parts.size() >= 2) return parts[1].toLongLong() / 1024;
    }
    return 0;
#endif
}

class ColorButton : public QToolButton {
public:
    ColorButton(QWidget* parent, const char* name) : QToolButton(parent)
    {
        setObjectName(name);
        setMinimumSize(24, 18);
        QObject::connect(this, &QToolButton::clicked, this, [this] {
            const QColor c = QColorDialog::getColor(color_, window(), "Color", QColorDialog::ShowAlphaChannel);
            if (c.isValid()) setColor(c);
        });
    }
    void setColor(const QColor& c)
    {
        color_ = c;
        update();
        if (onChanged) onChanged(c);
    }
    QColor color() const { return color_; }
    std::function<void(const QColor&)> onChanged;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), palette().button());
        const QRect r = rect().adjusted(3, 3, -4, -4);
        if (color_.alpha() < 255) {
            for (int y = 0; y < r.height(); y += 4)
                for (int x = 0; x < r.width(); x += 4) p.fillRect(QRect(r.x() + x, r.y() + y, 4, 4), ((x + y) / 4) % 2 ? Qt::lightGray : Qt::white);
        }
        p.fillRect(r, color_);
        p.setPen(palette().color(QPalette::Mid));
        p.drawRect(r);
    }

private:
    QColor color_ = Qt::black;
};

class FolderList : public QWidget {
public:
    FolderList(QWidget* parent, const char* name, const std::vector<std::string>& dirs) : QWidget(parent)
    {
        setObjectName(name);
        QVBoxLayout* v = new QVBoxLayout(this);
        v->setContentsMargins(0, 0, 0, 0);
        list_ = new QListWidget(this);
        list_->setObjectName("folders");
        list_->setFixedHeight(64);
        set(dirs);
        v->addWidget(list_);
        QHBoxLayout* h = new QHBoxLayout();
        edit_ = new QLineEdit(this);
        edit_->setObjectName("folderEdit");
        edit_->setPlaceholderText("A folder to add");
        QPushButton* add = new QPushButton("Add", this);
        add->setObjectName("folderAdd");
        QPushButton* browse = new QPushButton("Browse...", this);
        browse->setObjectName("folderBrowse");
        QPushButton* remove = new QPushButton("Remove", this);
        remove->setObjectName("folderRemove");
        h->addWidget(edit_, 1);
        h->addWidget(add);
        h->addWidget(browse);
        h->addWidget(remove);
        v->addLayout(h);
        QObject::connect(add, &QPushButton::clicked, this, [this] { addFolder(edit_->text()); });
        QObject::connect(edit_, &QLineEdit::returnPressed, this, [this] { addFolder(edit_->text()); });
        QObject::connect(browse, &QPushButton::clicked, this, [this] {
            const QString d = QFileDialog::getExistingDirectory(window(), "Plug-In Folder");
            if (!d.isEmpty()) addFolder(d);
        });
        QObject::connect(remove, &QPushButton::clicked, this, [this] { delete list_->currentItem(); });
    }
    void set(const std::vector<std::string>& dirs)
    {
        list_->clear();
        for (const auto& d : dirs) list_->addItem(QDir::toNativeSeparators(qs(d)));
    }
    std::vector<std::string> folders() const
    {
        std::vector<std::string> v;
        for (int k = 0; k < list_->count(); ++k) v.push_back(s8(QDir::fromNativeSeparators(list_->item(k)->text())));
        return v;
    }

private:
    void addFolder(const QString& d)
    {
        const QString t = QDir::toNativeSeparators(d.trimmed());
        if (t.isEmpty()) return;
        for (int k = 0; k < list_->count(); ++k)
            if (list_->item(k)->text() == t) return;
        list_->addItem(t);
        edit_->clear();
    }
    QListWidget* list_;
    QLineEdit* edit_;
};

// VEGAS's General tab is a list of check boxes in a box (built at run time, not in its dialog resource); these are its
// items in order, with the setting each one is.
struct GeneralItem {
    const char* key;
    const char* text;
    const char* keptNote;  // non-empty: Reno keeps the setting but nothing changes with it (the tool tip says why)
};
const GeneralItem kGeneralItems[] = {
    {"prefs/openLastProject", "Automatically open last project on startup", ""},
    {"prefs/confirmMediaDelete", "Confirm media file deletion when still in use", ""},
    {"prefs/savePrerenders", "Save active prerenders on project close", ""},
    {"prefs/closeMediaInactive", "Close media files when not the active application", ""},
    {"prefs/closePortsInactive", "Close audio and MIDI ports when not the active application", ""},
    {"prefs/newsfeed", "Use Newsfeed to stay informed about VEGAS product updates", "Reno has no such feature"},
    {"prefs/promptMatchMedia", "Prompt to adjust project settings to match first media added to timeline", ""},
    {"prefs/fileAssociations", "Check project file type associations at startup", "Reno has no such feature"},
    {"prefs/joystick", "Enable joystick support", "Reno has no such feature"},
    {"prefs/detect360", "Enable auto-detection of 360 media", "Reno has no such feature"},
    {"prefs/wave64", "Render large Wave files as Wave64", ""},
    {"prefs/wave4gb", "Allow Wave renders up to 4 GB", ""},
    {"prefs/ignoreFactChunk", "Ignore fact chunk when opening compressed Wave files", "Reno has no such feature"},
    {"prefs/pulldownRemoval", "Allow pulldown removal when opening 24p DV", "Reno has no such feature"},
    {"prefs/aafFrameUnit", "AAF Export - Use frame unit for audio", "Reno has no such feature"},
    {"prefs/aafClipEnvelope", "AAF Export - Use clip-based audio envelope", "Reno has no such feature"},
    {"prefs/mxfMultichannel", "Import MXF as multichannel", ""},
    {"prefs/stereoDualMono", "Import stereo as dual mono", ""},
    {"prefs/noRecompress", "Enable no-recompress rendering", "Reno has no such feature"},
    {"prefs/qsv", "Enable QSV Encoding and Decoding (where available)", ""},
    {"prefs/acesGpu", "Use GPU processing for ACES color management", "Reno has no such feature"},
    {"prefs/openclInterop", "Enable OpenCL/GL Interop for Intel GPU", "Reno has no such feature"},
    {"prefs/promptKeepRecorded", "Prompt to keep files after recording", ""},
    {"prefs/fxUndo", "Create undos for FX parameter changes", ""},
    {"prefs/autoNameMarkers", "Automatically name regions and markers if not playing", ""},
    {"prefs/spacePlayPause", "Make spacebar and F12 Play/Pause instead of Play/Stop", ""},
    {"prefs/alwaysMarkerLines", "Always draw marker lines", ""},
    {"prefs/dblClickTrimmer", "Double-click on media file loads into Trimmer instead of tracks", ""},
    {"prefs/trimmerHistoryFileFirst", "Show Trimmer history with filename first, then folder", ""},
    {"trimmer/autoSaveMarkers", "Automatically save Trimmer markers and regions with media file", ""},
    {"prefs/warnDeleteTrack", "Provide a warning if a track being deleted holds at least one event", ""},
};

const std::vector<std::pair<QString, QString>> kCurves = {
    {"Linear", "linear"}, {"Fast", "fast"}, {"Slow", "slow"}, {"Smooth", "smooth"}, {"Sharp", "sharp"}, {"Hold", "hold"}};
const std::vector<std::pair<QString, QString>> kKeyframeTypes = {
    {"Linear", "linear"}, {"Fast", "fast"}, {"Slow", "slow"}, {"Smooth", "smooth"}, {"Sharp", "sharp"}, {"Hold", "hold"}};

const std::vector<std::pair<QString, QString>> kEnvelopeKinds = {
    {"Volume", "volume"}, {"Pan", "pan"}, {"Mute", "mute"}, {"Bus Send", "send"}, {"Composite Level", "composite"},
    {"Fade to Color", "fadecolor"}, {"Velocity", "velocity"}, {"Motion Blur Amount", "motionblur"},
    {"Video Supersampling", "supersample"}, {"Transition Progress", "transition"}, {"FX Automation", "fx"}};
QColor envelopeDefaultColor(const QString& kind)
{
    if (kind == "volume" || kind == "composite") return QColor(52, 104, 255);
    if (kind == "pan" || kind == "fadecolor") return QColor(236, 52, 52);
    if (kind == "mute") return QColor(240, 206, 40);
    if (kind == "send") return QColor(196, 104, 240);
    if (kind == "velocity") return QColor(52, 206, 76);
    if (kind == "motionblur") return QColor(230, 120, 220);
    if (kind == "supersample") return QColor(40, 196, 206);
    if (kind == "transition") return QColor(186, 120, 250);
    return QColor(150, 226, 90);
}
QColor trackDefaultColor(int k)
{
    static const QColor colors[] = {QColor(89, 115, 168), QColor(124, 52, 65), QColor(150, 116, 52), QColor(70, 128, 74),
                                    QColor(52, 124, 132), QColor(118, 82, 158), QColor(160, 84, 116), QColor(108, 110, 64)};
    return colors[(size_t)k % 8];
}

// ---- VEGAS's Preferences layout: every tab's controls at the positions and sizes of the real dialog (prefs_layout.inc, from a
// capture of VEGAS Pro 18). The dialog is 435 x 686 pixels with the title bar and frame included; the positions are
// relative to the window's top left, so this is how far the client area is in from it.
#include "prefs_layout.inc"

constexpr int kFrameX = 1, kFrameY = 31;

// Texts compare without the trailing spaces VEGAS leaves in some (the mnemonic & stays in).
QString trimmed(const char* t) { return QString::fromUtf8(t).trimmed(); }

// A color button as VEGAS draws them (the capture lists them as owner-drawn buttons).
// One tab's page: places the controls Reno has at the cells VEGAS has for them, by their text, and finish() puts a disabled
// stand-in at every cell Reno has nothing for (so the page always has all of VEGAS's controls, in its places).
class Pg {
public:
    Pg() = default;
    Pg(QWidget* inner, const PrefTabCells* cells) : inner_(inner), t_(cells), used_((size_t)cells->count, false) {}
    QWidget* parent() const { return inner_; }

    // A label: the & of the text is a mnemonic only when the label has a control to give the focus to (set with link()).
    static QString plainLabel(QString t)
    {
        t.replace("&&", "\x01");
        t.remove('&');
        t.replace('\x01', '&');
        return t;
    }
    static void link(QLabel* lab, QWidget* w)
    {
        if (!lab || !w) return;
        if (w->focusPolicy() != Qt::NoFocus && !qobject_cast<QLabel*>(w)) {
            lab->setText(lab->property("mnemonicText").toString());
            lab->setBuddy(w);
        }
    }
    QLabel* newLabel(const QString& text)
    {
        QLabel* l = new QLabel(plainLabel(text), inner_);
        l->setProperty("mnemonicText", text);
        return l;
    }

    // The first cell of this kind with this text (the n-th, counting from 'n'), or -1.
    int find(const char* kind, const QString& text, int n = 0) const
    {
        for (int i = 0; i < t_->count; ++i)
            if (!strcmp(t_->cells[i].kind, kind) && trimmed(t_->cells[i].text) == text.trimmed() && n-- == 0) return i;
        return -1;
    }
    // The first cell of 'kind' after the cell 'from' that no control has taken yet.
    int after(int from, const char* kind) const
    {
        for (int i = from + 1; i < t_->count; ++i)
            if (!used_[(size_t)i] && !strcmp(t_->cells[i].kind, kind)) return i;
        return -1;
    }
    QRect rect(int i) const { return QRect(t_->cells[i].x - 11, t_->cells[i].y - 96, t_->cells[i].w, t_->cells[i].h); }
    template <class W> W* put(W* w, int cell)
    {
        if (cell < 0) {
            w->hide();
            return w;
        }
        used_[(size_t)cell] = true;
        w->setParent(inner_);
        w->setGeometry(rect(cell));
        w->show();
        return w;
    }
    QLabel* label(const QString& text, int n = 0)
    {
        const int c = find("label", text, n);
        QLabel* l = newLabel(c >= 0 ? trimmed(t_->cells[c].text) : QString());
        return put(l, c);
    }
    // A text label's partner: the 'kind' cell after the label with this text.
    template <class W> W* beside(W* w, const QString& labelText, const char* kind = nullptr, int n = 0)
    {
        int l = find("label", labelText, n);
        if (l >= 0) used_[(size_t)l] = true;
        if (l >= 0 && !t_->cells[l].text[0]) l = -1;
        QLabel* lab = nullptr;
        if (l >= 0) {
            lab = newLabel(trimmed(t_->cells[l].text));
            lab->setGeometry(rect(l));
            lab->show();
        }
        const char* k = kind ? kind : widgetKind(w);
        put(w, l >= 0 ? after(l, k) : -1);
        link(lab, w);
        return w;
    }
    // A label with its edit box and spin arrows (one spin box).
    template <class W> W* spin(W* w, const QString& labelText)
    {
        const int l = find("label", labelText);
        QLabel* lab = nullptr;
        if (l >= 0) {
            lab = newLabel(trimmed(t_->cells[l].text));
            lab->setGeometry(rect(l));
            lab->show();
            used_[(size_t)l] = true;
        }
        const int e = l >= 0 ? after(l, "edit") : -1;
        const int s = e >= 0 ? after(e, "spin") : -1;
        if (e < 0) {
            w->hide();
            return w;
        }
        QRect r = rect(e);
        if (s >= 0) {
            r = r.united(rect(s));
            used_[(size_t)s] = true;
        }
        used_[(size_t)e] = true;
        w->setParent(inner_);
        w->setGeometry(r);
        w->show();
        link(lab, w);
        return w;
    }
    // An edit box (and spin arrows) that follows another cell of the page and has no label of its own.
    template <class W> W* edit(W* w, int e)
    {
        if (e < 0) {
            w->hide();
            return w;
        }
        const int s = after(e, "spin");
        QRect r = rect(e);
        if (s >= 0 && t_->cells[s].x == t_->cells[e].x + t_->cells[e].w) {
            r = r.united(rect(s));
            used_[(size_t)s] = true;
        }
        used_[(size_t)e] = true;
        w->setParent(inner_);
        w->setGeometry(r);
        w->show();
        return w;
    }
    // A check box, radio button or push button by its text.
    template <class W> W* button(W* w, const QString& text, int n = 0)
    {
        const char* k = widgetKind(w);
        int c = find(k, text, n);
        if (c < 0 && !strcmp(k, "check")) c = find("push", text, n);
        if (c >= 0) w->setText(trimmed(t_->cells[c].text));
        return put(w, c);
    }
    int cellOfLabel(const QString& text) { return find("label", text); }
    void take(int c)
    {
        if (c >= 0) used_[(size_t)c] = true;
    }
    bool has(const char* kind, const QString& text) const { return find(kind, text) >= 0; }

    // Everything VEGAS has here that Reno gave no control to: disabled stand-ins (labels and separator lines as they are).
    void finish()
    {
        for (int i = 0; i < t_->count; ++i) {
            if (used_[(size_t)i]) continue;
            const PrefCell& c = t_->cells[i];
            const QString k = c.kind, text = trimmed(c.text);
            QWidget* w = nullptr;
            if (k == "label") {
                if (text.isEmpty() && c.h <= 3) {
                    QFrame* line = new QFrame(inner_);
                    line->setFrameShape(QFrame::HLine);
                    line->setFrameShadow(QFrame::Sunken);
                    w = line;
                } else {
                    QLabel* l = new QLabel(text == "<value>" ? QString() : plainLabel(text), inner_);
                    l->setWordWrap(c.h > 20);
                    w = l;
                }
            } else if (k == "check") {
                w = new QCheckBox(text, inner_);
            } else if (k == "radio") {
                w = new QRadioButton(text, inner_);
            } else if (k == "push" || k == "defpush") {
                w = new QPushButton(text, inner_);
            } else if (k == "edit") {
                if (c.h > 40) {
                    QPlainTextEdit* e = new QPlainTextEdit(inner_);
                    e->setReadOnly(true);
                    w = e;
                } else if (i + 1 < t_->count && !strcmp(t_->cells[i + 1].kind, "spin") && t_->cells[i + 1].x == c.x + c.w) {
                    QSpinBox* s = new QSpinBox(inner_);
                    s->setGeometry(rect(i).united(rect(i + 1)));
                    used_[(size_t)i + 1] = true;
                    s->setObjectName("standIn");
                    s->setEnabled(false);
                    s->show();
                    used_[(size_t)i] = true;
                    continue;
                } else {
                    w = new QLineEdit(inner_);
                }
            } else if (k == "combo") {
                w = new QComboBox(inner_);
            } else if (k == "slider") {
                QSlider* s = new QSlider(Qt::Horizontal, inner_);
                w = s;
            } else if (k == "list") {
                w = new QListWidget(inner_);
            } else if (k == "listview") {
                QTreeWidget* t = new QTreeWidget(inner_);
                t->setRootIsDecorated(false);
                w = t;
            } else if (k == "color") {
                w = new ColorButton(inner_, "standInColor");
            } else if (k == "spin") {
                continue;
            } else if (k == "pane") {
                continue;
            } else {
                continue;
            }
            w->setGeometry(rect(i));
            w->setObjectName("standIn");
            if (k != "label") {
                w->setEnabled(false);
                w->setToolTip("VEGAS has this here; Reno has no such setting.");
            }
            w->show();
            used_[(size_t)i] = true;
        }
    }

private:
    static const char* widgetKind(QWidget* w)
    {
        if (qobject_cast<QRadioButton*>(w)) return "radio";
        if (qobject_cast<QCheckBox*>(w)) return "check";
        if (qobject_cast<QPushButton*>(w)) return "push";
        if (qobject_cast<QComboBox*>(w)) return "combo";
        if (qobject_cast<QSlider*>(w)) return "slider";
        if (qobject_cast<QListWidget*>(w)) return "list";
        if (qobject_cast<QTreeWidget*>(w)) return "listview";
        if (qobject_cast<QPlainTextEdit*>(w) || qobject_cast<QLineEdit*>(w)) return "edit";
        if (dynamic_cast<ColorButton*>(w)) return "color";
        return "edit";
    }
    QWidget* inner_ = nullptr;
    const PrefTabCells* t_ = nullptr;
    std::vector<bool> used_;
};

// VEGAS's tab bar: the tabs wrap onto several rows that are stretched to the full width, and the row of the selected tab is
// the one next to the page (the others keep their order above it).
class WrapTabBar : public QTabBar {
public:
    explicit WrapTabBar(QWidget* parent) : QTabBar(parent)
    {
        setExpanding(false);
        setDrawBase(false);
        setFocusPolicy(Qt::TabFocus);
    }
    QSize sizeHint() const override { return QSize(421, 3 * 18 + 4); }
    QSize minimumSizeHint() const override { return sizeHint(); }
    // Where the tabs are (bar coordinates): VEGAS's three rows, each stretched to the full width, the selected tab's row last.
    // The rows are fixed (General ... VST Effects, Editing ... Sync, the rest) and the tabs share a row's width in the
    // proportions VEGAS gives them, so the bar looks the same whatever the font.
    std::vector<QRect> layoutRects() const
    {
        static const std::map<QString, int> widths = {
            {"General", 52}, {"Video", 45}, {"Preview Device", 87}, {"Audio", 44}, {"Audio Device", 76}, {"MIDI", 44}, {"VST Effects", 69},
            {"Editing", 99}, {"Display", 100}, {"CD Settings", 122}, {"Sync", 96}, {"External Control && Automation", 191},
            {"Deprecated Features", 147}, {"File I/O", 79}};
        const int n = count();
        std::vector<std::vector<int>> rows(3);
        for (int i = 0; i < n; ++i) rows[(size_t)(i < 7 ? 0 : i < 11 ? 1 : 2)].push_back(i);
        auto nat = [&](int i) {
            auto it = widths.find(tabText(i));
            return it != widths.end() ? it->second : 45;
        };
        int sel = 0;
        for (size_t r = 0; r < rows.size(); ++r)
            for (int i : rows[r])
                if (i == currentIndex()) sel = (int)r;
        std::vector<std::vector<int>> order;
        for (size_t r = 0; r < rows.size(); ++r)
            if ((int)r != sel) order.push_back(rows[r]);
        order.push_back(rows[(size_t)sel]);
        std::vector<QRect> out((size_t)n);
        for (size_t r = 0; r < order.size(); ++r) {
            int sum = 0;
            for (int i : order[r]) sum += nat(i);
            int x = 2;
            for (size_t k = 0; k < order[r].size(); ++k) {
                const int i = order[r][k];
                const int w = k + 1 == order[r].size() ? kRowWidth - (x - 2) : (int)std::lround((double)nat(i) * kRowWidth / std::max(1, sum));
                out[(size_t)i] = QRect(x, 2 + (int)r * 18, w, 18);
                x += w;
            }
        }
        return out;
    }
    int hit(QPoint p) const
    {
        const std::vector<QRect> r = layoutRects();
        for (size_t i = 0; i < r.size(); ++i)
            if (r[i].contains(p)) return (int)i;
        return -1;
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        const std::vector<QRect> rects = layoutRects();
        for (int i = 0; i < count(); ++i) {
            const bool sel = i == currentIndex();
            const QRect r = rects[(size_t)i];
            // Windows 10's tabs when the dialog is light: grey, white for the selected one, light blue under the mouse
            const bool lightLook = palette().color(QPalette::Window).lightness() > 160;
            const bool hot = !sel && lightLook && r.contains(mapFromGlobal(QCursor::pos())) && underMouse();
            p.fillRect(r, sel ? palette().color(QPalette::Base) : hot ? QColor(216, 234, 249) : lightLook ? QColor(240, 240, 240) : palette().color(QPalette::Button));
            p.setPen(lightLook ? QColor(217, 217, 217) : palette().color(QPalette::Mid));
            p.drawRect(r.adjusted(0, 0, -1, sel ? 0 : -1));
            p.setPen(palette().color(QPalette::ButtonText));
            QString label = tabText(i);
            label.replace("&&", "\x01").remove('&').replace('\x01', '&');
            // a text wider than its tab (a wider font than VEGAS's) is drawn smaller rather than cut off
            QFont f = font();
            for (int px = f.pixelSize() > 0 ? f.pixelSize() : 12; px > 7 && QFontMetrics(f).horizontalAdvance(label) > r.width() - 6; --px) f.setPixelSize(px);
            p.setFont(f);
            p.drawText(r, Qt::AlignCenter, label);
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const int i = hit(e->pos());
        if (i >= 0) setCurrentIndex(i);
    }
    void mouseReleaseEvent(QMouseEvent*) override {}

private:
    static constexpr int kRowWidth = 417;
};

class PrefsTabs : public QTabWidget {
public:
    explicit PrefsTabs(QWidget* parent) : QTabWidget(parent) { setTabBar(new WrapTabBar(this)); }
};

}  // namespace

// Preferences > Audio Device > Advanced...: VEGAS's Advanced Audio Configuration (352 x 413 with its frame; positions are the
// capture's less the frame's 1 and 31). The list holds the output and input devices; the settings of the selected one are kept
// (QSettings "audioAdvanced/<device>/...") for VEGAS's names, while Reno's audio output chooses its own buffers.
void advancedAudioConfiguration(QWidget* parent, const QString& deviceName, const QString& deviceId)
{
    QDialog d(parent);
    d.setObjectName("advancedAudioConfiguration");
    d.setWindowTitle("Advanced Audio Configuration");
    QFont f = d.font();
    f.setPixelSize(12);
    d.setFont(f);
    d.setFixedSize(352 - 2, 413 - 31 - 1);
    auto at = [](QWidget* w, int x, int y, int cw, int ch) { w->setGeometry(x - 1, y - 31, cw, ch); return w; };
    QLabel* devLabel = new QLabel("Audio &devices:", &d);
    at(devLabel, 12, 42, 83, 13);
    QListWidget* devices = new QListWidget(&d);
    devices->setObjectName("audioDevices");
    at(devices, 12, 57, 329, 96);
    devLabel->setBuddy(devices);
    const QString out = deviceName.isEmpty() ? QString("Default") : deviceName;
    devices->addItem("Output : " + out);
    devices->addItem("Input : Default");
    devices->setCurrentRow(0);
    QLabel* settingsLabel = new QLabel("Settings for selected audio device:", &d);
    at(settingsLabel, 12, 159, 195, 13);
    QCheckBox* interpolate = new QCheckBox("&Interpolate position (required for inaccurate devices)", &d);
    at(interpolate, 24, 179, 278, 16);
    QLabel* biasLabel = new QLabel("&Position bias (-64 to 64):", &d);
    at(biasLabel, 24, 203, 135, 13);
    QSlider* bias = new QSlider(Qt::Horizontal, &d);
    bias->setRange(-64, 64);
    at(bias, 177, 200, 135, 20);
    biasLabel->setBuddy(bias);
    QLabel* biasValue = new QLabel("0", &d);
    at(biasValue, 319, 203, 21, 13);
    QCheckBox* noPreroll = new QCheckBox("Do not pre-&roll buffers before starting playback", &d);
    at(noPreroll, 24, 228, 270, 16);
    QFrame* rule1 = new QFrame(&d);
    rule1->setFrameShape(QFrame::HLine);
    rule1->setFrameShadow(QFrame::Sunken);
    at(rule1, 1, 254, 349, 2);
    QLabel* buffersLabel = new QLabel("Audio &buffers:", &d);
    at(buffersLabel, 24, 267, 135, 13);
    QSlider* buffers = new QSlider(Qt::Horizontal, &d);
    buffers->setRange(2, 16);
    at(buffers, 177, 263, 135, 20);
    buffersLabel->setBuddy(buffers);
    QLabel* buffersValue = new QLabel("4", &d);
    at(buffersValue, 319, 267, 21, 13);
    QLabel* sizeLabel = new QLabel("Buffer &size (samples):", &d);
    at(sizeLabel, 24, 294, 135, 13);
    QComboBox* size = new QComboBox(&d);
    size->setObjectName("bufferSize");
    for (const char* t : {"MME", "64", "96", "128", "192", "256", "384", "512", "768", "1024", "1536", "2048", "3072", "4096", "8192", "16384"}) size->addItem(t);
    at(size, 177, 291, 165, 21);
    sizeLabel->setBuddy(size);
    QLabel* prioLabel = new QLabel("Priorit&y:", &d);
    at(prioLabel, 24, 322, 135, 13);
    QComboBox* prio = new QComboBox(&d);
    prio->setObjectName("audioPriority");
    for (const char* t : {"Time Critical", "Highest", "Above Normal", "Normal", "Below Normal", "Lowest", "Idle"}) prio->addItem(t);
    at(prio, 177, 319, 165, 21);
    prioLabel->setBuddy(prio);
    QFrame* rule2 = new QFrame(&d);
    rule2->setFrameShape(QFrame::HLine);
    rule2->setFrameShadow(QFrame::Sunken);
    at(rule2, 9, 371, 334, 2);
    QPushButton* ok = new QPushButton("OK", &d);
    ok->setDefault(true);
    at(ok, 268, 381, 75, 23);
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);

    // the settings of the selected device (output and input keep their own); kept for VEGAS's names
    const QString base = "audioAdvanced/" + (deviceId.isEmpty() ? QString("default") : deviceId) + "/";
    auto key = [&devices, base](const char* n) { return base + (devices->currentRow() == 1 ? "in/" : "out/") + n; };
    QSettings st;
    auto load = [&] {
        interpolate->setChecked(st.value(key("interpolate"), true).toBool());
        bias->setValue(st.value(key("bias"), 0).toInt());
        noPreroll->setChecked(st.value(key("noPreroll"), false).toBool());
        buffers->setValue(st.value(key("buffers"), 4).toInt());
        size->setCurrentText(st.value(key("size"), "MME").toString());
        prio->setCurrentText(st.value(key("priority"), "Time Critical").toString());
        biasValue->setText(QString::number(bias->value()));
        buffersValue->setText(QString::number(buffers->value()));
    };
    auto save = [&] {
        st.setValue(key("interpolate"), interpolate->isChecked());
        st.setValue(key("bias"), bias->value());
        st.setValue(key("noPreroll"), noPreroll->isChecked());
        st.setValue(key("buffers"), buffers->value());
        st.setValue(key("size"), size->currentText());
        st.setValue(key("priority"), prio->currentText());
    };
    int shown = 0;
    load();
    // the buffer size box is for the driver types that take a size; the default MME driver sets its own
    size->setEnabled(false);
    QObject::connect(bias, &QSlider::valueChanged, &d, [biasValue](int v) { biasValue->setText(QString::number(v)); });
    QObject::connect(buffers, &QSlider::valueChanged, &d, [buffersValue](int v) { buffersValue->setText(QString::number(v)); });
    QObject::connect(devices, &QListWidget::currentRowChanged, &d, [&](int row) {
        if (row < 0) return;
        const int now = devices->currentRow();
        // keep what was shown for the device just left, then show the other's
        devices->blockSignals(true);
        devices->setCurrentRow(shown);
        save();
        devices->setCurrentRow(now);
        devices->blockSignals(false);
        shown = now;
        load();
    });
    d.exec();
    save();
}

namespace {

class PrefsDialog : public QDialog {
public:
    explicit PrefsDialog(QWidget* parent) : QDialog(parent), pr_(AppState::get().prefs)
    {
        setObjectName("preferencesDialog");
        setWindowTitle("Preferences");
        // VEGAS's dialog is 435 x 686 with its frame; it has a fixed size
        QFont f = font();
        f.setPixelSize(12);
        setFont(f);
        setFixedSize(435 - 2 * kFrameX, 686 - kFrameY - kFrameX);
        tabs_ = new PrefsTabs(this);
        tabs_->setObjectName("tabs");
        tabs_->setGeometry(7 - kFrameX, 38 - kFrameY, 421, 611);
        tabs_->setDocumentMode(false);
        tabs_->setStyleSheet("QTabWidget::pane { border: 1px solid #d9d9d9; margin: 0px; padding: 0px; background: #ffffff; }");
        renoPage();

        buildGeneral();
        buildVideo();
        buildPreviewDevice();
        buildAudio();
        buildAudioDevice();
        buildMidi();
        buildVst();
        buildEditing();
        buildDisplay();
        buildCd();
        buildSync();
        buildExternal();
        buildDeprecated();
        buildFileIo();
        buildReno();

        fitFont();
        for (Bind& b : binds_) b.load();

        // OK, Cancel and Apply sit where VEGAS has them
        auto button = [this](const QString& text, const char* name, int x) {
            QPushButton* b = new QPushButton(text, this);
            b->setObjectName(name);
            b->setGeometry(x - kFrameX, 655 - kFrameY, 75, 23);
            return b;
        };
        QPushButton* ok = button("OK", "ok", 191);
        ok->setDefault(true);
        QPushButton* cancel = button("Cancel", "cancel", 272);
        QPushButton* apply = button("&Apply", "apply", 353);
        QObject::connect(ok, &QPushButton::clicked, this, [this] { applyAll(); accept(); });
        QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
        QObject::connect(apply, &QPushButton::clicked, this, [this] { applyAll(); });
        const int last = QSettings().value("prefs/dialogTab", 0).toInt();
        if (last >= 0 && last < tabs_->count()) tabs_->setCurrentIndex(last);
        QObject::connect(tabs_, &QTabWidget::currentChanged, this, [this](int i) {
            QSettings().setValue("prefs/dialogTab", i);
            tabs_->tabBar()->update();
            QTimer::singleShot(0, this, [this] { alignPages(); });
        });
        QTimer::singleShot(0, this, [this] { alignPages(); });
    }

protected:
    void showEvent(QShowEvent* e) override
    {
        QDialog::showEvent(e);
        alignPages();
    }

private:
    struct Bind {
        int tab;
        std::function<void()> load, save, reset;
    };
    std::vector<Bind> binds_;
    QTabWidget* tabs_ = nullptr;
    Preferences pr_;
    int tab_ = 0;
    std::vector<QWidget*> inners_;
    QWidget* renoInner_ = nullptr;
    QFormLayout* renoForm_ = nullptr;
    QString renoGroup_;

    // VEGAS's cells are sized for Segoe UI at 9 pt (12 px). With a wider font (any other platform's default) the texts would
    // be cut off, so the dialog's font is made as much smaller as the tightest cell needs (never larger than 12 px and never
    // below 10 px, and what is still too wide is given room); with Segoe UI nothing changes.
    void fitFont()
    {
        double ratio = 1.0;
        const QFontMetrics fm(font());
        auto need = [&](QWidget* w, const QString& raw, int extra) {
            QString t = raw;
            t.replace("&&", "\x01");
            t.remove('&');
            t.replace('\x01', "&");
            if (t.isEmpty() || w->width() <= 0) return;
            const int have = w->width() - extra;
            const int wide = fm.horizontalAdvance(t);
            if (wide > have && have >= 30) {
                ratio = std::min(ratio, (double)have / wide);
            }
        };
        for (QWidget* in : inners_) {
            if (in->parentWidget() == renoInner_) continue;
            for (QWidget* w : in->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively)) {
                if (QLabel* l = qobject_cast<QLabel*>(w)) {
                    if (!l->wordWrap() && l->pixmap().isNull()) need(l, l->property("mnemonicText").isValid() ? l->property("mnemonicText").toString() : l->text(), 2);
                } else if (QCheckBox* c = qobject_cast<QCheckBox*>(w)) need(c, c->text(), 20);
                else if (QRadioButton* r = qobject_cast<QRadioButton*>(w)) need(r, r->text(), 20);
                else if (QPushButton* p = qobject_cast<QPushButton*>(w)) need(p, p->text(), 14);
                else if (QGroupBox* g = qobject_cast<QGroupBox*>(w)) need(g, g->title(), 20);
            }
        }
        if (ratio < 1.0) {
            QFont f = font();
            f.setPixelSize(std::max(10, (int)std::floor(12 * std::max(ratio, 0.85))));
            setFont(f);
        }
        // every control gets the size itself (some styles give the control classes fonts of their own that ignore the parent's)
        for (QWidget* in : inners_) {
            if (in->parentWidget() == renoInner_) continue;
            in->setFont(font());
            for (QWidget* w : in->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively))
                if (!qobject_cast<QLabel*>(w) || w->objectName() != "note") w->setFont(font());
        }
        // What still does not fit gets as much more width as there is room for (up to the next control on its line).
        const QFontMetrics fm2(font());
        for (QWidget* in : inners_) {
            if (in->parentWidget() == renoInner_) continue;
            const QList<QWidget*> all = in->findChildren<QWidget*>(QString(), Qt::FindChildrenRecursively);
            for (QWidget* w : all) {
                QString raw;
                int extra = 2;
                if (QLabel* l = qobject_cast<QLabel*>(w)) {
                    if (l->wordWrap() || !l->pixmap().isNull()) continue;
                    raw = l->text();
                } else if (QCheckBox* c = qobject_cast<QCheckBox*>(w)) { raw = c->text(); extra = 20; }
                else if (QRadioButton* r = qobject_cast<QRadioButton*>(w)) { raw = r->text(); extra = 20; }
                else continue;
                raw.replace("&&", "\x01");
                raw.remove('&');
                raw.replace('\x01', "&");
                const int wide = fm2.horizontalAdvance(raw) + extra;
                if (raw.isEmpty() || wide <= w->width()) continue;
                int right = in->width() - 2;
                for (QWidget* o : all) {
                    if (o == w || !o->isVisible() || o->parentWidget() != w->parentWidget()) continue;
                    if (QFrame* fr = qobject_cast<QFrame*>(o)) if (!qobject_cast<QLabel*>(fr) && fr->frameShape() != QFrame::NoFrame) continue;
                    if (o->x() >= w->x() + w->width() - 1 && o->y() < w->y() + w->height() && o->y() + o->height() > w->y())
                        right = std::min(right, o->x() - 3);
                }
                w->resize(std::min(wide, std::max(w->width(), right - w->x())), w->height());
            }
        }
    }

    // The pages' controls are placed by the dialog's coordinates, so each page's inner widget is moved to where the dialog
    // wants its top left whatever frame the style draws around the page.
    void alignPages()
    {
        for (QWidget* in : inners_) {
            const QPoint have = in->mapTo(this, QPoint(0, 0));
            const QPoint want(11 - kFrameX, 96 - kFrameY);
            if (have != want) in->move(in->pos() + want - have);
        }
    }

    // A page of VEGAS's: a plain widget 413 x 549 with absolutely placed controls (see Pg), a "De&fault All" button of its
    // own (and whatever buttons 'extra' lists, placed by their text) and a tab in the tab bar.
    Pg vegasPage(const QString& title, const char* name, std::vector<QPushButton*> extra = {})
    {
        QWidget* outer = new QWidget(tabs_);
        outer->setObjectName(name);
        QWidget* inner = new QWidget(outer);
        inner->setObjectName("pageArea");
        inner->setGeometry(0, 0, 413, 549);
        inners_.push_back(inner);
        const int index = tabs_->addTab(outer, title);
        tab_ = index;
        const PrefTabCells* cells = nullptr;
        for (const PrefTabCells& t : kPrefTabs) {
            const QString n = QString::fromUtf8(t.name);
            // "tab 12 External Control & Automation": the title after the number ("&&" is how the tab shows it)
            if (n.startsWith("tab ") && n.mid(n.indexOf(' ', 4) + 1) == QString(title).replace("&&", "&")) cells = &t;
        }
        Pg pg(inner, cells);
        for (QPushButton* b : extra) pg.button(b, b->text());
        QPushButton* def = new QPushButton("De&fault All", inner);
        def->setObjectName("defaultAll");
        pg.button(def, "De&fault All");
        QObject::connect(def, &QPushButton::clicked, this, [this, index] {
            for (Bind& b : binds_)
                if (b.tab == index && b.reset) b.reset();
        });
        return pg;
    }

    // Reno's own settings that VEGAS's tabs have no place for: their own tab at the end, in groups named for the tab they
    // belong to.
    void renoPage()
    {
        renoInner_ = new QWidget(tabs_);
        renoInner_->setObjectName("renoTab");
        QWidget* inner = new QWidget(renoInner_);
        inner->setObjectName("pageArea");
        inner->setGeometry(0, 0, 413, 549);
        inners_.push_back(inner);
        QScrollArea* scroll = new QScrollArea(inner);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setGeometry(8, 8, 397, 505);
        QWidget* body = new QWidget(scroll);
        scroll->setWidget(body);
        renoForm_ = new QFormLayout(body);
        QFrame* line = new QFrame(inner);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        line->setGeometry(8, 512, 396, 2);
        renoBody_ = body;
        renoDefault_ = new QPushButton("De&fault All", inner);
        renoDefault_->setObjectName("defaultAll");
        renoDefault_->setGeometry(330, 520, 75, 23);
    }
    QWidget* renoBody_ = nullptr;
    QPushButton* renoDefault_ = nullptr;

    // The form of Reno's tab, with a heading for the group of the tab it belongs to (once per group).
    QFormLayout* extra(const QString& group)
    {
        if (renoGroup_ != group) {
            renoGroup_ = group;
            renoForm_->addRow(heading(group, renoBody_));
        }
        return renoForm_;
    }

    void bindCheck(QCheckBox* c, const QString& key)
    {
        binds_.push_back({tab_, [c, key] { c->setChecked(vprefs::on(key)); }, [c, key] { vprefs::set(key, c->isChecked()); },
                          [c, key] { c->setChecked(vprefs::defaultOf(key).toBool()); }});
    }
    QCheckBox* check(QWidget* parent, const QString& text, const QString& key, const char* name, const QString& kept = QString())
    {
        QCheckBox* c = new QCheckBox(text, parent);
        c->setObjectName(name);
        if (!kept.isEmpty()) c->setToolTip("Kept: " + kept + ".");
        bindCheck(c, key);
        return c;
    }
    QComboBox* combo(QWidget* parent, const QString& key, const char* name, const std::vector<std::pair<QString, QString>>& items)
    {
        QComboBox* c = new QComboBox(parent);
        c->setObjectName(name);
        for (const auto& it : items) c->addItem(it.first, it.second);
        auto pick = [c](const QString& v) {
            const int i = c->findData(v);
            c->setCurrentIndex(i >= 0 ? i : 0);
        };
        binds_.push_back({tab_, [pick, key] { pick(vprefs::text(key)); }, [c, key] { vprefs::set(key, c->currentData().toString()); },
                          [pick, key] { pick(vprefs::defaultOf(key).toString()); }});
        return c;
    }
    QDoubleSpinBox* real(QWidget* parent, const QString& key, const char* name, double lo, double hi, int decimals, double step)
    {
        QDoubleSpinBox* s = new QDoubleSpinBox(parent);
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        s->setSingleStep(step);
        s->setKeyboardTracking(false);
        binds_.push_back({tab_, [s, key] { s->setValue(vprefs::real(key)); }, [s, key] { vprefs::set(key, s->value()); },
                          [s, key] { s->setValue(vprefs::defaultOf(key).toDouble()); }});
        return s;
    }
    QSpinBox* integer(QWidget* parent, const QString& key, const char* name, int lo, int hi)
    {
        QSpinBox* s = new QSpinBox(parent);
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setKeyboardTracking(false);
        binds_.push_back({tab_, [s, key] { s->setValue(vprefs::number(key)); }, [s, key] { vprefs::set(key, s->value()); },
                          [s, key] { s->setValue(vprefs::defaultOf(key).toInt()); }});
        return s;
    }
    QLineEdit* textBox(QWidget* parent, const QString& key, const char* name)
    {
        QLineEdit* e = new QLineEdit(parent);
        e->setObjectName(name);
        binds_.push_back({tab_, [e, key] { e->setText(QDir::toNativeSeparators(vprefs::text(key))); },
                          [e, key] { vprefs::set(key, QDir::fromNativeSeparators(e->text().trimmed())); },
                          [e, key] { e->setText(QDir::toNativeSeparators(vprefs::defaultOf(key).toString())); }});
        return e;
    }
    ColorButton* colorButton(QWidget* parent, const QString& key, const char* name)
    {
        ColorButton* b = new ColorButton(parent, name);
        binds_.push_back({tab_, [b, key] { b->setColor(vprefs::color(key, Qt::black)); }, [b, key] { vprefs::set(key, b->color().name(QColor::HexArgb)); },
                          [b, key] { b->setColor(QColor(vprefs::defaultOf(key).toString())); }});
        return b;
    }
    QWidget* browseRow(QWidget* parent, QLineEdit* e, bool folder, const QString& filter = QString())
    {
        QWidget* row = new QWidget(parent);
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        e->setParent(row);
        QPushButton* b = new QPushButton("&Browse...", row);
        h->addWidget(e, 1);
        h->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, row, [e, folder, filter, row] {
            const QString now = QDir::fromNativeSeparators(e->text());
            const QString f = folder ? QFileDialog::getExistingDirectory(row->window(), "Folder", now)
                                     : QFileDialog::getOpenFileName(row->window(), "File", now, filter);
            if (!f.isEmpty()) e->setText(QDir::toNativeSeparators(f));
        });
        return row;
    }
    void bindPref(std::function<void(const Preferences&)> load, std::function<void(Preferences&)> save)
    {
        binds_.push_back({tab_, [this, load] { load(pr_); }, [this, save] { save(pr_); }, [load] { load(Preferences()); }});
    }

    // A Browse... button for an edit box.
    void wireBrowse(QPushButton* b, QLineEdit* e, bool folder, const QString& filter = QString())
    {
        QObject::connect(b, &QPushButton::clicked, b, [e, folder, filter, b] {
            const QString now = QDir::fromNativeSeparators(e->text());
            const QString f = folder ? QFileDialog::getExistingDirectory(b->window(), "Browse For Folder", now)
                                     : QFileDialog::getOpenFileName(b->window(), "Browse", now, filter);
            if (!f.isEmpty()) e->setText(QDir::toNativeSeparators(f));
        });
    }
    // The "N frames at F fps" text under a length setting.
    void framesLabel(QLabel* fr, QDoubleSpinBox* s)
    {
        const double fps = std::max(1.0, AppState::get().project().settings.fps());
        auto show = [s, fr, fps] { fr->setText(QString("%1 frames at %2 fps").arg((int)std::lround(s->value() * fps)).arg(fps, 0, 'f', 3)); };
        QObject::connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), fr, [show](double) { show(); });
        QTimer::singleShot(0, fr, show);
    }
    // A trackbar with its value shown beside it (VEGAS's way of setting a number): 'scale' units per 1 of the setting.
    QSlider* trackbar(const char* name, int lo, int hi)
    {
        QSlider* s = new QSlider(Qt::Horizontal);
        s->setObjectName(name);
        s->setRange(lo, hi);
        return s;
    }

    void buildGeneral()
    {
        Pg p = vegasPage("General", "generalTab");
        QListWidget* list = new QListWidget();
        list->setObjectName("generalList");
        for (const GeneralItem& g : kGeneralItems) {
            QListWidgetItem* it = new QListWidgetItem(QString::fromUtf8(g.text), list);
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setData(Qt::UserRole, QString(g.key));
            if (*g.keptNote) it->setToolTip(QString("Kept: %1.").arg(g.keptNote));
            const QString key = g.key;
            binds_.push_back({tab_, [it, key] { it->setCheckState(vprefs::on(key) ? Qt::Checked : Qt::Unchecked); },
                              [it, key] { vprefs::set(key, it->checkState() == Qt::Checked); },
                              [it, key] { it->setCheckState(vprefs::defaultOf(key).toBool() ? Qt::Checked : Qt::Unchecked); }});
        }
        p.beside(list, "&General preferences:");
        QCheckBox* recent = p.button(check(p.parent(), "&Recently used project list:", "prefs/recentList", "recentList"), "&Recently used project list:");
        QSpinBox* count = integer(p.parent(), "prefs/recentCount", "recentCount", 1, 9);
        p.edit(count, p.after(p.find("check", "&Recently used project list:"), "edit"));
        (void)recent;
        QLineEdit* temp = textBox(p.parent(), "prefs/tempFolder", "tempFolder");
        temp->setPlaceholderText(QDir::toNativeSeparators(QDir::tempPath()));
        p.beside(temp, "&Temporary files folder:");
        QPushButton* browse = p.button(new QPushButton("&Browse...", p.parent()), "&Browse...");
        wireBrowse(browse, temp, true);
        p.label("Free storage space in selected folder:");
        QLabel* space = new QLabel(p.parent());
        space->setObjectName("tempFolderSpace");
        p.put(space, p.after(p.cellOfLabel("Free storage space in selected folder:"), "label"));
        auto showSpace = [temp, space] { space->setText(freeSpace(temp->text())); };
        QObject::connect(temp, &QLineEdit::textChanged, space, [showSpace](const QString&) { showSpace(); });
        QTimer::singleShot(0, space, showSpace);
        p.finish();
    }

    void buildVideo()
    {
        Pg p = vegasPage("Video", "videoTab");
        QWidget* b = p.parent();
        QSpinBox* ram = new QSpinBox(b);
        ram->setObjectName("ramPreviewMb");
        ram->setRange(0, 65536);
        bindPref([ram](const Preferences& pr) { ram->setValue(pr.ramPreviewMb); }, [ram](Preferences& pr) { pr.ramPreviewMb = ram->value(); });
        p.spin(ram, "&Dynamic RAM Preview max (MB):");
        QLabel* avail = new QLabel(QString("Max available: %1 MB").arg(physicalMemoryMb()), b);
        p.put(avail, p.after(p.cellOfLabel("&Dynamic RAM Preview max (MB):"), "label"));
        QSpinBox* threads = new QSpinBox(b);
        threads->setObjectName("renderThreads");
        threads->setRange(0, 16);
        threads->setSpecialValueText("Auto");
        bindPref([threads](const Preferences& pr) { threads->setValue(pr.renderThreads); }, [threads](Preferences& pr) { pr.renderThreads = threads->value(); });
        p.spin(threads, "Ma&ximum number of rendering threads:");
        // GPU acceleration: one choice here (the card or not); the API and the way Reno picks are in Reno's own tab
        QComboBox* gpu = new QComboBox(b);
        gpu->setObjectName("gpuAccel");
        gpu->addItem("Off", false);
        gpu->addItem("Optimal", true);
        binds_.push_back({tab_, [gpu] { gpu->setCurrentIndex(vprefs::on("prefs/gpuAccel") ? 1 : 0); },
                          [gpu] { vprefs::set("prefs/gpuAccel", gpu->currentData().toBool()); },
                          [gpu] { gpu->setCurrentIndex(vprefs::defaultOf("prefs/gpuAccel").toBool() ? 1 : 0); }});
        p.beside(gpu, "&GPU acceleration of video processing:");
        QComboBox* numbers = new QComboBox(b);
        numbers->setObjectName("thumbnailNumbers");
        for (auto it : std::vector<std::pair<QString, QString>>{{"None", "none"}, {"Frame Numbers", "frames"}, {"Time", "time"}, {"Timecode", "timecode"}})
            numbers->addItem(it.first, it.second);
        bindPref([numbers](const Preferences& pr) { numbers->setCurrentIndex(std::max(0, numbers->findData(qs(pr.thumbnailNumbers)))); },
                 [numbers](Preferences& pr) { pr.thumbnailNumbers = s8(numbers->currentData().toString()); });
        p.beside(numbers, "Show source frame &numbers on event thumbnails as:");
        QComboBox* thumbs = new QComboBox(b);
        thumbs->setObjectName("videoThumbnails");
        for (auto it : std::vector<std::pair<QString, QString>>{{"All", "all"}, {"Head, Center, Tail", "headCenterTail"}, {"Head, Tail", "headTail"}, {"Head", "head"}, {"None", "none"}})
            thumbs->addItem(it.first, it.second);
        bindPref([thumbs](const Preferences& pr) { thumbs->setCurrentIndex(std::max(0, thumbs->findData(qs(pr.videoThumbnails)))); },
                 [thumbs](Preferences& pr) { pr.videoThumbnails = s8(thumbs->currentData().toString()); });
        p.beside(thumbs, "Thumbnails to show in video &events:");
        QComboBox* proxy4k = new QComboBox(b);
        proxy4k->setObjectName("autoProxy4k");
        proxy4k->addItem("Auto", true);
        proxy4k->addItem("Off", false);
        binds_.push_back({tab_, [proxy4k] { proxy4k->setCurrentIndex(vprefs::on("prefs/autoProxy4k") ? 0 : 1); },
                          [proxy4k] { vprefs::set("prefs/autoProxy4k", proxy4k->currentData().toBool()); },
                          [proxy4k] { proxy4k->setCurrentIndex(vprefs::defaultOf("prefs/autoProxy4k").toBool() ? 0 : 1); }});
        p.beside(proxy4k, "Automatically create video proxies for greater than 4K resolution media:");
        auto safe = [&](const char* name, double hi, int dec, std::function<void(const Preferences&, QDoubleSpinBox*)> load,
                        std::function<void(Preferences&, QDoubleSpinBox*)> save) {
            QDoubleSpinBox* s = new QDoubleSpinBox(b);
            s->setObjectName(name);
            s->setRange(0, hi);
            s->setDecimals(dec);
            bindPref([s, load](const Preferences& pr) { load(pr, s); }, [s, save](Preferences& pr) { save(pr, s); });
            return s;
        };
        p.spin(safe("actionSafe", 45, 1, [](const Preferences& pr, QDoubleSpinBox* s) { s->setValue(pr.actionSafe); },
                    [](Preferences& pr, QDoubleSpinBox* s) { pr.actionSafe = s->value(); }),
               "Action &safe area (%):");
        QSpinBox* cols = new QSpinBox(b);
        cols->setObjectName("gridColumns");
        cols->setRange(1, 64);
        bindPref([cols](const Preferences& pr) { cols->setValue(pr.gridColumns); }, [cols](Preferences& pr) { pr.gridColumns = cols->value(); });
        p.spin(cols, "&Horizontal grid divisions (#):");
        p.spin(safe("titleSafe", 45, 1, [](const Preferences& pr, QDoubleSpinBox* s) { s->setValue(pr.titleSafe); },
                    [](Preferences& pr, QDoubleSpinBox* s) { pr.titleSafe = s->value(); }),
               "&Title safe area (%):");
        QSpinBox* rows = new QSpinBox(b);
        rows->setObjectName("gridRows");
        rows->setRange(1, 64);
        bindPref([rows](const Preferences& pr) { rows->setValue(pr.gridRows); }, [rows](Preferences& pr) { pr.gridRows = rows->value(); });
        p.spin(rows, "&Vertical grid divisions (#):");
        // the preview's own switches: its right-click menu's Scale Video to Fit Preview Window and Simulate Device Aspect Ratio
        QCheckBox* projectSize = p.button(new QCheckBox(b), "Display at project si&ze");
        projectSize->setObjectName("displayAtProjectSize");
        binds_.push_back({tab_, [projectSize] { projectSize->setChecked(!QSettings().value("preview/scaleToFit", true).toBool()); },
                          [projectSize] { QSettings().setValue("preview/scaleToFit", !projectSize->isChecked()); }, [projectSize] { projectSize->setChecked(false); }});
        QCheckBox* simulate = p.button(new QCheckBox(b), "Simulate device aspect &ratio");
        simulate->setObjectName("simulateAspect");
        binds_.push_back({tab_, [simulate] { simulate->setChecked(QSettings().value("preview/simulateAspect", false).toBool()); },
                          [simulate] { QSettings().setValue("preview/simulateAspect", simulate->isChecked()); }, [simulate] { simulate->setChecked(false); }});
        QComboBox* bg = combo(b, "prefs/previewBackground", "previewBackground",
                              {{"Default", "default"}, {"Black", "black"}, {"White", "white"}, {"Checkerboard", "checkerboard"}});
        bg->setToolTip("What the Video Preview shows where the picture is transparent (and round it)");
        p.beside(bg, "Bac&kground color:");
        p.beside(colorButton(b, "prefs/fadeTop", "fadeTop"), "Track fade to&p:", "color");
        p.beside(colorButton(b, "prefs/fadeBottom", "fadeBottom"), "Track fade botto&m:", "color");
        p.finish();

        // Reno's own settings for this tab
        QFormLayout* f = extra("Video");
        f->addRow(note("Frames shown while playing or scrubbing are kept in the Dynamic RAM Preview too, so going back over them plays from memory. 0 turns it off.", renoBody_));
        QComboBox* gpuApi = combo(renoBody_, "prefs/gpuApi", "gpuApi", {{"Vulkan", "vulkan"}, {"OpenGL", "opengl"}, {"Off (CPU only)", "off"}});
        gpuApi->setToolTip("Vulkan: built-in effects with GPU kernels run on the graphics card (falling back to the CPU if there is no usable Vulkan driver). "
                           "OpenGL: only plug-ins that render with OpenGL use the card. Off: everything on the CPU. "
                           "Plug-in GPU use also needs \"GPU acceleration\" above. \"reno_cli vkcheck\" tests Vulkan.");
        f->addRow("GPU &API:", gpuApi);
        QComboBox* gpuPick = combo(renoBody_, "prefs/gpuPick", "gpuPick", {{"The quicker of card and CPU for each effect (measured)", "auto"}, {"Always the card", "gpu"}});
        gpuPick->setToolTip("A frame has to be sent to the card and back, so a simple effect can be quicker on the CPU. Reno times each effect both ways, once for each "
                            "graphics card and driver (behind the splash screen), and uses the quicker. The pictures are the same either way.");
        QPushButton* gpuMeasure = new QPushButton("Measure again", renoBody_);
        gpuMeasure->setObjectName("gpuMeasure");
        gpuMeasure->setToolTip("Times the effects on the CPU and on the card again (in the background; \"reno_cli gpupick\" does the same and prints the times).");
        QObject::connect(gpuMeasure, &QPushButton::clicked, [] { ofx::builtin::pick::startBackground(renoVersion(), true); });
        QWidget* gpuRow = new QWidget(renoBody_);
        QHBoxLayout* gpuRowL = new QHBoxLayout(gpuRow);
        gpuRowL->setContentsMargins(0, 0, 0, 0);
        gpuRowL->addWidget(gpuPick, 1);
        gpuRowL->addWidget(gpuMeasure);
        f->addRow("GPU e&ffects:", gpuRow);
        QComboBox* vegasFx = combo(renoBody_, "prefs/vegasFxSource", "vegasFxSource",
                                   {{"Reno's built-in versions (ignore the installed ones)", "builtin"}, {"VEGAS's own plug-ins when installed", "real"}});
        vegasFx->setToolTip("VEGAS's own effects (Gaussian Blur, Cookie Cutter, ...) are reproduced in Reno to match VEGAS's output. By default Reno uses its "
                            "versions and does not even load VEGAS's installed OpenFX plug-ins (other OpenFX plug-ins are loaded as before). "
                            "The other choice uses VEGAS's plug-ins when VEGAS is installed and they work here. "
                            "Effects already in the project are recreated when this changes. (reno_cli vegasfx compares the two.)");
        f->addRow("VEGAS &effects:", vegasFx);
        QComboBox* vegasEfx = combo(renoBody_, "prefs/vegasEfxSource", "vegasEfxSource",
                                    {{"Reno's built-in versions", "builtin"}, {"The installed plug-ins (need VEGAS's licence)", "real"}});
        vegasEfx->setToolTip("VEGAS's eFX audio effects (Limiter, Compressor, Reverb, TubeStage, Phaser, ChorusFlanger) are MAGIX plug-ins that only run fully inside VEGAS "
                             "(the Limiter, for one, does not limit elsewhere). Reno's built-in versions read the same settings and show the same controls, but sound different. "
                             "Effects already in the project are recreated when this changes.");
        f->addRow("VEGAS &eFX audio effects:", vegasEfx);
        QComboBox* vegasDx = combo(renoBody_, "prefs/vegasDxSource", "vegasDxSource",
                                   {{"Reno's built-in versions (ignore the installed ones)", "builtin"}, {"The installed DirectX plug-ins when they work", "real"}});
        vegasDx->setToolTip("VEGAS's DirectX audio effects (Reverb, Distortion, the EQs, Track Compressor, ...) are reproduced in Reno to sound the same. "
                            "By default Reno uses its versions and ignores the installed DirectX plug-ins. The other choice uses the installed ones on Windows "
                            "when they load, and Reno's version otherwise. Effects already in the project are recreated when this changes.");
        f->addRow("VEGAS &DirectX audio effects:", vegasDx);
        QCheckBox* all = check(renoBody_, "Render every effect at the preview's size in smaller previews", "prefs/previewSizeAllFx", "previewSizeAllFx");
        all->setToolTip("At Draft/Preview quality or a small preview window, effects that cannot render at a smaller size "
                        "(no multi-resolution support, e.g. many third-party plug-ins) are still given the preview's frame, "
                        "as VEGAS does. Much faster; they may look slightly different from the final render (sizes in pixels). "
                        "Off: such effects work at the full project size in previews too. Final renders always use the full size.");
        f->addRow(all);
        f->addRow(note("New video tracks get the fade colours above (a track's own: its header's menu, Fade Colors).", renoBody_));
    }

    void buildPreviewDevice()
    {
        QPushButton* devDefaults = new QPushButton("De&vice Defaults", this);
        devDefaults->setObjectName("deviceDefaults");
        Pg p = vegasPage("Preview Device", "previewDeviceTab", {devDefaults});
        QWidget* b = p.parent();
        QComboBox* dev = combo(b, "prefs/pdDevice", "pdDevice", {{"Windows Graphics Card", "graphics"}, {"(None)", "none"}});
        p.beside(dev, "&Device:");
        QPlainTextEdit* details = new QPlainTextEdit(b);
        details->setObjectName("pdDetails");
        details->setReadOnly(true);
        p.beside(details, "Details:");
        QComboBox* adapter = new QComboBox(b);
        adapter->setObjectName("pdAdapter");
        adapter->addItem("A display other than Reno's", -1);
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (int k = 0; k < screens.size(); ++k)
            adapter->addItem(QString("%1. %2 x %3").arg(k + 1).arg(screens[k]->size().width()).arg(screens[k]->size().height()), k);
        binds_.push_back({tab_, [adapter] { adapter->setCurrentIndex(std::max(0, adapter->findData(vprefs::number("prefs/pdAdapter")))); },
                          [adapter] { vprefs::set("prefs/pdAdapter", adapter->currentData().toInt()); }, [adapter] { adapter->setCurrentIndex(0); }});
        p.beside(adapter, "Display adapter:", "combo");
        QPushButton* identify = p.button(new QPushButton("Identify Displays", b), "Identify Displays");
        identify->setObjectName("identifyDisplays");
        QObject::connect(identify, &QPushButton::clicked, this, [] {
            const QList<QScreen*> all = QGuiApplication::screens();
            for (int k = 0; k < all.size(); ++k) {
                QLabel* l = new QLabel(QString::number(k + 1));
                l->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
                l->setAttribute(Qt::WA_DeleteOnClose);
                l->setAlignment(Qt::AlignCenter);
                l->setStyleSheet("font-size: 120px; font-weight: bold; color: white; background: rgba(0,0,0,170);");
                l->resize(220, 220);
                const QRect g = all[k]->geometry();
                l->move(g.center() - QPoint(110, 110));
                l->show();
                QTimer::singleShot(2000, l, &QWidget::close);
            }
        });
        auto updateDetails = [details, dev] {
            if (dev->currentData().toString() == "none") { details->setPlainText("No external preview device.\n"); return; }
            QString t = "Device connected\n";
            for (QScreen* s : QGuiApplication::screens())
                t += QString("%1: %2 x %3, %4 Hz\n").arg(s->name()).arg(s->size().width()).arg(s->size().height()).arg(s->refreshRate(), 0, 'f', 1);
            details->setPlainText(t);
        };
        QObject::connect(dev, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [updateDetails](int) { updateDetails(); });
        QTimer::singleShot(0, b, updateDetails);
        p.button(check(b, "Scale output to fit display", "prefs/pdScale", "pdScale"), "Scale output to fit display");
        p.button(check(b, "Apply deinterlace filter", "prefs/pdDeinterlace", "pdDeinterlace"), "Apply deinterlace filter");
        p.button(check(b, "Adjust levels from studio RGB to computer RGB", "prefs/pdStudioRgb", "pdStudioRgb"), "Adjust levels from studio RGB to computer RGB");
        p.button(check(b, "Display frames in video preview window during &playback", "prefs/pdShowInPreview", "pdShowInPreview"),
                 "Display frames in video preview window during &playback");
        p.finish();
        extra("Preview Device")->addRow(note("AJA, Blackmagic Design DeckLink and OHCI IEEE 1394 devices are not supported: Reno previews on a display "
                                             "(Tools > Preview on External Monitor, Alt+Shift+4).", renoBody_));
        const int myTab = tab_;
        QObject::connect(devDefaults, &QPushButton::clicked, this, [this, myTab] {
            for (Bind& x : binds_)
                if (x.tab == myTab && x.reset) x.reset();
        });
    }

    void buildAudio()
    {
        Pg p = vegasPage("Audio", "audioTab");
        QWidget* b = p.parent();
        QDoubleSpinBox* peak = new QDoubleSpinBox(b);
        peak->setObjectName("normalizePeakDb");
        peak->setRange(-60, 0);
        peak->setDecimals(1);
        peak->setSingleStep(0.1);
        binds_.push_back({tab_, [peak] { peak->setValue(QSettings().value("prefs/normalizePeakDb", -0.1).toDouble()); },
                          [peak] { QSettings().setValue("prefs/normalizePeakDb", peak->value()); }, [peak] { peak->setValue(-0.1); }});
        p.spin(peak, "Normali&ze peak level (dB):");
        p.button(check(b, "Recor&d to Broadcast Wave Format", "prefs/recordBwf", "recordBwf"), "Recor&d to Broadcast Wave Format");
        p.button(check(b, "Pr&ompt user when no track has been armed for record", "prefs/promptNoArmed", "promptNoArmed"),
                 "Pr&ompt user when no track has been armed for record");
        QLineEdit* editor = new QLineEdit(b);
        editor->setObjectName("audioEditor");
        bindPref([editor](const Preferences& pr) { editor->setText(qs(pr.audioEditor)); }, [editor](Preferences& pr) { pr.audioEditor = s8(editor->text().trimmed()); });
        p.beside(editor, "Preferred audio &editor:");
        wireBrowse(p.button(new QPushButton("&Browse...", b), "&Browse..."), editor, false, "Programs (*.exe);;All files (*)");
        QRadioButton* def = p.button(new QRadioButton("Use default &metronome sounds", b), "Use default &metronome sounds");
        def->setObjectName("metronomeDefault");
        QRadioButton* custom = p.button(new QRadioButton("Use custom metronome so&unds", b), "Use custom metronome so&unds");
        custom->setObjectName("metronomeCustom");
        binds_.push_back({tab_, [def, custom] { (vprefs::on("prefs/metronomeCustom") ? custom : def)->setChecked(true); },
                          [custom] { vprefs::set("prefs/metronomeCustom", custom->isChecked()); }, [def] { def->setChecked(true); }});
        const QString soundFilter = "Sound files (*.wav *.mp3 *.flac *.ogg *.aif *.aiff);;All files (*)";
        QLineEdit* normal = textBox(b, "prefs/metronomeNormal", "metronomeNormal");
        QLineEdit* accent = textBox(b, "prefs/metronomeAccent", "metronomeAccent");
        p.beside(normal, "&Normal:");
        QPushButton* nb = p.button(new QPushButton("B&rowse...", b), "B&rowse...");
        wireBrowse(nb, normal, false, soundFilter);
        p.beside(accent, "A&ccent:");
        QPushButton* ab = p.button(new QPushButton("Br&owse...", b), "Br&owse...");
        wireBrowse(ab, accent, false, soundFilter);
        auto enable = [custom, normal, accent, nb, ab] {
            for (QWidget* w : std::vector<QWidget*>{normal, accent, nb, ab}) w->setEnabled(custom->isChecked());
        };
        QObject::connect(custom, &QRadioButton::toggled, b, [enable](bool) { enable(); });
        QTimer::singleShot(0, b, enable);
        p.button(check(b, "Open editor when new loudness log is generated", "prefs/loudnessOpenEditor", "loudnessOpenEditor"),
                 "Open editor when new loudness log is generated");
        p.finish();

        QFormLayout* f = extra("Audio");
        QComboBox* mc = new QComboBox(renoBody_);
        mc->setObjectName("multichannelImport");
        for (auto it : std::vector<std::pair<QString, QString>>{{"One mono event per channel, each on its own track", "tracks"},
                                                                {"One stereo event per pair of channels (group multichannel stereo pairs)", "pairs"},
                                                                {"One event, all its channels mixed down to stereo", "mixdown"}})
            mc->addItem(it.first, it.second);
        bindPref([mc](const Preferences& pr) { mc->setCurrentIndex(std::max(0, mc->findData(qs(pr.multichannelImport)))); },
                 [mc](Preferences& pr) { pr.multichannelImport = s8(mc->currentData().toString()); });
        f->addRow("Add multichannel audio as:", mc);
        QCheckBox* pan = new QCheckBox("Set pan for new tracks from multichannel", renoBody_);
        pan->setObjectName("panMultichannelTracks");
        bindPref([pan](const Preferences& pr) { pan->setChecked(pr.panMultichannelTracks); }, [pan](Preferences& pr) { pr.panMultichannelTracks = pan->isChecked(); });
        f->addRow(pan);
    }

    void buildAudioDevice()
    {
        QPushButton* adv = new QPushButton("Ad&vanced...", this);
        adv->setObjectName("audioAdvanced");
        Pg p = vegasPage("Audio Device", "audioDeviceTab", {adv});
        QWidget* b = p.parent();
        QComboBox* device = new QComboBox(b);
        device->setObjectName("audioDevice");
        device->addItem("The system's default device", QString());
#ifdef RENO_HAVE_QTMULTIMEDIA
        for (const QAudioDevice& d : QMediaDevices::audioOutputs()) device->addItem(d.description(), QString::fromUtf8(d.id()));
#else
        device->setEnabled(false);
#endif
        bindPref([device](const Preferences& pr) {
                     int i = device->findData(qs(pr.audioDevice));
                     if (i < 0 && !pr.audioDevice.empty()) {
                         device->addItem(QString("(not connected) %1").arg(qs(pr.audioDevice)), qs(pr.audioDevice));
                         i = device->count() - 1;
                     }
                     device->setCurrentIndex(std::max(0, i));
                 },
                 [device](Preferences& pr) { pr.audioDevice = s8(device->currentData().toString()); });
        p.beside(device, "&Default stereo and front playback device:");
        QSlider* buffer = trackbar("playbackBuffer", 2, 100);
        binds_.push_back({tab_, [buffer] { buffer->setValue((int)std::lround(vprefs::real("prefs/playbackBuffer") * 100)); },
                          [buffer] { vprefs::set("prefs/playbackBuffer", buffer->value() / 100.0); },
                          [buffer] { buffer->setValue((int)std::lround(vprefs::defaultOf("prefs/playbackBuffer").toDouble() * 100)); }});
        buffer->setToolTip("How much sound is made ready before it is heard: larger stops gaps, smaller answers sooner. Used the next time playback starts.");
        p.beside(buffer, "&Playback buffering (seconds):");
        QLabel* bufferValue = new QLabel(b);
        p.put(bufferValue, p.after(p.cellOfLabel("&Playback buffering (seconds):"), "label"));
        QObject::connect(buffer, &QSlider::valueChanged, bufferValue, [bufferValue](int v) { bufferValue->setText(QString::number(v / 100.0, 'f', 2)); });
        QComboBox* rec = new QComboBox(b);
        rec->setObjectName("audioInputDevice");
        rec->addItem("The system's default device", QString());
#ifdef RENO_HAVE_QTMULTIMEDIA
        for (const QAudioDevice& d : QMediaDevices::audioInputs()) rec->addItem(d.description(), QString::fromUtf8(d.id()));
#endif
        bindPref([rec](const Preferences& pr) { rec->setCurrentIndex(std::max(0, rec->findData(qs(pr.audioInputDevice)))); },
                 [rec](Preferences& pr) { pr.audioInputDevice = s8(rec->currentData().toString()); });
        p.beside(rec, "Default audi&o recording device:");
        QCheckBox* autoLat = p.button(check(b, "Au&tomatically detect and offset for hardware recording latency", "prefs/autoRecordLatency", "autoRecordLatency"),
                                      "Au&tomatically detect and offset for hardware recording latency");
        QSlider* offset = trackbar("recordOffsetMs", -1000, 1000);
        binds_.push_back({tab_, [offset] { offset->setValue((int)std::lround(vprefs::real("prefs/recordOffsetMs"))); },
                          [offset] { vprefs::set("prefs/recordOffsetMs", (double)offset->value()); },
                          [offset] { offset->setValue((int)std::lround(vprefs::defaultOf("prefs/recordOffsetMs").toDouble())); }});
        offset->setToolTip("Recordings are placed this much earlier on the timeline (used when the box above is cleared)");
        p.beside(offset, "Us&er offset (samples) (ms):");
        QLabel* offsetValue = new QLabel(b);
        p.put(offsetValue, p.after(p.cellOfLabel("Us&er offset (samples) (ms):"), "label"));
        QObject::connect(offset, &QSlider::valueChanged, offsetValue, [offsetValue](int v) { offsetValue->setText(QString("%1 ms").arg(v)); });
        QObject::connect(autoLat, &QCheckBox::toggled, b, [offset](bool on) { offset->setEnabled(!on); });
        QTimer::singleShot(0, b, [offset, autoLat] { offset->setEnabled(!autoLat->isChecked()); });
        p.finish();

        QFormLayout* f = extra("Audio Device");
        QLabel* channels = new QLabel(renoBody_);
        channels->setObjectName("deviceChannels");
        f->addRow("Playback channels:", channels);
        QCheckBox* downmix = new QCheckBox("Downmix 5.1 output to stereo", renoBody_);
        downmix->setObjectName("downmixOutput");
        bindPref([downmix](const Preferences& pr) { downmix->setChecked(pr.downmixOutput); }, [downmix](Preferences& pr) { pr.downmixOutput = downmix->isChecked(); });
        f->addRow(QString(), downmix);
        auto showChannels = [device, downmix, channels] {
            const int n = audioOutputChannels(s8(device->currentData().toString()));
            QString t = n <= 0 ? QString("unknown") : QString::number(n);
            if (n >= 6) t += downmix->isChecked() ? " - 5.1 projects are folded down to stereo (Downmix)" : " - 5.1 projects play in 5.1 (L R C LFE Ls Rs)";
            else if (n > 0) t += " - 5.1 projects are folded down to stereo";
            channels->setText(t);
        };
        QObject::connect(device, QOverload<int>::of(&QComboBox::currentIndexChanged), renoBody_, [showChannels](int) { showChannels(); });
        QObject::connect(downmix, &QCheckBox::toggled, renoBody_, [showChannels](bool) { showChannels(); });
        QTimer::singleShot(0, renoBody_, showChannels);
        QObject::connect(adv, &QPushButton::clicked, this, [this, device] { advancedAudioConfiguration(this, device->currentText(), device->currentData().toString()); });
    }

    void buildMidi()
    {
        Pg p = vegasPage("MIDI", "midiTab");
        QWidget* b = p.parent();
        auto devList = [this, b, &p](const QString& label, const char* name, const std::vector<std::string>& names, const QString& key) {
            QListWidget* l = new QListWidget(b);
            l->setObjectName(name);
            for (const std::string& n : names) {
                QListWidgetItem* it = new QListWidgetItem(qs(n), l);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            }
            if (names.empty()) {
                QListWidgetItem* it = new QListWidgetItem("(no MIDI devices)", l);
                it->setFlags(Qt::NoItemFlags);
            }
            p.beside(l, label, "listview");
            auto load = [l](const QStringList& on) {
                for (int k = 0; k < l->count(); ++k)
                    if (l->item(k)->flags() & Qt::ItemIsUserCheckable) l->item(k)->setCheckState(on.isEmpty() || on.contains(l->item(k)->text()) ? Qt::Checked : Qt::Unchecked);
            };
            binds_.push_back({tab_, [load, key] { load(vprefs::list(key)); },
                              [l, key] {
                                  QStringList on;
                                  bool all = true;
                                  for (int k = 0; k < l->count(); ++k) {
                                      if (!(l->item(k)->flags() & Qt::ItemIsUserCheckable)) continue;
                                      if (l->item(k)->checkState() == Qt::Checked) on << l->item(k)->text();
                                      else all = false;
                                  }
                                  vprefs::set(key, all ? QStringList() : (on.isEmpty() ? QStringList() << "(none)" : on));
                              },
                              [load] { load(QStringList()); }});
        };
        devList("&Make these devices available for MIDI output:", "midiOutputs", midiOutputNames(), "prefs/midiOutputs");
        devList("Ma&ke these devices available for MIDI input:", "midiInputs", midiInputNames(), "prefs/midiInputs");
        p.finish();
        extra("MIDI")->addRow(note("Reno uses MIDI for timecode and clock (Options > Timecode, and the Sync tab), and has no control surfaces or "
                                   "soft synths.", renoBody_));
    }

    void buildVst()
    {
        Pg p = vegasPage("VST Effects", "vstTab");
        QWidget* b = p.parent();
        QLineEdit* def = new QLineEdit(b);
        def->setObjectName("vstDefaultFolder");
        def->setReadOnly(true);
#ifdef _WIN32
        def->setText("C:\\Program Files\\Common Files\\VST3; C:\\Program Files\\VSTPlugins; C:\\Program Files\\Steinberg\\VSTPlugins");
#else
        def->setText("~/.vst; ~/.vst3; /usr/lib/vst; /usr/lib/vst3");
#endif
        def->setToolTip("The folders Reno always looks in");
        p.beside(def, "&Default VST search folder:");
        alt1_ = new QLineEdit(b);
        alt1_->setObjectName("vstFolder1");
        alt2_ = new QLineEdit(b);
        alt2_->setObjectName("vstFolder2");
        p.beside(alt1_, "Alternate VST search folder &1:");
        wireBrowse(p.button(new QPushButton("Br&owse...", b), "Br&owse..."), alt1_, true);
        p.beside(alt2_, "Alternate VST search folder &2:");
        wireBrowse(p.button(new QPushButton("Bro&wse...", b), "Bro&wse..."), alt2_, true);
        QTreeWidget* list = new QTreeWidget(b);
        list->setObjectName("vstList");
        list->setHeaderLabels({"Name", "File"});
        list->setRootIsDecorated(false);
        for (const AudioFxEntry& e : Catalog::get().audio()) {
            if (e.desc.format != "VST" && e.desc.format != "VST3" && e.desc.format != "VST2") continue;
            QTreeWidgetItem* it = new QTreeWidgetItem(list, {qs(e.desc.name), QDir::toNativeSeparators(qs(e.desc.path))});
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setData(0, Qt::UserRole, qs(e.desc.path));
            if (!e.ok) it->setToolTip(0, "This plug-in did not work when it was checked: " + qs(e.error));
        }
        list->header()->resizeSection(0, 180);
        p.beside(list, "Select VST &effects to be available as audio plug-ins:");
        QPushButton* refresh = p.button(new QPushButton("&Refresh", b), "&Refresh");
        refresh->setObjectName("vstRefresh");
        QObject::connect(refresh, &QPushButton::clicked, this, [this] { applyAll(); runPluginScan(this, false); });
        binds_.push_back({tab_,
                          [list] {
                              const QStringList off = vprefs::list("prefs/vstDisabled");
                              for (int k = 0; k < list->topLevelItemCount(); ++k)
                                  list->topLevelItem(k)->setCheckState(0, off.contains(list->topLevelItem(k)->data(0, Qt::UserRole).toString()) ? Qt::Unchecked : Qt::Checked);
                          },
                          [list] {
                              QStringList off;
                              for (int k = 0; k < list->topLevelItemCount(); ++k)
                                  if (list->topLevelItem(k)->checkState(0) != Qt::Checked) off << list->topLevelItem(k)->data(0, Qt::UserRole).toString();
                              vprefs::set("prefs/vstDisabled", off);
                          },
                          [list] {
                              for (int k = 0; k < list->topLevelItemCount(); ++k) list->topLevelItem(k)->setCheckState(0, Qt::Checked);
                          }});
        p.finish();
        QFormLayout* f = extra("VST Effects");
        if (list->topLevelItemCount() == 0) f->addRow(note("No VST plug-ins have been found (Refresh looks again).", renoBody_));
        f->addRow(note("Cleared plug-ins are left out of the effect lists (projects that use them still load them). New folders are "
                       "searched by Refresh.", renoBody_));
    }

    void buildEditing()
    {
        Pg p = vegasPage("Editing", "editingTab");
        QWidget* b = p.parent();
        auto boolBox = [&](const QString& text, const char* name, bool Preferences::*field) {
            QCheckBox* c = p.button(new QCheckBox(b), text);
            c->setObjectName(name);
            bindPref([c, field](const Preferences& pr) { c->setChecked(pr.*field); }, [c, field](Preferences& pr) { pr.*field = c->isChecked(); });
            return c;
        };
        boolBox("Enable &looping on events by default", "loopEvents", &Preferences::loopEvents);
        boolBox("&Preserve pitch when stretching audio events", "preservePitch", &Preferences::preservePitch);
        p.button(check(b, "Cut, copy, and delete g&rouped events", "prefs/groupedCutCopy", "groupedCutCopy"), "Cut, copy, and delete g&rouped events");
        boolBox("Do &not quantize to frames for audio-only edits", "noQuantizeAudio", &Preferences::noQuantizeAudioOnly);
        p.button(check(b, "&Collapse loop region when no time selection is present", "prefs/collapseLoop", "collapseLoop"),
                 "&Collapse loop region when no time selection is present");
        p.beside(combo(b, "prefs/jklSpeed", "jklSpeed", {{"Slow", "slow"}, {"Medium", "medium"}, {"Fast", "fast"}}), "&JKL / shuttle speed:");
        QDoubleSpinBox* qf = new QDoubleSpinBox(b);
        qf->setObjectName("quickFadeMs");
        qf->setRange(1, 1000);
        qf->setDecimals(0);
        bindPref([qf](const Preferences& pr) { qf->setValue(pr.quickFadeMs); }, [qf](Preferences& pr) { pr.quickFadeMs = qf->value(); });
        p.spin(qf, "&Quick fade length for audio events (ms):");
        auto lengthBox = [&](const char* name, double lo, double hi, const QString& label, std::function<void(const Preferences&, QDoubleSpinBox*)> load,
                             std::function<void(Preferences&, QDoubleSpinBox*)> save) {
            QDoubleSpinBox* s = new QDoubleSpinBox(b);
            s->setObjectName(name);
            s->setRange(lo, hi);
            s->setDecimals(3);
            bindPref([s, load](const Preferences& pr) { load(pr, s); }, [s, save](Preferences& pr) { save(pr, s); });
            p.spin(s, label);
            QLabel* fr = new QLabel(b);
            p.put(fr, p.after(p.cellOfLabel(label), "label"));
            framesLabel(fr, s);
            return s;
        };
        lengthBox("stillLength", 0.1, 3600, "New &still image length (seconds):", [](const Preferences& pr, QDoubleSpinBox* s) { s->setValue(pr.stillLength); },
                  [](Preferences& pr, QDoubleSpinBox* s) { pr.stillLength = s->value(); });
        lengthBox("cursorPreview", 0.1, 60, "Cursor preview &duration (seconds):", [](const Preferences& pr, QDoubleSpinBox* s) { s->setValue(pr.cursorPreview); },
                  [](Preferences& pr, QDoubleSpinBox* s) { pr.cursorPreview = s->value(); });
        boolBox("Automatically &overlap multiple selected media when added", "autoOverlapMedia", &Preferences::autoOverlapMedia);
        lengthBox("cutToOverlap", 0.01, 60, "Cu&t-to-overlap conversion (seconds):", [](const Preferences& pr, QDoubleSpinBox* s) { s->setValue(pr.cutToOverlap); },
                  [](Preferences& pr, QDoubleSpinBox* s) { pr.cutToOverlap = s->value(); });
        p.beside(combo(b, "prefs/overlapAlign", "overlapAlign", {{"Before cut", "before"}, {"Centered on cut", "center"}, {"After cut", "after"}}), "Align&ment:");
        // Time selection envelope fades: the check box, its length in milliseconds (with the frames it comes to) and the alignment
        boolBox("T&ime selection envelope fades (ms):", "timeSelEnvFades", &Preferences::timeSelEnvFades);
        QDoubleSpinBox* len = new QDoubleSpinBox(b);
        len->setObjectName("timeSelEnvFadeLength");
        len->setRange(0, 60000);
        len->setDecimals(0);
        p.edit(len, p.after(p.find("check", "T&ime selection envelope fades (ms):"), "edit"));
        QLabel* lenFrames = new QLabel(b);
        p.put(lenFrames, p.after(p.find("check", "T&ime selection envelope fades (ms):"), "label"));
        {
            const double fps = std::max(1.0, AppState::get().project().settings.fps());
            QObject::connect(len, QOverload<double>::of(&QDoubleSpinBox::valueChanged), lenFrames, [lenFrames, fps](double v) {
                lenFrames->setText(QString("%1 frames at %2 fps").arg((int)std::lround(v / 1000.0 * fps)).arg(fps, 0, 'f', 3));
            });
        }
        QComboBox* align = new QComboBox(b);
        align->setObjectName("timeSelEnvFadeAlign");
        align->addItem("Outside", "outside");
        align->addItem("Centered", "centered");
        align->addItem("Inside", "inside");
        p.beside(align, "Alignm&ent:");
        bindPref([len, align](const Preferences& pr) {
            len->setValue(pr.timeSelEnvFadeLength * 1000.0);
            const int i = align->findData(QString::fromStdString(pr.timeSelEnvFadeAlign));
            align->setCurrentIndex(i >= 0 ? i : 0);
        }, [len, align](Preferences& pr) {
            pr.timeSelEnvFadeLength = len->value() / 1000.0;
            pr.timeSelEnvFadeAlign = align->currentData().toString().toStdString();
        });
        p.button(check(b, "A&utomatically crop still images added to timeline", "prefs/autoCropStills", "autoCropStills"),
                 "A&utomatically crop still images added to timeline");
        QSpinBox* smart = integer(b, "prefs/smartSplitFrames", "smartSplitFrames", 1, 1000);
        p.spin(smart, "Smart split default length (frames):");
        p.finish();

        QFormLayout* f = extra("Editing");
        QCheckBox* rm = new QCheckBox("Use the right mouse button to switch to the Selection Edit tool", renoBody_);
        rm->setObjectName("rightMouseSelect");
        bindPref([rm](const Preferences& pr) { rm->setChecked(pr.rightMouseSelect); }, [rm](Preferences& pr) { pr.rightMouseSelect = rm->isChecked(); });
        f->addRow(rm);
    }

    void buildDisplay()
    {
        Pg p = vegasPage("Display", "displayTab");
        QWidget* b = p.parent();
        trackColors_.clear();
        const QStringList savedTracks = vprefs::list("prefs/trackColors");
        for (int k = 0; k < 8; ++k) trackColors_.push_back(k < savedTracks.size() && QColor(savedTracks[k]).isValid() ? QColor(savedTracks[k]) : trackDefaultColor(k));
        oldTrackColors_ = trackColors_;
        QComboBox* track = new QComboBox(b);
        track->setObjectName("trackColorIndex");
        for (int k = 0; k < 8; ++k) track->addItem(QString("Track Color %1").arg(k + 1));
        ColorButton* tc = new ColorButton(b, "trackColor");
        p.beside(track, "&Track:");
        p.put(tc, p.after(p.cellOfLabel("&Track:"), "color"));
        QObject::connect(track, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [this, tc](int i) {
            if (i >= 0 && i < (int)trackColors_.size()) tc->setColor(trackColors_[(size_t)i]);
        });
        tc->onChanged = [this, track](const QColor& c) {
            const int i = track->currentIndex();
            if (i >= 0 && i < (int)trackColors_.size()) trackColors_[(size_t)i] = c;
        };
        binds_.push_back({tab_, [tc, this] { tc->setColor(trackColors_[0]); },
                          [this] {
                              QStringList l;
                              bool def = true;
                              for (int k = 0; k < 8; ++k) {
                                  l << trackColors_[(size_t)k].name();
                                  if (trackColors_[(size_t)k] != trackDefaultColor(k)) def = false;
                              }
                              vprefs::set("prefs/trackColors", def ? QStringList() : l);
                          },
                          [this, track, tc] {
                              for (int k = 0; k < 8; ++k) trackColors_[(size_t)k] = trackDefaultColor(k);
                              tc->setColor(trackColors_[(size_t)std::max(0, track->currentIndex())]);
                          }});
        envColors_.clear();
        for (const QString& s : vprefs::list("prefs/envelopeColors")) {
            const int eq = s.indexOf('=');
            if (eq > 0 && QColor(s.mid(eq + 1)).isValid()) envColors_[s.left(eq)] = QColor(s.mid(eq + 1));
        }
        QComboBox* env = new QComboBox(b);
        env->setObjectName("envelopeType");
        for (const auto& k : kEnvelopeKinds) env->addItem(k.first, k.second);
        ColorButton* ec = new ColorButton(b, "envelopeColor");
        p.beside(env, "En&velope Type:");
        p.put(ec, p.after(p.cellOfLabel("En&velope Type:"), "color"));
        auto envColorOf = [this](const QString& kind) { return envColors_.count(kind) ? envColors_[kind] : envelopeDefaultColor(kind); };
        QObject::connect(env, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [env, ec, envColorOf](int) { ec->setColor(envColorOf(env->currentData().toString())); });
        ec->onChanged = [this, env](const QColor& c) {
            const QString kind = env->currentData().toString();
            if (c == envelopeDefaultColor(kind)) envColors_.erase(kind);
            else envColors_[kind] = c;
        };
        binds_.push_back({tab_, [env, ec, envColorOf] { ec->setColor(envColorOf(env->currentData().toString())); },
                          [this] {
                              QStringList l;
                              for (const auto& kv : envColors_) l << kv.first + "=" + kv.second.name();
                              vprefs::set("prefs/envelopeColors", l);
                          },
                          [this, env, ec] {
                              envColors_.clear();
                              ec->setColor(envelopeDefaultColor(env->currentData().toString()));
                          }});
        QComboBox* snap = new QComboBox(b);
        snap->setObjectName("snapType");
        snap->addItem("Snap Indicator", "indicator");
        ColorButton* sc = new ColorButton(b, "snapColor");
        p.beside(snap, "&Snap Type:");
        p.put(sc, p.after(p.cellOfLabel("&Snap Type:"), "color"));
        binds_.push_back({tab_,
                          [sc] {
                              QColor c(70, 170, 255);
                              for (const QString& s : vprefs::list("prefs/snapColors"))
                                  if (s.startsWith("indicator=")) c = QColor(s.mid(10));
                              sc->setColor(c);
                          },
                          [sc] {
                              vprefs::set("prefs/snapColors", sc->color() == QColor(70, 170, 255) ? QStringList() : QStringList() << "indicator=" + sc->color().name());
                          },
                          [sc] { sc->setColor(QColor(70, 170, 255)); }});
        auto box = [&](const QString& text, const char* key, const char* name) { p.button(check(b, text, key, name), text); };
        box("&Display timeline at bottom of main window", "prefs/timelineAtBottom", "timelineAtBottom");
        box("&Position tabs at top of docked windows", "prefs/tabsAtTop", "tabsAtTop");
        box("&Allow floating windows to dock (hold Ctrl to override)", "prefs/floatingDock", "floatingDock");
        box("&Reverse Event Background Color", "prefs/reverseEventBg", "reverseEventBg");
        box("&Use High DPI scaling (where available)", "prefs/highDpi", "highDpi");
        box("Use Alternate HighDPI Settings", "prefs/altHighDpi", "altHighDpi");
        p.finish();
        extra("Display")->addRow(note("The High DPI settings are read when Reno starts.", renoBody_));
    }

    // CD Settings: Reno has no CD burning, so the page only shows VEGAS's controls.
    void buildCd()
    {
        Pg p = vegasPage("CD Settings", "cdTab");
        p.finish();
    }

    void buildSync()
    {
        QPushButton* adv = new QPushButton("Ad&vanced...", this);
        adv->setObjectName("syncAdvanced");
        Pg p = vegasPage("Sync", "syncTab", {adv});
        QWidget* b = p.parent();
        const std::vector<std::string> outs = midiOutputNames(), ins = midiInputNames();
        auto devCombo = [b](const char* name, const std::vector<std::string>& names) {
            QComboBox* c = new QComboBox(b);
            c->setObjectName(name);
            if (names.empty()) c->addItem("(no MIDI devices)", -1);
            for (size_t k = 0; k < names.size(); ++k) c->addItem(qs(names[k]), (int)k);
            return c;
        };
        const std::vector<std::pair<QString, int>> rates = {{"Use project frame rate", -1}, {"SMPTE Film Sync (24 fps)", 0}, {"SMPTE EBU (25 fps, Video)", 1},
                                                            {"SMPTE Drop (29.97 fps, Video)", 2}, {"SMPTE 30 (30 fps, Audio)", 3}};
        auto rateCombo = [b, &rates](const char* name, const QString& key, std::vector<Bind>& binds, int tab) {
            QComboBox* c = new QComboBox(b);
            c->setObjectName(name);
            for (const auto& r : rates) c->addItem(r.first, r.second);
            binds.push_back({tab, [c, key] { c->setCurrentIndex(std::max(0, c->findData(vprefs::number(key)))); },
                             [c, key] { vprefs::set(key, c->currentData().toInt()); }, [c] { c->setCurrentIndex(0); }});
            return c;
        };
        QComboBox* mtcOut = devCombo("mtcOutputDevice", outs);
        p.beside(mtcOut, "&Output device:");
        p.beside(rateCombo("mtcRate", "midi/mtcRate", binds_, tab_), "F&rame rate:");
        QComboBox* clockOut = devCombo("clockOutputDevice", outs);
        clockOut->setToolTip("Reno sends MIDI timecode and MIDI clock through one port: choosing one chooses both");
        p.beside(clockOut, "O&utput device:");
        QComboBox* mtcIn = devCombo("mtcInputDevice", ins);
        p.beside(mtcIn, "&Input device:");
        p.beside(rateCombo("chaseRate", "midi/chaseRate", binds_, tab_), "Fra&me rate:");
        p.finish();
        QObject::connect(mtcOut, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [mtcOut, clockOut](int i) {
            if (clockOut->currentIndex() != i) clockOut->setCurrentIndex(i);
            (void)mtcOut;
        });
        QObject::connect(clockOut, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [mtcOut](int i) {
            if (mtcOut->currentIndex() != i) mtcOut->setCurrentIndex(i);
        });
        binds_.push_back({tab_,
                          [mtcOut, mtcIn] {
                              QSettings st;
                              mtcOut->setCurrentIndex(std::max(0, mtcOut->findData(st.value("midi/output", 0).toInt())));
                              mtcIn->setCurrentIndex(std::max(0, mtcIn->findData(st.value("midi/input", 0).toInt())));
                          },
                          [mtcOut, mtcIn] {
                              QSettings st;
                              if (mtcOut->currentData().toInt() >= 0) st.setValue("midi/output", mtcOut->currentData().toInt());
                              if (mtcIn->currentData().toInt() >= 0) st.setValue("midi/input", mtcIn->currentData().toInt());
                          },
                          [mtcOut, mtcIn] {
                              mtcOut->setCurrentIndex(0);
                              mtcIn->setCurrentIndex(0);
                          }});
        extra("Sync")->addRow(note("Options > Timecode turns them on (F7, Shift+F7, Ctrl+F7).", renoBody_));
        QObject::connect(adv, &QPushButton::clicked, this, [this] { syncAdvanced(); });
    }

    // VEGAS's three Advanced dialogs of the Sync tab (MTC Input, MTC Output, MIDI Clock Output) as one window.
    void syncAdvanced()
    {
        QDialog d(this);
        d.setObjectName("syncAdvancedDialog");
        d.setWindowTitle("Advanced Sync Preferences");
        QVBoxLayout* v = new QVBoxLayout(&d);
        QGroupBox* in = new QGroupBox("MTC Input", &d);
        QFormLayout* fi = new QFormLayout(in);
        QCheckBox* fw = new QCheckBox("Free-&wheel for timecode loss", in);
        fw->setChecked(vprefs::on("midi/freewheel"));
        fi->addRow(fw);
        auto spin = [](QWidget* p, double v, double lo, double hi, int dec) {
            QDoubleSpinBox* s = new QDoubleSpinBox(p);
            s->setRange(lo, hi);
            s->setDecimals(dec);
            s->setValue(v);
            return s;
        };
        QDoubleSpinBox* slack = spin(in, vprefs::real("midi/freewheelSlack"), 0, 10, 2);
        fi->addRow("Free-wheel &slack time (seconds):", slack);
        QDoubleSpinBox* fwp = spin(in, vprefs::real("midi/freewheelPlayback"), 0, 60, 2);
        fi->addRow("Free-wheel &playback time (seconds):", fwp);
        QDoubleSpinBox* delay = spin(in, vprefs::real("midi/syncDelay"), 0, 10, 2);
        fi->addRow("Synchronization &delay time (seconds):", delay);
        QSpinBox* offset = new QSpinBox(in);
        offset->setRange(-400, 400);
        offset->setValue(vprefs::number("midi/offsetQuarterFrames"));
        fi->addRow("&Offset adjust (quarter frames):", offset);
        QCheckBox* trig = new QCheckBox("&Trigger playback only (do not chase)", in);
        trig->setChecked(vprefs::on("midi/triggerOnly"));
        fi->addRow(trig);
        v->addWidget(in);
        auto radios = [&d](QGroupBox* g, const QString& label, const QString& key) {
            QVBoxLayout* l = new QVBoxLayout(g);
            l->addWidget(new QLabel(label, g));
            std::vector<QRadioButton*> rs;
            for (auto it : std::vector<std::pair<QString, QString>>{{"Never", "never"}, {"On start and stop of playback and record", "startStop"},
                                                                    {"For all cursor postion changes", "all"}}) {
                QRadioButton* r = new QRadioButton(it.first, g);
                r->setProperty("value", it.second);
                r->setChecked(vprefs::text(key) == it.second);
                l->addWidget(r);
                rs.push_back(r);
            }
            (void)d;
            return rs;
        };
        QGroupBox* out = new QGroupBox("MTC Output", &d);
        std::vector<QRadioButton*> ff = radios(out, "&Full-frame message generation:", "midi/fullFrame");
        v->addWidget(out);
        QGroupBox* clk = new QGroupBox("MIDI Clock Output", &d);
        QCheckBox* start = new QCheckBox("&Send Start instead of Continue when beginning playback", clk);
        start->setChecked(vprefs::on("midi/clockStart"));
        std::vector<QRadioButton*> spp = radios(clk, "Song &Position Pointer generation:", "midi/songPosition");
        static_cast<QVBoxLayout*>(clk->layout())->insertWidget(0, start);
        v->addWidget(clk);
        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        QPushButton* ok = new QPushButton("OK", &d);
        ok->setDefault(true);
        QPushButton* cancel = new QPushButton("Cancel", &d);
        buttons->addWidget(ok);
        buttons->addWidget(cancel);
        v->addLayout(buttons);
        QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
        QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
        if (d.exec() != QDialog::Accepted) return;
        vprefs::set("midi/freewheel", fw->isChecked());
        vprefs::set("midi/freewheelSlack", slack->value());
        vprefs::set("midi/freewheelPlayback", fwp->value());
        vprefs::set("midi/syncDelay", delay->value());
        vprefs::set("midi/offsetQuarterFrames", offset->value());
        vprefs::set("midi/triggerOnly", trig->isChecked());
        for (QRadioButton* r : ff)
            if (r->isChecked()) vprefs::set("midi/fullFrame", r->property("value").toString());
        vprefs::set("midi/clockStart", start->isChecked());
        for (QRadioButton* r : spp)
            if (r->isChecked()) vprefs::set("midi/songPosition", r->property("value").toString());
    }

    void buildExternal()
    {
        Pg p = vegasPage("External Control && Automation", "externalTab");
        QWidget* b = p.parent();
        QCheckBox* thin = p.button(new QCheckBox(b), "Smooth and &thin automation data after recording or drawing");
        thin->setObjectName("thinAutomation");
        bindPref([thin](const Preferences& pr) { thin->setChecked(pr.thinAutomation); }, [thin](Preferences& pr) { pr.thinAutomation = thin->isChecked(); });
        p.beside(combo(b, "prefs/curveAudioEnv", "curveAudioEnv", kCurves), "New &audio envelopes:");
        p.beside(combo(b, "prefs/curveAudioGain", "curveAudioGain", kCurves), "New aud&io event gain:");
        p.beside(combo(b, "prefs/curveAudioFx", "curveAudioFx", kCurves), "New audio F&X automation envelopes:");
        p.beside(combo(b, "prefs/curveVideoEnv", "curveVideoEnv", kCurves), "New vide&o envelopes:");
        p.beside(combo(b, "prefs/curveVideoOpacity", "curveVideoOpacity", kCurves), "New video eve&nt opacity:");
        p.beside(combo(b, "prefs/curveVideoFx", "curveVideoFx", kKeyframeTypes), "New video FX &keyframes:");
        p.beside(combo(b, "prefs/curveVideoMotion", "curveVideoMotion", kKeyframeTypes), "New video &motion keyframes:");
        p.spin(real(b, "prefs/motionSmoothness", "motionSmoothness", 0, 100, 1, 1), "Track Motion d&efault smoothness:");
        p.spin(real(b, "prefs/panCropSmoothness", "panCropSmoothness", 0, 100, 1, 1), "Pan/Crop defa&ult smoothness:");
        p.finish();
        extra("External Control && Automation")->addRow(note("Reno supports no control surfaces (Mackie Control, TranzPort, FaderPort, generic MIDI).", renoBody_));
    }

    void buildDeprecated()
    {
        Pg p = vegasPage("Deprecated Features", "deprecatedTab");
        QWidget* b = p.parent();
        QListWidget* list = new QListWidget(b);
        list->setObjectName("deprecatedList");
        const QString kept = "Reno has no such feature; the setting only shows the menu item VEGAS shows for it";
        for (auto it : std::vector<std::pair<const char*, const char*>>{{"Enable XDCam Explorer", "prefs/depXdcamExplorer"},
                                                                       {"Enable Import DVD Camcorder Disc", "prefs/depDvdCamcorder"}}) {
            QListWidgetItem* li = new QListWidgetItem(it.first, list);
            li->setFlags(li->flags() | Qt::ItemIsUserCheckable);
            li->setToolTip("Kept: " + kept + ".");
            const QString key = it.second;
            binds_.push_back({tab_, [li, key] { li->setCheckState(vprefs::on(key) ? Qt::Checked : Qt::Unchecked); },
                              [li, key] { vprefs::set(key, li->checkState() == Qt::Checked); },
                              [li, key] { li->setCheckState(vprefs::defaultOf(key).toBool() ? Qt::Checked : Qt::Unchecked); }});
        }
        p.beside(list, "&Deprecated Features:");
        p.finish();
    }

    void buildFileIo()
    {
        Pg p = vegasPage("File I/O", "fileIoTab");
        QWidget* b = p.parent();
        const std::vector<std::pair<QString, QString>> levels = {{"Off", "off"}, {"Basic", "basic"}, {"Detailed", "detailed"}};
        QListWidget* options = new QListWidget(b);
        options->setObjectName("fileIoList");
        p.beside(options, "&File I/O options:");
        // The hardware decoder to use: Reno's choice of graphics API (on the Reno tab: whether to decode on the card at all)
        std::vector<std::pair<QString, QString>> apis = {{"Auto", ""}};
        for (const std::string& api : reno::hardwareDecodeApis()) {
            const QString n = QString::fromStdString(api);
            const QString pretty = n == "d3d11va" ? "Direct3D 11 (any graphics card)" : n == "dxva2" ? "DXVA2 (any graphics card)" :
                                   n == "cuda" ? "NVIDIA NVDEC (CUDA)" : n == "vaapi" ? "VA-API" : n == "vdpau" ? "VDPAU" :
                                   n == "videotoolbox" ? "VideoToolbox" : n;
            apis.push_back({pretty, n});
        }
        QComboBox* hwApi = combo(b, "prefs/hwDecodeApi", "hwDecodeApi", apis);
        hwApi->setToolTip("Which graphics API decodes the video (Auto tries them in turn)");
        p.beside(hwApi, "Hardware Decoder To Use:");
        p.beside(combo(b, "prefs/logPlugins", "logPlugins", levels), "Log Plugin Loading:");
        p.beside(combo(b, "prefs/logReads", "logReads", levels), "Log Reads:");
        p.beside(combo(b, "prefs/logRenders", "logRenders", levels), "Log Renders:");
        p.finish();

        // Decoding AVC/HEVC (and VP9, AV1, MPEG-2) on the graphics card, as VEGAS's "Enable hardware decoding": the card
        // where it can, the processor for whatever it can't (decided file by file, silently).
        QFormLayout* f = extra("File I/O");
        QComboBox* hwMode = combo(renoBody_, "prefs/hwDecode", "hwDecode", {{"Auto (graphics card where possible)", "auto"}, {"Off (processor only)", "off"}});
        hwMode->setToolTip("Decode H.264, HEVC, VP9, AV1 and MPEG-2 video on the graphics card. Applies to files opened from now on");
        f->addRow("Hardware decoding:", hwMode);
        QSpinBox* hwCount = integer(renoBody_, "prefs/hwDecoders", "hwDecoders", 1, 64);
        hwCount->setToolTip("Graphics cards run a limited number of decoders at once; files beyond this many use the processor");
        f->addRow("Files decoded on the card at once:", hwCount);
        auto hwEnable = [hwApi, hwCount, hwMode] {
            const bool on = hwMode->currentData().toString() != "off";
            hwApi->setEnabled(on);
            hwCount->setEnabled(on);
        };
        QTimer::singleShot(0, b, hwEnable);
        QObject::connect(hwMode, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [hwEnable](int) { hwEnable(); });
        f->addRow(note(QString("The logs are text files in %1.").arg(QDir::toNativeSeparators(logFolder())), renoBody_));
    }

    void buildReno()
    {
        QWidget* b = renoBody_;
        QFormLayout* f = renoForm_;
        tab_ = tabs_->addTab(renoInner_, "Reno");
        const int renoIndex = tab_;
        QObject::connect(renoDefault_, &QPushButton::clicked, this, [this, renoIndex] {
            for (Bind& x : binds_)
                if (x.tab == renoIndex && x.reset) x.reset();
        });
        renoGroup_.clear();

        f->addRow(heading("Performance", b));
        QCheckBox* all = check(b, "Use all processors for rendering, effects and decoding", "prefs/useAllCores", "useAllCores");
        f->addRow(all);
        QSpinBox* maxThreads = integer(b, "prefs/maxThreads", "maxThreads", 1, 256);
        maxThreads->setToolTip("The most processors Reno's helper threads, the effects and the video decoders may use when the box above is cleared");
        f->addRow("Processors to use otherwise:", maxThreads);
        QObject::connect(all, &QCheckBox::toggled, b, [maxThreads](bool on) { maxThreads->setEnabled(!on); });
        QTimer::singleShot(0, b, [maxThreads, all] { maxThreads->setEnabled(!all->isChecked()); });
        QSpinBox* decoders = integer(b, "prefs/decoderThreads", "decoderThreads", 0, 64);
        decoders->setSpecialValueText("Automatic");
        decoders->setToolTip("Threads each video file's decoder uses (applies to files opened from now on)");
        f->addRow("Threads per video decoder:", decoders);
        QSpinBox* audioCache = integer(b, "prefs/audioCacheMb", "audioCacheMb", 0, 16384);
        audioCache->setSuffix(" MB");
        audioCache->setToolTip("Time-stretched and pitch-shifted sound kept ready for playing");
        f->addRow("Stretched-sound cache:", audioCache);
        QSpinBox* undo = new QSpinBox(b);
        undo->setObjectName("undoLevels");
        undo->setRange(1, 10000);
        bindPref([undo](const Preferences& p) { undo->setValue(p.undoLevels); }, [undo](Preferences& p) { p.undoLevels = undo->value(); });
        f->addRow("Undo steps remembered:", undo);
        f->addRow(note("The Video tab sets the Dynamic RAM Preview size and the rendering threads for playback.", b));

        f->addRow(heading("Waveforms and pictures", b));
        QSpinBox* thumbCache = integer(b, "prefs/thumbCacheMb", "thumbCacheMb", 0, 65536);
        thumbCache->setSuffix(" MB");
        thumbCache->setToolTip("The thumbnails Reno keeps on disk between sessions (the oldest go first)");
        f->addRow("Thumbnail cache on disk:", thumbCache);
        QSpinBox* peakCache = integer(b, "prefs/peakCacheMb", "peakCacheMb", 0, 65536);
        peakCache->setSuffix(" MB");
        peakCache->setToolTip("The waveforms Reno keeps itself on disk between sessions");
        f->addRow("Waveform cache on disk:", peakCache);
        f->addRow("Thumbnail quality:", combo(b, "prefs/thumbQuality", "thumbQuality", {{"Draft (half size, stretched)", "draft"}, {"Normal", "normal"}, {"High (twice the size)", "high"}}));
        QCheckBox* peaks = new QCheckBox("Keep waveforms in peak files (.sfk), as VEGAS does", b);
        peaks->setObjectName("vegasPeakFiles");
        bindPref([peaks](const Preferences& p) { peaks->setChecked(p.vegasPeakFiles); }, [peaks](Preferences& p) { p.vegasPeakFiles = peaks->isChecked(); });
        f->addRow(peaks);
        QLineEdit* peakFolder = textBox(b, "prefs/peakFolder", "peakFolder");
        peakFolder->setPlaceholderText("Beside the media");
        f->addRow("Peak files go in:", browseRow(b, peakFolder, true));
        QCheckBox* proxies = new QCheckBox("Create audio proxy files for nested projects (.sfap0)", b);
        proxies->setObjectName("createAudioProxies");
        bindPref([proxies](const Preferences& p) { proxies->setChecked(p.createAudioProxies); }, [proxies](Preferences& p) { p.createAudioProxies = proxies->isChecked(); });
        f->addRow(proxies);
        f->addRow(check(b, "Automatically create preview proxies for the long-GOP video (H.264, HEVC, MPEG-2, VP9, AV1) the project uses", "prefs/autoProxyPreview", "autoProxyPreview"));
        f->addRow(note("Made in the background while not playing: small copies (at most 960 x 540, a key frame every half second) that the preview uses below Best quality, "
                       "so a cut into a long file costs a few frames instead of seconds. Renders and Best quality use the originals.", b));
        f->addRow(check(b, "Also make a video proxy at once, as media comes in, for long-GOP media at least this wide (used by the project or not):", "prefs/autoProxyLongGop", "autoProxyLongGop"));
        QSpinBox* proxyWidth = integer(b, "prefs/autoProxyMinWidth", "autoProxyMinWidth", 16, 16384);
        proxyWidth->setSuffix(" pixels wide");
        proxyWidth->setToolTip("Long-GOP video at least this wide gets a proxy when it comes into the project");
        f->addRow("... when it is at least:", proxyWidth);
        QLineEdit* proxyFolder = textBox(b, "prefs/proxyFolder", "proxyFolder");
        proxyFolder->setPlaceholderText("Reno's cache folder");
        f->addRow("Video proxies go in:", browseRow(b, proxyFolder, true));
        f->addRow(check(b, "Keep video proxies beside the media (as VEGAS does) when no folder is chosen", "prefs/proxyBeside", "proxyBeside"));
        f->addRow("Proxies in Reno's cache folder: at most", integer(b, "prefs/proxyCacheMb", "proxyCacheMb", 256, 1048576));
        f->addRow(note("Proxies of greater than 4K media follow the Video tab's setting. Proxies and peak files beside the media are still used when the folder has none. "
                       "Old proxies in the cache folder are deleted (oldest first) above the limit (MB).", b));

        f->addRow(heading("Files", b));
        QSpinBox* autosave = new QSpinBox(b);
        autosave->setObjectName("autoSaveMinutes");
        autosave->setRange(0, 240);
        autosave->setSuffix(" minutes");
        autosave->setSpecialValueText("Never");
        bindPref([autosave](const Preferences& p) { autosave->setValue(p.autoSaveMinutes); }, [autosave](Preferences& p) { p.autoSaveMinutes = autosave->value(); });
        f->addRow("Auto-save a backup every:", autosave);
        QSpinBox* keep = new QSpinBox(b);
        keep->setObjectName("autoSaveKeep");
        keep->setRange(1, 50);
        bindPref([keep](const Preferences& p) { keep->setValue(p.autoSaveKeep); }, [keep](Preferences& p) { p.autoSaveKeep = keep->value(); });
        f->addRow("Auto-save backups kept:", keep);
        f->addRow(note("Reno's own auto-save (<project>.reno.bak, older ones .bak.1, .bak.2...), used when Tools > Advanced Save's backups are off.", b));
        QLineEdit* tpl = textBox(b, "prefs/templateProject", "templateProject");
        tpl->setPlaceholderText("An empty project");
        f->addRow("File > New starts from:", browseRow(b, tpl, false, "Projects (*.veg *.reno);;All files (*)"));
        QDoubleSpinBox* gen = new QDoubleSpinBox(b);
        gen->setObjectName("generatedLength");
        gen->setRange(0.1, 3600);
        gen->setDecimals(2);
        gen->setSuffix(" s");
        bindPref([gen](const Preferences& p) { gen->setValue(p.generatedLength); }, [gen](Preferences& p) { p.generatedLength = gen->value(); });
        f->addRow("New generated media length:", gen);

        f->addRow(heading("Editing", b));
        QSpinBox* fadeIn = integer(b, "prefs/defaultFadeInMs", "defaultFadeInMs", 0, 60000);
        fadeIn->setSuffix(" ms");
        fadeIn->setToolTip("Fade-in an event gets when it is put on the timeline (no more than half its length); the curve is External Control & Automation's");
        f->addRow("New events fade in over:", fadeIn);
        QSpinBox* fadeOut = integer(b, "prefs/defaultFadeOutMs", "defaultFadeOutMs", 0, 60000);
        fadeOut->setSuffix(" ms");
        f->addRow("New events fade out over:", fadeOut);
        QSpinBox* audioH = integer(b, "prefs/newAudioTrackHeight", "newAudioTrackHeight", 0, 2000);
        audioH->setSpecialValueText("Reno's");
        audioH->setSuffix(" pixels");
        f->addRow("New audio tracks are:", audioH);
        QSpinBox* videoH = integer(b, "prefs/newVideoTrackHeight", "newVideoTrackHeight", 0, 2000);
        videoH->setSpecialValueText("Reno's");
        videoH->setSuffix(" pixels");
        f->addRow("New video tracks are:", videoH);
        QCheckBox* snapping = new QCheckBox("Enable snapping", b);
        snapping->setObjectName("snapping");
        bindPref([snapping](const Preferences& p) { snapping->setChecked(p.snapping); }, [snapping](Preferences& p) { p.snapping = snapping->isChecked(); });
        f->addRow(snapping);
        QCheckBox* grid = new QCheckBox("Snap to the ruler's marks as well", b);
        grid->setObjectName("snapToGrid");
        bindPref([grid](const Preferences& p) { grid->setChecked(p.snapToGrid); }, [grid](Preferences& p) { p.snapToGrid = grid->isChecked(); });
        f->addRow(grid);
        QSpinBox* snapDistance = integer(b, "prefs/snapDistance", "snapDistance", 1, 64);
        snapDistance->setSuffix(" pixels");
        snapDistance->setToolTip("How near something must come to snap to it");
        f->addRow("Snap distance:", snapDistance);
        QCheckBox* follow = new QCheckBox("Cursor stays where playback stops", b);
        follow->setObjectName("cursorFollowsPlayback");
        bindPref([follow](const Preferences& p) { follow->setChecked(p.cursorFollowsPlayback); }, [follow](Preferences& p) { p.cursorFollowsPlayback = follow->isChecked(); });
        f->addRow(follow);
        QComboBox* scroll = new QComboBox(b);
        scroll->setObjectName("scrollMode");
        for (auto it : std::vector<std::pair<QString, QString>>{{"Page (jump a screen at a time)", "page"}, {"Smooth (keep the cursor in view)", "smooth"}, {"None", "none"}})
            scroll->addItem(it.first, it.second);
        bindPref([scroll](const Preferences& p) { scroll->setCurrentIndex(std::max(0, scroll->findData(qs(p.scrollMode)))); },
                 [scroll](Preferences& p) { p.scrollMode = s8(scroll->currentData().toString()); });
        f->addRow("Timeline during playback:", scroll);
        QComboBox* quality = new QComboBox(b);
        quality->setObjectName("previewQuality");
        for (auto it : std::vector<std::pair<QString, QString>>{{"Draft", "draft"}, {"Preview", "preview"}, {"Good", "good"}, {"Best", "best"}})
            quality->addItem(it.first, it.second);
        bindPref([quality](const Preferences& p) { quality->setCurrentIndex(std::max(0, quality->findData(qs(p.previewQuality)))); },
                 [quality](Preferences& p) { p.previewQuality = s8(quality->currentData().toString()); });
        f->addRow("Preview quality to start with:", quality);
        QCheckBox* scrub = check(b, "Play the sound while the cursor is dragged on the ruler (scrub audio)", "prefs/scrubAudio", "scrubAudio");
        f->addRow(scrub);
        QSpinBox* scrubMs = integer(b, "prefs/scrubAudioMs", "scrubAudioMs", 20, 2000);
        scrubMs->setSuffix(" ms");
        f->addRow("Scrub audio piece:", scrubMs);
        QSpinBox* scrubVol = integer(b, "prefs/scrubVolume", "scrubVolume", 0, 400);
        scrubVol->setSuffix(" %");
        f->addRow("Scrub audio volume:", scrubVol);
        QObject::connect(scrub, &QCheckBox::toggled, b, [scrubMs, scrubVol](bool on) { scrubMs->setEnabled(on); scrubVol->setEnabled(on); });
        QTimer::singleShot(0, b, [scrubMs, scrubVol, scrub] { scrubMs->setEnabled(scrub->isChecked()); scrubVol->setEnabled(scrub->isChecked()); });

        f->addRow(heading("Interface", b));
        QSpinBox* uiScale = integer(b, "prefs/uiScale", "uiScale", 50, 300);
        uiScale->setSuffix(" %");
        uiScale->setSingleStep(10);
        uiScale->setToolTip("The size of everything in Reno's windows; read when Reno starts");
        f->addRow("Interface size (after a restart):", uiScale);
        QSpinBox* tlFont = integer(b, "prefs/timelineFontPt", "timelineFontPt", 0, 32);
        tlFont->setSpecialValueText("Reno's");
        tlFont->setSuffix(" pt");
        f->addRow("Timeline text size:", tlFont);

        f->addRow(heading("Questions", b));
        f->addRow(check(b, "Ask to save changes when Reno is closed (cleared: kept in the auto-save file)", "prefs/confirmExit", "confirmExit"));
        f->addRow(check(b, "Ask before removing media from the project", "prefs/confirmRemoveMedia", "confirmRemoveMedia"));
        f->addRow(check(b, "Ask before Clean Up Prerendered Video deletes the files", "prefs/confirmCleanPrerender", "confirmCleanPrerender"));
        f->addRow(check(b, "Say which plug-ins are missing when a project opens", "prefs/warnMissingPlugins", "warnMissingPlugins"));

        f->addRow(heading("Plug-ins", b));
        f->addRow(new QLabel("Also look for video effects (OpenFX) in:", b));
        videoDirs_ = new FolderList(b, "videoFolders", Catalog::get().extraVideoDirs);
        f->addRow(videoDirs_);
        f->addRow(new QLabel("Also look for audio effects (VST) in (the first two are the VST Effects tab's alternate folders):", b));
        audioDirs_ = new FolderList(b, "audioFolders", Catalog::get().extraAudioDirs);
        f->addRow(audioDirs_);
        f->addRow(check(b, "Look for new and changed plug-ins when Reno starts", "prefs/scanAtStartup", "scanAtStartup"));
        QLineEdit* cli = textBox(b, "prefs/renoCliPath", "renoCliPath");
        cli->setPlaceholderText(QDir::toNativeSeparators(qs(defaultProbeExe())));
        f->addRow("reno_cli that checks plug-ins:", browseRow(b, cli, false, "Programs (*.exe);;All files (*)"));
        {
            QPushButton* look = new QPushButton("Look for New Plug-Ins Now", b);
            look->setObjectName("lookForPlugins");
            QPushButton* again = new QPushButton("Check All Plug-Ins Again", b);
            again->setObjectName("checkAllPlugins");
            QWidget* row = new QWidget(b);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(look);
            h->addWidget(again);
            h->addStretch(1);
            f->addRow(row);
            QObject::connect(look, &QPushButton::clicked, this, [this] { applyAll(); runPluginScan(this, false); });
            QObject::connect(again, &QPushButton::clicked, this, [this] { applyAll(); runPluginScan(this, true); });
        }
        QComboBox* host = new QComboBox(b);
        host->setObjectName("ofxHostName");
        host->addItem("VEGAS Pro 18 (com.vegascreativesoftware.vegas)", kVegasHost);
        host->addItem("VEGAS Pro 13 and older (com.sonycreativesoftware.vegas)", kSonyHost);
        host->addItem("A plain OpenFX host (org.reno.host)", kPlainHost);
        bindPref([host](const Preferences& p) {
                     if (host->findData(qs(p.ofxHostName)) < 0) host->addItem(qs(p.ofxHostName), qs(p.ofxHostName));
                     host->setCurrentIndex(std::max(0, host->findData(qs(p.ofxHostName))));
                 },
                 [host](Preferences& p) { p.ofxHostName = s8(host->currentData().toString()); });
        f->addRow("Tell effects the host is:", host);
        QCheckBox* ext = new QCheckBox("VEGAS extensions (suites, properties, picture order)", b);
        ext->setObjectName("vegasExtensions");
        bindPref([ext](const Preferences& p) { ext->setChecked(p.vegasExtensions); }, [ext](Preferences& p) { p.vegasExtensions = ext->isChecked(); });
        f->addRow(ext);
        QCheckBox* stubs = new QCheckBox("Stand-ins for VEGAS suites Reno does not provide", b);
        stubs->setObjectName("stubUnknownSuites");
        bindPref([stubs](const Preferences& p) { stubs->setChecked(p.stubUnknownSuites); }, [stubs](Preferences& p) { p.stubUnknownSuites = stubs->isChecked(); });
        f->addRow(stubs);
        f->addRow(note("Effects already loaded keep the old OpenFX settings until Reno is restarted.", b));
        binds_.push_back({tab_,
                          [this] {
                              const std::vector<std::string> a = Catalog::get().extraAudioDirs;
                              alt1_->setText(a.size() > 0 ? QDir::toNativeSeparators(qs(a[0])) : QString());
                              alt2_->setText(a.size() > 1 ? QDir::toNativeSeparators(qs(a[1])) : QString());
                          },
                          nullptr, nullptr});
    }

    static QString logFolder() { return qs(userDataDir()) + "/Logs"; }

    void applyAll()
    {
        AppState& app = AppState::get();
        for (Bind& b : binds_)
            if (b.save) b.save();
        std::vector<std::string> ad = audioDirs_->folders();
        {
            const std::string a1 = s8(QDir::fromNativeSeparators(alt1_->text().trimmed())), a2 = s8(QDir::fromNativeSeparators(alt2_->text().trimmed()));
            const std::vector<std::string> old = Catalog::get().extraAudioDirs;
            const std::string o1 = old.size() > 0 ? old[0] : std::string(), o2 = old.size() > 1 ? old[1] : std::string();
            if (a1 != o1 || a2 != o2) {
                std::vector<std::string> rest;
                for (size_t k = 2; k < ad.size(); ++k) rest.push_back(ad[k]);
                ad.clear();
                if (!a1.empty()) ad.push_back(a1);
                if (!a2.empty()) ad.push_back(a2);
                for (const auto& r : rest)
                    if (r != a1 && r != a2) ad.push_back(r);
                audioDirs_->set(ad);
            }
        }
        Catalog& cat = Catalog::get();
        const std::vector<std::string> vd = videoDirs_->folders();
        if (vd != cat.extraVideoDirs || ad != cat.extraAudioDirs) {
            cat.extraVideoDirs = vd;
            cat.extraAudioDirs = ad;
            cat.saveCache();
        }
        {
            std::map<uint32_t, uint32_t> remap;
            for (size_t k = 0; k < trackColors_.size() && k < oldTrackColors_.size(); ++k)
                if (trackColors_[k] != oldTrackColors_[k]) {
                    const QColor& o = oldTrackColors_[k];
                    const QColor& n = trackColors_[k];
                    remap[((uint32_t)o.red() << 16) | ((uint32_t)o.green() << 8) | (uint32_t)o.blue()] =
                        ((uint32_t)n.red() << 16) | ((uint32_t)n.green() << 8) | (uint32_t)n.blue();
                }
            bool any = false;
            for (const Track& t : app.project().tracks) any = any || remap.count(t.color & 0xFFFFFFu);
            if (any)
                app.edit("Track Color", [&](Project& p) {
                    for (Track& t : p.tracks) {
                        auto it = remap.find(t.color & 0xFFFFFFu);
                        if (it != remap.end()) t.color = it->second;
                    }
                });
            oldTrackColors_ = trackColors_;
        }
        const bool snapWas = app.prefs.snapping;
        app.prefs = pr_;
        if (pr_.snapping != snapWas) app.snapping = pr_.snapping;
        vprefs::reload();
        app.applyPreferences();
    }

    QLineEdit *alt1_ = nullptr, *alt2_ = nullptr;
    FolderList *videoDirs_ = nullptr, *audioDirs_ = nullptr;
    std::vector<QColor> trackColors_, oldTrackColors_;
    std::map<QString, QColor> envColors_;
};

}  // namespace

void runPreferences(QWidget* parent)
{
    PrefsDialog dlg(parent);
    dlg.exec();
}

}  // namespace app
}  // namespace reno
