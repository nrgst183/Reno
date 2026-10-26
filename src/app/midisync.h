// MIDI synchronisation (Options > Timecode): while playing, Reno can send MIDI Timecode (F7: quarter frames plus a full-frame message
// when playing starts or jumps) and MIDI Clock (Shift+F7: 24 clocks a beat at the project tempo, with Start / Continue,
// Song Position, Stop),
// and can chase incoming MIDI Timecode (Ctrl+F7, "Trigger from MIDI Timecode"): play from the timecode, keep to it, stop
// when it stops.
// Ports are Windows' (winmm), chosen in Options > Timecode; elsewhere there are none (tests supply made-up ports).
#pragma once
#include "appstate.h"
#include <cstdint>
#include <memory>

class QTimer;

namespace reno {
namespace app {

class Playback;

// A MIDI port: messages out, or in (onReceive, called on the interface thread).
class MidiPort {
public:
    virtual ~MidiPort() {}
    virtual bool send(const std::vector<uint8_t>& message) = 0;     // an input port sends nothing
    std::function<void(const std::vector<uint8_t>& message)> onReceive;
    virtual std::string name() const = 0;
};
std::vector<std::string> midiOutputNames();
std::vector<std::string> midiInputNames();
// The ports Preferences > MIDI makes available: Windows' numbers (as taken by openMidiOutput / openMidiInput) of the
// ports in 'names' that are not cleared there.
std::vector<int> availableMidiPorts(const std::vector<std::string>& names, bool output);
std::unique_ptr<MidiPort> openMidiOutput(int index, std::string* error);
std::unique_ptr<MidiPort> openMidiInput(int index, std::string* error);

class MidiSync {
public:
    explicit MidiSync(Playback* playback);
    ~MidiSync();

    bool setGenerateTimecode(bool on, std::string* error = nullptr);     // F7
    bool generatingTimecode() const { return mtcOn_; }
    bool setGenerateClock(bool on, std::string* error = nullptr);        // Shift+F7
    bool generatingClock() const { return clockOn_; }
    bool setChase(bool on, std::string* error = nullptr);                // Ctrl+F7
    bool chasing() const { return chaseOn_; }

    // Which ports (Windows' numbering; kept between runs).
    int outputDevice() const { return outIndex_; }
    int inputDevice() const { return inIndex_; }
    void setOutputDevice(int index);
    void setInputDevice(int index);

    // For tests: what opens the ports (default openMidiOutput / openMidiInput with the chosen devices).
    std::function<std::unique_ptr<MidiPort>()> outputFactory, inputFactory;
    std::function<void()> onChanged;
    // Per-tick work (called by its timer; tests may call it).
    void tick();
    // Time display text for a MIDI source (timesource.h): incoming timecode ("Listening..." while none, "Wrong format"
    // at a different frame rate),
    // outgoing timecode, outgoing MIDI clock (measure.beat.clock).
    std::string displayText(int source) const;

    // MTC pieces. Frame-rate code for a project rate (0 24, 1 25, 2 29.97 drop-frame, 3 30).
    // The code Preferences > Sync gives for the send rate, and the rate expected coming in (the project's if "Use
    // project frame rate").
    static int sendRateCode(double projectFps);
    // Milliseconds timecode may stay away before following stops (Preferences > Sync > Advanced, free-wheel for timecode loss).
    static double timecodeLossMs();
    static int chaseRateCode(double projectFps);
    static int rateCode(double fps);
    static double rateOfCode(int code);
    // Timecode of a time (whole frames at the MTC rate), and back.
    static void timecodeOf(double seconds, int code, int& h, int& m, int& s, int& f);
    static double secondsOf(int h, int m, int s, int f, int code);
    static std::vector<uint8_t> fullFrame(double seconds, int code);
    static std::vector<uint8_t> quarterFrame(int piece, int h, int m, int s, int f, int code);

private:
    bool openOut(std::string* error);
    void closeOutIfUnused();
    void received(const std::vector<uint8_t>& msg);
    Playback* pb_;
    std::unique_ptr<MidiPort> out_, in_;
    QTimer* timer_ = nullptr;
    bool mtcOn_ = false, clockOn_ = false, chaseOn_ = false;
    int outIndex_ = 0, inIndex_ = 0;
    // sending
    bool wasPlaying_ = false;
    int64_t lastQuarter_ = INT64_MIN;       // last quarter frame sent (count from the timeline start)
    double lastPos_ = 0;
    int64_t lastClock_ = INT64_MIN;         // last clock sent (24 a beat, from the timeline start)
    // following
    int pieces_[8] = {0};
    int piecesSeen_ = 0, nextPiece_ = 0;
    double chaseTime_ = -1;                 // time the incoming timecode says (seconds), -1 none
    int heardRate_ = -1;                    // frame-rate code of the incoming timecode (-1 none yet)
    int64_t lastHeard_ = 0;                 // when last heard (ms)
    int64_t startedMs_ = 0;                 // when playing last began on incoming timecode (ms)
};

}  // namespace app
}  // namespace reno
