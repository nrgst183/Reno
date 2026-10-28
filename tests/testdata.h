// Where the tests find data that is not in the repository. Each folder comes from an environment variable (or the
// test's own command-line argument, which wins). When the variable is not set the function returns the text
// "(NAME not set)": no such folder exists, so the test SKIPs and its SKIP line says what to set.
//
//   RENO_SAMPLES      folder with sample VEGAS projects (.veg, searched recursively) and their media
//   RENO_CMP          folder with what the VEGAS bridge scripts wrote (tools/vegas/bridge_scripts): "info", "shorttests", ...
//   RENO_TEST_PLUGIN  the test OpenFX plug-in file (effects.ofx) the host tests load besides their own
//   RENO_TEST_VST_DIR folder with test VST plug-ins
//   RENO_VEGAS_DIR    a VEGAS Pro 18 program folder
//   RENO_SAMPLE_VEG   one particular .veg file for the tests that look at a single project (the CLI test, reno_cli_test.sh, uses it too)
//   RENO_VEGAS_FILES  VEGAS's own peak files and audio proxies for vegasfiles_test / peakfiles_test (peaks_a.wav + .sfk, peaks_b.wav + .sfk, nested.veg.sfap0)
#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <map>
#include <vector>
#include <algorithm>

inline std::string testDataDir(const char* variable)
{
    const char* v = std::getenv(variable);
    return v && *v ? std::string(v) : std::string("(") + variable + " not set)";
}

// True (after printing a SKIP line) when the folder or file is not there: "if (testDataMissing(dir)) return 0;".
inline bool testDataMissing(const std::string& path)
{
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return false;
    std::printf("SKIP: test data not found: %s (name it on the command line or set the environment variable)\n", path.c_str());
    return true;
}

// Every .veg file under a folder (recursively), sorted; empty when the folder is not there. For tests that run over
// "the sample projects" without naming any of them.
inline std::vector<std::string> testVegFiles(const std::string& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return out;
    for (std::filesystem::recursive_directory_iterator it(dir, std::filesystem::directory_options::skip_permission_denied | std::filesystem::directory_options::follow_directory_symlink, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".veg") out.push_back(it->path().string());
    std::sort(out.begin(), out.end());
    return out;
}

// One particular sample project: the test's command-line argument if it has one, else $RENO_SAMPLE_VEG; "" when neither.
inline std::string testSampleVeg(const char* arg = nullptr)
{
    if (arg && *arg) return arg;
    const char* v = std::getenv("RENO_SAMPLE_VEG");
    return v && *v ? std::string(v) : std::string();
}

// A comparison key for a name: lower case, every run of other characters one '_' ("Some File 1.veg" and "some_file_1" agree).
inline std::string testKey(const std::string& name)
{
    std::string k;
    for (char ch : name) {
        const bool alnum = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
        if (alnum) k += (char)(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        else if (!k.empty() && k.back() != '_') k += '_';
    }
    while (!k.empty() && k.back() == '_') k.pop_back();
    return k;
}

// The .veg files under a folder by testKey of their name without ".veg" (the first, in path order, when two have one key).
inline std::map<std::string, std::string> testVegsByKey(const std::string& dir)
{
    std::map<std::string, std::string> out;
    for (const std::string& f : testVegFiles(dir)) {
        std::string name = std::filesystem::path(f).filename().string();
        name = name.substr(0, name.size() - 4);
        out.insert({testKey(name), f});
    }
    return out;
}
