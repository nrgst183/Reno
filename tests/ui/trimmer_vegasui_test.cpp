// The Trimmer's tool bars as VEGAS 18 has them (manual pages 135-142, "Using the Trimmer" and "Trimmer Transport Tool
// Bar"; its pictures p0030 and p0140): the history with Sort, Remove Current Media, Clear and Trimmer on External
// Monitor along the top; under the strip the transport (Play, Pause, Stop, Add to Timeline from Cursor, Fit to Fill
// and the hamburger at first, the rest through Edit Visible Button Set) and the three time boxes. Saves pictures.
//
//   trimmer_vegasui_test <output folder>
//
// QT_QPA_PLATFORM=offscreen.
#include "../../src/app/trimmer.h"
#include "uitestlib.h"
#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPalette>
#include <QPushButton>
#include <QSettings>
#include <QStyleFactory>
#include <QTimer>
#include <cmath>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

AppState& st() { return AppState::get(); }
bool near(double a, double b, double eps = 1e-3) { return std::fabs(a - b) < eps; }
std::string num(double v) { return std::to_string(v); }

// The main window's dark theme (as applyTheme() in mainwindow.cpp sets it).
void theme()
{
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(220, 220, 220), mid(62, 62, 66), accent(0, 122, 204);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, QColor(38, 38, 41));
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, mid);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    QApplication::setPalette(p);
}

QAbstractButton* btn(QWidget* w, const char* name) { return w->findChild<QAbstractButton*>(name); }

// The names of the visible buttons among 'names', left to right.
QStringList visibleOrder(QWidget* w, const QStringList& names)
{
    std::vector<std::pair<int, QString>> at;
    for (const QString& n : names) {
        QWidget* b = w->findChild<QWidget*>(n);
        if (b && b->isVisible()) at.push_back(std::make_pair(b->mapTo(w, QPoint(0, 0)).x(), n));
    }
    std::sort(at.begin(), at.end());
    QStringList out;
    for (const auto& a : at) out << a.second;
    return out;
}

const QStringList kAllTransport = {"trimmerLoop", "trimmerPlayFromStart", "trimmerPlay", "trimmerPause", "trimmerStop", "trimmerPrevFrame",
                                   "trimmerNextFrame", "trimmerIn", "trimmerOut", "trimmerAdd", "trimmerAddEnd", "trimmerFitToFill",
                                   "trimmerOverwrite", "trimmerCreateSubclip", "trimmerInsertMarker", "trimmerInsertRegion",
                                   "trimmerSaveMarkers", "trimmerTransportMenu"};

