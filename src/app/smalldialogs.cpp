// Small VEGAS dialogs (see smalldialogs.h).
#include "smalldialogs.h"
#include "appstate.h"
#include "keyboard.h"
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
    const double start0 = s.atCursor ? app.cursor() : app.project().length();
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

void runSlideshowCreator(QWidget* parent)
{
    QSettings st;
    QDialog d(parent);
    d.setObjectName("slideshowCreator");
    d.setWindowTitle("Slideshow Creator");
    d.resize(560, 520);
    QVBoxLayout* v = new QVBoxLayout(&d);
    QGroupBox* pics = new QGroupBox("Pictures", &d);
    QVBoxLayout* pv = new QVBoxLayout(pics);
    QListWidget* list = new QListWidget(pics);
    list->setObjectName("slideshowPictures");
    list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list->setDragDropMode(QAbstractItemView::InternalMove);
    pv->addWidget(list, 1);
    QHBoxLayout* pb = new QHBoxLayout();
    QPushButton* add = new QPushButton("Add Pictures...", pics);
    add->setObjectName("slideshowAdd");
    QPushButton* del = new QPushButton("Remove", pics);
    del->setObjectName("slideshowRemove");
    QPushButton* up = new QPushButton("Move Up", pics);
    QPushButton* down = new QPushButton("Move Down", pics);
    QComboBox* sort = new QComboBox(pics);
    sort->setObjectName("slideshowSort");
    sort->addItems({"Sort By...", "Sort A to Z", "Sort Z to A", "Sort Old to New", "Sort New to Old"});
    for (QWidget* w : std::initializer_list<QWidget*>{add, del, up, down, sort}) pb->addWidget(w);
    pv->addLayout(pb);
    v->addWidget(pics, 1);
    QGroupBox* dur = new QGroupBox("Duration", &d);
    QFormLayout* df = new QFormLayout(dur);
    QRadioButton* perRadio = new QRadioButton("Time per picture (sec):", dur);
    QRadioButton* totalRadio = new QRadioButton("Total length (sec):", dur);
    perRadio->setObjectName("slideshowPerPicture");
    totalRadio->setObjectName("slideshowTotal");
    QDoubleSpinBox* per = new QDoubleSpinBox(dur);
    per->setObjectName("slideshowPerSeconds");
    per->setRange(0.1, 3600);
    per->setDecimals(2);
    per->setValue(st.value("slideshow/perPicture", 5.0).toDouble());
    QDoubleSpinBox* total = new QDoubleSpinBox(dur);
    total->setObjectName("slideshowTotalSeconds");
    total->setRange(0.1, 36000);
    total->setDecimals(2);
    total->setValue(st.value("slideshow/total", 60.0).toDouble());
    QDoubleSpinBox* overlap = new QDoubleSpinBox(dur);
    overlap->setObjectName("slideshowOverlap");
    overlap->setRange(0, 60);
    overlap->setDecimals(2);
    overlap->setValue(st.value("slideshow/overlap", 1.0).toDouble());
    (st.value("slideshow/useTotal", false).toBool() ? totalRadio : perRadio)->setChecked(true);
    df->addRow(perRadio, per);
    df->addRow(totalRadio, total);
    df->addRow("Overlap (sec):", overlap);
    v->addWidget(dur);
    QGroupBox* eff = new QGroupBox("Effects", &d);
    QFormLayout* ef = new QFormLayout(eff);
    QComboBox* effect = new QComboBox(eff);
    effect->setObjectName("slideshowEffect");
    for (const std::string& e : slideshowEffects()) effect->addItem(QString::fromStdString(e));
    effect->setCurrentText(st.value("slideshow/effect", "Fades").toString());
    effect->setToolTip("Fades: cross-fades where the pictures overlap. Random: a transition picked at random for each.\n"
                       "With Pan / Zoom: each picture moves across or zooms (Event Pan/Crop keyframes).");
    ef->addRow("Effect:", effect);
    v->addWidget(eff);
    QGroupBox* place = new QGroupBox("Slideshow Placement", &d);
    QHBoxLayout* pl = new QHBoxLayout(place);
    QRadioButton* atCursor = new QRadioButton("Start at cursor", place);
    QRadioButton* atEnd = new QRadioButton("At the end of the project", place);
    atCursor->setObjectName("slideshowAtCursor");
    atEnd->setObjectName("slideshowAtEnd");
    (st.value("slideshow/atCursor", true).toBool() ? atCursor : atEnd)->setChecked(true);
    pl->addWidget(atCursor);
    pl->addWidget(atEnd);
    v->addWidget(place);
    QLabel* status = new QLabel(&d);
    status->setObjectName("slideshowStatus");
    v->addWidget(status);
    QDialogButtonBox* bb = new QDialogButtonBox(&d);
    QPushButton* create = bb->addButton("Insert Slideshow", QDialogButtonBox::AcceptRole);
    create->setObjectName("ok");
    QPushButton* close = bb->addButton(QDialogButtonBox::Close);
    close->setObjectName("cancel");
    v->addWidget(bb);

    auto files = [list] {
        std::vector<std::string> f;
        for (int i = 0; i < list->count(); ++i) f.push_back(list->item(i)->data(Qt::UserRole).toString().toStdString());
        return f;
    };
    auto update = [=] {
        const int n = list->count();
        double len = per->value(), ov = std::min(overlap->value(), per->value() * 0.49);
        double whole = n > 0 ? n * len - (n - 1) * ov : 0;
        if (totalRadio->isChecked()) whole = n > 0 ? total->value() : 0;
        status->setText(QString("Total: %1 picture(s), Length: %2 s").arg(n).arg(whole, 0, 'f', 2));
        create->setEnabled(n > 0);
        per->setEnabled(perRadio->isChecked());
        total->setEnabled(totalRadio->isChecked());
    };
    auto addFile = [list](const QString& f) {
        QListWidgetItem* it = new QListWidgetItem(QFileInfo(f).fileName(), list);
        it->setData(Qt::UserRole, f);
        it->setToolTip(f);
    };
    QObject::connect(add, &QPushButton::clicked, &d, [&, addFile, update] {
        const QString dir = st.value("slideshow/folder", QDir::homePath()).toString();
        const QStringList fs = QFileDialog::getOpenFileNames(&d, "Add Pictures", dir,
                                                             "Pictures (*.png *.jpg *.jpeg *.bmp *.gif *.tif *.tiff *.webp *.tga);;All files (*)");
        if (!fs.isEmpty()) st.setValue("slideshow/folder", QFileInfo(fs.first()).absolutePath());
        for (const QString& f : fs) addFile(f);
        update();
    });
    QObject::connect(del, &QPushButton::clicked, &d, [list, update] {
        for (QListWidgetItem* it : list->selectedItems()) delete it;
        update();
    });
    auto move = [list](int dir) {
        const int r = list->currentRow(), to = r + dir;
        if (r < 0 || to < 0 || to >= list->count()) return;
        QListWidgetItem* it = list->takeItem(r);
        list->insertItem(to, it);
        list->setCurrentRow(to);
    };
    QObject::connect(up, &QPushButton::clicked, &d, [move] { move(-1); });
    QObject::connect(down, &QPushButton::clicked, &d, [move] { move(1); });
    QObject::connect(sort, &QComboBox::activated, &d, [list, sort](int i) {
        if (i <= 0) return;
        std::vector<QString> fs;
        for (int k = 0; k < list->count(); ++k) fs.push_back(list->item(k)->data(Qt::UserRole).toString());
        auto name = [](const QString& f) { return QFileInfo(f).fileName().toLower(); };
        auto when = [](const QString& f) { return QFileInfo(f).lastModified(); };
        std::stable_sort(fs.begin(), fs.end(), [&](const QString& a, const QString& b) {
            switch (i) {
            case 1: return name(a) < name(b);
            case 2: return name(b) < name(a);
            case 3: return when(a) < when(b);
            default: return when(b) < when(a);
            }
        });
        list->clear();
        for (const QString& f : fs) {
            QListWidgetItem* it = new QListWidgetItem(QFileInfo(f).fileName(), list);
            it->setData(Qt::UserRole, f);
            it->setToolTip(f);
        }
        sort->setCurrentIndex(0);
    });
    for (QRadioButton* r : {perRadio, totalRadio}) QObject::connect(r, &QRadioButton::toggled, &d, [update] { update(); });
    for (QDoubleSpinBox* sp : {per, total, overlap}) QObject::connect(sp, &QDoubleSpinBox::valueChanged, &d, [update] { update(); });
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::reject);
    QObject::connect(create, &QPushButton::clicked, &d, [&, files] {
        SlideshowSettings s;
        s.files = files();
        s.useTotal = totalRadio->isChecked();
        s.perPicture = per->value();
        s.total = total->value();
        s.overlap = overlap->value();
        s.effect = effect->currentText().toStdString();
        s.atCursor = atCursor->isChecked();
        s.seed = (unsigned)QDateTime::currentMSecsSinceEpoch();
        st.setValue("slideshow/perPicture", s.perPicture);
        st.setValue("slideshow/total", s.total);
        st.setValue("slideshow/overlap", s.overlap);
        st.setValue("slideshow/useTotal", s.useTotal);
        st.setValue("slideshow/effect", effect->currentText());
        st.setValue("slideshow/atCursor", s.atCursor);
        std::string err;
        std::vector<std::string> skipped;
        const std::vector<uint64_t> made = makeSlideshow(s, &err, &skipped);
        if (made.empty()) {
            QMessageBox::warning(&d, "Slideshow Creator", QString::fromStdString(err));
            return;
        }
        if (!skipped.empty()) {
            QStringList names;
            for (const std::string& f : skipped) names << QFileInfo(QString::fromStdString(f)).fileName();
            QMessageBox::warning(&d, "Slideshow Creator", "Warning: Some files could not be added to the slideshow:\n" + names.join("\n"));
        }
        d.accept();
    });
    update();
    if (slideshowDialogShown) QMetaObject::invokeMethod(&d, [&d] { slideshowDialogShown(&d); }, Qt::QueuedConnection);
    d.exec();
}

}  // namespace app
}  // namespace reno
