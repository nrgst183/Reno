// VEGAS's Plug-In Chooser (see pluginchooser.h).
//
// Laid out as in VEGAS Pro 18: the chain being built across the top (a dark bar, "No plug-ins in chain" while empty)
// with its own undo and redo; under it the folder path with Up One Level, Create New Folder and Delete; the folder
// tree on the left and the folder's contents in columns filling the window; OK, Cancel, Add, Remove, Save As... down
// the right. Reno adds a search box.
#include "pluginchooser.h"
#include "vprefs.h"
#include "fxlibrary.h"
#include "../engine/catalog.h"
#include "../engine/fxnames.h"
#include "strconv.h"
#include <QApplication>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QScreen>
#include <QAction>
#include <QResizeEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QSettings>
#include <QInputDialog>
#include <algorithm>
#include <cmath>
#include <set>
#include <map>

namespace reno {
namespace app {

std::function<void(PluginChooser*)> PluginChooser::onShown;
std::function<QString(const QStringList&)> PluginChooser::packageNameForTests;
std::function<QAction*(QMenu*)> PluginChooser::menuForTests;
std::function<QString(const QString&)> PluginChooser::folderNameForTests;

namespace {


const char* const kMimeChoice = "application/x-reno-chooser-plugin";
const char* const kMimeChainPos = "application/x-reno-chooser-chain";
const int kRoleChoice = Qt::UserRole + 1;
const int kRoleFolder = Qt::UserRole + 2;

enum class Pic { Folder, Video, Audio, Transition, Generator, Package, Left, Right, Remove, Up, Views, Undo, Redo, NewFolder, Delete, Badge };

QPixmap picture(Pic what, int size, const QColor& ink)
{
    QPixmap pm(size * 2, size * 2);
    pm.setDevicePixelRatio(2.0);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = size / 16.0;
    p.scale(s, s);
    QPen pen(ink, 1.3);
    pen.setJoinStyle(Qt::RoundJoin);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    switch (what) {
    case Pic::Folder: {
        QPainterPath f;
        f.moveTo(1.5, 4);
        f.lineTo(6, 4);
        f.lineTo(7.5, 5.5);
        f.lineTo(14.5, 5.5);
        f.lineTo(14.5, 13);
        f.lineTo(1.5, 13);
        f.closeSubpath();
        p.setBrush(QColor(214, 178, 92));
        p.setPen(QPen(QColor(150, 118, 50), 1));
        p.drawPath(f);
        break;
    }
    case Pic::Video:
    case Pic::Audio:
    case Pic::Transition:
    case Pic::Generator: {
        const QColor tile = what == Pic::Video ? QColor(64, 112, 176) : what == Pic::Audio ? QColor(70, 140, 92)
                          : what == Pic::Transition ? QColor(150, 92, 160) : QColor(176, 120, 58);
        p.setPen(QPen(tile.lighter(140), 1));
        p.setBrush(tile);
        p.drawRoundedRect(QRectF(1.5, 1.5, 13, 13), 3, 3);
        p.setPen(QPen(QColor(240, 240, 240), 1.3));
        p.setBrush(Qt::NoBrush);
        if (what == Pic::Video) {
            p.drawLine(QPointF(8, 4), QPointF(8, 12));
            p.drawLine(QPointF(4, 8), QPointF(12, 8));
            p.drawLine(QPointF(5.2, 5.2), QPointF(10.8, 10.8));
            p.drawLine(QPointF(10.8, 5.2), QPointF(5.2, 10.8));
        } else if (what == Pic::Audio) {
            QPainterPath w;
            w.moveTo(3.5, 8);
            for (int k = 0; k <= 18; ++k) w.lineTo(3.5 + k * 0.5, 8 - 3.5 * std::sin(k * 0.7));
            p.drawPath(w);
        } else if (what == Pic::Transition) {
            p.setBrush(QColor(240, 240, 240, 200));
            const QPointF tri[3] = {QPointF(4, 4), QPointF(12, 4), QPointF(4, 12)};
            p.drawPolygon(tri, 3);
        } else {
            p.drawEllipse(QRectF(4.5, 4.5, 7, 7));
            p.drawPoint(QPointF(8, 8));
        }
        break;
    }
    case Pic::Package: {
        p.setBrush(QColor(196, 160, 110));
        p.setPen(QPen(QColor(120, 92, 52), 1));
        p.drawRect(QRectF(2, 5, 12, 9));
        p.drawLine(QPointF(2, 5), QPointF(4, 2));
        p.drawLine(QPointF(14, 5), QPointF(12, 2));
        p.drawLine(QPointF(4, 2), QPointF(12, 2));
        p.setPen(QPen(QColor(90, 70, 40), 1));
        p.drawLine(QPointF(8, 5), QPointF(8, 14));
        break;
    }
    case Pic::Left: {
        const QPointF a[3] = {QPointF(10, 3.5), QPointF(5.5, 8), QPointF(10, 12.5)};
        p.drawPolyline(a, 3);
        break;
    }
    case Pic::Right: {
        const QPointF a[3] = {QPointF(6, 3.5), QPointF(10.5, 8), QPointF(6, 12.5)};
        p.drawPolyline(a, 3);
        break;
    }
    case Pic::Remove:
        p.drawLine(QPointF(4.5, 4.5), QPointF(11.5, 11.5));
        p.drawLine(QPointF(11.5, 4.5), QPointF(4.5, 11.5));
        break;
    case Pic::Up: {
        const QPointF a[3] = {QPointF(4, 8), QPointF(8, 4), QPointF(12, 8)};
        p.drawPolyline(a, 3);
        p.drawLine(QPointF(8, 4), QPointF(8, 13));
        break;
    }
    case Pic::Views:
        for (int r = 0; r < 3; ++r) {
            p.drawRect(QRectF(2.5, 3 + r * 4, 2, 2));
            p.drawLine(QPointF(6.5, 4 + r * 4), QPointF(13.5, 4 + r * 4));
        }
        break;
    case Pic::Undo:
    case Pic::Redo: {
        if (what == Pic::Redo) {
            p.translate(16, 0);
            p.scale(-1, 1);
        }
        QPainterPath a;
        a.moveTo(4, 7);
        a.cubicTo(6, 3.5, 12.5, 3.5, 13, 9);
        a.cubicTo(13.2, 11, 12, 12.5, 10.5, 13);
        p.drawPath(a);
        const QPointF head[3] = {QPointF(3, 3.5), QPointF(4, 7.5), QPointF(8, 6.8)};
        p.drawPolyline(head, 3);
        break;
    }
    case Pic::NewFolder: {
        QPainterPath f;
        f.moveTo(1.5, 5);
        f.lineTo(5.5, 5);
        f.lineTo(7, 6.5);
        f.lineTo(12.5, 6.5);
        f.lineTo(12.5, 13.5);
        f.lineTo(1.5, 13.5);
        f.closeSubpath();
        p.setBrush(QColor(214, 178, 92));
        p.setPen(QPen(QColor(150, 118, 50), 1));
        p.drawPath(f);
        p.setPen(QPen(ink, 1.3));
        p.drawLine(QPointF(13, 1.5), QPointF(13, 5.5));
        p.drawLine(QPointF(11, 3.5), QPointF(15, 3.5));
        break;
    }
    case Pic::Delete:
        p.setPen(QPen(ink, 1.6));
        p.drawLine(QPointF(4, 4), QPointF(12, 12));
        p.drawLine(QPointF(12, 4), QPointF(4, 12));
        break;
    case Pic::Badge: {
        QFont f = QApplication::font();
        f.setBold(true);
        f.setPixelSize(10);
        f.setStretch(85);
        p.setFont(f);
        p.setPen(QColor(70, 156, 226));
        p.drawText(QRectF(-1, 0, 18, 16), Qt::AlignCenter, "FX");
        break;
    }
    }
    return pm;
}

QIcon icon(Pic what, int size = 16)
{
    const QPalette pal = QApplication::palette();
    QIcon i;
    if (what == Pic::Badge) {
        i.addPixmap(picture(what, size, QColor()), QIcon::Normal);
        QPixmap white = picture(what, size, QColor());
        QPainter p(&white);
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(white.rect(), QColor(255, 255, 255));
        p.end();
        i.addPixmap(white, QIcon::Selected);
        return i;
    }
    i.addPixmap(picture(what, size, pal.color(QPalette::Active, QPalette::ButtonText)), QIcon::Normal);
    i.addPixmap(picture(what, size, pal.color(QPalette::Disabled, QPalette::ButtonText)), QIcon::Disabled);
    return i;
}

QToolButton* toolButton(QWidget* parent, Pic pic, const QString& tip, const char* name)
{
    QToolButton* b = new QToolButton(parent);
    b->setObjectName(name);
    b->setIcon(icon(pic));
    b->setIconSize(QSize(16, 16));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    return b;
}

// Whether a plug-in is one of VEGAS's own (VEGAS's "VEGAS" folder; everything else is "Third Party").
bool isVegasVideo(const VideoFxEntry& e)
{
    const std::string id = toLower(e.id);
    return id.compare(0, 25, "com.vegascreativesoftware") == 0 || id.compare(0, 24, "com.sonycreativesoftware") == 0 ||
           id.compare(0, 9, "de.magix:") == 0 || id.compare(0, 10, "com.magix.") == 0 || e.label.compare(0, 6, "VEGAS ") == 0 ||
           e.label.compare(0, 5, "Sony ") == 0;
}
bool isVegasAudio(const AudioPluginDesc& d)
{
    const std::string v = toLower(d.vendor), n = toLower(d.name);
    return v.find("sony") != std::string::npos || v.find("vegas") != std::string::npos || v.find("magix") != std::string::npos ||
           n.compare(0, 6, "vegas ") == 0 || n.compare(0, 8, "express ") == 0;
}

struct Choice {
    QString label, group, vendor, format, tip;
    bool vegas = false, works = true, package = false;
    FxChain plugins;
    FxPackage pack;
};

// What a user folder remembers of an entry: "package:<name>", "ofx:<plug-in id>", "audio:<AudioPluginDesc::key>".
QString choiceKey(const Choice& c)
{
    if (c.package) return "package:" + c.label;
    if (c.plugins.empty()) return QString();
    const FxSlot& s = c.plugins[0];
    return s.kind == "audio" ? "audio:" + qs(s.audio.key()) : "ofx:" + qs(s.pluginId);
}

// User folders of the Plug-In Chooser (Create New Folder, Rename, Delete; plug-ins and packages are put in them with
// Add to Folder or by dragging). Saved in Reno's settings per chooser kind as JSON: [{"name": ..., "items":
// [choiceKey, ...]}, ...].
struct UserFolder {
    QString name;
    QStringList items;
};

QString folderSettingsKey(ChooserKind k)
{
    switch (k) {
    case ChooserKind::AudioFx: return "pluginChooser/userFolders/audio";
    case ChooserKind::Transition: return "pluginChooser/userFolders/transitions";
    case ChooserKind::Generator: return "pluginChooser/userFolders/generators";
    default: return "pluginChooser/userFolders/video";
    }
}

std::vector<UserFolder> loadUserFolders(ChooserKind k)
{
    std::vector<UserFolder> out;
    Json j;
    if (!Json::parse(QSettings().value(folderSettingsKey(k)).toString().toStdString(), j)) return out;
    for (const Json& f : j.items()) {
        UserFolder u;
        u.name = qs(f.get("name").asString());
        if (u.name.trimmed().isEmpty()) continue;
        for (const Json& i : f.get("items").items()) u.items << qs(i.asString());
        out.push_back(u);
    }
    return out;
}

void saveUserFolders(ChooserKind k, const std::vector<UserFolder>& list)
{
    Json j = Json::array();
    for (const UserFolder& u : list) {
        Json f = Json::object();
        f["name"] = u.name.toStdString();
        Json items = Json::array();
        for (const QString& i : u.items) items.push(i.toStdString());
        f["items"] = items;
        j.push(f);
    }
    QSettings().setValue(folderSettingsKey(k), QString::fromStdString(j.dump()));
}

// The list of plug-ins (and of the folder's subfolders, first): dragging a plug-in carries its number.
class ChoiceList : public QListWidget {
public:
    explicit ChoiceList(QWidget* parent) : QListWidget(parent) {}
protected:
    QStringList mimeTypes() const override { return QStringList() << kMimeChoice; }
    QMimeData* mimeData(const QList<QListWidgetItem*>& items) const override
    {
        if (items.isEmpty() || items[0]->data(kRoleChoice).toInt() < 0) return nullptr;
        QMimeData* m = new QMimeData();
        m->setData(kMimeChoice, QByteArray::number(items[0]->data(kRoleChoice).toInt()));
        return m;
    }
};

class ChainChip : public QToolButton {
public:
    int pos = 0;
    std::function<void()> onPick;
    std::function<void(const QPoint&)> onMenu;
    explicit ChainChip(QWidget* parent) : QToolButton(parent)
    {
        setObjectName("chooserChainItem");
        setCheckable(true);
        setAutoRaise(false);
        setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        setIconSize(QSize(14, 14));
        setFocusPolicy(Qt::NoFocus);
    }
protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() == Qt::LeftButton || e->button() == Qt::RightButton) {
            press_ = e->position().toPoint();
            if (onPick) onPick();
        }
        if (e->button() == Qt::RightButton && onMenu) onMenu(e->position().toPoint());
        e->accept();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        if (!(e->buttons() & Qt::LeftButton) || (e->position().toPoint() - press_).manhattanLength() < QApplication::startDragDistance()) return;
        QDrag* drag = new QDrag(this);
        QMimeData* m = new QMimeData();
        m->setData(kMimeChainPos, QByteArray::number(pos));
        drag->setMimeData(m);
        drag->setPixmap(grab());
        drag->exec(Qt::MoveAction);
    }
    void mouseReleaseEvent(QMouseEvent* e) override { e->accept(); }
private:
    QPoint press_;
};

// The row the chain is shown in (VEGAS's dark bar across the top: "No plug-ins in chain" while empty, the plug-ins as
// buttons from the left otherwise). Accepts plug-ins dropped from the list and chain plug-ins dragged along it.
class ChainRow : public QWidget {
public:
    std::function<void(int choice, int at)> onDropChoice;
    std::function<void(int from, int at)> onDropMove;
    QHBoxLayout* row = nullptr;
    QLabel* empty = nullptr;
    explicit ChainRow(QWidget* parent) : QWidget(parent)
    {
        setObjectName("chooserChain");
        setAcceptDrops(true);
        row = new QHBoxLayout(this);
        row->setContentsMargins(3, 2, 3, 2);
        row->setSpacing(3);
        row->addStretch(1);
        empty = new QLabel("No plug-ins in chain", this);
        empty->setObjectName("chooserChainEmpty");
        empty->setAlignment(Qt::AlignCenter);
        empty->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    int gapAt(int x) const
    {
        int n = 0;
        for (QObject* o : children()) {
            ChainChip* c = dynamic_cast<ChainChip*>(o);
            if (!c || !c->isVisible()) continue;
            if (x < c->geometry().center().x()) return c->pos;
            n = std::max(n, c->pos + 1);
        }
        return n;
    }
protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        empty->setGeometry(rect());
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMimeChoice) || e->mimeData()->hasFormat(kMimeChainPos)) e->acceptProposedAction();
    }
    void dragMoveEvent(QDragMoveEvent* e) override { e->acceptProposedAction(); }
    void dropEvent(QDropEvent* e) override
    {
        const int at = gapAt(e->position().toPoint().x());
        if (e->mimeData()->hasFormat(kMimeChoice)) {
            if (onDropChoice) onDropChoice(e->mimeData()->data(kMimeChoice).toInt(), at);
        } else if (e->mimeData()->hasFormat(kMimeChainPos)) {
            if (onDropMove) onDropMove(e->mimeData()->data(kMimeChainPos).toInt(), at);
        }
        e->acceptProposedAction();
    }
};

