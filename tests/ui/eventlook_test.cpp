// How an audio event is drawn, against VEGAS Pro 18's timeline (Reverse Event Background Color on): a stereo file one
// waveform row per channel split by a grey line (Swap: swapped; Combine: one row), the gain handle in a notch in the
// middle of the top edge, the snap offset flag at the bottom left of every audio event, the top corners cut off, a
// selected event's dark background, light waveform and yellow rim, and the time selection's blue tint over the events.
//
//   eventlook_test <output folder>
//
// QT_QPA_PLATFORM=offscreen. Saves eventlook.png.
#include "../../src/app/timeline.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/vprefs.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSettings>
#include <cmath>
#include <cstring>

using namespace reno;
using namespace reno::app;
using uitest::check;

static AppState& st() { return AppState::get(); }

// four seconds at 8 kHz: a loud tone on the left, silence on the right
static bool writeStereoWav(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    const int rate = 8000, n = rate * 4;
    auto u32 = [&](uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4); u32(36 + n * 4); f.write("WAVE", 4);
    f.write("fmt ", 4); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(n * 4);
    QByteArray d(n * 4, 0);
    for (int k = 0; k < n; ++k) {
        const int16_t l = (int16_t)std::lround(26000.0 * std::sin(k * 0.05));
        std::memcpy(d.data() + k * 4, &l, 2);
    }
    f.write(d);
    return true;
}

static bool near(const QColor& a, const QColor& b, int tol) { return std::abs(a.red() - b.red()) <= tol && std::abs(a.green() - b.green()) <= tol && std::abs(a.blue() - b.blue()) <= tol; }

