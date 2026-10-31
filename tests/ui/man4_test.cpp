// VEGAS Pro 18 manual pages 365-522: closed captions (import, Command Properties' 608CC commands, the Video Preview's CC overlays,
// export, promote), the Video Preview (360° mode, Retain Split Screen Setting, visible transport buttons, Display at Project Size,
// Show Transport Bar), ACES color management, Mute All Audio / Video, Loudness Meters' options and Generate Loudness Log, Video
// Scopes settings, composite waveform and histogram types, Lay Out Audio CD from Events, Relink Wireless Adapter Media, and the menu
// entries for what needs hardware (Burn Disc, Print Video to Tape, XDCAM, External Control).
//
//   man4_test <output folder>
//
// Build: uibuild man4_test tests/ui/man4_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/mainwindow_tools.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/loudnesswin.h"
#include "../../src/app/outputcmds.h"
#include "../../src/app/previewextras.h"
#include "../../src/app/audiotools.h"
#include "../../src/engine/captions.h"
#include "../../src/engine/colormgmt.h"
#include "uitestlib.h"
#include <QLineEdit>
#include <QPushButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QMenu>
#include <QMenuBar>
#include <QSettings>
#include <QTimer>
#include <QToolButton>
#include <cmath>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
uint64_t clip = 0, snd = 0, v1 = 0, a1 = 0;

void buildProject()
{
    st().newProject();
    std::string err;
    clip = st().importMedia("synth:1", &err);
    snd = st().importMedia("synthaudio:2", &err);
    st().edit("Build", [&](Project& p) {
        for (int k = 0; k < 2; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k == 0 ? TrackType::Video : TrackType::Audio;
            p.tracks.push_back(t);
        }
        v1 = p.tracks[0].id;
        a1 = p.tracks[1].id;
        Event e;
        e.id = p.newId();
        e.mediaId = clip;
        e.start = 0;
        e.length = 4;
        p.tracks[0].events.push_back(e);
        // two audio events touching (one CD track) and one apart (another)
        for (double at : {0.0, 3.0, 7.0}) {
            Event m;
            m.id = p.newId();
            m.mediaId = snd;
            m.start = at;
            m.length = 3;
            p.tracks[1].events.push_back(m);
        }
    });
}

QAction* action(MainWindow& w, const char* name) { return w.findChild<QAction*>(name); }

// Does a menu (or one inside it) hold the action called 'name'?
bool menuHas(QMenu* m, const QString& name)
{
    if (!m) return false;
    for (QAction* a : m->actions()) {
        if (a->objectName() == name) return true;
        if (a->menu() && menuHas(a->menu(), name)) return true;
    }
    return false;
}

QMenu* topMenu(MainWindow& w, const QString& title)
{
    for (QAction* a : w.menuBar()->actions())
        if (a->menu() && a->text().remove('&') == title) return a->menu();
    return nullptr;
}

// Runs fn on the next modal dialog that opens (then closes it with 'accept').
void onDialog(std::function<void(QDialog*)> fn, bool accept = true)
{
    QTimer::singleShot(150, [fn, accept] {
        QDialog* d = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!d) return;
        fn(d);
        if (accept) d->accept();
        else d->reject();
    });
}

