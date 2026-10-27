// ExpressFX Time Stretch (sfxpfx3's CXpTime, the same engine as Time Stretch with five modes of its own) against what the DLL itself produces: the outputs below, made by running the DLL's own
// stream engine on the test signal of vegasfx_script.h and fitting what it delivers into 512-frame blocks the way Reno
// does (silence until the first frame is ready, then the next frames of the stream; a block the stream cannot fill is
// finished with silence), must come out of Reno's version bit for bit. The stretch cases are the plug-in's float32 stereo path
// at 44100 and 48000 Hz for the five modes; "updates" changes the settings in the middle (a new length only keeps the
// buffers, a new mode starts them again), "resets" jumps in time.
//
//   vegasfx_xptimestretch_test
#include "vegasfx_script.h"
#include "../../src/audiofx/vegastime.h"

static const vegfx::Case kCases[] = {
    {"44100 expand 110 music", "rate 44100\nch 2\nparams 0 1086684160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x84c2a432aa82c78eull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbd87d400, 0xbd8d7400, 0xbdc2e400, 0xbdc2d000}},
    {"44100 expand 200 speech", "rate 44100\nch 2\nparams 0 1087604736 1 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x6708776140d62c9aull, 200000, {0x00000000, 0x00000000, 0x3ecacf3b, 0x3ecae33c, 0x3dc20800, 0x3d10f000, 0xbd85268f, 0xbd84d690}},
    {"44100 expand 150 solo", "rate 44100\nch 2\nparams 0 1087196160 2 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xb3853a5ae2a4385cull, 200000, {0x00000000, 0x00000000, 0xbe8cbafe, 0xbe8ca6fe, 0xbe0d5c00, 0xbe85a200, 0xbedb3300, 0xbedb1f00}},
    {"44100 identity", "rate 44100\nch 2\nparams 0 1086556160 2 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xcb2daf6cda4eccf8ull, 200000, {0x00000000, 0x00000000, 0xbecdcaea, 0xbecdb6ea, 0x3c979800, 0x3eef2200, 0x3d67f800, 0x3d682000}},
    {"44100 compress 85", "rate 44100\nch 2\nparams 0 1086364160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x912d3f557a86f75cull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3dfe1800, 0xbdbe4200, 0xbefe9000, 0xbefe7c00}},
    {"44100 compress 50 drums", "rate 44100\nch 2\nparams 0 1085507584 3 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xf2518227f410cc9eull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3df6fa00, 0x00000000, 0x00000000}},
    {"44100 expand 500 toms", "rate 44100\nch 2\nparams 0 1088973312 4 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x81a26710a1e0cb5bull, 200000, {0x00000000, 0x00000000, 0x3eaace00, 0x3eaae200, 0x3eedca42, 0x3d71e023, 0xbc2f7000, 0xbc2ed000}},
    {"44100 updates", "rate 44100\nch 2\nparams 0 1086556160 0 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1087196160 0 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1087196160 2 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086684160 2 12000 0 1094987768 0 0\nupdate\nproc 20000\n", 0x1a1d156c7d1d2b4cull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3e9b5200, 0x3e9ff600, 0x3c37c842, 0x3c386842}},
    {"44100 resets", "rate 44100\nch 2\nparams 0 1086812160 0 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1086428160 1 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1088244736 1 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086556160 0 12000 0 1094987768 0 0\nreset\nproc 20000\n", 0xb8b09b02e4b52dafull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbde1bc00, 0x3eac7800, 0xbeb9aa00, 0xbeb99600}},
    {"48000 expand 110 music", "rate 48000\nch 2\nparams 0 1086684160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0xf2f1f59ab576433bull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc8c6000, 0xbda88800, 0xbd0a2800, 0xbd0a0000}},
    {"48000 expand 200 speech", "rate 48000\nch 2\nparams 0 1087604736 1 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x691a68e3397e1e5eull, 200000, {0x00000000, 0x00000000, 0x3eede800, 0x3eedfc00, 0x3df65400, 0xbddb0a00, 0xbea7996a, 0xbea7856a}},
    {"48000 expand 150 solo", "rate 48000\nch 2\nparams 0 1087196160 2 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x6cfefcc97a490c92ull, 200000, {0x00000000, 0x00000000, 0xbe137400, 0xbe134c00, 0xbefe0c00, 0x3e70e000, 0x3eb1aa00, 0x3eb1be00}},
    {"48000 identity", "rate 48000\nch 2\nparams 0 1086556160 2 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x76c387d60328c52dull, 200000, {0x00000000, 0x00000000, 0xbe137400, 0xbe134c00, 0x3c480000, 0x39500000, 0xbc7ae000, 0xbc7a4000}},
    {"48000 compress 85", "rate 48000\nch 2\nparams 0 1086364160 0 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x0fb8d4cf3c742d76ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3dfe4600, 0xbda27e00, 0xbe0c3000, 0xbe0c0c00}},
    {"48000 compress 50 drums", "rate 48000\nch 2\nparams 0 1085507584 3 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x99b3d9dc56eb8ff3ull, 200000, {0x00000000, 0x00000000, 0xbda61000, 0xbda5fc00, 0x00000000, 0x3d0b3400, 0x3e359c00, 0x3e35c400}},
    {"48000 expand 500 toms", "rate 48000\nch 2\nparams 0 1088973312 4 12000 0 1094987768 0 0\nreset\nproc 100000\n", 0x980e0bbd4e966139ull, 200000, {0x00000000, 0x00000000, 0x3eac3dd6, 0x3eac50c0, 0x3d775cd5, 0x3bfcc000, 0xbdbf8800, 0xbdbf7400}},
    {"48000 updates", "rate 48000\nch 2\nparams 0 1086556160 0 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1087196160 0 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1087196160 2 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086684160 2 12000 0 1094987768 0 0\nupdate\nproc 20000\n", 0x70816fe924ace1adull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc8e9000, 0xbda88800, 0x3df96800, 0x3df97c00}},
    {"48000 resets", "rate 48000\nch 2\nparams 0 1086812160 0 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1086428160 1 12000 0 1094987768 0 0\nreset\nproc 30000\nparams 0 1088244736 1 12000 0 1094987768 0 0\nupdate\nproc 30000\nparams 0 1086556160 0 12000 0 1094987768 0 0\nreset\nproc 20000\n", 0xc6081aed99b6210bull, 220000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xbc8ba800, 0xbdbea000, 0x3d876200, 0x3d877600}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xptimestretch", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    using namespace reno;
    auto check = [&](bool ok, const char* what) {
        std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok) ++bad;
    };
    const vegpack1::Info* info = vegpack1::findInfo("reno.xptimestretch");
    check(info != nullptr, "registered");
    if (info) {
        // the saved form: u32 length 0x34, the plug-in's class id, 32 bytes of settings
        std::unique_ptr<AudioPlugin> p = vegpack1::create("reno.xptimestretch");
        const std::string st = p->saveState();
        const std::string raw = base64Decode(st.substr(2));
        check(st.compare(0, 2, "S:") == 0 && raw.size() == 0x34, "saved form is 52 bytes");
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const unsigned char guid[16] = {0x84, 0x03, 0x0f, 0xe7, 0xb1, 0x64, 0xc0, 0x44, 0x8f, 0x7c, 0x8a, 0xc0, 0xcc, 0x8a, 0x22, 0xde};
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
        p->paramSet(1, 1.0f);
        check(p->paramText(1) == "Drums (better for toms)", "five modes");
        // loads what it saved, refuses another effect's form
        std::unique_ptr<AudioPlugin> q = vegpack1::create("reno.xptimestretch");
        check(q->loadState(p->saveState()) && q->saveState() == p->saveState(), "save and load");
        check(!q->loadState(vegpack1::encode(*vegpack1::findInfo("reno.pitchshift"), std::vector<int32_t>(12, 0).data())), "other effect's form refused");
        // the DirectX class id
        check(vegpack1::uidForDx("dshow:{0000000B-0F56-11D2-9887-00A0C969725B}") == "reno.xptimestretch" &&
                  vegpack1::dxForUid("reno.xptimestretch") == "dshow:{0000000B-0F56-11D2-9887-00A0C969725B}",
              "DirectX mapping");
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
