// Effect presets and plug-in packages: what VEGAS keeps outside the project about effects.
//
// Presets are XML in the OfxPresetPackage format, as VEGAS keeps OpenFX presets:
//     <OfxPresetPackage>
//       <OfxPreset plugin="com.vegascreativesoftware:addnoise" context="Filter" name="Grainy">
//         <OfxPlugin>com.vegascreativesoftware:addnoise</OfxPlugin> <OfxPluginVersion>1 0</OfxPluginVersion>
//         <OfxParamTypeDouble name="Amount"><OfxParamValue>0.5</OfxParamValue></OfxParamTypeDouble> ...
// An effect's own presets are in its bundle (<x>.ofx.bundle/Contents/Presets/PresetPackage.xml). User presets are one <OfxPreset> per
// file: VEGAS's in Documents\OFX Presets (read) and Reno's in "OFX Presets\<plug-in>\<context>\<name>.xml" in Reno's
// folder (read and written).
//
// Packages (VEGAS's "filter packages") are effect chains with settings, kept as small JSON files in Reno's folder,
// "Plug-In Packages\<Video FX|Audio FX|...>". Reno does not read VEGAS's binary ones.
#pragma once
#include "../engine/project.h"
#include <functional>
#include <string>
#include <vector>

namespace reno {
namespace ofx {
class Instance;
}
namespace app {

struct OfxPreset {
    struct Value {
        std::string type;               // OpenFX parameter type: "OfxParamTypeDouble", "OfxParamTypeRGBA"...
        std::string name;
        std::vector<double> numbers;    // numbers
        std::string text;               // String / Custom
    };
    std::string name, pluginId;
    std::string context;                // "Filter", "Transition", "Generator", "General" (as VEGAS writes it)
    std::string version;                // "1 0"
    std::vector<Value> values;
    bool user = false;                  // the user's own (can be replaced and deleted)
    bool vegas = false;                 // one of VEGAS's own user presets (read only)
    std::string file;                   // source file
};

// Context word presets use for an instance's OpenFX context ("OfxImageEffectContextFilter" -> "Filter").
std::string presetContext(const std::string& ofxContext);

// Reads an OfxPresetPackage (or a single OfxPreset). Bad XML gives what was read before it.
std::vector<OfxPreset> parsePresets(const std::string& xml);
// Writes one preset as VEGAS does (an OfxPresetPackage holding it).
std::string writePreset(const OfxPreset& p);

// Presets for an effect in VEGAS's order: its own, then the user's by name. 'binaryPath' is the effect's file (its
// bundle's Presets folder is searched); empty: looked up in the catalog.
std::vector<OfxPreset> presetsFor(const std::string& pluginId, const std::string& context, const std::string& binaryPath = std::string());

// The effect's current settings as a preset (values at 'time', in frames).
OfxPreset capturePreset(ofx::Instance* inst, double time, const std::string& name);
// Puts a preset's values into the effect as if the user changed them at 'time' (keyframed settings get a keyframe).
// Settings the preset omits are untouched. Returns the count set.
int applyPreset(ofx::Instance* inst, const OfxPreset& p, double time);
// A preset as an effect slot's saved state (FxSlot::state, for ofx::Instance::loadState).
Json presetToState(const OfxPreset& p);
// The effect's default settings (VEGAS's "(Default)" entry).
int applyDefaults(ofx::Instance* inst, double time);

// Saves a user preset (replacing one of the same name). False with *error if it cannot be written or the name is one of
// the effect's own.
bool saveUserPreset(const OfxPreset& p, std::string* error);
bool deleteUserPreset(const OfxPreset& p, std::string* error);
// Where Reno keeps user presets and where VEGAS's are read from (tests point these elsewhere).
std::string userPresetDir();
void setUserPresetDir(const std::string& dir);
std::string vegasPresetDir();
void setVegasPresetDir(const std::string& dir);
// Forgets what was read (files may have changed).
void forgetPresets();

// ------------------------------------------------------------------------------------------ audio presets
// An audio effect's presets (VST / DirectX): the plug-in's own programs, plus the user's as JSON files "Audio
// Presets\<plug-in>\<name>.json"
// in Reno's folder holding the plug-in's saved state (AudioPlugin::saveState). .fxp / .fxb files are read and written
// from the effects window too.
struct AudioPreset {
    std::string name;
    std::string pluginKey;
    std::string state;
    std::string file;
};
std::vector<AudioPreset> audioPresetsFor(const std::string& pluginKey, const std::string& pluginName);
bool saveAudioPreset(const AudioPreset& p, const std::string& pluginName, std::string* error);
bool deleteAudioPreset(const AudioPreset& p, std::string* error);
std::string audioPresetDir();
void setAudioPresetDir(const std::string& dir);    // tests

// ------------------------------------------------------------------------------------------ packages
enum class PackageKind { VideoFx, AudioFx, Transition, Generator };
struct FxPackage {
    std::string name;
    PackageKind kind = PackageKind::VideoFx;
    FxChain chain;                      // slot ids are 0; new ids are given when used
    std::string file;
};
std::string packageDir(PackageKind kind);
void setPackageRoot(const std::string& dir);    // tests
std::vector<FxPackage> listPackages(PackageKind kind);
bool savePackage(const FxPackage& p, std::string* error);
bool deletePackage(const FxPackage& p, std::string* error);
// Called on the interface thread whenever a package is saved or deleted; returns an id for unregistering.
int onPackagesChanged(std::function<void()> fn);
void forgetPackagesListener(int id);
// A chain as package text and back.
std::string packageToText(const FxPackage& p);
bool packageFromText(const std::string& text, FxPackage& out);
// A name usable as a file name ("a/b:c" -> "a_b_c").
std::string safeFileName(const std::string& name);

}  // namespace app
}  // namespace reno
