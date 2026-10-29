// The Project Media window.
#include "panels_internal.h"
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
#include "theme.h"
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
#include <type_traits>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <cstring>
#include <unordered_map>

namespace reno {
namespace app {

namespace {

const QSize kThumb(96, 54);

struct ThumbJob {
    uint64_t id = 0;
    std::string path;
    bool hasVideo = false, hasAudio = false;
    double duration = 0;
    MediaPool::Opener opener;
};

QImage blankTile()
{
    QImage t(kThumb, QImage::Format_ARGB32_Premultiplied);
    t.fill(QColor(27, 27, 29));
    return t;
}

void drawNote(QPainter& p, const QRectF& r, const QColor& c)
{
    p.setPen(Qt::NoPen);
    p.setBrush(c);
    const QPointF m = r.center();
    p.drawEllipse(QPointF(m.x() - 5, m.y() + 8), 6, 4.5);
    p.drawRect(QRectF(m.x() - 0.5, m.y() - 14, 2.2, 22));
    QPainterPath flag;
    flag.moveTo(m.x() + 1.7, m.y() - 14);
    flag.quadTo(m.x() + 10, m.y() - 8, m.x() + 7, m.y() + 1);
    flag.quadTo(m.x() + 8, m.y() - 6, m.x() + 1.7, m.y() - 8);
    flag.closeSubpath();
    p.drawPath(flag);
}

// Runs on the thumbnail thread. Only shapes and pixels here, no text.
QImage makeThumbnail(const ThumbJob& job)
{
    QImage tile = blankTile();
    std::string err;
    std::unique_ptr<MediaReader> reader;
    try {
        reader = job.opener ? job.opener(job.path, &err) : openMediaOrProject(job.path, &err);
    } catch (...) {
    }
    QPainter p(&tile);
    p.setRenderHint(QPainter::Antialiasing);
    if (!reader) {
        p.setPen(QPen(QColor(200, 70, 60), 3));
        p.drawLine(QPointF(34, 14), QPointF(62, 40));
        p.drawLine(QPointF(62, 14), QPointF(34, 40));
        return tile;
    }
    const MediaInfo& mi = reader->info();
    if (job.hasVideo) {
        const double t = mi.isStill ? 0.0 : std::min(1.0, std::max(0.0, job.duration * 0.1));
        ImagePtr img;
        try {
            img = reader->videoFrame(t, kThumb.width() * 2, kThumb.height() * 2);
        } catch (...) {
        }
        if (img && img->w > 0 && img->h > 0) {
            const QImage full = toQImage(*img);
            const QImage small = full.scaled(kThumb, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            p.drawImage(QPoint((kThumb.width() - small.width()) / 2, (kThumb.height() - small.height()) / 2), small);
            return tile;
        }
    }
    if (job.hasAudio && mi.hasAudio && mi.duration > 0) {
        const int rate = mi.sampleRate > 0 ? mi.sampleRate : 48000;
        const int cols = kThumb.width() / 2, block = 600;
        std::vector<float> l(block), r(block), peaks(cols, 0.f);
        float top = 0.0001f;
        for (int k = 0; k < cols; ++k) {
            const int64_t start = (int64_t)(mi.duration * rate * (k + 0.25) / cols);
            if (!reader->audio(start, block, rate, l.data(), r.data())) break;
            float m = 0;
            for (int n = 0; n < block; ++n) m = std::max(m, std::max(std::fabs(l[n]), std::fabs(r[n])));
            peaks[k] = m;
            top = std::max(top, m);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(80, 200, 130));
        for (int k = 0; k < cols; ++k) {
            const double h = std::max(1.5, (double)(peaks[k] / top) * (kThumb.height() - 10));
            p.drawRect(QRectF(k * 2, (kThumb.height() - h) / 2, 1.4, h));
        }
        return tile;
    }
    drawNote(p, QRectF(QPointF(0, 0), QSizeF(kThumb)), QColor(150, 150, 160));
    return tile;
}

class ThumbWorker {
public:
    typedef std::function<void(uint64_t, const QImage&)> Deliver;
    explicit ThumbWorker(Deliver deliver) : deliver_(std::move(deliver))
    {
        thread_ = std::thread([this] {
            lowerThreadPriority();
            run();
        });
    }
    ~ThumbWorker()
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void add(const ThumbJob& job)
    {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.push_back(job);
        }
        cv_.notify_one();
    }

private:
    void run()
    {
        for (;;) {
            ThumbJob job;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this] { return quit_ || !queue_.empty(); });
                if (quit_) return;
                job = queue_.front();
                queue_.pop_front();
            }
            const QImage img = makeThumbnail(job);
            deliver_(job.id, img);
        }
    }
    Deliver deliver_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<ThumbJob> queue_;
    bool quit_ = false;
    std::thread thread_;
};

const int kRoleId = Qt::UserRole;
const char* const kMimeMediaList = "application/x-reno-media-list";  // several media ids, space-separated (dropped on a bin)

// The list of media. Carries a media id when dragged and accepts files dropped from the desktop.
class MediaTree : public QTreeWidget {
public:
    explicit MediaTree(QWidget* parent) : QTreeWidget(parent)
    {
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
    }
    std::function<void(const QStringList&)> onFiles;
    std::function<void()> onDelete;

protected:
    QStringList mimeTypes() const override { return QStringList() << kMimeMedia << kMimeMediaList; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        if (items.isEmpty()) return nullptr;
        QList<QTreeWidgetItem*> order = items;
        std::stable_sort(order.begin(), order.end(), [this](QTreeWidgetItem* x, QTreeWidgetItem* y) {
            const QRect a = visualItemRect(x), b = visualItemRect(y);
            return a.top() != b.top() ? a.top() < b.top() : a.left() < b.left();
        });
        if (QTreeWidgetItem* c = currentItem())
            if (order.removeOne(c)) order.prepend(c);
        auto* m = new QMimeData();
        m->setData(kMimeMedia, order.first()->data(0, kRoleId).toString().toUtf8());
        QStringList all;
        for (QTreeWidgetItem* it : order) all << it->data(0, kRoleId).toString();
        m->setData(kMimeMediaList, all.join(' ').toUtf8());
        return m;
    }
    static QStringList localFiles(const QMimeData* m)
    {
        QStringList files;
        if (m && m->hasUrls())
            for (const QUrl& u : m->urls())
                if (u.isLocalFile()) files << u.toLocalFile();
        return files;
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (!localFiles(e->mimeData()).isEmpty()) e->acceptProposedAction();
        else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (!localFiles(e->mimeData()).isEmpty()) e->acceptProposedAction();
        else e->ignore();
    }
    void dropEvent(QDropEvent* e) override
    {
        const QStringList files = localFiles(e->mimeData());
        if (files.isEmpty()) { e->ignore(); return; }
        e->acceptProposedAction();
        if (onFiles) onFiles(files);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete && onDelete) { onDelete(); return; }
        QTreeWidget::keyPressEvent(e);
    }
    void paintEvent(QPaintEvent* e) override
    {
        QTreeWidget::paintEvent(e);
        if (topLevelItemCount() == 0) {
            QPainter p(viewport());
            p.setPen(mutedText(this));
            p.drawText(viewport()->rect().adjusted(16, 16, -16, -16), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                       AppState::get().project().media.empty() ? QString("No media in this project yet.\nChoose Import Media, or drop files here.")
                                                               : QString("Nothing here: this bin is empty, or nothing matches the search.\nDrag media onto a bin to put it there."));
        }
    }
};

QString kindText(const MediaItem& m)
{
    if (m.generated) return "Generated";
    if (m.info.isStill) return "Picture";
    if (m.info.hasVideo) return "Video";
    if (m.info.hasAudio) return "Audio";
    return "Unknown";
}

QString formatText(const MediaItem& m)
{
    if (m.info.hasVideo) {
        QString t = QString("%1 x %2").arg(m.info.width).arg(m.info.height);
        if (!m.info.isStill && m.info.fps > 0) t += QString(", %1 fps").arg(fpsText(m.info.fps));
        return t;
    }
    if (m.info.hasAudio) {
        QString t = QString("%1 kHz").arg(m.info.sampleRate / 1000.0, 0, 'g', 4);
        if (m.info.channels == 1) t += ", mono";
        else if (m.info.channels == 2) t += ", stereo";
        else if (m.info.channels > 2)
            t += QString(", %1 channels%2").arg(m.info.channels).arg(m.info.channelLayout.empty() ? QString() : " (" + qs(m.info.channelLayout) + ")");
        return t;
    }
    return QString();
}

QIcon generatedTile()
{
    QImage t = blankTile();
    QPainter p(&t);
    QLinearGradient g(0, 0, kThumb.width(), kThumb.height());
    g.setColorAt(0, QColor(70, 40, 120));
    g.setColorAt(1, QColor(30, 100, 140));
    p.fillRect(t.rect(), g);
    p.setPen(QColor(240, 240, 245));
    QFont f;
    f.setBold(true);
    f.setPixelSize(20);
    p.setFont(f);
    p.drawText(t.rect(), Qt::AlignCenter, "FX");
    return QIcon(QPixmap::fromImage(t));
}

QIcon offlineTile()
{
    QImage t = blankTile();
    QPainter p(&t);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(200, 70, 60), 3));
    p.drawLine(QPointF(34, 14), QPointF(62, 40));
    p.drawLine(QPointF(62, 14), QPointF(34, 40));
    return QIcon(QPixmap::fromImage(t));
}

