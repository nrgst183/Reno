// Recording: see recorder.h.
#include "recorder.h"
#include "playback.h"
#include "vprefs.h"
#include <QDialog>
#include <QDateTime>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
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

// The Recorded Files dialog (Preferences > General "Prompt to keep files after recording"): the files of the recording
// with Delete / Delete All, Rename and Done. Returns the files to keep (renamed ones under their new names), in the
// order given; a deleted file is gone from the list.
static QList<QPair<int, QString>> recordedFilesDialog(const QStringList& files)
{
    QDialog d(QApplication::activeWindow());
    d.setObjectName("recordedFilesDialog");
    d.setWindowTitle("Recorded Files");
    QVBoxLayout* v = new QVBoxLayout(&d);
    v->addWidget(new QLabel("Recorded files:", &d));
    QListWidget* list = new QListWidget(&d);
    list->setObjectName("recordedFiles");
    for (const QString& f : files) list->addItem(QDir::toNativeSeparators(f));
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
    QList<QPair<int, QString>> keep;
    for (int i = 0; i < files.size(); ++i) keep.append(qMakePair(i, files[i]));
    auto removeRows = [&](bool all) {
        const int row = list->currentRow();
        if (!all && (row < 0 || row >= keep.size())) return;
        const QString what = all ? QString("all the recorded files") : QDir::toNativeSeparators(keep[row].second);
        if (QMessageBox::question(&d, "Recorded Files", "Delete " + what + "?") != QMessageBox::Yes) return;
        if (all) {
            for (const auto& f : keep) QFile::remove(f.second);
            keep.clear();
            list->clear();
        } else {
            QFile::remove(keep[row].second);
            keep.removeAt(row);
            delete list->takeItem(row);
        }
        if (keep.isEmpty()) d.accept();
    };
    QObject::connect(del, &QPushButton::clicked, &d, [&] { removeRows(false); });
    QObject::connect(delAll, &QPushButton::clicked, &d, [&] { removeRows(true); });
    QObject::connect(ren, &QPushButton::clicked, &d, [&] {
        const int row = list->currentRow();
        if (row < 0 || row >= keep.size()) return;
        bool ok = false;
        const QString name = QInputDialog::getText(&d, "Rename", "New name:", QLineEdit::Normal, QFileInfo(keep[row].second).completeBaseName(), &ok).trimmed();
        if (!ok || name.isEmpty()) return;
        const QString to = QFileInfo(keep[row].second).absoluteDir().filePath(name + ".wav");
        if (QFileInfo::exists(to)) { QMessageBox::warning(&d, "Rename", "There is a file of that name already."); return; }
        if (QFile::rename(keep[row].second, to)) {
            keep[row].second = to;
            list->item(row)->setText(QDir::toNativeSeparators(to));
        }
    });
    QObject::connect(done, &QPushButton::clicked, &d, &QDialog::accept);
    d.exec();
    return keep;
}

// ---------------------------------------------------------------------------------------------- the shared inputs
InputHub& inputHub()
{
    static InputHub* hub = new InputHub();
    return *hub;
}

bool InputHub::open(const std::vector<std::pair<std::string, int>>& wanted, int sampleRate, std::string* error)
{
    std::vector<std::shared_ptr<Dev>> closing;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        rate_ = sampleRate > 0 ? sampleRate : 48000;
        for (auto it = devs_.begin(); it != devs_.end();) {
            bool keep = false;
            for (const auto& w : wanted)
                if (w.first == it->first && it->second->in) keep = true;
            if (keep) ++it;
            else {
                closing.push_back(it->second);
                it = devs_.erase(it);
            }
        }
    }
    for (auto& d : closing)
        if (d->in) d->in->stop();
    closing.clear();
    bool all = true;
    for (const auto& w : wanted) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (devs_.count(w.first)) continue;
        }
        std::unique_ptr<AudioInput> in = factory ? factory() : createAudioInput();
        if (!in) {
            if (error) *error = "this build of Reno has no sound input (Qt Multimedia)";
            all = false;
            continue;
        }
        std::shared_ptr<Dev> d = std::make_shared<Dev>();
        const std::string id = w.first;
        std::string err;
        if (!in->startDevice(id, std::max(1, w.second), rate_, [this, id](const float* x, int channels, int frames) { incoming(id, x, channels, frames); }, &err)) {
            if (error) *error = err;
            all = false;
            continue;
        }
        d->channels = in->channelCount();
        d->in = std::move(in);
        std::lock_guard<std::mutex> lk(mutex_);
        devs_[id] = d;
    }
    return all;
}

