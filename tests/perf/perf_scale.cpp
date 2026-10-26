// perf_scale: times what a huge project costs the model itself (no interface): reading and writing the files, copying
// the project (every edit and undo step does), looking events up, and the memory it takes. Pairs with gen_big and
// ui_scale_test.
//   perf_scale project.veg|project.reno ...
// Prints "SCALE <project> <measure> <value> <unit>".
#include "../../src/engine/engine.h"
#include "../../src/vegas/vegas.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <algorithm>
#include <unistd.h>
#include "sampler.h"

using namespace reno;

static double cpuMs()
{
    timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}
static double wallMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static double rssMb()
{
    FILE* f = std::fopen("/proc/self/statm", "r");
    long a = 0, b = 0;
    if (f) { if (std::fscanf(f, "%ld %ld", &a, &b) != 2) b = 0; std::fclose(f); }
    return (double)b * (double)sysconf(_SC_PAGESIZE) / 1048576.0;
}

static std::string gName;
static void out(const char* what, double v, const char* unit)
{
    std::printf("SCALE %-18s %-34s %12.3f %s\n", gName.c_str(), what, v, unit);
    std::fflush(stdout);
}

int main(int argc, char** argv)
{
    setProjectFileLoader([](const std::string& path, Project& o, std::string* error) { return loadProjectFile(path, o, error); });
    for (int a = 1; a < argc; ++a) {
        const std::string path = argv[a];
        gName = pathFileName(path);
        double r0 = rssMb();
        double c0 = cpuMs();
        auto p = std::make_shared<Project>();
        std::string err;
        const bool profLoad = std::getenv("RENO_PROF") && std::strstr(std::getenv("RENO_PROF"), "load");
        const bool profSave = std::getenv("RENO_PROF") && std::strstr(std::getenv("RENO_PROF"), "save");
        {
            prof::Scope ps("load", profLoad);
            if (!loadProjectFile(path, *p, &err)) { std::fprintf(stderr, "%s: %s\n", path.c_str(), err.c_str()); continue; }
        }
        out("load cpu", cpuMs() - c0, "ms");
        out("load memory", rssMb() - r0, "MB");
        size_t events = 0;
        for (const Track& t : p->tracks) events += t.events.size();
        out("events", (double)events, "");
        out("sizeof Event", (double)sizeof(Event), "bytes");

        // copying the project: every edit, undo step and snapshot does it
        {
            double t0 = cpuMs();
            const int n = 5;
            for (int k = 0; k < n; ++k) { Project c = *p; (void)c; }
            out("project copy", (cpuMs() - t0) / n, "ms");
            r0 = rssMb();
            std::vector<Project> keep;
            for (int k = 0; k < 20; ++k) keep.push_back(*p);
            out("memory per project copy kept", (rssMb() - r0) / 20, "MB");
            // (one event changed in each copy: what 20 undo steps of single-event edits hold)
            keep.clear();
        }
        // looking events up
        {
            std::vector<uint64_t> ids;
            for (const Track& t : p->tracks) for (const Event& e : t.events) ids.push_back(e.id);
            std::mt19937 g(3);
            const int n = 300;
            double t0 = cpuMs();
            size_t hit = 0;
            for (int k = 0; k < n; ++k) hit += p->findEvent(ids[g() % ids.size()]) != nullptr;
            out("findEvent", (cpuMs() - t0) / n * 1000.0, "us");
            t0 = cpuMs();
            double len = 0;
            for (int k = 0; k < 20; ++k) len += p->length();
            out("Project::length", (cpuMs() - t0) / 20 * 1000.0, "us");
            (void)hit; (void)len;
        }
        // files
        {
            double t0 = cpuMs();
            const Json j = p->toJson();
            out("toJson", cpuMs() - t0, "ms");
            t0 = cpuMs();
            const std::string text = j.dump();
            out("Json dump", cpuMs() - t0, "ms");
            t0 = cpuMs();
            Json back;
            Json::parse(text, back);
            out("Json parse", cpuMs() - t0, "ms");
            t0 = cpuMs();
            Project q;
            q.fromJson(back);
            out("fromJson", cpuMs() - t0, "ms");
            t0 = cpuMs();
            const std::string tmp = "/tmp/perf_scale_tmp.reno";
            p->save(tmp, &err);
            out("save .reno", cpuMs() - t0, "ms");
            std::remove(tmp.c_str());
            t0 = cpuMs();
            std::vector<std::string> warn;
            std::string data;
            {
                prof::Scope ps("write .veg", profSave);
                data = writeVegasData(*p, "/tmp/perf_scale_tmp.veg", &warn);
            }
            out("write .veg bytes", cpuMs() - t0, "ms");
            out(".veg size", (double)data.size() / 1048576.0, "MB");
            const std::string tv = "/tmp/perf_scale_tmp.veg";
            t0 = cpuMs();
            Project copy = *p;
            VegasSaveResult sr = saveVegasProject(copy, tv);
            out("save .veg (to disk)", cpuMs() - t0, "ms");
            std::remove(tv.c_str());
            std::remove((tv + ".bak").c_str());
        }
        // the engine's own copy: a render thread takes a snapshot
        {
            double t0 = cpuMs();
            for (int k = 0; k < 5; ++k) { auto s = std::make_shared<const Project>(*p); (void)s; }
            out("snapshot make", (cpuMs() - t0) / 5, "ms");
        }
        out("rss total", rssMb(), "MB");
    }
    return 0;
}
