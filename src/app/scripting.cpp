// Tools > Scripting: running VEGAS scripts, and the Script Menu. See scripting.h.
//
// How a script runs (Windows):
// 1. Reno's ScriptPortal.Vegas.cs and ScriptPortal.Vegas.Unsupported.cs (in scripting\ next to reno.exe) are compiled once
//    with the .NET Framework 4's csc.exe (C:\Windows\Microsoft.NET\Framework64\v4.0.30319) into Reno's data folder, named
//    by the sources' checksum so a newer Reno compiles its own.
// 2. The .NET Framework is started once in Reno's process through its hosting interface (mscoree.dll: CLRCreateInstance
//    -> ICLRMetaHost -> ICLRRuntimeInfo -> ICLRRuntimeHost).
// 3. ExecuteInDefaultAppDomain calls ScriptPortal.Vegas.RenoHost.Run("api=<address>") on the interface thread, so the
//    script's Windows Forms dialogs work as in VEGAS. Run compiles the script as VEGAS does (CodeDOM: C#, VB, JScript
// .NET),
//    finds FromVegas(Vegas) (or JScript's global code, or a VB module's Main) and runs it. Every call it makes comes back
//    here synchronously through the C functions of a ScriptSession working on a copy of the project.
// 4. The copy becomes the project as one undo step; selection, cursor and time selection follow.
// It runs in-process because scripts call Reno thousands of times (every property read is a call) and VEGAS does the same,
// so modal dialogs behave alike. As in VEGAS, a script that crashes the .NET runtime takes Reno down; auto-save keeps
// the work.
#include "scripting.h"
#include "strconv.h"
#include <cctype>
#include <QIcon>
#include "appstate.h"
#include "keyboard.h"
#include "../engine/catalog.h"
#include "../engine/fxnames.h"
#include "../engine/fxruntime.h"
#include "../engine/scriptsession.h"
#include "../ofx/ofxhost.h"
#include "../ofx/vegas_ext.h"
#include "../vegas/vegas.h"
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QWidget>
#include <algorithm>
#include <cfloat>
#include <map>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#endif

