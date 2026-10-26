// Helpers shared by the timeline files: colors, ruler steps, ripple and the track level text.
#include "timeline_internal.h"

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
QColor kSelectionWash(255, 255, 255, 34);
QColor kMarkerColor(255, 152, 32);
QColor kRegionColor(76, 176, 76);
QColor kTimeText(222, 222, 222);
QColor kFxOn(110, 210, 120);
QColor kFxBad(230, 90, 76);
QColor kFadeHandle(28, 40, 255);
QColor kAccent(0, 122, 204);
QColor kSelect(255, 196, 61);
QColor kLaneAudio(33, 35, 38);

void useColors(UiTheme theme)
{
    static UiTheme inUse = UiTheme::Vegas;
    if (theme == inUse) return;
    inUse = theme;
    const bool reno = theme == UiTheme::Reno;
    auto set = [reno](QColor& c, QColor vegas, QColor own) { c = reno ? own : vegas; };
    set(kWindow, QColor(55, 55, 55), QColor(45, 45, 48));
    set(kPanel, QColor(70, 70, 70), QColor(30, 30, 33));
    set(kHeaderBg, QColor(45, 45, 45), QColor(50, 50, 54));
    set(kHeaderCurrent, QColor(130, 130, 130), QColor(66, 72, 84));
    set(kLane, QColor(45, 45, 45), QColor(37, 37, 41));
    set(kBusLane, QColor(58, 58, 62), QColor(40, 40, 46));
    set(kBelowBg, QColor(35, 35, 35), QColor(27, 27, 29));
    set(kLine, QColor(35, 35, 35), QColor(22, 22, 24));
    set(kLightLine, QColor(70, 70, 70), QColor(62, 62, 66));
    set(kText, QColor(222, 222, 222), QColor(222, 222, 222));
    set(kTextOnLight, QColor(20, 20, 20), QColor(240, 240, 244));
    set(kDimText, QColor(160, 160, 160), QColor(150, 150, 154));
    set(kSnapLine, QColor(70, 170, 255), QColor(255, 196, 61));
    set(kCursorLine, QColor(8, 8, 8), QColor(238, 238, 238));
    set(kRulerBg, QColor(55, 55, 55), QColor(54, 54, 58));
    set(kRulerText, QColor(214, 214, 214), QColor(204, 204, 208));
    set(kSelectionWash, QColor(255, 255, 255, 34), QColor(120, 170, 235, 30));
    set(kTimeText, QColor(222, 222, 222), QColor(232, 232, 236));
    set(kFxOn, QColor(110, 210, 120), QColor(72, 160, 92));
    set(kFxBad, QColor(230, 90, 76), QColor(200, 72, 60));
    set(kFadeHandle, QColor(28, 40, 255), QColor(236, 236, 236, 205));
}

// Colors tracks get in turn when they have none of their own (in the spirit of VEGAS's defaults: muted blue, deep
// red, ...); sound events are filled with them.
QColor defaultTrackColor(int index)
{
    if (renoTheme()) {
        static const QColor own[] = {QColor(120, 98, 200), QColor(64, 132, 214), QColor(38, 160, 150), QColor(112, 168, 64),
                                     QColor(226, 150, 48), QColor(214, 90, 90), QColor(170, 86, 180), QColor(40, 160, 190)};
        return own[(size_t)index % (sizeof(own) / sizeof(own[0]))];
    }
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

}  // namespace app
}  // namespace reno
