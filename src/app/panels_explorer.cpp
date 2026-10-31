// The Explorer, Edit Details and the transport bar.
#include "panels_internal.h"
#include "panels_glyph.h"
#include "vprefs.h"
#include "audiotools.h"
#include "../engine/colormgmt.h"
#include "../engine/captions.h"
#include "previewextras.h"
#include "../engine/timefmt.h"
#include "../engine/fxnames.h"
#include "logo.h"
#include "fxlibrary.h"
#include "fxthumbs.h"
#include "playback.h"
#include "timeline.h"
#include "../media/media.h"
#include "lazyrefresh.h"
#include "strconv.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QWheelEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGuiApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QListWidget>
#include <QInputDialog>
#include <QDesktopServices>
#include <QProcess>
#include <QTabWidget>
#include <QPlainTextEdit>
#include <QLocale>
#include <QStyle>
#include <QTreeWidgetItemIterator>
#include <QDateTime>
#include <QContextMenuEvent>
#include <QCursor>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollBar>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QDir>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeView>
#include <QListView>
#include <QDrag>
#include <QActionGroup>
#include <QSplitter>
#include <QFileSystemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace reno {
namespace app {

// The Explorer: media files on this computer as in VEGAS: folders at the left (with Favorites), media files at the right.
namespace {
QStringList mediaPatterns()
{
    return mediaGlobs(MediaKind::Any) << "*.veg" << "*.reno";
}
// VEGAS keeps the favorites in NewExplorerFavorites.txt (one folder per line); Reno uses the same file in its own folder.
QString favoritesFile() { return QString::fromStdString(userDataDir()) + "/NewExplorerFavorites.txt"; }
QStringList readFavorites()
{
    QStringList out;
    QFile f(favoritesFile());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        for (const QByteArray& line : f.readAll().split('\n')) {
            const QString t = QString::fromUtf8(line).trimmed();
            if (!t.isEmpty()) out << t;
        }
    return out;
}
void writeFavorites(const QStringList& l)
{
    QFile f(favoritesFile());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) f.write(l.join("\n").toUtf8() + "\n");
}

// The regions list under the files (Views > Regions): dragging a row drags that part of the file onto the timeline.
class RegionList : public QTreeWidget {
public:
    explicit RegionList(QWidget* parent) : QTreeWidget(parent)
    {
        setObjectName("explorerRegions");
        setHeaderLabels({"Region", "Start", "Length", "Name"});
        setRootIsDecorated(false);
        setDragEnabled(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }
    QString file;

protected:
    void startDrag(Qt::DropActions) override
    {
        QTreeWidgetItem* it = currentItem();
        if (!it || file.isEmpty()) return;
        AppState& app = AppState::get();
        std::string err;
        const uint64_t id = app.importMedia(file.toStdString(), &err);
        if (!id) return;
        app.nextAddRange.mediaId = id;
        app.nextAddRange.in = it->data(1, Qt::UserRole).toDouble();
        app.nextAddRange.out = it->data(2, Qt::UserRole).toDouble();
        app.nextAddRange.streams = 0;
        QMimeData* md = new QMimeData();
        md->setData(kMimeMedia, QByteArray::number((qulonglong)id));
        md->setUrls({QUrl::fromLocalFile(file)});
        QDrag* drag = new QDrag(this);
        drag->setMimeData(md);
        drag->exec(Qt::CopyAction);
    }
};
}  // namespace

struct MediaExplorer::Impl {
    QFileSystemModel* dirs = nullptr;
    QFileSystemModel* files = nullptr;
    QTreeView* tree = nullptr;
    QTreeView* list = nullptr;
    QListView* thumbs = nullptr;
    QListWidget* favorites = nullptr;
    QWidget* folderPane = nullptr;
    RegionList* regions = nullptr;
    QLabel* summary = nullptr;
    QLineEdit* path = nullptr;
    QToolButton *back = nullptr, *forward = nullptr, *autoPreview = nullptr;
    QString folder;
    QStringList history;
    int historyAt = -1;
    bool navigating = false;
    QString view = "details";
    bool showRegions = false, showSummary = true, showTree = true, allFiles = false;
    QString previewing;

