// Private parts of the timeline widget, shared by the timeline*.cpp files: sizes, colors, helper types and the widget's
// implementation struct. Not for use outside the timeline.
#pragma once

#include "timeline.h"
#include "../engine/fxnames.h"
#include "../engine/timefmt.h"
#include "../media/vegasfiles.h"
#include "audiotools.h"
#include "cachedscroll.h"
#include "cursors.h"
#include "man2dialogs.h"
#include "mediacache.h"
#include "panels.h"
#include "playback.h"
#include "pluginchooser.h"
#include "timesource.h"
#include "vprefs.h"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QTextLayout>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace reno {
namespace app {

namespace tl {

// ---- sizes and colors
// Measured from VEGAS Pro 18's track view.
const int kHeaderDefault = 161;
const int kHeaderMin = 130, kHeaderMax = 420;
const int kHeaderGap = 6;
const int kHeaderMargin = 3;
const int kStripW = 24;
const int kMarkerTop = 1;
const int kMarkerH = 16;
const int kLoopTop = kMarkerTop + kMarkerH;
const int kLoopH = 12;
const int kRulerTop = kLoopTop + kLoopH + 2;
const int kRulerH = 16;
const int kTopH = kRulerTop + kRulerH;
const int kVideoH = 64;
const int kAudioH = 68;
const int kMinTrackH = 24;  // a minimized track (the ` key), and the least a track can be dragged to (VEGAS's collapsed header)
const int kMaxTrackH = 1000;
const int kTrackEdgePx = 4;         // how near a header's bottom edge the mouse takes hold of it (from above; 2 from below)
const int kBusH = 48;
const int kTitleH = 15;
const int kTitleMinRow = 88;
const int kBelow = 48;
const int kBar = 17;
const double kEdgePx = 5.0;
const double kGainGrabPx = 4.0;
const double kHandlePx = 11.0;
const double kMinRate = 0.1, kMaxRate = 10.0;
const int kEventButton = 16;

inline int clampHeaderW(int w) { return std::max(kHeaderMin, std::min(kHeaderMax, w)); }
inline int clampTrackH(int h) { return std::max(kMinTrackH, std::min(kMaxTrackH, h)); }

extern QColor kWindow;
extern QColor kPanel;
extern QColor kHeaderBg;
extern QColor kHeaderCurrent;
extern QColor kLane;
extern QColor kBusLane;
extern QColor kBelowBg;
extern QColor kLine;
extern QColor kLightLine;
extern QColor kText;
extern QColor kTextOnLight;
extern QColor kDimText;
extern QColor kSnapLine;
extern QColor kDrop;
extern QColor kCursorLine;
extern QColor kRulerBg;
extern QColor kRulerText;
extern QColor kLoopOn;
extern QColor kLoopOff;
extern QColor kSelectionWash;
extern QColor kMarkerColor;
extern QColor kRegionColor;
extern QColor kTimeText;
extern QColor kFxOn;
extern QColor kFxBad;
extern QColor kFadeHandle;


QColor defaultTrackColor(int index);

inline QColor trackColor(const Track& t, int index)
{
    if (t.color) return QColor((int)((t.color >> 16) & 0xff), (int)((t.color >> 8) & 0xff), (int)(t.color & 0xff));
    return defaultTrackColor(index);
}

inline QColor mix(const QColor& a, const QColor& b, double t)
{
    return QColor((int)std::lround(a.red() + (b.red() - a.red()) * t), (int)std::lround(a.green() + (b.green() - a.green()) * t),
                  (int)std::lround(a.blue() + (b.blue() - a.blue()) * t));
}

inline double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline void splitPair(const std::string& data, std::string& id, std::string& label)
{
    const size_t nl = data.find('\n');
    id = data.substr(0, nl);
    label = nl == std::string::npos ? id : data.substr(nl + 1);
}

// The part of a track's level slider that stands for a volume. A cube law, with 0 dB a little under two thirds of the
// way along and +12 dB at the end: fine steps where they matter, and all the way down to silence.
const double kMaxVolume = 4.0;
inline double volumeToSlider(double gain) { return clampd(std::cbrt(std::max(0.0, gain) / kMaxVolume), 0.0, 1.0); }
inline double sliderToVolume(double s) { return kMaxVolume * s * s * s; }

inline QString levelText(bool video, double level)
{
    if (video) return QString::fromStdString(strfmt("%.1f %%", level * 100.0));
    if (level <= 1e-5) return QString("-Inf dB");
    return QString::fromStdString(strfmt("%.1f dB", 20.0 * std::log10(level)));
}

inline QString panText(double pan)
{
    if (std::fabs(pan) < 0.005) return QString("Center");
    return QString::fromStdString(strfmt("%d%% %s", (int)std::lround(std::fabs(pan) * 100.0), pan < 0 ? "L" : "R"));
}

inline int64_t rulerStep(double pps, double fps, double minPx) { return rulerStepFrames(pps, fps, minPx); }

inline int rulerDivisions(int64_t stepFrames, double pps, double fps)
{
    for (int d : {10, 5, 4, 3, 2})
        if (stepFrames % d == 0 && (double)(stepFrames / d) / fps * pps >= 40.0) return d;
    return 1;
}

typedef std::vector<std::pair<double, double>> Ranges;

Ranges merged(Ranges v);

double removedBefore(const Ranges& gone, double t);

void rippleClose(Project& p, const std::map<uint64_t, Ranges>& removed, RippleMode mode);

// What a ripple carries along when an edit makes room or takes it away at 'from' on the given tracks: the events on
// those tracks (all tracks for AllTracks) that start at or after 'from', and the markers after it (unless only the
// affected tracks ripple). The ids of what moves are kept so a drag can move them again from where they were.
struct Followers {
    std::vector<std::pair<uint64_t, double>> events;
    std::vector<std::pair<uint64_t, double>> markers;
    void apply(Project& p, double delta, bool quantize) const
    {
        for (const auto& f : events)
            if (Event* e = p.findEvent(f.first)) e->start = std::max(0.0, quantize ? p.quantize(f.second + delta) : f.second + delta);
        for (const auto& f : markers)
            for (auto& m : p.markers)
                if (m.id == f.first) m.time = std::max(0.0, f.second + delta);
    }
};

Followers followersOf(const Project& p, const std::set<uint64_t>& tracks, double from, const std::set<uint64_t>& exclude, RippleMode mode);

// ---- thumbnails and waveforms
// A waveform: the lowest and highest sample of each short stretch of a file's sound, at several levels of detail
// (each kPeakStep times coarser than the one before), so drawing a long file zoomed out is as quick as a short one.
// The loader thread fills it in from the start; 'filled' says how far it has got and the interface only reads below
// that, so no lock is needed.
const int kPeakRate = 6000;
const int kPeakBucket = 30;
const int kPeakStep = 8;
const double kPeakPerSecond = (double)kPeakRate / kPeakBucket;

struct Peaks {
    struct Level { std::vector<int8_t> lo, hi; };
    std::vector<Level> levels;
    std::atomic<int64_t> filled{0};
    std::atomic<bool> complete{false};

    int64_t ready(size_t level) const
    {
        if (complete.load()) return (int64_t)levels[level].lo.size();
        int64_t n = filled.load();
        for (size_t k = 0; k < level; ++k) n /= kPeakStep;
        return n;
    }
};

struct ThumbKey {
    int media = 0;
    int height = 0;
    int64_t ms = 0;
    bool operator<(const ThumbKey& o) const { return std::tie(media, height, ms) < std::tie(o.media, o.height, o.ms); }
};

inline QImage imageToQt(const Image& img)
{
    QImage q(img.w, img.h, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < img.h; ++y) std::memcpy(q.scanLine(y), img.row(img.h - 1 - y), (size_t)img.w * 4);
    return q;
}

class Loader {
public:
    struct Done { ThumbKey key; QImage image; };

    Loader(MediaPool::Opener opener, std::function<void()> onReady, bool rebuild = false);

    ~Loader();

    void requestThumb(const ThumbKey& key, const MediaItem& m, double seconds, int w, int h);
    std::shared_ptr<Peaks> peaks(int user, const MediaItem& m, int first = -1, int count = 0);
    double duration(const MediaItem& m);
    std::vector<std::string> takeProbed();
    std::set<int> takeDropped();
    void forgetMedia(int user, const std::string& path);
    void forgetFiles();
    std::vector<Done> take();

private:
    struct Job { ThumbKey key; MediaItem media; double seconds = 0; int w = 0, h = 0; };
    struct Wave {
        MediaItem media;
        uint64_t user = 0;
        int first = -1, count = 0;
        std::shared_ptr<Peaks> peaks;
        std::vector<int64_t> done;
        bool failed = false;
        QString cacheFile;
        uint64_t leftOut = 0;
        // VEGAS's peaks (media/vegasfiles.h): read from a .sfk beside the file, or, for a file of one or two channels,
        // made while its sound is read at its own rate (and then written there); the waveform is computed from them.
        bool native = false;
        int rate = 0;
        std::shared_ptr<vegasfiles::PeakBuilder> builder;
    };
    struct Shared {
        std::mutex mutex;
        std::condition_variable cv;
        bool quit = false;
        std::deque<Job> thumbs;
        std::deque<Wave> waves;
        std::vector<Done> finished;
        bool rebuild = false;
        std::deque<MediaItem> probes;
        std::set<std::string> probing;
        std::map<std::string, MediaInfo> probed;
        std::vector<std::string> probedNew;
        bool posted = false;
        std::function<void()> onReady;
    };

