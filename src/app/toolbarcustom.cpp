// Options > Customize Toolbar and Customize Timeline Toolbar, as in VEGAS: available buttons at the left (every menu
// command and
// a Separator), the tool bar's buttons at the right, Add -> / <- Remove (or double-click), Move Up / Move Down, Reset
// (VEGAS's
// set) and Close. The tool bar changes as buttons move and is saved (QSettings "toolbars/main", "toolbars/timeline").
//
// Default sets are VEGAS 18's (manual pages 14 and 23-26): main bar New Empty Project, Open, Save, Render As,
// Properties, Cut,
// Copy, Paste, Undo, Redo, Interactive Tutorials, What's This Help; editing bar tools, Delete, Trim, Trim Start, Trim End.
#include "mainwindow.h"
#include <QAction>
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QSettings>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <algorithm>
#include <functional>
#include <map>

namespace reno {
namespace app {

QIcon commandIcon(const QString& name, const QString& text);

namespace {

QString plainText(QString t) { return t.split('\t')[0].remove('&').remove("..."); }

// The tool bar's buttons by name as they were made, including drop-down buttons (widget actions named after
// their buttons: "undoButton", "editToolButton"...).
struct Registry {
    std::map<QString, QAction*> widgets;
    QStringList factory;
};
std::map<QToolBar*, Registry>& registries()
{
    static std::map<QToolBar*, Registry> r;
    return r;
}

QString nameOf(QAction* a)
{
    if (a->isSeparator()) return "-";
    if (QWidgetAction* wa = qobject_cast<QWidgetAction*>(a)) {
        if (wa->defaultWidget() && !wa->defaultWidget()->objectName().isEmpty()) return wa->defaultWidget()->objectName();
    }
    return a->objectName();
}

const char* settingsKey(bool timeline) { return timeline ? "toolbars/timeline" : "toolbars/main"; }

QStringList vegasDefault(bool timeline)
{
    if (timeline)
        return {"-", "editToolButton", "toolEnvelope", "toolSelection", "toolZoom", "-", "delete", "trim", "trimStart", "trimEnd", "split", "smartSplit",
                "lockEvent", "-", "marker", "region", "-", "snapping", "autoCrossfade", "autoRippleButton", "lockEnvelopes", "ignoreGrouping", "-",
                "colorGrading", "nestParent", "nestOpen", "nestCreate", "-", "quantize", "-", "group", "ungroup", "clearGroup"};
    return {"newPlain", "open", "save", "render", "properties", "cut", "copy", "paste", "undoButton", "redoButton", "interactiveTutorials", "whatsThis"};
}

std::map<QString, QAction*> commands(QMainWindow* w, QToolBar* tb)
{
    std::map<QString, QAction*> all;
    std::function<void(QMenu*)> walk = [&](QMenu* m) {
        for (QAction* a : m->actions()) {
            if (a->isSeparator()) continue;
            if (a->menu()) { walk(a->menu()); continue; }
            if (a->objectName().isEmpty() || a->text().isEmpty()) continue;
            all.emplace(a->objectName(), a);
        }
    };
    for (QAction* a : w->menuBar()->actions())
        if (a->menu()) walk(a->menu());
    for (QAction* a : w->actions())
        if (!a->objectName().isEmpty() && !a->text().isEmpty() && !a->menu()) all.emplace(a->objectName(), a);
    for (const auto& kv : registries()[tb].widgets) all[kv.first] = kv.second;
    for (QAction* a : tb->actions())
        if (!a->isSeparator()) all.emplace(nameOf(a), a);
    return all;
}

void applyList(QMainWindow* w, QToolBar* tb, const QStringList& names)
{
    const std::map<QString, QAction*> all = commands(w, tb);
    for (QAction* a : tb->actions()) tb->removeAction(a);
    for (const QString& n : names) {
        if (n == "-") { tb->addSeparator(); continue; }
        auto it = all.find(n);
        if (it == all.end()) continue;
        if (!qobject_cast<QWidgetAction*>(it->second) && it->second->icon().isNull())
            it->second->setIcon(commandIcon(n, plainText(it->second->text())));
        tb->addAction(it->second);
    }
}

QStringList currentList(QToolBar* tb)
{
    QStringList l;
    for (QAction* a : tb->actions()) l << nameOf(a);
    return l;
}

QString label(QAction* a, const QString& name)
{
    if (name == "-") return "Separator";
    if (QWidgetAction* wa = qobject_cast<QWidgetAction*>(a))
        if (QToolButton* b = qobject_cast<QToolButton*>(wa->defaultWidget()))
            if (b->defaultAction()) return plainText(b->defaultAction()->text());
    QString t = plainText(a->text());
    return t.isEmpty() ? name : t;
}

QIcon iconOf(QAction* a)
{
    if (QWidgetAction* wa = qobject_cast<QWidgetAction*>(a))
        if (QToolButton* b = qobject_cast<QToolButton*>(wa->defaultWidget()))
            if (b->defaultAction()) return b->defaultAction()->icon();
    return a->icon();
}

}  // namespace

// Called once the tool bars and menus exist: remembers the tool bars' own buttons and applies the saved (or
// VEGAS's default) sets.
void installToolbars(QMainWindow* w)
{
    for (bool timeline : {false, true}) {
        QToolBar* tb = w->findChild<QToolBar*>(timeline ? "timelineToolBar" : "mainToolBar");
        if (!tb) continue;
        Registry& r = registries()[tb];
        for (QAction* a : tb->actions()) {
            r.factory << nameOf(a);
            if (qobject_cast<QWidgetAction*>(a)) r.widgets[nameOf(a)] = a;
        }
        const QStringList saved = QSettings().value(settingsKey(timeline)).toStringList();
        applyList(w, tb, saved.isEmpty() ? vegasDefault(timeline) : saved);
    }
}

void runCustomizeToolbar(QMainWindow* w, bool timeline)
{
    QToolBar* tb = w->findChild<QToolBar*>(timeline ? "timelineToolBar" : "mainToolBar");
    if (!tb) return;
    QDialog d(w);
    d.setObjectName(timeline ? "customizeTimelineToolbar" : "customizeToolbar");
    d.setWindowTitle(timeline ? "Customize Timeline Toolbar" : "Customize Toolbar");
    d.resize(620, 420);
    QHBoxLayout* h = new QHBoxLayout(&d);
    QVBoxLayout* left = new QVBoxLayout();
    left->addWidget(new QLabel("Available toolbar b&uttons:", &d));
    QListWidget* avail = new QListWidget(&d);
    avail->setObjectName("availableButtons");
    left->addWidget(avail, 1);
    h->addLayout(left, 1);
    QVBoxLayout* mid = new QVBoxLayout();
    mid->addStretch(1);
    QPushButton* add = new QPushButton("&Add ->", &d);
    add->setObjectName("addButton");
    QPushButton* remove = new QPushButton("<- &Remove", &d);
    remove->setObjectName("removeButton");
    mid->addWidget(add);
    mid->addWidget(remove);
    mid->addStretch(1);
    h->addLayout(mid);
    QVBoxLayout* right = new QVBoxLayout();
    right->addWidget(new QLabel("Current toolbar bu&ttons:", &d));
    QListWidget* cur = new QListWidget(&d);
    cur->setObjectName("currentButtons");
    right->addWidget(cur, 1);
    h->addLayout(right, 1);
    QVBoxLayout* buttons = new QVBoxLayout();
    QPushButton* close = new QPushButton("&Close", &d);
    close->setObjectName("close");
    close->setDefault(true);
    QPushButton* reset = new QPushButton("R&eset", &d);
    reset->setObjectName("reset");
    QPushButton* up = new QPushButton("Move &Up", &d);
    up->setObjectName("moveUp");
    QPushButton* down = new QPushButton("Move &Down", &d);
    down->setObjectName("moveDown");
    buttons->addWidget(close);
    buttons->addWidget(reset);
    buttons->addSpacing(20);
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addStretch(1);
    h->addLayout(buttons);

    auto fill = [w, tb, avail, cur] {
        const std::map<QString, QAction*> all = commands(w, tb);
        const QStringList now = currentList(tb);
        const int curRow = cur->currentRow(), availRow = avail->currentRow();
        cur->clear();
        for (const QString& n : now) {
            auto it = all.find(n);
            QListWidgetItem* item = new QListWidgetItem(n == "-" ? QString("Separator") : (it != all.end() ? label(it->second, n) : n), cur);
            if (it != all.end()) item->setIcon(iconOf(it->second));
            item->setData(Qt::UserRole, n);
        }
        avail->clear();
        QListWidgetItem* sep = new QListWidgetItem("Separator", avail);
        sep->setData(Qt::UserRole, QString("-"));
        std::vector<std::pair<QString, QString>> rest;
        for (const auto& kv : all)
            if (!now.contains(kv.first)) rest.push_back({label(kv.second, kv.first), kv.first});
        std::sort(rest.begin(), rest.end(), [](const auto& a, const auto& b) { return a.first.compare(b.first, Qt::CaseInsensitive) < 0; });
        for (const auto& r : rest) {
            QListWidgetItem* item = new QListWidgetItem(r.first, avail);
            item->setIcon(iconOf(all.at(r.second)));
            item->setData(Qt::UserRole, r.second);
        }
        cur->setCurrentRow(std::min(std::max(0, curRow), cur->count() - 1));
        avail->setCurrentRow(std::min(std::max(0, availRow), avail->count() - 1));
    };
    auto commit = [w, tb, timeline](const QStringList& names) {
        applyList(w, tb, names);
        QSettings().setValue(settingsKey(timeline), names);
    };
    auto names = [cur] {
        QStringList l;
        for (int k = 0; k < cur->count(); ++k) l << cur->item(k)->data(Qt::UserRole).toString();
        return l;
    };
    QObject::connect(add, &QPushButton::clicked, &d, [=] {
        QListWidgetItem* it = avail->currentItem();
        if (!it) return;
        QStringList l = names();
        const int at = cur->currentRow() >= 0 ? cur->currentRow() : l.size();
        l.insert(at, it->data(Qt::UserRole).toString());
        commit(l);
        fill();
        cur->setCurrentRow(at + 1 < cur->count() ? at + 1 : at);
    });
    QObject::connect(avail, &QListWidget::itemDoubleClicked, add, &QPushButton::click);
    QObject::connect(remove, &QPushButton::clicked, &d, [=] {
        const int r = cur->currentRow();
        if (r < 0) return;
        QStringList l = names();
        l.removeAt(r);
        commit(l);
        fill();
    });
    QObject::connect(cur, &QListWidget::itemDoubleClicked, remove, &QPushButton::click);
    auto move = [=](int dir) {
        const int r = cur->currentRow();
        QStringList l = names();
        if (r < 0 || r + dir < 0 || r + dir >= l.size()) return;
        l.swapItemsAt(r, r + dir);
        commit(l);
        fill();
        cur->setCurrentRow(r + dir);
    };
    QObject::connect(up, &QPushButton::clicked, &d, [move] { move(-1); });
    QObject::connect(down, &QPushButton::clicked, &d, [move] { move(1); });
    QObject::connect(reset, &QPushButton::clicked, &d, [=] {
        applyList(w, tb, vegasDefault(timeline));
        QSettings().remove(settingsKey(timeline));
        fill();
    });
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::accept);
    fill();
    d.exec();
}

}  // namespace app
}  // namespace reno
