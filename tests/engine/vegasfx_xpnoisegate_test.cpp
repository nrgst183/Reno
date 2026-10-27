// ExpressFX Noise Gate (sfxpfx3_x64.dll, CXpNoisgt) against what the DLL itself produces: the outputs below, made by running the DLL's
// own streaming code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit, behind the one block of
// silence the latency puts first (and after each reset). Also the saved form, presets and the DirectX map.
//
//   vegasfx_xpnoisegate_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 3 100 -200\nreset\nproc 100000\n", 0xd8865ad57d7af7caull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 strong", "rate 44100\nch 2\nparams 1 300 -60\nreset\nproc 100000\n", 0xccd28b3844aea803ull, 200000, {0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000}},
    {"44100 long", "rate 44100\nch 2\nparams 10 1000 -380\nreset\nproc 100000\n", 0x38365867fb2bb716ull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 gate1", "rate 44100\nch 2\nparams 10 500 -600\nreset\nproc 100000\n", 0x38365867fb2bb716ull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 gate2", "rate 44100\nch 2\nparams 1 500 -320\nreset\nproc 100000\n", 0x7bacefd8251697e3ull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 slowattack", "rate 44100\nch 2\nparams 300 1000 -400\nreset\nproc 100000\n", 0x31f754a11cc7b0e3ull, 200000, {0x00000000, 0x00000000, 0x3d6d1a1b, 0x3d6ecfe2, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 inf", "rate 44100\nch 2\nparams 5 50 -800\nreset\nproc 100000\n", 0x4257895099c89f6aull, 200000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ecc6800, 0x3dc5de00, 0x3e96de00, 0x3e96f200}},
    {"44100 mid", "rate 44100\nch 2\nparams 20 200 -130\nreset\nproc 100000\n", 0x893e114eb48a883aull, 200000, {0x00000000, 0x00000000, 0x3cb7c8bc, 0x3c92109b, 0x3ebfb721, 0x3d458b1e, 0x3e96b760, 0x3e96cb5c}},
    {"44100 update mid", "rate 44100\nch 2\nparams 3 100 -300\nreset\nproc 20000\nparams 50 400 -100\nupdate\nproc 20000\nparams 1 20 -500\nupdate\nproc 50000\nreset\nproc 20000\n", 0x9d7e02369646ae75ull, 220000, {0x00000000, 0x00000000, 0x3d781000, 0x3d783800, 0x3ea59065, 0x3dc5de00, 0xbe88f000, 0xbe88dc00}},
    {"48000 default", "rate 48000\nch 2\nparams 3 100 -200\nreset\nproc 100000\n", 0x52e5d4365eca08c4ull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 strong", "rate 48000\nch 2\nparams 1 300 -60\nreset\nproc 100000\n", 0xe65c1126b5729303ull, 200000, {0x00000000, 0x00000000, 0x80000000, 0x80000000, 0x00000000, 0x00000000, 0x80000000, 0x80000000}},
    {"48000 long", "rate 48000\nch 2\nparams 10 1000 -380\nreset\nproc 100000\n", 0xe7a73c6d81bee7d8ull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 gate1", "rate 48000\nch 2\nparams 10 500 -600\nreset\nproc 100000\n", 0xe7a73c6d81bee7d8ull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 gate2", "rate 48000\nch 2\nparams 1 500 -320\nreset\nproc 100000\n", 0x295aa9aba88135dcull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 slowattack", "rate 48000\nch 2\nparams 300 1000 -400\nreset\nproc 100000\n", 0x5f89018701bdc719ull, 200000, {0x00000000, 0x00000000, 0xbc77360a, 0xbc78d691, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 inf", "rate 48000\nch 2\nparams 5 50 -800\nreset\nproc 100000\n", 0x1d0b5a27e8e37349ull, 200000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3dbe1800, 0x3ca3e800, 0xbc408000, 0xbc3e0000}},
    {"48000 mid", "rate 48000\nch 2\nparams 20 200 -130\nreset\nproc 100000\n", 0x09d87f9e52dc6761ull, 200000, {0x00000000, 0x00000000, 0xbc179ad7, 0xbbf041a4, 0x3dad9ecf, 0x3c558ed3, 0xbc404eb8, 0xbc3dcf5c}},
    {"48000 update mid", "rate 48000\nch 2\nparams 3 100 -300\nreset\nproc 20000\nparams 50 400 -100\nupdate\nproc 20000\nparams 1 20 -500\nupdate\nproc 50000\nreset\nproc 20000\n", 0x6e0c69b4e5adc6eaull, 220000, {0x00000000, 0x00000000, 0xbc836800, 0xbc831800, 0x3d91aeef, 0x3ca3e800, 0x3eda2e00, 0x3eda4200}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpnoisegate", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpnoisegate");
    if (!info || (int)info->params.size() != 3 || (int)info->presets.size() != 7) { std::printf("FAIL info\n"); ++bad; }
    else {
        // the saved form: u32 length (32), the plug-in's GUID, attack, release, threshold
        const std::string st = reno::vegpack1::encode(*info, info->presets[4].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0xba, 0xe8, 0x04, 0x52, 0x57, 0x46, 0x33, 0x47, 0xa6, 0xeb, 0x1e, 0x65, 0xf7, 0x6c, 0x99, 0xa4};
        const bool ok = raw.size() == 32 && len == 32 && std::memcmp(raw.data() + 4, guid, 16) == 0 && reno::vegpack1::decode(*info, st, &back) &&
                        back == std::vector<int32_t>({10, 500, -600});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
        const bool map = reno::vegpack1::uidForDx("dshow:{0000000A-0F56-11D2-9887-00A0C969725B}") == "reno.xpnoisegate" &&
                         reno::vegpack1::dxForUid("reno.xpnoisegate") == "dshow:{0000000A-0F56-11D2-9887-00A0C969725B}";
        std::printf("%s DirectX map\n", map ? "PASS" : "FAIL");
        if (!map) ++bad;
        std::unique_ptr<reno::AudioPlugin> p = reno::vegpack1::create("reno.xpnoisegate");
        p->activate(48000, 512);
        const bool lat = p->latencySamples() == 960 && p->paramText(0) == "-20.0 dB";
        std::printf("%s latency and texts\n", lat ? "PASS" : "FAIL");
        if (!lat) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
