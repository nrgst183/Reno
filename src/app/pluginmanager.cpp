// The Plug-In Manager: see pluginmanager.h.
#include "pluginmanager.h"
#include "toolwin.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <functional>
#include <map>
#include <set>

namespace reno {
namespace app {

using namespace tw;

namespace {

const int kRoleKind = Qt::UserRole;
const int kRoleData = Qt::UserRole + 1;
const int kRoleKey = Qt::UserRole + 2;
const int kRoleWorks = Qt::UserRole + 3;

const char* mimeOf(FxBrowser::Kind k)
{
    switch (k) {
    case FxBrowser::VideoFx: return kMimeVideoFx;
    case FxBrowser::AudioFx: return kMimeAudioFx;
    case FxBrowser::Generators: return kMimeGenerator;
    default: return kMimeTransition;
    }
}

class PlugTree : public QTreeWidget {
public:
    explicit PlugTree(QWidget* parent) : QTreeWidget(parent)
    {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
    }

protected:
    QStringList mimeTypes() const override { return QStringList() << kMimeVideoFx << kMimeAudioFx << kMimeGenerator << kMimeTransition; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        for (QTreeWidgetItem* it : items) {
            const int k = it->data(0, kRoleKind).toInt();
            if (k <= 0 || !it->data(0, kRoleWorks).toBool()) continue;
            auto* m = new QMimeData();
            m->setData(mimeOf((FxBrowser::Kind)(k - 1)), it->data(0, kRoleData).toString().toUtf8());
            return m;
        }
        return nullptr;
    }
};

struct Plug {
    FxBrowser::Kind kind;
    QString name, group, format, file, data, key, status;
    bool works = true;
};

}  // namespace

struct PluginManager::Impl {
    PluginManager* q = nullptr;
    QLineEdit* search = nullptr;
    PlugTree* tree = nullptr;
    QLabel* summary = nullptr;
    QStringList favs;
    QString filter;
    std::unique_ptr<Listener> listener;

    std::vector<Plug> plugs() const
    {
        std::vector<Plug> out;
        for (const VideoFxEntry& e : Catalog::get().video()) {
            Plug p;
            p.kind = e.transition ? FxBrowser::Transitions : e.generator ? FxBrowser::Generators : FxBrowser::VideoFx;
            if (!e.filter && !e.transition && !e.generator) p.kind = FxBrowser::VideoFx;
            p.name = qs(e.label.empty() ? e.id : e.label);
            p.group = e.group.empty() ? QString("Other") : qs(e.group);
            p.format = "OpenFX";
            p.file = qs(e.path);
            p.data = qs(e.id + "\n" + (e.label.empty() ? e.id : e.label));
            p.key = "video:" + qs(e.id);
            p.works = e.ok;
            p.status = e.ok ? QString("Works") : QString("Does not work: %1").arg(qs(e.error));
            out.push_back(p);
            if (e.filter && (e.generator || e.transition)) {
                Plug f = p;
                f.kind = FxBrowser::VideoFx;
                out.push_back(f);
            }
        }
        for (const AudioFxEntry& e : Catalog::get().audio()) {
            Plug p;
            p.kind = FxBrowser::AudioFx;
            p.name = qs(e.desc.name);
            p.group = e.desc.vendor.empty() ? QString("Unknown Vendor") : qs(e.desc.vendor);
            p.format = e.desc.format == "DX" ? QString("DirectX") : e.desc.format == "BUILTIN" ? QString("Built-in") : qs(e.desc.format == "VST2" ? std::string("VST") : e.desc.format);
            p.file = qs(e.desc.path);
            p.data = qs(e.desc.toJson().dump());
            p.key = "audio:" + qs(e.desc.key());
            p.works = e.ok;
            p.status = e.ok ? QString("Works") : QString("Does not work: %1").arg(qs(e.error));
            if (e.desc.bridged32) p.status += qs(audioPluginKindNote(e.desc));
            out.push_back(p);
        }
        return out;
    }

    bool matches(const Plug& p) const
    {
        for (const QString& w : filter.split(' ', Qt::SkipEmptyParts))
            if (!p.name.contains(w, Qt::CaseInsensitive) && !p.group.contains(w, Qt::CaseInsensitive) && !p.format.contains(w, Qt::CaseInsensitive))
                return false;
        return true;
    }

    QTreeWidgetItem* folder(QTreeWidgetItem* parent, const QString& name)
    {
        for (int k = 0; parent && k < parent->childCount(); ++k)
            if (parent->child(k)->text(0) == name && parent->child(k)->data(0, kRoleKind).toInt() == 0) return parent->child(k);
        auto* f = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
        f->setText(0, name);
        f->setFlags(Qt::ItemIsEnabled);
        QFont font = f->font(0);
        font.setBold(true);
        f->setFont(0, font);
        return f;
    }