    static MediaItem lightCopy(const MediaItem& m);
    static void wake(const std::shared_ptr<Shared>& s);

    static int8_t pack(float v) { return (int8_t)std::lround(std::min(1.0f, std::max(-1.0f, v)) * 127.0f); }

    static void coarsen(Peaks& pk, size_t level, int64_t from, int64_t to, int64_t below);
    static bool waveStep(Wave& w, MediaPool& pool, std::vector<float>& l, std::vector<float>& r);
    static void levelsFollow(Wave& w);
    static void levelsFinish(Wave& w, bool ok);

    // ---- VEGAS's peaks
    // Level 0 of the waveform from VEGAS's peaks of the file ('rate' its sample rate), as far as the first 'ready'
    // blocks reach (all of it when 'finished'). A stretch of the waveform takes the lowest and highest of the blocks
    // it overlaps, over the channels it is of.
    static void fillFromPeakFile(Wave& w, const vegasfiles::PeakFile& pf, int64_t ready, bool finished);
    static bool loadPeakFile(Wave& w);
    static void chooseNative(Wave& w);
    static bool nativeStep(Wave& w, MediaPool& pool, std::vector<float>& l, std::vector<float>& r);
    static void keepNative(const Wave& w);

    static const char* peaksMagic() { return "RenoPeaks1"; }

    static bool loadPeaks(Wave& w);
    static void savePeaks(const Wave& w);

    static bool cacheable(const MediaItem& m) { return !m.path.empty() && m.path.compare(0, 6, "synth:") != 0; }

    static void run(std::shared_ptr<Shared> s, MediaPool* pool);

    std::shared_ptr<Shared> s_;
    MediaPool pool_;
    std::thread thread_;
    std::set<ThumbKey> pending_;
    std::set<int> dropped_;
    std::map<std::tuple<int, int, int>, std::shared_ptr<Peaks>> peaks_;
};

class NameEdit : public QLineEdit {
public:
    explicit NameEdit(QWidget* parent) : QLineEdit(parent) {}
    std::function<void()> onCancel;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape) {
            if (onCancel) onCancel();
            e->accept();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

class Filter : public QObject {
public:
    explicit Filter(QObject* parent) : QObject(parent) {}
    std::function<bool(QEvent*)> fn;

protected:
    bool eventFilter(QObject*, QEvent* e) override { return fn ? fn(e) : false; }
};

enum class Part {
    None, Corner, Ruler, LoopStrip, MarkerBar, MarkerTag, RegionEndTag, Lane,
    EventBody, EventLeft, EventRight, FadeIn, FadeOut, EventFx, EventCrop,
    EventSnap,
    EventGain,
    EventMenu,
    HeaderBody, HeaderMenu, HeaderName, HeaderMotion, HeaderMute, HeaderSolo, HeaderFx, HeaderLevel, HeaderPan,
    HeaderSurround,
    // VEGAS's Automation Settings button (right-click: the automation mode), a video track's Fade slider (in automation
    // mode), an audio track's Invert Track Phase and bus (output) buttons
    HeaderAuto, HeaderFade, HeaderPhase, HeaderBus,
    HeaderGap, HeaderArm,
    BusHeader, BusLane,
    EnvPoint, EnvLine,
    HeaderEdge,
    TrackKey
};

struct Hit {
    Part part = Part::None;
    int row = -1;
    uint64_t track = 0;
    uint64_t event = 0;
    uint64_t marker = 0;
    EnvelopeOwner bus;
    EnvelopeRef env;
    int point = -1;
    double keyTime = -1;
    bool operator==(const Hit& o) const
    {
        return part == o.part && row == o.row && track == o.track && event == o.event && marker == o.marker && bus == o.bus && env == o.env && point == o.point &&
               keyTime == o.keyTime;
    }
    bool onEvent() const { return part >= Part::EventBody && part <= Part::EventMenu; }
    bool onHeader() const { return part >= Part::HeaderBody && part <= Part::HeaderGap; }
    bool onEnvelope() const { return part == Part::EnvPoint || part == Part::EnvLine; }
};

enum class Drag {
    None, Scrub, LoopRange, Move, TrimLeft, TrimRight, StretchLeft, StretchRight, FadeIn, FadeOut, Level, Pan,
    Fade,
    Marker, RegionStart, RegionEnd, RegionMove, RubberBand, ZoomRange, HeaderWidth,
    EnvPoints, EnvSegment, EnvBand,
    EnvBrush,                           // Shift+drag over an envelope: VEGAS's envelope brush draws points
    Slip, Slide,                        // Alt+drag an event: its media slips inside it; Ctrl+Alt+drag: it slides over its media
    SlipTrimLeft, SlipTrimRight,        // Alt+drag an edge (Alt+Shift+drag inside: the end): the media moves with the edge
    // Ctrl+Alt+drag an edge: the edit slides (the event next to it is trimmed the other way)
    RollLeft, RollRight,
    SplitTrim,                          // Ctrl+Alt+Shift+drag: split where pressed, then trim the way the mouse goes
    Shuffle,
    SurroundPoint,
    TrackHeight,
    SnapOffset,
    EventGain,
    TrackKeys
};

struct Mover {
    uint64_t id = 0;
    int track = 0;
    int ordinal = -1;
    double start = 0, length = 0, offset = 0, rate = 1, fadeIn = 0, fadeOut = 0;
    bool loop = true;
    double mediaLength = 0;
    double end() const { return start + length; }
};

enum class DropKind { None, Media, Files, VideoFx, AudioFx, Generator, Transition };

struct Ghost {
    uint64_t track = 0;
    bool video = true;
    double start = 0, length = 0;
};

const char* const kMimeMediaListFormat = "application/x-reno-media-list";

struct DropState {
    DropKind kind = DropKind::None;
    bool ok = false;
    double time = 0;
    uint64_t track = 0;
    std::vector<uint64_t> media;
    std::vector<Ghost> ghosts;
    uint64_t event = 0;
    bool onHeader = false;
    std::string data;
    bool takes = false;
};

class BarButton : public QAbstractButton {
public:
    typedef std::function<void(QPainter&, const QRectF&)> Picture;
    BarButton(QWidget* parent, const char* name, const QString& tip, std::function<void()> fn, Picture picture);

protected:
    void paintEvent(QPaintEvent*) override;
    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    Picture picture_;
};

inline void drawPlusMinus(QPainter& p, const QRectF& r, bool plus)
{
    const QPointF c = r.center();
    p.fillRect(QRectF(std::floor(c.x()) - 4.0, std::floor(c.y()) - 0.5, 9.0, 2.0), QColor(224, 224, 224));
    if (plus) p.fillRect(QRectF(std::floor(c.x()) - 0.5, std::floor(c.y()) - 4.5, 2.0, 9.0), QColor(224, 224, 224));
}

void drawMagnifier(QPainter& p, const QRectF& r);

// ---- envelopes
// How VEGAS shows each kind of envelope: its color, and where a value sits between the bottom (0) and the top (1) of
// the space it is drawn in.
QColor envelopeColor(const std::string& kind);

QString envelopeName(const std::string& kind);

const double kEnvMaxGain = 2.0;
double envToFrac(const std::string& kind, double v);

double fracToEnv(const std::string& kind, double f);

QString envValueText(const std::string& kind, double v);

std::vector<std::pair<QString, double>> envPresets(const std::string& kind);

QIcon curveIcon(int curve, bool falling, int crossCurve = -1);

const char* const kCurveNames[] = {"Linear", "Fast", "Slow", "Smooth", "Sharp", "Hold"};


}
using namespace tl;

struct TimelineWidget::Impl {
    TimelineWidget* q;
    Playback* playback;
    AppState& app;
    int listener = 0;
    QScrollBar* hbar = nullptr;
    static constexpr int kHBarRange = 1000000;
    static constexpr int kHBarMinBox = 20;
    double hbarSpan = 0, hbarTotal = 0;
    QScrollBar* vbar = nullptr;
    BarButton *zoomInButton = nullptr, *zoomOutButton = nullptr, *zoomToolButton = nullptr, *tallerButton = nullptr, *shorterButton = nullptr;
    // VEGAS's Marker Tool (its button at the top right of the timeline): markers chosen in the marker bar (Ctrl+click,
    // Shift+click, Left / Right, Shift+Left / Right) move together when one is dragged and go with Delete
    BarButton* markerToolButton = nullptr;
    bool markerTool = false;
    std::set<uint64_t> selMarkers;
    uint64_t markerAnchor = 0;
    int headerW = kHeaderDefault;
    NameEdit* nameEdit = nullptr;
    enum class Naming { None, Track, Event, Marker };
    Naming naming = Naming::None;
    uint64_t renaming = 0;
    QTimer* autoScroll = nullptr;
    // Automation recording (VEGAS's Automation Write Touch / Latch): a control moved while playing writes points at
    // the playing position; Latch goes on writing its last value until playback stops. The whole recording is one
    // drag of AppState's (one undo step).
    struct AutoRec {
        bool on = false, latch = false;
        std::vector<uint64_t> tracks;
        std::string kind;
        std::vector<EnvPoint> pts;
        double value = 0;
    } rec;
    QTimer* recTimer = nullptr;
    std::unique_ptr<Loader> loader;
    TimelineOptions opt;

