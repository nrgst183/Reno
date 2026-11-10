// Small VEGAS dialogs (see smalldialogs.h).
#include "smalldialogs.h"
#include "appstate.h"
#include "keyboard.h"
#include "dialoglook.h"
#include <QPainter>
#include <QTabBar>
#include <QToolButton>
#include <QLineEdit>
#include <map>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QFileInfo>
#include <QDateTime>
#include <QListWidget>
#include <QPushButton>
#include "../engine/catalog.h"
#include <algorithm>
#include <cmath>
#include <random>

namespace reno {
namespace app {

std::function<void(QWidget*)> pasteRepeatDialogShown;

bool askPasteRepeat(QWidget* parent, PasteRepeatChoice& out)
{
    const ProjectSettings& ps = AppState::get().project().settings;
    const double fps = ps.fps() > 0 ? ps.fps() : 30.0;
    const double beat = 60.0 / (ps.tempo > 0 ? ps.tempo : 120.0);
    const double measure = beat * std::max(1, ps.beatsPerMeasure);
    QSettings s;
    QDialog d(parent);
    d.setObjectName("pasteRepeat");
    d.setWindowTitle("Paste Repeat");
    QVBoxLayout* v = new QVBoxLayout(&d);
    QFormLayout* f = new QFormLayout();
    QSpinBox* times = new QSpinBox(&d);
    times->setObjectName("times");
    times->setRange(1, 999);
    times->setValue(std::max(1, s.value("pasteRepeat/times", 2).toInt()));
    f->addRow("&Number of times to paste:", times);
    v->addLayout(f);
    QGroupBox* g = new QGroupBox("Paste spacing", &d);
    QVBoxLayout* gv = new QVBoxLayout(g);
    QRadioButton* endToEnd = new QRadioButton("End to end", g);
    endToEnd->setObjectName("endToEnd");
    QRadioButton* even = new QRadioButton("Even spacing", g);
    even->setObjectName("evenSpacing");
    gv->addWidget(endToEnd);
    gv->addWidget(even);
    QHBoxLayout* row = new QHBoxLayout();
    row->addSpacing(20);
    QDoubleSpinBox* every = new QDoubleSpinBox(g);
    every->setObjectName("every");
    every->setDecimals(3);
    every->setRange(0.001, 100000);
    every->setValue(std::max(0.001, s.value("pasteRepeat/every", 1.0).toDouble()));
    QComboBox* unit = new QComboBox(g);
    unit->setObjectName("unit");
    unit->addItem("Seconds", 1.0);
    unit->addItem("Frames", 1.0 / fps);
    unit->addItem("Measures", measure);
    unit->addItem("Beats", beat);
    unit->setCurrentIndex(std::min(3, std::max(0, s.value("pasteRepeat/unit", 0).toInt())));
    QLabel* everyLabel = new QLabel("Paste &every:", g);
    everyLabel->setBuddy(every);
    row->addWidget(everyLabel);
    row->addWidget(every);
    row->addWidget(unit, 1);
    gv->addLayout(row);
    v->addWidget(g);
    (s.value("pasteRepeat/even", false).toBool() ? even : endToEnd)->setChecked(true);
    auto enable = [=] {
        every->setEnabled(even->isChecked());
        unit->setEnabled(even->isChecked());
    };
    enable();
    QObject::connect(even, &QRadioButton::toggled, &d, [enable](bool) { enable(); });
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    v->addWidget(bb);
    if (pasteRepeatDialogShown) pasteRepeatDialogShown(&d);
    if (d.exec() != QDialog::Accepted) return false;
    out.times = times->value();
    out.even = even->isChecked();
    out.every = every->value() * unit->currentData().toDouble();
    s.setValue("pasteRepeat/times", out.times);
    s.setValue("pasteRepeat/even", out.even);
    s.setValue("pasteRepeat/every", every->value());
    s.setValue("pasteRepeat/unit", unit->currentIndex());
    return true;
}

}  // namespace app
}  // namespace reno

