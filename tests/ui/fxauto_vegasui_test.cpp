// The FX Automation Chooser (src/app/fxautomation.cpp): a track with two built-in audio effects, the chooser's chain
// buttons and settings list, ticking a setting and OK (an envelope, one undo step), clearing it again, Select All / None,
// Cancel, and a picture to set beside the manual's (VEGAS Pro 18 manual page 243).
//
//   fxauto_vegasui_test <output folder>
//
// QT_QPA_PLATFORM=offscreen. Prints PASS/FAIL lines and a summary; exits non-zero on failure.
#include "../../src/app/fxautomation.h"
#include "../../src/audiofx/audioplugin.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QLabel>
#include <QListWidget>
#include <QPalette>
#include <QPushButton>
#include <QStyleFactory>
#include <QToolButton>
#include <cstdio>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

void theme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(220, 220, 220), mid(62, 62, 66), accent(0, 122, 204);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(p);
}

AudioPluginDesc builtin(const std::string& uid)
{
    for (const AudioPluginDesc& d : builtinAudioPlugins())
        if (d.uid == uid) return d;
    return AudioPluginDesc();
}

const Envelope* fxEnvelope(uint64_t track, uint64_t slot, const std::string& param)
{
    for (const Track& t : AppState::get().project().tracks)
        if (t.id == track)
            for (const Envelope& e : t.envelopes)
                if (e.kind == "fx" && e.target == slot && e.param == param) return &e;
    return nullptr;
}

int fxEnvelopeCount(uint64_t track)
{
    int n = 0;
    for (const Track& t : AppState::get().project().tracks)
        if (t.id == track)
            for (const Envelope& e : t.envelopes) n += e.kind == "fx";
    return n;
}

