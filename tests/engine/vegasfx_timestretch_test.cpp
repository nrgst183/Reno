// Time Stretch (sfppack1's CSfTime) against what the DLL itself produces: the outputs below, made by running the DLL's own
// stream engine on the test signal of vegasfx_script.h and fitting what it delivers into 512-frame blocks the way Reno
// does (silence until the first frame is ready, then the next frames of the stream; a block the stream cannot fill is
// finished with silence), must come out of Reno's version bit for bit. The stretch cases are the plug-in's float32 stereo path
// at 44100 and 48000 Hz for several modes; "updates" changes the settings in the middle (a new length only keeps the
// buffers, a new mode starts them again), "resets" jumps in time.
//
//   vegasfx_timestretch_test
#include "vegasfx_script.h"
#include "../../src/audiofx/vegastime.h"

static const vegfx::Case kCases[] = {
    {"44100 expand 110", "rate 44100\nch 2\nparams 0 1086684160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x84c2a432aa82c78eull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbd87d400, 0xbd8d7400, 0xbdc2e400, 0xbdc2d000}},
    {"44100 expand 200 speech", "rate 44100\nch 2\nparams 0 1087604736 6 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x6708776140d62c9aull, 200000, {0x00000000, 0x00000000, 0x3ecacf3b, 0x3ecae33c, 0x3dc20800, 0x3d10f000, 0xbd85268f, 0xbd84d690}},
    {"44100 expand 150 solo", "rate 44100\nch 2\nparams 0 1087196160 9 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xb3853a5ae2a4385cull, 200000, {0x00000000, 0x00000000, 0xbe8cbafe, 0xbe8ca6fe, 0xbe0d5c00, 0xbe85a200, 0xbedb3300, 0xbedb1f00}},
    {"44100 identity", "rate 44100\nch 2\nparams 0 1086556160 2 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x9093addb03b0fb69ull, 200000, {0x00000000, 0x00000000, 0xbecddddc, 0xbecdc9dc, 0x3cf0a800, 0x3eef2200, 0x3d89e200, 0x3d89f600}},
    {"44100 compress 85", "rate 44100\nch 2\nparams 0 1086364160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x912d3f557a86f75cull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3dfe1800, 0xbdbe4200, 0xbefe9000, 0xbefe7c00}},
    {"44100 compress 50 drums", "rate 44100\nch 2\nparams 0 1085507584 16 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xb73a3003cdc6dd1eull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbd20c000, 0xbc836800, 0x00000000, 0x00000000}},
    {"44100 expand 500 music6", "rate 44100\nch 2\nparams 0 1088973312 5 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xa23b1c4eb4a5e816ull, 200000, {0x00000000, 0x00000000, 0xbe823e00, 0xbe822c00, 0x3ee51e00, 0x3de0fa00, 0x3da72000, 0x3da73400}},
    {"44100 updates", "rate 44100\nch 2\nparams 0 1086556160 3 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1087196160 3 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1087196160 12 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086684160 12 12000 0 1094987768 0 0\nupdate\nproc 20000\n", 0x5cfb6d6faf813a6eull, 220000, {0x00000000, 0x00000000, 0x3df25000, 0x3df2a000, 0x3e1e9800, 0x3d152800, 0x3d15b000, 0x3d15d800}},
    {"44100 resets", "rate 44100\nch 2\nparams 0 1086812160 3 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1086428160 7 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1088244736 7 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086556160 0 12000 0 1094987768 0 0\nreset\nproc 20000\n", 0xf5c2e580c23e930cull, 220000, {0x00000000, 0x00000000, 0x3df25000, 0x3df2a000, 0x3e930000, 0x3ebb7c51, 0xbeb9aa00, 0xbeb99600}},
    {"48000 expand 110", "rate 48000\nch 2\nparams 0 1086684160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xf2f1f59ab576433bull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc8c6000, 0xbda88800, 0xbd0a2800, 0xbd0a0000}},
    {"48000 expand 200 speech", "rate 48000\nch 2\nparams 0 1087604736 6 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x691a68e3397e1e5eull, 200000, {0x00000000, 0x00000000, 0x3eede800, 0x3eedfc00, 0x3df65400, 0xbddb0a00, 0xbea7996a, 0xbea7856a}},
    {"48000 expand 150 solo", "rate 48000\nch 2\nparams 0 1087196160 9 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x6cfefcc97a490c92ull, 200000, {0x00000000, 0x00000000, 0xbe137400, 0xbe134c00, 0xbefe0c00, 0x3e70e000, 0x3eb1aa00, 0x3eb1be00}},
    {"48000 identity", "rate 48000\nch 2\nparams 0 1086556160 2 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x690a8970f3b6d86dull, 200000, {0x00000000, 0x00000000, 0xbe137400, 0xbe134c00, 0x3b976000, 0x3b410000, 0xbca39800, 0xbca34800}},
    {"48000 compress 85", "rate 48000\nch 2\nparams 0 1086364160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x0fb8d4cf3c742d76ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3dfe4600, 0xbda27e00, 0xbe0c3000, 0xbe0c0c00}},
    {"48000 compress 50 drums", "rate 48000\nch 2\nparams 0 1085507584 16 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x08f471334efa2518ull, 200000, {0x00000000, 0x00000000, 0xbd2c6000, 0xbd2c3800, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {"48000 expand 500 music6", "rate 48000\nch 2\nparams 0 1088973312 5 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x6d3a39bebac7d017ull, 200000, {0x00000000, 0x00000000, 0xbe137400, 0xbe134c00, 0xbb3bc000, 0xbca94000, 0xbd7a3800, 0xbd7a1000}},
    {"48000 updates", "rate 48000\nch 2\nparams 0 1086556160 3 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1087196160 3 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1087196160 12 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086684160 12 12000 0 1094987768 0 0\nupdate\nproc 20000\n", 0xa9e600e19d25ee2aull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbe469c00, 0x3cf40000, 0x3ce5908c, 0x3ce5e08c}},
    {"48000 resets", "rate 48000\nch 2\nparams 0 1086812160 3 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1086428160 7 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1088244736 7 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086556160 0 12000 0 1094987768 0 0\nreset\nproc 20000\n", 0xb804de3af9aa57d5ull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3ed29c00, 0xbec52400, 0x3d876200, 0x3d877600}},
};

int main()
{
    int bad = vegfx::checkCases("reno.timestretch", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    using namespace reno;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++bad;
    };
    const vegpack1::Info* info = vegpack1::findInfo("reno.timestretch");
    check(info != nullptr, "registered");
    if (info) {
        // the saved form: u32 length 0x34, the plug-in's class id, 32 bytes of settings
        std::unique_ptr<AudioPlugin> p = vegpack1::create("reno.timestretch");
        const std::string st = p->saveState();
        const std::string raw = base64Decode(st.substr(2));
        check(st.compare(0, 2, "S:") == 0 && raw.size() == 0x34, "saved form is 52 bytes");
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const unsigned char guid[16] = {0xa4, 0xb4, 0xf5, 0x5f, 0x8f, 0x85, 0xd0, 0x11, 0xae, 0xbc, 0x00, 0xa0, 0xc9, 0x05, 0x39, 0x12};
        check(len == 0x34 && std::memcmp(raw.data() + 4, guid, 16) == 0, "saved form: length and class id");
        double d30;
        std::memcpy(&d30, raw.data() + 20, 8);
        int32_t mode, tempo;
        std::memcpy(&mode, raw.data() + 28, 4);
        std::memcpy(&tempo, raw.data() + 32, 4);
        double ini;
        std::memcpy(&ini, raw.data() + 36, 8);
        check(d30 == 10000.0 && mode == 0 && tempo == 12000 && ini == 2646000.0, "default settings");
        // the presets are VEGAS's four
        check(p->programCount() == 4 && p->programName(0) == "(Default)" && p->programName(1) == "Default all parameters" &&
                  p->programName(2) == "Time expand 110 % of original" && p->programName(3) == "Time compress 85 % of original",
              "presets");
        p->setProgram(2);
        std::memcpy(&d30, base64Decode(p->saveState().substr(2)).data() + 20, 8);
        check(d30 == 11000.0, "preset 110 %");
        // parameters: the final length in percent
        check(p->paramCount() == 5 && p->paramName(0) == "Final Length", "parameters");
        p->paramSet(0, 1.0f);
        check(p->paramText(0) == "500.0 %", "final length text");
        p->paramSet(0, 0.0f);
        check(p->paramText(0) == "50.0 %", "final length minimum");
        // loads what it saved, refuses another effect's form
        std::unique_ptr<AudioPlugin> q = vegpack1::create("reno.timestretch");
        check(q->loadState(p->saveState()) && q->saveState() == p->saveState(), "save and load");
        check(!q->loadState(vegpack1::encode(*vegpack1::findInfo("reno.pitchshift"), std::vector<int32_t>(12, 0).data())), "other effect's form refused");
        // the DirectX class id
        check(vegpack1::uidForDx("dshow:{B97C0F22-196D-11D1-B99B-00A0C9053912}") == "reno.timestretch" &&
                  vegpack1::dxForUid("reno.timestretch") == "dshow:{B97C0F22-196D-11D1-B99B-00A0C9053912}",
              "DirectX mapping");
    }

    // Apply Non-Real-Time Event FX: the whole sound through a fresh run of the effect and the stream flushed at its end, as
    // the DLL's engine gives it (lengths and hashes made with the DLL on the 100000-frame test signal)
    struct Whole { const char* uid; int rate, pct100, mode, frames; uint64_t hash; };
    static const Whole kWhole[] = {
    {"reno.timestretch", 44100, 12000, 0, 119989, 0xb90d208d33765f4full},
    {"reno.timestretch", 48000, 8000, 16, 80000, 0xee214721649a5153ull},
    {"reno.timestretch", 48000, 20000, 6, 200000, 0xddef28addec0f880ull},
    {"reno.xptimestretch", 44100, 15000, 1, 149575, 0x7cf3910d6e0d25ebull},
    {"reno.xptimestretch", 48000, 7000, 3, 70027, 0x97cf2c867bb26159ull},
    };
    for (const Whole& w : kWhole) {
        const vegpack1::Info* wi = vegpack1::findInfo(w.uid);
        std::unique_ptr<AudioPlugin> p = vegpack1::create(w.uid);
        std::vector<int32_t> v(8, 0);
        const double d = (double)w.pct100, ini = 2646000.0;
        std::memcpy(&v[0], &d, 8);
        v[2] = w.mode;
        v[3] = 12000;
        std::memcpy(&v[4], &ini, 8);
        p->loadState(vegpack1::encode(*wi, v.data()));
        p->activate(w.rate, 512);
        const std::vector<float> in = vegfx::testInput(100000);
        std::vector<float> l(100000), r(100000);
        for (int k = 0; k < 100000; ++k) {
            l[(size_t)k] = in[(size_t)2 * k];
            r[(size_t)k] = in[(size_t)2 * k + 1];
        }
        std::vector<float> ol, orr;
        const bool ok = p->changesLength() && p->processWhole(l.data(), r.data(), 100000, ol, orr);
        std::vector<float> inter(ol.size() * 2);
        for (size_t k = 0; k < ol.size(); ++k) {
            inter[2 * k] = ol[k];
            inter[2 * k + 1] = orr[k];
        }
        char what[96];
        std::snprintf(what, sizeof what, "whole sound: %s %d Hz %d%% mode %d (%zu frames)", w.uid, w.rate, w.pct100 / 100, w.mode, ol.size());
        check(ok && ol.size() == (size_t)w.frames && vegfx::fnv(inter) == w.hash, what);
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
