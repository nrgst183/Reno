// The plug-in lists (Video FX, Transitions, Media Generators, Audio FX).
#include "panels_internal.h"
#include "onshow.h"
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
#include <QTabBar>
#include <cmath>
#include <QFileSystemModel>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <set>

namespace reno {
namespace app {

namespace {

const int kRoleData = Qt::UserRole;
const int kRoleLeaf = Qt::UserRole + 1;
const int kRoleWorks = Qt::UserRole + 2;
const int kRolePackage = Qt::UserRole + 3;
const int kRolePlugin = Qt::UserRole + 4;  // an OpenFX plug-in's identifier (for its presets)

class FxTree : public QTreeWidget {
public:
    FxTree(const char* format, QWidget* parent) : QTreeWidget(parent), format_(format)
    {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
    }

protected:
    QStringList mimeTypes() const override { return QStringList() << format_; }
    QMimeData* mimeData(const QList<QTreeWidgetItem*>& items) const override
    {
        for (QTreeWidgetItem* it : items) {
            if (!it->data(0, kRoleLeaf).toBool() || !it->data(0, kRoleWorks).toBool()) continue;
            auto* m = new QMimeData();
            m->setData(format_, it->data(0, kRoleData).toString().toUtf8());
            return m;
        }
        return nullptr;
    }

private:
    const char* format_;
};

struct FxRow {
    QString name, group, vendor, data, tip;
    bool works = true;
    bool package = false;  // a saved plug-in package (its group is the packages folder, listed last)
    bool favorite = false;
    std::string pluginId;
};

// VEGAS 18's category bar: flat text tabs over the list (All Plug-ins, the groups, Third Party, Favorites). It keeps a data string
// per tab, as the combo box it replaces did.
class CategoryBar : public QTabBar {
public:
    explicit CategoryBar(QWidget* parent) : QTabBar(parent)
    {
        setExpanding(false);
        setDrawBase(false);
        setUsesScrollButtons(true);
        setElideMode(Qt::ElideNone);
        setFocusPolicy(Qt::NoFocus);
        setObjectName("fxCategory");
        setStyleSheet("QTabBar { background: transparent; }"
                      "QTabBar::tab { background: transparent; color: #dcdcdc; padding: 3px 9px; border: none; margin: 0px; }"
                      "QTabBar::tab:selected { background: #464646; color: #ffffff; }"
                      "QTabBar::tab:hover:!selected { background: #383838; }");
    }
    QVariant currentData() const { return currentIndex() >= 0 ? tabData(currentIndex()) : QVariant(); }
    void clear() { while (count()) removeTab(0); }
    void addItem(const QString& text, const QString& data)
    {
        const int i = addTab(text);
        setTabData(i, data);
    }
    int findData(const QString& data) const
    {
        for (int i = 0; i < count(); ++i)
            if (tabData(i).toString() == data) return i;
        return -1;
    }
};

// A star for the list's favourites (outlined when the plug-in is not one).
QIcon starIcon(bool filled)
{
    static QIcon on, off;
    QIcon& ic = filled ? on : off;
    if (ic.isNull()) {
        QPixmap pm(14, 14);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPolygonF star;
        for (int k = 0; k < 10; ++k) {
            const double a = -M_PI / 2 + k * M_PI / 5, r = k % 2 ? 2.8 : 6.2;
            star << QPointF(7 + r * std::cos(a), 7.4 + r * std::sin(a));
        }
        p.setPen(QPen(filled ? QColor(240, 190, 40) : QColor(160, 160, 160), 1.0));
        p.setBrush(filled ? QColor(240, 190, 40) : Qt::NoBrush);
        p.drawPolygon(star);
        ic = QIcon(pm);
    }
    return ic;
}

// The magnifier at the end of the search box.
QIcon searchIcon()
{
    QPixmap pm(16, 16);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(70, 190, 255), 1.6));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(2, 2, 8, 8));
    p.drawLine(QPointF(9, 9), QPointF(13.5, 13.5));
    return QIcon(pm);
}