PackageKind packageKindOf(ChooserKind k)
{
    switch (k) {
    case ChooserKind::AudioFx: return PackageKind::AudioFx;
    case ChooserKind::Transition: return PackageKind::Transition;
    case ChooserKind::Generator: return PackageKind::Generator;
    default: return PackageKind::VideoFx;
    }
}

}

struct PluginChooser::Impl {
    PluginChooser* q = nullptr;
    ChooserKind kind = ChooserKind::VideoFx;
    bool single = false;
    std::vector<Choice> choices;
    FxChain chain;
    int chainSel = -1;
    struct ChainState {
        FxChain chain;
        int sel = -1;
    };
    std::vector<ChainState> undos, redos;

    QTreeWidget* folders = nullptr;
    ChoiceList* list = nullptr;
    QComboBox* path = nullptr;
    QLineEdit* search = nullptr;
    QLabel* info = nullptr;
    QWidget* chainBox = nullptr;
    ChainRow* chainRow = nullptr;
    QToolButton *upBtn = nullptr, *newFolderBtn = nullptr, *deleteBtn = nullptr, *viewBtn = nullptr;
    QToolButton *leftBtn = nullptr, *rightBtn = nullptr, *undoBtn = nullptr, *redoBtn = nullptr;
    QPushButton *addBtn = nullptr, *removeBtn = nullptr, *saveBtn = nullptr, *lutBtn = nullptr, *okBtn = nullptr;
    std::vector<ChainChip*> chips;

    QString packagesFolderName() const
    {
        switch (kind) {
        case ChooserKind::AudioFx: return "FX Packages";
        case ChooserKind::Transition: return "Transition Packages";
        case ChooserKind::Generator: return "Media Generator Packages";
        default: return "Filter Packages";
        }
    }
    QString rootName() const
    {
        switch (kind) {
        case ChooserKind::Transition: return "Transitions";
        case ChooserKind::Generator: return "Media Generators";
        default: return "FX";
        }
    }
    Pic pluginPic() const
    {
        switch (kind) {
        case ChooserKind::AudioFx: return Pic::Audio;
        case ChooserKind::Transition: return Pic::Transition;
        case ChooserKind::Generator: return Pic::Generator;
        default: return Pic::Video;
        }
    }

