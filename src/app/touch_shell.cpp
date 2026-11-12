#include "touch_shell_internal.h"
#include "panels.h"
#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPropertyAnimation>
#include <QScreen>
#include <QScrollBar>
#include <QScroller>
#include <QScrollerProperties>
#include <QSettings>
#include <QVBoxLayout>
#include <cmath>
#include <set>

namespace reno {
namespace app {

using namespace touch;

bool touchInterfaceWanted()
{
    if (qEnvironmentVariableIsSet("RENO_TOUCH")) return qEnvironmentVariable("RENO_TOUCH") != "0";
#if defined(Q_OS_ANDROID) || defined(Q_OS_IOS)
    return QSettings().value("touch/enabled", true).toBool();
#else
    return QSettings().value("touch/enabled", false).toBool();
#endif
}

void setTouchInterfaceWanted(bool on) { QSettings().setValue("touch/enabled", on); }

namespace {

// A plain filled area.
class Fill : public QWidget {
public:
    Fill(const QColor& c, QWidget* parent) : QWidget(parent), c_(c) { setAttribute(Qt::WA_OpaquePaintEvent, true); }
protected:
    void paintEvent(QPaintEvent* e) override { QPainter(this).fillRect(e->rect(), c_); }
private:
    QColor c_;
};

// Over everything but an open sheet: a tap here closes the sheet (and goes no further).
class Backdrop : public QWidget {
public:
    std::function<void()> onTap;
    explicit Backdrop(QWidget* parent) : QWidget(parent) { setAttribute(Qt::WA_NoSystemBackground, true); }
protected:
    void paintEvent(QPaintEvent*) override { QPainter(this).fillRect(rect(), QColor(0, 0, 0, 90)); }
    void mousePressEvent(QMouseEvent* e) override
    {
        e->accept();
        if (onTap) onTap();
    }
};

// Android's back button (and Escape on a keyboard): closes the sheet, or leaves the full-size preview.
class BackKey : public QObject {
public:
    std::function<bool()> onBack;
    using QObject::QObject;
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() != QEvent::KeyPress) return false;
        const int key = static_cast<QKeyEvent*>(e)->key();
        if (key != Qt::Key_Back && key != Qt::Key_Escape) return false;
        return onBack && onBack();
    }
};

// A tap on the picture plays or pauses (unless an effect's on-screen controls are there to be dragged).
class PictureTap : public QObject {
public:
    std::function<bool()> allowed;
    std::function<void()> onTap;
    using QObject::QObject;
    bool eventFilter(QObject*, QEvent* e) override
    {
        if (e->type() == QEvent::MouseButtonPress) {
            const auto* me = static_cast<QMouseEvent*>(e);
            at_ = me->globalPosition();
            clock_.start();
        } else if (e->type() == QEvent::MouseButtonRelease && clock_.isValid()) {
            const auto* me = static_cast<QMouseEvent*>(e);
            const QPointF d = me->globalPosition() - at_;
            const bool tap = clock_.elapsed() < 350 && std::hypot(d.x(), d.y()) < 12;
            clock_.invalidate();
            if (tap && (!allowed || allowed()) && onTap) onTap();
        }
        return false;
    }

private:
    QPointF at_;
    QElapsedTimer clock_;
};

// A short message over the bottom of the screen that goes away by itself (what the desktop shows in its status bar).
class Toast : public QWidget {
public:
    explicit Toast(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        hide();
        timer_.setSingleShot(true);
        QObject::connect(&timer_, &QTimer::timeout, this, [this] { hide(); });
    }
    void say(const QString& text, int bottom)
    {
        text_ = text;
        const QFont f = uiFont(14);
        const int maxW = std::max(160, parentWidget()->width() - 48);
        const QRect br = QFontMetrics(f).boundingRect(QRect(0, 0, maxW - 32, 400), Qt::TextWordWrap, text_);
        const int w = std::min(maxW, br.width() + 32), h = br.height() + 20;
        setGeometry((parentWidget()->width() - w) / 2, bottom - h - 12, w, h);
        show();
        raise();
        update();
        timer_.start(std::min(6000, 1800 + (int)text_.size() * 40));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(52, 52, 58, 245));
        p.drawRoundedRect(rect(), 12, 12);
        p.setPen(color::text);
        p.setFont(uiFont(14));
        p.drawText(rect().adjusted(16, 10, -16, -10), Qt::AlignCenter | Qt::TextWordWrap, text_);
    }

private:
    QString text_;
    QTimer timer_;
};