    QAbstractItemView* fileView() const { return view == "thumbnail" ? (QAbstractItemView*)thumbs : (QAbstractItemView*)list; }
    QStringList selectedFiles() const
    {
        QStringList out;
        for (const QModelIndex& i : fileView()->selectionModel()->selectedIndexes())
            if (i.column() == 0 && !files->isDir(i)) out << files->filePath(i);
        out.removeDuplicates();
        return out;
    }
    QString selectedPath() const
    {
        const QModelIndex i = fileView()->currentIndex();
        return i.isValid() ? files->filePath(i.siblingAtColumn(0)) : QString();
    }
};

MediaExplorer::MediaExplorer(QWidget* parent) : QWidget(parent), d(new Impl())
{
    setObjectName("explorer");
    Impl* impl = d.get();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget* bar = new QWidget(this);
    bar->setFixedHeight(29);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(4, 2, 4, 2);
    h->setSpacing(3);
    d->path = new QLineEdit(bar);
    d->path->setObjectName("explorerPath");
    auto button = [&](const char* name, const QString& text, const QString& tip) {
        QToolButton* b = new QToolButton(bar);
        b->setObjectName(name);
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        return b;
    };
    d->back = button("explorerBack", QString::fromUtf8("←"), "Back");
    d->forward = button("explorerForward", QString::fromUtf8("→"), "Forward");
    QToolButton* up = button("explorerUp", QString::fromUtf8("↑"), "Up One Level");
    QToolButton* refresh = button("explorerRefresh", QString::fromUtf8("↻"), "Refresh");
    QToolButton* del = button("explorerDelete", QString::fromUtf8("✕"), "Delete: the selected file or folder (to the Recycle Bin)");
    QToolButton* fav = button("explorerAddFavorite", QString::fromUtf8("☆"), "Add to Favorites: the folder shown");
    QToolButton* play = button("explorerStartPreview", QString::fromUtf8("▶"), "Start Preview: plays the selected media file (in the Trimmer)");
    QToolButton* stop = button("explorerStopPreview", QString::fromUtf8("■"), "Stop Preview");
    d->autoPreview = button("explorerAutoPreview", "Auto", "Auto Preview: media files play as they are clicked");
    d->autoPreview->setCheckable(true);
    d->autoPreview->setChecked(QSettings().value("explorer/autoPreview", false).toBool());
    QToolButton* web = button("explorerWeb", "Web", "Get Media from the Web: MAGIX's online service (not in Reno)");
    web->setEnabled(false);
    QToolButton* views = button("explorerViews", "Views", "Views: Details, List, Thumbnail; Regions, Summary, Tree, All Files");
    views->setPopupMode(QToolButton::InstantPopup);
    QToolButton* add = button("explorerAdd", "Add to Project Media", "Add to Project Media List: the selected files, without putting them on the timeline");
    for (QToolButton* b : {d->back, d->forward, up, refresh, del, fav}) h->addWidget(b);
    h->addSpacing(4);
    for (QToolButton* b : {play, stop, d->autoPreview}) h->addWidget(b);
    h->addSpacing(4);
    h->addWidget(web);
    h->addWidget(views);
    h->addWidget(d->path, 1);
    h->addWidget(add);
    v->addWidget(bar);

    QSplitter* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName("explorerSplit");
    split->setChildrenCollapsible(false);
    d->folderPane = new QWidget(split);
    {
        QVBoxLayout* fv = new QVBoxLayout(d->folderPane);
        fv->setContentsMargins(0, 0, 0, 0);
        fv->setSpacing(1);
        QLabel* fl = new QLabel(QString::fromUtf8("★ Favorites"), d->folderPane);
        fl->setObjectName("explorerFavoritesLabel");
        fv->addWidget(fl);
        d->favorites = new QListWidget(d->folderPane);
        d->favorites->setObjectName("explorerFavorites");
        d->favorites->setMaximumHeight(90);
        d->favorites->setContextMenuPolicy(Qt::CustomContextMenu);
        fv->addWidget(d->favorites);
        d->dirs = new QFileSystemModel(this);
        d->dirs->setFilter(QDir::AllDirs | QDir::NoDotAndDotDot | QDir::Drives);
        d->dirs->setRootPath(QString());
        d->tree = new QTreeView(d->folderPane);
        d->tree->setObjectName("explorerFolders");
        d->tree->setModel(d->dirs);
        for (int c = 1; c < d->dirs->columnCount(); ++c) d->tree->hideColumn(c);
        d->tree->setHeaderHidden(true);
        d->tree->setContextMenuPolicy(Qt::CustomContextMenu);
        fv->addWidget(d->tree, 1);
    }
    for (const QString& f : readFavorites()) d->favorites->addItem(QDir::toNativeSeparators(f));
    d->files = new QFileSystemModel(this);
    d->files->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    d->files->setNameFilters(mediaPatterns());
    d->files->setNameFilterDisables(false);  // other files are left out, not greyed
    QWidget* right = new QWidget(split);
    QVBoxLayout* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(1);
    d->list = new QTreeView(right);
    d->list->setObjectName("explorerFiles");
    d->list->setModel(d->files);
    d->list->setRootIsDecorated(false);
    d->list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->list->setDragEnabled(true);  // the model hands out the files' URLs, which the timeline takes
    d->list->setDragDropMode(QAbstractItemView::DragOnly);
    d->list->setSortingEnabled(true);
    d->list->sortByColumn(0, Qt::AscendingOrder);
    d->list->header()->resizeSection(0, 260);
    d->list->setContextMenuPolicy(Qt::CustomContextMenu);
    d->thumbs = new QListView(right);
    d->thumbs->setObjectName("explorerThumbnails");
    d->thumbs->setModel(d->files);
    d->thumbs->setViewMode(QListView::IconMode);
    d->thumbs->setResizeMode(QListView::Adjust);
    d->thumbs->setIconSize(QSize(48, 48));
    d->thumbs->setGridSize(QSize(110, 80));
    d->thumbs->setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->thumbs->setDragEnabled(true);
    d->thumbs->setDragDropMode(QAbstractItemView::DragOnly);
    d->thumbs->setContextMenuPolicy(Qt::CustomContextMenu);
    d->thumbs->hide();
    rv->addWidget(d->list, 1);
    rv->addWidget(d->thumbs, 1);
    d->regions = new RegionList(right);
    d->regions->setMaximumHeight(110);
    d->regions->hide();
    rv->addWidget(d->regions);
    d->summary = new QLabel(right);
    d->summary->setObjectName("explorerSummary");
    d->summary->setTextFormat(Qt::PlainText);
    d->summary->setContentsMargins(4, 1, 4, 1);
    rv->addWidget(d->summary);
    split->addWidget(d->folderPane);
    split->addWidget(right);
    split->setStretchFactor(1, 1);
    split->setSizes({220, 480});
    v->addWidget(split, 1);
    d->thumbs->setRootIndex(d->files->index(QDir::homePath()));

    {
        QSettings st;
        d->view = st.value("explorer/view", "details").toString();
        d->showRegions = st.value("explorer/regions", false).toBool();
        d->showSummary = st.value("explorer/summary", true).toBool();
        d->showTree = st.value("explorer/tree", true).toBool();
        d->allFiles = st.value("explorer/allFiles", false).toBool();
    }
    QMenu* vm = new QMenu(views);
    vm->setObjectName("explorerViewsMenu");
    QActionGroup* vg = new QActionGroup(vm);
    auto viewItem = [&](const QString& text, const QString& key) {
        QAction* a = vm->addAction(text);
        a->setObjectName("explorerView_" + key);
        a->setCheckable(true);
        a->setActionGroup(vg);
        QObject::connect(a, &QAction::triggered, this, [this, key] { setView(key); });
        return a;
    };
    QAction* vDetails = viewItem("&Details", "details");
    QAction* vList = viewItem("&List", "list");
    QAction* vThumb = viewItem("&Thumbnail", "thumbnail");
    vm->addSeparator();
    auto toggle = [&](const QString& text, const QString& key, bool Impl::*flag) {
        QAction* a = vm->addAction(text);
        a->setObjectName("explorerView_" + key);
        a->setCheckable(true);
        QObject::connect(a, &QAction::toggled, this, [this, impl, key, flag](bool on) {
            impl->*flag = on;
            QSettings().setValue("explorer/" + key, on);
            applyViews();
        });
        return a;
    };
    QAction* vRegions = toggle("&Regions", "regions", &Impl::showRegions);
    QAction* vSummary = toggle("&Summary", "summary", &Impl::showSummary);
    QAction* vTree = toggle("T&ree", "tree", &Impl::showTree);
    QAction* vAll = toggle("&All Files", "allFiles", &Impl::allFiles);
    views->setMenu(vm);
    QObject::connect(vm, &QMenu::aboutToShow, this, [=] {
        vDetails->setChecked(impl->view == "details");
        vList->setChecked(impl->view == "list");
        vThumb->setChecked(impl->view == "thumbnail");
        QSignalBlocker b1(vRegions), b2(vSummary), b3(vTree), b4(vAll);
        vRegions->setChecked(impl->showRegions);
        vSummary->setChecked(impl->showSummary);
        vTree->setChecked(impl->showTree);
        vAll->setChecked(impl->allFiles);
    });

    QObject::connect(d->tree, &QTreeView::clicked, this, [this](const QModelIndex& i) { setFolder(d->dirs->filePath(i)); });
    QObject::connect(d->favorites, &QListWidget::itemClicked, this, [this](QListWidgetItem* it) { setFolder(QDir::fromNativeSeparators(it->text())); });
    auto activate = [this](const QModelIndex& i) {
        if (d->files->isDir(i)) { setFolder(d->files->filePath(i)); return; }
        const QString f = d->files->filePath(i);
        if ((f.endsWith(".veg", Qt::CaseInsensitive) || f.endsWith(".reno", Qt::CaseInsensitive)) && openProjectFile) { openProjectFile(f); return; }
        if (openInTrimmer && vprefs::on("prefs/dblClickTrimmer")) { openInTrimmer(0, f); return; }
        addFiles(QStringList() << f, true);
    };
    QObject::connect(d->list, &QTreeView::doubleClicked, this, activate);
    QObject::connect(d->thumbs, &QListView::doubleClicked, this, activate);
    auto selected = [this, impl] {
        const QString f = impl->selectedPath();
        showDetails(f);
        if (impl->autoPreview->isChecked() && !f.isEmpty() && QFileInfo(f).isFile() && previewMedia) {
            impl->previewing = f;
            previewMedia(0, f, true);
        }
    };
    QObject::connect(d->list->selectionModel(), &QItemSelectionModel::currentChanged, this, [selected](const QModelIndex&, const QModelIndex&) { selected(); });
    QObject::connect(d->thumbs->selectionModel(), &QItemSelectionModel::currentChanged, this, [selected](const QModelIndex&, const QModelIndex&) { selected(); });
    QObject::connect(d->path, &QLineEdit::returnPressed, this, [this] { setFolder(QDir::fromNativeSeparators(d->path->text())); });
    QObject::connect(d->back, &QToolButton::clicked, this, [this] { goHistory(-1); });
    QObject::connect(d->forward, &QToolButton::clicked, this, [this] { goHistory(1); });
    QObject::connect(up, &QToolButton::clicked, this, [this] {
        QDir dir(d->folder);
        if (dir.cdUp()) setFolder(dir.absolutePath());
    });
    QObject::connect(refresh, &QToolButton::clicked, this, [this] {
        const QString f = d->folder;
        d->folder.clear();
        d->navigating = true;
        setFolder(f);
        d->navigating = false;
    });
    QObject::connect(add, &QToolButton::clicked, this, [this] { addFiles(d->selectedFiles(), false); });
    QObject::connect(del, &QToolButton::clicked, this, [this] { deleteSelected(); });
    QObject::connect(fav, &QToolButton::clicked, this, [this] { addFavorite(d->folder); });
    QObject::connect(play, &QToolButton::clicked, this, [this] {
        const QString f = d->selectedPath();
        if (f.isEmpty() || !QFileInfo(f).isFile() || !previewMedia) return;
        d->previewing = f;
        previewMedia(0, f, true);
    });
    QObject::connect(stop, &QToolButton::clicked, this, [this] { if (previewMedia) previewMedia(0, d->previewing, false); });
    QObject::connect(d->autoPreview, &QToolButton::toggled, this, [](bool on) { QSettings().setValue("explorer/autoPreview", on); });
    auto fileMenu = [this](QAbstractItemView* view, const QPoint& pos) {
        const QModelIndex i = view->indexAt(pos);
        QMenu menu(this);
        menu.setObjectName("explorerMenu");
        if (i.isValid()) {
            const QString f = d->files->filePath(i.siblingAtColumn(0));
            if (d->files->isDir(i)) {
                menu.addAction("Open", [this, f] { setFolder(f); });
                menu.addAction("Add Folder to Favorites", [this, f] { addFavorite(f); });
            } else {
                menu.addAction("Start Preview", [this, f] { if (previewMedia) { d->previewing = f; previewMedia(0, f, true); } });
                menu.addAction("Add to Project Media List", [this] { addFiles(d->selectedFiles(), false); });
                menu.addAction("Add to Timeline", [this] { addFiles(d->selectedFiles(), true); });
                if (openInTrimmer) menu.addAction("Open in Trimmer", [f] { openInTrimmer(0, f); });
                if ((f.endsWith(".veg", Qt::CaseInsensitive) || f.endsWith(".reno", Qt::CaseInsensitive)) && openProjectFile)
                    menu.addAction("Open Project", [f] { openProjectFile(f); });
            }
            menu.addSeparator();
            menu.addAction("Delete", [this] { deleteSelected(); });
            menu.addSeparator();
        }
        menu.addAction("Refresh", [this] { const QString f = d->folder; d->folder.clear(); setFolder(f); });
        menu.exec(view->viewport()->mapToGlobal(pos));
    };
    QObject::connect(d->list, &QWidget::customContextMenuRequested, this, [this, fileMenu](const QPoint& p) { fileMenu(d->list, p); });
    QObject::connect(d->thumbs, &QWidget::customContextMenuRequested, this, [this, fileMenu](const QPoint& p) { fileMenu(d->thumbs, p); });
    QObject::connect(d->tree, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex i = d->tree->indexAt(pos);
        if (!i.isValid()) return;
        const QString f = d->dirs->filePath(i);
        QMenu menu(this);
        menu.addAction("Add Folder to Favorites", [this, f] { addFavorite(f); });
        menu.exec(d->tree->viewport()->mapToGlobal(pos));
    });
    QObject::connect(d->favorites, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* it = d->favorites->itemAt(pos);
        if (!it) return;
        QMenu menu(this);
        menu.addAction("Delete", [this, it] {
            delete it;
            QStringList l;
            for (int k = 0; k < d->favorites->count(); ++k) l << QDir::fromNativeSeparators(d->favorites->item(k)->text());
            writeFavorites(l);
        });
        menu.exec(d->favorites->viewport()->mapToGlobal(pos));
    });
    applyViews();
    setFolder(QSettings().value("explorer/folder", QDir::homePath()).toString());
}

