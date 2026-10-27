// ExpressFX Chorus (sfxpfx2's CXpChorus) against what the DLL itself produces: the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit.
//
//   vegasfx_xpchorus_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 1 100 50 500\nreset\nproc 100000\n", 0xb4369732aa118a7aull, 200000, {0xbde99800, 0xbde97000, 0x3c705d2c, 0x3c82aeb5, 0x3cb21fdc, 0xbc518aa4, 0xbd12bc43, 0xbd1933a7}},
    {"44100 wacky 1", "rate 44100\nch 2\nparams 1 4000 50 1000\nreset\nproc 100000\n", 0x3519c78c57162479ull, 200000, {0x00000000, 0x00000000, 0x3ce05d4c, 0x3ced133d, 0x3d5cb545, 0x3d80e7f3, 0x3e67f4e8, 0x3e74fa0b}},
    {"44100 wacky 3", "rate 44100\nch 2\nparams 3 5000 10 750\nreset\nproc 100000\n", 0xda17e16df047c30full, 200000, {0xbd699800, 0xbd697000, 0xbda9ef3d, 0xbdbc594b, 0xbcfba50b, 0x3d7f198e, 0xbe69417e, 0xbe6e532f}},
    {"44100 wacky 4", "rate 44100\nch 2\nparams 2 10000 50 1000\nreset\nproc 100000\n", 0x44b6e8a1b76ec97eull, 200000, {0x00000000, 0x00000000, 0x3c1f8482, 0x3c2045be, 0x3ea75aa1, 0xbdd8020a, 0x3e811d9b, 0x3e859136}},
    {"44100 slow heavy chorus", "rate 44100\nch 2\nparams 3 300 40 750\nreset\nproc 100000\n", 0x0e07ee22d93750a8ull, 200000, {0xbd699800, 0xbd697000, 0xbd23132a, 0xbd23665e, 0xbde9cba5, 0xbc65584d, 0x3e6fe3d5, 0x3e8a1f17}},
    {"44100 fast medium chorus", "rate 44100\nch 2\nparams 2 5000 5 500\nreset\nproc 100000\n", 0x7acbd17dd9c17c30ull, 200000, {0xbde99800, 0xbde97000, 0xbd13f4b9, 0xbd18ef39, 0x3e0e8cee, 0x3d29afb9, 0xbdb57766, 0xbdc07847}},
    {"44100 guitar", "rate 44100\nch 2\nparams 3 4000 1 1000\nreset\nproc 100000\n", 0x4dfc45b71f49d1ecull, 200000, {0x00000000, 0x00000000, 0xbd8da50a, 0xbd90a9b5, 0x3ea12b6a, 0x3ca7630c, 0xbeac9e63, 0xbeb4bc5b}},
    {"44100 extreme", "rate 44100\nch 2\nparams 3 20000 100 1000\nreset\nproc 100000\n", 0x3feea17b01784544ull, 200000, {0x00000000, 0x00000000, 0x3d2cb8bb, 0x3d4fe13f, 0x3cd09c65, 0xbd9f454c, 0x3d2f8bed, 0x3d19d14b}},
    {"44100 slowest", "rate 44100\nch 2\nparams 2 1 100 500\nreset\nproc 100000\n", 0xb1eac019b5f59e56ull, 200000, {0xbde99800, 0xbde97000, 0x3cfa2bc7, 0x3d089929, 0xbc774ea0, 0xbc8550a5, 0xbc95ac2b, 0xbc912f88}},
    {"44100 dry", "rate 44100\nch 2\nparams 3 3000 50 0\nreset\nproc 100000\n", 0x9cbe8717336629e5ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"44100 updates and a size change", "rate 44100\nch 2\nparams 1 100 50 500\nreset\nproc 20000\nparams 1 500 80 700\nupdate\nproc 20000\nparams 3 500 80 700\nupdate\nproc 20000\nparams 2 2000 20 300\nupdate\nproc 20000\nparams 2 2000 20 300\nreset\nproc 20000\n", 0x378e42d65716b133ull, 200000, {0xbde99800, 0xbde97000, 0x3c705d2c, 0x3c82aeb5, 0x3d85ffe0, 0x3dceec75, 0x3d923a00, 0x3d9a0226}},
    {"48000 default", "rate 48000\nch 2\nparams 1 100 50 500\nreset\nproc 100000\n", 0x085f6ad7693338a4ull, 200000, {0xbde99800, 0xbde97000, 0xbc82e910, 0xbc865d39, 0x3da07d5a, 0xbc32c9a8, 0xbe10465a, 0xbe17de62}},
    {"48000 wacky 1", "rate 48000\nch 2\nparams 1 4000 50 1000\nreset\nproc 100000\n", 0x14639eb6ce446279ull, 200000, {0x00000000, 0x00000000, 0xbda41421, 0xbdad211f, 0xbd0a11b3, 0xbdb764f0, 0x3e8a6713, 0x3e922891}},
    {"48000 wacky 3", "rate 48000\nch 2\nparams 3 5000 10 750\nreset\nproc 100000\n", 0x3851169f5f5c0552ull, 200000, {0xbd699800, 0xbd697000, 0xbda19c3a, 0xbdaec675, 0xbe3048ef, 0x3debb9a2, 0x3e6223de, 0x3e6f4176}},
    {"48000 wacky 4", "rate 48000\nch 2\nparams 2 10000 50 1000\nreset\nproc 100000\n", 0xc2d5668161dee3c0ull, 200000, {0x00000000, 0x00000000, 0xbc99c747, 0xbc8c352f, 0x3c77a3c1, 0xbd4275fd, 0xbea7afb8, 0xbeb182c5}},
    {"48000 slow heavy chorus", "rate 48000\nch 2\nparams 3 300 40 750\nreset\nproc 100000\n", 0xe26578501c7db701ull, 200000, {0xbd699800, 0xbd697000, 0x3be661cf, 0x3bfa3159, 0xbc7ee4f0, 0x3e431213, 0xbd046b92, 0xbd0a0c8d}},
    {"48000 fast medium chorus", "rate 48000\nch 2\nparams 2 5000 5 500\nreset\nproc 100000\n", 0xb3c77e85b1ed3dacull, 200000, {0xbde99800, 0xbde97000, 0x3c654cc0, 0x3c6ff5db, 0x3c4432a6, 0x3c4148cd, 0xbe1bd847, 0xbe273e6d}},
    {"48000 guitar", "rate 48000\nch 2\nparams 3 4000 1 1000\nreset\nproc 100000\n", 0x474ec3a738a8c991ull, 200000, {0x00000000, 0x00000000, 0x3cec2d37, 0x3d03d0f9, 0xbead4d64, 0x3e368004, 0xbe987348, 0xbea58634}},
    {"48000 extreme", "rate 48000\nch 2\nparams 3 20000 100 1000\nreset\nproc 100000\n", 0xf64e4e79114b2363ull, 200000, {0x00000000, 0x00000000, 0xbda3a6ce, 0xbdb13947, 0x3e20d400, 0x3eac3b7c, 0x3f056f34, 0x3f1186ec}},
    {"48000 slowest", "rate 48000\nch 2\nparams 2 1 100 500\nreset\nproc 100000\n", 0x0020b37743ebdfe6ull, 200000, {0xbde99800, 0xbde97000, 0xba72caee, 0xb9ad396e, 0x3e798755, 0xbcc70923, 0x3e1c843d, 0x3e24e860}},
    {"48000 dry", "rate 48000\nch 2\nparams 3 3000 50 0\nreset\nproc 100000\n", 0x9cbe8717336629e5ull, 200000, {0xbe699800, 0xbe697000, 0xbc7e7000, 0xbc7dd000, 0x3dc24000, 0x3ca81800, 0xbc1f4000, 0xbc1cc000}},
    {"48000 updates and a size change", "rate 48000\nch 2\nparams 1 100 50 500\nreset\nproc 20000\nparams 1 500 80 700\nupdate\nproc 20000\nparams 3 500 80 700\nupdate\nproc 20000\nparams 2 2000 20 300\nupdate\nproc 20000\nparams 2 2000 20 300\nreset\nproc 20000\n", 0x5b961b88b132a6ccull, 200000, {0xbde99800, 0xbde97000, 0xbc82e910, 0xbc865d39, 0x3d51dd99, 0xbd88f4e9, 0x3e31dc4e, 0x3e3c508a}},
};

int main()
{
    const int bad = vegfx::checkCases("reno.xpchorus", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
