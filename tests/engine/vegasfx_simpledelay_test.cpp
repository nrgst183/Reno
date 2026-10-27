// Simple Delay (sfppack1's CSfEchos) against what the DLL itself produces: the outputs below, made by running the DLL's
// own code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit.
//
//   vegasfx_simpledelay_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 0 100 1 -60 -60 0 13 1000 0 18 1000 0\nreset\nproc 60000\n", 0xf6d89a2bd4a5d32bull, 120000, {0xbdea25fe, 0xbde9fde6, 0x3e63fcc2, 0x3e6415d1, 0xb9fd99ca, 0xbe40b75e, 0xb8a06142, 0xb8206142}},
    {"44100 echo chamber", "rate 44100\nch 2\nparams 1 400 20 -30 -90 0 13 1000 0 18 1000 0\nreset\nproc 100000\n", 0x20d5f153f2a6a8c1ull, 200000, {0xbda5c3ae, 0xbda5a74c, 0xbbb48e40, 0xbbb41cb6, 0x3d870b25, 0x3e19cc89, 0x3dec4e65, 0x3deca0e5}},
    {"44100 canyon", "rate 44100\nch 2\nparams 1 1000 30 -140 -40 0 13 1000 0 18 1000 0\nreset\nproc 100000\n", 0x99c7c951d361bff5ull, 200000, {0xbe136340, 0xbe134a03, 0xbc208a0a, 0xbc202516, 0x3d75207e, 0x3d958445, 0x3c35200c, 0x3c36dde0}},
    {"44100 pseudo", "rate 44100\nch 2\nparams 0 10 1 0 -800 0 13 1000 0 18 1000 0\nreset\nproc 100000\n", 0xdfb2289632998beeull, 200000, {0xbd9b8000, 0xbd9b3000, 0x3cb87800, 0x3cb8c000, 0x3e7cf800, 0x3d6fe400, 0x3e11e400, 0x3e120c00}},
    {"44100 tempo measures", "rate 44100\nch 2\ntempo 5 97.5 3 4\nparams 1 100 50 -30 -60 0 13 1500 1 18 2000 1\nreset\nproc 100000\n", 0xe1e2d81807453ea4ull, 200000, {0xbdea25fe, 0xbde9fde6, 0xbbff0aaa, 0xbbfe6a49, 0x3d42b614, 0x3e39a303, 0x3d371cc9, 0x3d37953c}},
    {"44100 tempo 8th triplet", "rate 44100\nch 2\ntempo 5 133.0 4 4\nparams 1 100 50 -30 -60 0 9 1000 1 12 250 1\nreset\nproc 100000\n", 0x59d56c1227dfbaf2ull, 200000, {0xbdea25fe, 0xbde9fde6, 0x3d977d34, 0x3d97ba33, 0x3dc4454d, 0x3e46db56, 0x3ccd7dd2, 0x3cce5710}},
    {"44100 update mid", "rate 44100\nch 2\nparams 1 300 30 -30 -60 0 13 1000 0 18 1000 0\nreset\nproc 20000\nparams 1 300 60 -20 -30 0 13 1000 0 18 1000 0\nupdate\nproc 20000\nparams 1 500 60 -20 -30 0 13 1000 0 18 1000 0\nupdate\nproc 50000\nreset\nproc 20000\n", 0xaf9d77bdd5f1a860ull, 220000, {0xbdea25fe, 0xbde9fde6, 0xbbff0aaa, 0xbbfe6a49, 0xb98a1ccd, 0x3c6e00a5, 0x3e9b31eb, 0x3e9b4014}},
    {"48000 default", "rate 48000\nch 2\nparams 0 100 1 -60 -60 0 13 1000 0 18 1000 0\nreset\nproc 60000\n", 0xac0fcb43eeb1ceaaull, 120000, {0xbdea25fe, 0xbde9fde6, 0xbd34535b, 0xbd33ef1e, 0x3d6e2a6e, 0x3d3db109, 0x3d6fd571, 0x3d6ffd89}},
    {"48000 echo chamber", "rate 48000\nch 2\nparams 1 400 20 -30 -90 0 13 1000 0 18 1000 0\nreset\nproc 100000\n", 0x954627dfd9abcfcaull, 200000, {0xbda5c3ae, 0xbda5a74c, 0xbbb48e40, 0xbbb41cb6, 0x3d9364d4, 0x3d15e6b6, 0xbd82ffe5, 0xbd82b010}},
    {"48000 canyon", "rate 48000\nch 2\nparams 1 1000 30 -140 -40 0 13 1000 0 18 1000 0\nreset\nproc 100000\n", 0xdcc1a3736d4391adull, 200000, {0xbe136340, 0xbe134a03, 0xbc208a0a, 0xbc202516, 0x3d75207e, 0x3c541eb7, 0xbd2b6d69, 0xbd2ade70}},
    {"48000 pseudo", "rate 48000\nch 2\nparams 0 10 1 0 -800 0 13 1000 0 18 1000 0\nreset\nproc 100000\n", 0x58a5ae1177b3ad8bull, 200000, {0x3e8ab000, 0x3e8ac400, 0x3ddfac00, 0x3ddfc000, 0xbecff600, 0xbdd68000, 0x3efa8000, 0x3efa9400}},
    {"48000 tempo measures", "rate 48000\nch 2\ntempo 5 97.5 3 4\nparams 1 100 50 -30 -60 0 13 1500 1 18 2000 1\nreset\nproc 100000\n", 0x5e085cbf4e4fa3acull, 200000, {0xbdea25fe, 0xbde9fde6, 0xbbff0aaa, 0xbbfe6a49, 0x3d42b614, 0x3eb4aa1d, 0x3d495037, 0x3d49c8aa}},
    {"48000 tempo 8th triplet", "rate 48000\nch 2\ntempo 5 133.0 4 4\nparams 1 100 50 -30 -60 0 9 1000 1 12 250 1\nreset\nproc 100000\n", 0x7d8455b54b03635dull, 200000, {0xbdea25fe, 0xbde9fde6, 0x3e9772a2, 0x3e9781e2, 0xbe7b9697, 0xbe957f7b, 0x3da468a5, 0x3da49ef4}},
    {"48000 update mid", "rate 48000\nch 2\nparams 1 300 30 -30 -60 0 13 1000 0 18 1000 0\nreset\nproc 20000\nparams 1 300 60 -20 -30 0 13 1000 0 18 1000 0\nupdate\nproc 20000\nparams 1 500 60 -20 -30 0 13 1000 0 18 1000 0\nupdate\nproc 50000\nreset\nproc 20000\n", 0x1dc1646f5c5b490dull, 220000, {0xbdea25fe, 0xbde9fde6, 0xbbff0aaa, 0xbbfe6a49, 0x3da3ab54, 0x3c6e00a5, 0x3e9b31eb, 0x3e9b4014}},
};

int main()
{
    const int bad = vegfx::checkCases("reno.simpledelay", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
