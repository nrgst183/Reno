// Track headers, button icons and Reno's own look of the timeline.
#include "timeline_internal.h"

namespace reno {
namespace app {

void TimelineWidget::Impl::drawFxIcon(QPainter& p, const QRectF& b, const QColor& color, bool bold) const
{
    QFont f = p.font();
    f.setFamily("serif");
    f.setPixelSize(std::max(8, (int)std::lround(b.height() * 0.86)));
    f.setItalic(true);
    f.setBold(bold);
    p.setFont(f);
    p.setPen(color);
    drawKeptText(p, b.adjusted(0, -1, 0, -1), QStringLiteral("fx"), fxSigns[bold ? 1 : 0], true);
}

void TimelineWidget::Impl::drawCropIcon(QPainter& p, const QRectF& b, const QColor& color)
{
    const QPointF c = b.center();
    const double s = std::min(b.width(), b.height()) * 0.3;
    p.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::SquareCap));
    p.setBrush(Qt::NoBrush);
    p.drawPolyline(QPolygonF() << QPointF(c.x() - s - 2.0, c.y() - s) << QPointF(c.x() + s, c.y() - s) << QPointF(c.x() + s, c.y() + s + 2.0));
    p.drawPolyline(QPolygonF() << QPointF(c.x() - s, c.y() - s - 2.0) << QPointF(c.x() - s, c.y() + s) << QPointF(c.x() + s + 2.0, c.y() + s));
}

// A round track header button, colored as VEGAS colors them (mute pink, solo gold, the others grey), brighter and ringed
// while on.
void TimelineWidget::Impl::drawHeaderButton(QPainter& p, const QRect& r, Icon icon, bool on, bool hot, const QColor& mark) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF b = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor fill;
    switch (icon) {
    case Icon::Mute: fill = on ? QColor(240, 70, 84) : QColor(224, 138, 150); break;
    case Icon::Solo: fill = on ? QColor(255, 206, 40) : QColor(186, 160, 86); break;
    case Icon::Arm: fill = on ? QColor(236, 60, 60) : QColor(150, 150, 150); break;
    default: fill = on ? QColor(150, 196, 150) : QColor(150, 150, 150); break;
    }
    if (hot) fill = fill.lighter(118);
    p.setPen(QPen(on ? QColor(255, 255, 255) : QColor(30, 30, 30), on ? 1.4 : 1.0));
    p.setBrush(fill);
    p.drawEllipse(b);
    const QColor ink(34, 30, 30);
    const QPointF c = b.center();
    QFont f = p.font();
    f.setBold(true);
    f.setPixelSize((int)std::lround(b.height() * 0.62));
    p.setFont(f);
    switch (icon) {
    case Icon::Mute:
        p.setPen(ink);
        p.drawText(b.adjusted(0, 0, -1, 0), Qt::AlignCenter, "M");
        {
            const QPointF n(b.right() - 2.8, b.top() + 3.2);
            p.setPen(QPen(QColor(255, 255, 255), 1.1));
            p.setBrush(QColor(28, 28, 28));
            p.drawEllipse(n, 3.4, 3.4);
            p.drawLine(QPointF(n.x() - 1.9, n.y() + 1.9), QPointF(n.x() + 1.9, n.y() - 1.9));
        }
        break;
    case Icon::Solo:
        p.setPen(ink);
        p.drawText(b.adjusted(-1, 0, -2, 0), Qt::AlignCenter, "S");
        p.setPen(QPen(QColor(28, 28, 28), 1.0));
        p.setBrush(QColor(255, 255, 255));
        p.setRenderHint(QPainter::Antialiasing, false);
        p.drawRect(QRectF(b.right() - 2.5, b.top() - 1.0, 2.5, 7.0));
        p.drawRect(QRectF(b.right() - 2.5, b.top() + 7.5, 2.5, 2.5));
        p.setRenderHint(QPainter::Antialiasing, true);
        break;
    case Icon::Fx:
        drawFxIcon(p, b, mark, on);
        break;
    case Icon::Arm:
        p.setPen(Qt::NoPen);
        p.setBrush(on ? QColor(255, 255, 255) : QColor(200, 40, 40));
        p.drawEllipse(c, 3.6, 3.6);
        break;
    case Icon::Motion:
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(mark, 1.1));
        p.drawRect(QRectF(c.x() - 4.5, c.y() - 3.5, 9, 7));
        p.setBrush(mark);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(c.x() - 0.5, c.y() - 2.5, 4, 3.5));
        break;
    }
    p.restore();
}

// A slider in a track header: a thin groove with VEGAS's square knob (for pan, a pointer above the groove). 'travel' is
// where the knob's middle goes (the groove reaches half a knob further each way); 's' is its place (0..1).
void TimelineWidget::Impl::drawSlider(QPainter& p, const QRect& travel, double s, bool pointer, bool hot, bool onLight)
{
    const QRect r = travel.adjusted(-6, 0, 6, 0);
    const int cy = r.center().y();
    const double x = travel.left() + clampd(s, 0.0, 1.0) * travel.width();
    p.save();
    if (pointer) {
        p.fillRect(QRect(r.left(), cy + 3, r.width(), 3), QColor(38, 38, 38));
        p.setRenderHint(QPainter::Antialiasing, true);
        QPolygonF tri;
        tri << QPointF(x - 6.5, cy - 4) << QPointF(x + 6.5, cy - 4) << QPointF(x, cy + 3);
        p.setPen(QPen(QColor(24, 24, 24), 1.0));
        p.setBrush(hot ? QColor(255, 255, 255) : QColor(236, 236, 236));
        p.drawPolygon(tri);
    } else {
        p.fillRect(QRectF(r.left(), cy - 1, x - r.left(), 2), onLight ? QColor(212, 212, 212) : QColor(132, 132, 132));
        p.fillRect(QRectF(r.left(), cy + 1, x - r.left(), 1), QColor(60, 60, 60));
        p.fillRect(QRectF(x, cy - 1, r.right() + 1 - x, 3), QColor(38, 38, 38));
        const QRectF k(std::floor(x) - 6.0, cy - 5.0, 12.0, 11.0);
        p.fillRect(k, QColor(24, 24, 24));
        p.fillRect(k.adjusted(1, 1, -1, -1), hot ? QColor(255, 255, 255) : QColor(240, 240, 240));
        p.fillRect(QRectF(k.left() + 3.0, k.top() + 3.0, 2.0, 5.0), QColor(24, 24, 24));
        p.fillRect(QRectF(k.right() - 5.0, k.top() + 3.0, 2.0, 5.0), QColor(24, 24, 24));
    }
    p.restore();
}

void TimelineWidget::Impl::drawKindIcon(QPainter& p, const QPointF& at, bool video)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    if (video) {
        const QRectF f(at.x(), at.y(), 9.0, 10.0);
        p.setPen(QPen(QColor(20, 20, 20), 1.0));
        p.setBrush(QColor(240, 240, 240));
        p.drawRect(f);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(20, 20, 20));
        for (int k = 0; k < 3; ++k) {
            p.drawRect(QRectF(f.left() + 1.0, f.top() + 1.5 + k * 3.0, 1.2, 1.5));
            p.drawRect(QRectF(f.right() - 2.2, f.top() + 1.5 + k * 3.0, 1.2, 1.5));
        }
    } else {
        QPolygonF s;
        s << QPointF(at.x() + 7.5, at.y()) << QPointF(at.x() + 7.5, at.y() + 11.0) << QPointF(at.x() + 2.5, at.y() + 7.0)
          << QPointF(at.x(), at.y() + 7.0) << QPointF(at.x(), at.y() + 4.0) << QPointF(at.x() + 2.5, at.y() + 4.0);
        p.setPen(QPen(QColor(20, 20, 20), 1.0));
        p.setBrush(QColor(240, 240, 240));
        p.drawPolygon(s);
    }
    p.restore();
}

void TimelineWidget::Impl::drawRenoButton(QPainter& p, const QRect& r, const QString& text, bool on, const QColor& onColor, bool hot, bool darkText)
{
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(hot ? QColor(200, 200, 204) : QColor(22, 22, 24), 1.0));
    p.setBrush(on ? onColor : QColor(70, 70, 75));
    p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 3.0, 3.0);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(on ? (darkText ? QColor(30, 26, 10) : QColor(255, 255, 255)) : QColor(206, 206, 210));
    p.drawText(r, Qt::AlignCenter, text);
}

