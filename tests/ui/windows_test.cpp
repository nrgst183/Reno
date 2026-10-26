// Windows VEGAS has that src/app/panels.cpp adds: Video Scopes, Explorer, Edit Details. Saves pictures.
//
//   windows_test <output folder>
//
// Build: uibuild windows_test tests/ui/windows_test.cpp panels
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTreeWidget>
#include <QPainter>
#include <QSettings>
#include <QToolButton>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

// 75% colour bars over a grey ramp.
QImage bars()
{
    QImage img(640, 360, QImage::Format_RGB32);
    const QColor c[] = {QColor(191, 191, 191), QColor(191, 191, 0), QColor(0, 191, 191), QColor(0, 191, 0),
                        QColor(191, 0, 191), QColor(191, 0, 0), QColor(0, 0, 191)};
    QPainter p(&img);
    for (int k = 0; k < 7; ++k) p.fillRect(QRect(k * 640 / 7, 0, 640 / 7 + 1, 240), c[k]);
    for (int x = 0; x < 640; ++x) p.fillRect(QRect(x, 240, 1, 120), QColor(x * 255 / 639, x * 255 / 639, x * 255 / 639));
    return img;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoWindowsTest");
    QApplication::setApplicationName("windows_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("scopesout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    Playback pb;
    VideoScopes scopes(&pb);
    scopes.resize(520, 360);
    scopes.show();
    uitest::settle(100);

    QImage grey(320, 180, QImage::Format_RGB32);
    grey.fill(QColor(128, 128, 128));
    scopes.setImage(grey);
    VideoScopes::Stats s = scopes.stats();
    check(s.pixels == 320 * 180 && std::abs(s.meanLuma - 128) < 0.01 && s.stdDevLuma < 0.01 && s.minLuma == 128 && s.maxLuma == 128,
          "a flat grey: mean luma 128, no spread", strfmt("%d %.2f %.2f %d %d", s.pixels, s.meanLuma, s.stdDevLuma, s.minLuma, s.maxLuma));
    QImage half(200, 100, QImage::Format_RGB32);
    half.fill(Qt::black);
    QPainter(&half).fillRect(QRect(0, 0, 100, 100), Qt::white);
    scopes.setImage(half);
    s = scopes.stats();
    check(std::abs(s.meanLuma - 127.5) < 0.01 && std::abs(s.stdDevLuma - 127.5) < 0.01 && s.minLuma == 0 && s.maxLuma == 255,
          "half black, half white: mean 127.5, spread 127.5", strfmt("%.2f %.2f", s.meanLuma, s.stdDevLuma));
    QImage green(100, 100, QImage::Format_RGB32);
    green.fill(QColor(0, 255, 0));
    scopes.setImage(green);
    check(std::abs(scopes.stats().meanLuma - 0.7152 * 255) < 0.6, "luma is Rec. 709's (pure green: 182)", strfmt("%.2f", scopes.stats().meanLuma));
    // big pictures are measured at a smaller size
    QImage big(1920, 1080, QImage::Format_RGB32);
    big.fill(QColor(10, 20, 30));
    scopes.setImage(big);
    check(scopes.stats().pixels == 720 * 405, "a 1920x1080 picture is measured at 720 wide", strfmt("%d", scopes.stats().pixels));

    scopes.setImage(bars());
    const struct { VideoScopes::Scope s; const char* name; } all[] = {{VideoScopes::Vectorscope, "vectorscope"}, {VideoScopes::Waveform, "waveform"},
                                                                      {VideoScopes::Parade, "parade"}, {VideoScopes::Histogram, "histogram"}};
    for (const auto& a : all) {
        scopes.setScope(a.s);
        uitest::settle(50);
        const QImage shot = uitest::shot(&scopes, out + "/scopes_" + a.name + ".png");
        // something was drawn in the scope's area beyond the background
        int lit = 0;
        for (int y = 40; y < shot.height(); y += 2)
            for (int x = 30; x < shot.width(); x += 2) {
                const QColor c = shot.pixelColor(x, y);
                if (c.lightness() > 90) ++lit;
            }
        check(lit > 50, std::string("the ") + a.name + " shows the picture", strfmt("%d", lit));
    }
    QComboBox* choice = scopes.findChild<QComboBox*>("scopeChoice");
    check(choice && choice->count() == 4 && choice->itemText(0) == "Vectorscope" && choice->currentText() == "Histogram", "the scope is picked from VEGAS's list");
    if (choice) choice->setCurrentIndex(1);
    check(scopes.scope() == VideoScopes::Waveform && QSettings().value("scopes/scope").toInt() == (int)VideoScopes::Waveform, "and remembered");
    QToolButton* playing = scopes.findChild<QToolButton*>("scopesWhilePlaying");
    check(playing && playing->isCheckable() && !playing->isChecked(), "Update Scopes While Playing is there, off");

    // ---------------------------------------------------------------- Explorer
    AppState& app = AppState::get();
    app.newProject();
    {
        const QString dir = out + "/explorer";
        QDir().mkpath(dir + "/sub");
        bars().save(dir + "/bars.png");
        grey.save(dir + "/grey.jpg");
        QFile notes(dir + "/notes.txt");
        notes.open(QIODevice::WriteOnly);
        notes.write("not media");
        notes.close();
        MediaExplorer ex;
        ex.resize(700, 300);
        ex.show();
        ex.setFolder(dir);
        QStringList names;
        for (int k = 0; k < 50 && names.size() < 3; ++k) {
            uitest::settle(50);
            names = ex.visibleFiles();
        }
        names.sort();
        check(QFileInfo(ex.folder()) == QFileInfo(dir) && names.join(",") == "bars.png,grey.jpg,sub", "Explorer lists a folder's media files and folders",
              names.join(",").toStdString());
        check(QSettings().value("explorer/folder").toString() == QFileInfo(dir).absoluteFilePath(), "and remembers the folder");
        uitest::shot(&ex, out + "/explorer.png");
        ex.addFiles(QStringList() << QFileInfo(dir + "/bars.png").absoluteFilePath(), false);
        check(app.project().media.size() == 1 && app.project().tracks.empty(), "Add to Project Media adds the file without placing it");
        app.setCursor(2.0);
        ex.addFiles(QStringList() << QFileInfo(dir + "/grey.jpg").absoluteFilePath(), true);
        bool placed = false;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) placed = placed || std::abs(e.start - 2.0) < 1e-9;
        check(app.project().media.size() == 2 && placed, "a double-click puts the file on the timeline at the cursor");
        ex.addFiles(QStringList() << QFileInfo(dir + "/notes.txt").absoluteFilePath(), true);
        check(app.project().media.size() == 2, "a file that is not media is refused");
    }

    // ---------------------------------------------------------------- Edit Details
    {
        uitest::buildDemoProject();
        app.edit("Markers", [](Project& p) {
            Project::Marker m;
            m.id = p.newId();
            m.time = 1.5;
            m.name = "Here";
            p.markers.push_back(m);
            Project::Marker r;
            r.id = p.newId();
            r.time = 3.0;
            r.length = 2.0;
            r.region = true;
            r.name = "Chorus";
            p.markers.push_back(r);
        });
        EditDetails details;
        details.resize(800, 300);
        details.show();
        uitest::settle(50);
        int events = 0;
        for (const Track& t : app.project().tracks) events += (int)t.events.size();
        check(details.rows() == events && events > 3, "Edit Details lists every event", strfmt("%d of %d", details.rows(), events));
        uitest::shot(&details, out + "/details_events.png");
        details.setShow(EditDetails::Markers);
        check(details.rows() == 1 && details.cell(0, 2) == "Here", "the markers", details.cell(0, 2).toStdString());
        details.setShow(EditDetails::Regions);
        check(details.rows() == 1 && details.cell(0, 4) == "Chorus", "the regions");
        details.setShow(EditDetails::Media);
        check(details.rows() == (int)app.project().media.size(), "the media");
        details.setShow(EditDetails::SelectedEvents);
        check(details.rows() == 0, "no event selected: none listed");
        uint64_t first = 0;
        for (const Track& t : app.project().tracks)
            if (!t.events.empty() && !first) first = t.events[0].id;
        app.selectOnly(first);
        uitest::settle(20);
        check(details.rows() == 1, "the selected event", strfmt("%d", details.rows()));
        details.setShow(EditDetails::Events);
        if (QTreeWidget* table = details.findChild<QTreeWidget*>("detailsTable")) table->setCurrentItem(table->topLevelItem(events - 1));
        uint64_t last = 0;
        double lastStart = 0;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) { last = e.id; lastStart = e.start; }
        check(app.selection().size() == 1 && app.selection().count(last) && std::abs(app.cursor() - lastStart) < 1e-9, "a click on a row selects that event and goes to it");
    }
    return uitest::summary();
}
