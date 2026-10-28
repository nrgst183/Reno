// Effects window (src/app/fxwindow.cpp), without a screen.
//
//   fxwindow_test <output folder> <testfx.ofx> [effects.ofx] [--vst file.so ...]
//
// Pass the test plug-in from tests/ui/fxwindow_testvst.cpp among the --vst files (its name must contain "fxwintest"). With
// -DRENO_FX_HOST_PLUGIN_WINDOWS the window also goes through embedding plug-in windows (normally Windows only), using plug-ins
// that only pretend to make one. Prints PASS / FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/fxwindow.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/playback.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QTimer>
#include <QPlainTextEdit>
#include <QPointer>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <dlfcn.h>
#endif

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;
FxWindow* gWin = nullptr;

void settle(int ms = 120)
{
    uitest::settle(ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Widgets of the window that are really there (not hidden ones on their way out).
template <class T>
std::vector<T*> all(const char* name)
{
    std::vector<T*> out;
    for (T* w : gWin->findChildren<T*>(name))
        if (w->isVisibleTo(gWin)) out.push_back(w);
    return out;
}

template <class T>
T* one(const char* name)
{
    std::vector<T*> v = all<T>(name);
    return v.empty() ? nullptr : v[0];
}

// The row of an OpenFX parameter (shown or not), and a control in it.
QWidget* row(const char* param)
{
    for (QWidget* w : gWin->findChildren<QWidget*>("fxRow"))
        if (w->property("param").toString() == param && w->window() == gWin) return w;
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

QString labelOf(const char* param)
{
    QWidget* l = in<QWidget>(param, "fxLabel");
    return l ? l->accessibleName() : QString();
}

QAbstractButton* groupHeader(const char* param)
{
    for (QAbstractButton* b : gWin->findChildren<QAbstractButton*>("fxGroup"))
        if (b->property("param").toString() == param) return b;
    return nullptr;
}

QWidget* chainButton(uint64_t slot)
{
    for (QWidget* b : all<QWidget>("fxChainButton"))
        if (b->property("slotId").toULongLong() == slot) return b;
    return nullptr;
}

QAction* menuAction(const QString& text)
{
    QMenu* menu = gWin->findChild<QMenu*>("fxAddMenu");
    if (!menu) return nullptr;
    QMetaObject::invokeMethod(menu, "aboutToShow");  // fills itself as when clicked
    for (QAction* a : menu->findChildren<QAction*>())
        if (a->text() == text) return a;
    return nullptr;
}

std::vector<uint64_t> chainIds(const FxTarget& t)
{
    std::vector<uint64_t> v;
    if (const FxChain* c = AppState::get().chainOf(AppState::get().project(), t))
        for (const FxSlot& s : *c) v.push_back(s.id);
    return v;
}

const FxSlot* slotOf(const FxTarget& t, uint64_t id)
{
    if (const FxChain* c = AppState::get().chainOf(AppState::get().project(), t))
        for (const FxSlot& s : *c)
            if (s.id == id) return &s;
    return nullptr;
}

std::vector<double> valueOf(uint64_t slot, const char* param, double time = 0)
{
    ofx::Instance* inst = AppState::get().fx.video(slot);
    ofx::Param* p = inst ? inst->params.find(param) : nullptr;
    if (!p) return std::vector<double>();
    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
    return p->valueAt(time);
}

std::string strOf(uint64_t slot, const char* param)
{
    ofx::Instance* inst = AppState::get().fx.video(slot);
    ofx::Param* p = inst ? inst->params.find(param) : nullptr;
    return p ? p->str : std::string("<none>");
}

size_t keysOf(uint64_t slot, const char* param)
{
    ofx::Instance* inst = AppState::get().fx.video(slot);
    ofx::Param* p = inst ? inst->params.find(param) : nullptr;
    return p ? p->keys.size() : 0;
}

bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }
std::string num(double v) { return QString::number(v, 'g', 10).toStdString(); }

QPoint centre(QWidget* w) { return QRect(QPoint(0, 0), w->size()).center(); }

// ---- what the test effects say happened to them
struct TestFxLog {
    int (*count)() = nullptr;
    const char* (*line)(int) = nullptr;
    void (*clear)() = nullptr;
    bool ok() const { return count && line && clear; }
    bool has(const std::string& what) const
    {
        if (!ok()) return false;
        for (int k = 0, n = count(); k < n; ++k)
            if (std::string(line(k)).find(what) != std::string::npos) return true;
        return false;
    }
    int countOf(const std::string& what) const
    {
        int c = 0;
        if (!ok()) return 0;
        for (int k = 0, n = count(); k < n; ++k)
            if (std::string(line(k)).find(what) != std::string::npos) ++c;
        return c;
    }
};

struct TestVst {
    int (*opens)() = nullptr;
    int (*closes)() = nullptr;
    int (*idles)() = nullptr;
    int (*openNow)() = nullptr;
    void* (*parent)() = nullptr;
    float (*param)(int) = nullptr;
    void (*move)(int, float) = nullptr;
    void (*resize)(int, int) = nullptr;
    bool ok() const { return opens && closes && idles && openNow && parent && param && move && resize; }
};

template <class F>
void sym(void* lib, const char* name, F& f)
{
#ifndef _WIN32
    f = lib ? (F)dlsym(lib, name) : nullptr;
#else
    (void)lib; (void)name; f = nullptr;
#endif
}

void* openLoaded(const std::string& path)
{
#ifndef _WIN32
    return dlopen(path.c_str(), RTLD_NOW);  // already loaded by the host: finds that copy
#else
    (void)path;
    return nullptr;
#endif
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTest");
    QApplication::setApplicationName("fxwindow_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 3) { qWarning("usage: fxwindow_test <output folder> <testfx.ofx> [effects.ofx] [--vst file ...]"); return 2; }
    gOut = args[1];
    QDir().mkpath(gOut);
    std::vector<std::string> ofxFiles, vstFiles;
    bool vst = false;
    for (int k = 2; k < args.size(); ++k) {
        if (args[k] == "--vst") { vst = true; continue; }
        (vst ? vstFiles : ofxFiles).push_back(args[k].toStdString());
    }
    std::string testVstPath;
    for (const auto& f : vstFiles)
        if (f.find("fxwintest") != std::string::npos) testVstPath = f;
    uitest::setup(ofxFiles, vstFiles);

    AppState& app = AppState::get();
    app.newProject();
    std::string err;
    const uint64_t media = app.importMedia("synth:1", &err);
    const std::vector<uint64_t> placed = app.addMediaToTimeline(media, 0.0);
    FxTarget vt, at;
    for (uint64_t id : placed) {
        FxTarget t;
        t.kind = FxTarget::Event;
        t.id = id;
        (app.targetIsVideo(t) ? vt : at) = t;
    }
    check(vt.id && at.id, "setup: a video event and an audio event");
    app.setCursor(1.0);

    Playback playback;
    FxWindow* w = new FxWindow(&playback, nullptr);
    gWin = w;
    w->resize(560, 700);

    TestFxLog fxlog;
    if (void* lib = openLoaded(ofxFiles[0])) {
        sym(lib, "testfx_log_count", fxlog.count);
        sym(lib, "testfx_log_line", fxlog.line);
        sym(lib, "testfx_log_clear", fxlog.clear);
    }

    // ------------------------------------------------------------------ an empty chain
    w->showTarget(vt, 0);
    w->show();
    settle();
    check(w->windowTitle().startsWith("Video Event FX: "), "title names an event's video effects", w->windowTitle().toStdString());
    check(w->target() == vt, "target() is what was shown");
    {
        QLabel* m = one<QLabel>("fxMessage");
        check(m && m->text().contains("No effects"), "empty chain: says there are none", m ? m->text().toStdString() : "no message");
        check(!one<QToolButton>("fxRemove")->isEnabled(), "empty chain: nothing to remove");
    }
    uitest::shot(w, gOut + "/fxwindow_empty.png");

    // ------------------------------------------------------------------ adding from the menu
    QAction* addParams = menuAction("Test Parameters");
    check(addParams != nullptr, "add menu lists the test effect");
    check(menuAction("Test Generator") == nullptr, "add menu leaves out generators");
    check(menuAction("Fail Describe") == nullptr && menuAction("Test Fail Describe") == nullptr, "add menu leaves out effects that do not work");
    addParams = menuAction("Test Parameters");  // the menu is rebuilt each time it opens
    if (addParams) addParams->trigger();
    settle();
    std::vector<uint64_t> ids = chainIds(vt);
    check(ids.size() == 1 && slotOf(vt, ids[0])->pluginId == "reno.test.params", "add: the chain has the new effect");
    const uint64_t P = ids.empty() ? 0 : ids[0];
    check(one<QWidget>("fxOfxPanel") != nullptr, "add: its controls are shown");
    check(chainButton(P) != nullptr, "add: it has a button in the chain");
    check(fxlog.has("params: beginEdit"), "the effect is told its controls are up (beginEdit)");
    {
        ofx::Instance* inst = app.fx.video(P);
        check(inst && inst->props.getInt(kOfxPropIsInteractive) == 1, "the instance is marked interactive while shown");
    }

    // ------------------------------------------------------------------ the rows
    check(row("p_custom") == nullptr && row("p_page") == nullptr, "custom and page parameters get no row");
    check(labelOf("p_int") == "Integer Value", "rows show the label", labelOf("p_int").toStdString());
    check(row("p_int") && row("p_int")->toolTip().contains("An integer from 0 to 100"), "rows have the hint as a tool tip");
    QAbstractButton* group = groupHeader("p_group");
    check(group && !group->isChecked() && !row("p_int")->isVisibleTo(w), "a group the effect says starts closed starts closed");
    if (group) group->click();
    settle();
    check(row("p_int") && row("p_int")->isVisibleTo(w), "clicking a group's title opens it");
    check(group && group->text() == "A Group", "group shows its label");
    QPointer<QWidget> intRow = row("p_int");
    uitest::shot(w, gOut + "/fxwindow_params.png");
    {
        // the whole panel, for looking at
        const QSize was = w->size();
        w->resize(600, 1180);
        settle();
        uitest::shot(w, gOut + "/fxwindow_params_tall.png");
        w->resize(was);
        settle();
    }

    // double: slider and number box
    QSlider* dblSlider = in<QSlider>("p_dbl", "fxSlider");
    QDoubleSpinBox* dblBox = in<QDoubleSpinBox>("p_dbl", "fxBox");
    check(dblSlider && dblBox, "double: slider and number box");
    if (dblSlider && dblBox) {
        check(dblSlider->maximum() == 1000, "double: slider steps from the increment over the display range", std::to_string(dblSlider->maximum()));
        check(dblBox->decimals() == 3, "double: decimals from the effect's digits", std::to_string(dblBox->decimals()));
        check(near(dblBox->minimum(), -10) && near(dblBox->maximum(), 10), "double: number box limited to the effect's min/max");
        dblSlider->setValue(750);
        check(near(valueOf(P, "p_dbl")[0], 2.5), "double: moving the slider sets the value at once", num(valueOf(P, "p_dbl")[0]));
        check(near(dblBox->value(), 2.5), "double: the number box follows the slider");
        dblBox->setValue(-1.234);
        check(near(valueOf(P, "p_dbl")[0], -1.234), "double: the number box sets the value", num(valueOf(P, "p_dbl")[0]));
        settle();
        check(dblSlider->value() == 377, "double: the slider follows the number box", std::to_string(dblSlider->value()));
        in<QToolButton>("p_dbl", "fxReset")->click();
        settle();
        check(near(valueOf(P, "p_dbl")[0], 0.25) && near(dblBox->value(), 0.25), "double: reset button goes back to the default");
    }
    // integer
    QSlider* intSlider = in<QSlider>("p_int", "fxSlider");
    QDoubleSpinBox* intBox = in<QDoubleSpinBox>("p_int", "fxBox");
    if (intSlider && intBox) {
        check(intSlider->maximum() == 80 && intBox->decimals() == 0, "integer: slider over the display range 10..90, whole numbers");
        intSlider->setValue(20);
        check(near(valueOf(P, "p_int")[0], 30), "integer: slider sets the value", num(valueOf(P, "p_int")[0]));
        intBox->setValue(77);
        check(near(valueOf(P, "p_int")[0], 77), "integer: number box sets the value");
        uitest::mouseDoubleClick(intSlider, centre(intSlider));
        settle();
        check(near(valueOf(P, "p_int")[0], 5), "integer: double-clicking the slider goes back to the default", num(valueOf(P, "p_int")[0]));
    } else {
        check(false, "integer: slider and number box");
    }
    // boolean, choice
    QCheckBox* flag = in<QCheckBox>("p_bool", "fxCheck");
    check(flag && flag->isChecked(), "boolean: tick box shows the default (on)");
    if (flag) flag->setChecked(false);
    check(near(valueOf(P, "p_bool")[0], 0), "boolean: clearing the tick sets it off");
    QComboBox* choice = in<QComboBox>("p_choice", "fxCombo");
    check(choice && choice->count() == 4 && choice->currentIndex() == 1 && choice->itemText(2) == "Two", "choice: list of the options, default picked");
    if (choice) choice->setCurrentIndex(3);
    check(near(valueOf(P, "p_choice")[0], 3), "choice: picking an option sets it");
    // colours
    QAbstractButton* rgb = in<QAbstractButton>("p_rgb", "fxColour");
    check(rgb != nullptr, "colour: a colour button");
    if (rgb) {
        rgb->click();
        settle();
        QColorDialog* dlg = w->findChild<QColorDialog*>("fxColourDialog");
        check(dlg && dlg->isVisible() && !dlg->testOption(QColorDialog::ShowAlphaChannel), "colour: the button opens a colour dialog (no alpha for RGB)");
        if (dlg) {
            dlg->setCurrentColor(QColor::fromRgbF(1.f, 0.f, 0.5f));
            const std::vector<double> v = valueOf(P, "p_rgb");
            check(near(v[0], 1, 3e-3) && near(v[1], 0, 3e-3) && near(v[2], 0.5, 3e-3), "colour: the value follows the dialog as it changes");
            dlg->reject();
            settle();
            const std::vector<double> b = valueOf(P, "p_rgb");
            check(near(b[0], 0.1, 1e-3) && near(b[1], 0.2, 1e-3) && near(b[2], 0.3, 1e-3), "colour: cancelling puts the colour back");
        }
    }
    QAbstractButton* rgba = in<QAbstractButton>("p_rgba", "fxColour");
    if (rgba) {
        rgba->click();
        settle();
        QColorDialog* dlg = w->findChild<QColorDialog*>("fxColourDialog");
        check(dlg && dlg->testOption(QColorDialog::ShowAlphaChannel), "colour: RGBA gets an alpha control");
        if (dlg) {
            dlg->setCurrentColor(QColor::fromRgbF(0.f, 1.f, 0.f, 0.5f));
            dlg->accept();
            settle();
            const std::vector<double> v = valueOf(P, "p_rgba");
            check(near(v[1], 1, 3e-3) && near(v[3], 0.5, 3e-3), "colour: RGBA value with alpha", num(v[3]));
        }
    }
    // 2D / 3D
    if (QDoubleSpinBox* y = in<QDoubleSpinBox>("p_d2", "fxBox", 1)) {
        y->setValue(-3.5);
        const std::vector<double> v = valueOf(P, "p_d2");
        check(near(v[0], 1.5) && near(v[1], -3.5), "2D: second number box sets the second value");
    } else {
        check(false, "2D: two number boxes");
    }
    check(row("p_d2") && row("p_d2")->findChildren<QDoubleSpinBox*>("fxBox").size() == 2, "2D: exactly two number boxes");
    if (QDoubleSpinBox* z = in<QDoubleSpinBox>("p_i3", "fxBox", 2)) {
        z->setValue(4);
        const std::vector<double> v = valueOf(P, "p_i3");
        check(near(v[0], 7) && near(v[1], 8) && near(v[2], 4), "3D integer: third box sets the third value");
    } else {
        check(false, "3D integer: three number boxes");
    }
    {
        QList<QLabel*> dims = row("p_d3") ? row("p_d3")->findChildren<QLabel*>() : QList<QLabel*>();
        bool xyz = false;
        for (QLabel* l : dims) if (l->text() == "z") xyz = true;
        check(xyz, "3D: dimension labels shown");
    }
    // text and button
    QPlainTextEdit* text = in<QPlainTextEdit>("p_str", "fxText");
    check(text && text->toPlainText() == "hello", "multi-line string: a text box with the value");
    if (text) text->setPlainText("abc\ndef");
    check(strOf(P, "p_str") == "abc\ndef", "multi-line string: typing sets it", strOf(P, "p_str"));
    if (QAbstractButton* push = in<QAbstractButton>("p_button", "fxPush")) {
        check(push->text() == "Do It", "push button: labelled");
        push->click();
        settle();
        check(strOf(P, "p_str") == "pushed 1" && text && text->toPlainText() == "pushed 1", "push button: the effect's answer shows in another control");
    } else {
        check(false, "push button: a button");
    }

    // ------------------------------------------------------------------ the effect changing its own controls
    QDoubleSpinBox* driver = in<QDoubleSpinBox>("driver", "fxBox");
    if (driver) {
        driver->setValue(3);
        settle();
        QDoubleSpinBox* slave = in<QDoubleSpinBox>("slave", "fxBox");
        check(slave && near(slave->value(), 1.5), "effect-driven: another parameter's new value is shown", slave ? num(slave->value()) : "");
        check(row("dep") && row("dep")->isEnabled() && row("dep")->isVisibleTo(w), "effect-driven: enabled state shown");
        check(choice && choice->count() == 3 && choice->itemText(0) == "Opt3_0", "effect-driven: a choice's new options are shown",
              choice ? std::to_string(choice->count()) : "");
        driver->setValue(12);
        settle();
        check(row("dep") && !row("dep")->isVisibleTo(w), "effect-driven: a parameter made secret is hidden");
        check(choice && choice->count() == 2, "effect-driven: the options change again");
        driver->setValue(0);
        settle();
        check(row("dep") && row("dep")->isVisibleTo(w) && !row("dep")->isEnabled(), "effect-driven: shown again, greyed out");
    } else {
        check(false, "driver: number box");
    }
    check(!intRow.isNull() && intRow == row("p_int"), "the controls were brought up to date, not made again");
    {
        // a property change that comes with no value change is picked up by the slow refresh
        ofx::Instance* inst = app.fx.video(P);
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            inst->params.find("p_bool")->props.setString(kOfxPropLabel, "Renamed Flag");
        }
        settle(450);
        check(labelOf("p_bool") == "Renamed Flag", "a new label shows up without a value changing", labelOf("p_bool").toStdString());
    }

    // ------------------------------------------------------------------ not fighting the user
    if (dblSlider) {
        ofx::Instance* inst = app.fx.video(P);
        const int before = dblSlider->value();
        dblSlider->setSliderDown(true);
        inst->userSetValue(inst->params.find("p_dbl"), std::vector<double>(1, 1.0), 30);
        settle();
        check(dblSlider->value() == before && near(dblBox->value(), 1.0), "a slider held by the user is not moved by a refresh (its box is)");
        dblSlider->setSliderDown(false);
        app.notify(kFxParams);
        settle();
        check(dblSlider->value() == 600, "once let go, the slider shows the value", std::to_string(dblSlider->value()));
    }

    // ------------------------------------------------------------------ keyframes
    QToolButton* key = in<QToolButton>("p_dbl", "fxKey");
    check(key && !key->isChecked(), "keyframes: a toggle per animatable row, off to begin with");
    check(in<QToolButton>("p_bool", "fxKey") == nullptr && in<QToolButton>("p_str", "fxKey") == nullptr, "keyframes: no toggle where the effect cannot animate");
    if (key && dblBox) {
        app.setCursor(1.0);                         // frame 30 of the event
        settle();
        key->click();
        settle();
        check(keysOf(P, "p_dbl") == 1 && key->isChecked() && key->text() == "1", "keyframes: switching on sets a keyframe at the cursor");
        app.setCursor(2.0);                         // frame 60
        settle();
        dblBox->setValue(4.0);
        settle();
        check(keysOf(P, "p_dbl") == 2 && key->text() == "2", "keyframes: an edit elsewhere adds a keyframe", std::to_string(keysOf(P, "p_dbl")));
        app.setCursor(1.0);
        settle();
        check(near(dblBox->value(), 1.0), "keyframes: the control shows the value at the cursor", num(dblBox->value()));
        app.setCursor(1.5);
        settle();
        check(near(dblBox->value(), 2.5), "keyframes: ... in between keyframes too", num(dblBox->value()));
        key->click();
        settle();
        check(keysOf(P, "p_dbl") == 0 && !key->isChecked() && key->text().isEmpty(), "keyframes: switching off removes them");
        check(near(valueOf(P, "p_dbl")[0], 2.5), "keyframes: ... keeping the value at the cursor", num(valueOf(P, "p_dbl")[0]));
        app.setCursor(1.0);
        settle();
    }

    // ------------------------------------------------------------------ undo of settings
    // each change in the window is one undo step; undoing restores the effect's settings and shows them
    if (dblSlider && dblBox && flag && key) {
        auto savedDbl = [&]() -> double {           // what the project's copy of the settings says
            const FxSlot* s = app.project().findEvent(vt.id) ? slotOf(vt, P) : nullptr;
            if (!s) return -999;
            const Json& e = s->state.get("params").get("p_dbl").get("v");
            return e.size() ? e.at(0).asDouble() : -999;
        };
        const double was = valueOf(P, "p_dbl")[0];
        dblSlider->setSliderDown(true);  // a drag: pressed, moved, released
        dblSlider->setValue(700);
        dblSlider->setValue(800);
        dblSlider->setValue(900);
        dblSlider->setSliderDown(false);
        settle();
        check(near(valueOf(P, "p_dbl")[0], 4.0) && app.canUndo() && app.undoName() == "Change \"Double Value\"",
              "undo: a slider drag is one undo step named after the setting", app.undoName());
        check(near(savedDbl(), 4.0), "undo: the project's copy of the settings is up to date after the step", num(savedDbl()));
        app.undo();
        settle();
        check(near(valueOf(P, "p_dbl")[0], was) && near(dblBox->value(), was) && dblSlider->value() == 1000 * (was + 5) / 10,
              "undo: the whole drag is undone at once, and the controls show it", num(valueOf(P, "p_dbl")[0]) + " " + num(dblBox->value()));
        check(near(savedDbl(), was), "undo: ... and the project's copy agrees", num(savedDbl()));
        app.redo();
        settle();
        check(near(valueOf(P, "p_dbl")[0], 4.0) && near(dblBox->value(), 4.0), "redo: the drag is done again", num(valueOf(P, "p_dbl")[0]));
        app.undo();
        settle();
        const bool flagWas = flag->isChecked();
        flag->click();
        settle();
        check(app.undoName() == "Change \"Renamed Flag\"" && !app.canRedo(), "undo: a click is a step of its own (and clears redo)", app.undoName());
        app.undo();
        settle();
        check(flag->isChecked() == flagWas && near(valueOf(P, "p_bool")[0], flagWas ? 1 : 0), "undo: the tick box goes back");
        // typing in a number box: one step per value entered
        dblBox->setValue(-2.0);
        dblBox->setValue(-3.0);
        settle();
        app.undo();
        settle();
        check(near(valueOf(P, "p_dbl")[0], -2.0), "undo: each value entered in a box is a step", num(valueOf(P, "p_dbl")[0]));
        app.undo();
        settle();
        check(near(valueOf(P, "p_dbl")[0], was), "undo: ... back to where it was", num(valueOf(P, "p_dbl")[0]));
        // a colour chosen in the dialog, through several colours: one step
        if (rgb) {
            const std::vector<double> before = valueOf(P, "p_rgb");
            rgb->click();
            settle();
            QColorDialog* dlg = w->findChild<QColorDialog*>("fxColourDialog");
            if (dlg) {
                dlg->setCurrentColor(QColor::fromRgbF(0.f, 0.f, 1.f));
                dlg->setCurrentColor(QColor::fromRgbF(0.f, 1.f, 1.f));
                dlg->accept();
                settle();
            }
            const std::vector<double> after = valueOf(P, "p_rgb");
            app.undo();
            settle();
            const std::vector<double> v = valueOf(P, "p_rgb");
            check(dlg && near(after[1], 1, 3e-3) && near(v[0], before[0], 1e-3) && near(v[1], before[1], 1e-3) && near(v[2], before[2], 1e-3),
                  "undo: everything chosen in one visit to the colour dialog is one step");
            // the colour's number boxes
            QList<QDoubleSpinBox*> cb = row("p_rgb") ? row("p_rgb")->findChildren<QDoubleSpinBox*>("fxColourBox") : QList<QDoubleSpinBox*>();
            check(cb.size() == 3 && near(cb[0]->value(), before[0], 1e-3), "colour: number boxes for red, green and blue show the colour");
            if (cb.size() == 3) {
                cb[1]->setValue(0.75);
                settle();
                check(near(valueOf(P, "p_rgb")[1], 0.75), "colour: typing a channel sets it", num(valueOf(P, "p_rgb")[1]));
                app.undo();
                settle();
                check(near(valueOf(P, "p_rgb")[1], before[1], 1e-3) && near(cb[1]->value(), before[1], 1e-3), "undo: and the box shows the colour that comes back");
            }
            QList<QDoubleSpinBox*> cba = row("p_rgba") ? row("p_rgba")->findChildren<QDoubleSpinBox*>("fxColourBox") : QList<QDoubleSpinBox*>();
            check(cba.size() == 4, "colour with alpha: four number boxes", std::to_string(cba.size()));
        }
        // keyframes on and off are steps too; and the arrows go from one keyframe to the next
        app.setCursor(1.0);
        settle();
        key->click();                               // a keyframe at frame 30
        app.setCursor(2.0);
        settle();
        dblBox->setValue(3.0);                      // and one at frame 60
        settle();
        QToolButton* prevKey = in<QToolButton>("p_dbl", "fxPrevKey");
        QToolButton* nextKey = in<QToolButton>("p_dbl", "fxNextKey");
        check(prevKey && nextKey && prevKey->isEnabled() && !nextKey->isEnabled(), "keyframes: at the last keyframe only 'previous' can be used");
        if (prevKey) prevKey->click();
        settle();
        check(std::fabs(app.cursor() - 1.0) < 1e-6 && nextKey && nextKey->isEnabled() && !prevKey->isEnabled(), "keyframes: 'previous' goes to the keyframe before",
              num(app.cursor()));
        if (nextKey) nextKey->click();
        settle();
        check(std::fabs(app.cursor() - 2.0) < 1e-6, "keyframes: 'next' goes to the one after", num(app.cursor()));
        app.undo();                                 // the second keyframe
        app.undo();                                 // the first
        settle();
        check(keysOf(P, "p_dbl") == 0 && !key->isChecked(), "undo: keyframes added are taken away again", std::to_string(keysOf(P, "p_dbl")));
        app.setCursor(1.0);
        settle();
        // typing: one step per burst of typing (it ends after a pause, or when the controls go away)
        if (text) {
            const std::string before = strOf(P, "p_str");
            text->setPlainText("typed 1");
            text->setPlainText("typed 12");
            settle(1100);
            check(app.undoName() == "Type in \"Text\"" && strOf(P, "p_str") == "typed 12", "undo: a burst of typing is one step", app.undoName());
            app.undo();
            settle();
            check(strOf(P, "p_str") == before && text->toPlainText() == QString::fromStdString(before), "undo: the text goes back", strOf(P, "p_str"));
            text->setPlainText("typed 3");
            w->hide();  // controls go away while typing
            settle();
            check(app.undoName() == "Type in \"Text\"", "undo: typing still going on when the controls close is a step too", app.undoName());
            app.undo();
            settle();
            check(strOf(P, "p_str") == before, "undo: ... and undoes", strOf(P, "p_str"));
            w->show();
            settle();
        }
    } else {
        check(false, "undo: the controls are there");
    }
    {
        // angles get a dial, positions a pad (the test effect's parameters are made into those kinds here)
        ofx::Instance* inst = app.fx.video(P);
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            inst->params.find("p_dbl")->props.setString(kOfxParamPropDoubleType, kOfxParamDoubleTypeAngle);
            inst->params.find("p_d2")->props.setString(kOfxParamPropDoubleType, kOfxParamDoubleTypeXYAbsolute);
        }
        w->hide();  // controls are rebuilt when the window shows
        settle();
        w->show();
        settle();
        QWidget* dial = in<QWidget>("p_dbl", "fxDial");
        check(dial && in<QSlider>("p_dbl", "fxSlider") == nullptr, "an angle gets a dial instead of a slider");
        QDoubleSpinBox* deg = in<QDoubleSpinBox>("p_dbl", "fxBox");
        check(deg && deg->suffix() == QString::fromUtf8("\xc2\xb0"), "and its number is in degrees");
        if (dial && deg) {
            const double was = valueOf(P, "p_dbl")[0];
            const QPoint c(dial->width() / 2, dial->height() / 2);
            // from straight right (0) round to a little above it (anticlockwise: more degrees, up to the effect's 10)
            uitest::mousePress(dial, QPoint(dial->width() - 2, c.y()));
            uitest::mouseMove(dial, QPoint(dial->width() - 2, c.y() - 1));
            uitest::mouseMove(dial, QPoint(dial->width() - 2, c.y() - 2));
            uitest::mouseRelease(dial, QPoint(dial->width() - 2, c.y() - 2));
            settle();
            const double v = valueOf(P, "p_dbl")[0];
            check(v > 1.0 && v <= 10.0 && near(deg->value(), v, 1e-3), "turning the dial sets the angle (anticlockwise is more)", num(v));
            check(app.undoName() == "Change \"Double Value\"", "turning the dial is one undo step", app.undoName());
            app.undo();
            settle();
            check(near(valueOf(P, "p_dbl")[0], was), "and undoing it puts the angle back", num(valueOf(P, "p_dbl")[0]));
        }
        QWidget* pad = in<QWidget>("p_d2", "fxPad");
        check(pad != nullptr && row("p_d2")->findChildren<QDoubleSpinBox*>("fxBox").size() == 2, "a position gets a pad as well as its two numbers");
        if (pad) {
            uitest::shot(w, gOut + "/fxwindow_angle_xy.png");
            // the pad is the frame (in canonical pixels, y counting up); a click near its bottom left corner
            const double W = app.project().settings.width, H = app.project().settings.height;
            const QRectF area = QRectF(pad->rect()).adjusted(3.5, 3.5, -3.5, -3.5);
            const QPoint at((int)(area.left() + 2), (int)(area.bottom() - 2));
            uitest::mousePress(pad, at);
            uitest::mouseRelease(pad, at);
            settle();
            const double wantX = (at.x() - area.left()) / area.width() * W, wantY = (area.bottom() - at.y()) / area.height() * H;
            const std::vector<double> v = valueOf(P, "p_d2");
            check(v.size() == 2 && std::fabs(v[0] - std::min(100.0, wantX)) < 1 && std::fabs(v[1] - std::min(100.0, wantY)) < 1 && v[1] > 10,
                  "clicking the pad puts the point there (in the frame's pixels, y up)", v.size() == 2 ? num(v[0]) + ", " + num(v[1]) + " want " + num(wantX) + ", " + num(wantY) : "");
            check(app.undoName() == "Move \"Point\"", "moving a point is one undo step", app.undoName());
            app.undo();
            settle();
        }
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            inst->params.find("p_dbl")->props.setString(kOfxParamPropDoubleType, kOfxParamDoubleTypePlain);
            inst->params.find("p_d2")->props.setString(kOfxParamPropDoubleType, kOfxParamDoubleTypePlain);
        }
        w->hide();
        settle();
        w->show();
        settle();
    }

    // ------------------------------------------------------------------ the chain: pick, bypass, move, remove
    const uint64_t I = app.addVideoFx(vt, "reno.test.invert", "Test Invert");
    settle();
    check(chainIds(vt).size() == 2 && row("p_dbl") != nullptr, "adding from elsewhere keeps the picked effect's controls");
    if (fxlog.ok()) fxlog.clear();
    if (QWidget* b = chainButton(I)) uitest::mouseClick(b, centre(b));
    settle();
    check(row("p_dbl") == nullptr && row("zeroRed") == nullptr ? true : row("p_dbl") == nullptr, "clicking another effect's button swaps the controls");
    check(fxlog.has("params: endEdit"), "the effect whose controls went away is told (endEdit)");
    check(app.fx.video(P)->props.getInt(kOfxPropIsInteractive) == 0, "... and is no longer marked interactive");
    if (QWidget* b = chainButton(I)) {
        QCheckBox* tick = b->findChild<QCheckBox*>("fxBypass");
        check(tick && tick->isChecked(), "bypass: ticked = on");
        if (tick) tick->click();
        settle();
        check(slotOf(vt, I) && slotOf(vt, I)->bypass, "bypass: clearing the tick bypasses the effect");
        tick = chainButton(I) ? chainButton(I)->findChild<QCheckBox*>("fxBypass") : nullptr;
        check(tick && !tick->isChecked(), "bypass: the tick follows the project");
    }
    one<QToolButton>("fxMoveLeft")->click();
    settle();
    check(chainIds(vt) == std::vector<uint64_t>({I, P}), "move left: the picked effect goes first");
    check(!one<QToolButton>("fxMoveLeft")->isEnabled() && one<QToolButton>("fxMoveRight")->isEnabled(), "move buttons enabled as they can be used");
    {
        QWidget* bp = chainButton(P);
        QWidget* bi = chainButton(I);
        if (bp && bi) {
            const QPoint to = bp->mapFrom(bi->parentWidget(), bi->geometry().topLeft()) + QPoint(3, bi->height() / 2);
            uitest::mouseDrag(bp, centre(bp), to);
            settle();
        }
        check(chainIds(vt) == std::vector<uint64_t>({P, I}), "drag: dropping a button before another moves it there");
    }
    const uint64_t G = app.addVideoFx(vt, "reno.test.general", "Test General");
    settle();
    {
        QWidget* bg = chainButton(G);
        QWidget* bp = chainButton(P);
        if (bg && bp) {
            const QPoint to = bg->mapFrom(bp->parentWidget(), bp->geometry().topLeft()) + QPoint(2, bp->height() / 2);
            uitest::mouseDrag(bg, centre(bg), to);
            settle();
        }
        check(chainIds(vt) == std::vector<uint64_t>({G, P, I}), "drag: from the end to the front");
        check(app.undoName() == "Reorder Effects", "drag: one step for Undo", app.undoName());
        app.undo();
        settle();
        check(chainIds(vt) == std::vector<uint64_t>({P, I, G}), "drag: undone in one step");
    }
    check(chainButton(G) && chainButton(G)->property("slotId").toULongLong() == G, "chain buttons follow undo");
    // remove: the picked one goes, a neighbour is picked
    if (QWidget* b = chainButton(I)) uitest::mouseClick(b, centre(b));
    settle();
    one<QToolButton>("fxRemove")->click();
    settle();
    check(chainIds(vt) == std::vector<uint64_t>({P, G}), "remove: the picked effect is taken out");
    check(chainButton(G) && chainButton(I) == nullptr, "remove: its button goes");
    {
        // the one that took its place is picked: General has no test-kind parameters, so look at the panel's rows
        QWidget* panel = one<QWidget>("fxOfxPanel");
        check(panel && row("p_dbl") == nullptr, "remove: the neighbour is picked");
    }
    app.undo();
    settle();
    check(chainIds(vt) == std::vector<uint64_t>({P, I, G}), "undo brings the removed effect back");

    // ------------------------------------------------------------------ effects before Event Pan/Crop
    {
        w->showTarget(vt, G);
        settle();
        w->setEffectBeforePanCrop(G, true);
        settle();
        check(chainIds(vt) == std::vector<uint64_t>({G, P, I}) && slotOf(vt, G) && slotOf(vt, G)->beforePanCrop,
              "before pan/crop: the effect moves to the start of the chain (where VEGAS keeps those) and is marked");
        QLabel* badge = chainButton(G) ? chainButton(G)->findChild<QLabel*>("fxPrePanCrop") : nullptr;
        QLabel* other = chainButton(P) ? chainButton(P)->findChild<QLabel*>("fxPrePanCrop") : nullptr;
        check(badge && badge->isVisible() && other && !other->isVisible(), "before pan/crop: its button says \"pre\", the others do not");
        uitest::shot(w, gOut + "/fxwindow_before_pancrop.png");
        w->setEffectBeforePanCrop(P, true);
        settle();
        check(chainIds(vt) == std::vector<uint64_t>({G, P, I}), "before pan/crop: a second one goes after the first");
        w->setEffectBeforePanCrop(G, false);
        settle();
        check(chainIds(vt) == std::vector<uint64_t>({P, G, I}) && !slotOf(vt, G)->beforePanCrop && slotOf(vt, P)->beforePanCrop,
              "after pan/crop again: it moves to just after the ones before");
        check(app.undoName() == "Effect After Pan/Crop", "before/after pan/crop: an undo step each", app.undoName());
        app.undo();
        app.undo();
        app.undo();
        settle();
        check(chainIds(vt) == std::vector<uint64_t>({P, I, G}) && !slotOf(vt, G)->beforePanCrop && !slotOf(vt, P)->beforePanCrop,
              "before pan/crop: undo puts the chain back");
    }

    // ------------------------------------------------------------------ an effect busy drawing
    {
        // while a render holds the instance, the panel waits (saying so) instead of freezing the window
        w->showTarget(vt, I);
        settle();
        ofx::Instance* inst = app.fx.video(P);
        std::atomic<int> state{0};
        std::thread busy([&] {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            state = 1;
            while (state != 2) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        });
        while (state != 1) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        w->showTarget(vt, P);
        settle(80);
        QLabel* waiting = one<QLabel>("fxWaiting");
        check(waiting != nullptr && row("p_dbl") == nullptr, "busy effect: the panel waits, saying so");
        uitest::shot(w, gOut + "/fxwindow_waiting.png");
        state = 2;
        busy.join();
        settle(150);
        check(one<QLabel>("fxWaiting") == nullptr && row("p_dbl") != nullptr, "busy effect: the controls appear once it is free");
    }

    // ------------------------------------------------------------------ effects that fail
    const uint64_t F = app.addVideoFx(vt, "reno.test.failcreate", "Fail Create");
    settle();
    w->showTarget(vt, F);
    settle();
    {
        QLabel* m = one<QLabel>("fxMessage");
        QLabel* dt = one<QLabel>("fxMessageDetail");
        check(m && m->text().contains("could not be loaded"), "an effect that cannot be made says so where its controls would be",
              m ? m->text().toStdString() : "no message");
        const std::string why = app.fx.error(F);
        check(!why.empty() && dt && dt->text().contains(QString::fromStdString(why)), "... with the reason", why);
        QWidget* b = chainButton(F);
        QWidget* warn = b ? b->findChild<QWidget*>("fxWarn") : nullptr;
        check(warn && warn->isVisibleTo(w) && b->toolTip().contains(QString::fromStdString(why)), "... and its chain button is marked");
        QWidget* bp = chainButton(P);
        check(bp && !bp->findChild<QWidget*>("fxWarn")->isVisibleTo(w), "... the others are not");
        uitest::shot(w, gOut + "/fxwindow_error.png");
    }
    {
        // one that is made but fails when drawing: its controls stay, with a line saying what went wrong
        FxTarget out;
        out.kind = FxTarget::VideoOut;
        const uint64_t R = app.addVideoFx(out, "reno.test.failrender", "Fail Render");
        w->showTarget(out, R);
        settle();
        check(w->windowTitle() == "Video Output FX", "title for the video output", w->windowTitle().toStdString());
        playback.renderNow(1.0);
        settle(450);
        QLabel* banner = one<QLabel>("fxBanner");
        check(!app.fx.error(R).empty() && banner && banner->text().contains("problem"), "an effect that fails to draw shows the problem",
              banner ? banner->text().toStdString() : app.fx.error(R));
        uitest::shot(w, gOut + "/fxwindow_render_error.png");
        app.removeFx(out, R);
        settle();
        check(one<QLabel>("fxMessage") && one<QLabel>("fxMessage")->text().contains("No effects"), "removing the last effect leaves the empty message");
    }

    // ------------------------------------------------------------------ a track's effects; the target going away
    {
        const Project& pr = app.project();
        FxTarget tt;
        tt.kind = FxTarget::Track;
        for (const auto& t : pr.tracks)
            if (t.type == TrackType::Video) tt.id = t.id;
        w->showTarget(tt, 0);
        settle();
        check(w->windowTitle().startsWith("Video Track FX: "), "title for a track", w->windowTitle().toStdString());
        app.addVideoFx(tt, "reno.test.params", "Test Parameters");
        settle();
        w->showTarget(tt, 0);
        settle();
        check(row("p_dbl") != nullptr, "showTarget with slot 0 picks the last effect");
    }
    w->showTarget(vt, P);
    settle();
    app.setSelection({vt.id});
    app.deleteSelection();
    settle();
    {
        QLabel* m = one<QLabel>("fxMessage");
        check(m && m->text().contains("no longer"), "the event deleted: the window says so", m ? m->text().toStdString() : "");
        check(w->target().kind == FxTarget::None, "... and lets go of it");
        check(all<QWidget>("fxChainButton").empty(), "... with no chain buttons left");
    }
    app.undo();
    settle();

    // ------------------------------------------------------------------ OpenFX effects with a window of their own
#ifdef RENO_FX_HOST_PLUGIN_WINDOWS
    {
        FxTarget out;
        out.kind = FxTarget::VideoOut;
        if (fxlog.ok()) fxlog.clear();
        const uint64_t V = app.addVideoFx(out, "reno.test.vegas", "Test Vegas");
        w->showTarget(out, V);
        settle();
        QWidget* host = one<QWidget>("fxPluginWindow");
        check(host != nullptr, "own window: a place is made for it at the top of the panel");
        check(fxlog.has("hwnd:OfxHWndInteractActionCreateWindow"), "own window: the effect is asked to make it");
        if (host) {
            check(host->height() == 240, "own window: as tall as the effect prefers", std::to_string(host->height()));
            check(host->y() < 20, "own window: above the ordinary controls", std::to_string(host->y()));
            const std::string loc = "location=0,0," + std::to_string(host->width()) + ",240";
            check(fxlog.has(loc), "own window: told where it is, in the window made for it", loc);
        }
        const int moves = fxlog.countOf("hwnd:OfxHWndInteractActionMoveWindow");
        w->resize(w->width() + 60, w->height());
        settle();
        check(fxlog.countOf("hwnd:OfxHWndInteractActionMoveWindow") > moves, "own window: moved when the window is resized");
        uitest::shot(w, gOut + "/fxwindow_ofx_own_window.png");
        w->hide();
        settle();
        check(fxlog.has("hwnd:OfxHWndInteractActionDisposeWindow"), "own window: disposed of when the window is hidden");
        w->show();
        settle();
        check(fxlog.countOf("hwnd:OfxHWndInteractActionCreateWindow") == 2, "own window: made again when the window is shown again");
        w->resize(560, 700);
        app.removeFx(out, V);
        settle();
        check(fxlog.countOf("hwnd:OfxHWndInteractActionDisposeWindow") == 2, "own window: disposed of when the effect is removed");
    }
#endif

    // ------------------------------------------------------------------ the user's real effects
    {
        FxTarget out;
        out.kind = FxTarget::VideoOut;
        // their pictures take long to draw here, and drawing them isn't what is tested
        app.bypassAllFx = true;
        struct Seen { uint64_t slot; std::string id; size_t params; };
        std::vector<Seen> seen;
        int built = 0, failed = 0;
        for (const VideoFxEntry& e : Catalog::get().video()) {
            if (!e.ok || !e.filter || e.id.compare(0, 4, "com.") != 0) continue;
            const uint64_t s = app.addVideoFx(out, e.id, e.label);
            settle(20);
            w->showTarget(out, s);
            settle(40);
            ofx::Instance* inst = app.fx.video(s);
            if (!inst) { ++failed; continue; }
            size_t n = 0;
            for (const auto& p : inst->params.params)
                if (p->kind != ofx::ParamKind::Group && p->kind != ofx::ParamKind::Page) ++n;
            const size_t rows = all<QWidget>("fxRow").size();
            if (rows > 0 || n == 0) ++built;
            seen.push_back({s, e.id, n});
        }
        if (!seen.empty()) {
            check(built == (int)seen.size() && failed == 0, "every effect of the test plug-in gets its controls",
                  std::to_string(built) + " of " + std::to_string(seen.size()) + ", " + std::to_string(failed) + " failed");
            std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.params > b.params; });
            const QSize was = w->size();
            w->resize(600, 1000);
            for (size_t k = 0; k < seen.size() && k < 3; ++k) {
                w->showTarget(out, seen[k].slot);
                settle(150);
                uitest::shot(w, gOut + QString("/fxwindow_realfx_%1.png").arg(k + 1));
                std::printf("  (picture %d: %s, %d parameters)\n", (int)k + 1, seen[k].id.c_str(), (int)seen[k].params);
            }
            w->resize(was);
            for (const Seen& s : seen) app.removeFx(out, s.slot);
            settle();
        }
        app.bypassAllFx = false;
    }

    // ------------------------------------------------------------------ audio effects
    {
        w->showTarget(at, 0);
        settle();
        check(w->windowTitle().startsWith("Audio Event FX: "), "title names an event's audio effects", w->windowTitle().toStdString());
        check(menuAction("Test Parameters") == nullptr, "audio: the add menu has no video effects");
        QMenu* menu = gWin->findChild<QMenu*>("fxAddMenu");
        QMetaObject::invokeMethod(menu, "aboutToShow");
        QAction* chosen = nullptr;
        for (QAction* a : menu->findChildren<QAction*>("fxAddAction"))
            if (!a->text().contains("FxWin")) { chosen = a; break; }
        check(chosen != nullptr, "audio: the add menu lists audio effects");
        if (chosen) {
            chosen->trigger();
            settle();
            const std::vector<uint64_t> aids = chainIds(at);
            const uint64_t A = aids.empty() ? 0 : aids.back();
            AudioPlugin* plugin = app.fx.audio(A);
            check(plugin != nullptr, "audio: the effect is made");
  // this part is about the sliders: switch to them if the plug-in's own window opened
            if (QToolButton* mode = one<QToolButton>("fxAudioMode"))
                if (!mode->isChecked()) { mode->click(); settle(); }
            std::vector<QSlider*> sliders = all<QSlider>("fxAudioSlider");
            check(plugin && !sliders.empty() && (int)sliders.size() == std::min(plugin->paramCount(), 300), "audio: a slider per setting",
                  std::to_string(sliders.size()));
            if (plugin && !sliders.empty()) {
                sliders[0]->setValue(800);
                check(std::fabs(plugin->paramGet(0) - 0.8f) < 0.002f, "audio: moving a slider sets the plug-in's setting", num(plugin->paramGet(0)));
                QWidget* textLabel = sliders[0]->parentWidget()->findChild<QWidget*>("fxAudioText");
                check(textLabel && textLabel->accessibleName() == QString::fromStdString(plugin->paramText(0)).trimmed(),
                      "audio: the plug-in's own wording of the value is shown", textLabel ? textLabel->accessibleName().toStdString() : "");
                plugin->paramSet(0, 0.1f);
                settle(450);
                check(sliders[0]->value() == 100, "audio: a change made elsewhere shows up", std::to_string(sliders[0]->value()));
                // undo: a drag of an audio plug-in's slider is one step
                sliders[0]->setSliderDown(true);
                sliders[0]->setValue(300);
                sliders[0]->setValue(500);
                sliders[0]->setSliderDown(false);
                settle();
                check(std::fabs(plugin->paramGet(0) - 0.5f) < 0.002f && app.undoName().compare(0, 7, "Change ") == 0, "audio undo: a slider drag is a step", app.undoName());
                app.undo();
                settle();
                check(std::fabs(plugin->paramGet(0) - 0.1f) < 0.002f && sliders[0]->value() == 100, "audio undo: the setting and the slider go back",
                      num(plugin->paramGet(0)));
                app.redo();
                settle();
                check(std::fabs(plugin->paramGet(0) - 0.5f) < 0.002f, "audio redo: and forward again", num(plugin->paramGet(0)));
            }
            uitest::shot(w, gOut + "/fxwindow_audio.png");
        }
    }
    if (!testVstPath.empty()) {
        TestVst tv;
        AudioPluginDesc desc;
        for (const AudioFxEntry& e : Catalog::get().audio())
            if (e.desc.path == testVstPath) desc = e.desc;
        FxTarget master;
        master.kind = FxTarget::AudioOut;
        const uint64_t T = app.addAudioFx(master, desc);
        w->showTarget(master, T);
        settle();
        if (void* lib = openLoaded(testVstPath)) {
            sym(lib, "fxwintest_opens", tv.opens);
            sym(lib, "fxwintest_closes", tv.closes);
            sym(lib, "fxwintest_idles", tv.idles);
            sym(lib, "fxwintest_open_now", tv.openNow);
            sym(lib, "fxwintest_parent", tv.parent);
            sym(lib, "fxwintest_param", tv.param);
            sym(lib, "fxwintest_move", tv.move);
            sym(lib, "fxwintest_resize", tv.resize);
        }
        check(tv.ok(), "test plug-in: found");
        if (QToolButton* mode = one<QToolButton>("fxAudioMode"))
            if (mode->isChecked()) { mode->click(); settle(); }  // back from sliders, as left above
        check(w->windowTitle() == "Master Bus FX", "title for the master bus", w->windowTitle().toStdString());
#ifdef RENO_FX_HOST_PLUGIN_WINDOWS
        if (tv.ok()) {
            QWidget* host = one<QWidget>("fxPluginWindow");
            check(tv.openNow() == 1 && tv.opens() == 1, "plug-in window: opened");
            check(host && tv.parent() == (void*)host->winId(), "plug-in window: inside the place made for it");
            check(host && host->size() == QSize(320, 180), "plug-in window: the size it asked for",
                  host ? std::to_string(host->width()) + "x" + std::to_string(host->height()) : "");
            QScrollArea* sa = one<QScrollArea>("fxScroll");
            check(sa && sa->viewport()->width() >= 320 && sa->viewport()->height() >= 180 && w->width() < 520,
                  "plug-in window: the window is fitted round it", std::to_string(w->width()) + "x" + std::to_string(w->height()));
            const int idles = tv.idles();
            settle(300);
            check(tv.idles() > idles + 2, "plug-in window: given idle time", std::to_string(tv.idles() - idles));
            std::thread([&tv] { tv.move(1, 0.9f); }).join();
            settle();
            tv.resize(400, 250);
            settle();
            host = one<QWidget>("fxPluginWindow");
            check(host && host->size() == QSize(400, 250), "plug-in window: grows when the plug-in asks");
            uitest::shot(w, gOut + "/fxwindow_audio_window.png");
            QToolButton* mode = one<QToolButton>("fxAudioMode");
            check(mode != nullptr, "plug-in window: a switch to plain sliders");
            if (mode) {
                mode->click();
                settle();
                check(tv.openNow() == 0 && tv.closes() == 1 && !all<QSlider>("fxAudioSlider").empty(), "switch: the window is closed and sliders shown");
                QSlider* mix = all<QSlider>("fxAudioSlider").size() > 1 ? all<QSlider>("fxAudioSlider")[1] : nullptr;
                check(mix && mix->value() == 900, "switch: the sliders show what was set in the plug-in's window");
                check(w->width() >= 540, "switch: the window goes back to its size", std::to_string(w->width()));
                mode->click();
                settle();
                check(tv.openNow() == 1 && tv.opens() == 2, "switch: and back to the plug-in's window");
            }
            w->hide();
            settle();
            check(tv.openNow() == 0, "plug-in window: closed when the window is hidden");
            w->show();
            settle();
            check(tv.openNow() == 1, "plug-in window: opened again when shown");
        }
#else
        if (tv.ok()) {
            check(one<QWidget>("fxPluginWindow") == nullptr && tv.opens() == 0, "plug-in with a window: not opened where windows cannot be hosted");
            std::vector<QSlider*> sliders = all<QSlider>("fxAudioSlider");
            check(sliders.size() == 3, "plug-in with a window: plain sliders instead");
            check(one<QToolButton>("fxAudioMode") == nullptr, "plug-in with a window: no switch when there is no choice");
            std::thread([&tv] { tv.move(1, 0.9f); }).join();  // as if from the plug-in's own thread
            settle();
            check(sliders.size() == 3 && sliders[1]->value() == 900, "a change the plug-in reports is shown at once");
            if (sliders.size() == 3) sliders[2]->setValue(1000);
            check(tv.param(2) == 1.0f, "slider sets the plug-in's setting");
            uitest::shot(w, gOut + "/fxwindow_audio_test_vst.png");
        }
#endif
    }

    // ------------------------------------------------------------------ About / Help: VEGAS's effects ask the host back
    {
        const uint64_t V = app.addVideoFx(vt, "reno.test.vegas18", "Test VEGAS 18");
        w->showTarget(vt, V);
        settle();
        check(app.fx.video(V) != nullptr && app.fx.error(V).empty(), "an effect made like VEGAS 18's own is made", app.fx.error(V));
        // the right-click menu: pick About, then read and close the box the host shows
        bool hadAbout = false, hadHelp = false, picked = false;
        QString boxText, boxName;
        QTimer pick;
        pick.setInterval(30);
        QObject::connect(&pick, &QTimer::timeout, [&] {
            if (QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                QAction* about = nullptr;
                for (QAction* a : m->actions()) {
                    if (a->objectName() == "fxAbout") { hadAbout = a->isEnabled(); about = a; }
                    if (a->objectName() == "fxHelp") hadHelp = a->isEnabled();
                }
                // chosen as a person would: item made current, then Return; the menu closes and says what was chosen
                if (about && !picked) {
                    picked = true;
                    m->setActiveAction(about);
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                    QApplication::sendEvent(m, &press);
                } else if (!about) m->close();
            } else if (QWidget* box = QApplication::activeModalWidget()) {
                if (QMessageBox* mb = qobject_cast<QMessageBox*>(box)) {
                    boxText = mb->text();
                    boxName = mb->objectName();
                    mb->done(0);
                }
            }
        });
        pick.start();
        if (QWidget* b = chainButton(V)) uitest::mousePress(b, centre(b), Qt::RightButton);
        for (int k = 0; k < 40 && boxText.isEmpty(); ++k) settle(50);
        pick.stop();
        check(hadAbout && hadHelp, "an effect's right-click menu has About and Help");
        check(fxlog.has("vegas18: about 0 bad=1 null=1"), "About goes to the effect, which asks the host back (OfxVegasEffectSuite)");
        check(boxName == "fxAboutBox" && boxText.contains("Test VEGAS 18") && boxText.contains("reno.test.vegas18"),
              "the host shows the effect's About: its name, identifier and description", boxText.toStdString());
        app.removeFx(vt, V);
        settle();
    }

    // ------------------------------------------------------------------ closing, reopening, a new project
    w->showTarget(vt, P);
    settle();
    if (fxlog.ok()) fxlog.clear();
    w->close();
    settle();
    check(fxlog.has("params: endEdit"), "closing the window: endEdit");
    w->show();
    settle();
    check(fxlog.has("params: beginEdit") && row("p_dbl") != nullptr, "reopening: the controls are back");
    app.newProject();
    settle();
    {
        QLabel* m = one<QLabel>("fxMessage");
        check(m && m->text().contains("no longer"), "New Project with the window open: it says the effects have gone");
    }
    settle(400);
    // and something new in the new project
    {
        const uint64_t m2 = app.importMedia("synth:2", &err);
        std::vector<uint64_t> ev2 = app.addMediaToTimeline(m2, 0.0);
        FxTarget t2;
        for (uint64_t id : ev2) {
            FxTarget t;
            t.kind = FxTarget::Event;
            t.id = id;
            if (app.targetIsVideo(t)) t2 = t;
        }
        const uint64_t s2 = app.addVideoFx(t2, "reno.test.params", "Test Parameters");
        w->showTarget(t2, s2);
        settle();
        check(row("p_dbl") != nullptr, "the window works in the new project");
        app.newProject();
        settle();
        app.undo();
        settle();
    }
    // destroyed while showing an effect
    delete w;
    gWin = nullptr;
    settle();
    check(true, "destroyed while showing an effect, without trouble");
    return uitest::summary();
}
