#include "subtitles.h"
#include "appstate.h"
#include "fxlibrary.h"
#include "../engine/catalog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace reno {
namespace app {
namespace {

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// hh:mm:ss;ff, mm:ss, hh:mm:ss.fff or plain seconds; -1 if it is none of them
double readStamp(const std::string& text, double fps)
{
    std::string t = trim(text);
    if (t.empty()) return -1;
    char* end = nullptr;
    const double plain = std::strtod(t.c_str(), &end);
    if (end && *end == 0) return plain >= 0 ? plain : -1;
    for (char& c : t) if (c == ';') c = ':';
    std::vector<std::string> parts;
    std::string cur;
    for (char c : t) { if (c == ':') { parts.push_back(cur); cur.clear(); } else cur += c; }
    parts.push_back(cur);
    if (parts.size() < 2 || parts.size() > 4) return -1;
    double frames = 0;
    if (parts.size() == 4) { frames = std::atof(parts.back().c_str()); parts.pop_back(); }
    double total = 0;
    for (const std::string& p : parts) {
        if (p.empty() || p.find_first_not_of("0123456789.") != std::string::npos) return -1;
        total = total * 60.0 + std::atof(p.c_str());
    }
    return total + (fps > 0 ? frames / fps : 0.0);
}

// A line split into its fields at tabs, or at commas outside quotes; tabs win when there are any.
std::vector<std::string> fieldsOf(const std::string& line)
{
    std::vector<std::string> out;
    const bool tabs = line.find('\t') != std::string::npos;
    std::string cur;
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (!tabs && c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
            else quoted = !quoted;
        } else if (!quoted && ((tabs && c == '\t') || (!tabs && c == ','))) {
            out.push_back(cur);
            cur.clear();
        } else cur += c;
    }
    out.push_back(cur);
    return out;
}

const char* const kTitlesId = "com.vegascreativesoftware:titlesandtext";

std::string titlesPluginId()
{
    std::string id;
    for (const VideoFxEntry& v : Catalog::get().video()) {
        if (!v.generator || !v.ok) continue;
        if (v.id == kTitlesId) return v.id;
        const std::string& l = v.label;
        if (l.find("Titles & Text") != std::string::npos) return v.id;
        if (id.empty() && l.find("Text") != std::string::npos) id = v.id;
    }
    return id;
}

}  // namespace

std::vector<SubtitleLine> parseSubtitleText(const std::string& text, double fps)
{
    std::vector<SubtitleLine> out;
    std::string data = text;
    if (data.size() >= 3 && (unsigned char)data[0] == 0xEF && (unsigned char)data[1] == 0xBB && (unsigned char)data[2] == 0xBF) data.erase(0, 3);
    double next = 0;
    size_t pos = 0;
    while (pos <= data.size()) {
        size_t nl = data.find('\n', pos);
        if (nl == std::string::npos) nl = data.size();
        std::string line = data.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        const std::vector<std::string> f = fieldsOf(line);
        SubtitleLine s;
        bool timed = false;
        if (f.size() >= 4) {
            const double a = readStamp(f[0], fps), b = readStamp(f[1], fps), l = readStamp(f[2], fps);
            if (a >= 0 && (b >= 0 || l >= 0)) {
                s.start = a;
                s.length = l > 0 ? l : b - a;
                std::string t = f[3];
                for (size_t k = 4; k < f.size(); ++k) t += std::string(line.find('\t') != std::string::npos ? "\t" : ",") + f[k];
                s.text = trim(t);
                timed = true;
            }
        }
        if (!timed) {
            s.start = next;
            s.length = 5.0;
            s.text = trim(line);
        }
        if (s.length <= 0) s.length = 5.0;
        next = std::max(next, s.start + s.length);
        for (size_t k = 0; (k = s.text.find("\\n", k)) != std::string::npos;) { s.text.replace(k, 2, "\n"); ++k; }
        out.push_back(s);
    }
    return out;
}

std::vector<SubtitleLine> subtitlesFromRegions()
{
    std::vector<SubtitleLine> out;
    for (const Project::Marker& m : AppState::get().project().markers) {
        if (!m.region || !m.kind.empty() || m.length <= 0) continue;
        SubtitleLine s;
        s.start = m.time;
        s.length = m.length;
        s.text = m.name;
        out.push_back(s);
    }
    std::sort(out.begin(), out.end(), [](const SubtitleLine& a, const SubtitleLine& b) { return a.start < b.start; });
    return out;
}

std::vector<std::string> subtitlePresetNames()
{
    std::vector<std::string> out;
    const std::string id = titlesPluginId();
    if (id.empty()) return out;
    for (const OfxPreset& p : presetsFor(id, "Generator")) out.push_back(p.name);
    return out;
}

std::string defaultSubtitlePreset() { return QSettings().value("subtitles/preset", QString()).toString().toStdString(); }
void setDefaultSubtitlePreset(const std::string& name) { QSettings().setValue("subtitles/preset", QString::fromStdString(name)); }