    void loadChoices()
    {
        choices.clear();
        if (kind == ChooserKind::AudioFx) {
            for (const AudioFxEntry& e : Catalog::get().audio()) {
                if (e.desc.isInstrument || !vprefs::audioPluginShown(e.desc.path)) continue;
                Choice c;
                c.label = qs(e.desc.name.empty() ? pathFileName(e.desc.path) : e.desc.name);
                c.vendor = qs(e.desc.vendor);
                c.format = e.desc.format == "DX" ? QString("DirectX") : e.desc.format == "BUILTIN" ? QString("Built-in") : qs(e.desc.format);
                c.group = c.vendor.isEmpty() ? QString("Unknown Vendor") : c.vendor;
                c.vegas = isVegasAudio(e.desc);
                c.works = e.ok;
                c.tip = e.ok ? QString("%1\n%2 plug-in%4 by %3").arg(c.label, c.format, c.vendor.isEmpty() ? QString("an unknown maker") : c.vendor, qs(audioPluginKindNote(e.desc)))
                             : QString("%1 does not work: %2").arg(c.label, qs(e.error));
                FxSlot s;
                s.kind = "audio";
                s.audio = e.desc;
                s.name = e.desc.name;
                c.plugins.push_back(s);
                choices.push_back(c);
            }
        } else {
            for (const VideoFxEntry& e : Catalog::get().video()) {
                const bool wanted = kind == ChooserKind::VideoFx ? e.filter : kind == ChooserKind::Generator ? e.generator : e.transition;
                if (!wanted) continue;
                Choice c;
                c.label = qs(e.label.empty() ? e.id : e.label);
                c.group = e.group.empty() ? QString("Other") : qs(e.group);
                c.format = "OFX";
                c.vegas = isVegasVideo(e);
                c.works = e.ok;
                c.tip = e.ok ? QString("%1\n%2 (%3)").arg(c.label, qs(e.id), c.group) : QString("%1 does not work: %2").arg(c.label, qs(e.error));
                FxSlot s;
                s.kind = "ofx";
                s.pluginId = e.id;
                s.name = e.label.empty() ? e.id : e.label;
                c.plugins.push_back(s);
                choices.push_back(c);
            }
        }
        std::sort(choices.begin(), choices.end(), [](const Choice& a, const Choice& b) { return a.label.compare(b.label, Qt::CaseInsensitive) < 0; });
        for (const FxPackage& p : listPackages(packageKindOf(kind))) {
            Choice c;
            c.label = qs(p.name);
            c.package = true;
            c.pack = p;
            c.plugins = p.chain;
            QStringList names;
            for (const FxSlot& s : p.chain) names << qs(fxSlotLabel(s));
            c.tip = QString("Package: %1").arg(names.join(", "));
            choices.push_back(c);
        }
    }