MediaExplorer::~MediaExplorer() {}

void MediaExplorer::setFolder(const QString& path)
{
    const QFileInfo fi(path);
    if (!fi.isDir()) {
        d->path->setText(QDir::toNativeSeparators(d->folder));
        return;
    }
    const QString f = fi.absoluteFilePath();
    if (f == d->folder) return;
    d->folder = f;
    if (!d->navigating) {
        while (d->historyAt + 1 < d->history.size()) d->history.removeLast();
        d->history << f;
        d->historyAt = d->history.size() - 1;
    }
    d->back->setEnabled(d->historyAt > 0);
    d->forward->setEnabled(d->historyAt + 1 < d->history.size());
    d->path->setText(QDir::toNativeSeparators(f));
    const QModelIndex root = d->files->setRootPath(f);
    d->list->setRootIndex(root);
    d->thumbs->setRootIndex(root);
    const QModelIndex ti = d->dirs->index(f);
    d->tree->setCurrentIndex(ti);
    d->tree->scrollTo(ti);
    d->tree->expand(ti);
    QSettings().setValue("explorer/folder", f);
}

QString MediaExplorer::folder() const { return d->folder; }

QStringList MediaExplorer::visibleFiles() const
{
    QStringList out;
    const QModelIndex root = d->list->rootIndex();
    for (int r = 0; r < d->files->rowCount(root); ++r) out << d->files->fileName(d->files->index(r, 0, root));
    return out;
}

