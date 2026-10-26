#include "uitestlib.h"
#include "../../src/ofx/probe.h"
#include <QApplication>
#include <QSettings>
#include <QElapsedTimer>
#include <QFont>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>
#include <cmath>
#include <cstdio>

using namespace reno;
using namespace reno::app;

namespace uitest {

namespace {
const double kPi = 3.14159265358979323846;

class Synth : public MediaReader {
public:
    MediaInfo mi;
    int tag = 1;
    const MediaInfo& info() const override { return mi; }
    ImagePtr videoFrame(double seconds, int maxW, int maxH) override
    {
        if (!mi.hasVideo) return nullptr;
        double s = std::min((double)maxW / mi.width, (double)maxH / mi.height);
        const int w = std::max(1, (int)(mi.width * s + 0.5)), h = std::max(1, (int)(mi.height * s + 0.5));
        const double frame = mi.isStill ? tag * 37.0 : std::floor(seconds * 30.0 + 1e-6);
        ImagePtr img = ofx::makeTestPattern(w, h, frame + tag * 20.0);
        return img;
    }
    bool audio(int64_t start, int frames, int sampleRate, float* left, float* right) override
    {
        if (!mi.hasAudio) return false;
        const int64_t end = (int64_t)std::llround(mi.duration * sampleRate);
        for (int k = 0; k < frames; ++k) {
            const int64_t n = start + k;
            // a tone that swells and fades twice a second, so the waveform has a shape to draw
            const double env = 0.35 + 0.3 * std::sin(2.0 * kPi * 2.0 * (double)n / sampleRate);
            const float v = (n < 0 || n >= end) ? 0.f : (float)(env * std::sin(2.0 * kPi * 220.0 * tag * (double)n / sampleRate));
            left[k] = v;
            right[k] = v;
        }
        return true;
    }
};

std::unique_ptr<MediaReader> open(const std::string& path, std::string* error)
{
    const bool clip = path.compare(0, 6, "synth:") == 0, pic = path.compare(0, 9, "synthpic:") == 0, snd = path.compare(0, 11, "synthaudio:") == 0;
    if (!clip && !pic && !snd) return openMediaOrProject(path, error);
    std::unique_ptr<Synth> r(new Synth());
    r->tag = std::atoi(path.c_str() + path.find(':') + 1);
    r->mi.path = path;
    r->mi.hasVideo = clip || pic;
    r->mi.hasAudio = clip || snd;
    r->mi.isStill = pic;
    r->mi.width = 320;
    r->mi.height = 180;
    r->mi.fps = pic ? 0 : 30;
    r->mi.duration = pic ? 0 : 10;
    r->mi.sampleRate = 48000;
    r->mi.channels = 2;
    return std::unique_ptr<MediaReader>(r.release());
}

int gPass = 0, gFail = 0;
}  // namespace

void setup(const std::vector<std::string>& ofxFiles, const std::vector<std::string>& vstFiles)
{
    AppState::get().mediaOpener = open;
    // Preferences > General's "Prompt to adjust project settings to match first media": a modal question would stop every test
    QSettings().setValue("prefs/promptMatchMedia", false);
    QApplication::setFont(QFont("DejaVu Sans", 9));  // a plain face; the font folder given to the tests has several
    // the catalog wants folders for OpenFX; a plain .ofx file's folder does
    std::vector<std::string> dirs;
    for (const auto& f : ofxFiles) dirs.push_back(pathDir(f));
    Catalog::get().scanInProcess(dirs, vstFiles);
}

void buildDemoProject()
{
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t a = app.importMedia("synth:1", &err), b = app.importMedia("synth:2", &err);
    const uint64_t pic = app.importMedia("synthpic:3", &err), snd = app.importMedia("synthaudio:2", &err);
    app.addMediaToTimeline(a, 0.0);                     // picture + sound, grouped
    app.addMediaToTimeline(b, 8.0);                     // overlaps the first by two seconds: a cross-fade
    const std::vector<uint64_t> p = app.addMediaToTimeline(pic, 3.0);
    app.addMediaToTimeline(snd, 12.0);
    app.edit("Demo Edits", [&](Project& pr) {
        if (!p.empty())
            if (Event* e = pr.findEvent(p[0])) { e->fadeIn = 1.0; e->fadeOut = 1.5; e->gain = 0.8; }
        for (auto& t : pr.tracks) {
            if (t.name.empty()) t.name = t.type == TrackType::Video ? "Video" : "Audio";
        }
    });
    // an effect or two on the first video event, if any effects are known
    const std::vector<VideoFxEntry> fx = Catalog::get().video();
    const Project& pr = app.project();
    uint64_t firstVideoEvent = 0;
    for (const auto& t : pr.tracks)
        if (t.type == TrackType::Video && !t.events.empty() && !firstVideoEvent) firstVideoEvent = t.events[0].id;
    for (const auto& t : pr.tracks)
        for (const auto& e : t.events)
            if (e.mediaId == a && t.type == TrackType::Video) firstVideoEvent = e.id;
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = firstVideoEvent;
    int added = 0;
    for (const auto& e : fx) {
        if (!e.ok || !e.filter || added >= 2) continue;
        if (e.id == "reno.test.params" || e.id == "reno.test.invert" || e.id.compare(0, 4, "com.") == 0) {
            app.addVideoFx(target, e.id, e.label);
            ++added;
        }
    }
    app.setCursor(1.0);
}

void settle(int ms)
{
    QElapsedTimer t;
    t.start();
    do {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents();
    } while (t.elapsed() < ms);
}

QImage shot(QWidget* w, const QString& path)
{
    QImage img = w->grab().toImage();
    if (!path.isEmpty()) img.save(path);
    return img;
}

static void send(QWidget* w, QEvent& e)
{
    QApplication::sendEvent(w, &e);
    QCoreApplication::processEvents();
}

void mousePress(QWidget* w, QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    QMouseEvent e(QEvent::MouseButtonPress, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, button, mods);
    send(w, e);
}

void mouseMove(QWidget* w, QPoint pos, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
{
    QMouseEvent e(QEvent::MouseMove, QPointF(pos), QPointF(w->mapToGlobal(pos)), Qt::NoButton, buttons, mods);
    send(w, e);
}

void mouseRelease(QWidget* w, QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    QMouseEvent e(QEvent::MouseButtonRelease, QPointF(pos), QPointF(w->mapToGlobal(pos)), button, Qt::NoButton, mods);
    send(w, e);
}

void mouseClick(QWidget* w, QPoint pos, Qt::MouseButton button, Qt::KeyboardModifiers mods)
{
    mousePress(w, pos, button, mods);
    mouseRelease(w, pos, button, mods);
}

void mouseDoubleClick(QWidget* w, QPoint pos)
{
    mouseClick(w, pos);
    QMouseEvent e(QEvent::MouseButtonDblClick, QPointF(pos), QPointF(w->mapToGlobal(pos)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    send(w, e);
    mouseRelease(w, pos);
}

void mouseDrag(QWidget* w, QPoint from, QPoint to, int steps, Qt::KeyboardModifiers mods)
{
    mousePress(w, from, Qt::LeftButton, mods);
    for (int k = 1; k <= steps; ++k) {
        const QPoint p(from.x() + (to.x() - from.x()) * k / steps, from.y() + (to.y() - from.y()) * k / steps);
        mouseMove(w, p, Qt::LeftButton, mods);
    }
    mouseRelease(w, to, Qt::LeftButton, mods);
}

void wheel(QWidget* w, QPoint pos, int delta, Qt::KeyboardModifiers mods)
{
    QWheelEvent e(QPointF(pos), QPointF(w->mapToGlobal(pos)), QPoint(0, 0), QPoint(0, delta), Qt::NoButton, mods, Qt::NoScrollPhase, false);
    send(w, e);
}

void key(QWidget* w, int k, Qt::KeyboardModifiers mods, const QString& text)
{
    QKeyEvent press(QEvent::KeyPress, k, mods, text);
    send(w, press);
    QKeyEvent release(QEvent::KeyRelease, k, mods, text);
    send(w, release);
}

void check(bool ok, const std::string& name, const std::string& detail)
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

int summary()
{
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}

}  // namespace uitest
