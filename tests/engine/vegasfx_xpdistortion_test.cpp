// ExpressFX Distortion (CXpDistor) against what the DLL itself produces (sfxpfx1_x64.dll): the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit. Also the saved form.
//
//   vegasfx_xpdistortion_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 0 500 0 500\nreset\nproc 100000\n", 0x0a7ea6c16d2624f5ull, 200000, {0xbed16257, 0xbed15307, 0xbd2ea36b, 0xbd2e3599, 0x3e8553d3, 0x3d66bfc7, 0xbcda9be4, 0xbcd72d56}},
    {"44100 mangle", "rate 44100\nch 2\nparams 0 1000 1 1000\nreset\nproc 100000\n", 0xbcf253c41e729112ull, 200000, {0xbf029994, 0xbf029914, 0xbeffc1ef, 0xbeffc1af, 0x3f00e4e1, 0x3effe2a3, 0x3ef872aa, 0x3ef48b3d}},
    {"44100 fuzz", "rate 44100\nch 2\nparams 1000 750 0 500\nreset\nproc 100000\n", 0x4bbabca6e36536d1ull, 200000, {0xbe72b580, 0xbe71f7e8, 0xbbc2262a, 0xbbbc3969, 0xbc2111c1, 0xbc901c55, 0xbd3cb4ab, 0xbd39be4a}},
    {"44100 gristle", "rate 44100\nch 2\nparams 750 250 1 500\nreset\nproc 100000\n", 0xa143396f4a196be9ull, 200000, {0xbeb068e1, 0xbeb05741, 0xbe03203b, 0xbe03176b, 0x3d372566, 0x3c090bb3, 0xbe1bbc6f, 0xbe1b992f}},
    {"44100 spaced", "rate 44100\nch 2\nparams 50 0 0 1000\nreset\nproc 100000\n", 0x5ead3ec62e9871d2ull, 200000, {0xbeab7a00, 0xbeab6600, 0xbd2cfa00, 0xbd2cd200, 0xbc43c000, 0xbbca8000, 0xbdeea000, 0xbdee5000}},
    {"44100 soft", "rate 44100\nch 2\nparams 333 777 0 1000\nreset\nproc 100000\n", 0x19d7ae32c87edd7aull, 200000, {0x3c3fb944, 0x3c410273, 0x3e4c4c51, 0x3e4c7cfb, 0xbda284af, 0xbe3334f6, 0x3ea62caa, 0x3ea68dfe}},
    {"44100 hard", "rate 44100\nch 2\nparams 999 999 1 300\nreset\nproc 100000\n", 0x206be90dd3cb6850ull, 200000, {0xbe22429e, 0xbe222602, 0xbd2abd8a, 0xbd2aa0ee, 0x3d84f1ac, 0x3c64783f, 0xbd1d7c3b, 0xbd1d09e9}},
    {"44100 update mid", "rate 44100\nch 2\nparams 300 600 0 800\nreset\nproc 20000\nparams 700 200 1 600\nupdate\nproc 20000\nparams 50 900 0 1000\nupdate\nproc 50000\nreset\nproc 20000\n", 0x1df0026237ea114dull, 220000, {0x3c194c77, 0x3c1ad8e0, 0xbe6ec39d, 0xbe6e9443, 0x3e421b26, 0xbdb84bb9, 0x3f279704, 0x3f279c1b}},
    {"48000 default", "rate 48000\nch 2\nparams 0 500 0 500\nreset\nproc 100000\n", 0x0a7ea6c16d2624f5ull, 200000, {0xbed16257, 0xbed15307, 0xbd2ea36b, 0xbd2e3599, 0x3e8553d3, 0x3d66bfc7, 0xbcda9be4, 0xbcd72d56}},
    {"48000 mangle", "rate 48000\nch 2\nparams 0 1000 1 1000\nreset\nproc 100000\n", 0xbcf253c41e729112ull, 200000, {0xbf029994, 0xbf029914, 0xbeffc1ef, 0xbeffc1af, 0x3f00e4e1, 0x3effe2a3, 0x3ef872aa, 0x3ef48b3d}},
    {"48000 fuzz", "rate 48000\nch 2\nparams 1000 750 0 500\nreset\nproc 100000\n", 0x4bbabca6e36536d1ull, 200000, {0xbe72b580, 0xbe71f7e8, 0xbbc2262a, 0xbbbc3969, 0xbc2111c1, 0xbc901c55, 0xbd3cb4ab, 0xbd39be4a}},
    {"48000 gristle", "rate 48000\nch 2\nparams 750 250 1 500\nreset\nproc 100000\n", 0xa143396f4a196be9ull, 200000, {0xbeb068e1, 0xbeb05741, 0xbe03203b, 0xbe03176b, 0x3d372566, 0x3c090bb3, 0xbe1bbc6f, 0xbe1b992f}},
    {"48000 spaced", "rate 48000\nch 2\nparams 50 0 0 1000\nreset\nproc 100000\n", 0x5ead3ec62e9871d2ull, 200000, {0xbeab7a00, 0xbeab6600, 0xbd2cfa00, 0xbd2cd200, 0xbc43c000, 0xbbca8000, 0xbdeea000, 0xbdee5000}},
    {"48000 soft", "rate 48000\nch 2\nparams 333 777 0 1000\nreset\nproc 100000\n", 0x19d7ae32c87edd7aull, 200000, {0x3c3fb944, 0x3c410273, 0x3e4c4c51, 0x3e4c7cfb, 0xbda284af, 0xbe3334f6, 0x3ea62caa, 0x3ea68dfe}},
    {"48000 hard", "rate 48000\nch 2\nparams 999 999 1 300\nreset\nproc 100000\n", 0x206be90dd3cb6850ull, 200000, {0xbe22429e, 0xbe222602, 0xbd2abd8a, 0xbd2aa0ee, 0x3d84f1ac, 0x3c64783f, 0xbd1d7c3b, 0xbd1d09e9}},
    {"48000 update mid", "rate 48000\nch 2\nparams 300 600 0 800\nreset\nproc 20000\nparams 700 200 1 600\nupdate\nproc 20000\nparams 50 900 0 1000\nupdate\nproc 50000\nreset\nproc 20000\n", 0x1df0026237ea114dull, 220000, {0x3c194c77, 0x3c1ad8e0, 0xbe6ec39d, 0xbe6e9443, 0x3e421b26, 0xbdb84bb9, 0x3f279704, 0x3f279c1b}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpdistortion", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    // the saved form: u32 length, the plug-in's GUID, the settings
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpdistortion");
    if (!info || (int)info->params.size() != 4 || (int)info->presets.size() != 6) { std::printf("FAIL info\n"); ++bad; }
    else {
        const std::string st = reno::vegpack1::encode(*info, info->presets[0].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0x84, 0xfb, 0xb5, 0xc7, 0x31, 0x10, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        const bool ok = st.compare(0, 2, "S:") == 0 && raw.size() == 36 && len == 36 && std::memcmp(raw.data() + 4, guid, 16) == 0 &&
                        reno::vegpack1::decode(*info, st, &back) && back == info->presets[0].v && back == std::vector<int32_t>({0, 500, 0, 500});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
