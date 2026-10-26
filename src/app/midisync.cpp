// MIDI Timecode and Clock; see midisync.h.
#include "midisync.h"
#include "vprefs.h"
#include "timesource.h"
#include "playback.h"
#include <QDateTime>
#include <QSettings>
#include <QTimer>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace reno {
namespace app {

// ------------------------------------------------------------------------------------------ Windows' MIDI ports
#ifdef _WIN32
namespace {
class WinMidiOut : public MidiPort {
public:
    HMIDIOUT h = nullptr;
    std::string label;
    ~WinMidiOut() override
    {
        if (h) {
            midiOutReset(h);
            midiOutClose(h);
        }
    }
    bool send(const std::vector<uint8_t>& m) override
    {
        if (!h || m.empty()) return false;
        if (m[0] == 0xF0) {
            // a system exclusive message goes as a "long" one, through a header given to Windows and then released
            std::vector<char> buf(m.begin(), m.end());
            MIDIHDR hdr = {};
            hdr.lpData = buf.data();
            hdr.dwBufferLength = (DWORD)buf.size();
            hdr.dwBytesRecorded = (DWORD)buf.size();
            if (midiOutPrepareHeader(h, &hdr, sizeof hdr) != MMSYSERR_NOERROR) return false;
            const bool ok = midiOutLongMsg(h, &hdr, sizeof hdr) == MMSYSERR_NOERROR;
            for (int k = 0; k < 200 && (midiOutUnprepareHeader(h, &hdr, sizeof hdr) == MIDIERR_STILLPLAYING); ++k) Sleep(1);
            return ok;
        }
        DWORD msg = 0;
        for (size_t k = 0; k < m.size() && k < 3; ++k) msg |= (DWORD)m[k] << (8 * k);
        return midiOutShortMsg(h, msg) == MMSYSERR_NOERROR;
    }
    std::string name() const override { return label; }
};

class WinMidiIn : public MidiPort {
public:
    HMIDIIN h = nullptr;
    std::string label;
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    MIDIHDR hdr[2] = {};
    char buf[2][256] = {};
    ~WinMidiIn() override
    {
        *alive = false;
        if (h) {
            midiInStop(h);
            midiInReset(h);
            for (MIDIHDR& x : hdr) midiInUnprepareHeader(h, &x, sizeof x);
            midiInClose(h);
        }
    }
    bool send(const std::vector<uint8_t>&) override { return false; }
    std::string name() const override { return label; }
    // Windows' thread: incoming data is carried over to the interface thread
    static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR inst, DWORD_PTR p1, DWORD_PTR)
    {
        WinMidiIn* self = (WinMidiIn*)inst;
        if (!self) return;
        std::shared_ptr<std::atomic<bool>> alive = self->alive;
        if (msg == MIM_DATA) {
            std::vector<uint8_t> m;
            const uint8_t status = (uint8_t)(p1 & 0xFF);
            m.push_back(status);
            const int len = (status == 0xF1 || status == 0xF3 || (status >= 0xC0 && status < 0xE0)) ? 2 : (status >= 0xF4 ? 1 : 3);
            for (int k = 1; k < len; ++k) m.push_back((uint8_t)((p1 >> (8 * k)) & 0xFF));
            AppState::post([self, alive, m] {
                if (*alive && self->onReceive) self->onReceive(m);
            });
        } else if (msg == MIM_LONGDATA) {
            MIDIHDR* hd = (MIDIHDR*)p1;
            std::vector<uint8_t> m((uint8_t*)hd->lpData, (uint8_t*)hd->lpData + hd->dwBytesRecorded);
            AppState::post([self, alive, m, hd] {
                if (!*alive) return;
                if (!m.empty() && self->onReceive) self->onReceive(m);
                midiInAddBuffer(self->h, hd, sizeof *hd);       // given back for the next one
            });
        }
    }
};
}  // namespace

std::vector<std::string> midiOutputNames()
{
    std::vector<std::string> out;
    const UINT n = midiOutGetNumDevs();
    for (UINT k = 0; k < n; ++k) {
        MIDIOUTCAPSW caps = {};
        out.push_back(midiOutGetDevCapsW(k, &caps, sizeof caps) == MMSYSERR_NOERROR ? narrow(caps.szPname) : strfmt("MIDI output %u", k + 1));
    }
    return out;
}

