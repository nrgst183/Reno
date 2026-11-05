// Dashboard window; see dashboard.h.
#include "dashboard.h"
#include "toolwin.h"
#include "pollrate.h"
#include "playback.h"
#include "recorder.h"
#include <QAction>
#include <QContextMenuEvent>
#include <QFrame>
#include <QPushButton>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QStorageInfo>
#include <QTimer>
#include <QVBoxLayout>
#include <chrono>
#include <cmath>
#include <deque>

namespace reno {
namespace app {

using namespace tw;

namespace {

const char* const kTileNames[] = {"Timecode", "Playback Frame Rate", "Master Peak", "Project", "Recording", "Disk Space"};

// A row of VEGAS's Dashboard workflow: a pictogram with the step's number, and the step's name in bold.
class StepButton : public QPushButton {
public:
    StepButton(int number, const QString& text, QWidget* parent) : QPushButton(text, parent), number_(number)
    {
        setFlat(true);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        QFont f = font();
        f.setBold(true);
        setFont(f);
        setFixedHeight(42);
        setStyleSheet("QPushButton { background: transparent; border: none; color: #f0f0f0; text-align: left; padding-left: 70px; }"
                      "QPushButton:hover:enabled { background: #5a5a5a; }"
                      "QPushButton:disabled { color: #8a8a8a; }");
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QPushButton::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const bool on = isEnabled();
        const QColor ink = on ? QColor(235, 235, 235) : QColor(120, 120, 120);
        const QColor accent = on ? QColor(236, 98, 70) : QColor(110, 110, 110);
        const QRectF art(14, (height() - 30) / 2.0, 30, 30);
        p.setPen(QPen(ink, 1.6));
        p.setBrush(Qt::NoBrush);
        switch (number_) {
        case 1: p.drawRect(art.adjusted(6, 6, -3, -3)); p.drawLine(QPointF(art.left() + 14, art.top() + 12), QPointF(art.left() + 22, art.top() + 20)); break;
        case 2: p.drawLine(QPointF(art.left() + 8, art.top() + 8), QPointF(art.left() + 24, art.top() + 8)); p.drawLine(QPointF(art.left() + 16, art.top() + 8), QPointF(art.left() + 16, art.top() + 25)); break;
        case 3: p.setBrush(accent); p.drawEllipse(art.adjusted(12, 10, -2, -2)); p.setBrush(ink); p.drawRect(art.adjusted(6, 6, -14, -14)); break;
        case 4: p.drawRect(art.adjusted(6, 8, -4, -8)); p.drawLine(art.topRight() + QPointF(-4, 8), art.bottomLeft() + QPointF(6, -8)); break;
        case 5: { QFont it = font(); it.setItalic(true); it.setBold(false); it.setPixelSize(20); p.setFont(it); p.drawText(art, Qt::AlignCenter, "fx"); break; }
        case 6: p.drawRect(art.adjusted(6, 6, -6, -6)); p.setBrush(accent); p.drawEllipse(art.adjusted(15, 15, -2, -2)); break;
        default: {   // Power User Mode: a gauge
            p.drawArc(art.adjusted(3, 6, -3, -6), 0, 180 * 16);
            p.setPen(QPen(accent, 2));
            p.drawLine(art.center() + QPointF(0, 4), art.center() + QPointF(8, -6));
            break;
        }
        }
        if (number_ >= 1 && number_ <= 6) {
            p.setPen(Qt::NoPen);
            p.setBrush(on ? QColor(235, 235, 235) : QColor(110, 110, 110));
            p.drawEllipse(QRectF(art.left() - 2, art.top() - 2, 13, 13));
            QFont nf = font();
            nf.setPixelSize(10);
            p.setFont(nf);
            p.setPen(QColor(40, 40, 40));
            p.drawText(QRectF(art.left() - 2, art.top() - 2, 13, 13), Qt::AlignCenter, QString::number(number_));
        }
    }

private:
    int number_;
};

// One tile: a title over a big reading (and a smaller line), or two bars for the meters.
class TileWidget : public QWidget {
public:
    TileWidget(const QString& title, QWidget* parent) : QWidget(parent), title_(title)
    {
        setMinimumSize(150, 74);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    QString big, small;
    bool meters = false;
    float levels[2] = {0, 0};
    QColor bigColour = text();

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = QRectF(rect()).adjusted(2, 2, -2, -2);
        p.setPen(QPen(QColor(80, 80, 80), 1));
        p.setBrush(gap());
        p.drawRoundedRect(r, 4, 4);
        QFont f = font();
        f.setPixelSize(10);
        p.setFont(f);
        p.setPen(QColor(160, 160, 160));
        p.drawText(r.adjusted(8, 4, -8, 0), Qt::AlignLeft | Qt::AlignTop, title_);
        if (meters) {
            const QRectF area = r.adjusted(10, 22, -10, -10);
            for (int k = 0; k < 2; ++k) {
                const double db = levels[k] > 1e-6f ? 20.0 * std::log10(levels[k]) : -60.0;
                const double frac = std::max(0.0, std::min(1.0, (db + 60.0) / 60.0));
                const QRectF groove(area.left(), area.top() + k * area.height() / 2 + 2, area.width(), area.height() / 2 - 6);
                p.fillRect(groove, QColor(20, 20, 20));
                QLinearGradient g(groove.left(), 0, groove.right(), 0);
                g.setColorAt(0, QColor(40, 170, 80));
                g.setColorAt(0.8, QColor(220, 200, 50));
                g.setColorAt(1, QColor(230, 60, 50));
                p.fillRect(QRectF(groove.left(), groove.top(), groove.width() * frac, groove.height()), g);
                p.setPen(QColor(200, 200, 200));
                p.drawText(groove.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight, db <= -59.9 ? QString::fromUtf8("-∞") : QString::number(db, 'f', 1));
            }
            return;
        }
        QFont b = font();
        b.setPixelSize(std::max(12, std::min(26, (int)(r.height() * 0.32))));
        b.setBold(true);
        p.setFont(b);
        p.setPen(bigColour);
        const QRectF bigR = r.adjusted(8, 18, -8, small.isEmpty() ? -6 : -20);
        p.drawText(bigR, Qt::AlignLeft | Qt::AlignVCenter, QFontMetrics(b).elidedText(big, Qt::ElideRight, (int)bigR.width()));
        if (!small.isEmpty()) {
            p.setFont(f);
            p.setPen(QColor(170, 170, 170));
            p.drawText(r.adjusted(8, 0, -8, -5), Qt::AlignLeft | Qt::AlignBottom, QFontMetrics(f).elidedText(small, Qt::ElideRight, (int)r.width() - 16));
        }
    }

private:
    QString title_;
};

}  // namespace

struct Dashboard::Impl {
    Playback* pb = nullptr;
    Recorder* rec = nullptr;
    TileWidget* tiles[TileCount] = {};
    bool shown[TileCount] = {true, true, true, true, true, true};
    QGridLayout* grid = nullptr;
    QWidget* tilesBox = nullptr;       // Reno's tiles: what Power User Mode shows
    QWidget* workflow = nullptr;       // VEGAS's numbered steps
    StepButton* steps[7] = {};
    QTimer* timer = nullptr;
    PollRate* pollRate = nullptr;          // slows the timer when nothing plays or records
    // frame rate: when frames were shown lately
    std::deque<std::pair<double, uint64_t>> samples;        // seconds, frames shown so far
    double fps = 0;
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    int diskTick = 0;