namespace reno {
namespace app {

namespace {

bool exported(const QString& key)
{
    return !key.startsWith("window/") && !key.startsWith("Reno Preferences/");
}

}  // namespace

int exportPreferences(const QString& path)
{
    QFile::remove(path);
    QSettings out(path, QSettings::IniFormat);
    QSettings in;
    int n = 0;
    out.setValue("Reno Preferences/version", 1);
    for (const QString& k : in.allKeys()) {
        if (!exported(k)) continue;
        out.setValue(k, in.value(k));
        ++n;
    }
    out.sync();
    return out.status() == QSettings::NoError && QFile::exists(path) ? n : -1;
}

int importPreferences(QMainWindow* window, const QString& path, QString* error)
{
    if (!QFile::exists(path)) {
        if (error) *error = "The file does not exist.";
        return -1;
    }
    QSettings in(path, QSettings::IniFormat);
    if (in.status() != QSettings::NoError || !in.contains("Reno Preferences/version")) {
        if (error) *error = "This is not a file of Reno preferences (Options > Export Preferences makes them).";
        return -1;
    }
    QSettings out;
    int n = 0;
    for (const QString& k : in.allKeys()) {
        if (!exported(k)) continue;
        out.setValue(k, in.value(k));
        ++n;
    }
    out.sync();
    AppState::get().loadPreferences();
    if (window) reloadKeyboardMap(window);
    return n;
}

void addPreferenceFileItems(QMainWindow* window)
{
    if (!window || !window->menuBar()) return;
    QMenu* options = nullptr;
    for (QAction* top : window->menuBar()->actions())
        if (top->menu() && top->text().remove('&') == "Options") options = top->menu();
    if (!options) return;
    QAction* exp = new QAction("E&xport Preferences...", window);
    exp->setObjectName("exportPreferences");
    QObject::connect(exp, &QAction::triggered, window, [window] {
        const QString f = QFileDialog::getSaveFileName(window, "Export Preferences",
                                                       QDir(QSettings().value("prefs/exportFolder", QDir::homePath()).toString()).filePath("Reno Preferences.ini"),
                                                       "Reno preferences (*.ini)");
        if (f.isEmpty()) return;
        QSettings().setValue("prefs/exportFolder", QFileInfo(f).absolutePath());
        const int n = exportPreferences(f);
        if (n < 0) QMessageBox::warning(window, "Export Preferences", "The preferences could not be written to " + QDir::toNativeSeparators(f) + ".");
        else AppState::get().status("Exported " + std::to_string(n) + " settings to " + QDir::toNativeSeparators(f).toStdString() + ".");
    });
    QAction* imp = new QAction("I&mport Preferences...", window);
    imp->setObjectName("importPreferences");
    imp->setStatusTip("Import preferences from a file that was saved from another machine.");
    QObject::connect(imp, &QAction::triggered, window, [window] {
        const QString f = QFileDialog::getOpenFileName(window, "Import Preferences", QSettings().value("prefs/exportFolder", QDir::homePath()).toString(),
                                                       "Reno preferences (*.ini);;All files (*)");
        if (f.isEmpty()) return;
        QSettings().setValue("prefs/exportFolder", QFileInfo(f).absolutePath());
        QString err;
        const int n = importPreferences(window, f, &err);
        if (n < 0) QMessageBox::warning(window, "Import Preferences", err);
        else
            QMessageBox::information(window, "Import Preferences",
                                     QString("%1 settings imported. The preferences and keys are in use now; some windows' own options count "
                                             "from the next time they open (or Reno starts).").arg(n));
    });
    window->addAction(exp);
    window->addAction(imp);
    QAction* prefs = nullptr;
    for (QAction* a : options->actions())
        if (a->objectName() == "preferences") prefs = a;
    if (prefs) {
        options->insertAction(prefs, exp);
        options->insertAction(prefs, imp);
        options->insertSeparator(prefs);
    } else {
        options->addAction(exp);
        options->addAction(imp);
    }
}

std::function<void(QWidget*)> slideshowDialogShown;

const std::vector<std::string>& slideshowEffects()
{
    static const std::vector<std::string> v = {"Fades", "Fades with Pan", "Fades with Pan and Zoom", "Fades with Zoom",
                                               "Random", "Random with Pan", "Random with Pan and Zoom", "Random with Zoom"};
    return v;
}

std::vector<uint64_t> makeSlideshow(const SlideshowSettings& s, std::string* error, std::vector<std::string>* skipped)
{
    AppState& app = AppState::get();
    std::vector<uint64_t> made;
    std::vector<MediaItem> items;
    for (const std::string& f : s.files) {
        bool have = false;
        for (const MediaItem& m : app.project().media)
            if (!m.generated && m.path == f && m.info.hasVideo) { items.push_back(m); have = true; break; }
        if (have) continue;
        std::string err;
        std::unique_ptr<MediaReader> r = app.mediaOpener ? app.mediaOpener(f, &err) : nullptr;
        if (!r || !r->info().hasVideo) { if (skipped) skipped->push_back(f); continue; }
        MediaItem m;
        m.path = f;
        m.name = pathFileName(f);
        m.info = r->info();
        items.push_back(m);
    }
    const size_t n = items.size();
    if (n == 0) {
        if (error) *error = s.files.empty() ? "Add some pictures first." : "None of the pictures could be read.";
        return made;
    }
    double overlap = std::max(0.0, std::isfinite(s.overlap) ? s.overlap : 0.0);
    double len = std::isfinite(s.perPicture) ? s.perPicture : 5.0;
    if (s.useTotal && std::isfinite(s.total) && s.total > 0) len = (s.total + (double)(n - 1) * overlap) / (double)n;
    len = std::min(36000.0, std::max(0.1, len));
    overlap = std::min(overlap, len * 0.49);
    const double fps = app.project().settings.fps() > 0 ? app.project().settings.fps() : 30.0;
    auto frames = [fps](double t) { return std::floor(t * fps + 0.5) / fps; };
    len = std::max(1.0 / fps, frames(len));
    overlap = frames(overlap);
    const std::string effect = s.effect;
    const bool random = effect.compare(0, 6, "Random") == 0;
    const bool pan = effect.find("Pan") != std::string::npos, zoom = effect.find("Zoom") != std::string::npos;
    std::vector<std::pair<std::string, std::string>> transitions;
    if (random)
        for (const VideoFxEntry& v : Catalog::get().video())
            if (v.transition && v.ok) transitions.emplace_back(v.id, v.label);
    std::mt19937 rng(s.seed);
    const double start0 = s.startAt >= 0 ? s.startAt : s.atCursor ? app.cursor() : app.project().length();
    const double span = (double)n * len - (double)(n - 1) * overlap;
    app.edit("Insert Slideshow", [&](Project& p) {
        Track* tr = nullptr;
        for (Track& t : p.tracks)
            if (t.id == app.currentTrack && t.type == TrackType::Video) tr = &t;
        if (!tr)
            for (Track& t : p.tracks)
                if (t.type == TrackType::Video) { tr = &t; break; }
        if (tr)
            for (const Event& e : tr->events)
                if (e.start < start0 + span - 1e-9 && e.end() > start0 + 1e-9) { tr = nullptr; break; }
        if (!tr) {
            Track t;
            t.id = p.newId();
            t.type = TrackType::Video;
            t.name = "Slideshow";
            p.tracks.insert(p.tracks.begin(), t);
            tr = &p.tracks.front();
        }
        for (size_t k = 0; k < n; ++k) {
            uint64_t mediaId = 0;
            for (const MediaItem& m : p.media)
                if (!m.generated && m.path == items[k].path) mediaId = m.id;
            if (!mediaId) {
                MediaItem m = items[k];
                m.id = mediaId = p.newId();
                p.media.push_back(m);
            }
            Event e;
            e.id = p.newId();
            e.mediaId = mediaId;
            e.start = start0 + (double)k * (len - overlap);
            e.length = len;
            e.name = items[k].name;
            if (random && k > 0 && !transitions.empty()) {
                const auto& t = transitions[std::uniform_int_distribution<size_t>(0, transitions.size() - 1)(rng)];
                e.transition.id = p.newId();
                e.transition.kind = "ofx";
                e.transition.pluginId = t.first;
                e.transition.name = t.second;
            }
            if (pan || zoom) {
                // Event Pan/Crop from a box at the start to one at the end: zoom in or out by a fifth and/or move
                // sideways or up/down by what the smaller box leaves
                PanCropKey a, b;
                b.time = len;
                const double small = 0.8;
                if (zoom) {
                    const bool in = std::uniform_int_distribution<int>(0, 1)(rng) == 0;
                    a.w = a.h = in ? 1.0 : small;
                    b.w = b.h = in ? small : 1.0;
                }
                if (pan) {
                    if (!zoom) a.w = a.h = b.w = b.h = small;
                    const double room = (1.0 - small) / 2;
                    const int dir = std::uniform_int_distribution<int>(0, 3)(rng);
                    double& from = dir < 2 ? a.cx : a.cy;
                    double& to = dir < 2 ? b.cx : b.cy;
                    from = 0.5 + (dir % 2 ? room : -room);
                    to = 0.5 + (dir % 2 ? -room : room);
                    if (zoom) {
                        const double ra = (1.0 - a.w) / 2, rb = (1.0 - b.w) / 2;
                        from = 0.5 + (from - 0.5) * (room > 0 ? ra / room : 0);
                        to = 0.5 + (to - 0.5) * (room > 0 ? rb / room : 0);
                    }
                }
                e.panCrop = {a, b};
            }
            tr->events.push_back(e);
            made.push_back(e.id);
        }
        std::stable_sort(tr->events.begin(), tr->events.end(), [](const Event& x, const Event& y) { return x.start < y.start; });
    });
    std::set<uint64_t> sel(made.begin(), made.end());
    app.setSelection(sel);
    return made;
}

namespace {

// A flat glyph for the Slideshow Creator's toolbar (Reno's own drawing).
QIcon slideshowGlyph(const QString& kind)
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink(200, 200, 200), gold(214, 178, 64);
    p.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    if (kind == "add") {
        p.setPen(Qt::NoPen);
        p.setBrush(gold);
        p.drawRect(QRectF(1.5, 4, 5.5, 2.5));
        p.drawRect(QRectF(1.5, 6, 13, 7.5));
    } else if (kind == "delete") {
        p.drawLine(QPointF(3.5, 3.5), QPointF(12.5, 12.5));
        p.drawLine(QPointF(12.5, 3.5), QPointF(3.5, 12.5));
    } else if (kind == "ccw" || kind == "cw") {
        const bool cw = kind == "cw";
        p.drawArc(QRectF(3, 4, 10, 10), cw ? 60 * 16 : -240 * 16, 240 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        const double x = cw ? 12.5 : 3.5;
        p.drawPolygon(QPolygonF() << QPointF(x - 2.5, 2) << QPointF(x + 2.5, 2) << QPointF(x, 6));
    } else if (kind == "up" || kind == "down") {
        const bool up = kind == "up";
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPolygon(up ? QPolygonF() << QPointF(8, 3) << QPointF(13, 9) << QPointF(3, 9) : QPolygonF() << QPointF(8, 13) << QPointF(13, 7) << QPointF(3, 7));
        p.drawRect(up ? QRectF(6, 9, 4, 4) : QRectF(6, 3, 4, 4));
    } else if (kind == "shuffle") {
        p.drawLine(QPointF(2, 4.5), QPointF(6, 4.5));
        p.drawLine(QPointF(6, 4.5), QPointF(10, 11.5));
        p.drawLine(QPointF(10, 11.5), QPointF(13, 11.5));
        p.drawLine(QPointF(2, 11.5), QPointF(6, 11.5));
        p.drawLine(QPointF(6, 11.5), QPointF(10, 4.5));
        p.drawLine(QPointF(10, 4.5), QPointF(13, 4.5));
    } else {
        // sorts: a pointer with a bar
        const bool up = kind == "az" || kind == "oldnew";
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawPolygon(up ? QPolygonF() << QPointF(8, 3) << QPointF(12, 8) << QPointF(4, 8) : QPolygonF() << QPointF(8, 13) << QPointF(12, 8) << QPointF(4, 8));
        p.drawRect(QRectF(6.5, 8, 3, 5));
        if (kind == "oldnew" || kind == "newold") {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(ink, 1.2));
            p.drawEllipse(QRectF(1.5, 1.5, 4, 4));
        }
    }
    return QIcon(pm);
}

// "00:01:30.50", "1:30.5" or plain seconds, as the Slideshow Creator's time boxes show and take them.
double slideshowTime(const QString& text, bool* ok)
{
    const QStringList parts = text.trimmed().split(':');
    double t = 0;
    bool good = !text.trimmed().isEmpty() && parts.size() <= 3;
    for (const QString& p : parts) {
        bool k = false;
        const double v = p.toDouble(&k);
        if (!k || v < 0) good = false;
        t = t * 60 + v;
    }
    if (ok) *ok = good;
    return good ? t : 0;
}

QString slideshowTimeText(double t)
{
    t = std::max(0.0, t);
    const int cs = (int)std::lround(t * 100);
    return QString("%1:%2:%3.%4").arg(cs / 360000, 2, 10, QChar('0')).arg(cs / 6000 % 60, 2, 10, QChar('0')).arg(cs / 100 % 60, 2, 10, QChar('0')).arg(cs % 100, 2, 10, QChar('0'));
}

}  // namespace

namespace {

// Insert > Slideshow Creator (VEGAS 18): pictures listed on one page, placement, duration and effects on the other, then
// Create puts the slideshow on the timeline. Laid out at VEGAS's own positions: VEGAS's window is 538 x 397 with its
// frame; positions below are the capture's less the frame's 1 and 31.
class SlideshowCreator : public QDialog {
public:
    explicit SlideshowCreator(QWidget* parent) : QDialog(parent)
    {
        setObjectName("slideshowCreator");
        setWindowTitle("Slideshow Creator");
        makeDialogDark(this);   // VEGAS 18's Slideshow Creator is a dark window
        applyLook();
        buildTabs();
        buildPicturesPage();
        buildSettingsPage();
        buildStatusStrip();
        connectControls();
        settingsPage_->hide();
        refresh();
    }

private:
    // The window's position for a widget given in VEGAS's window coordinates (with its frame), and the same for a page's.
    static QWidget* at(QWidget* w, int x, int y, int cw, int ch)
    {
        w->setGeometry(x - 1, y - 31, cw, ch);
        return w;
    }
    static QWidget* pageAt(QWidget* w, int x, int y, int cw, int ch)
    {
        w->setGeometry(x - 7, y - 59, cw, ch);
        return w;
    }
    // A widget placed in one of the Settings page's group boxes, at window coordinates.
    QWidget* inGroup(QWidget* w, QGroupBox* g, int x, int y, int cw, int ch)
    {
        w->setParent(g);
        w->setGeometry(x - (10 + 0), y - (g == placement_ ? 65 : g == duration_ ? 142 : 224), cw, ch);
        w->show();
        return w;
    }

