// Device Explorer; see deviceexplorer.h.
#include "deviceexplorer.h"
#include "toolwin.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <set>
#ifdef _WIN32
#include <windows.h>
#include <cwctype>
#endif

namespace reno {
namespace app {

using namespace tw;

namespace {

const QStringList& mediaSuffixes()
{
    // camera raw stills and every media kind Reno reads (media.h's list)
    static const QStringList s = [] {
        QStringList l = {"insv", "lrv", "bwf", "dng", "arw", "cr2", "nef", "heic"};
        for (const std::string& e : mediaExtensions(MediaKind::Any))
            if (!l.contains(QString::fromStdString(e))) l << QString::fromStdString(e);
        return l;
    }();
    return s;
}

bool isMedia(const QFileInfo& fi) { return fi.isFile() && mediaSuffixes().contains(fi.suffix().toLower()); }

// Files with media suffixes in 'dir' and its folders ('depth' deep), at most 'most', sorted.
void collect(const QString& dir, int depth, QStringList& out, int most = 5000)
{
    QDir d(dir);
    if (!d.exists() || out.size() >= most) return;
    const QFileInfoList list = d.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo& fi : list) {
        if (out.size() >= most) return;
        if (fi.isDir()) {
            if (depth > 0 && !fi.isSymLink() && !fi.fileName().startsWith('.') && fi.fileName() != "System Volume Information" &&
                fi.fileName() != "$RECYCLE.BIN")
                collect(fi.absoluteFilePath(), depth - 1, out, most);
        } else if (isMedia(fi)) {
            out << fi.absoluteFilePath();
        }
    }
}

// A folder of the device whatever its letter case (cards are FAT).
QString childCi(const QString& dir, const QString& name)
{
    QDir d(dir);
    for (const QString& e : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (e.compare(name, Qt::CaseInsensitive) == 0) return d.absoluteFilePath(e);
    return QString();
}
QString pathCi(const QString& root, const QStringList& parts)
{
    QString at = root;
    for (const QString& p : parts) {
        at = childCi(at, p);
        if (at.isEmpty()) return QString();
    }
    return at;
}

QString humanSize(qint64 b)
{
    if (b < 1024) return QString("%1 B").arg(b);
    if (b < 1024 * 1024) return QString("%1 KB").arg(b / 1024.0, 0, 'f', 1);
    if (b < 1024ll * 1024 * 1024) return QString("%1 MB").arg(b / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(b / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

}  // namespace

QStringList DeviceExplorer::findClips(const QString& root, QString* kind)
{
    QStringList out;
    // where cameras keep their clips
    const struct { QStringList path; const char* kind; int depth; } places[] = {
        {{"PRIVATE", "AVCHD", "BDMV", "STREAM"}, "Camera card (AVCHD)", 0},
        {{"AVCHD", "BDMV", "STREAM"}, "Camera card (AVCHD)", 0},
        {{"PRIVATE", "M4ROOT", "CLIP"}, "Camera card (XAVC S)", 0},
        {{"XDROOT", "Clip"}, "Camera card (XDCAM)", 0},
        {{"Clip"}, "Camera card (XDCAM)", 0},
        {{"CONTENTS", "VIDEO"}, "Camera card (P2)", 0},
        {{"CONTENTS", "AUDIO"}, "Camera card (P2)", 0},
        {{"MP_ROOT"}, "Camera card (MP4)", 2},
        {{"DCIM"}, "Camera card", 2},
    };
    QString k;
    for (const auto& pl : places) {
        const QString dir = pathCi(root, pl.path);
        if (dir.isEmpty()) continue;
        const int before = out.size();
        collect(dir, pl.depth, out);
        if (out.size() > before && k.isEmpty()) k = pl.kind;
    }
    if (out.isEmpty()) {
        // not laid out like a camera: any media on it (not too deep)
        collect(root, 4, out);
        k = "Removable drive";
    }
    out.removeDuplicates();
    if (kind) *kind = k;
    return out;
}

struct DeviceExplorer::Impl {
    DeviceExplorer* q = nullptr;
    QListWidget* list = nullptr;
    QTreeWidget* clips = nullptr;
    QLineEdit* folder = nullptr;
    QLabel* info = nullptr;
    QTimer* poll = nullptr;
    QStringList fakeRoots;
    bool useFake = false;
    std::vector<Device> devs;
    QString current;
    QStringList currentClips;

    std::vector<Device> scan() const
    {
        std::vector<Device> out;
        auto add = [&](const QString& root, const QString& name, qint64 total, qint64 free) {
            Device d;
            d.root = QDir(root).absolutePath();
            d.name = name.isEmpty() ? QFileInfo(d.root).fileName() : name;
            if (d.name.isEmpty()) d.name = QDir::toNativeSeparators(d.root);
            d.bytesTotal = total;
            d.bytesFree = free;
            out.push_back(d);
        };
        if (useFake) {
            for (const QString& r : fakeRoots) add(r, QString(), 0, 0);
        } else {
            for (const QStorageInfo& s : QStorageInfo::mountedVolumes()) {
                if (!s.isValid() || !s.isReady() || s.isRoot()) continue;
                const QString root = s.rootPath();
                bool removable = false;
#ifdef _WIN32
                const std::wstring w = QDir::toNativeSeparators(root).toStdWString();
                const UINT type = GetDriveTypeW(w.c_str());
                removable = type == DRIVE_REMOVABLE;
                // the Windows drive is never a card
                wchar_t sys[MAX_PATH] = {0};
                if (GetWindowsDirectoryW(sys, MAX_PATH) && !w.empty() && towupper(sys[0]) == towupper(w[0])) continue;
#else
                removable = root.startsWith("/media/") || root.startsWith("/run/media/") || root.startsWith("/mnt/");
#endif
                // a fixed drive laid out like a camera card counts too
                if (!removable && (childCi(root, "DCIM").isEmpty() && pathCi(root, {"PRIVATE", "AVCHD"}).isEmpty() && pathCi(root, {"PRIVATE", "M4ROOT"}).isEmpty()))
                    continue;
                add(root, s.displayName(), s.bytesTotal(), s.bytesAvailable());
            }
        }
        for (Device& d : out) {
            QString kind;
            // the kind is found again when the device is chosen
            if (!pathCi(d.root, {"PRIVATE", "AVCHD"}).isEmpty() || !pathCi(d.root, {"AVCHD"}).isEmpty()) kind = "Camera card (AVCHD)";
            else if (!pathCi(d.root, {"PRIVATE", "M4ROOT"}).isEmpty()) kind = "Camera card (XAVC S)";
            else if (!pathCi(d.root, {"XDROOT"}).isEmpty() || !childCi(d.root, "Clip").isEmpty()) kind = "Camera card (XDCAM)";
            else if (!pathCi(d.root, {"CONTENTS", "VIDEO"}).isEmpty()) kind = "Camera card (P2)";
            else if (!childCi(d.root, "DCIM").isEmpty()) kind = "Camera card";
            else kind = "Removable drive";
            d.kind = kind;
        }
        return out;
    }

    void fillDevices()
    {
        const std::vector<Device> now = scan();
        bool same = now.size() == devs.size();
        for (size_t k = 0; same && k < now.size(); ++k) same = now[k].root == devs[k].root;
        if (same && list->count() > 0) return;
        devs = now;
        list->clear();
        for (const Device& d : devs) {
            auto* it = new QListWidgetItem(QString("%1\n%2").arg(d.name, d.kind), list);
            it->setData(Qt::UserRole, d.root);
            it->setToolTip(QDir::toNativeSeparators(d.root) + (d.bytesTotal > 0 ? QString("\n%1 free of %2").arg(humanSize(d.bytesFree), humanSize(d.bytesTotal)) : QString()));
        }
        if (devs.empty()) {
            auto* it = new QListWidgetItem("(no removable drives or camera cards)", list);
            it->setFlags(Qt::NoItemFlags);
            current.clear();
            fillClips();
            return;
        }
        // keep the device shown if it is still there, else show the first
        int pick = 0;
        for (size_t k = 0; k < devs.size(); ++k)
            if (devs[k].root == current) pick = (int)k;
        list->setCurrentRow(pick);
        if (devs[(size_t)pick].root != current) {
            current = devs[(size_t)pick].root;
            fillClips();
        }
    }

    QString destFor(const QString& file) const { return QDir(q->importFolder()).filePath(QFileInfo(file).fileName()); }
    bool imported(const QString& file) const
    {
        const QFileInfo dst(destFor(file));
        return dst.exists() && dst.size() == QFileInfo(file).size();
    }

    void fillClips()
    {
        clips->clear();
        currentClips.clear();
        if (current.isEmpty()) {
            info->setText(QString());
            return;
        }
        QString kind;
        currentClips = findClips(current, &kind);
        qint64 total = 0;
        for (const QString& f : currentClips) {
            const QFileInfo fi(f);
            auto* it = new QTreeWidgetItem(clips);
            it->setText(0, fi.fileName());
            it->setText(1, humanSize(fi.size()));
            it->setText(2, fi.lastModified().toString("yyyy-MM-dd hh:mm"));
            it->setText(3, imported(f) ? QString("Imported") : QString());
            it->setData(0, Qt::UserRole, f);
            it->setToolTip(0, QDir::toNativeSeparators(f));
            total += fi.size();
        }
        info->setText(QString("%1: %2 clip%3, %4").arg(kind).arg(currentClips.size()).arg(currentClips.size() == 1 ? "" : "s").arg(humanSize(total)));
    }

    QStringList selected() const
    {
        QStringList out;
        for (QTreeWidgetItem* it : clips->selectedItems()) out << it->data(0, Qt::UserRole).toString();
        return out;
    }
};

DeviceExplorer::DeviceExplorer(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->q = this;
    setObjectName("deviceExplorer");
    Impl* impl = d.get();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* top = new QHBoxLayout();
    QPushButton* refresh = new QPushButton("Refresh", this);
    refresh->setObjectName("deviceRefresh");
    QPushButton* importSel = new QPushButton("Import Selected", this);
    importSel->setObjectName("deviceImportSelected");
    QPushButton* importAll = new QPushButton("Import All", this);
    importAll->setObjectName("deviceImportAll");
    top->addWidget(refresh);
    top->addStretch(1);
    top->addWidget(importSel);
    top->addWidget(importAll);
    v->addLayout(top);
    QSplitter* split = new QSplitter(Qt::Horizontal, this);
    split->setChildrenCollapsible(false);
    d->list = new QListWidget(split);
    d->list->setObjectName("deviceList");
    d->list->setIconSize(QSize(16, 16));
    d->clips = new QTreeWidget(split);
    d->clips->setObjectName("deviceClips");
    d->clips->setColumnCount(4);
    d->clips->setHeaderLabels(QStringList() << "Clip" << "Size" << "Date" << "");
    d->clips->setRootIsDecorated(false);
    d->clips->setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->clips->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    d->clips->setColumnWidth(0, 220);
    d->clips->setColumnWidth(1, 80);
    d->clips->setColumnWidth(2, 120);
    split->addWidget(d->list);
    split->addWidget(d->clips);
    split->setSizes({200, 480});
    v->addWidget(split, 1);
    d->info = new QLabel(this);
    d->info->setObjectName("deviceInfo");
    v->addWidget(d->info);
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel("Import to:", this));
    d->folder = new QLineEdit(this);
    d->folder->setObjectName("deviceImportFolder");
    d->folder->setPlaceholderText("beside the project, in \"Imported Media\"");
    d->folder->setText(QSettings().value("deviceExplorer/folder").toString());
    QPushButton* browse = new QPushButton("Browse...", this);
    row->addWidget(d->folder, 1);
    row->addWidget(browse);
    v->addLayout(row);

    d->poll = new QTimer(this);
    d->poll->setInterval(3000);
    QObject::connect(d->poll, &QTimer::timeout, this, [impl] { impl->fillDevices(); });
    QObject::connect(refresh, &QPushButton::clicked, this, [this] { this->refresh(); });
    QObject::connect(d->list, &QListWidget::currentRowChanged, this, [impl](int row) {
        if (row < 0 || row >= (int)impl->devs.size()) return;
        if (impl->devs[(size_t)row].root == impl->current) return;
        impl->current = impl->devs[(size_t)row].root;
        impl->fillClips();
    });
    QObject::connect(importSel, &QPushButton::clicked, this, [this, impl] {
        const QStringList s = impl->selected();
        if (!s.isEmpty()) importClips(s);
    });
    QObject::connect(importAll, &QPushButton::clicked, this, [this] { importClips(); });
    QObject::connect(d->clips, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* it, int) {
        importClips(QStringList() << it->data(0, Qt::UserRole).toString());
    });
    QObject::connect(d->folder, &QLineEdit::editingFinished, this, [impl] {
        QSettings().setValue("deviceExplorer/folder", impl->folder->text().trimmed());
        impl->fillClips();
    });
    QObject::connect(browse, &QPushButton::clicked, this, [this, impl] {
        const QString f = QFileDialog::getExistingDirectory(this, "Import Clips To", importFolder());
        if (f.isEmpty()) return;
        impl->folder->setText(QDir::toNativeSeparators(f));
        QSettings().setValue("deviceExplorer/folder", f);
        impl->fillClips();
    });
}

DeviceExplorer::~DeviceExplorer() {}

std::vector<DeviceExplorer::Device> DeviceExplorer::devices() const { return d->devs; }

void DeviceExplorer::refresh()
{
    d->devs.clear();
    d->list->clear();
    d->fillDevices();
    d->fillClips();
}

void DeviceExplorer::selectDevice(const QString& root)
{
    const QString r = QDir(root).absolutePath();
    for (size_t k = 0; k < d->devs.size(); ++k)
        if (d->devs[k].root == r) {
            d->list->setCurrentRow((int)k);
            d->current = r;
            d->fillClips();
        }
}

QStringList DeviceExplorer::clips() const { return d->currentClips; }

QString DeviceExplorer::importFolder() const
{
    const QString chosen = d->folder->text().trimmed();
    if (!chosen.isEmpty()) return QDir::fromNativeSeparators(chosen);
    const std::string proj = AppState::get().path();
    if (!proj.empty()) return QDir(QFileInfo(qs(proj)).absolutePath()).filePath("Imported Media");
    QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (docs.isEmpty()) docs = QDir::homePath();
    return QDir(docs).filePath("Reno Imported Media");
}

std::vector<uint64_t> DeviceExplorer::importClips(const QStringList& files, const QString& folder)
{
    std::vector<uint64_t> out;
    const QStringList list = files.isEmpty() ? d->currentClips : files;
    if (list.isEmpty()) return out;
    const QString dir = folder.isEmpty() ? importFolder() : folder;
    if (!QDir().mkpath(dir)) {
        AppState::get().status("The folder " + ss(QDir::toNativeSeparators(dir)) + " could not be made.");
        return out;
    }
    qint64 total = 0;
    for (const QString& f : list) total += QFileInfo(f).size();
    QProgressDialog progress("Importing clips...", "Cancel", 0, 1000, window());
    progress.setWindowTitle("Device Explorer");
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);
    qint64 done = 0;
    QStringList failed;
    AppState& app = AppState::get();
    for (const QString& f : list) {
        if (progress.wasCanceled()) break;
        const QFileInfo src(f);
        const QString dst = QDir(dir).filePath(src.fileName());
        // a clip already copied (same name and size) is skipped
        const QFileInfo have(dst);
        bool ok = have.exists() && have.size() == src.size();
        if (!ok) {
            QFile in(f), outF(dst);
            ok = in.open(QIODevice::ReadOnly) && outF.open(QIODevice::WriteOnly | QIODevice::Truncate);
            QByteArray buf;
            while (ok && !in.atEnd()) {
                buf = in.read(4 << 20);
                if (buf.isEmpty() && !in.atEnd()) ok = false;
                if (outF.write(buf) != buf.size()) ok = false;
                done += buf.size();
                progress.setValue(total > 0 ? (int)(done * 1000 / total) : 0);
                QCoreApplication::processEvents();
                if (progress.wasCanceled()) ok = false;
            }
            outF.close();
            if (!ok) QFile::remove(dst);
            else outF.setFileTime(src.lastModified(), QFileDevice::FileModificationTime);
        } else {
            done += src.size();
        }
        if (!ok) {
            failed << src.fileName();
            continue;
        }
        std::string err;
        const uint64_t id = app.importMedia(ss(dst), &err);
        if (id) out.push_back(id);
        else failed << QString("%1 (%2)").arg(src.fileName(), qs(err));
    }
    progress.setValue(1000);
    d->fillClips();
    app.status(failed.isEmpty() ? strfmt("Imported %d clip%s into %s.", (int)out.size(), out.size() == 1 ? "" : "s", ss(QDir::toNativeSeparators(dir)).c_str())
                                : "Could not import: " + ss(failed.join("; ")));
    return out;
}

void DeviceExplorer::setDeviceRoots(const QStringList& roots)
{
    d->fakeRoots = roots;
    d->useFake = true;
    refresh();
}

void DeviceExplorer::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    d->fillDevices();
    d->poll->start();
}

void DeviceExplorer::hideEvent(QHideEvent* e)
{
    QWidget::hideEvent(e);
    d->poll->stop();
}

}  // namespace app
}  // namespace reno
