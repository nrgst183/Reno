// reno_cli (src/cli/main.cpp) against File > Render As: the same project rendered with the same template by the dialog (the
// editor's own path: AppState::open as File > Open, RenderAsDialog, renderProjectAs) and by reno_cli must give the same file -
// picture for picture and sample for sample when the template is lossless, otherwise the same streams, sizes, rates and length.
// The project (made here from real media files, saved as a VEGAS .veg and as a .reno) has a cross-fade, a fade, an OpenFX effect
// (the test plug-in, when given), Reno's built-in Titles & Text (Qt's fonts in both programs), sound from two files, a region and
// a loop region.
//
//   rendercli_test <output folder> <reno_cli> <clipA> <clipB> <sound> [testfx.ofx]
//
// QT_QPA_PLATFORM=offscreen (tests/cli/reno_cli_test.sh makes the media and runs it).
#include "../../src/app/appstate.h"
#include "../../src/app/builtintext.h"
#include "../../src/app/mainwindow.h"  // so the stand-in checks link the windows the main window makes
#include "../../src/app/renderdialog.h"
#include "../../src/app/rendertemplates.h"
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
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut, gRenoRender, gOfxDir;

template <class T>
T* kid(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

void drive(std::function<bool(QWidget*)> step, const std::string& what, int limitMs = 300000)
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

void later(QAbstractButton* b) { QTimer::singleShot(0, b, [b] { b->click(); }); }

QTreeWidgetItem* findRow(QTreeWidget* t, const QString& format, const QString& name)
{
    for (int k = 0; k < t->topLevelItemCount(); ++k) {
        QTreeWidgetItem* top = t->topLevelItem(k);
        if (top->text(0) != format) continue;
        for (int c = 0; c < top->childCount(); ++c) {
            QString n = top->child(c)->text(0);
            if (n.endsWith("  (custom)")) n.chop(10);
            if (n == name) return top->child(c);
        }
    }
    return nullptr;
}

// Renders the open project with the Render As dialog. True when it finished.
bool dialogRender(const QString& format, const QString& tmpl, const QString& name, bool regionOnly)
{
    bool done = false;
    int stage = 0;
    drive([&](QWidget* m) {
        if (stage == 0) {
            auto* tree = kid<QTreeWidget>(m, "templates");
            QTreeWidgetItem* row = tree ? findRow(tree, format, tmpl) : nullptr;
            if (!row) {
                check(false, ("dialog lists " + format + "/" + tmpl).toStdString());
                static_cast<QDialog*>(m)->reject();
                return true;
            }
            tree->setCurrentItem(row);
            kid<QComboBox>(m, "outputFolder")->setEditText(gOut);
            kid<QLineEdit>(m, "outputName")->setText(name);
            auto* region = kid<QCheckBox>(m, "regionOnly");
            if (region && region->isEnabled()) region->setChecked(regionOnly);
            later(kid<QPushButton>(m, "renderButton"));
            stage = 1;
            return false;
        }
        if (auto* box = qobject_cast<QMessageBox*>(m)) {
            box->button(QMessageBox::Yes) ? box->button(QMessageBox::Yes)->click() : box->accept();
            return false;
        }
        auto* c = kid<QPushButton>(m, "closeRender");
        if (c && !c->isHidden()) {
            const QString result = kid<QLabel>(m, "resultLabel") ? kid<QLabel>(m, "resultLabel")->text() : QString();
            done = !result.contains("fail", Qt::CaseInsensitive) && !result.contains("error", Qt::CaseInsensitive);
            if (!done) std::printf("  dialog result: %s\n", result.toUtf8().constData());
            c->click();
            return true;
        }
        return false;
    }, ("dialog render " + name).toStdString());
    runRenderDialog(nullptr);
    return done;
}

// Runs reno_cli; returns its exit code (-1 if it did not run), its output in *text.
int cli(const QStringList& args, QString* text = nullptr)
{
    QProcess p;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!gOfxDir.isEmpty()) env.insert("OFX_PLUGIN_PATH", gOfxDir);
    p.setProcessEnvironment(env);
    p.setProcessChannelMode(QProcess::MergedChannels);
    p.start(gRenoRender, args);
    if (!p.waitForStarted(20000)) return -1;
    p.waitForFinished(600000);
    const QString o = QString::fromUtf8(p.readAll());
    if (text) *text = o;
    if (p.exitStatus() != QProcess::NormalExit) {
        std::printf("%s\n", o.toUtf8().constData());
        return -1;
    }
    if (p.exitCode() != 0) std::printf("  reno_cli said:\n%s\n", o.toUtf8().constData());
    return p.exitCode();
}