    double pps = 50.0;
    double viewStart = 0;
    int scrollY = 0;
    bool fitWanted = false;
    QColor timeText, timeBack;
    // VEGAS's Expand Track Layers: which sub-lane (0 the A roll, 1 the B roll) each event of the track is on, and the
    // time spans where two events overlap (the transition roll). See buildLayers.
    struct LayerMap {
        std::unordered_map<uint64_t, int> of;
        std::vector<std::pair<double, double>> trans;
    };
    struct Row { int index; uint64_t id; bool video; int y, h; std::shared_ptr<LayerMap> lm; };
    std::vector<Row> rows;
    int contentH = kBelow;
    double heightScale = 1.0;
    double waveZoom = 1.0;
    std::set<uint64_t> minimized;
    // tracks made as tall as the timeline shows (the track menu's Maximize Track Height): like minimized, a way of
    // looking at them that leaves their heights alone; Restore Track Height gives the height back
    std::set<uint64_t> maximized;
    struct BusRow { EnvelopeOwner owner; QString name; bool video; int y, h; };
    std::vector<BusRow> busRows;

    EnvelopeRef frontEnv;
    EnvelopeRef selEnv;
    struct EnvClip { std::string kind; std::vector<EnvPoint> points; double span = 0; } envClip;
    std::set<int> selPoints;
    QString envLabel;
    QPointF envLabelAt;

    // ---- the timeline as last painted, kept while playing. While only the cursor moves (a hundred times a second)
    // just two thin strips and the time display are repainted, and painting the lanes, headers and ruler in them was
    // most of the cost. So once the cursor has moved by itself a few times, the timeline without the cursor and time
    // display is painted once into 'layer' and the strips are copied from it. Anything else asking for a repaint lets
    // the picture go (layerValid); it is made again after a few more cursor moves.
    QImage layer;
    QImage cornerStatic;
    QString cornerKey;
    bool layerValid = false;
    bool layerOther = false;
    int layerTries = 0;
    int layerPaints = 0;
    QRegion cursorAsked;
    bool layerUsable() const;
    void buildLayer();
    void paintFromLayer(QPainter& p, const QRegion& region);
    double paintedCursor = -1;
    double paintedLoopA = 0, paintedLoopB = 0;
    bool paintedLoopOn = false;

    Hit hover;
    struct DragState {
        Drag kind = Drag::None;
        bool started = false;
        QPointF pressPos, lastPos;
        Qt::KeyboardModifiers lastMods = Qt::NoModifier;
        double pressTime = 0;
        Hit hit;
        bool collapseOnRelease = false;
        bool resumePlayback = false;
        std::vector<std::pair<uint64_t, double>> gains;
        bool gainVideo = false;
        double gainHeight = 1, gainDy = 0, gainLastY = 0;
        std::vector<Mover> movers;
        size_t grabbed = 0;
        std::vector<int> kindTracks;
        std::vector<double> snaps;
        double anchor = 0;
        double oldLoopA = 0, oldLoopB = 0;
        double startValue = 0;
        bool absolute = false;
        Followers followers;
        bool ripple = false;
        double markerTime = 0, markerLength = 0;
        std::set<uint64_t> baseSelection;
        EnvelopeRef env;
        std::vector<EnvPoint> envBase;
        std::vector<int> envMoving;
        double envOrigin = 0;
        double envFrom = -1e300, envTo = 1e300;
        QRectF envArea;
        std::map<long long, double> brush;
        double brushPps = 1;
        long long lastBrushCol = 0;
        struct EnvFollow { uint64_t track; size_t env, point; double time; };
        std::vector<EnvFollow> envFollow;
        bool ignoreGroup = false;
        uint64_t splitRight = 0;
        // Shuffle: the dragged event's time span, the tracks shuffled (its own and those of the events grouped with it),
        // the places it can go (edit points on its track), and where it goes now (its start: no change)
        double shufStart = 0, shufEnd = 0, shufTo = 0;
        std::set<uint64_t> shufTracks;
        std::vector<double> shufPoints;
        bool right = false;
        mutable int audioOnly = -1;
        int bandMode = 0;
        std::vector<std::pair<uint64_t, int>> heights;
    } drag;
    qint64 rightMenuAt = 0;
    double snapLine = -1;
    DropState drop;
    bool dropRight = false;
    bool groupBypass = false, groupBypassWas = false, swallowRight = false;
    bool expandedAtPress = false;

    struct EdgeSel {
        uint64_t event = 0;
        int side = 0;
        bool expanded = false;
        uint64_t other = 0;
        int which = 0;
    } edgeSel;
    double edgeCursor = -1;
    struct LastEdit {
        uint64_t revision = 0;
        std::set<uint64_t> tracks, exclude;
        double from = 0, delta = 0;
        std::map<uint64_t, Ranges> removed;
    } lastEdit;
    bool trackListFocus = false;
    std::set<uint64_t> selTracks;
    std::set<uint64_t> collapsedGroups;
    int listWidthBefore = 0;
    double lastLoopA = 0, lastLoopB = 0;

    std::map<std::string, int> mediaKeys;
    struct Thumb { QImage image; uint64_t used = 0; bool stale = false; };
    std::map<ThumbKey, Thumb> thumbs;
    std::set<int> projectMediaKeys;
    // Many events in view at once (a stutter edit zoomed out): their shapes are drawn without smoothing, which takes a
    // good part of painting them and is hardly seen at that size. Set by paintLanes.
    bool dense = false;
    std::string loadedPath;
    uint64_t paintCount = 0;
    QFont smallFont;
    QFont rulerFont;
    QFont fontsFor;
    bool fontsMade = false;
    QFontMetrics smallFm{QFont()};
    mutable double rulerLabelW = 0;
    mutable double rulerLabelFps = 0, rulerLabelWide = 0;
    // Each track's events by time: AppState keeps them in order of start, and 'maxEnd' (the latest end among events [0,
    // k]) lets the ones that reach into a time span be found by binary search instead of scanning thousands. Rebuilt.
    struct TrackIndex {
        std::vector<double> maxEnd;
        bool sorted = true;
        size_t count = 0;
        bool fadesMade = false;
        std::vector<int> fades;
    };
    mutable std::vector<TrackIndex> evIndex;
    mutable bool indexValid = false;
    mutable uint64_t indexRevision = 0;
    mutable std::unordered_map<uint64_t, size_t> mediaIndex;
    mutable std::unordered_map<const MediaItem*, int> mediaKeyOf;
    mutable std::unordered_map<uint64_t, std::pair<size_t, size_t>> eventPlaces;

    Impl(TimelineWidget* w, Playback* pb);

    ~Impl();

    double fps() const { return app.project().settings.fps(); }
    double frame() const { return 1.0 / fps(); }
    int lanesRight() const { return q->width() - kBar; }
    int lanesBottom() const { return q->height() - kBar; }
    int viewW() const { return std::max(1, lanesRight() - headerW); }
    QRect lanesRect() const { return QRect(headerW, kTopH, viewW(), std::max(0, lanesBottom() - kTopH)); }
    double timeAtF(double x) const { return viewStart + (x - headerW) / pps; }
    double xAtF(double t) const { return headerW + (t - viewStart) * pps; }
    int rowTop(const Row& r) const { return kTopH + r.y - scrollY; }

    double extent() const { return std::max(std::max(app.project().length(), app.cursor()), app.loopEnd > app.loopStart ? app.loopEnd : 0.0); }

    int trackHeight(const Track& t) const;
    void layout();
    std::shared_ptr<LayerMap> buildLayers(const Track& t) const;
    static double layerH(const Row& r) { return std::floor((r.h - 2.0) * 0.38); }

    const BusRow* busRowOf(const EnvelopeOwner& o) const
    {
        for (const BusRow& b : busRows) if (b.owner == o) return &b;
        return nullptr;
    }

    int busRowAtY(double y) const;
    int busTop(const BusRow& b) const { return kTopH + b.y - scrollY; }

    const Row* rowOf(uint64_t trackId) const
    {
        for (const Row& r : rows) if (r.id == trackId) return &r;
        return nullptr;
    }

    int rowAtY(double y) const;
    QRectF eventRect(const Row& r, const Event& e) const;

