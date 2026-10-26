// Recording: see recorder.h.
#include "recorder.h"
#include "playback.h"
#include "vprefs.h"
#include <QDialog>
#include <QDateTime>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace reno {
namespace app {

static void put16(std::vector<char>& b, size_t at, uint16_t v) { b[at] = (char)(v & 255); b[at + 1] = (char)(v >> 8); }
static void put32(std::vector<char>& b, size_t at, uint32_t v)
{
    for (int k = 0; k < 4; ++k) b[at + (size_t)k] = (char)((v >> (8 * k)) & 255);
}

WavWriter::~WavWriter()
{
    if (f_) finish();
}

bool WavWriter::open(const std::string& path, int sampleRate, int channels, int bits, std::string* error)
{
    if (f_) finish();
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    channels_ = channels == 1 ? 1 : 2;
    bits_ = (bits == 24 || bits == 32) ? bits : 16;
    frames_ = 0;
    failed_ = false;
#ifdef _WIN32
    f_ = _wfopen(widen(path).c_str(), L"wb");
#else
    f_ = std::fopen(path.c_str(), "wb");
#endif
    if (!f_) {
        if (error) *error = "the file " + path + " could not be made";
        return false;
    }
    std::vector<char> h(44, 0);
    std::memcpy(&h[0], "RIFF", 4);
    std::memcpy(&h[8], "WAVE", 4);
    std::memcpy(&h[12], "fmt ", 4);
    put32(h, 16, 16);
    put16(h, 20, bits_ == 32 ? 3 : 1);
    put16(h, 22, (uint16_t)channels_);
    put32(h, 24, (uint32_t)rate_);
    const int block = channels_ * bits_ / 8;
    put32(h, 28, (uint32_t)(rate_ * block));
    put16(h, 32, (uint16_t)block);
    put16(h, 34, (uint16_t)bits_);
    std::memcpy(&h[36], "data", 4);
    if (std::fwrite(h.data(), 1, h.size(), f_) != h.size()) failed_ = true;
    return !failed_;
}

bool WavWriter::write(const float* left, const float* right, int frames)
{
    if (!f_ || frames <= 0) return f_ != nullptr;
    const int bytes = bits_ / 8;
    buf_.resize((size_t)frames * (size_t)channels_ * (size_t)bytes);
    char* o = buf_.data();
    for (int k = 0; k < frames; ++k) {
        for (int c = 0; c < channels_; ++c) {
            float v = c == 0 ? left[k] : (right ? right[k] : left[k]);
            if (!(v == v)) v = 0.f;
            if (bits_ == 32) {
                std::memcpy(o, &v, 4);
            } else {
                v = std::min(1.f, std::max(-1.f, v));
                if (bits_ == 16) {
                    const int s = (int)std::lround(v * 32767.f);
                    o[0] = (char)(s & 255);
                    o[1] = (char)((s >> 8) & 255);
                } else {
                    const int s = (int)std::lround(v * 8388607.f);
                    o[0] = (char)(s & 255);
                    o[1] = (char)((s >> 8) & 255);
                    o[2] = (char)((s >> 16) & 255);
                }
            }
            o += bytes;
        }
    }
    if (std::fwrite(buf_.data(), 1, buf_.size(), f_) != buf_.size()) failed_ = true;
    frames_ += frames;
    return !failed_;
}

bool WavWriter::finish()
{
    if (!f_) return false;
    const uint64_t data = (uint64_t)frames_ * (uint64_t)channels_ * (uint64_t)(bits_ / 8);
    std::vector<char> s(4);
    put32(s, 0, (uint32_t)std::min<uint64_t>(0xFFFFFFFFull - 36, data + 36));
    if (std::fseek(f_, 4, SEEK_SET) != 0 || std::fwrite(s.data(), 1, 4, f_) != 4) failed_ = true;
    put32(s, 0, (uint32_t)std::min<uint64_t>(0xFFFFFFFFull - 72, data));
    if (std::fseek(f_, 40, SEEK_SET) != 0 || std::fwrite(s.data(), 1, 4, f_) != 4) failed_ = true;
    if (std::fclose(f_) != 0) failed_ = true;
    f_ = nullptr;
    return !failed_;
}

// Preferences > Audio "Record to Broadcast Wave Format": a finished recording gets a bext chunk after its fmt chunk.
static bool addBextChunk(const QString& path, uint64_t timeReference, int rate)
{
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = in.read(36);
    if (head.size() < 36 || head.left(4) != "RIFF" || head.mid(12, 4) != "fmt ") return false;
    const QString tmp = path + ".bwf.tmp";
    QFile out(tmp);
    if (!out.open(QIODevice::WriteOnly)) return false;
    QByteArray bext(602, 0);
    auto put = [&bext](int at, const QByteArray& v, int max) { std::memcpy(bext.data() + at, v.constData(), (size_t)std::min<int>(v.size(), max)); };
    put(0, "Recorded with Reno", 256);
    put(256, "Reno", 32);
    put(288, QString("RENO%1").arg(QDateTime::currentMSecsSinceEpoch()).toLatin1(), 32);
    const QDateTime now = QDateTime::currentDateTime();
    put(320, now.toString("yyyy-MM-dd").toLatin1(), 10);
    put(330, now.toString("HH:mm:ss").toLatin1(), 8);
    for (int k = 0; k < 8; ++k) bext[338 + k] = (char)((timeReference >> (8 * k)) & 255);
    bext[346] = 1;
    (void)rate;
    QByteArray chunk("bext", 4);
    const quint32 n = (quint32)bext.size();
    chunk.append((char)(n & 255)).append((char)((n >> 8) & 255)).append((char)((n >> 16) & 255)).append((char)((n >> 24) & 255));
    chunk.append(bext);
    QByteArray h = head;
    const quint64 riff = (quint64)in.size() - 8 + (quint64)chunk.size();
    const quint32 r32 = (quint32)std::min<quint64>(0xFFFFFFFFull, riff);
    h[4] = (char)(r32 & 255); h[5] = (char)((r32 >> 8) & 255); h[6] = (char)((r32 >> 16) & 255); h[7] = (char)((r32 >> 24) & 255);
    bool ok = out.write(h) == h.size() && out.write(chunk) == chunk.size();
    while (ok && !in.atEnd()) {
        const QByteArray b = in.read(1 << 20);
        ok = out.write(b) == b.size();
    }
    out.close();
    in.close();
    if (!ok) { QFile::remove(tmp); return false; }
    QFile::remove(path + ".bak.tmp");
    if (!QFile::rename(path, path + ".bak.tmp")) { QFile::remove(tmp); return false; }
    if (!QFile::rename(tmp, path)) { QFile::rename(path + ".bak.tmp", path); return false; }
    QFile::remove(path + ".bak.tmp");
    return true;
}

// The Recorded Files dialog (Preferences > General "Prompt to keep files after recording"): Delete / Delete All,
// Rename and Done. Returns the file to keep (possibly renamed), or empty if it was deleted.
static QString recordedFilesDialog(const QString& file)
{
    QDialog d(QApplication::activeWindow());
    d.setObjectName("recordedFilesDialog");
    d.setWindowTitle("Recorded Files");
    QVBoxLayout* v = new QVBoxLayout(&d);
    v->addWidget(new QLabel("Recorded files:", &d));
    QListWidget* list = new QListWidget(&d);
    list->setObjectName("recordedFiles");
    list->addItem(QDir::toNativeSeparators(file));
    list->setCurrentRow(0);
    v->addWidget(list, 1);
    QHBoxLayout* h = new QHBoxLayout();
    QPushButton* del = new QPushButton("&Delete", &d);
    del->setObjectName("delete");
    QPushButton* delAll = new QPushButton("Delete &All", &d);
    delAll->setObjectName("deleteAll");
    QPushButton* ren = new QPushButton("&Rename...", &d);
    ren->setObjectName("rename");
    QPushButton* done = new QPushButton("Done", &d);
    done->setObjectName("done");
    done->setDefault(true);
    h->addWidget(del);
    h->addWidget(delAll);
    h->addWidget(ren);
    h->addStretch(1);
    h->addWidget(done);
    v->addLayout(h);
    QString keep = file;
    auto remove = [&] {
        if (QMessageBox::question(&d, "Recorded Files", "Delete " + QDir::toNativeSeparators(keep) + "?") != QMessageBox::Yes) return;
        QFile::remove(keep);
        keep.clear();
        d.accept();
    };
    QObject::connect(del, &QPushButton::clicked, &d, remove);
    QObject::connect(delAll, &QPushButton::clicked, &d, remove);
    QObject::connect(ren, &QPushButton::clicked, &d, [&] {
        bool ok = false;
        const QString name = QInputDialog::getText(&d, "Rename", "New name:", QLineEdit::Normal, QFileInfo(keep).completeBaseName(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        const QString to = QFileInfo(keep).absoluteDir().filePath(name + ".wav");
        if (QFileInfo::exists(to)) { QMessageBox::warning(&d, "Rename", "There is a file of that name already."); return; }
        if (QFile::rename(keep, to)) {
            keep = to;
            list->item(0)->setText(QDir::toNativeSeparators(keep));
        }
    });
    QObject::connect(done, &QPushButton::clicked, &d, &QDialog::accept);
    d.exec();
    return keep;
}

Recorder::Recorder(Playback* playback) : pb_(playback)
{
    inputFactory = [] { return createAudioInput(); };
    listener_ = AppState::get().addListener([this](int changes) {
        if ((changes & kPlayback) && recording_ && !stopping_ && pb_ && !pb_->playing()) stop();
        if ((changes & kProject) && !recording_) {
            AppState& app = AppState::get();
            bool changed = false;
            for (auto it = app.armedTracks.begin(); it != app.armedTracks.end();) {
                const Track* t = nullptr;
                for (const Track& x : app.project().tracks)
                    if (x.id == *it) t = &x;
                if (!t || t->type != TrackType::Audio) {
                    it = app.armedTracks.erase(it);
                    changed = true;
                } else {
                    ++it;
                }
            }
            if (changed) syncArming();
        }
    });
}

Recorder::~Recorder()
{
    AppState::get().removeListener(listener_);
    if (recording_) {
        std::lock_guard<std::mutex> lk(mutex_);
        writer_.finish();
    }
    closeInput();
}

std::string Recorder::recordFolder() const
{
    AppState& app = AppState::get();
    std::string dir = app.prefs.recordFolder;
    if (dir.empty()) {
        if (!app.path().empty()) dir = pathJoin(pathDir(app.path()), "Recorded");
        else {
            QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            if (docs.isEmpty()) docs = QDir::homePath();
            dir = QDir(docs).filePath("Reno Recorded").toStdString();
        }
    }
    QDir().mkpath(QString::fromStdString(dir));
    return dir;
}

std::string Recorder::inputName() const { return input_ ? input_->name() : std::string(); }

bool Recorder::openInput(std::string* error)
{
    if (input_) return true;
    std::unique_ptr<AudioInput> in = inputFactory ? inputFactory() : nullptr;
    if (!in) {
        if (error) *error = "this build of Reno has no sound input (Qt Multimedia)";
        return false;
    }
    rate_ = AppState::get().project().settings.sampleRate > 0 ? AppState::get().project().settings.sampleRate : 48000;
    if (!in->start(rate_, [this](const float* l, const float* r, int n) { push(l, r, n); }, error)) return false;
    input_ = std::move(in);
    return true;
}

void Recorder::closeInput()
{
    if (!input_) return;
    input_->stop();
    input_.reset();
    std::lock_guard<std::mutex> lk(mutex_);
    peakL_ = peakR_ = 0;
}

void Recorder::push(const float* l, const float* r, int frames)
{
    std::lock_guard<std::mutex> lk(mutex_);
    float pl = 0, pr = 0;
    for (int k = 0; k < frames; ++k) {
        pl = std::max(pl, std::fabs(l[k]));
        pr = std::max(pr, std::fabs(r ? r[k] : l[k]));
    }
    peakL_ = std::max(peakL_, pl);
    peakR_ = std::max(peakR_, pr);
    if (recording_ && writer_.isOpen()) writer_.write(l, r, frames);
}

void Recorder::inputLevel(float& left, float& right)
{
    std::lock_guard<std::mutex> lk(mutex_);
    left = peakL_;
    right = peakR_;
    peakL_ *= 0.8f;
    peakR_ *= 0.8f;
}

double Recorder::recordedSeconds() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return recording_ ? (double)writer_.frames() / rate_ : 0.0;
}

void Recorder::setArmed(uint64_t trackId, bool on)
{
    AppState& app = AppState::get();
    const Track* t = nullptr;
    for (const Track& x : app.project().tracks)
        if (x.id == trackId) t = &x;
    if (!t || t->type != TrackType::Audio) return;
    if (on) app.armedTracks.insert(trackId);
    else app.armedTracks.erase(trackId);
    syncArming();
    app.notify(kSelection);
}

bool Recorder::armed(uint64_t trackId) const { return AppState::get().armedTracks.count(trackId) != 0; }

void Recorder::armSelected()
{
    AppState& app = AppState::get();
    const Track* t = nullptr;
    for (const Track& x : app.project().tracks)
        if (x.id == app.currentTrack) t = &x;
    if (!t || t->type != TrackType::Audio) {
        app.status("Arm for Record: select an audio track first.");
        return;
    }
    setArmed(t->id, !armed(t->id));
    app.status(armed(t->id) ? "Track armed for recording." : "Track no longer armed for recording.");
}

void Recorder::syncArming()
{
    if (recording_) return;
    if (AppState::get().armedTracks.empty()) closeInput();
    else {
        std::string err;
        if (!openInput(&err)) AppState::get().status("The sound input could not be opened: " + err);
    }
    if (onChanged) onChanged();
}

bool Recorder::start(std::string* error)
{
    if (recording_) return true;
    AppState& app = AppState::get();
    // nothing armed: the selected audio track, else a new one (asks first if Preferences > Audio "Prompt user when no
    // track has been armed for record" is on)
    if (app.armedTracks.empty() && vprefs::on("prefs/promptNoArmed") && QApplication::activeWindow()) {
        if (QMessageBox::question(QApplication::activeWindow(), "Record", "No track has been armed for recording. Record onto the selected audio track "
                                                                           "(or a new one)?") != QMessageBox::Yes) {
            if (error) *error = "no track is armed for recording";
            return false;
        }
    }
    if (app.armedTracks.empty()) {
        uint64_t id = 0;
        for (const Track& t : app.project().tracks)
            if (t.id == app.currentTrack && t.type == TrackType::Audio) id = t.id;
        if (!id) id = app.addTrack(TrackType::Audio);
        if (id) app.armedTracks.insert(id);
        app.notify(kSelection);
    }
    if (!openInput(error)) return false;
    const std::string dir = recordFolder();
    std::string base = app.path().empty() ? std::string("Recording") : pathStem(app.path()) + " Recording";
    std::string path;
    for (int n = 1; n < 100000; ++n) {
        path = pathJoin(dir, strfmt("%s %d.wav", base.c_str(), n));
        if (!fileExists(path)) break;
    }
    const int bits = app.project().settings.audioBits == 24 ? 24 : (app.project().settings.audioBits >= 32 ? 32 : 16);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!writer_.open(path, rate_, 2, bits, error)) return false;
    }
    tracks_.assign(app.armedTracks.begin(), app.armedTracks.end());
    file_ = path;
    if (pb_) pb_->pause();
    startTime_ = app.cursor();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        recording_ = true;
    }
    if (pb_) pb_->play();
    app.status("Recording to " + path);
    if (onChanged) onChanged();
    return true;
}