// The presets of the plug-in picked in a Video FX / Transitions / Media Generators window, as thumbnails. Dragging
// one carries a package of that effect with the preset's settings.
class PresetList : public QListWidget {
public:
    PresetList(const char* format, QWidget* parent) : QListWidget(parent), format_(format)
    {
        setDragEnabled(true);
        setDragDropMode(QAbstractItemView::DragOnly);
        setDefaultDropAction(Qt::CopyAction);
        setViewMode(QListView::IconMode);
        setIconSize(QSize(120, 68));
        setGridSize(QSize(132, 100));
        setResizeMode(QListView::Adjust);
        setMovement(QListView::Static);
        setWordWrap(true);
        setUniformItemSizes(true);
        setMouseTracking(true);
        setSelectionMode(QAbstractItemView::SingleSelection);
    }
protected:
    QStringList mimeTypes() const override { return QStringList() << format_; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty()) return nullptr;
        auto* m = new QMimeData();
        m->setData(format_, items[0]->data(Qt::UserRole).toString().toUtf8());
        return m;
    }
private:
    const char* format_;
};

}  // namespace

struct FxBrowser::Impl {
    FxBrowser* owner = nullptr;
    Kind kind = VideoFx;
    QLineEdit* search = nullptr;
    FxTree* tree = nullptr;
    QLabel* status = nullptr;
    QWidget* emptyPage = nullptr;
    QStackedWidget* stack = nullptr;
    std::unique_ptr<ListenerHandle> listener;
    std::set<QString> collapsed;
    int packagesListener = 0;
    // VEGAS 18's category bar: All plug-ins, Favorites, Third party (with Filter by provider) and each plug-in group;
    // favorites are starred and saved.
    CategoryBar* category = nullptr;
    QComboBox* provider = nullptr;
    QLabel* providerLabel = nullptr;
    QString favoritesKey() const { return QString("fxFavorites/%1").arg((int)kind); }
    QStringList favorites() const { return QSettings().value(favoritesKey()).toStringList(); }
    static QString favoriteKeyOf(const FxRow& r) { return r.pluginId.empty() ? r.data.left(200) : qs(r.pluginId); }
    // A plug-in not made by VEGAS (or Reno): its provider from its identifier ("com.NewBlue.Foo" -> "NewBlue")
    static QString providerOf(const FxRow& r)
    {
        if (r.package) return QString();
        if (!r.vendor.isEmpty()) return r.vendor;
        const QString id = qs(r.pluginId);
        if (id.isEmpty() || id.startsWith("com.vegascreativesoftware") || id.startsWith("com.sonycreativesoftware") || id.startsWith("com.magix") ||
            id.startsWith("org.reno"))
            return QString();
        static const QRegularExpression dotOrColon("[.:]");       // (compiled once: this runs for every plug-in)
        QStringList parts = id.split(dotOrColon, Qt::SkipEmptyParts);
        if (parts.size() >= 2 && (parts[0] == "com" || parts[0] == "org" || parts[0] == "net" || parts[0] == "de" || parts[0] == "fr" || parts[0] == "uk")) return parts[1];
        return parts.isEmpty() ? QString() : parts[0];
    }
    PresetList* presets = nullptr;
    QLabel* presetsTitle = nullptr;
    QTimer* thumbTimer = nullptr;
    QTimer* animTimer = nullptr;
    std::string presetPlugin;
    QString presetLabel;
    std::vector<OfxPreset> presetList;
    int nextThumb = 0;
    QListWidgetItem* animItem = nullptr;
    int animStep = 0;

    const char* presetContextWord() const { return kind == Transitions ? "Transition" : kind == Generators ? "Generator" : "Filter"; }
    QString packagesGroup() const { return kind == AudioFx ? "FX Packages" : kind == Transitions ? "Transition Packages" : "Filter Packages"; }

    std::string presetData(int index) const
    {
        FxPackage p;
        p.name = presetLabel.toStdString();
        p.kind = kind == Transitions ? PackageKind::Transition : kind == Generators ? PackageKind::Generator : PackageKind::VideoFx;
        FxSlot s;
        s.kind = "ofx";
        s.pluginId = presetPlugin;
        s.name = presetLabel.toStdString();
        if (index >= 0 && (size_t)index < presetList.size()) s.state = presetToState(presetList[(size_t)index]);
        p.chain.push_back(s);
        Json j;
        Json::parse(packageToText(p), j);
        return j.dump();
    }

