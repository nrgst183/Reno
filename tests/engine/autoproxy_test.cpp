// Tests of the automatic preview proxies (src/app/autoproxy.h, assessVideoProxy in media/proxy.cpp): which files need
// a proxy, in what order, the worker (queueing, pausing during playback, cancel, retry, progress), and that renders
// never read proxies.
//
//   autoproxy_test <scratch folder>
#include "../../src/app/autoproxy.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

using namespace reno;
using namespace reno::app;
using namespace reno::app::autoproxy;

static int gPass = 0, gFail = 0;
static void check(bool ok, const std::string& name, const std::string& detail = std::string())
{
    if (ok) { ++gPass; std::printf("PASS %s\n", name.c_str()); }
    else { ++gFail; std::printf("FAIL %s%s%s\n", name.c_str(), detail.empty() ? "" : ": ", detail.c_str()); }
    std::fflush(stdout);
}

// A video of 'frames' pictures at 25 fps: the left half the colour (b, g, r), a white box that moves across the right half.
static bool makeVideo(const std::string& path, int w, int h, int frames, int b, int g, int r, double keyframeSeconds = 2.0, const char* family = "h264")
{
    EncodeSettings es;
    es.path = path;
    es.audio = false;
    es.width = w;
    es.height = h;
    es.fpsNum = 25;
    es.fpsDen = 1;
    es.videoFamily = family;
    es.encodeMode = "cpu";
    es.rateControl = "quality";
    es.quality = 18;
    es.keyframeSeconds = keyframeSeconds;
    es.bFrames = 0;
    std::string err;
    auto out = createMediaWriter(es, &err);
    if (!out) { std::printf("cannot write %s: %s\n", path.c_str(), err.c_str()); return false; }
    for (int f = 0; f < frames; ++f) {
        ImagePtr img = Image::make(w, h, PixelDepth::Byte, false);
        for (int y = 0; y < h; ++y) {
            uint8_t* d = img->row(y);
            for (int x = 0; x < w; ++x, d += 4) {
                const bool box = x >= w / 2 + f * 2 && x < w / 2 + f * 2 + w / 10 && y > h / 3 && y < 2 * h / 3;
                if (x < w / 2) { d[0] = (uint8_t)b; d[1] = (uint8_t)g; d[2] = (uint8_t)r; }
                else { const uint8_t v = box ? 255 : 0; d[0] = d[1] = d[2] = v; }
                d[3] = 255;
            }
        }
        if (!out->writeVideo(*img)) return false;
    }
    return out->finish(&err);
}

struct Px { int b, g, r; };
static Px at(const ImagePtr& img, double fx, double fy)
{
    if (!img) return {-1, -1, -1};
    const int x = std::min(img->w - 1, (int)(fx * img->w)), y = std::min(img->h - 1, (int)(fy * img->h));
    const uint8_t* d = img->row(img->h - 1 - y) + x * 4;
    return {d[0], d[1], d[2]};
}
static bool near(const Px& p, int b, int g, int r, int tol = 14) { return std::abs(p.b - b) <= tol && std::abs(p.g - g) <= tol && std::abs(p.r - r) <= tol; }

