// The first part of VEGAS Pro 18's manual in the main window: see vegasextras.h.
#include "vegasextras.h"
#include "fxlibrary.h"
#include "mainwindow.h"
#include "playback.h"
#include "timeline.h"
#include "vprefs.h"
#include "../engine/timefmt.h"
#include "../media/media.h"
#include "../vegas/vegas.h"
#include "menuutil.h"
#include "strconv.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QElapsedTimer>
#include <QSpinBox>
#include <QTextStream>
#include <QTimer>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace reno {
namespace app {

void runCustomizeToolbar(QMainWindow* w, bool timeline);
void installToolbars(QMainWindow* w);

namespace {

QAction* findItem(QMenu* m, const QString& objectName)
{
    if (!m) return nullptr;
    for (QAction* a : m->actions())
        if (a->objectName() == objectName) return a;
    return nullptr;
}

QAction* command(QWidget* owner, const char* name, const QString& text, const QString& tip, std::function<void()> fn)
{
    QAction* a = new QAction(text, owner);
    a->setObjectName(name);
    a->setStatusTip(tip);
    a->setToolTip(plainMenuText(text) + (tip.isEmpty() ? QString() : ": " + tip));
    QObject::connect(a, &QAction::triggered, owner, [fn] { fn(); });
    owner->addAction(a);
    return a;
}

QAction* unavailable(QWidget* owner, const char* name, const QString& text, const QString& why)
{
    QAction* a = new QAction(text, owner);
    a->setObjectName(name);
    a->setEnabled(false);
    a->setStatusTip(why);
    a->setToolTip(plainMenuText(text) + ": " + why);
    a->setWhatsThis(why);
    return a;
}

void insertBefore(QMenu* m, QAction* a, const QString& before)
{
    if (!m) return;
    m->insertAction(findItem(m, before), a);
}

void insertAfter(QMenu* m, QAction* a, const QString& after)
{
    if (!m) return;
    const QList<QAction*> all = m->actions();
    for (int k = 0; k < all.size(); ++k)
        if (all[k]->objectName() == after) {
            m->insertAction(k + 1 < all.size() ? all[k + 1] : nullptr, a);
            return;
        }
    m->addAction(a);
}

double fpsOf(const Project& p) { return std::max(1.0, p.settings.fps()); }

}  // namespace

void matchProjectVideoToMedia(uint64_t mediaId, const std::string& what)
{
    AppState& app = AppState::get();
    const MediaItem* m = app.project().findMedia(mediaId);
    if (!m || !m->info.hasVideo || m->info.width <= 0 || m->info.height <= 0) return;
    const MediaInfo info = m->info;
    app.edit(what, [&](Project& p) {
        p.settings.width = info.width & ~1;
        p.settings.height = info.height & ~1;
        if (info.fps > 0.5 && !info.isStill) {
            const double f = info.fps;
            if (std::fabs(f - std::round(f)) < 1e-3) { p.settings.fpsNum = (int)std::lround(f); p.settings.fpsDen = 1; }
            else if (std::fabs(f * 1.001 - std::round(f * 1.001)) < 1e-3) { p.settings.fpsNum = (int)std::lround(f * 1.001) * 1000; p.settings.fpsDen = 1001; }
            else { p.settings.fpsNum = (int)std::lround(f * 1000); p.settings.fpsDen = 1000; }
        }
        p.settings.fieldOrder = info.fieldOrder == "upper" || info.fieldOrder == "lower" ? info.fieldOrder : "none";
    });
}

namespace {

// Preferences > General "Prompt to adjust project settings to match first media added to timeline": the first video event
// put into a project with none asks whether the project should take the media's size and frame rate.
class MatchMediaPrompt : public QObject {
public:
    explicit MatchMediaPrompt(QWidget* window) : QObject(window), window_(window)
    {
        AppState& app = AppState::get();
        hadVideo_ = videoEvents() > 0;
        redo_ = app.redoNames(100000).size();
        path_ = app.path();
        listener_ = app.addListener([this](int changes) { changed(changes); });
    }
    ~MatchMediaPrompt() override { AppState::get().removeListener(listener_); }

private:
    int videoEvents() const
    {
        int n = 0;
        for (const Track& t : AppState::get().project().tracks)
            if (t.type == TrackType::Video) n += (int)t.events.size();
        return n;
    }
    void changed(int changes)
    {
        AppState& app = AppState::get();
        if (!(changes & (kProject | kFile))) return;
        const bool has = videoEvents() > 0;
        const size_t redo = app.redoNames(100000).size();
        const bool fresh = app.path() != path_ || (changes & kFile) || redo != redo_;
        path_ = app.path();
        redo_ = redo;
        const bool first = has && !hadVideo_ && !fresh;
        hadVideo_ = has;
        const bool always = vprefs::on("prefs/alwaysBaseOnFirst");
        if (!first || asking_ || (!always && !vprefs::on("prefs/promptMatchMedia"))) return;
        uint64_t media = 0;
        for (const Track& t : app.project().tracks)
            if (t.type == TrackType::Video)
                for (const Event& e : t.events)
                    if (!media) media = e.mediaId;
        const MediaItem* m = app.project().findMedia(media);
        if (!m || m->generated || !m->info.hasVideo || m->info.isStill || m->info.width <= 0) return;
        const ProjectSettings& s = app.project().settings;
        const bool same = m->info.width == s.width && m->info.height == s.height && (m->info.fps <= 0 || std::fabs(m->info.fps - s.fps()) < 0.01);
        if (same) return;
        if (always) {
            matchProjectVideoToMedia(media, "Match Media Video Settings");
            return;
        }
        asking_ = true;
        QTimer::singleShot(0, this, [this, media] {
            AppState& a = AppState::get();
            const MediaItem* mm = a.project().findMedia(media);
            if (!mm) { asking_ = false; return; }
            QMessageBox box(QMessageBox::Question, "Match Media Video Settings",
                            QString("Do you want to set your project video settings to match this media?"),
                            QMessageBox::Yes | QMessageBox::No, window_);
            box.setObjectName("matchMediaPrompt");
            QCheckBox* again = new QCheckBox("Always base project video settings on first-added video", &box);
            again->setObjectName("alwaysBase");
            box.setCheckBox(again);
            const bool yes = box.exec() == QMessageBox::Yes;
            if (again->isChecked()) {
                vprefs::set("prefs/promptMatchMedia", false);
                vprefs::set("prefs/alwaysBaseOnFirst", yes);
            }
            if (yes) matchProjectVideoToMedia(media, "Match Media Video Settings");
            asking_ = false;
        });
    }
    QWidget* window_;
    int listener_ = 0;
    bool hadVideo_ = false, asking_ = false;
    size_t redo_ = 0;
    std::string path_;
};

}  // namespace

static QString searchForFile(const QString& oldPath, const QString& projectFile)
{
    const QString name = QFileInfo(oldPath).fileName();
    QStringList roots;
    roots << QFileInfo(oldPath).absolutePath();
    if (!projectFile.isEmpty()) roots << QFileInfo(projectFile).absolutePath();
    for (const QString& r : roots) {
        if (QFileInfo::exists(QDir(r).filePath(name))) return QDir(r).filePath(name);
        if (!QFileInfo(r).isDir()) continue;
        QDirIterator it(r, QStringList() << name, QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) return it.next();
    }
    return QString();
}

int resolveMissingMedia(QWidget* parent, const std::function<int(const QString&, QString*)>& choose)
{
    AppState& app = AppState::get();
    std::vector<std::pair<uint64_t, QString>> missing;
    std::set<std::string> seenPaths;
    for (const MediaItem& m : app.project().media)
        if (m.offline && !m.generated && !m.path.empty() && seenPaths.insert(m.path).second) missing.push_back({m.id, QString::fromStdString(m.path)});
    std::map<std::string, std::string> relink;
    bool ignoreAll = false;
    for (const auto& mm : missing) {
        if (ignoreAll) break;
        int what = 0;
        QString file;
        if (choose) {
            what = choose(mm.second, &file);
        } else {
            QDialog dlg(parent);
            dlg.setObjectName("missingMediaDialog");
            dlg.setWindowTitle("Missing Media");
            auto* lay = new QVBoxLayout(&dlg);
            lay->addWidget(new QLabel("The following file could not be found in the specified location:", &dlg));
            auto* path = new QLabel(mm.second, &dlg);
            path->setObjectName("missingPath");
            path->setTextInteractionFlags(Qt::TextSelectableByMouse);
            path->setContentsMargins(16, 4, 0, 4);
            lay->addWidget(path);
            lay->addWidget(new QLabel("What do you want to do?", &dlg));
            const char* const labels[] = {"Search for missing file", "Specify a new location or replacement file", "Recapture media",
                                          "Recapture all offline media", "Ignore missing file and leave it offline",
                                          "Ignore all missing files and leave them offline"};
            QRadioButton* radios[6];
            for (int i = 0; i < 6; ++i) {
                radios[i] = new QRadioButton(labels[i], &dlg);
                radios[i]->setObjectName(QString("missingChoice%1").arg(i));
                lay->addWidget(radios[i]);
            }
            radios[0]->setChecked(true);
            radios[2]->setEnabled(false);
            radios[3]->setEnabled(false);
            auto* row = new QHBoxLayout;
            row->addStretch(1);
            auto* ok = new QPushButton("OK", &dlg);
            auto* cancel = new QPushButton("Cancel", &dlg);
            ok->setDefault(true);
            row->addWidget(ok);
            row->addWidget(cancel);
            lay->addLayout(row);
            QObject::connect(ok, &QPushButton::clicked, &dlg, &QDialog::accept);
            QObject::connect(cancel, &QPushButton::clicked, &dlg, &QDialog::reject);
            if (dlg.exec() != QDialog::Accepted) break;
            what = radios[1]->isChecked() ? 1 : radios[4]->isChecked() ? 2 : radios[5]->isChecked() ? 3 : 0;
            if (what == 1)
                file = QFileDialog::getOpenFileName(parent, "Specify a New Location or Replacement File", QFileInfo(mm.second).absolutePath());
        }
        if (what == 3) { ignoreAll = true; break; }
        if (what == 2) continue;
        if (what == 0) file = searchForFile(mm.second, QString::fromStdString(app.path()));
        if (file.isEmpty() || !QFileInfo::exists(file)) continue;
        relink[mm.second.toStdString()] = file.toStdString();
    }
    if (relink.empty()) return 0;
    std::map<uint64_t, MediaInfo> infos;
    for (const MediaItem& m : app.project().media) {
        auto it = relink.find(m.path);
        if (it == relink.end() || !app.mediaOpener) continue;
        std::string err;
        std::unique_ptr<MediaReader> r;
        try { r = app.mediaOpener(it->second, &err); } catch (...) {}
        if (r) infos[m.id] = r->info();
    }
    int n = 0;
    app.edit("Relink Missing Media", [&](Project& p) {
        for (MediaItem& m : p.media) {
            auto it = relink.find(m.path);
            if (it == relink.end()) continue;
            m.path = it->second;
            m.offline = false;
            auto inf = infos.find(m.id);
            if (inf != infos.end()) {
                const double sub = m.subclipLength;
                m.info = inf->second;
                if (sub > 0) m.info.duration = std::max(0.0, std::min(sub, inf->second.duration - m.subclipStart));
            }
            ++n;
        }
    });
    return n;
}

static const char* const kEdlColumns[] = {"ID", "Track", "StartTime", "Length", "PlayRate", "Locked", "Normalized", "StretchMethod", "Looped",
                                          "OnRuler", "MediaType", "FileName", "Stream", "StreamStart", "StreamLength", "FadeTimeIn", "FadeTimeOut",
                                          "SustainGain", "CurveIn", "GainIn", "CurveOut", "GainOut", "Layer", "Color", "CurveInR", "CurveOutR",
                                          "PlayPitch", "LockPitch", "FirstChannel", "Channels"};

bool writeEdl(const Project& p, const QString& file, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    QTextStream out(&f);
    QStringList head;
    for (const char* c : kEdlColumns) head << QString("\"%1\"").arg(c);
    out << head.join(";") << "\n";
    int id = 1, trackNo = 0;
    auto ms = [](double s) { return QString::number(s * 1000.0, 'f', 4); };
    for (const Track& t : p.tracks) {
        for (const Event& e : t.events) {
            const MediaItem* m = p.findMedia(e.mediaId);
            const QString name = m ? QDir::toNativeSeparators(qs(m->generated ? m->name : m->path)) : QString();
            const double streamLength = e.length * e.rate;
            QStringList v;
            v << QString::number(id++) << QString::number(trackNo) << ms(e.start) << ms(e.length) << QString::number(e.rate, 'f', 6)
              << (e.locked ? "TRUE" : "FALSE") << (e.normalize ? "TRUE" : "FALSE") << "0" << (e.loop ? "TRUE" : "FALSE") << "FALSE"
              << (t.type == TrackType::Video ? "VIDEO" : "AUDIO") << QString("\"%1\"").arg(name) << "0" << ms(e.offset) << ms(streamLength)
              << ms(e.fadeIn) << ms(e.fadeOut) << QString::number(e.gain, 'f', 6) << QString::number(e.fadeInCurve) << "0.000000"
              << QString::number(e.fadeOutCurve) << "0.000000" << "0" << "-1" << QString::number(e.fadeInCurve) << QString::number(e.fadeOutCurve)
              << QString::number(e.pitch, 'f', 6) << (e.keepPitch ? "TRUE" : "FALSE") << "0" << "0";
            out << v.join("; ") << "\n";
        }
        ++trackNo;
    }
    f.close();
    if (f.error() != QFile::NoError) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool readEdl(const QString& file, Project& out, QString* error)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return false;
    }
    QTextStream in(&f);
    const QString headLine = in.readLine();
    QStringList head;
    for (QString h : headLine.split(';')) head << h.trimmed().remove('"');
    auto col = [&head](const char* n) { return head.indexOf(n); };
    const int cTrack = col("Track"), cStart = col("StartTime"), cLength = col("Length"), cRate = col("PlayRate"), cType = col("MediaType"),
              cFile = col("FileName"), cStreamStart = col("StreamStart"), cFadeIn = col("FadeTimeIn"), cFadeOut = col("FadeTimeOut"),
              cGain = col("SustainGain"), cLooped = col("Looped"), cLocked = col("Locked");
    if (cStart < 0 || cLength < 0 || cType < 0 || cFile < 0) {
        if (error) *error = "This is not a VEGAS EDL text file (its first line does not name VEGAS's columns).";
        return false;
    }
    Project p;
    p.settings = AppState::get().project().settings;
    std::map<QString, uint64_t> mediaByFile;
    std::map<int, uint64_t> audioTracks;
    uint64_t videoTrack = 0;
    auto trackFor = [&](bool video, int edlTrack) -> uint64_t {
        if (video) {
            if (!videoTrack) {
                Track t;
                t.id = videoTrack = p.newId();
                t.type = TrackType::Video;
                p.tracks.insert(p.tracks.begin(), t);
            }
            return videoTrack;
        }
        auto it = audioTracks.find(edlTrack);
        if (it != audioTracks.end()) return it->second;
        if (audioTracks.size() >= 4) return audioTracks.rbegin()->second;
        Track t;
        t.id = p.newId();
        t.type = TrackType::Audio;
        p.tracks.push_back(t);
        audioTracks[edlTrack] = t.id;
        return t.id;
    };
    int lines = 0;
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        QStringList v;
        QString cur;
        bool quoted = false;
        for (QChar c : line) {
            if (c == '"') { quoted = !quoted; continue; }
            if (c == ';' && !quoted) { v << cur.trimmed(); cur.clear(); continue; }
            cur += c;
        }
        v << cur.trimmed();
        auto num = [&v](int c, double d) { bool ok = false; const double x = c >= 0 && c < v.size() ? v[c].toDouble(&ok) : d; return ok ? x : d; };
        auto flag = [&v](int c, bool d) { return c >= 0 && c < v.size() ? v[c].compare("TRUE", Qt::CaseInsensitive) == 0 : d; };
        if (cFile >= v.size() || cType >= v.size()) continue;
        const bool video = v[cType].compare("VIDEO", Qt::CaseInsensitive) == 0;
        const QString path = QDir::fromNativeSeparators(v[cFile]);
        QString found = path;
        if (!QFileInfo::exists(found)) found = QFileInfo(file).absoluteDir().filePath(QFileInfo(path).fileName());
        uint64_t mid = 0;
        auto mit = mediaByFile.find(found);
        if (mit != mediaByFile.end()) mid = mit->second;
        else {
            MediaItem m;
            m.id = mid = p.newId();
            m.path = s8(found);
            m.name = s8(QFileInfo(found).fileName());
            std::string err;
            std::unique_ptr<MediaReader> r = QFileInfo::exists(found) && AppState::get().mediaOpener ? AppState::get().mediaOpener(m.path, &err) : nullptr;
            if (r) m.info = r->info();
            else m.offline = true;
            p.media.push_back(m);
            mediaByFile[found] = mid;
        }
        Event e;
        e.id = p.newId();
        e.mediaId = mid;
        e.start = num(cStart, 0) / 1000.0;
        e.length = std::max(0.001, num(cLength, 1000) / 1000.0);
        e.rate = std::max(0.01, num(cRate, 1.0));
        e.offset = num(cStreamStart, 0) / 1000.0;
        e.fadeIn = num(cFadeIn, 0) / 1000.0;
        e.fadeOut = num(cFadeOut, 0) / 1000.0;
        e.gain = num(cGain, 1.0);
        e.loop = flag(cLooped, true);
        e.locked = flag(cLocked, false);
        e.name = p.findMedia(mid) ? p.findMedia(mid)->name : std::string();
        setDefaultFadeCurves(e, video ? TrackType::Video : TrackType::Audio);
        const uint64_t tid = trackFor(video, (int)num(cTrack, 0));
        if (Track* t = p.findTrack(tid)) t->events.push_back(e);
        ++lines;
    }
    for (Track& t : p.tracks) std::sort(t.events.begin(), t.events.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
    if (!lines) {
        if (error) *error = "The EDL holds no events.";
        return false;
    }
    out = p;
    return true;
}

