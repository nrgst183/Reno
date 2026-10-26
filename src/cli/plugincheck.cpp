// Plug-in checking commands for reno_cli (called from main.cpp). The probe commands try one plug-in file in a
// separate process so a crash or hang cannot take the caller down.
//
//   reno_cli check [--out-dir DIR] [--no-audio] [--no-video] [--no-dx]
//       Tries every OpenFX, VST and DirectX audio plug-in, each in a separate process, and writes
//       plugin_report.txt and plugin_report.json.
//   reno_cli ofxprobe [--out FILE] [--no-render] [--verbose] [--stubs] [--plain-host] [--host-name NAME] file.ofx ...
//   reno_cli audioprobe [--out FILE] [--describe-only] file.dll|file.vst3 ...
//   reno_cli dxprobe [--out FILE] [uid]
#include "plugincheck.h"
#include "../core/base.h"
#include "../core/json.h"
#include "../core/process.h"
#include "../ofx/probe.h"
#include "../ofx/fxcompare.h"
#include "../audiofx/audioplugin.h"
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <shellapi.h>
#  include <objbase.h>
#endif

using namespace reno;

static void emit(const Json& j, const std::string& outPath)
{
    const std::string text = j.dump(2);
    if (outPath.empty()) std::printf("%s\n", text.c_str());
    else writeFile(outPath, text);
}

// ---------------------------------------------------------------------------------------------- one file
static int cmdOfxProbe(const std::vector<std::string>& args)
{
    ofx::ProbeOptions opt;
    std::vector<std::string> files;
    std::string out;
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        if (a == "--no-render") opt.render = false;
        else if (a == "--verbose") ofx::Host::get().config.verbose = true;
        else if (a == "--stubs") ofx::Host::get().config.stubUnknownSuites = true;
        else if (a == "--host-name" && k + 1 < args.size()) ofx::Host::get().config.hostName = args[++k];
        else if (a == "--plain-host") { ofx::Host::get().config.vegasExtensions = false; ofx::Host::get().config.hostName = "com.reno.editor"; }
        else if (a == "--out" && k + 1 < args.size()) out = args[++k];
        else files.push_back(a);
    }
    ofx::Host::get().applyConfig();
    Json all = Json::array();
    for (const auto& f : files) {
        all.push(ofx::probeBinary(f, opt));
        emit(all, out);                 // written after every file
    }
    emit(all, out);
    return 0;
}

// Runs a second of noise through an audio plug-in and reports the result.
static void tryAudioPlugin(const AudioPluginDesc& d, Json& j)
{
    std::string e;
    std::unique_ptr<AudioPlugin> p = createAudioPlugin(d, &e);
    if (!p) { j["ok"] = false; j["error"] = e; return; }
    j["name"] = p->desc.name;
    j["vendor"] = p->desc.vendor;
    j["inputs"] = p->desc.numInputs;
    j["outputs"] = p->desc.numOutputs;
    j["params"] = p->paramCount();
    j["editor"] = p->hasEditor();
    bool ok = p->activate(48000.0, 512);
    if (!ok) e = "would not start processing";
    if (ok) {
        std::vector<float> l(48000), r(48000);
        unsigned seed = 12345;
        double inE = 0, outE = 0;
        for (size_t k = 0; k < l.size(); ++k) {
            seed = seed * 1664525u + 1013904223u;
            float v = ((seed >> 8) & 0xFFFF) / 32768.f - 1.f;
            l[k] = r[k] = 0.25f * v * (float)std::sin(k * 0.01);
            inE += (double)l[k] * l[k];
        }
        for (size_t at = 0; at < l.size(); at += 512) p->process(l.data() + at, r.data() + at, (int)std::min<size_t>(512, l.size() - at), (int64_t)at, true);
        bool finite = true;
        for (size_t k = 0; k < l.size(); ++k) {
            if (!std::isfinite(l[k]) || !std::isfinite(r[k])) finite = false;
            outE += (double)l[k] * l[k];
        }
        j["inRms"] = std::sqrt(inE / l.size());
        j["outRms"] = std::sqrt(outE / l.size());
        j["finite"] = finite;
        if (!finite) { ok = false; e = "put out numbers that are not numbers"; }
        std::string st = p->saveState();
        j["stateChars"] = (int)st.size();
        j["stateRestores"] = p->loadState(st);
        p->deactivate();
    }
    j["ok"] = ok;
    if (!ok) j["error"] = e;
}

