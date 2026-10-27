// File > Real-Time Render, Tools > Audio > Apply Non-Real-Time Event FX, Remove Assignable FX, Auto Ducking, Beat
// Detection and Tempo Detection; see audiotools.h.
#include "audiotools.h"
#include "pluginchooser.h"
#include "fxwindow.h"
#include "rendertemplates.h"
#include "../engine/engine.h"
#include "strconv.h"
#include <QAction>
#include <QCoreApplication>
#include <QFile>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include "../engine/audioanalysis.h"
#include "../engine/automation.h"
#include "../media/media.h"
#include <QFormLayout>
#include <QCheckBox>
#include <QComboBox>
#include <QRegularExpression>
#include <QTimer>
#include <set>
#include <QPainter>
#include <QSlider>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace reno {
namespace app {

namespace {


// A WAV of the project's sound (24-bit PCM at the project rate).
EncodeSettings wavSettings(const Project& p, const std::string& path)
{
    EncodeSettings es;
    es.path = path;
    es.video = false;
    es.audio = true;
    es.sampleRate = p.settings.sampleRate;
    es.audioFamily = "pcm";
    es.audioBits = 24;
    es.audioChannels = 2;
    return es;
}

// Runs an export with a progress window (Cancel stops it). False with *error ("stopped" if cancelled).
bool exportWithProgress(QWidget* parent, const QString& title, const Project& p, const EncodeSettings& es, double from, double to, std::string* error)
{
    AppState& app = AppState::get();
    std::unique_ptr<QProgressDialog> dlg;
    if (parent) {
        dlg.reset(new QProgressDialog(title, "Cancel", 0, 1000, parent));
        dlg->setWindowTitle(title);
        dlg->setWindowModality(Qt::WindowModal);
        dlg->setMinimumDuration(300);
    }
    std::string err;
    const bool ok = exportProject(p, app.fx, es, from, to,
                                  [&](const ExportProgress& ep) {
                                      if (!dlg) return true;
                                      dlg->setValue((int)std::lround(std::min(1.0, std::max(0.0, ep.fraction)) * 1000.0));
                                      QApplication::processEvents();
                                      return !dlg->wasCanceled();
                                  },
                                  &err, app.mediaOpener);
    if (!ok && error) *error = err;
    return ok;
}

// A free file name next to the project (or in Documents): "<base> <n>.wav".
QString freeName(const QString& base)
{
    AppState& app = AppState::get();
    QString dir = app.path().empty() ? QDir::homePath() : QFileInfo(qs(app.path())).absolutePath();
    for (int n = 2; n < 100000; ++n) {      // the original is take 1
        const QString f = QDir(dir).filePath(n == 1 ? base + ".wav" : QString("%1 %2.wav").arg(base).arg(n));
        if (!QFileInfo::exists(f)) return f;
    }
    return QDir(dir).filePath(base + ".wav");
}

}  // namespace

// ------------------------------------------------------------------------------------------ Real-Time Render
bool realTimeRenderTo(const std::string& path, std::string* error)
{
    AppState& app = AppState::get();
    std::shared_ptr<const Project> p = app.snapshot();
    const double end = p->length();
    if (!(end > 0)) {
        if (error) *error = "the project is empty";
        return false;
    }
    return exportWithProgress(nullptr, QString(), *p, wavSettings(*p, path), 0.0, end, error);
}

bool runRealTimeRender(QWidget* parent)
{
    AppState& app = AppState::get();
    std::shared_ptr<const Project> p = app.snapshot();
    if (!(p->length() > 0)) {
        QMessageBox::information(parent, "Real-Time Render", "There is nothing on the timeline to render.");
        return false;
    }
    const QString base = app.path().empty() ? QString("Untitled") : QFileInfo(qs(app.path())).completeBaseName();
    const QString path = QFileDialog::getSaveFileName(parent, "Real-Time Render", freeName(base), "Wave (Microsoft) (*.wav)");
    if (path.isEmpty()) return false;
    std::string err;
    const bool ok = exportWithProgress(parent, "Real-Time Render", *p, wavSettings(*p, s8(path)), 0.0, p->length(), &err);
    if (!ok) {
        if (err != "stopped") QMessageBox::warning(parent, "Real-Time Render", "The sound could not be rendered: " + qs(err));
        QFile::remove(path);
        return false;
    }
    app.status(s8("Real-Time Render: " + QDir::toNativeSeparators(path)));
    return true;
}

// ------------------------------------------------------------------------------------------ Non-Real-Time Event FX
namespace {

const char* const kProjectTemplate = "Project sample rate, 24 Bit, Stereo";

// Formats Apply Non Real-Time FX offers: sound-only ones that keep every sample (Wave, Sony Wave64, AIFF, FLAC), each
// with Render As's templates and, first, one at the project rate and 24 bits (default).
std::vector<rt::Format> nrtFormatList()
{
    std::vector<rt::Format> out;
    for (const char* key : {"wav", "w64", "aiff", "flac"})
        for (rt::Format f : rt::loadFormats()) {
            if (f.key != key || !f.unavailable.isEmpty() || f.hasVideo) continue;
            std::vector<rt::Tmpl> keep;
            rt::Tmpl mine = f.templates.empty() ? rt::Tmpl() : f.templates.front();
            mine.name = kProjectTemplate;
            mine.description = "Audio: the project's sample rate, 24 Bit, Stereo.";
            mine.custom = false;
            mine.video = false;
            mine.audio = true;
            mine.sampleRate = 0;
            mine.bits = 24;
            mine.channels = 2;
            mine.multipleMono = false;
            keep.push_back(mine);
            for (const rt::Tmpl& t : f.templates)
                if (!t.multipleMono && t.audio && !t.video) keep.push_back(t);
            f.templates = keep;
            out.push_back(f);
        }
    return out;
}

// The project that renders one event's sound: its stretch of media as it is in the media (no rate, fades, level or
// envelopes; those stay on the event), through 'chain'. 'length' is its length.
Project nrtProject(const Project& p, const Event& e, const MediaItem& m, const FxChain& chain, double* length)
{
    Project q;
    q.settings = p.settings;
    q.settings.audioChannels = 2;
    q.media.push_back(m);
    Track t;
    t.id = 1;
    t.type = TrackType::Audio;
    Event x;
    x.id = 2;
    x.mediaId = m.id;
    x.start = 0;
    // VEGAS ignores effect tails (reverb, delay) in Apply Non-Real-Time Event FX, so the file is as long as the event
    x.length = std::max(1e-3, e.length * std::max(1e-6, e.rate));
    x.offset = e.offset;
    x.loop = e.loop;
    x.mediaChannel = e.mediaChannel;
    x.mediaChannels = e.mediaChannels;
    x.fx = chain;
    t.events.push_back(x);
    q.tracks.push_back(t);
    q.nextId = std::max<uint64_t>(p.nextId, 3);
    if (length) *length = x.length;
    return q;
}

QString lowerPath(const QString& s) { return QDir::cleanPath(s).toLower(); }

// ---- effects that make the sound longer or shorter (Time Stretch, Gapper/Snipper, Vibrato)
// A render of the event through them would cut what they deliver to the event's length (and a block-by-block chain cannot
// give more or fewer samples than it takes), so the file is made the way the DirectX filter graph of VEGAS makes it: the
// event's sound as it is in the media is run through the effects one after the other, each effect giving everything it
// delivers, and that is the file. The take's file is then as long as the effects make it (Time Stretch at 200 %: twice as
// long); the event itself keeps its length.

bool chainChangesLength(AppState& app, const Project& q, const FxChain& chain)
{
    app.fx.prepare(q);
    FxRuntime::Hold hold(app.fx);
    for (const FxSlot& s : chain) {
        if (s.kind != "audio" || s.bypass) continue;
        AudioPlugin* pl = app.fx.audio(s.id);
        if (pl && pl->changesLength()) return true;
    }
    return false;
}

// 32-bit float stereo Wave file (the intermediate the render then converts to the format that was asked for)
bool writeFloatWav(const std::string& path, const std::vector<float>& l, const std::vector<float>& r, int rate)
{
    QFile f(qs(path));
    if (!f.open(QIODevice::WriteOnly)) return false;
    const uint32_t frames = (uint32_t)std::min<size_t>(l.size(), 0x1FFFFFF0u);
    const uint32_t bytes = frames * 8;
    auto u32 = [&](uint32_t v) { char b[4] = {(char)(v & 255), (char)((v >> 8) & 255), (char)((v >> 16) & 255), (char)((v >> 24) & 255)}; f.write(b, 4); };
    auto u16 = [&](uint16_t v) { char b[2] = {(char)(v & 255), (char)((v >> 8) & 255)}; f.write(b, 2); };
    f.write("RIFF", 4);
    u32(36 + bytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(3);
    u16(2);
    u32((uint32_t)rate);
    u32((uint32_t)rate * 8);
    u16(8);
    u16(32);
    f.write("data", 4);
    u32(bytes);
    std::vector<float> chunk;
    for (uint32_t at = 0; at < frames;) {
        const uint32_t n = std::min<uint32_t>(frames - at, 16384);
        chunk.resize((size_t)n * 2);
        for (uint32_t k = 0; k < n; ++k) {
            chunk[(size_t)k * 2] = l[at + k];
            chunk[(size_t)k * 2 + 1] = r[at + k];
        }
        f.write((const char*)chunk.data(), (qint64)n * 8);
        at += n;
    }
    return f.error() == QFileDevice::NoError;
}

// The event's sound, then every effect of the chain over all of it. 'step' is told how far along it is (0..1) and says
// whether to go on.
bool nrtSoundThroughChain(AppState& app, const Project& dry, const Project& withFx, const FxChain& chain, double length,
                          const std::function<bool(double)>& step, std::vector<float>* outL, std::vector<float>* outR, std::string* why)
{
    const ProjectTempoScope tempoScope(dry.settings.tempo, dry.settings.beatsPerMeasure);
    const int rate = std::max(8000, dry.settings.sampleRate);
    const int64_t n = std::max<int64_t>(1, timeToSample(length, rate));
    std::vector<float> L((size_t)n, 0.f), R((size_t)n, 0.f);
    {
        MediaPool pool;
        if (app.mediaOpener) pool.setOpener(app.mediaOpener);
        app.fx.prepare(dry);
        AudioEngine audio(pool, app.fx);
        const int block = 4096;
        for (int64_t at = 0; at < n; at += block) {
            const int c = (int)std::min<int64_t>(block, n - at);
            audio.render(dry, at, c, L.data() + at, R.data() + at, false);
            if (!step(0.4 * (double)(at + c) / (double)n)) {
                if (why) *why = "stopped";
                return false;
            }
        }
    }
    app.fx.prepare(withFx);
    FxRuntime::Hold hold(app.fx);
    size_t done = 0;
    for (const FxSlot& s : chain) {
        if (s.kind != "audio" || s.bypass) continue;
        AudioPlugin* pl = app.fx.audio(s.id);
        ++done;
        if (!pl) continue;
        std::vector<float> oL, oR;
        if (pl->changesLength() && pl->processWhole(L.data(), R.data(), (int64_t)L.size(), oL, oR)) {
            L.swap(oL);
            R.swap(oR);
        } else {
            // an effect that gives back what it is given: block by block, a late one (latency) read ahead as the mixer does
            const int lat = std::max(0, pl->latencySamples());
            L.resize(L.size() + (size_t)lat, 0.f);
            R.resize(R.size() + (size_t)lat, 0.f);
            const int64_t total = (int64_t)L.size();
            for (int64_t at = 0; at < total; at += FxRuntime::kAudioBlock)
                pl->process(L.data() + at, R.data() + at, (int)std::min<int64_t>(FxRuntime::kAudioBlock, total - at), at, false);
            if (lat > 0) {
                L.erase(L.begin(), L.begin() + lat);
                R.erase(R.begin(), R.begin() + lat);
            }
        }
        if (!step(0.4 + 0.1 * (double)done / (double)std::max<size_t>(1, chain.size()))) {
            if (why) *why = "stopped";
            return false;
        }
    }
    if (L.size() != R.size()) R.resize(L.size(), 0.f);
    *outL = std::move(L);
    *outR = std::move(R);
    return true;
}

}  // namespace

std::string nrtDefaultFolder(const Project& p, uint64_t eventId)
{
    // VEGAS puts the new file beside the take's media, or in the project's folder if that is not writable
    AppState& app = AppState::get();
    if (const Event* e = p.findEvent(eventId))
        if (const MediaItem* m = p.findMedia(e->mediaId))
            if (!m->path.empty()) {
                const QString dir = QFileInfo(qs(m->path)).absolutePath();
                if (QFileInfo(dir).isDir() && QFileInfo(dir).isWritable()) return s8(dir);
            }
    if (!app.path().empty()) return s8(QFileInfo(qs(app.path())).absolutePath());
    return s8(QDir::homePath());
}

std::vector<std::pair<std::string, std::string>> nrtFormats()
{
    std::vector<std::pair<std::string, std::string>> v;
    for (const rt::Format& f : nrtFormatList()) v.push_back({s8(f.key), s8(f.name)});
    return v;
}

std::vector<std::string> nrtTemplates(const std::string& format)
{
    std::vector<std::string> v;
    for (const rt::Format& f : nrtFormatList())
        if (s8(f.key) == format)
            for (const rt::Tmpl& t : f.templates) v.push_back(s8(t.name));
    return v;
}

NrtFile nextNrtFile(const Project& p, uint64_t eventId, const std::string& folder, const std::string& ext, const std::set<std::string>& reserved)
{
    const Event* e = p.findEvent(eventId);
    const MediaItem* m = e ? p.findMedia(e->mediaId) : nullptr;
    QString name = m ? qs(m->path.empty() ? m->name : m->path) : (e ? qs(e->name) : QString());
    QString base = QFileInfo(name).completeBaseName();
    if (base.isEmpty()) base = "Event";
    // VEGAS names the file after the take's whole media name: "song Take 2" processed again is "song Take 2 Take 2"
    std::set<QString> usedStems;
    for (const MediaItem& x : p.media) usedStems.insert(QFileInfo(qs(x.name)).completeBaseName().toLower());
    for (const Track& t : p.tracks)
        for (const Event& ev : t.events) {
            usedStems.insert(qs(ev.name).toLower());
            for (const Take& tk : ev.takes) usedStems.insert(qs(tk.name).toLower());
        }
    std::set<QString> res;
    for (const std::string& r : reserved) res.insert(lowerPath(qs(r)));
    NrtFile out;
    for (int n = 2; n < 100000; ++n) {      // the original is take 1
        const QString stem = QString("%1 Take %2").arg(base).arg(n);
        const QString path = QDir(qs(folder)).filePath(stem + "." + qs(ext));
        bool taken = usedStems.count(stem.toLower()) || res.count(lowerPath(path));
        for (const char* x : {"wav", "w64", "aif", "aiff", "flac"})
            if (QFileInfo::exists(QDir(qs(folder)).filePath(stem + "." + x))) taken = true;
        if (taken && n < 99999) continue;
        out.path = s8(path);
        out.takeName = s8(stem);
        break;
    }
    return out;
}

namespace {
// The format and template named in 'f' (the first of each if not found).
bool nrtSpec(const NrtFormat& f, const Project& q, const std::string& path, RenderSpec* spec, std::string* ext, std::string* error)
{
    const std::vector<rt::Format> all = nrtFormatList();
    if (all.empty()) {
        if (error) *error = "no sound format is available";
        return false;
    }
    const rt::Format* fmt = &all.front();
    for (const rt::Format& x : all)
        if (s8(x.key) == f.format) fmt = &x;
    const rt::Tmpl* tm = &fmt->templates.front();
    for (const rt::Tmpl& t : fmt->templates)
        if (!f.templateName.empty() && s8(t.name) == f.templateName) tm = &t;
    rt::RenderChoices c;
    c.writerPath = path;
    QString why;
    if (!rt::makeRenderSpec(*fmt, *tm, c, q.settings, spec, &why)) {
        if (error) *error = s8(why);
        return false;
    }
    if (ext) *ext = s8(rt::fileExt(*fmt, tm));
    return true;
}
}  // namespace

bool applyNonRealTimeFxToEvents(const std::vector<uint64_t>& eventIds, const FxChain& chain, const NrtOptions& opt, std::string* error)
{
    AppState& app = AppState::get();
    const size_t undo0 = app.undoCount();
    const bool dirty0 = app.dirty();
    struct Job { uint64_t event = 0, media = 0; std::string path, takeName; };
    std::vector<Job> jobs;
    for (uint64_t id : eventIds) {
        const Track* tr = nullptr;
        const Event* e = app.project().findEvent(id, &tr);
        if (!e || !tr || tr->type != TrackType::Audio) continue;
        const MediaItem* m = app.project().findMedia(e->mediaId);
        if (!m || m->offline || !m->info.hasAudio) continue;
        bool again = false;
        for (const Job& j : jobs) again = again || j.event == id;
        if (!again) jobs.push_back(Job{id, 0, std::string(), std::string()});
    }
    if (jobs.empty()) {
        if (error) *error = eventIds.size() > 1 ? "choose audio events whose media has sound that can be read" : "the event's media has no sound that can be read (choose an audio event)";
        return false;
    }
    // ---- file names
    std::string ext;
    {
        RenderSpec probe;
        std::string why;
        if (!nrtSpec(opt.format, app.project(), "x", &probe, &ext, &why)) {
            if (error) *error = why;
            return false;
        }
    }
    std::string folder = opt.folder.empty() ? nrtDefaultFolder(app.project(), jobs.front().event) : opt.folder;
    if (opt.exactPath.empty() && !QDir().mkpath(qs(folder))) {
        if (error) *error = "the folder " + folder + " cannot be made";
        return false;
    }
    std::set<std::string> reserved;
    for (Job& j : jobs) {
        if (!opt.exactPath.empty() && jobs.size() == 1) {
            j.path = opt.exactPath;
            j.takeName = s8(QFileInfo(qs(j.path)).completeBaseName());
        } else if (!opt.fileName.empty() && jobs.size() == 1) {
            QString stem = qs(opt.fileName).trimmed();
            if (stem.endsWith("." + qs(ext), Qt::CaseInsensitive)) stem.chop((qsizetype)ext.size() + 1);
            if (stem.isEmpty() || stem.contains('/') || stem.contains('\\')) {
                if (error) *error = "the file name is not usable";
                return false;
            }
            j.path = s8(QDir(qs(folder)).filePath(stem + "." + qs(ext)));
            j.takeName = s8(stem);
            if (QFileInfo::exists(qs(j.path))) {
                if (error) *error = "the file " + j.path + " already exists (a take's file is never written over: choose another name)";
                return false;
            }
        } else {
            const NrtFile f = nextNrtFile(app.project(), j.event, folder, ext, reserved);
            j.path = f.path;
            j.takeName = f.takeName;
        }
        reserved.insert(j.path);
    }

    // ---- rendering
    std::unique_ptr<QProgressDialog> dlg;
    if (opt.parent) {
        dlg.reset(new QProgressDialog("Rendering...", "Cancel", 0, 1000, opt.parent));
        dlg->setObjectName("nrtProgress");
        dlg->setWindowTitle("Apply Non Real-Time FX");
        dlg->setWindowModality(Qt::WindowModal);
        dlg->setMinimumDuration(300);
    }
    std::vector<std::string> made;
    auto fail = [&](const std::string& why) {
        for (const std::string& f : made) QFile::remove(qs(f));
        if (error) *error = why;
        return false;
    };
    for (size_t k = 0; k < jobs.size(); ++k) {
        Job& j = jobs[k];
        const Event* e = app.project().findEvent(j.event);
        const MediaItem* m = e ? app.project().findMedia(e->mediaId) : nullptr;
        if (!e || !m) return fail("the event has gone");
        double length = 0;
        Project q = nrtProject(app.project(), *e, *m, chain, &length);
        const double n = (double)jobs.size();
        // the progress window and the caller's progress: 'within' is how far along this event is (0..1)
        auto step = [&](double within) {
            const double all = (k + std::min(1.0, std::max(0.0, within))) / n;
            if (dlg) {
                dlg->setValue((int)std::lround(all * 1000.0));
                QApplication::processEvents();
                if (dlg->wasCanceled()) return false;
            }
            return !opt.progress || opt.progress(all);
        };
        // effects that change the length: the sound goes through them whole into an intermediate file, which the render
        // below converts
        std::string tempWav;
        double from = 0.0;
        if (chainChangesLength(app, q, chain)) {
            const Project dry = nrtProject(app.project(), *e, *m, FxChain(), nullptr);
            std::vector<float> L, R;
            std::string why;
            if (!nrtSoundThroughChain(app, dry, q, chain, length, step, &L, &R, &why)) return fail(why.empty() ? "the sound could not be made" : why);
            const int rate = std::max(8000, q.settings.sampleRate);
            tempWav = s8(QDir(QDir::tempPath()).filePath(QString("reno_nrt_%1_%2.wav").arg(QCoreApplication::applicationPid()).arg(k)));
            made.push_back(tempWav);
            if (!writeFloatWav(tempWav, L, R, rate)) return fail("the intermediate file could not be written");
            length = std::max(1e-3, (double)L.size() / (double)rate);
            MediaItem tm;
            tm.id = 1;
            tm.path = tempWav;
            tm.name = "nrt";
            tm.info.path = tempWav;
            tm.info.hasAudio = true;
            tm.info.sampleRate = rate;
            tm.info.channels = 2;
            tm.info.duration = length;
            tm.info.audioCodec = "pcm_f32le";
            Project q2;
            q2.settings = q.settings;
            q2.media.push_back(tm);
            Track t;
            t.id = 1;
            t.type = TrackType::Audio;
            Event x;
            x.id = 2;
            x.mediaId = tm.id;
            x.start = 0;
            x.length = length;
            x.loop = false;
            t.events.push_back(x);
            q2.tracks.push_back(t);
            q2.nextId = 3;
            q = q2;
            from = 0.0;
        }
        RenderSpec spec;
        std::string why;
        if (!nrtSpec(opt.format, q, j.path, &spec, nullptr, &why)) return fail(why);
        if (dlg) dlg->setLabelText(QString("Rendering %1 (%2 of %3)...").arg(qs(j.takeName)).arg(k + 1).arg(jobs.size()));
        made.push_back(j.path);
        const double base = tempWav.empty() ? 0.0 : 0.5;
        const bool ok = renderProjectAs(q, app.fx, spec, from, length,
                                        [&](const ExportProgress& ep) { return step(base + (1.0 - base) * std::min(1.0, std::max(0.0, ep.fraction))); },
                                        &why, app.mediaOpener);
        if (!tempWav.empty()) {
            QFile::remove(qs(tempWav));
            made.erase(std::remove(made.begin(), made.end(), tempWav), made.end());
        }
        if (!ok) return fail(why.empty() ? "the sound could not be rendered" : why);
    }
    dlg.reset();

    // ---- the new files become Project Media and the events' new active takes: one undo step
    for (Job& j : jobs) {
        std::string why;
        j.media = app.importMedia(j.path, &why);
        if (!j.media) {
            app.discardUndoTo(undo0, dirty0);
            return fail("the rendered file could not be read back: " + why);
        }
    }
    std::set<uint64_t> chainIds;
    for (const FxSlot& s : chain) chainIds.insert(s.id);
    app.edit("Apply Non-Real-Time Event FX", [&](Project& pr) {
        // the effects were for the render only
        for (Track& t : pr.tracks)
            for (Event& ev : t.events)
                ev.fx.erase(std::remove_if(ev.fx.begin(), ev.fx.end(), [&](const FxSlot& s) { return chainIds.count(s.id) != 0; }), ev.fx.end());
        for (const Job& j : jobs) {
            Event* ev = pr.findEvent(j.event);
            if (!ev) continue;
            Take nt;
            nt.mediaId = j.media;
            nt.offset = 0;                      // the new file starts where the event starts in its media
            nt.name = j.takeName;
            ev->takes.push_back(nt);
            AppState::setActiveTake(*ev, (int)AppState::takesOf(*ev).size() - 1);
        }
    });
    app.mergeUndo(app.undoCount() - undo0, "Apply Non-Real-Time Event FX");
    return true;
}

bool applyNonRealTimeFx(uint64_t eventId, const FxChain& chain, const std::string& path, std::string* error)
{
    NrtOptions o;
    o.exactPath = path;
    const QString ext = QFileInfo(qs(path)).suffix().toLower();
    o.format.format = ext == "w64" ? "w64" : (ext == "aif" || ext == "aiff") ? "aiff" : ext == "flac" ? "flac" : "wav";
    return applyNonRealTimeFxToEvents({eventId}, chain, o, error);
}

void runNonRealTimeEventFx(QWidget* parent, const NrtHooks& hooks)
{
    AppState& app = AppState::get();
    const Project& p = app.project();
    // selected audio events that have sound to process (VEGAS processes the active take)
    std::vector<uint64_t> ids;
    for (const Track& t : p.tracks)
        if (t.type == TrackType::Audio)
            for (const Event& e : t.events)
                if (app.selection().count(e.id)) {
                    const MediaItem* m = p.findMedia(e.mediaId);
                    if (m && !m->offline && m->info.hasAudio) ids.push_back(e.id);
                }
    if (ids.empty()) {
        QMessageBox::information(parent, "Apply Non-Real-Time Event FX", "Select the audio event to process first.");
        return;
    }
    const uint64_t first = ids.front();
    const size_t undo0 = app.undoCount();
    const bool dirty0 = app.dirty();

    // 1. the Plug-In Chooser
    PluginChooser chooser(ChooserKind::AudioFx, "Plug-In Chooser - Non-Real-Time Event FX", FxChain(), parent);
    if (chooser.exec() != QDialog::Accepted) return;
    FxChain chosen = chooser.chain();
    if (chosen.empty()) return;
    for (FxSlot& s : chosen) s.id = 0;
    // the effects are put on the first event for now (after its own) so they run and can be set; they are not kept
    FxTarget target;
    target.kind = FxTarget::Event;
    target.id = first;
    std::vector<uint64_t> before;
    FxChain all;
    if (const Event* e = app.project().findEvent(first)) {
        all = e->fx;
        for (const FxSlot& s : e->fx) before.push_back(s.id);
    }
    all.insert(all.end(), chosen.begin(), chosen.end());
    applyChain(target, all, "Non-Real-Time Event FX");
    std::vector<uint64_t> added;
    if (const Event* e = app.project().findEvent(first))
        for (const FxSlot& s : e->fx)
            if (std::find(before.begin(), before.end(), s.id) == before.end()) added.push_back(s.id);
    auto giveUp = [&] { app.discardUndoTo(undo0, dirty0); };
    if (added.empty()) { giveUp(); return; }

    // 2. VEGAS's Take dialog: the settings of each plug-in of the chain, and Preview
    {
        QDialog fx(parent);
        fx.setObjectName("nonRealTimeTakeFxDialog");
        const Event* e = app.project().findEvent(first);
        fx.setWindowTitle("Take FX" + (e && !e->name.empty() ? " - " + qs(e->name) : QString()));
        fx.resize(640, 440);
        auto* v = new QVBoxLayout(&fx);
        FxWindow* win = new FxWindow(nullptr, &fx);
        win->setObjectName("nrtFxWindow");
        win->setOnlySlots(added, true);
        win->showTarget(target, added.front());
        v->addWidget(win, 1);
        auto* row = new QHBoxLayout();
        QPushButton* prev = new QPushButton("Preview", &fx);
        prev->setObjectName("nrtPreview");
        prev->setCheckable(true);
        prev->setToolTip("Plays the event through the effects");
        prev->setEnabled(bool(hooks.preview));
        const double at = e ? e->start : 0.0, span = e ? e->length : 1.0;
        QObject::connect(prev, &QPushButton::toggled, &fx, [prev, hooks, at, span, &fx](bool on) {
            prev->setText(on ? "Stop" : "Preview");
            if (hooks.preview) hooks.preview(on, at);
            if (on) QTimer::singleShot((int)std::min(3.6e6, span * 1000.0 + 200.0), &fx, [prev] { if (prev->isChecked()) prev->setChecked(false); });
        });
        row->addWidget(prev);
        row->addStretch(1);
        QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &fx);
        QObject::connect(bb, &QDialogButtonBox::accepted, &fx, &QDialog::accept);
        QObject::connect(bb, &QDialogButtonBox::rejected, &fx, &QDialog::reject);
        row->addWidget(bb);
        v->addLayout(row);
        const bool ok = fx.exec() == QDialog::Accepted;
        if (prev->isChecked()) prev->setChecked(false);
        if (!ok) { giveUp(); return; }
    }
    FxChain nrtChain;
    if (const Event* e = app.project().findEvent(first))
        for (const FxSlot& s : e->fx)
            if (std::find(added.begin(), added.end(), s.id) != added.end()) nrtChain.push_back(s);

