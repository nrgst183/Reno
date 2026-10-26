// Panels and dialogs in src/app/panels.cpp.
//
//   panels_test <output folder> [testfx.ofx ...] [--vst file ...]
//
// QT_QPA_PLATFORM=offscreen. The folder gets the pictures and the rendered files.
#include "../../src/app/mainwindow.h"
#include "../../src/app/panels.h"
#include "../../src/app/playback.h"
#include "../../src/media/media.h"
#include "uitestlib.h"
#include <QStandardPaths>
#include <QCoreApplication>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QTreeWidget>
#include <QUrl>
#include <cstdio>
#include <memory>
#include <cstdlib>

using namespace reno;
using namespace reno::app;
using uitest::check;

namespace {

QString gOut;

template <class T>
T* kid(QWidget* w, const char* name) { return w ? w->findChild<T*>(name) : nullptr; }

// Calls step every 40 ms with the modal dialog that is open; step returns true when it is finished with it.
// If it has not finished after 'limitMs' the dialog is closed and the test fails.
void drive(std::function<bool(QWidget*)> step, const std::string& what, int limitMs = 60000)
{
    auto* t = new QTimer(qApp);
    auto* clock = new QElapsedTimer();
    clock->start();
    QObject::connect(t, &QTimer::timeout, [=] {
        QWidget* m = QApplication::activeModalWidget();
        if (!m) return;
        if (step(m)) {
            t->stop();
            t->deleteLater();
            delete clock;
        } else if (clock->elapsed() > limitMs) {
            check(false, what + ": dialog did not finish in time");
            t->stop();
            t->deleteLater();
            delete clock;
            m->close();
        }
    });
    t->start(40);
}

QTreeWidget* tree(QWidget* w, const char* name)
{
    auto* t = kid<QTreeWidget>(w, name);
    return t;
}

// the entries (not group headings) of a plug-in list
std::vector<QTreeWidgetItem*> leaves(QTreeWidget* t)
{
    std::vector<QTreeWidgetItem*> out;
    for (int g = 0; g < t->topLevelItemCount(); ++g)
        for (int k = 0; k < t->topLevelItem(g)->childCount(); ++k) out.push_back(t->topLevelItem(g)->child(k));
    return out;
}

int eventCount()
{
    int n = 0;
    for (const auto& t : AppState::get().project().tracks) n += (int)t.events.size();
    return n;
}

void testMediaBin()
{
    AppState& app = AppState::get();
    MediaBin bin;
    bin.resize(760, 380);
    bin.show();
    uitest::settle(300);
    QTreeWidget* list = tree(&bin, "mediaList");
    check(list != nullptr, "media bin: has a list");
    if (!list) return;
    check(list->topLevelItemCount() == (int)app.project().media.size() && list->topLevelItemCount() == 4, "media bin: lists the demo project's media",
          std::to_string(list->topLevelItemCount()));
    check(list->topLevelItem(0)->text(1) == "Video" && list->topLevelItem(2)->text(1) == "Picture" && list->topLevelItem(3)->text(1) == "Audio", "media bin: kinds");
    check(list->topLevelItem(0)->text(2) == "00:00:10;00", "media bin: duration", list->topLevelItem(0)->text(2).toStdString());
    bool thumbs = true;
    for (int k = 0; k < list->topLevelItemCount(); ++k) {
        const QImage im = list->topLevelItem(k)->icon(0).pixmap(96, 54).toImage();
        // a thumbnail is not the plain dark placeholder: some pixel differs from the corner
        bool differs = false;
        for (int y = 0; y < im.height() && !differs; y += 3)
            for (int x = 0; x < im.width(); x += 3)
                if (im.pixel(x, y) != im.pixel(0, 0)) { differs = true; break; }
        thumbs = thumbs && differs;
    }
    check(thumbs, "media bin: thumbnails arrive from the background thread (video, picture and waveform)");
    uitest::shot(&bin, gOut + "/media.png");

    // import and removal show up
    std::string err;
    (void)err;
    const uint64_t extra = app.importMedia("synth:9", &err);
    uitest::settle(200);
    check(list->topLevelItemCount() == 5, "media bin: an import appears");
    // an unused one can be removed
    list->clearSelection();
    list->topLevelItem(4)->setSelected(true);
    auto* remove = kid<QPushButton>(&bin, "removeMedia");
    check(remove && remove->isEnabled(), "media bin: remove button is enabled with a selection");
    remove->click();
    uitest::settle(100);
    check(list->topLevelItemCount() == 4 && !app.project().findMedia(extra), "media bin: unused media is removed from the project");
    // a used one is refused, with a message
    list->clearSelection();
    list->topLevelItem(0)->setSelected(true);
    std::string said;
    drive([&](QWidget* m) {
        auto* box = qobject_cast<QMessageBox*>(m);
        if (!box) return false;
        said = box->text().toStdString();
        uitest::shot(box, gOut + "/media_remove_refused.png");
        box->accept();
        return true;
    }, "remove used media");
    remove->click();
    uitest::settle(100);
    check(list->topLevelItemCount() == 4 && app.project().findMedia(app.project().media[0].id), "media bin: media used by an event is not removed");
    check(said.find("still used") != std::string::npos, "media bin: says why", said);

    // a double-click adds it to the timeline at the cursor
    app.setCursor(5.0);
    list->clearSelection();
    const int before = eventCount();
    QTreeWidgetItem* snd = list->topLevelItem(3);
    uitest::mouseDoubleClick(list->viewport(), list->visualItemRect(snd).center());
    uitest::settle(100);
    check(eventCount() == before + 1, "media bin: double-click adds an event", std::to_string(before) + " -> " + std::to_string(eventCount()));
    bool atCursor = false;
    for (const auto& t : app.project().tracks)
        for (const auto& e : t.events)
            if (e.mediaId == app.project().media[3].id && std::fabs(e.start - 5.0) < 0.05) atCursor = true;
    check(atCursor, "media bin: the new event starts at the cursor");
    // the Add to Timeline button, two items one after the other
    list->clearSelection();
    list->topLevelItem(0)->setSelected(true);
    list->topLevelItem(2)->setSelected(true);
    const int before2 = eventCount();
    kid<QPushButton>(&bin, "addToTimeline")->click();
    uitest::settle(100);
    check(eventCount() >= before2 + 2, "media bin: Add to Timeline adds the selected media", std::to_string(before2) + " -> " + std::to_string(eventCount()));
    app.undo();
    app.undo();
    app.undo();
    uitest::settle(100);

    // what a drag carries
    const QModelIndex ix = list->indexFromItem(list->topLevelItem(1));
    QMimeData* mime = list->model()->mimeData(QModelIndexList() << ix);
    const uint64_t id1 = app.project().media[1].id;
    check(mime && mime->hasFormat(kMimeMedia) && mime->data(kMimeMedia).toStdString() == std::to_string(id1), "media bin: a drag carries the media id as text",
          mime ? mime->data(kMimeMedia).toStdString() : "no mime data");
    delete mime;
    check(list->dragEnabled(), "media bin: items can be dragged");

    // files dropped from the desktop are imported (a real file: a second of black and a tone, written here)
    const QString real = gOut + "/render_loop.mp4";
    {
        EncodeSettings es;
        es.path = real.toStdString();
        es.width = 320;
        es.height = 180;
        std::unique_ptr<MediaWriter> w = createMediaWriter(es, &err);
        if (w) {
            ImagePtr black = Image::make(320, 180);
            std::vector<float> l(1600), r(1600);
            for (int k = 0; k < 30; ++k) {
                w->writeVideo(*black);
                for (int n = 0; n < 1600; ++n) l[n] = r[n] = 0.2f * (float)std::sin((k * 1600 + n) * 0.05);
                w->writeAudio(l.data(), r.data(), 1600);
            }
            w->finish(&err);
        }
    }
    check(QFileInfo::exists(real), "media bin: a file to drop was written", err);
    if (QFileInfo::exists(real)) {
        QMimeData dropData;
        dropData.setUrls(QList<QUrl>() << QUrl::fromLocalFile(real));
        QDragEnterEvent enter(QPoint(50, 50), Qt::CopyAction, &dropData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &enter);
        check(enter.isAccepted(), "media bin: a dragged file is accepted over the list");
        QDropEvent drop(QPointF(50, 50), Qt::CopyAction, &dropData, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &drop);
        uitest::settle(400);
        check(list->topLevelItemCount() == 5 && list->topLevelItem(4)->text(0) == "render_loop.mp4", "media bin: a dropped file is imported",
              std::to_string(list->topLevelItemCount()));
        uitest::shot(&bin, gOut + "/media_after_drop.png");
        // and a file that is not media is reported in one message
        QString bad = gOut + "/notmedia.txt";
        QFile f(bad);
        f.open(QIODevice::WriteOnly);
        f.write("hello");
        f.close();
        QString msg;
        drive([&](QWidget* m) {
            auto* box = qobject_cast<QMessageBox*>(m);
            if (!box) return false;
            msg = box->text();
            box->accept();
            return true;
        }, "import failure message");
        QMimeData dropBad;
        dropBad.setUrls(QList<QUrl>() << QUrl::fromLocalFile(bad));
        QDragEnterEvent enter2(QPoint(50, 50), Qt::CopyAction, &dropBad, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &enter2);
        QDropEvent drop2(QPointF(50, 50), Qt::CopyAction, &dropBad, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(list->viewport(), &drop2);
        uitest::settle(100);
        check(msg.contains("notmedia.txt") && list->topLevelItemCount() == 5, "media bin: a failed import is reported", msg.toStdString() + " count " + std::to_string(list->topLevelItemCount()));
    }
    // an offline item is marked
    app.edit("Test Offline", [](Project& p) {
        MediaItem m;
        m.id = p.newId();
        m.path = "/nowhere/missing.mp4";
        m.name = "missing.mp4";
        m.offline = true;
        p.media.push_back(m);
    });
    uitest::settle(200);
    QTreeWidgetItem* last = list->topLevelItem(list->topLevelItemCount() - 1);
    check(last->text(1) == "Offline", "media bin: offline media is marked", last->text(1).toStdString());
    uitest::shot(&bin, gOut + "/media_offline.png");
    app.undo();
}

void testBrowsers()
{
    std::map<FxBrowser::Kind, std::pair<const char*, const char*>> names = {
        {FxBrowser::VideoFx, {"videofx", kMimeVideoFx}}, {FxBrowser::AudioFx, {"audiofx", kMimeAudioFx}},
        {FxBrowser::Generators, {"generators", kMimeGenerator}}, {FxBrowser::Transitions, {"transitions", kMimeTransition}}};
    const std::vector<VideoFxEntry> video = Catalog::get().video();
    const std::vector<AudioFxEntry> audio = Catalog::get().audio();
    int nf = 0, ng = 0, nt = 0, broken = 0;
    for (const auto& e : video) { nf += e.filter; ng += e.generator; nt += e.transition; broken += !e.ok; }
    std::printf("catalog: %d video (%d filters, %d generators, %d transitions, %d not working), %d audio\n", (int)video.size(), nf, ng, nt, broken, (int)audio.size());
    for (const auto& kv : names) {
        const FxBrowser::Kind kind = kv.first;
        FxBrowser b(kind);
        std::vector<std::pair<FxBrowser::Kind, std::string>> got;
        b.onActivated = [&](FxBrowser::Kind k, const std::string& data) { got.push_back(std::make_pair(k, data)); };
        b.resize(320, 420);
        b.show();
        uitest::settle(100);
        QTreeWidget* t = tree(&b, "fxTree");
        const size_t expect = kind == FxBrowser::VideoFx ? nf : kind == FxBrowser::Generators ? ng : kind == FxBrowser::Transitions ? nt : audio.size();
        const std::string n = kv.second.first;
        check(t && leaves(t).size() == expect, n + ": lists the right entries", std::to_string(t ? leaves(t).size() : 0) + " of " + std::to_string(expect));
        if (!t || leaves(t).empty()) { uitest::shot(&b, gOut + "/fx_" + QString::fromStdString(n) + ".png"); continue; }
        uitest::shot(&b, gOut + "/fx_" + QString::fromStdString(n) + ".png");

        // a search narrows the list, whatever the case
        QTreeWidgetItem* first = nullptr;
        for (auto* it : leaves(t)) if (it->data(0, Qt::UserRole + 2).toBool()) { first = it; break; }
        if (first) {
            QString word = first->text(0).split(' ').first().left(5);
            auto* box = kid<QLineEdit>(&b, "searchBox");
            box->setText(word.toUpper());
            uitest::settle(50);
            const auto found = leaves(t);
            bool all = !found.empty();
            for (auto* it : found) all = all && (it->text(0).contains(word, Qt::CaseInsensitive) || it->parent()->text(0).contains(word, Qt::CaseInsensitive));
            check(all && found.size() <= expect, n + ": the search box filters as you type (" + word.toStdString() + ")", std::to_string(found.size()));
            uitest::shot(&b, gOut + "/fx_" + QString::fromStdString(n) + "_search.png");
            box->setText("zzzz-no-such-effect");
            uitest::settle(50);
            check(leaves(t).empty(), n + ": a search with no match shows nothing");
            box->clear();
            uitest::settle(50);
            check(leaves(t).size() == expect, n + ": clearing the search brings everything back");
        }

        // a double-click reports the same text a drag carries
        std::vector<QTreeWidgetItem*> all = leaves(t);
        first = nullptr;
        for (auto* it : all) if (it->data(0, Qt::UserRole + 2).toBool()) { first = it; break; }
        if (first) {
            t->scrollToItem(first);
            uitest::settle(20);
            uitest::mouseDoubleClick(t->viewport(), t->visualItemRect(first).center());
            uitest::settle(20);
            QMimeData* m = t->model()->mimeData(QModelIndexList() << t->indexFromItem(first));
            const std::string carried = m ? m->data(kv.second.second).toStdString() : "";
            check(got.size() == 1 && got[0].first == kind && got[0].second == carried && !carried.empty(), n + ": double-click calls onActivated with the dragged text",
                  got.empty() ? "no call" : got[0].second);
            if (kind != FxBrowser::AudioFx) check(carried.find('\n') != std::string::npos, n + ": the text is \"identifier\\nlabel\"");
            else {
                Json j;
                check(Json::parse(carried, j), n + ": the text is the plug-in as JSON");
            }
            delete m;
            // Enter does the same
            got.clear();
            t->setCurrentItem(first);
            uitest::key(t, Qt::Key_Return);
            check(got.size() == 1, n + ": Enter activates the entry");
        }
        // headings are not draggable or activatable
        got.clear();
        QTreeWidgetItem* head = t->topLevelItem(0);
        QMimeData* hm = t->model()->mimeData(QModelIndexList() << t->indexFromItem(head));
        check(hm == nullptr || !hm->hasFormat(kv.second.second), n + ": a group heading carries nothing");
        delete hm;
        // entries that do not work are grey and cannot be dragged or activated
        for (auto* it : all) {
            if (it->data(0, Qt::UserRole + 2).toBool()) continue;
            QMimeData* bm = t->model()->mimeData(QModelIndexList() << t->indexFromItem(it));
            check(it->foreground(0).color().lightness() < 150 && !it->toolTip(0).isEmpty() && !(it->flags() & Qt::ItemIsDragEnabled) && (bm == nullptr || !bm->hasFormat(kv.second.second)),
                  n + ": a broken entry is grey, explained, and cannot be dragged");
            delete bm;
            t->setCurrentItem(it);
            uitest::key(t, Qt::Key_Return);
            check(got.empty(), n + ": a broken entry is not activated");
            break;
        }
    }

    // an empty catalog: Video FX still lists Reno's built-in effects; a window with nothing to list has a line of text and a button
    Catalog::get().scanInProcess(std::vector<std::string>(), std::vector<std::string>());
    {
        FxBrowser v(FxBrowser::VideoFx);
        v.resize(320, 420);
        v.show();
        uitest::settle(100);
        QTreeWidget* vt = tree(&v, "fxTree");
        check(vt && vt->isVisible() && !leaves(vt).empty() && !kid<QWidget>(&v, "noPlugins")->isVisible(), "browser: with nothing scanned Video FX lists the built-in effects");
    }
    FxBrowser b(FxBrowser::Transitions);
    b.resize(320, 420);
    b.show();
    uitest::settle(100);
    auto* page = kid<QWidget>(&b, "noPlugins");
    auto* button = kid<QPushButton>(&b, "lookForPlugins");
    check(page && page->isVisible() && button && button->isVisible(), "browser: an empty catalog shows the explanation and the button");
    uitest::shot(&b, gOut + "/fx_empty.png");
    // the button runs the scan; with no reno_cli next to this program it says so
    QString text;
    const bool hadProbe = fileExists(defaultProbeExe());
    if (!hadProbe) {
        // only driven when it will come: a driver left waiting would touch 'text' after this function returned
        drive([&](QWidget* m) {
            auto* box = qobject_cast<QMessageBox*>(m);
            if (!box) return false;
            text = box->text();
            uitest::shot(box, gOut + "/scan_missing_probe.png");
            box->accept();
            return true;
        }, "missing probe");
        button->click();
        uitest::settle(100);
        check(text.contains("missing") && text.contains("reno_cli"), "scan: a missing reno_cli is named plainly", text.toStdString());
    }
}

void testPreview()
{
    AppState& app = AppState::get();
    app.setCursor(2.5);
    std::unique_ptr<Playback> owned(new Playback());
    Playback& pb = *owned;
    PreviewWidget pv(&pb);
    pv.resize(840, 420);
    pv.show();
    QElapsedTimer first;
    first.start();
    do uitest::settle(50); while (pb.lastFrame().isNull() && first.elapsed() < 5000);
    auto* view = kid<QWidget>(&pv, "previewPicture");
    check(view != nullptr, "preview: has a picture area");
    check(!pb.lastFrame().isNull(), "preview: a first picture is asked for when shown");
    pb.requestFrame();
    uitest::settle(400);
    const QImage shown = uitest::shot(&pv, gOut + "/preview.png");
    // the picture area must hold something that is not the dark background
    const QImage v = view->grab().toImage();
    int bright = 0;
    for (int y = 0; y < v.height(); y += 4)
        for (int x = 0; x < v.width(); x += 4)
            if (qGray(v.pixel(x, y)) > 60) ++bright;
    check(bright > 200, "preview: shows a picture after requestFrame", std::to_string(bright));
    // kept in shape: the picture is 16:9 here, the area is wider, so there are bars at the sides, in VEGAS's dark grey
    check(qGray(v.pixel(2, v.height() / 2)) == 45, "preview: the picture keeps its shape with bars at the sides");
    // as VEGAS shows them under the picture: the frame number at the cursor, the project's and the preview's sizes
    auto* frame = kid<QLabel>(&pv, "frameNumber");
    check(frame && frame->text() == "75", "preview: the frame number at the cursor", frame ? frame->text().toStdString() : "");
    app.setCursor(7.0);
    uitest::settle(200);
    check(frame->text() == "210", "preview: the frame number follows the cursor", frame->text().toStdString());
    check(frame->toolTip().contains(" ms"), "preview: tells how long the picture took", frame->toolTip().toStdString());
    auto* fmt = kid<QLabel>(&pv, "projectFormat");
    check(fmt && fmt->text() == "1920x1080x32, 30.000p", "preview: shows the project format", fmt ? fmt->text().toStdString() : "");
    app.edit("Test Format", [](Project& p) { p.settings.fpsNum = 30000; p.settings.fpsDen = 1001; });
    uitest::settle(100);
    check(fmt->text().contains("29.970p"), "preview: the format follows project changes", fmt->text().toStdString());
    app.undo();

    // the tool bar
    auto* bypass = kid<QAbstractButton>(&pv, "bypassFx");
    bypass->click();
    check(app.bypassAllFx, "preview: Bypass FX sets the flag");
    bypass->click();
    check(!app.bypassAllFx, "preview: Bypass FX clears the flag");
    int outFx = 0;
    pv.onOpenFx = [&](const FxTarget& t, uint64_t) { if (t.kind == FxTarget::VideoOut) ++outFx; };
    kid<QAbstractButton>(&pv, "videoOutputFx")->click();
    check(outFx == 1, "preview: the Video Output FX button opens the output's effects");
    // preview quality: the size the picture is drawn at before it is fitted to the window
    pv.setQuality(PreviewQuality::Best, PreviewSize::Full);
    check(pv.previewSize() == QSize(1920, 1080), "preview: Best (Full) is the project's size", strfmt("%dx%d", pv.previewSize().width(), pv.previewSize().height()));
    pv.setQuality(PreviewQuality::Good, PreviewSize::Half);
    check(pv.previewSize() == QSize(960, 540), "preview: Good (Half) is half of it");
    pv.setQuality(PreviewQuality::Draft, PreviewSize::Quarter);
    check(pv.previewSize() == QSize(120, 68), "preview: Draft (Quarter) a quarter of a quarter", strfmt("%dx%d", pv.previewSize().width(), pv.previewSize().height()));
    uitest::settle(100);
    uitest::shot(&pv, gOut + "/preview_draft.png");
    auto* sizes = kid<QLabel>(&pv, "previewSize");
    auto* quality = kid<QAbstractButton>(&pv, "previewQuality");
    check(sizes && sizes->text().startsWith("120x68") && quality && quality->text().startsWith("Draft (Quarter)"), "preview: the preview size and quality are shown",
          sizes ? (sizes->text() + " / " + (quality ? quality->text() : QString())).toStdString() : "");
    pv.setQuality(PreviewQuality::Good, PreviewSize::Auto);

    // the transport bar
    TransportBar tb(&pb);
    tb.show();
    auto* loop = kid<QAbstractButton>(&tb, "loopButton");
    app.loopOn = false;
    loop->click();
    check(app.loopOn && loop->isChecked(), "transport: loop button turns looping on");
    loop->click();
    check(!app.loopOn, "transport: loop button turns looping off");
    app.loopOn = true;
    app.notify(kCursor);
    check(loop->isChecked(), "transport: loop button follows outside changes");
    app.loopOn = false;
    app.notify(kCursor);
    check(!kid<QAbstractButton>(&tb, "recordButton")->isEnabled(), "transport: Record is there but not available");
    kid<QAbstractButton>(&tb, "goToStart")->click();
    check(app.cursor() == 0.0, "transport: go to start");
    kid<QAbstractButton>(&tb, "goToEnd")->click();
    check(app.cursor() == app.project().length(), "transport: go to end");
    app.setCursor(1.0);
    kid<QAbstractButton>(&tb, "nextFrame")->click();
    check(std::fabs(app.cursor() - (1.0 + 1.0 / 30.0)) < 1e-9, "transport: next frame");
    kid<QAbstractButton>(&tb, "prevFrame")->click();
    check(std::fabs(app.cursor() - 1.0) < 1e-9, "transport: previous frame");
    kid<QAbstractButton>(&tb, "playButton")->click();
    check(pb.playing(), "transport: the play button plays");
    uitest::settle(700);
    uitest::shot(&pv, gOut + "/preview_playing.png");
    uitest::shot(&tb, gOut + "/transport.png");
    check(app.cursor() > 1.2, "preview: the timecode runs while playing", std::to_string(app.cursor()));
    kid<QAbstractButton>(&tb, "pauseButton")->click();
    check(!pb.playing() && app.cursor() > 1.2, "transport: the pause button pauses where it is");
    kid<QAbstractButton>(&tb, "playButton")->click();
    uitest::settle(200);
    kid<QAbstractButton>(&tb, "stopButton")->click();
    check(!pb.playing() && app.cursor() > 1.2, "transport: stop goes back to where playing began");
    kid<QAbstractButton>(&tb, "playFromStart")->click();
    check(pb.playing(), "transport: play from start plays");
    uitest::settle(150);
    check(app.cursor() < 1.0, "transport: ... from the start", std::to_string(app.cursor()));
    pb.pause();
    app.setCursor(2.5);
    uitest::settle(300);
}

void testPreviewTeardown()
{
    std::unique_ptr<Playback> owned(new Playback());
    Playback& pb = *owned;
    {
        PreviewWidget pv(&pb);
        pv.show();
        uitest::settle(100);
    }
    check(!pb.onFrame, "preview: the frame callback is cleared when the panel goes");
    pb.requestFrame();
    QElapsedTimer wait;
    wait.start();
    while (pb.lastFrame().isNull() && wait.elapsed() < 5000) uitest::settle(50);
    uitest::settle(100);
    check(!pb.lastFrame().isNull(), "preview: a frame arriving after the panel is gone does no harm");
    // a Playback that goes while its picture thread is making a frame (or has one on its way to the UI): the frame is dropped and
    // nothing touches the destroyed Playback (run under ASan)
    for (int k = 0; k < 5; ++k) {
        std::unique_ptr<Playback> brief(new Playback());
        brief->requestFrame();
        if (k) uitest::settle(k * 7);
        brief.reset();
        uitest::settle(30);
    }
    check(true, "preview: a Playback destroyed with a frame on its way does no harm");
}

void testProperties()
{
    AppState& app = AppState::get();
    // cancel changes nothing
    drive([&](QWidget* m) {
        check(kid<QSpinBox>(m, "width")->value() == 1920 && kid<QComboBox>(m, "frameRate")->currentText().startsWith("30 fps"), "properties: the dialog starts with the project's settings");
        uitest::shot(m, gOut + "/properties.png");
        kid<QSpinBox>(m, "width")->setValue(640);
        kid<QPushButton>(m, "cancel")->click();
        return true;
    }, "properties cancel");
    runProjectProperties(nullptr);
    check(app.project().settings.width == 1920, "properties: Cancel changes nothing");

    drive([&](QWidget* m) {
        kid<QSpinBox>(m, "width")->setValue(1280);
        kid<QSpinBox>(m, "height")->setValue(720);
        auto* tmpl = kid<QComboBox>(m, "template");
        check(tmpl->currentText().startsWith("1280 x 720"), "properties: the template follows the size", tmpl->currentText().toStdString());
        auto* rate = kid<QComboBox>(m, "frameRate");
        rate->setCurrentIndex(3);
        check(kid<QSpinBox>(m, "fpsNum")->value() == 30000 && kid<QSpinBox>(m, "fpsDen")->value() == 1001 && !kid<QSpinBox>(m, "fpsNum")->isEnabled(), "properties: 29.97 is stored as 30000/1001");
        auto* sr = kid<QComboBox>(m, "sampleRate");
        sr->setCurrentIndex(sr->findData(44100));
        uitest::shot(m, gOut + "/properties_changed.png");
        kid<QPushButton>(m, "ok")->click();
        return true;
    }, "properties ok");
    runProjectProperties(nullptr);
    const ProjectSettings& s = app.project().settings;
    check(s.width == 1280 && s.height == 720 && s.fpsNum == 30000 && s.fpsDen == 1001 && s.sampleRate == 44100, "properties: OK applies the settings");
    check(app.undoName() == "Project Properties", "properties: it is one undo step named Project Properties", app.undoName());

    drive([&](QWidget* m) {
        check(kid<QComboBox>(m, "frameRate")->currentText().startsWith("29.97") && kid<QSpinBox>(m, "width")->value() == 1280, "properties: reopened with the new settings");
        auto* rate = kid<QComboBox>(m, "frameRate");
        rate->setCurrentIndex(rate->count() - 1);
        check(kid<QSpinBox>(m, "fpsNum")->isEnabled(), "properties: Custom enables the fraction");
        kid<QSpinBox>(m, "fpsNum")->setValue(48);
        kid<QSpinBox>(m, "fpsDen")->setValue(1);
        kid<QSpinBox>(m, "width")->setValue(641);
        uitest::shot(m, gOut + "/properties_custom.png");
        kid<QPushButton>(m, "ok")->click();
        return true;
    }, "properties custom");
    runProjectProperties(nullptr);
    check(s.fpsNum == 48 && s.fpsDen == 1 && s.width == 640, "properties: a custom rate is kept and sizes are made even");
    app.undo();
    app.undo();
    check(app.project().settings.width == 1920 && app.project().settings.fpsNum == 30, "properties: undo restores the old settings");
}

// waits until the dialog's close button shows (render or scan finished)
bool finishedPage(QWidget* m, const char* closeName) { auto* c = kid<QPushButton>(m, closeName); return c && !c->isHidden(); }

// (File > Render As is tested in render_test.cpp)

void testScan()
{
    const std::string exe = defaultProbeExe();
    // a built reno_cli: $RENO_CLI, else one on the PATH, else one in the folder above this program's or in a "cli" folder there
    std::string real = qEnvironmentVariable("RENO_CLI").toStdString();
    if (real.empty() || !fileExists(real)) real = QStandardPaths::findExecutable("reno_cli").toStdString();
    const QString here = QCoreApplication::applicationDirPath();
    for (const QString& c : {here + "/../reno_cli", here + "/cli/reno_cli", here + "/../cli/reno_cli"})
        if (real.empty() || !fileExists(real)) real = QFileInfo(c).absoluteFilePath().toStdString();
    if (!fileExists(real)) { check(false, "scan: a real reno_cli is needed (set RENO_CLI to the built one)"); return; }
    // copied next to this program, where Reno looks for it, unless it was built there
    const bool copied = QFileInfo(QString::fromStdString(real)).canonicalFilePath() != QFileInfo(QString::fromStdString(exe)).canonicalFilePath();
    if (copied) {
        QFile::remove(QString::fromStdString(exe));
        check(QFile::copy(QString::fromStdString(real), QString::fromStdString(exe)), "scan: copied reno_cli next to the test program");
    }
    QFile::setPermissions(QString::fromStdString(exe), QFile::permissions(QString::fromStdString(exe)) | QFile::ExeOwner | QFile::ExeUser);
    int notified = 0;
    const int id = AppState::get().addListener([&](int c) { if (c & kCatalog) ++notified; });
    int stage = 0;
    QString summary, title;
    drive([&](QWidget* m) {
        if (stage == 0) { uitest::shot(m, gOut + "/scan_running.png"); stage = 1; }
        if (!finishedPage(m, "scanClose")) return false;
        summary = kid<QLabel>(m, "scanSummary")->text();
        title = kid<QLabel>(m, "scanTitle")->text();
        uitest::shot(m, gOut + "/scan_done.png");
        kid<QPushButton>(m, "scanClose")->click();
        return true;
    }, "scan", 120000);
    runPluginScan(nullptr, true);
    check(title == "Finished" && summary.contains("video") && summary.contains("audio"), "scan: runs a real scan and ends with a summary", title.toStdString() + " / " + summary.toStdString());
    check(notified >= 1, "scan: tells the lists the catalog changed");
    std::printf("scan summary: %s\n", summary.toStdString().c_str());

    // cancel at once
    stage = 0;
    drive([&](QWidget* m) {
        if (stage == 0) { if (auto* c = kid<QPushButton>(m, "scanCancel")) c->click(); stage = 1; return false; }
        if (!finishedPage(m, "scanClose")) return false;
        title = kid<QLabel>(m, "scanTitle")->text();
        kid<QPushButton>(m, "scanClose")->click();
        return true;
    }, "scan cancel", 120000);
    runPluginScan(nullptr, true);
    check(title == "Stopped" || title == "Finished", "scan: Cancel ends the scan", title.toStdString());
    AppState::get().removeListener(id);
    if (copied) QFile::remove(QString::fromStdString(exe));
}

void testAbout()
{
    QString facts, notices;
    drive([&](QWidget* m) {
        facts = kid<QLabel>(m, "aboutFacts")->text();
        notices = kid<QWidget>(m, "aboutNotices") ? kid<QWidget>(m, "aboutNotices")->property("plainText").toString() : QString();
        uitest::shot(m, gOut + "/about.png");
        m->close();
        return true;
    }, "about");
    showAbout(nullptr);
    check(facts.contains(QString::fromStdString(mediaBackendVersion())) && facts.contains(qVersion()), "about: names the FFmpeg and Qt versions", facts.toStdString());
    check(notices.contains("BSD") && notices.contains("LGPL") && notices.contains("OpenFX") && notices.contains("VST") && notices.contains("FFmpeg") && notices.contains("Qt"),
          "about: lists the third-party notices");
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication qapp(argc, argv);
    QApplication::setOrganizationName("RenoPanelsTest");
    QApplication::setApplicationName("panels_test");
    applyTheme();
    const QStringList args = QApplication::arguments();
    if (args.size() < 2) { qWarning("usage: panels_test <output folder> [file.ofx ...] [--vst file ...]"); return 2; }
    gOut = args[1];
    QDir().mkpath(gOut);
    QSettings().clear();
    std::vector<std::string> ofxFiles, vstFiles;
    bool vst = false;
    for (int k = 2; k < args.size(); ++k) {
        if (args[k] == "--vst") { vst = true; continue; }
        (vst ? vstFiles : ofxFiles).push_back(args[k].toStdString());
    }
    uitest::setup(ofxFiles, vstFiles);
    uitest::buildDemoProject();

    testPreview();
    testPreviewTeardown();
    if (qEnvironmentVariableIsSet("PANELS_PREVIEW_ONLY")) return uitest::summary();
    testMediaBin();
    testBrowsers();
    testProperties();
    testAbout();
    testScan();
    // the Playback objects and their threads are gone: the program ends the ordinary way, as Reno does
    return uitest::summary();
}