    // An event's buttons (Pan/Crop (video), FX, and the event's hamburger menu at the end) sit at the right of its
    // title bar (the event header), as in VEGAS 18; an event with no title bar (a short track, headers off) keeps
    // them at its bottom right.
    double eventTitleH(const QRectF& ev) const { return titleHeight((int)std::lround(ev.height()) + 2, ev.height()); }
    QRectF menuButtonRect(const QRectF& ev) const;
    QRectF fxButtonRect(const QRectF& ev) const { const QRectF m = menuButtonRect(ev); return m.translated(-(m.width() + (eventTitleH(ev) > 0 ? 3.0 : 4.0)), 0); }
    QRectF cropButtonRect(const QRectF& ev) const { const QRectF m = menuButtonRect(ev); return m.translated(-2.0 * (m.width() + (eventTitleH(ev) > 0 ? 3.0 : 4.0)), 0); }
    bool hasMenuButton(const QRectF& ev) const { return ev.width() >= kEventButton + 10.0 && ev.height() >= kEventButton + 6.0; }
    bool hasFxButton(const QRectF& ev) const { return ev.width() >= 2.0 * kEventButton + 14.0 && ev.height() >= kEventButton + 6.0; }
    bool hasCropButton(const QRectF& ev, bool video) const { return video && ev.width() >= 3.0 * kEventButton + 18.0 && ev.height() >= kEventButton + 6.0; }
    bool hasFadeHandles(const QRectF& ev) const { return opt.showFadeHandles && ev.width() >= 24.0; }
    // The gain line (VEGAS's event gain / opacity): straight across the event, level with its gain on a straight scale
    // (the top is 0 dB or 100 %, the bottom silence or nothing; a gain above the top is drawn at the top).
    bool hasGainLine(const QRectF& ev) const { return opt.showGainHandle && ev.width() >= 24.0 && ev.height() >= 12.0; }
    static double gainFraction(double gain) { return std::min(1.0, std::max(0.0, gain)); }
    static double gainLineY(const QRectF& ev, double gain) { return ev.top() + (1.0 - gainFraction(gain)) * ev.height(); }
    static QString gainText(bool video, double gain);

    // A header, as VEGAS lays it out: the colored strip down the left with the track menu button, number and an image
    // of its kind; then the body with the name at the top left, round buttons at the top right (solo, mute and, where
    // there is room, FX and Track Motion), and under them the level slider (for sound: volume, and pan under it) with
    // its value to the left.
    struct HeaderRects { QRect all, strip, menu, body, name, motion, fx, mute, solo, level, levelValue, pan, panValue, number, arm;
                         QRect surround;
                         // (see Part::HeaderAuto...)     // (an audio track in a 5.1 project: its small surround
                         // panner, in place of the pan slider)
                         QRect autoBtn, fade, phase, bus; };
    HeaderRects headerRects(const Row& r) const;
    void placeSurround(HeaderRects& h) const;

    // The header buttons Edit Visible Button Set has switched off (the track header's hamburger menu): per kind of
    // track, the keys "mute" "solo" "fx" "motion" "arm" "auto".
    std::set<QString> hiddenButtons[2];       // [0] audio, [1] video
    bool buttonHidden(bool video, const char* key) const { return hiddenButtons[video ? 1 : 0].count(QString::fromLatin1(key)) != 0; }

    HeaderRects vegasHeaderRects(const Row& r) const;
    void setHeaderWidth(int w);

    // The height of an event's title bar (0: none). Shown on tall tracks only (or with View > Event Headers).
    double titleHeight(int rowH, double eventH) const
    {
        return (rowH >= kTitleMinRow || opt.eventHeaders) ? std::min((double)kTitleH, eventH) : 0.0;
    }

    // VEGAS zooms out no further than the whole project (its scroll box then fills the bar): as far as zoom to fit goes
    double minPps() const { const double e = extent(); return viewW() / ((e > 0 ? std::max(1.0, e) : 30.0) * 1.04); }
    double maxPps() const { return std::max(minPps(), viewW() / (4.0 * frame())); }

    void updateBars();

    // The scroll bars' ends dragged (CachedScrollBar::onEdgeDrag). The groove's scale is frozen at the press (seconds
    // or content pixels per pixel of groove), and the dragged end of the view moves as far as the pointer has moved
    // from where it was pressed, at that scale: no jump when an end is grabbed, and the other end stays put. Dragging
    // an end outwards zooms out, inwards zooms in, as in VEGAS.
    struct BarZoom {
        bool active = false;
        bool vertical = false;
        double total0 = 0;
        double pos0 = 0;
        double scale = 0;
        double viewStart0 = 0, viewEnd0 = 0;
        double contentTop = 0, contentBottom = 0, tracksH = 0, totalH = 0;
        int visH = 0;
        std::map<uint64_t, int> heights;
        bool changed = false;
    } barZoom;

    void barEdgeDrag(bool vertical, int phase, int edge, double pos, double len);
    void setViewStart(double t);
    void setScrollY(int y);
    void scrollViewTo(double t);
    void viewMoved(double oldStart, int oldScrollY);
    void setZoom(double newPps, double anchorTime, double anchorX);
    void zoomBy(double factor);
    void zoomLarge(bool in);

    void centerOnCursor() { setViewStart(app.cursor() - viewW() / pps * 0.5); }

    void zoomTracks(int a, int b);
    void zoomToFit();
    void scrollToCursor();

    const Track* trackOf(const Row& r) const
    {
        const Project& p = app.project();
        return (size_t)r.index < p.tracks.size() ? &p.tracks[(size_t)r.index] : nullptr;
    }

    static const Event* fadesFrom(const Track& tr, const Event& e);
    static void fadesFromAll(const Track& tr, std::vector<const Event*>& out);

    // ---- the index of the events by time
    // Rebuilt whenever the project has changed since (or its tracks no longer match it). 'thorough': the tracks'
    // event counts are checked against it as well, once a paint, so a slip can't take the index past the end of a
    // track.
    void ensureIndex(bool thorough = false) const;
    std::pair<size_t, size_t> eventRange(size_t t, double from, double to) const;
    const MediaItem* mediaOf(uint64_t id) const;
    const Event* fadeSource(size_t t, size_t k) const;
    QRectF markerTag(int index, const Project::Marker& m) const;
    QRectF regionEndTag(const Project::Marker& m) const
    {
        return QRectF(std::floor(xAtF(m.time + m.length)) - 8.0, kMarkerTop + 2.0, 9.0, kMarkerH - 4.0);
    }
    int markerIndex(uint64_t id) const;

    int timelineFontSet = 0;

    void updateFonts();

    struct EnvLane {
        EnvelopeRef ref;
        const Envelope* env = nullptr;
        QRectF area;
        QRectF clip;
        double origin = 0;
        double from = -1e300, to = 1e300;
    };

    static QRectF envArea(const QRectF& r)
    {
        const double inset = std::min(4.0, std::floor(r.height() * 0.15));
        return r.adjusted(0, inset, 0, -inset - 1.0);
    }
    double envY(const EnvLane& L, double value) const { return L.area.bottom() - envToFrac(L.ref.kind, value) * L.area.height(); }
    double envValueAt(const EnvLane& L, double timelineT) const { return L.env->valueAt(timelineT - L.origin, AppState::envelopeDefault(L.ref.kind)); }
    QPointF envPointPos(const EnvLane& L, size_t k) const
    {
        const EnvPoint& pt = L.env->points[k];
        return QPointF(xAtF(L.origin + pt.time), envY(L, pt.value));
    }
    bool pointShown(const EnvLane& L, size_t k) const
    {
        const double t = L.origin + L.env->points[k].time;
        return t >= L.from - 1e-9 && t <= L.to + 1e-9;
    }
    static int segmentAt(const Envelope& e, double t);
    std::vector<EnvLane> envLanes() const;
    bool envLaneOf(const EnvelopeRef& ref, EnvLane* out) const;
    bool hitEnvelope(const QPointF& pos, Hit& h, bool points, bool lines) const;
    void drawEnvelope(QPainter& p, const EnvLane& L, const QRect& c);
    void drawEnvelopeLabel(QPainter& p);
    Hit hitTest(const QPointF& pos) const;

    bool snapOn(Qt::KeyboardModifiers mods) const { return app.snapping && !(mods & Qt::ShiftModifier); }

    std::vector<double> snapTargets(const std::set<uint64_t>& moving, bool withCursor, uint64_t movingMarker = 0) const;
    double gridStep(double minPx = 6.0) const;
    double quantized(double t) const;
    bool dragIsAudioOnly() const;
    bool nearest(const std::vector<double>& targets, double t, double* out) const;
    double snapped(const std::vector<double>& targets, double t, Qt::KeyboardModifiers mods, double* line) const;

    void showSnap(double line, double edge)
    {
        snapLine = (line >= 0 && std::fabs(edge - line) < frame() * 0.6) ? line : -1;
    }

    std::shared_ptr<const Project> notedProject;
    std::vector<Row> notedRows;
    std::set<uint64_t> notedSelection, notedArmed;
    uint64_t notedCurrent = 0;
    uint64_t notedFxErrors = 0;
    bool releasing = false;

    QRect eventArea(const Row& row, const Event& e) const;
    QRect eventArea(uint64_t id) const;
    QRect laneArea(const Row& r) const { return QRect(headerW, rowTop(r), viewW(), r.h).intersected(lanesRect()); }
    QRect headerColumn() const { return QRect(0, kTopH, headerW, std::max(0, lanesBottom() - kTopH)); }
    QRect timeStrip(double t) const { return QRect((int)std::floor(xAtF(t)) - 2, kTopH, 5, std::max(0, lanesBottom() - kTopH)); }

    static bool sameEnvelopes(const Envelopes& a, const Envelopes& b);
    static bool sameChain(const FxChain& a, const FxChain& b);
    static bool sameLook(const Event& a, const Event& b);
    static bool sameHeader(const Track& a, const Track& b);
    bool projectChangeRegion(QRegion& out);
    bool countDense() const;
    bool selectionChangeRegion(QRegion& out);
    uint64_t fxErrorSignature() const;

