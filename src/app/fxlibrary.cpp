// Effect presets (OpenFX preset XML, as VEGAS keeps them) and plug-in packages. See fxlibrary.h.
#include "fxlibrary.h"
#include "../core/base.h"
#include "../engine/catalog.h"
#include "../ofx/ofxhost.h"
#include "../ofx/builtins.h"
#include "strconv.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <sstream>

namespace reno {
namespace app {

namespace {


std::mutex gMutex;
std::string gUserDir, gVegasDir, gPackageRoot;
// presets read from a file, by file (with its time and size, to re-read on change)
struct FileCache {
    qint64 stamp = -1, size = -1;
    std::vector<OfxPreset> presets;
};
std::map<std::string, FileCache> gFiles;

const std::vector<OfxPreset>& readCached(const std::string& path)
{
    static const std::vector<OfxPreset> none;
    QFileInfo fi(qs(path));
    if (!fi.isFile()) return none;
    FileCache& c = gFiles[path];
    const qint64 stamp = fi.lastModified().toMSecsSinceEpoch(), size = fi.size();
    if (c.stamp != stamp || c.size != size) {
        c.stamp = stamp;
        c.size = size;
        c.presets.clear();
        std::string text;
        if (size < 64 * 1024 * 1024 && readFile(path, text)) c.presets = parsePresets(text);
        for (OfxPreset& p : c.presets) p.file = path;
    }
    return c.presets;
}

bool isNumberType(const std::string& t)
{
    return t != "OfxParamTypeString" && t != "OfxParamTypeCustom" && t != "OfxParamTypeImage" && t != "OfxParamTypeGroup" &&
           t != "OfxParamTypePage" && t != "OfxParamTypePushButton";
}

std::string numbersText(const std::vector<double>& v, bool whole)
{
    std::string out;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) out += ' ';
        out += whole ? strfmt("%d", (int)std::lround(v[k])) : strfmt("%.6f", v[k]);
    }
    return out;
}

// Every user preset file under 'dir' (any depth: VEGAS sorts them into a folder per effect).
std::vector<std::string> xmlFilesUnder(const std::string& dir)
{
    std::vector<std::string> out;
    if (dir.empty() || !QDir(qs(dir)).exists()) return out;
    QDirIterator it(qs(dir), QStringList() << "*.xml" << "*.XML", QDir::Files, QDirIterator::Subdirectories);
    int n = 0;
    while (it.hasNext() && n++ < 20000) out.push_back(s8(it.next()));
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

std::string presetContext(const std::string& c)
{
    const std::string prefix = "OfxImageEffectContext";
    if (c.compare(0, prefix.size(), prefix) == 0) return c.substr(prefix.size());
    return c.empty() ? std::string("Filter") : c;
}

std::vector<OfxPreset> parsePresets(const std::string& xml)
{
    std::vector<OfxPreset> out;
    QXmlStreamReader r(QByteArray(xml.data(), (qsizetype)xml.size()));
    OfxPreset cur;
    bool inPreset = false;
    OfxPreset::Value val;
    bool inValue = false;
    while (!r.atEnd()) {
        const QXmlStreamReader::TokenType t = r.readNext();
        if (t == QXmlStreamReader::StartElement) {
            const QString n = r.name().toString();
            if (n == "OfxPreset") {
                cur = OfxPreset();
                cur.pluginId = s8(r.attributes().value("plugin").toString());
                cur.context = s8(r.attributes().value("context").toString());
                cur.name = s8(r.attributes().value("name").toString());
                inPreset = true;
            } else if (inPreset && n == "OfxPlugin") {
                const std::string id = s8(r.readElementText().trimmed());
                if (cur.pluginId.empty()) cur.pluginId = id;
            } else if (inPreset && n == "OfxPluginVersion") {
                cur.version = s8(r.readElementText().trimmed());
            } else if (inPreset && n.startsWith("OfxParamType")) {
                val = OfxPreset::Value();
                val.type = s8(n);
                val.name = s8(r.attributes().value("name").toString());
                inValue = true;
            } else if (inValue && n == "OfxParamValue") {
                const QString text = r.readElementText();
                if (isNumberType(val.type)) {
                    for (const QString& w : text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
                        bool ok = false;
                        const double d = w.toDouble(&ok);
                        val.numbers.push_back(ok && std::isfinite(d) ? d : 0.0);
                    }
                } else {
                    val.text = s8(text);
                }
            }
        } else if (t == QXmlStreamReader::EndElement) {
            const QString n = r.name().toString();
            if (inValue && n.startsWith("OfxParamType")) {
                if (!val.name.empty()) cur.values.push_back(val);
                inValue = false;
            } else if (inPreset && n == "OfxPreset") {
                if (!cur.name.empty() && !cur.pluginId.empty()) out.push_back(cur);
                inPreset = false;
            }
        }
    }
    return out;
}

std::string writePreset(const OfxPreset& p)
{
    QByteArray buf;
    QXmlStreamWriter w(&buf);
    w.setAutoFormatting(true);
    w.setAutoFormattingIndent(-1);          // tabs, as VEGAS's files have
    w.writeStartDocument();
    w.writeStartElement("OfxPresetPackage");
    w.writeStartElement("OfxPreset");
    w.writeAttribute("plugin", qs(p.pluginId));
    w.writeAttribute("context", qs(p.context.empty() ? std::string("Filter") : p.context));
    w.writeAttribute("name", qs(p.name));
    w.writeTextElement("OfxPlugin", qs(p.pluginId));
    w.writeTextElement("OfxPluginVersion", qs(p.version.empty() ? std::string("1 0") : p.version));
    for (const OfxPreset::Value& v : p.values) {
        w.writeStartElement(qs(v.type));
        w.writeAttribute("name", qs(v.name));
        const bool whole = v.type == "OfxParamTypeInteger" || v.type == "OfxParamTypeBoolean" || v.type == "OfxParamTypeChoice" ||
                           v.type == "OfxParamTypeInteger2D" || v.type == "OfxParamTypeInteger3D";
        w.writeTextElement("OfxParamValue", isNumberType(v.type) ? qs(numbersText(v.numbers, whole)) : qs(v.text));
        w.writeEndElement();
    }
    w.writeEndElement();
    w.writeEndElement();
    w.writeEndDocument();
    return std::string(buf.constData(), (size_t)buf.size());
}

std::string userPresetDir()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gUserDir.empty() ? pathJoin(userDataDir(), "OFX Presets") : gUserDir;
}

void setUserPresetDir(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(gMutex);
    gUserDir = dir;
}

std::string vegasPresetDir()
{
    std::lock_guard<std::mutex> lk(gMutex);
    if (!gVegasDir.empty()) return gVegasDir == "-" ? std::string() : gVegasDir;
    // VEGAS keeps presets saved in its effects windows in the user's Documents, "OFX Presets"
    const QString docs = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return docs.isEmpty() ? std::string() : s8(QDir(docs).filePath("OFX Presets"));
}

void setVegasPresetDir(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(gMutex);
    gVegasDir = dir.empty() ? std::string("-") : dir;
}

void forgetPresets()
{
    std::lock_guard<std::mutex> lk(gMutex);
    gFiles.clear();
}

std::vector<OfxPreset> presetsFor(const std::string& pluginId, const std::string& context, const std::string& binaryPath)
{
    std::string binary = binaryPath;
    if (binary.empty()) {
        VideoFxEntry e;
        if (Catalog::get().findVideo(pluginId, &e)) binary = e.path;
    }
    const std::string userDir = userPresetDir(), vegasDir = vegasPresetDir();
    std::vector<OfxPreset> own, user;
    std::lock_guard<std::mutex> lk(gMutex);
    auto wanted = [&](const OfxPreset& p) { return p.pluginId == pluginId && (context.empty() || p.context.empty() || p.context == context); };
    if (ofx::isBuiltinPath(binary)) {
        // Reno's built-in plug-ins carry their presets in the program
        static const std::vector<OfxPreset> builtin = parsePresets(ofx::builtinPresetPackage());
        for (const OfxPreset& p : builtin)
            if (wanted(p)) own.push_back(p);
    } else if (!binary.empty()) {
        // <name>.ofx.bundle/Contents/<platform>/<name>.ofx -> <name>.ofx.bundle/Contents/Presets/PresetPackage.xml
        const std::string contents = pathDir(pathDir(binary));
        for (const OfxPreset& p : readCached(pathJoin(pathJoin(contents, "Presets"), "PresetPackage.xml")))
            if (wanted(p)) own.push_back(p);
    }
    for (const std::string& dir : {vegasDir, userDir}) {
        if (dir.empty()) continue;
        const bool vegas = dir == vegasDir && dir != userDir;
        for (const std::string& f : xmlFilesUnder(dir))
            for (OfxPreset p : readCached(f)) {
                if (!wanted(p)) continue;
                p.user = true;
                p.vegas = vegas;
                // one of the same name saved in Reno replaces VEGAS's
                user.erase(std::remove_if(user.begin(), user.end(), [&](const OfxPreset& o) { return o.name == p.name; }), user.end());
                user.push_back(p);
            }
    }
    std::sort(user.begin(), user.end(), [](const OfxPreset& a, const OfxPreset& b) {
        return QString::compare(qs(a.name), qs(b.name), Qt::CaseInsensitive) < 0;
    });
    own.insert(own.end(), user.begin(), user.end());
    return own;
}

OfxPreset capturePreset(ofx::Instance* inst, double time, const std::string& name)
{
    OfxPreset p;
    p.name = name;
    if (!inst || !inst->plugin) return p;
    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
    p.pluginId = inst->plugin->info.id;
    p.context = presetContext(inst->context());
    p.version = strfmt("%d %d", inst->plugin->info.versionMajor, inst->plugin->info.versionMinor);
    for (const auto& up : inst->params.params) {
        const ofx::Param* pr = up.get();
        if (!pr->hasValue()) continue;
        if (!pr->props.getInt(kOfxParamPropPersistant, 0, 1)) continue;
        OfxPreset::Value v;
        v.type = pr->type;
        v.name = pr->name;
        if (pr->isText()) v.text = pr->strAt(time);
        else v.numbers = pr->valueAt(time);
        p.values.push_back(v);
    }
    return p;
}

int applyPreset(ofx::Instance* inst, const OfxPreset& p, double time)
{
    if (!inst) return 0;
    int n = 0;
    for (const OfxPreset::Value& v : p.values) {
        ofx::Param* pr = nullptr;
        {
            std::lock_guard<std::recursive_mutex> lk(inst->mutex);
            pr = inst->params.find(v.name);
            if (!pr || !pr->hasValue()) continue;
        }
        if (pr->isText()) {
            if (isNumberType(v.type)) continue;
            inst->userSetString(pr, v.text, time);
        } else {
            if (v.numbers.empty()) continue;
            std::vector<double> nums = v.numbers;
            nums.resize((size_t)pr->dim, nums.back());
            // kept inside the setting's range (kOfxParamPropMin / Max), as VEGAS does: its own presets give Cookie
            // Cutter's Repeat X and Y as 0 (they are 1..100), which cut everything away
            {
                std::lock_guard<std::recursive_mutex> lk(inst->mutex);
                for (int k = 0; k < pr->dim; ++k) {
                    double& x = nums[(size_t)k];
                    if (pr->props.dimension(kOfxParamPropMin) > k) x = std::max(x, pr->props.getDouble(kOfxParamPropMin, k));
                    if (pr->props.dimension(kOfxParamPropMax) > k) x = std::min(x, pr->props.getDouble(kOfxParamPropMax, k));
                }
            }
            inst->userSetValue(pr, nums, time, false);
        }
        ++n;
    }
    return n;
}

Json presetToState(const OfxPreset& p)
{
    Json j = Json::object();
    j["plugin"] = p.pluginId;
    Json ps = Json::object();
    for (const OfxPreset::Value& v : p.values) {
        Json e = Json::object();
        if (isNumberType(v.type)) {
            if (v.numbers.empty()) continue;
            Json a = Json::array();
            for (double x : v.numbers) a.push(x);
            e["v"] = a;
        } else {
            e["s"] = v.text;
        }
        ps[v.name] = e;
    }
    j["params"] = ps;
    j["preset"] = true;         // the host keeps values inside each setting's range, as when a preset is picked
    return j;
}

int applyDefaults(ofx::Instance* inst, double time)
{
    if (!inst) return 0;
    OfxPreset p;
    {
        std::lock_guard<std::recursive_mutex> lk(inst->mutex);
        for (const auto& up : inst->params.params) {
            const ofx::Param* pr = up.get();
            if (!pr->hasValue()) continue;
            if (!pr->props.getInt(kOfxParamPropPersistant, 0, 1)) continue;
            OfxPreset::Value v;
            v.type = pr->type;
            v.name = pr->name;
            if (pr->isText()) v.text = pr->props.getString(kOfxParamPropDefault);
            else
                for (int k = 0; k < pr->dim; ++k) v.numbers.push_back(pr->props.getDouble(kOfxParamPropDefault, k));
            p.values.push_back(v);
        }
    }
    return applyPreset(inst, p, time);
}

std::string safeFileName(const std::string& name)
{
    std::string out;
    for (char c : name) out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) ? '_' : c;
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out.empty() ? std::string("Untitled") : out;
}