// The share of a picture's pixels in 'r' that are nearly white.
double whiteShare(const QImage& img, const QRect& r)
{
    int n = 0, white = 0;
    for (int y = std::max(0, r.top()); y <= std::min(img.height() - 1, r.bottom()); ++y)
        for (int x = std::max(0, r.left()); x <= std::min(img.width() - 1, r.right()); ++x) {
            const QRgb c = img.pixel(x, y);
            ++n;
            if (qRed(c) > 200 && qGreen(c) > 200 && qBlue(c) > 200) ++white;
        }
    return n ? (double)white / n : 0;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoMan4Test");
    QApplication::setApplicationName("man4_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = QDir(args.size() > 1 ? args[1] : QString("man4out")).absolutePath();
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    buildProject();
    MainWindow w;
    w.resize(1500, 900);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    buildProject();
    uitest::settle(200);

    // ================================================================================== the menus
    {
        QMenu* file = topMenu(w, "File");
        QMenu* tools = topMenu(w, "Tools");
        QMenu* options = topMenu(w, "Options");
        QMenu* view = topMenu(w, "View");
        check(menuHas(file, "importClosedCaptioning") && menuHas(file, "exportXdcamDisc"), "File > Import > Closed Captioning and File > Export > XDCAM Disc");
        check(menuHas(view, "showXdcamExplorer"), "View > Window > XDCAM Explorer is listed");
        check(menuHas(tools, "burnTaoCd") && menuHas(tools, "burnDaoCd") && menuHas(tools, "burnDvd") && menuHas(tools, "burnBluray") &&
                  menuHas(tools, "printToTape") && menuHas(tools, "printToHdvTape"),
              "Tools has Burn Disc's four commands and both Print Video to Tape commands");
        check(!action(w, "burnDvd")->isEnabled() && !action(w, "printToTape")->isEnabled() && !action(w, "externalControl")->isEnabled() &&
                  !action(w, "burnDvd")->property("notAvailable").toString().isEmpty(),
              "the hardware commands are greyed and say why");
        check(menuHas(tools, "relinkWireless") && menuHas(tools, "layOutAudioCd") && menuHas(tools, "generateLoudnessLog"),
              "Tools has Relink Wireless Adapter Media, Lay Out Audio CD from Events and Generate Loudness Log");
        check(menuHas(tools, "scriptPromoteCaptions") && menuHas(tools, "scriptExportCaptionsYouTube") && menuHas(tools, "scriptExportCaptionsDvda") &&
                  menuHas(tools, "scriptExportCaptionsWmp") && menuHas(tools, "scriptExportCaptionsQuickTime"),
              "Tools > Scripting has the closed captioning commands");
        // kept when the Script Menu folder is scanned again
        if (QAction* rescan = action(w, "rescanScripts")) rescan->trigger();
        uitest::settle(50);
        check(menuHas(tools, "scriptPromoteCaptions") && menuHas(tools, "scriptExportCaptionsYouTube"), "a rescan of the Script Menu folder keeps them");
        check(menuHas(options, "muteAllAudio") && menuHas(options, "muteAllVideo") && menuHas(options, "externalControl") && menuHas(options, "loudnessMetersMenu"),
              "Options has Mute All Audio, Mute All Video, Loudness Meters and External Control");
    }

    // ================================================================================== Mute All Audio / Video
    {
        Playback* pb = w.playback();
        st().setCursor(1.0);
        pb->requestFrame();
        uitest::settle(400);
        const QImage before = pb->lastFrame();
        action(w, "muteAllVideo")->trigger();
        uitest::settle(500);
        const QImage after = pb->lastFrame();
        bool black = !after.isNull();
        for (int y = 0; black && y < after.height(); y += 7)
            for (int x = 0; x < after.width(); x += 7)
                if (qGray(after.pixel(x, y)) > 8) { black = false; break; }
        check(st().muteAllVideo && black && !before.isNull(), "Mute All Video: the preview shows no video track");
        action(w, "muteAllVideo")->trigger();
        action(w, "muteAllAudio")->trigger();
        check(st().muteAllAudio && action(w, "muteAllAudio")->isChecked(), "Mute All Audio is ticked and on");
        action(w, "muteAllAudio")->trigger();
        check(!st().muteAllAudio && !st().muteAllVideo, "and both go off again");
    }

    // ================================================================================== closed captions
    PreviewWidget* pv = w.preview();
    {
        const QString srt = QDir(out).filePath("captions.srt");
        QFile f(srt);
        f.open(QIODevice::WriteOnly);
        f.write("1\r\n00:00:00,500 --> 00:00:01,500\r\nHello there\r\n\r\n2\r\n00:00:02,000 --> 00:00:03,000\r\nSecond <i>caption</i>\r\nTwo lines\r\n\r\n");
        f.close();
        std::string err;
        const int n = importClosedCaptions(srt.toStdString(), &err);
        const std::vector<Caption> caps = projectCaptions(st().project());
        check(n == 4 && caps.size() == 4 && std::fabs(caps[0].time - 0.5) < 1e-6 && captionText(caps[0].markup) == "Hello there" && caps[1].markup == "{EDM}" &&
                  captionText(caps[2].markup) == "Second caption[BR]Two lines",
              "File > Import > Closed Captioning: an .srt becomes 608CC1 command markers (with the screen cleared at each end)",
              strfmt("%d markers, %s", n, caps.empty() ? "" : caps[0].markup.c_str()));
        // the overlay: CC1 over the picture at 0.7 s, nothing at 1.7 s
        pv->setCaptionChannel(1);
        st().setCursor(0.7);
        uitest::settle(300);
        const QRect t = pv->pictureRect();
        QWidget* pic = pv->pictureWidget();
        QImage g = pic->grab().toImage();
        uitest::shot(pv, out + "/caption_overlay.png");
        const double shown = whiteShare(g, QRect(t.left(), t.top() + t.height() * 3 / 4, t.width(), t.height() / 4));
        st().setCursor(1.7);
        uitest::settle(300);
        g = pic->grab().toImage();
        const double cleared = whiteShare(g, QRect(t.left(), t.top() + t.height() * 3 / 4, t.width(), t.height() / 4));
        check(pv->captionChannel() == 1 && shown > 0.0005 && cleared < shown * 0.5, "the Video Preview's CC1 overlay shows the caption, and the cleared screen none",
              strfmt("white %.4f then %.4f", shown, cleared));
        pv->setCaptionChannel(0);
        // export
        const QString yt = QDir(out).filePath("export.srt"), dvda = QDir(out).filePath("export.sub"), wmp = QDir(out).filePath("export.smi");
        const QString qt = QDir(out).filePath("export.txt"), scc = QDir(out).filePath("export.scc");
        bool ok = exportClosedCaptions(yt.toStdString(), CaptionFormat::Srt, &err) && exportClosedCaptions(dvda.toStdString(), CaptionFormat::Sub, &err) &&
                  exportClosedCaptions(wmp.toStdString(), CaptionFormat::Smi, &err) && exportClosedCaptions(qt.toStdString(), CaptionFormat::QuickTime, &err) &&
                  exportClosedCaptions(scc.toStdString(), CaptionFormat::Scc, &err);
        std::string text;
        readFile(yt.toStdString(), text);
        check(ok && text.find("00:00:00,500 --> 00:00:01,500") != std::string::npos && text.find("Second caption\r\nTwo lines") != std::string::npos &&
                  QFileInfo::exists(QDir(out).filePath("export.asx")) && QFileInfo::exists(QDir(out).filePath("export.smil")),
              "Export Closed Captioning for YouTube, DVD Architect, Windows Media Player and QuickTime write their files", err);
        // what was exported reads back the same
        std::vector<Caption> back;
        readCaptionFile(scc.toStdString(), 29.97, back, &err);
        std::vector<Caption> back2;
        readCaptionFile(dvda.toStdString(), st().project().settings.fps(), back2, &err);
        check(back.size() == 4 && captionText(back[2].markup) == "Second caption[BR]Two lines" && std::fabs(back[2].time - 2.0) < 0.05 &&
                  back2.size() == 4 && captionText(back2[2].markup) == "Second caption[BR]Two lines" && back2[1].markup == "{EDM}",
              "the .scc and the .sub read back as the captions they were made from", strfmt("%d, %d", (int)back.size(), (int)back2.size()));
        // promote: an .scc beside the media file (the made-up media have no file, so a real one is made)
        st().edit("Captions out", [](Project& p) { p.markers.clear(); });
    }

    // ================================================================================== Lay Out Audio CD, the loudness log
    {
        const int n = layOutAudioCdFromEvents();
        int regions = 0;
        double firstLen = 0;
        for (const auto& m : st().project().markers)
            if (m.kind == "cdtrack") {
                ++regions;
                if (std::fabs(m.time) < 1e-6) firstLen = m.length;
            }
        check(n == 2 && regions == 2 && std::fabs(firstLen - 6.0) < 1e-6, "Lay Out Audio CD from Events: touching events are one track, the event apart another",
              strfmt("%d tracks, the first %.2f s", regions, firstLen));
        std::string path, err;
        const QString logPath = QDir(out).filePath("timeline_loud.txt");
        const bool ok = generateLoudnessLog(logPath.toStdString(), &path, &err);
        std::string text;
        readFile(path, text);
        check(ok && text.find("Integrated:") != std::string::npos && text.find("EBU R 128") != std::string::npos && text.find("00:00:01.000") != std::string::npos,
              "Tools > Generate Loudness Log writes the timeline's loudness, its mode and the loudness every second", err);
    }

    // ================================================================================== the Loudness Meters' options
    {
        MainWindow::Tools* tools = w.tools();
        LoudnessWindow* lw = tools ? tools->loudness : nullptr;
        if (lw) {
            tools->loudnessDock->show();
            uitest::settle(100);
            lw->setAtscMode(true);
            check(lw->atscMode() && std::fabs(lw->target() + 24) < 1e-9 && QSettings().value("loudness/mode").toString() == "atsc",
                  "ATSC A 85 Mode: the target is -24 LUFS (and kept)");
            lw->setAtscMode(false);
            QMenu m;
            fillLoudnessMenu(&m, lw);
            QStringList names;
            for (QAction* a : m.actions()) names << a->text();
            check(names.contains("EBU R 128 Mode") && names.contains("ATSC A 85 Mode") && names.contains("Absolute (-23 LUFS)") &&
                      names.contains("Show True Peak Meter") && names.contains("True Peak Blocking Filter") && names.contains("Reset Clip") &&
                      names.contains("Reset Metering Engine") && names.contains("Loudness Scale"),
                  "the Loudness Meters' menu has VEGAS's options", names.join(", ").toStdString());
            // a loud project: the true peak over the maximum lights the indicator, Reset Clip puts it out
            lw->measure(0.0, 4.0);
            uitest::settle(200);
            const LoudnessWindow::Reading r = lw->reading();
            const bool lit = lw->truePeakOver();
            lw->resetClip();
            uitest::settle(150);
            check((r.truePeak > -1.0) == lit && !lw->truePeakOver(), "the true peak indicator, and Reset Clip", strfmt("TP %.2f", r.truePeak));
            lw->setWideScale(true);
            lw->setAbsolute(true);
            lw->resize(520, 260);
            uitest::settle(150);
            uitest::shot(lw, out + "/loudness_meters.png");
            check(lw->wideScale() && lw->absolute(), "EBU +18 and Absolute");
            lw->setWideScale(false);
            lw->setAbsolute(false);
        } else {
            check(false, "the Loudness Meters window is there");
        }
    }

    // ================================================================================== the Video Scopes
    {
        VideoScopes scopes(w.playback());
        scopes.resize(520, 300);
        scopes.show();
        QImage img(64, 64, QImage::Format_RGB32);
        img.fill(QColor(16, 16, 16));
        for (int y = 0; y < 32; ++y)
            for (int x = 0; x < 64; ++x) img.setPixel(x, y, qRgb(235, 235, 235));
        scopes.setImage(img);
        check(std::fabs(scopes.waveformIre(0)) < 1e-9 && std::fabs(scopes.waveformIre(255) - 100) < 1e-9, "the waveform reads 0..255 as 0..100 IRE");
        scopes.setDisplaySettings(false, true);
        check(std::fabs(scopes.waveformIre(16)) < 1e-9 && std::fabs(scopes.waveformIre(235) - 100) < 1e-9, "Studio RGB: 16 is 0 IRE and 235 is 100");
        scopes.setDisplaySettings(true, true);
        check(std::fabs(scopes.waveformIre(16) - 7.5) < 1e-9, "7.5 IRE setup: black at 7.5");
        scopes.setDisplaySettings(false, false);
        double mean = 0, sd = 0;
        scopes.setScope(VideoScopes::Histogram);
        scopes.setHistogramType(VideoScopes::HistRed);
        scopes.histogramStats(VideoScopes::HistRed, mean, sd);
        check(std::fabs(mean - 125.5) < 0.6 && std::fabs(sd - 109.5) < 0.6, "the histogram's mean and standard deviation", strfmt("%.2f, %.2f", mean, sd));
        QComboBox* mode = scopes.findChild<QComboBox*>("scopeMode");
        check(mode && mode->count() == 7 && mode->itemText(6) == "Lightness/a/b" && mode->itemText(4) == "Alpha", "the histogram's seven charts");
        scopes.setHistogramType(VideoScopes::HistLab);
        uitest::settle(50);
        uitest::shot(&scopes, out + "/scope_histogram_lab.png");
        scopes.setScope(VideoScopes::Waveform);
        check(mode->count() == 2 && mode->itemText(0) == "Composite", "the waveform's Composite and Luminance");
        scopes.setWaveformComposite(true);
        uitest::settle(50);
        uitest::shot(&scopes, out + "/scope_waveform_composite.png");
        scopes.setScope(VideoScopes::Vectorscope);
        uitest::settle(50);
        uitest::shot(&scopes, out + "/scope_vectorscope.png");
        check(scopes.findChild<QToolButton*>("scopesSettings") != nullptr, "the Settings button");
    }

    // ================================================================================== the Video Preview
    {
        // 360°: only for a 360° project
        QToolButton* b360 = pv->findChild<QToolButton*>("preview360");
        check(b360 && !b360->isEnabled(), "the 360° Preview button is greyed in a project that is not 360°");
        st().edit("360", [](Project& p) { p.settings.output360 = true; });
        uitest::settle(100);
        check(b360 && b360->isEnabled(), "and works in a 360° one");
        st().setCursor(1.0);
        uitest::settle(300);
        const QImage flat = pv->pictureWidget()->grab().toImage();
        pv->set360Mode(true);
        pv->set360View(90, 0, 60);
        uitest::settle(200);
        const QImage round = pv->pictureWidget()->grab().toImage();
        uitest::shot(pv, out + "/preview_360.png");
        check(pv->mode360() && b360->isChecked() && flat != round, "360° mode looks round the picture");
        pv->set360Mode(false);
        // transport: the visible button set
        pv->setVisibleTransportButtons({"record", "loop", "play", "pause", "stop", "goToEnd", "nextFrame"});
        uitest::settle(100);
        QWidget* rec = pv->findChild<QWidget*>("previewRecord");
        QWidget* toEnd = pv->findChild<QWidget*>("previewGoToEnd");
        QWidget* fromStart = pv->findChild<QWidget*>("previewPlayFromStart");
        check(rec && toEnd && fromStart && rec->isVisible() && !fromStart->isVisible(), "Edit Visible Button Set: the buttons chosen show");
        uitest::shot(pv, out + "/preview_transport.png");
        pv->setVisibleTransportButtons({"play", "pause", "stop"});
        check(QSettings().value("preview/transportButtons").toStringList().size() == 3, "and are kept for the next start");
        // Retain Split Screen Setting
        QSettings().setValue("preview/retainSplit", true);
        pv->setSplitScreen(true, false);
        check(QSettings().value("preview/splitOn").toBool(), "with Retain Split Screen Setting, the split-screen view is kept");
        pv->setSplitScreen(false, false);
        QSettings().setValue("preview/retainSplit", false);
        st().edit("not 360", [](Project& p) { p.settings.output360 = false; });
    }

    // ================================================================================== Project Properties: color management, 360°
    {
        bool sawPixel = false, sawViews = false, greyed = false, saw360 = false;
        onDialog([&](QDialog* d) {
            QComboBox* format = d->findChild<QComboBox*>("pixelFormat");
            QComboBox* view = d->findChild<QComboBox*>("viewTransform");
            QComboBox* aces = d->findChild<QComboBox*>("acesVersion");
            QCheckBox* o360 = d->findChild<QCheckBox*>("output360");
            sawPixel = format && format->findData("32bit|full") >= 0 && format->findData("32bit|video") >= 0;
            greyed = view && !view->isEnabled();
            if (format) format->setCurrentIndex(format->findData("32bit|full"));
            if (aces) aces->setCurrentIndex(aces->findData("1.0"));
            sawViews = view && view->isEnabled() && view->count() == (int)viewTransforms("1.0").size() && view->findData("rrt_srgb") >= 0;
            if (view) view->setCurrentIndex(view->findData("rrt_srgb"));
            saw360 = o360 != nullptr;
            if (o360) o360->setChecked(true);
            if (QPushButton* ok = d->findChild<QPushButton*>("ok")) ok->click();
        }, false);
        if (QAction* props = action(w, "properties")) props->trigger();
        uitest::settle(100);
        const ProjectSettings& s = st().project().settings;
        check(sawPixel && greyed && sawViews && saw360, "Project Properties: 32-bit formats, ACES version, View transform, 360° output");
        check(s.pixelFormat == "32bit" && s.pixelRange == "full" && s.viewTransform == "rrt_srgb" && s.output360, "and they are kept",
              s.pixelFormat + " " + s.viewTransform);
        // the picture through the view transform is not the plain one
        st().setCursor(1.0);
        // a 32-bit frame takes a while: wait until the preview shows a picture other than the one before
        auto nextFrame = [&](const QImage& before) {
            QImage img;
            for (int i = 0; i < 40; ++i) {
                w.playback()->requestFrame();
                uitest::settle(250);
                img = w.playback()->lastFrame();
                // a draft at half size may come first while frames are slow; wait for the full one
                if (!img.isNull() && img != before && img.width() == st().project().settings.width) break;
            }
            return img;
        };
        const QImage aces = nextFrame(w.playback()->lastFrame());
        st().edit("Off", [](Project& p) { p.settings.viewTransform = "off"; });
        const QImage off = nextFrame(aces);
        st().edit("8-bit", [](Project& p) { p.settings.pixelFormat = "8bit"; p.settings.output360 = false; });
        const QImage eight = nextFrame(off);
        int diffAces = 0, diffOff = 0, n = 0;
        if (!aces.isNull() && !off.isNull() && !eight.isNull() && aces.size() == eight.size() && off.size() == eight.size())
            for (int y = 0; y < eight.height(); y += 5)
                for (int x = 0; x < eight.width(); x += 5) {
                    ++n;
                    diffAces = std::max(diffAces, std::abs(qGray(aces.pixel(x, y)) - qGray(eight.pixel(x, y))));
                    diffOff = std::max(diffOff, std::abs(qGray(off.pixel(x, y)) - qGray(eight.pixel(x, y))));
                }
        aces.save(out + "/aces_frame.png");
        off.save(out + "/off_frame.png");
        eight.save(out + "/eight_frame.png");
        check(n > 0 && diffOff <= 3 && diffAces > 10, "full range with the View transform Off looks as 8-bit does; through the ACES RRT it does not",
              strfmt("off %d, aces %d", diffOff, diffAces));
    }

    // ================================================================================== Command Properties: 608CC
    {
        bool listed = false;
        onDialog([&](QDialog* d) {
            QComboBox* c = d->findChild<QComboBox*>("command");
            QLineEdit* p = d->findChild<QLineEdit*>("parameter");
            listed = c && c->findText("608CC1") >= 0 && c->findText("608CC4") >= 0;
            if (c) c->setCurrentText("608CC1");
            if (p) p->setText("Typed caption[BR]second row");
        });
        st().setCursor(2.5);
        if (QAction* cmd = action(w, "commandMarker")) cmd->trigger();
        uitest::settle(100);
        const std::vector<Caption> caps = projectCaptions(st().project());
        check(listed && caps.size() == 1 && caps[0].markup.compare(0, 10, "{RCL}{ENM}") == 0 && captionText(caps[0].markup) == "Typed caption[BR]second row",
              "Insert > Command: 608CC1 to 608CC4, and typed text becomes a pop-on caption", caps.empty() ? std::string() : caps[0].markup);
    }

    // ================================================================================== VEGAS Pro 23: audio editing, rendering, automation
    {
        QMenu* insertM = topMenu(w, "Insert");
        QMenu* toolsM = topMenu(w, "Tools");
        QMenu* optionsM = topMenu(w, "Options");
        check(menuHas(insertM, "assignableFx") && menuHas(toolsM, "extractAudioCd") && menuHas(optionsM, "bypassAudioFx") && menuHas(optionsM, "metronome"),
              "Insert > Audio Assignable FX, Tools > Extract Audio from CD, Options > Bypass All Audio FX and Metronome are listed");
        check(!action(w, "extractAudioCd")->isEnabled() && !action(w, "extractAudioCd")->property("notAvailable").toString().isEmpty(), "Extract Audio from CD is greyed and says why");

        // Bypass All Audio FX
        action(w, "bypassAudioFx")->trigger();
        const bool on = st().bypassAudioFx.load();
        action(w, "bypassAudioFx")->trigger();
        check(on && !st().bypassAudioFx.load(), "Options > Bypass All Audio FX turns the bypass on and off");

        // Assignable FX
        const size_t before = st().project().buses.size();
        // the new chain's Plug-In Chooser opens: closed without a choice
        onDialog([](QDialog*) {}, false);
        action(w, "assignableFx")->trigger();
        uitest::settle(300);
        onDialog([](QDialog*) {}, false);
        action(w, "assignableFx")->trigger();
        uitest::settle(300);
        int assignable = 0;
        std::string lastName;
        for (const Bus& b : st().project().buses)
            if (b.assignable) { ++assignable; lastName = b.name; }
        check(st().project().buses.size() == before + 2 && assignable == 2 && lastName == "FX 2", "Insert > Audio Assignable FX adds FX 1, FX 2", lastName);
        st().undo();
        st().undo();
        check(st().project().buses.size() == before, "and undo takes them away");

        // Beat Detection and Tempo Detection dialogs
        bool beat = false, tempo = false;
        onDialog([&](QDialog* d) { beat = d->objectName() == "beatDetection"; d->grab().save(out + "/beat_detection.png"); });
        showBeatDetectionDialog(&w, snd);
        onDialog([&](QDialog* d) { tempo = d->objectName() == "tempoDetection"; d->grab().save(out + "/tempo_detection.png"); });
        showTempoDetectionDialog(&w, snd);
        check(beat && tempo, "Beat Detection and Tempo Detection open their dialogs");

        // Render As: the Filters menu, View and Sort
        bool filters = false;
        onDialog([&](QDialog* d) {
            filters = (d->findChild<QAction*>("filterMatchProject") && d->findChild<QAction*>("showAllFormats"));
            d->grab().save(out + "/render_as.png");
        }, false);
        action(w, "render")->trigger();
        check(filters, "Render As has the Match project settings filter and Show all formats");

        // the Mixing Console with automation and phase buttons
        if (QDockWidget* dock = w.findChild<QDockWidget*>("dockMixer"))
            if (!dock->isVisible()) dock->show();
        uitest::settle(300);
        const QList<QAbstractButton*> autos = w.findChildren<QAbstractButton*>("automation");
        QList<QAbstractButton*> phases = w.findChildren<QAbstractButton*>("phase");
        phases += w.findChildren<QAbstractButton*>("invertPhase");  // the strips' Invert Track Phase button
        check(!autos.isEmpty() && !phases.isEmpty(), "the Mixing Console's strips have Automation and Phase buttons", strfmt("%d %d", (int)autos.size(), (int)phases.size()));
        w.grab().save(out + "/mixer_vp23.png");
    }

    return uitest::summary();
}
