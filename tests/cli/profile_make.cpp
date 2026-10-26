// Makes a small .reno project for the preview profiler's test (profile_test.sh): a video file on one track with the test
// plug-in's Test Invert on the event, and the same file on a second track with it on the track.
//   profile_make <video file> <out.reno>
#include "../../src/engine/project.h"
#include "../../src/media/media.h"
#include <cstdio>

using namespace reno;

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::printf("usage: profile_make <video file> <out.reno>\n");
        return 2;
    }
    std::string error;
    std::unique_ptr<MediaReader> r = openMedia(argv[1], &error);
    if (!r) {
        std::printf("cannot open %s: %s\n", argv[1], error.c_str());
        return 1;
    }
    Project p;
    p.settings.width = 640;
    p.settings.height = 360;
    p.settings.fpsNum = 30;
    p.settings.fpsDen = 1;
    MediaItem m;
    m.id = p.newId();
    m.path = argv[1];
    m.name = "clip";
    m.info = r->info();
    p.media.push_back(m);
    for (int k = 0; k < 2; ++k) {
        Track t;
        t.id = p.newId();
        t.type = TrackType::Video;
        Event e;
        e.id = p.newId();
        e.mediaId = m.id;
        e.start = 0;
        e.length = std::min(10.0, m.info.duration);
        FxSlot s;
        s.id = p.newId();
        s.kind = "ofx";
        s.pluginId = "reno.test.invert";
        s.name = "Test Invert";
        (k ? t.fx : e.fx).push_back(s);
        t.events.push_back(e);
        p.tracks.push_back(t);
    }
    if (!p.save(argv[2], &error)) {
        std::printf("cannot save: %s\n", error.c_str());
        return 1;
    }
    return 0;
}