// Project Media's tool bar images for the VEGAS look: 0 Import Media, 1 Add to Timeline, 2 Remove from Project, 3
// Remove All Unused Media, 4 Properties, 5 views.
QIcon mediaBarIcon(int which)
{
    const qreal dpr = qApp ? std::max<qreal>(1.0, qApp->devicePixelRatio()) : 1.0;
    QPixmap pm(QSize((int)std::ceil(16 * dpr), (int)std::ceil(16 * dpr)));
    pm.setDevicePixelRatio(dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor light(226, 226, 226), rim(30, 30, 30);
    auto cross = [&](const QPointF& c, double r, const QColor& col) {
        p.setPen(QPen(rim, 4.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
        p.drawLine(QPointF(c.x() + r, c.y() - r), QPointF(c.x() - r, c.y() + r));
        p.setPen(QPen(col, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
        p.drawLine(QPointF(c.x() + r, c.y() - r), QPointF(c.x() - r, c.y() + r));
    };
    switch (which) {
    case 0: {
        QPainterPath folder;
        folder.moveTo(1.5, 4.5);
        folder.lineTo(6.0, 4.5);
        folder.lineTo(7.5, 6.0);
        folder.lineTo(14.5, 6.0);
        folder.lineTo(14.5, 14.5);
        folder.lineTo(1.5, 14.5);
        folder.closeSubpath();
        p.setPen(QPen(rim, 1.0));
        p.setBrush(QColor(196, 170, 104));
        p.drawPath(folder);
        QPolygonF arrow;
        arrow << QPointF(8.0, 13.0) << QPointF(4.5, 9.5) << QPointF(6.8, 9.5) << QPointF(6.8, 1.0) << QPointF(9.2, 1.0) << QPointF(9.2, 9.5) << QPointF(11.5, 9.5);
        p.setBrush(light);
        p.drawPolygon(arrow);
        break;
    }
    case 1: {
        p.setPen(QPen(rim, 1.0));
        p.setBrush(light);
        p.drawRect(QRectF(1.5, 1.5, 13.0, 6.0));
        p.setPen(Qt::NoPen);
        p.setBrush(rim);
        for (double x = 3.0; x < 14.0; x += 3.0) {
            p.drawRect(QRectF(x, 2.5, 1.2, 1.2));
            p.drawRect(QRectF(x, 5.3, 1.2, 1.2));
        }
        p.setBrush(QColor(18, 132, 214));
        p.setPen(QPen(rim, 1.0));
        p.drawRect(QRectF(1.5, 12.5, 13.0, 2.5));
        QPolygonF arrow;
        arrow << QPointF(8.0, 12.0) << QPointF(5.0, 9.0) << QPointF(11.0, 9.0);
        p.setBrush(light);
        p.drawPolygon(arrow);
        break;
    }
    case 2: cross(QPointF(8, 8), 4.5, QColor(232, 72, 64)); break;
    case 3: {
        p.setPen(QPen(rim, 1.0));
        p.setBrush(QColor(150, 150, 150));
        p.drawRect(QRectF(1.5, 1.5, 9.0, 7.0));
        p.setBrush(QColor(190, 190, 190));
        p.drawRect(QRectF(3.5, 3.5, 9.0, 7.0));
        cross(QPointF(10.5, 10.5), 3.5, light);
        break;
    }
    case 4: {
        p.setPen(QPen(rim, 1.0));
        p.setBrush(light);
        p.drawRect(QRectF(2.5, 1.5, 11.0, 13.0));
        p.setPen(QPen(QColor(90, 90, 90), 1.0));
        for (double y = 4.5; y < 13.0; y += 2.5) p.drawLine(QPointF(4.5, y), QPointF(11.5, y));
        break;
    }
    default: {
        p.setPen(Qt::NoPen);
        p.setBrush(light);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) p.drawRect(QRectF(1.5 + c * 4.6, 2.0 + r * 4.3, 3.6, 3.3));
        break;
    }
    }
    p.end();
    return QIcon(pm);
}

}  // namespace

namespace {

const int kRoleBinKey = Qt::UserRole + 10;     // a bins-tree row: "all", "bin:<path>", "smart:<name>", "type:<kind>"

class MediaTiles : public QListWidget {
public:
    explicit MediaTiles(QWidget* parent) : QListWidget(parent)
    {
        setViewMode(QListView::IconMode);
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setDragEnabled(true);
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DragDrop);
        setDefaultDropAction(Qt::CopyAction);
        setSelectionMode(QAbstractItemView::ExtendedSelection);
        setIconSize(kThumb);
        setGridSize(QSize(kThumb.width() + 24, kThumb.height() + 40));
        setWordWrap(true);
        setUniformItemSizes(true);
    }
    std::function<void(const QStringList&)> onFiles;
    std::function<void()> onDelete;

protected:
    QStringList mimeTypes() const override { return QStringList() << kMimeMedia << kMimeMediaList; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty()) return nullptr;
        QList<QListWidgetItem*> order = items;
        std::stable_sort(order.begin(), order.end(), [this](QListWidgetItem* x, QListWidgetItem* y) {
            const QRect a = visualItemRect(x), b = visualItemRect(y);
            return a.top() != b.top() ? a.top() < b.top() : a.left() < b.left();
        });
        if (QListWidgetItem* c = currentItem())
            if (order.removeOne(c)) order.prepend(c);
        auto* m = new QMimeData();
        m->setData(kMimeMedia, order.first()->data(kRoleId).toString().toUtf8());
        QStringList all;
        for (QListWidgetItem* it : order) all << it->data(kRoleId).toString();
        m->setData(kMimeMediaList, all.join(' ').toUtf8());
        return m;
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
        else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (e->mimeData()->hasUrls()) e->acceptProposedAction();
        else e->ignore();
    }
    void dropEvent(QDropEvent* e) override
    {
        QStringList files;
        for (const QUrl& u : e->mimeData()->urls())
            if (u.isLocalFile()) files << u.toLocalFile();
        if (files.isEmpty()) { e->ignore(); return; }
        e->acceptProposedAction();
        if (onFiles) onFiles(files);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Delete && onDelete) { onDelete(); return; }
        QListWidget::keyPressEvent(e);
    }
};

// The bins: Project Media (everything), bins made with Create New Bin, smart bins (saved searches), and media by type.
class BinTree : public QTreeWidget {
public:
    explicit BinTree(QWidget* parent) : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDropIndicatorShown(true);
        setHeaderHidden(true);
    }
    std::function<void(const QString& binKey, const std::vector<uint64_t>& ids)> onDropMedia;

protected:
    QString keyAt(const QPoint& pos) const
    {
        QTreeWidgetItem* it = itemAt(pos);
        if (!it) return QString();
        const QString k = it->data(0, kRoleBinKey).toString();
        return k == "all" || k.startsWith("bin:") ? k : QString();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMimeMediaList) || e->mimeData()->hasFormat(kMimeMedia)) e->acceptProposedAction();
        else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if ((e->mimeData()->hasFormat(kMimeMediaList) || e->mimeData()->hasFormat(kMimeMedia)) && !keyAt(e->position().toPoint()).isEmpty())
            e->acceptProposedAction();
        else e->ignore();
    }
    void dropEvent(QDropEvent* e) override
    {
        const QString key = keyAt(e->position().toPoint());
        const QByteArray list = e->mimeData()->hasFormat(kMimeMediaList) ? e->mimeData()->data(kMimeMediaList) : e->mimeData()->data(kMimeMedia);
        std::vector<uint64_t> ids;
        for (const QByteArray& part : list.split(' ')) {
            bool ok = false;
            const uint64_t id = part.trimmed().toULongLong(&ok);
            if (ok && id) ids.push_back(id);
        }
        if (key.isEmpty() || ids.empty()) { e->ignore(); return; }
        e->acceptProposedAction();
        if (onDropMedia) onDropMedia(key, ids);
    }
};

bool matchesSearch(const MediaItem& m, const QString& text)
{
    const QStringList words = text.split(' ', Qt::SkipEmptyParts);
    if (words.isEmpty()) return true;
    QString hay = qs(m.name) + " " + qs(m.path) + " " + qs(m.comment) + " " + kindText(m) + " " + formatText(m);
    for (const auto& t : m.tags) hay += " " + qs(t);
    for (const QString& w : words)
        if (!hay.contains(w, Qt::CaseInsensitive)) return false;
    return true;
}

std::set<uint64_t> usedMedia(const Project& p)
{
    std::set<uint64_t> used;
    for (const auto& t : p.tracks)
        for (const auto& e : t.events) {
            used.insert(e.mediaId);
            for (const auto& k : e.takes) used.insert(k.mediaId);
        }
    return used;
}

void showInFolder(const QString& file)
{
#ifdef _WIN32
    // Explorer reads "/select," and the path as written: QProcess would quote the whole argument when the path has a
    // space, and Explorer would then open Documents instead
    QProcess p;
    p.setProgram("explorer.exe");
    p.setNativeArguments("/select,\"" + QDir::toNativeSeparators(QFileInfo(file).absoluteFilePath()) + "\"");
    p.startDetached();
#else
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(file).absolutePath()));
#endif
}

