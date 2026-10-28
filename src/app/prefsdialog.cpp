// Options > Preferences, as in VEGAS Pro 18: its tabs in order (General, Video, Preview Device, Audio, Audio Device,
// MIDI, VST
// Effects, Editing, Display, Sync, External Control & Automation, Deprecated Features, File I/O; no CD Settings) with
// VEGAS's
// wording and layout (from vegas180.exe's dialog resources 810-912, see notes/vegas_dialogs.txt), Default All on each tab.
//
// Values are Reno's Preferences (appstate.h) where the setting already existed there, otherwise vprefs.h's QSettings
// values.
// VEGAS settings for hardware and features Reno lacks (control surfaces, CD burning, GPU and stereoscopic 3D,
// ASIO...) are not shown.
#include "editdialogs.h"
#include "panels.h"
#include "midisync.h"
#include "playback.h"
#include "theme.h"
#include "vprefs.h"
#include "../engine/catalog.h"
#include "strconv.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
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
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#ifdef RENO_HAVE_QTMULTIMEDIA
#include <QAudioDevice>
#include <QMediaDevices>
#endif
#include <cmath>
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
    return st.isValid() && st.bytesAvailable() > 0 ? QString("%1 MB").arg(st.bytesAvailable() / (1024 * 1024)) : QString("-");
}

