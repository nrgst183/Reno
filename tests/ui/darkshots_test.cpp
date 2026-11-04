// Pictures and geometry dumps of Reno's dialogs and windows, to compare the light dialogs with the dark ones (dialoglook.h).
//
//   darkshots_test <output folder> [--light] [--check]
//
// Every dialog is opened, photographed (<name>.png) and every child widget's position, size, font and text written to
// <name>.geo. With --light the "Use light dialogs like VEGAS" option is on (the way Reno looked before the option existed).
// The geometry files of a dark run and of a light run must be identical: only colours differ. With --check, a dark run also
// fails if a dialog's window or base colour came out light. Prints PASS / FAIL lines.
#include "../../src/app/mainwindow.h"
#include "../../src/app/editdialogs.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/app/smalldialogs.h"
#include "../../src/app/trackdefaults.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFontInfo>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTabBar>
#include <QTabWidget>
#include <QTextStream>
#include <QTimer>
#include <algorithm>
#include <functional>

using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
bool gLight = false;

QString key(QString t)
{
    QString k;
    for (QChar c : t)
        if (c.isLetterOrNumber()) k += c.toLower();
    return k;
}

QAction* find(QMenu* m, const QStringList& path, int at)
{
    for (QAction* a : m->actions()) {
        if (a->isSeparator() || key(a->text()) != key(path[at])) continue;
        if (at + 1 == path.size()) return a;
        if (a->menu()) {
            emit a->menu()->aboutToShow();
            if (QAction* r = find(a->menu(), path, at + 1)) return r;
        }
    }
    return nullptr;
}

QAction* item(MainWindow& w, const QString& path)
{
    const QStringList p = path.split('/');
    for (QAction* top : w.menuBar()->actions())
        if (top->menu() && key(top->text()) == key(p[0])) {
            emit top->menu()->aboutToShow();
            return find(top->menu(), p, 1);
        }
    return nullptr;
}

// Where a widget sits among its parents, with the class and object name so the lines compare between runs.
QString pathOf(QWidget* w, QWidget* top)
{
    QStringList parts;
    for (QWidget* x = w; x && x != top; x = x->parentWidget()) {
        int idx = 0;
        if (x->parentWidget())
            for (QObject* c : x->parentWidget()->children()) {
                if (c == x) break;
                if (c->isWidgetType()) ++idx;
            }
        parts.prepend(QString("%1[%2]%3").arg(x->metaObject()->className()).arg(idx).arg(x->objectName().isEmpty() ? QString() : "#" + x->objectName()));
    }
    return parts.join("/");
}

void dumpGeometry(QWidget* d, const QString& file)
{
    QStringList lines;
    lines << QString("TOP %1x%2 font=%3/%4").arg(d->width()).arg(d->height()).arg(QFontInfo(d->font()).family()).arg(QFontInfo(d->font()).pixelSize());
    for (QWidget* w : d->findChildren<QWidget*>()) {
        if (!w->isVisibleTo(d)) continue;   // (what is hidden has not been laid out)
        QString text;
        if (auto* b = qobject_cast<QAbstractButton*>(w)) text = b->text();
        else if (auto* l = qobject_cast<QLabel*>(w)) text = l->text();
        else if (auto* e = qobject_cast<QLineEdit*>(w)) text = e->text();
        const QFontInfo f(w->font());
        lines << QString("%1 %2,%3 %4x%5 font=%6/%7/%8 text=%9")
                     .arg(pathOf(w, d)).arg(w->x()).arg(w->y()).arg(w->width()).arg(w->height())
                     .arg(f.family()).arg(f.pixelSize()).arg(f.bold() ? 1 : 0).arg(text.left(60));
    }
    QFile f(file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Text)) QTextStream(&f) << lines.join("\n") << "\n";
}

// True when most of the picture's area (sampled) is light: a white panel in the dark scheme.
double lightShare(const QImage& im)
{
    if (im.isNull()) return 0;
    int light = 0, n = 0;
    for (int y = 0; y < im.height(); y += 3)
        for (int x = 0; x < im.width(); x += 3) {
            ++n;
            if (qGray(im.pixel(x, y)) > 200) ++light;
        }
    return n ? double(light) / n : 0;
}

