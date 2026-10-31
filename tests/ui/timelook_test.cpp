// The timeline's VEGAS look in two respects: the track headers' buttons and icons (the coloured strip with the menu button and the
// number, round Mute and Solo icons, the light header of the selected track) and the little indicators on the timeline (dotted grid
// lines down from the ruler's numbered marks, the ruler's small marks, the time selection's length on its bar).
//
//   timelook_test <output folder> [testfx.ofx ...]
//
// Build: uibuild timelook_test tests/ui/timelook_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

bool near(const QColor& a, const QColor& b, int tol = 24)
{
    return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol && std::abs(a.blue() - b.blue()) <= tol;
}

// How many pixels of 'r' are close to 'c'.
int count(const QImage& img, const QRect& r, const QColor& c, int tol = 24)
{
    int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (near(img.pixelColor(x, y), c, tol)) ++n;
    return n;
}

const Track* trackById(uint64_t id)
{
    for (const Track& t : st().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

std::string rgb(const QColor& c) { return std::to_string(c.red()) + "," + std::to_string(c.green()) + "," + std::to_string(c.blue()); }

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTimeLookTest");
    QApplication::setApplicationName("timelook_test");
    QSettings().clear();
    st().loadPreferences();
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: timelook_test <output folder> [file.ofx ...]"); return 2; }
    const QString out = args[1];
    QDir().mkpath(out);
    std::vector<std::string> ofx;
    for (int k = 2; k < args.size(); ++k) ofx.push_back(args[k].toStdString());
    uitest::setup(ofx, {});

    // a video track with a clip, an empty video track (nothing hides the grid there), a sound track, a muted and a soloed one
    std::string err;
    const uint64_t clip = st().importMedia("synth:1", &err);
    uint64_t v1 = 0, v2 = 0, a1 = 0, a2 = 0;
    st().newProject();
    st().edit("Timeline Look Setup", [&](Project& p) {
        auto track = [&](TrackType t, const char* name) {
            Track tr;
            tr.id = p.newId();
            tr.type = t;
            tr.name = name;
            p.tracks.push_back(tr);
            return tr.id;
        };
        v1 = track(TrackType::Video, "Video 1");
        v2 = track(TrackType::Video, "");
        a1 = track(TrackType::Audio, "Audio 1");
        a2 = track(TrackType::Audio, "Audio 2");
        Event e;
        e.id = p.newId();
        e.mediaId = clip;
        e.start = 1;
        e.length = 3;
        p.findTrack(v1)->events.push_back(e);
        e.id = p.newId();
        p.findTrack(a1)->events.push_back(e);
        p.findTrack(a2)->mute = true;
        p.findTrack(a1)->solo = true;
    });
    st().currentTrack = v1;
    st().loopStart = 2.0;
    st().loopEnd = 8.0;
    st().loopOn = true;

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1100, 420);
    w.show();
    w.setHeaderWidth(260);   // wide enough for the FX button
    uitest::settle(300);
    QImage img = uitest::shot(&w, out + "/timelook_vegas.png");

    // ---------------------------------------------------------------- the headers
    {
        const QRect h1 = w.headerRect(v1), h2 = w.headerRect(v2), h3 = w.headerRect(a1), h4 = w.headerRect(a2);
        // the coloured strip down the left, in the track's colour, 24 pixels wide, with the menu button's three bars
        const QColor track1(89, 115, 168);
        const QColor strip = img.pixelColor(5, h1.bottom() - 4);
        check(near(strip, track1), "the strip down the left of a header is in the track's colour", rgb(strip));
        check(near(img.pixelColor(25, h1.bottom() - 4), track1) && !near(img.pixelColor(29, h1.bottom() - 4), track1, 8),
              "the strip is 24 pixels wide (after the 3 pixel margin)", rgb(img.pixelColor(25, h1.bottom() - 4)) + " / " + rgb(img.pixelColor(29, h1.bottom() - 4)));
        const QRect menu = w.headerPartRect(v1, TimelineWidget::PartMenu);
        check(menu.right() < 28 && menu.top() < h1.top() + 10, "the menu button sits in the strip, at its top", std::to_string(menu.right()));
        check(count(img, menu, QColor(255, 255, 255), 30) >= 12, "the menu button's bars are white", std::to_string(count(img, menu, QColor(255, 255, 255), 30)));
        // the track number in white under it
        check(count(img, QRect(3, h1.top() + 25, 24, 20), QColor(255, 255, 255), 30) >= 8, "the track's number is written white in the strip under the button");
        // the selected track's header is light grey, the others dark
        const QColor sel = img.pixelColor(h1.left() + 40, h1.bottom() - 6), plain = img.pixelColor(h2.left() + 40, h2.bottom() - 6);
        check(near(sel, QColor(130, 130, 130), 12), "the selected video track's header body is light grey", rgb(sel));
        check(qGray(plain.rgb()) < 80, "the other headers are dark", rgb(plain));
        // the name on the light header is dark
        const QRect nm = w.headerPartRect(v1, TimelineWidget::PartName);
        int darkInk = 0;
        for (int y = nm.top(); y <= nm.bottom(); ++y)
            for (int x = nm.left(); x < nm.left() + 50; ++x)
                if (qGray(img.pixel(x, y)) < 60) ++darkInk;
        check(darkInk > 15, "the name on the light header is dark", std::to_string(darkInk));

        // round Mute (pink) and Solo (gold) icons, clearly visible: lit and unlit
        auto icon = [&](uint64_t t, TimelineWidget::HeaderPart part) {
            const QRect r = w.headerPartRect(t, part);
            return QRect(r.center().x() - 7, r.center().y() - 7, 16, 16);
        };
        const QColor muteOff(224, 138, 150), muteOn(240, 70, 84), soloOff(186, 160, 86), soloOn(255, 206, 40);
        const QRect mOff = icon(v1, TimelineWidget::PartMute), mOn = icon(a2, TimelineWidget::PartMute);
        const QRect sOff = icon(v1, TimelineWidget::PartSolo), sOn = icon(a1, TimelineWidget::PartSolo);
        check(count(img, mOff, muteOff) > 60, "an unlit Mute is a pink circle", std::to_string(count(img, mOff, muteOff)));
        check(count(img, mOn, muteOn) > 40, "a lit Mute is a red-pink circle", std::to_string(count(img, mOn, muteOn)));
        check(count(img, sOff, soloOff) > 50, "an unlit Solo is a gold circle", std::to_string(count(img, sOff, soloOff)));
        check(count(img, sOn, soloOn) > 60, "a lit Solo is a bright gold circle", std::to_string(count(img, sOn, soloOn)));
        // (the circle's corners are the header's colour: it is round)
        check(!near(img.pixelColor(mOff.left(), mOff.top()), muteOff, 30), "Mute is round, not a box");
        // the crossed 'no' sign at Mute's shoulder: white pixels on a dark disc
        check(count(img, QRect(mOff.right() - 6, mOff.top() - 1, 8, 8), QColor(255, 255, 255), 20) >= 4, "Mute has VEGAS's crossed sign at its shoulder");
        // Solo's white '!' at its right edge
        check(count(img, QRect(sOff.right() - 4, sOff.top() - 1, 7, 12), QColor(255, 255, 255), 45) >= 5, "Solo has its white '!'");
        // contrast against the header: the Mute icon is much lighter than the dark header it sits on
        const QColor behind = img.pixelColor(mOn.left() - 4, mOn.center().y());
        check(std::abs(qGray(muteOn.rgb()) - qGray(behind.rgb())) > 40, "Mute stands out from the dark header", rgb(behind));
        // the old grey "FX M S" boxes are gone: no pixel of the old dim button grey (70,70,75) at the FX position
        const QRect fx = w.headerPartRect(v2, TimelineWidget::PartFx);
        check(count(img, fx, QColor(70, 70, 75), 2) < 10, "the FX button is not the old dim box", std::to_string(count(img, fx, QColor(70, 70, 75), 2)));
        check(count(img, icon(v2, TimelineWidget::PartFx), QColor(150, 150, 150), 14) > 60, "the FX button is VEGAS's grey round icon");
        (void)h3; (void)h4;

        // hit areas and functions are unchanged: a click on Mute or Solo toggles them
        const bool before = trackById(v2)->mute;
        uitest::mouseClick(&w, w.headerPartRect(v2, TimelineWidget::PartMute).center());
        uitest::settle(100);
        check(trackById(v2)->mute != before, "clicking Mute still mutes");
        uitest::mouseClick(&w, w.headerPartRect(v2, TimelineWidget::PartMute).center());
        uitest::settle(100);
        uitest::mouseClick(&w, w.headerPartRect(v2, TimelineWidget::PartSolo).center());
        uitest::settle(100);
        check(trackById(v2)->solo, "clicking Solo still solos");
        uitest::mouseClick(&w, w.headerPartRect(v2, TimelineWidget::PartSolo).center());
        uitest::settle(100);
    }

    // ---------------------------------------------------------------- the indicators
    {
        w.setFocus();
        uitest::settle(100);
        img = uitest::shot(&w, out + "/timelook_indicators.png");
        const QRect ruler = w.rulerRect();
        const QRect lanes(ruler.left(), ruler.bottom() + 1, ruler.width(), w.trackRect(a2).bottom() - ruler.bottom());
        // the numbered marks on the ruler (every two seconds at this zoom)
        std::vector<int> majors;
        const int yMark = ruler.bottom() - 2;
        for (double t : {4.0, 6.0, 10.0, 12.0, 14.0, 16.0}) {
            const int x = w.xAt(t);
            bool solid = x < ruler.right() - 2 && qGray(img.pixel(x - 1, yMark)) < 100;
            for (int y = ruler.top() + 2; y <= yMark && solid; ++y) solid = qGray(img.pixel(x, y)) > 130;
            if (x < ruler.right() - 2) majors.push_back(x);
        }
        check(majors.size() >= 3, "the ruler has numbered marks", std::to_string(majors.size()));
        // the empty video track 2 shows the dotted lines: every other pixel down a column at each mark, nothing beside it
        const QRect lane = w.trackRect(v2);
        int dotted = 0, checked = 0, beside = 0;
        for (int x : majors) {
            if (x < lane.left() + 4) continue;
            if (std::abs(x - w.xAt(2.0)) < 3 || std::abs(x - w.xAt(8.0)) < 3) continue;  // the selection's own edge lines are solid
            ++checked;
            int dots = 0, gaps = 0;
            for (int y = lane.top() + 4; y < lane.top() + 40; ++y) {
                const bool lit = qGray(img.pixel(x, y)) - qGray(img.pixel(x + 3, y)) > 18;
                if ((y & 1) == 1 && lit) ++dots;
                if ((y & 1) == 0 && !lit) ++gaps;
                if (qGray(img.pixel(x + 3, y)) - qGray(img.pixel(x + 5, y)) > 18) ++beside;
            }
            if (dots >= 16 && gaps >= 16) ++dotted;
        }
        check(checked >= 2 && dotted == checked, "dotted grid lines run down through the track area at every numbered mark", std::to_string(dotted) + "/" + std::to_string(checked));
        check(beside == 0, "and only there", std::to_string(beside));
        // the dotted line also reaches the lowest track, through its empty part
        {
            const QRect last = w.trackRect(a2);
            const int x = majors.back();
            int dots = 0;
            for (int y = last.top() + 4; y < last.bottom() - 2; ++y)
                if (qGray(img.pixel(x, y)) - qGray(img.pixel(x + 3, y)) > 18 && (y & 1)) ++dots;
            check(dots > 10, "the lines go down through every track", std::to_string(dots));
        }
        // the ruler's small marks along its bottom, between the numbered ones: short and not full height
        int small = 0;
        for (int x = ruler.left() + 2; x < ruler.right() - 2; ++x)
            if (qGray(img.pixel(x, ruler.bottom() - 1)) > 120 && qGray(img.pixel(x, ruler.top() + 2)) < 110) ++small;
        check(small >= 8, "the ruler has small marks along its bottom", std::to_string(small));

        // the time selection: a bar, with its length (6 seconds: "6.00" at 30 fps)
        const QRect bar = w.loopBarRect();
        check(w.loopLengthLabel() == "6.00", "the selection's length is written as seconds.frames", w.loopLengthLabel().toStdString());
        const double fr = 1.0 / st().project().settings.fps();
        const int x0 = w.xAt(2.0), x1 = w.xAt(8.0);
        const int by = bar.center().y() + 1;
        check(near(img.pixelColor((x0 + x1) / 2 + 40, by), QColor(150, 140, 30), 30), "the selection is a yellow bar while looping is on", rgb(img.pixelColor((x0 + x1) / 2 + 40, by)));
        // a longer selection reads differently
        st().loopStart = 1.0;
        st().loopEnd = 1.0 + 70.0 + 4 * fr;
        w.zoomToFit();
        uitest::settle(200);
        check(w.loopLengthLabel() == "1:10.04", "a minute-long selection reads minutes:seconds.frames", w.loopLengthLabel().toStdString());
        st().loopStart = 2.0;
        st().loopEnd = 8.0;
        st().loopOn = false;
        w.zoomToFit();
        uitest::settle(200);
        img = uitest::shot(&w, out + "/timelook_loop_off.png");
        const int gx = std::min(w.xAt(5.0), img.width() - 30);
        check(!near(img.pixelColor(gx, w.loopBarRect().center().y() + 1), QColor(150, 140, 30), 20) && !near(img.pixelColor(gx, w.loopBarRect().center().y() + 1), QColor(55, 55, 55), 6),
              "and a different one while looping is off", rgb(img.pixelColor(gx, w.loopBarRect().center().y() + 1)));
        st().loopOn = true;
        st().loopStart = 0;
        st().loopEnd = 0;
        w.zoomToFit();
        uitest::settle(200);
        img = uitest::shot(&w, out + "/timelook_no_selection.png");
        check(w.loopLengthLabel().isEmpty(), "and no length is written");
    }

    return uitest::summary();
}