QString clockText(double t, double fps)
{
    t = std::max(0.0, t);
    const int64_t frames = (int64_t)std::floor(t * fps + 1e-6);
    const int fpsI = std::max(1, (int)std::lround(fps));
    const int64_t secs = frames / fpsI;
    const int f = (int)(frames % fpsI);
    const int m = (int)(secs / 60), s = (int)(secs % 60);
    if (m >= 60) return QString("%1:%2:%3.%4").arg(m / 60).arg(m % 60, 2, 10, QChar('0')).arg(s, 2, 10, QChar('0')).arg(f, 2, 10, QChar('0'));
    return QString("%1:%2.%3").arg(m).arg(s, 2, 10, QChar('0')).arg(f, 2, 10, QChar('0'));
}

}  // namespace

TouchShellImpl::TouchShellImpl(TouchShell* shell, MainWindow* window) : q(shell), mw(window), pb(window->playback()) {}

void TouchShellImpl::build()
{
    q->setObjectName("touchShell");
    q->setAttribute(Qt::WA_StyledBackground, true);
    q->setStyleSheet(styleSheet());
    QPalette pal = q->palette();
    pal.setColor(QPalette::Window, color::bg);
    pal.setColor(QPalette::WindowText, color::text);
    q->setPalette(pal);
    q->setAutoFillBackground(true);

    buildTopBar();
    previewBox = new Fill(Qt::black, q);
    previewBox->setObjectName("touchPreviewBox");
    auto* pl = new QVBoxLayout(previewBox);
    pl->setContentsMargins(0, 0, 0, 0);
    buildTransport();
    timeline = new TouchTimeline(pb, q);
    timeline->setObjectName("touchTimeline");
    timeline->onTrackTapped = [this](uint64_t id) { trackSheet(id); };
    timeline->onAddTrack = [this] { addTrackSheet(); };
    timeline->onEmptyTapped = [this] { addMedia(false); };
    timeline->onEventDoubleTapped = [this](uint64_t id) {
        const Event* e = app.project().findEvent(id);
        const MediaItem* m = e ? app.project().findMedia(e->mediaId) : nullptr;
        if (m && m->generated) textSheet(true);
        else moreSheet();
    };
    toolScroll = new QScrollArea(q);
    toolScroll->setObjectName("touchTools");
    toolScroll->setFrameShape(QFrame::NoFrame);
    toolScroll->setWidgetResizable(true);
    toolScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    toolScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QPalette tp = toolScroll->palette();
    tp.setColor(QPalette::Window, color::bar);
    toolScroll->setPalette(tp);
    toolScroll->setAutoFillBackground(true);
    makeFlickable(toolScroll);
    // the tool bar scrolls sideways (portrait) or up and down (landscape), whichever it has room to
    {
        QScrollerProperties sp = QScroller::scroller(toolScroll->viewport())->scrollerProperties();
        sp.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy, QScrollerProperties::OvershootWhenScrollable);
        QScroller::scroller(toolScroll->viewport())->setScrollerProperties(sp);
    }
    backdrop = new Backdrop(q);
    static_cast<Backdrop*>(backdrop)->onTap = [this] { q->closeSheet(); };
    backdrop->hide();
    clock = new QTimer(q);
    clock->setInterval(33);
    QObject::connect(clock, &QTimer::timeout, q, [this] { refreshTime(); });

    auto* back = new BackKey(q);
    back->onBack = [this] {
        if (!q->isVisible()) return false;
        if (sheet) {
            q->closeSheet();
            return true;
        }
        if (fullPreview) {
            setFullPreview(false);
            return true;
        }
        if (!app.selection().empty()) {
            app.setSelection({});
            return true;
        }
        return false;
    };
    qApp->installEventFilter(back);

    toast = new Toast(q);
    // the messages the desktop window shows in its status bar
    oldStatus = app.onStatus;
    app.onStatus = [this](const std::string& m) {
        if (oldStatus) oldStatus(m);
        say(QString::fromStdString(m));
    };
    listener = app.addListener([this](int changes) { this->changed(changes); });
    refreshTitle();
    refreshTime();
    refreshTools(true);
}

