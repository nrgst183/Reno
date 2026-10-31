// vegmediaorder_test: the media pool keeps its order through save -> load -> save -> load of a .veg.
//   vegmediaorder_test [folder with .veg files] [--scratch folder]    (default $RENO_SAMPLES; only read)
// 1. A project made here (files used in a different order than the pool lists them) is written and read twice.
// 2. Every sample project is read, its media list is changed the way a user could (reversed, an item moved to the front,
//    a second item for the same file) and then written and read twice: the second read must list the same media as the first.
#include "../../src/vegas/vegas.h"
#include "../../src/engine/project.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include "../testdata.h"

using namespace reno;
namespace fs = std::filesystem;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what, const std::string& detail = std::string())
{
    if (ok) ++g_pass;
    else { ++g_fail; std::printf("FAIL %s%s%s\n", what.c_str(), detail.empty() ? "" : " - ", detail.c_str()); std::fflush(stdout); }
}

static std::string sig(const Project& p)
{
    std::string s;
    for (const auto& m : p.media) s += m.path + "|" + m.name + "|" + (m.generated ? "g" : "f") + "\n";
    return s;
}

// writes q, reads it back twice (with a save in between) and compares the two media lists
static void roundTrip(Project q0, const std::string& scratch, const std::string& what)
{
    Project q1, q2;
    std::string e;
    const std::string a = scratch + "/mo_a.veg", b = scratch + "/mo_b.veg";
    if (!saveVegasProject(q0, a).ok || !loadProjectFile(a, q1, &e)) { check(false, what + ": first save and load"); return; }
    if (!saveVegasProject(q1, b).ok || !loadProjectFile(b, q2, &e)) { check(false, what + ": second save and load"); return; }
    check(sig(q1) == sig(q2), what + ": the media list is the same after a second save and load", "first:\n" + sig(q1) + "second:\n" + sig(q2));
}

int main(int argc, char** argv)
{
    std::string folder = testDataDir("RENO_SAMPLES"), scratch = (fs::temp_directory_path() / "vegmediaorder_test").string();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--scratch" && i + 1 < argc) scratch = argv[++i];
        else folder = a;
    }
    std::error_code ec;
    fs::create_directories(scratch, ec);
    {
        Project p;
        const char* names[] = {"C:\\m\\one.mp4", "C:\\m\\two.wav", "C:\\m\\three.mp4", "C:\\m\\four.wav"};
        for (int i = 0; i < 4; ++i) {
            MediaItem m;
            m.id = p.newId();
            m.path = names[i];
            m.name = fs::path(names[i]).filename().string();
            m.info.path = m.path;
            m.info.hasVideo = i % 2 == 0;
            m.info.hasAudio = true;
            m.info.duration = 10;
            m.info.width = 640;
            m.info.height = 360;
            m.info.fps = 30;
            m.info.sampleRate = 48000;
            m.info.channels = 2;
            p.media.push_back(m);
        }
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        const int use[] = {3, 1, 2, 0};
        double at = 0;
        for (int k : use) {
            Event e;
            e.id = p.newId();
            e.mediaId = p.media[(size_t)k].id;
            e.start = at;
            e.length = 2;
            at += 2;
            t.events.push_back(e);
        }
        p.tracks.push_back(t);
        roundTrip(p, scratch, "made here");
    }
    int n = 0;
    if (!folder.empty() && fs::exists(folder)) {
        std::vector<std::string> files;
        for (auto it = fs::recursive_directory_iterator(folder, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file() && it->path().extension() == ".veg") files.push_back(it->path().string());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            Project q;
            std::string e;
            if (!loadProjectFile(f, q, &e)) continue;
            const std::string name = fs::path(f).filename().string();
            roundTrip(q, scratch, name + " as read");
            if (q.media.size() > 2) {
                Project r = q;
                std::reverse(r.media.begin(), r.media.end());
                roundTrip(r, scratch, name + " media reversed");
                Project s = q;
                std::swap(s.media.front(), s.media.back());
                roundTrip(s, scratch, name + " first and last swapped");
                Project d = q;
                MediaItem c = d.media[1];
                c.id = d.newId();
                d.media.insert(d.media.begin(), c);
                roundTrip(d, scratch, name + " an item for the same file added in front");
                Project m3 = q;
                std::swap(m3.media[0], m3.media.back());
                MediaItem c3 = m3.media[1];
                c3.id = m3.newId();
                c3.veg = nullptr;
                m3.media.insert(m3.media.begin(), c3);
                roundTrip(m3, scratch, name + " first and last swapped, a copy of the second added in front");
                for (size_t k = 0; k < q.media.size(); ++k) {
                    Project a2 = q;
                    MediaItem c2 = a2.media[k];
                    c2.id = a2.newId();
                    a2.media.push_back(c2);
                    roundTrip(a2, scratch, name + " item " + std::to_string(k) + " copied to the end");
                }
            }
            ++n;
        }
    }
    std::printf("%d sample projects\nSUMMARY: %d passed, %d failed\n", n, g_pass, g_fail);
    return g_fail ? 1 : 0;
}
