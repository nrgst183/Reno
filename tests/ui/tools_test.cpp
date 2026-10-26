// Windows and tools that mainwindow_tools.cpp adds to the main window: the Trimmer (Alt+2), recording (Ctrl+R, arming), the Loudness
// Meters (Ctrl+Alt+7) and Selective Prerender (Shift+M). Saves pictures.
//
//   tools_test <output folder> [testfx.ofx]
//
// Build: uibuild tools_test tests/ui/tools_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// (with EXTRA_CFLAGS for the Qt private headers, as keys_test). QT_QPA_PLATFORM=offscreen.
#include "../../src/app/mainwindow.h"
#include "../../src/app/mainwindow_tools.h"
#include "../../src/app/timeline.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/app/trimmer.h"
#include "../../src/app/recorder.h"
#include "../../src/app/loudnesswin.h"
#include "../../src/app/pluginmanager.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/app/dashboard.h"
#include "../../src/app/deviceexplorer.h"
#include "../../src/app/externalmonitor.h"
#include "../../src/app/colorgrading.h"
#include <QToolButton>
#include <QCheckBox>
#include "../../src/app/motiontracking.h"
#include "../../src/app/tts.h"
#include "../../src/app/midisync.h"
#include "../../src/engine/builtinfx.h"
#include "../../src/engine/tracker.h"
#include <QPushButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSlider>
#include <QTabWidget>
#include <QKeyEvent>
#include <QScreen>
#include "uitestlib.h"
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QDockWidget>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTimer>
#include <QElapsedTimer>
#include <QTreeWidget>
#include <QWindow>
#include <qpa/qwindowsysteminterface.h>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
MainWindow* gW = nullptr;
bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }
std::string num(double v) { return strfmt("%.4f", v); }

QString textOf(int key, Qt::KeyboardModifiers mods)
{
    if (mods & (Qt::ControlModifier | Qt::AltModifier)) return QString();
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return QString(QChar((mods & Qt::ShiftModifier) ? key : key - Qt::Key_A + 'a'));
    if (key >= Qt::Key_Space && key <= Qt::Key_AsciiTilde) return QString(QChar(key));
    if (key == Qt::Key_Return || key == Qt::Key_Enter) return QString("\r");
    return QString();
}

// A key pressed and released (to whatever has the focus).
void press(int key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    gW->activateWindow();
    QCoreApplication::processEvents();
    QWindow* win = QGuiApplication::focusWindow() ? QGuiApplication::focusWindow() : gW->windowHandle();
    const QString text = textOf(key, mods);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyPress, key, mods, text);
    QWindowSystemInterface::handleKeyEvent<QWindowSystemInterface::SynchronousDelivery>(win, QEvent::KeyRelease, key, mods, text);
    QCoreApplication::processEvents();
}
void press(const char* seq)
{
    const QKeySequence ks(QString::fromUtf8(seq));
    for (int k = 0; k < ks.count(); ++k) press(ks[k].key(), ks[k].keyboardModifiers());
}

uint64_t clip = 0, snd = 0, v1 = 0, a1 = 0, a2 = 0, ev1 = 0, ev2 = 0;

void buildProject()
{
    st().newProject();
    std::string err;
    clip = st().importMedia("synth:1", &err);
    snd = st().importMedia("synthaudio:2", &err);
    st().edit("Build", [&](Project& p) {
        for (int k = 0; k < 3; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k == 0 ? TrackType::Video : TrackType::Audio;
            t.name = k == 0 ? "Camera" : (k == 1 ? "Voice" : "Music");
            p.tracks.push_back(t);
        }
        v1 = p.tracks[0].id;
        a1 = p.tracks[1].id;
        a2 = p.tracks[2].id;
        Event e;
        e.id = p.newId();
        e.mediaId = clip;
        e.start = 0;
        e.length = 4;
        e.name = "clip";
        p.tracks[0].events.push_back(e);
        ev1 = e.id;
        Event m;
        m.id = p.newId();
        m.mediaId = snd;
        m.start = 0;
        m.length = 8;
        m.name = "music";
        p.tracks[2].events.push_back(m);
        ev2 = m.id;
        // and a second picture later on the video track
        Event e2 = e;
        e2.id = p.newId();
        e2.start = 6;
        e2.length = 2;
        p.tracks[0].events.push_back(e2);
    });
    st().setSelection(std::set<uint64_t>());
    st().setCursor(0.0);
    st().setLoop(0, 0);
    st().loopOn = false;
    st().currentTrack = a1;
}

// A pretend microphone: a 440 Hz tone at half scale, pushed in 10 ms pieces on a timer (as Qt's input does on the UI thread).
class FakeInput : public AudioInput {
public:
    bool start(int sampleRate, Push push, std::string*) override
    {
        rate_ = sampleRate;
        push_ = push;
        timer_ = new QTimer();
        timer_->setInterval(10);
        clock_.start();
        QObject::connect(timer_, &QTimer::timeout, [this] {
            const int64_t due = clock_.elapsed() * (int64_t)rate_ / 1000;
            const int n = (int)(due - sent_);
            if (n <= 0) return;
            std::vector<float> l((size_t)n), r((size_t)n);
            for (int k = 0; k < n; ++k) l[(size_t)k] = r[(size_t)k] = 0.5f * (float)std::sin(2.0 * 3.14159265358979 * 440.0 * (double)(sent_ + k) / rate_);
            sent_ += n;
            push_(l.data(), r.data(), n);
        });
        timer_->start();
        ++started;
        return true;
    }
    void stop() override
    {
        delete timer_;
        timer_ = nullptr;
    }
    std::string name() const override { return "Pretend Microphone"; }
    static int started;

private:
    int rate_ = 48000;
    Push push_;
    QTimer* timer_ = nullptr;
    QElapsedTimer clock_;
    int64_t sent_ = 0;
};
int FakeInput::started = 0;

double meanDiff(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull()) return 1e9;
    const QImage x = a.convertToFormat(QImage::Format_RGB32), y = b.convertToFormat(QImage::Format_RGB32);
    double sum = 0;
    for (int r = 0; r < x.height(); ++r) {
        const QRgb* p = (const QRgb*)x.constScanLine(r);
        const QRgb* q = (const QRgb*)y.constScanLine(r);
        for (int c = 0; c < x.width(); ++c)
            sum += std::abs(qRed(p[c]) - qRed(q[c])) + std::abs(qGreen(p[c]) - qGreen(q[c])) + std::abs(qBlue(p[c]) - qBlue(q[c]));
    }
    return sum / (3.0 * x.width() * x.height());
}

// "movingbox:" a 10-second 320x180 clip whose picture scales with the size asked for (as real video does): a textured background
// and a textured square moving right and bobbing up and down (see boxAt).
float texNoise(int x, int y, int seed)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xffff) / 65535.f;
}
void boxAt(double t, double& cx, double& cy)
{
    const double f = std::floor(t * 30.0 + 1e-6);
    cx = 0.2 + 0.008 * f;
    cy = 0.5 + 0.12 * std::sin(f / 8.0);
}
class MovingBox : public MediaReader {
public:
    MediaInfo mi;
    MovingBox()
    {
        mi.path = "movingbox:";
        mi.hasVideo = true;
        mi.width = 320;
        mi.height = 180;
        mi.fps = 30;
        mi.duration = 10;
    }
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        const double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)std::lround(mi.width * s)), h = std::max(1, (int)std::lround(mi.height * s));
        double cx, cy;
        boxAt(seconds, cx, cy);
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(h - 1 - y);
            for (int x = 0; x < w; ++x, d += 4) {
                const double u = (x + 0.5) / w, v = (y + 0.5) / h;
                double val = 0.25 + 0.3 * texNoise((int)(u * 40), (int)(v * 22), 1);
                const double bu = (u - (cx - 0.06)) / 0.12, bv = (v - (cy - 0.1)) / 0.2;
                if (bu >= 0 && bu < 1 && bv >= 0 && bv < 1) val = 0.15 + 0.8 * texNoise((int)(bu * 6), (int)(bv * 6), 9);
                const uint8_t c = (uint8_t)std::lround(val * 255);
                d[0] = c; d[1] = c; d[2] = (uint8_t)std::min(255, c + 20); d[3] = 255;
            }
        }
        return img;
    }
    bool audio(int64_t, int, int, float*, float*) override { return false; }
};