class MediaPropertiesDialog : public QDialog {
public:
    MediaPropertiesDialog(QWidget* parent, const MediaItem& m) : QDialog(parent), id_(m.id)
    {
        setObjectName("mediaProperties");
        setWindowTitle("Properties");
        setMinimumWidth(460);
        auto* root = new QVBoxLayout(this);
        auto* tabs = new QTabWidget(this);
        tabs->setObjectName("mediaTabs");
        root->addWidget(tabs);

        auto* gen = new QWidget(tabs);
        auto* gf = new QFormLayout(gen);
        name_ = new QLineEdit(qs(m.name), gen);
        name_->setObjectName("mediaName");
        gf->addRow("Name:", name_);
        auto* path = new QLabel(m.generated ? QString("(generated by %1)").arg(qs(m.generator.name)) : QDir::toNativeSeparators(qs(m.path)), gen);
        path->setObjectName("mediaPath");
        path->setWordWrap(true);
        path->setTextInteractionFlags(Qt::TextSelectableByMouse);
        gf->addRow("File name:", path);
        if (!m.generated && !m.path.empty()) {
            const QFileInfo fi(qs(m.path));
            gf->addRow("File size:", new QLabel(fi.exists() ? sizeText(fi.size()) : QString("(the file is missing)"), gen));
            gf->addRow("Modified:", new QLabel(fi.exists() ? QLocale().toString(fi.lastModified(), QLocale::ShortFormat) : QString("-"), gen));
        }
        gf->addRow("Type:", new QLabel(m.offline ? QString("Offline") : kindText(m), gen));
        if (m.subclipLength > 0) {
            const double fps = m.info.fps > 0 ? m.info.fps : 30.0;
            auto* sub = new QLabel(QString("from %1 to %2 of the file").arg(qs(timecode(m.subclipStart, fps)), qs(timecode(m.subclipStart + m.subclipLength, fps))), gen);
            sub->setObjectName("subclipRange");
            gf->addRow("Subclip:", sub);
        }
        QStringList tagList;
        for (const auto& t : m.tags) tagList << qs(t);
        tags_ = new QLineEdit(tagList.join(", "), gen);
        tags_->setObjectName("mediaTags");
        tags_->setPlaceholderText("words separated by commas");
        gf->addRow("Media tags:", tags_);
        comment_ = new QPlainTextEdit(qs(m.comment), gen);
        comment_->setObjectName("mediaComment");
        comment_->setFixedHeight(70);
        comment_->setTabChangesFocus(true);
        gf->addRow("Comment:", comment_);
        tabs->addTab(gen, "General");

        auto* med = new QWidget(tabs);
        auto* mf = new QFormLayout(med);
        const MediaInfo& i = m.info;
        if (i.hasVideo) {
            mf->addRow(new QLabel("<b>Video stream</b>", med));
            mf->addRow("Format:", new QLabel(qs(i.videoCodec.empty() ? std::string("-") : i.videoCodec), med));
            mf->addRow("Frame size:", new QLabel(QString("%1 x %2 (as shown: turned and with square pixels)").arg(i.width).arg(i.height), med));
            if (!i.isStill) mf->addRow("Frame rate:", new QLabel(QString("%1 fps").arg(i.fps, 0, 'f', 3), med));
            fileInfo_ = i;
            if (!m.generated && !m.offline && !m.path.empty() && AppState::get().mediaOpener) {
                std::string err;
                if (std::unique_ptr<MediaReader> r = AppState::get().mediaOpener(m.readPath(), &err))
                    if (r->info().hasVideo) fileInfo_ = r->info();
            }
            mf->addRow("Bit depth:", new QLabel(QString("%1 bits per sample").arg(i.bitDepth), med));
            alpha_ = new QComboBox(med);
            alpha_->setObjectName("mediaAlpha");
            alpha_->addItem("Undefined (as the file says)", "");
            alpha_->addItem("None", "none");
            alpha_->addItem("Straight (unmatted)", "straight");
            alpha_->addItem("Premultiplied", "premultiplied");
            alpha_->setCurrentIndex(std::max(0, alpha_->findData(qs(m.alphaMode))));
            alpha_->setToolTip("How transparency is read: None shows the picture opaque; Premultiplied is for files whose colors are already "
                               "multiplied by their alpha (otherwise dark fringes show at the edges)");
            mf->addRow("Alpha channel:", alpha_);
            colorSpace_ = new QComboBox(med);
            colorSpace_->setObjectName("mediaColorSpace");
            for (const ColorSpaceChoice& c : mediaColorSpaces(AppState::get().project().settings.acesVersion)) colorSpace_->addItem(qs(c.label), qs(c.id));
            if (colorSpace_->findData(qs(m.colorSpace)) < 0) colorSpace_->addItem(qs(colorSpaceLabel(m.colorSpace)), qs(m.colorSpace));
            colorSpace_->setCurrentIndex(std::max(0, colorSpace_->findData(qs(m.colorSpace))));
            colorSpace_->setToolTip("The file's color space: used only when Project Properties' pixel format is 32-bit floating point (full range)");
            mf->addRow("Color space:", colorSpace_);
            // Field order, Pixel aspect ratio and Rotation can each override what the file says
            // (MediaItem::fieldOrderOverride & co).
            field_ = new QComboBox(med);
            field_->setObjectName("mediaFieldOrder");
            field_->addItem("None (progressive scan)", "none");
            field_->addItem("Upper field first", "upper");
            field_->addItem("Lower field first", "lower");
            const std::string fieldNow = m.fieldOrderOverride.empty() ? fileInfo_.fieldOrder : m.fieldOrderOverride;
            field_->setCurrentIndex(std::max(0, field_->findData(qs(fieldNow))));
            field_->setToolTip("Which field of an interlaced picture comes first - or progressive, when the file only says so wrongly. "
                               "Progressive media is never deinterlaced.");
            mf->addRow("Field order:", field_);
            par_ = new QComboBox(med);
            par_->setObjectName("mediaPixelAspect");
            static const struct { const char* label; double v; } pars[] = {
                {"1.0000 (Square)", 1.0}, {"0.9091 (NTSC DV)", 0.9091}, {"1.2121 (NTSC DV Widescreen)", 1.2121}, {"1.0926 (PAL DV)", 1.0926},
                {"1.4568 (PAL DV Widescreen)", 1.4568}, {"1.3333 (HDV 1080)", 1.3333}, {"2.0000 (Anamorphic 2:1)", 2.0}};
            for (const auto& pr : pars) par_->addItem(pr.label, pr.v);
            const double parNow = m.pixelAspectOverride > 0 ? m.pixelAspectOverride : fileInfo_.pixelAspect;
            int at = -1;
            for (int k = 0; k < par_->count(); ++k)
                if (std::fabs(par_->itemData(k).toDouble() - parNow) < 0.0005) at = k;
            if (at < 0) {
                par_->addItem(QString::number(parNow, 'f', 4), parNow);
                at = par_->count() - 1;
            }
            par_->setCurrentIndex(at);
            par_->setToolTip("Pixel shape (width / height): 1.0 for computer pictures, 0.9091 for NTSC DV. Changes how wide the picture is shown.");
            mf->addRow("Pixel aspect ratio:", par_);
            rot_ = new QComboBox(med);
            rot_->setObjectName("mediaRotation");
            rot_->addItem(QString::fromUtf8("0\u00b0 (original)"), 0);
            rot_->addItem(QString::fromUtf8("90\u00b0 clockwise"), 90);
            rot_->addItem(QString::fromUtf8("180\u00b0"), 180);
            rot_->addItem(QString::fromUtf8("90\u00b0 counter-clockwise"), 270);
            rot_->setCurrentIndex(std::max(0, rot_->findData(m.rotationOverride)));
            rot_->setToolTip("Turns the picture, on top of any turn the file itself asks for");
            mf->addRow("Rotation:", rot_);
        }
        if (i.hasAudio) {
            mf->addRow(new QLabel("<b>Audio stream</b>", med));
            mf->addRow("Format:", new QLabel(qs(i.audioCodec.empty() ? std::string("-") : i.audioCodec), med));
            mf->addRow("Sample rate:", new QLabel(QString("%1 Hz").arg(i.sampleRate), med));
            {
                QString ch = i.channels == 1 ? QString("Mono") : i.channels == 2 ? QString("Stereo") : QString::number(i.channels);
                if (i.channels > 2 && !i.channelLayout.empty()) ch += " (" + qs(i.channelLayout) + ")";
                QStringList names;
                for (size_t c = 0; c < i.channelNames.size(); ++c)
                    names << (i.channelNames[c].empty() ? QString::number(c + 1) : QString("%1 %2").arg(c + 1).arg(qs(i.channelNames[c])));
                if (i.channels > 2 && !names.isEmpty()) ch += ": " + names.join(", ");
                QLabel* cl = new QLabel(ch, med);
                cl->setObjectName("mediaChannels");
                cl->setWordWrap(true);
                mf->addRow("Channels:", cl);
            }
        }
        if (!i.isStill && i.duration > 0) mf->addRow("Length:", new QLabel(qs(timecode(i.duration, i.fps > 0 ? i.fps : 30.0)), med));
        if (!i.markers.empty()) mf->addRow("Markers in the file:", new QLabel(QString::number(i.markers.size()), med));
        tabs->addTab(med, "Media");

        auto* row = new QHBoxLayout();
        row->addStretch(1);
        auto* ok = new QPushButton("OK", this);
        ok->setObjectName("ok");
        ok->setDefault(true);
        auto* cancel = new QPushButton("Cancel", this);
        cancel->setObjectName("cancel");
        row->addWidget(ok);
        row->addWidget(cancel);
        root->addLayout(row);
        QObject::connect(ok, &QPushButton::clicked, this, [this] { apply(); accept(); });
        QObject::connect(cancel, &QPushButton::clicked, this, [this] { reject(); });
    }

private:
    void apply()
    {
        const std::string name = name_->text().trimmed().toStdString();
        std::vector<std::string> tags;
        for (const QString& t : tags_->text().split(',', Qt::SkipEmptyParts))
            if (!t.trimmed().isEmpty()) tags.push_back(t.trimmed().toStdString());
        const std::string comment = comment_->toPlainText().toStdString();
        const MediaItem* m = AppState::get().project().findMedia(id_);
        if (!m || name.empty()) return;
        const std::string alpha = alpha_ ? alpha_->currentData().toString().toStdString() : m->alphaMode;
        const std::string space = colorSpace_ ? colorSpace_->currentData().toString().toStdString() : m->colorSpace;
        std::string field = m->fieldOrderOverride;
        double par = m->pixelAspectOverride;
        int rot = m->rotationOverride;
        if (field_) {
            const std::string f = field_->currentData().toString().toStdString();
            const std::string was = m->fieldOrderOverride.empty() ? fileInfo_.fieldOrder : m->fieldOrderOverride;
            if (f != was) field = f == fileInfo_.fieldOrder ? std::string() : f;
        }
        if (par_) {
            const double v = par_->currentData().toDouble();
            const double was = m->pixelAspectOverride > 0 ? m->pixelAspectOverride : fileInfo_.pixelAspect;
            if (std::fabs(v - was) > 0.0005) par = std::fabs(v - fileInfo_.pixelAspect) < 0.0005 ? 0.0 : v;
        }
        if (rot_) rot = rot_->currentData().toInt();
        if (m->name == name && m->tags == tags && m->comment == comment && m->alphaMode == alpha && m->colorSpace == space &&
            m->fieldOrderOverride == field && m->pixelAspectOverride == par && m->rotationOverride == rot)
            return;
        const uint64_t id = id_;
        const MediaInfo fi = fileInfo_;
        const bool haveFile = fileInfo_.hasVideo;
        AppState::get().edit("Media Properties", [=](Project& p) {
            if (MediaItem* mm = p.findMedia(id)) {
                mm->name = name;
                mm->tags = tags;
                mm->comment = comment;
                mm->alphaMode = alpha;
                mm->colorSpace = space;
                mm->fieldOrderOverride = field;
                mm->pixelAspectOverride = par;
                mm->rotationOverride = rot;
                if (haveFile && (field.size() || par > 0 || rot)) {
                    mm->info.pixelAspect = fi.pixelAspect;
                    mm->info.rotation = fi.rotation;
                    mm->info.fieldOrder = fi.fieldOrder;
                }
            }
        });
    }
    uint64_t id_;
    MediaInfo fileInfo_;
    QComboBox *field_ = nullptr, *par_ = nullptr, *rot_ = nullptr;
    QComboBox* alpha_ = nullptr;
    QComboBox* colorSpace_ = nullptr;
    QLineEdit *name_ = nullptr, *tags_ = nullptr;
    QPlainTextEdit* comment_ = nullptr;
};

}  // namespace

std::function<bool(uint64_t& mediaId, double& in, double& out)> trimmerSelection;

uint64_t createSubclip(uint64_t mediaId, double in, double out, const std::string& name)
{
    AppState& app = AppState::get();
    const MediaItem* m0 = app.project().findMedia(mediaId);
    if (!m0 || m0->generated || m0->info.isStill || !(out > in) || in < 0) return 0;
    const MediaItem src = *m0;
    uint64_t made = 0;
    app.edit("Create Subclip", [&](Project& p) {
        MediaItem m = src;
        m.id = p.newId();
        m.veg.reset();
        const double base = src.subclipLength > 0 ? src.subclipStart : 0.0;
        m.subclipStart = base + in;
        m.subclipLength = out - in;
        if (src.subclipLength > 0) m.subclipLength = std::min(m.subclipLength, src.subclipLength - in);
        if (src.info.duration > 0) m.subclipLength = std::min(m.subclipLength, std::max(0.0, src.info.duration + (src.subclipLength > 0 ? src.subclipStart : 0.0) - m.subclipStart));
        m.info.duration = m.subclipLength;
        m.name = name.empty() ? src.name + " Subclip" : name;
        p.media.push_back(m);
        made = m.id;
    });
    return made;
}