struct Diff {
    bool ok = false;
    double maxPix = 0, meanPix = 0, maxAudio = 0;
    int framesCompared = 0;
    std::string why;
};

// Picture by picture (at each frame time in 'times') and the sound (the first 'seconds').
Diff compare(const QString& a, const QString& b, const std::vector<double>& times, double seconds)
{
    Diff d;
    std::string e1, e2;
    std::unique_ptr<MediaReader> ra = openMedia(a.toStdString(), &e1), rb = openMedia(b.toStdString(), &e2);
    if (!ra || !rb) {
        d.why = "cannot open: " + e1 + " " + e2;
        return d;
    }
    const MediaInfo &ia = ra->info(), &ib = rb->info();
    if (ia.hasVideo != ib.hasVideo || ia.hasAudio != ib.hasAudio || ia.width != ib.width || ia.height != ib.height ||
        std::fabs(ia.fps - ib.fps) > 1e-3 || ia.sampleRate != ib.sampleRate || ia.channels != ib.channels || std::fabs(ia.duration - ib.duration) > 0.05) {
        d.why = strfmt("streams differ: %dx%d %.3f fps %d Hz %d ch %.3f s / %dx%d %.3f fps %d Hz %d ch %.3f s", ia.width, ia.height, ia.fps, ia.sampleRate,
                       ia.channels, ia.duration, ib.width, ib.height, ib.fps, ib.sampleRate, ib.channels, ib.duration);
        return d;
    }
    double sum = 0;
    long long n = 0;
    if (ia.hasVideo)
        for (double t : times) {
            ImagePtr fa = ra->videoFrame(t, ia.width, ia.height), fb = rb->videoFrame(t, ib.width, ib.height);
            if (!fa || !fb || fa->w != fb->w || fa->h != fb->h) {
                d.why = strfmt("no picture at %.3f", t);
                return d;
            }
            if (fa->depth != PixelDepth::Byte) fa = fa->converted(PixelDepth::Byte);
            if (fb->depth != PixelDepth::Byte) fb = fb->converted(PixelDepth::Byte);
            for (int y = 0; y < fa->h; ++y) {
                const uint8_t *pa = fa->row(y), *pb = fb->row(y);
                for (int x = 0; x < fa->w * 4; ++x) {
                    if ((x & 3) == 3) continue;     // (alpha)
                    const double v = std::abs((int)pa[x] - (int)pb[x]);
                    d.maxPix = std::max(d.maxPix, v);
                    sum += v;
                    ++n;
                }
            }
            ++d.framesCompared;
        }
    d.meanPix = n ? sum / n : 0;
    if (ia.hasAudio) {
        const int count = (int)(seconds * ia.sampleRate);
        std::vector<float> la(count), lb(count), xa(count), xb(count);
        if (!ra->audio(0, count, ia.sampleRate, la.data(), xa.data()) || !rb->audio(0, count, ib.sampleRate, lb.data(), xb.data())) {
            d.why = "no sound";
            return d;
        }
        for (int k = 0; k < count; ++k) d.maxAudio = std::max(d.maxAudio, (double)std::max(std::fabs(la[k] - lb[k]), std::fabs(xa[k] - xb[k])));
    }
    d.ok = true;
    return d;
}