void MediaExplorer::addFiles(const QStringList& paths, bool toTimeline)
{
    AppState& app = AppState::get();
    QStringList failed;
    for (const QString& path : paths) {
        std::string err;
        const uint64_t id = app.importMedia(path.toStdString(), &err);
        if (!id) { failed << QFileInfo(path).fileName() + (err.empty() ? QString() : ": " + qs(err)); continue; }
        if (toTimeline) app.addMediaToTimeline(id, app.cursor(), app.currentTrack);
    }
    if (!failed.isEmpty()) app.status(QString("Could not be read: %1").arg(failed.join("; ")).toStdString());
}

void MediaExplorer::goHistory(int dir)
{
    const int to = d->historyAt + dir;
    if (to < 0 || to >= d->history.size()) return;
    d->historyAt = to;
    d->navigating = true;
    setFolder(d->history[to]);
    d->navigating = false;
    d->back->setEnabled(d->historyAt > 0);
    d->forward->setEnabled(d->historyAt + 1 < d->history.size());
}

void MediaExplorer::setView(const QString& view)
{
    d->view = view == "list" || view == "thumbnail" ? view : QString("details");
    QSettings().setValue("explorer/view", d->view);
    applyViews();
}

void MediaExplorer::applyViews()
{
    const bool thumb = d->view == "thumbnail";
    d->thumbs->setVisible(thumb);
    d->list->setVisible(!thumb);
    for (int c = 1; c < d->files->columnCount(); ++c) d->list->setColumnHidden(c, d->view == "list");
    d->list->setHeaderHidden(d->view == "list");
    d->regions->setVisible(d->showRegions);
    d->summary->setVisible(d->showSummary);
    d->folderPane->setVisible(d->showTree);
    d->files->setNameFilters(d->allFiles ? QStringList() : mediaPatterns());
    showDetails(d->selectedPath());
}

void MediaExplorer::addFavorite(const QString& folder)
{
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) return;
    QStringList l = readFavorites();
    const QString f = QFileInfo(folder).absoluteFilePath();
    if (l.contains(f)) return;
    l << f;
    writeFavorites(l);
    d->favorites->addItem(QDir::toNativeSeparators(f));
}

QStringList MediaExplorer::favorites() const { return readFavorites(); }

void MediaExplorer::deleteSelected()
{
    QStringList targets = d->selectedFiles();
    if (targets.isEmpty()) {
        const QString f = d->selectedPath();
        if (!f.isEmpty()) targets << f;
    }
    if (targets.isEmpty()) return;
    // asked first; files the project uses are named (Preferences > General "Confirm media deletion when still in use")
    QStringList used;
    for (const QString& t : targets)
        for (const MediaItem& m : AppState::get().project().media)
            if (qs(m.path) == t) used << QFileInfo(t).fileName();
    QString question = targets.size() == 1 ? QString("Move %1 to the Recycle Bin?").arg(QDir::toNativeSeparators(targets.first()))
                                           : QString("Move these %1 files to the Recycle Bin?").arg(targets.size());
    if (!used.isEmpty() && vprefs::on("prefs/confirmMediaDelete")) question += "\n\nThe project uses " + used.join(", ") + ".";
    if (QMessageBox::question(this, "Delete", question) != QMessageBox::Yes) return;
    QStringList failed;
    for (const QString& t : targets)
        if (!QFile::moveToTrash(t)) failed << QFileInfo(t).fileName();
    if (!failed.isEmpty()) QMessageBox::warning(this, "Delete", "Could not be deleted: " + failed.join(", "));
}

void MediaExplorer::showDetails(const QString& file)
{
    const QFileInfo fi(file);
    d->regions->clear();
    d->regions->file.clear();
    if (file.isEmpty() || !fi.isFile()) {
        d->summary->setText(QString("%1").arg(QDir::toNativeSeparators(d->folder)));
        return;
    }
    if (!d->showSummary && !d->showRegions) return;
    AppState& app = AppState::get();
    std::string err;
    std::unique_ptr<MediaReader> r = (hasMediaExtension(file.toStdString()) && app.mediaOpener) ? app.mediaOpener(file.toStdString(), &err) : nullptr;
    QString text = QString("%1   %2 KB").arg(fi.fileName()).arg((fi.size() + 1023) / 1024);
    if (r) {
        const MediaInfo& in = r->info();
        if (in.hasVideo) text += QString("   %1x%2%3").arg(in.width).arg(in.height).arg(in.fps > 0 ? QString(", %1 fps").arg(in.fps, 0, 'f', 3) : QString());
        if (in.hasAudio) text += QString("   %1 Hz, %2 ch").arg(in.sampleRate).arg(in.channels);
        if (in.duration > 0) text += QString("   %1").arg(qs(timecode(in.duration, std::max(1.0, app.project().settings.fps()))));
        if (!in.videoCodec.empty() || !in.audioCodec.empty()) text += QString("   %1").arg(qs(in.videoCodec + (in.videoCodec.empty() || in.audioCodec.empty() ? "" : " / ") + in.audioCodec));
        // the file's regions: its chapters (each to the next) and those the Trimmer saved with it in this project
        if (d->showRegions) {
            d->regions->file = file;
            int n = 0;
            const double fps = std::max(1.0, app.project().settings.fps());
            for (size_t k = 0; k < in.markers.size(); ++k) {
                const double a = in.markers[k].first, b = k + 1 < in.markers.size() ? in.markers[k + 1].first : in.duration;
                if (!(b > a)) continue;
                QTreeWidgetItem* it = new QTreeWidgetItem(d->regions, {QString::number(++n), qs(timecode(a, fps)), qs(timecode(b - a, fps)), qs(in.markers[k].second)});
                it->setData(1, Qt::UserRole, a);
                it->setData(2, Qt::UserRole, b);
            }
            for (const MediaItem& m : app.project().media)
                if (qs(m.path) == file)
                    for (const MediaItem::Marker& mk : m.markers)
                        if (mk.length > 0) {
                            QTreeWidgetItem* it = new QTreeWidgetItem(d->regions, {QString::number(++n), qs(timecode(mk.time, fps)), qs(timecode(mk.length, fps)), qs(mk.name)});
                            it->setData(1, Qt::UserRole, mk.time);
                            it->setData(2, Qt::UserRole, mk.time + mk.length);
                        }
        }
    }
    d->summary->setText(text);
}

// VEGAS's Edit Details: events, markers, regions or media as a table. A click selects a row and moves the cursor there.
struct EditDetails::Impl {
    QComboBox* show = nullptr;
    QTreeWidget* table = nullptr;
    std::unique_ptr<ListenerHandle> listener;
    LazyRefresh* lazy = nullptr;  // refilled once per event loop pass, and not while hidden
    bool filling = false;
    struct RowSig { uint64_t id = 0; int track = 0; bool video = false; double start = 0, length = 0, fadeIn = 0, fadeOut = 0; bool mute = false; uint64_t mediaId = 0; size_t nameHash = 0; };
    std::vector<RowSig> rowSigs;
    QString rowsKey;