    // 3. VEGAS's Apply Non Real-Time FX window: file name and format, then Render
    const std::vector<rt::Format> formats = nrtFormatList();
    if (formats.empty()) { giveUp(); return; }
    QDialog dlg(parent);
    dlg.setObjectName("nonRealTimeFxDialog");
    dlg.setWindowTitle("Apply Non Real-Time FX");
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel(ids.size() > 1 ? QString("The sound of the %1 selected events is processed into new files, one per event, which become new takes.")
                                                 .arg(ids.size())
                                           : QString("The event's sound is processed into a new file, which becomes a new take."),
                            &dlg));
    auto* form = new QFormLayout();
    QLineEdit* name = new QLineEdit(&dlg);
    name->setObjectName("nrtFile");
    form->addRow("Name:", name);
    QLineEdit* folder = new QLineEdit(&dlg);
    folder->setObjectName("nrtFolder");
    folder->setText(qs(nrtDefaultFolder(app.project(), first)));
    QPushButton* browse = new QPushButton("Browse...", &dlg);
    browse->setObjectName("nrtBrowse");
    auto* folderRow = new QHBoxLayout();
    folderRow->addWidget(folder, 1);
    folderRow->addWidget(browse);
    form->addRow("Folder:", folderRow);
    QComboBox* format = new QComboBox(&dlg);
    format->setObjectName("nrtFormat");
    for (const rt::Format& f : formats) format->addItem(f.name, f.key);
    form->addRow("Format:", format);
    QComboBox* templ = new QComboBox(&dlg);
    templ->setObjectName("nrtTemplate");
    form->addRow("Template:", templ);
    v->addLayout(form);
    QLabel* info = new QLabel(&dlg);
    info->setObjectName("nrtInfo");
    info->setWordWrap(true);
    v->addWidget(info);
    bool edited = false;
    auto currentFormat = [&]() -> const rt::Format& { return formats[(size_t)std::max(0, format->currentIndex())]; };
    auto refreshName = [&] {
        if (ids.size() > 1) {
            name->setEnabled(false);
            name->setText("(each event: <media name> Take <number>)");
            return;
        }
        if (edited) return;
        name->setText(qs(nextNrtFile(app.project(), first, s8(folder->text()), s8(currentFormat().ext)).takeName));
    };
    auto refreshInfo = [&] {
        const int k = templ->currentIndex();
        const rt::Format& f = currentFormat();
        info->setText(k >= 0 && k < (int)f.templates.size() ? f.templates[(size_t)k].description : QString());
    };
    auto fillTemplates = [&] {
        templ->clear();
        for (const rt::Tmpl& t : currentFormat().templates) templ->addItem(t.name);
        refreshName();
        refreshInfo();
    };
    QObject::connect(format, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [fillTemplates](int) { fillTemplates(); });
    QObject::connect(templ, QOverload<int>::of(&QComboBox::currentIndexChanged), &dlg, [refreshInfo](int) { refreshInfo(); });
    QObject::connect(name, &QLineEdit::textEdited, &dlg, [&edited](const QString&) { edited = true; });
    QObject::connect(folder, &QLineEdit::textChanged, &dlg, [refreshName](const QString&) { refreshName(); });
    QObject::connect(browse, &QPushButton::clicked, &dlg, [&dlg, folder] {
        const QString f = QFileDialog::getExistingDirectory(&dlg, "Apply Non Real-Time FX", folder->text());
        if (!f.isEmpty()) folder->setText(f);
    });
    fillTemplates();
    QDialogButtonBox* bb = new QDialogButtonBox(&dlg);
    QPushButton* render = bb->addButton("Render", QDialogButtonBox::AcceptRole);
    render->setObjectName("nrtRender");
    bb->addButton(QDialogButtonBox::Cancel);
    QObject::connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);

    while (true) {
        if (dlg.exec() != QDialog::Accepted) { giveUp(); return; }
        NrtOptions o;
        o.parent = parent;
        o.folder = s8(folder->text().trimmed());
        o.format.format = s8(currentFormat().key);
        o.format.templateName = s8(templ->currentText());
        if (ids.size() == 1) o.fileName = s8(name->text().trimmed());
        std::string err;
        if (applyNonRealTimeFxToEvents(ids, nrtChain, o, &err)) {
            // Take dialog edits, effects put on and new takes: one undo step
            app.mergeUndo(app.undoCount() - undo0, "Apply Non-Real-Time Event FX");
            app.status(ids.size() > 1 ? strfmt("Applied the effects into %d new files (new takes).", (int)ids.size())
                                      : s8("Applied the effects into " + QDir::toNativeSeparators(qs(o.folder)) + " as a new take."));
            return;
        }
        if (err == "stopped") break;
        const bool exists = err.find("already exists") != std::string::npos;
        QMessageBox::warning(parent, "Apply Non-Real-Time Event FX", "The effects could not be applied: " + qs(err.empty() ? std::string("no file name") : err));
        if (!exists) break;
        edited = true;      // the name to change is the one typed
    }
    giveUp();
}