static uint64_t addMedia(Project& p, const std::string& path)
{
    std::string err;
    auto r = openMedia(path, &err);
    MediaItem m;
    m.id = p.newId();
    m.path = path;
    m.name = pathFileName(path);
    if (r) m.info = r->info();
    else std::printf("cannot open %s: %s\n", path.c_str(), err.c_str());
    p.media.push_back(m);
    return m.id;
}
static Track& addTrack(Project& p, TrackType type)
{
    Track t;
    t.id = p.newId();
    t.type = type;
    p.tracks.push_back(t);
    return p.tracks.back();
}
// 'n' events of 'len' seconds of the media, one after another
static void addEvents(Project& p, Track& t, uint64_t media, int n, double len, double from = 0)
{
    for (int k = 0; k < n; ++k) {
        Event e;
        e.id = p.newId();
        e.mediaId = media;
        e.start = from + k * len;
        e.length = len;
        e.offset = std::fmod(k * 0.7, 3.0);
        t.events.push_back(e);
    }
}
static std::string names(const std::vector<Candidate>& v)
{
    std::string s;
    for (const Candidate& c : v) s += (s.empty() ? "" : ", ") + pathFileName(c.path);
    return s;
}
static std::string names(const std::vector<std::string>& v)
{
    std::string s;
    for (const std::string& c : v) s += (s.empty() ? "" : ", ") + pathFileName(c);
    return s;
}
static bool waitFor(const std::function<bool()>& f, double seconds)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int64_t)(seconds * 1000));
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}
static std::string slurp(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main(int argc, char** argv)
{
    const std::string dir = argc > 1 ? argv[1] : "autoproxy_out";
    std::filesystem::create_directories(dir);
    auto P = [&](const char* n) { return dir + "/" + n; };
    bool madeAll = true;
    // long-GOP (a key frame every 2 s): A, B, C, D, J, K, I, H; short GOP and small: E; intra-frame: F
    madeAll &= makeVideo(P("A.mp4"), 320, 180, 100, 200, 40, 10);
    madeAll &= makeVideo(P("B.mp4"), 320, 180, 100, 10, 200, 10);
    madeAll &= makeVideo(P("C.mp4"), 320, 180, 100, 10, 10, 200);
    madeAll &= makeVideo(P("D.mp4"), 320, 180, 100, 100, 100, 10);
    madeAll &= makeVideo(P("J.mp4"), 320, 180, 100, 10, 100, 100);
    madeAll &= makeVideo(P("K.mp4"), 320, 180, 100, 100, 10, 100);
    madeAll &= makeVideo(P("I.mp4"), 320, 180, 100, 50, 50, 50);
    madeAll &= makeVideo(P("H.mp4"), 320, 180, 100, 150, 150, 150);
    madeAll &= makeVideo(P("E.mp4"), 320, 180, 100, 20, 200, 200, 0.2);
    madeAll &= makeVideo(P("F.mov"), 320, 180, 50, 200, 200, 20, 0.0, "mjpeg");
    if (!madeAll) {
        check(false, "the test videos can be made");
        std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
        return 1;
    }
    for (const char* n : {"A", "B", "C", "D", "J", "K", "I", "H", "E"}) std::filesystem::remove(videoProxyPath(P((std::string(n) + ".mp4").c_str())));
    std::filesystem::remove(videoProxyPath(P("F.mov")));

    // ---- what the file's index says
    {
        std::string err;
        auto a = openMedia(P("A.mp4"), &err), e = openMedia(P("E.mp4"), &err), f = openMedia(P("F.mov"), &err);
        const VideoProxyAssessment va = assessVideoProxy(P("A.mp4"), a->info()), ve = assessVideoProxy(P("E.mp4"), e->info()), vf = assessVideoProxy(P("F.mov"), f->info());
        check(va.worth && va.keyframes >= 2 && va.keyframeSeconds > 1.0, "a long-GOP file (key frame every 2 s) is worth a proxy", va.reason);
        check(!ve.worth && ve.keyframeSeconds > 0 && ve.keyframeSeconds < 0.6, "a small file with a key frame every 0.2 s is not", ve.reason + strfmt(" (%.2f s)", ve.keyframeSeconds));
        check(!vf.worth, "an intra-frame codec (" + f->info().videoCodec + ") is not", vf.reason);
        MediaInfo big = e->info();
        big.width = 1920;
        big.height = 1080;
        check(assessVideoProxy(P("E.mp4"), big).worth, "short GOP but larger than 960 x 540: worth it (decoding the big picture is the cost)");
        MediaInfo still = a->info();
        still.isStill = true;
        check(!assessVideoProxy(P("A.mp4"), still).worth, "a still is not");
        check(assessVideoProxy(dir + "/no-such-file.mp4", a->info()).worth, "a long-GOP file whose index cannot be read is taken to be long-GOP");
    }

    // ---- the plan
    // nested projects: N (D, and N2 inside it), N2 (J, and N again: a project that contains itself is looked at once)
    Project n2, n1, top;
    {
        n2.settings.width = 320; n2.settings.height = 180; n2.settings.fpsNum = 25;
        const uint64_t j = addMedia(n2, P("J.mp4"));
        Track& t = addTrack(n2, TrackType::Video);
        addEvents(n2, t, j, 30, 0.2);
        MediaItem back;
        back.id = n2.newId();
        back.path = P("N1.reno");
        back.name = "N1.reno";
        n2.media.push_back(back);
        addEvents(n2, t, back.id, 1, 2.0, 10);
        std::string err;
        check(n2.save(P("N2.reno"), &err), "a nested project can be saved", err);
        n1 = n2;
        n1.media.clear();
        n1.tracks.clear();
        const uint64_t d = addMedia(n1, P("D.mp4"));
        Track& t1 = addTrack(n1, TrackType::Video);
        addEvents(n1, t1, d, 100, 0.2);
        MediaItem in2;
        in2.id = n1.newId();
        in2.path = P("N2.reno");
        in2.name = "N2.reno";
        n1.media.push_back(in2);
        addEvents(n1, t1, in2.id, 1, 2.0, 30);
        check(n1.save(P("N1.reno"), &err), "and so can the one that nests it", err);
    }
    auto loader = [](const std::string& path, Project& out) { return out.load(path, nullptr); };
    uint64_t mA, mB, mC, mE, mF, mH, mI, mStill, mN;
    {
        top.settings.width = 320; top.settings.height = 180; top.settings.fpsNum = 25;
        mA = addMedia(top, P("A.mp4"));
        mB = addMedia(top, P("B.mp4"));
        mC = addMedia(top, P("C.mp4"));
        mE = addMedia(top, P("E.mp4"));
        mF = addMedia(top, P("F.mov"));
        mH = addMedia(top, P("H.mp4"));
        mI = addMedia(top, P("I.mp4"));
        addMedia(top, P("K.mp4"));
        mStill = addMedia(top, P("A.mp4"));
        top.media.back().info.isStill = true;
        top.media.back().path = P("still.png");
        mN = top.newId();
        MediaItem nm;
        nm.id = mN;
        nm.path = P("N1.reno");
        nm.name = "N1.reno";
        top.media.push_back(nm);
        Track& v1 = addTrack(top, TrackType::Video);
        addEvents(top, v1, mA, 40, 0.2);
        addEvents(top, v1, mB, 4, 3.0, 20);
        addEvents(top, v1, mC, 10, 0.3, 40);
        addEvents(top, v1, mE, 40, 0.2, 50);
        addEvents(top, v1, mF, 40, 0.2, 60);
        addEvents(top, v1, mH, 40, 0.2, 70);
        addEvents(top, v1, mStill, 5, 1.0, 80);
        addEvents(top, v1, mN, 1, 5.0, 90);
        Track& a1 = addTrack(top, TrackType::Audio);
        addEvents(top, a1, mI, 40, 0.2);           // (the sound of a file: no picture is read)
    }
    // H already has a proxy (one VEGAS made beside the media)
    writeFile(videoProxyPath(P("H.mp4")), "a proxy");
    PlanOptions opt;
    std::vector<Candidate> got = plan(top, loader, opt);
    check(names(got) == "D.mp4, A.mp4, J.mp4, C.mp4, B.mp4", "plan: files that gain, most jumped about first, nested projects included: D, A, J, C, B", names(got));
    check(got.size() == 5 && got[0].events == 100 && got[1].events == 40 && got[4].events == 4, "plan: the events are counted per file",
          got.empty() ? "" : strfmt("%d, %d, %d", got[0].events, got.size() > 1 ? got[1].events : -1, got.size() > 4 ? got[4].events : -1));
    check(got.size() == 5 && got[0].score > got[1].score && got[1].score > got[2].score && got[2].score > got[3].score && got[3].score > got[4].score, "plan: and scored in that order");
    {
        // shorter events count for more than the same number of longer ones
        Project q = top;
        q.tracks.clear();
        Track& t = addTrack(q, TrackType::Video);
        addEvents(q, t, mB, 20, 0.15);
        addEvents(q, t, mC, 20, 1.0, 10);
        std::vector<Candidate> g2 = plan(q, loader, opt);
        check(names(g2) == "B.mp4, C.mp4", "plan: the same number of events: the shorter ones first", names(g2));
    }
    {
        PlanOptions off = opt;
        off.longGop = false;
        check(plan(top, loader, off).empty(), "plan: long-GOP files are left alone when that setting is off");
        // media only a nested project uses, with no loader to open it: only what the project itself uses
        std::vector<Candidate> g3 = plan(top, ProjectLoader(), opt);
        check(names(g3) == "A.mp4, C.mp4, B.mp4", "plan: without a way to read nested projects, the project's own media", names(g3));
        // a file in the skip list is left out
        PlanOptions sk = opt;
        sk.skip = [&](const std::string& p) { return pathFileName(p) == "A.mp4"; };
        check(names(plan(top, loader, sk)) == "D.mp4, J.mp4, C.mp4, B.mp4", "plan: files that failed are left out");
        // offline media and generated media are not looked at
        Project q = top;
        for (MediaItem& m : q.media)
            if (m.id == mA) m.offline = true;
        check(names(plan(q, loader, opt)) == "D.mp4, J.mp4, C.mp4, B.mp4", "plan: offline media is left out");
    }
    {
        // greater than 4K, whatever the codec: with that setting
        Project q = top;
        q.tracks.clear();
        for (MediaItem& m : q.media)
            if (m.id == mF) { m.info.width = 4096; m.info.height = 2304; }
        Track& t = addTrack(q, TrackType::Video);
        addEvents(q, t, mF, 3, 1.0);
        check(plan(q, loader, opt).empty(), "plan: a 4K intra-frame file is not long-GOP");
        PlanOptions k = opt;
        k.fourK = true;
        check(names(plan(q, loader, k)) == "F.mov", "plan: greater than 4K gets one with the 4K setting on, whatever the codec", names(plan(q, loader, k)));
        for (MediaItem& m : q.media)
            if (m.id == mF) { m.info.width = 3840; m.info.height = 2160; }
        check(plan(q, loader, k).empty(), "plan: 3840 x 2160 is not greater than 4K");
    }
    {
        // media asked for by name: forced ones first, unforced after the used ones (and only if they would gain)
        PlanOptions h = opt;
        h.extra[P("K.mp4")] = false;
        h.extra[P("E.mp4")] = false;
        check(names(plan(top, loader, h)) == "D.mp4, A.mp4, J.mp4, C.mp4, B.mp4, K.mp4", "plan: new media not yet on the timeline comes after used media", names(plan(top, loader, h)));
        h.extra[P("E.mp4")] = true;
        check(names(plan(top, loader, h)) == "E.mp4, D.mp4, A.mp4, J.mp4, C.mp4, B.mp4, K.mp4", "plan: ... a forced one (the at-once rule) comes first and needs no gain", names(plan(top, loader, h)));
    }

    // a proxy older than the file it was made from (the file was written again) does not count: it is not used and is made again
    {
        const std::string proxy = videoProxyPath(P("C.mp4"));
        writeFile(proxy, "a proxy");
        check(hasVideoProxy(P("C.mp4")), "a proxy made after the media counts");
        const auto then = std::filesystem::last_write_time(P("C.mp4"));
        std::filesystem::last_write_time(proxy, then - std::chrono::hours(1));
        check(!hasVideoProxy(P("C.mp4")), "a proxy older than its file does not (the file was rendered again)");
        std::vector<Candidate> g4 = plan(top, loader, opt);
        check(names(g4) == "D.mp4, A.mp4, J.mp4, C.mp4, B.mp4", "plan: ... and that file is planned again", names(g4));
        std::filesystem::last_write_time(proxy, then + std::chrono::seconds(1));
        check(hasVideoProxy(P("C.mp4")), "(a second either way is nothing: files copied together)");
        std::filesystem::remove(proxy);
        // a very long file name: the proxy's name in a folder is cut, still different for different names
        setVideoProxyFolder(dir + "/pfolder");
        const std::string longA = "/m/" + std::string(200, 'a') + "1.mp4", longB = "/m/" + std::string(200, 'a') + "2.mp4";
        const std::string pa = videoProxyPath(longA), pb = videoProxyPath(longB);
        check(pathFileName(pa).size() < 160 && pa != pb, "a very long media name gives a proxy name under the file name limit, still its own", strfmt("%d characters", (int)pathFileName(pa).size()));
        setVideoProxyFolder(std::string());
    }

    // ---- the worker (with a stand-in maker that writes a file where the proxy goes)
    Worker& w = Worker::get();
    w.setLoader(loader);
    w.setSettleMs(40);
    std::vector<std::string> madeOrder;
    std::vector<std::string> attempts;
    std::mutex mm;
    std::atomic<int> failNext{0};
    std::atomic<bool> hold{false};
    std::function<void(const std::string&)> beforeMake;
    w.setMaker([&](const std::string& path, const std::function<bool(double)>& progress, std::string* err) {
        {
            std::lock_guard<std::mutex> lk(mm);
            attempts.push_back(path);
        }
        for (int k = 1; k <= 4; ++k) {
            if (!progress(k / 4.0)) return false;           // (stopped: nothing left behind)
            while (hold) {
                if (!progress(0.5)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (failNext > 0 && pathFileName(path) == "B.mp4") { *err = "made up failure"; return false; }
        writeFile(videoProxyPath(path), "a proxy");
        videoProxiesChanged();
        std::lock_guard<std::mutex> lk(mm);
        madeOrder.push_back(path);
        return true;
    });
    auto reset = [&] {
        w.setModes(false, false, false);
        w.forgetFailures();
        std::lock_guard<std::mutex> lk(mm);
        madeOrder.clear();
        attempts.clear();
        for (const char* n : {"A", "B", "C", "D", "J", "K"}) std::filesystem::remove(videoProxyPath(P((std::string(n) + ".mp4").c_str())));
    };
    auto shared = [](const Project& p) { return std::make_shared<const Project>(p); };

    check(!w.enabled() && !w.status().enabled, "worker: off until turned on");
    w.projectChanged(shared(top));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    check(attempts.empty(), "worker: nothing is made while it is off");
    // a project opened: queued in the order of the plan
    hold = true;
    w.setModes(true, false, false);
    check(waitFor([&] { return w.status().working; }, 10), "worker: it starts on the project it was given when turned on");
    {
        const Status st = w.status();
        check(st.enabled && st.total == 5 && st.done == 0 && st.current == "D.mp4", "worker: the first is the file the project jumps about in most; 5 to make", strfmt("%s, total %d, done %d", st.current.c_str(), st.total, st.done));
        check(names(w.queued()) == "A.mp4, J.mp4, C.mp4, B.mp4", "worker: the rest are queued in order", names(w.queued()));
        check(names(w.planned()) == "D.mp4, A.mp4, J.mp4, C.mp4, B.mp4", "worker: planned() lists them all", names(w.planned()));
        check(st.fraction > 0.2 && st.fraction < 0.9, "worker: tells how far the current one has got", strfmt("%.2f", st.fraction));
    }
    hold = false;
    check(w.waitIdle(20), "worker: all are made");
    {
        std::lock_guard<std::mutex> lk(mm);
        check(names(madeOrder) == "D.mp4, A.mp4, J.mp4, C.mp4, B.mp4", "worker: in the order planned", names(madeOrder));
    }
    check(!w.status().working && w.status().total == 0, "worker: idle again, nothing left to count", strfmt("total %d", w.status().total));
    // everything has its proxy now: a changed project makes nothing more
    attempts.clear();
    w.projectChanged(shared(top));
    check(w.waitIdle(5) && attempts.empty(), "worker: a project whose files all have proxies makes none");

    // ---- waits while the preview is playing
    reset();
    setPreviewPlaying(true);
    w.setModes(true, false, false);
    w.projectChanged(shared(top));
    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    check(attempts.empty(), "worker: not started while the preview plays");
    check(w.status().working || !w.queued().empty(), "worker: ... it is waiting with the queue, not dropped");
    setPreviewPlaying(false);
    check(waitFor([&] { std::lock_guard<std::mutex> lk(mm); return !attempts.empty(); }, 10), "worker: starts when playing stops");
    check(w.waitIdle(20), "worker: and makes them all");
    // a proxy under way waits too
    reset();
    hold = true;
    w.setModes(true, false, false);
    w.projectChanged(shared(top));
    check(waitFor([&] { return w.status().working && w.status().fraction > 0.1; }, 10), "worker: under way");
    setPreviewPlaying(true);
    check(waitFor([&] { return w.status().paused; }, 3), "worker: playing starts: it says it is paused");
    const double frozen = w.status().fraction;
    hold = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    check(w.status().paused && std::fabs(w.status().fraction - frozen) < 0.3 && madeOrder.empty(), "worker: and makes no progress while it plays");
    setPreviewPlaying(false);
    check(w.waitIdle(20) && madeOrder.size() == 5, "worker: and goes on when it stops", strfmt("%d made", (int)madeOrder.size()));

    // ---- another project: what the new one does not need is stopped; what it needs goes on
    reset();
    hold = true;
    w.setModes(true, false, false);
    w.projectChanged(shared(top));
    check(waitFor([&] { return w.status().current == "D.mp4"; }, 10), "worker: making D");
    Project other = top;
    other.tracks.clear();
    {
        Track& t = addTrack(other, TrackType::Video);
        addEvents(other, t, mC, 5, 0.5);
    }
    w.projectChanged(shared(other));
    // (the proxy under way is held until the worker has looked at the new project, and stopped it)
    check(waitFor([&] { return names(w.planned()) == "C.mp4"; }, 10), "worker: the new project needs only C");
    hold = false;
    check(w.waitIdle(20), "worker: ... and it settles");
    {
        std::lock_guard<std::mutex> lk(mm);
        check(names(madeOrder) == "C.mp4", "worker: D was stopped (the new project does not use it) and only C was made", names(madeOrder));
    }
    check(!hasVideoProxy(P("D.mp4")) && !std::filesystem::exists(videoProxyPath(P("D.mp4")) + ".part.mp4"), "worker: nothing left of the stopped one");
    // media that comes in: a hint
    w.hint(P("K.mp4"), false);
    {
        Project q = other;
        const uint64_t id = addMedia(q, P("K.mp4"));
        (void)id;
        w.projectChanged(shared(q));
    }
    check(w.waitIdle(20), "worker: media that has come in, not on the timeline yet, still settles");
    {
        std::lock_guard<std::mutex> lk(mm);
        check(names(madeOrder) == "C.mp4, K.mp4", "worker: it gets its proxy too (when it would gain)", names(madeOrder));
    }

    // ---- failures are not retried; forgetting them tries again
    reset();
    failNext = 1;
    w.setModes(true, false, false);
    w.projectChanged(shared(top));
    check(w.waitIdle(20), "worker: a run with a failing file ends");
    int bAttempts = 0;
    {
        std::lock_guard<std::mutex> lk(mm);
        for (const std::string& a : attempts) if (pathFileName(a) == "B.mp4") ++bAttempts;
        check(madeOrder.size() == 5 && bAttempts == 1 && !hasVideoProxy(P("B.mp4")), "worker: the failed one was tried once, the others made (K too: asked for by name earlier)", strfmt("%d made, B tried %d times", (int)madeOrder.size(), bAttempts));
    }
    w.projectChanged(shared(top));
    check(w.waitIdle(5), "worker: ...");
    {
        std::lock_guard<std::mutex> lk(mm);
        bAttempts = 0;
        for (const std::string& a : attempts) if (pathFileName(a) == "B.mp4") ++bAttempts;
        check(bAttempts == 1, "worker: and not again when the project changes", strfmt("%d", bAttempts));
    }
    failNext = 0;
    w.forgetFailures();
    w.projectChanged(shared(top));
    check(w.waitIdle(10), "worker: after forgetting the failure ...");
    check(hasVideoProxy(P("B.mp4")), "worker: ... it is tried again, and made");

    // ---- turned off: stops what is under way and drops the queue
    reset();
    hold = true;
    w.setModes(true, false, false);
    w.projectChanged(shared(top));
    check(waitFor([&] { return w.status().working; }, 10), "worker: under way again");
    w.setModes(false, false, false);
    hold = false;
    check(waitFor([&] { return !w.status().working; }, 5), "worker: turned off: the proxy under way is stopped");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    check(madeOrder.empty() && w.queued().empty() && !w.status().enabled, "worker: and the queue is dropped; nothing is made");
    // "at once" media alone turns it on too
    reset();
    w.hint(P("K.mp4"), true);
    w.setModes(false, false, true);
    check(w.enabled(), "worker: the 'at once' setting alone turns it on");
    {
        Project q;
        q.settings = top.settings;
        addMedia(q, P("K.mp4"));
        w.projectChanged(shared(q));
    }
    check(w.waitIdle(20), "worker: settles");
    check(hasVideoProxy(P("K.mp4")), "worker: media asked for by the at-once rule is made though no event uses it");
    w.setModes(false, false, false);
    reset();

    // ---- a proxy made while the preview is running is read by the next frames; renders do not read proxies
    {
        std::string err;
        const std::string cA = P("pick_A.mp4"), cB = P("pick_other.mp4");
        // (a video of the same size and length in another colour stands in for what the proxy looks like)
        check(makeVideo(cA, 320, 180, 100, 200, 40, 10) && makeVideo(cB, 320, 180, 100, 10, 200, 10), "the videos for the pick-up test can be made");
        std::filesystem::remove(videoProxyPath(cA));
        Project p;
        p.settings.width = 320; p.settings.height = 180; p.settings.fpsNum = 25;
        const uint64_t m = addMedia(p, cA);
        Track& t = addTrack(p, TrackType::Video);
        addEvents(p, t, m, 8, 0.25);
        FxRuntime fx;
        MediaPool pool;
        pool.setUseProxies(true);                       // (the preview below Best quality)
        VideoEngine ve(pool, fx);
        FrameOptions fo;
        fo.scale = 0.5;
        ImagePtr img = ve.render(p, 0.1, fo);
        check(near(at(img, 0.2, 0.5), 200, 40, 10), "pick-up: before the proxy: the original", strfmt("%d %d %d", at(img, 0.2, 0.5).b, at(img, 0.2, 0.5).g, at(img, 0.2, 0.5).r));
        w.setMaker([&](const std::string& path, const std::function<bool(double)>& progress, std::string*) {
            for (int k = 1; k <= 10; ++k) {
                if (!progress(k / 10.0)) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(40));
            }
            std::filesystem::copy_file(cB, videoProxyPath(path), std::filesystem::copy_options::overwrite_existing);
            videoProxiesChanged();
            return true;
        });
        w.setModes(true, false, false);
        w.projectChanged(shared(p));
        // (frames keep being asked for while it is made: playing)
        bool sawOriginal = false, sawProxy = false;
        const auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < 400 && !sawProxy; ++f) {
            ImagePtr im = ve.render(p, (f % 40) / 25.0 * 0.5, fo);
            if (near(at(im, 0.2, 0.5), 200, 40, 10)) sawOriginal = true;
            if (near(at(im, 0.2, 0.5), 10, 200, 10)) sawProxy = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        check(sawOriginal && sawProxy, "pick-up: frames show the original while the proxy is made, then the proxy",
              strfmt("original %d, proxy %d, after %.1f s", (int)sawOriginal, (int)sawProxy, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()));
        check(w.waitIdle(10), "pick-up: the worker is done");
        // Best quality: the original again
        pool.setUseProxies(false);
        img = ve.render(p, 0.3, fo);
        check(near(at(img, 0.2, 0.5), 200, 40, 10), "pick-up: Best quality keeps showing the original while a proxy exists");

        // a render with the proxy present is byte for byte the render without it
        auto render = [&](const std::string& out) {
            EncodeSettings es;
            es.path = out;
            es.audio = false;
            es.width = 320;
            es.height = 180;
            es.fpsNum = 25;
            es.fpsDen = 1;
            es.videoFamily = "h264";
            es.encodeMode = "cpu";
            es.rateControl = "quality";
            es.quality = 20;
            es.encoderOptions = "threads=1";
            std::string e2;
            FxRuntime fx2;
            const bool ok = exportProject(p, fx2, es, 0.0, 2.0, nullptr, &e2);
            if (!ok) std::printf("export failed: %s\n", e2.c_str());
            return ok;
        };
        check(hasVideoProxy(cA), "export: the proxy is there");
        const bool r1 = render(P("export_with.mp4"));
        std::filesystem::remove(videoProxyPath(cA));
        videoProxiesChanged();
        const bool r2 = render(P("export_without.mp4"));
        const std::string s1 = slurp(P("export_with.mp4")), s2 = slurp(P("export_without.mp4"));
        check(r1 && r2 && !s1.empty() && s1 == s2, "export: the file rendered with a proxy present is byte for byte the one rendered without",
              strfmt("%d / %d bytes", (int)s1.size(), (int)s2.size()));
        {
            // and the frames of it are the original's colour (not the proxy's)
            auto r = openMedia(P("export_with.mp4"), &err);
            ImagePtr f = r ? r->videoFrame(0.3, 320, 180) : nullptr;
            check(f && near(at(f, 0.2, 0.5), 200, 40, 10, 30), "export: and shows the original's picture");
        }
        fx.releaseExcept({});
    }
    w.setModes(false, false, false);
    std::printf("SUMMARY: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
