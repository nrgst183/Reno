// VEGAS's Text to Speech (Ctrl+Shift+P): type text, pick a voice, its speed and loudness; Create puts a WAV made by
// Windows' SAPI 5 on the timeline at the cursor, saved beside the project in "Text to Speech" (or the recording
// folder), added to Project Media and put on the selected audio track (else the first free one, else a new one) as
// one undo step. Windows only: elsewhere the window says so (tests use a stand-in speaker in 'synthesizer').
#pragma once
#include "appstate.h"
#include <QWidget>
#include <memory>

namespace reno {
namespace app {

std::vector<std::pair<std::string, std::string>> speechVoices();
// Speaks 'text' into a WAV (48 kHz, 16-bit mono) with the voice of that id ("" = default), 'rate' -10..10,
// 'volume' 0..100. False with *error on failure.
bool speakToFile(const std::string& text, const std::string& voiceId, int rate, int volume, const std::string& path, std::string* error);

class TextToSpeechWindow : public QWidget {
public:
    explicit TextToSpeechWindow(QWidget* parent = nullptr);
    ~TextToSpeechWindow() override;

    void setText(const QString& text);
    void setRate(int rate);
    void setVolume(int volume);
    std::vector<uint64_t> create();
    std::string lastFile() const;

    std::function<bool(const std::string& text, const std::string& voiceId, int rate, int volume, const std::string& path, std::string* error)> synthesizer;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

}  // namespace app
}  // namespace reno