QListWidgetItem* row(QListWidget* l, const QString& name)
{
    for (int i = 0; l && i < l->count(); ++i)
        if (l->item(i)->text() == name) return l->item(i);
    return nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qa(argc, argv);
    const QString out = argc > 1 ? QString(argv[1]) : QString("fxauto_out");
    QDir().mkpath(out);
    theme();
    uitest::setup({}, {});
    AppState& app = AppState::get();
    app.newProject();
    const uint64_t track = app.addTrack(TrackType::Audio);
    FxTarget t;
    t.kind = FxTarget::Track;
    t.id = track;
    const uint64_t res = app.addAudioFx(t, builtin("reno.resonantfilter"));
    const uint64_t dist = app.addAudioFx(t, builtin("reno.distortion"));
    uitest::settle(100);
    AudioPlugin* pl = app.fx.audio(res);
    check(res && dist && pl && pl->paramCount() > 0, "two built-in effects on an audio track, running");
    std::string freq = pl && pl->paramCount() > 0 ? pl->paramName(0) : std::string();
    std::string second = pl && pl->paramCount() > 1 ? pl->paramName(1) : std::string();
    std::string names;
    for (int i = 0; pl && i < pl->paramCount(); ++i) names += (i ? ", " : "") + pl->paramName(i);
    std::printf("Resonant Filter's settings: %s\n", names.c_str());

    // ---- the dialog as it opens
    QDialog* d = makeFxAutomationChooser(nullptr, track);
    QList<QToolButton*> plugs = d->findChildren<QToolButton*>("fxAutoPlugin");
    QListWidget* list = d->findChild<QListWidget*>("fxAutoParams");
    QLabel* hint = d->findChild<QLabel*>("fxAutoHint");
    check(d->windowTitle() == "FX Automation Chooser - Track 1", "title names the track", d->windowTitle().toStdString());
    check(plugs.size() == 2 && plugs[0]->text() == "Resonant Filter" && plugs[1]->text() == "Distortion",
          "a button for each effect of the chain, in order", plugs.size() == 2 ? (plugs[0]->text() + " / " + plugs[1]->text()).toStdString() : "");
    check(plugs.size() == 2 && plugs[0]->isChecked() && !plugs[1]->isChecked(), "the first effect is picked");
    check(hint && hint->text() == "Select the plug-in parameters that you want to automate using track envelopes.", "the hint");
    check(list && pl && list->count() == pl->paramCount(), "the list has the effect's settings", list ? std::to_string(list->count()) : "");
    check(list && list->count() && list->item(0)->text() == QString::fromStdString(freq) && list->item(0)->checkState() == Qt::Unchecked,
          "named by the setting alone, none ticked");
    for (const char* b : {"fxAutoOk", "fxAutoCancel", "fxAutoSelectAll", "fxAutoSelectNone"})
        check(d->findChild<QPushButton*>(b) != nullptr, std::string("button ") + b);
    // the picture: two settings ticked, the second row picked, as in the manual
    if (list && list->count() > 1) {
        list->item(0)->setCheckState(Qt::Checked);
        list->item(1)->setCheckState(Qt::Checked);
        list->setCurrentRow(1);
    }
    d->show();
    uitest::settle(100);
    list->setFocus();
    uitest::settle(50);
    uitest::shot(d, out + "/fxauto_chooser.png");
    check(d->size() == QSize(429, 200), "VEGAS's size, 429 x 200");

    // ---- another effect, and back: what was ticked stays
    if (plugs.size() == 2) {
        plugs[1]->click();
        uitest::settle(20);
        AudioPlugin* dp = app.fx.audio(dist);
        check(dp && list->count() == dp->paramCount() && plugs[1]->isChecked(), "the second button shows its own settings");
        uitest::shot(d, out + "/fxauto_chooser_second.png");
        plugs[0]->click();
        uitest::settle(20);
        check(list->count() > 1 && list->item(0)->checkState() == Qt::Checked && list->item(1)->checkState() == Qt::Checked,
              "back on the first, its ticks are kept");
    }
    // Select None / All work on the effect showing
    d->findChild<QPushButton*>("fxAutoSelectAll")->click();
    int ticked = 0;
    for (int i = 0; i < list->count(); ++i) ticked += list->item(i)->checkState() == Qt::Checked;
    check(ticked == list->count(), "Select All ticks every setting");
    d->findChild<QPushButton*>("fxAutoSelectNone")->click();
    ticked = 0;
    for (int i = 0; i < list->count(); ++i) ticked += list->item(i)->checkState() == Qt::Checked;
    check(ticked == 0, "Select None clears them");
    // a click in a row's box ticks it
    if (list->count() > 1) {
        const QRect r = list->visualItemRect(list->item(1));
        uitest::mouseClick(list->viewport(), QPoint(r.x() + 18, r.center().y()));
        uitest::settle(20);
        check(list->item(1)->checkState() == Qt::Checked, "a click in the box ticks it");
        uitest::mouseClick(list->viewport(), QPoint(r.x() + 80, r.center().y()));
        uitest::settle(20);
        check(list->item(1)->checkState() == Qt::Checked, "a click on the name only picks the row");
    }
    // Cancel changes nothing
    const size_t undo0 = app.undoCount();
    d->reject();
    delete d;
    check(fxEnvelopeCount(track) == 0 && app.undoCount() == undo0, "Cancel adds no envelope");

    // ---- OK: the ticked settings get envelopes, as one undo step
    d = makeFxAutomationChooser(nullptr, track);
    list = d->findChild<QListWidget*>("fxAutoParams");
    if (QListWidgetItem* it = row(list, QString::fromStdString(freq))) it->setCheckState(Qt::Checked);
    if (QListWidgetItem* it = row(list, QString::fromStdString(second))) it->setCheckState(Qt::Checked);
    d->accept();
    delete d;
    check(fxEnvelope(track, res, freq) && fxEnvelope(track, res, second) && fxEnvelopeCount(track) == 2, "OK adds an envelope for each ticked setting",
          std::to_string(fxEnvelopeCount(track)));
    check(app.undoCount() == undo0 + 1 && app.undoName() == "FX Automation", "as one undo step", app.undoName());
    app.undo();
    check(fxEnvelopeCount(track) == 0, "undo takes both away");
    app.redo();
    check(fxEnvelopeCount(track) == 2, "redo puts them back");

    // ---- opening again: the envelopes show as ticks; clearing one and OK removes it
    d = makeFxAutomationChooser(nullptr, track);
    list = d->findChild<QListWidget*>("fxAutoParams");
    QListWidgetItem* f = row(list, QString::fromStdString(freq));
    check(f && f->checkState() == Qt::Checked && row(list, QString::fromStdString(second))->checkState() == Qt::Checked,
          "settings with an envelope are ticked");
    if (f) f->setCheckState(Qt::Unchecked);
    const size_t undo1 = app.undoCount();
    d->accept();
    delete d;
    check(!fxEnvelope(track, res, freq) && fxEnvelope(track, res, second), "clearing a tick and OK removes that envelope only");
    check(app.undoCount() == undo1 + 1, "one undo step");
    app.undo();
    check(fxEnvelope(track, res, freq) != nullptr, "and undo restores it");

    // OK with nothing changed: no undo step
    const size_t undo2 = app.undoCount();
    d = makeFxAutomationChooser(nullptr, track);
    d->accept();
    delete d;
    check(app.undoCount() == undo2, "OK with no change adds no undo step");
    return uitest::summary();
}