// ------------------------------------------------------------------------------------------ Remove Assignable FX
void fillRemoveAssignableMenu(QMenu* m)
{
    m->clear();
    const Project& p = AppState::get().project();
    for (const Bus& b : p.buses) {
        if (!b.assignable) continue;
        const uint64_t id = b.id;
        QAction* a = m->addAction(qs(b.name));
        QObject::connect(a, &QAction::triggered, m, [id] {
            AppState::get().edit("Remove Assignable FX", [&](Project& pr) {
                for (auto& t : pr.tracks) {
                    t.sends.erase(std::remove_if(t.sends.begin(), t.sends.end(), [&](const Send& s) { return s.bus == id; }), t.sends.end());
                    t.envelopes.erase(std::remove_if(t.envelopes.begin(), t.envelopes.end(), [&](const Envelope& e) { return e.kind == "send" && e.target == id; }),
                                      t.envelopes.end());
                    if (t.output == id) t.output = 0;
                }
                for (auto& o : pr.buses)
                    if (o.output == id) o.output = 0;
                pr.buses.erase(std::remove_if(pr.buses.begin(), pr.buses.end(), [&](const Bus& x) { return x.id == id; }), pr.buses.end());
            });
        });
    }
    if (m->isEmpty()) m->addAction("(no assignable FX chains)")->setEnabled(false);
}

