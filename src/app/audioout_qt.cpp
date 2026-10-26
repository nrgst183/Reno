// Sound output and recording through Qt Multimedia (built only with RENO_HAVE_QTMULTIMEDIA).
#include "playback.h"
#include "recorder.h"
#include "vprefs.h"
#include <atomic>
#ifdef RENO_HAVE_QTMULTIMEDIA
#include <QAudioSink>
#include <QAudioSource>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QAudioDevice>
#include <QIODevice>

namespace reno {
namespace app {

namespace {
std::atomic<float>& outputGainRef()
{
    static std::atomic<float> g{1.0f};
    return g;
}
}  // namespace
void setOutputGain(float gain) { outputGainRef().store(gain < 0 ? 0.f : gain > 4.f ? 4.f : gain); }
float outputGain() { return outputGainRef().load(); }

namespace {
// Sink source: pulls interleaved sound from the mixer, stereo (pull) or multichannel (pullChannels).
class PullDevice : public QIODevice {
public:
    AudioOutput::Pull pull;
    AudioOutput::PullChannels pullChannels;
    bool asFloat = true;
    int channels = 2;
    std::vector<float> ch[6];

    qint64 readData(char* data, qint64 maxSize) override
    {
        const int nch = channels == 6 && pullChannels ? 6 : 2;
        const int frameBytes = (asFloat ? 4 : 2) * nch;
        const int frames = (int)(maxSize / frameBytes);
        if (frames <= 0) return 0;
        for (int c = 0; c < nch; ++c)
            if ((int)ch[c].size() < frames) ch[c].resize((size_t)frames);
        if (nch == 6) {
            float* out[6] = {ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data(), ch[4].data(), ch[5].data()};
            pullChannels(out, 6, frames);
        } else {
            pull(ch[0].data(), ch[1].data(), frames);
        }
        // scrub bursts can be quieter (Preferences > Reno > Editing, Scrub audio volume)
        const float gain = outputGainRef().load();
        if (gain != 1.0f)
            for (int c = 0; c < nch; ++c)
                for (int k = 0; k < frames; ++k) ch[c][(size_t)k] *= gain;
        if (asFloat) {
            // Clip at full scale like VEGAS: Windows' shared mixer would otherwise limit float sound above it and a loud
            // mix would sound held back.
            float* out = (float*)data;
            for (int k = 0; k < frames; ++k)
                for (int c = 0; c < nch; ++c) {
                    const float a = ch[c][(size_t)k];
                    out[k * nch + c] = a < -1.f ? -1.f : (a > 1.f ? 1.f : a);
                }
        } else {
            qint16* out = (qint16*)data;
            for (int k = 0; k < frames; ++k)
                for (int c = 0; c < nch; ++c) {
                    float a = ch[c][(size_t)k];
                    a = a < -1.f ? -1.f : (a > 1.f ? 1.f : a);
                    out[k * nch + c] = (qint16)(a * 32767.f);
                }
        }
        return (qint64)frames * frameBytes;
    }
    qint64 writeData(const char*, qint64) override { return 0; }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }
};

// The sound card chosen in Preferences, else the system default.
QAudioDevice chosenOutput(const std::string& id)
{
    QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    const QByteArray want = QByteArray::fromStdString(id);
    if (!want.isEmpty())
        for (const QAudioDevice& d : QMediaDevices::audioOutputs())
            if (d.id() == want) { dev = d; break; }
    return dev;
}

class QtOutput : public AudioOutput {
public:
    ~QtOutput() override { stop(); }
    bool start(int sampleRate, Pull pull) override
    {
        stop();
        QAudioDevice dev = chosenOutput(AppState::get().prefs.audioDevice);
        if (dev.isNull()) return false;
        device_.pull = std::move(pull);
        device_.pullChannels = nullptr;
        return open(dev, sampleRate, 2);
    }
    int maxChannels() const override
    {
        const QAudioDevice dev = chosenOutput(AppState::get().prefs.audioDevice);
        return dev.isNull() ? 2 : std::max(2, dev.maximumChannelCount());
    }
    bool startChannels(int sampleRate, int channels, PullChannels pull) override
    {
        stop();
        if (channels != 6) return false;
        QAudioDevice dev = chosenOutput(AppState::get().prefs.audioDevice);
        if (dev.isNull() || dev.maximumChannelCount() < 6) return false;
        device_.pull = nullptr;
        device_.pullChannels = std::move(pull);
        return open(dev, sampleRate, 6);
    }
    void stop() override
    {
        if (sink_) {
            sink_->stop();
            delete sink_;
            sink_ = nullptr;
        }
        if (device_.isOpen()) device_.close();
    }
    double latencySeconds() const override
    {
        // handed over but not yet played: about a buffer
        return sink_ ? (double)sink_->bufferSize() / frameBytes_ / rate_ : 0.0;
    }
    std::string name() const override { return name_; }

private:
    bool open(const QAudioDevice& dev, int sampleRate, int channels)
    {
        QAudioFormat f;
        f.setSampleRate(sampleRate);
        f.setChannelCount(channels);
        // six channels: Qt's 5.1 order (FL, FR, C, LFE, BL, BR), same as the mixer's
        if (channels == 6) f.setChannelConfig(QAudioFormat::ChannelConfigSurround5Dot1);
        f.setSampleFormat(QAudioFormat::Float);
        device_.asFloat = true;
        if (!dev.isFormatSupported(f)) {
            f.setSampleFormat(QAudioFormat::Int16);
            device_.asFloat = false;
            if (!dev.isFormatSupported(f)) return false;
        }
        device_.channels = channels;
        name_ = dev.description().toStdString();
        device_.open(QIODevice::ReadOnly);
        sink_ = new QAudioSink(dev, f);
        frameBytes_ = (device_.asFloat ? 4 : 2) * channels;
        rate_ = sampleRate;
        // Preferences > Audio Device, Playback buffering (50 ms until set)
        const double seconds = std::max(0.02, std::min(1.0, vprefs::real("prefs/playbackBuffer")));
        sink_->setBufferSize((int)(sampleRate * seconds) * frameBytes_);
        sink_->start(&device_);
        if (sink_->error() != QAudio::NoError) { stop(); return false; }
        return true;
    }
    PullDevice device_;
    QAudioSink* sink_ = nullptr;
    int frameBytes_ = 8, rate_ = 48000;
    std::string name_ = "default sound device";
};
}  // namespace

int audioOutputChannels(const std::string& deviceId)
{
    const QAudioDevice dev = chosenOutput(deviceId);
    return dev.isNull() ? 0 : dev.maximumChannelCount();
}

std::unique_ptr<AudioOutput> createAudioOutput() { return std::unique_ptr<AudioOutput>(new QtOutput()); }

// ---- sound in (recording)
namespace {
// Records from the input chosen in Preferences (else the system default) through a QAudioSource, read on the thread that
// started it and handed on as two float sides.
class QtInput : public AudioInput {
public:
    ~QtInput() override { stop(); }
    bool start(int sampleRate, Push push, std::string* error) override
    {
        stop();
        QAudioDevice dev = QMediaDevices::defaultAudioInput();
        const QByteArray want = QByteArray::fromStdString(AppState::get().prefs.audioInputDevice);
        if (!want.isEmpty())
            for (const QAudioDevice& d : QMediaDevices::audioInputs())
                if (d.id() == want) { dev = d; break; }
        if (dev.isNull()) {
            if (error) *error = "there is no sound input device";
            return false;
        }
        QAudioFormat f;
        f.setSampleRate(sampleRate);
        f.setChannelCount(2);
        f.setSampleFormat(QAudioFormat::Float);
        if (!dev.isFormatSupported(f)) f.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(f)) {
            f.setChannelCount(1);
            f.setSampleFormat(QAudioFormat::Float);
            if (!dev.isFormatSupported(f)) f.setSampleFormat(QAudioFormat::Int16);
        }
        if (!dev.isFormatSupported(f)) {
            if (error) *error = dev.description().toStdString() + " cannot record at " + std::to_string(sampleRate) + " Hz";
            return false;
        }
        format_ = f;
        push_ = std::move(push);
        name_ = dev.description().toStdString();
        source_ = new QAudioSource(dev, f);
        source_->setBufferSize(sampleRate / 10 * f.bytesPerFrame());
        io_ = source_->start();
        if (!io_ || source_->error() != QAudio::NoError) {
            if (error) *error = name_ + " could not be started";
            stop();
            return false;
        }
        QObject::connect(io_, &QIODevice::readyRead, io_, [this] { read(); });
        return true;
    }
    void stop() override
    {
        if (source_) {
            source_->stop();
            delete source_;
            source_ = nullptr;
        }
        io_ = nullptr;
    }
    std::string name() const override { return name_; }

private:
    void read()
    {
        if (!io_) return;
        const QByteArray data = io_->readAll();
        const int bpf = format_.bytesPerFrame();
        if (bpf <= 0) return;
        const int frames = (int)(data.size() / bpf);
        if (frames <= 0) return;
        l_.resize((size_t)frames);
        r_.resize((size_t)frames);
        const int ch = format_.channelCount();
        for (int k = 0; k < frames; ++k) {
            const char* p = data.constData() + (size_t)k * bpf;
            float a, b;
            if (format_.sampleFormat() == QAudioFormat::Float) {
                a = reinterpret_cast<const float*>(p)[0];
                b = ch > 1 ? reinterpret_cast<const float*>(p)[1] : a;
            } else {
                a = reinterpret_cast<const qint16*>(p)[0] / 32768.f;
                b = ch > 1 ? reinterpret_cast<const qint16*>(p)[1] / 32768.f : a;
            }
            l_[(size_t)k] = a;
            r_[(size_t)k] = b;
        }
        if (push_) push_(l_.data(), r_.data(), frames);
    }
    QAudioFormat format_;
    Push push_;
    QAudioSource* source_ = nullptr;
    QIODevice* io_ = nullptr;
    std::string name_;
    std::vector<float> l_, r_;
};
}  // namespace

std::unique_ptr<AudioInput> createAudioInput() { return std::unique_ptr<AudioInput>(new QtInput()); }

std::vector<std::pair<std::string, std::string>> audioInputDevices()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) out.push_back(std::make_pair(d.id().toStdString(), d.description().toStdString()));
    return out;
}

}  // namespace app
}  // namespace reno
#else
namespace reno {
namespace app {
std::unique_ptr<AudioOutput> createAudioOutput() { return createSilentOutput(); }
int audioOutputChannels(const std::string&) { return 0; }
static float gGainNoSound = 1.0f;
void setOutputGain(float gain) { gGainNoSound = gain; }
float outputGain() { return gGainNoSound; }
std::unique_ptr<AudioInput> createAudioInput() { return nullptr; }
std::vector<std::pair<std::string, std::string>> audioInputDevices() { return {}; }
}
}
#endif