struct MediaBin::Impl {
    MediaBin* owner = nullptr;
    MediaTree* tree = nullptr;
    MediaTiles* tiles = nullptr;
    BinTree* bins = nullptr;
    QLineEdit* search = nullptr;
    QToolButton *viewButton = nullptr, *removeUnused = nullptr, *properties = nullptr;
    QPushButton *importButton = nullptr, *addButton = nullptr, *removeButton = nullptr;
    QLabel* countLabel = nullptr;
    int view = 0;
    QString binKey = "all";
    Alive alive = makeAlive();
    std::map<uint64_t, QIcon> icons;
    std::map<uint64_t, bool> requested;
    std::string signature;
    std::unique_ptr<ThumbWorker> worker;
    std::unique_ptr<ListenerHandle> listener;
    QIcon generatedIcon, offlineIcon, pendingIcon;

    ~Impl()
    {
        alive->store(false);
        listener.reset();
        worker.reset();
    }

    QTreeWidgetItem* itemFor(uint64_t id) const
    {
        for (int k = 0; k < tree->topLevelItemCount(); ++k)
            if (tree->topLevelItem(k)->data(0, kRoleId).toULongLong() == id) return tree->topLevelItem(k);
        return nullptr;
    }

    std::vector<uint64_t> selectedIds() const
    {
        std::vector<uint64_t> ids;
        if (view == 2) {
            for (QListWidgetItem* it : tiles->selectedItems()) ids.push_back(it->data(kRoleId).toULongLong());
        } else {
            for (QTreeWidgetItem* it : tree->selectedItems()) ids.push_back(it->data(0, kRoleId).toULongLong());
        }
        return ids;
    }

    void thumbArrived(uint64_t id, const QImage& img)
    {
        icons[id] = QIcon(QPixmap::fromImage(img));
        if (QTreeWidgetItem* it = itemFor(id)) it->setIcon(0, icons[id]);
        for (int k = 0; k < tiles->count(); ++k)
            if (tiles->item(k)->data(kRoleId).toULongLong() == id) tiles->item(k)->setIcon(icons[id]);
    }

    bool shown(const MediaItem& m, const Project& p) const
    {
        if (!matchesSearch(m, search->text())) return false;
        if (binKey == "all") return true;
        if (binKey.startsWith("bin:")) return qs(m.bin) == binKey.mid(4);
        if (binKey.startsWith("smart:")) {
            const std::string name = binKey.mid(6).toStdString();
            for (const auto& sb : p.smartBins)
                if (sb.first == name) return matchesSearch(m, qs(sb.second));
            return false;
        }
        if (binKey == "type:video") return !m.generated && m.info.hasVideo && !m.info.isStill;
        if (binKey == "type:audio") return !m.generated && m.info.hasAudio && !m.info.hasVideo;
        if (binKey == "type:image") return !m.generated && m.info.isStill;
        if (binKey == "type:generated") return m.generated;
        return true;
    }

    QIcon iconFor(const MediaItem& m)
    {
        if (m.offline) return offlineIcon;
        if (m.generated) return generatedIcon;
        auto it = icons.find(m.id);
        if (it != icons.end()) return it->second;
        if (!requested[m.id]) {
            requested[m.id] = true;
            ThumbJob job;
            job.id = m.id;
            job.path = m.path;
            job.hasVideo = m.info.hasVideo;
            job.hasAudio = m.info.hasAudio;
            job.duration = m.info.duration;
            job.opener = AppState::get().mediaOpener;
            worker->add(job);
        }
        return pendingIcon;
    }

    void refreshBins()
    {
        const Project& p = AppState::get().project();
        QSignalBlocker block(bins);
        bins->clear();
        auto* root = new QTreeWidgetItem(bins, QStringList() << "Project Media");
        root->setData(0, kRoleBinKey, "all");
        root->setToolTip(0, "All the project's media; drop media here to take it out of its bin");
        std::map<std::string, QTreeWidgetItem*> made;
        std::vector<std::string> all = p.mediaBins;
        for (const auto& m : p.media)
            if (!m.bin.empty() && std::find(all.begin(), all.end(), m.bin) == all.end()) all.push_back(m.bin);
        std::sort(all.begin(), all.end());
        std::function<QTreeWidgetItem*(const std::string&)> binItem = [&](const std::string& path) -> QTreeWidgetItem* {
            auto it = made.find(path);
            if (it != made.end()) return it->second;
            const size_t slash = path.find_last_of('/');
            QTreeWidgetItem* parent = slash == std::string::npos ? root : binItem(path.substr(0, slash));
            auto* item = new QTreeWidgetItem(parent, QStringList() << qs(slash == std::string::npos ? path : path.substr(slash + 1)));
            item->setData(0, kRoleBinKey, "bin:" + qs(path));
            item->setIcon(0, owner->style()->standardIcon(QStyle::SP_DirIcon));
            made[path] = item;
            return item;
        };
        for (const auto& b : all) binItem(b);
        for (const auto& sb : p.smartBins) {
            auto* item = new QTreeWidgetItem(root, QStringList() << qs(sb.first));
            item->setData(0, kRoleBinKey, "smart:" + qs(sb.first));
            item->setToolTip(0, QString("A smart bin: the media that match \"%1\"").arg(qs(sb.second)));
            item->setIcon(0, owner->style()->standardIcon(QStyle::SP_FileDialogContentsView));
        }
        auto* types = new QTreeWidgetItem(root, QStringList() << "By Type");
        types->setData(0, kRoleBinKey, "all");
        for (const char* t : {"Video", "Audio", "Images", "Generated Media"}) {
            auto* item = new QTreeWidgetItem(types, QStringList() << t);
            const QString k = QString(t) == "Images" ? "type:image" : QString(t) == "Generated Media" ? "type:generated" : "type:" + QString(t).toLower();
            item->setData(0, kRoleBinKey, k);
        }
        bins->expandItem(root);
        QTreeWidgetItem* pick = root;
        for (QTreeWidgetItemIterator it(bins); *it; ++it)
            if ((*it)->data(0, kRoleBinKey).toString() == binKey && *it != types) pick = *it;
        if (pick == root) binKey = "all";
        for (QTreeWidgetItem* up = pick->parent(); up; up = up->parent()) up->setExpanded(true);
        bins->setCurrentItem(pick);
    }

    void refresh(bool force = false)
    {
        const Project& p = AppState::get().project();
        // What the lists show, as bytes (numbers as they are, text with its length): computed on every project change,
        // so it must be quick - hundreds of media and thousands of events
        std::string sig;
        sig.reserve(64 + p.media.size() * 96);
        auto num = [&sig](uint64_t v) { sig.append(reinterpret_cast<const char*>(&v), sizeof v); };
        auto text = [&](const std::string& t) { num(t.size()); sig += t; };
        text(binKey.toStdString());
        text(search->text().toStdString());
        num((uint64_t)view);
        for (const auto& b : p.mediaBins) text(b);
        for (const auto& b : p.smartBins) { text(b.first); text(b.second); }
        std::unordered_map<uint64_t, int> usesOf;
        usesOf.reserve(p.media.size() * 2 + 1);
        for (const auto& t : p.tracks)
            for (const auto& e : t.events) ++usesOf[e.mediaId];
        for (const auto& m : p.media) {
            const auto u = usesOf.find(m.id);
            num(m.id);
            text(m.name);
            num(m.offline ? 1 : 0);
            text(m.path);
            double dur = m.info.duration;
            uint64_t bits;
            std::memcpy(&bits, &dur, sizeof bits);
            num(bits);
            text(m.bin);
            text(m.comment);
            num(u == usesOf.end() ? 0 : (uint64_t)u->second);
            for (const auto& t : m.tags) text(t);
            num(m.tags.size());
        }
        if (sig == signature && !force) return;
        signature = sig;
        refreshBins();

        const std::vector<uint64_t> keep = selectedIds();
        const int scroll = tree->verticalScrollBar()->value();
        tree->setUpdatesEnabled(false);
        tree->clear();
        tiles->clear();
        int count = 0;
        for (const auto& m : p.media) {
            if (!shown(m, p)) continue;
            ++count;
            auto* it = new QTreeWidgetItem(tree);
            it->setData(0, kRoleId, QVariant::fromValue<qulonglong>(m.id));
            it->setText(0, qs(m.name.empty() ? pathFileName(m.path) : m.name));
            it->setText(1, m.offline ? "Offline" : m.subclipLength > 0 ? kindText(m) + " (subclip)" : kindText(m));
            it->setText(2, (m.info.isStill || m.generated || m.info.duration <= 0) ? QString(m.info.isStill ? "Still" : "") : qs(timecode(m.info.duration, m.info.fps > 0 ? m.info.fps : 30.0)));
            it->setText(3, formatText(m));
            QStringList tags;
            for (const auto& t : m.tags) tags << qs(t);
            it->setText(4, tags.join(", "));
            it->setText(5, qs(m.comment).replace('\n', ' '));
            const auto u = usesOf.find(m.id);
            it->setText(6, QString::number(u == usesOf.end() ? 0 : u->second));
            it->setText(7, m.generated ? QString() : QDir::toNativeSeparators(qs(m.path)));
            it->setSizeHint(0, QSize(10, view == 1 ? 22 : kThumb.height() + 6));
            it->setToolTip(0, m.generated ? QString("Generated by %1").arg(qs(m.generator.name)) : qs(m.path));
            if (m.offline) {
                it->setToolTip(0, qs(m.path) + "\nThis file could not be opened.");
                for (int c = 0; c < 8; ++c) it->setForeground(c, QColor(230, 110, 100));
            }
            const QIcon icon = iconFor(m);
            it->setIcon(0, icon);
            if (std::find(keep.begin(), keep.end(), m.id) != keep.end()) it->setSelected(true);
            auto* tile = new QListWidgetItem(icon, it->text(0), tiles);
            tile->setData(kRoleId, QVariant::fromValue<qulonglong>(m.id));
            tile->setToolTip(it->toolTip(0));
            if (std::find(keep.begin(), keep.end(), m.id) != keep.end()) tile->setSelected(true);
        }
        tree->setUpdatesEnabled(true);
        tree->verticalScrollBar()->setValue(scroll);
        countLabel->setText(QString("%1 of %2").arg(count).arg(p.media.size()));
        updateButtons();
    }

