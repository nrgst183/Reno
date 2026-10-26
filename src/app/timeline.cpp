// The timeline. One widget paints everything (time display, loop bar, marker bar, ruler, track headers, events) and
// works out
// what is under the mouse itself; the only child widgets are the two scroll bars and the box for typing a name.
//
// Layout and colors follow VEGAS Pro's dark track view. Thumbnails and waveforms are made on a thread of their own (Loader)
// and cached; painting only uses what is there and requests the rest.
#include "timeline_internal.h"
#include <cstdlib>

namespace reno {
namespace app {

TimelineWidget::Impl::Impl(TimelineWidget* w, Playback* pb) : q(w), playback(pb), app(AppState::get())
{
    q->setFocusPolicy(Qt::StrongFocus);
    q->setMouseTracking(true);
    q->setAcceptDrops(true);
    q->setMinimumSize(360, 160);
    q->setAutoFillBackground(false);
    q->setAttribute(Qt::WA_OpaquePaintEvent);

    CachedScrollBar* hb = new CachedScrollBar(Qt::Horizontal, q);
    CachedScrollBar* vb = new CachedScrollBar(Qt::Vertical, q);
    hbar = hb;
    vbar = vb;
    // VEGAS: the ends of the scroll box zoom (time across, track height down); double-click zooms out to everything
    hb->setMouseTracking(true);
    vb->setMouseTracking(true);
    hb->onEdgeDrag = [this](int phase, int edge, double pos, double len) { barEdgeDrag(false, phase, edge, pos, len); };
    vb->onEdgeDrag = [this](int phase, int edge, double pos, double len) { barEdgeDrag(true, phase, edge, pos, len); };
    hb->onDoubleClick = [this] { zoomToFit(); };
    vb->onDoubleClick = [this] { if (!rows.empty()) zoomTracks(0, (int)rows.size() - 1); };
    hbar->setCursor(Qt::ArrowCursor);
    vbar->setCursor(Qt::ArrowCursor);
    hbar->setObjectName("timelineHScroll");
    vbar->setObjectName("timelineVScroll");
    shownTheme = uiTheme();
    useColors(shownTheme);
    {
        QSettings s;
        headerW = clampHeaderW(s.value(headerWidthKey(), headerDefault()).toInt());
        for (const QString& k : s.value("timeline/hiddenButtons/video").toStringList()) hiddenButtons[1].insert(k);
        for (const QString& k : s.value("timeline/hiddenButtons/audio").toStringList()) hiddenButtons[0].insert(k);
    }
    zoomInButton = new BarButton(q, "zoomInButton", "Zoom In Time (Up)", [this] { zoomBy(1.5); }, [](QPainter& p, const QRectF& r) { drawPlusMinus(p, r, true); });
    zoomOutButton = new BarButton(q, "zoomOutButton", "Zoom Out Time (Down)", [this] { zoomBy(1.0 / 1.5); }, [](QPainter& p, const QRectF& r) { drawPlusMinus(p, r, false); });
    zoomToolButton = new BarButton(q, "zoomToolButton", "Zoom Tool: drag across the events to zoom to that stretch", [this] {
        opt.tool = EditTool::Zoom;
        q->optionsChanged();
    }, [](QPainter& p, const QRectF& r) { drawMagnifier(p, r); });
    tallerButton = new BarButton(q, "tallerTracksButton", "Increase Track Height (Ctrl+Shift+Up)", [this] { changeTrackHeights(1); },
                                 [](QPainter& p, const QRectF& r) { drawPlusMinus(p, r, true); });
    shorterButton = new BarButton(q, "shorterTracksButton", "Decrease Track Height (Ctrl+Shift+Down)", [this] { changeTrackHeights(-1); },
                                  [](QPainter& p, const QRectF& r) { drawPlusMinus(p, r, false); });
    markerToolButton = new BarButton(q, "markerToolButton", "Marker Tool: choose markers in the marker bar (Ctrl+click, Shift+click, Left / Right), "
                                                           "drag them together, Delete deletes them", [this] { setMarkerTool(!markerTool); },
                                     [this](QPainter& p, const QRectF& r) {
                                         p.save();
                                         p.setRenderHint(QPainter::Antialiasing, true);
                                         if (markerTool) p.fillRect(r.adjusted(1, 1, -1, -1), QColor(255, 176, 60, 110));
                                         p.setPen(QPen(QColor(230, 230, 234), 1.2));
                                         const double x = r.left() + r.width() * 0.35;
                                         p.drawLine(QPointF(x, r.top() + 3.0), QPointF(x, r.bottom() - 3.0));
                                         p.setPen(Qt::NoPen);
                                         p.setBrush(markerTool ? QColor(255, 176, 60) : QColor(230, 150, 60));
                                         p.drawRect(QRectF(x, r.top() + 3.0, r.width() * 0.4, r.height() * 0.32));
                                         p.restore();
                                     });
    markerToolButton->setAutoRepeat(false);
    QObject::connect(hbar, &QScrollBar::valueChanged, q, [this](int v) {
        const double was = viewStart;
        viewStart = (double)v / kHBarRange * hbarSpan;
        viewMoved(was, scrollY);
    });
    QObject::connect(hbar, &QScrollBar::sliderReleased, q, [this] { updateBars(); });
    QObject::connect(vbar, &QScrollBar::valueChanged, q, [this](int v) {
        const int was = scrollY;
        scrollY = v;
        placeNameEdit();
        viewMoved(viewStart, was);
    });

    nameEdit = new NameEdit(q);
    nameEdit->hide();
    nameEdit->setMaxLength(60);
    nameEdit->onCancel = [this] { endRename(false); };
    QObject::connect(nameEdit, &QLineEdit::editingFinished, q, [this] { endRename(true); });

    autoScroll = new QTimer(q);
    autoScroll->setInterval(40);
    QObject::connect(autoScroll, &QTimer::timeout, q, [this] { autoScrollTick(); });
    recTimer = new QTimer(q);
    recTimer->setInterval(25);
    QObject::connect(recTimer, &QTimer::timeout, q, [this] { recTick(); });

    Filter* f = new Filter(q);
    f->fn = [this](QEvent* e) {
        if (e->type() != QEvent::ToolTip) return false;
        QHelpEvent* he = static_cast<QHelpEvent*>(e);
        const QString text = tipAt(QPointF(he->pos()));
        if (text.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(he->globalPos(), text, q);
        return true;
    };
    q->installEventFilter(f);

    updateFonts();
    loader.reset(new Loader(app.mediaOpener, [this] { loaderReady(); }));
    loadOptions();
    listener = app.addListener([this](int changes) { changed(changes); });
    layout();
    updateBars();
}

TimelineWidget::Impl::~Impl()
{
    app.removeListener(listener);
    loader.reset();
}


TimelineWidget::TimelineWidget(Playback* playback, QWidget* parent) : QWidget(parent), d(new Impl(this, playback))
{
    QSettings st;
    d->timeText = QColor(st.value("timeline/timeDisplayText").toString());
    d->timeBack = QColor(st.value("timeline/timeDisplayBackground").toString());
}

TimelineWidget::~TimelineWidget()
{
    QObject::disconnect(d->hbar, nullptr, this, nullptr);
    QObject::disconnect(d->vbar, nullptr, this, nullptr);
    QObject::disconnect(d->nameEdit, nullptr, this, nullptr);
    QObject::disconnect(d->autoScroll, nullptr, this, nullptr);
    d->nameEdit->onCancel = nullptr;
    for (QObject* o : children())
        if (Filter* f = dynamic_cast<Filter*>(o)) {
            removeEventFilter(f);
            f->fn = nullptr;
        }
    d.reset();
}

TimelineOptions& TimelineWidget::options() { return d->opt; }

void TimelineWidget::optionsChanged()
{
    if (d->opt.tool != EditTool::Normal) d->cancelMouseDrag();
    d->saveOptions();
    d->layout();
    d->updateBars();
    update();
    if (onOptionsChanged) onOptionsChanged();
}

void TimelineWidget::zoomIn() { d->zoomBy(1.5); }
void TimelineWidget::zoomOut() { d->zoomBy(1.0 / 1.5); }
void TimelineWidget::zoomToFit() { d->zoomToFit(); }
void TimelineWidget::zoomLarge(bool in) { d->zoomLarge(in); }
void TimelineWidget::scrollToCursor() { d->scrollToCursor(); }
void TimelineWidget::centerOnCursor() { d->centerOnCursor(); }
double TimelineWidget::viewStartTime() const { return d->viewStart; }
double TimelineWidget::viewEndTime() const { return d->timeAtF(d->lanesRight()); }

void TimelineWidget::deleteSelection() { d->removeSelection(false); }
void TimelineWidget::cutSelection() { d->removeSelection(true); }
void TimelineWidget::pasteInsert() { d->pasteInsert(); }
void TimelineWidget::trimStartToCursor() { d->trimStart(); }
void TimelineWidget::trimEndToCursor() { d->trimEnd(); }
void TimelineWidget::trimToTimeSelection() { d->trimToSelection(); }
void TimelineWidget::nudgeSelection(int frames, int pixels, int tracks) { d->nudge(frames, pixels, tracks); }
void TimelineWidget::insertMarker() { d->insertMarker(); }
void TimelineWidget::insertRegion() { d->insertRegion(); }
void TimelineWidget::goToMarker(int number) { d->goToMarker(number); }
void TimelineWidget::cursorToMarker(int dir) { d->cursorToEdge(dir, Impl::EdgeKind::Markers); }
void TimelineWidget::cursorToEventEdge(int dir) { d->cursorToEdge(dir, Impl::EdgeKind::Events); }
void TimelineWidget::cursorToEditPoint(int dir) { d->cursorToEdge(dir, Impl::EdgeKind::EditPoints); }
void TimelineWidget::cursorToSelectedEdge(int dir)
{
    d->cursorToEdge(dir, Impl::EdgeKind::Selected);
    if (d->edgeValid() && !d->edgeSel.expanded) {
        const AppState& app = AppState::get();
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events)
                if (app.selection().count(e.id))
                    for (int side : {-1, 1})
                        if (std::fabs((side < 0 ? e.start : e.end()) - app.cursor()) < 1e-9) {
                            d->edgeSel.event = e.id;
                            d->edgeSel.side = side;
                            update();
                            return;
                        }
    }
}
void TimelineWidget::timeSelectSelectedEvent(int dir) { d->timeSelectSelected(dir); }
void TimelineWidget::renameSelected() { d->renameSelected(); }
void TimelineWidget::changeTrackHeights(int dir) { d->changeTrackHeights(dir); }
void TimelineWidget::toggleMinimizeTrack() { d->toggleMinimized(AppState::get().currentTrack); }
void TimelineWidget::toggleMaximizeTrack() { d->toggleMaximized(AppState::get().currentTrack); }

void TimelineWidget::resetTrackHeights()
{
    d->heightScale = 1.0;
    d->minimized.clear();
    d->maximized.clear();
    AppState& app = AppState::get();
    bool own = false;
    for (const Track& t : app.project().tracks) own = own || t.height > 0;
    if (own) app.edit("Default Track Heights", [](Project& p) { for (Track& t : p.tracks) t.height = 0; });
    d->layout();
    d->updateBars();
    update();
}

void TimelineWidget::zoomToFrames()
{
    int h = defaultVideoH();
    for (const Impl::Row& r : d->rows) if (r.video) h = std::min(h, r.h);
    const double tileW = std::max(8.0, (h - 2.0) * 16.0 / 9.0);
    const double x = clampd(d->xAtF(AppState::get().cursor()), d->headerW, d->lanesRight());
    d->setZoom(tileW / d->frame(), AppState::get().cursor(), x);
}
bool TimelineWidget::cancelMouseDrag() { return d->cancelMouseDrag(); }
void TimelineWidget::toggleTrackEnvelope(const std::string& kind) { d->toggleTrackEnvelope(kind); }
void TimelineWidget::removeTrackEnvelope(const std::string& kind) { d->removeTrackEnvelope(kind); }
void TimelineWidget::cycleEnvelope(int dir) { d->cycleEnvelope(dir); }
void TimelineWidget::fillEnvelopeMenu(QMenu* menu, bool video) { d->fillEnvelopeMenu(menu, video); }
EnvelopeRef TimelineWidget::selectedEnvelope() const { return d->selEnv; }

std::vector<int> TimelineWidget::selectedEnvelopePoints() const
{
    d->validateEnvSelection();
    return std::vector<int>(d->selPoints.begin(), d->selPoints.end());
}

QPoint TimelineWidget::envelopePointPos(const EnvelopeRef& ref, int index) const
{
    Impl::EnvLane L;
    if (!d->envLaneOf(ref, &L) || index < 0 || index >= (int)L.env->points.size()) return QPoint(-1, -1);
    return d->envPointPos(L, (size_t)index).toPoint();
}

QRect TimelineWidget::busTrackRect(const EnvelopeOwner& owner) const
{
    const Impl::BusRow* b = d->busRowOf(owner);
    return b ? QRect(d->headerW, d->busTop(*b), d->viewW(), b->h) : QRect();
}
void TimelineWidget::clearGroups() { d->clearGroups(); }

void TimelineWidget::paste()
{
    AppState& app = AppState::get();
    if (!app.hasClipboard()) return;
    const std::set<uint64_t> before = app.selection();
    const uint64_t rev = app.revision();
    const double at = d->quantized(app.cursor());
    app.pasteAtCursor();
    if (app.revision() == rev) return;
    std::set<uint64_t> tracks;
    double last = at;
    for (const Track& t : app.project().tracks)
        for (const Event& e : t.events)
            if (app.selection().count(e.id) && !before.count(e.id)) {
                tracks.insert(t.id);
                last = std::max(last, e.end());
            }
    if (!tracks.empty()) d->rememberEdit(tracks, at, last - at, app.selection());
}

bool TimelineWidget::copyEnvelopePoints() { return d->copyEnvelopePoints(); }
bool TimelineWidget::pasteEnvelopePoints() { return d->pasteEnvelopePoints(); }
bool TimelineWidget::hasEnvelopeClip() const { return d->opt.tool == EditTool::Envelope && !d->envClip.points.empty(); }
void TimelineWidget::selectEdge(int dir) { d->selectEdge(dir); }
void TimelineWidget::toggleExpandedEdit() { d->toggleExpandedEdit(); }
void TimelineWidget::exitEditMode() { d->exitEditMode(); }
bool TimelineWidget::edgeTrimming() const { return d->edgeValid(); }
uint64_t TimelineWidget::trimEvent() const { return d->edgeValid() ? d->edgeSel.event : 0; }
int TimelineWidget::trimSide() const { return !d->edgeValid() ? 0 : d->edgeSel.expanded ? 0 : d->edgeSel.side; }
void TimelineWidget::keypadEdit(int frames, int pixels, Qt::KeyboardModifiers mods) { d->keypadEdit(frames, pixels, mods); }
void TimelineWidget::jog(int dir) { d->jog(dir); }
void TimelineWidget::smartSplit() { d->smartSplit(); }
void TimelineWidget::convertCutToTransition(int kind) { d->convertCutToTransition(kind); }
void TimelineWidget::convertTransitionToCut() { d->convertTransitionToCut(); }
void TimelineWidget::postEditRipple(RippleMode mode) { d->postEditRipple(mode); }
void TimelineWidget::selectLoopRegion() { d->selectLoopRegion(); }
void TimelineWidget::setLocked(bool locked) { d->setLocked(locked); }
void TimelineWidget::insertCdTrack() { d->insertCdTrack(); }
void TimelineWidget::insertCdIndex() { d->insertCdIndex(); }
void TimelineWidget::cursorToCd(int dir, bool tracks) { d->cursorToCd(dir, tracks); }
void TimelineWidget::insertCommandMarker(const std::string& command, const std::string& parameter) { d->insertCommandMarker(command, parameter); }
void TimelineWidget::insertCommandMarker(const std::string& command, const std::string& parameter, const std::string& comment, double at)
{
    AppState& app = AppState::get();
    app.edit("Insert Command Marker", [&](Project& p) {
        Project::Marker m;
        m.id = p.newId();
        m.time = std::max(0.0, d->quantized(at));
        m.kind = "command";
        m.command = command;
        m.name = parameter;
        m.comment = comment;
        p.markers.push_back(m);
    });
}
void TimelineWidget::editCommandMarker(uint64_t markerId) { d->editCommandMarker(markerId); }
void TimelineWidget::rebuildPeaks() { d->rebuildPeaks(); }
void TimelineWidget::addTakesToSelected(const std::vector<uint64_t>& media) { d->addTakesToSelected(media); }
void TimelineWidget::toggleTrackList() { d->toggleTrackList(); }
bool TimelineWidget::trackListFocused() const { return d->trackListFocus && hasFocus(); }

void TimelineWidget::setTrackListFocus(bool on)
{
    d->trackListFocus = on;
    if (on) setFocus(Qt::TabFocusReason);
    update();
}

void TimelineWidget::changeTrackLevel(int dir, bool pan) { d->changeTrackLevel(dir, pan); }
std::set<uint64_t> TimelineWidget::selectedTracks() const { return d->selectedTracks(); }

void TimelineWidget::setSelectedTracks(const std::set<uint64_t>& ids)
{
    d->selTracks = ids;
    update();
}

void TimelineWidget::groupTracks(bool group) { d->groupTracks(group); }
void TimelineWidget::collapseTrackGroup(bool collapse) { d->collapseTrackGroup(collapse); }
void TimelineWidget::muteSoloTrackGroup(bool solo, bool exclusive) { d->muteSoloTrackGroup(solo, exclusive); }
void TimelineWidget::createMulticamTrack() { d->createMulticamTrack(); }
bool TimelineWidget::multicamCut(int take, bool crossfade) { return d->multicamCut(take, crossfade); }

bool TimelineWidget::focusNextPrevChild(bool next)
{
    if ((d->nameEdit && d->nameEdit->hasFocus()) || !isVisible()) return QWidget::focusNextPrevChild(next);
    d->trackListFocus = !d->trackListFocus;
    AppState::get().status(d->trackListFocus ? "The track list has the keyboard (Ctrl+Left / Right: volume, Ctrl+Shift+Left / Right: pan; Tab: back)."
                                             : "The timeline has the keyboard.");
    update();
    return true;
}
void TimelineWidget::selectGroups() { d->selectGroups(); }

double TimelineWidget::timeAt(int x) const { return d->timeAtF(x); }
int TimelineWidget::xAt(double seconds) const { return (int)std::lround(clampd(d->xAtF(seconds), -1.0e9, 1.0e9)); }

QRect TimelineWidget::trackRect(uint64_t trackId) const
{
    const Impl::Row* r = d->rowOf(trackId);
    return r ? QRect(d->headerW, d->rowTop(*r), d->viewW(), r->h) : QRect();
}

QRect TimelineWidget::headerRect(uint64_t trackId) const
{
    const Impl::Row* r = d->rowOf(trackId);
    return r ? QRect(0, d->rowTop(*r), d->headerW, r->h) : QRect();
}

QRect TimelineWidget::headerPartRect(uint64_t trackId, HeaderPart part) const
{
    const Impl::Row* r = d->rowOf(trackId);
    if (!r) return QRect();
    const Impl::HeaderRects h = d->headerRects(*r);
    switch (part) {
    case PartMenu: return h.menu;
    case PartName: return h.name;
    case PartMotion: return h.motion;
    case PartFx: return h.fx;
    case PartMute: return h.mute;
    case PartSolo: return h.solo;
    case PartArm: return h.arm;
    case PartLevel: return h.level;
    case PartPan: return h.pan;
    case PartSurround: return h.surround;
    case PartAuto: return h.autoBtn;
    case PartFade: return h.fade;
    case PartPhase: return h.phase;
    case PartBus: return h.bus;
    }
    return QRect();
}

QRect TimelineWidget::eventRectOf(uint64_t eventId) const
{
    const Track* tr = nullptr;
    const Event* e = AppState::get().project().findEvent(eventId, &tr);
    const Impl::Row* r = (e && tr) ? d->rowOf(tr->id) : nullptr;
    return r ? d->eventRect(*r, *e).toAlignedRect() : QRect();
}

QRect TimelineWidget::eventButtonRect(uint64_t eventId, bool panCrop) const
{
    const Track* tr = nullptr;
    const Event* e = AppState::get().project().findEvent(eventId, &tr);
    const Impl::Row* r = (e && tr) ? d->rowOf(tr->id) : nullptr;
    if (!r) return QRect();
    const QRectF er = d->eventRect(*r, *e);
    if (panCrop) return d->hasCropButton(er, r->video) ? d->cropButtonRect(er).toAlignedRect() : QRect();
    return d->hasFxButton(er) ? d->fxButtonRect(er).toAlignedRect() : QRect();
}

QRect TimelineWidget::eventMenuRect(uint64_t eventId) const
{
    const Track* tr = nullptr;
    const Event* e = AppState::get().project().findEvent(eventId, &tr);
    const Impl::Row* r = (e && tr) ? d->rowOf(tr->id) : nullptr;
    if (!r) return QRect();
    const QRectF er = d->eventRect(*r, *e);
    return d->hasMenuButton(er) ? d->menuButtonRect(er).toAlignedRect() : QRect();
}

int TimelineWidget::headerWidth() const { return d->headerW; }
void TimelineWidget::setHeaderWidth(int width) { d->setHeaderWidth(width); }

QRect TimelineWidget::markerTagRect(uint64_t markerId) const
{
    const int k = d->markerIndex(markerId);
    if (k < 0) return QRect();
    return d->markerTag(k, AppState::get().project().markers[(size_t)k]).toAlignedRect();
}

QString TimelineWidget::loopLengthLabel() const
{
    const AppState& app = AppState::get();
    return app.loopEnd > app.loopStart ? d->selectionLengthText(app.loopEnd - app.loopStart) : QString();
}
QRect TimelineWidget::loopBarRect() const { return QRect(d->headerW, kLoopTop, d->viewW(), kLoopH); }
QRect TimelineWidget::markerBarRect() const { return QRect(d->headerW, kMarkerTop, d->viewW(), kMarkerH); }
QRect TimelineWidget::rulerRect() const { return QRect(d->headerW, kRulerTop, d->viewW(), kRulerH); }
QRect TimelineWidget::timeDisplayRect() const { return QRect(0, 0, d->headerW, kTopH); }

std::string rulerTime(const Project& p, double seconds)
{
    return formatTime(p.settings, seconds + p.settings.rulerStart, p.settings.rulerFormat);
}

bool parseRulerTime(const Project& p, const QString& text, double* seconds)
{
    double shown = 0;
    if (!parseTimeText(p.settings, text.toStdString(), p.settings.rulerFormat, &shown)) return false;
    *seconds = shown - p.settings.rulerStart;
    return true;
}

namespace {
class InPlaceEdit : public QLineEdit {
public:
    std::function<void(const QString&)> done;
    using QLineEdit::QLineEdit;
    void finish(bool keep)
    {
        if (finished) return;
        finished = true;
        const bool hadFocus = hasFocus() || (window() && window()->focusWidget() == this);
        if (hadFocus && before && before != this) before->setFocus(Qt::OtherFocusReason);
        hide();
        QTimer::singleShot(0, this, [this, keep] {
            if (keep && done) done(text());
            deleteLater();
        });
    }
protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) { finish(false); return; }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) { finish(true); return; }
        QLineEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        QLineEdit::focusOutEvent(e);
        if (e->reason() != Qt::PopupFocusReason) finish(false);
    }
public:
    QPointer<QWidget> before;
private:
    bool finished = false;
};
}  // namespace