bool archiveProject(const QString& projectFile, const ArchiveOptions& o, QString* error, int* copied)
{
    AppState& app = AppState::get();
    const QFileInfo target(projectFile);
    const QString folder = target.absolutePath();
    if (!QDir().mkpath(folder)) {
        if (error) *error = "The folder could not be made: " + folder;
        return false;
    }
    Project copy = app.project();
    app.fx.captureStates(copy);
    std::set<uint64_t> used;
    for (const Track& t : copy.tracks)
        for (const Event& e : t.events) {
            used.insert(e.mediaId);
            for (const Take& k : e.takes) used.insert(k.mediaId);
        }
    int n = 0;
    std::map<QString, QString> done;
    auto copyFile = [&](const QString& src) -> QString {
        if (src.isEmpty() || !QFileInfo::exists(src)) return QString();
        auto it = done.find(src);
        if (it != done.end()) return it->second;
        QString dst = QDir(folder).filePath(QFileInfo(src).fileName());
        for (int k = 2; QFileInfo::exists(dst) && QFileInfo(dst).size() != QFileInfo(src).size(); ++k)
            dst = QDir(folder).filePath(QString("%1 (%2).%3").arg(QFileInfo(src).completeBaseName()).arg(k).arg(QFileInfo(src).suffix()));
        if (!QFileInfo::exists(dst) && QFileInfo(src).absoluteFilePath() != QFileInfo(dst).absoluteFilePath()) {
            if (!QFile::copy(src, dst)) return QString();
            ++n;
        }
        done[src] = dst;
        return dst;
    };
    std::vector<MediaItem> kept;
    for (MediaItem& m : copy.media) {
        if (o.excludeUnused && !used.count(m.id)) continue;
        if (!m.generated && !m.path.empty()) {
            const QString src = qs(m.path);
            const bool nested = src.endsWith(".veg", Qt::CaseInsensitive) || src.endsWith(".reno", Qt::CaseInsensitive);
            if ((nested && o.nested) || (!nested && o.media)) {
                const QString dst = copyFile(src);
                if (!dst.isEmpty()) {
                    m.path = s8(dst);
                    if (o.proxies)
                        for (const char* ext : {".sfk", ".sfap0", ".sfvp0", ".sfl"}) copyFile(src + ext);
                }
            }
        }
        kept.push_back(m);
    }
    copy.media = kept;
    if (projectFile.endsWith(".veg", Qt::CaseInsensitive)) {
        const VegasSaveResult r = saveVegasProject(copy, s8(projectFile));
        if (!r.ok) {
            if (error) *error = qs(r.error);
            return false;
        }
    } else {
        std::string err;
        if (!copy.save(s8(projectFile), &err)) {
            if (error) *error = qs(err);
            return false;
        }
    }
    if (copied) *copied = n;
    return true;
}