    // What is drawn for mouse and keyboard work beyond the project and selection (snap line, a point's value, selection
    // rectangle, a held slider, the edge selected for trimming, selected envelope points and tracks), as last painted.
    struct Overlays {
        double snap = -1;
        QRect label, band;
        uint64_t sliderTrack = 0;
        bool gapHeld = false;
        EnvelopeRef dragEnv, front, selEnv;
        std::set<int> selPoints;
        uint64_t edgeEvent = 0, edgeOther = 0;
        int edgeSide = 0, edgeWhich = 0;
        bool edgeExpanded = false;
        bool listFocus = false;
        std::set<uint64_t> selTracks;
    } notedOverlays;

    QRectF envLabelBox() const;
    Overlays currentOverlays() const;
    void overlaysChanged();

    // Whether a media file has been re-read since the last look and found to be there now or other than it was (its
    // length): Project Media's Refresh. (A file replaced by another is another path: its thumbnails are of the new one.)
    std::map<uint64_t, std::pair<bool, double>> mediaSeen;
    bool mediaReread();
    void changed(int changes);

    // The waveforms drawn so far, with how far each had been read when it was last looked at: when the loader says there
    // is more, only the events of the files whose thumbnails or waveforms came in are repainted.
    struct ShownPeaks { std::weak_ptr<Peaks> peaks; int64_t seen = -2; };
    std::map<std::tuple<int, int, int>, ShownPeaks> shownPeaks;

    void loaderReady();
    int mediaKeyFast(const MediaItem& m);

    struct NestedStamp { std::string path; qint64 size = -1, time = 0; };
    std::map<int, NestedStamp> nestedStamps;
    static NestedStamp nestedStampOf(const std::string& path);
    int mediaKey(const MediaItem& m);
    const QImage* thumb(const ThumbKey& key, const MediaItem& m, double seconds, int w, int h);

    void paint(QPainter& p, const QRect& clip);
    double rulerLabelWidth() const;
    static void drawRamp(QPainter& p, double xa, double ya, double xb, double yb, double clipL, double clipR, bool shade, double shadeTo);
    static void drawFade(QPainter& p, double xa, double xb, double top, double bottom, int curve, bool in, double clipL, double clipR, bool shade);
    static void drawLinkMark(QPainter& p, const QPointF& at, const QColor& color);
    static double thumbGrain(double tileSeconds, double mediaFps);
    void drawThumbNumber(QPainter& p, const QRectF& target, double mt, double mediaFps, const std::string& how);
    void drawThumbs(QPainter& p, const Event& e, const MediaItem& m, const QRectF& r, const QRectF& content, const QRect& c);
    void drawWave(QPainter& p, const Event& e, const MediaItem& m, const QRectF& content, const QColor& color, const QRect& c);

    template <class Fn>
    static void outlined(QPainter& p, Fn draw)
    {
        p.save();
        for (const QPointF& o : {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) {
            p.save();
            p.translate(o);
            draw(QColor(0, 0, 0, 200));
            p.restore();
        }
        p.restore();
        draw(QColor(255, 255, 255));
    }

    static void drawMenuIcon(QPainter& p, const QRectF& b, const QColor& color);

    // An event's name as its title bar shows it, elided to 'width' (cached from paint to paint: making and eliding
    // the text for each
    // of thousands of events on every paint costs).
    struct KeptText {
        QString text;
        QFont font;
        int dpi = 0;
        std::unique_ptr<QTextLayout> layout;
        double height = 0, width = 0;
    };
    static void drawKeptText(QPainter& p, const QRectF& r, const QString& text, KeptText& k, bool centre = false);

    struct TitleText {
        std::string source;
        double rate = 1.0;
        bool reverse = false;
        bool made = false;
        QString full;
        double fullWidth = -1;
        int width = -1;
        QString elided;
        KeptText kept;
    };
    std::unordered_map<uint64_t, TitleText> titles;
    double titleWidth(const Event& e);
    TitleText& titleText(const Event& e, const MediaItem* m, int width);

    // The colors of a track's events (computed once per paint per track). As in VEGAS: a sound event is filled with
    // its track's
    // color with the waveform in a lighter shade; a video event is its thumbnails, edge to edge. A selected event is
    // lit up.
    struct RowLook {
        QColor tc, body, title, wave;
    };
    RowLook rowLook(const Row& row) const;
    int sliverOf(const Row& row, const Event& e, const QRect& c, const RowLook& look, QRectF* rect, QColor* color) const;
    void drawEvent(QPainter& p, const Row& row, const Event& e, const QRect& c, const RowLook& look);
    struct EventPaint {
        const Row& row;
        const Event& e;
        const MediaItem* m;
        const QRect& c;
        const RowLook& look;
        QRectF r;
        QRectF v;
        QRectF vd;
        bool selected, narrow;
    };
    void drawEventContent(QPainter& p, const EventPaint& ev, double titleH);
    void drawStretchMark(QPainter& p, const EventPaint& ev, const QRectF& content);
    void drawEventTitle(QPainter& p, const EventPaint& ev, double titleH);
    void drawFadesAndGain(QPainter& p, const EventPaint& ev);
    void drawLoopNotches(QPainter& p, const EventPaint& ev);
    void drawEventOutline(QPainter& p, const EventPaint& ev);
    void drawFadeLengths(QPainter& p, const EventPaint& ev);
    void drawFadeHandles(QPainter& p, const EventPaint& ev);
    void drawEventButtons(QPainter& p, const EventPaint& ev);
    void drawEventExtras(QPainter& p, const Event& e, const QRectF& r, const QRectF& v);
    static QColor markerColor(const Project::Marker& m);
    void drawCrossFade(QPainter& p, const Row& row, const Event& a, const Event& b, const QRect& c);
    void paintGrid(QPainter& p, const QRect& c, int tracksBottom);
    void paintLayerLanes(QPainter& p, const Row& r, int y, const QRect& c);
    void paintLanes(QPainter& p, const QRect& clip);
    void paintDrop(QPainter& p, const QRect& c, int tracksBottom);

    mutable KeptText fxSigns[2];
    void drawFxIcon(QPainter& p, const QRectF& b, const QColor& color, bool bold) const;
    static void drawCropIcon(QPainter& p, const QRectF& b, const QColor& color);

    enum class Icon { Motion, Fx, Mute, Solo, Arm };

    void drawHeaderButton(QPainter& p, const QRect& r, Icon icon, bool on, bool hot, const QColor& mark) const;
    static void drawSlider(QPainter& p, const QRect& travel, double s, bool pointer, bool hot, bool onLight);
    static void drawKindIcon(QPainter& p, const QPointF& at, bool video);

    // A header button: a rounded box with its letters, filled with its colour while switched on.
    static void drawTextButton(QPainter& p, const QRect& r, const QString& text, bool on, const QColor& onColor, bool hot, bool darkText = false);
    static void drawAutoKnob(QPainter& p, const QRect& travel, double s);
    static void drawAutoIcon(QPainter& p, const QRect& r, bool on, const std::string& mode);
    static void drawAutoArrow(QPainter& p, const QRect& r);
    QString busLabel(const Track& t) const;
    void drawSmallSurround(QPainter& p, const QRect& r, const SurroundPan& sp, bool hot) const;

    // ---- the track headers kept as images. A header is drawn into an image of its own, shown until something the
    // header shows changes: everything that can change one is in HeaderKey, directly or through 'headerGeneration'
    // (moved on by any change to the project, the selection, the effects' settings, the preferences). The image is
    // made at the same fraction of a pixel the header would be painted at on screen.
    struct HeaderKey {
        uint64_t generation = 0;
        int headerW = 0, h = 0, index = 0, top = 0;
        bool video = false;
        double sx = 0, sy = 0, fx = 0, fy = 0;
        Part hotPart = Part::None;
        Drag held = Drag::None;
        bool current = false, selected = false, armed = false, naming = false, dropTarget = false, bad = false;
        double a1 = 0, a2 = 0, a3 = 0;
        bool operator==(const HeaderKey& o) const
        {
            return generation == o.generation && headerW == o.headerW && h == o.h && index == o.index && top == o.top &&
                   video == o.video && sx == o.sx && sy == o.sy && fx == o.fx && fy == o.fy && hotPart == o.hotPart && held == o.held &&
                   current == o.current && selected == o.selected && armed == o.armed && naming == o.naming && dropTarget == o.dropTarget && bad == o.bad &&
                   a1 == o.a1 && a2 == o.a2 && a3 == o.a3;
        }
    };
    struct HeaderPicture { HeaderKey key; QImage image; uint64_t used = 0; };
    std::unordered_map<uint64_t, HeaderPicture> headerPictures;
    uint64_t headerGeneration = 1;

    template <class Fn>
    void drawHeaderCached(QPainter& p, const Row& r, const QRect& header, const QColor& under, Fn draw)
    {
        const QRect all = header.adjusted(0, 0, 0, 16);
        const QTransform t = p.deviceTransform();
        const Track* tr = trackOf(r);
        if (t.type() > QTransform::TxScale || !tr || all.isEmpty() || t.m11() <= 0 || t.m11() != t.m22()) {
            p.save();
            draw(p);
            p.restore();
            return;
        }
        const QPointF d0 = t.map(QPointF(all.topLeft())), d1 = t.map(QPointF(all.left() + all.width(), all.top() + all.height()));
        const double ix = std::floor(d0.x()), iy = std::floor(d0.y());
        if (d0.x() != ix || d0.y() != iy) {
            // the screen scaled by a fraction, and the header's corner inside a device pixel: that pixel is shared with
            // the header above, and an image would cover all of it, so it is drawn as it is instead
            p.save();
            draw(p);
            p.restore();
            return;
        }
        HeaderKey k;
        k.generation = headerGeneration;
        k.headerW = headerW;
        k.h = r.h;
        k.index = r.index;
        k.top = rowTop(r) - all.top();
        k.video = r.video;
        k.sx = t.m11();
        k.sy = t.m22();
        k.fx = d0.x() - ix;
        k.fy = d0.y() - iy;
        if ((hover.onHeader() || hover.part == Part::HeaderArm) && hover.track == r.id) k.hotPart = hover.part;
        if ((drag.kind == Drag::Level || drag.kind == Drag::Pan || drag.kind == Drag::Fade || drag.kind == Drag::SurroundPoint) && drag.hit.track == r.id) k.held = drag.kind;
        k.current = r.id == app.currentTrack;
        k.selected = selTracks.count(r.id) != 0;
        k.armed = app.armedTracks.count(r.id) != 0;
        k.naming = naming == Naming::Track && renaming == r.id;
        k.dropTarget = drop.ok && !drop.event && drop.track == r.id && (drop.kind == DropKind::AudioFx || drop.kind == DropKind::VideoFx);
        for (const FxSlot& sl : tr->fx)
            if (!app.fx.error(sl.id).empty()) k.bad = true;
        if (tr->automation) {
            k.a1 = shownAutomation(*tr, "volume");
            k.a2 = shownAutomation(*tr, r.video ? "fadecolor" : "pan");
            k.a3 = shownAutomation(*tr, "mute") + 1.0;
        }
        if (headerPictures.size() > 400) headerPictures.clear();
        HeaderPicture& pic = headerPictures[r.id];
        pic.used = paintCount;
        if (pic.image.isNull() || !(pic.key == k)) {
            const QSize size((int)std::ceil(d1.x() - ix - 1e-9), (int)std::ceil(d1.y() - iy - 1e-9));
            if (size.width() <= 0 || size.height() <= 0 || size.width() > 4000 || size.height() > 4000) {
                p.save();
                draw(p);
                p.restore();
                return;
            }
            pic.key = k;
            pic.image = QImage(size, QImage::Format_ARGB32_Premultiplied);
            pic.image.fill(under);
            QPainter ip(&pic.image);
            ip.setTransform(QTransform(t.m11(), 0, 0, t.m22(), t.dx() - ix, t.dy() - iy));
            ip.setClipRect(all);
            ip.setFont(p.font());
            ip.setPen(p.pen());
            ip.setBrush(p.brush());
            ip.setRenderHints(p.renderHints());
            draw(ip);
            ip.end();
            pic.image.setDevicePixelRatio(t.m11());
        }
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, false);
        p.drawImage(QPointF((ix - t.dx()) / t.m11(), (iy - t.dy()) / t.m22()), pic.image);
        p.restore();
    }