    void applyLook()
    {
        QFont f = font();
        f.setPixelSize(12);
        setFont(f);
        setFixedSize(538 - 2, 397 - 31 - 1);
        QPalette pal = palette();
        const QColor dark(45, 45, 45);
        pal.setColor(QPalette::Window, dark);
        pal.setColor(QPalette::Base, dark);
        pal.setColor(QPalette::Button, dark);
        pal.setColor(QPalette::WindowText, QColor(230, 230, 230));
        pal.setColor(QPalette::Text, QColor(230, 230, 230));
        pal.setColor(QPalette::ButtonText, QColor(230, 230, 230));
        for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) pal.setColor(QPalette::Disabled, r, QColor(110, 110, 110));
        setPalette(pal);
        setStyleSheet("QDialog { background: #464646; }"
                      "QTabWidget::pane { border: none; background: #2d2d2d; }"
                      "QTabBar::tab { background: #464646; color: #d0d0d0; padding: 0px; border: none; min-width: 65px; max-width: 65px; min-height: 21px; max-height: 21px; }"
                      "QTabBar::tab:selected { background: #2d2d2d; color: #ffffff; font-weight: bold; }"
                      "QGroupBox { border: 1px solid #c8c8c8; margin: 0px; padding: 0px; }"
                      "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0px 1px; }"
                      "QLineEdit, QAbstractSpinBox { background: #2d2d2d; border: 1px solid #646464; color: #e6e6e6; selection-background-color: #12b0ff; }"
                      "QAbstractSpinBox { background: #828282; color: #ffffff; }"
                      "QAbstractSpinBox:disabled { background: #2d2d2d; color: #6e6e6e; }"
                      "QLineEdit:disabled { color: #6e6e6e; }"
                      "QPushButton { background: #2d2d2d; border: 1px solid #5a5a5a; color: #e6e6e6; }"
                      "QPushButton:hover:enabled { border-color: #12b0ff; }"
                      "QPushButton:disabled { color: #6e6e6e; border-color: #464646; }"
                      "QComboBox { background: #2d2d2d; border: 1px solid #12b0ff; color: #ffffff; padding-left: 3px; }"
                      "QComboBox::drop-down { border: none; width: 17px; }"
                      "QToolButton { background: transparent; border: 1px solid transparent; }"
                      "QToolButton:hover:enabled { border-color: #12b0ff; }");
    }