    // The line under the lists, as VEGAS's: the picked plug-in's name and what it is (the counts show while none is picked).
    QString counts;
    void describe(QTreeWidgetItem* it)
    {
        if (!status) return;
        if (it && it->data(0, kRoleLeaf).toBool() && !it->data(0, kRolePackage).toBool()) {
            QString name = it->text(0);
            const QString tip = it->toolTip(0);
            QString what = kind == AudioFx ? QString("audio effect") : QString("OFX, 32-bit floating point");
            if (kind == AudioFx) {
                const int i = name.lastIndexOf("  [");
                if (i > 0) { what = name.mid(i + 3).chopped(1) + ", 32-bit floating point"; name = name.left(i); }
            }
            status->setText(name + ": " + what);
        } else {
            status->setText(counts);
        }
    }

    void fillPresets(QTreeWidgetItem* it)
    {
        if (!presets) return;
        std::string id;
        QString label;
        if (it && it->data(0, kRoleLeaf).toBool() && it->data(0, kRoleWorks).toBool() && !it->data(0, kRolePackage).toBool()) {
            id = it->data(0, kRolePlugin).toString().toStdString();
            label = it->text(0);
        }
        if (id == presetPlugin && presets->count()) return;
        presetPlugin = id;
        presetLabel = label;
        animItem = nullptr;
        animTimer->stop();
        presets->clear();
        presetList.clear();
        if (id.empty()) {
            presetsTitle->setText("Presets: pick a plug-in above");
            return;
        }
        presetList = presetsFor(id, presetContextWord());
        presetsTitle->setText(QString("Presets of %1 (%2): drag one onto the timeline").arg(label).arg(presetList.size() + 1));
        QPixmap blank(120, 68);
        blank.fill(QColor(40, 40, 44));
        for (int k = -1; k < (int)presetList.size(); ++k) {
            auto* item = new QListWidgetItem(QIcon(blank), k < 0 ? QString("(Default)") : QString::fromStdString(presetList[(size_t)k].name), presets);
            item->setData(Qt::UserRole, QString::fromStdString(presetData(k)));
            item->setData(Qt::UserRole + 1, k);
            item->setToolTip(item->text());
        }
        nextThumb = 0;
        thumbTimer->start(0);
    }

    QImage thumbOf(QListWidgetItem* item, double phase) const
    {
        const int k = item->data(Qt::UserRole + 1).toInt();
        const Json state = k >= 0 && (size_t)k < presetList.size() ? presetToState(presetList[(size_t)k]) : Json();
        return fxThumbnail(presetPlugin, presetContextWord(), state, QSize(120, 68), phase);
    }

    void drawNextThumb()
    {
        if (!presets || nextThumb >= presets->count()) { thumbTimer->stop(); return; }
        QListWidgetItem* item = presets->item(nextThumb++);
        const QImage img = thumbOf(item, 0.5);
        if (!img.isNull()) item->setIcon(QIcon(QPixmap::fromImage(img)));
    }

    void animate()
    {
        if (!animItem || !presets->viewport()->underMouse()) {
            if (animItem) {
                const QImage img = thumbOf(animItem, 0.5);
                if (!img.isNull()) animItem->setIcon(QIcon(QPixmap::fromImage(img)));
            }
            animItem = nullptr;
            animTimer->stop();
            return;
        }
        animStep = (animStep + 1) % 8;
        const QImage img = thumbOf(animItem, animStep / 7.0);
        if (!img.isNull()) animItem->setIcon(QIcon(QPixmap::fromImage(img)));
    }

