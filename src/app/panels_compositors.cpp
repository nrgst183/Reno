// VEGAS's Compositors window (see panels.h).
#include "panels_internal.h"
#include "onshow.h"
#include "fxlibrary.h"
#include "fxthumbs.h"
#include "lazyrefresh.h"
#include "strconv.h"
#include "../engine/catalog.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace reno {
namespace app {

namespace {

const int kRoleData = Qt::UserRole;         // what a drag carries ("identifier\nlabel")
const int kRolePlugin = Qt::UserRole + 1;   // a plug-in row: its identifier
const int kRoleFolder = Qt::UserRole + 2;   // a folder row: its key
const QSize kPresetThumb(108, 61);

// VEGAS's own compositing plug-ins (its "VEGAS.Compositors" group).
bool vegasCompositor(const std::string& id)
{
    return id == "com.vegascreativesoftware:bumpmap" || id == "com.vegascreativesoftware:displacementmap" ||
           id == "com.vegascreativesoftware:heightmap" || id == "com.vegascreativesoftware:layerdimensionality";
}

bool fromVegas(const std::string& id)
{
    return id.rfind("com.vegascreativesoftware", 0) == 0 || id.rfind("com.sonycreativesoftware", 0) == 0 || id.rfind("com.magix", 0) == 0 ||
           id.rfind("de.magix", 0) == 0;
}

QIcon folderIcon()
{
    static QIcon ic;
    if (ic.isNull()) {
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath back;
        back.moveTo(1.5, 3.5);
        back.lineTo(6.0, 3.5);
        back.lineTo(7.5, 5.0);
        back.lineTo(14.5, 5.0);
        back.lineTo(14.5, 13.5);
        back.lineTo(1.5, 13.5);
        back.closeSubpath();
        p.setPen(QPen(QColor(150, 115, 30), 1));
        p.setBrush(QColor(222, 178, 70));
        p.drawPath(back);
        p.setBrush(QColor(247, 211, 110));
        p.drawRect(QRectF(1.5, 6.5, 13.0, 7.0));
        ic = QIcon(pm);
    }
    return ic;
}

// VEGAS's blue "FX" in front of each plug-in.
QIcon fxIcon()
{
    static QIcon ic;
    if (ic.isNull()) {
        QPixmap pm(16, 16);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::TextAntialiasing);
        QFont f;
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(70, 160, 240));
        p.drawText(QRect(0, 0, 16, 16), Qt::AlignCenter, "FX");
        ic = QIcon(pm);
    }
    return ic;
}

QIcon magnifier()
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(220, 220, 220), 1.6));
    p.drawEllipse(QRectF(2, 2, 8, 8));
    p.drawLine(QPointF(9, 9), QPointF(13.5, 13.5));
    return QIcon(pm);
}

class CompositorTree : public QTreeWidget {
public:
    explicit CompositorTree(QWidget* parent) : QTreeWidget(parent)
    {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
    }

protected:
    QStringList mimeTypes() const override { return QStringList() << kMimeVideoFx; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        for (QTreeWidgetItem* it : items) {
            if (it->data(0, kRolePlugin).toString().isEmpty()) continue;
            auto* m = new QMimeData();
            m->setData(kMimeVideoFx, it->data(0, kRoleData).toString().toUtf8());
            return m;
        }
        return nullptr;
    }
};

class PresetThumbs : public QListWidget {
public:
    explicit PresetThumbs(QWidget* parent) : QListWidget(parent)
    {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
        setViewMode(QListView::IconMode);
        setIconSize(kPresetThumb);
        setGridSize(QSize(kPresetThumb.width() + 16, kPresetThumb.height() + 48));
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setWordWrap(true);
        setUniformItemSizes(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
        setSpacing(2);
    }

protected:
    QStringList mimeTypes() const override { return QStringList() << kMimeVideoFx; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty()) return nullptr;
        auto* m = new QMimeData();
        m->setData(kMimeVideoFx, items[0]->data(Qt::UserRole).toString().toUtf8());
        return m;
    }
};

