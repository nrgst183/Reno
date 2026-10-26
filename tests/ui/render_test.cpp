// File > Render As (src/app/renderdialog.cpp) and the rendering under it (src/engine/renderas.cpp, src/media/ffmpeg_writer.cpp):
// the template list, search, favourites, Customize Template and custom templates, every format rendered and read back, letterbox /
// stretch, output rotation, chapters, two passes, the loudness log, an encoder that can't start falling back, cancelling.
//
//   render_test <output folder> [testfx.ofx ...]
//
// QT_QPA_PLATFORM=offscreen. The folder gets the pictures and the rendered files.
#include "../../src/app/appstate.h"
#include "../../src/app/mainwindow.h"  // so the stand-in checks link the windows the main window makes, this dialog among them
#include "../../src/app/panels.h"
#include "../../src/app/renderdialog.h"
#include "../../src/app/theme.h"
#include "../../src/engine/renderas.h"
#include "../../src/media/media.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QAction>
#include <QLabel>
#include <QProcess>
#include <QListWidget>
#include <QMenu>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

template <class T>
T* kid(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

// Calls step every 40 ms with the modal dialog that is open; step returns true when it is done with it.
void drive(std::function<bool(QWidget*)> step, const std::string& what, int limitMs = 120000)
{
    auto* t = new QTimer(qApp);
    auto* clock = new QElapsedTimer();
    clock->start();
    QObject::connect(t, &QTimer::timeout, [=] {
        QWidget* m = QApplication::activeModalWidget();
        if (!m) return;
        if (step(m)) {
            t->stop();
            t->deleteLater();
            delete clock;
        } else if (clock->elapsed() > limitMs) {
            check(false, what + ": dialog did not finish in time");
            t->stop();
            t->deleteLater();
            delete clock;
            m->close();
        }
    });
    t->start(40);
}

// Clicks a button once the timer that drives the dialog has returned (a button that opens a modal dialog or a question would
// otherwise block the timer inside its own call).
void later(QAbstractButton* b)
{
    QTimer::singleShot(0, b, [b] { b->click(); });
}

bool finishedPage(QWidget* m) { auto* c = kid<QPushButton>(m, "closeRender"); return c && !c->isHidden(); }

// The template row of "Format/Template" (by the template's name as shown, without "(custom)"): the format is picked in the Formats
// list first (VEGAS 18's two panes), then the Templates list holds that format's templates.
QTreeWidgetItem* findRow(QTreeWidget* t, const QString& format, const QString& name)
{
    QListWidget* fl = t->window()->findChild<QListWidget*>("formats");
    if (!fl) return nullptr;
    for (int k = 0; k < fl->count(); ++k)
        if (fl->item(k)->data(Qt::DisplayRole).toString() == format) {
            fl->setCurrentRow(k);
            break;
        }
    for (int c = 0; c < t->topLevelItemCount(); ++c) {
        QString n = t->topLevelItem(c)->text(0);
        if (n.endsWith("  (custom)")) n.chop(10);
        if (n == name) return t->topLevelItem(c);
    }
    return nullptr;
}

int templateRows(QTreeWidget* t) { return t->topLevelItemCount(); }

struct Outcome {
    QString title, result, info;
    bool done = false;
};

// Renders with the dialog: picks the template, sets the name, ticks the options asked for, renders, and closes.
// 'tweak' can change anything else on the first page before Render is pressed.
Outcome render(const QString& format, const QString& tmpl, const QString& name, bool regionOnly, std::function<void(QWidget*)> tweak = nullptr,
               const QString& shotName = QString())
{
    Outcome o;
    int stage = 0;
    drive([&](QWidget* m) {
        if (stage == 0) {
            auto* tree = kid<QTreeWidget>(m, "templates");
            QTreeWidgetItem* row = tree ? findRow(tree, format, tmpl) : nullptr;
            if (!row) {
                check(false, ("render: the template is listed: " + format + "/" + tmpl).toStdString());
                static_cast<QDialog*>(m)->reject();
                return true;
            }
            tree->setCurrentItem(row);
            kid<QComboBox>(m, "outputFolder")->setEditText(gOut);
            kid<QLineEdit>(m, "outputName")->setText(name);
            auto* region = kid<QAction>(m, "regionOnly");
            if (region->isEnabled()) region->setChecked(regionOnly);
            if (tweak) tweak(m);
            o.info = kid<QLabel>(m, "templateInfo")->text();
            if (!shotName.isEmpty()) uitest::shot(m, gOut + "/" + shotName);
            later(kid<QPushButton>(m, "renderButton"));
            stage = 1;
            return false;
        }
        if (auto* box = qobject_cast<QMessageBox*>(m)) {
            // "already exists. Replace it?"
            box->button(QMessageBox::Yes) ? box->button(QMessageBox::Yes)->click() : box->accept();
            return false;
        }
        if (finishedPage(m)) {
            o.title = m->windowTitle();
            o.result = kid<QLabel>(m, "resultLabel")->text();
            o.done = true;
            kid<QPushButton>(m, "closeRender")->click();
            return true;
        }
        return false;
    }, ("render " + name).toStdString());
    runRenderDialog(nullptr);
    return o;
}

std::unique_ptr<MediaReader> reopen(const QString& file)
{
    std::string err;
    std::unique_ptr<MediaReader> r = openMedia(file.toStdString(), &err);
    if (!r) std::printf("  (%s: %s)\n", file.toUtf8().constData(), err.c_str());
    return r;
}

double meanGreen(MediaReader& r, double t, int y0, int y1)
{
    ImagePtr f = r.videoFrame(t, r.info().width, r.info().height);
    if (!f) return -1;
    long long sum = 0, n = 0;
    for (int y = std::max(0, y0); y < std::min(f->h, y1); ++y)
        for (int x = 0; x < f->w; x += 2) {
            sum += f->row(f->h - 1 - y)[x * 4 + 1];
            ++n;
        }
    return n ? (double)sum / n : -1;
}

void testList()
{
    const QStringList names = renderTemplateNames();
    for (const char* n : {"MAGIX AVC/AAC MP4/Internet HD 1080p 29.97 fps", "MAGIX AVC/AAC MP4/Internet UHD 2160p 59.94 fps", "MAGIX HEVC/AAC MP4/Internet HD 1080p 25 fps",
                          "Windows Media Video V11/8 Mbps HD 1080-30p Video", "Video for Windows/NTSC DV", "QuickTime 7/Apple ProRes 422 HQ",
                          "Sony MXF/HD422 1920x1080-60i, 50 Mbps", "Sony XAVC S/XAVC S HD 1920x1080-30p, 50 Mbps", "Wave (Microsoft)/48,000 Hz, 24 Bit, Stereo, PCM",
                          "MP3 Audio/192 Kbps, CD Transparent Audio", "FLAC Audio/Default Template", "OggVorbis/128 Kbps, CD Quality", "MAGIX AAC Audio/192 Kbps Stereo",
                          "Image Sequence/PNG (with alpha)"})
        check(names.contains(n), std::string("render: VEGAS's template is there: ") + n);
    check(names.size() > 80, "render: the formats have their templates", std::to_string(names.size()));

    int stage = 0;
    drive([&](QWidget* m) {
        auto* tree = kid<QTreeWidget>(m, "templates");
        if (stage == 0) {
            auto* cur = tree->currentItem();
            check(cur && cur->text(0) == "Internet HD 1080p 29.97 fps" && kid<QListWidget>(m, "formats")->currentItem()->text() == "MAGIX AVC/AAC MP4",
                  "render: VEGAS's Internet HD 1080p is chosen at first", cur ? cur->text(0).toStdString() : "none");
            const QString info = kid<QLabel>(m, "templateInfo")->text();
            check(info.contains("1920x1080") && info.contains("29.970 fps") && info.contains("192 Kbps") && info.contains("48,000 Hz"),
                  "render: the template's summary says what it makes", info.toStdString());
            check(kid<QLabel>(m, "estimate")->text().startsWith("Estimated file size: "), "render: the estimated file size is shown", kid<QLabel>(m, "estimate")->text().toStdString());
            check(kid<QLineEdit>(m, "outputName")->text() == "Untitled.mp4", "render: an unsaved project's file is Untitled with the extension", kid<QLineEdit>(m, "outputName")->text().toStdString());
            uitest::shot(m, gOut + "/render_dialog.png");
            // VEGAS 18's wording: the search box and filter, the two panes, the Render Options menu, the folder row
            check(kid<QToolButton>(m, "templateFilter")->text() == "Filters Off", "render: the filter button says Filters Off", kid<QToolButton>(m, "templateFilter")->text().toStdString());
            check(kid<QLabel>(m, "formatsLabel")->text() == "Formats" && kid<QLabel>(m, "templatesLabel")->text() == "Templates", "render: Formats and Templates panes");
            check(kid<QPushButton>(m, "projectFolder") && kid<QPushButton>(m, "projectFolder")->text() == "Project Location", "render: a Project Location button");
            check(kid<QToolButton>(m, "renderOptions") && kid<QToolButton>(m, "renderOptions")->text() == "Render Options", "render: the Render Options button");
            if (auto* ro = kid<QToolButton>(m, "renderOptions")) {
                QStringList items;
                for (QAction* a : ro->menu()->actions())
                    if (!a->isSeparator() && a->objectName() != "swapFiles" && a->objectName() != "saveSummary") items << a->text();
                const QStringList want = {"Render loop region only", "Stretch video to fill output frame size (do not letterbox)", "Use project output rotation setting",
                                          "Save project markers in media file", "Save project as path reference in rendered file", "Save loudness log file next to media file",
                                          "Enable multichannel mapping", "Channels..."};
                check(items == want, "render: Render Options lists VEGAS 18's check boxes in order", items.join(" | ").toStdString());
            }
            {
                QStringList fmts;
                auto* fl = kid<QListWidget>(m, "formats");
                for (int k = 0; k < fl->count(); ++k) fmts << fl->item(k)->text();
                for (const char* f : {"Panasonic P2 MXF", "Sony Perfect Clarity Audio", "FFmpeg (Voukoder-style)"})
                    check(fmts.contains(f), std::string("render: the formats list has ") + f);
                check(fmts.indexOf("Panasonic P2 MXF") >= 0 && fmts.indexOf("Panasonic P2 MXF") < fmts.indexOf("Sony MXF"), "render: the formats are in alphabetical order");
                for (int k = 0; k < fl->count(); ++k)
                    if (fl->item(k)->text() == "Sony Perfect Clarity Audio")
                        check(!(fl->item(k)->flags() & Qt::ItemIsEnabled) && !fl->item(k)->toolTip().isEmpty(), "render: Perfect Clarity Audio is greyed with a reason");
            }
            // search
            const int all = templateRows(tree);
            kid<QLineEdit>(m, "templateSearch")->setText("uhd 2160p 25");
            uitest::settle(30);
            const int found = templateRows(tree);
            bool onlyUhd = found > 0;
            for (int k = 0; k < tree->topLevelItemCount(); ++k) onlyUhd = onlyUhd && tree->topLevelItem(k)->text(0).contains("UHD 2160p 25");
            check(onlyUhd && found < all, "render: the search box narrows the templates", std::to_string(found) + " of " + std::to_string(all));
            uitest::shot(m, gOut + "/render_search.png");
            kid<QLineEdit>(m, "templateSearch")->clear();
            uitest::settle(30);
            check(templateRows(tree) == all, "render: clearing the search shows them all again");
            // favourite: a click on the star
            QTreeWidgetItem* mp3 = findRow(tree, "MP3 Audio", "320 Kbps, CD Transparent Audio");
            check(mp3 != nullptr, "render: MP3's templates are listed");
            if (mp3) {
                tree->scrollToItem(mp3);
                uitest::settle(30);
                uitest::mouseClick(tree->viewport(), tree->visualRect(tree->indexFromItem(mp3, 1)).center());
                uitest::settle(30);
                check(QSettings().value("render/favorites").toStringList().contains("mp3/320 Kbps, CD Transparent Audio"), "render: the star makes a template a favourite");
                QToolButton* filter = kid<QToolButton>(m, "templateFilter");
                QAction* favs = nullptr;
                for (QAction* a : filter->menu()->actions())
                    if (a->objectName() == "filterFavorites") favs = a;
                favs->setChecked(true);
                uitest::settle(30);
                check(templateRows(tree) == 1 && tree->topLevelItem(0)->text(1) == QString::fromUtf8("\u2605"), "render: the Favorites filter shows only favourites",
                      std::to_string(templateRows(tree)));
                uitest::shot(m, gOut + "/render_favorites.png");
                favs->setChecked(false);
                uitest::settle(30);
            }
            // match project settings: a 1920x1080 30 fps project has no 29.97 templates
            QAction* match = nullptr;
            for (QAction* a : kid<QToolButton>(m, "templateFilter")->menu()->actions())
                if (a->objectName() == "filterMatchProject") match = a;
            match->setChecked(true);
            uitest::settle(30);
            bool allMatch = templateRows(tree) > 0;
            for (int k = 0; k < tree->topLevelItemCount(); ++k) allMatch = allMatch && !tree->topLevelItem(k)->text(0).contains("29.97");
            check(allMatch, "render: Match project settings leaves out other rates", std::to_string(templateRows(tree)));
            match->setChecked(false);
            uitest::settle(30);
            // the loop region option follows the loop region
            check(!kid<QAction>(m, "regionOnly")->isEnabled(), "render: Render loop region only is greyed without a loop region");
            check(!kid<QCheckBox>(m, "regionOnlyBox")->isEnabled() && !kid<QCheckBox>(m, "regionOnlyBox")->isHidden(),
                  "render: the dialog's own Render loop region only check box is there, greyed too");
            stage = 1;
            static_cast<QDialog*>(m)->reject();
            return true;
        }
        return true;
    }, "render list");
    runRenderDialog(nullptr);
}

void testFormats()
{
    AppState& app = AppState::get();
    app.setLoop(1.0, 1.5);
    // markers: one in the region, one out of it
    app.edit("Markers", [](Project& p) {
        Project::Marker a;
        a.id = p.newId();
        a.time = 1.2;
        a.name = "Inside";
        p.markers.push_back(a);
        Project::Marker b;
        b.id = p.newId();
        b.time = 3.0;
        b.name = "Outside";
        p.markers.push_back(b);
    });

    // AVC at another size and rate, with chapters and a loudness log
    Outcome o = render("MAGIX AVC/AAC MP4", "Internet HD 720p 25 fps", "avc720", true, [](QWidget* m) {
        kid<QAction>(m, "saveMarkers")->setChecked(true);
        kid<QAction>(m, "loudnessLog")->setChecked(true);
    }, "render_avc_setup.png");
    check(o.title == "Render Complete", "render: AVC 720p 25 fps renders", (o.title + " " + o.result).toStdString());
    if (auto r = reopen(gOut + "/avc720.mp4")) {
        const MediaInfo& i = r->info();
        check(i.width == 1280 && i.height == 720 && std::fabs(i.fps - 25.0) < 0.01, "render: the template's size and rate", strfmt("%dx%d %.3f", i.width, i.height, i.fps));
        check(std::fabs(i.duration - 0.5) < 0.1, "render: only the loop region", strfmt("%.3f", i.duration));
        check(i.videoCodec.find("264") != std::string::npos && i.audioCodec.find("aac") != std::string::npos && i.sampleRate == 48000, "render: H.264 and AAC at 48 kHz",
              i.videoCodec + " " + i.audioCodec);
        bool inside = false, outside = false;
        for (const auto& mk : i.markers) {
            inside = inside || (mk.second == "Inside" && std::fabs(mk.first - 0.2) < 0.01);
            outside = outside || mk.second == "Outside";
        }
        check(inside && !outside,
              "render: the project's marker in the region is a chapter, at its place in the file",
              i.markers.empty() ? std::string("none") : strfmt("%d chapters, first %.3f '%s'", (int)i.markers.size(), i.markers[0].first, i.markers[0].second.c_str()));
        check(meanGreen(*r, 0.25, 0, 720) > 10, "render: the picture is not black");
    }
    QFile log(gOut + "/avc720_loud.txt");
    check(log.open(QIODevice::ReadOnly) && QString::fromUtf8(log.readAll()).contains("Integrated:"), "render: the loudness log is written beside the file");

    // HEVC
    o = render("MAGIX HEVC/AAC MP4", "Internet HD 1080p 29.97 fps", "hevc1080", true);
    if (auto r = reopen(gOut + "/hevc1080.mp4")) check(r->info().videoCodec.find("hevc") != std::string::npos && r->info().width == 1920, "render: HEVC", r->info().videoCodec);
    else check(false, "render: HEVC renders", o.result.toStdString());

    // Windows Media
    o = render("Windows Media Video V11", "3 Mbps Video", "wmv", true);
    if (auto r = reopen(gOut + "/wmv.wmv")) check(r->info().width == 640 && r->info().height == 480 && r->info().hasAudio, "render: Windows Media Video", r->info().videoCodec + " " + r->info().audioCodec);
    else check(false, "render: Windows Media renders", o.result.toStdString());

    // Video for Windows: DV (letterboxed into 720x480) and uncompressed
    o = render("Video for Windows", "NTSC DV", "dv", true);
    if (auto r = reopen(gOut + "/dv.avi")) {
        check(r->info().videoCodec.find("dv") != std::string::npos && r->info().width == 720, "render: NTSC DV in an AVI",
              r->info().videoCodec + strfmt(" %dx%d", r->info().width, r->info().height));
    } else check(false, "render: NTSC DV renders", o.result.toStdString());
    o = render("Video for Windows", "Default Template (uncompressed)", "uncompressed", true);
    if (auto r = reopen(gOut + "/uncompressed.avi")) check(r->info().width == 1920 && r->info().height == 1080 && r->info().videoCodec.find("raw") != std::string::npos,
                                                            "render: uncompressed AVI at the project's size", r->info().videoCodec);
    else check(false, "render: uncompressed AVI renders", o.result.toStdString());
    o = render("Video for Windows", "HD 720-30p YUV", "utvideo", true);
    if (auto r = reopen(gOut + "/utvideo.avi")) check(r->info().width == 1280 && r->info().videoCodec.find("ut") != std::string::npos, "render: lossless YUV (UT Video)", r->info().videoCodec);
    else check(false, "render: UT Video renders", o.result.toStdString());

    // QuickTime: ProRes, ProRes 4444 with alpha, PNG
    for (const char* t : {"Apple ProRes 422 HQ", "Apple ProRes 4444 (with alpha)", "PNG (with alpha)", "H.264 1080p (MOV)"}) {
        const QString file = QString("mov_%1").arg(QString(t).left(16).replace(' ', '_').replace('(', '_'));
        o = render("QuickTime 7", t, file, true);
        auto r = reopen(gOut + "/" + file + ".mov");
        check(r && r->info().hasVideo && r->info().width == 1920, std::string("render: QuickTime ") + t, r ? r->info().videoCodec : o.result.toStdString());
    }

    // MXF and XAVC S
    o = render("Sony MXF", "HD422 1920x1080-30p, 50 Mbps", "mxf", true);
    if (auto r = reopen(gOut + "/mxf.mxf")) check(r->info().videoCodec.find("mpeg2") != std::string::npos && r->info().hasAudio, "render: MXF HD422", r->info().videoCodec);
    else check(false, "render: MXF renders", o.result.toStdString());
    o = render("Sony XAVC S", "XAVC S HD 1920x1080-30p, 50 Mbps", "xavcs", true);
    if (auto r = reopen(gOut + "/xavcs.mp4")) check(r->info().videoCodec.find("264") != std::string::npos && r->info().hasAudio, "render: XAVC S (H.264 and PCM in MP4)", r->info().audioCodec);
    else check(false, "render: XAVC S renders", o.result.toStdString());

    // sound only
    struct A { const char* format; const char* tmpl; const char* file; int rate; };
    const A sounds[] = {{"Wave (Microsoft)", "48,000 Hz, 24 Bit, Stereo, PCM", "sound24.wav", 48000},
                        {"Wave (Microsoft)", "48,000 Hz, 32 Bit (float), Stereo, IEEE", "soundfloat.wav", 48000},
                        {"Sony Wave64", "44,100 Hz, 16 Bit, Mono, PCM", "sound.w64", 44100},
                        {"MP3 Audio", "128 Kbps, CD Quality Audio", "sound.mp3", 44100},
                        {"FLAC Audio", "48,000 Hz, 24 Bit, Stereo, FLAC", "sound.flac", 48000},
                        {"OggVorbis", "128 Kbps, CD Quality", "sound.ogg", 44100},
                        {"MAGIX AAC Audio", "192 Kbps Stereo", "sound.m4a", 48000},
                        {"Windows Media Audio V11", "128 Kbps, 44 kHz, stereo CBR", "sound.wma", 44100}};
    for (const A& a : sounds) {
        o = render(a.format, a.tmpl, QFileInfo(a.file).completeBaseName(), true);
        auto r = reopen(gOut + "/" + a.file);
        check(r && r->info().hasAudio && !r->info().hasVideo && r->info().sampleRate == a.rate, std::string("render: ") + a.format + " " + a.tmpl,
              r ? strfmt("%d Hz %s", r->info().sampleRate, r->info().audioCodec.c_str()) : o.result.toStdString());
        if (r && std::string(a.file) == "sound.w64") check(r->info().channels == 1, "render: a mono template makes mono sound");
    }

    // image sequence: one PNG per frame
    for (int k = 0; k < 40; ++k) QFile::remove(gOut + QString("/frames_%1.png").arg(k, 6, 10, QChar('0')));
    o = render("Image Sequence", "PNG", "frames", true);
    int pictures = 0;
    while (QFileInfo::exists(gOut + QString("/frames_%1.png").arg(pictures, 6, 10, QChar('0')))) ++pictures;
    check(pictures == 15, "render: an image sequence writes one picture per frame (0.5 s at 30 fps)", std::to_string(pictures));
    QImage firstPic(gOut + "/frames_000000.png");
    check(firstPic.width() == 1920 && firstPic.height() == 1080, "render: the pictures are the project's size");
    check(o.result.contains("frames_000000.png"), "render: the result names the first picture", o.result.toStdString());
}

void testLetterboxAndRotation()
{
    AppState& app = AppState::get();
    app.setLoop(1.0, 1.2);
    // a square custom size (made with Customize Template): letterboxed, the top is black; stretched, it is not
    int stage = 0;
    QString saved;
    drive([&](QWidget* m) {
        if (stage == 0) {
            auto* tree = kid<QTreeWidget>(m, "templates");
            tree->setCurrentItem(findRow(tree, "MAGIX AVC/AAC MP4", "Internet HD 720p 29.97 fps"));
            stage = 1;
            later(kid<QPushButton>(m, "customizeTemplate"));
            return false;
        }
        if (stage == 1 && m->objectName() == "customizeTemplate") {
            check(kid<QTabWidget>(m, "customTabs")->count() == 3, "customize: Video, Audio and Project tabs");
            check(kid<QComboBox>(m, "encodeMode") != nullptr && kid<QComboBox>(m, "encodeMode")->count() == 6, "customize: the encode modes are listed");
            check(kid<QLabel>(m, "gpuStatus") != nullptr, "customize: says which graphics card encoders work");
            auto* size = kid<QComboBox>(m, "frameSize");
            size->setCurrentIndex(size->count() - 1);       // custom
            kid<QSpinBox>(m, "width")->setValue(720);
            kid<QSpinBox>(m, "height")->setValue(720);
            kid<QComboBox>(m, "frameRate")->setCurrentIndex(6);     // 50
            kid<QRadioButton>(m, "cbr")->setChecked(true);
            kid<QSpinBox>(m, "cbrKbps")->setValue(3000);
            kid<QComboBox>(m, "templateName")->setEditText("Square 720 50p");
            kid<QLineEdit>(m, "templateDescription")->setText("A test");
            uitest::shot(m, gOut + "/render_customize.png");
            kid<QToolButton>(m, "saveTemplate")->click();
            kid<QPushButton>(m, "ok")->click();
            stage = 2;
            return false;
        }
        if (stage == 2 && m->objectName() == "renderDialog") {
            auto* tree = kid<QTreeWidget>(m, "templates");
            QTreeWidgetItem* cur = tree->currentItem();
            saved = cur ? cur->text(0) : QString();
            uitest::shot(m, gOut + "/render_custom_selected.png");
            static_cast<QDialog*>(m)->reject();
            return true;
        }
        return false;
    }, "customize");
    runRenderDialog(nullptr);
    check(saved == "Square 720 50p  (custom)", "customize: the saved template is chosen and marked custom", saved.toStdString());
    QFile tf(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/render_templates.json");
    check(tf.open(QIODevice::ReadOnly) && QString::fromUtf8(tf.readAll()).contains("Square 720 50p"), "customize: custom templates are kept in render_templates.json");

    Outcome o = render("MAGIX AVC/AAC MP4", "Square 720 50p", "square_letterbox", true, [](QWidget* m) { kid<QAction>(m, "stretch")->setChecked(false); });
    auto r = reopen(gOut + "/square_letterbox.mp4");
    check(r && r->info().width == 720 && r->info().height == 720 && std::fabs(r->info().fps - 50) < 0.01, "customize: the custom size and rate are rendered",
          r ? strfmt("%dx%d %.2f", r->info().width, r->info().height, r->info().fps) : o.result.toStdString());
    double barLetter = -1, middle = -1;
    if (r) {
        barLetter = meanGreen(*r, 0.1, 0, 100);
        middle = meanGreen(*r, 0.1, 300, 420);
    }
    check(barLetter >= 0 && barLetter < 20 && middle > 20, "render: letterboxed: black above the picture", strfmt("%.1f / %.1f", barLetter, middle));
    o = render("MAGIX AVC/AAC MP4", "Square 720 50p", "square_stretch", true, [](QWidget* m) { kid<QAction>(m, "stretch")->setChecked(true); });
    r = reopen(gOut + "/square_stretch.mp4");
    const double barStretch = r ? meanGreen(*r, 0.1, 0, 100) : -1;
    check(barStretch > 20, "render: Stretch video to fill: no black bars", strfmt("%.1f", barStretch));

    // output rotation: 90 degrees turns the 1920x1080 project into a 1080x1920 file
    app.edit("Rotation", [](Project& p) { p.settings.outputRotation = 90; });
    o = render("Matroska (MKV)", "Project size and rate", "rotated", true, [](QWidget* m) { kid<QAction>(m, "useRotation")->setChecked(true); });
    r = reopen(gOut + "/rotated.mkv");
    check(r && r->info().width == 1080 && r->info().height == 1920, "render: Use project output rotation turns the picture",
          r ? strfmt("%dx%d", r->info().width, r->info().height) : o.result.toStdString());
    o = render("Matroska (MKV)", "Project size and rate", "unrotated", true, [](QWidget* m) { kid<QAction>(m, "useRotation")->setChecked(false); });
    r = reopen(gOut + "/unrotated.mkv");
    check(r && r->info().width == 1920 && r->info().height == 1080, "render: without it the picture is not turned");
    app.undo();

    // two passes (x264)
    stage = 0;
    drive([&](QWidget* m) {
        if (stage == 0) {
            auto* tree = kid<QTreeWidget>(m, "templates");
            tree->setCurrentItem(findRow(tree, "MAGIX AVC/AAC MP4", "Internet 360p 29.97 fps widescreen"));
            stage = 1;
            later(kid<QPushButton>(m, "customizeTemplate"));
            return false;
        }
        if (stage == 1 && m->objectName() == "customizeTemplate") {
            kid<QRadioButton>(m, "vbr")->setChecked(true);
            kid<QCheckBox>(m, "twoPass")->setChecked(true);
            kid<QComboBox>(m, "templateName")->setEditText("Two pass 360p");
            kid<QToolButton>(m, "saveTemplate")->click();
            kid<QPushButton>(m, "ok")->click();
            stage = 2;
            return false;
        }
        if (stage == 2 && m->objectName() == "renderDialog") {
            static_cast<QDialog*>(m)->reject();
            return true;
        }
        return false;
    }, "two pass template");
    runRenderDialog(nullptr);
    o = render("MAGIX AVC/AAC MP4", "Two pass 360p", "twopass", true);
    r = reopen(gOut + "/twopass.mp4");
    check(r && r->info().width == 640 && o.result.contains("two passes"), "render: two passes", o.result.toStdString());
    QStringList leftovers = QDir(gOut).entryList(QStringList() << "twopass.mp4.*");
    check(leftovers.isEmpty(), "render: the first pass's files are cleaned up", leftovers.join(", ").toStdString());

    // a custom template survives into the next session (read again from the file)
    bool found = false;
    stage = 0;
    drive([&](QWidget* m) {
        auto* tree = kid<QTreeWidget>(m, "templates");
        found = findRow(tree, "MAGIX AVC/AAC MP4", "Square 720 50p") != nullptr;
        static_cast<QDialog*>(m)->reject();
        return true;
    }, "custom template again");
    runRenderDialog(nullptr);
    check(found, "customize: the custom template is listed again next time");
}

void testFallbackAndWriter()
{
    // an encode mode whose encoder cannot start (no NVIDIA card here) falls back to the others
    std::vector<EncoderMode> modes = videoEncoderModes("h264", true);
    bool cpuWorks = false;
    for (const EncoderMode& m : modes) cpuWorks = cpuWorks || (m.mode == "cpu" && m.works);
    check(cpuWorks && modes.size() == 5, "writer: H.264's encode modes are known, and the CPU one starts", std::to_string(modes.size()));
    for (const EncoderMode& m : modes) std::printf("  h264 %s: %s present %d works %d\n", m.mode.c_str(), m.encoder.c_str(), (int)m.present, (int)m.works);
    EncodeSettings es;
    es.path = (gOut + "/fallback.mp4").toStdString();
    es.width = 320;
    es.height = 180;
    es.videoFamily = "h264";
    es.encodeMode = "nvenc";
    es.audio = false;
    std::string err;
    auto w = createMediaWriter(es, &err);
    bool nvencWorks = false;
    for (const EncoderMode& m : modes) nvencWorks = nvencWorks || (m.mode == "nvenc" && m.works);
    check(w && (nvencWorks || w->describe().find("h264_nvenc") == std::string::npos), "writer: NVENC asked for, without the card: another encoder makes it",
          w ? w->describe() : err);
    if (w) {
        ImagePtr img = Image::make(320, 180);
        for (int k = 0; k < 10; ++k) w->writeVideo(*img);
        check(w->finish(&err), "writer: the fallback file is finished", err);
    }
    // letterboxing in the writer: a 16:9 picture into 4:3
    es = EncodeSettings();
    es.path = (gOut + "/letterbox.mkv").toStdString();
    es.width = 320;
    es.height = 240;
    es.inWidth = 320;
    es.inHeight = 180;
    es.audio = false;
    es.videoFamily = "utvideo";
    w = createMediaWriter(es, &err);
    check(w != nullptr, "writer: pictures of another size are taken", err);
    if (w) {
        ImagePtr img = Image::make(320, 180);
        for (int y = 0; y < 180; ++y)
            for (int x = 0; x < 320; ++x) {
                uint8_t* p = img->row(y) + x * 4;
                p[0] = p[1] = p[2] = 200;
                p[3] = 255;
            }
        check(w->writeVideo(*img), "writer: a smaller picture is written");
        check(!w->writeVideo(*Image::make(100, 100)), "writer: a picture of a third size is refused");
        w->finish(nullptr);
        auto r = reopen(gOut + "/letterbox.mkv");
        if (r) {
            const double top = meanGreen(*r, 0, 0, 20), mid = meanGreen(*r, 0, 100, 140);
            check(top < 10 && mid > 150, "writer: letterboxed into the frame with black above and below", strfmt("%.1f / %.1f", top, mid));
        }
    }
    // a pixel aspect ratio and field order are written into the file
    es = EncodeSettings();
    es.path = (gOut + "/interlaced.mp4").toStdString();
    es.width = 720;
    es.height = 480;
    es.fpsNum = 30000;
    es.fpsDen = 1001;
    es.audio = false;
    es.fieldOrder = "upper";
    es.pixelAspect = 1.2121;
    es.videoFamily = "h264";
    w = createMediaWriter(es, &err);
    if (w) {
        ImagePtr img = Image::make(720, 480);
        for (int k = 0; k < 6; ++k) w->writeVideo(*img);
        w->finish(nullptr);
    }
    auto r = reopen(gOut + "/interlaced.mp4");
    check(r && r->info().fieldOrder == "upper", "writer: the field order is in the file", r ? r->info().fieldOrder : err);
    check(r && std::abs(r->info().width - 873) <= 2, "writer: the pixel aspect ratio is in the file (720 wide pixels show 873 wide)", r ? std::to_string(r->info().width) : err);
    // nonsense is refused, not crashed on
    es = EncodeSettings();
    es.path = (gOut + "/nonsense.mp4").toStdString();
    es.videoFamily = "no-such-family";
    es.profile = "no-such-profile";
    es.level = "99";
    es.quality = 900;
    es.maxKbps = -5;
    es.inWidth = -3;
    w = createMediaWriter(es, &err);
    check(true, "writer: nonsense settings do not crash (" + std::string(w ? "opened" : err) + ")");
    if (w) w->finish(nullptr);
}

void testCancel()
{
    AppState& app = AppState::get();
    app.setLoop(0, 0);
    const QString big = gOut + "/render_cancel.mp4";
    QFile::remove(big);
    int stage = 0;
    QString title;
    drive([&](QWidget* m) {
        if (stage == 0) {
            auto* tree = kid<QTreeWidget>(m, "templates");
            tree->setCurrentItem(findRow(tree, "MAGIX AVC/AAC MP4", "Internet HD 1080p 29.97 fps"));
            kid<QComboBox>(m, "outputFolder")->setEditText(gOut);
            kid<QLineEdit>(m, "outputName")->setText("render_cancel");
            later(kid<QPushButton>(m, "renderButton"));
            stage = 1;
            return false;
        }
        if (stage == 1 && kid<QProgressBar>(m, "progressBar")->value() > 20) {
            check(kid<QLabel>(m, "elapsedTime")->text().startsWith("Elapsed: "), "render: the time elapsed is shown", kid<QLabel>(m, "elapsedTime")->text().toStdString());
            check(kid<QLabel>(m, "percentOf")->text().contains("% of "), "render: and how much is done", kid<QLabel>(m, "percentOf")->text().toStdString());
            check(kid<QLabel>(m, "remainingTime")->text().startsWith("Remaining: ") && kid<QLabel>(m, "renderedSize")->text().startsWith("Rendered: "),
                  "render: Remaining: and Rendered: are shown", kid<QLabel>(m, "remainingTime")->text().toStdString());
            check(kid<QLabel>(m, "freeDiskSpace")->text().endsWith(" GB") || kid<QLabel>(m, "freeDiskSpace")->text().endsWith(" MB") || kid<QLabel>(m, "freeDiskSpace")->text().endsWith(" TB"),
                  "render: Estimated Properties - Free Disk Space", kid<QLabel>(m, "freeDiskSpace")->text().toStdString());
            for (const char* n : {"renderingDuration", "timeOfCompletion", "averageSpeed", "currentSpeed", "cpuUsage", "memoryUsage"})
                check(kid<QLabel>(m, n) != nullptr, std::string("render: the progress dialog has ") + n);
            check(kid<QCheckBox>(m, "openFolderWhenDone") && kid<QCheckBox>(m, "openFolderWhenDone")->text() == "Open folder when rendering is complete" &&
                      kid<QCheckBox>(m, "closeWhenDone") && kid<QCheckBox>(m, "closeWhenDone")->text() == "Close dialog when rendering is complete",
                  "render: Options - Open folder / Close dialog when rendering is complete");
            check(kid<QPushButton>(m, "openFolder") && !kid<QPushButton>(m, "openFolder")->isEnabled() && kid<QPushButton>(m, "openFile") && !kid<QPushButton>(m, "openFile")->isEnabled(),
                  "render: Open Folder and Open are greyed while rendering");
            check(m->windowTitle().startsWith("Rendering: "), "render: the title says Rendering: N%", m->windowTitle().toStdString());
            uitest::shot(m, gOut + "/render_progress.png");
            kid<QPushButton>(m, "cancelRender")->click();
            stage = 2;
            return false;
        }
        if (stage == 2 && finishedPage(m)) {
            title = m->windowTitle();
            kid<QPushButton>(m, "closeRender")->click();
            return true;
        }
        return false;
    }, "render cancel");
    runRenderDialog(nullptr);
    check(title == "Render cancelled", "render: Cancel stops the render", title.toStdString());
    check(!QFileInfo(big).exists(), "render: the unfinished file is removed");

    // closing the dialog while it renders cancels and waits
    stage = 0;
    drive([&](QWidget* m) {
        if (stage == 0) {
            kid<QComboBox>(m, "outputFolder")->setEditText(gOut);
            kid<QLineEdit>(m, "outputName")->setText("render_cancel");
            later(kid<QPushButton>(m, "renderButton"));
            stage = 1;
            return false;
        }
        if (auto* box = qobject_cast<QMessageBox*>(m)) {
            box->accept();
            return false;
        }
        if (kid<QProgressBar>(m, "progressBar") && kid<QProgressBar>(m, "progressBar")->value() > 10) {
            static_cast<QDialog*>(m)->reject();
            return true;
        }
        return false;
    }, "render close");
    runRenderDialog(nullptr);
    check(!QFileInfo(big).exists(), "render: closing the dialog stops the render and removes the file");

    // the finished page: Open and Open Folder, and the result
    app.setLoop(1.0, 1.3);
    stage = 0;
    bool buttons = false;
    drive([&](QWidget* m) {
        if (stage == 0) {
            auto* tree = kid<QTreeWidget>(m, "templates");
            tree->setCurrentItem(findRow(tree, "MAGIX AVC/AAC MP4", "Internet 360p 29.97 fps widescreen"));
            kid<QComboBox>(m, "outputFolder")->setEditText(gOut);
            kid<QLineEdit>(m, "outputName")->setText("finished");
            // the check box on the dialog (as VEGAS's): ticking it ticks the Render Options one
            auto* box = kid<QCheckBox>(m, "regionOnlyBox");
            check(box->isEnabled() && !box->isHidden() && box->text().contains("to"), "render: with a loop region the dialog's check box can be ticked and shows the region",
                  box->text().toStdString());
            box->setChecked(true);
            check(kid<QAction>(m, "regionOnly")->isChecked(), "render: ticking it ticks Render Options > Render loop region only");
            kid<QAction>(m, "regionOnly")->setChecked(false);
            check(!box->isChecked(), "render: and the other way round");
            box->setChecked(true);
            uitest::shot(m, gOut + "/render_regionbox.png");
            later(kid<QPushButton>(m, "renderButton"));
            stage = 1;
            return false;
        }
        if (auto* box = qobject_cast<QMessageBox*>(m)) {
            box->accept();
            return false;
        }
        if (finishedPage(m)) {
            buttons = !kid<QPushButton>(m, "openFile")->isHidden() && !kid<QPushButton>(m, "openFolder")->isHidden();
            uitest::shot(m, gOut + "/render_done.png");
            kid<QPushButton>(m, "closeRender")->click();
            return true;
        }
        return false;
    }, "render finished page");
    runRenderDialog(nullptr);
    check(buttons, "render: Open and Open Folder are offered when it is done");
    {
        // only the loop region (1.0 - 1.3 s) was rendered
        std::unique_ptr<MediaReader> r = openMedia((gOut + "/finished.mp4").toStdString(), nullptr);
        const double d = r ? r->info().duration : -1;
        check(r && d > 0.2 && d < 0.45, "render: with the check box ticked only the loop region is in the file", std::to_string(d));
    }
    check(QSettings().value("render/template").toString() == "avc/Internet 360p 29.97 fps widescreen", "render: the template is remembered",
          QSettings().value("render/template").toString().toStdString());
}

void testNewTrack()
{
    AppState& app = AppState::get();
    QString got;
    drive([&](QWidget* m) {
        if (m->objectName() != "renderDialog") return false;
        check(m->windowTitle() == "Render to New Track", "render to new track: the dialog is titled so");
        check(kid<QLineEdit>(m, "outputName")->text().startsWith("suggested"), "render to new track: the suggested name", kid<QLineEdit>(m, "outputName")->text().toStdString());
        auto* tree = kid<QTreeWidget>(m, "templates");
        tree->setCurrentItem(findRow(tree, "Wave (Microsoft)", "Default Template"));
        kid<QComboBox>(m, "outputFolder")->setEditText(gOut);
        later(kid<QPushButton>(m, "renderButton"));
        return true;
    }, "new track");
    QString path;
    const bool ok = runRenderToNewTrackDialog(nullptr, 1.0, 1.5, gOut + "/suggested.mp4", &path);
    check(ok && path == gOut + "/suggested.wav" && QFileInfo::exists(path), "render to new track: renders with the chosen template and says where", path.toStdString());
    (void)app;
}

}  // namespace

// Master Display: an HDR project rendered through an HDR view transform carries mastering display metadata (and the matching
// colour tags) in the file; a project with HDR Mode Off does not.
void testMasterDisplay()
{
    AppState& app = AppState::get();
    if (QStandardPaths::findExecutable("ffprobe").isEmpty()) { std::printf("INFO master display render check skipped: no ffprobe\n"); return; }
    app.edit("HDR project", [](Project& p) {
        p.settings.pixelFormat = "32bit";
        p.settings.pixelRange = "full";
        p.settings.hdrMode = "hdr10";
        p.settings.viewTransform = "rrt_pq1000";
        p.settings.masterDisplay.preset = "custom";
        p.settings.masterDisplay.maxNits = 4000;
        p.settings.masterDisplay.minNits = 0.01;
        p.settings.masterDisplay.maxCll = 900;
        p.settings.masterDisplay.maxFall = 300;
    });
    app.setLoop(1.0, 1.2);
    Outcome o = render("Matroska (MKV)", "HEVC/AAC 1080p, project frame rate", "hdr_master", true);
    check(o.done, "master display: the HDR render finishes", o.result.toStdString());
    auto probe = [](const QString& file) {
        QProcess pr;
        pr.start("ffprobe", {"-v", "error", "-select_streams", "v:0", "-show_streams", "-show_frames", "-read_intervals", "%+#1", file});
        pr.waitForFinished(60000);
        return QString::fromUtf8(pr.readAllStandardOutput());
    };
    QString text = probe(gOut + "/hdr_master.mkv");
    check(text.contains("max_luminance=4000/1") || text.contains("max_luminance=40000000/10000"), "master display: the file says the display's peak (4000 nits)");
    check(text.contains("max_content=900") && text.contains("max_average=300"), "master display: and MaxCLL / MaxFALL");
    check(text.contains("color_transfer=smpte2084") && text.contains("color_primaries=bt2020"), "master display: the picture is tagged PQ / BT.2020");
    app.edit("HDR off", [](Project& p) { p.settings.hdrMode = "off"; });
    o = render("Matroska (MKV)", "HEVC/AAC 1080p, project frame rate", "hdr_off", true);
    text = probe(gOut + "/hdr_off.mkv");
    check(!text.contains("max_luminance"), "master display: none is written when HDR Mode is Off");
    app.undo();
    app.undo();
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoRenderTest");
    QApplication::setApplicationName("render_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: render_test <output folder> [file.ofx ...]"); return 2; }
    gOut = QDir(args[1]).absolutePath();
    QDir().mkpath(gOut);
    QSettings().clear();
    QSettings().setValue("render/openFolderWhenDone", false);  // the dialog opens the folder when done, by default as VEGAS 18 does
    QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/render_templates.json");
    std::vector<std::string> ofxFiles;
    for (int k = 2; k < args.size(); ++k) ofxFiles.push_back(args[k].toStdString());
    uitest::setup(ofxFiles, std::vector<std::string>());
    uitest::buildDemoProject();

    if (std::getenv("RENDER_TEST_ONLY_HDR")) {  // a quick run of the master display check alone
        testMasterDisplay();
        const int rc = uitest::summary();
        std::fflush(stdout);
        std::_Exit(rc);
    }
    testList();
    testFallbackAndWriter();
    testFormats();
    testLetterboxAndRotation();
    testCancel();
    testNewTrack();
    testMasterDisplay();
    const int rc = uitest::summary();
    std::fflush(stdout);
    std::_Exit(rc);
}