size_t eventCount()
{
    size_t n = 0;
    for (const Track& t : st().project().tracks) n += t.events.size();
    return n;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoTrimmerVegasUiTest");
    QApplication::setApplicationName("trimmer_vegasui_test");
    QSettings().clear();
    const QString out = argc > 1 ? QString(argv[1]) : QString("trimmer_vegasui_out");
    QDir().mkpath(out);
    theme();
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());

    st().newProject();
    std::string err;
    const uint64_t clip = st().importMedia("synth:1", &err);
    st().edit("Build", [&](Project& p) {
        for (int k = 0; k < 2; ++k) {
            Track t;
            t.id = p.newId();
            t.type = k == 0 ? TrackType::Video : TrackType::Audio;
            p.tracks.push_back(t);
        }
    });
    st().currentTrack = st().project().tracks[0].id;

    TrimmerWindow tr;
    tr.resize(537, 415);
    tr.show();
    uitest::settle(100);

    // ---- the top bar: the history, then Sort, Remove Current Media, Clear and Trimmer on External Monitor (p0140)
    const QStringList top = visibleOrder(&tr, {"trimmerHistory", "trimmerSortHistory", "trimmerRemoveCurrent", "trimmerClearHistory", "trimmerExternalMonitor"});
    check(top == QStringList({"trimmerHistory", "trimmerSortHistory", "trimmerRemoveCurrent", "trimmerClearHistory", "trimmerExternalMonitor"}),
          "top bar: history, Sort, Remove Current Media, Clear, Trimmer on External Monitor", top.join(",").toStdString());
    QWidget* history = tr.findChild<QWidget*>("trimmerHistory");
    QWidget* sortB = btn(&tr, "trimmerSortHistory");
    check(history && sortB && sortB->mapTo(&tr, QPoint(0, 0)).y() < 30 && history->width() > tr.width() / 2,
          "the buttons sit on the same bar as the history, which takes the rest of the width");
    check(btn(&tr, "trimmerSortHistory")->text().isEmpty() && btn(&tr, "trimmerClearHistory")->text().isEmpty(), "they are pictures, not words");
    check(!btn(&tr, "trimmerExternalMonitor")->isEnabled(), "Trimmer on External Monitor waits for the main window");
    bool monitorOn = false;
    tr.setExternalMonitorHook([&](bool on) { monitorOn = on; });
    btn(&tr, "trimmerExternalMonitor")->click();
    check(monitorOn && btn(&tr, "trimmerExternalMonitor")->isChecked(), "with it set, the button turns the external monitor on");
    btn(&tr, "trimmerExternalMonitor")->click();
    check(!monitorOn, "and off");

    // ---- the transport: VEGAS's default set (the manual) and its order
    const QStringList def = visibleOrder(&tr, kAllTransport);
    check(def == QStringList({"trimmerPlay", "trimmerPause", "trimmerStop", "trimmerAdd", "trimmerFitToFill", "trimmerTransportMenu"}),
          "transport at first: Play, Pause, Stop, Add to Timeline from Cursor, Fit to Fill, hamburger", def.join(",").toStdString());
    QWidget* play = btn(&tr, "trimmerPlay");
    QWidget* strip = tr.strip();
    check(play && strip && play->mapTo(&tr, QPoint(0, 0)).y() > strip->mapTo(&tr, QPoint(0, 0)).y() + strip->height() - 1,
          "the transport is under the strip");
    {
        const int l = btn(&tr, "trimmerPlay")->mapTo(&tr, QPoint(0, 0)).x();
        const int r = btn(&tr, "trimmerTransportMenu")->mapTo(&tr, QPoint(btn(&tr, "trimmerTransportMenu")->width(), 0)).x();
        check(std::abs((l + r) / 2 - tr.width() / 2) <= 8, "and centered", num(l) + ".." + num(r));
    }

    // ---- open media; the time boxes
    check(tr.openMedia(clip), "the Trimmer opens the clip");
    uitest::settle(300);
    QLabel* startBox = tr.findChild<QLabel*>("trimmerInTime");
    QLabel* endBox = tr.findChild<QLabel*>("trimmerOutTime");
    QLabel* lenBox = tr.findChild<QLabel*>("trimmerLength");
    auto tc = [](QLabel* l) { return l->text().replace(QLatin1Char(';'), QLatin1Char(':')); };   // (drop-frame timecode has a ;)
    check(startBox && endBox && lenBox, "three time boxes");
    if (!startBox || !endBox || !lenBox) return uitest::summary();
    tr.setPosition(2.0);
    check(tc(startBox) == "00:00:02:00" && endBox->text().isEmpty() && tc(lenBox) == "00:00:10:00",
          "no selection: the cursor, nothing, the file's length",
          (startBox->text() + "|" + endBox->text() + "|" + lenBox->text()).toStdString());
    check(startBox->y() > play->mapTo(startBox->parentWidget(), QPoint(0, 0)).y() + 10 && std::abs(startBox->y() - lenBox->y()) <= 1,
          "on their own row under the transport");
    check(startBox->x() < endBox->x() && endBox->x() < lenBox->x(), "start, end, length from left to right");
    tr.setSelection(1.0, 3.5);
    check(tc(startBox) == "00:00:01:00" && tc(endBox) == "00:00:03:15" && tc(lenBox) == "00:00:02:15",
          "with a selection: its start, end and length", (startBox->text() + "|" + endBox->text() + "|" + lenBox->text()).toStdString());
    // a double-click types a time
    uitest::mouseDoubleClick(endBox, QPoint(60, 10));
    QLineEdit* ed = endBox->findChild<QLineEdit*>("trimmerTimeEdit");
    check(ed != nullptr, "a double-click on a box gives a place to type a time");
    if (ed) {
        ed->setText("00:00:04:00");
        emit ed->editingFinished();
        uitest::settle(50);
        check(near(tr.selectionOut(), 4.0) && near(tr.selectionIn(), 1.0), "typing the end moves the out point", num(tr.selectionOut()));
    }
    uitest::mouseDoubleClick(lenBox, QPoint(60, 10));
    ed = lenBox->findChild<QLineEdit*>("trimmerTimeEdit");
    if (ed) {
        ed->setText("2:00");
        emit ed->editingFinished();
        uitest::settle(50);
    }
    check(near(tr.selectionIn(), 1.0) && near(tr.selectionOut(), 3.0), "typing a length sets the out point from the in point", num(tr.selectionOut()));

    tr.setVisibleTransportButtons(QStringList({"play", "pause", "stop", "addFromCursor", "fitToFill", "insertMarker", "insertRegion", "saveMarkers"}));
    uitest::settle(50);
    tr.setSelection(0, 0);
    tr.setPosition(1.0);
    uitest::settle(200);
    uitest::shot(&tr, out + "/trimmer_vegas18_p0140.png");

    // ---- Play, Pause, Stop
    tr.setPosition(1.0);
    btn(&tr, "trimmerPlay")->click();
    uitest::settle(400);
    check(tr.playing() && tr.position() > 1.15, "Play plays", num(tr.position()));
    btn(&tr, "trimmerPause")->click();
    const double paused = tr.position();
    uitest::settle(100);
    check(!tr.playing() && paused > 1.15 && near(tr.position(), paused), "Pause stops where it is", num(paused));
    btn(&tr, "trimmerPlay")->click();
    uitest::settle(300);
    check(tr.playing(), "Play again");
    btn(&tr, "trimmerStop")->click();
    check(!tr.playing() && near(tr.position(), paused), "Stop puts the cursor back where Play found it", num(tr.position()));

    // ---- Add to Timeline from Cursor; Fit to Fill (needs a time selection on the timeline)
    st().setCursor(20.0);
    tr.setSelection(2.0, 4.0);
    size_t before = eventCount();
    btn(&tr, "trimmerAdd")->click();
    bool added = false;
    for (const Event& e : st().project().tracks[0].events) added = added || (near(e.start, 20.0) && near(e.length, 2.0));
    check(eventCount() > before && added, "Add to Timeline from Cursor puts the selection at the timeline's cursor");
    check(!btn(&tr, "trimmerFitToFill")->isEnabled(), "Fit to Fill is unavailable without a time selection on the timeline");
    st().setLoop(30.0, 34.0);
    uitest::settle(50);
    check(btn(&tr, "trimmerFitToFill")->isEnabled(), "and available with one");
    before = eventCount();
    btn(&tr, "trimmerFitToFill")->click();
    bool fitted = false;
    for (const Event& e : st().project().tracks[0].events) fitted = fitted || (near(e.start, 30.0) && near(e.length, 4.0) && near(e.rate, 0.5));
    check(eventCount() > before && fitted, "Fit to Fill stretches the selection over the time selection");
    st().setLoop(0, 0);

    // ---- markers and regions
    tr.setPosition(5.0);
    btn(&tr, "trimmerInsertMarker")->click();
    check(tr.markers().size() == 1 && near(tr.markers()[0].time, 5.0) && !tr.markersSaved(), "Insert Marker puts one at the cursor");
    check(btn(&tr, "trimmerSaveMarkers")->isEnabled(), "Save Markers/Regions is available");
    tr.setSelection(6.0, 7.0);
    btn(&tr, "trimmerInsertRegion")->click();
    check(tr.markers().size() == 2 && near(tr.markers()[1].length, 1.0), "Insert Region makes one over the selection");
    btn(&tr, "trimmerSaveMarkers")->click();
    check(tr.markersSaved() && st().project().findMedia(clip)->markers.size() == 2, "Save Markers/Regions keeps them with the media");
    check(!btn(&tr, "trimmerSaveMarkers")->isEnabled(), "then there is nothing to save");

    // ---- the hamburger: the hidden buttons, then Edit Visible Button Set
    {
        QMenu* m = tr.transportMenu();
        QStringList texts;
        for (QAction* a : m->actions())
            if (!a->isSeparator()) texts << a->text();
        check(texts.size() == 10 && texts.first() == "Loop Playback" && texts.contains("Add to Timeline up to Cursor") && texts.contains("Enable Timeline Overwrite") &&
                  texts.last() == "Edit Visible Button Set...",
              "the hamburger lists the buttons not on the bar, then Edit Visible Button Set", texts.join(",").toStdString());
        st().setCursor(50.0);
        tr.setSelection(1.0, 2.0);
        before = eventCount();
        for (QAction* a : m->actions())
            if (a->text() == "Add to Timeline up to Cursor") a->trigger();
        bool upTo = false;
        for (const Event& e : st().project().tracks[0].events) upTo = upTo || (near(e.start, 49.0) && near(e.length, 1.0));
        check(eventCount() > before && upTo, "its Add to Timeline up to Cursor ends the selection at the cursor");
        for (QAction* a : m->actions())
            if (a->text() == "Enable Timeline Overwrite") a->trigger();
        check(btn(&tr, "trimmerOverwrite")->isChecked(), "its Enable Timeline Overwrite turns overwriting on");
        delete m;
        tr.setTimelineOverwrite(false);
    }
    // Edit Visible Button Set: every button on
    QTimer::singleShot(200, [&] {
        QDialog* dlg = tr.findChild<QDialog*>("trimmerButtonSet");
        if (!dlg) return;
        for (QCheckBox* c : dlg->findChildren<QCheckBox*>()) c->setChecked(true);
        uitest::shot(dlg, out + "/trimmer_button_set.png");
        dlg->accept();
    });
    tr.editVisibleTransportButtons();
    uitest::settle(50);
    const QStringList all = visibleOrder(&tr, kAllTransport);
    check(all == kAllTransport, "every button on: VEGAS's order (Loop, Play From Start, Play, Pause, Stop, ... Save Markers/Regions, hamburger)",
          all.join(",").toStdString());
    check(QSettings().value("trimmer/transportButtons").toStringList().size() == 17, "the set is remembered");
    {
        TrimmerWindow again;
        check(again.visibleTransportButtons().size() == 17, "a new Trimmer has the same set");
    }
    // the rest of the buttons
    tr.setSelection(0, 0);
    tr.setPosition(3.0);
    btn(&tr, "trimmerNextFrame")->click();
    check(near(tr.position(), 3.0 + 1.0 / 30), "Next Frame", num(tr.position()));
    btn(&tr, "trimmerPrevFrame")->click();
    check(near(tr.position(), 3.0), "Previous Frame");
    btn(&tr, "trimmerIn")->click();
    tr.setPosition(4.0);
    btn(&tr, "trimmerOut")->click();
    check(near(tr.selectionIn(), 3.0) && near(tr.selectionOut(), 4.0), "Mark In and Mark Out");
    btn(&tr, "trimmerLoop")->click();
    btn(&tr, "trimmerPlayFromStart")->click();
    uitest::settle(200);
    check(tr.playing() && tr.position() >= 3.0 && tr.position() < 4.0, "Play From Start plays the selection from its start", num(tr.position()));
    btn(&tr, "trimmerStop")->click();
    btn(&tr, "trimmerLoop")->click();
    bool subclip = false;
    check(!btn(&tr, "trimmerCreateSubclip")->isEnabled(), "Create Subclip waits for the main window");
    tr.setCreateSubclipHook([&] { subclip = true; });
    btn(&tr, "trimmerCreateSubclip")->click();
    check(subclip, "then asks it for a subclip of the selection");
    tr.resize(700, 415);
    uitest::settle(100);
    uitest::shot(&tr, out + "/trimmer_all_buttons.png");

    // ---- the top bar's buttons work
    tr.openFile("synthaudio:3");
    tr.openFile("synth:2");
    check(tr.history().size() == 3, "three files in the history", tr.history().join(",").toStdString());
    btn(&tr, "trimmerSortHistory")->click();
    check(tr.history() == QStringList({"synth:1", "synth:2", "synthaudio:3"}), "Sort Trimmer History", tr.history().join(",").toStdString());
    btn(&tr, "trimmerRemoveCurrent")->click();
    check(tr.history() == QStringList({"synth:1", "synthaudio:3"}), "Remove Current Media", tr.history().join(",").toStdString());
    btn(&tr, "trimmerClearHistory")->click();
    check(tr.history().isEmpty(), "Clear Trimmer History");

    QSettings().clear();
    return uitest::summary();
}