struct Row {
    std::string id;
    QString name, label;
    bool vegas = false, builtin = false;
};

}  // namespace

struct CompositorsWindow::Impl {
    CompositorsWindow* owner = nullptr;
    QLineEdit* search = nullptr;
    CompositorTree* tree = nullptr;
    PresetThumbs* presets = nullptr;
    QLabel* status = nullptr;
    QTimer* thumbTimer = nullptr;
    std::unique_ptr<ListenerHandle> listener;
    std::vector<Row> rows;
    std::string presetPlugin;
    QString presetLabel;
    std::vector<OfxPreset> presetList;
    int nextThumb = 0;
    bool stale = true;

    // The compositors in the catalog: VEGAS's own, and any other plug-in grouped as a compositor.
    std::vector<Row> catalogRows() const
    {
        std::vector<Row> out;
        for (const VideoFxEntry& e : Catalog::get().video()) {
            if (!e.ok) continue;
            const bool vegas = vegasCompositor(e.id);
            if (!vegas && !QString::fromStdString(e.group).contains("Compositor", Qt::CaseInsensitive)) continue;
            if (std::any_of(out.begin(), out.end(), [&](const Row& r) { return r.id == e.id; })) continue;
            Row r;
            r.id = e.id;
            r.label = qs(e.label.empty() ? e.id : e.label);
            r.name = r.label;
            if (r.name.startsWith("VEGAS ")) r.name = r.name.mid(6);
            r.vegas = fromVegas(e.id);
            r.builtin = e.builtin;
            out.push_back(r);
        }
        std::sort(out.begin(), out.end(), [](const Row& a, const Row& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
        return out;
    }

    void fill()
    {
        stale = false;
        rows = catalogRows();
        std::set<QString> open;
        QString current = tree->currentItem() ? tree->currentItem()->text(0) : QString();
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->isExpanded()) open.insert((*it)->data(0, kRoleFolder).toString());
        const bool first = tree->topLevelItemCount() == 0;
        const QStringList words = search->text().split(' ', Qt::SkipEmptyParts);
        auto matches = [&](const Row& r) {
            for (const QString& w : words)
                if (!r.label.contains(w, Qt::CaseInsensitive)) return false;
            return true;
        };
        tree->clear();
        const struct { const char* key; const char* name; } folders[] = {
            {"all", "All"}, {"ofx", "OFX"}, {"float", "32-bit floating point"}, {"gpu", "GPU Accelerated"}, {"vegas", "VEGAS"}, {"thirdparty", "Third Party"}};
        QTreeWidgetItem* pickItem = nullptr;
        for (const auto& f : folders) {
            auto* folder = new QTreeWidgetItem(tree, QStringList() << f.name);
            folder->setIcon(0, folderIcon());
            folder->setData(0, kRoleFolder, QString(f.key));
            folder->setFlags(Qt::ItemIsEnabled);
            const QString key = f.key;
            for (const Row& r : rows) {
                if (!matches(r)) continue;
                // (VEGAS's own are OFX, 32-bit floating point and GPU accelerated; others are listed as OFX only)
                if ((key == "float" || key == "gpu") && !r.vegas) continue;
                if (key == "vegas" && !r.vegas) continue;
                if (key == "thirdparty" && r.vegas) continue;
                auto* it = new QTreeWidgetItem(folder, QStringList() << r.name);
                it->setIcon(0, fxIcon());
                it->setData(0, kRolePlugin, qs(r.id));
                it->setData(0, kRoleData, qs(r.id) + "\n" + r.label);
                it->setToolTip(0, r.label + "\n" + qs(r.id));
                it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
                if (!pickItem && !current.isEmpty() && r.name == current) pickItem = it;
            }
            folder->setExpanded(!words.isEmpty() || (first ? key == "all" : open.count(key) > 0));
        }
        if (pickItem) tree->setCurrentItem(pickItem);
        else describe(nullptr);
    }

    void ensureFilled()
    {
        if (stale) fill();
    }

    const Row* rowOf(const std::string& id) const
    {
        for (const Row& r : rows)
            if (r.id == id) return &r;
        return nullptr;
    }

    // The line under the window, as VEGAS's: the plug-in's name, what it is, and where it comes from.
    void describe(QTreeWidgetItem* it)
    {
        const Row* r = it ? rowOf(it->data(0, kRolePlugin).toString().toStdString()) : nullptr;
        if (!r) {
            status->setText(QString("%1 compositor%2").arg(rows.size()).arg(rows.size() == 1 ? "" : "s"));
            return;
        }
        QString what = r->vegas ? QString("OFX, 32-bit floating point, GPU Accelerated, Grouping VEGAS.Compositors, Version 1.0") : QString("OFX");
        QString from = r->builtin ? QString("Reno's built-in stand-in for VEGAS's plug-in")
                                  : r->vegas ? QString("From Magix Computer Products Intl. Co.") : qs(r->id);
        status->setText(r->label + ": " + what + "\nDescription: " + from);
    }

    std::string presetData(int index) const
    {
        if (index < 0) return presetPlugin + "\n" + presetLabel.toStdString();
        FxPackage p;
        p.name = presetLabel.toStdString();
        p.kind = PackageKind::VideoFx;
        FxSlot s;
        s.kind = "ofx";
        s.pluginId = presetPlugin;
        s.name = presetLabel.toStdString();
        if ((size_t)index < presetList.size()) s.state = presetToState(presetList[(size_t)index]);
        p.chain.push_back(s);
        Json j;
        Json::parse(packageToText(p), j);
        return j.dump();
    }

    void fillPresets(QTreeWidgetItem* it)
    {
        const std::string id = it ? it->data(0, kRolePlugin).toString().toStdString() : std::string();
        const Row* r = rowOf(id);
        if (id == presetPlugin && presets->count()) return;
        presetPlugin = r ? id : std::string();
        presetLabel = r ? r->label : QString();
        presets->clear();
        presetList.clear();
        thumbTimer->stop();
        if (!r) return;
        presetList = presetsFor(id, "Filter");
        QPixmap blank(kPresetThumb);
        blank.fill(QColor(40, 40, 44));
        for (int k = -1; k < (int)presetList.size(); ++k) {
            auto* item = new QListWidgetItem(QIcon(blank), k < 0 ? QString("(Default)") : qs(presetList[(size_t)k].name), presets);
            item->setData(Qt::UserRole, qs(presetData(k)));
            item->setData(Qt::UserRole + 1, k);
            item->setToolTip(item->text());
            if (k < 0) item->setForeground(QColor(140, 140, 140));
        }
        nextThumb = 0;
        thumbTimer->start(0);
    }

    void drawNextThumb()
    {
        if (nextThumb >= presets->count()) { thumbTimer->stop(); return; }
        QListWidgetItem* item = presets->item(nextThumb++);
        const int k = item->data(Qt::UserRole + 1).toInt();
        const Json state = k >= 0 && (size_t)k < presetList.size() ? presetToState(presetList[(size_t)k]) : Json();
        const QImage img = fxThumbnail(presetPlugin, "Filter", state, kPresetThumb, 0.5);
        if (!img.isNull()) item->setIcon(QIcon(QPixmap::fromImage(img)));
    }
};

CompositorsWindow::CompositorsWindow(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    Impl* impl = d.get();
    setObjectName("compositors");
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 0);
    root->setSpacing(4);

