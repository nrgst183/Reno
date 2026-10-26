// reno_cli's plug-in checking commands: check, ofxprobe, audioprobe, dxprobe (plugincheck.cpp).
#pragma once
#include <string>
#include <vector>

namespace reno {

// True if cmd (the first argument) is one of the plug-in checking commands.
bool isPluginCheckCommand(const std::string& cmd);
// Runs the command with its remaining arguments; returns the exit code.
int runPluginCheckCommand(const std::string& cmd, const std::vector<std::string>& args);

}  // namespace reno
