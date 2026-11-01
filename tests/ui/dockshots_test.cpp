// Reno's docked windows next to the VEGAS Pro 18 ones from the UI capture: each window is floated at the size VEGAS's floating window
// has in the capture picture and saved beside the VEGAS picture (its floating frame cut out), and the main window at the capture's size
// beside the capture's main window. Checks what can be: the windows exist with VEGAS's title and tab names, and the default layout docks
// them where VEGAS does.
//
//   dockshots_test <output folder> [capture folder]
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/pluginchooser.h"
#include "layoutcmp.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QAbstractButton>
#include <QToolButton>
#include <QAction>
#include <QCheckBox>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QRadioButton>
#include <QTextStream>
#include <QToolBar>
#include <algorithm>
#include <map>
#include <QSettings>
#include <QTabBar>
#include <QToolButton>

using namespace reno::app;
using uitest::check;

namespace {

struct Pic {
    const char* dock;       // object name of Reno's window
    const char* title;
    const char* picture;    // glob of the capture picture
    QRect frame;            // the floating window in that picture
};

void beside(const QImage& reno, const QString& vegasPng, const QRect& crop, const QString& out)
{
    QImage v(vegasPng);
    if (!v.isNull() && !crop.isNull()) v = v.copy(crop);
    QImage img(reno.width() + v.width() + 12, std::max(reno.height(), v.height()), QImage::Format_RGB32);
    img.fill(QColor(255, 0, 255));
    QPainter p(&img);
    p.drawImage(0, 0, reno);
    if (!v.isNull()) p.drawImage(reno.width() + 12, 0, v);
    p.end();
    img.save(out);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoDockShots");
    QApplication::setApplicationName("dockshots_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("dockshots");
    QString caps = args.size() > 2 && QDir(args[2]).exists() ? args[2] : QString::fromLocal8Bit(qgetenv("VEGAS_CAPTURE_DIR"));
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState::get().newProject();
    MainWindow w;
    w.resize(1550, 923);
    w.show();
    uitest::settle(500);
    auto pic = [&](const char* glob) {
        const QStringList l = QDir(caps).entryList(QStringList() << glob, QDir::Files);
        return l.isEmpty() ? QString() : caps + "/" + l[0];
    };
    // the main window as the capture's first picture (1550 x 923 with the title bar; Reno's picture is the client area)
    {
        const QImage main = uitest::shot(&w, out + "/main.png");
        if (!caps.isEmpty()) {
            QImage v(pic("001 main window.png"));
            if (!v.isNull()) v = v.copy(0, 31, 1550, 923 - 31);
            layoutcmp::sideBySide(main, QString(), out + "/main_vs_vegas.png");
            QImage img(main.width() + v.width() + 12, std::max(main.height(), v.height()), QImage::Format_RGB32);
            img.fill(QColor(255, 0, 255));
            QPainter p(&img);
            p.drawImage(0, 0, main);
            p.drawImage(main.width() + 12, 0, v);
            p.end();
            img.save(out + "/main_vs_vegas.png");
        }
    }
    // the tool bars: the tool tips of the buttons, left to right, as the capture's main window lists them (tests/ui/data/vegas_toolbars.txt)
    {
        const QString file = QFileInfo(QString(__FILE__)).absolutePath() + "/data/vegas_toolbars.txt";
        std::map<QString, std::vector<std::pair<QString, bool>>> want;
        {
            QFile f(file);
            QString cur;
            if (f.open(QIODevice::ReadOnly))
                for (const QString& ln : QString::fromUtf8(f.readAll()).split('\n')) {
                    if (ln.startsWith("## ")) cur = ln.mid(3).trimmed();
                    else if (!ln.isEmpty() && ln[0] != '#' && !cur.isEmpty()) want[cur].push_back({ln.section('\t', 0, 0), ln.section('\t', 1, 1) == "1"});
                }
        }
        auto buttons = [&](QWidget* box, int y0, int y1) {
            std::vector<std::pair<QPoint, QAbstractButton*>> found;
            if (!box) return std::vector<QAbstractButton*>();
            for (QAbstractButton* b : box->findChildren<QAbstractButton*>()) {
                if (!b->isVisibleTo(box) || qobject_cast<QCheckBox*>(b) || qobject_cast<QRadioButton*>(b)) continue;
                const QPoint at = b->mapTo(box, QPoint(0, 0));
                if (at.y() < y0 || at.y() > y1) continue;
                found.push_back({at, b});
            }
            std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first.x() < b.first.x(); });
            std::vector<QAbstractButton*> out;
            for (auto& f : found) out.push_back(f.second);
            return out;
        };
        auto plain = [](QString t) {
            t.replace('&', "");
            t = t.simplified();
            return t;
        };
        auto name = [&](QAbstractButton* b) {
            QString t = b->toolTip().isEmpty() ? b->text() : b->toolTip();
            QToolButton* tb = qobject_cast<QToolButton*>(b);
            if (tb && tb->defaultAction() && !tb->defaultAction()->toolTip().isEmpty() && b->toolTip().isEmpty()) t = tb->defaultAction()->toolTip();
            return plain(t);
        };
        QStringList report;
        auto compareBar = [&](const QString& section, const std::vector<QAbstractButton*>& have) {
            const auto& w = want[section];
            int same = 0;
            QStringList have_;
            for (QAbstractButton* b : have) have_ << name(b);
            QStringList want_;
            for (auto& x : w) want_ << plain(x.first);
            for (int i = 0; i < std::min(want_.size(), have_.size()); ++i) same += want_[i] == have_[i];
            report << "== " + section << QString("VEGAS  (%1): %2").arg(want_.size()).arg(want_.join(" | ")) << QString("Reno   (%1): %2").arg(have_.size()).arg(have_.join(" | "));
            check(want_ == have_, ("tool bar " + section + ": the buttons and their tool tips are VEGAS's").toStdString(), QString("%1 of %2 alike, Reno has %3").arg(same).arg(want_.size()).arg(have_.size()).toStdString());
        };
        QToolBar* mainBar = w.findChild<QToolBar*>("mainToolBar");
        compareBar("Standard", buttons(mainBar, 0, 40));
        if (QDockWidget* pv = w.findChild<QDockWidget*>("dockPreview")) compareBar("Video Preview", buttons(pv->widget(), 0, 36));
        if (QDockWidget* mb = w.findChild<QDockWidget*>("dockMaster")) compareBar("Master Bus", buttons(mb->widget(), 0, 36));
        compareBar("Timeline", buttons(w.findChild<QWidget*>("transportRow"), 0, 40));
        QFile f(out + "/toolbars_diff.txt");
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) QTextStream(&f) << report.join("\n") << "\n";
    }
    const Pic pics[] = {
        {"dockNotes", "Project Notes", "024 window Project Notes*.png", QRect(588, 197, 284, 492)},
        {"dockTransitions", "Transitions", "025 window Transitions*.png", QRect(378, 87, 704, 711)},
        {"dockVideoFx", "Video FX", "026 window Video FX*.png", QRect(378, 87, 704, 711)},
        {"dockGenerators", "Media Generators", "027 window Media Generators*.png", QRect(378, 87, 704, 711)},
        {"dockDashboard", "Dashboard", "036 window Dashboard*.png", QRect(588, 197, 284, 492)},
    };
    for (const Pic& p : pics) {
        QDockWidget* d = w.findChild<QDockWidget*>(p.dock);
        check(d != nullptr, QString("%1: the window exists").arg(p.title).toStdString());
        if (!d) continue;
        check(d->windowTitle() == p.title, QString("%1: its title").arg(p.title).toStdString(), d->windowTitle().toStdString());
        d->setFloating(true);
        d->resize(p.frame.size());
        d->show();
        uitest::settle(300);
        const QImage shot = uitest::shot(d, QString("%1/%2.png").arg(out, p.dock));
        if (!caps.isEmpty()) beside(shot, pic(p.picture), p.frame, QString("%1/%2_vs_vegas.png").arg(out, p.dock));
        d->setFloating(false);
        d->hide();
    }
    // the Plug-In Chooser (Tools > Video > Video Output FX): VEGAS's window is 1346 x 746 with its frame
    {
        PluginChooser chooser(ChooserKind::VideoFx, "Plug-In Chooser - Video Output FX", reno::FxChain(), &w);
        chooser.show();
        uitest::settle(400);
        check(chooser.width() == 1344 && chooser.height() == 715, "Plug-In Chooser: VEGAS's size less the frame", QString("%1x%2").arg(chooser.width()).arg(chooser.height()).toStdString());
        const QImage shot = uitest::shot(&chooser, out + "/pluginchooser.png");
        if (!caps.isEmpty()) beside(shot, pic("043 Tools*.png"), QRect(1, 31, 1344, 715), out + "/pluginchooser_vs_vegas.png");
        chooser.hide();
    }
    return uitest::summary();
}