int importMediaFromProject(const QString& file, bool bins, bool merge, QString* error)
{
    Project other;
    std::string err;
    if (!loadProjectFile(s8(file), other, &err)) {
        if (error) *error = qs(err);
        return 0;
    }
    AppState& app = AppState::get();
    std::set<std::string> have;
    for (const MediaItem& m : app.project().media)
        if (!m.path.empty()) have.insert(m.path + "|" + std::to_string(m.subclipStart) + "|" + std::to_string(m.subclipLength));
    const std::string under = merge ? std::string() : s8(QFileInfo(file).completeBaseName());
    std::vector<MediaItem> add;
    for (const MediaItem& m : other.media) {
        if (m.generated || m.path.empty()) continue;
        if (m.offline || !QFileInfo::exists(qs(m.path))) continue;
        if (have.count(m.path + "|" + std::to_string(m.subclipStart) + "|" + std::to_string(m.subclipLength))) continue;
        MediaItem c = m;
        c.veg.reset();
        if (!bins) c.bin.clear();
        else if (!under.empty()) c.bin = c.bin.empty() ? under : under + "/" + c.bin;
        add.push_back(c);
    }
    if (add.empty()) {
        if (error) *error = "That project has no media this project does not have already (generated and offline media are left out).";
        return 0;
    }
    app.edit("Import Media from Project", [&](Project& p) {
        for (MediaItem& m : add) {
            m.id = p.newId();
            p.media.push_back(m);
        }
    });
    return (int)add.size();
}

bool bwfTimeReference(const QString& file, double* seconds)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(12);
    if (head.size() < 12 || (head.left(4) != "RIFF" && head.left(4) != "RF64") || head.mid(8, 4) != "WAVE") return false;
    quint32 rate = 0;
    quint64 ref = 0;
    bool gotRef = false;
    while (!f.atEnd()) {
        const QByteArray ch = f.read(8);
        if (ch.size() < 8) break;
        const QByteArray id = ch.left(4);
        const quint32 size = (quint8)ch[4] | ((quint8)ch[5] << 8) | ((quint8)ch[6] << 16) | ((quint32)(quint8)ch[7] << 24);
        const qint64 next = f.pos() + size + (size & 1);
        if (id == "fmt ") {
            const QByteArray d = f.read(std::min<quint32>(size, 16));
            if (d.size() >= 8) rate = (quint8)d[4] | ((quint8)d[5] << 8) | ((quint8)d[6] << 16) | ((quint32)(quint8)d[7] << 24);
        } else if (id == "bext" && size >= 346) {
            // Description 256, Originator 32, OriginatorReference 32, OriginationDate 10, OriginationTime 8, then
            // TimeReferenceLow and High: samples since midnight
            const QByteArray d = f.read(346);
            if (d.size() == 346) {
                for (int k = 7; k >= 0; --k) ref = (ref << 8) | (quint8)d[338 + k];
                gotRef = true;
            }
        } else if (id == "data") {
            if (rate && gotRef) break;
        }
        if (!f.seek(next)) break;
    }
    if (!gotRef || !rate) return false;
    if (seconds) *seconds = (double)ref / rate;
    return true;
}

int importBroadcastWave(const QStringList& files, int arrange, int order, bool relativeToCursor, QString* error)
{
    AppState& app = AppState::get();
    struct F {
        QString path;
        double ref = 0;
        uint64_t media = 0;
    };
    std::vector<F> list;
    QStringList failed;
    for (const QString& f : files) {
        F x;
        x.path = f;
        bwfTimeReference(f, &x.ref);
        std::string err;
        x.media = app.importMedia(s8(f), &err);
        if (!x.media) { failed << QFileInfo(f).fileName(); continue; }
        list.push_back(x);
    }
    if (list.empty()) {
        if (error) *error = "None of the files could be read: " + failed.join(", ");
        return 0;
    }
    if (order == 0) std::stable_sort(list.begin(), list.end(), [](const F& a, const F& b) { return a.ref < b.ref; });
    else std::stable_sort(list.begin(), list.end(), [](const F& a, const F& b) { return QFileInfo(a.path).fileName().compare(QFileInfo(b.path).fileName(), Qt::CaseInsensitive) < 0; });
    const double base = relativeToCursor ? app.cursor() : 0.0;
    const double rulerStart = app.project().settings.rulerStart;
    int made = 0;
    uint64_t track = 0;
    for (const F& x : list) {
        double at = relativeToCursor ? base + x.ref : x.ref - rulerStart;
        at = std::max(0.0, at);
        if (arrange == 0 || !track) {
            track = app.addTrack(TrackType::Audio);
        }
        made += (int)app.addMediaToTimeline(x.media, at, track).size();
    }
    if (!failed.isEmpty() && error) *error = "Could not be read: " + failed.join(", ");
    return made;
}

static double parseSubTime(const QString& t, double fps)
{
    double v = -1;
    const ProjectSettings& st = AppState::get().project().settings;
    if (parseTimeText(st, s8(t.trimmed()), "smpte", &v) && v >= 0) return v;
    if (parseTimeText(st, s8(t.trimmed()), st.rulerFormat, &v) && v >= 0) return v;
    const QStringList parts = QString(t).replace(';', ':').split(':');
    bool ok = false;
    if (parts.size() == 1) { const double s = parts[0].toDouble(&ok); return ok ? s : -1; }
    if (parts.size() >= 3) {
        const double h = parts[0].toDouble(), m = parts[1].toDouble();
        const double s = parts[2].toDouble(&ok);
        double f = parts.size() > 3 ? parts[3].toDouble() / fps : 0;
        if (ok) return h * 3600 + m * 60 + s + f;
    }
    return -1;
}