    // VEGAS's folders: the top one ("FX") holds every plug-in; in it, in alphabetical order, the packages, the plug-ins'
    // own groupings (OpenFX groups; for sound the makers) and formats (OFX; DirectX, VST2, VST3), Third Party, VEGAS.
    void buildFolders()
    {
        const QString was = folders->currentItem() ? folders->currentItem()->data(0, kRoleFolder).toString() : QString("all");
        const QSignalBlocker block(folders);
        folders->clear();
        QTreeWidgetItem* root = new QTreeWidgetItem(folders);
        root->setText(0, rootName());
        root->setIcon(0, icon(Pic::Folder));
        root->setData(0, kRoleFolder, "all");
        struct Sub {
            QString name, key;
            Pic pic;
            bool user;
        };
        std::vector<Sub> subs;
        std::set<QString> taken;
        auto add = [&](const QString& name, const QString& key, Pic pic = Pic::Folder, bool user = false) {
            if (name.trimmed().isEmpty() || taken.count(name.toLower())) return;
            taken.insert(name.toLower());
            subs.push_back({name, key, pic, user});
        };
        add(packagesFolderName(), "packages", Pic::Package);
        add("Third Party", "third");
        add("VEGAS", "vegas");
        if (kind == ChooserKind::AudioFx) {
            std::set<QString> formats;
            for (const Choice& c : choices)
                if (!c.package) formats.insert(c.format);
            for (const QString& f : formats) add(f, "format:" + f);
        } else {
            add("OFX", "ofx");
        }
        std::set<QString> groups;
        for (const Choice& c : choices)
            if (!c.package) groups.insert(c.group);
        for (const QString& g : groups) add(g, "group:" + g);
        for (const UserFolder& u : loadUserFolders(kind)) add(u.name, "user:" + u.name, Pic::Folder, true);
        std::stable_sort(subs.begin(), subs.end(), [](const Sub& a, const Sub& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
        for (const Sub& s : subs) {
            QTreeWidgetItem* it = new QTreeWidgetItem(root);
            it->setText(0, s.name);
            it->setIcon(0, icon(s.pic));
            it->setData(0, kRoleFolder, s.key);
            if (s.user) it->setFlags(it->flags() | Qt::ItemIsDropEnabled);
        }
        folders->expandAll();
        const QSignalBlocker blockPath(path);
        path->clear();
        QTreeWidgetItemIterator it(folders);
        for (; *it; ++it) path->addItem((*it)->icon(0), (*it)->text(0), (*it)->data(0, kRoleFolder));
        select(was);
    }

    QTreeWidgetItem* folderItem(const QString& key) const
    {
        QTreeWidgetItemIterator it(folders);
        for (; *it; ++it)
            if ((*it)->data(0, kRoleFolder).toString() == key) return *it;
        return nullptr;
    }
    QString currentKey() const { return folders->currentItem() ? folders->currentItem()->data(0, kRoleFolder).toString() : QString("all"); }

    // ---- the user's folders (VEGAS's Create New Folder / Rename / Delete)
    // the name of a folder Reno makes itself
    bool reservedName(const QString& n) const
    {
        QTreeWidgetItemIterator it(folders);
        for (; *it; ++it)
            if (!(*it)->data(0, kRoleFolder).toString().startsWith("user:") && (*it)->text(0).compare(n, Qt::CaseInsensitive) == 0) return true;
        return n.compare("All", Qt::CaseInsensitive) == 0;
    }
    QString uniqueFolderName(const QString& base) const
    {
        const std::vector<UserFolder> all = loadUserFolders(kind);
        auto taken = [&](const QString& n) {
            for (const UserFolder& u : all) if (u.name.compare(n, Qt::CaseInsensitive) == 0) return true;
            return reservedName(n);
        };
        if (!taken(base)) return base;
        for (int k = 2;; ++k) if (!taken(QString("%1 (%2)").arg(base).arg(k))) return QString("%1 (%2)").arg(base).arg(k);
    }

    bool createFolder(const QString& name)
    {
        const QString n = name.trimmed();
        if (n.isEmpty()) return false;
        std::vector<UserFolder> all = loadUserFolders(kind);
        if (reservedName(n)) return false;
        for (const UserFolder& u : all) if (u.name.compare(n, Qt::CaseInsensitive) == 0) return false;
        UserFolder u;
        u.name = n;
        all.push_back(u);
        saveUserFolders(kind, all);
        buildFolders();
        select("user:" + n);
        return true;
    }

    bool renameFolder(const QString& from, const QString& to)
    {
        const QString n = to.trimmed();
        if (n.isEmpty()) return false;
        std::vector<UserFolder> all = loadUserFolders(kind);
        for (const UserFolder& u : all) if (u.name.compare(n, Qt::CaseInsensitive) == 0 && u.name != from) return false;
        bool found = false;
        if (reservedName(n)) return false;
        for (UserFolder& u : all) if (u.name == from) { u.name = n; found = true; }
        if (!found) return false;
        saveUserFolders(kind, all);
        buildFolders();
        select("user:" + n);
        return true;
    }

    bool deleteFolder(const QString& name)
    {
        std::vector<UserFolder> all = loadUserFolders(kind);
        const size_t before = all.size();
        all.erase(std::remove_if(all.begin(), all.end(), [&](const UserFolder& u) { return u.name == name; }), all.end());
        if (all.size() == before) return false;
        saveUserFolders(kind, all);
        buildFolders();
        return true;
    }

    bool putInFolder(int choice, const QString& folder, bool in)
    {
        if (choice < 0 || (size_t)choice >= choices.size()) return false;
        const QString key = choiceKey(choices[(size_t)choice]);
        if (key.isEmpty()) return false;
        std::vector<UserFolder> all = loadUserFolders(kind);
        bool found = false;
        for (UserFolder& u : all)
            if (u.name == folder) {
                found = true;
                if (in && !u.items.contains(key)) u.items << key;
                if (!in) u.items.removeAll(key);
            }
        if (!found) return false;
        saveUserFolders(kind, all);
        fillList();
        return true;
    }

    bool renamePackage(int choice, const QString& to, std::string* error)
    {
        if (choice < 0 || (size_t)choice >= choices.size() || !choices[(size_t)choice].package) return false;
        const QString n = to.trimmed();
        FxPackage p = choices[(size_t)choice].pack;
        const QString from = qs(p.name);
        if (n.isEmpty() || n == from) return false;
        for (const FxPackage& o : listPackages(packageKindOf(kind)))
            if (qs(o.name).compare(n, Qt::CaseInsensitive) == 0) {
                if (error) *error = strfmt("There is already a package called '%s'.", n.toStdString().c_str());
                return false;
            }
        FxPackage renamed = p;
        renamed.name = n.toStdString();
        renamed.file.clear();
        if (!savePackage(renamed, error)) return false;
        std::string ignore;
        deletePackage(p, &ignore);
        std::vector<UserFolder> all = loadUserFolders(kind);
        for (UserFolder& u : all)
            for (QString& i : u.items)
                if (i == "package:" + from) i = "package:" + n;
        saveUserFolders(kind, all);
        loadChoices();
        fillList();
        return true;
    }

    void newFolderAsked()
    {
        const QString n = PluginChooser::folderNameForTests ? PluginChooser::folderNameForTests(uniqueFolderName("New Folder"))
                                                            : QInputDialog::getText(q, "Create New Folder", "Folder name:", QLineEdit::Normal, uniqueFolderName("New Folder"));
        if (!n.trimmed().isEmpty() && !createFolder(n)) QMessageBox::warning(q, "Create New Folder", QString("There is already a folder called '%1'.").arg(n.trimmed()));
    }
    void renameFolderAsked(const QString& from)
    {
        const QString n = PluginChooser::folderNameForTests ? PluginChooser::folderNameForTests(from)
                                                            : QInputDialog::getText(q, "Rename", "Folder name:", QLineEdit::Normal, from);
        if (!n.trimmed().isEmpty() && n.trimmed() != from && !renameFolder(from, n))
            QMessageBox::warning(q, "Rename", QString("There is already a folder called '%1'.").arg(n.trimmed()));
    }
    void deleteFolderAsked(const QString& name)
    {
        if (PluginChooser::folderNameForTests || QMessageBox::question(q, "Delete", QString("Delete the folder '%1'? (The plug-ins in it stay where they are.)").arg(name)) == QMessageBox::Yes)
            deleteFolder(name);
    }
    void deletePackageAsked(int k)
    {
        if (k < 0 || (size_t)k >= choices.size() || !choices[(size_t)k].package) return;
        const FxPackage p = choices[(size_t)k].pack;
        if (!PluginChooser::folderNameForTests && QMessageBox::question(q, "Delete Package", QString("Delete the package '%1'?").arg(qs(p.name))) != QMessageBox::Yes) return;
        std::string error;
        if (!deletePackage(p, &error)) QMessageBox::warning(q, "Delete Package", qs(error));
        loadChoices();
        fillList();
    }

    void deleteAsked()
    {
        if (QListWidgetItem* it = list->currentItem()) {
            if (it->isSelected()) {
                const QString key = it->data(kRoleFolder).toString();
                const int k = it->data(kRoleChoice).toInt();
                if (key.startsWith("user:")) { deleteFolderAsked(key.mid(5)); return; }
                if (k >= 0 && (size_t)k < choices.size() && choices[(size_t)k].package) { deletePackageAsked(k); return; }
                // a plug-in in one of the user's folders: removed from the folder (the plug-in itself stays
                // everywhere else)
                if (k >= 0 && currentKey().startsWith("user:")) { putInFolder(k, currentKey().mid(5), false); return; }
                if (k >= 0) return;
            }
        }
        if (currentKey().startsWith("user:")) deleteFolderAsked(currentKey().mid(5));
    }
    bool canDelete() const
    {
        if (QListWidgetItem* it = list->currentItem())
            if (it->isSelected()) {
                const int k = it->data(kRoleChoice).toInt();
                if (it->data(kRoleFolder).toString().startsWith("user:")) return true;
                if (k >= 0 && (size_t)k < choices.size() && choices[(size_t)k].package) return true;
            }
        return currentKey().startsWith("user:");
    }

    void folderMenu(const QString& key, const QPoint& global)
    {
        QMenu menu;
        QAction* create = menu.addAction("Create &New Folder");
        QAction* rename = menu.addAction("&Rename");
        QAction* del = menu.addAction("&Delete");
        const bool user = key.startsWith("user:");
        rename->setEnabled(user);
        del->setEnabled(user);
        QAction* got = PluginChooser::menuForTests ? PluginChooser::menuForTests(&menu) : menu.exec(global);
        if (got == create) newFolderAsked();
        else if (got == rename && user) renameFolderAsked(key.mid(5));
        else if (got == del && user) deleteFolderAsked(key.mid(5));
    }

    bool select(const QString& name)
    {
        QTreeWidgetItem* found = folderItem(name);
        if (!found) {
            QTreeWidgetItemIterator again(folders);
            for (; *again; ++again)
                if ((*again)->text(0) == name) { found = *again; break; }
        }
        if (!found && (name.compare("All", Qt::CaseInsensitive) == 0 || name.isEmpty()) && folders->topLevelItemCount()) found = folders->topLevelItem(0);
        if (!found) return false;
        if (folders->currentItem() == found) fillList();
        else folders->setCurrentItem(found);
        return true;
    }

    bool inFolder(const Choice& c, const QString& key) const
    {
        if (key.startsWith("user:")) {
            const QString ck = choiceKey(c);
            for (const UserFolder& u : userFolders) if (u.name == key.mid(5)) return u.items.contains(ck);
            return false;
        }
        if (key == "packages") return c.package;
        if (c.package) return false;
        if (key == "all" || key == "ofx") return true;
        if (key == "vegas") return c.vegas;
        if (key == "third") return !c.vegas;
        if (key.startsWith("group:")) return c.group == key.mid(6);
        if (key.startsWith("format:")) return c.format == key.mid(7);
        return false;
    }

    std::vector<UserFolder> userFolders;

    bool iconView() const { return list->viewMode() == QListView::IconMode; }

    // The folder's subfolders, then its plug-ins (packages too in the top one, after the plug-ins), flowing down the
    // columns as VEGAS lists them. A search lists only the plug-ins (in this folder) that match.
    void fillList()
    {
        userFolders = loadUserFolders(kind);
        QTreeWidgetItem* cur = folders->currentItem();
        const QString key = currentKey();
        const QStringList words = search->text().split(' ', Qt::SkipEmptyParts);
        const QSignalBlocker block(list);
        list->clear();
        const QFontMetrics fm(list->font());
        int widest = 0;
        const int iconSize = iconView() ? 32 : 16;
        if (cur && words.isEmpty())
            for (int k = 0; k < cur->childCount(); ++k) {
                QTreeWidgetItem* f = cur->child(k);
                QListWidgetItem* it = new QListWidgetItem(f->text(0), list);
                it->setData(kRoleChoice, -1);
                it->setData(kRoleFolder, f->data(0, kRoleFolder));
                it->setIcon(f->data(0, kRoleFolder).toString() == "packages" ? icon(Pic::Package, iconSize) : icon(Pic::Folder, iconSize));
                it->setToolTip(QString("Folder: %1 (double-click to open it)").arg(f->text(0)));
                it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                widest = std::max(widest, fm.horizontalAdvance(f->text(0)));
            }
        const QIcon plug = iconView() ? icon(pluginPic(), iconSize) : icon(Pic::Badge, iconSize), pack = icon(Pic::Package, iconSize);
        for (int pass = 0; pass < 2; ++pass)
            for (size_t k = 0; k < choices.size(); ++k) {
                const Choice& c = choices[k];
                if (c.package != (pass == 1)) continue;
                if (!(inFolder(c, key) || (key == "all" && c.package))) continue;
                bool match = true;
                for (const QString& w : words)
                    if (!c.label.contains(w, Qt::CaseInsensitive) && !c.group.contains(w, Qt::CaseInsensitive) && !c.vendor.contains(w, Qt::CaseInsensitive)) match = false;
                if (!match) continue;
                QListWidgetItem* it = new QListWidgetItem(c.label, list);
                it->setData(kRoleChoice, (int)k);
                it->setToolTip(c.tip);
                it->setIcon(c.package ? pack : plug);
                if (!c.works) {
                    it->setFlags(Qt::ItemIsEnabled);
                    it->setForeground(QColor(130, 130, 134));
                } else {
                    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled);
                }
                widest = std::max(widest, fm.horizontalAdvance(c.label));
            }
        if (!iconView()) list->setGridSize(QSize(std::min(std::max(widest + iconSize + 22, 140), 300), std::max(fm.height() + 3, 18)));
        upBtn->setEnabled(cur && cur->parent());
        {
            const QSignalBlocker b(path);
            const int at = path->findData(key);
            if (at >= 0) path->setCurrentIndex(at);
        }
        if (list->count()) {
            int first = 0;
            while (first < list->count() && list->item(first)->data(kRoleChoice).toInt() < 0) ++first;
            list->setCurrentRow(first < list->count() ? first : 0);
        }
        showInfo();
        updateButtons();
    }

    const Choice* current() const
    {
        QListWidgetItem* it = list->currentItem();
        if (!it || !it->isSelected()) return nullptr;
        const int k = it->data(kRoleChoice).toInt();
        return k >= 0 && (size_t)k < choices.size() ? &choices[(size_t)k] : nullptr;
    }

    void showInfo()
    {
        const Choice* c = current();
        info->setText(c ? c->tip.section('\n', 0, 1).replace('\n', QString::fromUtf8("  \xe2\x80\x94  ")) : QString());
    }

    void remember()
    {
        undos.push_back({chain, chainSel});
        if (undos.size() > 100) undos.erase(undos.begin());
        redos.clear();
    }
    void undoChain()
    {
        if (undos.empty()) return;
        redos.push_back({chain, chainSel});
        chain = undos.back().chain;
        chainSel = undos.back().sel;
        undos.pop_back();
        syncChain();
    }
    void redoChain()
    {
        if (redos.empty()) return;
        undos.push_back({chain, chainSel});
        chain = redos.back().chain;
        chainSel = redos.back().sel;
        redos.pop_back();
        syncChain();
    }

    void addChoice(int k, int at)
    {
        if (k < 0 || (size_t)k >= choices.size() || !choices[(size_t)k].works) return;
        const Choice& c = choices[(size_t)k];
        if (single) {
            chain = FxChain(c.plugins.begin(), c.plugins.begin() + std::min<size_t>(1, c.plugins.size()));
            chainSel = chain.empty() ? -1 : 0;
            syncChain();
            return;
        }
        if (c.plugins.empty()) return;
        remember();
        if (at < 0 || at > (int)chain.size()) at = (int)chain.size();
        for (const FxSlot& s : c.plugins) {
            FxSlot n = s;
            n.id = 0;
            chain.insert(chain.begin() + at, n);
            chainSel = at++;
        }
        syncChain();
    }

    void syncChain()
    {
        for (ChainChip* c : chips) {
            chainRow->row->removeWidget(c);
            c->hide();
            c->deleteLater();
        }
        chips.clear();
        if (chainSel >= (int)chain.size()) chainSel = (int)chain.size() - 1;
        for (size_t k = 0; k < chain.size(); ++k) {
            const FxSlot& s = chain[k];
            ChainChip* chip = new ChainChip(chainRow);
            chip->pos = (int)k;
            chip->setText(qs(fxSlotLabel(s)));
            chip->setIcon(icon(s.kind == "audio" ? Pic::Audio : pluginPic(), 14));
            chip->setChecked((int)k == chainSel);
            const int pos = (int)k;
            chip->onPick = [this, pos] {
                chainSel = pos;
                for (ChainChip* c : chips) c->setChecked(c->pos == pos);
                updateButtons();
            };
            chip->onMenu = [this, chip](const QPoint& at) { chipMenu(chip->mapToGlobal(at)); };
            chainRow->row->insertWidget((int)k, chip);
            chip->show();
            chips.push_back(chip);
        }
        chainRow->empty->setVisible(chain.empty());
        updateButtons();
    }

    void chipMenu(const QPoint& global)
    {
        QMenu menu;
        QAction* left = menu.addAction("Move Plug-In &Left");
        QAction* right = menu.addAction("Move Plug-In &Right");
        menu.addSeparator();
        QAction* remove = menu.addAction("&Remove");
        left->setEnabled(chainSel > 0);
        right->setEnabled(chainSel >= 0 && chainSel < (int)chain.size() - 1);
        QAction* got = PluginChooser::menuForTests ? PluginChooser::menuForTests(&menu) : menu.exec(global);
        if (got == left) moveSel(-1);
        else if (got == right) moveSel(1);
        else if (got == remove) removeSel();
    }

    void moveSel(int delta)
    {
        const int to = chainSel + delta;
        if (chainSel < 0 || to < 0 || to >= (int)chain.size()) return;
        remember();
        std::swap(chain[(size_t)chainSel], chain[(size_t)to]);
        chainSel = to;
        syncChain();
    }

    void moveTo(int from, int gap)
    {
        if (from < 0 || from >= (int)chain.size()) return;
        int to = gap > from ? gap - 1 : gap;
        to = std::max(0, std::min(to, (int)chain.size() - 1));
        if (to == from) return;
        remember();
        FxSlot s = chain[(size_t)from];
        chain.erase(chain.begin() + from);
        chain.insert(chain.begin() + to, s);
        chainSel = to;
        syncChain();
    }

    void removeSel()
    {
        if (chainSel < 0 || chainSel >= (int)chain.size()) return;
        remember();
        chain.erase(chain.begin() + chainSel);
        if (chainSel >= (int)chain.size()) chainSel = (int)chain.size() - 1;
        syncChain();
    }

    void updateButtons()
    {
        const Choice* c = current();
        addBtn->setEnabled(c && c->works);
        deleteBtn->setEnabled(canDelete());
        if (single) {
            okBtn->setEnabled(c && c->works && !c->package ? true : (c && c->package && !c->plugins.empty()));
            return;
        }
        removeBtn->setEnabled(chainSel >= 0);
        leftBtn->setEnabled(chainSel > 0);
        rightBtn->setEnabled(chainSel >= 0 && chainSel < (int)chain.size() - 1);
        saveBtn->setEnabled(!chain.empty());
        undoBtn->setEnabled(!undos.empty());
        redoBtn->setEnabled(!redos.empty());
    }

    void saveAs()
    {
        QStringList names;
        for (const FxPackage& p : listPackages(packageKindOf(kind))) names << qs(p.name);
        QString name;
        if (PluginChooser::packageNameForTests) {
            name = PluginChooser::packageNameForTests(names);
        } else {
            QDialog dlg(q);
            dlg.setObjectName("savePackageDialog");
            dlg.setWindowTitle("Save Plug-In Package");
            QVBoxLayout* v = new QVBoxLayout(&dlg);
            QHBoxLayout* h = new QHBoxLayout();
            QLabel* l = new QLabel("&Name:", &dlg);
            QComboBox* box = new QComboBox(&dlg);
            box->setObjectName("packageName");
            box->setEditable(true);
            box->addItems(names);
            box->setCurrentText(names.contains("New Package") ? QString() : QString("New Package"));
            box->setMinimumWidth(260);
            l->setBuddy(box);
            h->addWidget(l);
            h->addWidget(box, 1);
            v->addLayout(h);
            QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            v->addWidget(bb);
            if (dlg.exec() != QDialog::Accepted) return;
            name = box->currentText().trimmed();
            if (names.contains(name) &&
                QMessageBox::question(q, "Save Plug-In Package", QString("The package '%1' already exists in the packages folder. Do you want to replace it?").arg(name)) != QMessageBox::Yes)
                return;
        }
        name = name.trimmed();
        if (name.isEmpty()) return;
        std::string error;
        if (!q->savePackageAs(name, &error) && !PluginChooser::packageNameForTests) QMessageBox::warning(q, "Save Plug-In Package", qs(error));
    }

    void listMenu(const QPoint& at)
    {
        QListWidgetItem* it = list->itemAt(at);
        if (!it) {
            folderMenu(currentKey(), list->viewport()->mapToGlobal(at));
            return;
        }
        const int k = it->data(kRoleChoice).toInt();
        if (k < 0) {
            folderMenu(it->data(kRoleFolder).toString(), list->viewport()->mapToGlobal(at));
            return;
        }
        if ((size_t)k >= choices.size()) return;
        const Choice c = choices[(size_t)k];
        const QString key = currentKey();
        QMenu menu;
        QMenu* to = menu.addMenu("Add to &Folder");
        std::vector<std::pair<QAction*, QString>> targets;
        for (const UserFolder& u : loadUserFolders(kind)) targets.push_back({to->addAction(u.name), u.name});
        if (targets.empty()) to->addAction("(no folders yet: Create New Folder makes one)")->setEnabled(false);
        QAction* out = nullptr;
        if (key.startsWith("user:")) out = menu.addAction("Remove from This Folder");
        QAction *rename = nullptr, *del = nullptr;
        if (c.package) {
            menu.addSeparator();
            rename = menu.addAction("&Rename Package");
            del = menu.addAction("Delete Package");
        }
        QAction* got = PluginChooser::menuForTests ? PluginChooser::menuForTests(&menu) : menu.exec(list->viewport()->mapToGlobal(at));
        if (!got) return;
        for (const auto& t : targets)
            if (got == t.first) { putInFolder(k, t.second, true); return; }
        if (got == out) { putInFolder(k, key.mid(5), false); return; }
        if (got == rename) {
            const QString n = PluginChooser::folderNameForTests ? PluginChooser::folderNameForTests(c.label)
                                                                : QInputDialog::getText(q, "Rename Package", "Package name:", QLineEdit::Normal, c.label);
            std::string error;
            if (!n.trimmed().isEmpty() && n.trimmed() != c.label && !renamePackage(k, n, &error) && !error.empty()) QMessageBox::warning(q, "Rename Package", qs(error));
            return;
        }
        if (got == del) deletePackageAsked(k);
    }

    // A double-click or Enter in the list: a folder opens, a plug-in goes on the end of the chain (a transition or
    // generator is the one picked, and the window closes).
    void activated(QListWidgetItem* it)
    {
        if (!it) return;
        const int k = it->data(kRoleChoice).toInt();
        if (k < 0) {
            select(it->data(kRoleFolder).toString());
            return;
        }
        addChoice(k, -1);
        if (single && !chain.empty()) q->accept();
    }

    void setIconView(bool icons)
    {
        applyView(icons);
        fillList();
    }
    void applyView(bool icons)
    {
        list->setViewMode(icons ? QListView::IconMode : QListView::ListMode);
        list->setFlow(icons ? QListView::LeftToRight : QListView::TopToBottom);
        list->setWrapping(true);
        list->setIconSize(icons ? QSize(32, 32) : QSize(16, 16));
        list->setGridSize(icons ? QSize(112, 72) : QSize());
        list->setWordWrap(icons);
        list->setResizeMode(QListView::Adjust);
        list->setMovement(QListView::Static);
        list->setHorizontalScrollBarPolicy(icons ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOn);
        list->setVerticalScrollBarPolicy(icons ? Qt::ScrollBarAsNeeded : Qt::ScrollBarAlwaysOff);
        list->setDragEnabled(!single);
    }
};

class FolderTree : public QTreeWidget {
public:
    std::function<void(int choice, const QString& folder)> onDrop;
    explicit FolderTree(QWidget* parent) : QTreeWidget(parent)
    {
        setAcceptDrops(true);
        setDragDropMode(QAbstractItemView::DropOnly);
    }
protected:
    QString folderAt(const QPoint& at) const
    {
        QTreeWidgetItem* it = itemAt(at);
        const QString key = it ? it->data(0, kRoleFolder).toString() : QString();
        return key.startsWith("user:") ? key.mid(5) : QString();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMimeChoice)) e->acceptProposedAction();
        else e->ignore();
    }
    void dragMoveEvent(QDragMoveEvent* e) override
    {
        if (e->mimeData()->hasFormat(kMimeChoice) && !folderAt(e->position().toPoint()).isEmpty()) e->acceptProposedAction();
        else e->ignore();
    }
    void dropEvent(QDropEvent* e) override
    {
        const QString f = folderAt(e->position().toPoint());
        if (f.isEmpty() || !e->mimeData()->hasFormat(kMimeChoice)) { e->ignore(); return; }
        if (onDrop) onDrop(e->mimeData()->data(kMimeChoice).toInt(), f);
        e->acceptProposedAction();
    }
};