    QString selectionLengthText(double length) const;
    static QString sizingText(QString text);
    void paintHeaders(QPainter& p, const QRect& clip);
    void paintRuler(QPainter& p, const QRect& clip);
    void paintCursor(QPainter& p, const QRect& clip);

    QString timeFormatName() const
    {
        const ProjectSettings& s = app.project().settings;
        return QString::fromStdString(timeFormatTitle(s, s.rulerFormat));
    }

    void paintCorners(QPainter& p, const QRect& clip);
    QString tipAt(const QPointF& pos) const;

    void setCurrentTrack(uint64_t id);
    void userCursor(double t);
    Mover moverOf(const Project& p, int track, const Event& e) const;
    int trackIndex(uint64_t id) const;
    void press(QMouseEvent* ev);
    bool pressRight(QMouseEvent* ev, const QPointF& pos);
    void selectTracksForPress(const Hit& h, Qt::KeyboardModifiers mods);
    void pressRuler(const QPointF& pos, Qt::KeyboardModifiers mods);
    void pressLoopStrip(const QPointF& pos, Qt::KeyboardModifiers mods);
    void pressMarkerTag(const Hit& h, Qt::KeyboardModifiers mods);
    void pressLane(const Hit& h, Qt::KeyboardModifiers mods);
    void pressSlipOrSlide(const Hit& h, bool ctrl, bool shift);
    void pressEdge(const Hit& h, Qt::KeyboardModifiers mods);
    void pressSnapFlag(const Hit& h);
    void pressMuteSolo(const Hit& h, bool ctrl);
    void pressInvertPhase(const Hit& h);
    Qt::KeyboardModifiers toolModifiers(Qt::KeyboardModifiers mods, bool edge) const;

    // ---- the Shuffle tool (VEGAS's Shuffle Events): the dragged event changes places with the events between it and
    // where it is dropped. It can go to the edit points on its track: dropped after events, they move up (earlier) by
    // its length and it goes after them; dropped before, they move down and it goes before. The events grouped with
    // it on other tracks are shuffled the same way.
    bool beginShuffle();
    static bool applyShuffle(Project& p, const std::set<uint64_t>& tracks, double from, double to, double place);
    void updateShuffle(const QPointF& pos);
    std::set<uint64_t> withGroup(uint64_t id) const;
    void selectGroupOf(uint64_t id) { app.setSelection(withGroup(id)); }

    void beginScrub(const QPointF& pos, Qt::KeyboardModifiers mods);
    void pressEvent(const Hit& h, Qt::KeyboardModifiers mods);

    uint64_t clickAnchor = 0;           // the event clicked last (Shift+click selects from it)

    void toggleMinimized(uint64_t trackId);
    void toggleMaximized(uint64_t trackId);

    // ---- VEGAS's track automation: the Automation Settings button, the automation modes, the controls in
    // automation mode and recording automation while playing
    const Track* findTrackConst(uint64_t id) const;
    std::vector<uint64_t> controlTracks(uint64_t id) const;
    static double& autoValueOf(Track& t, const std::string& kind);
    static void setEnvelopeAt(Envelope& e, double at, double v, double frame, const std::string& kind);
    static Envelope& ensureTrackEnvelope(Track& t, const std::string& kind);
    static std::string applyAutomation(Project& p, const std::vector<uint64_t>& ids, const std::string& kind, double value, double at, double frame);
    double shownAutomation(const Track& t, const std::string& kind) const
    {
        const std::string k = (kind == "volume" && t.type == TrackType::Video) ? std::string("composite") : kind;
        return trackAutomationValue(t, k, app.cursor());
    }
    bool headersFollowCursor() const;
    void toggleAutomation(uint64_t id);
    void setAutomationMode(uint64_t id, const std::string& mode);
    void automationMenu(QMenu& m, uint64_t id);
    void automationModes(QMenu& m, uint64_t id);
    void setLayersExpanded(uint64_t id, bool on);

    // ---- the hamburger buttons (VEGAS 18)
    // A track header's hamburger menu: the commands of the header's buttons (VEGAS lists those not shown; here all,
    // with their ticks), then Edit Visible Button Set. A video track: Bypass Motion Blur, Track Motion, Track FX,
    // Activate Automation Controls, Automation Settings, Compositing Mode, Make Compositing Parent / Child, Minimize,
    // Maximize. An audio track: Arm for Record, Invert Track Phase, Track FX, Activate Automation Controls,
    // Automation Settings, Minimize, Maximize.
    void trackHamburger(QMenu& menu, uint64_t id);
    void setButtonHidden(bool video, const QString& key, bool hidden);
    static std::vector<std::pair<QString, QString>> headerButtonList(bool video);
    void editVisibleButtons(bool video);
    std::function<void(QDialog*)> visibleButtonsHook;

    void eventHamburger(QMenu& menu, uint64_t id);

    bool recordingWanted(uint64_t id) const
    {
        const Track* t = findTrackConst(id);
        return t && t->automation && (t->automationMode == "touch" || t->automationMode == "latch") && playback && playback->playing();
    }
    void recordValue(const std::vector<uint64_t>& ids, const std::string& kind, double value);
    void recPoint(double t);
    void recApply(bool thin);
    void recTick();
    void finishRecording();
    void beginSlider(const Hit& h, const QPointF& pos, bool pan);
    void updateSlider(const QPointF& pos, Qt::KeyboardModifiers mods);
    void updateSurroundPoint(const QPointF& pos);
    void scrub(const QPointF& pos, Qt::KeyboardModifiers mods);

    // Preferences > Reno > Editing, "Scrub audio": while the cursor is dragged along the ruler a short piece of the
    // sound there is played now and then (Cursor Preview, this long, this loud), so a place can be found by ear.
    qint64 scrubSoundAt = 0;
    static int scrubPieceMs() { return std::max(20, std::min(2000, vprefs::number("prefs/scrubAudioMs"))); }
    void scrubSound();
    void scrubSoundDone(bool resume);
    void updateLoop(const QPointF& pos, Qt::KeyboardModifiers mods);
    bool beginMove();
    void updateMove(const QPointF& pos, Qt::KeyboardModifiers mods);

