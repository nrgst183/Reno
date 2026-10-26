// Timeline and project workflow as in VEGAS Pro 18:
//   - VEGAS's fifteen time formats (engine/timefmt): written and read back, drop-frame timecode, feet+frames; Options > Ruler
//     Format, the ruler's Time Format menu, the time display's own menu (Time Format, Text / Background Color), Set Time at Cursor
//     (typed in place, one undo step), Project Properties' list;
//   - times typed in place: Go To (Ctrl+G) in the time display, Ctrl+Shift+G and the readouts under the timeline (cursor,
//     selection end, selection length: double-click);
//   - the marker bar's Grid Spacing menu;
//   - Preferences > Video's event thumbnails (All / Head, Center, Tail / Head, Tail / Head / None) and source frame numbers;
//   - crash recovery: a run that ended without closing is found, VEGAS's question asked, its auto-saved copy opened untitled.
//
//   parity_timeline_test <output folder>      (also runs itself with --crash-child <dir> as the run that crashes)
//
// Build: uibuild parity_timeline_test tests/ui/parity_timeline_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "../../src/app/recovery.h"
#include "../../src/app/editdialogs.h"
#include "../../src/engine/timefmt.h"
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QCheckBox>
#include <QFileDialog>
#include "../../src/vegas/vegas.h"
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QTreeWidget>
#include "../../src/app/panels.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;

QString plain(QString t)
{
    t = t.split('\t')[0];
    t.replace("&&", QString(QChar(1)));
    t.remove('&');
    t.replace(QChar(1), '&');
    return t;
}

QStringList texts(QMenu* m)
{
    QStringList out;
    if (!m) return out;
    for (QAction* a : m->actions()) out << (a->isSeparator() ? QString("-") : plain(a->text()));
    return out;
}

QMenu* subOf(QMenu* m, const QString& title)
{
    if (!m) return nullptr;
    for (QAction* a : m->actions())
        if (a->menu() && plain(a->text()) == title) return a->menu();
    return nullptr;
}

QAction* itemOf(QMenu* m, const QString& text)
{
    if (!m) return nullptr;
    for (QAction* a : m->actions())
        if (!a->isSeparator() && plain(a->text()) == text) return a;
    return nullptr;
}

QAction* item(const QString& path)
{
    const QStringList parts = path.split('/');
    QList<QAction*> acts = gW->menuBar()->actions();
    QAction* found = nullptr;
    for (const QString& p : parts) {
        found = nullptr;
        for (QAction* a : acts)
            if (!a->isSeparator() && plain(a->text()) == p) { found = a; break; }
        if (!found) return nullptr;
        if (found->menu()) {
            emit found->menu()->aboutToShow();
            acts = found->menu()->actions();
        }
    }
    return found;
}

bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
std::string num(double v) { char b[64]; std::snprintf(b, sizeof b, "%.6f", v); return b; }

// The box editTextInPlace made (null if none).
QLineEdit* inPlace()
{
    for (QWidget* t : QApplication::topLevelWidgets())
        if (QLineEdit* e = t->findChild<QLineEdit*>("inPlaceEdit"))
            if (e->isVisible()) return e;
    return nullptr;
}

bool typeInPlace(const QString& text)
{
    uitest::settle(30);
    QLineEdit* e = inPlace();
    if (!e) return false;
    e->setText(text);
    uitest::key(e, Qt::Key_Return);
    uitest::settle(30);
    return true;
}

void buildProject()
{
    AppState& app = st();
    app.newProject();
    std::string err;
    const uint64_t clip = app.importMedia("synth:1", &err);
    const uint64_t v = app.addTrack(TrackType::Video);
    app.addTrack(TrackType::Audio);
    app.addMediaToTimeline(clip, 0.0, v);
    app.addMediaToTimeline(clip, 12.0, v);
    app.setCursor(0);
    app.setLoop(0, 0);
}

// The run that crashes: a project with three audio tracks, auto-saved, then the process ends without closing.
int crashChild(const QString& dir)
{
    session::begin(dir);
    AppState& app = st();
    app.newProject();
    for (int k = 0; k < 3; ++k) app.addTrack(TrackType::Audio);
    std::string err;
    if (!app.autoSaveNow(&err)) { std::fprintf(stderr, "auto-save failed: %s\n", err.c_str()); return 2; }
    std::fflush(nullptr);
    std::_Exit(3);
}

int countPixels(const QImage& img, const QRect& r, std::function<bool(QColor)> f)
{
    int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x)
            if (img.rect().contains(x, y) && f(img.pixelColor(x, y))) ++n;
    return n;
}

}  // namespace

