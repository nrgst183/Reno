// The UI's two looks (theme.h): Reno's own (the default: wide headers, events with title bars, the big time display) and VEGAS's.
// Renders the main window in both at 1920x1018 and 1366x728 and saves the pictures; checks the header widths and parts, the time
// display, events' title bars, that View > Theme and Preferences switch the look at once and that the choice is remembered.
//
//   theme_test <output folder> [testfx.ofx ...]
//
// Build: uibuild theme_test tests/ui/theme_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFontInfo>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QToolBar>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

// The demo project, with what the newer features show on the timeline: markers and a region, a time selection, a track envelope,
// a pan/crop, a compositing child, an event's fades.
void buildProject()
{
    uitest::buildDemoProject();
    st().edit("Theme Test Setup", [](Project& p) {
        Project::Marker m;
        m.id = p.newId();
        m.time = 1.5;
        m.name = "Here";
        p.markers.push_back(m);
        Project::Marker r;
        r.id = p.newId();
        r.time = 5.0;
        r.length = 2.0;
        r.region = true;
        r.name = "Chorus";
        p.markers.push_back(r);
        for (Track& t : p.tracks) {
            if (t.type == TrackType::Audio && t.envelopes.empty()) {
                Envelope e;
                e.kind = "volume";
                e.points = {{0.0, 1.0, CurveLinear}, {6.0, 0.5, CurveLinear}, {12.0, 1.0, CurveLinear}};
                t.envelopes.push_back(e);
            }
        }
        int video = 0;
        for (Track& t : p.tracks)
            if (t.type == TrackType::Video && ++video == 2) {
                t.compositeLevel = 1;
                t.composite = "add";
            }
    });
    st().loopStart = 2.0;
    st().loopEnd = 4.0;
    st().setCursor(2.5);
}

// Switches the look the way the user does: View > Theme.
void chooseTheme(MainWindow& w, UiTheme t)
{
    QAction* a = w.findChild<QAction*>(t == UiTheme::Reno ? "themeReno" : "themeVegas");
    if (a) a->trigger();
    uitest::settle(150);
}

uint64_t firstTrack(TrackType type)
{
    for (const Track& t : st().project().tracks)
        if (t.type == type) return t.id;
    return 0;
}

bool near(const QColor& a, const QColor& b, int tol = 10)
{
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol && std::abs(a.blue() - b.blue()) <= tol;
}

