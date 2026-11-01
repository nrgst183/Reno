// Reno's whole menu bar against the menu tree captured from VEGAS Pro 18 (tests/ui/data/vegas_menus.txt: depth, text with
// & mnemonics, shortcut, S = submenu, G = grayed, C = checked; the capture's recent-file entries are left out).
// Every menu is opened (aboutToShow) and walked; the text (with its &), the shortcut and whether the item is a submenu
// are compared item by item, in order. Items VEGAS has and Reno does not run are still in Reno's menu (disabled or
// saying so in the shortcut table), so the trees are expected to be equal apart from the entries listed in 'dynamic'.
//
//   menutree_test <output folder> [vegas_menus.txt]
//
// Writes <output folder>/reno_menus.txt (Reno's tree in the same format) and menu_diff.txt (what differs).
// Build: uibuild menutree_test tests/ui/menutree_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "../../src/engine/engine.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTextStream>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

struct Row {
    int depth = 0;
    QString text, shortcut;
    bool submenu = false;
    QString path;  // "Edit > Navigate > Go to Time..." (text without &) for messages
    QString line() const { return QString("%1\t%2\t%3\t%4").arg(depth).arg(text, shortcut, submenu ? "S" : ""); }
};

QString plainText(QString t) { return t.remove('&'); }

void walk(QMenu* m, int depth, const QString& parentPath, std::vector<Row>& out)
{
    emit m->aboutToShow();
    for (QAction* a : m->actions()) {
        if (a->isSeparator() || !a->isVisible()) continue;
        Row r;
        r.depth = depth;
        const QStringList parts = a->text().split('\t');
        r.text = parts[0];
        QStringList keys;
        if (parts.size() > 1 && !parts[1].isEmpty()) keys << parts[1];
        for (const QKeySequence& k : a->shortcuts()) keys << k.toString(QKeySequence::PortableText);
        r.shortcut = keys.join("; ");
        r.submenu = a->menu() != nullptr;
        r.path = parentPath + (parentPath.isEmpty() ? "" : " > ") + plainText(r.text);
        out.push_back(r);
        if (a->menu()) walk(a->menu(), depth + 1, r.path, out);
    }
}

std::vector<Row> readVegas(const QString& file)
{
    std::vector<Row> rows;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return rows;
    QStringList stack;
    for (const QString& ln : QString::fromUtf8(f.readAll()).split('\n')) {
        if (ln.isEmpty() || ln[0] == '#') continue;
        const QStringList p = ln.split('\t');
        if (p.size() < 4) continue;
        Row r;
        r.depth = p[0].toInt();
        r.text = p[1];
        r.shortcut = p[2];
        r.submenu = p[3] == "S";
        while (stack.size() > r.depth) stack.removeLast();
        r.path = (stack.isEmpty() ? "" : stack.join(" > ") + " > ") + plainText(r.text);
        stack << plainText(r.text);
        rows.push_back(r);
    }
    return rows;
}