    auto* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName("compositorSplit");
    split->setChildrenCollapsible(false);
    auto* left = new QWidget(split);
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(4);
    d->search = new QLineEdit(left);
    d->search->setObjectName("searchBox");
    d->search->setPlaceholderText("Search plug-ins");
    d->search->setClearButtonEnabled(true);
    d->search->addAction(magnifier(), QLineEdit::LeadingPosition);
    lv->addWidget(d->search);
    d->tree = new CompositorTree(left);
    d->tree->setObjectName("compositorTree");
    d->tree->setHeaderHidden(true);
    d->tree->setSelectionMode(QAbstractItemView::SingleSelection);
    d->tree->setIndentation(16);
    d->tree->setAnimated(false);
    d->tree->setIconSize(QSize(16, 16));
    d->tree->setRootIsDecorated(true);
    lv->addWidget(d->tree, 1);
    auto* right = new QWidget(split);
    auto* rv = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    rv->setSpacing(2);
    auto* title = new QLabel("Preset:", right);
    title->setObjectName("compositorPresetTitle");
    QFont tf = title->font();
    tf.setBold(true);
    title->setFont(tf);
    title->setContentsMargins(2, 3, 0, 3);
    rv->addWidget(title);
    d->presets = new PresetThumbs(right);
    d->presets->setObjectName("compositorPresets");
    rv->addWidget(d->presets, 1);
    split->addWidget(left);
    split->addWidget(right);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({185, 300});
    left->setMinimumWidth(120);
    root->addWidget(split, 1);

