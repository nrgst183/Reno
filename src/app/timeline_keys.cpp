// Keyboard handling of the timeline and the box for typing a name.
#include "timeline_internal.h"

namespace reno {
namespace app {

void TimelineWidget::Impl::keyCursor(double t)
{
    if (playback) playback->jumpTo(std::max(0.0, t));
    else app.setCursor(std::max(0.0, t));
    scrollToCursor();
}

double TimelineWidget::Impl::stepFrom(double t, int dir) const
{
    const Project& p = app.project();
    double n = t + dir / pps;
    if (opt.quantize) {
        n = p.quantize(n);
        if (std::fabs(n - t) < frame() * 0.5) n = p.frameTime(std::max<int64_t>(0, p.frameAt(t) + dir));
    }
    return std::max(0.0, n);
}

bool TimelineWidget::Impl::key(QKeyEvent* ev)
{
    const Qt::KeyboardModifiers mods = ev->modifiers() & ~Qt::KeypadModifier;
    const bool keypad = (ev->modifiers() & Qt::KeypadModifier) != 0;
    const bool shift = (mods & Qt::ShiftModifier) != 0, ctrl = (mods & Qt::ControlModifier) != 0, alt = (mods & Qt::AltModifier) != 0;
    const bool plain = mods == Qt::NoModifier;
    const Project& pr = app.project();
    if (markerTool && !keypad) {
        if ((ev->key() == Qt::Key_Left || ev->key() == Qt::Key_Right) && (plain || mods == Qt::ShiftModifier))
            return markerToolKey(ev->key() == Qt::Key_Right ? 1 : -1, shift);
        if (ev->key() == Qt::Key_Delete && plain && !selMarkers.empty()) {
            deleteChosenMarkers();
            return true;
        }
        if (ev->key() == Qt::Key_Escape) {
            setMarkerTool(false);
            return true;
        }
    }
    if (keypad && (drag.kind == Drag::EnvPoints || drag.kind == Drag::EnvSegment) && drag.started) {
        const int k = ev->key();
        if (k == Qt::Key_8 || k == Qt::Key_2 || k == Qt::Key_4 || k == Qt::Key_6) {
            if (k == Qt::Key_8) drag.pressPos.ry() += 1.0;
            if (k == Qt::Key_2) drag.pressPos.ry() -= 1.0;
            if (k == Qt::Key_4 && drag.kind == Drag::EnvPoints) drag.pressTime += 1.0 / pps;
            if (k == Qt::Key_6 && drag.kind == Drag::EnvPoints) drag.pressTime -= 1.0 / pps;
            updateEnvelope(drag.lastPos, drag.lastMods);
            return true;
        }
    }
    if (keypad) {
        switch (ev->key()) {
        case Qt::Key_Slash:
            if (ctrl) convertTransitionToCut();
            else convertCutToTransition(0);
            return true;
        case Qt::Key_Asterisk: convertCutToTransition(1); return true;
        case Qt::Key_Minus: convertCutToTransition(2); return true;
        case Qt::Key_1: keypadEdit(-1, 0, mods); return true;
        case Qt::Key_3: keypadEdit(1, 0, mods); return true;
        case Qt::Key_4: keypadEdit(0, -1, mods); return true;
        case Qt::Key_6: keypadEdit(0, 1, mods); return true;
        case Qt::Key_8: if (plain) { nudge(0, 0, -1); return true; } break;
        case Qt::Key_2: if (plain) { nudge(0, 0, 1); return true; } break;
        case Qt::Key_7: if (plain) { selectEdge(-1); return true; } break;
        case Qt::Key_9: if (plain) { selectEdge(1); return true; } break;
        case Qt::Key_5: if (plain) { toggleExpandedEdit(); return true; } break;
        case Qt::Key_Clear: if (plain) { toggleExpandedEdit(); return true; } break;
        default: break;
        }
    }
    switch (ev->key()) {
    case Qt::Key_Escape:
        if (cancelMouseDrag()) return true;
        if (edgeSel.event) { exitEditMode(); return true; }
        return false;
    case Qt::Key_Left:
    case Qt::Key_Right: {
        const int dir = ev->key() == Qt::Key_Right ? 1 : -1;
        if (ctrl && alt) cursorToEdge(dir, EdgeKind::EditPoints);
        else if (ctrl) cursorToEdge(dir, EdgeKind::Markers);
        else if (alt) keyCursor(pr.frameTime(std::max<int64_t>(0, pr.frameAt(app.cursor()) + dir)));
        else if (shift) {
            const double c = app.cursor();
            double anchor = c;
            if (app.loopEnd > app.loopStart) {
                if (std::fabs(c - app.loopEnd) < 1e-9) anchor = app.loopStart;
                else if (std::fabs(c - app.loopStart) < 1e-9) anchor = app.loopEnd;
            }
            const double t = stepFrom(c, dir);
            keyCursor(t);
            app.setLoop(anchor, t);
        } else keyCursor(stepFrom(app.cursor(), dir));
        return true;
    }
    case Qt::Key_Up:
    case Qt::Key_Down: {
        const bool up = ev->key() == Qt::Key_Up;
        if (ctrl && shift) changeTrackHeights(up ? 1 : -1);
        else if (shift && !ctrl && !alt) {
            waveZoom = clampd(waveZoom * (up ? 1.5 : 1.0 / 1.5), 1.0, 32.0);
            if (waveZoom < 1.01) waveZoom = 1.0;
            app.status(strfmt("Waveforms zoomed %.1f x", waveZoom));
            q->update(lanesRect());
        }
        else if (ctrl) zoomLarge(up);
        else if (plain) zoomBy(up ? 1.5 : 1.0 / 1.5);
        else return false;
        return true;
    }
    case Qt::Key_Home:
        if (ctrl) keyCursor(0.0);
        else if (plain) keyCursor(homeTime());
        else return false;
        return true;
    case Qt::Key_End:
        if (ctrl) keyCursor(pr.length());
        else if (plain) keyCursor(endTime());
        else return false;
        return true;
    case Qt::Key_PageUp:
    case Qt::Key_PageDown: {
        if (!plain) return false;
        const double step = gridStep();
        const double c = app.cursor();
        double t = ev->key() == Qt::Key_PageDown ? (std::floor(c / step + 1e-6) + 1.0) * step : (std::ceil(c / step - 1e-6) - 1.0) * step;
        keyCursor(std::max(0.0, t));
        return true;
    }
    case Qt::Key_F2:
        if (!plain) return false;
        renameSelected();
        return true;
    case Qt::Key_QuoteLeft:
        if (!plain) return false;
        toggleMinimized(app.currentTrack);
        return true;
    case Qt::Key_Backslash:
        if (!plain) return false;
        centerOnCursor();
        return true;
    case Qt::Key_BracketLeft:
    case Qt::Key_BracketRight: {
        const int dir = ev->key() == Qt::Key_BracketRight ? 1 : -1;
        if (ctrl && shift) timeSelectSelected(dir);
        else if (ctrl) cursorToEdge(dir, EdgeKind::Selected);
        else if (alt) {
            if (dir < 0) trimStart();
            else trimEnd();
        } else if (plain) cursorToEdge(dir, EdgeKind::Events);
        else return false;
        return true;
    }
    case Qt::Key_I:
    case Qt::Key_O: {
        if (!plain) return false;
        const double c = app.cursor();
        if (ev->key() == Qt::Key_I) app.setLoop(c, app.loopEnd > c ? app.loopEnd : c);
        else app.setLoop(app.loopStart < c ? app.loopStart : c, c);
        return true;
    }
    default:
        return false;
    }
}

bool TimelineWidget::Impl::namedStillThere() const
{
    switch (naming) {
    case Naming::Track: return rowOf(renaming) != nullptr;
    case Naming::Event: return app.project().findEvent(renaming) != nullptr;
    case Naming::Marker: return markerIndex(renaming) >= 0;
    default: return false;
    }
}

void TimelineWidget::Impl::placeNameEdit()
{
    if (naming == Naming::None || !nameEdit) return;
    QRect n;
    if (naming == Naming::Track) {
        const Row* r = rowOf(renaming);
        if (!r) return;
        const HeaderRects h = headerRects(*r);
        n = QRect(h.name.left() - 3, h.name.top() - 1, h.solo.right() - h.name.left() + 4, h.name.height() + 2);
        nameEdit->setVisible(n.top() >= kTopH - 2 && n.bottom() <= lanesBottom());
    } else if (naming == Naming::Event) {
        const Track* tr = nullptr;
        const Event* e = app.project().findEvent(renaming, &tr);
        const Row* r = (e && tr) ? rowOf(tr->id) : nullptr;
        if (!r) return;
        const QRectF er = eventRect(*r, *e);
        const int left = (int)std::max(er.left(), (double)headerW);
        const int right = std::max(left + 160, (int)std::min(er.right(), (double)lanesRight()));
        n = QRect(left, (int)er.top() - 2, right - left, kTitleH + 6);
        nameEdit->setVisible(n.top() >= kTopH - 2 && n.bottom() <= lanesBottom());
    } else {
        const int k = markerIndex(renaming);
        if (k < 0) return;
        const QRectF tag = markerTag(k, app.project().markers[(size_t)k]);
        n = QRect((int)tag.right() + 2, kMarkerTop, 180, kMarkerH + 2);
        nameEdit->setVisible(true);
    }
    nameEdit->setGeometry(n);
}

void TimelineWidget::Impl::startRename(Naming kind, uint64_t id)
{
    std::string current;
    if (kind == Naming::Track) {
        const Row* r = rowOf(id);
        const Track* tr = r ? trackOf(*r) : nullptr;
        if (!tr) return;
        current = tr->name;
    } else if (kind == Naming::Event) {
        const Event* e = app.project().findEvent(id);
        if (!e) return;
        current = e->name;
    } else {
        const int k = markerIndex(id);
        if (k < 0) return;
        current = app.project().markers[(size_t)k].name;
    }
    if (naming != Naming::None) endRename(true);
    naming = kind;
    renaming = id;
    nameEdit->setText(QString::fromStdString(current));
    placeNameEdit();
    nameEdit->show();
    nameEdit->raise();
    nameEdit->setFocus(Qt::OtherFocusReason);
    nameEdit->selectAll();
    q->update();
}

void TimelineWidget::Impl::endRename(bool keep)
{
    if (naming == Naming::None) return;
    const Naming kind = naming;
    const uint64_t id = renaming;
    naming = Naming::None;
    renaming = 0;
    const std::string text = nameEdit->text().trimmed().toStdString();
    const bool hadFocus = nameEdit->hasFocus();
    nameEdit->hide();
    if (hadFocus) q->setFocus(Qt::OtherFocusReason);
    if (keep) {
        if (kind == Naming::Track) {
            const Row* r = rowOf(id);
            const Track* tr = r ? trackOf(*r) : nullptr;
            if (tr && tr->name != text)
                app.edit("Rename Track", [&](Project& p) { if (Track* t = p.findTrack(id)) t->name = text; });
        } else if (kind == Naming::Event) {
            const Event* e = app.project().findEvent(id);
            if (e && e->name != text)
                app.edit("Rename Event", [&](Project& p) { if (Event* o = p.findEvent(id)) o->name = text; });
        } else {
            const int k = markerIndex(id);
            if (k >= 0 && app.project().markers[(size_t)k].name != text)
                app.edit("Rename Marker", [&](Project& p) { for (auto& m : p.markers) if (m.id == id) m.name = text; });
        }
    }
    q->update();
}

}  // namespace app
}  // namespace reno
