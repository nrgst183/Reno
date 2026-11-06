// "reno_cli model": prints a project as JSON in the schema tools/vegas/RenoCoverage.cs writes from VEGAS's own object
// model, so tools/vegcov_compare.py can compare what VEGAS says a .veg holds with what Reno reads from it.
// No Qt and no window: only the project reader.
#pragma once
#include "../engine/project.h"
#include <string>
#include <vector>

namespace reno {

// The project in the model schema ("reno-model/1"; the field list is in tools/vegcov_compare.py).
// Times are in VEGAS's Timecode.Nanos (100 ns units), named ...Nanos.
Json projectModelJson(const Project& p);

// reno_cli model <project.veg|.reno> [-o out.json] [--resave out.veg] [--notes]
//   -o        write the JSON there instead of standard output
//   --resave  also save the project as a .veg with Reno's writer (VEGAS can then describe it: the round trip)
//   --notes   print what Reno cannot use of the file (to standard error)
int runModelCommand(const std::vector<std::string>& args);

}  // namespace reno