namespace reno {
namespace app {

namespace {

bool isScriptFile(const QString& name)
{
    const QString l = name.toLower();
    return l.endsWith(".cs") || l.endsWith(".js") || l.endsWith(".vb") || l.endsWith(".dll");
}
}  // namespace

std::vector<std::string> scriptMenuFolders()
{
    QStringList dirs;
    // VEGAS's: Documents\Vegas Script Menu, and its Script Menu folders in the install and application data
    dirs << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/Vegas Script Menu";
#ifdef _WIN32
    const QString appData = qEnvironmentVariable("APPDATA"), local = qEnvironmentVariable("LOCALAPPDATA"),
                  common = qEnvironmentVariable("PROGRAMDATA");
    for (const QString& base : {appData, local, common})
        if (!base.isEmpty())
            for (const QString& maker : {QString("VEGAS Pro"), QString("Sony/Vegas Pro"), QString("MAGIX/VEGAS Pro")}) {
                QDir d(base + "/" + maker);
                for (const QString& ver : d.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) dirs << d.filePath(ver) + "/Script Menu";
            }
    for (const QString& pf : {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramW6432")})
        if (!pf.isEmpty())
            for (const QString& maker : {QString("VEGAS"), QString("MAGIX"), QString("Sony")}) {
                QDir d(pf + "/" + maker);
                for (const QString& prog : d.entryList({"VEGAS Pro*", "Vegas Pro*"}, QDir::Dirs | QDir::NoDotAndDotDot))
                    dirs << d.filePath(prog) + "/Script Menu";
            }
#endif
    const QString exe = QCoreApplication::applicationDirPath();
    dirs << exe + "/Script Menu" << exe + "/../tools/scripts" << exe + "/scripts";
    std::vector<std::string> out;
    QStringList seen;
    for (const QString& d : dirs) {
        const QFileInfo fi(d);
        if (!fi.isDir()) continue;
        const QString c = fi.canonicalFilePath();
        if (seen.contains(c, Qt::CaseInsensitive)) continue;
        seen << c;
        out.push_back(s8(c));
    }
    return out;
}

std::vector<ScriptMenuItem> scanScriptMenu()
{
    std::map<QString, ScriptMenuItem> byName;
    for (const std::string& dir : scriptMenuFolders()) {
        const QDir root(qs(dir));
        QDirIterator it(qs(dir), QDir::Files, QDirIterator::Subdirectories);
        int count = 0;
        while (it.hasNext() && count < 2000) {
            QString f = it.next();
            // a shortcut (.lnk; VEGAS's Script Menu folder may hold these to avoid copies): the script it points at,
            // listed under the shortcut's name
            QString shortcutName;
            if (f.endsWith(".lnk", Qt::CaseInsensitive)) {
                const QString target = QFileInfo(f).symLinkTarget();
                if (target.isEmpty() || !isScriptFile(target) || !QFileInfo::exists(target)) continue;
                shortcutName = QFileInfo(f).path() + "/" + QFileInfo(f).completeBaseName();
                f = target;
            } else if (!isScriptFile(f)) {
                continue;
            }
            const QFileInfo fi(f);
            if (fi.suffix().toLower() == "dll" &&
                (QFileInfo::exists(fi.path() + "/" + fi.completeBaseName() + ".cs") || QFileInfo::exists(fi.path() + "/" + fi.completeBaseName() + ".js") ||
                 QFileInfo::exists(fi.path() + "/" + fi.completeBaseName() + ".vb")))
                continue;
            QString rel = root.relativeFilePath(shortcutName.isEmpty() ? fi.path() + "/" + fi.completeBaseName() : shortcutName);
            if (rel.count('/') > 2) continue;
            ++count;
            if (!byName.count(rel.toLower())) byName[rel.toLower()] = {s8(rel), s8(f)};
        }
    }
    std::vector<ScriptMenuItem> out;
    for (auto& kv : byName) out.push_back(kv.second);
    return out;
}

CommandLine parseCommandLine(const std::vector<std::string>& args)
{
    CommandLine c;
    auto lower = [](std::string s) {
        for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
        return s;
    };
    for (size_t k = 1; k < args.size(); ++k) {
        const std::string& a = args[k];
        const bool option = a.size() > 1 && (a[0] == '/' || a[0] == '-') && a.find_first_of("\\:./", 1) == std::string::npos;
        if (!option) {
            if (c.open.empty()) c.open = a;
            continue;
        }
        const std::string name = lower(a.substr(1));
        const bool hasValue = k + 1 < args.size();
        if (name == "nologo") c.noLogo = true;
        else if (name == "open" && hasValue) c.open = args[++k];
        else if ((name == "runscript" || name == "script") && hasValue) c.script = args[++k];
        else if (name == "scriptargs" && hasValue) c.scriptArgs = args[++k];
        else if (name == "cmdmodule" && hasValue) c.cmdModule = args[++k];
        else if (name == "openprj" && hasValue) c.openPrj = args[++k];
        else if (name == "colors" && hasValue) {
            const std::string v = args[++k];
            c.colors = v == "1" ? 1 : v == "0" ? 0 : -1;
        }
    }
    return c;
}

void fillScriptMenu(QMenu* menu, QWidget* parent)
{
    // the fixed actions first (Run Script..., Rescan, the closed captioning commands: "fixedScriptItem"); everything
    // after was put there by an earlier scan
    const QList<QAction*> acts = menu->actions();
    int fixed = std::min(2, (int)acts.size());
    while (fixed < acts.size() && (acts[fixed]->isSeparator() ? fixed + 1 < acts.size() && acts[fixed + 1]->property("fixedScriptItem").toBool()
                                                              : acts[fixed]->property("fixedScriptItem").toBool()))
        ++fixed;
    for (int k = fixed; k < acts.size(); ++k) {
        menu->removeAction(acts[k]);
        if (QMenu* sub = acts[k]->menu()) sub->deleteLater();
        else if (acts[k]->parent() == menu) acts[k]->deleteLater();
    }
    const std::vector<ScriptMenuItem> items = scanScriptMenu();
    if (items.empty()) return;
    menu->addSeparator();
    std::map<QString, QMenu*> subs;
    for (const ScriptMenuItem& item : items) {
        QString name = qs(item.name);
        QMenu* where = menu;
        const int slash = name.lastIndexOf('/');
        if (slash > 0) {
            const QString folder = name.left(slash);
            QMenu*& sub = subs[folder];
            if (!sub) {
                sub = new QMenu(folder, menu);
                menu->addMenu(sub);
            }
            where = sub;
            name = name.mid(slash + 1);
        }
        QAction* a = new QAction(name, where);
        a->setStatusTip(qs(item.path));
        if (QFileInfo::exists(qs(item.path) + ".png")) a->setIcon(QIcon(qs(item.path) + ".png"));
        // a name of its own, so Options > Customize Keyboard can give the script keys, as in VEGAS
        a->setObjectName("Script." + qs(item.name));
        const std::string path = item.path;
        QObject::connect(a, &QAction::triggered, menu, [parent, path] { runScript(parent, path); });
        where->addAction(a);
    }
    // the keys saved for scripts, applied to the recreated items (only once the keyboard map is in use: at start-up
    // installKeyboardMap does it after the menus exist)
    if (QMainWindow* w = parent ? qobject_cast<QMainWindow*>(parent->window()) : nullptr)
        if (w->findChild<QAction*>("customizeKeyboard")) reloadKeyboardMap(w);
}

namespace {

// Reno's ScriptPortal.Vegas source: ScriptPortal.Vegas.cs and ScriptPortal.Vegas.Unsupported.cs (what Reno lacks of
// VEGAS's API, each member throwing), in scripting\ beside reno.exe (the source tree's, when run from a build
// folder).
QStringList shimSources()
{
    const QString exe = QCoreApplication::applicationDirPath();
    for (const QString& d : {exe + "/scripting", exe + "/../scripting", exe + "/../src/scripting", exe + "/../../src/scripting"}) {
        const QString a = d + "/ScriptPortal.Vegas.cs", b = d + "/ScriptPortal.Vegas.Unsupported.cs";
        if (QFileInfo::exists(a) && QFileInfo::exists(b)) return {QFileInfo(a).canonicalFilePath(), QFileInfo(b).canonicalFilePath()};
    }
    return QStringList();
}

#ifdef _WIN32
// The .NET Framework's hosting interfaces (metahost.h / mscoree.h from the .NET Framework SDK, not in the Windows SDK):
// only the methods Reno calls, in their table positions.
struct ICLRRuntimeInfoMin : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetVersionString(LPWSTR, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeDirectory(LPWSTR, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLoaded(HANDLE, BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadErrorString(UINT, LPWSTR, DWORD*, LONG) = 0;
    virtual HRESULT STDMETHODCALLTYPE LoadLibrary_(LPCWSTR, HMODULE*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcAddress_(LPCSTR, LPVOID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInterface(REFCLSID, REFIID, LPVOID*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsLoadable(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultStartupFlags(DWORD, LPCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultStartupFlags(DWORD*, LPWSTR, DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE BindAsLegacyV2Runtime() = 0;
    virtual HRESULT STDMETHODCALLTYPE IsStarted(BOOL*, DWORD*) = 0;
};
struct ICLRMetaHostMin : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetRuntime(LPCWSTR, REFIID, LPVOID*) = 0;
};
struct ICLRRuntimeHostMin : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Start() = 0;
    virtual HRESULT STDMETHODCALLTYPE Stop() = 0;
    virtual HRESULT STDMETHODCALLTYPE SetHostControl(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCLRControl(IUnknown**) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnloadAppDomain(DWORD, BOOL) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteInAppDomain(DWORD, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentAppDomainId(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteApplication(LPCWSTR, DWORD, LPCWSTR*, DWORD, LPCWSTR*, int*) = 0;
    virtual HRESULT STDMETHODCALLTYPE ExecuteInDefaultAppDomain(LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, DWORD*) = 0;
};
const GUID kCLSID_CLRMetaHost = {0x9280188d, 0x0e8e, 0x4867, {0xb3, 0x0c, 0x7f, 0xa8, 0x38, 0x84, 0xe8, 0xde}};
const GUID kIID_ICLRMetaHost = {0xd332db9e, 0xb9b3, 0x4125, {0x82, 0x07, 0xa1, 0x48, 0x84, 0xf5, 0x32, 0x16}};
const GUID kIID_ICLRRuntimeInfo = {0xbd39d1d2, 0xba2f, 0x486a, {0x89, 0xb0, 0xb4, 0xb0, 0xcb, 0x46, 0x68, 0x91}};
const GUID kCLSID_CLRRuntimeHost = {0x90f1a06e, 0x7712, 0x4762, {0x86, 0xb5, 0x7a, 0x5e, 0xba, 0x6b, 0xdb, 0x02}};
const GUID kIID_ICLRRuntimeHost = {0x90f1a06c, 0x7712, 0x4762, {0x86, 0xb5, 0x7a, 0x5e, 0xba, 0x6b, 0xdb, 0x02}};

QString hr(HRESULT h) { return QString("0x%1").arg((quint32)h, 8, 16, QChar('0')); }

ICLRRuntimeHostMin* g_host = nullptr;

ICLRRuntimeHostMin* clrHost(QString* error)
{
    if (g_host) return g_host;
    HMODULE m = LoadLibraryW(L"mscoree.dll");
    if (!m) { *error = "the .NET Framework is not installed (mscoree.dll was not found)"; return nullptr; }
    typedef HRESULT(STDAPICALLTYPE * CreateFn)(REFCLSID, REFIID, LPVOID*);
    CreateFn create = (CreateFn)(void*)GetProcAddress(m, "CLRCreateInstance");
    if (!create) { *error = "the .NET Framework 4 is not installed (CLRCreateInstance was not found)"; return nullptr; }
    ICLRMetaHostMin* meta = nullptr;
    HRESULT h = create(kCLSID_CLRMetaHost, kIID_ICLRMetaHost, (LPVOID*)&meta);
    if (FAILED(h) || !meta) { *error = "the .NET Framework could not be started (CLRCreateInstance " + hr(h) + ")"; return nullptr; }
    ICLRRuntimeInfoMin* info = nullptr;
    h = meta->GetRuntime(L"v4.0.30319", kIID_ICLRRuntimeInfo, (LPVOID*)&info);
    meta->Release();
    if (FAILED(h) || !info) { *error = "the .NET Framework 4 is not installed (" + hr(h) + ")"; return nullptr; }
    ICLRRuntimeHostMin* host = nullptr;
    h = info->GetInterface(kCLSID_CLRRuntimeHost, kIID_ICLRRuntimeHost, (LPVOID*)&host);
    info->Release();
    if (FAILED(h) || !host) { *error = "the .NET Framework could not be started (" + hr(h) + ")"; return nullptr; }
    h = host->Start();
    if (FAILED(h)) { *error = "the .NET Framework could not be started (Start " + hr(h) + ")"; host->Release(); return nullptr; }
    g_host = host;
    return g_host;
}
#endif

QString shimAssembly(QWidget* parent, QString* error)
{
    const QStringList srcs = shimSources();
    if (srcs.isEmpty()) {
        *error = "Reno's ScriptPortal.Vegas.cs and ScriptPortal.Vegas.Unsupported.cs are missing (they belong in the \"scripting\" folder beside reno.exe).";
        return QString();
    }
    QCryptographicHash hash(QCryptographicHash::Sha1);
    for (const QString& src : srcs) {
        QFile f(src);
        if (!f.open(QIODevice::ReadOnly)) { *error = "Reno's ScriptPortal.Vegas source could not be read: " + src; return QString(); }
        hash.addData(f.readAll());
    }
    const QString sum = QString::fromLatin1(hash.result().toHex().left(16));
    const QString dir = qs(userDataDir()) + "/scripting/" + sum;
    const QString dll = dir + "/ScriptPortal.Vegas.dll";
    if (QFileInfo::exists(dll)) return dll;
#ifdef _WIN32
    QString windir = qEnvironmentVariable("WINDIR", "C:/Windows");
    QString csc = windir + "/Microsoft.NET/Framework64/v4.0.30319/csc.exe";
    if (!QFileInfo::exists(csc)) csc = windir + "/Microsoft.NET/Framework/v4.0.30319/csc.exe";
    if (!QFileInfo::exists(csc)) { *error = "The .NET Framework 4's C# compiler (csc.exe) was not found in " + windir + "\\Microsoft.NET."; return QString(); }
    QDir().mkpath(dir);
    const QString tmp = dir + "/ScriptPortal.Vegas.tmp.dll";
    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QStringList args = {"/nologo", "/target:library", "/optimize+", "/nowarn:67,108,114,649,660,661,1591", "/out:" + QDir::toNativeSeparators(tmp),
                        "/r:System.dll", "/r:System.Core.dll", "/r:System.Data.dll", "/r:System.Drawing.dll", "/r:System.Windows.Forms.dll",
                        "/r:System.Xml.dll"};
    for (const QString& src : srcs) args << QDir::toNativeSeparators(src);
    p.start(csc, args);
    const bool done = p.waitForFinished(120000);
    QApplication::restoreOverrideCursor();
    if (!done || p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0 || !QFileInfo::exists(tmp)) {
        *error = "Reno's ScriptPortal.Vegas could not be compiled with " + QDir::toNativeSeparators(csc) + ":\n\n" +
                 QString::fromLocal8Bit(p.readAll()).left(4000);
        QFile::remove(tmp);
        return QString();
    }
    QFile::remove(dll);
    if (!QFile::rename(tmp, dll)) { *error = "Could not write " + dll; return QString(); }
    (void)parent;
    return dll;
#else
    (void)parent;
    *error = "Running VEGAS scripts needs Windows: they are .NET Framework programs, and Reno runs them with the .NET Framework "
             "that comes with Windows.";
    return QString();
#endif
}

int vegasParamType(ofx::ParamKind k)
{
    switch (k) {
    case ofx::ParamKind::Int: return 8;
    case ofx::ParamKind::Double: return 11;
    case ofx::ParamKind::Bool: return 4;
    case ofx::ParamKind::Choice: return 5;
    case ofx::ParamKind::RGBA: return 7;
    case ofx::ParamKind::RGB: return 6;
    case ofx::ParamKind::Double2D: return 12;
    case ofx::ParamKind::Int2D: return 9;
    case ofx::ParamKind::Double3D: return 13;
    case ofx::ParamKind::Int3D: return 10;
    case ofx::ParamKind::String: return 14;
    case ofx::ParamKind::Custom: return 15;
    case ofx::ParamKind::Group: return 2;
    case ofx::ParamKind::Page: return 1;
    case ofx::ParamKind::PushButton: return 3;
    default: return 0;
    }
}

// An OpenFX effect's parameters (OFXEffect.Parameters): an instance of the plug-in is created to ask it, as the effect
// thumbnails do (src/app/fxthumbs.cpp), then dropped.
bool describeOfx(const std::string& id, const std::string& context, const ProjectSettings& st, ScriptOfxDesc* d, std::string* error)
{
    ofx::Plugin* pl = findVideoPlugin(id, error);
    if (!pl) return false;
    std::string ctx = "OfxImageEffectContext" + context;
    if (context == "Filter" && !pl->supportsContext(ctx)) ctx = pl->filterContext();
    if (ctx.empty() || !pl->supportsContext(ctx)) {
        if (error) *error = "the plug-in cannot be used as a " + context;
        return false;
    }
    ofx::InstanceSetup setup;
    setup.width = std::max(16, st.width);
    setup.height = std::max(16, st.height);
    setup.fps = st.fpsDen > 0 && st.fpsNum > 0 ? (double)st.fpsNum / st.fpsDen : 30.0;
    setup.vegasContext = context == "Generator" ? kVegasContextGenerator : context == "Transition" ? kVegasContextEventFadeIn : kVegasContextEvent;
    std::unique_ptr<ofx::Instance> inst = pl->createInstance(ctx, setup, error);
    if (!inst) return false;
    d->label = pl->info.label.empty() || pl->info.label == id ? videoFxLabel(id) : pl->info.label;
    d->group = pl->info.grouping;
    d->path = pl->info.bundlePath.empty() ? pl->info.binaryPath : pl->info.bundlePath;
    std::lock_guard<std::recursive_mutex> lk(inst->mutex);
    for (const auto& up : inst->params.params) {
        const ofx::Param& pr = *up;
        ScriptOfxParam p;
        p.name = pr.name;
        p.label = pr.label();
        p.hint = pr.hint();
        p.parent = pr.parent();
        p.type = vegasParamType(pr.kind);
        p.dim = pr.dim;
        p.whole = pr.isWhole();
        for (int k = 0; k < pr.dim; ++k) {
            p.def.push_back(pr.props.getDouble(kOfxParamPropDefault, k, 0.0));
            p.min.push_back(pr.props.getDouble(kOfxParamPropMin, k, -DBL_MAX));
            p.max.push_back(pr.props.getDouble(kOfxParamPropMax, k, DBL_MAX));
            p.displayMin.push_back(pr.props.getDouble(kOfxParamPropDisplayMin, k, p.min.back()));
            p.displayMax.push_back(pr.props.getDouble(kOfxParamPropDisplayMax, k, p.max.back()));
        }
        if (pr.isText()) p.defText = pr.props.getString(kOfxParamPropDefault);
        p.choices = pr.choices();
        p.enabled = pr.enabled();
        p.secret = pr.secret();
        p.canAnimate = pr.hasValue() && pr.props.getInt(kOfxParamPropAnimates, 0, 1) != 0;
        p.persist = pr.hasValue() && pr.props.getInt(kOfxParamPropPersistant, 0, 1) != 0;
        d->params.push_back(p);
    }
    return true;
}

ScriptHost makeHost(AppState& app, QWidget* parent, const std::string& script, const std::string& args)
{
    ScriptHost h;
    h.version = "Version 18.0 (Build 527) Reno";
    h.appName = "Reno";
    h.installDir = s8(QCoreApplication::applicationDirPath());
    h.appDataDir = userDataDir();
    h.tempDir = s8(QDir::tempPath());
    h.scriptFile = script;
    h.scriptArgs = args;
    h.projectPath = app.path();
    h.probe = [&app](const std::string& path, MediaInfo* info, std::string* err) {
        std::unique_ptr<MediaReader> r = app.mediaOpener ? app.mediaOpener(path, err) : nullptr;
        if (!r) return false;
        *info = r->info();
        return true;
    };
    for (const VideoFxEntry& v : Catalog::get().video()) {
        if (!v.ok) continue;
        ScriptPlugin p;
        p.type = v.transition ? ScriptPlugin::Transition : v.generator ? ScriptPlugin::Generator : ScriptPlugin::VideoFilter;
        if (!v.filter && !v.transition && !v.generator) continue;
        p.id = v.id;
        p.name = v.label;
        p.group = v.group;
        h.plugins.push_back(p);
    }
    for (const AudioFxEntry& a : Catalog::get().audio()) {
        if (!a.ok || a.desc.isInstrument) continue;
        ScriptPlugin p;
        p.type = ScriptPlugin::Audio;
        p.id = a.desc.key();
        p.name = a.desc.name;
        p.group = a.desc.vendor;
        p.audio = a.desc;
        h.plugins.push_back(p);
    }
    h.newSettings = app.project().settings;
    h.loopEvents = app.prefs.loopEvents;
    h.stillLength = app.prefs.stillLength;
    h.eventPeak = [&app](const Project&, const Event& e) { return app.eventPeak(e); };
    const ProjectSettings st = app.project().settings;
    h.describeOfx = [st](const std::string& id, const std::string& context, ScriptOfxDesc* d, std::string* error) {
        return describeOfx(id, context, st, d, error);
    };
#ifdef _WIN32
    h.mainWindow = parent ? (int64_t)parent->window()->winId() : 0;
#else
    (void)parent;
#endif
    return h;
}

}  // namespace

// ---- running a script
// Before a script's new or opened project replaces the user's: unsaved changes are saved, kept (Cancel: the script's
// project is dropped) or let go, as File > New asks.
static bool letProjectGo(QWidget* parent, AppState& app, const QString& script)
{
    if (!app.dirty()) return true;
    const QString name = app.path().empty() ? QString("Untitled") : QFileInfo(qs(app.path())).fileName();
    const auto r = QMessageBox::question(parent, "Run Script",
                                         script + " replaces the project with another one.\n\nSave changes to " + name + " first?",
                                         QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (r == QMessageBox::Cancel) return false;
    if (r == QMessageBox::Discard) return true;
    QString file = qs(app.path());
    if (file.isEmpty()) {
        file = QFileDialog::getSaveFileName(parent, "Save Project", QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
                                            "Reno project (*.reno)");
        if (file.isEmpty()) return false;
    }
    std::string err;
    if (!app.save(s8(file), &err)) {
        QMessageBox::warning(parent, "Run Script", "The project could not be saved: " + qs(err));
        return false;
    }
    return true;
}

void runScript(QWidget* parent, const std::string& path, const std::string& args)
{
    AppState& app = AppState::get();
    const QString name = QFileInfo(qs(path)).completeBaseName();
    QString error;
    const QString dll = shimAssembly(parent, &error);
    if (dll.isEmpty()) {
        QMessageBox::warning(parent, "Run Script", error);
        return;
    }
    // the script starts from the project with effect settings as the plug-ins hold them now (copies and splits of events
    // carry them)
    Project start = app.project();
    app.fx.captureStates(start);
    ScriptHost host = makeHost(app, parent, path, args);
    std::string label = "Run Script: " + s8(name);
    ScriptSession* running = nullptr;
    bool replaceAsked = false, replaceOk = true;
    // VEGAS's SaveProject during a script: what the script has done so far becomes the project, which is saved
    bool committed = false;
    host.save = [&](const Project& p, const std::string& file, std::string* err) {
        if (running && (running->startedNew || !running->openedPath.empty()) && !replaceAsked) {
            replaceAsked = true;
            replaceOk = letProjectGo(parent, app, name);
        }
        if (!replaceOk) {
            if (err) *err = "the user kept the project that was open";
            return false;
        }
        Project copy = p;
        std::vector<uint64_t> fxIds;
        if (running) fxIds.assign(running->fxStatesChanged.begin(), running->fxStatesChanged.end());
        app.editFx(label, [&](Project& live) {
            copy.autoCrossfades = live.autoCrossfades;
            live = copy;
        }, fxIds);
        committed = true;
        return app.save(file, err);
    };
    host.open = [](const std::string& file, Project* p, std::string* err) { return loadProjectFile(file, *p, err); };
    std::vector<std::string> output;
    host.log = [&output](const std::string& s) { output.push_back(s); };
    ScriptSession session(start, host);
    running = &session;
    session.selection = app.selection();
    if (app.currentTrack) session.selectedTracks.insert(app.currentTrack);
    session.cursor = app.cursor();
    session.selStart = app.loopStart;
    session.selLength = std::max(0.0, app.loopEnd - app.loopStart);
    session.loopOn = app.loopOn;
    session.bypassFx = app.bypassAllFx;

#ifdef _WIN32
    ICLRRuntimeHostMin* clr = clrHost(&error);
    if (!clr) {
        QMessageBox::warning(parent, "Run Script", "The script cannot run: " + error);
        return;
    }
    // Windows Forms dialogs need this thread in a single-threaded COM apartment; Qt already made it one (for drag and
    // drop), so this only checks (and counts once more, undone below).
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (apartment == RPC_E_CHANGED_MODE) app.status("Note: the interface thread is not a single-threaded COM apartment: a script's file dialogs may fail.");
    app.status("Running " + s8(name) + "...");
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::wstring arg = QString("api=%1").arg((quint64)(uintptr_t)session.api(), 0, 16).toStdWString();
    const std::wstring wdll = QDir::toNativeSeparators(dll).toStdWString();
    DWORD ret = 0;
    const HRESULT h = clr->ExecuteInDefaultAppDomain(wdll.c_str(), L"ScriptPortal.Vegas.RenoHost", L"Run", arg.c_str(), &ret);
    QApplication::restoreOverrideCursor();
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (FAILED(h) && !session.finished) {
        session.finished = session.failed = true;
        session.message = s8("Reno's ScriptPortal.Vegas could not be started (" + hr(h) + "). If this keeps happening, delete the folder " +
                             QDir::toNativeSeparators(QFileInfo(dll).path()) + " so that Reno compiles it again.");
    }
#else
    session.finished = session.failed = true;
    session.message = "Running VEGAS scripts needs Windows.";
#endif
    running = nullptr;

    if (!session.undoLabel.empty()) label = session.undoLabel;
    const bool replaces = session.startedNew || !session.openedPath.empty();
    if (replaces && !replaceAsked) replaceOk = letProjectGo(parent, app, name);
    if (session.cancelled) {
        app.status(s8(name) + ": the script cancelled its changes.");
    } else if (!replaceOk) {
        app.status(s8(name) + ": the project the script made was not used (the open project was kept).");
    } else {
        if (committed) {
        } else if (!session.openedPath.empty()) {
            std::string err;
            if (!app.open(session.openedPath, &err)) QMessageBox::warning(parent, "Run Script", qs("The project the script opened could not be opened: " + err));
        } else if (session.startedNew) {
            app.newProject();
        }
        if (session.changed || session.startedNew) {
            Project result = session.project;
            // effects whose settings the script changed: the running plug-ins take the new ones (after a new or opened
            // project there are none running)
            std::vector<uint64_t> fxIds;
            if (!replaces) fxIds.assign(session.fxStatesChanged.begin(), session.fxStatesChanged.end());
            app.editFx(label, [&](Project& live) {
                result.autoCrossfades = live.autoCrossfades;
                live = result;
            }, fxIds);
        }
        if (session.viewChanged || session.changed) {
            std::set<uint64_t> sel;
            for (uint64_t id : session.selection)
                if (app.project().findEvent(id)) sel.insert(id);
            app.setSelection(sel);
            if (!session.selectedTracks.empty()) app.currentTrack = *session.selectedTracks.begin();
            app.setCursor(session.cursor);
            app.setLoop(session.selStart, session.selStart + std::max(0.0, session.selLength));
            app.loopOn = session.loopOn;
            app.bypassAllFx = session.bypassFx;
            app.notify(kSelection | kCursor | kPlayback);
        }
        app.status(session.failed ? s8(name) + " stopped with an error." : s8(name) + (session.changed || committed ? ": done." : ": done (nothing was changed)."));
    }
    if (session.failed) {
        QString text = qs(session.message);
        if (!output.empty()) {
            text += "\n\nThe script's output:";
            for (size_t k = 0; k < output.size() && k < 30; ++k) text += "\n" + qs(output[k]);
        }
        QMessageBox box(QMessageBox::Warning, "Run Script", name + ": the script stopped with an error.", QMessageBox::Ok, parent);
        box.setInformativeText(session.changed && !session.cancelled ? "What it changed before the error is kept (Edit > Undo takes it back)." : QString());
        box.setDetailedText(text);
        box.setText(name + ": " + text.left(600));
        box.exec();
    }
}

void runScriptDialog(QWidget* parent)
{
    QSettings st;
    QString dir = st.value("scripting/lastFolder").toString();
    if (dir.isEmpty()) {
        const std::vector<std::string> f = scriptMenuFolders();
        dir = f.empty() ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) : qs(f.front());
    }
    const QString file = QFileDialog::getOpenFileName(parent, "Run Script", dir,
                                                      "VEGAS scripts (*.cs *.js *.vb *.dll);;C# (*.cs);;JScript (*.js);;Visual Basic (*.vb);;Compiled scripts (*.dll)");
    if (file.isEmpty()) return;
    st.setValue("scripting/lastFolder", QFileInfo(file).path());
    runScript(parent, s8(file));
}

}  // namespace app
}  // namespace reno
