// Finding menus in the menu bar by title, for the files that add to or rearrange the main window's menus.
#pragma once
#include <QAction>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QString>

namespace reno {
namespace app {

// A menu or action title without its & marker and the shortcut text after the tab.
inline QString plainMenuText(QString text)
{
    return text.split('\t')[0].remove('&');
}

// The menu bar's menu called 'title' ("Edit"...).
inline QMenu* topMenu(QMainWindow* w, const QString& title)
{
    for (QAction* a : w->menuBar()->actions())
        if (a->menu() && plainMenuText(a->text()) == title) return a->menu();
    return nullptr;
}

// A submenu of 'm' by title (without the & and the key).
inline QMenu* subMenu(QMenu* m, const QString& title)
{
    if (!m) return nullptr;
    for (QAction* a : m->actions())
        if (a->menu() && plainMenuText(a->text()) == title) return a->menu();
    return nullptr;
}

}  // namespace app
}  // namespace reno