void editTextInPlace(QWidget* parent, const QRect& r, const QString& text, std::function<void(const QString&)> done)
{
    InPlaceEdit* e = new InPlaceEdit(parent);
    e->setObjectName("inPlaceEdit");
    e->before = QApplication::focusWidget() ? QApplication::focusWidget() : parent->window()->focusWidget();
    e->done = std::move(done);
    e->setText(text);
    e->setGeometry(r);
    e->show();
    e->raise();
    e->setFocus(Qt::OtherFocusReason);
    e->selectAll();
}

void TimelineWidget::setTimeAtCursor()
{
    AppState& app = AppState::get();
    // Audio CD time always starts at zero: VEGAS allows no offset there
    if (app.project().settings.rulerFormat == "cdtime") return;
    const int x = (int)std::lround(d->xAtF(app.cursor()));
    QRect r(x - 2, kRulerTop - 4, 120, kRulerH + 4);
    if (x < d->headerW || x > d->lanesRight() - 20) r = QRect(4, 2, d->headerW - kHeaderGap - 8, kTopH - 4);
    else if (r.right() > d->lanesRight()) r.moveRight(d->lanesRight());
    editTextInPlace(this, r, QString::fromStdString(rulerTime(app.project(), app.cursor())), [](const QString& t) {
        AppState& a = AppState::get();
        double shown = 0;
        const ProjectSettings& s = a.project().settings;
        if (!parseTimeText(s, t.toStdString(), s.rulerFormat, &shown)) {
            a.status("Set Time at Cursor: \"" + t.toStdString() + "\" is not a time.");
            return;
        }
        const double start = shown - a.cursor();
        if (std::fabs(start - s.rulerStart) > 1e-9) a.edit("Set Time at Cursor", [&](Project& p) { p.settings.rulerStart = start; });
    });
}