void TouchShellImpl::buildTopBar()
{
    topBar = new Fill(color::bar, q);
    topBar->setObjectName("touchTopBar");
    auto* h = new QHBoxLayout(topBar);
    h->setContentsMargins(4, 2, 10, 2);
    h->setSpacing(2);
    menuB = new Button(Button::Bar, Icon::Menu, QString(), topBar);
    menuB->setObjectName("touchMenu");
    QObject::connect(menuB, &QAbstractButton::clicked, q, [this] { projectMenu(); });
    h->addWidget(menuB);
    title = new QLabel(topBar);
    title->setObjectName("touchTitle");
    title->setFont(uiFont(16, true));
    QPalette pal = title->palette();
    pal.setColor(QPalette::WindowText, color::text);
    title->setPalette(pal);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    h->addWidget(title, 1);
    undoB = new Button(Button::Bar, Icon::Undo, QString(), topBar);
    undoB->setObjectName("touchUndo");
    QObject::connect(undoB, &QAbstractButton::clicked, q, [this] {
        const std::string what = app.undoName();
        app.undo();
        if (!what.empty()) say("Undone: " + QString::fromStdString(what));
    });
    redoB = new Button(Button::Bar, Icon::Redo, QString(), topBar);
    redoB->setObjectName("touchRedo");
    QObject::connect(redoB, &QAbstractButton::clicked, q, [this] {
        const std::string what = app.redoName();
        app.redo();
        if (!what.empty()) say("Redone: " + QString::fromStdString(what));
    });
    h->addWidget(undoB);
    h->addWidget(redoB);
    h->addSpacing(6);
    exportB = new Button(Button::Primary, Icon::Export, "Export", topBar);
    exportB->setObjectName("touchExport");
    QObject::connect(exportB, &QAbstractButton::clicked, q, [this] { exportSheet(); });
    h->addWidget(exportB);
}

void TouchShellImpl::buildTransport()
{
    transport = new Fill(color::bg, q);
    transport->setObjectName("touchTransport");
    auto* h = new QHBoxLayout(transport);
    h->setContentsMargins(12, 0, 6, 0);
    h->setSpacing(0);
    timeText = new QLabel(transport);
    timeText->setObjectName("touchTime");
    timeText->setTextFormat(Qt::RichText);
    h->addWidget(timeText);
    h->addStretch(1);
    auto bar = [&](Icon icon, const char* name, std::function<void()> fn) {
        Button* b = new Button(Button::Bar, icon, QString(), transport);
        b->setObjectName(name);
        QObject::connect(b, &QAbstractButton::clicked, q, std::move(fn));
        h->addWidget(b);
        return b;
    };
    bar(Icon::ToStart, "touchToStart", [this] {
        timeline->stopFling();
        if (pb->playing()) pb->jumpTo(0);
        else app.setCursor(0);
    });
    playB = bar(Icon::Play, "touchPlay", [this] { togglePlay(); });
    bar(Icon::ToEnd, "touchToEnd", [this] {
        timeline->stopFling();
        if (pb->playing()) pb->pause();
        app.setCursor(app.project().length());
    });
    h->addStretch(1);
    h->addSpacing(8);
    fullB = bar(Icon::Fullscreen, "touchFullPreview", [this] { setFullPreview(!fullPreview); });
}

bool TouchShell::wide() const { return width() > height() * 1.15 && width() >= 560; }
bool TouchShell::tablet() const { return std::min(width(), height()) >= 600; }