namespace {
}

// ================================================================================================== Auto Ducking
bool analyzeAutoDucking(uint64_t controllerId, double from, double to, std::string* error)
{
    AppState& app = AppState::get();
    std::shared_ptr<const Project> snap = app.snapshot();
    const Track* ctl = nullptr;
    for (const Track& t : snap->tracks)
        if (t.id == controllerId && t.type == TrackType::Audio) ctl = &t;
    if (!ctl) {
        if (error) *error = "Auto Ducking needs an audio track";
        return false;
    }
    const double length = snap->length();
    const bool whole = !(to > from);
    if (whole) {
        from = 0;
        to = length;
    }
    if (!(to > from)) {
        if (error) *error = "there is no sound to analyse";
        return false;
    }
    const double floorDb = ctl->duckFloor, peak = ctl->duckPeak;
    // the controller's sound alone: everything else muted
    Project solo = *snap;
    for (Track& t : solo.tracks) {
        t.solo = false;
        if (t.id != controllerId) t.mute = true;
        t.duckListener = false;
    }
    solo.masterMute = false;
    for (Bus& b : solo.buses) b.solo = false;
    const int rate = solo.settings.sampleRate > 0 ? solo.settings.sampleRate : 48000;
    const int step = std::max(1, rate / 50);               // 20 ms
    MediaPool pool;
    if (app.mediaOpener) pool.setOpener(app.mediaOpener);
    AudioEngine engine(pool, app.fx);
    std::vector<float> l((size_t)step), r((size_t)step);
    std::vector<float> levels;
    const int64_t a = (int64_t)std::llround(from * rate), b = (int64_t)std::llround(to * rate);
    for (int64_t at = a; at < b; at += step) {
        std::fill(l.begin(), l.end(), 0.f);
        std::fill(r.begin(), r.end(), 0.f);
        engine.render(solo, at, step, l.data(), r.data(), false);
        double e = 0;
        for (int k = 0; k < step; ++k) e += 0.5 * ((double)l[(size_t)k] * l[(size_t)k] + (double)r[(size_t)k] * r[(size_t)k]);
        levels.push_back((float)(10.0 * std::log10(std::max(1e-12, e / step))));
    }
    std::vector<EnvPoint> pts = duckPoints(levels, (double)step / rate, from, floorDb, peak);
    if (pts.empty()) {
        if (error) *error = "the analysis found nothing";
        return false;
    }
    app.edit("Auto Ducking", [&](Project& p) {
        Track* t = p.findTrack(controllerId);
        if (!t) return;
        t->duckController = true;
        Envelope* env = findEnvelope(t->envelopes, "duck", 0);
        if (!env) {
            Envelope e;
            e.kind = "duck";
            e.visible = true;
            t->envelopes.push_back(e);
            env = &t->envelopes.back();
        }
        if (whole) {
            env->points = pts;
        } else {
            std::vector<EnvPoint> out;
            for (const EnvPoint& o : env->points)
                if (o.time < from - 1e-9) out.push_back(o);
            const double last = pts.back().time;
            for (const EnvPoint& n : pts) out.push_back(n);
            for (const EnvPoint& o : env->points)
                if (o.time > last + 1e-9) out.push_back(o);
            std::stable_sort(out.begin(), out.end(), [](const EnvPoint& x, const EnvPoint& y) { return x.time < y.time; });
            env->points = out;
        }
    });
    return true;
}