// The project: real media, a cross-fade, a fade, an effect, Titles & Text, two sounds, a region.
bool buildProject(const QString& clipA, const QString& clipB, const QString& sound)
{
    AppState& app = AppState::get();
    app.newProject();
    app.edit("Settings", [](Project& p) {
        p.settings.width = 640;
        p.settings.height = 360;
        p.settings.fpsNum = 25;
        p.settings.fpsDen = 1;
        p.settings.sampleRate = 48000;
    });
    std::string err;
    const uint64_t a = app.importMedia(clipA.toStdString(), &err), b = app.importMedia(clipB.toStdString(), &err), s = app.importMedia(sound.toStdString(), &err);
    check(a && b && s, "media imported", err);
    if (!a || !b || !s) return false;
    app.addMediaToTimeline(a, 0.0);
    app.addMediaToTimeline(b, 2.5);                     // overlaps the first by half a second: a cross-fade
    const uint64_t text = app.addGeneratedMedia("com.vegascreativesoftware:titlesandtext", "Titles & Text");
    const std::vector<uint64_t> te = app.addMediaToTimeline(text, 1.0);
    const std::vector<uint64_t> se = app.addMediaToTimeline(s, 0.5);
    app.edit("Edits", [&](Project& p) {
        for (auto& t : p.tracks)
            for (auto& e : t.events) {
                if (!te.empty() && e.id == te[0]) { e.length = 2.0; e.fadeIn = 0.5; e.fadeOut = 0.5; }
                if (!se.empty() && e.id == se[0]) { e.length = std::min(e.length, 3.0); e.gain = 0.5; e.fadeOut = 0.4; }
            }
        Project::Marker r;
        r.id = p.newId();
        r.time = 1.0;
        r.length = 1.2;
        r.name = "Middle";
        r.region = true;
        p.markers.push_back(r);
    });
    // the test effect on the first clip's picture
    for (const auto& e : Catalog::get().video())
        if (e.ok && e.id == "reno.test.invert") {
            for (const auto& t : app.project().tracks)
                if (t.type == TrackType::Video)
                    for (const auto& ev : t.events)
                        if (ev.mediaId == a) {
                            FxTarget target;
                            target.kind = FxTarget::Event;
                            target.id = ev.id;
                            app.addVideoFx(target, e.id, e.label);
                        }
        }
    return true;
}