    void fill()
    {
        if (lazy) lazy->cancel();
        filling = true;
        const AppState& app = AppState::get();
        const Project& p = app.project();
        const int what = show->currentData().toInt();
        auto t = [&p](double seconds) { return qs(rulerTime(p, seconds)); };
        auto len = [&p](double seconds) { return qs(formatTime(p.settings, seconds, p.settings.rulerFormat)); };
        // Events: if the rows are the same events in the same order, only changed rows are rewritten (a full rebuild
        // of tens of thousands of rows took half a second).
        if (what == Events) {
            std::vector<RowSig> now;
            size_t total = 0;
            for (const Track& tr : p.tracks) total += tr.events.size();
            now.reserve(total);
            int n = 0;
            for (const Track& tr : p.tracks) {
                ++n;
                for (const Event& e : static_cast<const Track&>(tr).events) {
                    RowSig r;
                    r.id = e.id; r.track = n; r.video = tr.type == TrackType::Video; r.start = e.start; r.length = e.length;
                    r.fadeIn = e.fadeIn; r.fadeOut = e.fadeOut; r.mute = e.mute; r.mediaId = e.mediaId; r.nameHash = std::hash<std::string>()(e.name);
                    now.push_back(r);
                }
            }
            uint64_t mediaHash = 1469598103934665603ull;
            for (const MediaItem& m : p.media) mediaHash = (mediaHash ^ std::hash<std::string>()(m.path + "|" + m.name)) * 1099511628211ull + m.id;
            const QString key = QString("%1|%2|%3|%4|%5|%6").arg(qs(p.settings.rulerFormat)).arg(p.settings.fpsNum).arg(p.settings.fpsDen).arg(p.settings.rulerStart).arg(mediaHash).arg(p.settings.sampleRate);
            bool same = key == rowsKey && rowSigs.size() == now.size() && (size_t)table->topLevelItemCount() == now.size();
            for (size_t i = 0; same && i < now.size(); ++i) same = rowSigs[i].id == now[i].id && rowSigs[i].track == now[i].track && rowSigs[i].video == now[i].video;
            if (same) {
                size_t i = 0;
                int row = 0;
                for (const Track& tr : p.tracks)
                    for (const Event& e : static_cast<const Track&>(tr).events) {
                        const RowSig& a = rowSigs[i];
                        const RowSig& b = now[i];
                        ++i;
                        const int r = row++;
                        if (a.start == b.start && a.length == b.length && a.fadeIn == b.fadeIn && a.fadeOut == b.fadeOut && a.mute == b.mute &&
                            a.mediaId == b.mediaId && a.nameHash == b.nameHash)
                            continue;
                        QTreeWidgetItem* it = table->topLevelItem(r);
                        if (!it) continue;
                        const MediaItem* m = p.findMedia(e.mediaId);
                        it->setText(1, t(e.start));
                        it->setText(2, t(e.end()));
                        it->setText(3, len(e.length));
                        it->setText(4, qs(e.name));
                        it->setText(5, m ? qs(m->path.empty() ? m->name : m->path) : QString());
                        it->setText(6, len(e.fadeIn));
                        it->setText(7, len(e.fadeOut));
                        it->setText(8, e.mute ? "Yes" : "No");
                        it->setData(1, Qt::UserRole, e.start);
                    }
                rowSigs = std::move(now);
                filling = false;
                return;
            }
            rowSigs = std::move(now);
            rowsKey = key;
        } else {
            rowSigs.clear();
            rowsKey.clear();
        }
        const QString keepId = table->currentItem() ? table->currentItem()->data(0, Qt::UserRole).toString() : QString();
        table->clear();
        auto editable = [](QTreeWidgetItem* it) { it->setFlags(it->flags() | Qt::ItemIsEditable); };
        QStringList heads;
        if (what == Events || what == SelectedEvents) {
            heads << "Track" << "Start" << "End" << "Length" << "Name" << "Media" << "Fade In" << "Fade Out" << "Muted";
            int n = 0;
            for (const Track& tr : p.tracks) {
                ++n;
                for (const Event& e : tr.events) {
                    if (what == SelectedEvents && !app.selection().count(e.id)) continue;
                    const MediaItem* m = p.findMedia(e.mediaId);
                    auto* it = new QTreeWidgetItem(table, QStringList() << QString("%1 %2").arg(n).arg(tr.type == TrackType::Video ? "Video" : "Audio") << t(e.start)
                                                                         << t(e.end()) << len(e.length) << qs(e.name) << (m ? qs(m->path.empty() ? m->name : m->path) : QString())
                                                                         << len(e.fadeIn) << len(e.fadeOut) << (e.mute ? "Yes" : "No"));
                    editable(it);
                    it->setData(0, Qt::UserRole, QString("e%1").arg(e.id));
                    it->setData(1, Qt::UserRole, e.start);
                }
            }
        } else if (what == Commands) {
            heads << "Position" << "Command" << "Parameter";
            std::vector<const Project::Marker*> list;
            for (const Project::Marker& m : p.markers)
                if (m.kind == "command") list.push_back(&m);
            std::stable_sort(list.begin(), list.end(), [](const Project::Marker* a, const Project::Marker* b) { return a->time < b->time; });
            for (const Project::Marker* m : list) {
                auto* it = new QTreeWidgetItem(table, QStringList() << t(m->time) << qs(m->command) << qs(m->name));
                editable(it);
                it->setData(0, Qt::UserRole, QString("c%1").arg(m->id));
                it->setData(1, Qt::UserRole, m->time);
            }
        } else if (what == AudioCdTrackList) {
            // VEGAS's Audio CD Track List: each track (index 1) and its indexes, numbered from the first track number
            // in Project Properties.
            heads << "Track" << "Index" << "Position" << "End" << "Length" << "Name" << "Prot" << "Emph" << "ISRC";
            std::vector<const Project::Marker*> tracks, indexes;
            for (const Project::Marker& m : p.markers) {
                if (m.kind == "cdtrack") tracks.push_back(&m);
                else if (m.kind == "cdindex") indexes.push_back(&m);
            }
            auto byTime = [](const Project::Marker* a, const Project::Marker* b) { return a->time < b->time; };
            std::stable_sort(tracks.begin(), tracks.end(), byTime);
            std::stable_sort(indexes.begin(), indexes.end(), byTime);
            int n = 0;
            for (const Project::Marker* tr : tracks) {
                ++n;
                auto* it = new QTreeWidgetItem(table, QStringList() << QString::number(n) << "1" << t(tr->time) << t(tr->time + tr->length) << len(tr->length)
                                                                     << qs(tr->name) << "No" << "No" << QString());
                editable(it);
                it->setData(0, Qt::UserRole, QString("m%1").arg(tr->id));
                it->setData(1, Qt::UserRole, tr->time);
                int k = 1;
                for (const Project::Marker* ix : indexes) {
                    if (ix->time <= tr->time + 1e-9 || ix->time >= tr->time + tr->length - 1e-9) continue;
                    auto* sub = new QTreeWidgetItem(table, QStringList() << QString::number(n) << QString::number(++k) << t(ix->time) << QString() << QString()
                                                                          << qs(ix->name) << QString() << QString() << QString());
                    editable(sub);
                    sub->setData(0, Qt::UserRole, QString("i%1").arg(ix->id));
                    sub->setData(1, Qt::UserRole, ix->time);
                }
            }
        } else if (what == Markers || what == Regions) {
            const bool regions = what == Regions;
            heads << "Number" << "Position";
            if (regions) heads << "End" << "Length";
            heads << "Name";
            int n = 0;
            for (const Project::Marker& m : p.markers) {
                if (m.region != regions) continue;
                if (!m.kind.empty()) continue;
                ++n;
                QStringList cols;
                cols << QString::number(n) << t(m.time);
                if (regions) cols << t(m.time + m.length) << len(m.length);
                cols << qs(m.name);
                auto* it = new QTreeWidgetItem(table, cols);
                editable(it);
                it->setData(0, Qt::UserRole, QString("m%1").arg(m.id));
                it->setData(1, Qt::UserRole, m.time);
            }
        } else {
            heads << "Name" << "File" << "Length" << "Used";
            for (const MediaItem& m : p.media) {
                int used = 0;
                for (const Track& tr : p.tracks)
                    for (const Event& e : tr.events) used += e.mediaId == m.id ? 1 : 0;
                auto* it = new QTreeWidgetItem(table, QStringList() << qs(m.name) << qs(m.path) << t(m.info.duration) << QString::number(used));
                it->setData(0, Qt::UserRole, QString("x%1").arg(m.id));
            }
        }
        table->setHeaderLabels(heads);
        for (int r = 0; r < table->topLevelItemCount(); ++r)
            if (table->topLevelItem(r)->data(0, Qt::UserRole).toString() == keepId) table->setCurrentItem(table->topLevelItem(r));
        filling = false;
    }

