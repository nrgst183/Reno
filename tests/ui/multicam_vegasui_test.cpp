// VEGAS's multicamera preview (VEGAS Pro 18 manual, "Editing multicamera video"): with Tools > Multicamera > Enable
// Multicamera Editing on, the Video Preview tiles every take of the multicamera event at the cursor (2 x 2 for four
// takes), each with its name and number, the active take outlined; a click on a take chooses it. Saves pictures.
//
//   multicam_vegasui_test <output folder>
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/appstate.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QPalette>
#include <QSettings>
#include <QStyleFactory>
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

// The main window's dark theme (as applyTheme() in mainwindow.cpp sets it).
void theme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(220, 220, 220), mid(62, 62, 66), accent(0, 122, 204);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(38, 38, 41));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(p);
}

AppState& st() { return AppState::get(); }

const Event* multicamEvent()
{
    for (const Track& t : st().project().tracks)
        for (const Event& e : t.events)
            if (!e.takes.empty()) return &e;
    return nullptr;
}

// Waits until the playback's newest frame is one rendered after 'since' frames were shown.
void waitFrame(Playback& pb, uint64_t since)
{
    QElapsedTimer clock;
    clock.start();
    while (pb.framesShown() <= since && clock.elapsed() < 8000) uitest::settle(30);
    uitest::settle(150);
}

std::string rgb(QRgb c) { return strfmt("%d %d %d", qRed(c), qGreen(c), qBlue(c)); }
bool near(QRgb a, QColor b, int tol = 30)
{
    return std::abs(qRed(a) - b.red()) <= tol && std::abs(qGreen(a) - b.green()) <= tol && std::abs(qBlue(a) - b.blue()) <= tol;
}

// The middle of tile k's left edge, a pixel in (the outline is three pixels wide at this size).
QPoint leftEdge(const QRect& r, int k)
{
    const int col = k % 2, row = k / 2;
    return QPoint(r.left() + col * r.width() / 2 + 1, r.top() + row * r.height() / 2 + r.height() / 4);
}
QPoint tileCenter(const QRect& r, int k)
{
    const int col = k % 2, row = k / 2;
    return QPoint(r.left() + col * r.width() / 2 + r.width() / 4, r.top() + row * r.height() / 2 + r.height() / 4);
}