std::vector<Subtitle> readSubtitleFile(const QString& file, double fps, QString* error)
{
    std::vector<Subtitle> out;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = f.errorString();
        return out;
    }
    QTextStream in(&f);
    const bool csv = file.endsWith(".csv", Qt::CaseInsensitive);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty()) continue;
        QStringList v = csv ? line.split(',') : line.split('\t');
        Subtitle s;
        if (v.size() >= 4) {
            const double a = parseSubTime(v[0], fps), b = parseSubTime(v[1], fps), len = parseSubTime(v[2], fps);
            if (a >= 0) {
                s.start = a;
                s.length = len > 0 ? len : (b > a ? b - a : 0);
                s.text = QStringList(v.mid(3)).join(csv ? "," : "\t").trimmed();
                if (s.text.startsWith('"') && s.text.endsWith('"') && s.text.size() >= 2) s.text = s.text.mid(1, s.text.size() - 2);
                out.push_back(s);
                continue;
            }
        }
        s.text = line.trimmed();
        out.push_back(s);
    }
    if (out.empty() && error) *error = "The file holds no text.";
    return out;
}

int insertSubtitles(const std::vector<Subtitle>& subs, const QString& presetName)
{
    if (subs.empty()) return 0;
    const std::string tt = "com.vegascreativesoftware:titlesandtext";
    std::vector<OfxPreset> presets = presetsFor(tt, "Generator");
    OfxPreset preset;
    bool havePreset = false;
    for (const OfxPreset& pr : presets)
        if (qs(pr.name) == presetName) { preset = pr; havePreset = true; }
    if (!havePreset && !presets.empty()) { preset = presets.front(); havePreset = true; }
    AppState& app = AppState::get();
    int made = 0;
    app.edit("Insert Subtitles", [&](Project& p) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.name = "Subtitles";
        if (app.onNewTrack) app.onNewTrack(p, t);
        double next = 0;
        for (const Subtitle& s : subs) {
            const double start = s.start >= 0 ? s.start : next;
            const double len = s.length > 0 ? s.length : 5.0;
            next = start + len;
            MediaItem m;
            m.id = p.newId();
            m.name = s8(s.text.left(40));
            m.generated = true;
            m.generator.id = p.newId();
            m.generator.kind = "ofx";
            m.generator.pluginId = tt;
            m.generator.name = "Titles & Text";
            if (havePreset) {
                OfxPreset pr = preset;
                bool set = false;
                for (OfxPreset::Value& v : pr.values)
                    if (v.name == "Text") { v.text = s8(s.text); set = true; }
                if (!set) {
                    OfxPreset::Value v;
                    v.type = "OfxParamTypeString";
                    v.name = "Text";
                    v.text = s8(s.text);
                    pr.values.push_back(v);
                }
                m.generator.state = presetToState(pr);
            }
            m.info.hasVideo = true;
            m.info.width = p.settings.width;
            m.info.height = p.settings.height;
            m.info.fps = p.settings.fps();
            p.media.push_back(m);
            Event e;
            e.id = p.newId();
            e.mediaId = m.id;
            e.start = start;
            e.length = len;
            e.name = m.name;
            setDefaultFadeCurves(e, TrackType::Video);
            t.events.push_back(e);
            ++made;
        }
        std::sort(t.events.begin(), t.events.end(), [](const Event& a, const Event& b) { return a.start < b.start; });
        p.tracks.insert(p.tracks.begin(), t);
    });
    return made;
}

namespace {

// The Insert Subtitles dialog (from a file, or from the regions): the file (Browse...), the Preset list and "Use
// current preset as default".
bool subtitleDialog(QWidget* parent, bool fromFile, QString* file, QString* preset)
{
    QDialog d(parent);
    d.setObjectName(fromFile ? "insertSubtitlesFile" : "insertSubtitlesRegions");
    d.setWindowTitle(fromFile ? "Insert Subtitles" : "Insert Subtitles from Regions");
    QFormLayout* f = new QFormLayout(&d);
    QLineEdit* path = nullptr;
    if (fromFile) {
        QWidget* row = new QWidget(&d);
        QHBoxLayout* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        path = new QLineEdit(row);
        path->setObjectName("subtitleFile");
        QPushButton* b = new QPushButton("Browse...", row);
        h->addWidget(path, 1);
        h->addWidget(b);
        QObject::connect(b, &QPushButton::clicked, &d, [&d, path] {
            const QString fn = QFileDialog::getOpenFileName(&d, "Insert Subtitles", path->text(), "Subtitle text (*.txt *.csv);;All files (*)");
            if (!fn.isEmpty()) path->setText(QDir::toNativeSeparators(fn));
        });
        f->addRow("File:", row);
    }
    QComboBox* presets = new QComboBox(&d);
    presets->setObjectName("subtitlePreset");
    for (const OfxPreset& pr : presetsFor("com.vegascreativesoftware:titlesandtext", "Generator")) presets->addItem(qs(pr.name));
    if (presets->count() == 0) presets->addItem("(Default)");
    const QString def = QSettings().value("subtitles/preset", "Subtitle").toString();
    if (presets->findText(def) >= 0) presets->setCurrentIndex(presets->findText(def));
    f->addRow("Preset:", presets);
    QCheckBox* asDefault = new QCheckBox("Use current preset as default", &d);
    asDefault->setObjectName("subtitleDefault");
    f->addRow(asDefault);
    QHBoxLayout* buttons = new QHBoxLayout();
    buttons->addStretch(1);
    QPushButton* ok = new QPushButton("OK", &d);
    ok->setObjectName("ok");
    ok->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &d);
    buttons->addWidget(ok);
    buttons->addWidget(cancel);
    f->addRow(buttons);
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return false;
    if (asDefault->isChecked()) QSettings().setValue("subtitles/preset", presets->currentText());
    if (file && path) *file = QDir::fromNativeSeparators(path->text().trimmed());
    *preset = presets->currentText();
    return true;
}

}  // namespace

QString backupFolder(const QString& kind)
{
    QString base;
    const QString where = QSettings().value("backup/location", "temp").toString();
    const std::string path = AppState::get().path();
    if (where == "project" && !path.empty()) base = QFileInfo(qs(path)).absolutePath() + "/VEGAS Backups";
    else if (where == "folder" && !QSettings().value("backup/folder").toString().isEmpty()) base = QSettings().value("backup/folder").toString();
    else {
        const QString temp = vprefs::text("prefs/tempFolder");
        base = (temp.isEmpty() ? QDir::tempPath() : temp) + "/Reno Backups";
    }
    return base + "/" + kind;
}

static QString backupBaseName()
{
    const std::string path = AppState::get().path();
    return path.empty() ? QString("Untitled") : QFileInfo(qs(path)).completeBaseName();
}