void TouchShellImpl::layout()
{
    const int W = q->width(), H = q->height();
    const bool wide = q->wide();
    const bool tab = q->tablet();
    timeline->setTablet(tab);
    topBar->setGeometry(0, 0, W, kBarH);
    int y = kBarH;
    if (fullPreview) {
        previewBox->setGeometry(0, y, W, H - y - kBarH);
        transport->setGeometry(0, H - kBarH, W, kBarH);
        timeline->hide();
        toolScroll->hide();
    } else if (!wide) {
        // portrait: preview, transport, timeline, tools, top to bottom
        const int toolsH = kToolH;
        const int room = H - kBarH - kBarH - toolsH;
        const int previewH = std::max(120, std::min((int)std::lround(W * 9.0 / 16.0), (int)(room * (tab ? 0.5 : 0.42))));
        previewBox->setGeometry(0, y, W, previewH);
        y += previewH;
        transport->setGeometry(0, y, W, kBarH);
        y += kBarH;
        timeline->setGeometry(0, y, W, std::max(0, H - toolsH - y));
        toolScroll->setGeometry(0, H - toolsH, W, toolsH);
        timeline->show();
        toolScroll->show();
    } else {
        // landscape: preview with the tools beside it, then the transport and the timeline across the whole width
        const int toolsW = 80;
        const int timelineH = std::max(140, (int)(H * (tab ? 0.4 : 0.42)));
        const int previewH = std::max(80, H - kBarH - kBarH - timelineH);
        previewBox->setGeometry(0, y, W - toolsW, previewH);
        toolScroll->setGeometry(W - toolsW, y, toolsW, previewH);
        y += previewH;
        transport->setGeometry(0, y, W, kBarH);
        y += kBarH;
        timeline->setGeometry(0, y, W, H - y);
        timeline->show();
        toolScroll->show();
    }
    if (tools) {
        auto* box = static_cast<QBoxLayout*>(tools->layout());
        const QBoxLayout::Direction want = wide && !fullPreview ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
        if (box && box->direction() != want) refreshTools(true);
    }
    if (sheet) placeSheet(sheet, false);
}

void TouchShellImpl::placeSheet(Sheet* s, bool animate)
{
    const int W = q->width(), H = q->height();
    QRect target;
    s->sideways = q->wide();
    if (s->sideways) {
        const int sw = std::min(420, std::max(300, (int)(W * 0.42)));
        target = QRect(W - sw, kBarH, sw, H - kBarH);
    } else {
        const int maxH = (int)(H * (s->modal ? 0.72 : 0.5));
        const int h = std::max(160, std::min(maxH, s->preferredHeight(W)));
        target = QRect(0, H - h, W, h);
    }
    s->setGeometry(target);
    if (s->modal) {
        backdrop->setGeometry(q->rect());
        backdrop->show();
        backdrop->raise();
    } else backdrop->hide();
    s->show();
    s->raise();
    if (animate) {
        auto* a = new QPropertyAnimation(s, "pos", s);
        a->setDuration(200);
        a->setEasingCurve(QEasingCurve::OutCubic);
        a->setStartValue(s->sideways ? QPoint(W, target.y()) : QPoint(0, H));
        a->setEndValue(target.topLeft());
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }
}

void TouchShellImpl::attachPreview()
{
    if (preview) return;
    PreviewWidget* pw = mw->preview();
    if (!pw) return;
    preview = pw;
    previewHome = pw->parentWidget();
    pw->setBare(true);
    pw->setParent(previewBox);
    previewBox->layout()->addWidget(pw);
    pw->show();
    if (QWidget* pic = pw->pictureWidget()) {
        auto* tap = new PictureTap(previewBox);
        tap->allowed = [pw] { return pw->overlayEffect() == 0; };
        tap->onTap = [this] { togglePlay(); };
        pic->installEventFilter(tap);
        pictureTap = tap;
    }
}