    // the two tabs and their pages (7,59 523x278 in the window)
    void buildTabs()
    {
        tabs_ = new QTabBar(this);
        tabs_->setObjectName("slideshowTabs");
        tabs_->setDrawBase(false);
        tabs_->setExpanding(false);
        tabs_->setUsesScrollButtons(false);
        tabs_->setStyleSheet("QTabBar::tab { padding: 0px 8px; min-height: 21px; }");
        tabs_->addTab("Pictures");
        tabs_->addTab("Settings");
        at(tabs_, 5, 36, 131, 21);
        picturesPage_ = makePage();
        settingsPage_ = makePage();
    }
    QWidget* makePage()
    {
        QWidget* pg = new QWidget(this);
        pg->setAutoFillBackground(true);
        at(pg, 7, 59, 523, 278);
        return pg;
    }

    // Pictures: the tool bar on top, the pictures under it (positions relative to the page at window 7,59); "Add
    // Pictures" in large letters while there are none.
    void buildPicturesPage()
    {
        list_ = new QListWidget(picturesPage_);
        list_->setObjectName("slideshowPictures");
        list_->setViewMode(QListView::IconMode);
        list_->setIconSize(QSize(64, 48));
        list_->setResizeMode(QListView::Adjust);
        list_->setMovement(QListView::Static);
        list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
        list_->setFrameShape(QFrame::NoFrame);
        list_->setStyleSheet("QListWidget { background: #2d2d2d; color: #e6e6e6; border: 1px solid #464646; }");
        // the picture area starts 59 px down the window, under the 30 px toolbar and its rule
        QWidget* bar = new QWidget(picturesPage_);
        bar->setObjectName("slideshowToolbar");
        bar->setStyleSheet("QWidget#slideshowToolbar { background: #2d2d2d; border-bottom: 1px solid #646464; }");
        struct B { const char* name; const char* tip; const char* glyph; int x; };
        const B buttons[] = {{"slideshowAdd", "Add Pictures...", "add", 7},
                             {"slideshowRemove", "Delete Selected", "delete", 31},
                             {"slideshowRotateLeft", "Rotate Counterclockwise", "ccw", 59},
                             {"slideshowRotateRight", "Rotate Clockwise", "cw", 83},
                             {"slideshowUp", "Move Selected -1", "up", 111},
                             {"slideshowDown", "Move Selected +1", "down", 135},
                             {"slideshowSortAZ", "Sort A to Z", "az", 163},
                             {"slideshowSortZA", "Sort Z to A", "za", 187},
                             {"slideshowSortNew", "Sort New to Old", "newold", 211},
                             {"slideshowSortOld", "Sort Old to New", "oldnew", 235},
                             {"slideshowShuffle", "Shuffle", "shuffle", 259}};
        for (const B& b : buttons) {
            QToolButton* t = new QToolButton(bar);
            t->setObjectName(b.name);
            t->setToolTip(b.tip);
            t->setIcon(slideshowGlyph(b.glyph));
            t->setGeometry(b.x - 7, 0, 24, 22);
            t->setAutoRaise(true);
            tools_[b.name] = t;
        }
        link_ = new QLabel("<a href=\"add\" style=\"color:#0000ee\">Add Pictures</a>", picturesPage_);
        link_->setObjectName("slideshowAddLink");
        link_->setAlignment(Qt::AlignCenter);
        QFont lf = link_->font();
        lf.setPixelSize(22);
        link_->setFont(lf);
        // the picture explorer fills the page (7,59 523x278); Reno's rotate buttons stay off (pictures are not rotated by the slideshow)
        tools_["slideshowRotateLeft"]->setEnabled(false);
        tools_["slideshowRotateRight"]->setEnabled(false);
        tools_["slideshowRotateLeft"]->setToolTip("Rotate Counterclockwise (not available in Reno)");
        tools_["slideshowRotateRight"]->setToolTip("Rotate Clockwise (not available in Reno)");
        bar->setGeometry(0, 0, 523, 31);
        list_->setGeometry(0, 31, 523, 247);
        link_->setGeometry(0, 90 - 59, 523, 85);
    }