int main(int argc, char** argv)
{
    if (argc >= 3 && std::string(argv[1]) == "--crash-child") {
        QCoreApplication core(argc, argv);
        QCoreApplication::setOrganizationName("RenoParityTest");
        QCoreApplication::setApplicationName("parity_timeline_test");
        return crashChild(argv[2]);
    }
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoParityTest");
    QApplication::setApplicationName("parity_timeline_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("parity");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    // ------------------------------------------------------------------ the time formats themselves
    {
        ProjectSettings s;
        s.fpsNum = 30000;
        s.fpsDen = 1001;
        s.sampleRate = 48000;
        auto f = [&](double t, const char* id) { return formatTime(s, t, id); };
        check(timeFormats().size() == 15, "fifteen time formats (VEGAS's list)");
        check(f(1.5, "samples") == "72000", "Samples", f(1.5, "samples"));
        check(f(3725.25, "time") == "01:02:05.250", "Time is hh:mm:ss.mmm", f(3725.25, "time"));
        check(f(2.5, "seconds") == "2.500", "Seconds", f(2.5, "seconds"));
        check(f(10.0, "frames") == "299", "Absolute Frames at the project's rate (29.97)", f(10.0, "frames"));
        // drop-frame: the first frame of minute 1 is ;02, of minute 10 is ;00
        check(f(1800 / (30000.0 / 1001.0) + 1e-9, "smpteDrop") == "00:01:00;02", "SMPTE Drop skips ;00 and ;01 at the minute",
              f(1800 / (30000.0 / 1001.0) + 1e-9, "smpteDrop"));
        check(f(17982 / (30000.0 / 1001.0) + 1e-9, "smpteDrop") == "00:10:00;00", "... but not at every tenth minute",
              f(17982 / (30000.0 / 1001.0) + 1e-9, "smpteDrop"));
        check(f(1800 / (30000.0 / 1001.0) + 1e-9, "smpteNonDrop") == "00:01:00:00", "SMPTE Non-Drop counts every frame",
              f(1800 / (30000.0 / 1001.0) + 1e-9, "smpteNonDrop"));
        check(f(2.0, "smpteEBU") == "00:00:02:00" && f(2.04, "smpteEBU") == "00:00:02:01", "SMPTE EBU (25 fps)", f(2.04, "smpteEBU"));
        check(f(1.0, "smpte24") == "00:00:01:00" && f(1.5, "smpte30") == "00:00:01:15", "SMPTE Film Sync and SMPTE 30");
        check(f(27 * 1001.0 / 24000.0 + 1e-9, "smpteIVTC") == "00:00:01:03" && f(1.0, "smpteIVTC") == "00:00:00:23", "SMPTE Film Sync IVTC (23.976)",
              f(27 * 1001.0 / 24000.0 + 1e-9, "smpteIVTC"));
        check(f(2.0, "cdtime") == "00:00:02:00" && f(2.5, "cdtime") == "00:00:02:37", "Audio CD Time (75 frames a second)", f(2.5, "cdtime"));
        s.fpsNum = 24;
        s.fpsDen = 1;
        check(f(1.0, "feet35") == "1+08" && f(1.0, "feet16") == "0+24", "Feet and Frames 35mm (16 fpf) and 16mm (40 fpf)", f(1.0, "feet35") + " " + f(1.0, "feet16"));
        check(f(-2.0, "seconds") == "-2.000" && f(-1.0, "timecode") == "-00:00:01:00", "a ruler starting before zero counts with a minus sign");
        // read back, in every format
        s.fpsNum = 30000;
        s.fpsDen = 1001;
        int bad = 0;
        std::string first;
        for (const TimeFormatInfo& tf : timeFormats()) {
            if (std::string(tf.id) == "measures") continue;
            for (double t : {0.0, 1.0, 61.5, 600.7, 3725.2}) {
                const std::string text = formatTime(s, t, tf.id);
                double back = -1;
                const bool ok = parseTimeText(s, text, tf.id, &back);
                // each format keeps the time to its own step: a frame, a sample, a millisecond
                const double step = std::string(tf.id) == "cdtime" ? 1 / 75.0 : std::string(tf.id).find("feet") == 0 ? 1001 / 30000.0 : 1 / 23.0;
                if (!ok || back > t + 1e-6 || back < t - step) {
                    ++bad;
                    if (first.empty()) first = std::string(tf.id) + " " + text + " -> " + num(back) + " (was " + num(t) + ")";
                }
                // and what is read is written the same way again
                if (ok && formatTime(s, back + 1e-7, tf.id) != text) {
                    ++bad;
                    if (first.empty()) first = std::string(tf.id) + " " + text + " rewritten as " + formatTime(s, back + 1e-7, tf.id);
                }
            }
        }
        check(bad == 0, "every format's times read back as written", first);
        double v = 0;
        check(parseTimeText(s, "1.3.500", "measures", &v) && near(v, 1.25, 1e-9),
              "measure.beat.tick typed (120 bpm, 4/4)", num(v));
        check(parseTimeText(s, "12+08", "feet35", &v) && near(v, (12 * 16 + 8) / (30000.0 / 1001.0), 1e-9), "feet+frames typed", num(v));
        check(parseTimeText(s, "00:10:00;00", "smpteDrop", &v) && near(v, 17982 / (30000.0 / 1001.0), 1e-9), "drop-frame timecode typed", num(v));
        check(parseTimeText(s, "12.5", "timecode", &v) && near(v, 12.5), "a plain number is seconds in the clock-like formats");
        check(parseTimeText(s, "48000", "samples", &v) && near(v, 1.0), "and samples in Samples");
        check(!parseTimeText(s, "abc", "timecode", &v) && !parseTimeText(s, "", "time", &v) && !parseTimeText(s, "1:2:3:4:5", "timecode", &v),
              "nonsense is refused");
        check(timeFormatFromScript(13) == "smpteDrop" && timeFormatInfo("smpteDrop").scriptValue == 13 && timeFormatFromScript(99) == "timecode",
              "ScriptPortal's RulerFormat values");
    }

    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1400, 860);
    w.show();
    uitest::settle(300);
    buildProject();
    TimelineWidget* tl = w.timeline();
    tl->zoomToFit();
    uitest::settle(100);
    AppState& app = st();

    // ------------------------------------------------------------------ Options > Ruler Format
    {
        QAction* rf = item("Options/Ruler Format");
        const QStringList want = {"Samples", "Time", "Seconds", "Time & Frames", "Absolute Frames", "Measures & Beats", "Feet and Frames 16mm (40 fpf)",
                                  "Feet and Frames 35mm (16 fpf)", "SMPTE Film Sync IVTC (23.976 fps, Video)", "SMPTE Film Sync (24 fps, Film)",
                                  "SMPTE EBU (25 fps, Video)", "SMPTE Non-Drop (29.97 fps, Video)", "SMPTE Drop (29.97 fps, Video)",
                                  "SMPTE 30 (30 fps, Audio)", "Audio CD Time", "-", "Set Time at Cursor"};
        const QStringList have = texts(rf ? rf->menu() : nullptr);
        check(have == want, "Options > Ruler Format has VEGAS's items in its order", have.join(" | ").toStdString());
        app.setCursor(65.0);
        QAction* drop = item("Options/Ruler Format/SMPTE Drop (29.97 fps, Video)");
        if (drop) drop->trigger();
        uitest::settle(50);
        QLabel* cur = w.findChild<QLabel*>("cursorPosition");
        check(app.project().settings.rulerFormat == "smpteDrop" && item("Options/Ruler Format/SMPTE Drop (29.97 fps, Video)")->isChecked(),
              "choosing SMPTE Drop sets the project's ruler format (ticked)");
        check(cur && cur->text() == QString::fromStdString(formatTime(app.project().settings, 65.0, "smpteDrop")) && cur->text().contains(';'),
              "the cursor readout counts in it", cur ? cur->text().toStdString() : "");
        app.undo();
        check(app.project().settings.rulerFormat == "timecode", "one undo step");
        uitest::shot(&w, out + "/ruler_format_menu_main.png");
    }

    // ------------------------------------------------------------------ the ruler's and the time display's menus
    {
        QMenu m;
        const QPoint ruler = tl->rulerRect().center();
        tl->buildContextMenu(&m, ruler);
        QMenu* tf = subOf(&m, "Time Format");
        const QStringList t = texts(tf);
        check(tf && t.size() == 20 && t[0] == "Use Media Frame Rate" && t[1] == "Use Project Frame Rate" && t[3] == "Samples" && t[17] == "Audio CD Time" &&
                  t[19] == "Set Time at Cursor",
              "the ruler's Time Format menu (VEGAS's 1004, and Set Time at Cursor)", t.join(" | ").toStdString());
        check(tf && !itemOf(tf, "Use Media Frame Rate")->isEnabled() && itemOf(tf, "Use Project Frame Rate")->isChecked() &&
                  itemOf(tf, "Time & Frames")->isChecked(),
              "frames at the project's rate; the project's format ticked");
        if (QAction* a = itemOf(tf, "Feet and Frames 35mm (16 fpf)")) a->trigger();
        check(app.project().settings.rulerFormat == "feet35", "a format chosen there is the project's");
        app.undo();

        QMenu c;
        tl->buildContextMenu(&c, tl->timeDisplayRect().center());
        const QStringList ct = texts(&c);
        check(ct.mid(0, 6) == QStringList({"Time at Cursor", "MIDI Timecode In", "MIDI Timecode Out", "MIDI Clock Out", "-", "Time Format"}) &&
                  ct.contains("Text Color") && ct.contains("Background Color"),
              "the time display's menu (VEGAS's 1012)", ct.join(" | ").toStdString());
        QMenu* ctf = subOf(&c, "Time Format");
        check(ctf && texts(ctf).size() == 17 && texts(ctf).last() == "Set Time at Cursor", "its Time Format: the fifteen and Set Time at Cursor",
              texts(ctf).join(" | ").toStdString());
        // Background Color > Custom...: a colour dialog
        QTimer::singleShot(200, [] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QColorDialog* d = qobject_cast<QColorDialog*>(t))
                    if (d->isVisible()) { d->setCurrentColor(QColor(0, 90, 160)); d->accept(); }
        });
        if (QAction* a = itemOf(subOf(&c, "Background Color"), "Custom...")) a->trigger();
        uitest::settle(80);
        const QImage img = uitest::shot(tl, out + "/time_display_color.png");
        const QRect td = tl->timeDisplayRect();
        const int blue = countPixels(img, td.adjusted(2, 2, -10, -2), [](QColor c) { return c.blue() > 140 && c.red() < 30 && c.green() > 70 && c.green() < 110; });
        check(blue > td.width() * td.height() / 3, "Background Color > Custom... colours the time display", std::to_string(blue));
        check(QSettings().value("timeline/timeDisplayBackground").toString() == "#005aa0", "and is kept for the next start");
        QMenu c2;
        tl->buildContextMenu(&c2, td.center());
        if (QAction* a = itemOf(subOf(&c2, "Background Color"), "Default")) a->trigger();
        uitest::settle(50);
        const QImage img2 = tl->grab().toImage();
        check(countPixels(img2, td.adjusted(2, 2, -10, -2), [](QColor c) { return c.blue() > 140 && c.red() < 30; }) == 0, "Default puts the look's colour back");
    }

    // ------------------------------------------------------------------ Set Time at Cursor
    {
        app.setCursor(2.0);
        tl->scrollToCursor();
        QAction* set = item("Options/Ruler Format/Set Time at Cursor");
        check(set && set->isEnabled(), "Set Time at Cursor is there");
        if (set) set->trigger();
        uitest::settle(50);
        QLineEdit* e = inPlace();
        check(e && e->text() == "00:00:02:00" && e->geometry().top() < tl->rulerRect().bottom(), "it opens a box in the ruler with the cursor's time",
              e ? e->text().toStdString() : "");
        uitest::shot(&w, out + "/set_time_at_cursor.png");
        typeInPlace("00:00:15:00");
        check(near(app.project().settings.rulerStart, 13.0), "ruler start puts the cursor at 00:00:15:00 (VEGAS's example: starts at 13:00)",
              num(app.project().settings.rulerStart));
        QLabel* cur = w.findChild<QLabel*>("cursorPosition");
        check(cur && cur->text() == "00:00:15:00", "the readouts count from it", cur ? cur->text().toStdString() : "");
        app.undo();
        check(near(app.project().settings.rulerStart, 0.0), "one undo step");
        // Esc drops it
        if (set) set->trigger();
        uitest::settle(30);
        if (QLineEdit* e2 = inPlace()) { e2->setText("00:01:00:00"); uitest::key(e2, Qt::Key_Escape); }
        uitest::settle(30);
        check(near(app.project().settings.rulerStart, 0.0) && !inPlace(), "Esc leaves it as it was");
        // nonsense changes nothing
        if (set) set->trigger();
        typeInPlace("soon");
        check(near(app.project().settings.rulerStart, 0.0), "a time that cannot be read changes nothing");
        // Audio CD time: no offset (VEGAS greys it)
        app.edit("Ruler Format", [](Project& p) { p.settings.rulerFormat = "cdtime"; });
        emit item("Options")->menu()->aboutToShow();
        check(!item("Options/Ruler Format/Set Time at Cursor")->isEnabled(), "no offset in Audio CD Time");
        app.undo();
    }

    // ------------------------------------------------------------------ times typed in place
    {
        app.setCursor(1.0);
        app.setLoop(0, 0);
        QAction* goTo = w.findChild<QAction*>("goTo");
        QAction* setEnd = w.findChild<QAction*>("setSelectionEnd");
        if (goTo) goTo->trigger();
        QLineEdit* e = inPlace();
        const QRect td = tl->timeDisplayRect();
        check(e && e->parentWidget() == tl && td.contains(e->geometry().center()), "Go To (Ctrl+G) types in the time display, as VEGAS does");
        typeInPlace("00:00:07;15");
        check(near(app.cursor(), 7.5), "and the cursor goes there", num(app.cursor()));
        // the readouts: double-click
        QLabel* cur = w.findChild<QLabel*>("cursorPosition");
        QLabel* end = w.findChild<QLabel*>("selectionEnd");
        QLabel* len = w.findChild<QLabel*>("selectionLength");
        uitest::mouseDoubleClick(cur, cur->rect().center());
        check(inPlace() != nullptr, "double-clicking the cursor readout opens a box over it");
        uitest::shot(&w, out + "/readout_edit.png");
        typeInPlace("4.25");
        check(near(app.cursor(), 4.25), "a time typed there moves the cursor (seconds work too)", num(app.cursor()));
        uitest::mouseDoubleClick(end, end->rect().center());
        typeInPlace("00:00:09:00");
        check(near(app.loopStart, 4.25) && near(app.loopEnd, 9.0), "the selection end readout: from the cursor to the time typed", num(app.loopStart) + " " + num(app.loopEnd));
        uitest::mouseDoubleClick(len, len->rect().center());
        typeInPlace("00:00:02:00");
        check(near(app.loopStart, 4.25) && near(app.loopEnd, 6.25), "the length readout: the selection that long", num(app.loopEnd));
        if (setEnd) setEnd->trigger();
        QLineEdit* e2 = inPlace();
        check(e2 && e2->parentWidget() == end->parentWidget(), "Ctrl+Shift+G types in the selection end readout");
        typeInPlace("00:00:08:00");
        check(near(app.loopEnd, 8.0), "Set End of Time Selection", num(app.loopEnd));
        // a ruler that starts elsewhere: typed times are ruler times
        app.edit("Ruler start", [](Project& p) { p.settings.rulerStart = 3600.0; });
        if (goTo) goTo->trigger();
        typeInPlace("01:00:05:00");
        check(near(app.cursor(), 5.0), "with the ruler starting at 1:00:00:00, 01:00:05:00 is 5 s into the project", num(app.cursor()));
        app.undo();
        app.setLoop(0, 0);
    }

    // ------------------------------------------------------------------ the marker bar's Grid Spacing
    {
        QMenu m;
        tl->buildContextMenu(&m, tl->markerBarRect().center() + QPoint(37, 0));
        QMenu* g = subOf(&m, "Grid Spacing");
        const QStringList t = texts(g);
        check(t == QStringList({"Ruler Marks", "-", "Minutes", "Seconds", "Milliseconds", "Frames", "-", "Measures", "Quarter Notes", "Quarter Note Triplets",
                                "8th Notes", "8th Note Triplets", "16th Notes", "16th Note Triplets", "32nd Notes", "32nd Note Triplets"}),
              "the marker bar's menu has VEGAS's Grid Spacing", t.join(" | ").toStdString());
        if (QAction* a = itemOf(g, "Frames")) a->trigger();
        check(tl->options().gridSpacing == "frames", "choosing Frames there sets the grid", tl->options().gridSpacing);
        tl->options().gridSpacing = "ruler";
        tl->optionsChanged();
    }

    // ------------------------------------------------------------------ event thumbnails
    {
        Preferences pr = app.prefs;
        app.setCursor(11.0);  // the cursor's line is away from the pictures looked at
        tl->zoomToFit();
        uitest::settle(200);
        auto picture = [&](const std::string& which, const std::string& numbers, const QString& name) {
            app.prefs.videoThumbnails = which;
            app.prefs.thumbnailNumbers = numbers;
            app.applyPreferences();
            for (int k = 0; k < 15; ++k) { uitest::settle(80); tl->update(); }
            return uitest::shot(tl, out + "/" + name);
        };
        // the first event (0 .. 10 s) on the first track: its middle and its ends
        const Track& vt = app.project().tracks[0];
        const QRect lanes(tl->rulerRect().left(), tl->rulerRect().bottom() + 1, tl->rulerRect().width(), 70);
        auto xOf = [&](double t) { return (int)std::lround(lanes.left() + (t - tl->viewStartTime()) / (tl->viewEndTime() - tl->viewStartTime()) * lanes.width()); };
        const int y = lanes.top() + 40;
        const double e0 = vt.events[0].start, e1 = vt.events[0].start + vt.events[0].length;
        const QRect middle(xOf((e0 + e1) / 2) - 3, y - 5, 6, 10);
        const QRect head(xOf(e0) + 6, y - 5, 6, 10);
        auto flat = [](QColor c) { return std::abs(c.red() - 34) <= 3 && std::abs(c.green() - 34) <= 3 && std::abs(c.blue() - 38) <= 3; };
        const QImage all = picture("all", "none", "thumbs_all.png");
        const QImage none = picture("none", "none", "thumbs_none.png");
        const QImage ht = picture("headTail", "none", "thumbs_head_tail.png");
        const QImage hct = picture("headCenterTail", "frames", "thumbs_hct_frames.png");
        check(countPixels(all, middle, flat) == 0, "All: pictures all along the event");
        check(countPixels(none, middle, flat) == 60 && countPixels(none, head, flat) == 60, "None: no pictures");
        check(countPixels(ht, middle, flat) == 60 && countPixels(ht, head, flat) == 0, "Head, Tail: a picture at the start, none in the middle");
        check(countPixels(hct, middle, flat) == 0, "Head, Center, Tail: one in the middle too");
        // frame numbers: a dark box with light text at each picture's top left
        const QRect corner(xOf(e0) + 2, lanes.top() + 2, 30, 40);
        const int light = countPixels(hct, corner, [](QColor c) { return c.red() > 200 && c.green() > 200 && c.blue() > 200; });
        check(light > 3, "source frame numbers drawn on the pictures", std::to_string(light));
        app.prefs = pr;
        app.applyPreferences();
        // Preferences > Video has both choices
        bool seen = false;
        QTimer::singleShot(300, [&seen] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QDialog* d = qobject_cast<QDialog*>(t))
                    if (d->isVisible()) {
                        QComboBox* a = d->findChild<QComboBox*>("videoThumbnails");
                        QComboBox* b = d->findChild<QComboBox*>("thumbnailNumbers");
                        seen = a && b && a->count() == 5 && b->count() == 4 && a->itemText(1) == "Head, Center, Tail" && b->itemText(1) == "Frame Numbers";
                        if (a) a->setCurrentIndex(3);
                        if (QPushButton* ok = d->findChild<QPushButton*>("ok")) ok->click();
                        else d->accept();
                    }
        });
        if (QAction* p = item("Options/Preferences...")) p->trigger();
        uitest::settle(100);
        check(seen, "Preferences > Video: source frame numbers and thumbnails to show");
        check(app.prefs.videoThumbnails == "head" && QSettings().value("prefs/videoThumbnails").toString() == "head", "the choice is put into effect and kept");
        app.prefs = pr;
        app.applyPreferences();
    }

    // ------------------------------------------------------------------ Project Properties' ruler formats
    {
        bool seen = false;
        QTimer::singleShot(300, [&seen] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QDialog* d = qobject_cast<QDialog*>(t))
                    if (d->isVisible()) {
                        if (QComboBox* c = d->findChild<QComboBox*>("rulerFormat")) seen = c->count() == 15 && c->itemText(3) == "Time & Frames" && c->itemText(12) == "SMPTE Drop (29.97 fps, Video)";
                        d->reject();
                    }
        });
        if (QAction* p = item("File/Properties...")) p->trigger();
        uitest::settle(100);
        check(seen, "Project Properties > Ruler lists the fifteen formats");
    }

    // ------------------------------------------------------------------ track Cut / Copy / Paste / Delete
    {
        buildProject();
        tl->zoomToFit();
        uitest::settle(50);
        const uint64_t v = app.project().tracks[0].id, a = app.project().tracks[1].id;
        app.edit("name", [&](Project& p) { p.findTrack(v)->name = "Pictures"; p.findTrack(v)->solo = true; });
        app.currentTrack = v;
        app.notify(kSelection);
        // the header's menu
        auto headerMenu = [&](int row, QMenu& m) {
            const QRect lanes = tl->rulerRect();
            const int y = lanes.bottom() + 20 + row * 0;  // the first track's header
            (void)y;
            m.clear();
            QPoint at(30, tl->rulerRect().bottom() + 30);
            if (row == 1) {
                // the second track's header: below the first
                for (int yy = at.y(); yy < tl->height(); yy += 4) {
                    QMenu probe;
                    tl->buildContextMenu(&probe, QPoint(30, yy));
                    if (app.currentTrack == a) { at.setY(yy); break; }
                }
            }
            tl->buildContextMenu(&m, at);
            return texts(&m);
        };
        QMenu m;
        const QStringList t = headerMenu(0, m);
        const int cut = (int)t.indexOf("Cut");
        check(cut > 0 && t.mid(cut, 4) == QStringList({"Cut", "Copy", "Paste", "Delete"}) && t.indexOf("Duplicate Track") < cut && !itemOf(&m, "Paste")->isEnabled(),
              "a track's menu: Cut, Copy, Paste (greyed: nothing copied), Delete, after Delete Track (VEGAS's order)", t.join(" | ").toStdString());
        if (QAction* c = itemOf(&m, "Copy")) c->trigger();
        const size_t n0 = app.project().tracks.size();
        // paste on the audio track: the copy goes under it
        headerMenu(1, m);
        check(app.currentTrack == a, "(the audio track's header)");
        QAction* paste = itemOf(&m, "Paste");
        check(paste && paste->isEnabled(), "Paste is offered once a track is copied");
        if (paste) paste->trigger();
        const Project& p1 = app.project();
        check(p1.tracks.size() == n0 + 1 && p1.tracks[2].name == "Pictures" && p1.tracks[2].type == TrackType::Video && p1.tracks[2].solo &&
                  p1.tracks[2].events.size() == p1.tracks[0].events.size() && p1.tracks[2].id != v &&
                  p1.tracks[2].events[0].id != p1.tracks[0].events[0].id && p1.tracks[2].events[0].mediaId == p1.tracks[0].events[0].mediaId,
              "the copy (its events, settings and name, with ids of its own) is pasted under the track clicked");
        check(app.currentTrack == p1.tracks[2].id, "and becomes the current track");
        app.undo();
        check(app.project().tracks.size() == n0, "pasting is one undo step");
        // Cut: the track goes, Paste brings it back
        headerMenu(0, m);
        if (QAction* c = itemOf(&m, "Cut")) c->trigger();
        check(app.project().tracks.size() == n0 - 1 && !const_cast<Project&>(app.project()).findTrack(v), "Cut takes the track away");
        app.currentTrack = app.project().tracks[0].id;
        QMenu m2;
        tl->buildContextMenu(&m2, QPoint(30, tl->rulerRect().bottom() + 30));
        if (QAction* pa = itemOf(&m2, "Paste")) pa->trigger();
        check(app.project().tracks.size() == n0 && app.project().tracks[1].name == "Pictures", "and Paste puts it back (under the track clicked)");
        app.undo();
        app.undo();
        check(app.project().tracks.size() == n0 && const_cast<Project&>(app.project()).findTrack(v), "Cut is one undo step");
        // Delete: the selected tracks
        headerMenu(0, m);
        if (QAction* d = itemOf(&m, "Delete")) d->trigger();
        check(app.project().tracks.size() == n0 - 1, "Delete deletes the track");
        app.undo();
        uitest::shot(&w, out + "/track_menu.png");
    }

    // ------------------------------------------------------------------ Freeze Frame at Cursor, Synchronize
    {
        buildProject();
        tl->zoomToFit();
        uitest::settle(50);
        const Track& vt = app.project().tracks[0];
        const uint64_t ev = vt.events[0].id;
        app.setCursor(3.0);
        app.selectOnly(ev);
        const Track* t0 = nullptr;
        const Event* e0 = app.project().findEvent(ev, &t0);
        // the event's menu, from a point on it
        const QRect lane(tl->rulerRect().left(), tl->rulerRect().bottom() + 1, tl->rulerRect().width(), 80);
        auto xOf = [&](double t) { return (int)std::lround(lane.left() + (t - tl->viewStartTime()) / (tl->viewEndTime() - tl->viewStartTime()) * lane.width()); };
        QMenu m;
        tl->buildContextMenu(&m, QPoint(xOf(e0->start + 6.0), lane.top() + 45));
        QMenu* env = subOf(&m, "Insert/Remove Envelope");
        QAction* freeze = itemOf(env, "Freeze Frame at Cursor");
        check(freeze && freeze->isEnabled(), "a video event's Insert/Remove Envelope has Freeze Frame at Cursor", texts(env).join(" | ").toStdString());
        if (freeze) freeze->trigger();
        const Event* f = app.project().findEvent(ev);
        const MediaItem* fm = f ? app.project().findMedia(f->mediaId) : nullptr;
        const Envelope* vel = f ? findEnvelope(f->envelopes, "velocity") : nullptr;
        check(vel && f && fm && near(eventMediaTime(*f, fm, 2.0), 2.0, 1e-3) && near(eventMediaTime(*f, fm, 3.0), 3.0, 1e-3) &&
                  near(eventMediaTime(*f, fm, 8.0), 3.0, 1e-3),
              "a velocity envelope: the event plays as before up to the cursor, then holds that frame",
              f && fm ? num(eventMediaTime(*f, fm, 2.0)) + " " + num(eventMediaTime(*f, fm, 8.0)) : "");
        app.undo();
        check(!findEnvelope(app.project().findEvent(ev)->envelopes, "velocity"), "one undo step");
        // Synchronize: the picture slipped half a second out of step with its sound
        const Track& at = app.project().tracks[1];
        const uint64_t snd = at.events[0].id;
        QMenu m0;
        tl->buildContextMenu(&m0, QPoint(xOf(6.0), lane.top() + 45));
        check(subOf(&m0, "Synchronize") && !itemOf(subOf(&m0, "Synchronize"), "By Moving")->isEnabled(), "Synchronize is greyed while picture and sound are in step");
        app.edit("Slip", [&](Project& p) { p.findEvent(ev)->offset += 0.5; });
        QMenu m1;
        tl->buildContextMenu(&m1, QPoint(xOf(6.0), lane.top() + 45));
        QMenu* sync = subOf(&m1, "Synchronize");
        check(sync && texts(sync) == QStringList({"By Moving", "By Slipping"}) && itemOf(sync, "By Slipping")->isEnabled(), "out of step: Synchronize > By Moving / By Slipping");
        if (QAction* a = itemOf(sync, "By Slipping")) a->trigger();
        const Event* a1 = app.project().findEvent(ev);
        const Event* s1 = app.project().findEvent(snd);
        check(near(a1->offset, s1->offset) && near(a1->start, s1->start), "By Slipping: the picture's media slips back into step", num(a1->offset) + " " + num(s1->offset));
        app.undo();
        QMenu m2;
        tl->buildContextMenu(&m2, QPoint(xOf(6.0), lane.top() + 45));
        if (QAction* a = itemOf(subOf(&m2, "Synchronize"), "By Moving")) a->trigger();
        const Event* a2 = app.project().findEvent(ev);
        const Event* s2 = app.project().findEvent(snd);
        check(near(a2->start - s2->start, 0.5) && near(a2->offset - a2->start, s2->offset - s2->start), "By Moving: the picture moves so that its media is in step again",
              num(a2->start) + " " + num(s2->start));
        app.undo();
        app.undo();
    }

    // ------------------------------------------------------------------ Edit Details: changed in place
    {
        buildProject();
        uitest::settle(50);
        EditDetails* ed = dynamic_cast<EditDetails*>(w.findChild<QWidget*>("editDetails"));
        QTreeWidget* table = ed ? ed->findChild<QTreeWidget*>("detailsTable") : nullptr;
        check(ed && table, "Edit Details is there");
        if (ed && table) {
            ed->setShow(EditDetails::Events);
            uitest::settle(30);
            const uint64_t e0 = app.project().tracks[0].events[0].id;
            auto rowOf = [&](const QString& id) -> QTreeWidgetItem* {
                for (int r = 0; r < table->topLevelItemCount(); ++r)
                    if (table->topLevelItem(r)->data(0, Qt::UserRole).toString() == id) return table->topLevelItem(r);
                return nullptr;
            };
            auto type = [&](const QString& id, int column, const QString& text) {
                if (QTreeWidgetItem* it = rowOf(id)) it->setText(column, text);
                uitest::settle(30);
            };
            const QString eid = QString("e%1").arg(e0);
            check(rowOf(eid) && (rowOf(eid)->flags() & Qt::ItemIsEditable) && table->editTriggers() & QAbstractItemView::DoubleClicked,
                  "its cells can be edited (double-click, F2)");
            type(eid, 1, "00:00:30:00");
            const Event* e = app.project().findEvent(e0);
            check(e && near(e->start, 30.0) && near(e->length, 10.0), "Start: the event moves there", e ? num(e->start) : "");
            app.undo();
            check(near(app.project().findEvent(e0)->start, 0.0), "one undo step");
            type(eid, 3, "00:00:04:00");
            check(near(app.project().findEvent(e0)->length, 4.0), "Length", num(app.project().findEvent(e0)->length));
            type(eid, 2, "00:00:06:15");
            check(near(app.project().findEvent(e0)->length, 6.5), "End", num(app.project().findEvent(e0)->length));
            type(eid, 4, "Opening");
            check(app.project().findEvent(e0)->name == "Opening", "Name");
            type(eid, 6, "1.5");
            type(eid, 7, "00:00:20:00");
            check(near(app.project().findEvent(e0)->fadeIn, 1.5) && near(app.project().findEvent(e0)->fadeOut, 6.5), "Fade In, Fade Out (no longer than the event)",
                  num(app.project().findEvent(e0)->fadeIn) + " " + num(app.project().findEvent(e0)->fadeOut));
            type(eid, 8, "Yes");
            check(app.project().findEvent(e0)->mute, "Muted");
            const double before = app.project().findEvent(e0)->start;
            type(eid, 1, "whenever");
            check(near(app.project().findEvent(e0)->start, before) && rowOf(eid) && rowOf(eid)->text(1) == QString::fromStdString(rulerTime(app.project(), before)),
                  "text that is not a time changes nothing (the cell shows the value again)");
            uitest::shot(w.findChild<QWidget*>("editDetails"), out + "/edit_details.png");
            // regions
            app.edit("Region", [](Project& p) {
                Project::Marker m;
                m.id = p.newId();
                m.time = 2.0;
                m.length = 3.0;
                m.region = true;
                m.name = "Verse";
                p.markers.push_back(m);
            });
            const uint64_t rid = app.project().markers.back().id;
            ed->setShow(EditDetails::Regions);
            uitest::settle(30);
            const QString mid = QString("m%1").arg(rid);
            type(mid, 3, "00:00:05:00");
            type(mid, 1, "00:00:01:00");
            type(mid, 4, "Chorus");
            const Project::Marker& r = app.project().markers.back();
            check(near(r.time, 1.0) && near(r.length, 5.0) && r.name == "Chorus", "a region's position, length and name", num(r.time) + " " + num(r.length) + " " + r.name);
            ed->setShow(EditDetails::Events);
        }
    }

    // ------------------------------------------------------------------ Save As: Copy media with project
    {
        // a sound file somewhere else
        const QString src = QDir(out).absoluteFilePath("elsewhere");
        const QString dst = QDir(out).absoluteFilePath("project folder");
        QDir(src).removeRecursively();
        QDir(dst).removeRecursively();
        QDir().mkpath(src);
        QDir().mkpath(dst);
        const QString wav = src + "/tone.wav";
        {
            QFile f(wav);
            f.open(QIODevice::WriteOnly);
            const int rate = 48000, n = rate;           // one second of a 440 Hz tone, mono 16-bit
            auto u32 = [&f](uint32_t v) { f.write((const char*)&v, 4); };
            auto u16 = [&f](uint16_t v) { f.write((const char*)&v, 2); };
            f.write("RIFF"); u32(36 + n * 2); f.write("WAVEfmt "); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
            f.write("data"); u32(n * 2);
            for (int k = 0; k < n; ++k) u16((uint16_t)(int16_t)(8000 * std::sin(2 * M_PI * 440 * k / rate)));
        }
        app.newProject();
        std::string err;
        const uint64_t media = app.importMedia(wav.toStdString(), &err);
        const uint64_t at = app.addTrack(TrackType::Audio);
        app.addMediaToTimeline(media, 0.0, at);
        check(media != 0, "(a sound file to copy)", err);
        bool sawBox = false;
        QTimer::singleShot(300, [&] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QFileDialog* d = qobject_cast<QFileDialog*>(t))
                    if (d->isVisible() && d->objectName() == "saveAsDialog") {
                        QCheckBox* c = d->findChild<QCheckBox*>("copyMedia");
                        sawBox = c && c->text() == "Copy media with project" && !c->isChecked();
                        uitest::shot(d, out + "/save_as_copy_media.png");
                        if (c) c->setChecked(true);
                        if (QLineEdit* name = d->findChild<QLineEdit*>("fileNameEdit")) name->setText(dst + "/copied.veg");
                        static_cast<QDialog*>(d)->accept();
                    }
        });
        // a message box would stop the test: answered, and what it said kept
        std::string boxes;
        QTimer catcher;
        QObject::connect(&catcher, &QTimer::timeout, [&boxes] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QMessageBox* b = qobject_cast<QMessageBox*>(t))
                    if (b->isVisible()) { boxes += b->text().toStdString() + " / "; b->reject(); }
        });
        catcher.start(200);
        QAction* saveAs = item("File/Save As...");
        if (saveAs) saveAs->trigger();
        const bool saved = saveAs != nullptr;
        check(sawBox, "Save As has VEGAS's \"Copy media with project\" (off)");
        const MediaItem* m = app.project().findMedia(media);
        check(saved && QFile::exists(dst + "/copied.veg") && QFile::exists(dst + "/tone.wav") && m && QString::fromStdString(m->path) == dst + "/tone.wav",
              "the sound file is copied beside the project, and the project uses the copy", (m ? m->path : "") + " " + boxes);
        check(QFile::exists(wav), "the original stays where it was");
        // the saved project reads back with the copy
        std::string err2;
        Project back;
        const bool read = loadProjectFile((dst + "/copied.veg").toStdString(), back, &err2);
        check(read && !back.media.empty() && QString::fromStdString(back.media[0].path).endsWith("project folder/tone.wav"), "the .veg written points at the copy",
              read && !back.media.empty() ? back.media[0].path : err2);
        // again, into the same folder: the copy is used again, not copied a second time
        app.undo();
        QTimer::singleShot(300, [&] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QFileDialog* d = qobject_cast<QFileDialog*>(t))
                    if (d->isVisible() && d->objectName() == "saveAsDialog") {
                        if (QCheckBox* c = d->findChild<QCheckBox*>("copyMedia")) c->setChecked(true);
                        if (QLineEdit* name = d->findChild<QLineEdit*>("fileNameEdit")) name->setText(dst + "/again.veg");
                        static_cast<QDialog*>(d)->accept();
                    }
        });
        if (saveAs) saveAs->trigger();
        check(QDir(dst).entryList(QStringList() << "*.wav", QDir::Files).size() == 1, "saved again with copies: the copy made before is used again",
              QDir(dst).entryList(QDir::Files).join(",").toStdString());
    }

    // ------------------------------------------------------------------ crash recovery
    {
        const QString dir = QDir(out).absoluteFilePath("sessions");
        QDir(dir).removeRecursively();
        session::begin(dir);        // this run
        check(session::crashed(dir).empty(), "this run's own record is not a crash");
        // a run that crashes after auto-saving a project with three audio tracks
        std::fflush(nullptr);
        const int rc = QProcess::execute(QCoreApplication::applicationFilePath(), {"--crash-child", dir});
        check(rc == 3, "the crashing run ran", std::to_string(rc));
        std::vector<CrashedSession> c = session::crashed(dir);
        check(c.size() == 1 && c[0].recoverable && c[0].project.isEmpty() && QFile::exists(c[0].autoSave), "a run that ended without closing is found, with its auto-saved copy",
              c.empty() ? "none" : c[0].autoSave.toStdString());
        check(session::crashed(dir).size() == 1, "(asking does not forget it)");
        // the question (VEGAS's words), answered No: nothing opens, and it is not asked again
        buildProject();
        const size_t tracksBefore = app.project().tracks.size();
        QString asked;
        bool restored = session::offerRecovery(&w, [](const CrashedSession&) { return true; }, [&asked](const QString& q) { asked = q; return false; }, dir);
        check(!restored && asked.startsWith("The last session did not complete properly.") && asked.contains("Would you like to restore the autosaved project?"),
              "VEGAS's question is asked", asked.toStdString());
        check(session::crashed(dir).empty() && app.project().tracks.size() == tracksBefore, "No: nothing opens, and it is not asked again");
        // again, answered Yes through the main window's message box
        std::fflush(nullptr);
        QProcess::execute(QCoreApplication::applicationFilePath(), {"--crash-child", dir});
        QTimer::singleShot(300, [] {
            for (QWidget* t : QApplication::topLevelWidgets())
                if (QMessageBox* b = qobject_cast<QMessageBox*>(t))
                    if (b->isVisible() && b->objectName() == "recoverProject") {
                        b->button(QMessageBox::Yes)->click();
                    }
        });
        // MainWindow::offerRecovery looks in the usual folder: point it at the test's by running the same steps
        restored = session::offerRecovery(&w, [&](const CrashedSession& s) {
            std::string err;
            if (!app.open(s.autoSave.toStdString(), &err)) return false;
            app.untieFromFile();
            return true;
        }, nullptr, dir);
        int audio = 0;
        for (const Track& t : app.project().tracks) audio += t.type == TrackType::Audio ? 1 : 0;
        check(restored && audio == 3 && app.path().empty() && app.dirty(), "Yes: the auto-saved project opens, untitled and unsaved (Save asks for a name)",
              std::to_string(audio) + " " + app.path());
        // MainWindow::offerRecovery with nothing crashed: no question
        check(!w.offerRecovery(), "nothing to recover: nothing asked");
        session::end();
        check(QDir(dir).entryList(QDir::Files).isEmpty(), "a clean exit leaves no record behind", QDir(dir).entryList(QDir::Files).join(",").toStdString());
    }

    return uitest::summary();
}