void TimelineWidget::update()
{
    d->layerOther = true;
    d->layerValid = false;
    QWidget::update();
}
void TimelineWidget::update(const QRect& r)
{
    d->layerOther = true;
    d->layerValid = false;
    QWidget::update(r);
}
void TimelineWidget::update(const QRegion& r)
{
    d->layerOther = true;
    d->layerValid = false;
    QWidget::update(r);
}
void TimelineWidget::update(int x, int y, int w, int h) { update(QRect(x, y, w, h)); }
void TimelineWidget::scroll(int dx, int dy)
{
    d->layerOther = true;
    d->layerValid = false;
    QWidget::scroll(dx, dy);
}
void TimelineWidget::scroll(int dx, int dy, const QRect& r)
{
    d->layerOther = true;
    d->layerValid = false;
    QWidget::scroll(dx, dy, r);
}
int TimelineWidget::layerPaints() const { return d->layerPaints; }
void TimelineWidget::updateCursor(const QRegion& r)
{
    d->cursorAsked |= r;
    QWidget::update(r);
}

// Whether the cached picture can be used: whole pixels (no fractional screen scaling), headers that don't follow the
// cursor, and a size that isn't huge.
bool TimelineWidget::Impl::layerUsable() const
{
    static const bool off = std::getenv("RENO_NO_TIMELINE_LAYER") != nullptr;
    if (off) return false;
    const qreal dpr = q->devicePixelRatioF();
    if (dpr != std::floor(dpr) || dpr < 1.0 || dpr > 4.0) return false;
    if ((double)q->width() * q->height() * dpr * dpr > 24e6) return false;
    return !headersFollowCursor() && q->isVisible();
}