bool writeBackup(const QString& kind)
{
    AppState& app = AppState::get();
    if (app.project().tracks.empty() && app.project().media.empty()) return false;
    const QString folder = backupFolder(kind);
    if (!QDir().mkpath(folder)) return false;
    const QString name = QString("%1_%2.veg").arg(backupBaseName(), QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss"));
    Project copy = app.project();
    app.fx.captureStates(copy);
    const VegasSaveResult r = saveVegasProject(copy, s8(QDir(folder).filePath(name)));
    if (!r.ok) return false;
    QFile::remove(QDir(folder).filePath(name) + ".bak");
    QDir dir(folder);
    const QFileInfoList all = dir.entryInfoList(QStringList() << backupBaseName() + "_*.veg", QDir::Files, QDir::Time);
    const QDateTime now = QDateTime::currentDateTime();
    const int keepCount = kind == "daily" ? QSettings().value("backup/dailyCount", 10).toInt() : kind == "realtime" ? QSettings().value("backup/realtimeCount", 50).toInt() : 100000;
    int k = 0;
    for (const QFileInfo& fi : all) {
        ++k;
        const qint64 age = fi.lastModified().secsTo(now);
        const bool tooOld = (kind == "minutes" && age > 3600) || (kind == "hours" && age > 36000);
        if (tooOld || k > keepCount) QFile::remove(fi.absoluteFilePath());
    }
    return true;
}

QStringList backupFiles(bool currentProjectOnly)
{
    QFileInfoList all;
    for (const char* kind : {"minutes", "hours", "daily", "realtime"}) {
        QDir dir(backupFolder(kind));
        all += dir.entryInfoList(QStringList() << (currentProjectOnly ? backupBaseName() + "_*.veg" : QString("*.veg")), QDir::Files);
    }
    std::sort(all.begin(), all.end(), [](const QFileInfo& a, const QFileInfo& b) { return a.lastModified() > b.lastModified(); });
    QStringList out;
    for (const QFileInfo& fi : all) out << fi.absoluteFilePath();
    return out;
}

namespace {

class BackupKeeper : public QObject {
public:
    explicit BackupKeeper(MainWindow* w) : QObject(w)
    {
        timer_ = new QTimer(this);
        timer_->setInterval(15000);
        QObject::connect(timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_->start();
        listener_ = AppState::get().addListener([this](int changes) {
            if (!(changes & kProject)) return;
            AppState& app = AppState::get();
            if (!app.dirty()) return;
            editedSinceMinute_ = editedSinceHour_ = editedSinceDay_ = true;
            if (!settingsRead_.isValid() || settingsRead_.elapsed() > 1000) {
                QSettings s;
                liveSave_ = s.value("backup/liveSave", false).toBool();
                realtime_ = s.value("backup/advanced", false).toBool() && s.value("backup/realtime", false).toBool();
                settingsRead_.start();
            }
            if (liveSave_ && !app.path().empty()) {
                if (!saving_) {
                    saving_ = true;
                    QTimer::singleShot(0, this, [this] {
                        AppState& a = AppState::get();
                        std::string err;
                        if (a.dirty() && !a.path().empty() && !a.save(a.path(), &err)) a.status("Live Save failed: " + err);
                        saving_ = false;
                    });
                }
            } else if (realtime_) {
                writeBackup("realtime");
            }
        });
        started_ = QDateTime::currentDateTime();
        lastMinute_ = lastHour_ = started_;
    }
    ~BackupKeeper() override { AppState::get().removeListener(listener_); }

private:
    void tick()
    {
        QSettings s;
        if (!s.value("backup/advanced", false).toBool() || s.value("backup/liveSave", false).toBool() || s.value("backup/realtime", false).toBool()) return;
        const QDateTime now = QDateTime::currentDateTime();
        if (s.value("backup/minutesOn", true).toBool() && editedSinceMinute_ && lastMinute_.secsTo(now) >= 60 * std::max(1, s.value("backup/minutes", 10).toInt())) {
            if (writeBackup("minutes")) editedSinceMinute_ = false;
            lastMinute_ = now;
        }
        if (s.value("backup/hoursOn", false).toBool() && editedSinceHour_ && lastHour_.secsTo(now) >= 3600 * std::max(1, s.value("backup/hours", 1).toInt())) {
            if (writeBackup("hours")) editedSinceHour_ = false;
            lastHour_ = now;
        }
        if (s.value("backup/dailyOn", false).toBool() && editedSinceDay_) {
            const QString today = now.date().toString(Qt::ISODate);
            if (s.value("backup/lastDaily").toString() != today) {
                if (writeBackup("daily")) {
                    s.setValue("backup/lastDaily", today);
                    editedSinceDay_ = false;
                }
            }
        }
    }
    QTimer* timer_ = nullptr;
    int listener_ = 0;
    bool saving_ = false;
    QElapsedTimer settingsRead_;
    bool liveSave_ = false, realtime_ = false;
    bool editedSinceMinute_ = false, editedSinceHour_ = false, editedSinceDay_ = false;
    QDateTime started_, lastMinute_, lastHour_;
};

// The Restore Backup dialog: the backups (newest first), Current Project Only, Restore Selected (with "Open in new
// instance of Reno"), Delete Selected.
void restoreBackupDialog(MainWindow* w)
{
    QDialog d(w);
    d.setObjectName("restoreBackupDialog");
    d.setWindowTitle("Restore Backup");
    d.resize(560, 380);
    QVBoxLayout* v = new QVBoxLayout(&d);
    QListWidget* list = new QListWidget(&d);
    list->setObjectName("backupList");
    QCheckBox* current = new QCheckBox("Current Project Only", &d);
    current->setObjectName("currentProjectOnly");
    auto fill = [list, current] {
        list->clear();
        for (const QString& f : backupFiles(current->isChecked())) {
            const QFileInfo fi(f);
            QListWidgetItem* it = new QListWidgetItem(QString("%1   (%2, %3)").arg(fi.fileName(), fi.dir().dirName(), fi.lastModified().toString("yyyy-MM-dd HH:mm:ss")), list);
            it->setData(Qt::UserRole, f);
        }
    };
    QObject::connect(current, &QCheckBox::toggled, &d, [fill](bool) { fill(); });
    fill();
    v->addWidget(list, 1);
    v->addWidget(current);
    QCheckBox* newInstance = new QCheckBox("Open in new instance of Reno", &d);
    newInstance->setObjectName("openNewInstance");
    v->addWidget(newInstance);
    QHBoxLayout* h = new QHBoxLayout();
    QPushButton* restore = new QPushButton("Restore Selected", &d);
    restore->setObjectName("restoreSelected");
    QPushButton* del = new QPushButton("Delete Selected", &d);
    del->setObjectName("deleteSelected");
    QPushButton* close = new QPushButton("Close", &d);
    h->addWidget(restore);
    h->addWidget(del);
    h->addStretch(1);
    h->addWidget(close);
    v->addLayout(h);
    QObject::connect(close, &QPushButton::clicked, &d, &QDialog::reject);
    QObject::connect(del, &QPushButton::clicked, &d, [list, fill] {
        if (!list->currentItem()) return;
        QFile::remove(list->currentItem()->data(Qt::UserRole).toString());
        fill();
    });
    QObject::connect(restore, &QPushButton::clicked, &d, [&d, w, list, newInstance] {
        if (!list->currentItem()) return;
        const QString f = list->currentItem()->data(Qt::UserRole).toString();
        d.accept();
        if (newInstance->isChecked()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(f));
            return;
        }
        w->openProject(f);
    });
    d.exec();
}

void advancedSaveDialog(MainWindow* w)
{
    QSettings s;
    QDialog d(w);
    d.setObjectName("advancedSaveDialog");
    d.setWindowTitle("Advanced Save");
    QVBoxLayout* v = new QVBoxLayout(&d);
    QCheckBox* live = new QCheckBox("Live Save", &d);
    live->setObjectName("liveSave");
    live->setChecked(s.value("backup/liveSave", false).toBool());
    live->setToolTip("The project is saved after every edit (once it has been saved once)");
    v->addWidget(live);
    QGroupBox* adv = new QGroupBox("Use Advanced Backup", &d);
    adv->setObjectName("useAdvancedBackup");
    adv->setCheckable(true);
    adv->setChecked(s.value("backup/advanced", false).toBool());
    QFormLayout* f = new QFormLayout(adv);
    auto spinRow = [&](const char* name, const QString& label, const char* onKey, bool onDefault, const char* key, int def, int lo, int hi) {
        QCheckBox* c = new QCheckBox(label, adv);
        c->setObjectName(name);
        c->setChecked(s.value(onKey, onDefault).toBool());
        QSpinBox* sp = new QSpinBox(adv);
        sp->setRange(lo, hi);
        sp->setValue(s.value(key, def).toInt());
        f->addRow(c, sp);
        return std::make_pair(c, sp);
    };
    auto minutes = spinRow("backupMinutes", "Backup Interval (Minutes):", "backup/minutesOn", true, "backup/minutes", 10, 1, 60);
    auto hours = spinRow("backupHours", "Backup Interval (Hours):", "backup/hoursOn", false, "backup/hours", 1, 1, 10);
    auto daily = spinRow("backupDaily", "Archive Daily Backups:", "backup/dailyOn", false, "backup/dailyCount", 10, 1, 500);
    auto realtime = spinRow("backupRealtime", "Archive Real-Time Backups:", "backup/realtime", false, "backup/realtimeCount", 50, 1, 500);
    f->addRow(new QLabel("Save Location:", adv));
    QRadioButton* temp = new QRadioButton("Temporary Files Folder", adv);
    QRadioButton* proj = new QRadioButton("Use Current Project Folder", adv);
    QRadioButton* sel = new QRadioButton("Select Folder", adv);
    const QString where = s.value("backup/location", "temp").toString();
    (where == "project" ? proj : where == "folder" ? sel : temp)->setChecked(true);
    f->addRow(temp);
    f->addRow(proj);
    QWidget* folderRow = new QWidget(adv);
    QHBoxLayout* fh = new QHBoxLayout(folderRow);
    fh->setContentsMargins(0, 0, 0, 0);
    QLineEdit* folder = new QLineEdit(QDir::toNativeSeparators(s.value("backup/folder").toString()), folderRow);
    QPushButton* browse = new QPushButton("Browse...", folderRow);
    fh->addWidget(folder, 1);
    fh->addWidget(browse);
    f->addRow(sel, folderRow);
    QObject::connect(browse, &QPushButton::clicked, &d, [&d, folder, sel] {
        const QString x = QFileDialog::getExistingDirectory(&d, "Backup Folder", QDir::fromNativeSeparators(folder->text()));
        if (!x.isEmpty()) { folder->setText(QDir::toNativeSeparators(x)); sel->setChecked(true); }
    });
    auto enable = [=] {
        adv->setEnabled(!live->isChecked());
        const bool rt = realtime.first->isChecked();
        for (auto p : {minutes, hours, daily}) { p.first->setEnabled(!rt); p.second->setEnabled(!rt && p.first->isChecked()); }
        realtime.second->setEnabled(rt);
    };
    for (QCheckBox* c : {live, minutes.first, hours.first, daily.first, realtime.first}) QObject::connect(c, &QCheckBox::toggled, &d, [enable](bool) { enable(); });
    enable();
    v->addWidget(adv);
    QHBoxLayout* h = new QHBoxLayout();
    QPushButton* restore = new QPushButton("Restore...", &d);
    restore->setObjectName("restoreBackups");
    QPushButton* clearAll = new QPushButton("Clear all backups", &d);
    clearAll->setObjectName("clearAllBackups");
    QPushButton* clearThis = new QPushButton("Clear current project backups", &d);
    clearThis->setObjectName("clearProjectBackups");
    h->addWidget(restore);
    h->addWidget(clearAll);
    h->addWidget(clearThis);
    v->addLayout(h);
    QObject::connect(restore, &QPushButton::clicked, &d, [&d, w] { d.accept(); restoreBackupDialog(w); });
    auto clear = [&d](bool current) {
        const QStringList files = backupFiles(current);
        if (files.isEmpty()) return;
        if (QMessageBox::question(&d, "Advanced Save", QString("Delete %1 backup file%2?").arg(files.size()).arg(files.size() == 1 ? "" : "s")) != QMessageBox::Yes) return;
        for (const QString& f : files) QFile::remove(f);
    };
    QObject::connect(clearAll, &QPushButton::clicked, &d, [clear] { clear(false); });
    QObject::connect(clearThis, &QPushButton::clicked, &d, [clear] { clear(true); });
    QHBoxLayout* b = new QHBoxLayout();
    b->addStretch(1);
    QPushButton* ok = new QPushButton("OK", &d);
    ok->setObjectName("ok");
    ok->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &d);
    b->addWidget(ok);
    b->addWidget(cancel);
    v->addLayout(b);
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    s.setValue("backup/liveSave", live->isChecked());
    s.setValue("backup/advanced", adv->isChecked());
    s.setValue("backup/minutesOn", minutes.first->isChecked());
    s.setValue("backup/minutes", minutes.second->value());
    s.setValue("backup/hoursOn", hours.first->isChecked());
    s.setValue("backup/hours", hours.second->value());
    s.setValue("backup/dailyOn", daily.first->isChecked());
    s.setValue("backup/dailyCount", daily.second->value());
    s.setValue("backup/realtime", realtime.first->isChecked());
    s.setValue("backup/realtimeCount", realtime.second->value());
    s.setValue("backup/location", proj->isChecked() ? "project" : sel->isChecked() ? "folder" : "temp");
    s.setValue("backup/folder", QDir::fromNativeSeparators(folder->text().trimmed()));
    if (live->isChecked() && AppState::get().path().empty())
        AppState::get().status("Live Save starts once the project has been saved (File > Save).");
}

}  // namespace