void InputHub::closeAll() { open({}, rate_, nullptr); }

bool InputHub::isOpen() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return !devs_.empty();
}

bool InputHub::isOpen(const std::string& device) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return devs_.count(device) != 0;
}

std::string InputHub::name(const std::string& device) const
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = devs_.find(device);
    return it != devs_.end() && it->second->in ? it->second->in->name() : std::string();
}

int InputHub::addListener(Listener l)
{
    std::lock_guard<std::mutex> lk(mutex_);
    listeners_[nextListener_] = std::move(l);
    return nextListener_++;
}

void InputHub::removeListener(int id)
{
    std::lock_guard<std::mutex> lk(mutex_);
    listeners_.erase(id);
}

void InputHub::push(const std::string& device, const float* x, int channels, int frames) { incoming(device, x, channels, frames); }

void InputHub::incoming(const std::string& device, const float* x, int channels, int frames)
{
    if (frames <= 0 || channels <= 0) return;
    std::vector<Listener> call;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        std::shared_ptr<Dev>& d = devs_[device];
        if (!d) d = std::make_shared<Dev>();       // sound pushed for a device that is not open (tests): kept as well
        Dev& dev = *d;
        if (dev.capacity != rate_ || dev.channels != channels || (int64_t)dev.ring.size() != (int64_t)rate_ * channels) {
            dev.channels = channels;
            dev.capacity = rate_;
            dev.ring.assign((size_t)rate_ * (size_t)channels, 0.f);
            dev.written = 0;
        }
        for (int k = 0; k < frames; ++k) {
            const size_t at = (size_t)((dev.written + k) % dev.capacity) * (size_t)channels;
            for (int c = 0; c < channels; ++c) dev.ring[at + (size_t)c] = x[(size_t)k * channels + c];
        }
        dev.written += frames;
        for (auto& l : listeners_) call.push_back(l.second);
    }
    for (auto& l : call) l(device, x, channels, frames);
}

bool InputHub::read(uint64_t reader, const std::string& device, int first, int channels, float* left, float* right, int frames)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = devs_.find(device);
    if (it == devs_.end() || it->second->capacity <= 0) return false;
    Dev& d = *it->second;
    auto key = std::make_pair(reader, device);
    auto pi = pos_.find(key);
    if (pi == pos_.end()) pi = pos_.insert(std::make_pair(key, d.written)).first;
    int64_t& pos = pi->second;
    if (d.written - pos > d.capacity) pos = d.written - d.capacity / 2;       // fell far behind: skip ahead
    if (pos > d.written) pos = d.written;
    if (d.written - pos < frames) return false;
    for (int k = 0; k < frames; ++k) {
        const size_t at = (size_t)((pos + k) % d.capacity) * (size_t)d.channels;
        const float a = first < d.channels ? d.ring[at + (size_t)first] : 0.f;
        left[k] = a;
        if (right) right[k] = channels > 1 ? (first + 1 < d.channels ? d.ring[at + (size_t)first + 1] : a) : a;
    }
    pos += frames;
    return true;
}

// ---------------------------------------------------------------------------------------------- the Recorder
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
        if ((changes & (kPlayback | kProject)) && !recording_ && !stopping_) {
            // input busses take their sound from the cards while the project plays
            AppState& app = AppState::get();
            const bool want = !app.project().inputBuses.empty() && pb_ && pb_->playing();
            if (want != busesOpen_ || (changes & kProject)) {
                busesOpen_ = want;
                syncArming();
            }
        }
    });
    hubListener_ = inputHub().addListener([this](const std::string& dev, const float* x, int ch, int n) { deviceSound(dev, x, ch, n); });
    timer_ = new QTimer();
    timer_->setInterval(30);
    QObject::connect(timer_, &QTimer::timeout, [this] {
        if (!recording_ || stopping_ || stopAt_ <= 0) return;
        if (AppState::get().cursor() >= stopAt_ - 1e-3) stop();
    });
    timer_->start();
    if (pb_) {
        pb_->setLiveInputs(
            [](const InputBus& ib, float* l, float* r, int n) { return inputHub().read(ib.id, ib.device, ib.first, ib.channels, l, r, n); },
            [this](uint64_t id, const float* l, const float* r, int n) { inputBusSound(id, l, r, n); },
            [this](const Track& tr, int64_t start, float* l, float* r, int n) { return monitorSound(tr, start, l, r, n); });
    }
}