void testProject(const QString& projectFile)
{
    const std::string tag = QFileInfo(projectFile).suffix().toStdString();
    AppState& app = AppState::get();
    std::string err;
    // opened as File > Open opens it, as reno_cli opens it
    check(app.open(projectFile.toStdString(), &err), tag + ": project opens", err);
    uitest::settle(200);
    const double len = app.project().length();
    check(len > 6.4 && len < 6.6, tag + ": project length", strfmt("%.3f", len));  // the second 4 s clip starts at 2.5

    // 1. lossless: uncompressed AVI, every picture and sample the same
    {
        const QString d = gOut + "/" + QString::fromStdString(tag) + "_dialog.avi", c = gOut + "/" + QString::fromStdString(tag) + "_cli.avi";
        QFile::remove(d);
        QFile::remove(c);
        check(dialogRender("Video for Windows", "Default Template (uncompressed)", QFileInfo(d).completeBaseName(), false), tag + ": dialog renders AVI");
        const int rc = cli({projectFile, "-o", c, "-t", "Video for Windows/Default Template (uncompressed)", "-y", "--no-scan"});
        check(rc == 0, tag + ": reno_cli renders AVI", strfmt("exit %d", rc));
        std::vector<double> times;
        for (int f = 0; f < 162; f += 2) times.push_back(f / 25.0 + 0.001);
        const Diff df = compare(d, c, times, 6.4);
        check(df.ok, tag + ": AVI files compared", df.why);
        check(df.framesCompared >= 80 && df.maxPix == 0, tag + ": AVI pictures identical", strfmt("%d frames, max diff %.0f", df.framesCompared, df.maxPix));
        check(df.maxAudio == 0, tag + ": AVI sound identical", strfmt("max diff %g", df.maxAudio));
    }
    // 2. the loop region only, as MP4 at another size and rate (Render As's "Render loop region only")
    {
        app.setLoop(1.0, 2.2);
        const QString d = gOut + "/" + QString::fromStdString(tag) + "_dialog_loop.mp4", c = gOut + "/" + QString::fromStdString(tag) + "_cli_loop.mp4";
        QFile::remove(d);
        QFile::remove(c);
        check(dialogRender("MAGIX AVC/AAC MP4", "Internet HD 720p 25 fps", QFileInfo(d).completeBaseName(), true), tag + ": dialog renders the loop region");
        const int rc = cli({projectFile, "-o", c, "-t", "MAGIX AVC/AAC MP4/Internet HD 720p 25 fps", "-y", "--no-scan", "--loop-region", "1,2.2"});
        check(rc == 0, tag + ": reno_cli renders the loop region", strfmt("exit %d", rc));
        std::vector<double> times;
        for (int f = 0; f < 30; f += 5) times.push_back(f / 25.0 + 0.001);
        const Diff df = compare(d, c, times, 1.1);
        check(df.ok, tag + ": loop-region MP4s have the same streams and length", df.why);
        // x264 runs on several threads, so two encodes of the same pictures can differ a little
        check(df.ok && df.meanPix < 1.0 && df.maxPix < 40, tag + ": loop-region MP4 pictures the same", strfmt("mean %.3f max %.0f", df.meanPix, df.maxPix));
        check(df.ok && df.maxAudio < 0.02, tag + ": loop-region MP4 sound the same", strfmt("max diff %g", df.maxAudio));
        // the region by name gives the same range
        const QString r = gOut + "/" + QString::fromStdString(tag) + "_cli_region.mp4";
        const int rc2 = cli({projectFile, "-o", r, "-t", "avc/Internet HD 720p 25 fps", "-y", "--no-scan", "--region", "Middle"});
        check(rc2 == 0, tag + ": reno_cli --region", strfmt("exit %d", rc2));
        const Diff dr = compare(c, r, times, 1.1);
        check(dr.ok && dr.meanPix < 1.0, tag + ": --region Middle = --loop-region 1,2.2", dr.why + strfmt(" mean %.3f", dr.meanPix));
        app.setLoop(0, 0);
    }
    // 3. sound only: WAV, sample for sample
    {
        const QString d = gOut + "/" + QString::fromStdString(tag) + "_dialog.wav", c = gOut + "/" + QString::fromStdString(tag) + "_cli.wav";
        QFile::remove(d);
        QFile::remove(c);
        check(dialogRender("Wave (Microsoft)", "48,000 Hz, 24 Bit, Stereo, PCM", QFileInfo(d).completeBaseName(), false), tag + ": dialog renders WAV");
        const int rc = cli({projectFile, "-o", c, "-t", "Wave (Microsoft)/48,000 Hz, 24 Bit, Stereo, PCM", "-y", "--no-scan"});
        check(rc == 0, tag + ": reno_cli renders WAV", strfmt("exit %d", rc));
        const Diff df = compare(d, c, {}, 6.4);
        check(df.ok && df.maxAudio == 0, tag + ": WAV sound identical", df.why + strfmt(" max diff %g", df.maxAudio));
    }
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoRenderCliTest");
    QApplication::setApplicationName("rendercli_test");
    applyTheme();
    installBuiltinTextRasterizer();  // as reno.exe's main() does: the built-in Titles & Text drawn with Qt's fonts
    const QStringList args = QApplication::arguments();
    if (args.size() < 6) {
        qWarning("usage: rendercli_test <output folder> <reno_cli> <clipA> <clipB> <sound> [testfx.ofx]");
        return 2;
    }
    gOut = QDir(args[1]).absolutePath();
    QDir().mkpath(gOut);
    gRenoRender = QFileInfo(args[2]).absoluteFilePath();
    QSettings().clear();
    std::vector<std::string> ofxFiles;
    if (args.size() > 6) {
        ofxFiles.push_back(args[6].toStdString());
        gOfxDir = QFileInfo(args[6]).absolutePath();
    }
    uitest::setup(ofxFiles, std::vector<std::string>());

    check(buildProject(QFileInfo(args[3]).absoluteFilePath(), QFileInfo(args[4]).absoluteFilePath(), QFileInfo(args[5]).absoluteFilePath()), "project built");
    std::string err;
    const QString veg = gOut + "/rendercli_project.veg", reno = gOut + "/rendercli_project.reno";
    QFile::remove(veg);
    QFile::remove(reno);
    check(AppState::get().save(veg.toStdString(), &err), "saved as .veg", err);
    check(AppState::get().save(reno.toStdString(), &err), "saved as .reno", err);
    testProject(reno);
    testProject(veg);
    const int rc = uitest::summary();
    std::fflush(stdout);
    std::_Exit(rc);
}
