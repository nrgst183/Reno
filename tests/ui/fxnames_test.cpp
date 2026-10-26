// Effects are shown by their plug-ins' labels, never identifiers (src/engine/fxnames.*): an effect read from a VEGAS project has
// only an identifier ("com.vegascreativesoftware:cookiecutter"), and VEGAS shows the label ("VEGAS Cookie Cutter"). Checks:
//   - readable forms of identifiers, VEGAS Pro 18's labels, the plug-in list's labels;
//   - an OpenFX plug-in with no label takes the one in its bundle's resource file (tests/ofx/nolabelfx.cpp), or a readable
//     form of its identifier;
//   - a plug-in list from an older Reno (identifiers as labels, no scan version) is shown by label and rescanned (changedSinceScan);
//   - the effects window's chain buttons and "could not be loaded" message, the Plug-In Chooser's chain and list, for effects
//     with no name, named by identifier, and missing plug-ins;
//   - a user VEGAS project, if given: every OpenFX effect in it is shown by a label.
//
//   fxnames_test <output folder> <testfx.ofx> <nolabelfx.ofx> [project.veg]
//
// HOME is set to the output folder (own plug-in list). Needs the plug-in chooser sources too (pluginchooser.cpp fxlibrary.cpp fxthumbs.cpp).
#include "../../src/app/fxwindow.h"
#include "../../src/app/playback.h"
#include "../../src/app/pluginchooser.h"
#include "../../src/engine/catalog.h"
#include "../../src/engine/fxnames.h"
#include "uitestlib.h"
#include "../../src/ofx/builtins.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <cstdlib>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

// Whether a shown name is an identifier rather than a label ("com.x:y", "reno.test.invert").
bool looksLikeId(const QString& s, const std::string& id)
{
    return s.toStdString() == id || s.contains(QString::fromStdString(id));
}

// The names on the effects window's chain buttons, in order.
QStringList chainNames(FxWindow* w)
{
    QStringList out;
    for (QWidget* b : w->findChildren<QWidget*>("fxChainButton")) {
        if (!b->isVisibleTo(w)) continue;
        if (QLabel* l = b->findChild<QLabel*>("fxChainName")) out << l->text();
    }
    return out;
}

FxSlot ofxSlot(const std::string& id, const std::string& name)
{
    FxSlot s;
    s.kind = "ofx";
    s.pluginId = id;
    s.name = name;
    return s;
}

const char* kResourceXml =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<!-- <OfxPlugin name=\"reno.nolabel:resourceNamed\"><OfxResourceSet><OfxPropLabel>From A Comment</OfxPropLabel> -->\n"
    "<OfxImageEffectResource>\n"
    "  <OfxPlugin name=\"reno.nolabel:resourceNamed\">\n"
    "    <OfxResourceSet ofxHost=\"default\" ofxLocale=\"fr-FR\">\n"
    "      <OfxPropLabel>Effet Nomm&#233;</OfxPropLabel>\n"
    "    </OfxResourceSet>\n"
    "    <OfxResourceSet ofxHost=\"com.someone.else\">\n"
    "      <OfxPropLabel>Another Host's Name</OfxPropLabel>\n"
    "    </OfxResourceSet>\n"
    "    <OfxResourceSet ofxHost=\"default\">\n"
    "      <OfxPropLabel>Reno Resource &amp; Label</OfxPropLabel>\n"
    "      <OfxImageEffectPluginPropGrouping>Reno Test Resources</OfxImageEffectPluginPropGrouping>\n"
    "      <OfxPropPluginDescription>Named by its resource file.</OfxPropPluginDescription>\n"
    "      <OfxImageEffectContext name=\"default\">\n"
    "        <OfxParamTypeDouble name=\"Amount\"><OfxPropLabel>Not The Effect's Label</OfxPropLabel></OfxParamTypeDouble>\n"
    "      </OfxImageEffectContext>\n"
    "    </OfxResourceSet>\n"
    "  </OfxPlugin>\n"
    "</OfxImageEffectResource>\n";

}  // namespace