    double moverSnapOffset(const Mover& m) const
    {
        const Event* e = app.project().findEvent(m.id);
        return e ? clampd(e->snapOffset, 0.0, m.length) : 0.0;
    }

    void pressGain(const Hit& h, Qt::KeyboardModifiers, bool right);
    void updateGain(const QPointF& pos, Qt::KeyboardModifiers mods);
    bool beginEdge();
    void updateEdge(const QPointF& pos, Qt::KeyboardModifiers mods);
    void dragTo(const QPointF& pos, Qt::KeyboardModifiers mods);
    void edgePreview(double t);
    void endEdgePreview();
    bool beginBody();
    void updateBody(const QPointF& pos, Qt::KeyboardModifiers);
    bool beginEdgeOp();
    void updateEdgeOp(const QPointF& pos, Qt::KeyboardModifiers mods);
    void pressSplitTrim(const Hit& h);

    void pressEnvelope(const Hit& h, Qt::KeyboardModifiers mods);
    void updateSnapOffset(const QPointF& pos, Qt::KeyboardModifiers mods);
    void pressEnvelopeInSelection(const EnvLane& L, const Hit& h);
    void updateEnvelope(const QPointF& pos, Qt::KeyboardModifiers mods);
    void updateEnvBand(const QPointF& pos, Qt::KeyboardModifiers mods);
    void addEnvelopePoint(const EnvelopeRef& ref, double timelineT, Qt::KeyboardModifiers mods);
    bool deleteEnvelopePoints();
    void validateEnvSelection();

    // ---- VEGAS's envelope commands: Thin, Flip Selected Points, the envelope brush, copying and pasting points
    // The points a command on 'ref' works on: those in the time selection (if there is one), else the selected ones.
    std::set<int> envCommandPoints(const EnvelopeRef& ref) const;
    static std::vector<EnvPoint> thinRun(const std::string& kind, const std::vector<EnvPoint>& pts, size_t a, size_t b, double tol);
    void thinEnvelope(const EnvelopeRef& ref, bool selectedOnly);
    void flipEnvelopePoints(const EnvelopeRef& ref);
    void pressEnvBrush(const Hit& h);
    std::vector<EnvPoint> brushedPoints(bool thin) const;
    void updateEnvBrush(const QPointF& pos);
    void endEnvBrush(const DragState& was);
    bool copyEnvelopePoints();
    bool pasteEnvelopePoints();

    void setMarkerTool(bool on);
    static int markerBar(const Project::Marker& m) { return m.kind == "command" ? 2 : (m.kind == "cdtrack" || m.kind == "cdindex") ? 1 : 0; }
    void chooseMarker(uint64_t id, Qt::KeyboardModifiers mods);
    bool markerToolKey(int dir, bool extend);
    void deleteChosenMarkers();
    void updateMarker(const QPointF& pos, Qt::KeyboardModifiers mods);
    void updateRubberBand(const QPointF& pos, Qt::KeyboardModifiers);
    QRegion hotRegion(const Hit& h) const;
    void move(QMouseEvent* ev);
    void setPointer(const Hit& h, Qt::KeyboardModifiers mods);
    int ownCursor = -1;

    void release(QMouseEvent* ev);
    QRegion dragVisualsOf(const DragState& d) const;
    void releaseRight(QMouseEvent* ev);
    bool cancelMouseDrag();
    void autoScrollTick();
    void doubleClick(QMouseEvent* ev);
    void wheel(QWheelEvent* ev);

    void keyCursor(double t);
    double stepFrom(double t, int dir) const;

    double homeTime() const { return app.loopEnd > app.loopStart ? app.loopStart : viewStart; }
    double endTime() const { return app.loopEnd > app.loopStart ? app.loopEnd : timeAtF(lanesRight() - 1); }

    bool key(QKeyEvent* ev);

    bool namedStillThere() const;
    void placeNameEdit();
    void startRename(Naming kind, uint64_t id);
    void endRename(bool keep);

    std::set<uint64_t> withGroups(const std::set<uint64_t>& ids) const;

    static double mediaDuration(const Project& p, const Event& e)
    {
        const MediaItem* m = p.findMedia(e.mediaId);
        return (m && !m->info.isStill && !m->generated) ? m->info.duration : 0.0;
    }

    static void trimStartTo(const Project& p, Event& e, double t, double fr);
    static void trimEndTo(Event& e, double t, double fr);
    void removeSelection(bool cut);
    void pasteInsert();
    void trimStart();
    void trimEnd();
    void trimToSelection();

    // ---- VEGAS's sync links: a linked event goes along when the event it is linked to moves, not the other way
    // Where the events that others are linked to start now (before an edit that may move them).
    static std::map<uint64_t, double> syncMemo(const Project& p);
    static void followSyncLinks(Project& p, const std::map<uint64_t, double>& before, const std::set<uint64_t>& moving);
    std::set<uint64_t> syncLinkGroup(uint64_t id) const;
    void createSyncLink(uint64_t head);
    void removeSyncLink(uint64_t head);
    void nudge(int frames, int pixels, int tracks);
    void insertMarker();
    void nameNewMarker(uint64_t id);
    void insertRegion();
    void goToMarker(int number);

    enum class EdgeKind { Markers, Events, EditPoints, Selected };

    void cursorToEdge(int dir, EdgeKind kind);
    void timeSelectSelected(int dir);
    void renameSelected();
    static int heightStep(int h, int dir);
    void changeTrackHeights(int dir);

    // ---- the options kept between runs (QSettings "timeline/..."), as VEGAS keeps its switches: saved each time
    // they change (optionsChanged), read when the timeline is made. Snap to Grid is Preferences' "Snap to the ruler's
    // marks as well" (prefs/snapToGrid), one switch in two places. Not kept: the edit tool and multicamera editing.
    struct OptionKey { const char* key; bool TimelineOptions::*field; };
    static const std::vector<OptionKey>& optionKeys();
    void loadOptions();
    void saveOptions();

    void pressTrackEdge(const Hit& h);
    void updateTrackEdge(const QPointF& pos);
    void clearGroups();
    void selectGroups();

    enum class EdgeOp { Trim, Stretch, SlipTrim, Slide };

    // The events an edge operation changes, as they were before it, so it can be done again from there for every step of
    // a drag.
    struct EdgeWork {
        EdgeOp op = EdgeOp::Trim;
        int side = 1;
        std::vector<Mover> parts;
        std::vector<Mover> others;
        Followers followers;
        bool ripple = false;
        std::set<uint64_t> ids() const
        {
            std::set<uint64_t> s;
            for (const Mover& m : parts) s.insert(m.id);
            for (const Mover& m : others) s.insert(m.id);
            return s;
        }
    };

    static const Event* neighbourOf(const Track& t, const Event& e, int side, double fr);
    bool planEdge(const std::vector<uint64_t>& ids, int side, EdgeOp op, bool ignoreGroup, EdgeWork& w, std::string* why) const;
    double clampEdge(const EdgeWork& w, double d) const;

    static void shiftTakes(Event& e, double mediaDelta)
    {
        for (Take& t : e.takes) t.offset = std::max(0.0, t.offset + mediaDelta);
    }

    static double wrapOffset(const Mover& m, double offset);
    static void trimLeftBy(Event& e, const Mover& m, double d);

    static void clampFades(Event& e)
    {
        e.fadeIn = std::min(e.fadeIn, e.length);
        e.fadeOut = std::min(e.fadeOut, std::max(0.0, e.length - e.fadeIn));
    }

    void applyEdge(Project& p, const EdgeWork& w, double d) const;
    double edgeStep(const std::vector<uint64_t>& ids, int side, EdgeOp op, bool ignoreGroup, double delta);

    // Slip (the media moves inside the events, which stay put) or slide (the events move, their media stays where it was
    // on the timeline) of the selected events and those grouped with them.
    struct BodyWork {
        bool slide = false;
        std::vector<Mover> parts;
    };

    EdgeWork dragEdge;
    BodyWork dragBody;

    bool planBody(const std::set<uint64_t>& ids, bool slide, bool ignoreGroup, BodyWork& w, std::string* why) const;
    double clampBody(const BodyWork& w, double d) const;
    void applyBody(Project& p, const BodyWork& w, double d) const;

    bool edgeValid() const
    {
        if (!edgeSel.event || !app.project().findEvent(edgeSel.event)) return false;
        return !edgeSel.expanded || app.project().findEvent(edgeSel.other);
    }

    void setEdge(uint64_t id, int side);
    void selectEdge(int dir);
    void toggleExpandedEdit();
    void exitEditMode();
    void keypadEdit(int frames, int pixels, Qt::KeyboardModifiers mods);
    void jog(int dir);
    bool jogging = false;

    void rememberEdit(const std::set<uint64_t>& tracks, double from, double delta, const std::set<uint64_t>& exclude);
    void rememberRemoval(const std::map<uint64_t, Ranges>& removed);
    void postEditRipple(RippleMode mode);