// how many pixels of a row segment differ from the background colour 'bg'
static int inked(const QImage& im, int y, int x0, int x1, const QColor& bg)
{
    int n = 0;
    for (int x = x0; x <= x1; ++x)
        if (!near(im.pixelColor(x, y), bg, 12)) ++n;
    return n;
}

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEventLookTest");
    QApplication::setApplicationName("eventlook_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("eventlook_out");
    QDir().mkpath(out);
    uitest::setup({}, {});
    vprefs::set("prefs/reverseEventBg", true);

    const QString wav = QDir(out).absoluteFilePath("stereo.wav");
    check(writeStereoWav(wav), "a stereo WAV is written");
    st().newProject();
    std::string err;
    const uint64_t media = st().importMedia(wav.toStdString(), &err);
    check(media != 0, "the WAV is imported", err);
    uint64_t ev = 0;
    st().edit("Build", [&](Project& p) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        t.name = "Audio";
        t.height = 90;
        Event e;
        e.id = p.newId();
        e.mediaId = media;
        e.start = 0.5;
        e.length = 3.0;
        e.name = "tone";
        ev = e.id;
        t.events.push_back(e);
        p.tracks.push_back(t);
    });
    st().setSelection(std::set<uint64_t>());
    st().loopStart = st().loopEnd = 0.0;

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1200, 300);
    w.show();
    w.zoomToFit();
    QImage im;
    QRect r;
    auto shot = [&]() {
        for (int k = 0; k < 60; ++k) {
            uitest::settle(80);
            r = w.eventRectOf(ev);
            im = w.grab().toImage();
            // until the waveform has been read
            if (r.width() > 40 && inked(im, r.top() + r.height() / 4, r.left() + 20, r.right() - 20, QColor(216, 216, 216)) > 20) break;
        }
    };
    shot();
    im.save(QDir(out).absoluteFilePath("eventlook.png"));
    check(r.width() > 200 && r.height() > 40, "the event is drawn", std::to_string(r.width()) + "x" + std::to_string(r.height()));
    const int x0 = r.left() + 20, x1 = r.right() - 20;
    const QColor grey(216, 216, 216);

    // two rows: the left channel (the tone) above, the right (silent) below, a grey line between
    int sep = -1;
    for (int y = r.top() + 8; y < r.bottom() - 8; ++y)
        if (inked(im, y, x0, x1, QColor(80, 80, 80)) < (x1 - x0) / 20) {
            sep = y;
            break;
        }
    check(sep > r.top() + r.height() / 3 && sep < r.bottom() - r.height() / 3, "stereo: a grey line splits the event into two rows", std::to_string(sep));
    const int upper = inked(im, r.top() + (sep - r.top()) / 2 - 6, x0, x1, grey), lower = inked(im, sep + (r.bottom() - sep) / 2 - 6, x0, x1, grey);
    check(upper > (x1 - x0) / 2 && lower < (x1 - x0) / 10, "stereo: the left channel's tone in the top row, the silent right one below",
          std::to_string(upper) + " / " + std::to_string(lower));

    // Swap: the rows change places
    st().edit("Swap", [&](Project& p) { p.findEvent(ev)->channels = "swap"; });
    shot();
    {
        const int u = inked(im, r.top() + (sep - r.top()) / 2 - 6, x0, x1, grey), l = inked(im, sep + (r.bottom() - sep) / 2 - 6, x0, x1, grey);
        check(u < (x1 - x0) / 10 && l > (x1 - x0) / 2, "Swap: the tone in the bottom row", std::to_string(u) + " / " + std::to_string(l));
    }
    // Combine (mono): one row, no line
    st().edit("Mono", [&](Project& p) { p.findEvent(ev)->channels = "combine"; });
    shot();
    check(inked(im, sep, x0, x1, QColor(80, 80, 80)) > (x1 - x0) / 2, "Combine: one row, no grey line");
    st().edit("Both", [&](Project& p) { p.findEvent(ev)->channels = "both"; });
    shot();

    // the gain handle: a blue tab in a notch in the middle of the top edge
    const int cx = (r.left() + r.right() + 1) / 2;
    int blue = 0;
    for (int x = cx - 8; x <= cx + 8; ++x)
        for (int y = r.top(); y <= r.top() + 2; ++y)
            if (near(im.pixelColor(x, y), QColor(0, 0, 255), 30)) ++blue;
    check(blue >= 12, "the gain handle: blue in a notch at the top middle, on an unselected event", std::to_string(blue));
    check(near(im.pixelColor(cx, r.top() + 2), QColor(45, 45, 45), 6), "the notch shows the lane under the handle");
    // the top corners are cut off; the snap offset flag sits at the bottom left
    check(near(im.pixelColor(r.left() + 5, r.top()), QColor(45, 45, 45), 6) && near(im.pixelColor(r.left() + 2, r.top() + 4), QColor(64, 64, 64), 4) &&
              !near(im.pixelColor(r.left() + 7, r.top() + 8), QColor(45, 45, 45), 6),
          "the top-left corner is cut off at 45 degrees, a dark edge along the cut");
    check(near(im.pixelColor(r.left(), r.top()), QColor(0, 0, 255), 4), "the fade-in handle: pure blue in the cut corner");
    check(near(im.pixelColor(r.left() + 2, r.bottom() - 2), QColor(255, 255, 255), 8) && near(im.pixelColor(r.left(), r.bottom() - 2), QColor(0, 0, 0), 8),
          "the snap offset flag: white with a black edge at the bottom left of an unselected event");

    // selected: a dark background, a light waveform and a yellow rim
    st().setSelection({ev});
    shot();
    {
        const QColor body = im.pixelColor(r.left() + 30, sep + 4);
        check(body.lightness() < 130, "selected: the background is dark", body.name().toStdString());
        check(near(im.pixelColor(r.left() + 40, r.top()), QColor(204, 228, 4), 10), "selected: a yellow rim", im.pixelColor(r.left() + 40, r.top()).name().toStdString());
    }
    st().setSelection(std::set<uint64_t>());

    // the time selection tints the events blue
    st().loopStart = 1.0;
    st().loopEnd = 2.0;
    shot();
    {
        const int xin = w.eventRectOf(ev).left() + (int)std::lround((1.5 - 0.5) / 3.0 * r.width());
        const QColor in = im.pixelColor(xin, sep + 4), outside = im.pixelColor(r.right() - 30, sep + 4);
        check(in.blue() > in.red() + 40 && in.lightness() < 170, "time selection: the event is blue inside it", in.name().toStdString());
        check(outside == grey, "time selection: the event stays grey outside it", outside.name().toStdString());
    }
    return uitest::summary();
}
