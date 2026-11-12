// The sheets the touch interface's tools open. Changes are AppState edits: a slider's whole drag is one undo step
// (AppState's drag steps for project values, beginFxEdit / endFxEdit for effect settings).
#include "touch_shell_internal.h"
#include "mobilefiles.h"
#include "timeline.h"
#include "../engine/catalog.h"
#include "../ofx/builtins.h"
#include "../ofx/ofxhost.h"
#include <QAction>
#include <QColorDialog>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputMethod>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>

namespace reno {
namespace app {

using namespace touch;

namespace {

double dbOf(double gain) { return gain <= 1e-6 ? -60.0 : std::max(-60.0, 20.0 * std::log10(gain)); }
double gainOf(double db) { return db <= -59.95 ? 0.0 : std::pow(10.0, db / 20.0); }

QString plain(const QString& menuText)
{
    QString t = menuText;
    t.remove(QRegularExpression("&(?!&)"));
    t.replace("&&", "&");
    const int tab = t.indexOf('\t');
    if (tab >= 0) t = t.left(tab);
    if (t.endsWith("...")) t.chop(3);
    return t;
}

// The selected events and those grouped with them (a clip's picture and its sound change together).
std::set<uint64_t> withGroups(const Project& p, const std::set<uint64_t>& ids)
{
    std::set<uint64_t> groups;
    for (uint64_t id : ids)
        if (const Event* e = p.findEvent(id))
            if (e->group) groups.insert(e->group);
    std::set<uint64_t> out = ids;
    if (groups.empty()) return out;
    for (const Track& t : p.tracks)
        for (const Event& e : t.events)
            if (e.group && groups.count(e.group)) out.insert(e.id);
    return out;
}

std::string titlesId()
{
    std::string fallback;
    for (const VideoFxEntry& v : Catalog::get().video()) {
        if (!v.generator || !v.ok) continue;
        if (v.id == "com.vegascreativesoftware:titlesandtext" || v.label.find("Titles & Text") != std::string::npos) return v.id;
        if (fallback.empty() && v.label.find("Text") != std::string::npos) fallback = v.id;
    }
    return fallback;
}

// The text a Titles & Text generator shows (its "Text" setting, rich text in VEGAS's projects).
QString generatorText(const MediaItem& m)
{
    const Json params = m.generator.state.get("params");
    std::string s = params.isObject() ? params.get("Text").get("s").asString(std::string()) : std::string();
    if (s.compare(0, 5, "{\\rtf") == 0) s = ofx::parseRichText(s).text;
    return QString::fromStdString(s);
}

}  // namespace

Sheet* TouchShellImpl::newSheet(const QString& title)
{
    Sheet* s = new Sheet(title, q);
    s->hide();
    return s;
}

namespace {

Button* row(Sheet* s, Icon icon, const QString& text, std::function<void()> fn, const QString& subtitle = QString())
{
    Button* b = new Button(Button::Row, icon, text, s);
    if (!subtitle.isEmpty()) b->setSubtitle(subtitle);
    QObject::connect(b, &QAbstractButton::clicked, s, [fn] { fn(); });
    s->body()->addWidget(b);
    return b;
}

QWidget* padded(QWidget* w, QWidget* parent, int side = 16)
{
    QWidget* box = new QWidget(parent);
    auto* h = new QHBoxLayout(box);
    h->setContentsMargins(side, 8, side, 8);
    h->addWidget(w);
    return box;
}

}  // namespace

// ---------------------------------------------------------------- project

void TouchShellImpl::projectMenu()
{
    Sheet* s = newSheet("Project");
    auto later = [this](std::function<void()> fn) {
        q->closeSheet();
        QTimer::singleShot(180, q, fn);
    };
    row(s, Icon::Add, "New project", [this, later] { later([this] { runAction("newPlain"); }); });
    row(s, Icon::Folder, "Open project", [this, later] { later([this] { runAction("open"); }); });
    row(s, Icon::Save, "Save", [this, later] { later([this] { runAction("save"); }); });
    row(s, Icon::Save, "Save as", [this, later] { later([this] { runAction("saveAs"); }); });
    row(s, Icon::Export, "Render as", [this, later] { later([this] { runAction("render"); }); }, "Every format and template");
    row(s, Icon::Settings, "Project properties", [this, later] { later([this] { runAction("properties"); }); },
        QString("%1 x %2, %3 fps").arg(app.project().settings.width).arg(app.project().settings.height).arg(app.project().settings.fps(), 0, 'f', 3));
    row(s, Icon::Settings, "Preferences", [this, later] { later([this] { runAction("preferences"); }); });
    row(s, Icon::Search, "All commands", [this] { commandsSheet(); }, "Every command of the desktop menus");
    row(s, Icon::Desktop, "Desktop interface", [this, later] { later([this] { if (q->onLeave) q->onLeave(); }); }, "VEGAS's menus and windows");
    const QStringList recent = QSettings().value("recentProjects").toStringList();
    if (!recent.isEmpty()) {
        s->body()->addWidget(sectionLabel("Recent", s));
        for (int k = 0; k < std::min<int>(6, recent.size()); ++k) {
            const QString f = recent[k];
            row(s, Icon::Folder, QFileInfo(f).completeBaseName(), [this, later, f] {
                later([this, f] { if (mw->maybeSaveChanges()) mw->openProject(f); });
            }, QDir::toNativeSeparators(QFileInfo(f).path()));
        }
    }
    q->openSheet(s);
}

// ---------------------------------------------------------------- adding

void TouchShellImpl::addMedia(bool audioOnly)
{
    q->closeSheet();
    QSettings st;
    QString dir = st.value("touch/mediaDir").toString();
    if (dir.isEmpty() || !QFileInfo::exists(dir)) {
        dir = QStandardPaths::writableLocation(audioOnly ? QStandardPaths::MusicLocation : QStandardPaths::MoviesLocation);
        if (dir.isEmpty()) dir = QDir::homePath();
    }
    const QString audioTypes = "*.wav *.mp3 *.m4a *.aac *.flac *.ogg *.opus *.wma *.aif *.aiff";
    const QString filter = audioOnly ? QString("Audio (%1);;All files (*)").arg(audioTypes)
                                     : QString("Photos, videos and audio (*.mp4 *.mov *.m4v *.mkv *.avi *.webm *.mts *.m2ts *.mxf *.wmv *.png *.jpg *.jpeg "
                                               "*.gif *.bmp *.tif *.tiff *.webp *.heic *.exr *.dpx %1);;All files (*)").arg(audioTypes);
    const QStringList files = QFileDialog::getOpenFileNames(q, audioOnly ? "Add Audio" : "Add Media", dir, filter);
    if (files.isEmpty()) return;
    st.setValue("touch/mediaDir", QFileInfo(files.front()).path());
    const size_t before = app.undoCount();
    double at = app.cursor();
    std::set<uint64_t> made;
    QStringList failed;
    for (const QString& f : files) {
        const std::string path = localFileFor(f).toStdString();
        std::string err;
        const uint64_t id = app.importMedia(path, &err);
        if (!id) {
            failed << QFileInfo(f).fileName();
            continue;
        }
        // one after another from the playhead, each where the one before ends
        for (uint64_t e : app.addMediaToTimeline(id, at)) {
            made.insert(e);
            if (const Event* ev = app.project().findEvent(e)) at = std::max(at, ev->start + ev->length);
        }
    }
    if (app.undoCount() > before + 1) app.mergeUndo(app.undoCount() - before, files.size() > 1 ? "Add Media" : "Add to Timeline");
    if (!made.empty()) app.setSelection(made);
    if (!failed.isEmpty()) app.status("These files could not be read: " + failed.join(", ").toStdString());
    if (app.project().length() > 0 && timeline->timeAtX(timeline->width()) - timeline->timeAtX(kHeaderW) > app.project().length() * 4)
        timeline->zoomToFit();
}

void TouchShellImpl::addTrackSheet()
{
    Sheet* s = newSheet("Add track");
    auto add = [this](TrackType type) {
        q->closeSheet();
        // video tracks go under the other video tracks, audio tracks at the end
        int index = -1;
        if (type == TrackType::Video) {
            index = 0;
            for (const Track& t : app.project().tracks)
                if (t.type == TrackType::Video) ++index;
        }
        app.currentTrack = app.addTrack(type, index);
        app.notify(kSelection);
    };
    row(s, Icon::Media, "Video track", [add] { add(TrackType::Video); }, "Pictures, video and text");
    row(s, Icon::Music, "Audio track", [add] { add(TrackType::Audio); }, "Music, voice and sound");
    q->openSheet(s);
}

void TouchShellImpl::trackSheet(uint64_t trackId)
{
    const Project& p = app.project();
    const Track* t = nullptr;
    int number = 0;
    for (const Track& tr : p.tracks)
        if (tr.id == trackId) { t = &tr; break; }
    if (!t) return;
    for (const Track& tr : p.tracks) {
        if (tr.type == t->type) ++number;
        if (tr.id == trackId) break;
    }
    const bool video = t->type == TrackType::Video;
    Sheet* s = newSheet(t->name.empty() ? QString("%1 track %2").arg(video ? "Video" : "Audio").arg(number) : QString::fromStdString(t->name));
    QLineEdit* name = new QLineEdit(QString::fromStdString(t->name), s);
    name->setPlaceholderText("Track name");
    QObject::connect(name, &QLineEdit::editingFinished, q, [this, trackId, name] {
        const std::string n = name->text().toStdString();
        const Track* tr = nullptr;
        for (const Track& x : app.project().tracks) if (x.id == trackId) tr = &x;
        if (!tr || tr->name == n) return;
        app.edit("Rename Track", [trackId, n](Project& pr) { if (Track* x = pr.findTrack(trackId)) x->name = n; });
    });
    s->body()->addWidget(padded(name, s));
    Switch* mute = new Switch(video ? "Hide (mute)" : "Mute", s);
    mute->setChecked(t->mute);
    QObject::connect(mute, &QAbstractButton::toggled, q, [this, trackId](bool on) {
        app.edit(on ? "Mute Track" : "Unmute Track", [trackId, on](Project& pr) { if (Track* x = pr.findTrack(trackId)) x->mute = on; });
    });
    s->body()->addWidget(mute);
    Switch* solo = new Switch("Solo", s);
    solo->setChecked(t->solo);
    QObject::connect(solo, &QAbstractButton::toggled, q, [this, trackId](bool on) {
        app.edit("Solo Track", [trackId, on](Project& pr) { if (Track* x = pr.findTrack(trackId)) x->solo = on; });
    });
    s->body()->addWidget(solo);
    Slider* level = video ? new Slider("Opacity", 0, 100, t->level * 100, s) : new Slider("Volume", -60, 12, dbOf(t->level), s);
    level->setDefault(video ? 100 : 0);
    level->setFormat(video ? std::function<QString(double)>([](double v) { return QString::number(v, 'f', 0) + " %"; })
                           : std::function<QString(double)>([](double v) { return v <= -59.95 ? QString::fromUtf8("-∞ dB") : QString::number(v, 'f', 1) + " dB"; }));
    level->setStep(video ? 1 : 0.1);
    level->onBegin = [this] { app.beginDrag(); };
    level->onChange = [this, trackId, video](double v) {
        const double lv = video ? v / 100.0 : gainOf(v);
        app.dragUpdate([trackId, lv](Project& pr) { if (Track* x = pr.findTrack(trackId)) x->level = lv; });
    };
    level->onEnd = [this, video] { app.endDrag(video ? "Track Opacity" : "Track Volume"); };
    s->body()->addWidget(level);
    if (!video) {
        Slider* pan = new Slider("Pan", -100, 100, t->pan * 100, s);
        pan->setDefault(0);
        pan->setStep(1);
        pan->setFormat([](double v) { return std::fabs(v) < 0.5 ? QString("Center") : QString("%1 % %2").arg(std::fabs(v), 0, 'f', 0).arg(v < 0 ? "L" : "R"); });
        pan->onBegin = [this] { app.beginDrag(); };
        pan->onChange = [this, trackId](double v) { app.dragUpdate([trackId, v](Project& pr) { if (Track* x = pr.findTrack(trackId)) x->pan = v / 100.0; }); };
        pan->onEnd = [this] { app.endDrag("Track Pan"); };
        s->body()->addWidget(pan);
    }
    row(s, Icon::Fx, "Track effects", [this, trackId] { fxSheet({FxTarget::Track, trackId}); });
    if (video)
        row(s, Icon::Motion, "Track motion", [this, trackId] {
            q->closeSheet();
            if (mw->timeline() && mw->timeline()->onTrackMotion) mw->timeline()->onTrackMotion(trackId);
        });
    row(s, Icon::Up, "Move up", [this, trackId] {
        app.edit("Move Track", [trackId](Project& pr) {
            for (size_t k = 1; k < pr.tracks.size(); ++k)
                if (pr.tracks[k].id == trackId) { std::swap(pr.tracks[k], pr.tracks[k - 1]); break; }
        });
    });
    row(s, Icon::Down, "Move down", [this, trackId] {
        app.edit("Move Track", [trackId](Project& pr) {
            for (size_t k = 0; k + 1 < pr.tracks.size(); ++k)
                if (pr.tracks[k].id == trackId) { std::swap(pr.tracks[k], pr.tracks[k + 1]); break; }
        });
    });
    row(s, Icon::Duplicate, "Duplicate track", [this, trackId] { q->closeSheet(); app.duplicateTrack(trackId); });
    Button* del = row(s, Icon::Delete, "Delete track", [this, trackId] { q->closeSheet(); app.removeTrack(trackId); });
    del->setDanger(true);
    q->openSheet(s);
}

// ---------------------------------------------------------------- adjusting the selection

void TouchShellImpl::speedSheet()
{
    const Event* first = nullptr;
    for (uint64_t id : app.selection())
        if ((first = app.project().findEvent(id))) break;
    if (!first) return;
    Sheet* s = newSheet("Speed");
    s->modal = false;
    // 0.25x to 4x on a scale even in octaves, so 1x is in the middle
    Slider* sl = new Slider("Playback rate", 0.25, 4.0, std::fabs(first->rate), s);
    sl->setObjectName("touchSpeed");
    sl->setDefault(1.0);
    sl->setMapping([](double v) { return (std::log2(v) + 2.0) / 4.0; }, [](double f) { return std::pow(2.0, f * 4.0 - 2.0); });
    sl->setFormat([](double v) { return QString::number(v, 'f', 2) + QChar(0xD7); });
    // the event keeps the same media: faster makes it shorter, slower longer (and its picture's sound with it)
    auto apply = [](const std::set<uint64_t>& ids, double rate) {
        return [ids, rate](Project& pr) {
            const double frame = 1.0 / std::max(1.0, pr.settings.fps());
            for (uint64_t id : withGroups(pr, ids)) {
                Event* e = pr.findEvent(id);
                if (!e || e->locked) continue;
                const double was = std::max(1e-6, std::fabs(e->rate));
                const double k = was / rate;
                e->length = std::max(frame, e->length * k);
                e->fadeIn = std::min(e->length, e->fadeIn * k);
                e->fadeOut = std::min(e->length, e->fadeOut * k);
                e->rate = rate;
            }
            for (Track& t : pr.tracks) t.sortEvents();
        };
    };
    sl->onBegin = [this] { app.beginDrag(); };
    sl->onChange = [this, apply](double v) { app.dragUpdate(apply(app.selection(), v)); };
    sl->onEnd = [this] { app.endDrag("Change Speed"); };
    s->body()->addWidget(sl);
    QWidget* chips = new QWidget(s);
    auto* ch = new QHBoxLayout(chips);
    ch->setContentsMargins(12, 0, 12, 8);
    ch->setSpacing(6);
    for (double r : {0.25, 0.5, 1.0, 1.5, 2.0, 4.0}) {
        Button* b = new Button(Button::Chip, Icon::Speed, QString::number(r) + QChar(0xD7), chips);
        b->setChecked(std::fabs(std::fabs(first->rate) - r) < 1e-6);
        QObject::connect(b, &QAbstractButton::clicked, q, [this, apply, r, sl, chips, b] {
            for (QObject* o : chips->children())
                if (Button* c = dynamic_cast<Button*>(o)) c->setChecked(c == b);
            sl->setValue(r);
            app.edit("Change Speed", apply(app.selection(), r));
        });
        ch->addWidget(b);
    }
    ch->addStretch(1);
    s->body()->addWidget(chips);
    if (selectionHasAudio()) {
        Switch* pitch = new Switch("Keep the pitch of the sound", s);
        pitch->setChecked(first->keepPitch);
        QObject::connect(pitch, &QAbstractButton::toggled, q, [this](bool on) {
            const std::set<uint64_t> ids = app.selection();
            app.edit("Keep Pitch", [ids, on](Project& pr) {
                for (uint64_t id : withGroups(pr, ids))
                    if (Event* e = pr.findEvent(id)) e->keepPitch = on;
            });
        });
        s->body()->addWidget(pitch);
    }
    q->openSheet(s);
}

void TouchShellImpl::levelSheet()
{
    Sheet* s = newSheet(selectionHasVideo() && !selectionHasAudio() ? "Opacity" : selectionHasAudio() && !selectionHasVideo() ? "Volume" : "Level");
    s->modal = false;
    // a clip's picture and its sound: the opacity of the one and the volume of the other
    auto firstOf = [this](TrackType type) -> const Event* {
        for (uint64_t id : selectionWithGroups()) {
            const Track* t = nullptr;
            const Event* e = app.project().findEvent(id, &t);
            if (e && t && t->type == type) return e;
        }
        return nullptr;
    };
    // only the events of that kind in the selection (and grouped with it) change
    auto apply = [](const std::set<uint64_t>& ids, TrackType type, double gain) {
        return [ids, type, gain](Project& pr) {
            for (uint64_t id : withGroups(pr, ids)) {
                Track* t = nullptr;
                Event* e = pr.findEvent(id, &t);
                if (e && t && t->type == type) e->gain = gain;
            }
        };
    };
    if (const Event* v = firstOf(TrackType::Video)) {
        Slider* sl = new Slider("Opacity", 0, 100, v->gain * 100, s);
        sl->setObjectName("touchOpacity");
        sl->setDefault(100);
        sl->setStep(1);
        sl->setFormat([](double x) { return QString::number(x, 'f', 0) + " %"; });
        sl->onBegin = [this] { app.beginDrag(); };
        sl->onChange = [this, apply](double x) { app.dragUpdate(apply(app.selection(), TrackType::Video, x / 100.0)); };
        sl->onEnd = [this] { app.endDrag("Event Opacity"); };
        s->body()->addWidget(sl);
    }
    if (const Event* a = firstOf(TrackType::Audio)) {
        Slider* sl = new Slider("Volume", -60, 12, dbOf(a->gain), s);
        sl->setObjectName("touchVolume");
        sl->setDefault(0);
        sl->setStep(0.1);
        sl->setFormat([](double x) { return x <= -59.95 ? QString::fromUtf8("-∞ dB") : QString::number(x, 'f', 1) + " dB"; });
        sl->onBegin = [this] { app.beginDrag(); };
        sl->onChange = [this, apply](double x) { app.dragUpdate(apply(app.selection(), TrackType::Audio, gainOf(x))); };
        sl->onEnd = [this] { app.endDrag("Event Volume"); };
        s->body()->addWidget(sl);
        Switch* norm = new Switch("Normalize", s);
        norm->setChecked(false);
        for (uint64_t id : selectionWithGroups())
            if (const Event* e = app.project().findEvent(id))
                if (e->normalize) norm->setChecked(true);
        QObject::connect(norm, &QAbstractButton::toggled, q, [this](bool on) {
            std::set<uint64_t> audio;
            for (uint64_t id : selectionWithGroups()) {
                const Track* t = nullptr;
                if (app.project().findEvent(id, &t) && t && t->type == TrackType::Audio) audio.insert(id);
            }
            app.setNormalize(audio, on);
        });
        s->body()->addWidget(norm);
    }
    q->openSheet(s);
}

void TouchShellImpl::fadeSheet()
{
    const Event* e = nullptr;
    double shortest = 1e300;
    for (uint64_t id : app.selection())
        if (const Event* x = app.project().findEvent(id)) {
            if (!e) e = x;
            shortest = std::min(shortest, x->length);
        }
    if (!e) return;
    Sheet* s = newSheet("Fades");
    s->modal = false;
    const double fps = app.project().settings.fps();
    const double most = std::max(1.0 / fps, std::min(10.0, shortest));
    for (int which = 0; which < 2; ++which) {
        const bool in = which == 0;
        Slider* sl = new Slider(in ? "Fade in" : "Fade out", 0, most, std::min(most, in ? e->fadeIn : e->fadeOut), s);
        sl->setObjectName(in ? "touchFadeIn" : "touchFadeOut");
        sl->setDefault(0);
        sl->setStep(1.0 / fps);
        sl->setFormat([](double v) { return QString::number(v, 'f', 2) + " s"; });
        sl->onBegin = [this] { app.beginDrag(); };
        sl->onChange = [this, in](double v) {
            const std::set<uint64_t> ids = app.selection();
            app.dragUpdate([ids, in, v](Project& pr) {
                for (uint64_t id : withGroups(pr, ids))
                    if (Event* x = pr.findEvent(id)) (in ? x->fadeIn : x->fadeOut) = std::min(v, x->length);
            });
        };
        sl->onEnd = [this, in] { app.endDrag(in ? "Fade In" : "Fade Out"); };
        s->body()->addWidget(sl);
    }
    q->openSheet(s);
}

void TouchShellImpl::toggleOnSelection(const char* what, bool Event::*field)
{
    const std::set<uint64_t> ids = app.selection();
    bool all = !ids.empty();
    for (uint64_t id : ids)
        if (const Event* e = app.project().findEvent(id)) all = all && e->*field;
    const bool on = !all;
    app.edit(std::string(on ? "" : "Undo ") + what, [ids, field, on](Project& pr) {
        for (uint64_t id : ids)
            if (Event* e = pr.findEvent(id)) e->*field = on;
    });
}

void TouchShellImpl::duplicateSelection()
{
    // a copy right after the selection, through copy and paste (which give the copies and their effects new ids)
    const std::set<uint64_t> ids = app.selection();
    double end = 0;
    uint64_t track = 0;
    for (uint64_t id : ids) {
        const Track* t = nullptr;
        if (const Event* e = app.project().findEvent(id, &t)) {
            end = std::max(end, e->start + e->length);
            if (!track && t) track = t->id;
        }
    }
    if (!track) return;
    const double cursor = app.cursor();
    const uint64_t current = app.currentTrack;
    app.copySelection();
    app.currentTrack = track;
    app.setCursor(end);
    const size_t before = app.undoCount();
    app.pasteAtCursor();
    if (app.undoCount() > before) app.mergeUndo(app.undoCount() - before, "Duplicate");
    app.currentTrack = current;
    app.setCursor(cursor);
}

void TouchShellImpl::addMarker()
{
    if (!runAction("marker")) {
        const double t = app.cursor();
        app.edit("Insert Marker", [t](Project& pr) {
            Project::Marker m;
            m.id = pr.newId();
            m.time = t;
            pr.markers.push_back(m);
        });
    }
}

// ---------------------------------------------------------------- text

void TouchShellImpl::textSheet(bool editSelected)
{
    const Event* e = editSelected ? soleEvent() : nullptr;
    const MediaItem* m = e ? app.project().findMedia(e->mediaId) : nullptr;
    if (editSelected && !(m && m->generated)) return;
    const uint64_t eventId = e ? e->id : 0;
    Sheet* s = newSheet(editSelected ? "Edit text" : "Add text");
    QPlainTextEdit* edit = new QPlainTextEdit(s);
    edit->setObjectName("touchTextEdit");
    edit->setFixedHeight(110);
    edit->setPlaceholderText("Type the text");
    edit->setFont(uiFont(17));
    edit->setStyleSheet(QString("QPlainTextEdit { background: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 6px; }")
                            .arg(color::raised.name(), color::text.name(), color::line.name()));
    if (m) edit->setPlainText(generatorText(*m));
    s->body()->addWidget(padded(edit, s));
    Button* ok = new Button(Button::Primary, editSelected ? Icon::Check : Icon::Add, editSelected ? "Save" : "Add", s);
    ok->setObjectName("touchTextOk");
    s->body()->addWidget(padded(ok, s));
    QObject::connect(ok, &QAbstractButton::clicked, q, [this, edit, editSelected, eventId] {
        const std::string text = edit->toPlainText().toStdString();
        q->closeSheet();
        if (text.empty()) return;
        const std::string firstLine = text.substr(0, text.find('\n'));
        if (editSelected) {
            const Event* ev = app.project().findEvent(eventId);
            const MediaItem* mi = ev ? app.project().findMedia(ev->mediaId) : nullptr;
            if (!mi) return;
            const uint64_t mediaId = mi->id, slot = mi->generator.id;
            // through the live effect, as the effects window does, so the picture changes at once
            ofx::Instance* inst = app.fx.video(slot);
            ofx::Param* p = inst ? inst->params.find("Text") : nullptr;
            const size_t before = app.undoCount();
            if (p) app.fxEditStep(slot, "Edit Text", [inst, p, text] { inst->userSetString(p, text, 0.0); });
            app.edit("Edit Text", [mediaId, eventId, firstLine, text, p](Project& pr) {
                MediaItem* x = pr.findMedia(mediaId);
                if (!x) return;
                if (!p) {
                    Json st = x->generator.state.isObject() ? x->generator.state : Json::object();
                    Json params = st.get("params").isObject() ? st.get("params") : Json::object();
                    Json t = Json::object();
                    t["s"] = text;
                    params["Text"] = t;
                    st["params"] = params;
                    x->generator.state = st;
                }
                const std::string old = x->name;
                x->name = firstLine;
                if (Event* ev2 = pr.findEvent(eventId))
                    if (ev2->name.empty() || ev2->name == old) ev2->name = firstLine;
            });
            if (app.undoCount() > before + 1) app.mergeUndo(app.undoCount() - before, "Edit Text");
            return;
        }
        const std::string plugin = titlesId();
        if (plugin.empty()) {
            app.status("Text needs the Titles & Text generator, and none was found.");
            return;
        }
        const double at = app.cursor(), len = app.prefs.generatedLength > 0 ? app.prefs.generatedLength : 5.0;
        uint64_t made = 0;
        app.edit("Add Text", [&](Project& pr) {
            // on the top video track if it is free there, else on a new track above the others
            Track* target = nullptr;
            for (Track& t : pr.tracks) {
                if (t.type != TrackType::Video) continue;
                bool free = true;
                for (const Event& ev : t.events) free = free && (ev.start + ev.length <= at + 1e-9 || ev.start >= at + len - 1e-9);
                if (free) target = &t;
                break;
            }
            if (!target) {
                Track t;
                t.id = pr.newId();
                t.type = TrackType::Video;
                t.name = "Text";
                pr.tracks.insert(pr.tracks.begin(), t);
                target = &pr.tracks.front();
            }
            MediaItem mi;
            mi.id = pr.newId();
            mi.name = firstLine;
            mi.generated = true;
            mi.generator.id = pr.newId();
            mi.generator.kind = "ofx";
            mi.generator.pluginId = plugin;
            mi.generator.name = "Titles & Text";
            Json st = Json::object(), params = Json::object(), t = Json::object();
            t["s"] = text;
            params["Text"] = t;
            st["params"] = params;
            mi.generator.state = st;
            mi.info.hasVideo = true;
            mi.info.width = pr.settings.width;
            mi.info.height = pr.settings.height;
            mi.info.fps = pr.settings.fps();
            Event ev;
            ev.id = pr.newId();
            ev.mediaId = mi.id;
            ev.start = pr.quantize(at);
            ev.length = len;
            ev.name = firstLine;
            pr.media.push_back(mi);
            target->events.mut().push_back(ev);
            target->sortEvents();
            made = ev.id;
        });
        if (made) app.selectOnly(made);
    });
    q->openSheet(s);
    // the keyboard comes up at once (a phone shows it only for a tapped field otherwise)
    edit->setFocus(Qt::OtherFocusReason);
    QTimer::singleShot(250, edit, [] { QGuiApplication::inputMethod()->show(); });
}

// ---------------------------------------------------------------- transitions

void TouchShellImpl::transitionSheet()
{
    const Track* track = nullptr;
    const Event* e = soleEvent(&track);
    if (!e || !track) return;
    const uint64_t eventId = e->id;
    // the event before it on the track, and how far they overlap
    const Event* prev = nullptr;
    for (const Event& x : track->events)
        if (x.id != e->id && x.start < e->start && (!prev || x.start + x.length > prev->start + prev->length)) prev = &x;
    const double overlap = prev ? prev->start + prev->length - e->start : 0;
    Sheet* s = newSheet("Transition");
    if (overlap <= 1e-6) {
        QLabel* l = new QLabel(prev ? "A transition goes where this clip overlaps the one before it." : "There is no clip before this one on its track.", s);
        l->setWordWrap(true);
        l->setFont(uiFont(14));
        QPalette pal = l->palette();
        pal.setColor(QPalette::WindowText, color::dim);
        l->setPalette(pal);
        l->setContentsMargins(16, 4, 16, 8);
        s->body()->addWidget(l);
        if (prev) {
            const double gap = e->start - (prev->start + prev->length);
            const double want = std::min({1.0, e->length * 0.5, prev->length * 0.5});
            Button* make = new Button(Button::Primary, Icon::Transition, "Make a cross-fade", s);
            make->setObjectName("touchMakeCrossfade");
            QObject::connect(make, &QAbstractButton::clicked, q, [this, eventId, gap, want] {
                // this clip and everything after it on the track (with what is grouped with them) moves back
                const double delta = -(gap + want);
                app.edit("Make Cross-Fade", [eventId, delta](Project& pr) {
                    Track* t = nullptr;
                    Event* ev = pr.findEvent(eventId, &t);
                    if (!ev || !t) return;
                    const double from = ev->start;
                    std::set<uint64_t> ids;
                    for (const Event& x : t->events)
                        if (x.start >= from - 1e-9) ids.insert(x.id);
                    for (uint64_t id : withGroups(pr, ids))
                        if (Event* x = pr.findEvent(id)) x->start = std::max(0.0, x->start + delta);
                    for (Track& tr : pr.tracks) tr.sortEvents();
                });
                q->closeSheet();
                QTimer::singleShot(200, q, [this] { transitionSheet(); });
            });
            s->body()->addWidget(padded(make, s));
        }
        q->openSheet(s);
        return;
    }
    const std::string current = e->transition.kind.empty() ? std::string() : e->transition.pluginId;
    Button* none = row(s, Icon::Transition, "Cross-fade", [this, eventId] { app.setTransition(eventId, "", ""); q->closeSheet(); }, "A plain dissolve");
    none->setCheckable(true);
    none->setChecked(current.empty());
    if (!current.empty() && e->transition.id) {
        const uint64_t slot = e->transition.id;
        row(s, Icon::Settings, "Transition settings", [this, eventId, slot] { effectSheet({FxTarget::Transition, eventId}, slot); });
    }
    s->body()->addWidget(sectionLabel("Transitions", s));
    std::vector<VideoFxEntry> list;
    for (const VideoFxEntry& v : Catalog::get().video())
        if (v.transition && v.ok) list.push_back(v);
    std::sort(list.begin(), list.end(), [](const VideoFxEntry& a, const VideoFxEntry& b) { return a.label < b.label; });
    for (const VideoFxEntry& v : list) {
        const std::string id = v.id, label = v.label;
        Button* b = row(s, Icon::Transition, QString::fromStdString(label), [this, eventId, id, label] {
            app.setTransition(eventId, id, label);
            q->closeSheet();
        });
        b->setCheckable(true);
        b->setChecked(id == current);
    }
    q->openSheet(s);
}

// ---------------------------------------------------------------- effects

void TouchShellImpl::fxSheet(const FxTarget& target)
{
    const FxChain* chain = app.chainOf(app.project(), target);
    Sheet* s = newSheet(target.kind == FxTarget::VideoOut ? "Project effects" : "Effects");
    QLabel* who = new QLabel(QString::fromStdString(app.targetName(target)), s);
    who->setFont(uiFont(13));
    QPalette pal = who->palette();
    pal.setColor(QPalette::WindowText, color::dim);
    who->setPalette(pal);
    who->setContentsMargins(16, 0, 16, 6);
    s->body()->addWidget(who);
    if (chain)
        for (const FxSlot& slot : *chain) {
            const uint64_t id = slot.id;
            Button* b = row(s, Icon::Fx, QString::fromStdString(slot.name.empty() ? slot.pluginId : slot.name),
                            [this, target, id] { effectSheet(target, id); }, slot.bypass ? "Off" : "On");
            b->setChevron(true);
        }
    Button* add = new Button(Button::Primary, Icon::Add, "Add effect", s);
    add->setObjectName("touchAddEffect");
    QObject::connect(add, &QAbstractButton::clicked, q, [this, target] { addFxSheet(target); });
    s->body()->addWidget(padded(add, s));
    q->openSheet(s);
}

void TouchShellImpl::addFxSheet(const FxTarget& target)
{
    Sheet* s = newSheet("Add effect");
    QLineEdit* search = new QLineEdit(s);
    search->setObjectName("touchFxSearch");
    search->setPlaceholderText("Search");
    s->body()->addWidget(padded(search, s));
    std::vector<Button*> rows;
    const bool video = app.targetIsVideo(target);
    if (video) {
        std::vector<VideoFxEntry> list;
        for (const VideoFxEntry& v : Catalog::get().video())
            if (v.filter && v.ok) list.push_back(v);
        std::sort(list.begin(), list.end(), [](const VideoFxEntry& a, const VideoFxEntry& b) { return a.label < b.label; });
        for (const VideoFxEntry& v : list) {
            const std::string id = v.id, label = v.label;
            rows.push_back(row(s, Icon::Fx, QString::fromStdString(label), [this, target, id, label] {
                const uint64_t slot = app.addVideoFx(target, id, label);
                if (slot) effectSheet(target, slot);
            }, QString::fromStdString(v.group)));
        }
    } else {
        std::vector<AudioFxEntry> list;
        for (const AudioFxEntry& a : Catalog::get().audio())
            if (a.ok && !a.desc.isInstrument) list.push_back(a);
        std::sort(list.begin(), list.end(), [](const AudioFxEntry& a, const AudioFxEntry& b) { return a.desc.name < b.desc.name; });
        for (const AudioFxEntry& a : list) {
            const AudioPluginDesc desc = a.desc;
            rows.push_back(row(s, Icon::Volume, QString::fromStdString(desc.name), [this, target, desc] {
                const uint64_t slot = app.addAudioFx(target, desc);
                if (slot) effectSheet(target, slot);
            }, QString::fromStdString(desc.vendor.empty() ? desc.format : desc.vendor)));
        }
    }
    QObject::connect(search, &QLineEdit::textChanged, q, [rows](const QString& t) {
        for (Button* b : rows) b->setVisible(t.isEmpty() || b->text().contains(t, Qt::CaseInsensitive));
    });
    q->openSheet(s);
}

void TouchShellImpl::effectSheet(const FxTarget& target, uint64_t slotId)
{
    const Project& pr = app.project();
    const FxSlot* slot = nullptr;
    if (target.kind == FxTarget::Transition) {
        if (const Event* e = pr.findEvent(target.id))
            if (e->transition.id == slotId) slot = &e->transition;
    } else if (target.kind == FxTarget::Generator) {
        if (const MediaItem* m = pr.findMedia(target.id))
            if (m->generator.id == slotId) slot = &m->generator;
    } else if (const FxChain* chain = app.chainOf(pr, target)) {
        for (const FxSlot& x : *chain)
            if (x.id == slotId) slot = &x;
    }
    if (!slot) return;
    Sheet* s = newSheet(target.kind == FxTarget::Generator ? QString("Style") : QString::fromStdString(slot->name.empty() ? slot->pluginId : slot->name));
    s->modal = false;
    if (target.kind != FxTarget::Transition && target.kind != FxTarget::Generator) {
        Switch* on = new Switch("On", s);
        on->setObjectName("touchFxOn");
        on->setChecked(!slot->bypass);
        QObject::connect(on, &QAbstractButton::toggled, q, [this, target, slotId](bool v) { app.setFxBypass(target, slotId, !v); });
        s->body()->addWidget(on);
    }
    // the effect's own settings, for OpenFX effects (audio plug-ins keep their own windows)
    app.fx.prepare(app.project());
    ofx::Instance* inst = slot->kind == "ofx" ? app.fx.video(slotId) : nullptr;
    // the effect's time: frames from the event's start for an event's effect, from the project's start otherwise
    auto fxTime = [this, target]() {
        const Project& p = app.project();
        double t = p.frameTime(p.frameAt(app.cursor()));
        if (target.kind == FxTarget::Event || target.kind == FxTarget::Transition)
            if (const Event* e = p.findEvent(target.id)) t -= e->start;
        return std::max(0.0, t) * p.settings.fps();
    };
    // A setting is looked up again before every use: the instance can be replaced under us (New Project, undo).
    auto live = [this, slotId](const std::string& name) -> std::pair<ofx::Instance*, ofx::Param*> {
        ofx::Instance* i = app.fx.video(slotId);
        return {i, i ? i->params.find(name) : nullptr};
    };
    auto setValues = [this, slotId, live, fxTime](const std::string& name, const std::vector<double>& v) {
        auto lp = live(name);
        if (!lp.second) return;
        const bool keyed = !lp.second->keys.empty();
        lp.first->userSetValue(lp.second, v, fxTime(), keyed);
        app.notify(kFxParams);
    };
    int shown = 0;
    // a group's heading is put in only when a setting of it is (groups can hold only hidden settings)
    QString group;
    auto heading = [&] {
        if (group.isEmpty()) return;
        s->body()->addWidget(sectionLabel(group, s));
        group.clear();
    };
    if (inst) {
        const double time = fxTime();
        for (const auto& up : inst->params.params) {
            const ofx::Param& p = *up;
            if (p.secret()) continue;
            if (p.name == kOfxImageEffectTransitionParamName) continue;
            if (target.kind == FxTarget::Generator && p.name == "Text") continue;     // (the text itself: Edit text)
            const std::string name = p.name;
            const QString label = QString::fromStdString(p.label().empty() ? p.name : p.label());
            const ofx::PropertySet& props = p.props;
            switch (p.kind) {
            case ofx::ParamKind::Group:
                group = label;
                break;
            case ofx::ParamKind::Double:
            case ofx::ParamKind::Int:
            case ofx::ParamKind::Double2D:
            case ofx::ParamKind::Int2D:
            case ofx::ParamKind::Double3D:
            case ofx::ParamKind::Int3D: {
                const std::vector<double> v = p.valueAt(time);
                const bool whole = p.isWhole();
                for (int k = 0; k < p.dim && k < (int)v.size(); ++k) {
                    double lo = props.getDouble(kOfxParamPropDisplayMin, k, -1e300), hi = props.getDouble(kOfxParamPropDisplayMax, k, 1e300);
                    if (!(lo > -1e299)) lo = props.getDouble(kOfxParamPropMin, k, -1e300);
                    if (!(hi < 1e299)) hi = props.getDouble(kOfxParamPropMax, k, 1e300);
                    if (!(lo > -1e299)) lo = std::min(0.0, v[k] - 100);
                    if (!(hi < 1e299)) hi = std::max(1.0, v[k] + 100);
                    if (hi <= lo) hi = lo + 1;
                    QString dimLabel = label;
                    if (p.dim > 1) {
                        const std::string dl = props.getString(kOfxParamPropDimensionLabel, k);
                        dimLabel += " " + QString::fromStdString(dl.empty() ? std::string(1, "XYZ"[k]) : dl).toUpper();
                    }
                    Slider* sl = new Slider(dimLabel, lo, hi, v[k], s);
                    sl->setDefault(props.getDouble(kOfxParamPropDefault, k, v[k]));
                    const int digits = whole ? 0 : std::max(0, std::min(4, (int)props.getInt(kOfxParamPropDigits, 0, 2)));
                    sl->setFormat([digits](double x) { return QString::number(x, 'f', digits); });
                    if (whole) sl->setStep(1);
                    const int dimK = k;
                    sl->onBegin = [this, slotId] { if (!app.fxEditOpen(slotId)) app.beginFxEdit(slotId); };
                    sl->onChange = [setValues, live, fxTime, name, dimK](double x) {
                        auto lp = live(name);
                        if (!lp.second) return;
                        std::vector<double> now = lp.second->valueAt(fxTime());
                        if ((int)now.size() <= dimK) now.resize((size_t)dimK + 1, 0.0);
                        now[(size_t)dimK] = x;
                        setValues(name, now);
                    };
                    sl->onEnd = [this, slotId, label] { if (app.fxEditOpen(slotId)) app.endFxEdit(slotId, "Change \"" + label.toStdString() + "\""); };
                    heading();
                    s->body()->addWidget(sl);
                    ++shown;
                }
                break;
            }
            case ofx::ParamKind::Bool: {
                Switch* sw = new Switch(label, s);
                const std::vector<double> v = p.valueAt(time);
                sw->setChecked(!v.empty() && v[0] != 0);
                QObject::connect(sw, &QAbstractButton::toggled, q, [this, slotId, setValues, name, label](bool on) {
                    app.fxEditStep(slotId, "Change \"" + label.toStdString() + "\"", [&] { setValues(name, {on ? 1.0 : 0.0}); });
                });
                heading();
                s->body()->addWidget(sw);
                ++shown;
                break;
            }
            case ofx::ParamKind::Choice: {
                const std::vector<std::string> choices = p.choices();
                const std::vector<double> v = p.valueAt(time);
                const int at = v.empty() ? 0 : (int)std::lround(v[0]);
                Button* b = new Button(Button::Row, Icon::More, label, s);
                b->setShowIcon(false);
                b->setValueText(at >= 0 && at < (int)choices.size() ? QString::fromStdString(choices[(size_t)at]) : QString());
                b->setChevron(true);
                QObject::connect(b, &QAbstractButton::clicked, q, [this, b, choices, slotId, setValues, name, label, target] {
                    // the choices in a menu sized for fingers
                    QMenu menu(q);
                    menu.setStyleSheet("QMenu { font-size: 16px; padding: 6px; } QMenu::item { padding: 12px 28px; }");
                    for (size_t k = 0; k < choices.size(); ++k) {
                        QAction* a = menu.addAction(QString::fromStdString(choices[k]));
                        a->setData((int)k);
                    }
                    QAction* picked = menu.exec(b->mapToGlobal(QPoint(b->width() / 2, b->height())));
                    if (!picked) return;
                    const int k = picked->data().toInt();
                    app.fxEditStep(slotId, "Change \"" + label.toStdString() + "\"", [&] { setValues(name, {(double)k}); });
                    b->setValueText(QString::fromStdString(choices[(size_t)k]));
                });
                heading();
                s->body()->addWidget(b);
                ++shown;
                break;
            }
            case ofx::ParamKind::RGB:
            case ofx::ParamKind::RGBA: {
                const std::vector<double> v = p.valueAt(time);
                auto toColor = [](const std::vector<double>& c) {
                    auto ch = [&](size_t k, double d) { return (int)std::lround(std::max(0.0, std::min(1.0, k < c.size() ? c[k] : d)) * 255); };
                    return QColor(ch(0, 0), ch(1, 0), ch(2, 0), ch(3, 1));
                };
                const bool alpha = p.kind == ofx::ParamKind::RGBA;
                Swatches* sw = new Swatches(label, toColor(v), s);
                // a colour is one undo step; its alpha stays as it was unless the full dialog changes it
                auto apply = [this, slotId, setValues, live, fxTime, name, label, alpha](const QColor& c, bool withAlpha) {
                    auto lp = live(name);
                    if (!lp.second) return;
                    std::vector<double> nv = {c.redF(), c.greenF(), c.blueF()};
                    if (alpha) {
                        const std::vector<double> was = lp.second->valueAt(fxTime());
                        nv.push_back(withAlpha ? c.alphaF() : (was.size() > 3 ? was[3] : 1.0));
                    }
                    app.fxEditStep(slotId, "Change \"" + label.toStdString() + "\"", [&] { setValues(name, nv); });
                };
                sw->onPick = [apply](const QColor& c) { apply(c, false); };
                sw->onMore = [this, sw, live, fxTime, name, label, alpha, toColor, apply] {
                    auto lp = live(name);
                    if (!lp.second) return;
                    const QColor c = QColorDialog::getColor(toColor(lp.second->valueAt(fxTime())), q, label,
                                                            alpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
                    if (!c.isValid()) return;
                    apply(c, true);
                    sw->setValue(c);
                };
                heading();
                s->body()->addWidget(sw);
                ++shown;
                break;
            }
            case ofx::ParamKind::String: {
                if (props.getString(kOfxParamPropStringMode, 0, kOfxParamStringIsSingleLine) == kOfxParamStringIsLabel) break;
                QLineEdit* ed = new QLineEdit(QString::fromStdString(p.strAt(time)), s);
                ed->setPlaceholderText(label);
                QObject::connect(ed, &QLineEdit::editingFinished, q, [this, ed, slotId, live, fxTime, name, label] {
                    auto lp = live(name);
                    if (!lp.second) return;
                    const std::string t = ed->text().toStdString();
                    if (t == lp.second->strAt(fxTime())) return;
                    app.fxEditStep(slotId, "Change \"" + label.toStdString() + "\"", [&] { lp.first->userSetString(lp.second, t, fxTime()); });
                    app.notify(kFxParams);
                });
                heading();
                s->body()->addWidget(sectionLabel(label, s));
                s->body()->addWidget(padded(ed, s));
                ++shown;
                break;
            }
            case ofx::ParamKind::PushButton: {
                heading();
                Button* b = row(s, Icon::Check, label, [this, slotId, live, fxTime, name] {
                    auto lp = live(name);
                    if (lp.second) lp.first->userPushButton(lp.second, fxTime());
                    app.notify(kFxParams);
                });
                b->setShowIcon(false);
                ++shown;
                break;
            }
            default:
                break;
            }
        }
    }
    if (!shown && slot->kind == "ofx") {
        QLabel* l = new QLabel(inst ? "This effect has no settings to change here." : "This effect could not be loaded.", s);
        l->setFont(uiFont(14));
        l->setContentsMargins(16, 8, 16, 8);
        s->body()->addWidget(l);
    }
    s->body()->addWidget(sectionLabel("Effect", s));
    row(s, Icon::Settings, "All controls", [this, target, slotId] {
        q->closeSheet();
        mw->openFx(target, slotId);
    }, slot->kind == "ofx" ? "The effects window, with keyframes and presets" : "The plug-in's own window");
    if (target.kind != FxTarget::Transition && target.kind != FxTarget::Generator) {
        row(s, Icon::Up, "Move earlier in the chain", [this, target, slotId] { app.moveFx(target, slotId, -1); });
        Button* rm = row(s, Icon::Delete, "Remove effect", [this, target, slotId] {
            app.removeFx(target, slotId);
            fxSheet(target);
        });
        rm->setDanger(true);
    }
    q->openSheet(s);
}

// ---------------------------------------------------------------- more

void TouchShellImpl::moreSheet()
{
    Sheet* s = newSheet("More");
    const std::set<uint64_t> sel = app.selection();
    auto later = [this](const char* action) {
        q->closeSheet();
        QTimer::singleShot(180, q, [this, action] { runAction(action); });
    };
    row(s, Icon::Settings, "Properties", [later] { later("eventProperties"); }, "Event Properties");
    bool locked = !sel.empty();
    for (uint64_t id : sel)
        if (const Event* e = app.project().findEvent(id)) locked = locked && e->locked;
    row(s, Icon::Lock, locked ? "Unlock" : "Lock", [this, sel, locked] { app.setLocked(sel, !locked); q->closeSheet(); });
    row(s, Icon::Group, "Group", [this] { app.groupSelection(true); q->closeSheet(); }, "Move and trim together");
    row(s, Icon::Group, "Ungroup", [this] { app.groupSelection(false); q->closeSheet(); });
    row(s, Icon::Copy, "Cut", [this] { app.cutSelection(); q->closeSheet(); });
    if (selectionHasVideo()) row(s, Icon::Split, "Split at scene changes", [later] { later("detectScenes"); });
    row(s, Icon::Freeze, "Select all", [this] {
        std::set<uint64_t> all;
        for (const Track& t : app.project().tracks)
            for (const Event& e : t.events) all.insert(e.id);
        app.setSelection(all);
        q->closeSheet();
    });
    row(s, Icon::Search, "All commands", [this] { commandsSheet(); });
    q->openSheet(s);
}

void TouchShellImpl::commandsSheet()
{
    Sheet* s = newSheet("All commands");
    QLineEdit* search = new QLineEdit(s);
    search->setObjectName("touchCommandSearch");
    search->setPlaceholderText("Search commands");
    s->body()->addWidget(padded(search, s));
    // every command of the desktop window's menus, with the menus it is in
    std::vector<Button*> rows;
    std::set<QAction*> seen;
    std::function<void(QMenu*, const QString&)> walk = [&](QMenu* menu, const QString& path) {
        menu->aboutToShow();            // (the signal) menus that fill or grey themselves when opened
        for (QAction* a : menu->actions()) {
            if (a->isSeparator() || !a->isVisible()) continue;
            if (QMenu* sub = a->menu()) {
                walk(sub, path + QString::fromUtf8(" › ") + plain(a->text()));
                continue;
            }
            if (a->text().isEmpty() || !seen.insert(a).second) continue;
            QPointer<QAction> act = a;
            Button* b = new Button(Button::Row, Icon::Right, plain(a->text()), s);
            b->setShowIcon(false);
            b->setSubtitle(path);
            b->setProperty("renoPath", path);
            b->setEnabled(a->isEnabled());
            if (a->isCheckable()) {
                b->setCheckable(true);
                b->setChecked(a->isChecked());
            }
            QObject::connect(b, &QAbstractButton::clicked, q, [this, act] {
                q->closeSheet();
                QTimer::singleShot(180, q, [act] { if (act && act->isEnabled()) act->trigger(); });
            });
            s->body()->addWidget(b);
            rows.push_back(b);
        }
    };
    for (QAction* top : mw->menuBar()->actions())
        if (QMenu* m = top->menu()) walk(m, plain(top->text()));
    QObject::connect(search, &QLineEdit::textChanged, q, [rows](const QString& t) {
        for (Button* b : rows) b->setVisible(t.isEmpty() || b->text().contains(t, Qt::CaseInsensitive) || b->property("renoPath").toString().contains(t, Qt::CaseInsensitive));
    });
    q->openSheet(s);
}

}  // namespace app
}  // namespace reno