Recorder::~Recorder()
{
    AppState::get().removeListener(listener_);
    inputHub().removeListener(hubListener_);
    if (pb_) pb_->setLiveInputs(nullptr, nullptr, nullptr);
    delete timer_;
    if (recording_) {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& w : writers_) w->finish();
    }
    closeInputs();
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

std::string Recorder::inputName() const
{
    for (const auto& d : lastDevices_) {
        const std::string n = inputHub().name(d.first);
        if (!n.empty()) return n;
    }
    return std::string();
}

// The inputs wanted now: those of the armed tracks, and (while the project plays) those of the input busses.
std::vector<std::pair<std::string, int>> Recorder::wantedInputs() const
{
    AppState& app = AppState::get();
    const Project& p = app.project();
    RecordRouter r;
    r.setup(planRecordSources(p, app.armedTracks), nullptr);
    std::vector<std::pair<std::string, int>> out = r.devices();
    if (busesOpen_ || recording_)
        for (const InputBus& ib : p.inputBuses) {
            const int need = ib.first + ib.channels;
            bool found = false;
            for (auto& d : out)
                if (d.first == ib.device) {
                    d.second = std::max(d.second, need);
                    found = true;
                }
            if (!found) out.push_back(std::make_pair(ib.device, need));
        }
    return out;
}

bool Recorder::openInputs(std::string* error)
{
    AppState& app = AppState::get();
    rate_ = app.project().settings.sampleRate > 0 ? app.project().settings.sampleRate : 48000;
    inputHub().factory = inputFactory;
    lastDevices_ = wantedInputs();
    // a device that is already open with fewer channels than now needed is opened again
    const bool ok = inputHub().open(lastDevices_, rate_, error);
    open_ = inputHub().isOpen();
    return ok;
}

void Recorder::closeInputs()
{
    inputHub().closeAll();
    open_ = false;
    lastDevices_.clear();
    std::lock_guard<std::mutex> lk(mutex_);
    peakL_ = peakR_ = 0;
}

void Recorder::deviceSound(const std::string& device, const float* x, int channels, int frames)
{
    std::lock_guard<std::mutex> lk(mutex_);
    float pl = 0, pr = 0;
    for (int k = 0; k < frames; ++k) {
        pl = std::max(pl, std::fabs(x[(size_t)k * channels]));
        pr = std::max(pr, std::fabs(channels > 1 ? x[(size_t)k * channels + 1] : x[(size_t)k * channels]));
    }
    peakL_ = std::max(peakL_, pl);
    peakR_ = std::max(peakR_, pr);
    if (recording_) router_.pushDevice(device, x, channels, frames);
}

void Recorder::inputBusSound(uint64_t id, const float* l, const float* r, int frames)
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (recording_) router_.pushInputBus(id, l, r, frames);
}

void Recorder::sink(int source, const float* l, const float* r, int frames)
{
    if (source >= 0 && source < (int)writers_.size() && writers_[(size_t)source]->isOpen()) writers_[(size_t)source]->write(l, r, frames);
}

// Input Monitoring: the sound of an armed track's input is heard through the track's effects and fader (during recording;
// with Auto, when recording into events, only where the cursor is over them).
bool Recorder::monitorSound(const Track& tr, int64_t start, float* l, float* r, int n)
{
    AppState& app = AppState::get();
    if (tr.recordInput.monitor == "off" || tr.recordInput.inputBus || !app.armedTracks.count(tr.id)) return false;
    if (recording_ && tr.recordInput.monitor == "auto") {
        // only over the events it records into
        const double t = (double)start / std::max(1, rate_);
        std::lock_guard<std::mutex> lk(mutex_);
        bool into = false, any = false;
        for (size_t i = 0; i < sources_.size(); ++i)
            for (uint64_t id : targets_[i]) {
                const Event* e = app.project().findEvent(id);
                if (!e) continue;
                bool mine = false;
                for (uint64_t tid : sources_[i].tracks) mine = mine || tid == tr.id;
                if (!mine) continue;
                any = true;
                if (t >= e->start && t < e->end()) into = true;
            }
        if (any && !into) return false;
    }
    return inputHub().read(tr.id | (1ull << 62), tr.recordInput.device, tr.recordInput.first, tr.recordInput.channels, l, r, n);
}