    // Settings: placement, duration and effects, and the defaults buttons.
    void buildSettingsPage()
    {
        QSettings st;
        placement_ = new QGroupBox("Slideshow Placement", settingsPage_);
        pageAt(placement_, 10, 65, 515, 71);
        duration_ = new QGroupBox("Duration", settingsPage_);
        pageAt(duration_, 10, 142, 515, 76);
        effects_ = new QGroupBox("Effects", settingsPage_);
        pageAt(effects_, 10, 224, 515, 76);
        atCursor_ = new QRadioButton("Start at cursor", placement_);
        atCursor_->setObjectName("slideshowAtCursor");
        atTime_ = new QRadioButton("Start at:", placement_);
        atTime_->setObjectName("slideshowAtTime");
        startEdit_ = new QLineEdit("00:00:00.00", placement_);
        startEdit_->setObjectName("slideshowStartAt");
        inGroup(atCursor_, placement_, 19, 84, 93, 18);
        inGroup(atTime_, placement_, 19, 107, 62, 18);
        inGroup(startEdit_, placement_, 173, 106, 121, 20);

        perRadio_ = new QRadioButton("Time per picture (sec):", duration_);
        perRadio_->setObjectName("slideshowPerPicture");
        totalRadio_ = new QRadioButton("Total length:", duration_);
        totalRadio_->setObjectName("slideshowTotal");
        per_ = new QDoubleSpinBox(duration_);
        per_->setObjectName("slideshowPerSeconds");
        per_->setRange(0.1, 3600);
        per_->setDecimals(2);
        per_->setValue(st.value("slideshow/perPicture", 3.0).toDouble());
        totalEdit_ = new QLineEdit(slideshowTimeText(st.value("slideshow/total", 60.0).toDouble()), duration_);
        totalEdit_->setObjectName("slideshowTotalLength");
        perLabel_ = new QLabel("Time per picture (sec): 0.00", duration_);
        perLabel_->setObjectName("slideshowPerLabel");
        inGroup(perRadio_, duration_, 19, 162, 135, 18);
        inGroup(per_, duration_, 173, 161, 120, 20);
        inGroup(totalRadio_, duration_, 19, 189, 85, 18);
        inGroup(totalEdit_, duration_, 173, 188, 121, 20);
        inGroup(perLabel_, duration_, 318, 191, 142, 17);

        QLabel* effLabel = new QLabel("Effect:", effects_);
        effect_ = new QComboBox(effects_);
        effect_->setObjectName("slideshowEffect");
        for (const std::string& e : slideshowEffects()) effect_->addItem(QString::fromStdString(e));
        effect_->setCurrentText(st.value("slideshow/effect", "Random").toString());
        effect_->setToolTip("Fades: cross-fades where the pictures overlap. Random: a transition picked at random for each.\n"
                           "With Pan / Zoom: each picture moves across or zooms (Event Pan/Crop keyframes).");
        QLabel* ovLabel = new QLabel("Overlap (sec):", effects_);
        overlap_ = new QDoubleSpinBox(effects_);
        overlap_->setObjectName("slideshowOverlap");
        overlap_->setRange(0, 60);
        overlap_->setDecimals(2);
        overlap_->setValue(st.value("slideshow/overlap", 1.0).toDouble());
        inGroup(effLabel, effects_, 16, 246, 36, 17);
        inGroup(effect_, effects_, 173, 243, 121, 21);
        inGroup(ovLabel, effects_, 16, 273, 75, 17);
        inGroup(overlap_, effects_, 173, 271, 120, 20);
        (st.value("slideshow/useTotal", false).toBool() ? totalRadio_ : perRadio_)->setChecked(true);
        (st.value("slideshow/atCursor", true).toBool() ? atCursor_ : atTime_)->setChecked(true);

        defaultAll_ = new QPushButton("Default All", settingsPage_);
        defaultAll_->setObjectName("slideshowDefaultAll");
        pageAt(defaultAll_, 10, 306, 100, 24);
        useAsDefaults_ = new QPushButton("Use as Defaults", settingsPage_);
        useAsDefaults_->setObjectName("slideshowUseAsDefaults");
        pageAt(useAsDefaults_, 117, 306, 110, 24);
    }