    void addLeaf(QTreeWidgetItem* parent, const Plug& p)
    {
        auto* it = new QTreeWidgetItem(parent);
        const bool fav = favs.contains(p.key);
        it->setText(0, (fav ? QString::fromUtf8("★ ") : QString()) + p.name);
        it->setText(1, p.works ? QString("Works") : QString("Does not work"));
        it->setText(2, p.format);
        it->setText(3, p.file);
        it->setToolTip(0, p.status + (p.file.isEmpty() ? QString() : "\n" + p.file));
        it->setToolTip(1, p.status);
        it->setData(0, kRoleKind, (int)p.kind + 1);
        it->setData(0, kRoleData, p.data);
        it->setData(0, kRoleKey, p.key);
        it->setData(0, kRoleWorks, p.works);
        Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        if (p.works) f |= Qt::ItemIsDragEnabled;
        it->setFlags(f);
        if (!p.works)
            for (int c = 0; c < 4; ++c) it->setForeground(c, QColor(230, 110, 100));
    }

    void fill()
    {
        std::map<QString, bool> open;
        std::function<void(QTreeWidgetItem*, const QString&)> remember = [&](QTreeWidgetItem* it, const QString& path) {
            for (int k = 0; k < it->childCount(); ++k) {
                QTreeWidgetItem* c = it->child(k);
                if (c->data(0, kRoleKind).toInt() != 0) continue;
                const QString p = path.isEmpty() ? c->text(0) : path + "/" + c->text(0);
                open[p] = c->isExpanded();
                remember(c, p);
            }
        };
        remember(tree->invisibleRootItem(), QString());
        tree->setUpdatesEnabled(false);
        tree->clear();
        const std::vector<Plug> all = plugs();
        QTreeWidgetItem* favorites = folder(nullptr, "Favorites");
        QTreeWidgetItem* video = folder(nullptr, "Video FX");
        QTreeWidgetItem* trans = folder(nullptr, "Transitions");
        QTreeWidgetItem* gens = folder(nullptr, "Media Generators");
        QTreeWidgetItem* audio = folder(nullptr, "Audio FX");
        QTreeWidgetItem* broken = folder(nullptr, "Not Working");
        int shown = 0, works = 0;
        std::vector<const Plug*> order;
        for (const Plug& p : all) order.push_back(&p);
        std::stable_sort(order.begin(), order.end(), [](const Plug* a, const Plug* b) {
            const int g = a->group.compare(b->group, Qt::CaseInsensitive);
            return g != 0 ? g < 0 : a->name.compare(b->name, Qt::CaseInsensitive) < 0;
        });
        std::set<QString> listedFav;
        for (const Plug* p : order) {
            if (p->works) ++works;
            if (!matches(*p)) continue;
            ++shown;
            QTreeWidgetItem* top = p->kind == FxBrowser::VideoFx ? video : p->kind == FxBrowser::Transitions ? trans : p->kind == FxBrowser::Generators ? gens : audio;
            QTreeWidgetItem* sub = p->kind == FxBrowser::AudioFx ? folder(top, p->format) : folder(top, p->group);
            addLeaf(sub, *p);
            if (favs.contains(p->key) && !listedFav.count(p->key)) {
                listedFav.insert(p->key);
                addLeaf(favorites, *p);
            }
            if (!p->works) addLeaf(broken, *p);
        }
        for (QTreeWidgetItem* f : {video, trans, gens, audio, broken})
            if (f->childCount() == 0) delete f;
        if (favorites->childCount() == 0) {
            auto* hint = new QTreeWidgetItem(favorites);
            hint->setText(0, "(right-click a plug-in to add it here)");
            hint->setFlags(Qt::ItemIsEnabled);
            hint->setForeground(0, QColor(140, 140, 140));
        }
        std::function<void(QTreeWidgetItem*, const QString&)> restore = [&](QTreeWidgetItem* it, const QString& path) {
            for (int k = 0; k < it->childCount(); ++k) {
                QTreeWidgetItem* c = it->child(k);
                if (c->data(0, kRoleKind).toInt() != 0) continue;
                const QString p = path.isEmpty() ? c->text(0) : path + "/" + c->text(0);
                auto o = open.find(p);
                c->setExpanded(!filter.isEmpty() || (o == open.end() ? path.isEmpty() : o->second));
                restore(c, p);
            }
        };
        restore(tree->invisibleRootItem(), QString());
        tree->setUpdatesEnabled(true);
        summary->setText(QString("%1 plug-ins, %2 working%3").arg(all.size()).arg(works).arg(filter.isEmpty() ? QString() : QString("; %1 shown").arg(shown)));
    }

