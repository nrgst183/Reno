// VEGAS dialog parity (besides Customize Keyboard, in keyboard_test): Event Properties' switches (Mute, Reduce interlace flicker,
// resample, Conform to Project Frame Rate, Normalize with gain and Recalculate) and Set Default Track Properties
// (volume, height, effects, compositing mode; Restore original defaults).
//
//   dialogs_test <output folder>
//
// Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/editdialogs.h"
#include "../../src/app/trackdefaults.h"
#include "../../src/app/smalldialogs.h"
#include "../../src/app/mainwindow.h"
#include <QAction>
#include <QComboBox>
#include <QSpinBox>
#include "uitestlib.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTimer>
#include <cmath>
#include <functional>
#include <algorithm>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
bool near(double a, double b, double tol = 1e-6) { return std::fabs(a - b) <= tol; }
template <class T>
T* child(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }
const Event* ev(uint64_t id) { return AppState::get().project().findEvent(id); }
const Track* trackOf(uint64_t id)
{
    for (const Track& t : AppState::get().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

void whenModal(std::function<void(QWidget*)> fn)
{
    QTimer::singleShot(150, [fn] {
        QWidget* d = QApplication::activeModalWidget();
        fn(d);
        if (d && d->isVisible()) d->close();
    });
}

void clickOk(QWidget* d)
{
    if (QPushButton* ok = child<QPushButton>(d, "ok")) {
        ok->click();
        return;
    }
    if (QDialogButtonBox* bb = d ? d->findChild<QDialogButtonBox*>() : nullptr) bb->button(QDialogButtonBox::Ok)->click();
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoDialogsTest");
    QApplication::setApplicationName("dialogs_test");
    QSettings().clear();
    applyTheme();
    const QStringList args = QApplication::arguments();
    gOut = args.size() > 1 ? args[1] : QString("dialogs");
    QDir().mkpath(gOut);
    uitest::setup({}, {});

    AppState& app = AppState::get();
    app.newProject();
    app.edit("Small Project", [](Project& p) {
        p.settings.width = 640;
        p.settings.height = 360;
        p.settings.fpsNum = 60;  // the made-up clip is 30 fps: Conform makes it play at 2x
        p.settings.fpsDen = 1;
    });
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    uint64_t V = 0, A = 0, AT = 0, VT = 0;
    for (uint64_t id : placed) {
        const Track* t = nullptr;
        app.project().findEvent(id, &t);
        if (t && t->type == TrackType::Video) {
            V = id;
            VT = t->id;
        } else {
            A = id;
            AT = t ? t->id : 0;
        }
    }
    check(V && A && AT && VT, "setup: a clip with picture and sound");

    // ================================================================== Video Event: switches, resample, conform
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "eventPropertiesDialog", "video event properties open");
        if (!d) return;
        QCheckBox* mute = child<QCheckBox>(d, "mute");
        QCheckBox* flicker = child<QCheckBox>(d, "reduceFlicker");
        QRadioButton* project = child<QRadioButton>(d, "resample_project");
        QRadioButton* force = child<QRadioButton>(d, "resample_force");
        QPushButton* conform = child<QPushButton>(d, "conformRate");
        check(mute && flicker && project && force && child<QRadioButton>(d, "resample_off") && conform,
              "Video Event has Mute, Reduce interlace flicker, the resample switches and Conform to Project Frame Rate");
        check(!child<QCheckBox>(d, "normalize"), "... and no Normalize (audio only)");
        if (!(mute && flicker && project && force && conform)) return;
        check(!mute->isChecked() && !flicker->isChecked() && project->isChecked(), "they show the event's settings");
        mute->setChecked(true);
        flicker->setChecked(true);
        force->setChecked(true);
        conform->click();
        check(near(child<QDoubleSpinBox>(d, "playbackRate")->value(), 2.0), "Conform to Project Frame Rate: 30 fps media in a 60 fps project plays at 2",
              std::to_string(child<QDoubleSpinBox>(d, "playbackRate")->value()));
        uitest::shot(d, gOut + "/video_event_properties.png");
        clickOk(d);
    });
    runEventProperties(nullptr, V);
    {
        const Event* e = ev(V);
        check(e && e->mute && e->reduceFlicker && e->resample == "force" && near(e->rate, 2.0), "OK sets mute, flicker, resample and the rate",
              e ? e->resample : "");
        check(app.undoName() == "Event Properties", "one undo step");
        app.undo();
        e = ev(V);
        check(e && !e->mute && !e->reduceFlicker && e->resample != "force" && near(e->rate, 1.0), "undo puts them back");
    }
    // nothing changed: no undo step
    {
        const std::string before = app.undoName();
        whenModal([&](QWidget* d) { clickOk(d); });
        runEventProperties(nullptr, V);
        check(app.undoName() == before, "OK with nothing changed adds no undo step", app.undoName());
    }

    // ================================================================== Audio Event: Normalize, Recalculate, Mute
    const double peak = app.eventPeak(*ev(A));
    whenModal([&](QWidget* d) {
        check(d != nullptr, "audio event properties open");
        if (!d) return;
        QCheckBox* norm = child<QCheckBox>(d, "normalize");
        QLabel* gain = child<QLabel>(d, "normalizeGain");
        QPushButton* recalc = child<QPushButton>(d, "recalculate");
        check(norm && gain && recalc && child<QCheckBox>(d, "mute") && !child<QCheckBox>(d, "reduceFlicker") && !child<QPushButton>(d, "conformRate"),
              "Audio Event has Mute and Normalize with its gain and Recalculate (and none of the video switches)");
        if (!(norm && gain && recalc)) return;
        norm->setChecked(true);
        const double want = 20.0 * std::log10(AppState::normalizeLevel() / peak);
        check(gain->text().startsWith("Gain: ") && std::fabs(gain->text().mid(6).remove(" dB").toDouble() - want) < 0.02,
              "ticking Normalize measures the event: the gain it will get is shown", gain->text().toStdString() + " want " + std::to_string(want));
        recalc->click();
        check(std::fabs(gain->text().mid(6).remove(" dB").toDouble() - want) < 0.02, "Recalculate measures it again");
        child<QCheckBox>(d, "mute")->setChecked(true);
        uitest::shot(d, gOut + "/audio_event_properties.png");
        clickOk(d);
    });
    runEventProperties(nullptr, A);
    {
        const Event* e = ev(A);
        check(e && e->normalize && e->mute && peak > 0 && near(e->normalizeGain, AppState::normalizeLevel() / peak, 1e-6),
              "OK turns Normalize on with the measured gain, and mutes", e ? std::to_string(e->normalizeGain) : "");
    }
    // and off again
    whenModal([&](QWidget* d) {
        child<QCheckBox>(d, "normalize")->setChecked(false);
        clickOk(d);
    });
    runEventProperties(nullptr, A);
    check(ev(A) && !ev(A)->normalize && near(ev(A)->normalizeGain, 1.0), "Normalize off: back to the event's own level");

    // ================================================================== Preferences > Audio > Normalize peak level
    whenModal([&](QWidget* d) {
        QDoubleSpinBox* peak = child<QDoubleSpinBox>(d, "normalizePeakDb");
        check(peak && near(peak->value(), -0.1, 1e-9), "Preferences > Audio has Normalize peak level, at VEGAS's -0.1 dB");
        if (peak) peak->setValue(-3.0);
        clickOk(d);
    });
    runPreferences(nullptr);
    check(near(AppState::normalizeLevel(), std::pow(10.0, -3.0 / 20.0), 1e-9), "Normalize now goes to -3 dB", std::to_string(AppState::normalizeLevel()));
    app.setNormalize({A}, true);
    check(ev(A) && near(ev(A)->normalizeGain * peak, std::pow(10.0, -3.0 / 20.0), 1e-6), "Edit > Switches > Normalize uses it");
    app.setNormalize({A}, false);
    QSettings().remove("prefs/normalizePeakDb");

    // ================================================================== Set Default Track Properties
    {
        QMenu menu;
        addTrackDefaultsItem(&menu, AT, nullptr);
        bool found = false;
        for (QAction* a : menu.actions()) found = found || a->objectName() == "setDefaultTrackProperties";
        check(found, "a track's menu gets Set Default Track Properties...");
    }
    installTrackDefaults();
    app.edit("Track settings", [&](Project& p) {
        Track* t = p.findTrack(AT);
        t->level = 0.5;
        t->height = 90;
        FxSlot s;
        s.id = p.newId();
        s.kind = "audio";
        s.name = "Test EQ";
        t->fx.push_back(s);
        Track* v = p.findTrack(VT);
        v->composite = "add";
        v->height = 120;
    });
    whenModal([&](QWidget* d) {
        check(d && d->objectName() == "setDefaultTrackProperties", "the dialog opens");
        if (!d) return;
        QCheckBox* vol = child<QCheckBox>(d, "audio_volume");
        QCheckBox* h = child<QCheckBox>(d, "audio_height");
        QCheckBox* fx = child<QCheckBox>(d, "audio_fx");
        QCheckBox* vcomp = child<QCheckBox>(d, "video_composite");
        QCheckBox* pantype = child<QCheckBox>(d, "audio_pantype");
        check(vol && h && fx && vcomp && pantype && vol->isEnabled() && vol->isChecked() && !vcomp->isEnabled() && pantype->isEnabled(),
              "for an audio track: the audio boxes (ticked) and the video ones greyed");
        if (h) h->setChecked(false);
        uitest::shot(d, gOut + "/set_default_track_properties.png");
        clickOk(d);
    });
    runSetDefaultTrackProperties(nullptr, AT);
    check(savedTrackDefaults(false) == QStringList({"volume", "pantype", "fx", "automation"}), "the ticked properties are saved", savedTrackDefaults(false).join(",").toStdString());
    {
        const uint64_t na = app.addTrack(TrackType::Audio);
        const Track* t = trackOf(na);
        check(t && near(t->level, 0.5) && t->height == 0 && t->fx.size() == 1 && t->fx[0].name == "Test EQ" && t->fx[0].id != trackOf(AT)->fx[0].id,
              "a new audio track starts with the saved volume and effects (new ids), not the unticked height");
        const uint64_t nv = app.addTrack(TrackType::Video);
        check(trackOf(nv) && trackOf(nv)->composite == "srcalpha" && trackOf(nv)->height == 0, "video tracks are not affected by audio defaults");
    }
    whenModal([&](QWidget* d) { clickOk(d); });
    runSetDefaultTrackProperties(nullptr, VT);
    {
        const uint64_t nv = app.addTrack(TrackType::Video);
        check(trackOf(nv) && trackOf(nv)->composite == "add" && trackOf(nv)->height == 120, "video defaults: compositing mode and height");
    }
    whenModal([&](QWidget* d) {
        child<QCheckBox>(d, "restoreDefaults")->setChecked(true);
        clickOk(d);
    });
    runSetDefaultTrackProperties(nullptr, AT);
    {
        const uint64_t na = app.addTrack(TrackType::Audio);
        check(savedTrackDefaults(false).isEmpty() && trackOf(na) && near(trackOf(na)->level, 1.0) && trackOf(na)->fx.empty(),
              "Restore original defaults: new audio tracks are plain again");
    }

    // ---- Paste Repeat (Ctrl+B)
    {
        MainWindow w;
        w.resize(1200, 800);
        w.show();
        uitest::settle(200);
        app.setSelection({V});
        uitest::settle(20);
        // Edit > Copy, so Paste Repeat is enabled as after a copy
        if (QAction* copy = w.findChild<QAction*>("copy")) copy->trigger();
        app.setCursor(20.0);
        bool seen = false;
        pasteRepeatDialogShown = [&](QWidget* d) {
            QTimer::singleShot(80, [&seen, d] {
                seen = true;
                child<QSpinBox>(d, "times")->setValue(3);
                child<QRadioButton>(d, "evenSpacing")->setChecked(true);
                child<QDoubleSpinBox>(d, "every")->setValue(15.0);
                child<QComboBox>(d, "unit")->setCurrentIndex(0);
                uitest::shot(d, gOut + "/paste_repeat.png");
                if (QDialogButtonBox* bb = d->findChild<QDialogButtonBox*>()) bb->button(QDialogButtonBox::Ok)->click();
            });
        };
        QAction* pr = w.findChild<QAction*>("pasteRepeat");
        if (pr) pr->trigger();
        uitest::settle(50);
        std::vector<double> starts;
        for (const Track& t : app.project().tracks)
            if (t.id == VT)
                for (const Event& e : t.events)
                    if (e.start > 19.0) starts.push_back(e.start);
        std::sort(starts.begin(), starts.end());
        check(seen && starts.size() == 3 && near(starts[0], 20.0, 1e-6) && near(starts[1], 35.0, 1e-6) && near(starts[2], 50.0, 1e-6),
              "Paste Repeat with even spacing: three copies 15 s apart from the cursor", std::to_string(starts.size()));
        pasteRepeatDialogShown = nullptr;
        check(QSettings().value("pasteRepeat/even").toBool() && QSettings().value("pasteRepeat/times").toInt() == 3, "the answers are offered next time");
    }
    return uitest::summary();
}
