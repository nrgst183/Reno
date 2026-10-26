// The effect log: fx-log.txt beside the program, written whenever a project is opened.
#pragma once
#include "fxruntime.h"
#include "project.h"
#include <string>

namespace reno {

// The log's text for project 'p' (opened from 'projectPath'): 'rt' says which effects have a live instance and why
// the others have none (call it after rt.prepare(p)).
std::string fxLogText(const Project& p, const FxRuntime& rt, const std::string& projectPath);

// Writes fxLogText to fx-log.txt in 'dir' (the folder of the running program when empty), replacing the last one.
bool writeFxLog(const Project& p, const FxRuntime& rt, const std::string& projectPath, const std::string& dir = std::string());

}  // namespace reno
