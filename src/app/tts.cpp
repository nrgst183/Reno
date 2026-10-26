// Text to Speech: see tts.h.
#include "tts.h"
#include "toolwin.h"
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QStandardPaths>
#include <QVBoxLayout>
#ifdef _WIN32
#include <windows.h>
#include <sapi.h>
#endif

namespace reno {
namespace app {

using namespace tw;

#ifdef _WIN32
namespace {
const GUID kWaveFormatExId = {0xC31ADBAE, 0x527F, 0x4ff5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};

struct ComScope {
    bool mine = false;
    ComScope()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        mine = hr == S_OK || hr == S_FALSE;
    }
    ~ComScope()
    {
        if (mine) CoUninitialize();
    }
};

template <class T> void release(T*& p)
{
    if (p) p->Release();
    p = nullptr;
}
}  // namespace

std::vector<std::pair<std::string, std::string>> speechVoices()
{
    std::vector<std::pair<std::string, std::string>> out;
    ComScope com;
    ISpObjectTokenCategory* cat = nullptr;
    IEnumSpObjectTokens* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(SpObjectTokenCategory), nullptr, CLSCTX_ALL, __uuidof(ISpObjectTokenCategory), (void**)&cat)) || !cat) return out;
    if (SUCCEEDED(cat->SetId(SPCAT_VOICES, FALSE)) && SUCCEEDED(cat->EnumTokens(nullptr, nullptr, &en)) && en) {
        for (;;) {
            ISpObjectToken* tok = nullptr;
            ULONG got = 0;
            if (en->Next(1, &tok, &got) != S_OK || !tok) break;
            WCHAR* id = nullptr;
            WCHAR* name = nullptr;
            if (SUCCEEDED(tok->GetId(&id)) && id) {
                std::string n;
                if (SUCCEEDED(tok->GetStringValue(nullptr, &name)) && name) n = narrow(name);
                out.push_back(std::make_pair(narrow(id), n.empty() ? narrow(id) : n));
            }
            if (id) CoTaskMemFree(id);
            if (name) CoTaskMemFree(name);
            tok->Release();
        }
    }
    release(en);
    release(cat);
    return out;
}

bool speakToFile(const std::string& text, const std::string& voiceId, int rate, int volume, const std::string& path, std::string* error)
{
    auto fail = [&](const char* what, HRESULT hr) {
        if (error) *error = strfmt("%s (0x%08lx)", what, (unsigned long)hr);
        return false;
    };
    ComScope com;
    ISpVoice* voice = nullptr;
    ISpStream* stream = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(SpVoice), nullptr, CLSCTX_ALL, __uuidof(ISpVoice), (void**)&voice);
    if (FAILED(hr) || !voice) return fail("Windows' speech is not available", hr);
    bool ok = false;
    do {
        if (!voiceId.empty()) {
            ISpObjectToken* tok = nullptr;
            if (SUCCEEDED(CoCreateInstance(__uuidof(SpObjectToken), nullptr, CLSCTX_ALL, __uuidof(ISpObjectToken), (void**)&tok)) && tok) {
                if (SUCCEEDED(tok->SetId(nullptr, widen(voiceId).c_str(), FALSE))) voice->SetVoice(tok);
                tok->Release();
            }
        }
        voice->SetRate(std::max(-10, std::min(10, rate)));
        voice->SetVolume((USHORT)std::max(0, std::min(100, volume)));
        hr = CoCreateInstance(__uuidof(SpStream), nullptr, CLSCTX_ALL, __uuidof(ISpStream), (void**)&stream);
        if (FAILED(hr) || !stream) { fail("the speech could not be given a file", hr); break; }
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = 1;
        wfx.nSamplesPerSec = 48000;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = (WORD)(wfx.nChannels * wfx.wBitsPerSample / 8);
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
        hr = stream->BindToFile(widen(path).c_str(), SPFM_CREATE_ALWAYS, &kWaveFormatExId, &wfx, 0);
        if (FAILED(hr)) { fail("the file for the speech could not be made", hr); break; }
        hr = voice->SetOutput(stream, TRUE);
        if (FAILED(hr)) { fail("the speech could not be sent to the file", hr); break; }
        hr = voice->Speak(widen(text).c_str(), SPF_DEFAULT | SPF_IS_NOT_XML, nullptr);
        if (FAILED(hr)) { fail("Windows could not speak the text", hr); break; }
        voice->WaitUntilDone(INFINITE);
        ok = true;
    } while (false);
    if (stream) stream->Close();
    release(stream);
    release(voice);
    return ok;
}
#else
std::vector<std::pair<std::string, std::string>> speechVoices() { return {}; }

bool speakToFile(const std::string&, const std::string&, int, int, const std::string&, std::string* error)
{
    if (error) *error = "Text to Speech uses Windows' own speech: it is not available on this system";
    return false;
}
#endif

struct TextToSpeechWindow::Impl {
    QPlainTextEdit* text = nullptr;
    QComboBox* voice = nullptr;
    QSlider* rate = nullptr;
    QSlider* volume = nullptr;
    QLabel* rateLbl = nullptr;
    QLabel* volumeLbl = nullptr;
    QLabel* note = nullptr;
    std::string lastFile;

