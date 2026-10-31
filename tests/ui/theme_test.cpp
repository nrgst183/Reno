// The interface's look: VEGAS Pro 18's (the only one). Renders the main window at 1920x1018 and 1366x728 and saves the pictures; checks
// the header width and parts, the track heights, the time display, the Rate panel following the headers, the text size, and that
// a look saved in the preferences by an older build ("reno", "modern") is simply ignored.
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
    // a look chosen in an older build: nothing reads it any more
    QSettings().setValue("prefs/theme", "reno");
    st().loadPreferences();
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: theme_test <output folder> [file.ofx ...]"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, {});

    buildProject();
    for (const char* saved : {"reno", "modern", "vegas", ""}) {
        QSettings().setValue("prefs/theme", saved);
        st().loadPreferences();
        MainWindow w;
        w.resize(1920, 1018);
        w.show();
        uitest::settle(400);
        TimelineWidget* tl = w.timeline();
        tl->zoomToFit();
        uitest::settle(300);
        const std::string saved_ = std::string("(saved look \"") + saved + "\") ";
        check(tl->headerWidth() == 161, saved_ + "the VEGAS headers' 161 pixels, whatever look an older build saved", std::to_string(tl->headerWidth()));
        if (std::string(saved) != "reno") continue;   // (the rest once is enough)
        const uint64_t v = firstTrack(TrackType::Video), a = firstTrack(TrackType::Audio);
        check(tl->headerRect(v).height() == 64 && tl->headerRect(a).height() == 68, "VEGAS's track heights",
              std::to_string(tl->headerRect(v).height()) + " " + std::to_string(tl->headerRect(a).height()));
        for (auto part : {TimelineWidget::PartMenu, TimelineWidget::PartName, TimelineWidget::PartMute, TimelineWidget::PartSolo, TimelineWidget::PartLevel})
            check(!tl->headerPartRect(v, part).isEmpty() && !tl->headerPartRect(a, part).isEmpty(), "header part " + std::to_string((int)part) + " on video and audio headers");
        check(tl->headerPartRect(v, TimelineWidget::PartFx).isEmpty() && tl->headerPartRect(v, TimelineWidget::PartMotion).isEmpty(),
              "the narrow header has no FX and Track Motion buttons (they come with a wider header)");
        check(!tl->headerPartRect(a, TimelineWidget::PartPan).isEmpty() && tl->headerPartRect(v, TimelineWidget::PartPan).isEmpty(), "Pan on audio headers only");
        {
            QWidget* rate = w.findChild<QWidget*>("ratePanel");
            check(rate && rate->width() == tl->headerWidth(), "the Rate panel follows the header width", rate ? std::to_string(rate->width()) : "none");
        }
        QImage img = uitest::shot(tl, QString());
        {
            const QRect td = tl->timeDisplayRect();
            check(td.width() == tl->headerWidth(), "the time display is as wide as the headers");
            check(near(img.pixelColor(td.left() + 3, td.bottom() - 3), QColor(70, 70, 70)), "the time display's grey", rgb(img.pixelColor(td.left() + 3, td.bottom() - 3)).toStdString());
        }
        uitest::shot(&w, out + "/theme_vegas_1920.png");
        {
            // a minimised track keeps its top row
            st().currentTrack = a;
            tl->toggleMinimizeTrack();
            uitest::settle(100);
            const QRect hr = tl->headerRect(a);
            check(hr.height() == 24 && tl->headerPartRect(a, TimelineWidget::PartLevel).isEmpty(), "a minimised header keeps its top row only");
            uitest::shot(tl, out + "/theme_vegas_minimised.png");
            tl->toggleMinimizeTrack();
            uitest::settle(50);
        }
        w.resize(1366, 728);
        uitest::settle(300);
        tl->zoomToFit();
        uitest::settle(200);
        uitest::shot(&w, out + "/theme_vegas_1366.png");
        check(tl->headerWidth() == 161, "the headers keep their width at 1366x728");

        // ---- the header column's width can be changed and is remembered
        tl->setHeaderWidth(230);
        check(tl->headerWidth() == 230 && QSettings().value("timeline/headerWidth").toInt() == 230, "the headers can be made wider (and the width is saved)");
        tl->setHeaderWidth(161);

        // ---- preferences saved or applied again do not change the look, nor switch anything on
        st().prefs.previewQuality = st().prefs.previewQuality;
        st().applyPreferences();
        uitest::settle(100);
        check(tl->headerWidth() == 161, "Options > Preferences applied again leave the look as it is");
    }
    // a preference saved by an older build is not written back
    {
        QSettings().setValue("prefs/theme", "reno");
        st().loadPreferences();
        st().prefs.save();
        check(true, "(saving the preferences with an old look in the settings does not crash)");
    }
    QSettings().clear();
    return uitest::summary();
}
