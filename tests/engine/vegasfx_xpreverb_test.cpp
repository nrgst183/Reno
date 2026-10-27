// ExpressFX Reverb (CXpReverb) against what the DLL itself produces (sfxpfx1_x64.dll): the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit. Also the saved form.
//
//   vegasfx_xpreverb_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 def", "rate 44100\nch 2\nparams -800 -240 -160 0 2 25 30 1 20 1 12000 500\nreset\nproc 100000\n", 0x9c1650c1540f0670ull, 200000, {0xbe815a32, 0xbe62c854, 0xbd37374c, 0xbb0bdead, 0x3d42a47e, 0xbd3f2f5c, 0xbdab46bb, 0x3d936ec9}},
    {"44100 dream", "rate 44100\nch 2\nparams -800 -240 -160 8 2 100 0 1 20 1 12000 1000\nreset\nproc 100000\n", 0x55dc803b3e09f9b9ull, 200000, {0xbe6a2131, 0xbe4a3720, 0xbdb25018, 0xbdf9de05, 0xbdbd0ce4, 0xbe74e7e1, 0x3e818946, 0x3f0fcbe0}},
    {"44100 pool", "rate 44100\nch 2\nparams -800 -240 -160 4 2 100 160 1 20 1 12000 750\nreset\nproc 100000\n", 0xffa2bec359107204ull, 200000, {0xbe7cdd65, 0xbe5cf254, 0xbd4fda96, 0xbd88ad7b, 0x3e08d612, 0xbcfd1635, 0xbe330dea, 0x3e1a9321}},
    {"44100 hall", "rate 44100\nch 2\nparams -800 -240 -160 5 2 65 70 1 20 1 12000 400\nreset\nproc 100000\n", 0x589ec6004a8632a2ull, 200000, {0xbe828532, 0xbe651ded, 0xbb8a368e, 0x3d2a0ef5, 0x3d7a7b69, 0x3d710e14, 0xbd9a19b8, 0x3dd5782c}},
    {"44100 metal", "rate 44100\nch 2\nparams -800 -240 -160 9 2 25 30 1 20 1 12000 1000\nreset\nproc 100000\n", 0xddfaa4bf944691edull, 200000, {0xbe770665, 0xbe571c54, 0xbe9c0da8, 0xbe6935f5, 0xbc2e05f5, 0x3c5d58ac, 0xbd71d408, 0x3d9d20de}},
    {"44100 club", "rate 44100\nch 2\nparams -800 -240 -160 7 2 15 60 1 20 1 12000 500\nreset\nproc 100000\n", 0x90a2b0b317fd762eull, 200000, {0xbe815a32, 0xbe62c854, 0xbcb1a3c9, 0xba1aceaa, 0x3d1b0cd8, 0xbb13253b, 0xbd9b1fb7, 0x3da13555}},
    {"44100 room1 pat1", "rate 44100\nch 2\nparams -800 -240 -300 1 3 60 35 1 150 1 3300 200\nreset\nproc 60000\n", 0x8366b42eeca12e7eull, 120000, {0xbe64ec00, 0xbe64c4cd, 0xbd001beb, 0xbd1698e2, 0x3db7299a, 0xbb8feb0f, 0x3d83a056, 0x3d6e195a}},
    {"44100 room4 pat4", "rate 44100\nch 2\nparams -800 -240 -300 4 2 60 80 1 450 1 7200 500\nreset\nproc 60000\n", 0xb3b361cb8793a6bfull, 120000, {0xbe654137, 0xbe5ec442, 0xbd96cd24, 0xbd95bab9, 0x3d84bd53, 0xbc063b2b, 0x3c4c5505, 0x3d5f874a}},
    {"44100 room9 pat9", "rate 44100\nch 2\nparams -800 -240 -300 9 7 60 155 1 950 1 13700 1000\nreset\nproc 60000\n", 0x530344e28c827bc2ull, 120000, {0xbe48ee59, 0xbe4efe1e, 0x3c6ec813, 0x3c83304f, 0x3e5781b0, 0x3e50a3a0, 0x3e7d9c1d, 0x3e82c1f0}},
    {"44100 rnd1", "rate 44100\nch 2\nparams 0 -575 0 2 6 88 24 0 4799 1 3688 982\nreset\nproc 90000\n", 0xa857865495805651ull, 180000, {0xbde12dae, 0x3da75daa, 0xbe0d4130, 0xbe33b2e8, 0xbeea1072, 0xbdccedea, 0xbe9ee204, 0xbcf1b2ed}},
    {"44100 rnd3", "rate 44100\nch 2\nparams -800 -270 -10 0 1 35 42 1 4315 0 11516 338\nreset\nproc 90000\n", 0xa388e5d28b1bc5e4ull, 180000, {0xbe2d41f4, 0x3d8f3b94, 0xbdb5c358, 0xbdefe650, 0xbe8e4614, 0xbd99e9f9, 0xbdc1a64a, 0xbe17f386}},
    {"44100 rnd5", "rate 44100\nch 2\nparams -800 -759 -10 9 3 23 69 0 3245 1 14325 736\nreset\nproc 90000\n", 0xe77be19da4b44536ull, 180000, {0xbe5866b8, 0xbe5841aa, 0xbe6987bf, 0xbda338dc, 0x3c97ac5e, 0x380e4982, 0xbdab9e7b, 0x3e5cbe1c}},
    {"44100 update quick", "rate 44100\nch 2\nparams -800 -240 -160 0 2 25 30 1 20 1 12000 500\nreset\nproc 20000\nparams -800 -240 -160 0 2 80 30 1 100 1 8000 800\nupdate\nproc 20000\nparams -800 -240 -100 0 2 10 30 0 100 0 8000 200\nupdate\nproc 20000\nparams -800 -240 -100 0 2 10 30 1 300 1 3000 1000\nupdate\nproc 30000\n", 0x6f656e57b9b69e2aull, 180000, {0xbe815a32, 0xbe62c854, 0xbd37374c, 0xbb0bdead, 0x3dc75d8e, 0xbcd92398, 0xbed7ba47, 0xbed1a72e}},
    {"44100 update rebuild", "rate 44100\nch 2\nparams -800 -240 -160 4 2 100 160 1 20 1 12000 750\nreset\nproc 30000\nparams -800 -240 -160 2 2 100 160 1 20 1 12000 750\nupdate\nproc 20000\nparams -800 -240 -160 2 6 100 80 1 20 1 12000 750\nupdate\nproc 20000\nparams -800 -240 -160 2 6 100 90 1 20 1 12000 750\nupdate\nproc 20000\nreset\nproc 20000\n", 0xa8a1a0bf4e71f29dull, 220000, {0xbe7cdd65, 0xbe5cf254, 0xbd4fda96, 0xbd88ad7b, 0x3e08d612, 0xbc61dba6, 0x3f095ed3, 0x3f27f237}},
    {"48000 def", "rate 48000\nch 2\nparams -800 -240 -160 0 2 25 30 1 20 1 12000 500\nreset\nproc 100000\n", 0x95402159b1ba94d7ull, 200000, {0xbe5dea00, 0xbe5dc400, 0xbd7e8fc1, 0xbd8000da, 0x3e11d988, 0x3cfcd953, 0xbda6c9e5, 0xbd01b5d9}},
    {"48000 dream", "rate 48000\nch 2\nparams -800 -240 -160 8 2 100 0 1 20 1 12000 1000\nreset\nproc 100000\n", 0xcc5d2f39d9d22074ull, 200000, {0xbe7f4b6d, 0xbe7f276d, 0x3db3d697, 0xbc79efb2, 0x3d0b288d, 0x3c045e4a, 0xbcd5c0aa, 0xbcc197ba}},
    {"48000 pool", "rate 48000\nch 2\nparams -800 -240 -160 4 2 100 160 1 20 1 12000 750\nreset\nproc 100000\n", 0x3ec915ad3c94337bull, 200000, {0xbe581300, 0xbe57ee00, 0x3cabbe01, 0x3cb721ab, 0x3e33108c, 0xb9ad052b, 0x3e06043a, 0x3deec247}},
    {"48000 hall", "rate 48000\nch 2\nparams -800 -240 -160 5 2 65 70 1 20 1 12000 400\nreset\nproc 100000\n", 0xc6eb14b23ab6af2eull, 200000, {0xbe604000, 0xbe60199a, 0xbdbbcbdb, 0xbda470a4, 0x3e36a6ff, 0x3cabcc75, 0xbd776450, 0xbb2c25c6}},
    {"48000 metal", "rate 48000\nch 2\nparams -800 -240 -160 9 2 25 30 1 20 1 12000 1000\nreset\nproc 100000\n", 0x8acf9177cf42c943ull, 200000, {0xbe523c00, 0xbe521800, 0xbdcb0e10, 0xbd913388, 0x3dcb7d9f, 0xbdf8d829, 0xbe2a5b4b, 0xbe0d3216}},
    {"48000 club", "rate 48000\nch 2\nparams -800 -240 -160 7 2 15 60 1 20 1 12000 500\nreset\nproc 100000\n", 0xf9ed7a9e77983248ull, 200000, {0xbe5dea00, 0xbe5dc400, 0x3d72ff38, 0x3cd0cc2a, 0x3dc4a446, 0x3d552535, 0xbd31e76d, 0xbc810172}},
    {"48000 room1 pat1", "rate 48000\nch 2\nparams -800 -240 -300 1 3 60 35 1 150 1 3300 200\nreset\nproc 60000\n", 0x36c74888890ccc7bull, 120000, {0xbe64ec00, 0xbe64c4cd, 0x3b3e35c6, 0xbb328167, 0x3db307fc, 0x3c627cdf, 0x3d75b580, 0x3d4fb36a}},
    {"48000 room4 pat4", "rate 48000\nch 2\nparams -800 -240 -300 4 2 60 80 1 450 1 7200 500\nreset\nproc 60000\n", 0x75918c649ab5a9ffull, 120000, {0xbe5dea00, 0xbe5dc400, 0x3d59a12c, 0x3c8defde, 0x3dc55d22, 0x3d0e5592, 0x3db857b5, 0x3d04aa05}},
    {"48000 room9 pat9", "rate 48000\nch 2\nparams -800 -240 -300 9 7 60 155 1 950 1 13700 1000\nreset\nproc 60000\n", 0x231f7c6c67e4400dull, 120000, {0xbe54b859, 0xbe52ec1e, 0x3d79efba, 0x3e0c546f, 0xbddb60d0, 0xbe80547f, 0x3e2933a0, 0x3e1c5075}},
    {"48000 rnd1", "rate 48000\nch 2\nparams -50 -129 -10 0 7 18 42 0 4662 1 12813 562\nreset\nproc 90000\n", 0xdb4daf921d6a956dull, 180000, {0xbe9142f6, 0xbe73abb7, 0xbbfb7517, 0x3b3414f4, 0x3e294c8b, 0x3dcc781f, 0xbea9ce48, 0xbf037f16}},
    {"48000 rnd3", "rate 48000\nch 2\nparams 0 -496 0 1 2 5 174 1 1273 1 2579 452\nreset\nproc 90000\n", 0x9ecfc81b991ae042ull, 180000, {0xbe5f090a, 0xbe5ee2d9, 0xbddaf8bb, 0xbd89c816, 0x3e4f60ab, 0x3cb97cbb, 0xbe9af0d4, 0xbf5690ee}},
    {"48000 rnd5", "rate 48000\nch 2\nparams 0 -676 -800 7 3 24 98 0 4416 1 4378 458\nreset\nproc 90000\n", 0xa7881a40a79f17e0ull, 180000, {0xbe5ee529, 0xbe5ebefe, 0xba9c9f46, 0x3c1ace4f, 0x3da728c5, 0xbcaff750, 0xbed5e935, 0xbed7c9d0}},
    {"48000 update quick", "rate 48000\nch 2\nparams -800 -240 -160 0 2 25 30 1 20 1 12000 500\nreset\nproc 20000\nparams -800 -240 -160 0 2 80 30 1 100 1 8000 800\nupdate\nproc 20000\nparams -800 -240 -100 0 2 10 30 0 100 0 8000 200\nupdate\nproc 20000\nparams -800 -240 -100 0 2 10 30 1 300 1 3000 1000\nupdate\nproc 30000\n", 0x09afc4497694feeaull, 180000, {0xbe5dea00, 0xbe5dc400, 0xbd7e8fc1, 0xbd8000da, 0x3e2a8fe3, 0x3ce28fc4, 0xbec545f3, 0xbf10a1b8}},
    {"48000 update rebuild", "rate 48000\nch 2\nparams -800 -240 -160 4 2 100 160 1 20 1 12000 750\nreset\nproc 30000\nparams -800 -240 -160 2 2 100 160 1 20 1 12000 750\nupdate\nproc 20000\nparams -800 -240 -160 2 6 100 80 1 20 1 12000 750\nupdate\nproc 20000\nparams -800 -240 -160 2 6 100 90 1 20 1 12000 750\nupdate\nproc 20000\nreset\nproc 20000\n", 0xb211001fafa5fcacull, 220000, {0xbe581300, 0xbe57ee00, 0x3cabbe01, 0x3cb721ab, 0x3e33108c, 0xbd97f8b9, 0x3e92ba30, 0x3c9f8450}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpreverb", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    // the saved form: u32 length, the plug-in's GUID, the settings
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpreverb");
    if (!info || (int)info->params.size() != 4 || (int)info->presets.size() != 7) { std::printf("FAIL info\n"); ++bad; }
    else {
        const std::string st = reno::vegpack1::encode(*info, info->presets[0].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0x30, 0xfc, 0x97, 0xba, 0x62, 0x0f, 0xd2, 0x11, 0x98, 0x87, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x5b};
        const bool ok = st.compare(0, 2, "S:") == 0 && raw.size() == 68 && len == 68 && std::memcmp(raw.data() + 4, guid, 16) == 0 &&
                        reno::vegpack1::decode(*info, st, &back) && back == info->presets[0].v && back == std::vector<int32_t>({-800, -240, -160, 0, 2, 25, 30, 1, 20, 1, 12000, 500});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
