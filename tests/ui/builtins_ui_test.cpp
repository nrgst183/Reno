// Built-in generators and effects (src/ofx/builtins*.cpp) with no plug-in installed: listed, put on the timeline,
// shown in the Media Generator window and drawn (Titles & Text uses Qt fonts).
//
//   builtins_ui_test <output folder>
#include "../../src/app/builtintext.h"
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/engine/catalog.h"
#include "../../src/engine/engine.h"
#include "../../src/ofx/builtins.h"
#include "uitestlib.h"
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QTreeWidget>

using namespace reno;
using namespace reno::app;
using uitest::check;

static std::vector<QTreeWidgetItem*> leaves(QTreeWidget* t)
{
    std::vector<QTreeWidgetItem*> out;
    std::function<void(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* it) {
        if (it->childCount() == 0) out.push_back(it);
        for (int k = 0; k < it->childCount(); ++k) walk(it->child(k));
    };
    for (int k = 0; k < t->topLevelItemCount(); ++k) walk(t->topLevelItem(k));
    return out;
}

static bool listed(QTreeWidget* t, const QString& name)
{
    for (auto* it : leaves(t)) if (it->text(0) == name) return true;
    return false;
}

// The project's picture at 'time' as a QImage (BGRA, bottom row first).
static QImage frame(double time, int w, int h)
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

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("builtins_ui_test");
    QApplication::setStyle("Fusion");
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("builtins_ui_out");
    QDir().mkpath(out);
    uitest::setup({}, {});
    installBuiltinTextRasterizer();

    // ---- listed with no plug-in installed
    int gens = 0, effects = 0;
    for (const auto& e : Catalog::get().video()) { gens += e.builtin && e.generator; effects += e.builtin && e.filter; }
    check(gens == 7 && effects >= 20, "the catalog has the built-in generators and effects with nothing installed", strfmt("%d generators, %d effects", gens, effects));
    {
        FxBrowser b(FxBrowser::Generators);
        b.resize(320, 460);
        b.show();
        uitest::settle(150);
        QTreeWidget* t = b.findChild<QTreeWidget*>("fxTree");
        const bool all = t && listed(t, "Solid Color") && listed(t, "Color Gradient") && listed(t, "Checkerboard") && listed(t, "Noise Texture") &&
                         listed(t, "Test Pattern") && listed(t, "Titles & Text");
        check(all, "Media Generators lists Solid Color, Color Gradient, Checkerboard, Noise Texture, Test Pattern and Titles & Text");
        uitest::shot(&b, out + "/builtin_generators.png");
    }
    {
        FxBrowser b(FxBrowser::VideoFx);
        b.resize(320, 600);
        b.show();
        uitest::settle(150);
        QTreeWidget* t = b.findChild<QTreeWidget*>("fxTree");
        check(t && listed(t, "Gaussian Blur") && listed(t, "Chroma Keyer") && listed(t, "Color Corrector (Secondary)") && listed(t, "Invert"),
              "Video FX lists the built-in effects");
        uitest::shot(&b, out + "/builtin_videofx.png");
    }

    // ---- a title on the timeline, its Media Generator window, and its picture
    AppState& app = AppState::get();
    app.newProject();
    const uint64_t media = app.addGeneratedMedia("com.vegascreativesoftware:titlesandtext", "Titles & Text");
    check(media != 0, "Titles & Text becomes generated media");
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    check(!placed.empty(), "and goes on the timeline");
    {
        Playback playback;
        FxWindow w(&playback, nullptr);
        w.resize(560, 720);
        FxTarget t;
        t.kind = FxTarget::Generator;
        t.id = media;
        w.showTarget(t, 0);
        w.show();
        uitest::settle(300);
        bool text = false, loc = false;
        for (QWidget* r : w.findChildren<QWidget*>("fxRow")) {
            if (r->property("param").toString() == "Text") text = true;
            if (r->property("param").toString() == "Location") loc = true;
        }
        check(text && loc, "its Media Generator window has VEGAS's controls (Text, Location...)");
        uitest::shot(&w, out + "/builtin_titles_window.png");
    }
    QImage pic = frame(1.0, 640, 360);
    int lit = 0;
    for (int y = 0; y < pic.height(); ++y)
        for (int x = 0; x < pic.width(); ++x) lit += qRed(pic.pixel(x, y)) > 200;
    check(!pic.isNull() && lit > 500, "its picture: \"Sample Text\" drawn with Qt's fonts", strfmt("%d white pixels", lit));
    pic.save(out + "/builtin_titles_frame.png");

    // ---- a test pattern under a built-in effect
    app.newProject();
    const uint64_t bars = app.addGeneratedMedia("com.vegascreativesoftware:testpattern", "Test Pattern");
    app.addMediaToTimeline(bars, 0.0);
    pic = frame(1.0, 640, 360);
    // VEGAS's 75% bars (level 180): grey on the left, then cyan, yellow, green, magenta, blue, and red on the right
    check(!pic.isNull() && qRed(pic.pixel(20, 50)) > 150 && qGreen(pic.pixel(20, 50)) > 150 && qBlue(pic.pixel(20, 50)) > 150 &&
              qBlue(pic.pixel(500, 50)) > 150 && qRed(pic.pixel(500, 50)) < 60 && qRed(pic.pixel(620, 50)) > 150 && qBlue(pic.pixel(620, 50)) < 60,
          "Test Pattern draws its bars", "");
    pic.save(out + "/builtin_testpattern_frame.png");
    return uitest::summary();
}