// Entries that change with the situation (the Undo text, the layout slots...) are not compared.
bool dynamic(const Row& r)
{
    return r.path.startsWith("View > Window Layouts > ") || r.path.startsWith("Insert > Audio Envelopes > ") ||
           r.path.startsWith("Insert > Video Envelopes > ") || r.path.startsWith("Edit > Stream > ") || r.path.startsWith("Edit > Extensions > ") || r.path.startsWith("View > Extensions > ") ||
           r.path.startsWith("Tools > Extensions > ") || r.path.startsWith("Tools > Scripting > ") || r.path.contains("<EMPTY>") ||
           r.path.startsWith("File > Share Online");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMenuTreeTest");
    QApplication::setApplicationName("menutree_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("menutree");
    QString ref = args.size() > 2 && args[2].endsWith(".txt") ? args[2] : QString();
    if (ref.isEmpty()) ref = QFileInfo(QString(__FILE__)).absolutePath() + "/data/vegas_menus.txt";
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState::get().newProject();
    MainWindow w;
    w.resize(1500, 900);
    w.show();
    uitest::settle(300);

    std::vector<Row> reno;
    for (QAction* m : w.menuBar()->actions()) {
        Row r;
        r.depth = 0;
        r.text = m->text();
        r.submenu = true;
        r.path = plainText(r.text);
        reno.push_back(r);
        if (m->menu()) walk(m->menu(), 1, r.path, reno);
    }
    const std::vector<Row> vegas = readVegas(ref);
    check(vegas.size() > 300, "the VEGAS menu list is read", QString::number(vegas.size()).toStdString());

    {
        QFile f(out + "/reno_menus.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream s(&f);
            for (const Row& r : reno) s << r.depth << '\t' << r.text << '\t' << r.shortcut << '\t' << (r.submenu ? "S" : "") << '\n';
        }
    }

    // Compare menu by menu: the list of children of each parent path, in order.
    auto children = [](const std::vector<Row>& rows) {
        std::vector<std::pair<QString, std::vector<Row>>> out;  // parent path -> children
        std::vector<QString> stack;
        for (const Row& r : rows) {
            const QString parent = r.depth == 0 ? QString() : r.path.left(r.path.size() - plainText(r.text).size() - (r.depth ? 3 : 0));
            size_t k = 0;
            while (k < out.size() && out[k].first != parent) ++k;
            if (k == out.size()) out.push_back({parent, {}});
            if (!dynamic(r)) out[k].second.push_back(r);
        }
        return out;
    };
    const auto cv = children(vegas), cr = children(reno);
    QStringList diffs, extras, shortcutGaps;
    int different = 0;
    // a shortcut as Qt and VEGAS write it, compared lower-case
    auto normKey = [](QString k) {
        k = k.trimmed().toLower();
        k.replace("del", "delete").replace("deleteete", "delete").replace("ins", "insert").replace("insertert", "insert");
        k.replace("backspace", "back").replace("pgup", "page up").replace("pgdown", "page down").replace("return", "enter");
        return k;
    };
    for (const auto& pv : cv) {
        const std::vector<Row>* rr = nullptr;
        for (const auto& pr : cr)
            if (pr.first == pv.first) rr = &pr.second;
        if (!rr) {
            diffs << "menu missing in Reno: " + (pv.first.isEmpty() ? QString("(bar)") : pv.first);
            ++different;
            continue;
        }
        const std::vector<Row>& a = pv.second;
        const std::vector<Row>& b = *rr;
        for (size_t i = 0; i < a.size(); ++i) {
            const QString va = a[i].text;
            const QString vb = i < b.size() ? b[i].text : QString("(nothing)");
            const bool aboutAlias = plainText(va) == "About ..." && plainText(vb) == "About Reno";
            // Reno keeps its own submenus where VEGAS has one command: Remove Assignable FX lists the chains, Quantize to Frames
            // has the Show Unquantized Event Edges switch beside it
            const bool structural = va == vb && (plainText(va) == "Remove Assignable FX..." || plainText(va) == "Quantize to Frames");
            if ((va != vb && !aboutAlias) || (i < b.size() && a[i].submenu != b[i].submenu && !structural)) {
                diffs << QString("%1 #%2: VEGAS %3 | Reno %4").arg(pv.first.isEmpty() ? "(bar)" : pv.first).arg(i + 1).arg(va, vb);
                ++different;
                continue;
            }
            if (!a[i].shortcut.isEmpty() && i < b.size() && !(b[i].submenu && !a[i].submenu)) {
                bool has = false;
                for (const QString& want : a[i].shortcut.split(';'))
                    for (const QString& k : b[i].shortcut.split(';')) has = has || normKey(k) == normKey(want);
                if (!has) shortcutGaps << QString("%1 > %2: VEGAS shows %3, Reno has [%4]").arg(pv.first, plainText(va), a[i].shortcut, b[i].shortcut);
            }
        }
        for (size_t i = a.size(); i < b.size(); ++i) extras << QString("%1 > %2").arg(pv.first, plainText(b[i].text));
    }
    {
        QFile f(out + "/menu_diff.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) QTextStream(&f) << diffs.join("\n") << "\n";
    }
    {
        QFile f(out + "/menu_extras.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) QTextStream(&f) << "Reno's own items (after VEGAS's):\n" << extras.join("\n") << "\n\nShortcuts VEGAS shows that Reno's item lacks:\n" << shortcutGaps.join("\n") << "\n";
    }
    check(different == 0, "Reno's menus start with VEGAS's items, in its order and wording", QString("%1 differing positions, see menu_diff.txt").arg(different).toStdString());
    check(shortcutGaps.isEmpty(), "every shortcut VEGAS's menus show is on Reno's item", QString("%1 lack it, see menu_extras.txt").arg(shortcutGaps.size()).toStdString());
    return uitest::summary();
}