void TimelineWidget::Impl::buildLayer()
{
    const qreal dpr = q->devicePixelRatioF();
    QImage img(QSize((int)std::lround(q->width() * dpr), (int)std::lround(q->height() * dpr)), QImage::Format_RGB32);
    if (img.isNull()) return;
    img.setDevicePixelRatio(dpr);
    img.fill(QColor(0, 0, 0));
    {
        QPainter lp(&img);
        const QRect all = q->rect();
        ++paintCount;
        if (shownTheme != uiTheme()) themeChanged();
        updateFonts();
        ensureIndex(true);
        paintLanes(lp, all);
        paintHeaders(lp, all);
        paintRuler(lp, all);
    }
    layer = img;
    layerValid = true;
}

void TimelineWidget::Impl::paintFromLayer(QPainter& p, const QRegion& region)
{
    const qreal dpr = layer.devicePixelRatio();
    for (const QRect& r : region) {
        const QRect rr = r.intersected(q->rect());
        if (rr.isEmpty()) continue;
        p.save();
        p.setClipRect(rr);
        p.drawImage(rr.topLeft(), layer, QRect((int)std::lround(rr.x() * dpr), (int)std::lround(rr.y() * dpr), (int)std::lround(rr.width() * dpr), (int)std::lround(rr.height() * dpr)));
        paintCursor(p, rr);
        paintCorners(p, rr);
        p.restore();
    }
    paintedCursor = app.cursor();
    paintedLoopA = app.loopStart;
    paintedLoopB = app.loopEnd;
    paintedLoopOn = app.loopOn;
}