    d->status = new QLabel(this);
    d->status->setObjectName("compositorStatus");
    d->status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);     // (cut off at the window's edge, as VEGAS's)
    QPalette sp = palette();
    sp.setColor(QPalette::WindowText, QColor(200, 200, 200));
    d->status->setPalette(sp);
    root->addWidget(d->status);

    d->thumbTimer = new QTimer(this);
    QObject::connect(d->thumbTimer, &QTimer::timeout, this, [impl] { impl->drawNextThumb(); });
    QObject::connect(d->search, &QLineEdit::textChanged, this, [impl](const QString&) { impl->fill(); });
    QObject::connect(d->tree, &QTreeWidget::currentItemChanged, this, [impl](QTreeWidgetItem* it, QTreeWidgetItem*) {
        impl->fillPresets(it);
        impl->describe(it);
    });
    QObject::connect(d->tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it, int) {
        if (it && !it->data(0, kRolePlugin).toString().isEmpty() && onActivated) onActivated(it->data(0, kRoleData).toString().toStdString());
    });
    QObject::connect(d->presets, &QListWidget::itemActivated, this, [this](QListWidgetItem* it) {
        if (it && onActivated) onActivated(it->data(Qt::UserRole).toString().toStdString());
    });
    d->listener.reset(new ListenerHandle([impl](int changes) {
        if (!(changes & kCatalog)) return;
        if (impl->owner->isVisible()) impl->fill();
        else impl->stale = true;
    }));
    callWhenShown(this, [impl] { impl->ensureFilled(); });
}

CompositorsWindow::~CompositorsWindow() {}

void CompositorsWindow::setSearch(const QString& text)
{
    d->ensureFilled();
    d->search->setText(text);
}

QStringList CompositorsWindow::listed(const QString& folder) const
{
    d->ensureFilled();
    QStringList out;
    for (int k = 0; k < d->tree->topLevelItemCount(); ++k) {
        QTreeWidgetItem* f = d->tree->topLevelItem(k);
        if (f->data(0, kRoleFolder).toString() != folder) continue;
        for (int c = 0; c < f->childCount(); ++c) out << f->child(c)->text(0);
    }
    return out;
}

bool CompositorsWindow::pick(const QString& name)
{
    d->ensureFilled();
    for (QTreeWidgetItemIterator it(d->tree); *it; ++it)
        if (!(*it)->data(0, kRolePlugin).toString().isEmpty() && (*it)->text(0) == name) {
            d->tree->setCurrentItem(*it);
            return true;
        }
    return false;
}

int CompositorsWindow::presetCount() const { return d->presets->count(); }
QString CompositorsWindow::description() const { return d->status->text(); }

}  // namespace app
}  // namespace reno