    // ---- more commands
    // Splits an event of p at t (a new event for the part after it, which is returned: 0 if t is not inside it).
    uint64_t splitIn(Project& p, uint64_t id, double t, std::map<uint64_t, uint64_t>* groups = nullptr) const;
    static std::pair<std::string, std::string> transitionNamed(const char* name);
    void smartSplit();
    std::set<uint64_t> commandTracks() const;
    void convertCutToTransition(int kind);
    void convertTransitionToCut();

    void selectLoopRegion()
    {
        if (lastLoopB > lastLoopA) app.setLoop(lastLoopA, lastLoopB);
        else app.status("There is no loop region yet: drag across the bar above the ruler to make one.");
    }

    void setLocked(bool locked)
    {
        if (app.selection().empty()) { app.status("Select the events to lock first."); return; }
        app.setLocked(app.selection(), locked);
    }

    void addMarkerOfKind(const char* kind, double at, double length, const std::string& command, const std::string& name, const char* what);
    void insertCdTrack();
    void insertCdIndex();
    void cursorToCd(int dir, bool tracks);
    void editCommandMarker(uint64_t id);

    void insertCommandMarker(const std::string& command, const std::string& parameter)
    {
        addMarkerOfKind("command", quantized(app.cursor()), 0, command, parameter, "Insert Command Marker");
    }

    void rebuildPeaks();
    void addTakesToSelected(const std::vector<uint64_t>& media);

    bool wheelScrolling = false;
    QPointF wheelScrollFrom, wheelScrollTo;
    QTimer* wheelScrollTimer = nullptr;

    void beginWheelScroll(const QPointF& at);
    void wheelScrollTick();
    void endWheelScroll();

    void toggleTrackList();
    std::set<uint64_t> selectedTracks() const;

    // ---- VEGAS's track Cut / Copy / Paste / Delete (a track header's menu): whole tracks, with their events, effects,
    // envelopes and settings, onto a clipboard of the timeline's own (events have theirs: Ctrl+C)
    std::vector<Track> trackClip;

    std::vector<uint64_t> selectedTracksInOrder() const;
    void copyTracks(bool cut);
    bool confirmTrackDelete(const std::vector<uint64_t>& ids);
    void deleteTracks(const std::vector<uint64_t>& ids, const char* what);
    void pasteTracks(uint64_t after);

    void changeTrackLevel(int dir, bool pan) { changeLevelOf(selectedTracks(), dir, pan, false); }

    void changeLevelOf(const std::set<uint64_t>& ids, int dir, bool pan, bool fine);
    void groupTracks(bool group);
    uint64_t currentTrackGroup() const;
    void collapseTrackGroup(bool collapse);
    void muteSoloTrackGroup(bool solo, bool exclusive);

    void createMulticamTrack();
    const Track* multicamTrack() const;
    bool multicamCut(int take, bool crossfade);

    static QAction* add(QMenu& menu, const QString& text, std::function<void()> fn, bool enabled = true);
    void insertTrack(TrackType type, int row);
    void contextMenu(QContextMenuEvent* ev);

    // ---- track keyframes: a video track's track-level keyframes (Track Motion, Parent Motion, 2D Shadow, 2D Glow)
    // are shown in a strip along the bottom of its lane, all kinds on one row (keyframes at the same time together,
    // as in VEGAS's collapsed view). Drag to move in time, double-click for the Track Motion window, right-click for
    // interpolation or delete.
    static constexpr int kTrackKeyBand = 11;
    std::map<double, int> trackKeyTimes(const Track& t) const;
    bool showsTrackKeys(const Row& r, const Track& t) const
    {
        return r.video && r.h >= 30 && (t.motion.size() + t.parentMotion.size() + t.shadow2d.size() + t.glow2d.size()) >= 2;
    }
    double trackKeyY(const Row& r) const { return rowTop(r) + r.h - 2 - kTrackKeyBand / 2.0; }
    void paintTrackKeys(QPainter& p, const Row& r, const Track& t, const QRect& c);
    bool hitTrackKey(const QPointF& pos, const Row& r, const Track& t, Hit& h) const;
    void dragTrackKeys(const QPointF& pos);
    void trackKeyMenu(QMenu& menu, const Hit& h);

    // ---- envelope menus
    // "Insert/Remove Envelope": what 'o' can have, ticked where it has it; choosing one puts it on or takes it off.
    void addEnvelopeChoices(QMenu* sub, const EnvelopeOwner& o);
    void envelopeMenu(QMenu& menu, const Hit& h, double t);
    void pickFadeColor(const EnvelopeOwner& o, bool top);
    void fxWindowsMenu(QMenu& menu, uint64_t id);

    // ---- the right-click menus, in VEGAS Pro 18's order and with its names (its menu resources)
    // A command of the main window (its QAction, by object name), shown as it is there with its key, ticked or
    // greyed. Left out when there is no main window.
    QAction* command(QMenu& menu, const char* name, const QString& text = QString());
    QAction* toggleItem(QMenu& menu, const QString& text, bool on, std::function<void()> fn, bool enabled = true);
    void busMenu(QMenu& menu, const EnvelopeOwner& o);
    bool showMenu(const Hit& h, const QPoint& globalPos, double time = -1);
    bool fillMenu(QMenu& menu, const Hit& hitIn, double time);

    void freezeFrameAtCursor(uint64_t id);
    uint64_t syncPartner(uint64_t id) const;
    void synchronize(uint64_t id, uint64_t partner, bool byMoving);

    // ---- time formats (VEGAS's Time Format menus: menu resources 1004 and 1012)
    // The formats, ticked where the project is; 'frameRates': VEGAS's Use Media / Use Project Frame Rate first (the
    // timeline always counts frames at the project's rate); 'setTime': Set Time at Cursor after them.
    void timeFormatItems(QMenu& m, bool frameRates, bool setTime);

    void setTimeFormat(const std::string& id)
    {
        if (app.project().settings.rulerFormat != id) app.edit("Ruler Format", [&](Project& p) { p.settings.rulerFormat = id; });
        QSettings().setValue("options/rulerFormat", QString::fromStdString(id));
    }

    QString timeDisplayText() const;
    void timeDisplayMenu(QMenu& menu);
    void setTimeDisplayColor(bool text, const QColor& c);
    void gridSpacingItems(QMenu& m);
    void deleteMarkers(bool inSelection);
    bool markerMenu(QMenu& menu, const Hit& h);
    void autoDuckingMenu(QMenu& menu, uint64_t id);
    bool panTypeMenu(QMenu& menu, uint64_t id);
    void busChoiceMenu(QMenu& menu, uint64_t id);
    void trackMenu(QMenu& menu, const Hit& h, bool lane, double time);
    bool eventMenu(QMenu& menu, const Hit& h, double time);
    void eventSwitchesMenu(QMenu& menu, const Event& e, bool video, const std::set<uint64_t>& sel);
    void eventTakeMenu(QMenu& menu, const Event& e, uint64_t id, const std::set<uint64_t>& sel);
    void eventGroupMenu(QMenu& menu, const std::set<uint64_t>& sel, bool grouped);
    void eventSyncLinkMenu(QMenu& menu, const Event& e, uint64_t id, const std::set<uint64_t>& sel);
    void eventSynchronizeMenu(QMenu& menu, uint64_t id);
    void eventChannelsMenu(QMenu& menu, const Event& e, const std::set<uint64_t>& sel);
    void eventRateMenu(QMenu& menu, const Event& e, uint64_t id, const std::set<uint64_t>& sel, bool transition);
    void openEventFx(uint64_t id);
    void addFadeTypes(QMenu& menu, const Track& tr, const Event& e, double time);

    EnvelopeRef trackEnvelopeRef(const std::string& kind) const;
    void toggleTrackEnvelope(const std::string& kind);

    void removeTrackEnvelope(const std::string& kind)
    {
        const EnvelopeRef r = trackEnvelopeRef(kind);
        if (r.valid()) app.removeEnvelope(r);
    }

    void cycleEnvelope(int dir);
    void fillEnvelopeMenu(QMenu* menu, bool video);

    static DropKind dropKindOf(const QMimeData* md);
    std::vector<Ghost> planMedia(const MediaItem& m, double time, uint64_t hint) const;
    double mediaLength(const MediaItem& m) const;
    void planDrop(const QMimeData* md, const QPointF& pos, Qt::KeyboardModifiers mods);

    // Preferences > Editing's "Automatically overlap multiple selected media when added" (with Automatic Crossfades on):
    // how much the next of several files dropped together overlaps one this long.
    double autoOverlap(double len) const
    {
        if (!app.prefs.autoOverlapMedia || !app.autoCrossfades()) return 0.0;
        return std::max(0.0, std::min(app.prefs.cutToOverlap, len * 0.5));
    }

    void overlapAdded(const std::vector<std::vector<uint64_t>>& each);
    std::vector<uint64_t> dropMediaIds(const DropState& dr, const std::vector<QUrl>& urls);
    std::set<uint64_t> addMediaAs(const std::vector<uint64_t>& media, double time, uint64_t track, int mode, int streams, uint64_t ontoEvent);
    void rightDropMenu(const DropState& dr, const std::vector<QUrl>& urls, const QPointF& pos, uint64_t ontoEvent);
    void dragEnter(QDragEnterEvent* ev);
    void dragMove(QDragMoveEvent* ev);
    void dragLeave();
    void dropped(QDropEvent* ev);
};

}  // namespace app
}  // namespace reno