PluginChooser::PluginChooser(ChooserKind kind, const QString& title, const FxChain& chain, QWidget* parent) : QDialog(parent), d(new Impl())
{
    d->q = this;
    d->kind = kind;
    d->single = kind == ChooserKind::Transition || kind == ChooserKind::Generator;
    if (!d->single) d->chain = chain;
    d->chainSel = d->chain.empty() ? -1 : 0;
    setObjectName("PluginChooser");
    setWindowTitle(title);
    setSizeGripEnabled(true);
    setStyleSheet(
        "QWidget#chooserChainBox { background: palette(dark); border: 1px solid palette(shadow); }"
        "QLabel#chooserChainEmpty { color: #b4b4b8; }"
        "QToolButton#chooserChainItem { padding: 1px 6px; }");

    QGridLayout* g = new QGridLayout(this);
    g->setContentsMargins(6, 6, 6, 6);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(5);

    d->chainBox = new QWidget(this);
    d->chainBox->setObjectName("chooserChainBox");
    d->chainBox->setAttribute(Qt::WA_StyledBackground);
    QHBoxLayout* ch = new QHBoxLayout(d->chainBox);
    ch->setContentsMargins(1, 1, 2, 1);
    ch->setSpacing(1);
    QScrollArea* chainScroll = new QScrollArea(d->chainBox);
    chainScroll->setObjectName("chooserChainScroll");
    chainScroll->setWidgetResizable(true);
    chainScroll->setFrameShape(QFrame::NoFrame);
    chainScroll->setFixedHeight(28);
    chainScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    chainScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    chainScroll->viewport()->setAutoFillBackground(false);
    d->chainRow = new ChainRow(chainScroll);
    d->chainRow->setAutoFillBackground(false);
    chainScroll->setWidget(d->chainRow);
    ch->addWidget(chainScroll, 1);
    d->leftBtn = toolButton(d->chainBox, Pic::Left, "Move Plug-In Left", "chooserLeft");
    d->rightBtn = toolButton(d->chainBox, Pic::Right, "Move Plug-In Right", "chooserRight");
    d->undoBtn = toolButton(d->chainBox, Pic::Undo, "Undo (Ctrl+Z)", "chooserUndo");
    d->redoBtn = toolButton(d->chainBox, Pic::Redo, "Redo (Ctrl+Y)", "chooserRedo");
    for (QToolButton* b : {d->leftBtn, d->rightBtn, d->undoBtn, d->redoBtn}) ch->addWidget(b);
    g->addWidget(d->chainBox, 0, 0);
    d->chainBox->setVisible(!d->single);

    QHBoxLayout* tools = new QHBoxLayout();
    tools->setSpacing(1);
    d->path = new QComboBox(this);
    d->path->setObjectName("chooserPath");
    d->path->setToolTip("The folder shown");
    d->path->setMaxVisibleItems(24);
    tools->addWidget(d->path, 1);
    tools->addSpacing(4);
    d->upBtn = toolButton(this, Pic::Up, "Up One Level (Backspace)", "chooserUp");
    d->newFolderBtn = toolButton(this, Pic::NewFolder, "Create New Folder", "chooserNewFolder");
    d->deleteBtn = toolButton(this, Pic::Delete, "Delete (a folder or package of your own)", "chooserDelete");
    d->viewBtn = toolButton(this, Pic::Views, "Views: the list in columns, or large icons", "chooserView");
    d->viewBtn->setCheckable(true);
    tools->addWidget(d->upBtn);
    tools->addWidget(d->newFolderBtn);
    tools->addWidget(d->deleteBtn);
    QFrame* sep = new QFrame(this);
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Sunken);
    sep->setFixedHeight(16);
    tools->addSpacing(2);
    tools->addWidget(sep);
    tools->addSpacing(2);
    tools->addWidget(d->viewBtn);
    tools->addSpacing(6);
    d->search = new QLineEdit(this);
    d->search->setObjectName("chooserSearch");
    d->search->setPlaceholderText("Search");
    d->search->setClearButtonEnabled(true);
    d->search->setFixedWidth(170);
    tools->addWidget(d->search);
    g->addLayout(tools, 1, 0);

    QSplitter* split = new QSplitter(Qt::Horizontal, this);
    split->setObjectName("chooserSplit");
    split->setChildrenCollapsible(false);
    FolderTree* tree = new FolderTree(split);
    d->folders = tree;
    d->folders->setObjectName("chooserFolders");
    d->folders->setContextMenuPolicy(Qt::CustomContextMenu);
    d->folders->setHeaderHidden(true);
    d->folders->setIndentation(14);
    d->folders->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    d->folders->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    d->folders->header()->setStretchLastSection(false);
    d->list = new ChoiceList(split);
    d->list->setObjectName("chooserList");
    d->list->setSelectionMode(QAbstractItemView::SingleSelection);
    d->list->setUniformItemSizes(true);
    d->list->setContextMenuPolicy(Qt::CustomContextMenu);
    d->list->setTextElideMode(Qt::ElideRight);
    d->list->setSpacing(0);
    d->applyView(false);
    split->addWidget(d->folders);
    split->addWidget(d->list);
    split->setStretchFactor(0, 0);
    split->setStretchFactor(1, 1);
    split->setSizes({150, 1000});
    g->addWidget(split, 2, 0);

    d->info = new QLabel(this);
    d->info->setObjectName("chooserInfo");
    d->info->setTextFormat(Qt::PlainText);
    d->info->setMinimumHeight(20);
    g->addWidget(d->info, 3, 0);

    auto button = [this](const QString& text, const char* name) {
        QPushButton* b = new QPushButton(text, this);
        b->setObjectName(name);
        b->setAutoDefault(false);
        b->setMinimumWidth(78);
        return b;
    };
    d->okBtn = button("OK", "chooserOk");
    d->okBtn->setDefault(true);
    QPushButton* cancel = button("Cancel", "chooserCancel");
    d->addBtn = button("&Add", "chooserAdd");
    d->removeBtn = button("&Remove", "chooserRemove");
    d->removeBtn->setToolTip("Remove the plug-in picked in the chain");
    d->saveBtn = button("Save &As...", "chooserSaveAs");
    d->saveBtn->setToolTip("Save Plug-In Package As...");
    d->lutBtn = button("Save &LUT...", "chooserSaveLut");
    d->lutBtn->setToolTip("Save Plug-In Chain as LUT file... (VEGAS writes the chain's colour changes as a look-up table here; Reno cannot do that yet)");
    d->lutBtn->setEnabled(false);
    g->addWidget(d->okBtn, 0, 1, Qt::AlignTop);
    g->addWidget(cancel, 1, 1, Qt::AlignTop);
    QVBoxLayout* side = new QVBoxLayout();
    side->setSpacing(4);
    side->addSpacing(14);
    side->addWidget(d->addBtn);
    side->addWidget(d->removeBtn);
    side->addWidget(d->saveBtn);
    side->addWidget(d->lutBtn);
    side->addStretch(1);
    g->addLayout(side, 2, 1);
    g->setColumnStretch(0, 1);
    g->setRowStretch(2, 1);
    for (QWidget* w : std::initializer_list<QWidget*>{d->addBtn, d->removeBtn, d->saveBtn}) w->setVisible(!d->single);
    d->lutBtn->setVisible(kind == ChooserKind::VideoFx);

    Impl* im = d.get();
    QObject::connect(d->folders, &QTreeWidget::currentItemChanged, this, [im](QTreeWidgetItem*, QTreeWidgetItem*) { im->fillList(); });
    QObject::connect(d->path, QOverload<int>::of(&QComboBox::activated), this, [im](int at) { im->select(im->path->itemData(at).toString()); });
    QObject::connect(d->search, &QLineEdit::textChanged, this, [im](const QString&) { im->fillList(); });
    QObject::connect(d->list, &QListWidget::itemSelectionChanged, this, [im] { im->showInfo(); im->updateButtons(); });
    QObject::connect(d->list, &QListWidget::itemActivated, this, [im](QListWidgetItem* it) { im->activated(it); });
    QObject::connect(d->list, &QListWidget::customContextMenuRequested, this, [im](const QPoint& at) { im->listMenu(at); });
    QObject::connect(d->folders, &QTreeWidget::customContextMenuRequested, this, [im](const QPoint& at) {
        QTreeWidgetItem* it = im->folders->itemAt(at);
        im->folderMenu(it ? it->data(0, kRoleFolder).toString() : QString(), im->folders->viewport()->mapToGlobal(at));
    });
    tree->onDrop = [im](int choice, const QString& folder) { im->putInFolder(choice, folder, true); };
    QObject::connect(d->upBtn, &QToolButton::clicked, this, [im] {
        QTreeWidgetItem* it = im->folders->currentItem();
        if (it && it->parent()) im->folders->setCurrentItem(it->parent());
    });
    QObject::connect(d->newFolderBtn, &QToolButton::clicked, this, [im] { im->newFolderAsked(); });
    QObject::connect(d->deleteBtn, &QToolButton::clicked, this, [im] { im->deleteAsked(); });
    QObject::connect(d->viewBtn, &QToolButton::toggled, this, [im](bool icons) { im->setIconView(icons); });
    QObject::connect(d->addBtn, &QPushButton::clicked, this, [im] {
        QListWidgetItem* it = im->list->currentItem();
        if (it && it->data(kRoleChoice).toInt() >= 0) im->addChoice(it->data(kRoleChoice).toInt(), -1);
    });
    QObject::connect(d->removeBtn, &QPushButton::clicked, this, [im] { im->removeSel(); });
    QObject::connect(d->saveBtn, &QPushButton::clicked, this, [im] { im->saveAs(); });
    QObject::connect(d->leftBtn, &QToolButton::clicked, this, [im] { im->moveSel(-1); });
    QObject::connect(d->rightBtn, &QToolButton::clicked, this, [im] { im->moveSel(1); });
    QObject::connect(d->undoBtn, &QToolButton::clicked, this, [im] { im->undoChain(); });
    QObject::connect(d->redoBtn, &QToolButton::clicked, this, [im] { im->redoChain(); });
    QObject::connect(d->okBtn, &QPushButton::clicked, this, [im] {
        if (im->single) {
            const Choice* c = im->current();
            if (!c || !c->works) return;
            im->chain.clear();
            if (!c->plugins.empty()) im->chain.push_back(c->plugins[0]);
        }
        im->q->accept();
    });
    QObject::connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
    d->chainRow->onDropChoice = [im](int choice, int at) { im->addChoice(choice, at); };
    d->chainRow->onDropMove = [im](int from, int at) { im->moveTo(from, at); };
    auto shortcut = [this](const QKeySequence& k, std::function<void()> fn) {
        QAction* a = new QAction(this);
        a->setShortcut(k);
        a->setShortcutContext(Qt::WindowShortcut);
        QObject::connect(a, &QAction::triggered, this, [fn] { fn(); });
        addAction(a);
    };
    if (!d->single) {
        shortcut(QKeySequence::Undo, [im] { im->undoChain(); });
        shortcut(QKeySequence::Redo, [im] { im->redoChain(); });
        shortcut(QKeySequence(Qt::CTRL | Qt::Key_Y), [im] { im->redoChain(); });
    }
    shortcut(QKeySequence(Qt::Key_Backspace), [im] {
        if (im->search->hasFocus()) return;
        im->upBtn->click();
    });
    shortcut(QKeySequence(Qt::Key_Delete), [im] {
        if (im->search->hasFocus()) return;
        if (im->list->hasFocus() || im->folders->hasFocus()) im->deleteAsked();
        else im->removeSel();
    });

    d->loadChoices();
    d->buildFolders();
    d->select("all");
    d->syncChain();

    QSize want(1340, 760);
    const QSize saved = QSettings().value("pluginChooser/size").toSize();
    if (saved.isValid() && saved.width() >= 400 && saved.height() >= 300) want = saved;
    if (QScreen* screen = parent ? parent->screen() : QGuiApplication::primaryScreen())
        want = want.boundedTo(screen->availableGeometry().size() - QSize(40, 80));
    resize(want);
    QObject::connect(this, &QDialog::finished, this, [this](int) { QSettings().setValue("pluginChooser/size", size()); });
}