    std::string folder() const
    {
        AppState& app = AppState::get();
        std::string dir;
        if (!app.prefs.recordFolder.empty()) dir = pathJoin(app.prefs.recordFolder, "Text to Speech");
        else if (!app.path().empty()) dir = pathJoin(pathDir(app.path()), "Text to Speech");
        else {
            QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            if (docs.isEmpty()) docs = QDir::homePath();
            dir = ss(QDir(docs).filePath("Reno Text to Speech"));
        }
        QDir().mkpath(qs(dir));
        return dir;
    }
};

TextToSpeechWindow::TextToSpeechWindow(QWidget* parent) : QWidget(parent), d(new Impl())
{
    setObjectName("textToSpeech");
    synthesizer = speakToFile;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(6, 6, 6, 6);
    d->text = new QPlainTextEdit(this);
    d->text->setObjectName("ttsText");
    d->text->setPlaceholderText("Type what is to be spoken");
    v->addWidget(d->text, 1);
    auto* form = new QFormLayout();
    d->voice = new QComboBox(this);
    d->voice->setObjectName("ttsVoice");
    d->voice->addItem("Windows' default voice", QString());
    for (const auto& vc : speechVoices()) d->voice->addItem(qs(vc.second), qs(vc.first));
    const int keep = d->voice->findData(QSettings().value("tts/voice").toString());
    if (keep >= 0) d->voice->setCurrentIndex(keep);
    form->addRow("Voice:", d->voice);
    auto slider = [&](const char* name, int lo, int hi, int value, QLabel*& lbl) {
        auto* row = new QHBoxLayout();
        QSlider* s = new QSlider(Qt::Horizontal, this);
        s->setObjectName(name);
        s->setRange(lo, hi);
        s->setValue(value);
        lbl = new QLabel(QString::number(value), this);
        lbl->setMinimumWidth(30);
        row->addWidget(s, 1);
        row->addWidget(lbl);
        QLabel* l = lbl;
        QObject::connect(s, &QSlider::valueChanged, l, [l](int x) { l->setText(QString::number(x)); });
        return std::make_pair(s, row);
    };
    auto r = slider("ttsRate", -10, 10, QSettings().value("tts/rate", 0).toInt(), d->rateLbl);
    d->rate = r.first;
    form->addRow("Speed:", r.second);
    auto vo = slider("ttsVolume", 0, 100, QSettings().value("tts/volume", 100).toInt(), d->volumeLbl);
    d->volume = vo.first;
    form->addRow("Volume:", vo.second);
    v->addLayout(form);
    auto* bottom = new QHBoxLayout();
    d->note = new QLabel(this);
    d->note->setObjectName("ttsNote");
    d->note->setWordWrap(true);
#ifndef _WIN32
    d->note->setText("Text to Speech uses Windows' own speech, which this system does not have.");
#endif
    QPushButton* create = new QPushButton("Create", this);
    create->setObjectName("ttsCreate");
    create->setToolTip("Speak the text into a file and put it on the timeline at the cursor");
    bottom->addWidget(d->note, 1);
    bottom->addWidget(create);
    v->addLayout(bottom);
    QObject::connect(create, &QPushButton::clicked, this, [this] { this->create(); });
}

TextToSpeechWindow::~TextToSpeechWindow() {}

void TextToSpeechWindow::setText(const QString& text) { d->text->setPlainText(text); }

void TextToSpeechWindow::setRate(int rate) { d->rate->setValue(rate); }
void TextToSpeechWindow::setVolume(int volume) { d->volume->setValue(volume); }
std::string TextToSpeechWindow::lastFile() const { return d->lastFile; }

std::vector<uint64_t> TextToSpeechWindow::create()
{
    AppState& app = AppState::get();
    const QString text = d->text->toPlainText().trimmed();
    if (text.isEmpty()) {
        app.status("Text to Speech: type some text first.");
        return {};
    }
    const std::string voice = ss(d->voice->currentData().toString());
    QSettings st;
    st.setValue("tts/voice", qs(voice));
    st.setValue("tts/rate", d->rate->value());
    st.setValue("tts/volume", d->volume->value());
    const std::string dir = d->folder();
    QString words = text.simplified();
    if (words.size() > 32) words = words.left(32).trimmed();
    for (QChar& c : words)
        if (QString("\\/:*?\"<>|.").contains(c) || c.unicode() < 32) c = ' ';
    words = words.simplified();
    std::string path;
    for (int n = 1; n < 100000; ++n) {
        path = pathJoin(dir, words.isEmpty() ? strfmt("Speech %d.wav", n) : strfmt("Speech %d - %s.wav", n, ss(words).c_str()));
        if (!fileExists(path)) break;
    }
    std::string err;
    if (!synthesizer || !synthesizer(ss(text), voice, d->rate->value(), d->volume->value(), path, &err)) {
        app.status("Text to Speech: " + (err.empty() ? std::string("the speech could not be made") : err));
        d->note->setText(qs(err));
        return {};
    }
    d->lastFile = path;
    const uint64_t media = app.importMedia(path, &err);
    if (!media) {
        app.status("Text to Speech: the speech file could not be read back: " + err);
        return {};
    }
    std::vector<uint64_t> made = app.addMediaToTimeline(media, app.cursor(), app.currentTrack);
    if (!made.empty()) app.status("Text to Speech: " + pathFileName(path) + " is on the timeline.");
    return made;
}

}  // namespace app
}  // namespace reno