double Recorder::recordedSeconds() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    int64_t most = 0;
    for (const auto& w : writers_) most = std::max(most, w->frames());
    return recording_ ? (double)most / rate_ : 0.0;
}

void Recorder::inputLevel(float& left, float& right)
{
    std::lock_guard<std::mutex> lk(mutex_);
    left = peakL_;
    right = peakR_;
    peakL_ *= 0.8f;
    peakR_ *= 0.8f;
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
    if (AppState::get().armedTracks.empty() && !busesOpen_) closeInputs();
    else {
        std::string err;
        if (!openInputs(&err)) AppState::get().status("The sound input could not be opened: " + err);
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
    recording_ = false;
    busesOpen_ = !app.project().inputBuses.empty();
    if (!openInputs(error)) return false;
    const Project& proj = app.project();
    sources_ = planRecordSources(proj, app.armedTracks);
    if (sources_.empty()) {
        if (error) *error = "no audio track is armed for recording";
        return false;
    }
    // where it starts and stops: into the selected events of the armed tracks (a take of each, a punch-in when a time
    // selection is made round them), else a new event from the cursor
    const bool region = app.loopEnd > app.loopStart;
    const RecordSpan span = planRecordSpan(proj, app.armedTracks, app.selection(), app.cursor(), region ? app.loopStart : 0.0, region ? app.loopEnd : 0.0);
    targets_.assign(sources_.size(), std::vector<uint64_t>());
    for (size_t i = 0; i < sources_.size(); ++i)
        for (uint64_t tid : sources_[i].tracks)
            for (const Track& t : proj.tracks)
                if (t.id == tid)
                    for (uint64_t eid : recordTargetEvents(t, app.selection())) targets_[i].push_back(eid);
    const std::string dir = recordFolder();
    std::string base = app.path().empty() ? std::string("Recording") : pathStem(app.path()) + " Recording";
    const int bits = proj.settings.audioBits == 24 ? 24 : (proj.settings.audioBits >= 32 ? 32 : 16);
    files_.clear();
    writers_.clear();
    int number = 0;
    for (size_t i = 0; i < sources_.size(); ++i) {
        std::string path;
        for (++number; number < 100000; ++number) {
            path = pathJoin(dir, strfmt("%s %d.wav", base.c_str(), number));
            bool taken = fileExists(path);
            for (const auto& f : files_) taken = taken || f == path;
            if (!taken) break;
        }
        std::unique_ptr<WavWriter> w(new WavWriter());
        if (!w->open(path, rate_, sources_[i].channels(), bits, error)) {
            for (auto& o : writers_) o->finish();
            for (const auto& f : files_) QFile::remove(QString::fromStdString(f));
            writers_.clear();
            files_.clear();
            return false;
        }
        writers_.push_back(std::move(w));
        files_.push_back(path);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        router_.setup(sources_, [this](int s, const float* l, const float* r, int n) { sink(s, l, r, n); });
    }
    tracks_.assign(app.armedTracks.begin(), app.armedTracks.end());
    if (pb_) pb_->pause();
    app.setCursor(span.start);
    startTime_ = app.cursor();
    stopAt_ = (!app.loopOn && span.stop > span.start) ? span.stop : 0.0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        recording_ = true;
    }
    if (pb_) {
        pb_->setPlayPastEnd(true);
        pb_->play();
    }
    app.status("Recording to " + files_.front());
    if (onChanged) onChanged();
    return true;
}

