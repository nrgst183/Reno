// Effects' on-screen controls over the Video Preview (OpenFX overlay interacts, drawn by PictureView in src/app/panels.cpp, wired to
// the Video FX window by mainwindow.cpp): the effect open in the Video FX window draws its controls over the picture with OpenGL,
// its canonical coordinates land on the picture, the pen and keys go to it, what it changes is one undo step, and the controls go
// when the window closes, while playing, while the cursor is off the event, and with "Show Video FX Overlays" off.
//
//   overlay_test <output folder> <testfx.ofx>
//
// Build: uibuild overlay_test tests/ui/overlay_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
// Drawing needs OpenGL: Mesa's EGL (llvmpipe) headless here; without it the checks of what was drawn fail with a note.
#include "../../src/app/mainwindow.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QSettings>
#include <cmath>
#include <dlfcn.h>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }

std::vector<double> centerOf(uint64_t slot)
{
    ofx::Instance* inst = st().fx.video(slot);
    ofx::Param* p = inst ? inst->params.find("Center") : nullptr;
    return p ? p->value : std::vector<double>();
}

std::string str(const std::vector<double>& v)
{
    std::string s;
    for (double x : v) s += strfmt("%s%.2f", s.empty() ? "" : ",", x);
    return s;
}

// Whether there is a pixel of about this colour within 'r' pixels of 'at'.
bool near(const QImage& img, QPoint at, QRgb want, int r = 1)
{
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            const QPoint p = at + QPoint(dx, dy);
            if (!img.rect().contains(p)) continue;
            const QRgb c = img.pixel(p);
            if (std::abs(qRed(c) - qRed(want)) < 40 && std::abs(qGreen(c) - qGreen(want)) < 40 && std::abs(qBlue(c) - qBlue(want)) < 40) return true;
        }
    return false;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoOverlayTest");
    QApplication::setApplicationName("overlay_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("overlayout");
    QDir().mkpath(out);
    if (args.size() < 3) {
        std::printf("usage: overlay_test <output folder> <testfx.ofx>\n");
        return 2;
    }
    const std::string fxPath = args[2].toStdString();
    uitest::setup({fxPath}, std::vector<std::string>());
    // the test effect's counters (same binary the host loaded)
    void* fxlib = dlopen(fxPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    auto counter = fxlib ? (int (*)(const char*))dlsym(fxlib, "testfx_counter") : nullptr;
    auto count = [&](const char* n) { return counter ? counter(n) : -1; };

    AppState& app = st();
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> evs = app.addMediaToTimeline(media, 0.0);
    uint64_t ev = 0;
    for (const Track& t : app.project().tracks)
        if (t.type == TrackType::Video)
            for (const Event& e : t.events) ev = e.id;
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = ev;
    const uint64_t slot = app.addVideoFx(target, "reno.test.overlay", "Test Overlay");
    check(ev && slot, "an event with the test effect");
    app.setCursor(1.0);

    MainWindow w;
    w.resize(1600, 900);
    w.show();
    uitest::settle(600);
    PreviewWidget* pv = w.preview();
    QWidget* pic = pv->pictureWidget();

    // ---------------------------------------------------------------- the controls appear with the Video FX window
    check(pv->overlayEffect() == 0 && !pv->overlayActive(), "no controls before the Video FX window shows an effect");
    w.openFx(target, slot);
    uitest::settle(800);
    check(pv->overlayEffect() == slot, "opening the effect in the Video FX window gives the preview its controls");
    check(count("overlayCreate") == 1, "the overlay was made once", strfmt("%d", count("overlayCreate")));
    pic->update();
    uitest::settle(200);
    check(pv->overlayActive(), "they are in use (cursor over the event, nothing playing)");
    check(pv->overlayDrawn(), "and were drawn (an OpenGL context was had)");
    if (!pv->overlayDrawn()) std::printf("NOTE no OpenGL here: what is drawn cannot be checked\n");
    QImage g = uitest::shot(pic, out + "/overlay_picture.png");
    QImage gs = g.convertToFormat(QImage::Format_RGB32);
    const double dpr = pic->devicePixelRatioF();
    const QPointF c = pv->canonicalToView(960, 540);
    const QPoint cpx((int)std::lround(c.x() * dpr), (int)std::lround(c.y() * dpr));
    check(!c.isNull() && std::abs(c.x() - pic->width() / 2.0) < 3 && std::abs(c.y() - pic->height() / 2.0) < 3,
          "the frame's centre (960, 540 canonical) is the middle of the picture", strfmt("%.1f,%.1f of %dx%d", c.x(), c.y(), pic->width(), pic->height()));
    check(near(gs, cpx + QPoint(15, 0), qRgb(255, 0, 0)) && near(gs, cpx + QPoint(0, -15), qRgb(255, 0, 0)) && near(gs, cpx + QPoint(-15, 0), qRgb(255, 0, 0)),
          "the effect's red cross is drawn over the picture where its centre is");
    check(near(gs, cpx + QPoint(8, 4), qRgb(0, 255, 0)), "and its green square round it (8 screen pixels whatever the zoom)");
    {
        // the picture is still under the controls: away from them it is what the preview shows without them
        pv->setShowOverlays(false);
        pic->update();
        uitest::settle(150);
        const QImage plain = pic->grab().toImage().convertToFormat(QImage::Format_RGB32);
        pv->setShowOverlays(true);
        const QPoint away = cpx + QPoint(120, 60);
        check(plain.pixel(away) == gs.pixel(away) && !near(plain, cpx + QPoint(15, 0), qRgb(255, 0, 0)),
              "the picture under the controls is the same picture", strfmt("%08x %08x", plain.pixel(away), gs.pixel(away)));
    }
    uitest::shot(&w, out + "/overlay_window.png");

    // ---------------------------------------------------------------- the pen moves the effect's position
    const QPoint from((int)std::lround(c.x()), (int)std::lround(c.y()));
    const QPoint to = from + QPoint(40, -30);
    uitest::mouseDrag(pic, from, to);
    uitest::settle(300);
    const std::vector<double> moved = centerOf(slot);
    // canonical units per widget pixel: the frame's width over the picture's width in the widget
    const QPointF c2 = pv->canonicalToView(1960, 540);
    const double perPixel = 1000.0 / (c2.x() - c.x());
    check(moved.size() == 2 && std::abs(moved[0] - (960 + 40 * perPixel)) < perPixel + 0.5 && std::abs(moved[1] - (540 + 30 * perPixel)) < perPixel + 0.5,
          "dragging the handle moves \"Center\" with the pen (in canonical units, y up)", str(moved) + strfmt(" (%.3f a pixel)", perPixel));
    check(app.undoName() == "Edit \"Test Overlay\" On Screen", "the drag is one undo step", app.undoName());
    const FxSlot* fs = nullptr;
    if (const FxChain* ch = app.chainOf(app.project(), target))
        for (const FxSlot& s : *ch) if (s.id == slot) fs = &s;
    check(fs && fs->state.dump().find(strfmt("%.0f", std::floor(moved[0]))) != std::string::npos, "the project has the new setting",
          fs ? fs->state.dump().substr(0, 200) : std::string("no slot"));
    pic->update();
    uitest::settle(300);
    g = uitest::shot(pic, out + "/overlay_moved.png").convertToFormat(QImage::Format_RGB32);
    const QPointF cm = pv->canonicalToView(moved.size() == 2 ? moved[0] : 0, moved.size() == 2 ? moved[1] : 0);
    const QPoint cmpx((int)std::lround(cm.x() * dpr), (int)std::lround(cm.y() * dpr));
    check(near(g, cmpx + QPoint(15, 0), qRgb(255, 0, 0)) && !near(g, cpx + QPoint(-15, 0), qRgb(255, 0, 0), 0), "the controls are drawn at the new place");
    app.undo();
    uitest::settle(300);
    check(str(centerOf(slot)) == "960.00,540.00", "undo puts the position back", str(centerOf(slot)));
    app.redo();
    uitest::settle(200);
    check(str(centerOf(slot)) == str(moved), "redo moves it again", str(centerOf(slot)));
    // a press away from the handle is not the effect's: nothing changes, no undo step
    const std::string undoName = app.undoName();
    const size_t before = app.project().tracks.size();
    uitest::mouseDrag(pic, QPoint(20, 20), QPoint(60, 40));
    uitest::settle(200);
    check(str(centerOf(slot)) == str(moved) && app.undoName() == undoName && app.project().tracks.size() == before, "a drag away from the handle changes nothing");

    // ---------------------------------------------------------------- keys
    pic->setFocus();
    uitest::key(pic, Qt::Key_Right);
    uitest::settle(200);
    const std::vector<double> keyed = centerOf(slot);
    check(keyed.size() == 2 && std::abs(keyed[0] - (moved[0] + 1)) < 1e-6, "the right arrow key goes to the effect (it moves the position a pixel)", str(keyed));
    check(app.undoName() == "Edit \"Test Overlay\" On Screen", "as an undo step of its own");

    // ---------------------------------------------------------------- when the controls go
    pv->setShowOverlays(false);
    check(!pv->overlayActive(), "\"Show Video FX Overlays\" off hides them");
    check(!QSettings().value("preview/fxOverlays", true).toBool(), "and is remembered");
    pv->setShowOverlays(true);
    check(pv->overlayActive(), "and on shows them again");
    app.setCursor(20.0);
    uitest::settle(200);
    check(!pv->overlayActive(), "not while the cursor is off the event");
    app.setCursor(1.0);
    uitest::settle(200);
    check(pv->overlayActive(), "back over the event, they are back");
    app.bypassAllFx = true;
    check(!pv->overlayActive(), "not while all effects are bypassed");
    app.bypassAllFx = false;
    w.playback()->play();
    uitest::settle(300);
    check(!pv->overlayActive(), "not while playing");
    w.playback()->stop();
    app.setCursor(1.0);
    uitest::settle(300);
    check(pv->overlayActive(), "after stopping, they are back");

    // undo the effect away: the controls go with it
    w.fxWindow()->close();
    uitest::settle(300);
    check(pv->overlayEffect() == 0 && !pv->overlayActive(), "closing the Video FX window takes the controls away");
    check(count("overlayDestroy") == count("overlayCreate"), "and the overlay was destroyed", strfmt("%d made, %d destroyed", count("overlayCreate"), count("overlayDestroy")));
    w.openFx(target, slot);
    uitest::settle(500);
    check(pv->overlayEffect() == slot && pv->overlayActive(), "opened again, they come back");
    // the effect is removed while its controls are up
    app.removeFx(target, slot);
    uitest::settle(500);
    check(!pv->overlayActive(), "removing the effect takes them away");
    QApplication::processEvents();

    // ---------------------------------------------------------------- the controls follow Track Motion and Pan/Crop
    {
        uint64_t track = 0;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (e.id == ev) track = t.id;
        const uint64_t slot2 = app.addVideoFx(target, "reno.test.overlay", "Test Overlay");
        app.edit("Track Motion", [&](Project& p) {
            MotionKey k;
            k.x = 0.75;
            k.y = 0.75;
            k.w = 0.5;
            k.h = 0.5;
            p.findTrack(track)->motion.assign(1, k);
        });
        app.setCursor(1.0);
        w.openFx(target, slot2);
        uitest::settle(600);
        pic->update();
        uitest::settle(300);
        const double pp = 1000.0 / (c2.x() - c.x());        // canonical units of the frame per widget pixel
        const QPointF m0 = pv->canonicalToView(960, 540);
        // the frame's middle, moved to the middle of the bottom right quarter: 480 right and 270 down
        check(pv->overlayActive() && std::abs(m0.x() - (c.x() + 480 / pp)) < 1.5 && std::abs(m0.y() - (c.y() + 270 / pp)) < 1.5,
              "with Track Motion the effect's controls are where its picture went", strfmt("%.1f,%.1f (centre %.1f,%.1f)", m0.x(), m0.y(), c.x(), c.y()));
        const QPointF m1 = pv->canonicalToView(1960, 540);
        check(std::abs((m1.x() - m0.x()) - 500 / pp) < 1.5, "at half the size", strfmt("%.1f", m1.x() - m0.x()));
        QImage gm = uitest::shot(pic, out + "/overlay_track_motion.png").convertToFormat(QImage::Format_RGB32);
        const QPoint m0px((int)std::lround(m0.x() * dpr), (int)std::lround(m0.y() * dpr));
        check(near(gm, m0px + QPoint(15, 0), qRgb(255, 0, 0)) && near(gm, m0px + QPoint(0, -15), qRgb(255, 0, 0)) && !near(gm, cpx + QPoint(15, 0), qRgb(255, 0, 0), 0),
              "the red cross is drawn there (the same size on screen)");
        // the pen: 20 widget pixels right is 20 x pp frame units, twice that in the effect's own units
        const QPoint from2((int)std::lround(m0.x()), (int)std::lround(m0.y()));
        uitest::mouseDrag(pic, from2, from2 + QPoint(20, -10));
        uitest::settle(300);
        const std::vector<double> mv = centerOf(slot2);
        check(mv.size() == 2 && std::abs(mv[0] - (960 + 40 * pp)) < 2 * pp + 0.5 && std::abs(mv[1] - (540 + 20 * pp)) < 2 * pp + 0.5,
              "dragging the moved handle moves the effect's position in its own units", str(mv) + strfmt(" (%.3f a pixel)", pp));
        app.undo();
        uitest::settle(200);
        // a pre-pan/crop effect also follows the event's Pan/Crop: a 2x zoom on the middle of the media
        app.edit("Pan/Crop", [&](Project& p) {
            Event* e = p.findEvent(ev);
            for (FxSlot& f : e->fx)
                if (f.id == slot2) f.beforePanCrop = true;
            PanCropKey k;
            k.w = 0.5;
            k.h = 0.5;
            e->panCrop.assign(1, k);
        });
        uitest::settle(300);
        const QPointF z0 = pv->canonicalToView(960, 540), z1 = pv->canonicalToView(1060, 540);
        check(std::abs(z0.x() - m0.x()) < 1.5 && std::abs(z0.y() - m0.y()) < 1.5 && std::abs((z1.x() - z0.x()) - 100 / pp) < 1.5,
              "with Pan/Crop zooming 2x (and Track Motion halving), a pre-pan/crop effect's controls are at full size again",
              strfmt("%.1f,%.1f %.1f", z0.x(), z0.y(), z1.x() - z0.x()));
        w.fxWindow()->close();
        uitest::settle(200);
        app.removeFx(target, slot2);
        app.edit("No motion", [&](Project& p) {
            p.findTrack(track)->motion.clear();
            p.findEvent(ev)->panCrop.clear();
        });
    }

    // ---------------------------------------------------------------- a setting's control drawn by the effect
    {
        const uint64_t ps = app.addVideoFx(target, "reno.test.paraminteract", "Test Param Interact");
        w.openFx(target, ps);
        uitest::settle(600);
        QWidget* iv = w.fxWindow()->findChild<QWidget*>("fxInteract");
        check(iv && iv->isVisible() && count("levelCreate") >= 1, "a setting whose control the effect draws gets that control in the Video FX window",
              strfmt("%d made", count("levelCreate")));
        auto level = [&]() {
            ofx::Instance* inst = st().fx.video(ps);
            ofx::Param* p = inst ? inst->params.find("Level") : nullptr;
            return p && !p->value.empty() ? p->value[0] : -1.0;
        };
        if (iv) {
            // drawn once the effect is not busy rendering
            QImage im;
            const int y = iv->height() / 2;
            for (int k = 0; k < 40; ++k) {
                iv->update();
                uitest::settle(50);
                im = uitest::shot(iv, out + "/param_interact.png").convertToFormat(QImage::Format_RGB32);
                if (near(im, QPoint(im.width() / 4, y), qRgb(0, 255, 0), 0)) break;
            }
            check(near(im, QPoint(im.width() / 4, y), qRgb(0, 255, 0), 0) && !near(im, QPoint(im.width() * 3 / 4, y), qRgb(0, 255, 0), 0),
                  "it is drawn by the effect: the bar filled to the level (half)", strfmt("%08x %08x", im.pixel(im.width() / 4, y), im.pixel(im.width() * 3 / 4, y)));
            uitest::mouseClick(iv, QPoint((int)(iv->width() * 0.8), iv->height() / 2));
            uitest::settle(200);
            const double l = level();
            check(std::abs(l - 0.8) < 0.03, "clicking it goes to the effect (the level follows the click)", strfmt("%.3f", l));
            check(app.undoName() == "Change \"Level\"", "as one undo step", app.undoName());
            QImage im2;
            for (int k = 0; k < 40; ++k) {
                uitest::settle(50);
                im2 = uitest::shot(iv, QString()).convertToFormat(QImage::Format_RGB32);
                if (near(im2, QPoint(im2.width() * 3 / 4, y), qRgb(0, 255, 0), 0)) break;
            }
            check(near(im2, QPoint(im2.width() * 3 / 4, y), qRgb(0, 255, 0), 0), "and it is drawn again at the new level");
            app.undo();
            uitest::settle(200);
            check(std::abs(level() - 0.5) < 1e-9, "undo puts it back", strfmt("%.3f", level()));
            iv->setFocus();
            uitest::key(iv, Qt::Key_Right);
            uitest::settle(100);
            check(std::abs(level() - 0.6) < 1e-9, "keys go to it too (Right: a tenth more)", strfmt("%.3f", level()));
            uitest::shot(w.fxWindow(), out + "/param_interact_window.png");
        }
        w.fxWindow()->close();
        uitest::settle(300);
        check(count("levelDestroy") == count("levelCreate"), "closing the window lets the effect's control go",
              strfmt("%d made, %d destroyed", count("levelCreate"), count("levelDestroy")));
    }
    return uitest::summary();
}