// ------------------------------------------------------------------------------------------ audio presets
namespace {
std::string gAudioDir;
// a plug-in's folder: its name plus a short hash of its key (two plug-ins may share a name)
std::string audioFolder(const std::string& key, const std::string& name)
{
    uint32_t h = 2166136261u;
    for (unsigned char c : key) h = (h ^ c) * 16777619u;
    return pathJoin(audioPresetDir(), safeFileName(name.empty() ? std::string("Plug-in") : name) + strfmt(" %08x", h));
}
}  // namespace

std::string audioPresetDir()
{
    std::lock_guard<std::mutex> lk(gMutex);
    return gAudioDir.empty() ? pathJoin(userDataDir(), "Audio Presets") : gAudioDir;
}

void setAudioPresetDir(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(gMutex);
    gAudioDir = dir;
}

std::vector<AudioPreset> audioPresetsFor(const std::string& pluginKey, const std::string& pluginName)
{
    std::vector<AudioPreset> out;
    QDir dir(qs(audioFolder(pluginKey, pluginName)));
    for (const QFileInfo& fi : dir.entryInfoList(QStringList() << "*.json", QDir::Files, QDir::Name | QDir::IgnoreCase)) {
        QFile f(fi.filePath());
        if (!f.open(QIODevice::ReadOnly) || f.size() > 64 * 1024 * 1024) continue;
        Json j;
        if (!Json::parse(f.readAll().toStdString(), j) || !j.isObject()) continue;
        AudioPreset p;
        p.name = j.get("name").asString(s8(fi.completeBaseName()));
        p.pluginKey = j.get("plugin").asString();
        p.state = j.get("state").asString();
        p.file = s8(fi.filePath());
        if (p.pluginKey == pluginKey && !p.state.empty()) out.push_back(p);
    }
    return out;
}