void testMulticam()
{
    st().newProject();
    std::string err;
    uint64_t cams[3];
    for (int k = 0; k < 3; ++k) cams[k] = st().importMedia(strfmt("synth:%d", k + 1), &err);
    // a multicamera event as Create Multicamera Track makes one, with a camera missing as the fourth take ("(no camera)")
    st().edit("Cameras", [&](Project& p) {
        p.settings.width = 1920;
        p.settings.height = 1080;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.name = "Multicamera";
        Event e;
        e.id = p.newId();
        e.mediaId = cams[0];
        e.start = 0;
        e.length = 8;
        e.offset = 0;
        e.name = "CAM 1";
        for (int k = 1; k < 4; ++k) {
            Take tk;
            tk.mediaId = k < 3 ? cams[k] : 0;
            tk.offset = 0;
            tk.name = strfmt("CAM %d", k + 1);
            e.takes.push_back(tk);
        }
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
    st().setCursor(2.0);

    Playback& pb = *new Playback();  // never destroyed: its video thread may still post a frame
    TimelineWidget* tl = new TimelineWidget(&pb);
    tl->resize(900, 300);
    PreviewWidget pv(&pb);
    pv.resize(600, 400);
    pv.show();
    uint64_t shown = pb.framesShown();
    pb.requestFrame();
    waitFrame(pb, shown);
    check(!pb.multicam() && !pb.lastFrame().isNull(), "multicamera editing off: the ordinary frame");
    const QImage plain = pb.lastFrame();
    uitest::shot(&pv, gOut + "/multicam_off.png");

    // Tools > Multicamera > Enable Multicamera Editing (the timeline's option; the preview follows it)
    tl->options().multicam = true;
    tl->optionsChanged();
    shown = pb.framesShown();
    st().setCursor(2.0 + 1.0 / 30);
    waitFrame(pb, shown);
    check(pb.multicam(), "the preview follows Enable Multicamera Editing");
    const Playback::MulticamView v = Playback::multicamAt(st().project(), st().cursor());
    check(v.valid() && v.columns == 2 && v.tiles.size() == 4 && v.active == 0, "four takes: a 2 x 2 grid, take 1 active",
          strfmt("%d columns, %d tiles, active %d", v.columns, (int)v.tiles.size(), v.active));
    check(v.tiles.size() == 4 && v.tiles[0].name == "CAM 1" && v.tiles[2].name == "CAM 3" && v.tiles[3].empty && v.tiles[3].name == "(no camera)",
          "tiles are named by their takes, an empty one \"(no camera)\"");

    // the frame: each take scaled into its quarter, the empty one black
    const QImage grid = pb.lastFrame();
    check(grid.size() == plain.size(), "the grid has the frame's size", strfmt("%dx%d", grid.width(), grid.height()));
    const QRgb q4 = grid.pixel(grid.width() * 3 / 4, grid.height() * 3 / 4);
    check(qRed(q4) < 8 && qGreen(q4) < 8 && qBlue(q4) < 8, "the empty take's tile is black", rgb(q4));
    bool tilesDiffer = false;
    for (int y = grid.height() / 8; y < grid.height() / 2 && !tilesDiffer; y += 7)
        for (int x = 4; x < grid.width() / 2 - 4 && !tilesDiffer; x += 7)
            tilesDiffer = grid.pixel(x, y) != grid.pixel(x + grid.width() / 2, y);
    check(tilesDiffer, "takes 1 and 2 show their own pictures");
    bool notPlain = false;
    for (int y = 0; y < grid.height() && !notPlain; y += 11)
        for (int x = 0; x < grid.width() && !notPlain; x += 11) notPlain = grid.pixel(x, y) != plain.pixel(x, y);
    check(notPlain, "the grid is not the ordinary frame");

    // the outline, names and numbers drawn over it
    const QRect r = pv.pictureRect();
    QImage shot = uitest::shot(&pv, gOut + "/multicam_grid.png");
    QWidget* picture = pv.pictureWidget();
    const QPoint off = picture->mapTo(&pv, QPoint(0, 0));
    auto at = [&](const QPoint& pt) { return shot.pixel(pt + off); };
    const QColor cyan(64, 224, 224);
    check(near(at(leftEdge(r, 0)), cyan), "the active take (1) has the cyan outline", rgb(at(leftEdge(r, 0))));
    check(!near(at(leftEdge(r, 1)), cyan), "take 2 has none", rgb(at(leftEdge(r, 1))));
    // the number box at take 2's bottom right (no outline there): white
    const QPoint box2(r.left() + r.width() - 2, r.top() + r.height() / 2 - 2);
    check(qRed(at(box2)) > 240 && qGreen(at(box2)) > 240 && qBlue(at(box2)) > 240, "take 2's number in a white box at its bottom right", rgb(at(box2)));
    const QPoint box4(r.left() + r.width() - 2, r.top() + r.height() - 2);
    check(qRed(at(box4)) < 60, "the empty take has no number box", rgb(at(box4)));

    // a click on take 3 chooses it (stopped: the event shows camera 3)
    uitest::mouseClick(picture, tileCenter(r, 2));
    uitest::settle(100);
    const Event* e = multicamEvent();
    check(e && e->mediaId == cams[2] && e->activeTake == 2, "a click on a take chooses it", e ? strfmt("active %d", e->activeTake) : "no event");
    shown = pb.framesShown();
    waitFrame(pb, shown);
    shot = uitest::shot(&pv, gOut + "/multicam_take3.png");
    check(near(at(leftEdge(r, 2)), cyan) && !near(at(leftEdge(r, 0)), cyan), "the outline moves to take 3", rgb(at(leftEdge(r, 2))));
    // "(no camera)" cannot be chosen
    uitest::mouseClick(picture, tileCenter(r, 3));
    uitest::settle(100);
    e = multicamEvent();
    check(e && e->activeTake == 2, "an empty take cannot be chosen");

    // Preferences > Video: Display take numbers off, another indicator color
    QSettings().setValue("prefs/multicamTakeNumbers", false);
    QSettings().setValue("prefs/multicamIndicator", QColor(255, 128, 0).name());
    st().notify(kPrefs);
    uitest::settle(100);
    shot = uitest::shot(&pv, gOut + "/multicam_prefs.png");
    check(qRed(at(box2)) < 240 || qBlue(at(box2)) < 240, "Display take numbers off: no number boxes", rgb(at(box2)));
    check(near(at(leftEdge(r, 2)), QColor(255, 128, 0)), "the Active take indicator color is the outline's", rgb(at(leftEdge(r, 2))));
    QSettings().remove("prefs/multicamTakeNumbers");
    QSettings().remove("prefs/multicamIndicator");
    st().notify(kPrefs);

    // off again: the ordinary frame (camera 3 alone)
    tl->options().multicam = false;
    tl->optionsChanged();
    shown = pb.framesShown();
    st().setCursor(2.0);
    waitFrame(pb, shown);
    check(!pb.multicam(), "multicamera editing off again");
    const QImage after = pb.lastFrame();
    const QRgb mid = after.pixel(after.width() * 3 / 4, after.height() * 3 / 4);
    check(qRed(mid) + qGreen(mid) + qBlue(mid) > 30, "the ordinary frame again (no black empty quarter)", rgb(mid));
    shot = uitest::shot(&pv, gOut + "/multicam_off_again.png");
    check(!near(at(leftEdge(r, 2)), cyan), "no outline with multicamera editing off");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMulticamVegasUiTest");
    QApplication::setApplicationName("multicam_vegasui_test");
    theme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: multicam_vegasui_test <output folder>"); return 2; }
    gOut = QDir(args[1]).absolutePath();
    QDir().mkpath(gOut);
    QSettings().clear();
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    testMulticam();
    const int rc = uitest::summary();
    std::fflush(stdout);
    std::_Exit(rc);
}
