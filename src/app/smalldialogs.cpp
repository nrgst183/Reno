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

void runSlideshowCreator(QWidget* parent)
{
    QSettings st;
    QDialog d(parent);
    d.setObjectName("slideshowCreator");
    d.setWindowTitle("Slideshow Creator");
    makeDialogDark(&d);   // VEGAS 18's Slideshow Creator is a dark window
    // VEGAS's window is 538 x 397 with its frame; positions below are the capture's less the frame's 1 and 31
    QFont f = d.font();
    f.setPixelSize(12);
    d.setFont(f);
    d.setFixedSize(538 - 2, 397 - 31 - 1);
    QPalette pal = d.palette();
    const QColor dark(45, 45, 45);
    pal.setColor(QPalette::Window, dark);
    pal.setColor(QPalette::Base, dark);
    pal.setColor(QPalette::Button, dark);
    pal.setColor(QPalette::WindowText, QColor(230, 230, 230));
    pal.setColor(QPalette::Text, QColor(230, 230, 230));
    pal.setColor(QPalette::ButtonText, QColor(230, 230, 230));
    for (QPalette::ColorRole r : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) pal.setColor(QPalette::Disabled, r, QColor(110, 110, 110));
    d.setPalette(pal);
    d.setStyleSheet("QDialog { background: #464646; }"
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
    auto at = [](QWidget* w, int x, int y, int cw, int ch) { w->setGeometry(x - 1, y - 31, cw, ch); return w; };

    // the two tabs and their pages (7,59 523x278 in the window)
    QTabBar* tabs = new QTabBar(&d);
    tabs->setObjectName("slideshowTabs");
    tabs->setDrawBase(false);
    tabs->setExpanding(false);
    tabs->setUsesScrollButtons(false);
    tabs->setStyleSheet("QTabBar::tab { padding: 0px 8px; min-height: 21px; }");
    tabs->addTab("Pictures");
    tabs->addTab("Settings");
    at(tabs, 5, 36, 131, 21);
    auto makePage = [&]() {
        QWidget* pg = new QWidget(&d);
        pg->setAutoFillBackground(true);
        at(pg, 7, 59, 523, 278);
        return pg;
    };

    // ---- Pictures
    QWidget* picPage = makePage();
    QListWidget* list = new QListWidget(picPage);
    list->setObjectName("slideshowPictures");
    list->setViewMode(QListView::IconMode);
    list->setIconSize(QSize(64, 48));
    list->setResizeMode(QListView::Adjust);
    list->setMovement(QListView::Static);
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setFrameShape(QFrame::NoFrame);
    list->setStyleSheet("QListWidget { background: #2d2d2d; color: #e6e6e6; border: 1px solid #464646; }");
    // the picture area starts 59 px down the window, under the 30 px toolbar and its rule
    QWidget* bar = new QWidget(picPage);
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
    std::map<QString, QToolButton*> tb;
    for (const B& b : buttons) {
        QToolButton* t = new QToolButton(bar);
        t->setObjectName(b.name);
        t->setToolTip(b.tip);
        t->setIcon(slideshowGlyph(b.glyph));
        t->setGeometry(b.x - 7, 0, 24, 22);
        t->setAutoRaise(true);
        tb[b.name] = t;
    }
    QToolButton* add = tb["slideshowAdd"];
    QToolButton* del = tb["slideshowRemove"];
    QLabel* link = new QLabel("<a href=\"add\" style=\"color:#0000ee\">Add Pictures</a>", picPage);
    link->setObjectName("slideshowAddLink");
    link->setAlignment(Qt::AlignCenter);
    QFont lf = link->font();
    lf.setPixelSize(22);
    link->setFont(lf);
    // the picture explorer fills the page (7,59 523x278); Reno's rotate buttons stay off (pictures are not rotated by the slideshow)
    tb["slideshowRotateLeft"]->setEnabled(false);
    tb["slideshowRotateRight"]->setEnabled(false);
    tb["slideshowRotateLeft"]->setToolTip("Rotate Counterclockwise (not available in Reno)");
    tb["slideshowRotateRight"]->setToolTip("Rotate Clockwise (not available in Reno)");
    // page geometry: the page is 523 x 278 at window 7,59; the toolbar 30 high, the rule at 89
    // (set after the tab widget has placed the page)

    // ---- Settings
    QWidget* setPage = makePage();
    auto pageAt = [&](QWidget* w, int x, int y, int cw, int ch) { w->setGeometry(x - 7, y - 59, cw, ch); return w; };
    QGroupBox* place = new QGroupBox("Slideshow Placement", setPage);
    pageAt(place, 10, 65, 515, 71);
    QGroupBox* dur = new QGroupBox("Duration", setPage);
    pageAt(dur, 10, 142, 515, 76);
    QGroupBox* eff = new QGroupBox("Effects", setPage);
    pageAt(eff, 10, 224, 515, 76);
    auto place0 = [&](QWidget* w, QGroupBox* g, int x, int y, int cw, int ch) {
        w->setParent(g);
        w->setGeometry(x - (10 + 0), y - (g == place ? 65 : g == dur ? 142 : 224), cw, ch);
        w->show();
        return w;
    };
    QRadioButton* atCursor = new QRadioButton("Start at cursor", place);
    atCursor->setObjectName("slideshowAtCursor");
    QRadioButton* atTime = new QRadioButton("Start at:", place);
    atTime->setObjectName("slideshowAtTime");
    QLineEdit* startEdit = new QLineEdit("00:00:00.00", place);
    startEdit->setObjectName("slideshowStartAt");
    place0(atCursor, place, 19, 84, 93, 18);
    place0(atTime, place, 19, 107, 62, 18);
    place0(startEdit, place, 173, 106, 121, 20);

    QRadioButton* perRadio = new QRadioButton("Time per picture (sec):", dur);
    perRadio->setObjectName("slideshowPerPicture");
    QRadioButton* totalRadio = new QRadioButton("Total length:", dur);
    totalRadio->setObjectName("slideshowTotal");
    QDoubleSpinBox* per = new QDoubleSpinBox(dur);
    per->setObjectName("slideshowPerSeconds");
    per->setRange(0.1, 3600);
    per->setDecimals(2);
    per->setValue(st.value("slideshow/perPicture", 3.0).toDouble());
    QLineEdit* totalEdit = new QLineEdit(slideshowTimeText(st.value("slideshow/total", 60.0).toDouble()), dur);
    totalEdit->setObjectName("slideshowTotalLength");
    QLabel* perLabel = new QLabel("Time per picture (sec): 0.00", dur);
    perLabel->setObjectName("slideshowPerLabel");
    place0(perRadio, dur, 19, 162, 135, 18);
    place0(per, dur, 173, 161, 120, 20);
    place0(totalRadio, dur, 19, 189, 85, 18);
    place0(totalEdit, dur, 173, 188, 121, 20);
    place0(perLabel, dur, 318, 191, 142, 17);

    QLabel* effLabel = new QLabel("Effect:", eff);
    QComboBox* effect = new QComboBox(eff);
    effect->setObjectName("slideshowEffect");
    for (const std::string& e : slideshowEffects()) effect->addItem(QString::fromStdString(e));
    effect->setCurrentText(st.value("slideshow/effect", "Random").toString());
    effect->setToolTip("Fades: cross-fades where the pictures overlap. Random: a transition picked at random for each.\n"
                       "With Pan / Zoom: each picture moves across or zooms (Event Pan/Crop keyframes).");
    QLabel* ovLabel = new QLabel("Overlap (sec):", eff);
    QDoubleSpinBox* overlap = new QDoubleSpinBox(eff);
    overlap->setObjectName("slideshowOverlap");
    overlap->setRange(0, 60);
    overlap->setDecimals(2);
    overlap->setValue(st.value("slideshow/overlap", 1.0).toDouble());
    place0(effLabel, eff, 16, 246, 36, 17);
    place0(effect, eff, 173, 243, 121, 21);
    place0(ovLabel, eff, 16, 273, 75, 17);
    place0(overlap, eff, 173, 271, 120, 20);
    (st.value("slideshow/useTotal", false).toBool() ? totalRadio : perRadio)->setChecked(true);
    (st.value("slideshow/atCursor", true).toBool() ? atCursor : atTime)->setChecked(true);

    QPushButton* defAll = new QPushButton("Default All", setPage);
    defAll->setObjectName("slideshowDefaultAll");
    pageAt(defAll, 10, 306, 100, 24);
    QPushButton* useDef = new QPushButton("Use as Defaults", setPage);
    useDef->setObjectName("slideshowUseAsDefaults");
    pageAt(useDef, 117, 306, 110, 24);

    // ---- the strip under the tabs
    QPushButton* create = new QPushButton("Create", &d);
    create->setObjectName("ok");
    at(create, 447, 346, 85, 24);
    QWidget* strip = new QWidget(&d);
    strip->setObjectName("slideshowStatusStrip");
    at(strip, -1, 376, 540, 22);
    strip->setStyleSheet("QWidget#slideshowStatusStrip { background: #2d2d2d; border-top: 1px solid #646464; }");
    QLabel* count = new QLabel("Total: 0", &d);
    count->setObjectName("slideshowStatus");
    at(count, 0, 377, 45, 20);
    QLabel* lengthLabel = new QLabel("00:00:00.00", &d);
    lengthLabel->setObjectName("slideshowLength");
    lengthLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lengthLabel->setStyleSheet("color: #12b0ff;");
    at(lengthLabel, 471, 377, 67, 20);
    count->setStyleSheet("background: transparent; padding-left: 5px;");
    count->raise();
    lengthLabel->raise();

    // the pictures page: the toolbar on top, the picture area under it (positions relative to the page at window 7,59)
    bar->setGeometry(0, 0, 523, 31);
    list->setGeometry(0, 31, 523, 247);
    link->setGeometry(0, 90 - 59, 523, 85);

    auto files = [list] {
        std::vector<std::string> fl;
        for (int i = 0; i < list->count(); ++i) fl.push_back(list->item(i)->data(Qt::UserRole).toString().toStdString());
        return fl;
    };
    auto update = [=] {
        const int n = list->count();
        double len = per->value(), ov = std::min(overlap->value(), per->value() * 0.49);
        bool okTime = false;
        const double tot = slideshowTime(totalEdit->text(), &okTime);
        double whole = n > 0 ? n * len - (n - 1) * ov : 0;
        if (totalRadio->isChecked()) whole = n > 0 && okTime ? tot : 0;
        count->setText(QString("Total: %1").arg(n));
        lengthLabel->setText(slideshowTimeText(whole));
        create->setEnabled(n > 0);
        per->setEnabled(perRadio->isChecked());
        totalEdit->setEnabled(totalRadio->isChecked());
        startEdit->setEnabled(atTime->isChecked());
        perLabel->setEnabled(false);
        perLabel->setText(QString("Time per picture (sec): %1").arg(n > 0 ? (totalRadio->isChecked() ? (okTime ? (tot + (n - 1) * ov) / n : 0.0) : len) : 0.0, 0, 'f', 2));
        link->setVisible(n == 0);
        del->setEnabled(!list->selectedItems().isEmpty());
        for (const char* nm : {"slideshowUp", "slideshowDown"}) tb.at(nm)->setEnabled(list->currentRow() >= 0);
        for (const char* nm : {"slideshowSortAZ", "slideshowSortZA", "slideshowSortNew", "slideshowSortOld", "slideshowShuffle"}) tb.at(nm)->setEnabled(n > 1);
    };
    auto addFile = [list](const QString& fn) {
        QListWidgetItem* it = new QListWidgetItem(QFileInfo(fn).fileName(), list);
        it->setData(Qt::UserRole, fn);
        it->setToolTip(fn);
        const QPixmap pm(fn);
        if (!pm.isNull()) it->setIcon(QIcon(pm.scaled(64, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    };
    auto addPictures = [&, addFile, update] {
        const QString dir = st.value("slideshow/folder", QDir::homePath()).toString();
        const QStringList fs = QFileDialog::getOpenFileNames(&d, "Add Pictures", dir,
                                                             "Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.tga);;All files (*)");
        if (!fs.isEmpty()) st.setValue("slideshow/folder", QFileInfo(fs.first()).absolutePath());
        for (const QString& fn : fs) addFile(fn);
        update();
    };
    QObject::connect(add, &QToolButton::clicked, &d, addPictures);
    QObject::connect(link, &QLabel::linkActivated, &d, [addPictures](const QString&) { addPictures(); });
    QObject::connect(del, &QToolButton::clicked, &d, [list, update] {
        for (QListWidgetItem* it : list->selectedItems()) delete it;
        update();
    });
    QObject::connect(list, &QListWidget::itemSelectionChanged, &d, [update] { update(); });
    auto move = [list](int dir) {
        const int r = list->currentRow(), to = r + dir;
        if (r < 0 || to < 0 || to >= list->count()) return;
        QListWidgetItem* it = list->takeItem(r);
        list->insertItem(to, it);
        list->setCurrentRow(to);
    };
    QObject::connect(tb["slideshowUp"], &QToolButton::clicked, &d, [move] { move(-1); });
    QObject::connect(tb["slideshowDown"], &QToolButton::clicked, &d, [move] { move(1); });
    auto sortBy = [list, addFile, update](int mode) {
        std::vector<QString> fs;
        for (int k = 0; k < list->count(); ++k) fs.push_back(list->item(k)->data(Qt::UserRole).toString());
        auto name = [](const QString& fn) { return QFileInfo(fn).fileName().toLower(); };
        auto when = [](const QString& fn) { return QFileInfo(fn).lastModified(); };
        if (mode == 5) std::shuffle(fs.begin(), fs.end(), std::mt19937((unsigned)QDateTime::currentMSecsSinceEpoch()));
        else std::stable_sort(fs.begin(), fs.end(), [&](const QString& a, const QString& b) {
            switch (mode) {
            case 1: return name(a) < name(b);
            case 2: return name(b) < name(a);
            case 3: return when(b) < when(a);
            default: return when(a) < when(b);
            }
        });
        list->clear();
        for (const QString& fn : fs) addFile(fn);
        update();
    };
    QObject::connect(tb["slideshowSortAZ"], &QToolButton::clicked, &d, [sortBy] { sortBy(1); });
    QObject::connect(tb["slideshowSortZA"], &QToolButton::clicked, &d, [sortBy] { sortBy(2); });
    QObject::connect(tb["slideshowSortNew"], &QToolButton::clicked, &d, [sortBy] { sortBy(3); });
    QObject::connect(tb["slideshowSortOld"], &QToolButton::clicked, &d, [sortBy] { sortBy(4); });
    QObject::connect(tb["slideshowShuffle"], &QToolButton::clicked, &d, [sortBy] { sortBy(5); });
    for (QRadioButton* r : {perRadio, totalRadio, atCursor, atTime}) QObject::connect(r, &QRadioButton::toggled, &d, [update] { update(); });
    for (QDoubleSpinBox* sp : {per, overlap}) QObject::connect(sp, &QDoubleSpinBox::valueChanged, &d, [update] { update(); });
    QObject::connect(totalEdit, &QLineEdit::textChanged, &d, [update] { update(); });
    // Default All: VEGAS's own settings; Use as Defaults: the ones shown are kept for the next slideshow
    auto remember = [=, &st] {
        bool k = false;
        st.setValue("slideshow/perPicture", per->value());
        const double t = slideshowTime(totalEdit->text(), &k);
        if (k) st.setValue("slideshow/total", t);
        st.setValue("slideshow/overlap", overlap->value());
        st.setValue("slideshow/useTotal", totalRadio->isChecked());
        st.setValue("slideshow/effect", effect->currentText());
        st.setValue("slideshow/atCursor", atCursor->isChecked());
    };
    QObject::connect(useDef, &QPushButton::clicked, &d, remember);
    QObject::connect(defAll, &QPushButton::clicked, &d, [=] {
        atCursor->setChecked(true);
        perRadio->setChecked(true);
        per->setValue(3.0);
        totalEdit->setText("00:01:00.00");
        effect->setCurrentText("Random");
        overlap->setValue(1.0);
    });
    QObject::connect(create, &QPushButton::clicked, &d, [&, files, remember] {
        SlideshowSettings s;
        s.files = files();
        s.useTotal = totalRadio->isChecked();
        s.perPicture = per->value();
        bool okTotal = false;
        const double tot = slideshowTime(totalEdit->text(), &okTotal);
        s.total = okTotal && tot > 0 ? tot : 60.0;
        s.overlap = overlap->value();
        s.effect = effect->currentText().toStdString();
        s.atCursor = atCursor->isChecked();
        if (atTime->isChecked()) {
            bool okStart = false;
            const double at0 = slideshowTime(startEdit->text(), &okStart);
            if (okStart) s.startAt = at0;
        }
        s.seed = (unsigned)QDateTime::currentMSecsSinceEpoch();
        remember();
        std::string err;
        std::vector<std::string> skipped;
        const std::vector<uint64_t> made = makeSlideshow(s, &err, &skipped);
        if (made.empty()) {
            QMessageBox::warning(&d, "Slideshow Creator", QString::fromStdString(err));
            return;
        }
        if (!skipped.empty()) {
            QStringList names;
            for (const std::string& fn : skipped) names << QFileInfo(QString::fromStdString(fn)).fileName();
            QMessageBox::warning(&d, "Slideshow Creator", "Warning: Some files could not be added to the slideshow:\n" + names.join("\n"));
        }
        d.accept();
    });
    QObject::connect(tabs, &QTabBar::currentChanged, &d, [picPage, setPage](int i) {
        picPage->setVisible(i == 0);
        setPage->setVisible(i == 1);
    });
    setPage->hide();
    update();
    if (slideshowDialogShown) QMetaObject::invokeMethod(&d, [&d] { slideshowDialogShown(&d); }, Qt::QueuedConnection);
    d.exec();
}

}  // namespace app
}  // namespace reno