    // A cell was edited in place: 'id' is its row ("e<event>" or "m<marker>"). Read-only columns (track, media, numbers)
    // and unparsable text change nothing; the table is refilled as it was.
    void changed(const QString& id, int column, const QString& text)
    {
        AppState& app = AppState::get();
        const Project& p = app.project();
        const ProjectSettings& st = p.settings;
        const int what = show->currentData().toInt();
        bool ok = false;
        auto position = [&](double* v) { return parseRulerTime(p, text, v) && *v >= 0; };
        auto length = [&](double* v) { return parseTimeText(st, text.toStdString(), st.rulerFormat, v) && *v >= 0; };
        double v = 0;
        if (id.startsWith('e')) {
            const uint64_t eid = id.mid(1).toULongLong();
            const Event* e = p.findEvent(eid);
            if (e) {
                const double minLen = 1.0 / std::max(1.0, st.fps());
                switch (column) {
                case 1:
                    if ((ok = position(&v))) app.edit("Event Start", [&](Project& x) { if (Event* o = x.findEvent(eid)) o->start = v; });
                    break;
                case 2:
                    if ((ok = position(&v) && v > e->start + minLen * 0.5))
                        app.edit("Event Length", [&](Project& x) { if (Event* o = x.findEvent(eid)) { o->length = v - o->start; o->fadeIn = std::min(o->fadeIn, o->length); o->fadeOut = std::min(o->fadeOut, o->length); } });
                    break;
                case 3:
                    if ((ok = length(&v) && v >= minLen * 0.5))
                        app.edit("Event Length", [&](Project& x) { if (Event* o = x.findEvent(eid)) { o->length = v; o->fadeIn = std::min(o->fadeIn, v); o->fadeOut = std::min(o->fadeOut, v); } });
                    break;
                case 4: {
                    const std::string name = text.trimmed().toStdString();
                    ok = true;
                    if (name != e->name) app.edit("Rename Event", [&](Project& x) { if (Event* o = x.findEvent(eid)) o->name = name; });
                    break;
                }
                case 6:
                case 7:
                    if ((ok = length(&v))) {
                        v = std::min(v, e->length);
                        const bool in = column == 6;
                        app.edit(in ? "Fade In" : "Fade Out", [&](Project& x) { if (Event* o = x.findEvent(eid)) (in ? o->fadeIn : o->fadeOut) = v; });
                    }
                    break;
                case 8: {
                    const QString t = text.trimmed().toLower();
                    const bool yes = t == "yes" || t == "y" || t == "1" || t == "true" || t == "on";
                    const bool no = t == "no" || t == "n" || t == "0" || t == "false" || t == "off";
                    if ((ok = yes || no) && yes != e->mute) app.edit(yes ? "Mute Event" : "Unmute Event", [&](Project& x) { if (Event* o = x.findEvent(eid)) o->mute = yes; });
                    break;
                }
                default: break;
                }
            }
        } else if (id.startsWith('m') && what != AudioCdTrackList) {
            const uint64_t mid = id.mid(1).toULongLong();
            const Project::Marker* m = nullptr;
            for (const auto& x : p.markers) if (x.id == mid) m = &x;
            const bool regions = what == Regions;
            const int nameCol = regions ? 4 : 2;
            if (m) {
                auto change = [&](const char* label, std::function<void(Project::Marker&)> fn) {
                    app.edit(label, [&](Project& x) { for (auto& o : x.markers) if (o.id == mid) fn(o); });
                };
                if (column == 1 && (ok = position(&v))) change(regions ? "Move Region" : "Move Marker", [v](Project::Marker& o) { o.time = v; });
                else if (regions && column == 2 && (ok = position(&v) && v > m->time)) change("Region Length", [v](Project::Marker& o) { o.length = v - o.time; });
                else if (regions && column == 3 && (ok = length(&v) && v > 0)) change("Region Length", [v](Project::Marker& o) { o.length = v; });
                else if (column == nameCol) {
                    ok = true;
                    const std::string name = text.trimmed().toStdString();
                    if (name != m->name) change("Rename Marker", [name](Project::Marker& o) { o.name = name; });
                }
            }
        }
        else if (id.startsWith('c') || id.startsWith('i') || (id.startsWith('m') && what == AudioCdTrackList)) {
            // a command: its position, command or parameter (edits apply to all selected rows at once, as in VEGAS);
            // a CD track or index: its position, end or name
            const uint64_t mid = id.mid(1).toULongLong();
            auto change = [&](const char* label, const std::vector<uint64_t>& ids, std::function<void(Project::Marker&)> fn) {
                app.edit(label, [&](Project& x) {
                    for (auto& o : x.markers)
                        if (std::find(ids.begin(), ids.end(), o.id) != ids.end()) fn(o);
                });
            };
            std::vector<uint64_t> ids{mid};
            for (const QString& s : editedRows)
                if (s.startsWith('c') && s.mid(1).toULongLong() != mid) ids.push_back(s.mid(1).toULongLong());
            if (what == Commands) {
                if (column == 0 && (ok = position(&v))) change("Move Command", {mid}, [v](Project::Marker& o) { o.time = v; });
                else if (column == 1 && (ok = !text.trimmed().isEmpty())) {
                    const std::string c = text.trimmed().toStdString();
                    change("Command", ids, [c](Project::Marker& o) { o.command = c; });
                } else if (column == 2) {
                    ok = true;
                    const std::string param = text.toStdString();
                    change("Command Parameter", {mid}, [param](Project::Marker& o) {
                        o.name = isCaptionCommand(o.command) ? captionMarkupOf(param) : param;
                    });
                }
            } else {
                const bool index = id.startsWith('i');
                if (column == 2 && (ok = position(&v))) change(index ? "Move CD Index" : "Move CD Track", {mid}, [v](Project::Marker& o) { o.time = v; });
                else if (!index && column == 3 && (ok = position(&v))) {
                    change("CD Track Length", {mid}, [v](Project::Marker& o) { if (v > o.time) o.length = v - o.time; });
                } else if (column == 5) {
                    ok = true;
                    const std::string name = text.trimmed().toStdString();
                    change("Rename CD Track", {mid}, [name](Project::Marker& o) { o.name = name; });
                }
            }
        }
        editedRows.clear();
        if (!ok) {
            if (!text.trimmed().isEmpty() && column != 0) app.status("Edit Details: \"" + text.toStdString() + "\" cannot go there.");
            fill();
        }
    }
    QStringList editedRows;  // the rows selected when a cell was changed: a command typed in goes to all of them
    bool placing = false;  // Ctrl+K selects the next row without moving the cursor
};

