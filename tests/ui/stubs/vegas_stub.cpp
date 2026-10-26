// Stand-in for src/vegas/*.cpp.
#include "../../../src/vegas/vegas.h"
namespace reno {
VegasImportResult importVegasProject(const std::string&, Project&) { VegasImportResult r; r.error = "VEGAS projects cannot be read in this build"; return r; }
bool isVegasProject(const std::string&) { return false; }
bool loadProjectFile(const std::string& path, Project& out, std::string* error) { return out.load(path, error); }
bool loadProjectFileWithNotes(const std::string& path, Project& out, std::string* error, std::vector<std::string>* notes)
{
    if (notes) notes->clear();
    return out.load(path, error);
}
bool vegasProjectMediaInfo(const std::string&, MediaInfo&, std::string* error) { if (error) *error = "not in this build"; return false; }
VegasSaveResult saveVegasProject(Project&, const std::string&) { VegasSaveResult r; r.error = "VEGAS projects cannot be written in this build"; return r; }
std::string writeVegasData(const Project&, const std::string&, std::vector<std::string>*) { return std::string(); }
}