int main(int argc, char** argv)
{
    if (argc < 4) {
        std::fprintf(stderr, "usage: fxnames_test <output folder> <testfx.ofx> <nolabelfx.ofx> [project.veg]\n");
        return 2;
    }
    const QString out = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    QDir().mkpath(out);
    // own plug-in list for this test
    qputenv("HOME", out.toLocal8Bit());
    qunsetenv("XDG_CONFIG_HOME");
    qunsetenv("OFX_PLUGIN_PATH");
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("fxnames_test");
    const std::string testfx = argv[2], nolabel = argv[3];
    const std::string veg = argc > 4 ? argv[4] : "";

    // ------------------------------------------------------------------ names made from identifiers
    check(readableFxId("com.NewBlue.STCartoonrPlus") == "ST Cartoonr Plus", "readable: com.NewBlue.STCartoonrPlus", readableFxId("com.NewBlue.STCartoonrPlus"));
    check(readableFxId("com.vegascreativesoftware:Filters.Stereoscopic3DAdjust") == "Stereoscopic 3D Adjust", "readable: VEGAS's Stereoscopic 3D Adjust",
          readableFxId("com.vegascreativesoftware:Filters.Stereoscopic3DAdjust"));
    check(readableFxId("com.weirdfx.MotionMelt") == "Motion Melt", "readable: com.weirdfx.MotionMelt", readableFxId("com.weirdfx.MotionMelt"));
    check(readableFxId("com.wmmfx.art.25") == "Art 25", "readable: a number takes the part before it", readableFxId("com.wmmfx.art.25"));
    check(readableFxId("de.magix:stereographicProjectionOCL") == "Stereographic Projection OCL", "readable: de.magix:stereographicProjectionOCL",
          readableFxId("de.magix:stereographicProjectionOCL"));
    check(readableFxId("com.NewBlue.ELPicture-In-Picture") == "EL Picture In Picture", "readable: dashes", readableFxId("com.NewBlue.ELPicture-In-Picture"));
    check(readableFxId("") == "" && !readableFxId("x").empty(), "readable: empty and one letter");
    check(vegasStockFxLabel("com.vegascreativesoftware:cookiecutter") == "VEGAS Cookie Cutter", "VEGAS's label: Cookie Cutter");
    check(vegasStockFxLabel("com.vegascreativesoftware:gaussianblur") == "VEGAS Gaussian Blur", "VEGAS's label: Gaussian Blur");
    check(vegasStockFxLabel("com.vegascreativesoftware:titlesandtext") == "VEGAS Titles & Text", "VEGAS's label: Titles & Text (the resource file's &amp;)");
    check(vegasStockFxLabel("de.magix:stereographicProjectionOCL") == "VEGAS Tiny Planet", "VEGAS's label: Tiny Planet");
    check(vegasStockFxLabel("com.example:nothing").empty(), "VEGAS's label: none for others");
    // with no plug-in list at all: VEGAS's effects by VEGAS's labels, others readable, never the identifier
    check(videoFxLabel("com.vegascreativesoftware:cookiecutter") == "VEGAS Cookie Cutter", "no plug-in: a VEGAS effect by VEGAS's label",
          videoFxLabel("com.vegascreativesoftware:cookiecutter"));
    check(videoFxLabel("com.weirdfx.DeepFry") == "Deep Fry", "no plug-in: another effect by its identifier made readable", videoFxLabel("com.weirdfx.DeepFry"));
    {
        FxSlot a;
        a.kind = "audio";
        a.audio.format = "VST2";
        a.audio.name = "ReaEQ";
        check(fxSlotLabel(a) == "ReaEQ", "an audio effect by its plug-in's name");
        a.audio.name.clear();
        a.audio.path = "/x/plugins/DeepFry.so";
        check(fxSlotLabel(a) == "DeepFry", "an audio effect with no name: its file's name");
    }

    // ------------------------------------------------------------------ a plug-in list kept by an older Reno
    {
        const std::string dir = userDataDir();
        QDir().mkpath(QString::fromStdString(dir));
        const char* old =
            "{\"files\": {\"/nowhere/OFX/Vfx1.ofx\": {\"stamp\": \"1:1\", \"kind\": \"ofx\", \"result\": {\"ok\": true, \"plugins\": ["
            "{\"id\": \"com.vegascreativesoftware:cookiecutter\", \"label\": \"com.vegascreativesoftware:cookiecutter\", \"ok\": true, "
            "\"contexts\": [\"OfxImageEffectContextFilter\"]},"
            "{\"id\": \"com.acme:WarpDrive\", \"label\": \"com.acme:WarpDrive\", \"ok\": true, \"contexts\": [\"OfxImageEffectContextFilter\"]},"
            "{\"id\": \"com.acme:Labelled\", \"label\": \"Acme Labelled\", \"ok\": true, \"contexts\": [\"OfxImageEffectContextFilter\"]}]}}}}";
        QFile f(QString::fromStdString(pathJoin(dir, "plugins.json")));
        check(f.open(QIODevice::WriteOnly) && f.write(old) > 0, "old list: written");
        f.close();
        check(Catalog::get().loadCache(), "old list: read");
        std::string cookie, warp, labelled;
        for (const VideoFxEntry& e : Catalog::get().video()) {
            if (e.id == "com.vegascreativesoftware:cookiecutter") cookie = e.label;
            if (e.id == "com.acme:WarpDrive") warp = e.label;
            if (e.id == "com.acme:Labelled") labelled = e.label;
        }
        check(cookie == "VEGAS Cookie Cutter", "old list: a VEGAS effect listed by VEGAS's label, not its identifier", cookie);
        check(warp == "Warp Drive", "old list: another effect listed by its identifier made readable", warp);
        check(labelled == "Acme Labelled", "old list: a real label is kept", labelled);
        check(videoFxLabel("com.acme:Labelled") == "Acme Labelled", "videoFxLabel: the list's label");
        std::string what = "?";
        check(Catalog::get().changedSinceScan(&what), "old list: Reno looks again (made by an older Reno)", what);
    }

    // ------------------------------------------------------------------ the test plug-ins
    // the label-less plug-in in a bundle with a resource file
    const QString bundle = out + "/nolabel/nolabelfx.ofx.bundle";
    const QString binDir = bundle + "/Contents/Linux-x86-64";
    QDir().mkpath(binDir);
    QDir().mkpath(bundle + "/Contents/Resources");
    QFile::remove(binDir + "/nolabelfx.ofx");
    check(QFile::copy(QString::fromStdString(nolabel), binDir + "/nolabelfx.ofx"), "nolabelfx copied into a bundle");
    {
        QFile x(bundle + "/Contents/Resources/nolabelfx.xml");
        check(x.open(QIODevice::WriteOnly) && x.write(kResourceXml) > 0, "its resource file written");
    }
    uitest::setup({testfx, (binDir + "/nolabelfx.ofx").toStdString()}, {});
    {
        bool found = false;
        for (const VideoFxEntry& e : Catalog::get().video()) {
            if (e.id == "reno.nolabel:resourceNamed") {
                found = true;
                check(e.label == "Reno Resource & Label", "resource file: the effect's label (this host's set, English, not a comment or a parameter's)", e.label);
                check(e.group == "Reno Test Resources", "resource file: the grouping", e.group);
            }
            if (e.id == "reno.nolabel:Filters.unnamedEffect3D")
                check(e.label == "Unnamed Effect 3D", "no label anywhere: the identifier made readable", e.label);
            if (e.id == "reno.test.invert") check(e.label == "Test Invert", "a plug-in's own label", e.label);
        }
        check(found, "the label-less plug-in was found in its bundle");
    }
    check(videoFxLabel("reno.test.invert") == "Test Invert", "videoFxLabel: the plug-in's label", videoFxLabel("reno.test.invert"));
    check(videoFxLabel("reno.nolabel:resourceNamed") == "Reno Resource & Label", "videoFxLabel: the resource file's label");

    // ------------------------------------------------------------------ the effects window
    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    FxTarget vt;
    for (uint64_t id : placed) {
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = id;
        if (app.targetIsVideo(t)) vt = t;
    }
    check(vt.id != 0, "setup: a video event");
    // as a .veg gives them (no name), named by identifier, one of VEGAS's own that isn't installed here, and another missing plug-in
    const std::vector<std::pair<std::string, std::string>> want = {
        {"reno.test.invert", "Test Invert"},
        {"reno.test.invert", "Test Invert"},
        {"reno.nolabel:resourceNamed", "Reno Resource & Label"},
        {"com.vegascreativesoftware:cookiecutter", "VEGAS Cookie Cutter"},
        {"com.weirdfx.MotionMelt", "Motion Melt"},
    };
    std::vector<uint64_t> ids;
    app.edit("Effects as a VEGAS project has them", [&](Project& p) {
        Event* e = p.findEvent(vt.id);
        if (!e) return;
        for (size_t k = 0; k < want.size(); ++k) {
            FxSlot s = ofxSlot(want[k].first, k == 1 ? want[k].first : std::string());
            s.id = p.newId();
            ids.push_back(s.id);
            e->fx.push_back(s);
        }
    });
    uitest::settle(300);
    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    w->resize(700, 600);
    w->showTarget(vt, ids.empty() ? 0 : ids[3]);
    w->show();
    uitest::settle(400);
    const QStringList names = chainNames(w);
    check(names.size() == (int)want.size(), "effects window: a button for each effect", std::to_string(names.size()));
    for (int k = 0; k < names.size() && k < (int)want.size(); ++k) {
        check(names[k] == QString::fromStdString(want[(size_t)k].second), "effects window: button " + std::to_string(k + 1) + " shows " + want[(size_t)k].second,
              names[k].toStdString());
        check(!looksLikeId(names[k], want[(size_t)k].first), "effects window: button " + std::to_string(k + 1) + " is not the identifier");
    }
    {
        bool said = false;
        QString text;
        for (QLabel* l : w->findChildren<QLabel*>("fxMessage"))
            if (l->isVisibleTo(w)) {
                text += l->text() + " | ";
                if (l->text().contains("VEGAS Cookie Cutter")) said = true;
            }
        // Reno has a built-in Cookie Cutter, so it is not missing and there may be no message
        bool standIn = false;
        for (const auto& b : ofx::builtinInfos())
            if (b.id == "com.vegascreativesoftware:cookiecutter") standIn = true;
        check(said || (standIn && !text.contains("cookiecutter")), "effects window: the missing VEGAS effect's message names it by label", text.toStdString());
        check(!text.contains("com.vegascreativesoftware:cookiecutter"), "effects window: the message is not the identifier");
    }
    uitest::shot(w, out + "/fxnames_window.png");
    // other places: fxSlotLabel of each slot, used by the timeline tooltips and envelope names
    if (const Event* e = app.project().findEvent(vt.id))
        for (size_t k = 0; k < e->fx.size() && k < want.size(); ++k)
            check(fxSlotLabel(e->fx[k]) == want[k].second, "fxSlotLabel " + want[k].first, fxSlotLabel(e->fx[k]));

    // ------------------------------------------------------------------ the Plug-In Chooser
    {
        const FxChain chain = app.project().findEvent(vt.id) ? app.project().findEvent(vt.id)->fx : FxChain();
        PluginChooser c(ChooserKind::VideoFx, "Plug-In Chooser - Video Event FX", chain);
        c.resize(900, 600);
        c.show();
        uitest::settle(300);
        QStringList chips;
        for (QToolButton* b : c.findChildren<QToolButton*>("chooserChainItem"))
            if (b->isVisibleTo(&c)) chips << b->text();
        check(chips.size() == (int)want.size(), "chooser: the chain's effects", std::to_string(chips.size()));
        for (int k = 0; k < chips.size() && k < (int)want.size(); ++k)
            check(chips[k] == QString::fromStdString(want[(size_t)k].second), "chooser chain: " + want[(size_t)k].second, chips[k].toStdString());
        c.openFolder("All");
        uitest::settle(100);
        const QStringList listed = c.listed();
        check(listed.contains("Test Invert") && listed.contains("Reno Resource & Label") && listed.contains("Unnamed Effect 3D"),
              "chooser list: the plug-ins by label", listed.join(", ").left(300).toStdString());
        bool anyId = false;
        for (const QString& l : listed)
            if (l.startsWith("reno.") || l.startsWith("com.")) anyId = true;
        check(!anyId, "chooser list: no identifiers", listed.join(", ").left(300).toStdString());
        uitest::shot(&c, out + "/fxnames_chooser.png");
        c.reject();
    }

    // ------------------------------------------------------------------ a VEGAS project
    if (!veg.empty()) {
        std::string e2;
        check(app.open(veg, &e2), "VEGAS project opens", e2);
        uitest::settle(500);
        int n = 0, asId = 0;
        std::string firstBad;
        FxTarget first;
        app.project().forEachFx([&](const FxSlot& s) {
            if (s.kind != "ofx") return;
            ++n;
            const std::string l = fxSlotLabel(s);
            if (l == s.pluginId || l.find(':') != std::string::npos || l.empty()) {
                ++asId;
                if (firstBad.empty()) firstBad = s.pluginId + " -> " + l;
            }
        });
        check(n > 0, "VEGAS project: has OpenFX effects", std::to_string(n));
        check(asId == 0, "VEGAS project: every OpenFX effect shown by a label", std::to_string(asId) + " not: " + firstBad);
        // the effects window on the first event with an effect of VEGAS's own
        for (const Track& t : app.project().tracks)
            for (const Event& ev : t.events)
                for (const FxSlot& s : ev.fx)
                    if (!first.id && s.kind == "ofx" && !vegasStockFxLabel(s.pluginId).empty()) {
                        first.kind = FxTarget::Event;
                        first.id = ev.id;
                    }
        if (first.id) {
            w->showTarget(first, 0);
            uitest::settle(600);
            const QStringList vn = chainNames(w);
            bool ok = !vn.isEmpty();
            for (const QString& s : vn)
                if (s.contains(':') || s.startsWith("com.")) ok = false;
            check(ok, "VEGAS project: the effects window's buttons are labels", vn.join(" | ").toStdString());
            uitest::shot(w, out + "/fxnames_veg.png");
        }
    }
    w->close();
    delete w;
    return uitest::summary();
}