static int cmdAudioProbe(const std::vector<std::string>& args)
{
    std::string out;
    std::vector<std::string> files;
    bool describeOnly = false;
    for (size_t k = 0; k < args.size(); ++k) {
        if (args[k] == "--out" && k + 1 < args.size()) out = args[++k];
        else if (args[k] == "--describe-only") describeOnly = true;
        else files.push_back(args[k]);
    }
    Json all = Json::array();
    for (const auto& path : files) {
        Json rep = Json::object();
        rep["path"] = path;
        std::string err;
        std::vector<AudioPluginDesc> descs = describeAudioPluginFile(path, &err);
        if (descs.empty()) {
            rep["ok"] = false;
            rep["error"] = err;
        } else {
            rep["ok"] = true;
            Json arr = Json::array();
            for (const auto& d : descs) {
                Json j = d.toJson();
                if (describeOnly) j["ok"] = true;
                else tryAudioPlugin(d, j);
                arr.push(j);
            }
            rep["plugins"] = arr;
        }
        all.push(rep);
        emit(all, out);
    }
    emit(all, out);
    return 0;
}

// Without a uid: lists the registered DirectX audio effects. With one: tries that effect.
static int cmdDxProbe(const std::vector<std::string>& args)
{
    std::string out, uid;
    for (size_t k = 0; k < args.size(); ++k) {
        if (args[k] == "--out" && k + 1 < args.size()) out = args[++k];
        else uid = args[k];
    }
#ifdef _WIN32
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
#endif
    Json all = Json::array();
    if (uid.empty()) {
        for (const auto& d : listDirectXAudioPlugins()) all.push(d.toJson());
    } else {
        AudioPluginDesc d;
        d.format = "DX";
        d.uid = uid;
        Json j = d.toJson();
        tryAudioPlugin(d, j);
        all.push(j);
    }
    emit(all, out);
    return 0;
}

// ---------------------------------------------------------------------------------------------- everything
static Json runChildProbe(const std::vector<std::string>& args, const std::string& outFile, int timeout, std::string* problem)
{
    writeFile(outFile, "");
    std::vector<std::string> a = args;
    a.insert(a.begin() + 1, outFile);
    a.insert(a.begin() + 1, "--out");
    ProcessResult r = runProcess(selfExePath(), a, timeout);
    std::string text;
    readFile(outFile, text);
    Json j;
    if (!text.empty()) Json::parse(text, j);
    if (!r.started || r.timedOut || r.exitCode != 0) *problem = r.describe();
    return j;
}

static std::string shortContext(const std::string& c)
{
    const std::string pre = "OfxImageEffectContext";
    return toLower(c.compare(0, pre.size(), pre) == 0 ? c.substr(pre.size()) : c);
}

// Collects what "reno_cli check" writes: report text (also printed), JSON and counts.
struct CheckRun {
    std::string outDir, tmp;
    std::string report;
    Json full = Json::object();
    int videoOk = 0, videoBad = 0, audioOk = 0, audioBad = 0, dxOk = 0, dxBad = 0;

    // Adds a report line; the file is rewritten each time so a crash keeps the progress.
    void line(const std::string& s)
    {
        report += s + "\r\n";
        std::printf("%s\n", s.c_str());
        std::fflush(stdout);
        writeFile(pathJoin(outDir, "plugin_report.txt"), report);
    }
};

