// VEGAS's own editors in the effects window (src/app/fxeditors.cpp): Color Curves graph, Color Grading curves, wheels and LUTs,
// LUT Filter's file picker, Color Match's pictures and the push buttons of the VEGAS effects. Driven with the mouse; the
// settings they write and what the built-in effect draws are checked.
//
//   fxeditors_test <output folder> <testfx.ofx>
#include "../../src/app/fxeditors.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/playback.h"
#include "../../src/engine/catalog.h"
#include "../../src/engine/engine.h"
#include "../../src/ofx/builtins.h"
#include "../../src/ofx/builtins_magixdata.h"
#include "../../src/ofx/vegascurves.h"
#include "../../src/core/base.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

FxWindow* gWin = nullptr;
QString gOut;

void settle(int ms = 120)
{
    uitest::settle(ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

QWidget* row(const char* param)
{
    for (QWidget* w : gWin->findChildren<QWidget*>("fxRow"))
        if (w->property("param").toString() == param && w->window() == gWin && w->isVisibleTo(gWin)) return w;
    return nullptr;
}

template <class T>
T* in(const char* param, const char* name, int k = 0)
{
    QWidget* r = row(param);
    if (!r) return nullptr;
    QList<T*> v = r->findChildren<T*>(name);
    return k < v.size() ? v[k] : nullptr;
}

ofx::Param* paramOf(uint64_t slot, const char* name)
{
    ofx::Instance* inst = AppState::get().fx.video(slot);
    return inst ? inst->params.find(name) : nullptr;
}
std::string textOf(uint64_t slot, const char* name, double time = 0)
{
    ofx::Param* p = paramOf(slot, name);
    return p ? p->strAt(time) : std::string("<none>");
}
double numberOf(uint64_t slot, const char* name, double time = 0, int k = 0)
{
    ofx::Param* p = paramOf(slot, name);
    if (!p) return -1e300;
    const std::vector<double> v = p->valueAt(time);
    return k < (int)v.size() ? v[(size_t)k] : -1e300;
}

// The project's picture at 'time' (BGRA, bottom row first).
QImage frame(double time, int w = 320, int h = 180)
{
    MediaPool pool;
    FxRuntime fx;
    VideoEngine video(pool, fx);
    Project p = AppState::get().project();
    p.settings.width = w;
    p.settings.height = h;
    fx.prepare(p);
    FrameOptions fo;
    ImagePtr img = video.render(p, time, fo);
    QImage out;
    if (img && img->depth == PixelDepth::Byte) {
        out = QImage(img->w, img->h, QImage::Format_ARGB32);
        for (int y = 0; y < img->h; ++y) std::memcpy(out.scanLine(img->h - 1 - y), img->row(y), (size_t)img->w * 4);
    }
    fx.releaseExcept({});
    return out;
}

// Sum of one colour channel over the picture (0 red, 1 green, 2 blue).
double sum(const QImage& im, int ch)
{
    double s = 0;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x) {
            const QRgb c = im.pixel(x, y);
            s += ch == 0 ? qRed(c) : ch == 1 ? qGreen(c) : qBlue(c);
        }
    return s;
}

// Where a point of the curve graph is in the widget (256 pixel square graph, 8 pixel margin; x and y in 0..range).
QPoint graphAt(QWidget* g, double x, double y, double range)
{
    return QPoint((int)std::lround(8 + x / range * 240.0), (int)std::lround(248 - y / range * 240.0));
}

void dumpRows(const char* what)
{
    if (const FxChain* c = AppState::get().chainOf(AppState::get().project(), FxTarget{FxTarget::VideoOut, 0})) {
        std::printf("  chain:");
        for (const FxSlot& sl : *c) std::printf(" %llu=%s", (unsigned long long)sl.id, sl.pluginId.c_str());
        std::printf("\n");
    }
    for (QLabel* l : gWin->findChildren<QLabel*>("fxMessage")) std::printf("  message: %s (%s)\n", l->text().toStdString().c_str(), l->isVisibleTo(gWin) ? "shown" : "hidden");
    std::printf("  panels: %d\n", (int)gWin->findChildren<QWidget*>("fxOfxPanel").size());
    std::printf("  [%s:", what);
    for (QWidget* r : gWin->findChildren<QWidget*>("fxRow")) std::printf(" %s%s", r->property("param").toString().toStdString().c_str(), r->isVisibleTo(gWin) ? "" : "(hidden)");
    std::printf("]\n");
}

bool near(double a, double b, double eps) { return std::fabs(a - b) <= eps; }

QString writeInvertingLut(const QString& dir)
{
    const QString path = dir + "/invert.cube";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    QString s = "TITLE \"invert\"\nLUT_3D_SIZE 2\n";
    for (int b = 0; b < 2; ++b)
        for (int g = 0; g < 2; ++g)
            for (int r = 0; r < 2; ++r) s += QString("%1 %2 %3\n").arg(1 - r).arg(1 - g).arg(1 - b);
    f.write(s.toUtf8());
    return path;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("fxeditors_test");
    QApplication::setStyle("Fusion");
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("fxeditors_out");
    QDir().mkpath(gOut);
    std::vector<std::string> ofxFiles;
    for (int k = 2; k < args.size(); ++k) ofxFiles.push_back(args[k].toStdString());
    uitest::setup(ofxFiles, {});

    // ------------------------------------------------------------------ the curve text of VEGAS
    {
        ofx::BezCurve c[4];
        check(ofx::decodeColorCurves(ofx::builtin::kMagixDefaultCurves, c), "Color Curves: the default text decodes");
        check(c[0].size() == 2 && c[3].size() == 2 && near(c[0][1].x, 255, 1e-9) && near(c[0][0].outShare, 0.2, 1e-9), "the default curves are two points each with 0.2 handles");
        const std::string again = ofx::encodeColorCurves(c);
        check(base64Decode(again) == base64Decode(ofx::builtin::kMagixDefaultCurves), "an unedited text is written back byte for byte");
        ofx::BezCurve g;
        check(ofx::decodeGradingCurve("2:0.0:0.0:-0.2:0.0:0.2:0.2:1.0:1.0:-0.2:-0.2:0.2:0.0", g) && g.size() == 2 && near(g[0].outSlope, 1, 1e-9),
              "Color Grading's curve text decodes");
        const std::string gt = ofx::encodeGradingCurve(g);
        ofx::BezCurve g2;
        check(ofx::decodeGradingCurve(gt, g2) && g2.size() == 2 && near(g2[1].inShare, 0.2, 1e-6), "and is written in the same format");
        size_t at = ofx::bezInsert(g2, 0.5, 0.7, 1.0);
        check(at == 1 && g2.size() == 3 && near(g2[1].y, 0.7, 1e-9), "a point is inserted between its neighbours");
        check(ofx::bezRemove(g2, 1) && !ofx::bezRemove(g2, 0) && g2.size() == 2, "a point is removed, the end points stay");
    }

    // ------------------------------------------------------------------ the window with a picture under it
    AppState& app = AppState::get();
    app.newProject();
    const uint64_t bars = app.addGeneratedMedia("com.vegascreativesoftware:testpattern", "Test Pattern");
    app.addMediaToTimeline(bars, 0.0);
    app.setCursor(1.0);
    FxTarget out;
    out.kind = FxTarget::VideoOut;
    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    gWin = w;
    w->resize(700, 900);
    w->show();
    settle();

    // ------------------------------------------------------------------ Color Curves
    {
        const uint64_t S = app.addVideoFx(out, "com.vegascreativesoftware:colorcurves", "Color Curves");
        std::printf("  curves slot %llu\n", (unsigned long long)S);
        w->showTarget(out, S);
        settle(200);
        check(row("Property") != nullptr, "Color Curves: the curves have a row");
        QWidget* g = in<QWidget>("Property", "fxCurveGraph");
        check(g != nullptr, "Color Curves: the row holds a curve graph");
        check(row("SelectedChannel") == nullptr && row("ShowAllChannels") == nullptr, "the channel settings are the editor's, with no rows of their own");
        uitest::shot(w, gOut + "/fxeditors_colorcurves.png");
        if (g) {
            const std::string t0 = textOf(S, "Property");
            ofx::BezCurve c[4];
            check(ofx::decodeColorCurves(t0, c) && c[0].size() == 2, "its text is the straight curves");
            const QImage p0 = frame(1.0);
            // drag a new point on the red curve up
            uitest::mouseDrag(g, graphAt(g, 128, 128, 255), graphAt(g, 128, 200, 255), 6);
            settle();
            const std::string t1 = textOf(S, "Property");
            check(ofx::decodeColorCurves(t1, c) && c[0].size() == 3 && c[1].size() == 2 && c[2].size() == 2 && c[3].size() == 2,
                  "dragging on the red curve adds a point to it only", "red points: " + std::to_string(c[0].size()));
            check(c[0].size() == 3 && near(c[0][1].x, 128, 3) && near(c[0][1].y, 200, 3), "the point is where it was dragged to",
                  c[0].size() == 3 ? std::to_string(c[0][1].x) + ", " + std::to_string(c[0][1].y) : "");
            check(c[0].size() == 3 && c[0][1].inOn && c[0][1].outOn && c[0][1].flags[0] == 1, "and has handles both ways, as VEGAS writes them");
            const QImage p1 = frame(1.0);
            check(sum(p1, 0) > sum(p0, 0) + 1000 && near(sum(p1, 1), sum(p0, 1), 1) && near(sum(p1, 2), sum(p0, 2), 1),
                  "the picture gets redder, the other channels stay", strfmt("red %.0f -> %.0f", sum(p0, 0), sum(p1, 0)));
            p1.save(gOut + "/fxeditors_colorcurves_frame.png");
            uitest::shot(w, gOut + "/fxeditors_colorcurves_edited.png");

            // the point's handle
            if (c[0].size() == 3) {
                double hdx, hdy;
                ofx::bezHandleOut(c[0], 1, &hdx, &hdy);
                const QPoint hp = graphAt(g, c[0][1].x + hdx, c[0][1].y + hdy, 255);
                const double slope0 = c[0][1].outSlope;
                uitest::mouseDrag(g, hp, hp + QPoint(6, -40), 6);
                settle();
                ofx::BezCurve d[4];
                check(ofx::decodeColorCurves(textOf(S, "Property"), d) && d[0].size() == 3, "a handle is dragged");
                check(d[0].size() == 3 && d[0][1].outSlope > slope0 + 0.3, "the out handle is steeper", strfmt("%.3f -> %.3f", slope0, d[0].size() == 3 ? d[0][1].outSlope : 0.0));
                check(d[0].size() == 3 && near(d[0][1].inSlope, d[0][1].outSlope, 1e-6), "the other handle turns with it (a smooth point)");
                check(d[0].size() == 3 && near(d[0][1].x, c[0][1].x, 1e-6) && near(d[0][1].y, c[0][1].y, 1e-6), "the point itself stays");
                uitest::shot(w, gOut + "/fxeditors_colorcurves_handle.png");
            }
            // one undo step each
            app.undo();
            settle();
            check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[0].size() == 3, "undo takes back the handle drag only");
            app.undo();
            settle();
            check(textOf(S, "Property") == t0, "and the next undo the point");
            app.redo();
            app.redo();
            settle();
            check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[0].size() == 3, "redo puts them back");

            // a point is taken away with a double-click, with the right button and with Delete
            uitest::mouseDoubleClick(g, graphAt(g, c[0][1].x, c[0][1].y, 255));
            settle();
            check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[0].size() == 2, "a double-click on a point takes it away");
            uitest::mouseClick(g, graphAt(g, 60, 100, 255));
            settle();
            check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[0].size() == 3, "a click adds a point");
            uitest::mouseClick(g, graphAt(g, c[0][1].x, c[0][1].y, 255), Qt::RightButton);
            settle();
            check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[0].size() == 2, "the right button takes it away");
            uitest::mouseClick(g, graphAt(g, 60, 100, 255));
            uitest::key(g, Qt::Key_Delete);
            settle();
            check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[0].size() == 2, "Delete takes the picked point away");

            // the channel
            if (QAbstractButton* green = in<QAbstractButton>("Property", "fxChannel1")) {
                green->click();
                settle();
                check(numberOf(S, "SelectedChannel") == 1, "Green picked: the effect's SelectedChannel is 1");
                uitest::mouseDrag(g, graphAt(g, 128, 128, 255), graphAt(g, 128, 60, 255), 6);
                settle();
                check(ofx::decodeColorCurves(textOf(S, "Property"), c) && c[1].size() == 3 && c[0].size() == 2, "a point is added to the green curve");
                const QImage p2 = frame(1.0);
                check(sum(p2, 1) < sum(p1, 1) - 1000, "the picture loses green");
            } else {
                check(false, "the channel buttons exist");
            }
            if (QAbstractButton* master = in<QAbstractButton>("Property", "fxChannel3")) {
                master->click();
                settle();
                check(numberOf(S, "SelectedChannel") == 3, "Master picked: SelectedChannel is 3");
            }
            if (QAbstractButton* all = in<QAbstractButton>("Property", "fxChannel4")) {
                all->click();
                settle();
                check(numberOf(S, "SelectedChannel") == 4, "All picked: SelectedChannel is 4");
            }
            if (QAbstractButton* show = in<QAbstractButton>("Property", "fxShowAll")) {
                show->click();
                settle();
                check(numberOf(S, "ShowAllChannels") == 0, "Show all channels unticked: the effect's ShowAllChannels is 0");
                show->click();
                settle();
            }
            // the point's numbers
            if (QDoubleSpinBox* ob = in<QDoubleSpinBox>("Property", "fxCurveOut")) {
                if (QAbstractButton* red = in<QAbstractButton>("Property", "fxChannel0")) red->click();
                uitest::mouseClick(g, graphAt(g, 200, 200, 255));
                settle();
                ob->setValue(150);
                settle();
                ofx::BezCurve d[4];
                bool found = false;
                if (ofx::decodeColorCurves(textOf(S, "Property"), d))
                    for (const auto& p : d[0]) found = found || (near(p.y, 150, 0.01) && near(p.x, 200, 4));
                check(found, "the Output box moves the picked point");
            }

        }
        app.removeFx(out, S);
        w->showTarget(out, 0);
        settle();
    }

    {
        // keyframes on the text
        const uint64_t S = app.addVideoFx(out, "com.vegascreativesoftware:colorcurves", "Color Curves");
        std::printf("  curves slot %llu\n", (unsigned long long)S);
        w->showTarget(out, S);
        settle(200);
        QWidget* g = in<QWidget>("Property", "fxCurveGraph");
        QToolButton* key = in<QToolButton>("Property", "fxKey");
        check(key != nullptr, "Color Curves: the row has a keyframe button");
        if (g && key) {
            app.setCursor(1.0);
            settle();
            key->click();
            settle();
            ofx::Param* p = paramOf(S, "Property");
            std::printf("  key: checked %d enabled %d visible %d keys %d inst %p cursor %g\n", (int)key->isChecked(), (int)key->isEnabled(), (int)key->isVisible(),
                        p ? (int)p->strKeys.size() : -1, (void*)AppState::get().fx.video(S), AppState::get().cursor());
            check(p && p->strKeys.size() == 1, "the keyframe button sets a keyframe on the curves");
            app.setCursor(3.0);
            settle();
            uitest::mouseDrag(g, graphAt(g, 128, 128, 255), graphAt(g, 128, 220, 255), 6);
            settle();
            p = paramOf(S, "Property");
            check(p && p->strKeys.size() == 2, "a change at another time adds a second keyframe", p ? std::to_string(p->strKeys.size()) : "");
            ofx::BezCurve a[4], b[4];
            const double t1 = p && !p->strKeys.empty() ? p->strKeys.begin()->first : 0, t2 = p && !p->strKeys.empty() ? p->strKeys.rbegin()->first : 0;
            check(p && ofx::decodeColorCurves(p->strAt(t1), a) && ofx::decodeColorCurves(p->strAt(t2), b) && a[0].size() == 2 && b[0].size() == 3,
                  "the first key keeps the straight curves, the second has the new point");
            app.setCursor(1.0);
            settle(200);
            QWidget* g2 = in<QWidget>("Property", "fxCurveGraph");
            const QImage f1 = frame(1.0), f3 = frame(3.0);
            check(sum(f3, 0) > sum(f1, 0) + 1000, "the picture follows the keyframes", strfmt("%.0f at 1 s, %.0f at 3 s", sum(f1, 0), sum(f3, 0)));
            (void)g2;
            key->click();                                      // off again: the keys go
            settle();
            p = paramOf(S, "Property");
            check(p && p->strKeys.empty(), "the keyframe button takes the keyframes away");
        }
        app.removeFx(out, S);
        w->showTarget(out, 0);
        settle();
    }

    // ------------------------------------------------------------------ Color Grading
    {
        const uint64_t S = app.addVideoFx(out, "com.vegascreativesoftware:colorgrading", "Color Grading");
        std::printf("  grading slot %llu\n", (unsigned long long)S);
        w->showTarget(out, S);
        settle(250);

        dumpRows("grading");
        check(row("Curve_R") != nullptr && in<QWidget>("Curve_R", "fxCurveGraph") != nullptr, "Color Grading: a row with the curves");
        check(row("Curve_G") == nullptr && row("Curve_Y") == nullptr, "the other three curves have no rows of their own");
        const char* wheels[4] = {"Lift_R", "Gamma_R", "Gain_R", "Offset_R"};
        bool all = true;
        for (const char* wn : wheels) all = all && in<QWidget>(wn, "fxWheel") != nullptr;
        check(all, "a wheel for Lift, Gamma, Gain and Offset");
        check(row("Lift_G") == nullptr && row("Lift_Y") == nullptr, "the wheel's other settings have no rows");
        check(row("LUTEnable_0") != nullptr && row("LUTEnable_1") != nullptr && in<QWidget>("LUTEnable_1", "fxLutFile") != nullptr, "and a row for each of the two LUTs");
        check(row("SaturationAdjust") != nullptr && row("ExposureAdjust") != nullptr, "the sliders are rows as before");
        w->resize(700, 1700);
        settle(200);
        uitest::shot(w, gOut + "/fxeditors_colorgrading.png");

        const QImage p0 = frame(1.0);
        if (QWidget* wheel = in<QWidget>("Lift_R", "fxWheel")) {
            const QPoint c(wheel->width() / 2, wheel->height() / 2);
            uitest::mouseDrag(wheel, c, c + QPoint(0, -40), 6);          // towards red (the top)
            settle();
            const double r = numberOf(S, "Lift_R"), g = numberOf(S, "Lift_G"), b = numberOf(S, "Lift_B");
            check(r > 0.1 && g < 0 && b < 0, "Lift wheel dragged to red: red up, green and blue down", strfmt("%.3f %.3f %.3f", r, g, b));
            const QImage p1 = frame(1.0);
            check(sum(p1, 0) > sum(p0, 0) + 1000, "the picture gets redder", strfmt("%.0f -> %.0f", sum(p0, 0), sum(p1, 0)));
            if (QDoubleSpinBox* yb = in<QDoubleSpinBox>("Lift_R", "fxWheelY")) {
                yb->setValue(0.1);
                settle();
                check(near(numberOf(S, "Lift_Y"), 0.1, 1e-9), "the Y box sets the wheel's Y");
            }
            if (QDoubleSpinBox* rb = in<QDoubleSpinBox>("Lift_R", "fxWheelR")) {
                rb->setValue(0.0);
                settle();
                check(near(numberOf(S, "Lift_R"), 0.0, 1e-9), "the R box sets the red amount");
            }
            if (QToolButton* key = in<QToolButton>("Lift_R", "fxKey")) {
                key->click();
                settle();
                ofx::Param* lr = paramOf(S, "Lift_R");
                ofx::Param* lv = paramOf(S, "Lift_V");
                check(lr && lv && lr->keys.size() == 1 && lv->keys.size() == 1, "the wheel's keyframe button keys all its settings");
                key->click();
                settle();
                lr = paramOf(S, "Lift_R");
                check(lr && lr->keys.empty(), "and takes the keyframes away");
            }
            if (QAbstractButton* rs = in<QAbstractButton>("Lift_R", "fxWheelReset")) {
                rs->click();
                settle();
                check(near(numberOf(S, "Lift_R"), 0, 1e-9) && near(numberOf(S, "Lift_Y"), 0, 1e-9) && near(numberOf(S, "Lift_G"), 0, 1e-9), "Reset puts the wheel back");
            }
        }
        if (QWidget* wheel = in<QWidget>("Gain_R", "fxWheel")) {
            const QPoint c(wheel->width() / 2, wheel->height() / 2);
            uitest::mouseDrag(wheel, c, c + QPoint(30, 30), 6);
            settle();
            check(numberOf(S, "Gain_R") != 1.0 || numberOf(S, "Gain_B") != 1.0, "the Gain wheel (neutral at 1) moves its amounts");
            check(near(numberOf(S, "Gain_Y"), 1.0, 1e-9), "and leaves Y alone");
            uitest::mouseDoubleClick(wheel, c);
            settle();
            check(near(numberOf(S, "Gain_R"), 1.0, 1e-9) && near(numberOf(S, "Gain_G"), 1.0, 1e-9), "a double-click puts the wheel back to neutral");
        }
        if (QWidget* g = in<QWidget>("Curve_R", "fxCurveGraph")) {
            const QImage q0 = frame(1.0);
            uitest::mouseDrag(g, graphAt(g, 128, 128, 255), graphAt(g, 128, 190, 255), 6);  // the master curve is the one picked
            settle();
            ofx::BezCurve cy;
            check(ofx::decodeGradingCurve(textOf(S, "Curve_Y"), cy) && cy.size() == 3, "the master curve (Curve_Y) gets the point");
            check(cy.size() == 3 && near(cy[1].y, 190.0 / 255.0, 0.015), "at the place it was dragged to", cy.size() == 3 ? std::to_string(cy[1].y) : "");
            ofx::BezCurve cr;
            check(ofx::decodeGradingCurve(textOf(S, "Curve_R"), cr) && cr.size() == 2, "the red curve is untouched");
            const QImage q1 = frame(1.0);
            check(sum(q1, 0) + sum(q1, 1) + sum(q1, 2) > sum(q0, 0) + sum(q0, 1) + sum(q0, 2) + 3000, "the picture gets brighter");
            if (QAbstractButton* blue = in<QAbstractButton>("Curve_R", "fxChannel2")) {
                blue->click();
                uitest::mouseDrag(g, graphAt(g, 100, 100, 255), graphAt(g, 100, 40, 255), 6);
                settle();
                check(ofx::decodeGradingCurve(textOf(S, "Curve_B"), cr) && cr.size() == 3, "the Blue button: the point goes to Curve_B");
            }
            uitest::shot(w, gOut + "/fxeditors_colorgrading_curves.png");
        }
        // a LUT
        {
            const QString cube = writeInvertingLut(gOut);
            fileChooser() = [cube](QWidget*, const QString&, const QString&, const QString&) { return cube; };
            const QImage p0b = frame(1.0);
            if (QAbstractButton* br = in<QAbstractButton>("LUTEnable_0", "fxLutBrowse")) {
                br->click();
                settle();
                check(textOf(S, "LUTFilename_0") == cube.toStdString(), "Browse: the LUT's file name is set");
                check(numberOf(S, "LUTEnable_0") == 1, "and the LUT is switched on");
                const QImage p1 = frame(1.0);
                check(sum(p1, 0) != sum(p0b, 0) && near(sum(p1, 0) + sum(p0b, 0), 255.0 * p1.width() * p1.height(), 3000), "an inverting LUT inverts the picture");
            } else {
                check(false, "the LUT row has a Browse button");
            }
            if (QSlider* st = in<QSlider>("LUTEnable_0", "fxLutStrength")) {
                st->setValue(500);
                settle();
                check(near(numberOf(S, "LUTGain_0"), 0.5, 1e-6), "the LUT's strength slider sets LUTGain_0");
            }
            fileChooser() = nullptr;
        }
        app.removeFx(out, S);
        w->showTarget(out, 0);
        settle();
    }

    // ------------------------------------------------------------------ LUT Filter
    {
        const uint64_t S = app.addVideoFx(out, "com.vegascreativesoftware:lutfilter", "LUT Filter");
        w->showTarget(out, S);
        settle(200);
        QAbstractButton* browse = in<QAbstractButton>("LUTFilename", "fxBrowse");
        check(browse != nullptr, "LUT Filter: its file has a Browse button");
        if (browse) {
            const QString cube = writeInvertingLut(gOut);
            QString askedFilter;
            fileChooser() = [cube, &askedFilter](QWidget*, const QString&, const QString&, const QString& f) { askedFilter = f; return cube; };
            const QImage p0 = frame(1.0);
            browse->click();
            settle();
            check(textOf(S, "LUTFilename") == cube.toStdString(), "the picked LUT becomes the setting");
            check(askedFilter.contains("*.cube"), "the dialog offers LUT files", askedFilter.toStdString());
            check(textOf(S, "LUTName") == "invert", "and its name is the file's name", textOf(S, "LUTName"));
            const QImage p1 = frame(1.0);
            check(near(sum(p1, 0) + sum(p0, 0), 255.0 * p1.width() * p1.height(), 3000), "and the picture is inverted by it");
            fileChooser() = nullptr;
        }
        uitest::shot(w, gOut + "/fxeditors_lutfilter.png");
        app.removeFx(out, S);
        w->showTarget(out, 0);
        settle();
    }

    // ------------------------------------------------------------------ Color Match
    {
        const uint64_t S = app.addVideoFx(out, "com.vegascreativesoftware:colormatch", "Color Match");
        w->showTarget(out, S);
        settle(200);
        check(in<QWidget>("ImageSource", "fxColorMatch") != nullptr, "Color Match: a row for the pictures");
        QImage orange(64, 64, QImage::Format_RGB32);
        orange.fill(QColor(230, 140, 40));
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x) orange.setPixelColor(x, y, QColor(200, 100, 20));
        const QString png = gOut + "/orange.png";
        orange.save(png);
        fileChooser() = [png](QWidget*, const QString&, const QString&, const QString&) { return png; };
        if (QAbstractButton* load = in<QAbstractButton>("ImageSource", "fxMatchSourceLoad")) {
            const QImage p0 = frame(1.0);
            load->click();
            settle();
            check(numberOf(S, "SourceCaptured") == 1, "Load: the source is captured");
            const std::string d = textOf(S, "internaldata");
            double st[12] = {0};
            const std::string b = base64Decode(d);
            if (b.size() >= 96) std::memcpy(st, b.data(), 96);
            check(st[0] > 55 && st[0] < 75 && st[2] > 15 && st[4] > 40, "its mean Lab colour is orange's", strfmt("L %.1f a %.1f b %.1f", st[0], st[2], st[4]));
            const QImage p1 = frame(1.0);
            check(sum(p1, 0) != sum(p0, 0), "the picture is matched towards it");
            p1.save(gOut + "/fxeditors_colormatch_frame.png");
            uitest::shot(w, gOut + "/fxeditors_colormatch.png");
            if (QAbstractButton* clear = in<QAbstractButton>("ImageSource", "fxMatchSourceClear")) {
                clear->click();
                settle();
                check(numberOf(S, "SourceCaptured") == 0, "Clear: no source again");
            }
        } else {
            check(false, "Color Match has a Load button");
        }
        fileChooser() = nullptr;
        app.removeFx(out, S);
        w->showTarget(out, 0);
        settle();
    }

    // ------------------------------------------------------------------ Color Corrector's wheels
    {
        const uint64_t S = app.addVideoFx(out, "com.vegascreativesoftware:colorcorrector", "Color Corrector");
        w->showTarget(out, S);
        settle(200);
        QWidget* wheel = in<QWidget>("Mid", "fxWheel");
        check(wheel != nullptr && in<QWidget>("Low", "fxWheel") != nullptr && in<QWidget>("High", "fxWheel") != nullptr, "Color Corrector: a wheel each for Low, Mid and High");
        if (wheel) {
            const QImage p0 = frame(1.0);
            const QPoint c(wheel->width() / 2, wheel->height() / 2);
            uitest::mouseDrag(wheel, c, c + QPoint(36, 0), 6);               // towards blue (the right)
            settle();
            const double ang = numberOf(S, "Mid", 0, 0), amt = numberOf(S, "Mid", 0, 1);
            check((ang < 10 || ang > 350) && amt > 0.4 && amt < 0.8, "dragged to the right: blue's angle and an amount", strfmt("%.1f degrees, %.3f", ang, amt));
            const QImage p1 = frame(1.0);
            check(sum(p1, 2) > sum(p0, 2) + 1000 && sum(p1, 2) - sum(p0, 2) > sum(p1, 0) - sum(p0, 0) + 1000, "the midtones get bluer", strfmt("blue %.0f -> %.0f", sum(p0, 2), sum(p1, 2)));
            if (QDoubleSpinBox* ab = in<QDoubleSpinBox>("Mid", "fxWheelAngle")) {
                ab->setValue(103.5);
                settle();
                check(near(numberOf(S, "Mid", 0, 0), 103.5, 1e-6) && near(numberOf(S, "Mid", 0, 1), amt, 0.01), "the Angle box turns the dot (Red Midtones' 103.5 degrees)");
            }
            uitest::mouseDoubleClick(wheel, c);
            settle();
            check(near(numberOf(S, "Mid", 0, 0), 0, 1e-9) && near(numberOf(S, "Mid", 0, 1), 0, 1e-9), "a double-click puts it back in the middle");
            uitest::shot(w, gOut + "/fxeditors_colorcorrector.png");
        }
        app.removeFx(out, S);
        w->showTarget(out, 0);
        settle();
    }

    // ------------------------------------------------------------------ push buttons show as buttons
    {
        struct B { const char* plugin; const char* param; };
        const B buttons[] = {{"com.vegascreativesoftware:whitebalance", "SelectWhiteColor"},
                             {"com.vegascreativesoftware:whitebalance", "EnhanceHistogram"},
                             {"com.vegascreativesoftware:brightnessandcontrast", "DoAnalysis"},
                             {"com.vegascreativesoftware:colorcorrectorsecondary", "SelectEffectRange"},
                             {"de.magix:MeshWarp", "ResetMesh"},
                             {"com.vegascreativesoftware:bzmasking", "StartTrk_0"}};
        for (const B& bt : buttons) {
            const uint64_t S = app.addVideoFx(out, bt.plugin, bt.param);
            if (!S) { check(false, std::string(bt.plugin) + " can be added"); continue; }
            w->showTarget(out, S);
            settle(150);
            QPushButton* push = in<QPushButton>(bt.param, "fxPush");
            check(push != nullptr && push->isVisibleTo(w), std::string(bt.plugin) + " " + bt.param + " shows as a button");
            app.removeFx(out, S);
            w->showTarget(out, 0);
            settle();
        }
    }

    // ------------------------------------------------------------------ every VEGAS setting that is hidden but may be edited
    {
        int seen = 0;
        for (const VideoFxEntry& e : Catalog::get().video()) {
            if (!e.ok || !e.filter || e.id.compare(0, 4, "com.") != 0) continue;
            const uint64_t S = app.addVideoFx(out, e.id, e.label);
            w->showTarget(out, S);
            settle(30);
            ofx::Instance* inst = app.fx.video(S);
            if (inst) {
                std::string hidden;
                for (const auto& p : inst->params.params)
                    if (p->secret() && p->kind != ofx::ParamKind::Group && p->kind != ofx::ParamKind::Page) hidden += " " + p->name + (p->kind == ofx::ParamKind::Custom ? "[custom]" : "");
                if (!hidden.empty() && std::getenv("FXEDITORS_LIST_HIDDEN")) std::printf("  hidden in %s:%s\n", e.id.c_str(), hidden.c_str());
                for (const auto& p : inst->params.params) {
                    if (p->kind != ofx::ParamKind::Custom || p->secret()) continue;
                    ++seen;
                    const bool shown = row(p->name.c_str()) != nullptr;
                    check(shown || isEditorMember(e.id, p->name), "Custom setting " + e.id + " " + p->name + " is shown by an editor or is the editor's");
                }
            }
            app.removeFx(out, S);
            w->showTarget(out, 0);
            settle(10);
        }
        settle();
        std::printf("  (%d visible custom settings looked at)\n", seen);
    }
    return uitest::summary();
}
