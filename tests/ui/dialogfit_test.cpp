// Fixed-size dialogs (Preferences here) can be resized: made wider, the drop-down lists that reach into the right half of
// a page grow with the window and the buttons at the right keep to the right edge; made taller, the page grows; made smaller
// than they were, they scroll and the OK / Cancel / Apply row stays in view. The Preferences tab rows stay where they are
// when a tab is chosen, and are taller than VEGAS's.
//
//   dialogfit_test [output folder]
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/editdialogs.h"
#include "../../src/app/mainwindow.h"
#include "uitestlib.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QPushButton>
#include <QScrollBar>
#include <QScrollArea>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTimer>

using namespace reno::app;
using uitest::check;

static QRect inDialog(QWidget* w, QWidget* d) { return QRect(w->mapTo(d, QPoint(0, 0)), w->size()); }

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoDialogFitTest");
    QApplication::setApplicationName("dialogfit_test");
    QSettings().clear();
    applyTheme();
    uitest::setup({}, {});
    QTimer::singleShot(400, [&] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(d && d->objectName() == "preferencesDialog", "the Preferences dialog opens");
        if (!d) return;
        QTabWidget* tabs = d->findChild<QTabWidget*>("tabs");
        tabs->setCurrentIndex(1);   // Video: drop-down lists down the right half
        uitest::settle(80);
        check(d->minimumWidth() < d->width() && d->maximumWidth() > d->width(), "the dialog can be resized");
        QComboBox* wide = nullptr;
        for (QComboBox* c : tabs->widget(1)->findChildren<QComboBox*>())
            if (c->isVisibleTo(d) && c->objectName() != "standIn" && (!wide || c->x() > wide->x())) wide = c;
        QPushButton* ok = d->findChild<QPushButton*>("ok");
        check(wide && ok, "a drop-down list on the page and the OK button");
        if (!wide || !ok) return;
        const QRect c0 = inDialog(wide, d), ok0 = inDialog(ok, d), page0 = inDialog(tabs, d);
        const QSize s0 = d->size();

        d->resize(s0.width() + 200, s0.height() + 100);
        uitest::settle(80);
        const QRect c1 = inDialog(wide, d), ok1 = inDialog(ok, d), page1 = inDialog(tabs, d);
        check(c1.x() == c0.x() && c1.width() == c0.width() + 200, "made 200 px wider, the drop-down list is 200 px wider",
              QString("%1 -> %2").arg(c0.width()).arg(c1.width()).toStdString());
        check(ok1.x() == ok0.x() + 200 && ok1.y() == ok0.y() + 100, "and the OK button keeps to the bottom right corner");
        check(page1.width() == page0.width() + 200 && page1.height() == page0.height() + 100, "the pages grow with the window");
        QTabBar* bar = tabs->tabBar();
        check(bar->width() >= page1.width() - 4, "the tab rows span the wider window", std::to_string(bar->width()));

        d->resize(s0);
        uitest::settle(80);
        check(inDialog(wide, d) == c0 && inDialog(ok, d) == ok0, "back at its size, everything is where it was");

        d->resize(s0.width() - 100, s0.height() - 250);
        uitest::settle(80);
        QScrollArea* sc = d->findChild<QScrollArea*>("dialogFitScroll");
        const QRect ok2 = inDialog(ok, d);
        check(sc && sc->verticalScrollBar()->isVisible() && sc->horizontalScrollBar()->isVisible(), "made smaller, the dialog scrolls");
        check(ok2.bottom() < d->height() && ok2.right() < d->width() && ok2.y() > d->height() - 40, "and its OK button stays in view at the bottom",
              QString("%1,%2 in %3x%4").arg(ok2.x()).arg(ok2.y()).arg(d->width()).arg(d->height()).toStdString());
        d->resize(s0);
        uitest::settle(80);

        // the tab rows: a tab keeps its place when chosen, and rows are taller than VEGAS's 18 px
        tabs->setCurrentIndex(0);
        uitest::settle(40);
        const int rowH = bar->height() / 3;
        check(rowH >= 24, "the tab rows are at least 24 px tall", std::to_string(rowH));
        // click every row's first tab at the same place twice: the same tab both times (rows don't move)
        for (int row = 0; row < 3; ++row) {
            const QPoint at(10, 2 + row * rowH + rowH / 2);
            uitest::mouseClick(bar, at);
            uitest::settle(20);
            const int first = tabs->currentIndex();
            uitest::mouseClick(bar, at);
            uitest::settle(20);
            check(tabs->currentIndex() == first, "row " + std::to_string(row + 1) + ": the tab under the mouse stays the same after choosing it",
                  std::to_string(first) + " then " + std::to_string(tabs->currentIndex()));
        }
        d->reject();
    });
    runPreferences(nullptr);
    return uitest::summary();
}