void TouchShellImpl::detachPreview()
{
    PreviewWidget* pw = static_cast<PreviewWidget*>(preview.data());
    if (!pw) return;
    previewBox->layout()->removeWidget(pw);
    if (pictureTap && pw->pictureWidget()) pw->pictureWidget()->removeEventFilter(pictureTap);
    delete pictureTap;
    pictureTap = nullptr;
    pw->setBare(false);
    if (previewHome) {
        pw->setParent(previewHome);
        if (auto* v = qobject_cast<QVBoxLayout*>(previewHome->layout())) v->insertWidget(0, pw, 1);
        pw->show();
    }
    preview = nullptr;
}

void TouchShellImpl::changed(int changes)
{
    if (changes & (kFile | kProject)) refreshTitle();
    if (changes & (kCursor | kProject | kPlayback)) refreshTime();
    if (changes & kPlayback) {
        playB->setIconKind(pb->playing() ? Icon::Pause : Icon::Play);
        if (pb->playing()) clock->start();
        else clock->stop();
    }
    if (changes & (kSelection | kProject)) {
        if (app.selection().empty() && timeline->multiSelect()) timeline->setMultiSelect(false);
        refreshTools();
        // an adjusting sheet belongs to the selection it was opened for
        if (sheet && !sheet->modal && (changes & kSelection)) {
            const QVariant was = sheet->property("renoSelection");
            QString now;
            for (uint64_t id : app.selection()) now += QString::number(id) + ",";
            if (was.isValid() && was.toString() != now) q->closeSheet();
        }
    }
    undoB->setEnabled(app.canUndo());
    redoB->setEnabled(app.canRedo());
}

void TouchShellImpl::refreshTitle()
{
    QString name = QFileInfo(QString::fromStdString(app.path())).completeBaseName();
    if (name.isEmpty()) name = "Untitled";
    title->setText(app.dirty() ? name + QString::fromUtf8(" •") : name);
    undoB->setEnabled(app.canUndo());
    redoB->setEnabled(app.canRedo());
}

void TouchShellImpl::refreshTime()
{
    const double fps = app.project().settings.fps();
    const double t = pb->playing() ? pb->position() : app.cursor();
    timeText->setText(QString("<span style='color:%1; font-size:17px; font-weight:600'>%2</span>"
                              "<span style='color:%3; font-size:13px'>&nbsp;/&nbsp;%4</span>")
                          .arg(color::text.name(), clockText(t, fps), color::dim.name(), clockText(app.project().length(), fps)));
}

const Event* TouchShellImpl::soleEvent(const Track** track) const
{
    if (app.selection().size() != 1) return nullptr;
    return app.project().findEvent(*app.selection().begin(), track);
}

// The selected events and those grouped with them: a clip's picture and its sound are edited as one.
std::set<uint64_t> TouchShellImpl::selectionWithGroups() const
{
    const Project& p = app.project();
    std::set<uint64_t> groups, out = app.selection();
    for (uint64_t id : out)
        if (const Event* e = p.findEvent(id))
            if (e->group) groups.insert(e->group);
    if (!groups.empty())
        for (const Track& t : p.tracks)
            for (const Event& e : t.events)
                if (e.group && groups.count(e.group)) out.insert(e.id);
    return out;
}

bool TouchShellImpl::selectionHasVideo() const
{
    for (uint64_t id : selectionWithGroups()) {
        const Track* t = nullptr;
        if (app.project().findEvent(id, &t) && t && t->type == TrackType::Video) return true;
    }
    return false;
}

bool TouchShellImpl::selectionHasAudio() const
{
    for (uint64_t id : selectionWithGroups()) {
        const Track* t = nullptr;
        if (app.project().findEvent(id, &t) && t && t->type == TrackType::Audio) return true;
    }
    return false;
}

bool TouchShellImpl::runAction(const char* name)
{
    QAction* a = mw->vegasAction(name);
    if (!a) a = mw->findChild<QAction*>(name);
    if (!a || !a->isEnabled()) return false;
    a->trigger();
    return true;
}

void TouchShellImpl::say(const QString& text)
{
    if (text.trimmed().isEmpty() || !q->isVisible()) return;
    // above the tool bar (or an open sheet)
    int bottom = toolScroll->isVisible() && !q->wide() ? toolScroll->y() : q->height();
    if (sheet && sheet->isVisible() && !sheet->sideways) bottom = std::min(bottom, sheet->y());
    static_cast<Toast*>(toast)->say(text, bottom);
}