int insertSubtitles(const std::vector<SubtitleLine>& lines, const std::string& preset, std::string* error)
{
    if (lines.empty()) { if (error) *error = "There is no text to make subtitles of."; return 0; }
    const std::string id = titlesPluginId();
    if (id.empty()) { if (error) *error = "Subtitles are made with the Titles & Text generator, and none was found among the plug-ins."; return 0; }
    Json base = Json::object();
    if (!preset.empty())
        for (const OfxPreset& p : presetsFor(id, "Generator"))
            if (p.name == preset) { base = presetToState(p); break; }
    AppState& app = AppState::get();
    int made = 0;
    app.edit("Insert Subtitles", [&](Project& p) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        t.name = "Subtitles";
        for (const SubtitleLine& s : lines) {
            MediaItem m;
            m.id = p.newId();
            m.name = s.text.substr(0, s.text.find('\n'));
            m.generated = true;
            m.generator.id = p.newId();
            m.generator.kind = "ofx";
            m.generator.pluginId = id;
            m.generator.name = "Titles & Text";
            Json st = base.isObject() ? base : Json::object();
            Json params = st.get("params").isObject() ? st.get("params") : Json::object();
            Json txt = Json::object();
            txt["s"] = s.text;
            params["Text"] = txt;
            st["params"] = params;
            m.generator.state = st;
            m.info.hasVideo = true;
            m.info.width = p.settings.width;
            m.info.height = p.settings.height;
            m.info.fps = p.settings.fps();
            Event e;
            e.id = p.newId();
            e.mediaId = m.id;
            e.start = std::max(0.0, s.start);
            e.length = s.length;
            e.name = m.name;
            p.media.push_back(m);
            t.events.push_back(e);
            ++made;
        }
        t.sortEvents();
        p.tracks.insert(p.tracks.begin(), t);
    });
    return made;
}

namespace {

struct PresetRow {
    QComboBox* preset = nullptr;
    QCheckBox* makeDefault = nullptr;
};
PresetRow addPresetRow(QDialog& d, QFormLayout* f)
{
    PresetRow r;
    r.preset = new QComboBox(&d);
    r.preset->setObjectName("subtitlePreset");
    r.preset->addItem("(Default)", QString());
    for (const std::string& n : subtitlePresetNames()) r.preset->addItem(QString::fromStdString(n), QString::fromStdString(n));
    const int at = r.preset->findData(QString::fromStdString(defaultSubtitlePreset()));
    if (at >= 0) r.preset->setCurrentIndex(at);
    f->addRow("Preset:", r.preset);
    r.makeDefault = new QCheckBox("Use current preset as default", &d);
    r.makeDefault->setObjectName("subtitleMakeDefault");
    f->addRow(QString(), r.makeDefault);
    return r;
}

}  // namespace

bool runInsertSubtitlesFromFile(QWidget* parent)
{
    AppState& app = AppState::get();
    QDialog d(parent);
    d.setObjectName("insertSubtitles");
    d.setWindowTitle("Insert Subtitles");
    QVBoxLayout* v = new QVBoxLayout(&d);
    QFormLayout* f = new QFormLayout();
    QHBoxLayout* h = new QHBoxLayout();
    QLineEdit* path = new QLineEdit(&d);
    path->setObjectName("subtitleFile");
    path->setMinimumWidth(320);
    QPushButton* browse = new QPushButton("Browse...", &d);
    browse->setObjectName("subtitleBrowse");
    h->addWidget(path, 1);
    h->addWidget(browse);
    f->addRow("File:", h);
    const PresetRow pr = addPresetRow(d, f);
    v->addLayout(f);
    QLabel* hint = new QLabel("A .txt or .csv file. A line of \"Start, End, Length, Text\" (timecodes as hh:mm:ss;ff, separated by tabs or commas) is placed as given; any other line becomes a 5 second title, one after another.", &d);
    hint->setWordWrap(true);
    v->addWidget(hint);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    QObject::connect(browse, &QPushButton::clicked, &d, [&] {
        const QString last = QSettings().value("subtitles/dir", QString()).toString();
        const QString file = QFileDialog::getOpenFileName(&d, "Insert Subtitles", last, "Subtitle files (*.txt *.csv);;All files (*)");
        if (file.isEmpty()) return;
        path->setText(file);
        QSettings().setValue("subtitles/dir", QFileInfo(file).absolutePath());
    });
    if (d.exec() != QDialog::Accepted) return false;
    std::string data;
    if (!readFile(path->text().toStdString(), data)) { app.status("The subtitle file could not be read."); return false; }
    const std::vector<SubtitleLine> lines = parseSubtitleText(data, app.project().settings.fps());
    const std::string preset = pr.preset->currentData().toString().toStdString();
    if (pr.makeDefault->isChecked()) setDefaultSubtitlePreset(preset);
    std::string err;
    const int n = insertSubtitles(lines, preset, &err);
    if (n == 0) { app.status(err); return false; }
    app.status(QString("Inserted %1 subtitle%2.").arg(n).arg(n == 1 ? "" : "s").toStdString());
    return true;
}

bool runInsertSubtitlesFromRegions(QWidget* parent)
{
    AppState& app = AppState::get();
    QDialog d(parent);
    d.setObjectName("insertSubtitlesFromRegions");
    d.setWindowTitle("Insert Subtitles from Regions");
    QVBoxLayout* v = new QVBoxLayout(&d);
    QFormLayout* f = new QFormLayout();
    const PresetRow pr = addPresetRow(d, f);
    v->addLayout(f);
    QDialogButtonBox* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    v->addWidget(bb);
    QObject::connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return false;
    const std::string preset = pr.preset->currentData().toString().toStdString();
    if (pr.makeDefault->isChecked()) setDefaultSubtitlePreset(preset);
    const std::vector<SubtitleLine> lines = subtitlesFromRegions();
    std::string err;
    const int n = insertSubtitles(lines, preset, &err);
    if (n == 0) { app.status(lines.empty() ? "There are no regions: make some with the R key and name them with the subtitle text." : err); return false; }
    app.status(QString("Inserted %1 subtitle%2.").arg(n).arg(n == 1 ? "" : "s").toStdString());
    return true;
}

}  // namespace app
}  // namespace reno
