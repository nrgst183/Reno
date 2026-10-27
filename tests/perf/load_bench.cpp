// load_bench: times reading .veg files (loadProjectFile) and prints a hash of the project it makes, so two builds can be
// compared. Lines: "LOAD <project> <ms (best of N)> ms <hash>".   load_bench [--runs N] project.veg...
#include "../../src/engine/project.h"
#include "../../src/vegas/vegas.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace reno;

int main(int argc, char** argv)
{
    int runs = 5;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--runs") && i + 1 < argc) runs = std::atoi(argv[++i]);
        else files.push_back(argv[i]);
    }
    for (const std::string& path : files) {
        double best = 1e30;
        uint64_t h = 1469598103934665603ull;
        bool ok = true;
        for (int r = 0; r < runs; ++r) {
            Project p;
            std::string err;
            const auto t0 = std::chrono::steady_clock::now();
            ok = loadProjectFile(path, p, &err);
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            if (!ok) break;
            if (r == 0) {
                const std::string s = p.toJson().dump();
                for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
            }
        }
        std::printf("LOAD %s %.2f ms %016llx%s\n", pathFileName(path).c_str(), best, (unsigned long long)h, ok ? "" : " (failed)");
    }
    return 0;
}