PluginChooser::~PluginChooser() {}

void PluginChooser::showEvent(QShowEvent* e)
{
    QDialog::showEvent(e);
    if (onShown) {
        QPointer<PluginChooser> self(this);
        QMetaObject::invokeMethod(this, [self] {
            const std::function<void(PluginChooser*)> fn = onShown;
            if (self && fn) fn(self);
        }, Qt::QueuedConnection);
    }
}

FxChain PluginChooser::chain() const { return d->chain; }

bool PluginChooser::picked(FxSlot* out) const
{
    if (d->chain.empty()) return false;
    if (out) *out = d->chain[0];
    return true;
}

bool PluginChooser::openFolder(const QString& name) { return d->select(name); }
bool PluginChooser::createFolder(const QString& name) { return d->createFolder(name); }
bool PluginChooser::renameFolder(const QString& from, const QString& to) { return d->renameFolder(from, to); }
bool PluginChooser::deleteFolder(const QString& name) { return d->deleteFolder(name); }
QTreeWidget* PluginChooser::folderTree() const { return d->folders; }
QListWidget* PluginChooser::choiceList() const { return d->list; }

bool PluginChooser::addToFolder(const QString& label, const QString& folder)
{
    for (size_t k = 0; k < d->choices.size(); ++k)
        if (d->choices[k].label == label) return d->putInFolder((int)k, folder, true);
    return false;
}