std::vector<uint64_t> Recorder::stop()
{
    std::vector<uint64_t> made;
    if (!recording_ || stopping_) return made;
    stopping_ = true;
    AppState& app = AppState::get();
    if (pb_) pb_->setPlayPastEnd(false);
    if (pb_ && pb_->playing()) pb_->pause();
    std::vector<int64_t> frames;
    bool ok = true;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        recording_ = false;
        for (auto& w : writers_) {
            frames.push_back(w->frames());
            ok = w->finish() && ok;
        }
    }
    lastFile_ = files_.empty() ? std::string() : files_.front();
    lastFiles_ = files_;
    busesOpen_ = false;
    if (app.armedTracks.empty()) closeInputs();
    else syncArming();
    stopAt_ = 0;
    stopping_ = false;
    int64_t most = 0;
    for (int64_t f : frames) most = std::max(most, f);
    if (!ok || most <= 0) {
        app.status(most <= 0 ? "Nothing was recorded." : "The recording could not be written to " + (files_.empty() ? std::string() : files_.front()));
        if (most <= 0)
            for (const auto& f : files_) QFile::remove(QString::fromStdString(f));
        if (onChanged) onChanged();
        return made;
    }
    // files with nothing in them go
    std::vector<std::string> names;
    std::vector<size_t> sourceOf;
    for (size_t i = 0; i < files_.size(); ++i) {
        if (frames[i] <= 0) {
            QFile::remove(QString::fromStdString(files_[i]));
            continue;
        }
        names.push_back(files_[i]);
        sourceOf.push_back(i);
    }
    if (vprefs::on("prefs/recordBwf"))
        for (const auto& f : names) addBextChunk(QString::fromStdString(f), (uint64_t)std::llround(std::max(0.0, startTime_) * rate_), rate_);
    if (vprefs::on("prefs/promptKeepRecorded") && QApplication::activeWindow()) {
        QStringList list;
        for (const auto& f : names) list << QString::fromStdString(f);
        const QList<QPair<int, QString>> keep = recordedFilesDialog(list);
        std::vector<std::string> kept;
        std::vector<size_t> keptSource;
        for (const auto& k : keep) {
            kept.push_back(k.second.toStdString());
            keptSource.push_back(sourceOf[(size_t)k.first]);
        }
        if (kept.empty()) {
            app.status("The recording was deleted.");
            if (onChanged) onChanged();
            return made;
        }
        names = kept;
        sourceOf = keptSource;
        lastFiles_ = names;
        lastFile_ = names.front();
    }
    // Preferences > Audio Device: unless "Automatically detect and offset for hardware recording latency" is on, the
    // user's offset (the input's lag) is taken off the recording's start
    double lenMost = 0;
    for (size_t i = 0; i < names.size(); ++i) lenMost = std::max(lenMost, (double)frames[sourceOf[i]] / rate_);
    const double lag = vprefs::on("prefs/autoRecordLatency") ? 0.0 : std::max(0.0, std::min(lenMost * 0.5, vprefs::real("prefs/recordOffsetMs") / 1000.0));
    RecordCommit rc;
    rc.start = startTime_;
    rc.lag = lag;
    rc.loop = app.loopOn && app.loopEnd > app.loopStart;
    rc.loopStart = app.loopStart;
    rc.loopEnd = app.loopEnd;
    std::string err;
    for (size_t i = 0; i < names.size(); ++i) {
        const uint64_t media = app.importMedia(names[i], &err);
        if (!media) {
            app.status("The recording " + names[i] + " could not be read back: " + err);
            continue;
        }
        const size_t src = sourceOf[i];
        for (uint64_t tid : sources_[src].tracks) {
            RecordCommit::Item it;
            it.track = tid;
            it.mediaId = media;
            it.length = (double)frames[src] / rate_;
            it.name = pathFileName(names[i]);
            for (uint64_t eid : targets_[src])
                if (const Event* e = app.project().findEvent(eid))
                    for (const Track& t : app.project().tracks)
                        if (t.id == tid)
                            for (const Event& x : t.events)
                                if (x.id == e->id) it.events.push_back(eid);
            rc.items.push_back(it);
        }
    }
    if (rc.items.empty()) {
        if (onChanged) onChanged();
        return made;
    }
    app.edit("Record", [&](Project& p) { made = commitRecording(p, rc); });
    app.status(strfmt("Recorded %.1f seconds to %s.", lenMost - lag, lastFile_.c_str()));
    if (onChanged) onChanged();
    return made;
}

}  // namespace app
}  // namespace reno