void TimelineWidget::Impl::drawAutoKnob(QPainter& p, const QRect& travel, double s)
{
    const double x = travel.left() + clampd(s, 0.0, 1.0) * travel.width();
    const QPointF c(x, travel.center().y() + 0.5);
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(18, 18, 20), 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 5.5, 5.5);
    p.setPen(QPen(QColor(255, 176, 60), 1.6));
    p.drawEllipse(c, 5.5, 5.5);
    p.restore();
}

void TimelineWidget::Impl::drawAutoIcon(QPainter& p, const QRect& r, bool on, const std::string& mode, bool reno)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = on ? QColor(255, 255, 255) : (reno ? QColor(206, 206, 210) : QColor(40, 40, 40));
    const QRectF b = QRectF(r).adjusted(4.0, 3.0, -4.0, -3.0);
    for (int k = 0; k < 3; ++k) {
        const double x = b.left() + b.width() * (0.18 + 0.32 * k);
        p.setPen(QPen(ink, 1.0));
        p.drawLine(QPointF(x, b.top()), QPointF(x, b.bottom()));
        const double y = b.top() + b.height() * (k == 0 ? 0.65 : k == 1 ? 0.3 : 0.5);
        p.setPen(Qt::NoPen);
        p.setBrush(ink);
        p.drawRect(QRectF(x - 1.6, y - 1.2, 3.2, 2.4));
    }
    if (on) {
        const char* letter = mode == "off" ? "O" : mode == "touch" ? "T" : mode == "latch" ? "L" : "R";
        QFont f = p.font();
        f.setPixelSize(7);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(255, 176, 60));
        p.drawText(QRectF(r).adjusted(0, 0, -1, 1), Qt::AlignRight | Qt::AlignBottom, letter);
    }
    p.restore();
}

void TimelineWidget::Impl::drawAutoArrow(QPainter& p, const QRect& r, bool reno)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPolygonF tri;
    const double x = r.right() - 3.0, y = r.top() + 3.0;
    tri << QPointF(x - 2.5, y) << QPointF(x + 2.5, y) << QPointF(x, y + 3.0);
    p.setPen(Qt::NoPen);
    p.setBrush(reno ? QColor(230, 230, 234) : QColor(30, 30, 30));
    p.drawPolygon(tri);
    p.restore();
}

QString TimelineWidget::Impl::busLabel(const Track& t) const
{
    if (!t.output) return "Master";
    for (const reno::Bus& b : app.project().buses)
        if (b.id == t.output) return QString::fromStdString(b.name.empty() ? std::string("Bus") : b.name);
    return "Master";
}

// A header slider: 's' is the knob's place (0..1), 'from' where the blue part starts, 'mark' a tick at the normal
// setting (-1: none).
void TimelineWidget::Impl::drawSmallSurround(QPainter& p, const QRect& r, const SurroundPan& sp, bool hot, bool reno) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(QPen(hot ? QColor(220, 220, 224) : (reno ? QColor(86, 86, 92) : QColor(20, 20, 20)), 1.0));
    p.setBrush(reno ? QColor(28, 28, 31) : QColor(46, 46, 46));
    if (reno) p.drawRoundedRect(box, 3.0, 3.0);
    else p.drawRect(box);
    const QRectF room = box.adjusted(3.0, 3.0, -3.0, -3.0);
    const QPointF mid = room.center();
    const QColor line = reno ? QColor(64, 64, 70) : QColor(84, 84, 84);
    p.setPen(QPen(line, 1.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(room);
    p.drawLine(QPointF(room.left(), mid.y()), QPointF(room.right(), mid.y()));
    p.drawLine(QPointF(mid.x(), room.top()), QPointF(mid.x(), room.bottom()));
    auto at = [&](double x, double y) { return QPointF(mid.x() + x * room.width() * 0.5, mid.y() - y * room.height() * 0.5); };
    const struct { int ch; double x, y; } spk[5] = {{ChL, -1, 1}, {ChR, 1, 1}, {ChC, 0, 1}, {ChLs, -1, -1}, {ChRs, 1, -1}};
    const bool fixed = sp.route != "surround";
    for (const auto& k : spk) {
        bool lit = false;
        if (fixed)
            lit = (sp.route == "front" && (k.ch == ChL || k.ch == ChR)) || (sp.route == "center" && k.ch == ChC) ||
                  (sp.route == "rear" && (k.ch == ChLs || k.ch == ChRs));
        const bool muted = (sp.speakerMutes & (1u << k.ch)) != 0;
        p.setPen(Qt::NoPen);
        p.setBrush(muted ? QColor(200, 60, 50) : lit ? (reno ? kAccent : QColor(240, 240, 240)) : (reno ? QColor(120, 120, 128) : QColor(150, 150, 150)));
        const double d = r.width() >= 30 ? 2.5 : 2.0;
        p.drawRect(QRectF(at(k.x, k.y) - QPointF(d, d), QSizeF(2 * d, 2 * d)));
    }
    if (fixed && sp.route == "lfe") {
        p.setPen(reno ? kAccent : QColor(240, 240, 240));
        QFont f = smallFont;
        f.setPixelSize(std::max(7, std::min(10, r.width() / 4)));
        p.setFont(f);
        p.drawText(room, Qt::AlignCenter, "LFE");
    }
    if (!fixed) {
        double x = 0, y = 0, c = 1;
        surroundPointAt(sp, app.cursor(), x, y, c);
        const QPointF pt = at(clampd(x, -1, 1), clampd(y, -1, 1));
        p.setPen(QPen(reno ? QColor(20, 20, 22) : QColor(0, 0, 0), 1.0));
        p.setBrush(reno ? kAccent : QColor(255, 255, 255));
        const double rad = r.width() >= 30 ? 3.5 : 2.5;
        p.drawEllipse(pt, rad, rad);
    }
    p.restore();
}

void TimelineWidget::Impl::drawRenoSlider(QPainter& p, const QRect& r, double s, double from, double mark, bool hot)
{
    const int cy = r.center().y();
    s = clampd(s, 0.0, 1.0);
    const double x = r.left() + s * r.width(), x0 = r.left() + from * r.width();
    p.fillRect(QRect(r.left(), cy - 1, r.width() + 1, 3), QColor(24, 24, 26));
    p.fillRect(QRectF(std::min(x, x0), cy - 1, std::fabs(x - x0), 3), hot ? kAccent.lighter(130) : kAccent);
    if (mark >= 0) p.fillRect(QRectF(std::floor(r.left() + mark * r.width()), cy - 6, 1, 3), QColor(150, 150, 154));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(18, 18, 20), 1.0));
    p.setBrush(hot ? QColor(255, 255, 255) : QColor(214, 214, 218));
    p.drawRoundedRect(QRectF(x - 3.5, cy - 6.5, 7.0, 13.0), 2.0, 2.0);
    p.setRenderHint(QPainter::Antialiasing, false);
}

QString TimelineWidget::Impl::compositeNote(const Track& t)
{
    static const std::pair<const char*, const char*> names[] = {
        {"add", "Add"}, {"subtract", "Subtract"}, {"cut", "Cut"}, {"multiply", "Multiply"}, {"overlay", "Overlay"},
        {"screen", "Screen"}, {"hardlight", "Hard Light"}, {"dodge", "Dodge"}, {"burn", "Burn"}, {"darken", "Darken"},
        {"lighten", "Lighten"}, {"difference", "Difference"}, {"diffsquared", "Difference Squared"}, {"3dsrcalpha", "3D Source Alpha"}};
    QStringList parts;
    if (t.adjustment) parts << "Adjustment";
    if (t.compositeLevel > 0) parts << "Child";
    if (!t.composite.empty() && t.composite != "srcalpha") {
        QString n = QString::fromStdString(t.composite);
        for (const auto& m : names)
            if (t.composite == m.first) n = m.second;
        parts << n;
    }
    return parts.join(QString::fromUtf8(" · "));
}