    void setView(int v)
    {
        view = std::max(0, std::min(2, v));
        QSettings().setValue("media/view", view);
        tree->setVisible(view != 2);
        tiles->setVisible(view == 2);
        tree->setIconSize(view == 1 ? QSize(28, 16) : kThumb);
        for (int c = 1; c < tree->columnCount(); ++c) tree->setColumnHidden(c, view == 1);
        tree->setHeaderHidden(view == 1);
        applyLook();
        refresh(true);
    }

    // The tool bar: words in Reno's look, small images (actions in tool tips) in VEGAS's.
    void applyLook()
    {
        const bool reno = renoTheme();
        const QString viewName = view == 0 ? "Detailed" : view == 1 ? "List" : "Thumbnails";
        struct B { QAbstractButton* b; const char* text; int icon; const char* tip; };
        const B all[] = {{importButton, "Import Media...", 0, "Import Media (Ctrl+I)"},
                         {addButton, "Add to Timeline", 1, "Add to Timeline: put the selected media on the timeline at the cursor"},
                         {removeButton, "Remove from Project", 2, "Remove from Project: take the selected media out of the project (only if no event uses it)"},
                         {removeUnused, "Remove Unused", 3, "Remove All Unused Media: everything no event uses"},
                         {properties, "Properties...", 4, "Media Properties: the selected media's streams, name, tags and comment"}};
        for (const B& x : all) {
            if (!x.b) continue;
            x.b->setText(reno ? QString(x.text) : QString());
            x.b->setIcon(reno ? QIcon() : mediaBarIcon(x.icon));
            x.b->setIconSize(QSize(16, 16));
            x.b->setToolTip(x.tip);
            if (QPushButton* pb = qobject_cast<QPushButton*>(x.b)) {
                pb->setFlat(!reno);
                if (reno) {
                    pb->setMinimumSize(0, 0);
                    pb->setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
                } else {
                    pb->setFixedSize(26, 24);
                }
            } else if (QToolButton* tb = qobject_cast<QToolButton*>(x.b)) {
                tb->setAutoRaise(!reno);
                tb->setToolButtonStyle(reno ? Qt::ToolButtonTextOnly : Qt::ToolButtonIconOnly);
            }
        }
        viewButton->setText(reno ? viewName + " ▾" : QString("▾"));
        viewButton->setIcon(reno ? QIcon() : mediaBarIcon(5));
        viewButton->setToolButtonStyle(reno ? Qt::ToolButtonTextOnly : Qt::ToolButtonTextBesideIcon);
        viewButton->setAutoRaise(!reno);
        viewButton->setToolTip("Views: " + viewName);
    }

    void updateButtons()
    {
        const bool any = !selectedIds().empty();
        addButton->setEnabled(any);
        removeButton->setEnabled(any);
        properties->setEnabled(selectedIds().size() == 1);
    }

    void addToTimeline(const std::vector<uint64_t>& ids)
    {
        AppState& app = AppState::get();
        double at = app.cursor();
        for (uint64_t id : ids) {
            const std::vector<uint64_t> made = app.addMediaToTimeline(id, at, app.currentTrack);
            for (uint64_t ev : made)
                if (const Event* e = app.project().findEvent(ev)) at = std::max(at, e->end());
        }
    }

    void removeSelected()
    {
        AppState& app = AppState::get();
        const std::vector<uint64_t> ids = selectedIds();
        if (ids.empty()) return;
        const std::set<uint64_t> used = usedMedia(app.project());
        std::vector<uint64_t> free;
        QStringList busy;
        for (uint64_t id : ids) {
            if (used.count(id)) {
                const MediaItem* m = app.project().findMedia(id);
                busy << (m ? qs(m->name) : QString::number(id));
            } else {
                free.push_back(id);
            }
        }
        if (!free.empty() && vprefs::on("prefs/confirmRemoveMedia") &&
            QMessageBox::question(owner->window(), "Remove from Project", QString("Remove %1 media file%2 from the project?").arg(free.size()).arg(free.size() == 1 ? "" : "s")) != QMessageBox::Yes)
            return;
        if (!free.empty())
            app.edit("Remove Media", [free](Project& pr) {
                for (uint64_t id : free)
                    pr.media.erase(std::remove_if(pr.media.begin(), pr.media.end(), [id](const MediaItem& m) { return m.id == id; }), pr.media.end());
            });
        if (!busy.isEmpty())
            QMessageBox::information(owner->window(), "Remove from Project",
                                     QString("%1 still used by events on the timeline, so %2 not removed:\n\n%3\n\nDelete those events first.")
                                         .arg(busy.size() == 1 ? "This media is" : "These media are", busy.size() == 1 ? "it was" : "they were", busy.join("\n")));
    }

    int removeUnusedMedia()
    {
        AppState& app = AppState::get();
        const std::set<uint64_t> used = usedMedia(app.project());
        int n = 0;
        for (const auto& m : app.project().media) n += !used.count(m.id);
        if (n == 0) {
            app.status("Remove All Unused Media: every media file is used.");
            return 0;
        }
        if (vprefs::on("prefs/confirmRemoveMedia") &&
            QMessageBox::question(owner->window(), "Remove from Project", QString("Remove %1 unused media file%2 from the project?").arg(n).arg(n == 1 ? "" : "s")) != QMessageBox::Yes)
            return 0;
        app.edit("Remove All Unused Media", [used](Project& pr) {
            pr.media.erase(std::remove_if(pr.media.begin(), pr.media.end(), [&used](const MediaItem& m) { return !used.count(m.id); }), pr.media.end());
        });
        app.status(strfmt("Removed %d unused media file%s.", n, n == 1 ? "" : "s"));
        return n;
    }

    void moveToBin(const QString& key, const std::vector<uint64_t>& ids)
    {
        const std::string bin = key.startsWith("bin:") ? key.mid(4).toStdString() : std::string();
        AppState::get().edit(bin.empty() ? "Remove from Bin" : "Move to Bin", [bin, ids](Project& p) {
            for (uint64_t id : ids)
                if (MediaItem* m = p.findMedia(id)) m->bin = bin;
        });
    }

    void newBin()
    {
        const std::string parent = binKey.startsWith("bin:") ? binKey.mid(4).toStdString() : std::string();
        const Project& p = AppState::get().project();
        std::string name;
        for (int n = 1; n < 1000; ++n) {
            name = (parent.empty() ? std::string() : parent + "/") + (n == 1 ? std::string("New Bin") : strfmt("New Bin %d", n));
            if (std::find(p.mediaBins.begin(), p.mediaBins.end(), name) == p.mediaBins.end()) break;
        }
        AppState::get().edit("Create New Bin", [name](Project& pr) { pr.mediaBins.push_back(name); });
        binKey = "bin:" + qs(name);
        refresh(true);
    }

    void renameBin()
    {
        if (!binKey.startsWith("bin:") && !binKey.startsWith("smart:")) return;
        const bool smart = binKey.startsWith("smart:");
        const QString old = binKey.mid(smart ? 6 : 4);
        const QString leaf = smart ? old : old.section('/', -1);
        bool ok = false;
        const QString n = QInputDialog::getText(owner->window(), "Rename", "Name:", QLineEdit::Normal, leaf, &ok).trimmed().replace('/', '-');
        if (!ok || n.isEmpty() || n == leaf) return;
        if (smart) {
            AppState::get().edit("Rename Bin", [old, n](Project& p) {
                for (auto& sb : p.smartBins)
                    if (qs(sb.first) == old) sb.first = n.toStdString();
            });
            binKey = "smart:" + n;
        } else {
            const QString parent = old.contains('/') ? old.section('/', 0, -2) + "/" : QString();
            const std::string from = old.toStdString(), to = (parent + n).toStdString();
            AppState::get().edit("Rename Bin", [from, to](Project& p) {
                auto moved = [&](const std::string& s) { return s == from || s.compare(0, from.size() + 1, from + "/") == 0 ? to + s.substr(from.size()) : s; };
                for (auto& b : p.mediaBins) b = moved(b);
                for (auto& m : p.media) m.bin = moved(m.bin);
            });
            binKey = "bin:" + qs(to);
        }
        refresh(true);
    }

    void deleteBin()
    {
        if (binKey.startsWith("smart:")) {
            const std::string name = binKey.mid(6).toStdString();
            AppState::get().edit("Delete Bin", [name](Project& p) {
                p.smartBins.erase(std::remove_if(p.smartBins.begin(), p.smartBins.end(), [&](const std::pair<std::string, std::string>& b) { return b.first == name; }), p.smartBins.end());
            });
        } else if (binKey.startsWith("bin:")) {
            const std::string path = binKey.mid(4).toStdString();
            const size_t slash = path.find_last_of('/');
            const std::string up = slash == std::string::npos ? std::string() : path.substr(0, slash);
            AppState::get().edit("Delete Bin", [path, up](Project& p) {
                auto inside = [&](const std::string& s) { return s == path || s.compare(0, path.size() + 1, path + "/") == 0; };
                p.mediaBins.erase(std::remove_if(p.mediaBins.begin(), p.mediaBins.end(), inside), p.mediaBins.end());
                for (auto& m : p.media)
                    if (inside(m.bin)) m.bin = up;
            });
        } else {
            return;
        }
        binKey = "all";
        refresh(true);
    }

    void saveSmartBin()
    {
        const QString text = search->text().trimmed();
        if (text.isEmpty()) return;
        bool ok = false;
        const QString n = QInputDialog::getText(owner->window(), "Save as Smart Bin", "Name:", QLineEdit::Normal, text, &ok).trimmed();
        if (!ok || n.isEmpty()) return;
        AppState::get().edit("Save as Smart Bin", [n, text](Project& p) {
            for (auto& sb : p.smartBins)
                if (qs(sb.first) == n) {
                    sb.second = text.toStdString();
                    return;
                }
            p.smartBins.push_back({n.toStdString(), text.toStdString()});
        });
        binKey = "smart:" + n;
        search->clear();
        refresh(true);
    }

    void rename(uint64_t id)
    {
        const MediaItem* m = AppState::get().project().findMedia(id);
        if (!m) return;
        bool ok = false;
        const QString n = QInputDialog::getText(owner->window(), "Rename", "Name:", QLineEdit::Normal, qs(m->name), &ok).trimmed();
        if (!ok || n.isEmpty() || n == qs(m->name)) return;
        AppState::get().edit("Rename Media", [id, n](Project& p) {
            if (MediaItem* mm = p.findMedia(id)) mm->name = n.toStdString();
        });
    }