    // the strip under the tabs: Create, and the number of pictures and the slideshow's length
    void buildStatusStrip()
    {
        create_ = new QPushButton("Create", this);
        create_->setObjectName("ok");
        at(create_, 447, 346, 85, 24);
        QWidget* strip = new QWidget(this);
        strip->setObjectName("slideshowStatusStrip");
        at(strip, -1, 376, 540, 22);
        strip->setStyleSheet("QWidget#slideshowStatusStrip { background: #2d2d2d; border-top: 1px solid #646464; }");
        count_ = new QLabel("Total: 0", this);
        count_->setObjectName("slideshowStatus");
        at(count_, 0, 377, 45, 20);
        length_ = new QLabel("00:00:00.00", this);
        length_->setObjectName("slideshowLength");
        length_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        length_->setStyleSheet("color: #12b0ff;");
        at(length_, 471, 377, 67, 20);
        count_->setStyleSheet("background: transparent; padding-left: 5px;");
        count_->raise();
        length_->raise();
    }

    void connectControls()
    {
        QObject::connect(tools_.at("slideshowAdd"), &QToolButton::clicked, this, [this] { addPictures(); });
        QObject::connect(link_, &QLabel::linkActivated, this, [this](const QString&) { addPictures(); });
        QObject::connect(tools_.at("slideshowRemove"), &QToolButton::clicked, this, [this] { removeSelected(); });
        QObject::connect(list_, &QListWidget::itemSelectionChanged, this, [this] { refresh(); });
        QObject::connect(tools_.at("slideshowUp"), &QToolButton::clicked, this, [this] { moveSelected(-1); });
        QObject::connect(tools_.at("slideshowDown"), &QToolButton::clicked, this, [this] { moveSelected(1); });
        QObject::connect(tools_.at("slideshowSortAZ"), &QToolButton::clicked, this, [this] { sortPictures(SortAToZ); });
        QObject::connect(tools_.at("slideshowSortZA"), &QToolButton::clicked, this, [this] { sortPictures(SortZToA); });
        QObject::connect(tools_.at("slideshowSortNew"), &QToolButton::clicked, this, [this] { sortPictures(SortNewToOld); });
        QObject::connect(tools_.at("slideshowSortOld"), &QToolButton::clicked, this, [this] { sortPictures(SortOldToNew); });
        QObject::connect(tools_.at("slideshowShuffle"), &QToolButton::clicked, this, [this] { sortPictures(Shuffle); });
        for (QRadioButton* r : {perRadio_, totalRadio_, atCursor_, atTime_}) QObject::connect(r, &QRadioButton::toggled, this, [this] { refresh(); });
        for (QDoubleSpinBox* sp : {per_, overlap_}) QObject::connect(sp, &QDoubleSpinBox::valueChanged, this, [this] { refresh(); });
        QObject::connect(totalEdit_, &QLineEdit::textChanged, this, [this] { refresh(); });
        // Default All: VEGAS's own settings; Use as Defaults: the ones shown are kept for the next slideshow
        QObject::connect(useAsDefaults_, &QPushButton::clicked, this, [this] { rememberSettings(); });
        QObject::connect(defaultAll_, &QPushButton::clicked, this, [this] { restoreDefaults(); });
        QObject::connect(create_, &QPushButton::clicked, this, [this] { create(); });
        QObject::connect(tabs_, &QTabBar::currentChanged, this, [this](int i) {
            picturesPage_->setVisible(i == 0);
            settingsPage_->setVisible(i == 1);
        });
    }