static void checkVideoPlugins(CheckRun& c)
{
    c.line("==== Video effects (OpenFX) ====");
    std::vector<std::string> dirs = ofx::Host::defaultSearchDirs();
    for (const auto& d : dirs) c.line("looked in: " + d);
    Json files = Json::array();
    for (const auto& f : ofx::Host::findBinaries(dirs)) {
        c.line("");
        c.line(f);
        std::string problem;
        Json j = runChildProbe({"ofxprobe", f}, c.tmp, 300, &problem);
        Json rep = j.at(0);
        bool anyBad = !problem.empty() || !rep.get("ok").asBool();
        for (const auto& p : rep.get("plugins").items()) if (!p.get("ok").asBool()) anyBad = true;
        std::string how;
        if (anyBad) {
            // retry with stand-ins for the VEGAS suites Reno lacks; some effects only need them to exist
            std::string problem2;
            Json j2 = runChildProbe({"ofxprobe", "--stubs", f}, c.tmp, 300, &problem2);
            Json rep2 = j2.at(0);
            int ok1 = 0, ok2 = 0;
            for (const auto& p : rep.get("plugins").items()) if (p.get("ok").asBool()) ++ok1;
            for (const auto& p : rep2.get("plugins").items()) if (p.get("ok").asBool()) ++ok2;
            if (ok2 > ok1) {
                rep = rep2;
                problem = problem2;
                how = " (only with stand-ins for missing VEGAS suites)";
                rep["neededStubs"] = true;
            }
        }
        if (!rep.get("ok").asBool() && rep.get("plugins").size() == 0) {
            ++c.videoBad;
            c.line("  FAILED  " + (rep.has("error") ? rep.get("error").asString() : (problem.empty() ? std::string("nothing came back") : problem)));
        }
        std::vector<std::string> missing;
        for (const auto& p : rep.get("plugins").items()) {
            const bool ok = p.get("ok").asBool();
            if (ok) ++c.videoOk; else ++c.videoBad;
            std::string s = strfmt("  %s  %s  [%s]", ok ? "ok    " : "FAILED", p.get("label").asString().c_str(), p.get("id").asString().c_str());
            if (ok) {
                s += strfmt("  %s, %d settings", shortContext(p.get("usedAs").asString()).c_str(), p.get("params").asInt());
                if (p.has("msPerFrame")) s += strfmt(", %.1f ms a frame", p.get("msPerFrame").asDouble());
                if (p.has("changedPicture") && !p.get("changedPicture").asBool()) s += ", left the picture as it was";
                if (p.get("ownWindow").asBool()) s += ", has a window of its own";
                s += how;
            } else {
                s += "  " + p.get("error").asString();
            }
            c.line(s);
            for (const auto& su : p.get("suites").items()) {
                const std::string& t = su.asString();
                if (t.find("not available") != std::string::npos || t.find("stand-in") != std::string::npos)
                    if (std::find(missing.begin(), missing.end(), t) == missing.end()) missing.push_back(t);
            }
        }
        if (!problem.empty()) {
            c.line("  the checking process " + problem + (rep.get("plugins").size() ? " - effects after the last one listed were not reached" : ""));
            if (rep.get("plugins").size() != 0) ++c.videoBad;
        }
        for (const auto& m : missing) c.line("  asked for: " + m);
        rep["processProblem"] = problem;
        files.push(rep);
    }
    c.full["ofx"] = files;
    c.line("");
}