namespace {

void importBwfDialog(MainWindow* w)
{
    const QStringList files = QFileDialog::getOpenFileNames(w, "Import Broadcast Wave Format", QString(), "Broadcast Wave files (*.wav *.bwf);;All files (*)");
    if (files.isEmpty()) return;
    QDialog d(w);
    d.setObjectName("importBwfDialog");
    d.setWindowTitle("Import Broadcast Wave Format");
    QFormLayout* f = new QFormLayout(&d);
    QListWidget* info = new QListWidget(&d);
    info->setFixedHeight(90);
    for (const QString& fn : files) {
        double ref = 0;
        const bool has = bwfTimeReference(fn, &ref);
        info->addItem(QString("%1   %2").arg(QFileInfo(fn).fileName(), has ? "time stamp " + qs(timecode(ref, fpsOf(AppState::get().project()))) : QString("no time stamp")));
    }
    f->addRow(info);
    QComboBox* arrange = new QComboBox(&d);
    arrange->setObjectName("bwfArrange");
    arrange->addItems({"Add across tracks", "Add across time"});
    f->addRow("Arrange:", arrange);
    QComboBox* order = new QComboBox(&d);
    order->setObjectName("bwfOrder");
    order->addItems({"By timestamp", "Alphabetically by filename"});
    f->addRow("Order tracks:", order);
    QComboBox* pos = new QComboBox(&d);
    pos->setObjectName("bwfPositioning");
    pos->addItems({"Use ruler time", "Relative to cursor"});
    f->addRow("Positioning:", pos);
    QObject::connect(arrange, QOverload<int>::of(&QComboBox::currentIndexChanged), &d, [order](int i) { order->setEnabled(i == 0); });
    QHBoxLayout* b = new QHBoxLayout();
    b->addStretch(1);
    QPushButton* ok = new QPushButton("Open", &d);
    ok->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &d);
    b->addWidget(ok);
    b->addWidget(cancel);
    f->addRow(b);
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QString err;
    const int n = importBroadcastWave(files, arrange->currentIndex(), order->currentIndex(), pos->currentIndex() == 1, &err);
    AppState::get().status(n ? s8(QString("%1 event%2 added from Broadcast Wave files.").arg(n).arg(n == 1 ? "" : "s") + (err.isEmpty() ? QString() : " " + err)) : s8(err));
}

void importMediaFromProjectDialog(MainWindow* w)
{
    const QString file = QFileDialog::getOpenFileName(w, "Import Media from Project", w->projectFolder(), "Projects (*.veg *.reno);;All files (*)");
    if (file.isEmpty()) return;
    QDialog d(w);
    d.setObjectName("importMediaFromProject");
    d.setWindowTitle("Import Media from Project");
    QVBoxLayout* v = new QVBoxLayout(&d);
    v->addWidget(new QLabel(QDir::toNativeSeparators(file), &d));
    QCheckBox* bins = new QCheckBox("Import bins", &d);
    bins->setObjectName("importBins");
    bins->setChecked(QSettings().value("importFromProject/bins", true).toBool());
    QCheckBox* merge = new QCheckBox("Merge with existing bins", &d);
    merge->setObjectName("mergeBins");
    merge->setChecked(QSettings().value("importFromProject/merge", false).toBool());
    v->addWidget(bins);
    v->addWidget(merge);
    QObject::connect(bins, &QCheckBox::toggled, merge, &QCheckBox::setEnabled);
    merge->setEnabled(bins->isChecked());
    QHBoxLayout* b = new QHBoxLayout();
    b->addStretch(1);
    QPushButton* ok = new QPushButton("Open", &d);
    ok->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &d);
    b->addWidget(ok);
    b->addWidget(cancel);
    v->addLayout(b);
    QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    QSettings().setValue("importFromProject/bins", bins->isChecked());
    QSettings().setValue("importFromProject/merge", merge->isChecked());
    QString err;
    const int n = importMediaFromProject(file, bins->isChecked(), merge->isChecked(), &err);
    if (n) AppState::get().status(s8(QString("%1 media file%2 imported from %3.").arg(n).arg(n == 1 ? "" : "s").arg(QFileInfo(file).fileName())));
    else QMessageBox::information(w, "Import Media from Project", err);
}

void archiveDialog(MainWindow* w)
{
    QDialog d(w);
    d.setObjectName("projectArchiveDialog");
    d.setWindowTitle("VEGAS Project Archive");
    QFormLayout* f = new QFormLayout(&d);
    QWidget* row = new QWidget(&d);
    QHBoxLayout* h = new QHBoxLayout(row);
    h->setContentsMargins(0, 0, 0, 0);
    const std::string cur = AppState::get().path();
    const QString suggestion = QDir(w->projectFolder()).filePath((cur.empty() ? QString("Untitled") : QFileInfo(qs(cur)).completeBaseName()) + " Archive/" +
                                                                  (cur.empty() ? QString("Untitled") : QFileInfo(qs(cur)).completeBaseName()) + ".veg");
    QLineEdit* file = new QLineEdit(QDir::toNativeSeparators(suggestion), row);
    file->setObjectName("archiveFile");
    QPushButton* browse = new QPushButton("Browse...", row);
    h->addWidget(file, 1);
    h->addWidget(browse);
    f->addRow("Project file:", row);
    QObject::connect(browse, &QPushButton::clicked, &d, [&d, file] {
        const QString x = QFileDialog::getSaveFileName(&d, "Project Archive", QDir::fromNativeSeparators(file->text()), "VEGAS Pro projects (*.veg)");
        if (!x.isEmpty()) file->setText(QDir::toNativeSeparators(x));
    });
    QCheckBox* nested = new QCheckBox("Include nested projects", &d);
    nested->setChecked(true);
    QCheckBox* media = new QCheckBox("Include media", &d);
    media->setChecked(true);
    QCheckBox* proxies = new QCheckBox("Include video proxy/audio peak files", &d);
    QCheckBox* unused = new QCheckBox("Exclude unused media", &d);
    unused->setChecked(true);
    for (QCheckBox* c : {nested, media, proxies, unused}) f->addRow(c);
    QHBoxLayout* b = new QHBoxLayout();
    b->addStretch(1);
    QPushButton* finish = new QPushButton("Finish", &d);
    finish->setObjectName("finish");
    finish->setDefault(true);
    QPushButton* cancel = new QPushButton("Cancel", &d);
    b->addWidget(finish);
    b->addWidget(cancel);
    f->addRow(b);
    QObject::connect(finish, &QPushButton::clicked, &d, &QDialog::accept);
    QObject::connect(cancel, &QPushButton::clicked, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    ArchiveOptions o;
    o.nested = nested->isChecked();
    o.media = media->isChecked();
    o.proxies = proxies->isChecked();
    o.excludeUnused = unused->isChecked();
    QString err;
    int n = 0;
    QString path = QDir::fromNativeSeparators(file->text().trimmed());
    if (!path.endsWith(".veg", Qt::CaseInsensitive)) path += ".veg";
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = archiveProject(path, o, &err, &n);
    QApplication::restoreOverrideCursor();
    if (ok) AppState::get().status(s8(QString("Project archived to %1 (%2 file%3 copied).").arg(QDir::toNativeSeparators(path)).arg(n).arg(n == 1 ? "" : "s")));
    else QMessageBox::warning(w, "VEGAS Project Archive", err);
}

}  // namespace

