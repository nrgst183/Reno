// Helpers shared by the timeline files: colors, ruler steps, ripple and the track level text; what the track headers'
// newer parts do (compositing parent and child, track group rows, the multipurpose send row, the output meters).
#include "timeline_internal.h"
#include "editdialogs.h"
#include "fxautomation.h"
#include <QActionGroup>

namespace reno {
namespace app {

namespace tl {

QColor kWindow(55, 55, 55);
QColor kPanel(70, 70, 70);
QColor kHeaderBg(45, 45, 45);
QColor kHeaderCurrent(130, 130, 130);
QColor kLane(45, 45, 45);
QColor kBusLane(58, 58, 62);
QColor kBelowBg(35, 35, 35);
QColor kLine(35, 35, 35);
QColor kLightLine(70, 70, 70);
QColor kText(222, 222, 222);
QColor kTextOnLight(20, 20, 20);
QColor kDimText(160, 160, 160);
QColor kSnapLine(70, 170, 255);
QColor kDrop(255, 196, 61);
QColor kCursorLine(8, 8, 8);
QColor kRulerBg(55, 55, 55);
QColor kRulerText(214, 214, 214);
QColor kLoopOn(240, 228, 20);
QColor kLoopOff(214, 204, 64);
QColor kLoopBar(0, 0, 128);         // VEGAS 18's loop region (looping on): the bar and its lines through the tracks
// the time selection over the empty lanes (and, kEventSelectionWash, over the events): VEGAS 18's blue
QColor kSelectionWash(28, 140, 248, 115);
QColor kEventSelectionWash(34, 153, 255, 91);
QColor kMarkerColor(255, 152, 32);
QColor kRegionColor(76, 176, 76);
QColor kTimeText(222, 222, 222);
QColor kFxOn(110, 210, 120);
QColor kFxBad(230, 90, 76);
QColor kFadeHandle(0, 0, 255);  // VEGAS 18 (a screenshot of its timeline): pure blue

// Colors tracks get in turn when they have none of their own (in the spirit of VEGAS's defaults: muted blue, deep
// red, ...); sound events are filled with them.
QColor defaultTrackColor(int index)
{
    static const QColor colors[] = {QColor(89, 115, 168), QColor(124, 52, 65), QColor(150, 116, 52), QColor(70, 128, 74),
                                    QColor(52, 124, 132), QColor(118, 82, 158), QColor(160, 84, 116), QColor(108, 110, 64)};
    const QStringList own = vprefs::list("prefs/trackColors");
    if (own.size() == 8) {
        const QColor c(own[index % 8]);
        if (c.isValid()) return c;
    }
    return colors[(size_t)index % (sizeof(colors) / sizeof(colors[0]))];
}

Ranges merged(Ranges v)
{
    std::sort(v.begin(), v.end());
    Ranges out;
    for (const auto& r : v) {
        if (r.second <= r.first) continue;
        if (!out.empty() && r.first <= out.back().second + 1e-9) out.back().second = std::max(out.back().second, r.second);
        else out.push_back(r);
    }
    return out;
}

double removedBefore(const Ranges& gone, double t)
{
    double s = 0;
    for (const auto& g : gone)
        if (g.second <= t + 1e-9) s += g.second - g.first;
    return s;
}

// Auto ripple after events were removed: what came after closes up. 'removed' holds, per track, the time spans its
// events covered.
void rippleClose(Project& p, const std::map<uint64_t, Ranges>& removed, RippleMode mode)
{
    Ranges all;
    for (const auto& r : removed) all.insert(all.end(), r.second.begin(), r.second.end());
    all = merged(all);
    for (Track& t : p.tracks) {
        Ranges gone;
        if (mode == RippleMode::AllTracks) {
            gone = all;
        } else {
            auto it = removed.find(t.id);
            if (it == removed.end()) continue;
            gone = merged(it->second);
        }
        for (Event& e : t.events)
            if (!e.locked) e.start = std::max(0.0, e.start - removedBefore(gone, e.start));
        t.sortEvents();
    }
    if (mode != RippleMode::AffectedTracks)
        for (auto& m : p.markers) m.time = std::max(0.0, m.time - removedBefore(all, m.time));
}

Followers followersOf(const Project& p, const std::set<uint64_t>& tracks, double from, const std::set<uint64_t>& exclude, RippleMode mode)
{
    Followers f;
    for (const Track& t : p.tracks) {
        if (mode != RippleMode::AllTracks && !tracks.count(t.id)) continue;
        for (const Event& e : t.events)
            if (!exclude.count(e.id) && !e.locked && e.start >= from - 1e-9) f.events.emplace_back(e.id, e.start);
    }
    if (mode != RippleMode::AffectedTracks)
        for (const auto& m : p.markers)
            if (m.time >= from - 1e-9) f.markers.emplace_back(m.id, m.time);
    return f;
}

void drawMagnifier(QPainter& p, const QRectF& r)
{
    const QPointF c = r.center();
    p.setPen(QPen(QColor(224, 224, 224), 1.4));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(c.x() - 1.0, c.y() - 1.0), 3.5, 3.5);
    p.setPen(QPen(QColor(224, 224, 224), 2.0, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(c.x() + 1.8, c.y() + 1.8), QPointF(c.x() + 4.5, c.y() + 4.5));
}

QColor envelopeColor(const std::string& kind)
{
    for (const QString& c : vprefs::list("prefs/envelopeColors"))
        if (c.startsWith(QString::fromStdString(kind) + "=")) {
            const QColor col(c.mid((int)kind.size() + 1));
            if (col.isValid()) return col;
        }
    if (kind == "volume" || kind == "composite") return QColor(52, 104, 255);
    if (kind == "pan" || kind == "fadecolor") return QColor(236, 52, 52);
    if (kind == "mute") return QColor(240, 206, 40);
    if (kind == "send") return QColor(196, 104, 240);
    if (kind == "velocity") return QColor(52, 206, 76);
    if (kind == "duck") return QColor(240, 150, 40);
    if (kind == "motionblur") return QColor(230, 120, 220);
    if (kind == "supersample") return QColor(40, 196, 206);
    if (kind == "transition") return QColor(186, 120, 250);
    return QColor(150, 226, 90);
}

QString envelopeName(const std::string& kind)
{
    if (kind == "volume") return "Volume";
    if (kind == "pan") return "Pan";
    if (kind == "mute") return "Mute";
    if (kind == "send") return "Send";
    if (kind == "composite") return "Composite Level";
    if (kind == "fadecolor") return "Fade to Color";
    if (kind == "velocity") return "Velocity";
    if (kind == "duck") return "Auto Ducking";
    if (kind == "motionblur") return "Motion Blur Amount";
    if (kind == "supersample") return "Video Supersampling";
    if (kind == "transition") return "Transition Progress";
    return "FX";
}

double envToFrac(const std::string& kind, double v)
{
    if (kind == "volume" || kind == "send") return clampd(std::cbrt(std::max(0.0, v) / kEnvMaxGain), 0.0, 1.0);
    if (kind == "pan") return clampd((1.0 - v) * 0.5, 0.0, 1.0);
    if (kind == "mute") return v >= 0.5 ? 0.0 : 1.0;
    if (kind == "fadecolor") return clampd((v + 1.0) * 0.5, 0.0, 1.0);
    if (kind == "velocity") return v <= 1.0 ? clampd((v + 1.0) * 0.25, 0.0, 0.5) : clampd(0.5 + (v - 1.0) / 18.0, 0.5, 1.0);
    if (kind == "supersample") return clampd((v - 1.0) / 7.0, 0.0, 1.0);
    return clampd(v, 0.0, 1.0);
}

double fracToEnv(const std::string& kind, double f)
{
    f = clampd(f, 0.0, 1.0);
    if (kind == "volume" || kind == "send") {
        if (std::fabs(f - envToFrac(kind, 1.0)) < 0.006) return 1.0;
        return kEnvMaxGain * f * f * f;
    }
    if (kind == "pan") {
        const double v = 1.0 - 2.0 * f;
        return std::fabs(v) < 0.02 ? 0.0 : v;
    }
    if (kind == "mute") return f < 0.5 ? 1.0 : 0.0;
    if (kind == "fadecolor") return 2.0 * f - 1.0;
    if (kind == "velocity") return f <= 0.5 ? 4.0 * f - 1.0 : 1.0 + (f - 0.5) * 18.0;
    if (kind == "supersample") return std::round(1.0 + 7.0 * f);
    return f;
}

QString envPointLabel(const std::string& kind, double time, double value)
{
    return envelopeName(kind) + " at " + QString::fromStdString(rulerTime(AppState::get().project(), time)) + " is " + envValueText(kind, value);
}

QString envValueText(const std::string& kind, double v)
{
    if (kind == "volume" || kind == "send") {
        if (v <= 1e-5) return QString("-Inf dB");
        return QString::fromStdString(strfmt("%.1f dB", 20.0 * std::log10(v)));
    }
    if (kind == "pan") {
        if (std::fabs(v) < 0.005) return QString("Center");
        return QString::fromStdString(strfmt("%d%% %s", (int)std::lround(std::fabs(v) * 100.0), v < 0 ? "L" : "R"));
    }
    if (kind == "mute") return v >= 0.5 ? QString("Muted") : QString("Not Muted");
    if (kind == "fadecolor") {
        if (std::fabs(v) < 0.005) return QString("0 %");
        return QString::fromStdString(strfmt("%.0f %% %s", std::fabs(v) * 100.0, v > 0 ? "Top" : "Bottom"));
    }
    if (kind == "supersample") return QString::number((int)std::lround(v));
    return QString::fromStdString(strfmt("%.1f %%", v * 100.0));
}

std::vector<std::pair<QString, double>> envPresets(const std::string& kind)
{
    if (kind == "volume" || kind == "send")
        return {{"+6.0 dB", 2.0}, {"+3.0 dB", std::pow(10.0, 3.0 / 20.0)}, {"0.0 dB", 1.0}, {"-3.0 dB", std::pow(10.0, -3.0 / 20.0)},
                {"-6.0 dB", std::pow(10.0, -6.0 / 20.0)}, {"-12.0 dB", std::pow(10.0, -12.0 / 20.0)}, {"-20.0 dB", 0.1}, {"-Inf dB", 0.0}};
    if (kind == "pan") return {{"100% L", -1.0}, {"50% L", -0.5}, {"Center", 0.0}, {"50% R", 0.5}, {"100% R", 1.0}};
    if (kind == "mute") return {{"Muted", 1.0}, {"Not Muted", 0.0}};
    if (kind == "fadecolor") return {{"100 % Top", 1.0}, {"50 % Top", 0.5}, {"0 %", 0.0}, {"50 % Bottom", -0.5}, {"100 % Bottom", -1.0}};
    if (kind == "velocity")
        return {{"Set to Maximum Forward Velocity", 10.0}, {"Set to 300% Forward Velocity", 3.0}, {"Set to 200% Forward Velocity", 2.0},
                {"Set to Normal Velocity", 1.0}, {"Set to 50% Forward Velocity", 0.5}, {"Set to 40% Forward Velocity", 0.4},
                {"Set to 20% Forward Velocity", 0.2}, {"Set to 0% Velocity", 0.0}, {"Set to 100% Reverse Velocity", -1.0}};
    if (kind == "supersample") return {{"1", 1.0}, {"2", 2.0}, {"4", 4.0}, {"8", 8.0}};
    return {{"100 %", 1.0}, {"75 %", 0.75}, {"50 %", 0.5}, {"25 %", 0.25}, {"0 %", 0.0}};
}

QIcon curveIcon(int curve, bool falling, int crossCurve)
{
    QPixmap pm(28, 18);
    pm.fill(QColor(40, 40, 40));
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    auto line = [&](int c, bool down, const QColor& col) {
        QPolygonF poly;
        for (int k = 0; k <= 24; ++k) {
            const double x = k / 24.0;
            const double v = down ? 1.0 - curveShape(c, x) : curveShape(c, x);
            poly << QPointF(2.0 + x * 24.0, 16.0 - v * 14.0);
        }
        p.setPen(QPen(col, 1.6));
        p.drawPolyline(poly);
    };
    line(curve, falling, QColor(120, 160, 255));
    if (crossCurve >= 0) line(crossCurve, !falling, QColor(255, 255, 255));
    return QIcon(pm);
}

}

// ---- compositing parents and children (VEGAS's Make Compositing Parent / Child)

// A video track's level as a compositing child: its compositeLevel, but never deeper than one more than the video track
// above it (a child needs a parent). 'parents' gets the tracks it is composited into, the outermost first.
int TimelineWidget::Impl::compositeDepth(const Track& t, std::vector<const Track*>* parents) const
{
    std::vector<const Track*> chain;
    int prev = -1;
    for (const Track& o : app.project().tracks) {
        if (o.type != TrackType::Video) continue;
        const int d = std::min(std::max(0, o.compositeLevel), prev + 1);
        chain.resize((size_t)d);
        if (o.id == t.id) {
            if (parents) *parents = chain;
            return d;
        }
        chain.push_back(&o);
        prev = d;
    }
    return 0;
}

bool TimelineWidget::Impl::hasCompositeChildren(const Track& t) const
{
    const Project& p = app.project();
    bool after = false;
    for (const Track& o : p.tracks) {
        if (o.type != TrackType::Video) continue;
        if (after) return std::max(0, o.compositeLevel) > std::max(0, t.compositeLevel);
        if (o.id == t.id) after = true;
    }
    return false;
}

// Make Compositing Child (by 1) / Parent (by -1): the track and its children go a level deeper or up (as the track
// header menu's commands do).
void TimelineWidget::Impl::shiftCompositeLevel(uint64_t trackId, int by, const char* what)
{
    app.edit(what, [&](Project& p) {
        int base = -1;
        for (Track& o : p.tracks) {
            if (o.type != TrackType::Video) continue;
            if (o.id == trackId) {
                base = std::max(0, o.compositeLevel);
                o.compositeLevel = std::max(0, base + by);
                continue;
            }
            if (base < 0) continue;
            if (std::max(0, o.compositeLevel) <= base) {
                base = -1;
                continue;
            }
            o.compositeLevel = std::max(0, o.compositeLevel + by);
        }
    });
}

// The compositing modes, the track's ticked: its own (Compositing Mode) or, for a parent, its group's (Parent Composite
// Mode).
void TimelineWidget::Impl::compositeModeMenu(QMenu& m, uint64_t trackId, bool parent)
{
    const Track* t = findTrackConst(trackId);
    if (!t) return;
    const std::string now = parent ? t->parentComposite : t->composite;
    for (const CompositeMode& cm : compositeModes()) {
        const std::string id = cm.id;
        QAction* a = add(m, cm.label, [this, trackId, id, parent] {
            app.edit(parent ? "Parent Composite Mode" : "Compositing Mode", [&](Project& p) {
                if (Track* tr = p.findTrack(trackId)) (parent ? tr->parentComposite : tr->composite) = id;
            });
        });
        a->setCheckable(true);
        a->setChecked(now == cm.id);
    }
}

// ---- track groups

void TimelineWidget::Impl::selectGroupTracks(uint64_t group)
{
    std::set<uint64_t> ids;
    uint64_t first = 0;
    for (const Track& t : app.project().tracks)
        if (t.trackGroup == group) {
            ids.insert(t.id);
            if (!first) first = t.id;
        }
    if (!first) return;
    trackListFocus = true;
    selTracks = ids;
    setCurrentTrack(first);
    ++headerGeneration;
    q->update();
}

// A click on a track group's header row: its triangle collapses or expands it, its buttons mute or solo it; elsewhere
// it selects the group's tracks (VEGAS 18 manual p215).
void TimelineWidget::Impl::pressGroup(const Hit& h)
{
    const Track* t = findTrackConst(h.track);
    if (!t || !t->trackGroup) return;
    const uint64_t g = t->trackGroup;
    setCurrentTrack(t->id);
    switch (h.part) {
    case Part::GroupToggle: q->collapseTrackGroup(!collapsedGroups.count(g)); break;
    case Part::GroupMute: q->muteSoloTrackGroup(false, false); break;
    case Part::GroupSolo: q->muteSoloTrackGroup(true, false); break;
    default: selectGroupTracks(g); break;
    }
}

// ---- the multipurpose send row

// What a track can send to, in the order of VEGAS's menu: the assignable FX chains, then the busses.
std::vector<const Bus*> TimelineWidget::Impl::sendTargets() const
{
    std::vector<const Bus*> out;
    const Project& p = app.project();
    for (const Bus& b : p.buses)
        if (b.assignable) out.push_back(&b);
    for (const Bus& b : p.buses)
        if (!b.assignable) out.push_back(&b);
    return out;
}

const Bus* TimelineWidget::Impl::sendShown(const Track& t) const
{
    const std::vector<const Bus*> all = sendTargets();
    if (all.empty()) return nullptr;
    const auto it = shownSend.find(t.id);
    if (it != shownSend.end())
        for (const Bus* b : all)
            if (b->id == it->second) return b;
    return all.front();
}

QString TimelineWidget::Impl::sendLevelText(double level)
{
    if (level <= 1e-5) return QString("-Inf.");
    return QString::fromStdString(strfmt("%.1f dB", 20.0 * std::log10(level)));
}

static double sendLevelOf(const Track& t, uint64_t bus)
{
    for (const Send& s : t.sends)
        if (s.bus == bus) return s.level;
    return 0.0;
}

// The track's send to 'bus', made if it has none: as the Mixing Console makes one, before the track's volume for a
// bus and after it for an assignable FX chain.
static Send* sendOf(Project& p, uint64_t track, uint64_t bus)
{
    Track* t = p.findTrack(track);
    if (!t) return nullptr;
    for (Send& s : t->sends)
        if (s.bus == bus) return &s;
    Send s;
    s.bus = bus;
    const Bus* b = p.findBus(bus);
    s.preFader = !(b && b->assignable);
    t->sends.push_back(s);
    return &t->sends.back();
}

// The row's button: every bus and assignable FX chain with the track's send level to it ("FX 1: -Inf."), the one the
// row shows ticked; choosing another shows that one.
void TimelineWidget::Impl::sendMenu(QMenu& m, uint64_t trackId)
{
    const Track* t = findTrackConst(trackId);
    if (!t) return;
    const Bus* shown = sendShown(*t);
    QActionGroup* group = new QActionGroup(&m);
    for (const Bus* b : sendTargets()) {
        const uint64_t id = b->id;
        QAction* a = add(m, QString("%1: %2").arg(QString::fromStdString(b->name), sendLevelText(sendLevelOf(*t, id))), [this, trackId, id] {
            shownSend[trackId] = id;
            ++headerGeneration;
            q->update();
        });
        a->setCheckable(true);
        a->setChecked(b == shown);
        group->addAction(a);
    }
}

void TimelineWidget::Impl::toggleSendPrePost(uint64_t trackId)
{
    const Track* t = findTrackConst(trackId);
    const Bus* b = t ? sendShown(*t) : nullptr;
    if (!b) return;
    const uint64_t bus = b->id;
    app.edit("Send Pre/Post", [&](Project& p) {
        if (Send* s = sendOf(p, trackId, bus)) s->preFader = !s->preFader;
    });
}

// The shown send of a track to 'level': as part of the drag going on ('drag'), or as an undo step of its own.
void TimelineWidget::Impl::setSendLevel(uint64_t trackId, double level, bool inDrag)
{
    const Track* t = findTrackConst(trackId);
    const Bus* b = t ? sendShown(*t) : nullptr;
    if (!b) return;
    const uint64_t bus = b->id;
    auto fn = [&](Project& p) {
        if (Send* s = sendOf(p, trackId, bus)) s->level = level;
    };
    if (inDrag) app.dragUpdate(fn);
    else app.edit("Send Level", fn);
    app.status(QString("%1: %2").arg(QString::fromStdString(b->name), sendLevelText(level)).toStdString());
}

void TimelineWidget::Impl::beginSendSlider(const Hit& h, const QPointF& pos)
{
    const Row* r = rowOf(h.track);
    const Track* t = r ? trackOf(*r) : nullptr;
    const Bus* b = t ? sendShown(*t) : nullptr;
    if (!b) return;
    const QRect s = headerRects(*r).send;
    const double value = volumeToSlider(sendLevelOf(*t, b->id));
    drag.kind = Drag::Send;
    drag.startValue = value;
    drag.absolute = std::fabs(pos.x() - (s.left() + value * s.width())) > 6.0;
}

// The send slider dragged (VEGAS's: as the Vol slider, Ctrl for fine steps, 0 dB a notch): one undo step for the drag.
void TimelineWidget::Impl::updateSendSlider(const QPointF& pos, Qt::KeyboardModifiers mods)
{
    const Row* r = rowOf(drag.hit.track);
    if (!r) return;
    if (!drag.started) {
        if (std::fabs(pos.x() - drag.pressPos.x()) < 2.0 && !drag.absolute) return;
        app.beginDrag();
        drag.started = true;
    }
    const QRect s = headerRects(*r).send;
    if (s.width() <= 0) return;
    const double fine = (mods & Qt::ControlModifier) ? 0.125 : 1.0;
    const double v = clampd(drag.absolute ? (pos.x() - s.left()) / s.width() : drag.startValue + (pos.x() - drag.pressPos.x()) / s.width() * fine, 0.0, 1.0);
    double level = sliderToVolume(v);
    if (std::fabs(v - volumeToSlider(1.0)) < 0.006) level = 1.0;
    if (v <= 0.0) level = 0.0;
    setSendLevel(drag.hit.track, level, true);
}

// ---- the FX button's arrow (VEGAS 18 manual p242): FX Automation... (with no track effects, the Plug-In Chooser)

void TimelineWidget::Impl::fxArrowMenu(QMenu& m, uint64_t trackId)
{
    add(m, "FX Automation...", [this, trackId] {
        const Track* t = findTrackConst(trackId);
        if (!t) return;
        if (t->fx.empty()) {
            if (!q->onOpenFx) return;
            FxTarget ft;
            ft.kind = FxTarget::Track;
            ft.id = trackId;
            q->onOpenFx(ft, 0);
            return;
        }
        if (runFxAutomationChooser(q, trackId)) q->update();
    });
}

// ---- the track output meters

void TimelineWidget::Impl::saveMeterOptions()
{
    QSettings s;
    s.setValue("timeline/trackMeters", metersOn);
    s.setValue("timeline/trackMetersVertical", meterVertical);
    s.setValue("timeline/trackMetersHold", meterHold);
    s.setValue("timeline/trackMetersRange", meterLo);
}

void TimelineWidget::Impl::startMeters()
{
    if (metersOn && meterTimer && !meterTimer->isActive() && playback && playback->playing()) meterTimer->start();
}

// About 30 times a second while playing: each audio track's meter takes the peaks Playback has published for it (the
// same the Mixing Console's meters show); only meters that look different are repainted. Stopped, the bars go out (the
// held peaks stay) and the timer stops.
void TimelineWidget::Impl::meterTick()
{
    if (!meterClock.isValid()) meterClock.start();
    const double now = meterClock.elapsed() / 1000.0;
    const bool playing = playback && playback->playing();
    std::vector<AudioEngine::StripPeak> levels;
    if (playing) playback->stripLevels(levels);
    QRegion dirty;
    for (const Row& r : rows) {
        if (r.video) continue;
        TrackMeter& m = trackMeters[r.id];
        float in[2] = {0, 0};
        for (const auto& x : levels)
            if (x.id == r.id) {
                in[0] = x.left;
                in[1] = x.right;
                break;
            }
        for (int c = 0; c < 2; ++c) {
            m.level[c] = in[c];
            if (in[c] >= m.hold[c]) {
                m.hold[c] = in[c];
                m.holdAt[c] = now;
            } else if (!meterHold && now - m.holdAt[c] > 1.5) {
                m.hold[c] = std::max(in[c], m.hold[c] * 0.8f);
            }
            if (in[c] > 1.0f) m.clip = true;
        }
        if (r.h <= 0 || !metersOn) continue;
        const HeaderRects h = headerRects(r);
        if (h.meter.isEmpty()) continue;
        // what the meter shows, in pixels: the two bars, the held peak (its readout to a tenth of a dB) and clipping
        const int len = h.meterVertical ? h.meter.height() : h.meter.width();
        auto px = [&](float v) {
            const double db = v > 1e-6f ? 20.0 * std::log10(v) : -200.0;
            return (int)std::lround(clampd((db - meterLo) / -meterLo, 0.0, 1.0) * len);
        };
        const float hold = std::max(m.hold[0], m.hold[1]);
        const QRect look(px(m.level[0]), px(m.level[1]), hold > 1e-6f ? (int)std::lround(200.0 * std::log10(hold)) : -9999, m.clip ? 1 : 0);
        auto it = meterPainted.find(r.id);
        if (it == meterPainted.end() || it->second != look) {
            meterPainted[r.id] = look;
            dirty |= h.meter.adjusted(-1, -12, 1, 1);
        }
    }
    if (!dirty.isEmpty()) q->updateCursor(dirty.intersected(headerColumn()));
    if (!playing) meterTimer->stop();
}

void TimelineWidget::Impl::paintMeters(QPainter& p, const QRect& clip)
{
    if (!metersOn) return;
    const QRect c = headerColumn().intersected(clip);
    if (c.isEmpty()) return;
    p.save();
    p.setClipRect(c);
    for (const Row& r : rows) {
        if (r.video || r.h <= 0) continue;
        const int top = rowTop(r);
        if (top > c.bottom() || top + r.h < c.top()) continue;
        const HeaderRects h = headerRects(r);
        if (h.meter.isEmpty() || !h.meter.adjusted(-1, -12, 1, 1).intersects(c)) continue;
        const auto it = trackMeters.find(r.id);
        p.save();
        p.setClipRect(h.all.intersected(c));
        drawMeter(p, h.meter, h.meterVertical, it == trackMeters.end() ? nullptr : &it->second);
        p.restore();
    }
    p.restore();
}

// Right-click on a track's meter (VEGAS 18 manual p220: reset the clip indicators, choose a scale, vertical or not, or
// turn the output meters off).
void TimelineWidget::Impl::meterMenu(QMenu& m)
{
    add(m, "Reset Clip", [this] {
        for (auto& tm : trackMeters) tm.second = TrackMeter();
        meterPainted.clear();
        q->update(headerColumn());
    });
    toggleItem(m, "Hold Peaks", meterHold, [this] {
        meterHold = !meterHold;
        saveMeterOptions();
    });
    m.addSeparator();
    QActionGroup* ranges = new QActionGroup(&m);
    for (int lo : {-12, -24, -42, -60, -78, -96}) {
        QAction* a = add(m, QString("%1 dB").arg(lo), [this, lo] {
            meterLo = lo;
            meterPainted.clear();
            saveMeterOptions();
            q->update(headerColumn());
        });
        a->setCheckable(true);
        a->setChecked(std::fabs(meterLo - lo) < 0.5);
        ranges->addAction(a);
    }
    m.addSeparator();
    toggleItem(m, "Vertical", meterVertical, [this] {
        meterVertical = !meterVertical;
        meterPainted.clear();
        saveMeterOptions();
        layout();
        updateBars();
        q->update();
    });
    toggleItem(m, "Show Meters", metersOn, [this] {
        metersOn = !metersOn;
        meterPainted.clear();
        saveMeterOptions();
        layout();
        updateBars();
        q->update();
        startMeters();
    });
}

}  // namespace app
}  // namespace reno