static void checkAudioPlugins(CheckRun& c)
{
    c.line("==== Audio effects (VST) ====");
    std::vector<std::string> dirs = audioPluginSearchDirs();
    for (const auto& d : dirs) c.line("looked in: " + d);
    c.line("");
    Json files = Json::array();
    for (const auto& f : findAudioPluginFiles(dirs)) {
        std::string problem;
        Json j = runChildProbe({"audioprobe", f}, c.tmp, 120, &problem);
        Json rep = j.at(0);
        if (rep.isNull()) { rep = Json::object(); rep["path"] = f; }
        if (rep.get("plugins").size() == 0) {
            const std::string err = rep.get("error").asString();
            if (problem.empty() && err == "is not a VST2 plug-in") continue;           // not a plug-in, just another DLL in the folder
            ++c.audioBad;
            c.line(strfmt("  FAILED  %s  %s", f.c_str(), problem.empty() ? err.c_str() : ("the checking process " + problem).c_str()));
        }
        for (const auto& p : rep.get("plugins").items()) {
            const bool ok = p.get("ok").asBool() && problem.empty();
            if (ok) ++c.audioOk; else ++c.audioBad;
            std::string s = strfmt("  %s  %s %s  (%s)", ok ? "ok    " : "FAILED", p.get("format").asString().c_str(), p.get("name").asString().c_str(), pathFileName(f).c_str());
            if (ok) s += strfmt("  %d in, %d out, %d settings%s", p.get("inputs").asInt(), p.get("outputs").asInt(), p.get("params").asInt(), p.get("editor").asBool() ? ", has a window" : "");
            else s += "  " + (problem.empty() ? p.get("error").asString() : "the checking process " + problem);
            c.line(s);
        }
        rep["processProblem"] = problem;
        files.push(rep);
    }
    c.full["audio"] = files;
    c.line("");
}

static void checkDirectXPlugins(CheckRun& c)
{
    c.line("==== Audio effects (DirectX) ====");
    std::string problem;
    Json list = runChildProbe({"dxprobe"}, c.tmp, 120, &problem);
    if (!problem.empty()) c.line("  listing them: the checking process " + problem);
    if (list.size() == 0 && problem.empty()) c.line("  none found");
    Json items = Json::array();
    for (const auto& d : list.items()) {
        std::string p2;
        Json j = runChildProbe({"dxprobe", d.get("uid").asString()}, c.tmp, 60, &p2);
        Json rep = j.at(0);
        if (rep.isNull()) rep = d;
        const bool ok = rep.get("ok").asBool() && p2.empty();
        if (ok) ++c.dxOk; else ++c.dxBad;
        std::string s = strfmt("  %s  %s", ok ? "ok    " : "FAILED", d.get("name").asString().c_str());
        if (ok) s += strfmt("  %d settings%s", rep.get("params").asInt(), rep.get("editor").asBool() ? ", has a window" : "");
        else s += "  " + (p2.empty() ? rep.get("error").asString() : "the checking process " + p2);
        c.line(s);
        rep["processProblem"] = p2;
        items.push(rep);
    }
    c.full["dx"] = items;
    c.line("");
}