namespace {

// Options > Metronome's clicks in the playback (custom sounds from Preferences > Audio, read at the playback's rate: up
// to a second of each).
std::vector<float> readClick(const QString& file, int rate)
{
    std::vector<float> out;
    if (file.isEmpty() || !AppState::get().mediaOpener) return out;
    std::string err;
    std::unique_ptr<MediaReader> r = AppState::get().mediaOpener(s8(file), &err);
    if (!r || !r->info().hasAudio) return out;
    const int n = std::min(rate, (int)std::max<int64_t>(1, (int64_t)std::llround(r->info().duration * rate)));
    std::vector<float> L((size_t)n), R((size_t)n);
    if (!r->audio(0, n, rate, L.data(), R.data())) return out;
    out.resize((size_t)n);
    for (int k = 0; k < n; ++k) out[(size_t)k] = 0.5f * (L[(size_t)k] + R[(size_t)k]);
    return out;
}

void updateMetronome(MainWindow* w)
{
    Playback* pb = w->playback();
    if (!pb) return;
    const bool on = QSettings().value("options/metronome", false).toBool();
    if (!on) { pb->setMetronome(false); return; }
    const int rate = AppState::get().project().settings.sampleRate > 0 ? AppState::get().project().settings.sampleRate : 48000;
    if (vprefs::on("prefs/metronomeCustom"))
        pb->setMetronome(true, readClick(vprefs::text("prefs/metronomeNormal"), rate), readClick(vprefs::text("prefs/metronomeAccent"), rate));
    else
        pb->setMetronome(true);
}

// Preferences > General "Close media files when not the active application" and "Close audio and MIDI ports when not the
// active application": done when Reno goes to the background.
class BackgroundWatcher : public QObject {
public:
    explicit BackgroundWatcher(MainWindow* w) : QObject(w), w_(w)
    {
        QObject::connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState st) {
            if (st == Qt::ApplicationActive) return;
            if (vprefs::on("prefs/closePortsInactive") && w_->playback() && w_->playback()->playing()) w_->playback()->pause();
            if (vprefs::on("prefs/closeMediaInactive")) {
                AppState::get().notify(kMediaPictures);
            }
        });
    }

private:
    MainWindow* w_;
};

}  // namespace

