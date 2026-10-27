// ExpressFX Graphic EQ (sfxpfx3_x64.dll, CXpGeq) against what the DLL itself produces: the outputs below, made by running the DLL's own
// streaming code (set-format, the filter design and the block convolver) on the test signal of vegasfx_script.h, must come out of Reno's
// version bit for bit, behind the 2048 frames of silence the effect's latency puts first (and after each reset). Also the saved form,
// presets and ranges.
//
//   vegasfx_xpgeq_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 1 0 0 0 0 0 0 0 0 0 0 0\nreset\nproc 100000\n", 0x433684b654d8318dull, 200000, {0x00000000, 0x00000000, 0x3d4d000a, 0x3d4d280e, 0x3db6dfec, 0x3db05402, 0x3e8155fb, 0x3e8169fe}},
    {"44100 bass", "rate 44100\nch 2\nparams 2 0 15 60 15 0 0 0 0 0 0 0\nreset\nproc 100000\n", 0x304883f04a979b80ull, 200000, {0x00000000, 0x00000000, 0x3d389bfa, 0x3d38c3fe, 0x3dc8c1c4, 0x3dbcce38, 0x3e9503f5, 0x3e9517fa}},
    {"44100 treble", "rate 44100\nch 2\nparams 2 0 0 0 0 0 0 30 60 60 60 0\nreset\nproc 100000\n", 0xaa71f96e91a1fde8ull, 200000, {0x00000000, 0x00000000, 0x3dc79fe9, 0x3dc7b442, 0x3e37df30, 0x3e345791, 0x3ee25ad4, 0x3ee26ec8}},
    {"44100 mid boost", "rate 44100\nch 2\nparams 1 0 0 0 0 40 60 40 0 0 0 0\nreset\nproc 100000\n", 0x864321c391053c86ull, 200000, {0x00000000, 0x00000000, 0x3d615740, 0x3d617eef, 0x3da97833, 0x3da737fc, 0x3e87ba27, 0x3e87ce5e}},
    {"44100 cut high", "rate 44100\nch 2\nparams 1 0 0 0 0 0 0 0 -60 -120 -120 0\nreset\nproc 100000\n", 0xd9781b37e38be434ull, 200000, {0x00000000, 0x00000000, 0x3d0596c4, 0x3d05bde2, 0x3cd1639e, 0x3cb694a2, 0x3e3b9cae, 0x3e3bc4b4}},
    {"44100 cut low", "rate 44100\nch 2\nparams 2 -600 -600 -100 0 0 0 0 0 0 0 0\nreset\nproc 100000\n", 0x86e3e55244e2044full, 200000, {0x00000000, 0x00000000, 0x3d554884, 0x3d5548fd, 0x3da601ce, 0x3da04d5b, 0x3e6a416e, 0x3e6a41dc}},
    {"44100 mid cut", "rate 44100\nch 2\nparams 1 0 0 0 0 -40 -60 -40 0 0 0 0\nreset\nproc 100000\n", 0xaaffcfabfb4c1770ull, 200000, {0x00000000, 0x00000000, 0x3d40f692, 0x3d411ed9, 0x3dbed56a, 0x3db5efb0, 0x3e7c0999, 0x3e7c3162}},
    {"44100 mixed", "rate 44100\nch 2\nparams 0 -480 240 -200 120 -480 60 -30 200 -100 240 -35\nreset\nproc 100000\n", 0xa0fde86bc921c94aull, 200000, {0x00000000, 0x00000000, 0xbe1101b4, 0xbe10fc23, 0xbf044ac1, 0xbf09ea71, 0xbf16204f, 0xbf161fed}},
    {"44100 outgain", "rate 44100\nch 2\nparams 1 0 0 0 0 0 0 0 0 0 0 150\nreset\nproc 100000\n", 0x9ff6262eca9c1212ull, 200000, {0x00000000, 0x00000000, 0x3e90199e, 0x3e9035bf, 0x3f008c29, 0x3ef7e43b, 0x3fb5d3c7, 0x3fb5efe9}},
    {"44100 zigzag", "rate 44100\nch 2\nparams 2 240 -480 240 -480 240 -480 240 -480 240 -480 -600\nreset\nproc 100000\n", 0xef7b7774d64ece94ull, 200000, {0x00000000, 0x00000000, 0x396171a3, 0x39621018, 0x3a29edd7, 0x3a45aa02, 0x3a8f37df, 0x3a8f8802}},
    {"44100 update mid", "rate 44100\nch 2\nparams 1 0 0 0 0 0 0 0 0 0 0 0\nreset\nproc 15000\nparams 2 0 15 60 15 0 0 0 0 0 0 0\nupdate\nproc 30000\nparams 1 -480 240 -200 120 -480 60 -30 200 -100 240 -35\nupdate\nproc 45000\nreset\nproc 20000\n", 0x338db4dcb22c9ee0ull, 220000, {0x00000000, 0x00000000, 0x3d4d000a, 0x3d4d280e, 0x3db6dfec, 0x3dbcce38, 0xbe4ab9e6, 0xbe4abafe}},
    {"48000 default", "rate 48000\nch 2\nparams 1 0 0 0 0 0 0 0 0 0 0 0\nreset\nproc 100000\n", 0x433684b654d8318dull, 200000, {0x00000000, 0x00000000, 0x3d4d000a, 0x3d4d280e, 0x3db6dfec, 0x3db05402, 0x3e8155fb, 0x3e8169fe}},
    {"48000 bass", "rate 48000\nch 2\nparams 2 0 15 60 15 0 0 0 0 0 0 0\nreset\nproc 100000\n", 0xf70381d48fec6de1ull, 200000, {0x00000000, 0x00000000, 0x3d416e74, 0x3d4196a2, 0x3dc667f4, 0x3dbe1ec9, 0x3e90813c, 0x3e909540}},
    {"48000 treble", "rate 48000\nch 2\nparams 2 0 0 0 0 0 0 30 60 60 60 0\nreset\nproc 100000\n", 0x113e590f4f43eb36ull, 200000, {0x00000000, 0x00000000, 0x3dc92790, 0x3dc93bea, 0x3e36f57b, 0x3e33b2b4, 0x3ee4ee2e, 0x3ee5021a}},
    {"48000 mid boost", "rate 48000\nch 2\nparams 1 0 0 0 0 40 60 40 0 0 0 0\nreset\nproc 100000\n", 0x81d59b52dfc403abull, 200000, {0x00000000, 0x00000000, 0x3d5cd770, 0x3d5cff38, 0x3daa3c02, 0x3da7a938, 0x3e869aa1, 0x3e86aed6}},
    {"48000 cut high", "rate 48000\nch 2\nparams 1 0 0 0 0 0 0 0 -60 -120 -120 0\nreset\nproc 100000\n", 0x79c3c079bc5077efull, 200000, {0x00000000, 0x00000000, 0x3cdce8c8, 0x3cdd3708, 0x3caf80ae, 0x3c94ddea, 0x3e23468e, 0x3e236e9a}},
    {"48000 cut low", "rate 48000\nch 2\nparams 2 -600 -600 -100 0 0 0 0 0 0 0 0\nreset\nproc 100000\n", 0x820de39a4e608987ull, 200000, {0x00000000, 0x00000000, 0x3d4fcc4e, 0x3d4fccc4, 0x3da836b6, 0x3da037d0, 0x3e70bcc3, 0x3e70bd38}},
    {"48000 mid cut", "rate 48000\nch 2\nparams 1 0 0 0 0 -40 -60 -40 0 0 0 0\nreset\nproc 100000\n", 0x23e3bec514806c67ull, 200000, {0x00000000, 0x00000000, 0x3d43a9f2, 0x3d43d224, 0x3dbe9834, 0x3db57a44, 0x3e7dd694, 0x3e7dfe60}},
    {"48000 mixed", "rate 48000\nch 2\nparams 0 -480 240 -200 120 -480 60 -30 200 -100 240 -35\nreset\nproc 100000\n", 0x11ca336ba496148eull, 200000, {0x00000000, 0x00000000, 0xbe9d9dfb, 0xbe9d9b11, 0xbefd147b, 0xbf026475, 0xbf95e338, 0xbf95e349}},
    {"48000 outgain", "rate 48000\nch 2\nparams 1 0 0 0 0 0 0 0 0 0 0 150\nreset\nproc 100000\n", 0x9ff6262eca9c1212ull, 200000, {0x00000000, 0x00000000, 0x3e90199e, 0x3e9035bf, 0x3f008c29, 0x3ef7e43b, 0x3fb5d3c7, 0x3fb5efe9}},
    {"48000 zigzag", "rate 48000\nch 2\nparams 2 240 -480 240 -480 240 -480 240 -480 240 -480 -600\nreset\nproc 100000\n", 0x940b57c70a717007ull, 200000, {0x00000000, 0x00000000, 0x39775d80, 0x3977fa37, 0x3a24559d, 0x3a4c2f8b, 0x3a44c80c, 0x3a4568e8}},
    {"48000 update mid", "rate 48000\nch 2\nparams 1 0 0 0 0 0 0 0 0 0 0 0\nreset\nproc 15000\nparams 2 0 15 60 15 0 0 0 0 0 0 0\nupdate\nproc 30000\nparams 1 -480 240 -200 120 -480 60 -30 200 -100 240 -35\nupdate\nproc 45000\nreset\nproc 20000\n", 0x91fd33598f11060eull, 220000, {0x00000000, 0x00000000, 0x3d4d000a, 0x3d4d280e, 0x3db6dfec, 0x3dbe1ec9, 0xbe11514f, 0xbe1151b8}},
};

