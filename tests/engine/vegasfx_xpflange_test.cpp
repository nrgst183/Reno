// ExpressFX Flange/Wah-Wah (CXpFlange) against what the DLL itself produces (sfxpfx1_x64.dll): the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit. Also the saved form.
//
//   vegasfx_xpflange_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 0 -800 0 1000 50 500 75 500\nreset\nproc 100000\n", 0x8853131d17a8768cull, 200000, {0xbe1f3a86, 0xbe2170f8, 0xbb2154c5, 0xbb0bb4c2, 0x3c80564b, 0x3cac2d19, 0x3d20f16e, 0x3d2b91e1}},
    {"44100 slow flange", "rate 44100\nch 2\nparams 0 -800 0 200 80 500 75 500\nreset\nproc 100000\n", 0x31389b04ff4c764cull, 200000, {0xbe58544c, 0xbe5db6d7, 0xbc7598db, 0xbc7b9911, 0xb98676e1, 0x3b89d7fa, 0x3c8980db, 0x3c94836b}},
    {"44100 mad flange", "rate 44100\nch 2\nparams 0 -800 0 20000 100 500 75 1000\nreset\nproc 100000\n", 0xa306ef438693e197ull, 200000, {0xbd7dfc23, 0xbd8585a1, 0xbd171962, 0xbd1f39fd, 0x39b19802, 0xbd8f49e6, 0xbd071e5f, 0xbd0d8f7c}},
    {"44100 tremolo flange", "rate 44100\nch 2\nparams 0 -800 0 15000 25 500 75 500\nreset\nproc 100000\n", 0xdc5f0db70e1591d5ull, 200000, {0xbe35c813, 0xbe393f87, 0xbcb6caf0, 0xbcbd1fd5, 0x3d3c60b4, 0x3ce8ed0c, 0xbcc80749, 0xbccfc5df}},
    {"44100 bouncing wah", "rate 44100\nch 2\nparams 2 -800 0 1000 50 500 75 500\nreset\nproc 100000\n", 0xf5f284d0d0f044b0ull, 200000, {0xbddcf0ec, 0xbddcc986, 0xbbb42248, 0xbbb38208, 0x3d82e135, 0x3c39dd55, 0x3be32d80, 0x3be5a910}},
    {"44100 slow wah", "rate 44100\nch 2\nparams 2 -800 0 200 80 500 75 500\nreset\nproc 100000\n", 0xfc20aa505479ac3aull, 200000, {0xbe08b276, 0xbe08a1db, 0xbb4330ca, 0xbb41f1e4, 0x3d7e6045, 0x3ca3cdc8, 0xbdfc4496, 0xbdfc1d56}},
    {"44100 mad wah", "rate 44100\nch 2\nparams 2 -800 0 20000 100 500 75 1000\nreset\nproc 100000\n", 0x34cd06c6b2cefac3ull, 200000, {0x3d50db6a, 0x3d50dc3f, 0x3c886443, 0x3c8863b4, 0x3e64bdd7, 0x3cb8c353, 0xbe474545, 0xbe474223}},
    {"44100 tremolo wah", "rate 44100\nch 2\nparams 2 -800 0 15000 25 500 75 500\nreset\nproc 100000\n", 0x025ecb72f45b3203ull, 200000, {0xbdea7404, 0xbdea4c24, 0xbbc3de73, 0xbbc33d2d, 0x3d730163, 0x3c4a4765, 0x3bb1e4dd, 0x3bb45fde}},
    {"44100 other wah", "rate 44100\nch 2\nparams 1 -800 0 3000 50 500 75 800\nreset\nproc 100000\n", 0x46c8852bd8000e4bull, 200000, {0xbe6ddbdb, 0xbe6db419, 0xbc80adec, 0xbc805e4f, 0x3dafe5b3, 0x3ca3d27d, 0xbc850dca, 0xbc83cc16}},
    {"44100 odd flange", "rate 44100\nch 2\nparams 0 -800 0 777 99 500 75 300\nreset\nproc 100000\n", 0x9973857c4f99e321ull, 200000, {0xbe1cf364, 0xbe1c73e2, 0xbba9a08a, 0xbb9e31a7, 0x3d84ab2f, 0x3c93cf08, 0x3c6fdd7c, 0x3c82c020}},
    {"44100 odd wah", "rate 44100\nch 2\nparams 2 -800 0 12345 70 500 60 600\nreset\nproc 100000\n", 0x3233c14d829e3b3bull, 200000, {0xbda7f923, 0xbda7dafe, 0xbacae3fd, 0xbac8cfde, 0x3d2c6d39, 0x3d3c184b, 0x38be8848, 0x390850c3}},
    {"44100 low resonance", "rate 44100\nch 2\nparams 2 -800 0 333 30 500 40 1000\nreset\nproc 100000\n", 0x64b43552c5e681eaull, 200000, {0x3c09af3c, 0x3c09a6eb, 0x3d0dfaed, 0x3d0dfb62, 0x3e40147c, 0x3d777955, 0x3e3aa6a0, 0x3e3aa584}},
    {"44100 update mid", "rate 44100\nch 2\nparams 0 -800 0 2000 70 500 75 800\nreset\nproc 20000\nparams 2 -800 0 700 60 500 75 600\nupdate\nproc 20000\nparams 0 -800 0 9000 100 500 75 1000\nupdate\nproc 20000\nparams 2 -800 0 3000 20 500 30 1000\nupdate\nproc 30000\nreset\nproc 20000\n", 0x549d8d09d5aff4caull, 220000, {0x3daa18dd, 0x3db923be, 0xbd82d9a5, 0xbd89aafd, 0x3d3fa358, 0xbdecf109, 0xbe77150b, 0xbe771514}},
    {"48000 default", "rate 48000\nch 2\nparams 0 -800 0 1000 50 500 75 500\nreset\nproc 100000\n", 0x20af37fc0a063462ull, 200000, {0xbd8207d9, 0xbd77f997, 0xbcd4d590, 0xbcdcd5f0, 0x3dc726cb, 0x3cd9a44b, 0xbd294262, 0xbd30fa27}},
    {"48000 slow flange", "rate 48000\nch 2\nparams 0 -800 0 200 80 500 75 500\nreset\nproc 100000\n", 0x6b831e4be442c478ull, 200000, {0xbdbe5e76, 0xbdbbae29, 0x3c0f90f3, 0x3c1f3059, 0x3ceb6559, 0x3c8f97eb, 0x3dc19d76, 0x3dcd3524}},
    {"48000 mad flange", "rate 48000\nch 2\nparams 0 -800 0 20000 100 500 75 1000\nreset\nproc 100000\n", 0x2880c575b16be08cull, 200000, {0x3e962324, 0x3e9e9bf4, 0x3b3f97b4, 0x3b4e825a, 0xbc1f26d0, 0xbe5c9170, 0xbe067fad, 0xbe0db5eb}},
    {"48000 tremolo flange", "rate 48000\nch 2\nparams 0 -800 0 15000 25 500 75 500\nreset\nproc 100000\n", 0x16a2a396c89be748ull, 200000, {0xbde27d79, 0xbde1ce59, 0x3c35facc, 0x3c47bc9c, 0x3e1fa1d7, 0x3a9258dc, 0xbd2eb654, 0xbd36bba9}},
    {"48000 bouncing wah", "rate 48000\nch 2\nparams 2 -800 0 1000 50 500 75 500\nreset\nproc 100000\n", 0x3bfcdf00b919a478ull, 200000, {0xbde7e225, 0xbde7b9b2, 0xbb8ea187, 0xbb8e0225, 0x3d82a0e2, 0x3c3b27e6, 0xba9debd4, 0xba93f7fb}},
    {"48000 slow wah", "rate 48000\nch 2\nparams 2 -800 0 200 80 500 75 500\nreset\nproc 100000\n", 0x6684c4b1198277acull, 200000, {0xbe0731cb, 0xbe0720f4, 0xbb2c9035, 0xbb2b50e6, 0x3d7136df, 0x3c8c718a, 0xbe3a7682, 0xbe3a62b6}},
    {"48000 mad wah", "rate 48000\nch 2\nparams 2 -800 0 20000 100 500 75 1000\nreset\nproc 100000\n", 0x7ca35e996e3da310ull, 200000, {0x3e2ca4fe, 0x3e2ca642, 0xbcfb6393, 0xbcfb635e, 0x3df31bee, 0xbd7b573c, 0x3e1604d3, 0x3e16054b}},
    {"48000 tremolo wah", "rate 48000\nch 2\nparams 2 -800 0 15000 25 500 75 500\nreset\nproc 100000\n", 0x3e2765af5ea47c09ull, 200000, {0xbddffd9d, 0xbddfd5d4, 0xbb4df86f, 0xbb4cba4c, 0x3d642ead, 0x3c384e47, 0x3ba23722, 0x3ba4afdc}},
    {"48000 other wah", "rate 48000\nch 2\nparams 1 -800 0 3000 50 500 75 800\nreset\nproc 100000\n", 0x75a3ae3fa6864f68ull, 200000, {0xbe6ef84f, 0xbe6ecfea, 0xbc7f0793, 0xbc7e683d, 0x3daac8a5, 0x3c9b5ad3, 0xbc8cf51e, 0xbc8bb2b9}},
    {"48000 odd flange", "rate 48000\nch 2\nparams 0 -800 0 777 99 500 75 300\nreset\nproc 100000\n", 0x587caff9a7c11f90ull, 200000, {0xbe060bce, 0xbe04468f, 0xbc861cc5, 0xbc88594f, 0x3db42602, 0x3c9c8e9e, 0xbcf67e53, 0xbd00032a}},
    {"48000 odd wah", "rate 48000\nch 2\nparams 2 -800 0 12345 70 500 60 600\nreset\nproc 100000\n", 0x7e8736f05ec47576ull, 200000, {0xbdaa160d, 0xbda9f79f, 0x3c66e2e2, 0x3c6724b4, 0x3e40597e, 0x3d336839, 0x3d8c2cee, 0x3d8c4d05}},
    {"48000 low resonance", "rate 48000\nch 2\nparams 2 -800 0 333 30 500 40 1000\nreset\nproc 100000\n", 0x7a3440a9cafd9059ull, 200000, {0x3c199a77, 0x3c199309, 0x3d18ded9, 0x3d18df26, 0x3e288f28, 0x3d573356, 0x3e3b00df, 0x3e3affc2}},
    {"48000 update mid", "rate 48000\nch 2\nparams 0 -800 0 2000 70 500 75 800\nreset\nproc 20000\nparams 2 -800 0 700 60 500 75 600\nupdate\nproc 20000\nparams 0 -800 0 9000 100 500 75 1000\nupdate\nproc 20000\nparams 2 -800 0 3000 20 500 30 1000\nupdate\nproc 30000\nreset\nproc 20000\n", 0xab23f59b44660f3bull, 220000, {0xbe181f85, 0xbe1dc70a, 0x3cf01727, 0x3cff3e3f, 0x3d305aab, 0x3db7926a, 0xbe8e1e2c, 0xbe8e1e30}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpflange", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    // the saved form: u32 length, the plug-in's GUID, the settings
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpflange");
    if (!info || (int)info->params.size() != 4 || (int)info->presets.size() != 10) { std::printf("FAIL info\n"); ++bad; }
    else {
        const std::string st = reno::vegpack1::encode(*info, info->presets[0].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0x28, 0xfc, 0x97, 0xba, 0x62, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        const bool ok = st.compare(0, 2, "S:") == 0 && raw.size() == 52 && len == 52 && std::memcmp(raw.data() + 4, guid, 16) == 0 &&
                        reno::vegpack1::decode(*info, st, &back) && back == info->presets[0].v && back == std::vector<int32_t>({0, -800, 0, 1000, 50, 500, 75, 500});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
