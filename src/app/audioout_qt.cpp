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
#include <QThread>
#include <QTimer>
#include <cstring>
#include <mutex>

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
// With no mixer attached (between plays, while the device is kept open) it gives silence. The mixer is attached and
// taken away under a lock, so once setPull(nullptr) returns no more sound is asked of it.
class PullDevice : public QIODevice {
public:
    bool asFloat = true;
    int channels = 2;
    std::vector<float> ch[6];

    void setPull(AudioOutput::Pull p, AudioOutput::PullChannels pc)
    {
        std::lock_guard<std::mutex> lk(pullMutex_);
        pull_ = std::move(p);
        pullChannels_ = std::move(pc);
    }

    qint64 readData(char* data, qint64 maxSize) override
    {
        std::lock_guard<std::mutex> lk(pullMutex_);
        const int frameBytes = (asFloat ? 4 : 2) * channels;
        const int frames = (int)(maxSize / frameBytes);
        if (frames <= 0) return 0;
        if (!pull_ && !pullChannels_) {
            std::memset(data, 0, (size_t)frames * (size_t)frameBytes);
            return (qint64)frames * frameBytes;
        }
        return mix(data, frames);
    }
    qint64 writeData(const char*, qint64) override { return 0; }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }

private:
    qint64 mix(char* data, int frames)
    {
        const int nch = channels == 6 && pullChannels_ ? 6 : 2;
        const int frameBytes = (asFloat ? 4 : 2) * nch;
        AudioOutput::Pull& pull = pull_;
        AudioOutput::PullChannels& pullChannels = pullChannels_;
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
    std::mutex pullMutex_;
    AudioOutput::Pull pull_;
    AudioOutput::PullChannels pullChannels_;
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

// The sound card is kept open for a while after playing stops, giving silence: opening a new QAudioSink takes a few
// hundred milliseconds on Windows, and that was heard as a wait before every play and every click on the timeline
// while playing. Playing again soon on the same card in the same format takes the open one.
class QtOutput : public AudioOutput {
public:
    QtOutput()
    {
        idle_.setSingleShot(true);
        QObject::connect(&idle_, &QTimer::timeout, [this] { close(); });
    }
    ~QtOutput() override { close(); }
    bool start(int sampleRate, Pull pull) override
    {
        QAudioDevice dev = chosenOutput(AppState::get().prefs.audioDevice);
        if (dev.isNull()) return false;
        if (!openFor(dev, sampleRate, 2)) return false;
        device_.setPull(std::move(pull), nullptr);
        return true;
    }
    int maxChannels() const override
    {
        const QAudioDevice dev = chosenOutput(AppState::get().prefs.audioDevice);
        return dev.isNull() ? 2 : std::max(2, dev.maximumChannelCount());
    }
    bool startChannels(int sampleRate, int channels, PullChannels pull) override
    {
        if (channels != 6) return false;
        QAudioDevice dev = chosenOutput(AppState::get().prefs.audioDevice);
        if (dev.isNull() || dev.maximumChannelCount() < 6) return false;
        if (!openFor(dev, sampleRate, 6)) return false;
        device_.setPull(nullptr, std::move(pull));
        return true;
    }
    void stop() override
    {
        // no more sound is asked of the mixer from here on; the card goes on with silence until it is closed
        device_.setPull(nullptr, nullptr);
        if (!sink_) return;
        if (QThread::currentThread() == idle_.thread()) idle_.start(kKeepOpenMs);
        else close();
    }
    double latencySeconds() const override
    {
        // handed over but not yet played: about a buffer
        return sink_ ? (double)sink_->bufferSize() / frameBytes_ / rate_ : 0.0;
    }
    std::string name() const override { return name_; }

private:
    static const int kKeepOpenMs = 30000;

    // The open sink if it is the same card in the same format and still running, else a new one.
    bool openFor(const QAudioDevice& dev, int sampleRate, int channels)
    {
        idle_.stop();
        const double seconds = bufferSeconds();
        if (sink_ && dev.id() == deviceId_ && sampleRate == rate_ && channels == device_.channels && seconds == bufferSeconds_ &&
            sink_->error() == QAudio::NoError && sink_->state() != QAudio::StoppedState)
            return true;
        close();
        return open(dev, sampleRate, channels);
    }

    // Preferences > Audio Device, Playback buffering (50 ms until set)
    static double bufferSeconds() { return std::max(0.02, std::min(1.0, vprefs::real("prefs/playbackBuffer"))); }

    void close()
    {
        idle_.stop();
        device_.setPull(nullptr, nullptr);
        if (sink_) {
            sink_->stop();
            delete sink_;
            sink_ = nullptr;
        }
        if (device_.isOpen()) device_.close();
        deviceId_.clear();
    }

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
        bufferSeconds_ = bufferSeconds();
        sink_->setBufferSize((int)(sampleRate * bufferSeconds_) * frameBytes_);
        sink_->start(&device_);
        if (sink_->error() != QAudio::NoError) {
            close();
            return false;
        }
        deviceId_ = dev.id();
        return true;
    }
    PullDevice device_;
    QTimer idle_;                   // closes the card when it has been idle a while
    QByteArray deviceId_;           // the card the open sink plays to
    double bufferSeconds_ = 0;
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
QAudioDevice chosenInput(const std::string& id)
{
    QAudioDevice dev = QMediaDevices::defaultAudioInput();
    // an empty id is the input chosen in Preferences, else the system's
    const QByteArray want = QByteArray::fromStdString(id.empty() ? AppState::get().prefs.audioInputDevice : id);
    if (!want.isEmpty())
        for (const QAudioDevice& d : QMediaDevices::audioInputs())
            if (d.id() == want) { dev = d; break; }
    return dev;
}

// Records from a sound card input (the one named, else the one chosen in Preferences, else the system default) through a
// QAudioSource, as many channels as asked for (the card's own number if it has fewer), handed on as interleaved floats.
class QtInput : public AudioInput {
public:
    ~QtInput() override { stop(); }
    bool start(int sampleRate, Push push, std::string* error) override
    {
        // stereo from the input chosen in Preferences, as two float sides
        return startDevice(std::string(), 2, sampleRate, [push, this](const float* x, int channels, int frames) {
            l_.resize((size_t)frames);
            r_.resize((size_t)frames);
            for (int k = 0; k < frames; ++k) {
                l_[(size_t)k] = x[(size_t)k * channels];
                r_[(size_t)k] = channels > 1 ? x[(size_t)k * channels + 1] : l_[(size_t)k];
            }
            push(l_.data(), r_.data(), frames);
        }, error);
    }
    bool startDevice(const std::string& id, int channels, int sampleRate, PushN push, std::string* error) override
    {
        stop();
        QAudioDevice dev = chosenInput(id);
        if (dev.isNull()) {
            if (error) *error = "there is no sound input device";
            return false;
        }
        int want = std::max(1, std::min(channels, std::max(1, dev.maximumChannelCount())));
        QAudioFormat f;
        bool ok = false;
        for (; want >= 1 && !ok; --want) {
            f.setSampleRate(sampleRate);
            f.setChannelCount(want);
            f.setSampleFormat(QAudioFormat::Float);
            ok = dev.isFormatSupported(f);
            if (!ok) {
                f.setSampleFormat(QAudioFormat::Int16);
                ok = dev.isFormatSupported(f);
            }
            if (ok) break;
        }
        if (!ok) {
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
    int channelCount() const override { return format_.channelCount(); }

private:
    void read()
    {
        if (!io_) return;
        const QByteArray data = io_->readAll();
        const int bpf = format_.bytesPerFrame();
        if (bpf <= 0) return;
        const int frames = (int)(data.size() / bpf);
        if (frames <= 0) return;
        const int ch = format_.channelCount();
        x_.resize((size_t)frames * (size_t)ch);
        for (int k = 0; k < frames * ch; ++k) {
            const char* p = data.constData() + (size_t)k * (bpf / ch);
            x_[(size_t)k] = format_.sampleFormat() == QAudioFormat::Float ? reinterpret_cast<const float*>(p)[0] : reinterpret_cast<const qint16*>(p)[0] / 32768.f;
        }
        if (push_) push_(x_.data(), ch, frames);
    }
    QAudioFormat format_;
    PushN push_;
    QAudioSource* source_ = nullptr;
    QIODevice* io_ = nullptr;
    std::string name_;
    std::vector<float> x_, l_, r_;
};
}  // namespace

std::unique_ptr<AudioInput> createAudioInput() { return std::unique_ptr<AudioInput>(new QtInput()); }

std::vector<std::pair<std::string, std::string>> audioInputDevices()
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const QAudioDevice& d : QMediaDevices::audioInputs()) out.push_back(std::make_pair(d.id().toStdString(), d.description().toStdString()));
    return out;
}

int audioInputChannels(const std::string& id)
{
    const QAudioDevice dev = chosenInput(id);
    return dev.isNull() ? 0 : dev.maximumChannelCount();
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
int audioInputChannels(const std::string&) { return 0; }
}
}
#endif