namespace reno {
namespace app {

namespace {
const std::vector<InputDeviceInfo>*& testDevices()
{
    static const std::vector<InputDeviceInfo>* list = nullptr;
    return list;
}
}

void setInputDevicesForTest(const std::vector<InputDeviceInfo>* list) { testDevices() = list; }

std::vector<InputDeviceInfo> inputDevices()
{
    if (testDevices()) return *testDevices();
    std::vector<InputDeviceInfo> out;
    for (const auto& d : audioInputDevices()) {
        InputDeviceInfo i;
        i.id = d.first;
        i.name = d.second;
        i.channels = std::max(1, audioInputChannels(d.first));
        out.push_back(i);
    }
    return out;
}

QString recordInputText(const Project& p, const RecordInput& r)
{
    if (r.inputBus) {
        const InputBus* b = p.findInputBus(r.inputBus);
        return b ? QString::fromStdString(b->name) : QString("Input Bus");
    }
    QString dev;
    for (const InputDeviceInfo& d : inputDevices())
        if (d.id == r.device) dev = QString::fromStdString(d.name);
    const QString ch = r.channels == 1 ? QString("In %1").arg(r.first + 1) : QString("In %1+%2").arg(r.first + 1).arg(r.first + 2);
    return dev.isEmpty() ? ch : dev + ": " + ch;
}

void fillRecordInputMenu(QMenu* m, const RecordInput& cur, bool withTrackItems, const std::function<void(const RecordInput&)>& set)
{
    auto pick = [&](QMenu* in, const QString& text, const QString& name, bool checked, std::function<void()> fn) {
        QAction* a = in->addAction(text);
        a->setObjectName(name);
        a->setCheckable(true);
        a->setChecked(checked);
        QObject::connect(a, &QAction::triggered, m, [fn] { fn(); });
        return a;
    };
    // (a menu that is filled again drops its old submenus: clear() leaves them standing)
    m->clear();
    for (QMenu* old : m->findChildren<QMenu*>(QString(), Qt::FindDirectChildrenOnly)) delete old;
    std::vector<InputDeviceInfo> devs = inputDevices();
    if (devs.empty()) {
        QAction* none = m->addAction("No input devices");
        none->setEnabled(false);
    }
    for (const InputDeviceInfo& d : devs) {
        QMenu* dm = m->addMenu(QString::fromStdString(d.name.empty() ? d.id : d.name));
        dm->setObjectName("device:" + QString::fromStdString(d.id));
        const int n = std::max(1, d.channels);
        for (int width = 1; width <= 2; ++width) {
            if (width == 2 && n < 2) break;
            QMenu* wm = dm->addMenu(width == 1 ? "Mono" : "Stereo");
            wm->setObjectName(QString("%1:%2").arg(width == 1 ? "mono" : "stereo", QString::fromStdString(d.id)));
            for (int first = 0; first + width <= n; first += width) {
                const QString text = width == 1 ? QString("Input %1").arg(first + 1) : QString("Inputs %1 + %2").arg(first + 1).arg(first + 2);
                const bool on = !cur.inputBus && cur.device == d.id && cur.first == first && cur.channels == width;
                const std::string id = d.id;
                pick(wm, text, QString("src:%1:%2:%3").arg(QString::fromStdString(id)).arg(first).arg(width), on, [=] {
                    RecordInput r = cur;
                    r.device = id;
                    r.first = first;
                    r.channels = width;
                    r.inputBus = 0;
                    set(r);
                });
            }
        }
    }
    if (!withTrackItems) return;
    const Project& p = AppState::get().project();
    if (!p.inputBuses.empty()) {
        m->addSeparator();
        QMenu* bm = m->addMenu("Input Busses");
        bm->setObjectName("inputBusses");
        for (const InputBus& b : p.inputBuses) {
            const uint64_t id = b.id;
            pick(bm, QString::fromStdString(b.name), QString("inputBus:%1").arg((qulonglong)id), cur.inputBus == id, [=] {
                RecordInput r = cur;
                r.inputBus = id;
                set(r);
            });
        }
    }
    m->addSeparator();
    QMenu* mm = m->addMenu("Input Monitoring");
    mm->setObjectName("inputMonitoring");
    static const char* kModes[3][2] = {{"off", "Off"}, {"on", "On"}, {"auto", "Auto"}};
    for (const auto& mode : kModes) {
        const std::string key = mode[0];
        pick(mm, mode[1], QString("monitor:%1").arg(mode[0]), cur.monitor == key, [=] {
            RecordInput r = cur;
            r.monitor = key;
            set(r);
        });
    }
}

}
}
