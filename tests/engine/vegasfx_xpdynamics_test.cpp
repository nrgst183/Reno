// ExpressFX Dynamics (sfxpfx3_x64.dll, CXpDyn) against what the DLL itself produces: the outputs below, made by running the DLL's own
// streaming code (set-format and its 20 ms block routine) on the test signal of vegasfx_script.h, must come out of Reno's version bit for
// bit, behind the one block of silence the effect's latency puts first (and after each reset). Also the saved form, presets and ranges.
//
//   vegasfx_xpdynamics_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams -200 10 10 5000 0\nreset\nproc 100000\n", 0xabe226177e513086ull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 limit", "rate 44100\nch 2\nparams -60 500 10 5000 0\nreset\nproc 100000\n", 0xabe226177e513086ull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 maximize", "rate 44100\nch 2\nparams -120 500 50 1000 1\nreset\nproc 100000\n", 0x536c15b20c326a9bull, 200000, {0x00000000, 0x00000000, 0x3e577b73, 0x3e579e32, 0x3f47d210, 0x3e9b0d2a, 0x3f1403e7, 0x3f141786}},
    {"44100 2to1", "rate 44100\nch 2\nparams -180 20 10 5000 1\nreset\nproc 100000\n", 0xcb97147f8dd0a2faull, 200000, {0x00000000, 0x00000000, 0x3dd8f60a, 0x3dd91906, 0x3f0c6987, 0x3e203da4, 0x3ed135a1, 0x3ed1515c}},
    {"44100 3to1", "rate 44100\nch 2\nparams -150 30 10 5000 1\nreset\nproc 100000\n", 0x7ed0421aab6a2689ull, 200000, {0x00000000, 0x00000000, 0x3df37e61, 0x3df3a5a5, 0x3f1f2cd2, 0x3e33dae1, 0x3eeadfd3, 0x3eeafef6}},
    {"44100 hard", "rate 44100\nch 2\nparams -400 500 0 50 0\nreset\nproc 100000\n", 0xdec70325652fa280ull, 200000, {0x00000000, 0x00000000, 0x3b9eed55, 0x3b9f06f5, 0x3c030bc3, 0x3bfd59e6, 0x3bc15339, 0x3bc16cda}},
    {"44100 slow", "rate 44100\nch 2\nparams -300 40 5000 50000 1\nreset\nproc 100000\n", 0xfbc881bf0d1680aaull, 200000, {0x00000000, 0x00000000, 0x3e079dd7, 0x3e07b3b6, 0x3fa26ec7, 0x3e8a3386, 0x3f504250, 0x3f505dec}},
    {"44100 mid", "rate 44100\nch 2\nparams -100 155 333 777 0\nreset\nproc 100000\n", 0xa3846c5d5ea1fbc8ull, 200000, {0x00000000, 0x00000000, 0x3d6b9ed4, 0x3d6bc4d2, 0x3eb87b5e, 0x3db0eb39, 0x3e44975f, 0x3e44b16e}},
    {"44100 update mid", "rate 44100\nch 2\nparams -300 40 100 2000 0\nreset\nproc 20000\nparams -100 500 5000 500 1\nupdate\nproc 20000\nparams -600 100 0 0 0\nupdate\nproc 50000\nreset\nproc 20000\n", 0x9c5f8650ecf26f4full, 220000, {0x00000000, 0x00000000, 0x3cb11279, 0x3cb12f06, 0x3f9b3e3a, 0x3aa43b8c, 0xba8299b3, 0xba82869f}},
    {"48000 default", "rate 48000\nch 2\nparams -200 10 10 5000 0\nreset\nproc 100000\n", 0xbe0de28889a4cd73ull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 limit", "rate 48000\nch 2\nparams -60 500 10 5000 0\nreset\nproc 100000\n", 0xbe0de28889a4cd73ull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 maximize", "rate 48000\nch 2\nparams -120 500 50 1000 1\nreset\nproc 100000\n", 0x08766e80aefb44ecull, 200000, {0x00000000, 0x00000000, 0xbd555ea1, 0xbd54dcbb, 0x3ea533bd, 0x3d7bc2cb, 0xbcbce243, 0xbcba6e49}},
    {"48000 2to1", "rate 48000\nch 2\nparams -180 20 10 5000 1\nreset\nproc 100000\n", 0x563d883dd5c02cd5ull, 200000, {0x00000000, 0x00000000, 0xbcda1096, 0xbcd98bd4, 0x3e0d0b6f, 0x3d03703f, 0xbc858561, 0xbc83c977}},
    {"48000 3to1", "rate 48000\nch 2\nparams -150 30 10 5000 1\nreset\nproc 100000\n", 0x44dc6cbc50c2feedull, 200000, {0x00000000, 0x00000000, 0xbcf4bdbb, 0xbcf428bc, 0x3e1e5510, 0x3d138742, 0xbc95eb61, 0xbc93f8f2}},
    {"48000 hard", "rate 48000\nch 2\nparams -400 500 0 50 0\nreset\nproc 100000\n", 0xea3dd818e3e16d2cull, 200000, {0x00000000, 0x00000000, 0xbaa84250, 0xbaa7dbe0, 0x3af39193, 0x3ad22b05, 0xb976db15, 0xb973a65e}},
    {"48000 slow", "rate 48000\nch 2\nparams -300 40 5000 50000 1\nreset\nproc 100000\n", 0x2582a72a8ffc14b6ull, 200000, {0x00000000, 0x00000000, 0xbd013e39, 0xbd00ef8a, 0x3e97be49, 0x3d69210a, 0xbd0ff6e0, 0xbd0e183d}},
    {"48000 mid", "rate 48000\nch 2\nparams -100 155 333 777 0\nreset\nproc 100000\n", 0xe239a0825956c006ull, 200000, {0x00000000, 0x00000000, 0xbc6ff44b, 0xbc6f6235, 0x3dba2f22, 0x3c909e29, 0xbbfc3229, 0xbbf8ebb1}},
    {"48000 update mid", "rate 48000\nch 2\nparams -300 40 100 2000 0\nreset\nproc 20000\nparams -100 500 5000 500 1\nupdate\nproc 20000\nparams -600 100 0 0 0\nupdate\nproc 50000\nreset\nproc 20000\n", 0x9b07800ae71f77bfull, 220000, {0x00000000, 0x00000000, 0xbbbb8a2d, 0xbbbb1800, 0x3e91c6e2, 0x39883857, 0x3ad01538, 0x3ad0284b}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpdynamics", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpdynamics");
    if (!info || (int)info->params.size() != 5 || (int)info->presets.size() != 6) { std::printf("FAIL info\n"); ++bad; }
    else {
        // the saved form: u32 length (40), the plug-in's GUID, the five settings
        const std::string st = reno::vegpack1::encode(*info, info->presets[3].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0x36, 0xed, 0xa0, 0x65, 0xa1, 0x90, 0xf6, 0x46, 0x99, 0xb7, 0xfc, 0x52, 0x58, 0x47, 0x09, 0x01};
        const bool ok = st.compare(0, 2, "S:") == 0 && raw.size() == 40 && len == 40 && std::memcmp(raw.data() + 4, guid, 16) == 0 &&
                        reno::vegpack1::decode(*info, st, &back) && back == info->presets[3].v && back == std::vector<int32_t>({-120, 500, 50, 1000, 1});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
        // the DirectX class id both ways, and the latency (one 20 ms block)
        const bool map = reno::vegpack1::uidForDx("dshow:{00000008-0F56-11D2-9887-00A0C969725B}") == "reno.xpdynamics" &&
                         reno::vegpack1::dxForUid("reno.xpdynamics") == "dshow:{00000008-0F56-11D2-9887-00A0C969725B}";
        std::printf("%s DirectX map\n", map ? "PASS" : "FAIL");
        if (!map) ++bad;
        std::unique_ptr<reno::AudioPlugin> p = reno::vegpack1::create("reno.xpdynamics");
        p->activate(44100, 512);
        const bool lat = p->latencySamples() == 882 && p->paramCount() == 5 && p->paramText(1) == "1.0 :1";
        std::printf("%s latency and texts\n", lat ? "PASS" : "FAIL");
        if (!lat) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