std::vector<uint64_t> Recorder::stop()
{
    std::vector<uint64_t> made;
    if (!recording_ || stopping_) return made;
    stopping_ = true;
    AppState& app = AppState::get();
    if (pb_ && pb_->playing()) pb_->pause();
    int64_t frames = 0;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        recording_ = false;
        frames = writer_.frames();
        ok = writer_.finish();
    }
    lastFile_ = file_;
    if (app.armedTracks.empty()) closeInput();
    stopping_ = false;
    if (!ok || frames <= 0) {
        app.status(frames <= 0 ? "Nothing was recorded." : "The recording could not be written to " + file_);
        if (frames <= 0) QFile::remove(QString::fromStdString(file_));
        if (onChanged) onChanged();
        return made;
    }
    if (vprefs::on("prefs/recordBwf")) addBextChunk(QString::fromStdString(file_), (uint64_t)std::llround(std::max(0.0, startTime_) * rate_), rate_);
    if (vprefs::on("prefs/promptKeepRecorded") && QApplication::activeWindow()) {
        const QString keep = recordedFilesDialog(QString::fromStdString(file_));
        if (keep.isEmpty()) {
            app.status("The recording was deleted.");
            if (onChanged) onChanged();
            return made;
        }
        file_ = keep.toStdString();
        lastFile_ = file_;
    }
    std::string err;
    const uint64_t media = app.importMedia(file_, &err);
    if (!media) {
        app.status("The recording " + file_ + " could not be read back: " + err);
        if (onChanged) onChanged();
        return made;
    }
    double len = (double)frames / rate_;
    const std::string name = pathFileName(file_);
    const double at = startTime_;
    // Preferences > Audio Device: unless "Automatically detect and offset for hardware recording latency" is on, the
    // user's offset (the input's lag) is taken off the recording's start
    const double lag = vprefs::on("prefs/autoRecordLatency") ? 0.0 : std::max(0.0, std::min(len * 0.5, vprefs::real("prefs/recordOffsetMs") / 1000.0));
    len -= lag;
    const std::vector<uint64_t> tracks = tracks_;
    app.edit("Record", [&](Project& p) {
        for (uint64_t tid : tracks) {
            Track* t = p.findTrack(tid);
            if (!t) continue;
            Event e;
            e.id = p.newId();
            e.mediaId = media;
            e.start = at;
            e.length = len;
            e.offset = lag;
            e.name = name;
            setDefaultFadeCurves(e, t->type);
            std::vector<Event> keep;
            for (Event& o : t->events) {
                if (o.end() <= at + 1e-9 || o.start >= at + len - 1e-9) { keep.push_back(o); continue; }
                if (o.start < at) {
                    Event a = o;
                    a.length = at - o.start;
                    a.fadeOut = std::min(a.fadeOut, a.length);
                    keep.push_back(a);
                }
                if (o.end() > at + len) {
                    Event b = o;
                    const double cut = at + len - o.start;
                    b.id = p.newId();
                    b.start = at + len;
                    b.length = o.end() - b.start;
                    b.offset += cut * o.rate;
                    b.fadeIn = std::min(b.fadeIn, b.length);
                    shiftEventEnvelopes(b, cut);
                    keep.push_back(b);
                }
            }
            t->events = keep;
            t->events.push_back(e);
            t->sortEvents();
            made.push_back(e.id);
        }
    });
    app.status(strfmt("Recorded %.1f seconds to %s.", len, file_.c_str()));
    if (onChanged) onChanged();
    return made;
}

}  // namespace app
}  // namespace reno