void TimelineWidget::paintEvent(QPaintEvent* e)
{
    QPainter p(this);
    {
        const bool cursorOnly = !d->layerOther && !d->cursorAsked.isEmpty() && (e->region() - d->cursorAsked).isEmpty();
        d->layerOther = false;
        d->cursorAsked = QRegion();
        if (cursorOnly && d->layerUsable()) {
            if (!d->layerValid && ++d->layerTries >= 3) d->buildLayer();
            if (d->layerValid) {
                ++d->layerPaints;
                d->paintFromLayer(p, e->region());
                return;
            }
        } else {
            d->layerValid = false;
            if (!cursorOnly) d->layerTries = 0;
        }
    }
    // What needs repainting is often a few small pieces far apart (the cursor's old and new place, the time display; an
    // event and a header): each is painted on its own rather than everything between them (the bounding rectangle).
    const QRegion& region = e->region();
    const QRect bounds = region.boundingRect();
    if (region.rectCount() > 1 && region.rectCount() <= 24) {
        double area = 0;
        for (const QRect& r : region) area += (double)r.width() * r.height();
        if (area < 0.6 * (double)bounds.width() * bounds.height()) {
            for (const QRect& r : region) {
                // a pixel more all round: with fractional screen scaling (125%, 150%) a piece's edge falls inside a
                // device pixel; the paint event's clip trims the excess and painting a pixel twice gives the same
                // color
                const QRect rr = r.adjusted(-1, -1, 1, 1);
                p.save();
                p.setClipRect(rr);
                d->paint(p, rr);
                p.restore();
            }
            return;
        }
    }
    d->paint(p, bounds.adjusted(-1, -1, 1, 1));
}