// A pretend MIDI port: what is sent is kept (with when, in ms); what is "received" is handed on by the test.
class FakeMidi : public MidiPort {
public:
    struct Sent { qint64 at; std::vector<uint8_t> msg; };
    std::shared_ptr<std::vector<Sent>> log = std::make_shared<std::vector<Sent>>();
    QElapsedTimer clock;
    FakeMidi() { clock.start(); }
    bool send(const std::vector<uint8_t>& m) override
    {
        log->push_back({clock.elapsed(), m});
        return true;
    }
    std::string name() const override { return "Pretend MIDI"; }
};

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoToolsTest");
    QApplication::setApplicationName("tools_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = QDir(args.size() > 1 ? args[1] : QString("toolsout")).absolutePath();
    QDir().mkpath(out);
    std::vector<std::string> ofxFiles;
    if (args.size() > 2) ofxFiles.push_back(args[2].toStdString());
    uitest::setup(ofxFiles, std::vector<std::string>());
    st().prefs.recordFolder = QDir(out).filePath("recorded").toStdString();
    st().prefs.prerenderFolder = QDir(out).filePath("prerendered").toStdString();

    buildProject();
    MainWindow w;
    gW = &w;
    w.resize(1600, 900);
    w.show();
    w.activateWindow();
    uitest::settle(300);
    buildProject();
    MainWindow::Tools* tools = w.tools();
    check(tools && tools->trimmer && tools->recorder && tools->loudness, "the main window makes the Trimmer, the recorder and the Loudness Meters");
    if (!tools) return uitest::summary();
    Playback* pb = w.playback();

    // ================================================================================== the Trimmer
    {
        TrimmerWindow* tr = tools->trimmer;
        press("Alt+2");
        uitest::settle(100);
        check(tools->trimmerDock->isVisible(), "Alt+2 shows the Trimmer");
        // a double-click in Project Media opens the media in it
        w.findChild<QDockWidget*>("dockMedia")->show();
        w.findChild<QDockWidget*>("dockMedia")->raise();
        uitest::settle(100);
        QTreeWidget* list = w.findChild<QTreeWidget*>("mediaList");
        QTreeWidgetItem* first = list ? list->topLevelItem(0) : nullptr;
        const size_t eventsBefore = st().project().tracks[0].events.size();
        // on the row's visible part: with its columns the row is wider than the list
        if (first) uitest::mouseDoubleClick(list->viewport(), QPoint(std::min(list->visualItemRect(first).center().x(), list->viewport()->width() / 2),
                                                                     list->visualItemRect(first).center().y()));
        uitest::settle(300);
        check(tr->currentMedia() == clip && tr->currentPath() == "synth:1" && st().project().tracks[0].events.size() == eventsBefore,
              "a double-click in Project Media opens the media in the Trimmer (not onto the timeline)", tr->currentPath().toStdString());
        for (int k = 0; k < 40 && (tr->picture().isNull() || !tr->waveformReady()); ++k) uitest::settle(50);
        check(!tr->picture().isNull(), "the Trimmer shows the picture");
        const std::vector<float> wave = tr->waveform();
        float lo = 1, hi = 0;
        for (float v : wave) { lo = std::min(lo, v); hi = std::max(hi, v); }
        check(tr->waveformReady() && wave.size() > 100 && hi > 0.55 && lo < 0.2, "and the sound's waveform (the tone swells and fades)",
              strfmt("%d peaks, %.2f..%.2f", (int)wave.size(), lo, hi));
        check(near(tr->duration(), 10.0), "its length is the file's", num(tr->duration()));
        const QImage at0 = tr->picture();
        tr->setPosition(2.0);
        for (int k = 0; k < 20; ++k) uitest::settle(25);
        check(meanDiff(at0, tr->picture()) > 1.0, "moving the Trimmer's cursor shows another frame", num(meanDiff(at0, tr->picture())));
        // in and out with I and O (the Trimmer has the focus)
        tr->setFocus();
        tr->setPosition(1.0);
        press(Qt::Key_I);
        tr->setPosition(3.0);
        press(Qt::Key_O);
        check(near(tr->selectionIn(), 1.0) && near(tr->selectionOut(), 3.0), "I and O set the in and out points", num(tr->selectionIn()) + " " + num(tr->selectionOut()));
        // A puts the selection on the timeline at the timeline's cursor
        st().setCursor(10.0);
        st().currentTrack = v1;
        press(Qt::Key_A);
        uitest::settle(50);
        const Event* added = nullptr;
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (t.type == TrackType::Video && near(e.start, 10.0)) added = &e;
        check(added && near(added->offset, 1.0) && near(added->length, 2.0) && added->mediaId == clip,
              "A puts the selection (1 s .. 3 s of the file) on the timeline at the cursor",
              added ? num(added->start) + " " + num(added->offset) + " " + num(added->length) : std::string("none"));
        check(near(st().cursor(), 12.0), "and the timeline's cursor moves to its end", num(st().cursor()));
        int audioAdded = 0;
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (t.type == TrackType::Audio && near(e.start, 10.0) && near(e.offset, 1.0) && near(e.length, 2.0)) ++audioAdded;
        check(audioAdded == 1, "with its sound beside it, trimmed the same");
        // Shift+A: so that it ends at the cursor
        st().setCursor(20.0);
        press(Qt::Key_A, Qt::ShiftModifier);
        bool endsThere = false;
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (t.type == TrackType::Video && near(e.end(), 20.0) && near(e.start, 18.0)) endsThere = true;
        check(endsThere && near(st().cursor(), 20.0), "Shift+A puts it so that it ends at the cursor");
        check(st().nextAddRange.mediaId == 0, "the Trimmer's range is used once, then forgotten");
        // the whole file with no selection
        tr->setSelection(0, 0);
        st().setCursor(30.0);
        tr->addToTimeline(false);
        bool whole = false;
        for (const Track& t : st().project().tracks)
            for (const Event& e : t.events)
                if (t.type == TrackType::Video && near(e.start, 30.0) && near(e.length, 10.0) && near(e.offset, 0.0)) whole = true;
        check(whole, "with no selection, A adds the whole file");
        // Space plays the Trimmer (within the selection), not the timeline
        tr->setSelection(4.0, 6.0);
        tr->setPosition(4.0);
        tr->setFocus();
        press(Qt::Key_Space);
        uitest::settle(400);
        check(tr->playing() && !pb->playing() && tr->position() > 4.15, "Space plays the Trimmer while it has the focus (not the timeline)", num(tr->position()));
        tr->setLoop(false);
        for (int k = 0; k < 120 && tr->playing(); ++k) uitest::settle(25);
        check(!tr->playing() && near(tr->position(), 6.0, 0.05), "and it stops at the end of the selection", num(tr->position()));
        // another file from the Explorer (not yet in the project): added when it goes on the timeline
        openInTrimmer(0, "synthaudio:5");
        uitest::settle(100);
        check(tr->currentPath() == "synthaudio:5" && tr->currentMedia() == 0, "a file from the Explorer opens in the Trimmer (not yet in the project)");
        const size_t mediaBefore = st().project().media.size();
        st().setCursor(40.0);
        tr->addToTimeline(false);
        check(st().project().media.size() == mediaBefore + 1 && tr->currentMedia() != 0, "and is added to Project Media when it goes on the timeline");
        // the history
        const QStringList h = tr->history();
        check(h.size() == 2 && h[0] == "synthaudio:5" && h[1] == "synth:1", "the Trimmer history lists the files, newest first", h.join(",").toStdString());
        tr->setFocus();
        press("Ctrl+H");
        check(tr->history() == QStringList() << "synth:1", "Ctrl+H takes the file out of the history", tr->history().join(",").toStdString());
        tr->openMedia(clip);
        tr->setSelection(2.0, 5.5);
        tr->setPosition(3.0);
        for (int k = 0; k < 40 && !tr->waveformReady(); ++k) uitest::settle(50);
        uitest::settle(200);
        tools->trimmerDock->resize(700, 420);
        tr->resize(700, 400);
        uitest::settle(100);
        uitest::shot(tr, out + "/trimmer.png");
        // ---- the other Trimmer keys
        tr->setFocus();
        // T / Shift+T: as long a stretch as the timeline's time selection, from / up to the Trimmer's cursor
        st().setLoop(12.0, 13.5);
        tr->setSelection(0, 0);
        tr->setPosition(4.0);
        press(Qt::Key_T);
        check(near(tr->selectionIn(), 4.0) && near(tr->selectionOut(), 5.5), "T selects the timeline's time selection's length from the Trimmer's cursor",
              num(tr->selectionIn()) + " " + num(tr->selectionOut()));
        press(Qt::Key_T, Qt::ShiftModifier);
        check(near(tr->selectionIn(), 2.5) && near(tr->selectionOut(), 4.0), "Shift+T up to it", num(tr->selectionIn()) + " " + num(tr->selectionOut()));
        tr->setPosition(9.5);
        press(Qt::Key_T);
        check(near(tr->selectionIn(), 8.5) && near(tr->selectionOut(), 10.0), "(kept inside the file)", num(tr->selectionIn()) + " " + num(tr->selectionOut()));
        // Tab: the picture only, the sound only, both
        press(Qt::Key_Tab);
        check(tr->streams() == TrimmerWindow::VideoOnly && tr->hasFocus(), "Tab: the video stream only (and the focus stays)");
        st().setCursor(50.0);
        std::vector<uint64_t> made = tr->addToTimeline(false);
        bool videoOnly = made.size() == 1;
        for (uint64_t id : made) {
            const Track* t = nullptr;
            st().project().findEvent(id, &t);
            videoOnly = videoOnly && t && t->type == TrackType::Video;
        }
        check(videoOnly, "and then A adds only the picture", strfmt("%d events", (int)made.size()));
        press(Qt::Key_Tab);
        uitest::shot(tr, out + "/trimmer_audio_only.png");
        st().setCursor(60.0);
        made = tr->addToTimeline(false);
        bool audioOnly = made.size() == 1;
        for (uint64_t id : made) {
            const Track* t = nullptr;
            st().project().findEvent(id, &t);
            audioOnly = audioOnly && t && t->type == TrackType::Audio;
        }
        check(tr->streams() == TrimmerWindow::AudioOnly && audioOnly, "Tab again: only the sound");
        press(Qt::Key_Tab);
        check(tr->streams() == TrimmerWindow::BothStreams, "and again both");
        // Ctrl+Shift+Up / Down: the picture or the waveform larger
        const int h0 = tr->strip()->height();
        press("Ctrl+Shift+Down");
        uitest::settle(50);
        const int hBig = tr->strip()->height();
        press("Ctrl+Shift+Up");
        press("Ctrl+Shift+Up");
        uitest::settle(50);
        const int hSmall = tr->strip()->height();
        check(hBig > h0 && hSmall < h0, "Ctrl+Shift+Down makes the waveform larger, Ctrl+Shift+Up the picture", strfmt("%d %d %d", h0, hBig, hSmall));
        press("Ctrl+Shift+Down");
        uitest::settle(50);
        // Ctrl+E: the audio editor
        std::string said;
        auto oldStatus = st().onStatus;
        st().onStatus = [&](const std::string& m) { said = m; };
        st().prefs.audioEditor = "/bin/true";
        tr->openFile(QDir(out).absoluteFilePath("trimmer.png"));          // (a real file)
        tr->setFocus();
        press("Ctrl+E");
        check(said.find("Opened trimmer.png in true") == 0, "Ctrl+E opens the Trimmer's file in the audio editor", said);
        st().prefs.audioEditor.clear();
        st().onStatus = oldStatus;
        tr->openMedia(clip);
        press("Ctrl+Shift+H");
        check(tr->history().isEmpty(), "Ctrl+Shift+H clears it");
        // a file that cannot be read
        check(!tr->openFile("/no/such/file.mp4"), "a file that cannot be read is refused (with a message)");
    }

    // ================================================================================== recording
    {
        buildProject();
        QSettings().setValue("prefs/promptKeepRecorded", false);
        Recorder* rec = tools->recorder.get();
        rec->inputFactory = [] { return std::unique_ptr<AudioInput>(new FakeInput()); };
        TimelineWidget* tl = w.timeline();
        tl->setFocus();
        uitest::settle(100);
        // the Arm for Record button on the audio track's header (shown where the header leaves the name room enough; VEGAS 18's look leaves it out of a header of its default width)
        const int headerWas = tl->headerWidth();
        tl->setHeaderWidth(260);
        uitest::settle(50);
        const QRect arm = tl->headerPartRect(a1, TimelineWidget::PartArm);
        check(!arm.isEmpty() && tl->headerPartRect(v1, TimelineWidget::PartArm).isEmpty(), "audio tracks have an Arm for Record button (video tracks none)");
        if (!arm.isEmpty()) uitest::mouseClick(tl, arm.center());
        uitest::settle(100);
        check(st().armedTracks.count(a1) == 1 && rec->inputOpen() && tools->inputMeter->isVisible(), "clicking it arms the track and opens the input (its meter shows)");
        uitest::settle(200);
        float l = 0, r = 0;
        rec->inputLevel(l, r);
        check(l > 0.3f && r > 0.3f, "the input meter reads the input's level", strfmt("%.2f %.2f", l, r));
        uitest::shot(tl, out + "/armed_track.png");
        tl->setHeaderWidth(headerWas);
        // Ctrl+R records from the cursor while the project plays
        st().setCursor(1.0);
        press("Ctrl+R");
        check(rec->recording() && pb->playing(), "Ctrl+R records (and plays the project)");
        uitest::settle(1100);
        check(rec->recordedSeconds() > 0.8, "sound comes in while it records", num(rec->recordedSeconds()));
        press("Ctrl+R");
        uitest::settle(100);
        check(!rec->recording() && !pb->playing(), "Ctrl+R again stops");
        const std::string file = rec->lastFile();
        check(QFileInfo::exists(QString::fromStdString(file)) && QString::fromStdString(file).startsWith(QDir(out).filePath("recorded")),
              "the recording is a WAV file in the recording folder", file);
        const Track* t1 = nullptr;
        for (const Track& t : st().project().tracks)
            if (t.id == a1) t1 = &t;
        const Event* re = t1 && t1->events.size() == 1 ? &t1->events[0] : nullptr;
        const MediaItem* rm = re ? st().project().findMedia(re->mediaId) : nullptr;
        check(re && near(re->start, 1.0) && re->length > 0.8 && re->length < 2.0 && rm && rm->path == file,
              "and an event of it is on the armed track where recording began", re ? num(re->start) + " " + num(re->length) : std::string("none"));
        // the file holds the tone
        {
            std::string err;
            std::unique_ptr<MediaReader> rd = openMedia(file, &err);
            std::vector<float> L(4800), R(4800);
            float pk = 0;
            if (rd && rd->audio(4800, 4800, 48000, L.data(), R.data()))
                for (float v : L) pk = std::max(pk, std::fabs(v));
            check(rd && rd->info().hasAudio && std::fabs(pk - 0.5f) < 0.02f, "the file holds what came in (a tone at half scale)", strfmt("%.3f %s", pk, err.c_str()));
        }
        check(st().canUndo() && st().undoName() == "Record", "recording is one undo step", st().undoName());
        // recording over part of an event cuts it back; Space stops the recording too
        rec->setArmed(a1, false);
        rec->setArmed(a2, true);
        check(st().armedTracks.size() == 1 && st().armedTracks.count(a2), "arming moves to the other track");
        st().setCursor(2.0);
        std::string err;
        check(rec->start(&err), "recording starts on the music track", err);
        uitest::settle(1000);
        press(Qt::Key_Space);
        uitest::settle(200);
        check(!rec->recording(), "Space (stopping playback) stops the recording too");
        const Track* t2 = nullptr;
        for (const Track& t : st().project().tracks)
            if (t.id == a2) t2 = &t;
        bool cutBefore = false, cutAfter = false, newOne = false;
        if (t2)
            for (const Event& e : t2->events) {
                if (e.mediaId == snd && near(e.start, 0.0) && near(e.length, 2.0)) cutBefore = true;
                if (e.mediaId == snd && e.start > 2.5 && near(e.end(), 8.0) && e.offset > 0.5) cutAfter = true;
                if (e.mediaId != snd && near(e.start, 2.0)) newOne = true;
            }
        check(cutBefore && cutAfter && newOne, "recording over an event cuts it back round the new recording", strfmt("%d events", t2 ? (int)t2->events.size() : 0));
        // Ctrl+Alt+R arms the selected track; with nothing armed, Ctrl+R arms the selected audio track
        rec->setArmed(a2, false);
        check(!rec->inputOpen(), "with nothing armed the input is closed");
        st().currentTrack = a1;
        press("Ctrl+Alt+R");
        check(st().armedTracks.count(a1) == 1, "Ctrl+Alt+R arms the selected audio track");
        press("Ctrl+Alt+R");
        check(st().armedTracks.empty(), "and again disarms it");
        st().currentTrack = a2;
        st().setCursor(9.0);
        press("Ctrl+R");
        uitest::settle(400);
        press("Ctrl+R");
        check(st().armedTracks.count(a2) == 1, "Ctrl+R with nothing armed arms the selected audio track and records into it");
        rec->setArmed(a2, false);
    }

    // ================================================================================== the Loudness Meters
    {
        buildProject();
        press("Ctrl+Alt+7");
        uitest::settle(100);
        LoudnessWindow* lw = tools->loudness;
        check(tools->loudnessDock->isVisible() && lw->listening(), "Ctrl+Alt+7 shows the Loudness Meters, listening to the output");
        // measured at once: the music event (8 s of a swelling tone at 440 Hz)
        lw->measure(0.0, 8.0);
        LoudnessWindow::Reading m = lw->reading();
        check(m.integrated > -30 && m.integrated < -5 && near(m.seconds, 8.0, 0.01) && m.truePeak < 0.0 && m.truePeak > -6.0,
              "Measure works out the project's loudness", strfmt("I %.2f LRA %.2f TP %.2f (%.2f s)", m.integrated, m.range, m.truePeak, m.seconds));
        // playing: the meters follow what is heard
        lw->reset();
        check(lw->reading().seconds == 0, "Reset starts again");
        st().setCursor(1.0);
        pb->play();
        uitest::settle(1200);
        const LoudnessWindow::Reading pl = lw->reading();
        pb->pause();
        check(pl.seconds > 0.8 && pl.momentary > -40 && pl.momentary < 0, "while playing it measures what is heard",
              strfmt("%.2f s, M %.2f, S %.2f", pl.seconds, pl.momentary, pl.shortTerm));
        // the master's volume turned down 10 dB: 10 LU quieter
        lw->measure(0.0, 8.0);
        const double before = lw->reading().integrated;
        st().edit("Quieter", [](Project& p) { p.masterVolume = std::pow(10.0, -10.0 / 20.0); });
        lw->measure(0.0, 8.0);
        const double after = lw->reading().integrated;
        check(std::fabs(before - after - 10.0) < 0.2, "the master 10 dB down measures 10 LU quieter", num(before) + " -> " + num(after));
        tools->loudnessDock->resize(520, 260);
        lw->resize(520, 240);
        lw->measure(0.0, 8.0);
        uitest::settle(150);
        uitest::shot(lw, out + "/loudness.png");
        // hidden: it stops listening
        tools->loudnessDock->hide();
        uitest::settle(50);
        check(!lw->listening(), "closed, it stops listening");
    }

    // ================================================================================== selective prerender
    {
        buildProject();
        st().setLoop(1.0, 2.0);
        press("Shift+M");
        uitest::settle(100);
        const std::vector<Playback::PrerenderedRange> pr = pb->prerenderedRanges();
        check(pr.size() == 1 && pr[0].valid && near(pr[0].from, 1.0) && near(pr[0].to, 2.0) && QFileInfo::exists(QString::fromStdString(pr[0].path)),
              "Shift+M prerenders the time selection to a file", pr.empty() ? std::string("none") : pr[0].path);
        check(pb->prerenderedAt(1.5) && !pb->prerenderedAt(2.5) && !pb->prerenderedAt(0.5), "the stretch plays from the file (and nothing else)");
        // the preview shows a frame from the file, the same picture as rendering it gives
        const uint64_t shownBefore = pb->prerenderedFramesShown();
        QImage fromFile;
        pb->onFrame = [&](const QImage& img, double, double) { fromFile = img; };
        st().setCursor(1.5);
        for (int k = 0; k < 40 && pb->prerenderedFramesShown() == shownBefore; ++k) uitest::settle(25);
        uitest::settle(50);
        pb->onFrame = nullptr;
        const QImage rendered = pb->renderNow(1.5);
        check(pb->prerenderedFramesShown() > shownBefore && meanDiff(fromFile, rendered) < 6.0, "the preview takes the frame from the file (as rendering gives it)",
              strfmt("%llu frames, difference %.2f", (unsigned long long)pb->prerenderedFramesShown(), meanDiff(fromFile, rendered)));
        // a change elsewhere keeps it; a change there lets it go
        st().edit("Elsewhere", [](Project& p) { if (Event* e = p.findEvent(ev2)) e->gain = 0.5; });
        check(pb->prerenderedAt(1.5), "a change to the sound or elsewhere keeps the prerendered stretch");
        st().edit("Later", [](Project& p) { p.tracks[0].events.back().start = 6.5; });
        check(pb->prerenderedAt(1.5), "and so does moving a picture after it");
        st().edit("There", [](Project& p) { if (Event* e = p.findEvent(ev1)) e->gain = 0.5; });
        check(!pb->prerenderedAt(1.5), "a change to an event there lets it go");
        st().undo();
        check(pb->prerenderedAt(1.5), "and undoing the change brings it back");
        st().edit("Output", [](Project& p) { p.settings.width = 640; });
        check(!pb->prerenderedAt(1.5), "so does a change to the project's settings");
        st().undo();
        w.findChild<QAction*>("cleanPrerender")->trigger();
        check(pb->prerenderedRanges().empty() && !QFileInfo::exists(QString::fromStdString(pr.empty() ? std::string() : pr[0].path)),
              "Clean Up Prerendered Video deletes the files");
        st().setLoop(0, 0);
        press("Shift+M");
        check(pb->prerenderedRanges().empty(), "with no time selection Shift+M does nothing");
    }

    // ================================================================================== adjustment tracks
    {
        buildProject();
        st().currentTrack = v1;
        const size_t before = st().project().tracks.size();
        // As in VEGAS, the new track's Track FX open with the Plug-In Chooser first (modal: driven from its onShown hook). Cancel keeps the empty adjustment track and opens nothing.
        bool chooserShown = false;
        PluginChooser::onShown = [&](PluginChooser* c) {
            PluginChooser::onShown = nullptr;
            chooserShown = true;
            c->reject();
        };
        press("Ctrl+Shift+Alt+Q");
        uitest::settle(200);
        PluginChooser::onShown = nullptr;
        const Project& p = st().project();
        check(p.tracks.size() == before + 1 && p.tracks[0].adjustment && p.tracks[0].type == TrackType::Video && p.tracks[0].events.empty() &&
                  p.tracks[0].name == "Adjustment Track" && st().currentTrack == p.tracks[0].id,
              "Ctrl+Shift+Alt+Q inserts an adjustment track above the selected video track, and selects it");
        check(st().undoName() == "Insert Adjustment Track", "as one undo step", st().undoName());
        check(chooserShown && !w.fxWindow()->isVisible(), "the Plug-In Chooser comes up for its Track FX; Cancel opens nothing more");
        st().undo();
        check(st().project().tracks.size() == before, "undo takes it away");
        // OK with an effect: the track has it, and the effects window shows it
        if (!ofxFiles.empty()) {
            const QImage plain = pb->renderNow(1.0);
            PluginChooser::onShown = [&](PluginChooser* c) {
                PluginChooser::onShown = nullptr;
                c->openFolder("All");
                c->addByLabel("Test Invert");
                c->findChild<QPushButton*>("chooserOk")->click();
            };
            press("Ctrl+Shift+Alt+Q");
            uitest::settle(200);
            PluginChooser::onShown = nullptr;
            const Project& q = st().project();
            check(q.tracks.size() == before + 1 && q.tracks[0].adjustment && q.tracks[0].fx.size() == 1,
                  "OK in the chooser puts the effect on the adjustment track", strfmt("%d effects", (int)q.tracks[0].fx.size()));
            check(w.fxWindow()->isVisible(), "with its Track FX open");
            w.fxWindow()->close();
            uitest::shot(w.timeline(), out + "/adjustment_track.png");
            // its effect changes the picture of the track under it
            const QImage inv = pb->renderNow(1.0);
            const QRgb a = plain.pixel(10, 10), b = inv.pixel(10, 10);
            check(std::abs(qRed(a) + qRed(b) - 255) <= 2 && std::abs(qGreen(a) + qGreen(b) - 255) <= 2, "an effect on it inverts the picture under it",
                  strfmt("%08x -> %08x", a, b));
            st().undo();
            st().undo();
        }
        check(st().project().tracks.size() == before, "undo takes it away");
    }

    // ================================================================================== the Plug-In Manager
    if (!ofxFiles.empty()) {
        buildProject();
        press("Ctrl+Alt+1");
        uitest::settle(150);
        PluginManager* pm = tools->plugins;
        check(tools->pluginsDock->isVisible(), "Ctrl+Alt+1 shows the Plug-In Manager");
        const int vfx = pm->count("Video FX"), trans = pm->count("Transitions"), gens = pm->count("Media Generators");
        check(vfx >= 8 && trans >= 1 && gens >= 1, "it lists the video effects, transitions and generators found",
              strfmt("%d effects, %d transitions, %d generators", vfx, trans, gens));
        check(pm->keys("Transitions").contains("video:reno.test.transition") && pm->keys("Media Generators").contains("video:reno.test.generator"),
              "each in its folder");
        check(pm->keys("Video FX/Reno Test").contains("video:reno.test.invert"), "effects in folders by their group");
        const int broken = pm->count("Not Working");
        int okCount = 0, badCount = 0;
        for (const VideoFxEntry& e : Catalog::get().video()) (e.ok ? okCount : badCount)++;
        check(broken == badCount, "those that do not work are listed under Not Working", strfmt("%d listed, %d do not work", broken, badCount));
        pm->setFilter("invert");
        // the test effects' Test Invert, and Reno's own Invert: every working filter named so, and nothing else
        int inverts = 0;
        for (const VideoFxEntry& e : Catalog::get().video())
            if (e.ok && e.filter && !e.transition && !e.generator && QString::fromStdString(e.label).contains("invert", Qt::CaseInsensitive)) ++inverts;
        check(inverts >= 1 && pm->count("Video FX") == inverts && pm->count("Transitions") == 0, "the search box narrows the list",
              strfmt("%d listed, %d named so", pm->count("Video FX"), inverts));
        pm->setFilter(QString());
        check(pm->count("Favorites") == 0, "no favourites at first");
        pm->setFavorite("video:reno.test.invert", true);
        check(pm->keys("Favorites") == QStringList() << "video:reno.test.invert" && QSettings().value("pluginManager/favorites").toStringList().size() == 1,
              "a favourite is listed under Favorites and remembered");
        // double-clicking an effect with a video event selected puts it on the event (as the Video FX window does)
        uint64_t vev = 0;
        for (const Track& t : st().project().tracks)
            if (t.id == v1 && !t.events.empty()) vev = t.events[0].id;
        st().selectOnly(vev);
        QTreeWidget* tree = static_cast<QTreeWidget*>(pm->tree());
        QTreeWidgetItem* item = nullptr;
        for (QTreeWidgetItem* it : tree->findItems("Test Invert", Qt::MatchRecursive | Qt::MatchContains))
            if (it->parent() && it->parent()->text(0) == "Reno Test") item = it;
        if (item) {
            tree->scrollToItem(item);
            uitest::settle(50);
            uitest::mouseDoubleClick(tree->viewport(), tree->visualItemRect(item).center());
        }
        uitest::settle(200);
        const Event* e = st().project().findEvent(vev);
        check(item && e && e->fx.size() == 1 && e->fx[0].pluginId == "reno.test.invert", "double-clicking an effect adds it to the selected event");
        check(item && (item->flags() & Qt::ItemIsDragEnabled), "and effects that work can be dragged onto the timeline");
        if (w.fxWindow()) w.fxWindow()->close();
        tools->pluginsDock->resize(700, 500);
        pm->resize(700, 480);
        uitest::settle(100);
        uitest::shot(pm, out + "/plugin_manager.png");
    }

    // ================================================================================== the Dashboard
    {
        buildProject();
        press("Ctrl+Alt+8");
        uitest::settle(200);
        Dashboard* db = tools->dashboard;
        check(tools->dashboardDock->isVisible(), "Ctrl+Alt+8 shows the Dashboard");
        st().setCursor(2.0);
        db->update1();
        check(db->tileText(Dashboard::Timecode).startsWith(QString::fromStdString(timecode(2.0, 30))), "its timecode tile shows the cursor",
              db->tileText(Dashboard::Timecode).toStdString());
        check(db->tileText(Dashboard::ProjectInfo).startsWith("1920x1080, 30 fps") && db->tileText(Dashboard::ProjectInfo).contains("1 video, 2 audio tracks; 3 events"),
              "the project tile", db->tileText(Dashboard::ProjectInfo).toStdString());
        check(db->tileText(Dashboard::Disk).contains("GB free"), "the disk tile", db->tileText(Dashboard::Disk).toStdString());
        check(db->tileText(Dashboard::Recording).startsWith("0 armed"), "the recording tile", db->tileText(Dashboard::Recording).toStdString());
        pb->play();
        // how many frames a second are shown depends on the machine: under sanitizers and load a 1080p frame can take most of a second, so this only asks for a measurement, given up to 4 s
        double fps = 0;
        for (int k = 0; k < 40 && fps <= 0; ++k) {
            uitest::settle(100);
            if (k >= 11) fps = db->measuredFps();
        }
        uitest::shot(db, out + "/dashboard_playing.png");
        pb->pause();
        check(fps > 0 && db->tileText(Dashboard::FrameRate).contains("fps"), "while playing it measures the frames shown a second", strfmt("%.1f fps", fps));
        db->setTileShown(Dashboard::Disk, false);
        check(!db->findChild<QWidget*>("dashboardTile5")->isVisible() && !QSettings().value("dashboard/tile5", true).toBool(), "tiles can be hidden (and it is remembered)");
        db->setTileShown(Dashboard::Disk, true);
        tools->dashboardDock->resize(420, 300);
        db->resize(420, 280);
        uitest::settle(150);
        uitest::shot(db, out + "/dashboard.png");
    }

    // ================================================================================== the Device Explorer
    {
        buildProject();
        // two pretend devices: a camera card (DCIM) and a plain drive, with real sound files on them
        const QString card = QDir(out).filePath("devices/CARD"), drive = QDir(out).filePath("devices/USB");
        QDir(out + "/devices").removeRecursively();
        QDir().mkpath(card + "/DCIM/100CANON");
        QDir().mkpath(drive + "/Music/old");
        auto wav = [](const QString& path, double freq, double seconds) {
            WavWriter ww;
            std::string err;
            ww.open(path.toStdString(), 48000, 2, 16, &err);
            std::vector<float> l(48000), r(48000);
            const int n = (int)(48000 * seconds);
            for (int k = 0; k < n; ++k) l[(size_t)(k % 48000)] = r[(size_t)(k % 48000)] = 0.3f * (float)std::sin(2.0 * 3.14159265 * freq * k / 48000.0);
            for (int k = 0; k < n; k += 48000) ww.write(l.data(), r.data(), std::min(48000, n - k));
            ww.finish();
        };
        wav(card + "/DCIM/100CANON/MVI_0001.WAV", 440, 1.0);
        wav(card + "/DCIM/100CANON/MVI_0002.WAV", 660, 0.5);
        QFile txt(card + "/DCIM/100CANON/notes.txt");
        txt.open(QIODevice::WriteOnly);
        txt.write("not media");
        txt.close();
        wav(drive + "/Music/old/song.wav", 330, 0.5);
        DeviceExplorer* de = tools->devices;
        de->setDeviceRoots(QStringList() << card << drive);
        press("Ctrl+Alt+6");
        uitest::settle(200);
        check(tools->devicesDock->isVisible(), "Ctrl+Alt+6 shows the Device Explorer");
        const std::vector<DeviceExplorer::Device> devs = de->devices();
        check(devs.size() == 2 && devs[0].kind == "Camera card" && devs[1].kind == "Removable drive", "it lists the devices, and what kind each is",
              devs.size() == 2 ? (devs[0].kind + " / " + devs[1].kind).toStdString() : std::string("?"));
        de->selectDevice(card);
        const QStringList cl = de->clips();
        check(cl.size() == 2 && cl[0].endsWith("MVI_0001.WAV") && cl[1].endsWith("MVI_0002.WAV"), "and a camera card's clips (only the media)", cl.join(",").toStdString());
        de->selectDevice(drive);
        check(de->clips().size() == 1 && de->clips()[0].endsWith("song.wav"), "a plain drive's media is found in its folders", de->clips().join(",").toStdString());
        de->selectDevice(card);
        const size_t mediaBefore = st().project().media.size();
        const QString into = QDir(out).filePath("imported");
        QDir(into).removeRecursively();
        std::vector<uint64_t> ids = de->importClips(QStringList(), into);
        check(ids.size() == 2 && st().project().media.size() == mediaBefore + 2 && QFileInfo::exists(into + "/MVI_0001.WAV") && QFileInfo::exists(into + "/MVI_0002.WAV"),
              "Import copies the clips to the folder and adds them to Project Media", strfmt("%d added", (int)ids.size()));
        const MediaItem* mi = ids.empty() ? nullptr : st().project().findMedia(ids[0]);
        check(mi && mi->info.hasAudio && std::fabs(mi->info.duration - 1.0) < 0.05 && QString::fromStdString(mi->path).startsWith(into), "the media is the copy",
              mi ? mi->path : std::string());
        ids = de->importClips(QStringList(), into);
        check(ids.size() == 2 && st().project().media.size() == mediaBefore + 2, "importing again copies and adds nothing new");
        tools->devicesDock->resize(700, 360);
        de->resize(700, 340);
        uitest::settle(100);
        uitest::shot(de, out + "/device_explorer.png");
    }

    // ================================================================================== external monitor
    {
        buildProject();
        st().setCursor(1.0);
        uitest::settle(300);
        press("Alt+Shift+4");
        uitest::settle(300);
        ExternalMonitor* em = tools->monitor;
        QScreen* sc = em->screen();
        check(em->isOpen() && (em->windowState() & Qt::WindowFullScreen) && sc && em->screenUsed() == sc->name(),
              "Alt+Shift+4 shows the preview full screen on a display", strfmt("%d %d %s %dx%d", (int)em->isOpen(), (int)em->windowState(),
                                                                                em->screenUsed().toStdString().c_str(), em->width(), em->height()));
        check(!em->shown().isNull() && em->shown().size() == pb->lastFrame().size(), "showing the preview's picture");
        uitest::shot(em, out + "/external_monitor.png");
        QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(em, &esc);
        uitest::settle(100);
        check(!em->isOpen(), "Esc closes it");
        press("Alt+Shift+4");
        uitest::settle(100);
        QKeyEvent again(QEvent::KeyPress, Qt::Key_4, Qt::AltModifier | Qt::ShiftModifier);
        QCoreApplication::sendEvent(em, &again);
        uitest::settle(100);
        check(!em->isOpen(), "and Alt+Shift+4 pressed on it closes it too (the main window's keys work there)");
    }

    // ================================================================================== color grading
    {
        buildProject();
        press("Alt+G");
        uitest::settle(150);
        ColorGradingPanel* cg = tools->grading;
        check(tools->gradingDock->isVisible(), "Alt+G shows the Color Grading panel");
        cg->setApplyTo(ColorGradingPanel::SelectedEvent);
        check(cg->target().kind == FxTarget::None, "with nothing selected it has nothing to grade");
        st().selectOnly(ev1);
        uitest::settle(50);
        check(cg->target().kind == FxTarget::Event && cg->target().id == ev1 && cg->slot() == 0, "it grades the selected video event (no effect on it yet)");
        const QImage before = pb->renderNow(1.0);
        cg->setValue("saturation", 0);
        const Event* e = st().project().findEvent(ev1);
        check(e && e->fx.size() == 1 && e->fx[0].kind == "builtin" && e->fx[0].pluginId == kColorGradeId && cg->slot() == e->fx[0].id,
              "a change puts the built-in Color Grading effect on the event");
        check(st().undoName() == "Color Grading", "as one undo step", st().undoName());
        const QImage grey = pb->renderNow(1.0);
        bool allGrey = true;
        for (int y = 0; y < grey.height(); y += 37)
            for (int x = 0; x < grey.width(); x += 41) {
                const QRgb c = grey.pixel(x, y);
                if (std::abs(qRed(c) - qGreen(c)) > 2 || std::abs(qGreen(c) - qBlue(c)) > 2) allGrey = false;
            }
        check(allGrey && before != grey, "saturation 0 makes the event grey");
        st().undo();
        e = st().project().findEvent(ev1);
        check(e && e->fx.empty() && cg->slot() == 0, "undo takes it off again");
        // a drag of the Gain wheel towards red is one step
        cg->resize(1100, 330);
        tools->gradingDock->resize(1120, 380);
        uitest::settle(100);
        QWidget* gain = cg->wheel(2);
        const QPoint c0(gain->property("wheelCx").toInt(), gain->property("wheelCy").toInt() - 9);
        uitest::mouseDrag(gain, c0, c0 + QPoint(0, -25), 6);
        uitest::settle(50);
        const ColorGrade g = ColorGrade::fromJson(cg->state());
        e = st().project().findEvent(ev1);
        const ColorGrade inProject = e && !e->fx.empty() ? ColorGrade::fromJson(e->fx[0].state) : ColorGrade();
        check(g.gain.y > 0.3 && std::fabs(g.gain.x) < 0.15 && inProject.gain.y == g.gain.y, "dragging the Gain wheel up pushes towards red",
              strfmt("%.2f,%.2f", g.gain.x, g.gain.y));
        check(st().undoName() == "Color Grading", "one undo step for the drag");
        const QImage warm = pb->renderNow(1.0);
        double dr = 0, db = 0;
        for (int y = 0; y < warm.height(); y += 23)
            for (int x = 0; x < warm.width(); x += 29) {
                dr += qRed(warm.pixel(x, y)) - qRed(before.pixel(x, y));
                db += qBlue(warm.pixel(x, y)) - qBlue(before.pixel(x, y));
            }
        check(dr > 0 && db < 0, "and the picture is redder", strfmt("%.0f %.0f", dr, db));
        // a click in the curves adds a point there
        QWidget* curves = cg->curveEditor();
        uitest::mouseClick(curves, QPoint(curves->width() / 2, curves->height() / 3));
        uitest::settle(50);
        const ColorGrade g2 = ColorGrade::fromJson(cg->state());
        check(g2.curve[0].size() == 3 && g2.curve[0][1].second > 0.55, "a click in the curves adds a point (raising the middle)",
              strfmt("%d points", (int)g2.curve[0].size()));
        cg->setValue("exposure", 0.5);
        uitest::settle(100);
        uitest::shot(cg, out + "/color_grading.png");
        // the effect shows in the Video FX window, with a way back to the panel
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = ev1;
        tools->gradingDock->hide();
        w.openFx(t, cg->slot());
        uitest::settle(300);
        QPushButton* openBtn = w.fxWindow()->findChild<QPushButton*>("fxOpenBuiltin");
        check(openBtn && openBtn->isVisible(), "the Video FX window shows the built-in effect, with a button for its panel");
        if (openBtn) openBtn->click();
        uitest::settle(100);
        check(tools->gradingDock->isVisible() && cg->target().id == ev1, "which opens the Color Grading panel on it");
        w.fxWindow()->close();
        // Reset
        cg->resetAll();
        check(ColorGrade::fromJson(cg->state()).isIdentity(), "Reset puts every control back");
        // VEGAS 18's layout: section 1 (Input LUT | Color Wheels | Color Channels | Input/Output), section 2 (Color Curves | HSL | Look LUT),
        // the Finishing column; Reno's RL Color Wheels and Utilities come after section 1's tabs
        {
            QTabWidget* s1 = cg->findChild<QTabWidget*>("gradeSection1");
            QTabWidget* s2 = cg->findChild<QTabWidget*>("gradeSection2");
            check(s1 && s1->count() == 6 && s1->tabText(0) == "Input LUT" && s1->tabText(1) == "Color Wheels" && s1->tabText(2) == "Color Channels" && s1->tabText(3) == "Input/Output" &&
                      s1->tabText(4) == "RL Color Wheels" && s1->tabText(5) == "Utilities",
                  "Color Grading section 1: Input LUT, Color Wheels, Color Channels, Input/Output (then RL Color Wheels, Utilities)");
            check(s2 && s2->count() == 3 && s2->tabText(0) == "Color Curves" && s2->tabText(1) == "HSL" && s2->tabText(2) == "Look LUT", "section 2: Color Curves, HSL, Look LUT");
            check(s1 && s1->currentIndex() == 1, "it opens on the Color Wheels");
            const char* fin[7] = {"gradeBypass", "gradeBezierMask", "gradeBroadcastColors", "gradeReset", "gradeExportLut", "gradePin", "gradeExit"};
            const char* finText[7] = {"Bypass Color Grading", "B\xc3\xa9zier Mask", "Broadcast Colors", "Reset All", "Export LUT", "Pin/Unpin", "Exit"};
            bool finOk = true;
            int lastY = -1;
            for (int k = 0; k < 7; ++k) {
                QPushButton* b = cg->findChild<QPushButton*>(fin[k]);
                if (!b || b->text() != QString::fromUtf8(finText[k]) || b->mapTo(cg, QPoint(0, 0)).y() <= lastY) finOk = false;
                else lastY = b->mapTo(cg, QPoint(0, 0)).y();
            }
            check(finOk, "the Finishing column: Bypass Color Grading, Bezier Mask, Broadcast Colors, Reset All, Export LUT, Pin/Unpin, Exit (in that order)");
            // the wheels' R, G, B boxes and the Color Channels sliders are the same numbers
            cg->setWheel("gain", 0.0, 0.5, 1.0);
            QDoubleSpinBox* gR = cg->wheel(2)->findChild<QDoubleSpinBox*>("wheelR");
            QDoubleSpinBox* gG = cg->wheel(2)->findChild<QDoubleSpinBox*>("wheelG");
            check(gR && gG && gR->value() > 1.4 && gG->value() < 1.0, "a Gain point pushed to red shows in the wheel's R box and not G", gR ? strfmt("%.2f %.2f", gR->value(), gG->value()) : "");
            QDoubleSpinBox* chR = cg->findChild<QDoubleSpinBox*>("gradeChannelBox_Gain_R");
            check(chR && std::fabs(chR->value() - gR->value()) < 1e-6, "and in the Color Channels page's Gain R box");
            if (chR) chR->setValue(1.0);
            uitest::settle(30);
            {
                const ColorGrade cgR = ColorGrade::fromJson(cg->state());
                check(std::fabs(cgR.gain.master - 1.0) < 0.2 && cgR.gain.y < 0.5 && st().undoName() == "Color Grading", "editing a channel box moves the wheel", strfmt("%.2f %.2f %.2f", cgR.gain.x, cgR.gain.y, cgR.gain.master));
            }
            if (QToolButton* wr = cg->wheel(2)->findChild<QToolButton*>("wheelReset")) wr->click();
            check(ColorGrade::fromJson(cg->state()).gain.master == 1.0 && ColorGrade::fromJson(cg->state()).gain.y == 0.0, "a wheel's reset arrow puts it back");
            // Input/Output: Levels and the Independent Controls are the grade's fields
            cg->setField("blackIn", 0.1);
            QDoubleSpinBox* inMin = cg->findChild<QDoubleSpinBox*>("gradeMoreBox_blackIn");
            check(inMin && std::fabs(inMin->value() - 0.1) < 1e-9, "Input/Output: Input Min shows the black point");
            cg->setValue("saturation", 0.5);
            QDoubleSpinBox* satBox = cg->findChild<QDoubleSpinBox*>("gradeMoreBox_saturation");
            check(satBox && std::fabs(satBox->value() - 0.5) < 1e-9, "Independent Controls: Saturation");
            if (s1) s1->setCurrentIndex(3);
            uitest::settle(100);
            uitest::shot(cg, out + "/color_grading_io.png");
            if (s1) s1->setCurrentIndex(2);
            uitest::settle(100);
            uitest::shot(cg, out + "/color_grading_channels.png");
            cg->resetAll();
            // LUT pages: Camera LUT (Input LUT) and Look LUT, as VEGAS's File / Interpolation / Strength / Enable
            QComboBox* lookBox = cg->findChild<QComboBox*>("gradeLookLut");
            check(lookBox && lookBox->count() >= 10 && lookBox->itemText(0) == "None", "Look LUT: the preset list", lookBox ? std::to_string(lookBox->count()) : "");
            cg->setLut(false, "preset:" + lutPresets().back().id, 0.5, true);
            ColorGrade lg = ColorGrade::fromJson(cg->state());
            check(lg.lookLut.source.rfind("preset:", 0) == 0 && lg.lookLut.strength == 0.5 && lg.lookLut.tetrahedral && st().undoName() == "Color Grading", "setLut puts a look LUT into the grade as one step");
            QDoubleSpinBox* strength = cg->findChild<QDoubleSpinBox*>("gradeLookLutStrength");
            check(strength && std::fabs(strength->value() - 0.5) < 0.005, "the Strength box shows it (0 to 1)", strength ? std::to_string(strength->value()) : "");
            QComboBox* interp = cg->findChild<QComboBox*>("gradeLookLutInterp");
            check(interp && interp->currentText() == "Tetrahedral", "Interpolation shows Tetrahedral");
            if (interp) interp->setCurrentIndex(0);
            check(!ColorGrade::fromJson(cg->state()).lookLut.tetrahedral, "Interpolation: Linear");
            if (strength) strength->setValue(0.8);
            uitest::settle(30);
            check(std::fabs(ColorGrade::fromJson(cg->state()).lookLut.strength - 0.8) < 1e-6, "the Strength box changes it");
            if (QAbstractButton* off = cg->findChild<QAbstractButton*>("gradeLookLutOn")) off->click();
            check(!ColorGrade::fromJson(cg->state()).lookLut.enabled, "Enable off");
            if (s2) s2->setCurrentIndex(2);
            uitest::settle(100);
            uitest::shot(cg, out + "/color_grading_look.png");
            if (QAbstractButton* rsb = cg->findChild<QAbstractButton*>("gradeLookLutReset")) rsb->click();
            check(ColorGrade::fromJson(cg->state()).lookLut == LutRef(), "the Look LUT's Reset puts it back");
            cg->setLut(true, "preset:" + lutPresets().front().id, 1.0, false);
            check(ColorGrade::fromJson(cg->state()).cameraLut.source.rfind("preset:", 0) == 0, "a camera LUT goes in the Input LUT page");
            cg->setLut(true, "", 1.0, false);
            cg->setLut(false, "", 1.0, false);
            cg->setField("blackIn", 0.1);
            cg->setField("hue", 0.25);
            cg->setHslCurve(ColorGrade::HueVsSat, {{0.0, 0.5}, {0.5, 0.0}, {1.0, 0.5}});
            lg = ColorGrade::fromJson(cg->state());
            check(lg.blackIn == 0.1 && lg.hue == 0.25 && lg.hslCurve[ColorGrade::HueVsSat].size() == 3, "setField / setHslCurve reach the grade");
            if (s2) s2->setCurrentIndex(1);
            uitest::settle(100);
            QSlider* hs = cg->findChild<QSlider*>("gradeMore_hue");
            check(hs && std::fabs(hs->value() - 625) <= 1, "HSL: the Hue slider shows 0.25", hs ? std::to_string(hs->value()) : "");
            QWidget* hc = cg->findChild<QWidget*>("gradeHslCurves");
            check(hc != nullptr, "HSL: the curve editor is there");
            if (hc) {
                uitest::mouseClick(hc, QPoint(hc->width() / 4, hc->height() / 3));
                uitest::settle(50);
                check(ColorGrade::fromJson(cg->state()).hslCurve[0].size() >= 3, "a click adds a point to the Hue vs Hue curve");
            }
            uitest::shot(cg, out + "/color_grading_hsl.png");
            if (s1) s1->setCurrentIndex(4);
            uitest::settle(100);
            uitest::shot(cg, out + "/color_grading_rl.png");
            if (s1) s1->setCurrentIndex(5);
            uitest::settle(100);
            uitest::shot(cg, out + "/color_grading_utilities.png");
            if (s1) s1->setCurrentIndex(0);
            uitest::settle(100);
            uitest::shot(cg, out + "/color_grading_luts.png");
            if (s1) s1->setCurrentIndex(1);
            if (s2) s2->setCurrentIndex(0);
            // Color Curves: the check boxes choose the channel; Reset
            if (QCheckBox* red = cg->findChild<QCheckBox*>("gradeCurveChannel1")) {
                red->click();
                check(!cg->findChild<QCheckBox*>("gradeCurveChannel0")->isChecked(), "Color Curves: one channel's check box at a time");
                uitest::mouseClick(cg->curveEditor(), QPoint(cg->curveEditor()->width() / 2, cg->curveEditor()->height() / 4));
                uitest::settle(30);
                check(ColorGrade::fromJson(cg->state()).curve[1].size() == 3 && ColorGrade::fromJson(cg->state()).curve[0].size() <= 2, "a click edits the Red curve");
                if (QAbstractButton* cr = cg->findChild<QAbstractButton*>("gradeCurveReset")) cr->click();
                check(ColorGrade::fromJson(cg->state()).curve[1].size() <= 2, "Reset puts the curve back");
                cg->findChild<QCheckBox*>("gradeCurveChannel0")->click();
            }
            uitest::shot(cg, out + "/color_grading.png");
            // pickers: a bright colour picked as the white point
            QImage reg(4, 4, QImage::Format_RGB32);
            reg.fill(QColor(200, 180, 160));
            cg->resetAll();
            cg->pickColour(2, reg);
            const ColorGrade pg = ColorGrade::fromJson(cg->state());
            check(!pg.isIdentity() && pg.gain.master > 1.0, "the White Point picker raises the gain", strfmt("%.2f", pg.gain.master));
            // and the button hooks the preview up
            bool hooked = false;
            cg->setPickerHook = [&](std::function<void(int, const QRectF&, const QImage&)> fn) { hooked = (bool)fn; };
            if (QAbstractButton* pb0 = cg->findChild<QAbstractButton*>("gradePick0")) {
                pb0->click();
                check(hooked, "the Black Point button asks the preview for the mouse");
                pb0->click();
                check(!hooked, "and gives it back when switched off");
            }
            cg->setPickerHook = nullptr;
            // Auto Color Balance: a picture with a colour cast becomes neutral
            cg->resetAll();
            cg->frameSource = [] { QImage im(8, 8, QImage::Format_RGB32); im.fill(QColor(200, 160, 120)); return im; };
            cg->autoAdjust(0);
            check(!ColorGrade::fromJson(cg->state()).isIdentity(), "Auto Color Balance changes the grade from the picture");
            cg->frameSource = nullptr;
            cg->resetAll();
            // Pin/Unpin: pinned, the panel stays on the event it was on
            st().selectOnly(ev1);
            uitest::settle(30);
            cg->setPinned(true);
            check(cg->pinned() && cg->findChild<QPushButton*>("gradePin")->isChecked(), "Pin/Unpin: pinned");
            const uint64_t evB = st().project().tracks[0].events[1].id;
            st().selectOnly(evB);
            uitest::settle(30);
            check(cg->target().id == ev1, "pinned, the panel stays on the first event when another is selected");
            cg->setPinned(false);
            check(!cg->pinned() && cg->target().id == evB, "unpinned, it follows the selection again");
            st().selectOnly(ev1);
            uitest::settle(30);
            cg->resetAll();
        }
        // the video output
        cg->setApplyTo(ColorGradingPanel::VideoOutput);
        cg->setValue("contrast", 1.5);
        check(st().project().videoOut.size() == 1 && isBuiltinVideo(st().project().videoOut[0]), "it can grade the video output");
        cg->setApplyTo(ColorGradingPanel::SelectedEvent);
        // Alt+D, C
        press("Alt+D, C");
        uitest::settle(300);
        auto vis = [&](const char* n) { QDockWidget* dk = w.findChild<QDockWidget*>(n); return dk && dk->isVisible(); };
        check(vis("dockPreview") && vis("dockScopes") && vis("dockColorGrading") && !vis("dockMedia") && !vis("dockMaster"),
              "Alt+D, C: the preview, the scopes and the Color Grading panel");
        check(w.currentLayout() == "Color Grading Layout", "named as VEGAS names it", w.currentLayout().toStdString());
        uitest::shot(&w, out + "/color_grading_layout.png");
        press("Alt+D, D");
        uitest::settle(200);
    }

    // ================================================================================== motion tracking
    {
        buildProject();
        // a clip with a square moving about in it
        const MediaPool::Opener before = st().mediaOpener;
        st().mediaOpener = [before](const std::string& path, std::string* err) -> std::unique_ptr<MediaReader> {
            if (path == "movingbox:") return std::unique_ptr<MediaReader>(new MovingBox());
            return before(path, err);
        };
        std::string err;
        const uint64_t mb = st().importMedia("movingbox:", &err);
        uint64_t mev = 0;
        st().edit("Moving box", [&](Project& p) {
            for (Track& t : p.tracks)
                if (t.id == v1) {
                    t.events.clear();
                    Event e;
                    e.id = p.newId();
                    e.mediaId = mb;
                    e.length = 10;
                    t.events.push_back(e);
                    mev = e.id;
                }
        });
        press("Alt+M");
        uitest::settle(150);
        MotionTrackingWindow* mt = tools->tracking;
        check(tools->trackingDock->isVisible(), "Alt+M shows Motion Tracking");
        st().selectOnly(mev);
        st().setCursor(0.0);
        uitest::settle(100);
        check(mt->trackedEvent() == mev, "it tracks in the selected video event");
        double x0, y0;
        boxAt(0, x0, y0);
        mt->setBox(x0, y0, 0.12 * 1.4, 0.2 * 1.4);  // round the square, with a little of what is round it
        bool lost = true;
        const int n = mt->track(1, nullptr, &lost);
        const std::vector<MotionTrackingWindow::Point> pts = mt->points();
        double worst = 0;
        for (const auto& p : pts) {
            double x, y;
            boxAt(p.time, x, y);
            if (x > 0.85) break;  // near the edge the box round it leaves the picture
            worst = std::max(worst, std::hypot((p.x - x) * 640, (p.y - y) * 360));
        }
        check(n >= 60 && worst < 2.0, "Track Forward follows the moving square (to within a pixel or two)",
              strfmt("%d frames, worst %.2f px%s", n, worst, lost ? ", lost" : ""));
        st().setCursor(1.0);
        uitest::settle(150);
        uitest::shot(mt, out + "/motion_tracking.png");
        // into Track Motion of the (new) top track: the track moves as the square did
        const uint64_t top = st().addTrack(TrackType::Video, 0);
        check(mt->applyToTrackMotion(top), "Apply to Track Motion");
        const Track* tt = nullptr;
        for (const Track& x : st().project().tracks)
            if (x.id == top) tt = &x;
        double x30, y30;
        boxAt(1.0, x30, y30);
        bool found = false;
        if (tt)
            for (const MotionKey& k : tt->motion)
                if (std::fabs(k.time - 1.0) < 1e-6) found = std::fabs(k.x - 0.5 - (x30 - x0)) < 0.005 && std::fabs(k.y - 0.5 - (y30 - y0)) < 0.008 && k.w == 1.0;
        check(tt && tt->motion.size() == pts.size() && found, "the track's keyframes move it as the square moved (size kept)",
              strfmt("%d keys", tt ? (int)tt->motion.size() : 0));
        check(st().undoName() == "Motion Tracking", "as one undo step", st().undoName());
        // stabilize: the event's Pan/Crop moves with the square, so in the picture it stays where it was
        check(mt->applyToPanCrop(true, 0.8), "Apply as stabilizing Pan/Crop");
        const Event* e = st().project().findEvent(mev);
        check(e && e->panCrop.size() == pts.size(), "the event gets a Pan/Crop keyframe a frame");
        // the picture: the square's place in frames 0 and 30 of the render, found by tracking the render itself
        {
            MediaPool pool;
            pool.setOpener(st().mediaOpener);
            const QImage r0 = pb->renderNow(0.0), r30 = pb->renderNow(1.0);
            auto gray = [](const QImage& q) {
                GrayImage g;
                const QImage s = q.scaled(640, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
                g.w = s.width();
                g.h = s.height();
                g.px.resize((size_t)g.w * g.h);
                for (int y = 0; y < g.h; ++y)
                    for (int x = 0; x < g.w; ++x) g.px[(size_t)y * g.w + x] = qGray(s.pixel(x, y)) / 255.f;
                return g;
            };
            const GrayImage g0 = gray(r0), g30 = gray(r30);
            // where the square is in the render at frame 0: the media's (0.2, 0.5) seen through the 0.8 box about the middle
            const double sx0 = 0.5 + (x0 - 0.5) / 0.8, sy0 = 0.5 + (y0 - 0.5) / 0.8;
            BoxTracker bt;
            double cx = sx0 * 640, cy = sy0 * 360;
            bt.searchRadius = 120;
            const bool started = bt.start(g0, cx, cy, 0.12 / 0.8 * 640 * 1.2, 0.2 / 0.8 * 360 * 1.2);
            double sc = 0;
            const bool ok = started && bt.step(g30, cx, cy, &sc);
            (void)sc;
            check(ok && std::hypot(cx - sx0 * 640, cy - sy0 * 360) < 3.0, "stabilized: in the picture the square stays where it was",
                  strfmt("%.1f,%.1f vs %.1f,%.1f", cx, cy, sx0 * 640, sy0 * 360));
        }
        st().undo();
        st().undo();
        mt->clearPoints();
        check(mt->points().empty(), "Clear forgets the track");
        st().mediaOpener = before;
    }

    // ================================================================================== text to speech
    {
        buildProject();
        press("Ctrl+Shift+P");
        uitest::settle(100);
        TextToSpeechWindow* tts = tools->speech;
        check(tools->speechDock->isVisible(), "Ctrl+Shift+P shows Text to Speech");
        tts->setText("Hello there, world");
#ifndef _WIN32
        const std::vector<uint64_t> none = tts->create();
        check(none.empty(), "without Windows' speech nothing is made (and the window says why)");
#endif
        // a stand-in for Windows' speech: a second of tone, saying it was asked for the right things
        std::string asked;
        tts->synthesizer = [&asked](const std::string& text, const std::string& voice, int rate, int volume, const std::string& path, std::string*) {
            asked = strfmt("%s|%s|%d|%d", text.c_str(), voice.c_str(), rate, volume);
            WavWriter ww;
            std::string e;
            if (!ww.open(path, 48000, 1, 16, &e)) return false;
            std::vector<float> l(48000);
            for (int k = 0; k < 48000; ++k) l[(size_t)k] = 0.4f * (float)std::sin(2.0 * 3.14159265 * 300.0 * k / 48000.0);
            ww.write(l.data(), nullptr, 48000);
            return ww.finish();
        };
        tts->setRate(3);
        tts->setVolume(80);
        st().currentTrack = a1;
        st().setCursor(3.0);
        const std::vector<uint64_t> made = tts->create();
        const Track* tr = nullptr;
        const Event* e = made.empty() ? nullptr : st().project().findEvent(made[0], &tr);
        check(asked == "Hello there, world||3|80", "Create asks for the text with the voice, speed and volume chosen", asked);
        check(e && tr && tr->id == a1 && std::fabs(e->start - 3.0) < 1e-6 && std::fabs(e->length - 1.0) < 0.01, "and puts the speech on the selected audio track at the cursor",
              e ? strfmt("%.3f + %.3f", e->start, e->length) : std::string("none"));
        check(QFileInfo(QString::fromStdString(tts->lastFile())).fileName() == "Speech 1 - Hello there, world.wav" && QFileInfo::exists(QString::fromStdString(tts->lastFile())),
              "the file is named after what it says", tts->lastFile());
        tools->speechDock->resize(500, 300);
        tts->resize(500, 280);
        uitest::settle(50);
        uitest::shot(tts, out + "/text_to_speech.png");
    }

    // ================================================================================== MIDI timecode and clock
    {
        buildProject();
        MidiSync* midi = tools->midi.get();
        std::shared_ptr<std::vector<FakeMidi::Sent>> log;
        midi->outputFactory = [&log] {
            FakeMidi* m = new FakeMidi();
            log = m->log;
            return std::unique_ptr<MidiPort>(m);
        };
        press("F7");
        check(midi->generatingTimecode() && w.findChild<QAction*>("generateMtc")->isChecked(), "F7 generates MIDI timecode");
        st().setCursor(2.0);
        pb->play();
        uitest::settle(1000);
        const double heard = pb->position();
        pb->pause();
        uitest::settle(50);
        int quarters = 0;
        std::vector<int> pieces(8, -1);
        int lastSetH = -1, lastSetM = -1, lastSetS = -1, lastSetF = -1;
        bool fullAtStart = false;
        if (log && !log->empty()) {
            const std::vector<uint8_t>& f = log->front().msg;
            // sent at the first tick that sees playback going, with the position then: a frame or two after 2 s on a slow machine (under sanitizers, say)
            fullAtStart = f.size() == 10 && f[0] == 0xF0 && f[5] == (3 << 5) && f[6] == 0 && f[7] == 2 && f[8] <= 15;
            int next = 0;
            for (const auto& sm : *log) {
                if (sm.msg.size() != 2 || sm.msg[0] != 0xF1) continue;
                ++quarters;
                const int piece = sm.msg[1] >> 4;
                if (piece != next) { next = 0; continue; }
                pieces[(size_t)piece] = sm.msg[1] & 15;
                next = (piece + 1) & 7;
                if (piece == 7) {
                    lastSetF = pieces[0] | (pieces[1] << 4);
                    lastSetS = pieces[2] | (pieces[3] << 4);
                    lastSetM = pieces[4] | (pieces[5] << 4);
                    lastSetH = pieces[6] | ((pieces[7] & 1) << 4);
                }
            }
        }
        check(fullAtStart, "playing starts with a full-frame message of where it starts (00:00:02:00, or a few frames on)");
        const double setTime = MidiSync::secondsOf(lastSetH, lastSetM, lastSetS, lastSetF, 3) + 2.0 / 30;
        check(quarters > 100 && quarters < 135 && std::fabs(setTime - heard) < 0.15, "then quarter frames, 4 a frame, carrying the time played",
              strfmt("%d quarter frames; last set says %.3f, played to %.3f", quarters, setTime, heard));
        press("F7");
        check(!midi->generatingTimecode(), "F7 again stops");
        // the clock
        press("Shift+F7");
        check(midi->generatingClock(), "Shift+F7 generates MIDI clock");
        log->clear();
        st().setCursor(0.0);
        pb->play();
        uitest::settle(1000);
        pb->pause();
        uitest::settle(50);
        int clocks = 0;
        bool started = log->size() >= 2 && (*log)[0].msg == std::vector<uint8_t>{0xF2, 0, 0} && (*log)[1].msg == std::vector<uint8_t>{0xFA};
        for (const auto& sm : *log)
            if (sm.msg == std::vector<uint8_t>{0xF8}) ++clocks;
        const bool stopped = !log->empty() && log->back().msg == std::vector<uint8_t>{0xFC};
        check(started && stopped, "from the start: Song Position 0 and Start; Stop when playback stops");
        check(clocks >= 40 && clocks <= 56, "24 clocks a beat at 120 beats a minute (48 in a second)", strfmt("%d", clocks));
        press("Shift+F7");
        // following MIDI timecode coming in
        FakeMidi* in = nullptr;
        midi->inputFactory = [&in] {
            in = new FakeMidi();
            return std::unique_ptr<MidiPort>(in);
        };
        press("Ctrl+F7");
        check(midi->chasing() && in, "Ctrl+F7 follows MIDI timecode");
        st().setCursor(0.0);
        // a sender playing from 00:00:05:00: quarter frames at 120 a second, for 0.8 s
        QElapsedTimer sendClock;
        sendClock.start();
        int64_t sentQ = -1;
        double cursorWhilePlaying = -1;
        bool playedWhileSent = false;
        while (sendClock.elapsed() < 800) {
            const int64_t q = sendClock.elapsed() * 120 / 1000;
            for (int64_t k = sentQ + 1; k <= q; ++k) {
                const int64_t start = k - k % 8;
                int h, m, sec, f;
                MidiSync::timecodeOf(5.0 + start / 120.0 + 1e-9, 3, h, m, sec, f);
                if (in && in->onReceive) in->onReceive(MidiSync::quarterFrame((int)(k % 8), h, m, sec, f, 3));
            }
            sentQ = q;
            uitest::settle(5);
            if (pb->playing()) {
                playedWhileSent = true;
                cursorWhilePlaying = st().cursor();
            }
        }
        const double sentTo = 5.0 + sendClock.elapsed() / 1000.0;
        check(playedWhileSent && cursorWhilePlaying > 5.0 && std::fabs(cursorWhilePlaying - sentTo) < 0.25, "it plays from where the timecode says, and keeps to it",
              strfmt("cursor %.3f, timecode at %.3f", cursorWhilePlaying, sentTo));
        uitest::settle(500);
        check(!pb->playing(), "and stops when the timecode stops");
        press("Ctrl+F7");
        check(!midi->chasing(), "Ctrl+F7 again stops following");
    }

    uitest::shot(&w, out + "/main_window.png");
    return uitest::summary();
}