QStringList PluginChooser::listed() const
{
    QStringList out;
    for (int k = 0; k < d->list->count(); ++k)
        if (d->list->item(k)->data(kRoleChoice).toInt() >= 0) out << d->list->item(k)->text();
    return out;
}

QStringList PluginChooser::listedFolders() const
{
    QStringList out;
    for (int k = 0; k < d->list->count(); ++k)
        if (d->list->item(k)->data(kRoleChoice).toInt() < 0) out << d->list->item(k)->text();
    return out;
}

QStringList PluginChooser::folders() const
{
    QStringList out;
    QTreeWidgetItemIterator it(d->folders);
    for (; *it; ++it) out << (*it)->text(0);
    return out;
}

bool PluginChooser::addByLabel(const QString& label)
{
    for (int k = 0; k < d->list->count(); ++k)
        if (d->list->item(k)->text() == label && d->list->item(k)->data(kRoleChoice).toInt() >= 0) {
            d->list->setCurrentRow(k);
            if (d->single) {
                d->addChoice(d->list->item(k)->data(kRoleChoice).toInt(), -1);
                return true;
            }
            d->addBtn->click();
            return true;
        }
    return false;
}

bool PluginChooser::savePackageAs(const QString& name, std::string* error)
{
    FxPackage p;
    p.name = s8(name);
    p.kind = packageKindOf(d->kind);
    p.chain = d->chain;
    if (p.chain.empty()) {
        if (error) *error = "No plug-ins in chain";
        return false;
    }
    if (!savePackage(p, error)) return false;
    d->loadChoices();
    d->buildFolders();
    return true;
}


ChooserKind chooserKindFor(const FxTarget& t)
{
    return AppState::get().targetIsVideo(t) ? ChooserKind::VideoFx : ChooserKind::AudioFx;
}

