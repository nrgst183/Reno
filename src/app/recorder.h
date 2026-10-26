// Recording sound onto the timeline (VEGAS's Record, Ctrl+R, and Arm for Record, Ctrl+Alt+R).
//
// Audio tracks are armed for recording (AppState::armedTracks: the round header button, Ctrl+Alt+R on the selected track).
#pragma once
#include "appstate.h"
#include <QString>
#include <cstdio>
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
    virtual bool start(int sampleRate, Push push, std::string* error) = 0;
    virtual void stop() = 0;
    virtual std::string name() const = 0;
};
std::unique_ptr<AudioInput> createAudioInput();
std::vector<std::pair<std::string, std::string>> audioInputDevices();

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

    // Starts recording into the armed tracks (if none is armed, arms the selected audio track, else a new one).
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
    bool inputOpen() const { return input_ != nullptr; }
    std::string inputName() const;

    std::string recordFolder() const;
    std::string lastFile() const { return lastFile_; }

    std::function<std::unique_ptr<AudioInput>()> inputFactory;
    std::function<void()> onChanged;

private:
    void push(const float* l, const float* r, int frames);
    bool openInput(std::string* error);
    void closeInput();
    Playback* pb_;
    std::unique_ptr<AudioInput> input_;
    bool recording_ = false;
    int rate_ = 48000;
    double startTime_ = 0;
    std::vector<uint64_t> tracks_;
    std::string file_, lastFile_;
    mutable std::mutex mutex_;
    WavWriter writer_;
    float peakL_ = 0, peakR_ = 0;
    int listener_ = 0;
    bool stopping_ = false;
};

}  // namespace app
}  // namespace reno
