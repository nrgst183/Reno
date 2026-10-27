// ExpressFX Equalization (sfxpfx2's CXpEq) against what the DLL itself produces: the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit.
//
//   vegasfx_xpeq_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 flat", "rate 44100\nch 2\nparams 1000 0 0 0 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x9cbe8717336629e5ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"44100 bass only", "rate 44100\nch 2\nparams 1000 -250 0 -250 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0xfe8ae50b5710b9d1ull, 200000, {0x3bac5cdd, 0x3bb14ed7, 0xbabbf90c, 0xbab709c4, 0xbc53bf2a, 0xbbce2727, 0xbb664863, 0xbb5c66c6}},
    {"44100 bass boost", "rate 44100\nch 2\nparams 1000 0 120 0 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x28092eb8f46decd4ull, 200000, {0xbe3d5183, 0xbe3cb39d, 0xbd2616f4, 0xbd2578f3, 0x3dc2a786, 0x3c1508fa, 0x3c93f2c5, 0x3c98e27a}},
    {"44100 contour", "rate 44100\nch 2\nparams 1000 -250 60 60 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0xd6e689822dea5380ull, 200000, {0xbed8b597, 0xbed88d8e, 0xbdaea866, 0xbdae8067, 0x3de932b4, 0x3caf206f, 0xbdb12eef, 0xbdb08d6c}},
    {"44100 treble only", "rate 44100\nch 2\nparams 1000 -250 -250 60 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x9dc856f6ac9af44dull, 200000, {0xbeeca744, 0xbeeca699, 0xbdb3bcde, 0xbdb3bc48, 0x3dd63482, 0x3cd62a9e, 0xbdf74335, 0xbdf73ebb}},
    {"44100 treble boost", "rate 44100\nch 2\nparams 1000 0 0 120 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x18dac6cf75e5dc4eull, 200000, {0xbf6ec04d, 0xbf6eb5e0, 0xbe176de1, 0xbe17635e, 0x3e93eb6c, 0x3d8d6149, 0xbe23372d, 0xbe230c60}},
    {"44100 real project", "rate 44100\nch 2\nparams 1000 0 -45 0 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x75290cc992c772d3ull, 200000, {0xbe6c0882, 0xbe6bf080, 0xbc91724e, 0xbc91422e, 0x3dc97855, 0x3cb9e4eb, 0xbc27d4ff, 0xbc2654c0}},
    {"44100 half mix", "rate 44100\nch 2\nparams 500 100 100 100 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x35dbde86f1aaf75aull, 200000, {0xbf05ebe0, 0xbf05d71c, 0xbd2c3c70, 0xbd2be8a1, 0x3e8277ba, 0x3d69be90, 0x3c7f49cc, 0x3c823be3}},
    {"44100 all boost", "rate 44100\nch 2\nparams 1000 250 250 250 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0xc6ee02fcaeeb844full, 200000, {0xc106b742, 0xc106ac83, 0x3e4ce9a9, 0x3e4da70b, 0x41148a14, 0x40094c1a, 0x40b0cf17, 0x40b0e3e0}},
    {"44100 all cut", "rate 44100\nch 2\nparams 1000 -250 -250 -250 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x3a6fe7e6863e1276ull, 200000, {0x3be56833, 0x3be568c7, 0x3bec04ea, 0x3bec03da, 0xbb063d0b, 0xba4f9521, 0x3afaa25f, 0x3afa96a0}},
    {"44100 mixed", "rate 44100\nch 2\nparams 1000 30 -30 70 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x26e9bb3263ffa328ull, 200000, {0xbf081c7f, 0xbf08153b, 0xbd8394d8, 0xbd838621, 0x3e5a9650, 0x3d51539d, 0xbd3076b3, 0xbd300162}},
    {"44100 small gains", "rate 44100\nch 2\nparams 1000 5 -5 5 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0xd2b47d5de448f90bull, 200000, {0xbe78cc88, 0xbe78a6b7, 0xbc8b758e, 0xbc8b29e5, 0x3dd01bba, 0x3cb76a7f, 0xbc2cde28, 0xbc2a8116}},
    {"44100 narrow band", "rate 44100\nch 2\nparams 1000 60 60 60 -250 0 5 100 1 1 100 200\nreset\nproc 100000\n", 0x6b0d92dbc29e0e57ull, 200000, {0xbedf908f, 0xbedf6870, 0xbd2dad41, 0xbd2d5d5e, 0x3e51c0f7, 0x3d39fec7, 0xbc072d3a, 0xbc022ea2}},
    {"44100 wide band high", "rate 44100\nch 2\nparams 1000 -100 60 -60 -250 0 100 8000 1 1 2000 10000\nreset\nproc 100000\n", 0x15d9350153801b16ull, 200000, {0xbda057c3, 0xbd9fb9b2, 0x3d1c5556, 0x3d1ca47f, 0x3ea50337, 0x3d97f534, 0x3e213203, 0x3e217e71}},
    {"44100 updates and a jump", "rate 44100\nch 2\nparams 1000 0 0 0 -250 0 25 400 1 1 300 500\nreset\nproc 20000\nparams 1000 -250 60 60 -250 0 25 400 1 1 300 500\nupdate\nproc 20000\nparams 1000 0 120 0 -250 0 25 400 1 1 300 500\nupdate\nproc 20000\nparams 1000 0 0 120 -250 0 25 400 1 1 300 500\nupdate\nproc 20000\nparams 1000 0 0 120 -250 0 25 400 1 1 300 500\nreset\nproc 20000\n", 0x22ceba263b324041ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3de932b4, 0x3c1508fa, 0xbe23372d, 0xbe230c60}},
    {"48000 flat", "rate 48000\nch 2\nparams 1000 0 0 0 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x9cbe8717336629e5ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"48000 bass only", "rate 48000\nch 2\nparams 1000 -250 0 -250 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x0faf0c38af6bc581ull, 200000, {0x3bb65d41, 0x3bbb4f21, 0xbb11c602, 0xbb0f4e45, 0xbc47635c, 0xbbc788c5, 0xbb3af885, 0xbb3115a0}},
    {"48000 bass boost", "rate 48000\nch 2\nparams 1000 0 120 0 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x44acc6ced729c2c1ull, 200000, {0xbe31d1c6, 0xbe3133dc, 0xbd28f86e, 0xbd285a70, 0x3dd96557, 0x3c31a966, 0x3cf29238, 0x3cf781f4}},
    {"48000 contour", "rate 48000\nch 2\nparams 1000 -250 60 60 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x22e883a1067fbc91ull, 200000, {0xbed7c1fb, 0xbed799f4, 0xbdad2c71, 0xbdad046d, 0x3e01db29, 0x3cc8d98b, 0xbd9a8997, 0xbd99e839}},
    {"48000 treble only", "rate 48000\nch 2\nparams 1000 -250 -250 60 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x0012d940545f2a99ull, 200000, {0xbeec819e, 0xbeec80ff, 0xbdb288fa, 0xbdb2885a, 0x3ded2de8, 0x3ced2d88, 0xbde12a80, 0xbde12644}},
    {"48000 treble boost", "rate 48000\nch 2\nparams 1000 0 0 120 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x4a78acc306aaacf4ull, 200000, {0xbf6e21d1, 0xbf6e1766, 0xbe137c7a, 0xbe1371f7, 0x3e99ebb0, 0x3d936aba, 0xbe16c62f, 0xbe169b90}},
    {"48000 real project", "rate 48000\nch 2\nparams 1000 0 -45 0 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0xe69ea476eff8b6ceull, 200000, {0xbe6be618, 0xbe6bce17, 0xbc8c742b, 0xbc8c440f, 0x3dc960f0, 0x3cb9cd85, 0xbc255a14, 0xbc23d9ee}},
    {"48000 half mix", "rate 48000\nch 2\nparams 500 100 100 100 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0xdc43bf48f328d25eull, 200000, {0xbf0334bf, 0xbf031ffa, 0xbd23a13d, 0xbd234d85, 0x3e80974d, 0x3d622470, 0x3c8f21e3, 0x3c91b886}},
    {"48000 all boost", "rate 48000\nch 2\nparams 1000 250 250 250 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x8a93e707d5342a40ull, 200000, {0xc0ec2474, 0xc0ec0ee4, 0x3e919cdc, 0x3e91faa2, 0x410e4073, 0x3ffad7c9, 0x40b39d82, 0x40b3b22d}},
    {"48000 all cut", "rate 48000\nch 2\nparams 1000 -250 -250 -250 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x750135d0e9eed407ull, 200000, {0x3bba70f5, 0x3bba71e4, 0x3bec4680, 0x3bec4576, 0xba7eb3fe, 0xba172319, 0x3ae8bff3, 0x3ae8b6f4}},
    {"48000 mixed", "rate 48000\nch 2\nparams 1000 30 -30 70 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x1ee9eeef74d98368ull, 200000, {0xbf078563, 0xbf077e20, 0xbd7bd66b, 0xbd7bb903, 0x3e5c3cd9, 0x3d530010, 0xbd246c3a, 0xbd23f738}},
    {"48000 small gains", "rate 48000\nch 2\nparams 1000 5 -5 5 -250 0 25 400 1 1 300 500\nreset\nproc 100000\n", 0x0ce211a4b67b72aaull, 200000, {0xbe78b9fa, 0xbe78942a, 0xbc8a19c4, 0xbc89ce1c, 0x3dcffd74, 0x3cb74bc7, 0xbc2c8f62, 0xbc2a3258}},
    {"48000 narrow band", "rate 48000\nch 2\nparams 1000 60 60 60 -250 0 5 100 1 1 100 200\nreset\nproc 100000\n", 0xfb82b1ad3d2f9759ull, 200000, {0xbee06b97, 0xbee043a1, 0xbd252fe0, 0xbd24e001, 0x3e51a419, 0x3d3b6c70, 0xbc1034ea, 0xbc0b366d}},
    {"48000 wide band high", "rate 48000\nch 2\nparams 1000 -100 60 -60 -250 0 100 8000 1 1 2000 10000\nreset\nproc 100000\n", 0xd895f1e5b8b30e02ull, 200000, {0xbda217d9, 0xbda179ec, 0x3d250ddb, 0x3d255d05, 0x3ea63990, 0x3d992b91, 0x3e347e4c, 0x3e34cae0}},
    {"48000 updates and a jump", "rate 48000\nch 2\nparams 1000 0 0 0 -250 0 25 400 1 1 300 500\nreset\nproc 20000\nparams 1000 -250 60 60 -250 0 25 400 1 1 300 500\nupdate\nproc 20000\nparams 1000 0 120 0 -250 0 25 400 1 1 300 500\nupdate\nproc 20000\nparams 1000 0 0 120 -250 0 25 400 1 1 300 500\nupdate\nproc 20000\nparams 1000 0 0 120 -250 0 25 400 1 1 300 500\nreset\nproc 20000\n", 0x60f51014baea88d6ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3e01db29, 0x3c31a966, 0xbe16c62f, 0xbe169b90}},
};

int main()
{
    const int bad = vegfx::checkCases("reno.xpeq", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