void TimelineWidget::Impl::paintRenoHeaders(QPainter& p, const QRect& clip)
{
    const QRect c = QRect(0, kTopH, headerW, std::max(0, lanesBottom() - kTopH)).intersected(clip);
    if (c.isEmpty()) return;
    p.save();
    p.setClipRect(c);
    p.fillRect(c, QColor(38, 38, 41));
    const Project& pr = app.project();
    QFont bold = smallFont;
    bold.setBold(true);
    const QFontMetrics nameMetrics(q->font());
    const QFontMetrics smallMetrics(smallFont);
    for (const Row& r : rows) {
        const HeaderRects h = headerRects(r);
        if (!h.all.intersects(c)) continue;
        drawHeaderCached(p, r, h.all, QColor(38, 38, 41), [&](QPainter& p) {
        const Track& tr = pr.tracks[(size_t)r.index];
        const bool current = r.id == app.currentTrack || selTracks.count(r.id) != 0;
        const bool hot = hover.onHeader() && hover.track == r.id;
        // as in VEGAS: the selected track's header is light (blue-grey for video, grey-blue for sound) with dark
        // writing; the others are dark with light writing
        p.fillRect(h.all.adjusted(0, 0, 0, -1), kHeaderBg);
        const QColor ink = current ? QColor(18, 24, 32) : kText, dimInk = current ? QColor(40, 52, 66) : kDimText;
        if (current) p.fillRect(h.body, r.video ? QColor(120, 146, 162) : QColor(134, 146, 164));
        p.fillRect(QRect(0, h.all.bottom(), headerW, 1), kLine);
        p.fillRect(h.strip, trackColor(tr, r.index));

        {
            const bool menuHot = hot && hover.part == Part::HeaderMenu;
            if (menuHot) p.fillRect(h.menu, QColor(255, 255, 255, 60));
            p.save();
            outlined(p, [&](const QColor& col) { drawMenuIcon(p, QRectF(h.menu), col); });
            p.restore();
            if (!h.number.isEmpty()) {
                p.setFont(bold);
                const QString n = QString::number(r.index + 1);
                const QRect nr(h.number.left(), h.number.top(), 12, 16);
                p.setPen(QColor(0, 0, 0, 170));
                p.drawText(nr.translated(1, 1), Qt::AlignLeft | Qt::AlignTop, n);
                p.setPen(QColor(255, 255, 255));
                p.drawText(nr, Qt::AlignLeft | Qt::AlignTop, n);
                drawKindIcon(p, QPointF(nr.left() + QFontMetrics(bold).horizontalAdvance(n) + 1.5, nr.top() + 7.5), r.video);
            }
        }
        if (!(naming == Naming::Track && renaming == r.id) && h.name.width() > 8) {
            if (hot && hover.part == Part::HeaderName) {
                p.setPen(QColor(110, 110, 116));
                p.setBrush(QColor(0, 0, 0, 40));
                p.drawRect(h.name.adjusted(-2, 0, 0, -1));
            }
            p.setFont(q->font());
            p.setPen(tr.name.empty() ? dimInk : ink);
            const QString name = tr.name.empty() ? QString(r.video ? (tr.adjustment ? "Adjustment" : "Video") : "Audio") : QString::fromStdString(tr.name);
            p.drawText(h.name, Qt::AlignVCenter | Qt::AlignLeft, nameMetrics.elidedText(name, Qt::ElideRight, h.name.width()));
        }

        bool bad = false;
        for (const FxSlot& sl : tr.fx)
            if (!app.fx.error(sl.id).empty()) bad = true;
        p.setFont(bold);
        auto round = [](const QRect& r) { return QRect(r.center().x() - 7, r.center().y() - 7, 16, 16); };
        if (!h.fx.isEmpty()) drawHeaderButton(p, round(h.fx), Icon::Fx, !tr.fx.empty(), hot && hover.part == Part::HeaderFx, bad ? QColor(150, 20, 10) : QColor(24, 24, 24));
        if (!h.autoBtn.isEmpty()) {
            drawRenoButton(p, h.autoBtn, QString(), tr.automation, kAccent, hot && hover.part == Part::HeaderAuto);
            drawAutoIcon(p, h.autoBtn, tr.automation, tr.automationMode, true);
            drawAutoArrow(p, h.autoBtn, true);
        }
        if (!h.arm.isEmpty()) drawHeaderButton(p, round(h.arm), Icon::Arm, app.armedTracks.count(tr.id) != 0, hot && hover.part == Part::HeaderArm, QColor());
        const bool muteShown = tr.automation ? shownAutomation(tr, "mute") >= 0.5 : tr.mute;
        drawHeaderButton(p, round(h.mute), Icon::Mute, muteShown, hot && hover.part == Part::HeaderMute, QColor());
        if (tr.automation) p.fillRect(QRectF(h.mute.left() + 5, h.mute.bottom() - 1, h.mute.width() - 10, 1.5), QColor(255, 176, 60));
        drawHeaderButton(p, round(h.solo), Icon::Solo, tr.solo, hot && hover.part == Part::HeaderSolo, QColor());

        if (!h.level.isEmpty()) {
            p.setFont(smallFont);
            p.setPen(dimInk);
            p.drawText(h.levelLabel, Qt::AlignVCenter | Qt::AlignLeft, r.video ? "Level" : "Vol");
            const bool levelHot = (hot && hover.part == Part::HeaderLevel) || (drag.kind == Drag::Level && drag.hit.track == r.id);
            const double lv = tr.automation ? shownAutomation(tr, "volume") : tr.level;
            const double ls = r.video ? clampd(lv, 0.0, 1.0) : volumeToSlider(lv);
            if (r.video) drawRenoSlider(p, h.level, lv, 0.0, -1.0, levelHot);
            else drawRenoSlider(p, h.level, ls, 0.0, volumeToSlider(1.0), levelHot);
            if (tr.automation) drawAutoKnob(p, h.level, ls);
            p.setFont(smallFont);
            p.setPen(ink);
            p.drawText(h.levelValue, Qt::AlignVCenter | Qt::AlignRight, levelText(r.video, lv));
        }
        if (!h.surround.isEmpty()) drawSmallSurround(p, h.surround, tr.surround,
                                                     (hot && hover.part == Part::HeaderSurround) || (drag.kind == Drag::SurroundPoint && drag.hit.track == r.id), true);
        if (!h.pan.isEmpty()) {
            p.setPen(dimInk);
            p.drawText(h.panLabel, Qt::AlignVCenter | Qt::AlignLeft, "Pan");
            const bool panHot = (hot && hover.part == Part::HeaderPan) || (drag.kind == Drag::Pan && drag.hit.track == r.id);
            const double pv = tr.automation ? shownAutomation(tr, "pan") : tr.pan;
            drawRenoSlider(p, h.pan, (pv + 1.0) * 0.5, 0.5, 0.5, panHot);
            if (tr.automation) drawAutoKnob(p, h.pan, (pv + 1.0) * 0.5);
            p.setFont(smallFont);
            p.setPen(ink);
            p.drawText(h.panValue, Qt::AlignVCenter | Qt::AlignRight, panText(pv));
        }
        if (!h.fade.isEmpty()) {
            p.setFont(smallFont);
            p.setPen(dimInk);
            p.drawText(h.fadeLabel, Qt::AlignVCenter | Qt::AlignLeft, "Fade");
            const bool fadeHot = (hot && hover.part == Part::HeaderFade) || (drag.kind == Drag::Fade && drag.hit.track == r.id);
            const double fv = shownAutomation(tr, "fadecolor");
            drawRenoSlider(p, h.fade, (fv + 1.0) * 0.5, 0.5, 0.5, fadeHot);
            drawAutoKnob(p, h.fade, (fv + 1.0) * 0.5);
            p.setPen(ink);
            p.drawText(h.fadeValue, Qt::AlignVCenter | Qt::AlignRight, QString("%1%").arg(fv * 100.0, 0, 'f', 0));
        }
        if (!h.phase.isEmpty()) {
            p.setFont(bold);
            drawRenoButton(p, h.phase, QString::fromUtf8("\u00d8"), tr.invertPhase, kAccent, hot && hover.part == Part::HeaderPhase);
            p.setFont(smallFont);
            drawRenoButton(p, h.bus, smallMetrics.elidedText(busLabel(tr), Qt::ElideRight, h.bus.width() - 6), tr.output != 0, kAccent,
                           hot && hover.part == Part::HeaderBus);
        }
        if (!h.motion.isEmpty()) {
            const bool on = !tr.motion.empty();
            const bool mHot = hot && hover.part == Part::HeaderMotion;
            drawHeaderButton(p, QRect(h.motion.center().x() - 8, h.motion.center().y() - 8, 16, 16), Icon::Motion, on, mHot, QColor(24, 24, 24));
            const QString note = compositeNote(tr);
            const QRect nr(h.motion.right() + 6, h.motion.top(), headerW - kHeaderGap - 4 - (h.motion.right() + 6), h.motion.height());
            p.setFont(smallFont);
            p.setPen(dimInk);
            p.drawText(nr, Qt::AlignVCenter | Qt::AlignLeft, smallMetrics.elidedText(note.isEmpty() ? QString("Track Motion") : note, Qt::ElideRight, nr.width()));
        }
        if (drop.ok && !drop.event && drop.track == r.id && (drop.kind == DropKind::AudioFx || drop.kind == DropKind::VideoFx)) {
            p.setPen(QPen(kSelect, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(h.all.adjusted(1, 1, -2, -2));
        }
        });
    }
    for (const BusRow& b : busRows) {
        const QRect all(0, busTop(b), headerW, b.h);
        if (!all.intersects(c)) continue;
        const bool hot = hover.part == Part::BusHeader && hover.bus == b.owner;
        p.fillRect(all.adjusted(0, 0, 0, -1), hot ? QColor(58, 58, 64) : QColor(46, 46, 52));
        p.fillRect(QRect(0, all.top(), 6, b.h - 1), b.video ? QColor(110, 110, 150) : QColor(100, 130, 104));
        p.setFont(q->font());
        p.setPen(kText);
        const int bw = headerW - kHeaderGap - 18;
        p.drawText(QRect(12, all.top() + 6, bw, 17), Qt::AlignVCenter | Qt::AlignLeft, nameMetrics.elidedText(b.name, Qt::ElideRight, bw));
        QString sub;
        if (b.owner.kind == EnvelopeOwner::Master) sub = QString("Volume: %1").arg(levelText(false, pr.masterVolume));
        else if (const Bus* bus = b.owner.kind == EnvelopeOwner::Bus ? pr.findBus(b.owner.id) : nullptr) sub = QString("Volume: %1").arg(levelText(false, bus->level));
        else sub = QString("Video output");
        p.setFont(smallFont);
        p.setPen(kDimText);
        p.drawText(QRect(12, all.top() + 25, bw, 14), Qt::AlignVCenter | Qt::AlignLeft, sub);
        p.fillRect(QRect(0, all.bottom(), headerW, 1), kLine);
    }
    const bool gapHot = hover.part == Part::HeaderGap || drag.kind == Drag::HeaderWidth;
    p.fillRect(QRect(headerW - kHeaderGap, c.top(), kHeaderGap, c.height()), gapHot ? QColor(0, 122, 204, 160) : kBelowBg);
    p.fillRect(QRect(headerW - kHeaderGap, c.top(), 1, c.height()), kLine);
    p.restore();
}

// The time selection's length as the box on the time selection bar writes it: seconds and frames ("30.04"), with minutes
// (and hours) before them when long.
QString TimelineWidget::Impl::selectionLengthText(double length) const
{
    const int rate = std::max(1, (int)std::lround(fps()));
    const int64_t frames = (int64_t)std::llround(std::max(0.0, length) * rate);
    const int64_t secs = frames / rate, ff = frames % rate;
    const int digits = frameDigits(rate);
    const QString fr = QString("%1").arg((qlonglong)ff, digits, 10, QChar('0'));
    if (secs >= 3600) return QString("%1:%2:%3.%4").arg((qlonglong)(secs / 3600)).arg((qlonglong)(secs / 60 % 60), 2, 10, QChar('0')).arg((qlonglong)(secs % 60), 2, 10, QChar('0')).arg(fr);
    if (secs >= 60) return QString("%1:%2.%3").arg((qlonglong)(secs / 60)).arg((qlonglong)(secs % 60), 2, 10, QChar('0')).arg(fr);
    return QString("%1.%2").arg((qlonglong)secs).arg(fr);
}

// The marker bar, time selection bar and ruler in Reno's colors: the time selection a blue bar while looping is on (grey
// while off), the ruler's times at its numbered marks.
void TimelineWidget::Impl::paintRenoRuler(QPainter& p, const QRect& clip)
{
    const QRect c = QRect(headerW, 0, viewW(), kTopH).intersected(clip);
    if (c.isEmpty()) return;
    const Project& pr = app.project();
    p.save();
    p.setClipRect(c);
    p.fillRect(QRect(c.left(), 0, c.width(), kRulerTop), QColor(32, 32, 35));
    p.fillRect(QRect(c.left(), kLoopTop - 1, c.width(), 1), QColor(40, 40, 44));
    p.fillRect(QRect(c.left(), kRulerTop, c.width(), kRulerH), kRulerBg);
    p.fillRect(QRect(c.left(), kRulerTop - 1, c.width(), 1), kLine);
    p.fillRect(QRect(c.left(), kTopH - 1, c.width(), 1), kLine);

    // the time selection, as VEGAS 18 draws it: a thin line (blue while looping is on, grey while off) between two end
    // marks with arrow heads, a white box with the selection's length at its left.
    {
        const bool region = app.loopEnd > app.loopStart;
        // the ends are not clamped to the part being painted: what is drawn at them (arrow heads, box) must be the same
        // whether the whole bar or a thin strip of it is painted
        const double x0 = clampd(xAtF(app.loopStart), -1e6, 1e6), x1 = clampd(xAtF(app.loopEnd), -1e6, 1e6);
        if (region) {
            const QColor col = app.loopOn ? QColor(25, 140, 254) : QColor(130, 130, 136);
            const double fx0 = std::floor(x0), fx1 = std::floor(x1);
            const double ly = kLoopTop + 3.0;
            p.fillRect(QRectF(fx0, ly, std::max(1.0, fx1 - fx0 + 1.0), 2.0), col);
            p.fillRect(QRectF(fx0, kLoopTop + 1.0, 1.0, 7.0), col);
            p.fillRect(QRectF(fx1, kLoopTop + 1.0, 1.0, 7.0), col);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(col);
            if (fx1 - fx0 > 14.0) {
                p.drawPolygon(QPolygonF() << QPointF(fx0 + 0.5, ly + 1.0) << QPointF(fx0 + 5.0, ly - 2.5) << QPointF(fx0 + 5.0, ly + 4.5));
                p.drawPolygon(QPolygonF() << QPointF(fx1 + 0.5, ly + 1.0) << QPointF(fx1 - 4.0, ly - 2.5) << QPointF(fx1 - 4.0, ly + 4.5));
            }
            p.setRenderHint(QPainter::Antialiasing, false);
            QFont lf = smallFont;
            lf.setPixelSize(9);
            const QFontMetrics lm(lf);
            const QString text = selectionLengthText(app.loopEnd - app.loopStart);
            const double w = lm.horizontalAdvance(text) + 6.0;
            const double lx = std::max(fx0 + 18.0, (double)headerW + 4.0);
            if (lx + w < fx1 - 8.0) {
                const QRectF box(std::floor(lx), kLoopTop + 1.0, std::ceil(w), (double)kLoopH - 2.0);
                p.fillRect(box.adjusted(-1, -1, 1, 1), QColor(20, 20, 22));
                p.fillRect(box, QColor(255, 255, 255));
                p.setFont(lf);
                p.setPen(QColor(10, 10, 12));
                p.drawText(box, Qt::AlignCenter, text);
            }
        }
    }

    const QFontMetrics fm(smallFont);
    p.setFont(smallFont);
    for (size_t k = 0; k < pr.markers.size(); ++k) {
        const Project::Marker& m = pr.markers[k];
        const QColor col = m.region ? kRegionColor : kMarkerColor;
        const QRectF tag = markerTag((int)k, m);
        const bool hot = hover.marker == m.id;
        if (m.region) {
            const double xe = xAtF(m.time + m.length);
            p.fillRect(QRectF(tag.left(), kLoopTop - 2.0, std::max(0.0, xe - tag.left()), 2.0), QColor(col.red(), col.green(), col.blue(), 150));
            const QRectF end = regionEndTag(m);
            p.fillRect(end, hover.part == Part::RegionEndTag && hot ? col.lighter(130) : col);
        }
        if (tag.right() < c.left() - 200 || tag.left() > c.right()) continue;
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(hot && hover.part == Part::MarkerTag ? col.lighter(130) : col);
        p.drawRoundedRect(tag, 2.0, 2.0);
        if (markerTool && selMarkers.count(m.id)) {
            p.setPen(QPen(QColor(255, 255, 255), 1.6));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(tag.adjusted(0.8, 0.8, -0.8, -0.8), 2.0, 2.0);
        }
        p.setRenderHint(QPainter::Antialiasing, false);
        p.setPen(QColor(16, 16, 16));
        p.drawText(tag, Qt::AlignCenter, QString::number(k + 1));
        if (!m.name.empty()) {
            double limit = lanesRight() - 1;
            for (const auto& o : pr.markers)
                if (o.id != m.id && o.time > m.time) limit = std::min(limit, xAtF(o.time));
            const double w = limit - tag.right() - 6.0;
            if (w > 12) {
                p.setPen(QColor(232, 232, 236));
                p.drawText(QRectF(tag.right() + 3.0, tag.top(), w, tag.height()), Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(QString::fromStdString(m.name), Qt::ElideRight, (int)w));
            }
        }
    }

    const double f = fps();
    const double labelW = rulerLabelWidth();
    const int64_t step = rulerStep(pps, f, labelW);
    const int64_t minor = step / rulerDivisions(step, pps, f);
    p.setFont(smallFont);
    const QFontMetrics rm(smallFont);
    p.setPen(kRulerText);
    for (int64_t k = std::max<int64_t>(0, (int64_t)std::floor(timeAtF(c.left() - labelW) * f / (double)minor));; ++k) {
        const int64_t frames = k * minor;
        const double t = (double)frames / f;
        const double x = std::floor(xAtF(t));
        if (x > c.right()) break;
        if (frames % step == 0) {
            p.fillRect(QRectF(x, kRulerTop + 1.0, 1.0, kRulerH - 1.0), QColor(150, 150, 156));
            p.drawText(QPointF(x + 4.0, kRulerTop + 1.0 + rm.ascent()), QString::fromStdString(rulerTime(pr, t)));
        } else {
            p.fillRect(QRectF(x, kTopH - 4.0, 1.0, 3.0), QColor(150, 150, 156));
        }
    }
    p.restore();
}

void TimelineWidget::Impl::paintRenoCursor(QPainter& p, const QRect& clip)
{
    const QRect c = QRect(headerW, 0, viewW(), std::max(0, lanesBottom())).intersected(clip);
    const double x = std::floor(xAtF(app.cursor()));
    if (c.isEmpty() || x < headerW - 8 || x > lanesRight() + 8) return;
    p.save();
    p.setClipRect(c);
    p.fillRect(QRectF(x, kRulerTop, 1.0, lanesBottom() - kRulerTop), kCursorLine);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(QColor(20, 20, 22), 1.0));
    p.setBrush(kCursorLine);
    QPolygonF head;
    head << QPointF(x - 5.5, kRulerTop) << QPointF(x + 6.5, kRulerTop) << QPointF(x + 0.5, kRulerTop + 8.0);
    p.drawPolygon(head);
    p.restore();
}

// The time display's text is sized by its form, not its digits (every digit counted as an 8, as wide as any in a font
// with digits of different widths), so the size doesn't change from one frame to the next.
QString TimelineWidget::Impl::sizingText(QString text)
{
    for (QChar& c : text)
        if (c.isDigit()) c = QChar('8');
    return text;
}

static QString elidedFormatName(const QFont& font, const QString& title, int width)
{
    static QFont lastFont;
    static QString lastTitle, lastOut;
    static int lastWidth = -1;
    if (lastWidth == width && lastTitle == title && lastFont == font) return lastOut;
    lastOut = QFontMetrics(font).elidedText(title, Qt::ElideRight, width);
    lastFont = font; lastTitle = title; lastWidth = width;
    return lastOut;
}

// The pixel size the big time display is drawn at: the biggest from 'top' down to 10 at which the form of the text
// (every digit an 8) fits 'width'. The time changes every tick but its form and the room don't, so the last answer is
// kept (trying size after size makes a font of each, which costs more than drawing the text).
static int fittingPixelSize(const QFont& base, bool bold, const QString& sizing, int width, int top)
{
    static QFont lastFont;
    static bool lastBold = false, have = false;
    static QString lastSizing;
    static int lastWidth = 0, lastTop = 0, lastPx = 0;
    if (have && lastBold == bold && lastWidth == width && lastTop == top && lastSizing == sizing && lastFont == base) return lastPx;
    QFont f = base;
    if (bold) f.setBold(true);
    int px = top;
    for (; px > 9; --px) {
        f.setPixelSize(px);
        if (QFontMetrics(f).horizontalAdvance(sizing) <= width) break;
    }
    lastPx = px > 9 ? px : 10;
    lastFont = base; lastBold = bold; lastSizing = sizing; lastWidth = width; lastTop = top; have = true;
    return lastPx;
}

// The big time display over the headers, the first build's: bold and light on near black, with what the time counts in
// small over it.
void TimelineWidget::Impl::paintRenoTimeDisplay(QPainter& p, const QRect& clip)
{
    const QRect corner(0, 0, headerW, kTopH);
    if (!corner.intersects(clip)) return;
    p.save();
    p.setClipRect(corner.intersected(clip));
    const QRect inner(6, 1, headerW - kHeaderGap - 12, kTopH - 2);
    // What doesn't change from one time to the next (fills, lines, the format's name) is drawn once into an image and
    // copied; the time is drawn over it. Same pixels, for whole screen pixels.
    const qreal dpr = q->devicePixelRatioF();
    const QColor back = timeBack.isValid() ? timeBack : kPanel;
    const QString title = timeFormatName();
    const bool whole = dpr == std::floor(dpr) && dpr >= 1.0 && dpr <= 4.0;
    if (whole) {
        const QString key = QString("%1 %2 %3 %4 %5 %6 %7 %8 %9").arg(headerW).arg(kTopH).arg(back.rgba(), 0, 16).arg(kLine.rgba(), 0, 16)
                                .arg(kBelowBg.rgba(), 0, 16).arg(kHeaderGap).arg(smallFont.key(), title).arg(dpr);
        if (cornerStatic.isNull() || cornerKey != key) {
            QImage img(QSize(headerW * (int)dpr, kTopH * (int)dpr), QImage::Format_RGB32);
            img.setDevicePixelRatio(dpr);
            {
                QPainter ip(&img);
                ip.fillRect(corner, back);
                ip.fillRect(QRect(0, kTopH - 1, headerW, 1), kLine);
                ip.fillRect(QRect(headerW - kHeaderGap, 0, kHeaderGap, kTopH), kBelowBg);
                ip.fillRect(QRect(headerW - kHeaderGap, 0, 1, kTopH), kLine);
                ip.setFont(smallFont);
                ip.setPen(QColor(120, 120, 126));
                ip.drawText(QRect(inner.left(), 1, inner.width(), 12), Qt::AlignLeft | Qt::AlignVCenter, elidedFormatName(smallFont, title, inner.width()));
            }
            cornerStatic = img;
            cornerKey = key;
        }
        p.drawImage(QPoint(0, 0), cornerStatic);
    } else {
        p.fillRect(corner, back);
        p.fillRect(QRect(0, kTopH - 1, headerW, 1), kLine);
        p.fillRect(QRect(headerW - kHeaderGap, 0, kHeaderGap, kTopH), kBelowBg);
        p.fillRect(QRect(headerW - kHeaderGap, 0, 1, kTopH), kLine);
        p.setFont(smallFont);
        p.setPen(QColor(120, 120, 126));
        p.drawText(QRect(inner.left(), 1, inner.width(), 12), Qt::AlignLeft | Qt::AlignVCenter, elidedFormatName(smallFont, title, inner.width()));
    }
    QFont f = q->font();
    f.setBold(true);
    const QString text = timeDisplayText();
    f.setPixelSize(fittingPixelSize(q->font(), true, sizingText(text), inner.width(), 22));
    p.setFont(f);
    p.setPen(timeText.isValid() ? timeText : kTimeText);
    p.drawText(QRect(inner.left(), 12, inner.width(), kTopH - 13), Qt::AlignCenter, text);
    p.restore();
}

void TimelineWidget::Impl::paintHeaders(QPainter& p, const QRect& clip)
{
    if (renoTheme()) return paintRenoHeaders(p, clip);
    const QRect c = QRect(0, kTopH, headerW, std::max(0, lanesBottom() - kTopH)).intersected(clip);
    if (c.isEmpty()) return;
    p.save();
    p.setClipRect(c);
    p.fillRect(c, kBelowBg);
    const Project& pr = app.project();
    const QFontMetrics nameMetrics(q->font());
    QFont number = q->font();
    number.setBold(true);
    number.setPixelSize(13);
    for (const Row& r : rows) {
        const HeaderRects h = headerRects(r);
        if (!h.all.intersects(c)) continue;
        drawHeaderCached(p, r, h.all, kBelowBg, [&](QPainter& p) {
        const Track& tr = pr.tracks[(size_t)r.index];
        const bool current = r.id == app.currentTrack || selTracks.count(r.id);
        const bool hot = hover.onHeader() && hover.track == r.id;
        const QColor tc = trackColor(tr, r.index);
        p.fillRect(h.body, current ? kHeaderCurrent : kHeaderBg);
        p.fillRect(h.strip, tc);
        if (tr.trackGroup) p.fillRect(QRect(h.all.right() - 3, h.all.top(), 3, h.all.height()), defaultTrackColor((int)(tr.trackGroup % 8)).lighter(120));
        {
            const bool menuHot = hot && hover.part == Part::HeaderMenu;
            if (menuHot) p.fillRect(h.menu, QColor(255, 255, 255, 50));
            p.save();
            outlined(p, [&](const QColor& col) { drawMenuIcon(p, QRectF(h.menu), col); });
            p.restore();
            if (r.h >= 40) {
                p.setFont(number);
                const QString n = QString::number(r.index + 1);
                const QRect nr(h.strip.left() + 3, h.strip.top() + (r.video ? 20 : 24), 12, 16);
                p.setPen(QColor(0, 0, 0, 170));
                p.drawText(nr.translated(1, 1), Qt::AlignLeft | Qt::AlignTop, n);
                p.setPen(QColor(255, 255, 255));
                p.drawText(nr, Qt::AlignLeft | Qt::AlignTop, n);
                const int nw = QFontMetrics(number).horizontalAdvance(n);
                drawKindIcon(p, QPointF(nr.left() + nw + 1.5, nr.top() + 8.5), r.video);
                if (tr.adjustment) {
                    const QPointF ac(nr.left() + nw + 6.0, nr.top() + 13.5);
                    p.save();
                    p.setRenderHint(QPainter::Antialiasing, true);
                    p.setPen(QPen(QColor(20, 20, 20), 1.0));
                    p.setBrush(QColor(240, 240, 240));
                    p.drawEllipse(ac, 4.5, 4.5);
                    p.setBrush(QColor(20, 20, 20));
                    p.drawPie(QRectF(ac.x() - 4.5, ac.y() - 4.5, 9.0, 9.0), 90 * 16, 180 * 16);
                    p.restore();
                }
            }
        }

        const QColor text = kText;
        if (!(naming == Naming::Track && renaming == r.id) && h.name.width() > 8) {
            if (hot && hover.part == Part::HeaderName) {
                p.setPen(QColor(110, 110, 110));
                p.setBrush(QColor(0, 0, 0, 40));
                p.drawRect(h.name.adjusted(-2, 0, 0, -1));
            }
            if (!tr.name.empty()) {
                p.setFont(q->font());
                p.setPen(current ? kTextOnLight : text);
                p.drawText(h.name.adjusted(1, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft,
                           nameMetrics.elidedText(QString::fromStdString(tr.name), Qt::ElideRight, h.name.width() - 2));
            }
        }

        bool bad = false;
        for (const FxSlot& s : tr.fx)
            if (!app.fx.error(s.id).empty()) bad = true;
        if (!h.motion.isEmpty()) drawHeaderButton(p, h.motion, Icon::Motion, !tr.motion.empty(), hot && hover.part == Part::HeaderMotion, QColor(24, 24, 24));
        if (!h.fx.isEmpty()) drawHeaderButton(p, h.fx, Icon::Fx, !tr.fx.empty(), hot && hover.part == Part::HeaderFx, bad ? QColor(150, 20, 10) : QColor(24, 24, 24));
        if (!h.autoBtn.isEmpty()) {
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(tr.automation ? QColor(255, 255, 255) : QColor(30, 30, 30), tr.automation ? 1.4 : 1.0));
            QColor fill = tr.automation ? QColor(96, 150, 220) : QColor(150, 150, 150);
            if (hot && hover.part == Part::HeaderAuto) fill = fill.lighter(118);
            p.setBrush(fill);
            p.drawEllipse(QRectF(h.autoBtn).adjusted(0.5, 0.5, -0.5, -0.5));
            p.restore();
            drawAutoIcon(p, h.autoBtn, tr.automation, tr.automationMode, false);
            drawAutoArrow(p, h.autoBtn, false);
        }
        if (!h.arm.isEmpty()) drawHeaderButton(p, h.arm, Icon::Arm, app.armedTracks.count(tr.id) != 0, hot && hover.part == Part::HeaderArm, QColor());
        const bool muteShown = tr.automation ? shownAutomation(tr, "mute") >= 0.5 : tr.mute;
        drawHeaderButton(p, h.mute, Icon::Mute, muteShown, hot && hover.part == Part::HeaderMute, QColor());
        if (tr.automation) p.fillRect(QRectF(h.mute.left() + 4, h.mute.bottom() + 1, h.mute.width() - 8, 1.5), QColor(255, 176, 60));
        drawHeaderButton(p, h.solo, Icon::Solo, tr.solo, hot && hover.part == Part::HeaderSolo, QColor());

        p.setFont(smallFont);
        if (!h.level.isEmpty()) {
            const bool levelHot = (hot && hover.part == Part::HeaderLevel) || (drag.kind == Drag::Level && drag.hit.track == r.id);
            const double lv = tr.automation ? shownAutomation(tr, "volume") : tr.level;
            const double ls = r.video ? clampd(lv, 0.0, 1.0) : volumeToSlider(lv);
            drawSlider(p, h.level, ls, false, levelHot, current);
            if (tr.automation) drawAutoKnob(p, h.level, ls);
            if (!h.levelValue.isEmpty()) {
                p.setPen(text);
                p.drawText(h.levelValue, Qt::AlignVCenter | Qt::AlignLeft, levelText(r.video, lv));
            }
        }
        if (!h.pan.isEmpty()) {
            const bool panHot = (hot && hover.part == Part::HeaderPan) || (drag.kind == Drag::Pan && drag.hit.track == r.id);
            const double pv = tr.automation ? shownAutomation(tr, "pan") : tr.pan;
            drawSlider(p, h.pan, clampd((pv + 1.0) * 0.5, 0.0, 1.0), true, panHot, current);
            if (tr.automation) drawAutoKnob(p, h.pan, clampd((pv + 1.0) * 0.5, 0.0, 1.0));
            p.setPen(text);
            p.drawText(h.panValue, Qt::AlignVCenter | Qt::AlignLeft, panText(pv));
        }
        if (!h.fade.isEmpty()) {
            const bool fadeHot = (hot && hover.part == Part::HeaderFade) || (drag.kind == Drag::Fade && drag.hit.track == r.id);
            const double fv = shownAutomation(tr, "fadecolor");
            drawSlider(p, h.fade, clampd((fv + 1.0) * 0.5, 0.0, 1.0), true, fadeHot, current);
            drawAutoKnob(p, h.fade, clampd((fv + 1.0) * 0.5, 0.0, 1.0));
        }
        if (!h.phase.isEmpty()) {
            p.setFont(smallFont);
            drawRenoButton(p, h.phase, QString::fromUtf8("\u00d8"), tr.invertPhase, QColor(96, 150, 220), hot && hover.part == Part::HeaderPhase);
            drawRenoButton(p, h.bus, QFontMetrics(smallFont).elidedText(busLabel(tr), Qt::ElideRight, h.bus.width() - 6), tr.output != 0, QColor(96, 150, 220),
                           hot && hover.part == Part::HeaderBus);
        }
        if (!h.surround.isEmpty()) drawSmallSurround(p, h.surround, tr.surround,
                                                     (hot && hover.part == Part::HeaderSurround) || (drag.kind == Drag::SurroundPoint && drag.hit.track == r.id), false);
        if (drop.ok && !drop.event && drop.track == r.id && (drop.kind == DropKind::AudioFx || drop.kind == DropKind::VideoFx)) {
            p.setPen(QPen(kDrop, 2.0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(h.all.adjusted(1, 1, -2, -2));
        }
        });
    }
    if (trackListFocus && q->hasFocus()) {
        p.setPen(QPen(kSnapLine, 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRect(0, kTopH, headerW - kHeaderGap - 1, std::max(0, lanesBottom() - kTopH - 1)));
    }
    for (const BusRow& b : busRows) {
        const QRect all(0, busTop(b), headerW, b.h);
        if (!all.intersects(c)) continue;
        const int bx = kHeaderMargin + kStripW + 1, br = headerW - kHeaderGap;
        const bool hot = hover.part == Part::BusHeader && hover.bus == b.owner;
        p.fillRect(QRect(bx, all.top() + 1, br - bx, b.h - 1), hot ? QColor(60, 60, 64) : QColor(52, 52, 56));
        p.fillRect(QRect(kHeaderMargin, all.top() + 1, kStripW, b.h - 1), b.video ? QColor(96, 96, 128) : QColor(92, 116, 96));
        drawKindIcon(p, QPointF(kHeaderMargin + 7.0, all.top() + 8.0), b.video);
        p.setFont(q->font());
        p.setPen(kText);
        p.drawText(QRect(bx + 4, all.top() + 3, br - bx - 8, 17), Qt::AlignVCenter | Qt::AlignLeft,
                   nameMetrics.elidedText(b.name, Qt::ElideRight, br - bx - 8));
        QString sub;
        if (b.owner.kind == EnvelopeOwner::Master) sub = QString("Volume: %1").arg(levelText(false, pr.masterVolume));
        else if (const Bus* bus = b.owner.kind == EnvelopeOwner::Bus ? pr.findBus(b.owner.id) : nullptr) sub = QString("Volume: %1").arg(levelText(false, bus->level));
        else sub = QString("Video output");
        p.setFont(smallFont);
        p.setPen(kDimText);
        p.drawText(QRect(bx + 4, all.top() + 22, br - bx - 8, 14), Qt::AlignVCenter | Qt::AlignLeft, sub);
        p.fillRect(QRect(0, all.bottom(), headerW, 1), kLine);
    }
    const bool gapHot = hover.part == Part::HeaderGap || drag.kind == Drag::HeaderWidth;
    p.fillRect(QRect(headerW - kHeaderGap, c.top(), kHeaderGap, c.height()), gapHot ? QColor(60, 60, 60) : kLine);
    p.restore();
}

void TimelineWidget::Impl::paintRuler(QPainter& p, const QRect& clip)
{
    if (renoTheme()) return paintRenoRuler(p, clip);
    const QRect c = QRect(headerW, 0, viewW(), kTopH).intersected(clip);
    if (c.isEmpty()) return;
    const Project& pr = app.project();
    p.save();
    p.setClipRect(c);
    p.fillRect(QRect(c.left(), 0, c.width(), kTopH), kRulerBg);
    p.fillRect(QRect(c.left(), 0, c.width(), 1), kLightLine);
    p.fillRect(QRect(c.left(), kRulerTop - 2, c.width(), 1), kLine);
    p.fillRect(QRect(c.left(), kRulerTop - 1, c.width(), 1), kLightLine);

    // the time selection: a bar between its two yellow flags (grey while looping is off); with none, VEGAS still shows
    // the flag where the last one began
    p.setRenderHint(QPainter::Antialiasing, false);
    const bool region = app.loopEnd > app.loopStart;
    const double x0 = clampd(std::floor(xAtF(app.loopStart)), -1e6, 1e6);
    const double x1 = clampd(std::floor(xAtF(app.loopEnd)), -1e6, 1e6);
    const QColor flag = app.loopOn ? kLoopOn : kLoopOff;
    const double fy = kRulerTop - 2 - 7;
    if (region) p.fillRect(QRectF(x0, fy + 1.0, std::max(1.0, x1 - x0), 5.0), app.loopOn ? QColor(150, 140, 30) : QColor(100, 100, 100));
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(flag);
    QPolygonF a;
    a << QPointF(x0, fy) << QPointF(x0 + 7.0, fy) << QPointF(x0, fy + 7.0);
    p.drawPolygon(a);
    if (region) {
        QPolygonF b;
        b << QPointF(x1 + 1.0, fy) << QPointF(x1 + 1.0 - 7.0, fy) << QPointF(x1 + 1.0, fy + 7.0);
        p.drawPolygon(b);
    }
    p.setRenderHint(QPainter::Antialiasing, false);

    const QFontMetrics fm(smallFont);
    p.setFont(smallFont);
    for (size_t k = 0; k < pr.markers.size(); ++k) {
        const Project::Marker& m = pr.markers[k];
        const QColor col = markerColor(m);
        const QRectF tag = markerTag((int)k, m);
        const bool hot = hover.marker == m.id;
        if (m.region) {
            const double xe = xAtF(m.time + m.length);
            p.fillRect(QRectF(tag.left(), kLoopTop - 2.0, std::max(0.0, xe - tag.left()), 2.0), QColor(col.red(), col.green(), col.blue(), 150));
            const QRectF end = regionEndTag(m);
            p.fillRect(end, hover.part == Part::RegionEndTag && hot ? col.lighter(130) : col);
        }
        if (tag.right() < c.left() - 200 || tag.left() > c.right()) continue;
        p.fillRect(tag, hot && hover.part == Part::MarkerTag ? col.lighter(130) : col);
        if (markerTool && selMarkers.count(m.id)) {
            p.setPen(QPen(QColor(255, 255, 255), 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawRect(tag.adjusted(0.5, 0.5, -0.5, -0.5));
        }
        p.setPen(QColor(16, 16, 16));
        p.drawText(tag, Qt::AlignCenter, QString::number(k + 1));
        if (!m.name.empty()) {
            double limit = lanesRight() - 1;
            for (const auto& o : pr.markers)
                if (o.id != m.id && o.time > m.time) limit = std::min(limit, xAtF(o.time));
            const double w = limit - tag.right() - 6.0;
            if (w > 12) {
                p.setPen(QColor(232, 232, 232));
                p.drawText(QRectF(tag.right() + 3.0, tag.top(), w, tag.height()), Qt::AlignVCenter | Qt::AlignLeft,
                           fm.elidedText(QString::fromStdString(m.name), Qt::ElideRight, (int)w));
            }
        }
    }

    const double f = fps();
    const double labelW = rulerLabelWidth();
    const int64_t step = rulerStep(pps, f, labelW);
    const int64_t minor = step / rulerDivisions(step, pps, f);
    p.setPen(kRulerText);
    p.setFont(rulerFont);
    const QFontMetrics rm(rulerFont);
    for (int64_t k = std::max<int64_t>(0, (int64_t)std::floor(timeAtF(c.left() - labelW) * f / (double)minor));; ++k) {
        const int64_t frames = k * minor;
        const double t = (double)frames / f;
        const double x = std::floor(xAtF(t));
        if (x > c.right()) break;
        if (frames % step == 0) {
            p.fillRect(QRectF(x, kRulerTop + 4.0, 1.0, kRulerH - 4.0), kRulerText);
            p.drawText(QPointF(x + 2.0, kRulerTop + 2.0 + rm.ascent()), QString::fromStdString(rulerTime(pr, t)));
        } else {
            p.fillRect(QRectF(x, kTopH - 4.0, 1.0, 4.0), kRulerText);
        }
    }
    p.restore();
}

void TimelineWidget::Impl::paintCursor(QPainter& p, const QRect& clip)
{
    if (renoTheme()) return paintRenoCursor(p, clip);
    const QRect c = QRect(headerW, 0, viewW(), std::max(0, lanesBottom())).intersected(clip);
    const double x = std::floor(xAtF(app.cursor()));
    if (c.isEmpty() || x < headerW - 8 || x > lanesRight() + 8) return;
    p.save();
    p.setClipRect(c);
    p.fillRect(QRectF(x, kRulerTop, 1.0, lanesBottom() - kRulerTop), kCursorLine);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath head;
    head.addRoundedRect(QRectF(x - 5.5, kRulerTop + 0.5, 12.0, 10.0), 2.0, 2.0);
    QPolygonF tip;
    tip << QPointF(x - 3.0, kRulerTop + 10.0) << QPointF(x + 4.0, kRulerTop + 10.0) << QPointF(x + 0.5, kRulerTop + 14.0);
    head.addPolygon(tip);
    head = head.simplified();
    p.setPen(QPen(QColor(20, 20, 20), 1.0));
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(head);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(20, 20, 20));
    p.drawRect(QRectF(x - 2.0, kRulerTop + 3.0, 1.5, 4.0));
    p.drawRect(QRectF(x + 1.5, kRulerTop + 3.0, 1.5, 4.0));
    p.restore();
}

void TimelineWidget::Impl::paintCorners(QPainter& p, const QRect& clip)
{
    const QRect corner(0, 0, headerW, kTopH);
    if (renoTheme()) paintRenoTimeDisplay(p, clip);
    else if (corner.intersects(clip)) {
        p.save();
        p.setClipRect(corner.intersected(clip));
        p.fillRect(corner, timeBack.isValid() ? timeBack : kPanel);
        p.fillRect(QRect(0, 0, headerW, 1), kHeaderCurrent);
        p.fillRect(QRect(0, kTopH - 1, headerW, 1), kLine);
        p.fillRect(QRect(headerW - kHeaderGap, 1, kHeaderGap, kTopH), kLine);
        p.setFont(smallFont);
        p.setPen(kText);
        const QRect inner(4, 1, headerW - kHeaderGap - 8, kTopH - 2);
        p.drawText(QRect(inner.left(), 2, inner.width(), 13), Qt::AlignLeft | Qt::AlignVCenter,
                   elidedFormatName(smallFont, timeFormatName(), inner.width()));
        for (int gy = 22; gy <= kTopH - 8; gy += 4) p.fillRect(QRect(5, gy, 2, 2), QColor(88, 88, 88));
        QFont f = q->font();
        const QString text = timeDisplayText();
        f.setPixelSize(fittingPixelSize(q->font(), false, sizingText(text), inner.width() - 6, 21));
        p.setFont(f);
        p.setPen(timeText.isValid() ? timeText : kTimeText);
        p.drawText(QRect(inner.left(), 15, inner.width() - 2, kTopH - 17), Qt::AlignRight | Qt::AlignVCenter, text);
        p.restore();
    }
    const int w = q->width(), h = q->height();
    p.fillRect(QRect(0, lanesBottom(), headerW, h - lanesBottom()).intersected(clip), kWindow);
    p.fillRect(QRect(lanesRight(), 0, w - lanesRight(), kLoopTop).intersected(clip), kWindow);
}

QString TimelineWidget::Impl::tipAt(const QPointF& pos) const
{
    const Hit h = hitTest(pos);
    const Project& pr = app.project();
    auto chain = [&](const FxChain& fx, const QString& title) {
        QString s = title;
        for (const FxSlot& f : fx) {
            const std::string err = app.fx.error(f.id);
            s += QString("\n%1%2").arg(QString::fromStdString(fxSlotLabel(f)), err.empty() ? QString() : QString(" - %1").arg(QString::fromStdString(err)));
        }
        return s;
    };
    switch (h.part) {
    case Part::HeaderArm: return QString("Arm for Record (Ctrl+Alt+R)");
    case Part::HeaderSurround: return QString("Surround Pan (drag the point; double-click: the Surround Panner, Ctrl+Alt+3)");
    case Part::HeaderMute: return QString("Mute (Z) - Ctrl+click: this track alone, or none");
    case Part::HeaderAuto: return QString("Automation Settings (its arrow: Off, Read, Automation Write (Touch), Automation Write (Latch))");
    case Part::HeaderFade: return QString("Fade to color (automation)");
    case Part::HeaderPhase: return QString("Invert Track Phase");
    case Part::HeaderBus: return QString("Bus: where the track's sound goes");
    case Part::HeaderSolo: return QString("Solo (X)");
    case Part::HeaderMotion: return QString("Track Motion");
    case Part::TrackKey: return QString("Track Motion keyframe: drag to move it, double-click for Track Motion, right-click for its type");
    case Part::HeaderMenu: return QString("Track menu");
    case Part::HeaderGap: return QString("Drag to make the track headers wider or narrower");
    case Part::HeaderEdge: return QString("Drag to make the track taller or shorter (all the selected tracks, if it is one of them)");
    case Part::EventCrop: return QString("Event Pan/Crop");
    case Part::EventMenu: return QString("Event menu");
    case Part::EventSnap: {
        const Event* se = pr.findEvent(h.event);
        return QString("Snap offset: %1 (drag along the event: where it snaps when it is moved)")
            .arg(QString::fromStdString(timecode(se ? se->snapOffset : 0.0, fps())));
    }
    case Part::MarkerTag:
    case Part::RegionEndTag: {
        const int k = markerIndex(h.marker);
        if (k < 0) return QString();
        const Project::Marker& m = pr.markers[(size_t)k];
        QString s = QString("%1 %2%3\n%4").arg(m.region ? "Region" : "Marker").arg(k + 1)
                        .arg(m.name.empty() ? QString() : QString(": %1").arg(QString::fromStdString(m.name)))
                        .arg(QString::fromStdString(rulerTime(pr, m.time)));
        if (m.region) s += QString(" - %1").arg(QString::fromStdString(rulerTime(pr, m.time + m.length)));
        return s;
    }
    case Part::HeaderFx:
        for (const Track& t : pr.tracks)
            if (t.id == h.track) return chain(t.fx, "Track FX");
        return QString();
    case Part::HeaderLevel:
        for (const Track& t : pr.tracks)
            if (t.id == h.track) return QString(t.type == TrackType::Video ? "Level: %1" : "Volume: %1").arg(levelText(t.type == TrackType::Video, t.level));
        return QString();
    case Part::HeaderPan:
        for (const Track& t : pr.tracks)
            if (t.id == h.track) return QString("Pan: %1").arg(panText(t.pan));
        return QString();
    case Part::EventFx:
        if (const Event* e = pr.findEvent(h.event)) return chain(e->fx, "Event FX");
        return QString();
    case Part::EnvPoint:
    case Part::EnvLine: {
        EnvLane L;
        if (!envLaneOf(h.env, &L)) return QString();
        QString name = envelopeName(h.env.kind);
        if (h.env.kind == "fx") name += QString(": %1").arg(QString::fromStdString(h.env.param));
        if (h.env.kind == "send")
            if (const Bus* b = pr.findBus(h.env.target)) name += QString(" to %1").arg(QString::fromStdString(b->name));
        if (h.part == Part::EnvPoint && h.point < (int)L.env->points.size()) {
            const EnvPoint& pt = L.env->points[(size_t)h.point];
            return QString("%1: %2\n%3 (%4)").arg(name, envValueText(h.env.kind, pt.value), QString::fromStdString(rulerTime(pr, L.origin + pt.time)),
                                                 QString(kCurveNames[std::max(0, std::min(pt.curve, (int)CurveHold))]));
        }
        return QString("%1: %2\nDouble-click to add a point").arg(name, envValueText(h.env.kind, envValueAt(L, timeAtF(pos.x()))));
    }
    case Part::BusHeader: return QString("Bus track: right-click for its envelopes");
    case Part::FadeIn: return QString("Fade In");
    case Part::FadeOut: return QString("Fade Out");
    case Part::EventGain:
        if (const Event* e = pr.findEvent(h.event)) {
            const Row* gr = rowOf(h.track);
            return gainText(gr && gr->video, e->gain) + QString("\nDrag to change it (Ctrl or the right button: finely)");
        }
        return QString();
    case Part::EventBody:
        if (const Event* e = pr.findEvent(h.event)) {
            QString s = QString("%1\nStart %2   Length %3").arg(QString::fromStdString(e->name), QString::fromStdString(rulerTime(pr, e->start)), QString::fromStdString(timecode(e->length, fps())));
            if (std::fabs(e->rate - 1.0) > 1e-6) s += QString::fromStdString(strfmt("\nPlayback Rate %d%%", (int)std::lround(e->rate * 100.0)));
            if (!e->transition.kind.empty()) s += QString("\nTransition: %1").arg(QString::fromStdString(e->transition.name));
            return s;
        }
        return QString();
    default: return QString();
    }
}

}  // namespace app
}  // namespace reno
