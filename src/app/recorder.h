// Recording sound onto the timeline (VEGAS's Record, Ctrl+R, and Arm for Record, Ctrl+Alt+R).
//
// Audio tracks are armed for recording (AppState::armedTracks: the round header button, Ctrl+Alt+R on the selected track).
#pragma once
#include "appstate.h"
#include "../engine/recording.h"
#include <QString>
#include <functional>
class QMenu;
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>

namespace reno {
namespace app {

class Playback;

// Where recorded sound comes from. push(left, right, frames) is called with the input, on the thread that started
// it or the device's own.
class AudioInput {
public:
    virtual ~AudioInput() {}
    typedef std::function<void(const float* left, const float* right, int frames)> Push;
    // interleaved sound of 'channels' channels
    typedef std::function<void(const float* interleaved, int channels, int frames)> PushN;
    // Stereo from the input chosen in Preferences.
    virtual bool start(int sampleRate, Push push, std::string* error) = 0;
    // The input 'deviceId' (empty: the one chosen in Preferences) with at least 'channels' channels where it has them.
    // The default is start(): stereo from whatever it takes.
    virtual bool startDevice(const std::string& deviceId, int channels, int sampleRate, PushN push, std::string* error)
    {
        (void)deviceId;
        (void)channels;
        return start(sampleRate, [push](const float* l, const float* r, int n) {
            std::vector<float> x((size_t)n * 2);
            for (int k = 0; k < n; ++k) {
                x[(size_t)k * 2] = l[k];
                x[(size_t)k * 2 + 1] = r ? r[k] : l[k];
            }
            push(x.data(), 2, n);
        }, error);
    }
    virtual void stop() = 0;
    virtual std::string name() const = 0;
    virtual int channelCount() const { return 2; }
};
std::unique_ptr<AudioInput> createAudioInput();
std::vector<std::pair<std::string, std::string>> audioInputDevices();
// How many channels a sound card input has (empty id: the one chosen in Preferences); 0 if there is none.
int audioInputChannels(const std::string& deviceId);

// The sound card inputs there are (id, name, channels). Tests give their own list with setInputDevicesForTest.
struct InputDeviceInfo {
    std::string id, name;
    int channels = 2;
};
std::vector<InputDeviceInfo> inputDevices();
void setInputDevicesForTest(const std::vector<InputDeviceInfo>* list);     // null: the real ones

// VEGAS's Record Input menu of a track (or an input bus, with withTrackItems false): the sound card input (each device
// with Mono and Stereo, and the channel or pair), for a track also the input busses and Input Monitoring (Off, On, Auto).
// 'set' gets the new choice. Actions are named src:<device>:<first>:<channels>, inputBus:<id>, monitor:<mode>.
void fillRecordInputMenu(QMenu* m, const RecordInput& cur, bool withTrackItems, const std::function<void(const RecordInput&)>& set);
// What the choice is called on a button.
QString recordInputText(const Project& p, const RecordInput& r);

// The sound card inputs that are open, shared by the Recorder (tracks armed for recording) and the playback of input
// busses. Each input's sound goes to listeners as it arrives and is kept for a second for readers that take it in
// blocks (the mix of an input bus).
class InputHub {
public:
    typedef std::function<void(const std::string& device, const float* interleaved, int channels, int frames)> Listener;
    std::function<std::unique_ptr<AudioInput>()> factory;      // empty: createAudioInput
    // Opens the inputs wanted (device id, the channels it must give at least) and closes the others. False with *error if one cannot be opened (the others stay).
    bool open(const std::vector<std::pair<std::string, int>>& wanted, int sampleRate, std::string* error);
    void closeAll();
    bool isOpen() const;
    bool isOpen(const std::string& device) const;
    std::string name(const std::string& device) const;
    int addListener(Listener l);
    void removeListener(int id);
    // The next 'frames' of the channels first.. (mono: left only) of a device for this reader, if that much has come in since
    // the reader's last read; else false and nothing is taken.
    bool read(uint64_t reader, const std::string& device, int first, int channels, float* left, float* right, int frames);
    // For tests and for input that does not come from a card: sound as a device would give it.
    void push(const std::string& device, const float* interleaved, int channels, int frames);

private:
    struct Dev {
        std::unique_ptr<AudioInput> in;
        int channels = 2;
        std::vector<float> ring;        // interleaved, capacity frames
        int64_t capacity = 0;
        int64_t written = 0;
    };
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<Dev>> devs_;
    std::map<std::pair<uint64_t, std::string>, int64_t> pos_;
    std::map<int, Listener> listeners_;
    int nextListener_ = 1;
    int rate_ = 48000;
    void incoming(const std::string& device, const float* x, int channels, int frames);
};
InputHub& inputHub();

// Writes a WAV file as it is recorded: 16- or 24-bit PCM, or 32-bit float. The header sizes are fixed up in finish().
class WavWriter {
public:
    ~WavWriter();
    bool open(const std::string& path, int sampleRate, int channels, int bits, std::string* error);
    bool write(const float* left, const float* right, int frames);
    bool finish();
    int64_t frames() const { return frames_; }
    bool isOpen() const { return f_ != nullptr; }

private:
    std::FILE* f_ = nullptr;
    int rate_ = 48000, channels_ = 2, bits_ = 16;
    int64_t frames_ = 0;
    bool failed_ = false;
    std::vector<char> buf_;
};

class Recorder {
public:
    explicit Recorder(Playback* playback);
    ~Recorder();

