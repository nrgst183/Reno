// Puts the main window's menus in VEGAS Pro 18's order and wording (the tree captured from VEGAS, src/app/menu_spec.inc): for
// every menu the items VEGAS has come first, in its order and with its & mnemonics; one Reno has no feature for is added as a
// disabled item that says so (the same "unavailable" items as elsewhere); what only Reno has follows after a separator.
// Menus that are filled while they are used (the Window Layouts, the Script Menu, the envelope insert lists) are left alone.
#include "mainwindow.h"
#include "menuutil.h"
#include <QAction>
#include <QMenu>
#include <QMenuBar>
#include <QWidget>
#include <deque>
#include <map>
#include <memory>
#include <set>

namespace reno {
namespace app {

namespace {

struct MenuSpecRow {
    int depth;
    const char* text;
    const char* shortcut;
    bool submenu;
};
#include "menu_spec.inc"

struct Node {
    QString text, shortcut;
    bool submenu = false;
    std::vector<Node> kids;
};

// What two texts must share to be the same item: the letters and digits, whatever the mnemonic, dots or case.
QString key(QString t)
{
    t = t.split('\t')[0];
    t.replace("&&", "and");
    t.remove('&');
    t = t.toLower();
    QString out;
    out.reserve(t.size());
    for (const QChar c : t)
        if ((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) out += c;
    return out;
}

std::vector<Node> specTree()
{
    std::vector<Node> top;
    std::vector<Node*> stack;
    for (const MenuSpecRow& r : kMenuSpec) {
        Node n;
        n.text = QString::fromUtf8(r.text);
        n.shortcut = QString::fromUtf8(r.shortcut);
        n.submenu = r.submenu;
        std::vector<Node>& into = r.depth == 0 ? top : stack[(size_t)r.depth - 1]->kids;
        into.push_back(n);
        stack.resize((size_t)r.depth);
        stack.push_back(&into.back());
    }
    return top;
}

// Menus that fill themselves as they open.
bool filledLater(const QString& path)
{
    return path == "View > Window Layouts" || path == "Tools > Scripting" || path == "Insert > Audio Envelopes" || path == "Insert > Video Envelopes";
}

// Reno's wording for items VEGAS names differently (the item keeps Reno's text).
const std::map<QString, QString>& aliases()
{
    static const std::map<QString, QString> a = {
        {"View > Audio Envelopes|pan", "trackpan"}, {"Help|about", "aboutreno"}, {"Help|keyboardshortcuts", "keyboardshortcuts"},
    };
    return a;
}

QAction* placeholder(QMenu* parent, const Node& n)
{
    QAction* a = nullptr;
    if (n.submenu) {
        QMenu* m = new QMenu(n.text, parent);
        for (const Node& k : n.kids) m->addAction(placeholder(m, k));
        if (n.kids.empty()) {
            QAction* none = new QAction("No Extensions", m);
            none->setEnabled(false);
            m->addAction(none);
        }
        a = m->menuAction();
    } else {
        a = new QAction(n.shortcut.isEmpty() ? n.text : n.text + "\t" + n.shortcut, parent);
    }
    // (a submenu opens, as VEGAS's, to its own disabled lines)
    a->setEnabled(n.submenu);
    const QString why = "VEGAS has this; Reno has no such feature";
    a->setToolTip(plainMenuText(n.text) + ": " + why);
    a->setStatusTip(why);
    a->setProperty("vegasUnavailable", true);
    return a;
}

// The main window, which keeps the Reno-only View items that are left out of the menu (so their keys still work).
QWidget*& mainWindow()
{
    static QWidget* w = nullptr;
    return w;
}

// Menus that show only VEGAS's items (Reno's own extras there are commands with keys: they stay on the window, out of
// the menu), the Touch Interface switch excepted.
bool vegasItemsOnly(const QString& path) { return path == "View"; }

// Actions moved from a menu to one of its submenus (VEGAS has them one level deeper than Reno), so the menu they left does
// not take them back.
std::set<QAction*>& moved()
{
    static std::set<QAction*> s;
    return s;
}

void conform(QMenu* m, const std::vector<Node>& spec, const QString& path, QMenu* parent = nullptr)
{
    if (filledLater(path)) return;
    // menus that fill themselves when they open are put in order again after that
    if (!m->property("vegasConformed").toBool()) {
        m->setProperty("vegasConformed", true);
        const std::vector<Node>* kids = &spec;
        QObject::connect(m, &QMenu::aboutToShow, m, [m, kids, path] { conform(m, *kids, path); });
    }
    const QList<QAction*> old = m->actions();
    std::map<QString, std::deque<QAction*>> byKey;
    for (QAction* a : old) {
        if (a->isSeparator()) continue;
        QString k = a->objectName() == "undo" ? QString("undo") : a->objectName() == "redo" ? QString("redo") : key(a->text());
        byKey[k].push_back(a);
    }
    std::vector<QAction*> order;
    std::vector<bool> placed;
    std::map<QAction*, bool> used;
    for (const Node& n : spec) {
        const QString k = key(n.text);
        QAction* found = nullptr;
        auto take = [&](const QString& kk) {
            auto it = byKey.find(kk);
            if (it != byKey.end() && !it->second.empty()) {
                found = it->second.front();
                it->second.pop_front();
            }
        };
        take(k);
        if (!found) {
            auto al = aliases().find(path + "|" + k);
            if (al != aliases().end()) take(al->second);
        }
        if (!found && parent) {
            for (QAction* pa : parent->actions())
                if (!pa->isSeparator() && !pa->menu() && key(pa->text()) == k && !moved().count(pa)) {
                    found = pa;
                    moved().insert(pa);
                    parent->removeAction(pa);
                    break;
                }
        }
        if (found) {
            used[found] = true;
            const QString plain = found->objectName() == "undo" || found->objectName() == "redo" || k == "about" ? QString() : n.text;
            if (!plain.isEmpty()) {
                const QStringList parts = found->text().split('\t');
                found->setText(parts.size() > 1 ? plain + "\t" + parts[1] : plain);
            }
            // the shortcut VEGAS shows, when Reno's item has no key of its own to show
            if (!n.shortcut.isEmpty() && found->shortcuts().isEmpty() && !found->text().contains('\t') && !found->menu())
                found->setText(found->text() + "\t" + n.shortcut);
            if (found->menu()) conform(found->menu(), n.kids, path.isEmpty() ? plainMenuText(n.text) : path + " > " + plainMenuText(n.text), m);
            order.push_back(found);
        } else {
            order.push_back(placeholder(m, n));
        }
    }
    // Reno's own items: after a separator, in the order they had
    std::vector<QAction*> extras;
    for (QAction* a : old)
        if (!a->isSeparator() && !used.count(a) && !moved().count(a)) {
            if (vegasItemsOnly(path) && a->objectName() != "touchInterface" && !a->menu()) {
                if (mainWindow()) mainWindow()->addAction(a);
                continue;
            }
            extras.push_back(a);
        }
    // separators stay after the item they followed (the File menu's list of recent projects is put in at its marker)
    std::map<QAction*, std::vector<QAction*>> sepAfter;
    QAction* prev = nullptr;
    for (QAction* a : old) {
        if (a->isSeparator()) {
            if (prev && used.count(prev)) sepAfter[prev].push_back(a);
        } else {
            prev = a;
        }
    }
    for (QAction* a : old) m->removeAction(a);
    for (QAction* a : old)
        if (moved().count(a)) moved().erase(a);
    QAction* last = nullptr;
    for (size_t i = 0; i < order.size(); ++i) {
        m->addAction(order[i]);
        last = order[i];
        if (i + 1 < order.size() || !extras.empty())
            for (QAction* s : sepAfter[order[i]]) m->addAction(s);
    }
    (void)last;
    if (!extras.empty()) {
        m->addSeparator();
        for (QAction* a : extras) m->addAction(a);
    }
}

}  // namespace

void conformMenusToVegas(QMenuBar* bar)
{
    static const std::vector<Node> spec = specTree();
    mainWindow() = bar->window();
    for (QAction* a : bar->actions()) {
        if (!a->menu()) continue;
        for (const Node& n : spec)
            if (key(n.text) == key(a->text())) {
                a->setText(n.text);
                conform(a->menu(), n.kids, plainMenuText(n.text));
            }
    }
}

}  // namespace app
}  // namespace reno