    bool replace(uint64_t id, QString file = QString())
    {
        AppState& app = AppState::get();
        const MediaItem* m = app.project().findMedia(id);
        if (!m || m->generated) return false;
        if (file.isEmpty()) file = QFileDialog::getOpenFileName(owner->window(), "Replace Media", QFileInfo(qs(m->path)).absolutePath());
        if (file.isEmpty()) return false;
        std::string err;
        std::unique_ptr<MediaReader> r;
        try {
            r = app.mediaOpener ? app.mediaOpener(file.toStdString(), &err) : openMediaOrProject(file.toStdString(), &err);
        } catch (...) {
            err = "it could not be read";
        }
        if (!r) {
            QMessageBox::warning(owner->window(), "Replace Media", QString("%1 cannot be used: %2").arg(QFileInfo(file).fileName(), qs(err)));
            return false;
        }
        const MediaInfo info = r->info();
        const bool keepName = m->name != pathFileName(m->path);
        app.edit("Replace Media", [id, file, info, keepName](Project& p) {
            if (MediaItem* mm = p.findMedia(id)) {
                mm->path = file.toStdString();
                if (!keepName) mm->name = pathFileName(mm->path);
                const double subStart = mm->subclipStart, subLength = mm->subclipLength;
                mm->info = info;
                if (subLength > 0) mm->info.duration = std::max(0.0, std::min(subLength, info.duration - subStart));
                mm->offline = false;
                mm->veg.reset();
            }
        });
        icons.erase(id);
        requested.erase(id);
        refresh(true);
        return true;
    }

    void reread(const std::vector<uint64_t>& ids)
    {
        AppState& app = AppState::get();
        for (uint64_t id : ids) {
            const MediaItem* m = app.project().findMedia(id);
            if (!m || m->generated) continue;
            std::string err;
            std::unique_ptr<MediaReader> r;
            try {
                r = app.mediaOpener ? app.mediaOpener(m->readPath(), &err) : openMediaOrProject(m->readPath(), &err);
            } catch (...) {
            }
            const bool offline = !r;
            MediaInfo info = r ? r->info() : m->info;
            app.edit("Refresh Media", [id, offline, info](Project& p) {
                if (MediaItem* mm = p.findMedia(id)) {
                    mm->offline = offline;
                    if (!offline) {
                        const double len = mm->subclipLength > 0 ? std::max(0.0, std::min(mm->subclipLength, info.duration - mm->subclipStart)) : info.duration;
                        mm->info = info;
                        mm->info.duration = len;
                    }
                }
            });
            icons.erase(id);
            requested.erase(id);
        }
        refresh(true);
    }

    void selectTimelineEvents(const std::vector<uint64_t>& ids)
    {
        AppState& app = AppState::get();
        std::set<uint64_t> events;
        double first = -1;
        for (const auto& t : app.project().tracks)
            for (const auto& e : t.events)
                if (std::find(ids.begin(), ids.end(), e.mediaId) != ids.end()) {
                    events.insert(e.id);
                    if (first < 0 || e.start < first) first = e.start;
                }
        // VEGAS: Ctrl or Shift held adds them to the selection; the cursor goes to the first of them
        if (QApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::ShiftModifier))
            for (uint64_t id : app.selection()) events.insert(id);
        app.setSelection(events);
        if (first >= 0) app.setCursor(first);
        app.status(strfmt("%d event%s selected.", (int)events.size(), events.size() == 1 ? "" : "s"));
    }

    // VEGAS's Add At Project Frame Rate: an event at the cursor whose playback rate makes each media frame last one
    // project frame (a 120 fps clip in a 30 fps project plays at 0.25). Beyond 0.25 .. 4 the rest is a velocity.
    void addAtProjectFrameRate(const std::vector<uint64_t>& ids)
    {
        AppState& app = AppState::get();
        const double pf = app.project().settings.fps();
        for (uint64_t id : ids) {
            const MediaItem* m = app.project().findMedia(id);
            if (!m || !m->info.hasVideo || m->info.isStill || !(m->info.fps > 0) || !(pf > 0)) continue;
            const double want = pf / m->info.fps;
            const double rate = std::max(0.25, std::min(4.0, want));
            const double velocity = want / rate;
            const std::vector<uint64_t> made = app.addMediaToTimeline(id, app.cursor(), app.currentTrack);
            if (made.empty()) continue;
            app.edit("Add At Project Frame Rate", [&](Project& p) {
                for (uint64_t eid : made) {
                    Track* t = nullptr;
                    Event* e = p.findEvent(eid, &t);
                    if (!e) continue;
                    e->rate = rate;
                    e->length /= want;
                    if (t && t->type == TrackType::Video && std::fabs(velocity - 1.0) > 1e-6) {
                        Envelope env;
                        env.kind = "velocity";
                        EnvPoint pt;
                        pt.time = 0;
                        pt.value = velocity;
                        env.points.push_back(pt);
                        e->envelopes.push_back(env);
                    }
                }
            });
        }
    }

    // VEGAS's Swap Media: each media file with a same-named file (any extension) in the chosen folder is replaced by
    // it, as one undo step.
    int swapMedia(QString folder = QString())
    {
        if (folder.isEmpty()) folder = QFileDialog::getExistingDirectory(owner->window(), "Swap Media: the folder with the other files");
        if (folder.isEmpty()) return 0;
        AppState& app = AppState::get();
        QDir dir(folder);
        std::map<QString, QString> byBase;
        for (const QFileInfo& fi : dir.entryInfoList(QDir::Files)) byBase.emplace(fi.completeBaseName().toLower(), fi.absoluteFilePath());
        struct Swap { uint64_t id; QString file; MediaInfo info; };
        std::vector<Swap> swaps;
        for (const MediaItem& m : app.project().media) {
            if (m.generated || m.path.empty()) continue;
            auto it = byBase.find(QFileInfo(qs(m.path)).completeBaseName().toLower());
            if (it == byBase.end() || QFileInfo(it->second).absoluteFilePath() == QFileInfo(qs(m.path)).absoluteFilePath()) continue;
            std::string err;
            std::unique_ptr<MediaReader> r = app.mediaOpener ? app.mediaOpener(it->second.toStdString(), &err) : openMediaOrProject(it->second.toStdString(), &err);
            if (!r) continue;
            swaps.push_back({m.id, it->second, r->info()});
        }
        if (swaps.empty()) {
            app.status("No media file has a file of the same name in that folder.");
            return 0;
        }
        app.edit("Swap Media", [&](Project& p) {
            for (const Swap& sw : swaps)
                if (MediaItem* mm = p.findMedia(sw.id)) {
                    const bool keepName = mm->name != pathFileName(mm->path);
                    mm->path = sw.file.toStdString();
                    if (!keepName) mm->name = pathFileName(mm->path);
                    const double subStart = mm->subclipStart, subLength = mm->subclipLength;
                    mm->info = sw.info;
                    if (subLength > 0) mm->info.duration = std::max(0.0, std::min(subLength, sw.info.duration - subStart));
                    mm->offline = false;
                    mm->veg.reset();
                }
        });
        for (const Swap& sw : swaps) { icons.erase(sw.id); requested.erase(sw.id); }
        refresh(true);
        app.status(strfmt("%d media file%s swapped.", (int)swaps.size(), swaps.size() == 1 ? "" : "s"));
        return (int)swaps.size();
    }

    void previewSelected()
    {
        const std::vector<uint64_t> ids = selectedIds();
        if (ids.empty() || !previewMedia) return;
        const MediaItem* m = AppState::get().project().findMedia(ids.front());
        if (!m || m->generated) return;
        previewing = m->id;
        previewMedia(m->id, qs(m->path), true);
    }
    uint64_t previewing = 0;
    QToolButton* autoPreview = nullptr;

    void subclipFromTrimmer()
    {
        uint64_t media = 0;
        double in = 0, out = 0;
        if (!trimmerSelection || !trimmerSelection(media, in, out) || !(out > in) || !AppState::get().project().findMedia(media)) {
            QMessageBox::information(owner->window(), "Create Subclip", "Select a part of a project media file in the Trimmer first (I and O, or drag across its strip).");
            return;
        }
        const MediaItem* m = AppState::get().project().findMedia(media);
        bool ok = false;
        const QString n = QInputDialog::getText(owner->window(), "Create Subclip", "Name:", QLineEdit::Normal, qs(m->name) + " Subclip", &ok).trimmed();
        if (!ok) return;
        createSubclip(media, in, out, n.toStdString());
    }

    void showProperties(uint64_t id)
    {
        const MediaItem* m = AppState::get().project().findMedia(id);
        if (!m) return;
        MediaPropertiesDialog dlg(owner->window(), *m);
        dlg.exec();
    }

    // VEGAS's Create Video Proxy: a proxy of each selected video file (media.h), with a progress window that can stop
    // it. The Video Preview uses the proxies below Best quality.
    void createProxies(const std::vector<uint64_t> ids)
    {
        std::vector<std::string> paths;
        for (uint64_t id : ids) {
            const MediaItem* m = AppState::get().project().findMedia(id);
            if (m && !m->generated && !m->offline && !m->path.empty() && m->info.hasVideo && !m->info.isStill && !isProjectFile(m->path) &&
                std::find(paths.begin(), paths.end(), m->path) == paths.end())
                paths.push_back(m->path);
        }
        if (paths.empty()) {
            QMessageBox::information(owner->window(), "Create Video Proxy", "Select video files in Project Media first (pictures, sound and generated media have no proxies).");
            return;
        }
        QProgressDialog progress("Creating video proxy...", "Cancel", 0, 1000, owner->window());
        progress.setObjectName("proxyProgress");
        progress.setWindowTitle("Create Video Proxy");
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        QStringList failed;
        int made = 0;
        bool cancelled = false;
        for (size_t k = 0; k < paths.size() && !cancelled; ++k) {
            progress.setLabelText(QString("Creating video proxy %1 of %2: %3").arg(k + 1).arg(paths.size()).arg(qs(pathFileName(paths[k]))));
            std::string err;
            const bool ok = createVideoProxy(paths[k], [&](double f) {
                progress.setValue((int)std::lround((k + f) / paths.size() * 1000));
                QCoreApplication::processEvents();
                cancelled = progress.wasCanceled();
                return !cancelled;
            }, &err);
            if (ok) ++made;
            else if (!cancelled) failed << qs(pathFileName(paths[k])) + ": " + qs(err);
        }
        progress.setValue(1000);
        if (!failed.isEmpty())
            QMessageBox::warning(owner->window(), "Create Video Proxy", "These proxies could not be made:\n\n" + failed.join("\n"));
        if (made && onProxiesMade) onProxiesMade();
    }

    void matchProjectVideo(uint64_t id)
    {
        AppState& app = AppState::get();
        const MediaItem* m = app.project().findMedia(id);
        if (!m || !m->info.hasVideo || m->info.width <= 0 || m->info.height <= 0) return;
        const MediaInfo info = m->info;
        app.edit("Match Project Video Settings", [&](Project& p) {
            p.settings.width = info.width & ~1;
            p.settings.height = info.height & ~1;
            if (info.fps > 0.5 && !info.isStill) {
                const double f = info.fps;
                if (std::fabs(f - std::round(f)) < 1e-3) { p.settings.fpsNum = (int)std::lround(f); p.settings.fpsDen = 1; }
                else if (std::fabs(f * 1.001 - std::round(f * 1.001)) < 1e-3) { p.settings.fpsNum = (int)std::lround(f * 1.001) * 1000; p.settings.fpsDen = 1001; }
                else { p.settings.fpsNum = (int)std::lround(f * 1000); p.settings.fpsDen = 1000; }
            }
            p.settings.fieldOrder = info.fieldOrder == "upper" || info.fieldOrder == "lower" ? info.fieldOrder : "none";
        });
    }

    void mediaMenu(QWidget* on, const QPoint& global)
    {
        const std::vector<uint64_t> ids = selectedIds();
        if (ids.empty()) return;
        const MediaItem* first = AppState::get().project().findMedia(ids.front());
        if (!first) return;
        const uint64_t id = first->id;
        const bool file = !first->generated && !first->path.empty();
        QMenu menu(on);
        menu.setObjectName("mediaMenu");
        if (openInTrimmer) {
            QAction* t = menu.addAction("Open in Trimmer", [id] {
                const MediaItem* m = AppState::get().project().findMedia(id);
                if (m && !m->generated) openInTrimmer(m->id, qs(m->path));
            });
            t->setEnabled(file);
        }
        menu.addAction("Add to Timeline", [this, ids] { addToTimeline(ids); });
        menu.addAction("Select Timeline Events", [this, ids] { selectTimelineEvents(ids); });
        menu.addAction("Add At Project Frame Rate", [this, ids] { addAtProjectFrameRate(ids); })->setEnabled(first->info.hasVideo && !first->info.isStill && first->info.fps > 0);
        menu.addAction("Match Project Video Settings", [this, id] { matchProjectVideo(id); })->setEnabled(first->info.hasVideo && first->info.width > 0);
        menu.addAction("Explore Containing Folder", [first] { showInFolder(qs(first->path)); })->setEnabled(file);
        menu.addAction("Create Video Proxy", [this, ids] { createProxies(ids); })->setEnabled(file && first->info.hasVideo && !first->info.isStill);
        menu.addAction("Create Subclip from Trimmer Selection...", [this] { subclipFromTrimmer(); })->setEnabled((bool)trimmerSelection);
        menu.addSeparator();
        QAction* fromBin = menu.addAction("Remove from Current Bin", [this, ids] { moveToBin("all", ids); });
        fromBin->setEnabled(binKey.startsWith("bin:"));
        menu.addAction("Remove from Project", [this] { removeSelected(); });
        menu.addSeparator();
        menu.addAction("Rename", [this, id] { rename(id); });
        menu.addAction("Replace...", [this, id] { replace(id); })->setEnabled(!first->generated);
        menu.addAction("Refresh", [this, ids] { reread(ids); });
        menu.addAction("Swap Media...", [this] { swapMedia(); });
        menu.addSeparator();
        menu.addAction("Remove All Unused Media", [this] { removeUnusedMedia(); });
        menu.addSeparator();
        if (first->info.hasAudio && !first->generated) {
            menu.addAction("Beat Detection...", [this, id] { showBeatDetectionDialog(owner->window(), id); });
            menu.addAction("Tempo Detection...", [this, id] { showTempoDetectionDialog(owner->window(), id); });
            menu.addSeparator();
        }
        menu.addAction("Properties...", [this, id] { showProperties(id); });
        menu.exec(global);
    }

    void binMenu(const QPoint& global)
    {
        QMenu menu(bins);
        menu.setObjectName("binMenu");
        menu.addAction("Create New Bin", [this] { newBin(); });
        menu.addAction("Save as Smart Bin...", [this] { saveSmartBin(); })->setEnabled(!search->text().trimmed().isEmpty());
        menu.addSeparator();
        const bool own = binKey.startsWith("bin:") || binKey.startsWith("smart:");
        menu.addAction("Delete", [this] { deleteBin(); })->setEnabled(own);
        menu.addAction("Rename", [this] { renameBin(); })->setEnabled(own);
        menu.addSeparator();
        menu.addAction("Search Media Bins...", [this] { search->setFocus(); search->selectAll(); });
        menu.exec(global);
    }

    void importPaths(const QStringList& files)
    {
        AppState& app = AppState::get();
        QStringList failed;
        std::vector<uint64_t> added;
        QApplication::setOverrideCursor(Qt::WaitCursor);
        for (const QString& f : files) {
            std::string err;
            const uint64_t id = app.importMedia(f.toStdString(), &err);
            if (!id) failed << QString("%1: %2").arg(QFileInfo(f).fileName(), qs(err.empty() ? "it could not be read" : err));
            else added.push_back(id);
        }
        QApplication::restoreOverrideCursor();
        if (!added.empty() && binKey.startsWith("bin:")) moveToBin(binKey, added);
        if (!failed.isEmpty())
            QMessageBox::warning(owner->window(), "Import Media", QString("These files could not be imported:\n\n%1").arg(failed.join("\n")));
        else if (!files.isEmpty())
            app.status(QString("Imported %1 file%2.").arg(files.size()).arg(files.size() == 1 ? "" : "s").toStdString());
    }
};