    // Starts recording into the armed tracks (if none is armed, arms the selected audio track, else a new one). Each track
    // records from its own Record Input (Track::recordInput), tracks with the same input share a file. Recording
    // into selected events of the armed tracks makes takes of them (punch-in within a time selection, a take per
    // pass while Loop Playback is on) instead of new events.
    // False with *error if the input can't be opened or the file can't be made.
    bool start(std::string* error);
    // Stops: playback stops, the file is finished and put on the armed tracks (one undo step). Returns the new events.
    std::vector<uint64_t> stop();
    bool recording() const { return recording_; }
    double recordedSeconds() const;

    void setArmed(uint64_t trackId, bool armed);
    bool armed(uint64_t trackId) const;
    void armSelected();
    void syncArming();

    // The input's level: the loudest sample of each side since the last call (0..1, falling back gently).
    void inputLevel(float& left, float& right);
    bool inputOpen() const { return open_ && inputHub().isOpen(); }
    std::string inputName() const;

    std::string recordFolder() const;
    std::string lastFile() const { return lastFile_; }

    std::function<std::unique_ptr<AudioInput>()> inputFactory;
    std::function<void()> onChanged;
    // The files of the last recording (one per input), and the events and takes it made on the last stop().
    const std::vector<std::string>& lastFiles() const { return lastFiles_; }
    // Sound for armed tracks that monitor their input (Record Input > Input Monitoring) and for input busses: the
    // Recorder installs these on the Playback it was made with.
    // Tests: input sound as a card would give it (the same as the hub's push).
    void feedInput(const std::string& device, const float* interleaved, int channels, int frames) { inputHub().push(device, interleaved, channels, frames); }

private:
    std::vector<std::pair<std::string, int>> wantedInputs() const;
    bool monitorSound(const Track& tr, int64_t start, float* l, float* r, int n);
    void deviceSound(const std::string& device, const float* x, int channels, int frames);
    void inputBusSound(uint64_t id, const float* l, const float* r, int frames);
    void sink(int source, const float* l, const float* r, int frames);
    bool openInputs(std::string* error);
    void closeInputs();
    Playback* pb_;
    bool open_ = false;
    bool busesOpen_ = false;            // the project plays with input busses: their inputs are open
    std::vector<std::pair<std::string, int>> lastDevices_;
    bool recording_ = false;
    int rate_ = 48000;
    double startTime_ = 0;
    double stopAt_ = 0;                 // recording ends by itself when the cursor gets here (0: when stopped)
    std::vector<uint64_t> tracks_;
    std::vector<RecordSource> sources_;
    std::vector<std::vector<uint64_t>> targets_;       // per source: the events (of its tracks) it records into
    std::vector<std::unique_ptr<WavWriter>> writers_;
    std::vector<std::string> files_;
    std::string lastFile_;
    std::vector<std::string> lastFiles_;
    RecordRouter router_;
    mutable std::mutex mutex_;
    float peakL_ = 0, peakR_ = 0;
    int listener_ = 0, hubListener_ = 0;
    bool stopping_ = false;
    class QTimer* timer_ = nullptr;
};

}  // namespace app
}  // namespace reno