bool reanalyzeAutoDucking(uint64_t eventId, std::string* error)
{
    const Project& p = AppState::get().project();
    const Track* t = nullptr;
    const Event* e = p.findEvent(eventId, &t);
    if (!e || !t || t->type != TrackType::Audio || !t->duckController) {
        if (error) *error = "the event is not on an Auto Ducking controller track";
        return false;
    }
    return analyzeAutoDucking(t->id, std::max(0.0, e->start - 0.5), e->end() + 1.0, error);
}

void setAutoDuckingListener(uint64_t trackId, bool on)
{
    AppState::get().edit(on ? "Auto Ducking Listener" : "Auto Ducking Listener Off", [&](Project& p) {
        if (Track* t = p.findTrack(trackId))
            if (t->type == TrackType::Audio) t->duckListener = on;
    });
}

void removeAutoDuckingController(uint64_t trackId)
{
    AppState::get().edit("Remove Auto Ducking", [&](Project& p) {
        Track* t = p.findTrack(trackId);
        if (!t) return;
        t->duckController = false;
        t->envelopes.erase(std::remove_if(t->envelopes.begin(), t->envelopes.end(), [](const Envelope& e) { return e.kind == "duck"; }), t->envelopes.end());
    });
}

void showAutoDuckingDialog(QWidget* parent, uint64_t trackId)
{
    AppState& app = AppState::get();
    const Track* t = nullptr;
    for (const Track& x : app.project().tracks)
        if (x.id == trackId && x.type == TrackType::Audio) t = &x;
    if (!t) return;
    QDialog dlg(parent);
    dlg.setObjectName("autoDucking");
    dlg.setWindowTitle("Auto Ducking");
    auto* v = new QVBoxLayout(&dlg);
    v->addWidget(new QLabel("This track's sound lowers the volume of tracks set as Auto Ducking listeners.", &dlg));
    auto* form = new QFormLayout();
    auto* floor = new QSlider(Qt::Horizontal, &dlg);
    floor->setObjectName("floorReduction");
    floor->setRange(-60, 0);
    floor->setValue((int)std::lround(t->duckController ? t->duckFloor : -12.0));
    auto* floorText = new QLabel(&dlg);
    auto* peak = new QSlider(Qt::Horizontal, &dlg);
    peak->setObjectName("peakReduction");
    peak->setRange(0, 100);
    peak->setValue((int)std::lround(t->duckPeak * 100.0));
    auto* peakText = new QLabel(&dlg);
    auto show = [=] {
        floorText->setText(QString("%1 dB").arg(floor->value()));
        peakText->setText(QString("%1 %").arg(peak->value()));
    };
    show();
    QObject::connect(floor, &QSlider::valueChanged, &dlg, show);
    QObject::connect(peak, &QSlider::valueChanged, &dlg, show);
    auto row = [&](const QString& label, QSlider* s, QLabel* text) {
        auto* h = new QHBoxLayout();
        h->addWidget(s, 1);
        h->addWidget(text);
        text->setMinimumWidth(50);
        form->addRow(label, h);
    };
    row("&Floor Reduction:", floor, floorText);
    row("&Peak Reduction:", peak, peakText);
    v->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;
    const double fl = floor->value(), pk = peak->value() / 100.0;
    app.edit("Auto Ducking Controls", [&](Project& p) {
        if (Track* x = p.findTrack(trackId)) {
            x->duckController = true;
            x->duckFloor = fl;
            x->duckPeak = pk;
        }
    });
    std::string err;
    if (!analyzeAutoDucking(trackId, 0, 0, &err)) app.status("Auto Ducking: " + err);
    else app.status("Auto Ducking: controller envelope created. Set the tracks to lower as Auto Ducking Listeners.");
}