void TimelineWidget::resizeEvent(QResizeEvent*)
{
    if (!d->maximized.empty()) d->layout();
    d->updateBars();
    if (d->fitWanted) d->zoomToFit();
}

void TimelineWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton) {
        d->beginWheelScroll(e->position());
        return;
    }
    d->press(e);
    d->overlaysChanged();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (d->wheelScrolling) {
        d->wheelScrollTo = e->position();
        return;
    }
    d->move(e);
    d->overlaysChanged();
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() == Qt::MiddleButton) {
        d->endWheelScroll();
        return;
    }
    d->release(e);
    d->overlaysChanged();
}
void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    d->doubleClick(e);
    d->overlaysChanged();
}
void TimelineWidget::wheelEvent(QWheelEvent* e) { d->wheel(e); }

static bool isModifierKey(int k) { return k == Qt::Key_Control || k == Qt::Key_Alt || k == Qt::Key_Shift || k == Qt::Key_Meta; }

void TimelineWidget::keyReleaseEvent(QKeyEvent* e)
{
    if (isModifierKey(e->key()) && d->drag.kind == Drag::None) d->setPointer(d->hover, e->modifiers());
    QWidget::keyReleaseEvent(e);
}

void TimelineWidget::keyPressEvent(QKeyEvent* e)
{
    if (isModifierKey(e->key()) && d->drag.kind == Drag::None) d->setPointer(d->hover, e->modifiers());
    if (d->key(e)) e->accept();
    else QWidget::keyPressEvent(e);
    d->overlaysChanged();
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e) { d->contextMenu(e); }

