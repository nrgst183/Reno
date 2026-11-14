// reno_cli's project commands: making and changing a project from the command line (new, edit, info, fx-list,
// fx-params). See projectcli.cpp and the help text (projectCliHelp).
#pragma once
#include <string>
#include <vector>

namespace reno {
namespace app {
class AppState;
}

// Whether 'cmd' (reno_cli's first argument) is one of the project commands.
bool isProjectCliCommand(const std::string& cmd);
// Runs one: args[0] is the command. Returns reno_cli's exit code (0 done, 1 a command failed, 2 a bad command line,
// 3 the project could not be opened or saved, 5 the file exists and -y was not given).
int runProjectCli(const std::vector<std::string>& args, app::AppState& app, bool quiet);
// The help for them (part of reno_cli --help).
extern const char* kProjectCliHelp;

}  // namespace reno