MediaBin::MediaBin(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    Impl* impl = d.get();
    d->generatedIcon = generatedTile();
    d->offlineIcon = offlineTile();
    d->pendingIcon = QIcon(QPixmap::fromImage(blankTile()));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(4);
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(6);
    d->importButton = new QPushButton("Import Media...", this);
    d->importButton->setObjectName("importMedia");
    d->addButton = new QPushButton("Add to Timeline", this);
    d->addButton->setObjectName("addToTimeline");
    d->addButton->setToolTip("Put the selected media on the timeline at the cursor");
    d->removeButton = new QPushButton("Remove from Project", this);
    d->removeButton->setObjectName("removeMedia");
    d->removeButton->setToolTip("Take the selected media out of the project (only if no event uses it)");
    d->removeUnused = new QToolButton(this);
    d->removeUnused->setObjectName("removeUnusedMedia");
    d->removeUnused->setText("Remove Unused");
    d->removeUnused->setToolTip("Remove All Unused Media: everything no event uses");
    d->properties = new QToolButton(this);
    d->properties->setObjectName("mediaProperties");
    d->properties->setText("Properties...");
    d->properties->setToolTip("The selected media's properties: its streams, name, tags and comment");
    d->viewButton = new QToolButton(this);
    d->viewButton->setObjectName("mediaView");
    d->viewButton->setPopupMode(QToolButton::InstantPopup);
    d->viewButton->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0px; }");
    auto* views = new QMenu(d->viewButton);
    views->addAction("Detailed", [impl] { impl->setView(0); });
    views->addAction("List", [impl] { impl->setView(1); });
    views->addAction("Thumbnails", [impl] { impl->setView(2); });
    d->viewButton->setMenu(views);
    d->search = new QLineEdit(this);
    d->search->setObjectName("mediaSearch");
    d->search->setPlaceholderText("Search media (name, tags, comment, type)");
    d->search->setClearButtonEnabled(true);
    d->countLabel = new QLabel(this);
    d->countLabel->setObjectName("mediaCount");
    QToolButton* importMore = new QToolButton(this);
    importMore->setObjectName("importMediaMenu");
    importMore->setText(QString::fromUtf8("\u25BE"));
    importMore->setToolTip("Other ways to bring media in");
    importMore->setPopupMode(QToolButton::InstantPopup);
    {
        QMenu* m = new QMenu(importMore);
        m->addAction("Import Media...", [this] { importFiles(); });
        m->addAction("Media from Project...", [this] { if (QAction* a = window()->findChild<QAction*>("importFromProject")) a->trigger(); });
        m->addSeparator();
        for (const char* t : {"Capture Video...", "Extract Audio from CD...", "Get Media from the Web...", "VEGAS Capture..."}) {
            QAction* a = m->addAction(t);
            a->setEnabled(false);
            a->setToolTip("Not in Reno: capture hardware, CD reading and MAGIX's services");
        }
        importMore->setMenu(m);
    }
    QToolButton* swap = new QToolButton(this);
    swap->setObjectName("swapMedia");
    swap->setText("Swap...");
    swap->setToolTip("Swap Media: replace the media files by files of the same name in another folder");
    QToolButton* play = new QToolButton(this);
    play->setObjectName("mediaStartPreview");
    play->setText(QString::fromUtf8("\u25B6"));
    play->setToolTip("Start Preview: the selected media file plays (in the Trimmer)");
    QToolButton* stop = new QToolButton(this);
    stop->setObjectName("mediaStopPreview");
    stop->setText(QString::fromUtf8("\u25A0"));
    stop->setToolTip("Stop Preview");
    d->autoPreview = new QToolButton(this);
    d->autoPreview->setObjectName("mediaAutoPreview");
    d->autoPreview->setText("Auto");
    d->autoPreview->setCheckable(true);
    d->autoPreview->setChecked(QSettings().value("media/autoPreview", false).toBool());
    d->autoPreview->setToolTip("Auto Preview: media files play as they are clicked");
    QObject::connect(swap, &QToolButton::clicked, [impl] { impl->swapMedia(); });
    QObject::connect(play, &QToolButton::clicked, [impl] { impl->previewSelected(); });
    QObject::connect(stop, &QToolButton::clicked, [impl] { if (previewMedia) previewMedia(impl->previewing, QString(), false); });
    QObject::connect(d->autoPreview, &QToolButton::toggled, [](bool on) { QSettings().setValue("media/autoPreview", on); });
    for (QWidget* w : std::initializer_list<QWidget*>{d->importButton, importMore, d->addButton, d->removeButton, d->removeUnused, swap, d->properties, play, stop, d->autoPreview, d->viewButton})
        buttons->addWidget(w);
    buttons->addWidget(d->search, 1);
    buttons->addWidget(d->countLabel);
    root->addLayout(buttons);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName("mediaSplitter");
    d->bins = new BinTree(split);
    d->bins->setObjectName("mediaBins");
    d->bins->setContextMenuPolicy(Qt::CustomContextMenu);
    auto* lists = new QWidget(split);
    auto* lv = new QVBoxLayout(lists);
    lv->setContentsMargins(0, 0, 0, 0);
    d->tree = new MediaTree(lists);
    d->tree->setObjectName("mediaList");
    d->tree->setColumnCount(8);
    d->tree->setHeaderLabels(QStringList() << "Name" << "Type" << "Length" << "Format" << "Tags" << "Comment" << "Use Count" << "Path");
    d->tree->setRootIsDecorated(false);
    d->tree->setIconSize(kThumb);
    d->tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->tree->setUniformRowHeights(true);
    d->tree->setAlternatingRowColors(true);
    d->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    d->tree->setSortingEnabled(false);
    d->tree->header()->setStretchLastSection(true);
    d->tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    d->tree->setColumnWidth(0, 260);
    d->tree->setColumnWidth(1, 80);
    d->tree->setColumnWidth(2, 100);
    d->tree->setColumnWidth(3, 150);
    d->tiles = new MediaTiles(lists);
    d->tiles->setObjectName("mediaThumbnails");
    d->tiles->setContextMenuPolicy(Qt::CustomContextMenu);
    lv->addWidget(d->tree);
    lv->addWidget(d->tiles);
    split->addWidget(d->bins);
    split->addWidget(lists);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes(QList<int>() << 150 << 600);
    root->addWidget(split, 1);

    auto* tree = static_cast<MediaTree*>(d->tree);
    tree->onFiles = [impl](const QStringList& files) { impl->importPaths(files); };
    tree->onDelete = [impl] { impl->removeSelected(); };
    d->tiles->onFiles = [impl](const QStringList& files) { impl->importPaths(files); };
    d->tiles->onDelete = [impl] { impl->removeSelected(); };
    d->bins->onDropMedia = [impl](const QString& key, const std::vector<uint64_t>& ids) { impl->moveToBin(key, ids); };

    d->worker.reset(new ThumbWorker([impl](uint64_t id, const QImage& img) {
        const Alive alive = impl->alive;
        AppState::post([alive, impl, id, img] {
            if (alive->load()) impl->thumbArrived(id, img);
        });
    }));

    AppState::get().showMediaProperties = [impl](uint64_t id) { impl->showProperties(id); };
    QObject::connect(d->importButton, &QPushButton::clicked, [this] { importFiles(); });
    QObject::connect(d->addButton, &QPushButton::clicked, [impl] { impl->addToTimeline(impl->selectedIds()); });
    QObject::connect(d->removeButton, &QPushButton::clicked, [impl] { impl->removeSelected(); });
    QObject::connect(d->removeUnused, &QToolButton::clicked, [impl] { impl->removeUnusedMedia(); });
    QObject::connect(d->properties, &QToolButton::clicked, [impl] {
        const std::vector<uint64_t> ids = impl->selectedIds();
        if (ids.size() == 1) impl->showProperties(ids.front());
    });
    QObject::connect(d->search, &QLineEdit::textChanged, [impl](const QString&) { impl->refresh(); });
    QObject::connect(d->tree, &QTreeWidget::itemSelectionChanged, [impl] { impl->updateButtons(); });
    QObject::connect(d->tree, &QTreeWidget::itemSelectionChanged, [impl] { if (impl->autoPreview && impl->autoPreview->isChecked()) impl->previewSelected(); });
    QObject::connect(d->tiles, &QListWidget::itemSelectionChanged, [impl] { if (impl->autoPreview && impl->autoPreview->isChecked()) impl->previewSelected(); });
    QObject::connect(d->tiles, &QListWidget::itemSelectionChanged, [impl] { impl->updateButtons(); });
    auto activate = [impl](uint64_t id) {
        const MediaItem* m = AppState::get().project().findMedia(id);
        if (openInTrimmer && m && !m->generated && vprefs::on("prefs/dblClickTrimmer")) { openInTrimmer(id, qs(m->path)); return; }
        impl->addToTimeline(std::vector<uint64_t>(1, id));
    };
    QObject::connect(d->tree, &QTreeWidget::itemDoubleClicked, [activate](QTreeWidgetItem* it, int) { activate(it->data(0, kRoleId).toULongLong()); });
    QObject::connect(d->tiles, &QListWidget::itemDoubleClicked, [activate](QListWidgetItem* it) { activate(it->data(kRoleId).toULongLong()); });
    QObject::connect(d->tree, &QWidget::customContextMenuRequested, [impl](const QPoint& pos) {
        if (!impl->tree->itemAt(pos)) return;
        impl->mediaMenu(impl->tree, impl->tree->viewport()->mapToGlobal(pos));
    });
    QObject::connect(d->tiles, &QWidget::customContextMenuRequested, [impl](const QPoint& pos) {
        if (!impl->tiles->itemAt(pos)) return;
        impl->mediaMenu(impl->tiles, impl->tiles->viewport()->mapToGlobal(pos));
    });
    QObject::connect(d->bins, &QTreeWidget::currentItemChanged, [impl](QTreeWidgetItem* it, QTreeWidgetItem*) {
        if (!it) return;
        impl->binKey = it->data(0, kRoleBinKey).toString();
        impl->refresh();
    });
    QObject::connect(d->bins, &QWidget::customContextMenuRequested, [impl](const QPoint& pos) {
        if (QTreeWidgetItem* it = impl->bins->itemAt(pos)) impl->bins->setCurrentItem(it);
        impl->binMenu(impl->bins->viewport()->mapToGlobal(pos));
    });

    d->listener.reset(new ListenerHandle([impl, serial = std::make_shared<uint64_t>(AppState::get().projectSerial())](int changes) {
        if ((changes & kFile) && AppState::get().projectSerial() != *serial) {
            *serial = AppState::get().projectSerial();
            // another project: the thumbnails are of the last one's media (whose ids the new one may reuse)
            impl->icons.clear();
            impl->requested.clear();
            impl->refresh(true);
        } else if (changes & kProject) {
            impl->refresh();
        }
        if (changes & kPrefs) impl->applyLook();
    }));
    d->setView(QSettings().value("media/view", 0).toInt());
}

