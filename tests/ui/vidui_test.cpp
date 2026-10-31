// VEGAS 18's hamburger buttons and Expand Track Layers on the timeline (src/app/timeline.cpp): the track header's hamburger menu
// (the header buttons' commands, then Edit Visible Button Set), the event header's hamburger (right of the title bar, beside the
// Pan/Crop and FX buttons), Edit Visible Button Set hiding header buttons, the right-click track menu in VEGAS's order, Activate
// Automation Controls, and Expand Track Layers' A roll / B roll / transition roll.
//
//   vidui_test <output folder>
//
// Build: uibuild vidui_test tests/ui/vidui_test.cpp timeline fxwindow panels mixer editdialogs vegas mainwindow
#include "../../src/app/playback.h"
#include "../../src/app/mainwindow.h"
#include "../../src/app/timeline.h"
#include "uitestlib.h"
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QMenu>
#include <QTimer>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {
QStringList texts(const QMenu& m)
{
    QStringList l;
    for (QAction* a : m.actions()) l << (a->isSeparator() ? QString("-") : a->text().section('\t', 0, 0));
    return l;
}
QAction* find(const QMenu& m, const QString& text)
{
    for (QAction* a : m.actions())
        if (a->text().section('\t', 0, 0) == text) return a;
    return nullptr;
}
// Clicks at 'pos' in 'w' with a menu opening: returns the texts of the menu that came up (and closes it).
QStringList clickForMenu(QWidget* w, QPoint pos)
{
    QStringList got;
    QTimer::singleShot(250, [&got] {
        if (QMenu* m = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
            got = texts(*m);
            m->close();
        }
    });
    uitest::mouseClick(w, pos);
    uitest::settle(100);
    return got;
}
}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoViduiTest");
    QApplication::setApplicationName("vidui_test");
    const QStringList args = QApplication::arguments();
    const QString out = args.size() > 1 ? args[1] : QString("viduiout");
    QDir().mkpath(out);
    uitest::setup(std::vector<std::string>(), std::vector<std::string>());
    AppState& app = AppState::get();

    {
        const QString tag = "vegas";
        applyTheme();
        app.newProject();
        uitest::buildDemoProject();
        Playback playback;
        TimelineWidget w(&playback);
        w.resize(1400, 620);
        w.show();
        w.zoomToFit();
        w.onTrackCompositing = [](QMenu* m, uint64_t) {
            m->addMenu("Compositing Mode");
            m->addAction("Make Compositing Parent");
            m->addAction("Make Compositing Child");
        };
        w.onPanCrop = [](uint64_t) {};
        w.onTrackDefaults = [](QMenu* m, uint64_t) { m->addAction("Set Default Track Properties..."); };
        uitest::settle(300);
        const Project& p = app.project();
        auto trk = [&](uint64_t id) { return const_cast<Project&>(app.project()).findTrack(id); };
        auto evt = [&](uint64_t id) { return const_cast<Project&>(app.project()).findEvent(id); };
        uint64_t v0 = 0, a0 = 0;
        for (const Track& t : p.tracks) {
            if (!v0 && t.type == TrackType::Video) v0 = t.id;
            if (!a0 && t.type == TrackType::Audio) a0 = t.id;
        }
        const std::string L = "[VEGAS look] ";

        // ------------------------------------------------------------ the track header's hamburger
        {
            QMenu m;
            w.buildTrackHamburger(&m, v0);
            const QStringList want = {"Bypass Motion Blur", "Track Motion", "Track FX", "Activate Automation Controls", "Automation Settings", "Compositing Mode",
                                      "Make Compositing Parent", "Make Compositing Child", "Minimize", "Maximize", "-", "Edit Visible Button Set..."};
            check(texts(m) == want, L + "a video track's hamburger menu lists the header buttons' commands as VEGAS 18's does", texts(m).join(" | ").toStdString());
            QAction* am = find(m, "Automation Settings");
            check(am && am->menu() && texts(*am->menu()) == QStringList({"Off", "Read", "Automation Write (Touch)", "Automation Write (Latch)"}), L + "Automation Settings holds the modes");
            if (QAction* bp = find(m, "Bypass Motion Blur")) bp->trigger();
            check(trk(v0) && trk(v0)->bypassMotionBlur, L + "Bypass Motion Blur switches the track's flag");
            if (QAction* act = find(m, "Activate Automation Controls")) act->trigger();
            check(trk(v0)->automation, L + "Activate Automation Controls turns the header's automation controls on");
            QMenu m2;
            w.buildTrackHamburger(&m2, v0);
            check(find(m2, "Bypass Motion Blur")->isChecked() && find(m2, "Activate Automation Controls")->isChecked(), L + "and both show ticked");
            if (QAction* act = find(m2, "Activate Automation Controls")) act->trigger();
            if (QAction* bp = find(m2, "Bypass Motion Blur")) bp->trigger();
            QMenu ma;
            w.buildTrackHamburger(&ma, a0);
            check(find(ma, "Arm for Record") || find(ma, "Track FX"), L + "an audio track has a hamburger menu too", texts(ma).join(" | ").toStdString());
            // the button itself opens it
            const QRect menuBtn = w.headerPartRect(v0, TimelineWidget::PartMenu);
            const QStringList clicked = clickForMenu(&w, menuBtn.center());
            check(clicked == want, L + "a click on the hamburger button at the header's top left opens that menu", clicked.join(" | ").toStdString());
            check(menuBtn.left() < 30 && menuBtn.top() - w.headerRect(v0).top() < 12, L + "the button sits above the track number, at the left");
            uitest::shot(&w, out + "/" + tag + "_header.png");
        }

        // ------------------------------------------------------------ Edit Visible Button Set
        {
            check(!w.headerPartRect(v0, TimelineWidget::PartMute).isEmpty() && !w.headerPartRect(v0, TimelineWidget::PartSolo).isEmpty(), L + "Mute and Solo are shown to begin with");
            w.editVisibleButtons(true, [](QDialog* d) {
                if (QCheckBox* c = d->findChild<QCheckBox*>("button_mute")) c->setChecked(false);
                d->accept();
            });
            uitest::settle(50);
            check(w.headerPartRect(v0, TimelineWidget::PartMute).isEmpty() && !w.headerPartRect(v0, TimelineWidget::PartSolo).isEmpty() && w.headerButtonHidden(true, "mute"),
                  L + "Edit Visible Button Set: unticking Mute takes the Mute button off video track headers (Solo stays)");
            check(!w.headerButtonHidden(false, "mute") && !w.headerPartRect(a0, TimelineWidget::PartMute).isEmpty(), L + "audio track headers keep theirs");
            w.editVisibleButtons(true, [](QDialog* d) {
                if (QAbstractButton* b = d->findChild<QAbstractButton*>("hideAll")) b->click();
                d->accept();
            });
            check(w.headerPartRect(v0, TimelineWidget::PartSolo).isEmpty() && w.headerPartRect(v0, TimelineWidget::PartFx).isEmpty(), L + "Hide All hides them all");
            uitest::shot(&w, out + "/" + tag + "_header_hidden.png");
            w.editVisibleButtons(true, [](QDialog* d) {
                d->findChild<QAbstractButton*>("hideAll")->click();
                d->reject();
            });
            check(w.headerPartRect(v0, TimelineWidget::PartSolo).isEmpty(), L + "Cancel changes nothing");
            w.editVisibleButtons(true, [](QDialog* d) {
                if (QAbstractButton* b = d->findChild<QAbstractButton*>("showAll")) b->click();
                d->accept();
            });
            check(!w.headerPartRect(v0, TimelineWidget::PartMute).isEmpty() && !w.headerPartRect(v0, TimelineWidget::PartSolo).isEmpty(),
                  L + "Show All brings them back");
        }

        // ------------------------------------------------------------ the event header's hamburger
        {
            w.options().eventHeaders = true;
            w.optionsChanged();
            uitest::settle(100);
            uint64_t ev = 0;
            for (const Event& e : trk(v0)->events) { ev = e.id; break; }
            const QRect head = w.eventHeaderRect(ev), mb = w.eventMenuRect(ev), fx = w.eventButtonRect(ev, false), crop = w.eventButtonRect(ev, true);
            check(!head.isEmpty() && !mb.isEmpty() && mb.top() >= head.top() - 1 && mb.bottom() <= head.bottom() + 1,
                  L + "with event headers on, the hamburger button is in the event's title bar", strfmt("header %d..%d button %d..%d", head.top(), head.bottom(), mb.top(), mb.bottom()));
            check(mb.right() <= w.eventRectOf(ev).right() && fx.right() < mb.left() && crop.right() < fx.left() && crop.top() == mb.top(),
                  L + "at its right end, after the Pan/Crop and FX buttons");
            QMenu m;
            w.buildEventHamburger(&m, ev);
            const QStringList t = texts(m);
            check(t.value(0) == "Event Pan/Crop" && t.contains("Event FX") && t.contains("Freeze Frame at Cursor") && t.contains("Playback Rate") && t.contains("Active Take Information") &&
                      t.contains("Event Length") && t.contains("Edit Visible Button Set..."),
                  L + "the event hamburger menu: the header buttons' commands, the display switches, Edit Visible Button Set", t.join(" | ").toStdString());
            const QStringList clicked = clickForMenu(&w, mb.center());
            check(clicked == t, L + "a click on the button opens it", clicked.join(" | ").toStdString());
            const bool lenBefore = w.options().showEventLength;
            if (QAction* a = find(m, "Event Length")) a->trigger();
            check(w.options().showEventLength != lenBefore, L + "Event Length switches the length display");
            uitest::settle(100);
            uitest::shot(&w, out + "/" + tag + "_event_header.png");
            if (QAction* a = find(m, "Fade Handles")) a->trigger();
            check(!w.options().showFadeHandles, L + "Fade Handles off");
            if (QAction* a = find(m, "Opacity Handle")) a->trigger();
            check(!w.options().showGainHandle, L + "Opacity Handle off");
            w.options().showFadeHandles = w.options().showGainHandle = true;
            w.options().showEventLength = lenBefore;
            QMenu ma;
            uint64_t aev = 0;
            for (const Event& e : trk(a0)->events) { aev = e.id; break; }
            w.buildEventHamburger(&ma, aev);
            check(find(ma, "Volume Handle") && !find(ma, "Event Pan/Crop"), L + "an audio event's menu has the Volume Handle and no Pan/Crop");
            w.options().eventHeaders = false;
            w.optionsChanged();
        }

        // ------------------------------------------------------------ the right-click track menu
        {
            QMenu m;
            const QRect hr = w.headerRect(v0);
            w.buildContextMenu(&m, QPoint(60, hr.top() + hr.height() / 2));
            const QStringList t = texts(m);
            auto at = [&](const char* s) { return t.indexOf(QString(s)); };
            const bool order = at("Rename") >= 0 && at("Rename") < at("Insert Video Track") && at("Insert Video Track") < at("Duplicate Track") && at("Duplicate Track") < at("Delete Track") &&
                               at("Delete Track") < at("Pair as Stereoscopic 3D Subclips") && at("Pair as Stereoscopic 3D Subclips") < at("Insert/Remove Envelope") &&
                               at("Insert/Remove Envelope") < at("Switches") && at("Switches") < at("Fade Colors") && at("Fade Colors") < at("Expand Track Layers") &&
                               at("Expand Track Layers") < at("Set Default Track Properties...") && at("Set Default Track Properties...") < at("Track Group") &&
                               at("Track Group") < at("Track Display Color");
            check(order, L + "the video track's right-click menu runs in VEGAS 18's order (Rename ... Track Display Color)", t.join(" | ").toStdString());
            QAction* pair = find(m, "Pair as Stereoscopic 3D Subclips");
            check(pair && !pair->isEnabled(), L + "Pair as Stereoscopic 3D Subclips is there, dimmed");
        }

        // ------------------------------------------------------------ Expand Track Layers
        {
            check(!w.trackLayersExpanded(v0), L + "a track starts collapsed");
            const int h0 = w.headerRect(v0).height();
            QMenu m;
            const QRect hr = w.headerRect(v0);
            w.buildContextMenu(&m, QPoint(60, hr.top() + hr.height() / 2));
            QAction* ex = find(m, "Expand Track Layers");
            check(ex != nullptr, L + "the track menu has Expand Track Layers");
            if (ex) ex->trigger();
            uitest::settle(100);
            check(w.trackLayersExpanded(v0) && trk(v0)->layersExpanded && app.undoName() == "Expand Track Layers", L + "it expands the track (one undo step)", app.undoName());
            check(w.headerRect(v0).height() > h0, L + "which gets taller for the three sub-lanes");
            // the demo project's first video track has overlapping events: they go on different rolls
            const Track* t = trk(v0);
            std::vector<uint64_t> ids;
            for (const Event& e : t->events) ids.push_back(e.id);
            bool overlap = false, different = false;
            for (size_t k = 1; k < ids.size(); ++k) {
                const Event* a = evt(ids[k - 1]);
                const Event* b = evt(ids[k]);
                if (a && b && b->start < a->end() - 1e-6) {
                    overlap = true;
                    different = w.eventLayer(ids[k - 1]) != w.eventLayer(ids[k]);
                    check(w.eventRectOf(ids[k - 1]).top() != w.eventRectOf(ids[k]).top(), L + "overlapping events are drawn on different sub-lanes");
                }
            }
            check(!overlap || different, L + "overlapping events are on the A roll and the B roll");
            check(w.eventLayer(ids.front()) == 0, L + "the first event is on the A roll");
            uitest::shot(&w, out + "/" + tag + "_layers.png");
            QMenu m2;
            w.buildContextMenu(&m2, QPoint(60, w.headerRect(v0).top() + 20));
            check(find(m2, "Collapse Track Layers") != nullptr, L + "the command then reads Collapse Track Layers");
            if (QAction* a = find(m2, "Collapse Track Layers")) a->trigger();
            uitest::settle(100);
            check(!w.trackLayersExpanded(v0) && w.headerRect(v0).height() == h0, L + "and puts the track back");
            app.undo();
            app.undo();
        }
        uitest::shot(&w, out + "/" + tag + "_timeline.png");
    }
    return uitest::summary();
}