    std::vector<FxRow> rows() const
    {
        std::vector<FxRow> out;
        if (kind == AudioFx) {
            for (const auto& e : Catalog::get().audio()) {
                if (!vprefs::audioPluginShown(e.desc.path)) continue;  // Preferences > VST Effects
                FxRow r;
                r.name = qs(e.desc.name);
                r.vendor = qs(e.desc.vendor);
                const QString fmt = e.desc.format == "DX" ? "DirectX" : (e.desc.format == "BUILTIN" || e.desc.vendor == kBuiltinAudioVendor) ? "Built-in" : qs(e.desc.format);
                r.group = r.vendor.isEmpty() ? QString("Unknown Vendor") : r.vendor;
                r.data = qs(e.desc.toJson().dump());
                r.works = e.ok;
                r.tip = e.ok ? QString("%1 (%2)\n%3").arg(r.name, fmt, qs(e.desc.path)) : QString("%1 does not work:\n%2").arg(r.name, qs(e.error));
                r.name = QString("%1  [%2]").arg(r.name, fmt);
                out.push_back(r);
            }
        } else {
            for (const auto& e : Catalog::get().video()) {
                const bool wanted = kind == VideoFx ? e.filter : kind == Generators ? e.generator : e.transition;
                if (!wanted) continue;
                FxRow r;
                r.name = qs(e.label.empty() ? e.id : e.label);
                r.group = e.group.empty() ? QString("Other") : qs(e.group);
                r.data = qs(e.id + "\n" + (e.label.empty() ? e.id : e.label));
                r.works = e.ok;
                r.tip = e.ok ? QString("%1\n%2").arg(r.name, qs(e.id)) : QString("%1 does not work:\n%2").arg(r.name, qs(e.error));
                r.pluginId = e.id;
                out.push_back(r);
            }
        }
        if (kind != Generators) {
            const PackageKind pk = kind == AudioFx ? PackageKind::AudioFx : kind == Transitions ? PackageKind::Transition : PackageKind::VideoFx;
            for (const FxPackage& p : listPackages(pk)) {
                FxRow r;
                r.name = qs(p.name);
                r.group = packagesGroup();
                r.package = true;
                Json j;
                Json::parse(packageToText(p), j);
                r.data = qs(j.dump());
                QStringList names;
                for (const FxSlot& sl : p.chain) names << qs(fxSlotLabel(sl));
                r.tip = QString("Package: %1").arg(names.join(", "));
                out.push_back(r);
            }
        }
        return out;
    }

    static bool matches(const FxRow& r, const QStringList& words)
    {
        for (const QString& w : words)
            if (!r.name.contains(w, Qt::CaseInsensitive) && !r.group.contains(w, Qt::CaseInsensitive) && !r.vendor.contains(w, Qt::CaseInsensitive)) return false;
        return true;
    }

    const char* noun(bool plural) const
    {
        switch (kind) {
        case VideoFx: return plural ? "video effects" : "video effect";
        case AudioFx: return plural ? "audio effects" : "audio effect";
        case Generators: return plural ? "generators" : "generator";
        default: return plural ? "transitions" : "transition";
        }
    }