int main()
{
    int bad = vegfx::checkCases("reno.xpgeq", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    const reno::vegpack1::Info* info = reno::vegpack1::findInfo("reno.xpgeq");
    if (!info || (int)info->params.size() != 11 || (int)info->presets.size() != 8) { std::printf("FAIL info\n"); ++bad; }
    else {
        // the saved form: u32 length (68), the plug-in's GUID, the twelve settings
        const std::string st = reno::vegpack1::encode(*info, info->presets[2].v.data());
        std::vector<int32_t> back;
        const std::string raw = reno::base64Decode(st.substr(2));
        uint32_t len = 0;
        std::memcpy(&len, raw.data(), 4);
        const uint8_t guid[16] = {0x96, 0xc7, 0xd7, 0xe2, 0x04, 0xd1, 0x28, 0x4b, 0x9f, 0xb3, 0x29, 0x65, 0xd5, 0x03, 0xd1, 0x10};
        const bool ok = st.compare(0, 2, "S:") == 0 && raw.size() == 68 && len == 68 && std::memcmp(raw.data() + 4, guid, 16) == 0 &&
                        reno::vegpack1::decode(*info, st, &back) && back == info->presets[2].v &&
                        back == std::vector<int32_t>({2, 0, 15, 60, 15, 0, 0, 0, 0, 0, 0, 0});
        std::printf("%s saved form\n", ok ? "PASS" : "FAIL");
        if (!ok) ++bad;
        const bool map = reno::vegpack1::uidForDx("dshow:{00000009-0F56-11D2-9887-00A0C969725B}") == "reno.xpgeq" &&
                         reno::vegpack1::dxForUid("reno.xpgeq") == "dshow:{00000009-0F56-11D2-9887-00A0C969725B}";
        std::printf("%s DirectX map\n", map ? "PASS" : "FAIL");
        if (!map) ++bad;
        std::unique_ptr<reno::AudioPlugin> p = reno::vegpack1::create("reno.xpgeq");
        p->activate(44100, 512);
        const bool lat = p->latencySamples() == 2048 && p->paramCount() == 11 && p->paramText(0) == "0.0 dB" && p->paramText(10) == "0.0 dB";
        std::printf("%s latency and texts\n", lat ? "PASS" : "FAIL");
        if (!lat) ++bad;
    }
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
