// ExpressFX Delay (sfxpfx2's CXpDelay) against what the DLL itself produces: the outputs below, made by running the DLL's own
// code on the test signal of vegasfx_script.h, must come out of Reno's version bit for bit.
//
//   vegasfx_xpdelay_test
#include "vegasfx_script.h"

static const vegfx::Case kCases[] = {
    {"44100 default", "rate 44100\nch 2\nparams 500 2500 0\nreset\nproc 100000\n", 0x55712407d1e6801eull, 200000, {0xbde99800, 0xbde97000, 0xbbfe7000, 0xbbfdd000, 0x3d58e000, 0x3c248800, 0xbd537000, 0xbd52d800}},
    {"44100 half second", "rate 44100\nch 2\nparams 500 5000 0\nreset\nproc 100000\n", 0x07d52f07a62cdd18ull, 200000, {0xbde99800, 0xbde97000, 0xbbfe7000, 0xbbfdd000, 0xbd370800, 0xbc6b6000, 0xbe18d600, 0xbe18b000}},
    {"44100 phased", "rate 44100\nch 2\nparams 500 30 0\nreset\nproc 100000\n", 0x9f6d8d68ed19325eull, 200000, {0xbea1ef00, 0xbea1db00, 0xbd19e400, 0xbd19bc00, 0x3bdb4000, 0xbac78000, 0xbdc87400, 0xbdc82400}},
    {"44100 robot", "rate 44100\nch 2\nparams 1000 100 900\nreset\nproc 100000\n", 0x29312d3604a6eba6ull, 200000, {0x3d119000, 0x3d123000, 0xbc968c2c, 0xbc910f14, 0x3da3eddd, 0x3d460842, 0xbe813098, 0xbe80986e}},
    {"44100 echo chamber", "rate 44100\nch 2\nparams 350 2500 500\nreset\nproc 100000\n", 0x74d40377e3eed6dcull, 200000, {0xbe17d600, 0xbe17bc00, 0xbc256266, 0xbc24fa66, 0x3d58339a, 0xbbaee600, 0xbda2cb28, 0xbda26c67}},
    {"44100 grand canyon", "rate 44100\nch 2\nparams 250 10000 250\nreset\nproc 110000\n", 0x90962b146289a915ull, 220000, {0xbe2f3200, 0xbe2f1400, 0xbc3ed400, 0xbc3e5c00, 0x3d91b000, 0x3df2b280, 0x3e8638a8, 0x3e864cf8}},
    {"44100 dreamscape", "rate 44100\nch 2\nparams 400 1000 700\nreset\nproc 100000\n", 0xfc0cf59f96639abcull, 200000, {0xbe0c2800, 0xbe0c1000, 0xbdfd0585, 0xbdfcc31f, 0x3e128592, 0xbd964d27, 0xbd8004cf, 0xbd7f4186}},
    {"44100 shortest", "rate 44100\nch 2\nparams 1000 1 300\nreset\nproc 100000\n", 0x44a998ec96332139ull, 200000, {0x3ea9c30a, 0x3ea9df9c, 0x3df51694, 0x3df53051, 0xbe9836a2, 0xbda18e10, 0xbee495f8, 0xbee47966}},
    {"44100 update grow and shrink", "rate 44100\nch 2\nparams 500 1000 500\nreset\nproc 20000\nparams 500 3000 500\nupdate\nproc 20000\nparams 500 9000 500\nupdate\nproc 20000\nparams 500 50 500\nupdate\nproc 20000\nparams 900 2000 100\nupdate\nproc 20000\n", 0x264cb4793277de34ull, 200000, {0xbde99800, 0xbde97000, 0xbe2f1c80, 0xbe2ef980, 0x3d424000, 0x3c281800, 0x3de78a6c, 0x3de7db39}},
    {"44100 reset keeps size", "rate 44100\nch 2\nparams 500 1000 500\nreset\nproc 20000\nparams 500 1000 500\nreset\nproc 20000\nparams 500 2000 500\nreset\nproc 20000\n", 0x7cfff22a252d867dull, 120000, {0xbde99800, 0xbde97000, 0xbe2f1c80, 0xbe2ef980, 0x3c5c5800, 0x3c281800, 0x3e4b7040, 0x3e4b8bc0}},
    {"48000 default", "rate 48000\nch 2\nparams 500 2500 0\nreset\nproc 100000\n", 0x53b6dda14ce3cb03ull, 200000, {0xbde99800, 0xbde97000, 0xbbfe7000, 0xbbfdd000, 0xbde5b000, 0xbcffd800, 0xbe5dec00, 0xbe5dc400}},
    {"48000 half second", "rate 48000\nch 2\nparams 500 5000 0\nreset\nproc 100000\n", 0x92a7a031ac4afa28ull, 200000, {0xbde99800, 0xbde97000, 0xbbfe7000, 0xbbfdd000, 0x3e001e00, 0x3ce61400, 0x3ca85000, 0x3ca99000}},
    {"48000 phased", "rate 48000\nch 2\nparams 500 30 0\nreset\nproc 100000\n", 0xd1b1fe11f22c6c9cull, 200000, {0xbe171400, 0xbe16ee00, 0x3b973000, 0x3b987000, 0x3e33a200, 0x3d268e00, 0x3d911c00, 0x3d916c00}},
    {"48000 robot", "rate 48000\nch 2\nparams 1000 100 900\nreset\nproc 100000\n", 0x9be8e73b196b6542ull, 200000, {0x3ec3c000, 0x3ec3d400, 0x3dcba331, 0x3dcd0969, 0xbd64814f, 0x3c31c718, 0xbdb91251, 0xbdb6bb36}},
    {"48000 echo chamber", "rate 48000\nch 2\nparams 350 2500 500\nreset\nproc 100000\n", 0x7bfd330b328731ccull, 200000, {0xbe17d600, 0xbe17bc00, 0xbc256266, 0xbc24fa66, 0xbb5bcccd, 0xbc048733, 0xbe1087d6, 0xbe10521e}},
    {"48000 grand canyon", "rate 48000\nch 2\nparams 250 10000 250\nreset\nproc 110000\n", 0xa6575dad073d4455ull, 220000, {0xbe2f3200, 0xbe2f1400, 0xbc3ed400, 0xbc3e5c00, 0x3d91b000, 0x3c7c2400, 0x3eeac420, 0x3eead960}},
    {"48000 dreamscape", "rate 48000\nch 2\nparams 400 1000 700\nreset\nproc 100000\n", 0x9295bd442ebf1851ull, 200000, {0xbe0c2800, 0xbe0c1000, 0x3d3b0971, 0x3d3b835c, 0x3e56a627, 0x3e20db2b, 0x3df46786, 0x3df4c737}},
    {"48000 shortest", "rate 48000\nch 2\nparams 1000 1 300\nreset\nproc 100000\n", 0xa41f91dd6371b20dull, 200000, {0x3eeebe0d, 0x3eeeda9e, 0xbda60eeb, 0xbda5f259, 0xbdccf344, 0xbcf25934, 0xbe7f71a2, 0xbe7f3c7e}},
    {"48000 update grow and shrink", "rate 48000\nch 2\nparams 500 1000 500\nreset\nproc 20000\nparams 500 3000 500\nupdate\nproc 20000\nparams 500 9000 500\nupdate\nproc 20000\nparams 500 50 500\nupdate\nproc 20000\nparams 900 2000 100\nupdate\nproc 20000\n", 0x290a180654ee0a33ull, 200000, {0xbde99800, 0xbde97000, 0x3d402e00, 0x3d40ae00, 0x3d424000, 0x3cfaf000, 0x3de78a6c, 0x3de7db39}},
    {"48000 reset keeps size", "rate 48000\nch 2\nparams 500 1000 500\nreset\nproc 20000\nparams 500 1000 500\nreset\nproc 20000\nparams 500 2000 500\nreset\nproc 20000\n", 0xa774de7694f84f90ull, 120000, {0xbde99800, 0xbde97000, 0x3d402e00, 0x3d40ae00, 0x3d935500, 0x3c281800, 0x3e9998c0, 0x3e99aa40}},
};

int main()
{
    const int bad = vegfx::checkCases("reno.xpdelay", kCases, (int)(sizeof kCases / sizeof kCases[0]));
    std::printf("%s\n", bad ? "FAILED" : "ALL PASSED");
    return bad ? 1 : 0;
}