// ================================================================================================== sound reading
bool readMonoSound(const MediaItem& m, double from, double to, int rate, std::vector<float>& out, std::string* error, double maxSeconds)
{
    out.clear();
    AppState& app = AppState::get();
    if (m.generated || m.path.empty() || !m.info.hasAudio) {
        if (error) *error = "the media has no sound";
        return false;
    }
    std::string err;
    std::unique_ptr<MediaReader> r = app.mediaOpener ? app.mediaOpener(m.readPath(), &err) : openMediaOrProject(m.readPath(), &err);
    if (!r) {
        if (error) *error = err.empty() ? "the file cannot be opened" : err;
        return false;
    }
    const double dur = m.info.duration > 0 ? m.info.duration : 600.0;
    if (!(to > from)) {
        from = 0;
        to = dur;
    }
    to = std::min(to, from + maxSeconds);
    const int chunk = 16384;
    std::vector<float> l((size_t)chunk), rr((size_t)chunk);
    int64_t pos = (int64_t)std::llround(std::max(0.0, from) * rate);
    const int64_t end = (int64_t)std::llround(to * rate);
    out.reserve((size_t)std::max<int64_t>(0, end - pos));
    while (pos < end) {
        const int n = (int)std::min<int64_t>(chunk, end - pos);
        if (!r->audio(pos, n, rate, l.data(), rr.data())) break;
        for (int k = 0; k < n; ++k) out.push_back(0.5f * (l[(size_t)k] + rr[(size_t)k]));
        pos += n;
    }
    if (out.empty()) {
        if (error) *error = "no sound could be read from the file";
        return false;
    }
    return true;
}