void TouchShellImpl::addTool(QBoxLayout* row, const QString& name, Icon icon, const QString& label, std::function<void()> fn, bool danger)
{
    Button* b = new Button(Button::Tool, icon, label, tools);
    b->setObjectName("tool_" + name);
    b->setDanger(danger);
    toolFns[name] = fn;
    toolOrder << name;
    QObject::connect(b, &QAbstractButton::clicked, q, [fn] { fn(); });
    row->addWidget(b);
}

// The tools follow what is selected: with nothing selected, adding things; with events selected, changing them.
void TouchShellImpl::refreshTools(bool force)
{
    const bool wide = q->wide() && !fullPreview;
    const Track* track = nullptr;
    const Event* one = soleEvent(&track);
    const MediaItem* m = one ? app.project().findMedia(one->mediaId) : nullptr;
    const bool sel = !app.selection().empty();
    const bool video = selectionHasVideo(), audio = selectionHasAudio();
    const bool text = m && m->generated;
    const QString key = QString("%1|%2|%3|%4|%5|%6|%7|%8").arg(wide).arg(sel).arg(video).arg(audio).arg(text).arg(app.hasClipboard()).arg(one ? 1 : 0).arg(timeline->multiSelect());
    if (!force && key == toolsKey) return;
    toolsKey = key;
    const int keepH = toolScroll->horizontalScrollBar()->value(), keepV = toolScroll->verticalScrollBar()->value();
    toolFns.clear();
    toolOrder.clear();
    tools = new QWidget();
    tools->setObjectName("touchToolRow");
    auto* row = new QBoxLayout(wide ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, tools);
    row->setContentsMargins(wide ? 6 : 4, 0, wide ? 6 : 4, 0);
    row->setSpacing(0);
    if (!sel) {
        addTool(row, "media", Icon::Media, "Media", [this] { addMedia(false); });
        addTool(row, "audio", Icon::Music, "Audio", [this] { addMedia(true); });
        addTool(row, "text", Icon::Text, "Text", [this] { textSheet(false); });
        addTool(row, "split", Icon::Split, "Split", [this] { app.splitAtCursor(); });
        if (app.hasClipboard()) addTool(row, "paste", Icon::Paste, "Paste", [this] { app.pasteAtCursor(); });
        addTool(row, "effects", Icon::Fx, "Effects", [this] { fxSheet({FxTarget::VideoOut, 0}); });
        addTool(row, "marker", Icon::Marker, "Marker", [this] { addMarker(); });
        addTool(row, "fit", Icon::Fullscreen, "Fit", [this] { timeline->zoomToFit(); });
        addTool(row, "tracks", Icon::Track, "Track", [this] { addTrackSheet(); });
        addTool(row, "commands", Icon::Search, "Commands", [this] { commandsSheet(); });
    } else {
        addTool(row, "done", Icon::Back, "Done", [this] {
            timeline->setMultiSelect(false);
            app.setSelection({});
        });
        addTool(row, "several", Icon::Check, "Select", [this] {
            timeline->setMultiSelect(!timeline->multiSelect());
            say(timeline->multiSelect() ? "Tap clips to add them to the selection or take them out" : "Taps select one clip again");
            refreshTools(true);
        });
        if (QAbstractButton* b = tools->findChild<QAbstractButton*>("tool_several")) {
            b->setCheckable(true);
            b->setChecked(timeline->multiSelect());
        }
        addTool(row, "split", Icon::Split, "Split", [this] { app.splitAtCursor(); });
        addTool(row, "speed", Icon::Speed, "Speed", [this] { speedSheet(); });
        if (audio && !video) addTool(row, "volume", Icon::Volume, "Volume", [this] { levelSheet(); });
        else addTool(row, "volume", Icon::Opacity, video && audio ? "Level" : "Opacity", [this] { levelSheet(); });
        addTool(row, "fade", Icon::Fade, "Fades", [this] { fadeSheet(); });
        if (one) {
            const FxTarget t{FxTarget::Event, one->id};
            addTool(row, "effects", Icon::Fx, "Effects", [this, t] { fxSheet(t); });
        }
        if (text) {
            addTool(row, "edittext", Icon::Text, "Edit text", [this] { textSheet(true); });
            const uint64_t mediaId = m->id, slot = m->generator.id;
            addTool(row, "textstyle", Icon::Settings, "Style", [this, mediaId, slot] { effectSheet({FxTarget::Generator, mediaId}, slot); });
        }
        if (one && track && track->type == TrackType::Video) {
            addTool(row, "transition", Icon::Transition, "Transition", [this] { transitionSheet(); });
            addTool(row, "crop", Icon::Crop, "Pan/Crop", [this] { runAction("eventPanCrop"); });
        }
        addTool(row, "duplicate", Icon::Duplicate, "Duplicate", [this] { duplicateSelection(); });
        addTool(row, "copy", Icon::Copy, "Copy", [this] { app.copySelection(); refreshTools(true); });
        addTool(row, "reverse", Icon::Reverse, "Reverse", [this] { toggleOnSelection("Reverse", &Event::reverse); });
        addTool(row, "mute", Icon::Mute, "Mute", [this] { toggleOnSelection("Mute", &Event::mute); });
        addTool(row, "more", Icon::More, "More", [this] { moreSheet(); });
        addTool(row, "delete", Icon::Delete, "Delete", [this] { app.deleteSelection(); }, true);
    }
    row->addStretch(1);
    QWidget* old = toolScroll->takeWidget();
    toolScroll->setWidget(tools);
    if (old) old->deleteLater();
    toolScroll->horizontalScrollBar()->setValue(keepH);
    toolScroll->verticalScrollBar()->setValue(keepV);
}