EditDetails::EditDetails(QWidget* parent) : QWidget(parent), d(new Impl())
{
    setObjectName("editDetails");
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    QWidget* bar = new QWidget(this);
    bar->setFixedHeight(29);
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(4, 2, 4, 2);
    h->addWidget(new QLabel("Show:", bar));
    d->show = new QComboBox(bar);
    d->show->setObjectName("detailsShow");
    d->show->addItem("Events", (int)Events);
    d->show->addItem("Selected Events", (int)SelectedEvents);
    d->show->addItem("Markers", (int)Markers);
    d->show->addItem("Regions", (int)Regions);
    d->show->addItem("Media", (int)Media);
    d->show->addItem("Commands", (int)Commands);
    d->show->addItem("Audio CD Track List", (int)AudioCdTrackList);
    h->addWidget(d->show);
    h->addStretch(1);
    v->addWidget(bar);
    d->table = new QTreeWidget(this);
    d->table->setObjectName("detailsTable");
    d->table->setRootIsDecorated(false);
    d->table->setAlternatingRowColors(true);
    d->table->setUniformRowHeights(true);
    d->table->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    v->addWidget(d->table, 1);
    Impl* impl = d.get();
    QObject::connect(d->table, &QTreeWidget::itemChanged, this, [this, impl](QTreeWidgetItem* it, int column) {
        if (impl->filling || !it) return;
        const QString id = it->data(0, Qt::UserRole).toString(), text = it->text(column);
        QStringList rows;
        for (QTreeWidgetItem* s : impl->table->selectedItems()) rows << s->data(0, Qt::UserRole).toString();
        QTimer::singleShot(0, this, [impl, id, column, text, rows] {
            impl->editedRows = rows;
            impl->changed(id, column, text);
        });
    });
    // several rows can be selected; Ctrl+C copies them, Ctrl+V pastes commands, Ctrl+K times a caption
    d->table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    {
        auto shortcut = [this](const char* name, const char* keys, std::function<void()> fn) {
            QAction* a = new QAction(this);
            a->setObjectName(name);
            a->setShortcut(QKeySequence(QString::fromUtf8(keys)));
            a->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            QObject::connect(a, &QAction::triggered, this, [fn] { fn(); });
            addAction(a);
        };
        shortcut("detailsCopy", "Ctrl+C", [this] { QGuiApplication::clipboard()->setText(selectedText()); });
        shortcut("detailsPaste", "Ctrl+V", [this, impl] {
            if (impl->show->currentData().toInt() == Commands) pasteCommands(QGuiApplication::clipboard()->text());
        });
        shortcut("detailsInsertCommand", "Ctrl+K", [this] { insertSelectedCommandAtCursor(); });
        d->table->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(d->table, &QWidget::customContextMenuRequested, this, [this, impl](const QPoint& at) {
            QMenu menu(this);
            menu.addAction("Copy", [this] { QGuiApplication::clipboard()->setText(selectedText()); })->setEnabled(!impl->table->selectedItems().isEmpty());
            if (impl->show->currentData().toInt() == Commands) {
                menu.addAction("Paste", [this] { pasteCommands(QGuiApplication::clipboard()->text()); });
                QTreeWidgetItem* it = impl->table->itemAt(at);
                const int col = impl->table->columnAt(at.x());
                if (it) menu.addAction("Edit", [impl, it, col] { impl->table->editItem(it, std::max(0, col)); });
            }
            menu.exec(impl->table->viewport()->mapToGlobal(at));
        });
    }
    QObject::connect(d->show, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [impl](int) { impl->fill(); });
    QObject::connect(d->table, &QTreeWidget::currentItemChanged, this, [impl](QTreeWidgetItem* it) {
        if (impl->filling || !it) return;
        AppState& app = AppState::get();
        const QString id = it->data(0, Qt::UserRole).toString();
        const QVariant at = it->data(1, Qt::UserRole);
        if (at.isValid() && !impl->placing) app.setCursor(at.toDouble());
        if (id.startsWith('e')) app.selectOnly(id.mid(1).toULongLong());
    });
    d->lazy = new LazyRefresh(this, [impl] { impl->fill(); });
    d->listener.reset(new ListenerHandle([impl](int changes) {
        if (changes & (kProject | kSelection | kFile)) impl->lazy->request();
    }));
    d->fill();
}

EditDetails::~EditDetails() {}

void EditDetails::setShow(Show what)
{
    d->show->setCurrentIndex(std::max(0, d->show->findData((int)what)));
    d->fill();
}

bool EditDetails::insertSelectedCommandAtCursor()
{
    d->lazy->flush();
    if (d->show->currentData().toInt() != Commands) return false;
    QTreeWidgetItem* it = d->table->currentItem();
    if (!it) return false;
    const QString id = it->data(0, Qt::UserRole).toString();
    if (!id.startsWith('c')) return false;
    AppState& app = AppState::get();
    double at = app.cursor();
    if (gPlayback && gPlayback->playing()) at = gPlayback->position();
    const int row = d->table->indexOfTopLevelItem(it);
    const uint64_t mid = id.mid(1).toULongLong();
    app.edit("Insert Command at Cursor", [&](Project& p) {
        for (auto& m : p.markers)
            if (m.id == mid) m.time = std::max(0.0, at);
    });
    d->fill();
    // the next command in time order (this row's old index now holds it or its successor)
    QTreeWidgetItem* next = nullptr;
    for (int r = 0; r < d->table->topLevelItemCount(); ++r)
        if (d->table->topLevelItem(r)->data(0, Qt::UserRole).toString() == id) next = d->table->topLevelItem(r + 1);
    if (!next && row + 1 < d->table->topLevelItemCount()) next = d->table->topLevelItem(row + 1);
    if (next) {
        d->placing = true;
        d->table->setCurrentItem(next);
        d->placing = false;
    }
    return true;
}