    // The list is filled when it is shown (a browser in a hidden tab costs nothing at start-up, and hidden ones are not
    // refilled for every catalog change); a visible one is filled at once.
    bool stale = false;
    void rebuild()
    {
        if (owner->isVisible()) fill();
        else stale = true;
    }
    void ensureFilled()
    {
        if (stale) fill();
    }
    void fill()
    {
        stale = false;
        for (int k = 0; k < tree->topLevelItemCount(); ++k) {
            QTreeWidgetItem* g = tree->topLevelItem(k);
            if (g->isExpanded()) collapsed.erase(g->text(0));
            else collapsed.insert(g->text(0));
        }
        // nothing scanned and nothing to list: show the explanation (built-in generators and effects are listed even
        // before a scan)
        std::vector<FxRow> all = rows(), shown;
        const bool noCatalog = Catalog::get().empty() && all.empty();
        stack->setCurrentWidget(noCatalog ? emptyPage : static_cast<QWidget*>(tree));
        search->setEnabled(!noCatalog);

        const QStringList words = search->text().split(' ', Qt::SkipEmptyParts);
        const QStringList favs = favorites();
        QString cat = category ? category->currentData().toString() : QString("all");
        if (category) {
            std::set<QString> groups;
            for (const auto& r : all)
                if (!r.package) groups.insert(r.group);
            QSignalBlocker b(category);
            category->clear();
            category->addItem("All Plug-ins", "all");
            for (const QString& g : groups) category->addItem(g, "group:" + g);
            category->addItem("Third Party", "thirdparty");
            category->addItem("Favorites", "favorites");
            category->setTabIcon(category->count() - 1, starIcon(true));
            const int i = category->findData(cat);
            category->setCurrentIndex(i >= 0 ? i : 0);
            cat = category->currentData().toString();
            std::set<QString> providers;
            for (const auto& r : all) {
                const QString pv = providerOf(r);
                if (!pv.isEmpty()) providers.insert(pv);
            }
            const QString pnow = provider->currentData().toString();
            QSignalBlocker pb(provider);
            provider->clear();
            provider->addItem("All providers", QString());
            for (const QString& pv : providers) provider->addItem(pv, pv);
            const int pi = provider->findData(pnow);
            provider->setCurrentIndex(pi >= 0 ? pi : 0);
            provider->setVisible(cat == "thirdparty");
            providerLabel->setVisible(cat == "thirdparty");
        }
        const QString pick = provider ? provider->currentData().toString() : QString();
        for (auto r : all) {
            if (!matches(r, words)) continue;
            if (cat == "favorites" && !favs.contains(favoriteKeyOf(r))) continue;
            if (cat == "thirdparty" && (providerOf(r).isEmpty() || (!pick.isEmpty() && providerOf(r) != pick))) continue;
            if (cat.startsWith("group:") && r.group != cat.mid(6)) continue;
            r.favorite = favs.contains(favoriteKeyOf(r));
            shown.push_back(r);
        }
        std::sort(shown.begin(), shown.end(), [](const FxRow& a, const FxRow& b) {
            if (a.package != b.package) return b.package;
            const bool ao = a.group == "Other", bo = b.group == "Other";
            if (ao != bo) return bo;
            const int g = a.group.compare(b.group, Qt::CaseInsensitive);
            if (g != 0) return g < 0;
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });

        tree->setUpdatesEnabled(false);
        tree->clear();
        QTreeWidgetItem* group = nullptr;
        const bool filtering = !words.isEmpty();
        for (const auto& r : shown) {
            if (!group || group->text(0) != r.group) {
                group = new QTreeWidgetItem(tree);
                group->setText(0, r.group);
                group->setFlags(Qt::ItemIsEnabled);
                group->setSizeHint(0, QSize(1, 1));
                QFont f = group->font(0);
                f.setBold(true);
                group->setFont(0, f);
            }
            auto* it = new QTreeWidgetItem(group);
            it->setText(0, r.name);
            if (!r.package) it->setIcon(0, starIcon(r.favorite));
            it->setData(0, kRoleData, r.data);
            it->setData(0, kRoleLeaf, true);
            it->setData(0, kRoleWorks, r.works);
            it->setData(0, kRolePackage, r.package);
            it->setData(0, kRolePlugin, qs(r.pluginId));
            it->setToolTip(0, r.tip);
            if (r.works) {
                it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
            } else {
                it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                it->setForeground(0, QColor(120, 120, 124));
            }
        }
        for (int k = 0; k < tree->topLevelItemCount(); ++k) {
            QTreeWidgetItem* g = tree->topLevelItem(k);
            g->setExpanded(filtering || !collapsed.count(g->text(0)));
            g->setText(0, QString("%1  (%2)").arg(g->text(0)).arg(g->childCount()));
        }
        tree->setUpdatesEnabled(true);

        if (noCatalog) counts = "No plug-ins known yet";
        else if (filtering) counts = QString("%1 of %2 %3").arg(shown.size()).arg(all.size()).arg(noun(all.size() != 1));
        else counts = QString("%1 %2").arg(all.size()).arg(noun(all.size() != 1));
        describe(tree->currentItem());
    }
    // Above the list (not for audio effects): the category tabs and the Filter (who makes them) box.
    void buildCategoryRow(QVBoxLayout* root)
    {
        QHBoxLayout* cats = new QHBoxLayout();
        cats->setContentsMargins(0, 0, 0, 0);
        cats->setSpacing(4);
        category = new CategoryBar(owner);
        providerLabel = new QLabel("Filter:", owner);
        provider = new QComboBox(owner);
        provider->setObjectName("fxProvider");
        cats->addWidget(category, 1);
        cats->addWidget(providerLabel);
        cats->addWidget(provider);
        root->addLayout(cats);
        QObject::connect(category, &QTabBar::currentChanged, owner, [this](int) {
            QSettings().setValue(QString("fxCategory/%1").arg((int)kind), category->currentData());
            rebuild();
        });
        QObject::connect(provider, QOverload<int>::of(&QComboBox::currentIndexChanged), owner, [this](int) { rebuild(); });
        QSignalBlocker b(category);
        category->addItem("All Plug-ins", QSettings().value(QString("fxCategory/%1").arg((int)kind), "all").toString());
    }