    void layout()
    {
        while (grid->count()) grid->takeAt(0);
        int n = 0;
        for (int k = 0; k < TileCount; ++k) {
            tiles[k]->setVisible(shown[k]);
            if (!shown[k]) continue;
            grid->addWidget(tiles[k], n / 2, n % 2);
            ++n;
        }
    }

    void read()
    {
        AppState& app = AppState::get();
        const Project& p = app.project();
        const double rate = p.settings.fps();
        const bool playing = pb && pb->playing();
        // the time
        tiles[Timecode]->big = qs(timecode(app.cursor(), rate));
        tiles[Timecode]->small = playing ? QString("Playing at %1x").arg(pb->rate(), 0, 'g', 3) : QString("Stopped");
        // frames a second over the last second
        const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const uint64_t shownNow = pb ? pb->framesShown() : 0;
        samples.push_back(std::make_pair(now, shownNow));
        while (samples.size() > 2 && now - samples.front().first > 1.0) samples.pop_front();
        if (playing && samples.size() >= 2 && now - samples.front().first > 0.2)
            fps = (double)(samples.back().second - samples.front().second) / (samples.back().first - samples.front().first);
        else if (!playing)
            fps = 0;
        if (playing) fps = pb->previewFrameRate();     // same figure as the preview's: frames really shown
        tiles[FrameRate]->big = playing ? QString("%1 fps").arg(fps, 0, 'f', 1) : QString("--");
        tiles[FrameRate]->small = QString("Project %1 fps; last frame %2 ms").arg(rate, 0, 'f', 3).arg(pb ? pb->lastRenderMs() : 0.0, 0, 'f', 1);
        // framedrop: late frames are dropped, the sound never waits; how many, and on how many threads
        if (pb && (playing || pb->droppedFrames() > 0))
            tiles[FrameRate]->small += QString("; %1 dropped%2").arg((qulonglong)pb->droppedFrames())
                                           .arg(playing && pb->renderThreadsInUse() > 1 ? QString(", %1 threads").arg(pb->renderThreadsInUse()) : QString());
        tiles[FrameRate]->bigColour = playing && fps < rate * 0.9 ? QColor(240, 170, 60) : text();
        // the master's peaks
        tiles[MasterPeak]->meters = true;
        tiles[MasterPeak]->levels[0] = pb ? pb->peakLeft() : 0.f;
        tiles[MasterPeak]->levels[1] = pb ? pb->peakRight() : 0.f;
        // the project
        size_t events = 0, video = 0, audio = 0;
        for (const Track& t : p.tracks) {
            events += t.events.size();
            (t.type == TrackType::Video ? video : audio)++;
        }
        tiles[ProjectInfo]->big = QString("%1x%2, %3 fps").arg(p.settings.width).arg(p.settings.height).arg(rate, 0, 'g', 5);
        tiles[ProjectInfo]->small = QString("%1 video, %2 audio tracks; %3 events; %4 media; %5 long; %6 Hz, %7")
                                        .arg(video).arg(audio).arg(events).arg(p.media.size()).arg(qs(timecode(p.length(), rate)))
                                        .arg(p.settings.sampleRate).arg(p.settings.pixelFormat == "32bit" ? (p.settings.pixelRange == "full" ? "32-bit float (full range)" : "32-bit float") : "8-bit");
        // recording
        if (rec) {
            float l = 0, r = 0;
            if (rec->inputOpen()) rec->inputLevel(l, r);
            const double peak = std::max(l, r);
            const QString level = rec->inputOpen() ? QString("input %1 dB").arg(peak > 1e-6 ? 20.0 * std::log10(peak) : -60.0, 0, 'f', 1) : QString("input closed");
            tiles[Recording]->big = rec->recording() ? QString("REC %1").arg(qs(timecode(rec->recordedSeconds(), rate))) : QString("%1 armed").arg(app.armedTracks.size());
            tiles[Recording]->bigColour = rec->recording() ? QColor(240, 80, 80) : text();
            tiles[Recording]->small = level;
        } else {
            tiles[Recording]->big = "--";
        }
        // the disk (checked once a second)
        if (diskTick++ % 10 == 0) {
            const QString dir = app.path().empty() ? QDir::homePath() : QFileInfo(qs(app.path())).absolutePath();
            const QStorageInfo disk(dir);
            if (disk.isValid() && disk.bytesAvailable() >= 0) {
                const double gb = (double)disk.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
                const double total = (double)disk.bytesTotal() / (1024.0 * 1024.0 * 1024.0);
                tiles[Disk]->big = QString("%1 GB free").arg(gb, 0, 'f', gb >= 100 ? 0 : 1);
                tiles[Disk]->small = QString("of %1 GB on %2").arg(total, 0, 'f', 0).arg(QDir::toNativeSeparators(disk.rootPath()));
            } else {
                tiles[Disk]->big = "--";
                tiles[Disk]->small.clear();
            }
        }
        for (TileWidget* t : tiles) t->update();
        // steps 4 to 6 wait for something on the timeline (VEGAS greys them in an empty project)
        if (steps[3]) {
            steps[3]->setEnabled(events > 0);
            steps[4]->setEnabled(events > 0);
            steps[5]->setEnabled(events > 0);
        }
        if (pollRate) pollRate->ticked(playing || (rec && (rec->recording() || rec->inputOpen())));
    }
};

Dashboard::Dashboard(Playback* playback, Recorder* recorder, QWidget* parent) : QWidget(parent), d(new Impl())
{
    setObjectName("dashboard");
    d->pb = playback;
    d->rec = recorder;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    d->tilesBox = new QWidget(this);
    d->tilesBox->setObjectName("dashboardTiles");
    auto* tv = new QVBoxLayout(d->tilesBox);
    tv->setContentsMargins(4, 4, 4, 4);
    d->grid = new QGridLayout();
    d->grid->setSpacing(4);
    tv->addLayout(d->grid);
    tv->addStretch(1);
    // VEGAS's Dashboard: the workflow in six steps; Power User Mode swaps it for Reno's readings (the tiles below)
    d->workflow = new QWidget(this);
    d->workflow->setObjectName("dashboardWorkflow");
    auto* wv = new QVBoxLayout(d->workflow);
    wv->setContentsMargins(0, 8, 0, 8);
    wv->setSpacing(0);
    const char* const stepNames[] = {"Add/Arrange Media", "Add Text and Titles", "Add Graphic Elements", "Add Video Transitions", "Add Video Effects", "Render As", "Power User Mode"};
    for (int k = 0; k < 7; ++k) {
        d->steps[k] = new StepButton(k + 1, stepNames[k], d->workflow);
        d->steps[k]->setObjectName(QString("dashboardStep%1").arg(k + 1));
        if (k == 6) {
            QFrame* rule = new QFrame(d->workflow);
            rule->setFrameShape(QFrame::HLine);
            rule->setStyleSheet("color: #6e6e6e;");
            wv->addSpacing(14);
            wv->addWidget(rule);
            wv->addSpacing(14);
        }
        wv->addWidget(d->steps[k]);
    }
    wv->addStretch(1);
    v->addWidget(d->workflow);
    v->addWidget(d->tilesBox, 1);
    d->tilesBox->hide();
    // each step runs the command of the same name (the window's own actions)
    auto run = [this](const char* action) {
        if (QWidget* w = window())
            if (QAction* a = w->findChild<QAction*>(action))
                if (a->isEnabled()) a->trigger();
    };
    QObject::connect(d->steps[0], &QPushButton::clicked, this, [run] { run("showMedia"); });
    QObject::connect(d->steps[1], &QPushButton::clicked, this, [run] { run("insertText"); });
    QObject::connect(d->steps[2], &QPushButton::clicked, this, [run] { run("showGenerators"); });
    QObject::connect(d->steps[3], &QPushButton::clicked, this, [run] { run("showTransitions"); });
    QObject::connect(d->steps[4], &QPushButton::clicked, this, [run] { run("showVideoFx"); });
    QObject::connect(d->steps[5], &QPushButton::clicked, this, [run] { run("render"); });
    QObject::connect(d->steps[6], &QPushButton::clicked, this, [this] {
        const bool tiles = !d->tilesBox->isVisible();
        d->tilesBox->setVisible(tiles);
        for (int k = 0; k < 6; ++k) d->steps[k]->setVisible(!tiles);
        d->steps[6]->setText(tiles ? "Workflow Mode" : "Power User Mode");
    });
    QSettings st;
    for (int k = 0; k < TileCount; ++k) {
        d->tiles[k] = new TileWidget(kTileNames[k], d->tilesBox);
        d->tiles[k]->setObjectName(QString("dashboardTile%1").arg(k));
        d->shown[k] = st.value(QString("dashboard/tile%1").arg(k), true).toBool();
    }
    d->layout();
    d->timer = new QTimer(this);
    d->timer->setInterval(100);
    Impl* impl = d.get();
    QObject::connect(d->timer, &QTimer::timeout, this, [impl] { impl->read(); });
    d->timer->start();
    d->pollRate = new PollRate(this, d->timer, 100);
    d->read();
}

Dashboard::~Dashboard() {}

void Dashboard::setTileShown(Tile t, bool on)
{
    if (t < 0 || t >= TileCount) return;
    d->shown[t] = on;
    QSettings().setValue(QString("dashboard/tile%1").arg((int)t), on);
    d->layout();
}


QString Dashboard::tileText(Tile t) const
{
    if (t < 0 || t >= TileCount) return QString();
    return d->tiles[t]->small.isEmpty() ? d->tiles[t]->big : d->tiles[t]->big + "\n" + d->tiles[t]->small;
}

double Dashboard::measuredFps() const { return d->fps; }

void Dashboard::update1() { d->read(); }

void Dashboard::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);
    for (int k = 0; k < TileCount; ++k) {
        QAction* a = menu.addAction(kTileNames[k]);
        a->setCheckable(true);
        a->setChecked(d->shown[k]);
        QObject::connect(a, &QAction::toggled, this, [this, k](bool on) { setTileShown((Tile)k, on); });
    }
    menu.exec(e->globalPos());
}

}  // namespace app
}  // namespace reno