// ================================================================================================== Beat Detection
void setBeatMarkers(uint64_t mediaId, const std::vector<double>& times)
{
    AppState::get().edit("Beat Detection", [&](Project& p) {
        for (MediaItem& m : p.media) {
            if (m.id != mediaId) continue;
            m.markers.erase(std::remove_if(m.markers.begin(), m.markers.end(), [](const MediaItem::Marker& k) { return k.length == 0 && k.name == "Beat"; }), m.markers.end());
            for (double t : times) {
                MediaItem::Marker k;
                k.time = t;
                k.name = "Beat";
                m.markers.push_back(k);
            }
            std::sort(m.markers.begin(), m.markers.end(), [](const MediaItem::Marker& a, const MediaItem::Marker& b) { return a.time < b.time; });
        }
    });
}

namespace {

// A strip showing the sound's loudness with the hits marked.
class BeatStrip : public QWidget {
public:
    BeatStrip(QWidget* parent) : QWidget(parent) { setMinimumHeight(110); setObjectName("beatStrip"); }
    std::vector<float> level;       // dB, 10 ms steps
    std::vector<double> beats;
    double duration = 1;
protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(24, 24, 26));
        const int w = width(), h = height();
        if (level.empty()) return;
        p.setPen(QColor(90, 150, 220));
        for (int x = 0; x < w; ++x) {
            const size_t a = (size_t)((double)x / w * level.size()), b = std::min(level.size(), (size_t)((double)(x + 1) / w * level.size()) + 1);
            float m = -120;
            for (size_t i = a; i < b; ++i) m = std::max(m, level[i]);
            const double f = std::min(1.0, std::max(0.0, (m + 70.0) / 70.0));
            p.drawLine(x, h, x, h - (int)(f * (h - 4)));
        }
        p.setPen(QColor(255, 170, 60));
        for (double t : beats) {
            const int x = (int)(t / duration * w);
            p.drawLine(x, 0, x, h);
        }
    }
};

}  // namespace

