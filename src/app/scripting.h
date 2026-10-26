// Tools > Scripting: runs VEGAS scripts (.cs, .js, .vb, compiled .dll) and builds the Script Menu.
//
// A script runs against Reno's ScriptPortal.Vegas (src/scripting/ScriptPortal.Vegas.cs), compiled once with the .NET
// Framework's C# compiler and cached. The framework is hosted inside Reno, as in VEGAS, and the script edits a copy
// of the project through a ScriptSession (src/engine/scriptsession.h). When it ends the copy becomes the project as
// one undo step.
//
// The .NET Framework is Windows-only; elsewhere Run Script says so.
#pragma once
#include <string>
#include <vector>

class QMenu;
class QWidget;

namespace reno {
namespace app {

// The folders VEGAS takes its Script Menu from that exist here: Documents\Vegas Script Menu, VEGAS's own
// Script Menu folders (installation and user app data) and Reno's example scripts.
std::vector<std::string> scriptMenuFolders();

// The scripts in those folders (.cs .js .vb .dll; subfolders as "Sub folder/Name"), sorted by name.
struct ScriptMenuItem {
    std::string name;  // as shown ("My Script", "My scripts/Chop")
    std::string path;
};
std::vector<ScriptMenuItem> scanScriptMenu();

// Puts the scripts after the menu's first actions (Run Script..., Rescan Script Menu Folder), replacing the
// ones from an earlier scan.
void fillScriptMenu(QMenu* menu, QWidget* parent);

void runScript(QWidget* parent, const std::string& path, const std::string& args = std::string());

void runScriptDialog(QWidget* parent);

// VEGAS's command-line options: /NOLOGO, /OPEN <file>, /RUNSCRIPT or /SCRIPT <script>, /SCRIPTARGS <arguments>,
// /CMDMODULE <extension>, /OPENPRJ <rendered file>, /COLORS 0|1. Case doesn't matter and "-" works like "/".
struct CommandLine {
    bool noLogo = false;
    std::string open, openPrj, script, scriptArgs, cmdModule;
    int colors = -1;  // 1 VEGAS's colors, 0 the system's, -1 not given
};
CommandLine parseCommandLine(const std::vector<std::string>& args);  // args[0] is the program

}  // namespace app
}  // namespace reno
