// VEGAS's Set Default Track Properties (see trackdefaults.h).
#include "trackdefaults.h"
#include "vprefs.h"
#include "appstate.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QSettings>
#include <QVBoxLayout>

namespace reno {
namespace app {

namespace {

QString key(bool video, const char* what)
{
    return QString("trackDefaults/%1/%2").arg(video ? "video" : "audio", what);
}

std::string trackJson(const Track& t)
{
    Project p;
    Track copy = t;
    copy.events.clear();
    copy.sends.clear();
    copy.envelopes = Envelopes();
    copy.veg.reset();
    for (FxSlot& s : copy.fx) s.veg.reset();
    p.tracks.push_back(copy);
    return p.toJson().dump();
}

bool trackFromJson(const std::string& text, Track& out)
{
    Json j;
    Project p;
    if (!Json::parse(text, j) || !p.fromJson(j) || p.tracks.size() != 1) return false;
    out = p.tracks[0];
    return true;
}

void applyDefaults(Project& p, Track& t)
{
    const bool video = t.type == TrackType::Video;
    if (video) {
        auto argb = [](const QColor& c) { return ((uint32_t)c.alpha() << 24) | ((uint32_t)c.red() << 16) | ((uint32_t)c.green() << 8) | (uint32_t)c.blue(); };
        const QColor top = vprefs::color("prefs/fadeTop", Qt::white), bottom = vprefs::color("prefs/fadeBottom", Qt::black);
        t.fadeTop = argb(top);
        t.fadeBottom = argb(bottom);
    }
    if (const int h = vprefs::number(video ? "prefs/newVideoTrackHeight" : "prefs/newAudioTrackHeight")) t.height = std::max(16, std::min(2000, h));
    QSettings s;
    const QStringList props = s.value(key(video, "properties")).toStringList();
    if (props.isEmpty()) return;
    Track saved;
    if (!trackFromJson(s.value(key(video, "track")).toString().toStdString(), saved) || saved.type != t.type) return;
    if (props.contains("volume") && !video) t.level = saved.level;
    if (props.contains("height")) t.height = saved.height;
    if (props.contains("composite") && video) t.composite = saved.composite;
    if (props.contains("pantype") && !video) t.panType = saved.panType;
    if (props.contains("automation")) {
        t.automation = saved.automation;
        t.automationMode = saved.automationMode;
    }
    if (props.contains("fx")) {
        t.fx = saved.fx;
        for (FxSlot& slot : t.fx) {
            slot.id = p.newId();
            slot.veg.reset();
        }
    }
}

const Track* trackOf(uint64_t id)
{
    for (const Track& t : AppState::get().project().tracks)
        if (t.id == id) return &t;
    return nullptr;
}

}  // namespace

QStringList savedTrackDefaults(bool video)
{
    return QSettings().value(key(video, "properties")).toStringList();
}

void saveTrackDefaults(uint64_t trackId, const QStringList& properties)
{
    const Track* t = trackOf(trackId);
    if (!t) return;
    const bool video = t->type == TrackType::Video;
    QSettings s;
    if (properties.isEmpty()) {
        s.remove(key(video, "properties"));
        s.remove(key(video, "track"));
        return;
    }
    s.setValue(key(video, "properties"), properties);
    s.setValue(key(video, "track"), QString::fromStdString(trackJson(*t)));
}

void installTrackDefaults()
{
    AppState::get().onNewTrack = [](Project& p, Track& t) { applyDefaults(p, t); };
}

bool runSetDefaultTrackProperties(QWidget* parent, uint64_t trackId)
{
    const Track* t = trackOf(trackId);
    if (!t) return false;
    const bool video = t->type == TrackType::Video;
    QDialog d(parent);
    d.setObjectName("setDefaultTrackProperties");
    d.setWindowTitle("Set Default Track Properties");
    QVBoxLayout* v = new QVBoxLayout(&d);
    QLabel* intro = new QLabel("Select the properties to use as future defaults for new tracks. These properties will default to the "
                               "values of the current track.", &d);
    intro->setWordWrap(true);
    v->addWidget(intro);
    QGridLayout* g = new QGridLayout();
    g->addWidget(new QLabel("Audio track properties:", &d), 0, 0);
    g->addWidget(new QLabel("Video track properties:", &d), 0, 1);
    const QStringList had = savedTrackDefaults(video);
    struct Box { const char* text; const char* id; bool forVideo; bool works; };
    const Box boxes[] = {
        {"&Volume", "volume", false, true},          {"&Pan type", "pantype", false, true}, {"&Height", "height", false, true},
        {"Track &FX", "fx", false, true},            {"&Input monitor", "monitor", false, false}, {"&Automation mode", "automation", false, true},
        {"&Composite mode", "composite", true, true}, {"H&eight", "height", true, true},     {"Track F&X", "fx", true, true},
        {"Automation &mode", "automation", true, true}};
    std::vector<std::pair<QCheckBox*, QString>> mine;
    int rows[2] = {1, 1};
    for (const Box& b : boxes) {
        QCheckBox* c = new QCheckBox(b.text, &d);
        c->setObjectName(QString(b.forVideo ? "video_" : "audio_") + b.id);
        const bool active = b.forVideo == video && b.works;
        c->setEnabled(active);
        if (!b.works) c->setToolTip("Reno has no such track setting");
        if (active) {
            c->setChecked(had.isEmpty() || had.contains(b.id));
            mine.emplace_back(c, QString(b.id));
        }
        g->addWidget(c, rows[b.forVideo ? 1 : 0]++, b.forVideo ? 1 : 0);
    }
    v->addLayout(g);
    QFrame* line = new QFrame(&d);
    line->setFrameShape(QFrame::HLine);
    v->addWidget(line);
    QHBoxLayout* bottom = new QHBoxLayout();
    QCheckBox* restore = new QCheckBox("&Restore original defaults", &d);
    restore->setObjectName("restoreDefaults");
    bottom->addWidget(restore, 1);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    bottom->addWidget(bb);
    v->addLayout(bottom);
    QObject::connect(restore, &QCheckBox::toggled, &d, [mine](bool on) {
        for (const auto& m : mine) m.first->setEnabled(!on);
    });
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return false;
    QStringList props;
    if (!restore->isChecked())
        for (const auto& m : mine)
            if (m.first->isChecked()) props << m.second;
    saveTrackDefaults(trackId, props);
    AppState::get().status(props.isEmpty() ? std::string("New tracks start with Reno's original defaults.")
                                           : std::string("New ") + (video ? "video" : "audio") + " tracks start with this track's " +
                                                 props.join(", ").toStdString() + ".");
    return true;
}

void addTrackDefaultsItem(QMenu* menu, uint64_t trackId, QWidget* parent)
{
    if (!menu) return;
    QAction* a = new QAction("Set Default Track Properties...", menu);
    a->setObjectName("setDefaultTrackProperties");
    QObject::connect(a, &QAction::triggered, menu, [trackId, parent] { runSetDefaultTrackProperties(parent, trackId); });
    menu->addSeparator();
    menu->addAction(a);
}

}  // namespace app
}  // namespace reno