    // The page shown before plug-ins have been looked for, with the button that looks for them.
    QWidget* buildEmptyPage()
    {
        emptyPage = new QWidget(stack);
        emptyPage->setObjectName("noPlugins");
        auto* empty = new QVBoxLayout(emptyPage);
        empty->setContentsMargins(12, 20, 12, 12);
        auto* explain = new QLabel("Plug-ins have not been looked for yet.\n\nReno finds OpenFX effects and VST sound effects installed on this computer. This can take a minute the first time.", emptyPage);
        explain->setWordWrap(true);
        explain->setAlignment(Qt::AlignTop | Qt::AlignLeft);
        auto* scan = new QPushButton("Look for Plug-Ins...", emptyPage);
        scan->setObjectName("lookForPlugins");
        empty->addWidget(explain);
        empty->addWidget(scan, 0, Qt::AlignLeft);
        empty->addStretch(1);
        QObject::connect(scan, &QPushButton::clicked, [this] { runPluginScan(owner->window(), false); });
        return emptyPage;
    }

    // The list beside a preset pane (not for audio effects): the picked plug-in's presets as pictures, the one under the
    // mouse animated.
    QSplitter* buildPresetPane(const char* format)
    {
        QSplitter* split = new QSplitter(Qt::Horizontal, owner);
        split->setObjectName("fxBrowserSplit");
        split->setChildrenCollapsible(false);
        split->addWidget(stack);
        QWidget* pbox = new QWidget(split);
        QVBoxLayout* pv = new QVBoxLayout(pbox);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(2);
        presetsTitle = new QLabel("Presets: pick a plug-in above", pbox);
        presetsTitle->setObjectName("fxPresetsTitle");
        presetsTitle->setTextFormat(Qt::PlainText);
        presets = new PresetList(format, pbox);
        presets->setObjectName("fxPresetThumbs");
        presets->setMinimumHeight(90);
        presetsTitle->hide();   // (VEGAS shows the presets with no heading)
        pv->addWidget(presetsTitle);
        pv->addWidget(presets, 1);
        split->addWidget(pbox);
        split->setStretchFactor(0, 0);
        split->setStretchFactor(1, 1);
        split->setSizes({210, 480});
        stack->setMinimumWidth(140);
        thumbTimer = new QTimer(owner);
        animTimer = new QTimer(owner);
        animTimer->setInterval(140);
        QObject::connect(thumbTimer, &QTimer::timeout, owner, [this] { drawNextThumb(); });
        QObject::connect(animTimer, &QTimer::timeout, owner, [this] { animate(); });
        QObject::connect(presets, &QListWidget::itemEntered, owner, [this](QListWidgetItem* it) { startAnimating(it); });
        QObject::connect(presets, &QListWidget::itemActivated, owner, [this](QListWidgetItem* it) {
            if (it && owner->onActivated) owner->onActivated(kind, it->data(Qt::UserRole).toString().toStdString());
        });
        QObject::connect(tree, &QTreeWidget::currentItemChanged, owner, [this](QTreeWidgetItem* it, QTreeWidgetItem*) {
            fillPresets(it);
            describe(it);
        });
        return split;
    }

    // The preset under the mouse plays its animation; the one it left goes back to its still picture.
    void startAnimating(QListWidgetItem* it)
    {
        if (animItem == it) return;
        if (animItem) {
            const QImage img = thumbOf(animItem, 0.5);
            if (!img.isNull()) animItem->setIcon(QIcon(QPixmap::fromImage(img)));
        }
        animItem = it;
        animStep = 3;
        animTimer->start();
    }

    // A plug-in's shortcut menu: Add to Favorites or Remove from Favorites.
    void showTreeMenu(const QPoint& pos)
    {
        QTreeWidgetItem* it = tree->itemAt(pos);
        if (!it || !it->data(0, kRoleLeaf).toBool() || it->data(0, kRolePackage).toBool()) return;
        FxRow r;
        r.pluginId = it->data(0, kRolePlugin).toString().toStdString();
        r.data = it->data(0, kRoleData).toString();
        const QString key = favoriteKeyOf(r);
        const bool is = favorites().contains(key);
        QMenu menu(owner);
        menu.setObjectName("fxMenu");
        menu.addAction(is ? "Remove from Favorites" : "Add to Favorites", [this, key, is] {
            QStringList f = favorites();
            if (is) f.removeAll(key);
            else f << key;
            QSettings().setValue(favoritesKey(), f);
            rebuild();
        });
        menu.exec(tree->viewport()->mapToGlobal(pos));
    }
};