bool saveAudioPreset(const AudioPreset& p, const std::string& pluginName, std::string* error)
{
    if (p.name.empty() || p.pluginKey.empty()) {
        if (error) *error = "The preset needs a name.";
        return false;
    }
    if (p.state.empty()) {
        if (error) *error = "The plug-in gave no settings to keep.";
        return false;
    }
    const std::string dir = audioFolder(p.pluginKey, pluginName);
    QDir().mkpath(qs(dir));
    Json j = Json::object();
    j["name"] = p.name;
    j["plugin"] = p.pluginKey;
    j["state"] = p.state;
    const std::string path = pathJoin(dir, safeFileName(p.name) + ".json");
    QFile f(qs(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(QByteArray::fromStdString(j.dump())) < 0) {
        if (error) *error = "Could not write " + path;
        return false;
    }
    return true;
}

bool deleteAudioPreset(const AudioPreset& p, std::string* error)
{
    if (p.file.empty() || !QFile::remove(qs(p.file))) {
        if (error) *error = "Could not delete " + (p.file.empty() ? p.name : p.file);
        return false;
    }
    return true;
}

bool saveUserPreset(const OfxPreset& p, std::string* error)
{
    if (p.name.empty() || p.pluginId.empty()) {
        if (error) *error = "The preset needs a name.";
        return false;
    }
    for (const OfxPreset& o : presetsFor(p.pluginId, p.context))
        if (!o.user && o.name == p.name) {
            if (error) *error = strfmt("The preset '%s' is part of the plug-in and cannot be replaced. Enter a different name.", p.name.c_str());
            return false;
        }
    const std::string dir = pathJoin(pathJoin(userPresetDir(), safeFileName(p.pluginId)), safeFileName(p.context.empty() ? std::string("Filter") : p.context));
    QDir().mkpath(qs(dir));
    const std::string file = pathJoin(dir, safeFileName(p.name) + ".xml");
    if (!writeFile(file, writePreset(p))) {
        if (error) *error = "Could not write " + file;
        return false;
    }
    forgetPresets();
    return true;
}

bool deleteUserPreset(const OfxPreset& p, std::string* error)
{
    if (!p.user || p.file.empty()) {
        if (error) *error = strfmt("The preset '%s' is part of the plug-in and cannot be deleted.", p.name.c_str());
        return false;
    }
    if (p.vegas) {
        if (error) *error = strfmt("The preset '%s' was saved by VEGAS and is not changed by Reno (%s).", p.name.c_str(), p.file.c_str());
        return false;
    }
    if (!QFile::remove(qs(p.file))) {
        if (error) *error = "Could not delete " + p.file;
        return false;
    }
    forgetPresets();
    return true;
}

// ------------------------------------------------------------------------------------------ packages
namespace {
const char* kindFolder(PackageKind k)
{
    switch (k) {
    case PackageKind::AudioFx: return "Audio FX";
    case PackageKind::Transition: return "Transitions";
    case PackageKind::Generator: return "Media Generators";
    default: return "Video FX";
    }
}
const char* kindWord(PackageKind k)
{
    switch (k) {
    case PackageKind::AudioFx: return "audio";
    case PackageKind::Transition: return "transition";
    case PackageKind::Generator: return "generator";
    default: return "video";
    }
}
}  // namespace

namespace {
std::map<int, std::function<void()>>& packageListeners()
{
    static std::map<int, std::function<void()>> l;
    return l;
}
void packagesChanged()
{
    const auto copy = packageListeners();
    for (const auto& kv : copy)
        if (kv.second) kv.second();
}
}  // namespace

int onPackagesChanged(std::function<void()> fn)
{
    static int next = 0;
    packageListeners()[++next] = std::move(fn);
    return next;
}

void forgetPackagesListener(int id) { packageListeners().erase(id); }

std::string packageDir(PackageKind kind)
{
    std::string root;
    {
        std::lock_guard<std::mutex> lk(gMutex);
        root = gPackageRoot;
    }
    if (root.empty()) root = pathJoin(userDataDir(), "Plug-In Packages");
    return pathJoin(root, kindFolder(kind));
}

void setPackageRoot(const std::string& dir)
{
    std::lock_guard<std::mutex> lk(gMutex);
    gPackageRoot = dir;
}

std::string packageToText(const FxPackage& p)
{
    Json j = Json::object();
    j["reno"] = "plug-in package";
    j["name"] = p.name;
    j["kind"] = kindWord(p.kind);
    Json list = Json::array();
    for (const FxSlot& s : p.chain) {
        Json e = Json::object();
        e["kind"] = s.kind;
        e["name"] = s.name;
        if (s.kind == "audio") e["audio"] = s.audio.toJson();
        else e["plugin"] = s.pluginId;
        if (s.bypass) e["bypass"] = true;
        if (s.beforePanCrop) e["beforePanCrop"] = true;
        if (!s.state.isNull()) e["state"] = s.state;
        list.push(e);
    }
    j["plugins"] = list;
    return j.dump(1);
}

bool packageFromText(const std::string& text, FxPackage& out)
{
    Json j;
    if (!Json::parse(text, j) || !j.isObject() || j.get("reno").asString("") != "plug-in package") return false;
    out = FxPackage();
    out.name = j.get("name").asString("");
    const std::string k = j.get("kind").asString("video");
    out.kind = k == "audio" ? PackageKind::AudioFx : k == "transition" ? PackageKind::Transition : k == "generator" ? PackageKind::Generator : PackageKind::VideoFx;
    for (const Json& e : j.get("plugins").items()) {
        if (!e.isObject()) continue;
        FxSlot s;
        s.kind = e.get("kind").asString("ofx");
        if (s.kind != "ofx" && s.kind != "audio") continue;
        s.name = e.get("name").asString("");
        if (s.kind == "audio") s.audio = AudioPluginDesc::fromJson(e.get("audio"));
        else s.pluginId = e.get("plugin").asString("");
        if (s.kind == "ofx" && s.pluginId.empty()) continue;
        s.bypass = e.get("bypass").asBool();
        s.beforePanCrop = e.get("beforePanCrop").asBool();
        s.state = e.get("state");
        out.chain.push_back(s);
    }
    return !out.name.empty();
}

std::vector<FxPackage> listPackages(PackageKind kind)
{
    std::vector<FxPackage> out;
    const std::string dir = packageDir(kind);
    for (const std::string& f : listDir(dir)) {
        if (!endsWithNoCase(f, ".json")) continue;
        std::string text;
        FxPackage p;
        if (!readFile(pathJoin(dir, f), text) || !packageFromText(text, p)) continue;
        p.kind = kind;
        p.file = pathJoin(dir, f);
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const FxPackage& a, const FxPackage& b) { return QString::compare(qs(a.name), qs(b.name), Qt::CaseInsensitive) < 0; });
    return out;
}

bool savePackage(const FxPackage& p, std::string* error)
{
    if (p.name.empty()) {
        if (error) *error = "The package needs a name.";
        return false;
    }
    const std::string dir = packageDir(p.kind);
    QDir().mkpath(qs(dir));
    FxPackage c = p;
    for (FxSlot& s : c.chain) s.id = 0;
    const std::string file = pathJoin(dir, safeFileName(p.name) + ".json");
    if (!writeFile(file, packageToText(c))) {
        if (error) *error = "Could not write " + file;
        return false;
    }
    packagesChanged();
    return true;
}

bool deletePackage(const FxPackage& p, std::string* error)
{
    if (p.file.empty() || !QFile::remove(qs(p.file))) {
        if (error) *error = "Could not delete " + p.file;
        return false;
    }
    packagesChanged();
    return true;
}

}  // namespace app
}  // namespace reno
