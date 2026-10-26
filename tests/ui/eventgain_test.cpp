// Event gain and opacity on the timeline (src/app/timeline.cpp): the line along the top of an event is dragged down. Audio gain
// runs 0 dB (top) to -Inf dB (bottom) and shows in dB while dragged; video opacity 100 % to 0. Ctrl (or right button) is finer,
// all selected events of the same kind change together, Escape cancels, one undo step ("Event Gain" / "Event Opacity"). The fade
// handles in the top corners keep working, and fades rise to the line.
//
//   eventgain_test <output folder>
//
// Build: uibuild eventgain_test tests/ui/eventgain_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

static const Event* ev(uint64_t id) { return AppState::get().project().findEvent(id); }

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoEventGainTest");
    QApplication::setApplicationName("eventgain_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("eventgainout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();
    app.newProject();
    uitest::buildDemoProject();

    Playback playback;
    TimelineWidget w(&playback);
    w.resize(1400, 560);
    w.show();
    w.zoomToFit();
    uitest::settle(300);

    // events: sound at 12 s alone on its track, picture at 3 s (opacity 0.8, with fades), grouped sound and picture at 8 s
    uint64_t sound = 0, pic = 0, aud8 = 0, vid8 = 0;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events) {
            const bool v = t.type == TrackType::Video;
            if (!v && std::fabs(e.start - 12.0) < 0.01) sound = e.id;
            if (v && std::fabs(e.start - 3.0) < 0.01) pic = e.id;
            if (!v && std::fabs(e.start - 8.0) < 0.01) aud8 = e.id;
            if (v && std::fabs(e.start - 8.0) < 0.01) vid8 = e.id;
        }
    if (!sound || !pic || !aud8 || !vid8) {
        check(false, "the demo project has the events the test needs", strfmt("%llu %llu %llu %llu", (unsigned long long)sound, (unsigned long long)pic,
                                                                              (unsigned long long)aud8, (unsigned long long)vid8));
        return uitest::summary();
    }
    auto rect = [&](uint64_t id) { return w.eventRectOf(id); };
    auto topPoint = [&](uint64_t id, double frac = 0.5) {
        const QRect r = rect(id);
        return QPoint(r.left() + (int)(r.width() * frac), r.top() + 2);
    };
    auto near = [](double a, double b, double tol = 0.006) { return std::fabs(a - b) <= tol; };
    check(near(ev(sound)->gain, 1.0) && near(ev(pic)->gain, 0.8), "the demo events start at gain 1 (sound) and opacity 0.8 (picture)");

    // ---------------------------------------------------------------- the pointer over the top of an event
    {
        uitest::mouseMove(&w, topPoint(sound), Qt::NoButton);
        const bool line = w.cursor().shape() == Qt::BitmapCursor;
        const QRect r = rect(sound);
        uitest::mouseMove(&w, QPoint(r.center().x(), r.center().y() + 4), Qt::NoButton);
        // the middle of the body is the event itself: plain arrow, not the gain pointer
        const bool body = w.cursor().shape() != Qt::BitmapCursor;
        check(line && body, "the gain pointer over the top edge of an event, not over its middle");
    }

    // ---------------------------------------------------------------- an audio event's gain dragged down
    const int hSound = std::max(20, rect(sound).height());
    {
        const size_t undo0 = app.undoCount();
        const QPoint from = topPoint(sound);
        uitest::mousePress(&w, from);
        uitest::mouseMove(&w, from + QPoint(0, 10));
        uitest::mouseMove(&w, from + QPoint(0, 20));
        uitest::mouseMove(&w, from + QPoint(0, 30));
        uitest::settle(30);
        uitest::shot(&w, out + "/gain_audio_drag.png");
        const double during = ev(sound)->gain;
        uitest::mouseRelease(&w, from + QPoint(0, 30));
        uitest::settle(50);
        const double want = 1.0 - 30.0 / hSound;
        check(near(ev(sound)->gain, want) && near(during, want), "dragging the top of an audio event down 30 pixels lowers its gain by that part of its height",
              strfmt("gain %.4f, wanted %.4f (event %d high)", ev(sound)->gain, want, hSound));
        check(app.undoCount() == undo0 + 1 && app.undoName() == "Event Gain", "one undo step, \"Event Gain\"", app.undoName());
        check(near(ev(sound)->start, 12.0, 1e-6), "the event did not move");
        uitest::shot(&w, out + "/gain_audio_lowered.png");
        app.undo();
        uitest::settle(30);
        check(near(ev(sound)->gain, 1.0, 1e-9), "undo puts the gain back");
    }

    // ---------------------------------------------------------------- Ctrl, and the right mouse button: fine steps
    {
        const QPoint from = topPoint(sound);
        uitest::mouseDrag(&w, from, from + QPoint(0, 30), 8, Qt::ControlModifier);
        uitest::settle(30);
        const double want = 1.0 - 30.0 / hSound * 0.125;
        check(near(ev(sound)->gain, want, 0.002), "with Ctrl the same drag changes the gain an eighth as much", strfmt("gain %.4f, wanted %.4f", ev(sound)->gain, want));
        app.undo();
        uitest::mousePress(&w, from, Qt::RightButton);
        for (int k = 1; k <= 3; ++k) uitest::mouseMove(&w, from + QPoint(0, 10 * k), Qt::RightButton);
        uitest::mouseRelease(&w, from + QPoint(0, 30), Qt::RightButton);
        uitest::settle(30);
        check(near(ev(sound)->gain, want, 0.002) && app.undoName() == "Event Gain", "with the right mouse button too (and no menu)", strfmt("gain %.4f", ev(sound)->gain));
        app.undo();
        uitest::settle(30);
    }

    // ---------------------------------------------------------------- Escape; the ends of the range
    {
        const size_t undo0 = app.undoCount();
        const QPoint from = topPoint(sound);
        uitest::mousePress(&w, from);
        uitest::mouseMove(&w, from + QPoint(0, 15));
        uitest::mouseMove(&w, from + QPoint(0, 25));
        const bool moved = ev(sound)->gain < 0.95;
        const bool cancelled = w.cancelMouseDrag();
        uitest::mouseRelease(&w, from + QPoint(0, 25));
        uitest::settle(30);
        check(moved && cancelled && near(ev(sound)->gain, 1.0, 1e-9) && app.undoCount() == undo0, "Escape gives the drag up: the gain is back and there is no undo step");

        // far down: silence (the bottom of the event), far up: stays at 0 dB, not above
        uitest::mouseDrag(&w, from, from + QPoint(0, 400), 10);
        uitest::settle(30);
        check(ev(sound)->gain == 0.0, "pulled to the bottom of the event and beyond, the gain is -Inf dB (0)", strfmt("%g", ev(sound)->gain));
        const QPoint low = QPoint(from.x(), rect(sound).top() + 2);  // the line is at the bottom now: the same press is found there
        (void)low;
        const QPoint bottomLine(from.x(), rect(sound).bottom() - 1);
        uitest::mouseDrag(&w, bottomLine, bottomLine - QPoint(0, 400), 10);
        uitest::settle(30);
        check(ev(sound)->gain == 1.0, "and pulled back up past the top it stops at 0 dB", strfmt("%g", ev(sound)->gain));
        app.undo();
        app.undo();
        uitest::settle(30);
        check(near(ev(sound)->gain, 1.0, 1e-9), "(undone)");
    }

    // ---------------------------------------------------------------- a video event: opacity
    {
        const size_t undo0 = app.undoCount();
        const int hv = std::max(20, rect(pic).height());
        // the line lies at the opacity: 80 % is a fifth of the way down
        const QRect r = rect(pic);
        const QPoint from(r.left() + r.width() / 2, (int)std::lround(r.top() + 0.2 * r.height()));
        uitest::mousePress(&w, from);
        uitest::mouseMove(&w, from + QPoint(0, 5));
        uitest::mouseMove(&w, from + QPoint(0, 10));
        uitest::settle(30);
        uitest::shot(&w, out + "/gain_video_drag.png");
        uitest::mouseRelease(&w, from + QPoint(0, 10));
        uitest::settle(50);
        const double want = 0.8 - 10.0 / hv;
        check(near(ev(pic)->gain, want) && app.undoCount() == undo0 + 1 && app.undoName() == "Event Opacity",
              "a video event's line sets its opacity (undo step \"Event Opacity\")", strfmt("opacity %.4f, wanted %.4f; %s", ev(pic)->gain, want, app.undoName().c_str()));
        uitest::shot(&w, out + "/gain_video_lowered.png");
        app.undo();
        uitest::settle(30);
        check(near(ev(pic)->gain, 0.8, 1e-9), "undo puts it back");
    }

    // ---------------------------------------------------------------- the fade handles are still the corners
    {
        const QRect r = rect(pic);
        const double fi0 = ev(pic)->fadeIn, g0 = ev(pic)->gain;
        // the handle is where the fade ends (1 s in)
        const double pps = (double)(rect(pic).width()) / ev(pic)->length;
        const QPoint handle((int)std::lround(r.left() + fi0 * pps), r.top() + 3);
        uitest::mouseDrag(&w, handle, handle + QPoint(30, 0));
        uitest::settle(30);
        check(ev(pic)->fadeIn > fi0 + 0.1 && near(ev(pic)->gain, g0, 1e-9) && app.undoName() == "Fade In", "the corner still drags the fade, and leaves the gain alone",
              strfmt("fade in %.3f -> %.3f, gain %.3f", fi0, ev(pic)->fadeIn, ev(pic)->gain));
        app.undo();
        uitest::settle(30);
    }

    // ---------------------------------------------------------------- several events selected: they change together
    {
        // the sound and the grouped sound at 8 s (its picture is in its group and must stay as it is)
        const double v0 = ev(vid8)->gain;
        uitest::mouseClick(&w, QPoint(rect(sound).center().x(), rect(sound).center().y() + 4));
        uitest::mouseClick(&w, QPoint(rect(aud8).center().x(), rect(aud8).center().y() + 4), Qt::LeftButton, Qt::ControlModifier);
        uitest::settle(30);
        // give them different gains first
        app.edit("Gains", [&](Project& p) { if (Event* e = p.findEvent(aud8)) e->gain = 0.6; });
        uitest::settle(30);
        const QPoint from = topPoint(sound);
        const size_t undo0 = app.undoCount();
        uitest::mouseDrag(&w, from, from + QPoint(0, 20));
        uitest::settle(30);
        const double d = 20.0 / hSound;
        check(near(ev(sound)->gain, 1.0 - d) && near(ev(aud8)->gain, std::max(0.0, 0.6 - d)), "with two events selected both change by as much", strfmt("%.4f %.4f (d %.4f)", ev(sound)->gain, ev(aud8)->gain, d));
        check(near(ev(vid8)->gain, v0, 1e-9), "a picture event in the group is left as it is (only the selected events of the same kind change)");
        check(app.undoCount() == undo0 + 1, "one undo step for both");
        uitest::shot(&w, out + "/gain_two_events.png");
        app.undo();
        uitest::settle(30);
        check(near(ev(sound)->gain, 1.0, 1e-9) && near(ev(aud8)->gain, 0.6, 1e-9), "undo restores both");
        app.undo();  // the gains edit
    }

    // ---------------------------------------------------------------- a locked event is not changed
    {
        app.edit("Lock", [&](Project& p) { if (Event* e = p.findEvent(sound)) e->locked = true; });
        uitest::settle(30);
        const QPoint from = topPoint(sound);
        uitest::mouseDrag(&w, from, from + QPoint(0, 20));
        uitest::settle(30);
        check(near(ev(sound)->gain, 1.0, 1e-9), "a locked event's gain is not changed");
        app.undo();
        uitest::settle(30);
    }

    // ---------------------------------------------------------------- the look: line, handle, fades meeting the line
    {
        app.edit("Look", [&](Project& p) {
            if (Event* e = p.findEvent(sound)) { e->gain = 0.5; e->fadeIn = 1.0; e->fadeOut = 1.0; }
        });
        uitest::mouseClick(&w, QPoint(rect(sound).center().x(), rect(sound).center().y() + 4));
        uitest::settle(50);
        uitest::shot(&w, out + "/gain_look.png");
        // the line is drawn: a bright row at the gain's height across the middle of the event
        const QImage img = w.grab().toImage();
        const QRect r = rect(sound);
        const int ly = (int)std::lround(r.top() + 0.5 * r.height());
        int bright = 0;
        for (int dy = -2; dy <= 3; ++dy)
            if (qGray(img.pixel(r.center().x() + 40, ly + dy)) > 215) ++bright;
        check(bright >= 1, "the gain line is drawn across the event at the gain's height", strfmt("%d bright pixels near y %d", bright, ly));
    }

    return uitest::summary();
}