void TouchShellImpl::togglePlay()
{
    timeline->stopFling();
    if (pb->playing()) {
        // pause where it is: the playhead stays on the frame being shown
        const double t = pb->position();
        pb->pause();
        app.setCursor(t);
        return;
    }
    if (app.cursor() >= app.project().length() - 1e-3) app.setCursor(0);
    pb->play();
}

void TouchShellImpl::setFullPreview(bool on)
{
    fullPreview = on;
    fullB->setChecked(false);
    fullB->setIconKind(on ? Icon::Close : Icon::Fullscreen);
    if (on) q->closeSheet();
    layout();
}

// ---------------------------------------------------------------- TouchShell

TouchShell::TouchShell(MainWindow* window) : QWidget(window), d(new TouchShellImpl(this, window))
{
    d->build();
}

TouchShell::~TouchShell()
{
    d->stopExport();
    AppState::get().onStatus = d->oldStatus;
    AppState::get().removeListener(d->listener);
    d->detachPreview();
}

TouchTimeline* TouchShell::timeline() const { return d->timeline; }
touch::Sheet* TouchShell::sheet() const { return d->sheet; }

void TouchShell::closeSheet()
{
    if (!d->sheet) return;
    touch::Sheet* s = d->sheet;
    d->sheet = nullptr;
    d->backdrop->hide();
    s->close();
}

void TouchShell::openSheet(touch::Sheet* s)
{
    if (d->sheet && d->sheet != s) {
        touch::Sheet* old = d->sheet;
        d->sheet = nullptr;
        old->hide();
        old->deleteLater();
    }
    d->sheet = s;
    s->body()->addStretch(1);
    QString sel;
    for (uint64_t id : AppState::get().selection()) sel += QString::number(id) + ",";
    s->setProperty("renoSelection", sel);
    d->placeSheet(s, true);
}

void TouchShell::runTool(const QString& name)
{
    auto it = d->toolFns.find(name);
    if (it != d->toolFns.end()) it->second();
}

QStringList TouchShell::toolNames() const { return d->toolOrder; }

void TouchShell::resizeEvent(QResizeEvent*) { d->layout(); }

void TouchShell::showEvent(QShowEvent*)
{
    d->attachPreview();
    d->layout();
}

}  // namespace app
}  // namespace reno