void showBeatDetectionDialog(QWidget* parent, uint64_t mediaId)
{
    AppState& app = AppState::get();
    const MediaItem* m = app.project().findMedia(mediaId);
    if (!m || m->generated || !m->info.hasAudio) {
        app.status("Beat Detection works on media with sound.");
        return;
    }
    const MediaItem media = *m;
    std::vector<float> mono;
    std::string err;
    const int rate = 22050;
    if (!readMonoSound(media, 0, 0, rate, mono, &err)) {
        app.status("Beat Detection: " + err);
        return;
    }
    QDialog dlg(parent);
    dlg.setObjectName("beatDetection");
    dlg.setWindowTitle("Beat Detection - " + qs(media.name));
    dlg.resize(620, 260);
    auto* v = new QVBoxLayout(&dlg);
    auto* strip = new BeatStrip(&dlg);
    strip->level = lowBandLevels(mono, rate, 0.01);
    strip->duration = std::max(0.01, (double)mono.size() / rate);
    v->addWidget(strip, 1);
    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel("Sensitivity:", &dlg));
    auto* sens = new QSlider(Qt::Horizontal, &dlg);
    sens->setObjectName("sensitivity");
    sens->setRange(0, 100);
    sens->setValue(QSettings().value("beatDetection/sensitivity", 50).toInt());
    sens->setToolTip("Increase the sensitivity to reduce the number of markers");
    row->addWidget(sens, 1);
    auto* count = new QLabel(&dlg);
    count->setObjectName("beatCount");
    count->setMinimumWidth(110);
    row->addWidget(count);
    v->addLayout(row);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    auto update = [&] {
        strip->beats = detectBeats(mono, rate, sens->value() / 100.0);
        count->setText(QString("%1 markers").arg(strip->beats.size()));
        strip->update();
    };
    update();
    QObject::connect(sens, &QSlider::valueChanged, &dlg, update);
    if (dlg.exec() != QDialog::Accepted) return;
    QSettings().setValue("beatDetection/sensitivity", sens->value());
    setBeatMarkers(mediaId, strip->beats);
    app.status(strfmt("Beat Detection: %d markers on %s (View > Event Media Markers).", (int)strip->beats.size(), media.name.c_str()));
}

// ================================================================================================== Tempo Detection
void applyDetectedTempo(double bpm, const TempoChoices& c)
{
    AppState& app = AppState::get();
    if (!(bpm > 0)) return;
    if (c.setTempo || c.rulerMeasures) {
        app.edit("Tempo Detection", [&](Project& p) {
            if (c.setTempo) p.settings.tempo = bpm;
            if (c.rulerMeasures) p.settings.rulerFormat = "measures";
        });
    }
    if (c.snapToGrid) { app.prefs.snapping = true; app.prefs.snapToGrid = true; app.applyPreferences(); }
    if (c.metronome) {
        // Options > Metronome (vegasextras.cpp): its switch, found in the main window
        bool done = false;
        for (QWidget* w : QApplication::topLevelWidgets())
            if (QAction* a = w->findChild<QAction*>("metronome")) {
                a->setChecked(true);
                done = true;
                break;
            }
        if (!done) QSettings().setValue("options/metronome", true);
    }
}

void showTempoDetectionDialog(QWidget* parent, uint64_t mediaId)
{
    AppState& app = AppState::get();
    const MediaItem* m = app.project().findMedia(mediaId);
    if (!m || m->generated || !m->info.hasAudio) {
        app.status("Tempo Detection works on media with sound.");
        return;
    }
    const MediaItem media = *m;
    std::vector<float> mono;
    std::string err;
    const int rate = 22050;
    if (!readMonoSound(media, 0, 0, rate, mono, &err, 240.0)) {
        app.status("Tempo Detection: " + err);
        return;
    }
    double conf = 0;
    const double bpm = detectTempo(mono, rate, &conf);
    QDialog dlg(parent);
    dlg.setObjectName("tempoDetection");
    dlg.setWindowTitle("Tempo Detection - " + qs(media.name));
    auto* v = new QVBoxLayout(&dlg);
    auto* tempo = new QLabel(bpm > 0 ? QString("Detected tempo: %1 BPM").arg(bpm, 0, 'f', 2) : QString("No tempo could be detected."), &dlg);
    tempo->setObjectName("detectedTempo");
    QFont f = tempo->font();
    f.setBold(true);
    f.setPointSizeF(f.pointSizeF() * 1.3);
    tempo->setFont(f);
    v->addWidget(tempo);
    auto box = [&](const char* name, const QString& text, bool on) {
        auto* c = new QCheckBox(text, &dlg);
        c->setObjectName(name);
        c->setChecked(on && bpm > 0);
        c->setEnabled(bpm > 0);
        v->addWidget(c);
        return c;
    };
    QSettings s;
    auto* setTempo = box("setTempo", "Set as project tempo", s.value("tempoDetection/setTempo", true).toBool());
    auto* ruler = box("rulerMeasures", "Set ruler time format to measures and beats", s.value("tempoDetection/ruler", true).toBool());
    auto* metro = box("metronome", "Activate metronome", s.value("tempoDetection/metronome", false).toBool());
    auto* snap = box("snapGrid", "Enable snapping to grid", s.value("tempoDetection/snap", true).toBool());
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    v->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted || !(bpm > 0)) return;
    s.setValue("tempoDetection/setTempo", setTempo->isChecked());
    s.setValue("tempoDetection/ruler", ruler->isChecked());
    s.setValue("tempoDetection/metronome", metro->isChecked());
    s.setValue("tempoDetection/snap", snap->isChecked());
    TempoChoices c;
    c.setTempo = setTempo->isChecked();
    c.rulerMeasures = ruler->isChecked();
    c.metronome = metro->isChecked();
    c.snapToGrid = snap->isChecked();
    applyDetectedTempo(bpm, c);
}

}  // namespace app
}  // namespace reno