void installManualExtras(MainWindow* w)
{
    AppState& app = AppState::get();
    QMenu* file = topMenu(w, "File");
    QMenu* view = topMenu(w, "View");
    QMenu* insert = topMenu(w, "Insert");
    QMenu* toolsMenu = topMenu(w, "Tools");
    QMenu* options = topMenu(w, "Options");
    QMenu* help = topMenu(w, "Help");
    const QString interchange = "a project interchange format Reno cannot read or write (VEGAS projects, EDL text files and media are what it takes)";

    if (QMenu* imp = subMenu(file, "Import")) {
        imp->addAction(command(w, "importFromProject", "Media from &Project...", "Adds the media of another project to this one (with its bins if wanted)",
                               [w] { importMediaFromProjectDialog(w); }));
        imp->addAction(unavailable(w, "importAaf", "&AAF...", interchange));
        imp->addAction(unavailable(w, "importPremiere", "P&remiere/After Effects (*.prproj)...", interchange));
        imp->addAction(unavailable(w, "importFcp7", "&Final Cut Pro 7/DaVinci Resolve (*.xml)...", interchange));
        imp->addAction(unavailable(w, "importFcpx", "Final Cut Pro &X (*.fcpxml)...", interchange));
        imp->addAction(command(w, "importEdl", "&EDL Text File (*.txt)...", "Opens a VEGAS EDL text file as a new project", [w] {
            const QString fn = QFileDialog::getOpenFileName(w, "Import EDL Text File", w->projectFolder(), "EDL text files (*.txt);;All files (*)");
            if (fn.isEmpty()) return;
            Project p;
            QString err;
            if (!readEdl(fn, p, &err)) { QMessageBox::warning(w, "Import EDL Text File", err); return; }
            const QString tmp = QDir(QDir::tempPath()).filePath(QFileInfo(fn).completeBaseName() + " (EDL).reno");
            std::string e2;
            if (!p.save(s8(tmp), &e2)) { QMessageBox::warning(w, "Import EDL Text File", qs(e2)); return; }
            if (w->openProject(tmp)) {
                AppState::get().untieFromFile();
                AppState::get().status(s8("Opened the EDL " + QDir::toNativeSeparators(fn) + " (events on one video and up to four audio tracks; transitions as cross-fades)."));
            }
            QFile::remove(tmp);
        }));
        imp->addAction(command(w, "importBwf", "&Broadcast Wave Format...", "Adds Broadcast Wave files at the times they were recorded", [w] { importBwfDialog(w); }));
        QAction* dvd = unavailable(w, "importDvdCamcorder", "&DVD Camcorder Disc...", "importing from DVD camcorders needs the camera's disc reader (VEGAS hides it unless Preferences > Deprecated Features shows it)");
        imp->addAction(dvd);
        QObject::connect(imp, &QMenu::aboutToShow, imp, [dvd] { dvd->setVisible(vprefs::on("prefs/depDvdCamcorder")); });
    }
    if (file) {
        QMenu* exp = new QMenu("E&xport", file);
        exp->setObjectName("exportMenu");
        exp->addAction(command(w, "exportArchive", "&VEGAS Project Archive (*.veg)...", "The project and its media copied into a folder of their own", [w] { archiveDialog(w); }));
        exp->addAction(unavailable(w, "exportProToolsAaf", "&Pro Tools AAF File (*.aaf)...", interchange));
        exp->addAction(unavailable(w, "exportAvidAaf", "&Media Composer AAF File (*.aaf)...", interchange));
        exp->addAction(unavailable(w, "exportPremiere", "Premiere/&After Effects (*.prproj)...", interchange));
        exp->addAction(unavailable(w, "exportFcp7", "&Final Cut Pro 7/DaVinci Resolve (*.xml)...", interchange));
        exp->addAction(unavailable(w, "exportFcpx", "Final Cut Pro &X (*.fcpxml)...", interchange));
        exp->addAction(command(w, "exportEdl", "&EDL Text File (*.txt)...", "Writes the project's events as a VEGAS EDL text file", [w] {
            const std::string cur = AppState::get().path();
            const QString sug = QDir(w->projectFolder()).filePath((cur.empty() ? QString("Untitled") : QFileInfo(qs(cur)).completeBaseName()) + ".txt");
            const QString fn = QFileDialog::getSaveFileName(w, "Export EDL Text File", sug, "EDL text files (*.txt)");
            if (fn.isEmpty()) return;
            QString err;
            if (writeEdl(AppState::get().project(), fn, &err)) AppState::get().status(s8("EDL written to " + QDir::toNativeSeparators(fn)));
            else QMessageBox::warning(w, "Export EDL Text File", err);
        }));
        exp->addAction(unavailable(w, "exportXdcam", "XDCAM &Disc...", "writing XDCAM discs needs an XDCAM drive"));
        QAction* addMedia = findItem(file, "addMedia");
        QAction* props = findItem(file, "properties");
        QAction* expAct = exp->menuAction();
        if (QMenu* imp = subMenu(file, "Import")) file->insertMenu(addMedia ? addMedia : props, exp), (void)imp;
        else file->insertMenu(props, exp);
        (void)expAct;
        QAction* dvdTop = unavailable(w, "dvdCamcorderTop", "Import &DVD Camcorder Disc...", "importing from DVD camcorders needs the camera's disc reader");
        QAction* capture = unavailable(w, "vegasCapture", "VEGAS Capt&ure...", "VEGAS Capture is a separate MAGIX program (screen and camera recording)");
        QAction* captureVideo = unavailable(w, "captureVideo", "Capture &Video...", "capturing from DV / HDV / SDI devices needs capture hardware");
        QAction* cd = unavailable(w, "extractCd", "&Extract Audio from CD...", "Reno reads no audio CDs (rip the tracks to files and import them)");
        QMenu* share = new QMenu("S&hare Online...", file);
        share->setObjectName("shareOnlineMenu");
        share->addAction(unavailable(w, "uploadVimeo", "Upload to &Vimeo...", "an online service (render the file and upload it in the browser)"));
        share->addAction(unavailable(w, "uploadYoutube", "Upload to &YouTube...", "an online service (render the file and upload it in the browser)"));
        QAction* connect1 = unavailable(w, "prepareConnect", "Prepare for VEGAS Pro Co&nnect...", "VEGAS Pro Connect is MAGIX's iPad companion app");
        QAction* connect2 = unavailable(w, "manageConnect", "Manage VEGAS Pro Conne&ct Clips...", "VEGAS Pro Connect is MAGIX's iPad companion app");
        file->insertAction(props, dvdTop);
        file->insertAction(props, capture);
        file->insertAction(props, captureVideo);
        file->insertAction(props, cd);
        file->insertSeparator(props);
        file->insertMenu(props, share);
        file->insertSeparator(props);
        file->insertAction(props, connect1);
        file->insertAction(props, connect2);
        file->insertSeparator(props);
        QObject::connect(file, &QMenu::aboutToShow, file, [dvdTop] { dvdTop->setVisible(vprefs::on("prefs/depDvdCamcorder")); });
    }

    if (QMenu* win = subMenu(view, "Window")) {
        win->addAction(unavailable(w, "showMediaManager", "Media Ma&nager", "VEGAS's old media database (VEGAS Pro 18 hides it too); Project Media's bins, tags and search do its work"));
        QAction* xdcam = unavailable(w, "showXdcamExplorer", "XD&CAM Explorer", "XDCAM drives and decks are hardware Reno does not drive (the Device Explorer imports XDCAM cards)");
        win->addAction(xdcam);
        win->addAction(unavailable(w, "showHubExplorer", "&Hub Explorer (Beta)", "VEGAS Prepare's collections and VEGAS Cloud are MAGIX services"));
        QObject::connect(win, &QMenu::aboutToShow, win, [xdcam] { xdcam->setVisible(vprefs::on("prefs/depXdcamExplorer")); });
    }

    if (insert) {
        QAction* fromFile = command(w, "insertSubtitlesFile", "Insert Subtitles from File", "Title events from the lines of a .txt or .csv file, on a new top track", [w] {
            QString fn, preset;
            if (!subtitleDialog(w, true, &fn, &preset)) return;
            QString err;
            const std::vector<Subtitle> subs = readSubtitleFile(fn, fpsOf(AppState::get().project()), &err);
            if (subs.empty()) { QMessageBox::warning(w, "Insert Subtitles", err.isEmpty() ? QString("No subtitles in that file.") : err); return; }
            const int n = insertSubtitles(subs, preset);
            AppState::get().status(s8(QString("%1 subtitle%2 inserted.").arg(n).arg(n == 1 ? "" : "s")));
        });
        QAction* fromRegions = command(w, "insertSubtitlesRegions", "Insert Subtitles from Regions", "Title events with the regions' names, over the regions, on a new top track", [w] {
            std::vector<Subtitle> subs;
            for (const Project::Marker& m : AppState::get().project().markers)
                if (m.region && !m.name.empty() && m.length > 0) {
                    Subtitle s;
                    s.start = m.time;
                    s.length = m.length;
                    s.text = qs(m.name);
                    subs.push_back(s);
                }
            if (subs.empty()) { QMessageBox::information(w, "Insert Subtitles from Regions", "There are no named regions (select a time, press R and type the subtitle)."); return; }
            QString preset;
            if (!subtitleDialog(w, false, nullptr, &preset)) return;
            const int n = insertSubtitles(subs, preset);
            AppState::get().status(s8(QString("%1 subtitle%2 inserted.").arg(n).arg(n == 1 ? "" : "s")));
        });
        insertBefore(insert, fromFile, "addMissingStream");
        insertBefore(insert, fromRegions, "addMissingStream");
    }

    if (toolsMenu) {
        QAction* adv = command(w, "advancedSave", "Advanced Save...", "Live Save, timed and real-time backups, Restore Backup", [w] { advancedSaveDialog(w); });
        QAction* sep = toolsMenu->insertSeparator(findItem(toolsMenu, "cleanProjectMedia"));
        toolsMenu->insertAction(sep, adv);
        toolsMenu->insertSeparator(adv);
    }

    if (options) {
        QAction* metronome = new QAction("Metr&onome", w);
        metronome->setObjectName("metronome");
        metronome->setCheckable(true);
        metronome->setStatusTip("Clicks at the project's tempo while recording (not in renders)");
        metronome->setChecked(QSettings().value("options/metronome", false).toBool());
        QObject::connect(metronome, &QAction::toggled, w, [w](bool on) {
            QSettings().setValue("options/metronome", on);
            updateMetronome(w);
        });
        updateMetronome(w);
        const int l = app.addListener([w](int changes) { if (changes & kPrefs) updateMetronome(w); });
        QObject::connect(w, &QObject::destroyed, [l] { AppState::get().removeListener(l); });
        w->addAction(metronome);
        insertAfter(options, metronome, "loop");
        QMenu* loud = options->findChild<QMenu*>("loudnessMetersMenu");
        if (loud) {
            options->removeAction(loud->menuAction());
            insertAfter(options, loud->menuAction(), "metronome");
        }
        QAction* muteA = new QAction("&Mute All Audio", w);
        QAction* muteV = new QAction("Mute All Vi&deo", w);
        muteA->setObjectName("muteAllAudio");
        muteV->setObjectName("muteAllVideo");
        muteA->setStatusTip("Plays no audio track (the project is not changed, renders are not affected)");
        muteV->setStatusTip("Shows no video track (the project is not changed, renders are not affected)");
        for (QAction* m : {muteA, muteV}) {
            m->setCheckable(true);
            w->addAction(m);
        }
        QObject::connect(muteA, &QAction::toggled, w, [](bool on) {
            AppState::get().muteAllAudio = on;
        });
        QObject::connect(muteV, &QAction::toggled, w, [](bool on) {
            AppState& a = AppState::get();
            a.muteAllVideo = on;
            a.notify(kFxParams);
        });
        {
            const QList<QAction*> all = options->actions();
            const int at = loud ? all.indexOf(loud->menuAction()) : -1;
            QAction* next = at >= 0 && at + 1 < all.size() ? all[at + 1] : nullptr;
            options->insertSeparator(next);
            options->insertAction(next, muteA);
            options->insertAction(next, muteV);
        }
        QAction* tb = command(w, "customizeToolbar", "Customize &Toolbar...", "Adds, removes and rearranges the main tool bar's buttons", [w] { runCustomizeToolbar(w, false); });
        QAction* ttb = command(w, "customizeTimelineToolbar", "Customize Timeline Toolbar...", "Adds, removes and rearranges the editing tool bar's buttons", [w] { runCustomizeToolbar(w, true); });
        QAction* keyboard = nullptr;
        for (QAction* a : options->actions())
            if (plainMenuText(a->text()).startsWith("Customize Keyboard")) keyboard = a;
        options->insertAction(keyboard, tb);
        options->insertAction(keyboard, ttb);
    }

    if (help) {
        QAction* shortcuts = findItem(help, "shortcuts");
        QList<QAction*> online = {unavailable(w, "showMeHow", "&Show Me How...", "MAGIX's online help pages"),
                                  unavailable(w, "interactiveTutorials", "Interactive Tutorial&s...", "MAGIX's interactive tutorials ship with VEGAS itself (Help > Contents and Index has Reno's read-me)")};
        QAction* after = nullptr;
        {
            const QList<QAction*> all = help->actions();
            const int i = all.indexOf(shortcuts);
            after = i >= 0 && i + 1 < all.size() ? all[i + 1] : nullptr;
        }
        for (QAction* a : online) help->insertAction(after, a);
        help->insertSeparator(after);
        for (QAction* a : {unavailable(w, "checkUpdates", "Check for Software &Updates...", "Reno has no update service"),
                           unavailable(w, "checkDrivers", "Check for &Driver Updates...", "Reno has no update service"),
                           unavailable(w, "checkNews", "Check for News...", "MAGIX's news service"),
                           unavailable(w, "deactivate", "Deactivate the Software on this PC...", "Reno needs no activation"),
                           unavailable(w, "ratings", "Ratings and Reviews", "MAGIX's web pages"),
                           unavailable(w, "community", "&VEGAS Community...", "MAGIX's web pages")})
            help->insertAction(after, a);
        help->insertSeparator(after);
    }

    installToolbars(w);
    new MatchMediaPrompt(w);
    new BackupKeeper(w);
    new BackgroundWatcher(w);
    (void)app;
}

}  // namespace app
}  // namespace reno