void TimelineWidget::buildTrackHamburger(QMenu* menu, uint64_t trackId) { if (menu) d->trackHamburger(*menu, trackId); }
void TimelineWidget::buildEventHamburger(QMenu* menu, uint64_t eventId) { if (menu) d->eventHamburger(*menu, eventId); }
void TimelineWidget::editVisibleButtons(bool video, const std::function<void(QDialog*)>& run)
{
    d->visibleButtonsHook = run;
    d->editVisibleButtons(video);
    d->visibleButtonsHook = nullptr;
}
bool TimelineWidget::headerButtonHidden(bool video, const QString& key) const { return d->hiddenButtons[video ? 1 : 0].count(key) != 0; }
QRect TimelineWidget::eventHeaderRect(uint64_t eventId) const
{
    const QRect r = eventRectOf(eventId);
    const AppState& app = AppState::get();
    const Impl::Row* row = nullptr;
    const Track* tr = nullptr;
    if (app.project().findEvent(eventId, &tr) && tr) row = d->rowOf(tr->id);
    if (!row) return QRect();
    return QRect(r.left(), r.top(), r.width(), (int)std::lround(d->eventTitleH(QRectF(r))));
}
bool TimelineWidget::trackLayersExpanded(uint64_t trackId) const { const Impl::Row* r = d->rowOf(trackId); return r && r->lm; }
int TimelineWidget::eventLayer(uint64_t eventId) const
{
    const Track* tr = nullptr;
    if (!AppState::get().project().findEvent(eventId, &tr) || !tr) return -1;
    const Impl::Row* r = d->rowOf(tr->id);
    if (!r || !r->lm) return -1;
    const auto it = r->lm->of.find(eventId);
    return it == r->lm->of.end() ? 0 : it->second;
}

