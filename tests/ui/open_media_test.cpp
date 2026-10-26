// Media files through the main window: File > Open of a media file (and reno.exe given one) puts it into the open project at the
// cursor, as VEGAS does - any kind FFmpeg reads, and animated WebP; a .veg still opens as a project; a file that is neither says so;
// the Explorer lists every kind of media file Reno reads (media.h's list).
//
//   open_media_test <output folder> <folder of sample media (formats_test's samples)> [a .veg]
//   (the .veg defaults to $RENO_SAMPLE_VEG, else the first .veg under $RENO_SAMPLES; without one that part SKIPs)
//
// Build: uibuild open_media_test tests/ui/open_media_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "uitestlib.h"
#include "../testdata.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileSystemModel>
#include <QMessageBox>
#include <QSettings>
#include <QTimer>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoOpenMediaTest");
    QApplication::setApplicationName("open_media_test");
    QSettings().clear();
    const QStringList args = QApplication::arguments();
    const QString out = QDir(args.size() > 1 ? args[1] : QString("open_media")).absolutePath();
    const QString samples = args.size() > 2 ? args[2] : QString();
    QString veg = args.size() > 3 ? args[3] : QString::fromStdString(testSampleVeg());
    if (veg.isEmpty()) {
        const std::vector<std::string> all = testVegFiles(qEnvironmentVariable("RENO_SAMPLES").toStdString());
        if (!all.empty()) veg = QString::fromStdString(all.front());
    }
    QDir(out).removeRecursively();
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    MainWindow w;
    w.resize(1400, 850);
    w.show();
    uitest::settle(200);

    // ---- media files opened with File > Open's path (openProject): each goes in at the cursor, nothing is closed
    const char* const files[] = {"vp9_opus.webm", "theora_vorbis.ogv", "h264_aac.mts", "mpeg2_pcm.mxf", "opus.opus", "alac.caf", "real.ra",
                                 "animated_lossless.webp", "linear_alpha.exr", "still.jp2", "rotated90.mp4", "streamed.webm", "raw.hevc"};
    size_t opened = 0;
    for (const char* f : files) {
        const QString path = QDir(samples).filePath(f);
        if (!QFile::exists(path)) {
            std::printf("SKIP %s is not there (run formats_test first)\n", f);
            continue;
        }
        const size_t media = st().project().media.size();
        size_t events = 0;
        for (const Track& t : st().project().tracks) events += t.events.size();
        st().setCursor(10.0 * (double)opened);
        const bool ok = w.openProject(path);
        size_t eventsAfter = 0;
        for (const Track& t : st().project().tracks) eventsAfter += t.events.size();
        bool atCursor = false;
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (std::fabs(e.start - 10.0 * (double)opened) < 1e-6) atCursor = true;
        check(ok && st().project().media.size() == media + 1 && eventsAfter > events && atCursor,
              std::string("File > Open of ") + f + " puts it on the timeline at the cursor");
        ++opened;
    }
    check(st().project().media.size() == opened, "nothing was closed in between: all of them are in the one project");

    // ---- a file that is neither a project nor media: says so, changes nothing
    {
        const QString junk = out + "/notes.txt";
        QFile j(junk);
        j.open(QIODevice::WriteOnly);
        j.write("not media\n");
        j.close();
        const size_t media = st().project().media.size();
        // the message box is closed as soon as it shows
        QTimer::singleShot(300, [] {
            for (QWidget* top : QApplication::topLevelWidgets())
                if (auto* m = qobject_cast<QMessageBox*>(top)) m->accept();
        });
        const bool ok = w.openProject(junk);
        check(!ok && st().project().media.size() == media, "a text file is neither: it says so and nothing changes");
    }

    // ---- a .veg still opens as the project
    if (QFile::exists(veg)) {
        const QString copy = out + "/sample.veg";
        QFile::copy(veg, copy);
        check(w.openProject(copy) && st().path() == copy.toStdString(), "a .veg still opens as the project itself");
    }

    // ---- the Explorer shows every kind Reno reads
    {
        QStringList filters;
        for (QFileSystemModel* m : w.findChildren<QFileSystemModel*>())
            if (m->nameFilters().size() > filters.size()) filters = m->nameFilters();
        bool all = true;
        std::string missing;
        for (const char* e : {"*.webm", "*.ogv", "*.opus", "*.mxf", "*.mts", "*.exr", "*.hdr", "*.webp", "*.apng", "*.caf", "*.ra", "*.y4m",
                              "*.aiff", "*.flac", "*.wv", "*.veg"})
            if (!filters.contains(e)) {
                all = false;
                missing += std::string(" ") + e;
            }
        check(all, "the Explorer lists every kind of media Reno reads" + (all ? std::string() : ": not" + missing));
    }
    return uitest::summary();
}