    std::vector<std::string> files() const
    {
        std::vector<std::string> fl;
        for (int i = 0; i < list_->count(); ++i) fl.push_back(list_->item(i)->data(Qt::UserRole).toString().toStdString());
        return fl;
    }

    // The status strip, the per-picture time and which controls can be used, for the pictures and settings now.
    void refresh()
    {
        const int n = list_->count();
        double len = per_->value(), ov = std::min(overlap_->value(), per_->value() * 0.49);
        bool okTime = false;
        const double tot = slideshowTime(totalEdit_->text(), &okTime);
        double whole = n > 0 ? n * len - (n - 1) * ov : 0;
        if (totalRadio_->isChecked()) whole = n > 0 && okTime ? tot : 0;
        count_->setText(QString("Total: %1").arg(n));
        length_->setText(slideshowTimeText(whole));
        create_->setEnabled(n > 0);
        per_->setEnabled(perRadio_->isChecked());
        totalEdit_->setEnabled(totalRadio_->isChecked());
        startEdit_->setEnabled(atTime_->isChecked());
        perLabel_->setEnabled(false);
        perLabel_->setText(QString("Time per picture (sec): %1").arg(n > 0 ? (totalRadio_->isChecked() ? (okTime ? (tot + (n - 1) * ov) / n : 0.0) : len) : 0.0, 0, 'f', 2));
        link_->setVisible(n == 0);
        tools_.at("slideshowRemove")->setEnabled(!list_->selectedItems().isEmpty());
        for (const char* nm : {"slideshowUp", "slideshowDown"}) tools_.at(nm)->setEnabled(list_->currentRow() >= 0);
        for (const char* nm : {"slideshowSortAZ", "slideshowSortZA", "slideshowSortNew", "slideshowSortOld", "slideshowShuffle"}) tools_.at(nm)->setEnabled(n > 1);
    }