FxBrowser::FxBrowser(Kind kind, QWidget* parent) : QWidget(parent), d(new Impl())
{
    d->owner = this;
    d->kind = kind;
    Impl* impl = d.get();
    const char* format = kind == VideoFx ? kMimeVideoFx : kind == AudioFx ? kMimeAudioFx : kind == Generators ? kMimeGenerator : kMimeTransition;

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 0);
    root->setSpacing(4);
    d->search = new QLineEdit(this);
    d->search->setObjectName("searchBox");
    d->search->setClearButtonEnabled(true);
    d->search->setToolTip("Search plug-ins (name, group or vendor)");
    d->search->addAction(searchIcon(), QLineEdit::TrailingPosition);
    d->search->setFixedWidth(220);
    QHBoxLayout* top = new QHBoxLayout();
    top->setContentsMargins(0, 0, 0, 0);
    top->addWidget(d->search);
    top->addStretch(1);
    root->addLayout(top);
    if (kind != AudioFx) d->buildCategoryRow(root);

    d->stack = new QStackedWidget(this);
    d->tree = new FxTree(format, d->stack);
    d->tree->setObjectName("fxTree");
    d->tree->setHeaderHidden(true);
    d->tree->setSelectionMode(QAbstractItemView::SingleSelection);
    d->tree->setIndentation(14);
    d->tree->setAnimated(false);
    d->tree->setUniformRowHeights(false);   // (the group rows are a sliver: VEGAS lists the plug-ins flat under its category tabs)
    d->tree->setIconSize(QSize(14, 14));
    d->tree->setContextMenuPolicy(Qt::CustomContextMenu);
    d->stack->addWidget(d->tree);
    d->stack->addWidget(d->buildEmptyPage());
    if (kind == AudioFx) root->addWidget(d->stack, 1);
    else root->addWidget(d->buildPresetPane(format), 1);

    d->status = new QLabel(this);
    d->status->setObjectName("fxStatus");
    QPalette statusPal = palette();
    statusPal.setColor(QPalette::WindowText, QColor(160, 160, 165));
    d->status->setPalette(statusPal);
    root->addWidget(d->status);

    QObject::connect(d->search, &QLineEdit::textChanged, [impl](const QString&) { impl->rebuild(); });
    QObject::connect(d->tree, &QTreeWidget::itemActivated, [this, impl](QTreeWidgetItem* it, int) {
        if (!it || !it->data(0, kRoleLeaf).toBool() || !it->data(0, kRoleWorks).toBool()) return;
        if (onActivated) onActivated(impl->kind, it->data(0, kRoleData).toString().toStdString());
    });
    QObject::connect(d->tree, &QWidget::customContextMenuRequested, this, [impl](const QPoint& pos) { impl->showTreeMenu(pos); });
    d->listener.reset(new ListenerHandle([impl](int changes) {
        if (changes & kCatalog) impl->rebuild();
    }));
    d->packagesListener = onPackagesChanged([impl] { impl->rebuild(); });
    // the list is filled when the browser is first shown, not when it is made
    callWhenShown(this, [impl] { impl->ensureFilled(); });
    d->rebuild();
}

void FxBrowser::setFavorite(const std::string& pluginId, bool on)
{
    QStringList f = d->favorites();
    const QString k = QString::fromStdString(pluginId);
    f.removeAll(k);
    if (on) f << k;
    QSettings().setValue(d->favoritesKey(), f);
    d->rebuild();
}

void FxBrowser::setCategory(const QString& key)
{
    d->ensureFilled();
    if (!d->category) return;
    const int i = d->category->findData(key);
    if (i >= 0) d->category->setCurrentIndex(i);
}

int FxBrowser::listedPlugins() const
{
    d->ensureFilled();
    int n = 0;
    for (QTreeWidgetItemIterator it(d->tree); *it; ++it)
        if ((*it)->data(0, kRoleLeaf).toBool()) ++n;
    return n;
}

FxBrowser::~FxBrowser() { forgetPackagesListener(d->packagesListener); }

}  // namespace app
}  // namespace reno