QString rgb(const QColor& c) { return QString("%1,%2,%3").arg(c.red()).arg(c.green()).arg(c.blue()); }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoThemeTest");
    QApplication::setApplicationName("theme_test");
    QSettings().clear();
    qunsetenv("RENO_THEME");
    st().loadPreferences();
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: theme_test <output folder> [file.ofx ...]"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, {});

    check(uiTheme() == UiTheme::Reno && st().prefs.theme == "reno", "Reno's own look is the default");
    const QFont face = QApplication::font();
    buildProject();
    {
        MainWindow w;
        w.resize(1920, 1018);
        w.show();
        uitest::settle(400);
        TimelineWidget* tl = w.timeline();
        tl->zoomToFit();
        uitest::settle(300);
        const uint64_t v = firstTrack(TrackType::Video), a = firstTrack(TrackType::Audio);

        // ---- Reno's own look
        check(tl->headerWidth() == 190, "Reno: the track headers are the first build's 190 pixels", std::to_string(tl->headerWidth()));
        check(tl->headerRect(v).height() == 88 && tl->headerRect(a).height() == 66, "Reno: video tracks 88 high, audio 66",
              std::to_string(tl->headerRect(v).height()) + " " + std::to_string(tl->headerRect(a).height()));
        for (auto part : {TimelineWidget::PartMenu, TimelineWidget::PartName, TimelineWidget::PartFx, TimelineWidget::PartMute, TimelineWidget::PartSolo, TimelineWidget::PartLevel})
            check(!tl->headerPartRect(v, part).isEmpty() && !tl->headerPartRect(a, part).isEmpty(), "Reno: header part " + std::to_string((int)part) + " on video and audio headers");
        check(!tl->headerPartRect(v, TimelineWidget::PartMotion).isEmpty(), "Reno: a video header has the Track Motion button");
        check(!tl->headerPartRect(a, TimelineWidget::PartPan).isEmpty() && tl->headerPartRect(v, TimelineWidget::PartPan).isEmpty(), "Reno: Pan on audio headers only");
        {
            // FX, M, S along the top right, in that order, the menu button and the name at the left
            const QRect fx = tl->headerPartRect(v, TimelineWidget::PartFx), m = tl->headerPartRect(v, TimelineWidget::PartMute),
                        s = tl->headerPartRect(v, TimelineWidget::PartSolo), menu = tl->headerPartRect(v, TimelineWidget::PartMenu);
            check(fx.right() < m.left() && m.right() < s.left() && s.right() < tl->headerWidth() && fx.top() == s.top() && menu.right() < fx.left(),
                  "Reno: the menu button at the left, FX, M and S along the top right");
            check(tl->headerPartRect(v, TimelineWidget::PartLevel).top() > fx.bottom(), "Reno: the level slider under the buttons");
        }
        QImage img = uitest::shot(tl, QString());
        {
            const QRect td = tl->timeDisplayRect();
            check(td.width() == tl->headerWidth(), "Reno: the time display is as wide as the headers");
            // the dark display, its text light and large: count the light pixels in its lower part
            int light = 0;
            for (int y = td.top() + 14; y < td.bottom(); ++y)
                for (int x = td.left(); x < td.right() - 6; ++x)
                    if (qGray(img.pixel(x, y)) > 200) ++light;
            check(near(img.pixelColor(td.left() + 3, td.bottom() - 3), QColor(30, 30, 33)), "Reno: the time display's dark ground", rgb(img.pixelColor(td.left() + 3, td.bottom() - 3)).toStdString());
            check(light > 250, "Reno: the time is written large", std::to_string(light));
        }
        {
            // the first video event's title bar: a shade of its track's colour (Reno's purple for track 1)
            const Track* tr = nullptr;
            for (const Track& t : st().project().tracks)
                if (t.id == v) tr = &t;
            const Event& e = tr->events.back();
            const int x = tl->xAt(e.start + e.length * 0.5), y = tl->trackRect(v).top() + 6;
            const QColor c = img.pixelColor(x, y);
            check(c.blue() > c.green() + 20 && c.red() > 60, "Reno: events have a coloured title bar", rgb(c).toStdString());
            // the header's strip down the left in the track's colour
            const QColor strip = img.pixelColor(2, tl->headerRect(v).center().y());
            check(near(strip, QColor(120, 98, 200)), "Reno: the header's strip in the track's colour", rgb(strip).toStdString());
        }
        uitest::shot(&w, out + "/theme_reno_1920.png");
        {
            // a minimised track keeps its top row: the menu button, the number, the name, FX, M and S
            st().currentTrack = a;
            tl->toggleMinimizeTrack();
            uitest::settle(100);
            const QRect hr = tl->headerRect(a);
            bool inside = hr.height() == 24;
            for (auto part : {TimelineWidget::PartMenu, TimelineWidget::PartName, TimelineWidget::PartFx, TimelineWidget::PartMute, TimelineWidget::PartSolo})
                inside = inside && hr.contains(tl->headerPartRect(a, part));
            check(inside && tl->headerPartRect(a, TimelineWidget::PartLevel).isEmpty(), "Reno: a minimised header keeps its top row only");
            uitest::shot(tl, out + "/theme_reno_minimised.png");
            tl->toggleMinimizeTrack();
            uitest::settle(50);
        }
        w.resize(1366, 728);
        uitest::settle(300);
        tl->zoomToFit();
        uitest::settle(200);
        uitest::shot(&w, out + "/theme_reno_1366.png");
        check(tl->headerWidth() == 190, "Reno: the headers keep their width at 1366x728");

        // ---- the header column's width is the look's own
        tl->setHeaderWidth(230);
        check(tl->headerWidth() == 230, "Reno: the headers can be made wider");

        // ---- View > Theme > VEGAS: at once
        chooseTheme(w, UiTheme::Vegas);
        check(uiTheme() == UiTheme::Vegas, "View > Theme > VEGAS switches the look");
        check(QSettings().value("prefs/theme").toString() == "vegas", "the choice is saved in the preferences", QSettings().value("prefs/theme").toString().toStdString());
        check(tl->headerWidth() == 161, "VEGAS: the VEGAS headers' 161 pixels", std::to_string(tl->headerWidth()));
        check(tl->headerRect(v).height() == 64 && tl->headerRect(a).height() == 68, "VEGAS: VEGAS's track heights",
              std::to_string(tl->headerRect(v).height()) + " " + std::to_string(tl->headerRect(a).height()));
        {
            QWidget* rate = w.findChild<QWidget*>("ratePanel");
            check(rate && rate->width() == tl->headerWidth(), "the Rate panel follows the header width", rate ? std::to_string(rate->width()) : "none");
        }
        img = uitest::shot(tl, QString());
        {
            const QRect td = tl->timeDisplayRect();
            check(near(img.pixelColor(td.left() + 3, td.bottom() - 3), QColor(70, 70, 70)), "VEGAS: the time display's grey", rgb(img.pixelColor(td.left() + 3, td.bottom() - 3)).toStdString());
        }
        uitest::shot(&w, out + "/theme_vegas_1366.png");
        w.resize(1920, 1018);
        uitest::settle(300);
        tl->zoomToFit();
        uitest::settle(200);
        uitest::shot(&w, out + "/theme_vegas_1920.png");

        // ---- and back: Reno's look remembers its own header width
        chooseTheme(w, UiTheme::Reno);
        check(uiTheme() == UiTheme::Reno && tl->headerWidth() == 230, "back in Reno's look, its own header width again", std::to_string(tl->headerWidth()));
        tl->setHeaderWidth(190);
        check(QApplication::font().pixelSize() != 10, "Reno: the system's text size, not VEGAS's smaller one");
        check(QApplication::font().family() == face.family() && QApplication::font().pointSizeF() == face.pointSizeF(), "Reno: the same text again after VEGAS's look",
              QApplication::font().toString().toStdString());
        {
            QLabel* format = w.findChild<QLabel*>("statusFormat");
            check(format && format->isVisible() && format->text().startsWith("1920x1080"), "Reno: the status bar shows the project's format",
                  format ? format->text().toStdString() : "none");
        }

        // ---- Preferences choose it too
        st().prefs.theme = "vegas";
        st().applyPreferences();
        uitest::settle(100);
        check(uiTheme() == UiTheme::Vegas && tl->headerWidth() == 161, "Options > Preferences switches the look at once");
        Preferences loaded;
        loaded.load();
        check(loaded.theme == "vegas", "the look chosen is read back next time");
        st().prefs.theme = "reno";
        st().applyPreferences();
        uitest::settle(100);
        check(uiTheme() == UiTheme::Reno, "and back to Reno's own");
        uitest::shot(&w, out + "/theme_reno_1920_again.png");
    }
    // the next start (applyTheme, then the window) is in the look chosen
    st().prefs.theme = "vegas";
    st().applyPreferences();
    st().loadPreferences();
    applyTheme();
    {
        MainWindow w2;
        w2.resize(1366, 728);
        w2.show();
        uitest::settle(200);
        check(w2.timeline()->headerWidth() == 161, "a new window starts in the look chosen");
    }
    st().prefs.theme = "reno";
    st().applyPreferences();
    QSettings().clear();
    return uitest::summary();
}