void capture(QWidget* d, const QString& name)
{
    uitest::settle(60);
    const QImage im = uitest::shot(d, QString("%1/%2.png").arg(gOut, name));
    dumpGeometry(d, QString("%1/%2.geo").arg(gOut, name));
    if (!gLight) {
        const double share = lightShare(im);
        check(share < 0.12, name.toStdString() + ": no white panel in the dark scheme", QString("%1% of it is light").arg(int(share * 100)).toStdString());
    }
}

// Opens a dialog with 'open' (it runs a modal loop); 'drive' gets it 500 ms later and must close it.
void modal(const std::function<void()>& open, const std::function<void(QDialog*)>& drive)
{
    QTimer::singleShot(500, [drive] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d) d = qobject_cast<QDialog*>(QApplication::activeWindow());
        check(d != nullptr, "a dialog opened");
        if (d) drive(d);
    });
    open();
    uitest::settle(80);
}

// A tabbed dialog: one picture per tab.
void captureTabs(QDialog* d, const QString& name, const char* tabWidgetName, const char* tabBarName)
{
    QTabWidget* tw = tabWidgetName ? d->findChild<QTabWidget*>(tabWidgetName) : nullptr;
    QTabBar* tb = tabBarName ? d->findChild<QTabBar*>(tabBarName) : nullptr;
    const int n = tw ? tw->count() : tb ? tb->count() : 0;
    if (n == 0) {
        capture(d, name);
        return;
    }
    for (int k = 0; k < n; ++k) {
        if (tw) tw->setCurrentIndex(k);
        else tb->setCurrentIndex(k);
        capture(d, QString("%1_%2").arg(name).arg(k + 1, 2, 10, QChar('0')));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoDarkShots");
    QApplication::setApplicationName("darkshots_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("darkshots");
    gLight = args.contains("--light");
    QDir().mkpath(gOut);
    if (gLight) vprefs::set("prefs/lightDialogs", true);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    uitest::buildDemoProject();
    AppState& app = AppState::get();
    MainWindow w;
    w.resize(1366, 760);
    w.show();
    uitest::settle(300);

    struct Menu { const char* path; const char* name; const char* tabWidget; const char* tabBar; };
    const Menu menus[] = {
        {"File/Properties...", "projectprops", "tabs", nullptr},
        {"File/Render As...", "renderas", nullptr, nullptr},
        {"Options/Customize Keyboard...", "customizekeyboard", nullptr, nullptr},
        {"Options/Customize Timeline Toolbar...", "customizetoolbar", nullptr, nullptr},
        {"Insert/Time...", "inserttime", nullptr, nullptr},
        {"Help/About Reno", "about", nullptr, "aboutTabs"},
        {"View/Window Layouts/Save Layout As...", "savelayout", nullptr, nullptr},
        {"View/Window Layouts/Organize Layouts...", "organizelayouts", nullptr, nullptr},
        {"Insert/Slideshow...", "slideshow", nullptr, "slideshowTabs"},
        {"Tools/Advanced Save...", "advancedsave", nullptr, nullptr},
    };
    for (const Menu& m : menus) {
        QAction* a = item(w, m.path);
        check(a != nullptr, std::string(m.path) + ": the command is there");
        if (!a) continue;
        const Menu mm = m;
        modal([a] { a->trigger(); }, [mm](QDialog* d) {
            captureTabs(d, mm.name, mm.tabWidget, mm.tabBar);
            d->reject();
        });
    }

    // Options > Preferences: every tab, and a combo box popup
    modal([&] { runPreferences(&w); }, [](QDialog* d) {
        captureTabs(d, "prefs", "tabs", nullptr);
        QTabWidget* tw = d->findChild<QTabWidget*>("tabs");
        if (tw) {
            // Reno's own tab, scrolled to the Display group where the light dialogs option is
            tw->setCurrentIndex(tw->count() - 1);
            uitest::settle(60);
            if (QCheckBox* c = d->findChild<QCheckBox*>("lightDialogs")) {
                for (QWidget* x = c->parentWidget(); x; x = x->parentWidget())
                    if (QScrollArea* sa = qobject_cast<QScrollArea*>(x)) {
                        sa->ensureWidgetVisible(c);
                        sa->horizontalScrollBar()->setValue(0);
                        break;
                    }
                uitest::settle(60);
                capture(d, "prefs_reno_display");
            }
            tw->setCurrentIndex(1);
            uitest::settle(60);
            QComboBox* c = tw->currentWidget()->findChild<QComboBox*>();
            if (c) {
                c->showPopup();
                uitest::settle(80);
                if (c->view() && c->view()->window()) capture(c->view()->window(), "prefs_combopopup");
                c->hidePopup();
            }
        }
        d->reject();
    });

    // Event Properties and the track's Set Default Track Properties
    uint64_t eventId = 0, trackId = 0;
    for (const auto& t : app.project().tracks)
        if (!t.events.empty() && !eventId) {
            eventId = t.events.front().id;
            trackId = t.id;
        }
    if (eventId) {
        modal([&] { runEventProperties(&w, eventId); }, [](QDialog* d) {
            captureTabs(d, "eventprops", "tabs", nullptr);
            d->reject();
        });
        modal([&] { runSetDefaultTrackProperties(&w, trackId); }, [](QDialog* d) {
            capture(d, "trackprops");
            d->reject();
        });
    }

    // Paste Repeat and Advanced Audio Configuration, the audio page of Project Properties
    modal([&] { PasteRepeatChoice c; askPasteRepeat(&w, c); }, [](QDialog* d) {
        capture(d, "pasterepeat");
        d->reject();
    });
    modal([&] { advancedAudioConfiguration(&w, "Speakers", "test-device"); }, [](QDialog* d) {
        capture(d, "advancedaudio");
        d->reject();
    });
    modal([&] { runProjectPropertiesAudio(&w); }, [](QDialog* d) {
        captureTabs(d, "projectpropsaudio", "tabs", nullptr);
        d->reject();
    });

    // Plug-In Chooser
    PluginChooser::onShown = [](PluginChooser* c) {
        capture(c, "chooser");
        PluginChooser::onShown = nullptr;
        c->reject();
    };
    {
        PluginChooser c(ChooserKind::VideoFx, "Plug-In Chooser - Video Event FX", reno::FxChain());
        c.exec();
    }
    PluginChooser::onShown = nullptr;

    // Help > Keyboard Shortcuts is a window of its own, not modal
    if (QAction* a = item(w, "Help/Keyboard Shortcuts")) {
        a->trigger();
        uitest::settle(300);
        for (QWidget* top : QApplication::topLevelWidgets())
            if (top != &w && top->isVisible() && top->windowTitle() == "Keyboard Shortcuts") {
                capture(top, "keyboardshortcuts");
                top->close();
            }
    }

    // The option: Preferences > Reno (Display group) "Use light dialogs like VEGAS", off by default, stored, and followed by the next dialogs
    {
        vprefs::set("prefs/lightDialogs", false);
        modal([&] { runPreferences(&w); }, [](QDialog* d) {
            QCheckBox* c = d->findChild<QCheckBox*>("lightDialogs");
            check(c != nullptr, "Preferences has the \"Use light dialogs like VEGAS\" check box");
            if (c) {
                check(!c->isChecked(), "the light dialogs option is off by default");
                check(c->text() == "Use light dialogs like VEGAS", "its text", c->text().toStdString());
                c->setChecked(true);
            }
            if (QPushButton* ok = d->findChild<QPushButton*>("ok")) ok->click();
            else d->accept();
        });
        uitest::settle(100);
        check(QSettings().value("prefs/lightDialogs").toBool(), "OK in Preferences stores the light dialogs option");
        auto windowLightness = [&](int* out) {
            QAction* a = item(w, "Insert/Time...");
            modal([a] { a->trigger(); }, [out](QDialog* d) {
                *out = d->palette().color(QPalette::Window).lightness();
                d->reject();
            });
        };
        int on = -1, off = -1;
        windowLightness(&on);
        check(on > 200, "with the option on a new dialog is light", std::to_string(on));
        vprefs::set("prefs/lightDialogs", false);
        windowLightness(&off);
        check(off >= 0 && off < 100, "with the option off a new dialog is dark", std::to_string(off));
    }

    return uitest::summary();
}