bool TimelineWidget::buildContextMenu(QMenu* menu, const QPoint& pos)
{
    if (!menu) return false;
    return d->fillMenu(*menu, d->hitTest(QPointF(pos)), d->timeAtF(pos.x()));
}

void TimelineWidget::selectEventsToEnd()
{
    AppState& app = AppState::get();
    if (app.selection().empty()) return;
    app.setSelection(app.eventsToEnd(app.selection()));
}

void TimelineWidget::selectAllAfterCursor(uint64_t onlyTrack)
{
    AppState& app = AppState::get();
    const std::set<uint64_t> tracks = d->selectedTracks();
    if (!onlyTrack && tracks.size() > 1) {
        QMessageBox box(this);
        box.setObjectName("selectAfterCursorQuestion");
        box.setWindowTitle("Select All After Cursor");
        box.setText("Select the events after the cursor on all the tracks, or only on the selected tracks?");
        QPushButton* all = box.addButton("&All Tracks", QMessageBox::AcceptRole);
        QPushButton* sel = box.addButton("&Selected Tracks", QMessageBox::AcceptRole);
        box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(all);
        box.exec();
        if (box.clickedButton() == sel) {
            std::set<uint64_t> out;
            for (uint64_t t : tracks)
                for (uint64_t e : app.eventsAfter(app.cursor(), t)) out.insert(e);
            app.setSelection(out);
            return;
        }
        if (box.clickedButton() != all) return;
    }
    app.setSelection(app.eventsAfter(app.cursor(), onlyTrack));
}

void TimelineWidget::setSelectionToView()
{
    AppState::get().setLoop(std::max(0.0, viewStartTime()), viewEndTime());
}

void TimelineWidget::setSelectionToProject()
{
    AppState& app = AppState::get();
    const double len = app.project().length();
    if (len > 0) app.setLoop(0.0, len);
}

void TimelineWidget::minimizeAllTracks()
{
    const Project& p = AppState::get().project();
    bool all = !p.tracks.empty();
    for (const Track& t : p.tracks) all = all && d->minimized.count(t.id) != 0;
    d->minimized.clear();
    d->maximized.clear();
    if (!all)
        for (const Track& t : p.tracks) d->minimized.insert(t.id);
    d->layout();
    d->updateBars();
    update();
}

void TimelineWidget::insertEmptyEvent()
{
    AppState& app = AppState::get();
    const Track* t = nullptr;
    for (const Track& x : app.project().tracks)
        if (x.id == app.currentTrack) t = &x;
    if (!t) { app.status("Insert Empty Event: select a track first."); return; }
    if (!(app.loopEnd > app.loopStart)) { app.status("Insert Empty Event: make a time selection for it first."); return; }
    app.insertEmptyEvent(t->id, app.loopStart, app.loopEnd - app.loopStart);
}

void TimelineWidget::insertAdjustmentEvent()
{
    AppState& app = AppState::get();
    const Track* t = nullptr;
    for (const Track& x : app.project().tracks)
        if (x.id == app.currentTrack) t = &x;
    if (!t || t->type != TrackType::Video) { app.status("Insert Adjustment Event: select a video track first (an adjustment event works on the tracks below it)."); return; }
    double start = app.cursor(), length = std::max(0.1, app.prefs.stillLength);
    if (app.loopEnd > app.loopStart) { start = app.loopStart; length = app.loopEnd - app.loopStart; }
    app.insertEmptyEvent(t->id, start, length, true);
}

void TimelineWidget::fadeColorDialog(uint64_t trackId, bool top)
{
    EnvelopeOwner o;
    if (trackId) {
        o.kind = EnvelopeOwner::Track;
        o.id = trackId;
    } else {
        o.kind = EnvelopeOwner::VideoOut;
    }
    d->pickFadeColor(o, top);
}

void TimelineWidget::duplicateTrack(uint64_t trackId)
{
    AppState& app = AppState::get();
    if (const uint64_t made = app.duplicateTrack(trackId)) {
        app.currentTrack = made;
        app.notify(kSelection);
    }
}
void TimelineWidget::dragEnterEvent(QDragEnterEvent* e) { d->dragEnter(e); }
void TimelineWidget::dragMoveEvent(QDragMoveEvent* e) { d->dragMove(e); }
void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*) { d->dragLeave(); }
void TimelineWidget::dropEvent(QDropEvent* e) { d->dropped(e); }

void TimelineWidget::leaveEvent(QEvent*)
{
    if (d->hover.part != Part::None) {
        update(d->hotRegion(d->hover));
        d->hover = Hit();
    }
}

}  // namespace app
}  // namespace reno