MediaBin::~MediaBin() {}

void MediaBin::setView(int view) { d->setView(view); }
int MediaBin::view() const { return d->view; }
void MediaBin::showBin(const QString& key)
{
    d->binKey = key;
    d->refresh(true);
}
QString MediaBin::currentBin() const { return d->binKey; }
void MediaBin::setSearch(const QString& text) { d->search->setText(text); }
int MediaBin::removeUnusedMedia() { return d->removeUnusedMedia(); }
void MediaBin::newBin() { d->newBin(); }
void MediaBin::moveToBin(const QString& key, const std::vector<uint64_t>& ids) { d->moveToBin(key, ids); }
void MediaBin::saveSmartBin(const QString& name)
{
    const QString text = d->search->text().trimmed();
    if (text.isEmpty() || name.trimmed().isEmpty()) return;
    const QString n = name.trimmed();
    AppState::get().edit("Save as Smart Bin", [n, text](Project& p) { p.smartBins.push_back({n.toStdString(), text.toStdString()}); });
    d->binKey = "smart:" + n;
    d->search->clear();
    d->refresh(true);
}
void MediaBin::deleteCurrentBin() { d->deleteBin(); }
bool MediaBin::replaceMedia(uint64_t id, const QString& file) { return d->replace(id, file); }
void MediaBin::refreshMedia(const std::vector<uint64_t>& ids) { d->reread(ids); }
void MediaBin::selectTimelineEvents(const std::vector<uint64_t>& ids) { d->selectTimelineEvents(ids); }
std::vector<uint64_t> MediaBin::shownMedia() const
{
    std::vector<uint64_t> ids;
    for (int k = 0; k < d->tree->topLevelItemCount(); ++k) ids.push_back(d->tree->topLevelItem(k)->data(0, kRoleId).toULongLong());
    return ids;
}

void MediaBin::selectMedia(const std::vector<uint64_t>& ids)
{
    bool hidden = false;
    for (uint64_t id : ids)
        if (!d->itemFor(id)) hidden = true;
    if (hidden) {
        d->search->setText(QString());
        d->binKey = "all";
        d->refresh(true);
    }
    d->tree->clearSelection();
    d->tiles->clearSelection();
    for (uint64_t id : ids) {
        if (QTreeWidgetItem* it = d->itemFor(id)) {
            it->setSelected(true);
            d->tree->scrollToItem(it);
        }
        for (int k = 0; k < d->tiles->count(); ++k)
            if (d->tiles->item(k)->data(kRoleId).toULongLong() == id) d->tiles->item(k)->setSelected(true);
    }
}

void MediaBin::addAtProjectFrameRate(const std::vector<uint64_t>& ids) { d->addAtProjectFrameRate(ids); }
int MediaBin::swapMedia(const QString& folder) { return folder.isEmpty() ? 0 : d->swapMedia(folder); }

void MediaBin::importFiles()
{
    QSettings settings;
    const QString dir = settings.value("media/importFolder", QDir::homePath()).toString();
    const QString video = mediaGlobs(MediaKind::Video).join(' ');
    const QString audio = mediaGlobs(MediaKind::Audio).join(' ');
    const QString pics = mediaGlobs(MediaKind::Picture).join(' ');
    const QString projects = "*.veg *.reno";
    const QString filters = QString("All Media Files (%1 %2 %3 %4);;Video Files (%1);;Audio Files (%2);;Picture Files (%3);;Projects (%4);;All Files (*)")
                                .arg(video, audio, pics, projects);
    const QStringList files = QFileDialog::getOpenFileNames(window(), "Import Media", dir, filters);
    if (files.isEmpty()) return;
    settings.setValue("media/importFolder", QFileInfo(files.first()).absolutePath());
    d->importPaths(files);
}

void MediaBin::importImageSequence()
{
    QSettings settings;
    const QString dir = settings.value("media/importFolder", QDir::homePath()).toString();
    const QString file = QFileDialog::getOpenFileName(window(), "Import Image Sequence - the first picture", dir,
                                                      QString("Picture Files (%1);;All Files (*)").arg(mediaGlobs(MediaKind::Picture).join(' ')));
    if (file.isEmpty()) return;
    settings.setValue("media/importFolder", QFileInfo(file).absolutePath());
    const std::string first = file.toStdString();
    const int count = imageSequenceLength(first);
    if (count < 2) {
        QMessageBox::information(window(), "Import Image Sequence",
                                 QFileInfo(file).fileName() + (count ? " is not followed by more numbered pictures (name_0001.png, name_0002.png ...)."
                                                                     : " has no number at the end of its name (name_0001.png, name_0002.png ...)."));
        return;
    }
    bool ok = false;
    const double fps = QInputDialog::getDouble(window(), "Import Image Sequence",
                                               QString("%1 pictures from %2.\nFrame rate (frames a second):").arg(count).arg(QFileInfo(file).fileName()),
                                               AppState::get().project().settings.fps(), 0.01, 1000, 3, &ok);
    if (!ok) return;
    std::string err;
    const uint64_t id = AppState::get().importImageSequence(first, fps, &err);
    if (!id) QMessageBox::warning(window(), "Import Image Sequence", QString::fromStdString(err));
}

}  // namespace app
}  // namespace reno