static int cmdCheck(const std::vector<std::string>& args)
{
    CheckRun c;
    c.outDir = ".";
    bool doVideo = true, doAudio = true, doDx = true;
    for (size_t k = 0; k < args.size(); ++k) {
        if (args[k] == "--out-dir" && k + 1 < args.size()) c.outDir = args[++k];
        else if (args[k] == "--no-video") doVideo = false;
        else if (args[k] == "--no-audio") doAudio = false;
        else if (args[k] == "--no-dx") doDx = false;
    }
    c.tmp = pathJoin(c.outDir, "plugin_check_tmp.json");
    std::time_t now = std::time(nullptr);
    char when[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M", std::localtime(&now));
    c.line(strfmt("Reno plug-in check, %s", when));
    c.line("Each plug-in file is tried in a process of its own: loaded, described, one of each effect made,");
    c.line("and a few frames (or a second of sound) put through it.");
    c.line("");

    if (doVideo) checkVideoPlugins(c);
    if (doAudio) checkAudioPlugins(c);
    if (doDx) checkDirectXPlugins(c);

    c.line("==== Summary ====");
    if (doVideo) c.line(strfmt("video effects (OpenFX):   %d work, %d do not", c.videoOk, c.videoBad));
    if (doAudio) c.line(strfmt("audio effects (VST):      %d work, %d do not", c.audioOk, c.audioBad));
    if (doDx) c.line(strfmt("audio effects (DirectX):  %d work, %d do not", c.dxOk, c.dxBad));
    writeFile(pathJoin(c.outDir, "plugin_report.json"), c.full.dump(1));
    writeFile(c.tmp, "");
    return 0;
}

// reno_cli vegasfx: VEGAS's own effects against Reno's built-in versions (src/ofx/fxcompare.cpp)
static int cmdVegasFx(const std::vector<std::string>& args)
{
    ofx::CompareOptions o;
    for (size_t k = 0; k < args.size(); ++k) {
        const std::string& a = args[k];
        auto need = [&](std::string& v) {
            if (k + 1 >= args.size()) { std::fprintf(stderr, "reno_cli vegasfx: %s needs a value\n", a.c_str()); return false; }
            v = args[++k];
            return true;
        };
        std::string v;
        if (a == "--out-dir") { if (!need(o.outDir)) return 2; }
        else if (a == "--size") {
            if (!need(v)) return 2;
            if (std::sscanf(v.c_str(), "%dx%d", &o.width, &o.height) != 2 || o.width < 16 || o.height < 16 || o.width > 4096 || o.height > 4096) {
                std::fprintf(stderr, "reno_cli vegasfx: --size wants WIDTHxHEIGHT (16 to 4096), e.g. 640x360\n");
                return 2;
            }
        }
        else if (a == "--frames") {
            if (!need(v)) return 2;
            o.frames = std::atoi(v.c_str());
            if (o.frames < 1 || o.frames > 30) { std::fprintf(stderr, "reno_cli vegasfx: --frames wants 1 to 30\n"); return 2; }
        }
        else if (a == "--only") { if (!need(o.only)) return 2; }
        else if (a == "--plugin-path") { if (!need(v)) return 2; o.pluginPaths.push_back(v); }
        else if (a == "--max-params") { if (!need(v)) return 2; o.maxParams = std::max(0, std::atoi(v.c_str())); }
        else if (a == "--timeout") { if (!need(v)) return 2; o.timeoutSec = std::max(10, std::atoi(v.c_str())); }
        else if (a == "--resources") { if (!need(v)) return 2; o.resourceDirs.push_back(v); }
        else if (a == "--params-only") o.paramsOnly = true;
        else if (a == "--quick") o.quick = true;
        else if (a == "--keep-real") o.keepReal = true;
        else if (a == "--stubs") o.stubs = true;
        else if (a == "--all") o.all = true;
        else if (a == "--gpu") {
            if (!need(v)) return 2;
            if (v != "on" && v != "off") { std::fprintf(stderr, "reno_cli vegasfx: --gpu wants on or off\n"); return 2; }
            o.gpu = v == "on";
        }
        else { std::fprintf(stderr, "reno_cli vegasfx: unknown option %s\n", a.c_str()); return 2; }
    }
    return ofx::runVegasFxReport(o);
}

bool reno::isPluginCheckCommand(const std::string& cmd)
{
    return cmd == "check" || cmd == "ofxprobe" || cmd == "audioprobe" || cmd == "dxprobe" || cmd == "vegasfx" || cmd == "vegasfxrun" || cmd == "vegasfxlist";
}

int reno::runPluginCheckCommand(const std::string& cmd, const std::vector<std::string>& args)
{
    if (cmd == "check") return cmdCheck(args);
    if (cmd == "vegasfx") return cmdVegasFx(args);
    quietCrashes();
    if (cmd == "vegasfxrun" || cmd == "vegasfxlist") return ofx::runVegasFxChild(cmd, args);
    if (cmd == "ofxprobe") return cmdOfxProbe(args);
    if (cmd == "audioprobe") return cmdAudioProbe(args);
    if (cmd == "dxprobe") return cmdDxProbe(args);
    std::fprintf(stderr, "reno_cli: unknown command '%s'\n", cmd.c_str());
    return 2;
}