class ColorButton : public QToolButton {
public:
    ColorButton(QWidget* parent, const char* name) : QToolButton(parent)
    {
        setObjectName(name);
        setFixedSize(48, 20);
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
    {"prefs/confirmMediaDelete", "Confirm media deletion when still in use", ""},
    {"prefs/savePrerenders", "Save active prerenders on project close", ""},
    {"prefs/closeMediaInactive", "Close media files when not the active application", ""},
    {"prefs/closePortsInactive", "Close audio and MIDI ports when not the active application", ""},
    {"prefs/promptMatchMedia", "Prompt to adjust project settings to match first media added to timeline", ""},
    {"prefs/wave64", "Render large Wave files as Wave64", ""},
    {"prefs/wave4gb", "Allow Wave renders up to 4 GB", ""},
    {"prefs/mxfMultichannel", "Import MXF as multichannel", ""},
    {"prefs/stereoDualMono", "Import stereo as dual mono", ""},
    {"prefs/qsv", "Enable QSV Encoding and Decoding (where available)", ""},
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

class PrefsDialog : public QDialog {
public:
    explicit PrefsDialog(QWidget* parent) : QDialog(parent), pr_(AppState::get().prefs)
    {
        setObjectName("preferencesDialog");
        setWindowTitle("Preferences");
        setMinimumWidth(600);
        resize(640, 640);
        QVBoxLayout* root = new QVBoxLayout(this);
        tabs_ = new QTabWidget(this);
        tabs_->setObjectName("tabs");
        tabs_->setUsesScrollButtons(true);
        root->addWidget(tabs_, 1);

        buildGeneral();
        buildVideo();
        buildPreviewDevice();
        buildAudio();
        buildAudioDevice();
        buildMidi();
        buildVst();
        buildEditing();
        buildDisplay();
        buildSync();
        buildExternal();
        buildDeprecated();
        buildFileIo();
        buildReno();

        for (Bind& b : binds_) b.load();

        QHBoxLayout* buttons = new QHBoxLayout();
        buttons->addStretch(1);
        QPushButton* ok = new QPushButton("OK", this);
        ok->setObjectName("ok");
        ok->setDefault(true);
        QPushButton* cancel = new QPushButton("Cancel", this);
        cancel->setObjectName("cancel");
        QPushButton* apply = new QPushButton("&Apply", this);
        apply->setObjectName("apply");
        buttons->addWidget(ok);
        buttons->addWidget(cancel);
        buttons->addWidget(apply);
        root->addLayout(buttons);
        QObject::connect(ok, &QPushButton::clicked, this, [this] { applyAll(); accept(); });
        QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
        QObject::connect(apply, &QPushButton::clicked, this, [this] { applyAll(); });
        const int last = QSettings().value("prefs/dialogTab", 0).toInt();
        if (last >= 0 && last < tabs_->count()) tabs_->setCurrentIndex(last);
        QObject::connect(tabs_, &QTabWidget::currentChanged, this, [](int i) { QSettings().setValue("prefs/dialogTab", i); });
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

    QWidget* page(const QString& title, const char* name, QWidget** body, std::vector<QPushButton*> extra = {})
    {
        QWidget* outer = new QWidget(tabs_);
        outer->setObjectName(name);
        QVBoxLayout* v = new QVBoxLayout(outer);
        QScrollArea* scroll = new QScrollArea(outer);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        QWidget* inner = new QWidget(scroll);
        scroll->setWidget(inner);
        v->addWidget(scroll, 1);
        QFrame* line = new QFrame(outer);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        v->addWidget(line);
        QHBoxLayout* h = new QHBoxLayout();
        h->addStretch(1);
        for (QPushButton* b : extra) {
            b->setParent(outer);
            h->addWidget(b);
        }
        QPushButton* def = new QPushButton("De&fault All", outer);
        def->setObjectName("defaultAll");
        h->addWidget(def);
        v->addLayout(h);
        const int index = tabs_->addTab(outer, title);
        tab_ = index;
        QObject::connect(def, &QPushButton::clicked, this, [this, index] {
            for (Bind& b : binds_)
                if (b.tab == index && b.reset) b.reset();
        });
        *body = inner;
        return outer;
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

    void buildGeneral()
    {
        QWidget* b;
        page("General", "generalTab", &b);
        QVBoxLayout* v = new QVBoxLayout(b);
        v->addWidget(new QLabel("&General preferences:", b));
        QListWidget* list = new QListWidget(b);
        list->setObjectName("generalList");
        list->setMinimumHeight(330);
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
        v->addWidget(list, 1);
        QHBoxLayout* recent = new QHBoxLayout();
        recent->addWidget(check(b, "&Recently used project list:", "prefs/recentList", "recentList"));
        QSpinBox* count = integer(b, "prefs/recentCount", "recentCount", 1, 9);
        recent->addWidget(count);
        recent->addStretch(1);
        v->addLayout(recent);
        v->addWidget(new QLabel("&Temporary files folder:", b));
        QLineEdit* temp = textBox(b, "prefs/tempFolder", "tempFolder");
        temp->setPlaceholderText(QDir::toNativeSeparators(QDir::tempPath()));
        v->addWidget(browseRow(b, temp, true));
        QHBoxLayout* fs = new QHBoxLayout();
        fs->addWidget(new QLabel("Free storage space in selected folder:", b));
        QLabel* space = new QLabel(b);
        space->setObjectName("tempFolderSpace");
        fs->addWidget(space);
        fs->addStretch(1);
        v->addLayout(fs);
        auto showSpace = [temp, space] { space->setText(freeSpace(temp->text())); };
        QObject::connect(temp, &QLineEdit::textChanged, b, [showSpace](const QString&) { showSpace(); });
        QTimer::singleShot(0, b, showSpace);
    }

    void buildVideo()
    {
        QWidget* b;
        page("Video", "videoTab", &b);
        QFormLayout* f = new QFormLayout(b);
        QSpinBox* ram = new QSpinBox(b);
        ram->setObjectName("ramPreviewMb");
        ram->setRange(0, 65536);
        bindPref([ram](const Preferences& p) { ram->setValue(p.ramPreviewMb); }, [ram](Preferences& p) { p.ramPreviewMb = ram->value(); });
        f->addRow("&Dynamic RAM Preview max (MB):", ram);
        f->addRow(note("Frames shown while playing or scrubbing are kept in this memory too, so going back over them plays from memory. 0 turns it off.", b));
        QSpinBox* threads = new QSpinBox(b);
        threads->setObjectName("renderThreads");
        threads->setRange(0, 16);
        threads->setSpecialValueText("Automatic");
        bindPref([threads](const Preferences& p) { threads->setValue(p.renderThreads); }, [threads](Preferences& p) { p.renderThreads = threads->value(); });
        f->addRow("Ma&ximum number of rendering threads:", threads);
        QComboBox* numbers = new QComboBox(b);
        numbers->setObjectName("thumbnailNumbers");
        for (auto it : std::vector<std::pair<QString, QString>>{{"None", "none"}, {"Frame Numbers", "frames"}, {"Time", "time"}, {"Timecode", "timecode"}})
            numbers->addItem(it.first, it.second);
        bindPref([numbers](const Preferences& p) { numbers->setCurrentIndex(std::max(0, numbers->findData(qs(p.thumbnailNumbers)))); },
                 [numbers](Preferences& p) { p.thumbnailNumbers = s8(numbers->currentData().toString()); });
        f->addRow("Show source frame &numbers on event thumbnails as:", numbers);
        QComboBox* thumbs = new QComboBox(b);
        thumbs->setObjectName("videoThumbnails");
        for (auto it : std::vector<std::pair<QString, QString>>{{"All", "all"}, {"Head, Center, Tail", "headCenterTail"}, {"Head, Tail", "headTail"}, {"Head", "head"}, {"None", "none"}})
            thumbs->addItem(it.first, it.second);
        bindPref([thumbs](const Preferences& p) { thumbs->setCurrentIndex(std::max(0, thumbs->findData(qs(p.videoThumbnails)))); },
                 [thumbs](Preferences& p) { p.videoThumbnails = s8(thumbs->currentData().toString()); });
        f->addRow("Thumbnails to show in video &events:", thumbs);
        f->addRow(check(b, "Automatically create video proxies for greater than 4K resolution media", "prefs/autoProxy4k", "autoProxy4k"));
        f->addRow(check(b, "GPU acceleration of video processing", "prefs/gpuAccel", "gpuAccel"));
        QComboBox* gpuApi = combo(b, "prefs/gpuApi", "gpuApi", {{"Vulkan", "vulkan"}, {"OpenGL", "opengl"}, {"Off (CPU only)", "off"}});
        gpuApi->setToolTip("Vulkan: built-in effects with GPU kernels run on the graphics card (falling back to the CPU if there is no usable Vulkan driver). "
                           "OpenGL: only plug-ins that render with OpenGL use the card. Off: everything on the CPU. "
                           "Plug-in GPU use also needs \"GPU acceleration\" above. \"reno_cli vkcheck\" tests Vulkan.");
        f->addRow("GPU &API:", gpuApi);
        QComboBox* vegasFx = combo(b, "prefs/vegasFxSource", "vegasFxSource",
                                   {{"Reno's built-in versions (ignore the installed ones)", "builtin"}, {"VEGAS's own plug-ins when installed", "real"}});
        vegasFx->setToolTip("VEGAS's own effects (Gaussian Blur, Cookie Cutter, ...) are reproduced in Reno to match VEGAS's output. By default Reno uses its "
                            "versions and ignores VEGAS's installed OpenFX plug-ins; effects Reno has no version of still come from the installed plug-ins. "
                            "The other choice uses VEGAS's plug-ins when VEGAS is installed and they work here. "
                            "Effects already in the project are recreated when this changes. (reno_cli vegasfx compares the two.)");
        f->addRow("VEGAS &effects:", vegasFx);
        QComboBox* vegasEfx = combo(b, "prefs/vegasEfxSource", "vegasEfxSource",
                                    {{"Reno's built-in versions", "builtin"}, {"The installed plug-ins (need VEGAS's licence)", "real"}});
        vegasEfx->setToolTip("VEGAS's eFX audio effects (Limiter, Compressor, Reverb, TubeStage, Phaser, ChorusFlanger) are MAGIX plug-ins that only run fully inside VEGAS "
                             "(the Limiter, for one, does not limit elsewhere). Reno's built-in versions read the same settings and show the same controls, but sound different. "
                             "Effects already in the project are recreated when this changes.");
        f->addRow("VEGAS &eFX audio effects:", vegasEfx);
        QComboBox* vegasDx = combo(b, "prefs/vegasDxSource", "vegasDxSource",
                                   {{"Reno's built-in versions (ignore the installed ones)", "builtin"}, {"The installed DirectX plug-ins when they work", "real"}});
        vegasDx->setToolTip("VEGAS's DirectX audio effects (Reverb, Distortion, the EQs, Track Compressor, ...) are reproduced in Reno to sound the same. "
                            "By default Reno uses its versions and ignores the installed DirectX plug-ins. The other choice uses the installed ones on Windows "
                            "when they load, and Reno's version otherwise. Effects already in the project are recreated when this changes.");
        f->addRow("VEGAS &DirectX audio effects:", vegasDx);
        {
            QCheckBox* all = check(b, "Render every effect at the preview's size in smaller previews", "prefs/previewSizeAllFx", "previewSizeAllFx");
            all->setToolTip("At Draft/Preview quality or a small preview window, effects that cannot render at a smaller size "
                            "(no multi-resolution support, e.g. many third-party plug-ins) are still given the preview's frame, "
                            "as VEGAS does. Much faster; they may look slightly different from the final render (sizes in pixels). "
                            "Off: such effects work at the full project size in previews too. Final renders always use the full size.");
            f->addRow(all);
        }

        f->addRow(heading("Video Preview display options", b));
        QDoubleSpinBox* action = new QDoubleSpinBox(b);
        action->setObjectName("actionSafe");
        action->setRange(0, 45);
        action->setDecimals(1);
        bindPref([action](const Preferences& p) { action->setValue(p.actionSafe); }, [action](Preferences& p) { p.actionSafe = action->value(); });
        QDoubleSpinBox* title = new QDoubleSpinBox(b);
        title->setObjectName("titleSafe");
        title->setRange(0, 45);
        title->setDecimals(1);
        bindPref([title](const Preferences& p) { title->setValue(p.titleSafe); }, [title](Preferences& p) { p.titleSafe = title->value(); });
        QSpinBox* cols = new QSpinBox(b);
        cols->setObjectName("gridColumns");
        cols->setRange(1, 64);
        bindPref([cols](const Preferences& p) { cols->setValue(p.gridColumns); }, [cols](Preferences& p) { p.gridColumns = cols->value(); });
        QSpinBox* rows = new QSpinBox(b);
        rows->setObjectName("gridRows");
        rows->setRange(1, 64);
        bindPref([rows](const Preferences& p) { rows->setValue(p.gridRows); }, [rows](Preferences& p) { p.gridRows = rows->value(); });
        auto pair = [b](const QString& l1, QWidget* w1, const QString& l2, QWidget* w2) {
            QWidget* row = new QWidget(b);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(new QLabel(l1, row));
            h->addWidget(w1);
            h->addSpacing(16);
            h->addWidget(new QLabel(l2, row));
            h->addWidget(w2);
            h->addStretch(1);
            return row;
        };
        f->addRow(pair("Action &safe area (%):", action, "&Horizontal grid divisions (#):", cols));
        f->addRow(pair("&Title safe area (%):", title, "&Vertical grid divisions (#):", rows));
        // the preview's own switches: its right-click menu's Scale Video to Fit Preview Window and Simulate Device
        // Aspect Ratio
        QCheckBox* projectSize = new QCheckBox("Display at project si&ze", b);
        projectSize->setObjectName("displayAtProjectSize");
        binds_.push_back({tab_, [projectSize] { projectSize->setChecked(!QSettings().value("preview/scaleToFit", true).toBool()); },
                          [projectSize] { QSettings().setValue("preview/scaleToFit", !projectSize->isChecked()); }, [projectSize] { projectSize->setChecked(false); }});
        QCheckBox* simulate = new QCheckBox("Simulate device aspect &ratio", b);
        simulate->setObjectName("simulateAspect");
        binds_.push_back({tab_, [simulate] { simulate->setChecked(QSettings().value("preview/simulateAspect", false).toBool()); },
                          [simulate] { QSettings().setValue("preview/simulateAspect", simulate->isChecked()); }, [simulate] { simulate->setChecked(false); }});
        {
            QWidget* row = new QWidget(b);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(projectSize);
            h->addWidget(simulate);
            h->addStretch(1);
            f->addRow(row);
        }
        QComboBox* bg = combo(b, "prefs/previewBackground", "previewBackground",
                              {{"Default", "default"}, {"Black", "black"}, {"White", "white"}, {"Checkerboard", "checkerboard"}});
        bg->setToolTip("What the Video Preview shows where the picture is transparent (and round it)");
        f->addRow("Bac&kground color:", bg);
        f->addRow(heading("Default track fade colors", b));
        QWidget* fades = new QWidget(b);
        {
            QHBoxLayout* h = new QHBoxLayout(fades);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(new QLabel("Track fade to&p:", fades));
            h->addWidget(colorButton(fades, "prefs/fadeTop", "fadeTop"));
            h->addSpacing(20);
            h->addWidget(new QLabel("Track fade botto&m:", fades));
            h->addWidget(colorButton(fades, "prefs/fadeBottom", "fadeBottom"));
            h->addStretch(1);
        }
        f->addRow(fades);
        f->addRow(note("New video tracks get these fade colours (a track's own: its header's menu, Fade Colors).", b));
    }

    void buildPreviewDevice()
    {
        QWidget* b;
        QPushButton* devDefaults = new QPushButton("De&vice Defaults", this);
        devDefaults->setObjectName("deviceDefaults");
        page("Preview Device", "previewDeviceTab", &b, {devDefaults});
        QFormLayout* f = new QFormLayout(b);
        QComboBox* dev = combo(b, "prefs/pdDevice", "pdDevice", {{"Windows Graphics Card", "graphics"}, {"(None)", "none"}});
        f->addRow("&Device:", dev);
        QPlainTextEdit* details = new QPlainTextEdit(b);
        details->setObjectName("pdDetails");
        details->setReadOnly(true);
        details->setFixedHeight(64);
        f->addRow("Details:", details);
        QComboBox* adapter = new QComboBox(b);
        adapter->setObjectName("pdAdapter");
        adapter->addItem("A display other than Reno's", -1);
        const QList<QScreen*> screens = QGuiApplication::screens();
        for (int k = 0; k < screens.size(); ++k)
            adapter->addItem(QString("%1: %2 (%3 x %4)").arg(k + 1).arg(screens[k]->name()).arg(screens[k]->size().width()).arg(screens[k]->size().height()), k);
        binds_.push_back({tab_, [adapter] { adapter->setCurrentIndex(std::max(0, adapter->findData(vprefs::number("prefs/pdAdapter")))); },
                          [adapter] { vprefs::set("prefs/pdAdapter", adapter->currentData().toInt()); }, [adapter] { adapter->setCurrentIndex(0); }});
        QPushButton* identify = new QPushButton("&Identify Displays", b);
        identify->setObjectName("identifyDisplays");
        QWidget* arow = new QWidget(b);
        {
            QHBoxLayout* h = new QHBoxLayout(arow);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(adapter, 1);
            h->addWidget(identify);
        }
        f->addRow("Display &adapter:", arow);
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
            if (dev->currentData().toString() == "none") { details->setPlainText("No external preview device."); return; }
            QString t = "Windows Graphics Card: the picture full screen on a display (View > External Monitor Display chooses it too).\n";
            for (QScreen* s : QGuiApplication::screens())
                t += QString("%1: %2 x %3, %4 Hz\n").arg(s->name()).arg(s->size().width()).arg(s->size().height()).arg(s->refreshRate(), 0, 'f', 1);
            details->setPlainText(t);
        };
        QObject::connect(dev, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [updateDetails](int) { updateDetails(); });
        QTimer::singleShot(0, b, updateDetails);
        QCheckBox* scale = check(b, "Scale output to fit display", "prefs/pdScale", "pdScale");
        f->addRow(scale);
        f->addRow(check(b, "Apply deinterlace filter", "prefs/pdDeinterlace", "pdDeinterlace"));
        f->addRow(check(b, "Adjust levels from studio RGB to computer RGB", "prefs/pdStudioRgb", "pdStudioRgb"));
        f->addRow(check(b, "Display frames in video preview window during &playback", "prefs/pdShowInPreview", "pdShowInPreview"));
        f->addRow(note("AJA, Blackmagic Design DeckLink and OHCI IEEE 1394 devices are not supported: Reno previews on a display "
                       "(Tools > Preview on External Monitor, Alt+Shift+4).", b));
        const int myTab = tab_;
        QObject::connect(devDefaults, &QPushButton::clicked, this, [this, myTab] {
            for (Bind& x : binds_)
                if (x.tab == myTab && x.reset) x.reset();
        });
    }

    void buildAudio()
    {
        QWidget* b;
        page("Audio", "audioTab", &b);
        QFormLayout* f = new QFormLayout(b);
        QDoubleSpinBox* peak = new QDoubleSpinBox(b);
        peak->setObjectName("normalizePeakDb");
        peak->setRange(-60, 0);
        peak->setDecimals(1);
        peak->setSingleStep(0.1);
        binds_.push_back({tab_, [peak] { peak->setValue(QSettings().value("prefs/normalizePeakDb", -0.1).toDouble()); },
                          [peak] { QSettings().setValue("prefs/normalizePeakDb", peak->value()); }, [peak] { peak->setValue(-0.1); }});
        f->addRow("Normali&ze peak level (dB):", peak);
        f->addRow(check(b, "Recor&d to Broadcast Wave Format", "prefs/recordBwf", "recordBwf"));
        f->addRow(check(b, "Pr&ompt user when no track has been armed for record", "prefs/promptNoArmed", "promptNoArmed"));
        f->addRow(new QLabel("Preferred audio &editor:", b));
        QLineEdit* editor = new QLineEdit(b);
        editor->setObjectName("audioEditor");
        bindPref([editor](const Preferences& p) { editor->setText(qs(p.audioEditor)); }, [editor](Preferences& p) { p.audioEditor = s8(editor->text().trimmed()); });
        f->addRow(browseRow(b, editor, false, "Programs (*.exe);;All files (*)"));
        f->addRow(heading("Metronome", b));
        QRadioButton* def = new QRadioButton("Use default &metronome sounds", b);
        def->setObjectName("metronomeDefault");
        QRadioButton* custom = new QRadioButton("Use custom metronome so&unds", b);
        custom->setObjectName("metronomeCustom");
        binds_.push_back({tab_, [def, custom] { (vprefs::on("prefs/metronomeCustom") ? custom : def)->setChecked(true); },
                          [custom] { vprefs::set("prefs/metronomeCustom", custom->isChecked()); }, [def] { def->setChecked(true); }});
        f->addRow(def);
        f->addRow(custom);
        const QString soundFilter = "Sound files (*.wav *.mp3 *.flac *.ogg *.aif *.aiff);;All files (*)";
        QLineEdit* normal = textBox(b, "prefs/metronomeNormal", "metronomeNormal");
        QLineEdit* accent = textBox(b, "prefs/metronomeAccent", "metronomeAccent");
        QWidget* nrow = browseRow(b, normal, false, soundFilter);
        QWidget* arow = browseRow(b, accent, false, soundFilter);
        f->addRow("&Normal:", nrow);
        f->addRow("A&ccent:", arow);
        auto enable = [custom, nrow, arow] {
            nrow->setEnabled(custom->isChecked());
            arow->setEnabled(custom->isChecked());
        };
        QObject::connect(custom, &QRadioButton::toggled, b, [enable](bool) { enable(); });
        QTimer::singleShot(0, b, enable);
        f->addRow(heading("Loudness configuration", b));
        f->addRow(check(b, "Open editor when new loudness log is generated", "prefs/loudnessOpenEditor", "loudnessOpenEditor"));

        f->addRow(heading("Multichannel files (Reno)", b));
        QComboBox* mc = new QComboBox(b);
        mc->setObjectName("multichannelImport");
        for (auto it : std::vector<std::pair<QString, QString>>{{"One mono event per channel, each on its own track", "tracks"},
                                                                {"One stereo event per pair of channels (group multichannel stereo pairs)", "pairs"},
                                                                {"One event, all its channels mixed down to stereo", "mixdown"}})
            mc->addItem(it.first, it.second);
        bindPref([mc](const Preferences& p) { mc->setCurrentIndex(std::max(0, mc->findData(qs(p.multichannelImport)))); },
                 [mc](Preferences& p) { p.multichannelImport = s8(mc->currentData().toString()); });
        f->addRow("Add multichannel audio as:", mc);
        QCheckBox* pan = new QCheckBox("Set pan for new tracks from multichannel", b);
        pan->setObjectName("panMultichannelTracks");
        bindPref([pan](const Preferences& p) { pan->setChecked(p.panMultichannelTracks); }, [pan](Preferences& p) { p.panMultichannelTracks = pan->isChecked(); });
        f->addRow(pan);
    }

    void buildAudioDevice()
    {
        QWidget* b;
        QPushButton* adv = new QPushButton("Ad&vanced...", this);
        adv->setObjectName("audioAdvanced");
        page("Audio Device", "audioDeviceTab", &b, {adv});
        QFormLayout* f = new QFormLayout(b);
        QComboBox* device = new QComboBox(b);
        device->setObjectName("audioDevice");
        device->addItem("The system's default device", QString());
#ifdef RENO_HAVE_QTMULTIMEDIA
        for (const QAudioDevice& d : QMediaDevices::audioOutputs()) device->addItem(d.description(), QString::fromUtf8(d.id()));
#else
        device->setEnabled(false);
#endif
        bindPref([device](const Preferences& p) {
                     int i = device->findData(qs(p.audioDevice));
                     if (i < 0 && !p.audioDevice.empty()) {
                         device->addItem(QString("(not connected) %1").arg(qs(p.audioDevice)), qs(p.audioDevice));
                         i = device->count() - 1;
                     }
                     device->setCurrentIndex(std::max(0, i));
                 },
                 [device](Preferences& p) { p.audioDevice = s8(device->currentData().toString()); });
        f->addRow("&Default stereo and front playback device:", device);
        QLabel* channels = new QLabel(b);
        channels->setObjectName("deviceChannels");
        f->addRow("Channels:", channels);
        QCheckBox* downmix = new QCheckBox("Downmix 5.1 output to stereo", b);
        downmix->setObjectName("downmixOutput");
        bindPref([downmix](const Preferences& p) { downmix->setChecked(p.downmixOutput); }, [downmix](Preferences& p) { p.downmixOutput = downmix->isChecked(); });
        f->addRow(QString(), downmix);
        auto showChannels = [device, downmix, channels] {
            const int n = audioOutputChannels(s8(device->currentData().toString()));
            QString t = n <= 0 ? QString("unknown") : QString::number(n);
            if (n >= 6) t += downmix->isChecked() ? " - 5.1 projects are folded down to stereo (Downmix)" : " - 5.1 projects play in 5.1 (L R C LFE Ls Rs)";
            else if (n > 0) t += " - 5.1 projects are folded down to stereo";
            channels->setText(t);
        };
        QObject::connect(device, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [showChannels](int) { showChannels(); });
        QObject::connect(downmix, &QCheckBox::toggled, b, [showChannels](bool) { showChannels(); });
        QTimer::singleShot(0, b, showChannels);
        QDoubleSpinBox* buffer = real(b, "prefs/playbackBuffer", "playbackBuffer", 0.02, 1.0, 2, 0.01);
        buffer->setToolTip("How much sound is made ready before it is heard: larger stops gaps, smaller answers sooner. Used the next time playback starts.");
        f->addRow("&Playback buffering (seconds):", buffer);
        QComboBox* rec = new QComboBox(b);
        rec->setObjectName("audioInputDevice");
        rec->addItem("The system's default device", QString());
#ifdef RENO_HAVE_QTMULTIMEDIA
        for (const QAudioDevice& d : QMediaDevices::audioInputs()) rec->addItem(d.description(), QString::fromUtf8(d.id()));
#endif
        bindPref([rec](const Preferences& p) { rec->setCurrentIndex(std::max(0, rec->findData(qs(p.audioInputDevice)))); },
                 [rec](Preferences& p) { p.audioInputDevice = s8(rec->currentData().toString()); });
        f->addRow("Default audi&o recording device:", rec);
        QCheckBox* autoLat = check(b, "Au&tomatically detect and offset for hardware recording latency", "prefs/autoRecordLatency", "autoRecordLatency");
        f->addRow(autoLat);
        QDoubleSpinBox* offset = real(b, "prefs/recordOffsetMs", "recordOffsetMs", -1000, 1000, 1, 1);
        offset->setSuffix(" ms");
        offset->setToolTip("Recordings are placed this much earlier on the timeline (used when the box above is cleared)");
        f->addRow("Us&er offset (ms):", offset);
        QObject::connect(autoLat, &QCheckBox::toggled, b, [offset](bool on) { offset->setEnabled(!on); });
        QTimer::singleShot(0, b, [offset, autoLat] { offset->setEnabled(!autoLat->isChecked()); });
        QObject::connect(adv, &QPushButton::clicked, this, [this, device] {
            const QString id = device->currentData().toString();
            QString t = QString("Device: %1\n").arg(device->currentText());
            t += QString("Output channels: %1\n").arg(audioOutputChannels(s8(id)));
#ifdef RENO_HAVE_QTMULTIMEDIA
            for (const QAudioDevice& d : QMediaDevices::audioOutputs())
                if (QString::fromUtf8(d.id()) == id || (id.isEmpty() && d == QMediaDevices::defaultAudioOutput()))
                    t += QString("Sample rates: %1 to %2 Hz\nChannels: %3 to %4\n").arg(d.minimumSampleRate()).arg(d.maximumSampleRate()).arg(d.minimumChannelCount()).arg(d.maximumChannelCount());
#endif
            t += "\nBuffers, priority and input monitoring are set by Windows' audio for this device.";
            QMessageBox::information(this, "Advanced Audio Configuration", t);
        });
    }

    void buildMidi()
    {
        QWidget* b;
        page("MIDI", "midiTab", &b);
        QVBoxLayout* v = new QVBoxLayout(b);
        auto devList = [this, b, v](const QString& label, const char* name, const std::vector<std::string>& names, const QString& key) {
            v->addWidget(new QLabel(label, b));
            QListWidget* l = new QListWidget(b);
            l->setObjectName(name);
            l->setFixedHeight(110);
            for (const std::string& n : names) {
                QListWidgetItem* it = new QListWidgetItem(qs(n), l);
                it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            }
            if (names.empty()) {
                QListWidgetItem* it = new QListWidgetItem("(no MIDI devices)", l);
                it->setFlags(Qt::NoItemFlags);
            }
            v->addWidget(l);
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
        v->addWidget(note("Reno uses MIDI for timecode and clock (Options > Timecode, and the Sync tab), and has no control surfaces or "
                          "soft synths.", b));
        v->addStretch(1);
    }

    void buildVst()
    {
        QWidget* b;
        page("VST Effects", "vstTab", &b);
        QFormLayout* f = new QFormLayout(b);
        QLineEdit* def = new QLineEdit(b);
        def->setObjectName("vstDefaultFolder");
        def->setReadOnly(true);
#ifdef _WIN32
        def->setText("C:\\Program Files\\Common Files\\VST3; C:\\Program Files\\VSTPlugins; C:\\Program Files\\Steinberg\\VSTPlugins");
#else
        def->setText("~/.vst; ~/.vst3; /usr/lib/vst; /usr/lib/vst3");
#endif
        def->setToolTip("The folders Reno always looks in");
        f->addRow("Default VST search folder:", def);
        alt1_ = new QLineEdit(b);
        alt1_->setObjectName("vstFolder1");
        alt2_ = new QLineEdit(b);
        alt2_->setObjectName("vstFolder2");
        f->addRow("Alternate VST search folder &1:", browseRow(b, alt1_, true));
        f->addRow("Alternate VST search folder &2:", browseRow(b, alt2_, true));
        f->addRow(new QLabel("Select VST effects to be available as audio plug-ins:", b));
        QTreeWidget* list = new QTreeWidget(b);
        list->setObjectName("vstList");
        list->setHeaderLabels({"Name", "File"});
        list->setRootIsDecorated(false);
        list->setMinimumHeight(180);
        for (const AudioFxEntry& e : Catalog::get().audio()) {
            if (e.desc.format != "VST" && e.desc.format != "VST3" && e.desc.format != "VST2") continue;
            QTreeWidgetItem* it = new QTreeWidgetItem(list, {qs(e.desc.name), QDir::toNativeSeparators(qs(e.desc.path))});
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
            it->setData(0, Qt::UserRole, qs(e.desc.path));
            if (!e.ok) it->setToolTip(0, "This plug-in did not work when it was checked: " + qs(e.error));
        }
        list->header()->resizeSection(0, 220);
        f->addRow(list);
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
        if (list->topLevelItemCount() == 0) f->addRow(note("No VST plug-ins have been found (Options > Look for New Plug-Ins looks again).", b));
        f->addRow(note("Cleared plug-ins are left out of the effect lists (projects that use them still load them). New folders are "
                       "searched by Options > Look for New Plug-Ins.", b));
    }

    void buildEditing()
    {
        QWidget* b;
        page("Editing", "editingTab", &b);
        QFormLayout* f = new QFormLayout(b);
        QCheckBox* loop = new QCheckBox("Enable &looping on events by default", b);
        loop->setObjectName("loopEvents");
        bindPref([loop](const Preferences& p) { loop->setChecked(p.loopEvents); }, [loop](Preferences& p) { p.loopEvents = loop->isChecked(); });
        f->addRow(loop);
        QCheckBox* pitch = new QCheckBox("&Preserve pitch when stretching audio events", b);
        pitch->setObjectName("preservePitch");
        bindPref([pitch](const Preferences& p) { pitch->setChecked(p.preservePitch); }, [pitch](Preferences& p) { p.preservePitch = pitch->isChecked(); });
        f->addRow(pitch);
        f->addRow(check(b, "Cut, copy, and delete g&rouped events", "prefs/groupedCutCopy", "groupedCutCopy"));
        {
            QCheckBox* nq = new QCheckBox("Do &not quantize to frames for audio-only edits", b);
            nq->setObjectName("noQuantizeAudio");
            bindPref([nq](const Preferences& p) { nq->setChecked(p.noQuantizeAudioOnly); }, [nq](Preferences& p) { p.noQuantizeAudioOnly = nq->isChecked(); });
            f->addRow(nq);
        }
        f->addRow(check(b, "&Collapse loop region when no time selection is present", "prefs/collapseLoop", "collapseLoop"));
        f->addRow("&JKL / shuttle speed:", combo(b, "prefs/jklSpeed", "jklSpeed", {{"Slow", "slow"}, {"Medium", "medium"}, {"Fast", "fast"}}));
        QDoubleSpinBox* qf = new QDoubleSpinBox(b);
        qf->setObjectName("quickFadeMs");
        qf->setRange(1, 1000);
        qf->setDecimals(0);
        bindPref([qf](const Preferences& p) { qf->setValue(p.quickFadeMs); }, [qf](Preferences& p) { p.quickFadeMs = qf->value(); });
        f->addRow("&Quick fade length for audio events (ms):", qf);
        const double fps = std::max(1.0, AppState::get().project().settings.fps());
        auto framesRow = [b, fps](QDoubleSpinBox* s) {
            QWidget* row = new QWidget(b);
            QHBoxLayout* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QLabel* fr = new QLabel(row);
            h->addWidget(s);
            h->addWidget(fr);
            h->addStretch(1);
            auto show = [s, fr, fps] { fr->setText(QString("Frames: %1 at %2 fps").arg((int)std::lround(s->value() * fps)).arg(fps, 0, 'f', 3)); };
            QObject::connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged), row, [show](double) { show(); });
            QTimer::singleShot(0, row, show);
            return row;
        };
        QDoubleSpinBox* still = new QDoubleSpinBox(b);
        still->setObjectName("stillLength");
        still->setRange(0.1, 3600);
        still->setDecimals(3);
        bindPref([still](const Preferences& p) { still->setValue(p.stillLength); }, [still](Preferences& p) { p.stillLength = still->value(); });
        f->addRow("New &still image length (seconds):", framesRow(still));
        QDoubleSpinBox* cp = new QDoubleSpinBox(b);
        cp->setObjectName("cursorPreview");
        cp->setRange(0.1, 60);
        cp->setDecimals(3);
        bindPref([cp](const Preferences& p) { cp->setValue(p.cursorPreview); }, [cp](Preferences& p) { p.cursorPreview = cp->value(); });
        f->addRow("Cursor preview &duration (seconds):", framesRow(cp));
        {
            auto boolRow = [&](const QString& text, const char* name, bool Preferences::*field) {
                QCheckBox* c = new QCheckBox(text, b);
                c->setObjectName(name);
                bindPref([c, field](const Preferences& p) { c->setChecked(p.*field); }, [c, field](Preferences& p) { p.*field = c->isChecked(); });
                f->addRow(c);
            };
            boolRow("Automatically &overlap multiple selected media when added", "autoOverlapMedia", &Preferences::autoOverlapMedia);
            boolRow("Time selection &envelope fades", "timeSelEnvFades", &Preferences::timeSelEnvFades);
            QWidget* row = new QWidget(b);
            QHBoxLayout* rl = new QHBoxLayout(row);
            rl->setContentsMargins(0, 0, 0, 0);
            QDoubleSpinBox* len = new QDoubleSpinBox(row);
            len->setObjectName("timeSelEnvFadeLength");
            len->setRange(0, 60000);
            len->setDecimals(0);
            len->setSuffix(" ms");
            QComboBox* align = new QComboBox(row);
            align->setObjectName("timeSelEnvFadeAlign");
            align->addItem("Outside", "outside");
            align->addItem("Centered", "centered");
            align->addItem("Inside", "inside");
            rl->addWidget(len);
            rl->addWidget(align, 1);
            bindPref([len, align](const Preferences& p) {
                len->setValue(p.timeSelEnvFadeLength * 1000.0);
                const int i = align->findData(QString::fromStdString(p.timeSelEnvFadeAlign));
                align->setCurrentIndex(i >= 0 ? i : 0);
            }, [len, align](Preferences& p) {
                p.timeSelEnvFadeLength = len->value() / 1000.0;
                p.timeSelEnvFadeAlign = align->currentData().toString().toStdString();
            });
            f->addRow("Envelope fade length / alignment:", row);
            boolRow("Smooth and &thin automation data after recording or drawing", "thinAutomation", &Preferences::thinAutomation);
            boolRow("Use the right mouse button to switch to the Selection Edit tool", "rightMouseSelect", &Preferences::rightMouseSelect);
        }
        QDoubleSpinBox* ov = new QDoubleSpinBox(b);
        ov->setObjectName("cutToOverlap");
        ov->setRange(0.01, 60);
        ov->setDecimals(3);
        bindPref([ov](const Preferences& p) { ov->setValue(p.cutToOverlap); }, [ov](Preferences& p) { p.cutToOverlap = ov->value(); });
        f->addRow("Cu&t-to-overlap conversion (seconds):", framesRow(ov));
        f->addRow("Align&ment:", combo(b, "prefs/overlapAlign", "overlapAlign", {{"Before cut", "before"}, {"Centered on cut", "center"}, {"After cut", "after"}}));
        f->addRow(check(b, "A&utomatically crop still images added to timeline", "prefs/autoCropStills", "autoCropStills"));
        f->addRow("Smart split default length (frames):", integer(b, "prefs/smartSplitFrames", "smartSplitFrames", 1, 1000));
    }

    void buildDisplay()
    {
        QWidget* b;
        page("Display", "displayTab", &b);
        QFormLayout* f = new QFormLayout(b);
        f->addRow(heading("Track Colors", b));
        trackColors_.clear();
        const QStringList savedTracks = vprefs::list("prefs/trackColors");
        for (int k = 0; k < 8; ++k) trackColors_.push_back(k < savedTracks.size() && QColor(savedTracks[k]).isValid() ? QColor(savedTracks[k]) : trackDefaultColor(k));
        oldTrackColors_ = trackColors_;
        QComboBox* track = new QComboBox(b);
        track->setObjectName("trackColorIndex");
        for (int k = 0; k < 8; ++k) track->addItem(QString("Track Color %1").arg(k + 1));
        ColorButton* tc = new ColorButton(b, "trackColor");
        QWidget* trow = new QWidget(b);
        {
            QHBoxLayout* h = new QHBoxLayout(trow);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(track, 1);
            h->addWidget(tc);
        }
        f->addRow("&Track:", trow);
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
        f->addRow(heading("Envelope Colors", b));
        envColors_.clear();
        for (const QString& s : vprefs::list("prefs/envelopeColors")) {
            const int eq = s.indexOf('=');
            if (eq > 0 && QColor(s.mid(eq + 1)).isValid()) envColors_[s.left(eq)] = QColor(s.mid(eq + 1));
        }
        QComboBox* env = new QComboBox(b);
        env->setObjectName("envelopeType");
        for (const auto& k : kEnvelopeKinds) env->addItem(k.first, k.second);
        ColorButton* ec = new ColorButton(b, "envelopeColor");
        QWidget* erow = new QWidget(b);
        {
            QHBoxLayout* h = new QHBoxLayout(erow);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(env, 1);
            h->addWidget(ec);
        }
        f->addRow("En&velope Type:", erow);
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
        f->addRow(heading("Snap Colors", b));
        QComboBox* snap = new QComboBox(b);
        snap->setObjectName("snapType");
        snap->addItem("Snap Indicator", "indicator");
        ColorButton* sc = new ColorButton(b, "snapColor");
        QWidget* srow = new QWidget(b);
        {
            QHBoxLayout* h = new QHBoxLayout(srow);
            h->setContentsMargins(0, 0, 0, 0);
            h->addWidget(snap, 1);
            h->addWidget(sc);
        }
        f->addRow("&Snap Type:", srow);
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
        f->addRow(check(b, "&Display timeline at bottom of main window", "prefs/timelineAtBottom", "timelineAtBottom"));
        f->addRow(check(b, "&Position tabs at top of docked windows", "prefs/tabsAtTop", "tabsAtTop"));
        f->addRow(check(b, "&Allow floating windows to dock (hold Ctrl to override)", "prefs/floatingDock", "floatingDock"));
        f->addRow(check(b, "&Reverse Event Background Color", "prefs/reverseEventBg", "reverseEventBg"));
        f->addRow(check(b, "&Use High DPI scaling (where available)", "prefs/highDpi", "highDpi"));
        f->addRow(check(b, "Use Alternate HighDPI Settings", "prefs/altHighDpi", "altHighDpi"));
        f->addRow(note("The High DPI settings are read when Reno starts.", b));
    }

    void buildSync()
    {
        QWidget* b;
        QPushButton* adv = new QPushButton("Ad&vanced...", this);
        adv->setObjectName("syncAdvanced");
        page("Sync", "syncTab", &b, {adv});
        QFormLayout* f = new QFormLayout(b);
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
        f->addRow(new QLabel("Generate MIDI Timecode settings:", b));
        QComboBox* mtcOut = devCombo("mtcOutputDevice", outs);
        f->addRow("&Output device:", mtcOut);
        f->addRow("F&rame rate:", rateCombo("mtcRate", "midi/mtcRate", binds_, tab_));
        f->addRow(new QLabel("Generate MIDI Clock settings:", b));
        QComboBox* clockOut = devCombo("clockOutputDevice", outs);
        clockOut->setToolTip("Reno sends MIDI timecode and MIDI clock through one port: choosing one chooses both");
        f->addRow("O&utput device:", clockOut);
        f->addRow(new QLabel("Trigger from MIDI Timecode settings:", b));
        QComboBox* mtcIn = devCombo("mtcInputDevice", ins);
        f->addRow("&Input device:", mtcIn);
        f->addRow("Fra&me rate:", rateCombo("chaseRate", "midi/chaseRate", binds_, tab_));
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
        f->addRow(note("Options > Timecode turns them on (F7, Shift+F7, Ctrl+F7).", b));
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
        QWidget* b;
        page("External Control && Automation", "externalTab", &b);
        QFormLayout* f = new QFormLayout(b);
        f->addRow(note("Reno supports no control surfaces (Mackie Control, TranzPort, FaderPort, generic MIDI).", b));
        f->addRow("New &audio envelopes:", combo(b, "prefs/curveAudioEnv", "curveAudioEnv", kCurves));
        f->addRow("New aud&io event gain:", combo(b, "prefs/curveAudioGain", "curveAudioGain", kCurves));
        f->addRow("New audio F&X automation envelopes:", combo(b, "prefs/curveAudioFx", "curveAudioFx", kCurves));
        f->addRow("New vide&o envelopes:", combo(b, "prefs/curveVideoEnv", "curveVideoEnv", kCurves));
        f->addRow("New video eve&nt opacity:", combo(b, "prefs/curveVideoOpacity", "curveVideoOpacity", kCurves));
        f->addRow("New video FX &keyframes:", combo(b, "prefs/curveVideoFx", "curveVideoFx", kKeyframeTypes));
        f->addRow("New video &motion keyframes:", combo(b, "prefs/curveVideoMotion", "curveVideoMotion", kKeyframeTypes));
        f->addRow("Track Motion d&efault smoothness:", real(b, "prefs/motionSmoothness", "motionSmoothness", 0, 100, 1, 1));
        f->addRow("Pan/Crop defa&ult smoothness:", real(b, "prefs/panCropSmoothness", "panCropSmoothness", 0, 100, 1, 1));
    }

    void buildDeprecated()
    {
        QWidget* b;
        page("Deprecated Features", "deprecatedTab", &b);
        QVBoxLayout* v = new QVBoxLayout(b);
        QLabel* l = new QLabel("These features are no longer supported and may be removed from the application in a future version. To use "
                               "the feature, select its checkbox below.",
                               b);
        l->setWordWrap(true);
        v->addWidget(l);
        v->addWidget(new QLabel("&Deprecated Features:", b));
        const QString kept = "Reno has no such feature; the setting only shows the menu item VEGAS shows for it";
        v->addWidget(check(b, "Enable XDCam Explorer", "prefs/depXdcamExplorer", "depXdcamExplorer", kept));
        v->addWidget(check(b, "Enable Import DVD Camcorder Disc", "prefs/depDvdCamcorder", "depDvdCamcorder", kept));
        v->addStretch(1);
    }

    void buildFileIo()
    {
        QWidget* b;
        page("File I/O", "fileIoTab", &b);
        QFormLayout* f = new QFormLayout(b);
        f->addRow(new QLabel("&File I/O options:", b));
        const std::vector<std::pair<QString, QString>> levels = {{"Off", "off"}, {"Basic", "basic"}, {"Detailed", "detailed"}};
        f->addRow("Log Plugin Loading:", combo(b, "prefs/logPlugins", "logPlugins", levels));
        f->addRow("Log Reads:", combo(b, "prefs/logReads", "logReads", levels));
        f->addRow("Log Renders:", combo(b, "prefs/logRenders", "logRenders", levels));
        // Decoding AVC/HEVC (and VP9, AV1, MPEG-2) on the graphics card, as VEGAS's "Enable hardware decoding": the card
        // where it can, the processor for whatever it can't (decided file by file, silently).
        f->addRow(heading("Hardware decoding", b));
        QComboBox* hwMode = combo(b, "prefs/hwDecode", "hwDecode", {{"Auto (graphics card where possible)", "auto"}, {"Off (processor only)", "off"}});
        hwMode->setToolTip("Decode H.264, HEVC, VP9, AV1 and MPEG-2 video on the graphics card. Applies to files opened from now on");
        f->addRow("Hardware decoding:", hwMode);
        std::vector<std::pair<QString, QString>> apis = {{"Automatic", ""}};
        for (const std::string& api : reno::hardwareDecodeApis()) {
            const QString n = QString::fromStdString(api);
            const QString pretty = n == "d3d11va" ? "Direct3D 11 (any graphics card)" : n == "dxva2" ? "DXVA2 (any graphics card)" :
                                   n == "cuda" ? "NVIDIA NVDEC (CUDA)" : n == "vaapi" ? "VA-API" : n == "vdpau" ? "VDPAU" :
                                   n == "videotoolbox" ? "VideoToolbox" : n;
            apis.push_back({pretty, n});
        }
        QComboBox* hwApi = combo(b, "prefs/hwDecodeApi", "hwDecodeApi", apis);
        hwApi->setToolTip("Which graphics API decodes the video (Automatic tries them in turn)");
        f->addRow("Hardware decoding API:", hwApi);
        QSpinBox* hwCount = integer(b, "prefs/hwDecoders", "hwDecoders", 1, 64);
        hwCount->setToolTip("Graphics cards run a limited number of decoders at once; files beyond this many use the processor");
        f->addRow("Files decoded on the card at once:", hwCount);
        QTimer::singleShot(0, b, [hwApi, hwCount, hwMode] {
            const bool on = hwMode->currentData().toString() != "off";
            hwApi->setEnabled(on);
            hwCount->setEnabled(on);
        });
        QObject::connect(hwMode, QOverload<int>::of(&QComboBox::currentIndexChanged), b, [hwApi, hwCount, hwMode](int) {
            const bool on = hwMode->currentData().toString() != "off";
            hwApi->setEnabled(on);
            hwCount->setEnabled(on);
        });
        f->addRow(note(QString("The logs are text files in %1.").arg(QDir::toNativeSeparators(logFolder())), b));
    }

    void buildReno()
    {
        QWidget* b;
        page("Reno", "renoTab", &b);
        QFormLayout* f = new QFormLayout(b);

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
        QComboBox* theme = new QComboBox(b);
        theme->setObjectName("theme");
        theme->addItem("Reno (wide track headers, events with title bars)", "reno");
        theme->addItem("VEGAS (VEGAS Pro 18's look)", "vegas");
        bindPref([theme](const Preferences& p) { theme->setCurrentIndex(std::max(0, theme->findData(qs(p.theme)))); },
                 [theme](Preferences& p) { p.theme = s8(theme->currentData().toString()); });
        f->addRow("Look:", theme);
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