QString EditDetails::selectedText() const
{
    d->lazy->flush();
    QStringList lines;
    for (int r = 0; r < d->table->topLevelItemCount(); ++r) {
        QTreeWidgetItem* it = d->table->topLevelItem(r);
        if (!it->isSelected() && it != d->table->currentItem()) continue;
        QStringList cols;
        for (int c = 0; c < d->table->columnCount(); ++c) cols << it->text(c);
        lines << cols.join('\t');
    }
    return lines.join("\r\n");
}

int EditDetails::pasteCommands(const QString& text)
{
    AppState& app = AppState::get();
    struct Row { double t; std::string command, param; };
    std::vector<Row> rows;
    for (const QString& line : text.split(QRegularExpression("[\r\n]+"), Qt::SkipEmptyParts)) {
        const QStringList f = line.split('\t');
        if (f.size() < 3) continue;
        double t = 0;
        if (!parseRulerTime(app.project(), f[0].trimmed(), &t)) t = 0;
        Row r;
        r.t = std::max(0.0, t);
        r.command = f[1].trimmed().toStdString();
        r.param = f[2].toStdString();
        if (r.command.empty()) continue;
        if (isCaptionCommand(r.command)) r.param = captionMarkupOf(r.param);
        rows.push_back(r);
    }
    if (rows.empty()) return 0;
    app.edit("Paste Commands", [&](Project& p) {
        for (const Row& r : rows) {
            Project::Marker m;
            m.id = p.newId();
            m.time = r.t;
            m.kind = "command";
            m.command = r.command;
            m.name = r.param;
            p.markers.push_back(m);
        }
    });
    return (int)rows.size();
}

int EditDetails::rows() const
{
    d->lazy->flush();
    return d->table->topLevelItemCount();
}
QString EditDetails::cell(int row, int column) const
{
    d->lazy->flush();
    QTreeWidgetItem* it = d->table->topLevelItem(row);
    return it ? it->text(column) : QString();
}

void stopPlayback(Playback* pb)
{
    if (!pb->playing()) return;
    if (AppState::get().prefs.cursorFollowsPlayback) pb->pause();
    else pb->stop();
}

struct TransportBar::Impl {
    Playback* pb = nullptr;
    GlyphButton* record = nullptr;
    std::function<void()> onRecord;
    GlyphButton *loop = nullptr, *play = nullptr;
    std::unique_ptr<ListenerHandle> listener;
    void sync()
    {
        loop->setChecked(AppState::get().loopOn);
        play->setPlaying(pb->playing());
    }
};

TransportBar::TransportBar(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->pb = playback;
    setObjectName("transportBar");
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(4, 2, 4, 2);
    row->setSpacing(1);
    auto button = [&](Glyph g, const char* name, const QString& tip) {
        auto* b = new GlyphButton(g, tip, this, 26, 24);
        b->setObjectName(name);
        row->addWidget(b);
        return b;
    };
    GlyphButton* record = button(Glyph::Record, "recordButton", "Record (Ctrl+R)");
    record->setEnabled(false);
    record->setCheckable(true);
    d->record = record;
    d->loop = button(Glyph::Loop, "loopButton", "Loop Playback (Q)");
    d->loop->setCheckable(true);
    GlyphButton* fromStart = button(Glyph::PlayFromStart, "playFromStart", "Play From Start (Shift+Space)");
    d->play = button(Glyph::Play, "playButton", "Play (Space)");
    GlyphButton* pause = button(Glyph::Pause, "pauseButton", "Pause (Enter)");
    GlyphButton* stop = button(Glyph::Stop, "stopButton", "Stop (Space, Esc)");
    row->addSpacing(6);
    GlyphButton* toStart = button(Glyph::GoToStart, "goToStart", "Go To Start (Ctrl+Home)");
    GlyphButton* toEnd = button(Glyph::GoToEnd, "goToEnd", "Go To End (Ctrl+End)");
    GlyphButton* prev = button(Glyph::PrevFrame, "prevFrame", "Previous Frame (Alt+Left)");
    GlyphButton* next = button(Glyph::NextFrame, "nextFrame", "Next Frame (Alt+Right)");

    Impl* impl = d.get();
    auto moved = [this] { if (onCursorMoved) onCursorMoved(); };
    QObject::connect(d->loop, &QAbstractButton::clicked, [](bool on) {
        AppState::get().loopOn = on;
        AppState::get().notify(kCursor);
    });
    QObject::connect(fromStart, &QAbstractButton::clicked, this, [impl, moved] {
        AppState& app = AppState::get();
        impl->pb->pause();
        app.setCursor(app.loopEnd > app.loopStart ? app.loopStart : 0.0);
        moved();
        impl->pb->play();
    });
    QObject::connect(d->play, &QAbstractButton::clicked, [impl] { if (!impl->pb->playing()) impl->pb->play(); });
    QObject::connect(pause, &QAbstractButton::clicked, [impl] { impl->pb->pause(); });
    QObject::connect(stop, &QAbstractButton::clicked, [impl] { stopPlayback(impl->pb); });
    QObject::connect(toStart, &QAbstractButton::clicked, this, [impl, moved] {
        impl->pb->jumpTo(0);
        moved();
    });
    QObject::connect(toEnd, &QAbstractButton::clicked, this, [impl, moved] {
        impl->pb->pause();
        AppState::get().setCursor(AppState::get().project().length());
        moved();
    });
    auto step = [impl, moved](int dir) {
        impl->pb->pause();
        AppState& app = AppState::get();
        const Project& p = app.project();
        app.setCursor(p.frameTime(std::max<int64_t>(0, p.frameAt(app.cursor()) + dir)));
        moved();
    };
    QObject::connect(prev, &QAbstractButton::clicked, [step] { step(-1); });
    QObject::connect(next, &QAbstractButton::clicked, [step] { step(1); });
    d->listener.reset(new ListenerHandle([impl](int changes) {
        if (changes & (kCursor | kPlayback)) impl->sync();
    }));
    d->sync();
}

TransportBar::~TransportBar() {}

void TransportBar::setRecordHandler(std::function<void()> fn)
{
    d->onRecord = std::move(fn);
    d->record->setEnabled((bool)d->onRecord);
    Impl* impl = d.get();
    QObject::disconnect(d->record, &QAbstractButton::clicked, nullptr, nullptr);
    QObject::connect(d->record, &QAbstractButton::clicked, d->record, [impl] {
        impl->record->setChecked(!impl->record->isChecked());
        if (impl->onRecord) impl->onRecord();
    });
}

void TransportBar::setRecording(bool on) { d->record->setChecked(on); }

}  // namespace app
}  // namespace reno