std::vector<std::string> midiInputNames()
{
    std::vector<std::string> out;
    const UINT n = midiInGetNumDevs();
    for (UINT k = 0; k < n; ++k) {
        MIDIINCAPSW caps = {};
        out.push_back(midiInGetDevCapsW(k, &caps, sizeof caps) == MMSYSERR_NOERROR ? narrow(caps.szPname) : strfmt("MIDI input %u", k + 1));
    }
    return out;
}

std::unique_ptr<MidiPort> openMidiOutput(int index, std::string* error)
{
    const std::vector<std::string> names = midiOutputNames();
    if (index < 0 || index >= (int)names.size()) {
        if (error) *error = names.empty() ? "there is no MIDI output" : "that MIDI output is not there";
        return nullptr;
    }
    std::unique_ptr<WinMidiOut> p(new WinMidiOut());
    if (midiOutOpen(&p->h, (UINT)index, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        p->h = nullptr;
        if (error) *error = names[(size_t)index] + " could not be opened";
        return nullptr;
    }
    p->label = names[(size_t)index];
    return std::unique_ptr<MidiPort>(p.release());
}

std::unique_ptr<MidiPort> openMidiInput(int index, std::string* error)
{
    const std::vector<std::string> names = midiInputNames();
    if (index < 0 || index >= (int)names.size()) {
        if (error) *error = names.empty() ? "there is no MIDI input" : "that MIDI input is not there";
        return nullptr;
    }
    std::unique_ptr<WinMidiIn> p(new WinMidiIn());
    if (midiInOpen(&p->h, (UINT)index, (DWORD_PTR)&WinMidiIn::proc, (DWORD_PTR)p.get(), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
        p->h = nullptr;
        if (error) *error = names[(size_t)index] + " could not be opened";
        return nullptr;
    }
    for (int k = 0; k < 2; ++k) {
        p->hdr[k].lpData = p->buf[k];
        p->hdr[k].dwBufferLength = sizeof p->buf[k];
        midiInPrepareHeader(p->h, &p->hdr[k], sizeof p->hdr[k]);
        midiInAddBuffer(p->h, &p->hdr[k], sizeof p->hdr[k]);
    }
    midiInStart(p->h);
    p->label = names[(size_t)index];
    return std::unique_ptr<MidiPort>(p.release());
}
#else
std::vector<std::string> midiOutputNames() { return {}; }
std::vector<std::string> midiInputNames() { return {}; }
std::unique_ptr<MidiPort> openMidiOutput(int, std::string* error)
{
    if (error) *error = "MIDI ports are only available on Windows";
    return nullptr;
}
std::unique_ptr<MidiPort> openMidiInput(int, std::string* error)
{
    if (error) *error = "MIDI ports are only available on Windows";
    return nullptr;
}
#endif

std::vector<int> availableMidiPorts(const std::vector<std::string>& names, bool output)
{
    const QStringList chosen = vprefs::list(output ? "prefs/midiOutputs" : "prefs/midiInputs");      // empty: every port
    std::vector<int> out;
    for (size_t k = 0; k < names.size(); ++k)
        if (chosen.isEmpty() || chosen.contains(QString::fromStdString(names[k]))) out.push_back((int)k);
    return out;
}

// ------------------------------------------------------------------------------------------ MTC
int MidiSync::rateCode(double fps)
{
    if (std::fabs(fps - 24.0) < 0.5 || std::fabs(fps - 23.976) < 0.01) return 0;
    if (std::fabs(fps - 25.0) < 0.5 || std::fabs(fps - 50.0) < 0.5) return 1;
    if (std::fabs(fps - 29.97) < 0.01 || std::fabs(fps - 59.94) < 0.02) return 2;
    return 3;
}

double MidiSync::rateOfCode(int code)
{
    static const double r[4] = {24.0, 25.0, 30000.0 / 1001.0, 30.0};
    return r[std::max(0, std::min(3, code))];
}

// 29.97 drop-frame: frame numbers 0 and 1 are left out of every minute except each tenth
void MidiSync::timecodeOf(double seconds, int code, int& h, int& m, int& s, int& f)
{
    if (!(seconds > 0)) seconds = 0;
    if (code == 2) {
        int64_t frames = (int64_t)std::floor(seconds * 30000.0 / 1001.0 + 1e-6);
        const int64_t per10 = 17982, per1 = 1798;
        const int64_t d = frames / per10, rem = frames % per10;
        frames += 18 * d + (rem > 1 ? 2 * ((rem - 2) / per1) : 0);
        f = (int)(frames % 30);
        s = (int)((frames / 30) % 60);
        m = (int)((frames / 1800) % 60);
        h = (int)((frames / 108000) % 24);
        return;
    }
    const int fps = code == 0 ? 24 : (code == 1 ? 25 : 30);
    const int64_t frames = (int64_t)std::floor(seconds * fps + 1e-6);
    f = (int)(frames % fps);
    s = (int)((frames / fps) % 60);
    m = (int)((frames / (60 * fps)) % 60);
    h = (int)((frames / (3600 * fps)) % 24);
}

double MidiSync::secondsOf(int h, int m, int s, int f, int code)
{
    if (code == 2) {
        const int64_t totalMinutes = 60 * h + m;
        int64_t frames = (int64_t)((3600 * h + 60 * m + s) * 30 + f) - 2 * (totalMinutes - totalMinutes / 10);
        return frames * 1001.0 / 30000.0;
    }
    const int fps = code == 0 ? 24 : (code == 1 ? 25 : 30);
    return (double)((3600 * h + 60 * m + s) * fps + f) / fps;
}

std::vector<uint8_t> MidiSync::fullFrame(double seconds, int code)
{
    int h, m, s, f;
    timecodeOf(seconds, code, h, m, s, f);
    return {0xF0, 0x7F, 0x7F, 0x01, 0x01, (uint8_t)((code << 5) | h), (uint8_t)m, (uint8_t)s, (uint8_t)f, 0xF7};
}

std::vector<uint8_t> MidiSync::quarterFrame(int piece, int h, int m, int s, int f, int code)
{
    int v = 0;
    switch (piece & 7) {
    case 0: v = f & 15; break;
    case 1: v = (f >> 4) & 1; break;
    case 2: v = s & 15; break;
    case 3: v = (s >> 4) & 3; break;
    case 4: v = m & 15; break;
    case 5: v = (m >> 4) & 3; break;
    case 6: v = h & 15; break;
    case 7: v = ((h >> 4) & 1) | (code << 1); break;
    }
    return {0xF1, (uint8_t)(((piece & 7) << 4) | v)};
}

// ------------------------------------------------------------------------------------------ MidiSync
MidiSync::MidiSync(Playback* playback) : pb_(playback)
{
    QSettings st;
    outIndex_ = st.value("midi/output", 0).toInt();
    inIndex_ = st.value("midi/input", 0).toInt();
    outputFactory = [this] { std::string e; return openMidiOutput(outIndex_, &e); };
    inputFactory = [this] { std::string e; return openMidiInput(inIndex_, &e); };
    timer_ = new QTimer();
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->setInterval(2);
    QObject::connect(timer_, &QTimer::timeout, [this] { tick(); });
    midiTimeDisplayText() = [this](TimeDisplaySource src) { return displayText((int)src); };
}

// Preferences > Sync: the send frame rate (-1: the project's) and the one expected coming in.
int MidiSync::sendRateCode(double projectFps)
{
    const int c = vprefs::number("midi/mtcRate");
    return c >= 0 && c <= 3 ? c : rateCode(projectFps);
}
int MidiSync::chaseRateCode(double projectFps)
{
    const int c = vprefs::number("midi/chaseRate");
    return c >= 0 && c <= 3 ? c : rateCode(projectFps);
}

double MidiSync::timecodeLossMs()
{
    const double lossMs = vprefs::on("midi/freewheel") ? (vprefs::real("midi/freewheelSlack") + vprefs::real("midi/freewheelPlayback")) * 1000.0 : 250.0;
    return std::max(250.0, lossMs);
}

std::string MidiSync::displayText(int source) const
{
    const AppState& app = AppState::get();
    const double fps = app.project().settings.fps();
    const int code = source == (int)TimeDisplaySource::MtcIn ? chaseRateCode(fps) : sendRateCode(fps);
    auto tc = [](double t, int rc) {
        int h, m, s, f;
        timecodeOf(std::max(0.0, t), rc, h, m, s, f);
        return strfmt("%02d:%02d:%02d%c%02d", h, m, s, rc == 2 ? ';' : ':', f);
    };
    const double at = pb_ && pb_->playing() ? pb_->position() : app.cursor();
    switch ((TimeDisplaySource)source) {
    case TimeDisplaySource::MtcIn:
        if (!chaseOn_) return "--:--:--:--";
        if (chaseTime_ < 0 || QDateTime::currentMSecsSinceEpoch() - lastHeard_ > 500) return "Listening...";
        if (heardRate_ >= 0 && heardRate_ != code) return "Wrong format";
        return tc(chaseTime_, code);
    case TimeDisplaySource::MtcOut:
        return mtcOn_ ? tc(at, code) : std::string("--:--:--:--");
    case TimeDisplaySource::ClockOut: {
        if (!clockOn_) return "---.--.--";
        const double tempo = app.project().settings.tempo > 0 ? app.project().settings.tempo : 120.0;
        const int perMeasure = std::max(1, app.project().settings.beatsPerMeasure);
        const int64_t clocks = (int64_t)std::floor(std::max(0.0, at) * tempo / 60.0 * 24.0);
        const int64_t beats = clocks / 24;
        return strfmt("%lld.%lld.%02lld", (long long)(beats / perMeasure + 1), (long long)(beats % perMeasure + 1), (long long)(clocks % 24));
    }
    default: return std::string();
    }
}

MidiSync::~MidiSync()
{
    midiTimeDisplayText() = nullptr;
    if (out_ && wasPlaying_ && clockOn_) out_->send({0xFC});
    delete timer_;
    out_.reset();
    in_.reset();
}

void MidiSync::setOutputDevice(int index)
{
    outIndex_ = index;
    QSettings().setValue("midi/output", index);
    if (out_) {
        // opened again on the new one
        out_.reset();
        std::string e;
        if (mtcOn_ || clockOn_) openOut(&e);
    }
}

void MidiSync::setInputDevice(int index)
{
    inIndex_ = index;
    QSettings().setValue("midi/input", index);
    if (chaseOn_) {
        setChase(false);
        setChase(true);
    }
}

bool MidiSync::openOut(std::string* error)
{
    if (out_) return true;
    out_ = outputFactory ? outputFactory() : nullptr;
    if (!out_) {
        if (error && error->empty()) {
            std::string e;
            openMidiOutput(outIndex_, &e);
            *error = e.empty() ? "the MIDI output could not be opened" : e;
        }
        return false;
    }
    lastQuarter_ = lastClock_ = INT64_MIN;
    return true;
}

void MidiSync::closeOutIfUnused()
{
    if (!mtcOn_ && !clockOn_) out_.reset();
    if (!mtcOn_ && !clockOn_ && !chaseOn_) timer_->stop();
}

bool MidiSync::setGenerateTimecode(bool on, std::string* error)
{
    if (on == mtcOn_) return true;
    if (on && !openOut(error)) return false;
    mtcOn_ = on;
    lastQuarter_ = INT64_MIN;
    if (on) timer_->start();
    else closeOutIfUnused();
    if (onChanged) onChanged();
    return true;
}

bool MidiSync::setGenerateClock(bool on, std::string* error)
{
    if (on == clockOn_) return true;
    if (on && !openOut(error)) return false;
    if (!on && out_ && wasPlaying_) out_->send({0xFC});
    clockOn_ = on;
    lastClock_ = INT64_MIN;
    if (on) timer_->start();
    else closeOutIfUnused();
    if (onChanged) onChanged();
    return true;
}

bool MidiSync::setChase(bool on, std::string* error)
{
    if (on == chaseOn_) return true;
    if (on) {
        in_ = inputFactory ? inputFactory() : nullptr;
        if (!in_) {
            if (error) {
                std::string e;
                openMidiInput(inIndex_, &e);
                *error = e.empty() ? "the MIDI input could not be opened" : e;
            }
            return false;
        }
        in_->onReceive = [this](const std::vector<uint8_t>& m) { received(m); };
        piecesSeen_ = 0;
        nextPiece_ = 0;
        chaseTime_ = -1;
        timer_->start();
    } else {
        in_.reset();
    }
    chaseOn_ = on;
    if (!on) closeOutIfUnused();
    if (onChanged) onChanged();
    return true;
}

void MidiSync::tick()
{
    AppState& app = AppState::get();
    const bool playing = pb_ && pb_->playing() && pb_->rate() > 0;
    const double pos = playing ? pb_->position() : app.cursor();
    const Project& p = app.project();
    // ---- sending
    if (out_ && (mtcOn_ || clockOn_)) {
        const int code = sendRateCode(p.settings.fps());
        const double fps = rateOfCode(code);
        // Preferences > Sync > Advanced: when full-frame messages, Song Position Pointers and Start / Continue are sent
        const QString fullFrames = vprefs::text("midi/fullFrame"), songPositions = vprefs::text("midi/songPosition");
        const bool sendFull = fullFrames != "never", sendSong = songPositions != "never";
        auto songPosition = [&](double at) {
            const double beats = at * p.settings.tempo / 60.0;
            const int sixteenths = (int)std::min(16383.0, std::floor(beats * 4.0 + 1e-6));
            out_->send({0xF2, (uint8_t)(sixteenths & 0x7F), (uint8_t)((sixteenths >> 7) & 0x7F)});
            return sixteenths;
        };
        // stopped, cursor moved: "for all cursor position changes"
        if (!playing && !wasPlaying_ && std::fabs(pos - lastPos_) > 1e-9) {
            if (mtcOn_ && fullFrames == "all") out_->send(fullFrame(pos, code));
            if (clockOn_ && songPositions == "all") songPosition(pos);
        }
        if (playing && !wasPlaying_) {
            // playing starts: where (full frame / song position), then the clock starts
            if (mtcOn_ && sendFull) out_->send(fullFrame(pos, code));
            if (clockOn_) {
                const int sixteenths = sendSong ? songPosition(pos) : (int)std::min(16383.0, std::floor(pos * p.settings.tempo / 60.0 * 4.0 + 1e-6));
                out_->send({(uint8_t)(sixteenths == 0 || vprefs::on("midi/clockStart") ? 0xFA : 0xFB)});
            }
            lastQuarter_ = (int64_t)std::floor(pos * fps * 4.0) - 1;
            lastClock_ = (int64_t)std::floor(pos * p.settings.tempo / 60.0 * 24.0 + 1e-9) - 1;
        } else if (!playing && wasPlaying_) {
            if (clockOn_) out_->send({0xFC});
            if (mtcOn_ && sendFull) out_->send(fullFrame(pos, code));        // where it stopped
        }
        if (playing) {
            if (mtcOn_) {
                const int64_t q = (int64_t)std::floor(pos * fps * 4.0);
                if (q < lastQuarter_ || q - lastQuarter_ > 16) {
                    // a jump (loop, locate): a full frame says where we are
                    if (sendFull) out_->send(fullFrame(pos, code));
                    lastQuarter_ = q - 1;
                }
                for (int64_t k = lastQuarter_ + 1; k <= q; ++k) {
                    // the eight pieces carry the time of the frame where piece 0 was sent (every second frame)
                    const int64_t start = k - (((k % 8) + 8) % 8);
                    int h, m, s, f;
                    timecodeOf((double)start / (fps * 4.0) + 1e-9, code, h, m, s, f);
                    out_->send(quarterFrame((int)(((k % 8) + 8) % 8), h, m, s, f, code));
                }
                lastQuarter_ = std::max(lastQuarter_, q);
            }
            if (clockOn_ && p.settings.tempo > 0) {
                const int64_t c = (int64_t)std::floor(pos * p.settings.tempo / 60.0 * 24.0 + 1e-9);
                if (c < lastClock_ || c - lastClock_ > 96) {
                    // a jump: send the song position again
                    const int sixteenths = (int)std::max(0LL, (long long)(c / 6));
                    if (sendSong) out_->send({0xF2, (uint8_t)(sixteenths & 0x7F), (uint8_t)((sixteenths >> 7) & 0x7F)});
                    lastClock_ = c - 1;
                }
                for (int64_t k = lastClock_ + 1; k <= c; ++k) out_->send({0xF8});
                lastClock_ = std::max(lastClock_, c);
            }
        }
    }
    wasPlaying_ = playing;
    lastPos_ = pos;
    // ---- following: stop playback when timecode stops coming
    // (Preferences > Sync > Advanced, "Free-wheel for timecode loss": playback continues through the slack time and the
    // free-wheel time before stopping; off, it stops at once)
    if (chaseOn_ && chaseTime_ >= 0 && QDateTime::currentMSecsSinceEpoch() - lastHeard_ > timecodeLossMs()) {
        chaseTime_ = -1;
        piecesSeen_ = 0;
        if (pb_ && pb_->playing()) pb_->pause();
        app.status("MIDI timecode stopped.");
    }
}

void MidiSync::received(const std::vector<uint8_t>& msg)
{
    if (!chaseOn_ || msg.empty()) return;
    AppState& app = AppState::get();
    const int code = chaseRateCode(app.project().settings.fps());
    const bool explicitRate = vprefs::number("midi/chaseRate") >= 0;
    // a full frame: go there (and wait for quarter frames to play)
    if (msg.size() >= 10 && msg[0] == 0xF0 && msg[1] == 0x7F && msg[3] == 0x01 && msg[4] == 0x01) {
        const int rc = (msg[5] >> 5) & 3;
        heardRate_ = rc;
        if (explicitRate && rc != code) return;         // Trigger from MIDI Timecode > Frame rate: other rates are not followed
        const double t = secondsOf(msg[5] & 31, msg[6], msg[7], msg[8], rc);
        if (pb_ && pb_->playing()) pb_->pause();
        app.setCursor(t);
        piecesSeen_ = 0;
        nextPiece_ = 0;
        return;
    }
    if (msg.size() < 2 || msg[0] != 0xF1) return;
    const int piece = (msg[1] >> 4) & 7, v = msg[1] & 15;
    if (piece != nextPiece_) {
        // out of order (or first heard): start again at piece 0
        piecesSeen_ = 0;
        if (piece != 0) {
            nextPiece_ = 0;
            return;
        }
    }
    pieces_[piece] = v;
    nextPiece_ = (piece + 1) & 7;
    ++piecesSeen_;
    lastHeard_ = QDateTime::currentMSecsSinceEpoch();
    if (piece != 7 || piecesSeen_ < 8) return;
    // all eight: the time they carry, plus the two frames they took to send
    const int f = pieces_[0] | ((pieces_[1] & 1) << 4), s = pieces_[2] | ((pieces_[3] & 3) << 4), m = pieces_[4] | ((pieces_[5] & 3) << 4);
    const int h = pieces_[6] | ((pieces_[7] & 1) << 4), rc = (pieces_[7] >> 1) & 3;
    heardRate_ = rc;
    if (explicitRate && rc != code) {
        chaseTime_ = -1;
        return;
    }
    // Preferences > Sync > Advanced "Offset adjust" (quarter frames) moves the time followed
    const double t = std::max(0.0, secondsOf(h, m, s, f, rc) + 2.0 / rateOfCode(rc) + vprefs::number("midi/offsetQuarterFrames") / (4.0 * rateOfCode(rc)));
    chaseTime_ = t;
    if (!pb_) return;
    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    if (!pb_->playing()) {
        app.setCursor(t);
        pb_->playAt(1.0);
        startedMs_ = now;
        app.status(strfmt("Following MIDI timecode from %02d:%02d:%02d:%02d.", h, m, s, f));
    } else if (std::fabs(pb_->position() - t) > 0.1 && !vprefs::on("midi/triggerOnly") &&
               now - startedMs_ >= (int64_t)(std::max(0.0, vprefs::real("midi/syncDelay")) * 1000.0)) {
        // drifted, or timecode jumped: restart from where it says (not within the synchronization delay after the start,
        // nor when timecode only triggers playback)
        pb_->pause();
        app.setCursor(t);
        pb_->playAt(1.0);
        startedMs_ = now;
    }
}

}  // namespace app
}  // namespace reno