QString chainTitle(const FxTarget& t)
{
    AppState& app = AppState::get();
    const bool video = app.targetIsVideo(t);
    switch (t.kind) {
    case FxTarget::Event: return video ? "Video Event FX" : "Audio Event FX";
    case FxTarget::Track: return video ? "Video Track FX" : "Audio Track FX";
    case FxTarget::VideoOut: return "Video Output FX";
    case FxTarget::AudioOut: return "Master Bus FX";
    case FxTarget::Bus: return "Bus FX";
    default: return "FX";
    }
}

uint64_t applyChain(const FxTarget& target, const FxChain& chosen, const std::string& what)
{
    AppState& app = AppState::get();
    uint64_t first = 0;
    app.edit(what, [&](Project& p) {
        FxChain* c = app.chainOf(p, target);
        if (!c) return;
        FxChain made;
        for (const FxSlot& s : chosen) {
            const FxSlot* kept = nullptr;
            if (s.id)
                for (const FxSlot& o : *c)
                    if (o.id == s.id) kept = &o;
            if (kept) {
                made.push_back(*kept);
            } else {
                FxSlot n = s;
                n.id = p.newId();
                n.veg.reset();
                if (!first) first = n.id;
                made.push_back(n);
            }
        }
        // an event's effects applied before Event Pan/Crop stay at the start of its chain, as in VEGAS
        if (target.kind == FxTarget::Event)
            std::stable_partition(made.begin(), made.end(), [](const FxSlot& s) { return s.beforePanCrop && s.kind == "ofx"; });
        else
            for (FxSlot& s : made) s.beforePanCrop = false;
        *c = made;
    });
    return first;
}

uint64_t addChainToTarget(const FxTarget& target, const FxChain& add)
{
    AppState& app = AppState::get();
    const FxChain* c = app.chainOf(app.project(), target);
    if (!c || add.empty()) return 0;
    FxChain all = *c;
    for (FxSlot s : add) {
        s.id = 0;
        all.push_back(s);
    }
    return applyChain(target, all, add.size() == 1 ? "Add Effect" : "Add Plug-In Package");
}

bool choosePluginsFor(QWidget* parent, const FxTarget& target, uint64_t* firstNew)
{
    AppState& app = AppState::get();
    const FxChain* c = app.chainOf(app.project(), target);
    if (!c) return false;
    FxChain now = *c;
    for (FxSlot& s : now) {
        const Json live = app.fxLiveState(s.id);
        if (!live.isNull()) s.state = live;
    }
    const QString name = qs(app.targetName(target));
    const int colon = (int)name.indexOf(": ");
    const QString title = QString("Plug-In Chooser - %1").arg(chainTitle(target)) + (colon >= 0 ? QString(": ") + name.mid(colon + 2) : QString());
    PluginChooser dlg(chooserKindFor(target), title, now, parent);
    if (dlg.exec() != QDialog::Accepted) return false;
    // the chain may have changed while the window was up (an undo, an auto-save...): it is applied to what is there now
    const uint64_t first = applyChain(target, dlg.chain());
    if (firstNew) *firstNew = first;
    return true;
}

uint64_t makeGeneratedMedia(const FxSlot& s)
{
    if (s.pluginId.empty()) return 0;
    AppState& app = AppState::get();
    uint64_t id = 0;
    app.edit("Add Generated Media", [&](Project& p) {
        MediaItem m;
        m.id = id = p.newId();
        m.name = fxSlotLabel(s);
        m.generated = true;
        m.generator = s;
        m.generator.id = p.newId();
        m.generator.kind = "ofx";
        m.generator.bypass = false;
        m.generator.veg.reset();
        m.info.hasVideo = true;
        m.info.width = p.settings.width;
        m.info.height = p.settings.height;
        m.info.fps = p.settings.fps();
        p.media.push_back(m);
    });
    return id;
}

uint64_t makeGeneratedMediaFromData(const std::string& data)
{
    FxPackage p;
    if (!isFxPackageData(data) || !packageFromText(data, p) || p.chain.empty()) return 0;
    return makeGeneratedMedia(p.chain.front());
}

uint64_t chooseGeneratedMedia(QWidget* parent)
{
    PluginChooser dlg(ChooserKind::Generator, "Plug-In Chooser - New Media Generator", FxChain(), parent);
    if (dlg.exec() != QDialog::Accepted) return 0;
    FxSlot s;
    if (!dlg.picked(&s)) return 0;
    return makeGeneratedMedia(s);
}

bool isFxPackageData(const std::string& data)
{
    return !data.empty() && data[0] == '{' && data.find("plug-in package") != std::string::npos;
}

uint64_t addFxData(const FxTarget& target, const std::string& data)
{
    if (!isFxPackageData(data)) return 0;
    FxPackage p;
    if (!packageFromText(data, p) || p.chain.empty()) return 0;
    AppState& app = AppState::get();
    if (target.kind == FxTarget::Transition) {
        const FxSlot& s = p.chain.front();
        const uint64_t ev = target.id;
        uint64_t made = 0;
        app.edit("Change Transition", [&](Project& pr) {
            if (Event* e = pr.findEvent(ev)) {
                e->transition = s;
                e->transition.id = made = pr.newId();
                e->transition.veg.reset();
            }
        });
        return made;
    }
    return addChainToTarget(target, p.chain);
}

bool pluginChooserFirst(QWidget* parent, const FxTarget& target, uint64_t* slotId)
{
    AppState& app = AppState::get();
    const FxChain* c = app.chainOf(app.project(), target);
    if (!c || !c->empty()) return false;
    uint64_t first = 0;
    if (!choosePluginsFor(parent, target, &first)) return true;
    if (slotId) *slotId = first;
    const FxChain* now = app.chainOf(app.project(), target);
    return !now || now->empty();
}

void insertGeneratedMedia(QWidget* parent, const std::function<void(const FxTarget&)>& openFx)
{
    AppState& app = AppState::get();
    const uint64_t media = chooseGeneratedMedia(parent);
    if (!media) return;
    uint64_t track = 0;
    for (const Track& t : app.project().tracks)
        if (t.id == app.currentTrack && t.type == TrackType::Video) track = t.id;
    std::set<uint64_t> sel;
    for (uint64_t e : app.addMediaToTimeline(media, app.cursor(), track)) sel.insert(e);
    if (!sel.empty()) app.setSelection(sel);
    FxTarget t;
    t.kind = FxTarget::Generator;
    t.id = media;
    if (openFx) openFx(t);
}

void fillChainMenu(QMenu* m, const FxTarget& target)
{
    AppState& app = AppState::get();
    const FxChain* c = app.chainOf(app.project(), target);
    const bool any = c && !c->empty();
    auto all = [target](const std::string& what, const std::function<void(FxChain&)>& fn) {
        AppState::get().edit(what, [&](Project& p) {
            if (FxChain* ch = AppState::get().chainOf(p, target)) fn(*ch);
        });
    };
    QAction* en = m->addAction("Enable All");
    en->setObjectName("chainEnableAll");
    en->setEnabled(any);
    QObject::connect(en, &QAction::triggered, m, [all] { all("Enable All Effects", [](FxChain& ch) { for (FxSlot& s : ch) s.bypass = false; }); });
    QAction* by = m->addAction("Bypass All");
    by->setObjectName("chainBypassAll");
    by->setEnabled(any);
    QObject::connect(by, &QAction::triggered, m, [all] { all("Bypass All Effects", [](FxChain& ch) { for (FxSlot& s : ch) s.bypass = true; }); });
    QAction* del = m->addAction("Delete All");
    del->setObjectName("chainDeleteAll");
    del->setEnabled(any);
    QObject::connect(del, &QAction::triggered, m, [all] { all("Delete All Effects", [](FxChain& ch) { ch.clear(); }); });
}

void fillAudioPluginMenu(QMenu* m, const std::function<void(const AudioPluginDesc&)>& chosen)
{
    std::map<QString, std::vector<AudioPluginDesc>> byMaker;
    for (const AudioFxEntry& e : Catalog::get().audio()) {
        if (!e.ok || e.desc.isInstrument) continue;
        const QString maker = e.desc.vendor.empty() ? QString("Other") : QString::fromStdString(e.desc.vendor);
        byMaker[maker].push_back(e.desc);
    }
    if (byMaker.empty()) {
        QAction* none = m->addAction("No audio plug-ins were found");
        none->setEnabled(false);
        return;
    }
    for (auto& g : byMaker) {
        std::sort(g.second.begin(), g.second.end(), [](const AudioPluginDesc& a, const AudioPluginDesc& b) { return a.name < b.name; });
        QMenu* sub = m->addMenu(g.first);
        for (const AudioPluginDesc& d : g.second) {
            QAction* a = sub->addAction(QString::fromStdString(d.name) + "  (" + QString::fromStdString(d.format) + ")");
            QObject::connect(a, &QAction::triggered, m, [chosen, d] { if (chosen) chosen(d); });
        }
    }
}

}  // namespace app
}  // namespace reno