    void addFile(const QString& fn)
    {
        QListWidgetItem* it = new QListWidgetItem(QFileInfo(fn).fileName(), list_);
        it->setData(Qt::UserRole, fn);
        it->setToolTip(fn);
        const QPixmap pm(fn);
        if (!pm.isNull()) it->setIcon(QIcon(pm.scaled(64, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    }

    void addPictures()
    {
        QSettings st;
        const QString dir = st.value("slideshow/folder", QDir::homePath()).toString();
        const QStringList fs = QFileDialog::getOpenFileNames(this, "Add Pictures", dir,
                                                             "Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.tga);;All files (*)");
        if (!fs.isEmpty()) st.setValue("slideshow/folder", QFileInfo(fs.first()).absolutePath());
        for (const QString& fn : fs) addFile(fn);
        refresh();
    }

    void removeSelected()
    {
        for (QListWidgetItem* it : list_->selectedItems()) delete it;
        refresh();
    }

    void moveSelected(int dir)
    {
        const int r = list_->currentRow(), to = r + dir;
        if (r < 0 || to < 0 || to >= list_->count()) return;
        QListWidgetItem* it = list_->takeItem(r);
        list_->insertItem(to, it);
        list_->setCurrentRow(to);
    }

    enum SortOrder { SortAToZ = 1, SortZToA, SortNewToOld, SortOldToNew, Shuffle };
    void sortPictures(SortOrder mode)
    {
        std::vector<QString> fs;
        for (int k = 0; k < list_->count(); ++k) fs.push_back(list_->item(k)->data(Qt::UserRole).toString());
        auto name = [](const QString& fn) { return QFileInfo(fn).fileName().toLower(); };
        auto when = [](const QString& fn) { return QFileInfo(fn).lastModified(); };
        if (mode == Shuffle) std::shuffle(fs.begin(), fs.end(), std::mt19937((unsigned)QDateTime::currentMSecsSinceEpoch()));
        else std::stable_sort(fs.begin(), fs.end(), [&](const QString& a, const QString& b) {
            switch (mode) {
            case SortAToZ: return name(a) < name(b);
            case SortZToA: return name(b) < name(a);
            case SortNewToOld: return when(b) < when(a);
            default: return when(a) < when(b);
            }
        });
        list_->clear();
        for (const QString& fn : fs) addFile(fn);
        refresh();
    }

    void rememberSettings()
    {
        QSettings st;
        bool k = false;
        st.setValue("slideshow/perPicture", per_->value());
        const double t = slideshowTime(totalEdit_->text(), &k);
        if (k) st.setValue("slideshow/total", t);
        st.setValue("slideshow/overlap", overlap_->value());
        st.setValue("slideshow/useTotal", totalRadio_->isChecked());
        st.setValue("slideshow/effect", effect_->currentText());
        st.setValue("slideshow/atCursor", atCursor_->isChecked());
    }

    void restoreDefaults()
    {
        atCursor_->setChecked(true);
        perRadio_->setChecked(true);
        per_->setValue(3.0);
        totalEdit_->setText("00:01:00.00");
        effect_->setCurrentText("Random");
        overlap_->setValue(1.0);
    }

    // Create: the slideshow made on the timeline (the settings kept for next time), naming any pictures that could not be
    // added.
    void create()
    {
        SlideshowSettings s;
        s.files = files();
        s.useTotal = totalRadio_->isChecked();
        s.perPicture = per_->value();
        bool okTotal = false;
        const double tot = slideshowTime(totalEdit_->text(), &okTotal);
        s.total = okTotal && tot > 0 ? tot : 60.0;
        s.overlap = overlap_->value();
        s.effect = effect_->currentText().toStdString();
        s.atCursor = atCursor_->isChecked();
        if (atTime_->isChecked()) {
            bool okStart = false;
            const double at0 = slideshowTime(startEdit_->text(), &okStart);
            if (okStart) s.startAt = at0;
        }
        s.seed = (unsigned)QDateTime::currentMSecsSinceEpoch();
        rememberSettings();
        std::string err;
        std::vector<std::string> skipped;
        const std::vector<uint64_t> made = makeSlideshow(s, &err, &skipped);
        if (made.empty()) {
            QMessageBox::warning(this, "Slideshow Creator", QString::fromStdString(err));
            return;
        }
        if (!skipped.empty()) {
            QStringList names;
            for (const std::string& fn : skipped) names << QFileInfo(QString::fromStdString(fn)).fileName();
            QMessageBox::warning(this, "Slideshow Creator", "Warning: Some files could not be added to the slideshow:\n" + names.join("\n"));
        }
        accept();
    }

    QTabBar* tabs_ = nullptr;
    QWidget *picturesPage_ = nullptr, *settingsPage_ = nullptr;
    QListWidget* list_ = nullptr;
    std::map<QString, QToolButton*> tools_;
    QLabel* link_ = nullptr;
    QGroupBox *placement_ = nullptr, *duration_ = nullptr, *effects_ = nullptr;
    QRadioButton *atCursor_ = nullptr, *atTime_ = nullptr, *perRadio_ = nullptr, *totalRadio_ = nullptr;
    QLineEdit *startEdit_ = nullptr, *totalEdit_ = nullptr;
    QDoubleSpinBox *per_ = nullptr, *overlap_ = nullptr;
    QLabel* perLabel_ = nullptr;
    QComboBox* effect_ = nullptr;
    QPushButton *defaultAll_ = nullptr, *useAsDefaults_ = nullptr, *create_ = nullptr;
    QLabel *count_ = nullptr, *length_ = nullptr;
};

}  // namespace

void runSlideshowCreator(QWidget* parent)
{
    SlideshowCreator d(parent);
    if (slideshowDialogShown) QMetaObject::invokeMethod(&d, [&d] { slideshowDialogShown(&d); }, Qt::QueuedConnection);
    d.exec();
}

}  // namespace app
}  // namespace reno