    QTreeWidgetItem* find(const QString& path) const
    {
        QTreeWidgetItem* it = tree->invisibleRootItem();
        for (const QString& part : path.split('/', Qt::SkipEmptyParts)) {
            QTreeWidgetItem* next = nullptr;
            for (int k = 0; k < it->childCount(); ++k)
                if (it->child(k)->text(0) == part && it->child(k)->data(0, kRoleKind).toInt() == 0) next = it->child(k);
            if (!next) return nullptr;
            it = next;
        }
        return it;
    }

    void leaves(QTreeWidgetItem* it, QStringList& out) const
    {
        for (int k = 0; k < it->childCount(); ++k) {
            QTreeWidgetItem* c = it->child(k);
            if (c->data(0, kRoleKind).toInt() > 0) out << c->data(0, kRoleKey).toString();
            else leaves(c, out);
        }
    }

    void activate(QTreeWidgetItem* it)
    {
        if (!it || it->data(0, kRoleKind).toInt() <= 0) return;
        if (!it->data(0, kRoleWorks).toBool()) return;
        if (q->onActivated) q->onActivated((FxBrowser::Kind)(it->data(0, kRoleKind).toInt() - 1), ss(it->data(0, kRoleData).toString()));
    }
};

PluginManager::PluginManager(QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->q = this;
    setObjectName("pluginManager");
    Impl* impl = d.get();
    d->favs = QSettings().value("pluginManager/favorites").toStringList();
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(4);
    auto* top = new QHBoxLayout();
    d->search = new QLineEdit(this);
    d->search->setObjectName("pluginSearch");
    d->search->setPlaceholderText("Find plug-ins");
    d->search->setClearButtonEnabled(true);
    QPushButton* rescan = new QPushButton("Look for New Plug-Ins", this);
    rescan->setObjectName("pluginRescan");
    top->addWidget(d->search, 1);
    top->addWidget(rescan);
    v->addLayout(top);
    d->tree = new PlugTree(this);
    d->tree->setObjectName("pluginTree");
    d->tree->setColumnCount(4);
    d->tree->setHeaderLabels(QStringList() << "Name" << "Status" << "Format" << "File");
    d->tree->setUniformRowHeights(true);
    d->tree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    d->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    d->tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    d->tree->setColumnWidth(0, 260);
    d->tree->setColumnWidth(1, 100);
    d->tree->setColumnWidth(2, 70);
    v->addWidget(d->tree, 1);
    d->summary = new QLabel(this);
    d->summary->setObjectName("pluginSummary");
    v->addWidget(d->summary);

    QObject::connect(d->search, &QLineEdit::textChanged, this, [this](const QString& t) { setFilter(t); });
    QObject::connect(rescan, &QPushButton::clicked, this, [this] {
        runPluginScan(window(), false);
        refresh();
    });
    QObject::connect(d->tree, &QTreeWidget::itemDoubleClicked, this, [impl](QTreeWidgetItem* it, int) { impl->activate(it); });
    QObject::connect(d->tree, &QWidget::customContextMenuRequested, this, [this, impl](const QPoint& pos) {
        QTreeWidgetItem* it = impl->tree->itemAt(pos);
        if (!it || it->data(0, kRoleKind).toInt() <= 0) return;
        const QString key = it->data(0, kRoleKey).toString();
        const bool fav = impl->favs.contains(key);
        QMenu menu(impl->tree);
        QAction* use = menu.addAction("Add to the Selection", [impl, it] { impl->activate(it); });
        use->setEnabled(it->data(0, kRoleWorks).toBool());
        menu.addAction(fav ? "Remove from Favorites" : "Add to Favorites", [this, key, fav] { setFavorite(key, !fav); });
        menu.exec(impl->tree->viewport()->mapToGlobal(pos));
    });
    d->listener.reset(new Listener([impl](int changes) {
        if (changes & kCatalog) impl->fill();
    }));
    d->fill();
}

PluginManager::~PluginManager() {}

QStringList PluginManager::favorites() const { return d->favs; }

void PluginManager::setFavorite(const QString& key, bool on)
{
    d->favs.removeAll(key);
    if (on) d->favs << key;
    QSettings().setValue("pluginManager/favorites", d->favs);
    d->fill();
}

void PluginManager::setFilter(const QString& text)
{
    d->filter = text.trimmed();
    if (d->search->text() != text) d->search->setText(text);
    d->fill();
}

void PluginManager::refresh() { d->fill(); }

int PluginManager::count(const QString& folderPath) const { return keys(folderPath).size(); }

QStringList PluginManager::keys(const QString& folderPath) const
{
    QStringList out;
    if (QTreeWidgetItem* f = d->find(folderPath)) d->leaves(f, out);
    return out;
}

QWidget* PluginManager::tree() const { return d->tree; }

}  // namespace app
}  // namespace reno
