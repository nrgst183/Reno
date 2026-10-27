// ExpressFX Stutter (CXpStuttr) against what the DLL itself produces (sfxpfx1_x64.dll): the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit. Also the saved form.
//
//   vegasfx_xpstutter_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 100 100 0 500\nreset\nproc 100000\n", 0x6daffb9070608d46ull, 200000, {0xbde99800, 0xbde97000, 0xbbfe7000, 0xbbfdd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"44100 crystal ball", "rate 44100\nch 2\nparams 2000 3500 1 1000\nreset\nproc 100000\n", 0x5b587a28d4e1415full, 200000, {0x00000000, 0xbe697000, 0x00000000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0x00000000, 0xbc1cc000}},
    {"44100 gravel", "rate 44100\nch 2\nparams 3000 2000 0 750\nreset\nproc 100000\n", 0xef01af24b224bf41ull, 200000, {0xbe699800, 0xbe697000, 0xbb7e7000, 0xbb7dd000, 0x3dc24000, 0x3ba81800, 0xbc1de555, 0xbc1b6ac7}},
    {"44100 low bell", "rate 44100\nch 2\nparams 100 600 1 650\nreset\nproc 100000\n", 0x39d14700d82ebf71ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3d07f99a, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"44100 walkie-talkie", "rate 44100\nch 2\nparams 100 100 1 500\nreset\nproc 100000\n", 0x617ede1bb769ec4eull, 200000, {0xbe699800, 0xbde97000, 0xbc7e7000, 0xbbfdd000, 0x3dc24000, 0x3ca81800, 0xbb9f4000, 0xbc1cc000}},
    {"44100 long", "rate 44100\nch 2\nparams 20000 20000 1 1000\nreset\nproc 100000\n", 0x91ce2f574f6484f2ull, 200000, {0x00000000, 0xbdd3bc15, 0xbc7e7000, 0x00000000, 0x3dc24000, 0x00000000, 0xbc1f4000, 0x00000000}},
    {"44100 odd", "rate 44100\nch 2\nparams 777 13333 1 300\nreset\nproc 100000\n", 0x9f226400fa336163ull, 200000, {0xbe327c15, 0xbe530beb, 0xbc7e7000, 0xbc7dd000, 0x3d9089b1, 0x3ca81800, 0xbbea63d7, 0xbc094cda}},
    {"44100 short", "rate 44100\nch 2\nparams 13 77 1 1000\nreset\nproc 100000\n", 0x71051f48cd1da6c9ull, 200000, {0xbe699800, 0xbe697000, 0x00000000, 0x80000000, 0x00000000, 0x00000000, 0xbb95e1e2, 0x00000000}},
    {"44100 update mid", "rate 44100\nch 2\nparams 2000 1500 1 800\nreset\nproc 20000\nparams 700 3000 0 600\nupdate\nproc 20000\nparams 1500 2500 1 1000\nupdate\nproc 50000\nreset\nproc 20000\n", 0x2b36c88ad4516e35ull, 220000, {0xbd3ae000, 0xbe697000, 0xbb4b8ccd, 0xbb4b0ccd, 0x3d964a5d, 0x3c9114f2, 0x00000000, 0x3edb4c00}},
    {"48000 default", "rate 48000\nch 2\nparams 100 100 0 500\nreset\nproc 100000\n", 0x2b5f74cf6f202fb4ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3c281800, 0xbb9f4000, 0xbb9cc000}},
    {"48000 crystal ball", "rate 48000\nch 2\nparams 2000 3500 1 1000\nreset\nproc 100000\n", 0x36b6a19fc2db8c21ull, 200000, {0x00000000, 0xbe697000, 0xbc7e7000, 0x00000000, 0x3dc24000, 0x3c3e819a, 0xbc1f4000, 0x00000000}},
    {"48000 gravel", "rate 48000\nch 2\nparams 3000 2000 0 750\nreset\nproc 100000\n", 0xf1b6172295f9682bull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3cc24000, 0x3ba81800, 0xbb1f4000, 0xbb1cc000}},
    {"48000 low bell", "rate 48000\nch 2\nparams 100 600 1 650\nreset\nproc 100000\n", 0x779fee5cebf4fef1ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc701077, 0x3dc24000, 0x3beb54cd, 0xbc1f4000, 0xbc1cc000}},
    {"48000 walkie-talkie", "rate 48000\nch 2\nparams 100 100 1 500\nreset\nproc 100000\n", 0xa3cfcef00fde99efull, 200000, {0xbde99800, 0xbe697000, 0xbbfe7000, 0xbc7dd000, 0x3dc24000, 0x3c281800, 0xbc1f4000, 0xbb9cc000}},
    {"48000 long", "rate 48000\nch 2\nparams 20000 20000 1 1000\nreset\nproc 100000\n", 0x262e73f3bbb7935full, 200000, {0x00000000, 0xbdc28800, 0xbba9a000, 0x00000000, 0x3dc24000, 0x3ca81800, 0x00000000, 0xbc1cc000}},
    {"48000 odd", "rate 48000\nch 2\nparams 777 13333 1 300\nreset\nproc 100000\n", 0x92baeea777ff788dull, 200000, {0xbe2ea3fa, 0xbe4f2d00, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"48000 short", "rate 48000\nch 2\nparams 13 77 1 1000\nreset\nproc 100000\n", 0xc3f5bef6f88a401dull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"48000 update mid", "rate 48000\nch 2\nparams 2000 1500 1 800\nreset\nproc 20000\nparams 700 3000 0 600\nupdate\nproc 20000\nparams 1500 2500 1 1000\nupdate\nproc 50000\nreset\nproc 20000\n", 0xc1819fbbf44e2d7bull, 220000, {0xbd3ae000, 0xbe697000, 0xbc6a151f, 0xbb4b0ccd, 0x3d1b6666, 0x00000000, 0x3e66e911, 0x3edb4c00}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpstutter", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    // the saved form: u32 length, the plug-in's GUID, the settings
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpstutter");
    if (!info || (int)info->params.size() != 4 || (int)info->presets.size() != 6) { std::printf("FAIL info\n"); ++bad; }
    else {
        const std::string st = reno::vegpack1::encode(*info, info->presets[0].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0x4c, 0xd7, 0x9d, 0xf5, 0xe1, 0x14, 0xd2, 0x11, 0xb3, 0xb2, 0x00, 0xa0, 0xc9, 0x06, 0x42, 0xcc};
        const bool ok = st.compare(0, 2, "S:") == 0 && raw.size() == 36 && len == 36 && std::memcmp(raw.data() + 4, guid, 16) == 0 &&
                        reno::vegpack1::decode(*info, st, &back) && back == info->presets[0].v && back == std::vector<int32_t>({100, 100, 0, 500});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
